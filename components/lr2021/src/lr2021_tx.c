// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The LR2021's transmit paths (lr2021_tx.h): setup sequences and sample formats, no I/O. Facts: spec section 3. */
#include "lr2021_tx.h"

#include <math.h>
#include <string.h>

#define PI_F 3.14159265f

/* SetFskSyncword (11.3.3) with a zero length: no sync word (none goes on air ahead of raw samples anyway, R3.11) */
static size_t cmd_no_syncword(uint8_t *b)
{
    memset(b, 0, 11);
    b[0] = 0x02;
    b[1] = 0x44;
    return 11;
}

int lr2021_tx_sample_bytes(lr2021_tx_mode_t mode)
{
    switch (mode) {
    case LR2021_TX_IQ_PHASE:
    case LR2021_TX_IQ_POLAR: return 2;
    case LR2021_TX_FREQ:
    case LR2021_TX_PHASE_STEP:
    case LR2021_TX_FSK4: return 1;
    default: return 0;
    }
}

int lr2021_tx_frames(const lr2021_tx_setup_t *s, uint8_t f[][LR2021_TX_FRAME_MAX], size_t *len)
{
    if (s->mode > LR2021_TX_FSK4 || (s->mode != LR2021_TX_CW && (s->rate == 0 || s->n == 0)))
        return -1;
    if ((s->mode == LR2021_TX_FREQ || s->mode == LR2021_TX_FSK4) && s->fdev_hz == 0)
        return -1;
    const bool ook = s->mode == LR2021_TX_IQ_POLAR;
    int k = 0;
    if (s->standby_first)
        len[k] = lr2021_cmd_u8(f[k], LR2021_SET_STANDBY, 0), k++;
    len[k] = lr2021_cmd_u8(f[k], LR2021_SET_PACKET_TYPE, ook ? LR2021_PACKET_OOK : LR2021_PACKET_FSK), k++;
    if (s->calib_fe)
        len[k] = lr2021_cmd_calib_fe(f[k], &s->freq_hz, 1, false), k++;
    len[k] = lr2021_cmd_set_rf_frequency(f[k], s->freq_hz), k++;
    len[k] = lr2021_cmd_set_pa_config(f[k], false, s->pa_lf_duty, s->pa_lf_slices, 16), k++;
    len[k] = lr2021_cmd_set_tx_params(f[k], s->power_code, s->ramp_code), k++;
    len[k] = lr2021_cmd_u8(f[k], LR2021_SET_FALLBACK_MODE, s->fallback ? s->fallback : LR2021_FALLBACK_STBY_RC), k++;
    if (s->mode == LR2021_TX_CW)
        return k;
    if (s->mode == LR2021_TX_FSK4)
        len[k] = lr2021_cmd_fsk_variant(f[k], 1), k++;
    if (ook) {
        len[k] = lr2021_cmd_ook_modulation(f[k], s->rate, 0, s->rx_bw, 0), k++;
    } else {
        /* for the sample modes the bit rate is the sample rate and the shape none; the fdev sets FREQ's scale */
        const bool fsk4 = s->mode == LR2021_TX_FSK4;
        const uint32_t fdev = s->fdev_hz ? s->fdev_hz : 2048;
        len[k] = lr2021_cmd_fsk_modulation(f[k], s->rate, fsk4 ? s->shape : 0, s->rx_bw, fdev), k++;
        len[k] = lr2021_cmd_fsk_packet(f[k], 0, s->n), k++;
        len[k] = cmd_no_syncword(f[k]), k++;
    }
    /* the 1 KB TX FIFO, after CalibFE (which overwrites that memory, R2.5, R3.9) */
    len[k] = lr2021_cmd_write_reg32(f[k], LR2021_REG_TX_FIFO_BASE, LR2021_TX_FIFO_BASE_1K), k++;
    len[k] = lr2021_cmd_write_reg32(f[k], LR2021_REG_TX_FIFO_SIZE, LR2021_TX_FIFO_SIZE_1K), k++;
    len[k] = lr2021_cmd(f[k], LR2021_CLEAR_TX_FIFO), k++;
    if (s->mode != LR2021_TX_FSK4) {
        const uint8_t sel = s->mode == LR2021_TX_FREQ ? LR2021_RAW_TX_FREQ
                            : s->mode == LR2021_TX_PHASE_STEP ? LR2021_RAW_TX_PHASE
                                                              : LR2021_RAW_TX_IQ;
        len[k] = lr2021_cmd_raw_tx(f[k], s->n, s->rate, sel), k++;
    }
    return k;
}

size_t lr2021_tx_start_frame(const lr2021_tx_setup_t *s, uint8_t *buf, uint32_t timeout)
{
    if (s->mode == LR2021_TX_CW)
        return lr2021_cmd_u8(buf, LR2021_SET_TX_TEST_MODE, LR2021_TEST_CW);
    return lr2021_cmd_set_tx(buf, timeout);
}

int8_t lr2021_phase_step_sample(float hz, uint32_t rate)
{
    float c = roundf(hz * 128.0f / (float)rate);
    return (int8_t)(c < -128 ? -128 : c > 127 ? 127 : c);
}

uint32_t lr2021_fsk4_fdev(float outer_hz) { return (uint32_t)lroundf(outer_hz * 64.0f / 60.0f); }

static uint8_t dibit(int8_t sym) { return sym == -1 ? 0 : sym == -3 ? 1 : sym == 1 ? 2 : 3; }

