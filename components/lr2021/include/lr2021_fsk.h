// SPDX-License-Identifier: MIT AND BSD-3-Clause-Clear
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * LR2021 generic modems ((G)FSK, OOK, FLRC, BPSK): frame builders, answer parsers and time on air, as plain C with
 * no I/O.
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

/*
 * Checked against the datasheet (DS.LR20xx Rev. 2.2): FSK chapter 11, BPSK 15, OOK 16, FLRC 18. Where USP and the
 * datasheet differ, the note at the field says which is followed.
 *
 * Builders write the whole frame (opcode first, fields big-endian) into buf and return its length; buf must hold
 * LR2021_FSK_FRAME_MAX bytes. The parsers take a read's answer: the bytes after the two status bytes, as
 * lr2021_read returns them, LR2021_*_LEN of them. Select the modem with SetPacketType first: each command here
 * fails (CMD_FAIL in the next status) under another packet type.
 *
 * lr2021_cmd.h keeps its short forms (lr2021_cmd_fsk_modulation, _fsk_packet, _ook_modulation, _fsk_variant);
 * these are the full ones.
 */
#ifndef LR2021_FSK_H
#define LR2021_FSK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lr2021_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opcodes (DS 11.3, 15.2, 16.3, 18.4). 0x0240, 0x0241 and 0x0281 are in lr2021_cmd.h. */
enum {
    LR2021_SET_FSK_WHITENING = 0x0242,
    LR2021_SET_FSK_CRC = 0x0243,
    LR2021_SET_FSK_SYNCWORD = 0x0244,
    LR2021_SET_FSK_ADDRESS = 0x0245,
    LR2021_GET_FSK_RX_STATS = 0x0246,
    LR2021_GET_FSK_PACKET_STATUS = 0x0247,
    LR2021_SET_FLRC_MODULATION = 0x0248,
    LR2021_SET_FLRC_PACKET = 0x0249,
    LR2021_GET_FLRC_RX_STATS = 0x024A,
    LR2021_GET_FLRC_PACKET_STATUS = 0x024B,
    LR2021_SET_FLRC_SYNCWORD = 0x024C,
    LR2021_SET_BPSK_MODULATION = 0x0250,
    LR2021_SET_BPSK_PACKET = 0x0251,
    LR2021_SET_OOK_PACKET = 0x0282,
    LR2021_SET_OOK_CRC = 0x0283,
    LR2021_SET_OOK_SYNCWORD = 0x0284,
    LR2021_SET_OOK_ADDRESS = 0x0285,
    LR2021_GET_OOK_RX_STATS = 0x0286,
    LR2021_GET_OOK_PACKET_STATUS = 0x0287,
    LR2021_SET_OOK_DETECTOR = 0x0288,
    LR2021_SET_OOK_WHITENING = 0x0289,
};

/* SetPacketType's codes for FLRC and BPSK (table 8-1); FSK and OOK are in lr2021_cmd.h. */
#ifndef LR2021_PACKET_FLRC
#define LR2021_PACKET_FLRC 0x5
#endif
#ifndef LR2021_PACKET_BPSK
#define LR2021_PACKET_BPSK 0x6
#endif

/* The longest frame here: SetFskModulationParams and SetFskSyncword. */
#define LR2021_FSK_FRAME_MAX 11

/* ── Shared ──────────────────────────────────────────────────────────────── */

/* A bit rate word (FSK, OOK, BPSK; DS 11.3.1): bit 31 clear, bits a second; set, bits 30:0 in 1/256 bit/s. */
#define LR2021_FSK_BITRATE_FRAC 0x80000000u
/* The word for a fractional rate: LR2021_FSK_BITRATE_FRAC | round(bps x 256). Whole rates can go as they are. */
uint32_t lr2021_fsk_bitrate_frac(double bps);
/* A bit rate word's rate in bits a second. */
double lr2021_fsk_bitrate_bps(uint32_t word);

