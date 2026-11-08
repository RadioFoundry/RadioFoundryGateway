// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * rdofradio's device on SoapySDR (rdofradio_dev.h): any SoapySDR device that transmits, CF32 both ways; receive
 * only, any that receives.
 */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <SoapySDR/Device.h>
#include <SoapySDR/Formats.h>

#include "rdof/benchline.h"
#include "rdofradio_dev.h"

typedef struct {
    rdofradio_dev_t d;
    SoapySDRDevice *dev;
    SoapySDRStream *rx, *tx;
} soapy_t;

static void fail(rdofradio_dev_t *d, const char *what)
{
    snprintf(d->error, sizeof d->error, "%s: %s", what, SoapySDRDevice_lastError());
}

static int set_rx_freq(rdofradio_dev_t *d, double hz)
{
    if (SoapySDRDevice_setFrequency(((soapy_t *)d)->dev, SOAPY_SDR_RX, 0, hz, NULL)) {
        fail(d, "RX frequency");
        return -1;
    }
    return 0;
}

static int set_tx_freq(rdofradio_dev_t *d, double hz)
{
    if (SoapySDRDevice_setFrequency(((soapy_t *)d)->dev, SOAPY_SDR_TX, 0, hz, NULL)) {
        fail(d, "TX frequency");
        return -1;
    }
    return 0;
}

static int set_tx_gain(rdofradio_dev_t *d, double db)
{
    if (SoapySDRDevice_setGain(((soapy_t *)d)->dev, SOAPY_SDR_TX, 0, db)) {
        fail(d, "TX gain");
        return -1;
    }
    return 0;
}

static double rx_gain(rdofradio_dev_t *d) { return SoapySDRDevice_getGain(((soapy_t *)d)->dev, SOAPY_SDR_RX, 0); }

static double bandwidth(rdofradio_dev_t *d) { return SoapySDRDevice_getBandwidth(((soapy_t *)d)->dev, SOAPY_SDR_RX, 0); }

static int start(rdofradio_dev_t *d)
{
    soapy_t *s = (soapy_t *)d;
    if (SoapySDRDevice_activateStream(s->dev, s->rx, 0, 0, 0) ||
        (s->tx && SoapySDRDevice_activateStream(s->dev, s->tx, 0, 0, 0))) {
        fail(d, "activate");
        return -1;
    }
    return 0;
}

static int rd(rdofradio_dev_t *d, rdof_cf_t *x, int n, long timeout_us)
{
    soapy_t *s = (soapy_t *)d;
    void *b[1] = {x};
    int flags = 0;
    long long t_ns = 0;
    const int k = SoapySDRDevice_readStream(s->dev, s->rx, b, (size_t)n, &flags, &t_ns, timeout_us);
    if (k >= 0)
        return k;
    if (k == SOAPY_SDR_TIMEOUT)
        return RDOF_DEV_TIMEOUT;
    if (k == SOAPY_SDR_OVERFLOW)
        return RDOF_DEV_OVERFLOW;
    snprintf(d->error, sizeof d->error, "read: %s", SoapySDR_errToStr(k));
    return RDOF_DEV_ERROR;
}

static int wr(rdofradio_dev_t *d, const rdof_cf_t *x, int n, long timeout_us)
{
    soapy_t *s = (soapy_t *)d;
    if (!s->tx)
        return n;
    const void *b[1] = {x};
    int flags = 0;
    const int k = SoapySDRDevice_writeStream(s->dev, s->tx, b, (size_t)n, &flags, 0, timeout_us);
    return k > 0 ? k : k == SOAPY_SDR_TIMEOUT || k == 0 ? RDOF_DEV_TIMEOUT : RDOF_DEV_ERROR;
}

static void stop_tx(rdofradio_dev_t *d)
{
    soapy_t *s = (soapy_t *)d;
    if (s->tx) {
        SoapySDRDevice_deactivateStream(s->dev, s->tx, 0, 0);
        SoapySDRDevice_closeStream(s->dev, s->tx);
        s->tx = NULL;
    }
}

static void close_dev(rdofradio_dev_t *d)
{
    soapy_t *s = (soapy_t *)d;
    stop_tx(d);
    if (s->rx) {
        SoapySDRDevice_deactivateStream(s->dev, s->rx, 0, 0);
        SoapySDRDevice_closeStream(s->dev, s->rx);
    }
    SoapySDRDevice_unmake(s->dev);
    free(s);
}