size_t lr2021_fsk4_pack(const int8_t *sym, size_t n, uint8_t *out)
{
    size_t bytes = (n + 3) / 4;
    for (size_t i = 0; i < bytes; i++) {
        uint8_t b = 0;
        for (size_t j = 0; j < 4; j++)
            b = (uint8_t)(b << 2 | (4 * i + j < n ? dibit(sym[4 * i + j]) : 0));
        out[i] = b;
    }
    return bytes;
}

void lr2021_fsk4_from_dmr(uint8_t *bytes, size_t n)
{
    for (size_t i = 0; i < n; i++)
        bytes[i] ^= 0xAA;
}

float lr2021_iq_freq_limit(uint32_t rate)
{
    float r = 0.31f * (float)rate;
    return r < 20000.0f ? r : 20000.0f;
}

static const uint8_t w12_in[] = {2, 6, 10, 14, 18, 22, 26, 30, 34, 38, 42, 46, 50, 54, 58, 62};
static const float w12_db[] = {-28.9f, -19.9f, -15.8f, -12.9f, -10.6f, -8.4f, -6.6f, -5.3f,
                               -4.2f,  -3.1f,  -2.2f,  -1.4f,  -0.8f,  -0.4f, -0.1f, 0.0f};
static const float w12_pm[] = {-40, -39, -38, -36, -32, -27, -21, -17, -13, -9, -6, -3, -2, 0, 0, 1};
const lr2021_am_table_t lr2021_am_w12_920 = {16, w12_in, w12_db, w12_pm};

static float wrap(float a)
{
    while (a > PI_F)
        a -= 2 * PI_F;
    while (a < -PI_F)
        a += 2 * PI_F;
    return a;
}

/* The input magnitude that gives out_db, and the phase shift there. */
static float table_in(const lr2021_am_table_t *t, float want_db, float *pm_deg)
{
    int top = t->n - 1;
    while (top > 0 && t->out_db[top - 1] >= t->out_db[t->n - 1] - 0.05f)
        top--; /* the first entry at full: past it the PA is saturated */
    float in;
    if (want_db <= t->out_db[0]) {
        in = t->in[0];
    } else if (want_db >= t->out_db[top]) {
        in = t->in[top];
    } else {
        int k = 0;
        while (k < top - 1 && t->out_db[k + 1] < want_db)
            k++;
        float span = t->out_db[k + 1] - t->out_db[k];
        float u = span > 0 ? (want_db - t->out_db[k]) / span : 0;
        in = t->in[k] + u * (float)(t->in[k + 1] - t->in[k]);
    }
    /* the phase shift at that input */
    int j = 0;
    while (j < t->n - 2 && t->in[j + 1] < in)
        j++;
    float w = t->in[j + 1] > t->in[j] ? (in - t->in[j]) / (float)(t->in[j + 1] - t->in[j]) : 0;
    w = w < 0 ? 0 : w > 1 ? 1 : w;
    *pm_deg = t->n > 1 ? t->pm_deg[j] + w * (t->pm_deg[j + 1] - t->pm_deg[j]) : t->pm_deg[0];
    return in;
}

static int8_t q8(float v)
{
    float r = roundf(v);
    return (int8_t)(r < -128 ? -128 : r > 127 ? 127 : r);
}

void lr2021_polar_iq8(const float *iq, size_t n, float peak, float r_min, const lr2021_am_table_t *tab, int8_t *out)
{
    if (peak <= 0) {
        for (size_t i = 0; i < n; i++) {
            float m = hypotf(iq[2 * i], iq[2 * i + 1]);
            peak = m > peak ? m : peak;
        }
        if (peak <= 0)
            peak = 1;
    }
    size_t i = 0;
    while (i < n) {
        float a = hypotf(iq[2 * i], iq[2 * i + 1]) / peak;
        if (a >= r_min) {
            float ph = atan2f(iq[2 * i + 1], iq[2 * i]), pm = 0;
            float mag = a > 1 ? 1 : a;
            float in = tab ? table_in(tab, 20.0f * log10f(mag), &pm) : mag * 127.0f;
            ph -= pm * PI_F / 180.0f;
            out[2 * i] = q8(in * cosf(ph));
            out[2 * i + 1] = q8(in * sinf(ph));
            i++;
            continue;
        }
        /* a dip under r_min: from the sample before it to the one after, the phase in a straight line */
        size_t j = i;
        while (j < n && hypotf(iq[2 * j], iq[2 * j + 1]) / peak < r_min)
            j++;
        size_t a0 = i ? i - 1 : i, a1 = j < n ? j : j - 1;
        float p0 = atan2f(iq[2 * a0 + 1], iq[2 * a0]), p1 = atan2f(iq[2 * a1 + 1], iq[2 * a1]);
        float d = wrap(p1 - p0), pm = 0;
        float in = tab ? table_in(tab, 20.0f * log10f(r_min > 0 ? r_min : 1e-6f), &pm) : r_min * 127.0f;
        for (size_t k = i; k < j; k++) {
            float t = a1 > a0 ? (float)(k - a0) / (float)(a1 - a0) : 0;
            float ph = p0 + t * d - pm * PI_F / 180.0f;
            out[2 * k] = q8(in * cosf(ph));
            out[2 * k + 1] = q8(in * sinf(ph));
        }
        i = j;
    }
}

void lr2021_phase_iq8(const float *iq, size_t n, int8_t *out)
{
    float ph = 0;
    for (size_t i = 0; i < n; i++) {
        if (iq[2 * i] != 0 || iq[2 * i + 1] != 0)
            ph = atan2f(iq[2 * i + 1], iq[2 * i]);
        out[2 * i] = q8(100.0f * cosf(ph));
        out[2 * i + 1] = q8(100.0f * sinf(ph));
    }
}
