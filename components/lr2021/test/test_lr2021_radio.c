// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
// Host tests for the LR2021 system and common radio commands (lr2021_radio.h): every builder's bytes, the answer
// parsers with known answers, the clamps. Where the datasheet gives an example (CAD threshold, XOSC trims, the RSSI
// and temperature formulas, the version), the expected value is the datasheet's.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "lr2021_radio.h"

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
    CHECK(same, "%s: got %s(%zu bytes)", what, g, n);
}
#define WANT(call, ...)                                     \
    do {                                                    \
        static const uint8_t w_[] = {__VA_ARGS__};          \
        size_t n_ = (call);                                 \
        frame(b, n_, w_, sizeof w_, #call);                 \
    } while (0)

static bool near(double a, double b, double tol) { return fabs(a - b) <= tol; }

static void modes(void)
{
    uint8_t b[LR2021_RADIO_FRAME_MAX];
    WANT(lr2021_radio_set_sleep(b, true, true, 32768), 0x01, 0x27, 0x03, 0x00, 0x00, 0x80, 0x00);
    WANT(lr2021_radio_set_sleep(b, false, false, 0), 0x01, 0x27, 0x00, 0x00, 0x00, 0x00, 0x00);
    WANT(lr2021_radio_set_sleep(b, false, true, 0), 0x01, 0x27, 0x02, 0x00, 0x00, 0x00, 0x00);
    WANT(lr2021_radio_set_standby(b, false), 0x01, 0x28, 0x00);
    WANT(lr2021_radio_set_standby(b, true), 0x01, 0x28, 0x01);
    WANT(lr2021_radio_set_fs(b), 0x01, 0x29);
    WANT(lr2021_radio_add_retained_reg(b, 31, LR2021_REG_RX_FIFO_BASE), 0x01, 0x2A, 0x1F, 0xF3, 0x00, 0x28);
    WANT(lr2021_radio_set_rx_default(b), 0x02, 0x0C);
    WANT(lr2021_radio_set_tx_default(b), 0x02, 0x0D);
    WANT(lr2021_radio_set_fallback(b, LR2021_FALLBACK_FS), 0x02, 0x06, 0x03);
    // the mode goes in bits 7:4 of the last byte (DS table 6-14)
    WANT(lr2021_radio_set_rx_duty_cycle(b, 0x000100, 0x001000, LR2021_DUTY_LORA_CAD), 0x02, 0x10, 0x00, 0x01, 0x00,
         0x00, 0x10, 0x00, 0x10);
    WANT(lr2021_radio_set_rx_duty_cycle(b, 0x1ABCDEF, 0x2FEDCBA, LR2021_DUTY_RX), 0x02, 0x10, 0xAB, 0xCD, 0xEF, 0xFE,
         0xDC, 0xBA, 0x00);
    WANT(lr2021_radio_set_auto_rx_tx(b, true, LR2021_AUTO_RX_OK, 0x123456, 0x01020304), 0x02, 0x11, 0x82, 0x12, 0x34,
         0x56, 0x01, 0x02, 0x03, 0x04);
    WANT(lr2021_radio_set_auto_rx_tx(b, false, LR2021_AUTO_OFF, 0, 0), 0x02, 0x11, 0, 0, 0, 0, 0, 0, 0, 0);
    WANT(lr2021_radio_stop_timeout_on_preamble(b, true), 0x02, 0x09, 0x01);
    WANT(lr2021_radio_stop_timeout_on_preamble(b, false), 0x02, 0x09, 0x00);
    // DS 6.3.14: 0x50 is a -80 dBm threshold
    WANT(lr2021_radio_set_cad_params(b, lr2021_radio_hf_ticks(1000), -80, LR2021_CAD_EXIT_TX, 0x000200), 0x02, 0x1B,
         0x00, 0x7D, 0x00, 0x50, 0x01, 0x00, 0x02, 0x00);
    WANT(lr2021_radio_set_cad_params(b, 0, 10, LR2021_CAD_EXIT_RX, 0), 0x02, 0x1B, 0, 0, 0, 0x00, 0x02, 0, 0, 0);
    WANT(lr2021_radio_set_cad_params(b, 0, -300, LR2021_CAD_EXIT_FALLBACK, 0), 0x02, 0x1B, 0, 0, 0, 0xFF, 0x00, 0, 0,
         0);
    WANT(lr2021_radio_set_cad(b), 0x02, 0x1C);
    WANT(lr2021_radio_set_cca(b, lr2021_radio_hf_ticks(1000), LR2021_CCA_GAIN_KEEP), 0x02, 0x18, 0x00, 0x7D, 0x00,
         0xFF);
    WANT(lr2021_radio_set_cca(b, 0x123456, 0), 0x02, 0x18, 0x12, 0x34, 0x56, 0x00);
    WANT(lr2021_radio_set_default_timeouts(b, 0x010203, 0xFFFFFF), 0x02, 0x15, 0x01, 0x02, 0x03, 0xFF, 0xFF, 0xFF);
    WANT(lr2021_radio_set_reg_mode(b, LR2021_REG_SIMO), 0x01, 0x21, 0x02);
    WANT(lr2021_radio_set_reg_mode(b, LR2021_REG_LDO), 0x01, 0x21, 0x00);
    WANT(lr2021_radio_calibrate(b, LR2021_CAL_PLL | LR2021_CAL_AAF), 0x01, 0x22, 0x0C);
    WANT(lr2021_radio_calibrate(b, 0xFF), 0x01, 0x22, 0x6F); // the RFU bits 7 and 4 dropped
    WANT(lr2021_radio_set_tx_test_mode(b, LR2021_TEST_CW), 0x02, 0x0E, 0x02);
    WANT(lr2021_radio_set_tx_test_mode(b, LR2021_TEST_NORMAL), 0x02, 0x0E, 0x00);
    WANT(lr2021_radio_sel_pa(b, true), 0x02, 0x0F, 0x01);
    WANT(lr2021_radio_sel_pa(b, false), 0x02, 0x0F, 0x00);
    WANT(lr2021_radio_set_packet_type(b, LR2021_PACKET_FLRC), 0x02, 0x07, 0x05);
    WANT(lr2021_radio_get_packet_type(b), 0x02, 0x08);
    WANT(lr2021_radio_reset_rx_stats(b), 0x02, 0x0A);
    WANT(lr2021_radio_get_rx_pkt_length(b), 0x02, 0x12);
    WANT(lr2021_radio_get_rssi_inst(b), 0x02, 0x0B);
    WANT(lr2021_radio_get_cca_result(b), 0x02, 0x19);
    WANT(lr2021_radio_set_timestamp_source(b, 2, LR2021_TS_FSK_SYNC), 0x02, 0x16, 0x23);
    WANT(lr2021_radio_set_timestamp_source(b, 0, LR2021_TS_LAST_BIT_SENT), 0x02, 0x16, 0x01);
    WANT(lr2021_radio_get_timestamp(b, 1), 0x02, 0x17, 0x01);
}

static void times(void)
{
    CHECK(lr2021_radio_rtc_steps(1000000) == 32768, "1 s");
    CHECK(lr2021_radio_rtc_steps(1000) == 33, "1 ms rounds 32.768 up: %u", (unsigned)lr2021_radio_rtc_steps(1000));
    CHECK(lr2021_radio_rtc_steps(0) == 0, "0");
    // DS 6.3.5: 512 s at most; 0xFFFFFF (continuous) is never made
    CHECK(lr2021_radio_rtc_steps(1000000000ull) == 0xFFFFFE, "clamp");
    CHECK(lr2021_radio_hf_ticks(1000) == 32000, "1 ms of 32 MHz");
    CHECK(lr2021_radio_hf_ticks(1000000) == 0xFFFFFF, "hf clamp");
}

static void measurements(void)
{
    uint8_t b[16];
    WANT(lr2021_radio_get_vbat(b, true, 13), 0x01, 0x24, 0x0D);
    WANT(lr2021_radio_get_vbat(b, false, 8), 0x01, 0x24, 0x00);
    WANT(lr2021_radio_get_vbat(b, true, 7), 0x01, 0x24, 0x08);  // clamped to 8 bits
    WANT(lr2021_radio_get_vbat(b, false, 20), 0x01, 0x24, 0x05); // and to 13
    WANT(lr2021_radio_get_temp(b, LR2021_TEMP_NTC, true, 13), 0x01, 0x25, 0x2D);
    WANT(lr2021_radio_get_temp(b, LR2021_TEMP_XOSC, false, 10), 0x01, 0x25, 0x12);
    WANT(lr2021_radio_get_random(b, LR2021_RNG_PLL | LR2021_RNG_ADC), 0x01, 0x26, 0x03);
    // enable in bit 0, the trim in bits 3:1, as the ROM reads it: the default 1.88 V is trim 4
    WANT(lr2021_radio_set_lbd(b, true, 4), 0x01, 0x30, 0x09);
    WANT(lr2021_radio_set_lbd(b, false, 7), 0x01, 0x30, 0x0E);

    // Vbat in mV, and raw by DS 6.5.1's formula: raw 4096 is (2.5 - 1) x 1.35 V
    const uint8_t mv[] = {0x0C, 0xE4};
    CHECK(near(lr2021_radio_vbat_v(mv, true), 3.300, 1e-9), "3300 mV");
    const uint8_t raw[] = {0x10, 0x00};
    CHECK(near(lr2021_radio_vbat_v(raw, false), 2.025, 1e-9), "raw vbat %f", lr2021_radio_vbat_v(raw, false));
    const uint8_t high[] = {0xFF, 0xFF};
    CHECK(lr2021_radio_meas_raw(high) == 0x1FFF, "13 bits");

    // degrees C: Temp / 32 in bits 15:3 (DS 6.5.2), signed
    const uint8_t t25[] = {0x19, 0x00}, tneg[] = {0xF6, 0x00}, thalf[] = {0x03, 0x28};
    CHECK(near(lr2021_radio_temp_c(t25, true), 25.0, 1e-9), "25 C %f", lr2021_radio_temp_c(t25, true));
    CHECK(near(lr2021_radio_temp_c(tneg, true), -10.0, 1e-9), "-10 C %f", lr2021_radio_temp_c(tneg, true));
    CHECK(near(lr2021_radio_temp_c(thalf, true), 3.15625, 1e-9), "%f", lr2021_radio_temp_c(thalf, true));
    // raw: Vbe = Vbe25 (0.7295 V of 1.35 V, 4427 of 8192) is 25 C; 1.7 mV lower is 26 C
    const uint8_t r25[] = {0x11, 0x4B};
    CHECK(near(lr2021_radio_temp_c(r25, false), 25.0, 0.1), "raw 25 C %f", lr2021_radio_temp_c(r25, false));
    const uint16_t r26 = (uint16_t)lround((0.7295 - 0.0017) / 1.35 * 8192);
    const uint8_t a26[] = {(uint8_t)(r26 >> 8), (uint8_t)r26};
    CHECK(near(lr2021_radio_temp_c(a26, false), 26.0, 0.15), "raw 26 C %f", lr2021_radio_temp_c(a26, false));
    // NTC: at half scale with a bias ratio of 1 the NTC is at its 25 C value
    CHECK(near(lr2021_radio_ntc_temp_c(4096, 1.0, 3380), 25.0, 1e-9), "ntc 25");
    CHECK(lr2021_radio_ntc_temp_c(3000, 1.0, 3380) > 25.0, "ntc lower R is warmer");

    const uint8_t rnd[] = {0xDE, 0xAD, 0xBE, 0xEF};
    CHECK(lr2021_radio_random(rnd) == 0xDEADBEEF, "random");
}

static void status(void)
{
    // DS table 6-40: an LR2021 answers 0x01 0x18
    const uint8_t ver[] = {0x01, 0x18};
    CHECK(lr2021_radio_version(ver) == (LR2021_VERSION_MAJOR << 8 | LR2021_VERSION_MINOR), "version");
    const uint8_t err[] = {0x02, 0x04};
    CHECK(lr2021_radio_errors(err) == (LR2021_ERR_RXFREQ_NO_FE_CAL | LR2021_ERR_PLL_LOCK), "errors");
    const uint8_t irq[] = {0x00, 0x08, 0x00, 0x01};
    CHECK(lr2021_radio_irq(irq) == (LR2021_IRQ_TX_DONE | LR2021_IRQ_RX_FIFO), "irq");
    const uint8_t ff[] = {LR2021_FIFO_HIGH | LR2021_FIFO_OVERFLOW, LR2021_FIFO_EMPTY};
    lr2021_fifo_flags_t f = lr2021_radio_fifo_flags(ff);
    CHECK(f.rx == 0x14 && f.tx == 0x01, "fifo flags");
    const uint8_t lvl[] = {0x03, 0xE8};
    CHECK(lr2021_radio_fifo_level(lvl) == 1000, "fifo level");

    // the IRQ table: 32 bits, each defined once (the three in lr2021_cmd.h included)
    const uint32_t all[] = {
        LR2021_IRQ_RX_FIFO,           LR2021_IRQ_TX_FIFO,           LR2021_IRQ_RNG_REQ_VALID,
        LR2021_IRQ_TX_TIMESTAMP,      LR2021_IRQ_RX_TIMESTAMP,      LR2021_IRQ_PREAMBLE_DETECTED,
        LR2021_IRQ_HEADER_VALID,      LR2021_IRQ_CAD_DETECTED,      LR2021_IRQ_LORA_HDR_TIMESTAMP,
        LR2021_IRQ_LORA_HEADER_ERR,   LR2021_IRQ_LOW_BATTERY,       LR2021_IRQ_PA_OCP_OVP,
        LR2021_IRQ_LORA_TX_RX_HOP,    LR2021_IRQ_SYNC_FAIL,         LR2021_IRQ_LORA_SYMBOL_END,
        LR2021_IRQ_LORA_TIMESTAMP_STAT, LR2021_IRQ_ERROR,           LR2021_IRQ_CMD_ERROR,
        LR2021_IRQ_RX_DONE,           LR2021_IRQ_TX_DONE,           LR2021_IRQ_CAD_DONE,
        LR2021_IRQ_TIMEOUT,           LR2021_IRQ_CRC_ERROR,         LR2021_IRQ_LEN_ERROR,
        LR2021_IRQ_ADDR_ERROR,        LR2021_IRQ_FHSS,              LR2021_IRQ_INTER_PACKET1,
        LR2021_IRQ_INTER_PACKET2,     LR2021_IRQ_RNG_RESP_DONE,     LR2021_IRQ_RNG_REQ_DISCARDED,
        LR2021_IRQ_RNG_EXCH_VALID,    LR2021_IRQ_RNG_TIMEOUT,
    };
    uint64_t sum = 0;
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++)
        sum += all[i];
    CHECK(sizeof all / sizeof all[0] == 32 && sum == LR2021_IRQ_ALL, "irq table");
    CHECK(all[18] == 1u << 18 && all[31] == 1u << 31, "irq order");
    CHECK(!strcmp(lr2021_radio_irq_name(19), "TX done") && !strcmp(lr2021_radio_irq_name(21), "timeout") &&
              !strcmp(lr2021_radio_irq_name(32), "?") && !strcmp(lr2021_radio_irq_name(-1), "?"),
          "irq names");
    for (int i = 0; i < 32; i++)
        CHECK(strcmp(lr2021_radio_irq_name(i), "?") != 0, "irq %d unnamed", i);
}

