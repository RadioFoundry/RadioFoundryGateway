// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * An SDR front end: where the receiver's samples come from (and, for one that
 * can transmit, where a transmitter's go). The receive path (channelizer,
 * demods, trackers) doesn't care which: it takes complex float at a rate that's
 * a multiple of 48 kS/s, around a center frequency.
 *
 * A front end is a table of operations over a context of its own: the
 * RTL-SDR on USB, the self-test clip in a loop ("sim"), an LR2021 on SPI;
 * later an SX1255. The platform implements each (they block, use the USB host,
 * SPI, timers); this header is portable C, so the logic around it (which front
 * end a setting means, whether a plan fits its capabilities, the sample
 * conversion) runs and is tested on the host.
 *
 * Samples come in the front end's native format (rdof_fe_format_t) and are
 * converted to float once (rdof_fe_convert). Whole ppm go to the chip
 * (set_ppm); what's left of a learned error goes into the channels' AFC
 * corrections, as the RTL has always done.
 */
#ifndef RDOF_SDR_FRONTEND_H
#define RDOF_SDR_FRONTEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/sdr/fft.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Which front end the settings ask for (device.type; "sim": true in the SDR's section is RDOF_FE_SIM). */
typedef enum { RDOF_FE_RTLSDR = 0, RDOF_FE_LR2021, RDOF_FE_SIM, RDOF_FE_SX1255, RDOF_FE_TYPES } rdof_fe_type_t;

/* device.type's word for t ("rtlsdr", "lr2021", "sim", "sx1255"). */
const char *rdof_fe_type_name(rdof_fe_type_t t);
/* The type a word names; -1 for none. NULL or "" is the default, rtlsdr. */
int rdof_fe_type_parse(const char *s);

/* Native sample formats, interleaved I then Q. */
typedef enum {
    RDOF_FE_CU8 = 0, /* unsigned 8-bit, 127.5 is zero (rtl_sdr) */
    RDOF_FE_CS8,     /* signed 8-bit */
    RDOF_FE_CS16,    /* signed 16-bit, little-endian (host order) */
} rdof_fe_format_t;

/* Bytes per complex sample. */
int rdof_fe_sample_bytes(rdof_fe_format_t f);
/* n complex samples from raw (n * rdof_fe_sample_bytes bytes) to float in full-scale units. */
void rdof_fe_convert(rdof_fe_format_t f, const void *raw, size_t n, rdof_cf_t *out);
/* The bytes that read back as zero signal (cu8: 0x80 bytes, near 127.5; the others 0), for filler. */
uint8_t rdof_fe_fill_byte(rdof_fe_format_t f);

/* Transmit: none, half duplex (one chain, switched), or full duplex (separate chains, own frequency). */
typedef enum { RDOF_FE_RX_ONLY = 0, RDOF_FE_HALF_DUPLEX, RDOF_FE_FULL_DUPLEX } rdof_fe_duplex_t;

/* How a front end takes what it transmits. */
typedef enum {
    RDOF_FE_TX_NONE = 0,
    RDOF_FE_TX_IQ,     /* I/Q at tx_rate in tx_format (sdr's DMR modulator makes it) */
    RDOF_FE_TX_FREQ,   /* instantaneous frequency deviation samples, int16 Hz, at tx_rate (an FM-capable chip) */
    RDOF_FE_TX_4FSK,   /* DMR symbols (2 bits each, 4800 a second); the chip shapes and modulates */
} rdof_fe_tx_mode_t;

typedef struct {
    const char *kind;           /* "RTL-SDR", "LR2021", ... for logs and the page */
    rdof_fe_format_t format;
    /* rates it can stream: any multiple of 48000 from rate_min to rate_max, or only rates[] when n_rates > 0 */
    double rate_min, rate_max;
    const double *rates;
    int n_rates;
    double freq_min, freq_max;  /* Hz it can tune */
    double usable;              /* the share of the rate its filters pass flat: channels go within +- usable/2 */
    double dc_guard_hz;         /* a channel this close to the center may be hurt by a DC spike (0: none) */
    bool gain;                  /* it has a settable gain (gain_steps) */
    bool usb;                   /* it's on the board's USB host port */
    double link_bytes_per_s;    /* what its link carries a second, for the page (0: format at the rate) */
    /* transmit */
    rdof_fe_duplex_t duplex;
    rdof_fe_tx_mode_t tx_mode;
    rdof_fe_format_t tx_format;
    double tx_rate;             /* S/s of tx_mode RDOF_FE_TX_IQ / _FREQ; 0: the receive rate */
    double tx_freq_min, tx_freq_max;
    double tx_power_min, tx_power_max, tx_power_step; /* dBm */
    double turnaround_us;       /* half duplex: RX to TX (or back) and settled */
} rdof_fe_caps_t;

