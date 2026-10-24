// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* CRC32c (Castagnoli, reflected 0x82F63B78), the SCTP checksum (RFC 9260 appendix A).
   A nibble at a time: a 64-byte table, plenty fast for packets this size. */
#include "tinysctp.h"

static const uint32_t table[16] = {
    0x00000000, 0x105EC76F, 0x20BD8EDE, 0x30E349B1, 0x417B1DBC, 0x5125DAD3, 0x61C69362, 0x7198540D,
    0x82F63B78, 0x92A8FC17, 0xA24BB5A6, 0xB21572C9, 0xC38D26C4, 0xD3D3E1AB, 0xE330A81A, 0xF36E6F75,
};

uint32_t tsctp_crc32c(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = data;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        crc = table[crc & 0x0F] ^ (crc >> 4);
        crc = table[crc & 0x0F] ^ (crc >> 4);
    }
    return ~crc;
}
