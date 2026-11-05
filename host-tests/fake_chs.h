// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* A fake channelizer for the bench line's channel source (rdof_bl_platform_t.ch_open / ch_read / ch_close): an
 * FPGA's, made from a test's wideband history. Each channel mixed down by its NCO (the test steps it, as the
 * receiver's AFC does), a Hamming-windowed sinc to 6 kHz, decimated to an instant every round(fs / 48000) samples;
 * an instant whose center is filler in the history is zeros, flagged. Every channel kept from fchs_init on (as the
 * receiver keeps each channel's recent outputs), for the test's platform with iq_read NULL; a handle is the channel. */
#ifndef FAKE_CHS_H
#define FAKE_CHS_H

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/benchline.h"

#define FCHS_CH 4
#define FCHS_KEEP 480000 /* instants a channel keeps (10 s) */

static struct {
    const rdof_bl_ring_t *ring;
    double fs, center;
    int nch, d, ntaps;
    double freq[FCHS_CH];  /* the channels */
    double step_hz[FCHS_CH]; /* the AFC's step (Hz on the NCO), from wideband sample step_sc on */
    uint64_t step_sc[FCHS_CH];
    double ph[FCHS_CH];
    float *taps, *dl[FCHS_CH];
    int di;
    uint64_t done;         /* wideband samples taken */
    struct { long n; rdof_bl_chs_t *x; } h[FCHS_CH];
    int opened, closed;
} fchs;

static void fchs_init(const rdof_bl_ring_t *ring, double fs, double center, const double *freq, int nch)
{
    for (int c = 0; c < FCHS_CH; c++)
        free(fchs.dl[c]);
    free(fchs.taps);
    for (int k = 0; k < FCHS_CH; k++)
        free(fchs.h[k].x);
    memset(&fchs, 0, sizeof fchs);
    fchs.ring = ring, fchs.fs = fs, fchs.center = center, fchs.nch = nch;
    fchs.d = (int)lround(fs / 48000);
    fchs.ntaps = 8 * fchs.d + 1;
    fchs.taps = malloc(sizeof(float) * (size_t)fchs.ntaps);
    double sum = 0, fc = 6000 / fs, mid = (fchs.ntaps - 1) / 2.0;
    for (int i = 0; i < fchs.ntaps; i++) {
        const double x = i - mid;
        const double v = (x == 0 ? 2 * fc : sin(2 * M_PI * fc * x) / (M_PI * x)) *
                         (0.54 - 0.46 * cos(2 * M_PI * i / (fchs.ntaps - 1)));
        fchs.taps[i] = (float)v, sum += v;
    }
    for (int i = 0; i < fchs.ntaps; i++)
        fchs.taps[i] = (float)(fchs.taps[i] / sum);
    for (int c = 0; c < nch; c++) {
        fchs.freq[c] = freq[c];
        fchs.step_sc[c] = UINT64_MAX;
        fchs.dl[c] = calloc(2 * (size_t)fchs.ntaps, sizeof(float));
        fchs.h[c].x = malloc(sizeof(rdof_bl_chs_t) * FCHS_KEEP);
    }
    fchs.done = ring->w;
}

static double fchs_nco(int c, uint64_t sc)
{
    return fchs.freq[c] - fchs.center + (sc >= fchs.step_sc[c] ? fchs.step_hz[c] : 0);
}

/* The history written since the last call, through every channel, into each channel's */
static void fchs_run(void)
{
    int8_t in[2 * 4096];
    const uint64_t g = (uint64_t)(fchs.ntaps - 1) / 2;
    while (fchs.done < fchs.ring->w) {
        uint64_t oldest, next;
        long n = fchs.ring->w - fchs.done < 4096 ? (long)(fchs.ring->w - fchs.done) : 4096;
        n = rdof_bl_ring_read(fchs.ring, fchs.done, in, n, &oldest, &next);
        if (n <= 0) {
            fchs.done = oldest;
            continue;
        }
        for (long i = 0; i < n; i++) {
            const uint64_t sc = fchs.done + (uint64_t)i;
            const bool out = sc % (uint64_t)fchs.d == 0 && sc >= g;
            for (int c = 0; c < fchs.nch; c++) {
                fchs.ph[c] = fmod(fchs.ph[c] - 2 * M_PI * fchs_nco(c, sc) / fchs.fs, 2 * M_PI);
                const float xi = in[2 * i] / 127.0f, xq = in[2 * i + 1] / 127.0f;
                const float pr = (float)cos(fchs.ph[c]), pq = (float)sin(fchs.ph[c]);
                float *a = fchs.dl[c] + 2 * fchs.di;
                a[0] = xi * pr - xq * pq, a[1] = xi * pq + xq * pr;
                if (!out)
                    continue;
                float yi = 0, yq = 0;
                for (int k = 0; k < fchs.ntaps; k++) {
                    const float *x = fchs.dl[c] + 2 * ((fchs.di + 1 + k) % fchs.ntaps);
                    yi += fchs.taps[k] * x[0], yq += fchs.taps[k] * x[1];
                }
                const uint64_t center = sc - g;
                const bool filler = rdof_bl_ring_filled(fchs.ring, center, 1) > 0;
                if (fchs.h[c].n < FCHS_KEEP)
                    fchs.h[c].x[fchs.h[c].n++] =
                        (rdof_bl_chs_t){.sc = center, .i = filler ? 0 : yi, .q = filler ? 0 : yq,
                                        .nco_hz = (float)fchs_nco(c, center), .filler = filler};
            }
            if (++fchs.di == fchs.ntaps)
                fchs.di = 0;
        }
        fchs.done += (uint64_t)n;
    }
}

static int fchs_open(void *ctx, double hz, double seconds, double *bw)
{
    fchs_run();
    for (int c = 0; c < fchs.nch; c++) {
        const double off = fabs(hz - fchs.freq[c]);
        if (off > 3250)
            continue;
        *bw = 2 * (6250 - off);
        fchs.opened++;
        return c;
    }
    return -1;
}

static long fchs_read(void *ctx, int h, uint64_t sc, rdof_bl_chs_t *out, long n, uint64_t *oldest, uint64_t *next)
{
    fchs_run();
    const long kept = fchs.h[h].n;
    const rdof_bl_chs_t *x = fchs.h[h].x;
    *next = fchs.ring->w;
    *oldest = kept ? x[0].sc : fchs.ring->w;
    long i = 0, k = 0;
    while (i < kept && x[i].sc < sc)
        i++;
    for (; k < n && i < kept; k++, i++)
        out[k] = x[i];
    return k;
}

static void fchs_close(void *ctx, int h)
{
    fchs.closed++;
}

#endif
