// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* Biquads, Goertzel and a Welch spectrum (rdof/sdr/filters.h). */
#include "rdof/sdr/filters.h"

#include "rdof/sdr/features.h"
#if RDOF_FEATURE_SDR

#include <math.h>
#include <string.h>

#include "sdr_private.h"

void rdof_bq_set(rdof_bq_t *f, int kind, double hz, double q, double fs)
{
    const double w = 2 * RDOF_PI * hz / fs, cw = cos(w), al = sin(w) / (2 * q), a0 = 1 + al;
    double b0, b1, b2;
    if (kind == RDOF_BQ_LP)
        b0 = (1 - cw) / 2, b1 = 1 - cw, b2 = (1 - cw) / 2;
    else if (kind == RDOF_BQ_HP)
        b0 = (1 + cw) / 2, b1 = -(1 + cw), b2 = (1 + cw) / 2;
    else
        b0 = 1, b1 = -2 * cw, b2 = 1;
    *f = (rdof_bq_t){(float)(b0 / a0), (float)(b1 / a0), (float)(b2 / a0), (float)(-2 * cw / a0),
                     (float)((1 - al) / a0), 0, 0};
}

void rdof_bq_butter(rdof_bq_t *f, int n, int kind, double hz, double fs)
{
    for (int k = 0; k < n; k++) /* each pole pair's Q: 1 / (2 sin((2k + 1) pi / 4n)) */
        rdof_bq_set(&f[k], kind, hz, 1 / (2 * sin((2 * k + 1) * RDOF_PI / (4 * n))), fs);
}

double rdof_bq_mag(const rdof_bq_t *f, int n, double hz, double fs)
{
    const double w = 2 * RDOF_PI * hz / fs, c1 = cos(w), s1 = -sin(w), c2 = cos(2 * w), s2 = -sin(2 * w);
    double m = 1;
    for (int i = 0; i < n; i++) {
        const double nr = f[i].b0 + f[i].b1 * c1 + f[i].b2 * c2, ni = f[i].b1 * s1 + f[i].b2 * s2;
        const double dr = 1 + f[i].a1 * c1 + f[i].a2 * c2, di = f[i].a1 * s1 + f[i].a2 * s2;
        m *= sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
    }
    return m;
}

double rdof_goertzel(const float *x, int n, double hz, double fs)
{
    double mean = 0;
    for (int i = 0; i < n; i++)
        mean += x[i];
    mean /= n;
    const double k = 2 * cos(2 * RDOF_PI * hz / fs);
    double s1 = 0, s2 = 0;
    for (int i = 0; i < n; i++) {
        const double s = (x[i] - mean) + k * s1 - s2;
        s2 = s1, s1 = s;
    }
    return 2 * (s1 * s1 + s2 * s2 - k * s1 * s2) / ((double)n * n);
}

int rdof_psd_init(rdof_psd_t *s, int n)
{
    memset(s, 0, sizeof *s);
    s->n = n;
    s->fft = rdof_fft_new_cold(n, false);
    s->in = rdof_sdr_alloc(sizeof(rdof_cf_t) * (size_t)n);
    s->out = rdof_sdr_alloc(sizeof(rdof_cf_t) * (size_t)n);
    s->win = rdof_sdr_alloc(sizeof(float) * (size_t)n);
    s->acc = rdof_sdr_zalloc(sizeof(double) * (size_t)(n / 2 + 1));
    if (!s->fft || !s->in || !s->out || !s->win || !s->acc) {
        rdof_psd_free(s);
        return -1;
    }
    double w2 = 0;
    for (int i = 0; i < n; i++) {
        s->win[i] = (float)(0.5 - 0.5 * cos(2 * RDOF_PI * i / n));
        w2 += (double)s->win[i] * s->win[i];
    }
    s->norm = 1 / ((double)n * w2);
    return 0;
}

void rdof_psd_free(rdof_psd_t *s)
{
    rdof_fft_free(s->fft);
    rdof_sdr_free(s->in), rdof_sdr_free(s->out), rdof_sdr_free(s->win), rdof_sdr_free(s->acc);
    memset(s, 0, sizeof *s);
}

void rdof_psd_in(rdof_psd_t *s, float x)
{
    s->in[s->fn] = (rdof_cf_t){x * s->win[s->fn], 0};
    if (++s->fn < s->n)
        return;
    s->fn = 0;
    rdof_fft_run(s->fft, s->in, s->out);
    for (int k = 0; k <= s->n / 2; k++)
        s->acc[k] += (k && k < s->n / 2 ? 2.0 : 1.0) *
                     ((double)s->out[k].re * s->out[k].re + (double)s->out[k].im * s->out[k].im) * s->norm;
    s->frames++;
}

#endif
