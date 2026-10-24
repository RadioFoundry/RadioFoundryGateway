// SPDX-License-Identifier: MIT AND BSD-3-Clause-Clear
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * LR2021 generic modems ((G)FSK, OOK, FLRC, BPSK; lr2021_fsk.h): frame builders, answer parsers and time on
 * air. No I/O here.
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
 */

#include "lr2021_fsk.h"

#include <math.h>

static size_t put_op(uint8_t *b, uint16_t op)
{
    b[0] = (uint8_t)(op >> 8);
    b[1] = (uint8_t)op;
    return 2;
}

static void put16(uint8_t *b, uint16_t v)
{
    b[0] = (uint8_t)(v >> 8);
    b[1] = (uint8_t)v;
}

static void put24(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)(v >> 16);
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)v;
}

static void put32(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)(v >> 24);
    put24(b + 1, v);
}

static uint16_t get16(const uint8_t *a) { return (uint16_t)(a[0] << 8 | a[1]); }

/* A frame that is the opcode and two 32-bit words: the CRC commands. */
static size_t op_two_words(uint8_t *b, uint16_t op, uint32_t w0, uint32_t w1)
{
    put_op(b, op);
    put32(b + 2, w0);
    put32(b + 6, w1);
    return 10;
}

static size_t op_two_bytes(uint8_t *b, uint16_t op, uint8_t x, uint8_t y)
{
    put_op(b, op);
    b[2] = x;
    b[3] = y;
    return 4;
}

/* time = bits / rate, rounded up to a microsecond; the rate a bit rate word */
static uint32_t bits_to_us(uint32_t bits, uint32_t word)
{
    uint64_t num = (uint64_t)bits * 1000000u, den = word;
    if (word & LR2021_FSK_BITRATE_FRAC) {
        num *= 256u;
        den = word & 0x7FFFFFFFu;
    }
    return den ? (uint32_t)((num + den - 1) / den) : 0;
}

/* ── Shared ──────────────────────────────────────────────────────────────── */

uint32_t lr2021_fsk_bitrate_frac(double bps)
{
    return LR2021_FSK_BITRATE_FRAC | ((uint32_t)llround(bps * 256.0) & 0x7FFFFFFFu);
}

double lr2021_fsk_bitrate_bps(uint32_t word)
{
    return (word & LR2021_FSK_BITRATE_FRAC) ? (word & 0x7FFFFFFFu) / 256.0 : (double)word;
}

/* Table 11-4's codes, narrowest first. */
static const uint8_t rx_bw_codes[90] = {
    231, 167, 223, 103, 39,  159, 95,  215, 31,  230, 87,  23,  166, 222, 102, 38,  158, 94,
    30,  229, 86,  165, 101, 37,  157, 93,  213, 29,  228, 85,  21,  164, 220, 100, 36,  156,
    92,  212, 28,  227, 84,  20,  163, 219, 99,  35,  155, 91,  211, 27,  226, 83,  19,  162,
    218, 98,  34,  154, 90,  210, 26,  225, 82,  18,  161, 217, 97,  33,  153, 89,  209, 25,
    224, 81,  17,  160, 216, 96,  32,  152, 88,  208, 24,  144, 200, 136, 192, 128, 64,  0,
};

uint32_t lr2021_fsk_rx_bw_hz(uint8_t code)
{
    static const uint32_t filter[4] = {13, 14, 15, 18};
    for (int i = 0; i < 90; i++) {
        if (rx_bw_codes[i] != code)
            continue;
        /* 32 MHz x 5 / (4 x 2^d x m x the filter's divisor) */
        const uint32_t den = (4u << (code & 7)) * (((code >> 3) & 7) + 1u) * filter[code >> 6];
        return (160000000u + den / 2) / den;
    }
    return 0;
}

int lr2021_fsk_rx_bw_code(uint32_t hz)
{
    for (int i = 0; i < 90; i++)
        if (lr2021_fsk_rx_bw_hz(rx_bw_codes[i]) >= hz)
            return rx_bw_codes[i];
    return -1;
}

