// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The bench's line protocol (rdof/benchline.h, rdof/benchnet.h) against a fake receiver: the ops through the engine
 * directly, then a session, a claim and a capture over a real TCP connection. */
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "rdof/benchjob.h"
#include "rdof/benchline.h"
#include "rdof/benchnet.h"
#include "rdof/jsonp.h"
#include "rdof/layer2.h"
#include "rdof/mmdvm.h"
#include "rdof/pdu.h"

static int checks, failures;

#define CHECK(cond, ...)                                                                                     \
    do {                                                                                                     \
        checks++;                                                                                            \
        if (!(cond)) {                                                                                       \
            failures++;                                                                                      \
            printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                           \
            printf(__VA_ARGS__);                                                                             \
            printf("\n");                                                                                    \
        }                                                                                                    \
    } while (0)

/* ── the fake receiver: 4 channels in a 768 kS/s window, a TX frequency, a history ring ── */

static rdof_bl_ring_t ring;
static bool can_tx = true;

static void f_device(void *ctx, char *k, size_t kc, char *n, size_t nc, char *v, size_t vc)
{
    snprintf(k, kc, "test");
    snprintf(n, nc, "bench \"one\"");
    snprintf(v, vc, "1.0");
}
/* the fake receiver's tune (test_tune): a center (NAN: the settings', 438.225 MHz) and its taps, the only channels
 * then */
static struct {
    double center, gain, taps[RDOF_BL_TAPS];
    int ntaps;
    rdof_bl_tune_t asked;
    int kind, polls;   /* asked: 1 a tune, 2 an untune; polls before it applies */
    bool refuse;       /* the next tune is refused when it applies */
    const char *busy;  /* tune refuses at once with this code */
    int tunes, untunes;
    int restart, down; /* window reads after a tune applies that say the receiver isn't running yet */
    char who[16];
    double lo_ppm, fs_ppm; /* the crystal's error left in the samples (test_xtal) */
    bool trusted;          /* the window says so, with a die temperature (test_track) */
    bool unsettled;        /* the die temperature hasn't settled yet (test_track) */
    int holds;             /* ticks that asked for the correction to be held */
} ft = {.center = NAN, .gain = NAN};
static void f_window(void *ctx, rdof_bl_window_t *w)
{
    const double c = isnan(ft.center) ? 438.225e6 : ft.center;
    const bool running = ft.down <= 0;
    if (!running)
        ft.down--;
    *w = (rdof_bl_window_t){.running = running, .center_hz = c, .fs = 768000, .lo_hz = c - 0.3e6,
                            .hi_hz = c + 0.3e6, .gain_db = isnan(ft.gain) ? 20 : ft.gain, .dc_guard_hz = 25e3,
                            .tx_lo_hz = c - 1e6, .tx_hi_hz = c + 1e6, .ppm = ft.lo_ppm, .lo_ppm = ft.lo_ppm,
                            .fs_ppm = ft.fs_ppm, .ppm_source = ft.lo_ppm ? "clock" : NULL,
                            .ppm_tol = ft.lo_ppm ? 0.05 : 0, .ppm_ref = ft.lo_ppm ? "pps" : NULL,
                            .ppm_trusted = ft.trusted, .die_known = ft.trusted, .die_c = 41.5,
                            .ppm_settled = ft.trusted && !ft.unsettled, .ppm_held = ft.holds > 0};
}
static void f_ppm_hold(void *ctx) { ft.holds++; }
static int f_freqs(void *ctx, rdof_bl_freq_t *out, int max)
{
    static const double f[] = {438.0625e6, 438.1125e6, 438.1625e6, 438.2125e6};
    int n = 0;
    if (!isnan(ft.center)) {
        for (int i = 0; i < ft.ntaps && n < max; i++, n++) {
            out[n] = (rdof_bl_freq_t){.freq_hz = ft.taps[i], .ch = i,
                                      .caps = RDOF_BL_CAP_RX | RDOF_BL_CAP_DMR | RDOF_BL_CAP_IQ};
            snprintf(out[n].id, sizeof out[n].id, "T%d", i + 1);
        }
        return n;
    }
    for (int i = 0; i < 4 && n < max; i++, n++) {
        out[n] = (rdof_bl_freq_t){.freq_hz = f[i], .ch = i,
                                  .caps = RDOF_BL_CAP_RX | RDOF_BL_CAP_DMR | RDOF_BL_CAP_IQ | (i == 1 ? RDOF_BL_CAP_TX : 0)};
        snprintf(out[n].id, sizeof out[n].id, "CH%d", i + 1);
    }
    if (n < max)
        out[n++] = (rdof_bl_freq_t){.freq_hz = 438.6e6, .ch = -1, .caps = RDOF_BL_CAP_TX};
    return n;
}
static int f_tx_bands(void *ctx, double (*out)[2], int max)
{
    out[0][0] = 144e6, out[0][1] = 148e6;
    return 1;
}
static long f_iq_read(void *ctx, uint64_t sc, int8_t *out, long n, uint64_t *oldest, uint64_t *next)
{
    return rdof_bl_ring_read(&ring, sc, out, n, oldest, next);
}
static long f_iq_filled(void *ctx, uint64_t sc, long n) { return rdof_bl_ring_filled(&ring, sc, n); }
static double f_utc(void *ctx) { return 1.8e9; }
static const char *f_tune(void *ctx, const rdof_bl_tune_t *t, const char *who, char *why, size_t cap)
{
    if (ft.busy) {
        snprintf(why, cap, "the HTTP API's session s-1234 holds the SDR");
        return ft.busy;
    }
    if (t->ntaps > 4) {
        snprintf(why, cap, "4 taps at most");
        return "bad_request";
    }
    ft.asked = *t, ft.kind = 1, ft.polls = 2, ft.tunes++;
    snprintf(ft.who, sizeof ft.who, "%s", who);
    return NULL;
}
static int f_tune_state(void *ctx, char *why, size_t cap)
{
    if (ft.polls > 0) {
        ft.polls--;
        return 0;
    }
    if (ft.kind == 1 && ft.refuse) {
        ft.refuse = false, ft.kind = 0;
        snprintf(why, cap, "a tap outside the window");
        return -1;
    }
    if (ft.kind == 1) {
        const rdof_bl_tune_t *t = &ft.asked;
        ft.down = ft.restart;
        ft.center = t->center_hz == 0 ? NAN : isnan(t->center_hz) ? ft.center : t->center_hz;
        ft.gain = t->gain_db == RDOF_BL_TUNE_AUTO ? NAN : isnan(t->gain_db) ? ft.gain : t->gain_db;
        if (t->ntaps >= 0) {
            ft.ntaps = t->ntaps;
            memcpy(ft.taps, t->taps_hz, sizeof ft.taps);
        }
    } else if (ft.kind == 2) {
        ft.down = ft.restart;
        ft.center = ft.gain = NAN, ft.ntaps = 0;
    }
    ft.kind = 0;
    return 1;
}
static void f_untune(void *ctx) { ft.kind = 2, ft.polls = 1, ft.untunes++; }
static double f_sc_utc(void *ctx, uint64_t sc) { return 1.8e9 + (double)sc / 768000; }
static const char *f_can_tx(void *ctx) { return can_tx ? NULL : "no bench.tx"; }

static pthread_mutex_t txmu = PTHREAD_MUTEX_INITIALIZER; /* serve: the server's thread and the loop */
/* The fake transmitter: a 4-burst queue that airs a burst every 60 ms of the test's clock (tx_air). */
static struct {
    bool open, unkeyed, as_built;
    uint32_t freq;
    int cc, power;
    uint8_t q[4][34];
    int qn;
    double next;
    uint8_t aired[600][34];
    int naired, keyups;
} tx;
static bool f_tx_open(void *ctx, uint32_t f, int cc, int power, bool as_built, char *why, size_t cap)
{
    if (tx.open) {
        snprintf(why, cap, "already open");
        return false;
    }
    tx.open = true, tx.freq = f, tx.cc = cc, tx.power = power, tx.qn = 0, tx.unkeyed = false, tx.as_built = as_built;
    snprintf(why, cap, "power %d noted", power);
    return true;
}
static int f_tx_space(void *ctx, char *why, size_t cap)
{
    pthread_mutex_lock(&txmu);
    const int n = 4 - tx.qn;
    pthread_mutex_unlock(&txmu);
    return n;
}
static void f_tx_push(void *ctx, uint8_t control, const uint8_t b[33])
{
    pthread_mutex_lock(&txmu);
    if (tx.qn == 0 && tx.unkeyed)
        tx.keyups++;
    tx.unkeyed = false;
    tx.q[tx.qn][0] = control;
    memcpy(tx.q[tx.qn++] + 1, b, 33);
    pthread_mutex_unlock(&txmu);
}
static void f_tx_unkey(void *ctx) { tx.unkeyed = true; }
static bool f_tx_idle(void *ctx)
{
    pthread_mutex_lock(&txmu);
    const bool idle = tx.qn == 0 && tx.unkeyed;
    pthread_mutex_unlock(&txmu);
    return idle;
}
static void f_tx_close(void *ctx) { tx.open = false; }
/* Air what's due by now (outside the engine's lock): the bursts, back to the engine as heard on channel ch. */
static void tx_air(rdof_bl_t *bl, double now, int ch)
{
    for (;;) {
        pthread_mutex_lock(&txmu);
        if (!tx.qn || now < tx.next) {
            pthread_mutex_unlock(&txmu);
            return;
        }
        uint8_t q[34];
        memcpy(q, tx.q[0], 34);
        if (tx.naired < 600)
            memcpy(tx.aired[tx.naired++], q, 34);
        memmove(tx.q[0], tx.q[1], sizeof tx.q[0] * 3);
        tx.qn--;
        tx.next = now + 0.06;
        const int cc = tx.cc;
        pthread_mutex_unlock(&txmu);
        rdof_bl_burst_t b = {.t = 1.8e9 + now, .ch = ch, .cc = (int8_t)cc, .family = 0};
        const uint8_t control = q[0];
        if (control & 0x40) {
            b.dt = control & 0x0F;
        } else if (control & 0x20) {
            b.dt = RDOF_BL_VOICE_SYNC;
        } else {
            b.dt = RDOF_BL_VOICE;
            b.n = (int8_t)(control & 0x0F);
        }
        memcpy(b.raw, q + 1, 33);
        if (ch >= 0)
            rdof_bl_burst(bl, &b);
    }
}

/* the repeater jobs: a fake repeater whose events the test queues */
static struct {
    bool open, closed;
    rdof_bl_rpt_cfg_t cfg;
    int polls, wakes;
    uint8_t wake_control;
    rdof_bl_rpt_ev_t ev[64];
    int nev;
} rp;
static bool f_rpt_open(void *ctx, const rdof_bl_rpt_cfg_t *cfg, char *why, size_t cap)
{
    rp.open = true, rp.closed = false, rp.cfg = *cfg, rp.polls = 0;
    return true;
}
static int f_rpt_state(void *ctx, char *why, size_t cap) { return ++rp.polls > 2 ? 1 : 0; }
static bool f_rpt_event(void *ctx, rdof_bl_rpt_ev_t *ev)
{
    if (!rp.nev)
        return false;
    *ev = rp.ev[0];
    memmove(rp.ev, rp.ev + 1, sizeof rp.ev[0] * (size_t)--rp.nev);
    return true;
}
static void f_rpt_wake(void *ctx, uint8_t control, const uint8_t b[33])
{
    rp.wakes++, rp.wake_control = control;
    rp.ev[rp.nev++] = (rdof_bl_rpt_ev_t){.kind = RDOF_BL_RPT_HEARD, .slot = 1, .control = control, .rssi = -60};
    memcpy(rp.ev[rp.nev - 1].burst, b, 33);
}
static void f_rpt_close(void *ctx) { rp.open = false, rp.closed = true; }

/* a path that frames the bursts again (the router's): only MS bursts go as built */
static bool routed;
static const char *f_why_family(void *ctx, int family)
{
    return routed && family != RDOF_FAMILY_MS ? "the router's transmitter frames the bursts with MS sync" : NULL;
}

/* the outbound jobs: a fake slot clock, a slot every 30 ms of the test's time from the key-up */
static struct {
    bool open, closed, unkeyed;
    rdof_bl_ob_cfg_t cfg;
    int polls;
    double now, t_key;
    long first, stop, skip_at;
    int skip_n;
    /* the voice: a duplex modem's queue a slot (prebuffer one), and what went out in each slot from the first */
    struct {
        uint8_t control, burst[33];
    } q[2][8];
    int qn[2], pushes, overflow;
    long queued;            /* the next slot the fake modem takes from its queue */
    bool has[2048];
    uint8_t control[2048], burst[2048][33];
} ob;
#define OB_BASE 1000000
static bool f_ob_open(void *ctx, const rdof_bl_ob_cfg_t *cfg, char *why, size_t cap)
{
    ob.open = true, ob.closed = ob.unkeyed = false, ob.cfg = *cfg, ob.polls = 0, ob.stop = -1;
    ob.qn[0] = ob.qn[1] = ob.pushes = ob.overflow = 0, ob.queued = -1;
    memset(ob.has, 0, sizeof ob.has);
    return true;
}
static void f_ob_push(void *ctx, int slot, uint8_t control, const uint8_t burst[33])
{
    const int s = slot - 1;
    ob.pushes++;
    if (ob.qn[s] == 8) {
        ob.overflow++;
        return;
    }
    ob.q[s][ob.qn[s]].control = control;
    memcpy(ob.q[s][ob.qn[s]].burst, burst, 33);
    ob.qn[s]++;
}
/* The fake modem queues slots [queued, next): each takes the head of its slot's queue, else an idle */
static void f_ob_take(long next)
{
    if (ob.queued < 0)
        ob.queued = ob.first;
    for (; ob.queued < next; ob.queued++) {
        const int s = (int)(ob.queued & 1);
        const long i = ob.queued - ob.first;
        if (!ob.qn[s] || i < 0 || i >= 2048)
            continue;
        ob.has[i] = true;
        ob.control[i] = ob.q[s][0].control;
        memcpy(ob.burst[i], ob.q[s][0].burst, 33);
        memmove(&ob.q[s][0], &ob.q[s][1], sizeof ob.q[s][0] * 7);
        ob.qn[s]--;
    }
}
static int f_ob_state(void *ctx, rdof_bl_ob_state_t *s, char *why, size_t cap)
{
    memset(s, 0, sizeof *s);
    s->base = OB_BASE, s->slot = 1440, s->rate = 48000, s->sync_off = 120 + 655, s->first = -1;
    s->ppm = -28;
    if (++ob.polls < 3)
        return 0;
    if (ob.polls == 3)
        ob.t_key = ob.now;
    s->keyed = s->on_air = true;
    s->first = ob.first;
    s->map_pos = OB_BASE, s->map_t = 1.9e9;
    s->next = ob.first + 2 + (long)((ob.now - ob.t_key) / 0.030); /* two slots queued ahead */
    if (ob.skip_n && s->next > ob.skip_at) { /* the clock fell behind once */
        s->skips = 1, s->last_from = ob.skip_at, s->last_to = ob.skip_at + ob.skip_n;
        s->next += ob.skip_n;
    }
    if (ob.stop >= 0 && s->next > ob.stop + 1) {
        s->next = ob.stop + 1;
        f_ob_take(s->next);
        return 2;
    }
    f_ob_take(s->next);
    return 1;
}
static void f_ob_unkey(void *ctx)
{
    rdof_bl_ob_state_t s;
    char why[8];
    ob.polls--; /* this poll doesn't count */
    f_ob_state(ctx, &s, why, sizeof why);
    ob.unkeyed = true, ob.stop = s.next;
}
static void f_ob_close(void *ctx) { ob.open = false, ob.closed = true; }

static const rdof_bl_platform_t PLAT = {.device = f_device, .window = f_window, .freqs = f_freqs, .tx_bands = f_tx_bands,
                                        .iq_read = f_iq_read, .iq_filled = f_iq_filled, .utc = f_utc,
                                        .ppm_hold = f_ppm_hold, .sc_utc = f_sc_utc, .can_tx = f_can_tx, .tx_open = f_tx_open, .tx_space = f_tx_space,
                                        .tx_push = f_tx_push, .tx_unkey = f_tx_unkey, .tx_idle = f_tx_idle,
                                        .tx_close = f_tx_close, .rpt_open = f_rpt_open, .rpt_state = f_rpt_state,
                                        .rpt_event = f_rpt_event, .rpt_wake = f_rpt_wake, .rpt_close = f_rpt_close,
                                        .tx_why_family = f_why_family, .ob_open = f_ob_open, .ob_state = f_ob_state,
                                        .ob_unkey = f_ob_unkey, .ob_close = f_ob_close, .ob_push = f_ob_push};

