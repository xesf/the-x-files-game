#include "action.h"

#include <stdio.h>
#include <string.h>

#include "xf_endian.h"

/* Expression layout: left value, right value (4 bytes each), left kind,
 * right kind, operation. */
static vc_expression read_expr(const u8 *p)
{
    vc_expression e;
    e.left.value = xf_be32(p);
    e.right.value = xf_be32(p + 4);
    e.left.kind = p[8];
    e.right.kind = p[9];
    e.op = p[10];
    return e;
}

/* 0x43B0F0: the engine byte-swaps the same fields per type after reading. */
int vc_read_action(const xf_neo_db *db, u32 id, vc_action *a)
{
    xf_neo_reader r, b;
    const u8 *p;
    size_t n, pre;

    memset(a, 0, sizeof(*a));
    if (vc_class(db, id) != VC_ACTION || xf_neo_object(db, id, &r, NULL, NULL) != 0)
        return -1;
    b = xf_neo_blob(db, &r);
    a->type = xf_neo_u8(&r);
    if (r.error || b.error)
        return -1;
    a->has_condition = (a->type & 0x80) != 0;
    a->type &= 0x7F;
    p = b.p;
    n = p ? (size_t)(b.end - b.p) : 0;
    pre = a->has_condition ? 12 : 0;
    if (n < pre)
        return 0;
    if (pre)
        a->condition = read_expr(p);
    p += pre;
    n -= pre;
    switch (a->type) {
    case VC_ACT_EXPRESSION:
        if ((a->valid = n == 11))
            a->u.statement = read_expr(p);
        break;
    case VC_ACT_MEDIA:
        if ((a->valid = n == 5)) {
            a->u.media.id = xf_be32(p);
            a->u.media.selector = p[4];
        }
        break;
    case VC_ACT_TIMER:
        if ((a->valid = n == 6)) {
            a->u.timer.duration = xf_be32(p);
            a->u.timer.id = p[4];
            a->u.timer.control = p[5];
        }
        break;
    case VC_ACT_ENABLE:
        if ((a->valid = n == 9)) {
            a->u.enable.id = xf_be32(p);
            a->u.enable.value = xf_be32(p + 4);
            a->u.enable.control = p[8];
        }
        break;
    case VC_ACT_SET_VIEW:
        if ((a->valid = n == 16)) {
            a->u.set_view.view = xf_be32(p);
            a->u.set_view.node = xf_be32(p + 4);
            a->u.set_view.location = xf_be32(p + 8);
            a->u.set_view.viewpoint = xf_be32(p + 12);
        }
        break;
    case VC_ACT_INTERFACE:
        if ((a->valid = n == 5)) {
            a->u.iface.id = xf_be32(p);
            a->u.iface.control = p[4];
        }
        break;
    case VC_ACT_FUNCTION:
        /* 15-byte name, argument count, argument values, argument kinds */
        if (n >= 16) {
            int i, argc = p[15];
            if ((a->valid = n == (size_t)(16 + 5 * argc) && argc <= VC_MAX_ARGS)) {
                memcpy(a->u.function.name, p, 15);
                a->u.function.name[15] = '\0';
                a->u.function.argc = argc;
                for (i = 0; i < argc; i++) {
                    a->u.function.args[i].value = xf_be32(p + 16 + 4 * i);
                    a->u.function.args[i].kind = p[16 + 4 * argc + i];
                }
            }
        }
        break;
    case VC_ACT_SOUND:
        if ((a->valid = n == 10)) {
            a->u.sound.id = xf_be32(p);
            a->u.sound.selector = p[4];
            a->u.sound.extra = p[5];
            a->u.sound.a = xf_be16(p + 6);
            a->u.sound.b = xf_be16(p + 8);
        }
        break;
    }
    return 0;
}

static void operand_text(const vc_operand *o, char *out, size_t size)
{
    switch (o->kind) {
    case VC_OPND_VARIABLE: snprintf(out, size, "var%u", o->value); break;
    case VC_OPND_PARAMETER: snprintf(out, size, "param%u", o->value); break;
    case VC_OPND_LITERAL: snprintf(out, size, "%d", (i32)o->value); break;
    case VC_OPND_CONTEXT: snprintf(out, size, "ctx%u", o->value); break;
    default: snprintf(out, size, "?%u:%u", o->kind, o->value); break;
    }
}

static void expr_text(const vc_expression *e, int condition, char *out, size_t size)
{
    static const char *const cond_ops[] = { "==", "!=", ">", "<", ">=", "<=", "and", "or" };
    static const char *const stmt_ops[] = { "=", "++", "--", "+=", "-=", "*=", "/=", "%=" };
    char l[32], r[32];
    operand_text(&e->left, l, sizeof(l));
    operand_text(&e->right, r, sizeof(r));
    snprintf(out, size, "%s %s %s", l, e->op < 8 ? (condition ? cond_ops : stmt_ops)[e->op] : "?op", r);
}

void vc_action_text(const vc_action *a, char *out, size_t size)
{
    char cond[96] = "", body[160], tmp[96];
    int i;

    if (a->has_condition) {
        expr_text(&a->condition, 1, tmp, sizeof(tmp));
        snprintf(cond, sizeof(cond), "if %s: ", tmp);
    }
    if (!a->valid) {
        snprintf(body, sizeof(body), "type %u (unexpected size)", a->type);
    } else {
        switch (a->type) {
        case VC_ACT_EXPRESSION:
            expr_text(&a->u.statement, 0, body, sizeof(body));
            break;
        case VC_ACT_MEDIA:
            snprintf(body, sizeof(body), "media #%u selector %u", a->u.media.id, a->u.media.selector);
            break;
        case VC_ACT_TIMER:
            snprintf(body, sizeof(body), "timer %u: %u control %u", a->u.timer.id, a->u.timer.duration,
                     a->u.timer.control);
            break;
        case VC_ACT_ENABLE:
            snprintf(body, sizeof(body), "enable #%u value %u control %u", a->u.enable.id,
                     a->u.enable.value, a->u.enable.control);
            break;
        case VC_ACT_SET_VIEW:
            snprintf(body, sizeof(body), "go to view #%u (node #%u location #%u viewpoint #%u)",
                     a->u.set_view.view, a->u.set_view.node, a->u.set_view.location,
                     a->u.set_view.viewpoint);
            break;
        case VC_ACT_INTERFACE:
            snprintf(body, sizeof(body), "interface #%u control %u", a->u.iface.id, a->u.iface.control);
            break;
        case VC_ACT_FUNCTION: {
            size_t o = (size_t)snprintf(body, sizeof(body), "%s(", a->u.function.name);
            for (i = 0; i < a->u.function.argc && o < sizeof(body); i++) {
                operand_text(&a->u.function.args[i], tmp, sizeof(tmp));
                o += (size_t)snprintf(body + o, sizeof(body) - o, "%s%s", i ? ", " : "", tmp);
            }
            if (o < sizeof(body))
                snprintf(body + o, sizeof(body) - o, ")");
            break;
        }
        case VC_ACT_SOUND:
            snprintf(body, sizeof(body), "sound #%u selector %u %u %u %u", a->u.sound.id,
                     a->u.sound.selector, a->u.sound.extra, a->u.sound.a, a->u.sound.b);
            break;
        default:
            snprintf(body, sizeof(body), "type %u", a->type);
        }
    }
    snprintf(out, size, "%s%s", cond, body);
}