static void clocks(void)
{
    uint8_t b[16];
    WANT(lr2021_radio_config_lf_clock(b, LR2021_LF_CLOCK_DIO11), 0x01, 0x18, 0x02);
    WANT(lr2021_radio_config_clk_outputs(b, 0x0F), 0x01, 0x19, 0x0F);
    WANT(lr2021_radio_config_clk_outputs(b, 0x13), 0x01, 0x19, 0x03);
    // DS tables 6-67 and 6-68: code 5 is 13.6 pF on XTA and 13.45 pF on XTB; 47 is the most
    CHECK(lr2021_radio_xosc_trim_code(13.6, false) == 5 && lr2021_radio_xosc_trim_code(13.45, true) == 5, "trim 5");
    CHECK(lr2021_radio_xosc_trim_code(33.4, false) == 47 && lr2021_radio_xosc_trim_code(40, true) == 47, "trim max");
    CHECK(lr2021_radio_xosc_trim_code(5, false) == 0, "trim min");
    WANT(lr2021_radio_set_xosc_trim(b, 5, 60, 100), 0x01, 0x31, 0x05, 0x2F, 0x64);
    WANT(lr2021_radio_set_temp_comp(b, LR2021_TEMP_COMP_ABSOLUTE, true), 0x01, 0x32, 0x06);
    WANT(lr2021_radio_set_temp_comp(b, LR2021_TEMP_COMP_RELATIVE, false), 0x01, 0x32, 0x01);
    // ratio 1.0 in 10.9 is 512; beta 3380 K in 2 K steps is 1690
    WANT(lr2021_radio_set_ntc_params(b, 1.0, 3380, 0x10), 0x01, 0x33, 0x02, 0x00, 0x06, 0x9A, 0x10);
    WANT(lr2021_radio_set_ntc_params(b, 3.0, 1e6, 0), 0x01, 0x33, 0x03, 0xFF, 0x0F, 0xFF, 0x00);
    WANT(lr2021_radio_set_ntc_params(b, -1, -1, 0), 0x01, 0x33, 0, 0, 0, 0, 0);
}

