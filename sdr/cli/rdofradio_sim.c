// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * rdofradio's device for no-RF tests (rdofradio_dev.h): --sim-sdr, a receiver with nothing on the air: noise at the
 * asked rate, paced by the wall clock, the receive-only device beside an MMDVM modem or a split's radio. It lets
 * rdofradio's receiver, bench line and modem path run on a machine with no SDR (the host tests, with fakemodem).
 */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rdofradio_dev.h"

typedef struct {
    rdofradio_dev_t d;
    uint64_t written;
    struct timespec t0;
    uint32_t noise; /* xorshift */
} sim_t;

static int s_rx(rdofradio_dev_t *d, double hz)
{
    (void)d;
    (void)hz;
    return 0;
}
static double s_gain(rdofradio_dev_t *d)
{
    (void)d;
    return 20.0;
}
static double s_bw(rdofradio_dev_t *d) { return d->rate; }
static int s_start(rdofradio_dev_t *d)
{
    sim_t *s = (sim_t *)d;
    clock_gettime(CLOCK_MONOTONIC, &s->t0);
    s->written = 0;
    return 0;
}

static int s_read(rdofradio_dev_t *d, rdof_cf_t *x, int n, long timeout_us)
{
    (void)timeout_us;
    sim_t *s = (sim_t *)d;
    const uint64_t at = s->written + (uint64_t)n;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    const double el = (double)(now.tv_sec - s->t0.tv_sec) + 1e-9 * (double)(now.tv_nsec - s->t0.tv_nsec);
    const double due = (double)at / d->rate;
    if (due > el) {
        const double w = due - el;
        const struct timespec ts = {(time_t)w, (long)((w - (double)(time_t)w) * 1e9)};
        nanosleep(&ts, NULL);
    }
    uint32_t r = s->noise;
    for (int i = 0; i < n; i++) { /* quiet noise, a few cs8 counts */
        r ^= r << 13, r ^= r >> 17, r ^= r << 5;
        x[i].re = (float)((int)(r & 15) - 8) / 256.0f;
        r ^= r << 13, r ^= r >> 17, r ^= r << 5;
        x[i].im = (float)((int)(r & 15) - 8) / 256.0f;
    }
    s->noise = r ? r : 1u;
    s->written = at;
    return n;
}

static void s_close(rdofradio_dev_t *d) { free(d); }

rdofradio_dev_t *rdofradio_sim_open(const rdofradio_dev_setup_t *su, char *err, size_t cap)
{
    if (!su->rx_only) {
        snprintf(err, cap, "--sim-sdr only receives: with --mmdvm or --split-tx");
        return NULL;
    }
    sim_t *s = calloc(1, sizeof *s);
    if (!s) {
        snprintf(err, cap, "no memory");
        return NULL;
    }
    snprintf(s->d.name, sizeof s->d.name, "simulated SDR");
    s->d.rate = su->rate;
    s->d.tx_rate = su->rate;
    s->d.iq_scale = 1.0f;
    s->noise = 0x12345678u;
    s->d.set_rx_freq = s_rx;
    s->d.rx_gain = s_gain;
    s->d.bandwidth = s_bw;
    s->d.start = s_start;
    s->d.read = s_read;
    s->d.close = s_close;
    return &s->d;
}
