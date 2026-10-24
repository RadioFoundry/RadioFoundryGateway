// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * The packet data CRCs (ETSI TS 102 361-1 B.3.9, B.3.10). Declarations and masks from RadioFoundry
 * core/dmr/dmrData.h (1ce780d1).
 */

#ifndef ODR_DMR_DATA_CRC_H
#define ODR_DMR_DATA_CRC_H

#include <stdint.h>

uint32_t dmrDataCRC32(const uint8_t *data, int length);
// The CRC-9 of a confirmed block (B.3.10): over the block's data octets then the 7 serial number bits, inverted, not
// masked (the rate's mask: DMR_CRC9_MASK_xxx, B.21)
uint16_t dmrDataCRC9(const uint8_t *data, int length, uint8_t serial);
#define DMR_CRC9_MASK_RATE_12		0x0F0
#define DMR_CRC9_MASK_RATE_34		0x1FF
#define DMR_CRC9_MASK_RATE_1		0x10F

#endif
