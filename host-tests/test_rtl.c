// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Host tests for the RTL-SDR driver (rtlsdr/src/rtl2832u.c, r820.c) against a fake dongle, checked against the
 * fact spec docs/spec/rtlsdr.md (the D1.n / T1.n in the comments).
 *
 * The fake keeps the RTL's register blocks and an R820T or R828D behind the I2C repeater: it decodes each vendor
 * request (D1.1 to D1.4), stores what's written, answers reads with the tuner's bits reversed (T1.2), reports the
 * PLL's lock and the filter calibration's code, logs every transfer and can fail any one of them.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtlsdr/rtl.h"

static int checks, failures;
#define CHECK(cond, ...)                                                                                     \
    do {                                                                                                     \
        checks++;                                                                                            \
        if (!(cond)) {                                                                                       \
            failures++;                                                                                      \
            printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                          \
            printf(__VA_ARGS__);                                                                             \
            printf("\n");                                                                                    \
        }                                                                                                    \
    } while (0)

/* ── the fake dongle ────────────────────────────────────────────────────── */

#define LOG_MAX 4096

typedef struct {
    bool in;
    uint16_t value, index, len;
    uint8_t data[64];
    int result;
} xfer_t;

typedef struct {
    uint8_t demod[5][256];
    uint8_t usb[0x200]; /* 0x2000.. */
    uint8_t sys[0x20];  /* 0x3000.. */
    uint8_t tuner_addr; /* 0x34, 0x74, 0 for none */
    uint8_t treg[32];   /* the tuner's registers, real bit order */
    bool repeater;
    int never_lock;     /* the lock bit never sets */
    int unlocked_reads; /* status reads after a PLL write before the lock shows */
    int reads_since_pll;
    uint8_t cal_code;
    int fail_at, count; /* fail transfer number fail_at (1-based) */
    int slept_ms;
    xfer_t log[LOG_MAX];
    int n_log;
} fake_t;

static uint8_t bitrev(uint8_t b)
{
    uint8_t r = 0;
    for (int i = 0; i < 8; i++)
        if (b & (1u << i))
            r |= (uint8_t)(0x80u >> i);
    return r;
}

static int fake_answer(fake_t *f, bool in, uint16_t value, uint16_t index, uint8_t *data, uint16_t len);

static int fake_control(void *ctx, bool in, uint16_t value, uint16_t index, uint8_t *data, uint16_t len)
{
    fake_t *f = ctx;
    int r = fake_answer(f, in, value, index, data, len);
    if (f->n_log <= LOG_MAX && f->n_log > 0)
        f->log[f->n_log - 1].result = r;
    return r;
}

static int fake_answer(fake_t *f, bool in, uint16_t value, uint16_t index, uint8_t *data, uint16_t len)
{
    f->count++;
    if (f->n_log < LOG_MAX) {
        xfer_t *x = &f->log[f->n_log++];
        x->in = in;
        x->value = value;
        x->index = index;
        x->len = len;
        if (!in && len <= 64)
            memcpy(x->data, data, len);
    }
    if (f->count == f->fail_at)
        return -1;
    const int block = index >> 8 & 0xF, page = index & 0xF;
    const bool wr = index & 0x10;
    if (wr == in || len > 64)
        return -9; /* direction and wIndex disagree */
    switch (block) {
    case 0: {
        if ((value & 0xFF) != 0x20)
            return -9;
        int addr = value >> 8;
        if (in && page == 0x0A && addr == 0x01) { /* the read after each write (D1.3) */
            data[0] = 0;
            return len;
        }
        if (page > 4 || addr + len > 256)
            return -9;
        if (in) {
            memcpy(data, &f->demod[page][addr], len);
        } else {
            memcpy(&f->demod[page][addr], data, len);
            if (page == 1 && addr == 0x01)
                f->repeater = data[0] & 0x08;
        }
        return len;
    }
    case 1:
    case 2: {
        uint8_t *mem = block == 1 ? f->usb : f->sys;
        int base = block == 1 ? 0x2000 : 0x3000, size = block == 1 ? (int)sizeof f->usb : (int)sizeof f->sys;
        int off = value - base;
        if (off < 0 || off + len > size)
            return -9;
        if (in)
            memcpy(data, mem + off, len);
        else
            memcpy(mem + off, data, len);
        return len;
    }
    case 6: {
        if (!f->repeater || !f->tuner_addr || value != f->tuner_addr)
            return -1; /* nobody answers */
        if (in) {
            f->treg[0] = 0x96;
            bool locked = !f->never_lock && f->reads_since_pll >= f->unlocked_reads;
            f->treg[2] = locked ? 0x40 : 0x00;
            f->treg[4] = (uint8_t)(0x20 | f->cal_code);
            for (int i = 0; i < len; i++)
                data[i] = bitrev(f->treg[i]);
            f->reads_since_pll++;
            return len;
        }
        if (len > 8)
            return -9; /* the bridge's chunk (D1.4) */
        int reg = data[0];
        for (int i = 1; i < len; i++) {
            if (reg + i - 1 < 5 || reg + i - 1 > 31)
                return -9;
            f->treg[reg + i - 1] = data[i];
            if (reg + i - 1 == 0x16) /* the PLL's block ends at R22 */
                f->reads_since_pll = 0;
        }
        return len;
    }
    default:
        return -9;
    }
}

static void fake_sleep(void *ctx, int ms) { ((fake_t *)ctx)->slept_ms += ms; }

