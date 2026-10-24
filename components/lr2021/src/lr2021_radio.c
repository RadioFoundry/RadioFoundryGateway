// SPDX-License-Identifier: MIT AND BSD-3-Clause-Clear
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * LR2021 system and common radio command encodings (lr2021_radio.h); no I/O here.
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
#include "lr2021_radio.h"

#include <math.h>

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

static size_t put24(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)(v >> 16);
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)v;
    return 3;
}

static size_t put32(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)(v >> 24);
    return 1 + put24(b + 1, v);
}

static uint16_t get16(const uint8_t *a) { return (uint16_t)(a[0] << 8 | a[1]); }
static uint32_t get32(const uint8_t *a) { return (uint32_t)a[0] << 24 | (uint32_t)a[1] << 16 | (uint32_t)a[2] << 8 | a[3]; }

static size_t op_u8(uint8_t *b, uint16_t op, uint8_t a)
{
    put_op(b, op);
    b[2] = a;
    return 3;
}

/* A resolution in bits (8 to 13) as GetVbat / GetTemp's code 0 to 5. */
static uint8_t res_code(int bits) { return (uint8_t)(bits < 8 ? 0 : bits > 13 ? 5 : bits - 8); }

const char *lr2021_radio_irq_name(int bit)
{
    static const char *const names[32] = {
        "RX FIFO", "TX FIFO", "ranging request", "TX timestamp", "RX timestamp", "preamble", "header / sync",
        "CAD detected", "LoRa header timestamp", "LoRa header error", "low battery", "PA OCP/OVP", "LoRa hop",
        "sync fail", "LoRa symbol end", "LoRa timestamp stats", "error", "command error", "RX done", "TX done",
        "CAD done", "timeout", "CRC error", "length error", "address error", "FHSS hop", "inter-packet 1",
        "inter-packet 2", "ranging response sent", "ranging request discarded", "ranging exchange valid",
        "ranging timeout",
    };
    return bit >= 0 && bit < 32 ? names[bit] : "?";
}

/* ── Time units ── */

uint32_t lr2021_radio_rtc_steps(uint64_t us)
{
    const uint64_t s = (us * 32768u + 500000u) / 1000000u;
    return s > 0xFFFFFEu ? 0xFFFFFEu : (uint32_t)s;
}

uint32_t lr2021_radio_hf_ticks(uint32_t us)
{
    const uint64_t t = (uint64_t)us * 32u;
    return t > 0xFFFFFFu ? 0xFFFFFFu : (uint32_t)t;
}

/* ── Chip modes ── */

size_t lr2021_radio_set_sleep(uint8_t *b, bool clk_32k, bool retain, uint32_t sleep_time)
{
    put_op(b, LR2021_SET_SLEEP);
    b[2] = (uint8_t)((retain ? 0x02 : 0) | (clk_32k ? 0x01 : 0));
    return 3 + put32(b + 3, sleep_time);
}

size_t lr2021_radio_set_standby(uint8_t *b, bool xosc) { return op_u8(b, LR2021_SET_STANDBY, xosc ? 1 : 0); }

size_t lr2021_radio_set_fs(uint8_t *b) { return put_op(b, LR2021_SET_FS); }

size_t lr2021_radio_add_retained_reg(uint8_t *b, uint8_t slot, uint32_t addr)
{
    put_op(b, LR2021_SET_ADDITIONAL_REG_TO_RETAIN);
    b[2] = slot;
    return 3 + put24(b + 3, addr);
}

size_t lr2021_radio_set_rx_default(uint8_t *b) { return put_op(b, LR2021_SET_RX); }

size_t lr2021_radio_set_tx_default(uint8_t *b) { return put_op(b, LR2021_SET_TX); }

size_t lr2021_radio_set_fallback(uint8_t *b, uint8_t mode) { return op_u8(b, LR2021_SET_FALLBACK_MODE, mode & 0xF); }

size_t lr2021_radio_set_rx_duty_cycle(uint8_t *b, uint32_t rx_max_time, uint32_t cycle_time, uint8_t mode)
{
    size_t o = put_op(b, LR2021_SET_RX_DUTY_CYCLE);
    o += put24(b + o, rx_max_time);
    o += put24(b + o, cycle_time);
    b[o++] = (uint8_t)((mode & 0xF) << 4); /* mode in bits 7:4, bits 3:0 RFU */
    return o;
}

