// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Analog FM as a signal mode (rdof/sigmode.h): the generator (rdof/sdr/fmgen.h) for transmit, the receiver
 * (rdof/sdr/fmrx.h) for receive. The JSON is bench-api.md section 14.6's. Audio FSK (AFSK: Bell 202, APRS) is an
 * fsk segment of the program, audio on this FM like any other; a mode that keys the carrier itself (4FSK, GMSK) is
 * a mode of its own.
 */
#include "rdof/sdr/modes.h"

#include "rdof/sdr/features.h"
#if RDOF_FEATURE_SDR

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/sdr/fmgen.h"
#include "rdof/sdr/dtmfdet.h"
#include "rdof/sdr/fmrx.h"
#include "sdr_private.h"

/* ── transmit ── */

typedef rdof_prog_tx_t fm_tx_t;

static double parse_dcs(const char *s, bool *inv)
{
    char *end;
    const long v = strtol(s, &end, 8);
    *inv = *end == 'I' || *end == 'i';
    if (end == s || (*end && !strchr("NnIi", *end)) || (*end && end[1]) || v < 0 || v > 0777)
        return -1;
    return (double)v;
}

/* program[i] into c: its segment, its digits or bytes in c->data. NULL, or why not. */
static const char *parse_seg(const rdof_jdoc_t *d, int o, rdof_fmg_cfg_t *c, size_t *used, char *why, size_t cap)
{
    char kind[16];
    rdof_sig_text(d, o, "kind", kind, sizeof kind);
    rdof_fmg_seg_t *s = &c->seg[c->nseg];
    memset(s, 0, sizeof *s);
    s->seconds = (float)rdof_sig_num(d, o, "s", 1);
    s->level = (float)rdof_sig_num(d, o, "level", !strcmp(kind, "noise") ? 0.3 : 1);
    const int hz = rdof_jget(d, o, "hz");
    const rdof_jnode_t *hn = rdof_jnode(d, hz);
    int nhz = 0;
    if (hn && hn->type == RDOF_JNUM)
        s->hz[nhz++] = (float)hn->num;
    for (int k = 0; hn && hn->type == RDOF_JARR && k < hn->count && nhz < RDOF_FMG_TONES; k++) {
        const rdof_jnode_t *e = rdof_jnode(d, rdof_jat(d, hz, k));
        if (!e || e->type != RDOF_JNUM)
            return "hz: numbers";
        s->hz[nhz++] = (float)e->num;
    }
    if (!strcmp(kind, "tone")) {
        s->kind = RDOF_FMG_TONE, s->ntones = (uint8_t)nhz;
    } else if (!strcmp(kind, "sweep")) {
        s->kind = RDOF_FMG_SWEEP, s->log = rdof_sig_flag(d, o, "log");
        if (nhz != 2)
            return "a sweep's hz: [from, to]";
    } else if (!strcmp(kind, "silence")) {
        s->kind = RDOF_FMG_SILENCE;
    } else if (!strcmp(kind, "noise")) {
        s->kind = RDOF_FMG_NOISE;
    } else if (!strcmp(kind, "pcm")) {
        s->kind = RDOF_FMG_PCM;
    } else if (!strcmp(kind, "dtmf")) {
        char digits[256];
        rdof_sig_text(d, o, "digits", digits, sizeof digits);
        const size_t n = strlen(digits);
        if (!n || *used + n > RDOF_FMG_DATA)
            return "dtmf: digits, within the program's 1024 bytes";
        s->kind = RDOF_FMG_DTMF, s->seconds = (float)rdof_sig_num(d, o, "s", 0.1);
        s->gap_s = (float)rdof_sig_num(d, o, "gap_s", 0.05);
        s->off = (uint16_t)*used, s->n = (uint32_t)n;
        memcpy(c->data + *used, digits, n);
        *used += n;
    } else if (!strcmp(kind, "fsk")) {
        /* the bytes: "hex" or "text"; bits: how many of them (all) */
        char buf[2 * RDOF_FMG_DATA + 1];
        size_t n = 0;
        if (rdof_sig_text(d, o, "hex", buf, sizeof buf)) {
            const size_t L = strlen(buf);
            if (L % 2 || *used + L / 2 > RDOF_FMG_DATA)
                return "fsk hex: whole bytes, within the program's 1024";
            for (size_t i = 0; i < L; i += 2) {
                unsigned v;
                if (sscanf(buf + i, "%2x", &v) != 1)
                    return "fsk hex: hex digits";
                c->data[*used + n++] = (uint8_t)v;
            }
        } else if (rdof_sig_text(d, o, "text", buf, sizeof buf)) {
            n = strlen(buf);
            if (*used + n > RDOF_FMG_DATA)
                return "fsk text: within the program's 1024 bytes";
            memcpy(c->data + *used, buf, n);
        }
        if (!n)
            return "fsk: hex or text to send";
        s->kind = RDOF_FMG_FSK;
        s->baud = (float)rdof_sig_num(d, o, "baud", 1200);
        if (nhz == 0)
            s->hz[0] = 1200, s->hz[1] = 2200; /* Bell 202 */
        else if (nhz != 2)
            return "fsk hz: [mark, space]";
        s->nrzi = rdof_sig_flag(d, o, "nrzi");
        s->lsb = rdof_sig_flag(d, o, "lsb");
        const double bits = rdof_sig_num(d, o, "bits", (double)(8 * n));
        if (!(bits >= 1 && bits <= 8.0 * n))
            return "fsk bits: 1 to the bytes' bits";
        s->off = (uint16_t)*used, s->n = (uint32_t)bits;
        *used += n;
    } else {
        snprintf(why, cap, "no segment kind \"%s\" (tone, sweep, dtmf, fsk, noise, silence, pcm)", kind);
        return why;
    }
    c->nseg++;
    return NULL;
}

