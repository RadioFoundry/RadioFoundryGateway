// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#include "rdof/sdr/chzhw.h"

#if RDOF_FEATURE_SDR

#include <stdio.h>
#include <string.h>

#define PROBE_READS 10000 /* the read-only registers read 0 until the block's first call after ap_start */

static uint32_t rd(const rdof_chzhw_t *h, uint32_t off)
{
    return h->io.rd(h->io.ctx, off);
}

static void wr(const rdof_chzhw_t *h, uint32_t off, uint32_t v)
{
    h->io.wr(h->io.ctx, off, v);
}

int rdof_chzhw_probe(rdof_chzhw_t *h, const rdof_chzhw_io_t *io, char *err, size_t cap)
{
    memset(h, 0, sizeof *h);
    h->io = *io;
    wr(h, RDOF_CHZHW_CTRL, 0);
    if ((rd(h, RDOF_CHZHW_AP_CTRL) & 0x80) == 0) /* not on auto-restart: never started */
        wr(h, RDOF_CHZHW_AP_CTRL, RDOF_CHZHW_AP_RUN);
    uint32_t id = 0;
    for (int i = 0; i < PROBE_READS && id != RDOF_CHZHW_ID_RCH1; i++)
        id = rd(h, RDOF_CHZHW_ID);
    if (id != RDOF_CHZHW_ID_RCH1) {
        snprintf(err, cap, "the channelizer's ID is 0x%08x, not 0x%08x", (unsigned)id, RDOF_CHZHW_ID_RCH1);
        return -1;
    }
    const uint32_t c0 = rd(h, RDOF_CHZHW_CAPS0), c1 = rd(h, RDOF_CHZHW_CAPS1);
    h->channels = (int)(c0 & 0xFF);
    h->lanes = (int)(c0 >> 8 & 0xFF);
    h->max_decim = (int)(c0 >> 16 & 0xFF);
    h->max_cic = (int)(c0 >> 24);
    h->max_taps = (int)(c1 & 0xFFFF);
    h->hist = (int)(c1 >> 16);
    if (h->channels < 1 || h->max_decim < 1 || h->max_taps < 1 || h->hist < 2) {
        snprintf(err, cap, "the channelizer's CAPS make no sense: 0x%08x 0x%08x", (unsigned)c0, (unsigned)c1);
        return -1;
    }
    return 0;
}

int rdof_chzhw_start(rdof_chzhw_t *h, const rdof_chzfx_cfg_t *cfg, int snap_period, int snap_len, char *err,
                     size_t cap)
{
    if (rdof_chzfx_check(cfg)) {
        snprintf(err, cap, "a channelizer configuration the model refuses");
        return -1;
    }
    if (cfg->channels > h->channels || cfg->decim > h->max_decim || cfg->taps > h->max_taps ||
        cfg->taps + cfg->decim > h->hist) {
        snprintf(err, cap, "%d channels, D %d, %d taps: the block takes %d, %d, %d (history %d)", cfg->channels,
                 cfg->decim, cfg->taps, h->channels, h->max_decim, h->max_taps, h->hist);
        return -1;
    }
    const int R = cfg->cic_r > 1 ? cfg->cic_r : 1;
    if (R > 1 && R > h->max_cic) {
        snprintf(err, cap, "a CIC of %d: the block takes %d", R, h->max_cic);
        return -1;
    }
    if (snap_period < 0 || snap_len < 0 || (snap_len && snap_len > snap_period) || (snap_len && R > 1)) {
        snprintf(err, cap, "RAW frames: %d every %d%s", snap_len, snap_period, R > 1 ? ", with a CIC" : "");
        return -1;
    }
    h->ctrl = 0;
    wr(h, RDOF_CHZHW_CTRL, h->ctrl);
    wr(h, RDOF_CHZHW_DECIM, (uint32_t)cfg->decim);
    wr(h, RDOF_CHZHW_TAPS, (uint32_t)cfg->taps);
    wr(h, RDOF_CHZHW_CHANNELS, (uint32_t)cfg->channels);
    wr(h, RDOF_CHZHW_OUT_SHIFT, (uint32_t)cfg->out_shift);
    wr(h, RDOF_CHZHW_SNAP_PERIOD, (uint32_t)snap_period);
    wr(h, RDOF_CHZHW_SNAP_LEN, (uint32_t)snap_len);
    if (h->max_cic) {
        wr(h, RDOF_CHZHW_CIC_R, (uint32_t)R);
        wr(h, RDOF_CHZHW_CIC_SHIFT, (uint32_t)cfg->cic_shift);
    }
    for (int i = 0; i < cfg->taps; i++)
        wr(h, RDOF_CHZHW_COEF + 4u * (uint32_t)i, (uint32_t)cfg->coef[i]);
    for (int c = 0; c < cfg->channels; c++) {
        wr(h, RDOF_CHZHW_INC + 4u * (uint32_t)c, cfg->inc[c]);
        wr(h, RDOF_CHZHW_PHASE0 + 4u * (uint32_t)c, cfg->phase0[c]);
    }
    h->nch = cfg->channels;
    h->ctrl = RDOF_CHZHW_CTRL_ENABLE;
    wr(h, RDOF_CHZHW_CTRL, h->ctrl);
    return 0;
}