/* ── helpers ── */

/* Everything c has to send, as text (binary payloads kept, NULs and all). */
static size_t drain(rdof_bl_t *bl, int c, char *out, size_t cap)
{
    size_t n = 0, k;
    while (n + 1 < cap && (k = rdof_bl_output(bl, c, (uint8_t *)out + n, cap - 1 - n)) > 0)
        n += k;
    out[n] = 0;
    return n;
}

static int ask(rdof_bl_t *bl, const char *req, double now)
{
    const int c = rdof_bl_open(bl, now);
    static char line[8192];
    snprintf(line, sizeof line, "%s\n", req);
    rdof_bl_input(bl, c, line, strlen(line), now);
    return c;
}

/* The value of "key":"..." or "key":N in text, as text. */
static bool field(const char *text, const char *key, char *out, size_t cap)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(text, pat);
    if (!p)
        return false;
    p += strlen(pat);
    size_t k = 0;
    if (*p == '"') {
        for (p++; *p && *p != '"' && k + 1 < cap; p++)
            out[k++] = *p;
    } else {
        for (; *p && *p != ',' && *p != '}' && k + 1 < cap; p++)
            out[k++] = *p;
    }
    out[k] = 0;
    return true;
}

static bool is_json_lines(const char *text)
{
    static rdof_jnode_t nodes[2048];
    static char sb[65536];
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        if (!e)
            return false;
        rdof_jdoc_t d = {.nodes = nodes, .max = 2048, .buf = sb, .cap = sizeof sb};
        if (rdof_jparse(&d, p, (size_t)(e - p)) < 0) {
            printf("not JSON (%s): %.*s\n", d.error, (int)(e - p), p);
            return false;
        }
        p = e + 1;
    }
    return true;
}

static void test_ops(void)
{
    rdof_bl_t *bl = rdof_bl_new(&PLAT);
    char out[65536], v[64];
    double now = 100;

    /* info: the window, the channels with their caps, the modem */
    int c = ask(bl, "{\"op\": \"info\"}", now);
    drain(bl, c, out, sizeof out);
    CHECK(is_json_lines(out) && strstr(out, "\"center\":438.225000") && strstr(out, "\"fs\":768000.0") &&
              strstr(out, "\"tx_lo\":437.225000,\"tx_hi\":439.225000") &&
              strstr(out, "\"tx_bands\":[[144.000000,148.000000]]"),
          "info: %s", out);
    CHECK(strstr(out, "{\"freq\":438.112500,\"ch\":1,\"id\":\"CH2\",\"in_window\":true,\"listed\":true,"
                      "\"caps\":[\"rx\",\"dmr\",\"iq\",\"mmdvm_tx\",\"sdr_tx\"],\"claimed_by\":null}") &&
              strstr(out, "{\"freq\":438.600000,\"ch\":null,\"id\":\"\",\"in_window\":false"),
          "info's channels: %s", out);
    CHECK(strstr(out, "\"name\":\"bench \\\"one\\\"\"") && strstr(out, "\"kind\":\"sdrmodem\"") &&
              strstr(out, "\"tx\":true"),
          "info's device and modem: %s", out);
    CHECK(rdof_bl_finished(bl, c), "info is one line");
    rdof_bl_closed(bl, c, now);

    /* a session, held by its connection; a second one */
    const int s1 = ask(bl, "{\"op\": \"session\", \"label\": \"hil-a\", \"max_s\": 60}", now);
    drain(bl, s1, out, sizeof out);
    char id1[16] = "", id2[16] = "";
    CHECK(field(out, "session", id1, sizeof id1) && !rdof_bl_finished(bl, s1), "session: %s", out);
    const int s2 = ask(bl, "{\"op\": \"session\", \"label\": \"hil-b\"}", now);
    drain(bl, s2, out, sizeof out);
    field(out, "session", id2, sizeof id2);
    CHECK(strcmp(id1, id2), "two sessions, two ids: %s %s", id1, id2);

    /* claims: a listed frequency, then the other session waits for it */
    char req[256];
    snprintf(req, sizeof req, "{\"op\": \"claim\", \"session\": \"%s\", \"freqs\": [438.1125]}", id1);
    c = ask(bl, req, now);
    rdof_bl_tick(bl, now);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "{\"claimed\":[438.112500],\"window\":{\"enabled\":true") && rdof_bl_finished(bl, c),
          "claim: %s", out);
    rdof_bl_closed(bl, c, now);
    snprintf(req, sizeof req, "{\"op\": \"claim\", \"session\": \"%s\", \"freqs\": [438.1125], \"wait_max\": 5}", id2);
    const int w = ask(bl, req, now);
    rdof_bl_tick(bl, now);
    drain(bl, w, out, sizeof out);
    CHECK(strstr(out, "\"state\":\"queued\",\"kind\":\"channel\"") && strstr(out, "\"label\":\"hil-a\"") &&
              !rdof_bl_finished(bl, w),
          "the second claim waits: %s", out);
    c = ask(bl, "{\"op\": \"status\"}", now);
    drain(bl, c, out, sizeof out);
    CHECK(is_json_lines(out) && strstr(out, "\"kind\":\"claim\"") && strstr(out, "\"waiting\":[{"), "status: %s", out);
    rdof_bl_closed(bl, c, now);
    /* not listed, and a cap it lacks */
    snprintf(req, sizeof req, "{\"op\": \"claim\", \"session\": \"%s\", \"freqs\": [439.525]}", id1);
    c = ask(bl, req, now);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "\"code\":\"refused\""), "an unlisted frequency: %s", out);
    rdof_bl_closed(bl, c, now);
    snprintf(req, sizeof req, "{\"op\": \"claim\", \"session\": \"%s\", \"freqs\": [438.0625], \"caps\": [\"mmdvm_tx\"]}",
             id1);
    c = ask(bl, req, now);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "\"code\":\"refused\""), "no TX cap there: %s", out);
    rdof_bl_closed(bl, c, now);
    /* the first session releases: the wait is granted */
    snprintf(req, sizeof req, "{\"op\": \"release\", \"session\": \"%s\", \"freqs\": [438.1125]}", id1);
    c = ask(bl, req, now + 1);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "{\"released\":[438.112500]"), "release: %s", out);
    rdof_bl_closed(bl, c, now);
    rdof_bl_tick(bl, now + 1);
    drain(bl, w, out, sizeof out);
    CHECK(strstr(out, "\"state\":\"granted\"") && strstr(out, "\"claimed\":[438.112500]"), "granted: %s", out);
    rdof_bl_closed(bl, w, now);

    /* the modem: a hold, then the other session waits for it and times out */
    snprintf(req, sizeof req, "{\"op\": \"hold\", \"session\": \"%s\", \"expect_s\": 20}", id2);
    c = ask(bl, req, now);
    rdof_bl_tick(bl, now);
    drain(bl, c, out, sizeof out);
    CHECK(field(out, "held", v, sizeof v) && atoi(v) > 0, "hold: %s", out);
    rdof_bl_closed(bl, c, now);
    snprintf(req, sizeof req, "{\"op\": \"hold\", \"session\": \"%s\", \"wait_max\": 2}", id1);
    c = ask(bl, req, now);
    rdof_bl_tick(bl, now);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "\"kind\":\"modem\"") && strstr(out, "\"eta_s\":20.0"), "the modem is held: %s", out);
    rdof_bl_tick(bl, now + 3);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "\"code\":\"timeout\"") && rdof_bl_finished(bl, c), "the wait times out: %s", out);
    rdof_bl_closed(bl, c, now);

    /* the session's connection closes: its claims and hold go */
    rdof_bl_closed(bl, s2, now + 4);
    c = ask(bl, "{\"op\": \"info\"}", now);
    drain(bl, c, out, sizeof out);
    CHECK(!strstr(out, "hil-b"), "the session's claims went with it: %s", out);
    rdof_bl_closed(bl, c, now);
    /* max_s ends a session */
    rdof_bl_tick(bl, now + 61);
    drain(bl, s1, out, sizeof out);
    CHECK(strstr(out, "{\"ended\":\"max_s\"}") && rdof_bl_finished(bl, s1), "max_s: %s", out);
    rdof_bl_closed(bl, s1, now);

    /* bursts: records for the channel's bursts, idle ones too (a repeater's hang); until_end */
    c = ask(bl, "{\"op\": \"bursts\", \"freq\": 438.1125, \"seconds\": 10, \"until_end\": true}", now);
    CHECK(rdof_bl_wants_bursts(bl), "bursts are wanted");
    for (int i = 0; i < 4; i++) {
        rdof_bl_burst_t b = {.ch = i < 3 ? 1 : 2, .t = 1.8e9 + 0.06 * i, .pos48 = 2880 * i, .cfo_hz = 10.0f * i,
                             .level_dbfs = -40,
                             .dt = i == 0 ? 1 : RDOF_BL_VOICE_SYNC, .n = 0, .slot = 1, .family = 1, .cc = 2};
        b.raw[0] = (uint8_t)i;
        rdof_bl_burst(bl, &b);
    }
    rdof_bl_burst_t idle = {.ch = 1, .t = 1.8e9 + 0.3, .dt = 9, .family = 1};
    rdof_bl_burst(bl, &idle);
    rdof_bl_tick(bl, now);
    drain(bl, c, out, sizeof out);
    CHECK(is_json_lines(out) && strstr(out, "{\"ready\":1800000000.000}") &&
              strstr(out, "\"kind\":\"VLC_HDR\"") && strstr(out, "\"kind\":\"VLC_HDR\",\"dt\":1") && strstr(out, "\"sync\":\"MS_DATA\",") && strstr(out, "\"sync\":\"MS_VOICE\",") &&
              strstr(out, "\"dt_prev_ms\":60.000,\"grid_n\":2,\"grid_err_ms\":0.000,\"slot_ab\":\"A\"") &&
              strstr(out, "\"kind\":\"IDLE\"") && !strstr(out, "\"t_rel_ms\":180") &&
              strstr(out, "\"cfo_hz\":10.0,\"cfo_tol_hz\":null,"),
          "burst records: %s", out);
    /* t and sc at the sync's center: 655 samples (48 kS/s) past the start, sc in the 768 kS/s window's count */
    CHECK(strstr(out, "\"t\":1800000000.013646,\"sc\":10480,") &&
              strstr(out, "\"t\":1800000000.073646,\"sc\":56560,"), "sync centers: %s", out);
    rdof_bl_tick(bl, now + 0.5);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "\"done\":{\"bursts\":4,\"reason\":\"ended\"") &&
              strstr(out, "\"kinds\":{\"VLC_HDR\":1,\"IDLE\":1,\"VOICE_A\":2}") && strstr(out, "\"MS_DATA\":2"),
          "the summary: %s", out);
    rdof_bl_closed(bl, c, now);
    CHECK(!rdof_bl_wants_bursts(bl), "no bursts wanted once it's done");
    c = ask(bl, "{\"op\": \"bursts\", \"freq\": 438.6}", now);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "\"code\":\"refused\""), "no receiver channel there: %s", out);
    rdof_bl_closed(bl, c, now);

    /* a capture: pre-roll from the ring, then what comes, cs8 frames, the metadata */
    rdof_bl_ring_init(&ring, 768000);
    float iq[2 * 1000];
    long clipped = 0;
    for (int k = 0; k < 2000; k++)
        iq[k] = k == 0 ? 2.0f : 0.5f;
    for (int k = 0; k < 77; k++) /* 77000 samples: 0.1 s */
        rdof_bl_ring_put(&ring, iq, 1000, &clipped);
    CHECK(clipped == 77, "the clipped samples counted: %ld", clipped);
    c = ask(bl, "{\"op\": \"capture\", \"freq\": 438.1125, \"seconds\": 0.05, \"pre_s\": 0.05}", now);
    rdof_bl_tick(bl, now);
    size_t n = drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "{\"ready\":1800000000.05026") && strstr(out, "\"pre_s\":0.050"), "capture ready: %.200s", out);
    CHECK(rdof_bl_wants_iq(bl), "I/Q wanted");
    static char all[300000];
    size_t total = n;
    memcpy(all, out, n);
    for (int k = 0; k < 40; k++)
        rdof_bl_ring_put(&ring, iq, 1000, &clipped);
    rdof_bl_tick(bl, now + 0.1);
    total += drain(bl, c, all + total, sizeof all - total);
    all[total] = 0;
    /* parse it: lines and payloads */
    long samples = 0, frames = 0;
    bool bad = false;
    char *p = all, *done = NULL;
    while (p < all + total) {
        char *e = memchr(p, '\n', (size_t)(all + total - p));
        if (!e)
            break;
        *e = 0;
        char b[32];
        if (field(p, "bytes", b, sizeof b)) {
            const long nb = atol(b);
            char *pl = e + 1;
            for (long i = 0; i < nb; i++)
                bad |= pl[i] != 64 && pl[i] != 127;
            samples += nb / 2, frames++;
            p = pl + nb;
        } else {
            if (strstr(p, "\"done\""))
                done = p;
            p = e + 1;
        }
    }
    CHECK(samples == 76800 && frames >= 10 && !bad, "%ld samples in %ld frames (bad %d)", samples, frames, bad);
    CHECK(done && strstr(done, "\"fmt\":\"cs8\"") && strstr(done, "\"samples\":76800") &&
              strstr(done, "\"fs\":768000.0") && strstr(done, "\"t0\":1800000000.050260"),
          "the metadata: %s", done ? done : "(none)");
    CHECK(rdof_bl_finished(bl, c), "the capture is over");
    rdof_bl_closed(bl, c, now);
    CHECK(!rdof_bl_wants_iq(bl), "no I/Q wanted after it");
    c = ask(bl, "{\"op\": \"capture\", \"freq\": 438.6, \"seconds\": 1}", now);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "\"code\":\"refused\""), "outside the window: %s", out);
    rdof_bl_closed(bl, c, now);

    /* what isn't here yet, and nonsense */
    c = ask(bl, "{\"op\": \"bridge\"}", now);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "\"code\":\"unsupported\""), "bridge: %s", out);
    rdof_bl_closed(bl, c, now);
    c = ask(bl, "{\"op\": ", now);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "\"code\":\"bad_request\""), "a broken request: %s", out);
    rdof_bl_closed(bl, c, now);
    /* all connections in use */
    int cs[RDOF_BL_CONNS];
    for (int k = 0; k < RDOF_BL_CONNS; k++)
        cs[k] = rdof_bl_open(bl, now);
    CHECK(rdof_bl_open(bl, now) < 0, "a ninth connection is refused");
    for (int k = 0; k < RDOF_BL_CONNS; k++)
        rdof_bl_closed(bl, cs[k], now);
    rdof_bl_ring_free(&ring);
    rdof_bl_free(bl);
}

/* ── run: bench_tx jobs on the fake transmitter, heard back by a listen job ── */

/* ── trigger: a channel cut from the history, a level trigger on it ── */

static uint32_t rng = 1;
static float noise(void)
{
    rng = rng * 1664525u + 1013904223u;
    return ((float)(rng >> 8) / 16777216.0f - 0.5f) * 0.04f; /* about +-2.5 LSB of cs8 */
}

/* fs = 768 kS/s from 438.225 MHz: `ms` of noise, plus a tone at off_hz from center (amplitude amp, 0 none). */
static void put_tone(double ms, double off_hz, float amp)
{
    static uint64_t n_all;
    float iq[2 * 768];
    for (int b = 0; b < (int)(ms + 0.5); b++) {
        for (int i = 0; i < 768; i++, n_all++) {
            const double ph = 2 * M_PI * off_hz * (double)n_all / 768000;
            iq[2 * i] = noise() + amp * (float)cos(ph);
            iq[2 * i + 1] = noise() + amp * (float)sin(ph);
        }
        rdof_bl_ring_put(&ring, iq, 768, NULL);
    }
}

