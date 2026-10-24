// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * An RTL2832U (RTL-SDR) driver: the demodulator's baseband, the resampler,
 * frequency correction, GPIO / bias tee and streaming control, and the
 * R820T / R820T2 / R828D tuner behind the RTL's I2C repeater.
 *
 * Portable C over one hook, a vendor control transfer (rtl_io_t), so the
 * register logic runs the same on an ESP32-S3 (esp/rtl_usb.c) and on a host
 * against a fake device (host-tests/test_rtl.c). The bulk sample stream
 * is the platform's (endpoint 0x81).
 *
 * Written from the fact spec docs/spec/rtlsdr.md (the RTL2832U datasheet, Rafael's R820T2 register
 * description, jtarrio/webrtlsdr under Apache-2.0, our bench findings): src/rtl2832u.c (D1.n) and the tuner in
 * src/r820.c (T1.n).
 */
#ifndef RTLSDR_RTL_H
#define RTLSDR_RTL_H

#include <stdbool.h>
#include <stdint.h>

#include "rtlsdr/r820.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RTL_VID 0x0BDA
#define RTL_PID_2838 0x2838
#define RTL_PID_2832 0x2832
#define RTL_BULK_EP 0x81
#define RTL_XTAL_HZ 28800000u

typedef struct {
    /* One vendor request (bRequest 0) to the device: in = device to host. Returns the bytes moved, < 0 on error. */
    int (*control)(void *ctx, bool in, uint16_t value, uint16_t index, uint8_t *data, uint16_t len);
    void (*sleep_ms)(void *ctx, int ms);
    void *ctx;
} rtl_io_t;

typedef enum { RTL_TUNER_NONE = 0, RTL_TUNER_R820T, RTL_TUNER_R828D } rtl_tuner_t;

typedef struct {
    rtl_io_t io;
    bool full_speed;       /* the bulk endpoint's FIFO is set to 64-byte packets, not 512 */
    bool blog_v4;          /* RTL-SDR Blog V4 (R828D on the 28.8 MHz clock) */
    uint32_t rtl_xtal, tun_xtal; /* nominal; ppm applied when used */
    int ppm;
    bool fixed_sample_rate; /* the correction leaves the sample frequency register at 0: only the LO and IF move */
    uint32_t rate;         /* the resampler's real rate */
    uint32_t freq;         /* the asked center frequency */
    rtl_tuner_t tuner;
    r820_t tun;            /* the tuner's state */
    int gpio5;             /* Blog V4: the upconverter's GPIO5 level as last set, -1 not set */
    int gain;              /* tenths of a dB (manual) */
    bool manual_gain;
    bool bias_tee;
    int errors;            /* failed transfers so far */
    char error[96];        /* the last failure, for the log */
} rtl_dev_t;

/* Errors: every call returns 0, or < 0 with dev->error set: -1 / -2 a control write / read failed, -3 an argument
 * out of range, -4 no R820T / R828D tuner answered, -5 the tuner's PLL didn't lock, -6 the tuner's init failed. */
#define RTL_EWRITE (-1)
#define RTL_EREAD (-2)
#define RTL_ERANGE (-3)
#define RTL_ENOTUNER (-4)
#define RTL_ENOLOCK (-5)
#define RTL_ETUNERINIT (-6)

/* Baseband up, tuner found and initialized (AGC on, no rate or frequency set yet). */
int rtl_open(rtl_dev_t *dev, const rtl_io_t *io, bool full_speed, bool blog_v4);
/* Power down the tuner and the demodulator. */
void rtl_close(rtl_dev_t *dev);

/* 225001..300000 or 900001..3200000 S/s. Sets the tuner's IF filter to the rate and re-tunes. dev->rate is
 * the real rate (equal for 28.8 MHz / rate with a 2^22 ratio, e.g. 288000, 240000, 250000). */
int rtl_set_sample_rate(rtl_dev_t *dev, uint32_t rate);
/* 24 MHz to 1766 MHz. */
int rtl_set_center_freq(rtl_dev_t *dev, uint32_t hz);
/* Crystal error in ppm (whole ppm, -400..400, as the chip's correction registers take); positive for a fast
 * crystal. Moves the tuner's LO and the DDC's IF, and unless fixed_sample_rate, the sample frequency register. */
int rtl_set_freq_correction(rtl_dev_t *dev, int ppm);
/* manual false: the tuner's own AGC. */
int rtl_set_tuner_gain_mode(rtl_dev_t *dev, bool manual);
/* Tenths of a dB: the nearest step at or below (rtl_tuner_gains). */
int rtl_set_tuner_gain(rtl_dev_t *dev, int tenths);
/* The tuner's gain steps in tenths of a dB; returns how many. */
int rtl_tuner_gains(const rtl_dev_t *dev, const int **gains);
/* The 4.5 V bias tee on GPIO0 (RTL-SDR Blog V3 / V4). */
int rtl_set_bias_tee(rtl_dev_t *dev, bool on);
/* Flush the sample FIFO; call before streaming. */
int rtl_reset_buffer(rtl_dev_t *dev);
/* Reads only, for looking at the chips: the tuner's registers 0..len-1 (through the I2C repeater, so not while
 * another tuner call runs; returns len, < 0 on failure), a demod page's register, or a USB / SYS block's (block 1, 2). */
int rtl_read_tuner_regs(rtl_dev_t *dev, uint8_t *out, int len);
int rtl_read_demod_reg(rtl_dev_t *dev, int page, uint8_t addr, uint8_t *val);
int rtl_read_block_reg(rtl_dev_t *dev, int block, uint16_t addr, uint8_t *val);

/* What the sample frequency register's formula says a crystal correction of ppm takes out of the sample rate, in
 * ppm (ppm itself, to the register's step of 2^-24; docs/spec/rtlsdr.md D1.8). Measured on an R820T dongle it took
 * out about a third of that (+10 moved the rate -3.4 ppm), so fixed_sample_rate leaves the register at 0 where the
 * rate matters. */
double rtl_sample_correction_ppm(int ppm);
/* The resampler ratio for a rate (what rtl_set_sample_rate writes, 28 bits; D1.7) and the real rate it gives. */
uint32_t rtl_resampler_ratio(uint32_t xtal, uint32_t rate, double *real_rate);
const char *rtl_tuner_name(rtl_tuner_t t);
/* The DDC FIR's 16 coefficients to its 20 register bytes and back (D1.9). Pack returns -1 for a coefficient out
 * of its field (c0..c7: -128..127, c8..c15: -2048..2047). */
int rtl_fir_pack(const int16_t taps[16], uint8_t out[20]);
void rtl_fir_unpack(const uint8_t in[20], int16_t taps[16]);

#ifdef __cplusplus
}
#endif

#endif
