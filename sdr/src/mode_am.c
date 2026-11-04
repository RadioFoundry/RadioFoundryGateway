// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Analog AM (double sideband, full carrier: airband voice, ACARS's MSK as an fsk segment) as a signal mode
 * (rdof/sigmode.h). Transmit: FM's program (rdof/sdr/fmgen.h: tones, sweeps, DTMF, FSK, noise, PCM) as the
 * envelope, rendered as complex baseband, the peak at full scale; receive: rdof/sdr/amrx.h. The JSON is
 * bench-api.md section 14.6's.
 */
#include "rdof/sdr/modes.h"

#include "rdof/sdr/features.h"
#if RDOF_FEATURE_SDR

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/sdr/amrx.h"
#include "rdof/sdr/fmgen.h"
#include "sdr_private.h"

/* ── transmit ── */

typedef struct {
    rdof_prog_tx_t p;            /* first: the program's stream input takes it as the transmitter */
    float depth;                 /* the modulation at the program's level 1 */
    float offset_hz;             /* the carrier from the channel */
    double step;                 /* offset_hz's phase a sample, radians */
    double ph;
} am_tx_t;

static void *am_tx_new(const rdof_jdoc_t *d, int job, double fs, double *seconds, char *error, size_t ecap)
{
    am_tx_t *t = rdof_sdr_zalloc(sizeof *t);
    rdof_fmg_cfg_t *c = rdof_sdr_alloc(sizeof *c);
    if (!t || !c) {
        rdof_sdr_free(t), rdof_sdr_free(c);
        snprintf(error, ecap, "no memory for an AM transmitter");
        return NULL;
    }
    memset(c, 0, sizeof *c);
    c->dev_hz = 1; /* the program at level 1 renders as +-1: the modulation, scaled by depth here */
    c->lead_s = (float)rdof_sig_num(d, job, "lead_s", 0.25);
    c->tail_s = (float)rdof_sig_num(d, job, "tail_s", 0.25);
    t->depth = (float)rdof_sig_num(d, job, "depth", 0.3);
    t->offset_hz = (float)rdof_sig_num(d, job, "offset_hz", 0);
    const char *why = !(t->depth >= 0 && t->depth <= 1)               ? "am: depth is 0-1"
                    : !(t->offset_hz >= -10000 && t->offset_hz <= 10000) ? "am: offset_hz is -10000 to 10000"
                                                                          : rdof_prog_parse(d, job, c, error, ecap);
    if (!why)
        why = rdof_fmg_check(c);
    if (why) {
        if (why != error)
            snprintf(error, ecap, "%s", why);
        rdof_sdr_free(t), rdof_sdr_free(c);
        return NULL;
    }
    const int ok = rdof_prog_start(&t->p, c, fs, error, ecap);
    rdof_sdr_free(c);
    if (ok < 0) {
        rdof_sdr_free(t);
        return NULL;
    }
    t->step = 2 * RDOF_PI * t->offset_hz / fs;
    *seconds = rdof_fmg_seconds(&t->p.g.c);
    return t;
}

static void am_tx_free(void *tx)
{
    am_tx_t *t = tx;
    if (!t)
        return;
    rdof_prog_end(&t->p);
    rdof_sdr_free(t);
}

/* |x| = (1 + depth a) / (1 + depth), a the program clamped to +-1 (no overmodulation): the peak at full scale. */
static int am_iq(void *ctx, float *iq, int n)
{
    am_tx_t *t = ctx;
    enum { B = 64 };
    float a[B], env[B];
    const float k = 1 / (1 + t->depth);
    int keyed = 0;
    for (int i0 = 0; i0 < n; i0 += B) {
        const int m = n - i0 < B ? n - i0 : B;
        const int got = rdof_fmg_render(&t->p.g, a, env, m);
        keyed += got;
        for (int j = 0; j < m; j++) {
            const float u = a[j] > 1 ? 1 : a[j] < -1 ? -1 : a[j];
            const float mag = j < got ? env[j] * (1 + t->depth * u) * k : 0;
            float *x = &iq[2 * (i0 + j)];
            if (t->step != 0) {
                x[0] = mag * (float)cos(t->ph), x[1] = mag * (float)sin(t->ph);
                t->ph = fmod(t->ph + t->step, 2 * RDOF_PI);
            } else {
                x[0] = mag, x[1] = 0;
            }
        }
        if (got < m)
            break;
    }
    return keyed;
}

static bool am_done(void *ctx) { return rdof_fmg_done(&((am_tx_t *)ctx)->p.g); }

static rdof_sig_src_t am_tx_src(void *tx)
{
    am_tx_t *t = tx;
    return (rdof_sig_src_t){.iq = am_iq, .done = am_done, .peak_hz = fabs(t->offset_hz), .ctx = t};
}

