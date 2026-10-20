/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
#include "vseq.h"

#include <string.h>

#include "xf_log.h"

/* How an instruction leaves its sequence (seg8): it goes on to the next
 * instruction at once (the handlers that end through seg8:1CC2 run
 * seg8:09C4 again), or the sequence waits for the next tick (through
 * seg8:65FF, which stores the pointer and returns). */
enum { RUN, YIELD, STOP };


/* At most five instructions a tick for each sequence (DS:44F0). */
#define MAX_DEPTH 5

u16 vs_get(const vs_state *s, int seq, int off) { return s->ctx[seq][off / 2]; }
void vs_set(vs_state *s, int seq, int off, u16 v) { s->ctx[seq][off / 2] = v; }

vx_ptr vs_get_ptr(const vs_state *s, int seq, int off)
{
    return VX_PTR(s->ctx[seq][off / 2 + 1], s->ctx[seq][off / 2]);
}

void vs_set_ptr(vs_state *s, int seq, int off, vx_ptr p)
{
    s->ctx[seq][off / 2] = VX_OFF(p);
    s->ctx[seq][off / 2 + 1] = (u16)VX_SEG(p);
}

static u32 get32(const vs_state *s, int seq, int off)
{
    return (u32)s->ctx[seq][off / 2] | (u32)s->ctx[seq][off / 2 + 1] << 16;
}

static void set32(vs_state *s, int seq, int off, u32 v)
{
    s->ctx[seq][off / 2] = (u16)v;
    s->ctx[seq][off / 2 + 1] = (u16)(v >> 16);
}

/* Clears a slot (seg8:0000 does this for all at start). */
static void clear_slot(vs_state *s, int i)
{
    memset(s->ctx[i], 0, sizeof(s->ctx[i]));
    vs_set(s, i, VC_INDEX, (u16)i);
    vs_set(s, i, VC_SPRITE, 0xFFFF);
    vs_set(s, i, 0x122, 0xFFFF);
    vs_set(s, i, 0x124, 0xFFFF);
}

void vs_init(vs_state *s, const vexe *x, const vs_host *host)
{
    int i;
    memset(s, 0, sizeof(*s));
    s->x = x;
    s->host = *host;
    s->rand_seed = 1;
    for (i = 0; i < VS_SEQS; i++)
        clear_slot(s, i);
    /* seg8:00B8: the first sequence starts at seg18:0000 */
    vs_set(s, 0, VC_USED, 1);
    vs_set_ptr(s, 0, VC_PC, VX_PTR(18, 0));
}

/* --- Operands (seg8:6EEA, 6F62, 6FEC) ------------------------------------
 * The top two bits: 00 the value itself, 01 a local (context + 0x64),
 * 10 a global (DS:39F8), 11 a word of the context. */
static u16 *operand_ref(vs_state *s, int seq, u16 w)
{
    static u16 scratch;
    u16 n = w & 0x3FFF;
    switch (w & 0xC000) {
    case 0x4000:
        return n < 6 ? &s->ctx[seq][VC_LOCALS / 2 + n] : &scratch;
    case 0x8000:
        return n < VS_GLOBALS ? &s->g[n] : &scratch;
    case 0xC000:
        return n < VS_CTX_WORDS ? &s->ctx[seq][n] : &scratch;
    }
    scratch = w; /* seg8:6F15: a value, through a scratch word */
    return &scratch;
}

static u16 operand(vs_state *s, int seq, u16 w)
{
    return *operand_ref(s, seq, w);
}

/* --- Reading instructions ------------------------------------------------- */

typedef struct {
    vs_state *s;
    int seq;
    vx_ptr pc;
} insn;

static u16 w_at(const insn *in, int off) { return vexe_u16(in->s->x, in->pc + (u32)off); }
static u8 b_at(const insn *in, int off) { return vexe_u8(in->s->x, in->pc + (u32)off); }
static vx_ptr far_at(const insn *in, int off) { return vexe_far(in->s->x, in->pc + (u32)off); }
static u16 val(const insn *in, int off) { return operand(in->s, in->seq, w_at(in, off)); }
static u16 *ref(const insn *in, int off) { return operand_ref(in->s, in->seq, w_at(in, off)); }

/* A string at `off`; its length (with the 0) in *len. */
static const char *str_at(const insn *in, int off, u32 *len)
{
    u32 avail, n;
    const char *p = (const char *)vexe_at(in->s->x, in->pc + (u32)off, &avail);
    if (!p) {
        *len = 0;
        return "";
    }
    for (n = 0; n < avail && p[n]; n++)
        ;
    *len = n + 1;
    return p;
}

/* --- Sequences ------------------------------------------------------------ */

static void stop_seq(vs_state *s, int i)
{
    /* seg8:768C: the sprite goes, the slot is freed */
    vs_set(s, i, VC_USED, 0);
    vs_set(s, i, VC_SPRITE, 0xFFFF);
    vs_set(s, i, 0x122, 0xFFFF);
    vs_set(s, i, 0x124, 0xFFFF);
    set32(s, i, VC_HANDLED, 0);
    set32(s, i, VC_EVENTS, 0);
    set32(s, i, VC_WAIT, 0);
}

/* Sequences a number names (seg8:0C84): 0x7FFF all; 0x2D to 0x3FF the
 * ones whose number (VC_ID) it is; below 0x2D a slot. */
static int matches(const vs_state *s, int i, u16 who)
{
    if (!vs_get(s, i, VC_USED))
        return 0;
    if (who == 0x7FFF)
        return 1;
    if (who >= 0x2D && who <= 0x3FF)
        return vs_get(s, i, VC_ID) == who;
    return i == who;
}

