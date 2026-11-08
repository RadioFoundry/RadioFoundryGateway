// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * A channel's carrier envelope (rdof/benchenv.h). Everything in linear power, compared against thresholds turned
 * linear once: a log a sample was a fifth of a W12 core at 48 kS/s. Logs only at an edge, a step and the noise
 * figure's 1 ms blocks.
 */
#include "rdof/benchenv.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/benchline.h" /* RDOF_BL_MALLOC */

#define QSEGS 16
#define NHIST 400 /* 0.5 dB bins from -150 dB */
#define MED_MAX 512

struct rdof_benv {
    double rate;
    int d;
    rdof_benv_opts_t o;
    /* lengths in outputs, the analyzer's at 48 kS/s scaled to the rate */
    int w, w1, pl0, pl1, ru_fwd, back, fwd, step_in, step_d, step_dedup, step_arg, minseg, lag;
    int n_ring;               /* a power of two */
    float *p, *env, *env1;    /* [n_ring]: the power, its 0.1 ms and 1 ms centered means */
    uint8_t *clip;
    long n;                   /* outputs pushed since the reset */
    double s5, s48;           /* the running sums */
    uint64_t sc0, last_sc;    /* output 0's wideband sample; the last one's */
    bool have_last;
    /* the decisions */
    bool armed;
    double floor_db;
    float lo, on, off;        /* linear thresholds */
    long m;                   /* the next output to decide, -1: from the first one that can be */
    long m_first;             /* the first decided since the reset or the arming (a carrier on there was on before) */
    bool keyed;
    rdof_benv_seg_t cur;
    long a;                   /* the key-up's output */
    long last_hit;
    long nhist[NHIST];
    long nblocks;
    rdof_benv_seg_t q[QSEGS];
    int qh, qn;
    float med[MED_MAX];       /* a median's values (off the caller's stack) */
};

static long scale(double rate, int n48)
{
    const long v = lround(n48 * rate / 48000.0);
    return v < 1 ? 1 : v;
}

rdof_benv_t *rdof_benv_new(double rate, int d, const rdof_benv_opts_t *o)
{
    if (!(rate > 1000) || d < 1)
        return NULL;
    rdof_benv_t *e = calloc(1, sizeof *e);
    if (!e)
        return NULL;
    e->rate = rate, e->d = d;
    e->o = o ? *o : (rdof_benv_opts_t){10, 7, 6, 3};
    e->w = (int)scale(rate, 5), e->w1 = (int)scale(rate, 48);
    e->pl0 = (int)scale(rate, 48), e->pl1 = (int)scale(rate, 144), e->ru_fwd = (int)scale(rate, 480);
    e->back = (int)scale(rate, 720), e->fwd = (int)scale(rate, 720);
    e->step_in = (int)scale(rate, 96), e->step_d = (int)scale(rate, 48), e->step_dedup = (int)scale(rate, 240);
    e->step_arg = (int)scale(rate, 96), e->minseg = (int)scale(rate, 24);
    e->lag = e->fwd > e->step_arg + e->step_d ? e->fwd : e->step_arg + e->step_d;
    int need = 2 * (e->lag + e->back) + e->w1 + 64, n = 256;
    while (n < need)
        n <<= 1;
    e->n_ring = n;
    e->p = RDOF_BL_MALLOC(sizeof(float) * (size_t)n);
    e->env = RDOF_BL_MALLOC(sizeof(float) * (size_t)n);
    e->env1 = RDOF_BL_MALLOC(sizeof(float) * (size_t)n);
    e->clip = RDOF_BL_MALLOC((size_t)n);
    if (!e->p || !e->env || !e->env1 || !e->clip) {
        rdof_benv_free(e);
        return NULL;
    }
    e->m = -1;
    return e;
}

void rdof_benv_free(rdof_benv_t *e)
{
    if (!e)
        return;
    free(e->p);
    free(e->env);
    free(e->env1);
    free(e->clip);
    free(e);
}

