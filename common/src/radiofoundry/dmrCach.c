// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The CACH and the short LC (see dmrCach.h), from ETSI TS 102 361-1 6.3, 7.1.4, 9.1.4, B.2.3, B.3.3, B.3.5, B.3.7, B.4.1
#include "dmrCach.h"

// Figure B.9: where each TACT bit (AT, TC, LS1, LS0, H2, H1, H0) and each payload bit (P16 .. P0) sits in TX(23..0)
static const uint8_t TACT_POS[7] = { 23, 19, 15, 11, 9, 5, 1 };
static const uint8_t PAYLOAD_POS[17] = { 22, 21, 20, 18, 17, 16, 14, 13, 12, 10, 8, 7, 6, 4, 3, 2, 0 };

// Remainder of data(x) * x^r modulo g(x) (degree r), data's first bit the highest power: a systematic cyclic code's
// parity (the generator matrices of tables B.13 and B.17 are these codes)
static uint32_t poly_parity(uint32_t data, uint8_t bits, uint32_t g, uint8_t r)
{
	uint32_t reg = 0;

	for (int i = bits - 1; i >= 0; i--) {
		uint32_t fb = ((data >> i) & 1u) ^ ((reg >> (r - 1)) & 1u);

		reg = (reg << 1) & ((1u << r) - 1u);
		if (fb) {
			reg ^= g & ((1u << r) - 1u);
		}
	}
	return reg;
}

uint8_t dmrCachTactEncode(uint8_t at, uint8_t tc, uint8_t lcss)
{
	uint8_t d = (uint8_t)(((at & 1u) << 3) | ((tc & 1u) << 2) | (lcss & 3u));

	return (uint8_t)((d << 3) | poly_parity(d, 4, 0x0Bu, 3));	// x^3 + x + 1
}

int dmrCachTactDecode(uint8_t code, uint8_t *at, uint8_t *tc, uint8_t *lcss)
{
	int fixed = 0;

	code &= 0x7Fu;
	uint8_t syn = (uint8_t)(poly_parity(code >> 3, 4, 0x0Bu, 3) ^ (code & 7u));
	if (syn != 0) {
		for (uint8_t bit = 0; bit < 7; bit++) {
			uint8_t c = (uint8_t)(code ^ (1u << bit));

			if (poly_parity(c >> 3, 4, 0x0Bu, 3) == (c & 7u)) {
				code = c;
				fixed = 1;
				break;
			}
		}
	}
	*at = (code >> 6) & 1u;
	*tc = (code >> 5) & 1u;
	*lcss = (code >> 3) & 3u;
	return fixed;
}

uint32_t dmrCachInterleave(uint8_t tact, uint32_t payload)
{
	uint32_t out = 0;

	for (int i = 0; i < 7; i++) {
		if ((tact >> (6 - i)) & 1u) {
			out |= 1u << TACT_POS[i];
		}
	}
	for (int i = 0; i < 17; i++) {
		if ((payload >> (16 - i)) & 1u) {
			out |= 1u << PAYLOAD_POS[i];
		}
	}
	return out;
}

void dmrCachDeinterleave(uint32_t bits, uint8_t *tact, uint32_t *payload)
{
	uint8_t t = 0;
	uint32_t p = 0;

	for (int i = 0; i < 7; i++) {
		t = (uint8_t)((t << 1) | ((bits >> TACT_POS[i]) & 1u));
	}
	for (int i = 0; i < 17; i++) {
		p = (p << 1) | ((bits >> PAYLOAD_POS[i]) & 1u);
	}
	*tact = t;
	*payload = p;
}

static uint32_t h1712(uint32_t data12)
{
	data12 &= 0x0FFFu;
	return (data12 << 5) | poly_parity(data12, 12, 0x05u, 5);	// x^5 + x^2 + 1
}

uint32_t dmrCachHamming1712Encode(uint32_t data)
{
	return h1712(data);
}

bool dmrCachHamming1712Correct(uint32_t *codeword)
{
	uint32_t c = *codeword & 0x1FFFFu;

	if (h1712(c >> 5) == c) {
		*codeword = c;
		return true;
	}
	for (uint8_t bit = 0; bit < 17; bit++) {
		uint32_t t = c ^ (1u << bit);

		if (h1712(t >> 5) == t) {
			*codeword = t;
			return true;
		}
	}
	return false;
}

