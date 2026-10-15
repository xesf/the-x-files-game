/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* Saved games (see docs/formats/save.md). The original engine copies the
 * GAM to Temp.gam and saves it, with a header object, as a NeoAccess .x
 * file (0x4D14B0). We keep the same state in a chunked file: the GAM
 * variables, enable states and strings, the trigger parameters, the view,
 * and what the engine itself holds (mail, photos, notes, ...). */
#include "game/game.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/xf_log.h"

#define SAVE_MAGIC "XFSV"
#define SAVE_VERSION 1

typedef struct {
    u8 *data;
    size_t size, cap;
    int error;
} wbuf;

static void w_bytes(wbuf *w, const void *p, size_t n)
{
    if (w->error)
        return;
    if (w->size + n > w->cap) {
        size_t cap = w->cap ? w->cap * 2 : 65536;
        u8 *d;
        while (cap < w->size + n)
            cap *= 2;
        if (!(d = realloc(w->data, cap))) {
            w->error = 1;
            return;
        }
        w->data = d;
        w->cap = cap;
    }
    memcpy(w->data + w->size, p, n);
    w->size += n;
}

static void w_u32(wbuf *w, u32 v)
{
    u8 b[4];
    b[0] = (u8)v;
    b[1] = (u8)(v >> 8);
    b[2] = (u8)(v >> 16);
    b[3] = (u8)(v >> 24);
    w_bytes(w, b, 4);
}

/* A chunk: four letters, then the size of what follows. */
static size_t w_begin(wbuf *w, const char *tag)
{
    w_bytes(w, tag, 4);
    w_u32(w, 0);
    return w->size;
}

static void w_end(wbuf *w, size_t start)
{
    u32 n = (u32)(w->size - start);
    if (w->error)
        return;
    w->data[start - 4] = (u8)n;
    w->data[start - 3] = (u8)(n >> 8);
    w->data[start - 2] = (u8)(n >> 16);
    w->data[start - 1] = (u8)(n >> 24);
}

static void w_u32s(wbuf *w, const char *tag, const u32 *v, int n)
{
    size_t c = w_begin(w, tag);
    int i;
    w_u32(w, (u32)n);
    for (i = 0; i < n; i++)
        w_u32(w, v[i]);
    w_end(w, c);
}

typedef struct {
    const u8 *p, *end;
} rbuf;

static u32 r_u32(rbuf *r)
{
    u32 v;
    if (r->end - r->p < 4) {
        r->p = r->end;
        return 0;
    }
    v = (u32)r->p[0] | (u32)r->p[1] << 8 | (u32)r->p[2] << 16 | (u32)r->p[3] << 24;
    r->p += 4;
    return v;
}

static void r_bytes(rbuf *r, void *out, size_t n)
{
    if ((size_t)(r->end - r->p) < n) {
        memset(out, 0, n);
        r->p = r->end;
        return;
    }
    memcpy(out, r->p, n);
    r->p += n;
}

/* Reads a counted u32 array into v (at most max), returns the count. */
static int r_u32s(rbuf *r, u32 *v, int max)
{
    int n = (int)r_u32(r), i;
    for (i = 0; i < n; i++) {
        u32 x = r_u32(r);
        if (i < max)
            v[i] = x;
    }
    return n < max ? n : max;
}