static fake_t *fake_new(uint8_t tuner_addr)
{
    fake_t *f = calloc(1, sizeof *f);
    f->tuner_addr = tuner_addr;
    f->cal_code = 9;
    return f;
}

static rtl_io_t fake_io(fake_t *f) { return (rtl_io_t){fake_control, fake_sleep, f}; }

static int open_fake(rtl_dev_t *dev, fake_t *f, bool full_speed, bool v4)
{
    rtl_io_t io = fake_io(f);
    return rtl_open(dev, &io, full_speed, v4);
}

static const xfer_t *find_write(const fake_t *f, int from, uint16_t value, uint16_t index)
{
    for (int i = from; i < f->n_log; i++)
        if (!f->log[i].in && f->log[i].value == value && f->log[i].index == index)
            return &f->log[i];
    return NULL;
}

/* The LO the tuner's PLL registers give, by RD's formula (T1.8). */
static double fake_lo(const fake_t *f, double xtal, int *sel_out)
{
    int sel = f->treg[0x10] >> 5;
    int nint = 4 * (f->treg[0x14] & 0x3F) + (f->treg[0x14] >> 6) + 13;
    int sdm = f->treg[0x16] << 8 | f->treg[0x15];
    if (sel_out)
        *sel_out = sel;
    return 2 * xtal * (nint + sdm / 65536.0) / (double)(2 << sel);
}

/* The DDC's IF from 1:19..1:1B (D1.6), in Hz. */
static double fake_ddc_if(const fake_t *f, double xtal)
{
    int32_t v = (int32_t)((uint32_t)(f->demod[1][0x19] & 0x3F) << 16 | f->demod[1][0x1A] << 8 | f->demod[1][0x1B]);
    if (v & 0x200000)
        v -= 0x400000;
    return -v * xtal / 4194304.0;
}

/* ── tests ──────────────────────────────────────────────────────────────── */

static void test_request_encoding(void)
{
    rtl_dev_t dev;
    fake_t *f = fake_new(0x34);
    CHECK(open_fake(&dev, f, false, false) == 0, "%s", dev.error);
    /* D1.5 row 1: USB_SYSCTL, little-endian, block 1 with the write bit */
    const xfer_t *x = &f->log[0];
    CHECK(!x->in && x->value == 0x2000 && x->index == 0x0110 && x->len == 1 && x->data[0] == 0x09, "first 0x%04x",
          x->value);
    x = &f->log[1];
    CHECK(x->value == 0x2158 && x->len == 2 && x->data[0] == 0x00 && x->data[1] == 0x02, "maxpkt %02x %02x",
          x->data[0], x->data[1]);
    x = &f->log[3];
    CHECK(x->value == 0x300B && x->index == 0x0210 && x->data[0] == 0x22, "sys 0x%04x/0x%04x", x->value, x->index);
    /* a demod write: wValue addr << 8 | 0x20, wIndex 0x10 | page, then the read at 0x0120 / 0x000A (D1.3) */
    x = &f->log[5];
    CHECK(!x->in && x->value == 0x0120 && x->index == 0x0011 && x->len == 1 && x->data[0] == 0x14, "demod 0x%04x/0x%04x",
          x->value, x->index);
    x = &f->log[6];
    CHECK(x->in && x->value == 0x0120 && x->index == 0x000A && x->len == 1, "read-back 0x%04x/0x%04x", x->value,
          x->index);
    /* I2C: wValue the address, wIndex 0x0610 / 0x0600, never more than 8 bytes a write (D1.4) */
    int i2c_w = 0, i2c_r = 0, too_long = 0;
    for (int i = 0; i < f->n_log; i++) {
        const xfer_t *y = &f->log[i];
        if (y->index == 0x0610) {
            i2c_w++;
            too_long += y->len > 8;
            CHECK(y->value == 0x34, "i2c addr 0x%02x", y->value);
        } else if (y->index == 0x0600) {
            i2c_r++;
        }
    }
    CHECK(i2c_w > 50 && i2c_r >= 5 && !too_long, "i2c %d writes %d reads, %d too long", i2c_w, i2c_r, too_long);
    /* the init block from 0x05 went in chunks of 7 values, each starting at its own register */
    x = find_write(f, 0, 0x34, 0x0610);
    while (x && !(x->len == 8 && x->data[0] == 0x05))
        x = find_write(f, (int)(x - f->log) + 1, 0x34, 0x0610);
    CHECK(x && x[1].len == 8 && x[1].data[0] == 0x0C && x[2].data[0] == 0x13 && x[3].data[0] == 0x1A &&
              x[3].len == 7,
          "init chunks");
    /* and their values are T1.3's, register by register */
    static const uint8_t t13[27] = {0x83, 0x32, 0x75, 0xC0, 0x40, 0xD6, 0x6C, 0xF5, 0x63, 0x75, 0x68, 0x64, 0x83, 0x80,
                                    0x00, 0x0F, 0x00, 0xC0, 0x30, 0x48, 0xCC, 0x60, 0x00, 0x54, 0xAE, 0x4A, 0xC0};
    uint8_t sent[27] = {0};
    for (int c = 0; x && c < 4; c++)
        memcpy(sent + 7 * c, x[c].data + 1, (size_t)(x[c].len - 1));
    CHECK(!memcmp(sent, t13, sizeof t13), "init values");
    CHECK(f->slept_ms >= 5, "slept %d", f->slept_ms);
    CHECK(dev.errors == 0 && dev.tuner == RTL_TUNER_R820T, "errors %d", dev.errors);
    free(f);

    /* Full Speed: 64-byte packets */
    f = fake_new(0x34);
    CHECK(open_fake(&dev, f, true, false) == 0, "%s", dev.error);
    CHECK(f->log[1].data[0] == 0x40 && f->log[1].data[1] == 0x00 && f->usb[0x158] == 0x40, "full speed maxpkt");
    free(f);
}

