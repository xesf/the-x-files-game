/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf and contributors */
#include "interp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xf_log.h"

#define MAX_DEPTH 16
#define MAX_LIST 512

static int cmp_var(const void *a, const void *b)
{
    u32 x = ((const vc_var *)a)->id, y = ((const vc_var *)b)->id;
    return x < y ? -1 : x > y;
}

static int cmp_enable(const void *a, const void *b)
{
    u32 x = ((const vc_enable *)a)->id, y = ((const vc_enable *)b)->id;
    return x < y ? -1 : x > y;
}

static const vc_interp *g_sort_ctx;
static int cmp_target(const void *a, const void *b)
{
    u32 x = g_sort_ctx->enables[*(const u32 *)a].target, y = g_sort_ctx->enables[*(const u32 *)b].target;
    return x < y ? -1 : x > y;
}

int vc_interp_init(vc_interp *in, const xf_neo_db *hdb, const xf_neo_db *gam, const vc_host *host)
{
    u32 i, nv = 0, ne = 0;

    memset(in, 0, sizeof(*in));
    in->hdb = hdb;
    if (host)
        in->host = *host;
    for (i = 0; i < gam->count; i++) {
        nv += gam->entries[i].class_id == 0x53;
        ne += gam->entries[i].class_id == 0x46;
    }
    in->vars = calloc(nv ? nv : 1, sizeof(*in->vars));
    in->enables = calloc(ne ? ne : 1, sizeof(*in->enables));
    in->enable_by_target = calloc(ne ? ne : 1, sizeof(u32));
    if (!in->vars || !in->enables || !in->enable_by_target)
        return -1;
    for (i = 0; i < gam->count; i++) {
        const xf_neo_entry *e = &gam->entries[i];
        xf_neo_reader r;
        if (xf_neo_object(gam, e->id, &r, NULL, NULL) != 0)
            continue;
        if (e->class_id == 0x53) {
            /* VCVariable: name, value, owner, owner class, type flags */
            vc_var *v = &in->vars[in->var_count++];
            xf_neo_reader name = xf_neo_blob(gam, &r);
            size_t i, k = 0;
            /* Names are padded with leading spaces in the GAM. */
            for (i = 0; name.p && name.p + i < name.end && name.p[i] && k + 1 < sizeof(v->name); i++)
                if (name.p[i] != ' ' || k)
                    v->name[k++] = (char)name.p[i];
            v->name[k] = '\0';
            v->id = e->id;
            v->value = (i32)xf_neo_u32(&r);
            xf_neo_u32(&r);
            xf_neo_u8(&r);
            v->type = xf_neo_u8(&r);
        } else if (e->class_id == 0x46) {
            /* VCEnabled: target, state, target class */
            vc_enable *n = &in->enables[in->enable_count++];
            n->id = e->id;
            n->target = xf_neo_u32(&r);
            n->enabled = xf_neo_u8(&r);
        }
    }
    qsort(in->vars, in->var_count, sizeof(*in->vars), cmp_var);
    qsort(in->enables, in->enable_count, sizeof(*in->enables), cmp_enable);
    for (i = 0; i < in->enable_count; i++)
        in->enable_by_target[i] = i;
    g_sort_ctx = in;
    qsort(in->enable_by_target, in->enable_count, sizeof(u32), cmp_target);
    xf_info("state: %u variables, %u enabled states", in->var_count, in->enable_count);
    return 0;
}

void vc_interp_free(vc_interp *in)
{
    free(in->vars);
    free(in->enables);
    free(in->enable_by_target);
    free(in->tlocals);
    memset(in, 0, sizeof(*in));
}

