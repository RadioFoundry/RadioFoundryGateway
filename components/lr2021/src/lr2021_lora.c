// SPDX-License-Identifier: MIT AND BSD-3-Clause-Clear
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * LR2021 LoRa, LR-FHSS and RTToF commands (lr2021_lora.h): frames, answers and time on air, no I/O.
 *
 * Command encodings ported from Semtech's USP LR20xx driver (github.com/Lora-net/usp, commit 540bba0), under the
 * Clear BSD License:
 *
 * The Clear BSD License
 * Copyright Semtech Corporation 2022-2026. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted (subject to the limitations in the disclaimer
 * below) provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Semtech corporation nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE GRANTED BY
 * THIS LICENSE. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT
 * NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL SEMTECH CORPORATION BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
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
#include "lr2021_lora.h"

#include <math.h>

static size_t put_op(uint8_t *b, uint16_t op)
{
    b[0] = (uint8_t)(op >> 8);
    b[1] = (uint8_t)op;
    return 2;
}

static size_t put16(uint8_t *b, uint16_t v)
{
    b[0] = (uint8_t)(v >> 8);
    b[1] = (uint8_t)v;
    return 2;
}

static size_t put24(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)(v >> 16);
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)v;
    return 3;
}

static size_t put32(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)(v >> 24);
    put24(b + 1, v);
    return 4;
}

static uint16_t get16(const uint8_t *a) { return (uint16_t)(a[0] << 8 | a[1]); }

static uint32_t get32(const uint8_t *a) { return (uint32_t)a[0] << 24 | (uint32_t)a[1] << 16 | (uint32_t)a[2] << 8 | a[3]; }

/* A signed 24-bit field. */
static int32_t get24s(const uint8_t *a)
{
    int32_t v = (int32_t)((uint32_t)a[0] << 16 | (uint32_t)a[1] << 8 | a[2]);
    return v & 0x800000 ? v - 0x1000000 : v;
}

static int clampn(int n, int max) { return n < 0 ? 0 : n > max ? max : n; }

/* ── LoRa ──────────────────────────────────────────────────────────────────── */

size_t lr2021_lora_modulation(uint8_t *b, const lr2021_lora_mod_t *m)
{
    put_op(b, LR2021_SET_LORA_MODULATION);
    b[2] = (uint8_t)((m->sf & 0xF) << 4 | (m->bw & 0xF));
    b[3] = (uint8_t)((m->cr & 0xF) << 4 | (m->ldro ? 1 : 0)); /* bits 3:2 RFU */
    return 4;
}

size_t lr2021_lora_packet(uint8_t *b, const lr2021_lora_pkt_t *p)
{
    put_op(b, LR2021_SET_LORA_PACKET);
    put16(b + 2, p->preamble);
    b[4] = p->payload_len;
    b[5] = (uint8_t)((p->implicit ? 4 : 0) | (p->crc ? 2 : 0) | (p->invert_iq ? 1 : 0));
    return 6;
}

size_t lr2021_lora_synch_timeout(uint8_t *b, uint8_t code, uint8_t format)
{
    put_op(b, LR2021_SET_LORA_SYNCH_TIMEOUT);
    b[2] = code;
    b[3] = format;
    return 4;
}

uint32_t lr2021_lora_mant_exp(uint16_t n, uint8_t *code)
{
    /* the mantissa counts pairs of symbols; each exponent step is a factor of 4 */
    uint32_t mant = ((uint32_t)n + 1) >> 1;
    uint8_t exp = 0;
    while (mant > 31 && exp < 7) {
        mant = (mant + 3) >> 2;
        exp++;
    }
    if (mant > 31)
        mant = 31;
    *code = (uint8_t)(mant << 3 | exp);
    return mant << (2 * exp + 1);
}

size_t lr2021_lora_synch_timeout_symbols(uint8_t *b, uint16_t n)
{
    if (n <= 255)
        return lr2021_lora_synch_timeout(b, (uint8_t)n, LR2021_LORA_TIMEOUT_SYMBOLS);
    uint8_t code;
    lr2021_lora_mant_exp(n, &code);
    return lr2021_lora_synch_timeout(b, code, LR2021_LORA_TIMEOUT_MANT_EXP);
}

size_t lr2021_lora_syncword(uint8_t *b, uint8_t syncword) { return lr2021_cmd_u8(b, LR2021_SET_LORA_SYNCWORD, syncword); }

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