static void test_init_writes(void)
{
    rtl_dev_t dev;
    fake_t *f = fake_new(0x34);
    CHECK(open_fake(&dev, f, false, false) == 0, "%s", dev.error);
    /* D1.5 */
    CHECK(f->sys[0x00] == 0xE8 && f->sys[0x0B] == 0x22, "sys %02x %02x", f->sys[0], f->sys[0x0B]);
    CHECK(f->usb[0x000] == 0x09 && f->usb[0x148] == 0x10 && f->usb[0x149] == 0x02, "usb");
    CHECK(f->demod[0][0x19] == 0x05 && f->demod[1][0x93] == 0xF0 && f->demod[1][0x94] == 0x0F, "sdr mode");
    CHECK(f->demod[1][0x11] == 0x00 && f->demod[1][0x04] == 0x00 && f->demod[0][0x61] == 0x60, "agc / pid");
    CHECK(f->demod[0][0x06] == 0x80 && f->demod[0][0x0D] == 0x83, "adc / clock");
    CHECK(f->demod[1][0xB1] == 0x1A && f->demod[0][0x08] == 0x4D && f->demod[1][0x15] == 0x01, "low IF %02x %02x %02x",
          f->demod[1][0xB1], f->demod[0][0x08], f->demod[1][0x15]);
    CHECK(f->demod[1][0x01] == 0x10 && !f->repeater, "repeater closed");
    CHECK(f->demod[1][0x3E] == 0 && f->demod[1][0x3F] == 0, "no sample correction");
    /* the FIR (D1.9, D1.10) */
    int16_t taps[16];
    rtl_fir_unpack(&f->demod[1][0x1C], taps);
    static const int16_t want[16] = {3, -18, -31, -49, -68, -81, -80, -59, -13, 58, 151, 259, 370, 471, 546, 587};
    CHECK(!memcmp(taps, want, sizeof want), "fir taps");
    int sum = 0;
    for (int i = 0; i < 16; i++)
        sum += 2 * taps[i];
    CHECK(abs(sum - 4096) <= 8, "fir gain %d", sum);
    /* the DDC's IF before any tune: the nominal 3.57 MHz (D1.6) */
    CHECK(fabs(fake_ddc_if(f, 28.8e6) - 3570000) < 7, "IF %.1f", fake_ddc_if(f, 28.8e6));
    /* the tuner (T1.3 to T1.6, T1.10's AGC) */
    CHECK((f->treg[0x0A] & 0x1F) == (0x10 | 9) && f->treg[0x0B] == 0x6B, "IF filter %02x %02x", f->treg[0x0A],
          f->treg[0x0B]);
    CHECK(dev.tun.cal == 9, "cal %d", dev.tun.cal);
    CHECK((f->treg[0x0C] & 0x80) && (f->treg[0x0C] & 0x1F) == 0x0B, "VGA %02x", f->treg[0x0C]);
    CHECK(!(f->treg[0x05] & 0x10) && (f->treg[0x07] & 0x10) && !dev.manual_gain, "AGC %02x %02x", f->treg[0x05],
          f->treg[0x07]);
    CHECK((f->treg[0x1A] & 0x0C) == 0x08 && (f->treg[0x1A] & 0x30) == 0x20, "R26 %02x", f->treg[0x1A]);
    CHECK(f->treg[0x1D] == ((0xAE & ~0xC7 & ~0x38) | 0xC5 | 0x18), "R29 %02x", f->treg[0x1D]);
    CHECK(f->treg[0x1E] == 0x4D && f->treg[0x0D] == 0x53 && f->treg[0x0F] == 0x68, "R30 %02x R13 %02x R15 %02x",
          f->treg[0x1E], f->treg[0x0D], f->treg[0x0F]);
    CHECK((f->treg[0x05] & 0xE0) == 0x00 && (f->treg[0x1F] & 0x80) == 0, "R5 %02x R31 %02x", f->treg[0x05],
          f->treg[0x1F]);
    /* the calibration's PLL at 56 MHz: SEL_DIV 4 (VCO 1.792 GHz) */
    int sel;
    double lo = fake_lo(f, 28.8e6, &sel);
    CHECK(sel == 4 && fabs(lo - 56e6) < 100, "cal LO %.0f sel %d", lo, sel);
    free(f);

    /* a calibration code of 15 counts as 0 (T1.5) */
    f = fake_new(0x34);
    f->cal_code = 15;
    CHECK(open_fake(&dev, f, false, false) == 0 && dev.tun.cal == 0 && (f->treg[0x0A] & 0x1F) == 0x10, "cal 15");
    free(f);
}

