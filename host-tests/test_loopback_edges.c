// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The SDR modem's loopback check when its set-up runs out of memory at each step: it must say so and leave nothing
 * behind. (The check's other failure reports are its verdict on the modem, which the modem's own tests keep from
 * firing.)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/sdr/fft.h"
#include "rdof/sdr/sdrmodem.h"

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

static long a_count, a_fail_at, a_live, a_fired;

static void *fa_alloc(size_t n)
{
    a_count++;
    if (a_fail_at && a_count == a_fail_at) {
        a_fired++;
        return NULL;
    }
    a_live++;
    return malloc(n);
}

static void fa_free(void *p)
{
    a_live--;
    free(p);
}

int main(void)
{
    for (int full = 0; full <= 1; full++) {
        long total = 0;
        for (long k = 1; k < 200; k++) {
            a_count = a_live = a_fired = 0;
            a_fail_at = k;
            rdof_sdr_set_allocator(fa_alloc, fa_free);
            char report[160];
            int r = rdof_sdrmodem_loopback(full, report, sizeof report);
            long live = a_live, fired = a_fired;
            rdof_sdr_set_allocator(NULL, NULL);
            if (!fired) {
                CHECK(r == 0 && live == 0, "%s duplex with every allocation good: %d (%s), %ld left", full ? "full" : "half", r,
                      report, live);
                total = k - 1;
                break;
            }
            CHECK(r >= 1 && strstr(report, "init failed") && live == 0, "%s duplex, allocation %ld failing: %d '%s', %ld left",
                  full ? "full" : "half", k, r, report, live);
        }
        CHECK(total > 20, "set-up makes %ld allocations", total);
    }
    /* the report cut short, or not asked for */
    a_count = a_live = a_fired = 0;
    a_fail_at = 1;
    rdof_sdr_set_allocator(fa_alloc, fa_free);
    char tiny[12];
    CHECK(rdof_sdrmodem_loopback(false, tiny, sizeof tiny) >= 1 && !strcmp(tiny, "loopback: i"), "cut report '%s'", tiny);
    a_count = a_live = a_fired = 0;
    CHECK(rdof_sdrmodem_loopback(true, NULL, 0) >= 1 && a_live == 0, "no report buffer");
    rdof_sdr_set_allocator(NULL, NULL);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