/* seg8:71E0: sequence `to` takes `from`'s state (its sprite, place,
 * clip, waits, handlers, locals, pointer); it keeps its slot, parent and
 * number. */
static void become(vs_state *s, int from, int to)
{
    u16 parent = vs_get(s, to, VC_PARENT), id = vs_get(s, to, VC_ID);
    memcpy(s->ctx[to], s->ctx[from], sizeof(s->ctx[to]));
    vs_set(s, to, VC_INDEX, (u16)to);
    vs_set(s, to, VC_PARENT, parent);
    vs_set(s, to, VC_ID, id);
    vs_set(s, to, VC_USED, 1);
    if (s->host.moved)
        s->host.moved(s->host.user, from, to);
}

/* Pushes the return address (seg8:7590): eight slots, the oldest dropped
 * when full. */
static void push_return(vs_state *s, int seq, vx_ptr ret)
{
    int k;
    for (k = 0; k < 8 && vs_get_ptr(s, seq, VC_STACK + 4 * k); k++)
        ;
    if (k == 8) {
        for (k = 0; k < 7; k++)
            vs_set_ptr(s, seq, VC_STACK + 4 * k, vs_get_ptr(s, seq, VC_STACK + 4 * k + 4));
        k = 7;
    }
    vs_set_ptr(s, seq, VC_STACK + 4 * k, ret);
}

static vx_ptr pop_return(vs_state *s, int seq)
{
    int k;
    vx_ptr p;
    for (k = 7; k > 0 && !vs_get_ptr(s, seq, VC_STACK + 4 * k); k--)
        ;
    p = vs_get_ptr(s, seq, VC_STACK + 4 * k);
    vs_set_ptr(s, seq, VC_STACK + 4 * k, 0);
    return p;
}

static u16 rnd(vs_state *s)
{
    s->rand_seed = s->rand_seed * 0x343FD + 0x269EC3; /* the C library's rand() */
    return (u16)(s->rand_seed >> 16 & 0x7FFF);
}

/* 0x44C, 0x44D: dst = a OP b, OP from the table at seg8:39DA (0x44C, b a
 * variable) and seg8:3B12 (0x44D, b a value). */
static u16 binop(int op, u16 a, u16 b, int literal)
{
    switch (op) {
    case 0: return a == b;
    case 1: return a != b;
    case 2: return a && b;
    case 3: return a || b;
    case 4: return !a && !b;
    case 5: return !a != !b;
    case 6: return a && !b;
    case 7: return a & b;
    case 8: return a | b;
    case 9: return literal ? (u16)~(a | b) : (u16)((b == 0) & ~a); /* as seg8:3998 does */
    case 10: return a ^ b;
    case 11: return a & (u16)~b;
    }
    return 0;
}

/* A sequence's size is its sprite's (seg8:7968, before each
 * instruction); a slot past the sprites is none. */
static void sprite_size(vs_state *s, int seq)
{
    u16 slot = vs_get(s, seq, VC_SPRITE);
    int w, h;
    if (slot >= VS_SPRITES) {
        vs_set(s, seq, VC_SPRITE, 0xFFFF);
        return;
    }
    if (s->host.sprite_size && s->host.sprite_size(s->host.user, slot, &w, &h) == 0) {
        vs_set(s, seq, VC_W, (u16)w);
        vs_set(s, seq, VC_H, (u16)h);
    }
}