static void radio(void)
{
    // DS 7.3.3: RSSI = -Rssi / 2, Rssi(8:1) in byte 0, Rssi(0) in byte 1 bit 0
    const uint8_t r80[] = {0x50, 0x00}, r_half[] = {0xF0, 0x01}, r_rfu[] = {0x50, 0xFE};
    CHECK(lr2021_radio_rssi_dbm(r80) == -80.0, "-80 dBm");
    CHECK(lr2021_radio_rssi_dbm(r_half) == -240.5, "-240.5 dBm %f", lr2021_radio_rssi_dbm(r_half));
    CHECK(lr2021_radio_rssi_dbm(r_rfu) == -80.0, "rfu bits ignored");

    // DS 7.3.8: three bytes of bits 8:1, then bit 0 of min, max, average in bits 2, 1, 0
    const uint8_t cca[] = {0x64, 0x50, 0x5A, 0x05};
    lr2021_cca_t c = lr2021_radio_cca(cca, 4);
    CHECK(c.min_dbm == -100.5 && c.max_dbm == -80.0 && c.avg_dbm == -90.5, "cca %f %f %f", c.min_dbm, c.max_dbm,
          c.avg_dbm);
    c = lr2021_radio_cca(cca, 3);
    CHECK(c.min_dbm == -100.0 && c.max_dbm == -80.0 && c.avg_dbm == -90.0, "cca whole dB");

    const uint8_t ts[] = {0x00, 0x00, 0x7D, 0x00};
    CHECK(lr2021_radio_timestamp_ticks(ts) == 32000 && lr2021_radio_timestamp_us(ts) == 1000.0, "timestamp");
    const uint8_t len[] = {0x01, 0x02};
    CHECK(lr2021_radio_rx_pkt_length(len) == 258, "pkt length");
    const uint8_t pt[] = {LR2021_PACKET_OQPSK};
    CHECK(lr2021_radio_packet_type(pt) == 0xD, "packet type");
}

