/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf and contributors */
/* Game state and action interpreter (see docs/engine/actions.md). */
#ifndef XF_VC_INTERP_H
#define XF_VC_INTERP_H

#include "vc/action.h"

typedef struct {
    u32 id;
    i32 value;
    u8 type;    /* type flags from the GAM */
    char name[32];
} vc_var;

typedef struct {
    u32 id;      /* GAM id of the VCEnabled object */
    u32 target;  /* HDB object it controls */
    u8 enabled;
} vc_enable;

/* What the interpreter asks the game to do. */
typedef struct {
    void *user;
    void (*go_to_view)(void *user, u32 view);
    void (*play_media)(void *user, u32 asset_ref, u8 selector);
    /* Timer action: control 0 starts timer `id` (bit 0x80: periodic), 1 stops it. */
    void (*timer)(void *user, u8 id, u32 ms, u8 control);
    /* Interface action: control 0 shows layout `id`, 1 closes it. */
    void (*interface)(void *user, u32 id, u8 control);
    /* Returns 1 when the function was handled. */
    int (*call_function)(void *user, const vc_action *a);
} vc_host;

typedef struct {
    const xf_neo_db *hdb;
    vc_var *vars;
    u32 var_count;
    vc_enable *enables;       /* sorted by id */
    u32 enable_count;
    u32 *enable_by_target;    /* indexes into enables, sorted by target */
    vc_host host;
    i32 context;              /* value of the event that runs the triggers (operand kind 4) */
    /* Parameters (locals) belong to the trigger that runs a list and keep
     * their values between runs: 0x40C2B0 makes the trigger the context
     * of each queued action, and parameter operands read it. */
    struct vc_trigger_locals *tlocals;
    u32 tlocal_count, tlocal_cap;
    int depth;
    int trace;                /* print every action */
} vc_interp;

/* Loads the initial state from the GAM database. */
int vc_interp_init(vc_interp *in, const xf_neo_db *hdb, const xf_neo_db *gam, const vc_host *host);
void vc_interp_free(vc_interp *in);

i32 vc_get_var(const vc_interp *in, u32 id);
/* Finds a variable by its GAM name; returns its id, or 0. */
u32 vc_find_var(const vc_interp *in, const char *name);
void vc_set_var(vc_interp *in, u32 id, i32 value);
/* 1 unless a VCEnabled object turns the HDB object off. */
int vc_is_enabled(const vc_interp *in, u32 hdb_object);
/* Turns an HDB object on or off through its VCEnabled entry, if it has one. */
void vc_set_enabled(vc_interp *in, u32 hdb_object, int on);

/* Runs every action of an action list. */
void vc_run_action_list(vc_interp *in, u32 list);
/* Size of one entry of `tlocals`, for saving and restoring them. */
size_t vc_trigger_locals_size(void);
/* Runs the triggers of `kind` in a trigger list. */
void vc_run_triggers(vc_interp *in, u32 trigger_list, int kind);

/* Runs the timer triggers (kind 7) of a trigger list for timer `timer`:
 * only the actions whose condition reads the trigger context run. */
void vc_run_timer_triggers(vc_interp *in, u32 trigger_list, int timer);

#endif
