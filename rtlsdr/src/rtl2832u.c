// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The RTL2832U side of the RTL-SDR: vendor requests, the baseband init, the
 * DDC's IF and FIR, the resampler, the corrections, GPIO, and the tuner's I2C
 * through the repeater. Written from docs/spec/rtlsdr.md, section 1 (D1.n
 * cited below), which takes its facts from the RTL2832U datasheet and from
 * jtarrio/webrtlsdr @ 9b3f587 (src/rtlsdr/rtlcom.ts, rtl2832u.ts),
 * Apache-2.0: Copyright 2024 Jacobo Tarrio Barreiro, Copyright 2013 Google
 * Inc. (LICENSES/Apache-2.0.txt). The code is this project's.
 */
#include "rtlsdr/rtl.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* D1.1: wIndex blocks */
#define BLK_DEMOD 0
#define BLK_USB 1
#define BLK_SYS 2
#define BLK_I2C 6
#define IDX_WRITE 0x10

/* D1.2 */
#define USB_SYSCTL 0x2000
#define USB_EPA_CTL 0x2148
#define USB_EPA_MAXPKT 0x2158
#define SYS_DEMOD_CTL 0x3000
#define SYS_GPO 0x3001
#define SYS_GPOE 0x3003
#define SYS_GPD 0x3004
#define SYS_DEMOD_CTL_1 0x300B

#define I2C_CHUNK 7 /* D1.4: values a transfer */

#define RATE_NARROW_MIN 225001u
#define RATE_NARROW_MAX 300000u
#define RATE_WIDE_MIN 900001u
#define RATE_WIDE_MAX 3200000u
#define FREQ_MIN 24000000u
#define FREQ_MAX 1766000000u
#define PPM_MAX 400

/* D1.10: the DDC's low-pass, from tools/rtl_fir_design.py (32 taps, symmetric, first half). */
static const int16_t fir_taps[16] = {3, -18, -31, -49, -68, -81, -80, -59, -13, 58, 151, 259, 370, 471, 546, 587};

/* ── transfers (D1.1 to D1.4) ───────────────────────────────────────────── */

static int xfer(rtl_dev_t *dev, bool in, uint16_t value, uint16_t index, uint8_t *data, uint16_t len)
{
    int r = dev->io.control(dev->io.ctx, in, value, index, data, len);
    if (r == (int)len)
        return 0;
    dev->errors++;
    snprintf(dev->error, sizeof dev->error, "control %s 0x%04x/0x%04x failed (%d)", in ? "read" : "write", value,
             index, r);
    return in ? RTL_EREAD : RTL_EWRITE;
}

/* USB / SYS register, little-endian (D1.2). */
static int blk_put(rtl_dev_t *dev, int block, uint16_t addr, uint32_t val, int len)
{
    uint8_t b[4];
    for (int i = 0; i < len; i++)
        b[i] = (uint8_t)(val >> (8 * i));
    return xfer(dev, false, addr, (uint16_t)(block << 8 | IDX_WRITE), b, (uint16_t)len);
}

static int blk_get(rtl_dev_t *dev, int block, uint16_t addr, uint8_t *val)
{
    return xfer(dev, true, addr, (uint16_t)(block << 8), val, 1);
}

/* Demodulator register, big-endian, then the read W does after each write (D1.3). */
static int dm_put(rtl_dev_t *dev, int page, uint8_t addr, uint32_t val, int len)
{
    uint8_t b[4];
    for (int i = 0; i < len; i++)
        b[i] = (uint8_t)(val >> (8 * (len - 1 - i)));
    int r = xfer(dev, false, (uint16_t)(addr << 8 | 0x20), (uint16_t)(IDX_WRITE | page), b, (uint16_t)len);
    if (r)
        return r;
    uint8_t sink;
    return xfer(dev, true, 0x0120, 0x000A, &sink, 1);
}

static int dm_get(rtl_dev_t *dev, int page, uint8_t addr, uint8_t *val)
{
    return xfer(dev, true, (uint16_t)(addr << 8 | 0x20), (uint16_t)page, val, 1);
}

