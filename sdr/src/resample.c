// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The rational resampler (rdof/sdr/resample.h). */
#include "rdof/sdr/resample.h"
#include "rdof/sdr/features.h"

#if RDOF_FEATURE_SDR

#include <math.h>
#include <string.h>

#include "sdr_private.h"

static long gcd(long a, long b)
{
    while (b) {
        long t = a % b;
        a = b;
        b = t;
    }
    return a;
}

/* The zeroth-order modified Bessel function, by its series. */
static double bessel_i0(double x)
{
    double s = 1, t = 1;
    for (int k = 1; k < 50; k++) {
        t *= (x / (2 * k)) * (x / (2 * k));
        s += t;
        if (t < 1e-12 * s)
            break;
    }
    return s;
}

int rdof_resamp_init(rdof_resamp_t *r, long in_rate, long out_rate, int K, double usable, int max_in)
{
    memset(r, 0, sizeof *r);
    if (in_rate <= 0 || out_rate <= 0 || K < 2 || max_in < 1)
        return -1;
    long g = gcd(in_rate, out_rate);
    r->L = (int)(out_rate / g);
    r->M = (int)(in_rate / g);
    if (r->L > 1000 || r->M > 1000)
        return -1;
    r->K = K;
    r->cap = max_in;
    const int N = r->L * K;
    r->h = rdof_sdr_alloc(sizeof(float) * (size_t)N);
    r->buf = rdof_sdr_zalloc(2 * sizeof(float) * (size_t)(K - 1 + max_in));
    if (!r->h || !r->buf) {
        rdof_resamp_free(r);
        return -1;
    }
    /* cutoff halfway between the usable band's edge and where aliases into it start, at L x the input rate */
    const double fp = usable * (double)out_rate / 2;
    const double fs = (double)(in_rate < out_rate ? in_rate : out_rate) - fp;
    const double fc = (fp + fs) / 2 / ((double)r->L * (double)in_rate); /* cycles a sample at the high rate */
    const double beta = 7.857;                                         /* about 80 dB down */
    const double mid = (N - 1) / 2.0, i0b = bessel_i0(beta);
    for (int n = 0; n < N; n++) {
        const double x = n - mid;
        const double sinc = x == 0 ? 2 * fc : sin(2 * RDOF_PI * fc * x) / (RDOF_PI * x);
        const double w = bessel_i0(beta * sqrt(1 - (x / mid) * (x / mid))) / i0b;
        const double v = r->L * sinc * w;
        /* phase p, tap j is prototype tap p + j L; stored reversed, so an output is a dot product over its
         * inputs oldest first */
        r->h[(n % r->L) * K + K - 1 - n / r->L] = (float)v;
    }
    r->next = K - 1;
    return 0;
}

void rdof_resamp_free(rdof_resamp_t *r)
{
    rdof_sdr_free(r->h);
    rdof_sdr_free(r->buf);
    r->h = NULL;
    r->buf = NULL;
}

int rdof_resamp_max_out(const rdof_resamp_t *r, int n) { return (int)((long)n * r->L / r->M) + 2; }

int rdof_resamp_run(rdof_resamp_t *r, const rdof_cf_t *in, int n, rdof_cf_t *out)
{
    const int K = r->K, hist = K - 1;
    float *re = r->buf, *im = r->buf + hist + r->cap;
    if (n > r->cap)
        n = r->cap;
    for (int i = 0; i < n; i++) {
        re[hist + i] = in[i].re;
        im[hist + i] = in[i].im;
    }
    const int total = hist + n;
    const int step = r->M / r->L, rem = r->M % r->L;
    int k = 0;
    while (r->next < total) {
        const float *h = r->h + r->phase * K;
        const int first = r->next - hist; /* the oldest of the K inputs */
        /* four sums at once: a single running sum waits on each add (about 140 cycles an output on the S3 at
         * 12 taps); esp-dsp's dsps_dotprod_f32 was slower at these lengths */
        const float *xr = re + first, *xi = im + first;
        float r0 = 0, r1 = 0, i0 = 0, i1 = 0;
        int j = 0;
        for (; j + 1 < K; j += 2) {
            r0 += h[j] * xr[j];
            i0 += h[j] * xi[j];
            r1 += h[j + 1] * xr[j + 1];
            i1 += h[j + 1] * xi[j + 1];
        }
        if (j < K) {
            r0 += h[j] * xr[j];
            i0 += h[j] * xi[j];
        }
        out[k].re = r0 + r1;
        out[k].im = i0 + i1;
        k++;
        /* phase + M, carried into next: a divide an output cost about a fifth of the time on the S3 */
        r->next += step;
        r->phase += rem;
        if (r->phase >= r->L) {
            r->phase -= r->L;
            r->next++;
        }
    }
    /* keep the newest K - 1 inputs as the next block's history */
    memmove(re, re + n, sizeof(float) * (size_t)hist);
    memmove(im, im + n, sizeof(float) * (size_t)hist);
    r->next -= n;
    return k;
}

#endif
