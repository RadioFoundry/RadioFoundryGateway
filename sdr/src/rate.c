// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#include "rdof/sdr/rate.h"

#include <math.h>
#include <string.h>

void rdof_rate_bin_init(rdof_rate_bin_t *b, double nominal)
{
    memset(b, 0, sizeof *b);
    b->nominal = nominal;
}

bool rdof_rate_bin_add(rdof_rate_bin_t *b, int64_t t_us, uint64_t n, double *t_s, double *x_us)
{
    if (!b->started) {
        b->started = true;
        b->t0 = t_us;
        b->n0 = n;
        b->bin_end = t_us + RDOF_RATE_BIN_S * 1000000LL;
    }
    const double t = (double)(t_us - b->t0);
    if (b->rebase) {
        b->rebase = false;
        b->n0 = n;
        b->x0 = t; /* x is 0 at the segment's first block */
    }
    const double x = t - b->x0 - (double)(n - b->n0) * 1e6 / b->nominal;
    bool closed = false;
    if (t_us >= b->bin_end) {
        if (b->any) {
            *t_s = b->min_t;
            *x_us = b->min_x;
            closed = true;
        }
        const int64_t bin = RDOF_RATE_BIN_S * 1000000LL;
        b->bin_end += (t_us - b->bin_end) / bin * bin + bin;
        b->any = false;
    }
    if (!b->any || x < b->min_x) {
        b->any = true;
        b->min_x = x;
        b->min_t = t / 1e6;
    }
    return closed;
}

void rdof_rate_bin_break(rdof_rate_bin_t *b)
{
    b->any = false;
    b->rebase = true;
    b->seg++;
}

void rdof_rate_init(rdof_rate_t *r) { memset(r, 0, sizeof *r); }

#define AT(r, i) (((r)->head + (i)) % RDOF_RATE_POINTS)

/* The segment that starts at the i-th oldest point (a segment's points are consecutive) ends before the returned
 * one, at most end; its mean time and x. */
static int seg_run(const rdof_rate_t *r, int i, int end, double *tm, double *xm)
{
    double st = 0, sx = 0;
    int j = i;
    for (; j < end && r->seg[AT(r, j)] == r->seg[AT(r, i)]; j++) {
        st += r->t[AT(r, j)];
        sx += r->x[AT(r, j)];
    }
    *tm = st / (j - i);
    *xm = sx / (j - i);
    return j;
}

/* The least-squares line through n points from the i0-th oldest, one slope (us a second) through every segment,
 * each with its own intercept: the last segment's xm at tm (its mean time), the sum of squared residuals and of
 * squared time deviations. The number of segments. */
static int fit(const rdof_rate_t *r, int i0, int n, double *slope, double *tm, double *xm, double *ss, double *sxx)
{
    const int end = i0 + n;
    double sxy = 0;
    int segs = 0;
    *sxx = 0;
    for (int i = i0; i < end; segs++) {
        const int j = seg_run(r, i, end, tm, xm);
        for (; i < j; i++) {
            const double dt = r->t[AT(r, i)] - *tm;
            *sxx += dt * dt;
            sxy += dt * (r->x[AT(r, i)] - *xm);
        }
    }
    *slope = *sxx > 0 ? sxy / *sxx : 0;
    *ss = 0;
    for (int i = i0; i < end;) {
        const int j = seg_run(r, i, end, tm, xm);
        for (; i < j; i++) {
            const double e = r->x[AT(r, i)] - (*xm + *slope * (r->t[AT(r, i)] - *tm));
            *ss += e * e;
        }
    }
    return segs; /* tm, xm: the last segment's */
}

/* The estimate from every point kept, once there are enough. */
static void refit(rdof_rate_t *r)
{
    r->span_s = r->count ? r->t[(r->head + r->count - 1) % RDOF_RATE_POINTS] - r->t[r->head] : 0.0;
    if (r->count < RDOF_RATE_MIN_POINTS)
        return; /* keep what we had, if anything */
    double b, tm, xm, ss, sxx;
    const int segs = fit(r, 0, r->count, &b, &tm, &xm, &ss, &sxx);
    const int dof = r->count - segs - 1; /* a slope, and an intercept a segment */
    if (dof < RDOF_RATE_MIN_POINTS - 2)
        return; /* segments too short to say */
    /* x falls by e us a second when the samples come e ppm fast */
    r->ppm = -b;
    r->rms_us = sqrt(ss / r->count);
    r->se_ppm = sxx > 0 ? sqrt(ss / dof / sxx) : INFINITY;
    r->valid = fabs(r->ppm) <= RDOF_RATE_MAX_PPM;
}

void rdof_rate_point(rdof_rate_t *r, double t_s, double x_us)
{
    rdof_rate_point_seg(r, t_s, x_us, r->count ? r->seg[AT(r, r->count - 1)] : 0);
}

void rdof_rate_point_seg(rdof_rate_t *r, double t_s, double x_us, uint32_t seg)
{
    double b, tm, xm, ss, sxx;
    if (r->count >= 3 && r->seg[AT(r, r->count - 1)] == seg) {
        fit(r, 0, r->count, &b, &tm, &xm, &ss, &sxx);
        if (fabs(x_us - (xm + b * (t_s - tm))) > RDOF_RATE_STEP_US) {
            r->steps++;
            r->head = r->count = 0;
        }
    }
    if (r->count == RDOF_RATE_POINTS) {
        r->head = (r->head + 1) % RDOF_RATE_POINTS;
        r->count--;
    }
    const int k = (r->head + r->count) % RDOF_RATE_POINTS;
    r->t[k] = t_s;
    r->x[k] = x_us;
    r->seg[k] = seg;
    r->count++;
    refit(r);
}

bool rdof_rate_recent(const rdof_rate_t *r, double span_s, double *ppm)
{
    if (r->count < RDOF_RATE_MIN_POINTS)
        return false;
    const double from = r->t[(r->head + r->count - 1) % RDOF_RATE_POINTS] - span_s;
    int i0 = 0;
    while (i0 < r->count && r->t[(r->head + i0) % RDOF_RATE_POINTS] < from)
        i0++;
    if (r->count - i0 < RDOF_RATE_MIN_POINTS)
        return false;
    double b, tm, xm, ss, sxx;
    if (r->count - i0 - fit(r, i0, r->count - i0, &b, &tm, &xm, &ss, &sxx) - 1 < RDOF_RATE_MIN_POINTS - 2)
        return false;
    *ppm = -b;
    return true;
}

void rdof_rate_trim(rdof_rate_t *r, double t_s)
{
    while (r->count && r->t[r->head] < t_s) {
        r->head = (r->head + 1) % RDOF_RATE_POINTS;
        r->count--;
    }
    refit(r);
}