static size_t am_tx_report(void *tx, char *out, size_t cap)
{
    const am_tx_t *t = tx;
    const rdof_fmg_cfg_t *c = &t->p.g.c;
    int n = snprintf(out, cap, ",\"depth\":%.3f,\"carrier_db\":%.2f,\"offset_hz\":%.1f,\"seconds\":%.3f", t->depth,
                     20 * log10(1 / (1 + t->depth)), t->offset_hz, rdof_fmg_seconds(c));
    if (n > 0 && (size_t)n < cap && c->pcm_samples)
        n += snprintf(out + n, cap - (size_t)n, ",\"pcm_samples\":%llu,\"underruns\":%u",
                      (unsigned long long)t->p.pushed, (unsigned)t->p.g.underruns);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

/* ── receive ── */

enum { S_AUDIO = 1, S_ENV = 2 };

typedef struct {
    rdof_amrx_t *r;
    unsigned streams;            /* S_* asked for */
    double fs, full_audio;
    float *env, *audio;          /* this feed's, when a stream isn't asked for */
    int room;
} am_rx_t;

static void *am_rx_new(const rdof_jdoc_t *d, int req, double fs, double squelch_dbfs, char *error, size_t ecap)
{
    rdof_amrx_opts_t o = {
        .squelch_dbfs = (float)rdof_sig_num(d, req, "squelch_dbfs", squelch_dbfs),
        .block_s = (float)rdof_sig_num(d, req, "block_s", 0.1),
        .audio_lo_hz = (float)rdof_sig_num(d, req, "audio_lo_hz", 300),
        .audio_hi_hz = (float)rdof_sig_num(d, req, "audio_hi_hz", 3000),
        .nominal_depth = (float)rdof_sig_num(d, req, "nominal_depth", 0),
        .spectrum_hz = (float)rdof_sig_num(d, req, "spectrum_hz", 0),
    };
    if (!(o.block_s >= 0.01f && o.block_s <= 10) || !(o.audio_lo_hz >= 50 && o.audio_lo_hz < o.audio_hi_hz) ||
        !(o.audio_hi_hz <= 6000) || !(o.nominal_depth >= 0 && o.nominal_depth <= 1) ||
        !(o.spectrum_hz >= 0 && o.spectrum_hz <= 8000)) {
        snprintf(error, ecap, "am: block_s 0.01-10, audio_lo_hz 50 to audio_hi_hz, audio_hi_hz to 6000, "
                              "nominal_depth 0-1, spectrum_hz 0-8000");
        return NULL;
    }
    am_rx_t *x = rdof_sdr_zalloc(sizeof *x);
    if (!x) {
        snprintf(error, ecap, "no memory for an AM receiver");
        return NULL;
    }
    if (!(x->r = rdof_amrx_new(fs, &o, rdof_sdr_alloc, rdof_sdr_free))) {
        rdof_sdr_free(x);
        snprintf(error, ecap, "no memory for an AM receiver, or %.0f S/s isn't 24-200 kS/s", fs);
        return NULL;
    }
    char s[48] = "";
    rdof_sig_text(d, req, "streams", s, sizeof s); /* "audio", "env", "audio,env" */
    x->streams = (strstr(s, "audio") ? S_AUDIO : 0) | (strstr(s, "env") ? S_ENV : 0);
    x->full_audio = rdof_sig_num(d, req, "audio_full", 1.0);
    x->fs = fs;
    return x;
}

static void am_rx_free(void *rx)
{
    am_rx_t *x = rx;
    if (!x)
        return;
    rdof_amrx_free(x->r);
    rdof_sdr_free(x->env), rdof_sdr_free(x->audio);
    rdof_sdr_free(x);
}

static int am_rx_streams(void *rx, rdof_sig_stream_t *out, int max)
{
    am_rx_t *x = rx;
    int n = 0;
    if ((x->streams & S_AUDIO) && n < max)
        out[n++] = (rdof_sig_stream_t){"audio", "depth", rdof_amrx_audio_rate(x->r), x->full_audio};
    if ((x->streams & S_ENV) && n < max)
        out[n++] = (rdof_sig_stream_t){"env", "FS", x->fs, 1.0};
    return n;
}

static void am_rx_feed(void *rx, const float *iq, int n, float *const *out, int *nout)
{
    am_rx_t *x = rx;
    if (n > x->room) { /* scratch for a stream nobody asked for */
        rdof_sdr_free(x->env), rdof_sdr_free(x->audio);
        x->env = rdof_sdr_alloc(sizeof(float) * (size_t)n), x->audio = rdof_sdr_alloc(sizeof(float) * (size_t)n);
        x->room = x->env && x->audio ? n : 0;
    }
    int k = 0, na = 0;
    float *audio = (x->streams & S_AUDIO) ? out[k++] : x->room ? x->audio : NULL;
    float *env = (x->streams & S_ENV) ? out[k] : NULL;
    rdof_amrx_feed(x->r, iq, n, env, audio, &na);
    k = 0;
    if (x->streams & S_AUDIO)
        nout[k++] = na;
    if (x->streams & S_ENV)
        nout[k] = n;
}

static size_t am_rx_record(void *rx, char *out, size_t cap)
{
    am_rx_t *x = rx;
    rdof_amrx_block_t b;
    if (cap < 16 || !rdof_amrx_block(x->r, &b))
        return 0;
    memcpy(out, "{\"block\":", 9);
    const size_t n = rdof_amrx_block_json(&b, out + 9, cap - 10);
    if (!n)
        return 0;
    out[9 + n] = '}';
    return 10 + n;
}

static size_t am_rx_summary(void *rx, char *out, size_t cap)
{
    am_rx_t *x = rx;
    rdof_amrx_summary_t s;
    rdof_amrx_summary(x->r, &s);
    return rdof_amrx_summary_json(x->r, &s, out, cap);
}

const rdof_sig_mode_t rdof_mode_am = {
    .name = "am",
    .about = "analog AM (DSB, full carrier): tones, sweeps, DTMF, FSK, noise or PCM at a depth; measured on receive",
    .tx_new = am_tx_new,
    .tx_free = am_tx_free,
    .tx_src = am_tx_src,
    .tx_in_room = rdof_prog_tx_in_room,
    .tx_in = rdof_prog_tx_in,
    .tx_in_end = rdof_prog_tx_in_end,
    .tx_in_want = rdof_prog_tx_in_want,
    .tx_report = am_tx_report,
    .rx_new = am_rx_new,
    .rx_free = am_rx_free,
    .rx_streams = am_rx_streams,
    .rx_feed = am_rx_feed,
    .rx_record = am_rx_record,
    .rx_summary = am_rx_summary,
};

#endif
