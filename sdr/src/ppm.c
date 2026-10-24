// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#include "rdof/sdr/ppm.h"

#include <math.h>
#include <stdlib.h>

void rdof_ppm_init(rdof_ppm_t *p, double chip, double saved)
{
    p->chip = chip;
    p->residual = NAN;
    p->steady_s = 0.0;
    p->last_t = NAN;
    p->channels = 0;
    p->measured = false;
    p->has_saved = !isnan(saved);
    p->saved = saved;
    p->saved_t = NAN;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static double median(double *v, int n)
{
    qsort(v, (size_t)n, sizeof *v, cmp_double);
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

int rdof_ppm_update(rdof_ppm_t *p, const double *corr_hz, const double *freq_hz, const double *age_s, int n, double t)
{
    const double dt = isnan(p->last_t) ? 0.0 : t - p->last_t;
    p->last_t = t;
    p->measured = false;
    double v[64], ok[64];
    int m = 0;
    for (int i = 0; i < n && m < 64; i++)
        if (age_s[i] >= 0.0 && age_s[i] <= RDOF_PPM_FRESH_S && freq_hz[i] > 0.0)
            v[m++] = -corr_hz[i] / freq_hz[i] * 1e6;
    p->channels = 0;
    if (m < RDOF_PPM_MIN_CHANNELS)
        return 0; /* nothing to go on: keep what we have, no more confident */
    for (int i = 0; i < m; i++)
        ok[i] = v[i];
    const double med = median(ok, m);
    int k = 0;
    for (int i = 0; i < m; i++)
        if (fabs(v[i] - med) <= RDOF_PPM_AGREE)
            ok[k++] = v[i];
    if (k < RDOF_PPM_MIN_CHANNELS)
        return 0; /* they disagree: transmitters' own offsets, not the tuner */
    p->channels = k;
    const double est = median(ok, k);
    if (isnan(p->residual) || fabs(est - p->residual) > RDOF_PPM_STEADY) {
        p->residual = est;
        p->steady_s = 0.0;
        return 0;
    }
    p->residual += 0.25 * (est - p->residual);
    p->steady_s += dt;
    if (p->steady_s >= RDOF_PPM_APPLY_S && fabs(p->residual) >= 1.0)
        return (int)lrint(p->residual);
    return 0;
}

int rdof_ppm_measured(rdof_ppm_t *p, double ppm, double t)
{
    const double dt = isnan(p->last_t) ? 0.0 : t - p->last_t;
    p->last_t = t;
    p->channels = 0;
    p->measured = true;
    const double est = ppm - p->chip;
    if (isnan(p->residual) || fabs(est - p->residual) > RDOF_PPM_STEADY) {
        p->residual = est;
        p->steady_s = 0.0;
        return 0;
    }
    p->residual = est; /* already averaged over many minutes */
    p->steady_s += dt;
    if (p->steady_s >= RDOF_PPM_APPLY_S && fabs(p->residual) >= 1.0)
        return (int)lrint(p->residual);
    return 0;
}

void rdof_ppm_applied(rdof_ppm_t *p, int step)
{
    p->chip += step;
    if (!isnan(p->residual))
        p->residual -= step;
}

double rdof_ppm_learned(const rdof_ppm_t *p) { return isnan(p->residual) ? NAN : p->chip + p->residual; }

bool rdof_ppm_should_save(const rdof_ppm_t *p, double t)
{
    const double v = rdof_ppm_learned(p);
    if (isnan(v) || p->steady_s < RDOF_PPM_FIRST_SAVE_S)
        return false;
    if (!p->has_saved)
        return true;
    if (fabs(v - p->saved) < RDOF_PPM_SAVE_MOVE)
        return false;
    /* loaded at boot: the first save of this run may come as soon as it's confident */
    return isnan(p->saved_t) || t - p->saved_t >= RDOF_PPM_SAVE_EVERY_S;
}

void rdof_ppm_saved(rdof_ppm_t *p, double t)
{
    p->has_saved = true;
    p->saved = rdof_ppm_learned(p);
    p->saved_t = t;
}
