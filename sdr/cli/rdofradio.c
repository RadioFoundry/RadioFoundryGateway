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
 * The clock (the settings' ntp): netclock, the board's NTP disciplined clock (components/netclock, its POSIX glue),
 * or the OS's (ntp.clock "os": chrony, ntpd). --clock-state FILE keeps netclock's learned rate across restarts;
 * --set-os-clock steps the OS clock to it (root; for a box with nothing else setting the time, a PlutoSDR).
 *
 *   rdofradio DEVICE --allow-tx --rx MHZ [--tx MHZ] [--duplex full|half] [--cc CC] [-r RATE] [--bw MHZ]
 *             [--tx-gain DB] [--tx-lo MHZ] [--gain DB] [--center MHZ] [--monitor] [--tx-offset S]
 *             [--parrot ID] [--stream ARGS] [--seconds S] [--bursts] [--quiet] [-v] [--web [ADDR:]PORT]
 *             [--fft builtin|pffft]
 *   rdofradio DEVICE --allow-tx --config FILE [--clock-state FILE] [--set-os-clock] [the device's flags as above]
 *   rdofradio DEVICE --allow-tx --split-tx PORT [--baud N] [--split-power-cap N] --rx MHZ --tx MHZ [--cc CC] [...]
 *
 * DEVICE is --soapy ARGS (SoapySDR; -r defaults to 2.4 MS/s) or --iio URI (an AD9361 through libiio: "local:" on a
 * PlutoSDR, "ip:192.168.2.1" from a host; -r defaults to 768 kS/s, which the chip's own FIR makes with
 * libad9361). --web serves the page on 127.0.0.1 unless ADDR is given (0.0.0.0: every interface). --fft: the
 * channelizer's FFT, pffft when it's built in (RDOF_PFFFT).
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
#include <unistd.h>

#include "rdof/hbpeer.h"
#include "rdof/layer2.h"
#include "rdof/modem.h"
#include "rdof/parrot.h"
#include "rdof/repeater.h"
#include "rdof/split.h"
#include "rdof/sdr/receiver.h"
#include "rdof/sdr/sdrmodem.h"
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
#ifndef RDOF_VERSION
#define RDOF_VERSION "dev"
#endif

#define CHUNK 24000        /* receive samples a turn: 10 ms at 2.4 MS/s */
#define POLL_S 0.02        /* the repeater's poll */
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
    int64_t cal48;         /* the calibrated tx_late48, with the ring at ring_ref samples (-1: not yet taken) */
    int64_t ring_ref, ring_min;
    double ring_win;       /* stream time the ring's current window started */
    double base_power;     /* the receive power before it */
    double delay_sum;
    int delay_n;
    double delay_min, delay_max;
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

/* --split-tx (below) */
static rdof_split_t S;
static int in_wait; /* inside the client's wait for a reply: what's heard waits for it to end */
static void split_turn(void);
static void split_flush(void);
static int64_t split_now48(void *ctx);

static void on_signal(int s) { stop = 1; }

/* --config: the file, and the radio it (or the command line) sets */
static const char *config_path;
static rdofradio_cfg_t rcfg;

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

static struct {
    rdof_cf_t buf[RING];
    int64_t head, tail;    /* samples put in, samples written to the device */
    bool quit;
    int errors;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t thread;
    bool running;
} W = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};

static void *writer(void *arg)
{
    pthread_mutex_lock(&W.mu);
    for (;;) {
        while (W.head == W.tail && !W.quit)
            pthread_cond_wait(&W.cv, &W.mu);
        if (W.quit)
            break;
        const int at = (int)(W.tail % RING);
        int n = (int)(W.head - W.tail);
        if (n > RING - at)
            n = RING - at;
        if (n > CHUNK)
            n = CHUNK;
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
            "                 [--fft builtin|pffft]\n"
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

static void io_reply(void *ctx, const uint8_t *f, size_t n)
{
    if (R.split)
        rdof_split_from_half(&S, RDOF_SPLIT_RX, f, n, now_s()); /* the RX half's reply; split_flush takes it on */
    else
        rdof_modem_feed(&R.client, f, n);
}
static int io_channel(void *ctx, uint32_t hz) { return hz == R.rx_hz ? 0 : -1; }
static bool io_tune(void *ctx, uint32_t hz, double *off)
{
    *off = (double)hz - R.tx_lo; /* the carrier from the TX LO */
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
    if (R.full)
        rdof_repeater_heard(&R.r, slot, control, burst, rssi);
    else
        rdof_txvr_heard(&R.t, slot, control, burst, rssi);
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
    if (R.full)
        rdof_repeater_lost(&R.r, slot);
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
}

static void on_tx(void *ctx, const char *action, const rdof_tx_call_info_t *c, double d)
{
    say("repeater: %s slot %d %u -> %s%u (%.2f s)\n", action, c->slot, c->src, c->group ? "TG " : "",
            c->dst, d);
}
static void on_key(void *ctx, bool keyed)
{
    R.keyed = keyed;
    say("repeater: %s\n", keyed ? "keyed" : "unkeyed");
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
static void txvr_send(void *ctx, const uint8_t *d, size_t n)
{
    static const char *status[] = {"on air", "channel busy", "not allowed", "radio error", "transceiver busy"};
    net_send(d, n);
    if (n >= 13 && !memcmp(d, "DMRK", 4))
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
    if (rdof_hb_dispatch(d, n, R.full ? &R.r : NULL, R.full ? NULL : &R.t, rcfg.peer_id, k))
        net_send(k, sizeof k);
}
static void net_config(void)
{
    uint8_t pkt[RDOF_DMRC_MAX];
    rdofnet_peer_set_config(&NET, pkt, rdof_hb_role_dmrc(&rcfg.cfg, R.rx_hz, R.cc, pkt), now_s());
}
static void sink_send(void *ctx, const uint8_t *d, size_t n) { net_send(d, n); }
static uint32_t sink_rand(void *ctx) { return (uint32_t)random() ^ ((uint32_t)random() << 16); }
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
    srandom((unsigned)time(NULL) ^ (unsigned)getpid());
    rdof_hb_sink_init(&NET_SINK, rcfg.peer_id);
    NET_SINK.send = sink_send;
    NET_SINK.rand32 = sink_rand;
    if (!R.full)
        NET_SINK.before_stream = sink_before;
    const rdof_sink_t sk = rdof_hb_sink_as_sink(&NET_SINK);
    rdof_sdr_rx_add_sink(&R.rx, 0, &sk);
    net_on = true;
    net_config();
    fprintf(stderr, "hblink: %s, peer %u, to %s:%d\n", rcfg.name, (unsigned)rcfg.peer_id, rcfg.cfg.master_host,
            rcfg.cfg.master_port);
}

static void net_poll(double now)
{
    if (net_on)
        rdofnet_peer_poll(&NET, now);
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

/* ── the receiver's output ─────────────────────────────────────────────────── */

static void on_json(void *ctx, const char *ch, const char *json)
{
#ifdef RDOF_HOST_WEB
    rdof_hostweb_tracker_json(R.full ? "repeater" : "roamer", rcfg.name, NULL, json);
#endif
    if (!quiet)
        printf("%s json %s\n", ch, json);
}

static void on_burst(void *ctx, int c, const rdof_sdr_burst_t *b, double t)
{
    rdof_sdrmodem_heard(&R.m, c, b);
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
static int relabel(void *ctx, const rdof_sdr_burst_t *b) { return rdof_sdrmodem_inbound(&R.m, b); }

static void tx_gain(double db)
{
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
    w.hblink = net_on && NET.peer.up;
    w.hblink_tx = NET.peer.tx, w.hblink_rx = NET.peer.rx;
    w.nextra = R.split ? 0 : 2; /* the TX half's timing is the radio's own */
    snprintf(w.extra[0].name, sizeof w.extra[0].name, "tx late (us)");
    w.extra[0].value = R.m.tx_late48 * 1000 / 48;
    snprintf(w.extra[1].name, sizeof w.extra[1].name, "inbound off (%%)");
    w.extra[1].value = (int64_t)llround(R.m.inbound_off * 100);
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
        rdofradio_cfg_from_flags(t, 1024, rcfg.full, rcfg.rx_hz, rcfg.tx_hz, rcfg.cc, rcfg.has_parrot, rcfg.parrot,
                                 rcfg.peer_id);
    return t;
}
static bool web_save(void *ctx, const char *text, size_t len, char *err, size_t cap)
{
    rdofradio_cfg_t *c = malloc(sizeof *c);
    if (!c) {
        snprintf(err, cap, "out of memory");
        return false;
    }
    const bool ok = rdofradio_cfg_parse(text, len, c, err, cap) &&
                    rdof_hostweb_save_file(config_path, text, len, err, cap);
    free(c);
    return ok;
}
static size_t web_resolved(void *ctx, char *out, size_t cap)
{
    char src[300];
    if (config_path)
        snprintf(src, sizeof src, "file %s", config_path);
    else
        snprintf(src, sizeof src, "command line flags");
    return rdofradio_cfg_describe(&rcfg, src, web_device, out, cap);
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

/* The radio's port: a thread reads it and stamps each read with the time it came, so the split's TIME round trips
 * don't include the main loop's wait on the receive stream. */
#define PORT_Q 64
static struct {
    int fd;
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

/* The receive stream against the wall clock: lag = wall-clock samples - samples read. Its least over half a second
 * is the freshest read; floor follows it (a crystal's ppm moves it slowly). A floor that stays over by more than
 * 2 ms for a second is samples the device dropped: zeros go to the receiver in their place. */
static struct {
    double t0, rate, floor, win_start, win_min, last_win;
    long long total, filled;
    int chunk, timeouts, errors, overflows;
} SC;

static void clock_read(double t)
{
    const double lag = (t - SC.t0) * SC.rate - (double)SC.total;
    if (lag < SC.win_min)
        SC.win_min = lag;
    if (t - SC.win_start < 0.5)
        return;
    const double w = SC.win_min, thr = SC.rate / 500;
    SC.win_start = t, SC.win_min = INFINITY;
    if (!isfinite(SC.floor) || w < SC.floor + thr) {
        SC.floor = w;
    } else if (SC.last_win >= SC.floor + thr) {
        static rdof_cf_t zeros[CHUNK];
        long long lost = llround(fmin(w, SC.last_win) - SC.floor);
        say("receive: %.1f ms of samples lost; zeros in their place\n", 1e3 * (double)lost / SC.rate);
        SC.filled += lost;
        SC.total += lost;
        for (; lost > 0; lost -= CHUNK)
            rdof_sdr_rx_process(&R.rx, zeros, lost < CHUNK ? (int)lost : CHUNK);
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
    const int n = dev->read(dev, in, SC.chunk, 100000);
    const double t = now_s();
    if (n == RDOF_DEV_TIMEOUT) {
        if (++SC.timeouts >= 50 && !stop) {
            say("read: nothing for 5 s, stopping\n");
            stop = 1;
        }
    } else if (n == RDOF_DEV_OVERFLOW) {
        SC.overflows++;
    } else if (n < 0) {
        if (++SC.errors > 10 && !stop) {
            say("%s\n", dev->error);
            stop = 1;
        }
    } else if (n > 0) {
        SC.timeouts = 0;
        SC.total += n;
        clock_read(t);
        rdof_sdr_rx_process(&R.rx, in, n);
    }
    split_service();
}

/* The split's whole run, once the device is open (receive only) and the SDR modem made. */
static int split_main(int argc, char **argv, const char *port, int baud, int power_cap, double seconds)
{
    char err[256];
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
    if (net_on)
        rdofnet_peer_close(&NET);
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

int main(int argc, char **argv)
{
    const char *soapy = NULL, *iio = NULL, *stream_args = "", *fft = NULL, *split_tx = NULL;
    int baud = 115200, power_cap = 0, tx_flags = 0;
    double bw = 0, rate = 0, rx = 0, tx = 0, tx_lo = 0, center = 0, gain = NAN, txg = 0, seconds = 0, offset_s = 0;
    int cc = 1, allow = 0, monitor = 0, verbose = 0;
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
#ifdef RDOF_HOST_WEB
        } else if (!strcmp(a, "--web")) {
            NEXT();
            const char *colon = strrchr(v, ':');
            if (colon) {
                snprintf(web_addr, sizeof web_addr, "%.*s", (int)(colon - v), v);
                v = colon + 1;
            }
            web_port = atoi(v);
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
            fprintf(stderr, "%s: %s\n", config_path, err);
            return 2;
        }
        if (rcfg.unused[0])
            fprintf(stderr, "%s: not used here: %s\n", config_path, rcfg.unused);
        rx = rcfg.rx_hz, tx = rcfg.tx_hz, cc = rcfg.cc;
        duplex = rcfg.full ? "full" : "half";
        if (rcfg.has_parrot) {
            static rdof_parrot_t parrot;
            rdof_parrot_init(&parrot, rcfg.parrot);
            parrot.start = parrot_start;
            parrot.send = parrot_send;
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
    if (!rate)
        rate = iio ? 768000 : 2400000;
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
        rdofradio_cfg_from_flags(doc, sizeof doc, R.full, R.rx_hz, R.tx_hz, cc, R.parrot != NULL,
                                 R.parrot ? R.parrot->id : 0, 9990601);
        if (!rdofradio_cfg_parse(doc, strlen(doc), &rcfg, err, sizeof err)) {
            fprintf(stderr, "the command line's radio: %s\n", err);
            return 2;
        }
    }
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* the receiver: the RX channel, and with --monitor the TX channel (our own outbound, heard back) */
    const rdof_sdr_rx_config_t rc = {rate, center, -55.0, 0.0, true, CHUNK};
    rdof_sdr_channel_t ch[2] = {{"rx", rx, cc, 1}, {"out", tx, cc, 1}};
    const int nch = monitor && R.tx_hz != R.rx_hz ? 2 : 1;
    rdof_sink_t sink = {.on_json = on_json};
    if (rdof_sdr_rx_init(&R.rx, &rc, ch, nch, &sink, (double)time(NULL))) {
        fprintf(stderr, "receiver: the rate isn't a multiple of 48 kS/s, or the channels don't fit one window\n");
        return 1;
    }
    if (nch == 2 || (monitor && R.tx_hz == R.rx_hz))
        R.mon = nch - 1;
    R.rx.on_burst = on_burst;
    R.rx.muted = muted;
    R.rx.relabel = relabel; /* the tracker's JSON (calls, BER, RSSI) with the inbound slot, not slot 1 */
    R.rx.burst_ctx = &R;
    net_start();
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
        R.r.on_key = on_key;
        R.r.ctx = &R;
        ok = rdof_repeater_start(&R.r);
    } else {
        rdof_txvr_init(&R.t, &R.radio, rcfg.peer_id);
        R.t.channels[0] = (rdof_channel_t){R.rx_hz, cc, 255};
        R.t.nchannels = 1;
        R.t.has_idle = true;
        R.t.idle = R.t.channels[0];
        if (R.tx_hz != R.rx_hz)
            R.t.channels[R.t.nchannels++] = (rdof_channel_t){R.tx_hz, cc, 255};
        R.t.clock = p_clock;
        R.t.send = txvr_send;
        R.t.on_tx = on_tx;
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
        t_read += now_s() - t0;
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
            continue;
        }
        if (n < 0) {
            if (++rx_errors > 10) {
                fprintf(stderr, "%s\n", dev->error);
                break;
            }
            continue;
        }
        const double now = now_s();
        if (now - last_poll >= POLL_S) {
            last_poll = now;
            if (R.parrot)
                rdof_parrot_poll(R.parrot, now);
            if (!(R.full ? rdof_repeater_poll(&R.r) : rdof_txvr_poll(&R.t)))
                fprintf(stderr, "poll: %s\n", R.client.error);
            net_poll(now);
        }
        /* as many transmit samples as were read, at the modem's position for them */
        const bool keyed = rdof_sdrmodem_pull_iq(&R.m, txn - lead + offset, out, n);
        /* Samples the receiver lost (a stall longer than the device's buffers: the stream falls behind the wall
         * clock) move what's heard against what's sent, and the transmitter's samples are paced by what's read: the
         * calibration no longer holds. Its floor over half a second against the last one's: a step is a loss. */
        if (R.full) {
            const double lag = (now - t_start) * rate - (double)total;
            if (lag < R.lag_min)
                R.lag_min = lag;
            if ((double)total / rate - R.lag_win >= 0.5) {
                if (!isnan(R.lag_prev) && R.lag_min - R.lag_prev > rate / 500) {
                    say("receive: %.1f ms of samples lost; calibrating again once idle\n", 1e3 * (R.lag_min - R.lag_prev) / rate);
                    R.recal = true, R.cal_tries = 0;
                }
                R.lag_prev = R.lag_min, R.lag_min = INFINITY;
                R.lag_win = (double)total / rate;
            }
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
            }
        } else if (hot && (double)txn / rate > keyed_until) {
            tx_gain(dev->tx_gain_min);
            hot = false;
        }
        for (int i = 0; i < n; i++)
            out[i].re *= 0.7f, out[i].im *= 0.7f;
        const double t1 = now_s();
        tx_put(out, n);
        t_write += now_s() - t1;
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
        rdof_sdr_rx_process(&R.rx, in, n);
        t_proc += now_s() - t2;
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
    for (int c = 0; c < nch; c++)
        fprintf(stderr, "%s %.4f MHz: %ld bursts, AFC %.1f Hz\n", ch[c].id, ch[c].freq_hz / 1e6, R.rx.bursts[c],
                R.rx.chz.correction_hz[c]);
    if (R.delay_n)
        fprintf(stderr, "monitor: a key-up heard %.2f ms after the modem's position (%.2f..%.2f, %d key-ups); "
                        "--tx-offset %.4f lines them up\n",
                1e3 * R.delay_sum / R.delay_n, 1e3 * R.delay_min, 1e3 * R.delay_max, R.delay_n,
                offset_s + R.delay_sum / R.delay_n);
    if (R.parrot)
        say("parrot: %u sent back, %u not to it, %u bursts cut\n", R.parrot->replayed, R.parrot->ignored,
                R.parrot->cut);
    if (net_on)
        rdofnet_peer_close(&NET);
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
