// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * pffft (pffft.c here, Julien Pommier's, FFTPACK license: LICENSES/FFTPACK-pffft.txt) as an FFT backend for the
 * host tools: rdof_fft_set_backend(rdof_pffft_backend()). It takes the sizes that are a multiple of 16 with no
 * prime factor but 2, 3 and 5 (every channelizer size); others fall back to the built-in FFT. Measured on a
 * PlutoSDR's Cortex-A9 at 667 MHz, 3200 points: 0.55 ms, against the built-in one's 1.8 ms.
 */
#ifndef RDOF_PFFFT_H
#define RDOF_PFFFT_H

#include "rdof/sdr/fft.h"

#ifdef __cplusplus
extern "C" {
#endif

const rdof_fft_backend_t *rdof_pffft_backend(void);
/* "SSE", "NEON", "Altivec" or "scalar": the code pffft was built with */
const char *rdof_pffft_simd(void);

#ifdef __cplusplus
}
#endif

#endif
