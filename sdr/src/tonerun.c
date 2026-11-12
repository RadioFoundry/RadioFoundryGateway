// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Timed steady tones in a voice-band signal (rdof/sdr/tonerun.h).
 */
#include "rdof/sdr/tonerun.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/sdr/fft.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define WIN_S 0.02
#define HOP_S 0.005
#define LO_HZ 300.0f
#define HI_HZ 3000.0f
#define BAND_LO 250.0f       /* the band whose energy a peak is measured against */
#define BAND_HI 3300.0f
#define PURITY 0.5f
#define MIN_DEV 30.0f        /* Hz: less is no tone */
#define PITCH_HZ 40.0f
#define EDGE 0.25f           /* of the median power: a Hann window centered on the tone's edge */
#define JOIN_S 0.05
#define MIN_PIECE_S 0.015
#define MIN_S 0.04
#define MAXH 256             /* windows kept for a piece's medians (1.28 s); a longer one keeps its first */
#define TAIL 16              /* the last windows' power, for the end's edge */
#define QRUNS 32

struct rdof_tonerun {
    void (*dealloc)(void *);
    double fs;               /* after the averaging */
    double lag;              /* the averaging's own delay, s */
    int dec, nacc;
    float acc;
    int win, hop, nfft, k0, k1, lobe; /* lobe: bins each side of a peak, a Hann window's main lobe */
    float bin, sumw2;
    float *ring, *hann, *pw;
    rdof_cf_t *in, *out;
    rdof_fft_t *fft;
    long n, next;
    /* the piece going on */
    int nh;                  /* its windows */
    float ref;               /* its pitch so far */
    double first, last;      /* its first and last windows' centers */
    float *hs, *ds, *ps;     /* the first MAXH windows' pitch, deviation, power */
    float tail[TAIL];
    /* the run being built */
    bool open;
    double t0, t1;
    float hsum, dsum, wsum;
    rdof_tone_run_t q[QRUNS];
    int qh, qn;
};

void rdof_tonerun_free(rdof_tonerun_t *d)
{
    if (!d)
        return;
    void (*f)(void *) = d->dealloc ? d->dealloc : free;
    if (d->fft)
        rdof_fft_free(d->fft);
    f(d->ring), f(d->hann), f(d->pw), f(d->in), f(d->out), f(d->hs), f(d->ds), f(d->ps);
    f(d);
}

