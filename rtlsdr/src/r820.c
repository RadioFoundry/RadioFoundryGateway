// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The R820T / R820T2 / R828D tuner. Written from docs/spec/rtlsdr.md, section 2 (T1.n cited below).
 *
 * The register values and sequences are facts from Rafael's R820T2 Register
 * Description and from jtarrio/webrtlsdr @ 9b3f587 (src/rtlsdr/r8xx.ts,
 * r820t.ts, r828d.ts), Apache-2.0: Copyright 2024 Jacobo Tarrio Barreiro,
 * Copyright 2013 Google Inc. (LICENSES/Apache-2.0.txt). The code, its
 * structure and its tables' layout are this project's.
 */
#include "rtlsdr/r820.h"

#include <math.h>
#include <string.h>

/* A masked write: reg = (reg & ~mask) | (val & mask). */
typedef struct {
    uint8_t reg, val, mask;
} r820_wr_t;

/* T1.3: registers 0x05..0x1F at init. */
static const uint8_t init_regs[] = {
    0x83, 0x32, 0x75, 0xC0, 0x40, 0xD6, 0x6C, 0xF5, 0x63, 0x75, 0x68, /* 0x05..0x0F */
    0x64, 0x83, 0x80, 0x00, 0x0F, 0x00, 0xC0, 0x30, 0x48, 0xCC, 0x60, 0x00, 0x54, 0xAE, 0x4A, 0xC0, /* 0x10..0x1F */
};
_Static_assert(sizeof init_regs == 0x20 - 0x05, "registers 0x05..0x1F");

/* T1.4: before the calibration. */
static const r820_wr_t init_before_cal[] = {
    {0x0C, 0x00, 0x0F},
    {0x13, 0x31, 0x3F},
    {0x1D, 0x00, 0x38},
};

/* T1.4: after the calibration and the IF filter, in this order. */
static const r820_wr_t init_after_filter[] = {
    {0x07, 0x00, 0x80}, {0x06, 0x10, 0x30}, {0x1E, 0x40, 0x60}, {0x05, 0x00, 0x80}, {0x1F, 0x00, 0x80},
    {0x0F, 0x00, 0x80}, {0x19, 0x60, 0x60}, {0x1D, 0xE5, 0xC7}, {0x1C, 0x24, 0xF8}, {0x0D, 0x53, 0xFF},
    {0x0E, 0x75, 0xFF}, {0x05, 0x00, 0x60}, {0x06, 0x00, 0x08}, {0x11, 0x38, 0x08}, {0x17, 0x30, 0x30},
    {0x0A, 0x40, 0x60}, {0x1D, 0x00, 0x38}, {0x1C, 0x00, 0x04}, {0x06, 0x00, 0x40}, {0x1A, 0x30, 0x30},
    {0x1D, 0x18, 0x38}, {0x1C, 0x24, 0x04}, {0x1E, 0x0D, 0x1F}, {0x1A, 0x20, 0x30},
};

/* T1.11: standby, whole registers. */
static const r820_wr_t standby[] = {
    {0x06, 0xB1, 0xFF}, {0x05, 0xB3, 0xFF}, {0x07, 0x3A, 0xFF}, {0x08, 0x40, 0xFF}, {0x09, 0xC0, 0xFF}, {0x0A, 0x3A, 0xFF},
    {0x0C, 0x35, 0xFF}, {0x0F, 0x68, 0xFF}, {0x11, 0x03, 0xFF}, {0x17, 0xF4, 0xFF}, {0x19, 0x0C, 0xFF},
};

/* T1.6: the IF filter by rate. One table; measured values (G.2) replace the interim row. */
typedef struct {
    uint32_t min_rate, max_rate;
    uint32_t if_hz;
    uint8_t filt_bw;     /* R11[6:5]: 0 widest .. 3 narrowest */
    int8_t code_offset;  /* added to the calibrated R10[3:0] (0 widest .. 15 narrowest), clamped */
    uint8_t hpf;         /* R11[3:0]: 0 highest corner .. 15 lowest */
} r820_band_t;

