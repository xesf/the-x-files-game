/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* DOS backend (Open Watcom + DOS/4GW):
 *
 *   Video     VESA 2.0 640x480 with a linear framebuffer, at 32, 16 or 15
 *             bits per pixel, mapped through DPMI. The frame is converted
 *             on each present.
 *   Input     INT 33h mouse, polled; BIOS keyboard (INT 16h).
 *   Timing    PIT channel 0 at 1 kHz with an INT 08 millisecond counter,
 *             chaining to the BIOS at 18.2 Hz.
 *   Sound     Sound Blaster from the BLASTER variable: 8-bit mono, auto-init
 *             DMA over two halves; the IRQ mixes the next half with the
 *             engine's mixer.
 *
 * Adapted from the DOS backend of the Ecstatica engine (same author,
 * GPL-3.0-or-later). */
#include <conio.h>
#include <ctype.h>
#include <dos.h>
#include <direct.h>
#include <i86.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/plat.h"

static int g_width, g_height;
static int g_headless;

/* --- DPMI helpers ----------------------------------------------------- */

#pragma pack(push, 1)
typedef struct {
    u32 edi, esi, ebp, reserved, ebx, edx, ecx, eax;
    u16 flags, es, ds, fs, gs, ip, cs, sp, ss;
} rminfo;
#pragma pack(pop)

/* Real-mode interrupt through DPMI 0300h: BIOS calls cannot take
 * protected-mode selectors. */
static int rm_int(int intno, rminfo *rmi)
{
    union REGS r;
    struct SREGS sr;
    segread(&sr);
    memset(&r, 0, sizeof(r));
    r.w.ax = 0x0300;
    r.h.bl = (unsigned char)intno;
    r.x.edi = (u32)rmi;
    sr.es = sr.ds;
    int386x(0x31, &r, &r, &sr);
    return !r.w.cflag;
}

/* Conventional memory (DPMI 0100h), for BIOS buffers and sound DMA. */
static void *dos_alloc_real(int paras, u16 *sel, u16 *seg)
{
    union REGS r;
    memset(&r, 0, sizeof(r));
    r.w.ax = 0x0100;
    r.w.bx = (u16)paras;
    int386(0x31, &r, &r);
    if (r.w.cflag)
        return NULL;
    *seg = r.w.ax;
    *sel = r.w.dx;
    return (void *)((u32)r.w.ax << 4);
}

static void dos_free_real(u16 sel)
{
    union REGS r;
    memset(&r, 0, sizeof(r));
    r.w.ax = 0x0101;
    r.w.dx = sel;
    int386(0x31, &r, &r);
}

/* Maps physical memory (the framebuffer) into the flat address space. */
static u8 *dpmi_map_physical(u32 phys, u32 len)
{
    union REGS r;
    memset(&r, 0, sizeof(r));
    r.w.ax = 0x0800;
    r.w.bx = (u16)(phys >> 16);
    r.w.cx = (u16)(phys & 0xFFFF);
    r.w.si = (u16)(len >> 16);
    r.w.di = (u16)(len & 0xFFFF);
    int386(0x31, &r, &r);
    if (r.w.cflag)
        return NULL;
    return (u8 *)(((u32)r.w.bx << 16) | r.w.cx);
}

/* --- Video ------------------------------------------------------------ */

#pragma pack(push, 1)
typedef struct {
    u16 attributes;
    u8 win_a, win_b;
    u16 granularity, win_size, seg_a, seg_b;
    u32 win_func;
    u16 pitch, width, height;
    u8 w_char, y_char, planes, bpp, banks;
    u8 memory_model, bank_size, image_pages, reserved0;
    u8 red_mask, red_pos, green_mask, green_pos, blue_mask, blue_pos, rsv_mask, rsv_pos, dcm_info;
    u32 phys_base;
    u32 reserved1;
    u16 reserved2;
} vbe_mode_info;
#pragma pack(pop)

static u8 *g_lfb;
static int g_bpp, g_pitch, g_green_bits;

static void set_bios_mode(int mode)
{
    rminfo rmi;
    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = (u32)mode;
    rm_int(0x10, &rmi);
}

/* Finds a w x h mode with a linear framebuffer, preferring 32 bits, then
 * 16, then 15. Returns the mode number, or 0. */
