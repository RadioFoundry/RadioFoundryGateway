// SPDX-License-Identifier: MIT AND BSD-3-Clause-Clear
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * LR2021 LoRa, LR-FHSS and RTToF (ranging) commands: frame builders, answer parsers and time on air, as plain C with
 * no I/O (the driver in lr2021.h sends them). The datasheet (DS.LR20xx Rev. 2.2) section each comes from is noted.
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
#ifndef LR2021_LORA_H
#define LR2021_LORA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lr2021_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opcodes (tables 5-8, 5-12, 5-18). SetLoraModulationParams, SetLoraPacketParams and SetLoraSyncword are in
 * lr2021_cmd.h. */
enum {
    LR2021_SET_LORA_TX_SYNC = 0x021D,
    LR2021_SET_LORA_SIDE_DET_CAD = 0x021E,
    LR2021_SET_LORA_SYNCH_TIMEOUT = 0x0222,
    LR2021_SET_LORA_SIDE_DET_CONFIG = 0x0224,
    LR2021_SET_LORA_SIDE_DET_SYNCWORD = 0x0225,
    LR2021_SET_LORA_CAD_PARAMS = 0x0227,
    LR2021_SET_LORA_CAD = 0x0228,
    LR2021_GET_LORA_RX_STATS = 0x0229,
    LR2021_GET_LORA_PACKET_STATUS = 0x022A,
    LR2021_SET_LORA_ADDRESS = 0x022B,
    LR2021_SET_LORA_HOPPING = 0x022C,
    /* Not in the datasheet: the 10-bit sync word forms */
    LR2021_LR_FHSS_BUILD_FRAME = 0x0256,
    LR2021_LR_FHSS_SET_SYNCWORD = 0x0257,
    LR2021_READ_LR_FHSS_HOPPING_TABLE = 0x0258,
    LR2021_WRITE_LR_FHSS_HOPPING_TABLE = 0x0259,
    LR2021_SET_RANGING_ADDR = 0x0278,
    LR2021_SET_RANGING_REQ_ADDR = 0x0279,
    LR2021_GET_RANGING_RESULT = 0x027A,
    LR2021_SET_RANGING_TX_RX_DELAY = 0x027B,
    LR2021_SET_RANGING_PARAMS = 0x027C,
    LR2021_GET_RANGING_STATS = 0x027D,
};

/* SetPacketType values for these modems (table 8-1); LoRa's is LR2021_PACKET_LORA. */
#define LR2021_RTTOF_PACKET_TYPE 0x4
#define LR2021_LR_FHSS_PACKET_TYPE 0x7

/* ── LoRa ──────────────────────────────────────────────────────────────────── */

/* Bandwidth codes past lr2021_cmd.h's 31, 62 and 125 kHz (table 9-3). 0x0, 0x1, 0x8 and 0x9 are RFU. The fractional
 * ones (101 to 812) are the SX128x's; 41 and 83 are a third of 125 and 250. lr2021_lora_bw_hz has each one's Hz. */
enum {
    LR2021_LORA_BW_250 = 0x5,
    LR2021_LORA_BW_500 = 0x6,
    LR2021_LORA_BW_1000 = 0x7,
    LR2021_LORA_BW_41 = 0xA,  /* 41.667 kHz */
    LR2021_LORA_BW_83 = 0xB,  /* 83.333 kHz */
    LR2021_LORA_BW_101 = 0xC, /* 101.5625 kHz */
    LR2021_LORA_BW_203 = 0xD, /* 203.125 kHz */
    LR2021_LORA_BW_406 = 0xE, /* 406.25 kHz */
    LR2021_LORA_BW_812 = 0xF, /* 812.5 kHz */
};

/* Coding rates (table 9-4). LI: the long interleaver. CC: a convolutional code with the long interleaver, which
 * the SX127x, SX126x, SX128x and LR11xx can't receive. */
enum {
    LR2021_LORA_CR_4_5 = 0x1,
    LR2021_LORA_CR_4_6 = 0x2,
    LR2021_LORA_CR_4_7 = 0x3,
    LR2021_LORA_CR_4_8 = 0x4,
    LR2021_LORA_CR_LI_4_5 = 0x5,
    LR2021_LORA_CR_LI_4_6 = 0x6,
    LR2021_LORA_CR_LI_4_8 = 0x7,
    LR2021_LORA_CR_CC_4_6 = 0x8,
    LR2021_LORA_CR_CC_4_8 = 0x9,
};

