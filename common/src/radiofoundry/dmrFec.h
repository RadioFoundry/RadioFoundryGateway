// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * DMR forward error correction used by the data, hotspot and SFR code, from ETSI TS 102 361-1 annex B:
 * BPTC(196,96) (B.1.1) with its Hamming(15,11,3) row and Hamming(13,9,3) column codes (B.3.4, B.3.5), the
 * slot type's Golay(20,8) (B.3.1), and the voice EMB's QR(16,7,6) (B.3.2).
 *
 * A burst is 33 bytes (264 bits, most significant bit of byte 0 first): 98 info bits, 10 slot type bits, 48 sync bits,
 * 10 slot type bits, 98 info bits. The 196 BPTC info bits are burst bits 0-97 and 166-263; the slot type is burst bits
 * 98-107 and 156-165.
 */

#ifndef ODR_DMR_FEC_H
#define ODR_DMR_FEC_H

#include <stdbool.h>
#include <stdint.h>

#define DMR_FEC_BURST_BYTES			33
#define DMR_FEC_BPTC_PAYLOAD_BYTES	12		// 96 bits, I(95) first

// BPTC(196,96): 12 payload bytes into the 196 info bits of the burst. Only the info bits are written; the slot type
// and sync bits are left as they are.
void dmrFecBptcEncode(const uint8_t *payload, uint8_t *burst);

// BPTC(196,96) back to the 12 payload bytes, single errors in rows and columns corrected. Returns the number of bits
// corrected, or -1 when errors remain that the code can't fix (the payload is then the best guess).
int dmrFecBptcDecode(const uint8_t *burst, uint8_t *payload);

// Hamming codewords as unsigned integers, codeword bit 0 (the first transmitted, data bit 0) in the most significant
// position: (15,11,3) is 15 bits wide, 11 data then 4 parity; (13,9,3) is 13 bits, 9 data then 4 parity.
uint16_t dmrFecHamming15113Encode(uint16_t data);
uint16_t dmrFecHamming1393Encode(uint16_t data);
// Correct one bit error in place. Returns false if the syndrome matches no single bit (more than one error).
bool dmrFecHamming15113Correct(uint16_t *codeword);
bool dmrFecHamming1393Correct(uint16_t *codeword);

// Golay(20,8): the 20 bit codeword for 8 data bits, data in bits 19-12, then 12 parity bits
uint32_t dmrFecGolay2087Encode(uint8_t data);
// Nearest codeword's data (up to 3 bit errors corrected). Returns the number of bit errors, or -1 if there were too many.
int dmrFecGolay2087Decode(uint32_t codeword, uint8_t *data);

// Slot type: color code (4 bits) and data type (4 bits), Golay(20,8) coded, into / out of the burst
void dmrFecSlotTypeEncode(uint8_t colorCode, uint8_t dataType, uint8_t *burst);
bool dmrFecSlotTypeDecode(const uint8_t *burst, uint8_t *colorCode, uint8_t *dataType);

// QR(16,7,6): the 16 bit codeword for 7 data bits, data in bits 15-9, then 9 parity bits
uint16_t dmrFecQr1676Encode(uint8_t data);
// Nearest codeword's data (up to 2 bit errors corrected). Returns the number of bit errors, or -1 if there were too many.
int dmrFecQr1676Decode(uint16_t codeword, uint8_t *data);

// The EMB of voice bursts B-F (ETSI TS 102 361-1 9.1.2): color code (4 bits), PI (1), LCSS (2), QR(16,7,6) coded.
// In a burst, codeword bits 15-8 are burst bits 108-115 and bits 7-0 are burst bits 148-155 (the 32 embedded
// signaling bits sit between them)
uint16_t dmrFecEmbEncode(uint8_t colorCode, bool pi, uint8_t lcss);
// Returns the number of bit errors corrected, or -1 if the EMB can't be trusted
int dmrFecEmbDecode(uint16_t emb, uint8_t *colorCode, bool *pi, uint8_t *lcss);

#endif
