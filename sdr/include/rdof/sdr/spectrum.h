// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * A spectrum of the whole tuner span for a display (spectrum and waterfall):
 * Hann-windowed n-point FFTs spread across each chunk fed in (up to
 * RDOF_SPECTRUM_FFTS: a pulsed signal, DMR's 30 ms bursts, would strobe
 * against FFTs taken only at each chunk's start), power averaged over a
 * number of chunks, out as one row of n bins, DC in the middle (bin n/2),
 * lowest frequency first. Each bin is a byte: dBFS times -2, so 0 is a full
 * scale tone and 255 is -127.5 dBFS or less.
 */
#ifndef RDOF_SDR_SPECTRUM_H
#define RDOF_SDR_SPECTRUM_H

#include <stdbool.h>
#include <stdint.h>

#include "rdof/sdr/fft.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_SPECTRUM_FFTS 4 /* FFTs a chunk at most */

typedef struct {
    int n;               /* FFT size, and bins a row */
    int per_row;         /* chunks averaged into a row */
    int count;           /* chunks in acc so far */
    int ffts;            /* FFTs in acc so far */
    rdof_fft_t *fft;
    float *win;          /* Hann window */
    float norm;          /* 1 / (sum of the window)^2: a full scale tone reads 1 */
    rdof_cf_t *in, *out;
    float *acc;          /* power, summed */
} rdof_spectrum_t;

/* 0, or -1 if n isn't an FFT size or out of memory. Buffers come from the SDR allocator (rdof_sdr_set_allocator). */
int rdof_spectrum_init(rdof_spectrum_t *s, int n, int per_row);
void rdof_spectrum_free(rdof_spectrum_t *s);
/* Takes FFTs of n samples spread evenly over iq, as many as fit up to RDOF_SPECTRUM_FFTS (len < n: one, zero
 * padded). true when a row is done: its n bytes are in row and the average starts over. */
bool rdof_spectrum_feed(rdof_spectrum_t *s, const rdof_cf_t *iq, int len, uint8_t *row);

#ifdef __cplusplus
}
#endif

#endif
