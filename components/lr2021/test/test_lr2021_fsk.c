// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
// Host tests for the generic modems' frames (lr2021_fsk.h): exact bytes against the datasheet's tables (DS 11.3,
// 15.2, 16.3, 18.4), the answer parsers on made-up answers, and time on air worked by hand.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "lr2021_fsk.h"

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
    char g[64] = "";
    for (size_t i = 0, o = 0; i < n && o + 4 < sizeof g; i++)
        o += (size_t)snprintf(g + o, sizeof g - o, "%02X ", got[i]);
    CHECK(same, "%s: got %s", what, g);
    CHECK(n <= LR2021_FSK_FRAME_MAX, "%s: %u bytes", what, (unsigned)n);
}
// the frame just built in b (length n) against the bytes given
#define WANT(what, ...)                               \
    do {                                              \
        static const uint8_t w_[] = {__VA_ARGS__};    \
        frame(b, n, w_, sizeof w_, what);             \
    } while (0)

static bool near(float a, float b) { return fabsf(a - b) < 1e-3f; }

static void shared(void)
{
    // 4800.5 b/s: 4800.5 x 256 = 1228928 = 0x12C080, with bit 31
    CHECK(lr2021_fsk_bitrate_frac(4800.5) == 0x8012C080u, "frac %08X", (unsigned)lr2021_fsk_bitrate_frac(4800.5));
    CHECK(lr2021_fsk_bitrate_bps(0x8012C080u) == 4800.5, "frac back");
    CHECK(lr2021_fsk_bitrate_bps(9600) == 9600.0, "whole");

    // rx_bw (table 11-4): 0x1C is 125 kS/s x 5 / 13 = 48.1 kHz; 0x00 3076.9 kHz; 0xE7 (231) 3.5 kHz; 0x88 1333.3
    CHECK(lr2021_fsk_rx_bw_hz(0x1C) == 48077, "0x1C: %u", (unsigned)lr2021_fsk_rx_bw_hz(0x1C));
    CHECK(lr2021_fsk_rx_bw_hz(0x00) == 3076923, "0x00: %u", (unsigned)lr2021_fsk_rx_bw_hz(0x00));
    CHECK(lr2021_fsk_rx_bw_hz(231) == 3472, "231: %u", (unsigned)lr2021_fsk_rx_bw_hz(231));
    CHECK(lr2021_fsk_rx_bw_hz(136) == 1333333, "136: %u", (unsigned)lr2021_fsk_rx_bw_hz(136));
    CHECK(lr2021_fsk_rx_bw_hz(222) == 8681, "222 (8.7 kHz): %u", (unsigned)lr2021_fsk_rx_bw_hz(222));
    // codes the table doesn't list
    CHECK(lr2021_fsk_rx_bw_hz(22) == 0 && lr2021_fsk_rx_bw_hz(214) == 0 && lr2021_fsk_rx_bw_hz(0xFF) == 0, "unlisted");
    // the narrowest at least as wide
    CHECK(lr2021_fsk_rx_bw_code(48000) == 0x1C, "48 kHz: %d", lr2021_fsk_rx_bw_code(48000));
    // just past 48.1 kHz: 55.6 kHz (227), not 46.3 (212)
    CHECK(lr2021_fsk_rx_bw_code(48078) == 227, "48.078 kHz: %d", lr2021_fsk_rx_bw_code(48078));
    CHECK(lr2021_fsk_rx_bw_code(1) == 231 && lr2021_fsk_rx_bw_code(3472) == 231, "narrowest");
    CHECK(lr2021_fsk_rx_bw_code(3473) == 167, "4.2 kHz next: %d", lr2021_fsk_rx_bw_code(3473));
    CHECK(lr2021_fsk_rx_bw_code(3076923) == 0 && lr2021_fsk_rx_bw_code(3076924) == -1, "widest");
    // every listed code is found again from its own width, and the list rises
    uint32_t last = 0;
    int listed = 0;
    for (int c = 0; c < 256; c++) {
        uint32_t hz = lr2021_fsk_rx_bw_hz((uint8_t)c);
        if (!hz)
            continue;
        listed++;
        CHECK(lr2021_fsk_rx_bw_hz((uint8_t)lr2021_fsk_rx_bw_code(hz)) == hz, "code %d round trip", c);
    }
    for (uint32_t hz = 1; hz < 3100000; hz += 997) {
        int c = lr2021_fsk_rx_bw_code(hz);
        uint32_t w = c < 0 ? 0 : lr2021_fsk_rx_bw_hz((uint8_t)c);
        if (c >= 0) {
            CHECK(w >= hz && w >= last, "code for %u", (unsigned)hz);
            last = w;
        }
    }
    CHECK(listed == 90, "listed %d", listed);

    // RSSI: 9 bits, -rssi / 2 dBm
    CHECK(lr2021_fsk_rssi_dbm(0x50, 0) == -80.0f && lr2021_fsk_rssi_dbm(0x50, 1) == -80.5f, "rssi");
    CHECK(lr2021_fsk_rssi_dbm(0x50, 0xFE) == -80.0f, "rssi uses bit 0 only");
}

