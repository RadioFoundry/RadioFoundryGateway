// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The memory plan (rdof/memplan.h): claims, placement against the free memory and the margin, regions and their
 * alignment, holds, elastic and fallback claims, release and run-time claims, the status JSON. */
#include <stdio.h>
#include <string.h>

#include "rdof/memplan.h"

static int checks, failures;

#define CHECK(c)                                                                                                     \
    do {                                                                                                             \
        checks++;                                                                                                    \
        if (!(c)) {                                                                                                  \
            failures++;                                                                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                      \
        }                                                                                                            \
    } while (0)

static rdof_memplan_t P;

static rdof_mem_avail_t avail(uint32_t internal, uint32_t psram)
{
    return (rdof_mem_avail_t){internal, internal * 3 / 4, internal, internal * 3 / 4, psram, psram};
}

static void test_add(void)
{
    rdof_memplan_init(&P, 1024, false);
    CHECK(rdof_memplan_add(&P, "sdr", "iq", 0, 4, RDOF_MEM_INTERNAL, RDOF_MEM_ALLOC) == -1); /* no size */
    CHECK(!P.ok && strstr(P.error, "sdr iq: bad claim"));
    rdof_memplan_init(&P, 1024, false);
    CHECK(rdof_memplan_add(&P, "sdr", "iq", 64, 6, RDOF_MEM_INTERNAL, RDOF_MEM_ALLOC) == -1);    /* not a power of 2 */
    CHECK(rdof_memplan_add(&P, "sdr", "iq", 64, 8192, RDOF_MEM_INTERNAL, RDOF_MEM_ALLOC) == -1); /* past 4096 */
    CHECK(rdof_memplan_add(&P, "sdr", "iq", 64, 4, 0, RDOF_MEM_ALLOC) == -1);                    /* nowhere */
    CHECK(rdof_memplan_add(&P, "sdr", "iq", 64, 4, RDOF_MEM_FALLBACK | RDOF_MEM_PSRAM, RDOF_MEM_ALLOC) == -1);
    CHECK(rdof_memplan_add(&P, "sdr", "iq", 64, 4, RDOF_MEM_INTERNAL, (rdof_mem_how_t)7) == -1);
    CHECK(rdof_memplan_add(&P, "a_long_role_name", "iq", 64, 4, RDOF_MEM_INTERNAL, RDOF_MEM_ALLOC) == -1);
    CHECK(rdof_memplan_add(&P, "sdr", "a_much_too_long_name", 64, 4, RDOF_MEM_INTERNAL, RDOF_MEM_ALLOC) == -1);
    CHECK(P.n == 0 && P.nroles == 0);
    rdof_memplan_init(&P, 1024, false);
    CHECK(rdof_memplan_add(&P, "sdr", "iq", 64, 0, RDOF_MEM_INTERNAL, RDOF_MEM_ALLOC) == 0);
    CHECK(P.c[0].align == 4 && P.c[0].count == 1 && P.c[0].region == -1);
    CHECK(rdof_memplan_add(&P, "sdr", "iq", 64, 4, RDOF_MEM_INTERNAL, RDOF_MEM_ALLOC) == -1 &&
          strstr(P.error, "claimed twice"));
    CHECK(rdof_memplan_find(&P, "sdr", "iq") == 0 && rdof_memplan_find(&P, "sdr", "raw") == -1 &&
          rdof_memplan_find(&P, "radio1", "iq") == -1);
    /* the tables' ends */
    rdof_memplan_init(&P, 0, false);
    char name[16];
    for (int i = 0; i < RDOF_MEM_MAX_CLAIMS; i++) {
        snprintf(name, sizeof name, "c%d", i);
        CHECK(rdof_memplan_add(&P, "board", name, 4, 4, RDOF_MEM_INTERNAL, RDOF_MEM_BUDGET) == i);
    }
    CHECK(rdof_memplan_add(&P, "board", "one_more", 4, 4, RDOF_MEM_INTERNAL, RDOF_MEM_BUDGET) == -1 &&
          strstr(P.error, "too many claims"));
    rdof_memplan_init(&P, 0, false);
    for (int i = 0; i < RDOF_MEM_MAX_ROLES; i++) {
        snprintf(name, sizeof name, "r%d", i);
        CHECK(rdof_memplan_add(&P, name, "x", 4, 4, RDOF_MEM_INTERNAL, RDOF_MEM_ALLOC) == i);
    }
    CHECK(rdof_memplan_add(&P, "r99", "x", 4, 4, RDOF_MEM_INTERNAL, RDOF_MEM_ALLOC) == -1 &&
          strstr(P.error, "too many roles"));
    /* counts and elastic limits */
    rdof_memplan_init(&P, 0, false);
    int i = rdof_memplan_add(&P, "sdr", "xfer", 8192, 4, RDOF_MEM_DMA, RDOF_MEM_HOLD);
    rdof_memplan_count(&P, i, 0);
    CHECK(P.c[i].count == 1);
    rdof_memplan_count(&P, i, 99);
    CHECK(P.c[i].count == RDOF_MEM_MAX_COUNT);
    rdof_memplan_elastic(&P, i, 100, 0); /* below its size: fixed */
    CHECK(P.c[i].max == 0 && P.c[i].unit == 1);
    rdof_memplan_count(&P, 42, 3);       /* out of range: nothing */
    rdof_memplan_elastic(&P, -1, 3, 3);
}