/* Sync words (9.9.5): LoRaWAN public and private (the chip's default). */
#define LR2021_LORA_SYNCWORD_PUBLIC 0x34
#define LR2021_LORA_SYNCWORD_PRIVATE 0x12

/* SetLoraModulationParams' arguments (9.9.1). */
typedef struct {
    uint8_t sf;  /* 5 to 12 */
    uint8_t bw;  /* a LR2021_LORA_BW_* code */
    uint8_t cr;  /* a LR2021_LORA_CR_* code */
    bool ldro;   /* low data rate optimization: a quarter of the symbol values (lr2021_lora_ldro has the rule) */
} lr2021_lora_mod_t;

/* SetLoraPacketParams' arguments (9.9.3). */
typedef struct {
    uint16_t preamble;   /* symbols, 1 to 65535: at least 12 for SF5 and SF6, 8 for the others (9.2.1) */
    uint8_t payload_len; /* implicit: the length; explicit RX: the longest accepted (0 any) */
    bool implicit;       /* no header: both ends must agree on the length, CR and CRC */
    bool crc;            /* a 16-bit payload CRC */
    bool invert_iq;      /* the chip inverts the sense itself at 203, 406 and 812 kHz for SX128x compatibility */
} lr2021_lora_pkt_t;

/* SetLoraModulationParams (9.9.1): 4 bytes. */
size_t lr2021_lora_modulation(uint8_t *buf, const lr2021_lora_mod_t *m);
/* SetLoraPacketParams (9.9.3): 6 bytes. */
size_t lr2021_lora_packet(uint8_t *buf, const lr2021_lora_pkt_t *p);

/* SetLoRaSynchTimeout (9.9.4): 4 bytes. code is the symbol count (format 0) or a mantissa / exponent byte (format
 * 1, mantissa in bits 7:3); 0 turns the symbol timeout off. */
enum { LR2021_LORA_TIMEOUT_SYMBOLS = 0, LR2021_LORA_TIMEOUT_MANT_EXP = 1 };
size_t lr2021_lora_synch_timeout(uint8_t *buf, uint8_t code, uint8_t format);
/* The same for a count of symbols: as is up to 255, past that the smallest mantissa / exponent count at least n. */
size_t lr2021_lora_synch_timeout_symbols(uint8_t *buf, uint16_t n);
/* The mantissa / exponent byte for at least n symbols (mantissa 0 to 31, exponent 0 to 7) and the count it gives:
 * mantissa x 2^(2 x exponent + 1), as Semtech's driver computes it. DS 9.9.4 writes 2^(exponent + 1): check on the
 * chip before relying on counts past 62. */
uint32_t lr2021_lora_mant_exp(uint16_t n, uint8_t *code);

/* SetLoraSyncword (9.9.5): 3 bytes. */
size_t lr2021_lora_syncword(uint8_t *buf, uint8_t syncword);

/* A side detector (9.3, 9.9.6): another SF on the same bandwidth. In RX the main SF (the modulation params') must be
 * the smallest, in CAD the largest; all different, the largest at most 4 over the smallest; at most two side
 * detectors past 500 kHz and one with a main SF of 10 to 12. SetLoraModulationParams turns them all off. */
typedef struct {
    uint8_t sf;
    bool ldro;
    bool invert_iq;
} lr2021_lora_side_det_t;
/* SetLoraSideDetConfig (9.9.6): n side detectors (0 to 3; 0 turns them off), 2 + n bytes. */
size_t lr2021_lora_side_det_config(uint8_t *buf, const lr2021_lora_side_det_t *d, int n);
/* SetLoraSideDetSyncword (9.9.7): n sync words (0 to 3), 2 + n bytes. */
size_t lr2021_lora_side_det_syncword(uint8_t *buf, const uint8_t *syncwords, int n);
/* SetLoraSideDetCad (9.9.13): each side detector's pnr_delta (0 to 15) and det_peak for CAD, n of them (0 to 3),
 * 2 + 2n bytes. */
