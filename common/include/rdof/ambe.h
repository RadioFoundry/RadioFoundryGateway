// SPDX-License-Identifier: MIT AND ISC
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * AMBE+2 channel frames: Golay(23,12) and the frame decode.
 * The tracker only needs the bit errors a frame
 * had (call BER); the 49 parameter bits come out too.
 */
#ifndef RDOF_AMBE_H
#define RDOF_AMBE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t rdof_golay2312_encode(uint32_t data12);
/* Corrected 12 data bits; *errors = data bits corrected (parity bits not counted, as mbelib). */
uint32_t rdof_golay2312_decode(uint32_t word23, int *errors);

/* One 9-byte channel frame: 49 parameter bits (0/1 bytes, may be NULL) and FEC bits corrected. */
int rdof_ambe_decode_frame(const uint8_t frame72[9], uint8_t params49[49]);
void rdof_ambe_encode_frame(const uint8_t params49[49], uint8_t frame72[9]);

#ifdef __cplusplus
}
#endif

#endif
