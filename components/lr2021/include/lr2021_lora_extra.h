// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * LoRa commands and data beside lr2021_lora.h, not in the datasheet: the 10-bit sync word forms and a starting
 * ranging delay. Frames and numbers only, no I/O.
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
#ifndef LR2021_LORA_EXTRA_H
#define LR2021_LORA_EXTRA_H

#include <stddef.h>
#include <stdint.h>

#include "lr2021_lora.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Not in the datasheet: the 10-bit sync word forms */
enum {
    LR2021_SET_LORA_SYNCWORD_EXT = 0x022D,
    LR2021_SET_LORA_SIDE_DET_SYNCWORD_EXT = 0x022E,
};

/* The 10-bit sync word (not in the datasheet): two 5-bit signed values, 4 bytes. Public is (6, 8), private (2, 4). */
size_t lr2021_lora_syncword_ext(uint8_t *buf, int8_t s1, int8_t s2);
/* An 8-bit sync word as the two 5-bit values: each nibble, read as 4-bit signed, times 2 (0x34 is 6, 8). */
void lr2021_lora_syncword_split(uint8_t syncword, int8_t *s1, int8_t *s2);
/* The side detectors' 10-bit sync words (not in the datasheet): three (s1, s2) pairs, 8 bytes. */
size_t lr2021_lora_side_det_syncword_ext(uint8_t *buf, const int8_t pairs[3][2]);

/* A starting SetRangingTxRxDelay for an SF and bandwidth, measured for 125 kHz and wider (TheClams/lr2021's table);
 * a rough estimate for narrower ones. The board adds its own offset on top. */
uint32_t lr2021_rttof_base_delay(uint8_t sf, uint8_t bw);

#ifdef __cplusplus
}
#endif

#endif
