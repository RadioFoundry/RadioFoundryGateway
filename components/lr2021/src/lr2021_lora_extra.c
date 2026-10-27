// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * LoRa commands and data beside lr2021_lora.c (lr2021_lora_extra.h).
 *
 * The extended sync word commands (0x022D, 0x022E) and the ranging delay table come from TheClams/lr2021
 * (github.com/TheClams/lr2021), under the MIT License:
 *
 * MIT License
 *
 * Copyright (c) 2025 TheClams
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "lr2021_lora_extra.h"

static size_t put_op(uint8_t *b, uint16_t op)
{
    b[0] = (uint8_t)(op >> 8);
    b[1] = (uint8_t)op;
    return 2;
}

size_t lr2021_lora_syncword_ext(uint8_t *b, int8_t s1, int8_t s2)
{
    put_op(b, LR2021_SET_LORA_SYNCWORD_EXT);
    b[2] = (uint8_t)s1 & 0x1F;
    b[3] = (uint8_t)s2 & 0x1F;
    return 4;
}

void lr2021_lora_syncword_split(uint8_t syncword, int8_t *s1, int8_t *s2)
{
    const int hi = syncword >> 4, lo = syncword & 0xF;
    *s1 = (int8_t)(2 * (hi > 7 ? hi - 16 : hi));
    *s2 = (int8_t)(2 * (lo > 7 ? lo - 16 : lo));
}

size_t lr2021_lora_side_det_syncword_ext(uint8_t *b, const int8_t pairs[3][2])
{
    size_t o = put_op(b, LR2021_SET_LORA_SIDE_DET_SYNCWORD_EXT);
    for (int i = 0; i < 3; i++) {
        b[o++] = (uint8_t)pairs[i][0] & 0x1F;
        b[o++] = (uint8_t)pairs[i][1] & 0x1F;
    }
    return o;
}

uint32_t lr2021_rttof_base_delay(uint8_t sf, uint8_t bw)
{
    /* a row per bandwidth (1000, 812, 500, 406, 250, 203, 125 kHz), a column per SF (5 to 12) */
    static const uint16_t delay[7][8] = {
        {21711, 21729, 21733, 21715, 21669, 21577, 21391, 21016},
        {25547, 25596, 25599, 25652, 25683, 25765, 25918, 26283},
        {20616, 20607, 20567, 20480, 20307, 19959, 19258, 17860},
        {24460, 24548, 24547, 24624, 24936, 25264, 26084, 27689},
        {20149, 20141, 20100, 20013, 19837, 19486, 18787, 17386},
        {23975, 24087, 24089, 24191, 24356, 24806, 25560, 27153},
        {19688, 19649, 19560, 19387, 19043, 18350, 16967, 14191},
    };
    static const uint8_t rows[7] = {LR2021_LORA_BW_1000, LR2021_LORA_BW_812, LR2021_LORA_BW_500, LR2021_LORA_BW_406,
                                    LR2021_LORA_BW_250,  LR2021_LORA_BW_203, LR2021_LORA_BW_125};
    if (sf < 5 || sf > 12)
        return 0;
    for (int i = 0; i < 7; i++)
        if (rows[i] == bw)
            return delay[i][sf - 5];
    return 18000u - (5600u >> (12 - sf));
}
