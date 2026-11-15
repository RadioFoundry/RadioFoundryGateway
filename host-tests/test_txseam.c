// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* Where a long key-up's seams fall (esp_idf/main/tx_seam_core.c), with the seams' skips as fe_lr2021.c's long_pull
 * makes them (the delay's samples, the fraction carried). */
#include <stdio.h>
#include <math.h>
#include <stdlib.h>

#include "tx_seam_core.h"

static int failures;

#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #c);                                                        \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

enum { SLOT = 1440, LEAD = 44, AT = 1388, TOL = 40, MAX = 65535, SYNC_LO = 660, SYNC_HI = 900 };

/* The stream position of each seam's first skipped sample, n seams from start, as long_pull skips: seam_us at 48 kS/s,
 * whole samples, the fraction carried. Each seam's place in the slot (ref a slot's start) into ph. */
static void seams(int64_t start, int64_t run, int seam_us, int64_t ref, int n, int64_t *ph)
{
    double carry = 0;
    int64_t at = start;
    for (int i = 0; i < n; i++) {
        at += run;
        ph[i] = ((at - ref) % SLOT + SLOT) % SLOT;
        carry += seam_us * 0.048;
        const int64_t k = (int64_t)carry;
        carry -= (double)k;
        at += k;
    }
}

/* A seam's hole, from its first skipped sample, against the sync's samples: the samples between (negative: on it). */
static int64_t from_sync(int64_t ph, int64_t hole)
{
    if (ph + hole > SYNC_LO && ph < SYNC_HI)
        return -1;
    const int64_t before = (SYNC_LO - (ph + hole) + SLOT) % SLOT, after = (ph - SYNC_HI + SLOT) % SLOT;
    return before < after ? before : after;
}

static void test_run(void)
{
    CHECK(rdof_seam_run(8.208, MAX, SLOT) == 64792, "171 us: %lld", (long long)rdof_seam_run(8.208, MAX, SLOT));
    CHECK(rdof_seam_run(6.96, MAX, SLOT) == 64793, "145 us: %lld", (long long)rdof_seam_run(6.96, MAX, SLOT));
    CHECK(rdof_seam_run(26.4, MAX, SLOT) == 64774, "550 us: %lld", (long long)rdof_seam_run(26.4, MAX, SLOT));
    CHECK(rdof_seam_run(0, MAX, SLOT) == 64800, "no skip: 45 slots");
    CHECK(rdof_seam_run(8, 1000, SLOT) == 0, "no slot fits");
    CHECK(rdof_seam_run(8, MAX, 0) == 0 && rdof_seam_run(-1, MAX, SLOT) == 0, "nonsense in, 0 out");
    for (int us = 100; us <= 600; us++) {
        const double seam = us * 0.048;
        const int64_t r = rdof_seam_run(seam, MAX, SLOT);
        CHECK(r > 0 && r <= MAX && (r + (int64_t)(seam + 0.5)) % SLOT == 0, "%d us: %lld", us, (long long)r);
    }
}

static void test_start(void)
{
    const int64_t run = rdof_seam_run(8.208, MAX, SLOT);
    /* from the key-on: where the seams already go, nothing moved */
    for (int64_t key = 1000; key < 1000 + 3 * SLOT; key += 7) {
        const int64_t ref = key + LEAD;
        CHECK(rdof_seam_start(key, run, SLOT, ref, AT, TOL) == key, "key-on at %lld moved", (long long)key);
        CHECK(rdof_seam_phase(key, run, SLOT, ref) == AT, "key-on at %lld: seams at %lld", (long long)key,
              (long long)rdof_seam_phase(key, run, SLOT, ref));
    }
    /* a key-up started late (tx_service's catch-up): any sample after the key-on; moved at most a slot, onto AT */
    const int64_t key = 50000, ref = key + LEAD;
    for (int64_t go = key; go < key + 3 * SLOT; go++) {
        const int64_t s = rdof_seam_start(go, run, SLOT, ref, AT, TOL);
        const int64_t ph = rdof_seam_phase(s, run, SLOT, ref);
        int64_t d = ph - AT;
        if (d > SLOT / 2)
            d -= SLOT;
        if (d < -SLOT / 2)
            d += SLOT;
        CHECK(s >= go && s < go + SLOT, "late by %lld: moved %lld", (long long)(go - key), (long long)(s - go));
        CHECK(llabs(d) <= TOL, "late by %lld: seams at %lld", (long long)(go - key), (long long)ph);
        CHECK(s == go || d == 0, "moved but not onto AT: %lld", (long long)ph);
    }
    /* negative positions and at past a period */
    CHECK(rdof_seam_phase(-5, 10, SLOT, 0) == 5 && rdof_seam_phase(0, SLOT, SLOT, 0) == 0, "phase wraps");
    CHECK(rdof_seam_start(0, 0, SLOT, 0, AT + SLOT, 0) == AT, "at past a period: %lld",
          (long long)rdof_seam_start(0, 0, SLOT, 0, AT + SLOT, 0));
    CHECK(rdof_seam_start(17, 0, 0, 0, 3, 0) == 17, "no period: k_go");
}

/* The seams over a 60 s outbound and a 20 minute key-up, for every delay R3.49 saw and well past: they stay put, away
 * from the sync. Runs of a whole 45 slots (before) walk 8 samples a seam and reach the sync. */
static void test_drift(void)
{
    enum { N60 = 45, N20M = 889 };
    static int64_t ph[N20M];
    const int64_t key = 123457, ref = key + LEAD;
    for (int us = 140; us <= 240; us++) {
        const int64_t run = rdof_seam_run(us * 0.048, MAX, SLOT);
        seams(key, run, us, ref, N20M, ph);
        int64_t lo = ph[0], hi = ph[0];
        for (int i = 0; i < N20M; i++) {
            const int64_t hole = (int64_t)(us * 0.048) + 1;
            CHECK(from_sync(ph[i], hole) >= (i < N60 ? 400 : 0), "%d us, seam %d at %lld: %lld from the sync", us, i + 1,
                  (long long)ph[i], (long long)from_sync(ph[i], hole));
            if (i < N60) {
                lo = ph[i] < lo ? ph[i] : lo;
                hi = ph[i] > hi ? ph[i] : hi;
            }
        }
        CHECK(hi - lo <= 24, "%d us: 60 s of seams over %lld samples", us, (long long)(hi - lo));
    }
    /* before: 45 slots a run, from the key-on */
    seams(key, 64800, 171, ref, 200, ph);
    int first_on = -1;
    for (int i = 0; i < 200 && first_on < 0; i++)
        if (from_sync(ph[i], 9) < 0)
            first_on = i + 1;
    CHECK(ph[0] == SLOT - LEAD && ph[1] == SLOT - LEAD + 8, "before: seams at %lld, %lld", (long long)ph[0],
          (long long)ph[1]);
    CHECK(first_on > 80 && first_on < 95, "before: seam %d the first on the sync", first_on);
    printf("txseam: before, seam 1 at slot sample %lld, then 8.2 later each; seam %d (%.0f s) the first on the sync\n",
           (long long)ph[0], first_on, first_on * 1.35);
}

/* The first seams' skips: learned from each key-up's TX_DONE times, a quarter at a time; the rest steady. */
static void test_first(void)
{
    int gap[RDOF_SEAM_FIRST] = {-1, -1};
    CHECK(rdof_seam_skip_us(gap, 1, 210, 171) == 210 && rdof_seam_skip_us(gap, 2, 210, 171) == 210,
          "not learned: the first seams' default");
    CHECK(rdof_seam_skip_us(gap, 3, 210, 171) == 171 && rdof_seam_skip_us(gap, 40, 210, 171) == 171 &&
              rdof_seam_skip_us(gap, 0, 210, 171) == 171 && rdof_seam_skip_us(NULL, 1, 210, 171) == 210,
          "the rest: steady");
    const double run_us = 64792 * 1e6 / 48000; /* 1349833.3 */
    /* the first run's TX_DONE 450 us past its length from key-up (not a gap), then 225, 198, 172, 171 */
    int64_t done[6] = {0};
    done[0] = 1000000 + (int64_t)run_us + 450;
    const int g[] = {225, 198, 172, 171, 170};
    for (int i = 1; i < 6; i++)
        done[i] = done[i - 1] + (int64_t)llround(run_us) + g[i - 1];
    rdof_seam_learn(gap, done, 6, run_us);
    CHECK(gap[0] == 225 && gap[1] == 198, "learned %d/%d", gap[0], gap[1]);
    CHECK(rdof_seam_skip_us(gap, 1, 210, 171) == 225 && rdof_seam_skip_us(gap, 2, 210, 171) == 198 &&
              rdof_seam_skip_us(gap, 3, 210, 171) == 171,
          "the skips");
    /* the next key-up's 205 and 190: a quarter of the way */
    done[1] = done[0] + (int64_t)llround(run_us) + 205;
    done[2] = done[1] + (int64_t)llround(run_us) + 190;
    rdof_seam_learn(gap, done, 3, run_us);
    CHECK(gap[0] == 220 && gap[1] == 196, "averaged %d/%d", gap[0], gap[1]);
    /* one run only, or a gap that isn't one (a run lost: past 1 ms; the clock back): nothing learned */
    rdof_seam_learn(gap, done, 1, run_us);
    done[1] = done[0] + (int64_t)llround(run_us) + 5000;
    done[2] = done[1] + (int64_t)llround(run_us) - 3;
    rdof_seam_learn(gap, done, 3, run_us);
    CHECK(gap[0] == 220 && gap[1] == 196, "kept %d/%d", gap[0], gap[1]);
}

int main(void)
{
    test_run();
    test_first();
    test_start();
    test_drift();
    if (failures) {
        printf("txseam: %d failures\n", failures);
        return 1;
    }
    printf("txseam: ok\n");
    return 0;
}