void rdof_chzhw_set_inc(rdof_chzhw_t *h, const uint32_t *inc)
{
    for (int c = 0; c < h->nch; c++)
        wr(h, RDOF_CHZHW_INC + 4u * (uint32_t)c, inc[c]);
    h->ctrl ^= RDOF_CHZHW_CTRL_APPLY;
    wr(h, RDOF_CHZHW_CTRL, h->ctrl);
}

uint32_t rdof_chzhw_status(const rdof_chzhw_t *h)
{
    return rd(h, RDOF_CHZHW_STATUS);
}

uint32_t rdof_chzhw_frames(const rdof_chzhw_t *h)
{
    return rd(h, RDOF_CHZHW_FRAMES);
}

void rdof_chzhw_stop(rdof_chzhw_t *h)
{
    h->ctrl = 0;
    wr(h, RDOF_CHZHW_CTRL, 0);
}

/* ── the frames ── */

void rdof_chzhw_rx_init(rdof_chzhw_rx_t *r, int channels,
                        void (*on_ch)(void *ctx, uint32_t seq, const rdof_iq16_t *x, int n),
                        void (*on_raw)(void *ctx, uint32_t seq, const rdof_iq16_t *x, int n), void *ctx)
{
    memset(r, 0, sizeof *r);
    r->channels = channels;
    r->on_ch = on_ch;
    r->on_raw = on_raw;
    r->ctx = ctx;
}

/* The words the frame headed by w takes, or 0 if w isn't a header this stream can have. */
static size_t frame_len(const rdof_chzhw_rx_t *r, uint64_t w)
{
    if ((w >> 48) != RDOF_CHZFX_MAGIC)
        return 0;
    const int type = (int)(w >> 44 & 0xF), n = (int)(w >> 32 & 0xFFF);
    if (!(type == RDOF_CHZFX_FRAME_CH && n == r->channels) &&
        !(type == RDOF_CHZFX_FRAME_RAW && n >= 1 && n <= RDOF_CHZFX_MAX_DECIM))
        return 0;
    const size_t words = rdof_chzfx_frame_words(n);
    return words <= RDOF_CHZHW_RX_WORDS ? words : 0;
}

static void deliver(rdof_chzhw_rx_t *r, const uint64_t *w, size_t words)
{
    rdof_chzfx_frame_t f;
    if (rdof_chzfx_unframe(w, words, &f) <= 0)
        return;
    rdof_iq16_t x[2 * RDOF_CHZHW_RX_WORDS];
    for (int k = 0; k < f.n; k++)
        x[k] = rdof_chzfx_frame_sample(&f, k);
    if (f.type == RDOF_CHZFX_FRAME_RAW) {
        r->raw_frames++;
        if (r->on_raw)
            r->on_raw(r->ctx, f.seq, x, f.n);
        return;
    }
    if (r->have_seq && f.seq != r->next_seq) {
        const uint32_t gap = f.seq - r->next_seq;
        if (gap < 0x80000000u)
            r->lost += gap;
    }
    r->have_seq = 1;
    r->next_seq = f.seq + 1;
    r->ch_frames++;
    if (r->on_ch)
        r->on_ch(r->ctx, f.seq, x, f.n);
}

void rdof_chzhw_rx_feed(rdof_chzhw_rx_t *r, const uint64_t *w, size_t n)
{
    size_t i = 0;
    if (r->npart) { /* a frame begun in the last buffer */
        const size_t need = frame_len(r, r->part[0]), take = need - r->npart < n ? need - r->npart : n;
        memcpy(r->part + r->npart, w, take * sizeof *w);
        r->npart += take;
        i = take;
        if (r->npart < need)
            return;
        deliver(r, r->part, need);
        r->npart = 0;
    }
    while (i < n) {
        const size_t len = frame_len(r, w[i]);
        if (!len) {
            r->junk++;
            i++;
        } else if (i + len <= n) {
            deliver(r, w + i, len);
            i += len;
        } else {
            memcpy(r->part, w + i, (n - i) * sizeof *w);
            r->npart = n - i;
            i = n;
        }
    }
}

#endif
