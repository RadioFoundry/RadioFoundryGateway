// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Host tests for the FPGA channelizer's driver (chzhw.h): the registers against a fake register file, and the frame
 * parser against the model's own frames, fed in uneven buffers with junk, a lost frame and RAW frames between.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/sdr/chzhw.h"

static int checks, failures;

#define CHECK(cond, ...)                                                                                     \
    do {                                                                                                     \
        checks++;                                                                                            \
        if (!(cond)) {                                                                                       \
            failures++;                                                                                      \
            printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                           \
            printf(__VA_ARGS__);                                                                             \
            printf("\n");                                                                                    \
        }                                                                                                    \
    } while (0)

/* A register file: the read-only registers as a started block shows them, every write logged. */
typedef struct {
    uint32_t reg[RDOF_CHZHW_SPAN / 4];
    int started;          /* AP_CTRL written: the read-only registers show */
    int id_delay;         /* reads of ID before it shows */
    uint32_t ctrl_log[16];
    int nctrl;
} fake_t;

static uint32_t fake_rd(void *ctx, uint32_t off)
{
    fake_t *f = ctx;
    if (off == RDOF_CHZHW_ID || off == RDOF_CHZHW_CAPS0 || off == RDOF_CHZHW_CAPS1) {
        if (!f->started || (off == RDOF_CHZHW_ID && f->id_delay-- > 0))
            return 0;
        return off == RDOF_CHZHW_ID ? RDOF_CHZHW_ID_RCH1 : off == RDOF_CHZHW_CAPS0 ? 60u << 16 | 8u << 8 | 16u
                                                                                  : 1024u << 16 | 1024u;
    }
    return f->reg[off / 4];
}

static void fake_wr(void *ctx, uint32_t off, uint32_t v)
{
    fake_t *f = ctx;
    if (off == RDOF_CHZHW_AP_CTRL && v == RDOF_CHZHW_AP_RUN)
        f->started = 1;
    if (off == RDOF_CHZHW_CTRL && f->nctrl < 16)
        f->ctrl_log[f->nctrl++] = v;
    f->reg[off / 4] = v;
}

static void test_registers(void)
{
    static fake_t f;
    memset(&f, 0, sizeof f);
    f.id_delay = 3;
    rdof_chzhw_io_t io = {fake_rd, fake_wr, &f};
    rdof_chzhw_t h;
    char err[160];
    CHECK(rdof_chzhw_probe(&h, &io, err, sizeof err) == 0, "%s", err);
    CHECK(f.started && f.reg[0] == RDOF_CHZHW_AP_RUN, "AP_CTRL 0x81 written");
    CHECK(h.channels == 16 && h.lanes == 8 && h.max_decim == 60 && h.max_taps == 1024 && h.hist == 1024,
          "CAPS: %d ch, %d lanes, D %d, %d taps, history %d", h.channels, h.lanes, h.max_decim, h.max_taps, h.hist);

    static rdof_chzfx_cfg_t cfg;
    const double off[3] = {-100e3, 12.5e3, 300e3};
    CHECK(rdof_chzfx_design(&cfg, 2.4e6, 50, off, 3, 7.5e3) == 0, "design");
    f.nctrl = 0;
    CHECK(rdof_chzhw_start(&h, &cfg, 4800, 82, err, sizeof err) == 0, "%s", err);
    CHECK(f.nctrl == 2 && f.ctrl_log[0] == 0 && f.ctrl_log[1] == RDOF_CHZHW_CTRL_ENABLE, "CTRL off, then ENABLE");
    CHECK(f.reg[RDOF_CHZHW_DECIM / 4] == 50 && f.reg[RDOF_CHZHW_TAPS / 4] == (uint32_t)cfg.taps &&
              f.reg[RDOF_CHZHW_CHANNELS / 4] == 3 && f.reg[RDOF_CHZHW_OUT_SHIFT / 4] == (uint32_t)cfg.out_shift &&
              f.reg[RDOF_CHZHW_SNAP_PERIOD / 4] == 4800 && f.reg[RDOF_CHZHW_SNAP_LEN / 4] == 82,
          "the scalars");
    int coef_ok = 1;
    for (int i = 0; i < cfg.taps; i++)
        coef_ok &= f.reg[(RDOF_CHZHW_COEF + 4 * i) / 4] == (uint32_t)cfg.coef[i]; /* negative ones sign-extended */
    CHECK(coef_ok, "COEF");
    CHECK(f.reg[(RDOF_CHZHW_INC + 4) / 4] == cfg.inc[1] && f.reg[(RDOF_CHZHW_PHASE0 + 8) / 4] == cfg.phase0[2],
          "INC and PHASE0");

    /* a retune: the steps, then APPLY toggled, ENABLE kept */
    uint32_t inc[3] = {1, 2, 3};
    f.nctrl = 0;
    rdof_chzhw_set_inc(&h, inc);
    rdof_chzhw_set_inc(&h, inc);
    CHECK(f.reg[(RDOF_CHZHW_INC + 8) / 4] == 3 && f.nctrl == 2 &&
              f.ctrl_log[0] == (RDOF_CHZHW_CTRL_ENABLE | RDOF_CHZHW_CTRL_APPLY) &&
              f.ctrl_log[1] == RDOF_CHZHW_CTRL_ENABLE,
          "APPLY toggles: 0x%x 0x%x", f.ctrl_log[0], f.ctrl_log[1]);
    rdof_chzhw_stop(&h);
    CHECK(f.reg[RDOF_CHZHW_CTRL / 4] == 0, "stop: CTRL 0");

    /* past the block's sizes */
    static rdof_chzfx_cfg_t big;
    CHECK(rdof_chzfx_design(&big, 2.88e6, 60, off, 3, 7.5e3) == 0, "design at D 60");
    h.max_taps = 900;
    CHECK(rdof_chzhw_start(&h, &big, 0, 0, err, sizeof err) == -1 && strstr(err, "taps"), "refused: %s", err);
    h.max_taps = 1024;
    CHECK(rdof_chzhw_start(&h, &cfg, 10, 11, err, sizeof err) == -1, "RAW frames longer than their period");

    /* a block that never shows its ID */
    static fake_t dead;
    memset(&dead, 0, sizeof dead);
    dead.id_delay = 1 << 30;
    rdof_chzhw_io_t dio = {fake_rd, fake_wr, &dead};
    CHECK(rdof_chzhw_probe(&h, &dio, err, sizeof err) == -1 && strstr(err, "ID"), "no ID: %s", err);
}