/* Runs one instruction. */
static int step(vs_state *s, int seq)
{
    insn in;
    u16 w, op, *d;
    u32 n;
    const char *str;
    int i, k;
    vexe_seg const *sg;

    sprite_size(s, seq);
    in.s = s;
    in.seq = seq;
    in.pc = vs_get_ptr(s, seq, VC_PC);
    sg = VX_SEG(in.pc) >= 1 && VX_SEG(in.pc) <= s->x->seg_count ? &s->x->segs[VX_SEG(in.pc)] : NULL;
    if (!sg || !sg->data || VX_OFF(in.pc) + 2u > sg->size)
        return YIELD;
    w = w_at(&in, 0);
    op = w & 0x7FFF;
    xf_debug("seq %d seg%d:%04X op %04X", seq, VX_SEG(in.pc), VX_OFF(in.pc), op);

#define NEXT(len) (vs_set_ptr(s, seq, VC_PC, in.pc + (len)))
#define GOTO(p) (vs_set_ptr(s, seq, VC_PC, (p)))

    switch (op) {
    case 0x0000: /* the end: the default case (seg8:0A02) leaves it where it is */
        return YIELD;
    case 0x0001: { /* start a sequence at a pointer; its slot into a variable */
        u16 dst = w_at(&in, 2);
        for (i = 0; i < VS_SEQS && vs_get(s, i, VC_USED); i++)
            ;
        if (i < VS_SEQS) {
            clear_slot(s, i);
            vs_set(s, i, VC_PARENT, (u16)seq);
            vs_set(s, i, VC_USED, 1);
            vs_set_ptr(s, i, VC_PC, far_at(&in, 4));
            if (dst & 0x8000)
                *operand_ref(s, seq, dst) = (u16)i;
            else if ((dst & 0x3FFF) < 6)
                s->ctx[seq][VC_LOCALS / 2 + (dst & 0x3FFF)] = (u16)i;
        }
        NEXT(8);
        return YIELD;
    }
    case 0x0002: { /* stop sequences (0x7FFF: all but this one) */
        u16 who = val(&in, 2);
        int self = 0;
        for (i = 0; i < VS_SEQS; i++)
            if (matches(s, i, who) && !(who == 0x7FFF && i == seq)) {
                if (i == seq)
                    self = 1;
                else
                    stop_seq(s, i);
            }
        NEXT(6);
        if (self) {
            stop_seq(s, seq);
            return STOP;
        }
        return YIELD;
    }
    case 0x0003: { /* carry on in another sequence's slot (seg8:0B74,
                    * 71E0): the state moves there, the pointer after this
                    * instruction; given a slot, this one stops */
        u16 who = val(&in, 2);
        if (seq == 0) {
            NEXT(4);
            return RUN;
        }
        NEXT(4);
        if (who < 0x2D) {
            if (who != seq)
                become(s, seq, who);
            if (who != seq) {
                stop_seq(s, seq);
                return STOP;
            }
            return YIELD;
        }
        for (i = 0; i < VS_SEQS; i++)
            if (i != seq && matches(s, i, who))
                become(s, seq, i);
        return YIELD;
    }
    case 0x0005: { /* a random number from lo to hi */
        u16 lo = w_at(&in, 4), hi = w_at(&in, 6);
        if (lo > hi) {
            u16 t = lo;
            lo = hi;
            hi = t;
        }
        *ref(&in, 2) = (u16)(lo + rnd(s) % (u32)(hi - lo + 1));
        NEXT(8);
        return YIELD;
    }
    case 0x0006: /* nothing */
        NEXT(2);
        return RUN;

    /* variables */
    case 0x0406: *ref(&in, 2) = val(&in, 4); NEXT(6); return RUN;
    case 0x0407: *ref(&in, 2) = w_at(&in, 4); NEXT(6); return RUN;
    case 0x03FC: *ref(&in, 2) = 0; NEXT(4); return YIELD;
    case 0x0410: *ref(&in, 2) |= val(&in, 4); NEXT(6); return YIELD;
    case 0x0411: *ref(&in, 2) |= w_at(&in, 4); NEXT(6); return YIELD;
    case 0x0412: *ref(&in, 2) &= (u16)~val(&in, 4); NEXT(6); return YIELD;
    case 0x0413: *ref(&in, 2) &= (u16)~w_at(&in, 4); NEXT(6); return YIELD;
    case 0x03E8: *ref(&in, 2) += val(&in, 4); NEXT(6); return YIELD;
    case 0x03E9: *ref(&in, 2) += w_at(&in, 4); NEXT(6); return YIELD;
    case 0x03EB: d = ref(&in, 2); *d = (u16)(*d * w_at(&in, 4)); NEXT(6); return YIELD;
    case 0x03F2: *ref(&in, 2) -= val(&in, 4); NEXT(6); return YIELD;
    case 0x03F3: *ref(&in, 2) -= w_at(&in, 4); NEXT(6); return YIELD;
    case 0x03F4: case 0x03F5: case 0x03F7: case 0x03F8: {
        u16 b = op == 0x03F4 || op == 0x03F7 ? val(&in, 4) : w_at(&in, 4);
        d = ref(&in, 2);
        if (b)
            *d = op <= 0x03F5 ? (u16)(*d / b) : (u16)(*d % b);
        NEXT(6);
        return YIELD;
    }
    case 0x044C: case 0x044D: {
        u16 a = val(&in, 4), b = op == 0x044C ? val(&in, 8) : w_at(&in, 8);
        *ref(&in, 2) = binop(w_at(&in, 6), a, b, op == 0x044D);
        NEXT(10);
        return RUN;
    }

    case 0x00FF: /* seg9:0134: the disc in the drive (9), or whether disc n is */
        *ref(&in, 4) = s->host.disc ? s->host.disc(s->host.user, val(&in, 2)) : 1;
        NEXT(6);
        return YIELD;

    case 0x000E: { /* pauses the other running sequences into a group, or
                    * resumes a group, 999 all (seg15:02AA) */
        u16 g = w_at(&in, 6);
        u16 me = val(&in, 2);
        if (w_at(&in, 4)) {
            for (i = 1; i < VS_SEQS; i++)
                if (i != me && vs_get(s, i, VC_USED) && !vs_get(s, i, VC_HALTED)) {
                    vs_set(s, i, VC_HALTED, 1);
                    if (g < XF_COUNT(s->paused))
                        s->paused[g] |= (u64)1 << i;
                }
        } else {
            for (i = 1; i < VS_SEQS; i++)
                if (vs_get(s, i, VC_USED) &&
                    (g == 999 || (g < XF_COUNT(s->paused) && s->paused[g] >> i & 1)))
                    vs_set(s, i, VC_HALTED, 0);
            if (g == 999)
                memset(s->paused, 0, sizeof(s->paused));
            else if (g < XF_COUNT(s->paused))
                s->paused[g] = 0;
        }
        NEXT(8);
        return YIELD;
    }
    case 0x0800: /* the score (seg8:2FB4): a song by three variables */
        if (s->host.score)
            s->host.score(s->host.user, val(&in, 2), val(&in, 4), val(&in, 6));
        NEXT(8);
        return YIELD;
    case 0x080E: { /* a sequence's sprite goes (seg8:7914) */
        u16 who = val(&in, 2);
        if (who < VS_SEQS) {
            vs_set(s, who, VC_SPRITE, 0xFFFF);
            vs_set(s, who, 0x122, 0xFFFF);
            vs_set(s, who, 0x124, 0xFFFF);
            vs_set(s, who, 0x120, 0);
        }
        NEXT(4);
        return RUN;
    }
    case 0x0822: /* shows a sprite (seg8:5F0A): the slot from a variable, x,
                  * y, z and the drawing mode as they are */
        vs_set(s, seq, VC_FLAGS, (u16)(vs_get(s, seq, VC_FLAGS) | 1));
        vs_set(s, seq, VC_SPRITE, *ref(&in, 2));
        vs_set(s, seq, VC_X, w_at(&in, 4));
        vs_set(s, seq, VC_Y, w_at(&in, 6));
        vs_set(s, seq, VC_Z, w_at(&in, 8));
        vs_set(s, seq, VC_MODE, w_at(&in, 10));
        NEXT(12);
        return RUN;
    case 0x080D: /* the sequence's slot into a variable (seg8:5F7C) */
        *ref(&in, 2) = vs_get(s, seq, VC_INDEX);
        NEXT(4);
        return RUN;
    case 0x0805: /* the "uber" device (seg13): its result into +2. Not written yet. */
        if (s->host.uber)
            *ref(&in, 2) = s->host.uber(s->host.user, seq, b_at(&in, 4), in.pc);
        else
            *ref(&in, 2) = 0;
        NEXT(34);
        return YIELD;

    /* screen updates: seg7:33B8 queues a command (the opcode, a rectangle
     * and three words) that seg7:35EA carries out after the tick: how the
     * new picture reaches the screen. The module redraws the whole
     * screen every frame, so these show at once for now. */
    case 0x012E: case 0x012F: case 0x0130: case 0x0132: case 0x0133:
    case 0x0134: case 0x0135: case 0x0136: case 0x0137: case 0x0138: {
        static const u8 len[] = { 16, 16, 12, 0, 2, 2, 2, 18, 12, 14, 18 };
        int x = 0, y = 0, w = 0, h = 0;
        if (op == 0x012E) { /* seg8:1512: where, 0x7FFE a rectangle, 0x7FFF
                             * the screen, else a sequence's sprite */
            u16 where = w_at(&in, 4);
            if (where == 0x7FFE) {
                x = (i16)val(&in, 6);
                y = (i16)val(&in, 8);
                w = (i16)val(&in, 0xA);
                h = (i16)val(&in, 0xC);
            } else if (where == 0x7FFF) {
                w = 640;
                h = 480;
            } else {
                u16 who = val(&in, 4);
                if (who < VS_SEQS) {
                    x = (i16)vs_get(s, who, VC_X);
                    y = (i16)vs_get(s, who, VC_Y);
                    w = (i16)vs_get(s, who, VC_W);
                    h = (i16)vs_get(s, who, VC_H);
                }
            }
        }
        if (s->host.screen)
            s->host.screen(s->host.user, seq, op, w_at(&in, 2), x, y, w, h);
        NEXT(len[op - 0x012E]);
        return YIELD;
    }

    case 0x0192: /* moves the sequence's clip (seg8:1E16); a flag */
        if (s->host.clip_move)
            s->host.clip_move(s->host.user, seq, (i16)val(&in, 2), (i16)val(&in, 4));
        NEXT(8);
        return YIELD;
    case 0x0194: /* the sequence's place becomes its clip's window (seg8:1B7E) */
        vs_set(s, seq, VC_X, vs_get(s, seq, VC_MOVIE_RECT));
        vs_set(s, seq, VC_Y, vs_get(s, seq, VC_MOVIE_RECT + 2));
        vs_set(s, seq, VC_W, (u16)(vs_get(s, seq, VC_MOVIE_RECT + 4) - vs_get(s, seq, VC_MOVIE_RECT)));
        vs_set(s, seq, VC_H, (u16)(vs_get(s, seq, VC_MOVIE_RECT + 6) - vs_get(s, seq, VC_MOVIE_RECT + 2)));
        NEXT(2);
        return RUN;
    case 0x0195: { /* waits until the clip reaches a time, 1/30 s (seg8:1BEA);
                    * then 8 goes on, 9 stops the clip too */
        long ms = s->host.clip_time ? s->host.clip_time(s->host.user, seq) : -1;
        u16 at;
        if (!vs_get(s, seq, VC_MOVIE) || ms < 0) {
            NEXT(5);
            return RUN;
        }
        if (!vs_get(s, seq, VC_MOVIE_AT))
            vs_set(s, seq, VC_MOVIE_AT, w_at(&in, 2));
        at = vs_get(s, seq, VC_MOVIE_AT);
        if ((long)at > ms * 30 / 1000)
            return YIELD;
        vs_set(s, seq, VC_MOVIE_AT, 0);
        if (b_at(&in, 4) == 9 && s->host.clip_op)
            s->host.clip_op(s->host.user, seq, 9, 0, 0);
        NEXT(5);
        return RUN;
    }
    case 0x0196: /* a clip's rate, from a variable (seg8:1D12): not done */
        NEXT(4);
        return YIELD;

    case 0x0197: { /* act on the clips of sequences (seg8:79C4): 9 stops and
                    * hides, 10 shows, 11 plays (an ended one goes) */
        u16 who = val(&in, 3);
        for (i = 0; i < VS_SEQS; i++)
            if ((who < 0x2D ? i == who : matches(s, i, who)) && s->host.clip_op)
                s->host.clip_op(s->host.user, i, b_at(&in, 2), w_at(&in, 5), w_at(&in, 7));
        NEXT(9);
        return YIELD;
    }

    case 0x04BA: { /* wait a number of milliseconds (seg8:2AB4) */
        if (!get32(s, seq, 0x20)) {
            set32(s, seq, 0x20, (u32)w_at(&in, 2) | (u32)w_at(&in, 4) << 16);
            s->sleep_start[seq] = s->now;
        }
        if (s->now - s->sleep_start[seq] < get32(s, seq, 0x20))
            return YIELD;
        set32(s, seq, 0x20, 0);
        NEXT(6);
        return RUN;
    }

    /* events (context + 0x2E, 0x32, 0x36, handlers at 0x70) and signals
     * between sequences (+ 0x3A, 0x3E, 0x42, handlers at 0xB8) */
    case 0x0065: case 0x00D9: { /* handlers: a mask, then where to go */
        u32 mask = (u32)w_at(&in, 2) | (u32)w_at(&in, 4) << 16;
        int on = op == 0x0065 ? VC_HANDLED : VC_HANDLED2, vec = op == 0x0065 ? VC_VECTORS : VC_VECTORS2;
        set32(s, seq, on, get32(s, seq, on) | mask);
        for (k = 0; k < 18; k++)
            if (mask & 1u << k)
                vs_set_ptr(s, seq, vec + 4 * k, far_at(&in, 6));
        NEXT(10);
        return YIELD;
    }
    case 0x00D6: case 0x00D8: { /* cancel waits and handlers in sequences;
                                 * a sequence waiting on them goes on */
        u16 who = val(&in, 2);
        u32 mask = (u32)w_at(&in, 4) | (u32)w_at(&in, 6) << 16;
        int wait = op == 0x00D6 ? VC_WAIT : VC_WAIT2, on = op == 0x00D6 ? VC_HANDLED : VC_HANDLED2;
        int vec = op == 0x00D6 ? VC_VECTORS : VC_VECTORS2;
        u16 waitop = op == 0x00D6 ? 0x00D5 : 0x00D7;
        for (i = 0; i < VS_SEQS; i++) {
            if (!(who < 0x2D ? i == who && vs_get(s, i, VC_USED) : matches(s, i, who)))
                continue;
            if (get32(s, i, wait) & mask && (vexe_u16(s->x, vs_get_ptr(s, i, VC_PC)) & 0x7FFF) == waitop)
                vs_set_ptr(s, i, VC_PC, vs_get_ptr(s, i, VC_PC) + 6);
            set32(s, i, wait, get32(s, i, wait) & ~mask);
            set32(s, i, on, get32(s, i, on) & ~mask);
            for (k = 0; k < 18; k++)
                if (mask & 1u << k)
                    vs_set_ptr(s, i, vec + 4 * k, 0);
        }
        NEXT(8);
        return YIELD;
    }
    case 0x00DA: { /* signal the sequences that wait for or handle these */
        u32 mask = (u32)w_at(&in, 2) | (u32)w_at(&in, 4) << 16;
        for (i = 0; i < VS_SEQS; i++)
            if (vs_get(s, i, VC_USED) && !vs_get(s, i, VC_HALTED) &&
                (get32(s, i, VC_WAIT2) | get32(s, i, VC_HANDLED2)) & mask)
                set32(s, i, VC_EVENTS2, get32(s, i, VC_EVENTS2) | mask);
        NEXT(6);
        return YIELD;
    }
    case 0x00D7: { /* wait for signals */
        u32 mask = (u32)w_at(&in, 2) | (u32)w_at(&in, 4) << 16, hit;
        if (!get32(s, seq, VC_WAIT2))
            set32(s, seq, VC_WAIT2, mask);
        hit = mask & s->signals;
        if (!hit)
            return YIELD;
        set32(s, seq, VC_EVENTS2, get32(s, seq, VC_EVENTS2) & ~hit);
        set32(s, seq, VC_WAIT2, get32(s, seq, VC_WAIT2) & ~hit);
        if (get32(s, seq, VC_WAIT2))
            return YIELD;
        NEXT(6);
        return RUN;
    }

    case 0x0008: { /* a string resource in a box (seg8:4312): x, y, w, h, red,
                    * green, blue, the font, the string's id */
        char text[256];
        int x = w_at(&in, 2) > 0x280 ? val(&in, 2) : w_at(&in, 2);
        int y = w_at(&in, 4) > 0x1E0 ? val(&in, 4) : w_at(&in, 4), bw = w_at(&in, 6), bh = w_at(&in, 8);
        if (x + bw > 0x280)
            bw = 0x280 - x;
        if (y + bh > 0x1E0)
            bh = 0x1E0 - y;
        vexe_string(s->x, w_at(&in, 0x12), text, sizeof(text));
        if (s->host.text)
            s->host.text(s->host.user, (u8)val(&in, 0x10), x, y, bw, bh,
                         (u32)(val(&in, 0xA) & 0xFF) << 16 | (u32)(val(&in, 0xC) & 0xFF) << 8 | (val(&in, 0xE) & 0xFF),
                         text);
        NEXT(20);
        return YIELD;
    }
    case 0x000A: { /* a text in the instruction (seg8:3E08): x, y, the text,
                    * then a word and red, green, blue */
        int x = w_at(&in, 2) > 0x280 ? val(&in, 2) : w_at(&in, 2);
        int y = w_at(&in, 4) > 0x1E0 ? val(&in, 4) : w_at(&in, 4);
        str = str_at(&in, 6, &n);
        if (s->host.text)
            s->host.text(s->host.user, 0, x, y, 0, 0,
                         (u32)(w_at(&in, 6 + (int)n + 2) & 0xFF) << 16 | (u32)(w_at(&in, 6 + (int)n + 4) & 0xFF) << 8 |
                             (w_at(&in, 6 + (int)n + 6) & 0xFF),
                         str);
        NEXT(6 + n + 8);
        return YIELD;
    }

    case 0x07E4: case 0x07E5: { /* a text as the sequence's sprite (seg8:4A96):
                                 * the slot, x, y, z, a box, the font, red,
                                 * green, blue, a flag, the text */
        u16 slot = val(&in, 2);
        int tw = 0, th = 0;
        u32 rgb = (u32)(val(&in, 0x10) & 0xFF) << 16 | (u32)(val(&in, 0x12) & 0xFF) << 8 | (val(&in, 0x14) & 0xFF);
        str = str_at(&in, 0x18, &n);
        vs_set(s, seq, VC_SPRITE, slot);
        vs_set(s, seq, VC_X, val(&in, 4));
        vs_set(s, seq, VC_Y, val(&in, 6));
        vs_set(s, seq, VC_Z, val(&in, 8));
        vs_set(s, seq, VC_FLAGS, (u16)(vs_get(s, seq, VC_FLAGS) | 1));
        if (slot < VS_SPRITES && s->host.text_sprite &&
            s->host.text_sprite(s->host.user, slot, (u8)val(&in, 0xE), rgb, str, &tw, &th) == 0) {
            vs_set(s, seq, VC_W, (u16)tw);
            vs_set(s, seq, VC_H, (u16)th);
        } else {
            vs_set(s, seq, VC_SPRITE, 0xFFFF);
            vs_set(s, seq, VC_FLAGS, (u16)(vs_get(s, seq, VC_FLAGS) & ~1));
        }
        NEXT(0x18 + n);
        return YIELD;
    }

    /* flow */
    case 0x00C8: /* go to */
        GOTO(far_at(&in, 2));
        return YIELD;
    case 0x00D0: /* count down; while not 0, go to */
        d = ref(&in, 2);
        if (--*d) {
            GOTO(far_at(&in, 4));
            return RUN;
        }
        NEXT(8);
        return RUN;
    case 0x00D2: /* call */
        push_return(s, seq, in.pc + 6);
        GOTO(far_at(&in, 2));
        return RUN;
    case 0x00D3: { /* return */
        vx_ptr r = pop_return(s, seq);
        if (r)
            GOTO(r);
        return RUN;
    }
    case 0x00DC: case 0x00DD: { /* if a variable is (DC) or is not (DD) 0, go to */
        u16 a = val(&in, 2);
        if ((op == 0x00DC) == (a == 0))
            GOTO(far_at(&in, 4));
        else
            NEXT(8);
        return RUN;
    }
    case 0x00DE: case 0x00DF: case 0x00E0: case 0x00E1:
    case 0x00E2: case 0x00E3: case 0x00E4: case 0x00E5: {
        /* compare a variable with a value (DE E0 E3 E5) or a variable (DF E1
         * E2 E4): equal, not equal, greater, less; true goes to the pointer */
        int lit = op == 0x00DE || op == 0x00E0 || op == 0x00E3 || op == 0x00E5, t;
        u16 a = val(&in, 2), b = lit ? w_at(&in, 4) : val(&in, 4);
        t = op <= 0x00DF ? a == b : op <= 0x00E1 ? a != b : op <= 0x00E3 ? a > b : a < b;
        if (t)
            GOTO(far_at(&in, 6));
        else
            NEXT(10);
        return RUN;
    }
    case 0x00E6: { /* switch over the case records that follow */
        u16 v = val(&in, 2);
        vx_ptr p = in.pc + 4;
        for (k = 0; k < 256; k++) {
            u16 c = vexe_u16(s->x, p) & 0x7FFF;
            if (c == 0x00E7) {
                if (vexe_u16(s->x, p + 2) <= v && v <= vexe_u16(s->x, p + 4)) {
                    GOTO(vexe_far(s->x, p + 6));
                    return RUN;
                }
                p += 10;
            } else if (c == 0x00E8) {
                GOTO(vexe_far(s->x, p + 2));
                return RUN;
            } else {
                GOTO(p + 2); /* 0xE9: the end, nothing matched */
                return RUN;
            }
        }
        return STOP;
    }
    case 0x0100: /* the targets of 0x101 */
        for (k = 0; k < 13; k++)
            s->labels[k] = far_at(&in, 2 + 4 * k);
        NEXT(54);
        return YIELD;
    case 0x0101: { /* go to a target of 0x100 */
        u16 i2 = val(&in, 2);
        if (i2 < 13 && s->labels[i2])
            GOTO(s->labels[i2]);
        return YIELD;
    }
    case 0x00D5: { /* wait for events (a mask of DS:4ED6's bits) */
        u32 mask = (u32)w_at(&in, 2) | (u32)w_at(&in, 4) << 16, hit;
        if (!get32(s, seq, VC_WAIT))
            set32(s, seq, VC_WAIT, mask);
        if (mask & 1 && !vs_get(s, seq, VC_MOVIE) && !vs_get(s, seq, VC_MOVIE + 2))
            s->events |= 1; /* no clip: as if it had ended */
        hit = mask & s->events;
        if (!hit)
            return YIELD;
        set32(s, seq, VC_EVENTS, get32(s, seq, VC_EVENTS) & ~hit);
        set32(s, seq, VC_WAIT, get32(s, seq, VC_WAIT) & ~hit);
        if (get32(s, seq, VC_WAIT))
            return YIELD;
        NEXT(6);
        return RUN;
    }

    /* the sequence's sprite */
    case 0x07DA: case 0x07DC: /* set the sprite, move by (x, y, z), and 0x08 */
        vs_set(s, seq, VC_SPRITE, op == 0x07DA ? val(&in, 2) : val(&in, 2));
        vs_set(s, seq, VC_X, (u16)(vs_get(s, seq, VC_X) + w_at(&in, 4)));
        vs_set(s, seq, VC_Y, (u16)(vs_get(s, seq, VC_Y) + w_at(&in, 6)));
        vs_set(s, seq, VC_Z, (u16)(vs_get(s, seq, VC_Z) + w_at(&in, 8)));
        vs_set(s, seq, 0x08, (u16)(vs_get(s, seq, 0x08) + w_at(&in, 10)));
        NEXT(12);
        return RUN;
    case 0x07DB: /* move by (x, y, z) */
        vs_set(s, seq, VC_X, (u16)(vs_get(s, seq, VC_X) + w_at(&in, 2)));
        vs_set(s, seq, VC_Y, (u16)(vs_get(s, seq, VC_Y) + w_at(&in, 4)));
        vs_set(s, seq, VC_Z, (u16)(vs_get(s, seq, VC_Z) + w_at(&in, 6)));
        NEXT(10);
        return RUN;
    case 0x07D1:
        vs_set(s, seq, 0x120, val(&in, 2));
        vs_set(s, seq, 0x124, val(&in, 4));
        vs_set(s, seq, 0x122, val(&in, 6));
        NEXT(8);
        return RUN;
    case 0x07D2: {
        u16 who = val(&in, 2);
        if (who < VS_SEQS)
            vs_set(s, who, 0x120, w_at(&in, 4));
        NEXT(6);
        return YIELD;
    }
    case 0x02BC: { /* the cursor over a sequence's sprite (0x0C: the default) */
        u16 c = w_at(&in, 2);
        int who = seq;
        if (b_at(&in, 4) != 0x0C) {
            u16 v = operand(s, seq, (u16)(b_at(&in, 5) | b_at(&in, 6) << 8));
            if (v < VS_SEQS)
                who = v;
        }
        if (b_at(&in, 4) == 0x0C)
            s->g[0xFF] = c; /* kept apart: DS:0408 */
        else
            vs_set(s, who, 0x2C, c);
        NEXT(7);
        return YIELD;
    }

    /* media, through the module */
    case 0x0190: {
        int sub = b_at(&in, 2);
        if (sub == 1 || sub == 6 || sub == 7 || sub == 8) {
            u16 f[4];
            str = str_at(&in, 3, &n);
            for (k = 0; k < 4; k++)
                f[k] = w_at(&in, 3 + (int)n + 4 + 2 * k);
            if (s->host.clip)
                s->host.clip(s->host.user, seq, sub, str, val(&in, 3 + (int)n), val(&in, 3 + (int)n + 2), f,
                             b_at(&in, 3 + (int)n + 12), b_at(&in, 3 + (int)n + 13));
            NEXT(3 + n + 14);
            return RUN;
        }
        if (s->host.clip)
            s->host.clip(s->host.user, seq, sub, NULL, 0, 0, NULL, 0, 0);
        NEXT(3);
        return YIELD;
    }
    case 0x012C: {
        int sub = b_at(&in, 2);
        if (sub == 1 || sub == 0x12 || sub == 4) {
            str = str_at(&in, 3, &n);
            if (s->host.picture)
                s->host.picture(s->host.user, seq, sub, str, sub == 4 ? 0 : val(&in, 3 + (int)n),
                                sub == 4 ? 0 : val(&in, 3 + (int)n + 2));
            NEXT(3 + n + (sub == 4 ? 0 : 4));
            return YIELD;
        }
        if (s->host.picture)
            s->host.picture(s->host.user, seq, sub, NULL, 0, 0);
        NEXT(sub == 2 || sub == 0x13 ? 9 : 3);
        return YIELD;
    }
    case 0x01F5: {
        int sub = b_at(&in, 4);
        if (sub == 1) {
            int h;
            str = str_at(&in, 5, &n);
            h = s->host.sound ? s->host.sound(s->host.user, sub, str, 0) : 0;
            *ref(&in, 2) = (u16)h;
            NEXT(5 + n);
        } else {
            NEXT(7);
        }
        return YIELD;
    }
    case 0x01F6: case 0x01F7: case 0x01F8: {
        u16 args[4];
        int len = op == 0x01F6 ? 10 : op == 0x01F7 ? 4 : 3;
        args[0] = b_at(&in, 4);
        args[1] = b_at(&in, 5);
        args[2] = w_at(&in, 6);
        args[3] = w_at(&in, 8);
        if (s->host.sound_op)
            s->host.sound_op(s->host.user, op, op == 0x01F8 ? b_at(&in, 2) : val(&in, 2), args);
        if (op == 0x01F7)
            *ref(&in, 2) = 0; /* seg8:3DB3: the handle is gone */
        NEXT(len);
        return YIELD;
    }
    case 0x0352: {
        int sub = b_at(&in, 4);
        str = sub == 1 ? str_at(&in, 5, &n) : (n = 0, "");
        if (s->host.music)
            s->host.music(s->host.user, sub, sub == 1 ? str : NULL, w_at(&in, 2));
        NEXT(5 + n);
        return YIELD;
    }
    case 0x07D0: /* loads bitmaps into the sprite slots (seg8:49E8, seg2:0324):
                  * how many into the variable */
        str = str_at(&in, 7, &n);
        if (b_at(&in, 6) == 1)
            *ref(&in, 2) = (u16)(s->host.sprite_load ? s->host.sprite_load(s->host.user, val(&in, 4), str) : 0);
        NEXT(7 + n);
        return YIELD;
    }
    /* not known yet: like the default case, the sequence stays there */
    if (s->host.unknown)
        s->host.unknown(s->host.user, seq, op, in.pc);
    s->unknown_count++;
    return YIELD;
#undef NEXT
#undef GOTO
}