/* The W12's shape: the SDR's DSP buffers, a DMA frame, stacks, a PSRAM ring; the board's budgets. */
static void w12(void)
{
    rdof_memplan_add(&P, "sdr", "dsp", 8192, 16, RDOF_MEM_INTERNAL, RDOF_MEM_ALLOC);
    rdof_memplan_add(&P, "sdr", "iq", 32768, 16, RDOF_MEM_INTERNAL, RDOF_MEM_ALLOC);
    rdof_memplan_add(&P, "sdr", "raw", 16384, 16, RDOF_MEM_INTERNAL, RDOF_MEM_ALLOC);
    rdof_memplan_add(&P, "sdr", "frames", 2101, 4, RDOF_MEM_DMA, RDOF_MEM_ALLOC);
    rdof_memplan_add(&P, "sdr", "ring", 262145, 4, RDOF_MEM_PSRAM, RDOF_MEM_ALLOC);
    rdof_memplan_add(&P, "board", "httpd", 8192, 4, RDOF_MEM_INTERNAL, RDOF_MEM_BUDGET);
}

static void test_solve(void)
{
    rdof_memplan_init(&P, 40 * 1024, false);
    w12();
    rdof_mem_avail_t a = avail(200000, 8000000);
    CHECK(rdof_memplan_solve(&P, &a) && P.ok && !P.error[0] && P.solved);
    const int dsp = rdof_memplan_find(&P, "sdr", "dsp"), iq = rdof_memplan_find(&P, "sdr", "iq"),
              raw = rdof_memplan_find(&P, "sdr", "raw"), fr = rdof_memplan_find(&P, "sdr", "frames"),
              ring = rdof_memplan_find(&P, "sdr", "ring"), httpd = rdof_memplan_find(&P, "board", "httpd");
    CHECK(P.c[dsp].where == RDOF_MEM_IN_INTERNAL && P.c[iq].where == RDOF_MEM_IN_INTERNAL &&
          P.c[fr].where == RDOF_MEM_IN_DMA && P.c[ring].where == RDOF_MEM_IN_PSRAM &&
          P.c[httpd].where == RDOF_MEM_IN_INTERNAL);
    /* three regions for the SDR (internal, DMA, PSRAM); none for a budget */
    CHECK(P.nr == 3 && P.c[httpd].region == -1);
    const rdof_mem_region_t *in = &P.r[P.c[iq].region];
    CHECK(P.c[dsp].region == P.c[iq].region && P.c[iq].region == P.c[raw].region && in->where == RDOF_MEM_IN_INTERNAL);
    CHECK(P.c[dsp].offset == 0 && P.c[iq].offset == 8192 && P.c[raw].offset == 8192 + 32768);
    CHECK(in->bytes == 8192 + 32768 + 16384 && in->align == 16);
    CHECK(P.r[P.c[fr].region].bytes == 2101 && P.r[P.c[fr].region].align == 4);
    CHECK(P.r[P.c[ring].region].where == RDOF_MEM_IN_PSRAM && P.r[P.c[ring].region].bytes == 262145);
    /* spare: free, less the margin, less every internal claim and budget, and what aligning to 16 can waste */
    const int64_t need = 8192 + 32768 + 16384 + 3 * 12 + 2101 + 8192;
    CHECK(P.spare == 200000 - 40 * 1024 - need);
    const int sdr = rdof_memplan_role(&P, "sdr"), board = rdof_memplan_role(&P, "board");
    CHECK(rdof_memplan_role_bytes(&P, sdr, RDOF_MEM_IN_INTERNAL) == 8192 + 32768 + 16384);
    CHECK(rdof_memplan_role_bytes(&P, sdr, RDOF_MEM_IN_DMA) == 2101);
    CHECK(rdof_memplan_role_bytes(&P, sdr, RDOF_MEM_IN_PSRAM) == 262145);
    CHECK(rdof_memplan_role_bytes(&P, board, RDOF_MEM_IN_INTERNAL) == 0 && rdof_memplan_role_budget(&P, board) == 8192);
    CHECK(rdof_memplan_internal_held(&P) == 8192 + 32768 + 16384 + 2101);
    CHECK(rdof_memplan_role_ok(&P, "sdr") && rdof_memplan_role_ok(&P, "board") && !rdof_memplan_role_ok(&P, "ipgw"));

    /* a claim's alignment within its region: a 4-byte one after a 2-byte size, then a 16-byte one */
    rdof_memplan_init(&P, 0, false);
    rdof_memplan_add(&P, "x", "a", 6, 4, RDOF_MEM_INTERNAL, RDOF_MEM_ALLOC);
    rdof_memplan_add(&P, "x", "b", 10, 16, RDOF_MEM_INTERNAL, RDOF_MEM_ALLOC);
    rdof_memplan_add(&P, "x", "c", 3, 4, RDOF_MEM_INTERNAL, RDOF_MEM_ALLOC);
    a = avail(100000, 0);
    CHECK(rdof_memplan_solve(&P, &a));
    CHECK(P.c[0].offset == 0 && P.c[1].offset == 16 && P.c[2].offset == 28 && P.r[0].bytes == 31 && P.r[0].align == 16);
}