float lr2021_fsk_rssi_dbm(uint8_t hi8, uint8_t bit0) { return -(float)(hi8 * 2 + (bit0 & 1)) / 2.0f; }

/* ── (G)FSK ──────────────────────────────────────────────────────────────── */

size_t lr2021_fsk_modulation(uint8_t *b, uint32_t bitrate, uint8_t shape, uint8_t rx_bw, uint32_t fdev_hz)
{
    put_op(b, LR2021_SET_FSK_MODULATION);
    put32(b + 2, bitrate);
    b[6] = shape & 0xF;
    b[7] = rx_bw;
    put24(b + 8, fdev_hz & 0xFFFFFF);
    return 11;
}

size_t lr2021_fsk_packet(uint8_t *b, const lr2021_fsk_packet_t *p)
{
    put_op(b, LR2021_SET_FSK_PACKET);
    put16(b + 2, p->preamble_bits);
    b[4] = p->detect_bits;
    /* rfu(1:0), long_preamble_mode, pld_lenUnit, addr_comp(1:0), pkt_format(1:0) (table 11-5) */
    b[5] = (uint8_t)((p->long_preamble ? 1 << 5 : 0) | (p->len_in_bits ? 1 << 4 : 0) | (p->addr & 3) << 2 |
                     (p->format & 3));
    put16(b + 6, p->payload_len);
    b[8] = (uint8_t)((p->crc & 0xF) << 4 | (p->whitening ? 1 : 0));
    return 9;
}

size_t lr2021_fsk_whitening(uint8_t *b, uint8_t type, uint16_t seed)
{
    return op_two_bytes(b, LR2021_SET_FSK_WHITENING, (uint8_t)((type & 0xF) << 4 | ((seed >> 8) & 0xF)),
                        (uint8_t)seed);
}

size_t lr2021_fsk_crc(uint8_t *b, uint32_t poly, uint32_t init)
{
    return op_two_words(b, LR2021_SET_FSK_CRC, poly, init);
}

size_t lr2021_fsk_syncword(uint8_t *b, uint64_t sync, uint8_t bits, bool msb_first)
{
    if (bits > 64)
        bits = 64;
    /* the chip sends the low bits bits of the 64 (11.3.5): the rest go as zeros */
    if (bits < 64)
        sync &= ((uint64_t)1 << bits) - 1;
    put_op(b, LR2021_SET_FSK_SYNCWORD);
    put32(b + 2, (uint32_t)(sync >> 32));
    put32(b + 6, (uint32_t)sync);
    b[10] = (uint8_t)((msb_first ? 0x80 : 0) | bits);
    return 11;
}

size_t lr2021_fsk_address(uint8_t *b, uint8_t node, uint8_t bcast)
{
    return op_two_bytes(b, LR2021_SET_FSK_ADDRESS, node, bcast);
}

size_t lr2021_fsk_get_rx_stats(uint8_t *b) { return lr2021_cmd(b, LR2021_GET_FSK_RX_STATS); }

void lr2021_fsk_parse_rx_stats(const uint8_t *a, lr2021_fsk_rx_stats_t *s)
{
    s->packets = get16(a);
    s->crc_errors = get16(a + 2);
    s->len_errors = get16(a + 4);
    s->preambles = get16(a + 6);
    s->sync_ok = get16(a + 8);
    s->sync_fail = get16(a + 10);
    s->timeouts = get16(a + 12);
}

size_t lr2021_fsk_get_packet_status(uint8_t *b) { return lr2021_cmd(b, LR2021_GET_FSK_PACKET_STATUS); }

void lr2021_fsk_parse_packet_status(const uint8_t *a, lr2021_fsk_packet_status_t *s)
{
    /* byte 4: rfu(1:0), AddrMatchBcast, AddrMatchNode, rfu, rssi_avg(0), rfu, rssi_sync(0) (table 11-18) */
    s->len = get16(a);
    s->rssi_avg_dbm = lr2021_fsk_rssi_dbm(a[2], a[4] >> 2);
    s->rssi_sync_dbm = lr2021_fsk_rssi_dbm(a[3], a[4]);
    s->addr_bcast = (a[4] >> 5) & 1;
    s->addr_node = (a[4] >> 4) & 1;
    s->lqi_db = a[5] * 0.25f;
}