static int vesa_find_mode(int w, int h, u32 *phys)
{
    static const int prefs[] = { 32, 16, 15 };
    u16 modes[256], sel, seg;
    rminfo rmi;
    u8 *buf = dos_alloc_real(512 / 16 + 1, &sel, &seg);
    int count = 0, found = 0, p, i;
    if (!buf)
        return 0;
    memset(buf, 0, 512);
    memcpy(buf, "VBE2", 4);
    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 0x4F00;
    rmi.es = seg;
    if (!rm_int(0x10, &rmi) || (rmi.eax & 0xFFFF) != 0x004F || memcmp(buf, "VESA", 4) != 0) {
        dos_free_real(sel);
        return 0;
    }
    {
        /* The list is in the card's memory; 4F01h reuses our buffer. */
        u32 mp = *(u32 *)(buf + 14);
        u16 *list = (u16 *)((((mp >> 16) & 0xFFFF) << 4) + (mp & 0xFFFF));
        while (count < 256 && list[count] != 0xFFFF) {
            modes[count] = list[count];
            count++;
        }
    }
    for (p = 0; p < 3 && !found; p++)
        for (i = 0; i < count && !found; i++) {
            vbe_mode_info *mi = (vbe_mode_info *)buf;
            int bpp;
            memset(buf, 0, 256);
            memset(&rmi, 0, sizeof(rmi));
            rmi.eax = 0x4F01;
            rmi.ecx = modes[i];
            rmi.es = seg;
            if (!rm_int(0x10, &rmi) || (rmi.eax & 0xFFFF) != 0x004F)
                continue;
            if (!(mi->attributes & 0x01) || !(mi->attributes & 0x80) || mi->width != w || mi->height != h)
                continue;
            bpp = mi->bpp == 16 && mi->green_mask == 5 ? 15 : mi->bpp;
            if (bpp != prefs[p])
                continue;
            *phys = mi->phys_base;
            g_bpp = bpp;
            g_green_bits = mi->green_mask;
            g_pitch = mi->pitch;
            found = modes[i];
        }
    dos_free_real(sel);
    return found;
}

static int vesa_set_mode(int mode)
{
    rminfo rmi;
    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 0x4F02;
    rmi.ebx = (u32)(mode | 0x4000); /* linear framebuffer */
    return rm_int(0x10, &rmi) && (rmi.eax & 0xFFFF) == 0x004F;
}

/* --- Timer ------------------------------------------------------------ */

#define PIT_HZ 1193182UL
#define PIT_DIVISOR ((u16)(PIT_HZ / 1000UL))

static volatile u32 g_ms;
static volatile u32 g_bios_accum;
static void(__interrupt __far *g_old_int8)(void);

static void __interrupt __far timer_isr(void)
{
    g_ms++;
    g_bios_accum += PIT_DIVISOR;
    if (g_bios_accum >= 0x10000UL) {
        g_bios_accum -= 0x10000UL;
        _chain_intr(g_old_int8); /* the BIOS sends the EOI */
    }
    outp(0x20, 0x20);
}

static void pit_set_divisor(u16 div)
{
    _disable();
    outp(0x43, 0x36);
    outp(0x40, (int)(div & 0xFF));
    outp(0x40, (int)(div >> 8));
    _enable();
}

/* --- Mouse ------------------------------------------------------------ */

static int g_mouse_ok, g_mouse_x, g_mouse_y, g_mouse_buttons;

static void mouse_init(void)
{
    union REGS r;
    memset(&r, 0, sizeof(r));
    r.w.ax = 0x0000;
    int386(0x33, &r, &r);
    g_mouse_ok = r.w.ax == 0xFFFF;
    if (!g_mouse_ok)
        return;
    memset(&r, 0, sizeof(r));
    r.w.ax = 0x0007; /* horizontal range */
    r.w.dx = (u16)(g_width - 1);
    int386(0x33, &r, &r);
    memset(&r, 0, sizeof(r));
    r.w.ax = 0x0008; /* vertical range */
    r.w.dx = (u16)(g_height - 1);
    int386(0x33, &r, &r);
}

/* --- Platform API ------------------------------------------------------ */

int plat_init(const char *title, int width, int height)
{
    u32 phys = 0;
    int mode;
    (void)title;
    g_width = width;
    g_height = height;
    g_old_int8 = _dos_getvect(8);
    _dos_setvect(8, timer_isr);
    pit_set_divisor(PIT_DIVISOR);
    g_headless = getenv("XF_HEADLESS") != NULL;
    if (g_headless)
        return 0;
    mode = vesa_find_mode(width, height, &phys);
    if (!mode || !vesa_set_mode(mode)) {
        fprintf(stderr, "No VESA 2.0 %dx%d mode with 15, 16 or 32 bits per pixel.\n", width, height);
        return -1;
    }
    g_lfb = dpmi_map_physical(phys, (u32)g_pitch * (u32)height);
    if (!g_lfb) {
        set_bios_mode(0x03);
        return -1;
    }
    mouse_init();
    return 0;
}

