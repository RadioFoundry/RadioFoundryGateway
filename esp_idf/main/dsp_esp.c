// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * esp-dsp behind sdr's hooks: dsps_fft2r_fc32 (S3 assembly) for the
 * power-of-two FFTs (the 64-point IFFT per channel; the 384-point one at
 * 288 kS/s stays on the built-in mixed radix), and dsps_fir_f32 for the
 * 81-tap matched filter. Each is checked against the built-in code on a test
 * signal at boot and left out if it doesn't agree, so a library change can't
 * quietly change the output.
 */
#include <math.h>
#include <string.h>

#include "app.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

bool g_esp_fft, g_esp_fir;

#if CONFIG_RDOF_SDR_ESP_DSP

#include "dsps_fft2r.h"
#include "dsps_fft4r.h"
#include "dsps_fir.h"
#include "rdof/sdr/fft.h"
#include "rdof/sdr/fm.h"
#include "rdof/sdr/resample.h"

static const char *TAG = "esp-dsp";

/* ── FFT ────────────────────────────────────────────────────────────────── */

#define FFT2_MAX 512 /* the spectrum's size: esp-dsp's radix-2 table is this many floats */
#define FFT4_MAX 256 /* the channelizer's inverse FFT (64), and the P x 256 forward ones */

typedef struct {
    int n;
    bool inverse;
    float *work;     /* 16-byte aligned, in place */
    uint16_t *perm;  /* radix-4: out[k] = work[perm[k]] (the digit reversal, and the sign for an inverse) */
    rdof_cf_t *tw;     /* 6 x 64 (the 384-point forward FFT at 288 kS/s): e^{-j 2 pi n1 k2 / 384}, n1 = 1..5 */
    int P, Q;        /* a P x Q forward FFT (P x 4^k: 960 kS/s is 5 x 256), 0 otherwise */
    rdof_cf_t *wp;     /* e^{-j 2 pi m / P}, m < P */
} fft_plan_t;

static bool fft_pq_ok = true; /* the P x Q plans passed their check at boot */

static bool fft_conj_trick; /* the library's forward sign is the other way: flip it */

static bool power_of_4(int n) { return n >= 16 && (n & (n - 1)) == 0 && (__builtin_ctz(n) % 2) == 0; }

/* Where radix-4 leaves each bin: transform an impulse at 1 (bin k is e^{-j 2 pi k / n}) and read k back
 * from each slot's phase. Works whichever sign and output order the library has; false if it isn't a
 * permutation. */
static bool fft4_perm(int n, bool inverse, float *w, uint16_t *perm)
{
    memset(w, 0, sizeof(float) * 2 * n);
    w[2] = 1.0f;
    if (dsps_fft4r_fc32(w, n) != ESP_OK)
        return false;
    for (int k = 0; k < n; k++)
        perm[k] = UINT16_MAX;
    for (int q = 0; q < n; q++) {
        int k = (int)lround(-atan2(w[2 * q + 1], w[2 * q]) * n / (2 * M_PI));
        k = ((k % n) + n) % n;
        if (inverse) /* an inverse FFT's bin k is the forward one's -k */
            k = (n - k) % n;
        if (perm[k] != UINT16_MAX)
            return false;
        perm[k] = (uint16_t)q;
    }
    return true;
}

/* The 384-point forward FFT (288 kS/s) as 6 x 64, decimation in time: x[6 n2 + n1] makes six 64-point
 * FFTs F_n1 (radix-4), and X[k2 + 64 k1] = sum_n1 (W384^(n1 k2) F_n1[k2]) W6^(n1 k1): a 6-point DFT
 * per k2, done as two 3-point DFTs (even and odd n1). About 21k cycles against the built-in's 50k. */
#define SIX_M 64

