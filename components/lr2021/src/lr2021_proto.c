// SPDX-License-Identifier: MIT AND BSD-3-Clause-Clear
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * LR2021 standard-protocol modems (Bluetooth LE, 802.15.4 O-QPSK, Z-Wave, Wireless M-Bus, Wi-SUN): frame
 * builders and answer parsers, no I/O.
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

/* Builders and parsers: see lr2021_proto.h. Field layouts from DS chapters 12, 13, 14, 19 and 20. */
#include "lr2021_proto.h"

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

static size_t put32(uint8_t *b, uint32_t v)
{
    put16(b, (uint16_t)(v >> 16));
    put16(b + 2, (uint16_t)v);
    return 4;
}

static uint16_t get16(const uint8_t *a) { return (uint16_t)(a[0] << 8 | a[1]); }

static size_t op_u8(uint8_t *b, uint16_t op, uint8_t v)
{
    put_op(b, op);
    b[2] = v;
    return 3;
}

/* ── Shared ── */

void lr2021_proto_parse_rx_stats(const uint8_t *a, lr2021_proto_rx_stats_t *s)
{
    s->packets = get16(a);
    s->crc_errors = get16(a + 2);
    s->len_errors = get16(a + 4);
}

float lr2021_proto_rssi_dbm(uint8_t hi8, uint8_t bit0) { return -(float)((hi8 << 1) | (bit0 & 1)) / 2.0f; }

/* LQI: 0.25 dB steps (DS 12.3.3, 13.3.4, 14.3.6, 19.3.3, 20.3.4) */
static float lqi_db(uint8_t v) { return (float)v * 0.25f; }

/* ── Bluetooth LE ── */

size_t lr2021_ble_modulation(uint8_t *b, uint8_t mode, uint8_t rx_bw)
{
    size_t n = op_u8(b, LR2021_SET_BLE_MODULATION, mode & 0x3);
    if (rx_bw != LR2021_PROTO_RX_BW_AUTO)
        b[n++] = rx_bw;
    return n;
}

size_t lr2021_ble_channel(uint8_t *b, const lr2021_ble_channel_t *c)
{
    put_op(b, LR2021_SET_BLE_CHANNEL);
    b[2] = (uint8_t)((c->crc_in_fifo ? 0x10 : 0) | (c->channel_type & 0xF)); /* rfu(2:0), crc_in_fifo, type(3:0) */
    b[3] = c->whitening_init;
    b[4] = (uint8_t)(c->crc_init >> 16);
    put16(b + 5, (uint16_t)c->crc_init);
    put32(b + 7, c->access_address);
    return 11;
}

size_t lr2021_ble_tx_pdu_len(uint8_t *b, uint8_t pdu_len) { return op_u8(b, LR2021_SET_BLE_TX_PDU_LEN, pdu_len); }
size_t lr2021_ble_tx(uint8_t *b, uint8_t pdu_len) { return op_u8(b, LR2021_SET_BLE_TX, pdu_len); }
size_t lr2021_ble_get_rx_stats(uint8_t *b) { return put_op(b, LR2021_GET_BLE_RX_STATS); }
size_t lr2021_ble_get_packet_status(uint8_t *b) { return put_op(b, LR2021_GET_BLE_PACKET_STATUS); }

/* table 14-8: len(2), rssi_avg(8:1), rssi_sync(8:1), flags (bit 2 rssi_avg(0), bit 0 rssi_sync(0)), lqi */
void lr2021_ble_parse_packet_status(const uint8_t *a, lr2021_ble_packet_status_t *s)
{
    s->len = get16(a);
    s->rssi_avg_dbm = lr2021_proto_rssi_dbm(a[2], a[4] >> 2);
    s->rssi_sync_dbm = lr2021_proto_rssi_dbm(a[3], a[4]);
    s->lqi_db = lqi_db(a[5]);
}

/* ── O-QPSK ── */

size_t lr2021_oqpsk_params(uint8_t *b, const lr2021_oqpsk_params_t *p)
{
    put_op(b, LR2021_SET_OQPSK_PARAMS);
    b[2] = p->mode;
    b[3] = p->rx_bw;
    b[4] = p->payload_len;
    put16(b + 5, p->preamble_bits);
    /* rfu(4:0), address_on, bypass_rx_length_check, fcs_mode */
    b[7] = (uint8_t)((p->address_filter ? 0x4 : 0) | (p->bypass_rx_length_check ? 0x2 : 0) | (p->fcs_in_fifo ? 1 : 0));
    return 8;
}

size_t lr2021_oqpsk_packet_len(uint8_t *b, uint8_t len) { return op_u8(b, LR2021_SET_OQPSK_PACKET_LEN, len); }

