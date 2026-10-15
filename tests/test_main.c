/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 xesf */
/* Unit tests that need no game data. Run with: make test */
#include <stdio.h>
#include <string.h>

#include "audio/mixer.h"
#include "inflate_vectors.h"
#include "xf_inflate.h"

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

int main(void)
{
    test_inflate();
    test_mixer();
    if (g_failed) {
        printf("%d checks failed\n", g_failed);
        return 1;
    }
    puts("all tests passed");
    return 0;
}
