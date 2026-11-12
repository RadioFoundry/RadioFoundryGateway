// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * DTMF digits in a voice-band signal (rdof/sdr/dtmfdet.h).
 */
#include "rdof/sdr/dtmfdet.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define WIN_S 0.04
#define HOP_S 0.005
#define OVER_DB 10.0f
#define PURITY 0.6f
#define MIN_S 0.03
#define MAXH 1024 /* windows kept for a run's medians (5 s); a longer run keeps its first */
#define QRUNS 32

static const float TONES[8] = {697, 770, 852, 941, 1209, 1336, 1477, 1633};
static const char KEYS[] = "123A456B789C*0#D";

struct rdof_dtmfdet {
    void (*dealloc)(void *);
    double fs;
    int win, hop;
    float *ring, *hann, *seg;
    float coef[8];
    long n;                   /* samples in */
    long next;                /* the next window's end (exclusive) */
    /* the run going on */
    char cur;                 /* 0 none */
    double t0, t1;
    int nh;
    float *devs, *twists;
    rdof_dtmf_run_t q[QRUNS];
    int qh, qn;
};

rdof_dtmfdet_t *rdof_dtmfdet_new(double fs, void *(*alloc)(size_t), void (*dealloc)(void *))
{
    if (!(fs >= 8000 && fs <= 48000))
        return NULL;
    if (!alloc)
        alloc = malloc, dealloc = free;
    rdof_dtmfdet_t *d = alloc(sizeof *d);
    if (!d)
        return NULL;
    memset(d, 0, sizeof *d);
    d->dealloc = dealloc;
    d->fs = fs;
    d->win = (int)(WIN_S * fs), d->hop = (int)(HOP_S * fs);
    d->ring = alloc(sizeof(float) * (size_t)d->win);
    d->hann = alloc(sizeof(float) * (size_t)d->win);
    d->seg = alloc(sizeof(float) * (size_t)d->win);
    d->devs = alloc(sizeof(float) * MAXH);
    d->twists = alloc(sizeof(float) * MAXH);
    if (!d->ring || !d->hann || !d->seg || !d->devs || !d->twists) {
        rdof_dtmfdet_free(d);
        return NULL;
    }
    for (int i = 0; i < d->win; i++) /* numpy's hanning: zero at both ends */
        d->hann[i] = (float)(0.5 - 0.5 * cos(2 * M_PI * i / (d->win - 1)));
    for (int k = 0; k < 8; k++)
        d->coef[k] = (float)(2 * cos(2 * M_PI * TONES[k] / fs));
    memset(d->ring, 0, sizeof(float) * (size_t)d->win);
    d->next = d->win;
    return d;
}

void rdof_dtmfdet_free(rdof_dtmfdet_t *d)
{
    if (!d)
        return;
    void (*f)(void *) = d->dealloc ? d->dealloc : free;
    f(d->ring), f(d->hann), f(d->seg), f(d->devs), f(d->twists);
    f(d);
}

static int cmpf(const void *a, const void *b)
{
    const float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

static float median(float *v, int n)
{
    qsort(v, (size_t)n, sizeof *v, cmpf);
    return n % 2 ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
}

static void close_run(rdof_dtmfdet_t *d)
{
    if (!d->cur)
        return;
    const double t0 = d->t0 - HOP_S / 2, t1 = d->t1 + HOP_S / 2;
    if (t1 - t0 >= MIN_S && d->nh) {
        if (d->qn == QRUNS)
            d->qh = (d->qh + 1) % QRUNS, d->qn--;
        rdof_dtmf_run_t *r = &d->q[(d->qh + d->qn++) % QRUNS];
        r->digit = d->cur, r->t0 = t0, r->t1 = t1;
        r->dev_hz = median(d->devs, d->nh);
        r->twist_db = median(d->twists, d->nh);
    }
    d->cur = 0, d->nh = 0;
}

/* The window ending at sample d->next (exclusive): a digit, or none */
static void window(rdof_dtmfdet_t *d)
{
    const int w = d->win;
    const long start = d->next - w;
    /* in float: an ESP32's FPU has no double, and a double sum here took more of a W12's core than the Goertzels */
    float sum = 0, sum2 = 0, tot = 0;
    for (int i = 0; i < w; i++) {
        const float x = d->ring[(start + i) % w];
        sum += x, sum2 += x * x;
        const float y = x * d->hann[i];
        d->seg[i] = y;
        tot += y * y;
    }
    float p[8];
    for (int k = 0; k < 8; k++) { /* Goertzel: |sum y[n] e^-jwn|^2 */
        float s1 = 0, s2 = 0;
        const float c = d->coef[k];
        for (int i = 0; i < w; i++) {
            const float s = d->seg[i] + c * s1 - s2;
            s2 = s1, s1 = s;
        }
        p[k] = s1 * s1 + s2 * s2 - c * s1 * s2;
    }
    int r = 0, col = 4;
    for (int k = 1; k < 4; k++)
        if (p[k] > p[r])
            r = k;
    for (int k = 5; k < 8; k++)
        if (p[k] > p[col])
            col = k;
    float others = 0;
    for (int k = 0; k < 8; k++)
        if (k != r && k != col && p[k] > others)
            others = p[k];
    const double total = (double)tot * w / 3.0 + 1e-12; /* a Hann-windowed tone's Goertzel power is N / 3 of its energy */
    const float lo = p[r], hi = p[col], m = lo < hi ? lo : hi;
    const bool ok = m > others * powf(10, OVER_DB / 10) && lo + hi >= PURITY * total;
    const double t = (start + w / 2.0) / d->fs;
    const char key = ok ? KEYS[r * 4 + (col - 4)] : 0;
    if (key != d->cur)
        close_run(d);
    if (!key)
        return;
    if (!d->cur)
        d->cur = key, d->t0 = t;
    d->t1 = t;
    if (d->nh < MAXH) {
        const double mean = (double)sum / w, var = (double)sum2 / w - mean * mean;
        d->devs[d->nh] = (float)(sqrt(2.0) * sqrt(var > 0 ? var : 0));
        d->twists[d->nh] = 10 * log10f((hi + 1e-30f) / (lo + 1e-30f));
        d->nh++;
    }
}

void rdof_dtmfdet_feed(rdof_dtmfdet_t *d, const float *x, int n)
{
    if (!d)
        return;
    for (int i = 0; i < n; i++) {
        d->ring[d->n % d->win] = x[i];
        if (++d->n == d->next) {
            window(d);
            d->next += d->hop;
        }
    }
}

void rdof_dtmfdet_finish(rdof_dtmfdet_t *d)
{
    if (d)
        close_run(d);
}

bool rdof_dtmfdet_take(rdof_dtmfdet_t *d, rdof_dtmf_run_t *out)
{
    if (!d || !d->qn)
        return false;
    *out = d->q[d->qh];
    d->qh = (d->qh + 1) % QRUNS, d->qn--;
    return true;
}