static void *fft6_create(void)
{
    const int n = 6 * SIX_M;
    fft_plan_t *p = sdr_hot_aligned(4, sizeof *p, true);
    float *w = sdr_hot_aligned(16, sizeof(float) * 2 * n, false);
    uint16_t *perm = sdr_hot_aligned(4, sizeof(uint16_t) * SIX_M, false);
    rdof_cf_t *tw = sdr_hot_aligned(4, sizeof(rdof_cf_t) * 5 * SIX_M, false);
    if (!p || !w || !perm || !tw || !fft4_perm(SIX_M, false, w, perm)) {
        sdr_hot_free(p);
        sdr_hot_free(w);
        sdr_hot_free(perm);
        sdr_hot_free(tw);
        return NULL;
    }
    for (int n1 = 1; n1 < 6; n1++)
        for (int k2 = 0; k2 < SIX_M; k2++) {
            const double ph = -2 * M_PI * n1 * k2 / n;
            tw[(n1 - 1) * SIX_M + k2].re = (float)cos(ph);
            tw[(n1 - 1) * SIX_M + k2].im = (float)sin(ph);
        }
    p->n = n;
    p->work = w;
    p->perm = perm;
    p->tw = tw;
    return p;
}

static inline rdof_cf_t cmul_(rdof_cf_t a, rdof_cf_t b)
{
    rdof_cf_t r = {a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re};
    return r;
}

/* 3-point DFT: y0 = a + b + c, y1/y2 = a - (b + c) / 2 -/+ j (sqrt(3) / 2) (b - c) */
static inline void dft3(rdof_cf_t a, rdof_cf_t b, rdof_cf_t c, rdof_cf_t *y0, rdof_cf_t *y1, rdof_cf_t *y2)
{
    const float h = 0.866025404f;
    const rdof_cf_t t = {b.re + c.re, b.im + c.im}, d = {b.re - c.re, b.im - c.im};
    const rdof_cf_t m = {a.re - 0.5f * t.re, a.im - 0.5f * t.im};
    y0->re = a.re + t.re;
    y0->im = a.im + t.im;
    y1->re = m.re + h * d.im;
    y1->im = m.im - h * d.re;
    y2->re = m.re - h * d.im;
    y2->im = m.im + h * d.re;
}

static void fft6_run(fft_plan_t *p, const rdof_cf_t *in, rdof_cf_t *out)
{
    rdof_cf_t *sub = (rdof_cf_t *)p->work; /* six 64-point blocks */
    for (int n1 = 0; n1 < 6; n1++) {
        rdof_cf_t *f = sub + n1 * SIX_M;
        for (int n2 = 0; n2 < SIX_M; n2++)
            f[n2] = in[6 * n2 + n1];
        dsps_fft4r_fc32((float *)f, SIX_M);
    }
    const rdof_cf_t *tw = p->tw;
    for (int k2 = 0; k2 < SIX_M; k2++) {
        const int q = p->perm[k2];
        const rdof_cf_t z0 = sub[q], z1 = cmul_(sub[SIX_M + q], tw[k2]), z2 = cmul_(sub[2 * SIX_M + q], tw[SIX_M + k2]),
                      z3 = cmul_(sub[3 * SIX_M + q], tw[2 * SIX_M + k2]),
                      z4 = cmul_(sub[4 * SIX_M + q], tw[3 * SIX_M + k2]),
                      z5 = cmul_(sub[5 * SIX_M + q], tw[4 * SIX_M + k2]);
        rdof_cf_t e0, e1, e2, o0, o1, o2;
        dft3(z0, z2, z4, &e0, &e1, &e2);
        dft3(z1, z3, z5, &o0, &o1, &o2);
        /* Y[k] = E[k] + W6^k O[k], Y[k + 3] = E[k] - W6^k O[k]; W6 = 1/2 - j sqrt(3)/2 */
        const float h = 0.866025404f;
        const rdof_cf_t w1 = {0.5f * o1.re + h * o1.im, 0.5f * o1.im - h * o1.re};
        const rdof_cf_t w2 = {-0.5f * o2.re + h * o2.im, -0.5f * o2.im - h * o2.re};
        out[k2].re = e0.re + o0.re;
        out[k2].im = e0.im + o0.im;
        out[k2 + 3 * SIX_M].re = e0.re - o0.re;
        out[k2 + 3 * SIX_M].im = e0.im - o0.im;
        out[k2 + SIX_M].re = e1.re + w1.re;
        out[k2 + SIX_M].im = e1.im + w1.im;
        out[k2 + 4 * SIX_M].re = e1.re - w1.re;
        out[k2 + 4 * SIX_M].im = e1.im - w1.im;
        out[k2 + 2 * SIX_M].re = e2.re + w2.re;
        out[k2 + 2 * SIX_M].im = e2.im + w2.im;
        out[k2 + 5 * SIX_M].re = e2.re - w2.re;
        out[k2 + 5 * SIX_M].im = e2.im - w2.im;
    }
}