size_t lr2021_lora_side_det_cad(uint8_t *buf, const uint8_t *pnr_delta, const uint8_t *det_peak, int n);

/* CAD's exits (table 6-18): the fallback mode, RX if it found something, TX if it didn't (listen before talk). */
enum { LR2021_LORA_CAD_ONLY = 0x00, LR2021_LORA_CAD_RX = 0x01, LR2021_LORA_CAD_LBT = 0x10 };
/* SetLoraCadParams' arguments (6.3.11). */
typedef struct {
    uint8_t symbols;   /* symbols to search */
    bool preamble_only; /* pbl_any: detect only preamble (a payload is still likely to trigger it) */
    uint8_t pnr_delta; /* 0: all the symbols; 8: fast CAD, stops early when there's clearly nothing (9.6) */
    uint8_t exit_mode; /* LR2021_LORA_CAD_* */
    uint32_t timeout;  /* 24 bits: the RX or TX that follows (not for CAD_ONLY). Semtech's driver gives it in 31.25 us
                        * steps (SetRx's); DS 6.3.11 says 32 MHz periods: check on the chip */
    uint8_t det_peak;  /* the peak to average threshold, 3.4 fixed point (55, the default, is about 5.3x);
                        * lr2021_lora_cad_det_peak has the recommended ones */
} lr2021_lora_cad_t;
/* SetLoraCadParams (6.3.11): 9 bytes. */
size_t lr2021_lora_cad_params(uint8_t *buf, const lr2021_lora_cad_t *c);
/* SetLoraCAD (6.3.12): 2 bytes. CadDone, and CadDetected if it found something. */
size_t lr2021_lora_cad(uint8_t *buf);
/* The recommended det_peak for an SF and symbol count (table 6-19; past 4 symbols, 4's); 0 for a bad SF. */
uint8_t lr2021_lora_cad_det_peak(uint8_t sf, uint8_t symbols);

/* SetLoraAddress (9.9.10): receive filtering on len (0 to 8; 0 off) address bytes at byte pos (0 to 15) of the
 * payload, the bytes in the order they appear there. 3 + len bytes. */
size_t lr2021_lora_address(uint8_t *buf, uint8_t pos, const uint8_t *addr, uint8_t len);

/* SetLoraHopping (9.9.11): intra-packet hopping. After the header, every period symbols (0 to 8191) the next of n
 * frequencies (Hz, 0 to 40; 0 turns hopping off). 4 + 4n bytes. For SX1276 compatibility also set bit 18 of
 * 0x00F30A24 (9.9.11.1). */
#define LR2021_LORA_HOPS_MAX 40
#define LR2021_LORA_HOPPING_FRAME_MAX (4 + 4 * LR2021_LORA_HOPS_MAX)
size_t lr2021_lora_hopping(uint8_t *buf, uint16_t period, const uint32_t *hz, int n);

/* SetLoraTxSync (9.9.12): RTToF timing sync on a DIO. Manager: sends the sync frame when the DIO rises;
 * subordinate: listens for it and drives the DIO. 3 bytes. */
enum { LR2021_LORA_TX_SYNC_OFF = 0, LR2021_LORA_TX_SYNC_MANAGER = 1, LR2021_LORA_TX_SYNC_SUBORDINATE = 2 };
size_t lr2021_lora_tx_sync(uint8_t *buf, uint8_t function, uint8_t dio);

/* GetLoraRxStats (9.9.8): the request (2 bytes) and its answer. */
#define LR2021_LORA_RX_STATS_LEN 10
size_t lr2021_lora_get_rx_stats(uint8_t *buf);
typedef struct {
    uint16_t packets;      /* received */
    uint16_t crc_errors;
    uint16_t header_errors;
    uint16_t headers_ok;   /* the fourth counter: Semtech's driver reads it as valid headers. DS table 9-11 lists only
                            * four counters (8 bytes) and false_sync as the fourth: check on the chip */
    uint16_t false_syncs;  /* a preamble with no sync word after it */
} lr2021_lora_rx_stats_t;
void lr2021_lora_parse_rx_stats(const uint8_t *answer, lr2021_lora_rx_stats_t *s);