static const uint8_t header_bits[4] = {0, 8, 9, 16};

uint32_t lr2021_fsk_packet_bits(const lr2021_fsk_packet_t *p, uint8_t sync_bits)
{
    const bool variable = (p->format & 3) != LR2021_FSK_FORMAT_FIXED;
    const uint32_t payload = p->len_in_bits ? p->payload_len : p->payload_len * 8u;
    return p->preamble_bits + sync_bits + header_bits[p->format & 3] +
           (variable && p->addr != LR2021_FSK_ADDR_OFF ? 8u : 0u) + payload + (p->crc & 7) * 8u;
}

uint32_t lr2021_fsk_time_on_air_us(const lr2021_fsk_packet_t *p, uint8_t sync_bits, uint32_t bitrate)
{
    return bits_to_us(lr2021_fsk_packet_bits(p, sync_bits), bitrate);
}

/* ── OOK ─────────────────────────────────────────────────────────────────── */

size_t lr2021_ook_modulation(uint8_t *b, uint32_t bitrate, uint8_t shape, uint8_t rx_bw, uint8_t depth)
{
    put_op(b, LR2021_SET_OOK_MODULATION);
    put32(b + 2, bitrate);
    b[6] = shape & 0xF;
    b[7] = rx_bw;
    b[8] = depth & 1;
    return 9;
}

size_t lr2021_ook_packet(uint8_t *b, const lr2021_ook_packet_t *p)
{
    put_op(b, LR2021_SET_OOK_PACKET);
    put16(b + 2, p->preamble_bits);
    b[4] = (uint8_t)((p->addr & 3) << 2 | (p->format & 3));
    put16(b + 5, p->payload_len);
    b[7] = (uint8_t)((p->crc & 0xF) << 4 | (p->encoding & 0xF));
    return 8;
}

size_t lr2021_ook_crc(uint8_t *b, uint32_t poly, uint32_t init)
{
    return op_two_words(b, LR2021_SET_OOK_CRC, poly, init);
}

size_t lr2021_ook_syncword(uint8_t *b, uint32_t sync, uint8_t bits, bool msb_first)
{
    if (bits > 32)
        bits = 32;
    if (bits < 32)
        sync &= ((uint32_t)1 << bits) - 1;
    put_op(b, LR2021_SET_OOK_SYNCWORD);
    put32(b + 2, sync);
    b[6] = (uint8_t)((msb_first ? 0x80 : 0) | bits);
    return 7;
}

size_t lr2021_ook_address(uint8_t *b, uint8_t node, uint8_t bcast)
{
    return op_two_bytes(b, LR2021_SET_OOK_ADDRESS, node, bcast);
}

size_t lr2021_ook_detector(uint8_t *b, const lr2021_ook_detector_t *d)
{
    put_op(b, LR2021_SET_OOK_DETECTOR);
    put16(b + 2, d->pattern);
    b[4] = (uint8_t)((d->pattern_bits ? d->pattern_bits - 1 : 0) & 0xF);
    b[5] = d->repeats & 0x1F;
    /* rfu(1:0), sw_is_raw, sfd_kind, sfd_length(3:0) (table 16-10) */
    b[6] = (uint8_t)((d->sync_raw ? 1 << 5 : 0) | (d->sfd_rising ? 1 << 4 : 0) | (d->sfd_bits & 0xF));
    return 7;
}

size_t lr2021_ook_whitening(uint8_t *b, uint8_t bit_idx, uint16_t poly, uint16_t seed)
{
    put_op(b, LR2021_SET_OOK_WHITENING);
    b[2] = (uint8_t)((bit_idx & 0xF) << 4 | ((poly >> 8) & 0xF));
    b[3] = (uint8_t)poly;
    b[4] = (seed >> 8) & 0xF;
    b[5] = (uint8_t)seed;
    return 6;
}

size_t lr2021_ook_get_rx_stats(uint8_t *b) { return lr2021_cmd(b, LR2021_GET_OOK_RX_STATS); }

