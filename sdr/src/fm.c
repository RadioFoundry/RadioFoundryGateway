// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * FM discriminator + RRC matched filter + |x|^2, in fixed blocks
 * so it needs no heap.
 */
#include "rdof/sdr/fm.h"

#if RDOF_FEATURE_SDR

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "sdr_private.h"

int rdof_rrc_taps(int sps, double alpha, int span, double *out)
{
    const int n = span * sps + 1, lo = -((span * sps + 1) / 2); /* floor(-span * sps / 2), rounded toward minus infinity */
    double energy = 0.0;
    for (int i = 0; i < n; i++) {
        double t = (double)(lo + i) / sps, h;
        if (fabs(t) < 1e-9) {
            h = 1 - alpha + 4 * alpha / RDOF_PI;
        } else if (fabs(fabs(4 * alpha * t) - 1) < 1e-9) {
            h = alpha / sqrt(2.0) *
                ((1 + 2 / RDOF_PI) * sin(RDOF_PI / (4 * alpha)) + (1 - 2 / RDOF_PI) * cos(RDOF_PI / (4 * alpha)));
        } else {
            h = (sin(RDOF_PI * t * (1 - alpha)) + 4 * alpha * t * cos(RDOF_PI * t * (1 + alpha))) /
                (RDOF_PI * t * (1 - (4 * alpha * t) * (4 * alpha * t)));
        }
        out[i] = h;
        energy += h * h;
    }
    const double norm = sqrt(energy);
    for (int i = 0; i < n; i++)
        out[i] /= norm;
    return n;
}

static const rdof_fir_backend_t *fir_backend;

void rdof_fm_set_fir_backend(const rdof_fir_backend_t *b) { fir_backend = b; }

void rdof_fm_free(rdof_fm_t *s)
{
    if (s->fir && s->fir_state)
        s->fir->destroy(s->fir_state);
    s->fir = NULL;
    s->fir_state = NULL;
}

void rdof_fm_init(rdof_fm_t *s, double fs)
{
    memset(s, 0, sizeof *s);
    double h[RDOF_FM_TAPS], sum = 0.0;
    rdof_rrc_taps(RDOF_FM_SPS, 0.2, 8, h);
    for (int i = 0; i < RDOF_FM_TAPS; i++)
        sum += h[i];
    for (int i = 0; i < RDOF_FM_TAPS; i++)
        s->h[i] = (float)(h[i] / sum);
    s->symmetric = 1;
    for (int i = 0; i < RDOF_FM_TAPS / 2; i++)
        if (s->h[i] != s->h[RDOF_FM_TAPS - 1 - i])
            s->symmetric = 0;
    s->scale = (float)(fs / (2.0 * RDOF_PI));
    if (fir_backend && (s->fir_state = fir_backend->create(s->h, RDOF_FM_TAPS)) != NULL)
        s->fir = fir_backend;
}

#if RDOF_SDR_FAST_ATAN2
/* A float's bits and back, register to register on the S3 (rfr / wfr), not through memory. */
static inline uint32_t f2u(float f)
{
    uint32_t u;
#if defined(__XTENSA__)
    __asm__("rfr %0, %1" : "=a"(u) : "f"(f));
#else
    memcpy(&u, &f, sizeof u);
#endif
    return u;
}

static inline float u2f(uint32_t u)
{
    float f;
#if defined(__XTENSA__)
    __asm__("wfr %0, %1" : "=f"(f) : "a"(u));
#else
    memcpy(&f, &u, sizeof f);
#endif
    return f;
}

/* atan2 to 2e-4 rad (1.6 Hz at 48 kS/s), plenty for 4FSK levels hundreds of Hz apart, with
 * no divide (a library call on the S3 and the P4) and no branches. The octant comes from the bits in
 * integer registers: base angle 0, +-pi/2 or +-pi from a table, plus atan of a ratio in [-1, 1]:
 *   |im| <= |re|: base + atan(im / re), base 0 (re >= 0) or +-pi by im's sign bit (-0.0 too)
 *   |im| >  |re|: base + atan(-re / im), base +-pi/2 by im's sign
 * 1 / |den| is the bit trick's guess (within 12%) and three Newton steps (to about 4e-8). |den| is at least
 * FLT_MIN, so atan2(0, 0) is 0. */
static const float atan2_base[8] = {0.0f, 0.0f, 3.14159274f, -3.14159274f, 1.57079637f, -1.57079637f, 1.57079637f,
                                    -1.57079637f};