static void test_short(void)
{
    /* over the margin, not strict: placed, and the plan says so */
    rdof_memplan_init(&P, 64 * 1024, false);
    w12();
    rdof_mem_avail_t a = avail(120000, 8000000);
    CHECK(!rdof_memplan_solve(&P, &a) && !P.ok);
    CHECK(strstr(P.error, "sdr needs 59 KB of internal RAM, 53 KB is free above the 64 KB margin") != NULL);
    const int sdr = rdof_memplan_role(&P, "sdr");
    CHECK(P.role[sdr].state == RDOF_ROLE_PLANNED && strstr(P.role[sdr].why, "over the margin"));
    CHECK(P.c[rdof_memplan_find(&P, "sdr", "iq")].where == RDOF_MEM_IN_INTERNAL && P.nr == 3);
    CHECK(P.spare < 0);
    /* strict: refused; the board's budget after it still fits */
    rdof_memplan_init(&P, 64 * 1024, true);
    w12();
    CHECK(!rdof_memplan_solve(&P, &a));
    CHECK(P.role[sdr].state == RDOF_ROLE_REFUSED && strstr(P.role[sdr].why, "sdr needs 59 KB"));
    CHECK(P.c[rdof_memplan_find(&P, "sdr", "iq")].where == RDOF_MEM_NOWHERE && P.nr == 0);
    CHECK(rdof_memplan_role_ok(&P, "board") && !rdof_memplan_role_ok(&P, "sdr"));
    CHECK(P.spare == 120000 - 64 * 1024 - 8192);
    /* DMA short while internal isn't (another chip's DMA-capable RAM is a part of it) */
    rdof_memplan_init(&P, 1024, true);
    rdof_memplan_add(&P, "fe", "frames", 20000, 4, RDOF_MEM_DMA, RDOF_MEM_ALLOC);
    a = (rdof_mem_avail_t){100000, 90000, 15000, 15000, 0, 0};
    CHECK(!rdof_memplan_solve(&P, &a) && strstr(P.error, "fe needs 20 KB of DMA RAM, 13 KB is free above the 1 KB margin"));
    /* PSRAM short: refused even when not strict */
    rdof_memplan_init(&P, 1024, false);
    w12();
    a = avail(200000, 100000);
    CHECK(!rdof_memplan_solve(&P, &a) && strstr(P.error, "sdr needs 257 KB of PSRAM, 98 KB is free"));
    CHECK(!rdof_memplan_role_ok(&P, "sdr") && rdof_memplan_role_ok(&P, "board"));
    /* no PSRAM at all: PSRAM claims are internal (and here don't fit) */
    rdof_memplan_init(&P, 1024, false);
    rdof_memplan_add(&P, "log", "ring", 16384, 4, RDOF_MEM_PSRAM, RDOF_MEM_ALLOC);
    a = avail(100000, 0);
    CHECK(rdof_memplan_solve(&P, &a) && P.c[0].where == RDOF_MEM_IN_INTERNAL && P.r[0].where == RDOF_MEM_IN_INTERNAL);
}

