// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * rdofradio: an SDR that receives and transmits, run as the gateway's sdr_transceiver on a host or on the SDR
 * itself (a PlutoSDR's own ARM, through libiio: rdofradio_dev.h). The
 * same parts as on a board (docs/reference.md, "Transmit: the SDR as a radio"): the receiver hears the RX
 * channel, the SDR modem (rdof/sdr/sdrmodem.h) takes what it hears and modulates what it's given, and the
 * repeater (full duplex) or the roaming transceiver (half duplex) drives the modem through an rdof_modem client,
 * as it would an MMDVM_HS. With an hblink section in the settings (--config) it's an HBlink4 peer as the board's
 * (rdofradio_net.h, common/src/hbpeer.c): what it hears goes up as DMRD, and the master's calls go out; without
 * one there's no network, and the repeater repeats what it hears on its outbound (repeater.md 4), the transceiver
 * only listens. --parrot ID sends back what's heard once it ends (rdof/parrot.h), as a network call would come.
 *
 * With an mqtt section it publishes as the board does (rdofradio_mqtt.h): the radio's calls, bursts, what it
 * transmits and its status on the repeater's or roamer's topics, the monitored channels' on the SDR's.
 *
 * The clock (the settings' ntp): netclock, the board's NTP disciplined clock (components/netclock, its POSIX glue),
 * or the OS's (ntp.clock "os": chrony, ntpd). --clock-state FILE keeps netclock's learned rate across restarts;
 * --set-os-clock steps the OS clock to it (root; for a box with nothing else setting the time, a PlutoSDR).
 *
 *   rdofradio DEVICE --allow-tx --rx MHZ [--tx MHZ] [--duplex full|half] [--cc CC] [-r RATE] [--bw MHZ]
 *             [--tx-gain DB] [--tx-lo MHZ] [--gain DB] [--center MHZ] [--monitor] [--tx-offset S]
 *             [--parrot ID] [--stream ARGS] [--seconds S] [--bursts] [--quiet] [-v] [--web [ADDR:]PORT]
 *             [--last-exit FILE] [--fft builtin|pffft] [--bench [ADDR:]PORT] [--bench-tx MHZ,...]
 *   rdofradio DEVICE --allow-tx --config FILE [--clock-state FILE] [--set-os-clock] [the device's flags as above]
 *   rdofradio DEVICE --allow-tx --split-tx PORT [--baud N] [--split-power-cap N] --rx MHZ --tx MHZ [--cc CC] [...]
 *
 * DEVICE is --soapy ARGS (SoapySDR; -r defaults to 2.4 MS/s) or --iio URI (an AD9361 through libiio: "local:" on a
 * PlutoSDR, "ip:192.168.2.1" from a host; -r defaults to 768 kS/s, which the chip's own FIR makes with
 * libad9361). --web serves the page on 127.0.0.1 unless ADDR is given (0.0.0.0: every interface). --fft: the
 * channelizer's FFT, pffft when it's built in (RDOF_PFFFT). --last-exit FILE: where a supervisor records how the
 * last run ended (rdof_hostweb_last_exit; the Pluto bundle's run.sh): the page's crash card. --bench serves the
 * bench's line protocol (docs/spec/bench-api.md section 14) on PORT (7373 by convention), every interface unless
 * ADDR is given: the receiver's bursts and wideband captures for tools/bench/rfgbench.py, and its transmit jobs
 * (bench_tx's voice, csbk, data) on the --bench-tx frequencies, at --tx-gain, in the transceiver's place while
 * they run (not in a duplex repeater).
 *
 * --split-tx PORT: a split duplex repeater (docs/spec/repeater.md): a RadioFoundry radio on PORT (its hotspot, with
 * the split duplex extension) transmits, and the SDR, opened receive only, is the RX half. Any receiver does, an
 * RTL-SDR included. rdof_split_t joins the two into one duplex modem for the repeater, as split_port.c does on a
 * board. The SDR modem's clock for the split's TIME is the receive stream's sample count against the wall clock,
 * at the freshest read; samples the device drops are made up with zeros, so the count keeps real time.
 * --split-power-cap is the radio's RF level cap (MMDVM scale, 1-255; the radio's default is 64, about 25 %).
 *
 * --config FILE takes the radio from the firmware's settings document (rdofradio_cfg.h) in place of --rx, --tx,
 * --duplex, --cc and --parrot; the device stays on the command line. With --web, the page edits the file: a save
 * stops the loop, closes the device (the TX gain to its least first) and starts rdofradio again with the same
 * arguments.
 *
 * Each turn reads a chunk of the receive stream, polls the repeater, pulls as many samples from the modulator into
 * a ring, then runs the chunk through the receiver; a second thread writes the ring to the transmit stream. A
 * device's read and write both block on USB transfers, and one thread doing both fell behind (the PlutoSDR,
 * whose driver drops what isn't read in time without a word). The device's RX and
 * TX run on one clock (one chip), so the two streams keep their distance; the transmit stream starts a fixed lead
 * ahead. Transmit sample j is the modem's position j - lead + tx_offset: --tx-offset (seconds) is the device's
 * loop delay (TX written to RX read), which --monitor measures by hearing the outbound back.
 *
 * The transmitter's LO sits at --tx-lo (default 100 kHz above --tx) and the modulator puts the carrier at the
 * offset, so the LO leakage and the I/Q image are off the channel. Unkeyed, the TX gain is the device's least
 * (the most attenuation); it goes to --tx-gain while anything keyed is in the stream. Nothing transmits without
 * --allow-tx.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rdof/benchline.h"
#include "rdof/benchnet.h"
#include "rdof/benchtx.h"
#include "rdof/hbpeer.h"
#include "rdof/layer2.h"
#include "rdof/modem.h"
#include "rdof/parrot.h"
#include "rdof/repeater.h"
#include "rdof/siteroute.h"
#include "rdof/split.h"
#include "rdof/sdr/modes.h"
#include "rdof/sdr/receiver.h"
#include "rdof/sdr/sdrmodem.h"
#include "rdof/sdr/spectrum.h"
#include "rdof/txvr.h"
#include "rdofradio_cfg.h"
#include "rdofradio_dev.h"
#include "rdofradio_net.h"
#include "rdofradio_serial.h"
#ifdef RDOF_PFFFT
#include "rdof_pffft.h"
#endif
#ifdef RDOF_HOST_WEB
#include "rdof/hostweb.h"
#endif
#ifdef RDOF_NETCLOCK
#include "netclock_posix.h"
#endif
#ifdef RDOF_MQTT
#include "rdof/netcalls.h"
#include "rdof/rawburst.h"
#include "rdof/sdr/status.h"
#include "rdof/status.h"
#include "rdofradio_mqtt.h"
#endif
#ifndef RDOF_VERSION
#define RDOF_VERSION "dev"
#endif

#define CHUNK 24000        /* receive samples a turn: 10 ms at 2.4 MS/s */
#define POLL_S 0.02        /* the repeater's poll */
#define FRESH_S 0.0003     /* a read that waited this long found the device's buffers empty: its samples are now */
#define RING (1 << 21)     /* transmit samples between the loop and the writer: 0.87 s at 2.4 MS/s */

#ifndef M_PI
#define M_PI 3.14159265358979323846 /* not in <math.h> under _POSIX_C_SOURCE alone (glibc) */
#endif

typedef struct {
    bool full;
    bool split;            /* --split-tx: a radio transmits, the SDR is the RX half */
    bool keyed;            /* the repeater's, from on_key */
    bool verbose;
    double rate, center, tx_lo;
    uint32_t rx_hz, tx_hz;
    int cc;
    rdof_sdrmodem_t m;
    rdof_modem_t client;
    rdof_radio_t radio;
    rdof_repeater_t r;
    rdof_txvr_t t;
    rdof_sdr_rx_t rx;
    rdof_parrot_t *parrot; /* --parrot, NULL none */
    int mon;               /* the monitor channel, -1 none */
    /* the monitor's timing: at each key-up, where the receiver should report the first burst (48 kS/s), against
     * where the monitor channel heard it */
    int64_t first_sent;
    bool waiting;          /* keyed, the first burst not heard back yet */
    /* the same from the receive level: our own key-up leaking into the receiver (a sample-exact edge) */
    int64_t onset;         /* the key-up's first keyed sample, the modem's position; -1 none pending */
    int cal;               /* duplex calibration: 1 the pulse is due, 2 it's out, 0 done or none */
    long long cal_at;      /* the pulse's stream position (cal 1) */
    bool recal;            /* calibrate again once idle: samples were lost, or the pulse wasn't heard */
    int cal_tries;
    double lag_min, lag_prev; /* the stream behind the wall clock (samples): this window's floor, the last one's */
    double lag_win;        /* stream time the lag's window started */
    bool lag_fresh;        /* a read in the lag's window waited (FRESH_S): the loop had caught up */
    int64_t cal48;         /* the calibrated tx_late48, with the ring at ring_ref samples (-1: not yet taken) */
    int64_t ring_ref, ring_min;
    double ring_win;       /* stream time the ring's current window started */
    double base_power;     /* the receive power before it */
    double delay_sum;
    int delay_n;
    double delay_min, delay_max;
    /* half duplex: the transmitter moves to the channel each call is for (io_tune). What it sends is heard back on
     * that channel (and on its I/Q image): echo_drop keeps it from going out again as heard. */
    int tx_ch, tx_img;     /* the receiver's channels for the tuned TX frequency and its image; -1 none */
    int echo_ch, echo_img; /* the same for the last key-up */
    int64_t echo_from, echo_to; /* the last key-up's span on the air, receiver positions (48 kS/s) */
} radio_t;

static radio_t R;
static volatile sig_atomic_t stop;
static int quiet, show_bursts;
#ifdef RDOF_HOST_WEB
static int web_port = -1;        /* --web [ADDR:]PORT: the firmware's page (0: any free port) */
static char web_addr[64] = "127.0.0.1";

/* what the page shows, from the modem, the tracker and the parrot */
static void web_publish(double rx, double tx, int cc);
#endif
static rdofradio_dev_t *dev;

/* The SDR's figures for the page, over 1 s windows: samples in, the receive loop's time and where it went */
static struct {
    double t0, wait, rx, spec, mod; /* this window: its start; seconds waiting (reads, the ring), in the receiver,
                                       the spectrum, the modulator */
    long long n;
    double load, share[3], sps;     /* the last window's: the time not waiting, the stages', samples a second */
    double last_samples;            /* when samples last came */
    long long overflows, errors, lost; /* lost: samples */
    double tx_gain, rx_gain, rx_gain_at;
    bool tx_on, agc;
} WS = {.load = -1, .sps = -1};

static void ws_turn(double now)
{
    const double w = now - WS.t0;
    if (WS.t0 > 0 && w < 1.0)
        return;
    if (WS.t0 > 0) {
        WS.sps = (double)WS.n / w;
        WS.load = fmin(1.0, fmax(0.0, 1.0 - WS.wait / w));
        WS.share[0] = WS.rx / w, WS.share[1] = WS.spec / w, WS.share[2] = WS.mod / w;
    }
    WS.t0 = now;
    WS.wait = WS.rx = WS.spec = WS.mod = 0;
    WS.n = 0;
}

/* --split-tx (below) */
static rdof_split_t S;

/* The radio's port: a thread reads it and stamps each read with the time it came, so the split's TIME round trips
 * don't include the main loop's wait on the receive stream. */
#define PORT_Q 64
static struct {
    int fd;
    const char *path;
    pthread_t thread;
    pthread_mutex_t mu;
    struct {
        double t;
        int n;
        uint8_t b[256];
    } q[PORT_Q];
    int head, tail;
    uint32_t dropped;
    volatile bool gone, quit;
} P = {.fd = -1, .mu = PTHREAD_MUTEX_INITIALIZER};
static int in_wait; /* inside the client's wait for a reply: what's heard waits for it to end */
static void split_turn(void);
static void split_flush(void);
static int64_t split_now48(void *ctx);

static void on_signal(int s) { stop = 1; }

/* --config: the file, and the radio it (or the command line) sets */
static const char *config_path;
static rdofradio_cfg_t rcfg;
static char held[300]; /* hold (below): why the settings don't run, "" when they do */

/* a file whole, NUL terminated (malloc'd); NULL with errno set */
static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    size_t cap = 4096, n = 0;
    char *b = malloc(cap + 1);
    for (size_t r; b && (r = fread(b + n, 1, cap - n, f)) > 0;) {
        n += r;
        if (n == cap && cap < (1u << 20)) {
            char *nb = realloc(b, (cap *= 2) + 1);
            if (!nb)
                free(b);
            b = nb;
        } else if (n == cap) {
            break;
        }
    }
    const int e = ferror(f) ? EIO : ENOMEM;
    fclose(f);
    if (!b)
        errno = e;
    else
        b[n] = 0, *len = n;
    return b;
}

/* a line on stderr, and on the web page's log when it runs */
static void say(const char *fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    char line[192];
    vsnprintf(line, sizeof line, fmt, a);
    va_end(a);
    fputs(line, stderr);
#ifdef RDOF_HOST_WEB
    line[strcspn(line, "\n")] = 0;
    rdof_hostweb_logf("%s", line);
#endif
}

static void say_log(void *ctx, const char *msg) { say("%s\n", msg); }

/* ── the transmit writer ───────────────────────────────────────────────────── */

#define GUARD_MARGIN_S 0.006 /* the guard's zeros go in this long before the device's queue would run out */

static struct {
    rdof_cf_t buf[RING];
    int64_t head, tail;    /* samples put in, samples written to the device */
    bool quit;
    int errors;
    pthread_mutex_t mu;
    pthread_cond_t cv;     /* on CLOCK_MONOTONIC (writer_init) */
    pthread_t thread;
    bool running;
    bool guarded;          /* the device takes blocks: the guard keeps the stream's timing through a stall */
    rdof_txguard_t guard;  /* the writer's thread alone (the device's write reports to it) */
} W = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};

static double now_s(void);

static void writer_init(void)
{
#ifndef __APPLE__ /* macOS has no condattr clock: the writer's waits go by the wall clock there (writer_wait) */
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_destroy(&W.cv);
    pthread_cond_init(&W.cv, &a);
    pthread_condattr_destroy(&a);
#endif
    W.guarded = dev->tx_block > 0 && dev->tx_nbuf > 0 && dev->tx_room;
    if (W.guarded) {
        rdof_txguard_init(&W.guard, dev->rate, dev->tx_block, dev->tx_nbuf);
        dev->txguard = &W.guard;
    }
}

/* W.cv until due (now_s's seconds), W.mu held */
static void writer_wait(double due)
{
#ifdef __APPLE__
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    due += (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec - now_s();
#endif
    const struct timespec at = {(time_t)due, (long)((due - floor(due)) * 1e9)};
    pthread_cond_timedwait(&W.cv, &W.mu, &at);
}

/* The loop is late and the device's queue nearly out: zeros finish the block being filled, and as many of the
 * samples that come late are dropped (rdof/sdr/txguard.h). W.mu held; let go while writing. */
static void writer_pad(void)
{
    static const rdof_cf_t zeros[CHUNK];
    int room = dev->tx_room(dev), sent = 0;
    pthread_mutex_unlock(&W.mu);
    while (room > 0) {
        const int k = dev->write(dev, zeros, room < CHUNK ? room : CHUNK, 100000);
        if (k <= 0)
            break;
        room -= k, sent += k;
    }
    pthread_mutex_lock(&W.mu);
    if (room > 0)
        W.errors++;
    rdof_txguard_padded(&W.guard, sent);
}

static void *writer(void *arg)
{
    pthread_mutex_lock(&W.mu);
    for (;;) {
        while (W.head == W.tail && !W.quit) {
            const double due = W.guarded ? rdof_txguard_deadline(&W.guard, GUARD_MARGIN_S) : NAN;
            if (isnan(due)) {
                pthread_cond_wait(&W.cv, &W.mu);
            } else if (now_s() < due) {
                writer_wait(due);
            } else {
                writer_pad();
            }
        }
        if (W.quit)
            break;
        const int at = (int)(W.tail % RING);
        int n = (int)(W.head - W.tail);
        if (n > RING - at)
            n = RING - at;
        if (n > CHUNK)
            n = CHUNK;
        if (W.guarded && W.guard.skip > 0) { /* what the guard's zeros stood in for */
            W.tail += rdof_txguard_drop(&W.guard, n);
            pthread_cond_broadcast(&W.cv);
            continue;
        }
        pthread_mutex_unlock(&W.mu);
        int k = dev->write(dev, W.buf + at, n, 100000);
        pthread_mutex_lock(&W.mu);
        if (k <= 0) {
            W.errors++;
            k = n; /* dropped: the stream slips by n, and the loop's measure of the delay shows it */
        }
        W.tail += k;
        pthread_cond_broadcast(&W.cv);
    }
    pthread_mutex_unlock(&W.mu);
    return NULL;
}

/* put n samples in the ring, waiting for room */
static void tx_put(const rdof_cf_t *x, int n)
{
    pthread_mutex_lock(&W.mu);
    while (RING - (W.head - W.tail) < n && !W.quit)
        pthread_cond_wait(&W.cv, &W.mu);
    for (int i = 0; i < n; i++)
        W.buf[(W.head + i) % RING] = x[i];
    W.head += n;
    pthread_cond_broadcast(&W.cv);
    pthread_mutex_unlock(&W.mu);
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: rdofradio DEVICE --allow-tx --rx MHZ [--tx MHZ] [--duplex full|half] [--cc CC] [-r RATE] [--bw MHZ]\n"
            "                 [--tx-gain DB] [--tx-lo MHZ] [--gain DB] [--center MHZ] [--monitor] [--tx-offset S]\n"
            "                 [--parrot ID] [--stream ARGS] [--seconds S] [--bursts] [--quiet] [-v] [--web [ADDR:]PORT]\n"
            "                 [--last-exit FILE] [--fft builtin|pffft] [--bench [ADDR:]PORT] [--bench-tx MHZ,...]\n"
            "       rdofradio DEVICE --allow-tx --config FILE [--clock-state FILE] [--set-os-clock] [the device's flags]\n"
            "       rdofradio DEVICE --allow-tx --split-tx PORT [--baud N] [--split-power-cap N] --rx MHZ --tx MHZ [--cc CC]\n"
            "                 [-r RATE] [--gain DB] [--center MHZ] [--parrot ID] [--seconds S] [-v] [--web [ADDR:]PORT]\n"
            "DEVICE: %s\n",
#if defined(RDOF_SOAPY) && defined(RDOF_IIO)
            "--soapy ARGS | --iio URI"
#elif defined(RDOF_SOAPY)
            "--soapy ARGS"
#else
            "--iio URI"
#endif
            );
    exit(2);
}

static double mhz(const char *s)
{
    double f = atof(s);
    return f < 1e5 ? f * 1e6 : f;
}

/* ── the modem's io and the client's port: synchronous, one thread ───────── */

static bool bench_modem_reply(const uint8_t *f, size_t n);

static void io_reply(void *ctx, const uint8_t *f, size_t n)
{
    if (bench_modem_reply(f, n))
        return; /* a bench job has the modem */
    if (R.split)
        rdof_split_from_half(&S, RDOF_SPLIT_RX, f, n, now_s()); /* the RX half's reply; split_flush takes it on */
    else
        rdof_modem_feed(&R.client, f, n);
}
/* The receiver's channels (main sets them) */
static rdof_sdr_channel_t CH[RDOF_SDR_MAX_CHANNELS];
static int NCH;
static int channel_of(double hz)
{
    for (int c = 0; c < NCH; c++)
        if (fabs(CH[c].freq_hz - hz) < 1000)
            return c;
    return -1;
}
/* The rate: -r, else the least that holds every receive channel in one window, from the device's usual rate up
 * to the most it streams here: on a PlutoSDR's own ARM (--iio local:) 2.4 MS/s, what its CPU is known to keep up
 * with; from a host over USB 2 or the network 3.84 MS/s (reference.md: about 3.5 to 4 MS/s each way); through
 * SoapySDR 3.2 MS/s, an RTL-SDR's (give others -r). */
static bool dev_iio, dev_local;
static double rate_flag;
static double span_of(const rdof_sdr_channel_t *c, int n)
{
    double lo = c[0].freq_hz, hi = c[0].freq_hz;
    for (int i = 1; i < n; i++)
        lo = fmin(lo, c[i].freq_hz), hi = fmax(hi, c[i].freq_hz);
    return hi - lo;
}
static double plan_rate(const rdof_sdr_channel_t *c, int n, char *why, size_t cap)
{
    double f[RDOF_SDR_MAX_CHANNELS], center;
    for (int i = 0; i < n; i++)
        f[i] = c[i].freq_hz;
    const double lo = rate_flag ? rate_flag : dev_iio ? 768000 : 2400000;
    const double hi = rate_flag ? rate_flag : dev_local ? 2400000 : dev_iio ? 3840000 : 3200000;
    for (double r = ceil(lo / 48000) * 48000; r <= hi; r += 48000) {
        if (rdof_plan_center(f, n, r, &center))
            continue;
        /* and one the channelizer takes: its FFT is 64 times the decimation, which the FFT may not do (pffft:
           factors 2, 3 and 5 only) */
        double off[RDOF_SDR_MAX_CHANNELS];
        for (int i = 0; i < n; i++)
            off[i] = f[i] - center;
        rdof_chz_t z;
        const int bad = rdof_chz_init(&z, r, off, n, (int)(r / 48000), RDOF_CHZ_OUT_FFT, RDOF_CHZ_CUTOFF_HZ);
        rdof_chz_free(&z);
        if (!bad)
            return r;
    }
    double flo = f[0], fhi = f[0];
    for (int i = 1; i < n; i++)
        flo = fmin(flo, f[i]), fhi = fmax(fhi, f[i]);
    snprintf(why, cap, "receive channels %.4f to %.4f MHz: wider than %.2f MS/s holds (%s); fewer channels%s",
             flo / 1e6, fhi / 1e6, hi / 1e6, rate_flag ? "-r" : "this device's most here",
             rate_flag ? ", or a wider -r" : "");
    return 0;
}
/* A repeater hears on its RX channel. A transceiver hears on any channel the receiver has, and may go where none
 * hears it to transmit (the modem then hears nothing there). */
static int io_channel(void *ctx, uint32_t hz)
{
    if (R.full)
        return hz == R.rx_hz ? 0 : -1;
    const int c = channel_of(hz);
    return c >= 0 ? c : RDOF_SDRMODEM_NOT_HEARD;
}
static bool io_tune(void *ctx, uint32_t hz, double *off)
{
    if (!R.full && fabs((double)hz - R.tx_lo) > R.rate * 0.4 - 12.5e3) {
        /* past the modulator's reach: the TX LO moves, 100 kHz above (unkeyed: the gain is at its least) */
        const double lo = (double)hz + 100e3;
        if (!dev->set_tx_freq || dev->set_tx_freq(dev, lo)) {
            fprintf(stderr, "transmit: can't move the TX LO to %.4f MHz: %s\n", lo / 1e6, dev->error);
            return false;
        }
        R.tx_lo = lo;
        if (R.verbose)
            fprintf(stderr, "transmit: the TX LO to %.4f MHz\n", lo / 1e6);
    }
    *off = (double)hz - R.tx_lo; /* the carrier from the TX LO */
    if (!R.full) {
        R.tx_hz = hz;
        R.tx_ch = channel_of(hz);
        R.tx_img = channel_of(2 * R.rx.center_hz - hz);
        if (R.tx_img == R.tx_ch)
            R.tx_img = -1;
    }
    return fabs(*off) < R.rate * 0.4;
}
static bool p_write(void *ctx, const uint8_t *d, size_t n)
{
    if (R.split) {
        rdof_split_upstream(&S, d, n, now_s());
        split_flush();
        return !S.failed[0];
    }
    rdof_sdrmodem_write(&R.m, d, n);
    return true;
}
static bool p_wait(void *ctx, int ms)
{
    if (!R.split)
        return R.client.reply_ready; /* the SDR modem answers at once */
    /* the radio answers over its port: the receiver keeps reading meanwhile (the device's buffers are short) */
    const double end = now_s() + ms / 1000.0;
    in_wait++;
    while (!R.client.reply_ready && !stop && !S.failed[0] && now_s() < end)
        split_turn();
    in_wait--;
    return R.client.reply_ready;
}
static double p_clock(void *ctx) { return now_s(); }
static void p_sleep(void *ctx, int ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* What's heard while the repeater waits for a reply (split): delivered once the wait is over. */
#define HEARD_Q 32
static struct {
    int slot, rssi; /* rssi INT_MIN: lost */
    uint8_t control, burst[RDOF_BURST_BYTES];
} heard_q[HEARD_Q];
static int heard_n;
static void deliver_heard(void);

/* A duplex repeater job (bench_repeater, from the bench's line protocol): the transceiver pauses, a repeater with no
 * network runs on the modem, and what it hears and does goes to the job. The bench's thread asks (the engine's rpt
 * hooks); the main loop runs it (bench_rpt_poll), as the transmit jobs. */
#define BR_EVENTS 64
static struct {
    pthread_mutex_t mu;    /* all but r, which is the main loop's */
    int want;              /* 1 open, 2 close */
    bool active, ready, failed;
    char why[120];
    rdof_bl_rpt_cfg_t cfg;
    double asked;
    rdof_bl_rpt_ev_t ev[BR_EVENTS];
    int nev;
    struct {
        uint8_t control, burst[RDOF_BURST_BYTES];
    } wake[4];
    int nwake;
    bool cal;              /* the first key-up's edge sets the slot timing */
    int64_t late48;        /* the modem's tx_late48 before the job */
    rdof_repeater_t r;
} BR = {.mu = PTHREAD_MUTEX_INITIALIZER};

static double wall_s(void);
static void br_event(const rdof_bl_rpt_ev_t *e)
{
    pthread_mutex_lock(&BR.mu);
    if (BR.nev < BR_EVENTS) {
        BR.ev[BR.nev] = *e;
        BR.ev[BR.nev++].t = wall_s();
    }
    pthread_mutex_unlock(&BR.mu);
}

static void on_rx(void *ctx, int slot, uint8_t control, const uint8_t *burst, int rssi)
{
    if (in_wait) {
        if (heard_n < HEARD_Q) {
            heard_q[heard_n].slot = slot, heard_q[heard_n].rssi = rssi, heard_q[heard_n].control = control;
            memcpy(heard_q[heard_n++].burst, burst, RDOF_BURST_BYTES);
        }
        return;
    }
    if (R.verbose && R.full && R.m.keyed)
        fprintf(stderr, "inbound: slot %d, %+.2f slots from its place\n", slot, R.m.inbound_off);
    if (R.parrot)
        rdof_parrot_heard(R.parrot, slot, control, burst, now_s());
    if (R.full) {
        rdof_repeater_heard(&R.r, slot, control, burst, rssi);
    } else if (BR.active) {
        rdof_bl_rpt_ev_t e = {.kind = RDOF_BL_RPT_HEARD, .slot = slot, .rssi = rssi, .control = control};
        memcpy(e.burst, burst, sizeof e.burst);
        br_event(&e);
        rdof_repeater_heard(&BR.r, slot, control, burst, rssi);
    } else {
        rdof_txvr_heard(&R.t, slot, control, burst, rssi);
    }
}
static void on_lost(void *ctx, int slot)
{
    if (in_wait) {
        if (heard_n < HEARD_Q)
            heard_q[heard_n].slot = slot, heard_q[heard_n++].rssi = INT_MIN;
        return;
    }
    if (R.parrot)
        rdof_parrot_lost(R.parrot, slot, now_s());
    if (R.full) {
        rdof_repeater_lost(&R.r, slot);
    } else if (BR.active) {
        br_event(&(rdof_bl_rpt_ev_t){.kind = RDOF_BL_RPT_LOST, .slot = slot});
        rdof_repeater_lost(&BR.r, slot);
    }
}
static void deliver_heard(void)
{
    for (int i = 0; i < heard_n; i++)
        if (heard_q[i].rssi == INT_MIN)
            on_lost(&R, heard_q[i].slot);
        else
            on_rx(&R, heard_q[i].slot, heard_q[i].control, heard_q[i].burst, heard_q[i].rssi);
    heard_n = 0;
}

static void on_nak(void *ctx, uint8_t cmd, int err)
{
    fprintf(stderr, "modem NAK 0x%02x err %d\n", cmd, err);
    if (R.full)
        rdof_repeater_nak(&R.r, cmd, err);
    else if (BR.active)
        rdof_repeater_nak(&BR.r, cmd, err);
}

/* MQTT (below) */
static void mq_tx_event(const char *action, const rdof_tx_call_info_t *c, double d);
static void mq_status_soon(void);
static void on_sent(void *ctx, const rdof_tx_call_info_t *c, const rdof_net_burst_t *b);
static long mq_overflows; /* the device's, for the SDR's status */

static void on_tx(void *ctx, const char *action, const rdof_tx_call_info_t *c, double d)
{
    say("repeater: %s slot %d %u -> %s%u (%.2f s)\n", action, c->slot, c->src, c->group ? "TG " : "",
            c->dst, d);
    mq_tx_event(action, c, d);
#ifdef RDOF_HOST_WEB
    rdof_hostweb_net_call(R.full ? "repeater" : "roamer", rcfg.name, action, c, d); /* last heard: NET */
#endif
}
static void on_key(void *ctx, bool keyed)
{
    R.keyed = keyed;
    say("repeater: %s\n", keyed ? "keyed" : "unkeyed");
    mq_status_soon(); /* as the board's */
}

/* ── the call router (version 3): what goes up, what comes down, where (rdof/siteroute.h) ─────────── */

static rdof_siteroute_t SR;
static bool sr_on;
static int sr_hblink = -1, sr_parrot = -1; /* the site's HBlink network, its parrot app */
static int sr_mqtt = -1;                   /* its MQTT network, when that carries calls */
static bool mq_report = true;              /* the MQTT network's "report": status, call JSON, raw bursts */

/* the frequency the radio hears on now: a repeater's RX, a transceiver's channel */
static uint32_t heard_hz(void)
{
    if (!R.full && R.t.ready) {
        uint32_t f;
        int cc;
        rdof_txvr_channel_now(&R.t, &f, &cc);
        if (f)
            return f;
    }
    return R.rx_hz;
}
static void sr_start(void)
{
    const rdof_site_t *s = &rcfg.site;
    rdof_siteroute_init(&SR, s);
    for (int k = 0; k < s->nnet; k++)
        if (s->net[k].type == RDOF_NET_HBLINK && sr_hblink < 0)
            sr_hblink = k;
        else if (s->net[k].type == RDOF_NET_MQTT) {
            mq_report = s->net[k].report;
            if (s->net[k].calls)
                sr_mqtt = k;
        }
    sr_parrot = rdof_site_app_of(s, RDOF_APP_PARROT);
    if (R.full) {
        for (int a = 0; a < s->napp; a++)
            if (s->app[a].type == RDOF_APP_REPEATER && s->freq[s->app[a].in].hz == R.rx_hz)
                rdof_siteroute_add_repeater(&SR, a, &R.r);
    } else {
        rdof_siteroute_add_txvr(&SR, &R.t, 0);
    }
    sr_on = true;
}
/* The HBlink sinks' admit: a stream heard at the sink's frequency (ctx: a monitored channel, NULL the radio's RX)
   goes up when its path's policy sends it to HBlink. */
typedef struct mon mon_t;
static uint32_t mon_hz(const mon_t *m);
static int rf_net(void *ctx, int slot, uint32_t src, uint32_t dst, bool group)
{
    const uint32_t hz = ctx ? mon_hz(ctx) : R.rx_hz;
    const int net = rdof_siteroute_rf(&SR, hz, slot, src, dst, group, now_s());
    if (net < 0 && SR.why && R.verbose)
        say("router: %.5f MHz slot %d, %u -> %s%u: %s\n", hz / 1e6, slot, (unsigned)src, group ? "TG " : "",
            (unsigned)dst, SR.why);
    return net;
}
static bool rf_admit(void *ctx, int slot, uint32_t src, uint32_t dst, bool group)
{
    if (!sr_on)
        return true;
    const int net = rf_net(ctx, slot, src, dst, group);
    return net >= 0 && net == sr_hblink;
}
/* The parrot's: the path it's heard on may reach it. */
static bool parrot_admit(void *ctx, int slot, uint32_t src, uint32_t dst, bool group)
{
    return !sr_on || rdof_siteroute_reaches(&SR, heard_hz(), slot, sr_parrot);
}

/* the parrot's replay, as the network would send it: a transceiver's tune, then the bursts */
static void parrot_start(void *ctx, const uint8_t sid[4], int slot, uint32_t src, uint32_t dst, bool group)
{
    say("parrot: sending back slot %d %u -> %s%u, %d bursts\n", slot, src, group ? "TG " : "", dst,
            R.parrot->n);
    if (R.full)
        return;
    rdof_tune_t tu = {{0}};
    memcpy(tu.stream_id, sid, 4);
    tu.freq = R.tx_hz;
    tu.cc = R.cc;
    tu.power = 255;
    rdof_txvr_on_tune(&R.t, &tu);
}
static void parrot_send(void *ctx, const rdof_net_burst_t *b)
{
    if (R.full)
        rdof_repeater_on_burst(&R.r, b);
    else
        rdof_txvr_on_burst(&R.t, b);
}
/* the transceiver's answer to a tune (DMRK), to HBlink4 when it's on */
static void net_send(const uint8_t *d, size_t n);
/* The streams a monitored channel's peer was sent (below): the transceiver plays them, and its DMRK for them goes
 * nowhere (the master sent no DMRT). */
static uint8_t mon_sids[16][4];
static int mon_sid_next;
static bool mon_sid(const uint8_t *sid)
{
    for (int i = 0; i < 16; i++)
        if (!memcmp(mon_sids[i], sid, 4))
            return true;
    return false;
}
static void txvr_send(void *ctx, const uint8_t *d, size_t n)
{
    static const char *status[] = {"on air", "channel busy", "not allowed", "radio error", "transceiver busy"};
    const bool dmrk = n >= 13 && !memcmp(d, "DMRK", 4);
    if (dmrk && (mon_sid(d + 8) || (sr_on && rdof_siteroute_tuned(&SR, d + 8)))) {
        say("transceiver (a monitored channel's call): %s\n", d[12] < 5 ? status[d[12]] : "?");
        return;
    }
    net_send(d, n);
    if (dmrk)
        say("transceiver: %s\n", d[12] < 5 ? status[d[12]] : "?");
}

/* ── the network: an HBlink4 peer, as the board's ──────────────────────────────── */

static rdofnet_peer_t NET;
static bool net_on;
static rdof_hb_sink_t NET_SINK; /* the RX channel's tracker -> DMRD */

static void net_send(const uint8_t *d, size_t n)
{
    if (net_on)
        rdofnet_peer_send(&NET, d, n);
}
static void net_say(const char *line) { say("%s\n", line); }
/* the master's DMRD and DMRT, to the repeater or the transceiver (the main loop's thread, as their polls) */
static void net_packet(void *ctx, const uint8_t *d, size_t n)
{
    uint8_t k[RDOF_DMRK_LEN];
    if (sr_on && rdof_siteroute_dmrd(&SR, sr_hblink, rcfg.peer_id, d, n, now_s()))
        return; /* by the call policies */
    if (rdof_hb_dispatch(d, n, R.full ? &R.r : NULL, R.full ? NULL : &R.t, rcfg.peer_id, k))
        net_send(k, sizeof k);
}
static void net_config(void)
{
    uint8_t pkt[RDOF_DMRC_MAX];
    uint32_t f = R.rx_hz;
    int cc = R.cc;
    if (!R.full && R.t.ready) { /* a transceiver: where it is now */
        uint32_t tf;
        int tcc;
        rdof_txvr_channel_now(&R.t, &tf, &tcc);
        if (tf)
            f = tf, cc = tcc;
    }
    rdofnet_peer_set_config(&NET, pkt, rdof_hb_role_dmrc(&rcfg.cfg, f, cc, pkt), now_s());
}
static void txvr_retune(void *ctx, uint32_t freq, int cc)
{
    say("transceiver: tuned to %.5f MHz CC%d\n", freq / 1e6, cc);
    net_config();
    mq_status_soon();
}
static void sink_send(void *ctx, const uint8_t *d, size_t n) { net_send(d, n); }
static uint32_t rng_state;
static uint32_t sink_rand(void *ctx) /* stream ids: xorshift32 */
{
    uint32_t x = rng_state;
    x ^= x << 13, x ^= x >> 17, x ^= x << 5;
    return rng_state = x;
}
static void sink_before(void *ctx) { net_config(); } /* HBlink4 credits a stream to the peer's last DMRC */

static void net_start(void)
{
    if (!rcfg.has_hblink)
        return;
    if (strcmp(rcfg.cfg.transport, "udp"))
        fprintf(stderr, "hblink: transport %s isn't on this host: UDP\n", rcfg.cfg.transport);
    rdofnet_peer_init(&NET, rcfg.cfg.master_host, rcfg.cfg.master_port, rcfg.peer_id,
                      R.full ? RDOF_HB_KEEPALIVE_SECONDS : RDOF_HB_ROAMING_KEEPALIVE_SECONDS, rcfg.name);
    NET.on_packet = net_packet;
    NET.say = net_say;
    rng_state = (uint32_t)time(NULL) ^ ((uint32_t)getpid() << 16) ^ (uint32_t)(now_s() * 1e6);
    if (!rng_state)
        rng_state = 1;
    rdof_hb_sink_init(&NET_SINK, rcfg.peer_id);
    NET_SINK.send = sink_send;
    NET_SINK.rand32 = sink_rand;
    if (!R.full)
        NET_SINK.before_stream = sink_before;
    NET_SINK.admit = rf_admit;
    const rdof_sink_t sk = rdof_hb_sink_as_sink(&NET_SINK);
    if (rcfg.cfg.receiver_hblink) /* what it hears goes up (the role's "hblink", default true) */
        rdof_sdr_rx_add_sink(&R.rx, 0, &sk);
    net_on = true;
    net_config();
    fprintf(stderr, "hblink: %s, peer %u, to %s:%d\n", rcfg.name, (unsigned)rcfg.peer_id, rcfg.cfg.master_host,
            rcfg.cfg.master_port);
}

/* The monitored channels (monitor roles on the device): receive only, each its own peer, as the board's SDR's */
struct mon {
    int c; /* the receiver's channel */
    const rdof_sdr_cfg_channel_t *ch;
    bool peer;  /* its own peer (identity per_path) */
    bool up;    /* its sink sends through the radio's peer (version 3, one identity) */
    rdofnet_peer_t net;
    rdof_hb_sink_t sink;
    uint8_t sid[4]; /* the last stream the master sent it */
};
static uint32_t mon_hz(const mon_t *m) { return (uint32_t)llround(m->ch->freq); }
static mon_t MON[RDOF_SDR_MAX_CHANNELS];
static int nmon;

static void mon_send(void *ctx, const uint8_t *d, size_t n) { rdofnet_peer_send(&((mon_t *)ctx)->net, d, n); }
/* Whether the transceiver may go to a monitored channel: one of its channels (the roamer's). */
static bool mon_sends(const mon_t *m, int *cc)
{
    if (R.full || !R.t.ready)
        return false;
    const uint32_t f = (uint32_t)llround(m->ch->freq);
    for (int i = 0; i < R.t.nchannels; i++)
        if (R.t.channels[i].freq + RDOF_TXVR_FREQ_TOLERANCE_HZ >= f && R.t.channels[i].freq <= f + RDOF_TXVR_FREQ_TOLERANCE_HZ) {
            *cc = R.t.channels[i].cc;
            return true;
        }
    return false;
}
static void mon_packet(void *ctx, const uint8_t *d, size_t n)
{
    mon_t *m = ctx;
    rdof_net_burst_t b;
    int cc;
    if (sr_on && rdof_siteroute_dmrd(&SR, sr_hblink, m->net.peer.peer_id, d, n, now_s()))
        return; /* by the call policies (identity per_path: this peer's paths) */
    /* a simplex channel: what the master sends this channel's peer goes out on it, through the transceiver (one
       channel at a time: a call while it's busy elsewhere is refused, as a DMRT would be) */
    if (rdof_hb_kind(d, n) == 'D' && mon_sends(m, &cc) && rdof_parse_dmrd(d, n, &b)) {
        if (memcmp(m->sid, b.stream_id, 4)) {
            memcpy(m->sid, b.stream_id, 4);
            memcpy(mon_sids[mon_sid_next], b.stream_id, 4);
            mon_sid_next = (mon_sid_next + 1) % 16;
            rdof_tune_t tu = {.freq = (uint32_t)llround(m->ch->freq), .cc = cc, .power = 255};
            memcpy(tu.stream_id, b.stream_id, 4);
            say("%s: a call from the network (%u to %u), to send on %.5f MHz\n", m->ch->id, (unsigned)b.src,
                (unsigned)b.dst, tu.freq / 1e6);
            rdof_txvr_on_tune(&R.t, &tu);
        }
        rdof_txvr_on_burst(&R.t, &b);
        return;
    }
    uint8_t k[RDOF_DMRK_LEN];
    if (rdof_hb_dispatch(d, n, NULL, NULL, m->net.peer.peer_id, k)) /* nothing here sends: a DMRT is refused */
        rdofnet_peer_send(&m->net, k, sizeof k);
}

static void mon_start(void)
{
    if (!rcfg.has_hblink)
        return;
    const rdof_location_t *loc = rcfg.cfg.has_location ? &rcfg.cfg.location : NULL;
    for (int i = 0; i < nmon; i++) {
        mon_t *m = &MON[i];
        if (!m->ch->hblink && net_on) {
            /* one identity: what it hears goes up through the radio's peer, as its policy says */
            rdof_hb_sink_init(&m->sink, rcfg.peer_id);
            m->sink.send = sink_send;
            m->sink.rand32 = sink_rand;
            if (!R.full)
                m->sink.before_stream = sink_before;
            m->sink.admit = rf_admit;
            m->sink.ctx = m;
            const rdof_sink_t sk = rdof_hb_sink_as_sink(&m->sink);
            rdof_sdr_rx_add_sink(&R.rx, m->c, &sk);
            m->up = true;
            continue;
        }
        if (!m->ch->hblink || m->ch->peer_id < 0)
            continue;
        rdofnet_peer_init(&m->net, rcfg.cfg.master_host, rcfg.cfg.master_port, (uint32_t)m->ch->peer_id,
                          RDOF_HB_KEEPALIVE_SECONDS, m->ch->id);
        m->net.on_packet = mon_packet;
        m->net.say = net_say;
        m->net.ctx = m;
        rdof_hb_sink_init(&m->sink, (uint32_t)m->ch->peer_id);
        m->sink.send = mon_send;
        m->sink.rand32 = sink_rand;
        m->sink.admit = rf_admit;
        m->sink.ctx = m;
        const rdof_sink_t sk = rdof_hb_sink_as_sink(&m->sink);
        rdof_sdr_rx_add_sink(&R.rx, m->c, &sk);
        uint8_t pkt[RDOF_DMRC_MAX];
        rdofnet_peer_set_config(&m->net, pkt,
                                rdof_hb_channel_dmrc(m->net.peer.peer_id, m->ch->id, (uint32_t)llround(m->ch->freq),
                                                     m->ch->cc, loc, pkt),
                                now_s());
        m->peer = true;
        fprintf(stderr, "hblink: %s, peer %u, to %s:%d\n", m->ch->id, (unsigned)m->net.peer.peer_id,
                rcfg.cfg.master_host, rcfg.cfg.master_port);
    }
}

static void net_poll(double now)
{
    if (net_on)
        rdofnet_peer_poll(&NET, now);
    for (int i = 0; i < nmon; i++)
        if (MON[i].peer)
            rdofnet_peer_poll(&MON[i].net, now);
}

static void net_close(void)
{
    if (net_on)
        rdofnet_peer_close(&NET);
    for (int i = 0; i < nmon; i++)
        if (MON[i].peer)
            rdofnet_peer_close(&MON[i].net);
}

/* ── the clock: netclock, or the OS's ─────────────────────────────────────────── */

static const char *clock_state;
static bool set_os_clock, clock_on;

#ifdef RDOF_NETCLOCK
#ifdef RDOF_HOST_WEB
static const char *clk_source(void) { return netclock_source_name(netclock_posix_source()); }
static size_t clk_json(char *out, size_t cap) { return netclock_posix_status_json(out, cap); }
static bool clk_set(int64_t utc_us) { return netclock_posix_set_from_client(utc_us); }
static size_t clk_settings(char *out, size_t cap)
{
    const int n = snprintf(out, cap, "{\"servers\":\"%s\",\"poll_s\":%u}", rcfg.ntp_servers, rcfg.ntp_poll_s);
    return n < 0 ? 0 : (size_t)n < cap ? (size_t)n : cap - 1;
}
#endif

static void clock_start(void)
{
    if (!rcfg.own_clock)
        return;
    const netclock_posix_opts_t o = {.mode = NETCLOCK_POSIX_DISCIPLINE, .servers = rcfg.ntp_servers,
                                     .poll_s = rcfg.ntp_poll_s, .state_path = clock_state, .log = say_log,
                                     .set_os_clock = set_os_clock};
    if (netclock_posix_init(&o) || netclock_posix_start()) {
        fprintf(stderr, "clock: %s\n", strerror(errno));
        return;
    }
    clock_on = true;
#ifdef RDOF_HOST_WEB
    static const rdof_hostweb_clock_t hc = {clk_source, clk_json, clk_set, clk_settings};
    rdof_hostweb_clock(&hc);
#endif
    fprintf(stderr, "clock: netclock from %s%s\n", rcfg.ntp_servers[0] ? rcfg.ntp_servers : "the browser only",
            set_os_clock ? ", setting the OS clock" : "");
}

static void clock_stop(void)
{
    if (!clock_on)
        return;
    netclock_posix_save();
    netclock_posix_stop();
}
#else
static void clock_start(void)
{
    if (rcfg.own_clock)
        fprintf(stderr, "clock: netclock isn't built in here: the OS's clock\n");
}
static void clock_stop(void) {}
#endif

/* ── MQTT: the board's topics (esp_idf/main/roam_mqtt.c for the radio, sdr_mqtt.c for the monitored channels) ─ */

#ifdef RDOF_MQTT
static rdofmq_t *MQ, *MQ_SDR; /* the radio's connection; the monitored channels' (the board's SDR's own) */
static char mq_status[96], mq_sdr_status[96];
static char mq_alias[2][112], mq_sdr_alias[2 * RDOF_SDR_MAX_CHANNELS][112];
static const char *mq_alias_p[2], *mq_sdr_alias_p[2 * RDOF_SDR_MAX_CHANNELS];
static rdof_netcalls_t mq_calls;
static double mq_next, mq_sdr_next; /* the next status (now_s) */
static bool mq_hb_was;
static void mq_status_soon(void) { mq_next = 0; }

/* Unix time: netclock's when it runs, else the OS's */
static double wall_s(void)
{
#ifdef RDOF_NETCLOCK
    if (clock_on)
        return (double)netclock_posix_now_ns() / 1e9;
#endif
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* {root}/{radio}/{app}/{kind}, app "repeater" or "roaming"; {root}/{channel}/{app}/{kind} for the SDR's, app "sdr" */
static void mq_topic(const char *kind, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s/%s/%s", rcfg.cfg.mqtt_root, rcfg.name, rcfg.cfg.mqtt_app, kind);
}
static void mq_sdr_topic(const char *ch, const char *kind, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s/%s/%s", rcfg.cfg.mqtt_root, ch, rcfg.mon_app, kind);
}
static bool is_monitored(const char *ch) { return strcmp(ch, "rx") && strcmp(ch, "out"); }

static void mq_publish_status(void)
{
    char buf[600];
    const bool hb = net_on && NET.peer.up;
    if (R.full) {
        const rdof_repeater_status_t st = {wall_s(), R.rx_hz, R.tx_hz, R.cc, R.r.ready, hb, rdof_repeater_on_air(&R.r),
                                         {false, R.r.enabled[1], R.r.enabled[2]},
                                         {false, rdof_repeater_busy(&R.r, 1), rdof_repeater_busy(&R.r, 2)}};
        rdof_repeater_status_json(&st, buf, sizeof buf);
    } else {
        const rdof_roamer_status_t st = {wall_s(), R.rx_hz, R.cc, rdof_txvr_listening(&R.t), rdof_txvr_scanning(&R.t),
                                       R.t.ready, hb, R.t.has_call && R.t.call.state == RDOF_CALL_ON_AIR};
        rdof_roamer_status_json(&st, buf, sizeof buf);
    }
    rdofmq_publish(MQ, mq_status, buf, -1, 1, true);
}

static void mq_publish_receiver(void)
{
    char buf[512], t[112];
    const rdof_location_t *loc = rcfg.cfg.has_location ? &rcfg.cfg.location : NULL;
    if (R.full)
        rdof_repeater_receiver_json(rcfg.name, R.rx_hz, R.tx_hz, R.cc, rcfg.cfg.callsign, loc, 0, buf, sizeof buf);
    else
        rdof_roamer_receiver_json(rcfg.name, R.rx_hz, R.cc, rcfg.cfg.callsign, loc, buf, sizeof buf);
    mq_topic("json", t, sizeof t);
    rdofmq_publish(MQ, t, buf, -1, 1, true);
}

static void mq_sdr_publish_receivers(void)
{
    char buf[512], t[112];
    const rdof_location_t *loc = rcfg.cfg.has_location ? &rcfg.cfg.location : NULL;
    for (int i = 0; i < nmon; i++) {
        const rdof_sdr_cfg_channel_t *c = MON[i].ch;
        rdof_sdr_receiver_json(c->name[0] ? c->name : c->id, c->freq, c->cc, rcfg.mon_name, c->tg, loc, buf, sizeof buf);
        mq_sdr_topic(c->id, "json", t, sizeof t);
        rdofmq_publish(MQ_SDR, t, buf, -1, 1, true);
    }
}

/* the SDR's status: every receiver channel (the radio's RX among them, by the radio's name), the monitored
   channels' peers */
static void mq_sdr_publish_status(void)
{
    static char buf[3072];
    const char *names[RDOF_SDR_MAX_CHANNELS] = {rcfg.name}, *ids[RDOF_SDR_MAX_CHANNELS];
    bool up[RDOF_SDR_MAX_CHANNELS];
    int nh = 0;
    for (int i = 0; i < nmon; i++) {
        names[MON[i].c] = MON[i].ch->name[0] ? MON[i].ch->name : NULL;
        if (MON[i].peer)
            ids[nh] = MON[i].ch->id, up[nh++] = MON[i].net.peer.up;
    }
    rdof_sdr_status_t st = {.name = rcfg.mon_name, .at = wall_s(), .cpu_load = NAN, .overflows = mq_overflows,
                            .names = names, .n_hblink = nh, .hblink_ids = ids, .hblink_up = up};
    if (rdof_sdr_status_json(&R.rx, &st, buf, sizeof buf))
        rdofmq_publish(MQ_SDR, mq_sdr_status, buf, -1, 1, true);
}

/* MQTT calls (docs/spec/fw/mqtt.md): a call heard on a path whose policy sends it to the MQTT network goes out as
   DMRD packets on {root}/calls/{tg|pc}/{dst}/{box}; other boxes' come in on {root}/calls/# to the router. Those
   arrive on libmosquitto's thread and wait here for the main loop's. */
static rdof_hb_sink_t mq_sinks[1 + RDOF_SDR_MAX_CHANNELS];
static char mq_calls_sub[112];
static pthread_mutex_t mq_in_mu = PTHREAD_MUTEX_INITIALIZER;
static uint8_t mq_in[64][RDOF_DMRD_LEN];
static int mq_in_head, mq_in_tail;
static unsigned mq_in_dropped, mq_calls_out, mq_calls_in;

static bool mq_call_admit(void *ctx, int slot, uint32_t src, uint32_t dst, bool group)
{
    return rf_net(ctx, slot, src, dst, group) == sr_mqtt;
}
static void mq_call_send(void *ctx, const uint8_t *d, size_t n)
{
    char t[160];
    if (rdof_siteroute_calls_topic(rcfg.cfg.mqtt_root, rcfg.site.name, d, n, t, sizeof t) &&
        rdofmq_publish(MQ, t, d, (int)n, 0, false))
        mq_calls_out++;
}
static void mq_message(void *ctx, const char *topic, const void *data, int len)
{
    if (len != RDOF_DMRD_LEN || !rdof_siteroute_calls_theirs(topic, rcfg.cfg.mqtt_root, rcfg.site.name))
        return;
    pthread_mutex_lock(&mq_in_mu);
    const int next = (mq_in_head + 1) % 64;
    if (next == mq_in_tail)
        mq_in_dropped++;
    else {
        memcpy(mq_in[mq_in_head], data, RDOF_DMRD_LEN);
        mq_in_head = next;
    }
    pthread_mutex_unlock(&mq_in_mu);
}
/* the other boxes' calls, to the router (the main loop's) */
static void mq_calls_poll(double now)
{
    uint8_t d[RDOF_DMRD_LEN];
    for (;;) {
        pthread_mutex_lock(&mq_in_mu);
        const bool any = mq_in_tail != mq_in_head;
        if (any) {
            memcpy(d, mq_in[mq_in_tail], RDOF_DMRD_LEN);
            mq_in_tail = (mq_in_tail + 1) % 64;
        }
        pthread_mutex_unlock(&mq_in_mu);
        if (!any)
            return;
        if (rdof_siteroute_dmrd(&SR, sr_mqtt, 0, d, sizeof d, now))
            mq_calls_in++;
    }
}
/* a sink on the radio's RX and on each monitored channel */
static void mq_calls_start(void)
{
    if (!MQ || sr_mqtt < 0)
        return;
    for (int i = 0; i <= nmon; i++) {
        rdof_hb_sink_t *k = &mq_sinks[i];
        rdof_hb_sink_init(k, rcfg.site.id);
        k->send = mq_call_send;
        k->rand32 = sink_rand;
        k->admit = mq_call_admit;
        k->ctx = i ? &MON[i - 1] : NULL;
        const rdof_sink_t sk = rdof_hb_sink_as_sink(k);
        rdof_sdr_rx_add_sink(&R.rx, i ? MON[i - 1].c : 0, &sk);
    }
    fprintf(stderr, "mqtt: calls on %s/calls/ as %s\n", rcfg.cfg.mqtt_root, rcfg.site.name);
}

static void mq_start(void)
{
    static char id[64], sid[64];
    const rdof_config_t *c = &rcfg.cfg;
    if (!c->has_mqtt || !c->mqtt_host[0])
        return;
    rdof_netcalls_init(&mq_calls);
    snprintf(mq_status, sizeof mq_status, "%s/_sdr/%s/status", c->mqtt_root, rcfg.name);
    snprintf(id, sizeof id, "rdof-%s-%s", R.full ? "repeater" : "roaming", rcfg.name);
    mq_topic("raw", mq_alias[0], sizeof mq_alias[0]);
    mq_topic("json", mq_alias[1], sizeof mq_alias[1]);
    mq_alias_p[0] = mq_alias[0], mq_alias_p[1] = mq_alias[1];
    rdofmq_opts_t o = {.host = c->mqtt_host, .user = c->mqtt_user, .pass = c->mqtt_pass, .port = c->mqtt_port,
                       .tls = c->mqtt_tls, .client_id = id, .will_topic = mq_status, .will_msg = RDOF_STATUS_OFFLINE,
                       .alias_topics = mq_alias_p, .n_alias = 2, .queue_max = 1000, .say = net_say};
    if (sr_mqtt >= 0) { /* the radio's connection takes the calls */
        snprintf(mq_calls_sub, sizeof mq_calls_sub, "%s/calls/#", c->mqtt_root);
        o.subscribe = mq_calls_sub, o.on_message = mq_message;
    }
    MQ = rdofmq_start(&o);
    o.subscribe = NULL, o.on_message = NULL;
    mq_calls_start();
    if (nmon && mq_report) {
        snprintf(mq_sdr_status, sizeof mq_sdr_status, "%s/_sdr/%s/status", c->mqtt_root, rcfg.mon_name);
        snprintf(sid, sizeof sid, "rdof-sdr-%s", rcfg.mon_name);
        for (int k = 0; k < 2; k++) /* aliases: every channel's raw, then every channel's json */
            for (int i = 0; i < nmon; i++) {
                mq_sdr_topic(MON[i].ch->id, k ? "json" : "raw", mq_sdr_alias[k * nmon + i], sizeof mq_sdr_alias[0]);
                mq_sdr_alias_p[k * nmon + i] = mq_sdr_alias[k * nmon + i];
            }
        o.client_id = sid, o.will_topic = mq_sdr_status, o.alias_topics = mq_sdr_alias_p, o.n_alias = 2 * nmon;
        MQ_SDR = rdofmq_start(&o);
    }
    fprintf(stderr, "mqtt: %s:%d, root %s, as %s%s%s%s\n", c->mqtt_host, c->mqtt_port, c->mqtt_root, id,
            MQ_SDR ? " and " : "", MQ_SDR ? sid : "", c->mqtt_tls ? ", TLS" : "");
}

/* the main loop's: the retained messages after each connect, the status every 10 s (the SDR's every
   status_seconds) and when HBlink4 comes or goes */
static void mq_poll(double now)
{
    if (sr_mqtt >= 0)
        mq_calls_poll(now);
    if (!mq_report)
        return;
    if (MQ) {
        const bool hb = net_on && NET.peer.up;
        if (rdofmq_connected(MQ)) {
            mq_publish_receiver();
            mq_next = 0;
        }
        if (rdofmq_up(MQ) && (now >= mq_next || hb != mq_hb_was)) {
            mq_hb_was = hb;
            mq_publish_status();
            mq_next = now + 10;
        }
    }
    if (MQ_SDR) {
        if (rdofmq_connected(MQ_SDR)) {
            mq_sdr_publish_receivers();
            mq_sdr_next = 0;
        }
        if (rdofmq_up(MQ_SDR) && now >= mq_sdr_next) {
            mq_sdr_publish_status();
            mq_sdr_next = now + (rcfg.mon_status_s > 1 ? rcfg.mon_status_s : 30);
        }
    }
}

static void mq_stop(void)
{
    rdofmq_stop(MQ);
    rdofmq_stop(MQ_SDR);
    MQ = MQ_SDR = NULL;
}

/* what the radio transmits: .../tx, and the network's calls on .../json and .../raw as the board's */
static void mq_tx_event(const char *action, const rdof_tx_call_info_t *c, double d)
{
    char buf[512], t[112];
    if (!MQ || !mq_report)
        return;
    const double now = wall_s();
    rdof_tx_json(action, c, d, now, buf, sizeof buf);
    mq_topic("tx", t, sizeof t);
    rdofmq_publish(MQ, t, buf, -1, 1, false);
    if (rdof_netcalls_event(&mq_calls, action, c, d, now, buf, sizeof buf)) {
        mq_topic("json", t, sizeof t);
        rdofmq_publish(MQ, t, buf, -1, 1, false);
    }
    if (!R.full)
        mq_publish_status();
}

static void on_sent(void *ctx, const rdof_tx_call_info_t *c, const rdof_net_burst_t *b)
{
    rdof_rawburst_t raw;
    uint8_t buf[RDOF_RAWBURST_MAX];
    char t[112];
    if (MQ && mq_report && rdof_netcalls_burst_raw(&mq_calls, c, b->control, b->burst, wall_s(), &raw)) {
        mq_topic("raw", t, sizeof t);
        rdofmq_publish(MQ, t, buf, (int)rdof_rawburst_encode(&raw, buf), 0, false);
    }
}

static void on_raw_burst(void *ctx, const char *ch, const rdof_rawburst_t *r)
{
    uint8_t buf[RDOF_RAWBURST_MAX];
    char t[112];
    if (!strcmp(ch, "out") || !mq_report)
        return;
    rdofmq_t *q = is_monitored(ch) ? MQ_SDR : MQ;
    if (!q)
        return;
    if (q == MQ)
        mq_topic("raw", t, sizeof t);
    else
        mq_sdr_topic(ch, "raw", t, sizeof t);
    rdofmq_publish(q, t, buf, (int)rdof_rawburst_encode(r, buf), 0, false);
}
#else
static void mq_start(void)
{
    if (rcfg.cfg.has_mqtt)
        fprintf(stderr, "mqtt: rdofradio was built without libmosquitto (RDOF_MQTT): MQTT off\n");
}
static void mq_poll(double now) {}
static void mq_stop(void) {}
static void mq_tx_event(const char *action, const rdof_tx_call_info_t *c, double d) {}
static void mq_status_soon(void) {}
static void on_sent(void *ctx, const rdof_tx_call_info_t *c, const rdof_net_burst_t *b) {}
#endif

/* ── the receiver's output ─────────────────────────────────────────────────── */

static void on_json(void *ctx, const char *ch, const char *json)
{
#ifdef RDOF_MQTT
    char t[112];
    if (!mq_report)
        ;
    else if (is_monitored(ch) && MQ_SDR) {
        mq_sdr_topic(ch, "json", t, sizeof t);
        rdofmq_publish(MQ_SDR, t, json, -1, 1, false);
    } else if (!strcmp(ch, "rx") && MQ) {
        mq_topic("json", t, sizeof t);
        rdofmq_publish(MQ, t, json, -1, 1, false);
    }
#endif
#ifdef RDOF_HOST_WEB
    if (strcmp(ch, "rx") && strcmp(ch, "out")) /* a monitored channel: the SDR's, as on the board */
        rdof_hostweb_tracker_json("sdr", rcfg.box, ch, json);
    else
        rdof_hostweb_tracker_json(R.full ? "repeater" : "roamer", rcfg.name, NULL, json);
#endif
    if (!quiet)
        printf("%s json %s\n", ch, json);
}

/* --bench [ADDR:]PORT: the bench's line protocol (docs/spec/bench-api.md section 14): the receiver's bursts and
 * captures from a history of the wideband stream, for the bench tools (tools/bench/rfgbench.py). */
static int bench_port = -1;
static char bench_addr[64] = "0.0.0.0";
static struct {
    rdof_bl_t *bl;
    rdof_bnet_t *net;
    rdof_bl_ring_t ring;
    pthread_mutex_t mu;     /* ring and the clock's reference, between the receive loop and the server */
    uint64_t ref_sc;        /* a wideband sample and its UTC */
    double ref_utc;
    double late_min, late_from; /* the least a chunk came late against the reference, since late_from */
    bool late_fresh;        /* a read since late_from waited (the loop had caught up) */
    long clipped, seen;
    double t_start;
} BL = {.mu = PTHREAD_MUTEX_INITIALIZER};

static void bl_device(void *ctx, char *kind, size_t kcap, char *name, size_t ncap, char *version, size_t vcap)
{
    snprintf(kind, kcap, "rdofradio");
    snprintf(name, ncap, "%s", rcfg.name[0] ? rcfg.name : "rdofradio");
    snprintf(version, vcap, "%s", RDOF_VERSION);
}

static void bl_window(void *ctx, rdof_bl_window_t *w)
{
    memset(w, 0, sizeof *w);
    w->running = BL.ring.w > 0;
    w->center_hz = R.rx.center_hz;
    w->fs = R.rate;
    w->lo_hz = R.rx.center_hz - 0.4 * R.rate;
    w->hi_hz = R.rx.center_hz + 0.4 * R.rate;
    w->gain_db = dev ? dev->rx_gain(dev) : 0;
    w->iq_scale_db = 20 * log10(BL.ring.scale > 0 ? BL.ring.scale : 1);
}

static long bl_iq_read(void *ctx, uint64_t sc, int8_t *out, long n, uint64_t *oldest, uint64_t *next)
{
    pthread_mutex_lock(&BL.mu);
    const long k = rdof_bl_ring_read(&BL.ring, sc, out, n, oldest, next);
    pthread_mutex_unlock(&BL.mu);
    return k;
}

static long bl_iq_filled(void *ctx, uint64_t sc, long n)
{
    pthread_mutex_lock(&BL.mu);
    const long k = rdof_bl_ring_filled(&BL.ring, sc, n);
    pthread_mutex_unlock(&BL.mu);
    return k;
}

static double bl_utc(void *ctx) { return wall_s(); }

static double bl_sc_utc(void *ctx, uint64_t sc)
{
    pthread_mutex_lock(&BL.mu);
    const double t = BL.ref_utc + ((double)sc - (double)BL.ref_sc) / R.rate;
    pthread_mutex_unlock(&BL.mu);
    return t;
}

static void bl_rx_stats(void *ctx, double *seconds, long *overflows, double *clip_pct, long *dsp_skipped, long *dropped)
{
    pthread_mutex_lock(&BL.mu);
    *seconds = (double)BL.ring.w / R.rate;
    *clip_pct = BL.seen ? 100.0 * (double)BL.clipped / (double)BL.seen : 0;
    pthread_mutex_unlock(&BL.mu);
    *overflows = (long)WS.overflows;
    *dsp_skipped = 0;
    *dropped = 0;
}

/* --bench-tx MHZ[,MHZ...]: where the bench's jobs may transmit (none: no transmit jobs). */
static double bench_tx_mhz[RDOF_BENCHTX_MAX_FREQS];
static int bench_ntx;
static double bench_txg; /* --tx-gain: what a job transmits at */

static bool bench_tx_listed(double hz)
{
    for (int i = 0; i < bench_ntx; i++)
        if (fabs(bench_tx_mhz[i] * 1e6 - hz) < 100.0)
            return true;
    return false;
}

static int bl_freqs(void *ctx, rdof_bl_freq_t *out, int max)
{
    int k = 0;
    for (int c = 0; c < NCH && k < max; c++, k++) {
        out[k] = (rdof_bl_freq_t){.freq_hz = CH[c].freq_hz, .ch = c,
                                  .caps = RDOF_BL_CAP_RX | RDOF_BL_CAP_DMR | RDOF_BL_CAP_IQ |
                                          (bench_tx_listed(CH[c].freq_hz) ? RDOF_BL_CAP_TX : 0)};
        snprintf(out[k].id, sizeof out[k].id, "%s", CH[c].id);
    }
    for (int i = 0; i < bench_ntx && k < max; i++) { /* a transmit frequency no channel hears */
        bool heard = false;
        for (int c = 0; c < NCH; c++)
            heard |= fabs(CH[c].freq_hz - bench_tx_mhz[i] * 1e6) < 100.0;
        if (!heard)
            out[k++] = (rdof_bl_freq_t){.freq_hz = bench_tx_mhz[i] * 1e6, .ch = -1, .caps = RDOF_BL_CAP_TX};
    }
    return k;
}

static const char *bl_can_tx(void *ctx)
{
    if (R.split)
        return "a split repeater's radio transmits, not the SDR: no transmit jobs here";
    if (R.full)
        return "a duplex repeater keeps the transmitter: transmit jobs need --duplex half";
    if (!bench_ntx)
        return "no transmit frequencies (--bench-tx)";
    return NULL;
}

/* A transmit job: the bench's thread asks (the engine's tx hooks), the main loop does it, since the SDR modem and
 * the transceiver over it are the main loop's. The transceiver pauses for the job (no polls: the network's calls
 * wait), rdof_benchtx_t drives the modem in DMO, then the modem is set up again and the transceiver goes on. */
static struct {
    pthread_mutex_t mu;     /* all but tx, which is the main loop's */
    int want;               /* 1 open, 2 close */
    bool active, ready, failed, idle;
    char why[120];
    uint32_t freq;
    int cc;
    double asked;
    rdof_benchtx_item_t in[RDOF_BENCHTX_QUEUE]; /* pushed, not yet in the job's queue */
    int nin, queued;
    rdof_benchtx_t tx;
} BT = {.mu = PTHREAD_MUTEX_INITIALIZER};

static bool bl_tx_open(void *ctx, uint32_t freq, int cc, int power, char *why, size_t cap)
{
    pthread_mutex_lock(&BT.mu);
    const bool busy = BT.want || BT.active || BR.want || BR.active;
    if (!busy) {
        BT.want = 1, BT.freq = freq, BT.cc = cc, BT.asked = now_s();
        BT.ready = BT.failed = BT.idle = false;
        BT.nin = BT.queued = 0;
    }
    pthread_mutex_unlock(&BT.mu);
    if (busy)
        snprintf(why, cap, "the transmitter is still busy with the last job");
    else
        snprintf(why, cap, "--power %d isn't applied: the SDR transmits at --tx-gain %.0f", power, bench_txg);
    return !busy;
}

static int bl_tx_space(void *ctx, char *why, size_t cap)
{
    pthread_mutex_lock(&BT.mu);
    int n = BT.failed ? -1 : !BT.ready ? 0 : RDOF_BENCHTX_QUEUE - BT.nin - BT.queued;
    if (BT.failed)
        snprintf(why, cap, "%s", BT.why);
    pthread_mutex_unlock(&BT.mu);
    return n;
}

static void bl_tx_push(void *ctx, uint8_t control, const uint8_t burst[33])
{
    pthread_mutex_lock(&BT.mu);
    if (BT.nin < RDOF_BENCHTX_QUEUE) {
        BT.in[BT.nin].control = control;
        memcpy(BT.in[BT.nin++].burst, burst, 33);
        BT.idle = false;
    }
    pthread_mutex_unlock(&BT.mu);
}

static void bl_tx_unkey(void *ctx) {} /* the job's modem unkeys once its queue has gone out */

static bool bl_tx_idle(void *ctx)
{
    pthread_mutex_lock(&BT.mu);
    const bool idle = (BT.ready && !BT.nin && BT.idle) || BT.failed;
    pthread_mutex_unlock(&BT.mu);
    return idle;
}

static void bl_tx_close(void *ctx)
{
    pthread_mutex_lock(&BT.mu);
    BT.want = 2;
    pthread_mutex_unlock(&BT.mu);
}

static void bt_to_modem(void *ctx, const uint8_t *b, size_t n) { rdof_sdrmodem_write(&R.m, b, n); }
static void bt_to_tool(void *ctx, const char *json, size_t n) {}
static void bt_keying(void *ctx, double start, double seconds, int bursts, const char *why)
{
    say("bench: keyed %.2f s, %d bursts (%s)\n", seconds, bursts, why);
}

/* The modem's replies while a job has it (the main loop's thread, as BT.active's writes). */
static bool bench_modem_reply(const uint8_t *f, size_t n)
{
    if (!BT.active)
        return false;
    rdof_benchtx_modem(&BT.tx, f, n, now_s());
    return true;
}

/* A signal mode's transmission (bench-api.md section 14.6), as a transmit job: the bench's thread asks, the main loop
 * (whose are the modem and its pulls) keys the source on the modem, and lets it go when asked. */
static struct {
    pthread_mutex_t mu;
    int want;               /* 1 open, 2 close */
    bool active, failed;
    int state;              /* rdof_sdrmodem_signal_state's, once active */
    char why[120];
    uint32_t freq;
    rdof_sig_src_t src;
    double asked;
} BS = {.mu = PTHREAD_MUTEX_INITIALIZER};

static bool bl_sig_open(void *ctx, uint32_t freq, int power, double seconds, const rdof_sig_src_t *src, char *why,
                        size_t cap)
{
    pthread_mutex_lock(&BS.mu);
    pthread_mutex_lock(&BT.mu);
    const bool busy = BS.want || BS.active || BT.want || BT.active || BR.want || BR.active;
    pthread_mutex_unlock(&BT.mu);
    if (!busy) {
        BS.want = 1, BS.freq = freq, BS.src = *src, BS.asked = now_s();
        BS.failed = false, BS.state = 0, BS.why[0] = 0;
    }
    pthread_mutex_unlock(&BS.mu);
    if (busy)
        snprintf(why, cap, "the transmitter is still busy with the last job");
    else
        snprintf(why, cap, "power %d isn't applied: the SDR transmits at --tx-gain %.0f", power, bench_txg);
    return !busy;
}

static int bl_sig_state(void *ctx, char *why, size_t cap)
{
    pthread_mutex_lock(&BS.mu);
    const int st = BS.failed ? -1 : BS.active ? (BS.state == 2 ? 2 : 1) : 1;
    if (BS.failed)
        snprintf(why, cap, "%s", BS.why);
    pthread_mutex_unlock(&BS.mu);
    return st;
}

/* Back only once the main loop let the source go: the engine frees it after this returns. */
static void bl_sig_close(void *ctx)
{
    pthread_mutex_lock(&BS.mu);
    const bool on = BS.want == 1 || BS.active;
    if (BS.want == 1)
        BS.want = 0; /* never keyed */
    else if (BS.active)
        BS.want = 2;
    pthread_mutex_unlock(&BS.mu);
    for (int i = 0; on && i < 2000; i++) {
        pthread_mutex_lock(&BS.mu);
        const bool done = !BS.active;
        pthread_mutex_unlock(&BS.mu);
        if (done)
            return;
        const struct timespec ts = {0, 1000000};
        nanosleep(&ts, NULL);
    }
    if (on)
        say("bench: the main loop kept a signal's source past 2 s\n");
}

static double bl_sig_rate(void *ctx) { return R.m.tx_fs; }

/* The main loop's turn for a signal: true while one has the modem. */
static bool bench_sig_poll(double now)
{
    pthread_mutex_lock(&BS.mu);
    if (BS.want == 1 && !BS.active) {
        if ((R.t.has_call || rdof_sdrmodem_transmitting(&R.m)) && now - BS.asked < 10.0) { /* its call ends first */
            pthread_mutex_unlock(&BS.mu);
            return false;
        }
        BS.want = 0;
        if (rdof_sdrmodem_signal(&R.m, BS.freq, &BS.src, BS.why, sizeof BS.why)) {
            BS.active = true;
            say("bench: a signal has the transmitter: %.4f MHz\n", BS.freq / 1e6);
        } else {
            BS.failed = true;
        }
    }
    if (BS.active && BS.want == 2) {
        rdof_sdrmodem_signal(&R.m, 0, NULL, BS.why, sizeof BS.why);
        BS.want = 0, BS.active = false;
        say("bench: the transmitter is the transceiver's again\n");
    }
    if (BS.active)
        BS.state = rdof_sdrmodem_signal_state(&R.m);
    const bool active = BS.active;
    pthread_mutex_unlock(&BS.mu);
    return active;
}

/* The main loop's turn (POLL_S): true while a job has the modem (the transceiver isn't polled). */
static bool bench_tx_poll(double now)
{
    if (!BL.bl)
        return false;
    if (bench_sig_poll(now))
        return true;
    pthread_mutex_lock(&BT.mu);
    if (BT.want == 1 && !BT.active) {
        if (R.t.has_call && now - BT.asked < 10.0) { /* the transceiver's call ends first */
            pthread_mutex_unlock(&BT.mu);
            return false;
        }
        BT.want = 0, BT.active = true;
        const rdof_benchtx_io_t io = {bt_to_modem, bt_to_tool, bt_keying, NULL, NULL};
        rdof_benchtx_init(&BT.tx, &io, BT.freq, BT.cc, 0, 120.0, now);
        say("bench: a job has the transmitter: %.4f MHz cc %d\n", BT.freq / 1e6, BT.cc);
    }
    if (!BT.active) {
        pthread_mutex_unlock(&BT.mu);
        return false;
    }
    if (BT.want == 2) { /* the job is over: the modem back to the transceiver */
        BT.want = 0, BT.nin = 0;
        pthread_mutex_unlock(&BT.mu);
        rdof_benchtx_stop(&BT.tx, NULL, "closed", now);
        pthread_mutex_lock(&BT.mu);
        BT.active = false;
        pthread_mutex_unlock(&BT.mu);
        if (!rdof_modem_start(&R.client, 1) || !rdof_txvr_start(&R.t))
            say("bench: the transceiver didn't start again: %s\n", R.client.error);
        else
            say("bench: the transmitter is the transceiver's again\n");
        return true;
    }
    for (int i = 0; i < BT.nin && BT.tx.qn < RDOF_BENCHTX_QUEUE; i++)
        BT.tx.q[(BT.tx.qh + BT.tx.qn++) % RDOF_BENCHTX_QUEUE] = BT.in[i];
    BT.nin = 0;
    rdof_benchtx_alive(&BT.tx, now); /* the job's connection is its dead-man */
    rdof_benchtx_tick(&BT.tx, now);
    BT.ready = BT.tx.state == RDOF_TXS_READY;
    BT.queued = BT.tx.qn;
    BT.idle = !BT.tx.keyed && !BT.tx.qn;
    if (BT.tx.state == RDOF_TXS_DONE && !BT.failed) {
        BT.failed = true;
        snprintf(BT.why, sizeof BT.why, "%s", BT.tx.error[0] ? BT.tx.error : "the modem stopped");
    }
    pthread_mutex_unlock(&BT.mu);
    return true;
}

static bool bl_rpt_open(void *ctx, const rdof_bl_rpt_cfg_t *cfg, char *why, size_t cap)
{
    pthread_mutex_lock(&BR.mu);
    const bool busy = BR.want || BR.active || BT.want || BT.active;
    if (!busy) {
        BR.want = 1, BR.cfg = *cfg, BR.asked = now_s();
        BR.ready = BR.failed = false;
        BR.nev = BR.nwake = 0;
    }
    pthread_mutex_unlock(&BR.mu);
    if (busy)
        snprintf(why, cap, "the transmitter is still busy with the last job");
    else if (cfg->power != 255)
        snprintf(why, cap, "--power %d isn't applied: the SDR transmits at --tx-gain %.0f", cfg->power, bench_txg);
    return !busy;
}

static int bl_rpt_state(void *ctx, char *why, size_t cap)
{
    pthread_mutex_lock(&BR.mu);
    const int st = BR.failed ? -1 : BR.ready ? 1 : 0;
    if (BR.failed)
        snprintf(why, cap, "%s", BR.why);
    pthread_mutex_unlock(&BR.mu);
    return st;
}

static bool bl_rpt_event(void *ctx, rdof_bl_rpt_ev_t *ev)
{
    pthread_mutex_lock(&BR.mu);
    const bool any = BR.nev > 0;
    if (any) {
        *ev = BR.ev[0];
        memmove(BR.ev, BR.ev + 1, sizeof BR.ev[0] * (size_t)--BR.nev);
    }
    pthread_mutex_unlock(&BR.mu);
    return any;
}

static void bl_rpt_wake(void *ctx, uint8_t control, const uint8_t burst[33])
{
    pthread_mutex_lock(&BR.mu);
    if (BR.nwake < (int)(sizeof BR.wake / sizeof BR.wake[0])) {
        BR.wake[BR.nwake].control = control;
        memcpy(BR.wake[BR.nwake++].burst, burst, RDOF_BURST_BYTES);
    }
    pthread_mutex_unlock(&BR.mu);
}

static void bl_rpt_close(void *ctx)
{
    pthread_mutex_lock(&BR.mu);
    BR.want = 2;
    pthread_mutex_unlock(&BR.mu);
}

static void br_on_key(void *ctx, bool keyed)
{
    say("bench: the job's repeater %s\n", keyed ? "keyed" : "unkeyed");
    br_event(&(rdof_bl_rpt_ev_t){.kind = RDOF_BL_RPT_KEY, .keyed = keyed});
}

/* The main loop's turn (POLL_S): true while a repeater job has the modem (the transceiver isn't polled). */
static bool bench_rpt_poll(double now)
{
    if (!BL.bl)
        return false;
    pthread_mutex_lock(&BR.mu);
    if (BR.want == 1 && !BR.active) {
        if (R.t.has_call && now - BR.asked < 10.0) { /* the transceiver's call ends first */
            pthread_mutex_unlock(&BR.mu);
            return false;
        }
        BR.want = 0, BR.active = true;
        const rdof_bl_rpt_cfg_t cfg = BR.cfg;
        pthread_mutex_unlock(&BR.mu);
        rdof_repeater_init(&BR.r, &R.radio, rcfg.peer_id, cfg.rx_hz, cfg.tx_hz, cfg.cc);
        BR.r.enabled[1] = cfg.slots & 2u, BR.r.enabled[2] = cfg.slots & 4u;
        BR.r.inactivity_s = cfg.hang_s;
        rdof_repeater_set_call_hang(&BR.r, cfg.call_hang_s);
        BR.r.clock = p_clock;
        BR.r.on_key = br_on_key;
        BR.r.ctx = &R;
        /* the inbound slots follow the transmit path's delay: the first key-up's edge in what's received sets it */
        BR.late48 = R.m.tx_late48;
        BR.cal = true;
        const bool ok = rdof_modem_start(&R.client, 1) && rdof_repeater_start(&BR.r);
        pthread_mutex_lock(&BR.mu);
        BR.ready = ok, BR.failed = !ok;
        if (!ok)
            snprintf(BR.why, sizeof BR.why, "%s", R.client.error);
        pthread_mutex_unlock(&BR.mu);
        say("bench: a job's repeater has the transmitter: RX %.4f TX %.4f MHz cc %d%s%s\n", cfg.rx_hz / 1e6,
            cfg.tx_hz / 1e6, cfg.cc, ok ? "" : ": ", ok ? "" : R.client.error);
        return true;
    }
    if (!BR.active) {
        pthread_mutex_unlock(&BR.mu);
        return false;
    }
    if (BR.want == 2) { /* the job is over: the modem back to the transceiver */
        BR.want = 0, BR.nwake = 0;
        pthread_mutex_unlock(&BR.mu);
        rdof_repeater_stop(&BR.r);
        R.m.tx_late48 = BR.late48;
        pthread_mutex_lock(&BR.mu);
        BR.active = false, BR.nev = 0;
        pthread_mutex_unlock(&BR.mu);
        if (!rdof_modem_start(&R.client, 1) || !rdof_txvr_start(&R.t))
            say("bench: the transceiver didn't start again: %s\n", R.client.error);
        else
            say("bench: the transmitter is the transceiver's again\n");
        return true;
    }
    const int nwake = BR.nwake;
    __typeof__(BR.wake[0]) wake[sizeof BR.wake / sizeof BR.wake[0]];
    memcpy(wake, BR.wake, sizeof wake[0] * (size_t)nwake);
    BR.nwake = 0;
    const bool failed = BR.failed;
    pthread_mutex_unlock(&BR.mu);
    if (failed)
        return true;
    for (int i = 0; i < nwake; i++) /* an idle beacon: as if a radio had asked on slot 1 */
        rdof_repeater_heard(&BR.r, 1, wake[i].control, wake[i].burst, 0);
    if (!rdof_repeater_poll(&BR.r)) {
        pthread_mutex_lock(&BR.mu);
        BR.failed = true;
        snprintf(BR.why, sizeof BR.why, "%s", R.client.error[0] ? R.client.error : "the modem stopped");
        pthread_mutex_unlock(&BR.mu);
    }
    return true;
}

/* A chunk of the wideband stream, before the receiver: into the history (2.5 s), with the clock's reference.
 * fresh: the read that gave it waited for the device (FRESH_S), so the loop wasn't behind. filler: zeros in place of
 * samples the device lost (a capture counts them in gap_samples). */
static void bench_feed(const rdof_cf_t *in, int n, bool fresh, bool filler)
{
    if (!BL.bl)
        return;
    const double utc = wall_s();
    pthread_mutex_lock(&BL.mu);
    long clip = 0;
    rdof_bl_ring_put(&BL.ring, (const float *)in, n, &clip);
    if (filler)
        rdof_bl_ring_mark_filler(&BL.ring, n);
    BL.clipped += clip, BL.seen += n;
    if (BL.seen > (long)(10 * R.rate))
        BL.clipped /= 2, BL.seen /= 2; /* the last ten seconds or so */
    /* The chunk's last sample is at most now: the stream's own count between. A read may come late (the device
     * buffers a stall's samples), so the reference follows the freshest read: at once when a chunk would be in the
     * future, and by the least it came late over a second when that stays over 20 ms (a crystal's drift, a loss).
     * Only a second the loop caught up in counts: while it works through a backlog every chunk comes late, the
     * samples on time (the device held them), and moving the reference then would move the bursts' times. */
    const uint64_t sc = BL.ring.w;
    const double late = utc - (BL.ref_utc + ((double)sc - (double)BL.ref_sc) / R.rate);
    if (!BL.ref_utc || late < 0) {
        BL.ref_sc = sc, BL.ref_utc = utc;
        BL.late_min = INFINITY, BL.late_from = utc, BL.late_fresh = false;
    } else {
        if (late < BL.late_min)
            BL.late_min = late;
        BL.late_fresh |= fresh;
        if (utc - BL.late_from >= 1.0 && BL.late_fresh) {
            if (BL.late_min > 0.02)
                BL.ref_utc += BL.late_min;
            BL.late_min = INFINITY, BL.late_from = utc, BL.late_fresh = false;
        }
    }
    pthread_mutex_unlock(&BL.mu);
}

static void bench_burst(int c, const rdof_sdr_burst_t *b)
{
    if (!BL.bl || !rdof_bl_wants_bursts(BL.bl))
        return;
    rdof_bl_burst_t o = {0};
    o.ch = c;
    o.pos48 = b->position;
    o.t = bl_sc_utc(NULL, (uint64_t)((double)b->position * R.rate / 48000.0));
    o.cfo_hz = (float)b->freq_offset_hz, o.dev_hz = (float)b->deviation_hz, o.level_dbfs = b->b.power_dbfs;
    o.dt = (uint8_t)b->b.dt, o.n = (int8_t)b->b.n, o.slot = (int8_t)b->b.slot, o.family = (int8_t)b->b.family;
    o.cc = (int8_t)b->b.cc, o.sync_errors = (int8_t)b->b.sync_errors;
    memcpy(o.raw, b->b.burst, sizeof o.raw);
    rdof_bl_burst(BL.bl, &o);
}

static void bench_start(void)
{
    if (bench_port < 0)
        return;
    if (!rdof_bl_ring_init(&BL.ring, (long)(2.5 * R.rate))) {
        fprintf(stderr, "bench: no memory for the history\n");
        return;
    }
    /* cs8 from a 12-bit ADC (an AD9361 at 1/2048 a step): its low 8 bits, where a bench signal is (a call at
     * -53 dBFS is a few steps); a signal over -24 dBFS clips, and the capture's clip_pct says so */
    BL.ring.scale = 16;
    static const rdof_bl_platform_t pf = {.device = bl_device, .window = bl_window, .freqs = bl_freqs,
                                          .iq_read = bl_iq_read, .iq_filled = bl_iq_filled, .utc = bl_utc,
                                          .sc_utc = bl_sc_utc,
                                          .rx_stats = bl_rx_stats, .can_tx = bl_can_tx, .tx_open = bl_tx_open,
                                          .tx_space = bl_tx_space, .tx_push = bl_tx_push, .tx_unkey = bl_tx_unkey,
                                          .tx_idle = bl_tx_idle, .tx_close = bl_tx_close, .rpt_open = bl_rpt_open,
                                          .rpt_state = bl_rpt_state, .rpt_event = bl_rpt_event,
                                          .rpt_wake = bl_rpt_wake, .rpt_close = bl_rpt_close,
                                          .modes = rdof_sdr_modes, .sig_open = bl_sig_open,
                                          .sig_state = bl_sig_state, .sig_close = bl_sig_close,
                                          .sig_rate = bl_sig_rate};
    BL.bl = rdof_bl_new(&pf);
    char err[160];
    BL.net = BL.bl ? rdof_bnet_start(BL.bl, bench_addr, bench_port, now_s, 0, err, sizeof err) : NULL;
    if (!BL.net) {
        fprintf(stderr, "bench: %s\n", BL.bl ? err : "no memory");
        rdof_bl_free(BL.bl);
        BL.bl = NULL;
        rdof_bl_ring_free(&BL.ring);
        return;
    }
    fprintf(stderr, "bench: %s:%d\n", bench_addr, rdof_bnet_port(BL.net));
}

static void bench_stop(void)
{
    if (!BL.bl)
        return;
    rdof_bnet_stop(BL.net);
    rdof_bl_free(BL.bl);
    BL.bl = NULL, BL.net = NULL;
    rdof_bl_ring_free(&BL.ring);
}

static void on_burst(void *ctx, int c, const rdof_sdr_burst_t *b, double t)
{
    rdof_sdrmodem_heard(&R.m, c, b);
    bench_burst(c, b);
    if (c == R.mon && R.waiting && R.first_sent) {
        const double d = (double)(b->position - R.first_sent) / 48000.0;
        R.waiting = false;
        fprintf(stderr, "monitor: key-up's first burst heard %.2f ms after its place\n", 1e3 * d);
        if (d > -0.5 && d < 0.5) {
            R.delay_sum += d;
            R.delay_min = R.delay_n ? fmin(R.delay_min, d) : d;
            R.delay_max = R.delay_n ? fmax(R.delay_max, d) : d;
            R.delay_n++;
        }
    }
    if (!show_bursts)
        return;
    char hex[67];
    for (int i = 0; i < RDOF_BURST_BYTES; i++)
        snprintf(hex + 2 * i, 3, "%02x", b->b.burst[i]);
    printf("%s burst %lld %d %d %d %s %d %d %.2f %.1f %.1f %s\n", R.rx.trackers[c].channel, (long long)b->position,
           b->b.dt, b->b.n, b->b.slot, rdof_family_name(b->b.family), b->b.cc, b->b.sync_errors, b->b.power_dbfs,
           b->freq_offset_hz, b->deviation_hz, hex);
}

static bool muted(void *ctx, int64_t p) { return rdof_sdrmodem_muted_at(&R.m, p); }
/* Our own transmission heard back (half duplex): on the channel it went out on, or its image, within its span on
 * the air (60 ms either side: a burst's length and the loop's delay). Not passed on: a monitored channel's peer
 * would send the network's call back to it. The bench's line protocol still gets it: it watches the air. */
static bool echo_drop(void *ctx, int c, const rdof_sdr_burst_t *b)
{
    if (R.full || (c != R.echo_ch && c != R.echo_img) || c < 0)
        return false;
    if (b->position <= R.echo_from - 2880 || b->position >= R.echo_to + 2880)
        return false;
    bench_burst(c, b);
    return true;
}
/* the inbound slot by timing: the radio's RX channel only (a monitored channel keeps its own CACH); a repeater job's
 * RX may be any receiver channel */
static int relabel(void *ctx, int c, const rdof_sdr_burst_t *b)
{
    const int rx = BR.active ? channel_of(BR.cfg.rx_hz) : 0;
    return c == rx ? rdof_sdrmodem_inbound(&R.m, b) : 0;
}

static void tx_gain(double db)
{
    WS.tx_gain = db;
    if (dev->set_tx_gain(dev, db))
        fprintf(stderr, "TX gain %.1f: %s\n", db, dev->error);
}

static void shutdown_tx(void)
{
    if (!dev)
        return;
    tx_gain(dev->tx_gain_min);
    if (W.running) {
        pthread_mutex_lock(&W.mu);
        W.quit = true;
        pthread_cond_broadcast(&W.cv);
        pthread_mutex_unlock(&W.mu);
        pthread_join(W.thread, NULL);
        W.running = false;
    }
    dev->stop_tx(dev);
}

#ifdef RDOF_HOST_WEB
/* The page's spectrum and waterfall: the receive stream, a row about every 200 ms, while the page looks. Not
 * during the calibration, when the receiver is on the TX LO. */
static void web_spectrum(const rdof_cf_t *in, int n)
{
    static rdof_spectrum_t sp;
    static int made; /* 1 made, -1 no memory */
    if (made < 0 || n <= 0 || R.cal || !rdof_hostweb_spectrum_wanted())
        return;
    /* the loop's read, 10 ms (a read can come back shorter) */
    const int chunk = (int)(R.rate / 100) < CHUNK ? (int)(R.rate / 100) : CHUNK;
    if (!made) {
        const int per_row = (int)lrint(0.2 * R.rate / chunk);
        made = rdof_spectrum_init(&sp, RDOF_HOSTWEB_SPECTRUM_BINS, per_row > 0 ? per_row : 1) ? -1 : 1;
        if (made < 0) {
            say("web: no memory for the spectrum\n");
            return;
        }
    }
    uint8_t row[RDOF_HOSTWEB_SPECTRUM_BINS];
    if (rdof_spectrum_feed(&sp, in, n, row))
        rdof_hostweb_spectrum_row(row, RDOF_HOSTWEB_SPECTRUM_BINS, R.rx.center_hz, R.rate,
                                  (int)lrint(1e3 * sp.per_row * chunk / R.rate));
}

/* The SDR's card: the device, its samples and faults, the loop's time, the transmitter, and every receiver channel */
static void web_sdr(rdof_hostweb_sdr_t *d)
{
    const double now = now_s();
    snprintf(d->name, sizeof d->name, "%s", rcfg.mon_name[0] ? rcfg.mon_name : "sdr");
    snprintf(d->device, sizeof d->device, "%s", dev->name);
    d->open = true;
    d->receiving = WS.last_samples > 0 && now - WS.last_samples < 1.0;
    d->calibrating = R.cal != 0;
    d->rate = R.rate;
    d->center_hz = R.rx.center_hz;
    if (now - WS.rx_gain_at >= 2.0) { /* a device read (a network round trip on a remote libiio): not every time */
        WS.rx_gain = dev->rx_gain(dev);
        WS.rx_gain_at = now;
    }
    d->gain_db = WS.rx_gain;
    d->agc = WS.agc;
    d->samples_per_s = WS.sps;
    d->overflows = (uint64_t)WS.overflows, d->read_errors = (uint64_t)WS.errors;
    d->lost_ms = 1e3 * (double)WS.lost / R.rate;
    d->load = WS.load;
    if (WS.load >= 0) {
        static const char *stages[] = {"receiver", "spectrum", "modulator"};
        for (int i = 0; i < (R.split ? 2 : 3); i++) {
            snprintf(d->stage[d->nstage].name, sizeof d->stage[0].name, "%s", stages[i]);
            d->stage[d->nstage++].share = (float)WS.share[i];
        }
    }
    if (!R.split) { /* the split's radio transmits: the SDR receives only */
        d->tx = true;
        d->tx_hz = (uint32_t)R.tx_hz, d->tx_lo_hz = (uint32_t)llround(R.tx_lo);
        d->tx_gain_db = WS.tx_gain;
        d->tx_on = WS.tx_on;
        d->tx_errors = (uint64_t)W.errors;
    }
#ifdef RDOF_MQTT
    d->has_mqtt = MQ_SDR != NULL;
    d->mqtt = rdofmq_up(MQ_SDR);
    d->mqtt_dropped = rdofmq_dropped(MQ_SDR);
#endif
    for (int i = 0; i < nmon; i++)
        if (MON[i].peer) {
            d->hblink_peers++;
            d->hblink_up += MON[i].net.peer.up;
        }
    for (int c = 0; c < R.rx.C && d->nchan < 16; c++) {
        const char *id = R.rx.trackers[c].channel, *name = "";
        if (c == 0)
            name = "receive";
        else if (c == R.mon)
            name = "transmit (heard back)";
        for (int i = 0; i < nmon; i++)
            if (MON[i].c == c)
                name = MON[i].ch->name;
        const double f = R.rx.trackers[c].freq_hz;
        const int r = R.rx.rssi[c];
        snprintf(d->chan[d->nchan].id, sizeof d->chan[0].id, "%s", c == 0 ? rcfg.name : id);
        snprintf(d->chan[d->nchan].name, sizeof d->chan[0].name, "%s", name);
        d->chan[d->nchan].freq = (uint32_t)llround(f);
        d->chan[d->nchan].in = fabs(f - R.rx.center_hz) < R.rate / 2;
        d->chan[d->nchan].bursts = (uint64_t)R.rx.bursts[c];
        d->chan[d->nchan].rssi = r == INT32_MIN ? 0 : r;
        d->chan[d->nchan].afc_hz = (float)R.rx.chz.correction_hz[c];
        d->chan[d->nchan++].idle_pct = R.rx.chunks ? (float)(100.0 * (double)R.rx.gated[c] / (double)R.rx.chunks) : 0.0f;
    }
}

/* The split's TX half: the RadioFoundry radio on the port */
static void web_split_radio(rdof_hostweb_modem_t *m)
{
    const rdof_split_half_t *h = &S.halves[RDOF_SPLIT_TX];
    snprintf(m->name, sizeof m->name, "TX half");
    snprintf(m->kind, sizeof m->kind, "RadioFoundry radio");
    snprintf(m->role, sizeof m->role, "transmits (the split's TX half)");
    snprintf(m->port, sizeof m->port, "%s", P.path ? P.path : "");
    snprintf(m->version, sizeof m->version, "%s", h->description);
    snprintf(m->state, sizeof m->state, "%s", S.epoch_ok ? "epoch aligned" : S.ready ? "aligning" : "starting");
    if (P.gone)
        snprintf(m->error, sizeof m->error, "the port went away");
    else if (S.failed[0])
        snprintf(m->error, sizeof m->error, "%s", S.failed);
    m->open = !P.gone;
    m->ready = S.ready;
    const struct {
        const char *name;
        int64_t v;
    } x[] = {{"clock bound (us)", (int64_t)llround(rdof_clock_sync_bound_us(&h->sync))},
             {"misses", h->misses},
             {"split dropped", S.dropped},
             {"port bytes dropped", P.dropped},
             {"out dropped", h->overflow}};
    for (size_t i = 0; i < sizeof x / sizeof x[0]; i++) {
        snprintf(m->extra[m->nextra].name, sizeof m->extra[0].name, "%s", x[i].name);
        m->extra[m->nextra++].value = x[i].v;
    }
}

static void web_publish(double rx, double tx, int cc)
{
    static const char *pstate[] = {"idle", "recording", "waiting", "replaying"};
    rdof_hostweb_status_t w;
    memset(&w, 0, sizeof w);
    w.role = R.full ? RDOF_HOSTWEB_REPEATER : RDOF_HOSTWEB_TRANSCEIVER;
    snprintf(w.name, sizeof w.name, "%s", rcfg.name);
    const bool keyed = R.split ? R.keyed : R.m.keyed;
    if (R.split) {
        snprintf(w.radio, sizeof w.radio, "TX %s; RX %s", S.halves[RDOF_SPLIT_TX].description, dev->name);
        snprintf(w.modem, sizeof w.modem, "split duplex: the radio and the SDR modem");
    } else {
        snprintf(w.radio, sizeof w.radio, "%s", dev->name);
        snprintf(w.modem, sizeof w.modem, "SDR modem");
    }
    snprintf(w.state, sizeof w.state, "%s", keyed ? "keyed" : "idle");
    w.ready = R.split ? S.ready && !S.failed[0] : true;
    w.keyed = keyed;
    w.cc = cc;
    w.rx_hz = (uint32_t)rx, w.tx_hz = (uint32_t)tx;
    w.cpu_load = -1;
    w.bursts_heard = R.m.rx_bursts, w.bursts_sent = R.m.tx_bursts, w.dropped = R.m.lost;
    const rdof_tracker_t *tr = &R.rx.trackers[0];
    for (int i = 0; i < 2; i++) {
        const rdof_call_t *c = &tr->slots[i].call;
        w.slot[i].active = c->active;
        w.slot[i].src = c->src, w.slot[i].dst = c->dst, w.slot[i].group = c->group;
        if (c->active && c->bits > 0)
            w.slot[i].has_ber = true, w.slot[i].ber = (float)(100.0 * (double)c->errs / (double)c->bits);
        if (c->active && c->rssi_n > 0)
            w.slot[i].has_rssi = true, w.slot[i].rssi = (int)(c->rssi_sum / c->rssi_n);
    }
    if (R.parrot) {
        w.parrot_on = true;
        snprintf(w.parrot_state, sizeof w.parrot_state, "%s", pstate[R.parrot->state & 3]);
        w.parrot_slot = R.parrot->slot, w.parrot_id = R.parrot->id;
        w.parrot_src = R.parrot->src, w.parrot_dst = R.parrot->dst;
        w.parrot_replayed = R.parrot->replayed, w.parrot_ignored = R.parrot->ignored, w.parrot_cut = R.parrot->cut;
    }
    w.links = net_on;
#ifdef RDOF_MQTT
    w.links = net_on || MQ;
    w.has_mqtt = MQ != NULL;
    w.mqtt = rdofmq_up(MQ);
    w.mqtt_dropped = rdofmq_dropped(MQ);
#endif
    w.hblink = net_on && NET.peer.up;
    w.hblink_tx = NET.peer.tx, w.hblink_rx = NET.peer.rx;
    for (int i = 0; i < nmon && w.nchan < 16; i++) {
        const mon_t *m = &MON[i];
        const int r = R.rx.rssi[m->c];
        snprintf(w.chan[w.nchan].id, sizeof w.chan[w.nchan].id, "%s", m->ch->id);
        w.chan[w.nchan].freq = (uint32_t)llround(m->ch->freq);
        w.chan[w.nchan].bursts = R.rx.bursts[m->c];
        w.chan[w.nchan].rssi = r == INT32_MIN ? 0 : r;
        w.chan[w.nchan].peer = m->peer;
        w.chan[w.nchan++].up = m->peer && m->net.peer.up;
    }
    w.nextra = R.split ? 0 : 2; /* the TX half's timing is the radio's own */
    snprintf(w.extra[0].name, sizeof w.extra[0].name, "tx late (us)");
    w.extra[0].value = R.m.tx_late48 * 1000 / 48;
    snprintf(w.extra[1].name, sizeof w.extra[1].name, "inbound off (%%)");
    w.extra[1].value = (int64_t)llround(R.m.inbound_off * 100);
    w.spectrum = true;
    web_sdr(&w.sdrs[w.nsdr++]);
    if (R.split)
        web_split_radio(&w.modems[w.nmodem++]);
    rdof_hostweb_status(&w);
}

/* The page's Settings tab: the --config file (editable), or the flags as a document (read only). A save checks
 * the document as a start would, writes the file whole, and restarts: the main loop stops, the transmitter goes
 * down as at any exit, and the program starts again with the same command line. */
static volatile sig_atomic_t restart_asked;
static char web_device[512]; /* the command line, for /api/resolved */

static char *web_get(void *ctx)
{
    size_t len;
    if (config_path)
        return read_file(config_path, &len);
    char *t = malloc(1024);
    if (t)
        rdofradio_cfg_from_flags(t, 1024, dev_iio ? "iio" : "soapy", rcfg.full, rcfg.rx_hz, rcfg.tx_hz, rcfg.cc,
                                 rcfg.has_parrot, rcfg.parrot, rcfg.peer_id);
    return t;
}
static bool web_save(void *ctx, const char *text, size_t len, char *err, size_t cap)
{
    rdofradio_cfg_t *c = malloc(sizeof *c);
    if (!c) {
        snprintf(err, cap, "out of memory");
        return false;
    }
    bool ok = rdofradio_cfg_parse(text, len, c, err, cap);
    if (ok) { /* the receive channels in one window, as the start will ask: the radio's RX and the monitored */
        rdof_sdr_channel_t ch[RDOF_SDR_MAX_CHANNELS];
        int n = 0;
        ch[n++] = (rdof_sdr_channel_t){"rx", (double)c->rx_hz, c->cc, c->rx_slot};
        for (int i = 0; i < c->nmon && n < RDOF_SDR_MAX_CHANNELS; i++)
            ch[n++] = (rdof_sdr_channel_t){c->mon[i].id, c->mon[i].freq, c->mon[i].cc, c->mon[i].slot};
        ok = plan_rate(ch, n, err, cap) > 0;
    }
    ok = ok && rdof_hostweb_save_file(config_path, text, len, err, cap);
    free(c);
    return ok;
}
static size_t web_resolved(void *ctx, char *out, size_t cap)
{
    if (held[0]) {
        const int n = snprintf(out, cap, "not running: %s\n\nFix the settings and save: it starts again.\n", held);
        return n < 0 ? 0 : (size_t)n < cap ? (size_t)n : cap - 1;
    }
    char src[300];
    if (config_path)
        snprintf(src, sizeof src, "file %s", config_path);
    else
        snprintf(src, sizeof src, "command line flags");
    size_t n = rdofradio_cfg_describe(&rcfg, src, web_device, out, cap);
    if (n < cap - 64) {
        n += (size_t)snprintf(out + n, cap - n, "\nthe site, as it runs (paths, policies, talkgroups):\n");
        n += rdof_site_resolved_text(&rcfg.site, out + n, cap - n);
    }
    return n;
}
static void web_restart(void *ctx)
{
    restart_asked = 1;
    stop = 1;
}
static void web_settings(int argc, char **argv)
{
    size_t n = 0;
    for (int i = 1; i < argc && n < sizeof web_device; i++)
        n += (size_t)snprintf(web_device + n, sizeof web_device - n, "%s%s", i > 1 ? " " : "", argv[i]);
    rdof_hostweb_settings_t s = {0};
    s.writable = config_path != NULL;
    if (config_path)
        snprintf(s.source, sizeof s.source, "file %s", config_path);
    else
        snprintf(s.source, sizeof s.source, "command line flags (start with --config FILE to edit them here)");
    s.get = web_get;
    s.save = web_save;
    s.resolved = web_resolved;
    s.restart = web_restart;
    rdof_hostweb_settings(&s);
}
#endif

/* ── --split-tx: the radio transmits, the SDR is the split's RX half ─────────── */


static void *port_reader(void *arg)
{
    while (!P.quit) {
        struct pollfd pf = {P.fd, POLLIN, 0};
        if (poll(&pf, 1, 100) <= 0)
            continue;
        uint8_t b[256];
        const int n = rdofradio_serial_read(P.fd, b, sizeof b);
        const double t = now_s();
        if (n < 0 || (pf.revents & (POLLHUP | POLLERR | POLLNVAL))) {
            P.gone = true;
            break;
        }
        if (n == 0)
            continue;
        pthread_mutex_lock(&P.mu);
        if (P.head - P.tail < PORT_Q) {
            const int i = P.head++ % PORT_Q;
            P.q[i].t = t, P.q[i].n = n;
            memcpy(P.q[i].b, b, (size_t)n);
        } else {
            P.dropped += (uint32_t)n;
        }
        pthread_mutex_unlock(&P.mu);
    }
    return NULL;
}

/* The receiver on a second core: the loop runs its share of the forward FFTs and of the channels (channelizer and
 * demod), a helper the rest, and every burst then goes to the sinks (the modem, the bench, the network) on the loop's
 * thread. A PlutoSDR's loop had one of its two cores to itself and fell behind the stream while a repeater job
 * transmitted and strong calls kept every channel's demod busy. */
static struct {
    pthread_t th;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int job, c0, c1; /* 1: forward FFTs of blocks c0 to c1; 2: channels c0 to c1, channelizer and demod; 3: demod */
    bool go, on, failed;
} HX = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};

static void *chz_helper(void *arg)
{
    for (;;) {
        pthread_mutex_lock(&HX.mu);
        while (!HX.go)
            pthread_cond_wait(&HX.cv, &HX.mu);
        const int job = HX.job, c0 = HX.c0, c1 = HX.c1;
        pthread_mutex_unlock(&HX.mu);
        if (job == 1)
            rdof_chz_forward_blocks(&R.rx.chz, c0, c1, 1);
        if (job == 2)
            rdof_chz_channels(&R.rx.chz, c0, c1, 1, R.rx.y, R.rx.stride);
        if (job >= 2)
            rdof_sdr_rx_feed(&R.rx, c0, c1);
        pthread_mutex_lock(&HX.mu);
        HX.go = false;
        pthread_cond_broadcast(&HX.cv);
        pthread_mutex_unlock(&HX.mu);
    }
    return NULL;
}

/* the helper's job, while this thread does its share */
static void hx_run(int job, int c0, int c1)
{
    pthread_mutex_lock(&HX.mu);
    HX.job = job, HX.c0 = c0, HX.c1 = c1, HX.go = true;
    pthread_cond_broadcast(&HX.cv);
    pthread_mutex_unlock(&HX.mu);
}

static void hx_join(void)
{
    pthread_mutex_lock(&HX.mu);
    while (HX.go)
        pthread_cond_wait(&HX.cv, &HX.mu);
    pthread_mutex_unlock(&HX.mu);
}

/* rdof_sdr_rx_process shared with the helper: half the forward FFTs, then two thirds of the channels (the loop has
 * the rest of its work), then the bursts */
static void rx_chunk(const rdof_cf_t *in, int n)
{
    if (!HX.on && !HX.failed && R.rx.C > 1) {
        HX.on = pthread_create(&HX.th, NULL, chz_helper, NULL) == 0;
        HX.failed = !HX.on;
    }
    if (!HX.on) {
        rdof_sdr_rx_forward(&R.rx, in, n);
        if (!R.rx.fused)
            rdof_chz_channels(&R.rx.chz, 0, R.rx.C, 0, R.rx.y, R.rx.stride);
    } else {
        rdof_sdr_rx_gather(&R.rx, in, n);
    }
    const int split = R.rx.C / 3 > 0 ? R.rx.C / 3 : 1;
    if (HX.on && !R.rx.fused) {
        const int nb = R.rx.chz.nblk, half = (nb + 1) / 2;
        hx_run(1, half, nb);
        rdof_chz_forward_blocks(&R.rx.chz, 0, half, 0);
        hx_join();
        hx_run(2, split, R.rx.C);
        rdof_chz_channels(&R.rx.chz, 0, split, 0, R.rx.y, R.rx.stride);
        rdof_sdr_rx_feed(&R.rx, 0, split);
        hx_join();
    } else if (HX.on) {
        hx_run(3, split, R.rx.C);
        rdof_sdr_rx_feed(&R.rx, 0, split);
        hx_join();
    } else {
        rdof_sdr_rx_feed(&R.rx, 0, R.rx.C);
    }
    rdof_sdr_rx_bursts(&R.rx, 0, R.rx.C);
    rdof_sdr_rx_finish(&R.rx);
}

/* The receive stream against the wall clock: lag = wall-clock samples - samples read. Its least over half a second
 * is the freshest read; floor follows it (a crystal's ppm moves it slowly). A floor that stays over by more than
 * 2 ms for a second is samples the device dropped: zeros go to the receiver in their place. Only a window with a
 * read that waited (FRESH_S) counts: one the loop spent behind runs on until it catches up. */
static struct {
    bool win_fresh;
    double t0, rate, floor, win_start, win_min, last_win;
    long long total, filled;
    int chunk, timeouts, errors, overflows;
} SC;

static void clock_read(double t, bool fresh)
{
    const double lag = (t - SC.t0) * SC.rate - (double)SC.total;
    if (lag < SC.win_min)
        SC.win_min = lag;
    SC.win_fresh |= fresh;
    if (t - SC.win_start < 0.5 || !SC.win_fresh)
        return;
    const double w = SC.win_min, thr = SC.rate / 500;
    SC.win_start = t, SC.win_min = INFINITY, SC.win_fresh = false;
    if (!isfinite(SC.floor) || w < SC.floor + thr) {
        SC.floor = w;
    } else if (SC.last_win >= SC.floor + thr) {
        static rdof_cf_t zeros[CHUNK];
        long long lost = llround(fmin(w, SC.last_win) - SC.floor);
        say("receive: %.1f ms of samples lost; zeros in their place\n", 1e3 * (double)lost / SC.rate);
        SC.filled += lost;
        WS.lost += lost;
        SC.total += lost;
        for (; lost > 0; lost -= CHUNK) {
            bench_feed(zeros, lost < CHUNK ? (int)lost : CHUNK, false, true);
            rx_chunk(zeros, lost < CHUNK ? (int)lost : CHUNK);
        }
    }
    SC.last_win = w;
}

/* the SDR modem's 48 kS/s position now: where the freshest read puts the stream */
static int64_t split_now48(void *ctx)
{
    if (!isfinite(SC.floor))
        return 0; /* not yet: the modem keeps the latest position it has seen */
    return (int64_t)(((now_s() - SC.t0) * SC.rate - SC.floor) * 48000.0 / SC.rate);
}

/* What the split has for the halves and the client, until nothing moves (the RX half and the client answer at
 * once, which may give it more). */
static void split_flush(void)
{
    static bool busy, again;
    if (busy) {
        again = true;
        return;
    }
    busy = true;
    uint8_t buf[1024];
    size_t n;
    bool moved;
    do {
        moved = again = false;
        while ((n = rdof_split_take(&S, RDOF_SPLIT_TX, buf, sizeof buf)) > 0) {
            if (!P.gone && !rdofradio_serial_write(P.fd, buf, n))
                P.gone = true;
            moved = true;
        }
        while ((n = rdof_split_take(&S, RDOF_SPLIT_RX, buf, sizeof buf)) > 0) {
            rdof_sdrmodem_write(&R.m, buf, n);
            moved = true;
        }
        while ((n = rdof_split_take(&S, -1, buf, sizeof buf)) > 0) {
            rdof_modem_feed(&R.client, buf, n);
            moved = true;
        }
    } while (moved || again);
    busy = false;
}

static void split_service(void)
{
    for (;;) {
        pthread_mutex_lock(&P.mu);
        if (P.tail == P.head) {
            pthread_mutex_unlock(&P.mu);
            break;
        }
        const int i = P.tail++ % PORT_Q;
        uint8_t b[256];
        const int n = P.q[i].n;
        const double t = P.q[i].t;
        memcpy(b, P.q[i].b, (size_t)n);
        pthread_mutex_unlock(&P.mu);
        rdof_split_from_half(&S, RDOF_SPLIT_TX, b, (size_t)n, t);
    }
    rdof_split_tick(&S, now_s());
    split_flush();
    if (!stop && (P.gone || S.failed[0])) {
        say("split: %s\n", P.gone ? "the radio's port went away" : S.failed);
        stop = 1;
    }
    if (!in_wait)
        deliver_heard();
}

/* a chunk of the receive stream through the receiver, then the split */
static void split_turn(void)
{
    static rdof_cf_t in[CHUNK];
    const double t_in = now_s();
    const int n = dev->read(dev, in, SC.chunk, 100000);
    const double t = now_s();
    WS.wait += t - t_in;
    if (n == RDOF_DEV_TIMEOUT) {
        if (++SC.timeouts >= 50 && !stop) {
            say("read: nothing for 5 s, stopping\n");
            stop = 1;
        }
    } else if (n == RDOF_DEV_OVERFLOW) {
        SC.overflows++;
        WS.overflows++;
    } else if (n < 0) {
        WS.errors++;
        if (++SC.errors > 10 && !stop) {
            say("%s\n", dev->error);
            stop = 1;
        }
    } else if (n > 0) {
        SC.timeouts = 0;
        SC.total += n;
        WS.last_samples = t;
        WS.n += n;
        clock_read(t, t - t_in >= FRESH_S);
#ifdef RDOF_HOST_WEB
        web_spectrum(in, n);
#endif
        bench_feed(in, n, t - t_in >= FRESH_S, false);
        const double t3 = now_s();
        rx_chunk(in, n);
        const double t4 = now_s();
        WS.spec += t3 - t, WS.rx += t4 - t3;
    }
    split_service();
    ws_turn(now_s());
}

/* The split's whole run, once the device is open (receive only) and the SDR modem made. */
static int split_main(int argc, char **argv, const char *port, int baud, int power_cap, double seconds)
{
    char err[256];
    P.path = port;
    P.fd = rdofradio_serial_open(port, baud, err, sizeof err);
    if (P.fd < 0) {
        fprintf(stderr, "--split-tx %s\n", err);
        return 1;
    }
    if (pthread_create(&P.thread, NULL, port_reader, NULL)) {
        fprintf(stderr, "the port reader: no thread\n");
        return 1;
    }
    SC.rate = R.rate;
    SC.floor = SC.last_win = SC.win_min = INFINITY;
    SC.chunk = (int)(R.rate / 100) < CHUNK ? (int)(R.rate / 100) : CHUNK;
    int rc = 1;
    if (dev->start(dev)) {
        fprintf(stderr, "start: %s\n", dev->error);
        goto out;
    }
    SC.t0 = SC.win_start = now_s();

    /* bring-up: both halves' versions, INFO and roles, eight TIME rounds each, the first epoch */
    rdof_split_params_t sp;
    rdof_split_params_default(&sp);
    sp.power_cap = power_cap;
    rdof_split_init(&S, now_s(), &sp);
    S.log = say_log;
    split_flush();
    const double t_up = now_s();
    while (!stop && !S.ready && !S.failed[0] && now_s() - t_up < 15)
        split_turn();
    if (!S.ready) {
        fprintf(stderr, "split: %s\n", S.failed[0] ? S.failed : P.gone ? "the radio's port went away" : "no answer in 15 s");
        goto out;
    }

    /* the client and the repeater over the split, as over any duplex modem */
    const rdof_modem_port_t mp = {p_write, p_wait, NULL, p_clock, p_sleep, &R};
    rdof_modem_init(&R.client, &mp);
    R.client.on_rx = on_rx;
    R.client.on_lost = on_lost;
    R.client.on_nak = on_nak;
    R.client.cb_ctx = &R;
    if (!rdof_modem_start(&R.client, 1)) {
        fprintf(stderr, "modem: %s\n", R.client.error);
        goto out;
    }
    rdof_radio_init(&R.radio, &R.client, rdof_radio_detect(R.client.version));
    rdof_repeater_init(&R.r, &R.radio, rcfg.peer_id, R.rx_hz, R.tx_hz, R.cc);
    if (config_path)
        rdofradio_cfg_apply_repeater(&rcfg, &R.r);
    R.r.clock = p_clock;
    R.r.on_tx = on_tx;
    R.r.on_sent = on_sent;
    R.r.on_key = on_key;
    R.r.ctx = &R;
    if (!rdof_repeater_start(&R.r)) {
        fprintf(stderr, "repeater start: %s\n", R.client.error);
        goto out;
    }
    deliver_heard();
    fprintf(stderr, "split repeater: TX %s on %s; RX %s, %.4f MHz (center %.4f), %.0f S/s; TX %.4f MHz, cc %d\n",
            S.halves[RDOF_SPLIT_TX].description, port, dev->name, R.rx_hz / 1e6, R.rx.center_hz / 1e6, dev->rate,
            R.tx_hz / 1e6, R.cc);
#ifdef RDOF_HOST_WEB
    if (web_port >= 0) {
        web_settings(argc, argv);
        const int wp = rdof_hostweb_start(web_addr, web_port, rcfg.box);
        if (wp < 0)
            fprintf(stderr, "web: %s\n", strerror(errno));
        else
            fprintf(stderr, "web: http://%s:%d/\n", web_addr, wp);
    }
    double last_web = 0;
#endif
    bench_start();
    double last_poll = 0, last_status = now_s();
    while (!stop && (seconds <= 0 || SC.total < (long long)(seconds * R.rate))) {
        split_turn();
        const double now = now_s();
        if (now - last_poll >= POLL_S) {
            last_poll = now;
            if (R.parrot)
                rdof_parrot_poll(R.parrot, now);
            if (!rdof_repeater_poll(&R.r))
                fprintf(stderr, "poll: %s\n", R.client.error);
            net_poll(now);
            mq_poll(now);
            deliver_heard();
        }
#ifdef RDOF_HOST_WEB
        if (now - last_web >= 0.25) {
            last_web = now;
            web_publish(R.rx_hz, R.tx_hz, R.cc);
        }
#endif
        if (R.verbose && now - last_status >= 1.0) {
            last_status = now;
            const rdof_split_half_t *tx = &S.halves[RDOF_SPLIT_TX];
            fprintf(stderr, "status %.1f s: %s, epoch %s, TX half bound %.0f us, SDR %u heard (%u TS1, %u TS2, %u unlabeled)\n",
                    SC.total / R.rate, R.keyed ? "keyed" : "idle", S.epoch_ok ? "ok" : "not yet",
                    rdof_clock_sync_bound_us(&tx->sync), R.m.rx_bursts, R.m.labeled[0], R.m.labeled[1], R.m.unlabeled);
        }
    }
    rdof_repeater_stop(&R.r);
    for (int i = 0; i < 10; i++) /* the repeater's last frames out to the radio */
        split_service();
    rc = 0;
out:
    P.quit = true;
    pthread_join(P.thread, NULL);
    close(P.fd);
    dev->close(dev);
    fprintf(stderr, "%.1f s (%.1f s wall), %d overflows, %.1f ms of samples lost; SDR modem: %u heard (%u TS1, %u TS2, "
                    "%u unlabeled), %u lost; split: %u dropped, port %u bytes dropped\n",
            SC.total / R.rate, SC.t0 ? now_s() - SC.t0 : 0.0, SC.overflows, 1e3 * (double)SC.filled / R.rate,
            R.m.rx_bursts, R.m.labeled[0], R.m.labeled[1], R.m.unlabeled, R.m.lost, S.dropped, P.dropped);
    if (R.parrot)
        say("parrot: %u sent back, %u not to it, %u bursts cut\n", R.parrot->replayed, R.parrot->ignored,
            R.parrot->cut);
    bench_stop();
    mq_stop();
    net_close();
    clock_stop();
    rdof_sdr_rx_free(&R.rx);
#ifdef RDOF_HOST_WEB
    rdof_hostweb_stop();
#endif
    rdof_sdrmodem_free(&R.m);
#ifdef RDOF_HOST_WEB
    if (rc == 0 && restart_asked) {
        fprintf(stderr, "restarting (asked for from the web page)\n");
        rdof_hostweb_reexec(argv);
        fprintf(stderr, "restart: %s\n", strerror(errno));
        return 1;
    }
#endif
    return rc;
}

/* Settings that can't run (refused, or channels no window holds): with --config and --web, the page stays up
 * with the reason, so the file can be fixed there; a save, or the file changing on disk, starts again. Without
 * them, exits 2 (a supervisor that restarts it gets the same reason each time). */
static int hold(int argc, char **argv, const char *why)
{
    fprintf(stderr, "%s\n", why);
#ifdef RDOF_HOST_WEB
    if (web_port < 0 || !config_path)
        return 2;
    snprintf(held, sizeof held, "%s", why);
    struct stat st0;
    const bool had = stat(config_path, &st0) == 0;
    web_settings(argc, argv);
    const int port = rdof_hostweb_start(web_addr, web_port, rcfg.box[0] ? rcfg.box : "rdofradio");
    if (port < 0) {
        fprintf(stderr, "web: %s\n", strerror(errno));
        return 2;
    }
    fprintf(stderr, "web: http://%s:%d/ (not running: fix the settings there)\n", web_addr, port);
    rdof_hostweb_logf("not running: %s", why);
    rdof_hostweb_version(RDOF_VERSION);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    while (!stop) {
        rdof_hostweb_status_t w;
        memset(&w, 0, sizeof w);
        w.role = rcfg.full ? RDOF_HOSTWEB_REPEATER : RDOF_HOSTWEB_TRANSCEIVER;
        snprintf(w.name, sizeof w.name, "%s", rcfg.name[0] ? rcfg.name : "rdofradio");
        snprintf(w.state, sizeof w.state, "not running");
        snprintf(w.error, sizeof w.error, "%s", why);
        w.cpu_load = -1;
        rdof_hostweb_status(&w);
        struct stat st;
        if (had && stat(config_path, &st) == 0 &&
            (st.st_mtime != st0.st_mtime || st.st_size != st0.st_size)) {
            fprintf(stderr, "%s changed: starting again\n", config_path);
            restart_asked = 1;
            break;
        }
        usleep(500000);
    }
    rdof_hostweb_stop();
    if (restart_asked) {
        rdof_hostweb_reexec(argv);
        fprintf(stderr, "restart: %s\n", strerror(errno));
        return 1;
    }
    return 0;
#else
    (void)argc, (void)argv;
    return 2;
#endif
}

int main(int argc, char **argv)
{
    const char *soapy = NULL, *iio = NULL, *stream_args = "", *fft = NULL, *split_tx = NULL;
    int baud = 115200, power_cap = 0, tx_flags = 0;
    double bw = 0, rate = 0, rx = 0, tx = 0, tx_lo = 0, center = 0, gain = NAN, txg = 0, seconds = 0, offset_s = 0;
    int cc = 1, rx_slot = 1, allow = 0, monitor = 0, verbose = 0;
    const char *duplex = "full";
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
#define NEXT()                                                                                                    \
    do {                                                                                                          \
        if (!v)                                                                                                   \
            usage();                                                                                              \
        i++;                                                                                                      \
    } while (0)
        if (!strcmp(a, "--soapy")) {
            NEXT();
            soapy = v;
        } else if (!strcmp(a, "--iio")) {
            NEXT();
            iio = v;
        } else if (!strcmp(a, "--split-tx")) {
            NEXT();
            split_tx = v;
        } else if (!strcmp(a, "--baud")) {
            NEXT();
            baud = atoi(v);
        } else if (!strcmp(a, "--split-power-cap")) {
            NEXT();
            power_cap = atoi(v);
            if (power_cap < 1 || power_cap > 255)
                usage();
        } else if (!strcmp(a, "--fft")) {
            NEXT();
            fft = v;
        } else if (!strcmp(a, "--config")) {
            NEXT();
            config_path = v;
        } else if (!strcmp(a, "-r") || !strcmp(a, "--rate")) {
            NEXT();
            rate = atof(v);
        } else if (!strcmp(a, "--rx")) {
            NEXT();
            rx = mhz(v);
        } else if (!strcmp(a, "--tx")) {
            NEXT();
            tx = mhz(v);
        } else if (!strcmp(a, "--tx-lo")) {
            NEXT();
            tx_lo = mhz(v);
            tx_flags++;
        } else if (!strcmp(a, "--center")) {
            NEXT();
            center = mhz(v);
        } else if (!strcmp(a, "--duplex")) {
            NEXT();
            duplex = v;
        } else if (!strcmp(a, "--cc")) {
            NEXT();
            cc = atoi(v);
        } else if (!strcmp(a, "--gain")) {
            NEXT();
            gain = atof(v);
        } else if (!strcmp(a, "--parrot")) {
            NEXT();
            static rdof_parrot_t parrot;
            rdof_parrot_init(&parrot, (uint32_t)strtoul(v, NULL, 10));
            parrot.start = parrot_start;
            parrot.send = parrot_send;
            R.parrot = &parrot;
        } else if (!strcmp(a, "--stream")) {
            NEXT();
            stream_args = v;
        } else if (!strcmp(a, "--bw")) {
            NEXT();
            bw = mhz(v);
        } else if (!strcmp(a, "--tx-gain")) {
            NEXT();
            txg = atof(v);
            tx_flags++;
        } else if (!strcmp(a, "--tx-offset")) {
            NEXT();
            offset_s = atof(v);
            tx_flags++;
        } else if (!strcmp(a, "--seconds")) {
            NEXT();
            seconds = atof(v);
        } else if (!strcmp(a, "--allow-tx")) {
            allow = 1;
        } else if (!strcmp(a, "--monitor")) {
            monitor = 1;
            tx_flags++;
        } else if (!strcmp(a, "--clock-state")) {
            NEXT();
            clock_state = v;
        } else if (!strcmp(a, "--set-os-clock")) {
            set_os_clock = true;
        } else if (!strcmp(a, "--bursts")) {
            show_bursts = 1;
        } else if (!strcmp(a, "--quiet")) {
            quiet = 1;
        } else if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) {
            verbose = 1;
            R.verbose = true;
        } else if (!strcmp(a, "--bench")) {
            NEXT();
            const char *colon = strrchr(v, ':');
            if (colon) {
                snprintf(bench_addr, sizeof bench_addr, "%.*s", (int)(colon - v), v);
                v = colon + 1;
            }
            bench_port = atoi(v);
        } else if (!strcmp(a, "--bench-tx")) {
            NEXT();
            for (const char *t = v; *t && bench_ntx < RDOF_BENCHTX_MAX_FREQS;) {
                bench_tx_mhz[bench_ntx++] = mhz(t) / 1e6;
                const char *comma = strchr(t, ',');
                t = comma ? comma + 1 : t + strlen(t);
            }
#ifdef RDOF_HOST_WEB
        } else if (!strcmp(a, "--web")) {
            NEXT();
            const char *colon = strrchr(v, ':');
            if (colon) {
                snprintf(web_addr, sizeof web_addr, "%.*s", (int)(colon - v), v);
                v = colon + 1;
            }
            web_port = atoi(v);
        } else if (!strcmp(a, "--last-exit")) {
            NEXT();
            rdof_hostweb_last_exit(v);
#endif
        } else {
            usage();
        }
    }
    if (config_path) {
        /* the radio from the file; the command line keeps the device */
        for (int i = 1; i < argc; i++) {
            static const char *const radio_flags[] = {"--rx", "--tx", "--cc", "--duplex", "--parrot", NULL};
            for (int k = 0; radio_flags[k]; k++)
                if (!strcmp(argv[i], radio_flags[k])) {
                    fprintf(stderr, "%s: the radio's settings are in %s with --config\n", argv[i], config_path);
                    return 2;
                }
        }
        size_t len = 0;
        char *text = read_file(config_path, &len), err[200];
        if (!text) {
            fprintf(stderr, "%s: %s\n", config_path, strerror(errno));
            return 2;
        }
        const bool ok = rdofradio_cfg_parse(text, len, &rcfg, err, sizeof err);
        free(text);
        if (!ok) {
            char why[300];
            snprintf(why, sizeof why, "%s: %s", config_path, err);
            return hold(argc, argv, why);
        }
        rx = rcfg.rx_hz, tx = rcfg.tx_hz, cc = rcfg.cc, rx_slot = rcfg.rx_slot;
        duplex = rcfg.full ? "full" : "half";
        if (rcfg.has_parrot) {
            static rdof_parrot_t parrot;
            rdof_parrot_init(&parrot, rcfg.parrot);
            parrot.start = parrot_start;
            parrot.send = parrot_send;
            parrot.admit = parrot_admit;
            R.parrot = &parrot;
        }
    }
    R.full = !strcmp(duplex, "full");
    if (!soapy == !iio || !rx || (!R.full && strcmp(duplex, "half")))
        usage();
#ifndef RDOF_SOAPY
    if (soapy) {
        fprintf(stderr, "rdofradio was built without SoapySDR (RDOF_SOAPY)\n");
        return 2;
    }
#endif
#ifndef RDOF_IIO
    if (iio) {
        fprintf(stderr, "rdofradio was built without libiio (RDOF_IIO)\n");
        return 2;
    }
#endif
    dev_iio = iio != NULL;
    dev_local = iio && !strncmp(iio, "local:", 6);
    rate_flag = rate;
#ifdef RDOF_PFFFT
    if (!fft || !strcmp(fft, "pffft"))
        rdof_fft_set_backend(rdof_pffft_backend());
    else if (strcmp(fft, "builtin"))
        usage();
#else
    if (fft && strcmp(fft, "builtin")) {
        fprintf(stderr, "rdofradio was built without pffft (RDOF_PFFFT)\n");
        return 2;
    }
#endif
    if (!allow) {
        fprintf(stderr, "rdofradio transmits: pass --allow-tx\n");
        return 2;
    }
    R.split = split_tx != NULL;
    if (R.split && !R.full) {
        fprintf(stderr, "--split-tx runs a repeater: full duplex\n");
        return 2;
    }
    if (R.split && tx_flags) {
        fprintf(stderr, "--split-tx: the radio transmits; --tx-gain, --tx-lo, --tx-offset and --monitor are the SDR's\n");
        return 2;
    }
    if (!tx)
        tx = rx;
    if (!tx_lo)
        tx_lo = tx + 100e3;
    R.rate = rate;
    R.rx_hz = (uint32_t)llround(rx);
    R.tx_hz = (uint32_t)llround(tx);
    R.tx_lo = tx_lo;
    R.cc = cc;
    R.mon = -1;
    if (!config_path) {
        /* the flags as a settings document: the page shows it, and it starts a file for --config */
        char doc[1024], err[200];
        rdofradio_cfg_from_flags(doc, sizeof doc, dev_iio ? "iio" : "soapy", R.full, R.rx_hz, R.tx_hz, cc,
                                 R.parrot != NULL, R.parrot ? R.parrot->id : 0, 9990601);
        if (!rdofradio_cfg_parse(doc, strlen(doc), &rcfg, err, sizeof err)) {
            fprintf(stderr, "the command line's radio: %s\n", err);
            return 2;
        }
        rcfg.has_parrot = R.parrot != NULL; /* --parrot 0 (everything) is no parrot app */
        rcfg.parrot = R.parrot ? R.parrot->id : 0;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* the receiver: the RX channel, with --monitor the TX channel (our own outbound, heard back), then the
       monitor roles' channels on this device */
    CH[0] = (rdof_sdr_channel_t){"rx", rx, cc, rx_slot};
    CH[1] = (rdof_sdr_channel_t){"out", tx, cc, 1};
    NCH = monitor && R.tx_hz != R.rx_hz ? 2 : 1;
    if (NCH == 2 || (monitor && R.tx_hz == R.rx_hz))
        R.mon = NCH - 1;
    for (int i = 0; i < rcfg.nmon && NCH < RDOF_SDR_MAX_CHANNELS; i++) {
        const rdof_sdr_cfg_channel_t *m = &rcfg.mon[i];
        MON[nmon].c = NCH;
        MON[nmon++].ch = m;
        CH[NCH++] = (rdof_sdr_channel_t){m->id, m->freq, m->cc, m->slot};
    }
    if (nmon < rcfg.nmon)
        fprintf(stderr, "receiver: %d channels at most: %d monitored channels left out\n", RDOF_SDR_MAX_CHANNELS,
                rcfg.nmon - nmon);
    {
        char why[200];
        const double fit = plan_rate(CH, NCH, why, sizeof why);
        if (!fit)
            return hold(argc, argv, why);
        if (!rate)
            fprintf(stderr, "receiver: %.0f S/s (%d channels over %.4f MHz)\n", fit, NCH, span_of(CH, NCH) / 1e6);
        rate = fit;
        R.rate = rate;
    }
    const rdof_sdr_rx_config_t rc = {rate, center, -55.0, 0.0, true, CHUNK};
    rdof_sink_t sink = {.on_json = on_json};
#ifdef RDOF_MQTT
    if (rcfg.cfg.has_mqtt)
        sink.on_raw_burst = on_raw_burst; /* every burst on .../raw, as the board's */
#endif
    if (rdof_sdr_rx_init(&R.rx, &rc, CH, NCH, &sink, (double)time(NULL))) {
        char why[200];
        snprintf(why, sizeof why, "receiver: the rate isn't a multiple of 48 kS/s, or the channels don't fit one "
                                  "window%s", nmon ? " (the monitored channels with the radio's: a wider --rate, or fewer)" : "");
        return hold(argc, argv, why);
    }
    R.rx.on_burst = on_burst;
    R.rx.muted = muted;
    R.rx.relabel = relabel; /* the tracker's JSON (calls, BER, RSSI) with the inbound slot, not slot 1 */
    R.rx.drop = echo_drop;
    R.tx_ch = R.tx_img = R.echo_ch = R.echo_img = -1;
    R.rx.burst_ctx = &R;
    sr_start();
    net_start();
    mon_start();
    mq_start();
    clock_start();
#ifdef RDOF_HOST_WEB
    rdof_hostweb_version(RDOF_VERSION);
#endif

    /* the device: RX and TX at one rate, each on its own LO, the TX gain at its least */
    const rdofradio_dev_setup_t su = {rate, R.rx.center_hz, tx_lo, gain, bw, stream_args, R.split};
    char derr[256] = "";
#ifdef RDOF_IIO
    if (iio)
        dev = rdofradio_iio_open(iio, &su, derr, sizeof derr);
#endif
#ifdef RDOF_SOAPY
    if (soapy)
        dev = rdofradio_soapy_open(soapy, &su, derr, sizeof derr);
#endif
    if (!dev) {
        fprintf(stderr, "%s\n", derr);
        return 1;
    }
    WS.agc = isnan(gain);
    WS.tx_gain = dev->tx_gain_min;
    if (!R.split && (txg < dev->tx_gain_min || txg > dev->tx_gain_max)) {
        fprintf(stderr, "TX gain %.1f outside the device's %.1f..%.1f\n", txg, dev->tx_gain_min, dev->tx_gain_max);
        dev->close(dev);
        return 1;
    }
    const double got = dev->rate;
    if (fabs(got - rate) > 2.0)
        fprintf(stderr, "%s: the device runs at %.1f S/s, not %.0f\n", dev->name, got, rate);
    /* the loop's turn: 10 ms of samples, so the repeater's 20 ms poll keeps its time at any rate */
    int chunk = (int)(rate / 100);
    if (chunk > CHUNK)
        chunk = CHUNK;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* the modem, its client, and the repeater or the transceiver over them */
    const rdof_sdrmodem_io_t io = {io_reply, io_channel, R.split ? NULL : io_tune, &R, R.split ? split_now48 : NULL};
    if (rdof_sdrmodem_init(&R.m, &io, (uint32_t)rate, (uint32_t)rate, !R.split)) {
        fprintf(stderr, "modem: %s\n", R.m.error);
        return 1;
    }
    if (R.split) {
        fprintf(stderr, "FFT: %s\n",
#ifdef RDOF_PFFFT
                fft && !strcmp(fft, "builtin") ? "built-in" : "pffft"
#else
                "built-in"
#endif
        );
        return split_main(argc, argv, split_tx, baud, power_cap, seconds);
    }
    const rdof_modem_port_t port = {p_write, p_wait, NULL, p_clock, p_sleep, &R};
    rdof_modem_init(&R.client, &port);
    R.client.on_rx = on_rx;
    R.client.on_lost = on_lost;
    R.client.on_nak = on_nak;
    R.client.cb_ctx = &R;
    if (!rdof_modem_start(&R.client, 1)) {
        fprintf(stderr, "modem: %s\n", R.client.error);
        return 1;
    }
    rdof_radio_init(&R.radio, &R.client, rdof_radio_detect(R.client.version));
    bool ok;
    if (R.full) {
        rdof_repeater_init(&R.r, &R.radio, rcfg.peer_id, R.rx_hz, R.tx_hz, cc);
        if (config_path)
            rdofradio_cfg_apply_repeater(&rcfg, &R.r); /* slots, hangs, jitter buffer, CW ID, as the firmware */
        R.r.clock = p_clock;
        R.r.on_tx = on_tx;
        R.r.on_sent = on_sent;
        R.r.on_key = on_key;
        R.r.ctx = &R;
        ok = rdof_repeater_start(&R.r);
    } else {
        rdof_txvr_init(&R.t, &R.radio, rcfg.peer_id);
        if (config_path) {
            rdofradio_cfg_apply_txvr(&rcfg, &R.t); /* its channels, listen, answer window, CW ID, as the firmware */
        } else {
            R.t.channels[0] = (rdof_channel_t){R.rx_hz, cc, 255};
            R.t.nchannels = 1;
            R.t.has_idle = true;
            R.t.idle = R.t.channels[0];
            if (R.tx_hz != R.rx_hz)
                R.t.channels[R.t.nchannels++] = (rdof_channel_t){R.tx_hz, cc, 255};
        }
        R.t.on_retune = txvr_retune;
        R.t.clock = p_clock;
        R.t.send = txvr_send;
        R.t.on_tx = on_tx;
        R.t.on_sent = on_sent;
        R.t.ctx = &R;
        ok = rdof_txvr_start(&R.t);
    }
    if (!ok) {
        fprintf(stderr, "%s start: %s\n", R.full ? "repeater" : "transceiver", R.client.error);
        shutdown_tx();
        dev->close(dev);
        return 1;
    }
    fprintf(stderr, "%s: %s, RX %.4f MHz (center %.4f), TX %.4f MHz (LO %.4f), cc %d, %.0f S/s, TX gain %.1f "
                    "(%.1f..%.1f), RX gain %s, analog bandwidth %.2f MHz\n",
            R.full ? "repeater" : "transceiver", R.client.version, rx / 1e6, R.rx.center_hz / 1e6, tx / 1e6,
            tx_lo / 1e6, cc, got, txg, dev->tx_gain_min, dev->tx_gain_max, isnan(gain) ? "auto" : "set",
            dev->bandwidth(dev) / 1e6);
    fprintf(stderr, "device: %s; FFT: %s\n", dev->name,
#ifdef RDOF_PFFFT
            fft && !strcmp(fft, "builtin") ? "built-in" : "pffft"
#else
            "built-in"
#endif
    );

    static rdof_cf_t in[CHUNK], out[CHUNK];
    const int lead = (int)(rate * 0.05); /* 50 ms of zeros ahead of the first read */
    const int64_t offset = (int64_t)llround(offset_s * rate);
    if (dev->start(dev)) {
        fprintf(stderr, "start: %s\n", dev->error);
        shutdown_tx();
        dev->close(dev);
        return 1;
    }
    memset(out, 0, sizeof out);
    writer_init();
    int64_t txn = 0; /* transmit samples put in the ring */
    for (int left = lead; left > 0; left -= chunk) {
        const int k = left < chunk ? left : chunk;
        tx_put(out, k);
        txn += k;
    }
    if (pthread_create(&W.thread, NULL, writer, NULL)) {
        fprintf(stderr, "the transmit writer: no thread\n");
        shutdown_tx();
        dev->close(dev);
        return 1;
    }
    W.running = true;
    { /* the writer first: the guard covers a late loop, not a late writer */
        const struct sched_param sp = {.sched_priority = 10};
        const int e = pthread_setschedparam(W.thread, SCHED_FIFO, &sp);
        if (e)
            fprintf(stderr, "the transmit writer: no real-time priority (%s)\n", strerror(e));
    }

#ifdef RDOF_HOST_WEB
    if (web_port >= 0) {
        web_settings(argc, argv);
        const int port = rdof_hostweb_start(web_addr, web_port, rcfg.box);
        if (port < 0)
            fprintf(stderr, "web: %s\n", strerror(errno));
        else
            fprintf(stderr, "web: http://%s:%d/\n", web_addr, port);
    }
    double last_web = 0;
#endif
    bench_txg = txg;
    bench_start();
    const double t_start = now_s();
    double last_poll = 0, keyed_until = -1;
    long long total = 0;
    int overflows = 0, rx_errors = 0, timeouts = 0;
    bool hot = false;
    R.onset = -1;
    R.ring_ref = -1, R.ring_min = INT64_MAX;
    R.lag_min = INFINITY, R.lag_prev = NAN;
    /* Full duplex: the transmit path's delay (the SDR's buffers; it differs run to run) decides which inbound slot a
     * handheld's burst is in, and the receiver can't hear the outbound at a split wider than its band. So, first,
     * the receiver listens on the TX LO for a 20 ms pulse sent on the output frequency, then goes back. */
    if (R.full) {
        if (dev->set_rx_freq(dev, tx_lo))
            say("calibration: can't tune the receiver to the TX LO: %s\n", dev->error);
        else
            R.cal = 1, R.cal_at = (long long)(rate * 0.3);
        tx_gain(txg); /* now: the gain step's LO leakage is in the baseline before the pulse, not taken for it */
    }
    double last_status = 0, level_sum = 0;
    long level_n = 0;
    double t_read = 0, t_write = 0, t_proc = 0; /* seconds spent in each, since the last status */
    while (!stop && (seconds <= 0 || total < (long long)(seconds * rate))) {
        const double t0 = now_s();
        int n = dev->read(dev, in, chunk, 500000);
        const double dt_read = now_s() - t0;
        t_read += dt_read;
        WS.wait += dt_read;
        if (n == RDOF_DEV_TIMEOUT) {
            if (++timeouts >= 10) { /* 5 s: the stream is dead (a Pluto after a USB read error) */
                fprintf(stderr, "read: nothing for 5 s, stopping\n");
                break;
            }
            continue;
        }
        timeouts = 0;
        if (n == RDOF_DEV_OVERFLOW) {
            overflows++;
            mq_overflows++;
            WS.overflows++;
            continue;
        }
        if (n < 0) {
            WS.errors++;
            if (++rx_errors > 10) {
                fprintf(stderr, "%s\n", dev->error);
                break;
            }
            continue;
        }
        const double now = now_s();
        WS.last_samples = now;
        WS.n += n;
        if (now - last_poll >= POLL_S) {
            last_poll = now;
            if (R.parrot)
                rdof_parrot_poll(R.parrot, now);
            if (!bench_tx_poll(now) && !bench_rpt_poll(now) &&
                !(R.full ? rdof_repeater_poll(&R.r) : rdof_txvr_poll(&R.t)))
                fprintf(stderr, "poll: %s\n", R.client.error);
            net_poll(now);
            mq_poll(now);
        }
        /* as many transmit samples as were read, at the modem's position for them */
        const double t_mod = now_s();
        const bool keyed = rdof_sdrmodem_pull_iq(&R.m, txn - lead + offset, out, n);
        WS.mod += now_s() - t_mod;
        /* Samples the receiver lost (a stall longer than the device's buffers: the stream falls behind the wall
         * clock) move what's heard against what's sent, and the transmitter's samples are paced by what's read: the
         * calibration no longer holds. Its floor over half a second against the last one's: a step is a loss. Half
         * duplex says so too (the bench's timing and a repeater job's slots move with it). A window the loop spent
         * behind (no read waited: the device's buffers held the samples) shows a lag that isn't a loss; it runs on
         * until a read waits. */
        {
            const double lag = (now - t_start) * rate - (double)total;
            if (lag < R.lag_min)
                R.lag_min = lag;
            R.lag_fresh |= dt_read >= FRESH_S;
            if ((double)total / rate - R.lag_win >= 0.5 && R.lag_fresh) {
                if (!isnan(R.lag_prev) && R.lag_min - R.lag_prev > rate / 500) {
                    WS.lost += llround(R.lag_min - R.lag_prev);
                    say("receive: %.1f ms of samples lost%s\n", 1e3 * (R.lag_min - R.lag_prev) / rate,
                        R.full ? "; calibrating again once idle" : "");
                    if (R.full)
                        R.recal = true, R.cal_tries = 0;
                }
                R.lag_prev = R.lag_min, R.lag_min = INFINITY, R.lag_fresh = false;
                R.lag_win = (double)total / rate;
                if (W.guarded) { /* what the transmit guard did this half second */
                    static uint32_t pads, dry;
                    static int64_t padded;
                    pthread_mutex_lock(&W.mu);
                    const uint32_t p = W.guard.pads, d = W.guard.dry;
                    const int64_t z = W.guard.padded;
                    pthread_mutex_unlock(&W.mu);
                    if (p != pads)
                        say("transmit: the loop fell behind; %.1f ms of zeros kept the stream's timing\n",
                            1e3 * (double)(z - padded) / rate);
                    if (d != dry)
                        say("transmit: the device ran dry %u times; what follows goes out later\n", d - dry);
                    pads = p, dry = d, padded = z;
                }
            }
        }
        if (R.full) {
            if (R.recal && R.cal == 0 && !hot && !keyed && !R.m.in_call[0] && !R.m.in_call[1]) {
                if (dev->set_rx_freq(dev, tx_lo) == 0) {
                    R.recal = false;
                    R.cal = 1, R.cal_at = total + (long long)(rate * 0.3);
                    tx_gain(txg);
                }
            }
        }
        if (R.cal == 1 && total >= R.cal_at && !keyed) {
            const int len = (int)(rate * 0.02) < n ? (int)(rate * 0.02) : n;
            for (int i = 0; i < len; i++) { /* 100 kHz below the TX LO, where the outbound goes */
                const double ph = -2 * M_PI * 100e3 * (double)i / rate;
                out[i].re = (float)(0.7 * cos(ph)), out[i].im = (float)(0.7 * sin(ph));
            }
            hot = true;
            keyed_until = (double)(txn + n) / rate + 0.3;
            R.onset = txn - lead + offset;
            R.cal = 2;
        }
        if (keyed) {
            keyed_until = (double)(txn + n) / rate + 0.3; /* past the device's buffers */
            if (!hot) {
                tx_gain(txg);
                hot = true;
                /* where the receiver should report the key-up's first burst: its first keyed sample, then the ramp
                 * and the CACH (4 + 12 symbols), then the receiver's delay (48 at 48 kS/s; reference.md, "Timing") */
                int i0 = 0;
                while (i0 < n && out[i0].re == 0.0f && out[i0].im == 0.0f)
                    i0++;
                R.first_sent = (txn - lead + offset + i0) * 48000 / (int64_t)rate + 16 * 10 + 48;
                R.waiting = R.mon >= 0;
                R.onset = txn - lead + offset + i0;
                R.echo_ch = R.tx_ch, R.echo_img = R.tx_img;
                R.echo_from = (int64_t)((double)(txn - lead + offset + i0) * 48000.0 / rate);
            }
            R.echo_to = (int64_t)((double)(txn - lead + offset + n) * 48000.0 / rate);
        } else if (hot && (double)txn / rate > keyed_until) {
            tx_gain(dev->tx_gain_min);
            hot = false;
        }
        for (int i = 0; i < n; i++)
            out[i].re *= 0.7f, out[i].im *= 0.7f;
        const double t1 = now_s();
        tx_put(out, n);
        t_write += now_s() - t1;
        WS.wait += now_s() - t1;
        txn += n;
        /* The transmit path's delay drifts: a stall in the writer lets the device run dry, and all that follows goes out
         * that much later. With the device's buffers full, the ring holds that much more: its floor over half a second,
         * against the floor just after the calibration, is the drift. Unkeyed, the ring holds zeros only, and the
         * excess is taken out (the newest samples, past what the writer may be reading); keyed, the modem follows it. */
        if (R.full) {
            pthread_mutex_lock(&W.mu);
            int64_t fill = W.head - W.tail;
            if (fill < R.ring_min)
                R.ring_min = fill;
            if ((double)total / rate - R.ring_win >= 0.5) {
                if (R.cal == 0 && R.ring_ref < 0) {
                    R.ring_ref = R.ring_min;
                    R.cal48 = R.m.tx_late48;
                } else if (R.ring_ref >= 0) {
                    int64_t drift = R.ring_min - R.ring_ref;
                    if (!hot && drift > (int64_t)rate / 500) {
                        const int64_t cut = drift < fill - CHUNK ? drift : fill - CHUNK;
                        if (cut > 0) {
                            W.head -= cut;
                            drift -= cut;
                            say("transmit: %.1f ms of delay taken out (the writer had fallen behind)\n", 1e3 * (double)cut / rate);
                        }
                    }
                    R.m.tx_late48 = R.cal48 + drift * 48000 / (int64_t)rate;
                }
                R.ring_min = INT64_MAX;
                R.ring_win = (double)total / rate;
            }
            pthread_mutex_unlock(&W.mu);
        }
        /* the key-up's edge in what's received: 20 dB over the power before it */
        /* the calibration's pulse may come back before its place (--tx-offset too large): looked for from 100 ms before */
        const int64_t edge_from = R.onset - (R.cal == 2 ? (int64_t)rate / 10 : 0);
        if (R.onset >= 0 && total + n > edge_from) {
            for (int i = 0; i < n; i++) {
                const double p2 = in[i].re * in[i].re + in[i].im * in[i].im;
                if (total + i >= edge_from && p2 > 100 * R.base_power) {
                    if (R.cal == 2) {
                        /* the modem's inbound slots follow it: a handheld keeps to what's on the air */
                        R.m.tx_late48 = (total + i - R.onset) * 48000 / (int64_t)rate;
                        say("calibration: the transmit path is %.2f ms past its place\n",
                                1e3 * (double)(total + i - R.onset) / rate);
                        dev->set_rx_freq(dev, R.rx.center_hz);
                        R.cal = 0, R.cal_tries = 0;
                        R.ring_ref = -1; /* the ring's floor taken again against this */
                    } else if (BR.active && BR.cal) {
                        /* a repeater job's first key-up, heard in the receiver's band: its inbound slots follow it */
                        R.m.tx_late48 = (total + i - R.onset) * 48000 / (int64_t)rate;
                        BR.cal = false;
                        say("bench: the job's transmit path is %.2f ms past its place\n",
                            1e3 * (double)(total + i - R.onset) / rate);
                    } else {
                        /* at a split wider than the receiver's band, this is the first inbound burst, not the TX */
                        say("loop delay: the key-up reached the receiver %.2f ms after its place\n",
                                1e3 * (double)(total + i - R.onset) / rate);
                    }
                    R.onset = -1;
                    break;
                }
            }
            if (R.onset >= 0 && total - R.onset > (long long)rate) {
                R.onset = -1; /* no edge within a second: too little leaks in to see it */
                if (R.cal == 2) {
                    dev->set_rx_freq(dev, R.rx.center_hz);
                    R.cal = 0;
                    if (++R.cal_tries < 5) {
                        say("calibration: the pulse wasn't heard; trying again\n");
                        R.recal = true;
                    } else
                        say("calibration: the pulse wasn't heard in 5 tries; the inbound slots may be swapped\n");
                }
            }
        } else if (!hot) {
            double p2 = 0;
            for (int i = 0; i < n; i += 16)
                p2 += in[i].re * in[i].re + in[i].im * in[i].im;
            R.base_power = p2 / ((n + 15) / 16) * 4; /* a little over the mean, for the edge's threshold */
        }
#ifdef RDOF_HOST_WEB
        if (now - last_web >= 0.25) {
            last_web = now;
            WS.tx_on = hot;
            web_publish(rx, tx, cc);
        }
#endif
        if (verbose) {
            for (int i = 0; i < n; i += 16)
                level_sum += in[i].re * in[i].re + in[i].im * in[i].im;
            level_n += (n + 15) / 16;
            if (now - last_status >= 1.0) {
                fprintf(stderr, "status %.1f s (wall %.1f; read %.2f, ring wait %.2f, process %.2f s; ring %.0f ms): RX gain %.0f dB, level %.1f dBFS, %s, modem sent %u idle %u heard %u\n",
                        total / rate, now - t_start, t_read, t_write, t_proc, 1e3 * (double)(W.head - W.tail) / rate, dev->rx_gain(dev),
                        10 * log10(level_sum / (level_n ? level_n : 1) + 1e-12), hot ? "TX on" : "TX off",
                        R.m.tx_bursts, R.m.idle_bursts, R.m.rx_bursts);
                last_status = now;
                level_sum = 0;
                level_n = 0;
                t_read = t_write = t_proc = 0;
            }
        }
        const double t2 = now_s();
#ifdef RDOF_HOST_WEB
        web_spectrum(in, n);
#endif
        bench_feed(in, n, dt_read >= FRESH_S, false);
        const double t3 = now_s();
        rx_chunk(in, n);
        const double t4 = now_s();
        t_proc += t4 - t2;
        WS.spec += t3 - t2, WS.rx += t4 - t3;
        ws_turn(t4);
        total += n;
    }
    if (R.full)
        rdof_repeater_stop(&R.r);
    shutdown_tx();
    dev->close(dev);

    fprintf(stderr, "%.1f s (%.1f s wall), %d overflows, %d TX write errors; modem: %u bursts sent, %u idle, %u heard, "
                    "%u muted, %u lost, %u refused\n",
            total / rate, now_s() - t_start, overflows, W.errors, R.m.tx_bursts, R.m.idle_bursts, R.m.rx_bursts,
            R.m.muted, R.m.lost, R.m.refused);
    if (W.guarded && (W.guard.pads || W.guard.dry))
        fprintf(stderr, "transmit guard: %u blocks finished with zeros (%.1f ms), ran dry %u times\n", W.guard.pads,
                1e3 * (double)W.guard.padded / rate, W.guard.dry);
    if (R.rx.echoes)
        fprintf(stderr, "receiver: %ld of our own bursts heard back and dropped\n", R.rx.echoes);
    for (int c = 0; c < NCH; c++)
        fprintf(stderr, "%s %.4f MHz: %ld bursts, AFC %.1f Hz\n", CH[c].id, CH[c].freq_hz / 1e6, R.rx.bursts[c],
                R.rx.chz.correction_hz[c]);
    if (R.delay_n)
        fprintf(stderr, "monitor: a key-up heard %.2f ms after the modem's position (%.2f..%.2f, %d key-ups); "
                        "--tx-offset %.4f lines them up\n",
                1e3 * R.delay_sum / R.delay_n, 1e3 * R.delay_min, 1e3 * R.delay_max, R.delay_n,
                offset_s + R.delay_sum / R.delay_n);
    if (R.parrot)
        say("parrot: %u sent back, %u not to it, %u bursts cut\n", R.parrot->replayed, R.parrot->ignored,
                R.parrot->cut);
    bench_stop();
    mq_stop();
    net_close();
    clock_stop();
    rdof_sdr_rx_free(&R.rx);
#ifdef RDOF_HOST_WEB
    rdof_hostweb_stop();
#endif
    rdof_sdrmodem_free(&R.m);
#ifdef RDOF_HOST_WEB
    if (restart_asked) {
        /* the transmitter is down and the device closed (above): start again with the same command line */
        fprintf(stderr, "restarting (asked for from the web page)\n");
        rdof_hostweb_reexec(argv);
        fprintf(stderr, "restart: %s\n", strerror(errno));
        return 1;
    }
#endif
    return 0;
}
