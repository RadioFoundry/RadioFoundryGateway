// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#include "rdof/bits.h"

#include <string.h>

void rdof_bytes_to_bits(const uint8_t *in, size_t nbytes, uint8_t *bits)
{
    for (size_t i = 0; i < nbytes; i++)
        for (int b = 0; b < 8; b++)
            bits[i * 8 + b] = (in[i] >> (7 - b)) & 1;
}

void rdof_bits_to_bytes(const uint8_t *bits, size_t nbits, uint8_t *out)
{
    memset(out, 0, (nbits + 7) / 8);
    for (size_t i = 0; i < nbits; i++)
        if (bits[i])
            out[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
}

uint64_t rdof_bits_to_u64(const uint8_t *bits, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n; i++)
        v = (v << 1) | (bits[i] & 1);
    return v;
}

void rdof_u64_to_bits(uint64_t value, int width, uint8_t *bits)
{
    for (int i = 0; i < width; i++)
        bits[i] = (value >> (width - 1 - i)) & 1;
}

uint64_t rdof_get_field(const uint8_t *bytes, int start, int n)
{
    uint64_t v = 0;
    for (int i = start; i < start + n; i++)
        v = (v << 1) | ((bytes[i >> 3] >> (7 - (i & 7))) & 1);
    return v;
}

void rdof_set_field(uint8_t *bytes, int start, int n, uint64_t value)
{
    for (int i = 0; i < n; i++) {
        int pos = start + i;
        uint8_t mask = (uint8_t)(0x80 >> (pos & 7));
        if ((value >> (n - 1 - i)) & 1)
            bytes[pos >> 3] |= mask;
        else
            bytes[pos >> 3] &= (uint8_t)~mask;
    }
}

int rdof_popcount64(uint64_t v)
{
    int n = 0;
    while (v) {
        v &= v - 1;
        n++;
    }
    return n;
}

uint32_t rdof_be24(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

uint32_t rdof_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

void rdof_put_be24(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 16);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)v;
}

void rdof_put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
