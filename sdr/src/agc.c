// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* Clip measurement, the gain control law and level stats. */
#include "rdof/sdr/agc.h"

#if RDOF_FEATURE_SDR

#include <math.h>
#include <string.h>

void rdof_agc_measure(const rdof_cf_t *x, size_t n, double *peak, double *clipped)
{
    float pk = 0.0f;
    size_t clip = 0;
    for (size_t i = 0; i < n; i++) {
        float a = fabsf(x[i].re), b = fabsf(x[i].im);
        pk = a > pk ? a : pk;
        pk = b > pk ? b : pk;
        clip += (a >= RDOF_AGC_FULL_SCALE) + (b >= RDOF_AGC_FULL_SCALE);
    }
    *peak = pk;
    *clipped = n ? (double)clip / (double)(2 * n) : 0.0;
}

void rdof_agc_measure_cu8(const uint8_t *x, size_t n, double *peak, double *clipped)
{
    /* |(v - 127.5) / 127.5| grows with |v - 127.5|: find the extreme byte, count the clipped ones */
    int far = 0;
    size_t clip = 0;
    for (size_t i = 0; i < 2 * n; i++) {
        int d = 2 * (int)x[i] - 255; /* 2 (v - 127.5) */
        d = d < 0 ? -d : d;
        far = d > far ? d : far;
        clip += x[i] <= 1 || x[i] >= 254; /* the bytes whose float is >= 0.99 */
    }
    *peak = n ? fabsf(((float)((far + 255) / 2) - 127.5f) / 127.5f) : 0.0;
    *clipped = n ? (double)clip / (double)(2 * n) : 0.0;
}

#if RDOF_FEATURE_SDR_AGC

void rdof_agc_config_default(rdof_agc_config_t *c)
{
    c->min = 0.0;
    c->clip = 1e-4;
    c->headroom_db = 6.0;
    c->hold_s = 10.0;
    c->relax_s = 300.0;
    c->settle_s = 0.3;
}

static double at_or_below(const rdof_agc_t *a, double g)
{
    double r = a->steps[0];
    for (int i = 0; i < a->nsteps; i++)
        if (a->steps[i] <= g + 1e-6)
            r = a->steps[i];
    return r;
}

static bool above(const rdof_agc_t *a, double g, double *out)
{
    for (int i = 0; i < a->nsteps; i++)
        if (a->steps[i] > g + 1e-6) {
            *out = a->steps[i];
            return true;
        }
    return false;
}

static double below(const rdof_agc_t *a, double g)
{
    double r = a->steps[0];
    for (int i = 0; i < a->nsteps; i++)
        if (a->steps[i] < g - 1e-6)
            r = a->steps[i];
    return r;
}

void rdof_agc_init(rdof_agc_t *a, const rdof_agc_config_t *cfg, const double *steps, int nsteps, double max_gain,
                 double gain)
{
    memset(a, 0, sizeof *a);
    a->cfg = *cfg;
    for (int i = 0; i < nsteps && a->nsteps < RDOF_AGC_MAX_STEPS; i++)
        if (cfg->min - 1e-6 <= steps[i] && steps[i] <= max_gain + 1e-6)
            a->steps[a->nsteps++] = steps[i];
    for (int i = 1; i < a->nsteps; i++) { /* sort */
        double v = a->steps[i];
        int j = i - 1;
        while (j >= 0 && a->steps[j] > v) {
            a->steps[j + 1] = a->steps[j];
            j--;
        }
        a->steps[j + 1] = v;
    }
    if (!a->nsteps)
        a->steps[a->nsteps++] = max_gain;
    a->max = a->steps[a->nsteps - 1];
    a->gain = at_or_below(a, isnan(gain) ? a->max : gain);
    a->ceiling = a->max;
    a->settle_until = -1.0;
    a->last_clip = -INFINITY;
    a->relaxed = -INFINITY;
}

static void peaks_clear(rdof_agc_t *a) { a->peak_head = a->peak_n = 0; }

static void peaks_push(rdof_agc_t *a, double t, double p)
{
    /* drop older entries that can no longer be the maximum */
    while (a->peak_n && a->peak_v[(a->peak_head + a->peak_n - 1) % RDOF_AGC_MAX_PEAKS] <= p)
        a->peak_n--;
    if (a->peak_n == RDOF_AGC_MAX_PEAKS) {
        /* Full (peaks falling for 256 chunks in one window): keep the larger peak a little longer
         * instead. The window maximum can only read high, so the gain goes up later, never sooner. */
        a->peak_t[(a->peak_head + a->peak_n - 1) % RDOF_AGC_MAX_PEAKS] = t;
        return;
    }
    int k = (a->peak_head + a->peak_n) % RDOF_AGC_MAX_PEAKS;
    a->peak_t[k] = t;
    a->peak_v[k] = p;
    a->peak_n++;
}

static bool set_gain(rdof_agc_t *a, double g, double t, double *out)
{
    if (fabs(g - a->gain) < 1e-6)
        return false;
    a->gain = g;
    a->changes++;
    a->settle_until = t + a->cfg.settle_s;
    peaks_clear(a);
    *out = g;
    return true;
}

bool rdof_agc_update(rdof_agc_t *a, double peak, double clipped, double t, double *new_gain)
{
    if (t < a->settle_until)
        return false;
    if (clipped > a->cfg.clip) {
        a->last_clip = a->relaxed = t;
        a->quiet = false;
        peaks_clear(a);
        double g = below(a, a->gain);
        a->ceiling = g;
        return set_gain(a, g, t, new_gain);
    }
    peaks_push(a, t, peak);
    while (a->peak_n && a->peak_t[a->peak_head] < t - a->cfg.hold_s) {
        a->peak_head = (a->peak_head + 1) % RDOF_AGC_MAX_PEAKS;
        a->peak_n--;
    }
    if (!a->quiet) {
        a->quiet = true;
        a->quiet_since = t;
    }
    if (a->ceiling < a->max && t - a->relaxed >= a->cfg.relax_s) {
        double up;
        a->ceiling = above(a, a->ceiling, &up) ? up : a->max;
        a->relaxed = t;
    }
    double up;
    if (!above(a, a->gain, &up) || up > a->ceiling + 1e-6 || t - a->quiet_since < a->cfg.hold_s)
        return false;
    double window_peak = a->peak_n ? a->peak_v[a->peak_head] : peak;
    if (window_peak * pow(10.0, (up - a->gain + a->cfg.headroom_db) / 20.0) >= (double)RDOF_AGC_FULL_SCALE_D)
        return false;
    a->quiet_since = t;
    return set_gain(a, up, t, new_gain);
}

#endif /* RDOF_FEATURE_SDR_AGC */

void rdof_level_stats_add(rdof_level_stats_t *s, size_t n, double peak, double clipped)
{
    s->samples += (double)n;
    s->clipped += clipped * (double)n;
    s->peak = peak > s->peak ? peak : s->peak;
}

void rdof_level_stats_take(rdof_level_stats_t *s, double *clip_pct, double *peak_dbfs)
{
    *clip_pct = s->samples > 0 ? nearbyint(100.0 * s->clipped / s->samples * 1000.0) / 1000.0 : NAN;
    *peak_dbfs = s->peak > 0 ? nearbyint(20.0 * log10(s->peak) * 10.0) / 10.0 : NAN;
    s->samples = s->clipped = s->peak = 0.0;
}

#endif