static void rssi_cal(void)
{
    uint8_t b[LR2021_RADIO_FRAME_MAX + 4];
    memset(b, 0xAA, sizeof b);
    // LF only: the path byte, then 27 entries of rfu | gain(9:8), gain(7:0), nf
    size_t n = lr2021_radio_set_rssi_cal(b, &lr2021_rssi_cal_lf_915, NULL);
    CHECK(n == 84, "lf length %zu", n);
    CHECK(b[0] == 0x02 && b[1] == 0x05 && b[2] == 0x01, "lf head");
    CHECK(b[3] == 0x03 && b[4] == 0xE0 && b[5] == 219, "G1 -32: %02X %02X %02X", b[3], b[4], b[5]); // -32 in 10 bits
    CHECK(b[9] == 0x00 && b[10] == 16 && b[11] == 194, "G3");
    CHECK(b[81] == 0x00 && b[82] == 232 && b[83] == 9, "G13 boost 7");
    CHECK(b[84] == 0xAA, "wrote past the end");
    // both: LF first (bytes 3 to 83), then HF (84 to 164)
    n = lr2021_radio_set_rssi_cal(b, &lr2021_rssi_cal_lf_490, &lr2021_rssi_cal_hf_2g4);
    CHECK(n == LR2021_RADIO_FRAME_MAX, "both length %zu", n);
    CHECK(b[2] == 0x03, "both paths");
    CHECK(b[3] == 0x03 && b[4] == 0xD2 && b[5] == 221, "490 G1 -46");
    CHECK(b[84] == 0x03 && b[85] == 0xC6 && b[86] == 240, "HF G1 -58");
    CHECK(b[164] == 24 && b[165] == 0xAA, "HF end");
    n = lr2021_radio_set_rssi_cal(b, NULL, &lr2021_rssi_cal_hf_2g4);
    CHECK(n == 84 && b[2] == 0x02 && b[3] == 0x03 && b[4] == 0xC6, "hf only");
    n = lr2021_radio_set_rssi_cal(b, NULL, NULL);
    CHECK(n == 3 && b[2] == 0x00, "none");
    // a gain over 255 keeps its bits 9:8
    lr2021_rssi_cal_t big = lr2021_rssi_cal_lf_915;
    big.gain[0] = 300;
    lr2021_radio_set_rssi_cal(b, &big, NULL);
    CHECK(b[3] == 0x01 && b[4] == 0x2C, "gain 300");
    // the tables: gains rise in each run (G1 to G11, then each boost run), as the datasheet's do apart from its
    // G12 B1 on 490 MHz and G12 B7 on 2.4 GHz
    CHECK(lr2021_rssi_cal_lf_915.gain[10] == 173 && lr2021_rssi_cal_lf_490.gain[26] == 242 &&
              lr2021_rssi_cal_hf_2g4.nf[0] == 240,
          "table values");
    for (int i = 1; i < 11; i++)
        CHECK(lr2021_rssi_cal_lf_915.gain[i] > lr2021_rssi_cal_lf_915.gain[i - 1] &&
                  lr2021_rssi_cal_lf_490.gain[i] > lr2021_rssi_cal_lf_490.gain[i - 1] &&
                  lr2021_rssi_cal_hf_2g4.gain[i] > lr2021_rssi_cal_hf_2g4.gain[i - 1],
              "gain %d not rising", i);
}