void plat_shutdown(void)
{
    plat_audio_close();
    if (g_lfb)
        set_bios_mode(0x03);
    g_lfb = NULL;
    if (g_old_int8) {
        pit_set_divisor(0);
        _dos_setvect(8, g_old_int8);
        g_old_int8 = NULL;
    }
}

static int bios_mods(void)
{
    int f = *(volatile u8 *)0x417;
    return (f & 0x03 ? PLAT_MOD_SHIFT : 0) | (f & 0x04 ? PLAT_MOD_CTRL : 0) | (f & 0x08 ? PLAT_MOD_ALT : 0);
}

int plat_poll(plat_event *ev)
{
    union REGS r;
    memset(ev, 0, sizeof(*ev));
    if (g_headless)
        return 0;
    if (g_mouse_ok) {
        int changed;
        memset(&r, 0, sizeof(r));
        r.w.ax = 0x0003;
        int386(0x33, &r, &r);
        ev->x = r.w.cx;
        ev->y = r.w.dx;
        changed = (r.w.bx ^ g_mouse_buttons) & 3;
        if (changed) {
            int b = changed & 1 ? 1 : 2;
            ev->type = r.w.bx & b ? PLAT_EV_MOUSE_DOWN : PLAT_EV_MOUSE_UP;
            ev->button = b == 1 ? PLAT_MOUSE_LEFT : PLAT_MOUSE_RIGHT;
            g_mouse_buttons ^= b;
            return 1;
        }
        if (ev->x != g_mouse_x || ev->y != g_mouse_y) {
            g_mouse_x = ev->x;
            g_mouse_y = ev->y;
            ev->type = PLAT_EV_MOUSE_MOVE;
            return 1;
        }
    }
    if (kbhit()) {
        int c = getch();
        ev->type = PLAT_EV_KEY_DOWN;
        ev->mods = bios_mods();
        if (c == 0 || c == 0xE0) {
            switch (getch()) { /* extended scan code */
            case 0x4B: ev->key = PLAT_KEY_LEFT; break;
            case 0x4D: ev->key = PLAT_KEY_RIGHT; break;
            case 0x48: ev->key = PLAT_KEY_UP; break;
            case 0x50: ev->key = PLAT_KEY_DOWN; break;
            case 0x3B: ev->key = PLAT_KEY_F1; break;
            default: return 0;
            }
        } else if (c == 27) {
            ev->key = PLAT_KEY_ESCAPE;
        } else if (c == 13) {
            ev->key = PLAT_KEY_RETURN;
        } else if (c >= 1 && c <= 26 && c != 8) {
            ev->key = 'a' + c - 1; /* Ctrl+letter */
        } else {
            ev->key = c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
        }
        return 1;
    }
    return 0;
}

/* The VESA mode is the game's own 640x480. */
void plat_output_size(int *width, int *height)
{
    *width = g_width;
    *height = g_height;
}

void plat_present(const u32 *pixels, int width, int height, int pitch)
{
    int x, y;
    if (!g_lfb)
        return;
    for (y = 0; y < height; y++) {
        const u32 *src = pixels + (size_t)y * (size_t)pitch;
        u8 *row = g_lfb + (size_t)y * (size_t)g_pitch;
        if (g_bpp == 32) {
            memcpy(row, src, (size_t)width * 4);
        } else if (g_bpp == 16) {
            u16 *dst = (u16 *)row;
            for (x = 0; x < width; x++) {
                u32 c = src[x];
                dst[x] = (u16)((c >> 8 & 0xF800) | (c >> 5 & 0x07E0) | (c >> 3 & 0x001F));
            }
        } else {
            u16 *dst = (u16 *)row;
            for (x = 0; x < width; x++) {
                u32 c = src[x];
                dst[x] = (u16)((c >> 9 & 0x7C00) | (c >> 6 & 0x03E0) | (c >> 3 & 0x001F));
            }
        }
    }
}

u32 plat_ticks_ms(void) { return g_ms; }

void plat_sleep_ms(u32 ms)
{
    u32 end = g_ms + ms;
    while ((i32)(g_ms - end) < 0)
        ;
}

/* --- Sound Blaster ------------------------------------------------------- */

#define SB_HALF 1024 /* frames (bytes) per half buffer */
#define SB_BUFSZ (SB_HALF * 2)