/* Ticks and drains c while `ms` of signal goes in, 10 ms at a time. */
static size_t trig_run(rdof_bl_t *bl, int c, double *now, double ms, double off_hz, float amp, char *all,
                       size_t total, size_t cap)
{
    for (double t = 0; t < ms; t += 10) {
        put_tone(10, off_hz, amp);
        *now += 0.01;
        rdof_bl_tick(bl, *now);
        total += drain(bl, c, all + total, cap - total);
    }
    rdof_bl_tick(bl, *now);
    total += drain(bl, c, all + total, cap - total);
    all[total] = 0;
    return total;
}

static void test_trigger(void)
{
    rdof_bl_t *bl = rdof_bl_new(&PLAT);
    double now = 2000;
    static char all[1 << 20];
    rdof_bl_ring_init(&ring, 768000);
    put_tone(300, 0, 0); /* 0.3 s of noise in the history: the floor */
    int c = ask(bl, "{\"op\":\"trigger\",\"freq\":438.1125,\"bw\":12500,\"pre_s\":0.05,\"post_s\":0.1,"
                    "\"threshold_db\":10,\"min_ms\":5,\"floor_s\":0.2,\"timeout_s\":5,\"fmt\":\"cf32\"}", now);
    /* a strong tone 50 kHz away first (another channel): it mustn't fire */
    size_t total = trig_run(bl, c, &now, 200, 438.1625e6 - 438.225e6, 0.3f, all, 0, sizeof all);
    CHECK(strstr(all, "{\"armed\":{\"floor_db\":") && !strstr(all, "\"iq\"") && !strstr(all, "\"error\""),
          "armed from the history, a neighbor ignored: %.300s", all);
    CHECK(rdof_bl_wants_iq(bl), "a trigger wants I/Q");
    /* then the channel's own: 1 kHz up, 0.3 s */
    total = trig_run(bl, c, &now, 300, 438.1125e6 + 1000 - 438.225e6, 0.3f, all, total, sizeof all);
    total = trig_run(bl, c, &now, 100, 0, 0, all, total, sizeof all);
    long samples = 0, frames = 0, next_idx = 0;
    bool order = true;
    double p_pre = 0, p_sig = 0;
    long n_pre = 0, n_sig = 0;
    char *p = all, *done = NULL;
    while (p < all + total) {
        char *e = memchr(p, '\n', (size_t)(all + total - p));
        if (!e)
            break;
        *e = 0;
        char b[32], sc[32];
        if (field(p, "bytes", b, sizeof b) && field(p, "sc", sc, sizeof sc)) {
            const long nb = atol(b), idx = atol(sc);
            order &= idx == next_idx;
            const float *y = (const float *)(e + 1);
            for (long i = 0; i < nb / 8; i++) {
                float v[2];
                memcpy(v, (const char *)y + 8 * i, 8);
                const double pw = (double)v[0] * v[0] + (double)v[1] * v[1];
                if (idx + i < 2000)
                    p_pre += pw, n_pre++;
                else if (idx + i > 2600 && idx + i < 6000)
                    p_sig += pw, n_sig++;
            }
            samples += nb / 8, frames++;
            next_idx = idx + nb / 8;
            p = e + 1 + nb;
        } else {
            if (strstr(p, "\"done\""))
                done = p;
            p = e + 1;
        }
    }
    char v[32];
    CHECK(done && field(done, "samples", v, sizeof v) && atol(v) == samples && samples > 7000 && samples < 7400 &&
              order, "%ld samples in %ld frames, in order %d: %s", samples, frames, order, done ? done : "(none)");
    CHECK(done && field(done, "signal_start_sample", v, sizeof v) && labs(atol(v) - 2400) < 60,
          "the rise 50 ms in: %s", v);
    CHECK(done && field(done, "snr_db", v, sizeof v) && atof(v) > 25 && strstr(done, "\"rate\":48000.000") &&
              strstr(done, "\"fmt\":\"cf32\""), "the metadata: %s", done ? done : "(none)");
    const double db = 10 * log10((p_sig / n_sig) / (p_pre / n_pre + 1e-30));
    CHECK(n_pre && n_sig && db > 25, "the tone in the samples, not before: %.1f dB", db);
    rdof_bl_tick(bl, now);
    CHECK(rdof_bl_finished(bl, c) && !rdof_bl_wants_iq(bl), "the trigger is over");
    rdof_bl_closed(bl, c, now);

    /* until_end and cs16: it runs while the signal lasts, then post_s more */
    c = ask(bl, "{\"op\":\"trigger\",\"freq\":438.1125,\"pre_s\":0,\"post_s\":0.05,\"min_ms\":5,\"floor_db\":-50,"
                "\"until_end\":true,\"max_s\":5,\"timeout_s\":5,\"fmt\":\"cs16\"}", now);
    total = trig_run(bl, c, &now, 50, 0, 0, all, 0, sizeof all);
    CHECK(strstr(all, "\"armed\":{\"floor_db\":-50.0"), "a given floor arms at once: %s", all);
    total = trig_run(bl, c, &now, 400, 438.1125e6 - 438.225e6, 0.3f, all, total, sizeof all);
    total = trig_run(bl, c, &now, 200, 0, 0, all, total, sizeof all);
    done = NULL;
    for (size_t i = 0; i + 7 < total && !done; i++)
        if (!memcmp(all + i, "{\"done\"", 7))
            done = all + i;
    CHECK(done && field(done, "samples", v, sizeof v) && labs(atol(v) - 21600) < 300 &&
              strstr(done, "\"fmt\":\"cs16\""), "0.4 s of signal and 50 ms after: %s", done ? done : "(none)");
    rdof_bl_closed(bl, c, now);

    /* nothing comes */
    c = ask(bl, "{\"op\":\"trigger\",\"freq\":438.1125,\"floor_db\":-50,\"timeout_s\":0.1}", now);
    trig_run(bl, c, &now, 200, 0, 0, all, 0, sizeof all);
    CHECK(strstr(all, "\"code\":\"no_signal\""), "no_signal: %s", all);
    rdof_bl_closed(bl, c, now);
    c = ask(bl, "{\"op\":\"trigger\",\"freq\":438.6}", now);
    drain(bl, c, all, sizeof all);
    CHECK(strstr(all, "\"code\":\"refused\""), "outside the window: %s", all);
    rdof_bl_closed(bl, c, now);
    CHECK(!rdof_bl_wants_iq(bl), "no I/Q wanted after them");
    rdof_bl_ring_free(&ring);
    rdof_bl_free(bl);
}

/* ── the crystal: a trigger tunes where a carrier truly on its frequency shows ── */

static void test_xtal(void)
{
    rdof_bl_t *bl = rdof_bl_new(&PLAT);
    double now = 3000;
    static char all[1 << 20];
    char info[4096];
    rdof_bl_ring_init(&ring, 768000);
    ft.lo_ppm = ft.fs_ppm = 10; /* 4.4 kHz at 439 MHz: past the trigger's 2 kHz */
    int c = ask(bl, "{\"op\":\"info\"}", now);
    drain(bl, c, info, sizeof info);
    CHECK(strstr(info, "\"ppm\":10.000,\"ppm_source\":\"clock\",\"ppm_tol\":0.050,\"ppm_ref\":\"pps\","
                       "\"lo_ppm\":10.000,\"fs_ppm\":10.000,"),
          "info's window says the correction: %.600s", info);
    rdof_bl_closed(bl, c, now);
    put_tone(300, 0, 0);
    c = ask(bl, "{\"op\":\"trigger\",\"freq\":438.1125,\"bw\":2000,\"pre_s\":0,\"post_s\":0.05,\"threshold_db\":10,"
                "\"min_ms\":5,\"floor_s\":0.2,\"timeout_s\":2,\"fmt\":\"cf32\"}", now);
    /* the carrier truly on 438.1125 MHz, where the crystal puts it: (f - center (1 + lo)) / (1 + fs) */
    const double seen = (438.1125e6 - 438.225e6 * (1 + 10e-6)) / (1 + 10e-6);
    size_t total = trig_run(bl, c, &now, 200, seen, 0.3f, all, 0, sizeof all);
    total = trig_run(bl, c, &now, 100, 0, 0, all, total, sizeof all);
    const char *done = NULL;
    for (size_t i = 0; i + 7 < total && !done; i++)
        if (!memcmp(all + i, "{\"done\"", 7))
            done = all + i;
    char v[32];
    CHECK(done && field(done, "corr_hz", v, sizeof v) && fabs(atof(v) - (seen - (438.1125e6 - 438.225e6))) < 0.01 &&
              strstr(done, "\"ppm_source\":\"clock\"") && strstr(done, "\"freq_tol_hz\":21.9") &&
              field(done, "snr_db", v, sizeof v) && atof(v) > 20,
          "fired on the carrier, tuned %.1f Hz for the crystal: %.500s", seen - (438.1125e6 - 438.225e6),
          done ? done : all);
    rdof_bl_closed(bl, c, now);
    /* a burst's cfo with the tolerance at its frequency, and the summary's */
    c = ask(bl, "{\"op\": \"bursts\", \"freq\": 438.1125, \"seconds\": 10, \"max_bursts\": 1}", now);
    rdof_bl_burst_t b = {.ch = 1, .t = 1.8e9, .cfo_hz = 151.0f, .level_dbfs = -40, .dt = 1, .family = 1, .cc = 1};
    rdof_bl_burst(bl, &b);
    rdof_bl_tick(bl, now);
    drain(bl, c, all, sizeof all);
    CHECK(strstr(all, "\"cfo_hz\":151.0,\"cfo_tol_hz\":21.9,") && strstr(all, "\"max\":151.0,\"tol\":21.9}"),
          "the burst's tolerance, 0.05 ppm at 439 MHz: %.800s", all);
    rdof_bl_closed(bl, c, now);
    /* the same carrier with the correction unsaid: the channel at the nominal offset doesn't hear it */
    ft.lo_ppm = ft.fs_ppm = 0;
    c = ask(bl, "{\"op\":\"trigger\",\"freq\":438.1125,\"bw\":2000,\"pre_s\":0,\"post_s\":0.05,\"threshold_db\":10,"
                "\"min_ms\":5,\"floor_s\":0.2,\"timeout_s\":0.5,\"fmt\":\"cf32\"}", now);
    trig_run(bl, c, &now, 200, 0, 0, all, 0, sizeof all);
    trig_run(bl, c, &now, 600, seen, 0.3f, all, 0, sizeof all);
    CHECK(strstr(all, "\"code\":\"no_signal\""), "uncorrected, 4.6 kHz off: %.300s", all);
    c = ask(bl, "{\"op\":\"info\"}", now);
    drain(bl, c, info, sizeof info);
    CHECK(strstr(info, "\"ppm_tol\":null,\"ppm_ref\":null"), "no tolerance known: null, %.600s", info);
    rdof_bl_closed(bl, c, now);
    rdof_bl_closed(bl, c, now);
    rdof_bl_ring_free(&ring);
    rdof_bl_free(bl);
}

/* ── track: an FM carrier and a lopsided 4FSK one, followed ─────────────── */

/* ms of a carrier off_hz from the window's center: FM, a 1 kHz tone at 3 kHz deviation; 4FSK (fsk), 4800 symbols a
 * second, +3 (1944 Hz) seven times in ten, so its mean is far from its center */
static void put_mod(double ms, double off_hz, bool fsk)
{
    static double ph;
    static uint64_t n;
    static int sym;
    float iq[2 * 768];
    for (int b = 0; b < (int)(ms + 0.5); b++) {
        for (int i = 0; i < 768; i++, n++) {
            double dev;
            if (fsk) {
                if (n % 160 == 0) {
                    const int r = rand() % 10;
                    sym = r < 7 ? 3 : r == 7 ? 1 : r == 8 ? -1 : -3;
                }
                dev = 648.0 * sym;
            } else {
                dev = 3000 * sin(2 * M_PI * 1000 * (double)n / 768000);
            }
            ph = fmod(ph + 2 * M_PI * (off_hz + dev) / 768000, 2 * M_PI);
            iq[2 * i] = noise() + 0.3f * (float)cos(ph);
            iq[2 * i + 1] = noise() + 0.3f * (float)sin(ph);
        }
        rdof_bl_ring_put(&ring, iq, 768, NULL);
    }
}

static size_t track_run(rdof_bl_t *bl, int c, double *now, double ms, double off_hz, bool fsk, bool on, char *all,
                        size_t total, size_t cap)
{
    for (double t = 0; t < ms; t += 10) {
        if (on)
            put_mod(10, off_hz, fsk);
        else
            put_tone(10, 0, 0);
        *now += 0.01;
        rdof_bl_tick(bl, *now);
        total += drain(bl, c, all + total, cap - total);
    }
    all[total] = 0;
    return total;
}

static void test_track(void)
{
    rdof_bl_t *bl = rdof_bl_new(&PLAT);
    double now = 4000;
    static char all[1 << 18];
    rdof_bl_ring_init(&ring, 768000);
    ft.lo_ppm = ft.fs_ppm = 10, ft.trusted = true;
    const double seen = (438.1125e6 - 438.225e6 * (1 + 10e-6)) / (1 + 10e-6);
    for (int fsk = 0; fsk < 2; fsk++) {
        const double off = fsk ? -300 : 150; /* the carrier from 438.1125 MHz */
        put_tone(300, 0, 0);
        int c = ask(bl, fsk ? "{\"op\":\"track\",\"freq\":438.1125,\"mode\":\"4fsk\",\"floor_s\":0.2,\"drift_s\":1.5,"
                              "\"timeout_s\":5}"
                            : "{\"op\":\"track\",\"freq\":438.1125,\"floor_s\":0.2,\"drift_s\":1.5,\"timeout_s\":5}",
                    now);
        size_t total = track_run(bl, c, &now, 100, 0, false, false, all, 0, sizeof all);
        total = track_run(bl, c, &now, 2000, seen + off * (1 + 10e-6) / (1 + 10e-6), fsk, true, all, total,
                          sizeof all);
        total = track_run(bl, c, &now, 800, 0, false, false, all, total, sizeof all);
        char v[32];
        int lines = 0, off_lines = 0, transient = 0;
        char *p = all, *done = strstr(all, "{\"done\""), *bad = all;
        while ((p = strstr(p, "{\"track\""))) {
            lines++;
            if (field(p, "hz", v, sizeof v) && fabs(atof(v) - off) > 25 &&
                !(strstr(p, "\"transient\":true") && strstr(p, "\"transient\":true") < strchr(p, '\n')))
                off_lines++, bad = p;
            transient += strstr(p, "\"transient\":true") && strstr(p, "\"transient\":true") < strchr(p, '\n');
            p++;
        }
        /* the first interval too, transient as it is: nothing but the carrier in it */
        const char *first = strstr(all, "{\"track\"");
        CHECK(first && field(first, "hz", v, sizeof v) && fabs(atof(v) - off) < 60, "%s: the first line: %.200s",
              fsk ? "4fsk" : "fm", first ? first : all);
        CHECK(lines >= 18 && lines <= 21 && !off_lines && transient == 3 &&
                  strstr(all, "\"ppm_source\":\"clock\",\"trusted\":true,\"tol_hz\":21.9,\"die_c\":41.5"),
              "%s: %d lines of 100 ms, %d off by more than 25 Hz, %d transient: %.700s", fsk ? "4fsk" : "fm", lines,
              off_lines, transient, bad);
        CHECK(done && field(done, "settled_hz", v, sizeof v) && fabs(atof(v) - off) < 10 &&
                  field(done, "drift_hz", v, sizeof v) && fabs(atof(v)) < 10 &&
                  strstr(done, "\"trusted_throughout\":true,\"settled_throughout\":true,\"gap_ms\":0,"
                               "\"warnings\":[],") &&
                  strstr(done, "\"ppm_moved\":0.000") && strstr(done, "\"freq_tol_hz\":21.9") &&
                  strstr(done, "\"ppm_held\":true"),
              "%s: settled on %.0f Hz: %.900s", fsk ? "4fsk" : "fm", off, done ? done : "(no done)");
        if (fsk)
            CHECK(strstr(all, "\"dev_hz\":19"), "4fsk: the outer deviation near 1944 Hz: %.300s", all);
        else
            CHECK(!strstr(all, "\"dev_hz\""), "fm: no deviation: %.300s", all);
        rdof_bl_closed(bl, c, now);
    }
    /* the chip moves 1 ppm while the engine is behind: no line is corrected with a window its samples weren't
     * written under. The 0.5 s backlog went in between two ticks, so it could be under either: left out (on a
     * board, ticks 10 ms apart, a change costs about that) */
    put_tone(300, 0, 0);
    int c = ask(bl, "{\"op\":\"track\",\"freq\":438.1125,\"floor_s\":0.2,\"timeout_s\":5}", now);
    const double off = 150, seen_new = (438.1125e6 - 438.225e6 * (1 + 11e-6)) / (1 + 10e-6);
    size_t total = track_run(bl, c, &now, 100, 0, false, false, all, 0, sizeof all);
    total = track_run(bl, c, &now, 600, seen + off, false, true, all, total, sizeof all);
    put_mod(500, seen + off, false); /* the backlog */
    ft.lo_ppm = 11;
    total = track_run(bl, c, &now, 1000, seen_new + off, false, true, all, total, sizeof all);
    total = track_run(bl, c, &now, 800, 0, false, false, all, total, sizeof all);
    int lines = 0, bad = 0;
    const char *badp = all;
    for (const char *p = all; (p = strstr(p, "{\"track\"")); p++) {
        char v[32];
        lines++;
        if (field(p, "hz", v, sizeof v) && fabs(atof(v) - off) > 25)
            bad++, badp = p;
    }
    CHECK(lines >= 14 && lines <= 17 && !bad && strstr(all, "\"ppm_moved\":1.000"),
          "the correction moved 461 Hz mid-track, %d lines, %d off: %.300s", lines, bad, badp);
    CHECK(strstr(all, "\"warnings\":[\"ppm_moved\"]"), "a move said: %.300s", strstr(all, "{\"done\"") ?: all);
    rdof_bl_closed(bl, c, now);
    /* warming and not yet on the trusted clock: measured, and said; the hold asked for each tick while open */
    ft.lo_ppm = 10, ft.unsettled = true;
    put_tone(300, 0, 0);
    ft.holds = 0;
    c = ask(bl, "{\"op\":\"track\",\"freq\":438.1125,\"floor_s\":0.2,\"timeout_s\":5}", now);
    total = track_run(bl, c, &now, 100, 0, false, false, all, 0, sizeof all);
    total = track_run(bl, c, &now, 1000, seen + off, false, true, all, total, sizeof all);
    total = track_run(bl, c, &now, 800, 0, false, false, all, total, sizeof all);
    const int held = ft.holds;
    rdof_bl_closed(bl, c, now);
    rdof_bl_tick(bl, now += 0.01);
    CHECK(strstr(all, "\"settled_throughout\":false") && strstr(all, "\"warnings\":[\"not_settled\"]") &&
              held > 100 && ft.holds == held,
          "not settled said, %d holds then %d: %.300s", held, ft.holds, strstr(all, "{\"done\"") ?: all);
    ft.unsettled = false;
    ft.lo_ppm = ft.fs_ppm = 0, ft.trusted = false;
    rdof_bl_ring_free(&ring);
    rdof_bl_free(bl);
}