static void test_detection(void)
{
    rtl_dev_t dev;
    fake_t *f = fake_new(0x34);
    CHECK(open_fake(&dev, f, false, false) == 0 && dev.tuner == RTL_TUNER_R820T, "R820T");
    CHECK(!strcmp(rtl_tuner_name(dev.tuner), "R820T"), "%s", rtl_tuner_name(dev.tuner));
    free(f);
    f = fake_new(0x74);
    CHECK(open_fake(&dev, f, false, false) == 0 && dev.tuner == RTL_TUNER_R828D, "R828D: %s", dev.error);
    CHECK(dev.errors == 0 && dev.tun.addr == 0x74, "the R820T probe isn't an error: %d", dev.errors);
    int i2c_74 = 0;
    for (int i = 0; i < f->n_log; i++)
        i2c_74 += f->log[i].index == 0x0610 && f->log[i].value == 0x74;
    CHECK(i2c_74 > 50, "writes to 0x74: %d", i2c_74);
    free(f);
    f = fake_new(0);
    CHECK(open_fake(&dev, f, false, false) == RTL_ENOTUNER && strstr(dev.error, "no R820T"), "none: %s", dev.error);
    CHECK(dev.tuner == RTL_TUNER_NONE && f->demod[1][0x01] == 0x10, "repeater closed after");
    const int *g;
    CHECK(rtl_tuner_gains(&dev, &g) == 0 && rtl_set_center_freq(&dev, 461000000) == RTL_ENOTUNER, "no tuner calls");
    free(f);
}

static void test_pll(void)
{
    /* the plan over the whole range, at three crystal errors (T1.8) */
    int bad = 0;
    for (int ppm = -50; ppm <= 50; ppm += 50) {
        double xtal = 28.8e6 * (1 + ppm / 1e6);
        for (double f = 24e6; f <= 1766e6; f += 0.25e6) {
            double lo = f + 3570000;
            r820_pll_t p;
            if (r820_pll_plan(xtal, lo, &p)) {
                bad++;
                continue;
            }
            double step = 2 * xtal / 65536 / (double)(2 << p.sel_div);
            double rd = 2 * xtal * (p.nint + p.sdm / 65536.0) / (double)(2 << p.sel_div); /* RD's formula */
            if (p.sel_div > 6 || p.nint < 13 || p.nint > 63 || p.vco_hz < 1.77e9 - 1 || p.vco_hz > 3.54e9 + step ||
                fabs(rd - lo) > step / 2 + 1e-3 || fabs(p.lo_hz - rd) > 1e-3)
                bad++;
        }
    }
    CHECK(bad == 0, "%d plans out of the rules", bad);
    r820_pll_t p;
    CHECK(r820_pll_plan(28.8e6, 1.9e9, &p) == R820_ERANGE, "above the VCO");
    CHECK(r820_pll_plan(28.8e6, 5e6, &p) == R820_ERANGE, "below the dividers");
    CHECK(r820_pll_plan(28.8e6, 0, &p) == R820_ERANGE, "zero");
    /* a fraction that rounds up carries into Nint: 2 * 28.8 MHz * 40 exactly, less a hair */
    CHECK(r820_pll_plan(28.8e6, 2 * 28.8e6 * 40 / 4 - 0.01, &p) == 0 && p.nint == 40 && p.sdm == 0 &&
              p.sel_div == 1,
          "carry: nint %d sdm %d", p.nint, p.sdm);

    /* through the fake: the registers give the LO within half a step, and the DDC the rest (D1.6) */
    rtl_dev_t dev;
    fake_t *f = fake_new(0x34);
    CHECK(open_fake(&dev, f, false, false) == 0, "%s", dev.error);
    static const uint32_t freqs[] = {24000000, 27000000, 50000000, 100000000, 146520000, 433920000, 461162500,
                                     885000000, 1000000000, 1296000000, 1766000000};
    for (int i = 0; i < (int)(sizeof freqs / sizeof freqs[0]); i++) {
        CHECK(rtl_set_center_freq(&dev, freqs[i]) == 0, "%u: %s", freqs[i], dev.error);
        int sel;
        double lo = fake_lo(f, 28.8e6, &sel);
        double step = 2 * 28.8e6 / 65536 / (double)(2 << sel);
        double want = freqs[i] + 3570000.0;
        CHECK(fabs(lo - want) <= step / 2 + 1e-3, "%u: LO %.1f, want %.1f", freqs[i], lo, want);
        CHECK(((f->treg[0x12] & 0x08) != 0) == ((f->treg[0x15] | f->treg[0x16]) == 0), "PW_SDM at %u", freqs[i]);
        CHECK((f->treg[0x12] & 0xE0) == 0x80 && (f->treg[0x1A] & 0x0C) == 0x08, "VCO current, auto clock");
        double ddc = fake_ddc_if(f, 28.8e6);
        CHECK(fabs(ddc - (lo - freqs[i])) < 7, "%u: DDC IF %.1f, tuner IF %.1f", freqs[i], ddc, lo - freqs[i]);
        CHECK(dev.freq == freqs[i], "freq");
    }
    /* the RF filters by LO (T1.7): 461 MHz (LO 464.7 MHz) on the 310 MHz row, 60 MHz (LO 63.6 MHz) on the 60 */
    CHECK(rtl_set_center_freq(&dev, 461000000) == 0 && (f->treg[0x1A] & 0xC3) == 0x41 && f->treg[0x1B] == 0x00 &&
              !(f->treg[0x17] & 0x08),
          "461 MHz row: %02x %02x %02x", f->treg[0x1A], f->treg[0x1B], f->treg[0x17]);
    CHECK(rtl_set_center_freq(&dev, 60000000) == 0 && (f->treg[0x1A] & 0xC3) == 0x02 && f->treg[0x1B] == 0x7B &&
              (f->treg[0x17] & 0x08),
          "60 MHz row: %02x %02x %02x", f->treg[0x1A], f->treg[0x1B], f->treg[0x17]);
    CHECK(rtl_set_center_freq(&dev, 23999999) == RTL_ERANGE && rtl_set_center_freq(&dev, 1766000001) == RTL_ERANGE,
          "range");
    CHECK(dev.freq == 60000000, "freq kept");

    /* the lock: missed 5 times (past the first 4 reads) locks after the VCO current goes to 3 */
    f->unlocked_reads = 5;
    CHECK(rtl_set_center_freq(&dev, 461162500) == 0, "%s", dev.error);
    CHECK((f->treg[0x12] & 0xE0) == 0x60, "VCO current %02x", f->treg[0x12]);
    f->unlocked_reads = 2;
    CHECK(rtl_set_center_freq(&dev, 461162500) == 0 && (f->treg[0x12] & 0xE0) == 0x80, "locked on the 3rd read");
    f->never_lock = 1;
    CHECK(rtl_set_center_freq(&dev, 461162500) == RTL_ENOLOCK && strstr(dev.error, "didn't lock"), "%s", dev.error);
    CHECK(f->demod[1][0x01] == 0x10, "repeater closed after a failure");
    free(f);

    f = fake_new(0x34);
    f->never_lock = 1;
    CHECK(open_fake(&dev, f, false, false) == RTL_ETUNERINIT && strstr(dev.error, "PLL"), "init: %s", dev.error);
    free(f);
}