static void fsk(void)
{
    uint8_t b[16];
    size_t n;

    // SetFskModulationParams (table 11-3): 4800 b/s, BT 0.5, 48.1 kHz, 2.4 kHz
    n = lr2021_fsk_modulation(b, 4800, LR2021_FSK_SHAPE_BT_0_5, 0x1C, 2400);
    WANT("fsk modulation", 0x02, 0x40, 0x00, 0x00, 0x12, 0xC0, 0x05, 0x1C, 0x00, 0x09, 0x60);
    // the same bytes as lr2021_cmd.h's short form for a whole rate
    uint8_t c[16];
    size_t cn = lr2021_cmd_fsk_modulation(c, 4800, 0x05, 0x1C, 2400);
    frame(b, n, c, cn, "fsk modulation as cmd's");
    // the fractional form keeps bit 31; fdev past 24 bits is cut
    n = lr2021_fsk_modulation(b, lr2021_fsk_bitrate_frac(4800.5), LR2021_FSK_SHAPE_RRC_0_3, 0x1C, 0x1123456);
    WANT("fsk modulation frac", 0x02, 0x40, 0x80, 0x12, 0xC0, 0x80, 0x0C, 0x1C, 0x12, 0x34, 0x56);

    // SetFskPacketParams (table 11-5): all off is lr2021_cmd_fsk_packet's frame
    lr2021_fsk_packet_t p = {0};
    p.payload_len = 33;
    n = lr2021_fsk_packet(b, &p);
    WANT("fsk packet plain", 0x02, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0x00);
    cn = lr2021_cmd_fsk_packet(c, 0, 33);
    frame(b, n, c, cn, "fsk packet as cmd's");
    // everything on: byte 5 = long preamble (bit 5) | addr 2 << 2 | format 1; byte 8 = CRC 0xA << 4 | whitening
    p = (lr2021_fsk_packet_t){.preamble_bits = 32,
                              .detect_bits = 16,
                              .long_preamble = true,
                              .addr = LR2021_FSK_ADDR_NODE_BCAST,
                              .format = LR2021_FSK_FORMAT_VAR_8,
                              .payload_len = 255,
                              .crc = LR2021_FSK_CRC_2_INV,
                              .whitening = true};
    n = lr2021_fsk_packet(b, &p);
    WANT("fsk packet full", 0x02, 0x41, 0x00, 0x20, 0x10, 0x29, 0x00, 0xFF, 0xA1);
    // length in bits is bit 4 (not the long preamble's)
    p = (lr2021_fsk_packet_t){.len_in_bits = true, .format = LR2021_FSK_FORMAT_VAR_16, .payload_len = 0x1234};
    n = lr2021_fsk_packet(b, &p);
    WANT("fsk packet bits", 0x02, 0x41, 0x00, 0x00, 0x00, 0x13, 0x12, 0x34, 0x00);

    // SetFskWhiteningParams (table 11-11): type in bits 7:4, a 12-bit seed
    n = lr2021_fsk_whitening(b, LR2021_FSK_WHITEN_SX128X, 0x1FF);
    WANT("fsk whitening", 0x02, 0x42, 0x11, 0xFF);
    n = lr2021_fsk_whitening(b, LR2021_FSK_WHITEN_SX126X, 0xF123);
    WANT("fsk whitening 12 bits", 0x02, 0x42, 0x01, 0x23);

    // SetFskCrcParams (table 11-12): CCITT
    n = lr2021_fsk_crc(b, 0x1021, 0x1D0F);
    WANT("fsk crc", 0x02, 0x43, 0x00, 0x00, 0x10, 0x21, 0x00, 0x00, 0x1D, 0x0F);

    // SetFskSyncword (table 11-13): the low bits, MSB first in bit 7 of the last byte
    n = lr2021_fsk_syncword(b, 0x12AD, 16, true);
    WANT("fsk sync 16", 0x02, 0x44, 0, 0, 0, 0, 0, 0, 0x12, 0xAD, 0x90);
    n = lr2021_fsk_syncword(b, 0xFFFF12ADull, 16, true);
    WANT("fsk sync 16 masked", 0x02, 0x44, 0, 0, 0, 0, 0, 0, 0x12, 0xAD, 0x90);
    n = lr2021_fsk_syncword(b, 0x0123456789ABCDEFull, 70, false);
    WANT("fsk sync 64", 0x02, 0x44, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0x40);
    n = lr2021_fsk_syncword(b, 0xABC, 0, true);
    WANT("fsk no sync", 0x02, 0x44, 0, 0, 0, 0, 0, 0, 0, 0, 0x80);

    n = lr2021_fsk_address(b, 0x11, 0xFF);
    WANT("fsk address", 0x02, 0x45, 0x11, 0xFF);

    // GetFskRxStats (tables 11-15, 11-16)
    n = lr2021_fsk_get_rx_stats(b);
    WANT("fsk rx stats", 0x02, 0x46);
    const uint8_t st[LR2021_FSK_RX_STATS_LEN] = {0, 10, 0, 1, 0, 2, 0, 12, 0, 11, 0, 1, 1, 0};
    lr2021_fsk_rx_stats_t s;
    lr2021_fsk_parse_rx_stats(st, &s);
    CHECK(s.packets == 10 && s.crc_errors == 1 && s.len_errors == 2 && s.preambles == 12 && s.sync_ok == 11 &&
              s.sync_fail == 1 && s.timeouts == 256,
          "fsk stats");

    // GetFskPacketStatus (tables 11-17, 11-18): byte 4 0x24: broadcast match, rssi_avg bit 0 set
    n = lr2021_fsk_get_packet_status(b);
    WANT("fsk packet status", 0x02, 0x47);
    const uint8_t ps[LR2021_FSK_PACKET_STATUS_LEN] = {0x00, 0x21, 0x50, 0x4E, 0x24, 0x30};
    lr2021_fsk_packet_status_t q;
    lr2021_fsk_parse_packet_status(ps, &q);
    CHECK(q.len == 33 && near(q.rssi_avg_dbm, -80.5f) && near(q.rssi_sync_dbm, -78.0f) && q.addr_bcast &&
              !q.addr_node && near(q.lqi_db, 12.0f),
          "fsk status %u %.1f %.1f %d %d %.2f", q.len, q.rssi_avg_dbm, q.rssi_sync_dbm, q.addr_bcast, q.addr_node,
          q.lqi_db);
    const uint8_t ps2[LR2021_FSK_PACKET_STATUS_LEN] = {0x01, 0x00, 0x50, 0x4E, 0x11, 0x00};
    lr2021_fsk_parse_packet_status(ps2, &q);
    CHECK(q.len == 256 && near(q.rssi_avg_dbm, -80.0f) && near(q.rssi_sync_dbm, -78.5f) && !q.addr_bcast &&
              q.addr_node && q.lqi_db == 0,
          "fsk status 2");
}