static inline float atan2_(float y, float x)
{
    const uint32_t ux = f2u(x), uy = f2u(y), ax = ux & 0x7FFFFFFFu, ay = uy & 0x7FFFFFFFu;
    const uint32_t steep = ay > ax;
    const uint32_t den = steep ? uy : ux, num = steep ? ux ^ 0x80000000u : uy;
    uint32_t aden = den & 0x7FFFFFFFu;
    aden = aden > 0x00800000u ? aden : 0x00800000u;
    const float d = u2f(aden), n = u2f(num ^ (den & 0x80000000u)); /* n / d = num / den */
    float r = u2f(0x7EF311C3u - aden);
    r = r * (2.0f - d * r);
    r = r * (2.0f - d * r);
    r = r * (2.0f - d * r);
    const float a = n * r, s2 = a * a;
    const float poly = ((-0.0464964749f * s2 + 0.15931422f) * s2 - 0.327622764f) * s2;
    return atan2_base[steep * 4 + (ux >> 31) * 2 + (uy >> 31)] + (a + a * poly);
}
#else
static inline float atan2_(float y, float x) { return atan2f(y, x); }
#endif

void rdof_fm_discriminate(rdof_cf_t *prev_io, float scale, const rdof_cf_t *x, int n, float *out)
{
    rdof_cf_t prev = *prev_io;
    int i = 0;
    for (; i + 1 < n; i += 2) { /* two at a time: independent work for the pipeline */
        const rdof_cf_t a = x[i], b = x[i + 1];
        const float re0 = a.re * prev.re + a.im * prev.im, im0 = a.im * prev.re - a.re * prev.im; /* x * conj(prev) */
        const float re1 = b.re * a.re + b.im * a.im, im1 = b.im * a.re - b.re * a.im;
        out[i] = atan2_(im0, re0) * scale;
        out[i + 1] = atan2_(im1, re1) * scale;
        prev = b;
    }
    for (; i < n; i++) {
        float re = x[i].re * prev.re + x[i].im * prev.im;
        float im = x[i].im * prev.re - x[i].re * prev.im;
        out[i] = atan2_(im, re) * scale;
        prev = x[i];
    }
    *prev_io = prev;
}

static void fm_block(rdof_fm_t *s, const rdof_cf_t *x, int n, float *outf, float *outp)
{
    enum { T = RDOF_FM_TAPS - 1, D = RDOF_FM_DELAY };
    float *w = s->hist; /* T history values, then n new ones */
    rdof_fm_discriminate(&s->prev, s->scale, x, n, w + T);
    const float *h = s->h;
    if (s->fir) {
        s->fir->run(s->fir_state, w + T, outf, n);
    } else if (s->symmetric) {
        for (int i = 0; i < n; i++) {
            const float *p = w + i;
            float acc = h[T / 2] * p[T / 2];
            for (int j = 0; j < T / 2; j++)
                acc += h[j] * (p[T - j] + p[j]);
            outf[i] = acc;
        }
    } else {
        for (int i = 0; i < n; i++) {
            const float *p = w + i;
            float acc = 0.0f;
            for (int j = 0; j <= T; j++)
                acc += h[j] * p[T - j];
            outf[i] = acc;
        }
    }
    memmove(w, w + n, sizeof(float) * T);
    /* power, delayed by D samples to line up with the filter output */
    const int held = n < D ? n : D;
    for (int i = 0; i < held; i++)
        outp[i] = s->phist[i];
    for (int i = D; i < n; i++) {
        const rdof_cf_t v = x[i - D];
        outp[i] = v.re * v.re + v.im * v.im;
    }
    if (n >= D) {
        for (int i = 0; i < D; i++) {
            const rdof_cf_t v = x[n - D + i];
            s->phist[i] = v.re * v.re + v.im * v.im;
        }
    } else {
        memmove(s->phist, s->phist + n, sizeof(float) * (D - n));
        for (int i = 0; i < n; i++)
            s->phist[D - n + i] = x[i].re * x[i].re + x[i].im * x[i].im;
    }
}

void rdof_fm_process(rdof_fm_t *s, const rdof_cf_t *in, int n, float *outf, float *outp)
{
    for (int done = 0; done < n;) {
        int k = n - done < RDOF_FM_BLOCK ? n - done : RDOF_FM_BLOCK;
        fm_block(s, in + done, k, outf + done, outp + done);
        done += k;
    }
}

#endif