static vc_var *find_var(const vc_interp *in, u32 id)
{
    u32 lo = 0, hi = in->var_count;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (in->vars[mid].id == id)
            return &in->vars[mid];
        if (in->vars[mid].id < id)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

static vc_enable *find_enable(const vc_interp *in, u32 id)
{
    u32 lo = 0, hi = in->enable_count;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (in->enables[mid].id == id)
            return &in->enables[mid];
        if (in->enables[mid].id < id)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

i32 vc_get_var(const vc_interp *in, u32 id)
{
    vc_var *v = find_var(in, id);
    return v ? v->value : 0;
}

void vc_set_var(vc_interp *in, u32 id, i32 value)
{
    vc_var *v = find_var(in, id);
    if (v)
        v->value = value;
    else
        xf_warn("interp: no variable %u", id);
}

u32 vc_find_var(const vc_interp *in, const char *name)
{
    u32 i;
    for (i = 0; i < in->var_count; i++)
        if (strcmp(in->vars[i].name, name) == 0)
            return in->vars[i].id;
    return 0;
}

void vc_set_enabled(vc_interp *in, u32 hdb_object, int on)
{
    u32 i;
    for (i = 0; i < in->enable_count; i++)
        if (in->enables[i].target == hdb_object)
            in->enables[i].enabled = on != 0;
}

int vc_is_enabled(const vc_interp *in, u32 hdb_object)
{
    u32 lo = 0, hi = in->enable_count;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        const vc_enable *e = &in->enables[in->enable_by_target[mid]];
        if (e->target == hdb_object)
            return e->enabled;
        if (e->target < hdb_object)
            lo = mid + 1;
        else
            hi = mid;
    }
    return 1;
}

/* Locals of one running action list: 8 integers (selectors 0-7), 12
 * booleans (10000-10011) and 4 bytes (20000-20003). */
typedef struct {
    i32 ints[8];
    i32 bools[12];
    i32 bytes[4];
} locals;

static i32 *local_slot(locals *l, u32 sel)
{
    if (!l)
        return NULL;
    if (sel < 8)
        return &l->ints[sel];
    if (sel >= 10000 && sel < 10012)
        return &l->bools[sel - 10000];
    if (sel >= 20000 && sel < 20004)
        return &l->bytes[sel - 20000];
    return NULL;
}

static i32 operand(const vc_interp *in, locals *l, const vc_operand *o)
{
    i32 *slot;
    switch (o->kind) {
    case VC_OPND_VARIABLE: return vc_get_var(in, o->value);
    case VC_OPND_PARAMETER:
        slot = local_slot(l, o->value);
        return slot ? *slot : 0;
    case VC_OPND_LITERAL:
    case VC_OPND_CHAR:
        return (i32)o->value;
    case VC_OPND_CONTEXT:
        return in->context;
    default:
        return 0;
    }
}

/* 0x43DB70 VCAction_test_condition */
static int test_condition(const vc_interp *in, locals *loc, const vc_expression *e)
{
    i32 l = operand(in, loc, &e->left), r = operand(in, loc, &e->right);
    switch (e->op) {
    case 0: return l == r;
    case 1: return l != r;
    case 2: return l > r;
    case 3: return l < r;
    case 4: return l >= r;
    case 5: return l <= r;
    case 6: return l && r;
    case 7: return l || r;
    }
    return 0;
}

/* 0x4756D0 act_expression: ops 1 and 2 do not read the right operand. */
static void run_statement(vc_interp *in, locals *loc, const vc_expression *e)
{
    i32 l, r = 0, *slot = NULL;
    if (e->left.kind == VC_OPND_PARAMETER) {
        slot = local_slot(loc, e->left.value);
        if (!slot)
            return;
    } else if (e->left.kind != VC_OPND_VARIABLE) {
        return; /* writes to the trigger context are ignored */
    }
    l = slot ? *slot : vc_get_var(in, e->left.value);
    if (e->op != 1 && e->op != 2)
        r = operand(in, loc, &e->right);
    switch (e->op) {
    case 0: l = r; break;
    case 1: l++; break;
    case 2: l--; break;
    case 3: l += r; break;
    case 4: l -= r; break;
    case 5: l *= r; break;
    case 6: if (r) l /= r; break;
    case 7: if (r) l %= r; break;
    }
    if (slot)
        *slot = l;
    else
        vc_set_var(in, e->left.value, l);
}

static void run_list(vc_interp *in, u32 list, u32 trigger, const i32 *args, int argc);

static void run_action(vc_interp *in, locals *loc, u32 id)
{
    vc_action a;
    if (vc_read_action(in->hdb, id, &a) != 0 || !a.valid)
        return;
    {
        int pass = !a.has_condition || test_condition(in, loc, &a.condition);
        if (in->trace) {
            char line[256];
            vc_action_text(&a, line, sizeof(line));
            xf_info("%*s#%u %s%s", in->depth * 2, "", id, line, pass ? "" : "  (skipped)");
        }
        if (!pass)
            return;
    }
    switch (a.type) {
    case VC_ACT_EXPRESSION:
        run_statement(in, loc, &a.u.statement);
        break;
    case VC_ACT_ENABLE: { /* 0x420000 act_enable */
        vc_enable *e = find_enable(in, a.u.enable.id);
        if (e)
            e->enabled = a.u.enable.control != 0;
        break;
    }
    case VC_ACT_TIMER: /* 0x4285A0 act_timer */
        if (in->host.timer)
            in->host.timer(in->host.user, a.u.timer.id, a.u.timer.duration, a.u.timer.control);
        break;
    case VC_ACT_INTERFACE: /* 0x44E760 act_interface */
        if (in->host.interface)
            in->host.interface(in->host.user, a.u.iface.id, a.u.iface.control);
        break;
    case VC_ACT_SET_VIEW:
        if (in->host.go_to_view)
            in->host.go_to_view(in->host.user, a.u.set_view.view);
        break;
    case VC_ACT_MEDIA:
    case VC_ACT_SOUND:
        if (in->host.play_media)
            in->host.play_media(in->host.user, a.type == VC_ACT_MEDIA ? a.u.media.id : a.u.sound.id,
                                a.type == VC_ACT_MEDIA ? a.u.media.selector : a.u.sound.selector);
        break;
    case VC_ACT_FUNCTION:
        if (strcmp(a.u.function.name, "DoActionList") == 0 && a.u.function.argc >= 1) {
            /* 0x480FF0 fn_DoActionList(trigger, action list): every call in
             * the data names a VCTrigger and then its action list. */
            u32 list = (u32)operand(in, loc, &a.u.function.args[a.u.function.argc > 1 ? 1 : 0]);
            u32 trigger = (u32)operand(in, loc, &a.u.function.args[0]);
            if (vc_class(in->hdb, trigger) != VC_TRIGGER)
                trigger = 0;
            if (vc_class(in->hdb, list) == VC_TRIGGER) {
                xf_neo_reader r;
                list = xf_neo_object(in->hdb, list, &r, NULL, NULL) == 0 ? xf_neo_u32(&r) : 0;
            }
            run_list(in, list, trigger, NULL, 0);
        } else if (!in->host.call_function || !in->host.call_function(in->host.user, &a)) {
            xf_debug("interp: function %s not implemented", a.u.function.name);
        }
        break;
    default:
        xf_debug("interp: action type %u not implemented", a.type);
    }
}

struct vc_trigger_locals {
    u32 trigger;
    locals l;
};

/* The parameters of a trigger, kept from run to run. */
static locals *trigger_locals(vc_interp *in, u32 trigger)
{
    u32 i;
    for (i = 0; i < in->tlocal_count; i++)
        if (in->tlocals[i].trigger == trigger)
            return &in->tlocals[i].l;
    if (in->tlocal_count == in->tlocal_cap) {
        u32 cap = in->tlocal_cap ? in->tlocal_cap * 2 : 64;
        struct vc_trigger_locals *t = realloc(in->tlocals, sizeof(*t) * cap);
        if (!t)
            return NULL;
        in->tlocals = t;
        in->tlocal_cap = cap;
    }
    memset(&in->tlocals[in->tlocal_count], 0, sizeof(in->tlocals[0]));
    in->tlocals[in->tlocal_count].trigger = trigger;
    return &in->tlocals[in->tlocal_count++].l;
}

static void run_list(vc_interp *in, u32 list, u32 trigger, const i32 *args, int argc)
{
    u32 ids[MAX_LIST];
    locals scratch, *loc = NULL;
    int n, i;
    if (!list || in->depth >= MAX_DEPTH)
        return;
    if (trigger)
        loc = trigger_locals(in, trigger);
    if (!loc) {
        memset(&scratch, 0, sizeof(scratch));
        loc = &scratch;
    }
    for (i = 0; i < argc && i < 8; i++)
        loc->ints[i] = args[i];
    n = vc_read_list(in->hdb, list, ids, MAX_LIST);
    in->depth++;
    for (i = 0; i < n; i++)
        run_action(in, loc, ids[i]);
    in->depth--;
}

void vc_run_action_list(vc_interp *in, u32 list) { run_list(in, list, 0, NULL, 0); }

size_t vc_trigger_locals_size(void) { return sizeof(struct vc_trigger_locals); }

void vc_run_triggers(vc_interp *in, u32 trigger_list, int kind)
{
    u32 ids[64];
    int n = vc_read_list(in->hdb, trigger_list, ids, 64), i;
    /* Entering (kind 8) makes a new handler for the owner's triggers
     * (TriggerHandler_attach), so their parameters start again from 0;
     * they then keep their values while the owner stays on screen. */
    if (kind == 8)
        for (i = 0; i < n; i++) {
            u32 k;
            for (k = 0; k < in->tlocal_count; k++)
                if (in->tlocals[k].trigger == ids[i])
                    memset(&in->tlocals[k].l, 0, sizeof(in->tlocals[k].l));
        }
    for (i = 0; i < n; i++) {
        xf_neo_reader r;
        u32 actions;
        int k;
        if (vc_class(in->hdb, ids[i]) != VC_TRIGGER || xf_neo_object(in->hdb, ids[i], &r, NULL, NULL) != 0)
            continue;
        actions = xf_neo_u32(&r);
        k = xf_neo_u8(&r);
        if (k == kind && vc_is_enabled(in, ids[i]))
            run_list(in, actions, ids[i], NULL, 0);
    }
}

/* 0x4163D0 files a kind 7 trigger under every timer number that one of
 * its actions compares the event value with (0x480560). When that timer
 * fires, the whole action list runs with the event value set, so the
 * conditions on the event act as filters. */
static int compares_event_with(const vc_expression *e, int timer)
{
    if (e->left.kind == VC_OPND_CONTEXT && e->right.kind != VC_OPND_CONTEXT)
        return (i32)e->right.value == timer;
    if (e->right.kind == VC_OPND_CONTEXT && e->left.kind != VC_OPND_CONTEXT)
        return (i32)e->left.value == timer;
    return 0;
}

void vc_run_timer_triggers(vc_interp *in, u32 trigger_list, int timer)
{
    u32 ids[64], acts[MAX_LIST];
    int n = vc_read_list(in->hdb, trigger_list, ids, 64), i, j, m;
    i32 saved = in->context;
    for (i = 0; i < n; i++) {
        xf_neo_reader r;
        u32 actions;
        int listens = 0;
        if (vc_class(in->hdb, ids[i]) != VC_TRIGGER || xf_neo_object(in->hdb, ids[i], &r, NULL, NULL) != 0)
            continue;
        actions = xf_neo_u32(&r);
        if (xf_neo_u8(&r) != 7 || !vc_is_enabled(in, ids[i]))
            continue;
        m = vc_read_list(in->hdb, actions, acts, MAX_LIST);
        for (j = 0; j < m && !listens; j++) {
            vc_action a;
            listens = vc_read_action(in->hdb, acts[j], &a) == 0 && a.has_condition &&
                      compares_event_with(&a.condition, timer);
        }
        if (!listens)
            continue;
        in->context = timer;
        run_list(in, actions, ids[i], NULL, 0);
    }
    in->context = saved;
}