/* ── I/Q formats: a channel's weak signal in cs8 at the wideband's gain ── */

static void test_iq_formats(void)
{
    /* an LR2021 channel's idle noise: about 1.4 of cs16's 32767 rms, a few counts at most */
    float iq[2 * 1000];
    double p = 0;
    for (int i = 0; i < 2000; i++) {
        iq[i] = (float)(2.0 * sin(0.37 * i) / 32767.0);
        p += (double)iq[i] * iq[i];
    }
    uint8_t out[4 * 1000];
    long nz = 0;
    rdof_bl_iq_convert(iq, 1000, RDOF_BL_IQ_CS8, 1.0f, out);
    for (int i = 0; i < 2000; i++)
        nz += out[i] != 0;
    CHECK(nz == 0, "at the ADC's full scale cs8 rounds it to nothing: %ld nonzero", nz);
    const int clipped = rdof_bl_iq_convert(iq, 1000, RDOF_BL_IQ_CS8, 128.0f, out);
    double q = 0;
    nz = 0;
    for (int i = 0; i < 2000; i++) {
        const int v = (int8_t)out[i];
        nz += v != 0;
        q += (double)v * v;
    }
    CHECK(nz > 1000 && clipped == 0, "at k 128 it's there: %ld of 2000 nonzero, %d clipped", nz, clipped);
    /* the gain, on a signal well over a count (rounding adds to a weak one's power) */
    float loud[2 * 1000];
    p = q = 0;
    for (int i = 0; i < 2000; i++) {
        loud[i] = 10 * iq[i];
        p += (double)loud[i] * loud[i];
    }
    rdof_bl_iq_convert(loud, 1000, RDOF_BL_IQ_CS8, 128.0f, out);
    for (int i = 0; i < 2000; i++)
        q += (double)(int8_t)out[i] * (int8_t)out[i];
    const double db = 10 * log10(q / 2000) - 10 * log10(p / 2000);
    CHECK(fabs(db - (20 * log10(127.0) + rdof_bl_iq_scale_db(RDOF_BL_IQ_CS8, 128.0f))) < 0.2,
          "its level less scale_db is the ADC's: %.2f dB", db);
    CHECK(fabs(rdof_bl_iq_scale_db(RDOF_BL_IQ_CS8, 128.0f) - 42.14) < 0.01 &&
              fabs(rdof_bl_iq_scale_db(RDOF_BL_IQ_CU8, 128.0f) - 42.14) < 0.01 &&
              rdof_bl_iq_scale_db(RDOF_BL_IQ_CS16, 128.0f) == 0 && rdof_bl_iq_scale_db(RDOF_BL_IQ_CF32, 128.0f) == 0,
          "scale_db for every format");
    rdof_bl_iq_convert(iq, 1000, RDOF_BL_IQ_CU8, 128.0f, out);
    CHECK(out[0] == 128 && out[2] != 128, "cu8 about 128: %u %u", out[0], out[2]);
    rdof_bl_iq_convert(iq, 1000, RDOF_BL_IQ_CS16, 128.0f, out);
    CHECK((int16_t)(out[2] | out[3] << 8) == (int16_t)lrintf(iq[1] * 32767.0f), "cs16 at full scale, little-endian");
    const float big[4] = {2.0f, -2.0f, 0.5f, -0.5f};
    CHECK(rdof_bl_iq_convert(big, 2, RDOF_BL_IQ_CS16, 1.0f, out) == 2 && (int16_t)(out[0] | out[1] << 8) == 32767 &&
              (int16_t)(out[2] | out[3] << 8) == -32767, "cs16 clips at its full scale");
    /* rounded as lrintf rounds (ties to even), its call left out: every value, halves and their neighbors too */
    int bad = 0;
    for (int i = -70000; i <= 70000 && !bad; i++) {
        const float x[6] = {i / 2.0f, i / 2.0f + 1e-3f, i / 2.0f - 1e-3f, i / 7.3f, i * 0.4999f, i * 0.5001f};
        for (int j = 0; j < 6; j++) {
            const float v[2] = {x[j] / 32767.0f, x[j] / 127.0f / 128.0f};
            uint8_t o16[4], o8[2];
            rdof_bl_iq_convert(v, 1, RDOF_BL_IQ_CS16, 1.0f, o16);
            rdof_bl_iq_convert(v, 1, RDOF_BL_IQ_CS8, 128.0f, o8);
            float a = v[0] * 32767.0f, b = v[1] * 127.0f * 128.0f;
            a = a > 32767 ? 32767 : a < -32767 ? -32767 : a;
            b = b > 127 ? 127 : b < -127 ? -127 : b;
            bad += (int16_t)(o16[0] | o16[1] << 8) != (int16_t)lrintf(a) || (int8_t)o8[1] != (int8_t)lrintf(b);
        }
    }
    CHECK(!bad, "rounds as lrintf does");
}

/* n samples of filler (zeros where the front end lost samples) into the history */
static void put_filler(long n)
{
    static const float z[2 * 768];
    for (long k = 0; k < n; k += 768)
        rdof_bl_ring_put(&ring, z, n - k < 768 ? n - k : 768, NULL);
    rdof_bl_ring_mark_filler(&ring, n);
}

/* ── filler: the history's record of it, a capture's and a trigger's gap_samples ── */

static void test_filler(void)
{
    /* the record: runs merge when they follow, counts by overlap, the oldest goes when it's full */
    rdof_bl_fills_t f;
    rdof_bl_fills_reset(&f);
    rdof_bl_fills_note(&f, 100, 50);
    rdof_bl_fills_note(&f, 150, 10); /* follows: one run, [100, 160) */
    rdof_bl_fills_note(&f, 400, 20);
    CHECK(f.count == 2 && rdof_bl_fills_count(&f, 0, 1000) == 80, "two runs, 80 samples: %d, %ld", f.count,
          rdof_bl_fills_count(&f, 0, 1000));
    CHECK(rdof_bl_fills_count(&f, 90, 20) == 10 && rdof_bl_fills_count(&f, 155, 250) == 10 &&
              rdof_bl_fills_count(&f, 160, 240) == 0 && rdof_bl_fills_count(&f, 410, 1) == 1,
          "counted by overlap");
    for (int k = 0; k < RDOF_BL_FILLS; k++)
        rdof_bl_fills_note(&f, 1000 + 10 * (uint64_t)k, 5);
    CHECK(f.count == RDOF_BL_FILLS && rdof_bl_fills_count(&f, 0, 1000) == 0 &&
              rdof_bl_fills_count(&f, 1000, 10 * RDOF_BL_FILLS) == 5 * RDOF_BL_FILLS,
          "full: the oldest went (%ld before 1000)", rdof_bl_fills_count(&f, 0, 1000));

    /* skip: counted as filler, nothing written, the zeros init gave it read back */
    {
        rdof_bl_ring_t r;
        CHECK(rdof_bl_ring_init(&r, 1000), "a ring");
        rdof_bl_ring_skip(&r, 600);
        rdof_bl_ring_skip(&r, 700);
        int8_t out[2 * 500];
        memset(out, 1, sizeof out);
        uint64_t oldest = 0, next = 0;
        const long k = rdof_bl_ring_read(&r, 800, out, 500, &oldest, &next);
        int nz = 0;
        for (long i = 0; i < 2 * k; i++)
            nz += out[i] != 0;
        CHECK(r.w == 1300 && next == 1300 && k == 500 && !nz, "skip: 1300 counted, 500 zeros read: %llu, %ld, %d",
              (unsigned long long)r.w, k, nz);
        CHECK(rdof_bl_ring_filled(&r, 800, 500) == 500, "skip: filler: %ld", rdof_bl_ring_filled(&r, 800, 500));
        rdof_bl_ring_free(&r);
    }

    /* a capture over filler: gap_samples counts what's in it (not what's before), the frame says gap */
    rdof_bl_t *bl = rdof_bl_new(&PLAT);
    double now = 3000;
    static char all[300000];
    char out[4096];
    rdof_bl_ring_init(&ring, 768000);
    float iq[2 * 1000];
    for (int k = 0; k < 2000; k++)
        iq[k] = 0.5f;
    for (int k = 0; k < 10; k++)
        rdof_bl_ring_put(&ring, iq, 1000, NULL);
    put_filler(1000); /* [10000, 11000): before the capture */
    for (int k = 0; k < 66; k++)
        rdof_bl_ring_put(&ring, iq, 1000, NULL);
    CHECK(ring.w == 77000, "the history: %llu", (unsigned long long)ring.w);
    int c = ask(bl, "{\"op\": \"capture\", \"freq\": 438.1125, \"seconds\": 0.05, \"pre_s\": 0.05}", now);
    size_t total = drain(bl, c, out, sizeof out); /* sc0 38600, to 115400 */
    memcpy(all, out, total);
    for (int k = 0; k < 10; k++)
        rdof_bl_ring_put(&ring, iq, 1000, NULL);
    put_filler(1500); /* [87000, 88500): in it */
    for (int k = 0; k < 30; k++)
        rdof_bl_ring_put(&ring, iq, 1000, NULL);
    rdof_bl_tick(bl, now + 0.1);
    total += drain(bl, c, all + total, sizeof all - total);
    all[total] = 0;
    long samples = 0, zeros = 0, flagged = 0;
    char *p = all, *done = NULL;
    while (p < all + total) {
        char *e = memchr(p, '\n', (size_t)(all + total - p));
        if (!e)
            break;
        *e = 0;
        char b[32], fl[16];
        if (field(p, "bytes", b, sizeof b)) {
            const long nb = atol(b);
            if (field(p, "flags", fl, sizeof fl) && atoi(fl) & 1)
                flagged++;
            for (long i = 0; i < nb; i += 2)
                zeros += !e[1 + i] && !e[2 + i];
            samples += nb / 2;
            p = e + 1 + nb;
        } else {
            if (strstr(p, "\"done\""))
                done = p;
            p = e + 1;
        }
    }
    /* frames of 8192 from 38600: the filler is in two of them */
    CHECK(samples == 76800 && zeros == 1500 && flagged == 2, "%ld samples, %ld zeros, %ld frames flagged", samples,
          zeros, flagged);
    CHECK(done && strstr(done, "\"gap_samples\":1500,") && strstr(done, "\"overflows\":1,"),
          "the filler in gap_samples: %s", done ? done : "(none)");
    rdof_bl_closed(bl, c, now);
    rdof_bl_ring_free(&ring);

    /* a trigger: the filler in what it sends, in the channel's samples (16 wideband ones each) */
    rdof_bl_ring_init(&ring, 768000);
    put_tone(100, 0, 0);
    c = ask(bl, "{\"op\":\"trigger\",\"freq\":438.1125,\"pre_s\":0.02,\"post_s\":0.2,\"min_ms\":5,"
                "\"floor_db\":-50,\"timeout_s\":5,\"fmt\":\"cs16\"}", now);
    total = trig_run(bl, c, &now, 20, 0, 0, all, 0, sizeof all);
    total = trig_run(bl, c, &now, 50, 438.1125e6 - 438.225e6, 0.3f, all, total, sizeof all);
    put_filler(7680); /* 10 ms: 480 of the channel's samples */
    total = trig_run(bl, c, &now, 300, 438.1125e6 - 438.225e6, 0.3f, all, total, sizeof all);
    done = NULL;
    for (size_t i = 0; i + 7 < total && !done; i++)
        if (!memcmp(all + i, "{\"done\"", 7))
            done = all + i;
    CHECK(done && strstr(done, "\"gap_samples\":480,") && strstr(done, "\"overflows\":1,"),
          "the trigger's gap_samples: %s", done ? done : "(none)");
    rdof_bl_closed(bl, c, now);
    rdof_bl_ring_free(&ring);
    rdof_bl_free(bl);
}

static size_t drain_all(rdof_bl_t *bl, int c, char *out, size_t cap, size_t have)
{
    size_t k;
    while (have + 1 < cap && (k = rdof_bl_output(bl, c, (uint8_t *)out + have, cap - 1 - have)) > 0)
        have += k;
    out[have] = 0;
    return have;
}

static int count(const char *text, const char *needle)
{
    int n = 0;
    for (const char *p = text; (p = strstr(p, needle)) != NULL; p += strlen(needle))
        n++;
    return n;
}

/* dmr-bursts records' decode: a voice call and CSBKs built by the job code, clean and with bit errors. */
static void decode_job(const char *json, rdof_bj_burst_t *out, int *n)
{
    static rdof_jnode_t nodes[64];
    static char sb[1024];
    static uint8_t ambe[64];
    rdof_jdoc_t d = {.nodes = nodes, .max = 64, .buf = sb, .cap = sizeof sb};
    rdof_bj_job_t j;
    char e[200], note[200], done[200];
    const int root = rdof_jparse(&d, json, strlen(json));
    e[0] = 0;
    CHECK(root >= 0 && rdof_bj_parse(&d, root, &j, ambe, sizeof ambe, e, sizeof e), "job parses (%d): %s", root, e);
    *n = rdof_bj_build(&j, 0, out, 64, note, done, sizeof note, e, sizeof e);
    CHECK(*n > 0, "job builds: %s", e);
}

static rdof_bl_burst_t heard(const rdof_bj_burst_t *q)
{
    rdof_bl_burst_t b = {.cc = 2};
    b.dt = q->control & 0x40 ? q->control & 0x0F : q->control & 0x20 ? RDOF_BL_VOICE_SYNC : RDOF_BL_VOICE;
    b.n = (int8_t)(b.dt == RDOF_BL_VOICE ? q->control & 0x0F : 0);
    memcpy(b.raw, q->burst, 33);
    return b;
}