#define R(e, i) ((size_t)((i) & ((e)->n_ring - 1)))

static float P(const rdof_benv_t *e, long i) { return e->p[R(e, i)]; }
static float E(const rdof_benv_t *e, long i) { return e->env[R(e, i)]; }
static float E1(const rdof_benv_t *e, long i) { return e->env1[R(e, i)]; }

static float db(float v) { return 10 * log10f(v + 1e-12f); }

/* The newest output whose 1 ms mean is in (the 0.1 ms one is in before it) */
static long newest_env(const rdof_benv_t *e) { return e->n - 1 - (e->w1 - 1 - e->w1 / 2); }
/* The oldest output still in the ring with its means */
static long oldest_env(const rdof_benv_t *e)
{
    const long o = e->n - e->n_ring + e->w1;
    return o > e->w1 ? o : e->w1;
}

static double sc_at(const rdof_benv_t *e, double i) { return (double)e->sc0 + i * e->d; }

void rdof_benv_floor(rdof_benv_t *e, double floor_db)
{
    if (!e || !isfinite(floor_db))
        return;
    e->floor_db = floor_db;
    const double f = pow(10, floor_db / 10);
    e->lo = (float)(f * pow(10, e->o.edge_db / 10));
    e->on = (float)(f * pow(10, e->o.on_db / 10));
    e->off = (float)(f * pow(10, e->o.off_db / 10));
    if (!e->armed)
        e->m = -1;
    e->armed = true;
}

static float median(rdof_benv_t *e, long from, long to)
{
    float *v = e->med;
    int n = 0;
    for (long i = from; i < to && n < MED_MAX; i++) {
        const float x = E(e, i);
        int k = n++;
        while (k > 0 && v[k - 1] > x) /* insertion: under a hundred values */
            v[k] = v[k - 1], k--;
        v[k] = x;
    }
    if (!n)
        return NAN;
    return n % 2 ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
}

/* Where the envelope crosses th (linear) between outputs i - 1 and i: interpolated in dB, as the analyzer does */
static double cross(const rdof_benv_t *e, long i, float th)
{
    const float a = db(E(e, i - 1)), b = db(E(e, i)), t = db(th);
    return b != a ? (double)(i - 1) + (t - a) / (b - a) : (double)i;
}

static void key_up(rdof_benv_t *e, long m, long hi)
{
    rdof_benv_seg_t *s = &e->cur;
    memset(s, 0, sizeof *s);
    s->down_sc = NAN;
    s->ramp_up_ms = s->ramp_down_ms = NAN;
    s->peak_dbfs = -INFINITY, s->min_dbfs = INFINITY;
    e->a = m;
    e->keyed = true;
    e->last_hit = LONG_MIN / 2; /* long is 32 bits on an ESP32 */
    const long lo_i = oldest_env(e);
    float plat = median(e, m + e->pl0, m + e->pl1 < hi ? m + e->pl1 : hi);
    if (!isfinite(plat))
        plat = E(e, m);
    s->level_dbfs = db(plat);
    if (m == e->m_first || m - 1 < lo_i) { /* on before the envelope could see it rise */
        s->open_start = true;
        s->up_sc = sc_at(e, (double)m);
        return;
    }
    s->up_sc = sc_at(e, cross(e, m, e->on));
    long i0 = m;
    const long lim = m - e->back > lo_i ? m - e->back : lo_i;
    while (i0 > lim && E(e, i0) > e->lo)
        i0--;
    if (E(e, i0) > e->lo)
        return; /* didn't come up from the noise: no ramp */
    long i1 = m;
    const long top = m + e->ru_fwd < hi ? m + e->ru_fwd : hi;
    while (i1 < top && E(e, i1) < plat * 0.5012f)
        i1++;
    s->ramp_up_ms = (float)((i1 - i0) / e->rate * 1000);
}

