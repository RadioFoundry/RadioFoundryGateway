// SPDX-License-Identifier: MIT AND BSD-3-Clause
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Built-in mixed-radix FFT (radix 4, 2, 3, 5), decimation in time, after the
 * structure of KISS FFT. Plans allocate once; running allocates nothing.
 */
#include "rdof/sdr/fft.h"
#include "rdof/sdr/features.h"
#include "sdr_private.h"

#if RDOF_FEATURE_SDR && RDOF_FEATURE_SDR_BUILTIN_FFT

#include <math.h>

#include "sdr_private.h"

#define MAX_FACTORS 32

struct rdof_fft {
    const rdof_fft_backend_t *ext; /* set: plan is the backend's */
    void *plan;
    int n;
    bool inverse;
    int factors[2 * MAX_FACTORS]; /* (radix, remaining length) pairs */
    rdof_cf_t *tw;                  /* e^{-+j 2 pi i / n} */
};

static inline rdof_cf_t cmul(rdof_cf_t a, rdof_cf_t b)
{
    rdof_cf_t r = {a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re};
    return r;
}

static void bfly2(rdof_cf_t *out, int fstride, const rdof_fft_t *st, int m)
{
    const rdof_cf_t *tw = st->tw;
    rdof_cf_t *o2 = out + m;
    for (int k = 0; k < m; k++) {
        rdof_cf_t t = cmul(o2[k], *tw);
        tw += fstride;
        o2[k].re = out[k].re - t.re;
        o2[k].im = out[k].im - t.im;
        out[k].re += t.re;
        out[k].im += t.im;
    }
}

static void bfly4(rdof_cf_t *out, int fstride, const rdof_fft_t *st, int m)
{
    const rdof_cf_t *tw1 = st->tw, *tw2 = st->tw, *tw3 = st->tw;
    const int m2 = 2 * m, m3 = 3 * m;
    for (int k = 0; k < m; k++, out++) {
        rdof_cf_t s0 = cmul(out[m], *tw1), s1 = cmul(out[m2], *tw2), s2 = cmul(out[m3], *tw3);
        rdof_cf_t s5 = {out[0].re - s1.re, out[0].im - s1.im};
        out[0].re += s1.re;
        out[0].im += s1.im;
        rdof_cf_t s3 = {s0.re + s2.re, s0.im + s2.im};
        rdof_cf_t s4 = {s0.re - s2.re, s0.im - s2.im};
        out[m2].re = out[0].re - s3.re;
        out[m2].im = out[0].im - s3.im;
        tw1 += fstride;
        tw2 += 2 * fstride;
        tw3 += 3 * fstride;
        out[0].re += s3.re;
        out[0].im += s3.im;
        if (st->inverse) {
            out[m].re = s5.re - s4.im;
            out[m].im = s5.im + s4.re;
            out[m3].re = s5.re + s4.im;
            out[m3].im = s5.im - s4.re;
        } else {
            out[m].re = s5.re + s4.im;
            out[m].im = s5.im - s4.re;
            out[m3].re = s5.re - s4.im;
            out[m3].im = s5.im + s4.re;
        }
    }
}

static void bfly3(rdof_cf_t *out, int fstride, const rdof_fft_t *st, int m)
{
    const rdof_cf_t *tw1 = st->tw, *tw2 = st->tw;
    const float epi3 = st->tw[fstride * m].im;
    const int m2 = 2 * m;
    for (int k = 0; k < m; k++, out++) {
        rdof_cf_t s1 = cmul(out[m], *tw1), s2 = cmul(out[m2], *tw2);
        rdof_cf_t s3 = {s1.re + s2.re, s1.im + s2.im};
        rdof_cf_t s0 = {s1.re - s2.re, s1.im - s2.im};
        tw1 += fstride;
        tw2 += 2 * fstride;
        out[m].re = out[0].re - 0.5f * s3.re;
        out[m].im = out[0].im - 0.5f * s3.im;
        s0.re *= epi3;
        s0.im *= epi3;
        out[0].re += s3.re;
        out[0].im += s3.im;
        out[m2].re = out[m].re + s0.im;
        out[m2].im = out[m].im - s0.re;
        out[m].re -= s0.im;
        out[m].im += s0.re;
    }
}

