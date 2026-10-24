// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * DMR FEC and CRCs (ETSI TS 102 361-1 annex B), implemented over
 * RadioFoundry's code in src/radiofoundry/ (fec.c). Bursts are 33 packed bytes; "info" and other bit fields
 * are arrays of 0/1 bytes, one bit each.
 */
#ifndef RDOF_FEC_H
#define RDOF_FEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "rdof/features.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_BURST_BYTES 33

/* Hamming (15,11,3), (13,9,3) and (16,11,4) on n 0/1 bytes, data first: decode corrects one bit in place. */
typedef struct rdof_hamming rdof_hamming_t;
extern const rdof_hamming_t *const RDOF_H_15_11;
extern const rdof_hamming_t *const RDOF_H_13_9;
extern const rdof_hamming_t *const RDOF_H_16_11;
void rdof_hamming_encode(const rdof_hamming_t *h, uint8_t *w);
/* Returns ok; *corrected says a bit was flipped. */
bool rdof_hamming_decode(const rdof_hamming_t *h, uint8_t *w, bool *corrected);
int rdof_hamming_syndrome(const rdof_hamming_t *h, const uint8_t *w);

/* The 196 info bits either side of the sync / slot type field. */
void rdof_info_bits(const uint8_t burst[RDOF_BURST_BYTES], uint8_t out196[196]);

/* BPTC(196,96): 12 bytes from a data/control burst; returns false if errors remain that it can't correct. */
bool rdof_bptc196_decode(const uint8_t burst[RDOF_BURST_BYTES], uint8_t out12[12]);
void rdof_bptc196_encode(const uint8_t payload12[12], uint8_t info196[196]);

/* A 33-byte burst from 196 info bits, the 48-bit middle field and the 20-bit slot type (NULL = zeros). */
void rdof_build_burst(const uint8_t info196[196], const uint8_t *middle48, const uint8_t *slot_type20,
                    uint8_t out[RDOF_BURST_BYTES]);

/* Slot type and EMB fields as read, without FEC (fec.slot_type / fec.emb). */
void rdof_slot_type_raw(const uint8_t burst[RDOF_BURST_BYTES], int *cc, int *dt);
void rdof_emb_raw(const uint8_t burst[RDOF_BURST_BYTES], int *cc, int *pi, int *lcss);

void rdof_embedded_signalling(const uint8_t burst[RDOF_BURST_BYTES], uint8_t out32[32]);
/* The three 72-bit AMBE channel frames of a voice burst, 9 bytes each. */
void rdof_ambe_frames(const uint8_t burst[RDOF_BURST_BYTES], uint8_t out[3][9]);

/* VBPTC(128,72) embedded LC: 9 bytes from the 4 x 32 bits of a superframe. */
bool rdof_embedded_lc_decode(const uint8_t raw128[128], uint8_t lc9[9]);
void rdof_embedded_lc_encode(const uint8_t lc9[9], uint8_t raw128[128]);
int rdof_five_bit_checksum(const uint8_t lc9[9]);

#if RDOF_FEATURE_DATA_BLOCKS
/* Rate 3/4 trellis: returns the Viterbi path metric (0 = clean). */
int rdof_trellis34_decode(const uint8_t burst[RDOF_BURST_BYTES], uint8_t out18[18]);
void rdof_trellis34_encode(const uint8_t payload18[18], uint8_t info196[196]);

void rdof_rate1_payload(const uint8_t burst[RDOF_BURST_BYTES], uint8_t out24[24]);

#endif /* RDOF_FEATURE_DATA_BLOCKS */

/* CRCs */
#define RDOF_MASK_VOICE_LC_HEADER 0x969696u
#define RDOF_MASK_TERMINATOR_LC 0x999999u
#define RDOF_MASK_PI_HEADER 0x6969u
#define RDOF_MASK_CSBK 0xA5A5u
#define RDOF_MASK_DATA_HEADER 0xCCCCu
#define RDOF_MASK_MBC_HEADER 0xAAAAu
#define RDOF_MASK_USBD 0x3333u
#define RDOF_MASK_CRC9_RATE12 0x0F0
#define RDOF_MASK_CRC9_RATE34 0x1FF
#define RDOF_MASK_CRC9_RATE1 0x10F

uint16_t rdof_crc16_ccitt(const uint8_t *data, size_t len);
bool rdof_check_crc16(const uint8_t block12[12], uint16_t mask);
#if RDOF_FEATURE_DATA_BLOCKS
uint32_t rdof_crc32_dmr(const uint8_t *data, size_t len);
bool rdof_check_crc32(const uint8_t *packet_with_crc, size_t len);
uint16_t rdof_crc9_dmr(const uint8_t *bits, size_t nbits);
bool rdof_check_crc9(const uint8_t *block_data, size_t len, int dbsn, int stored, int mask);
#endif /* RDOF_FEATURE_DATA_BLOCKS */


/* Reed-Solomon (12,9): parity, and an exact check (no correction). */
void rdof_rs129_parity(const uint8_t msg9[9], uint8_t out3[3]);
bool rdof_check_rs129(const uint8_t lc12[12], uint32_t mask);

#ifdef __cplusplus
}
#endif

#endif