static void test_rates(void)
{
    double real;
    CHECK(rtl_resampler_ratio(28800000, 288000, &real) == 0x09000000 && real == 288000.0, "288000: %f", real);
    CHECK(rtl_resampler_ratio(28800000, 240000, &real) == 0x0E000000 && real == 240000.0, "240000: %f", real);
    CHECK(rtl_resampler_ratio(28800000, 250000, &real) == (483183820u & 0x0FFFFFFC) && fabs(real - 250000) < 0.001,
          "250000: %f", real);
    CHECK(rtl_resampler_ratio(28800000, 2400000, &real) == 0x03000000 && real == 2400000.0, "2.4M: %f", real);
    CHECK(rtl_resampler_ratio(28800000, 0, &real) == 0 && real == 0, "0");
    /* every rate in range: the ratio is a multiple of 4, the real rate within one ratio step of the ask */
    int bad = 0;
    for (uint32_t r = 225001; r <= 3200000; r += r < 300000 ? 1013 : 7919) {
        if (r > 300000 && r < 900001)
            r = 900001;
        uint32_t ratio = rtl_resampler_ratio(28800000, r, &real);
        double step = real * 4 / (28800000.0 * 4194304.0 / real);
        if ((ratio & 3) || real < r - 1e-9 || real - r > step + 1e-9)
            bad++;
    }
    CHECK(bad == 0, "%d rates off", bad);

    rtl_dev_t dev;
    fake_t *f = fake_new(0x34);
    CHECK(open_fake(&dev, f, false, false) == 0, "%s", dev.error);
    CHECK(rtl_set_sample_rate(&dev, 288000) == 0 && dev.rate == 288000, "%s", dev.error);
    CHECK(f->demod[1][0x9F] == 0x09 && f->demod[1][0xA0] == 0 && f->demod[1][0xA1] == 0 && f->demod[1][0xA2] == 0,
          "ratio regs %02x %02x %02x %02x", f->demod[1][0x9F], f->demod[1][0xA0], f->demod[1][0xA1], f->demod[1][0xA2]);
    CHECK(dev.tun.band == 0, "narrow row %d", dev.tun.band);
    CHECK(rtl_set_sample_rate(&dev, 250000) == 0 && dev.rate == 250000, "250000");
    CHECK(rtl_set_sample_rate(&dev, 240000) == 0 && dev.rate == 240000, "240000");
    /* the ratio from the nominal crystal: a ppm doesn't change it (D1.7) */
    CHECK(rtl_set_freq_correction(&dev, 25) == 0 && rtl_set_sample_rate(&dev, 288000) == 0 &&
              f->demod[1][0x9F] == 0x09 && dev.rate == 288000,
          "ratio with ppm");
    CHECK(rtl_set_sample_rate(&dev, 2400000) == 0 && dev.rate == 2400000 && dev.tun.band == 1, "2.4M");
    static const uint32_t refused[] = {225000, 300001, 600000, 900000, 3200001, 0};
    for (int i = 0; i < 6; i++)
        CHECK(rtl_set_sample_rate(&dev, refused[i]) == RTL_ERANGE && dev.rate == 2400000, "%u", refused[i]);
    /* with a frequency set, a rate change re-tunes */
    CHECK(rtl_set_center_freq(&dev, 461162500) == 0, "%s", dev.error);
    int before = f->n_log;
    CHECK(rtl_set_sample_rate(&dev, 288000) == 0 && find_write(f, before, 0x34, 0x0610), "re-tuned");
    free(f);
}

