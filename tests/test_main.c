/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* Unit tests that need no game data. Run with: make test */
#include <stdio.h>
#include <string.h>

#include "audio/mixer.h"
#include "gfx/screen.h"
#include "inflate_vectors.h"
#include "xf_inflate.h"
#include "xf_text.h"

static int g_failed;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            g_failed++;                                                 \
        }                                                               \
    } while (0)

static void test_inflate(void)
{
    u8 out[sizeof(inflate_plain)];
    size_t i;

    for (i = 0; i < XF_COUNT(inflate_cases); i++) {
        long n = xf_zlib_uncompress(out, sizeof(out), inflate_cases[i], inflate_case_sizes[i]);
        CHECK(n == (long)sizeof(inflate_plain));
        CHECK(memcmp(out, inflate_plain, sizeof(out)) == 0);
        /* A buffer one byte short must fail, not overflow. */
        CHECK(xf_zlib_uncompress(out, sizeof(out) - 1, inflate_cases[i], inflate_case_sizes[i]) < 0);
        /* Truncated input must fail. */
        CHECK(xf_zlib_uncompress(out, sizeof(out), inflate_cases[i], inflate_case_sizes[i] / 2) < 0);
    }
}

static void test_mixer(void)
{
    i16 in[200], out[400];
    int ch, i, same = 1;

    xf_mixer_init(22050, NULL, NULL);
    ch = xf_mixer_open(22050, 1);
    CHECK(ch >= 0);
    for (i = 0; i < 200; i++)
        in[i] = (i16)(i * 100 - 10000);
    CHECK(xf_mixer_write(ch, in, 200) == 200);
    xf_mixer_finish(ch);
    /* Same rate, full volume: a mono source comes out unchanged on both
     * sides. The last frame stays queued for interpolation. */
    xf_mixer_render(NULL, out, 199);
    for (i = 0; i < 199; i++)
        same &= out[i * 2] == in[i] && out[i * 2 + 1] == in[i];
    CHECK(same);
    CHECK(xf_mixer_played(ch) == 199);
    CHECK(!xf_mixer_active(ch));
    xf_mixer_close(ch);

    /* Half rate: each source frame covers two output frames. */
    ch = xf_mixer_open(11025, 1);
    xf_mixer_write(ch, in, 200);
    xf_mixer_render(NULL, out, 100);
    CHECK(xf_mixer_played(ch) == 50);
    CHECK(out[2] == (in[0] + in[1]) / 2);
    xf_mixer_close(ch);
}

