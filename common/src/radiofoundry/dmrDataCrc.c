// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * The packet data CRCs of ETSI TS 102 361-1: the 32-bit message CRC (B.3.9) and the confirmed block CRC-9 (B.3.10),
 * see dmrDataCrc.h. Extracted unchanged from RadioFoundry core/dmr/dmrData.c (1ce780d1).
 */

#include "dmrDataCrc.h"

// Clause B.3.9: the octets are taken in pairs, least significant octet of each 16 bit word first,
// MSB first, generator 0x04C11DB7, initial remainder zero.
// A zero initial value makes the direct form equivalent to the spec's augmented form.
uint32_t dmrDataCRC32(const uint8_t *data, int length)
{
	uint32_t crc = 0;
	int paddedLength = (length + 1) & ~1;

	for (int i = 0; i < paddedLength; i++)
	{
		int idx = i ^ 1;// swap the octets of each 16 bit word
		crc ^= (uint32_t)((idx < length) ? data[idx] : 0) << 24;

		for (int b = 0; b < 8; b++)
		{
			crc = (crc & 0x80000000) ? ((crc << 1) ^ 0x04C11DB7) : (crc << 1);
		}
	}

	return crc;
}

// x^9 M(x) mod G9(x), G9 = x^9 + x^6 + x^4 + x^3 + 1, M(x) the data bits MSB first then the 7 serial number bits, zero
// initial remainder; plus I9(x) = all ones (B.25)
uint16_t dmrDataCRC9(const uint8_t *data, int length, uint8_t serial)
{
	uint16_t crc = 0;

	for (int i = 0; i < (length * 8) + 7; i++)
	{
		int bit = (i < (length * 8)) ? ((data[i >> 3] >> (7 - (i & 7))) & 1) : ((serial >> (6 - (i - (length * 8)))) & 1);
		int feedback = ((crc >> 8) & 1) ^ bit;

		crc = (crc << 1) & 0x1FF;
		if (feedback)
		{
			crc ^= 0x059;
		}
	}

	return crc ^ 0x1FF;
}

