/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf and contributors */
/* Decoding of VCAction data (see docs/engine/actions.md). */
#ifndef XF_VC_ACTION_H
#define XF_VC_ACTION_H

#include "vc/vc.h"

enum {
    VC_ACT_EXPRESSION = 0, /* variable statement */
    VC_ACT_MEDIA = 1,      /* play a media reference */
    VC_ACT_TIMER = 2,
    VC_ACT_ENABLE = 3,
    VC_ACT_SET_VIEW = 4,
    VC_ACT_INTERFACE = 5,
    VC_ACT_FUNCTION = 6,
    VC_ACT_SOUND = 7,
};

/* Operand kinds */
enum {
    VC_OPND_VARIABLE = 0,  /* GAM variable id */
    VC_OPND_PARAMETER = 1, /* local of the running list: 0-7, 10000-10011, 20000-20003 */
    VC_OPND_LITERAL = 2,   /* number */
    VC_OPND_CHAR = 3,      /* character, for example 'A' */
    VC_OPND_CONTEXT = 4    /* value of the event: timer number (kind 7), key (kind 3) */
};

typedef struct {
    u32 value;
    u8 kind;
} vc_operand;

typedef struct {
    vc_operand left, right;
    u8 op;
} vc_expression;

#define VC_MAX_ARGS 32 /* RegPreferences passes 24 */

typedef struct {
    u8 type;          /* VC_ACT_* */
    int has_condition;
    vc_expression condition;
    int valid;        /* the data has the size this type expects */
    union {
        vc_expression statement;
        struct { u32 id; u8 selector; } media;
        struct { u32 duration; u8 id, control; } timer; /* ms; control 0 start, 1 stop, bit 0x80 periodic */
        struct { u32 id, value; u8 control; } enable; /* control: new state; value is always 0 */
        struct { u32 view, node, location, viewpoint; } set_view;
        struct { u32 id; u8 control; } iface;
        struct { char name[16]; int argc; vc_operand args[VC_MAX_ARGS]; } function;
        struct { u32 id; u8 selector, extra; u16 a, b; } sound;
    } u;
} vc_action;

int vc_read_action(const xf_neo_db *db, u32 id, vc_action *a);
/* Writes a one-line description of the action. */
void vc_action_text(const vc_action *a, char *out, size_t size);

#endif