void lr2021_ook_parse_rx_stats(const uint8_t *a, lr2021_ook_rx_stats_t *s)
{
    s->packets = get16(a);
    s->crc_errors = get16(a + 2);
    s->len_errors = get16(a + 4);
}

size_t lr2021_ook_get_packet_status(uint8_t *b) { return lr2021_cmd(b, LR2021_GET_OOK_PACKET_STATUS); }

void lr2021_ook_parse_packet_status(const uint8_t *a, lr2021_ook_packet_status_t *s)
{
    /* byte 4 as FSK's, RssiHigh in place of rssi_sync (table 16-9) */
    s->len = get16(a);
    s->rssi_avg_dbm = lr2021_fsk_rssi_dbm(a[2], a[4] >> 2);
    s->rssi_high_dbm = lr2021_fsk_rssi_dbm(a[3], a[4]);
    s->addr_bcast = (a[4] >> 5) & 1;
    s->addr_node = (a[4] >> 4) & 1;
    s->lqi_db = a[5] * 0.25f;
}

uint32_t lr2021_ook_packet_bits(const lr2021_ook_packet_t *p, uint8_t sync_bits)
{
    const bool variable = (p->format & 3) != LR2021_FSK_FORMAT_FIXED;
    const uint32_t coded = sync_bits + header_bits[p->format & 3] +
                           (variable && p->addr != LR2021_FSK_ADDR_OFF ? 8u : 0u) + p->payload_len * 8u +
                           (p->crc & 7) * 8u;
    return p->preamble_bits + coded * (p->encoding != LR2021_OOK_ENC_NONE ? 2u : 1u);
}

uint32_t lr2021_ook_time_on_air_us(const lr2021_ook_packet_t *p, uint8_t sync_bits, uint32_t bitrate)
{
    return bits_to_us(lr2021_ook_packet_bits(p, sync_bits), bitrate);
}

/* ── FLRC ────────────────────────────────────────────────────────────────── */

uint32_t lr2021_flrc_bitrate(uint8_t code)
{
    static const uint32_t kbps[8] = {2600, 2080, 1300, 1040, 650, 520, 325, 260};
    return code < 8 ? kbps[code] * 1000u : 0;
}

uint32_t lr2021_flrc_bw_hz(uint8_t code)
{
    static const uint32_t khz[8] = {2666, 2222, 1333, 1333, 740, 571, 357, 307};
    return code < 8 ? khz[code] * 1000u : 0;
}

size_t lr2021_flrc_modulation(uint8_t *b, uint8_t rate, uint8_t cr, uint8_t shape)
{
    return op_two_bytes(b, LR2021_SET_FLRC_MODULATION, rate, (uint8_t)((cr & 0xF) << 4 | (shape & 0xF)));
}

static uint8_t flrc_preamble_code(uint8_t bits)
{
    return bits < 4 ? 0 : bits >= 32 ? 7 : (uint8_t)(bits / 4 - 1);
}

size_t lr2021_flrc_packet(uint8_t *b, const lr2021_flrc_packet_t *p)
{
    const uint8_t sync = p->sync_bits == 32 ? 2 : p->sync_bits == 16 ? 1 : 0;
    const uint8_t crc = p->crc_bytes >= 2 && p->crc_bytes <= 4 ? (uint8_t)(p->crc_bytes - 1) : 0;
    put_op(b, LR2021_SET_FLRC_PACKET);
    /* rfu(1:0), agc_pbl_len(3:0), sync_len(1:0); sync_tx(1:0), sync_match(2:0), pkt_format, Crc(1:0) (table 18-5) */
    b[2] = (uint8_t)(flrc_preamble_code(p->preamble_bits) << 2 | sync);
    b[3] = (uint8_t)((p->sync_tx & 3) << 6 | (p->sync_match & 7) << 3 | (p->fixed ? 1 << 2 : 0) | crc);
    put16(b + 4, p->payload_len);
    return 6;
}

