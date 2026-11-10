// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * rdofradio: an SDR that receives and transmits, run as the gateway's sdr_transceiver on a host or on the SDR
 * itself (a PlutoSDR's own ARM, through libiio: rdofradio_dev.h). The same parts as on a board (docs/reference.md,
 * "Transmit: the SDR as a radio"): the receiver hears the settings' SDR channels, all alike (each its own HBlink4
 * peer with identity per_path), the SDR modem (rdof/sdr/sdrmodem.h) takes what it hears on the channel at its
 * frequency and modulates what it's given, and the repeater (full duplex) or the roaming transceiver (half duplex)
 * drives the modem through an rdof_modem client, as it would an MMDVM_HS. With an hblink section in the settings
 * (--config) it's an HBlink4 peer as the board's (rdofradio_net.h, common/src/hbpeer.c): what it hears goes up as
 * DMRD, and the master's calls go out (to the unit's peer, or to any channel's: one transmitter); without
 * one there's no network, and the repeater repeats what it hears on its outbound (repeater.md 4), the transceiver
 * only listens. --parrot ID sends back what's heard once it ends (rdof/parrot.h), as a network call would come.
 *
 * With an mqtt section it publishes as the board does (rdofradio_mqtt.h): the radio's calls, bursts, what it
 * transmits and its status on the repeater's or roamer's topics (its calls: the channel it's on), every channel's
 * on the SDR's.
 *
 * The clock (the settings' ntp): netclock, the board's NTP disciplined clock (components/netclock, its POSIX glue),
 * or the OS's (ntp.clock "os": chrony, ntpd). --clock-state FILE keeps netclock's learned rate across restarts;
 * --set-os-clock steps the OS clock to it (root; for a box with nothing else setting the time, a PlutoSDR).
 *
 *   rdofradio DEVICE --allow-tx --rx MHZ [--tx MHZ] [--duplex full|half] [--cc CC] [-r RATE] [--bw MHZ]
 *             [--tx-power DBM [--tx-max-dbm DBM] | --tx-gain DB] [--tx-lo MHZ] [--gain DB] [--center MHZ] [--monitor]
 *             [--tx-offset S] [--ppm PPM [--ppm-tol PPM]] [--parrot ID] [--stream ARGS] [--seconds S] [--bursts] [--quiet] [-v] [--web [ADDR:]PORT]
 *             [--last-exit FILE] [--ota FILE] [--fft builtin|pffft] [--bench [ADDR:]PORT] [--bench-tx MHZ|LOW-HIGH,...]
 *   rdofradio DEVICE --allow-tx --config FILE [--clock-state FILE] [--set-os-clock] [the device's flags as above]
 *   rdofradio DEVICE --allow-tx --split-tx PORT [--baud N] [--split-power-cap N] --rx MHZ --tx MHZ [--cc CC] [...]
 *   rdofradio [DEVICE] --allow-tx --mmdvm PORT [--baud N] [--rf-level N] [--rx-offset-hz HZ] [--tx-offset-hz HZ]
 *             [--rx-level PCT] [--tx-level PCT] --rx MHZ [--tx MHZ] [--duplex full|half] [--cc CC] | --config FILE [...]
 *
 * --sim-sdr in place of DEVICE, receive only with --mmdvm or --split-tx: a receiver of noise, for tests with no RF.
 *
 * DEVICE is --soapy ARGS (SoapySDR; -r defaults to 2.4 MS/s) or --iio URI (an AD9361 through libiio: "local:" on a
 * PlutoSDR, "ip:192.168.2.1" from a host; -r defaults to 768 kS/s, which the chip's own FIR makes with
 * libad9361). --ppm PPM: the device's crystal error (positive: fast, fractions too), measured elsewhere: rdofradio
 * doesn't measure it (its netclock disciplines the time, not the device's crystal). Taken out in software, receive
 * and transmit (xtal_ppm); without it, every frequency the bench reads or sends is off by the crystal's error.
 * --ppm-tol PPM: how far --ppm may be off (the measurement's own error), given to the bench's reports as their
 * ppm_tol and freq_tol_hz; without it they say it isn't known. --web serves the page on 127.0.0.1 unless ADDR is given (0.0.0.0: every interface). --fft: the
 * channelizer's FFT, pffft when it's built in (RDOF_PFFFT). --last-exit FILE: where a supervisor records how the
 * last run ended (rdof_hostweb_last_exit; the Pluto bundle's run.sh): the page's crash card. --ota FILE: the page's
 * update (POST /api/ota) writes the new program there and stops, for the supervisor to start it. --bench serves the
 * bench's line protocol (docs/spec/bench-api.md section 14) on PORT (7373 by convention), every interface unless
 * ADDR is given: the receiver's bursts and wideband captures for tools/bench/rfgbench.py, and its transmit jobs
 * (bench_tx's voice, csbk, data) on the --bench-tx frequencies and bands (LOW-HIGH), at --tx-power, in the transceiver's
 * place while they run (not in a duplex repeater). --fpga-chz [ADDR] (with --iio, on a PlutoSDR whose FPGA has the
 * channelizer block, its registers at ADDR, 0x7C480000 by default): the block channelizes and the receiver takes its
 * 48 kS/s channel frames from the receive DMA, its AFC steering the block's NCOs; the rate can then go to 24.576 MS/s
 * (R D up to 512). No wideband captures, and the spectrum is the channels' own (the span between them empty); with
 * --split-tx or --mmdvm too. --fpga-duc [ADDR] (with --iio, an FPGA with the TX interpolator, its registers at ADDR,
 * 0x7C4A0000 by default; only on a design that has it: elsewhere nothing answers there): the transmit stream goes at
 * 192 kS/s (96 kS/s if the rate isn't a multiple of 192 kS/s; not 48, whose images land on a 50 kHz grid's next
 * channels) and the interpolator brings it to the rate, so a wide receiver doesn't make a wide transmit stream for the
 * ARM. The TX LO then sits 62.5 kHz above the carrier (18.75 at 96 kS/s; --tx-lo unless given), the most the stream
 * reaches, in steps of 6.25 kHz. The rate is planned to one it makes; with none for the channels, it's off. With
 * --split-tx or --mmdvm the SDR doesn't transmit: not used.
 * With --iio local: on the board, either loads their bitstream first (rdofradio_fpga.h: --fpga-bit FILE, else
 * rfg-pluto-<board>.bit.bin where it's found: /mnt/jffs2/rdof/, rdof/ on an SD card or USB stick, /lib/firmware/);
 * with none for the board, both are off and the ARM channelizes, with the channels that fit its window.
 * --fpga-board prints the board and the bitstream's file name.
 *
 * --split-tx PORT: a split duplex repeater (docs/spec/repeater.md): a RadioFoundry radio on PORT (its hotspot, with
 * the split duplex extension) transmits, and the SDR, opened receive only, is the RX half. Any receiver does, an
 * RTL-SDR included. rdof_split_t joins the two into one duplex modem for the repeater, as split_port.c does on a
 * board. The SDR modem's clock for the split's TIME is the receive stream's sample count against the wall clock,
 * at the freshest read; samples the device drops are made up with zeros, so the count keeps real time.
 * --split-power-cap is the radio's RF level cap (MMDVM scale, 1-255; the radio's default is 64, about 25 %).
 *
 * --mmdvm PORT: the unit is on an MMDVM modem on a serial port (an MMDVM_HS hat on a UART, a hotspot on USB), as
 * the board runs one on its UART: a duplex modem (an MMDVM_HS dual hat, an MMDVM board) as the repeater, any as
 * the half duplex transceiver. DEVICE is optional: an SDR, opened receive only (an RTL-SDR does), hears the
 * modem's RX frequency and the settings' channels, for the page, MQTT, HBlink4 and the bench's line protocol.
 * What the modem hears goes through a call tracker of its own, as on the board. --rf-level caps every SET_FREQ's
 * RF level (MMDVM scale 0-255; 255, no cap); the bench's jobs transmit DMR on the modem, in simplex, at their
 * power under that cap. --rx-offset-hz, --tx-offset-hz, --rx-level and --tx-level are the modem's settings (with
 * --config: the device's mmdvm settings, and each frequency's power).
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
 * (the most attenuation); it goes to the transmit power while anything keyed is in the stream. Nothing transmits
 * without --allow-tx.
 *
 * Transmit power: --tx-power DBM at the antenna, from --tx-max-dbm, the device's output at its most gain (+7 dBm by
 * default, a PlutoSDR's nominal: uncalibrated; set it for another device). With --config and neither flag, the SDR
 * device's tx.power_dbm. --tx-gain DB sets the device's gain itself (its own scale: a PlutoSDR's 0..89, 89 the most).
 * A SET_FREQ's RF level is a level of the device's map (rdof_txpower_sdr_map): 255 is the transmit power, each step
 * 0.2 dB under it; the gain takes it at the next key-up.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdatomic.h>
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
#include <sys/resource.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rdof/benchline.h"
#include "rdof/benchnet.h"
#include "rdof/benchtx.h"
#include "rdof/hbpeer.h"
#include "rdof/layer2.h"
#include "rdof/modem.h"
#include "rdof/mssim.h"
#include "rdof/parrot.h"
#include "rdof/pdu.h"
#include "rdof/repeater.h"
#include "rdof/siteroute.h"
#include "rdof/txpower.h"
#include "rdof/split.h"
#include "rdof/sdr/chzhw.h"
#include "rdof/sdr/plutofpga.h"
#include "rdof/sdr/modes.h"
#include "rdof/sdr/ppm.h"
#include "rdof/sdr/receiver.h"
#include "rdof/sdr/sdrmodem.h"
#include "rdof/sdr/sdrsplit.h"
#include "rdof/sdr/spectrum.h"
#include "rdof/txvr.h"
#include "rdofradio_cfg.h"
#include "rdofradio_dev.h"
#include "rdofradio_fpga.h"
#include "rdofradio_dnssd.h"
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

#define TX_LEAD_S 0.05     /* zeros the transmit stream starts with, ahead of the first read */
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
    bool mmdvm;            /* --mmdvm: the unit is on an MMDVM modem on a serial port; the SDR, if any, receives */
    bool keyed;            /* the repeater's, from on_key */
    bool verbose;
    double rate, center, tx_lo;
    double tx_rate;        /* the transmit stream's: rate, or rate / txr with --fpga-duc */
    int txr;               /* receive samples a transmit sample */
    double lo_off;         /* the TX LO above the carrier when it moves (io_tune): 100 kHz, or with --fpga-duc
                              the most the stream reaches (dx_start) */
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
    int64_t onset;         /* the key-up's first keyed sample, the modem's position times txr (the receiver's
                              samples); -1 none pending */
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
static int mm_rf_level = 255; /* --rf-level: the cap on every SET_FREQ's RF level to the MMDVM modem */
static long long mm_heard_n, mm_capped; /* --mmdvm: bursts the modem heard, SET_FREQs capped */
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
static rdof_sdrsplits_t XS; /* the settings' other split repeaters on the SDR (xsplit_t) */
static int NX;

/* The radio's port: a thread reads it and stamps each read with the time it came, so the split's TIME round trips
 * don't include the main loop's wait on the receive stream. */
#define PORT_Q 64
typedef struct {
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
} port_t;
static port_t P = {.fd = -1, .mu = PTHREAD_MUTEX_INITIALIZER};
static int in_wait; /* inside the client's wait for a reply: what's heard waits for it to end */
static void split_turn(void);
static void split_flush(void);
static int64_t split_now48(void *ctx);

/* The device or the radio's port went away, or the SDR stopped giving samples: the program stops and exits with 1, so
 * a supervisor (systemd's Restart=on-failure) starts it again. */
static volatile sig_atomic_t lost_device;
static void on_signal(int s) { stop = 1; }

/* --config: the file, and the radio it (or the command line) sets */
static const char *config_path;
static rdofradio_cfg_t rcfg;
#ifdef RDOF_HOST_WEB
static char held[300]; /* hold (below): why the settings don't run, "" when they do */
#endif

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
    int64_t pushed;        /* tail at the end of the last write: what a hold drops isn't in it */
    bool quit;
    int errors;
    pthread_mutex_t mu;
    pthread_cond_t cv;     /* on CLOCK_MONOTONIC (writer_init) */
    pthread_t thread;
    bool running;
    bool guarded;          /* the device takes blocks: the guard keeps the stream's timing through a stall */
    bool hold;             /* a raw capture has the chip off its rate: what comes is dropped, nothing padded */
    bool writing;          /* in the device's write, the lock let go: the chip's rate waits for it */
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
        rdof_txguard_init(&W.guard, R.tx_rate, dev->tx_block, dev->tx_nbuf);
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
    W.writing = true;
    pthread_mutex_unlock(&W.mu);
    while (room > 0) {
        const int k = dev->write(dev, zeros, room < CHUNK ? room : CHUNK, 100000);
        if (k <= 0)
            break;
        room -= k, sent += k;
    }
    pthread_mutex_lock(&W.mu);
    W.writing = false;
    pthread_cond_broadcast(&W.cv);
    if (room > 0)
        W.errors++;
    rdof_txguard_padded(&W.guard, sent);
}

static void *writer(void *arg)
{
    pthread_mutex_lock(&W.mu);
    for (;;) {
        if (W.hold && !W.quit) { /* dropped as if sent: the stream's count keeps real time */
            W.tail = W.head;
            pthread_cond_broadcast(&W.cv);
            pthread_cond_wait(&W.cv, &W.mu);
            continue;
        }
        while (W.head == W.tail && !W.quit && !W.hold) {
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
        W.writing = true;
        pthread_mutex_unlock(&W.mu);
        int k = dev->write(dev, W.buf + at, n, 100000);
        pthread_mutex_lock(&W.mu);
        W.writing = false;
        pthread_cond_broadcast(&W.cv);
        if (k <= 0) {
            W.errors++;
            k = n; /* dropped: the stream slips by n, and the loop's measure of the delay shows it */
        }
        W.tail += k;
        W.pushed = W.tail;
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
            "                 [--tx-power DBM [--tx-max-dbm DBM] | --tx-gain DB] [--tx-lo MHZ] [--gain DB] [--center MHZ] [--monitor]\n"
            "                 [--tx-offset S] [--ppm PPM [--ppm-tol PPM]]\n"
            "                 [--parrot ID] [--stream ARGS] [--seconds S] [--bursts] [--quiet] [-v] [--web [ADDR:]PORT]\n"
            "                 [--last-exit FILE] [--ota FILE] [--fft builtin|pffft] [--bench [ADDR:]PORT] [--bench-tx MHZ|LOW-HIGH,...]\n"
            "                 [--fpga-chz [ADDR]] (with --iio: the FPGA channelizer on a PlutoSDR, at 0x7C480000 by default)\n"
            "                 [--fpga-duc [ADDR]] (with --iio: the FPGA's TX interpolator, at 0x7C4A0000 by default)\n"
            "                 [--fpga-bit FILE] (the bitstream with them; else looked for, rdofradio_fpga.h)\n"
            "       rdofradio --fpga-board (a PlutoSDR's board and its bitstream's file name)\n"
            "       rdofradio DEVICE --allow-tx --config FILE [--clock-state FILE] [--set-os-clock] [the device's flags]\n"
            "       rdofradio DEVICE --allow-tx --split-tx PORT [--baud N] [--split-power-cap N] --rx MHZ --tx MHZ [--cc CC]\n"
            "                 [-r RATE] [--gain DB] [--center MHZ] [--parrot ID] [--seconds S] [-v] [--web [ADDR:]PORT]\n"
            "       rdofradio [DEVICE] --allow-tx --mmdvm PORT [--baud N] [--rf-level N] [--rx-offset-hz HZ]\n"
            "                 [--tx-offset-hz HZ] [--rx-level PCT] [--tx-level PCT] --rx MHZ [--tx MHZ] [--duplex full|half]\n"
            "                 [--cc CC] | --config FILE [--parrot ID] [--seconds S] [-v] [--web ...] [--bench ...]\n"
            "--soapy takes --setting KEY=VALUE, repeated: the driver's own (an RTL-SDR's biastee=true)\n"
            "DEVICE: %s (or, receive only, --sim-sdr: noise, for tests with no RF)\n",
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
    if (R.mmdvm)
        return; /* the modem is on the port: the SDR modem only hears for the bench */
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
 * SoapySDR 3.2 MS/s, an RTL-SDR's (give others -r). With --fpga-chz, up to the most the block channelizes. */
static bool dev_iio, dev_local;
static bool fpga_fallback; /* --fpga-chz or --fpga-duc asked for, no bitstream for them: the ARM's path */
static double rate_flag;
/* --ppm: the device's crystal error (ppm, positive: fast), NAN: not given. rdofradio doesn't measure it, and a
 * PlutoSDR's own correction (xo_correction) reaches only 0.5 ppm: it is taken out in software, its LO and sample
 * clock alike. The receiver's channels start where a carrier truly on frequency shows, the bench's rx and trigger
 * tune there (the window's lo_ppm, fs_ppm) and its bursts' cfo is from the true frequency; the transmitter's
 * carrier is moved by as much (rdof_sdrmodem_carrier_ppm). */
static double xtal_ppm = NAN, xtal_tol; /* xtal_tol: --ppm-tol, 0 not given */
static double xtal(void) { return isfinite(xtal_ppm) ? xtal_ppm : 0.0; }
/* Where channel c's carrier sits when truly on frequency, from its nominal offset (Hz) */
static double xtal_base_hz(int c)
{
    const double f = CH[c].freq_hz, c0 = R.rx.center_hz;
    return rdof_ppm_seen_hz(f, c0, xtal(), xtal()) - (f - c0);
}

/* --fpga-chz [ADDR]: the FPGA channelizer on a PlutoSDR (rdof/sdr/chzhw.h) in place of the receiver's own. rdofradio
 * starts it with the receiver's channels, reads its frames from the receive DMA (rdofradio_iio.c's words) and gives
 * them to the receiver (rdof_sdr_rx_channels_in); the AFC's corrections go to its NCOs. A frame is an output
 * instant, step = R D wideband samples: the loop counts in those, so the transmitter and the clocks keep their
 * wideband timeline. There is no wideband I/Q: no spectrum, and the bench's I/Q ops (capture, trigger, track, rx)
 * take the channels' own outputs (bl_ch_*), each instant with the NCO offset it was mixed from. (below, fx_*) */
#define FX_ADDR 0x7C480000ul /* the block's registers in the Pluto design */
#define FX_MAX 4096          /* channel samples a receiver call */
#define FX_GAP 48000         /* most lost output instants made up with zeros at once (1 s) */
#define FX_LATE 9            /* a new NCO step reaches the filter's middle this many output instants after FRAMES:
                                the next instant, then 8 (half of 16 D + 1 taps) */

static struct {
    bool on;
    unsigned long addr;
    void *regs;
    rdof_chzhw_t h;
    rdof_chzhw_rx_t prx;
    rdof_chzfx_cfg_t cfg;
    bool started;
    int R, D, step, C;
    double off[RDOF_CHZFX_MAX_CH];
    uint32_t inc[RDOF_CHZFX_MAX_CH];
    bool dirty;              /* inc changed: to the block after the receiver's turn */
    bool cal;                /* channel 0 on the duplex calibration's pulse */
    bool have0;
    uint32_t seq0, expect;   /* the first frame's number, the next one's */
    rdof_iq16_t *y;          /* the last read's output instants (zeros for lost ones), C samples each */
    float *pw;               /* each one's power: the strongest channel's (channel 0's while calibrating) */
    bool *fl;                /* each one's a lost one's zeros */
    long long nall, base;    /* instants so far, as the receiver counts them; the last read's first one's */
    struct fx_nco {          /* a channel's NCO offset (wideband Hz from the center) as the bench reads it: the one */
        double hz;           /* now, and the next ones from the instant they take effect */
        int n;
        struct { long long from; double hz; } q[4];
    } nco[RDOF_CHZFX_MAX_CH];
    int ny, cap, j, turn;    /* how many, room; the next the loop's turns take, this turn's first */
    long long lost, made_up, skipped; /* instants lost; zeros in their place; neither (past FX_GAP or the room) */
    uint32_t status;         /* STATUS at the last read */
    long ovf;                /* overflow bits seen set (STATUS IN_OVF, DMA_OVF) */
    double t_rx;             /* seconds the last read spent in the receiver */
} FX = {.addr = FX_ADDR};

/* Whether the block takes R and D for step = R D: its sizes (CAPS) and the model's. */
static bool fx_fits(int step, int r, int d)
{
    const rdof_chzhw_t *h = &FX.h;
    const int taps = RDOF_CHZ_OUT_FFT / 4 * d + 1; /* rdof_chzfx_design's */
    if (d > h->max_decim || d > RDOF_CHZFX_MAX_DECIM || taps > h->max_taps || taps + d > h->hist ||
        taps > RDOF_CHZFX_MAX_TAPS || taps + d > RDOF_CHZFX_HIST)
        return false;
    return r == 1 || (r <= h->max_cic && r <= RDOF_CHZFX_MAX_CIC && step <= RDOF_CHZFX_MAX_STEP);
}

/* The block's R and D for step = R D (48 kS/s out): no CIC when the FIR alone decimates that much, else the CIC
 * with the smallest D from FX_MIN_D that divides it, else the largest below. The FIR's taps grow with D (its
 * response doesn't), and the block's time for them: R 9, D 46 (737 taps) overflowed the 7010's block's input at
 * 19.872 MS/s, R 60, D 8 (129) didn't at 23.04. False if none fits. */
#define FX_MIN_D 8
static bool fx_plan(int step, int *R, int *D)
{
    if (fx_fits(step, 1, step)) {
        *R = 1, *D = step;
        return true;
    }
    for (int d = FX_MIN_D; d <= step; d++)
        if (step % d == 0 && fx_fits(step, step / d, d)) {
            *R = step / d, *D = d;
            return true;
        }
    for (int d = FX_MIN_D - 1; d >= 1; d--)
        if (step % d == 0 && fx_fits(step, step / d, d)) {
            *R = step / d, *D = d;
            return true;
        }
    return false;
}
static double span_of(const rdof_sdr_channel_t *c, int n)
{
    double lo = c[0].freq_hz, hi = c[0].freq_hz;
    for (int i = 1; i < n; i++)
        lo = fmin(lo, c[i].freq_hz), hi = fmax(hi, c[i].freq_hz);
    return hi - lo;
}
/* --fpga-duc [ADDR]: the FPGA's TX interpolator (rdof/sdr/chzhw.h): the transmit stream at R.tx_rate, the
 * interpolator making up the device's rate. Only when asked: on a design without it nothing answers at ADDR, and
 * a read there is a bus error. */
#define DX_ADDR 0x7C4A0000ul
static struct {
    bool on, probed, started;
    unsigned long addr;
    void *regs;
    rdof_chzhw_duc_t d;
} DX = {.addr = DX_ADDR};
static long dx_r(double rate);
static double plan_rate(const rdof_sdr_channel_t *c, int n, char *why, size_t cap)
{
    double f[RDOF_SDR_MAX_CHANNELS], center;
    for (int i = 0; i < n; i++)
        f[i] = c[i].freq_hz;
    if (FX.on && n > FX.h.channels) {
        snprintf(why, cap, "%d receive channels: the FPGA channelizer has %d", n, FX.h.channels);
        return 0;
    }
    int fx_r, fx_d;
    const int fx_most = FX.h.max_cic > 1 ? RDOF_CHZFX_MAX_STEP : FX.h.max_decim; /* R D at most */
    const double lo = rate_flag ? rate_flag : dev_iio ? 768000 : 2400000;
    const double hi = rate_flag ? rate_flag : FX.on ? 48000.0 * fx_most : dev_local ? 2400000 : dev_iio ? 3840000 : 3200000;
    for (double r = ceil(lo / 48000) * 48000; r <= hi; r += 48000) {
        if (rdof_plan_center(f, n, r, &center))
            continue;
        if (DX.probed && !dx_r(r))
            continue; /* --fpga-duc: a rate its interpolator makes from 192 or 96 kS/s */
        if (FX.on) { /* the block's decimation, not the ARM's FFT */
            if (fx_plan((int)(r / 48000), &fx_r, &fx_d))
                return r;
            continue;
        }
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
    if (FX.on && rate_flag && !rdof_plan_center(f, n, rate_flag, &center)) {
        snprintf(why, cap, "-r %.0f: the FPGA channelizer can't make 48 kS/s of it (D up to %d, a CIC's R up to %d, "
                           "R D up to %d)", rate_flag, FX.h.max_decim, FX.h.max_cic, RDOF_CHZFX_MAX_STEP);
        return 0;
    }
    snprintf(why, cap, "receive channels %.4f to %.4f MHz: wider than %.2f MS/s holds (%s); fewer channels%s",
             flo / 1e6, fhi / 1e6, hi / 1e6, rate_flag ? "-r" : "this device's most here",
             rate_flag ? ", or a wider -r" : "");
    return 0;
}
/* A bench job has the transmitter (owner_yield): the modem's owner paused. tuned: the modem simplex (a repeater's);
 * lo: the TX LO before the job, put back for the repeater after it. */
static struct {
    bool on, tuned;
    double lo;
} RP;
/* A repeater hears on its RX channel. A transceiver hears on any channel the receiver has, and may go where none
 * hears it to transmit (the modem then hears nothing there); so does a repeater's modem while a bench job has it. */
static int io_channel(void *ctx, uint32_t hz)
{
    if (R.full && !RP.on)
        return hz == R.rx_hz ? channel_of(hz) : -1;
    const int c = channel_of(hz);
    return c >= 0 ? c : RDOF_SDRMODEM_NOT_HEARD;
}
/* The transmit power: the gain keyed (--tx-power or --tx-gain), and what SET_FREQ's RF level takes off it. */
static double txg_full;         /* the gain at level 255 */
static double tx_max_dbm = NAN; /* --tx-max-dbm: the device's output at its most gain */
static int tx_rf_level = 255;
static double gain_dbm(double g) { return tx_max_dbm - (dev->tx_gain_max - g); }
static double keyed_gain(void)
{
    const rdof_txmap_t m = rdof_txpower_sdr_map(0);
    const double g = txg_full + rdof_txpower_dbm(&m, tx_rf_level); /* 0 at 255, -0.2 dB a level under it */
    return g < dev->tx_gain_min ? dev->tx_gain_min : g;
}
static void io_tx_level(void *ctx, int level)
{
    if (level != tx_rf_level && R.verbose)
        fprintf(stderr, "transmit: RF level %d, %.1f dBm at the antenna (nominal)\n", level,
                gain_dbm(txg_full) - (255 - level) * RDOF_TXPOWER_SDR_SPAN_DB / 255.0);
    tx_rf_level = level;
}
static bool io_tune(void *ctx, uint32_t hz, double *off)
{
    /* a repeater's TX LO stays, but moves for a bench job (and back after it) */
    if ((!R.full || RP.on) && fabs((double)hz - R.tx_lo) > R.tx_rate * 0.4 - 12.5e3) {
        /* past the modulator's reach: the TX LO moves, lo_off above (unkeyed: the gain is at its least) */
        const double lo = (double)hz + R.lo_off;
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
    return fabs(*off) < R.tx_rate * 0.4;
}
static bool mm_write(const uint8_t *d, size_t n);
static void mm_turn(void);
static bool p_write(void *ctx, const uint8_t *d, size_t n)
{
    if (R.mmdvm)
        return mm_write(d, n);
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
    if (R.mmdvm) { /* as the split's: the receiver keeps reading while the modem answers */
        const double end = now_s() + ms / 1000.0;
        in_wait++;
        while (!R.client.reply_ready && !stop && !P.gone && now_s() < end)
            mm_turn();
        in_wait--;
        return R.client.reply_ready;
    }
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
    int rx_ch;             /* the input's receiver channel: it decodes the job's color code meanwhile (-1: none) */
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

static void mm_heard(int slot, uint8_t control, const uint8_t *burst, int rssi);
static void on_rx(void *ctx, int slot, uint8_t control, const uint8_t *burst, int rssi)
{
    if (in_wait) {
        if (heard_n < HEARD_Q) {
            heard_q[heard_n].slot = slot, heard_q[heard_n].rssi = rssi, heard_q[heard_n].control = control;
            memcpy(heard_q[heard_n++].burst, burst, RDOF_BURST_BYTES);
        }
        return;
    }
    if (R.mmdvm)
        mm_heard(slot, control, burst, rssi);
    if (R.verbose && R.full && R.m.keyed)
        fprintf(stderr, "inbound: slot %d, %+.2f slots from its place\n", slot, R.m.inbound_off);
    if (R.parrot)
        rdof_parrot_heard(R.parrot, slot, control, burst, now_s());
    if (BR.active) { /* a job's repeater has the modem, the site's repeater stopped for it (R.full) or not */
        rdof_bl_rpt_ev_t e = {.kind = RDOF_BL_RPT_HEARD, .slot = slot, .rssi = rssi, .control = control};
        memcpy(e.burst, burst, sizeof e.burst);
        br_event(&e);
        rdof_repeater_heard(&BR.r, slot, control, burst, rssi);
    } else if (R.full) {
        rdof_repeater_heard(&R.r, slot, control, burst, rssi);
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
    if (BR.active) {
        br_event(&(rdof_bl_rpt_ev_t){.kind = RDOF_BL_RPT_LOST, .slot = slot});
        rdof_repeater_lost(&BR.r, slot);
    } else if (R.full) {
        rdof_repeater_lost(&R.r, slot);
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
    if (BR.active)
        rdof_repeater_nak(&BR.r, cmd, err);
    else if (R.full)
        rdof_repeater_nak(&R.r, cmd, err);
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
        rdof_siteroute_add_txvr(&SR, &R.t, 0, &rcfg.cfg.tx_map);
    }
    sr_on = true;
}
/* The HBlink sinks' admit: a stream heard at the sink's frequency (ctx: a receiver channel, NULL the modem's RX with
   --mmdvm) goes up when its path's policy sends it to HBlink. */
typedef struct chan chan_t;
static uint32_t chan_hz(const chan_t *m);
static int rf_net(void *ctx, int slot, uint32_t src, uint32_t dst, bool group)
{
    const uint32_t hz = ctx ? chan_hz(ctx) : R.rx_hz;
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
/* The streams a channel's peer was sent (below): the transceiver plays them, and its DMRK for them goes nowhere (the
 * master sent no DMRT). */
static uint8_t chan_sids[16][4];
static int chan_sid_next;
static bool chan_sid(const uint8_t *sid)
{
    for (int i = 0; i < 16; i++)
        if (!memcmp(chan_sids[i], sid, 4))
            return true;
    return false;
}
static void txvr_send(void *ctx, const uint8_t *d, size_t n)
{
    static const char *status[] = {"on air", "channel busy", "not allowed", "radio error", "transceiver busy"};
    const bool dmrk = n >= 13 && !memcmp(d, "DMRK", 4);
    if (dmrk && (chan_sid(d + 8) || (sr_on && rdof_siteroute_tuned(&SR, d + 8)))) {
        say("transceiver (a channel's call): %s\n", d[12] < 5 ? status[d[12]] : "?");
        return;
    }
    net_send(d, n);
    if (dmrk)
        say("transceiver: %s\n", d[12] < 5 ? status[d[12]] : "?");
}

/* --mmdvm: what the modem hears, through a call tracker of its own (as the board's radio), channel "rx" */
static rdof_tracker_t MT;

/* ── the network: an HBlink4 peer, as the board's ──────────────────────────────── */

static rdofnet_peer_t NET;
static bool net_on;
static rdof_hb_sink_t NET_SINK; /* --mmdvm: the modem's tracker -> DMRD (the SDR's channels have their own sinks) */

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
    rdof_hb_peer_set_power(&NET.peer, rdof_hb_role_power_dbm(&rcfg.cfg, f), now_s());
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
    if (rcfg.cfg.hb_login)
        rdofnet_peer_set_login(&NET, rcfg.cfg.hb_password, rcfg.cfg.hb_location, rcfg.cfg.hb_description,
                               rcfg.cfg.hb_url);
    rng_state = (uint32_t)time(NULL) ^ ((uint32_t)getpid() << 16) ^ (uint32_t)(now_s() * 1e6);
    if (!rng_state)
        rng_state = 1;
    if (R.mmdvm && rcfg.cfg.receiver_hblink) { /* what the modem hears goes up (the role's "hblink") */
        rdof_hb_sink_init(&NET_SINK, rcfg.peer_id);
        NET_SINK.send = sink_send;
        NET_SINK.rand32 = sink_rand;
        if (!R.full)
            NET_SINK.before_stream = sink_before;
        NET_SINK.admit = rf_admit;
        rdof_tracker_add_sink(&MT, rdof_hb_sink_as_sink(&NET_SINK));
    }
    net_on = true;
    net_config();
    fprintf(stderr, "%s: %s, peer %u, to %s:%d\n", rcfg.cfg.hb_login ? "brandmeister" : "hblink", rcfg.name,
            (unsigned)rcfg.peer_id, rcfg.cfg.master_host, rcfg.cfg.master_port);
}

/* The receiver's channels from the settings (the SDR's, as the board's SDR resolves them), all alike: each its own
 * peer (identity per_path), or what it hears goes up through the unit's peer as its path's policy says. The unit
 * (the transceiver or the repeater) hears on the one at its frequency (io_channel); a net call to any channel's
 * peer takes the one transmitter. */
struct chan {
    int c; /* the receiver's channel */
    const rdof_sdr_cfg_channel_t *ch;
    bool peer;  /* its own peer (identity per_path) */
    bool up;    /* its sink sends through the unit's peer (one identity, or the unit's own peer id) */
    bool unit;  /* its peer id is the unit's: the unit's peer is its peer */
    rdofnet_peer_t net;
    rdof_hb_sink_t sink;
    uint8_t sid[4]; /* the last stream the master sent it */
};
static uint32_t chan_hz(const chan_t *m) { return (uint32_t)llround(m->ch->freq); }
static chan_t CHN[RDOF_SDR_MAX_CHANNELS];
static int nchn;

/* The settings' channel on receiver channel c; NULL for one of our own (--monitor's "out", --mmdvm's "modem-rx" and
 * "modem-tx") */
static const chan_t *chan_at(int c)
{
    for (int i = 0; i < nchn; i++)
        if (CHN[i].c == c)
            return &CHN[i];
    return NULL;
}

/* Whether it shows a peer on the page and in the status, and whether that peer is up */
static bool chan_has_peer(const chan_t *m) { return m->peer || m->unit; }
static bool chan_peer_up(const chan_t *m) { return m->peer ? m->net.peer.up : m->unit && net_on && NET.peer.up; }

static void chan_send(void *ctx, const uint8_t *d, size_t n) { rdofnet_peer_send(&((chan_t *)ctx)->net, d, n); }
/* Whether the transceiver may go to a channel: one of its channels (the roamer's). */
static bool chan_sends(const chan_t *m, int *cc)
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
static void chan_packet(void *ctx, const uint8_t *d, size_t n)
{
    chan_t *m = ctx;
    rdof_net_burst_t b;
    int cc;
    if (sr_on && rdof_siteroute_dmrd(&SR, sr_hblink, m->net.peer.peer_id, d, n, now_s()))
        return; /* by the call policies (identity per_path: this peer's paths) */
    /* a simplex channel: what the master sends this channel's peer goes out on it, through the transceiver (one
       channel at a time: a call while it's busy elsewhere is refused, as a DMRT would be) */
    if (rdof_hb_kind(d, n) == 'D' && chan_sends(m, &cc) && rdof_parse_dmrd(d, n, &b)) {
        if (memcmp(m->sid, b.stream_id, 4)) {
            memcpy(m->sid, b.stream_id, 4);
            memcpy(chan_sids[chan_sid_next], b.stream_id, 4);
            chan_sid_next = (chan_sid_next + 1) % 16;
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

static void chan_start(void)
{
    if (!rcfg.has_hblink)
        return;
    const rdof_location_t *loc = rcfg.cfg.has_location ? &rcfg.cfg.location : NULL;
    for (int i = 0; i < nchn; i++) {
        chan_t *m = &CHN[i];
        /* the unit's peer id (per_path gives the unit its frequency's): a second login with it would take the
           unit's away at the master, so it's the same peer */
        m->unit = net_on && m->ch->hblink && m->ch->peer_id >= 0 && (uint32_t)m->ch->peer_id == rcfg.peer_id;
        if (net_on && (!m->ch->hblink || m->unit)) {
            /* one identity: what it hears goes up through the unit's peer, as its policy says */
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
        m->net.on_packet = chan_packet;
        m->net.say = net_say;
        m->net.ctx = m;
        if (rcfg.cfg.hb_login)
            rdofnet_peer_set_login(&m->net, rcfg.cfg.hb_password, rcfg.cfg.hb_location, rcfg.cfg.hb_description,
                                   rcfg.cfg.hb_url);
        rdof_hb_sink_init(&m->sink, (uint32_t)m->ch->peer_id);
        m->sink.send = chan_send;
        m->sink.rand32 = sink_rand;
        m->sink.admit = rf_admit;
        m->sink.ctx = m;
        const rdof_sink_t sk = rdof_hb_sink_as_sink(&m->sink);
        rdof_sdr_rx_add_sink(&R.rx, m->c, &sk);
        uint8_t pkt[RDOF_DMRC_MAX];
        rdofnet_peer_set_config(&m->net, pkt,
                                rdof_hb_channel_dmrc(m->net.peer.peer_id,
                                                     rcfg.cfg.hb_login && rcfg.cfg.callsign[0] ? rcfg.cfg.callsign
                                                                                               : m->ch->id,
                                                     (uint32_t)llround(m->ch->freq),
                                                     m->ch->cc, loc, pkt),
                                now_s());
        m->peer = true;
        fprintf(stderr, "%s: %s, peer %u, to %s:%d\n", rcfg.cfg.hb_login ? "brandmeister" : "hblink", m->ch->id,
                (unsigned)m->net.peer.peer_id,
                rcfg.cfg.master_host, rcfg.cfg.master_port);
    }
}

/* The receiver channel the unit hears on now (the transceiver's channel, the repeater's RX); -1 none, or the modem
 * hears (--mmdvm) */
static int unit_channel(void) { return R.mmdvm ? -1 : channel_of(heard_hz()); }
/* The receiver channel called id; -1 none */
static int channel_named(const char *id)
{
    for (int c = 0; c < NCH; c++)
        if (!strcmp(CH[c].id, id))
            return c;
    return -1;
}
/* What a tracker's output (channel id) is: the unit's (what it hears: --mmdvm's modem, or the channel it's on), a
 * channel's from the settings (the SDR's), or both */
static bool is_unit(const char *ch)
{
    const int c = channel_named(ch);
    return R.mmdvm ? c < 0 && !strcmp(ch, "rx") : c >= 0 && c == unit_channel();
}
static bool is_sdr(const char *ch)
{
    const int c = channel_named(ch);
    return c >= 0 && chan_at(c) != NULL;
}

static void net_poll(double now)
{
    if (net_on)
        rdofnet_peer_poll(&NET, now);
    for (int i = 0; i < nchn; i++)
        if (CHN[i].peer)
            rdofnet_peer_poll(&CHN[i].net, now);
}

static void net_close(void)
{
    if (net_on)
        rdofnet_peer_close(&NET);
    for (int i = 0; i < nchn; i++)
        if (CHN[i].peer)
            rdofnet_peer_close(&CHN[i].net);
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

/* ── MQTT: the board's topics (esp_idf/main/roam_mqtt.c for the radio, sdr_mqtt.c for the SDR's channels) ─ */

#ifdef RDOF_MQTT
static rdofmq_t *MQ, *MQ_SDR; /* the radio's connection; the SDR's channels' (the board's SDR's own) */
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
    snprintf(out, cap, "%s/%s/%s/%s", rcfg.cfg.mqtt_root, ch, rcfg.sdr_app, kind);
}

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
    for (int i = 0; i < nchn; i++) {
        const rdof_sdr_cfg_channel_t *c = CHN[i].ch;
        rdof_sdr_receiver_json(c->name[0] ? c->name : c->id, c->freq, c->cc, rcfg.sdr_name, c->tg, loc, buf,
                               sizeof buf);
        mq_sdr_topic(c->id, "json", t, sizeof t);
        rdofmq_publish(MQ_SDR, t, buf, -1, 1, true);
    }
}

/* the SDR's status: every receiver channel, the channels' peers */
static void mq_sdr_publish_status(void)
{
    static char buf[3072];
    const char *names[RDOF_SDR_MAX_CHANNELS] = {0}, *ids[RDOF_SDR_MAX_CHANNELS];
    bool up[RDOF_SDR_MAX_CHANNELS];
    int nh = 0;
    for (int i = 0; i < nchn; i++) {
        names[CHN[i].c] = CHN[i].ch->name[0] ? CHN[i].ch->name : NULL;
        if (chan_has_peer(&CHN[i]))
            ids[nh] = CHN[i].ch->id, up[nh++] = chan_peer_up(&CHN[i]);
    }
    rdof_sdr_status_t st = {.name = rcfg.sdr_name, .at = wall_s(), .cpu_load = NAN, .overflows = mq_overflows,
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
/* a sink on each channel, and with --mmdvm on the modem's tracker */
static void mq_calls_start(void)
{
    if (!MQ || sr_mqtt < 0)
        return;
    for (int i = R.mmdvm ? -1 : 0; i < nchn; i++) {
        rdof_hb_sink_t *k = &mq_sinks[i + 1];
        rdof_hb_sink_init(k, rcfg.site.id);
        k->send = mq_call_send;
        k->rand32 = sink_rand;
        k->admit = mq_call_admit;
        k->ctx = i >= 0 ? &CHN[i] : NULL;
        const rdof_sink_t sk = rdof_hb_sink_as_sink(k);
        if (i >= 0)
            rdof_sdr_rx_add_sink(&R.rx, CHN[i].c, &sk);
        else
            rdof_tracker_add_sink(&MT, sk);
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
    if (nchn && mq_report) {
        snprintf(mq_sdr_status, sizeof mq_sdr_status, "%s/_sdr/%s/status", c->mqtt_root, rcfg.sdr_name);
        snprintf(sid, sizeof sid, "rdof-sdr-%s", rcfg.sdr_name);
        for (int k = 0; k < 2; k++) /* aliases: every channel's raw, then every channel's json */
            for (int i = 0; i < nchn; i++) {
                mq_sdr_topic(CHN[i].ch->id, k ? "json" : "raw", mq_sdr_alias[k * nchn + i], sizeof mq_sdr_alias[0]);
                mq_sdr_alias_p[k * nchn + i] = mq_sdr_alias[k * nchn + i];
            }
        o.client_id = sid, o.will_topic = mq_sdr_status, o.alias_topics = mq_sdr_alias_p, o.n_alias = 2 * nchn;
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
            mq_sdr_next = now + (rcfg.sdr_status_s > 1 ? rcfg.sdr_status_s : 30);
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
    if (!mq_report)
        return;
    int n = -1;
    if (is_sdr(ch) && MQ_SDR) {
        n = (int)rdof_rawburst_encode(r, buf);
        mq_sdr_topic(ch, "raw", t, sizeof t);
        rdofmq_publish(MQ_SDR, t, buf, n, 0, false);
    }
    if (is_unit(ch) && MQ) {
        if (n < 0)
            n = (int)rdof_rawburst_encode(r, buf);
        mq_topic("raw", t, sizeof t);
        rdofmq_publish(MQ, t, buf, n, 0, false);
    }
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
    if (mq_report && is_sdr(ch) && MQ_SDR) {
        mq_sdr_topic(ch, "json", t, sizeof t);
        rdofmq_publish(MQ_SDR, t, json, -1, 1, false);
    }
    if (mq_report && is_unit(ch) && MQ) {
        mq_topic("json", t, sizeof t);
        rdofmq_publish(MQ, t, json, -1, 1, false);
    }
#endif
#ifdef RDOF_HOST_WEB
    const bool sdr = is_sdr(ch);
    if (sdr) /* a channel: the SDR's, as on the board */
        rdof_hostweb_tracker_json("sdr", rcfg.box, ch, json);
    if (rdofradio_cfg_heard_unit(sdr, is_unit(ch))) /* the unit's own only off the SDR's channels: one entry a call */
        rdof_hostweb_tracker_json(R.full ? "repeater" : "roamer", rcfg.name, NULL, json);
#endif
    if (!quiet)
        printf("%s json %s\n", ch, json);
}

/* the calls on the channel the transceiver is on: its listen-before-talk, as the board's roamer (a reply to the call
 * just heard isn't held off by that call); with --mmdvm the modem's tracker does it (mm_on_call) */
static void on_call(void *ctx, const char *ch, const char *action, uint32_t src, uint32_t dst)
{
    if (!R.full && !R.mmdvm && is_unit(ch))
        rdof_txvr_heard_call(&R.t, action, src, dst);
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

/* The bench's tune (below, bl_tune) */
static const char *const TAP_ID[RDOF_BL_TAPS] = {"T1", "T2", "T3", "T4"};
#define TU_SETTLE_S 0.5 /* after the LO moves: what's received before is the AD9363 settling */
static struct {
    pthread_mutex_t mu;
    int want;                  /* 1 a tune asked, 2 an untune asked */
    rdof_bl_tune_t t;
    int state;                 /* tune_state's: 1 applied, 0 under way, -1 refused */
    char why[160];
    /* the receiver's thread's (CH changes under mu, for bl_freqs) */
    bool on, moved;
    double center0;            /* the settings' center */
    bool gained;               /* a tune set the RX gain: gain_db until its untune, the settings' after */
    double gain_db;
    int ntap, tap[RDOF_BL_TAPS];
    rdof_sdr_channel_t saved[RDOF_BL_TAPS];
    uint64_t hist_from;
} TU = {.mu = PTHREAD_MUTEX_INITIALIZER, .state = 1};

static int tap_of(int c)
{
    for (int i = 0; i < TU.ntap; i++)
        if (TU.tap[i] == c)
            return i;
    return -1;
}

static void bl_window(void *ctx, rdof_bl_window_t *w)
{
    memset(w, 0, sizeof *w);
    w->running = BL.ring.w > 0 && BL.ring.w >= TU.hist_from; /* the LO settles after a tune moves it */
    w->center_hz = R.rx.center_hz;
    w->fs = R.rate;
    w->lo_hz = R.rx.center_hz - 0.4 * R.rate;
    w->hi_hz = R.rx.center_hz + 0.4 * R.rate;
    w->gain_db = dev ? dev->rx_gain(dev) : 0;
    w->ppm = w->lo_ppm = w->fs_ppm = xtal(); /* nothing in the device takes it out */
    w->ppm_source = isfinite(xtal_ppm) ? "setting" : NULL;
    w->ppm_tol = isfinite(xtal_ppm) ? xtal_tol : 0;
    w->iq_scale_db = 20 * log10(BL.ring.scale > 0 ? BL.ring.scale : 1);
    /* tx_lo_hz, tx_hi_hz 0: a job tunes anywhere, a repeater's TX LO moving for it (owner_yield) */
}

static long bl_iq_read(void *ctx, uint64_t sc, int8_t *out, long n, uint64_t *oldest, uint64_t *next)
{
    pthread_mutex_lock(&BL.mu);
    const uint64_t from = TU.hist_from; /* the history from before the LO moved isn't this window's */
    const long k = sc < from ? 0 : rdof_bl_ring_read(&BL.ring, sc, out, n, oldest, next);
    if (sc < from && next)
        *next = BL.ring.w;
    if (oldest && *oldest < from)
        *oldest = from;
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

/* --fpga-chz: no wideband history, but each channel's: the block's output instants for the last BLH_S, each at its
 * wideband sample, with the NCO offset each channel was mixed from (as the wideband ring keeps 2.5 s for its
 * pre-roll and a floor learned from before a request: 15 channels' are about 130 bytes an instant, 16 MB, where the
 * wideband ring at 19.872 MS/s is 99 MB). A bench op reads one channel's (bl_ch_*: the handle is the channel). */
#define BLH_S 2.5
#define BLH_EDGE 6250 /* a channel's passband either side of its frequency (12.5 kHz channels): a frequency in it
                         with 3 kHz of it left is in the channel */
static struct {
    long cap;
    uint64_t n;       /* instants written */
    uint64_t *sc;     /* [cap]: each one's wideband sample */
    bool *fill;       /* [cap]: a lost one's zeros */
    rdof_iq16_t *y;   /* [cap][C] */
    float *nco;       /* [cap][C]: each channel's NCO offset (wideband Hz from the center) */
} BLH;

static int bl_ch_open(void *ctx, double hz, double seconds, double *bw)
{
    int c = -1;
    for (int k = 0; k < R.rx.C; k++)
        if (fabs(CH[k].freq_hz - hz) <= BLH_EDGE - 3000 && (c < 0 || fabs(CH[k].freq_hz - hz) < fabs(CH[c].freq_hz - hz)))
            c = k;
    if (c < 0 || !FX.on || !BLH.cap)
        return -1;
    *bw = 2 * (BLH_EDGE - fabs(CH[c].freq_hz - hz));
    return c;
}

static long bl_ch_read(void *ctx, int c, uint64_t sc, rdof_bl_chs_t *out, long n, uint64_t *oldest, uint64_t *next)
{
    pthread_mutex_lock(&BL.mu);
    const uint64_t cap = (uint64_t)BLH.cap, first = BLH.n > cap ? BLH.n - cap : 0;
    *next = BL.ring.w;
    *oldest = first < BLH.n ? BLH.sc[first % cap] : BL.ring.w;
    if (*oldest < TU.hist_from) /* the history from before the LO moved isn't this window's */
        *oldest = TU.hist_from;
    if (sc < TU.hist_from)
        sc = TU.hist_from;
    uint64_t lo = first, hi = BLH.n; /* the first instant at or after sc: they run in order */
    while (lo < hi) {
        const uint64_t mid = lo + (hi - lo) / 2;
        if (BLH.sc[mid % cap] < sc)
            lo = mid + 1;
        else
            hi = mid;
    }
    const float g = RDOF_CHZFX_OUT_SCALE * BL.ring.scale; /* the history's units: a cs8 sample over 127 */
    long k = 0;
    for (; k < n && lo + (uint64_t)k < BLH.n; k++) {
        const uint64_t i = (lo + (uint64_t)k) % cap;
        const rdof_iq16_t y = BLH.y[i * (uint64_t)FX.C + (uint64_t)c];
        out[k] = (rdof_bl_chs_t){.sc = BLH.sc[i], .i = y.i * g, .q = y.q * g,
                                 .nco_hz = BLH.nco[i * (uint64_t)FX.C + (uint64_t)c], .filler = BLH.fill[i]};
    }
    pthread_mutex_unlock(&BL.mu);
    return k;
}

static void bl_ch_close(void *ctx, int c) {}

/* This turn's output instants (k of them, from FX.turn) into the channels' history, before bench_feed counts them:
 * each with each channel's NCO offset then (a step from the instant it takes effect) */
static void bench_chan_feed(int k)
{
    if (BL.bl && !BLH.cap) { /* the history, once the bench runs */
        const long cap = (long)(BLH_S * 48000);
        BLH.sc = malloc(sizeof *BLH.sc * (size_t)cap);
        BLH.fill = malloc(sizeof *BLH.fill * (size_t)cap);
        BLH.y = malloc(sizeof *BLH.y * (size_t)cap * (size_t)FX.C);
        BLH.nco = malloc(sizeof *BLH.nco * (size_t)cap * (size_t)FX.C);
        if (BLH.sc && BLH.fill && BLH.y && BLH.nco)
            BLH.cap = cap;
        else
            fprintf(stderr, "bench: no memory for the channels' history\n");
    }
    if (BLH.cap)
        pthread_mutex_lock(&BL.mu);
    for (int i = 0; i < k; i++) {
        const long long at = FX.base + FX.turn + i;
        for (int c = 0; c < FX.C; c++) { /* the NCO steps as they take effect, kept or not */
            struct fx_nco *q = &FX.nco[c];
            while (q->n && q->q[0].from <= at) {
                q->hz = q->q[0].hz;
                memmove(q->q, q->q + 1, sizeof q->q[0] * (size_t)--q->n);
            }
        }
        if (!BLH.cap)
            continue;
        const uint64_t j = BLH.n++ % (uint64_t)BLH.cap;
        BLH.sc[j] = BL.ring.w + (uint64_t)i * FX.step;
        BLH.fill[j] = FX.fl[FX.turn + i];
        memcpy(BLH.y + j * (uint64_t)FX.C, FX.y + (size_t)(FX.turn + i) * FX.C, sizeof *BLH.y * (size_t)FX.C);
        for (int c = 0; c < FX.C; c++)
            BLH.nco[j * (uint64_t)FX.C + (uint64_t)c] = (float)FX.nco[c].hz;
    }
    if (BLH.cap)
        pthread_mutex_unlock(&BL.mu);
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

/* --bench-tx MHZ[,LOW-HIGH...]: where the bench's jobs may transmit, frequencies and bands (none: no transmit
 * jobs). */
static double bench_tx_mhz[RDOF_BENCHTX_MAX_FREQS];
static int bench_ntx;
static double bench_band_hz[RDOF_BL_TX_BANDS][2];
static int bench_nband;
/* A job's power: an SDR sends at the transmit power (--tx-power); a modem at the job's RF level under --rf-level, a
 * dBm on its device's map (tx.min_dbm / tx.max_dbm, with --config) */
static rdof_txmap_t bench_map;
static int bench_level(rdof_bench_power_t p)
{
    const rdof_bench_limit_t lim = {false, 0, mm_rf_level};
    return rdof_bench_power_level(&lim, p, &bench_map);
}
/* The note for a job's power; "" when it's what's sent. */
static void bench_note(rdof_bench_power_t p, char *why, size_t cap)
{
    char asked[48];
    if (!isnan(p.dbm))
        snprintf(asked, sizeof asked, "power_dbm %.1f", p.dbm);
    else
        snprintf(asked, sizeof asked, "--power %d", p.level);
    why[0] = 0;
    if (R.mmdvm && !isnan(p.dbm) && !rdof_txmap_valid(&bench_map))
        snprintf(why, cap, "%s on the MMDVM modem: RF level 0, the least (no tx.min_dbm / tx.max_dbm for it)", asked);
    else if (R.mmdvm)
        snprintf(why, cap, "%s on the MMDVM modem: RF level %d%s", asked, bench_level(p),
                 bench_level(p) == mm_rf_level ? " (--rf-level)" : "");
    else if (!isnan(p.dbm) || p.level != 255)
        snprintf(why, cap, "%s isn't applied: the SDR transmits at %.1f dBm (--tx-power)", asked, gain_dbm(txg_full));
}

static bool bench_tx_listed(double hz)
{
    for (int i = 0; i < bench_ntx; i++)
        if (fabs(bench_tx_mhz[i] * 1e6 - hz) < 100.0)
            return true;
    for (int i = 0; i < bench_nband; i++)
        if (hz >= bench_band_hz[i][0] && hz <= bench_band_hz[i][1])
            return true;
    return false;
}

static int bl_tx_bands(void *ctx, double (*out)[2], int max)
{
    int k = 0;
    for (; k < bench_nband && k < max; k++)
        out[k][0] = bench_band_hz[k][0], out[k][1] = bench_band_hz[k][1];
    return k;
}

static int bl_freqs(void *ctx, rdof_bl_freq_t *out, int max)
{
    int k = 0;
    pthread_mutex_lock(&TU.mu); /* a tune changes CH */
    for (int c = 0; c < NCH && k < max; c++) {
        if (TU.moved && tap_of(c) < 0) { /* the LO moved for a job: only its taps hear where CH says */
            if (bench_tx_listed(CH[c].freq_hz)) /* still a transmit frequency (a repeater job's output) */
                out[k] = (rdof_bl_freq_t){.freq_hz = CH[c].freq_hz, .ch = -1, .caps = RDOF_BL_CAP_TX},
                snprintf(out[k].id, sizeof out[k].id, "%s", CH[c].id), k++;
            continue;
        }
        out[k] = (rdof_bl_freq_t){.freq_hz = CH[c].freq_hz, .ch = c,
                                  .caps = RDOF_BL_CAP_RX | RDOF_BL_CAP_DMR | RDOF_BL_CAP_IQ |
                                          (bench_tx_listed(CH[c].freq_hz) ? RDOF_BL_CAP_TX : 0)};
        snprintf(out[k].id, sizeof out[k].id, "%s", CH[c].id);
        k++;
    }
    pthread_mutex_unlock(&TU.mu);
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
    if (!bench_ntx && !bench_nband)
        return "no transmit frequencies (--bench-tx)";
    return NULL;
}

/* --mmdvm: an MMDVM modem on a serial port is what transmits. */
static const char *bl_station(void *ctx, char *model, size_t cap)
{
    snprintf(model, cap, "%.*s", (int)(cap > 1 ? cap - 1 : 0), R.client.version);
    return "mmdvm";
}

/* What an MMDVM modem physically can't do (the SDR beside it only receives). */
static const char *bl_cannot(void *ctx, const char *what)
{
    if (!strcmp(what, "signal_tx"))
        return "an MMDVM modem's serial protocol sends DMR bursts: no FM, AM or arbitrary I/Q, and the SDR beside it "
               "only receives";
    if ((!strcmp(what, "repeater") || !strcmp(what, "outbound")) && rdof_modem_simplex_only(R.client.version))
        return "this modem has one transceiver (a simplex hotspot): a repeater or an outbound needs a duplex modem";
    return NULL;
}

/* An outbound job (bench_tx outbound, bench-api.md 14.3), as a transmit job: the bench's thread asks, the main loop
 * keys the SDR modem as a repeater's TX half with no traffic (rdof_sdrmodem_outbound_frames: idle bursts and the
 * CACH on both slots), copies its slot clock out each turn, and at the end unkeys, drops the role and gives the modem
 * back to the transceiver. */
static struct {
    pthread_mutex_t mu;     /* all of it: the main loop writes active, keyed and the clock, the bench's thread reads */
    int want;               /* 1 open, 2 unkey, 3 close */
    bool active, unkeyed, failed;
    char why[120];
    rdof_bl_ob_cfg_t cfg;
    double asked;
    rdof_bl_ob_state_t s;   /* the slot clock, as the main loop last saw it */
    int state;              /* ob_state's answer from s */
    bool mapped;            /* the key-up's edge was heard back: modem sample map_pos was on the air at map_t */
    int64_t map_pos;
    double map_t, keyed_at; /* first seen keyed (0: not yet) */
    int ch;                 /* --mmdvm: the receiver channel the outbound is heard on, -1 none */
    int64_t key48;          /* --mmdvm: the receiver's 48 kS/s sample when the modem keyed */
    int64_t slot0_48;       /* --mmdvm: where its slot 0 started (a heard burst's slot, else key48) */
    struct {                /* voice bursts the bench's thread pushed, for the main loop to write (bench_ob_fast) */
        uint8_t slot, control, burst[33];
    } q[32];
    unsigned qhead, qtail;
} BO = {.mu = PTHREAD_MUTEX_INITIALIZER};

/* A transmit job: the bench's thread asks (the engine's tx hooks), the main loop does it, since the SDR modem and
 * the transceiver over it are the main loop's. The transceiver pauses for the job (no polls: the network's calls
 * wait), rdof_benchtx_t drives the modem in DMO, then the modem is set up again and the transceiver goes on. */
static struct {
    pthread_mutex_t mu;     /* all but tx, which is the main loop's */
    int want;               /* 1 open, 2 close */
    bool active, ready, failed, idle;
    char why[120];
    uint32_t freq;
    int cc, power;
    double asked;
    rdof_benchtx_item_t in[RDOF_BENCHTX_QUEUE]; /* pushed, not yet in the job's queue */
    int nin, queued;
    rdof_benchtx_t tx;
    int heard_ch;     /* the receive channel that hears the job, -1 none */
} BT = {.mu = PTHREAD_MUTEX_INITIALIZER, .heard_ch = -1};
static bool bt_ticking; /* the main loop holds BT.mu (a modem's reply can come back inside the tick) */

static void last_job_closes(void);
static bool bl_tx_open(void *ctx, uint32_t freq, int cc, rdof_bench_power_t power, bool as_built, char *why,
                       size_t cap)
{
    last_job_closes();
    pthread_mutex_lock(&BT.mu);
    const bool busy = BT.want || BT.active || BR.want || BR.active || BO.want || BO.active;
    if (!busy) {
        BT.want = 1, BT.freq = freq, BT.cc = cc, BT.asked = now_s();
        BT.ready = BT.failed = BT.idle = false;
        BT.nin = BT.queued = 0;
    }
    pthread_mutex_unlock(&BT.mu);
    if (busy)
        snprintf(why, cap, "the transmitter is still busy with the last job");
    else
        bench_note(power, why, cap);
    if (!busy)
        BT.power = bench_level(power);
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

static size_t bl_tx_event(void *ctx, char *out, size_t cap)
{
    pthread_mutex_lock(&BT.mu);
    const size_t n = BT.active ? rdof_benchtx_next_event(&BT.tx, out, cap) : 0;
    pthread_mutex_unlock(&BT.mu);
    return n;
}

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

static void bt_to_modem(void *ctx, const uint8_t *b, size_t n)
{
    if (R.mmdvm)
        mm_write(b, n);
    else
        rdof_sdrmodem_write(&R.m, b, n);
}
static void bt_to_tool(void *ctx, const char *json, size_t n) {}
static void bt_keying(void *ctx, double start, double seconds, int bursts, const char *why)
{
    say("bench: keyed %.2f s, %d bursts (%s)\n", seconds, bursts, why);
}

/* The modem's replies while a job has it (the main loop's thread, as BT.active's writes). */
static bool bench_modem_reply(const uint8_t *f, size_t n)
{
    if (BO.active) { /* an outbound's: the setup's ACKs swallowed, a NAK fails it */
        if (n >= 5 && f[2] == RDOF_MMDVM_NAK) {
            pthread_mutex_lock(&BO.mu);
            if (!BO.failed) {
                BO.failed = true;
                snprintf(BO.why, sizeof BO.why, "the modem refused command 0x%02X (reason %u)", f[3], f[4]);
            }
            pthread_mutex_unlock(&BO.mu);
        }
        return true;
    }
    if (!BT.active)
        return false;
    if (!bt_ticking)
        pthread_mutex_lock(&BT.mu); /* its measurements are read by the bench's thread */
    rdof_benchtx_modem(&BT.tx, f, n, now_s());
    if (!bt_ticking)
        pthread_mutex_unlock(&BT.mu);
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

/* The engine lets a job go (its close hook) before the main loop has given the transmitter back: the next job's open
 * waits for that (3 s at most) instead of failing "still busy with the last job". */
static bool ms_disarming(void);
static bool job_closing(void)
{
    if (ms_disarming()) /* a responder's disarm reaches the main loop on its next turn */
        return true;
    pthread_mutex_lock(&BS.mu);
    pthread_mutex_lock(&BT.mu);
    pthread_mutex_lock(&BO.mu);
    pthread_mutex_lock(&BR.mu);
    const bool c = BS.want == 2 || BT.want == 2 || BO.want == 3 || BR.want == 2;
    pthread_mutex_unlock(&BR.mu);
    pthread_mutex_unlock(&BO.mu);
    pthread_mutex_unlock(&BT.mu);
    pthread_mutex_unlock(&BS.mu);
    return c;
}
static void last_job_closes(void)
{
    for (int i = 0; i < 3000 && job_closing(); i++) {
        const struct timespec ts = {0, 1000000};
        nanosleep(&ts, NULL);
    }
}

static int bl_sig_open(void *ctx, uint32_t freq, rdof_bench_power_t power, double seconds, const rdof_sig_src_t *src,
                       char *why, size_t cap)
{
    last_job_closes();
    pthread_mutex_lock(&BS.mu);
    pthread_mutex_lock(&BT.mu);
    const bool busy = BS.want || BS.active || BT.want || BT.active || BR.want || BR.active || BO.want || BO.active;
    pthread_mutex_unlock(&BT.mu);
    if (!busy) {
        BS.want = 1, BS.freq = freq, BS.src = *src, BS.asked = now_s();
        BS.failed = false, BS.state = 0, BS.why[0] = 0;
    }
    pthread_mutex_unlock(&BS.mu);
    if (busy)
        snprintf(why, cap, "the transmitter is still busy with the last job");
    else
        bench_note(power, why, cap);
    return busy ? 0 : 1;
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

static double bl_sig_rate(void *ctx) { return rdof_sdrmodem_signal_rate(&R.m); }

/* bench_tx outbound's hooks. The modem's own slot clock, its TX samples on the receiver's timeline: the key-up's edge,
 * heard back (the AD9361 hears its own transmitter), ties the modem sample it was put at to the receiver's sample
 * and so to UTC (the bursts' times as the receiver reports them). Unheard within a second, modem sample p is taken to
 * be on the air at wideband sample p plus tx_late48. */
static const char *bl_ob_can(void *ctx) { return bl_can_tx(ctx); }

static bool bl_ob_open(void *ctx, const rdof_bl_ob_cfg_t *cfg, char *why, size_t cap)
{
    last_job_closes();
    pthread_mutex_lock(&BS.mu);
    pthread_mutex_lock(&BT.mu);
    pthread_mutex_lock(&BO.mu);
    const bool busy = BO.want || BO.active || BT.want || BT.active || BR.want || BR.active || BS.want || BS.active;
    if (!busy) {
        BO.want = 1, BO.cfg = *cfg, BO.asked = now_s();
        BO.unkeyed = BO.failed = false, BO.why[0] = 0;
        BO.s = (rdof_bl_ob_state_t){.first = -1, .next = -1};
        BO.state = 0, BO.mapped = false, BO.keyed_at = 0;
        BO.qhead = BO.qtail = 0;
    }
    pthread_mutex_unlock(&BO.mu);
    pthread_mutex_unlock(&BT.mu);
    pthread_mutex_unlock(&BS.mu);
    if (busy)
        snprintf(why, cap, "the transmitter is still busy with the last job");
    else
        bench_note(cfg->power, why, cap);
    return !busy;
}

static int bl_ob_state(void *ctx, rdof_bl_ob_state_t *s, char *why, size_t cap)
{
    pthread_mutex_lock(&BO.mu);
    *s = BO.s;
    const int st = BO.failed ? -1 : BO.state;
    if (BO.failed)
        snprintf(why, cap, "%s", BO.why);
    pthread_mutex_unlock(&BO.mu);
    return st;
}

/* An outbound's voice burst (ob_push): queued for the main loop, which owns the modem; it writes it as the host's
 * DMR_DATA1 / DMR_DATA2 on its next turn (bench_ob_fast), a few ms later. A full queue drops it (counted late). */
/* --mmdvm: the outbound as a duplex modem runs it: the modem keeps its own slot clock, unseen from here, so its
 * slot 0 and the slots' numbers (even: TS1) come from a burst the SDR hears, or failing that from when it keyed. */
static int64_t mm_now48(void) { return (int64_t)((double)BL.ring.w * 48000.0 / R.rate); }

static void bl_ob_push(void *ctx, int slot, uint8_t control, const uint8_t burst[33])
{
    pthread_mutex_lock(&BO.mu);
    if (BO.qtail - BO.qhead < sizeof BO.q / sizeof BO.q[0] && !BO.unkeyed) {
        __typeof__(BO.q[0]) *e = &BO.q[BO.qtail++ % (sizeof BO.q / sizeof BO.q[0])];
        e->slot = (uint8_t)slot, e->control = control;
        memcpy(e->burst, burst, 33);
    }
    pthread_mutex_unlock(&BO.mu);
}

static void bl_ob_unkey(void *ctx)
{
    pthread_mutex_lock(&BO.mu);
    if (BO.want != 3 && (BO.want == 1 || BO.active))
        BO.want = 2;
    pthread_mutex_unlock(&BO.mu);
}

static void bl_ob_close(void *ctx)
{
    pthread_mutex_lock(&BO.mu);
    if (BO.want == 1 && !BO.active)
        BO.want = 0; /* never keyed */
    else if (BO.active)
        BO.want = 3;
    pthread_mutex_unlock(&BO.mu);
}

static void ob_clock_mm(double now)
{
    if (!BO.mapped && BO.keyed_at && now - BO.keyed_at > 1.0) { /* never heard (no channel on it): when it keyed */
        BO.mapped = true, BO.slot0_48 = BO.key48;
        say("bench: the outbound wasn't heard: its slot times are from when the modem keyed\n");
    }
    const double rate = R.rate;
    const int64_t z = (int64_t)((double)BO.slot0_48 * rate / 48000.0); /* wideband samples, as the bursts' sc */
    const double slot = rate * 0.03;
    long next = BO.s.next; /* the key-up over: the last slot queued stays the last */
    if (!BO.unkeyed) {
        next = (long)floor((double)(mm_now48() - BO.slot0_48) / 1440.0) + 1;
        if (next < 0)
            next = 0;
    }
    BO.s = (rdof_bl_ob_state_t){.keyed = !BO.unkeyed && BO.mapped, .first = 0, .next = next, .base = z,
                                .slot = slot, .rate = rate, .sync_off = (int)llround(rate * 775.0 / 48000.0),
                                .on_air = BO.mapped, .map_pos = z,
                                .map_t = BO.mapped ? bl_sc_utc(NULL, (uint64_t)z) : 0.0, .ppm = xtal()};
    BO.state = BO.unkeyed ? 2 : BO.mapped ? 1 : 0;
}

/* The outbound's slot clock copied out for the bench's thread (BO.mu held). */
static void ob_clock(double now)
{
    if (R.mmdvm) {
        ob_clock_mm(now);
        return;
    }
    const rdof_sdrmodem_t *m = &R.m;
    const bool keyed = m->keyed && m->key_k >= 0;
    if (keyed && !BO.keyed_at)
        BO.keyed_at = now;
    if (keyed && !BO.mapped && now - BO.keyed_at > 1.5) { /* the edge not heard: as the modem has it */
        BO.mapped = true, BO.map_pos = 0;
        BO.map_t = bl_sc_utc(NULL, (uint64_t)((double)(m->tx_late48 > 0 ? m->tx_late48 : 0) * R.rate / 48000));
        say("bench: the outbound's key-up wasn't heard back: its times are the modem's\n");
    }
    BO.s = (rdof_bl_ob_state_t){.keyed = keyed, .first = m->key_k, .next = m->slot_k, .base = m->base,
                                .slot = (double)rdof_mod_slot_samples(&m->mod), .rate = m->tx_fs,
                                .skips = (long)m->slot_skips, .last_from = m->skip_from, .last_to = m->skip_to,
                                .on_air = keyed && BO.mapped, .map_pos = BO.map_pos, .map_t = BO.map_t,
                                .ppm = xtal()};
    /* the CACH (12 symbols) and the burst's first 65.5 symbols, 10 samples a symbol at 48 kS/s */
    BO.s.sync_off = (int)llround(775.0 * m->tx_fs / 48000.0);
    BO.state = !m->keyed && m->key_k >= 0 ? 2 : keyed ? 1 : 0;
}

/* A bench job comes first: the modem's owner yields at once (the owner's rule: a bench command never waits on the
 * site's own traffic). Its transmission, a transceiver's call or a repeater's, is cut and what it queued dropped; a
 * full duplex repeater stops, the modem goes simplex on the repeater's RX channel, and the TX LO may move for the job
 * (io_tune). Only an MMDVM modem's call is waited for (10 s at most): its queue isn't the program's to drop.
 * 1: the job may start; 0: not yet, the owner paused while the key-down ramps out; -1: not yet, the owner runs. */
static int owner_yield(double now, double asked)
{
    if (R.mmdvm) {
        if (R.full) { /* a duplex repeater stops for the job and starts again after it (owner_resume) */
            if (!RP.on) {
                RP.on = true;
                RP.lo = R.tx_lo;
                rdof_repeater_stop(&R.r);
                say("bench: a job stops the repeater on the modem\n");
            }
            return 1;
        }
        return R.t.has_call && now - asked < 10.0 ? -1 : 1;
    }
    if (!RP.on) {
        RP.on = true;
        RP.lo = R.tx_lo;
        RP.tuned = !R.full;
        if (R.full)
            rdof_repeater_stop(&R.r);
        if (rdof_sdrmodem_transmitting(&R.m))
            say("bench: a job cuts the %s's transmission\n", R.full ? "repeater" : "transceiver");
        rdof_sdrmodem_preempt(&R.m);
    }
    if (rdof_sdrmodem_transmitting(&R.m) && now - asked < 2.0)
        return 0;
    if (!RP.tuned) {
        RP.tuned = true;
        if (!rdof_radio_tune(&R.radio, R.rx_hz, R.cc, 255))
            say("bench: the repeater's modem didn't go simplex: %s\n", R.client.error);
        say("bench: the repeater gives the transmitter to a job\n");
    }
    return 1;
}

/* The job is over: the transmitter back to its owner. A repeater's TX LO goes back and it starts again; a
 * transceiver starts again (txvr: after a DMR job, which set the modem up its own way). */
static void owner_resume(bool txvr)
{
    const bool was = RP.on;
    RP.on = false;
    if (R.full && was) {
        if (R.tx_lo != RP.lo) {
            if (!dev || !dev->set_tx_freq || dev->set_tx_freq(dev, RP.lo))
                say("bench: can't move the TX LO back to %.4f MHz: %s\n", RP.lo / 1e6, dev ? dev->error : "");
            else
                R.tx_lo = RP.lo;
        }
        if (!rdof_modem_start(&R.client, 1) || !rdof_repeater_start(&R.r))
            say("bench: the repeater didn't start again: %s\n", R.client.error);
        else
            say("bench: the transmitter is the repeater's again\n");
        return;
    }
    if (txvr && (!rdof_modem_start(&R.client, 1) || !rdof_txvr_start(&R.t)))
        say("bench: the transceiver didn't start again: %s\n", R.client.error);
    else
        say("bench: the transmitter is the transceiver's again\n");
}

/* Each main loop turn while an outbound has the modem: the voice bursts pushed since the last turn go to the modem
 * and the slot clock is copied out, so the engine sees the slot being queued within a few ms (POLL_S is 20 ms, under
 * a slot). */
static void bench_ob_fast(void)
{
    pthread_mutex_lock(&BO.mu);
    if (!BO.active) {
        pthread_mutex_unlock(&BO.mu);
        return;
    }
    while (BO.qhead != BO.qtail) {
        const __typeof__(BO.q[0]) e = BO.q[BO.qhead++ % (sizeof BO.q / sizeof BO.q[0])];
        if (BO.unkeyed)
            continue;
        uint8_t p[34], f[40];
        p[0] = e.control;
        memcpy(p + 1, e.burst, 33);
        const size_t fn = rdof_mmdvm_frame(e.slot == 1 ? RDOF_MMDVM_DMR_DATA1 : RDOF_MMDVM_DMR_DATA2, p, sizeof p, f,
                                           sizeof f);
        bt_to_modem(NULL, f, fn);
    }
    ob_clock(now_s());
    pthread_mutex_unlock(&BO.mu);
}

/* --mmdvm: the outbound's turn on a duplex modem. It keys with DMR_START; its idle outbound and CACH are the modem's
 * own, the voice bursts its host's DMR_DATA1 / DMR_DATA2 (bench_ob_fast). */
static bool bench_ob_poll_mm(double now)
{
    pthread_mutex_lock(&BO.mu);
    if (BO.want == 1 && !BO.active) {
        const int y = owner_yield(now, BO.asked);
        if (y <= 0) {
            pthread_mutex_unlock(&BO.mu);
            return y == 0;
        }
        BO.want = 0;
        const rdof_bl_ob_cfg_t c = BO.cfg;
        pthread_mutex_unlock(&BO.mu);
        /* blocking, BO.active still false: the modem's replies go to its client. Its RX frequency is unused:
           the job's own */
        const bool ok = rdof_modem_start(&R.client, 1) &&
                        rdof_radio_tune_duplex(&R.radio, c.freq_hz, c.freq_hz, c.cc, bench_level(c.power)) &&
                        rdof_modem_set_mode(&R.client, RDOF_MODE_DMR) && /* the firmware takes DMR_START only in DMR mode */
                        rdof_modem_dmr_start(&R.client, true);
        pthread_mutex_lock(&BO.mu);
        if (!ok) {
            BO.failed = true;
            snprintf(BO.why, sizeof BO.why, "the modem refused the duplex setup: %s", R.client.error);
            pthread_mutex_unlock(&BO.mu);
            owner_resume(true);
            return true;
        }
        BO.active = true, BO.keyed_at = now, BO.mapped = false;
        BO.key48 = BO.slot0_48 = mm_now48();
        BO.ch = channel_of(c.freq_hz);
        say("bench: an outbound has the modem: %.4f MHz cc %d, %.1f s\n", c.freq_hz / 1e6, c.cc, c.seconds);
    }
    if (!BO.active) {
        pthread_mutex_unlock(&BO.mu);
        return false;
    }
    if (BO.want == 2 && !BO.unkeyed) { /* the key-up ends after the slot being queued */
        BO.unkeyed = true, BO.want = 0;
        pthread_mutex_unlock(&BO.mu);
        rdof_modem_dmr_start(&R.client, false);
        pthread_mutex_lock(&BO.mu);
    }
    if (BO.want == 3) { /* over: DMR_START off, the modem back to its owner */
        const bool unkeyed = BO.unkeyed;
        pthread_mutex_unlock(&BO.mu);
        if (!unkeyed)
            rdof_modem_dmr_start(&R.client, false);
        pthread_mutex_lock(&BO.mu);
        BO.active = false, BO.want = 0;
        pthread_mutex_unlock(&BO.mu);
        owner_resume(true);
        return true;
    }
    ob_clock_mm(now);
    pthread_mutex_unlock(&BO.mu);
    return true;
}

/* --mmdvm: a burst the SDR heard while an outbound keys: the first with its CACH places the modem's slots. */
static void ob_heard(int c, const rdof_sdr_burst_t *b)
{
    pthread_mutex_lock(&BO.mu);
    if (BO.active && !BO.mapped && c == BO.ch && b->has_cach && (b->b.slot == 1 || b->b.slot == 2)) {
        const int64_t s = b->position - 120; /* the burst starts after its slot's 12-symbol CACH */
        long k = (long)floor((double)(s - BO.key48) / 1440.0 + 0.5);
        if (k < 0)
            k = 0;
        if ((k & 1) != b->b.slot - 1)
            k++;
        BO.slot0_48 = s - k * 1440;
        BO.mapped = true;
        say("bench: the outbound heard on the SDR: slot %d, slot 0 %.2f ms after the modem keyed\n", b->b.slot,
            (double)(BO.slot0_48 - BO.key48) / 48.0);
    }
    pthread_mutex_unlock(&BO.mu);
}

/* The main loop's turn for an outbound: true while one has the modem. */
static bool bench_ob_poll(double now)
{
    if (R.mmdvm)
        return bench_ob_poll_mm(now);
    pthread_mutex_lock(&BO.mu);
    if (BO.want == 1 && !BO.active) {
        const int y = owner_yield(now, BO.asked);
        if (y <= 0) {
            pthread_mutex_unlock(&BO.mu);
            return y == 0;
        }
        BO.want = 0, BO.active = true;
        const rdof_bl_ob_cfg_t c = BO.cfg;
        pthread_mutex_unlock(&BO.mu);
        const int s = (int)ceil(c.seconds);
        uint8_t f[160];
        const size_t n = rdof_sdrmodem_outbound_frames(c.freq_hz, c.cc, 255, s + 15, s + 10, f, sizeof f);
        rdof_sdrmodem_write(&R.m, f, n); /* the replies to bench_modem_reply: a NAK fails the job */
        say("bench: an outbound has the transmitter: %.4f MHz cc %d, %.1f s\n", c.freq_hz / 1e6, c.cc, c.seconds);
        pthread_mutex_lock(&BO.mu);
    }
    if (!BO.active) {
        pthread_mutex_unlock(&BO.mu);
        return false;
    }
    if (BO.want == 2 && !BO.unkeyed) { /* the key-up ends after the slot being queued */
        BO.unkeyed = true, BO.want = 0;
        pthread_mutex_unlock(&BO.mu); /* the modem's reply comes back through bench_modem_reply */
        const uint8_t off = 0;
        uint8_t f[8];
        rdof_sdrmodem_write(&R.m, f, rdof_mmdvm_frame(RDOF_MMDVM_DMR_START, &off, 1, f, sizeof f));
        pthread_mutex_lock(&BO.mu);
    }
    if (BO.want == 3) { /* over: the role dropped (SET_ROLE none), the modem back to the transceiver */
        pthread_mutex_unlock(&BO.mu);
        static const uint8_t off = 0, none[2] = {1, 0}; /* 0xD6 SET_ROLE 0 */
        uint8_t f[8];
        if (!BO.unkeyed)
            rdof_sdrmodem_write(&R.m, f, rdof_mmdvm_frame(RDOF_MMDVM_DMR_START, &off, 1, f, sizeof f));
        rdof_sdrmodem_write(&R.m, f, rdof_mmdvm_frame(0xD6, none, sizeof none, f, sizeof f));
        pthread_mutex_lock(&BO.mu);
        BO.active = false, BO.want = 0;
        pthread_mutex_unlock(&BO.mu);
        owner_resume(true);
        return true;
    }
    ob_clock(now);
    pthread_mutex_unlock(&BO.mu);
    return true;
}

/* The main loop's turn for a signal: true while one has the modem. */
static bool bench_sig_poll(double now)
{
    pthread_mutex_lock(&BS.mu);
    if (BS.want == 1 && !BS.active) {
        const int y = owner_yield(now, BS.asked);
        if (y <= 0) {
            pthread_mutex_unlock(&BS.mu);
            return y == 0;
        }
        BS.want = 0;
        if (rdof_sdrmodem_signal(&R.m, BS.freq, &BS.src, BS.why, sizeof BS.why)) {
            BS.active = true;
            say("bench: a signal has the transmitter: %.4f MHz\n", BS.freq / 1e6);
        } else {
            BS.failed = true;
            pthread_mutex_unlock(&BS.mu);
            owner_resume(false);
            return true;
        }
    }
    if (BS.active && BS.want == 2) {
        rdof_sdrmodem_signal(&R.m, 0, NULL, BS.why, sizeof BS.why);
        BS.want = 0, BS.active = false;
        pthread_mutex_unlock(&BS.mu);
        owner_resume(false);
        return true;
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
    if (bench_sig_poll(now) || bench_ob_poll(now))
        return true;
    pthread_mutex_lock(&BT.mu);
    if (BT.want == 1 && !BT.active) {
        const int y = owner_yield(now, BT.asked);
        if (y <= 0) {
            pthread_mutex_unlock(&BT.mu);
            return y == 0;
        }
        BT.want = 0, BT.active = true;
        const rdof_benchtx_io_t io = {bt_to_modem, bt_to_tool, bt_keying, NULL, NULL};
        bt_ticking = true;
        rdof_benchtx_init(&BT.tx, &io, BT.freq, BT.cc, R.mmdvm ? BT.power : 255, 120.0, now); /* SDR: --tx-power */
        bt_ticking = false;
        BT.heard_ch = R.mmdvm ? channel_of(BT.freq) : -1;
        static rdof_benchtx_track_t bt_track;
        rdof_benchtx_track(&BT.tx, BT.heard_ch >= 0 ? &bt_track : NULL,
                           R.mmdvm ? "no receive channel on the job's frequency: a tune with a tap there gives one"
                                   : "the SDR's own transmitter: no separate receiver hears it");
        say("bench: a job has the transmitter: %.4f MHz cc %d\n", BT.freq / 1e6, BT.cc);
    }
    if (!BT.active) {
        pthread_mutex_unlock(&BT.mu);
        return false;
    }
    if (BT.want == 2) { /* the job is over: the modem back to the transceiver (want stays 2 until it is: job_closing) */
        BT.nin = 0;
        pthread_mutex_unlock(&BT.mu);
        rdof_benchtx_stop(&BT.tx, NULL, "closed", now);
        pthread_mutex_lock(&BT.mu);
        BT.active = false, BT.want = 0;
        pthread_mutex_unlock(&BT.mu);
        owner_resume(true);
        return true;
    }
    for (int i = 0; i < BT.nin && BT.tx.qn < RDOF_BENCHTX_QUEUE; i++)
        BT.tx.q[(BT.tx.qh + BT.tx.qn++) % RDOF_BENCHTX_QUEUE] = BT.in[i];
    BT.nin = 0;
    rdof_benchtx_alive(&BT.tx, now); /* the job's connection is its dead-man */
    bt_ticking = true;
    rdof_benchtx_tick(&BT.tx, now);
    bt_ticking = false;
    BT.ready = BT.tx.state == RDOF_TXS_READY;
    BT.queued = BT.tx.qn;
    BT.idle = !BT.tx.keyed && !BT.tx.qn && rdof_benchtx_settled(&BT.tx);
    if (BT.tx.state == RDOF_TXS_DONE && !BT.failed) {
        BT.failed = true;
        snprintf(BT.why, sizeof BT.why, "%s", BT.tx.error[0] ? BT.tx.error : "the modem stopped");
    }
    pthread_mutex_unlock(&BT.mu);
    return true;
}

/* The confirmed-data responder (the bench's mssim op, rdof/mssim.h): the bench's thread arms it and reads its log;
 * the main loop feeds it the bursts heard on the job's channel and places its answers on the receiver's timeline,
 * over a transmit job of its own (BT: the modem in DMO on the frequency, the transceiver paused). */
static struct {
    pthread_mutex_t mu;
    int want;     /* 1 arm once the job has the transmitter, 2 disarm */
    bool on;
    double until; /* disarmed then */
    int ch;       /* the receive channel on the frequency */
    uint32_t freq;
    char why[160]; /* why the last arming ended */
    int64_t late48; /* the modem's tx_late48 before arming, put back after */
    int64_t cal48;  /* the calibration burst's place, -1 heard back (or none) */
    double cal_at;
    rdof_mssim_profile_t prof;
    rdof_mssim_t s;
} MS = {.mu = PTHREAD_MUTEX_INITIALIZER, .ch = -1};

static bool ms_disarming(void)
{
    pthread_mutex_lock(&MS.mu);
    const bool d = MS.want == 2;
    pthread_mutex_unlock(&MS.mu);
    return d;
}

#define MS_LEAD48 1440 /* answers go to the modulator this far ahead of where it is */

/* --mmdvm: an MMDVM modem can't place a burst at a sample: the answers go to it when they're due, this far ahead
 * (the serial port, its queue and a key-up's delay; each answer heard back corrects it) */
#define MM_LEAD48_0 9600  /* 200 ms */
static int64_t mm_lead48 = MM_LEAD48_0;
#define MM_SENT 16
static struct {
    uint8_t burst[33];
    int64_t at48;
} mm_sent[MM_SENT]; /* what went to the modem lately: a burst heard that matches is our own, heard back */
static unsigned mm_nsent;

static bool ms_send_mm(void *ctx, int64_t at48, const uint8_t burst[33], int64_t *placed48)
{
    int cc = 0, dt = 0, err = 0;
    if (!rdof_decode_slot_type(burst, &cc, &dt, &err))
        return false;
    uint8_t p[34], f[40];
    p[0] = RDOF_SYNC_DATA | (uint8_t)dt;
    memcpy(p + 1, burst, 33);
    if (!mm_write(f, rdof_mmdvm_frame(RDOF_MMDVM_DMR_DATA2, p, sizeof p, f, sizeof f)))
        return false;
    int64_t now = mm_now48() + mm_lead48;
    *placed48 = at48 > now ? at48 : now;
    memcpy(mm_sent[mm_nsent % MM_SENT].burst, burst, 33);
    mm_sent[mm_nsent++ % MM_SENT].at48 = *placed48;
    return true;
}

/* Our own answer on the SDR's channel: within 400 ms of where it was placed and within RDOF_SDRMODEM_SENT_BITS bits
 * of what was sent. */
static bool mm_is_echo(const rdof_sdr_burst_t *b)
{
    for (unsigned i = 0; i < mm_nsent && i < MM_SENT; i++) {
        if (llabs(b->position - mm_sent[(mm_nsent - 1 - i) % MM_SENT].at48) > 19200)
            continue;
        int d = 0;
        for (int k = 0; k < 33; k++)
            d += __builtin_popcount(b->b.burst[k] ^ mm_sent[(mm_nsent - 1 - i) % MM_SENT].burst[k]);
        if (d <= RDOF_SDRMODEM_SENT_BITS)
            return true;
    }
    return false;
}

static bool ms_send(void *ctx, int64_t at48, const uint8_t burst[33], int64_t *placed48)
{
    if (R.mmdvm)
        return ms_send_mm(ctx, at48, burst, placed48);
    return rdof_sdrmodem_place48(&R.m, at48, burst, placed48);
}

/* A job set the modem's transmit delay itself (its own key-up's edge, the responder's echo) or put the site's back:
 * on a full duplex repeater the drift tracker (the main loop) takes its reference again from it, not over it (it
 * had put the site's calibration plus 29 ms of drift back over a job's 52.7 ms, and the job's inbound slots came out
 * one off, 2026-11-08) */
static atomic_int late_rebase;
static int64_t path48 = -1; /* the transmit path's delay last measured from a key-up's edge (48 kS/s), -1: none yet */

/* The transmit path's delay moved by d48: the SDR modem's tx_late48, or the MMDVM path's lead. */
static void ms_late(int64_t d48)
{
    if (R.mmdvm) {
        mm_lead48 += d48;
        mm_lead48 = mm_lead48 < 240 ? 240 : mm_lead48 > 28800 ? 28800 : mm_lead48; /* 5 ms to 600 ms */
    } else {
        R.m.tx_late48 += d48;
        atomic_store(&late_rebase, 1);
    }
}

static void ms_off(const char *why)
{
    if (MS.on)
        R.m.tx_late48 = MS.late48, atomic_store(&late_rebase, 1);
    MS.on = false;
    MS.want = 0;
    rdof_mssim_disarm(&MS.s);
    snprintf(MS.why, sizeof MS.why, "%s", why);
    bl_tx_close(NULL);
    say("bench: the responder is off: %s\n", why);
}

/* the main loop, each turn before the transmitter's samples */
static void ms_poll(double now)
{
    pthread_mutex_lock(&MS.mu);
    if (MS.want == 1) {
        pthread_mutex_lock(&BT.mu);
        const bool ready = BT.ready, failed = BT.failed;
        char why[120];
        snprintf(why, sizeof why, "%s", BT.why);
        pthread_mutex_unlock(&BT.mu);
        if (failed) {
            ms_off(why);
        } else if (now > MS.until) {
            ms_off("the transmitter wasn't ready in time");
        } else if (ready) {
            rdof_mssim_init(&MS.s, ms_send, NULL);
            rdof_mssim_arm(&MS.s, &MS.prof);
            MS.want = 0;
            MS.on = true;
            /* the transmit path's delay: one idle burst, where it's heard back against where it was placed */
            MS.late48 = R.m.tx_late48;
            uint8_t idle[33], z[12] = {0};
            rdof_data_burst(z, RDOF_DT_IDLE, BT.cc, rdof_data_sync(RDOF_MS_VOICE), idle);
            int64_t at = (R.mmdvm ? mm_now48() + mm_lead48 : rdof_sdrmodem_tx_frontier48(&R.m)) + RDOF_MSSIM_SLOT48;
            int64_t placed = -1;
            MS.cal48 = ms_send(NULL, at, idle, &placed) ? placed : -1;
            MS.cal_at = now;
            say("bench: the responder is armed on %.4f MHz%s\n", MS.freq / 1e6,
                MS.cal48 >= 0 ? ", calibrating" : " (no calibration burst)");
        }
    } else if (MS.on) {
        if (MS.want == 2)
            ms_off("disarmed");
        else if (now > MS.until)
            ms_off("its time is up");
        else if (!MS.s.armed)
            ms_off("it answered its count");
        else if (R.mmdvm)
            rdof_mssim_poll(&MS.s, mm_now48(), mm_lead48);
        else
            rdof_mssim_poll(&MS.s, rdof_sdrmodem_tx_frontier48(&R.m), MS_LEAD48);
    } else if (MS.want == 2) {
        MS.want = 0;
    }
    pthread_mutex_unlock(&MS.mu);
}

static void ms_heard(int c, const rdof_sdr_burst_t *b, bool echo)
{
    if (!MS.on || c != MS.ch) /* a racy peek: on changes on this thread only */
        return;
    pthread_mutex_lock(&MS.mu);
    echo = echo || (R.mmdvm && mm_is_echo(b));
    if (MS.on) {
        if (!echo) {
            rdof_mssim_heard(&MS.s, &b->b, b->position);
        } else if (MS.cal48 >= 0 && b->b.dt == RDOF_DT_IDLE && llabs(b->position - MS.cal48) < 4 * 2880) {
            ms_late(b->position - MS.cal48);
            say("bench: the responder's transmit path is %.2f ms past its place (tx_late %.2f ms)\n",
                (double)(b->position - MS.cal48) / 48.0, (double)(R.mmdvm ? mm_lead48 : R.m.tx_late48) / 48.0);
            MS.cal48 = -1;
        } else if (b->b.dt == RDOF_DT_DATA_HEADER) {
            rdof_mssim_echo(&MS.s, b->position);
            /* an answer heard back: half its error goes into the delay (a stall in the writer moves it) */
            for (int i = 0; i < MS.s.nlog; i++) {
                const rdof_mssim_entry_t *e = &MS.s.log[i];
                if (e->air48 == b->position && e->placed48 >= 0 && llabs(e->air48 - e->placed48) > 24)
                    ms_late((e->air48 - e->placed48) / 2);
            }
        }
    }
    pthread_mutex_unlock(&MS.mu);
}

static bool bl_mssim(void *ctx, const char *action, const char *req, size_t len, uint32_t freq_hz, int cc,
                     rdof_bench_power_t power, double seconds, uint32_t since, char *out, size_t cap,
                     const char **code)
{
    if (!strcmp(action, "arm")) {
        rdof_mssim_profile_t p;
        char err[120];
        if (!rdof_mssim_parse(req, len, &p, err, sizeof err)) {
            *code = "bad_request";
            snprintf(out, cap, "%s", err);
            return false;
        }
        /* an MMDVM modem takes it too, over the transmit job's setup: its timing to the serial port's and the key-up's
           delay (mm_lead48), measured from the calibration burst and each answer heard back; the log says where they went */
        const int ch = channel_of(freq_hz);
        if (ch < 0) {
            *code = "refused";
            snprintf(out, cap, "no receive channel on %.4f MHz", freq_hz / 1e6);
            return false;
        }
        pthread_mutex_lock(&MS.mu);
        const bool busy = MS.on || MS.want;
        pthread_mutex_unlock(&MS.mu);
        char why[160] = "";
        if (busy || !bl_tx_open(ctx, freq_hz, cc, power, false, why, sizeof why)) {
            *code = "busy";
            snprintf(out, cap, "%s", busy ? "the responder is armed already" : why);
            return false;
        }
        pthread_mutex_lock(&MS.mu);
        MS.prof = p;
        MS.ch = ch;
        MS.freq = freq_hz;
        MS.until = now_s() + (seconds > 0 && seconds < 1800 ? seconds : 1800);
        MS.why[0] = 0;
        MS.want = 1;
        pthread_mutex_unlock(&MS.mu);
        char q[200];
        snprintf(q, sizeof q, "%s", why);
        for (char *s = q; *s; s++)
            if (*s == '"' || *s == '\\' || (unsigned char)*s < 0x20)
                *s = '\'';
        snprintf(out, cap, "\"armed\":true,\"freq\":%.6f,\"cc\":%d,\"note\":\"%s\"", freq_hz / 1e6, cc, q);
        return true;
    }
    pthread_mutex_lock(&MS.mu);
    if (!strcmp(action, "disarm") && (MS.on || MS.want == 1))
        MS.want = 2;
    const int n = snprintf(out, cap, "\"armed\":%s,\"arming\":%s,\"answered\":%d,\"ended\":\"%s\",\"log\":",
                           MS.on ? "true" : "false", MS.want == 1 ? "true" : "false", MS.s.answered, MS.why);
    uint32_t next = since;
    if (n > 0 && (size_t)n < cap) {
        size_t k = rdof_mssim_log_json(&MS.s, since, out + n, cap - (size_t)n - 24, &next);
        snprintf(out + n + k, cap - (size_t)n - k, ",\"next\":%u", next);
    }
    pthread_mutex_unlock(&MS.mu);
    return true;
}

static bool bl_rpt_open(void *ctx, const rdof_bl_rpt_cfg_t *cfg, char *why, size_t cap)
{
    last_job_closes();
    pthread_mutex_lock(&BR.mu);
    const bool busy = BR.want || BR.active || BT.want || BT.active || BO.want || BO.active;
    if (!busy) {
        BR.want = 1, BR.cfg = *cfg, BR.asked = now_s();
        BR.ready = BR.failed = false;
        BR.nev = BR.nwake = 0;
    }
    pthread_mutex_unlock(&BR.mu);
    if (busy)
        snprintf(why, cap, "the transmitter is still busy with the last job");
    else
        bench_note(cfg->power, why, cap);
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
        const int y = owner_yield(now, BR.asked);
        if (y <= 0) {
            pthread_mutex_unlock(&BR.mu);
            return y == 0;
        }
        BR.want = 0, BR.active = true;
        const rdof_bl_rpt_cfg_t cfg = BR.cfg;
        pthread_mutex_unlock(&BR.mu);
        /* the input's channel decodes the job's color code, not the site's: it heard nothing of a job on another */
        BR.rx_ch = channel_of(cfg.rx_hz);
        if (BR.rx_ch >= 0)
            rdof_sdr_rx_set_cc(&R.rx, BR.rx_ch, cfg.cc);
        rdof_repeater_init(&BR.r, &R.radio, rcfg.peer_id, cfg.rx_hz, cfg.tx_hz, cfg.cc);
        BR.r.keep_up = R.mmdvm; /* a modem drops its carrier when keyed with nothing queued */
        BR.r.enabled[1] = cfg.slots & 2u, BR.r.enabled[2] = cfg.slots & 4u;
        BR.r.inactivity_s = cfg.hang_s;
        rdof_repeater_set_call_hang(&BR.r, cfg.call_hang_s);
        BR.r.power = R.mmdvm ? bench_level(cfg.power) : 255; /* --rf-level caps it on the port too */
        BR.r.clock = p_clock;
        BR.r.on_key = br_on_key;
        BR.r.ctx = &R;
        /* the inbound slots follow the transmit path's delay: the first key-up's edge in what's received sets it */
        BR.late48 = R.m.tx_late48;
        BR.cal = true;
        /* until this job's own edge (missed when something else in the band raised the power before it): the path as
           last measured, not the site's tracked value, which can be a slot out */
        if (path48 >= 0)
            R.m.tx_late48 = path48, atomic_store(&late_rebase, 1);
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
        BR.nwake = 0;
        pthread_mutex_unlock(&BR.mu);
        rdof_repeater_stop(&BR.r);
        R.m.tx_late48 = BR.late48;
        atomic_store(&late_rebase, 1);
        if (BR.rx_ch >= 0)
            rdof_sdr_rx_set_cc(&R.rx, BR.rx_ch, CH[BR.rx_ch].cc);
        pthread_mutex_lock(&BR.mu);
        BR.active = false, BR.nev = 0, BR.want = 0;
        pthread_mutex_unlock(&BR.mu);
        owner_resume(true);
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
    if (FX.on) { /* no wideband I/Q: counted, not written (19.9 MS/s of zeros put took a third of the loop) */
        rdof_bl_ring_skip(&BL.ring, n);
    } else {
        rdof_bl_ring_put(&BL.ring, (const float *)in, n, &clip);
        if (filler)
            rdof_bl_ring_mark_filler(&BL.ring, n);
    }
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
    o.lag48 = RDOF_DEMOD_LAG;
    o.t = bl_sc_utc(NULL, (uint64_t)((double)b->position * R.rate / 48000.0));
    o.cfo_hz = (float)(rdof_sdr_rx_correction_at(&R.rx, c, b->position + RDOF_AFC_AT) + b->freq_offset_hz -
                       xtal_base_hz(c));
    o.dev_hz = (float)b->deviation_hz, o.level_dbfs = b->b.power_dbfs;
    o.dt = (uint8_t)b->b.dt, o.n = (int8_t)b->b.n, o.slot = (int8_t)b->b.slot, o.family = (int8_t)b->b.family;
    o.cc = (int8_t)b->b.cc, o.sync_errors = (int8_t)b->b.sync_errors;
    memcpy(o.raw, b->b.burst, sizeof o.raw);
    o.has_cach = b->has_cach;
    memcpy(o.cach, b->cach, sizeof o.cach);
    rdof_bl_burst(BL.bl, &o);
}

#ifdef RDOF_HOST_WEB
/* The page and the bench line, listed over DNS-SD (rdofradio_dnssd.c) when they listen past loopback */
static void advertise_web(int port)
{
    if (strncmp(web_addr, "127.", 4) && strcmp(web_addr, "localhost"))
        rdofdnssd_publish(rcfg.box[0] ? rcfg.box : "rdofradio", "_http._tcp", port, (const char *const[]){"path=/", NULL});
}
#endif

static const char *bl_tune(void *ctx, const rdof_bl_tune_t *t, const char *who, char *why, size_t cap);
static int bl_tune_state(void *ctx, char *why, size_t cap);
static void bl_untune(void *ctx);
static int bl_raw_start(void *ctx, double seconds, double fs, double center, char *err, size_t cap);
static int bl_raw_state(void *ctx, rdof_bl_raw_t *o, char *err, size_t cap);
static void bl_raw_read(void *ctx, long at, int16_t *out, long n);
static void bl_raw_close(void *ctx);
static int bl_raw_demod(void *ctx, bool reset, int cc, const float *iq, int n, rdof_bl_burst_t *out, int cap);

static void bench_start(void)
{
    if (bench_port < 0)
        return;
    /* --fpga-chz has no wideband I/Q to keep: a short ring of filler, which counts the samples */
    if (!rdof_bl_ring_init(&BL.ring, FX.on ? 4L * CHUNK : (long)(2.5 * R.rate))) {
        fprintf(stderr, "bench: no memory for the history\n");
        return;
    }
    /* cs8 from a wider ADC (an AD9361's 12 bits at 1/2048 a step): its low 8 bits, where a bench signal is (a call at
     * -53 dBFS is a few steps); a signal over -24 dBFS clips, and the capture's clip_pct says so. An 8-bit device
     * (an RTL-SDR) keeps its own 8 bits: at the wider ADC's gain a signal over -24 dBFS, which an RTL-SDR's gain puts
     * in reach on the bench, clipped every capture, trigger and rx */
    BL.ring.scale = dev && dev->iq_scale > 0 ? dev->iq_scale : 16;
    static const rdof_bl_platform_t pf = {.device = bl_device, .window = bl_window, .freqs = bl_freqs,
                                          .tx_bands = bl_tx_bands,
                                          .iq_read = bl_iq_read, .iq_filled = bl_iq_filled, .utc = bl_utc,
                                          .sc_utc = bl_sc_utc,
                                          .rx_stats = bl_rx_stats, .can_tx = bl_can_tx, .tx_open = bl_tx_open, .mssim = bl_mssim,
                                          .tx_space = bl_tx_space, .tx_push = bl_tx_push, .tx_unkey = bl_tx_unkey,
                                          .tx_idle = bl_tx_idle, .tx_close = bl_tx_close, .tx_event = bl_tx_event, .rpt_open = bl_rpt_open,
                                          .rpt_state = bl_rpt_state, .rpt_event = bl_rpt_event,
                                          .rpt_wake = bl_rpt_wake, .rpt_close = bl_rpt_close,
                                          .modes = rdof_sdr_modes, .sig_open = bl_sig_open,
                                          .sig_state = bl_sig_state, .sig_close = bl_sig_close,
                                          .sig_rate = bl_sig_rate, .ob_open = bl_ob_open, .ob_state = bl_ob_state,
                                          .ob_unkey = bl_ob_unkey, .ob_close = bl_ob_close, .ob_can = bl_ob_can,
                                          .ob_push = bl_ob_push, .tune = bl_tune, .tune_state = bl_tune_state,
                                          .untune = bl_untune};
    rdof_bl_platform_t p = pf;
    if (R.mmdvm) { /* the modem transmits DMR only: no signal modes */
        p.sig_open = NULL, p.sig_state = NULL, p.sig_close = NULL;
        p.station = bl_station, p.cannot = bl_cannot;
        p.rpt_own_rx = true; /* the repeater job hears on the modem's own receiver */
    }
    if (FX.on) /* no wideband I/Q: capture, trigger, track and rx take a channel's own outputs (bl_ch_*); the ring
                  stays, filler, for the sample count the bursts' times come from */
        p.iq_read = NULL, p.iq_filled = NULL, p.iq_warm = NULL, p.ch_open = bl_ch_open, p.ch_read = bl_ch_read,
        p.ch_close = bl_ch_close, p.raw_start = bl_raw_start, p.raw_state = bl_raw_state, p.raw_read = bl_raw_read,
        p.raw_close = bl_raw_close, p.raw_demod = bl_raw_demod;
    BL.bl = rdof_bl_new(&p);
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
    if (strncmp(bench_addr, "127.", 4) && strcmp(bench_addr, "localhost")) {
        char kind[32], name[64], ver[48], k[48], v[64];
        bl_device(NULL, kind, sizeof kind, name, sizeof name, ver, sizeof ver);
        snprintf(k, sizeof k, "kind=%s", kind);
        snprintf(v, sizeof v, "ver=%s", ver);
        rdofdnssd_publish(name, "_rfgbench._tcp", rdof_bnet_port(BL.net), (const char *const[]){k, v, NULL});
    }
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
    pthread_mutex_lock(&BT.mu);
    if (BT.active && c == BT.heard_ch)
        rdof_benchtx_heard(&BT.tx, b->b.burst, now_s());
    pthread_mutex_unlock(&BT.mu);
    if (R.mmdvm)
        ob_heard(c, b);
    else
        rdof_sdrmodem_heard(&R.m, c, b);
    ms_heard(c, b, false);
    if (NX)
        rdof_sdrsplits_heard(&XS, c, b);
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

/* The bench's tune (bl_tune, the tune op and a request's own): a bench job comes first, so channels are pointed at the
 * job's frequencies (taps), and the RX LO moves only when they don't fit the window as it is (a Pluto's 19.872 MS/s
 * window holds a whole plan). The bench thread asks; the receiver's thread applies it between chunks
 * (bench_tune_poll), as it does the calibration's LO. A tap takes a channel the site isn't using for its repeater or
 * transceiver (from the last one down) and gives it back after. While the LO is moved, the other channels hear
 * somewhere else: their bursts are dropped (tune_drop), and the repeater and the transceiver hear nothing. A tap's
 * bursts go to the bench, the modem and the responder, never to the network. The I/Q history from before the LO
 * moved isn't read (TU.hist_from). */
/* Channel c mixed from where CH says, against the center now, its correction its frequency's (--ppm) */
static void chan_point(int c)
{
    const double off = CH[c].freq_hz - R.rx.center_hz;
    if (FX.on)
        FX.off[c] = off; /* fx_correction takes it to the block with the correction */
    else
        rdof_chz_set_offset(&R.rx.chz, c, off);
    rdof_sdr_rx_set_correction(&R.rx, c, xtal_base_hz(c));
    rdof_sdr_rx_set_cc(&R.rx, c, CH[c].cc); /* a tap decodes any, a channel its own again */
}

static bool move_lo(double hz, char *why, size_t cap)
{
    if (!dev || !dev->set_rx_freq || dev->set_rx_freq(dev, hz)) {
        snprintf(why, cap, "the receiver's LO can't move to %.4f MHz%s%s", hz / 1e6, dev ? ": " : "",
                 dev ? dev->error : "");
        return false;
    }
    R.rx.center_hz = R.rx.cfg.center_hz = hz;
    for (int c = 0; c < NCH; c++)
        chan_point(c);
    /* an AD9363 recalibrates after a long jump and streams zeros meanwhile (a 313 MHz one, 2026-11-08: an rx's floor
       learned at -150 dBFS): the history starts, and the receiver runs for the bench, a while after */
    TU.hist_from = BL.ring.w + (uint64_t)(TU_SETTLE_S * R.rate);
    return true;
}

static double rx_gain_set = NAN; /* --gain: the settings' RX gain (NAN: the device's AGC) */

/* The settings again: the taps' channels back, the LO back */
static void tune_restore(void)
{
    int tap[RDOF_BL_TAPS];
    pthread_mutex_lock(&TU.mu);
    const int n = TU.ntap;
    for (int i = 0; i < n; i++)
        CH[tap[i] = TU.tap[i]] = TU.saved[i];
    TU.ntap = 0;
    pthread_mutex_unlock(&TU.mu);
    char why[160];
    if (TU.moved) {
        if (!move_lo(TU.center0, why, sizeof why))
            say("bench: %s\n", why);
        else
            say("bench: the receiver's LO is back at %.4f MHz\n", TU.center0 / 1e6);
    } else {
        for (int i = 0; i < n; i++)
            chan_point(tap[i]);
    }
    TU.on = TU.moved = false;
    if (TU.gained) {
        if (dev->set_rx_gain(dev, rx_gain_set))
            say("bench: the receiver's gain didn't go back: %s\n", dev->error);
        else
            say("bench: the receiver's gain is the settings' again\n");
        TU.gained = false;
    }
}

static bool in_window(double center, double f)
{
    return fabs(f - center) <= 0.4 * R.rate && fabs(f - center) >= 25e3; /* clear of the DC */
}

static bool tune_apply(const rdof_bl_tune_t *t, char *why, size_t cap)
{
    double f[RDOF_BL_TAPS];
    int nf = 0;
    if (t->ntaps < 0) /* the taps as they are */
        for (int i = 0; i < TU.ntap; i++)
            f[nf++] = CH[TU.tap[i]].freq_hz;
    else
        for (int i = 0; i < t->ntaps && i < RDOF_BL_TAPS; i++)
            f[nf++] = t->taps_hz[i];
    /* the gain this tune leaves: the one asked, the last tune's for "as it is", NAN for the settings' */
    const double gain_now = TU.gained ? TU.gain_db : NAN;
    const double want_gain = isfinite(t->gain_db) && t->gain_db != RDOF_BL_TUNE_AUTO ? t->gain_db
                           : isnan(t->gain_db) ? gain_now : NAN;
    if (TU.on)
        tune_restore();
    const double c0 = R.rx.center_hz;
    TU.center0 = c0;
    bool fit = true;
    for (int i = 0; i < nf; i++)
        fit &= in_window(c0, f[i]);
    /* the LO stays while the taps fit; else where asked (when they fit there), else an eighth of the rate under the
       first tap; "auto" with no taps: the settings */
    double center = c0;
    if (!fit || (!nf && t->center_hz > 0)) {
        center = t->center_hz > 0 ? t->center_hz : nf ? f[0] - R.rate / 8 : c0;
        for (int i = 0; i < nf; i++)
            if (!in_window(center, f[i]))
                center = f[0] - R.rate / 8;
        for (int i = 0; i < nf; i++)
            if (!in_window(center, f[i])) {
                snprintf(why, cap, "taps %.4f and %.4f MHz don't fit one window (%.2f MS/s)", f[0] / 1e6,
                         f[i] / 1e6, R.rate / 1e6);
                return false;
            }
    }
    /* the taps' channels: a channel already there, else one the site's repeater or transceiver isn't on */
    int tap[RDOF_BL_TAPS], nt = 0;
    const int keep[6] = {channel_of(R.rx_hz), channel_of(R.tx_hz), R.tx_ch, R.tx_img, R.echo_ch, R.echo_img};
    for (int i = 0; i < nf; i++) {
        const int at = center == c0 ? channel_of(f[i]) : -1;
        if (at >= 0)
            continue; /* heard as it is */
        int c = NCH - 1;
        for (; c >= 0; c--) {
            bool used = false;
            for (int k = 0; k < 6; k++)
                used |= keep[k] == c;
            for (int k = 0; k < nt; k++)
                used |= tap[k] == c;
            if (!used)
                break;
        }
        if (c < 0) /* none to spare: the job comes first, so one of the site's own (it hears nothing meanwhile) */
            for (c = NCH - 1; c >= 0; c--) {
                bool used = false;
                for (int k = 0; k < nt; k++)
                    used |= tap[k] == c;
                if (!used)
                    break;
            }
        if (c < 0) {
            snprintf(why, cap, "%d taps: the receiver has %d channels", nf, NCH);
            return false;
        }
        tap[nt++] = c;
    }
    if (center != c0 && !move_lo(center, why, cap))
        return false;
    TU.moved = center != c0;
    if (isfinite(want_gain)) {
        if (dev->set_rx_gain(dev, want_gain)) {
            snprintf(why, cap, "the receiver's gain: %s", dev->error);
            return false;
        }
        TU.gained = true, TU.gain_db = want_gain;
    }
    pthread_mutex_lock(&TU.mu);
    int k = 0;
    for (int i = 0; i < nf; i++) {
        if (center == c0 && channel_of(f[i]) >= 0)
            continue;
        const int c = tap[k];
        TU.saved[k] = CH[c];
        TU.tap[k] = c;
        CH[c].freq_hz = f[i];
        CH[c].cc = -1;
        CH[c].id = TAP_ID[k];
        k++;
    }
    TU.ntap = k;
    pthread_mutex_unlock(&TU.mu);
    for (int i = 0; i < TU.ntap; i++)
        chan_point(TU.tap[i]);
    TU.on = true;
    char line[300];
    int ln = snprintf(line, sizeof line, "%d tap%s", TU.ntap, TU.ntap == 1 ? "" : "s");
    for (int i = 0; i < TU.ntap && ln > 0 && (size_t)ln < sizeof line; i++)
        ln += snprintf(line + ln, sizeof line - (size_t)ln, "%s %.4f MHz on channel %d", i ? "," : ":",
                       CH[TU.tap[i]].freq_hz / 1e6, TU.tap[i]);
    if (TU.moved)
        say("bench: the receiver tuned for a job, its LO moved to %.4f MHz (the site's channels hear nothing "
            "meanwhile): %s\n", center / 1e6, line);
    else
        say("bench: the receiver tuned for a job: %s\n", line);
    return true;
}

/* the receiver's thread, between chunks */
static void bench_tune_poll(void)
{
    if (!TU.want) /* a racy peek: want is set under mu */
        return;
    if (R.cal) /* the duplex calibration has the LO: after it */
        return;
    pthread_mutex_lock(&TU.mu);
    const int want = TU.want;
    const rdof_bl_tune_t t = TU.t;
    TU.want = 0;
    pthread_mutex_unlock(&TU.mu);
    char why[160] = "";
    bool ok = true;
    if (want == 1)
        ok = tune_apply(&t, why, sizeof why);
    else if (TU.on)
        tune_restore();
    pthread_mutex_lock(&TU.mu);
    if (!TU.want) {
        TU.state = ok ? 1 : -1;
        snprintf(TU.why, sizeof TU.why, "%s", why);
    }
    pthread_mutex_unlock(&TU.mu);
}

static const char *bl_tune(void *ctx, const rdof_bl_tune_t *t, const char *who, char *why, size_t cap)
{
    (void)ctx;
    if (t->ntaps > RDOF_BL_TAPS || t->ntaps > NCH) {
        snprintf(why, cap, "%d taps at most here", NCH < RDOF_BL_TAPS ? NCH : RDOF_BL_TAPS);
        return "bad_request";
    }
    if (isfinite(t->gain_db) && t->gain_db != RDOF_BL_TUNE_AUTO && (!dev || !dev->set_rx_gain)) {
        snprintf(why, cap, "this device's receive gain can't be set");
        return "refused";
    }
    pthread_mutex_lock(&TU.mu);
    TU.t = *t;
    TU.want = 1;
    TU.state = 0;
    pthread_mutex_unlock(&TU.mu);
    say("bench: a tune for %s\n", who);
    return NULL;
}

static int bl_tune_state(void *ctx, char *why, size_t cap)
{
    (void)ctx;
    pthread_mutex_lock(&TU.mu);
    const int st = TU.want ? 0 : TU.state;
    snprintf(why, cap, "%s", TU.why);
    pthread_mutex_unlock(&TU.mu);
    return st;
}

static void bl_untune(void *ctx)
{
    (void)ctx;
    pthread_mutex_lock(&TU.mu);
    TU.want = 2;
    TU.state = 0;
    pthread_mutex_unlock(&TU.mu);
}

/* While the LO is moved, the channels that aren't taps hear somewhere else: dropped. */
static bool tune_drop(int c)
{
    return TU.on && TU.moved && c >= 0 && tap_of(c) < 0;
}

/* A tap's burst (not our own heard back) goes to the bench, the modem and the responder, never the network. */
static bool tap_heard(int c, const rdof_sdr_burst_t *b)
{
    if (!TU.on || c < 0 || tap_of(c) < 0)
        return false;
    if (!R.mmdvm)
        rdof_sdrmodem_heard(&R.m, c, b);
    ms_heard(c, b, false);
    bench_burst(c, b);
    return true;
}

static bool muted(void *ctx, int64_t p) { return rdof_sdrmodem_muted_at(&R.m, p); }
/* Our own transmission heard back (half duplex): on the channel it went out on, or its image, within its span on
 * the air (60 ms either side: a burst's length and the loop's delay); on any other channel, a burst we sent there
 * (the transmitter's spurs: on the Pluto, 100 kHz either side). A full duplex repeater's outbound is on the air
 * all the time it's keyed, so there only a burst we sent counts, on any channel but its input: on the Pluto a
 * repeater sending on 438.3625 was heard on 438.2625 as a call of its own. Not passed on: a channel's peer would
 * send the network's call back to it. The bench's line protocol still gets it: it watches the air. */
static bool echo_drop_(int c, const rdof_sdr_burst_t *b)
{
    if (c < 0)
        return false;
    if (R.full) {
        if (c == channel_of(R.rx_hz))
            return false;
        const int d = rdof_sdrmodem_sent_bits(&R.m, b->position, 5760, b->b.burst);
        if (d < 0 || d > RDOF_SDRMODEM_SENT_BITS)
            return false;
        bench_burst(c, b);
        return true;
    }
    if (b->position <= R.echo_from - 2880 || b->position >= R.echo_to + 2880)
        return false;
    if (c != R.echo_ch && c != R.echo_img) {
        const int d = rdof_sdrmodem_sent_bits(&R.m, b->position, 5760, b->b.burst);
        if (d < 0 || d > RDOF_SDRMODEM_SENT_BITS) {
            if (b->b.dt != RDOF_DT_IDLE)
                say("%s: heard while we sent, not ours (%d bits from the nearest sent)\n", R.rx.trackers[c].channel, d);
            return false;
        }
    }
    bench_burst(c, b);
    return true;
}
static bool echo_drop(void *ctx, int c, const rdof_sdr_burst_t *b)
{
    if (tune_drop(c))
        return true;
    const bool ours = echo_drop_(c, b);
    if (ours)
        ms_heard(c, b, true); /* the responder's answer on the air: where it was heard */
    return ours || tap_heard(c, b);
}
/* the inbound slot by timing: the repeater's RX channel only (another channel keeps its own CACH), the job's with a
 * bench repeater job */
static int relabel(void *ctx, int c, const rdof_sdr_burst_t *b)
{
    const int rx = channel_of(BR.active ? BR.cfg.rx_hz : R.rx_hz);
    return c == rx && !R.mmdvm ? rdof_sdrmodem_inbound(&R.m, b) : 0;
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

/* --fpga-chz: no wideband I/Q, so the page's spectrum is the channels' own: each channel's 48 kS/s through an FFT of
 * its own, and the row is the bins within 12 kHz of each (CS_KEEP of them at 750 Hz: the channel, what's off its
 * center and the channel filter's skirt past 7.5 kHz), a piece of the row at the channel's frequency
 * (rdof_hostweb_spectrum_pieces); the page cuts out the wide spans between them. Rows about every 200 ms of
 * instants. */
#define CS_N 64 /* 750 Hz bins */
#define CS_KEEP 33 /* -12000 .. +12000 Hz */
static void web_spectrum_chs(const rdof_iq16_t *y, int ny, int C)
{
    static rdof_spectrum_t sp[RDOF_CHZFX_MAX_CH];
    static int made; /* 1 made, -1 no memory */
    static rdof_cf_t *buf;
    static int cap, got;
    static uint8_t rows[RDOF_CHZFX_MAX_CH][CS_N];
    if (made < 0 || ny <= 0 || C <= 0 || R.cal || !rdof_hostweb_spectrum_wanted())
        return;
    if (!made) {
        for (int c = 0; c < RDOF_CHZFX_MAX_CH && made >= 0; c++)
            if (rdof_spectrum_init(&sp[c], CS_N, 1 << 30)) /* rows by the instants taken (below) */
                made = -1;
        if (made < 0) {
            say("web: no memory for the spectrum\n");
            return;
        }
        made = 1;
    }
    if (ny > cap) {
        rdof_cf_t *b = realloc(buf, sizeof *b * (size_t)ny);
        if (!b)
            return;
        buf = b, cap = ny;
    }
    const bool done = (got += ny) >= 9600;
    for (int c = 0; c < C && c < RDOF_CHZFX_MAX_CH; c++) {
        for (int i = 0; i < ny; i++) {
            const rdof_iq16_t *v = &y[(size_t)i * C + c];
            buf[i].re = v->i * RDOF_CHZFX_OUT_SCALE, buf[i].im = v->q * RDOF_CHZFX_OUT_SCALE;
        }
        sp[c].per_row = done ? sp[c].count + 1 : 1 << 30;
        rdof_spectrum_feed(&sp[c], buf, ny, rows[c]);
    }
    if (!done)
        return;
    const int row_ms = (int)lrint(1e3 * got / 48000.0);
    got = 0;
    enum { NP = RDOF_HOSTWEB_SPECTRUM_PIECES < RDOF_CHZFX_MAX_CH ? RDOF_HOSTWEB_SPECTRUM_PIECES : RDOF_CHZFX_MAX_CH };
    _Static_assert(NP * CS_KEEP <= RDOF_HOSTWEB_SPECTRUM_MAX, "the pieces fit a row");
    const int nc = C < NP ? C : NP, bw = 48000 / CS_N;
    uint8_t row[NP * CS_KEEP];
    double f0[NP];
    int pn[NP];
    for (int c = 0; c < nc; c++) {
        memcpy(row + c * CS_KEEP, rows[c] + CS_N / 2 - CS_KEEP / 2, CS_KEEP);
        f0[c] = CH[c].freq_hz - (CS_KEEP / 2 + 0.5) * bw, pn[c] = CS_KEEP;
    }
    rdof_hostweb_spectrum_pieces(0, row, f0, pn, nc, bw, RDOF_CHZ_CUTOFF_HZ, R.rx.center_hz, R.rate, row_ms);
}

/* The SDR's card: the device, its samples and faults, the loop's time, the transmitter, and every receiver channel */
static void web_sdr(rdof_hostweb_sdr_t *d)
{
    const double now = now_s();
    snprintf(d->name, sizeof d->name, "%s", rcfg.sdr_name[0] ? rcfg.sdr_name : "sdr");
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
        for (int i = 0; i < (R.split || R.mmdvm ? 2 : 3); i++) {
            snprintf(d->stage[d->nstage].name, sizeof d->stage[0].name, "%s", stages[i]);
            d->stage[d->nstage++].share = (float)WS.share[i];
        }
    }
    if (!R.split && !R.mmdvm) { /* the split's radio or the modem transmits: the SDR receives only */
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
    for (int i = 0; i < nchn; i++)
        if (chan_has_peer(&CHN[i])) {
            d->hblink_peers++;
            d->hblink_up += chan_peer_up(&CHN[i]);
        }
    for (int c = 0; c < R.rx.C && d->nchan < 16; c++) {
        const char *id = R.rx.trackers[c].channel, *name = "";
        const chan_t *m = chan_at(c);
        if (m)
            name = m->ch->name;
        else /* ours: --monitor's TX heard back, --mmdvm's modem's RX */
            name = !strcmp(id, "out")        ? "transmit (heard back)"
                   : !strcmp(id, "modem-rx") ? "the modem's RX"
                   : !strcmp(id, "modem-tx") ? "the modem's TX"
                                             : "receive";
        const double f = R.rx.trackers[c].freq_hz;
        const int r = R.rx.rssi[c];
        snprintf(d->chan[d->nchan].id, sizeof d->chan[0].id, "%s", id);
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

/* --mmdvm: the modem on the port */
static void web_mmdvm(rdof_hostweb_modem_t *m)
{
    snprintf(m->name, sizeof m->name, "%s", R.full ? "repeater" : "transceiver");
    snprintf(m->kind, sizeof m->kind, "%s", rdof_radio_kind_name(R.radio.kind));
    snprintf(m->role, sizeof m->role, "%s", R.full ? "receives and transmits (full duplex)" : "half duplex");
    snprintf(m->port, sizeof m->port, "%s", P.path ? P.path : "");
    snprintf(m->version, sizeof m->version, "%s", R.client.version);
    snprintf(m->state, sizeof m->state, "%s", R.keyed ? "keyed" : "idle");
    if (P.gone)
        snprintf(m->error, sizeof m->error, "the port went away");
    else if (R.client.failed[0])
        snprintf(m->error, sizeof m->error, "%s", R.client.failed);
    m->open = !P.gone;
    m->ready = !P.gone && !R.client.failed[0];
    const struct {
        const char *name;
        int64_t v;
    } x[] = {{"bursts heard", mm_heard_n}, {"requests", R.client.requests}, {"unanswered", R.client.no_reply},
             {"refused", R.client.refused},  {"RF level cap", mm_rf_level},  {"port bytes dropped", P.dropped}};
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
    const bool keyed = R.split || R.mmdvm ? R.keyed : R.m.keyed;
    if (R.mmdvm) {
        snprintf(w.radio, sizeof w.radio, "%s on %s%s%s", rdof_radio_kind_name(R.radio.kind), P.path ? P.path : "",
                 dev ? "; RX " : "", dev ? dev->name : "");
        snprintf(w.modem, sizeof w.modem, "%s", R.client.version);
    } else if (R.split) {
        snprintf(w.radio, sizeof w.radio, "TX %s; RX %s", S.halves[RDOF_SPLIT_TX].description, dev->name);
        snprintf(w.modem, sizeof w.modem, "split duplex: the radio and the SDR modem");
    } else {
        snprintf(w.radio, sizeof w.radio, "%s", dev->name);
        snprintf(w.modem, sizeof w.modem, "SDR modem");
    }
    snprintf(w.state, sizeof w.state, "%s", keyed ? "keyed" : "idle");
    w.ready = R.split ? S.ready && !S.failed[0] : R.mmdvm ? !P.gone && !R.client.failed[0] : true;
    w.keyed = keyed;
    w.cc = cc;
    w.rx_hz = (uint32_t)rx, w.tx_hz = (uint32_t)tx;
    w.cpu_load = -1;
    w.bursts_heard = R.m.rx_bursts, w.bursts_sent = R.m.tx_bursts, w.dropped = R.m.lost;
    if (R.mmdvm)
        w.bursts_heard = (uint64_t)mm_heard_n, w.bursts_sent = 0, w.dropped = 0;
    const int uc = unit_channel(); /* what the unit hears: the channel it's on, or the modem */
    const rdof_tracker_t *tr = R.mmdvm ? &MT : uc >= 0 ? &R.rx.trackers[uc] : NULL;
    for (int i = 0; tr && i < 2; i++) {
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
    if (net_on)
        rdof_hb_peer_login_status(&NET.peer, w.brandmeister, sizeof w.brandmeister);
    for (int i = 0; i < nchn && w.nchan < 16; i++) {
        const chan_t *m = &CHN[i];
        const int r = R.rx.rssi[m->c];
        snprintf(w.chan[w.nchan].id, sizeof w.chan[w.nchan].id, "%s", m->ch->id);
        w.chan[w.nchan].freq = (uint32_t)llround(m->ch->freq);
        w.chan[w.nchan].bursts = R.rx.bursts[m->c];
        w.chan[w.nchan].rssi = r == INT32_MIN ? 0 : r;
        w.chan[w.nchan].peer = chan_has_peer(m);
        w.chan[w.nchan++].up = chan_peer_up(m);
    }
    w.nextra = R.split || R.mmdvm ? 0 : 2; /* the TX half's timing is the radio's own, and the modem's */
    snprintf(w.extra[0].name, sizeof w.extra[0].name, "tx late (us)");
    w.extra[0].value = R.m.tx_late48 * 1000 / 48;
    snprintf(w.extra[1].name, sizeof w.extra[1].name, "inbound off (%%)");
    w.extra[1].value = (int64_t)llround(R.m.inbound_off * 100);
    w.spectrum = dev != NULL; /* --fpga-chz: the channels' own (web_spectrum_chs) */
    if (dev)
        web_sdr(&w.sdrs[w.nsdr++]);
    if (R.split)
        web_split_radio(&w.modems[w.nmodem++]);
    if (R.mmdvm)
        web_mmdvm(&w.modems[w.nmodem++]);
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
    if (ok && c->nchan) { /* the receive channels in one window, as the start will ask */
        rdof_sdr_channel_t ch[RDOF_SDR_MAX_CHANNELS];
        int n = 0;
        for (int i = 0; i < c->nchan && n < RDOF_SDR_MAX_CHANNELS; i++)
            ch[n++] = (rdof_sdr_channel_t){c->chan[i].id, c->chan[i].freq, c->chan[i].cc, c->chan[i].slot};
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

/* --ota FILE: the page's update (POST /api/ota) writes a new program there, gzipped as the Pluto bundle's
 * rdofradio.gz or an ELF for this machine (the class, byte order and machine of the one running), and this program
 * stops: its supervisor (the bundle's run.sh) puts the image in place and starts it. */
static bool ota_check(void *ctx, const uint8_t *img, size_t len, char *err, size_t cap)
{
    if (len >= 18 && img[0] == 0x1f && img[1] == 0x8b && img[2] == 8)
        return true; /* gzip, deflate: run.sh tests it whole before it takes it */
    if (len < 64 || memcmp(img, "\x7f" "ELF", 4)) {
        snprintf(err, cap, "not a program: a gzipped rdofradio (the bundle's rdofradio.gz) or an ELF");
        return false;
    }
    uint8_t me[20];
    FILE *f = fopen("/proc/self/exe", "rb");
    const bool mine = f && fread(me, 1, sizeof me, f) == sizeof me && !memcmp(me, "\x7f" "ELF", 4);
    if (f)
        fclose(f);
    if (mine && (img[4] != me[4] || img[5] != me[5] || img[18] != me[18] || img[19] != me[19])) {
        snprintf(err, cap, "an ELF for another machine (class %u, data %u, machine %u; this one runs %u, %u, %u)",
                 img[4], img[5], img[18] | img[19] << 8, me[4], me[5], me[18] | me[19] << 8);
        return false;
    }
    const unsigned type = img[5] == 2 ? (unsigned)(img[16] << 8 | img[17]) : (unsigned)(img[16] | img[17] << 8);
    if (type != 2 && type != 3) { /* a program, or a PIE (shared objects too: run.sh tries it before it takes it) */
        snprintf(err, cap, "an ELF that isn't a program (type %u)", type);
        return false;
    }
    return true;
}
static void ota_done(void *ctx)
{
    stop = 1; /* not a restart: the supervisor starts the new program */
}
static void web_ota(const char *path)
{
    rdof_hostweb_ota_t o = {.max = 16u << 20, .check = ota_check, .done = ota_done};
    snprintf(o.path, sizeof o.path, "%s", path);
    rdof_hostweb_ota(&o);
}
#endif

/* ── --split-tx: the radio transmits, the SDR is the split's RX half ─────────── */


static void *port_reader(void *arg)
{
    port_t *const p = arg ? arg : &P;
    while (!p->quit) {
        struct pollfd pf = {p->fd, POLLIN, 0};
        if (poll(&pf, 1, 100) <= 0)
            continue;
        uint8_t b[256];
        const int n = rdofradio_serial_read(p->fd, b, sizeof b);
        const double t = now_s();
        if (n < 0 || (pf.revents & (POLLHUP | POLLERR | POLLNVAL))) {
            p->gone = true;
            break;
        }
        if (n == 0)
            continue;
        pthread_mutex_lock(&p->mu);
        if (p->head - p->tail < PORT_Q) {
            const int i = p->head++ % PORT_Q;
            p->q[i].t = t, p->q[i].n = n;
            memcpy(p->q[i].b, b, (size_t)n);
        } else {
            p->dropped += (uint32_t)n;
        }
        pthread_mutex_unlock(&p->mu);
    }
    return NULL;
}

/* What the reader has, oldest first: its bytes and the time they came; 0 none. */
static int port_take(port_t *p, uint8_t b[256], double *t)
{
    pthread_mutex_lock(&p->mu);
    if (p->tail == p->head) {
        pthread_mutex_unlock(&p->mu);
        return 0;
    }
    const int i = p->tail++ % PORT_Q, n = p->q[i].n;
    *t = p->q[i].t;
    memcpy(b, p->q[i].b, (size_t)n);
    pthread_mutex_unlock(&p->mu);
    return n;
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

/* ── --fpga-chz: the block's frames through the receiver (FX, above plan_rate) ─────────── */

static uint32_t fx_rd(void *ctx, uint32_t off) { return *(volatile uint32_t *)((uint8_t *)ctx + off); }
static void fx_wr(void *ctx, uint32_t off, uint32_t v) { *(volatile uint32_t *)((uint8_t *)ctx + off) = v; }

/* span bytes of registers at addr through /dev/mem, or NULL with why in err */
static void *map_regs(unsigned long addr, size_t span, char *err, size_t cap)
{
    const int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        snprintf(err, cap, "/dev/mem: %s", strerror(errno));
        return NULL;
    }
    void *p = mmap(NULL, span, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)addr);
    close(fd);
    if (p == MAP_FAILED) {
        snprintf(err, cap, "/dev/mem at 0x%08lx: %s", addr, strerror(errno));
        return NULL;
    }
    return p;
}

/* The block's registers through /dev/mem, its ID and CAPS. 0, or -1 with why in err. */
static int fx_probe(char *err, size_t cap)
{
    if (!(FX.regs = map_regs(FX.addr, RDOF_CHZHW_SPAN, err, cap)))
        return -1;
    const rdof_chzhw_io_t io = {fx_rd, fx_wr, FX.regs};
    char e[160];
    if (rdof_chzhw_probe(&FX.h, &io, e, sizeof e)) {
        snprintf(err, cap, "no FPGA channelizer at 0x%08lx: %s", FX.addr, e);
        return -1;
    }
    return 0;
}

/* ENABLE off: the stock path to the DMA again (at exit, whatever the way) */
static void fx_stop(void)
{
    if (FX.started)
        rdof_chzhw_stop(&FX.h);
    FX.started = false;
}

/* off: the stock path to the DAC again (at exit, whatever the way) */
static void dx_stop(void)
{
    if (DX.started)
        rdof_chzhw_duc_stop(&DX.d);
    DX.started = false;
}

/* The interpolator on for the device's rate, the transmit stream at 192 kS/s, else 96 kS/s: R.txr, R.tx_rate,
 * R.lo_off. Its CIC leaves the stream's images at multiples of that from the carrier, the first 94 dB down at
 * 192 kS/s and 76 dB at 96; at 48 kS/s, 56 dB, beside the next channels of a 50 kHz grid the board is receiving:
 * not taken. 0, or -1 with why in err. */
static int dx_probe(char *err, size_t cap)
{
    if (!(DX.regs = map_regs(DX.addr, RDOF_CHZHW_DUC_SPAN, err, cap)))
        return -1;
    const rdof_chzhw_io_t io = {fx_rd, fx_wr, DX.regs};
    char e[160];
    if (rdof_chzhw_duc_probe(&DX.d, &io, e, sizeof e)) {
        snprintf(err, cap, "no TX interpolator at 0x%08lx: %s", DX.addr, e);
        return -1;
    }
    DX.probed = true;
    return 0;
}
/* The interpolation for the device's rate (from 192 kS/s, else 96), or 0: none */
static long dx_r(double rate)
{
    const long fs = lround(rate), lo = rdof_chzhw_duc_min_r(&DX.d), hi = rdof_chzhw_duc_max_r(&DX.d);
    for (long in = 192000; in >= 96000; in /= 2)
        if (fs % in == 0 && fs / in >= lo && fs / in <= hi)
            return fs / in;
    return 0;
}
static int dx_start(double rate, char *err, size_t cap)
{
    const long lo = rdof_chzhw_duc_min_r(&DX.d), hi = rdof_chzhw_duc_max_r(&DX.d), r = dx_r(rate);
    char e[160];
    if (!r) {
        snprintf(err, cap, "the TX interpolator can't make %.0f S/s from 192 or 96 kS/s (R %ld to %ld): a rate that's "
                           "a multiple of 96 kS/s", rate, lo, hi);
        return -1;
    }
    if (rdof_chzhw_duc_start(&DX.d, (int)r, e, sizeof e)) {
        snprintf(err, cap, "%s", e);
        return -1;
    }
    DX.started = true;
    static bool at_exit;
    if (!at_exit) /* once: a raw capture off the rate starts it again */
        at_exit = !atexit(dx_stop);
    R.txr = (int)r;
    R.tx_rate = rate / (double)r;
    /* as far from the carrier as io_tune's reach (0.4 tx_rate less 12.5 kHz) allows, a kHz inside, in steps of a
     * half channel: 62.5 kHz at 192 kS/s, 18.75 at 96 */
    R.lo_off = floor((0.4 * R.tx_rate - 13.5e3) / 6250) * 6250;
    return 0;
}

/* One output instant to FX.y (NULL: zeros), with its power for the loop's edge detection */
static void fx_put(const rdof_iq16_t *x)
{
    if (FX.ny == FX.cap) {
        FX.skipped++;
        return;
    }
    rdof_iq16_t *o = FX.y + (size_t)FX.ny * FX.C;
    float p = 0.0f;
    if (x) {
        memcpy(o, x, sizeof *o * (size_t)FX.C);
        for (int c = 0; c < (FX.cal ? 1 : FX.C); c++) {
            const float q = (float)x[c].i * x[c].i + (float)x[c].q * x[c].q;
            p = q > p ? q : p;
        }
    } else {
        memset(o, 0, sizeof *o * (size_t)FX.C);
    }
    FX.fl[FX.ny] = !x;
    FX.pw[FX.ny++] = p * RDOF_CHZFX_OUT_SCALE * RDOF_CHZFX_OUT_SCALE;
    FX.nall++;
}

/* Channel c's NCO goes to offset hz from instant `from` on (the bench's channels' record of it; a full queue drops
 * the oldest step, which only a reader of those instants would miss) */
static void fx_nco(int c, double hz, long long from)
{
    struct fx_nco *n = &FX.nco[c];
    if (n->n == 4) {
        n->hz = n->q[0].hz;
        memmove(n->q, n->q + 1, sizeof n->q[0] * 3);
        n->n--;
    }
    n->q[n->n].from = from, n->q[n->n].hz = hz, n->n++;
}

/* A raw capture (bench "capture" with "raw"): a thread stops the block and the stream, takes the ADC's samples in one DMA
 * block and starts them again, while the loop keeps turning on silence for that long, so the transmitter, the clocks and
 * every count keep real time. The silence is filler (and counted lost): those bursts really weren't heard. */
static struct {
    _Atomic int state;  /* 0 idle, 1 being made, 2 ready, -1 failed */
    atomic_bool fill, ack, closing;
    long n, clipped;
    int16_t *buf;
    double t0, t_fill, fs, center; /* its rate; where it's tuned, NAN: where the receiver is */
    long long made;
    char err[160];
} RAW;
static bool fx_resync; /* the block started again: its next frame carries on from the count so far */
static long long fx_raw_made; /* channel frames of silence made while a raw capture had the receiver */
static long long fx_owe;      /* and the frames its silence came short of (the loop's lag said so after): made up */

/* A channel frame from the parser: the instants lost before it made up with zeros (up to FX_GAP), so the receiver's
 * sample count stays the block's (seq - seq0) */
static void fx_on_ch(void *ctx, uint32_t seq, const rdof_iq16_t *x, int n)
{
    if (!FX.have0) { /* STATUS's flags from the start (the DMA without a buffer until it had one): the baseline, as below */
        FX.have0 = true, FX.seq0 = FX.expect = seq;
        FX.status = rdof_chzhw_status(&FX.h);
    }
    if (fx_resync) {
        fx_resync = false;
        FX.expect = seq;
        FX.seq0 = seq - (uint32_t)(FX.nall + FX.skipped); /* as if it had never stopped */
        /* STATUS's flags from the restart itself (the FIFO full while off, the DMA without a buffer until it had one) are
         * no overflow: the first frame is the baseline. A frame lost after it still shows, as a gap in the numbers. */
        FX.status = rdof_chzhw_status(&FX.h);
    }
    const uint32_t gap = seq - FX.expect;
    if (gap >= 0x80000000u)
        return; /* behind the last: not this stream's */
    FX.lost += gap;
    for (uint32_t k = 0; k < gap; k++) {
        if (k < FX_GAP) {
            fx_put(NULL);
            FX.made_up++;
        } else {
            FX.skipped++;
        }
    }
    fx_put(x);
    FX.expect = seq + 1;
}

/* The receiver's AFC moved channel c: its NCO's next step (to the block after the turn, all at once). The channel
 * sample the step takes effect from: FRAMES now, as the receiver counts. */
static int64_t fx_correction(void *ctx, int c, double hz)
{
    if (c < 0 || c >= FX.C)
        return -1;
    const int64_t from = (int64_t)(uint32_t)(rdof_chzhw_frames(&FX.h) - FX.seq0) - FX.skipped + FX_LATE;
    if (!(FX.cal && c == 0)) { /* channel 0 is on the pulse: its step comes back at the calibration's end */
        FX.inc[c] = rdof_chzfx_inc(FX.off[c] + hz, R.rate);
        FX.dirty = true;
        fx_nco(c, FX.off[c] + hz, from);
    }
    return from;
}

/* The duplex calibration (full duplex: the RX LO on the TX LO, the pulse lo_off below it): channel 0 hears the
 * pulse while on, its own frequency again after */
static void fx_cal(bool on)
{
    if (!FX.on)
        return;
    FX.cal = on;
    FX.inc[0] = rdof_chzfx_inc(on ? -R.lo_off : FX.off[0] + R.rx.chz.correction_hz[0], R.rate);
    fx_nco(0, on ? -R.lo_off : FX.off[0] + R.rx.chz.correction_hz[0],
           (int64_t)(uint32_t)(rdof_chzhw_frames(&FX.h) - FX.seq0) - FX.skipped + FX_LATE);
    rdof_chzhw_set_inc(&FX.h, FX.inc);
    FX.dirty = false;
}

/* The block started with the receiver's channels and their corrections. 0, or -1 with why in err. */
static int fx_start(char *err, size_t cap)
{
    FX.C = R.rx.C;
    FX.step = (int)lround(R.rate / 48000);
    if (!fx_plan(FX.step, &FX.R, &FX.D)) {
        snprintf(err, cap, "the FPGA channelizer can't decimate %.0f S/s to 48 kS/s", R.rate);
        return -1;
    }
    double off[RDOF_CHZFX_MAX_CH];
    for (int c = 0; c < FX.C; c++) {
        FX.off[c] = CH[c].freq_hz - R.rx.center_hz;
        off[c] = FX.off[c] + R.rx.chz.correction_hz[c];
        FX.nco[c].hz = off[c], FX.nco[c].n = 0;
    }
    if (rdof_chzfx_design_cic(&FX.cfg, R.rate, FX.R, FX.D, off, FX.C, RDOF_CHZ_CUTOFF_HZ)) {
        snprintf(err, cap, "no FPGA channelizer configuration for %d channels, R %d, D %d at %.0f S/s", FX.C, FX.R,
                 FX.D, R.rate);
        return -1;
    }
    memcpy(FX.inc, FX.cfg.inc, sizeof FX.inc);
    FX.cap = FX_GAP + 8192; /* a second of lost instants and a read's (about 480) */
    FX.y = malloc(sizeof *FX.y * (size_t)FX.cap * (size_t)FX.C);
    FX.pw = malloc(sizeof *FX.pw * (size_t)FX.cap);
    FX.fl = malloc(sizeof *FX.fl * (size_t)FX.cap);
    if (!FX.y || !FX.pw || !FX.fl) {
        snprintf(err, cap, "out of memory");
        return -1;
    }
    rdof_chzhw_rx_init(&FX.prx, FX.C, fx_on_ch, NULL, NULL);
    if (rdof_chzhw_start(&FX.h, &FX.cfg, 0, 0, err, cap))
        return -1;
    FX.started = true;
    atexit(fx_stop);
    R.rx.on_correction = fx_correction;
    return 0;
}

/* The last read's instants through the receiver (the helper takes a share of the demods, as rx_chunk), then the
 * AFC's new steps to the block */
static void fx_rx(void)
{
    if (!HX.on && !HX.failed && R.rx.C > 1) {
        HX.on = pthread_create(&HX.th, NULL, chz_helper, NULL) == 0;
        HX.failed = !HX.on;
    }
    const int split = R.rx.C / 3 > 0 ? R.rx.C / 3 : 1;
#ifdef RDOF_HOST_WEB
    web_spectrum_chs(FX.y, FX.ny, FX.C);
#endif
    for (int k = 0; k < FX.ny; k += FX_MAX) {
        const int n = FX.ny - k < FX_MAX ? FX.ny - k : FX_MAX;
        rdof_sdr_rx_channels_in(&R.rx, FX.y + (size_t)k * FX.C, n, RDOF_CHZFX_OUT_SCALE);
        if (HX.on) {
            hx_run(3, split, R.rx.C);
            rdof_sdr_rx_feed(&R.rx, 0, split);
            hx_join();
        } else {
            rdof_sdr_rx_feed(&R.rx, 0, R.rx.C);
        }
        rdof_sdr_rx_bursts(&R.rx, 0, R.rx.C);
        rdof_sdr_rx_finish(&R.rx);
    }
    if (FX.dirty && !atomic_load(&RAW.fill)) { /* the raw capture's thread has the block */
        rdof_chzhw_set_inc(&FX.h, FX.inc);
        FX.dirty = false;
    }
}

/* The loop's turn in place of a device read: up to chunk wideband samples' worth of output instants, a whole number
 * of them; a DMA buffer is read (and goes through the receiver) when the last one's are all taken. Returns the
 * wideband samples, or RDOF_DEV_* (TIMEOUT for a buffer with no frames). */
static int fx_take(int chunk, long timeout_us)
{
    /* a raw capture waits for a calibration under way: it has the receiver on the TX LO */
    if (FX.j >= FX.ny && atomic_load(&RAW.fill) && R.cal == 0) { /* no stream: silence, at the pace it would have come */
        const double rate = R.rate / FX.step;
        double t = now_s();
        if (!atomic_exchange(&RAW.ack, true))
            RAW.t_fill = t, RAW.made = 0;
        long long k = (long long)((t - RAW.t_fill) * rate) - RAW.made;
        if (k < (long long)(0.005 * rate)) {
            nanosleep(&(struct timespec){0, 5000000}, NULL);
            k = (long long)((now_s() - RAW.t_fill) * rate) - RAW.made;
        }
        k = k < 1 ? 1 : k > FX.cap ? FX.cap : k;
        RAW.made += k;
        FX.ny = FX.j = 0;
        for (long long i = 0; i < k; i++)
            fx_put(NULL);
        FX.made_up += k, FX.lost += k;
        fx_raw_made += k;
        WS.lost += k * FX.step;
        FX.base = FX.nall - FX.ny;
        fx_rx();
    } else if (FX.j >= FX.ny && fx_owe > 0) { /* a raw capture's silence, short: the rest, at once */
        long long k = fx_owe > FX.cap ? FX.cap : fx_owe;
        fx_owe -= k;
        FX.ny = FX.j = 0;
        for (long long i = 0; i < k; i++)
            fx_put(NULL);
        FX.made_up += k;
        FX.base = FX.nall - FX.ny;
        fx_rx();
    } else if (FX.j >= FX.ny) {
        const uint64_t *w;
        const int nw = dev->read_words(dev, &w, timeout_us);
        if (nw < 0)
            return nw;
        FX.ny = FX.j = 0;
        const long long lost = FX.lost;
        rdof_chzhw_rx_feed(&FX.prx, w, (size_t)nw);
        FX.base = FX.nall - FX.ny;
        WS.lost += (FX.lost - lost) * FX.step;
        const uint32_t st = rdof_chzhw_status(&FX.h), now = st & ~FX.status;
        const long ovf = !!(now & RDOF_CHZHW_STATUS_IN_OVF) + !!(now & RDOF_CHZHW_STATUS_DMA_OVF);
        FX.ovf += ovf;
        WS.overflows += ovf;
        FX.status = st;
        const double t = now_s();
        fx_rx();
        FX.t_rx = now_s() - t;
        if (!FX.ny)
            return RDOF_DEV_TIMEOUT;
    }
    int k = chunk / FX.step;
    k = k < 1 ? 1 : k > FX.ny - FX.j ? FX.ny - FX.j : k;
    FX.turn = FX.j;
    FX.j += k;
    return k * FX.step;
}

static void *raw_thread(void *arg)
{
    (void)arg;
    char err[160] = "";
    int16_t *const buf = RAW.buf;
    const long n = RAW.n;
    const bool off_rate = RAW.fs != R.rate, retune = isfinite(RAW.center);
    double tl_dry = NAN;
    int64_t tl_pushed = 0, tl_skip = 0;
    atomic_store(&RAW.ack, false);
    atomic_store(&RAW.fill, true);
    while (!atomic_load(&RAW.ack))
        nanosleep(&(struct timespec){0, 1000000}, NULL);
    /* the TX held before the receiver stops: its timeline is taken while the stream still runs */
    if (off_rate) { /* the TX at that rate too: nothing goes out meanwhile */
        pthread_mutex_lock(&W.mu);
        W.hold = true;
        pthread_cond_broadcast(&W.cv);
        while (W.writing && W.running) /* a push under way finishes on the buffer it began on */
            pthread_cond_wait(&W.cv, &W.mu);
        if (W.guarded) /* where the stream stands on the air: its queue's end, and the sample that ends it */
            tl_dry = W.guard.dry_at, tl_pushed = W.pushed, tl_skip = W.guard.skip;
        pthread_mutex_unlock(&W.mu);
    }
    dev->rx_pause(dev);
    rdof_chzhw_stop(&FX.h); /* the stock path: the ADC's samples to the DMA */
    double fs = R.rate;
    int rc = 0;
    if ((off_rate || retune) && dev->raw_tune(dev, off_rate ? RAW.fs : 0, retune ? RAW.center : 0, &fs, err, sizeof err))
        rc = -1;
    RAW.fs = fs;
    long after = 0;
    if (!rc)
        rc = dev->raw_fill(dev, n, fs, buf, &after, err, sizeof err);
    const double t_end = bl_utc(NULL); /* the last DMA block's last sample just came */
    if (!rc)
        RAW.t0 = t_end - (double)(n + after) / fs;
    if ((off_rate || retune) && dev->raw_untune(dev)) {
        say("raw capture: the chip's rate and tuning didn't come back: %s\n", dev->error);
        exit(1); /* run.sh starts it again */
    }
    if (off_rate) { /* the rate changes reset the DAC core's source and stalled its DMA: the interpolator and the
                     * transmit buffer anew, as at start (the writer's held: nothing writes meanwhile) */
        char e1[160] = "";
        if (DX.started) {
            dx_stop();
            if (dx_start(R.rate, e1, sizeof e1)) {
                say("raw capture: the TX interpolator didn't start again: %s\n", e1);
                exit(1);
            }
        }
        if (dev->tx_restart && dev->tx_restart(dev)) {
            say("raw capture: the transmit buffer didn't come back: %s\n", dev->error);
            exit(1);
        }
    }
    char e2[160];
    memcpy(FX.cfg.inc, FX.inc, sizeof FX.inc);
    rdof_chzhw_rx_init(&FX.prx, FX.C, fx_on_ch, NULL, NULL);
    if (rdof_chzhw_start(&FX.h, &FX.cfg, 0, 0, e2, sizeof e2) || dev->rx_resume(dev)) {
        say("raw capture: the receive stream didn't come back: %s\n", dev->error);
        exit(1); /* run.sh starts it again */
    }
    fx_resync = true;
    if (off_rate) {
        pthread_mutex_lock(&W.mu);
        /* the guard anew, as at start: nothing was pushed through the hold, so its queue's time ran out seconds ago
         * and it took every block after for one that came late (zeros sent, the samples dropped, for good) */
        if (W.guarded) { /* its counts go on: the loop's log takes differences of them */
            const rdof_txguard_t was = W.guard;
            rdof_txguard_init(&W.guard, R.tx_rate, dev->tx_block, dev->tx_nbuf);
            W.guard.padded = was.padded, W.guard.pads = was.pads, W.guard.dry = was.dry;
        }
        /* The stream goes on where it would have without the hold: sample s on the air at tl_dry + (s - tl_pushed -
         * tl_skip) / rate (the zeros the guard still owed for count as sent). The new queue first just over full
         * with zeros, so its pushes wait and the guard has its end from them; then the ring's next sample is the one
         * due there: the samples before it dropped, or, when that one went by in the hold (the stream's lead is in
         * the ring, which the hold emptied), the ring back to it, zeros in its place. A fixed lead in its place put
         * the stream 52-58 ms after its place as the DMA happened to start (the calibration moved with each
         * capture); zeros pushed past a full queue only wait. The writer's held, so this thread writes. */
        if (dev->tx_block > 0 && dev->tx_nbuf > 0) {
            static const rdof_cf_t zeros[CHUNK];
            pthread_mutex_unlock(&W.mu);
            for (long left = (long)dev->tx_block * (dev->tx_nbuf + 1) + 1; left > 0;) {
                const int k = dev->write(dev, zeros, left < CHUNK ? (int)left : CHUNK, 100000);
                if (k <= 0)
                    break;
                left -= k;
            }
            pthread_mutex_lock(&W.mu);
            const double txrate = R.tx_rate;
            if (W.guarded && !isnan(tl_dry) && !isnan(W.guard.dry_at)) {
                const int64_t due = tl_pushed + tl_skip + llround((W.guard.dry_at - tl_dry) * txrate);
                const int64_t d = due - W.tail; /* > 0: samples to drop; < 0: zeros to go back over */
                if (d >= 0 && d < (int64_t)(0.25 * txrate)) {
                    W.guard.skip += d;
                    say("transmit: back on its timeline after the raw capture (%.2f ms dropped)\n", 1e3 * (double)d / txrate);
                } else if (d < 0 && -d < (int64_t)(0.25 * txrate) && W.head - due < RING) {
                    for (int64_t q = due; q < W.tail; q++)
                        W.buf[q % RING] = (rdof_cf_t){0, 0};
                    W.tail = due;
                    say("transmit: back on its timeline after the raw capture (%.2f ms of zeros)\n", -1e3 * (double)d / txrate);
                } else {
                    say("transmit: after the raw capture its timeline is %.1f ms off; it goes on from its queue\n",
                        1e3 * (double)d / txrate);
                }
            } else {
                say("transmit: after the raw capture its timeline wasn't known; it goes on from its queue\n");
            }
        }
        W.hold = false;
        pthread_cond_broadcast(&W.cv);
        pthread_mutex_unlock(&W.mu);
    }
    atomic_store(&RAW.fill, false);
    long clip = 0;
    if (!rc)
        for (long i = 0; i < 2 * n; i++)
            clip += buf[i] >= 2047 || buf[i] <= -2048;
    RAW.clipped = clip;
    snprintf(RAW.err, sizeof RAW.err, "%s", err);
    if (atomic_load(&RAW.closing)) { /* its client went: nothing to keep */
        free(buf);
        RAW.buf = NULL;
        atomic_store(&RAW.state, 0);
        return NULL;
    }
    atomic_store(&RAW.state, rc ? -1 : 2);
    return NULL;
}

static int bl_raw_start(void *ctx, double seconds, double fs, double center, char *err, size_t cap)
{
    (void)ctx;
    if (atomic_load(&RAW.state))
        return 1;
    if (!dev || !dev->raw_fill) {
        snprintf(err, cap, "this device can't hand over its ADC's samples");
        return -1;
    }
    if (fs > 0 && fs != R.rate && !dev->raw_tune) {
        snprintf(err, cap, "this device's rate can't change for a capture");
        return -1;
    }
    RAW.fs = fs > 0 ? fs : R.rate, RAW.center = center;
    const long n = lround(seconds * RAW.fs);
    int16_t *buf = malloc((size_t)n * 4);
    if (!buf) {
        snprintf(err, cap, "no memory for %.0f MB of samples", (double)n * 4 / 1e6);
        return -1;
    }
    RAW.buf = buf, RAW.n = n, RAW.err[0] = 0;
    atomic_store(&RAW.closing, false);
    atomic_store(&RAW.state, 1);
    pthread_t th;
    if (pthread_create(&th, NULL, raw_thread, NULL)) {
        free(buf);
        RAW.buf = NULL;
        atomic_store(&RAW.state, 0);
        snprintf(err, cap, "no thread for it");
        return -1;
    }
    pthread_detach(th);
    return 0;
}

static int bl_raw_state(void *ctx, rdof_bl_raw_t *o, char *err, size_t cap)
{
    (void)ctx;
    const int st = atomic_load(&RAW.state);
    if (st < 0) {
        snprintf(err, cap, "%s", RAW.err);
        return -1;
    }
    if (st != 2)
        return 0;
    *o = (rdof_bl_raw_t){.n = RAW.n, .clipped = RAW.clipped, .fs = RAW.fs, .t0 = RAW.t0, .full_scale = 2048, .bits = 12,
                         .center_hz = isfinite(RAW.center) ? RAW.center : R.rx.center_hz};
    return 1;
}

static void bl_raw_read(void *ctx, long at, int16_t *out, long n)
{
    (void)ctx;
    memcpy(out, RAW.buf + 2 * at, (size_t)n * 4);
}

static void bl_raw_close(void *ctx)
{
    (void)ctx;
    int st = atomic_load(&RAW.state);
    if (st == 1) { /* still being made: the thread frees it */
        atomic_store(&RAW.closing, true);
        return;
    }
    free(RAW.buf);
    RAW.buf = NULL;
    if (st)
        atomic_store(&RAW.state, 0);
}

/* A raw bursts stream's decoder: the receiver's own demod on the engine's 48 kS/s outputs, its bursts as the
 * receiver's (bench_burst), the carrier from the channel's center as the demod saw it (the engine's mixer corrected
 * the crystal already) */
static struct {
    rdof_demod_t d;
    bool on;
} RAWD;

static int bl_raw_demod(void *ctx, bool reset, int cc, const float *iq, int n, rdof_bl_burst_t *out, int cap)
{
    (void)ctx;
    enum { FEED = 4800 };
    static const rdof_cf_t zeros[FEED];
    if (reset || !RAWD.on) {
        if (RAWD.on)
            rdof_demod_free(&RAWD.d);
        RAWD.on = false;
        if (rdof_demod_init(&RAWD.d, cc, 1, RDOF_DEMOD_SQUELCH_HZ, FEED)) {
            rdof_demod_free(&RAWD.d);
            return -1;
        }
        RAWD.on = true;
    }
    const bool flush = n == 0;
    int left = flush ? 3 * FEED : n, got = 0; /* the end: 0.3 s of nothing after, for the bursts it still holds */
    while (left > 0) {
        const int k = left < FEED ? left : FEED;
        int nb = 0;
        const rdof_sdr_burst_t *b = rdof_demod_feed(&RAWD.d, flush ? zeros : (const rdof_cf_t *)(const void *)iq, k, &nb);
        for (int j = 0; j < nb && got < cap; j++, got++) {
            rdof_bl_burst_t *o = &out[got];
            memset(o, 0, sizeof *o);
            o->pos48 = b[j].position, o->lag48 = RDOF_DEMOD_LAG;
            o->cfo_hz = b[j].freq_offset_hz, o->dev_hz = b[j].deviation_hz, o->level_dbfs = b[j].b.power_dbfs;
            o->dt = (uint8_t)b[j].b.dt, o->n = (int8_t)b[j].b.n, o->slot = (int8_t)b[j].b.slot;
            o->family = (int8_t)b[j].b.family, o->cc = (int8_t)b[j].b.cc, o->sync_errors = (int8_t)b[j].b.sync_errors;
            memcpy(o->raw, b[j].b.burst, sizeof o->raw);
            o->has_cach = b[j].has_cach;
            memcpy(o->cach, b[j].cach, sizeof o->cach);
        }
        if (!flush)
            iq += 2 * k;
        left -= k;
    }
    if (flush) {
        rdof_demod_free(&RAWD.d);
        RAWD.on = false;
    }
    return got;
}

/* With --fpga-chz, an edge in the channels' power comes the filter's delay late (16 D + 1 taps: 8 output
 * instants; the CIC's few inputs left out): wideband samples */
#define FX_EDGE (FX.on ? (int64_t)(FX_LATE - 1) * FX.step : 0)

/* A turn's power at its wideband sample i: the sample's, or with --fpga-chz its output instant's */
static inline double turn_p2(const rdof_cf_t *in, int i)
{
    return FX.on ? FX.pw[FX.turn + i / FX.step] : (double)in[i].re * in[i].re + (double)in[i].im * in[i].im;
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

/* The other split repeaters on the SDR (the settings' second on): each its radio on a port, a repeater over the
 * split, and an SDR modem of its own in XS, receive only, on its input's channel. The first is R, S and P. */
typedef struct {
    const rdofradio_split_t *u;
    int i; /* in XS */
    port_t p;
    rdof_modem_t client;
    rdof_radio_t radio;
    rdof_repeater_t r;
    bool keyed, up;
    struct {
        int slot, rssi; /* rssi INT_MIN: lost */
        uint8_t control, burst[RDOF_BURST_BYTES];
    } q[HEARD_Q];
    int nq;
} xsplit_t;
static xsplit_t *X[RDOF_ROLES_MAX];

static void xs_to_radio(void *ctx, int i, const uint8_t *d, size_t n)
{
    if (!X[i]->p.gone && !rdofradio_serial_write(X[i]->p.fd, d, n))
        X[i]->p.gone = true;
}
static void xs_to_host(void *ctx, int i, const uint8_t *d, size_t n) { rdof_modem_feed(&X[i]->client, d, n); }
static int xs_channel(void *ctx, uint32_t hz) { return channel_of(hz); }

static void x_rx(void *ctx, int slot, uint8_t control, const uint8_t *burst, int rssi)
{
    xsplit_t *x = ctx;
    if (in_wait || !x->up) {
        if (x->nq < HEARD_Q) {
            x->q[x->nq].slot = slot, x->q[x->nq].rssi = rssi, x->q[x->nq].control = control;
            memcpy(x->q[x->nq++].burst, burst, RDOF_BURST_BYTES);
        }
        return;
    }
    if (rssi == INT_MIN)
        rdof_repeater_lost(&x->r, slot);
    else
        rdof_repeater_heard(&x->r, slot, control, burst, rssi);
}
static void x_lost(void *ctx, int slot) { x_rx(ctx, slot, 0, (const uint8_t[RDOF_BURST_BYTES]){0}, INT_MIN); }
static void x_deliver(xsplit_t *x)
{
    if (!x->up)
        return;
    for (int i = 0; i < x->nq; i++)
        if (x->q[i].rssi == INT_MIN)
            rdof_repeater_lost(&x->r, x->q[i].slot);
        else
            rdof_repeater_heard(&x->r, x->q[i].slot, x->q[i].control, x->q[i].burst, x->q[i].rssi);
    x->nq = 0;
}
static void x_nak(void *ctx, uint8_t cmd, int err)
{
    xsplit_t *x = ctx;
    fprintf(stderr, "%s: modem NAK 0x%02x err %d\n", x->u->name, cmd, err);
    rdof_repeater_nak(&x->r, cmd, err);
}
static void x_tx(void *ctx, const char *action, const rdof_tx_call_info_t *c, double d)
{
    const xsplit_t *x = ctx;
    say("%s: %s slot %d %u -> %s%u (%.2f s)\n", x->u->name, action, c->slot, c->src, c->group ? "TG " : "", c->dst, d);
}
static void x_key(void *ctx, bool keyed)
{
    xsplit_t *x = ctx;
    x->keyed = keyed;
    say("%s: %s\n", x->u->name, keyed ? "keyed" : "unkeyed");
}
static bool x_write(void *ctx, const uint8_t *d, size_t n)
{
    const xsplit_t *x = ctx;
    rdof_sdrsplits_upstream(&XS, x->i, d, n, now_s());
    return !rdof_sdrsplits_split(&XS, x->i)->failed[0];
}
static bool x_wait(void *ctx, int ms)
{
    xsplit_t *x = ctx;
    const double end = now_s() + ms / 1000.0;
    in_wait++;
    while (!x->client.reply_ready && !stop && !x->p.gone && now_s() < end)
        split_turn();
    in_wait--;
    return x->client.reply_ready;
}

/* A split's client, radio and repeater once its bring-up is done, and its place in the router. */
static bool x_start(xsplit_t *x)
{
    const rdof_modem_port_t mp = {x_write, x_wait, NULL, p_clock, p_sleep, x};
    rdof_modem_init(&x->client, &mp);
    x->client.on_rx = x_rx;
    x->client.on_lost = x_lost;
    x->client.on_nak = x_nak;
    x->client.cb_ctx = x;
    if (!rdof_modem_start(&x->client, 1)) {
        fprintf(stderr, "%s: modem: %s\n", x->u->name, x->client.error);
        return false;
    }
    rdof_radio_init(&x->radio, &x->client, rdof_radio_detect(x->client.version));
    rdof_repeater_init(&x->r, &x->radio, x->u->peer_id, x->u->rx_hz, x->u->tx_hz, x->u->cc);
    x->r.keep_up = true; /* a modem drops its carrier when keyed with nothing queued */
    rdofradio_cfg_apply_unit(&x->u->cfg, &x->r);
    x->r.clock = p_clock;
    x->r.on_tx = x_tx;
    x->r.on_key = x_key;
    x->r.ctx = x;
    if (!rdof_repeater_start(&x->r)) {
        fprintf(stderr, "%s: repeater start: %s\n", x->u->name, x->client.error);
        return false;
    }
    x->up = true;
    x_deliver(x);
    if (sr_on) {
        const rdof_site_t *s = &rcfg.site;
        for (int a = 0; a < s->napp; a++)
            if (s->app[a].type == RDOF_APP_REPEATER && s->freq[s->app[a].in].hz == x->u->rx_hz)
                rdof_siteroute_add_repeater(&SR, a, &x->r);
    }
    const rdof_sdrmodem_t *m = rdof_sdrsplits_modem(&XS, x->i);
    fprintf(stderr, "split repeater %s: TX %s on %s; RX %.4f MHz (SDR channel %d); TX %.4f MHz, cc %d\n", x->u->name,
            rdof_sdrsplits_split(&XS, x->i)->halves[RDOF_SPLIT_TX].description, x->u->port, x->u->rx_hz / 1e6,
            m ? m->channel : -1, x->u->tx_hz / 1e6, x->u->cc);
    return true;
}

static void split_service(void)
{
    uint8_t b[256];
    double t;
    int n;
    while ((n = port_take(&P, b, &t)) > 0)
        rdof_split_from_half(&S, RDOF_SPLIT_TX, b, (size_t)n, t);
    rdof_split_tick(&S, now_s());
    split_flush();
    if (!stop && (P.gone || S.failed[0])) {
        say("split: %s\n", P.gone ? "the radio's port went away" : S.failed);
        lost_device = 1;
        stop = 1;
    }
    for (int k = 0; k < NX; k++) {
        while ((n = port_take(&X[k]->p, b, &t)) > 0)
            rdof_sdrsplits_from_radio(&XS, k, b, (size_t)n, t);
        const rdof_split_t *xs = rdof_sdrsplits_split(&XS, k);
        if (!stop && (X[k]->p.gone || xs->failed[0])) {
            say("%s: split: %s\n", X[k]->u->name, X[k]->p.gone ? "the radio's port went away" : xs->failed);
            lost_device = 1;
            stop = 1;
        }
    }
    if (NX)
        rdof_sdrsplits_tick(&XS, now_s());
    if (!in_wait) {
        deliver_heard();
        for (int k = 0; k < NX; k++)
            x_deliver(X[k]);
    }
}

/* a chunk of the receive stream through the receiver (--split-tx, --mmdvm: the SDR receives only); with
   --fpga-chz the block's frames, which went through the receiver in the read (fx_take) */
static void rx_turn(void)
{
    static rdof_cf_t in[CHUNK];
    bench_tune_poll();
    const double t_in = now_s();
    FX.t_rx = 0;
    const int n = FX.on ? fx_take(SC.chunk, 100000) : dev->read(dev, in, SC.chunk, 100000);
    const double t = now_s() - FX.t_rx;
    WS.wait += t - t_in, WS.rx += FX.t_rx;
    if (n == RDOF_DEV_TIMEOUT) {
        if (++SC.timeouts >= 50 && !stop) {
            say("read: nothing for 5 s, stopping\n");
            lost_device = 1;
            stop = 1;
        }
    } else if (n == RDOF_DEV_OVERFLOW) {
        SC.overflows++;
        WS.overflows++;
    } else if (n < 0) {
        WS.errors++;
        if (++SC.errors > 10 && !stop) {
            say("%s\n", dev->error);
            lost_device = 1;
            stop = 1;
        }
    } else if (n > 0) {
        SC.timeouts = 0;
        SC.total += n;
        WS.last_samples = t;
        WS.n += n;
        clock_read(t, t - t_in >= FRESH_S);
#ifdef RDOF_HOST_WEB
        if (!FX.on)
            web_spectrum(in, n);
#endif
        if (FX.on)
            bench_chan_feed(n / FX.step);
        bench_feed(in, n, t - t_in >= FRESH_S, FX.on);
        const double t3 = now_s();
        if (!FX.on)
            rx_chunk(in, n);
        const double t4 = now_s();
        WS.spec += t3 - t, WS.rx += t4 - t3;
    }
}

/* a chunk through the receiver, then the split */
static void split_turn(void)
{
    rx_turn();
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
    /* the other split repeaters: each radio's port and reader */
    const rdof_sdrsplits_io_t xio = {xs_to_radio, xs_to_host, xs_channel, split_now48, NULL};
    rdof_sdrsplits_init(&XS, &xio, (uint32_t)R.rate);
    for (int k = 1; k < rcfg.nsplit; k++) {
        const rdofradio_split_t *u = &rcfg.split[k];
        xsplit_t *x = calloc(1, sizeof *x);
        if (!x) {
            fprintf(stderr, "%s: no memory\n", u->name);
            return 1;
        }
        x->u = u, x->p.path = u->port, x->p.mu = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
        x->p.fd = rdofradio_serial_open(u->port, u->baud ? (int)u->baud : baud, err, sizeof err);
        if (x->p.fd < 0) {
            fprintf(stderr, "%s: %s\n", u->name, err);
            free(x);
            return 1;
        }
        if (pthread_create(&x->p.thread, NULL, port_reader, &x->p)) {
            fprintf(stderr, "%s: the port reader: no thread\n", u->name);
            close(x->p.fd);
            free(x);
            return 1;
        }
        X[NX++] = x;
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
    for (int k = 0; k < NX; k++) {
        const rdof_config_t *c = &X[k]->u->cfg;
        rdof_split_params_t xp = {c->split_power_cap, c->split_prebuffer, c->split_lease_s, c->split_cooldown_s,
                                  c->split_max_key_s};
        if ((X[k]->i = rdof_sdrsplits_add(&XS, &xp, now_s())) < 0) {
            fprintf(stderr, "%s: %s\n", X[k]->u->name, XS.error);
            goto out;
        }
        rdof_sdrsplits_split(&XS, k)->log = say_log;
    }
    const double t_up = now_s();
    while (!stop && !(S.ready && (!NX || rdof_sdrsplits_ready(&XS))) && !S.failed[0] && rdof_sdrsplits_failed(&XS) < 0 &&
           now_s() - t_up < 15)
        split_turn();
    if (!S.ready) {
        fprintf(stderr, "split: %s\n", S.failed[0] ? S.failed : P.gone ? "the radio's port went away" : "no answer in 15 s");
        goto out;
    }
    for (int k = 0; k < NX; k++) {
        const rdof_split_t *xs = rdof_sdrsplits_split(&XS, k);
        if (!xs->ready) {
            fprintf(stderr, "%s: split: %s\n", X[k]->u->name,
                    xs->failed[0] ? xs->failed : X[k]->p.gone ? "the radio's port went away" : "no answer in 15 s");
            goto out;
        }
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
    R.r.keep_up = R.mmdvm; /* a modem drops its carrier when keyed with nothing queued */
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
    for (int k = 0; k < NX; k++)
        if (!x_start(X[k]))
            goto out;
#ifdef RDOF_HOST_WEB
    if (web_port >= 0) {
        web_settings(argc, argv);
        const int wp = rdof_hostweb_start(web_addr, web_port, rcfg.box);
        if (wp < 0)
            fprintf(stderr, "web: %s\n", strerror(errno));
        else
            fprintf(stderr, "web: http://%s:%d/\n", web_addr, wp), advertise_web(wp);
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
            for (int k = 0; k < NX; k++)
                if (X[k]->up && !rdof_repeater_poll(&X[k]->r))
                    fprintf(stderr, "%s: poll: %s\n", X[k]->u->name, X[k]->client.error);
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
    for (int k = 0; k < NX; k++)
        if (X[k]->up)
            rdof_repeater_stop(&X[k]->r);
    for (int i = 0; i < 10; i++) /* the repeaters' last frames out to the radios */
        split_service();
    rc = lost_device ? 1 : 0;
out:
    P.quit = true;
    pthread_join(P.thread, NULL);
    close(P.fd);
    for (int k = 0; k < NX; k++) {
        const rdof_sdrmodem_t *m = rdof_sdrsplits_modem(&XS, k);
        if (m)
            fprintf(stderr, "%s: SDR modem %u heard (%u TS1, %u TS2, %u unlabeled), %u lost; port %u bytes dropped\n",
                    X[k]->u->name, m->rx_bursts, m->labeled[0], m->labeled[1], m->unlabeled, m->lost, X[k]->p.dropped);
        X[k]->p.quit = true;
        pthread_join(X[k]->p.thread, NULL);
        close(X[k]->p.fd);
        free(X[k]);
        X[k] = NULL;
    }
    NX = 0;
    rdof_sdrsplits_free(&XS);
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

/* ── --mmdvm: the unit on an MMDVM modem on a serial port; the SDR, if any, receives only ─────────── */

/* a whole frame to the modem, its RF level capped */
static bool mm_write(const uint8_t *d, size_t n)
{
    uint8_t f[RDOF_MMDVM_MAX_FRAME];
    if (P.gone)
        return false;
    if (n <= sizeof f && d[0] == RDOF_MMDVM_FRAME_START && n > 2 && d[2] == RDOF_MMDVM_SET_FREQ) {
        memcpy(f, d, n);
        if (rdof_mmdvm_cap_rf_level(f, n, mm_rf_level))
            mm_capped++;
        d = f;
    }
    if (!rdofradio_serial_write(P.fd, d, n))
        P.gone = true;
    return !P.gone;
}

/* what the port read: to the bench's job while one has the modem, else to the client */
static void mm_service(void)
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
        memcpy(b, P.q[i].b, (size_t)n);
        pthread_mutex_unlock(&P.mu);
        if (!bench_modem_reply(b, (size_t)n))
            rdof_modem_feed(&R.client, b, (size_t)n);
    }
    if (!stop && P.gone) {
        say("modem: the port went away\n");
        lost_device = 1;
        stop = 1;
    }
    if (!in_wait)
        deliver_heard();
}

/* the loop's turn: a chunk of the receive stream (no SDR: a short sleep), then what the modem sent */
static void mm_turn(void)
{
    if (dev) {
        rx_turn();
    } else {
        const struct timespec ts = {0, 5000000L};
        nanosleep(&ts, NULL);
    }
    mm_service();
    ws_turn(now_s());
}

/* a burst the modem heard, to its tracker: the page, MQTT, HBlink4 */
static void mm_heard(int slot, uint8_t control, const uint8_t *burst, int rssi)
{
    uint32_t f = heard_hz();
    int cc = R.cc, dbm = 0;
    if (!R.full && R.t.ready) {
        uint32_t tf;
        int tcc;
        rdof_txvr_channel_now(&R.t, &tf, &tcc);
        if (tf)
            cc = tcc;
    }
    rdof_burst_t b;
    rdof_modem_burst(control, burst, rdof_radio_rssi(&R.radio, rssi, f, &dbm) ? (double)dbm : NAN, cc, slot, &b);
    MT.freq_hz = f;
    rdof_tracker_feed(&MT, &b, wall_s());
    mm_heard_n++;
}

/* the tracker's calls: the transceiver's listen-before-talk, as the board's roamer */
static void mm_on_call(void *ctx, const char *ch, const char *action, uint32_t src, uint32_t dst)
{
    if (!R.full)
        rdof_txvr_heard_call(&R.t, action, src, dst);
}

static void mm_tracker_init(void)
{
    rdof_tracker_init(&MT, "rx", 0.0, R.rx_hz);
    rdof_sink_t out = {.on_json = on_json, .on_call = mm_on_call};
#ifdef RDOF_MQTT
    if (rcfg.cfg.has_mqtt)
        out.on_raw_burst = on_raw_burst;
#endif
    rdof_tracker_add_sink(&MT, out);
}

/* The whole run with --mmdvm, once the SDR (if any) is open receive only. */
static int mmdvm_main(int argc, char **argv, const char *port, int baud, double seconds, double rx_off, double tx_off,
                      double rx_level, double tx_level)
{
    char err[256];
    P.path = port;
    P.fd = rdofradio_serial_open(port, baud, err, sizeof err);
    if (P.fd < 0) {
        fprintf(stderr, "--mmdvm %s\n", err);
        return 1;
    }
    if (pthread_create(&P.thread, NULL, port_reader, NULL)) {
        fprintf(stderr, "the port reader: no thread\n");
        close(P.fd);
        return 1;
    }
    int rc = 1;
    const double t0 = now_s();
    if (dev) {
        SC.rate = R.rate;
        SC.floor = SC.last_win = SC.win_min = INFINITY;
        SC.chunk = (int)(R.rate / 100) < CHUNK ? (int)(R.rate / 100) : CHUNK;
        if (dev->start(dev)) {
            fprintf(stderr, "start: %s\n", dev->error);
            goto out;
        }
        SC.t0 = SC.win_start = now_s();
    }
    const rdof_modem_port_t mp = {p_write, p_wait, NULL, p_clock, p_sleep, &R};
    rdof_modem_init(&R.client, &mp);
    R.client.on_rx = on_rx;
    R.client.on_lost = on_lost;
    R.client.on_nak = on_nak;
    R.client.cb_ctx = &R;
    if (!rdof_modem_start(&R.client, 3)) {
        fprintf(stderr, "modem on %s: %s\n", port, R.client.error);
        goto out;
    }
    if (config_path) {
        if (!rdofradio_cfg_radio(&rcfg, &R.radio, &R.client, R.client.version, err, sizeof err)) {
            fprintf(stderr, "modem: %s\n", err);
            goto out;
        }
    } else {
        rdof_radio_init(&R.radio, &R.client, rdof_radio_detect(R.client.version));
        R.radio.rx_offset_hz = (int32_t)lround(rx_off);
        R.radio.tx_offset_hz = (int32_t)lround(tx_off);
        if (!isnan(rx_level))
            R.radio.config.rx_level = rx_level;
        if (!isnan(tx_level))
            R.radio.config.tx_level = tx_level;
    }
    bool ok;
    if (R.full) {
        rdof_repeater_init(&R.r, &R.radio, rcfg.peer_id, R.rx_hz, R.tx_hz, R.cc);
        R.r.keep_up = R.mmdvm; /* a modem drops its carrier when keyed with nothing queued */
        if (config_path) {
            rdofradio_cfg_apply_repeater(&rcfg, &R.r);
            R.r.power = rcfg.cfg.power;
        }
        R.r.clock = p_clock;
        R.r.on_tx = on_tx;
        R.r.on_sent = on_sent;
        R.r.on_key = on_key;
        R.r.ctx = &R;
        ok = rdof_repeater_start(&R.r);
    } else {
        rdof_txvr_init(&R.t, &R.radio, rcfg.peer_id);
        if (config_path) {
            rdofradio_cfg_apply_txvr(&rcfg, &R.t);
        } else {
            R.t.channels[0] = (rdof_channel_t){R.rx_hz, R.cc, 255};
            R.t.nchannels = 1;
            R.t.has_idle = true;
            R.t.idle = R.t.channels[0];
            if (R.tx_hz != R.rx_hz)
                R.t.channels[R.t.nchannels++] = (rdof_channel_t){R.tx_hz, R.cc, 255};
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
        goto out;
    }
    deliver_heard();
    fprintf(stderr, "%s: %s (%s) on %s, RX %.4f MHz, TX %.4f MHz, cc %d, RF level cap %d%s%s\n",
            R.full ? "repeater" : "transceiver", R.client.version, rdof_radio_kind_name(R.radio.kind), port,
            R.rx_hz / 1e6, R.tx_hz / 1e6, R.cc, mm_rf_level, dev ? "; SDR: " : "", dev ? dev->name : "");
#ifdef RDOF_HOST_WEB
    if (web_port >= 0) {
        web_settings(argc, argv);
        const int wp = rdof_hostweb_start(web_addr, web_port, rcfg.box);
        if (wp < 0)
            fprintf(stderr, "web: %s\n", strerror(errno));
        else
            fprintf(stderr, "web: http://%s:%d/\n", web_addr, wp), advertise_web(wp);
    }
    double last_web = 0;
#endif
    if (dev)
        bench_start();
    else if (bench_port >= 0)
        fprintf(stderr, "bench: its line protocol needs an SDR (DEVICE): off\n");
    double last_poll = 0, last_status = now_s();
    while (!stop && (seconds <= 0 || now_s() - t0 < seconds)) {
        mm_turn();
        bench_ob_fast();
        const double now = now_s();
        ms_poll(now);
        if (now - last_poll >= POLL_S) {
            last_poll = now;
            if (R.parrot)
                rdof_parrot_poll(R.parrot, now);
            if (!bench_tx_poll(now) && !bench_rpt_poll(now) &&
                !(R.full ? rdof_repeater_poll(&R.r) : rdof_txvr_poll(&R.t)))
                fprintf(stderr, "poll: %s\n", R.client.error);
            rdof_tracker_tick(&MT, wall_s());
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
            fprintf(stderr, "status %.1f s: %s, modem heard %lld, %u requests (%u unanswered, %u refused)%s\n",
                    now - t0, R.keyed ? "keyed" : "idle", mm_heard_n, R.client.requests, R.client.no_reply,
                    R.client.refused, dev ? "" : "; no SDR");
        }
    }
    if (R.full)
        rdof_repeater_stop(&R.r);
    rdof_modem_set_mode(&R.client, RDOF_MODE_IDLE); /* the modem idle: it doesn't key on its own */
    for (int i = 0; i < 10; i++) /* the last frames out to the modem */
        mm_service();
    rc = lost_device ? 1 : 0;
out:
    P.quit = true;
    pthread_join(P.thread, NULL);
    close(P.fd);
    if (dev)
        dev->close(dev);
    fprintf(stderr, "%.1f s; modem: %lld bursts heard, %u requests, %u unanswered, %u refused, %lld SET_FREQ capped; "
                    "port: %u bytes dropped",
            now_s() - t0, mm_heard_n, R.client.requests, R.client.no_reply, R.client.refused, mm_capped, P.dropped);
    if (dev)
        fprintf(stderr, "; SDR: %d overflows, %.1f ms of samples lost", SC.overflows, 1e3 * (double)SC.filled / R.rate);
    fprintf(stderr, "\n");
    for (int c = 0; dev && c < NCH; c++)
        fprintf(stderr, "%s %.4f MHz: %ld bursts\n", CH[c].id, CH[c].freq_hz / 1e6, R.rx.bursts[c]);
    if (R.parrot)
        say("parrot: %u sent back, %u not to it, %u bursts cut\n", R.parrot->replayed, R.parrot->ignored,
            R.parrot->cut);
    bench_stop();
    mq_stop();
    net_close();
    clock_stop();
    if (dev)
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
    advertise_web(port);
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
        const struct timespec half = {0, 500000000L}; /* usleep isn't POSIX 2008 (glibc hides it) */
        nanosleep(&half, NULL);
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
    static const char *settings[16];
    int n_settings = 0;
    const char *soapy = NULL, *iio = NULL, *stream_args = "", *fft = NULL, *split_tx = NULL, *mmdvm = NULL;
    bool sim_sdr = false; /* --sim-sdr: rdofradio_sim.c's receiver, for no-RF tests */
    double mm_rx_off = 0, mm_tx_off = 0, mm_rx_level = NAN, mm_tx_level = NAN;
    int mm_flags = 0;
    int baud = 115200, power_cap = 0, tx_flags = 0;
    double bw = 0, rate = 0, rx = 0, tx = 0, tx_lo = 0, center = 0, gain = NAN, txg = 0, seconds = 0, offset_s = 0;
    double tx_dbm = NAN;
    bool txg_given = false;
    int cc = 1, rx_slot = 1, allow = 0, monitor = 0, verbose = 0, fpga_board = 0;
    const char *duplex = "full", *fpga_bit = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
#define NEXT()                                                                                                    \
    do {                                                                                                          \
        if (!v)                                                                                                   \
            usage();                                                                                              \
        i++;                                                                                                      \
    } while (0)
        if (!strcmp(a, "--sim-sdr")) {
            sim_sdr = true;
        } else if (!strcmp(a, "--soapy")) {
            NEXT();
            soapy = v;
        } else if (!strcmp(a, "--iio")) {
            NEXT();
            iio = v;
        } else if (!strcmp(a, "--fpga-chz")) {
            FX.on = true;
            if (v && !strncmp(v, "0x", 2)) {
                FX.addr = strtoul(v, NULL, 16);
                i++;
            }
        } else if (!strcmp(a, "--fpga-duc")) {
            DX.on = true;
            if (v && !strncmp(v, "0x", 2)) {
                DX.addr = strtoul(v, NULL, 16);
                i++;
            }
        } else if (!strcmp(a, "--split-tx")) {
            NEXT();
            split_tx = v;
        } else if (!strcmp(a, "--mmdvm")) {
            NEXT();
            mmdvm = v;
        } else if (!strcmp(a, "--rf-level")) {
            NEXT();
            mm_rf_level = atoi(v);
            mm_flags++;
            if (mm_rf_level < 0 || mm_rf_level > 255)
                usage();
        } else if (!strcmp(a, "--rx-offset-hz")) {
            NEXT();
            mm_rx_off = atof(v);
            mm_flags++;
        } else if (!strcmp(a, "--tx-offset-hz")) {
            NEXT();
            mm_tx_off = atof(v);
            mm_flags++;
        } else if (!strcmp(a, "--rx-level")) {
            NEXT();
            mm_rx_level = atof(v);
            mm_flags++;
        } else if (!strcmp(a, "--tx-level")) {
            NEXT();
            mm_tx_level = atof(v);
            mm_flags++;
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
        } else if (!strcmp(a, "--setting")) {
            NEXT();
            if (n_settings >= 16 || !strchr(v, '=')) {
                fprintf(stderr, "--setting KEY=VALUE (at most 16): %s\n", v);
                return 2;
            }
            settings[n_settings++] = v;
        } else if (!strcmp(a, "--stream")) {
            NEXT();
            stream_args = v;
        } else if (!strcmp(a, "--bw")) {
            NEXT();
            bw = mhz(v);
        } else if (!strcmp(a, "--ppm")) {
            NEXT();
            char *end;
            xtal_ppm = strtod(v, &end);
            if (*end || !isfinite(xtal_ppm) || fabs(xtal_ppm) > 200) {
                fprintf(stderr, "--ppm: the crystal's error in ppm (positive: fast), -200 to 200\n");
                return 2;
            }
        } else if (!strcmp(a, "--ppm-tol")) {
            NEXT();
            char *end;
            xtal_tol = strtod(v, &end);
            if (*end || !(xtal_tol > 0) || xtal_tol > 50) {
                fprintf(stderr, "--ppm-tol: how far --ppm may be off, ppm (over 0, at most 50)\n");
                return 2;
            }
        } else if (!strcmp(a, "--tx-gain")) {
            NEXT();
            txg = atof(v);
            txg_given = true;
            tx_flags++;
        } else if (!strcmp(a, "--tx-power")) {
            NEXT();
            char *end;
            tx_dbm = strtod(v, &end);
            if (*end || !(tx_dbm >= -60 && tx_dbm <= 40)) {
                fprintf(stderr, "--tx-power: dBm at the antenna, -60 to 40\n");
                return 2;
            }
            tx_flags++;
        } else if (!strcmp(a, "--tx-max-dbm")) {
            NEXT();
            char *end;
            tx_max_dbm = strtod(v, &end);
            if (*end || !(tx_max_dbm >= -30 && tx_max_dbm <= 40)) {
                fprintf(stderr, "--tx-max-dbm: the device's output at its most gain, dBm (-30 to 40)\n");
                return 2;
            }
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
            for (const char *t = v; *t;) {
                char *end;
                strtod(t, &end);
                if (*end == '-') { /* a band: LOW-HIGH */
                    const double lo = mhz(t), hi = mhz(end + 1);
                    if (!(lo < hi) || bench_nband >= RDOF_BL_TX_BANDS) {
                        fprintf(stderr, "--bench-tx: a band is LOW-HIGH, low below high, %d at most\n",
                                RDOF_BL_TX_BANDS);
                        return 2;
                    }
                    bench_band_hz[bench_nband][0] = lo, bench_band_hz[bench_nband++][1] = hi;
                } else if (bench_ntx < RDOF_BENCHTX_MAX_FREQS) {
                    bench_tx_mhz[bench_ntx++] = mhz(t) / 1e6;
                } else {
                    fprintf(stderr, "--bench-tx: %d frequencies at most (a band LOW-HIGH takes any number)\n",
                            RDOF_BENCHTX_MAX_FREQS);
                    return 2;
                }
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
        } else if (!strcmp(a, "--ota")) {
            NEXT();
            web_ota(v);
#endif
        } else if (!strcmp(a, "--fpga-bit")) {
            NEXT();
            fpga_bit = v;
        } else if (!strcmp(a, "--fpga-board")) {
            fpga_board = 1;
        } else {
            usage();
        }
    }
    if (fpga_board) { /* the board's name for its bitstream (tools/pluto/bundle/install.sh) */
        char err[200];
        const char *b = rdofradio_fpga_board(err, sizeof err);
        if (!b) {
            fprintf(stderr, "%s\n", err);
            return 1;
        }
        char f[64];
        rdof_plutofpga_file(b, f, sizeof f);
        printf("%s %s\n", b, f);
        return 0;
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
        /* split repeaters on the SDR: the first radio's port from the file unless --split-tx gives it */
        if (rcfg.nsplit && !split_tx && !mmdvm && rcfg.split[0].port[0]) {
            split_tx = rcfg.split[0].port;
            if (rcfg.split[0].baud)
                baud = (int)rcfg.split[0].baud;
        }
        if (rcfg.nsplit > 1 && !split_tx) {
            fprintf(stderr, "%s: %d split repeaters: their radios transmit (give the first a port path or --split-tx)\n",
                    config_path, rcfg.nsplit);
            return 2;
        }
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
    R.mmdvm = mmdvm != NULL;
    if (n_settings && !soapy) {
        fprintf(stderr, "--setting: a SoapySDR driver's (--soapy)\n");
        return 2;
    }
    if ((soapy && iio) || (!soapy && !iio && !sim_sdr && !R.mmdvm) || !rx || (!R.full && strcmp(duplex, "half")))
        usage();
    if (config_path && !R.mmdvm && rcfg.on_modem && !rcfg.full) {
        char why[300];
        snprintf(why, sizeof why, "%s: %s: a transceiver on a modem: give the modem's port with --mmdvm PORT",
                 config_path, rcfg.name);
        return hold(argc, argv, why);
    }
    if (config_path && R.mmdvm && !rcfg.on_modem) {
        char why[300];
        snprintf(why, sizeof why, "%s: %s isn't on a modem: --mmdvm runs a unit on a modem device", config_path,
                 rcfg.name);
        return hold(argc, argv, why);
    }
    if (R.mmdvm && (split_tx || tx_flags)) {
        fprintf(stderr, "--mmdvm: the modem transmits; --split-tx, --tx-power, --tx-max-dbm, --tx-gain, --tx-lo, "
                        "--tx-offset and --monitor are the SDR's\n");
        return 2;
    }
    if (!R.mmdvm && mm_flags) {
        fprintf(stderr, "--rf-level, --rx-offset-hz, --tx-offset-hz, --rx-level and --tx-level are --mmdvm's\n");
        return 2;
    }
    if (config_path && R.mmdvm && (mm_rx_off || mm_tx_off || !isnan(mm_rx_level) || !isnan(mm_tx_level))) {
        fprintf(stderr, "--config: the modem's offsets and levels are the device's mmdvm settings in %s\n",
                config_path);
        return 2;
    }
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
    if ((FX.on || DX.on) && dev_local) {
        /* the bitstream in (rdofradio_fpga.h); none: the ARM's channelizer and a full-rate transmit stream, with
           the channels that fit (fpga_fallback) */
        char msg[400];
        const int r = rdofradio_fpga_load(fpga_bit, msg, sizeof msg);
        if (r == RDOFRADIO_FPGA_ERROR) {
            fprintf(stderr, "fpga: %s\n", msg);
            return 1;
        }
        if (r == RDOFRADIO_FPGA_NONE) {
            fprintf(stderr, "fpga: %s: running without it (--fpga-chz and --fpga-duc off)\n", msg);
            FX.on = DX.on = false;
            fpga_fallback = true;
        } else {
            fprintf(stderr, "fpga: %s\n", msg);
        }
    }
    if (FX.on) {
        char why[256];
        if (!iio) {
            fprintf(stderr, "--fpga-chz: the FPGA channelizer is a PlutoSDR's: with --iio\n");
            return 2;
        }
        if (fx_probe(why, sizeof why)) {
            fprintf(stderr, "--fpga-chz: %s\n", why);
            return 1;
        }
        fprintf(stderr, "--fpga-chz: the block at 0x%08lx: %d channels, D up to %d, %d taps, CIC R up to %d\n",
                FX.addr, FX.h.channels, FX.h.max_decim, FX.h.max_taps, FX.h.max_cic);
    }
    if (DX.on && !iio) {
        fprintf(stderr, "--fpga-duc: the FPGA's TX interpolator is a PlutoSDR's transmitter's: with --iio\n");
        return 2;
    }
    if (DX.on && (split_tx || mmdvm)) {
        fprintf(stderr, "--fpga-duc: %s transmits, not the SDR: not used\n", split_tx ? "the --split-tx radio" : "the modem");
        DX.on = false;
    }
    if (DX.on) {
        char why[256];
        if (dx_probe(why, sizeof why)) {
            fprintf(stderr, "--fpga-duc: %s\n", why);
            return 1;
        }
    }
    R.split = split_tx != NULL;
    if (R.split && !R.full) {
        fprintf(stderr, "--split-tx runs a repeater: full duplex\n");
        return 2;
    }
    if (R.split && tx_flags) {
        fprintf(stderr, "--split-tx: the radio transmits; --tx-power, --tx-max-dbm, --tx-gain, --tx-lo, --tx-offset "
                        "and --monitor are the SDR's\n");
        return 2;
    }
    if (!tx)
        tx = rx;
    const bool tx_lo_given = tx_lo != 0;
    if (!tx_lo)
        tx_lo = tx + 100e3;
    R.txr = 1, R.lo_off = 100e3;
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

    if (R.mmdvm) {
        mm_tracker_init(); /* the modem's: the radio's RX for the network, MQTT and the page */
        if (!soapy && !iio && !sim_sdr) { /* no SDR: the modem alone */
            if (rcfg.nchan)
                fprintf(stderr, "receiver: no SDR (DEVICE): its %d channels left out\n", rcfg.nchan);
            sr_start();
            net_start();
            mq_start();
            clock_start();
#ifdef RDOF_HOST_WEB
            rdof_hostweb_version(RDOF_VERSION);
#endif
            signal(SIGINT, on_signal);
            signal(SIGTERM, on_signal);
            return mmdvm_main(argc, argv, mmdvm, baud, seconds, mm_rx_off, mm_tx_off, mm_rx_level, mm_tx_level);
        }
    }
    /* the receiver's channels: the settings' (the SDR's, as the board's SDR resolves them; the unit's among them), in
       their order; then --monitor's TX (our own outbound, heard back) and --mmdvm's modem RX when no channel is on
       them. Without the FPGA's bitstream, the ones that fit the ARM's window, the unit's kept. */
    int unfit = 0, nlisted = 0;
    {
        rdof_sdr_channel_t own[2];
        int nown = 0;
        if (R.mmdvm && rdofradio_cfg_channel_at(&rcfg, R.rx_hz) < 0)
            own[nown++] = (rdof_sdr_channel_t){"modem-rx", rx, cc, rx_slot};
        if (monitor && rdofradio_cfg_channel_at(&rcfg, R.tx_hz) < 0 && R.tx_hz != R.rx_hz)
            own[nown++] = (rdof_sdr_channel_t){"out", tx, cc, 1};
        if (!rcfg.nchan && !R.mmdvm) /* the settings put nothing on the SDR (it can't be: a unit on it hears there) */
            own[nown++] = (rdof_sdr_channel_t){"rx", rx, cc, rx_slot};
        for (int i = 0; i < rcfg.nchan && NCH < RDOF_SDR_MAX_CHANNELS; i++) {
            const rdof_sdr_cfg_channel_t *m = &rcfg.chan[i];
            const uint32_t f = (uint32_t)llround(m->freq);
            const bool unit = f == R.rx_hz || (!R.full && f == R.tx_hz);
            nlisted++;
            CH[NCH] = (rdof_sdr_channel_t){m->id, m->freq, m->cc, m->slot};
            if (fpga_fallback && !unit) { /* what's kept, this one, the unit's still to come and our own */
                rdof_sdr_channel_t t[RDOF_SDR_MAX_CHANNELS + 2];
                int n = NCH + 1;
                memcpy(t, CH, sizeof t[0] * (size_t)n);
                for (int k = i + 1; k < rcfg.nchan && n < RDOF_SDR_MAX_CHANNELS; k++)
                    if ((uint32_t)llround(rcfg.chan[k].freq) == R.rx_hz)
                        t[n++] = (rdof_sdr_channel_t){rcfg.chan[k].id, rcfg.chan[k].freq, rcfg.chan[k].cc, 1};
                for (int k = 0; k < nown; k++)
                    t[n++] = own[k];
                char why[200];
                if (!plan_rate(t, n, why, sizeof why)) {
                    unfit++;
                    continue;
                }
            }
            CHN[nchn].c = NCH;
            CHN[nchn++].ch = m;
            NCH++;
        }
        for (int k = 0; k < nown && NCH < RDOF_SDR_MAX_CHANNELS; k++)
            CH[NCH++] = own[k];
        /* --mmdvm's other side: the SDR hears both of a duplex modem's frequencies, when one window holds them. A
           split wider than the SDR's rate still runs, the modem's RX kept; the TX side then takes a tune of its own. */
        if (R.mmdvm && R.tx_hz != R.rx_hz && rdofradio_cfg_channel_at(&rcfg, R.tx_hz) < 0 && NCH < RDOF_SDR_MAX_CHANNELS) {
            char why[200];
            CH[NCH] = (rdof_sdr_channel_t){"modem-tx", tx, cc, 1};
            if (plan_rate(CH, NCH + 1, why, sizeof why))
                NCH++;
            else
                fprintf(stderr, "receiver: the modem's TX, %.4f MHz, has no channel: %s\n", tx / 1e6, why);
        }
    }
    R.mon = monitor ? channel_of(tx) : -1;
    if (unfit)
        fprintf(stderr, "receiver: without the FPGA's bitstream %d channels don't fit the ARM's window: left out\n",
                unfit);
    if (nlisted < rcfg.nchan)
        fprintf(stderr, "receiver: %d channels at most: %d channels left out\n", RDOF_SDR_MAX_CHANNELS,
                rcfg.nchan - nlisted);
    {
        char why[200];
        double fit = plan_rate(CH, NCH, why, sizeof why);
        if (!fit && DX.on) {
            DX.on = DX.probed = false; /* the transmit stream at the device's rate */
            if ((fit = plan_rate(CH, NCH, why, sizeof why)))
                fprintf(stderr, "--fpga-duc: no rate it makes holds these channels: off, the transmit stream at "
                                "%.0f S/s\n", fit);
        }
        if (!fit)
            return hold(argc, argv, why);
        if (!rate)
            fprintf(stderr, "receiver: %.0f S/s (%d channels over %.4f MHz)\n", fit, NCH, span_of(CH, NCH) / 1e6);
        rate = fit;
        R.rate = rate;
    }
    /* --fpga-chz: the receiver takes the block's channel samples (rdof_sdr_rx_channels_in), FX_MAX a call */
    const rdof_sdr_rx_config_t rc = {rate, center, -55.0, 0.0, true, FX.on ? FX_MAX : CHUNK, FX.on};
    rdof_sink_t sink = {.on_json = on_json, .on_call = on_call};
#ifdef RDOF_MQTT
    if (rcfg.cfg.has_mqtt)
        sink.on_raw_burst = on_raw_burst; /* every burst on .../raw, as the board's */
#endif
    if (rdof_sdr_rx_init(&R.rx, &rc, CH, NCH, &sink, (double)time(NULL))) {
        char why[200];
        snprintf(why, sizeof why, "receiver: the rate isn't a multiple of 48 kS/s, or the channels don't fit one "
                                  "window%s", NCH > 1 ? " (a wider --rate, or fewer channels)" : "");
        return hold(argc, argv, why);
    }
    for (int c = 0; c < NCH; c++) /* --ppm: each channel where its carrier shows */
        rdof_sdr_rx_set_correction(&R.rx, c, xtal_base_hz(c));
    R.rx.on_burst = on_burst;
    R.rx.muted = muted;
    R.rx.relabel = relabel; /* the tracker's JSON (calls, BER, RSSI) with the inbound slot, not slot 1 */
    R.rx.drop = echo_drop;
    R.tx_ch = R.tx_img = R.echo_ch = R.echo_img = -1;
    R.rx.burst_ctx = &R;
    sr_start();
    net_start();
    chan_start();
    mq_start();
    clock_start();
#ifdef RDOF_HOST_WEB
    rdof_hostweb_version(RDOF_VERSION);
#endif

    /* the device: RX and TX at one rate, each on its own LO, the TX gain at its least */
    char derr[256] = "";
    if (FX.on) { /* the block first, so the DMA's first buffer holds its frames */
        if (fx_start(derr, sizeof derr)) {
            fprintf(stderr, "--fpga-chz: %s\n", derr);
            return 1;
        }
        fprintf(stderr, "--fpga-chz: %d channels, R %d, D %d, %d taps, 48 kS/s from %.0f S/s\n", FX.C, FX.R, FX.D,
                FX.cfg.taps, rate);
    }
    R.tx_rate = rate;
    if (DX.on) { /* on before the transmit stream starts: its first sample is the interpolator's */
        if (dx_start(rate, derr, sizeof derr)) {
            fprintf(stderr, "--fpga-duc: %s\n", derr);
            return 1;
        }
        if (!tx_lo_given)
            tx_lo = tx + R.lo_off, R.tx_lo = tx_lo;
        fprintf(stderr, "--fpga-duc: transmit at %.0f S/s, R %d to %.0f S/s, the TX LO %.0f kHz above\n", R.tx_rate,
                R.txr, rate, (tx_lo - tx) / 1e3);
    }
    const rdofradio_dev_setup_t su = {rate, R.rx.center_hz, tx_lo, gain, bw, stream_args, R.split || R.mmdvm,
                                      FX.on ? 48000.0 * (double)rdof_chzfx_frame_words(FX.C) : 0, R.txr, settings,
                                      n_settings};
#ifdef RDOF_IIO
    if (iio)
        dev = rdofradio_iio_open(iio, &su, derr, sizeof derr);
#endif
#ifdef RDOF_SOAPY
    if (soapy)
        dev = rdofradio_soapy_open(soapy, &su, derr, sizeof derr);
#endif
    if (sim_sdr)
        dev = rdofradio_sim_open(&su, derr, sizeof derr);
    if (!dev) {
        fprintf(stderr, "%s\n", derr);
        return 1;
    }
    WS.agc = isnan(gain);
    rx_gain_set = gain;
    WS.tx_gain = dev->tx_gain_min;
    if (isnan(tx_max_dbm))
        tx_max_dbm = 7.0; /* a PlutoSDR's nominal at full gain */
    if (txg_given && !isnan(tx_dbm)) {
        fprintf(stderr, "--tx-power or --tx-gain, not both\n");
        dev->close(dev);
        return 2;
    }
    if (!txg_given && isnan(tx_dbm) && config_path && !isnan(rcfg.tx_power_dbm))
        tx_dbm = rcfg.tx_power_dbm;
    if (!isnan(tx_dbm)) {
        txg = dev->tx_gain_max - (tx_max_dbm - tx_dbm);
        if (txg < dev->tx_gain_min || txg > dev->tx_gain_max) {
            const double g = txg < dev->tx_gain_min ? dev->tx_gain_min : dev->tx_gain_max;
            fprintf(stderr, "transmit power %.1f dBm: past the device's %.1f..%.1f dBm, %.1f dBm\n", tx_dbm,
                    gain_dbm(dev->tx_gain_min), tx_max_dbm, gain_dbm(g));
            txg = g;
        }
    }
    txg_full = txg;
    bench_map = rcfg.cfg.tx_map;
    if (!R.split && !R.mmdvm && (txg < dev->tx_gain_min || txg > dev->tx_gain_max)) {
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
    const bool rx_only = R.split || R.mmdvm;
    const rdof_sdrmodem_io_t io = {io_reply, io_channel, rx_only ? NULL : io_tune, &R, R.split ? split_now48 : NULL,
                                   rx_only ? NULL : io_tx_level};
    /* the nominal rate when the device's is within 2 S/s of it, as the receiver takes it (below 2.083 MS/s an
       AD9361 reports 1295999 for 1296000, which the modulator can't phase) */
    R.tx_rate = fabs(got - rate) <= 2.0 ? dev->tx_rate * rate / got : dev->tx_rate;
    if (rdof_sdrmodem_init(&R.m, &io, (uint32_t)rate, (uint32_t)llround(R.tx_rate), !rx_only)) {
        fprintf(stderr, "modem: %s\n", R.m.error);
        return 1;
    }
    rdof_sdrmodem_carrier_ppm(&R.m, xtal());
    if (isfinite(xtal_ppm))
        fprintf(stderr, "crystal: %+.3f ppm (--ppm%s), taken out in software: %+.1f Hz at %.5f MHz\n", xtal_ppm,
                xtal_tol > 0 ? ", --ppm-tol given" : "; no --ppm-tol: its tolerance is reported as not known",
                -R.rx.center_hz * xtal_ppm * 1e-6, R.rx.center_hz / 1e6);
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
    if (R.mmdvm)
        return mmdvm_main(argc, argv, mmdvm, baud, seconds, mm_rx_off, mm_tx_off, mm_rx_level, mm_tx_level);
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
        R.r.keep_up = R.mmdvm; /* a modem drops its carrier when keyed with nothing queued */
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
    fprintf(stderr, "%s: %s, RX %.4f MHz (center %.4f), TX %.4f MHz (LO %.4f), cc %d, %.0f S/s, TX %.1f dBm at the "
                    "antenna (nominal: %.1f at full gain), gain %.1f (%.1f..%.1f), RX gain %s, analog bandwidth "
                    "%.2f MHz\n",
            R.full ? "repeater" : "transceiver", R.client.version, rx / 1e6, R.rx.center_hz / 1e6, tx / 1e6,
            tx_lo / 1e6, cc, got, gain_dbm(txg), tx_max_dbm, txg, dev->tx_gain_min, dev->tx_gain_max,
            isnan(gain) ? "auto" : "set",
            dev->bandwidth(dev) / 1e6);
    fprintf(stderr, "device: %s; FFT: %s\n", dev->name,
#ifdef RDOF_PFFFT
            fft && !strcmp(fft, "builtin") ? "built-in" : "pffft"
#else
            "built-in"
#endif
    );

    static rdof_cf_t in[CHUNK], out[CHUNK];
    /* the transmit stream's positions are its own samples (txr receive samples each with --fpga-duc) */
    const double txrate = R.tx_rate;
    const int txr = R.txr;
    const int lead = (int)(txrate * TX_LEAD_S);
    const int64_t offset = (int64_t)llround(offset_s * txrate);
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
            fprintf(stderr, "web: http://%s:%d/\n", web_addr, port), advertise_web(port);
    }
    double last_web = 0;
#endif
    bench_start();
    /* The receive loop ahead of the rest: it paces the transmitter, and a busy moment elsewhere (three ssh logins at
     * once on a PlutoSDR) left it behind, half a second of the transmit guard's zeros on the air at a time. Real-time
     * under the writer (the receiver's helper, made by this thread, takes it too); else, on Linux, nice -10 (the
     * thread's own there); elsewhere as it is. The kernel keeps 5% of each second for the rest (sched_rt_runtime_us). */
    {
        const struct sched_param sp = {.sched_priority = 5};
        const int e = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
#ifdef __linux__
        if (e && setpriority(PRIO_PROCESS, 0, -10))
            say("the receive loop: no higher priority (%s)\n", strerror(e));
#else
        (void)e; /* elsewhere as it is */
#endif
    }
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
        else {
            R.cal = 1, R.cal_at = (long long)(rate * 0.3);
            fx_cal(true); /* --fpga-chz: channel 0 on the pulse */
        }
        tx_gain(txg); /* now: the gain step's LO leakage is in the baseline before the pulse, not taken for it */
    }
    double last_status = 0, level_sum = 0;
    long level_n = 0;
    double t_read = 0, t_write = 0, t_proc = 0; /* seconds spent in each, since the last status */
    double raw_lag = 0;                    /* the samples the clock says its silence fell behind */
    double raw_end = -INFINITY;            /* the stream's seconds when a raw capture's silence ended */
    long long raw_quiet = 0, raw_seen = 0; /* channel frames a raw capture took, so far; fx_raw_made at the last turn */
    while (!stop && (seconds <= 0 || total < (long long)(seconds * rate))) {
        bench_tune_poll();
        const double t0 = now_s();
        const long long fx_lost = FX.lost;
        /* --fpga-chz: in stays zeros; the turn's power is the block's (turn_p2) */
        FX.t_rx = 0;
        int n = FX.on ? fx_take(chunk, 500000) : dev->read(dev, in, chunk, 500000);
        const long long raw_k = fx_raw_made - raw_seen; /* a raw capture's: one line at its end */
        raw_seen = fx_raw_made, raw_quiet += raw_k;
        if (raw_quiet && !atomic_load(&RAW.fill)) {
            say("receive: %.2f s not heard for a raw capture (%lld channel frames, %.1f ms of them late, made up)%s\n",
                ((double)raw_quiet * FX.step + raw_lag) / rate, raw_quiet, 1e3 * raw_lag / rate,
                R.full ? "; calibrating again once idle" : "");
            raw_quiet = 0, raw_lag = 0;
            raw_end = (double)total / rate;
            if (R.full)
                R.recal = true, R.cal_tries = 0;
        }
        if (FX.lost - fx_lost > raw_k) { /* frames lost: zeros kept the timeline, but the calibration no longer holds */
            say("receive: %lld channel frames lost; zeros in their place%s\n", FX.lost - fx_lost - raw_k,
                R.full ? "; calibrating again once idle" : "");
            if (R.full)
                R.recal = true, R.cal_tries = 0;
        }
        const double dt_read = now_s() - t0 - FX.t_rx; /* --fpga-chz: less the receiver's turn */
        t_read += dt_read;
        t_proc += FX.t_rx, WS.rx += FX.t_rx;
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
        bench_ob_fast();
        const double now = now_s();
        WS.last_samples = now;
        WS.n += n;
        const double t_poll = now;
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
        /* as many transmit samples as were read (one for every txr), at the modem's position for them */
        const int ntx = (int)((total + n) / txr - total / txr);
        const double t_mod = now_s();
        ms_poll(now);
        const bool keyed = rdof_sdrmodem_pull_iq(&R.m, txn - lead + offset, out, ntx);
        WS.mod += now_s() - t_mod;
        const double d_poll = t_mod - t_poll, d_mod = now_s() - t_mod;
        /* Samples the receiver lost (a stall longer than the device's buffers: the stream falls behind the wall
         * clock) move what's heard against what's sent, and the transmitter's samples are paced by what's read: the
         * calibration no longer holds. Its floor over half a second against the last one's: a step is a loss. Half
         * duplex says so too (the bench's timing and a repeater job's slots move with it). A window the loop spent
         * behind (no read waited: the device's buffers held the samples) shows a lag that isn't a loss; it runs on
         * until a read waits. The transmit guard's zeros stood in for the lost part, which the loop never makes:
         * rdof_txguard_slipped, or every block after goes out as zeros. */
        {
            const double lag = (now - t_start) * rate - (double)total;
            if (lag < R.lag_min)
                R.lag_min = lag;
            R.lag_fresh |= dt_read >= FRESH_S;
            if ((double)total / rate - R.lag_win >= 0.5 && R.lag_fresh) {
                if (!isnan(R.lag_prev) && R.lag_min - R.lag_prev > rate / 500) {
                    WS.lost += llround(R.lag_min - R.lag_prev);
                    /* A raw capture's: its silence missed a few ms at its ends (2.2-2.4 ms off the rate). Made up
                     * as more silence (fx_owe), the stream's count is back on the clock, and with the transmitter
                     * back on its own timeline what's heard against what's sent is as it was: the calibration
                     * holds. Else the guard's zeros stood for the loss: the stream is that much behind for good,
                     * not samples to drop. */
                    const bool by_raw = FX.on && (raw_quiet || atomic_load(&RAW.fill) ||
                                                  (double)total / rate - raw_end < 1.5);
                    if (by_raw)
                        fx_owe += llround((R.lag_min - R.lag_prev) / FX.step);
                    else if (W.guarded) {
                        pthread_mutex_lock(&W.mu);
                        rdof_txguard_slipped(&W.guard, llround((R.lag_min - R.lag_prev) / txr));
                        pthread_mutex_unlock(&W.mu);
                    }
                    if (by_raw && (raw_quiet || atomic_load(&RAW.fill))) /* said at the capture's end */
                        raw_lag += R.lag_min - R.lag_prev;
                    else if (by_raw)
                        say("receive: a raw capture's silence came %.1f ms short; made up\n",
                            1e3 * (R.lag_min - R.lag_prev) / rate);
                    else
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
                            1e3 * (double)(z - padded) / txrate);
                    if (d != dry)
                        say("transmit: the device ran dry %u times; what follows goes out later\n", d - dry);
                    pads = p, dry = d, padded = z;
                }
            }
        }
        if (R.full) {
            if (R.recal && R.cal == 0 && !hot && !keyed && !R.m.in_call[0] && !R.m.in_call[1] && !atomic_load(&RAW.fill)) {
                if (dev->set_rx_freq(dev, tx_lo) == 0) {
                    R.recal = false;
                    R.cal = 1, R.cal_at = total + (long long)(rate * 0.3);
                    fx_cal(true);
                    tx_gain(txg);
                }
            }
        }
        if (R.cal == 1 && total >= R.cal_at && !keyed) {
            const int len = (int)(txrate * 0.02) < ntx ? (int)(txrate * 0.02) : ntx;
            for (int i = 0; i < len; i++) { /* lo_off below the TX LO, where the outbound goes */
                const double ph = -2 * M_PI * R.lo_off * (double)i / txrate;
                out[i].re = (float)(0.7 * cos(ph)), out[i].im = (float)(0.7 * sin(ph));
            }
            hot = true;
            keyed_until = (double)(txn + ntx) / txrate + 0.3;
            R.onset = (txn - lead + offset) * txr;
            R.cal = 2;
        }
        if (keyed) {
            keyed_until = (double)(txn + ntx) / txrate + 0.3; /* past the device's buffers */
            if (!hot) {
                tx_gain(keyed_gain());
                hot = true;
                /* where the receiver should report the key-up's first burst: its first keyed sample, then the ramp
                 * and the CACH (4 + 12 symbols), then the receiver's delay (48 at 48 kS/s; reference.md, "Timing") */
                int i0 = 0;
                while (i0 < ntx && out[i0].re == 0.0f && out[i0].im == 0.0f)
                    i0++;
                R.first_sent = (int64_t)((double)(txn - lead + offset + i0) * 48000.0 / txrate) + 16 * 10 + 48;
                R.waiting = R.mon >= 0;
                R.onset = (txn - lead + offset + i0) * txr;
                R.echo_ch = R.tx_ch, R.echo_img = R.tx_img;
                R.echo_from = (int64_t)((double)(txn - lead + offset + i0) * 48000.0 / txrate);
            }
            R.echo_to = (int64_t)((double)(txn - lead + offset + ntx) * 48000.0 / txrate);
        } else if (hot && (double)txn / txrate > keyed_until) {
            tx_gain(dev->tx_gain_min);
            hot = false;
        }
        for (int i = 0; i < ntx; i++)
            out[i].re *= 0.7f, out[i].im *= 0.7f;
        const double t1 = now_s();
        tx_put(out, ntx);
        const double d_write = now_s() - t1;
        t_write += now_s() - t1;
        WS.wait += now_s() - t1;
        txn += ntx;
        /* The transmit path's delay drifts: a stall in the writer lets the device run dry, and all that follows goes out
         * that much later. With the device's buffers full, the ring holds that much more: its floor over half a second,
         * against the floor just after the calibration, is the drift. Unkeyed, the ring holds zeros only, and the
         * excess is taken out (the newest samples, past what the writer may be reading); keyed, the modem follows it. */
        if (R.full) {
            pthread_mutex_lock(&W.mu);
            int64_t fill = W.head - W.tail;
            if (fill < R.ring_min)
                R.ring_min = fill;
            if (atomic_exchange(&late_rebase, 0))
                R.ring_ref = -1; /* a job's own delay: the reference taken again against it */
            if ((double)total / rate - R.ring_win >= 0.5) {
                if (R.cal == 0 && R.ring_ref < 0) {
                    R.ring_ref = R.ring_min;
                    R.cal48 = R.m.tx_late48;
                } else if (R.ring_ref >= 0) {
                    int64_t drift = R.ring_min - R.ring_ref;
                    if (!hot && drift > (int64_t)txrate / 500) {
                        const int64_t cut = drift < fill - CHUNK ? drift : fill - CHUNK;
                        if (cut > 0) {
                            W.head -= cut;
                            drift -= cut;
                            say("transmit: %.1f ms of delay taken out (the writer had fallen behind)\n", 1e3 * (double)cut / txrate);
                        }
                    }
                    R.m.tx_late48 = R.cal48 + drift * 48000 / (int64_t)txrate;
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
            /* --fpga-chz: one output instant at a time (its power is the same over its step; a division a wideband
             * sample took a 7010's loop to a third of real time until the edge came) */
            for (int i = 0; i < n; i += FX.on ? FX.step : 1) {
                const double p2 = turn_p2(in, i);
                const int64_t at = total + i - FX_EDGE; /* --fpga-chz: less the filter's delay */
                if (at >= edge_from && p2 > 100 * R.base_power) {
                    if (R.cal == 2) {
                        /* the modem's inbound slots follow it: a handheld keeps to what's on the air */
                        R.m.tx_late48 = path48 = (at - R.onset) * 48000 / (int64_t)rate;
                        say("calibration: the transmit path is %.2f ms past its place\n",
                                1e3 * (double)(at - R.onset) / rate);
                        dev->set_rx_freq(dev, R.rx.center_hz);
                        fx_cal(false);
                        R.cal = 0, R.cal_tries = 0;
                        R.ring_ref = -1; /* the ring's floor taken again against this */
                    } else if (BR.active && BR.cal) {
                        /* a repeater job's first key-up, heard in the receiver's band: its inbound slots follow it */
                        R.m.tx_late48 = path48 = (at - R.onset) * 48000 / (int64_t)rate;
                        BR.cal = false;
                        atomic_store(&late_rebase, 1);
                        say("bench: the job's transmit path is %.2f ms past its place\n",
                            1e3 * (double)(at - R.onset) / rate);
                    } else if (BO.active && !BO.mapped) {
                        /* an outbound's key-up: its modem samples on the receiver's timeline */
                        const double t = bl_sc_utc(NULL, (uint64_t)at);
                        pthread_mutex_lock(&BO.mu);
                        BO.mapped = true, BO.map_pos = R.onset / R.txr, BO.map_t = t;
                        pthread_mutex_unlock(&BO.mu);
                        say("bench: the outbound's key-up heard back %.2f ms after its place\n",
                            1e3 * (double)(at - R.onset) / rate);
                    } else {
                        /* at a split wider than the receiver's band, this is the first inbound burst, not the TX */
                        say("loop delay: the key-up reached the receiver %.2f ms after its place\n",
                                1e3 * (double)(at - R.onset) / rate);
                    }
                    R.onset = -1;
                    break;
                }
            }
            if (R.onset >= 0 && total - R.onset > (long long)rate) {
                R.onset = -1; /* no edge within a second: too little leaks in to see it */
                if (R.cal == 2) {
                    dev->set_rx_freq(dev, R.rx.center_hz);
                    fx_cal(false);
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
                p2 += turn_p2(in, i);
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
                level_sum += turn_p2(in, i);
            level_n += (n + 15) / 16;
            if (now - last_status >= 1.0) {
                fprintf(stderr, "status %.1f s (wall %.1f; read %.2f, ring wait %.2f, process %.2f s; ring %.0f ms): RX gain %.0f dB, level %.1f dBFS, %s, modem sent %u idle %u heard %u\n",
                        total / rate, now - t_start, t_read, t_write, t_proc, 1e3 * (double)(W.head - W.tail) / txrate, dev->rx_gain(dev),
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
        if (!FX.on)
            web_spectrum(in, n);
#endif
        if (FX.on)
            bench_chan_feed(n / FX.step);
        bench_feed(in, n, dt_read >= FRESH_S, FX.on); /* --fpga-chz: filler, the receiver had its turn in the read */
        const double t3 = now_s();
        if (!FX.on)
            rx_chunk(in, n);
        const double t4 = now_s();
        t_proc += t4 - t2;
        WS.spec += t3 - t2, WS.rx += t4 - t3;
        ws_turn(t4);
        if (verbose && t4 - t0 > 0.04) /* a turn this long eats into the transmit stream's 50 ms lead */
            say("slow turn %.0f ms: read %.0f, receiver %.0f, poll %.0f, modem %.0f, write %.0f\n", 1e3 * (t4 - t0),
                1e3 * dt_read, 1e3 * FX.t_rx, 1e3 * d_poll, 1e3 * d_mod, 1e3 * d_write);
        total += n;
    }
    if (R.full)
        rdof_repeater_stop(&R.r);
    shutdown_tx();
    fx_stop();
    dev->close(dev);
    if (FX.on)
        fprintf(stderr, "--fpga-chz: %lld channel frames lost (%lld made up with zeros), %ld overflows (STATUS)\n",
                FX.lost, FX.made_up, FX.ovf);

    fprintf(stderr, "%.1f s (%.1f s wall), %d overflows, %d TX write errors; modem: %u bursts sent, %u idle, %u heard, "
                    "%u muted, %u lost, %u refused\n",
            total / rate, now_s() - t_start, overflows, W.errors, R.m.tx_bursts, R.m.idle_bursts, R.m.rx_bursts,
            R.m.muted, R.m.lost, R.m.refused);
    if (W.guarded && (W.guard.pads || W.guard.dry))
        fprintf(stderr, "transmit guard: %u blocks finished with zeros (%.1f ms), ran dry %u times\n", W.guard.pads,
                1e3 * (double)W.guard.padded / txrate, W.guard.dry);
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
    return lost_device ? 1 : 0;
}
