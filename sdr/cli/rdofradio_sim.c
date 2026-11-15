// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * rdofradio's device for no-RF tests (rdofradio_dev.h): --sim-sdr, a receiver paced by the wall clock, the
 * receive-only device beside an MMDVM modem or a split's radio. It lets rdofradio's receiver, bench line and modem
 * path run on a machine with no SDR (the host tests, with fakemodem).
 *
 * What it hears: quiet noise, or --sim-iq FILE:MHZ, a cu8 recording at the device's rate centered on MHZ, looped,
 * mixed to wherever the RX LO is (a channel in the recording shows where it would on the air).
 *
 * With --fpga-chz it is a PlutoSDR's FPGA channelizer too (rdof/sdr/chzhw.h): rdofradio_sim_chz gives the block's
 * registers, as the HLS block answers them (ID and CAPS once AP_CTRL runs, ENABLE resets it, APPLY's toggle takes the
 * new steps), and read_words hands over its DMA frames: the samples, at 12 bits, through the bit-exact model
 * (rdof/sdr/chzfx.h). Before ENABLE the DMA holds the stock design's samples, one a word. A reader late by more
 * than SIM_LATE_S loses samples, as the DMA does (STATUS's DMA_OVF, gaps in the frames' numbers). For the FPGA
 * path's tests: rdofradio's probe, start, parser, AFC and raw capture, with no Pluto.
 */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rdof/sdr/iq.h"
#include "rdofradio_dev.h"

#define SIM_GAIN 20.0
#define SIM_2PI 6.283185307179586
#define SIM_BUF_S 0.02  /* a DMA buffer's input: 20 ms */
#define SIM_LATE_S 0.5  /* a reader this late loses the samples past it */
#define SIM_CHANNELS 16 /* CAPS: a 7020's block (a 7010's has fewer; rdofradio reads them) */
#define SIM_LANES 8
#define SIM_MAX_DECIM 60
#define SIM_MAX_CIC 64
#define SIM_TAPS 1024
#define SIM_HIST 1024

/* ── the block ─────────── */

static struct {
    pthread_mutex_t mu;
    bool made;
    uint32_t ap_ctrl, ctrl;
    uint32_t reg[RDOF_CHZHW_SPAN / 4]; /* what was written, by word */
    rdof_chzfx_t *m;                   /* ENABLE on: the model */
    uint32_t status;
} B = {.mu = PTHREAD_MUTEX_INITIALIZER};

static uint32_t reg(uint32_t off) { return B.reg[(off & (RDOF_CHZHW_SPAN - 1)) / 4]; }

static void enable(void)
{
    static rdof_chzfx_cfg_t c;
    memset(&c, 0, sizeof c);
    c.decim = (int)reg(RDOF_CHZHW_DECIM);
    c.taps = (int)reg(RDOF_CHZHW_TAPS);
    c.channels = (int)reg(RDOF_CHZHW_CHANNELS);
    c.out_shift = (int)reg(RDOF_CHZHW_OUT_SHIFT);
    c.cic_r = (int)reg(RDOF_CHZHW_CIC_R);
    c.cic_shift = (int)reg(RDOF_CHZHW_CIC_SHIFT);
    for (int i = 0; i < c.taps && i < RDOF_CHZFX_MAX_TAPS; i++)
        c.coef[i] = (int32_t)reg(RDOF_CHZHW_COEF + 4u * (uint32_t)i);
    for (int k = 0; k < c.channels && k < RDOF_CHZFX_MAX_CH; k++)
        c.inc[k] = reg(RDOF_CHZHW_INC + 4u * (uint32_t)k), c.phase0[k] = reg(RDOF_CHZHW_PHASE0 + 4u * (uint32_t)k);
    rdof_chzfx_free(B.m);
    /* a configuration past the model's: the block runs nothing (rdofradio checks before, as chzhw does) */
    B.m = rdof_chzfx_check(&c) ? NULL : rdof_chzfx_new(&c);
    B.status = B.m ? RDOF_CHZHW_STATUS_RUNNING : 0;
}

static uint32_t b_rd(void *ctx, uint32_t off)
{
    (void)ctx;
    pthread_mutex_lock(&B.mu);
    const bool on = B.ap_ctrl & 0x80; /* the HLS block answers once started */
    uint32_t v = 0;
    switch (off) {
    case RDOF_CHZHW_AP_CTRL: v = B.ap_ctrl; break;
    case RDOF_CHZHW_CTRL: v = B.ctrl; break;
    case RDOF_CHZHW_ID: v = on ? RDOF_CHZHW_ID_RCH1 : 0; break;
    case RDOF_CHZHW_CAPS0:
        v = on ? (uint32_t)SIM_MAX_CIC << 24 | (uint32_t)SIM_MAX_DECIM << 16 | SIM_LANES << 8 | SIM_CHANNELS : 0;
        break;
    case RDOF_CHZHW_CAPS1: v = on ? (uint32_t)SIM_HIST << 16 | SIM_TAPS : 0; break;
    case RDOF_CHZHW_STATUS: v = B.status; break;
    case RDOF_CHZHW_FRAMES: v = B.m ? B.m->seq : 0; break;
    default: v = reg(off);
    }
    pthread_mutex_unlock(&B.mu);
    return v;
}