/* The job's settings into c: NULL, or why not (in why, or a constant). */
static const char *fm_cfg(const rdof_jdoc_t *d, int job, rdof_fmg_cfg_t *c, char *why, size_t cap)
{
    memset(c, 0, sizeof *c);
    c->dev_hz = (float)rdof_sig_num(d, job, "dev_hz", 2500);
    c->preemph_us = (float)rdof_sig_num(d, job, "preemph_us", 0);
    c->lead_s = (float)rdof_sig_num(d, job, "lead_s", 0.25);
    c->tail_s = (float)rdof_sig_num(d, job, "tail_s", 0.25);
    char dcs[16];
    const double ctcss = rdof_sig_num(d, job, "ctcss", 0);
    const bool has_dcs = rdof_sig_text(d, job, "dcs", dcs, sizeof dcs) && dcs[0];
    if (ctcss > 0 && has_dcs)
        return "ctcss or dcs, not both";
    if (ctcss > 0) {
        c->sub = RDOF_FMG_SUB_CTCSS, c->ctcss_hz = (float)ctcss;
        c->sub_end_s = (float)rdof_sig_num(d, job, "sub_end_s", 0.15); /* the reverse burst */
    } else if (has_dcs) {
        bool inv;
        const double v = parse_dcs(dcs, &inv);
        if (v < 0)
            return "dcs: an octal code, N or I after it (023N)";
        c->sub = RDOF_FMG_SUB_DCS, c->dcs = (uint16_t)v, c->dcs_inv = inv;
        c->sub_end_s = (float)rdof_sig_num(d, job, "sub_end_s", 0.18); /* the turn-off tone */
    }
    c->sub_dev_hz = (float)rdof_sig_num(d, job, "sub_dev_hz", c->sub ? 0.2 * c->dev_hz : 0);
    const char *e = rdof_prog_parse(d, job, c, why, cap);
    return e ? e : rdof_fmg_check(c);
}