/* Pulse shapes. FSK (11.3.1) lists none and the Gaussians; OOK takes the FSK list (16.3.1); FLRC takes none, BT 0.3,
 * 0.5 and 1.0 (18.4.1). BPSK (15.2.1) lists all of them, raised cosine (RC) and root-raised cosine (RRC) too: those
 * are RFU for FSK in the datasheet, but the 4-FSK variant took 0x0C (spec R3.21). */
enum {
    LR2021_FSK_SHAPE_NONE = 0x0,
    LR2021_FSK_SHAPE_BT_2_0 = 0x2,
    LR2021_FSK_SHAPE_RRC_0_4 = 0x3,
    LR2021_FSK_SHAPE_BT_0_3 = 0x4,
    LR2021_FSK_SHAPE_BT_0_5 = 0x5,
    LR2021_FSK_SHAPE_BT_0_7 = 0x6,
    LR2021_FSK_SHAPE_BT_1_0 = 0x7,
    LR2021_FSK_SHAPE_RC_0_3 = 0x8,
    LR2021_FSK_SHAPE_RC_0_5 = 0x9,
    LR2021_FSK_SHAPE_RC_0_7 = 0xA,
    LR2021_FSK_SHAPE_RC_1_0 = 0xB,
    LR2021_FSK_SHAPE_RRC_0_3 = 0xC,
    LR2021_FSK_SHAPE_RRC_0_5 = 0xD,
    LR2021_FSK_SHAPE_RRC_0_7 = 0xE,
    LR2021_FSK_SHAPE_RRC_1_0 = 0xF,
};

/* rx_bw (table 11-4), shared by FSK, OOK and the protocol modems. A code is bits 2:0 d, bits 5:3 m - 1, bits 7:6 f;
 * its double-sided bandwidth is 32 MHz / (4 x 2^d) / m x 5 / (13, 14, 15 or 18 for f = 0..3): that gives every row
 * of the table to its 0.1 kHz. Hz for a listed code (rounded), 0 for one the table doesn't list. */
uint32_t lr2021_fsk_rx_bw_hz(uint8_t code);
/* The narrowest listed code at least hz wide; -1 past 3076.9 kHz. */
int lr2021_fsk_rx_bw_code(uint32_t hz);

/* CRC (table 11-9; OOK the same): bits 2:0 the length in bytes, bit 3 the CRC inverted. */
enum {
    LR2021_FSK_CRC_OFF = 0x0,
    LR2021_FSK_CRC_1 = 0x1,
    LR2021_FSK_CRC_2 = 0x2,
    LR2021_FSK_CRC_3 = 0x3,
    LR2021_FSK_CRC_4 = 0x4,
    LR2021_FSK_CRC_1_INV = 0x9,
    LR2021_FSK_CRC_2_INV = 0xA,
    LR2021_FSK_CRC_3_INV = 0xB,
    LR2021_FSK_CRC_4_INV = 0xC,
};
/* Address filtering (table 11-7; OOK the same). */
enum { LR2021_FSK_ADDR_OFF = 0, LR2021_FSK_ADDR_NODE = 1, LR2021_FSK_ADDR_NODE_BCAST = 2 };
/* Packet format (table 11-8): fixed length, or a length header of 8, 9 (SX128x) or 16 bits. OOK has no 9-bit one. */
enum {
    LR2021_FSK_FORMAT_FIXED = 0,
    LR2021_FSK_FORMAT_VAR_8 = 1,
    LR2021_FSK_FORMAT_VAR_9 = 2,
    LR2021_FSK_FORMAT_VAR_16 = 3,
};

/* An RSSI in a packet status: 9 bits, the top 8 in one byte and bit 0 elsewhere; the power is -rssi / 2 dBm. */
float lr2021_fsk_rssi_dbm(uint8_t hi8, uint8_t bit0);

/* ── (G)FSK (DS 11) ──────────────────────────────────────────────────────── */

/* SetFskModulationParams (11.3.1): 11 bytes. bitrate a bit rate word (500 b/s to 2 Mb/s, table 11-2), shape
 * LR2021_FSK_SHAPE_*, rx_bw a table 11-4 code, fdev in Hz (24 bits). The fractional form goes through, unlike
 * lr2021_cmd_fsk_modulation's. */
