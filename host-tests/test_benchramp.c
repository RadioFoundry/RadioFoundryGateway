// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * A burst's PA ramp (rdof/benchramp.h) on synthetic 4FSK bursts at 48 kS/s, one slot in two, whose envelope is known:
 * a ramp over 0.3 ms from -60 dB, then a shape that settles to 0 dB, and a fall over 0.3 ms.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/benchramp.h"
#include "rdof/sdr/demod.h"
#include "rdof/sdr/mod.h"
#include "fixtures/mod-vectors/mod_vectors.h"

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
#define NEAR(v, want, tol) CHECK(fabs((double)(v) - (want)) <= (tol), #v " = %.3f, want %.3f +-%.3f", (double)(v), (double)(want), (double)(tol))

#define RATE 48000.0
#define D 8
#define SPS 10
#define BURST_MS 60.0
#define N_BURSTS 6

static unsigned rng = 12345;
static double urand(void)
{
    rng = rng * 1103515245u + 12345u;
    return ((rng >> 8) + 0.5) / 16777216.0;
}
static double nrand(void) { return sqrt(-2 * log(urand())) * cos(2 * M_PI * urand()); }

typedef enum { FLAT, OVER, SAG } shape_t;

static double shape_db(shape_t s, double u)
{
    return s == OVER ? 8 * exp(-u / 0.8) : s == SAG ? -3 * exp(-u / 0.5) : 0;
}

/* The level (dB) at t ms, from a burst whose payload starts at p0 and stops at p1 ms, keyed offset ms from p0 */
static double level_db(double t, double p0, double p1, double offset, shape_t s)
{
    const double up = p0 + offset, down = p1 + 0.1;
    if (t < up - 0.3 || t >= down + 0.3)
        return -200;
    if (t < up)
        return -60 + (t - (up - 0.3)) / 0.3 * (shape_db(s, 0) + 60);
    if (t < down)
        return shape_db(s, t - up);
    return -(t - down) / 0.3 * 60;
}

typedef struct {
    rdof_bramp_t *r;
    uint8_t raw[N_BURSTS][33];
    double sync_sc[N_BURSTS];
    long n;
} run_t;

/* Pushes the bursts' outputs */
static void make(run_t *rn, double offset, shape_t s)
{
    const long n = (long)(RATE * (0.1 + N_BURSTS * BURST_MS / 1000 + 0.1));
    double *fr = calloc((size_t)n, sizeof(double));
    double *lv = malloc(sizeof(double) * (size_t)n);
    for (long i = 0; i < n; i++)
        lv[i] = -200;
    for (int b = 0; b < N_BURSTS; b++) {
        const long p0 = (long)(RATE * (0.1 + b * BURST_MS / 1000));
        for (int i = 0; i < 33; i++)
            rn->raw[b][i] = (uint8_t)(urand() * 256);
        rn->sync_sc[b] = (double)(p0 + 660) * D; /* the sync's center: 66 symbols past the leading edge */
        for (int k = 0; k < 132; k++) {
            const int dib = (rn->raw[b][k / 4] >> (6 - 2 * (k % 4))) & 3;
            const double sy = dib == 1 ? 3 : dib == 0 ? 1 : dib == 2 ? -1 : -3;
            for (int j = 0; j < SPS; j++)
                fr[p0 + k * SPS + j] = sy * 648;
        }
        for (long i = p0 - 30 * SPS; i < p0 + 170 * SPS && i < n; i++) {
            const double l = level_db(i * 1000 / RATE, p0 * 1000 / RATE, (p0 + 1320) * 1000 / RATE, offset, s);
            if (l > lv[i])
                lv[i] = l;
        }
    }
    double ph = 0;
    for (long i = 0; i < n; i++) {
        double f = 0;
        for (int k = -2; k <= 2; k++)
            f += i + k >= 0 && i + k < n ? fr[i + k] / 5 : 0;
        ph += 2 * M_PI * f / RATE;
        const double a = pow(10, lv[i] / 20);
        const float ni = (float)(1e-3 / sqrt(2) * nrand()), nq = (float)(1e-3 / sqrt(2) * nrand());
        rdof_bramp_push(rn->r, (uint64_t)i * D, (float)(a * cos(ph)) + ni, (float)(a * sin(ph)) + nq, 0);
    }
    rn->n = n;
    free(fr);
    free(lv);
}

static void measure(run_t *rn, int b, rdof_bramp_res_t *o)
{
    const int rc = rdof_bramp_measure(rn->r, rn->sync_sc[b], rn->raw[b], -60.0, false, o);
    CHECK(rc == RDOF_BRAMP_OK, "burst %d: rc %d", b, rc);
}