void vs_event(vs_state *s, int seq, u32 bits)
{
    if (seq < 0) {
        s->events |= bits;
        return;
    }
    set32(s, seq, VC_EVENTS, get32(s, seq, VC_EVENTS) | bits);
}

/* seg8:0110: a sequence's turn. Its pending events become DS:4ED6;
 * handlers registered for them are called; then its instructions run. */
static void run_seq(vs_state *s, int seq)
{
    int bit, r = RUN;
    u32 pending;
    if (!vs_get(s, seq, VC_USED) || vs_get(s, seq, VC_HALTED))
        return;
    pending = get32(s, seq, VC_EVENTS);
    s->events = pending | (s->events & 0xFFFF0000u);
    s->signals = get32(s, seq, VC_EVENTS2);
    /* seg8:04F9: the sound channels: 0x20 and 0x40 while channel 1 or 2
     * is quiet, 0x80 and 0x100 while it plays */
    if (s->host.sound_busy) {
        s->events |= s->host.sound_busy(s->host.user, 1) ? 0x80 : 0x20;
        s->events |= s->host.sound_busy(s->host.user, 2) ? 0x100 : 0x40;
    }
    for (bit = 0; bit < 18; bit++)
        if (get32(s, seq, VC_HANDLED) & pending & 1u << bit) {
            set32(s, seq, VC_EVENTS, get32(s, seq, VC_EVENTS) & ~(1u << bit));
            set32(s, seq, VC_HANDLED, get32(s, seq, VC_HANDLED) & ~(1u << bit));
            push_return(s, seq, vs_get_ptr(s, seq, VC_PC));
            vs_set_ptr(s, seq, VC_PC, vs_get_ptr(s, seq, VC_VECTORS + 4 * bit));
        }
    pending = get32(s, seq, VC_EVENTS2);
    for (bit = 0; bit < 18; bit++)
        if (get32(s, seq, VC_HANDLED2) & pending & 1u << bit) {
            set32(s, seq, VC_EVENTS2, get32(s, seq, VC_EVENTS2) & ~(1u << bit));
            set32(s, seq, VC_HANDLED2, get32(s, seq, VC_HANDLED2) & ~(1u << bit));
            push_return(s, seq, vs_get_ptr(s, seq, VC_PC));
            vs_set_ptr(s, seq, VC_PC, vs_get_ptr(s, seq, VC_VECTORS2 + 4 * bit));
        }
    for (s->depth = 0; s->depth < MAX_DEPTH && r == RUN; s->depth++)
        r = step(s, seq);
}