static uint8_t crc8_bits(uint32_t msg, uint8_t bits)
{
	uint8_t crc = 0;

	for (int i = bits - 1; i >= 0; i--) {
		uint8_t fb = (uint8_t)(((msg >> i) & 1u) ^ (crc >> 7));

		crc = (uint8_t)(crc << 1);
		if (fb) {
			crc ^= 0x07u;	// x^8 + x^2 + x + 1, no inversion, initial remainder 0
		}
	}
	return crc;
}

uint8_t dmrShortLcCrc8(uint32_t slc)
{
	return crc8_bits(slc & 0x0FFFFFFFu, 28);
}

uint8_t dmrShortLcHash(uint32_t address)
{
	return crc8_bits(address & 0xFFFFFFu, 24);
}

// The BPTC encode matrix (figure B.6): rows 0-2 are LC(27..16) / LC(15..4) / LC(3..0) CR(7..0), each with its
// Hamming(17,12) parity, row 3 the even column parity. Transmit bit i (0..67) is encode[i % 4][i / 4]
static void matrix_to_rows(const uint32_t enc[4], uint32_t rows[4])
{
	for (int r = 0; r < 4; r++) {
		rows[r] = 0;
	}
	for (int i = 0; i < 68; i++) {
		uint32_t bit = (enc[i % 4] >> (16 - i / 4)) & 1u;

		rows[i / 17] |= bit << (16 - i % 17);
	}
}

static void rows_to_matrix(const uint32_t rows[4], uint32_t enc[4])
{
	for (int r = 0; r < 4; r++) {
		enc[r] = 0;
	}
	for (int i = 0; i < 68; i++) {
		uint32_t bit = (rows[i / 17] >> (16 - i % 17)) & 1u;

		enc[i % 4] |= bit << (16 - i / 4);
	}
}

void dmrShortLcEncode(uint32_t slc, uint32_t rows[4])
{
	uint64_t all = ((uint64_t)(slc & 0x0FFFFFFFu) << 8) | dmrShortLcCrc8(slc);	// 36 bits
	uint32_t enc[4];

	enc[0] = h1712((uint32_t)(all >> 24) & 0x0FFFu);
	enc[1] = h1712((uint32_t)(all >> 12) & 0x0FFFu);
	enc[2] = h1712((uint32_t)all & 0x0FFFu);
	enc[3] = enc[0] ^ enc[1] ^ enc[2];		// each column even
	matrix_to_rows(enc, rows);
}

bool dmrShortLcDecode(const uint32_t rows[4], uint32_t *slc)
{
	uint32_t enc[4];
	bool ok = true;

	rows_to_matrix(rows, enc);
	for (int r = 0; r < 3; r++) {
		ok &= dmrCachHamming1712Correct(&enc[r]);
	}
	ok &= ((enc[0] ^ enc[1] ^ enc[2] ^ enc[3]) & 0x1FFFFu) == 0;
	uint64_t all = ((uint64_t)(enc[0] >> 5) << 24) | ((uint64_t)(enc[1] >> 5) << 12) | (enc[2] >> 5);
	uint32_t s = (uint32_t)(all >> 8) & 0x0FFFFFFFu;

	ok &= dmrShortLcCrc8(s) == (uint8_t)all;
	*slc = s;
	return ok;
}

void dmrShortLcPack(const uint32_t rows[4], uint8_t out[DMR_CACH_SLC_BYTES])
{
	for (int i = 0; i < DMR_CACH_SLC_BYTES; i++) {
		out[i] = 0;
	}
	for (int i = 0; i < 68; i++) {
		if ((rows[i / 17] >> (16 - i % 17)) & 1u) {
			out[i / 8] |= (uint8_t)(0x80u >> (i % 8));
		}
	}
}

void dmrShortLcUnpack(const uint8_t in[DMR_CACH_SLC_BYTES], uint32_t rows[4])
{
	for (int r = 0; r < 4; r++) {
		rows[r] = 0;
	}
	for (int i = 0; i < 68; i++) {
		if (in[i / 8] & (0x80u >> (i % 8))) {
			rows[i / 17] |= 1u << (16 - i % 17);
		}
	}
}
