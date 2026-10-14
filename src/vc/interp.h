/* Game state and action interpreter (see docs/engine/actions.md). */
#ifndef XF_VC_INTERP_H
#define XF_VC_INTERP_H

#include "vc/action.h"

typedef struct {
    u32 id;
    i32 value;
    u8 type;    /* type flags from the GAM */
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
    int depth;
    int trace;                /* print every action */
} vc_interp;

/* Loads the initial state from the GAM database. */
int vc_interp_init(vc_interp *in, const xf_neo_db *hdb, const xf_neo_db *gam, const vc_host *host);
void vc_interp_free(vc_interp *in);

i32 vc_get_var(const vc_interp *in, u32 id);
void vc_set_var(vc_interp *in, u32 id, i32 value);
/* 1 unless a VCEnabled object turns the HDB object off. */
int vc_is_enabled(const vc_interp *in, u32 hdb_object);

/* Runs every action of an action list. */
void vc_run_action_list(vc_interp *in, u32 list);
/* Runs the triggers of `kind` in a trigger list. */
void vc_run_triggers(vc_interp *in, u32 trigger_list, int kind);

#endif
