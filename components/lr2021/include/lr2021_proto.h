// SPDX-License-Identifier: MIT AND BSD-3-Clause-Clear
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * LR2021 standard-protocol modems (Bluetooth LE, 802.15.4 O-QPSK, Z-Wave, Wireless M-Bus, Wi-SUN): frame builders
 * and answer parsers, as plain C with no I/O.
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
 * Checked against the datasheet (DS.LR20xx Rev. 2.2): WM-Bus chapter 12, Wi-SUN 13, Bluetooth LE 14, O-QPSK 19,
 * Z-Wave 20. The LR2022 has no O-QPSK or Z-Wave; the LR2012 has neither those nor Bluetooth LE (DS chapter 1).
 *
 * Builders write the whole frame (opcode first, fields big-endian) into buf and return its length; buf must hold
 * LR2021_PROTO_FRAME_MAX bytes. The parsers take a read's answer: the bytes after the two status bytes, as
 * lr2021_read returns them, LR2021_*_LEN of them. Select the modem with SetPacketType first.
 */
#ifndef LR2021_PROTO_H
#define LR2021_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lr2021_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opcodes (DS tables 12-2 to 20-10) */
enum {
    LR2021_SET_BLE_MODULATION = 0x0260,
    LR2021_SET_BLE_CHANNEL = 0x0261,
    LR2021_SET_BLE_TX = 0x0262,
    LR2021_GET_BLE_RX_STATS = 0x0264,
    LR2021_GET_BLE_PACKET_STATUS = 0x0265,
    LR2021_SET_BLE_TX_PDU_LEN = 0x0266,
    LR2021_SET_WMBUS_PARAMS = 0x026A,
    LR2021_GET_WMBUS_RX_STATS = 0x026C,
    LR2021_GET_WMBUS_PACKET_STATUS = 0x026D,
    LR2021_SET_WMBUS_ADDRESS = 0x026E,
    LR2021_SET_WISUN_MODE = 0x0270,
    LR2021_SET_WISUN_PACKET = 0x0271,
    LR2021_GET_WISUN_RX_STATS = 0x0272,
    LR2021_GET_WISUN_PACKET_STATUS = 0x0273,
    LR2021_SET_WISUN_PACKET_LEN = 0x0274,
    LR2021_SET_ZWAVE_PARAMS = 0x0297,
    LR2021_SET_ZWAVE_HOME_ID = 0x0298,
    LR2021_GET_ZWAVE_RX_STATS = 0x0299,
    LR2021_GET_ZWAVE_PACKET_STATUS = 0x029A,
    LR2021_SET_ZWAVE_BEAM_FILTER = 0x029B,
    LR2021_SET_ZWAVE_SCAN_CONFIG = 0x029C,
    LR2021_SET_ZWAVE_SCAN = 0x029D,
    LR2021_SET_OQPSK_PARAMS = 0x029F,
    LR2021_GET_OQPSK_RX_STATS = 0x02A0,
    LR2021_GET_OQPSK_PACKET_STATUS = 0x02A1,
    LR2021_SET_OQPSK_PACKET_LEN = 0x02A2,
    LR2021_SET_OQPSK_ADDRESS = 0x02A3,
};

/* The longest frame here: SetZwaveScanConfig with four channels. */
#define LR2021_PROTO_FRAME_MAX 26

/* rx_bw: an FSK rx_bw code (DS table 11-4), or this for the chip to pick one for the mode. */
#define LR2021_PROTO_RX_BW_AUTO 0xFF
/* A preamble detection length of this: the chip picks it (WM-Bus, Z-Wave; Wi-SUN: 24 bits). */
#define LR2021_PROTO_PBL_DETECT_AUTO 0xFF

/* ── Shared ──────────────────────────────────────────────────────────────── */

/* Every Get*RxStats answer (DS tables 12-4, 13-5, 14-6, 19-3, 20-5): three counts, reset on POR, sleep without
 * retention and ResetRxStats. */
#define LR2021_PROTO_RX_STATS_LEN 6
typedef struct {
    uint16_t packets;    /* received */
    uint16_t crc_errors; /* received with a CRC error */
    uint16_t len_errors; /* with a length error */
} lr2021_proto_rx_stats_t;
void lr2021_proto_parse_rx_stats(const uint8_t *a, lr2021_proto_rx_stats_t *s);

/* An RSSI in a packet status: 9 bits, the top 8 in one byte and bit 0 elsewhere; the power is -rssi / 2 dBm. */
float lr2021_proto_rssi_dbm(uint8_t hi8, uint8_t bit0);