const char *rdof_prog_parse(const rdof_jdoc_t *d, int job, rdof_fmg_cfg_t *c, char *why, size_t cap)
{
    c->repeat = (uint16_t)rdof_sig_num(d, job, "repeat", 1);
    c->seed = (uint32_t)rdof_sig_num(d, job, "seed", 0);
    const int p = rdof_jget(d, job, "pcm");
    if (p >= 0) {
        c->pcm_rate = (float)rdof_sig_num(d, p, "rate", 8000);
        c->pcm_samples = (uint32_t)rdof_sig_num(d, p, "samples", 0);
    }
    const int prog = rdof_jget(d, job, "program");
    const rdof_jnode_t *pn = rdof_jnode(d, prog);
    if (pn && pn->type != RDOF_JARR)
        return "program: a list of segments";
    size_t used = 0;
    for (int i = 0; pn && i < pn->count; i++) {
        if (c->nseg >= RDOF_FMG_SEGS)
            return "program: 32 segments at most";
        const int o = rdof_jat(d, prog, i);
        const rdof_jnode_t *on = rdof_jnode(d, o);
        if (!on || on->type != RDOF_JOBJ)
            return "program: objects";
        const char *e = parse_seg(d, o, c, &used, why, cap);
        if (e) {
            if (e != why)
                snprintf(why, cap, "program[%d]: %s", i, e);
            else {
                char t[200];
                snprintf(t, sizeof t, "program[%d]: %s", i, why);
                snprintf(why, cap, "%s", t);
            }
            return why;
        }
    }
    bool has_pcm = false;
    for (int i = 0; i < c->nseg; i++)
        has_pcm |= c->seg[i].kind == RDOF_FMG_PCM;
    if (has_pcm && !c->pcm_samples)
        return "a pcm segment needs pcm: {rate, samples}";
    if (!has_pcm && c->pcm_samples) { /* pcm with no program: the program is the PCM */
        if (c->nseg >= RDOF_FMG_SEGS)
            return "program: 32 segments at most";
        c->seg[c->nseg++] = (rdof_fmg_seg_t){.kind = RDOF_FMG_PCM, .level = 1};
    }
    return NULL;
}

int rdof_prog_start(rdof_prog_tx_t *t, const rdof_fmg_cfg_t *c, double fs, char *error, size_t ecap)
{
    uint32_t cap = 0;
    t->ring = NULL;
    if (c->pcm_samples) { /* 2 s of PCM buffered, 16-128 K samples */
        cap = 16384;
        while (cap < 131072 && cap < 2 * c->pcm_rate)
            cap <<= 1;
        if (!(t->ring = rdof_sdr_alloc(sizeof(int16_t) * cap))) {
            snprintf(error, ecap, "no memory for the PCM buffer");
            return -1;
        }
    }
    if (rdof_fmg_init(&t->g, c, fs, t->ring, cap) < 0) {
        snprintf(error, ecap, "%s", rdof_fmg_check(c) ? rdof_fmg_check(c) : "the generator can't start");
        rdof_sdr_free(t->ring);
        t->ring = NULL;
        return -1;
    }
    t->pushed = 0;
    t->carry = -1;
    return 0;
}

void rdof_prog_end(rdof_prog_tx_t *t)
{
    rdof_sdr_free(t->ring);
    t->ring = NULL;
}

static void *fm_tx_new(const rdof_jdoc_t *d, int job, double fs, double *seconds, char *error, size_t ecap)
{
    fm_tx_t *t = rdof_sdr_zalloc(sizeof *t);
    rdof_fmg_cfg_t *c = rdof_sdr_alloc(sizeof *c);
    if (!t || !c) {
        rdof_sdr_free(t), rdof_sdr_free(c);
        snprintf(error, ecap, "no memory for an FM transmitter");
        return NULL;
    }
    const char *why = fm_cfg(d, job, c, error, ecap);
    if (why) {
        if (why != error)
            snprintf(error, ecap, "%s", why);
        rdof_sdr_free(t), rdof_sdr_free(c);
        return NULL;
    }
    const int ok = rdof_prog_start(t, c, fs, error, ecap);
    rdof_sdr_free(c);
    if (ok < 0) {
        rdof_sdr_free(t);
        return NULL;
    }
    *seconds = rdof_fmg_seconds(&t->g.c);
    return t;
}

static void fm_tx_free(void *tx)
{
    fm_tx_t *t = tx;
    if (!t)
        return;
    rdof_prog_end(t);
    rdof_sdr_free(t);
}

static int fm_freq(void *ctx, float *hz, float *env, int n) { return rdof_fmg_render(ctx, hz, env, n); }
static bool fm_done(void *ctx) { return rdof_fmg_done(ctx); }

static rdof_sig_src_t fm_tx_src(void *tx)
{
    fm_tx_t *t = tx;
    return (rdof_sig_src_t){.freq = fm_freq, .done = fm_done, .peak_hz = rdof_fmg_peak_hz(&t->g.c), .ctx = &t->g};
}

