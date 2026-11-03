// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * rdofradio's device on files (rdofradio_dev.h), for tests without a radio: --file RX[,TX]. RX is a recording (cf32,
 * or cu8 when its name ends in .cu8) at the run's rate, centered on the RX LO, played in real time, then silence.
 * TX (optional) gets what the radio transmits, cf32, centered on the TX LO, with the time it didn't transmit as
 * zeros, so its samples line up with the clock: another rdofradio can play it as its RX. Both run from the start.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rdofradio_dev.h"

typedef struct {
    rdofradio_dev_t d;
    FILE *rx, *tx;
    bool cu8;
    double t0;
    long long rx_pos, tx_pos; /* samples read, written (zeros included) */
} file_t;

static double mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void nap(double s)
{
    if (s > 0)
        nanosleep(&(struct timespec){(time_t)s, (long)((s - (time_t)s) * 1e9)}, NULL);
}

/* the samples due by now */
static long long due(const file_t *f) { return (long long)((mono() - f->t0) * f->d.rate); }

static int set_freq(rdofradio_dev_t *d, double hz) { return 0; }
static int set_tx_gain(rdofradio_dev_t *d, double db) { return 0; }
static double rx_gain(rdofradio_dev_t *d) { return 0; }
static double bandwidth(rdofradio_dev_t *d) { return d->rate; }

static int start(rdofradio_dev_t *d)
{
    ((file_t *)d)->t0 = mono();
    return 0;
}

static int rd(rdofradio_dev_t *d, rdof_cf_t *x, int n, long timeout_us)
{
    file_t *f = (file_t *)d;
    const double until = mono() + timeout_us * 1e-6;
    long long have;
    while ((have = due(f) - f->rx_pos) <= 0) {
        if (mono() >= until)
            return RDOF_DEV_TIMEOUT;
        nap(-have / d->rate + 0.001);
    }
    if (have < n)
        n = (int)have;
    int k = 0;
    if (f->rx && f->cu8) {
        uint8_t b[2 * 4096];
        while (k < n) {
            const int want = n - k < 4096 ? n - k : 4096;
            const int got = (int)fread(b, 2, (size_t)want, f->rx);
            for (int i = 0; i < got; i++) {
                x[k + i].re = (b[2 * i] - 127.5f) / 127.5f;
                x[k + i].im = (b[2 * i + 1] - 127.5f) / 127.5f;
            }
            k += got;
            if (got < want)
                break;
        }
    } else if (f->rx) {
        k = (int)fread(x, sizeof *x, (size_t)n, f->rx);
    }
    memset(x + k, 0, (size_t)(n - k) * sizeof *x); /* past the end: silence */
    f->rx_pos += n;
    return n;
}

static int wr(rdofradio_dev_t *d, const rdof_cf_t *x, int n, long timeout_us)
{
    file_t *f = (file_t *)d;
    /* no further than 50 ms ahead of the clock, as a device's buffers hold it */
    const long long ahead = (long long)(0.05 * d->rate);
    long long over;
    while ((over = f->tx_pos - due(f) - ahead) > 0)
        nap(over / d->rate);
    if (f->tx) {
        static const rdof_cf_t zero[1024];
        for (long long gap = due(f) - f->tx_pos - ahead; gap > 0;) { /* it was idle: zeros up to the clock */
            const int z = gap < 1024 ? (int)gap : 1024;
            fwrite(zero, sizeof *zero, (size_t)z, f->tx);
            f->tx_pos += z;
            gap -= z;
        }
        fwrite(x, sizeof *x, (size_t)n, f->tx);
        fflush(f->tx);
    }
    f->tx_pos += n;
    return n;
}

static void stop_tx(rdofradio_dev_t *d)
{
    file_t *f = (file_t *)d;
    if (f->tx)
        fflush(f->tx);
}

static void close_dev(rdofradio_dev_t *d)
{
    file_t *f = (file_t *)d;
    if (f->rx)
        fclose(f->rx);
    if (f->tx)
        fclose(f->tx);
    free(f);
}

rdofradio_dev_t *rdofradio_file_open(const char *spec, const rdofradio_dev_setup_t *s, char *err, size_t cap)
{
    char rx[512], *tx;
    snprintf(rx, sizeof rx, "%s", spec);
    tx = strchr(rx, ',');
    if (tx)
        *tx++ = 0;
    file_t *f = calloc(1, sizeof *f);
    if (!f) {
        snprintf(err, cap, "out of memory");
        return NULL;
    }
    rdofradio_dev_t *d = &f->d;
    snprintf(d->name, sizeof d->name, "file");
    d->rate = s->rate;
    d->tx_gain_min = 0, d->tx_gain_max = 89;
    d->set_rx_freq = set_freq;
    d->set_tx_freq = set_freq;
    d->set_tx_gain = set_tx_gain;
    d->rx_gain = rx_gain;
    d->bandwidth = bandwidth;
    d->start = start;
    d->read = rd;
    d->write = wr;
    d->stop_tx = stop_tx;
    d->close = close_dev;
    const size_t l = strlen(rx);
    f->cu8 = l > 4 && !strcmp(rx + l - 4, ".cu8");
    if (rx[0] && !(f->rx = fopen(rx, "rb"))) {
        snprintf(err, cap, "file %s: can't read it", rx);
        free(f);
        return NULL;
    }
    if (tx && tx[0] && !s->rx_only && !(f->tx = fopen(tx, "wb"))) {
        snprintf(err, cap, "file %s: can't write it", tx);
        close_dev(d);
        return NULL;
    }
    f->t0 = mono();
    return d;
}