/* What the parser hands over, kept to compare. */
typedef struct {
    rdof_iq16_t ch[4096][4];
    uint32_t seq[4096];
    int n;
    int raw, raw_samples;
} got_t;

static void on_ch(void *ctx, uint32_t seq, const rdof_iq16_t *x, int n)
{
    got_t *g = ctx;
    if (g->n < 4096) {
        memcpy(g->ch[g->n], x, (size_t)n * sizeof *x);
        g->seq[g->n++] = seq;
    }
}

static void on_raw(void *ctx, uint32_t seq, const rdof_iq16_t *x, int n)
{
    got_t *g = ctx;
    g->raw++;
    g->raw_samples += n;
}

static void test_frames(void)
{
    enum { C = 4, D = 50, N = 50 * 400 };
    static rdof_chzfx_cfg_t cfg;
    const double off[C] = {-300e3, -25e3, 12.5e3, 400e3};
    CHECK(rdof_chzfx_design(&cfg, 2.4e6, D, off, C, 7.5e3) == 0, "design");
    rdof_chzfx_t *m = rdof_chzfx_new(&cfg);
    static rdof_iq16_t in[N], out[N / D * C + C];
    uint32_t s = 1;
    for (int i = 0; i < N; i++) {
        s = s * 1664525u + 1013904223u;
        in[i].i = (int16_t)((int)(s >> 20) - 2048);
        in[i].q = (int16_t)((int)(s >> 8 & 0xFFF) - 2048);
    }
    const int nout = rdof_chzfx_process(m, in, N, out);
    rdof_chzfx_free(m);

    /* the stream: a RAW frame (the D inputs) before every 10th channel frame, junk before frame 7, frame 100 lost */
    static uint64_t words[N / D * 40];
    size_t nw = 0;
    int in_at = 0;
    for (int k = 0; k < nout; k++) {
        const int nin = k == 0 ? 1 : D;
        if (k % 10 == 0)
            nw += rdof_chzfx_frame(RDOF_CHZFX_FRAME_RAW, (uint32_t)k, in + in_at, nin, words + nw);
        in_at += nin;
        if (k == 7) {
            words[nw++] = 0x1234;
            words[nw++] = (uint64_t)RDOF_CHZFX_MAGIC << 48 | 5ull << 32; /* a header for 5 channels: junk */
        }
        if (k == 100)
            continue;
        nw += rdof_chzfx_frame(RDOF_CHZFX_FRAME_CH, (uint32_t)k, out + (size_t)k * C, C, words + nw);
    }

    static got_t g;
    memset(&g, 0, sizeof g);
    rdof_chzhw_rx_t r;
    rdof_chzhw_rx_init(&r, C, on_ch, on_raw, &g);
    /* buffers of uneven sizes, down to one word, so frames straddle them */
    const size_t sizes[] = {1, 2, 3, 5, 7, 11, 64, 1000};
    size_t at = 0;
    for (int b = 0; at < nw; b++) {
        size_t len = sizes[b % 8];
        if (len > nw - at)
            len = nw - at;
        rdof_chzhw_rx_feed(&r, words + at, len);
        at += len;
    }

    CHECK(g.n == nout - 1 && r.ch_frames == (uint64_t)nout - 1, "channel frames: %d of %d", g.n, nout - 1);
    CHECK(r.lost == 1, "lost %llu", (unsigned long long)r.lost);
    CHECK(r.junk == 2, "junk %llu", (unsigned long long)r.junk);
    const int nraw = (nout + 9) / 10;
    CHECK(g.raw == nraw && g.raw_samples == 1 + (nraw - 1) * D, "RAW frames %d, samples %d", g.raw, g.raw_samples);
    int same = 1;
    for (int j = 0, k = 0; j < g.n; j++, k++) {
        if (k == 100)
            k++;
        same &= g.seq[j] == (uint32_t)k && !memcmp(g.ch[j], out + (size_t)k * C, C * sizeof *out);
    }
    CHECK(same, "the samples and numbers as the model put them out");
}

int main(void)
{
    test_registers();
    test_frames();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