static void fsk_time(void)
{
    // 32 preamble + 16 sync + 8 header + 20 x 8 payload + 16 CRC = 232 bits; at 4800 b/s 48 333.3 us -> 48 334
    lr2021_fsk_packet_t p = {.preamble_bits = 32,
                             .format = LR2021_FSK_FORMAT_VAR_8,
                             .payload_len = 20,
                             .crc = LR2021_FSK_CRC_2_INV};
    CHECK(lr2021_fsk_packet_bits(&p, 16) == 232, "bits %u", (unsigned)lr2021_fsk_packet_bits(&p, 16));
    CHECK(lr2021_fsk_time_on_air_us(&p, 16, 4800) == 48334, "toa %u",
          (unsigned)lr2021_fsk_time_on_air_us(&p, 16, 4800));
    // an address byte in a variable-length packet: 240 bits, 50 ms exactly; the same rate in the fractional form
    p.addr = LR2021_FSK_ADDR_NODE;
    CHECK(lr2021_fsk_time_on_air_us(&p, 16, 4800) == 50000, "toa addr");
    CHECK(lr2021_fsk_time_on_air_us(&p, 16, lr2021_fsk_bitrate_frac(4800)) == 50000, "toa frac");
    // fixed length: no header, the address inside the payload: 224 bits, 46 666.7 -> 46 667
    p.format = LR2021_FSK_FORMAT_FIXED;
    CHECK(lr2021_fsk_time_on_air_us(&p, 16, 4800) == 46667, "toa fixed");
    // the 9- and 16-bit headers
    p.addr = LR2021_FSK_ADDR_OFF;
    p.format = LR2021_FSK_FORMAT_VAR_9;
    CHECK(lr2021_fsk_packet_bits(&p, 16) == 233, "9-bit header");
    p.format = LR2021_FSK_FORMAT_VAR_16;
    CHECK(lr2021_fsk_packet_bits(&p, 16) == 240, "16-bit header");
    // 13 bits at 1000 b/s; a zero rate gives 0
    p = (lr2021_fsk_packet_t){.len_in_bits = true, .payload_len = 13};
    CHECK(lr2021_fsk_time_on_air_us(&p, 0, 1000) == 13000, "bits payload");
    CHECK(lr2021_fsk_time_on_air_us(&p, 0, 0) == 0 && lr2021_fsk_time_on_air_us(&p, 0, LR2021_FSK_BITRATE_FRAC) == 0,
          "zero rate");
    // the 2-FSK packet of spec R3.21 (33 bytes, no preamble, no sync word, 4800 b/s) should last 55 ms
    p = (lr2021_fsk_packet_t){.payload_len = 33};
    CHECK(lr2021_fsk_time_on_air_us(&p, 0, 4800) == 55000, "R3.21 packet");
}