static void test_ppm(void)
{
    CHECK(fabs(rtl_sample_correction_ppm(10) - 167 / 16.777216) < 1e-9, "%f", rtl_sample_correction_ppm(10));
    CHECK(fabs(rtl_sample_correction_ppm(-10) + 168 / 16.777216) < 1e-9, "%f", rtl_sample_correction_ppm(-10));
    CHECK(rtl_sample_correction_ppm(0) == 0, "0");

    rtl_dev_t dev;
    fake_t *f = fake_new(0x34);
    CHECK(open_fake(&dev, f, false, false) == 0, "%s", dev.error);
    CHECK(rtl_set_center_freq(&dev, 461162500) == 0, "%s", dev.error);
    /* the sample frequency register: -floor(10 * 2^24 / 1e6) = -167, 14 bits (D1.8) */
    CHECK(rtl_set_freq_correction(&dev, 10) == 0 && dev.ppm == 10, "%s", dev.error);
    CHECK(f->demod[1][0x3E] == 0x3F && f->demod[1][0x3F] == 0x59, "ppm regs %02x %02x", f->demod[1][0x3E],
          f->demod[1][0x3F]);
    /* the LO follows the corrected crystal: by the true crystal it lands on the frequency */
    double xtal = 28.8e6 * (1 + 10 / 1e6);
    int sel;
    double lo = fake_lo(f, xtal, &sel);
    double step = 2 * xtal / 65536 / (double)(2 << sel);
    CHECK(fabs(lo - (461162500 + 3570000.0)) <= step / 2 + 1e-3, "LO %.1f", lo);
    CHECK(fabs(fake_ddc_if(f, xtal) - (lo - 461162500)) < 7, "DDC IF %.1f", fake_ddc_if(f, xtal));
    /* fixed_sample_rate: the register stays 0 */
    dev.fixed_sample_rate = true;
    CHECK(rtl_set_freq_correction(&dev, -7) == 0 && f->demod[1][0x3E] == 0 && f->demod[1][0x3F] == 0, "fixed");
    CHECK(rtl_set_freq_correction(&dev, 401) == RTL_ERANGE && rtl_set_freq_correction(&dev, -401) == RTL_ERANGE &&
              dev.ppm == -7,
          "range");
    CHECK(rtl_set_freq_correction(&dev, 400) == 0, "400");
    free(f);
}

static void test_gain(void)
{
    rtl_dev_t dev;
    fake_t *f = fake_new(0x34);
    CHECK(open_fake(&dev, f, false, false) == 0, "%s", dev.error);
    const int *g;
    int n = rtl_tuner_gains(&dev, &g);
    CHECK(n == 27 && g[0] == 0 && g[1] == 32 && g[2] == 45 && g[26] == 462, "%d steps", n);
    int up = 1;
    for (int i = 1; i < n; i++)
        up &= g[i] > g[i - 1];
    CHECK(up, "ascending");
    /* the table's codes: LNA ceil(k/2), mixer floor(k/2) up to mixer 12, then the LNA alone; VGA 8 (T1.10) */
    int tn;
    const r820_gain_step_t *t = r820_gain_table(&tn);
    int rule = tn == n;
    for (int k = 0; k < tn; k++)
        rule &= t[k].lna == (k < 25 ? (k + 1) / 2 : k - 12) && t[k].mix == (k < 25 ? k / 2 : 12) && t[k].vga == 8 &&
                t[k].tenths == g[k];
    CHECK(rule, "gain table");

    CHECK(rtl_set_tuner_gain(&dev, 297) == 0 && dev.gain == 291 && dev.manual_gain, "297 -> %d", dev.gain);
    CHECK(f->treg[0x05] == ((0x83 & 0xE0 & ~0xE0) | 0x10 | 7) && (f->treg[0x07] & 0x1F) == 7 &&
              (f->treg[0x0C] & 0x1F) == 8 && (f->treg[0x0C] & 0x80),
          "codes %02x %02x %02x", f->treg[0x05], f->treg[0x07], f->treg[0x0C]);
    CHECK(rtl_set_tuner_gain(&dev, 319) == 0 && dev.gain == 319 && (f->treg[0x05] & 0x0F) == 8 &&
              (f->treg[0x07] & 0x0F) == 7,
          "319");
    CHECK(rtl_set_tuner_gain(&dev, -10) == 0 && dev.gain == 0 && (f->treg[0x05] & 0x1F) == 0x10, "below");
    CHECK(rtl_set_tuner_gain(&dev, 1000) == 0 && dev.gain == 462 && (f->treg[0x05] & 0x0F) == 14 &&
              (f->treg[0x07] & 0x0F) == 12,
          "above");
    CHECK(rtl_set_tuner_gain_mode(&dev, false) == 0 && !dev.manual_gain && !(f->treg[0x05] & 0x10) &&
              (f->treg[0x07] & 0x10) && (f->treg[0x0C] & 0x1F) == 0x0B,
          "AGC");
    CHECK(rtl_set_tuner_gain_mode(&dev, true) == 0 && dev.manual_gain && dev.gain == 462 &&
              (f->treg[0x05] & 0x1F) == 0x1E,
          "manual again");
    free(f);
}

