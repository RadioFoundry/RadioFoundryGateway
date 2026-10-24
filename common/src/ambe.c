// SPDX-License-Identifier: MIT AND ISC
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* AMBE+2 channel frames: Golay(23,12) and the frame decode (rdof/ambe.h). */
#include "rdof/ambe.h"

#include "rdof/bits.h"

static const uint8_t RW[36] = {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
                               0, 1, 0, 1, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2};
static const uint8_t RX[36] = {23, 10, 22, 9, 21, 8, 20, 7, 19, 6, 18, 5, 17, 4, 16, 3, 15, 2,
                               14, 1, 13, 0, 12, 10, 11, 9, 10, 8, 9, 7, 8, 6, 7, 5, 6, 4};
static const uint8_t RY[36] = {0, 2, 0, 2, 0, 2, 0, 2, 0, 3, 0, 3, 1, 3, 1, 3, 1, 3,
                               1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3};
static const uint8_t RZ[36] = {5, 3, 4, 2, 3, 1, 2, 0, 1, 13, 0, 12, 22, 11, 21, 10, 20, 9,
                               19, 8, 18, 7, 17, 6, 16, 5, 15, 4, 14, 3, 13, 2, 12, 1, 11, 0};

#define GOLAY_POLY 0xC75u

static uint32_t golay_parity(uint32_t data12)
{
    uint32_t reg = data12 << 11;
    for (int i = 22; i > 10; i--)
        if (reg & (1u << i))
            reg ^= GOLAY_POLY << (i - 11);
    return reg & 0x7FF;
}

static uint32_t syndrome(uint32_t word23)
{
    return golay_parity(word23 >> 11) ^ (word23 & 0x7FF);
}

uint32_t rdof_golay2312_encode(uint32_t data12)
{
    return (data12 << 11) | golay_parity(data12);
}

/* The code is perfect: every syndrome has exactly one error pattern of up to
 * 3 bits. The syndrome is linear, so search the patterns by XORing the single
 * bit syndromes (no 8 KB table). */
uint32_t rdof_golay2312_decode(uint32_t word23, int *errors)
{
    uint32_t s = syndrome(word23), col[23], err = 0;
    if (s) {
        for (int i = 0; i < 23; i++)
            col[i] = syndrome(1u << i);
        for (int a = 0; a < 23 && !err; a++) {
            if (col[a] == s) {
                err = 1u << a;
                break;
            }
            for (int b = a + 1; b < 23 && !err; b++) {
                uint32_t ab = col[a] ^ col[b];
                if (ab == s) {
                    err = (1u << a) | (1u << b);
                    break;
                }
                for (int c = b + 1; c < 23; c++)
                    if ((ab ^ col[c]) == s) {
                        err = (1u << a) | (1u << b) | (1u << c);
                        break;
                    }
            }
        }
    }
    if (errors)
        *errors = rdof_popcount64(err >> 11);
    return (word23 ^ err) >> 11;
}

static uint32_t word(const uint8_t *row, int hi, int lo)
{
    uint32_t v = 0;
    for (int j = hi; j >= lo; j--)
        v = (v << 1) | row[j];
    return v;
}

static uint32_t prng_mask(uint32_t c0)
{
    uint32_t pr = 16 * c0, mask = 0;
    for (int i = 0; i < 23; i++) {
        pr = (173 * pr + 13849) & 0xFFFF;
        mask = (mask << 1) | (pr >> 15);
    }
    return mask;
}

int rdof_ambe_decode_frame(const uint8_t frame72[9], uint8_t params49[49])
{
    uint8_t bits[72], fr[4][24] = {{0}};
    rdof_bytes_to_bits(frame72, 9, bits);
    for (int i = 0; i < 36; i++) {
        fr[RW[i]][RX[i]] = bits[2 * i];
        fr[RY[i]][RZ[i]] = bits[2 * i + 1];
    }
    int e0, e1;
    uint32_t c0 = rdof_golay2312_decode(word(fr[0], 23, 1), &e0); /* fr[0][0] is the Golay(24,12) extra parity */
    uint32_t c1 = rdof_golay2312_decode(word(fr[1], 22, 0) ^ prng_mask(c0), &e1);
    if (params49) {
        for (int i = 0; i < 12; i++) {
            params49[i] = (c0 >> (11 - i)) & 1;
            params49[12 + i] = (c1 >> (11 - i)) & 1;
        }
        int n = 24;
        for (int j = 10; j >= 0; j--)
            params49[n++] = fr[2][j];
        for (int j = 13; j >= 0; j--)
            params49[n++] = fr[3][j];
    }
    return e0 + e1;
}

void rdof_ambe_encode_frame(const uint8_t params49[49], uint8_t frame72[9])
{
    uint8_t fr[4][24] = {{0}}, bits[72];
    uint32_t c0 = (uint32_t)rdof_bits_to_u64(params49, 12), c1 = (uint32_t)rdof_bits_to_u64(params49 + 12, 12);
    uint32_t w0 = rdof_golay2312_encode(c0), w1 = rdof_golay2312_encode(c1) ^ prng_mask(c0);
    for (int j = 0; j < 23; j++) {
        fr[0][j + 1] = (w0 >> j) & 1;
        fr[1][j] = (w1 >> j) & 1;
    }
    fr[0][0] = rdof_popcount64(w0) & 1;
    int n = 24;
    for (int j = 10; j >= 0; j--)
        fr[2][j] = params49[n++];
    for (int j = 13; j >= 0; j--)
        fr[3][j] = params49[n++];
    for (int i = 0; i < 36; i++) {
        bits[2 * i] = fr[RW[i]][RX[i]];
        bits[2 * i + 1] = fr[RY[i]][RZ[i]];
    }
    rdof_bits_to_bytes(bits, 72, frame72);
}
