// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* LR2021 command encodings (lr2021_cmd.h). From the datasheet DS.LR20xx Rev. 2.2; no I/O here. */
#include "lr2021_cmd.h"

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
    put24(b + 1, v);
    return 4;
}

lr2021_status_t lr2021_status(uint8_t hi, uint8_t lo)
{
    lr2021_status_t s;
    s.cmd = (hi >> 1) & 0x7; /* bits 11:9 of the word */
    s.irq = hi & 1;          /* bit 8 */
    s.reset = (lo >> 4) & 0xF;
    s.mode = lo & 0x7;
    return s;
}

const char *lr2021_mode_name(uint8_t mode)
{
    static const char *const names[] = {"sleep", "standby RC", "standby XOSC", "FS", "RX", "TX"};
    return mode < 6 ? names[mode] : "?";
}

const char *lr2021_error_name(uint16_t e)
{
    static const char *const names[] = {"HF XOSC didn't start", "LF XOSC didn't start", "PLL didn't lock",
                                        "LF RC calibration",    "HF RC calibration",    "PLL calibration",
                                        "AAF calibration",      "image calibration",    "chip busy",
                                        "no front-end calibration at this frequency", "measure unit calibration",
                                        "PA offset calibration"};
    for (int i = 0; i < 12; i++)
        if (e & (1u << i))
            return names[i];
    return "";
}

size_t lr2021_cmd(uint8_t *b, uint16_t op) { return put_op(b, op); }

size_t lr2021_cmd_u8(uint8_t *b, uint16_t op, uint8_t a)
{
    put_op(b, op);
    b[2] = a;
    return 3;
}

size_t lr2021_cmd_set_rf_frequency(uint8_t *b, uint32_t hz)
{
    put_op(b, LR2021_SET_RF_FREQUENCY);
    return 2 + put32(b + 2, hz);
}

size_t lr2021_cmd_set_rx_path(uint8_t *b, bool hf, uint8_t boost)
{
    put_op(b, LR2021_SET_RX_PATH);
    b[2] = hf ? 1 : 0;
    b[3] = boost & 0x7;
    return 4;
}

uint16_t lr2021_calib_fe_word(uint32_t hz, bool hf)
{
    uint32_t steps = hz / 4000000u;
    return (uint16_t)((steps & 0x7FFF) | (hf ? 0x8000 : 0));
}

size_t lr2021_cmd_calib_fe(uint8_t *b, const uint32_t *hz, int n, bool hf)
{
    size_t o = put_op(b, LR2021_CALIB_FE);
    for (int i = 0; i < 3; i++)
        o += put16(b + o, i < n && hz[i] ? lr2021_calib_fe_word(hz[i], hf) : 0);
    return o;
}

size_t lr2021_cmd_set_tcxo(uint8_t *b, uint8_t tune, uint32_t start)
{
    put_op(b, LR2021_SET_TCXO_MODE);
    b[2] = tune & 0x7;
    return 3 + put32(b + 3, start);
}

size_t lr2021_cmd_set_dio_function(uint8_t *b, uint8_t dio, uint8_t func, uint8_t pull)
{
    put_op(b, LR2021_SET_DIO_FUNCTION);
    b[2] = dio;
    b[3] = (uint8_t)((func & 0xF) << 4 | (pull & 0xF));
    return 4;
}

size_t lr2021_cmd_set_dio_rf_switch(uint8_t *b, uint8_t dio, uint8_t modes)
{
    put_op(b, LR2021_SET_DIO_RF_SWITCH);
    b[2] = dio;
    b[3] = modes & 0x1F;
    return 4;
}

size_t lr2021_cmd_set_dio_irq(uint8_t *b, uint8_t dio, uint32_t irqs)
{
    put_op(b, LR2021_SET_DIO_IRQ_CONFIG);
    b[2] = dio;
    return 3 + put32(b + 3, irqs);
}

size_t lr2021_cmd_clear_irq(uint8_t *b, uint32_t irqs)
{
    put_op(b, LR2021_CLEAR_IRQ);
    return 2 + put32(b + 2, irqs);
}

size_t lr2021_cmd_clear_fifo_flags(uint8_t *b, uint8_t rx, uint8_t tx)
{
    put_op(b, LR2021_CLEAR_FIFO_IRQ_FLAGS);
    b[2] = rx;
    b[3] = tx;
    return 4;
}

size_t lr2021_cmd_config_fifo_irq(uint8_t *b, uint8_t rx_en, uint8_t tx_en, uint16_t rx_high, uint16_t tx_low,
                                  uint16_t rx_low, uint16_t tx_high)
{
    size_t o = put_op(b, LR2021_CONFIG_FIFO_IRQ);
    b[o++] = rx_en;
    b[o++] = tx_en;
    o += put16(b + o, rx_high);
    o += put16(b + o, tx_low);
    o += put16(b + o, rx_low);
    o += put16(b + o, tx_high);
    return o;
}

