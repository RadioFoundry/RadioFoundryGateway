// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Complex float FFT behind a small interface, so a platform can swap in its
 * own (esp-dsp on the ESP32-P4) by building with RDOF_FEATURE_SDR_BUILTIN_FFT=0
 * and defining these three functions.
 *
 * The built-in one is a mixed-radix (4, 2, 3, 5) decimation-in-time FFT in
 * the style of KISS FFT: any size whose only prime factors are 2, 3 and 5.
 * That covers the channelizer at every sample rate the SDR config allows with its
 * default 64-bin output (N = 64 * decimation): 2048 at 1.536 MS/s, 384 at
 * 288 kS/s, 3200 at 2.4 MS/s.
 */
#ifndef RDOF_SDR_FFT_H
#define RDOF_SDR_FFT_H

#include <stdbool.h>
#include <stddef.h>

#include "rdof/sdr/features.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Interleaved re, im: the layout of numpy complex64, FFTW's fftwf_complex and esp-dsp's fc32. */
typedef struct {
    float re, im;
} rdof_cf_t;

typedef struct rdof_fft rdof_fft_t;

/* A plan for size n; inverse is e^{+j...} and unnormalized (like FFTW_BACKWARD).
 * NULL if n isn't supported or out of memory. */
rdof_fft_t *rdof_fft_new(int n, bool inverse);
void rdof_fft_free(rdof_fft_t *f);
/* out = FFT(in). in and out must not overlap. */
void rdof_fft_run(rdof_fft_t *f, const rdof_cf_t *in, rdof_cf_t *out);

#if RDOF_FEATURE_SDR_BUILTIN_FFT
/* A faster FFT for some sizes (esp-dsp's powers of two), tried first by rdof_fft_new; create returns NULL
 * for a size it can't do and the built-in one is used. Set it before creating plans; NULL removes it. */
typedef struct {
    void *(*create)(int n, bool inverse);
    void (*run)(void *plan, const rdof_cf_t *in, rdof_cf_t *out);
    void (*destroy)(void *plan);
} rdof_fft_backend_t;
void rdof_fft_set_backend(const rdof_fft_backend_t *b);
#endif

/* Where sdr allocates (plans, buffers, demod state): malloc and free unless set. Set before init. */
void rdof_sdr_set_allocator(void *(*alloc)(size_t n), void (*release)(void *p));
/* Where the buffers touched for every sample go (the channelizer's FFT input and output, its twiddles, the FFT
 * plan's): the plain allocator unless set. On the S3, internal RAM whatever the reserve: one of them landing in
 * PSRAM cost a 576 kS/s receiver its margin during calls. Freed with the release function. */
void rdof_sdr_set_hot_allocator(void *(*alloc)(size_t n));

#ifdef __cplusplus
}
#endif

#endif