size_t lr2021_oqpsk_address(uint8_t *b, const uint8_t long_addr[8], const uint8_t short_addr[2], uint16_t pan_id,
                            uint8_t trans_id)
{
    put_op(b, LR2021_SET_OQPSK_ADDRESS);
    for (int i = 0; i < 8; i++)
        b[2 + i] = long_addr[i];
    b[10] = short_addr[0];
    b[11] = short_addr[1];
    put16(b + 12, pan_id);
    b[14] = trans_id;
    return 15;
}

size_t lr2021_oqpsk_get_rx_stats(uint8_t *b) { return put_op(b, LR2021_GET_OQPSK_RX_STATS); }
size_t lr2021_oqpsk_get_packet_status(uint8_t *b) { return put_op(b, LR2021_GET_OQPSK_PACKET_STATUS); }

/* table 19-5: RxHeader, len(2), rssi_avg(8:1), rssi_sync(8:1), flags (bit 2, bit 0), lqi */
void lr2021_oqpsk_parse_packet_status(const uint8_t *a, lr2021_oqpsk_packet_status_t *s)
{
    s->phr = a[0];
    s->len = get16(a + 1);
    s->rssi_avg_dbm = lr2021_proto_rssi_dbm(a[3], a[5] >> 2);
    s->rssi_sync_dbm = lr2021_proto_rssi_dbm(a[4], a[5]);
    s->lqi_db = lqi_db(a[6]);
}

/* ── Z-Wave ── */

size_t lr2021_zwave_params(uint8_t *b, const lr2021_zwave_params_t *p)
{
    put_op(b, LR2021_SET_ZWAVE_PARAMS);
    b[2] = p->rate & 0x3;
    b[3] = p->rx_bw;
    b[4] = p->addr_filter;
    b[5] = p->payload_len;
    put16(b + 6, p->preamble_bits);
    b[8] = p->preamble_detect_bits;
    b[9] = p->fcs_in_fifo ? 1 : 0; /* options: bit 0 fcs_mode */
    return 10;
}

size_t lr2021_zwave_home_id(uint8_t *b, uint32_t home_id)
{
    put_op(b, LR2021_SET_ZWAVE_HOME_ID);
    return 2 + put32(b + 2, home_id);
}

size_t lr2021_zwave_beam_filter(uint8_t *b, uint8_t tag, bool addr_12bit, uint16_t node_id, uint8_t id_hash)
{
    put_op(b, LR2021_SET_ZWAVE_BEAM_FILTER);
    b[2] = tag;
    b[3] = (uint8_t)((addr_12bit ? 0x80 : 0) | ((node_id >> 8) & 0xF)); /* addr_len, rfu(2:0), node_id(11:8) */
    b[4] = (uint8_t)node_id;
    b[5] = id_hash;
    return 6;
}

size_t lr2021_zwave_scan_config(uint8_t *b, const lr2021_zwave_scan_config_t *c)
{
    if (c->n < 2 || c->n > 4)
        return 0;
    put_op(b, LR2021_SET_ZWAVE_SCAN_CONFIG);
    uint8_t det = 0, rates = 0;
    for (int i = 0; i < 4; i++) {
        /* all four channels' bits go in, sent or not (as Semtech's driver does) */
        det |= (uint8_t)((c->ch[i].cca ? 1 : 0) << i);
        rates |= (uint8_t)((c->ch[i].rate & 0x3) << (2 * i));
    }
    b[2] = (uint8_t)(c->n << 4 | det); /* num_ch(3:0), det4..det1 */
    b[3] = rates;                      /* bitrate_ch4(1:0) .. bitrate_ch1(1:0) */
    b[4] = c->addr_filter;
    b[5] = c->fcs_in_fifo ? 1 : 0;
    size_t o = 6;
    for (int i = 0; i < c->n; i++) {
        o += put32(b + o, c->ch[i].freq_hz);
        b[o++] = c->ch[i].timeout;
    }
    return o;
}

size_t lr2021_zwave_scan(uint8_t *b, uint8_t mode)
{
    if (mode == LR2021_ZWAVE_SCAN_PLAIN)
        return put_op(b, LR2021_SET_ZWAVE_SCAN);
    return op_u8(b, LR2021_SET_ZWAVE_SCAN, mode);
}

size_t lr2021_zwave_get_rx_stats(uint8_t *b) { return put_op(b, LR2021_GET_ZWAVE_RX_STATS); }
size_t lr2021_zwave_get_packet_status(uint8_t *b) { return put_op(b, LR2021_GET_ZWAVE_PACKET_STATUS); }

/* table 20-7: len(2), rssi_avg(8:1), rssi_sync(8:1), LastDetected, flags (bit 2, bit 0; bits 7:3 the channel in
 * Semtech's driver), lqi */
