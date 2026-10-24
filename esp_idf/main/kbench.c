// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Kernel shootout for the self-test: cycles per call of the DSP kernels the
 * receiver uses, against the faster ones the S3 offers (esp-dsp's radix-4 and
 * 16-bit SIMD FFTs and FIR, a divide-free discriminator), and the transmit
 * modulator's. Numbers only; the receiver's choices are made elsewhere.
 */
#include <math.h>
#include <string.h>

#include "app.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"

#if CONFIG_RDOF_SDR_SELFTEST && CONFIG_RDOF_SDR_ESP_DSP

#include "dsps_fft2r.h"
#include "dsps_fft4r.h"
#include "dsps_fir.h"
#include "rdof/sdr/fft.h"
#include "rdof/sdr/fm.h"
#include "rdof/sdr/mod.h"
#include "rdof/sdr/resample.h"

static const char *TAG = "kbench";

#define REPS 50

static void *ialloc(size_t n) { return heap_caps_aligned_calloc(16, 1, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }

typedef void (*kern_t)(void *ctx);

static uint32_t cycles(kern_t k, void *ctx)
{
    uint32_t best = UINT32_MAX;
    for (int r = 0; r < REPS; r++) {
        uint32_t t = esp_cpu_get_cycle_count();
        k(ctx);
        t = esp_cpu_get_cycle_count() - t;
        if (t < best)
            best = t;
    }
    return best;
}

typedef struct {
    int n;
    float *f;
    int16_t *s;
    rdof_fft_t *plan;
    rdof_cf_t *in, *out;
} fft_ctx_t;

static void k_builtin(void *c)
{
    fft_ctx_t *x = c;
    rdof_fft_run(x->plan, x->in, x->out);
}
static void k_fc32_2r(void *c)
{
    fft_ctx_t *x = c;
    dsps_fft2r_fc32(x->f, x->n);
    dsps_bit_rev_fc32(x->f, x->n);
}
static void k_fc32_4r(void *c)
{
    fft_ctx_t *x = c;
    dsps_fft4r_fc32(x->f, x->n);
    dsps_bit_rev4r_fc32(x->f, x->n);
}
static void k_sc16_2r(void *c)
{
    fft_ctx_t *x = c;
    dsps_fft2r_sc16(x->s, x->n);
    dsps_bit_rev_sc16_ansi(x->s, x->n);
}
static void k_sc16_nobr(void *c)
{
    fft_ctx_t *x = c;
    dsps_fft2r_sc16(x->s, x->n);
}

static void fft_row(int n)
{
    fft_ctx_t x = {.n = n};
    x.f = ialloc(sizeof(float) * 2 * n);
    x.s = ialloc(sizeof(int16_t) * 2 * n);
    x.in = ialloc(sizeof(rdof_cf_t) * n);
    x.out = ialloc(sizeof(rdof_cf_t) * n);
    dsp_esp_fft_enable(false);
    x.plan = rdof_fft_new(n, false);
    uint32_t b = x.plan ? cycles(k_builtin, &x) : 0;
    rdof_fft_free(x.plan);
    dsp_esp_fft_enable(true);
    uint32_t c2 = 0, c4 = 0, s16 = 0, s16n = 0;
    if ((n & (n - 1)) == 0) {
        c2 = cycles(k_fc32_2r, &x);
        c4 = n <= 64 ? cycles(k_fc32_4r, &x) : 0;
        s16 = cycles(k_sc16_2r, &x);
        s16n = cycles(k_sc16_nobr, &x);
    }
    ESP_LOGI(TAG, "FFT %4d: built-in %6lu, fc32 radix-2 %6lu, fc32 radix-4 %6lu, sc16 SIMD %6lu (%lu without bit "
             "reversal)",
             n, (unsigned long)b, (unsigned long)c2, (unsigned long)c4, (unsigned long)s16, (unsigned long)s16n);
    heap_caps_free(x.f);
    heap_caps_free(x.s);
    heap_caps_free(x.in);
    heap_caps_free(x.out);
}

/* ── FIR ────────────────────────────────────────────────────────────────── */

enum { FN = 1024, FT = 84 };

typedef struct {
    fir_f32_t ff;
    fir_s16_t fs;
    float *in, *out, *h;
    int16_t *sin, *sout, *sh;
} fir_ctx_t;

static void k_fir_f32(void *c)
{
    fir_ctx_t *x = c;
    dsps_fir_f32(&x->ff, x->in, x->out, FN);
}
static void k_fir_s16(void *c)
{
    fir_ctx_t *x = c;
    dsps_fird_s16(&x->fs, x->sin, x->sout, FN);
}
static void k_fir_sym(void *c)
{
    fir_ctx_t *x = c;
    enum { T = RDOF_FM_TAPS - 1 };
    for (int i = 0; i < FN - T; i++) {
        const float *p = x->in + i;
        float acc = x->h[T / 2] * p[T / 2];
        for (int j = 0; j < T / 2; j++)
            acc += x->h[j] * (p[T - j] + p[j]);
        x->out[i] = acc;
    }
}

static void fir_row(void)
{
    fir_ctx_t x;
    memset(&x, 0, sizeof x);
    x.in = ialloc(sizeof(float) * FN);
    x.out = ialloc(sizeof(float) * FN);
    x.h = ialloc(sizeof(float) * FT);
    x.sin = ialloc(sizeof(int16_t) * FN);
    x.sout = ialloc(sizeof(int16_t) * FN);
    x.sh = ialloc(sizeof(int16_t) * 88);
    float *d = ialloc(sizeof(float) * (FT + 4));
    for (int i = 0; i < FN; i++) {
        x.in[i] = 1000.0f * sinf(i * 0.1f);
        x.sin[i] = (int16_t)x.in[i];
    }
    for (int i = 0; i < FT; i++) {
        x.h[i] = 0.01f;
        x.sh[i] = 327;
    }
    dsps_fir_init_f32(&x.ff, x.h, d, FT);
    dsps_fird_init_s16(&x.fs, x.sh, NULL, 88, 1, 0, 0);
    uint32_t f32 = cycles(k_fir_f32, &x), s16 = cycles(k_fir_s16, &x), sym = cycles(k_fir_sym, &x);
    ESP_LOGI(TAG, "FIR %d taps, cycles per sample: built-in symmetric %.1f, esp-dsp f32 %.1f, esp-dsp s16 SIMD %.1f",
             RDOF_FM_TAPS, (double)sym / (FN - RDOF_FM_TAPS + 1), (double)f32 / FN, (double)s16 / FN);
    dsps_fird_s16_aexx_free(&x.fs);
    heap_caps_free(d);
    heap_caps_free(x.in);
    heap_caps_free(x.out);
    heap_caps_free(x.h);
    heap_caps_free(x.sin);
    heap_caps_free(x.sout);
    heap_caps_free(x.sh);
}

/* ── our int16 SIMD FIR (fir_s3.S) and the whole matched filter stage ───── */

void rdof_s3_fir8(const int16_t *x, const int16_t *coefs, int32_t *out, const int32_t *round, int shift);

typedef struct {
    int16_t *x, *c;
    int32_t *r, acc[8];
    rdof_cf_t *iq;
    float *f, *p;
} s3fir_ctx_t;

static void k_s3fir(void *c)
{
    s3fir_ctx_t *x = c;
    for (int g = 0; g < FN; g += 8)
        rdof_s3_fir8(x->x + g, x->c, x->acc, x->r, 9);
}
static void k_fm(void *c)
{
    s3fir_ctx_t *x = c;
    rdof_fm_t fm;
    rdof_fm_init(&fm, 48000.0);
    rdof_fm_process(&fm, x->iq, FN, x->f, x->p);
    rdof_fm_free(&fm);
}

static void s3fir_row(void)
{
    s3fir_ctx_t x = {.x = ialloc(sizeof(int16_t) * (FN + 128)), .c = ialloc(sizeof(int16_t) * (8 * 96 + 16)),
                     .r = ialloc(sizeof(int32_t) * 4)};
    x.iq = ialloc(sizeof(rdof_cf_t) * FN);
    x.f = ialloc(sizeof(float) * FN);
    x.p = ialloc(sizeof(float) * FN);
    for (int i = 0; i < FN; i++) {
        x.iq[i].re = cosf(i * 0.3f);
        x.iq[i].im = sinf(i * 0.3f);
    }
    uint32_t a = cycles(k_s3fir, &x), b = cycles(k_fm, &x);
    ESP_LOGI(TAG, "int16 SIMD FIR alone %.1f cycles per sample; rdof_fm_process (discriminator, FIR, power) %.1f",
             (double)a / FN, (double)b / FN);
    heap_caps_free(x.x);
    heap_caps_free(x.c);
    heap_caps_free(x.r);
    heap_caps_free(x.iq);
    heap_caps_free(x.f);
    heap_caps_free(x.p);
}

/* ── discriminator ──────────────────────────────────────────────────────── */

typedef struct {
    rdof_cf_t *in;
    float *out;
} disc_ctx_t;

static void k_disc(void *c)
{
    disc_ctx_t *x = c;
    rdof_cf_t prev = {1, 0};
    rdof_fm_discriminate(&prev, 7639.4f, x->in, FN, x->out);
}

static void disc_row(void)
{
    disc_ctx_t x = {ialloc(sizeof(rdof_cf_t) * FN), ialloc(sizeof(float) * FN)};
    for (int i = 0; i < FN; i++) {
        x.in[i].re = cosf(i * 0.3f + sinf(i * 0.01f));
        x.in[i].im = sinf(i * 0.3f + sinf(i * 0.01f));
    }
    uint32_t c = cycles(k_disc, &x);
    ESP_LOGI(TAG, "discriminator: %.1f cycles per sample", (double)c / FN);
    heap_caps_free(x.in);
    heap_caps_free(x.out);
}

/* The DMR modulator (mod.c): a repeater's continuous downlink at 288 kS/s, I/Q out, 5 ms a call. */
#define MN 1440

typedef struct {
    rdof_mod_t m;
    int64_t next, slot;
    int8_t sym[144];
    rdof_cf_t *out;
} mod_ctx_t;

static void k_mod(void *c)
{
    mod_ctx_t *x = c;
    while (x->next < x->m.pos + 3 * x->slot) {
        rdof_mod_queue(&x->m, x->next, x->sym, -12, 144, x->next == 2000 ? RDOF_MOD_RAMP_UP : 0);
        x->next += x->slot;
    }
    rdof_mod_iq(&x->m, x->out, MN);
}

static void mod_row(void)
{
    mod_ctx_t *x = ialloc(sizeof *x);
    if (!x)
        return;
    x->out = ialloc(sizeof(rdof_cf_t) * MN);
    if (!x->out || rdof_mod_init(&x->m, 288000, 62500.0, 4.0, 1.0f, 4, 0) != 0) {
        ESP_LOGE(TAG, "modulator init failed");
    } else {
        uint32_t r = 1;
        for (int i = 0; i < 144; i++) {
            r = r * 1103515245u + 12345u;
            x->sym[i] = (int8_t)(((r >> 16) & 2 ? 3 : 1) * ((r >> 17) & 1 ? -1 : 1));
        }
        x->slot = rdof_mod_slot_samples(&x->m);
        x->next = 2000;
        uint32_t c = cycles(k_mod, x);
        ESP_LOGI(TAG, "modulator: %.1f cycles per I/Q sample, %.1f Mcycles/s at 288 kS/s", (double)c / MN,
                 (double)c / MN * 0.288);
    }
    rdof_mod_free(&x->m);
    heap_caps_free(x->out);
    heap_caps_free(x);
}

/* The resampler a 32 MHz front end needs (the LR2021's 666 667 S/s to 576 000, 108 / 125), a block of 512 inputs,
 * at 10 and 32 taps a phase. */
#define RN 512

typedef struct {
    rdof_resamp_t r;
    rdof_cf_t in[RN], out[RN + 4];
} rs_ctx_t;

static void k_resamp(void *c)
{
    rs_ctx_t *x = c;
    rdof_resamp_run(&x->r, x->in, RN, x->out);
}

static void resamp_row(void)
{
    rs_ctx_t *x = ialloc(sizeof *x);
    if (!x)
        return;
    for (int i = 0; i < RN; i++) {
        x->in[i].re = sinf(0.1f * i);
        x->in[i].im = cosf(0.1f * i);
    }
    static const int taps[2] = {10, 32};
    for (int t = 0; t < 2; t++) {
        if (rdof_resamp_init(&x->r, 2000000, 1728000, taps[t], 0.4, RN)) {
            ESP_LOGE(TAG, "resampler init failed");
            break;
        }
        uint32_t c = cycles(k_resamp, x);
        const double outs = RN * 108.0 / 125;
        ESP_LOGI(TAG, "resampler 666 667 to 576 000, %d taps: %.1f cycles an output, %.1f Mcycles/s (taps %s)",
                 taps[t], c / outs, c / outs * 0.576, esp_ptr_external_ram(x->r.h) ? "PSRAM" : "internal");
        rdof_resamp_free(&x->r);
    }
    heap_caps_free(x);
}

void kbench_run(void)
{
    /* radix-4 runs on the table dsp_esp_install set up (to 64 points) */
    if (dsps_fft2r_init_sc16(NULL, 1024) != ESP_OK) {
        ESP_LOGE(TAG, "esp-dsp init failed");
        return;
    }
    fft_row(64);
    fft_row(128);
    fft_row(384);
    fft_row(512);
    fir_row();
    s3fir_row();
    disc_row();
    mod_row();
    resamp_row();
    dsps_fft2r_deinit_sc16();
}

#else

void kbench_run(void) {}

#endif
