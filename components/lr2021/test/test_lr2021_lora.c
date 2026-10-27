// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
// Host tests for the LoRa, LR-FHSS and RTToF commands (lr2021_lora.h): the frames against the datasheet's tables
// (DS.LR20xx Rev. 2.2, chapters 6, 9, 10 and 17), the answers against hand-made ones, and time on air against
// Semtech's LoRa calculator (SF7 and SF12, 10 bytes) and the formula worked by hand.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "lr2021_lora.h"

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
    CHECK(same, "%s: got %s(%zu bytes)", what, g, n);
}

#define FRAME(call, ...)                           \
    do {                                           \
        uint8_t b_[LR2021_LR_FHSS_FRAME_MAX];      \
        static const uint8_t w_[] = {__VA_ARGS__}; \
        size_t n_ = call;                          \
        frame(b_, n_, w_, sizeof w_, #call);       \
    } while (0)

#define NEAR(x, want, tol) CHECK(fabs((double)(x) - (want)) <= (tol), "%s = %.6f, want %.6f", #x, (double)(x), (double)(want))

static void lora_frames(void)
{
    // SetLoraModulationParams (9.9.1): sf 7:4, bw 3:0; cr 7:4, ldro 1:0
    lr2021_lora_mod_t m = {7, LR2021_LORA_BW_125, LR2021_LORA_CR_4_5, false};
    FRAME(lr2021_lora_modulation(b_, &m), 0x02, 0x20, 0x74, 0x10);
    m = (lr2021_lora_mod_t){12, LR2021_LORA_BW_812, LR2021_LORA_CR_LI_4_8, true};
    FRAME(lr2021_lora_modulation(b_, &m), 0x02, 0x20, 0xCF, 0x71);
    // the same bytes as lr2021_cmd.h's minimal builder
    uint8_t a[8], b[8];
    m = (lr2021_lora_mod_t){9, LR2021_LORA_BW_41, LR2021_LORA_CR_CC_4_6, true};
    CHECK(lr2021_lora_modulation(a, &m) == lr2021_cmd_lora_modulation(b, 9, LR2021_LORA_BW_41, 8, 1) && !memcmp(a, b, 4),
          "matches lr2021_cmd_lora_modulation");

    // SetLoraPacketParams (9.9.3): preamble, length, header_type 2, crc 1, invert_iq 0
    lr2021_lora_pkt_t p = {8, 10, false, true, false};
    FRAME(lr2021_lora_packet(b_, &p), 0x02, 0x21, 0x00, 0x08, 0x0A, 0x02);
    p = (lr2021_lora_pkt_t){0x1234, 255, true, false, true};
    FRAME(lr2021_lora_packet(b_, &p), 0x02, 0x21, 0x12, 0x34, 0xFF, 0x05);

    // SetLoRaSynchTimeout (9.9.4): a count up to 255, else mantissa / exponent with format 1
    FRAME(lr2021_lora_synch_timeout_symbols(b_, 0), 0x02, 0x22, 0x00, 0x00);
    FRAME(lr2021_lora_synch_timeout_symbols(b_, 255), 0x02, 0x22, 0xFF, 0x00);
    FRAME(lr2021_lora_synch_timeout_symbols(b_, 256), 0x02, 0x22, 0x42, 0x01); // 8 x 2^5
    FRAME(lr2021_lora_synch_timeout(b_, 0x13, LR2021_LORA_TIMEOUT_MANT_EXP), 0x02, 0x22, 0x13, 0x01);
    uint8_t code;
    CHECK(lr2021_lora_mant_exp(300, &code) == 320 && code == 0x52, "300 symbols: %02X", code);
    CHECK(lr2021_lora_mant_exp(1000, &code) == 1024 && code == 0x43, "1000 symbols: %02X", code);
    CHECK(lr2021_lora_mant_exp(62, &code) == 62 && code == (31 << 3), "62 symbols: %02X", code);
    CHECK(lr2021_lora_mant_exp(65535, &code) == 65536 && code == 0x46, "65535 symbols: %02X", code);

    // sync words (9.9.5)
    FRAME(lr2021_lora_syncword(b_, LR2021_LORA_SYNCWORD_PUBLIC), 0x02, 0x23, 0x34);

    // side detectors (9.9.6, 9.9.7, 9.9.13): sf 7:4, ldro 3:2, invert_iq 0; none turns them off
    const lr2021_lora_side_det_t d[2] = {{8, false, true}, {9, true, false}};
    FRAME(lr2021_lora_side_det_config(b_, d, 2), 0x02, 0x24, 0x81, 0x94);
    FRAME(lr2021_lora_side_det_config(b_, d, 0), 0x02, 0x24);
    FRAME(lr2021_lora_side_det_config(b_, d, -1), 0x02, 0x24);
    const uint8_t sw[3] = {0x12, 0x34, 0x56};
    FRAME(lr2021_lora_side_det_syncword(b_, sw, 3), 0x02, 0x25, 0x12, 0x34, 0x56);
    FRAME(lr2021_lora_side_det_syncword(b_, sw, 5), 0x02, 0x25, 0x12, 0x34, 0x56);
    const uint8_t pnr[2] = {8, 0x10}, peak[2] = {60, 56};
    FRAME(lr2021_lora_side_det_cad(b_, pnr, peak, 2), 0x02, 0x1E, 0x08, 0x3C, 0x00, 0x38);

    // SetLoraCadParams (6.3.11): pbl_any is bit 4 beside pnr_delta; a 24-bit timeout
    lr2021_lora_cad_t c = {4, true, 8, LR2021_LORA_CAD_LBT, 0x123456, 51};
    FRAME(lr2021_lora_cad_params(b_, &c), 0x02, 0x27, 0x04, 0x18, 0x10, 0x12, 0x34, 0x56, 0x33);
    c = (lr2021_lora_cad_t){1, false, 0, LR2021_LORA_CAD_ONLY, 0xFF000000u, 55};
    FRAME(lr2021_lora_cad_params(b_, &c), 0x02, 0x27, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x37);
    FRAME(lr2021_lora_cad(b_), 0x02, 0x28);
    // table 6-19
    CHECK(lr2021_lora_cad_det_peak(7, 1) == 60 && lr2021_lora_cad_det_peak(12, 2) == 68 &&
              lr2021_lora_cad_det_peak(9, 3) == 56 && lr2021_lora_cad_det_peak(12, 8) == 64 &&
              lr2021_lora_cad_det_peak(5, 0) == 60 && lr2021_lora_cad_det_peak(4, 1) == 0,
          "CAD det_peak table");

    // SetLoraAddress (9.9.10): length 7:4, position 3:0, then the bytes; at most 8
    const uint8_t addr[9] = {0xAA, 0xBB, 3, 4, 5, 6, 7, 8, 9};
    FRAME(lr2021_lora_address(b_, 2, addr, 2), 0x02, 0x2B, 0x22, 0xAA, 0xBB);
    FRAME(lr2021_lora_address(b_, 0, addr, 0), 0x02, 0x2B, 0x00);
    FRAME(lr2021_lora_address(b_, 1, addr, 9), 0x02, 0x2B, 0x81, 0xAA, 0xBB, 3, 4, 5, 6, 7, 8);

    // SetLoraHopping (9.9.11): hop_ctrl in 7:6, period 12:0, then 32-bit frequencies
    const uint32_t hz[2] = {915000000, 916200000};
    FRAME(lr2021_lora_hopping(b_, 0x1234, hz, 2), 0x02, 0x2C, 0x52, 0x34, 0x36, 0x89, 0xCA, 0xC0, 0x36, 0x9C, 0x1A,
          0x40);
    FRAME(lr2021_lora_hopping(b_, 10, hz, 0), 0x02, 0x2C, 0x00, 0x0A);
    uint8_t big[LR2021_LORA_HOPPING_FRAME_MAX + 8];
    uint32_t many[50] = {0};
    CHECK(lr2021_lora_hopping(big, 1, many, 50) == LR2021_LORA_HOPPING_FRAME_MAX, "40 hops at most");

    // SetLoraTxSync (9.9.12): function 7:6, DIO 5:0
    FRAME(lr2021_lora_tx_sync(b_, LR2021_LORA_TX_SYNC_MANAGER, 7), 0x02, 0x1D, 0x47);
    FRAME(lr2021_lora_tx_sync(b_, LR2021_LORA_TX_SYNC_SUBORDINATE, 11), 0x02, 0x1D, 0x8B);
}

static void lora_answers(void)
{
    FRAME(lr2021_lora_get_rx_stats(b_), 0x02, 0x29);
    const uint8_t st[LR2021_LORA_RX_STATS_LEN] = {0x00, 0x05, 0x00, 0x01, 0x00, 0x02, 0x00, 0x03, 0x01, 0x00};
    lr2021_lora_rx_stats_t s;
    lr2021_lora_parse_rx_stats(st, &s);
    CHECK(s.packets == 5 && s.crc_errors == 1 && s.header_errors == 2 && s.headers_ok == 3 && s.false_syncs == 256,
          "rx stats");

    // GetLoraPacketStatus (table 9-13): CRC on, CR 4/8, 10 bytes, SNR -2.5 dB, RSSI 361 / 2 and 364 / 2 (the low
    // bits in byte 5 with detector side 1), -1000 Hz
    FRAME(lr2021_lora_get_packet_status(b_), 0x02, 0x2A);
    const uint8_t ps[LR2021_LORA_PACKET_STATUS_LEN] = {0x14, 0x0A, 0xF6, 0xB4, 0xB6, 0x0A, 0xFF, 0xFC, 0x18};
    lr2021_lora_packet_status_t q;
    lr2021_lora_parse_packet_status(ps, &q);
    CHECK(q.crc && q.cr == LR2021_LORA_CR_4_8 && q.length == 10, "packet status head");
    NEAR(q.snr_db, -2.5, 1e-6);
    NEAR(q.rssi_dbm, -180.5, 1e-6);
    NEAR(q.signal_rssi_dbm, -182.0, 1e-6);
    CHECK(q.detectors == LR2021_LORA_DET_SIDE1, "detectors %X", q.detectors);
    CHECK(q.freq_offset_hz == -1000, "freq offset %ld", (long)q.freq_offset_hz);
    const uint8_t ps2[LR2021_LORA_PACKET_STATUS_LEN] = {0x01, 0x00, 0x28, 0x2A, 0x2B, 0x07, 0x00, 0x03, 0xE8};
    lr2021_lora_parse_packet_status(ps2, &q);
    CHECK(!q.crc && q.cr == 1 && q.detectors == LR2021_LORA_DET_MAIN && q.freq_offset_hz == 1000, "packet status 2");
    NEAR(q.snr_db, 10.0, 1e-6);
    NEAR(q.rssi_dbm, -42.5, 1e-6);
    NEAR(q.signal_rssi_dbm, -43.5, 1e-6);
}

static void lora_helpers(void)
{
    NEAR(lr2021_lora_bw_hz(LR2021_LORA_BW_125), 125000, 0);
    NEAR(lr2021_lora_bw_hz(LR2021_LORA_BW_812), 812500, 0);
    NEAR(lr2021_lora_bw_hz(LR2021_LORA_BW_101), 101562.5, 0);
    NEAR(lr2021_lora_bw_hz(LR2021_LORA_BW_41), 41666.667, 0.001);
    CHECK(lr2021_lora_bw_hz(0x0) == 0 && lr2021_lora_bw_hz(0x9) == 0 && lr2021_lora_bw_hz(0x10) == 0, "RFU bw");
    CHECK(lr2021_lora_cr_denominator(LR2021_LORA_CR_4_7) == 7 && lr2021_lora_cr_denominator(LR2021_LORA_CR_LI_4_8) == 8 &&
              lr2021_lora_cr_denominator(LR2021_LORA_CR_CC_4_6) == 6 && lr2021_lora_cr_denominator(10) == 0,
          "CR denominators");
    CHECK(!lr2021_lora_cr_long(LR2021_LORA_CR_4_8) && lr2021_lora_cr_long(LR2021_LORA_CR_LI_4_5) &&
              lr2021_lora_cr_long(LR2021_LORA_CR_CC_4_8) && !lr2021_lora_cr_long(10),
          "long interleaver");

    // LDRO: DS 9.9.1 for the integer bandwidths; on at SF11 / SF12 for the SX128x ones
    CHECK(lr2021_lora_ldro(11, LR2021_LORA_BW_125) && !lr2021_lora_ldro(11, LR2021_LORA_BW_250) &&
              lr2021_lora_ldro(12, LR2021_LORA_BW_250) && !lr2021_lora_ldro(12, LR2021_LORA_BW_500) &&
              !lr2021_lora_ldro(12, LR2021_LORA_BW_1000) && !lr2021_lora_ldro(10, LR2021_LORA_BW_31) &&
              lr2021_lora_ldro(11, LR2021_LORA_BW_812) && lr2021_lora_ldro(12, LR2021_LORA_BW_41),
          "LDRO rule");

    NEAR(lr2021_lora_symbol_us(7, LR2021_LORA_BW_125), 1024, 1e-9);
    NEAR(lr2021_lora_symbol_us(12, LR2021_LORA_BW_812), 5041.2308, 1e-3);
    CHECK(lr2021_lora_symbol_us(7, 0x1) == 0, "symbol of RFU bw");
}

static void lora_toa(void)
{
    // Semtech's calculator: SF7, 125 kHz, CR 4/5, explicit, CRC, 8 symbols, 10 bytes: 41.22 ms. By hand:
    // 8 + 4.25 + 8 + ceil((80 - 28 + 28 + 16) / 28) x 5 = 40.25 symbols of 1.024 ms
    lr2021_lora_mod_t m = {7, LR2021_LORA_BW_125, LR2021_LORA_CR_4_5, false};
    lr2021_lora_pkt_t p = {8, 10, false, true, false};
    NEAR(lr2021_lora_toa_symbols(&m, &p), 40.25, 1e-9);
    NEAR(lr2021_lora_toa_us(&m, &p), 41216, 1e-6);
    CHECK(lr2021_lora_toa_ms(&m, &p) == 42, "SF7 ms %u", (unsigned)lr2021_lora_toa_ms(&m, &p));

    // SF12 with LDRO, the same packet: 991.23 ms (8 + 4.25 + 8 + ceil(76 / 40) x 5 = 30.25 symbols of 32.768 ms)
    m = (lr2021_lora_mod_t){12, LR2021_LORA_BW_125, LR2021_LORA_CR_4_5, true};
    NEAR(lr2021_lora_toa_symbols(&m, &p), 30.25, 1e-9);
    NEAR(lr2021_lora_toa_us(&m, &p), 991232, 1e-6);
    CHECK(lr2021_lora_toa_ms(&m, &p) == 992, "SF12 ms");

    // SF9, CR 4/7, implicit, no CRC, 12 symbols, 20 bytes: 8 + ceil((160 - 36 + 28 - 20) / 36) x 7 = 36 data
    // symbols, 52.25 in all, of 4.096 ms
    m = (lr2021_lora_mod_t){9, LR2021_LORA_BW_125, LR2021_LORA_CR_4_7, false};
    p = (lr2021_lora_pkt_t){12, 20, true, false, false};
    NEAR(lr2021_lora_toa_symbols(&m, &p), 52.25, 1e-9);
    NEAR(lr2021_lora_toa_us(&m, &p), 214016, 1e-6);

    // SF5 adds 2 sync symbols and no -4 SF + 8 term: 12 + 6.25 + 8 + ceil((8 + 16 - 20 + 20) / 20) x 5 = 36.25
    m = (lr2021_lora_mod_t){5, LR2021_LORA_BW_500, LR2021_LORA_CR_4_5, false};
    p = (lr2021_lora_pkt_t){12, 1, false, true, false};
    NEAR(lr2021_lora_toa_symbols(&m, &p), 36.25, 1e-9);
    NEAR(lr2021_lora_toa_us(&m, &p), 2320, 1e-6);

    // an empty explicit packet: the header block alone
    m = (lr2021_lora_mod_t){7, LR2021_LORA_BW_125, LR2021_LORA_CR_4_8, false};
    p = (lr2021_lora_pkt_t){8, 0, false, false, false};
    NEAR(lr2021_lora_toa_symbols(&m, &p), 20.25, 1e-9);

    // long interleaver, explicit: ceil((96 x 5 + 8 x 4 x 7) / 28) = 26 symbols after the preamble
    m = (lr2021_lora_mod_t){7, LR2021_LORA_BW_125, LR2021_LORA_CR_LI_4_5, false};
    p = (lr2021_lora_pkt_t){8, 10, false, true, false};
    NEAR(lr2021_lora_toa_symbols(&m, &p), 38.25, 1e-9);
    // implicit: a short payload fits the first 7 symbols' bits (ceil(96 / 20) = 5), a longer one doesn't
    // (ceil((224 + 240 - 160) / 28) = 11)
    m.cr = LR2021_LORA_CR_LI_4_6;
    p = (lr2021_lora_pkt_t){8, 2, true, false, false};
    NEAR(lr2021_lora_toa_symbols(&m, &p), 17.25, 1e-9);
    p.payload_len = 5;
    NEAR(lr2021_lora_toa_symbols(&m, &p), 23.25, 1e-9);
    // convolutional 4/8 at SF8: ceil((32 x 8 + 256) / 32) = 16
    m = (lr2021_lora_mod_t){8, LR2021_LORA_BW_125, LR2021_LORA_CR_CC_4_8, false};
    p = (lr2021_lora_pkt_t){8, 4, false, false, false};
    NEAR(lr2021_lora_toa_symbols(&m, &p), 28.25, 1e-9);

    // an RFU bandwidth has no time on air
    m.bw = 0x8;
    CHECK(lr2021_lora_toa_us(&m, &p) == 0 && lr2021_lora_toa_ms(&m, &p) == 0, "RFU bw toa");
}

static void lr_fhss(void)
{
    // LrFhssBuildFrame (table 17-1): headers 7:4 / cr 3:0, mod_type 7:4 / grid 3:0, hopping 7:4 / bw 3:0,
    // sequence bit 8 then 7:0, offset, payload
    lr2021_lr_fhss_t p = {3, LR2021_LR_FHSS_CR_1_3, LR2021_LR_FHSS_GRID_25, LR2021_LR_FHSS_HOPPING,
                          LR2021_LR_FHSS_BW_336, 0x1A5, -3};
    const uint8_t pl[2] = {0xDE, 0xAD};
    FRAME(lr2021_lr_fhss_build_frame(b_, &p, pl, 2), 0x02, 0x56, 0x33, 0x00, 0x14, 0x01, 0xA5, 0xFD, 0xDE, 0xAD);
    p.grid = LR2021_LR_FHSS_GRID_4;
    p.sequence = 0xFFFF;
    p.offset = 3;
    FRAME(lr2021_lr_fhss_build_frame(b_, &p, pl, 0), 0x02, 0x56, 0x33, 0x01, 0x14, 0x01, 0xFF, 0x03);
    uint8_t big[LR2021_LR_FHSS_FRAME_MAX];
    static uint8_t pl255[255];
    CHECK(lr2021_lr_fhss_build_frame(big, &p, pl255, 255) == LR2021_LR_FHSS_FRAME_MAX, "255-byte payload");

    FRAME(lr2021_lr_fhss_syncword(b_, LR2021_LR_FHSS_SYNCWORD_DEFAULT), 0x02, 0x57, 0x2C, 0x0F, 0x79, 0x95);

    // the hopping table (17.2.3, 17.2.4): the written frame reads back
    FRAME(lr2021_lr_fhss_read_table(b_), 0x02, 0x58);
    lr2021_lr_fhss_table_t t = {.hopping = true, .freq_hz = true, .pkt_length = 32, .used_freqs = 3, .blocks = 2,
                                .n = 2};
    t.hop[0].freq = 915000000;
    t.hop[0].symbols = 16;
    t.hop[1].freq = 916200000;
    t.hop[1].symbols = 0x0102;
    FRAME(lr2021_lr_fhss_write_table(b_, &t), 0x02, 0x59, 0x81, 0x00, 0x20, 0x03, 0x02, 0x36, 0x89, 0xCA, 0xC0, 0x00,
          0x10, 0x36, 0x9C, 0x1A, 0x40, 0x01, 0x02);
    uint8_t w[2 + LR2021_LR_FHSS_HOP_TABLE_LEN];
    size_t wn = lr2021_lr_fhss_write_table(w, &t);
    lr2021_lr_fhss_table_t r;
    CHECK(lr2021_lr_fhss_parse_table(w + 2, wn - 2, &r) == 2, "parse n %d", r.n);
    CHECK(r.hopping && r.freq_hz && r.pkt_length == 32 && r.used_freqs == 3 && r.blocks == 2 &&
              r.hop[1].freq == 916200000 && r.hop[1].symbols == 0x0102,
          "table round trip");
    // no hopping: freq0 only; a short answer: what fits
    w[2] = 0x00;
    CHECK(lr2021_lr_fhss_parse_table(w + 2, wn - 2, &r) == 1 && !r.hopping && r.hop[0].freq == 915000000, "freq0");
    w[2] = 0x01;
    CHECK(lr2021_lr_fhss_parse_table(w + 2, 5 + 6 + 3, &r) == 1, "short answer");
    CHECK(lr2021_lr_fhss_parse_table(w + 2, 4, &r) == 0, "no head");
    t.n = 60;
    CHECK(lr2021_lr_fhss_write_table(w, &t) == 2 + LR2021_LR_FHSS_HOP_TABLE_LEN, "40 entries at most");

    // time on air: 3 headers, CR 1/3, 10 bytes: ((10 + 2) x 8 + 6) x 3 = 306 bits, 6 x 50 + 18 + 2 = 320 on air,
    // + 342 header bits = 662, at 2.048 ms a bit
    p = (lr2021_lr_fhss_t){3, LR2021_LR_FHSS_CR_1_3, LR2021_LR_FHSS_GRID_25, LR2021_LR_FHSS_HOPPING,
                           LR2021_LR_FHSS_BW_137, 0, 0};
    CHECK(lr2021_lr_fhss_bits(&p, 10) == 662, "bits %u", (unsigned)lr2021_lr_fhss_bits(&p, 10));
    NEAR(lr2021_lr_fhss_toa_us(&p, 10), 1355776, 1e-6);
    CHECK(lr2021_lr_fhss_toa_ms(&p, 10) == 1356, "LR-FHSS ms");
    // 2 headers, CR 5/6: ceil(102 x 6 / 5) = 123 bits, 100 + 27 + 2 on air
    p.headers = 2;
    p.cr = LR2021_LR_FHSS_CR_5_6;
    CHECK(lr2021_lr_fhss_bits(&p, 10) == 357 && lr2021_lr_fhss_toa_ms(&p, 10) == 732, "CR 5/6");
    p.headers = 1;
    p.cr = LR2021_LR_FHSS_CR_2_3;
    CHECK(lr2021_lr_fhss_bits(&p, 10) == 275, "CR 2/3");
    p.headers = 2;
    p.cr = LR2021_LR_FHSS_CR_1_2;
    CHECK(lr2021_lr_fhss_bits(&p, 10) == 442, "CR 1/2");

    CHECK(lr2021_lr_fhss_sequences(&p) == 384, "25 kHz grid");
    p.grid = LR2021_LR_FHSS_GRID_4;
    p.bw = LR2021_LR_FHSS_BW_184;
    CHECK(lr2021_lr_fhss_sequences(&p) == 384, "4 kHz grid, narrow");
    p.bw = LR2021_LR_FHSS_BW_336;
    CHECK(lr2021_lr_fhss_sequences(&p) == 512, "4 kHz grid, wide");

    NEAR(lr2021_lr_fhss_bw_hz(LR2021_LR_FHSS_BW_39), 39062.5, 0);
    NEAR(lr2021_lr_fhss_bw_hz(LR2021_LR_FHSS_BW_1574), 1574218.75, 0);
    CHECK(lr2021_lr_fhss_bw_hz(10) == 0, "RFU LR-FHSS bw");
}

static void rttof(void)
{
    FRAME(lr2021_rttof_addr(b_, 0x12345678, 4), 0x02, 0x78, 0x12, 0x34, 0x56, 0x78, 0x04);
    FRAME(lr2021_rttof_req_addr(b_, 0x19), 0x02, 0x79, 0x00, 0x00, 0x00, 0x19);
    FRAME(lr2021_rttof_tx_rx_delay(b_, 21391), 0x02, 0x7B, 0x00, 0x00, 0x53, 0x8F);
    // SetRangingParams (10.2.6): extended bit 7, spy bit 6, symbols 5:0
    FRAME(lr2021_rttof_params(b_, true, true, 12), 0x02, 0x7C, 0xCC);
    FRAME(lr2021_rttof_params(b_, false, false, 64), 0x02, 0x7C, 0x00);
    FRAME(lr2021_rttof_get_result(b_, LR2021_RTTOF_RESULT_EXT), 0x02, 0x7A, 0x01);
    FRAME(lr2021_rttof_get_stats(b_), 0x02, 0x7D);

    // GetRangingResult (table 10-4): signed 24-bit results, RSSI -raw / 2
    const uint8_t res[LR2021_RTTOF_RESULT_EXT_LEN] = {0x00, 0x10, 0x00, 0xA0, 0xFF, 0xF0, 0x00, 0x9F};
    lr2021_rttof_result_t r;
    lr2021_rttof_parse_result(res, true, &r);
    CHECK(r.rng1 == 4096 && r.rng2 == -4096, "rng %ld %ld", (long)r.rng1, (long)r.rng2);
    NEAR(r.rssi1_dbm, -80.0, 1e-6);
    NEAR(r.rssi2_dbm, -79.5, 1e-6);
    lr2021_rttof_parse_result(res, false, &r);
    CHECK(r.rng1 == 4096 && r.rng2 == 0 && r.rssi2_dbm == 0, "plain result");

    // distance = rng x 150 / (2^12 x BW in MHz)
    NEAR(lr2021_rttof_distance_m(4096, LR2021_LORA_BW_1000), 150.0, 1e-9);
    NEAR(lr2021_rttof_distance_m(4096, LR2021_LORA_BW_812), 184.6154, 1e-3);
    NEAR(lr2021_rttof_distance_m(-2048, LR2021_LORA_BW_500), -150.0, 1e-9);
    CHECK(lr2021_rttof_distance_m(4096, 0x1) == 0, "RFU bw distance");

    const uint8_t st[LR2021_RTTOF_STATS_LEN] = {0, 10, 0, 9, 0, 8, 0, 7, 1, 6};
    lr2021_rttof_stats_t s;
    lr2021_rttof_parse_stats(st, &s);
    CHECK(s.exchanges_ok == 10 && s.requests_ok == 9 && s.responses == 8 && s.timeouts == 7 && s.discarded == 262,
          "ranging stats");
}

int main(void)
{
    lora_frames();
    lora_answers();
    lora_helpers();
    lora_toa();
    lr_fhss();
    rttof();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