/* A forward FFT of n = P x Q points, P in {2, 3, 5, 6} and Q a power of 4 up to FFT4_MAX, the way fft6 does
 * 6 x 64: P radix-4 FFTs of Q points (x[P n2 + n1]), then per k2 the twiddles W_n^(n1 k2) and a P-point DFT.
 * The channelizer's forward FFT at 576, 960 and 1152 kS/s (768, 1280, 1536 points), which the built-in mixed
 * radix does at about 250 cycles a point. NULL for a size it can't do. */
static void *fftpq_create(int n)
{
    static const int ps[] = {5, 3, 6, 2};
    for (size_t i = 0; i < sizeof ps / sizeof ps[0]; i++) {
        const int P = ps[i], Q = n / P;
        if (n % P || !power_of_4(Q) || Q > FFT4_MAX)
            continue;
        fft_plan_t *p = sdr_hot_aligned(4, sizeof *p, true);
        float *w = sdr_hot_aligned(16, sizeof(float) * 2 * n, false);
        uint16_t *perm = sdr_hot_aligned(4, sizeof(uint16_t) * Q, false);
        rdof_cf_t *tw = sdr_hot_aligned(4, sizeof(rdof_cf_t) * (P - 1) * Q, false);
        rdof_cf_t *wp = sdr_hot_aligned(4, sizeof(rdof_cf_t) * P, false);
        if (!p || !w || !perm || !tw || !wp || !fft4_perm(Q, false, w, perm)) {
            sdr_hot_free(p);
            sdr_hot_free(w);
            sdr_hot_free(perm);
            sdr_hot_free(tw);
            sdr_hot_free(wp);
            return NULL;
        }
        for (int n1 = 1; n1 < P; n1++)
            for (int k2 = 0; k2 < Q; k2++) {
                const double ph = -2 * M_PI * n1 * k2 / n;
                tw[(n1 - 1) * Q + k2] = (rdof_cf_t){(float)cos(ph), (float)sin(ph)};
            }
        for (int m = 0; m < P; m++)
            wp[m] = (rdof_cf_t){(float)cos(-2 * M_PI * m / P), (float)sin(-2 * M_PI * m / P)};
        p->n = n;
        p->P = P;
        p->Q = Q;
        p->work = w;
        p->perm = perm;
        p->tw = tw;
        p->wp = wp;
        return p;
    }
    return NULL;
}

