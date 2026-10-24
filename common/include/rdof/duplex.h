// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Bursts as a repeater sends them, and the CACH short LC
 * (docs/spec/repeater.md, sections 5 and 9).
 */
#ifndef RDOF_DUPLEX_H
#define RDOF_DUPLEX_H

#include <stdbool.h>
#include <stdint.h>

#include "rdof/fec.h"
#include "rdof/features.h"

#ifdef __cplusplus
extern "C" {
#endif

#if RDOF_FEATURE_REPEATER

/* An idle burst with this color code (the idle message of ETSI TS 102 361-1 D.2). */
void rdof_duplex_idle(int cc, uint8_t out[RDOF_BURST_BYTES]);
/* A voice LC header or terminator for lc9, BS data sync. */
void rdof_duplex_lc_burst(const uint8_t lc9[9], int dt, int cc, uint8_t out[RDOF_BURST_BYTES]);
/* A data-sync burst (not an LC header / terminator) for a repeater to send; false when it isn't repeated (R5.5). */
bool rdof_duplex_data(uint8_t control, const uint8_t burst[RDOF_BURST_BYTES], int cc, uint8_t out[RDOF_BURST_BYTES]);
/* The three AMBE frames' FEC regenerated in place; returns the bits corrected. */
int rdof_regen_ambe(uint8_t burst[RDOF_BURST_BYTES]);

/* A silence burst for position n (0 = A, 1-5 = B-F), R5.10; returns its control byte. */
uint8_t rdof_duplex_silence(int n, int cc, uint8_t out[RDOF_BURST_BYTES]);

/* One voice call's bursts A-F, regenerated (R5.3, R5.7-R5.9). */
typedef struct {
    uint8_t lc9[9];
    int cc;
    bool regen_ambe;
    uint8_t lc_raw[128], sending[128];
    uint8_t heard[5][32];
    uint8_t have; /* bit n: heard[n] holds burst n's fragment (1..4) */
    long errors;
} rdof_voice_regen_t;

void rdof_voice_regen_init(rdof_voice_regen_t *v, const uint8_t lc9[9], int cc);
/* control: SYNC_AUDIO (burst A) or the burst's number 1-5. */
void rdof_voice_regen_burst(rdof_voice_regen_t *v, uint8_t control, const uint8_t in[RDOF_BURST_BYTES],
                          uint8_t out[RDOF_BURST_BYTES]);

/* The CACH short LC: lc5 is 4 zero bits, the 28-bit short LC and its CRC-8; out9 / sl9 the 9 bytes
 * DMR_SHORTLC takes. Decode corrects one error per Hamming row and checks the column parity and CRC. */
void rdof_short_lc(const uint8_t lc5[5], uint8_t out9[9]);
bool rdof_decode_short_lc(const uint8_t sl9[9], uint8_t lc5[5]);
/* The four 17-bit CACH payloads in sl9 (rows[0] goes in the CACH with LCSS first fragment), and back. */
void rdof_short_lc_rows(const uint8_t sl9[9], uint32_t rows[4]);
void rdof_short_lc_join(const uint32_t rows[4], uint8_t sl9[9]);
/* DMR_SHORTLC's 9 bytes as a transmitter takes them (RadioFoundry split-duplex.md 5.3): the encoded form if it checks, else
 * the 5-byte form (lc5 in the first 5 bytes) if its CRC matches, encoded into out9. False: neither. */
bool rdof_short_lc_accept(const uint8_t in9[9], uint8_t out9[9]);

typedef enum { RDOF_ACT_NONE = 0, RDOF_ACT_VOICE, RDOF_ACT_DATA, RDOF_ACT_CSBK } rdof_activity_kind_t;
typedef struct {
    rdof_activity_kind_t kind;
    uint32_t dst;
    bool group;
} rdof_activity_t;

/* The encoded short LC for what's on each slot (R9.1), the Null short LC when both are idle (R9.4); ts1_low:
 * TS1 in the low nibble and the second hash (the short_lc_ts1_low interop option). */
void rdof_activity_lc(const rdof_activity_t *slot1, const rdof_activity_t *slot2, bool ts1_low, uint8_t out9[9]);

#endif /* RDOF_FEATURE_REPEATER */

#ifdef __cplusplus
}
#endif

#endif
