// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Bit helpers. Bursts are packed bytes, MSB first. Bit fields
 * are arrays of uint8_t holding 0 or 1.
 */
#ifndef RDOF_BITS_H
#define RDOF_BITS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void rdof_bytes_to_bits(const uint8_t *in, size_t nbytes, uint8_t *bits);
/* Packs nbits into (nbits + 7) / 8 bytes; the last byte is zero padded. */
void rdof_bits_to_bytes(const uint8_t *bits, size_t nbits, uint8_t *out);
uint64_t rdof_bits_to_u64(const uint8_t *bits, int n);
void rdof_u64_to_bits(uint64_t value, int width, uint8_t *bits);
/* n (<= 64) bits from packed bytes, starting at bit `start`, MSB first. */
uint64_t rdof_get_field(const uint8_t *bytes, int start, int n);
void rdof_set_field(uint8_t *bytes, int start, int n, uint64_t value);
int rdof_popcount64(uint64_t v);
uint32_t rdof_be24(const uint8_t *p);
uint32_t rdof_be32(const uint8_t *p);
void rdof_put_be24(uint8_t *p, uint32_t v);
void rdof_put_be32(uint8_t *p, uint32_t v);

#ifdef __cplusplus
}
#endif

#endif
