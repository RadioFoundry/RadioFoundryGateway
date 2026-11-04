// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * An analog AM receiver that measures as it goes, as rdof/sdr/fmrx.h does for FM: a channel's complex baseband in
 * (about 48 kS/s), the envelope, the audio and a running analysis out.
 *
 * - The envelope |x| (raw, full scale 1.0) at the input rate. The modulation is the envelope over the carrier's
 *   level less 1 (1.0 is 100%), the carrier tracked over 0.2 s; the audio is that, band-passed (300 Hz to 3 kHz by
 *   default), in the same unit.
 * - A series of blocks (block_s): power, carrier level, carrier offset, peak modulation each way, audio rms, and the
 *   residual FM a transmitter's AM-to-PM puts on the carrier. A block mostly under the squelch has no carrier;
 *   samples under it stay out of the figures and count to the noise.
 * - A summary: power, carrier, offset, modulation depth, audio rms and peaks, the strongest audio tone with its
 *   depth, SINAD and THD, the spectrum's peaks (and its bins if asked), and the residual FM.
 *
 * The envelope and the discriminator are low-passed to 5 kHz at the input rate; the rest runs at about 16 kS/s, in
 * float, so it fits an ESP32's core beside other work.
 *
 * Portable C11, one thread.
 */
#ifndef RDOF_AMRX_H
#define RDOF_AMRX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/sdr/filters.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_AMRX_FFT 2048       /* the spectrum's points, at the audio rate */

typedef struct {
    float squelch_dbfs;          /* a block under it has no carrier (-1000: every block has) */
    float block_s;               /* the series' block (0.1) */
    float audio_lo_hz, audio_hi_hz; /* the audio's band (300, 3000; 0: those) */
    float nominal_depth;         /* audio levels in dB against this depth's tone as well (0: none) */
    float spectrum_hz;           /* the summary's spectrum bins up to here (0: none, peaks only) */
} rdof_amrx_opts_t;

typedef struct {
    double t;                    /* seconds from the first sample to the block's start */
    bool carrier;
    float power_dbfs;            /* mean |x|^2, full scale 1.0 */
    float carrier_dbfs;          /* the tracked carrier's level, |x| unmodulated */
    float cfo_hz;
    float depth_pos, depth_neg;  /* the peak modulation each way (5 kHz band): 0.3 is 30% */
    float audio_rms;             /* the band's modulation, rms */
    float fm_rms_hz;             /* residual FM, rms (5 kHz band) */
} rdof_amrx_block_t;

typedef struct {
    double seconds, carrier_s;   /* carrier_s: time over the squelch */
    float power_dbfs, power_min_dbfs, power_max_dbfs; /* carrier blocks; the rest's mean is noise_dbfs */
    float noise_dbfs;
    float carrier_dbfs;          /* the carrier's level, unmodulated */
    float cfo_hz, cfo_min_hz, cfo_max_hz;
    float depth_pos, depth_neg, depth_rms; /* the modulation, 5 kHz band */
    float audio_rms, audio_pos, audio_neg;
    float audio_rms_db;          /* against nominal_depth's tone (NAN without it) */
    bool tone;                   /* an audio tone stands out */
    float tone_hz, tone_depth, sinad_db, thd_pct;
    int npeaks;
    float peak_hz[RDOF_PSD_PEAKS], peak_depth[RDOF_PSD_PEAKS];
    float fm_rms_hz, fm_pos_hz, fm_neg_hz; /* residual FM */
} rdof_amrx_summary_t;

typedef struct rdof_amrx rdof_amrx_t;

/* A receiver for input at fs (24-200 kS/s); alloc: its buffers' allocator (NULL: malloc). NULL: no memory, or a
 * rate or band it can't take. Free with rdof_amrx_free. */
rdof_amrx_t *rdof_amrx_new(double fs, const rdof_amrx_opts_t *o, void *(*alloc)(size_t), void (*dealloc)(void *));
void rdof_amrx_free(rdof_amrx_t *r);
/* The audio's rate (the input's over an integer, about 16 kS/s). */
double rdof_amrx_audio_rate(const rdof_amrx_t *r);
/* What comes out is this much behind what goes in (the squelch looks ahead). The analysis leaves out the first 60 ms
 * after the key-up (the carrier learned, the filters settled). */
double rdof_amrx_delay_s(const rdof_amrx_t *r);
/* n complex samples (I, Q pairs, full scale 1.0). env: n envelope values out (|x|), or NULL. audio: the audio
 * (modulation, 1.0 is 100%) at the audio rate, *naudio of them (room for n / 2 + 1), or NULL. */
void rdof_amrx_feed(rdof_amrx_t *r, const float *iq, int n, float *env, float *audio, int *naudio);
/* The next finished block (false: none yet). */
bool rdof_amrx_block(rdof_amrx_t *r, rdof_amrx_block_t *b);
/* The summary so far. */
void rdof_amrx_summary(rdof_amrx_t *r, rdof_amrx_summary_t *s);
/* As JSON objects, depths in percent: the length, or 0 when cap is too small. */
size_t rdof_amrx_summary_json(rdof_amrx_t *r, const rdof_amrx_summary_t *s, char *out, size_t cap);
size_t rdof_amrx_block_json(const rdof_amrx_block_t *b, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
