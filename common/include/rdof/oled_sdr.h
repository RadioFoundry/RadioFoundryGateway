// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The OLED's screen for a board running an SDR (oled.h has the frame, the font
 * and the roamer's screen), and the board's oled settings, read from the box
 * config whatever radios it runs.
 *
 *     ((( 192.0.2.8                    the network: Wi-Fi bars, the address
 *     MQ HB SDR          12:41:07      health: inverted = up, struck through = down
 *     ----------------------------
 *     RX 4 13                          a badge per channel, inverted while it hears a call
 *      4 T9<9990123                    the calls being heard (then "+N more"); with none,
 *     13 P9990456<9990123              each channel: badge, MHz, name
 *     ----------------------------
 *     461.0475 288k cpu 19%            the tuner: center, rate, DSP load (or a roamer's TX row)
 */
#ifndef RDOF_OLED_SDR_INCLUDED
#define RDOF_OLED_SDR_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/oled.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A health flag: not shown, down or up. */
typedef enum { RDOF_OLED_NONE = -1, RDOF_OLED_DOWN = 0, RDOF_OLED_UP = 1 } rdof_oled_flag_t;

typedef struct {
    const char *id;   /* the receiver id (SDR04): its badge */
    const char *name; /* "" or NULL: none */
    double freq;      /* Hz */
    bool in;          /* in the tuner's window (a badge out of it is struck through) */
    bool active;      /* a call is being heard */
    uint32_t src, dst;
    bool group;
} rdof_oled_channel_t;

typedef struct {
    bool wifi, ap;    /* ap: the board's own access point */
    int wifi_dbm;     /* the station's signal; 0: not known */
    const char *ip;   /* "" or NULL: none */
    rdof_oled_flag_t mqtt, hblink, sdr, radio; /* radio: a roamer's or repeater's modem (HAT) */
    const char *clock; /* "23:41:07" or "" */
    const rdof_oled_channel_t *ch;
    int nch;
    const char *error; /* the front end's error ("" none): shown in place of the calls */
    bool receiving;
    double center_hz, sample_rate, cpu_load; /* cpu_load: 0..1, NAN not known */
    /* A roamer or repeater beside the SDR: its TX row in place of the tuner's line. */
    bool has_tx, tx_active;
    const char *tx_line; /* the call it's sending, or "idle 467.375" */
} rdof_oled_sdr_t;

void rdof_oled_render_sdr(rdof_frame_buf_t *f, const rdof_oled_sdr_t *s);

/* A channel's badge: the receiver id's trailing number (SDR04 -> "4"), its first letter before the number for
   other ids (T1 -> "T1", ROAM2 -> "R2"), else its first three characters. Returns out. */
char *rdof_oled_badge(const char *id, char out[8]);
/* Wi-Fi bars for a signal: 0 to 4 (-55, -65, -75, -85 dBm and up), -1 for none (0 dBm). */
int rdof_oled_bars(int dbm);
/* The bottom line: "461.0475 288k cpu 19%", or "starting" while it isn't receiving. */
size_t rdof_oled_tuner_line(const rdof_oled_sdr_t *s, char *out, size_t cap);

/* The box config's "oled": on, its address and panel, and its pins. */
typedef struct {
    bool on;
    bool set;        /* the settings have the key (else the board's default decides) */
    int address;     /* 7-bit, 0x3C */
    bool sh1106, rotate180;
    double refresh_s; /* 0.5 */
    int sda, scl;    /* -1: the board's */
} rdof_oled_settings_t;

/* From the box config (or a plain SDR config): "oled" is an object, true, or null / false (off); address,
   sh1106 and rotate180 also under "i2c". false with error set when a value isn't a number. */
bool rdof_oled_settings_parse(const char *text, size_t len, rdof_oled_settings_t *out, char *error, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