/* D1.4: the repeater, 1:01 = 0x18 open, 0x10 closed. */
static int repeater(rtl_dev_t *dev, bool open) { return dm_put(dev, 1, 0x01, open ? 0x18 : 0x10, 1); }

/* The tuner's bus: standard I2C requests, writes in chunks (D1.4). */
static int bus_write(void *ctx, uint8_t addr, uint8_t reg, const uint8_t *vals, int n)
{
    rtl_dev_t *dev = ctx;
    for (int i = 0; i < n; i += I2C_CHUNK) {
        int k = n - i < I2C_CHUNK ? n - i : I2C_CHUNK;
        uint8_t b[1 + I2C_CHUNK];
        b[0] = (uint8_t)(reg + i);
        memcpy(b + 1, vals + i, (size_t)k);
        int r = xfer(dev, false, addr, BLK_I2C << 8 | IDX_WRITE, b, (uint16_t)(k + 1));
        if (r)
            return r;
    }
    return 0;
}

static int bus_read(void *ctx, uint8_t addr, uint8_t *out, int n)
{
    rtl_dev_t *dev = ctx;
    uint8_t start = 0;
    int r = xfer(dev, false, addr, BLK_I2C << 8 | IDX_WRITE, &start, 1);
    return r ? r : xfer(dev, true, addr, BLK_I2C << 8, out, (uint16_t)n);
}

/* Detection reads: a tuner that isn't there fails the transfer, which isn't an error (T1.1). */
static int probe_read(void *ctx, uint8_t addr, uint8_t *out, int n)
{
    rtl_dev_t *dev = ctx;
    uint8_t start = 0;
    if (dev->io.control(dev->io.ctx, false, addr, BLK_I2C << 8 | IDX_WRITE, &start, 1) != 1)
        return -1;
    return dev->io.control(dev->io.ctx, true, addr, BLK_I2C << 8, out, (uint16_t)n) == n ? 0 : -1;
}

static void bus_sleep(void *ctx, int ms)
{
    rtl_dev_t *dev = ctx;
    if (dev->io.sleep_ms)
        dev->io.sleep_ms(dev->io.ctx, ms);
}

static r820_bus_t tuner_bus(rtl_dev_t *dev) { return (r820_bus_t){bus_write, bus_read, bus_sleep, dev}; }

/* A tuner call's result to ours, with the message for what isn't a transfer's (those set it already). */
static int tuner_err(rtl_dev_t *dev, int r, const char *what)
{
    if (r == R820_ENOLOCK) {
        snprintf(dev->error, sizeof dev->error, "%s: the tuner's PLL didn't lock", what);
        return RTL_ENOLOCK;
    }
    if (r == R820_ERANGE) {
        snprintf(dev->error, sizeof dev->error, "%s: out of the tuner's range", what);
        return RTL_ERANGE;
    }
    return r < 0 ? RTL_EWRITE : r;
}

static int no_tuner(rtl_dev_t *dev)
{
    if (dev->tuner != RTL_TUNER_NONE)
        return 0;
    snprintf(dev->error, sizeof dev->error, "no tuner");
    return RTL_ENOTUNER;
}

/* ── corrections, IF, GPIO ──────────────────────────────────────────────── */

static double xtal_corrected(uint32_t nominal, int ppm) { return nominal * (1.0 + ppm / 1e6); }

/* D1.6: the IF the tuner really gives, to the DDC. */
static int set_ddc_if(rtl_dev_t *dev)
{
    double if_hz = dev->tuner != RTL_TUNER_NONE ? r820_if_hz(&dev->tun) : R820_IF_HZ;
    int32_t v = (int32_t)-llround(if_hz * 4194304.0 / xtal_corrected(dev->rtl_xtal, dev->ppm));
    uint32_t u = (uint32_t)v & 0x3FFFFF;
    int r = dm_put(dev, 1, 0x19, (u >> 16) & 0x3F, 1);
    if (!r)
        r = dm_put(dev, 1, 0x1A, (u >> 8) & 0xFF, 1);
    if (!r)
        r = dm_put(dev, 1, 0x1B, u & 0xFF, 1);
    return r;
}

