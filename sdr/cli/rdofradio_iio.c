// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * rdofradio's device on libiio 0.x (rdofradio_dev.h): an AD9361 or AD9363 (the PlutoSDR family and its clones),
 * through the Linux driver's IIO devices. URI: "local:" on the board, "ip:192.168.2.1" or "usb:" from a host.
 *
 *   ad9361-phy                 the chip: voltage0 in (RX gain, mode, bandwidth, rate), voltage0 out (TX
 *                              attenuation as a negative "hardwaregain"), altvoltage0 / 1 out (the RX / TX LO,
 *                              each with its "powerdown": SoapyPlutoSDR leaves both down when it closes)
 *   cf-ad9361-lpc              the receive DMA: voltage0 and voltage1 (I, Q), 12 bits, LSB aligned; with the
 *                              FPGA channelizer (words_rate), every scan element on, so a DMA sample is one 64-bit
 *                              word of its frames (a 2r2t design: voltage0..3)
 *   cf-ad9361-dds-core-lpc     the transmit DMA: voltage0 and voltage1, 16 bits, the DAC takes the top 12
 *
 * The rate: with libad9361 (RDOF_AD9361), ad9361_set_bb_rate, which loads the chip's own FIR for the rates under
 * 2.083 MS/s (down to 521 kS/s); without it, the chip's rate as is, which the driver refuses there. The FPGA's
 * decimation and interpolation (its own "sampling_frequency", 1/8 of the chip's) are kept off: one board's
 * decimator gave nothing but noise (docs/reference.md, "PlutoSDR").
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

#include <iio.h>
#ifdef RDOF_AD9361
#include <ad9361.h>
#endif

#include "rdofradio_dev.h"

#define TX_ATTEN_MAX 89.0 /* dB; TX gain 0 is this much attenuation, 89 none (as SoapyPlutoSDR) */
#define TX_BUFFERS 3      /* the transmit queue, in buffers (start) */

typedef struct {
    rdofradio_dev_t d;
    struct iio_context *ctx;
    struct iio_device *phy, *rxd, *txd;
    struct iio_channel *rx_ch, *tx_ch, *rx_lo, *tx_lo, *rx_i, *rx_q, *tx_i, *tx_q;
    struct iio_buffer *rxb, *txb;
    size_t len;          /* samples a DMA buffer */
    size_t rx_len;       /* a receive buffer's: len, or words for the FPGA channelizer */
    const int16_t *rp;   /* the receive buffer's next sample, r_left of them */
    size_t r_left;
    int16_t *wp;         /* the transmit buffer's next free sample, w_left of them */
    size_t w_left;
    bool rx_only;
} iio_t;

static void fail(rdofradio_dev_t *d, const char *what, int err)
{
    char e[100];
    iio_strerror(err < 0 ? -err : err, e, sizeof e);
    snprintf(d->error, sizeof d->error, "%s: %s", what, e);
}

static int put_ll(rdofradio_dev_t *d, struct iio_channel *c, const char *attr, long long v)
{
    const int r = iio_channel_attr_write_longlong(c, attr, v);
    if (r < 0) {
        char what[64];
        snprintf(what, sizeof what, "%s %lld", attr, v);
        fail(d, what, r);
    }
    return r < 0 ? -1 : 0;
}

static int set_rx_freq(rdofradio_dev_t *d, double hz)
{
    return put_ll(d, ((iio_t *)d)->rx_lo, "frequency", llround(hz));
}
static int set_tx_freq(rdofradio_dev_t *d, double hz)
{
    return put_ll(d, ((iio_t *)d)->tx_lo, "frequency", llround(hz));
}

static int set_tx_gain(rdofradio_dev_t *d, double db)
{
    const int r = iio_channel_attr_write_double(((iio_t *)d)->tx_ch, "hardwaregain", db - TX_ATTEN_MAX);
    if (r < 0)
        fail(d, "TX gain", r);
    return r < 0 ? -1 : 0;
}

static int set_rx_gain(rdofradio_dev_t *d, double db)
{
    iio_t *s = (iio_t *)d;
    int r = (int)iio_channel_attr_write(s->rx_ch, "gain_control_mode", isnan(db) ? "slow_attack" : "manual");
    if (r < 0 || (!isnan(db) && (r = iio_channel_attr_write_double(s->rx_ch, "hardwaregain", db)) < 0)) {
        fail(d, "RX gain", r);
        return -1;
    }
    return 0;
}

static double rx_gain(rdofradio_dev_t *d)
{
    double g = NAN;
    if (iio_channel_attr_read_double(((iio_t *)d)->rx_ch, "hardwaregain", &g) < 0)
        return NAN;
    return g;
}

static double bandwidth(rdofradio_dev_t *d)
{
    long long bw = 0;
    if (iio_channel_attr_read_longlong(((iio_t *)d)->rx_ch, "rf_bandwidth", &bw) < 0)
        return NAN;
    return (double)bw;
}

static int start(rdofradio_dev_t *d)
{
    iio_t *s = (iio_t *)d;
    /* 16 receive buffers (about 230 ms at 864 kS/s), not libiio's 4: the loop's slower turns (a key-up, the modem's
     * polls) are caught up, not lost (a PlutoSDR dropped about 0.5% of the samples while transmitting, 2026-10-29).
     * The transmit side's 3 are the one being filled and two queued (28.4 ms at 864 kS/s; a push takes the next
     * free one, so with 4 the queue was 42.7 ms): less than the 50 ms the stream runs ahead, so its pushes wait for
     * a block to play out, the guard's clock (rdof/sdr/txguard.h). */
    iio_device_set_kernel_buffers_count(s->rxd, 16);
    s->rxb = iio_device_create_buffer(s->rxd, s->rx_len, false);
    if (!s->rxb) {
        fail(d, "receive buffer", errno);
        return -1;
    }
    if (s->rx_only)
        return 0;
    iio_device_set_kernel_buffers_count(s->txd, TX_BUFFERS);
    s->txb = iio_device_create_buffer(s->txd, s->len, false);
    if (!s->txb) {
        fail(d, "transmit buffer", errno);
        return -1;
    }
    s->wp = iio_buffer_start(s->txb);
    s->w_left = s->len;
    return 0;
}

static int rd(rdofradio_dev_t *d, rdof_cf_t *x, int n, long timeout_us)
{
    iio_t *s = (iio_t *)d;
    if (!s->r_left) {
        const ssize_t r = iio_buffer_refill(s->rxb);
        if (r < 0) {
            if (r == -ETIMEDOUT || r == -EAGAIN)
                return RDOF_DEV_TIMEOUT;
            fail(d, "read", (int)r);
            return RDOF_DEV_ERROR;
        }
        s->rp = iio_buffer_start(s->rxb);
        s->r_left = (size_t)((const char *)iio_buffer_end(s->rxb) - (const char *)s->rp) / (2 * sizeof(int16_t));
    }
    const int k = (size_t)n < s->r_left ? n : (int)s->r_left;
    const float scale = 1.0f / 2048.0f;
    /* one wide copy out of the DMA buffer, then the conversion: on a PlutoSDR, 16-bit loads straight from the buffer
     * took a fifth of a core, this a twentieth */
    enum { STAGE = 8192 };
    static int16_t stage[2 * STAGE];
    for (int done = 0; done < k;) {
        const int m = k - done < STAGE ? k - done : STAGE;
        memcpy(stage, s->rp + 2 * done, (size_t)m * 2 * sizeof(int16_t));
        for (int i = 0; i < m; i++) {
            x[done + i].re = (float)stage[2 * i] * scale;
            x[done + i].im = (float)stage[2 * i + 1] * scale;
        }
        done += m;
    }
    s->rp += 2 * k;
    s->r_left -= (size_t)k;
    return k;
}

/* the FPGA channelizer's frames: a DMA buffer's words as they are */
static int rd_words(rdofradio_dev_t *d, const uint64_t **w, long timeout_us)
{
    iio_t *s = (iio_t *)d;
    const ssize_t r = iio_buffer_refill(s->rxb);
    if (r < 0) {
        if (r == -ETIMEDOUT || r == -EAGAIN)
            return RDOF_DEV_TIMEOUT;
        fail(d, "read", (int)r);
        return RDOF_DEV_ERROR;
    }
    *w = iio_buffer_start(s->rxb);
    return (int)((size_t)((const char *)iio_buffer_end(s->rxb) - (const char *)*w) / sizeof(uint64_t));
}

#define RAW_GUARD 8192 /* samples left off the front of a raw capture: what the FIFOs held from before */

static void rx_pause(rdofradio_dev_t *d)
{
    iio_t *s = (iio_t *)d;
    if (s->rxb)
        iio_buffer_destroy(s->rxb);
    s->rxb = NULL;
}

#define RAW_BLOCK (8L << 20) /* samples a DMA block at most (32 MB): one block stops at 64 MB */
#define ADC_DMA_STATUS 0x80000088u /* the ADC core's DMA status (debugfs register access): bit 2 overflow, write 1 to clear */

/* DMA blocks all queued before the first sample, so the DMA fills them one after the other with nothing between: its
 * samples are in a row (the ADC core's overflow flag says if not). With only the first two channels on, a sample is 4
 * bytes (cpack sits ahead of the block's mux, so this is the stock path as on a stock board). */
static int raw_fill(rdofradio_dev_t *d, long n, int16_t *out, long *after, char *err, size_t cap)
{
    iio_t *s = (iio_t *)d;
    for (unsigned i = 0; i < iio_device_get_channels_count(s->rxd); i++) {
        struct iio_channel *c = iio_device_get_channel(s->rxd, i);
        if (iio_channel_is_scan_element(c))
            iio_channel_disable(c);
    }
    iio_channel_enable(s->rx_i);
    iio_channel_enable(s->rx_q);
    const long all = n + RAW_GUARD, blocks = (all + RAW_BLOCK - 1) / RAW_BLOCK, len = ((all + blocks - 1) / blocks + 1023) & ~1023L; /* whole DMA bursts */
    iio_device_set_kernel_buffers_count(s->rxd, (unsigned)blocks);
    s->rxb = iio_device_create_buffer(s->rxd, (size_t)len, false);
    if (!s->rxb) {
        char e[100];
        iio_strerror(errno, e, sizeof e);
        snprintf(err, cap, "%ld DMA blocks of %.0f MB: %s", blocks, (double)len * 4 / 1e6, e);
        return -1;
    }
    const bool status = iio_device_reg_write(s->rxd, ADC_DMA_STATUS, 4) == 0; /* clear it */
    iio_context_set_timeout(s->ctx, (unsigned)((double)len / d->rate * 1000) + 5000);
    long got = 0, skip = RAW_GUARD; /* the first samples: what the FIFOs held from before */
    int rc = 0;
    for (long b = 0; b < blocks && got < n; b++) {
        const ssize_t r = iio_buffer_refill(s->rxb);
        if (r < (ssize_t)len * 4) {
            snprintf(err, cap, "DMA block %ld of %ld gave %zd bytes of %ld", b + 1, blocks, r, len * 4);
            rc = -1;
            break;
        }
        long k = len - skip;
        if (k > n - got)
            k = n - got;
        memcpy(out + 2 * got, (const int16_t *)iio_buffer_start(s->rxb) + 2 * skip, (size_t)k * 4);
        got += k, skip = 0;
        *after = len - k; /* in the last block, past those kept */
    }
    iio_context_set_timeout(s->ctx, 5000);
    uint32_t st = 0;
    if (!rc && status && iio_device_reg_read(s->rxd, ADC_DMA_STATUS, &st) == 0 && (st & 4)) {
        snprintf(err, cap, "the DMA overflowed: samples are missing between its blocks");
        rc = -1;
    }
    rx_pause(d);
    return rc;
}

static int rx_resume(rdofradio_dev_t *d)
{
    iio_t *s = (iio_t *)d;
    for (unsigned i = 0; i < iio_device_get_channels_count(s->rxd); i++) {
        struct iio_channel *c = iio_device_get_channel(s->rxd, i);
        if (iio_channel_is_scan_element(c))
            iio_channel_enable(c);
    }
    iio_device_set_kernel_buffers_count(s->rxd, 16);
    s->rxb = iio_device_create_buffer(s->rxd, s->rx_len, false);
    if (!s->rxb) {
        fail(d, "receive buffer", errno);
        return -1;
    }
    return 0;
}

static double mono_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

static int tx_room(rdofradio_dev_t *d) { return ((iio_t *)d)->txb ? (int)((iio_t *)d)->w_left : 0; }

static int16_t dac(float v)
{
    const float y = v * 32767.0f;
    return (int16_t)(y > 32767.0f ? 32767 : y < -32767.0f ? -32767 : lrintf(y));
}

static int wr(rdofradio_dev_t *d, const rdof_cf_t *x, int n, long timeout_us)
{
    iio_t *s = (iio_t *)d;
    if (!s->txb)
        return n;
    const int k = (size_t)n < s->w_left ? n : (int)s->w_left;
    for (int i = 0; i < k; i++) {
        s->wp[2 * i] = dac(x[i].re);
        s->wp[2 * i + 1] = dac(x[i].im);
    }
    s->wp += 2 * k;
    s->w_left -= (size_t)k;
    if (!s->w_left) {
        const double t0 = mono_s();
        const ssize_t r = iio_buffer_push(s->txb); /* blocks while the driver's buffers are all queued */
        if (d->txguard && r >= 0)
            rdof_txguard_pushed(d->txguard, t0, mono_s());
        s->wp = iio_buffer_start(s->txb);
        s->w_left = s->len;
        if (r < 0) {
            fail(d, "write", (int)r);
            return RDOF_DEV_ERROR;
        }
    }
    return k;
}

/* an LO on or off; failures don't matter on the way down */
static int lo_power(rdofradio_dev_t *d, struct iio_channel *lo, bool on)
{
    const int r = iio_channel_attr_write_bool(lo, "powerdown", !on);
    if (r < 0)
        fail(d, on ? "LO power up" : "LO power down", r);
    return r < 0 ? -1 : 0;
}

static void stop_tx(rdofradio_dev_t *d)
{
    iio_t *s = (iio_t *)d;
    if (s->txb) {
        iio_buffer_destroy(s->txb);
        s->txb = NULL;
    }
    if (s->tx_lo)
        lo_power(d, s->tx_lo, false); /* no LO leakage once it's stopped */
}

static void close_dev(rdofradio_dev_t *d)
{
    iio_t *s = (iio_t *)d;
    stop_tx(d);
    if (s->rxb)
        iio_buffer_destroy(s->rxb);
    if (s->rx_lo)
        lo_power(d, s->rx_lo, false);
    if (s->ctx)
        iio_context_destroy(s->ctx);
    free(s);
}

/* a device's own rate as the chip's (no FPGA decimation), when it has the attribute */
static void fpga_rate(struct iio_device *dev, bool output, long long rate)
{
    struct iio_channel *c = iio_device_find_channel(dev, "voltage0", output);
    if (c && iio_channel_attr_write_longlong(c, "sampling_frequency", rate) < 0) {
        /* boards without the FPGA decimator have no such attribute: nothing to turn off */
    }
}

/* "bufflen=N" in --stream */
static size_t stream_len(const char *stream, double rate)
{
    const char *p = stream ? strstr(stream, "bufflen=") : NULL;
    if (p)
        return (size_t)strtoul(p + 8, NULL, 10);
    /* 13.7 ms, as 32768 samples at 2.4 MS/s (rdofradio_soapy.c): the loop delay steady, the transmitter fed */
    size_t n = (size_t)(rate * 32768.0 / 2400000.0);
    n = (n + 4095) / 4096 * 4096;
    return n < 4096 ? 4096 : n;
}

rdofradio_dev_t *rdofradio_iio_open(const char *uri, const rdofradio_dev_setup_t *su, char *err, size_t cap)
{
    iio_t *s = calloc(1, sizeof *s);
    if (!s) {
        snprintf(err, cap, "out of memory");
        return NULL;
    }
    rdofradio_dev_t *d = &s->d;
    d->set_rx_freq = set_rx_freq;
    d->set_tx_freq = set_tx_freq;
    d->set_tx_gain = set_tx_gain;
    d->rx_gain = rx_gain;
    d->set_rx_gain = set_rx_gain;
    d->bandwidth = bandwidth;
    d->start = start;
    d->read = rd;
    d->write = wr;
    d->stop_tx = stop_tx;
    d->close = close_dev;
    d->tx_gain_min = 0, d->tx_gain_max = TX_ATTEN_MAX;
    d->iq_scale = 16; /* a 12-bit ADC at 1/2048 a step */
    snprintf(d->name, sizeof d->name, "libiio AD9361");
    s->ctx = uri && *uri ? iio_create_context_from_uri(uri) : iio_create_default_context();
    if (!s->ctx) {
        char e[100];
        iio_strerror(errno, e, sizeof e);
        snprintf(err, cap, "iio %s: %s", uri && *uri ? uri : "(default)", e);
        free(s);
        return NULL;
    }
    s->phy = iio_context_find_device(s->ctx, "ad9361-phy");
    s->rxd = iio_context_find_device(s->ctx, "cf-ad9361-lpc");
    s->txd = iio_context_find_device(s->ctx, "cf-ad9361-dds-core-lpc");
    if (s->phy) {
        s->rx_ch = iio_device_find_channel(s->phy, "voltage0", false);
        s->tx_ch = iio_device_find_channel(s->phy, "voltage0", true);
        s->rx_lo = iio_device_find_channel(s->phy, "altvoltage0", true);
        s->tx_lo = iio_device_find_channel(s->phy, "altvoltage1", true);
    }
    if (s->rxd) {
        s->rx_i = iio_device_find_channel(s->rxd, "voltage0", false);
        s->rx_q = iio_device_find_channel(s->rxd, "voltage1", false);
    }
    if (s->txd) {
        s->tx_i = iio_device_find_channel(s->txd, "voltage0", true);
        s->tx_q = iio_device_find_channel(s->txd, "voltage1", true);
    }
    if (!s->rx_ch || !s->tx_ch || !s->rx_lo || !s->tx_lo || !s->rx_i || !s->rx_q || !s->tx_i || !s->tx_q) {
        snprintf(err, cap, "iio %s: no AD9361 there (ad9361-phy, cf-ad9361-lpc, cf-ad9361-dds-core-lpc)",
                 uri && *uri ? uri : "(default)");
        close_dev(d);
        return NULL;
    }
    if (set_tx_gain(d, d->tx_gain_min)) {
        snprintf(err, cap, "iio: %s", d->error);
        close_dev(d);
        return NULL;
    }
    const long long rate = llround(su->rate);
    int r;
#ifdef RDOF_AD9361
    r = ad9361_set_bb_rate(s->phy, (unsigned long)rate);
    if (r < 0)
        fail(d, "rate", r);
#else
    r = put_ll(d, s->rx_ch, "sampling_frequency", rate);
    if (rate < 2083334)
        snprintf(d->error + strlen(d->error), sizeof d->error - strlen(d->error),
                 " (under 2.083 MS/s needs libad9361)");
#endif
    fpga_rate(s->rxd, false, rate);
    fpga_rate(s->txd, true, rate);
    if (r < 0 || put_ll(d, s->rx_lo, "frequency", llround(su->rx_hz)) ||
        (!su->rx_only && put_ll(d, s->tx_lo, "frequency", llround(su->tx_hz))) || lo_power(d, s->rx_lo, true) ||
        lo_power(d, s->tx_lo, !su->rx_only) ||
        (su->bw > 0 && (put_ll(d, s->rx_ch, "rf_bandwidth", llround(su->bw)) ||
                        put_ll(d, s->tx_ch, "rf_bandwidth", llround(su->bw))))) {
        snprintf(err, cap, "iio setup: %s", d->error);
        close_dev(d);
        return NULL;
    }
    /* No --bw: the analog filters as wide as the rate. The AD9361 keeps the last bandwidth written, by anything, so a
     * 4 MHz left by another program at 19.872 MS/s put the channels 8 MHz off center under the noise's last bit (their
     * spectrum all zeros). The driver clamps to its range; a refusal only warns. */
    if (!(su->bw > 0) && (put_ll(d, s->rx_ch, "rf_bandwidth", rate) || put_ll(d, s->tx_ch, "rf_bandwidth", rate)))
        fprintf(stderr, "iio: the analog bandwidth stays as it was: %s\n", d->error);
    const char *mode = isnan(su->gain) ? "slow_attack" : "manual";
    r = (int)iio_channel_attr_write(s->rx_ch, "gain_control_mode", mode);
    if (r < 0 || (!isnan(su->gain) && (r = iio_channel_attr_write_double(s->rx_ch, "hardwaregain", su->gain)) < 0)) {
        fail(d, "RX gain", r);
        snprintf(err, cap, "iio setup: %s", d->error);
        close_dev(d);
        return NULL;
    }
    long long got = 0;
    if (iio_channel_attr_read_longlong(s->rx_ch, "sampling_frequency", &got) < 0)
        got = rate;
    d->rate = (double)got;
    if (su->words_rate > 0) {
        /* every scan element on: one DMA sample is then a whole 64-bit frame word */
        for (unsigned i = 0; i < iio_device_get_channels_count(s->rxd); i++) {
            struct iio_channel *c = iio_device_get_channel(s->rxd, i);
            if (iio_channel_is_scan_element(c))
                iio_channel_enable(c);
        }
        const ssize_t size = iio_device_get_sample_size(s->rxd);
        if (size != (ssize_t)sizeof(uint64_t)) {
            snprintf(err, cap, "iio: the receive DMA's samples are %zd bytes, not 8: not the FPGA channelizer's "
                               "design (two receive channels)", size);
            close_dev(d);
            return NULL;
        }
        d->read_words = rd_words;
        d->rx_pause = rx_pause, d->raw_fill = raw_fill, d->rx_resume = rx_resume;
    } else {
        iio_channel_enable(s->rx_i);
        iio_channel_enable(s->rx_q);
    }
    s->rx_only = su->rx_only;
    if (!s->rx_only) {
        iio_channel_enable(s->tx_i);
        iio_channel_enable(s->tx_q);
    }
    s->len = s->rx_len = stream_len(su->stream, d->rate);
    d->tx_rate = d->rate;
    if (su->tx_interp > 1) {
        /* the transmit stream at rate / tx_interp, in buffers of about 5 ms: the queue (two), the one being filled,
         * a loop turn (10 ms) and the guard's margin stay inside the 50 ms the stream runs ahead. 16 ms buffers
         * (1536 at 96 kS/s) needed 64 ms, and once a stall made the guard pad, its dropping the late samples left
         * every block after short too: zeros for a whole key-up. */
        d->tx_rate = d->rate / su->tx_interp;
        const size_t n = (size_t)(d->tx_rate * 0.005);
        s->len = n < 256 ? 256 : (n + 255) / 256 * 256;
    }
    if (su->words_rate > 0) {
        /* 10 ms of the channelizer's words a receive buffer */
        const size_t n = (size_t)(su->words_rate / 100);
        s->rx_len = n < 512 ? 512 : (n + 63) / 64 * 64;
    }
    if (!s->rx_only)
        d->tx_block = (int)s->len, d->tx_nbuf = TX_BUFFERS - 1, d->tx_room = tx_room; /* the one being filled */
    return d;
}