static void test_decode(void)
{
    rdof_bj_burst_t q[64];
    int n = 0;
    char out[1200];
    decode_job("{\"tool\":\"bench_tx\",\"cmd\":\"voice\",\"freq\":438.1125,\"cc\":2,\"src\":9990301,\"dst\":215,\"superframes\":1}", q, &n);
    rdof_bj_emb_t emb = {0};
    int lcs = 0, ok = 0;
    for (int i = 0; i < n; i++) {
        rdof_bl_burst_t b = heard(&q[i]);
        if (i == 0)
            b.raw[5] ^= 0x10; /* one bit in the header's BPTC */
        rdof_bj_burst_decode(&emb, &b, out, sizeof out);
        ok += strstr(out, "\"fec_ok\":true") != NULL;
        if (i == 0)
            CHECK(strstr(out, "\"bptc_corrected\":1") && strstr(out, "\"rs129_ok\":true") &&
                      strstr(out, "\"bit_err_est\":1") && strstr(out, "\"type\":\"grp_voice\"") &&
                      strstr(out, "\"src\":9990301,\"dst\":215"),
                  "the header's decode: %s", out);
        if (b.dt == RDOF_BL_VOICE && b.n == 4) {
            lcs++;
            CHECK(strstr(out, "\"lc\":{\"type\":\"grp_voice\"") && strstr(out, "\"emb_flco\":0") &&
                      strstr(out, "\"emb_corrected\":0"),
                  "voice E's embedded LC: %s", out);
        } else if (b.dt == RDOF_BL_VOICE || b.dt == RDOF_BL_VOICE_SYNC) {
            CHECK(strstr(out, "\"lc\":null") && strstr(out, "\"ambe_corrected\":[0,0,0]"), "voice: %s", out);
        }
    }
    CHECK(lcs == 1 && ok == n, "one embedded LC (%d), every burst's FEC ok (%d of %d)", lcs, ok, n);
    decode_job("{\"tool\":\"bench_tx\",\"cmd\":\"csbk\",\"freq\":438.1125,\"cc\":2,\"count\":1}", q, &n);
    rdof_bl_burst_t b = heard(&q[0]);
    rdof_bj_burst_decode(&emb, &b, out, sizeof out);
    CHECK(strstr(out, "\"crc_ok\":true") && strstr(out, "\"lc\":{\"type\":\"csbk\",\"csbko\":"), "csbk: %s", out);
    memset(b.raw, 0x55, 12); /* past what the BPTC corrects */
    rdof_bj_burst_decode(&emb, &b, out, sizeof out);
    CHECK(strstr(out, "\"fec_ok\":false"), "a ruined csbk: %s", out);
}

static void no_emit(void *ctx, const char *line) { (void)ctx, (void)line; }

/* A late entry (--late K): no header, the call's first K bursts left out, each burst at its own position; the
 * listener takes it as one call, "late entry", its addresses from the embedded LC, ended by its terminators. */
static void test_late(void)
{
    rdof_bj_burst_t q[64];
    int n = 0;
    decode_job("{\"tool\":\"bench_tx\",\"cmd\":\"voice\",\"freq\":438.1125,\"cc\":2,\"src\":9990301,\"dst\":215,"
               "\"superframes\":6,\"late\":8,\"terminators\":3}", q, &n);
    CHECK(n == 36 - 8 + 3, "28 voice bursts and 3 terminators: %d", n);
    int headers = 0, misplaced = 0;
    for (int i = 0; i < n; i++) {
        headers += q[i].control == (RDOF_SYNC_DATA | RDOF_DT_VOICE_LC_HEADER);
        if (i < 28) {
            const int pos = (8 + i) % 6, got = q[i].control == RDOF_SYNC_AUDIO ? 0 : q[i].control;
            misplaced += got != pos;
        }
    }
    CHECK(headers == 0 && misplaced == 0 && q[28].control == (RDOF_SYNC_DATA | RDOF_DT_TERMINATOR_WITH_LC),
          "no header (%d), from voice C on at each position (%d misplaced), then the terminator", headers, misplaced);
    static rdof_bj_listener_t l;
    rdof_bj_listen_init(&l, true, false);
    for (int i = 0; i < n; i++) {
        rdof_bl_burst_t b = heard(&q[i]);
        b.t = 100.0 + 0.06 * i;
        rdof_bj_listen_burst(&l, &b, no_emit, NULL);
    }
    const rdof_bj_call_t *c = &l.calls[0];
    CHECK(l.ncalls == 1 && c->start && !strcmp(c->start, "late entry") && c->headers == 0 && c->voice == 28,
          "one late entry call, 28 voice bursts: %d %s %d", l.ncalls, c->start ? c->start : "-", c->voice);
    CHECK(c->has_src && c->src == 9990301 && c->dst == 215 && c->terminators == 3 && c->ended_by &&
              !strcmp(c->ended_by, "terminator"),
          "its addresses from the embedded LC, ended by its terminators: %d %u %u %d", c->has_src, (unsigned)c->src,
          (unsigned)c->dst, c->terminators);
    rdof_bj_listen_free(&l);
    /* --late 0: no header, the whole call from its first A */
    decode_job("{\"tool\":\"bench_tx\",\"cmd\":\"voice\",\"freq\":438.1125,\"cc\":2,\"superframes\":2,\"late\":0}", q, &n);
    CHECK(n >= 12 && q[0].control == RDOF_SYNC_AUDIO, "late 0: from A, no header (%d bursts, first %02x)", n,
          q[0].control);
}

static void test_run(void)
{
    static char out[1 << 20], lis[1 << 20];
    rdof_bl_t *bl = rdof_bl_new(&PLAT);
    double now = 2000;
    memset(&tx, 0, sizeof tx);

    /* refused: not a TX frequency, no job, an option it can't read */
    int c = ask(bl, "{\"op\":\"run\",\"job\":{\"tool\":\"bench_tx\",\"cmd\":\"voice\",\"freq\":438.0625}}", now);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "\"code\":\"refused\""), "not a TX frequency: %s", out);
    rdof_bl_closed(bl, c, now);
    c = ask(bl, "{\"op\":\"run\",\"argv\":[\"ls\"]}", now);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "\"code\":\"unsupported\""), "no job: %s", out);
    rdof_bl_closed(bl, c, now);
    c = ask(bl, "{\"op\":\"run\",\"job\":{\"tool\":\"bench_tx\",\"cmd\":\"voice\",\"freq\":438.1125,"
                "\"family\":\"xx\"}}", now);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "\"code\":\"unsupported\"") && strstr(out, "--family"), "a bad option: %s", out);
    rdof_bl_closed(bl, c, now);

    /* a listen job on the TX frequency's channel (1), then two voice calls with a talker alias heard by it */
    const int cl = ask(bl, "{\"op\":\"run\",\"job\":{\"tool\":\"bench_tx\",\"cmd\":\"listen\",\"freq\":438.1125,"
                           "\"seconds\":5,\"json\":true}}", now);
    const int ct = ask(bl, "{\"op\":\"run\",\"job\":{\"tool\":\"bench_tx\",\"cmd\":\"voice\",\"freq\":438.1125,\"cc\":2,"
                           "\"power\":32,\"src\":9990301,\"dst\":9990199,\"private\":true,\"superframes\":2,"
                           "\"calls\":2,\"talker_alias\":\"BENCH\"}}", now);
    /* a second voice job waits for the first */
    const int cw = ask(bl, "{\"op\":\"run\",\"job\":{\"tool\":\"bench_tx\",\"cmd\":\"csbk\",\"freq\":438.1125,"
                           "\"count\":3}}", now);
    size_t no = 0, nl = 0, nw = 0;
    static char ow[65536];
    bool overlap = false;
    for (int i = 0; i < 700 && !(rdof_bl_finished(bl, ct) && rdof_bl_finished(bl, cw) && rdof_bl_finished(bl, cl)); i++) {
        now += 0.01;
        rdof_bl_tick(bl, now);
        tx_air(bl, now, 1);
        no = drain_all(bl, ct, out, sizeof out, no);
        nl = drain_all(bl, cl, lis, sizeof lis, nl);
        nw = drain_all(bl, cw, ow, sizeof ow, nw);
        overlap |= strstr(ow, "\"started\"") && !strstr(out, "\"exit\"");
    }
    CHECK(is_json_lines(out) && strstr(out, "\"started\"") && strstr(out, "\"exit\":0"), "the voice job: %.300s", out);
    CHECK(strstr(out, "voice on 438.1125 MHz cc 2") && strstr(out, "power 32 noted"), "its log: %.600s", out);
    CHECK(count(out, "call from ") == 2 && strstr(out, "call 2 from 9990302 done"), "two calls: %.900s", out);
    CHECK(tx.cc == 2 && tx.freq == 438112500 && tx.power == 255, "the last job opened at %u cc %d power %d", tx.freq,
          tx.cc, tx.power);
    CHECK(strstr(ow, "\"state\":\"queued\"") && strstr(ow, "\"exit\":0") && !overlap, "the csbk job waited: %s", ow);
    CHECK(strstr(ow, "csbk: 3 burst(s) sent"), "the csbk job's log: %s", ow);
    CHECK(!tx.open, "the transmitter is given back");
    CHECK(is_json_lines(lis) && strstr(lis, "\"exit\":0"), "the listen job: %.300s", lis);
    CHECK(strstr(lis, "\\\"src\\\": 9990301") || strstr(lis, "9990301"), "heard the first call: %.600s", lis);
    CHECK(strstr(lis, "9990302") && strstr(lis, "BENCH"), "heard the second call and its alias: %.2000s",
          lis + (nl > 2000 ? nl - 2000 : 0));
    CHECK(!rdof_bl_wants_bursts(bl), "no bursts wanted after the jobs");
    rdof_bl_closed(bl, cl, now);
    rdof_bl_closed(bl, ct, now);
    rdof_bl_closed(bl, cw, now);

    /* closing the connection mid-call unkeys */
    c = ask(bl, "{\"op\":\"run\",\"job\":{\"tool\":\"bench_tx\",\"cmd\":\"voice\",\"freq\":438.1125,\"superframes\":5}}",
            now);
    for (int i = 0; i < 50; i++) {
        now += 0.01;
        rdof_bl_tick(bl, now);
        tx_air(bl, now, -1);
    }
    CHECK(tx.open && !tx.as_built, "keyed (%d), may be framed on the way (%d)", tx.open, tx.as_built);
    rdof_bl_closed(bl, c, now);
    CHECK(!tx.open, "closing gives the transmitter back");

    /* a frequency in one of the bench's TX bands, listed nowhere, is a transmit frequency; one past the band isn't */
    c = ask(bl, "{\"op\":\"run\",\"job\":{\"tool\":\"bench_tx\",\"cmd\":\"voice\",\"freq\":144.95,\"superframes\":5}}",
            now);
    for (int i = 0; i < 50; i++) {
        now += 0.01;
        rdof_bl_tick(bl, now);
        tx_air(bl, now, -1);
    }
    CHECK(tx.open && tx.freq == 144950000, "keyed in a band (%d) at %u", tx.open, tx.freq);
    rdof_bl_closed(bl, c, now);
    CHECK(!tx.open, "the band's job gives the transmitter back");
    c = ask(bl, "{\"op\":\"run\",\"job\":{\"tool\":\"bench_tx\",\"cmd\":\"voice\",\"freq\":148.0125}}", now);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "\"code\":\"refused\"") && strstr(out, "isn't a transmit frequency"), "past the band: %s", out);
    rdof_bl_closed(bl, c, now);
    /* headers, terminators or late entry: the bursts as built */
    static const char *const BUILT[] = {"\"headers\":0", "\"terminators\":2", "\"late\":2"};
    for (int k = 0; k < 3; k++) {
        char req[200];
        snprintf(req, sizeof req, "{\"op\":\"run\",\"job\":{\"tool\":\"bench_tx\",\"cmd\":\"voice\",\"freq\":438.1125,%s}}",
                 BUILT[k]);
        c = ask(bl, req, now);
        for (int i = 0; i < 20; i++) {
            now += 0.01;
            rdof_bl_tick(bl, now);
            tx_air(bl, now, -1);
        }
        CHECK(tx.open && tx.as_built, "%s: as built (%d %d)", BUILT[k], tx.open, tx.as_built);
        rdof_bl_closed(bl, c, now);
    }
    /* max_s ends a job */
    c = ask(bl, "{\"op\":\"run\",\"max_s\":0.5,\"job\":{\"tool\":\"bench_tx\",\"cmd\":\"voice\",\"freq\":438.1125,"
                "\"superframes\":5}}", now);
    no = 0;
    for (int i = 0; i < 100; i++) {
        now += 0.01;
        rdof_bl_tick(bl, now);
        tx_air(bl, now, -1);
        no = drain_all(bl, c, out, sizeof out, no);
    }
    CHECK(strstr(out, "\"exit\":-9") && strstr(out, "max_s") && !tx.open, "max_s: %.400s", out);
    rdof_bl_closed(bl, c, now);
    rdof_bl_free(bl);
}

/* ── bench_tx outbound and --family ── */

/* Each `"key": N` in the tool's out lines (JSON escaped in JSON) after p, into v (up to max): how many. */
static int out_values(const char *p, const char *key, double *v, int max)
{
    char pat[48];
    snprintf(pat, sizeof pat, "\\\"%s\\\": ", key);
    int n = 0;
    for (; (p = strstr(p, pat)) != NULL && n < max; p += strlen(pat))
        v[n++] = atof(p + strlen(pat));
    return n;
}

static int run_ob(rdof_bl_t *bl, const char *req, double *now, char *out, size_t cap)
{
    const int c = ask(bl, req, *now);
    size_t n = 0;
    for (int i = 0; i < 300 && !rdof_bl_finished(bl, c); i++) {
        *now += 0.01;
        ob.now = *now;
        rdof_bl_tick(bl, *now);
        n = drain_all(bl, c, out, cap, n);
    }
    return c;
}

