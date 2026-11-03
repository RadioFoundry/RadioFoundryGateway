// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Small blocks the signal modes share, beside the channelizer (channelizer.h), the discriminator (fm.h,
 * rdof_fm_discriminate) and the FFT (fft.h, which a platform may replace): biquad filters with Butterworth design,
 * a Goertzel tone power, and a Welch power spectrum of a real stream on rdof_fft.
 */
#ifndef RDOF_SDR_FILTERS_H
#define RDOF_SDR_FILTERS_H

#include <stdbool.h>
#include <stddef.h>

#include "rdof/sdr/fft.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float b0, b1, b2, a1, a2, z1, z2;
} rdof_bq_t;

enum { RDOF_BQ_LP, RDOF_BQ_HP, RDOF_BQ_NOTCH };

/* RBJ's cookbook: a low-pass, high-pass or notch at hz, quality q, rate fs (its state cleared). */
void rdof_bq_set(rdof_bq_t *f, int kind, double hz, double q, double fs);
/* A Butterworth low-pass or high-pass of order 2n, as n sections. */
void rdof_bq_butter(rdof_bq_t *f, int n, int kind, double hz, double fs);
/* |H| at hz of n sections in a row. */
double rdof_bq_mag(const rdof_bq_t *f, int n, double hz, double fs);

static inline float rdof_bq(rdof_bq_t *f, float x)
{
    const float y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

/* The power at hz in n samples, their mean taken out: a tone of amplitude A gives A^2 / 2. */
double rdof_goertzel(const float *x, int n, double hz, double fs);

/* The mean power spectrum of a real stream: Hann frames of n (an FFT size), back to back, one-sided, so a tone of
 * amplitude A sums to A^2 / 2 over its bins. Its buffers and plan from the SDR's plain allocator (off the sample
 * path's internal RAM). */
typedef struct {
    int n, fn;
    long frames;
    rdof_fft_t *fft;
    rdof_cf_t *in, *out;
    float *win;
    double *acc;         /* n / 2 + 1 sums */
    double norm;
} rdof_psd_t;

int rdof_psd_init(rdof_psd_t *s, int n);
void rdof_psd_free(rdof_psd_t *s);
void rdof_psd_in(rdof_psd_t *s, float x);
/* Bin k's mean power (0 before a frame). */
static inline double rdof_psd_bin(const rdof_psd_t *s, int k) { return s->frames ? s->acc[k] / (double)s->frames : 0; }

#ifdef __cplusplus
}
#endif

#endif