static const r820_band_t bands[] = {
    /* Measured (G.2, R1.3): no setting comes near 300 kHz. The narrowest low-pass starts about 2.2 MHz above the IF;
     * HPF 2 brings the high-pass corner to about 2.7 MHz, under 1 dB down at IF - 150 kHz. About 3 MHz passed. */
    {225001, 300000, R820_IF_HZ, 3, 15, 2},
    {900001, 3200000, R820_IF_HZ, 3, 0, 11},  /* W's */
};
#define N_BANDS ((int)(sizeof bands / sizeof bands[0]))
#define BAND_DEFAULT 1

/* T1.7: the RF input filters by LO frequency: from (kHz), R23[3] OPEN_D, R26 [7:6] RFMUX + [1:0] RFFILT, R27. */
typedef struct {
    uint32_t from_khz;
    uint8_t open_d, r26, r27;
} r820_rf_t;

static const r820_rf_t rf_std[] = {
    {0, 1, 0x02, 0xDF},      {50000, 1, 0x02, 0xBE},  {55000, 1, 0x02, 0x8B},  {60000, 1, 0x02, 0x7B},
    {65000, 1, 0x02, 0x69},  {70000, 1, 0x02, 0x58},  {75000, 0, 0x02, 0x44},  {90000, 0, 0x02, 0x34},
    {110000, 0, 0x02, 0x24}, {140000, 0, 0x02, 0x14}, {180000, 0, 0x02, 0x13}, {250000, 0, 0x02, 0x11},
    {280000, 0, 0x02, 0x00}, {310000, 0, 0x41, 0x00}, {588000, 0, 0x40, 0x00},
};

static const r820_rf_t rf_blog_v4[] = {
    {0, 0, 0x02, 0xDF},      {2200, 1, 0x02, 0xDF},   {50000, 1, 0x02, 0xBE},  {55000, 1, 0x02, 0x8B},
    {60000, 1, 0x02, 0x7B},  {65000, 1, 0x02, 0x69},  {70000, 1, 0x02, 0x58},  {75000, 1, 0x02, 0x44},
    {85000, 0, 0x02, 0x44},  {90000, 0, 0x02, 0x34},  {110000, 0, 0x02, 0x24}, {112000, 1, 0x02, 0x24},
    {140000, 1, 0x02, 0x14}, {172000, 0, 0x02, 0x14}, {180000, 0, 0x02, 0x13}, {242000, 1, 0x02, 0x13},
    {250000, 1, 0x02, 0x11}, {280000, 1, 0x02, 0x00}, {310000, 1, 0x41, 0x00}, {588000, 1, 0x40, 0x00},
};

/* T1.10: the manual gain steps, lowest first, in tenths of a dB above the lowest: measured (G.3, R1.2) on an
 * R820T, a CW carrier's level at each LNA code (mixer 0) and each mixer code (LNA 0), averaged over 230.4, 460.8
 * and 864 MHz. LNA and mixer alternate as W's steps do; mixer codes above 12 add nothing, so the last steps are the
 * LNA's alone, and a step that adds under 0.3 dB is left out. The VGA at code 8 (16 dB, RD). */
static const r820_gain_step_t gain_steps[] = {
    {0, 0, 0, 8},     {32, 1, 0, 8},    {45, 1, 1, 8},    {77, 2, 1, 8},    {90, 2, 2, 8},    {126, 3, 2, 8},
    {141, 3, 3, 8},   {166, 4, 3, 8},   {178, 4, 4, 8},   {191, 5, 4, 8},   {205, 5, 5, 8},   {236, 6, 5, 8},
    {251, 6, 6, 8},   {278, 7, 6, 8},   {291, 7, 7, 8},   {319, 8, 7, 8},   {334, 8, 8, 8},   {348, 9, 8, 8},
    {362, 9, 9, 8},   {378, 10, 9, 8},  {391, 10, 10, 8}, {401, 11, 10, 8}, {414, 11, 11, 8}, {422, 12, 11, 8},
    {436, 12, 12, 8}, {447, 13, 12, 8}, {462, 14, 12, 8},
};
#define N_GAIN_STEPS ((int)(sizeof gain_steps / sizeof gain_steps[0]))

#define VCO_MIN_HZ 1770000000.0 /* T1.8: SEL_DIV keeps the VCO from here to twice this */
#define CAL_LO_HZ 56000000.0    /* T1.5 */
#define BLOG_V4_UPCONVERT_HZ 28800000.0 /* T1.12 */
#define R828D_AIR_ABOVE_HZ 345000000.0
#define BLOG_V4_UHF_FROM_HZ 250000000.0