static void test_outbound(void)
{
    static char out[1 << 18];
    static double sc[200], slot[200], nn[200];
    rdof_bl_t *bl = rdof_bl_new(&PLAT);
    double now = 4000;
    memset(&ob, 0, sizeof ob);
    memset(&tx, 0, sizeof tx);

    /* a family the routed path would frame again goes as built (the raw modem), not refused */
    routed = true;
    static const char *const FAM[] = {"{\"op\":\"run\",\"job\":{\"tool\":\"bench_tx\",\"cmd\":\"voice\",\"freq\":438.1125,"
                                      "\"family\":\"bs\"}}",
                                      "{\"op\":\"run\",\"job\":{\"tool\":\"bench_tx\",\"cmd\":\"csbk\",\"freq\":438.1125,"
                                      "\"family\":\"ts2\"}}"};
    int c = -1;
    for (int k = 0; k < 2; k++) {
        c = ask(bl, FAM[k], now);
        for (int i = 0; i < 20; i++) {
            now += 0.01;
            rdof_bl_tick(bl, now);
            tx_air(bl, now, -1);
        }
        CHECK(tx.open && tx.as_built, "%s: as built (%d %d)", FAM[k], tx.open, tx.as_built);
        rdof_bl_closed(bl, c, now);
    }
    c = ask(bl, "{\"op\":\"run\",\"job\":{\"tool\":\"bench_tx\",\"cmd\":\"data\",\"freq\":438.1125,"
                "\"family\":\"ms\"}}", now);
    drain(bl, c, out, sizeof out);
    CHECK(!strstr(out, "\"error\""), "MS goes through the router: %s", out);
    rdof_bl_closed(bl, c, now);
    routed = false;
    c = ask(bl, "{\"op\":\"run\",\"job\":{\"tool\":\"bench_tx\",\"cmd\":\"outbound\",\"freq\":438.1125,"
                "\"voice_slot\":1}}", now);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "\"code\":\"unsupported\"") && strstr(out, "--voice-slot"), "--voice-slot: %s", out);
    rdof_bl_closed(bl, c, now);
    c = ask(bl, "{\"op\":\"run\",\"job\":{\"tool\":\"bench_tx\",\"cmd\":\"outbound\",\"freq\":438.1125,"
                "\"family\":\"ms\"}}", now);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "\"code\":\"unsupported\"") && strstr(out, "--family ms"), "an MS outbound: %s", out);
    rdof_bl_closed(bl, c, now);
    c = ask(bl, "{\"op\":\"run\",\"job\":{\"tool\":\"bench_tx\",\"cmd\":\"outbound\",\"freq\":438.1125,"
                "\"seconds\":61}}", now);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "\"code\":\"unsupported\"") && strstr(out, "--seconds 0.5-60"), "61 s: %s", out);
    rdof_bl_closed(bl, c, now);
    c = ask(bl, "{\"op\":\"run\",\"job\":{\"tool\":\"bench_tx\",\"cmd\":\"outbound\",\"freq\":438.0625}}", now);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "\"code\":\"refused\""), "not a TX frequency: %s", out);
    rdof_bl_closed(bl, c, now);
    CHECK(!ob.open && !ob.polls, "nothing keyed");

    /* 1 s from TS1: 33 slots, a record each, 30 ms (1440 samples) apart, the slots alternating */
    ob.first = 8;
    c = run_ob(bl, "{\"op\":\"run\",\"job\":{\"tool\":\"bench_tx\",\"cmd\":\"outbound\",\"freq\":438.1125,"
                   "\"cc\":1,\"seconds\":1,\"power\":10}}", &now, out, sizeof out);
    CHECK(ob.cfg.freq_hz == 438112500 && ob.cfg.cc == 1 && ob.cfg.power == 10 && ob.cfg.seconds == 1,
          "its setup: %u cc %d power %d %.1f s", ob.cfg.freq_hz, ob.cfg.cc, ob.cfg.power, ob.cfg.seconds);
    CHECK(is_json_lines(out) && strstr(out, "\"started\"") && strstr(out, "\"exit\":0") && ob.closed && !ob.open &&
              ob.unkeyed, "the job: %.400s", out);
    CHECK(strstr(out, "outbound on 438.1125 MHz cc 1: BS idle bursts with the CACH on both slots, 33 slots"),
          "its log: %.800s", out);
    const char *st = strstr(out, "\\\"start\\\"");
    CHECK(st && strstr(st, "\\\"first_slot\\\": 1") && strstr(st, "\\\"period_samples\\\": 1440.0") &&
              strstr(st, "\\\"sc0\\\": 1012295") && strstr(st, "\\\"t0\\\": 1900000000.256146") &&
              strstr(st, "\\\"ppm\\\": -28.00"), "the start record: %.400s", st ? st : out);
    const char *recs = st ? strstr(st, "\n") : out;
    const int k = out_values(recs, "sc", sc, 200);
    out_values(recs, "slot", slot, 200);
    out_values(recs, "n", nn, 200);
    bool steady = k >= 33;
    for (int i = 1; i < 33; i++)
        steady &= sc[i] - sc[i - 1] == 1440 && nn[i] == i && slot[i] == (i % 2 ? 2 : 1);
    CHECK(steady && sc[0] == OB_BASE + 8 * 1440 + 775, "%d records, 1440 samples apart, TS1 first: %.0f %.0f", k, sc[0],
          sc[1]);
    const int idles = count(out, "\\\"kind\\\": \\\"IDLE\\\"");
    CHECK(idles >= 33 && idles <= 35 && count(out, "\\\"sync\\\": \\\"BS_DATA\\\"") == idles, "an idle burst each: %d",
          idles);
    const char *sum = strstr(out, "\\\"summary\\\"");
    CHECK(sum && strstr(sum, "\\\"skips\\\": 0") && strstr(sum, "\\\"sc0\\\": 1012295"), "the summary: %.300s",
          sum ? sum : "(none)");
    rdof_bl_closed(bl, c, now);

    /* TS2 only, the slot clock jumping 3 slots once: TS2's records, and the skip */
    ob.first = 7, ob.skip_at = 20, ob.skip_n = 3;
    c = run_ob(bl, "{\"op\":\"run\",\"job\":{\"tool\":\"bench_tx\",\"cmd\":\"outbound\",\"freq\":438.1125,"
                   "\"seconds\":1,\"slots\":\"2\"}}", &now, out, sizeof out);
    st = strstr(out, "\\\"start\\\"");
    const int k2 = out_values(st ? st : out, "slot", slot, 200);
    bool ts2 = k2 > 10;
    for (int i = 0; i < k2; i++)
        ts2 &= slot[i] == 2;
    CHECK(ts2 && st && strstr(st, "\\\"first_slot\\\": 2"), "TS2 only: %d records", k2);
    CHECK(strstr(out, "\\\"skip\\\": {\\\"from_n\\\": 13, \\\"to_n\\\": 16, \\\"slots\\\": 3") &&
              strstr(out, "\\\"skips\\\": 1"), "the skip: %.3000s", out);
    CHECK(strstr(out, "\"exit\":0") && ob.closed, "the TS2 job: %.300s", out);
    rdof_bl_closed(bl, c, now);

    /* closing mid-job gives the modem back */
    ob.skip_n = 0;
    c = ask(bl, "{\"op\":\"run\",\"job\":{\"tool\":\"bench_tx\",\"cmd\":\"outbound\",\"freq\":438.1125,"
                "\"seconds\":15}}", now);
    for (int i = 0; i < 20; i++) {
        ob.now = now += 0.01;
        rdof_bl_tick(bl, now);
    }
    CHECK(ob.open, "running");
    rdof_bl_closed(bl, c, now);
    CHECK(ob.closed && !ob.open, "closing ends the outbound");
    rdof_bl_free(bl);
}


/* ── bench_tx outbound with voice in its slots ── */

/* The embedded LC of a call's voice bursts B-E from slot i on (i a VOICE_B): true with lc9. */
static bool ob_emb_lc(long i, uint8_t lc9[9])
{
    uint8_t raw[128];
    for (int f = 0; f < 4; f++) {
        if (!ob.has[i + 2 * f])
            return false;
        const uint64_t mid = rdof_middle_field(ob.burst[i + 2 * f]);
        for (int b = 0; b < 32; b++)
            raw[32 * f + b] = (uint8_t)((mid >> (47 - 8 - b)) & 1);
    }
    return rdof_embedded_lc_decode(raw, lc9);
}

static int run_ob_long(rdof_bl_t *bl, const char *req, double *now, char *out, size_t cap)
{
    const int c = ask(bl, req, *now);
    size_t n = 0;
    for (int i = 0; i < 1000 && !rdof_bl_finished(bl, c); i++) {
        *now += 0.01;
        ob.now = *now;
        rdof_bl_tick(bl, *now);
        n = drain_all(bl, c, out, cap, n);
    }
    return c;
}

/* An outbound with a voice call in each slot, against the fake duplex modem's queues: what went out in each slot */
static void test_outbound_voice(void)
{
    static char out[1 << 19];
    double now = 5000;
    memset(&ob, 0, sizeof ob);

    /* refused before anything keys */
    rdof_bl_platform_t idle_only = PLAT;
    idle_only.ob_push = NULL;
    rdof_bl_t *bl = rdof_bl_new(&idle_only);
    int c = ask(bl, "{\"op\":\"run\",\"job\":{\"tool\":\"bench_tx\",\"cmd\":\"outbound\",\"freq\":438.1125,"
                    "\"seconds\":3,\"voice\":[{\"slot\":1}]}}", now);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "\"code\":\"unsupported\"") && strstr(out, "idle bursts only"), "no ob_push: %s", out);
    rdof_bl_closed(bl, c, now);
    rdof_bl_free(bl);
    bl = rdof_bl_new(&PLAT);
    static const struct {
        const char *voice, *why;
    } REFUSE[] = {
        {"[{\"slot\":3}]", "slot 1 or 2"},
        {"[]", "a list of 1-8 calls"},
        {"{\"slot\":1}", "a list of 1-8 calls"},
        {"[{\"slot\":1,\"superframes\":10}]", "past --seconds"},
        {"[{\"slot\":2,\"late\":2,\"headers\":2}]", "a late entry has no headers"},
        {"[{\"slot\":1},{\"slot\":2},{\"slot\":1,\"start_s\":0.5}]", "calls 1 and 3 overlap on slot 1"},
        {"[{\"slot\":1,\"ambe_hex\":\"zz\"}]", "ambe_hex"},
    };
    for (size_t r = 0; r < sizeof REFUSE / sizeof REFUSE[0]; r++) {
        char req[400];
        snprintf(req, sizeof req, "{\"op\":\"run\",\"job\":{\"tool\":\"bench_tx\",\"cmd\":\"outbound\","
                                  "\"freq\":438.1125,\"seconds\":3,\"voice\":%s}}", REFUSE[r].voice);
        c = ask(bl, req, now);
        drain(bl, c, out, sizeof out);
        CHECK(strstr(out, "\"code\":\"unsupported\"") && strstr(out, REFUSE[r].why), "%s: %s", REFUSE[r].voice, out);
        rdof_bl_closed(bl, c, now);
    }
    CHECK(!ob.open && !ob.pushes, "nothing keyed");

    /* TS1 from 0.2 s (2 headers, 2 superframes, 2 terminators: 16 bursts, to 1.16 s); TS2 a late entry from 0.8 s,
     * while TS1 talks, from voice burst C (2 superframes less 2, 1 terminator: 11 bursts, to 1.46 s): TS1 ends while
     * TS2 goes on */
    ob.first = 8;
    c = run_ob_long(bl, "{\"op\":\"run\",\"job\":{\"tool\":\"bench_tx\",\"cmd\":\"outbound\",\"freq\":438.1125,"
                        "\"cc\":3,\"seconds\":2,\"voice\":["
                        "{\"slot\":1,\"src\":9990301,\"dst\":91,\"start_s\":0.2,\"superframes\":2,\"headers\":2,"
                        "\"terminators\":2},"
                        "{\"slot\":2,\"src\":9990302,\"dst\":92,\"group\":false,\"start_s\":0.8,\"superframes\":2,"
                        "\"late\":2,\"terminators\":1}]}}", &now, out, sizeof out);
    CHECK(is_json_lines(out) && strstr(out, "\"exit\":0") && ob.closed && !ob.overflow, "the job: %.600s", out);
    CHECK(ob.pushes == 27, "27 bursts pushed: %d", ob.pushes);
    /* slot index i (from the first, TS1): TS1 even, TS2 odd */
    const long k1 = 7, k2 = 27; /* round(0.2 / 0.03) = 7 -> 8 (TS1: even); round(0.8 / 0.03) = 27 (TS2: odd) */
    int n1 = 0, n2 = 0, stray = 0, bad = 0;
    for (long i = 0; i < 2048; i++) {
        if (!ob.has[i])
            continue;
        int cc, dt, e;
        const bool data = rdof_sync_errors(ob.burst[i], rdof_data_sync(RDOF_BS_VOICE)) == 0;
        const bool voice = rdof_sync_errors(ob.burst[i], RDOF_BS_VOICE) == 0;
        if (i % 2 == 0 && i >= k1 + 1 && i < k1 + 1 + 32) {
            const long b = (i - k1 - 1) / 2;
            n1++;
            if (b < 2)
                bad += !(data && rdof_decode_slot_type(ob.burst[i], &cc, &dt, &e) && cc == 3 &&
                         dt == RDOF_DT_VOICE_LC_HEADER && ob.control[i] == (RDOF_SYNC_DATA | RDOF_DT_VOICE_LC_HEADER));
            else if (b < 14)
                bad += ((b - 2) % 6 == 0) != voice || ob.control[i] != ((b - 2) % 6 == 0 ? RDOF_SYNC_AUDIO : (b - 2) % 6);
            else
                bad += !(data && rdof_decode_slot_type(ob.burst[i], &cc, &dt, &e) && cc == 3 &&
                         dt == RDOF_DT_TERMINATOR_WITH_LC);
        } else if (i % 2 == 1 && i >= k2 && i < k2 + 22) {
            const long b = (i - k2) / 2;
            n2++;
            if (b < 10) { /* C D E F, then A-F */
                const int v = (int)((b + 2) % 6);
                bad += (v == 0) != voice || ob.control[i] != (v == 0 ? RDOF_SYNC_AUDIO : v);
                if (v != 0) {
                    int pi, lcss;
                    bad += !(rdof_decode_emb(ob.burst[i], &cc, &pi, &lcss, &e) && cc == 3);
                }
            } else {
                bad += !(data && rdof_decode_slot_type(ob.burst[i], &cc, &dt, &e) && dt == RDOF_DT_TERMINATOR_WITH_LC);
            }
        } else {
            stray++;
        }
    }
    CHECK(n1 == 16 && n2 == 11 && !stray && !bad, "TS1 %d of 16 at slots %ld.., TS2 %d of 11 at %ld.., %d stray, %d bad",
          n1, k1 + 1, n2, k2, stray, bad);
    CHECK(!ob.has[k2 - 2] && ob.has[k2] && ob.has[k1 + 1 + 30] && ob.has[k2 + 20] && !ob.has[k2 + 22],
          "TS2 starts mid TS1 call, TS1 ends first, idle after");
    /* the embedded LC: each call's own */
    uint8_t lc[9];
    CHECK(ob_emb_lc(k1 + 1 + 2 * 3, lc) && lc[0] == RDOF_FLCO_GROUP && lc[5] == 91 &&
              ((uint32_t)lc[6] << 16 | lc[7] << 8 | lc[8]) == 9990301, "TS1's LC: %02x .. %u", lc[0], lc[5]);
    CHECK(ob_emb_lc(k2 + 2 * 5, lc) && lc[0] == RDOF_FLCO_PRIVATE && lc[5] == 92 &&
              ((uint32_t)lc[6] << 16 | lc[7] << 8 | lc[8]) == 9990302, "TS2's LC (its second superframe's B): %02x %u",
          lc[0], lc[5]);
    /* the records: a voice burst's in its slot, each call's summary, the voice counts */
    CHECK(count(out, "\\\"kind\\\": \\\"VLC_HDR\\\"") == 2 && count(out, "\\\"kind\\\": \\\"TERM_LC\\\"") == 3 &&
              count(out, "\\\"kind\\\": \\\"VOICE_A\\\"") == 3 && count(out, "\\\"kind\\\": \\\"VOICE_C\\\"") == 4,
          "the bursts' records: %.2000s", out);
    char want[160];
    snprintf(want, sizeof want, "\\\"n\\\": %ld, \\\"slot\\\": 2, \\\"sync\\\": \\\"EMB\\\", \\\"kind\\\": \\\"VOICE_C\\\", "
                                "\\\"cc\\\": 3, \\\"call\\\": 2, \\\"src\\\": 9990302, \\\"dst\\\": 92, \\\"burst\\\": 0", k2);
    CHECK(strstr(out, want), "TS2's first record: %s", want);
    CHECK(strstr(out, "\\\"call\\\": {\\\"index\\\": 1, \\\"slot\\\": 1, \\\"src\\\": 9990301, \\\"dst\\\": 91, "
                      "\\\"group\\\": true, \\\"first_n\\\": 8, \\\"last_n\\\": 38, \\\"bursts\\\": 16, \\\"late\\\": 0, "
                      "\\\"late_entry\\\": false}") &&
              strstr(out, "\\\"late_entry\\\": true"), "the calls' records: %.3000s", out);
    CHECK(strstr(out, "\\\"voice\\\": {\\\"calls\\\": 2, \\\"ts1\\\": 16, \\\"ts2\\\": 11, \\\"late\\\": 0}"),
          "the summary: %.300s", strstr(out, "summary") ? strstr(out, "summary") : out);
    CHECK(strstr(out, "call 2 on TS2 from 9990302 to ID 92: 11 bursts from slot n=27 (0.810 s), late entry"),
          "the log: %.2000s", out);
    rdof_bl_closed(bl, c, now);

    /* from 0 s: the first slot still to be queued, not one already gone */
    ob.first = 9;
    c = run_ob_long(bl, "{\"op\":\"run\",\"job\":{\"tool\":\"bench_tx\",\"cmd\":\"outbound\",\"freq\":438.1125,"
                        "\"seconds\":1,\"voice\":[{\"slot\":1,\"superframes\":1,\"headers\":0,\"terminators\":0}]}}",
                    &now, out, sizeof out);
    int first = -1, nv = 0;
    for (long i = 0; i < 2048; i++)
        if (ob.has[i]) {
            nv++;
            if (first < 0)
                first = (int)i;
        }
    CHECK(strstr(out, "\"exit\":0") && nv == 6 && first == 3 && ob.control[first] == RDOF_SYNC_AUDIO,
          "a late entry from 0 s: %d bursts from slot %d (TS1 = odd here)", nv, first);
    rdof_bl_closed(bl, c, now);
    rdof_bl_free(bl);
}