static void test_bias_tee_and_buffer(void)
{
    rtl_dev_t dev;
    fake_t *f = fake_new(0x34);
    CHECK(open_fake(&dev, f, false, false) == 0, "%s", dev.error);
    f->sys[0x04] = 0xFF; /* all inputs */
    f->sys[0x01] = 0x10;
    CHECK(rtl_set_bias_tee(&dev, true) == 0 && dev.bias_tee, "%s", dev.error);
    CHECK(f->sys[0x04] == 0xFE && (f->sys[0x03] & 1) && f->sys[0x01] == 0x11, "on: %02x %02x %02x", f->sys[0x04],
          f->sys[0x03], f->sys[0x01]);
    CHECK(rtl_set_bias_tee(&dev, false) == 0 && !dev.bias_tee && f->sys[0x01] == 0x10, "off");
    int before = f->n_log;
    CHECK(rtl_reset_buffer(&dev) == 0, "reset");
    CHECK(f->n_log == before + 2 && f->log[before].value == 0x2148 && f->log[before].data[0] == 0x10 &&
              f->log[before].data[1] == 0x02 && f->log[before + 1].data[0] == 0 && f->log[before + 1].data[1] == 0,
          "fifo reset");
    /* reads for looking at the chips */
    uint8_t regs[32], v;
    CHECK(rtl_read_tuner_regs(&dev, regs, 32) == 32 && regs[0] == 0x96 && regs[5] == f->treg[5], "tuner regs");
    CHECK(rtl_read_tuner_regs(&dev, regs, 33) == RTL_ERANGE && rtl_read_tuner_regs(&dev, regs, 0) == RTL_ERANGE,
          "tuner len");
    CHECK(rtl_read_demod_reg(&dev, 0, 0x19, &v) == 0 && v == 0x05, "demod read");
    CHECK(rtl_read_demod_reg(&dev, 5, 0, &v) == RTL_ERANGE, "page 5");
    CHECK(rtl_read_block_reg(&dev, 2, 0x3000, &v) == 0 && v == 0xE8, "sys read");
    CHECK(rtl_read_block_reg(&dev, 3, 0x3000, &v) == RTL_ERANGE, "block 3");
    /* standby (T1.11, D1.13) */
    rtl_close(&dev);
    CHECK(f->treg[0x06] == 0xB1 && f->treg[0x05] == 0xB3 && f->treg[0x19] == 0x0C && f->sys[0x00] == 0x20,
          "standby %02x %02x %02x", f->treg[0x06], f->treg[0x05], f->sys[0]);
    free(f);
}

static void test_r828d_inputs(void)
{
    /* the Blog V4: HF through the upconverter and GPIO5, then cable 1, then air (T1.12) */
    rtl_dev_t dev;
    fake_t *f = fake_new(0x74);
    CHECK(open_fake(&dev, f, false, true) == 0 && dev.tun.blog_v4, "%s", dev.error);
    CHECK(rtl_set_center_freq(&dev, 100000000) == 0, "%s", dev.error);
    CHECK((f->treg[0x05] & 0x60) == 0x60 && !(f->treg[0x06] & 0x08) && (f->sys[0x01] & 0x20) &&
              !(f->sys[0x04] & 0x20) && (f->sys[0x03] & 0x20),
          "VHF %02x %02x gpo %02x", f->treg[0x05], f->treg[0x06], f->sys[0x01]);
    CHECK(rtl_set_center_freq(&dev, 25000000) == 0, "%s", dev.error);
    CHECK((f->treg[0x05] & 0x60) == 0x20 && (f->treg[0x06] & 0x08) && !(f->sys[0x01] & 0x20), "HF");
    double lo = fake_lo(f, 28.8e6, NULL);
    CHECK(fabs(lo - (25e6 + 28.8e6 + 3.57e6)) < 500, "HF LO %.0f", lo);
    CHECK(fabs(fake_ddc_if(f, 28.8e6) - 3.57e6) < 500, "HF IF %.0f", fake_ddc_if(f, 28.8e6));
    CHECK(rtl_set_center_freq(&dev, 461000000) == 0 && (f->treg[0x05] & 0x60) == 0 && (f->sys[0x01] & 0x20), "UHF");
    /* the V4's RF table: 461 MHz on its 310 MHz row with OPEN_D 1 */
    CHECK((f->treg[0x17] & 0x08) && (f->treg[0x1A] & 0xC3) == 0x41, "V4 row");
    int before = f->n_log;
    CHECK(rtl_set_center_freq(&dev, 462000000) == 0, "same input");
    int gpio = 0;
    for (int i = before; i < f->n_log; i++)
        gpio += f->log[i].value == 0x3001;
    CHECK(gpio == 0, "GPIO untouched when the input stays");
    free(f);

    /* a plain R828D: cable 1 to 345 MHz, air above; no GPIO */
    f = fake_new(0x74);
    CHECK(open_fake(&dev, f, false, false) == 0 && !dev.tun.blog_v4, "%s", dev.error);
    CHECK(rtl_set_center_freq(&dev, 146000000) == 0 && (f->treg[0x05] & 0x60) == 0x60, "cable 1");
    CHECK(rtl_set_center_freq(&dev, 461000000) == 0 && (f->treg[0x05] & 0x60) == 0x00, "air");
    CHECK(f->sys[0x03] == 0 && f->sys[0x01] == 0, "no GPIO");
    CHECK(rtl_set_center_freq(&dev, 25000000) == 0 && fabs(fake_lo(f, 28.8e6, NULL) - 28.57e6) < 500, "no upconvert");
    free(f);

    /* blog_v4 on an R820T means nothing */
    f = fake_new(0x34);
    CHECK(open_fake(&dev, f, false, true) == 0 && !dev.tun.blog_v4, "%s", dev.error);
    free(f);
}

