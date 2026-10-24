// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
// Host tests for the LR2021 command encodings (lr2021_cmd.h), against the byte layouts in the datasheet's
// tables (DS.LR20xx Rev. 2.2) and its worked example for CalibFE.
#include <stdio.h>
#include <string.h>

#include "lr2021_cmd.h"

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
}

#define FRAME(call, ...)                                         \
    do {                                                         \
        uint8_t b_[16];                                          \
        static const uint8_t w_[] = {__VA_ARGS__};               \
        size_t n_ = call;                                        \
        frame(b_, n_, w_, sizeof w_, #call);                     \
    } while (0)

int main(void)
{
    // the status word (table 6-38): command status in 11:9, IRQ in 8, reset source in 7:4, mode in 2:0
    lr2021_status_t s = lr2021_status(0x05, 0x24);
    CHECK(s.cmd == LR2021_CMD_OK && s.irq && s.reset == 2 && s.mode == LR2021_MODE_RX, "status %d %d %d %d", s.cmd,
          s.irq, s.reset, s.mode);
    s = lr2021_status(0x06, 0x01);
    CHECK(s.cmd == LR2021_CMD_DATA && !s.irq && s.mode == LR2021_MODE_STBY_RC, "status data");
    CHECK(!strcmp(lr2021_mode_name(5), "TX") && !strcmp(lr2021_mode_name(9), "?"), "mode names");
    CHECK(!strcmp(lr2021_error_name(LR2021_ERR_PLL_LOCK | LR2021_ERR_IMG_CALIB), "PLL didn't lock") &&
              !strcmp(lr2021_error_name(0), ""),
          "error names");

    // frames: opcode big-endian, then the arguments
    FRAME(lr2021_cmd(b_, LR2021_GET_VERSION), 0x01, 0x01);
    FRAME(lr2021_cmd_u8(b_, LR2021_SET_STANDBY, 0), 0x01, 0x28, 0x00);
    FRAME(lr2021_cmd_u8(b_, LR2021_CALIBRATE, LR2021_CAL_PLL | LR2021_CAL_AAF), 0x01, 0x22, 0x0C);
    FRAME(lr2021_cmd_set_rf_frequency(b_, 461137500), 0x02, 0x00, 0x1B, 0x7C, 0x66, 0x5C);
    FRAME(lr2021_cmd_set_rx_path(b_, false, 7), 0x02, 0x01, 0x00, 0x07);
    FRAME(lr2021_cmd_set_rx_path(b_, true, 4), 0x02, 0x01, 0x01, 0x04);
    FRAME(lr2021_cmd_set_tcxo(b_, 7, 0x8000), 0x01, 0x20, 0x07, 0x00, 0x00, 0x80, 0x00);
    FRAME(lr2021_cmd_set_dio_function(b_, 9, LR2021_DIO_RF_SWITCH, 0), 0x01, 0x12, 0x09, 0x20);
    FRAME(lr2021_cmd_set_dio_rf_switch(b_, 10, LR2021_SW_RX_LF | LR2021_SW_TX_LF), 0x01, 0x13, 0x0A, 0x06);
    FRAME(lr2021_cmd_set_dio_irq(b_, 7, 0x00000001), 0x01, 0x15, 0x07, 0x00, 0x00, 0x00, 0x01);
    FRAME(lr2021_cmd_clear_fifo_flags(b_, 0x3F, 0x3F), 0x01, 0x14, 0x3F, 0x3F);
    FRAME(lr2021_cmd_clear_irq(b_, LR2021_IRQ_RX_FIFO), 0x01, 0x16, 0x00, 0x00, 0x00, 0x01);
    FRAME(lr2021_cmd_config_fifo_irq(b_, LR2021_FIFO_HIGH, 0, 512, 0, 0, 0), 0x01, 0x1A, 0x04, 0x00, 0x02, 0x00, 0x00,
          0x00, 0x00, 0x00, 0x00, 0x00);
    FRAME(lr2021_cmd_set_rx(b_, 0xFFFFFF), 0x02, 0x0C, 0xFF, 0xFF, 0xFF);
    FRAME(lr2021_cmd_set_tx(b_, 0x1234567), 0x02, 0x0D, 0x23, 0x45, 0x67);
    FRAME(lr2021_cmd_set_gain(b_, 12), 0x02, 0x1A, 0x0C);
    FRAME(lr2021_cmd_set_gain(b_, 20), 0x02, 0x1A, 0x0D);
    FRAME(lr2021_cmd_set_tx_params(b_, lr2021_tx_power_code(22), lr2021_ramp_code(48)), 0x02, 0x03, 0x2C, 0x05);
    FRAME(lr2021_cmd_write_reg32(b_, LR2021_REG_RX_FIFO_BASE, LR2021_RX_FIFO_BASE_1K), 0x01, 0x04, 0xF3, 0x00, 0x28,
          0x00, 0x80, 0x44, 0x00);
    FRAME(lr2021_cmd_read_reg32(b_, LR2021_REG_RX_FIFO_SIZE, 1), 0x01, 0x06, 0xF3, 0x00, 0x30, 0x01);

    // CalibFE: 4 MHz steps, bit 15 the HF path; the datasheet's example: 900 MHz is 0x00E1 on LF, 0x80E1 on HF
    CHECK(lr2021_calib_fe_word(900000000, false) == 0x00E1 && lr2021_calib_fe_word(900000000, true) == 0x80E1,
          "calib fe 900");
    CHECK(lr2021_calib_fe_word(461137500, false) == 115, "calib fe 461 (rounded down as the chip does)");
    const uint32_t f[2] = {461137500, 0};
    FRAME(lr2021_cmd_calib_fe(b_, f, 2, false), 0x01, 0x23, 0x00, 0x73, 0x00, 0x00, 0x00, 0x00);

    // power: half dB, -9.5 to +22 dBm on the LF PA; ramps from table 7-24
    CHECK(lr2021_tx_power_code(-30) == -19 && lr2021_tx_power_code(30) == 44 && lr2021_tx_power_code(10.3) == 20 &&
              lr2021_tx_power_code(-9.5) == -19 && lr2021_tx_power_dbm(13) == 6.5,
          "tx power");
    CHECK(lr2021_ramp_code(0) == 0 && lr2021_ramp_code(2) == 0 && lr2021_ramp_code(33) == 5 &&
              lr2021_ramp_code(250) == 0x11 && lr2021_ramp_code(1000) == 0x12 && lr2021_ramp_us(0x10) == 240 &&
              lr2021_ramp_us(19) == -1,
          "ramps");

    // a crystal 10 ppm fast: ask for less
    CHECK(lr2021_freq_corrected(461137500, 10) == 461132889u && lr2021_freq_corrected(461137500, 0) == 461137500u &&
              lr2021_freq_corrected(461137500, -2.5) == 461138653u,
          "ppm: %u", lr2021_freq_corrected(461137500, 10));

    // documented setups: LoRa (tables 9-1, 9-3), FSK (tables 11-3, 11-5)
    FRAME(lr2021_cmd_lora_modulation(b_, 7, LR2021_LORA_BW_31, 1, 0), 0x02, 0x20, 0x72, 0x10);
    FRAME(lr2021_cmd_lora_packet(b_, 8, 255, 2), 0x02, 0x21, 0x00, 0x08, 0xFF, 0x02);
    FRAME(lr2021_cmd_fsk_modulation(b_, 125000, 0, 226, 2048), 0x02, 0x40, 0x00, 0x01, 0xE8, 0x48, 0x00, 0xE2, 0x00,
          0x08, 0x00);
    FRAME(lr2021_cmd_fsk_packet(b_, 32, 3750), 0x02, 0x41, 0x00, 0x20, 0x00, 0x00, 0x0E, 0xA6, 0x00);
    // the raw paths (docs/spec/lr2021-iq.md 2.2, 3.1)
    FRAME(lr2021_cmd_raw_route_fifo(b_), 0x02, 0x90, 0xFF, 0, 0, 0, 0, 0x0A, 0, 0, 0, 0, 0x01);
    FRAME(lr2021_cmd_raw_tx(b_, 3750, 125000, LR2021_RAW_TX_FREQ), 0x02, 0x93, 0x0E, 0xA6, 0x00, 0x01, 0xE8, 0x48, 0x01);
    FRAME(lr2021_cmd_raw_route(b_, 0x12, LR2021_RAW_SRC_CS8), 0x02, 0x90, 0x12, 0, 0, 0, 0, 0x09, 0, 0, 0, 0, 0x01);
    /* 576 000 S/s from 800 kS/s (the ROM notes' frame) */
    FRAME(lr2021_cmd_raw_route_rate(b_, 0x21, 576000, LR2021_RAW_SRC_CONV), 0x02, 0x90, 0x21, 0x00, 0x08, 0xCA, 0x00,
          0x08, 0, 0, 0, 0, 0x01);
    /* 0.37 x rate <= 0.8 x 576 000: up to 1.245 MS/s: 1 MS/s (decimation 8, mode 4: 0x19; 1.143 MS/s, 0x30, isn't
     * in the datasheet's table and the chip refuses it) */
    CHECK(lr2021_conv_rx_bw_for(576000, 0.8) == 0x19, "converter code for 576 k: %d", lr2021_conv_rx_bw_for(576000, 0.8));
    /* 384 000: up to 830 kS/s: 800 k (0x21) */
    CHECK(lr2021_conv_rx_bw_for(384000, 0.8) == 0x21, "converter code for 384 k");
    CHECK(lr2021_conv_rx_bw_for(96000, 0.8) >= 0, "converter code for 96 k");
    CHECK(lr2021_raw_sample_bytes(LR2021_RAW_SRC_CS8) == 2 && lr2021_raw_sample_bytes(LR2021_RAW_SRC_CS16) == 4 &&
              lr2021_raw_sample_bytes(6) == 2 && !lr2021_raw_sample_bytes(7),
          "raw sample bytes");
    {
        uint32_t num, den;
        lr2021_fsk_rx_rate(0x12, &num, &den); /* decimation 16, mode 3: 666 666.7 S/s */
        CHECK(num == 2000000 && den == 3, "rx_bw 0x12's rate");
        lr2021_fsk_rx_rate(0x1C, &num, &den); /* 64, 4: 125 kS/s */
        CHECK(num == 500000 && den == 4, "rx_bw 0x1C's rate");
        lr2021_fsk_rx_rate(0xDC, &num, &den); /* the filter bits don't change it */
        CHECK(num == 500000 && den == 4, "rx_bw 0xDC's rate");
    }
    CHECK(lr2021_fsk_filter_share(0x12) == 0.37 && lr2021_fsk_filter_share(0xDC) == 0.27, "filter share");
    CHECK(lr2021_fsk_rx_bw_for(576000, 800000) == 0x12 && lr2021_fsk_rx_bw_for(672000, 800000) == 0x21 &&
              lr2021_fsk_rx_bw_for(240000, 800000) == 0x1B && lr2021_fsk_rx_bw_for(96000, 800000) == 0x24 &&
              lr2021_fsk_rx_bw_for(480000, 800000) == 0x1A &&
              lr2021_fsk_rx_bw_for(900000, 800000) == -1,
          "rx_bw for a rate");
    CHECK(lr2021_raw_rx_rate(LR2021_LORA_BW_31) == 125000 && lr2021_raw_rx_rate(LR2021_LORA_BW_62) == 250000 &&
              !lr2021_raw_rx_rate(LR2021_LORA_BW_125),
          "raw rates");
    CHECK(lr2021_raw_freq_sample(1944, 2048) == 61 && lr2021_raw_freq_sample(-1944, 2048) == -61 &&
              lr2021_raw_freq_sample(9000, 2048) == 127 && lr2021_raw_freq_sample(-9000, 2048) == -128 &&
              lr2021_raw_freq_sample(16, 2048) == 1,
          "frequency samples");

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