/* D1.8 */
static int set_sample_correction(rtl_dev_t *dev)
{
    int32_t v = dev->fixed_sample_rate ? 0 : (int32_t)-floor(dev->ppm * 16777216.0 / 1e6);
    int r = dm_put(dev, 1, 0x3E, ((uint32_t)v >> 8) & 0x3F, 1);
    return r ? r : dm_put(dev, 1, 0x3F, (uint32_t)v & 0xFF, 1);
}

/* D1.11: GPIO n an output at level. */
static int gpio_out(rtl_dev_t *dev, int n, bool level)
{
    const uint8_t bit = (uint8_t)(1u << n);
    uint8_t v;
    int r = blk_get(dev, BLK_SYS, SYS_GPD, &v);
    if (!r)
        r = blk_put(dev, BLK_SYS, SYS_GPD, v & (uint8_t)~bit, 1);
    if (!r)
        r = blk_get(dev, BLK_SYS, SYS_GPOE, &v);
    if (!r)
        r = blk_put(dev, BLK_SYS, SYS_GPOE, v | bit, 1);
    if (!r)
        r = blk_get(dev, BLK_SYS, SYS_GPO, &v);
    if (!r)
        r = blk_put(dev, BLK_SYS, SYS_GPO, level ? (v | bit) : (v & (uint8_t)~bit), 1);
    return r;
}

/* ── FIR (D1.9) ─────────────────────────────────────────────────────────── */

int rtl_fir_pack(const int16_t taps[16], uint8_t out[20])
{
    for (int i = 0; i < 16; i++) {
        int lim = i < 8 ? 128 : 2048;
        if (taps[i] < -lim || taps[i] >= lim)
            return -1;
    }
    for (int i = 0; i < 8; i++)
        out[i] = (uint8_t)taps[i];
    for (int i = 0; i < 4; i++) {
        uint16_t a = (uint16_t)taps[8 + 2 * i] & 0xFFF, b = (uint16_t)taps[9 + 2 * i] & 0xFFF;
        out[8 + 3 * i] = (uint8_t)(a >> 4);
        out[9 + 3 * i] = (uint8_t)((a & 0xF) << 4 | b >> 8);
        out[10 + 3 * i] = (uint8_t)(b & 0xFF);
    }
    return 0;
}

void rtl_fir_unpack(const uint8_t in[20], int16_t taps[16])
{
    for (int i = 0; i < 8; i++)
        taps[i] = (int8_t)in[i];
    for (int i = 0; i < 4; i++) {
        int a = in[8 + 3 * i] << 4 | in[9 + 3 * i] >> 4, b = (in[9 + 3 * i] & 0xF) << 8 | in[10 + 3 * i];
        taps[8 + 2 * i] = (int16_t)(a >= 2048 ? a - 4096 : a);
        taps[9 + 2 * i] = (int16_t)(b >= 2048 ? b - 4096 : b);
    }
}

/* ── open / close ───────────────────────────────────────────────────────── */

/* D1.5 rows 4 to 18 after the FIR's place; the USB rows and the FIR are written around it. */
typedef struct {
    uint8_t page, addr, val;
} dm_wr_t;

static const dm_wr_t init_demod_head[] = {
    {1, 0x01, 0x14}, {1, 0x01, 0x10}, {1, 0x15, 0x00}, {1, 0x16, 0x00}, {1, 0x17, 0x00},
    {1, 0x18, 0x00}, {1, 0x19, 0x00}, {1, 0x1A, 0x00}, {1, 0x1B, 0x00},
};
static const dm_wr_t init_demod_tail[] = {
    {0, 0x19, 0x05}, {1, 0x93, 0xF0}, {1, 0x94, 0x0F}, {1, 0x11, 0x00}, {1, 0x04, 0x00},
    {0, 0x61, 0x60}, {0, 0x06, 0x80}, {1, 0xB1, 0x1B}, {0, 0x0D, 0x83},
};
/* D1.5 rows 19 to 21: a tuner on a low IF. */
static const dm_wr_t init_low_if[] = {{1, 0xB1, 0x1A}, {0, 0x08, 0x4D}, {1, 0x15, 0x01}};