/* GetLoraPacketStatus (9.9.9): the request (2 bytes) and its answer. Valid after RxDone or CadDone. */
#define LR2021_LORA_PACKET_STATUS_LEN 9
size_t lr2021_lora_get_packet_status(uint8_t *buf);
/* detectors: which received the packet (in CAD, every one that triggered) */
enum { LR2021_LORA_DET_MAIN = 0x1, LR2021_LORA_DET_SIDE1 = 0x2, LR2021_LORA_DET_SIDE2 = 0x4, LR2021_LORA_DET_SIDE3 = 0x8 };
typedef struct {
    bool crc;              /* from the header (explicit) or the settings (implicit) */
    uint8_t cr;            /* a LR2021_LORA_CR_* code, the same way */
    uint8_t length;        /* payload bytes */
    float snr_db;          /* 0.25 dB steps */
    float rssi_dbm;        /* the average power over the packet, 0.5 dB steps */
    float signal_rssi_dbm; /* the LoRa signal's own power, after despreading */
    uint8_t detectors;     /* LR2021_LORA_DET_* */
    int32_t freq_offset_hz;
} lr2021_lora_packet_status_t;
void lr2021_lora_parse_packet_status(const uint8_t *answer, lr2021_lora_packet_status_t *s);

/* ── LoRa helpers ──────────────────────────────────────────────────────────── */

/* A bandwidth code's width in Hz (table 9-3; 41 and 83 kHz are 125 / 3 and 250 / 3); 0 for an RFU code. */
double lr2021_lora_bw_hz(uint8_t bw);
/* A coding rate's denominator over 4 (table 9-4: 4/5 is 5, CC 4/6 is 6); 0 for an RFU code. */
int lr2021_lora_cr_denominator(uint8_t cr);
/* The long interleaver (codes 5 to 9). */
bool lr2021_lora_cr_long(uint8_t cr);
/* The recommended LDRO: on for SF11 and SF12 except SF11 at 250 kHz and both at 500 and 1000 kHz. This is
 * Semtech's driver's rule, which keeps the SX128x bandwidths (203, 406, 812 kHz) on for SX128x compatibility;
 * DS 9.9.1's (SF11 up to 125 kHz, SF12 up to 250 kHz) differs only there. */
bool lr2021_lora_ldro(uint8_t sf, uint8_t bw);
/* A symbol's length, 2^SF / bandwidth, in us (0 for an RFU bandwidth). */
double lr2021_lora_symbol_us(uint8_t sf, uint8_t bw);
/* A packet's length in symbols: the preamble, 4.25 sync symbols (6.25 at SF5 and SF6), then the header and
 * payload. Semtech's formula, with the long interleaver's code rates from table 9-4. */
double lr2021_lora_toa_symbols(const lr2021_lora_mod_t *m, const lr2021_lora_pkt_t *p);
/* Time on air in us (0 for an RFU bandwidth). */
double lr2021_lora_toa_us(const lr2021_lora_mod_t *m, const lr2021_lora_pkt_t *p);
/* Time on air in whole ms, rounded up. */
uint32_t lr2021_lora_toa_ms(const lr2021_lora_mod_t *m, const lr2021_lora_pkt_t *p);

/* ── LR-FHSS (chapter 17; transmit only) ───────────────────────────────────── */

enum { LR2021_LR_FHSS_CR_5_6 = 0, LR2021_LR_FHSS_CR_2_3 = 1, LR2021_LR_FHSS_CR_1_2 = 2, LR2021_LR_FHSS_CR_1_3 = 3 };
/* The hopping grid: 25.390625 or 3.90625 kHz. */
enum { LR2021_LR_FHSS_GRID_25 = 0, LR2021_LR_FHSS_GRID_4 = 1 };
/* Hopping, or test modes that encode as for hopping or ramp the PA as for it without hopping. */
enum {
    LR2021_LR_FHSS_NO_HOPPING = 0,
    LR2021_LR_FHSS_HOPPING = 1,
    LR2021_LR_FHSS_TEST_PAYLOAD = 2,
    LR2021_LR_FHSS_TEST_PA = 3,
};
/* The span the hops cover: codes 0 to 9 (39.06, 85.94, 136.72, 183.59, 335.94, 386.72, 722.66, 773.44, 1523.4,
 * 1574.2 kHz); lr2021_lr_fhss_bw_hz has each one's Hz. */