static void put_seg(rdof_benv_t *e)
{
    if (e->qn == QSEGS) /* the reader is behind: the oldest goes */
        e->qh = (e->qh + 1) % QSEGS, e->qn--;
    e->q[(e->qh + e->qn++) % QSEGS] = e->cur;
}

static void key_down(rdof_benv_t *e, long b, long hi, bool gap)
{
    rdof_benv_seg_t *s = &e->cur;
    e->keyed = false;
    if (gap || b < 0) { /* still keyed at the end */
        s->open_end = true, s->gap = gap;
        put_seg(e);
        return;
    }
    if (b - e->a < e->minseg)
        return; /* under 0.5 ms: a blip */
    s->down_sc = sc_at(e, cross(e, b, e->off));
    const long from = b - e->pl1 > e->a ? b - e->pl1 : e->a, to = b - e->pl0 > from ? b - e->pl0 : from + 1;
    float plat = median(e, from, to);
    if (!isfinite(plat))
        plat = E(e, b - 1);
    s->end_dbfs = db(plat);
    long i1 = b;
    const long top = b + e->fwd < hi ? b + e->fwd : hi;
    while (i1 < top && E(e, i1) > e->lo)
        i1++;
    if (E(e, i1) <= e->lo) {
        long i0 = b;
        const long lim = b - e->ru_fwd > e->a ? b - e->ru_fwd : e->a;
        while (i0 > lim && E(e, i0) < plat * 0.5012f)
            i0--;
        s->ramp_down_ms = (float)((i1 - i0) / e->rate * 1000);
    }
    /* steps found past the analyzer's window (2 ms before the key-down) go */
    int k = 0;
    const int kept = s->steps < RDOF_BENV_STEPS ? s->steps : RDOF_BENV_STEPS;
    for (int i = 0; i < kept; i++) {
        const long j = (long)llround((s->step[i].sc - (double)e->sc0) / e->d) - e->step_d / 2;
        if (j + e->step_d < b - e->step_in)
            s->step[k++] = s->step[i];
    }
    s->steps -= kept - k;
    put_seg(e);
}

static void decide(rdof_benv_t *e, long m, long hi)
{
    const float v = E(e, m);
    if (!e->keyed) {
        if (v > e->on)
            key_up(e, m, hi);
        return;
    }
    rdof_benv_seg_t *s = &e->cur;
    if (v < e->off) {
        key_down(e, m, hi, false);
        return;
    }
    e->cur.clipped += e->clip[R(e, m)];
    e->cur.inputs += 2L * e->d;
    const long q = m - e->step_in; /* 2 ms in from the key-up and, once decided, from the key-down */
    if (q >= e->a + e->step_in) {
        const float x = E1(e, q);
        if (x > 0) {
            const float xd = db(x); /* once an output in a segment: affordable */
            s->peak_dbfs = xd > s->peak_dbfs ? xd : s->peak_dbfs;
            s->min_dbfs = xd < s->min_dbfs ? xd : s->min_dbfs;
        }
    }
    /* a step: the 1 ms envelope 1 ms apart differs by over step_db, from 2 ms in */
    if (m >= e->a + e->step_in && m - e->last_hit >= e->step_dedup && m + e->step_arg + e->step_d <= hi) {
        const float r = E1(e, m + e->step_d) / (E1(e, m) + 1e-30f), k = powf(10, (float)e->o.step_db / 10);
        if (r > k || r < 1 / k) {
            e->last_hit = m;
            long j = m;
            float best = 0;
            for (long i = m; i < m + e->step_arg; i++) {
                const float dd = fabsf(db(E1(e, i + e->step_d)) - db(E1(e, i)));
                if (dd > best)
                    best = dd, j = i;
            }
            if (s->steps < RDOF_BENV_STEPS) {
                s->step[s->steps].sc = sc_at(e, (double)(j + e->step_d / 2));
                s->step[s->steps].db = db(E1(e, j + e->step_d)) - db(E1(e, j));
            }
            s->steps++;
        }
    }
}

