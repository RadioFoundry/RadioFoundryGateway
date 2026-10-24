// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * RRC taps and the FM discriminator with its matched filter.
 */
#ifndef RDOF_SDR_FM_H
#define RDOF_SDR_FM_H

#include "rdof/sdr/features.h"
#include "rdof/sdr/fft.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_FM_SPS 10
#define RDOF_FM_TAPS (8 * RDOF_FM_SPS + 1)  /* span 8 symbols */
#define RDOF_FM_DELAY ((RDOF_FM_TAPS - 1) / 2)
#define RDOF_FM_BLOCK 256                 /* samples per inner block (bounds the work buffer) */

/* Root-raised-cosine taps, span symbols long, unit energy: (span * sps + 1) values. */
int rdof_rrc_taps(int sps, double alpha, int span, double *out);

/* A faster FIR for the matched filter (esp-dsp's dsps_fir_f32 on the S3): a causal FIR,
 * y[i] = sum_k h[k] x[i-k], starting from zero history. create returns NULL to decline. */
typedef struct {
    void *(*create)(const float *h, int taps);
    void (*run)(void *state, const float *in, float *out, int n);
    void (*destroy)(void *state);
} rdof_fir_backend_t;
/* Used by every rdof_fm_init after this; NULL for the built-in folded loop. */
void rdof_fm_set_fir_backend(const rdof_fir_backend_t *b);

typedef struct {
    const rdof_fir_backend_t *fir;       /* set: the matched filter runs there */
    void *fir_state;
    float h[RDOF_FM_TAPS];               /* RRC normalized to DC gain 1 */
    float hist[RDOF_FM_TAPS - 1 + RDOF_FM_BLOCK];
    float phist[RDOF_FM_DELAY];
    rdof_cf_t prev;
    float scale;                       /* fs / 2 pi */
    int symmetric;
} rdof_fm_t;

void rdof_fm_init(rdof_fm_t *s, double fs);
/* Releases a FIR backend's state (nothing else is allocated). */
void rdof_fm_free(rdof_fm_t *s);
/* The discriminator alone: n instantaneous frequencies (Hz; scale = fs / 2 pi) following *prev. */
void rdof_fm_discriminate(rdof_cf_t *prev, float scale, const rdof_cf_t *in, int n, float *out);
/* n complex samples in; outf: n filtered frequencies (Hz); outp: n values of |x|^2 delayed by
 * RDOF_FM_DELAY to line up with outf. */
void rdof_fm_process(rdof_fm_t *s, const rdof_cf_t *in, int n, float *outf, float *outp);

#ifdef __cplusplus
}
#endif

#endif