size_t lr2021_cmd_set_rx(uint8_t *b, uint32_t timeout)
{
    put_op(b, LR2021_SET_RX);
    return 2 + put24(b + 2, timeout & 0xFFFFFF);
}

size_t lr2021_cmd_set_tx(uint8_t *b, uint32_t timeout)
{
    put_op(b, LR2021_SET_TX);
    return 2 + put24(b + 2, timeout & 0xFFFFFF);
}

size_t lr2021_cmd_set_gain(uint8_t *b, uint8_t step)
{
    return lr2021_cmd_u8(b, LR2021_SET_AGC_GAIN_MANUAL, step > 13 ? 13 : step);
}

size_t lr2021_cmd_set_tx_params(uint8_t *b, int8_t power, uint8_t ramp)
{
    put_op(b, LR2021_SET_TX_PARAMS);
    b[2] = (uint8_t)power;
    b[3] = ramp;
    return 4;
}

size_t lr2021_cmd_write_reg32(uint8_t *b, uint32_t addr, uint32_t value)
{
    put_op(b, LR2021_WRITE_REG_MEM32);
    put24(b + 2, addr);
    return 5 + put32(b + 5, value);
}

size_t lr2021_cmd_read_reg32(uint8_t *b, uint32_t addr, uint8_t words)
{
    put_op(b, LR2021_READ_REG_MEM32);
    put24(b + 2, addr);
    b[5] = words;
    return 6;
}

size_t lr2021_cmd_set_pa_config(uint8_t *b, bool hf, uint8_t lf_duty, uint8_t lf_slices, uint8_t hf_duty)
{
    put_op(b, LR2021_SET_PA_CONFIG);
    b[2] = hf ? 0x80 : 0x00; /* pa_sel, then pa_lf_mode 0 (full single-ended) */
    b[3] = (uint8_t)((lf_duty & 0xF) << 4 | (lf_slices & 0xF));
    b[4] = hf_duty & 0x1F;
    return 5;
}

int8_t lr2021_tx_power_code(double dbm)
{
    double c = floor(dbm * 2.0 + 1e-9);
    return (int8_t)(c < -19 ? -19 : c > 44 ? 44 : c);
}

double lr2021_tx_power_dbm(int8_t code) { return code / 2.0; }

static const uint16_t ramps[19] = {2, 4, 8, 16, 32, 48, 64, 80, 96, 112, 128, 144, 160, 176, 192, 208, 240, 272, 304};

uint8_t lr2021_ramp_code(int us)
{
    for (uint8_t c = 0; c < 19; c++)
        if (ramps[c] >= us)
            return c;
    return 18;
}

int lr2021_ramp_us(uint8_t code) { return code < 19 ? ramps[code] : -1; }

uint32_t lr2021_freq_corrected(double hz, double ppm)
{
    /* the chip's crystal runs fast by ppm: asking for f gives f * (1 + ppm 1e-6), so ask for f / (1 + ppm 1e-6) */
    return (uint32_t)llround(hz / (1.0 + ppm * 1e-6));
}

size_t lr2021_cmd_lora_modulation(uint8_t *b, uint8_t sf, uint8_t bw, uint8_t cr, uint8_t ldro)
{
    put_op(b, LR2021_SET_LORA_MODULATION);
    b[2] = (uint8_t)((sf & 0xF) << 4 | (bw & 0xF));
    b[3] = (uint8_t)((cr & 0xF) << 4 | (ldro & 0x3));
    return 4;
}

size_t lr2021_cmd_lora_packet(uint8_t *b, uint16_t preamble, uint8_t payload_len, uint8_t flags)
{
    put_op(b, LR2021_SET_LORA_PACKET);
    put16(b + 2, preamble);
    b[4] = payload_len;
    b[5] = flags;
    return 6;
}

size_t lr2021_cmd_fsk_modulation(uint8_t *b, uint32_t bitrate, uint8_t shape, uint8_t rx_bw, uint32_t fdev_hz)
{
    put_op(b, LR2021_SET_FSK_MODULATION);
    put32(b + 2, bitrate & 0x7FFFFFFF); /* bit 31 clear: whole bits a second */
    b[6] = shape;
    b[7] = rx_bw;
    return 8 + put24(b + 8, fdev_hz & 0xFFFFFF);
}

size_t lr2021_cmd_fsk_packet(uint8_t *b, uint16_t preamble_bits, uint16_t payload_len)
{
    put_op(b, LR2021_SET_FSK_PACKET);
    put16(b + 2, preamble_bits);
    b[4] = 0x00; /* no preamble detection */
    b[5] = 0x00; /* fixed length in bytes, no address filter */
    put16(b + 6, payload_len);
    b[8] = 0x00; /* no CRC, no DC-free coding */
    return 9;
}

size_t lr2021_cmd_ook_modulation(uint8_t *b, uint32_t bitrate, uint8_t shape, uint8_t rx_bw, uint8_t depth)
{
    put_op(b, LR2021_SET_OOK_MODULATION);
    put32(b + 2, bitrate & 0x7FFFFFFF);
    b[6] = shape;
    b[7] = rx_bw;
    b[8] = depth;
    return 9;
}