static void test_fallback_elastic(void)
{
    /* the second radio's stacks: internal while the spare holds them, then PSRAM */
    rdof_memplan_init(&P, 10000, false);
    rdof_memplan_add(&P, "radio1", "radio", 8192, 4, RDOF_MEM_INTERNAL, RDOF_MEM_ALLOC);
    const int a1 = rdof_memplan_add(&P, "radio2", "radio", 8192, 4, RDOF_MEM_INTERNAL | RDOF_MEM_FALLBACK, RDOF_MEM_ALLOC);
    const int a2 = rdof_memplan_add(&P, "radio2", "rx", 8192, 4, RDOF_MEM_INTERNAL | RDOF_MEM_FALLBACK, RDOF_MEM_ALLOC);
    rdof_mem_avail_t a = avail(10000 + 8192 + 9000, 1000000);
    CHECK(rdof_memplan_solve(&P, &a) && P.c[a1].where == RDOF_MEM_IN_INTERNAL && P.c[a2].where == RDOF_MEM_IN_PSRAM);
    CHECK(P.c[a1].region != P.c[a2].region && P.r[P.c[a2].region].where == RDOF_MEM_IN_PSRAM);
    CHECK(P.spare == 9000 - 8192);
    /* with no PSRAM it's internal anyway, and short */
    a = avail(10000 + 8192 + 9000, 0);
    CHECK(!rdof_memplan_solve(&P, &a) && P.c[a2].where == RDOF_MEM_IN_INTERNAL && strstr(P.error, "radio2 rx: 8 KB short"));

    /* an elastic queue: as deep as the spare allows, in whole entries */
    rdof_memplan_init(&P, 10000, false);
    const int q = rdof_memplan_add(&P, "ipgw", "queue", 64 * 40, 4, RDOF_MEM_INTERNAL, RDOF_MEM_ALLOC);
    rdof_memplan_elastic(&P, q, 256 * 40, 40);
    a = avail(100000, 0);
    CHECK(rdof_memplan_solve(&P, &a) && P.c[q].got == 256 * 40 && P.r[0].bytes == 256 * 40);
    a = avail(10000 + 64 * 40 + 1000, 0);
    CHECK(rdof_memplan_solve(&P, &a) && P.c[q].got == 64 * 40 + 1000 / 40 * 40 && P.spare == 1000 % 40);
    a = avail(10000 + 64 * 40 - 5, 0); /* nothing spare: its least, over the margin */
    CHECK(!rdof_memplan_solve(&P, &a) && P.c[q].got == 64 * 40);
    /* an elastic claim of several blocks grows each */
    rdof_memplan_init(&P, 0, false);
    const int b = rdof_memplan_add(&P, "x", "bufs", 100, 4, RDOF_MEM_INTERNAL, RDOF_MEM_ALLOC);
    rdof_memplan_count(&P, b, 4);
    rdof_memplan_elastic(&P, b, 1000, 10);
    a = avail(400 + 2000, 0);
    CHECK(rdof_memplan_solve(&P, &a) && P.c[b].got == 600 && P.spare == 0 && P.r[0].bytes == 2400);
}

