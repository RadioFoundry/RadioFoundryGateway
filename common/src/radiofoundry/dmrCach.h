// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * The CACH of a BS outbound (ETSI TS 102 361-1 4.5, 6.3) and the short LC it carries, written from the standard:
 *   - TACT: AT, TC, LCSS (4 bits) with Hamming(7,4,3) parity, generator x^3 + x + 1 (9.1.4, B.3.5 table B.17)
 *   - the 24-bit CACH burst: TACT spread over the burst, 17 payload bits in the gaps (B.4.1 figure B.9)
 *   - short LC: 28 bits (SLCO + 3 octets, 7.1.4) + CRC-8 x^8 + x^2 + x + 1 (B.3.7), BPTC of three Hamming(17,12,3)
 *     rows (generator x^5 + x^2 + 1, B.3.3 table B.13) and a column parity row, read out by columns into four 17-bit
 *     CACH payloads (B.2.3 figure B.6)
 * Shared by the split duplex TX half (docs/spec/split-duplex.md 5.2) and the Tier III plan (dmr-tier3-plan.md).
 *
 * Bit conventions: a 7-bit TACT codeword holds AT in bit 6, TC bit 5, LCSS bits 4-3, H(2..0) bits 2-0. A 24-bit CACH
 * holds TX(23) (the first bit on air) in bit 23. A 17-bit payload holds P(16) (first on air) in bit 16. A short LC is
 * its 28 bits right aligned: SLCO in bits 27-24 (octet 0 bits 3-0), octets 1-3 in bits 23-0.
 */
#ifndef ODR_DMR_CACH_H
#define ODR_DMR_CACH_H

#include <stdbool.h>
#include <stdint.h>

#define DMR_LCSS_SINGLE		0	// not used in the CACH
#define DMR_LCSS_FIRST		1
#define DMR_LCSS_LAST		2
#define DMR_LCSS_CONT		3

#define DMR_SLCO_NULL		0x0	// Nul_Msg (TS 102 361-2 7.1.3.1)
#define DMR_SLCO_ACTIVITY	0x1	// Act_Updt (TS 102 361-2 7.1.3.2)

#define DMR_CACH_SLC_BYTES	9	// the four 17-bit payloads packed (DMR_SHORTLC, split-duplex.md 5.3)

uint8_t dmrCachTactEncode(uint8_t at, uint8_t tc, uint8_t lcss);
// Single errors corrected. Returns the number of bits corrected (0 or 1); a Hamming(7,4,3) codeword always decodes
int dmrCachTactDecode(uint8_t code, uint8_t *at, uint8_t *tc, uint8_t *lcss);

uint32_t dmrCachInterleave(uint8_t tact, uint32_t payload);
void dmrCachDeinterleave(uint32_t bits, uint8_t *tact, uint32_t *payload);

// A whole CACH: the TACT encoded and interleaved with the payload
static inline uint32_t dmrCachBuild(uint8_t at, uint8_t tc, uint8_t lcss, uint32_t payload)
{
	return dmrCachInterleave(dmrCachTactEncode(at, tc, lcss), payload);
}

uint32_t dmrCachHamming1712Encode(uint32_t data);	// 12 data bits (first on air in bit 11) -> 17 (bit 16 first)
bool dmrCachHamming1712Correct(uint32_t *codeword);	// false when the syndrome matches no single bit

uint8_t dmrShortLcCrc8(uint32_t slc);
// The four CACH payloads (rows[0] goes in the first CACH, LCSS first fragment)
void dmrShortLcEncode(uint32_t slc, uint32_t rows[4]);
// Back to the 28 bits: single errors per row corrected, column parity and CRC checked. False if any check fails
bool dmrShortLcDecode(const uint32_t rows[4], uint32_t *slc);

// The 68 payload bits as 9 bytes, rows[0] first, P(16) first, most significant bit first, 4 zero pad bits at the end
void dmrShortLcPack(const uint32_t rows[4], uint8_t out[DMR_CACH_SLC_BYTES]);
void dmrShortLcUnpack(const uint8_t in[DMR_CACH_SLC_BYTES], uint32_t rows[4]);

static inline uint32_t dmrShortLcNull(void)
{
	return (uint32_t)DMR_SLCO_NULL << 24;
}
// Act_Updt: activity ID of each BS slot (4 bits: 0 none, 8 group voice, 9 individual voice, ...) and the hashed
// destination addresses
static inline uint32_t dmrShortLcActivity(uint8_t ts1, uint8_t ts2, uint8_t hash1, uint8_t hash2)
{
	return ((uint32_t)DMR_SLCO_ACTIVITY << 24) | ((uint32_t)(ts1 & 0x0Fu) << 20) | ((uint32_t)(ts2 & 0x0Fu) << 16) |
	       ((uint32_t)hash1 << 8) | hash2;
}
// The 8-bit hash of a 24-bit destination address for Act_Updt: its CRC-8 (B.3.7)
uint8_t dmrShortLcHash(uint32_t address);

#endif
