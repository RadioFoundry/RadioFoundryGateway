// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * DMR FEC from ETSI TS 102 361-1 annex B, see dmrFec.h. Written from the standard.
 */

#include <string.h>
#include "dmrFec.h"

// --- Burst bit access ---

static inline int burstBit(const uint8_t *burst, int n)
{
	return (burst[n / 8] >> (7 - (n % 8))) & 1;
}

static inline void burstSetBit(uint8_t *burst, int n, int value)
{
	uint8_t m = (uint8_t)(0x80u >> (n % 8));

	if (value)
	{
		burst[n / 8] |= m;
	}
	else
	{
		burst[n / 8] &= (uint8_t)~m;
	}
}

// The 196 info bits sit either side of the 68 bits of slot type and sync
static inline int infoToBurstBit(int i)
{
	return (i < 98) ? i : (i + 68);
}

// --- Hamming codes ---
//
// Each code is given by the syndrome its data bits produce (the columns of the parity check matrix): bit 3 of a column
// is parity bit 0, bit 0 is parity bit 3. A parity bit's own column is the single bit for it.

// B.3.5 Hamming(15,11,3):
//   p0 = d0+d1+d2+d3+d5+d7+d8, p1 = d1+d2+d3+d4+d6+d8+d9, p2 = d2+d3+d4+d5+d7+d9+d10, p3 = d0+d1+d2+d4+d6+d7+d10
static const uint8_t H15113_COLUMNS[11] = { 0x9, 0xD, 0xF, 0xE, 0x7, 0xA, 0x5, 0xB, 0xC, 0x6, 0x3 };

// B.3.4 Hamming(13,9,3):
//   p0 = d0+d1+d3+d5+d6, p1 = d0+d1+d2+d4+d6+d7, p2 = d0+d1+d2+d3+d5+d7+d8, p3 = d0+d2+d4+d5+d8
static const uint8_t H1393_COLUMNS[9] = { 0xF, 0xE, 0x7, 0xA, 0x5, 0xB, 0xC, 0x6, 0x3 };

static uint16_t hammingEncode(uint16_t data, const uint8_t *columns, int dataBits)
{
	uint8_t parity = 0;

	for (int i = 0; i < dataBits; i++)
	{
		if ((data >> (dataBits - 1 - i)) & 1)
		{
			parity ^= columns[i];
		}
	}
	return (uint16_t)((data << 4) | parity);
}

static bool hammingCorrect(uint16_t *codeword, const uint8_t *columns, int dataBits)
{
	uint16_t cw = *codeword;
	uint16_t data = cw >> 4;
	uint8_t syndrome = (uint8_t)((hammingEncode(data, columns, dataBits) ^ cw) & 0x0F);

	if (syndrome == 0)
	{
		return true;
	}

	// A single parity bit
	for (int j = 0; j < 4; j++)
	{
		if (syndrome == (0x8 >> j))
		{
			*codeword = cw ^ (uint16_t)(0x8 >> j);
			return true;
		}
	}

	// A single data bit
	for (int i = 0; i < dataBits; i++)
	{
		if (syndrome == columns[i])
		{
			*codeword = cw ^ (uint16_t)(1u << (dataBits + 3 - i));
			return true;
		}
	}
	return false;
}

uint16_t dmrFecHamming15113Encode(uint16_t data)
{
	return hammingEncode(data & 0x7FF, H15113_COLUMNS, 11);
}

uint16_t dmrFecHamming1393Encode(uint16_t data)
{
	return hammingEncode(data & 0x1FF, H1393_COLUMNS, 9);
}

bool dmrFecHamming15113Correct(uint16_t *codeword)
{
	return hammingCorrect(codeword, H15113_COLUMNS, 11);
}

bool dmrFecHamming1393Correct(uint16_t *codeword)
{
	return hammingCorrect(codeword, H1393_COLUMNS, 9);
}

// --- BPTC(196,96) ---
//
// The code matrix is index 0 (reserved R(3)) and then 13 rows of 15: row r is indices 1 + 15r .. 15 + 15r. Rows 0-8
// carry 11 data bits and 4 Hamming(15,11,3) parity bits each; row 0's first three data bits are R(2..0), always 0.
// Rows 9-12 are the Hamming(13,9,3) parity of each of the 15 columns. Matrix index k goes out as info bit
// (k * 181) mod 196.

#define BPTC_BITS			196
#define BPTC_ROWS			13
#define BPTC_COLS			15
#define BPTC_DATA_ROWS		9

static inline int matrixIndex(int row, int col)
{
	return 1 + (row * BPTC_COLS) + col;
}