static int dm_list(rtl_dev_t *dev, const dm_wr_t *w, int n)
{
    for (int i = 0; i < n; i++) {
        int r = dm_put(dev, w[i].page, w[i].addr, w[i].val, 1);
        if (r)
            return r;
    }
    return 0;
}
#define DM_LIST(dev, a) dm_list(dev, a, (int)(sizeof a / sizeof a[0]))

static int baseband_init(rtl_dev_t *dev)
{
    uint8_t fir[20];
    rtl_fir_pack(fir_taps, fir);
    int r = blk_put(dev, BLK_USB, USB_SYSCTL, 0x09, 1);
    if (!r)
        r = blk_put(dev, BLK_USB, USB_EPA_MAXPKT, dev->full_speed ? 0x0040 : 0x0200, 2);
    if (!r)
        r = blk_put(dev, BLK_USB, USB_EPA_CTL, 0x0210, 2);
    if (!r)
        r = blk_put(dev, BLK_SYS, SYS_DEMOD_CTL_1, 0x22, 1);
    if (!r)
        r = blk_put(dev, BLK_SYS, SYS_DEMOD_CTL, 0xE8, 1);
    if (!r)
        r = DM_LIST(dev, init_demod_head);
    for (int i = 0; i < 20 && !r; i++)
        r = dm_put(dev, 1, (uint8_t)(0x1C + i), fir[i], 1);
    if (!r)
        r = DM_LIST(dev, init_demod_tail);
    return r;
}

int rtl_open(rtl_dev_t *dev, const rtl_io_t *io, bool full_speed, bool blog_v4)
{
    memset(dev, 0, sizeof *dev);
    dev->io = *io;
    dev->full_speed = full_speed;
    dev->blog_v4 = blog_v4;
    dev->rtl_xtal = dev->tun_xtal = RTL_XTAL_HZ; /* D1.5, T1.13 */
    dev->gpio5 = -1;

    int r = baseband_init(dev);
    if (r)
        return r;

    r = repeater(dev, true);
    if (r)
        return r;
    r820_bus_t probe = {bus_write, probe_read, bus_sleep, dev};
    r820_chip_t chip = r820_probe(&probe);
    r = repeater(dev, false);
    if (r)
        return r;
    if (chip == R820_NONE) {
        snprintf(dev->error, sizeof dev->error, "no R820T / R828D tuner answered");
        return RTL_ENOTUNER;
    }

    r = DM_LIST(dev, init_low_if);
    if (!r)
        r = repeater(dev, true);
    if (r)
        return r;
    r820_bus_t bus = tuner_bus(dev);
    int t = r820_init(&dev->tun, &bus, chip, blog_v4, dev->tun_xtal, RATE_WIDE_MAX);
    r = repeater(dev, false);
    if (t == R820_ENOLOCK || t == R820_ERANGE) {
        snprintf(dev->error, sizeof dev->error, "tuner init failed: the PLL didn't lock for the filter calibration");
        return RTL_ETUNERINIT;
    }
    if (t)
        return RTL_EWRITE; /* the transfer set the message */
    if (r)
        return r;
    dev->tuner = chip == R820_R828D ? RTL_TUNER_R828D : RTL_TUNER_R820T;

    r = set_sample_correction(dev);
    if (!r)
        r = set_ddc_if(dev);
    if (!r)
        r = rtl_set_tuner_gain_mode(dev, false);
    return r;
}

void rtl_close(rtl_dev_t *dev)
{
    if (dev->tuner != RTL_TUNER_NONE && !repeater(dev, true)) {
        r820_standby(&dev->tun); /* T1.11 */
        repeater(dev, false);
    }
    blk_put(dev, BLK_SYS, SYS_DEMOD_CTL, 0x20, 1); /* D1.13 */
}

