// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * The Rate 3/4 trellis code and Rate 1 data (ETSI TS 102 361-1 B.2.4, B.2.5) on 33 byte DMR
 * bursts. Extracted from RadioFoundry core/hotspot/dmrDataFrame.c (1ce780d1), where these are static functions:
 * here they are exported, with the names below, and the burst <-> dmrBurst_t conversion around them is left out.
 *
 * Only the 196 info bits of the burst are read or written (burst bits 0-97 and 166-263).
 */

#ifndef ODR_DMR_TRELLIS_H
#define ODR_DMR_TRELLIS_H

#include <stdint.h>

// 18 payload bytes into the info bits of the burst
void dmrTrellisEncode(const uint8_t *payload, uint8_t *frame);
// Hard decision Viterbi decoder: 18 payload bytes. Returns the number of bit errors on the best path
int dmrTrellisDecode(const uint8_t *frame, uint8_t *payload);
// Rate 1: 24 payload bytes to and from the info bits (info bits 96-99 reserved, sent as 0)
void dmrRate1Encode(const uint8_t *payload, uint8_t *frame);
void dmrRate1Decode(const uint8_t *frame, uint8_t *payload);

#endif