size_t lr2021_cmd_fsk_variant(uint8_t *b, uint8_t variant) { return lr2021_cmd_u8(b, LR2021_SET_FSK_VARIANT, variant); }

uint32_t lr2021_pll_word(uint32_t hz) { return (uint32_t)llround((double)hz * 1048576.0 / 1e6); }

size_t lr2021_cmd_retune(uint8_t *b, uint32_t hz) { return lr2021_cmd_write_reg32(b, LR2021_REG_PLL_FREQ, lr2021_pll_word(hz)); }

size_t lr2021_cmd_raw_route(uint8_t *b, uint8_t rate, uint8_t source)
{
    /* rate, the NCO (0), the FIFO (1 << 3) and the source, no delay, no length, the FIFO's width flag */
    const uint8_t arg[11] = {rate, 0, 0, 0, 0, (uint8_t)(1 << 3 | (source & 7)), 0, 0, 0, 0, 0x01};
    put_op(b, LR2021_RAW_ROUTE);
    for (int i = 0; i < 11; i++)
        b[2 + i] = arg[i];
    return 13;
}

/* The rx_bw codes with the widest filter (bits 7:6 = 0) in the datasheet's table 11-4: 0x00, 0x11 to 0x15, 0x17 to
 * 0x27. The others (mode 2, 6, 7, 8 and some decimations) are refused: SetFskModulationParams and the route both
 * answered 0x30 with an error. */
static bool code_listed(int code) { return code < 64 && ((0xFFFFBE0001ull >> code) & 1); }

size_t lr2021_cmd_raw_route_rate(uint8_t *b, uint8_t rate, uint32_t out_hz, uint8_t source)
{
    size_t n = lr2021_cmd_raw_route(b, rate, source);
    put32(b + 3, out_hz & 0x7FFFFFFF); /* bit 31 clear: whole Hz */
    return n;
}

int lr2021_conv_rx_bw_for(double out_hz, double share)
{
    int best = -1;
    double best_rate = 0;
    for (int code = 0; code < 64; code++) {
        uint32_t num, den;
        lr2021_fsk_rx_rate((uint8_t)code, &num, &den);
        const double r = (double)num / den;
        if (!code_listed(code) || r < out_hz || r >= 4 * out_hz ||
            lr2021_fsk_filter_share((uint8_t)code) * r > share * out_hz)
            continue;
        /* of equal rates, the larger decimation */
        if (best < 0 || r > best_rate || (r == best_rate && (code & 7) > (best & 7))) {
            best = code;
            best_rate = r;
        }
    }
    return best;
}

size_t lr2021_cmd_raw_route_fifo(uint8_t *b) { return lr2021_cmd_raw_route(b, 0xFF, LR2021_RAW_SRC_CS16); }

int lr2021_raw_sample_bytes(uint8_t source)
{
    static const uint8_t bytes[7] = {2, 2, 4, 4, 4, 4, 2};
    return source < 7 ? bytes[source] : 0;
}

void lr2021_fsk_rx_rate(uint8_t rx_bw, uint32_t *num, uint32_t *den)
{
    const uint32_t decimation = 4u << (rx_bw & 7), mode = ((rx_bw >> 3) & 7) + 1;
    /* 32 MHz is a multiple of every decimation (4 to 512): the fraction is 32 MHz / decimation over mode */
    *num = 32000000u / decimation;
    *den = mode;
}

double lr2021_fsk_filter_share(uint8_t rx_bw)
{
    static const double share[4] = {0.37, 0.34, 0.30, 0.27};
    return share[rx_bw >> 6];
}

int lr2021_fsk_rx_bw_for(double rate, double max_rate)
{
    int best = -1;
    double best_rate = 0;
    for (int code = 0; code < 64; code++) {
        uint32_t num, den;
        lr2021_fsk_rx_rate((uint8_t)code, &num, &den);
        const double r = (double)num / den;
        if (!code_listed(code))
            continue;
        /* of equal rates, the larger decimation (codes count it up in bits 2:0) */
        if (r >= rate && r <= max_rate &&
            (best < 0 || r < best_rate || (r == best_rate && (code & 7) > (best & 7)))) {
            best = code;
            best_rate = r;
        }
    }
    return best;
}

uint32_t lr2021_raw_rx_rate(uint8_t bw)
{
    return bw == LR2021_LORA_BW_31 ? 125000 : bw == LR2021_LORA_BW_62 ? 250000 : 0;
}

size_t lr2021_cmd_raw_tx(uint8_t *b, uint16_t n, uint32_t rate, uint8_t selector)
{
    put_op(b, LR2021_RAW_TX);
    put16(b + 2, n);
    put32(b + 4, rate);
    b[8] = selector;
    return 9;
}

int8_t lr2021_raw_freq_sample(float hz, float fdev_hz)
{
    float c = roundf(hz * 64.0f / fdev_hz);
    return (int8_t)(c < -128 ? -128 : c > 127 ? 127 : c);
}
