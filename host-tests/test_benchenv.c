// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The carrier envelope (rdof/benchenv.h) on synthetic keyings: a noise floor at -60 dB, carriers at -20 dB whose
 * amplitude ramps linearly over 1 ms, so the edges, ramps, on time and a level step inside are known.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "rdof/benchenv.h"

static int fails;
#define CHECK(cond, ...)                                                                                     \
    do {                                                                                                     \
        if (!(cond)) {                                                                                       \
            fails++;                                                                                         \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                      \
            printf(__VA_ARGS__);                                                                             \
            printf("\n");                                                                                    \
        }                                                                                                    \
    } while (0)

#define RATE 48000.0
#define D 8

static unsigned rng = 12345;
static double urand(void)
{
    rng = rng * 1103515245u + 12345u;
    return ((rng >> 8) + 0.5) / 16777216.0;
}

/* The carrier's amplitude at output i: 0 off; segments [start, start + len) outputs, ramped over 1 ms each end */
typedef struct { long start, len; double amp, amp2; long step_at; } key_t;

static double amp_at(const key_t *k, int nk, long i)
{
    const long r = 48;
    for (int j = 0; j < nk; j++) {
        const long x = i - k[j].start;
        if (x < 0 || x >= k[j].len)
            continue;
        double a = k[j].step_at > 0 && x >= k[j].step_at ? k[j].amp2 : k[j].amp;
        if (x < r)
            a *= (double)x / r;
        if (k[j].len - x <= r)
            a *= (double)(k[j].len - x) / r;
        return a;
    }
    return 0;
}

static void push(rdof_benv_t *e, uint64_t sc, double amp)
{
    const double noise = -log(urand()) * 1e-6; /* exponential power: complex Gaussian noise at -60 dB */
    const double ph = 2 * M_PI * urand();
    const double ni = sqrt(noise) * cos(ph), nq = sqrt(noise) * sin(ph);
    const double yi = amp + ni, yq = nq;
    rdof_benv_push(e, sc, (float)(yi * yi + yq * yq), amp > 0.3 ? 1 : 0);
}