/* ── rate, frequency, corrections ───────────────────────────────────────── */

uint32_t rtl_resampler_ratio(uint32_t xtal, uint32_t rate, double *real_rate)
{
    if (!rate) {
        if (real_rate)
            *real_rate = 0;
        return 0;
    }
    uint64_t ratio = ((uint64_t)xtal << 22) / rate & ~(uint64_t)3; /* D1.7 */
    if (real_rate)
        *real_rate = ratio ? (double)xtal * 4194304.0 / (double)ratio : 0;
    return (uint32_t)(ratio & 0x0FFFFFFC);
}

int rtl_set_sample_rate(rtl_dev_t *dev, uint32_t rate)
{
    if (!((rate >= RATE_NARROW_MIN && rate <= RATE_NARROW_MAX) || (rate >= RATE_WIDE_MIN && rate <= RATE_WIDE_MAX))) {
        snprintf(dev->error, sizeof dev->error, "%lu S/s: the RTL takes 225001..300000 or 900001..3200000",
                 (unsigned long)rate);
        return RTL_ERANGE;
    }
    int r = no_tuner(dev);
    if (r)
        return r;
    double real;
    uint32_t ratio = rtl_resampler_ratio(dev->rtl_xtal, rate, &real);
    r = dm_put(dev, 1, 0x9F, ratio >> 16, 2);
    if (!r)
        r = dm_put(dev, 1, 0xA1, ratio & 0xFFFF, 2);
    if (!r)
        r = dm_put(dev, 1, 0x01, 0x14, 1);
    if (!r)
        r = dm_put(dev, 1, 0x01, 0x10, 1);
    if (r)
        return r;
    dev->rate = (uint32_t)lround(real);

    r = repeater(dev, true);
    if (r)
        return r;
    int t = r820_set_rate(&dev->tun, rate);
    r = repeater(dev, false);
    if (t)
        return tuner_err(dev, t, "IF filter");
    if (r)
        return r;
    return dev->freq ? rtl_set_center_freq(dev, dev->freq) : set_ddc_if(dev);
}

int rtl_set_center_freq(rtl_dev_t *dev, uint32_t hz)
{
    if (hz < FREQ_MIN || hz > FREQ_MAX) {
        snprintf(dev->error, sizeof dev->error, "%lu Hz: the tuner takes 24 to 1766 MHz", (unsigned long)hz);
        return RTL_ERANGE;
    }
    int r = no_tuner(dev);
    if (r)
        return r;
    r = repeater(dev, true);
    if (r)
        return r;
    int t = r820_tune(&dev->tun, hz);
    r = repeater(dev, false);
    if (t) {
        char what[32];
        snprintf(what, sizeof what, "%lu Hz", (unsigned long)hz);
        return tuner_err(dev, t, what);
    }
    if (r)
        return r;
    if (dev->tun.blog_v4 && dev->gpio5 != (dev->tun.hf ? 0 : 1)) { /* T1.12: the upconverter */
        r = gpio_out(dev, 5, !dev->tun.hf);
        if (r)
            return r;
        dev->gpio5 = dev->tun.hf ? 0 : 1;
    }
    r = set_ddc_if(dev);
    if (r)
        return r;
    dev->freq = hz;
    return 0;
}

int rtl_set_freq_correction(rtl_dev_t *dev, int ppm)
{
    if (ppm < -PPM_MAX || ppm > PPM_MAX) {
        snprintf(dev->error, sizeof dev->error, "%d ppm: out of -%d..%d", ppm, PPM_MAX, PPM_MAX);
        return RTL_ERANGE;
    }
    int r = no_tuner(dev);
    if (r)
        return r;
    dev->ppm = ppm;
    r = set_sample_correction(dev);
    if (r)
        return r;
    r820_set_xtal(&dev->tun, xtal_corrected(dev->tun_xtal, ppm));
    return dev->freq ? rtl_set_center_freq(dev, dev->freq) : set_ddc_if(dev);
}