int game_save_mem(game *g, const char *name, u8 **out, size_t *size)
{
    wbuf w = { 0 };
    size_t c;
    u32 i, tmp[GAME_MAX_TIMERS * 3];
    int k;

    w_bytes(&w, SAVE_MAGIC, 4);
    w_u32(&w, SAVE_VERSION);

    c = w_begin(&w, "NAME");
    w_bytes(&w, name ? name : "", strlen(name ? name : "") + 1);
    w_end(&w, c);

    c = w_begin(&w, "VIEW");
    w_u32(&w, g->view);
    w_u32(&w, (u32)g->disc);
    w_end(&w, c);
    w_u32s(&w, "HIST", g->history, g->history_len);

    c = w_begin(&w, "VARS");
    w_u32(&w, g->interp.var_count);
    for (i = 0; i < g->interp.var_count; i++) {
        w_u32(&w, g->interp.vars[i].id);
        w_u32(&w, (u32)g->interp.vars[i].value);
    }
    w_end(&w, c);

    c = w_begin(&w, "ENAB");
    w_u32(&w, g->interp.enable_count);
    for (i = 0; i < g->interp.enable_count; i++) {
        w_u32(&w, g->interp.enables[i].id);
        w_u32(&w, g->interp.enables[i].enabled);
    }
    w_end(&w, c);

    c = w_begin(&w, "STRS");
    w_u32(&w, g->interp.string_count);
    for (i = 0; i < g->interp.string_count; i++) {
        w_u32(&w, g->interp.strings[i].id);
        w_bytes(&w, g->interp.strings[i].text, sizeof(g->interp.strings[i].text));
    }
    w_end(&w, c);

    c = w_begin(&w, "TLOC");
    w_u32(&w, g->interp.tlocal_count);
    for (i = 0; i < g->interp.tlocal_count; i++) {
        i32 v[VC_TRIGGER_LOCALS];
        w_u32(&w, vc_trigger_locals_get(&g->interp, i, v));
        for (k = 0; k < VC_TRIGGER_LOCALS; k++)
            w_u32(&w, (u32)v[k]);
    }
    w_end(&w, c);

    /* Timers keep the time left. */
    for (k = 0; k < g->timer_count; k++) {
        tmp[k * 3] = g->timers[k].id;
        tmp[k * 3 + 1] = (i32)(g->timers[k].due - g->now) > 0 ? g->timers[k].due - g->now : 0;
        tmp[k * 3 + 2] = g->timers[k].period;
    }
    w_u32s(&w, "TIME", tmp, g->timer_count * 3);

    c = w_begin(&w, "ASKD");
    w_u32(&w, (u32)g->asked_count);
    for (k = 0; k < g->asked_count; k++) {
        w_u32(&w, g->asked[k].conversation);
        w_u32(&w, (u32)g->asked[k].count);
    }
    w_end(&w, c);

    c = w_begin(&w, "MAIL");
    w_u32(&w, (u32)g->email_count);
    for (k = 0; k < g->email_count; k++) {
        w_u32(&w, g->emails[k].asset);
        w_u32(&w, g->emails[k].read);
    }
    w_end(&w, c);

    c = w_begin(&w, "REGS");
    w_u32(&w, (u32)g->reg_count);
    for (k = 0; k < g->reg_count; k++) {
        int j;
        w_bytes(&w, g->regs[k].name, sizeof(g->regs[k].name));
        w_u32(&w, (u32)g->regs[k].argc);
        for (j = 0; j < VC_MAX_ARGS; j++)
            w_u32(&w, g->regs[k].vars[j]);
    }
    w_end(&w, c);

    w_u32s(&w, "PHOT", g->photos, g->photo_count);
    w_u32s(&w, "NOTE", g->pda_notes, g->pda_note_count);
    w_u32s(&w, "KITI", g->kit_icons, g->kit_count);
    w_u32s(&w, "EVID", g->evidence, g->evidence_count);
    {
        /* Filled one by one: C89 compilers (Watcom) want constant
         * initializers. */
        u32 misc[30];
        int n = 0;
        misc[n++] = g->rand_seed;
        misc[n++] = (u32)g->ing_cook_shown;
        misc[n++] = g->ing_notes;
        misc[n++] = g->ai_trigger;
        misc[n++] = g->ai_list;
        misc[n++] = (u32)g->ai_glow;
        misc[n++] = g->smol_vars[0];
        misc[n++] = g->smol_vars[1];
        misc[n++] = g->smol_vars[2];
        misc[n++] = (u32)g->smol_clear[0];
        misc[n++] = (u32)g->smol_clear[1];
        misc[n++] = (u32)g->smol_clear[2];
        misc[n++] = (u32)g->smol_visits[0];
        misc[n++] = (u32)g->smol_visits[1];
        misc[n++] = (u32)g->smol_visits[2];
        misc[n++] = (u32)g->smol_area;
        misc[n++] = (u32)g->woods_down;
        misc[n++] = (u32)g->text_off;
        misc[n++] = (u32)g->credits_rolling;
        misc[n++] = (u32)g->flight_mode;
        misc[n++] = (u32)(u8)g->flight_place;
        misc[n++] = (u32)g->lbox_tinted;
        misc[n++] = g->lbox_tint[0];
        misc[n++] = g->lbox_tint[1];
        misc[n++] = g->lbox_tint[2];
        misc[n++] = g->binoc_from;
        misc[n++] = g->sound;
        misc[n++] = (u32)g->show_evidence;
        misc[n++] = (u32)g->rauch_down;
        misc[n++] = (u32)g->note_page;
        w_u32s(&w, "MISC", misc, n);
    }
    if (w.error) {
        free(w.data);
        return -1;
    }
    *out = w.data;
    *size = w.size;
    return 0;
}