static void ook(void)
{
    uint8_t b[16];
    size_t n;

    // SetOokModulationParams (table 16-1): as lr2021_cmd_ook_modulation for a whole rate
    n = lr2021_ook_modulation(b, 4096, LR2021_FSK_SHAPE_NONE, 0x1C, LR2021_OOK_DEPTH_20DB);
    WANT("ook modulation", 0x02, 0x81, 0x00, 0x00, 0x10, 0x00, 0x00, 0x1C, 0x01);
    uint8_t c[16];
    size_t cn = lr2021_cmd_ook_modulation(c, 4096, 0, 0x1C, 1);
    frame(b, n, c, cn, "ook modulation as cmd's");
    n = lr2021_ook_modulation(b, lr2021_fsk_bitrate_frac(1000.25), LR2021_FSK_SHAPE_BT_1_0, 0x26, 0);
    WANT("ook modulation frac", 0x02, 0x81, 0x80, 0x03, 0xE8, 0x40, 0x07, 0x26, 0x00);

    // SetOokPacketParams (table 16-2)
    lr2021_ook_packet_t p = {.preamble_bits = 16,
                             .addr = LR2021_FSK_ADDR_NODE,
                             .format = LR2021_FSK_FORMAT_VAR_8,
                             .payload_len = 64,
                             .crc = LR2021_FSK_CRC_2,
                             .encoding = LR2021_OOK_ENC_MANCHESTER};
    n = lr2021_ook_packet(b, &p);
    WANT("ook packet", 0x02, 0x82, 0x00, 0x10, 0x05, 0x00, 0x40, 0x21);
    p = (lr2021_ook_packet_t){.format = LR2021_FSK_FORMAT_VAR_16,
                              .payload_len = 0x0102,
                              .crc = LR2021_FSK_CRC_4_INV,
                              .encoding = LR2021_OOK_ENC_BIPHASE_INV};
    n = lr2021_ook_packet(b, &p);
    WANT("ook packet 2", 0x02, 0x82, 0x00, 0x00, 0x03, 0x01, 0x02, 0xCA);

    n = lr2021_ook_crc(b, 0x8005, 0xFFFF);
    WANT("ook crc", 0x02, 0x83, 0x00, 0x00, 0x80, 0x05, 0x00, 0x00, 0xFF, 0xFF);
    n = lr2021_ook_syncword(b, 0xFF2DD4, 16, true);
    WANT("ook sync", 0x02, 0x84, 0x00, 0x00, 0x2D, 0xD4, 0x90);
    n = lr2021_ook_syncword(b, 0xDEADBEEF, 40, false);
    WANT("ook sync 32", 0x02, 0x84, 0xDE, 0xAD, 0xBE, 0xEF, 0x20);
    n = lr2021_ook_address(b, 0x01, 0xFF);
    WANT("ook address", 0x02, 0x85, 0x01, 0xFF);

    // SetOokDetector (table 16-10): ADS-B's pattern 0x0285, 10 bits (sent as 9), no repeats, falling edge, no SFD
    lr2021_ook_detector_t d = {.pattern = 0x0285, .pattern_bits = 10};
    n = lr2021_ook_detector(b, &d);
    WANT("ook detector ads-b", 0x02, 0x88, 0x02, 0x85, 0x09, 0x00, 0x00);
    d = (lr2021_ook_detector_t){
        .pattern = 0x5555, .pattern_bits = 16, .repeats = 31, .sync_raw = true, .sfd_rising = true, .sfd_bits = 4};
    n = lr2021_ook_detector(b, &d);
    WANT("ook detector", 0x02, 0x88, 0x55, 0x55, 0x0F, 0x1F, 0x34);

    // SetOokWhiteningParams (table 16-11): bit index 7:4 and the polynomial's top 4 bits; the seed's top 4 bits
    n = lr2021_ook_whitening(b, 5, 0x108, 0xFFFF);
    WANT("ook whitening", 0x02, 0x89, 0x51, 0x08, 0x0F, 0xFF);

    n = lr2021_ook_get_rx_stats(b);
    WANT("ook rx stats", 0x02, 0x86);
    const uint8_t st[LR2021_OOK_RX_STATS_LEN] = {0x12, 0x34, 0, 5, 0, 6};
    lr2021_ook_rx_stats_t s;
    lr2021_ook_parse_rx_stats(st, &s);
    CHECK(s.packets == 0x1234 && s.crc_errors == 5 && s.len_errors == 6, "ook stats");

    n = lr2021_ook_get_packet_status(b);
    WANT("ook packet status", 0x02, 0x87);
    const uint8_t ps[LR2021_OOK_PACKET_STATUS_LEN] = {0x00, 0x08, 0x64, 0x3C, 0x15, 0x50};
    lr2021_ook_packet_status_t q;
    lr2021_ook_parse_packet_status(ps, &q);
    CHECK(q.len == 8 && near(q.rssi_avg_dbm, -100.5f) && near(q.rssi_high_dbm, -60.5f) && !q.addr_bcast &&
              q.addr_node && near(q.lqi_db, 20.0f),
          "ook status %.1f %.1f", q.rssi_avg_dbm, q.rssi_high_dbm);

    // time on air: 16 preamble, then (16 sync + 8 header + 4 x 8 + 16 CRC) x 2 for Manchester = 160 bits;
    // at 4096 b/s 39 062.5 us -> 39 063
    p = (lr2021_ook_packet_t){.preamble_bits = 16,
                              .format = LR2021_FSK_FORMAT_VAR_8,
                              .payload_len = 4,
                              .crc = LR2021_FSK_CRC_2,
                              .encoding = LR2021_OOK_ENC_MANCHESTER};
    CHECK(lr2021_ook_packet_bits(&p, 16) == 160, "ook bits %u", (unsigned)lr2021_ook_packet_bits(&p, 16));
    CHECK(lr2021_ook_time_on_air_us(&p, 16, 4096) == 39063, "ook toa");
    p.encoding = LR2021_OOK_ENC_NONE;
    CHECK(lr2021_ook_packet_bits(&p, 16) == 88, "ook bits uncoded");
}