double rtl_sample_correction_ppm(int ppm) { return floor(ppm * 16777216.0 / 1e6) * 1e6 / 16777216.0; }

/* ── gain ───────────────────────────────────────────────────────────────── */

static int gain_step_for(int tenths)
{
    int n;
    const r820_gain_step_t *g = r820_gain_table(&n);
    int best = 0;
    for (int i = 0; i < n; i++)
        if (g[i].tenths <= tenths)
            best = i;
    return best;
}

static int apply_gain(rtl_dev_t *dev, bool manual, int step)
{
    int r = repeater(dev, true);
    if (r)
        return r;
    int t = manual ? r820_set_gain_step(&dev->tun, step) : r820_set_agc(&dev->tun);
    r = repeater(dev, false);
    if (t)
        return tuner_err(dev, t, "gain");
    return r;
}

int rtl_set_tuner_gain_mode(rtl_dev_t *dev, bool manual)
{
    int r = no_tuner(dev);
    if (r)
        return r;
    int step = gain_step_for(dev->gain);
    r = apply_gain(dev, manual, step);
    if (r)
        return r;
    dev->manual_gain = manual;
    if (manual)
        dev->gain = dev->tun.gains[step];
    return 0;
}

int rtl_set_tuner_gain(rtl_dev_t *dev, int tenths)
{
    int r = no_tuner(dev);
    if (r)
        return r;
    int step = gain_step_for(tenths);
    r = apply_gain(dev, true, step);
    if (r)
        return r;
    dev->manual_gain = true;
    dev->gain = dev->tun.gains[step];
    return 0;
}

int rtl_tuner_gains(const rtl_dev_t *dev, const int **gains)
{
    if (dev->tuner == RTL_TUNER_NONE) {
        *gains = NULL;
        return 0;
    }
    *gains = dev->tun.gains;
    return dev->tun.n_gains;
}

/* ── the rest ───────────────────────────────────────────────────────────── */

int rtl_set_bias_tee(rtl_dev_t *dev, bool on)
{
    int r = gpio_out(dev, 0, on); /* D1.11 */
    if (!r)
        dev->bias_tee = on;
    return r;
}

int rtl_reset_buffer(rtl_dev_t *dev)
{
    int r = blk_put(dev, BLK_USB, USB_EPA_CTL, 0x0210, 2); /* D1.12 */
    return r ? r : blk_put(dev, BLK_USB, USB_EPA_CTL, 0x0000, 2);
}

int rtl_read_tuner_regs(rtl_dev_t *dev, uint8_t *out, int len)
{
    if (len < 1 || len > R820_NREGS) {
        snprintf(dev->error, sizeof dev->error, "tuner registers: %d asked, 1..%d", len, R820_NREGS);
        return RTL_ERANGE;
    }
    int r = no_tuner(dev);
    if (r)
        return r;
    r = repeater(dev, true);
    if (r)
        return r;
    int t = r820_read(&dev->tun, out, len);
    r = repeater(dev, false);
    if (t)
        return RTL_EREAD;
    return r ? r : len;
}

int rtl_read_demod_reg(rtl_dev_t *dev, int page, uint8_t addr, uint8_t *val)
{
    if (page < 0 || page > 4) {
        snprintf(dev->error, sizeof dev->error, "demod page %d: 0..4", page);
        return RTL_ERANGE;
    }
    return dm_get(dev, page, addr, val);
}

int rtl_read_block_reg(rtl_dev_t *dev, int block, uint16_t addr, uint8_t *val)
{
    if (block != BLK_USB && block != BLK_SYS) {
        snprintf(dev->error, sizeof dev->error, "block %d: 1 (USB) or 2 (SYS)", block);
        return RTL_ERANGE;
    }
    return blk_get(dev, block, addr, val);
}

const char *rtl_tuner_name(rtl_tuner_t t)
{
    switch (t) {
    case RTL_TUNER_R820T:
        return "R820T";
    case RTL_TUNER_R828D:
        return "R828D";
    default:
        return "no tuner";
    }
}