/* Copies the saved name out of a save; returns 0 when it is one. */
int game_save_name(const u8 *data, size_t size, char *name, size_t name_size)
{
    rbuf r;
    name[0] = '\0';
    if (size < 8 || memcmp(data, SAVE_MAGIC, 4) != 0)
        return -1;
    r.p = data + 8;
    r.end = data + size;
    while (r.end - r.p >= 8) {
        char tag[4];
        u32 n;
        r_bytes(&r, tag, 4);
        n = r_u32(&r);
        if (n > (size_t)(r.end - r.p))
            return -1;
        if (memcmp(tag, "NAME", 4) == 0) {
            snprintf(name, name_size, "%.*s", (int)n, (const char *)r.p);
            return 0;
        }
        r.p += n;
    }
    return 0;
}

/* Back to the state of a fresh start: the GAM as shipped, nothing on
 * screen. The serials go on counting so that the window notices. */
int game_reset(game *g)
{
    char dir[sizeof(g->data_dir)];
    u32 serials[4];
    u8 *cp = g->checkpoint;
    size_t cp_size = g->checkpoint_size;
    char save_dir[sizeof(g->save_dir)];
    u32 now = g->now;
    u8 play_once[sizeof(g->play_once)];
    serials[0] = g->iface_serial;
    serials[1] = g->sound_serial;
    serials[2] = g->fade_serial;
    serials[3] = g->overlay_serial;
    memcpy(play_once, g->play_once, sizeof(play_once)); /* per session, like the original */
    memcpy(dir, g->data_dir, sizeof(dir));
    memcpy(save_dir, g->save_dir, sizeof(save_dir));
    g->checkpoint = NULL;
    game_close(g);
    if (game_open(g, dir) != 0) {
        free(cp);
        return -1;
    }
    g->iface_serial = serials[0] + 1;
    g->sound_serial = serials[1] + 1;
    g->fade_serial = serials[2] + 1;
    g->overlay_serial = serials[3] + 1;
    g->checkpoint = cp;
    g->checkpoint_size = cp_size;
    g->now = now;
    memcpy(g->play_once, play_once, sizeof(play_once));
    memcpy(g->save_dir, save_dir, sizeof(save_dir));
    return 0;
}