int main(void)
{
    const double amp = 0.1; /* -20 dB */
    /* a 28 ms burst at 100 ms, another at 160 ms; 200 ms of carrier at 300 ms stepping 6 dB down 100 ms in; a
     * carrier from 700 ms on to the end */
    const key_t k[] = {
        {4800, 1344, amp, 0, 0},
        {7680, 1344, amp, 0, 0},
        {14400, 9600, amp, amp / 2, 4800},
        {33600, 100000, amp, 0, 0},
    };
    rdof_benv_t *e = rdof_benv_new(RATE, D, NULL);
    CHECK(e != NULL, "no envelope");
    if (!e)
        return 1;
    rdof_benv_floor(e, -60);
    const uint64_t sc0 = 1000000;
    for (long i = 0; i < 40000; i++)
        push(e, sc0 + (uint64_t)i * D, amp_at(k, 4, i));
    rdof_benv_finish(e);
    rdof_benv_seg_t s[8];
    int n = 0;
    while (n < 8 && rdof_benv_take(e, &s[n]))
        n++;
    CHECK(n == 4, "segments %d, want 4", n);
    const double ms = 1000.0 / RATE / D; /* ms a wideband sample */
    for (int j = 0; j < n && j < 2; j++) {
        const double up = (s[j].up_sc - sc0) * ms, on = (s[j].down_sc - s[j].up_sc) * ms;
        printf("burst %d: up %.3f ms, on %.3f ms, ramps %.3f / %.3f ms, level %.1f dBFS, steps %d\n", j, up, on,
               s[j].ramp_up_ms, s[j].ramp_down_ms, s[j].level_dbfs, s[j].steps);
        CHECK(fabs(up - k[j].start / 48.0) < 0.1, "burst %d up at %.3f ms", j, up);
        CHECK(fabs(on - 27.95) < 0.15, "burst %d on %.3f ms", j, on);
        CHECK(s[j].ramp_up_ms > 0.5 && s[j].ramp_up_ms < 0.95, "burst %d ramp up %.3f", j, s[j].ramp_up_ms);
        CHECK(s[j].ramp_down_ms > 0.5 && s[j].ramp_down_ms < 0.95, "burst %d ramp down %.3f", j, s[j].ramp_down_ms);
        CHECK(fabs(s[j].level_dbfs + 20) < 0.5, "burst %d level %.1f", j, s[j].level_dbfs);
        CHECK(s[j].steps == 0, "burst %d steps %d", j, s[j].steps);
        CHECK(!s[j].open_start && !s[j].open_end, "burst %d open", j);
        CHECK(s[j].clipped == 0 && s[j].inputs > 0, "burst %d clipped %ld of %ld", j, s[j].clipped, s[j].inputs);
    }
    if (n >= 3) {
        const double at = (s[2].step[0].sc - sc0) * ms;
        printf("carrier: on %.1f ms, steps %d (%.2f ms, %+.1f dB), end %.1f dBFS\n",
               (s[2].down_sc - s[2].up_sc) * ms, s[2].steps, at, s[2].step[0].db, s[2].end_dbfs);
        CHECK(s[2].steps == 1, "carrier steps %d", s[2].steps);
        CHECK(fabs(at - 400) < 1.0, "step at %.2f ms", at);
        CHECK(fabs(s[2].step[0].db + 6.0) < 0.6, "step %.1f dB", s[2].step[0].db);
        CHECK(fabs(s[2].end_dbfs + 26) < 0.5, "end level %.1f", s[2].end_dbfs);
        CHECK(fabs(s[2].peak_dbfs + 20) < 0.5 && fabs(s[2].min_dbfs + 26) < 0.6, "peak %.1f min %.1f",
              s[2].peak_dbfs, s[2].min_dbfs);
    }
    if (n >= 4) {
        CHECK(s[3].open_end && !s[3].gap && isnan(s[3].down_sc), "the last carrier isn't open");
        CHECK(s[3].clipped == 0, "clipped");
    }
    const double noise = rdof_benv_noise_db(e);
    CHECK(noise > -62 && noise < -58, "noise %.1f dB", noise);
    rdof_benv_free(e);

    /* keyed from the start, a gap in the middle of a carrier, clipping counted */
    e = rdof_benv_new(RATE, D, NULL);
    rdof_benv_floor(e, -60);
    const key_t k2[] = {{0, 6000, 0.5, 0, 0}};
    for (long i = 0; i < 3000; i++)
        push(e, sc0 + (uint64_t)i * D, amp_at(k2, 1, i));
    for (long i = 3500; i < 9000; i++) /* 500 outputs lost */
        push(e, sc0 + (uint64_t)i * D, amp_at(k2, 1, i));
    rdof_benv_finish(e);
    n = 0;
    while (n < 8 && rdof_benv_take(e, &s[n]))
        n++;
    CHECK(n == 2, "gap: segments %d, want 2", n);
    if (n == 2) {
        CHECK(s[0].open_start && s[0].open_end && s[0].gap, "before the gap: open %d %d gap %d", s[0].open_start,
              s[0].open_end, s[0].gap);
        CHECK(isnan(s[0].ramp_up_ms), "a ramp up with no rise");
        CHECK(s[0].clipped > 0, "clipping not counted");
        CHECK(s[1].open_start && !s[1].open_end && !isnan(s[1].ramp_down_ms), "after the gap: open %d %d",
              s[1].open_start, s[1].open_end);
        CHECK(fabs((s[1].down_sc - sc0) / D - 6000) < 5, "after the gap: down at output %.0f",
              (s[1].down_sc - sc0) / D);
    }
    rdof_benv_free(e);
    printf(fails ? "benchenv: %d FAILED\n" : "benchenv: all passed\n", fails);
    return fails ? 1 : 0;
}