size_t lr2021_fsk_modulation(uint8_t *buf, uint32_t bitrate, uint8_t shape, uint8_t rx_bw, uint32_t fdev_hz);

/* SetFskPacketParams (11.3.2): 9 bytes, the same fields lr2021_cmd_fsk_packet zeroes. */
typedef struct {
    uint16_t preamble_bits; /* sent in TX */
    uint8_t detect_bits;    /* RX preamble detector: 0 (detect on the sync word), 8, 16, 24 or 32 (table 11-6) */
    bool long_preamble;     /* RX: preamble first, then the sync word (needed past 2000 preamble bits) */
    bool len_in_bits;       /* payload_len counts bits, not bytes */
    uint8_t addr;           /* LR2021_FSK_ADDR_* */
    uint8_t format;         /* LR2021_FSK_FORMAT_* */
    uint16_t payload_len;   /* fixed: the length; variable: the longest accepted */
    uint8_t crc;            /* LR2021_FSK_CRC_* */
    bool whitening;         /* dc_free: whiten as SetFskWhiteningParams says */
} lr2021_fsk_packet_t;
size_t lr2021_fsk_packet(uint8_t *buf, const lr2021_fsk_packet_t *p);

/* SetFskWhiteningParams (11.3.3): 4 bytes. type 0 SX126x / LR11xx, 1 SX128x; a 12-bit seed. */
enum { LR2021_FSK_WHITEN_SX126X = 0, LR2021_FSK_WHITEN_SX128X = 1 };
size_t lr2021_fsk_whitening(uint8_t *buf, uint8_t type, uint16_t seed);
/* SetFskCrcParams (11.3.4): 10 bytes. IBM: poly 0x8005, init 0xFFFF, LR2021_FSK_CRC_2; CCITT: 0x1021, 0x1D0F,
 * LR2021_FSK_CRC_2_INV. */
size_t lr2021_fsk_crc(uint8_t *buf, uint32_t poly, uint32_t init);
/* SetFskSyncword (11.3.5): 11 bytes. The sync word is the low bits bits of sync (up to 64); msb_first for SX126x,
 * LR11xx and SX128x. 0 bits: no sync word. */
size_t lr2021_fsk_syncword(uint8_t *buf, uint64_t sync, uint8_t bits, bool msb_first);
/* SetFskAddress (11.3.6): 4 bytes. */
size_t lr2021_fsk_address(uint8_t *buf, uint8_t node, uint8_t bcast);

/* GetFskRxStats (11.3.7): 2 bytes; the answer, 14 bytes. Counts reset on POR, sleep without retention and
 * ResetRxStats. */
#define LR2021_FSK_RX_STATS_LEN 14
typedef struct {
    uint16_t packets;    /* received */
    uint16_t crc_errors; /* received with a CRC error */
    uint16_t len_errors; /* with a length error */
    uint16_t preambles;  /* preambles detected */
    uint16_t sync_ok;    /* sync words found */
    uint16_t sync_fail;  /* sync words failed */
    uint16_t timeouts;   /* RX timeouts */
} lr2021_fsk_rx_stats_t;
size_t lr2021_fsk_get_rx_stats(uint8_t *buf);
void lr2021_fsk_parse_rx_stats(const uint8_t *a, lr2021_fsk_rx_stats_t *s);

/* GetFskPacketStatus (11.3.8): 2 bytes; the answer, 6 bytes. Updated at RxDone; rssi_sync already at SyncwordValid. */
#define LR2021_FSK_PACKET_STATUS_LEN 6
typedef struct {
    uint16_t len;        /* the last packet's bytes, with what the chip adds to the FIFO (CRC...) */
    float rssi_avg_dbm;  /* over the packet */
    float rssi_sync_dbm; /* at the sync word */
    bool addr_bcast;     /* matched the broadcast address */
    bool addr_node;      /* matched the node address */
    float lqi_db;        /* correlation peak over average power; over 10 dB is good */
} lr2021_fsk_packet_status_t;
size_t lr2021_fsk_get_packet_status(uint8_t *buf);
void lr2021_fsk_parse_packet_status(const uint8_t *a, lr2021_fsk_packet_status_t *s);