static u16 g_sb_base, g_dma_sel;
static int g_sb_irq = -1, g_sb_dma = -1, g_sb_ready;
static volatile u8 *g_dma_buf;
static volatile int g_dma_half, g_in_mix;
static void(__interrupt __far *g_old_irq)(void);
static plat_audio_fn g_audio_fn;
static void *g_audio_user;
static i16 g_mix[SB_HALF * 2];

static void sb_write(u8 v)
{
    int guard = 0xFFFF;
    while ((inp(g_sb_base + 0x0C) & 0x80) && --guard)
        ;
    outp(g_sb_base + 0x0C, v);
}

static int sb_read(void)
{
    int guard;
    for (guard = 0; guard < 0x10000; guard++)
        if (inp(g_sb_base + 0x0E) & 0x80)
            return inp(g_sb_base + 0x0A) & 0xFF;
    return -1;
}

static int sb_reset(void)
{
    int guard;
    volatile int d;
    outp(g_sb_base + 0x06, 1);
    for (d = 0; d < 1000; d++)
        (void)inp(g_sb_base + 0x06);
    outp(g_sb_base + 0x06, 0);
    for (guard = 0; guard < 0x10000; guard++)
        if ((inp(g_sb_base + 0x0E) & 0x80) && (inp(g_sb_base + 0x0A) & 0xFF) == 0xAA)
            return 1;
    return 0;
}

/* BLASTER=A220 I7 D1: address (hex), IRQ and 8-bit DMA. */
static int sb_parse_blaster(void)
{
    const char *e = getenv("BLASTER");
    if (!e)
        return 0;
    while (*e) {
        char k = (char)toupper((unsigned char)*e);
        if (k == 'A' || k == 'I' || k == 'D') {
            int v = 0, base = k == 'A' ? 16 : 10;
            for (e++; *e && *e != ' '; e++) {
                int c = toupper((unsigned char)*e);
                int dgt = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                if (dgt < 0 || dgt >= base)
                    break;
                v = v * base + dgt;
            }
            if (k == 'A')
                g_sb_base = (u16)v;
            else if (k == 'I')
                g_sb_irq = v;
            else
                g_sb_dma = v;
        } else {
            while (*e && *e != ' ')
                e++;
        }
        while (*e == ' ')
            e++;
    }
    return g_sb_base && g_sb_irq >= 0 && g_sb_dma >= 0 && g_sb_dma < 4;
}

/* The engine's mixer renders 16-bit stereo; the card plays 8-bit mono. */
static void sb_mix(volatile u8 *dst)
{
    int i;
    g_audio_fn(g_audio_user, g_mix, SB_HALF);
    for (i = 0; i < SB_HALF; i++)
        dst[i] = (u8)(((g_mix[i * 2] + g_mix[i * 2 + 1]) >> 9) + 128);
}

static void __interrupt __far sb_isr(void)
{
    (void)inp(g_sb_base + 0x0E); /* acknowledge 8-bit DMA */
    if (g_sb_irq >= 8)
        outp(0xA0, 0x20);
    outp(0x20, 0x20);
    if (g_in_mix)
        return;
    g_in_mix = 1;
    _enable();
    sb_mix(g_dma_buf + (g_dma_half ? SB_HALF : 0));
    g_dma_half ^= 1;
    _disable();
    g_in_mix = 0;
}

static void dma_setup(u32 phys, int len, int ch)
{
    static const u8 page_port[4] = { 0x87, 0x83, 0x81, 0x82 };
    _disable();
    outp(0x0A, 0x04 | ch);
    outp(0x0C, 0x00);
    outp(0x0B, 0x58 | ch); /* single, auto-init, memory to device */
    outp(ch * 2, (int)(phys & 0xFF));
    outp(ch * 2, (int)((phys >> 8) & 0xFF));
    outp(page_port[ch], (int)((phys >> 16) & 0xFF));
    outp(ch * 2 + 1, (int)((len - 1) & 0xFF));
    outp(ch * 2 + 1, (int)(((len - 1) >> 8) & 0xFF));
    outp(0x0A, ch);
    _enable();
}