static void b_wr(void *ctx, uint32_t off, uint32_t v)
{
    (void)ctx;
    pthread_mutex_lock(&B.mu);
    if (off == RDOF_CHZHW_AP_CTRL) {
        B.ap_ctrl = v;
    } else if (off == RDOF_CHZHW_CTRL) {
        const uint32_t was = B.ctrl;
        B.ctrl = v;
        if ((v & RDOF_CHZHW_CTRL_ENABLE) && !(was & RDOF_CHZHW_CTRL_ENABLE)) {
            enable();
        } else if (!(v & RDOF_CHZHW_CTRL_ENABLE)) {
            rdof_chzfx_free(B.m);
            B.m = NULL;
            B.status = 0;
        } else if (((v ^ was) & RDOF_CHZHW_CTRL_APPLY) && B.m) {
            for (int c = 0; c < B.m->cfg.channels; c++)
                rdof_chzfx_set_inc(B.m, c, reg(RDOF_CHZHW_INC + 4u * (uint32_t)c));
        }
    } else if (off < RDOF_CHZHW_SPAN && off != RDOF_CHZHW_ID && off != RDOF_CHZHW_CAPS0 && off != RDOF_CHZHW_CAPS1 &&
               off != RDOF_CHZHW_STATUS && off != RDOF_CHZHW_FRAMES) {
        B.reg[off / 4] = v;
    }
    pthread_mutex_unlock(&B.mu);
}

void rdofradio_sim_chz(rdof_chzhw_io_t *io)
{
    B.made = true;
    io->rd = b_rd, io->wr = b_wr, io->ctx = NULL;
}

/* ── the source ─────────── */

static char src_path[512];
static double src_hz;

int rdofradio_sim_source(const char *spec, char *err, size_t cap)
{
    const char *colon = strrchr(spec, ':');
    char *e = NULL;
    const double mhz = colon ? strtod(colon + 1, &e) : 0;
    if (!colon || colon == spec || e == colon + 1 || *e || mhz <= 0 || (size_t)(colon - spec) >= sizeof src_path) {
        snprintf(err, cap, "--sim-iq FILE:MHZ (a cu8 recording at -r, centered on MHZ)");
        return -1;
    }
    memcpy(src_path, spec, (size_t)(colon - spec));
    src_path[colon - spec] = 0;
    src_hz = mhz * 1e6;
    return 0;
}

typedef struct {
    rdofradio_dev_t d;
    uint64_t written;
    struct timespec t0;
    uint32_t noise; /* xorshift */
    double gain;
    double lo, lo_was; /* the RX LO; raw_tune's before */
    rdof_cf_t *iq;     /* --sim-iq, looped */
    size_t niq, pos;
    double ph;         /* the mix to the LO, radians */
    /* read_words */
    bool words, paused;
    rdof_cf_t *x;
    rdof_iq16_t *in, *out;
    uint64_t *w;
    int nbuf, nw_cap;
} sim_t;

/* n samples of what the antenna has */
static void source(sim_t *s, rdof_cf_t *x, int n)
{
    if (!s->iq) {
        uint32_t r = s->noise;
        for (int i = 0; i < n; i++) { /* quiet noise, a few cs8 counts */
            r ^= r << 13, r ^= r >> 17, r ^= r << 5;
            x[i].re = (float)((int)(r & 15) - 8) / 256.0f;
            r ^= r << 13, r ^= r >> 17, r ^= r << 5;
            x[i].im = (float)((int)(r & 15) - 8) / 256.0f;
        }
        s->noise = r ? r : 1u;
        return;
    }
    const double dph = SIM_2PI * (src_hz - s->lo) / s->d.rate;
    for (int i = 0; i < n; i++) {
        const rdof_cf_t v = s->iq[s->pos];
        s->pos = s->pos + 1 < s->niq ? s->pos + 1 : 0;
        const float c = (float)cos(s->ph), sn = (float)sin(s->ph);
        x[i].re = v.re * c - v.im * sn, x[i].im = v.re * sn + v.im * c;
        s->ph = fmod(s->ph + dph, SIM_2PI);
    }
}