static void fftpq_run(fft_plan_t *p, const rdof_cf_t *in, rdof_cf_t *out)
{
    const int P = p->P, Q = p->Q;
    rdof_cf_t *sub = (rdof_cf_t *)p->work;
    for (int n1 = 0; n1 < P; n1++) {
        rdof_cf_t *f = sub + n1 * Q;
        for (int n2 = 0; n2 < Q; n2++)
            f[n2] = in[P * n2 + n1];
        dsps_fft4r_fc32((float *)f, Q);
    }
    const rdof_cf_t *tw = p->tw;
    if (P == 5) { /* the 5-point DFT with its constants: no table, no modulo */
        const float c1 = 0.309016994f, c2 = -0.809016994f, s1 = 0.951056516f, s2 = 0.587785252f;
        for (int k2 = 0; k2 < Q; k2++) {
            const int q = p->perm[k2];
            const rdof_cf_t z0 = sub[q], z1 = cmul_(sub[Q + q], tw[k2]), z2 = cmul_(sub[2 * Q + q], tw[Q + k2]),
                          z3 = cmul_(sub[3 * Q + q], tw[2 * Q + k2]), z4 = cmul_(sub[4 * Q + q], tw[3 * Q + k2]);
            const float a1r = z1.re + z4.re, a1i = z1.im + z4.im, a2r = z2.re + z3.re, a2i = z2.im + z3.im;
            const float b1r = z1.re - z4.re, b1i = z1.im - z4.im, b2r = z2.re - z3.re, b2i = z2.im - z3.im;
            /* y1, y4 = z0 + c1 a1 + c2 a2 -/+ j (s1 b1 + s2 b2); y2, y3 = z0 + c2 a1 + c1 a2 -/+ j (s2 b1 - s1 b2) */
            const float p1r = z0.re + c1 * a1r + c2 * a2r, p1i = z0.im + c1 * a1i + c2 * a2i;
            const float p2r = z0.re + c2 * a1r + c1 * a2r, p2i = z0.im + c2 * a1i + c1 * a2i;
            const float q1r = s1 * b1r + s2 * b2r, q1i = s1 * b1i + s2 * b2i;
            const float q2r = s2 * b1r - s1 * b2r, q2i = s2 * b1i - s1 * b2i;
            out[k2] = (rdof_cf_t){z0.re + a1r + a2r, z0.im + a1i + a2i};
            out[k2 + Q] = (rdof_cf_t){p1r + q1i, p1i - q1r}; /* -j (x + j y) = y - j x */
            out[k2 + 4 * Q] = (rdof_cf_t){p1r - q1i, p1i + q1r};
            out[k2 + 2 * Q] = (rdof_cf_t){p2r + q2i, p2i - q2r};
            out[k2 + 3 * Q] = (rdof_cf_t){p2r - q2i, p2i + q2r};
        }
        return;
    }
    const rdof_cf_t *wp = p->wp;
    for (int k2 = 0; k2 < Q; k2++) {
        const int q = p->perm[k2];
        rdof_cf_t z[6];
        z[0] = sub[q];
        for (int n1 = 1; n1 < P; n1++)
            z[n1] = cmul_(sub[n1 * Q + q], tw[(n1 - 1) * Q + k2]);
        for (int k1 = 0; k1 < P; k1++) {
            rdof_cf_t acc = z[0];
            for (int n1 = 1, m = k1; n1 < P; n1++) {
                const rdof_cf_t t = cmul_(z[n1], wp[m]);
                acc.re += t.re;
                acc.im += t.im;
                m += k1; /* n1 k1 mod P, without a divide */
                if (m >= P)
                    m -= P;
            }
            out[k2 + Q * k1] = acc;
        }
    }
}

static void *fft_create(int n, bool inverse)
{
    if (!inverse && n != 6 * SIX_M && fft_pq_ok && !(power_of_4(n) && n <= FFT4_MAX) &&
        !(n >= 16 && n <= FFT2_MAX && !(n & (n - 1)))) {
        void *p = fftpq_create(n);
        if (p)
            return p;
    }
    if (n == 6 * SIX_M && !inverse)
        return fft6_create();
    const bool r4 = power_of_4(n) && n <= FFT4_MAX;
    if (!r4 && (n < 16 || n > FFT2_MAX || (n & (n - 1))))
        return NULL;
    fft_plan_t *p = sdr_hot_aligned(4, sizeof *p, true);
    float *w = sdr_hot_aligned(16, sizeof(float) * 2 * n, false);
    uint16_t *perm = r4 ? sdr_hot_aligned(4, sizeof(uint16_t) * n, false) : NULL;
    if (!p || !w || (r4 && (!perm || !fft4_perm(n, inverse, w, perm)))) {
        sdr_hot_free(p);
        sdr_hot_free(w);
        sdr_hot_free(perm);
        return NULL;
    }
    p->n = n;
    p->inverse = inverse;
    p->work = w;
    p->perm = perm;
    return p;
}

static void fft_run(void *plan, const rdof_cf_t *in, rdof_cf_t *out)
{
    fft_plan_t *p = plan;
    float *w = p->work;
    if (p->P) {
        fftpq_run(p, in, out);
        return;
    }
    if (p->tw) {
        fft6_run(p, in, out);
        return;
    }
    memcpy(w, in, sizeof(float) * 2 * p->n);
    if (p->perm) {
        dsps_fft4r_fc32(w, p->n);
        const rdof_cf_t *wc = (const rdof_cf_t *)w;
        for (int k = 0; k < p->n; k++)
            out[k] = wc[p->perm[k]];
        return;
    }
    const bool conj = p->inverse != fft_conj_trick;
    if (conj)
        for (int i = 1; i < 2 * p->n; i += 2)
            w[i] = -w[i];
    dsps_fft2r_fc32(w, p->n);
    dsps_bit_rev_fc32(w, p->n);
    if (conj)
        for (int i = 1; i < 2 * p->n; i += 2)
            w[i] = -w[i];
    memcpy(out, w, sizeof(float) * 2 * p->n);
}