size_t lr2021_radio_set_auto_rx_tx(uint8_t *b, bool clear, uint8_t mode, uint32_t timeout, uint32_t delay)
{
    size_t o = put_op(b, LR2021_SET_AUTO_RX_TX);
    b[o++] = (uint8_t)((clear ? 0x80 : 0) | (mode & 0x7F));
    o += put24(b + o, timeout);
    return o + put32(b + o, delay);
}

size_t lr2021_radio_stop_timeout_on_preamble(uint8_t *b, bool on_preamble)
{
    return op_u8(b, LR2021_STOP_TIMEOUT_ON_PREAMBLE, on_preamble ? 1 : 0);
}

size_t lr2021_radio_set_cad_params(uint8_t *b, uint32_t cad_timeout, int threshold_dbm, uint8_t exit,
                                   uint32_t trx_timeout)
{
    size_t o = put_op(b, LR2021_SET_CAD_PARAMS);
    o += put24(b + o, cad_timeout);
    b[o++] = (uint8_t)(threshold_dbm > 0 ? 0 : threshold_dbm < -255 ? 255 : -threshold_dbm);
    b[o++] = exit;
    return o + put24(b + o, trx_timeout);
}

size_t lr2021_radio_set_cad(uint8_t *b) { return put_op(b, LR2021_SET_CAD); }

size_t lr2021_radio_set_cca(uint8_t *b, uint32_t duration, uint8_t gain_step)
{
    put_op(b, LR2021_SET_CCA);
    put24(b + 2, duration);
    b[5] = gain_step;
    return 6;
}

size_t lr2021_radio_set_default_timeouts(uint8_t *b, uint32_t rx, uint32_t tx)
{
    size_t o = put_op(b, LR2021_SET_DEFAULT_RX_TX_TIMEOUT);
    o += put24(b + o, rx);
    return o + put24(b + o, tx);
}

size_t lr2021_radio_set_reg_mode(uint8_t *b, uint8_t mode) { return op_u8(b, LR2021_SET_REG_MODE, mode); }

size_t lr2021_radio_calibrate(uint8_t *b, uint8_t blocks) { return op_u8(b, LR2021_CALIBRATE, blocks & LR2021_CAL_ALL); }

size_t lr2021_radio_set_tx_test_mode(uint8_t *b, uint8_t mode) { return op_u8(b, LR2021_SET_TX_TEST_MODE, mode); }

/* ── Measurements ── */

size_t lr2021_radio_get_vbat(uint8_t *b, bool mv, int bits)
{
    return op_u8(b, LR2021_GET_VBAT, (uint8_t)((mv ? 0x08 : 0) | res_code(bits)));
}

uint16_t lr2021_radio_meas_raw(const uint8_t *a) { return (uint16_t)((a[0] & 0x1F) << 8 | a[1]); }

double lr2021_radio_vbat_v(const uint8_t *a, bool mv)
{
    const uint16_t v = lr2021_radio_meas_raw(a);
    return mv ? v / 1000.0 : (5.0 * v / 8192.0 - 1.0) * 1.35;
}

size_t lr2021_radio_get_temp(uint8_t *b, uint8_t source, bool celsius, int bits)
{
    return op_u8(b, LR2021_GET_TEMP, (uint8_t)((source & 0x3) << 4 | (celsius ? 0x08 : 0) | res_code(bits)));
}

double lr2021_radio_temp_c(const uint8_t *a, bool celsius)
{
    if (celsius) {
        int v = a[0] << 5 | a[1] >> 3; /* Temp(12:5) in byte 0, Temp(4:0) in byte 1 bits 7:3 */
        if (v & 0x1000)
            v -= 0x2000;
        return v / 32.0;
    }
    return (lr2021_radio_meas_raw(a) / 8192.0 * 1.35 - 0.7295) * 1000.0 / -1.7 + 25.0;
}

double lr2021_radio_ntc_temp_c(uint16_t raw, double r_bias_ratio, double beta_k)
{
    const double x = raw / 8192.0;
    const double r_ratio = r_bias_ratio * x / (1.0 - x);
    return beta_k / (log(r_ratio) + beta_k / 298.15) - 273.15;
}

size_t lr2021_radio_set_lbd(uint8_t *b, bool enable, uint8_t trim)
{
    return op_u8(b, LR2021_SET_LBD_CFG, (uint8_t)((trim & 0x7) << 1 | (enable ? 1 : 0)));
}

size_t lr2021_radio_get_random(uint8_t *b, uint8_t sources) { return op_u8(b, LR2021_GET_RANDOM_NUMBER, sources & 0x3); }

