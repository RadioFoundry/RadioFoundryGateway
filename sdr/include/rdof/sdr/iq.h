// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * IQ sample formats to complex float. The results are exact IEEE single
 * operations, so every platform gives the same floats.
 */
#ifndef RDOF_SDR_IQ_H
#define RDOF_SDR_IQ_H

#include <stddef.h>
#include <stdint.h>

#include "rdof/sdr/fft.h"

#ifdef __cplusplus
extern "C" {
#endif

/* rtl_sdr's unsigned 8-bit: (v - 127.5) / 127.5. n complex samples from 2n bytes. */
void rdof_iq_from_cu8(const uint8_t *in, size_t n, rdof_cf_t *out);
/* Signed 8-bit (HackRF): v / 128. */
void rdof_iq_from_cs8(const int8_t *in, size_t n, rdof_cf_t *out);
/* Signed 16-bit: v / 32768. */
void rdof_iq_from_cs16(const int16_t *in, size_t n, rdof_cf_t *out);

#ifdef __cplusplus
}
#endif

#endif