/* Bits on air for a packet (11.2): preamble, sync word, the length header (0, 8, 9 or 16 bits), an address byte
 * in a variable-length packet, the payload and the CRC. In a fixed-length packet the address is part of the
 * payload (11.2.1). */
uint32_t lr2021_fsk_packet_bits(const lr2021_fsk_packet_t *p, uint8_t sync_bits);
/* Its time on air in microseconds, rounded up, at a bit rate word; 0 for a zero rate. */
uint32_t lr2021_fsk_time_on_air_us(const lr2021_fsk_packet_t *p, uint8_t sync_bits, uint32_t bitrate);

/* ── OOK (DS 16) ─────────────────────────────────────────────────────────── */

/* SetOokModulationParams (16.3.1): 9 bytes. bitrate a bit rate word; shape and rx_bw as FSK's; depth 0 full (the
 * PA's), 1 up to 20 dB. The fractional form goes through, unlike lr2021_cmd_ook_modulation's. */
enum { LR2021_OOK_DEPTH_FULL = 0, LR2021_OOK_DEPTH_20DB = 1 };
size_t lr2021_ook_modulation(uint8_t *buf, uint32_t bitrate, uint8_t shape, uint8_t rx_bw, uint8_t depth);

/* SetOokPacketParams (16.3.2): 8 bytes. Encoding codes are USP's: the datasheet gives 0 off, 1 Manchester, 3
 * Manchester inverted, and no biphase; USP has bit 3 for inverted, as the CRC codes do. Semtech recommends
 * whitening or Manchester for OOK. */
enum {
    LR2021_OOK_ENC_NONE = 0x0,
    LR2021_OOK_ENC_MANCHESTER = 0x1,
    LR2021_OOK_ENC_BIPHASE = 0x2,
    LR2021_OOK_ENC_MANCHESTER_INV = 0x9,
    LR2021_OOK_ENC_BIPHASE_INV = 0xA,
};
typedef struct {
    uint16_t preamble_bits;
    uint8_t addr;         /* LR2021_FSK_ADDR_* */
    uint8_t format;       /* LR2021_FSK_FORMAT_FIXED, _VAR_8 or _VAR_16 */
    uint16_t payload_len; /* bytes: fixed, the length; variable, the longest accepted */
    uint8_t crc;          /* LR2021_FSK_CRC_* */
    uint8_t encoding;     /* LR2021_OOK_ENC_* */
} lr2021_ook_packet_t;
size_t lr2021_ook_packet(uint8_t *buf, const lr2021_ook_packet_t *p);

/* SetOokCrcParams (16.3.3): 10 bytes. */
size_t lr2021_ook_crc(uint8_t *buf, uint32_t poly, uint32_t init);
/* SetOokSyncword (16.3.4): 7 bytes. The low bits bits of sync (up to 32). */
size_t lr2021_ook_syncword(uint8_t *buf, uint32_t sync, uint8_t bits, bool msb_first);
/* SetOokAddress (16.3.5): 4 bytes. */
size_t lr2021_ook_address(uint8_t *buf, uint8_t node, uint8_t bcast);

/* SetOokDetector (16.3.8), RX only (in TX the pattern goes in the FIFO with the data): 7 bytes. */
typedef struct {
    uint16_t pattern;      /* the preamble pattern, first bit received in the LSB; its two LSBs 01 or 10 (ADS-B:
                            * 0x0285) */
    uint8_t pattern_bits;  /* its length, 1 to 16 (sent as length - 1) */
    uint8_t repeats;       /* 0 to 31; 31 turns the sync word search's timeout off (ADS-B: 0) */
    bool sync_raw;         /* the sync word isn't encoded as the payload is */
    bool sfd_rising;       /* the start-of-frame delimiter is a rising edge (ADS-B: falling) */
    uint8_t sfd_bits;      /* its length, 0 to 15 (ADS-B: 0) */
} lr2021_ook_detector_t;
size_t lr2021_ook_detector(uint8_t *buf, const lr2021_ook_detector_t *d);