size_t lr2021_lora_side_det_config(uint8_t *b, const lr2021_lora_side_det_t *d, int n)
{
    size_t o = put_op(b, LR2021_SET_LORA_SIDE_DET_CONFIG);
    n = clampn(n, 3);
    for (int i = 0; i < n; i++)
        b[o++] = (uint8_t)((d[i].sf & 0xF) << 4 | (d[i].ldro ? 1 : 0) << 2 | (d[i].invert_iq ? 1 : 0));
    return o;
}

size_t lr2021_lora_side_det_syncword(uint8_t *b, const uint8_t *sw, int n)
{
    size_t o = put_op(b, LR2021_SET_LORA_SIDE_DET_SYNCWORD);
    n = clampn(n, 3);
    for (int i = 0; i < n; i++)
        b[o++] = sw[i];
    return o;
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

size_t lr2021_lora_side_det_cad(uint8_t *b, const uint8_t *pnr_delta, const uint8_t *det_peak, int n)
{
    size_t o = put_op(b, LR2021_SET_LORA_SIDE_DET_CAD);
    n = clampn(n, 3);
    for (int i = 0; i < n; i++) {
        b[o++] = pnr_delta[i] & 0xF;
        b[o++] = det_peak[i];
    }
    return o;
}

size_t lr2021_lora_cad_params(uint8_t *b, const lr2021_lora_cad_t *c)
{
    put_op(b, LR2021_SET_LORA_CAD_PARAMS);
    b[2] = c->symbols;
    b[3] = (uint8_t)((c->preamble_only ? 0x10 : 0) | (c->pnr_delta & 0xF));
    b[4] = c->exit_mode;
    put24(b + 5, c->timeout & 0xFFFFFF);
    b[8] = c->det_peak;
    return 9;
}

size_t lr2021_lora_cad(uint8_t *b) { return lr2021_cmd(b, LR2021_SET_LORA_CAD); }

uint8_t lr2021_lora_cad_det_peak(uint8_t sf, uint8_t symbols)
{
    /* table 6-19: a row per symbol count (1 to 4), a column per SF (5 to 12) */
    static const uint8_t peak[4][8] = {
        {60, 60, 60, 64, 64, 66, 70, 74},
        {56, 56, 56, 58, 58, 60, 64, 68},
        {51, 51, 52, 54, 56, 60, 60, 65},
        {51, 51, 51, 54, 56, 60, 60, 64},
    };
    if (sf < 5 || sf > 12)
        return 0;
    const int row = symbols <= 1 ? 0 : symbols >= 4 ? 3 : symbols - 1;
    return peak[row][sf - 5];
}

size_t lr2021_lora_address(uint8_t *b, uint8_t pos, const uint8_t *addr, uint8_t len)
{
    size_t o = put_op(b, LR2021_SET_LORA_ADDRESS);
    if (len > 8)
        len = 8;
    b[o++] = (uint8_t)(len << 4 | (pos & 0xF));
    for (int i = 0; i < len; i++)
        b[o++] = addr[i];
    return o;
}

size_t lr2021_lora_hopping(uint8_t *b, uint16_t period, const uint32_t *hz, int n)
{
    size_t o = put_op(b, LR2021_SET_LORA_HOPPING);
    n = clampn(n, LR2021_LORA_HOPS_MAX);
    b[o++] = (uint8_t)((n ? 1 : 0) << 6 | ((period >> 8) & 0x1F)); /* hop_ctrl in bits 7:6, period bits 12:8 */
    b[o++] = (uint8_t)period;
    for (int i = 0; i < n; i++)
        o += put32(b + o, hz[i]);
    return o;
}

size_t lr2021_lora_tx_sync(uint8_t *b, uint8_t function, uint8_t dio)
{
    return lr2021_cmd_u8(b, LR2021_SET_LORA_TX_SYNC, (uint8_t)((function & 0x3) << 6 | (dio & 0x3F)));
}

size_t lr2021_lora_get_rx_stats(uint8_t *b) { return lr2021_cmd(b, LR2021_GET_LORA_RX_STATS); }

void lr2021_lora_parse_rx_stats(const uint8_t *a, lr2021_lora_rx_stats_t *s)
{
    s->packets = get16(a);
    s->crc_errors = get16(a + 2);
    s->header_errors = get16(a + 4);
    s->headers_ok = get16(a + 6);
    s->false_syncs = get16(a + 8);
}

size_t lr2021_lora_get_packet_status(uint8_t *b) { return lr2021_cmd(b, LR2021_GET_LORA_PACKET_STATUS); }

void lr2021_lora_parse_packet_status(const uint8_t *a, lr2021_lora_packet_status_t *s)
{
    s->crc = (a[0] >> 4) & 1;
    s->cr = a[0] & 0xF;
    s->length = a[1];
    s->snr_db = (int8_t)a[2] / 4.0f;
    /* 9-bit powers: bits 8:1 in their own byte, bit 0 in byte 5 (bit 1 the packet's, bit 0 the signal's) */
    s->rssi_dbm = -(float)(a[3] << 1 | ((a[5] >> 1) & 1)) / 2.0f;
    s->signal_rssi_dbm = -(float)(a[4] << 1 | (a[5] & 1)) / 2.0f;
    s->detectors = (a[5] >> 2) & 0xF;
    s->freq_offset_hz = get24s(a + 6);
}

/* ── LoRa helpers ──────────────────────────────────────────────────────────── */

double lr2021_lora_bw_hz(uint8_t bw)
{
    switch (bw) {
    case LR2021_LORA_BW_31: return 31250.0;
    case LR2021_LORA_BW_62: return 62500.0;
    case LR2021_LORA_BW_125: return 125000.0;
    case LR2021_LORA_BW_250: return 250000.0;
    case LR2021_LORA_BW_500: return 500000.0;
    case LR2021_LORA_BW_1000: return 1000000.0;
    case LR2021_LORA_BW_41: return 125000.0 / 3.0;
    case LR2021_LORA_BW_83: return 250000.0 / 3.0;
    case LR2021_LORA_BW_101: return 101562.5;
    case LR2021_LORA_BW_203: return 203125.0;
    case LR2021_LORA_BW_406: return 406250.0;
    case LR2021_LORA_BW_812: return 812500.0;
    default: return 0;
    }
}

int lr2021_lora_cr_denominator(uint8_t cr)
{
    static const uint8_t den[10] = {0, 5, 6, 7, 8, 5, 6, 8, 6, 8};
    return cr < 10 ? den[cr] : 0;
}

bool lr2021_lora_cr_long(uint8_t cr) { return cr >= LR2021_LORA_CR_LI_4_5 && cr <= LR2021_LORA_CR_CC_4_8; }

bool lr2021_lora_ldro(uint8_t sf, uint8_t bw)
{
    if (sf < 11 || bw == LR2021_LORA_BW_500 || bw == LR2021_LORA_BW_1000)
        return false;
    if (bw == LR2021_LORA_BW_250)
        return sf == 12;
    return true;
}

double lr2021_lora_symbol_us(uint8_t sf, uint8_t bw)
{
    const double hz = lr2021_lora_bw_hz(bw);
    return hz > 0 ? (double)(1u << (sf & 0xF)) * 1e6 / hz : 0;
}

static int ceil_div(int num, int den) { return (num + den - 1) / den; }

double lr2021_lora_toa_symbols(const lr2021_lora_mod_t *m, const lr2021_lora_pkt_t *p)
{
    const int sf = m->sf;
    const int fine = sf <= 6;                         /* SF5 and SF6 add two sync symbols */
    const int bytes = p->payload_len + (p->crc ? 2 : 0);
    const int bits_sym = sf - (m->ldro ? 2 : 0);      /* bits a symbol carries */
    int data;
    if (lr2021_lora_cr_long(m->cr)) {
        /* the long interleaver codes whole payload bits at the code rate, after an 8-symbol header block */
        const int den = lr2021_lora_cr_denominator(m->cr);
        if (p->implicit) {
            const int start = sf - 2 + 2 * fine; /* bits in each of the first 8 symbols */
            if (8 * bytes * den <= 7 * 4 * start)
                data = ceil_div(8 * bytes * den, 4 * start);
            else
                data = ceil_div(8 * 4 * bits_sym + 8 * bytes * den - 4 * 8 * start, 4 * bits_sym);
        } else {
            int head = (sf * 4 + fine * 8 - 28) & ~7; /* payload bits carried in the header block */
            if (head < 8 * bytes && head > 8 * p->payload_len)
                head = 8 * p->payload_len;
            int pay = 8 * bytes - head;
            if (pay < 0)
                pay = 0;
            data = ceil_div(pay * den + 8 * 4 * bits_sym, 4 * bits_sym);
        }
    } else {
        /* the classic formula: 8 header symbols at 4/8, then blocks of cr + 4 symbols */
        int head = sf * 4 + fine * 8 - 8 - (p->implicit ? 0 : 20);
        int pay = 8 * bytes - head;
        if (pay < 0)
            pay = 0;
        data = ceil_div(pay, 4 * bits_sym) * (m->cr + 4) + 8;
    }
    return p->preamble + 4.25 + 2 * fine + data;
}

double lr2021_lora_toa_us(const lr2021_lora_mod_t *m, const lr2021_lora_pkt_t *p)
{
    return lr2021_lora_toa_symbols(m, p) * lr2021_lora_symbol_us(m->sf, m->bw);
}

uint32_t lr2021_lora_toa_ms(const lr2021_lora_mod_t *m, const lr2021_lora_pkt_t *p)
{
    return (uint32_t)ceil(lr2021_lora_toa_us(m, p) / 1000.0 - 1e-9);
}

/* ── LR-FHSS ───────────────────────────────────────────────────────────────── */

size_t lr2021_lr_fhss_build_frame(uint8_t *b, const lr2021_lr_fhss_t *p, const uint8_t *payload, uint8_t n)
{
    size_t o = put_op(b, LR2021_LR_FHSS_BUILD_FRAME);
    b[o++] = (uint8_t)((p->headers & 0xF) << 4 | (p->cr & 0xF));
    b[o++] = (uint8_t)(p->grid & 0xF); /* mod_type 0 (GMSK 488 b/s) in bits 7:4 */
    b[o++] = (uint8_t)((p->hopping & 0xF) << 4 | (p->bw & 0xF));
    o += put16(b + o, p->sequence & 0x1FF);
    b[o++] = (uint8_t)p->offset;
    for (int i = 0; i < n; i++)
        b[o++] = payload[i];
    return o;
}

size_t lr2021_lr_fhss_syncword(uint8_t *b, uint32_t syncword)
{
    put_op(b, LR2021_LR_FHSS_SET_SYNCWORD);
    return 2 + put32(b + 2, syncword);
}

size_t lr2021_lr_fhss_read_table(uint8_t *b) { return lr2021_cmd(b, LR2021_READ_LR_FHSS_HOPPING_TABLE); }

int lr2021_lr_fhss_parse_table(const uint8_t *a, size_t len, lr2021_lr_fhss_table_t *t)
{
    t->n = 0;
    if (len < 5)
        return 0;
    t->hopping = a[0] & 1;
    t->freq_hz = (a[0] >> 7) & 1;
    t->pkt_length = get16(a + 1);
    t->used_freqs = a[3];
    t->blocks = a[4];
    int n = t->hopping ? (t->used_freqs < t->blocks ? t->used_freqs : t->blocks) : 1;
    n = clampn(n, LR2021_LR_FHSS_HOPS_MAX);
    const int fits = (int)((len - 5) / 6);
    if (n > fits)
        n = fits;
    for (int i = 0; i < n; i++) {
        t->hop[i].freq = get32(a + 5 + 6 * i);
        t->hop[i].symbols = get16(a + 9 + 6 * i);
    }
    t->n = n;
    return n;
}

size_t lr2021_lr_fhss_write_table(uint8_t *b, const lr2021_lr_fhss_table_t *t)
{
    size_t o = put_op(b, LR2021_WRITE_LR_FHSS_HOPPING_TABLE);
    b[o++] = (uint8_t)((t->freq_hz ? 0x80 : 0) | (t->hopping ? 1 : 0));
    o += put16(b + o, t->pkt_length);
    b[o++] = t->used_freqs;
    b[o++] = t->blocks;
    const int n = clampn(t->n, LR2021_LR_FHSS_HOPS_MAX);
    for (int i = 0; i < n; i++) {
        o += put32(b + o, t->hop[i].freq);
        o += put16(b + o, t->hop[i].symbols);
    }
    return o;
}

double lr2021_lr_fhss_bw_hz(uint8_t bw)
{
    /* table 17.2.1's spans, each a whole number of 3.90625 kHz steps (10, 22, 35, 47, 86, 99, 185, 198, 390, 403) */
    static const double hz[10] = {39062.5, 85937.5, 136718.75, 183593.75, 335937.5,
                                  386718.75, 722656.25, 773437.5, 1523437.5, 1574218.75};
    return bw < 10 ? hz[bw] : 0;
}

uint32_t lr2021_lr_fhss_bits(const lr2021_lr_fhss_t *p, uint16_t n)
{
    /* the payload, a 16-bit CRC and a 6-bit tail, coded */
    uint32_t bits = ((uint32_t)n + 2) * 8 + 6;
    switch (p->cr) {
    case LR2021_LR_FHSS_CR_5_6: bits = (bits * 6 + 4) / 5; break;
    case LR2021_LR_FHSS_CR_2_3: bits = bits * 3 / 2; break;
    case LR2021_LR_FHSS_CR_1_2: bits *= 2; break;
    default: bits *= 3; break;
    }
    /* 48-bit fragments, each with 2 bits of block preamble; a short last one too */
    uint32_t air = bits / 48 * 50;
    if (bits % 48)
        air += bits % 48 + 2;
    return 114u * p->headers + air;
}

double lr2021_lr_fhss_toa_us(const lr2021_lr_fhss_t *p, uint16_t n)
{
    return lr2021_lr_fhss_bits(p, n) * 1e6 / LR2021_LR_FHSS_BITRATE;
}

uint32_t lr2021_lr_fhss_toa_ms(const lr2021_lr_fhss_t *p, uint16_t n)
{
    /* 1000 / 488.28125 = 256 / 125 exactly */
    return (lr2021_lr_fhss_bits(p, n) * 256u + 124u) / 125u;
}

unsigned lr2021_lr_fhss_sequences(const lr2021_lr_fhss_t *p)
{
    if (p->grid == LR2021_LR_FHSS_GRID_25 || (p->grid == LR2021_LR_FHSS_GRID_4 && p->bw < LR2021_LR_FHSS_BW_336))
        return 384;
    return 512;
}

/* ── RTToF ─────────────────────────────────────────────────────────────────── */

size_t lr2021_rttof_addr(uint8_t *b, uint32_t addr, uint8_t check_len)
{
    put_op(b, LR2021_SET_RANGING_ADDR);
    put32(b + 2, addr);
    b[6] = check_len;
    return 7;
}

size_t lr2021_rttof_req_addr(uint8_t *b, uint32_t addr)
{
    put_op(b, LR2021_SET_RANGING_REQ_ADDR);
    return 2 + put32(b + 2, addr);
}

size_t lr2021_rttof_tx_rx_delay(uint8_t *b, uint32_t delay)
{
    put_op(b, LR2021_SET_RANGING_TX_RX_DELAY);
    return 2 + put32(b + 2, delay);
}

size_t lr2021_rttof_params(uint8_t *b, bool extended, bool spy, uint8_t symbols)
{
    return lr2021_cmd_u8(b, LR2021_SET_RANGING_PARAMS,
                         (uint8_t)((extended ? 0x80 : 0) | (spy ? 0x40 : 0) | (symbols & 0x3F)));
}

size_t lr2021_rttof_get_result(uint8_t *b, uint8_t type) { return lr2021_cmd_u8(b, LR2021_GET_RANGING_RESULT, type); }

void lr2021_rttof_parse_result(const uint8_t *a, bool extended, lr2021_rttof_result_t *r)
{
    r->rng1 = get24s(a);
    r->rssi1_dbm = -(float)a[3] / 2.0f;
    r->rng2 = extended ? get24s(a + 4) : 0;
    r->rssi2_dbm = extended ? -(float)a[7] / 2.0f : 0.0f;
}

double lr2021_rttof_distance_m(int32_t rng, uint8_t bw)
{
    const double mhz = lr2021_lora_bw_hz(bw) / 1e6;
    return mhz > 0 ? rng * 150.0 / (4096.0 * mhz) : 0;
}

size_t lr2021_rttof_get_stats(uint8_t *b) { return lr2021_cmd(b, LR2021_GET_RANGING_STATS); }

void lr2021_rttof_parse_stats(const uint8_t *a, lr2021_rttof_stats_t *s)
{
    s->exchanges_ok = get16(a);
    s->requests_ok = get16(a + 2);
    s->responses = get16(a + 4);
    s->timeouts = get16(a + 6);
    s->discarded = get16(a + 8);
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