uint8_t r820_bitrev(uint8_t b)
{
    b = (uint8_t)((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = (uint8_t)((b & 0xCC) >> 2 | (b & 0x33) << 2);
    return (uint8_t)((b & 0xAA) >> 1 | (b & 0x55) << 1);
}

/* ── register access (T1.2) ─────────────────────────────────────────────── */

static int put(r820_t *t, uint8_t reg, uint8_t val, uint8_t mask)
{
    uint8_t v = (uint8_t)((t->regs[reg] & ~mask) | (val & mask));
    t->regs[reg] = v;
    return t->bus.write(t->bus.ctx, t->addr, reg, &v, 1) < 0 ? R820_EIO : 0;
}

static int put_list(r820_t *t, const r820_wr_t *w, int n)
{
    for (int i = 0; i < n; i++)
        if (put(t, w[i].reg, w[i].val, w[i].mask))
            return R820_EIO;
    return 0;
}

static int put_block(r820_t *t, uint8_t first, int n)
{
    return t->bus.write(t->bus.ctx, t->addr, first, &t->regs[first], n) < 0 ? R820_EIO : 0;
}

int r820_read(r820_t *t, uint8_t *out, int n)
{
    if (t->bus.read(t->bus.ctx, t->addr, out, n) < 0)
        return R820_EIO;
    for (int i = 0; i < n; i++)
        out[i] = r820_bitrev(out[i]);
    return 0;
}

static void nap(r820_t *t, int ms)
{
    if (t->bus.sleep_ms)
        t->bus.sleep_ms(t->bus.ctx, ms);
}

/* ── detection (T1.1) ───────────────────────────────────────────────────── */

r820_chip_t r820_probe(const r820_bus_t *bus)
{
    static const struct {
        uint8_t addr;
        r820_chip_t chip;
    } where[] = {{R820_ADDR_R820T, R820_R820T}, {R820_ADDR_R828D, R820_R828D}};
    for (int i = 0; i < 2; i++) {
        uint8_t id = 0;
        if (bus->read(bus->ctx, where[i].addr, &id, 1) == 0 && id == R820_CHIP_ID)
            return where[i].chip;
    }
    return R820_NONE;
}

/* ── the PLL (T1.8) ─────────────────────────────────────────────────────── */

int r820_pll_plan(double xtal, double lo_hz, r820_pll_t *out)
{
    if (!(lo_hz > 0) || !(xtal > 0))
        return R820_ERANGE;
    int sel = 0;
    while (sel < 6 && lo_hz * (double)(2u << (sel + 1)) <= 2 * VCO_MIN_HZ)
        sel++;
    double vco = lo_hz * (double)(2u << sel);
    double n = vco / (2 * xtal);
    double whole = floor(n);
    long sdm = lround((n - whole) * 65536.0);
    long nint = (long)whole;
    if (sdm >= 65536) {
        nint++;
        sdm -= 65536;
    }
    if (nint < 13 || nint > 63)
        return R820_ERANGE;
    out->sel_div = (uint8_t)sel;
    out->nint = (uint8_t)nint;
    out->sdm = (uint16_t)sdm;
    out->vco_hz = 2 * xtal * ((double)nint + (double)sdm / 65536.0);
    out->lo_hz = out->vco_hz / (double)(2u << sel);
    return 0;
}

static int pll_locked(r820_t *t, bool *locked)
{
    for (int i = 1; i <= 4; i++) {
        uint8_t st[3];
        if (r820_read(t, st, 3))
            return R820_EIO;
        if (st[2] & 0x40) {
            *locked = true;
            return 0;
        }
        nap(t, 5 * i);
    }
    *locked = false;
    return 0;
}

static int pll_set(r820_t *t, double lo_hz)
{
    r820_pll_t p;
    if (r820_pll_plan(t->xtal, lo_hz, &p))
        return R820_ERANGE;
    if (put(t, 0x1A, 0x00, 0x0C)) /* PLL_AUTO_CLK 128 kHz while it settles */
        return R820_EIO;
    const unsigned ni = (p.nint - 13u) / 4u, si = (p.nint - 13u) % 4u;
    t->regs[0x10] = (uint8_t)((t->regs[0x10] & 0x0F) | p.sel_div << 5);     /* SEL_DIV, REF_DIV2 0 */
    t->regs[0x12] = (uint8_t)((t->regs[0x12] & 0x17) | 0x80 | (p.sdm ? 0 : 0x08)); /* VCO current 4, PW_SDM */
    t->regs[0x14] = (uint8_t)(si << 6 | ni);
    t->regs[0x15] = (uint8_t)(p.sdm & 0xFF);
    t->regs[0x16] = (uint8_t)(p.sdm >> 8);
    if (put_block(t, 0x10, 7))
        return R820_EIO;
    bool locked;
    if (pll_locked(t, &locked))
        return R820_EIO;
    if (!locked) {
        if (put(t, 0x12, 0x60, 0xE0) || pll_locked(t, &locked)) /* more VCO current, once */
            return R820_EIO;
        if (!locked)
            return R820_ENOLOCK;
    }
    if (put(t, 0x1A, 0x08, 0x0C)) /* 8 kHz once locked */
        return R820_EIO;
    t->pll = p;
    return 0;
}

/* ── the IF filter (T1.5, T1.6) ─────────────────────────────────────────── */

static int calibrate(r820_t *t)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        if (put(t, 0x0B, 0x60, 0x60) || put(t, 0x0F, 0x04, 0x04) || put(t, 0x10, 0x00, 0x03))
            return R820_EIO;
        int r = pll_set(t, CAL_LO_HZ);
        if (r)
            return r;
        if (put(t, 0x0B, 0x10, 0x10) || put(t, 0x0B, 0x00, 0x10) || put(t, 0x0F, 0x00, 0x04))
            return R820_EIO;
        uint8_t st[5];
        if (r820_read(t, st, 5))
            return R820_EIO;
        uint8_t code = st[4] & 0x0F;
        if (code == 0x0F)
            code = 0;
        t->cal = code;
        if (code == 0)
            break;
    }
    return 0;
}