static void flrc(void)
{
    uint8_t b[16];
    size_t n;

    CHECK(lr2021_flrc_bitrate(LR2021_FLRC_260) == 260000 && lr2021_flrc_bw_hz(LR2021_FLRC_650) == 740000 &&
              lr2021_flrc_bitrate(8) == 0 && lr2021_flrc_bw_hz(8) == 0,
          "flrc rates");

    // SetFlrcModulationParams (table 18-4): cr in bits 7:4, shape in 3:0
    n = lr2021_flrc_modulation(b, LR2021_FLRC_650, LR2021_FLRC_CR_1_2, LR2021_FSK_SHAPE_BT_0_5);
    WANT("flrc modulation", 0x02, 0x48, 0x04, 0x05);
    n = lr2021_flrc_modulation(b, LR2021_FLRC_2600, LR2021_FLRC_CR_2_3, LR2021_FSK_SHAPE_BT_1_0);
    WANT("flrc modulation 2", 0x02, 0x48, 0x00, 0x37);

    // SetFlrcPacketParams (table 18-5): 32-bit preamble (7) and sync word (2): 7 << 2 | 2; sync 1 sent and matched,
    // variable length, 2-byte CRC (1): 1 << 6 | 1 << 3 | 1
    lr2021_flrc_packet_t p = {.preamble_bits = 32,
                              .sync_bits = 32,
                              .sync_tx = 1,
                              .sync_match = 1,
                              .crc_bytes = 2,
                              .payload_len = 100};
    n = lr2021_flrc_packet(b, &p);
    WANT("flrc packet", 0x02, 0x49, 0x1E, 0x49, 0x00, 0x64);
    // 8 bits (1), 16-bit sync (1); sync 2 sent, 2 or 3 matched (6), fixed, 4-byte CRC (3)
    p = (lr2021_flrc_packet_t){.preamble_bits = 8,
                               .sync_bits = 16,
                               .sync_tx = 2,
                               .sync_match = 6,
                               .fixed = true,
                               .crc_bytes = 4,
                               .payload_len = 511};
    n = lr2021_flrc_packet(b, &p);
    WANT("flrc packet 2", 0x02, 0x49, 0x05, 0xB7, 0x01, 0xFF);
    // out of range: preamble clamped to 4..32, odd sync and CRC lengths taken as none
    p = (lr2021_flrc_packet_t){.preamble_bits = 0, .sync_bits = 24, .crc_bytes = 1, .payload_len = 6};
    n = lr2021_flrc_packet(b, &p);
    WANT("flrc packet clamp", 0x02, 0x49, 0x00, 0x00, 0x00, 0x06);
    p.preamble_bits = 200;
    n = lr2021_flrc_packet(b, &p);
    WANT("flrc packet clamp 2", 0x02, 0x49, 0x1C, 0x00, 0x00, 0x06);

    // SetFlrcSyncword (table 18-10): 32 bits, or 16 with bytes 5 and 6 left off
    n = lr2021_flrc_syncword(b, 1, 0x12345678, 32);
    WANT("flrc sync 32", 0x02, 0x4C, 0x01, 0x12, 0x34, 0x56, 0x78);
    n = lr2021_flrc_syncword(b, 2, 0xABCD, 16);
    WANT("flrc sync 16", 0x02, 0x4C, 0x02, 0xAB, 0xCD);

    n = lr2021_flrc_get_rx_stats(b);
    WANT("flrc rx stats", 0x02, 0x4A);
    const uint8_t st[LR2021_FLRC_RX_STATS_LEN] = {0, 100, 0, 3, 1, 0};
    lr2021_flrc_rx_stats_t s;
    lr2021_flrc_parse_rx_stats(st, &s);
    CHECK(s.packets == 100 && s.crc_errors == 3 && s.len_errors == 256, "flrc stats");

    // GetFlrcPacketStatus (table 18-9): sync word 2, both RSSI bit 0s set, -1000 Hz
    n = lr2021_flrc_get_packet_status(b);
    WANT("flrc packet status", 0x02, 0x4B);
    const uint8_t ps[LR2021_FLRC_PACKET_STATUS_LEN] = {0x01, 0x00, 0x46, 0x45, 0x25, 0xFF, 0xFC, 0x18};
    lr2021_flrc_packet_status_t q;
    lr2021_flrc_parse_packet_status(ps, &q);
    CHECK(q.len == 256 && near(q.rssi_avg_dbm, -70.5f) && near(q.rssi_sync_dbm, -69.5f) && q.sync_num == 2 &&
              q.freq_offset_hz == -1000,
          "flrc status %.1f %.1f %u %d", q.rssi_avg_dbm, q.rssi_sync_dbm, q.sync_num, (int)q.freq_offset_hz);
    const uint8_t ps2[LR2021_FLRC_PACKET_STATUS_LEN] = {0, 6, 0x46, 0x45, 0x10, 0x00, 0x03, 0xE8};
    lr2021_flrc_parse_packet_status(ps2, &q);
    CHECK(q.freq_offset_hz == 1000 && q.sync_num == 1 && near(q.rssi_avg_dbm, -70.0f), "flrc status 2");
    const uint8_t ps3[LR2021_FLRC_PACKET_STATUS_LEN] = {0, 6, 0, 0, 0, 0x80, 0x00, 0x00};
    lr2021_flrc_parse_packet_status(ps3, &q);
    CHECK(q.freq_offset_hz == -8388608, "flrc most negative offset");
}