/* ── Bluetooth LE (DS 14) ────────────────────────────────────────────────── */

/* SetBleModulationParams' mode (14.3.1). The coded PHYs: "contact Semtech". */
enum { LR2021_BLE_1M = 0, LR2021_BLE_2M = 1, LR2021_BLE_CODED_S2 = 2, LR2021_BLE_CODED_S8 = 3 };
/* SetBleChannelParams' channel_type (14.3.2) */
enum { LR2021_BLE_ADVERTISING = 0, LR2021_BLE_DATA_16BIT_HEADER = 1, LR2021_BLE_DATA_24BIT_HEADER = 2 };

/* SetBleModulationParams (14.3.1). rx_bw is optional: LR2021_PROTO_RX_BW_AUTO leaves it off (3 bytes, the chip
 * picks it); anything else is sent (4 bytes). */
size_t lr2021_ble_modulation(uint8_t *buf, uint8_t mode, uint8_t rx_bw);

/* SetBleChannelParams (14.3.2): 11 bytes. */
typedef struct {
    uint8_t channel_type;    /* LR2021_BLE_ADVERTISING, _DATA_* */
    bool crc_in_fifo;        /* RX: append the 3 CRC bytes to the FIFO */
    uint8_t whitening_init;  /* the channel's whitening seed (0x53 for channel 37); 0 turns whitening off */
    uint32_t crc_init;       /* 24 bits: 0x555555 for advertising */
    uint32_t access_address; /* the sync word: 0x8E89BED6 for advertising */
} lr2021_ble_channel_t;
size_t lr2021_ble_channel(uint8_t *buf, const lr2021_ble_channel_t *c);

/* SetBleTxPduLen (14.3.3): the PDU's length with its header, without the CRC. */
size_t lr2021_ble_tx_pdu_len(uint8_t *buf, uint8_t pdu_len);
/* SetBleTx (14.3.4): SetBleTxPduLen and SetTx(0) in one. */
size_t lr2021_ble_tx(uint8_t *buf, uint8_t pdu_len);

/* GetBleRxStats (14.3.5): parse with lr2021_proto_parse_rx_stats. */
size_t lr2021_ble_get_rx_stats(uint8_t *buf);

/* GetBlePacketStatus (14.3.6, table 14-8). Updated on RxDone; rssi_sync already on SyncwordValid. */
#define LR2021_BLE_PACKET_STATUS_LEN 6
typedef struct {
    uint16_t len;        /* the last packet's bytes, with whatever went in the FIFO with it */
    float rssi_avg_dbm;  /* over the packet */
    float rssi_sync_dbm; /* at the sync word */
    float lqi_db;        /* link quality: the margin over the detection level */
} lr2021_ble_packet_status_t;
size_t lr2021_ble_get_packet_status(uint8_t *buf);
void lr2021_ble_parse_packet_status(const uint8_t *a, lr2021_ble_packet_status_t *s);

/* ── IEEE 802.15.4 O-QPSK (DS 19, LR2021 only) ───────────────────────────── */

/* SetOqpskParams' mode (19.3.1): only 250 kb/s, 2 Mchip/s; the rest are RFU. */
enum { LR2021_OQPSK_250K = 0 };

/* SetOqpskParams (19.3.1): 8 bytes. */
typedef struct {
    uint8_t mode;                /* LR2021_OQPSK_250K */
    uint8_t rx_bw;               /* LR2021_PROTO_RX_BW_AUTO, or an FSK rx_bw code */
    uint8_t payload_len;         /* TX: the bytes to send; RX: the longest MPDU accepted; at most 127 (else PERR) */
    uint16_t preamble_bits;      /* TX preamble, typically 32 */
    bool address_filter;         /* RX: filter on SetOqpskAddress's addresses */
    bool bypass_rx_length_check; /* needs the firmware patch (DS 22.3) */
    bool fcs_in_fifo;            /* the host makes the 16-bit FCS in TX and gets it, unchecked, in RX */
} lr2021_oqpsk_params_t;
size_t lr2021_oqpsk_params(uint8_t *buf, const lr2021_oqpsk_params_t *p);

/* SetOqpskPacketLen (19.3.4): payload_len alone, faster than SetOqpskParams. At most 127. */
size_t lr2021_oqpsk_packet_len(uint8_t *buf, uint8_t len);

/* SetOqpskAddress (19.3.5): the addresses RX filters on, 15 bytes. The 64-bit long address and 16-bit short address
 * as byte arrays in frame order (the DS's bits 63:56 and 15:8 first), the PAN ID, the transaction ID. A packet
 * matches when its destination (long or short, or broadcast) and PAN ID do; no multicast. */