void lr2021_zwave_parse_packet_status(const uint8_t *a, lr2021_zwave_packet_status_t *s)
{
    s->len = get16(a);
    s->rssi_avg_dbm = lr2021_proto_rssi_dbm(a[2], a[5] >> 2);
    s->rssi_sync_dbm = lr2021_proto_rssi_dbm(a[3], a[5]);
    s->last_detected = a[4];
    switch (a[4]) {
    case 1: s->rate = LR2021_ZWAVE_R1; break;
    case 2: s->rate = LR2021_ZWAVE_R2; break;
    case 3: s->rate = LR2021_ZWAVE_R3; break;
    case 4: s->rate = LR2021_ZWAVE_LR1; break;
    default: s->rate = -1; break;
    }
    s->channel = a[5] >> 3;
    s->lqi_db = lqi_db(a[6]);
}

/* ── Wireless M-Bus ── */

size_t lr2021_wmbus_params(uint8_t *b, const lr2021_wmbus_params_t *p)
{
    put_op(b, LR2021_SET_WMBUS_PARAMS);
    b[2] = p->mode & 0xF;
    b[3] = p->rx_bw;
    b[4] = p->format & 1;
    b[5] = p->address_filter ? 1 : 0;
    b[6] = p->payload_len;
    put16(b + 7, p->preamble_bits);
    b[9] = p->preamble_detect_bits;
    return 10;
}

size_t lr2021_wmbus_address(uint8_t *b, const uint8_t addr[6])
{
    put_op(b, LR2021_SET_WMBUS_ADDRESS);
    for (int i = 0; i < 6; i++)
        b[2 + i] = addr[i];
    return 8;
}

size_t lr2021_wmbus_get_rx_stats(uint8_t *b) { return put_op(b, LR2021_GET_WMBUS_RX_STATS); }
size_t lr2021_wmbus_get_packet_status(uint8_t *b) { return put_op(b, LR2021_GET_WMBUS_PACKET_STATUS); }

/* table 12-6: LField, len(2), rssi_avg(8:1), rssi_sync(8:1), CrcError(16:9), CrcError(8:1), then bit 7 SwIndex,
 * bit 6 CrcError(0), bit 4 rssi_avg(0), bit 0 rssi_sync(0); lqi */
void lr2021_wmbus_parse_packet_status(const uint8_t *a, lr2021_wmbus_packet_status_t *s)
{
    s->l_field = a[0];
    s->len = get16(a + 1);
    s->rssi_avg_dbm = lr2021_proto_rssi_dbm(a[3], a[7] >> 4);
    s->rssi_sync_dbm = lr2021_proto_rssi_dbm(a[4], a[7]);
    s->crc_errors = (uint32_t)a[5] << 9 | (uint32_t)a[6] << 1 | ((a[7] >> 6) & 1u);
    s->format = (a[7] >> 7) & 1;
    s->lqi_db = lqi_db(a[8]);
}

/* ── Wi-SUN ── */

size_t lr2021_wisun_mode(uint8_t *b, uint8_t mode, uint8_t rx_bw)
{
    put_op(b, LR2021_SET_WISUN_MODE);
    b[2] = mode & 0x7;
    b[3] = rx_bw;
    return 4;
}

size_t lr2021_wisun_packet(uint8_t *b, const lr2021_wisun_packet_t *p)
{
    put_op(b, LR2021_SET_WISUN_PACKET);
    /* rfu(1:0), fcs_tx, whitening, crc_on, mode_switch_tx, fec_tx(1:0) */
    b[2] = (uint8_t)((p->fcs_16bit ? 0x20 : 0) | (p->whitening ? 0x10 : 0) | (p->crc_by_chip ? 0x08 : 0) |
                     (p->mode_switch ? 0x04 : 0) | (p->fec & 0x3));
    put16(b + 3, p->frame_len);
    b[5] = p->preamble_len;
    b[6] = p->preamble_detect;
    return 7;
}

size_t lr2021_wisun_packet_len(uint8_t *b, uint16_t frame_len)
{
    put_op(b, LR2021_SET_WISUN_PACKET_LEN);
    return 2 + put16(b + 2, frame_len);
}

size_t lr2021_wisun_get_rx_stats(uint8_t *b) { return put_op(b, LR2021_GET_WISUN_RX_STATS); }
size_t lr2021_wisun_get_packet_status(uint8_t *b) { return put_op(b, LR2021_GET_WISUN_PACKET_STATUS); }

/* table 13-7: Header(2), len(2), rssi_avg(8:1), rssi_sync(8:1), then bit 7 SyncwordIdx, bit 2 rssi_avg(0), bit 0
 * rssi_sync(0); lqi */
void lr2021_wisun_parse_packet_status(const uint8_t *a, lr2021_wisun_packet_status_t *s)
{
    s->header = get16(a);
    s->len = get16(a + 2);
    s->rssi_avg_dbm = lr2021_proto_rssi_dbm(a[4], a[6] >> 2);
    s->rssi_sync_dbm = lr2021_proto_rssi_dbm(a[5], a[6]);
    s->syncword = (a[6] >> 7) & 1;
    s->lqi_db = lqi_db(a[7]);
}
