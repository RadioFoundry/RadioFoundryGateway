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
    s->max_ffts = RDOF_SPECTRUM_FFTS;
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

void rdof_spectrum_set_ffts(rdof_spectrum_t *s, int k)
{
    s->max_ffts = k < 1 ? 1 : k > RDOF_SPECTRUM_FFTS ? RDOF_SPECTRUM_FFTS : k;
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

/* A channel's floor: the mean power of the quietest quarter of its bins within half_hz, so a tone or a carrier in it
 * doesn't lift it */
static double channel_floor(const uint8_t *r, int cn, double cb, double half_hz)
{
    int h[256] = {0}, m = 0;
    for (int j = 0; j < cn; j++)
        if (fabs((j - cn / 2) * cb) <= half_hz)
            h[255 - r[j]]++, m++; /* quietest first: the largest byte */
    const int want = m / 4 > 0 ? m / 4 : 1;
    double p = 0;
    int got = 0;
    for (int b = 0; b < 256 && got < want; b++)
        for (; h[b] && got < want; h[b]--, got++)
            p += pow(10.0, -(255 - b) / 20.0);
    return got ? p / got : 0;
}

void rdof_spectrum_place(uint8_t *row, int n, double center_hz, double rate, const uint8_t *const *ch_rows,
                         const double *ch_hz, int nch, int cn, double ch_rate, double half_hz)
{
    const double wb = rate / n, cb = ch_rate / cn;
    for (int k = 0; k < n; k++) {
        const double lo = center_hz - rate / 2 + k * wb, hi = lo + wb;
        double p = 0, cover = 0; /* power, and the channel bins' width in it (in channel bins) */
        int near = -1;
        for (int c = 0; c < nch; c++) {
            if (ch_hz[c] + half_hz + cb / 2 <= lo || ch_hz[c] - half_hz - cb / 2 >= hi)
                continue;
            for (int j = 0; j < cn; j++) {
                const double fj = (j - cn / 2) * cb;
                if (fabs(fj) > half_hz)
                    continue;
                const double a = fmax(lo, ch_hz[c] + fj - cb / 2), b = fmin(hi, ch_hz[c] + fj + cb / 2);
                if (b > a) {
                    const double w = (b - a) / cb;
                    p += w * pow(10.0, -ch_rows[c][j] / 20.0), cover += w;
                    if (near < 0)
                        near = c;
                }
            }
        }
        if (near < 0) {
            row[k] = 255;
            continue;
        }
        if (wb / cb > cover) /* the rest of the wide bin (between channels) at the channel's floor */
            p += channel_floor(ch_rows[near], cn, cb, half_hz) * (wb / cb - cover);
        const double v = -20 * log10(p + 1e-30); /* -2 x 10 log10 */
        row[k] = v <= 0 ? 0 : v >= 255 ? 255 : (uint8_t)lrint(v);
    }
}

bool rdof_spectrum_feed(rdof_spectrum_t *s, const rdof_cf_t *iq, int len, uint8_t *row)
{
    const int n = s->n;
    const int fit = len / n;
    int m = fit < 1 ? 1 : fit > RDOF_SPECTRUM_FFTS ? RDOF_SPECTRUM_FFTS : fit;
    if (m > s->max_ffts && len > n) {
        /* fewer than fit: k FFTs a stride apart, the first one turning through the stride chunk by chunk (a step
         * of 0.618 of it: no short cycle for a pulse train to lock onto) */
        const int k = s->max_ffts, span = len - n, stride = span / k;
        s->turn += (uint32_t)(stride * 0.618) + 1;
        const int first = stride > 0 ? (int)(s->turn % (uint32_t)stride) : 0;
        for (int j = 0; j < k; j++)
            one_fft(s, iq + first + (size_t)j * (size_t)stride, n);
    } else if (m == 1) {
        one_fft(s, iq, len);
    } else {
        for (int j = 0; j < m; j++)
            one_fft(s, iq + (size_t)j * (size_t)(len - n) / (size_t)(m - 1), n);
    }
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