/* ── over TCP ── */

/* ── bench_repeater jobs ── */

static void test_repeater(void)
{
    static char out[1 << 16];
    rdof_bl_t *bl = rdof_bl_new(&PLAT);
    double now = 3000;
    memset(&rp, 0, sizeof rp);
    /* refused: the input isn't a receiver channel, an option it can't honor, one frequency */
    int c = ask(bl, "{\"op\":\"run\",\"job\":{\"tool\":\"bench_repeater\",\"rx\":438.6,\"tx\":438.1125}}", now);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "\"code\":\"refused\"") && strstr(out, "input"), "no receiver at the input: %s", out);
    rdof_bl_closed(bl, c, now);
    c = ask(bl, "{\"op\":\"run\",\"job\":{\"tool\":\"bench_repeater\",\"rx\":438.0625,\"tx\":438.1125,"
                "\"rx_offset\":120}}", now);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "\"code\":\"unsupported\"") && strstr(out, "--rx-offset"), "an offset: %s", out);
    rdof_bl_closed(bl, c, now);
    c = ask(bl, "{\"op\":\"run\",\"job\":{\"tool\":\"bench_repeater\",\"rx\":438.1125,\"tx\":438.1125}}", now);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "separate RX and TX"), "one frequency: %s", out);
    rdof_bl_closed(bl, c, now);

    /* a repeater with beacons: a radio wakes it and calls through it */
    c = ask(bl, "{\"op\":\"run\",\"job\":{\"tool\":\"bench_repeater\",\"rx\":438.0625,\"tx\":438.1125,\"cc\":3,"
                "\"seconds\":3,\"json\":true,\"hang\":4,\"call_hang\":2,\"slots\":\"1,2\",\"beacon\":1.5,"
                "\"beacon_len\":0.5,\"beacon_phase\":0.2}}", now);
    size_t n = 0;
    bool heard = false;
    for (int i = 0; i < 500 && !rdof_bl_finished(bl, c); i++) {
        now += 0.01;
        rdof_bl_tick(bl, now);
        if (rp.open && rp.polls > 2 && !heard && now > 3000.5) { /* a radio: BS_Dwn_Act, the key, a voice header */
            heard = true;
            rdof_bl_rpt_ev_t *e = &rp.ev[rp.nev++];
            *e = (rdof_bl_rpt_ev_t){.kind = RDOF_BL_RPT_HEARD, .slot = 2, .rssi = -70};
            e->control = rdof_bj_rpt_wake_burst(3, e->burst);
            rp.ev[rp.nev++] = (rdof_bl_rpt_ev_t){.kind = RDOF_BL_RPT_KEY, .keyed = true};
            rdof_bj_job_t j = {.cmd = RDOF_BJ_VOICE, .cc = 3, .burst_cc = 3, .src = 9990301, .dst = 91,
                               .family = RDOF_FAMILY_MS, .superframes = 1, .calls = 1, .headers = 1,
                               .terminators = 1};
            rdof_bj_burst_t b[64];
            char note[200], done[200], err[200];
            const int nb = rdof_bj_build(&j, 0, b, 64, note, done, sizeof note, err, sizeof err);
            for (int k = 0; k < nb && rp.nev < 60; k++) {
                rp.ev[rp.nev] = (rdof_bl_rpt_ev_t){.kind = RDOF_BL_RPT_HEARD, .slot = 2, .control = b[k].control};
                memcpy(rp.ev[rp.nev++].burst, b[k].burst, 33);
            }
            rp.ev[rp.nev++] = (rdof_bl_rpt_ev_t){.kind = RDOF_BL_RPT_LOST, .slot = 2};
            rp.ev[rp.nev++] = (rdof_bl_rpt_ev_t){.kind = RDOF_BL_RPT_KEY, .keyed = false};
        }
        n = drain_all(bl, c, out, sizeof out, n);
    }
    CHECK(rp.cfg.rx_hz == 438062500 && rp.cfg.tx_hz == 438112500 && rp.cfg.cc == 3 && rp.cfg.slots == 6 &&
              rp.cfg.hang_s == 0.5 && rp.cfg.call_hang_s == 2, "its setup: %u %u cc %d slots %u hang %.1f %.1f",
          rp.cfg.rx_hz, rp.cfg.tx_hz, rp.cfg.cc, rp.cfg.slots, rp.cfg.hang_s, rp.cfg.call_hang_s);
    CHECK(is_json_lines(out) && strstr(out, "\"started\"") && strstr(out, "\"exit\":0") && rp.closed && !rp.open,
          "the job: %.300s", out);
    CHECK(strstr(out, "rfg repeater RX 438.0625 TX 438.1125 MHz cc 3"), "its log: %.600s", out);
    CHECK(strstr(out, "\\\"wakeup\\\": 0, \\\"slot\\\": 2, \\\"while_down\\\": true"), "the wakeup: %.2000s", out);
    CHECK(strstr(out, "\\\"src\\\": 9990301, \\\"dst\\\": 91, \\\"flco\\\": 0"), "the header's LC: %.2000s", out);
    CHECK(strstr(out, "\\\"key\\\": true") && strstr(out, "\\\"key\\\": false") && strstr(out, "\\\"lost\\\": 2"),
          "key and lost: %.2000s", out);
    CHECK(rp.wakes >= 1 && rp.wake_control == (RDOF_SYNC_DATA | RDOF_DT_CSBK | RDOF_CONTROL_IDLE) &&
              strstr(out, "\\\"beacon\\\": 1, \\\"len_s\\\": 0.5"), "a beacon: %d wakes", rp.wakes);
    CHECK(strstr(out, "\\\"skipped\\\": \\\"keyed\\\"") || rp.wakes == 2, "the second beacon: %d wakes", rp.wakes);
    const char *sum = strstr(out, "\\\"summary\\\"");
    CHECK(sum && strstr(sum, "\\\"2\\\": {\\\"dt2\\\": 1, \\\"dt3\\\": 1, \\\"voice\\\": 6, \\\"wakeups\\\": 1}"),
          "the summary: %s", sum ? sum : "(none)");
    rdof_bl_closed(bl, c, now);
    /* closing mid-job gives the transmitter back */
    c = ask(bl, "{\"op\":\"run\",\"job\":{\"tool\":\"bench_repeater\",\"rx\":438.0625,\"tx\":438.1125,\"seconds\":30}}",
            now);
    for (int i = 0; i < 20; i++)
        rdof_bl_tick(bl, now += 0.01);
    CHECK(rp.open, "running");
    rdof_bl_closed(bl, c, now);
    CHECK(rp.closed && !rp.open, "closing ends the repeater");
    rdof_bl_free(bl);
}

static double mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int dial(int port)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa)) {
        close(fd);
        return -1;
    }
    struct timeval tv = {3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return fd;
}

/* One line from fd (without the newline). */
static bool readline(int fd, char *out, size_t cap)
{
    size_t k = 0;
    while (k + 1 < cap) {
        char ch;
        if (recv(fd, &ch, 1, 0) != 1)
            return false;
        if (ch == '\n')
            break;
        out[k++] = ch;
    }
    out[k] = 0;
    return true;
}

static void test_tcp(void)
{
    rdof_bl_t *bl = rdof_bl_new(&PLAT);
    char err[128], line[8192];
    rdof_bnet_t *n = rdof_bnet_start(bl, "127.0.0.1", 0, mono, 0, err, sizeof err);
    CHECK(n && rdof_bnet_port(n) > 0, "the server starts: %s", n ? "" : err);
    if (!n) {
        rdof_bl_free(bl);
        return;
    }
    const int port = rdof_bnet_port(n);
    /* a session, held open, and a claim in it over another connection */
    const int s = dial(port);
    const char *req = "{\"op\":\"session\",\"label\":\"tcp\"}\n";
    send(s, req, strlen(req), 0);
    char id[16] = "";
    CHECK(readline(s, line, sizeof line) && field(line, "session", id, sizeof id), "session over TCP: %s", line);
    int c = dial(port);
    char r2[160];
    snprintf(r2, sizeof r2, "{\"op\":\"claim\",\"session\":\"%s\",\"freqs\":[438.1625]}\n", id);
    send(c, r2, strlen(r2), 0);
    CHECK(readline(c, line, sizeof line) && strstr(line, "\"claimed\":[438.162500]"), "claim over TCP: %s", line);
    char ch;
    CHECK(recv(c, &ch, 1, 0) == 0, "the server closes a finished request");
    close(c);
    /* closing the session's connection ends it */
    close(s);
    usleep(50000);
    c = dial(port);
    send(c, "{\"op\":\"status\"}\n", 16, 0);
    CHECK(readline(c, line, sizeof line) && strstr(line, "\"sessions\":[]"), "the session ended with its socket: %s",
          line);
    close(c);
    /* a capture's binary frames come through whole */
    rdof_bl_ring_init(&ring, 768000);
    float iq[2000];
    for (int k = 0; k < 2000; k++)
        iq[k] = -0.25f;
    for (int k = 0; k < 20; k++) /* pre_s takes at most half of what's kept */
        rdof_bl_ring_put(&ring, iq, 1000, NULL);
    c = dial(port);
    const char *cap = "{\"op\":\"capture\",\"freq\":438.0625,\"seconds\":0.02,\"pre_s\":0.01}\n";
    send(c, cap, strlen(cap), 0);
    CHECK(readline(c, line, sizeof line) && strstr(line, "\"ready\""), "capture over TCP: %s", line);
    for (int k = 0; k < 20; k++)
        rdof_bl_ring_put(&ring, iq, 1000, NULL);
    long samples = 0;
    bool good = true, done = false;
    while (readline(c, line, sizeof line)) {
        char b[32];
        if (field(line, "bytes", b, sizeof b)) {
            long nb = atol(b);
            while (nb > 0) {
                char buf[4096];
                const ssize_t k = recv(c, buf, nb < (long)sizeof buf ? (size_t)nb : sizeof buf, 0);
                if (k <= 0) {
                    good = false;
                    break;
                }
                for (ssize_t i = 0; i < k; i++)
                    good &= (int8_t)buf[i] == -32; /* char is unsigned on aarch64 Linux */
                nb -= k, samples += k / 2;
            }
        } else if (strstr(line, "\"done\"")) {
            done = true;
        }
    }
    CHECK(done && good && samples == (long)(0.03 * 768000), "%ld samples over TCP (good %d, done %d)", samples, good,
          done);
    close(c);

    /* several clients at once: three watch one channel (two bursts, one listen) and a capture runs beside them, while
     * two sessions queue for the modem; each gets all of its own */
    int w[3];
    const char *wreq[3] = {"{\"op\":\"bursts\",\"freq\":438.1125,\"seconds\":5,\"max_bursts\":5}\n",
                           "{\"op\":\"bursts\",\"freq\":438.1125,\"seconds\":5,\"max_bursts\":5}\n",
                           "{\"op\":\"listen\",\"freq\":438.1125,\"seconds\":1}\n"};
    for (int k = 0; k < 3; k++) {
        w[k] = dial(port);
        send(w[k], wreq[k], strlen(wreq[k]), 0);
    }
    for (int k = 0; k < 2; k++)
        CHECK(readline(w[k], line, sizeof line) && strstr(line, "\"ready\""), "watcher %d ready: %s", k, line);
    const int cap2 = dial(port);
    const char *creq = "{\"op\":\"capture\",\"freq\":438.0625,\"seconds\":0.01,\"pre_s\":0.005}\n";
    send(cap2, creq, strlen(creq), 0);
    int sess[2];
    char sid[2][16];
    for (int k = 0; k < 2; k++) {
        sess[k] = dial(port);
        send(sess[k], req, strlen(req), 0);
        readline(sess[k], line, sizeof line);
        field(line, "session", sid[k], sizeof sid[k]);
    }
    usleep(50000);
    for (int i = 0; i < 5; i++) {
        rdof_bl_burst_t b = {.ch = 1, .t = 1.8e9 + 0.06 * i, .dt = RDOF_BL_VOICE_SYNC, .family = 1, .cc = 1};
        rdof_bl_burst(bl, &b);
        rdof_bl_ring_put(&ring, iq, 1000, NULL);
        rdof_bl_ring_put(&ring, iq, 1000, NULL); /* 10 ms more in all than the capture asks */
    }
    for (int k = 0; k < 3; k++) {
        int got = 0;
        while (readline(w[k], line, sizeof line))
            got += strstr(line, "\"burst\"") != NULL;
        CHECK(got == 5, "watcher %d got %d bursts of 5", k, got);
        close(w[k]);
    }
    bool cdone = false;
    while (readline(cap2, line, sizeof line)) {
        char b[32];
        if (field(line, "bytes", b, sizeof b)) {
            char buf[20000];
            long nb = atol(b);
            while (nb > 0) {
                const ssize_t k = recv(cap2, buf, nb < (long)sizeof buf ? (size_t)nb : sizeof buf, 0);
                if (k <= 0)
                    break;
                nb -= k;
            }
        }
        cdone |= strstr(line, "\"done\"") != NULL;
    }
    CHECK(cdone, "the capture beside them finished");
    close(cap2);
    int hd[2];
    for (int k = 0; k < 2; k++) {
        char hreq[160];
        snprintf(hreq, sizeof hreq, "{\"op\":\"hold\",\"session\":\"%s\",\"wait_max\":10}\n", sid[k]);
        hd[k] = dial(port);
        send(hd[k], hreq, strlen(hreq), 0);
        usleep(30000);
    }
    CHECK(readline(hd[0], line, sizeof line) && strstr(line, "\"held\""), "the first session holds the modem: %s",
          line);
    CHECK(readline(hd[1], line, sizeof line) && strstr(line, "\"queued\""), "the second waits: %s", line);
    close(sess[0]); /* the first session ends: the modem passes on */
    CHECK(readline(hd[1], line, sizeof line) && strstr(line, "\"granted\"") && readline(hd[1], line, sizeof line) &&
              strstr(line, "\"held\""),
          "the second gets it: %s", line);
    close(hd[0]), close(hd[1]), close(sess[1]);
    rdof_bnet_stop(n);
    rdof_bl_ring_free(&ring);
    rdof_bl_free(bl);
}

/* `rdof_benchline_tests serve PORT SECONDS`: the fake receiver behind a real port for the client
 * (tools/bench/rfgbench.py), a voice burst on CH2 every 60 ms and noise in the I/Q. */
static int serve(int port, double seconds)
{
    rdof_bl_t *bl = rdof_bl_new(&PLAT);
    char err[128];
    rdof_bnet_t *n = rdof_bnet_start(bl, "127.0.0.1", port, mono, 0, err, sizeof err);
    if (!n) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    rdof_bl_ring_init(&ring, 768000 * 3);
    printf("serving on %d\n", rdof_bnet_port(n));
    fflush(stdout);
    float iq[2 * 7680];
    const double t0 = mono();
    for (long k = 0; mono() - t0 < seconds; k++) {
        for (int i = 0; i < 2 * 7680; i++)
            iq[i] = (float)((rand() % 200) - 100) / 400.0f;
        rdof_bl_ring_put(&ring, iq, 7680, NULL);
        tx_air(bl, mono(), 1); /* a run's bursts, heard back on 438.1125 */
        if (k % 6 == 0 && !tx.open) {
            rdof_bl_burst_t b = {.ch = 1, .t = 1.8e9 + 0.01 * k, .dt = k % 36 ? RDOF_BL_VOICE : RDOF_BL_VOICE_SYNC, .n = (int8_t)(k / 6 % 6),
                                 .slot = 1, .family = 1, .cc = 1, .level_dbfs = -40};
            rdof_bl_burst(bl, &b);
        }
        usleep(10000);
    }
    rdof_bnet_stop(n);
    rdof_bl_ring_free(&ring);
    rdof_bl_free(bl);
    return 0;
}

/* Tick until c has something to say (at most n ticks, 20 ms apart). */
static void tick_for(rdof_bl_t *bl, int c, double *now, int n, char *out, size_t cap)
{
    out[0] = 0;
    for (int i = 0; i < n && !out[0]; i++) {
        *now += 0.02;
        rdof_bl_tick(bl, *now);
        drain(bl, c, out, cap);
    }
}

/* tune: the session's own window until it ends or releases it; others busy meanwhile; a trigger outside the window
 * tunes for itself */
