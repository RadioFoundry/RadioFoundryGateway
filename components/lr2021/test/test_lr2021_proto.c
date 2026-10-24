// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
// Host tests for the standard-protocol modems' frames and answers (lr2021_proto.h): exact bytes for every builder
// against the DS tables (chapters 12, 13, 14, 19, 20), and the parsers on known answers.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "lr2021_proto.h"

static int checks, fails;

#define CHECK(cond, ...)                                \
    do {                                                \
        checks++;                                       \
        if (!(cond)) {                                  \
            fails++;                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

static void frame(const uint8_t *got, size_t n, const uint8_t *want, size_t wn, const char *what)
{
    bool same = n == wn && !memcmp(got, want, n);
    char g[96] = "";
    for (size_t i = 0, o = 0; i < n && o + 4 < sizeof g; i++)
        o += (size_t)snprintf(g + o, sizeof g - o, "%02X ", got[i]);
    CHECK(same, "%s: got %s", what, g);
}
#define WANT(what, ...)                                \
    do {                                               \
        static const uint8_t w_[] = {__VA_ARGS__};     \
        frame(b, n, w_, sizeof w_, what);              \
    } while (0)

static bool near(float a, float b) { return fabsf(a - b) < 1e-4f; }

static uint8_t b[LR2021_PROTO_FRAME_MAX + 8];
static size_t n;

static void shared(void)
{
    // -rssi / 2 dBm over 9 bits: 0x50 and 1 is 161, -80.5 dBm; bit0 takes only bit 0 of what it's given
    CHECK(near(lr2021_proto_rssi_dbm(0x50, 1), -80.5f), "rssi -80.5");
    CHECK(near(lr2021_proto_rssi_dbm(0x50, 0xFE), -80.0f), "rssi bit 0 only");
    CHECK(near(lr2021_proto_rssi_dbm(0xFF, 1), -255.5f) && near(lr2021_proto_rssi_dbm(0, 0), 0), "rssi ends");

    const uint8_t a[] = {0x01, 0x02, 0x00, 0x03, 0xFF, 0xFE};
    lr2021_proto_rx_stats_t s;
    lr2021_proto_parse_rx_stats(a, &s);
    CHECK(s.packets == 258 && s.crc_errors == 3 && s.len_errors == 65534, "rx stats %u %u %u", s.packets,
          s.crc_errors, s.len_errors);
}

static void ble(void)
{
    // modulation: rx_bw left off when auto (the chip picks it), sent otherwise
    n = lr2021_ble_modulation(b, LR2021_BLE_2M, LR2021_PROTO_RX_BW_AUTO);
    WANT("ble modulation", 0x02, 0x60, 0x01);
    n = lr2021_ble_modulation(b, LR2021_BLE_CODED_S8, 0x0A);
    WANT("ble modulation rx_bw", 0x02, 0x60, 0x03, 0x0A);

    // advertising on channel 37: whitening 0x53, CRC seed 0x555555, access address 0x8E89BED6
    lr2021_ble_channel_t c = {LR2021_BLE_ADVERTISING, false, 0x53, 0x555555, 0x8E89BED6};
    n = lr2021_ble_channel(b, &c);
    WANT("ble adv", 0x02, 0x61, 0x00, 0x53, 0x55, 0x55, 0x55, 0x8E, 0x89, 0xBE, 0xD6);
    // CRC in the FIFO is bit 4; a 24-bit CRC seed drops its top byte
    c = (lr2021_ble_channel_t){LR2021_BLE_DATA_24BIT_HEADER, true, 0, 0xAB123456, 0x12345678};
    n = lr2021_ble_channel(b, &c);
    WANT("ble data", 0x02, 0x61, 0x12, 0x00, 0x12, 0x34, 0x56, 0x12, 0x34, 0x56, 0x78);

    n = lr2021_ble_tx_pdu_len(b, 37);
    WANT("ble pdu len", 0x02, 0x66, 37);
    n = lr2021_ble_tx(b, 255);
    WANT("ble tx", 0x02, 0x62, 0xFF);
    n = lr2021_ble_get_rx_stats(b);
    WANT("ble rx stats", 0x02, 0x64);
    n = lr2021_ble_get_packet_status(b);
    WANT("ble packet status", 0x02, 0x65);

    // 39 bytes, avg 0x60:1 (-96.5), sync 0x5A:0 (-90), LQI 40 (10 dB); rfu bits set must not leak
    const uint8_t a[LR2021_BLE_PACKET_STATUS_LEN] = {0x00, 0x27, 0x60, 0x5A, 0xFC, 40};
    lr2021_ble_packet_status_t s;
    lr2021_ble_parse_packet_status(a, &s);
    CHECK(s.len == 39 && near(s.rssi_avg_dbm, -96.5f) && near(s.rssi_sync_dbm, -90.0f) && near(s.lqi_db, 10),
          "ble status %u %.1f %.1f %.2f", s.len, s.rssi_avg_dbm, s.rssi_sync_dbm, s.lqi_db);
    const uint8_t a2[] = {0x01, 0x02, 0x10, 0x20, 0x01, 0};
    lr2021_ble_parse_packet_status(a2, &s);
    CHECK(s.len == 258 && near(s.rssi_avg_dbm, -16.0f) && near(s.rssi_sync_dbm, -32.5f) && s.lqi_db == 0,
          "ble status 2 %.1f %.1f", s.rssi_avg_dbm, s.rssi_sync_dbm);
}

static void oqpsk(void)
{
    lr2021_oqpsk_params_t p = {LR2021_OQPSK_250K, LR2021_PROTO_RX_BW_AUTO, 127, 32, false, false, false};
    n = lr2021_oqpsk_params(b, &p);
    WANT("oqpsk params", 0x02, 0x9F, 0x00, 0xFF, 0x7F, 0x00, 0x20, 0x00);
    // flags: address_on bit 2, bypass bit 1, fcs_mode bit 0
    p = (lr2021_oqpsk_params_t){LR2021_OQPSK_250K, 0x0B, 20, 0x0123, true, false, true};
    n = lr2021_oqpsk_params(b, &p);
    WANT("oqpsk params flags", 0x02, 0x9F, 0x00, 0x0B, 0x14, 0x01, 0x23, 0x05);
    p.address_filter = false, p.bypass_rx_length_check = true, p.fcs_in_fifo = false;
    n = lr2021_oqpsk_params(b, &p);
    CHECK(b[7] == 0x02, "bypass bit %02X", b[7]);

    n = lr2021_oqpsk_packet_len(b, 100);
    WANT("oqpsk len", 0x02, 0xA2, 100);

    const uint8_t la[8] = {0x00, 0x12, 0x4B, 0x00, 0x01, 0x02, 0x03, 0x04};
    const uint8_t sa[2] = {0xBE, 0xEF};
    n = lr2021_oqpsk_address(b, la, sa, 0x1A2B, 7);
    WANT("oqpsk address", 0x02, 0xA3, 0x00, 0x12, 0x4B, 0x00, 0x01, 0x02, 0x03, 0x04, 0xBE, 0xEF, 0x1A, 0x2B, 0x07);

    n = lr2021_oqpsk_get_rx_stats(b);
    WANT("oqpsk rx stats", 0x02, 0xA0);
    n = lr2021_oqpsk_get_packet_status(b);
    WANT("oqpsk packet status", 0x02, 0xA1);

    // PHR 20, 18 bytes in the FIFO (FCS checked and dropped), avg 0x4B:1 (-75.5), sync 0x4A:1 (-74.5), LQI 3
    const uint8_t a[LR2021_OQPSK_PACKET_STATUS_LEN] = {20, 0x00, 18, 0x4B, 0x4A, 0x05, 3};
    lr2021_oqpsk_packet_status_t s;
    lr2021_oqpsk_parse_packet_status(a, &s);
    CHECK(s.phr == 20 && s.len == 18 && near(s.rssi_avg_dbm, -75.5f) && near(s.rssi_sync_dbm, -74.5f) &&
              near(s.lqi_db, 0.75f),
          "oqpsk status %u %u %.1f %.1f %.2f", s.phr, s.len, s.rssi_avg_dbm, s.rssi_sync_dbm, s.lqi_db);
}

static void zwave(void)
{
    lr2021_zwave_params_t p = {LR2021_ZWAVE_R3, LR2021_PROTO_RX_BW_AUTO, LR2021_ZWAVE_FILTER_HOME_ID, 64, 0,
                               LR2021_PROTO_PBL_DETECT_AUTO, false};
    n = lr2021_zwave_params(b, &p);
    WANT("zwave params", 0x02, 0x97, 0x03, 0xFF, 0x01, 0x40, 0x00, 0x00, 0xFF, 0x00);
    p = (lr2021_zwave_params_t){LR2021_ZWAVE_LR1, 0x1C, LR2021_ZWAVE_FILTER_HOME_ID_BEAM, 170, 0x0140, 32, true};
    n = lr2021_zwave_params(b, &p);
    WANT("zwave params LR1", 0x02, 0x97, 0x00, 0x1C, 0x02, 0xAA, 0x01, 0x40, 0x20, 0x01);

    n = lr2021_zwave_home_id(b, 0xC0FFEE42);
    WANT("zwave home id", 0x02, 0x98, 0xC0, 0xFF, 0xEE, 0x42);

    // beam: addr_len bit 7, node_id(11:8) in bits 3:0; a node id past 12 bits is cut
    n = lr2021_zwave_beam_filter(b, 0x55, false, 0xFF, 0x4A);
    WANT("zwave beam 8", 0x02, 0x9B, 0x55, 0x00, 0xFF, 0x4A);
    n = lr2021_zwave_beam_filter(b, 0x55, true, 0xFABC, 0x0A);
    WANT("zwave beam 12", 0x02, 0x9B, 0x55, 0x8A, 0xBC, 0x0A);

    // a 3-channel scan at the DS's timeouts: 6 + 15 bytes; num_ch high nibble, CCA bits low, rates 2 bits each
    lr2021_zwave_scan_config_t c = {0};
    c.n = 3;
    c.ch[0] = (lr2021_zwave_scan_channel_t){908400000, LR2021_ZWAVE_R1, 19, false};
    c.ch[1] = (lr2021_zwave_scan_channel_t){908420000, LR2021_ZWAVE_R2, 34, true};
    c.ch[2] = (lr2021_zwave_scan_channel_t){916000000, LR2021_ZWAVE_R3, 8, false};
    c.addr_filter = LR2021_ZWAVE_FILTER_HOME_ID;
    n = lr2021_zwave_scan_config(b, &c);
    WANT("zwave scan 3", 0x02, 0x9C, 0x32, 0x39, 0x01, 0x00,       //
         0x36, 0x25, 0x15, 0x80, 19, 0x36, 0x25, 0x63, 0xA0, 34,    //
         0x36, 0x99, 0x0D, 0x00, 8);
    // four channels: LR1 in ch 4 is 0 in bits 7:6; the longest frame
    c.n = 4;
    c.ch[3] = (lr2021_zwave_scan_channel_t){912000000, LR2021_ZWAVE_LR1, 12, true};
    c.fcs_in_fifo = true;
    n = lr2021_zwave_scan_config(b, &c);
    CHECK(n == LR2021_PROTO_FRAME_MAX && b[2] == 0x4A && b[3] == 0x39 && b[5] == 0x01 && b[25] == 12,
          "zwave scan 4: %zu %02X %02X", n, b[2], b[3]);
    // refused: under 2 or over 4 channels
    c.n = 1;
    CHECK(lr2021_zwave_scan_config(b, &c) == 0, "1 channel accepted");
    c.n = 5;
    CHECK(lr2021_zwave_scan_config(b, &c) == 0, "5 channels accepted");

    n = lr2021_zwave_scan(b, LR2021_ZWAVE_SCAN_PLAIN);
    WANT("zwave scan", 0x02, 0x9D);
    n = lr2021_zwave_scan(b, LR2021_ZWAVE_SCAN_COMPUTE);
    WANT("zwave scan compute", 0x02, 0x9D, 0x01);
    n = lr2021_zwave_scan(b, LR2021_ZWAVE_SCAN_START);
    WANT("zwave scan start", 0x02, 0x9D, 0x02);

    n = lr2021_zwave_get_rx_stats(b);
    WANT("zwave rx stats", 0x02, 0x99);
    n = lr2021_zwave_get_packet_status(b);
    WANT("zwave packet status", 0x02, 0x9A);

    // 12 bytes at LR1 (sent as 4) on scan channel 2, avg 0x70:1, sync 0x6F:0, LQI 8 (2 dB)
    const uint8_t a[LR2021_ZWAVE_PACKET_STATUS_LEN] = {0x00, 12, 0x70, 0x6F, 0x04, (2 << 3) | 0x04, 8};
    lr2021_zwave_packet_status_t s;
    lr2021_zwave_parse_packet_status(a, &s);
    CHECK(s.len == 12 && near(s.rssi_avg_dbm, -112.5f) && near(s.rssi_sync_dbm, -111.0f) && s.last_detected == 4 &&
              s.rate == LR2021_ZWAVE_LR1 && s.channel == 2 && near(s.lqi_db, 2),
          "zwave status %u %.1f %.1f %u %d %u", s.len, s.rssi_avg_dbm, s.rssi_sync_dbm, s.last_detected, s.rate,
          s.channel);
    uint8_t a2[LR2021_ZWAVE_PACKET_STATUS_LEN] = {0, 1, 0, 0, 0x01, 0x01, 0};
    lr2021_zwave_parse_packet_status(a2, &s);
    CHECK(s.rate == LR2021_ZWAVE_R1 && s.channel == 0 && near(s.rssi_sync_dbm, -0.5f), "zwave R1");
    a2[4] = 3;
    lr2021_zwave_parse_packet_status(a2, &s);
    CHECK(s.rate == LR2021_ZWAVE_R3, "zwave R3");
    a2[4] = 0; // no patch, or nothing detected
    lr2021_zwave_parse_packet_status(a2, &s);
    CHECK(s.rate == -1, "zwave rate 0: %d", s.rate);
    a2[4] = 5;
    lr2021_zwave_parse_packet_status(a2, &s);
    CHECK(s.rate == -1, "zwave rate 5: %d", s.rate);
}

static void wmbus(void)
{
    lr2021_wmbus_params_t p = {LR2021_WMBUS_C1, LR2021_PROTO_RX_BW_AUTO, LR2021_WMBUS_FORMAT_B, true, 30, 64,
                               LR2021_PROTO_PBL_DETECT_AUTO};
    n = lr2021_wmbus_params(b, &p);
    WANT("wmbus params", 0x02, 0x6A, 0x05, 0xFF, 0x01, 0x01, 0x1E, 0x00, 0x40, 0xFF);
    p = (lr2021_wmbus_params_t){LR2021_WMBUS_F2, 0x2A, LR2021_WMBUS_FORMAT_A, false, 255, 0x0258, 24};
    n = lr2021_wmbus_params(b, &p);
    WANT("wmbus params F2", 0x02, 0x6A, 0x0C, 0x2A, 0x00, 0x00, 0xFF, 0x02, 0x58, 0x18);

    const uint8_t addr[6] = {0x78, 0x56, 0x34, 0x12, 0x01, 0x07};
    n = lr2021_wmbus_address(b, addr);
    WANT("wmbus address", 0x02, 0x6E, 0x78, 0x56, 0x34, 0x12, 0x01, 0x07);

    n = lr2021_wmbus_get_rx_stats(b);
    WANT("wmbus rx stats", 0x02, 0x6C);
    n = lr2021_wmbus_get_packet_status(b);
    WANT("wmbus packet status", 0x02, 0x6D);

    // L 0x2C, 49 bytes, avg 0x64:1 (-100.5), sync 0x63:1 (-99.5), CRC errors 0x1_0203 -> bits 16:9 0x81, 8:1 0x01,
    // bit 0 1; format B; LQI 4
    const uint8_t a[LR2021_WMBUS_PACKET_STATUS_LEN] = {0x2C, 0x00, 49, 0x64, 0x63, 0x81, 0x01, 0x80 | 0x40 | 0x10 | 0x01,
                                                       4};
    lr2021_wmbus_packet_status_t s;
    lr2021_wmbus_parse_packet_status(a, &s);
    CHECK(s.l_field == 0x2C && s.len == 49 && near(s.rssi_avg_dbm, -100.5f) && near(s.rssi_sync_dbm, -99.5f) &&
              s.crc_errors == 0x10203 && s.format == LR2021_WMBUS_FORMAT_B && near(s.lqi_db, 1),
          "wmbus status %u %u %.1f %.1f %05X %u", s.l_field, s.len, s.rssi_avg_dbm, s.rssi_sync_dbm,
          (unsigned)s.crc_errors, s.format);
    // the rfu bits (5, 3:1) set and nothing else: all zero
    const uint8_t a2[LR2021_WMBUS_PACKET_STATUS_LEN] = {0, 0, 0, 0, 0, 0, 0, 0x2E, 0};
    lr2021_wmbus_parse_packet_status(a2, &s);
    CHECK(s.crc_errors == 0 && s.format == 0 && s.rssi_avg_dbm == 0 && s.rssi_sync_dbm == 0, "wmbus rfu bits");
}

static void wisun(void)
{
    n = lr2021_wisun_mode(b, LR2021_WISUN_2A, LR2021_PROTO_RX_BW_AUTO);
    WANT("wisun mode", 0x02, 0x70, 0x02, 0xFF);
    n = lr2021_wisun_mode(b, LR2021_WISUN_5, 0x08);
    WANT("wisun mode 5", 0x02, 0x70, 0x07, 0x08);

    // FCS 16 bit 5, whitening 4, CRC by chip 3, mode switch 2, FEC 1:0
    lr2021_wisun_packet_t p = {true, true, true, false, LR2021_WISUN_FEC_NRNSC, 2047, 64, 24};
    n = lr2021_wisun_packet(b, &p);
    WANT("wisun packet", 0x02, 0x71, 0x39, 0x07, 0xFF, 0x40, 0x18);
    p = (lr2021_wisun_packet_t){false, false, false, true, LR2021_WISUN_FEC_RSC_INTERL, 0x1234, 8,
                                LR2021_PROTO_PBL_DETECT_AUTO};
    n = lr2021_wisun_packet(b, &p);
    WANT("wisun packet ms", 0x02, 0x71, 0x07, 0x12, 0x34, 0x08, 0xFF);

    n = lr2021_wisun_packet_len(b, 1500);
    WANT("wisun len", 0x02, 0x74, 0x05, 0xDC);
    n = lr2021_wisun_get_rx_stats(b);
    WANT("wisun rx stats", 0x02, 0x72);
    n = lr2021_wisun_get_packet_status(b);
    WANT("wisun packet status", 0x02, 0x73);

    // PHR 0x0812, 2066 bytes, avg 0x58:0 (-88), sync 0x57:1 (-87.5), sync word 1, LQI 12 (3 dB); rfu bits ignored
    const uint8_t a[LR2021_WISUN_PACKET_STATUS_LEN] = {0x08, 0x12, 0x08, 0x12, 0x58, 0x57, 0x80 | 0x78 | 0x02 | 0x01,
                                                       12};
    lr2021_wisun_packet_status_t s;
    lr2021_wisun_parse_packet_status(a, &s);
    CHECK(s.header == 0x0812 && s.len == 2066 && near(s.rssi_avg_dbm, -88.0f) && near(s.rssi_sync_dbm, -87.5f) &&
              s.syncword == 1 && near(s.lqi_db, 3),
          "wisun status %04X %u %.1f %.1f %u %.2f", s.header, s.len, s.rssi_avg_dbm, s.rssi_sync_dbm, s.syncword,
          s.lqi_db);
    const uint8_t a2[LR2021_WISUN_PACKET_STATUS_LEN] = {0, 0, 0, 1, 0x58, 0x57, 0x04, 0};
    lr2021_wisun_parse_packet_status(a2, &s);
    CHECK(s.syncword == 0 && near(s.rssi_avg_dbm, -88.5f) && near(s.rssi_sync_dbm, -87.0f), "wisun status 2");
}

int main(void)
{
    shared();
    ble();
    oqpsk();
    zwave();
    wmbus();
    wisun();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
