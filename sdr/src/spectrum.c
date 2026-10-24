// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The display spectrum (rdof/sdr/spectrum.h). */
#include "rdof/sdr/spectrum.h"

#include <math.h>
#include <string.h>

#include "sdr_private.h"

int rdof_spectrum_init(rdof_spectrum_t *s, int n, int per_row)
{
    memset(s, 0, sizeof *s);
    s->n = n;
    s->per_row = per_row < 1 ? 1 : per_row;
    s->fft = rdof_fft_new(n, false);
    s->win = rdof_sdr_alloc(sizeof(float) * (size_t)n);
    s->in = rdof_sdr_alloc(sizeof(rdof_cf_t) * (size_t)n);
    s->out = rdof_sdr_alloc(sizeof(rdof_cf_t) * (size_t)n);
    s->acc = rdof_sdr_zalloc(sizeof(float) * (size_t)n);
    if (!s->fft || !s->win || !s->in || !s->out || !s->acc) {
        rdof_spectrum_free(s);
        return -1;
    }
    double sum = 0;
    for (int i = 0; i < n; i++) {
        s->win[i] = (float)(0.5 - 0.5 * cos(2 * RDOF_PI * i / n));
        sum += s->win[i];
    }
    s->norm = (float)(1.0 / (sum * sum));
    return 0;
}

void rdof_spectrum_free(rdof_spectrum_t *s)
{
    if (s->fft)
        rdof_fft_free(s->fft);
    rdof_sdr_free(s->win);
    rdof_sdr_free(s->in);
    rdof_sdr_free(s->out);
    rdof_sdr_free(s->acc);
    memset(s, 0, sizeof *s);
}

static void one_fft(rdof_spectrum_t *s, const rdof_cf_t *iq, int len)
{
    const int n = s->n;
    int m = len < n ? len : n;
    for (int i = 0; i < m; i++) {
        s->in[i].re = iq[i].re * s->win[i];
        s->in[i].im = iq[i].im * s->win[i];
    }
    for (int i = m; i < n; i++)
        s->in[i].re = s->in[i].im = 0;
    rdof_fft_run(s->fft, s->in, s->out);
    for (int i = 0; i < n; i++)
        s->acc[i] += s->out[i].re * s->out[i].re + s->out[i].im * s->out[i].im;
    s->ffts++;
}

bool rdof_spectrum_feed(rdof_spectrum_t *s, const rdof_cf_t *iq, int len, uint8_t *row)
{
    const int n = s->n;
    int m = len / n;
    m = m < 1 ? 1 : m > RDOF_SPECTRUM_FFTS ? RDOF_SPECTRUM_FFTS : m;
    if (m == 1)
        one_fft(s, iq, len);
    else
        for (int j = 0; j < m; j++)
            one_fft(s, iq + (size_t)j * (size_t)(len - n) / (size_t)(m - 1), n);
    if (++s->count < s->per_row)
        return false;
    const float k = s->norm / (float)s->ffts;
    for (int i = 0; i < n; i++) {
        float p = s->acc[(i + n / 2) % n] * k; /* FFT order to lowest frequency first */
        float v = p > 1e-30f ? -20.0f * log10f(p) : 255.0f; /* -2 x 10 log10 */
        row[i] = v <= 0 ? 0 : v >= 255 ? 255 : (uint8_t)lrintf(v);
    }
    memset(s->acc, 0, sizeof(float) * (size_t)n);
    s->count = s->ffts = 0;
    return true;
}