static int16_t to12(float v)
{
    const long q = lrintf(v * 2048.0f);
    return (int16_t)(q > 2047 ? 2047 : q < -2048 ? -2048 : q);
}

static int s_rx(rdofradio_dev_t *d, double hz)
{
    ((sim_t *)d)->lo = hz;
    return 0;
}
static double s_gain(rdofradio_dev_t *d) { return ((sim_t *)d)->gain; }
static double s_bw(rdofradio_dev_t *d) { return d->rate; }
static int s_gain_set(rdofradio_dev_t *d, double db)
{
    ((sim_t *)d)->gain = db == db ? db : SIM_GAIN; /* NAN: the "AGC" */
    return 0;
}
static int s_start(rdofradio_dev_t *d)
{
    sim_t *s = (sim_t *)d;
    clock_gettime(CLOCK_MONOTONIC, &s->t0);
    s->written = 0;
    return 0;
}

static double elapsed(const sim_t *s)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - s->t0.tv_sec) + 1e-9 * (double)(now.tv_nsec - s->t0.tv_nsec);
}

/* until sample at is due */
static void pace(sim_t *s, uint64_t at)
{
    const double w = (double)at / s->d.rate - elapsed(s);
    if (w > 0) {
        const struct timespec ts = {(time_t)w, (long)((w - (double)(time_t)w) * 1e9)};
        nanosleep(&ts, NULL);
    }
}

static int s_read(rdofradio_dev_t *d, rdof_cf_t *x, int n, long timeout_us)
{
    (void)timeout_us;
    sim_t *s = (sim_t *)d;
    pace(s, s->written + (uint64_t)n);
    source(s, x, n);
    s->written += (uint64_t)n;
    return n;
}

static int s_read_words(rdofradio_dev_t *d, const uint64_t **w, long timeout_us)
{
    (void)timeout_us;
    sim_t *s = (sim_t *)d;
    if (s->paused)
        return RDOF_DEV_ERROR;
    /* late past SIM_LATE_S: the DMA had no buffer for those (the block ran on: its frames' numbers jump) */
    const uint64_t now_at = (uint64_t)(elapsed(s) * d->rate);
    if (now_at > s->written + (uint64_t)(SIM_LATE_S * d->rate)) {
        const uint64_t skip = now_at - s->written - (uint64_t)s->nbuf;
        pthread_mutex_lock(&B.mu);
        if (B.m) {
            const int step = rdof_chzfx_step(&B.m->cfg);
            B.m->seq += (uint32_t)(skip / (uint64_t)step);
            B.status |= RDOF_CHZHW_STATUS_DMA_OVF;
        }
        pthread_mutex_unlock(&B.mu);
        s->written += skip;
        if (s->iq)
            s->pos = (size_t)((s->pos + skip) % s->niq);
    }
    pace(s, s->written + (uint64_t)s->nbuf);
    source(s, s->x, s->nbuf);
    s->written += (uint64_t)s->nbuf;
    for (int i = 0; i < s->nbuf; i++)
        s->in[i] = (rdof_iq16_t){to12(s->x[i].re), to12(s->x[i].im)};
    size_t nw = 0;
    pthread_mutex_lock(&B.mu);
    if (B.m) {
        const int C = B.m->cfg.channels;
        const uint32_t seq0 = B.m->seq;
        const int got = rdof_chzfx_process(B.m, s->in, s->nbuf, s->out);
        for (int j = 0; j < got && nw + rdof_chzfx_frame_words(C) <= (size_t)s->nw_cap; j++)
            nw += rdof_chzfx_frame(RDOF_CHZFX_FRAME_CH, seq0 + (uint32_t)j, s->out + (size_t)j * C, C, s->w + nw);
    } else { /* the stock design: voltage0..3 a word, RX2 off */
        for (int i = 0; i < s->nbuf && nw < (size_t)s->nw_cap; i++)
            s->w[nw++] = (uint16_t)s->in[i].i | (uint64_t)(uint16_t)s->in[i].q << 16;
    }
    pthread_mutex_unlock(&B.mu);
    *w = s->w;
    return (int)nw;
}

