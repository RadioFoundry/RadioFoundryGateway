// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Signal modes: modulations the bench can send and analyze besides the DMR it runs natively (docs/spec/bench-api.md
 * section 14.6). Each mode is one table with the same shape, so a new modulation (P25, NXDN, M17, D-STAR) is a new
 * file and one line in the platform's list; the line protocol, the SDR modem and the front ends don't change.
 *
 * Only the interface is here: the modes are in sdr (rdof/sdr/modes.h), on its shared DSP (the channelizer, the
 * discriminator, the filters, the FFT a platform may replace), and the platform hands their list to whoever serves
 * them (rdof_bl_platform_t.modes), so common never depends on sdr.
 *
 * Transmit: a mode makes a transmitter state from a job's JSON (its parameters), at the transmitter's sample rate,
 * and hands out a sample source (rdof_sig_src_t): frequency and envelope (an FM-family mode: FM, FSK, 4FSK) or
 * complex baseband (any other), rendered on the transmitter's thread. A mode may take a stream after the request
 * (PCM audio, a payload): the server feeds it bytes as the mode has room.
 *
 * Receive: a mode makes a receiver from the analysis request's JSON, at the channel's rate (about 48 kS/s, complex,
 * full scale 1.0). Channelizing is the platform's (software here, an FPGA's on some), so a receiver only sees its
 * channel. A platform whose hardware also discriminates may feed instantaneous frequency instead (rx_feed_freq,
 * where a mode has it). It gives back named output streams (audio, the discriminator: int16 frames, each with its
 * rate and full scale), records as it goes (JSON objects, one a line), and a summary at the end.
 *
 * Portable C11 (the ESP32, Linux, macOS). Memory comes from the SDR's allocator (PSRAM first on the ESP32). A mode's
 * state is one thread's, except what its source and stream input say.
 */
#ifndef RDOF_SIGMODE_H
#define RDOF_SIGMODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/jsonp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_SIG_STREAMS 4   /* a receiver's output streams at most */

/* What a transmitter renders, n samples at a time, from its thread. A mode gives freq, iq or both. */
typedef struct {
    /* Frequency from the carrier (Hz) and envelope (0..1): how many of the n are keyed (the rest zeros: the end). */
    int (*freq)(void *ctx, float *hz, float *env, int n);
    /* Complex baseband, I and Q interleaved, |x| up to 1: how many are keyed. NULL: the caller integrates freq. */
    int (*iq)(void *ctx, float *iq, int n);
    /* Everything is sent (the source keys no more). */
    bool (*done)(void *ctx);
    double peak_hz;          /* freq's largest |Hz|: a front end that sends frequency sets its scale from it */
    double rate;             /* samples a second it renders at (the platform's sig_rate; 0: the transmitter's own) */
    void *ctx;
} rdof_sig_src_t;

/* A receiver's output stream. */
typedef struct {
    const char *name;        /* "audio", "disc" */
    const char *unit;        /* what a sample is: "Hz" (of deviation) */
    double rate;             /* samples a second */
    double full;             /* the value int16 full scale (32767) stands for */
} rdof_sig_stream_t;

typedef struct rdof_sig_mode {
    const char *name;        /* "fm" */
    const char *about;       /* a line for info */
    /* ── transmit (NULL: this mode only receives) ── */
    /* A transmitter for job (an object) at fs samples a second: NULL with error. *seconds: how long it keys (for
     * the modem's queue). */
    void *(*tx_new)(const rdof_jdoc_t *d, int job, double fs, double *seconds, char *error, size_t ecap);
    void (*tx_free)(void *tx);
    rdof_sig_src_t (*tx_src)(void *tx);
    /* The stream after the request (NULL: none): bytes it takes now; take them (the count taken); none will come
     * (gone: the client stopped short). in_want: bytes it still expects in all (0: none, the stream is complete). */
    size_t (*tx_in_room)(void *tx);
    size_t (*tx_in)(void *tx, const uint8_t *b, size_t n);
    void (*tx_in_end)(void *tx, bool gone);
    uint64_t (*tx_in_want)(void *tx);
    /* What happened, as a JSON object's members (",\"underruns\":0"), for the job's summary: its length. */
    size_t (*tx_report)(void *tx, char *out, size_t cap);
    /* ── receive (NULL: this mode only transmits) ── */
    /* A receiver for req (an object) at fs complex samples a second: NULL with error. squelch_dbfs: the platform's
     * default (from the channel's noise floor); req may set its own. */
    void *(*rx_new)(const rdof_jdoc_t *d, int req, double fs, double squelch_dbfs, char *error, size_t ecap);
    void (*rx_free)(void *rx);
    /* Its output streams (those asked for in the request): how many. */
    int (*rx_streams)(void *rx, rdof_sig_stream_t *out, int max);
    /* n complex samples in. out[k]: stream k's samples from them (room for n each), *nout[k] how many. */
    void (*rx_feed)(void *rx, const float *iq, int n, float *const *out, int *nout);
    /* Optional, for hardware that discriminates: n instantaneous frequencies (Hz) and powers (full scale 1.0). */
    void (*rx_feed_freq)(void *rx, const float *hz, const float *power, int n, float *const *out, int *nout);
    /* The next record (a whole JSON object): its length, 0 none. */
    size_t (*rx_record)(void *rx, char *out, size_t cap);
    /* The summary so far (a JSON object): its length, 0 when cap is too small. */
    size_t (*rx_summary)(void *rx, char *out, size_t cap);
} rdof_sig_mode_t;

/* In a NULL-terminated list (NULL: none): the mode by name, or NULL. */
const rdof_sig_mode_t *rdof_sig_find(const rdof_sig_mode_t *const *modes, const char *name);
/* The names in the list that can transmit (tx) or receive, as a JSON array: its length (0: no room). */
size_t rdof_sig_names_json(const rdof_sig_mode_t *const *modes, bool tx, char *out, size_t cap);

/* For a mode's parser: a job's member, as a number, a flag or text. */
double rdof_sig_num(const rdof_jdoc_t *d, int obj, const char *key, double dflt);
bool rdof_sig_flag(const rdof_jdoc_t *d, int obj, const char *key);
bool rdof_sig_text(const rdof_jdoc_t *d, int obj, const char *key, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
