// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Host tests for the RTL-SDR stream's gap filler (rtlsdr/src/gaps.c).
 */
#include <stdio.h>
#include <string.h>

#include "rtlsdr/gaps.h"

static int checks, failures;
#define CHECK(cond, ...)                                                                                     \
    do {                                                                                                     \
        checks++;                                                                                            \
        if (!(cond)) {                                                                                       \
            failures++;                                                                                      \
            printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                          \
            printf(__VA_ARGS__);                                                                             \
            printf("\n");                                                                                    \
        }                                                                                                    \
    } while (0)

/* A stream of transfers, some dropped, read back in odd-sized pieces through the gap list: the reader must see the
 * original stream with every dropped transfer replaced by filler (up to the cap), in order. With more gaps
 * pending than the list holds, the extras join the last one: the length is still right, the places aren't. */
static void gaps_case(const char *name, int xfer, int n, const char *drop, uint32_t max_fill, int seed, bool exact)
{
    static uint8_t ring[1 << 20], want[1 << 20], got[1 << 20];
    static rtl_gaps_t g;
    rtl_gaps_reset(&g, max_fill);
    size_t w = 0, nw = 0, run = 0;
    for (int t = 0; t < n; t++) {
        if (drop[t] == 'x') {
            rtl_gaps_note(&g, w, (uint32_t)xfer);
            size_t fill = run + xfer <= max_fill ? (size_t)xfer : run < max_fill ? max_fill - run : 0;
            memset(want + nw, 0x80, fill);
            nw += fill;
            run += xfer;
            continue;
        }
        run = 0;
        for (int i = 0; i < xfer; i++) {
            ring[w] = (uint8_t)((t * 7 + i) % 251);
            want[nw++] = ring[w++];
        }
    }
    size_t r = 0, ng = 0;
    unsigned rnd = (unsigned)seed;
    for (int guard = 0; guard < 100000; guard++) {
        rnd = rnd * 1103515245u + 12345u;
        size_t ask = 1 + (rnd >> 8) % 3000;
        bool fill;
        size_t k = rtl_gaps_next(&g, r, ask, &fill);
        if (fill) {
            memset(got + ng, 0x80, k);
            rtl_gaps_took(&g, k);
        } else {
            if (k > w - r)
                k = w - r;
            if (!k)
                break;
            memcpy(got + ng, ring + r, k);
            r += k;
        }
        ng += k;
    }
    CHECK(ng == nw && (!exact || memcmp(got, want, nw) == 0), "%s: %zu bytes back, want %zu", name, ng, nw);
    size_t dropped = 0;
    for (int t = 0; t < n; t++)
        dropped += drop[t] == 'x' ? (size_t)xfer : 0;
    CHECK(g.lost == dropped && g.filled == nw - w, "%s: lost %llu filled %llu", name, (unsigned long long)g.lost,
          (unsigned long long)g.filled);
}

static void test_gaps(void)
{
    gaps_case("no gaps", 512, 20, "....................", 1 << 20, 1, true);
    gaps_case("one gap", 512, 20, ".....x..............", 1 << 20, 2, true);
    gaps_case("first and last", 512, 20, "x..................x", 1 << 20, 3, true);
    gaps_case("back to back", 512, 20, "...xxx....x.x.......", 1 << 20, 4, true);
    gaps_case("capped", 512, 20, "..xxxxx...x.........", 1200, 5, true);
    char many[60];
    for (int i = 0; i < 59; i++)
        many[i] = i % 2 ? 'x' : '.';
    many[59] = 0;
    gaps_case("more gaps than slots", 256, 59, many, 1 << 20, 6, false);
}

/* The corners: a list with no filler allowed, gaps of nothing, and the reader finishing a gap it didn't start. */
static void test_gaps_edges(void)
{
    rtl_gaps_t g;
    bool fill;
    /* took() with nothing noted changes nothing */
    rtl_gaps_reset(&g, 1000);
    rtl_gaps_took(&g, 50);
    CHECK(g.filled == 0 && g.count == 0, "took with no gap");
    CHECK(rtl_gaps_next(&g, 0, 77, &fill) == 77 && !fill, "no gaps: all data");
    /* max_fill 0: a gap is counted as lost but never filled, and next() skips over it */
    rtl_gaps_reset(&g, 0);
    rtl_gaps_note(&g, 100, 512);
    rtl_gaps_note(&g, 200, 512);
    CHECK(g.lost == 1024 && g.count == 2, "lost %llu, %d gaps", (unsigned long long)g.lost, g.count);
    CHECK(rtl_gaps_next(&g, 0, 5000, &fill) == 5000 && !fill && g.count == 0, "empty gaps dropped, then all data");
    /* a zero-length drop ahead of a real one */
    rtl_gaps_reset(&g, 1000);
    rtl_gaps_note(&g, 10, 0);
    rtl_gaps_note(&g, 20, 300);
    CHECK(g.count == 2, "two gaps");
    size_t k = rtl_gaps_next(&g, 0, 100, &fill);
    CHECK(k == 20 && !fill && g.count == 1, "data up to the real gap (%zu)", k);
    k = rtl_gaps_next(&g, 20, 100, &fill);
    CHECK(k == 100 && fill, "filler (%zu)", k);
    rtl_gaps_took(&g, 100);
    k = rtl_gaps_next(&g, 20, 1000, &fill);
    CHECK(k == 200 && fill, "the rest of the filler (%zu)", k);
    rtl_gaps_took(&g, 200);
    CHECK(g.count == 0 && g.filled == 300, "filled %llu", (unsigned long long)g.filled);
    /* a gap behind the reader is filled at once */
    rtl_gaps_reset(&g, 1000);
    rtl_gaps_note(&g, 5, 64);
    CHECK(rtl_gaps_next(&g, 50, 1000, &fill) == 64 && fill, "late reader");
    /* a long stall is capped, in one gap or across many */
    rtl_gaps_reset(&g, 600);
    rtl_gaps_note(&g, 7, 512);
    rtl_gaps_note(&g, 7, 512);
    rtl_gaps_note(&g, 7, 512);
    CHECK(g.lost == 1536 && g.len[0] == 600 && g.count == 1, "capped at 600: %u", g.len[0]);
}

int main(void)
{
    test_gaps();
    test_gaps_edges();
    printf("rtlsdr_gaps_tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