size_t lr2021_flrc_syncword(uint8_t *b, uint8_t num, uint32_t sync, uint8_t bits)
{
    put_op(b, LR2021_SET_FLRC_SYNCWORD);
    b[2] = num;
    if (bits == 16) {
        put16(b + 3, (uint16_t)sync);
        return 5;
    }
    put32(b + 3, sync);
    return 7;
}

size_t lr2021_flrc_get_rx_stats(uint8_t *b) { return lr2021_cmd(b, LR2021_GET_FLRC_RX_STATS); }

void lr2021_flrc_parse_rx_stats(const uint8_t *a, lr2021_flrc_rx_stats_t *s) { lr2021_ook_parse_rx_stats(a, s); }

size_t lr2021_flrc_get_packet_status(uint8_t *b) { return lr2021_cmd(b, LR2021_GET_FLRC_PACKET_STATUS); }

void lr2021_flrc_parse_packet_status(const uint8_t *a, lr2021_flrc_packet_status_t *s)
{
    /* byte 4: sw_num(3:0), rfu, rssi_avg(0), rfu, rssi_sync(0) (table 18-9) */
    s->len = get16(a);
    s->rssi_avg_dbm = lr2021_fsk_rssi_dbm(a[2], a[4] >> 2);
    s->rssi_sync_dbm = lr2021_fsk_rssi_dbm(a[3], a[4]);
    s->sync_num = a[4] >> 4;
    const uint32_t f = (uint32_t)a[5] << 16 | (uint32_t)a[6] << 8 | a[7];
    s->freq_offset_hz = (int32_t)(f ^ 0x800000u) - 0x800000; /* signed 24 bits */
}

uint32_t lr2021_flrc_packet_bits(const lr2021_flrc_packet_t *p, uint8_t cr)
{
    /* the coding rate as twelfths: 1/2 6, 3/4 9, 1 12, 2/3 8 */
    static const uint32_t twelfths[4] = {6, 9, 12, 8};
    if (cr > 3)
        return 0;
    const uint32_t coded = (p->fixed ? 0u : 16u) + (cr != LR2021_FLRC_CR_1 ? 6u : 0u) +
                           8u * ((p->crc_bytes >= 2 && p->crc_bytes <= 4 ? p->crc_bytes : 0u) + p->payload_len);
    const uint32_t sync = p->sync_bits == 32 ? 32u : p->sync_bits == 16 ? 16u : 0u;
    const uint32_t uncoded = (flrc_preamble_code(p->preamble_bits) + 1u) * 4u + 21u + sync;
    return uncoded + (12u * coded + twelfths[cr] - 1) / twelfths[cr];
}

uint32_t lr2021_flrc_time_on_air_us(const lr2021_flrc_packet_t *p, uint8_t rate, uint8_t cr)
{
    const uint32_t bps = lr2021_flrc_bitrate(rate);
    return bps ? bits_to_us(lr2021_flrc_packet_bits(p, cr), bps) : 0;
}

/* ── BPSK ────────────────────────────────────────────────────────────────── */

size_t lr2021_bpsk_modulation(uint8_t *b, uint32_t bitrate, uint8_t shape, bool diff, bool init_prev,
                              bool init_parity)
{
    put_op(b, LR2021_SET_BPSK_MODULATION);
    put32(b + 2, bitrate);
    /* pulse_shape(3:0), rfu, diff_mode, diff_mode_init(1:0): bit 1 the parity, bit 0 the previous (table 15-1) */
    b[6] = (uint8_t)((shape & 0xF) << 4 | (diff ? 1 << 2 : 0) | (init_parity ? 1 << 1 : 0) | (init_prev ? 1 : 0));
    return 7;
}

size_t lr2021_bpsk_packet(uint8_t *b, uint8_t len, uint8_t mode, bool sigfox_control, uint8_t sigfox_rank)
{
    /* rfu(1:0), Mode(1:0), rfu, bpsk_s_msg, bpsk_s_rank(1:0) (table 15-2) */
    return op_two_bytes(b, LR2021_SET_BPSK_PACKET, len,
                        (uint8_t)((mode & 3) << 4 | (sigfox_control ? 1 << 2 : 0) | (sigfox_rank & 3)));
}
