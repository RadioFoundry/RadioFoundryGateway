// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The Rafael R820T / R820T2 / R828D tuner, over an I2C hook (the RTL2832U's
 * repeater in rtl2832u.c, a fake in the host tests). Written from the fact
 * spec docs/spec/rtlsdr.md, section 2; the T1.n there are cited in r820.c.
 *
 * Register values and sequences come from Rafael's R820T2 Register
 * Description and from jtarrio/webrtlsdr (Apache-2.0; Copyright 2024 Jacobo
 * Tarrio Barreiro, Copyright 2013 Google Inc.), as facts; the code is this
 * project's.
 */
#ifndef RTLSDR_R820_H
#define RTLSDR_R820_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define R820_ADDR_R820T 0x34 /* I2C write address (T1.1) */
#define R820_ADDR_R828D 0x74
#define R820_CHIP_ID 0x69    /* register 0 as the RTL hands it over (0x96, bits reversed) */
#define R820_IF_HZ 3570000u  /* T1.6 */
#define R820_NREGS 32
#define R820_GAIN_STEPS_MAX 32

/* Errors (negative returns). */
#define R820_EIO (-1)     /* an I2C transfer failed */
#define R820_ERANGE (-3)  /* a frequency or rate the tuner can't take */
#define R820_ENOLOCK (-5) /* the PLL didn't lock */

typedef enum { R820_NONE = 0, R820_R820T, R820_R828D } r820_chip_t;

typedef struct {
    /* Write n values to registers reg, reg+1, ... of the tuner at addr; 0 or < 0. */
    int (*write)(void *ctx, uint8_t addr, uint8_t reg, const uint8_t *vals, int n);
    /* Read n bytes from register 0 up, as the bus hands them over (bits not reversed); 0 or < 0. */
    int (*read)(void *ctx, uint8_t addr, uint8_t *out, int n);
    void (*sleep_ms)(void *ctx, int ms);
    void *ctx;
} r820_bus_t;

/* One PLL setting (T1.8). */
typedef struct {
    uint8_t sel_div; /* LO = VCO / 2^(sel_div + 1) */
    uint8_t nint;    /* 13..63 */
    uint16_t sdm;    /* the fraction in 2^-16 */
    double vco_hz, lo_hz; /* what that gives */
} r820_pll_t;

/* One manual gain step: its nominal gain and its codes (T1.10). */
typedef struct {
    int16_t tenths; /* tenths of a dB above the lowest step: to measure (G.3) */
    uint8_t lna, mix, vga;
} r820_gain_step_t;

typedef struct {
    r820_bus_t bus;
    r820_chip_t chip;
    uint8_t addr;
    bool blog_v4;          /* an R828D on an RTL-SDR Blog V4: its inputs and upconverter (T1.12) */
    double xtal;           /* the crystal with the ppm applied */
    uint8_t regs[R820_NREGS]; /* 0x05..0x1F as last written */
    uint8_t cal;           /* the IF filter's calibrated code (T1.5) */
    int band;              /* the IF filter row in use (T1.6) */
    int input;             /* R828D: 0 air, 1 cable 1, 2 cable 2 (HF); -1 not set yet */
    bool hf;               /* the Blog V4's upconverter is in the path (GPIO5 low) */
    r820_pll_t pll;        /* the last PLL setting */
    double rf_hz;          /* the RF the tuner was set to (with the upconverter's 28.8 MHz added) */
    int gains[R820_GAIN_STEPS_MAX]; /* the steps' tenths, for rtl_tuner_gains */
    int n_gains;
} r820_t;

/* Which tuner answers on the bus (the repeater open): R820_NONE if none. */
r820_chip_t r820_probe(const r820_bus_t *bus);
/* Init (T1.3 to T1.6) for a probed chip, with the IF filter for `rate`. 0, or < 0. */
int r820_init(r820_t *t, const r820_bus_t *bus, r820_chip_t chip, bool blog_v4, double xtal, uint32_t rate);
/* The crystal with the ppm applied; takes effect at the next tune. */
void r820_set_xtal(r820_t *t, double xtal);
/* The IF filter for a sample rate (T1.6): writes it if the row changes. 0, or < 0. */
int r820_set_rate(r820_t *t, uint32_t rate);
/* Tune to rf_hz (T1.9). 0, or < 0. Afterward t->pll.lo_hz - t->rf_hz is the IF the tuner really gives. */
int r820_tune(r820_t *t, double rf_hz);
/* The IF the last tune gives (LO - RF), or the nominal before any tune. */
double r820_if_hz(const r820_t *t);
/* Manual gain at step i of the table, or the tuner's AGC (T1.10). 0, or < 0. */
int r820_set_gain_step(r820_t *t, int step);
int r820_set_agc(r820_t *t);
/* Power down (T1.11). */
int r820_standby(r820_t *t);
/* Read registers 0..n-1 with their bits in order (T1.2). 0, or < 0. */
int r820_read(r820_t *t, uint8_t *out, int n);

/* The PLL setting for an LO at a crystal (pure; T1.8). 0, or R820_ERANGE. */
int r820_pll_plan(double xtal, double lo_hz, r820_pll_t *out);
/* The gain table: its steps, lowest first. */
const r820_gain_step_t *r820_gain_table(int *n);
/* The IF filter's settings for a rate (T1.6): which row, and its IF; -1 for a rate no row covers. */
int r820_band_for_rate(uint32_t rate, uint32_t *if_hz);
/* Bit order of a byte reversed (the tuner sends LSB first). */
uint8_t r820_bitrev(uint8_t b);

#ifdef __cplusplus
}
#endif

#endif