static void test_hold_release(void)
{
    rdof_memplan_init(&P, 1000, false);
    const int x = rdof_memplan_add(&P, "sdr", "usb_xfer", 8192, 4, RDOF_MEM_DMA, RDOF_MEM_HOLD);
    rdof_memplan_count(&P, x, 8);
    const int iq = rdof_memplan_add(&P, "sdr", "iq", 32768, 16, RDOF_MEM_INTERNAL, RDOF_MEM_ALLOC);
    rdof_memplan_add(&P, "ipgw", "stack", 6144, 4, RDOF_MEM_INTERNAL, RDOF_MEM_ALLOC);
    rdof_mem_avail_t a = avail(200000, 1000000);
    CHECK(rdof_memplan_solve(&P, &a));
    const int sdr = rdof_memplan_role(&P, "sdr");
    CHECK(P.c[x].region == -1 && P.c[x].where == RDOF_MEM_IN_DMA && P.nr == 2);
    CHECK(rdof_memplan_role_bytes(&P, sdr, RDOF_MEM_IN_DMA) == 8 * 8192);
    for (int k = 0; k < 8; k++)
        CHECK(rdof_memplan_hand_over(&P, x));
    CHECK(!rdof_memplan_hand_over(&P, x) && !rdof_memplan_hand_over(&P, iq) && !rdof_memplan_hand_over(&P, 99));
    CHECK(rdof_memplan_role_bytes(&P, sdr, RDOF_MEM_IN_DMA) == 0);
    CHECK(rdof_memplan_internal_held(&P) == 32768 + 6144);

    /* the SDR stops: its memory goes back, and another role claims some */
    CHECK(rdof_memplan_release(&P, "sdr") == 32768);
    CHECK(rdof_memplan_release(&P, "sdr") == 0 && rdof_memplan_release(&P, "nobody") == 0);
    CHECK(P.role[sdr].state == RDOF_ROLE_RELEASED && strstr(P.role[sdr].why, "32 KB of internal RAM given back"));
    CHECK(P.c[iq].where == RDOF_MEM_NOWHERE && P.r[0].released && !rdof_memplan_role_ok(&P, "sdr"));
    CHECK(rdof_memplan_internal_held(&P) == 6144);
    char why[160];
    rdof_mem_avail_t now = avail(60000, 1000000);
    const int big = rdof_memplan_add_now(&P, "ipgw", "queue2", 30000, 4, RDOF_MEM_INTERNAL, &now, why, sizeof why);
    CHECK(big >= 0 && !why[0] && P.c[big].where == RDOF_MEM_IN_INTERNAL && P.c[big].region == 0 && P.nr == 2);
    CHECK(P.r[0].bytes == 30000 && !P.r[0].released && P.r[0].role == rdof_memplan_role(&P, "ipgw"));
    /* one that doesn't fit above the margin: refused, and the plan's state stays */
    const int n = P.n;
    CHECK(rdof_memplan_add_now(&P, "web", "big", 59500, 4, RDOF_MEM_INTERNAL, &now, why, sizeof why) == -1);
    CHECK(strstr(why, "web big: 59 KB doesn't fit (internal: 57 KB free above the 1 KB margin)") && P.n == n &&
          rdof_memplan_role(&P, "web") == -1 && P.ok);
    /* larger than the largest block */
    now.internal_largest = 20000;
    CHECK(rdof_memplan_add_now(&P, "web", "big", 25000, 4, RDOF_MEM_INTERNAL, &now, why, sizeof why) == -1);
    /* PSRAM and FALLBACK at run time */
    const int ps = rdof_memplan_add_now(&P, "web", "pages", 100000, 4, RDOF_MEM_PSRAM, &now, why, sizeof why);
    CHECK(ps >= 0 && P.c[ps].where == RDOF_MEM_IN_PSRAM);
    const int fb = rdof_memplan_add_now(&P, "web", "fb", 59500, 4, RDOF_MEM_INTERNAL | RDOF_MEM_FALLBACK, &now, why,
                                        sizeof why);
    CHECK(fb >= 0 && P.c[fb].where == RDOF_MEM_IN_PSRAM);
    /* the SDR again, after its release: its claim's name is free to take */
    now = avail(100000, 1000000);
    const int again = rdof_memplan_add_now(&P, "sdr", "iq", 32768, 16, RDOF_MEM_INTERNAL, &now, why, sizeof why);
    CHECK(again == iq && P.c[iq].where == RDOF_MEM_IN_INTERNAL && rdof_memplan_role_ok(&P, "sdr") && !P.role[sdr].why[0]);
    CHECK(rdof_memplan_add_now(&P, "sdr", "iq", 32768, 16, RDOF_MEM_INTERNAL, &now, why, sizeof why) == -1 &&
          strstr(why, "claimed twice"));
    /* the region table full */
    rdof_memplan_init(&P, 0, false);
    now = avail(1000000, 0);
    char name[16];
    int made = 0;
    for (int k = 0; k < RDOF_MEM_MAX_REGIONS + 2; k++) {
        snprintf(name, sizeof name, "r%d", k);
        made += rdof_memplan_add_now(&P, "x", name, 16, 4, RDOF_MEM_INTERNAL, &now, why, sizeof why) >= 0;
    }
    CHECK(made == RDOF_MEM_MAX_REGIONS && strstr(why, "too many regions"));
    /* the platform's allocation failed: refused */
    rdof_memplan_init(&P, 0, false);
    rdof_memplan_add(&P, "sdr", "iq", 64, 4, RDOF_MEM_INTERNAL, RDOF_MEM_ALLOC);
    a = avail(1000, 0);
    CHECK(rdof_memplan_solve(&P, &a));
    rdof_memplan_refuse(&P, 0, "sdr: the internal region (64 bytes) wasn't allocated");
    CHECK(!P.ok && !rdof_memplan_role_ok(&P, "sdr") && P.c[0].where == RDOF_MEM_NOWHERE);
    rdof_memplan_refuse(&P, 7, "nothing");
}

