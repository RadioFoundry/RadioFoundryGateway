// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The receiver on channel samples from outside (rdof_sdr_rx_channels_in): the FPGA channelizer's model (chzfx.h)
 * in place of the float channelizer, as rdofradio --fpga-chz runs it on a Pluto.
 *
 *   scale  a tone through both reads the same level (RDOF_CHZFX_OUT_SCALE)
 *   noise  the gate's level on noise alone reads as the channelizer's (1 to 2), and RDOF_EXT_Q25 is what the
 *          filter gives
 *   e2e    e2e_288k.cu8 as 12-bit samples, through the model and its DMA frames (chzhw's parser) into the receiver,
 *          the AFC's corrections back to the model's NCOs: the same bursts and calls as the float path
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/fec.h"
#include "rdof/pdu.h"
#include "rdof/sdr/channelizer.h"
#include "rdof/sdr/chzfx.h"
#include "rdof/sdr/chzhw.h"
#include "rdof/sdr/iq.h"
#include "rdof/sdr/receiver.h"
#include "sdr_vectors.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef RDOF_SDR_DATA
#define RDOF_SDR_DATA "sdr-data/"
#endif

/* What the two paths may differ by (the filters are the same, the arithmetic isn't: overlap-save in float against a
 * direct-form FIR in integers, an NCO after the filter against one before it) */
#define TOL_SCALE_DB 0.05   /* a tone's level */
#define TOL_POS 2           /* a burst's position, 48 kS/s samples */
#define TOL_POWER_DB 0.5    /* a burst's power */
#define TOL_AFC_HZ 15.0     /* the AFC's correction at the end */

static int checks, failures;

#define CHECK(cond, ...)                                                                                     \
    do {                                                                                                     \
        checks++;                                                                                            \
        if (!(cond)) {                                                                                       \
            failures++;                                                                                      \
            if (failures <= 40) {                                                                            \
                printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                      \
                printf(__VA_ARGS__);                                                                         \
                printf("\n");                                                                                \
            }                                                                                                \
        }                                                                                                    \
    } while (0)

static rdof_iq16_t to12(rdof_cf_t x)
{
    const float i = nearbyintf(x.re * 2048.0f), q = nearbyintf(x.im * 2048.0f);
    rdof_iq16_t y = {(int16_t)(i > 2047 ? 2047 : i < -2048 ? -2048 : i),
                     (int16_t)(q > 2047 ? 2047 : q < -2048 ? -2048 : q)};
    return y;
}

/* ── scale ─────────────────────────────────────────────────────────────────── */

static void test_scale(void)
{
    const double fs = 288000, off[2] = {-75000, 25000};
    const int n = 288000 / 4, D = 6;
    rdof_cf_t *x = malloc(sizeof *x * (size_t)n);
    rdof_iq16_t *x16 = malloc(sizeof *x16 * (size_t)n), *y16 = malloc(sizeof *y16 * (size_t)(n / D + 1) * 2);
    for (int i = 0; i < n; i++) { /* a tone 1 kHz into channel 0, at a quarter of full scale */
        const double ph = 2 * M_PI * (off[0] + 1000) * i / fs;
        x[i].re = (float)(0.25 * cos(ph)), x[i].im = (float)(0.25 * sin(ph));
        x16[i] = to12(x[i]);
    }
    rdof_chz_t z;
    CHECK(rdof_chz_init(&z, fs, off, 2, D, RDOF_CHZ_OUT_FFT, RDOF_CHZ_CUTOFF_HZ) == 0, "chz init");
    const int stride = n / D + 64;
    rdof_cf_t *y = calloc((size_t)stride * 2, sizeof *y);
    const int got = rdof_chz_process(&z, x, n, y, stride);
    static rdof_chzfx_cfg_t cfg;
    CHECK(rdof_chzfx_design(&cfg, fs, D, off, 2, RDOF_CHZ_CUTOFF_HZ) == 0, "design");
    rdof_chzfx_t *m = rdof_chzfx_new(&cfg);
    const int got16 = rdof_chzfx_process(m, x16, n, y16);
    double pf = 0, px = 0, pa = 0;
    int k0 = 200, k1 = (got < got16 ? got : got16) - 100;
    for (int k = k0; k < k1; k++) {
        pf += (double)y[k].re * y[k].re + (double)y[k].im * y[k].im;
        const double i = y16[2 * k].i * RDOF_CHZFX_OUT_SCALE, q = y16[2 * k].q * RDOF_CHZFX_OUT_SCALE;
        px += i * i + q * q;
        const double ai = y16[2 * k + 1].i * RDOF_CHZFX_OUT_SCALE, aq = y16[2 * k + 1].q * RDOF_CHZFX_OUT_SCALE;
        pa += ai * ai + aq * aq; /* channel 1: the tone 101 kHz away, filtered out */
    }
    const double db = 10 * log10(px / pf), want = 20 * log10(0.25);
    CHECK(fabs(db) < TOL_SCALE_DB, "the model reads %.3f dB against the channelizer", db);
    CHECK(fabs(10 * log10(pf / (k1 - k0)) - want) < TOL_SCALE_DB, "the channelizer reads %.3f dBFS, want %.3f",
          10 * log10(pf / (k1 - k0)), want);
    CHECK(10 * log10(pa / px) < -60, "the other channel %.1f dB under", 10 * log10(pa / px));
    printf("  scale: a -12.04 dBFS tone reads %.3f dBFS through the channelizer, %+.4f dB more through the model "
           "(RDOF_CHZFX_OUT_SCALE = 1/%.0f); the next channel %.0f dB under\n",
           10 * log10(pf / (k1 - k0)), db, 1.0 / RDOF_CHZFX_OUT_SCALE, 10 * log10(pa / px));
    rdof_chzfx_free(m);
    rdof_chz_free(&z);
    free(x), free(x16), free(y16), free(y);
}

/* ── noise: the gate's level ─────────────────────────────────────────────────── */

static double gauss(unsigned *s)
{
    double u = 0, v = 0;
    while (u <= 0) {
        *s = *s * 1103515245u + 12345u;
        u = (*s >> 8) / 16777216.0;
    }
    *s = *s * 1103515245u + 12345u;
    v = (*s >> 8) / 16777216.0;
    return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}

static int cmp_float(const void *a, const void *b)
{
    const float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

static void test_noise(void)
{
    const double fs = 288000, off[4] = {-75000, -25000, 25000, 75000};
    const int D = 6, chunk = 32768, chunks = 40, n = chunk * chunks;
    rdof_cf_t *x = malloc(sizeof *x * (size_t)n);
    rdof_iq16_t *x16 = malloc(sizeof *x16 * (size_t)n);
    unsigned seed = 7;
    for (int i = 0; i < n; i++) { /* 20 steps of the 12-bit ADC an axis */
        x[i].re = (float)(gauss(&seed) * 20 / 2048), x[i].im = (float)(gauss(&seed) * 20 / 2048);
        x16[i] = to12(x[i]);
    }
    static rdof_chzfx_cfg_t cfg;
    CHECK(rdof_chzfx_design(&cfg, fs, D, off, 4, RDOF_CHZ_CUTOFF_HZ) == 0, "design");
    rdof_chzfx_t *m = rdof_chzfx_new(&cfg);
    rdof_iq16_t *y16 = malloc(sizeof *y16 * (size_t)(chunk / D + 1) * 4);
    rdof_sdr_channel_t ch[4];
    for (int c = 0; c < 4; c++)
        ch[c] = (rdof_sdr_channel_t){"n", 439e6 + off[c], -1, 1};
    rdof_sdr_rx_config_t rc = {fs, 439e6, -55, 0, false, 8192, true};
    static rdof_sdr_rx_t rx;
    CHECK(rdof_sdr_rx_init(&rx, &rc, ch, 4, NULL, 0) == 0, "external init");
    rdof_chz_t z;
    CHECK(rdof_chz_init(&z, fs, off, 4, D, RDOF_CHZ_OUT_FFT, RDOF_CHZ_CUTOFF_HZ) == 0, "chz init");
    rdof_cf_t *yz = malloc(sizeof *yz * (size_t)(chunk / D + 64) * 4);
    float *pw = malloc(sizeof *pw * (size_t)(n / D / RDOF_EXT_BLOCK + 1) * 4);
    int npw = 0;
    double ext_sum = 0, chz_sum = 0, ext_max = 0, chz_max = 0;
    int counted = 0;
    for (int k = 0; k < chunks; k++) {
        const int got = rdof_chzfx_process(m, x16 + k * chunk, chunk, y16);
        rdof_sdr_rx_channels_in(&rx, y16, got, RDOF_CHZFX_OUT_SCALE);
        rdof_chz_process(&z, x + k * chunk, chunk, yz, chunk / D + 64);
        for (int c = 0; c < 4; c++)
            for (int b = 0; b + RDOF_EXT_BLOCK <= got; b += RDOF_EXT_BLOCK) {
                double p = 0;
                for (int j = b; j < b + RDOF_EXT_BLOCK; j++)
                    p += (double)rx.y[(size_t)c * rx.stride + j].re * rx.y[(size_t)c * rx.stride + j].re +
                         (double)rx.y[(size_t)c * rx.stride + j].im * rx.y[(size_t)c * rx.stride + j].im;
                if (k >= 2)
                    pw[npw++] = (float)(p / RDOF_EXT_BLOCK);
            }
        if (k < 2)
            continue; /* the filters' start, the floor's first chunks */
        for (int c = 0; c < 4; c++) {
            ext_sum += rx.chz.level[c], chz_sum += z.level[c];
            ext_max = fmax(ext_max, rx.chz.level[c]), chz_max = fmax(chz_max, z.level[c]);
        }
        counted += 4;
    }
    /* the 25th percentile's share of the mean, through this filter */
    double mean = 0;
    for (int i = 0; i < npw; i++)
        mean += pw[i];
    mean /= npw;
    qsort(pw, (size_t)npw, sizeof *pw, cmp_float);
    const double q25 = pw[npw / 4] / mean;
    CHECK(fabs(q25 - RDOF_EXT_Q25) < 0.03, "the 25th percentile is %.3f of the mean, RDOF_EXT_Q25 %.2f", q25,
          RDOF_EXT_Q25);
    CHECK(ext_max < RDOF_GATE_LEVEL && ext_sum / counted > 1.0 && ext_sum / counted < 2.0,
          "noise reads %.2f on average, %.2f at most", ext_sum / counted, ext_max);
    printf("  noise: the gate's level %.2f on average, %.2f at most (the channelizer's %.2f, %.2f); a block's 25th "
           "percentile %.3f of the mean\n",
           ext_sum / counted, ext_max, chz_sum / counted, chz_max, q25);

    /* a signal 10 dB over the noise in one channel opens it, the others stay shut */
    for (int i = 0; i < chunk; i++) {
        const double ph = 2 * M_PI * off[2] * i / fs, a = 40.0 / 2048; /* about 18 dB over the noise in a channel */
        rdof_cf_t s = {(float)(x[i].re + a * cos(ph)), (float)(x[i].im + a * sin(ph))};
        x16[i] = to12(s);
    }
    const int got = rdof_chzfx_process(m, x16, chunk, y16);
    rdof_sdr_rx_channels_in(&rx, y16, got, RDOF_CHZFX_OUT_SCALE);
    CHECK(rx.chz.level[2] > 3 * RDOF_GATE_LEVEL && rx.chz.level[0] < RDOF_GATE_LEVEL &&
              rx.chz.level[1] < RDOF_GATE_LEVEL && rx.chz.level[3] < RDOF_GATE_LEVEL,
          "a tone in channel 2: levels %.2f %.2f %.2f %.2f", rx.chz.level[0], rx.chz.level[1], rx.chz.level[2],
          rx.chz.level[3]);
    /* a chunk of zeros (frames lost) reads 0 and keeps the floor */
    memset(y16, 0, sizeof *y16 * (size_t)got * 4);
    const float floor0 = rx.ext_floor[(rx.ext_ifloor + RDOF_EXT_FLOORS - 1) % RDOF_EXT_FLOORS];
    rdof_sdr_rx_channels_in(&rx, y16, got, RDOF_CHZFX_OUT_SCALE);
    CHECK(rx.chz.level[2] == 0 && rx.ext_floor[(rx.ext_ifloor + RDOF_EXT_FLOORS - 1) % RDOF_EXT_FLOORS] == floor0,
          "zeros: level %.2f", rx.chz.level[2]);
    /* not external: the entry takes nothing */
    static rdof_sdr_rx_t plain;
    rc.external = false;
    rc.max_chunk = 32768;
    CHECK(rdof_sdr_rx_init(&plain, &rc, ch, 4, NULL, 0) == 0, "init");
    CHECK(rdof_sdr_rx_channels_in(&plain, y16, got, RDOF_CHZFX_OUT_SCALE) == 0 && plain.got == 0,
          "a receiver with its own channelizer takes no channel samples");
    rdof_sdr_rx_free(&plain);
    rdof_sdr_rx_free(&rx);
    rdof_chz_free(&z);
    rdof_chzfx_free(m);
    free(x), free(x16), free(y16), free(yz), free(pw);
}

/* ── e2e: the recording both ways ─────────────────────────────────────────── */

#define MAX_REC 4096
typedef struct {
    int c;
    int64_t pos;
    int dt, slot, cc, n, family;
    float power, cfo;
    uint8_t burst[RDOF_BURST_BYTES];
} rec_t;

typedef struct {
    char ch[16], action[16];
    uint32_t src, dst;
} call_t;

typedef struct {
    rec_t rec[MAX_REC];
    int nrec;
    call_t call[256];
    int ncall;
    long json;
} run_t;

static void on_burst(void *ctx, int c, const rdof_sdr_burst_t *b, double t)
{
    run_t *r = ctx;
    if (r->nrec == MAX_REC)
        return;
    rec_t *o = &r->rec[r->nrec++];
    o->c = c, o->pos = b->position, o->dt = b->b.dt, o->slot = b->b.slot, o->cc = b->b.cc, o->n = b->b.n;
    o->family = (int)b->b.family, o->power = (float)b->b.power_dbfs, o->cfo = b->freq_offset_hz;
    memcpy(o->burst, b->b.burst, RDOF_BURST_BYTES);
}

static void on_call(void *ctx, const char *channel, const char *action, uint32_t src, uint32_t dst)
{
    run_t *r = ctx;
    if (r->ncall == 256)
        return;
    call_t *o = &r->call[r->ncall++];
    snprintf(o->ch, sizeof o->ch, "%s", channel);
    snprintf(o->action, sizeof o->action, "%s", action);
    o->src = src, o->dst = dst;
}

static void on_json(void *ctx, const char *channel, const char *json) { ((run_t *)ctx)->json++; }

/* A data or control burst whose payload checks: BPTC, then its CRC. */
static bool crc_good(const rec_t *r, uint8_t out[12])
{
    uint32_t mask;
    bool rs = false;
    switch (r->dt) {
    case RDOF_DT_PI_HEADER: mask = RDOF_MASK_PI_HEADER; break;
    case RDOF_DT_VOICE_LC_HEADER: mask = RDOF_MASK_VOICE_LC_HEADER, rs = true; break;
    case RDOF_DT_TERMINATOR_WITH_LC: mask = RDOF_MASK_TERMINATOR_LC, rs = true; break;
    case RDOF_DT_CSBK: mask = RDOF_MASK_CSBK; break;
    case RDOF_DT_MBC_HEADER: mask = RDOF_MASK_MBC_HEADER; break;
    case RDOF_DT_DATA_HEADER: mask = RDOF_MASK_DATA_HEADER; break;
    case RDOF_DT_USBD: mask = RDOF_MASK_USBD; break;
    default: return false;
    }
    if (!rdof_bptc196_decode(r->burst, out))
        return false;
    return rs ? rdof_check_rs129(out, mask) : rdof_check_crc16(out, (uint16_t)mask);
}

/* the model in rdofradio's place: its frames through chzhw's parser, the AFC back to its NCOs */
typedef struct {
    rdof_chzfx_t *m;
    double fs, off[8];
    int C;
    rdof_iq16_t *y;  /* output instants, C each */
    int ny, cap;
    long corrections;
    uint32_t last_inc[8];
} fx_t;

static void fx_on_ch(void *ctx, uint32_t seq, const rdof_iq16_t *x, int n)
{
    fx_t *f = ctx;
    if (f->ny < f->cap)
        memcpy(f->y + (size_t)f->ny++ * f->C, x, sizeof *x * (size_t)n);
}

static int64_t fx_correction(void *ctx, int c, double hz)
{
    fx_t *f = ctx;
    f->last_inc[c] = rdof_chzfx_inc(f->off[c] + hz, f->fs);
    rdof_chzfx_set_inc(f->m, c, f->last_inc[c]);
    f->corrections++;
    /* the step changes after the next output instant; the filter's middle passes it 8 later (16 D + 1 taps) */
    return (int64_t)f->m->seq + 1 + 8;
}

static void run_float(const rdof_cf_t *x, int n, const rdof_sdr_channel_t *ch, run_t *r, double *afc, int *rssi)
{
    static rdof_sdr_rx_t rx;
    rdof_sdr_rx_config_t cfg = {288e3, 0.0, V_E2E_RSSI_OFFSET, 0.0, true, V_E2E_CHUNK};
    rdof_sink_t sink = {.on_json = on_json, .on_call = on_call, .ctx = r};
    CHECK(rdof_sdr_rx_init(&rx, &cfg, ch, (int)V_E2E_CHANS_N, &sink, V_E2E_T0) == 0, "float init");
    rx.on_burst = on_burst, rx.burst_ctx = r;
    for (int i = 0; i < n; i += V_E2E_CHUNK)
        rdof_sdr_rx_process(&rx, x + i, n - i < V_E2E_CHUNK ? n - i : V_E2E_CHUNK);
    for (int c = 0; c < (int)V_E2E_CHANS_N; c++)
        afc[c] = rx.chz.correction_hz[c], rssi[c] = rx.rssi[c];
    rdof_sdr_rx_free(&rx);
}

static void run_fx(const rdof_cf_t *x, int n, const rdof_sdr_channel_t *ch, run_t *r, double *afc, int *rssi,
                   fx_t *f, bool afc_on)
{
    const int C = (int)V_E2E_CHANS_N, D = 6;
    static rdof_sdr_rx_t rx;
    rdof_sdr_rx_config_t cfg = {288e3, V_E2E_CENTER, V_E2E_RSSI_OFFSET, 0.0, afc_on, 8192, true};
    rdof_sink_t sink = {.on_json = on_json, .on_call = on_call, .ctx = r};
    CHECK(rdof_sdr_rx_init(&rx, &cfg, ch, C, &sink, V_E2E_T0) == 0, "external init");
    rx.on_burst = on_burst, rx.burst_ctx = r;
    rx.on_correction = fx_correction, rx.correction_ctx = f;
    f->fs = 288e3, f->C = C;
    for (int c = 0; c < C; c++)
        f->off[c] = ch[c].freq_hz - rx.center_hz;
    static rdof_chzfx_cfg_t fc;
    CHECK(rdof_chzfx_design(&fc, 288e3, D, f->off, C, RDOF_CHZ_CUTOFF_HZ) == 0, "design");
    f->m = rdof_chzfx_new(&fc);
    rdof_iq16_t *in16 = malloc(sizeof *in16 * V_E2E_CHUNK), *out16 = malloc(sizeof *out16 * (V_E2E_CHUNK / D + 1) * C);
    uint64_t *words = malloc(sizeof *words * rdof_chzfx_frame_words(C) * (V_E2E_CHUNK / D + 1));
    f->cap = V_E2E_CHUNK / D + 1;
    f->y = malloc(sizeof *f->y * (size_t)f->cap * C);
    static rdof_chzhw_rx_t prx;
    rdof_chzhw_rx_init(&prx, C, fx_on_ch, NULL, f);
    for (int i = 0; i < n; i += V_E2E_CHUNK) {
        const int k = n - i < V_E2E_CHUNK ? n - i : V_E2E_CHUNK;
        for (int j = 0; j < k; j++)
            in16[j] = to12(x[i + j]);
        const uint32_t seq0 = f->m->seq;
        const int got = rdof_chzfx_process(f->m, in16, k, out16);
        size_t nw = 0;
        for (int j = 0; j < got; j++)
            nw += rdof_chzfx_frame(RDOF_CHZFX_FRAME_CH, seq0 + (uint32_t)j, out16 + (size_t)j * C, C, words + nw);
        f->ny = 0; /* the DMA's words in two pieces: a frame across buffers */
        rdof_chzhw_rx_feed(&prx, words, nw / 3);
        rdof_chzhw_rx_feed(&prx, words + nw / 3, nw - nw / 3);
        CHECK(f->ny == got, "%d instants out of the frames, %d in", f->ny, got);
        rdof_sdr_rx_channels_in(&rx, f->y, f->ny, RDOF_CHZFX_OUT_SCALE);
        rdof_sdr_rx_feed(&rx, 0, C);
        rdof_sdr_rx_bursts(&rx, 0, C);
        rdof_sdr_rx_finish(&rx);
    }
    CHECK(prx.lost == 0 && prx.junk == 0, "the parser: %llu lost, %llu junk", (unsigned long long)prx.lost,
          (unsigned long long)prx.junk);
    for (int c = 0; c < C; c++)
        afc[c] = rx.chz.correction_hz[c], rssi[c] = rx.rssi[c];
    rdof_sdr_rx_free(&rx);
    rdof_chzfx_free(f->m);
    free(in16), free(out16), free(words), free(f->y);
}

static void compare(const char *name, const run_t *a, const run_t *b)
{
    const int C = (int)V_E2E_CHANS_N;
    int crc_a = 0, crc_b = 0, same_payload = 0, voice = 0, voice_same = 0, matched = 0;
    double worst_db = 0;
    int worst_pos = 0;
    for (int c = 0; c < C; c++) {
        int ia = 0, ib = 0, na = 0, nb = 0;
        for (int i = 0; i < a->nrec; i++)
            na += a->rec[i].c == c && a->rec[i].dt != RDOF_DT_IDLE;
        for (int i = 0; i < b->nrec; i++)
            nb += b->rec[i].c == c && b->rec[i].dt != RDOF_DT_IDLE;
        CHECK(na == nb, "%s channel %d: %d bursts, the float path %d", name, c, nb, na);
        for (;;) { /* every burst, idle included, in order */
            while (ia < a->nrec && a->rec[ia].c != c)
                ia++;
            while (ib < b->nrec && b->rec[ib].c != c)
                ib++;
            if (ia == a->nrec || ib == b->nrec) {
                CHECK(ia == a->nrec && ib == b->nrec, "%s channel %d: a burst more on one side", name, c);
                break;
            }
            const rec_t *ra = &a->rec[ia++], *rb = &b->rec[ib++];
            matched++;
            const int dp = (int)(rb->pos - ra->pos);
            worst_pos = abs(dp) > abs(worst_pos) ? dp : worst_pos;
            CHECK(abs(dp) <= TOL_POS && ra->dt == rb->dt && ra->slot == rb->slot && ra->cc == rb->cc,
                  "%s channel %d: burst at %lld dt %d slot %d cc %d, the float path's at %lld dt %d slot %d cc %d",
                  name, c, (long long)rb->pos, rb->dt, rb->slot, rb->cc, (long long)ra->pos, ra->dt, ra->slot,
                  ra->cc);
            const double ddb = rb->power - ra->power;
            worst_db = fabs(ddb) > fabs(worst_db) ? ddb : worst_db;
            CHECK(fabs(ddb) < TOL_POWER_DB, "%s channel %d: burst power %.2f dBFS, the float path %.2f", name, c,
                  rb->power, ra->power);
            uint8_t pa[12], pb[12];
            const bool ga = crc_good(ra, pa), gb = crc_good(rb, pb);
            crc_a += ga, crc_b += gb;
            if (ga && gb && !memcmp(pa, pb, 12))
                same_payload++;
            if (ra->dt == RDOF_DT_VOICE || ra->dt == RDOF_DT_VOICE_SYNC) {
                voice++;
                voice_same += !memcmp(ra->burst, rb->burst, RDOF_BURST_BYTES);
            }
        }
    }
    CHECK(crc_a == crc_b && same_payload == crc_a && crc_a > 10,
          "%s: %d CRC-good payloads, the float path %d, %d the same", name, crc_b, crc_a, same_payload);
    CHECK(a->ncall == b->ncall, "%s: %d call events, the float path %d", name, b->ncall, a->ncall);
    for (int i = 0; i < a->ncall && i < b->ncall; i++)
        CHECK(!strcmp(a->call[i].ch, b->call[i].ch) && !strcmp(a->call[i].action, b->call[i].action) &&
                  a->call[i].src == b->call[i].src && a->call[i].dst == b->call[i].dst,
              "%s call event %d: %s %s %u > %u, the float path %s %s %u > %u", name, i, b->call[i].ch,
              b->call[i].action, b->call[i].src, b->call[i].dst, a->call[i].ch, a->call[i].action, a->call[i].src,
              a->call[i].dst);
    CHECK(a->json == b->json, "%s: %ld JSON messages, the float path %ld", name, b->json, a->json);
    printf("  %s: %d bursts matched, %d CRC-good payloads the same, %d of %d voice bursts bit for bit, %d call "
           "events and %ld JSON messages the same; worst position %+d samples, power %+.2f dB\n",
           name, matched, same_payload, voice_same, voice, b->ncall, b->json, worst_pos, worst_db);
}

static void test_e2e(void)
{
    char path[512];
    snprintf(path, sizeof path, "%s%s", RDOF_SDR_DATA, "e2e_288k.cu8");
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        printf("FAIL cannot open %s\n", path);
        failures++;
        return;
    }
    fseek(fp, 0, SEEK_END);
    const long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *raw = malloc((size_t)size);
    const int n = fread(raw, 1, (size_t)size, fp) == (size_t)size ? (int)(size / 2) : 0;
    fclose(fp);
    rdof_cf_t *x = malloc(sizeof *x * (size_t)n);
    rdof_iq_from_cu8(raw, (size_t)n, x);
    free(raw);
    rdof_sdr_channel_t ch[8];
    for (size_t i = 0; i < V_E2E_CHANS_N; i++)
        ch[i] = (rdof_sdr_channel_t){v_e2e_chans[i].id, v_e2e_chans[i].freq, v_e2e_chans[i].cc, v_e2e_chans[i].slot};

    static run_t ra, rb;
    double afc_a[8], afc_b[8];
    int rssi_a[8], rssi_b[8];
    run_float(x, n, ch, &ra, afc_a, rssi_a);
    static fx_t f;
    run_fx(x, n, ch, &rb, afc_b, rssi_b, &f, true);
    compare("e2e through the model", &ra, &rb);

    /* the AFC: the same corrections, and each one went to the model's NCO */
    double worst = 0;
    for (int c = 0; c < (int)V_E2E_CHANS_N; c++) {
        worst = fmax(worst, fabs(afc_b[c] - afc_a[c]));
        CHECK(fabs(afc_b[c] - afc_a[c]) < TOL_AFC_HZ, "%s: AFC %.1f Hz, the float path %.1f", ch[c].id, afc_b[c],
              afc_a[c]);
        CHECK(f.last_inc[c] == rdof_chzfx_inc(f.off[c] + afc_b[c], 288e3), "%s: the NCO's step isn't the correction's",
              ch[c].id);
        CHECK(abs(rssi_b[c] - rssi_a[c]) <= 1, "%s: RSSI %d, the float path %d", ch[c].id, rssi_b[c], rssi_a[c]);
    }
    CHECK(f.corrections > 10, "%ld corrections to the model", f.corrections);
    printf("  AFC: %ld corrections to the model's NCOs; the end's within %.2f Hz of the float path's (%.1f %.1f %.1f "
           "%.1f Hz)\n", f.corrections, worst, afc_b[0], afc_b[1], afc_b[2], afc_b[3]);

    /* without the AFC nothing goes to the model: its steps stay the plan's */
    static run_t rc;
    static fx_t g;
    run_fx(x, n, ch, &rc, afc_b, rssi_b, &g, false);
    CHECK(g.corrections == 0, "AFC off: %ld corrections", g.corrections);
    free(x);
}

int main(void)
{
    printf("channel samples from outside (the FPGA channelizer's model):\n");
    test_scale();
    test_noise();
    test_e2e();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