static void fft_destroy(void *plan)
{
    fft_plan_t *p = plan;
    sdr_hot_free(p->work);
    sdr_hot_free(p->perm);
    sdr_hot_free(p->tw);
    sdr_hot_free(p->wp);
    sdr_hot_free(p);
}

static const rdof_fft_backend_t fft_backend = {fft_create, fft_run, fft_destroy};

static double fft_check(int n, bool inverse)
{
    rdof_cf_t *x = malloc(sizeof(rdof_cf_t) * n), *a = malloc(sizeof(rdof_cf_t) * n), *b = malloc(sizeof(rdof_cf_t) * n);
    for (int i = 0; i < n; i++) {
        x[i].re = (float)(((i * 7919) % 255 - 127) / 128.0);
        x[i].im = (float)(((i * 104729) % 251 - 125) / 128.0);
    }
    rdof_fft_set_backend(NULL);
    rdof_fft_t *ref = rdof_fft_new(n, inverse);
    rdof_fft_run(ref, x, a);
    rdof_fft_free(ref);
    void *p = fft_create(n, inverse);
    fft_run(p, x, b);
    fft_destroy(p);
    double err = 0, mx = 0;
    for (int i = 0; i < n; i++) {
        err = fmax(err, hypot(a[i].re - b[i].re, a[i].im - b[i].im));
        mx = fmax(mx, hypot(a[i].re, a[i].im));
    }
    free(x);
    free(a);
    free(b);
    return err / mx;
}

/* ── FIR ────────────────────────────────────────────────────────────────── */

#define FIR_TAPS ((RDOF_FM_TAPS + 3) / 4 * 4) /* esp32s3: a multiple of 4, 16-byte aligned */

typedef struct {
    fir_f32_t fir;
    float *coeffs, *delay;
} fir_state_t;

static int fir_pad_front; /* where the zero taps go (which way round the library takes the taps) */

static void *fir_create(const float *h, int taps)
{
    if (taps > FIR_TAPS)
        return NULL;
    fir_state_t *s = sdr_hot_aligned(4, sizeof *s, true);
    float *c = sdr_hot_aligned(16, (FIR_TAPS) * sizeof(float), true);
    float *d = sdr_hot_aligned(16, (FIR_TAPS + 4) * sizeof(float), true);
    if (!s || !c || !d) {
        free(s);
        sdr_hot_free(c);
        sdr_hot_free(d);
        return NULL;
    }
    memcpy(c + (fir_pad_front ? FIR_TAPS - taps : 0), h, sizeof(float) * taps);
    s->coeffs = c;
    s->delay = d;
    if (dsps_fir_init_f32(&s->fir, c, d, FIR_TAPS) != ESP_OK) {
        sdr_hot_free(c);
        sdr_hot_free(d);
        free(s);
        return NULL;
    }
    return s;
}

static void fir_run(void *state, const float *in, float *out, int n)
{
    fir_state_t *s = state;
    dsps_fir_f32(&s->fir, in, out, n);
}

static void fir_destroy(void *state)
{
    fir_state_t *s = state;
    sdr_hot_free(s->coeffs);
    sdr_hot_free(s->delay);
    free(s);
}

static const rdof_fir_backend_t fir_backend = {fir_create, fir_run, fir_destroy};

/* The same filter in int16 on the SIMD unit (fir_s3.S): input in whole Hz (the discriminator gives at
 * most +-24 kHz), taps x 2^17. Output i is taps . x[i - 80 .. i]; x is kept with H samples of history so
 * that output i's window starts at x[8 + i], and output r of each group of 8 uses tap set r, the taps
 * moved r along a 96-tap row, so every load is aligned. About 1 Hz of rounding at worst, far below the
 * 4FSK levels' spacing. */