size_t lr2021_oqpsk_address(uint8_t *buf, const uint8_t long_addr[8], const uint8_t short_addr[2], uint16_t pan_id,
                            uint8_t trans_id);

/* GetOqpskRxStats (19.3.2): parse with lr2021_proto_parse_rx_stats. */
size_t lr2021_oqpsk_get_rx_stats(uint8_t *buf);

/* GetOqpskPacketStatus (19.3.3, table 19-5) */
#define LR2021_OQPSK_PACKET_STATUS_LEN 7
typedef struct {
    uint8_t phr;         /* the PHY header: the frame length, FCS included */
    uint16_t len;        /* the last packet's bytes, with whatever went in the FIFO with it */
    float rssi_avg_dbm;
    float rssi_sync_dbm;
    float lqi_db;
} lr2021_oqpsk_packet_status_t;
size_t lr2021_oqpsk_get_packet_status(uint8_t *buf);
void lr2021_oqpsk_parse_packet_status(const uint8_t *a, lr2021_oqpsk_packet_status_t *s);

/* ── Z-Wave (DS 20, LR2021 only) ─────────────────────────────────────────── */

/* Rates (20.3.1): R1 9.6 kb/s Manchester, R2 40 kb/s, R3 100 kb/s GFSK, LR1 O-QPSK. In LR1 the host checks the
 * CRC itself after RxDone (DS 20.1). */
enum { LR2021_ZWAVE_LR1 = 0, LR2021_ZWAVE_R1 = 1, LR2021_ZWAVE_R2 = 2, LR2021_ZWAVE_R3 = 3 };
/* addr_comp (20.3.1) */
enum { LR2021_ZWAVE_FILTER_OFF = 0, LR2021_ZWAVE_FILTER_HOME_ID = 1, LR2021_ZWAVE_FILTER_HOME_ID_BEAM = 2 };

/* SetZwaveParams (20.3.1): 10 bytes. */
typedef struct {
    uint8_t rate;                 /* LR2021_ZWAVE_* */
    uint8_t rx_bw;                /* LR2021_PROTO_RX_BW_AUTO, or an FSK rx_bw code */
    uint8_t addr_filter;          /* LR2021_ZWAVE_FILTER_* */
    uint8_t payload_len;          /* TX: the bytes to send; RX: the longest MPDU accepted */
    uint16_t preamble_bits;       /* TX preamble; 0: the mode's shortest */
    uint8_t preamble_detect_bits; /* RX: 32 recommended, or LR2021_PROTO_PBL_DETECT_AUTO */
    bool fcs_in_fifo;             /* the host makes the FCS in TX and gets it, unchecked, in RX */
} lr2021_zwave_params_t;
size_t lr2021_zwave_params(uint8_t *buf, const lr2021_zwave_params_t *p);

/* SetZwaveHomeIdFiltering (20.3.2): the HomeID RX accepts, 6 bytes. */
size_t lr2021_zwave_home_id(uint8_t *buf, uint32_t home_id);

/* SetZwaveBeamFiltering (20.3.5): the beam frames RX accepts, 6 bytes. tag typically 0x55; node_id 8 bits, or 12
 * with addr_12bit (LR1); 0xFF always passes. id_hash: the HomeID hash (0x55, 0x0A and 0x4A always pass). */
size_t lr2021_zwave_beam_filter(uint8_t *buf, uint8_t tag, bool addr_12bit, uint16_t node_id, uint8_t id_hash);

/* SetZwaveScanConfig (20.3.6): 2 to 4 channels to scan, 6 + 5 per channel bytes; 0 for a bad channel count.
 * Timeouts the DS recommends, in 30 us steps: two channels LR1 11 each; three R1 19, R2 34, R3 8; four R1 19,
 * R2 30, R3 7, LR1 12. */
typedef struct {
    uint32_t freq_hz; /* as SetRfFrequency takes it */
    uint8_t rate;     /* LR2021_ZWAVE_* */
    uint8_t timeout;  /* 30 us steps */
    bool cca;         /* move on at once when there's no CCA detection (off is recommended) */
} lr2021_zwave_scan_channel_t;
typedef struct {
    uint8_t n; /* 2 to 4 */
    lr2021_zwave_scan_channel_t ch[4];
    uint8_t addr_filter; /* LR2021_ZWAVE_FILTER_* */
    bool fcs_in_fifo;
} lr2021_zwave_scan_config_t;
size_t lr2021_zwave_scan_config(uint8_t *buf, const lr2021_zwave_scan_config_t *c);