enum {
    LR2021_LR_FHSS_BW_39 = 0,
    LR2021_LR_FHSS_BW_86 = 1,
    LR2021_LR_FHSS_BW_137 = 2,
    LR2021_LR_FHSS_BW_184 = 3,
    LR2021_LR_FHSS_BW_336 = 4,
    LR2021_LR_FHSS_BW_387 = 5,
    LR2021_LR_FHSS_BW_723 = 6,
    LR2021_LR_FHSS_BW_773 = 7,
    LR2021_LR_FHSS_BW_1523 = 8,
    LR2021_LR_FHSS_BW_1574 = 9,
};
/* The bit rate, 488.28125 b/s (GMSK, mod_type 0), and the reset sync word (17.2.2). */
#define LR2021_LR_FHSS_BITRATE 488.28125
#define LR2021_LR_FHSS_SYNCWORD_DEFAULT 0x2C0F7995u

/* LrFhssBuildFrame's arguments (17.2.1). */
typedef struct {
    uint8_t headers;   /* sync headers, 1 to 4 (2 or more resist interference better) */
    uint8_t cr;        /* LR2021_LR_FHSS_CR_* */
    uint8_t grid;      /* LR2021_LR_FHSS_GRID_* */
    uint8_t hopping;   /* LR2021_LR_FHSS_* hopping mode */
    uint8_t bw;        /* LR2021_LR_FHSS_BW_* */
    uint16_t sequence; /* the hopping sequence's seed, 9 bits (lr2021_lr_fhss_sequences counts them) */
    int8_t offset;     /* this device's offset in 488.28125 Hz steps: -26 to 25 on the 25 kHz grid, -4 to 3 on the 4 */
} lr2021_lr_fhss_t;

/* LrFhssBuildFrame (17.2.1): encodes n payload bytes and sets up the hopping table; SetTx sends it. 8 + n bytes. */
#define LR2021_LR_FHSS_FRAME_MAX (8 + 255)
size_t lr2021_lr_fhss_build_frame(uint8_t *buf, const lr2021_lr_fhss_t *p, const uint8_t *payload, uint8_t n);
/* LrFhssSetSyncword (17.2.2): 6 bytes. */
size_t lr2021_lr_fhss_syncword(uint8_t *buf, uint32_t syncword);

/* The hopping table (17.2.3): a 5-byte head, then up to 40 entries of a frequency (32 bits) and a symbol count
 * (16 bits). */
#define LR2021_LR_FHSS_HOPS_MAX 40
#define LR2021_LR_FHSS_HOP_TABLE_LEN (5 + 6 * LR2021_LR_FHSS_HOPS_MAX)
typedef struct {
    bool hopping;         /* hopping_en */
    bool freq_hz;         /* convert_freq: the frequencies are Hz, not PLL steps */
    uint16_t pkt_length;
    uint8_t used_freqs;   /* nb_used_freqs */
    uint8_t blocks;       /* nb_hopping_blocks */
    int n;                /* entries */
    struct {
        uint32_t freq;
        uint16_t symbols;
    } hop[LR2021_LR_FHSS_HOPS_MAX];
} lr2021_lr_fhss_table_t;
/* ReadLrFhssHoppingTable (17.2.3): the request (2 bytes). The answer is up to LR2021_LR_FHSS_HOP_TABLE_LEN bytes. */
size_t lr2021_lr_fhss_read_table(uint8_t *buf);
/* The answer's len bytes into t: with hopping, min(used_freqs, blocks) entries (at most 40), else only freq0; never
 * more than len holds. Returns t->n. */
int lr2021_lr_fhss_parse_table(const uint8_t *answer, size_t len, lr2021_lr_fhss_table_t *t);
/* WriteLrFhssHoppingTable (17.2.4): the head and t->n entries (at most 40), 7 + 6n bytes. */
size_t lr2021_lr_fhss_write_table(uint8_t *buf, const lr2021_lr_fhss_table_t *t);