int r820_band_for_rate(uint32_t rate, uint32_t *if_hz)
{
    for (int i = 0; i < N_BANDS; i++)
        if (rate >= bands[i].min_rate && rate <= bands[i].max_rate) {
            if (if_hz)
                *if_hz = bands[i].if_hz;
            return i;
        }
    return -1;
}

static int write_band(r820_t *t, int b)
{
    int code = t->cal + bands[b].code_offset;
    code = code < 0 ? 0 : code > 15 ? 15 : code;
    if (put(t, 0x0A, (uint8_t)(0x10 | code), 0x1F) || put(t, 0x0B, (uint8_t)(bands[b].filt_bw << 5 | bands[b].hpf), 0xEF))
        return R820_EIO;
    t->band = b;
    return 0;
}

int r820_set_rate(r820_t *t, uint32_t rate)
{
    int b = r820_band_for_rate(rate, NULL);
    if (b < 0)
        return R820_ERANGE;
    return b == t->band ? 0 : write_band(t, b);
}

/* ── init ───────────────────────────────────────────────────────────────── */

int r820_init(r820_t *t, const r820_bus_t *bus, r820_chip_t chip, bool blog_v4, double xtal, uint32_t rate)
{
    memset(t, 0, sizeof *t);
    t->bus = *bus;
    t->chip = chip;
    t->addr = chip == R820_R828D ? R820_ADDR_R828D : R820_ADDR_R820T;
    t->blog_v4 = blog_v4 && chip == R820_R828D;
    t->xtal = xtal;
    t->band = -1;
    t->input = -1;
    for (int i = 0; i < N_GAIN_STEPS && i < R820_GAIN_STEPS_MAX; i++)
        t->gains[t->n_gains++] = gain_steps[i].tenths;
    int b = r820_band_for_rate(rate, NULL);
    if (b < 0)
        b = BAND_DEFAULT;

    memcpy(&t->regs[0x05], init_regs, sizeof init_regs);
    if (put_block(t, 0x05, (int)sizeof init_regs))
        return R820_EIO;
    nap(t, 5);
    if (put_list(t, init_before_cal, (int)(sizeof init_before_cal / sizeof init_before_cal[0])))
        return R820_EIO;
    int r = calibrate(t);
    if (r)
        return r;
    if (write_band(t, b))
        return R820_EIO;
    return put_list(t, init_after_filter, (int)(sizeof init_after_filter / sizeof init_after_filter[0]));
}