static void test_json(void)
{
    rdof_memplan_init(&P, 40 * 1024, false);
    w12();
    const int x = rdof_memplan_add(&P, "sdr", "usb_xfer", 8192, 4, RDOF_MEM_DMA, RDOF_MEM_HOLD);
    rdof_mem_avail_t a = avail(200000, 8000000);
    rdof_memplan_solve(&P, &a);
    rdof_memplan_hand_over(&P, x);
    P.c[rdof_memplan_find(&P, "sdr", "iq")].taken = true;
    rdof_mem_avail_t now = {20000, 9000, 19000, 8000, 7000000, 4000000};
    char buf[4096];
    rdof_json_t j;
    rdof_json_init(&j, buf, sizeof buf);
    rdof_json_open(&j, NULL);
    rdof_memplan_json(&P, &now, &j);
    rdof_json_close(&j);
    CHECK(rdof_json_done(&j));
    CHECK(strstr(buf, "{\"memory\":{\"planned\":true,\"ok\":true,\"error\":\"\",\"margin\":40960,\"strict\":false,"));
    /* planned: 56 KB internal, 2101 DMA; outside: 200000 - 20000 - 59445 */
    CHECK(strstr(buf, "\"internal\":{\"free\":20000,\"largest\":9000,\"at_plan\":200000,\"planned\":59445,\"budget\":8192,"
                      "\"outside_plan\":120555}"));
    CHECK(strstr(buf, "\"dma\":{\"free\":19000,\"largest\":8000}"));
    CHECK(strstr(buf, "\"psram\":{\"free\":7000000,\"largest\":4000000,\"planned\":262145}"));
    CHECK(strstr(buf, "\"roles\":{\"sdr\":{\"state\":\"planned\",\"why\":\"\",\"internal\":57344,\"dma\":2101,"
                      "\"psram\":262145,\"budget\":0,\"claims\":{\"dsp\":{\"bytes\":8192,\"in\":\"internal\",\"how\":"
                      "\"alloc\",\"align\":16,\"taken\":false},\"iq\":{\"bytes\":32768,\"in\":\"internal\",\"how\":"
                      "\"alloc\",\"align\":16,\"taken\":true}"));
    CHECK(strstr(buf, "\"usb_xfer\":{\"bytes\":8192,\"in\":\"dma\",\"how\":\"hold\",\"handed\":1}"));
    CHECK(strstr(buf, "\"board\":{\"state\":\"planned\",\"why\":\"\",\"internal\":0,\"dma\":0,\"psram\":0,\"budget\":8192,"
                      "\"claims\":{\"httpd\":{\"bytes\":8192,\"in\":\"internal\",\"how\":\"budget\"}}}}}}"));
    char d[512];
    size_t n = rdof_memplan_describe(&P, d, sizeof d);
    CHECK(n == strlen(d));
    CHECK(!strcmp(d, "sdr planned: 59 KB internal (3 DMA), 257 KB PSRAM, 0 KB budgeted\n"
                     "board planned: 0 KB internal (0 DMA), 0 KB PSRAM, 8 KB budgeted\n"));
    CHECK(rdof_memplan_describe(&P, d, 20) == 19 && strlen(d) == 19);
    rdof_memplan_release(&P, "sdr");
    rdof_json_init(&j, buf, sizeof buf);
    rdof_json_open(&j, NULL);
    rdof_memplan_json(&P, &now, &j);
    rdof_json_close(&j);
    CHECK(strstr(buf, "\"sdr\":{\"state\":\"released\",\"why\":\"released: 59 KB of internal RAM given back\""));
    CHECK(strstr(buf, "\"iq\":{\"bytes\":32768,\"in\":\"none\",\"how\":\"alloc\",\"align\":16,\"taken\":false}"));
    CHECK(!strcmp(rdof_mem_where_name(9), "none") && !strcmp(rdof_mem_how_name(RDOF_MEM_BUDGET), "budget"));
}

int main(void)
{
    test_add();
    test_solve();
    test_short();
    test_fallback_elastic();
    test_hold_release();
    test_json();
    printf("memplan: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