static void test_overshoot(void)
{
    run_t rn = {rdof_bramp_new(RATE, D)};
    make(&rn, -0.5, OVER);
    rdof_bramp_res_t o;
    measure(&rn, 2, &o);
    NEAR(o.align_ms, 0, 0.04);
    NEAR(o.p0_ms, -13.75, 0.1);
    NEAR(o.settled_db, 0, 0.3);
    NEAR(o.rise_ms[5], -0.5 - 0.3 * 11 / 68, 0.06); /* -3 dB on the ramp up to +8 */
    NEAR(o.rise_ms[1], -0.5 - 0.3 * 38 / 68, 0.06); /* -30 dB */
    NEAR(o.overshoot_db, 8, 1.2);
    NEAR(o.settle_1db_ms, -0.5 + 0.8 * log(8), 0.2);
    NEAR(o.sym1_db, 8 * exp(-0.604 / 0.8), 0.5);
    NEAR(o.fall_ms[0], 0.1 + 0.015, 0.06);
    NEAR(o.fall_ms[2], 0.1 + 0.15, 0.06);
    NEAR(o.dev3_hz, 1944, 80);
    NEAR(o.f_first1ms_hz, 0, 60);
    CHECK(!o.f_first1ms_unreliable, "strong carrier from the first symbol");
    NEAR(o.pre_db[0], 0, 4); /* quiet, 5 ms before */
    CHECK(o.pre_db[9] > 40, "the last bin holds the ramp: %.1f", o.pre_db[9]);
    NEAR(o.empty_db[4], 0, 4);
    rdof_bramp_free(rn.r);
}

static void test_sag(void)
{
    run_t rn = {rdof_bramp_new(RATE, D)};
    make(&rn, 0, SAG);
    rdof_bramp_res_t o;
    measure(&rn, 3, &o);
    NEAR(o.sym1_db, -3 * exp(-0.104 / 0.5), 0.5);
    CHECK(o.err5_mean_db < -0.8, "err5 %.2f", o.err5_mean_db);
    NEAR(o.rise_ms[6], 0.5 * log(3), 0.12); /* the last rising through -1 dB, after the sag */
    rdof_bramp_free(rn.r);
}

static void test_late(void)
{
    run_t rn = {rdof_bramp_new(RATE, D)};
    make(&rn, 4.0, FLAT);
    rdof_bramp_res_t o;
    measure(&rn, 1, &o);
    NEAR(o.rise_ms[5], 4.0 - 0.3 * 3 / 60, 0.06);
    NEAR(o.rise_ms[6], 4.0 - 0.3 * 1 / 60, 0.06);
    CHECK(o.sym1_db >= -61.0f, "the floor, not log of nothing: %.1f", o.sym1_db);
    rdof_bramp_free(rn.r);
}

/* Continuous carrier: no edge to claim */
static void test_no_edges(void)
{
    rdof_bramp_t *r = rdof_bramp_new(RATE, D);
    uint8_t raw[33];
    for (int i = 0; i < 33; i++)
        raw[i] = (uint8_t)(urand() * 256);
    double ph = 0;
    const long p0 = 4800;
    for (long i = 0; i < 9600; i++) {
        const long k = (i - p0) / SPS;
        double f = 0;
        if (i >= p0 && k < 132) {
            const int dib = (raw[k / 4] >> (6 - 2 * (k % 4))) & 3;
            f = (dib == 1 ? 3 : dib == 0 ? 1 : dib == 2 ? -1 : -3) * 648.0;
        }
        ph += 2 * M_PI * f / RATE;
        rdof_bramp_push(r, (uint64_t)i * D, (float)cos(ph), (float)sin(ph), 1);
    }
    rdof_bramp_res_t o;
    CHECK(rdof_bramp_measure(r, (p0 + 660.0) * D, raw, -60, false, &o) == RDOF_BRAMP_OK, "measures");
    CHECK(isnan(o.rise_ms[5]) && isnan(o.overshoot_db) && isnan(o.fall_ms[0]), "no edges: rise %.2f over %.2f fall %.2f",
          o.rise_ms[5], o.overshoot_db, o.fall_ms[0]);
    CHECK(o.clipped > 1000, "clipped %ld", o.clipped);
    rdof_bramp_free(r);
}