static void test_fir_packing(void)
{
    static const int16_t ours[16] = {3, -18, -31, -49, -68, -81, -80, -59, -13, 58, 151, 259, 370, 471, 546, 587};
    uint8_t b[20];
    int16_t back[16];
    CHECK(rtl_fir_pack(ours, b) == 0, "pack");
    rtl_fir_unpack(b, back);
    CHECK(!memcmp(back, ours, sizeof ours), "round trip");
    /* the default bytes W writes unpack to its smooth half low-pass (D1.9), and pack back */
    static const uint8_t w[20] = {0xCA, 0xDC, 0xD7, 0xD8, 0xE0, 0xF2, 0x0E, 0x35, 0x06, 0x50,
                                  0x9C, 0x0D, 0x71, 0x11, 0x14, 0x71, 0x74, 0x19, 0x41, 0xA5};
    static const int16_t wt[16] = {-54, -36, -41, -40, -32, -14, 14, 53, 101, 156, 215, 273, 327, 372, 404, 421};
    rtl_fir_unpack(w, back);
    CHECK(!memcmp(back, wt, sizeof wt), "default unpacks");
    CHECK(rtl_fir_pack(wt, b) == 0 && !memcmp(b, w, 20), "default packs");
    /* extremes and limits */
    int16_t ext[16] = {-128, 127, 0, 0, 0, 0, 0, 0, -2048, 2047, -1, 1, 0, 0, 0, 0};
    CHECK(rtl_fir_pack(ext, b) == 0, "extremes");
    rtl_fir_unpack(b, back);
    CHECK(!memcmp(back, ext, sizeof ext), "extremes round trip");
    ext[0] = 128;
    CHECK(rtl_fir_pack(ext, b) == -1, "c0 too big");
    ext[0] = 0;
    ext[15] = -2049;
    CHECK(rtl_fir_pack(ext, b) == -1, "c15 too small");
}

/* Every transfer of a call failing in turn: the call returns < 0 with dev->error set. */
typedef int (*call_fn)(rtl_dev_t *dev);
static int c_rate(rtl_dev_t *d) { return rtl_set_sample_rate(d, 288000); }
static int c_freq(rtl_dev_t *d) { return rtl_set_center_freq(d, 461162500); }
static int c_ppm(rtl_dev_t *d) { return rtl_set_freq_correction(d, 12); }
static int c_gain(rtl_dev_t *d) { return rtl_set_tuner_gain(d, 297); }
static int c_agc(rtl_dev_t *d) { return rtl_set_tuner_gain_mode(d, false); }
static int c_bias(rtl_dev_t *d) { return rtl_set_bias_tee(d, true); }
static int c_reset(rtl_dev_t *d) { return rtl_reset_buffer(d); }
static int c_regs(rtl_dev_t *d)
{
    uint8_t r[16];
    int n = rtl_read_tuner_regs(d, r, 16);
    return n == 16 ? 0 : n < 0 ? n : -99;
}

static void test_failures(void)
{
    /* rtl_open: count a clean open's transfers, then fail each */
    rtl_dev_t dev;
    fake_t *f = fake_new(0x74); /* the R828D: the probe at 0x34 fails on its own too */
    CHECK(open_fake(&dev, f, false, true) == 0, "%s", dev.error);
    const int n_open = f->count;
    static bool natural[LOG_MAX]; /* transfers that fail anyway: the probe at 0x34 */
    for (int k = 1; k <= n_open; k++)
        natural[k - 1] = f->log[k - 1].result < 0;
    free(f);
    int bad = 0;
    for (int k = 1; k <= n_open; k++) {
        if (natural[k - 1])
            continue;
        f = fake_new(0x74);
        f->fail_at = k;
        int r = open_fake(&dev, f, false, true);
        if (r >= 0 || !dev.error[0])
            bad++, printf("  open, transfer %d of %d: %d \"%s\"\n", k, n_open, r, dev.error);
        free(f);
    }
    CHECK(bad == 0, "open: %d of %d failures not reported", bad, n_open);

    static const struct {
        const char *name;
        call_fn fn;
    } calls[] = {{"rate", c_rate}, {"freq", c_freq}, {"ppm", c_ppm},     {"gain", c_gain},
                 {"agc", c_agc},   {"bias", c_bias}, {"reset", c_reset}, {"regs", c_regs}};
    for (int c = 0; c < (int)(sizeof calls / sizeof calls[0]); c++) {
        f = fake_new(0x74);
        CHECK(open_fake(&dev, f, false, true) == 0 && c_freq(&dev) == 0, "%s", dev.error);
        int start = f->count;
        CHECK(calls[c].fn(&dev) == 0, "%s clean: %s", calls[c].name, dev.error);
        const int n = f->count - start;
        free(f);
        bad = 0;
        for (int k = 1; k <= n; k++) {
            f = fake_new(0x74);
            open_fake(&dev, f, false, true);
            c_freq(&dev);
            dev.error[0] = 0;
            int errors = dev.errors;
            f->fail_at = f->count + k;
            int r = calls[c].fn(&dev);
            if (r >= 0 || !dev.error[0] || dev.errors != errors + 1)
                bad++, printf("  %s, transfer %d of %d: %d \"%s\"\n", calls[c].name, k, n, r, dev.error);
            free(f);
        }
        CHECK(bad == 0 && n > 0, "%s: %d of %d failures not reported", calls[c].name, bad, n);
    }
}

int main(void)
{
    test_request_encoding();
    test_init_writes();
    test_detection();
    test_pll();
    test_rates();
    test_ppm();
    test_gain();
    test_bias_tee_and_buffer();
    test_r828d_inputs();
    test_fir_packing();
    test_failures();
    printf("rtlsdr_tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
