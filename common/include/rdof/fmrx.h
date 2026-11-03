// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * An analog FM receiver that measures as it goes: a channel's complex baseband in (about 48 kS/s), the discriminator,
 * the audio and a running analysis out.
 *
 * - The discriminator (raw, Hz) at the input rate, and filtered audio: the subaudio taken out (a 300 Hz high-pass,
 *   and a notch on the CTCSS tone once it is known), a 3 kHz low-pass, de-emphasis if asked (0 dB at 1 kHz).
 * - A series of blocks (block_s): carrier power, envelope, carrier offset, peak deviation each way, audio and
 *   subaudio rms. A block mostly under the squelch has no carrier; samples under it stay out of the deviation and
 *   audio figures, and count to the noise.
 * - A summary: power, envelope, offset, deviation, audio rms and peaks, the strongest audio tone with its SINAD and
 *   THD, the spectrum's peaks (and its bins if asked), and the subaudio: a CTCSS tone (its measured frequency,
 *   deviation and reverse burst) or a DCS code (its polarity, aliases and turn-off).
 *
 * Deviation is measured in a 5 kHz band (the discriminator low-passed there), the subaudio below 270 Hz. DCS words
 * are read as rdof/fmgen.h sends them. The processing runs in float, at the input rate only for the discriminator
 * and a 5 kHz low-pass; the rest at a third of it or less (about 16 and 4 kS/s), so it fits an ESP32's core beside
 * other work.
 *
 * Portable C11, one thread.
 */
#ifndef RDOF_FMRX_H
#define RDOF_FMRX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_FMRX_FFT 2048       /* the spectrum's points, at the audio rate */
#define RDOF_FMRX_PEAKS 6

typedef struct {
    float squelch_dbfs;          /* a block under it has no carrier (-1000: every block has) */
    float block_s;               /* the series' block (0.1) */
    float deemph_us;             /* the filtered audio's de-emphasis (0 flat; 750 TIA-603) */
    float nominal_dev_hz;        /* audio levels in dB against it as well (0: none) */
    float spectrum_hz;           /* the summary's spectrum bins up to here (0: none, peaks only) */
} rdof_fmrx_opts_t;

typedef struct {
    double t;                    /* seconds from the first sample to the block's start */
    bool carrier;
    float power_dbfs;            /* mean |x|^2, full scale 1.0 */
    float env_min, env_max;      /* |x| */
    float cfo_hz;                /* the mean frequency */
    float dev_pos_hz, dev_neg_hz; /* the peak deviation each way from the carrier (5 kHz band) */
    float audio_rms_hz;          /* filtered, flat */
    float sub_rms_hz;
} rdof_fmrx_block_t;

typedef struct {
    double seconds, carrier_s;   /* carrier_s: time over the squelch */
    float power_dbfs, power_min_dbfs, power_max_dbfs; /* carrier blocks; the rest's mean is noise_dbfs */
    float noise_dbfs;
    float env_mean, am_pct;      /* the envelope's mean, and its rms change in percent of it */
    float cfo_hz, cfo_min_hz, cfo_max_hz;
    float dev_pos_hz, dev_neg_hz, dev_rms_hz;
    float audio_rms_hz, audio_pos_hz, audio_neg_hz;
    float audio_rms_db;          /* against nominal_dev_hz (NAN without it) */
    bool tone;                   /* an audio tone stands out */
    float tone_hz, tone_dev_hz, sinad_db, thd_pct;
    int npeaks;
    float peak_hz[RDOF_FMRX_PEAKS], peak_dev_hz[RDOF_FMRX_PEAKS];
    int sub;                     /* RDOF_FMG_SUB_* */
    float sub_rms_hz;
    float ctcss_hz, ctcss_nominal_hz, ctcss_dev_hz;
    double reverse_burst_t;      /* the last 180-degree turn of the CTCSS tone, s (-1 none) */
    int dcs_code;                /* -1 none */
    bool dcs_inv, dcs_turnoff;
    float dcs_dev_hz;            /* the NRZ's level each way */
    long dcs_words;              /* whole words read */
    char dcs_aliases[64];        /* the same bits as other codes: "047I 456N" */
} rdof_fmrx_summary_t;

typedef struct rdof_fmrx rdof_fmrx_t;

/* A receiver for input at fs (24-200 kS/s); alloc: its buffers' allocator (NULL: malloc). NULL: no memory, or a
 * rate it can't take. Free with rdof_fmrx_free (and the free that goes with alloc). */
rdof_fmrx_t *rdof_fmrx_new(double fs, const rdof_fmrx_opts_t *o, void *(*alloc)(size_t), void (*dealloc)(void *));
void rdof_fmrx_free(rdof_fmrx_t *r);
/* The filtered audio's rate (the input's over an integer, about 16 kS/s). */
double rdof_fmrx_audio_rate(const rdof_fmrx_t *r);
/* What comes out (raw and audio) is this much behind what goes in: the squelch looks ahead, so a key-up and key-down
 * are cut where they are. The analysis leaves out the first 60 ms after the key-up (the offset learned, the filters
 * settled). */
double rdof_fmrx_delay_s(const rdof_fmrx_t *r);
/* n complex samples (I, Q pairs, full scale 1.0). raw: n discriminator values out (Hz), or NULL. audio: the
 * filtered audio (Hz of deviation) at the audio rate, *naudio of them (room for n / 2 + 1), or NULL. */
void rdof_fmrx_feed(rdof_fmrx_t *r, const float *iq, int n, float *raw, float *audio, int *naudio);
/* The next finished block (false: none yet). */
bool rdof_fmrx_block(rdof_fmrx_t *r, rdof_fmrx_block_t *b);
/* The summary so far. */
void rdof_fmrx_summary(rdof_fmrx_t *r, rdof_fmrx_summary_t *s);
/* The summary as a JSON object (its spectrum's bins too when asked): its length, or 0 when cap is too small. */
size_t rdof_fmrx_summary_json(rdof_fmrx_t *r, const rdof_fmrx_summary_t *s, char *out, size_t cap);
size_t rdof_fmrx_block_json(const rdof_fmrx_block_t *b, char *out, size_t cap);

/* DCS: the code a 23-bit word read (in the order received, bit 0 first) is, normal or inverted; -1 none. */
int rdof_dcs_lookup(uint32_t word, bool *inv);

#ifdef __cplusplus
}
#endif

#endif