static void flrc_time(void)
{
    // 650 kb/s, CR 1/2, 32-bit preamble and sync word, variable length, 2-byte CRC, 100 bytes:
    // coded 16 header + 6 tail + 8 x 102 = 838, x 2 = 1676; uncoded 32 + 21 + 32 = 85; 1761 bits = 2709.2 us
    lr2021_flrc_packet_t p = {.preamble_bits = 32, .sync_bits = 32, .crc_bytes = 2, .payload_len = 100};
    CHECK(lr2021_flrc_packet_bits(&p, LR2021_FLRC_CR_1_2) == 1761, "flrc bits %u",
          (unsigned)lr2021_flrc_packet_bits(&p, LR2021_FLRC_CR_1_2));
    CHECK(lr2021_flrc_time_on_air_us(&p, LR2021_FLRC_650, LR2021_FLRC_CR_1_2) == 2710, "flrc toa");
    // CR 1, fixed, 16-bit preamble and sync word, no CRC, 10 bytes: 80 coded + 53 = 133 bits at 2.6 Mb/s = 51.2 us
    p = (lr2021_flrc_packet_t){.preamble_bits = 16, .sync_bits = 16, .fixed = true, .payload_len = 10};
    CHECK(lr2021_flrc_packet_bits(&p, LR2021_FLRC_CR_1) == 133, "flrc bits cr 1");
    CHECK(lr2021_flrc_time_on_air_us(&p, LR2021_FLRC_2600, LR2021_FLRC_CR_1) == 52, "flrc toa cr 1");
    // CR 3/4, variable, 4-byte CRC: coded 16 + 6 + 8 x 14 = 134, x 4/3 = 178.7 -> 179; + 53 = 232
    p = (lr2021_flrc_packet_t){.preamble_bits = 16, .sync_bits = 16, .crc_bytes = 4, .payload_len = 10};
    CHECK(lr2021_flrc_packet_bits(&p, LR2021_FLRC_CR_3_4) == 232, "flrc bits cr 3/4: %u",
          (unsigned)lr2021_flrc_packet_bits(&p, LR2021_FLRC_CR_3_4));
    // CR 2/3: 134 x 3/2 = 201; + 53 = 254
    CHECK(lr2021_flrc_packet_bits(&p, LR2021_FLRC_CR_2_3) == 254, "flrc bits cr 2/3");
    CHECK(lr2021_flrc_packet_bits(&p, 4) == 0 && lr2021_flrc_time_on_air_us(&p, 8, LR2021_FLRC_CR_1_2) == 0,
          "flrc bad codes");
}