enum { S16_H = 88, S16_SET = 96, S16_COEF_SHIFT = 17, S16_OUT_SHIFT = 9 };

void rdof_s3_fir8(const int16_t *x, const int16_t *coefs, int32_t *out, const int32_t *round, int shift);

typedef struct {
    int16_t *coefs; /* 8 x 96 + 8 */
    int16_t *x;     /* S16_H history + RDOF_FM_BLOCK + 24 */
    int32_t *round; /* {1 << (shift - 1), 0} */
} fir16_t;

static void *fir16_create(const float *h, int taps)
{
    if (taps > S16_H - 7 || taps > S16_SET - 7)
        return NULL;
    fir16_t *s = sdr_hot_aligned(4, sizeof *s, true);
    int16_t *c = sdr_hot_aligned(16, (8 * S16_SET + 8) * sizeof(int16_t), true);
    int16_t *x = sdr_hot_aligned(16, (S16_H + RDOF_FM_BLOCK + 24) * sizeof(int16_t), true);
    int32_t *r = sdr_hot_aligned(16, (4) * sizeof(int32_t), true);
    if (!s || !c || !x || !r) {
        sdr_hot_free(s);
        sdr_hot_free(c);
        sdr_hot_free(x);
        sdr_hot_free(r);
        return NULL;
    }
    /* output i = sum_j h[j] x[i - j] = sum_k h[taps - 1 - k] x[i - (taps - 1) + k]; its window starts at
     * x[S16_H - (taps - 1) + i], which is 8 + i (8g + r) when taps is 81 */
    const int lead = S16_H - (taps - 1) - 8; /* 0 for 81 taps; more taps start the window earlier */
    for (int r8 = 0; r8 < 8; r8++)
        for (int k = 0; k < taps; k++) {
            const int at = r8 + lead + k;
            if (at < 0 || at >= S16_SET) {
                sdr_hot_free(s);
                sdr_hot_free(c);
                sdr_hot_free(x);
                sdr_hot_free(r);
                return NULL;
            }
            c[r8 * S16_SET + at] = (int16_t)lrintf(h[taps - 1 - k] * (float)(1 << S16_COEF_SHIFT));
        }
    r[0] = 1 << (S16_OUT_SHIFT - 1);
    s->coefs = c;
    s->x = x;
    s->round = r;
    return s;
}

static void fir16_run(void *state, const float *in, float *out, int n)
{
    fir16_t *s = state;
    int16_t *x = s->x;
    const float scale = 1.0f / (float)(1 << (S16_COEF_SHIFT - S16_OUT_SHIFT));
    for (int done = 0; done < n;) {
        const int k = n - done < RDOF_FM_BLOCK ? n - done : RDOF_FM_BLOCK;
        /* to whole Hz: adding 1.5 x 2^23 leaves the rounded value in the low bits. The discriminator gives
         * at most +-fs / 2 (24 kHz), well inside int16. */
        for (int i = 0; i < k; i++) {
            const float v = in[done + i] + 12582912.0f;
            uint32_t u;
            __asm__("rfr %0, %1" : "=a"(u) : "f"(v));
            x[S16_H + i] = (int16_t)u;
        }
        int32_t acc[8];
        for (int g = 0; g < k; g += 8) {
            rdof_s3_fir8(x + 8 + g, s->coefs, acc, s->round, S16_OUT_SHIFT);
            const int m = k - g < 8 ? k - g : 8;
            for (int r8 = 0; r8 < m; r8++)
                out[done + g + r8] = (float)acc[r8] * scale;
        }
        memmove(x, x + k, sizeof(int16_t) * S16_H);
        done += k;
    }
}

static void fir16_destroy(void *state)
{
    fir16_t *s = state;
    sdr_hot_free(s->coefs);
    sdr_hot_free(s->x);
    sdr_hot_free(s->round);
    sdr_hot_free(s);
}

static const rdof_fir_backend_t fir16_backend = {fir16_create, fir16_run, fir16_destroy};

