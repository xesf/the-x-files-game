/* Unit tests that need no game data. Run with: make test */
#include <stdio.h>
#include <string.h>

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

int main(void)
{
    test_inflate();
    if (g_failed) {
        printf("%d checks failed\n", g_failed);
        return 1;
    }
    puts("all tests passed");
    return 0;
}