/* SetZwaveScan (20.3.7) with its optional mode byte. LR2021_ZWAVE_SCAN_PLAIN leaves the byte off (as the
 * chip's default, compute and scan). */
enum {
    LR2021_ZWAVE_SCAN_COMPUTE_AND_START = 0,
    LR2021_ZWAVE_SCAN_COMPUTE = 1, /* compute the scan's configuration only */
    LR2021_ZWAVE_SCAN_START = 2,   /* start with the configuration computed before (redo it after a front-end change) */
    LR2021_ZWAVE_SCAN_PLAIN = 0xFF,
};
size_t lr2021_zwave_scan(uint8_t *buf, uint8_t mode);

/* GetZwaveRxStats (20.3.3): parse with lr2021_proto_parse_rx_stats. */
size_t lr2021_zwave_get_rx_stats(uint8_t *buf);

/* GetZwavePacketStatus (20.3.4, table 20-7) */
#define LR2021_ZWAVE_PACKET_STATUS_LEN 7
typedef struct {
    uint16_t len;
    float rssi_avg_dbm;
    float rssi_sync_dbm;
    uint8_t last_detected; /* as sent: DS 1 R1, 2 R2, 3 R3, 4 LR1 (firmware patch, DS 22.3) */
    int rate;              /* that as LR2021_ZWAVE_*; -1 for none or RFU */
    uint8_t channel;       /* the scan channel it came on (0 to 3): RFU bits in the DS, patch only */
    float lqi_db;
} lr2021_zwave_packet_status_t;
size_t lr2021_zwave_get_packet_status(uint8_t *buf);
void lr2021_zwave_parse_packet_status(const uint8_t *a, lr2021_zwave_packet_status_t *s);

/* ── Wireless M-Bus (DS 12, EN 13757-4:2019) ─────────────────────────────── */

/* Modes (12.3.1). T2 and C2 come as the meter receiving (other to meter) and the meter sending. */
enum {
    LR2021_WMBUS_S = 0x0,
    LR2021_WMBUS_T1 = 0x1,
    LR2021_WMBUS_T2_METER_RX = 0x2,
    LR2021_WMBUS_T2_METER_TX = 0x3,
    LR2021_WMBUS_R2 = 0x4,
    LR2021_WMBUS_C1 = 0x5,
    LR2021_WMBUS_C2_METER_RX = 0x6,
    LR2021_WMBUS_C2_METER_TX = 0x7,
    LR2021_WMBUS_N_4K8 = 0x8,
    LR2021_WMBUS_N_2K4 = 0x9,
    LR2021_WMBUS_N_6K4 = 0xA,  /* Reserved in the DS; Semtech's driver names it N 6.4 kb/s */
    LR2021_WMBUS_N_19K2 = 0xB, /* 4-GFSK, TX; needs the firmware patch (DS 22.3) */
    LR2021_WMBUS_F2 = 0xC,
};
enum { LR2021_WMBUS_FORMAT_A = 0, LR2021_WMBUS_FORMAT_B = 1 };

/* SetWmbusParams (12.3.1): 10 bytes. */
typedef struct {
    uint8_t mode;                 /* LR2021_WMBUS_* */
    uint8_t rx_bw;                /* LR2021_PROTO_RX_BW_AUTO (recommended), or an FSK rx_bw code */
    uint8_t format;               /* TX: LR2021_WMBUS_FORMAT_A or _B; RX takes either, by the sync word */
    bool address_filter;          /* RX: filter on SetWmbusFilteringAddress's A-field */
    uint8_t payload_len;          /* TX: C, M, A and CI fields and data, no CRCs; RX: the largest L-field accepted */
    uint16_t preamble_bits;       /* TX preamble (4-FSK sends half as many symbols) */
    uint8_t preamble_detect_bits; /* RX: 32 (24 for C1, C2), or LR2021_PROTO_PBL_DETECT_AUTO */
} lr2021_wmbus_params_t;
size_t lr2021_wmbus_params(uint8_t *buf, const lr2021_wmbus_params_t *p);

/* SetWmbusFilteringAddress (12.3.4): the 6-byte A-field RX filters on, in frame order (bits 47:40 first). 8 bytes. */
size_t lr2021_wmbus_address(uint8_t *buf, const uint8_t addr[6]);

/* GetWmbusRxStats (12.3.2): parse with lr2021_proto_parse_rx_stats. */
size_t lr2021_wmbus_get_rx_stats(uint8_t *buf);