/* Any radix (used for 5): O(p^2) per group, fine for the odd factor. */
static void bfly_generic(rdof_cf_t *out, int fstride, const rdof_fft_t *st, int m, int p)
{
    rdof_cf_t scratch[8];
    const int n = st->n;
    for (int u = 0; u < m; u++) {
        for (int q1 = 0, k = u; q1 < p; q1++, k += m)
            scratch[q1] = out[k];
        for (int q1 = 0, k = u; q1 < p; q1++, k += m) {
            int twidx = 0;
            rdof_cf_t acc = scratch[0];
            for (int q = 1; q < p; q++) {
                twidx += fstride * k;
                if (twidx >= n)
                    twidx -= n * (twidx / n);
                rdof_cf_t t = cmul(scratch[q], st->tw[twidx]);
                acc.re += t.re;
                acc.im += t.im;
            }
            out[k] = acc;
        }
    }
}

static void work(rdof_cf_t *out, const rdof_cf_t *in, int fstride, const int *factors, const rdof_fft_t *st)
{
    const int p = factors[0], m = factors[1];
    rdof_cf_t *const beg = out, *const end = out + p * m;
    if (m == 1) {
        for (; out != end; out++, in += fstride)
            *out = *in;
    } else {
        for (; out != end; out += m, in += fstride)
            work(out, in, fstride * p, factors + 2, st);
    }
    out = beg;
    switch (p) {
    case 2: bfly2(out, fstride, st, m); break;
    case 3: bfly3(out, fstride, st, m); break;
    case 4: bfly4(out, fstride, st, m); break;
    default: bfly_generic(out, fstride, st, m, p); break;
    }
}

static const rdof_fft_backend_t *backend;

void rdof_fft_set_backend(const rdof_fft_backend_t *b) { backend = b; }

rdof_fft_t *rdof_fft_new(int n, bool inverse)
{
    if (n < 1)
        return NULL;
    rdof_fft_t *f = rdof_sdr_zalloc(sizeof *f);
    if (!f)
        return NULL;
    if (backend && (f->plan = backend->create(n, inverse)) != NULL) {
        f->ext = backend;
        f->n = n;
        f->inverse = inverse;
        return f;
    }
    f->n = n;
    f->inverse = inverse;
    int rest = n, nf = 0;
    static const int radices[] = {4, 2, 3, 5};
    for (int r = 0; r < 4; r++) {
        while (rest % radices[r] == 0 && rest > 1) {
            rest /= radices[r];
            f->factors[2 * nf] = radices[r];
            f->factors[2 * nf + 1] = rest;
            nf++;
        }
    }
    if (n == 1) {
        f->factors[0] = 1;
        f->factors[1] = 1;
    } else if (rest != 1) {
        rdof_sdr_free(f);
        return NULL;
    }
    f->tw = rdof_sdr_alloc_hot(sizeof(rdof_cf_t) * (size_t)n);
    if (!f->tw) {
        rdof_sdr_free(f);
        return NULL;
    }
    const double sign = inverse ? 1.0 : -1.0;
    for (int i = 0; i < n; i++) {
        double ph = sign * 2.0 * RDOF_PI * (double)i / (double)n;
        f->tw[i].re = (float)cos(ph);
        f->tw[i].im = (float)sin(ph);
    }
    return f;
}

void rdof_fft_free(rdof_fft_t *f)
{
    if (!f)
        return;
    if (f->ext)
        f->ext->destroy(f->plan);
    rdof_sdr_free(f->tw);
    rdof_sdr_free(f);
}

void rdof_fft_run(rdof_fft_t *f, const rdof_cf_t *in, rdof_cf_t *out)
{
    if (f->ext) {
        f->ext->run(f->plan, in, out);
        return;
    }
    if (f->n == 1) {
        out[0] = in[0];
        return;
    }
    work(out, in, 1, f->factors, f);
}

#endif