/* SetOokWhiteningParams (16.3.9): 6 bytes. bit_idx the LFSR bit used (0 to 15), a 12-bit polynomial (0 turns
 * whitening off) and seed. */
size_t lr2021_ook_whitening(uint8_t *buf, uint8_t bit_idx, uint16_t poly, uint16_t seed);

/* GetOokRxStats (16.3.6): 2 bytes; the answer, 6 bytes. */
#define LR2021_OOK_RX_STATS_LEN 6
typedef struct {
    uint16_t packets;
    uint16_t crc_errors;
    uint16_t len_errors;
} lr2021_ook_rx_stats_t;
size_t lr2021_ook_get_rx_stats(uint8_t *buf);
void lr2021_ook_parse_rx_stats(const uint8_t *a, lr2021_ook_rx_stats_t *s);

/* GetOokPacketStatus (16.3.7): 2 bytes; the answer, 6 bytes. */
#define LR2021_OOK_PACKET_STATUS_LEN 6
typedef struct {
    uint16_t len;
    float rssi_avg_dbm;  /* over the packet */
    float rssi_high_dbm; /* over the high ('1') bits */
    bool addr_bcast;
    bool addr_node;
    float lqi_db;        /* in 0.25 dB steps (the datasheet; USP says whole dB) */
} lr2021_ook_packet_status_t;
size_t lr2021_ook_get_packet_status(uint8_t *buf);
void lr2021_ook_parse_packet_status(const uint8_t *a, lr2021_ook_packet_status_t *s);

/* Bits on air: the preamble, then sync word, header (0, 8 or 16 bits), address byte (variable length), payload
 * and CRC, doubled by Manchester or biphase coding. The time in microseconds, rounded up. */
uint32_t lr2021_ook_packet_bits(const lr2021_ook_packet_t *p, uint8_t sync_bits);
uint32_t lr2021_ook_time_on_air_us(const lr2021_ook_packet_t *p, uint8_t sync_bits, uint32_t bitrate);

/* ── FLRC (DS 18) ────────────────────────────────────────────────────────── */

/* Bit rate and bandwidth (18.4.1): raw kb/s and kHz double-sided. */
enum {
    LR2021_FLRC_2600 = 0, /* 2600 kb/s, 2666 kHz */
    LR2021_FLRC_2080 = 1, /* 2080, 2222 */
    LR2021_FLRC_1300 = 2, /* 1300, 1333 */
    LR2021_FLRC_1040 = 3, /* 1040, 1333 */
    LR2021_FLRC_650 = 4,  /* 650, 740 */
    LR2021_FLRC_520 = 5,  /* 520, 571 */
    LR2021_FLRC_325 = 6,  /* 325, 357 */
    LR2021_FLRC_260 = 7,  /* 260, 307 */
};
/* A code's raw bit rate in bits a second, and its bandwidth in Hz; 0 for an unknown code. */
uint32_t lr2021_flrc_bitrate(uint8_t code);
uint32_t lr2021_flrc_bw_hz(uint8_t code);
/* Coding rate (18.4.1). Rate 1 isn't recommended (table 18-2). */
enum { LR2021_FLRC_CR_1_2 = 0, LR2021_FLRC_CR_3_4 = 1, LR2021_FLRC_CR_1 = 2, LR2021_FLRC_CR_2_3 = 3 };

/* SetFlrcModulationParams (18.4.1): 4 bytes. shape LR2021_FSK_SHAPE_NONE, _BT_0_3, _BT_0_5 or _BT_1_0. */
size_t lr2021_flrc_modulation(uint8_t *buf, uint8_t rate, uint8_t cr, uint8_t shape);