/* GetWmbusPacketStatus (12.3.3, table 12-6). Updated on RxDone; the format and rssi_sync on SyncwordValid. */
#define LR2021_WMBUS_PACKET_STATUS_LEN 9
typedef struct {
    uint8_t l_field;     /* as demodulated (format A: can differ from len) */
    uint16_t len;
    float rssi_avg_dbm;
    float rssi_sync_dbm;
    uint32_t crc_errors; /* 17 bits, one a CRC block that failed: format A bit 0 the header, then each block */
    uint8_t format;      /* LR2021_WMBUS_FORMAT_A or _B, as received */
    float lqi_db;
} lr2021_wmbus_packet_status_t;
size_t lr2021_wmbus_get_packet_status(uint8_t *buf);
void lr2021_wmbus_parse_packet_status(const uint8_t *a, lr2021_wmbus_packet_status_t *s);

/* ── Wi-SUN FSK (DS 13) ──────────────────────────────────────────────────── */

/* Operating modes (13.3.1, table 13-1): 1a 50 ksym/s h 0.5, 1b 50 h 1, 2a 100 h 0.5, 2b 100 h 1, 3 150 h 0.5,
 * 4a 200 h 0.5, 4b 200 h 1, 5 300 h 0.5. */
enum {
    LR2021_WISUN_1A = 0,
    LR2021_WISUN_1B = 1,
    LR2021_WISUN_2A = 2,
    LR2021_WISUN_2B = 3,
    LR2021_WISUN_3 = 4,
    LR2021_WISUN_4A = 5,
    LR2021_WISUN_4B = 6,
    LR2021_WISUN_5 = 7,
};
/* fec_tx (13.3.2); the RSC ones are TX only. */
enum {
    LR2021_WISUN_FEC_NONE = 0,
    LR2021_WISUN_FEC_NRNSC = 1,      /* with interleaver */
    LR2021_WISUN_FEC_RSC = 2,        /* without interleaver */
    LR2021_WISUN_FEC_RSC_INTERL = 3, /* with interleaver */
};

/* SetWisunMode (13.3.1): mode and rx_bw (LR2021_PROTO_RX_BW_AUTO: the chip picks it), 4 bytes. */
size_t lr2021_wisun_mode(uint8_t *buf, uint8_t mode, uint8_t rx_bw);

/* SetWisunPacketParams (13.3.2): 7 bytes. */
typedef struct {
    bool fcs_16bit;         /* TX: a 2-byte CRC (else 4) */
    bool whitening;         /* off is a test mode */
    bool crc_by_chip;       /* TX: the chip computes the CRC (else it's in the payload) */
    bool mode_switch;       /* TX: send a mode switch PHR (frame_len is then the header) */
    uint8_t fec;            /* LR2021_WISUN_FEC_* */
    uint16_t frame_len;     /* TX: 0 to 2047 bytes, or the mode switch header */
    uint8_t preamble_len;   /* TX preamble: bits in DS 13.3.2, bytes in Semtech's driver (check on the chip) */
    uint8_t preamble_detect; /* RX: 0 detect on the sync word, 1 to 32 bits, LR2021_PROTO_PBL_DETECT_AUTO 24 bits */
} lr2021_wisun_packet_t;
size_t lr2021_wisun_packet(uint8_t *buf, const lr2021_wisun_packet_t *p);

/* SetWisunPacketLen (13.3.5): frame_len alone (the length, or the mode switch header), 4 bytes. */
size_t lr2021_wisun_packet_len(uint8_t *buf, uint16_t frame_len);

/* GetWisunRxStats (13.3.3): parse with lr2021_proto_parse_rx_stats. */
size_t lr2021_wisun_get_rx_stats(uint8_t *buf);

/* GetWisunPacketStatus (13.3.4, table 13-7). Updated on RxDone; syncword and rssi_sync on SyncwordValid. */
#define LR2021_WISUN_PACKET_STATUS_LEN 8
typedef struct {
    uint16_t header; /* the raw 16-bit PHR */
    uint16_t len;
    float rssi_avg_dbm;
    float rssi_sync_dbm;
    uint8_t syncword; /* which of the two sync words matched: 0 or 1 */
    float lqi_db;
} lr2021_wisun_packet_status_t;
size_t lr2021_wisun_get_packet_status(uint8_t *buf);
void lr2021_wisun_parse_packet_status(const uint8_t *a, lr2021_wisun_packet_status_t *s);

#ifdef __cplusplus
}
#endif

#endif