/* Decide every output that has its look-ahead in (all of them at the end) */
static void run(rdof_benv_t *e, bool all)
{
    const long newest = newest_env(e);
    if (!e->armed || newest < e->w1)
        return;
    if (e->m < 0) { /* from the oldest the ring still holds with its look-back, or the first */
        long m = newest - e->lag - e->back;
        if (m < oldest_env(e))
            m = oldest_env(e);
        e->m = e->m_first = m;
    }
    const long last = all ? newest : newest - e->lag;
    for (; e->m <= last; e->m++)
        decide(e, e->m, newest);
}

static void finish(rdof_benv_t *e, bool gap)
{
    run(e, true);
    if (e->keyed)
        key_down(e, -1, newest_env(e), gap);
    e->n = 0, e->s5 = e->s48 = 0, e->have_last = false, e->m = -1, e->keyed = false;
}

void rdof_benv_push(rdof_benv_t *e, uint64_t sc, float p, int clipped)
{
    if (!e)
        return;
    if (e->have_last && sc != e->last_sc + (uint64_t)e->d)
        finish(e, true);
    if (!e->n)
        e->sc0 = sc;
    e->last_sc = sc, e->have_last = true;
    const long n = e->n;
    e->p[R(e, n)] = p;
    e->clip[R(e, n)] = (uint8_t)(clipped > 255 ? 255 : clipped);
    e->s5 += p - (n >= e->w ? P(e, n - e->w) : 0);
    e->s48 += p - (n >= e->w1 ? P(e, n - e->w1) : 0);
    if ((n & 4095) == 4095) { /* the running sums exact again */
        double a = 0, b = 0;
        for (long i = n - e->w1 + 1; i <= n; i++)
            b += i >= 0 ? P(e, i) : 0;
        for (long i = n - e->w + 1; i <= n; i++)
            a += i >= 0 ? P(e, i) : 0;
        e->s5 = a, e->s48 = b;
    }
    e->n++;
    /* the centered means: the 0.1 ms one's at n - (w - 1 - w/2), the 1 ms one's at n - (w1 - 1 - w1/2) */
    const long i5 = n - (e->w - 1 - e->w / 2), i48 = n - (e->w1 - 1 - e->w1 / 2);
    if (i5 >= 0)
        e->env[R(e, i5)] = (float)(e->s5 / e->w);
    if (i48 >= 0) {
        const float v = (float)(e->s48 / e->w1);
        e->env1[R(e, i48)] = v;
        if (i48 >= e->w1 && i48 % e->w1 == 0) { /* the noise figure: a 1 ms block's */
            int k = (int)((db(v) + 150) * 2);
            e->nhist[k < 0 ? 0 : k >= NHIST ? NHIST - 1 : k]++;
            e->nblocks++;
        }
    }
    run(e, false);
}

void rdof_benv_finish(rdof_benv_t *e)
{
    if (e)
        finish(e, false);
}

bool rdof_benv_take(rdof_benv_t *e, rdof_benv_seg_t *out)
{
    if (!e || !e->qn)
        return false;
    *out = e->q[e->qh];
    e->qh = (e->qh + 1) % QSEGS, e->qn--;
    return true;
}

double rdof_benv_noise_db(const rdof_benv_t *e)
{
    if (!e || !e->nblocks)
        return NAN;
    long acc = 0;
    int k = 0;
    while (k < NHIST - 1 && (acc += e->nhist[k]) < e->nblocks / 10 + 1)
        k++;
    return -150 + k * 0.5 + 0.25;
}

uint64_t rdof_benv_lag(const rdof_benv_t *e)
{
    return e ? (uint64_t)(e->lag + e->w1) * (uint64_t)e->d : 0;
}