/* SetFlrcPacketParams (18.4.2): 6 bytes, in real units; the builder sends the codes. */
typedef struct {
    uint8_t preamble_bits; /* AGC preamble: 4 to 32, a multiple of 4 */
    uint8_t sync_bits;     /* 0, 16 or 32; 0 if sync_match is 0 */
    uint8_t sync_tx;       /* the sync word sent: 0 none, 1 to 3 */
    uint8_t sync_match;    /* RX: a mask of the sync words searched for, bit 0 for 1 ... bit 2 for 3 */
    bool fixed;            /* fixed length; else a 16-bit length header */
    uint8_t crc_bytes;     /* 0, 2, 3 or 4 */
    uint16_t payload_len;  /* 6 to 511 bytes (127 for SX1280); variable: the longest accepted */
} lr2021_flrc_packet_t;
size_t lr2021_flrc_packet(uint8_t *buf, const lr2021_flrc_packet_t *p);

/* SetFlrcSyncword (18.4.5): sync word num (1 to 3), 16 or 32 bits (the low bits of sync): 5 or 7 bytes. */
size_t lr2021_flrc_syncword(uint8_t *buf, uint8_t num, uint32_t sync, uint8_t bits);

/* GetFlrcRxStats (18.4.3): 2 bytes; the answer, 6 bytes (USP reads 8, a count of good CRCs last; the datasheet
 * doesn't list it). */
#define LR2021_FLRC_RX_STATS_LEN 6
typedef lr2021_ook_rx_stats_t lr2021_flrc_rx_stats_t;
size_t lr2021_flrc_get_rx_stats(uint8_t *buf);
void lr2021_flrc_parse_rx_stats(const uint8_t *a, lr2021_flrc_rx_stats_t *s);

/* GetFlrcPacketStatus (18.4.4): 2 bytes; the answer, 8 bytes with the frequency offset (added in DS Rev. 2.2; USP
 * reads the first 5). */
#define LR2021_FLRC_PACKET_STATUS_LEN 8
typedef struct {
    uint16_t len;
    float rssi_avg_dbm;
    float rssi_sync_dbm;
    uint8_t sync_num;    /* the sync word found, 1 to 3 */
    int32_t freq_offset_hz;
} lr2021_flrc_packet_status_t;
size_t lr2021_flrc_get_packet_status(uint8_t *buf);
void lr2021_flrc_parse_packet_status(const uint8_t *a, lr2021_flrc_packet_status_t *s);

/* Bits on air (18.3): uncoded, the AGC preamble, 21 bits of preamble and the sync word; coded at the rate, the
 * header (16 bits, variable length), payload, CRC and a 6-bit tail with FEC (CR under 1). USP's count, followed
 * here: it codes the header (as 18.2.2 says) and adds the tail for 2/3 too (18.3 names 1/2 and 3/4). The time in
 * microseconds, rounded up; 0 for an unknown code. */
uint32_t lr2021_flrc_packet_bits(const lr2021_flrc_packet_t *p, uint8_t cr);
uint32_t lr2021_flrc_time_on_air_us(const lr2021_flrc_packet_t *p, uint8_t rate, uint8_t cr);

/* ── BPSK (DS 15), TX only ───────────────────────────────────────────────── */

/* SetBpskModulationParams (15.2.1): 7 bytes. bitrate a bit rate word; shape LR2021_FSK_SHAPE_*; diff for DBPSK, with
 * the encoder's start: the previous bit and the parity. */
size_t lr2021_bpsk_modulation(uint8_t *buf, uint32_t bitrate, uint8_t shape, bool diff, bool init_prev,
                              bool init_parity);
/* SetBpskPacketParams (15.2.2): 4 bytes. Raw mode: len in bits, the FIFO sent as it is. Sigfox mode: len in bytes
 * of the UL container, the chip adding the rest; a control message or not, the frame's rank (0 first or single,
 * 1 second, 2 third). */
enum { LR2021_BPSK_RAW = 0, LR2021_BPSK_SIGFOX = 1 };
size_t lr2021_bpsk_packet(uint8_t *buf, uint8_t len, uint8_t mode, bool sigfox_control, uint8_t sigfox_rank);

#ifdef __cplusplus
}
#endif

#endif