static inline int interleave(int k)
{
	return (k * 181) % BPTC_BITS;
}

// Payload bit n's place in the matrix: row 0 columns 3-10, then rows 1-8 columns 0-10
static int payloadToMatrix(int n)
{
	if (n < 8)
	{
		return matrixIndex(0, n + 3);
	}
	n -= 8;
	return matrixIndex(1 + (n / 11), n % 11);
}

static uint16_t getRow(const uint8_t *m, int row)
{
	uint16_t v = 0;

	for (int c = 0; c < BPTC_COLS; c++)
	{
		v = (uint16_t)((v << 1) | m[matrixIndex(row, c)]);
	}
	return v;
}

static void putRow(uint8_t *m, int row, uint16_t v)
{
	for (int c = 0; c < BPTC_COLS; c++)
	{
		m[matrixIndex(row, c)] = (v >> (BPTC_COLS - 1 - c)) & 1;
	}
}

static uint16_t getColumn(const uint8_t *m, int col)
{
	uint16_t v = 0;

	for (int r = 0; r < BPTC_ROWS; r++)
	{
		v = (uint16_t)((v << 1) | m[matrixIndex(r, col)]);
	}
	return v;
}

static void putColumn(uint8_t *m, int col, uint16_t v)
{
	for (int r = 0; r < BPTC_ROWS; r++)
	{
		m[matrixIndex(r, col)] = (v >> (BPTC_ROWS - 1 - r)) & 1;
	}
}

void dmrFecBptcEncode(const uint8_t *payload, uint8_t *burst)
{
	uint8_t m[BPTC_BITS];

	memset(m, 0, sizeof(m));
	for (int n = 0; n < 96; n++)
	{
		m[payloadToMatrix(n)] = (payload[n / 8] >> (7 - (n % 8))) & 1;
	}

	for (int r = 0; r < BPTC_DATA_ROWS; r++)
	{
		putRow(m, r, dmrFecHamming15113Encode((uint16_t)(getRow(m, r) >> 4)));
	}
	for (int c = 0; c < BPTC_COLS; c++)
	{
		putColumn(m, c, dmrFecHamming1393Encode((uint16_t)(getColumn(m, c) >> 4)));
	}

	for (int k = 0; k < BPTC_BITS; k++)
	{
		burstSetBit(burst, infoToBurstBit(interleave(k)), m[k]);
	}
}

int dmrFecBptcDecode(const uint8_t *burst, uint8_t *payload)
{
	uint8_t m[BPTC_BITS];
	int corrected = 0;
	bool clean = false;

	for (int k = 0; k < BPTC_BITS; k++)
	{
		m[k] = (uint8_t)burstBit(burst, infoToBurstBit(interleave(k)));
	}

	// Columns, then rows, until both are clean or nothing more can be fixed
	for (int pass = 0; (pass < 4) && !clean; pass++)
	{
		bool changed = false;

		clean = true;
		for (int c = 0; c < BPTC_COLS; c++)
		{
			uint16_t v = getColumn(m, c), fixed = v;

			if (!dmrFecHamming1393Correct(&fixed))
			{
				clean = false;
			}
			else if (fixed != v)
			{
				putColumn(m, c, fixed);
				corrected++;
				changed = true;
			}
		}
		// The column parity rows are sums of data rows, so row codewords too: all 13 rows are checked
		for (int r = 0; r < BPTC_ROWS; r++)
		{
			uint16_t v = getRow(m, r), fixed = v;

			if (!dmrFecHamming15113Correct(&fixed))
			{
				clean = false;
			}
			else if (fixed != v)
			{
				putRow(m, r, fixed);
				corrected++;
				changed = true;
			}
		}
		if (changed)
		{
			clean = false;// check again after the fixes
		}
	}

	memset(payload, 0, DMR_FEC_BPTC_PAYLOAD_BYTES);
	for (int n = 0; n < 96; n++)
	{
		if (m[payloadToMatrix(n)])
		{
			payload[n / 8] |= (uint8_t)(0x80u >> (n % 8));
		}
	}
	return clean ? corrected : -1;
}

// --- Golay(20,8) ---
//
// The cyclic Golay(23,12) code (generator x^11 + x^10 + x^6 + x^5 + x^4 + x^2 + 1) shortened to 8 data bits, plus an
// overall even parity bit: data, 11 cyclic parity bits, the even parity bit. Minimum distance 8.

#define GOLAY23_GENERATOR	0xC75u