uint32_t lr2021_radio_random(const uint8_t *a) { return get32(a); }

/* ── Status, IRQs, FIFOs ── */

uint16_t lr2021_radio_version(const uint8_t *a) { return get16(a); }

uint16_t lr2021_radio_errors(const uint8_t *a) { return get16(a); }

uint32_t lr2021_radio_irq(const uint8_t *a) { return get32(a); }

lr2021_fifo_flags_t lr2021_radio_fifo_flags(const uint8_t *a)
{
    lr2021_fifo_flags_t f = {a[0], a[1]};
    return f;
}

uint16_t lr2021_radio_fifo_level(const uint8_t *a) { return get16(a); }

/* ── Clocks and temperature compensation ── */

size_t lr2021_radio_config_lf_clock(uint8_t *b, uint8_t source) { return op_u8(b, LR2021_CONFIG_LF_CLOCK, source & 0x3); }

size_t lr2021_radio_config_clk_outputs(uint8_t *b, uint8_t scaling)
{
    return op_u8(b, LR2021_CONFIG_CLK_OUTPUTS, scaling & 0xF);
}

size_t lr2021_radio_set_xosc_trim(uint8_t *b, uint8_t xta, uint8_t xtb, uint8_t start_us)
{
    put_op(b, LR2021_SET_XOSC_CP_TRIM);
    b[2] = xta > 47 ? 47 : xta;
    b[3] = xtb > 47 ? 47 : xtb;
    b[4] = start_us;
    return 5;
}

uint8_t lr2021_radio_xosc_trim_code(double pf, bool xtb)
{
    const double c = round((pf - (xtb ? 11.1 : 11.3)) / 0.47);
    return (uint8_t)(c < 0 ? 0 : c > 47 ? 47 : c);
}

size_t lr2021_radio_set_temp_comp(uint8_t *b, uint8_t mode, bool ntc)
{
    return op_u8(b, LR2021_SET_TEMP_COMP_CFG, (uint8_t)((ntc ? 0x04 : 0) | (mode & 0x3)));
}

size_t lr2021_radio_set_ntc_params(uint8_t *b, double r_ratio, double beta_k, uint8_t delay)
{
    const double r = round(r_ratio * 512.0), beta = round(beta_k / 2.0);
    put_op(b, LR2021_SET_NTC_PARAMS);
    put16(b + 2, (uint16_t)(r < 0 ? 0 : r > 1023 ? 1023 : r));          /* 10 bits, 9 fractional */
    put16(b + 4, (uint16_t)(beta < 0 ? 0 : beta > 4095 ? 4095 : beta)); /* 12 bits of 2 K */
    b[6] = delay;
    return 7;
}

/* ── Common radio ── */

size_t lr2021_radio_get_rssi_inst(uint8_t *b) { return put_op(b, LR2021_GET_RSSI_INST); }

double lr2021_radio_rssi_dbm(const uint8_t *a) { return -((a[0] << 1) | (a[1] & 1)) / 2.0; }

static size_t put_cal(uint8_t *b, const lr2021_rssi_cal_t *t)
{
    size_t o = 0;
    for (int i = 0; i < LR2021_RSSI_CAL_ENTRIES; i++) {
        o += put16(b + o, (uint16_t)((uint16_t)t->gain[i] & 0x3FF)); /* rfu(5:0), gain(9:8); gain(7:0) */
        b[o++] = t->nf[i];
    }
    return o;
}

size_t lr2021_radio_set_rssi_cal(uint8_t *b, const lr2021_rssi_cal_t *lf, const lr2021_rssi_cal_t *hf)
{
    size_t o = put_op(b, LR2021_SET_RSSI_CALIBRATION);
    b[o++] = (uint8_t)((hf ? 0x02 : 0) | (lf ? 0x01 : 0));
    if (lf)
        o += put_cal(b + o, lf);
    if (hf)
        o += put_cal(b + o, hf);
    return o;
}

/* Tables 7-6 to 7-8. The datasheet prints every gain without a sign; the lowest steps' are negative here, since the
 * gains fall and then rise in 6 dB (24-count) steps otherwise (LF 915: -32, -8, 16, 36, ...). */