static void test_wait_and_lost(void)
{
    rdof_bramp_t *r = rdof_bramp_new(RATE, D);
    uint8_t raw[33] = {0};
    rdof_bramp_res_t o;
    CHECK(rdof_bramp_measure(r, 1000, raw, -60, false, &o) == RDOF_BRAMP_WAIT, "nothing yet");
    CHECK(rdof_bramp_measure(r, 1000, raw, -60, true, &o) == RDOF_BRAMP_LOST, "and never will");
    for (long i = 0; i < 2000; i++)
        rdof_bramp_push(r, (uint64_t)i * D, 1, 0, 0);
    CHECK(rdof_bramp_measure(r, 1900.0 * D, raw, -60, false, &o) == RDOF_BRAMP_WAIT, "its end isn't in");
    for (long i = 0; i < 2000; i++) /* a gap: what came before is gone */
        rdof_bramp_push(r, (uint64_t)(100000 + i) * D, 1, 0, 0);
    CHECK(rdof_bramp_measure(r, 1000.0 * D, raw, -60, false, &o) == RDOF_BRAMP_LOST, "before the gap");
    rdof_bramp_free(r);
}

/* The modulator's bursts through the demodulator: the burst's position, less RDOF_DEMOD_LAG, is its first symbol's
 * center, and from there the fit has nothing to move (without it, it starts 0.83 ms late, past its +-0.42 ms reach, and
 * settled on a lobe off by ~0.4 ms or more, by the burst's content). */
static void test_demod_seed(void)
{
    const struct v_mod *v = &v_mods[0];
    CHECK(v->fs == 48000, "tdma48 at 48 kS/s");
    rdof_mod_t m;
    CHECK(rdof_mod_init(&m, v->fs, 0, v->ramp, (float)v->amplitude, v->nseg, 0) == 0, "mod init");
    for (int i = 0; i < v->nseg; i++) {
        const struct v_mod_seg *g = &v_mod_segs[v->first_seg + i];
        CHECK(rdof_mod_queue(&m, g->start, g->sym, g->first, g->count, (unsigned)g->flags) == 0, "queue %d", i);
    }
    rdof_cf_t *x = malloc(sizeof(rdof_cf_t) * (size_t)v->n);
    rdof_mod_iq(&m, x, v->n);
    rdof_mod_free(&m);
    rdof_bramp_t *r = rdof_bramp_new(RATE, 1);
    rdof_demod_t d;
    CHECK(rdof_demod_init(&d, 2, 1, RDOF_DEMOD_SQUELCH_HZ, 2400) == 0, "demod init");
    struct { double sync; uint8_t raw[33]; } pend[16];
    int np = 0, k = 0, measured = 0;
    double worst = 0;
    for (int i = 0; i < v->n; i += 2400) {
        const int n = v->n - i < 2400 ? v->n - i : 2400;
        for (int j = 0; j < n; j++)
            rdof_bramp_push(r, (uint64_t)(i + j), x[i + j].re + (float)(1e-3 * nrand()), x[i + j].im + (float)(1e-3 * nrand()), 0);
        int nb;
        const rdof_sdr_burst_t *bs = rdof_demod_feed(&d, x + i, n, &nb);
        for (int j = 0; j < nb; j++, k++) {
            if (k >= v->nseg || np >= 16)
                continue;
            const long long start = v_mod_segs[v->first_seg + k].start; /* its first symbol's peak */
            CHECK(bs[j].position - RDOF_DEMOD_LAG == start, "burst %d at %lld, first symbol at %lld", k,
                  (long long)bs[j].position, start);
            pend[np].sync = (double)(bs[j].position - RDOF_DEMOD_LAG) + 65.5 * SPS;
            memcpy(pend[np++].raw, bs[j].b.burst, 33);
        }
        const bool last = i + n >= v->n;
        for (int q = 0; q < np;) {
            rdof_bramp_res_t o;
            const int rc = rdof_bramp_measure(r, pend[q].sync, pend[q].raw, -60.0, last, &o);
            if (rc == RDOF_BRAMP_WAIT) {
                q++;
                continue;
            }
            if (rc == RDOF_BRAMP_OK) {
                measured++;
                worst = fmax(worst, fabs(o.align_ms));
                NEAR(o.p0_ms, -(65.5 + 0.5) * SPS / 48.0 + o.align_ms, 0.001);
            }
            pend[q] = pend[--np];
        }
    }
    CHECK(measured >= v->nseg - 1, "%d of %d bursts measured", measured, v->nseg);
    printf("demod seed: %d bursts, the fit moved them at most %.3f ms\n", measured, worst);
    CHECK(worst < 0.03, "the fit moved a burst %.3f ms", worst);
    rdof_demod_free(&d);
    rdof_bramp_free(r);
    free(x);
}

int main(void)
{
    test_overshoot();
    test_sag();
    test_late();
    test_no_edges();
    test_wait_and_lost();
    test_demod_seed();
    printf("%s\n", fails ? "benchramp: FAILED" : "benchramp: ok");
    return fails ? 1 : 0;
}