uint64_t rdof_prog_tx_in_want(void *tx)
{
    const rdof_prog_tx_t *t = tx;
    return 2 * (t->g.c.pcm_samples - t->pushed) - (t->carry >= 0);
}

size_t rdof_prog_tx_in_room(void *tx)
{
    rdof_prog_tx_t *t = tx;
    if (!t->ring)
        return 0;
    const uint64_t want = rdof_prog_tx_in_want(tx), room = 2 * (uint64_t)rdof_fmg_pcm_space(&t->g) - (t->carry >= 0);
    return (size_t)(room < want ? room : want);
}

size_t rdof_prog_tx_in(void *tx, const uint8_t *b, size_t n)
{
    rdof_prog_tx_t *t = tx;
    size_t used = 0;
    int16_t s[256];
    while (used < n && t->ring) {
        uint32_t k = 0;
        const uint32_t space = rdof_fmg_pcm_space(&t->g);
        while (used < n && k < 256 && k < space && t->pushed + k < t->g.c.pcm_samples) {
            if (t->carry < 0) {
                t->carry = b[used++];
                continue;
            }
            s[k++] = (int16_t)(uint16_t)(t->carry | b[used++] << 8);
            t->carry = -1;
        }
        if (!k)
            break;
        t->pushed += rdof_fmg_pcm_push(&t->g, s, k);
    }
    if (t->pushed >= t->g.c.pcm_samples)
        rdof_fmg_pcm_end(&t->g);
    return used;
}

void rdof_prog_tx_in_end(void *tx, bool gone)
{
    (void)gone; /* the rest plays as silence, counted in underruns */
    rdof_fmg_pcm_end(&((rdof_prog_tx_t *)tx)->g);
}