const lr2021_rssi_cal_t lr2021_rssi_cal_hf_2g4 = {
    {-58, -34, -10, 7, 25, 40, 53, 77, 102, 126, 146, 163, 166, 168, 170, 171, 172, 176, 175, 187, 190, 193, 194, 195,
     196, 198, 199},
    {240, 221, 208, 190, 171, 153, 137, 113, 89, 65, 47, 34, 32, 31, 30, 30, 29, 26, 27, 29, 29, 27, 26, 26, 25, 24, 24},
};
const lr2021_rssi_cal_t lr2021_rssi_cal_lf_915 = {
    {-32, -8, 16, 36, 56, 73, 90, 112, 130, 152, 173, 190, 196, 200, 202, 203, 205, 207, 208, 214, 220, 223, 226, 227,
     228, 230, 232},
    {219, 204, 194, 175, 156, 137, 122, 95, 62, 41, 26, 16, 13, 13, 13, 12, 12, 12, 12, 12, 11, 11, 10, 10, 11, 10, 9},
};
const lr2021_rssi_cal_t lr2021_rssi_cal_lf_490 = {
    {-46, -22, 2, 24, 44, 63, 84, 104, 138, 158, 181, 200, 196, 208, 210, 211, 215, 217, 218, 224, 228, 231, 234, 237,
     238, 240, 242},
    {221, 205, 190, 169, 147, 130, 113, 87, 46, 30, 18, 13, 13, 13, 13, 13, 12, 12, 12, 12, 12, 12, 12, 11, 11, 10, 10},
};

size_t lr2021_radio_reset_rx_stats(uint8_t *b) { return put_op(b, LR2021_RESET_RX_STATS); }

size_t lr2021_radio_set_timestamp_source(uint8_t *b, uint8_t index, uint8_t source)
{
    return op_u8(b, LR2021_SET_TIMESTAMP_SOURCE, (uint8_t)((index & 0x3) << 4 | (source & 0xF)));
}

size_t lr2021_radio_get_timestamp(uint8_t *b, uint8_t index) { return op_u8(b, LR2021_GET_TIMESTAMP_VALUE, index & 0x3); }

uint32_t lr2021_radio_timestamp_ticks(const uint8_t *a) { return get32(a); }

double lr2021_radio_timestamp_us(const uint8_t *a) { return get32(a) / 32.0; }

size_t lr2021_radio_get_cca_result(uint8_t *b) { return put_op(b, LR2021_GET_CCA_RESULT); }

lr2021_cca_t lr2021_radio_cca(const uint8_t *a, size_t n)
{
    /* bytes 0-2: min, max, average, bits 8:1; byte 3: their bit 0 in bits 2, 1, 0 (as the ROM writes them) */
    const uint8_t lsb = n >= 4 ? a[3] : 0;
    lr2021_cca_t c;
    c.min_dbm = -((a[0] << 1) | (lsb >> 2 & 1)) / 2.0;
    c.max_dbm = -((a[1] << 1) | (lsb >> 1 & 1)) / 2.0;
    c.avg_dbm = -((a[2] << 1) | (lsb & 1)) / 2.0;
    return c;
}

size_t lr2021_radio_get_rx_pkt_length(uint8_t *b) { return put_op(b, LR2021_GET_RX_PKT_LENGTH); }

uint16_t lr2021_radio_rx_pkt_length(const uint8_t *a) { return get16(a); }

size_t lr2021_radio_sel_pa(uint8_t *b, bool hf) { return op_u8(b, LR2021_SEL_PA, hf ? 1 : 0); }

size_t lr2021_radio_set_packet_type(uint8_t *b, uint8_t type) { return op_u8(b, LR2021_SET_PACKET_TYPE, type); }

size_t lr2021_radio_get_packet_type(uint8_t *b) { return put_op(b, LR2021_GET_PACKET_TYPE); }

uint8_t lr2021_radio_packet_type(const uint8_t *a) { return a[0]; }

/* ── Register blocks ── */

size_t lr2021_radio_write_regs(uint8_t *b, uint32_t addr, const uint32_t *words, int n)
{
    if (n < 1 || n > 32 || !words)
        return 0;
    size_t o = put_op(b, LR2021_WRITE_REG_MEM32);
    o += put24(b + o, addr);
    for (int i = 0; i < n; i++)
        o += put32(b + o, words[i]);
    return o;
}

size_t lr2021_radio_write_reg_mask(uint8_t *b, uint32_t addr, uint32_t mask, uint32_t value)
{
    size_t o = put_op(b, LR2021_WRITE_REG_MEM_MASK32);
    o += put24(b + o, addr);
    o += put32(b + o, mask);
    return o + put32(b + o, value);
}

void lr2021_radio_words(const uint8_t *a, int n, uint32_t *out)
{
    for (int i = 0; i < n; i++)
        out[i] = get32(a + 4 * i);
}