static uint32_t golay23Parity(uint32_t data)
{
	uint32_t r = data << 11;

	for (int b = 22; b >= 11; b--)
	{
		if (r & (1u << b))
		{
			r ^= GOLAY23_GENERATOR << (b - 11);
		}
	}
	return r & 0x7FFu;
}

static int popcount32(uint32_t v)
{
	int n = 0;

	while (v)
	{
		v &= v - 1;
		n++;
	}
	return n;
}

uint32_t dmrFecGolay2087Encode(uint8_t data)
{
	uint32_t cw = ((uint32_t)data << 11) | golay23Parity(data);

	return (cw << 1) | (uint32_t)(popcount32(cw) & 1);
}

int dmrFecGolay2087Decode(uint32_t codeword, uint8_t *data)
{
	int best = 21, bestData = 0;

	codeword &= 0xFFFFFu;
	for (int d = 0; d < 256; d++)
	{
		int dist = popcount32(dmrFecGolay2087Encode((uint8_t)d) ^ codeword);

		if (dist < best)
		{
			best = dist;
			bestData = d;
		}
	}
	*data = (uint8_t)bestData;
	return (best <= 3) ? best : -1;
}

// --- QR(16,7,6) (B.3.2) ---
//
// The cyclic quadratic residue code QR(17,9,5) (generator x^8 + x^5 + x^4 + x^3 + 1) shortened to 7 data bits, plus an
// overall even parity bit: data, 8 cyclic parity bits, the even parity bit. Minimum distance 6.

#define QR17_GENERATOR		0x139u

static uint16_t qr17Parity(uint16_t data)
{
	uint32_t r = (uint32_t)data << 8;

	for (int b = 14; b >= 8; b--)
	{
		if (r & (1u << b))
		{
			r ^= QR17_GENERATOR << (b - 8);
		}
	}
	return (uint16_t)(r & 0xFFu);
}

uint16_t dmrFecQr1676Encode(uint8_t data)
{
	uint32_t cw = ((uint32_t)(data & 0x7F) << 8) | qr17Parity(data & 0x7F);

	return (uint16_t)((cw << 1) | (uint32_t)(popcount32(cw) & 1));
}

int dmrFecQr1676Decode(uint16_t codeword, uint8_t *data)
{
	int best = 17, bestData = 0;

	for (int d = 0; d < 128; d++)
	{
		int dist = popcount32((uint32_t)(dmrFecQr1676Encode((uint8_t)d) ^ codeword));

		if (dist < best)
		{
			best = dist;
			bestData = d;
		}
	}
	*data = (uint8_t)bestData;
	return (best <= 2) ? best : -1;
}

uint16_t dmrFecEmbEncode(uint8_t colorCode, bool pi, uint8_t lcss)
{
	return dmrFecQr1676Encode((uint8_t)(((colorCode & 0x0F) << 3) | (pi ? 0x04 : 0) | (lcss & 0x03)));
}

int dmrFecEmbDecode(uint16_t emb, uint8_t *colorCode, bool *pi, uint8_t *lcss)
{
	uint8_t d;
	int errors = dmrFecQr1676Decode(emb, &d);

	*colorCode = d >> 3;
	*pi = (d & 0x04) != 0;
	*lcss = d & 0x03;
	return errors;
}

// --- Slot type ---

#define SLOT_TYPE_FIRST		98		// bits 98-107: codeword bits 19-10
#define SLOT_TYPE_SECOND	156		// bits 156-165: codeword bits 9-0

void dmrFecSlotTypeEncode(uint8_t colorCode, uint8_t dataType, uint8_t *burst)
{
	uint32_t cw = dmrFecGolay2087Encode((uint8_t)(((colorCode & 0x0F) << 4) | (dataType & 0x0F)));

	for (int i = 0; i < 10; i++)
	{
		burstSetBit(burst, SLOT_TYPE_FIRST + i, (cw >> (19 - i)) & 1);
		burstSetBit(burst, SLOT_TYPE_SECOND + i, (cw >> (9 - i)) & 1);
	}
}

bool dmrFecSlotTypeDecode(const uint8_t *burst, uint8_t *colorCode, uint8_t *dataType)
{
	uint32_t cw = 0;
	uint8_t d;

	for (int i = 0; i < 10; i++)
	{
		cw = (cw << 1) | (uint32_t)burstBit(burst, SLOT_TYPE_FIRST + i);
	}
	for (int i = 0; i < 10; i++)
	{
		cw = (cw << 1) | (uint32_t)burstBit(burst, SLOT_TYPE_SECOND + i);
	}

	int errors = dmrFecGolay2087Decode(cw, &d);

	*colorCode = d >> 4;
	*dataType = d & 0x0F;
	return errors >= 0;
}