/*
 * What a front end transmits: it pulls samples from the source as it needs them, n at a time, counting from the
 * stream's first sample (the receiver's sample index, scaled by tx_rate / rate when they differ). An RDOF_FE_TX_IQ
 * front end calls pull_iq (complex baseband at tx_rate, the carrier where tx_tune put it), an RDOF_FE_TX_FREQ one
 * pull_freq (Hz and an envelope 0..1). Each returns whether any of the n samples is keyed.
 */
typedef struct {
    bool (*pull_iq)(void *ctx, int64_t pos, rdof_cf_t *iq, int n);
    bool (*pull_freq)(void *ctx, int64_t pos, float *hz, float *env, int n);
    void *ctx;
} rdof_fe_tx_source_t;

typedef struct {
    uint64_t bytes;           /* native bytes in since the stream started */
    uint32_t errors;          /* failed reads or transfers */
    uint32_t short_reads;     /* reads or transfers that came back short */
    uint32_t overflows;       /* data dropped because the ring was full (sim: chunks skipped for being late) */
    uint64_t bytes_lost;      /* in those */
    uint64_t bytes_filled;    /* filler read in their place */
    uint64_t ring_peak;
} rdof_fe_stats_t;

/*
 * The operations. Each returns 0 on success, < 0 with the context's error
 * text (error()) on failure, unless it says otherwise. All but init are called
 * from the DSP task; init from the role's start.
 */
typedef struct rdof_fe_ops {
    /* Bring up the driver (bus, tasks, ring). Nothing streams yet. */
    int (*init)(void *ctx);
    /* Wait up to timeout_ms for the device to be there and open: true once it is. */
    bool (*wait)(void *ctx, int timeout_ms);
    /* Sample rate and center frequency, with the crystal correction (whole ppm, positive for a fast crystal);
     * the rate it really runs at in *got_rate. */
    int (*tune)(void *ctx, double rate, double center_hz, int ppm, double *got_rate);
    /* Move the crystal correction (whole ppm) while it runs. */
    int (*set_ppm)(void *ctx, int ppm);
    /* Gain in dB (NAN: the device's own AGC); the gain it took in *got (NAN for AGC). */
    int (*set_gain)(void *ctx, double db, double *got);
    /* The gains it can set (dB, ascending); returns how many. */
    int (*gain_steps)(void *ctx, const double **db);
    int (*start)(void *ctx);
    void (*stop)(void *ctx);
    /* Up to len bytes (a whole number of samples): blocks until len are there or timeout_ms. Returns the bytes
     * read. Where the stream lost data, filler stands in so time keeps its place; *filled says how much. */
    size_t (*read)(void *ctx, uint8_t *out, size_t len, int timeout_ms, size_t *filled);
    /* The device went away (unplugged, bus error): stop and wait() again. */
    bool (*gone)(void *ctx);
    void (*stats)(void *ctx, rdof_fe_stats_t *out);
    /* "VID:PID serial", the chip's unique id, ...: what a learned ppm is kept against. */
    const char *(*identity)(void *ctx);
    /* The last failure, for the log and the page. */
    const char *(*error)(void *ctx);
    /* Optional: the ppm the crystal correction (set_ppm) takes out of the sample rate, as the device really sets
     * it (the RTL's register has steps of 1/16.8 ppm). NULL: the correction doesn't touch the sample rate. Used to
     * work the crystal's error out from the sample rate (rdof/sdr/rate.h). */
    double (*sample_ppm)(void *ctx);
    /* Optional: run on each chunk after the conversion (sim: its simulated crystal error). */
    void (*post)(void *ctx, rdof_cf_t *iq, int n);
    /* Optional: a throughput test (the RTL's USB spike), run before tuning when the settings ask. */
    void (*spike)(void *ctx);
    /* Optional (duplex other than RX_ONLY): transmit what src gives from now on (NULL: stop), from the DSP task.
     * The sim front end mixes it into what it reads (its loopback). */
    int (*tx_set)(void *ctx, const rdof_fe_tx_source_t *src, double power_dbm);
} rdof_fe_ops_t;

typedef struct {
    const rdof_fe_ops_t *ops;
    const rdof_fe_caps_t *caps;
    void *ctx;
} rdof_fe_t;

/*
 * Whether the settings fit the front end: rate (in range, or one of its rates) and every channel's frequency
 * (it can tune it). 0, or -1 with why in error. Channels outside the window at a given center are not an
 * error: the planner leaves them out.
 */
int rdof_fe_check(const rdof_fe_caps_t *caps, double rate, const double *freq_hz, int n, char *error, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