static void test_tune(void)
{
    rdof_bl_platform_t pt = PLAT;
    pt.tune = f_tune, pt.tune_state = f_tune_state, pt.untune = f_untune;
    rdof_bl_t *bl = rdof_bl_new(&pt);
    rdof_bl_ring_init(&ring, 768000);
    char out[65536], req[512], ida[16] = "", idb[16] = "";
    double now = 100;

    int c = ask(bl, "{\"op\": \"info\"}", now);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "\"tune\":null") && strstr(out, "\"center\":438.225000"), "untuned: %s", out);
    rdof_bl_closed(bl, c, now);
    const int sa = ask(bl, "{\"op\": \"session\", \"label\": \"md9600\"}", now);
    drain(bl, sa, out, sizeof out);
    field(out, "session", ida, sizeof ida);
    const int sb = ask(bl, "{\"op\": \"session\", \"label\": \"other\"}", now);
    drain(bl, sb, out, sizeof out);
    field(out, "session", idb, sizeof idb);

    /* the requests a tune refuses at once */
    c = ask(bl, "{\"op\": \"tune\", \"center\": 145.05}", now);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "\"code\":\"bad_request\"") && rdof_bl_finished(bl, c), "no session: %s", out);
    rdof_bl_closed(bl, c, now);
    const char *bad[] = {"\"center\": \"high\"", "\"gain\": [1]", "\"taps\": 145.05", "\"taps\": [\"x\"]",
                         "\"taps\": [1,2,3,4,5,6,7,8,9]"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        snprintf(req, sizeof req, "{\"op\": \"tune\", \"session\": \"%s\", %s}", ida, bad[i]);
        c = ask(bl, req, now);
        drain(bl, c, out, sizeof out);
        CHECK(strstr(out, "\"code\":\"bad_request\""), "%s: %s", bad[i], out);
        rdof_bl_closed(bl, c, now);
    }
    snprintf(req, sizeof req, "{\"op\": \"tune\", \"session\": \"%s\", \"rate\": 1e6}", ida);
    c = ask(bl, req, now);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "\"code\":\"unsupported\""), "a rate: %s", out);
    rdof_bl_closed(bl, c, now);
    snprintf(req, sizeof req, "{\"op\": \"tune\", \"session\": \"%s\", \"taps\": [145.05, 145.1875, 145.2875, 145.3875, 145.4875]}",
             ida);
    c = ask(bl, req, now);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "\"code\":\"bad_request\"") && strstr(out, "4 taps at most") && ft.tunes == 0,
          "the platform's refusal: %s", out);
    rdof_bl_closed(bl, c, now);
    ft.busy = "busy";
    snprintf(req, sizeof req, "{\"op\": \"tune\", \"session\": \"%s\", \"center\": 145.05}", ida);
    c = ask(bl, req, now);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "\"code\":\"busy\"") && strstr(out, "HTTP"), "an HTTP lease holds it: %s", out);
    rdof_bl_closed(bl, c, now);
    ft.busy = NULL;

    /* a's bursts stream on a channel of the settings' window: the tune ends it */
    c = ask(bl, "{\"op\": \"bursts\", \"freq\": 438.1125, \"seconds\": 30}", now);
    const int bursts = c;
    drain(bl, c, out, sizeof out);

    /* a tunes: the reply once it applies, with the window and its channels */
    snprintf(req, sizeof req,
             "{\"op\": \"tune\", \"session\": \"%s\", \"center\": 144.9, \"gain\": 30, \"taps\": [145.1]}", ida);
    const int t = ask(bl, req, now);
    drain(bl, t, out, sizeof out);
    CHECK(!out[0] && ft.tunes == 1 && fabs(ft.asked.center_hz - 144.9e6) < 1 && ft.asked.gain_db == 30 &&
              ft.asked.ntaps == 1 && fabs(ft.asked.taps_hz[0] - 145.1e6) < 1 && !strcmp(ft.who, ida),
          "asked: %s, center %.0f", out, ft.asked.center_hz);
    c = ask(bl, "{\"op\": \"info\"}", now);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "\"state\":\"tuning\""), "info while it's being applied: %s", out);
    rdof_bl_closed(bl, c, now);
    snprintf(req, sizeof req, "{\"op\": \"tune\", \"session\": \"%s\"}", idb);
    c = ask(bl, req, now);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "\"code\":\"busy\""), "another tune while one is applied: %s", out);
    rdof_bl_closed(bl, c, now);
    tick_for(bl, t, &now, 10, out, sizeof out);
    CHECK(strstr(out, "{\"tuned\":{\"session\":\"") && strstr(out, "\"center\":144.900000") &&
              strstr(out, "\"gain\":30.0") &&
              strstr(out, "\"channels\":[{\"freq\":145.100000,\"ch\":0,\"id\":\"T1\"}]") && rdof_bl_finished(bl, t),
          "tuned: %s", out);
    rdof_bl_closed(bl, t, now);
    drain(bl, bursts, out, sizeof out);
    CHECK(strstr(out, "\"reason\":\"retuned\"") && rdof_bl_finished(bl, bursts), "the bursts stream ends: %s", out);
    rdof_bl_closed(bl, bursts, now);
    c = ask(bl, "{\"op\": \"info\"}", now);
    drain(bl, c, out, sizeof out);
    snprintf(req, sizeof req,
             "\"tune\":{\"session\":\"%s\",\"label\":\"md9600\",\"for\":\"session\",\"state\":\"tuned\"}", ida);
    CHECK(strstr(out, req) && strstr(out, "\"center\":144.900000") && strstr(out, "{\"freq\":145.100000,\"ch\":0"),
          "info shows the tune: %s", out);
    rdof_bl_closed(bl, c, now);

    /* b meanwhile: its own tune busy; outside the window busy; inside it a capture runs */
    snprintf(req, sizeof req, "{\"op\": \"tune\", \"session\": \"%s\", \"center\": \"auto\"}", idb);
    c = ask(bl, req, now);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "\"code\":\"busy\"") && strstr(out, "(md9600)"), "b's tune: %s", out);
    rdof_bl_closed(bl, c, now);
    const char *others[] = {"bursts\", \"freq\": 438.1125", "capture\", \"freq\": 438.1125, \"seconds\": 1",
                            "trigger\", \"freq\": 438.1125"};
    for (size_t i = 0; i < sizeof others / sizeof others[0]; i++) {
        snprintf(req, sizeof req, "{\"op\": \"%s, \"session\": \"%s\"}", others[i], idb);
        c = ask(bl, req, now);
        drain(bl, c, out, sizeof out);
        CHECK(strstr(out, "\"code\":\"busy\"") && strstr(out, "has tuned until it ends"), "b's %s: %s", others[i],
              out);
        rdof_bl_closed(bl, c, now);
    }
    snprintf(req, sizeof req, "{\"op\": \"capture\", \"session\": \"%s\", \"freq\": 145.1, \"seconds\": 1}", idb);
    const int cap = ask(bl, req, now);
    drain(bl, cap, out, sizeof out);
    CHECK(strstr(out, "{\"ready\":") && strstr(out, "\"center\":144.900000"), "b's capture in the window: %s", out);

    /* a tunes again (the gain back to the settings'): b's capture ends */
    snprintf(req, sizeof req, "{\"op\": \"tune\", \"session\": \"%s\", \"gain\": \"auto\"}", ida);
    c = ask(bl, req, now);
    tick_for(bl, c, &now, 10, out, sizeof out);
    CHECK(strstr(out, "\"tuned\"") && strstr(out, "\"center\":144.900000") && strstr(out, "\"gain\":20.0") &&
              strstr(out, "\"id\":\"T1\""),
          "a's second tune: %s", out);
    rdof_bl_closed(bl, c, now);
    drain(bl, cap, out, sizeof out);
    CHECK(strstr(out, "\"code\":\"busy\"") && strstr(out, "retuned"), "b's capture ends: %s", out);
    rdof_bl_closed(bl, cap, now);

    /* a releases its claims and hold: the tune stays; then the tune: the settings again */
    snprintf(req, sizeof req, "{\"op\": \"release\", \"session\": \"%s\", \"freqs\": [438.1125]}", ida);
    c = ask(bl, req, now);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "\"tune\":false") && ft.untunes == 0, "a release of a frequency: %s", out);
    rdof_bl_closed(bl, c, now);
    snprintf(req, sizeof req, "{\"op\": \"release\", \"session\": \"%s\", \"tune\": true}", idb);
    c = ask(bl, req, now);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "\"tune\":false") && ft.untunes == 0, "b can't release a's tune: %s", out);
    rdof_bl_closed(bl, c, now);
    snprintf(req, sizeof req, "{\"op\": \"release\", \"session\": \"%s\", \"tune\": true}", ida);
    c = ask(bl, req, now);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "\"tune\":true") && ft.untunes == 1, "a releases its tune: %s", out);
    rdof_bl_closed(bl, c, now);
    c = ask(bl, "{\"op\": \"capture\", \"freq\": 438.1125, \"seconds\": 1}", now);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "\"code\":\"busy\"") && strstr(out, "being retuned"), "while it goes back: %s", out);
    rdof_bl_closed(bl, c, now);
    for (int i = 0; i < 5; i++)
        rdof_bl_tick(bl, now += 0.02);
    c = ask(bl, "{\"op\": \"info\"}", now);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "\"tune\":null") && strstr(out, "\"center\":438.225000") && strstr(out, "\"id\":\"CH2\""),
          "the settings again: %s", out);
    rdof_bl_closed(bl, c, now);

    /* a refused tune: nobody's */
    ft.refuse = true;
    snprintf(req, sizeof req, "{\"op\": \"tune\", \"session\": \"%s\", \"taps\": [470]}", ida);
    c = ask(bl, req, now);
    tick_for(bl, c, &now, 10, out, sizeof out);
    CHECK(strstr(out, "\"code\":\"refused\"") && strstr(out, "outside the window"), "refused: %s", out);
    rdof_bl_closed(bl, c, now);
    c = ask(bl, "{\"op\": \"info\"}", now);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "\"tune\":null") && ft.untunes == 1, "nobody's after a refusal: %s", out);
    rdof_bl_closed(bl, c, now);

    /* the session's connection drops: its tune goes with it */
    snprintf(req, sizeof req, "{\"op\": \"tune\", \"session\": \"%s\", \"center\": 144.9}", ida);
    c = ask(bl, req, now);
    tick_for(bl, c, &now, 10, out, sizeof out);
    CHECK(strstr(out, "\"tuned\""), "tuned again: %s", out);
    rdof_bl_closed(bl, c, now);
    rdof_bl_closed(bl, sa, now);
    CHECK(ft.untunes == 2, "the session's end untunes (%d)", ft.untunes);
    for (int i = 0; i < 5; i++)
        rdof_bl_tick(bl, now += 0.02);
    CHECK(isnan(ft.center), "the settings' window");
    /* a session dropping while its tune is applied: the settings once it has */
    const int sc = ask(bl, "{\"op\": \"session\", \"label\": \"brief\"}", now);
    char idc[16] = "";
    drain(bl, sc, out, sizeof out);
    field(out, "session", idc, sizeof idc);
    snprintf(req, sizeof req, "{\"op\": \"tune\", \"session\": \"%s\", \"center\": 144.9}", idc);
    c = ask(bl, req, now);
    rdof_bl_closed(bl, sc, now);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "\"code\":\"refused\""), "the tune's request ends with the session: %s", out);
    rdof_bl_closed(bl, c, now);
    for (int i = 0; i < 8; i++)
        rdof_bl_tick(bl, now += 0.02);
    CHECK(isnan(ft.center) && ft.untunes == 3, "applied, then untuned (%d, %.0f)", ft.untunes, ft.center);

    /* a trigger outside the window, with a session: it tunes for itself, then the settings once it ends */
    c = ask(bl, "{\"op\": \"trigger\", \"freq\": 145.1, \"floor_db\": -60}", now);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "\"code\":\"refused\"") && strstr(out, "with a session"), "no session, no tune: %s", out);
    rdof_bl_closed(bl, c, now);
    snprintf(req, sizeof req, "{\"op\": \"trigger\", \"session\": \"%s\", \"freq\": 145.1, \"tune\": false}", idb);
    c = ask(bl, req, now);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "\"code\":\"refused\"") && ft.tunes == 5, "tune false: %s (%d)", out, ft.tunes);
    rdof_bl_closed(bl, c, now);
    c = ask(bl, "{\"op\": \"bursts\", \"freq\": 438.1125, \"seconds\": 30}", now);
    const int busy = c;
    snprintf(req, sizeof req, "{\"op\": \"trigger\", \"session\": \"%s\", \"freq\": 145.1, \"floor_db\": -60}", idb);
    c = ask(bl, req, now);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "\"code\":\"busy\"") && strstr(out, "another request"), "a stream uses the window: %s", out);
    rdof_bl_closed(bl, c, now);
    rdof_bl_closed(bl, busy, now);
    ft.restart = 3; /* the receiver comes back a few polls after the tune applies (a board's stream restarting) */
    const int tr = ask(bl, req, now);
    drain(bl, tr, out, sizeof out);
    CHECK(strstr(out, "{\"note\":\"tuning the receiver to 145.1000 MHz for this request\"}") && ft.tunes == 6 &&
              fabs(ft.asked.center_hz - (145.1e6 - 96000)) < 1 && ft.asked.ntaps == 1 && isnan(ft.asked.gain_db),
          "the trigger tunes: %s (center %.0f)", out, ft.asked.center_hz);
    tick_for(bl, tr, &now, 10, out, sizeof out);
    CHECK(strstr(out, "{\"armed\":{\"floor_db\":-60.0"), "then it's armed: %s", out);
    CHECK(ft.down == 0, "the receiver came back first (%d)", ft.down);
    ft.restart = 0;
    c = ask(bl, "{\"op\": \"info\"}", now);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "\"for\":\"request\",\"state\":\"tuned\"") && strstr(out, "\"center\":145.004000"),
          "info: the request's tune: %s", out);
    rdof_bl_closed(bl, c, now);
    snprintf(req, sizeof req, "{\"op\": \"tune\", \"session\": \"%s\"}", idb);
    c = ask(bl, req, now);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "\"code\":\"busy\"") && strstr(out, "has the receiver tuned"), "even its own session's: %s",
          out);
    rdof_bl_closed(bl, c, now);
    ft.restart = 3;
    rdof_bl_closed(bl, tr, now);
    CHECK(ft.untunes == 4, "the trigger's end untunes (%d)", ft.untunes);
    for (int i = 0; i < 2; i++)
        rdof_bl_tick(bl, now += 0.02);
    CHECK(isnan(ft.center) && ft.down > 0, "the settings again, the receiver restarting (%d)", ft.down);
    /* a request while it restarts waits for it, then runs */
    snprintf(req, sizeof req, "{\"op\": \"trigger\", \"session\": \"%s\", \"freq\": 438.1125, \"floor_db\": -60}", idb);
    c = ask(bl, req, now);
    drain(bl, c, out, sizeof out);
    CHECK(!out[0], "no answer yet: %s", out);
    tick_for(bl, c, &now, 10, out, sizeof out);
    CHECK(strstr(out, "{\"armed\":{\"floor_db\":-60.0") && ft.down == 0, "then it runs: %s (%d)", out, ft.down);
    ft.restart = 0;
    rdof_bl_closed(bl, c, now);
    rdof_bl_closed(bl, sb, now);
    rdof_bl_ring_free(&ring);
    rdof_bl_free(bl);

    /* a bench with no tune */
    bl = rdof_bl_new(&PLAT);
    const int s = ask(bl, "{\"op\": \"session\"}", now);
    drain(bl, s, out, sizeof out);
    field(out, "session", ida, sizeof ida);
    snprintf(req, sizeof req, "{\"op\": \"tune\", \"session\": \"%s\", \"center\": 144.9}", ida);
    c = ask(bl, req, now);
    drain(bl, c, out, sizeof out);
    CHECK(strstr(out, "\"code\":\"unsupported\""), "no tune here: %s", out);
    rdof_bl_closed(bl, c, now);
    rdof_bl_closed(bl, s, now);
    rdof_bl_free(bl);
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "serve"))
        return serve(atoi(argv[2]), atof(argv[3]));
    test_ops();
    test_trigger();
    test_xtal();
    test_track();
    test_filler();
    test_iq_formats();
    test_decode();
    test_late();
    test_run();
    test_repeater();
    test_outbound();
    test_outbound_voice();
    test_tune();
    test_tcp();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