int plat_audio_open(int rate, plat_audio_fn fn, void *user)
{
    u16 seg;
    u32 phys;
    int vec, major, i;
    u8 *raw;
    if (g_headless || !sb_parse_blaster() || !sb_reset())
        return -1;
    /* Under 1 MB and within one 64 KB page. */
    raw = dos_alloc_real((SB_BUFSZ * 2) / 16 + 1, &g_dma_sel, &seg);
    if (!raw)
        return -1;
    phys = (u32)seg << 4;
    if ((phys & 0xFFFF0000UL) != ((phys + SB_BUFSZ - 1) & 0xFFFF0000UL))
        phys = (phys + 0xFFFFUL) & 0xFFFF0000UL;
    g_dma_buf = (volatile u8 *)phys;
    for (i = 0; i < SB_BUFSZ; i++)
        g_dma_buf[i] = 128;
    g_audio_fn = fn;
    g_audio_user = user;
    vec = g_sb_irq < 8 ? 0x08 + g_sb_irq : 0x70 + g_sb_irq - 8;
    g_old_irq = _dos_getvect(vec);
    _dos_setvect(vec, sb_isr);
    if (g_sb_irq < 8) {
        outp(0x21, inp(0x21) & ~(1 << g_sb_irq));
    } else {
        outp(0xA1, inp(0xA1) & ~(1 << (g_sb_irq - 8)));
        outp(0x21, inp(0x21) & ~(1 << 2));
    }
    dma_setup(phys, SB_BUFSZ, g_sb_dma);
    sb_write(0xD1); /* speaker on */
    sb_write(0xE1);
    major = sb_read();
    (void)sb_read();
    if (major >= 4) {
        sb_write(0x41); /* SB16: exact output rate */
        sb_write((u8)(rate >> 8));
        sb_write((u8)(rate & 0xFF));
        sb_write(0xC6); /* 8-bit auto-init */
        sb_write(0x00); /* mono, unsigned */
        sb_write((u8)((SB_HALF - 1) & 0xFF));
        sb_write((u8)((SB_HALF - 1) >> 8));
    } else {
        sb_write(0x40); /* time constant */
        sb_write((u8)(256 - 1000000L / rate));
        sb_write(0x48);
        sb_write((u8)((SB_HALF - 1) & 0xFF));
        sb_write((u8)((SB_HALF - 1) >> 8));
        sb_write(0x1C);
    }
    g_sb_ready = 1;
    return 0;
}

void plat_audio_close(void)
{
    int vec;
    if (!g_sb_ready)
        return;
    g_sb_ready = 0;
    sb_write(0xDA);
    sb_write(0xD3);
    sb_reset();
    outp(0x0A, 0x04 | g_sb_dma);
    if (g_sb_irq < 8)
        outp(0x21, inp(0x21) | (1 << g_sb_irq));
    else
        outp(0xA1, inp(0xA1) | (1 << (g_sb_irq - 8)));
    vec = g_sb_irq < 8 ? 0x08 + g_sb_irq : 0x70 + g_sb_irq - 8;
    if (g_old_irq)
        _dos_setvect(vec, g_old_irq);
    g_old_irq = NULL;
    if (g_dma_sel)
        dos_free_real(g_dma_sel);
    g_dma_sel = 0;
}

/* The mixer runs in the IRQ: the engine locks it out while it changes the
 * mixer's state. */
void plat_audio_lock(void) { _disable(); }
void plat_audio_unlock(void) { _enable(); }

/* --- Fonts and cursor ----------------------------------------------------- */

/* A Windows install on the same machine has the fonts the game expects. */
int plat_font_path(const char *family, char *out, size_t size)
{
    static const struct { const char *family, *file; } files[] = {
        { "Courier New", "COUR.TTF" }, { "Arial", "ARIAL.TTF" }, { "Times New Roman", "TIMES.TTF" } };
    size_t i;
    for (i = 0; i < sizeof(files) / sizeof(files[0]); i++)
        if (strcmp(family, files[i].family) == 0) {
            FILE *f;
            snprintf(out, size, "C:\\WINDOWS\\FONTS\\%s", files[i].file);
            f = fopen(out, "rb");
            if (f) {
                fclose(f);
                return 0;
            }
        }
    return -1;
}

/* The game draws its own cursor; the mouse driver's stays hidden. */
/* The mouse is the user's: navigation does not move it. */
int plat_text_input(const char *title, const char *initial, int password, int clear)
{
    (void)title;
    (void)initial;
    (void)password;
    (void)clear;
    return 0; /* a physical keyboard types into the field */
}

void plat_set_pointer(int x, int y)
{
    (void)x;
    (void)y;
}

void plat_show_cursor(int show) { (void)show; }

/* plat_save_dir: see plat.h. */
int plat_save_dir(char *out, size_t size)
{
    if (size < 8)
        return -1;
    snprintf(out, size, "SAVES");
    mkdir(out);
    return 0;
}

/* --- MIDI --------------------------------------------------------------- */

/* No system synthesizer here yet: the earlier games play without music
 * (docs/OTHER-GAMES.md). */
int plat_midi_open(void) { return -1; }
void plat_midi_send(u8 status, u8 d1, u8 d2) { (void)status; (void)d1; (void)d2; }
void plat_midi_close(void) {}