int game_load_mem(game *g, const u8 *data, size_t size)
{
    rbuf r;
    u32 view = 0, history[GAME_HISTORY];
    int history_len = 0;
    u32 timers[GAME_MAX_TIMERS * 3];
    int timer_n = 0, k;

    if (size < 8 || memcmp(data, SAVE_MAGIC, 4) != 0) {
        xf_warn("save: not a saved game");
        return -1;
    }
    r.p = data + 4;
    r.end = data + size;
    if (r_u32(&r) != SAVE_VERSION) {
        xf_warn("save: unknown save version");
        return -1;
    }
    if (game_reset(g) != 0)
        return -1;
    while (r.end - r.p >= 8) {
        char tag[4];
        u32 n, i;
        rbuf c;
        r_bytes(&r, tag, 4);
        n = r_u32(&r);
        if (n > (size_t)(r.end - r.p))
            break;
        c.p = r.p;
        c.end = r.p + n;
        r.p += n;
        if (memcmp(tag, "VIEW", 4) == 0) {
            view = r_u32(&c);
            g->disc = (int)r_u32(&c);
        } else if (memcmp(tag, "HIST", 4) == 0) {
            history_len = r_u32s(&c, history, GAME_HISTORY);
        } else if (memcmp(tag, "VARS", 4) == 0) {
            u32 count = r_u32(&c);
            for (i = 0; i < count && c.p < c.end; i++) {
                u32 id = r_u32(&c);
                vc_set_var(&g->interp, id, (i32)r_u32(&c));
            }
        } else if (memcmp(tag, "ENAB", 4) == 0) {
            u32 count = r_u32(&c), j;
            for (i = 0; i < count && c.p < c.end; i++) {
                u32 id = r_u32(&c), on = r_u32(&c);
                for (j = 0; j < g->interp.enable_count; j++)
                    if (g->interp.enables[j].id == id) {
                        g->interp.enables[j].enabled = (u8)(on != 0);
                        break;
                    }
            }
        } else if (memcmp(tag, "STRS", 4) == 0) {
            u32 count = r_u32(&c);
            for (i = 0; i < count && c.p < c.end; i++) {
                u32 id = r_u32(&c);
                char text[sizeof(g->interp.strings[0].text)];
                r_bytes(&c, text, sizeof(text));
                text[sizeof(text) - 1] = '\0';
                vc_string_set(&g->interp, id, text);
            }
        } else if (memcmp(tag, "TLOC", 4) == 0) {
            u32 count = r_u32(&c);
            for (i = 0; i < count && c.p < c.end; i++) {
                u32 trigger = r_u32(&c);
                i32 v[VC_TRIGGER_LOCALS];
                for (k = 0; k < VC_TRIGGER_LOCALS; k++)
                    v[k] = (i32)r_u32(&c);
                vc_trigger_locals_set(&g->interp, trigger, v);
            }
        } else if (memcmp(tag, "TIME", 4) == 0) {
            timer_n = r_u32s(&c, timers, GAME_MAX_TIMERS * 3) / 3;
        } else if (memcmp(tag, "ASKD", 4) == 0) {
            u32 count = r_u32(&c);
            for (i = 0; i < count && c.p < c.end; i++) {
                u32 conv = r_u32(&c), times = r_u32(&c);
                if (g->asked_count < GAME_MAX_ASKED) {
                    g->asked[g->asked_count].conversation = conv;
                    g->asked[g->asked_count++].count = (int)times;
                }
            }
        } else if (memcmp(tag, "MAIL", 4) == 0) {
            u32 count = r_u32(&c);
            for (i = 0; i < count && c.p < c.end; i++) {
                u32 asset = r_u32(&c), read = r_u32(&c);
                if (g->email_count < GAME_MAX_EMAILS) {
                    g->emails[g->email_count].asset = asset;
                    g->emails[g->email_count++].read = (u8)read;
                }
            }
        } else if (memcmp(tag, "REGS", 4) == 0) {
            u32 count = r_u32(&c);
            for (i = 0; i < count && c.p < c.end; i++) {
                game_registration reg;
                int j;
                r_bytes(&c, reg.name, sizeof(reg.name));
                reg.name[sizeof(reg.name) - 1] = '\0';
                reg.argc = (int)r_u32(&c);
                for (j = 0; j < VC_MAX_ARGS; j++)
                    reg.vars[j] = r_u32(&c);
                if (g->reg_count < (int)XF_COUNT(g->regs))
                    g->regs[g->reg_count++] = reg;
            }
        } else if (memcmp(tag, "PHOT", 4) == 0) {
            g->photo_count = r_u32s(&c, g->photos, GAME_MAX_PHOTOS);
        } else if (memcmp(tag, "NOTE", 4) == 0) {
            g->pda_note_count = r_u32s(&c, g->pda_notes, GAME_MAX_NOTES);
        } else if (memcmp(tag, "KITI", 4) == 0) {
            g->kit_count = r_u32s(&c, g->kit_icons, GAME_MAX_EVIDENCE);
        } else if (memcmp(tag, "EVID", 4) == 0) {
            g->evidence_count = r_u32s(&c, g->evidence, GAME_MAX_EVIDENCE);
        } else if (memcmp(tag, "MISC", 4) == 0) {
            u32 m[32] = { 0 };
            r_u32s(&c, m, 32);
            g->rand_seed = m[0];
            g->ing_cook_shown = (int)m[1];
            g->ing_notes = m[2];
            g->ai_trigger = m[3];
            g->ai_list = m[4];
            g->ai_glow = (i32)m[5];
            for (k = 0; k < 3; k++) {
                g->smol_vars[k] = m[6 + k];
                g->smol_clear[k] = (int)m[9 + k];
                g->smol_visits[k] = (int)m[12 + k];
            }
            g->smol_area = (int)m[15];
            g->woods_down = (int)m[16];
            g->text_off = (int)m[17];
            g->credits_rolling = (int)m[18];
            g->flight_mode = (int)m[19];
            g->flight_place = (char)m[20];
            g->lbox_tinted = (int)m[21];
            g->lbox_tint[0] = (u8)m[22];
            g->lbox_tint[1] = (u8)m[23];
            g->lbox_tint[2] = (u8)m[24];
            g->binoc_from = m[25];
            g->sound = m[26];
            g->show_evidence = (int)m[27];
            g->rauch_down = (int)m[28];
            g->note_page = (int)m[29];
            if (g->sound)
                g->sound_serial++;
        }
    }
    for (k = 0; k < timer_n && g->timer_count < GAME_MAX_TIMERS; k++) {
        game_timer *t = &g->timers[g->timer_count++];
        t->id = (u8)timers[k * 3];
        t->due = g->now + timers[k * 3 + 1];
        t->period = timers[k * 3 + 2];
    }
    if (!view || game_resume_view(g, view) != 0) {
        xf_warn("save: no view to go to");
        return -1;
    }
    memcpy(g->history, history, sizeof(history[0]) * (size_t)history_len);
    g->history_len = history_len;
    xf_info("save: loaded, view %u", view);
    return 0;
}