/* The discriminator + matched filter on a test FM signal, with and without the backend: max difference in Hz. */
static double fir_check(const rdof_fir_backend_t *backend)
{
    enum { N = 2000 };
    rdof_cf_t *x = malloc(sizeof(rdof_cf_t) * N);
    float *fa = malloc(sizeof(float) * N), *fb = malloc(sizeof(float) * N), *p = malloc(sizeof(float) * N);
    double ph = 0;
    for (int i = 0; i < N; i++) {
        ph += 2 * M_PI * (1500.0 * sin(i * 0.01) + 300.0 * ((i / 10) % 3 - 1)) / 48000.0;
        x[i].re = (float)cos(ph);
        x[i].im = (float)sin(ph);
    }
    rdof_fm_t a, b;
    rdof_fm_set_fir_backend(NULL);
    rdof_fm_init(&a, 48000.0);
    rdof_fm_process(&a, x, N, fa, p);
    rdof_fm_set_fir_backend(backend);
    rdof_fm_init(&b, 48000.0);
    rdof_fm_process(&b, x, N, fb, p);
    rdof_fm_set_fir_backend(NULL);
    double err = b.fir ? 0 : INFINITY;
    for (int i = 0; i < N; i++)
        err = fmax(err, fabs(fa[i] - fb[i]));
    rdof_fm_free(&a);
    rdof_fm_free(&b);
    free(x);
    free(fa);
    free(fb);
    free(p);
    return err;
}

void dsp_esp_fft_enable(bool on) { rdof_fft_set_backend(on && g_esp_fft ? &fft_backend : NULL); }

void dsp_esp_install(void)
{
    if (dsps_fft2r_init_fc32(NULL, FFT2_MAX) == ESP_OK && dsps_fft4r_init_fc32(NULL, FFT4_MAX) == ESP_OK) {
        /* 64 inverse and forward on radix-4, 512 and 32 on radix-2, then 384 (6 x 64) */
        double e = fmax(fmax(fft_check(64, true), fft_check(64, false)), fmax(fft_check(512, false), fft_check(32, true)));
        if (e > 1e-4) {
            fft_conj_trick = true;
            e = fmax(fmax(fft_check(64, true), fft_check(64, false)), fmax(fft_check(512, false), fft_check(32, true)));
        }
        e = fmax(e, fft_check(384, false));
        g_esp_fft = e < 1e-4;
        /* the P x Q forward plans (576, 960 and 1152 kS/s): left out on their own if they don't agree */
        const double epq = fmax(fft_check(768, false), fmax(fft_check(1280, false), fft_check(1536, false)));
        fft_pq_ok = g_esp_fft && epq < 1e-4;
        ESP_LOGI(TAG, "FFT 768 / 1280 / 1536 as 3 / 5 / 6 x 256: %s (error %.2g)", fft_pq_ok ? "in use" : "LEFT OUT",
                 epq);
        ESP_LOGI(TAG, "FFT (dsps_fft4r_fc32 to %d and 384 as 6 x 64, dsps_fft2r_fc32 to %d): %s (error %.2g of the largest bin)%s", FFT4_MAX, FFT2_MAX,
                 g_esp_fft ? "in use" : "LEFT OUT",
                 e, fft_conj_trick && g_esp_fft ? ", sign flipped" : "");
    }
    /* the matched filter: int16 SIMD if it agrees to 2 Hz, else esp-dsp's float FIR to 0.05 Hz */
    const rdof_fir_backend_t *fir = NULL;
    double e16 = fir_check(&fir16_backend), e = INFINITY;
    if (e16 <= 2.0) {
        fir = &fir16_backend;
    } else {
        e = fir_check(&fir_backend);
        if (e > 0.05) {
            fir_pad_front = 1;
            e = fir_check(&fir_backend);
        }
        if (e <= 0.05)
            fir = &fir_backend;
    }
    g_esp_fir = fir != NULL;
    ESP_LOGI(TAG, "matched filter: %s (int16 SIMD within %.2g Hz%s)",
             fir == &fir16_backend ? "int16 SIMD" : fir ? "dsps_fir_f32" : "built-in", e16,
             fir == &fir16_backend ? "" : ", float");
    if (g_esp_fft)
        rdof_fft_set_backend(&fft_backend);
    if (fir)
        rdof_fm_set_fir_backend(fir);
}

#else

void dsp_esp_install(void) {}
void dsp_esp_fft_enable(bool on) {}

#endif