void r820_set_xtal(r820_t *t, double xtal) { t->xtal = xtal; }

/* ── tuning (T1.7, T1.9, T1.12) ─────────────────────────────────────────── */

static const r820_rf_t *rf_row(const r820_t *t, double lo_hz)
{
    const r820_rf_t *tab = t->blog_v4 ? rf_blog_v4 : rf_std;
    const int n = t->blog_v4 ? (int)(sizeof rf_blog_v4 / sizeof rf_blog_v4[0]) : (int)(sizeof rf_std / sizeof rf_std[0]);
    const double khz = lo_hz / 1000.0;
    int i = 0;
    while (i + 1 < n && khz >= tab[i + 1].from_khz)
        i++;
    return &tab[i];
}

static int set_input(r820_t *t, double asked_hz)
{
    if (t->chip != R820_R828D)
        return 0;
    int in;
    if (t->blog_v4)
        in = asked_hz < BLOG_V4_UPCONVERT_HZ ? 2 : asked_hz < BLOG_V4_UHF_FROM_HZ ? 1 : 0;
    else
        in = asked_hz > R828D_AIR_ABOVE_HZ ? 0 : 1;
    if (in == t->input)
        return 0;
    int r;
    if (!t->blog_v4)
        r = put(t, 0x05, in ? 0x60 : 0x00, 0x60);
    else if (in == 0)
        r = put(t, 0x06, 0x00, 0x08) || put(t, 0x05, 0x00, 0x60);
    else if (in == 1)
        r = put(t, 0x06, 0x00, 0x08) || put(t, 0x05, 0x60, 0x60);
    else
        r = put(t, 0x06, 0x08, 0x08) || put(t, 0x05, 0x20, 0x60);
    if (r)
        return R820_EIO;
    t->input = in;
    return 0;
}

int r820_tune(r820_t *t, double rf_hz)
{
    const bool hf = t->blog_v4 && rf_hz < BLOG_V4_UPCONVERT_HZ;
    const double f = rf_hz + (hf ? BLOG_V4_UPCONVERT_HZ : 0);
    const double lo = f + bands[t->band < 0 ? BAND_DEFAULT : t->band].if_hz;
    r820_pll_t p;
    if (r820_pll_plan(t->xtal, lo, &p))
        return R820_ERANGE;
    const r820_rf_t *row = rf_row(t, lo);
    if (put(t, 0x17, (uint8_t)(row->open_d << 3), 0x08) || put(t, 0x1A, row->r26, 0xC3) || put(t, 0x1B, row->r27, 0xFF) ||
        put(t, 0x10, 0x00, 0x0B) || put(t, 0x08, 0x00, 0x3F) || put(t, 0x09, 0x00, 0x3F))
        return R820_EIO;
    int r = pll_set(t, lo);
    if (r)
        return r;
    t->rf_hz = f;
    t->hf = hf;
    return set_input(t, rf_hz);
}

double r820_if_hz(const r820_t *t)
{
    if (t->pll.lo_hz > 0 && t->rf_hz > 0)
        return t->pll.lo_hz - t->rf_hz;
    return bands[t->band < 0 ? BAND_DEFAULT : t->band].if_hz;
}

/* ── gain (T1.10) ───────────────────────────────────────────────────────── */

const r820_gain_step_t *r820_gain_table(int *n)
{
    *n = N_GAIN_STEPS;
    return gain_steps;
}

int r820_set_gain_step(r820_t *t, int step)
{
    if (step < 0 || step >= N_GAIN_STEPS)
        return R820_ERANGE;
    const r820_gain_step_t *g = &gain_steps[step];
    /* LNA manual + code, mixer manual + code, VGA from its code; R12[7] (fixed 1) left alone */
    if (put(t, 0x05, (uint8_t)(0x10 | g->lna), 0x1F) || put(t, 0x07, g->mix, 0x1F) || put(t, 0x0C, g->vga, 0x1F))
        return R820_EIO;
    return 0;
}

int r820_set_agc(r820_t *t)
{
    if (put(t, 0x05, 0x00, 0x10) || put(t, 0x07, 0x10, 0x10) || put(t, 0x0C, 0x0B, 0x1F))
        return R820_EIO;
    return 0;
}

int r820_standby(r820_t *t) { return put_list(t, standby, (int)(sizeof standby / sizeof standby[0])); }