int game_save_file(game *g, const char *path, const char *name)
{
    u8 *data;
    size_t size;
    FILE *f;
    int ok;
    if (game_save_mem(g, name, &data, &size) != 0)
        return -1;
    if (!(f = fopen(path, "wb"))) {
        xf_warn("save: cannot write %s", path);
        free(data);
        return -1;
    }
    ok = fwrite(data, 1, size, f) == size;
    ok &= fclose(f) == 0;
    free(data);
    return ok ? 0 : -1;
}

static u8 *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    u8 *data = NULL;
    long n;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0 &&
        (data = malloc((size_t)n)) && fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        data = NULL;
    }
    if (data)
        *size = (size_t)n;
    fclose(f);
    return data;
}

int game_load_file(game *g, const char *path)
{
    size_t size = 0;
    u8 *data = read_file(path, &size);
    int r;
    if (!data) {
        xf_warn("save: cannot read %s", path);
        return -1;
    }
    r = game_load_mem(g, data, size);
    free(data);
    return r;
}

int game_save_file_name(const char *path, char *name, size_t name_size)
{
    size_t size = 0;
    u8 *data = read_file(path, &size);
    int r;
    name[0] = '\0';
    if (!data)
        return -1;
    r = game_save_name(data, size, name, name_size);
    free(data);
    return r;
}

/* 0x4BBFA0 fn_SaveGameState: the story keeps a checkpoint before danger;
 * after a death, EndGame offers to go back to it. */
void game_checkpoint(game *g)
{
    u8 *data;
    size_t size;
    if (game_save_mem(g, "checkpoint", &data, &size) != 0)
        return;
    free(g->checkpoint);
    g->checkpoint = data;
    g->checkpoint_size = size;
    xf_info("checkpoint at view %u", g->view);
}

int game_restore_checkpoint(game *g)
{
    u8 *data;
    size_t size = g->checkpoint_size;
    int r;
    if (!g->checkpoint)
        return -1;
    /* game_reset keeps the checkpoint; load from a copy all the same. */
    if (!(data = malloc(size)))
        return -1;
    memcpy(data, g->checkpoint, size);
    r = game_load_mem(g, data, size);
    free(data);
    return r;
}