static void regs(void)
{
    uint8_t b[LR2021_RADIO_FRAME_MAX];
    const uint32_t w[] = {LR2021_RX_FIFO_BASE_1K, LR2021_RX_FIFO_SIZE_1K};
    WANT(lr2021_radio_write_regs(b, LR2021_REG_RX_FIFO_BASE, w, 2), 0x01, 0x04, 0xF3, 0x00, 0x28, 0x00, 0x80, 0x44,
         0x00, 0x00, 0x00, 0x03, 0xE8);
    // one word is what lr2021_cmd_write_reg32 writes
    uint8_t one[16];
    size_t n1 = lr2021_cmd_write_reg32(one, LR2021_REG_PLL_FREQ, 0x39806984);
    const uint32_t pll = 0x39806984;
    size_t n2 = lr2021_radio_write_regs(b, LR2021_REG_PLL_FREQ, &pll, 1);
    CHECK(n1 == n2 && !memcmp(one, b, n1), "one word");
    uint32_t many[33] = {0};
    CHECK(lr2021_radio_write_regs(b, 0, many, 32) == 5 + 128, "32 words");
    CHECK(lr2021_radio_write_regs(b, 0, many, 33) == 0 && lr2021_radio_write_regs(b, 0, many, 0) == 0 &&
              lr2021_radio_write_regs(b, 0, NULL, 1) == 0,
          "refused counts");
    WANT(lr2021_radio_write_reg_mask(b, 0xF30154, 0x000F0000, 0x00050000), 0x01, 0x05, 0xF3, 0x01, 0x54, 0x00, 0x0F,
         0x00, 0x00, 0x00, 0x05, 0x00, 0x00);
    const uint8_t ans[] = {0x00, 0x80, 0x44, 0x00, 0x00, 0x00, 0x03, 0xE8};
    uint32_t out[2];
    lr2021_radio_words(ans, 2, out);
    CHECK(out[0] == LR2021_RX_FIFO_BASE_1K && out[1] == LR2021_RX_FIFO_SIZE_1K, "words");
}

int main(void)
{
    modes();
    times();
    measurements();
    status();
    clocks();
    radio();
    rssi_cal();
    regs();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