static void bpsk(void)
{
    uint8_t b[16];
    size_t n;

    // SetBpskModulationParams (table 15-1): shape 7:4, diff_mode bit 2, parity bit 1, previous bit 0
    n = lr2021_bpsk_modulation(b, 100, LR2021_FSK_SHAPE_RC_0_5, true, true, false);
    WANT("bpsk modulation", 0x02, 0x50, 0x00, 0x00, 0x00, 0x64, 0x95);
    n = lr2021_bpsk_modulation(b, 600, LR2021_FSK_SHAPE_NONE, true, false, true);
    WANT("bpsk modulation parity", 0x02, 0x50, 0x00, 0x00, 0x02, 0x58, 0x06);
    n = lr2021_bpsk_modulation(b, lr2021_fsk_bitrate_frac(100.5), LR2021_FSK_SHAPE_RRC_1_0, false, false, false);
    WANT("bpsk modulation frac", 0x02, 0x50, 0x80, 0x00, 0x64, 0x80, 0xF0);

    // SetBpskPacketParams (table 15-2): mode 5:4, message type bit 2, rank 1:0
    n = lr2021_bpsk_packet(b, 12, LR2021_BPSK_SIGFOX, true, 2);
    WANT("bpsk packet sigfox", 0x02, 0x51, 0x0C, 0x16);
    n = lr2021_bpsk_packet(b, 160, LR2021_BPSK_RAW, false, 0);
    WANT("bpsk packet raw", 0x02, 0x51, 0xA0, 0x00);
}

int main(void)
{
    shared();
    fsk();
    fsk_time();
    ook();
    flrc();
    flrc_time();
    bpsk();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