static void test_screen(void)
{
    static u32 scene[SCR_W * SCR_H], ui[SCR_W * SCR_H], out[1920 * 1080];
    scr_layout l;
    scr_rect dlg = { 0, 285, 246, 120 };
    int x, y, i, same = 1;

    /* 640x480: the original screen, one to one. */
    scr_layout_begin(&l, SCR_W, SCR_H, 1);
    CHECK(!l.wide);
    scr_to_screen(&l, 123, 456, &x, &y);
    CHECK(x == 123 && y == 456);
    for (i = 0; i < SCR_W * SCR_H; i++)
        scene[i] = (u32)i * 2654435761u & 0xFFFFFF;
    scr_compose(&l, out, scene, NULL, 0);
    for (i = 0; i < SCR_W * SCR_H; i++)
        same &= out[i] == scene[i];
    CHECK(same);

    /* 1920x1080: the strip keeps the 150 parts under it for the icons and
     * the inventory (337 pixels), so it is 1857x743 at the top. */
    scr_layout_begin(&l, 1920, 1080, 1);
    CHECK(l.wide && l.strip.x == 31 && l.strip.w == 1857 && l.strip.h == 743 && l.strip.y == 0);
    scr_to_screen(&l, 960, 371, &x, &y);
    CHECK(x == 320 && (y == 209 || y == 210));
    scr_add_piece(&l, dlg, SCR_ANCHOR_UNDER, NULL); /* under its centre, x 123 */
    CHECK(l.pieces[0].dst.x == 73 && l.pieces[0].dst.y == 642 && l.pieces[0].dst.w == 553);
    scr_to_screen(&l, 73 + 9, 642 + 9, &x, &y);
    CHECK(x == 4 && y == 289);
    scr_to_screen(&l, 1500, 1079, &x, &y); /* the bottom edge: the inventory */
    CHECK(y == 479);
    {
        /* the menu icon's corner, both ways: over the strip it is a piece */
        scr_rect corner = { 500, 0, 140, 51 };
        scr_add_piece(&l, corner, SCR_ANCHOR_TOP_RIGHT, NULL);
    }
    scr_to_output(&l, 565, 29, &x, &y);
    scr_to_screen(&l, x, y, &x, &y);
    CHECK(x >= 564 && x <= 566 && y >= 28 && y <= 30); /* within a pixel */
    scr_to_output(&l, 320, 210, &x, &y);
    CHECK(x >= 959 && x <= 960 && y >= 371 && y <= 372);
    {
        /* The dialogue box keeps above the inventory's band. */
        scr_layout v;
        scr_layout_begin(&v, 960, 544, 1);
        scr_reserve_bottom(&v, 420);
        scr_add_piece(&v, dlg, SCR_ANCHOR_UNDER, NULL);
        CHECK(v.reserve_y == 544 - 68 && v.pieces[0].dst.y + v.pieces[0].dst.h <= v.reserve_y);
    }
    for (i = 0; i < SCR_W * SCR_H; i++)
        ui[i] = SCR_CLEAR;
    ui[300 * SCR_W + 100] = 0x123456;
    scr_compose(&l, out, scene, ui, 0);
    CHECK(out[1079 * 1920] == 0); /* under the strip and the pieces */

    /* Vita, Steam Deck, ultrawide and super ultrawide. */
    scr_layout_begin(&l, 960, 544, 1);
    CHECK(l.strip.w == 935 && l.strip.h == 374 && l.strip.x == 12 && l.strip.y == 0);
    scr_layout_begin(&l, 1280, 800, 1);
    CHECK(l.strip.w == 1280 && l.strip.h == 512 && l.strip.y == 38);
    scr_layout_begin(&l, 3440, 1440, 1);
    CHECK(l.strip.w == 2475 && l.strip.h == 990 && l.strip.x == 482 && l.strip.y == 0); /* room for the icons */
    scr_layout_begin(&l, 5120, 1440, 1);
    CHECK(l.strip.w == 2475 && l.strip.h == 990 && l.strip.x == 1322);
    /* 4:3 and taller outputs keep the original screen. */
    scr_layout_begin(&l, 1024, 768, 1);
    CHECK(!l.wide && l.screen.w == 1024);
}

/* Shift-JIS documents, UTF-8 checks and the accent folding of search words. */
static void test_text(void)
{
    static const u8 sjis[] = { 0x82, 0xA0, 'A', 0xB1, 0x81, 0x46 }; /* あ A ｱ ： */
    char out[32];
    CHECK(xf_sjis_to_utf8(out, sizeof(out), sjis, sizeof(sjis)) == 10);
    CHECK(strcmp(out, "\xE3\x81\x82" "A" "\xEF\xBD\xB1" "\xEF\xBC\x9A") == 0);
    CHECK(xf_is_utf8("Portugu\xC3\xAAs", 10));
    CHECK(!xf_is_utf8("caf\xE9", 4)); /* Windows-1252 */
    xf_fold_ascii(out, sizeof(out), "Plut\xC3\xB3nio, OVNI");
    CHECK(strcmp(out, "plutonio, ovni") == 0);
    xf_fold_ascii(out, sizeof(out), "Fen\xF3meno"); /* Windows-1252 */
    CHECK(strcmp(out, "fenomeno") == 0);
}

int main(void)
{
    test_inflate();
    test_mixer();
    test_screen();
    test_text();
    if (g_failed) {
        printf("%d checks failed\n", g_failed);
        return 1;
    }
    puts("all tests passed");
    return 0;
}