void vs_tick(vs_state *s)
{
    int i;
    s->ticks++;
    for (i = 0; i < VS_SEQS; i++)
        run_seq(s, i);
    s->events = 0;
}

void vs_click(vs_state *s, int x, int y, int right)
{
    int i, best = -1;
    i16 best_z = 0x7FFF;
    for (i = 0; i < VS_SEQS; i++) {
        i16 sx, sy, sw, sh;
        if (!vs_get(s, i, VC_USED) || vs_get(s, i, VC_HALTED))
            continue;
        xf_debug("click: seq %d at seg%d:%04X rect %d,%d %dx%d z %d wait %08X handled %08X", i,
                 VX_SEG(vs_get_ptr(s, i, VC_PC)), VX_OFF(vs_get_ptr(s, i, VC_PC)), (i16)vs_get(s, i, VC_X),
                 (i16)vs_get(s, i, VC_Y), (i16)vs_get(s, i, VC_W), (i16)vs_get(s, i, VC_H), (i16)vs_get(s, i, VC_Z),
                 get32(s, i, VC_WAIT), get32(s, i, VC_HANDLED));
        set32(s, i, VC_EVENTS, get32(s, i, VC_EVENTS) | 0x2);
        if (!((get32(s, i, VC_WAIT) | get32(s, i, VC_HANDLED)) & 0x1C))
            continue;
        sx = (i16)vs_get(s, i, VC_X);
        sy = (i16)vs_get(s, i, VC_Y);
        sw = (i16)vs_get(s, i, VC_W);
        sh = (i16)vs_get(s, i, VC_H);
        if (x < sx || x > sx + sw || y < sy || y > sy + sh || (i16)vs_get(s, i, VC_Z) >= best_z)
            continue;
        best = i;
        best_z = (i16)vs_get(s, i, VC_Z);
    }
    if (best >= 0)
        set32(s, best, VC_EVENTS, get32(s, best, VC_EVENTS) | (right ? 0x14u : 0x0Cu));
}

void vs_key(vs_state *s, int key)
{
    int i;
    for (i = 0; i < VS_SEQS; i++) {
        if (!vs_get(s, i, VC_USED) || vs_get(s, i, VC_HALTED))
            continue;
        if (key == 27 && get32(s, i, VC_HANDLED) & 0x8000)
            set32(s, i, VC_EVENTS, get32(s, i, VC_EVENTS) | 0x8000);
        set32(s, i, VC_EVENTS, get32(s, i, VC_EVENTS) | 0x400);
    }
}
