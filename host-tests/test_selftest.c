// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The firmware self-test's check on the host: the C receiver on the embedded clip must give the reference output,
 * on one core's path and on the split (two-core) path. */
#include <stdio.h>
#include <stdlib.h>

#include "selftest/clip.h"
#include "selftest_core.h"

int main(void)
{
    FILE *f = fopen(RDOF_CLIP_PATH, "rb");
    if (!f) {
        printf("FAIL cannot open %s\n", RDOF_CLIP_PATH);
        return 1;
    }
    static uint8_t clip[1 << 20];
    size_t n = fread(clip, 1, sizeof clip, f);
    fclose(f);
    int failures = 0;
    for (int split = 0; split <= 3; split += 3) {
        sdr_check_t c;
        bool ok = sdr_selftest_check(clip, n, split, &c);
        printf("%s %s: %d messages (want %d), CRC %08lx (want %08lx), %ld bursts (want %ld)\n", ok ? "pass" : "FAIL",
               split ? "split over two cores" : "one core", c.messages, c.expect_messages, (unsigned long)c.crc,
               (unsigned long)c.expect_crc, c.bursts, c.expect_bursts);
        failures += !ok;
    }
    /* the faults: on the host the C must give the reference output exactly, CRC too (the same float filter) */
    for (int v = 0; v < CLIP_VARIANTS; v++) {
        sdr_check_t c;
        bool ok = sdr_selftest_check_variant(clip, n, v, &c) && c.crc == c.expect_crc;
        printf("%s %s: %d messages (want %d), CRC %08lx (want %08lx), %ld bursts (want %ld)\n", ok ? "pass" : "FAIL",
               c.name, c.messages, c.expect_messages, (unsigned long)c.crc, (unsigned long)c.expect_crc, c.bursts,
               c.expect_bursts);
        failures += !ok;
    }
    return failures ? 1 : 0;
}
