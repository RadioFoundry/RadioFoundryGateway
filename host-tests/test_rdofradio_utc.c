// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * rdofradio's UTC slot timing (sdr/cli/rdofradio_utc.c): an SDR whose sample
 * clock runs 20 ppm fast, read in 10 ms chunks that arrive late by a jittery
 * USB and scheduler delay, for three hours. The map stays on the stream's
 * earliest arrival, with no drift: a slot boundary it gives is the same few
 * microseconds from the truth at the end as at the start (free running, the
 * 20 ppm would have moved it 216 ms, seven slots). The gate: off the grid
 * until the host's clock is synced, holdover on a bad estimate, then free.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "rdofradio_utc.h"

static int fails;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fails++;                                                                                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

#define RATE 960000.0
#define PPM 20.0
#define CHUNK 9600
#define LATENCY_NS 2000000LL
#define T0_NS 1793000000000000000LL /* 2026-10 */

/* the host: UTC is the true time; its discipline as the test sets it */
static struct {
    int64_t now_ns; /* true time since the stream's start */
    bool synced;
    double err_us;
} H;
static int64_t fake_utc(void *ctx) { return T0_NS + H.now_ns; }
static bool fake_synced(void *ctx, double *err_us)
{
    *err_us = H.synced ? H.err_us : NAN;
    return H.synced;
}

static uint32_t lcg = 12345;
static int64_t jitter_ns(void)
{
    lcg = lcg * 1664525u + 1013904223u;
    const uint32_t r = lcg >> 8;
    if (r % 500 == 0)
        return 30000000; /* now and then the loop is 30 ms late */
    return (int64_t)(r % 3000) * 1000; /* 0 to 3 ms */
}

/* the true time sample e was taken: the SDR's clock 20 ppm fast */
static double true_s(int64_t e) { return (double)e / (RATE * (1.0 + PPM * 1e-6)); }

/* what the map says about 48 kS/s position p, against the truth plus the earliest arrival's latency, µs */
static double map_err_us(int64_t e)
{
    int64_t i0, t0;
    double r48;
    if (!rdofradio_utc_map48(&i0, &t0, &r48))
        return NAN;
    const double p = (double)e * 48000.0 / RATE;
    const double est_ns = (double)t0 + (p - (double)i0) * 1e9 / r48;
    const double want_ns = (double)T0_NS + true_s(e) * 1e9 + (double)LATENCY_NS;
    return (est_ns - want_ns) / 1000.0;
}

static int64_t e; /* samples read so far */
/* once a minute: the worst error and the mean, the first and the last hour's */
static double worst_us, sum_first, sum_last;
static int nchecks;
static void run(double seconds, bool check)
{
    const int64_t until = e + (int64_t)(seconds * RATE);
    while (e < until) {
        e += CHUNK;
        H.now_ns = (int64_t)llround(true_s(e) * 1e9) + LATENCY_NS + jitter_ns();
        rdofradio_utc_arrived(e, (double)H.now_ns / 1e9);
        if (check && e % (int64_t)(RATE * 60) < CHUNK) {
            const double err = map_err_us(e);
            if (!(fabs(err) <= fabs(worst_us)))
                worst_us = err;
            if (nchecks < 60)
                sum_first += err;
            else if (nchecks >= 120)
                sum_last += err;
            nchecks++;
        }
    }
}

int main(void)
{
    const rdofradio_utc_clock_t clock = {fake_utc, fake_synced, NULL};
    rdof_slot_timing_t st;
    rdof_slot_timing_default(&st);
    st.utc = true;
    rdofradio_utc_setup(&st, RATE, false, &clock);
    CHECK(rdofradio_utc_enabled(), "slot timing utc");

    /* the host's clock not synced yet: off the grid */
    run(5, false);
    int64_t i0, t0;
    double r48;
    CHECK(!rdofradio_utc_map48(&i0, &t0, &r48), "no map before the clock syncs");
    const char *why;
    CHECK(rdofradio_utc_state(&why, NULL) == RDOF_GATE_FREE, "free before the clock syncs (%s)", why);

    /* synced: on the grid; three hours with no drift */
    H.synced = true, H.err_us = 300;
    run(10, false);
    CHECK(rdofradio_utc_state(&why, NULL) == RDOF_GATE_ALIGNED, "aligned once synced (%s)", why);
    const double first = map_err_us(e);
    CHECK(fabs(first) < 50, "the map on the earliest arrival: %.1f us", first);
    double ppm = 0;
    run(3 * 3600, true);
    rdofradio_utc_state(NULL, &ppm);
    const double drift = sum_last / 60 - sum_first / 60;
    printf("rdofradio utc: 3 h at %+.0f ppm: the map %.1f us at the start, worst %.1f us, the last hour's mean less "
           "the first's %.1f us, ppm %.3f\n",
           PPM, first, worst_us, drift, ppm);
    /* the floor is the earliest of a window's arrivals (here 200 of them, 0 to 3 ms late): tens of us */
    CHECK(fabs(worst_us) < 100, "the map stays within 100 us over 3 h: worst %.1f us", worst_us);
    CHECK(nchecks == 180 && fabs(drift) < 10, "no drift: %.1f us over %d minutes", drift, nchecks);
    CHECK(fabs(ppm - PPM) < 0.2, "the sample clock's ppm %.3f, want %.0f", ppm, PPM);

    /* a bad estimate: holdover for 60 s, then free (off the grid) */
    H.err_us = 5000;
    run(2, false);
    CHECK(rdofradio_utc_state(&why, NULL) == RDOF_GATE_HOLDOVER, "holdover on a bad estimate (%s)", why);
    CHECK(rdofradio_utc_map48(&i0, &t0, &r48), "the map holds over");
    run(65, false);
    CHECK(rdofradio_utc_state(&why, NULL) == RDOF_GATE_FREE, "free after the holdover (%s)", why);
    CHECK(!rdofradio_utc_map48(&i0, &t0, &r48), "no map after the holdover");

    /* own time: on the grid with the host's clock not synced, and then it is */
    H.synced = false;
    rdofradio_utc_setup(&st, RATE, true, &clock);
    run(10, false);
    CHECK(rdofradio_utc_state(&why, NULL) == RDOF_GATE_ALIGNED, "own time: aligned unsynced (%s)", why);
    CHECK(fabs(map_err_us(e)) < 100, "own time: the map %.1f us", map_err_us(e));
    H.synced = true, H.err_us = 300;
    run(2, false);
    CHECK(rdofradio_utc_state(&why, NULL) == RDOF_GATE_ALIGNED, "own time: aligned synced (%s)", why);

    /* slot timing off: nothing */
    st.utc = false;
    rdofradio_utc_setup(&st, RATE, false, &clock);
    CHECK(!rdofradio_utc_enabled(), "slot timing free");

    if (fails) {
        printf("%d failures\n", fails);
        return 1;
    }
    printf("rdofradio utc: all passed\n");
    return 0;
}
