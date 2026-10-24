// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * DMR layer 2: sync patterns, slot type Golay(20,8), EMB QR(16,7), CACH TACT
 * Hamming(7,4), and building bursts. The
 * block codes are RadioFoundry's (src/radiofoundry/dmrFec.c, src/radiofoundry/dmrCach.c).
 *
 * The short block codes decode by nearest codeword (no lookup table of every
 * n-bit word, a million entries for Golay(20,8)), which gives the same answer inside each code's correction radius
 * (the radius is below half the minimum distance, so there are no ties).
 */
#ifndef RDOF_LAYER2_H
#define RDOF_LAYER2_H

#include <stdbool.h>
#include <stdint.h>

#include "rdof/fec.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_BS_VOICE 0x755FD7DF75F7ULL
#define RDOF_MS_VOICE 0x7F7D5DD57DFDULL
#define RDOF_TS1_VOICE 0x5D577F7757FFULL
#define RDOF_TS2_VOICE 0x7DFFD5F55D5FULL
#define RDOF_DATA_XOR 0xAAAAAAAAAAAAULL

typedef enum { RDOF_FAMILY_BS = 0, RDOF_FAMILY_MS, RDOF_FAMILY_TS1, RDOF_FAMILY_TS2, RDOF_FAMILY_COUNT } rdof_family_t;

uint64_t rdof_family_voice_sync(rdof_family_t f);
const char *rdof_family_name(rdof_family_t f);
static inline uint64_t rdof_data_sync(uint64_t voice) { return voice ^ RDOF_DATA_XOR; }

/* The 48-bit sync / EMB field. */
uint64_t rdof_middle_field(const uint8_t burst[RDOF_BURST_BYTES]);
int rdof_sync_errors(const uint8_t burst[RDOF_BURST_BYTES], uint64_t pattern48);

/* Sync detection on a whole burst: the family and kind (voice or data sync)
 * whose pattern is nearest the middle field, if within max_errors bits.
 * Returns false when none is. Ties go to the first in BS, MS, TS1, TS2 order, voice before data. */
bool rdof_detect_sync(const uint8_t burst[RDOF_BURST_BYTES], int max_errors, rdof_family_t *family, bool *data,
                    int *errors);

/* Block codes: decode returns the data value, or -1 beyond the code's correction (errors = max + 1). */
uint32_t rdof_golay2087_encode(int value8);
int rdof_golay2087_decode(uint32_t word20, int *errors);
uint32_t rdof_qr1676_encode(int value7);
int rdof_qr1676_decode(uint32_t word16, int *errors);
uint32_t rdof_hamming74_encode(int value4);
int rdof_hamming74_decode(uint32_t word7, int *errors);

/* (color code, data type, bits corrected); false if the slot type is beyond repair. */
bool rdof_decode_slot_type(const uint8_t burst[RDOF_BURST_BYTES], int *cc, int *dt, int *errors);
/* (color code, PI, LCSS, bits corrected); false if the EMB is beyond repair (pi, lcss then 0). */
bool rdof_decode_emb(const uint8_t burst[RDOF_BURST_BYTES], int *cc, int *pi, int *lcss, int *errors);
/* Slot (1/2) of the burst after a 24-bit CACH (0/1 bytes), or 0 if unreadable. */
int rdof_decode_cach_slot(const uint8_t cach24[24]);
void rdof_encode_cach(int slot, int at, int lcss, uint8_t out24[24]);
/* A CACH with its 17-bit short LC payload (a row of rdof_short_lc_rows), and the payload and LCSS back out. */
void rdof_encode_cach_lc(int slot, int at, int lcss, uint32_t payload17, uint8_t out24[24]);
uint32_t rdof_decode_cach_payload(const uint8_t cach24[24], int *lcss);

void rdof_slot_type_bits(int cc, int dt, uint8_t out20[20]);
void rdof_emb_bits(int cc, int lcss, int pi, uint8_t out16[16]);

void rdof_data_burst(const uint8_t payload12[12], int dt, int cc, uint64_t sync48, uint8_t out[RDOF_BURST_BYTES]);
void rdof_info_burst(const uint8_t info196[196], int dt, int cc, uint64_t sync48, uint8_t out[RDOF_BURST_BYTES]);
/* Full LC + RS(12,9) parity masked for a voice LC header or terminator. */
void rdof_voice_lc12(const uint8_t lc9[9], int data_type, uint8_t out12[12]);
void rdof_group_lc(uint32_t src, uint32_t dst, bool group, int service_options, uint8_t out9[9]);
void rdof_lc_burst(const uint8_t lc9[9], int data_type, int cc, uint64_t sync48, uint8_t out[RDOF_BURST_BYTES]);
void rdof_voice_burst(const uint8_t voice216[216], const uint8_t middle48[48], uint8_t out[RDOF_BURST_BYTES]);
void rdof_voice_middle(int cc, int lcss, const uint8_t frag32[32], int pi, uint8_t out48[48]);

#ifdef __cplusplus
}
#endif

#endif