rdof_tonerun_t *rdof_tonerun_new(double fs, void *(*alloc)(size_t), void (*dealloc)(void *))
{
    if (!(fs >= 8000 && fs <= 48000))
        return NULL;
    if (!alloc)
        alloc = malloc, dealloc = free;
    rdof_tonerun_t *d = alloc(sizeof *d);
    if (!d)
        return NULL;
    memset(d, 0, sizeof *d);
    d->dealloc = dealloc;
    d->dec = (int)(fs / 8000);
    d->fs = fs / d->dec;
    d->lag = (d->dec - 1) / 2.0 / fs;
    d->win = (int)lround(WIN_S * d->fs), d->hop = (int)lround(HOP_S * d->fs);
    for (d->nfft = 64; d->nfft < d->win * 3 / 2;)
        d->nfft *= 2; /* zero padded, for the parabola */
    d->bin = (float)(d->fs / d->nfft);
    d->lobe = (int)ceil(2.5 * d->nfft / d->win);
    d->k0 = (int)ceilf(BAND_LO / d->bin), d->k1 = (int)floorf(BAND_HI / d->bin);
    d->ring = alloc(sizeof(float) * (size_t)d->win);
    d->hann = alloc(sizeof(float) * (size_t)d->win);
    d->pw = alloc(sizeof(float) * (size_t)(d->k1 + 2));
    d->in = alloc(sizeof(rdof_cf_t) * (size_t)d->nfft);
    d->out = alloc(sizeof(rdof_cf_t) * (size_t)d->nfft);
    d->hs = alloc(sizeof(float) * MAXH);
    d->ds = alloc(sizeof(float) * MAXH);
    d->ps = alloc(sizeof(float) * MAXH);
    d->fft = rdof_fft_new_cold(d->nfft, false);
    if (!d->ring || !d->hann || !d->pw || !d->in || !d->out || !d->hs || !d->ds || !d->ps || !d->fft) {
        rdof_tonerun_free(d);
        return NULL;
    }
    for (int i = 0; i < d->win; i++) {
        d->hann[i] = (float)(0.5 - 0.5 * cos(2 * M_PI * i / (d->win - 1)));
        d->sumw2 += d->hann[i] * d->hann[i];
    }
    memset(d->ring, 0, sizeof(float) * (size_t)d->win);
    d->next = d->win;
    return d;
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

static void end_run(rdof_tonerun_t *d)
{
    if (!d->open)
        return;
    d->open = false;
    if (d->t1 - d->t0 < MIN_S)
        return;
    if (d->qn == QRUNS)
        d->qh = (d->qh + 1) % QRUNS, d->qn--;
    rdof_tone_run_t *r = &d->q[(d->qh + d->qn++) % QRUNS];
    r->t0 = d->t0, r->t1 = d->t1, r->hz = d->hsum / d->wsum, r->dev_hz = d->dsum / d->wsum;
}

/* The piece is over: its edges, then into the run or a run of its own. */
static void end_piece(rdof_tonerun_t *d)
{
    if (!d->nh)
        return;
    const double hop = (double)d->hop / d->fs;
    const int nh = d->nh, kept = nh < MAXH ? nh : MAXH, nt = nh < TAIL ? nh : TAIL;
    d->nh = 0;

    /* the start's edge first: median sorts ps */
    float head[TAIL];
    const int nhead = kept < TAIL ? kept : TAIL;
    memcpy(head, d->ps, sizeof(float) * (size_t)nhead);
    const float level = median(d->ps, kept), thr = EDGE * level, hz = median(d->hs, kept), dev = median(d->ds, kept);
    double t0 = d->first - hop / 2, t1 = d->last + hop / 2;
    for (int i = 0; i < nhead; i++)
        if (head[i] >= thr) {
            if (i)
                t0 = d->first + hop * (i - 1 + (thr - head[i - 1]) / (head[i] - head[i - 1]));
            break;
        }
    for (int j = 0; j < nt; j++) { /* j windows back from the last */
        const float p = d->tail[(nh - 1 - j) % TAIL];
        if (p >= thr) {
            if (j) {
                const float after = d->tail[(nh - j) % TAIL];
                t1 = d->last - hop * j + hop * (p - thr) / (p - after);
            }
            break;
        }
    }

    /* the averaging's droop at this pitch */
    const float x = (float)M_PI * hz / (float)(d->fs * d->dec), droop = d->dec > 1 ? sinf(d->dec * x) / (d->dec * sinf(x)) : 1;
    const float w = (float)nh;
    const bool same = d->open && fabsf(hz - d->hsum / d->wsum) <= PITCH_HZ;
    if (same && t0 - d->t1 < JOIN_S) {
        if (t1 > d->t1)
            d->t1 = t1;
    } else {
        /* another pitch: too short to count, unless what's open is as short */
        if (d->open && t1 - t0 < MIN_PIECE_S && d->t1 - d->t0 >= MIN_PIECE_S)
            return;
        end_run(d);
        d->open = true, d->t0 = t0, d->t1 = t1;
        d->hsum = d->dsum = d->wsum = 0;
    }
    d->hsum += hz * w, d->dsum += dev / droop * w, d->wsum += w;
}

/* The window ending at decimated sample d->next (exclusive) */
static void window(rdof_tonerun_t *d)
{
    const int w = d->win;
    const long start = d->next - w;
    for (int i = 0; i < w; i++) {
        d->in[i].re = d->ring[(start + i) % w] * d->hann[i];
        d->in[i].im = 0;
    }
    memset(d->in + w, 0, sizeof(rdof_cf_t) * (size_t)(d->nfft - w));
    rdof_fft_run(d->fft, d->in, d->out);

    float band = 0, top = 0;
    int k = -1;
    for (int i = d->k0; i <= d->k1; i++) {
        const float p = d->out[i].re * d->out[i].re + d->out[i].im * d->out[i].im;
        d->pw[i] = p;
        band += p;
        if (p > top)
            top = p, k = i;
    }
    const double t = (start + w / 2.0) / d->fs - d->lag;
    if (d->open && !d->nh && t - d->t1 > JOIN_S)
        end_run(d);
    bool tone = k > d->k0 && k < d->k1 && top > 0;
    float hz = 0, energy = 0;
    if (tone) {
        const float a = logf(d->pw[k - 1] + 1e-20f), b = logf(top), c = logf(d->pw[k + 1] + 1e-20f);
        const float den = a - 2 * b + c, off = den < 0 ? 0.5f * (a - c) / den : 0;
        hz = ((float)k + (off > 1 ? 1 : off < -1 ? -1 : off)) * d->bin;
        for (int i = k - d->lobe; i <= k + d->lobe; i++)
            if (i >= d->k0 && i <= d->k1)
                energy += d->pw[i];
        /* x = A cos: the positive side's energy is A^2 / 4 * N * sum w^2 */
        const float dev = sqrtf(4 * energy / ((float)d->nfft * d->sumw2));
        tone = hz >= LO_HZ && hz <= HI_HZ && dev >= MIN_DEV && energy >= PURITY * band;
        if (tone) {
            if (d->nh && fabsf(hz - d->ref) > PITCH_HZ)
                end_piece(d);
            if (!d->nh)
                d->first = t, d->ref = hz;
            else
                d->ref += 0.2f * (hz - d->ref);
            d->last = t;
            if (d->nh < MAXH)
                d->hs[d->nh] = hz, d->ds[d->nh] = dev, d->ps[d->nh] = top;
            d->tail[d->nh % TAIL] = top;
            d->nh++;
        }
    }
    if (!tone)
        end_piece(d);
}

void rdof_tonerun_feed(rdof_tonerun_t *d, const float *x, int n)
{
    if (!d)
        return;
    for (int i = 0; i < n; i++) {
        d->acc += x[i];
        if (++d->nacc < d->dec)
            continue;
        d->ring[d->n % d->win] = d->acc / (float)d->dec;
        d->acc = 0, d->nacc = 0;
        if (++d->n == d->next) {
            window(d);
            d->next += d->hop;
        }
    }
}

void rdof_tonerun_finish(rdof_tonerun_t *d)
{
    if (!d)
        return;
    end_piece(d);
    end_run(d);
}

bool rdof_tonerun_take(rdof_tonerun_t *d, rdof_tone_run_t *out)
{
    if (!d || !d->qn)
        return false;
    *out = d->q[d->qh];
    d->qh = (d->qh + 1) % QRUNS, d->qn--;
    return true;
}