rdofradio_dev_t *rdofradio_soapy_open(const char *args, const rdofradio_dev_setup_t *su, char *err, size_t cap)
{
    soapy_t *s = calloc(1, sizeof *s);
    if (!s) {
        snprintf(err, cap, "out of memory");
        return NULL;
    }
    rdofradio_dev_t *d = &s->d;
    d->set_rx_freq = set_rx_freq;
    d->set_tx_freq = set_tx_freq;
    d->set_tx_gain = set_tx_gain;
    d->rx_gain = rx_gain;
    d->bandwidth = bandwidth;
    d->start = start;
    d->read = rd;
    d->write = wr;
    d->stop_tx = stop_tx;
    d->close = close_dev;
    SoapySDRKwargs kw = SoapySDRKwargs_fromString(args);
    s->dev = SoapySDRDevice_make(&kw);
    SoapySDRKwargs_clear(&kw);
    if (!s->dev) {
        snprintf(err, cap, "soapy %s: %s", args, SoapySDRDevice_lastError());
        free(s);
        return NULL;
    }
    char *driver = SoapySDRDevice_getDriverKey(s->dev);
    snprintf(d->name, sizeof d->name, "SoapySDR %s", driver ? driver : "");
    const bool pluto = driver && !strcasecmp(driver, "plutosdr");
    free(driver);
    const bool tx = !su->rx_only;
    if (tx) {
        const SoapySDRRange tr = SoapySDRDevice_getGainRange(s->dev, SOAPY_SDR_TX, 0);
        d->tx_gain_min = tr.minimum, d->tx_gain_max = tr.maximum;
        set_tx_gain(d, d->tx_gain_min);
    }
    if (SoapySDRDevice_setSampleRate(s->dev, SOAPY_SDR_RX, 0, su->rate) ||
        (tx && SoapySDRDevice_setSampleRate(s->dev, SOAPY_SDR_TX, 0, su->rate)) ||
        SoapySDRDevice_setFrequency(s->dev, SOAPY_SDR_RX, 0, su->rx_hz, NULL) ||
        (tx && SoapySDRDevice_setFrequency(s->dev, SOAPY_SDR_TX, 0, su->tx_hz, NULL)) ||
        SoapySDRDevice_setGainMode(s->dev, SOAPY_SDR_RX, 0, isnan(su->gain)) ||
        (!isnan(su->gain) && SoapySDRDevice_setGain(s->dev, SOAPY_SDR_RX, 0, su->gain)) ||
        (su->bw > 0 && (SoapySDRDevice_setBandwidth(s->dev, SOAPY_SDR_RX, 0, su->bw) ||
                        (tx && SoapySDRDevice_setBandwidth(s->dev, SOAPY_SDR_TX, 0, su->bw))))) {
        snprintf(err, cap, "soapy setup: %s", SoapySDRDevice_lastError());
        SoapySDRDevice_unmake(s->dev);
        free(s);
        return NULL;
    }
    d->rate = SoapySDRDevice_getSampleRate(s->dev, SOAPY_SDR_RX, 0);
    d->tx_rate = d->rate;
    double native_scale = 0;
    free(SoapySDRDevice_getNativeStreamFormat(s->dev, SOAPY_SDR_RX, 0, &native_scale));
    d->iq_scale = rdof_bl_ring_scale(native_scale);
    /* the streams' own arguments (--stream), for both. A PlutoSDR's own 65536-sample buffers made the loop delay
     * vary by a buffer (27 ms at 2.4 MS/s) from run to run; 16384 underran its transmitter without a word (bit
     * errors in every burst); 32768 holds the delay within a few ms and sends clean */
    const char *stream = su->stream && *su->stream ? su->stream : pluto ? "bufflen=32768" : "";
    SoapySDRKwargs sa = SoapySDRKwargs_fromString(stream);
    s->rx = SoapySDRDevice_setupStream(s->dev, SOAPY_SDR_RX, SOAPY_SDR_CF32, NULL, 0, &sa);
    if (tx)
        s->tx = SoapySDRDevice_setupStream(s->dev, SOAPY_SDR_TX, SOAPY_SDR_CF32, NULL, 0, &sa);
    SoapySDRKwargs_clear(&sa);
    if (!s->rx || (tx && !s->tx)) {
        snprintf(err, cap, "soapy streams: %s", SoapySDRDevice_lastError());
        close_dev(d);
        return NULL;
    }
    return d;
}