/* a raw capture: the stream stops, the ADC's own samples, the stream again */
static void s_pause(rdofradio_dev_t *d) { ((sim_t *)d)->paused = true; }
static int s_resume(rdofradio_dev_t *d)
{
    sim_t *s = (sim_t *)d;
    s->paused = false;
    s->written = (uint64_t)(elapsed(s) * d->rate); /* the stream starts now: nothing owed */
    return 0;
}
static int s_raw_fill(rdofradio_dev_t *d, long n, double rate, int16_t *out, long *after, char *err, size_t cap)
{
    sim_t *s = (sim_t *)d;
    if (rate != d->rate) {
        snprintf(err, cap, "the simulated SDR runs at %.0f S/s only", d->rate);
        return -1;
    }
    rdof_cf_t x[1024];
    for (long k = 0; k < n;) {
        const int m = n - k < 1024 ? (int)(n - k) : 1024;
        pace(s, s->written + (uint64_t)m);
        source(s, x, m);
        s->written += (uint64_t)m;
        for (int i = 0; i < m; i++)
            out[2 * (k + i)] = to12(x[i].re), out[2 * (k + i) + 1] = to12(x[i].im);
        k += m;
    }
    *after = 0;
    return 0;
}
static int s_raw_tune(rdofradio_dev_t *d, double rate, double lo_hz, double *got, char *err, size_t cap)
{
    sim_t *s = (sim_t *)d;
    if (rate && rate != d->rate) {
        snprintf(err, cap, "the simulated SDR runs at %.0f S/s only", d->rate);
        return -1;
    }
    s->lo_was = s->lo;
    if (lo_hz)
        s->lo = lo_hz;
    *got = d->rate;
    return 0;
}
static int s_raw_untune(rdofradio_dev_t *d)
{
    sim_t *s = (sim_t *)d;
    s->lo = s->lo_was;
    return 0;
}

static void s_close(rdofradio_dev_t *d)
{
    sim_t *s = (sim_t *)d;
    free(s->iq), free(s->x), free(s->in), free(s->out), free(s->w);
    free(s);
}

static rdof_cf_t *load_cu8(const char *path, size_t *n, char *err, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, cap, "--sim-iq %s: cannot open", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *raw = size >= 2 ? malloc((size_t)size) : NULL;
    rdof_cf_t *x = raw ? malloc(sizeof *x * (size_t)(size / 2)) : NULL;
    const bool ok = x && fread(raw, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    if (!ok) {
        snprintf(err, cap, "--sim-iq %s: unreadable or empty", path);
        free(raw), free(x);
        return NULL;
    }
    *n = (size_t)size / 2;
    rdof_iq_from_cu8(raw, *n, x);
    free(raw);
    return x;
}

rdofradio_dev_t *rdofradio_sim_open(const rdofradio_dev_setup_t *su, char *err, size_t cap)
{
    if (!su->rx_only) {
        snprintf(err, cap, "--sim-sdr only receives: with --mmdvm, --split-tx or --listen");
        return NULL;
    }
    if (su->words_rate > 0 && !B.made) {
        snprintf(err, cap, "--sim-sdr: the DMA's words with no channelizer block (rdofradio_sim_chz)");
        return NULL;
    }
    sim_t *s = calloc(1, sizeof *s);
    if (!s) {
        snprintf(err, cap, "no memory");
        return NULL;
    }
    snprintf(s->d.name, sizeof s->d.name, su->words_rate > 0 ? "simulated SDR, FPGA channelizer" : "simulated SDR");
    s->d.rate = su->rate;
    s->d.tx_rate = su->rate;
    s->d.iq_scale = su->words_rate > 0 ? 16.0f : 1.0f; /* the block's input is an AD9361's 12 bits */
    s->noise = 0x12345678u;
    s->gain = SIM_GAIN;
    s->lo = s->lo_was = su->rx_hz;
    s->d.set_rx_freq = s_rx;
    s->d.rx_gain = s_gain;
    s->d.set_rx_gain = s_gain_set;
    s->d.bandwidth = s_bw;
    s->d.start = s_start;
    s->d.read = s_read;
    s->d.close = s_close;
    if (src_path[0] && !(s->iq = load_cu8(src_path, &s->niq, err, cap))) {
        free(s);
        return NULL;
    }
    if (su->words_rate > 0) {
        s->words = true;
        s->nbuf = (int)(su->rate * SIM_BUF_S);
        s->nw_cap = s->nbuf + (int)(su->words_rate * SIM_BUF_S) + 64;
        s->x = malloc(sizeof *s->x * (size_t)s->nbuf);
        s->in = malloc(sizeof *s->in * (size_t)s->nbuf);
        s->out = malloc(sizeof *s->out * (size_t)s->nbuf * RDOF_CHZFX_MAX_CH);
        s->w = malloc(sizeof *s->w * (size_t)s->nw_cap);
        if (!s->x || !s->in || !s->out || !s->w) {
            snprintf(err, cap, "no memory");
            s_close(&s->d);
            return NULL;
        }
        s->d.read = NULL;
        s->d.read_words = s_read_words;
        s->d.rx_pause = s_pause;
        s->d.rx_resume = s_resume;
        s->d.raw_fill = s_raw_fill;
        s->d.raw_tune = s_raw_tune;
        s->d.raw_untune = s_raw_untune;
    }
    return &s->d;
}