/* A span code's width in Hz (0 for an RFU code). */
double lr2021_lr_fhss_bw_hz(uint8_t bw);
/* The bits on air for n payload bytes: 114 a header, then the coded payload in 48-bit fragments of 50 bits. */
uint32_t lr2021_lr_fhss_bits(const lr2021_lr_fhss_t *p, uint16_t n);
/* Time on air for n payload bytes, in us and in whole ms rounded up. */
double lr2021_lr_fhss_toa_us(const lr2021_lr_fhss_t *p, uint16_t n);
uint32_t lr2021_lr_fhss_toa_ms(const lr2021_lr_fhss_t *p, uint16_t n);
/* How many hopping sequences (seeds) there are: 384 on the 25 kHz grid and on the 4 kHz grid under 335.94 kHz,
 * 512 otherwise. */
unsigned lr2021_lr_fhss_sequences(const lr2021_lr_fhss_t *p);

/* ── RTToF (chapter 10): LoRa ranging ──────────────────────────────────────── */

/* SetRangingAddr (10.2.1): the subordinate's address; it answers requests whose low check_len bytes (1 to 4)
 * match. Default 0x00000019, all 4 checked. 7 bytes. */
size_t lr2021_rttof_addr(uint8_t *buf, uint32_t addr, uint8_t check_len);
/* SetRangingReqAddr (10.2.2): the address the manager asks for. 6 bytes. */
size_t lr2021_rttof_req_addr(uint8_t *buf, uint32_t addr);
/* SetRangingTxRxDelay (10.2.5): the calibration delay, in 1 / (1024 x bandwidth in MHz) us. 6 bytes. */
size_t lr2021_rttof_tx_rx_delay(uint8_t *buf, uint32_t delay);
/* SetRangingParams (10.2.6): extended (a second exchange the other way, which cancels Doppler), spy (measure other
 * pairs' exchanges), symbols in the response (0 to 63; 8 to 16, 12 about best). 3 bytes. */
size_t lr2021_rttof_params(uint8_t *buf, bool extended, bool spy, uint8_t symbols);

/* GetRangingResult (10.2.3, manager only): the request (3 bytes) and each type's answer length. */
enum { LR2021_RTTOF_RESULT = 0, LR2021_RTTOF_RESULT_EXT = 1, LR2021_RTTOF_GAIN_STEPS = 2 };
#define LR2021_RTTOF_RESULT_LEN 4
/* Rng1, Rssi1, Rng2 in DS table 10-4 (7 bytes); Semtech's driver reads an eighth, Rssi2. */
#define LR2021_RTTOF_RESULT_EXT_LEN 8
/* The AGC's gain step for each exchange (bytes 0 and 1). */
#define LR2021_RTTOF_GAIN_STEPS_LEN 2
size_t lr2021_rttof_get_result(uint8_t *buf, uint8_t type);
typedef struct {
    int32_t rng1, rng2; /* raw round-trip times, signed 24 bits (rng2: extended only; 0 otherwise) */
    float rssi1_dbm, rssi2_dbm; /* -raw / 2, as Semtech's driver reads it */
} lr2021_rttof_result_t;
/* An answer of LR2021_RTTOF_RESULT_LEN (extended false) or LR2021_RTTOF_RESULT_EXT_LEN bytes. */
void lr2021_rttof_parse_result(const uint8_t *answer, bool extended, lr2021_rttof_result_t *r);
/* A raw result in meters: rng x 150 / (2^12 x bandwidth in MHz) (10.2.3). For an extended exchange average the
 * two; their difference is the Doppler. 0 for an RFU bandwidth. */
double lr2021_rttof_distance_m(int32_t rng, uint8_t bw);

/* GetRangingStats (10.2.4): the request (2 bytes) and its answer. Extended exchanges count twice. */
#define LR2021_RTTOF_STATS_LEN 10
size_t lr2021_rttof_get_stats(uint8_t *buf);
typedef struct {
    uint16_t exchanges_ok;  /* manager: valid responses */
    uint16_t requests_ok;   /* subordinate: valid requests */
    uint16_t responses;     /* subordinate: responses sent */
    uint16_t timeouts;      /* manager: no response; subordinate: no extended request */
    uint16_t discarded;     /* subordinate: requests for another address */
} lr2021_rttof_stats_t;
void lr2021_rttof_parse_stats(const uint8_t *answer, lr2021_rttof_stats_t *s);

#ifdef __cplusplus
}
#endif

#endif