static size_t fm_tx_report(void *tx, char *out, size_t cap)
{
    const fm_tx_t *t = tx;
    const rdof_fmg_cfg_t *c = &t->g.c;
    int n = snprintf(out, cap, ",\"dev_hz\":%.0f,\"peak_hz\":%.0f,\"seconds\":%.3f", c->dev_hz, rdof_fmg_peak_hz(c),
                     rdof_fmg_seconds(c));
    if (n > 0 && (size_t)n < cap && c->sub == RDOF_FMG_SUB_CTCSS)
        n += snprintf(out + n, cap - (size_t)n, ",\"ctcss\":%.1f,\"sub_dev_hz\":%.0f", c->ctcss_hz, c->sub_dev_hz);
    else if (n > 0 && (size_t)n < cap && c->sub == RDOF_FMG_SUB_DCS)
        n += snprintf(out + n, cap - (size_t)n, ",\"dcs\":\"%03o%c\",\"sub_dev_hz\":%.0f", c->dcs,
                      c->dcs_inv ? 'I' : 'N', c->sub_dev_hz);
    if (n > 0 && (size_t)n < cap && c->pcm_samples)
        n += snprintf(out + n, cap - (size_t)n, ",\"pcm_samples\":%llu,\"underruns\":%u",
                      (unsigned long long)t->pushed, (unsigned)t->g.underruns);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

/* ── receive ── */

enum { S_AUDIO = 1, S_DISC = 2 };

typedef struct {
    rdof_fmrx_t *r;
    unsigned streams;            /* S_* asked for */
    double fs, full_audio;
    float *disc, *audio;         /* this feed's, when a stream isn't asked for */
    int room;
    rdof_dtmfdet_t *dtmf;        /* "dtmf": digits in the filtered audio */
    double delay_s, ra;          /* the audio's lag behind the input; its rate */
    char digits[65];             /* the summary's (at most 64) */
    int ndigits;
    char runs[1400];             /* and each digit's record, for the summary */
    size_t nruns;
} fm_rx_t;

static void *fm_rx_new(const rdof_jdoc_t *d, int req, double fs, double squelch_dbfs, char *error, size_t ecap)
{
    rdof_fmrx_opts_t o = {
        .squelch_dbfs = (float)rdof_sig_num(d, req, "squelch_dbfs", squelch_dbfs),
        .block_s = (float)rdof_sig_num(d, req, "block_s", 0.1),
        .deemph_us = (float)rdof_sig_num(d, req, "deemph_us", 0),
        .nominal_dev_hz = (float)rdof_sig_num(d, req, "nominal_dev_hz", 0),
        .spectrum_hz = (float)rdof_sig_num(d, req, "spectrum_hz", 0),
    };
    if (!(o.block_s >= 0.01f && o.block_s <= 10) || !(o.deemph_us >= 0 && o.deemph_us <= 2000) ||
        !(o.spectrum_hz >= 0 && o.spectrum_hz <= 8000)) {
        snprintf(error, ecap, "fm: block_s 0.01-10, deemph_us 0-2000, spectrum_hz 0-8000");
        return NULL;
    }
    fm_rx_t *x = rdof_sdr_zalloc(sizeof *x);
    if (!x) {
        snprintf(error, ecap, "no memory for an FM receiver");
        return NULL;
    }
    if (!(x->r = rdof_fmrx_new(fs, &o, rdof_sdr_alloc, rdof_sdr_free))) {
        rdof_sdr_free(x);
        snprintf(error, ecap, "no memory for an FM receiver, or %.0f S/s isn't 24-200 kS/s", fs);
        return NULL;
    }
    char s[48] = "";
    rdof_sig_text(d, req, "streams", s, sizeof s); /* "audio", "disc", "audio,disc" */
    x->streams = (strstr(s, "audio") ? S_AUDIO : 0) | (strstr(s, "disc") ? S_DISC : 0);
    x->full_audio = rdof_sig_num(d, req, "audio_full_hz", 5000);
    x->fs = fs;
    x->ra = rdof_fmrx_audio_rate(x->r);
    x->delay_s = rdof_fmrx_delay_s(x->r);
    if (rdof_sig_flag(d, req, "dtmf") && !(x->dtmf = rdof_dtmfdet_new(x->ra, rdof_sdr_alloc, rdof_sdr_free))) {
        rdof_fmrx_free(x->r);
        rdof_sdr_free(x);
        snprintf(error, ecap, "no memory for a DTMF decoder");
        return NULL;
    }
    return x;
}

static void fm_rx_free(void *rx)
{
    fm_rx_t *x = rx;
    if (!x)
        return;
    rdof_fmrx_free(x->r);
    rdof_dtmfdet_free(x->dtmf);
    rdof_sdr_free(x->disc), rdof_sdr_free(x->audio);
    rdof_sdr_free(x);
}

static int fm_rx_streams(void *rx, rdof_sig_stream_t *out, int max)
{
    fm_rx_t *x = rx;
    int n = 0;
    if ((x->streams & S_AUDIO) && n < max)
        out[n++] = (rdof_sig_stream_t){"audio", "Hz", rdof_fmrx_audio_rate(x->r), x->full_audio};
    if ((x->streams & S_DISC) && n < max)
        out[n++] = (rdof_sig_stream_t){"disc", "Hz", x->fs, x->fs / 2};
    return n;
}

static void fm_rx_feed(void *rx, const float *iq, int n, float *const *out, int *nout)
{
    fm_rx_t *x = rx;
    if (n > x->room) { /* scratch for a stream nobody asked for (the receiver writes both or neither) */
        rdof_sdr_free(x->disc), rdof_sdr_free(x->audio);
        x->disc = rdof_sdr_alloc(sizeof(float) * (size_t)n), x->audio = rdof_sdr_alloc(sizeof(float) * (size_t)n);
        x->room = x->disc && x->audio ? n : 0;
    }
    int k = 0, na = 0;
    float *audio = (x->streams & S_AUDIO) ? out[k++] : x->room ? x->audio : NULL;
    float *disc = (x->streams & S_DISC) ? out[k] : NULL;
    rdof_fmrx_feed(x->r, iq, n, disc, audio, &na);
    if (x->dtmf && audio)
        rdof_dtmfdet_feed(x->dtmf, audio, na);
    k = 0;
    if (x->streams & S_AUDIO)
        nout[k++] = na;
    if (x->streams & S_DISC)
        nout[k] = n;
}

/* A digit's JSON: its span in s from the stream's start (the audio's lag taken out), as the blocks' t */
static size_t dtmf_json(const fm_rx_t *x, const rdof_dtmf_run_t *r, char *out, size_t cap)
{
    const int n = snprintf(out, cap, "{\"digit\":\"%c\",\"t0\":%.3f,\"t1\":%.3f,\"s\":%.3f,\"dev_hz\":%.0f,"
                           "\"twist_db\":%.1f}", r->digit, r->t0 - x->delay_s, r->t1 - x->delay_s, r->t1 - r->t0,
                           r->dev_hz, r->twist_db);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

/* A digit kept for the summary: its JSON's length into one (0: none) */
static size_t dtmf_keep(fm_rx_t *x, const rdof_dtmf_run_t *r, char *one, size_t cap)
{
    if (x->ndigits < 64)
        x->digits[x->ndigits++] = r->digit, x->digits[x->ndigits] = 0;
    const size_t m = dtmf_json(x, r, one, cap);
    if (m && x->nruns + m + 2 < sizeof x->runs) {
        if (x->nruns)
            x->runs[x->nruns++] = ',';
        memcpy(x->runs + x->nruns, one, m);
        x->nruns += m;
        x->runs[x->nruns] = 0;
    }
    return m;
}

/* A digit: a record of its own, and kept for the summary */
static size_t dtmf_record(fm_rx_t *x, char *out, size_t cap)
{
    rdof_dtmf_run_t r;
    if (!x->dtmf || cap < 200 || !rdof_dtmfdet_take(x->dtmf, &r))
        return 0;
    char one[160];
    const size_t m = dtmf_keep(x, &r, one, sizeof one);
    if (!m)
        return 0;
    memcpy(out, "{\"dtmf\":", 8);
    memcpy(out + 8, one, m);
    out[8 + m] = '}';
    return 9 + m;
}

static size_t fm_rx_record(void *rx, char *out, size_t cap)
{
    fm_rx_t *x = rx;
    const size_t dn = dtmf_record(x, out, cap);
    if (dn)
        return dn;
    rdof_fmrx_block_t b;
    if (cap < 16 || !rdof_fmrx_block(x->r, &b))
        return 0;
    memcpy(out, "{\"block\":", 9);
    const size_t n = rdof_fmrx_block_json(&b, out + 9, cap - 10);
    if (!n)
        return 0;
    out[9 + n] = '}';
    return 10 + n;
}

static size_t fm_rx_summary(void *rx, char *out, size_t cap)
{
    fm_rx_t *x = rx;
    rdof_fmrx_summary_t s;
    rdof_fmrx_summary(x->r, &s);
    size_t n = rdof_fmrx_summary_json(x->r, &s, out, cap);
    if (!x->dtmf || !n || out[n - 1] != '}')
        return n;
    /* the digits: a run still going ends here, its record left out (the summary has it) */
    rdof_dtmfdet_finish(x->dtmf);
    rdof_dtmf_run_t r;
    char one[160];
    while (rdof_dtmfdet_take(x->dtmf, &r))
        dtmf_keep(x, &r, one, sizeof one);
    const int m = snprintf(out + n - 1, cap - (n - 1), ",\"dtmf\":{\"digits\":\"%s\",\"runs\":[%s]}}", x->digits,
                           x->runs);
    return m > 0 && (size_t)m < cap - (n - 1) ? n - 1 + (size_t)m : 0;
}

const rdof_sig_mode_t rdof_mode_fm = {
    .name = "fm",
    .about = "analog FM: tones, sweeps, DTMF, FSK/AFSK, noise or PCM; CTCSS or DCS; measured on receive",
    .tx_new = fm_tx_new,
    .tx_free = fm_tx_free,
    .tx_src = fm_tx_src,
    .tx_in_room = rdof_prog_tx_in_room,
    .tx_in = rdof_prog_tx_in,
    .tx_in_end = rdof_prog_tx_in_end,
    .tx_in_want = rdof_prog_tx_in_want,
    .tx_report = fm_tx_report,
    .rx_new = fm_rx_new,
    .rx_free = fm_rx_free,
    .rx_streams = fm_rx_streams,
    .rx_feed = fm_rx_feed,
    .rx_record = fm_rx_record,
    .rx_summary = fm_rx_summary,
};

#endif
