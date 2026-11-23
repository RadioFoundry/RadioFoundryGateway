// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The line protocol's signal modes (bench-api.md section 14.6): a tx op's source, rendered by a fake transmitter
 * into the fake receiver's wideband history, analyzed by an rx op on that history; a PCM stream's backpressure; the
 * refusals. */
#define _DEFAULT_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/benchline.h"
#include "fake_chs.h"
#include "rdof/jsonp.h"
#include "rdof/sdr/modes.h"

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

/* ── the fake bench: a 192 kS/s window at 438.1 MHz, 438.075 MHz received and sent ── */

#define FS 192000.0
#define CENTER 438.1e6
#define FREQ 438.075e6

static rdof_bl_ring_t ring;
static double clock_s;

static void f_window(void *ctx, rdof_bl_window_t *w)
{
    *w = (rdof_bl_window_t){.running = true, .center_hz = CENTER, .fs = FS, .lo_hz = 438.01e6, .hi_hz = 438.19e6,
                            .tx_fs = FS};
}
static int f_freqs(void *ctx, rdof_bl_freq_t *out, int max)
{
    out[0] = (rdof_bl_freq_t){.freq_hz = FREQ, .ch = 0, .caps = RDOF_BL_CAP_RX | RDOF_BL_CAP_IQ | RDOF_BL_CAP_TX};
    out[1] = (rdof_bl_freq_t){.freq_hz = 438.15e6, .ch = 1, .caps = RDOF_BL_CAP_RX | RDOF_BL_CAP_IQ};
    return 2;
}
static long f_iq_read(void *ctx, uint64_t sc, int8_t *out, long n, uint64_t *oldest, uint64_t *next)
{
    return rdof_bl_ring_read(&ring, sc, out, n, oldest, next);
}
static double f_utc(void *ctx) { return 1.9e9 + clock_s; }
static double f_sc_utc(void *ctx, uint64_t sc) { return 1.9e9 + (double)sc / FS; }
static const char *f_can_tx(void *ctx) { return NULL; }

/* the transmitter: it keeps the source; the test pulls it */
static struct {
    bool open, closed, done;
    uint32_t freq;
    int power;
    double power_dbm;
    rdof_sig_src_t src;
} tx;
static int busy_n; /* sig_open says not yet this many times first */
static int f_sig_open(void *ctx, uint32_t hz, rdof_bench_power_t power, double seconds, const rdof_sig_src_t *src,
                      char *why, size_t cap)
{
    if (busy_n > 0) {
        busy_n--;
        snprintf(why, cap, "the roamer is transmitting");
        return 0;
    }
    tx.open = true, tx.closed = tx.done = false, tx.freq = hz, tx.power = power.level, tx.power_dbm = power.dbm;
    tx.src = *src;
    snprintf(why, cap, "power %d as 21 dBm", power.level);
    return 1;
}
static int f_sig_state(void *ctx, char *why, size_t cap) { return tx.done ? 2 : 1; }
static void f_sig_close(void *ctx) { tx.open = false, tx.closed = true; }
static double f_sig_rate(void *ctx) { return FS; }

static const rdof_bl_platform_t PLAT = {.window = f_window, .freqs = f_freqs, .iq_read = f_iq_read, .utc = f_utc,
                                        .sc_utc = f_sc_utc, .can_tx = f_can_tx, .modes = rdof_sdr_modes,
                                        .sig_open = f_sig_open, .sig_state = f_sig_state, .sig_close = f_sig_close,
                                        .sig_rate = f_sig_rate};

/* ── the air: the source's frequency into the history at the channel's offset, with noise ── */

static double phase;
static uint32_t seed = 1;
static float gauss(void)
{
    double u = 0;
    for (int i = 0; i < 12; i++) {
        seed = seed * 1664525u + 1013904223u;
        u += (seed >> 8) / 16777216.0;
    }
    return (float)(u - 6);
}

/* n samples: the source's when it's keyed (amplitude 0.5: frequency and envelope, or I/Q), noise at -50 dBFS always */
static void air(int n, bool keyed)
{
    static float hz[4096], env[4096], iq[8192], x[8192];
    while (n > 0) {
        const int k = n < 4096 ? n : 4096;
        int got = 0;
        if (keyed && tx.open && tx.src.freq)
            got = tx.src.freq(tx.src.ctx, hz, env, k);
        else if (keyed && tx.open && tx.src.iq)
            got = tx.src.iq(tx.src.ctx, x, k);
        if (keyed && tx.open && got < k && tx.src.done && tx.src.done(tx.src.ctx))
            tx.done = true;
        const float sn = 0.00316f / sqrtf(2); /* -50 dBFS of complex noise */
        for (int i = 0; i < k; i++) {
            const double f = (i < got && tx.src.freq ? hz[i] : 0) + (FREQ - CENTER);
            phase += 2 * M_PI * f / FS;
            const float c = (float)cos(phase), s = (float)sin(phase);
            if (tx.src.freq || i >= got) {
                const float a = i < got ? 0.5f * env[i] : 0;
                iq[2 * i] = a * c + sn * gauss();
                iq[2 * i + 1] = a * s + sn * gauss();
            } else {
                const float xr = 0.5f * x[2 * i], xi = 0.5f * x[2 * i + 1];
                iq[2 * i] = xr * c - xi * s + sn * gauss();
                iq[2 * i + 1] = xr * s + xi * c + sn * gauss();
            }
        }
        long clipped = 0;
        rdof_bl_ring_put(&ring, iq, k, &clipped);
        clock_s += k / FS;
        n -= k;
    }
}

/* ── the client's side ── */

static char text[1 << 22];
static size_t ntext;

static void take(rdof_bl_t *bl, int c)
{
    size_t k;
    while (ntext + 1 < sizeof text && (k = rdof_bl_output(bl, c, (uint8_t *)text + ntext, sizeof text - 1 - ntext)))
        ntext += k;
    text[ntext] = 0;
}

static int ask(rdof_bl_t *bl, const char *req)
{
    const int c = rdof_bl_open(bl, clock_s);
    static char line[4096];
    snprintf(line, sizeof line, "%s\n", req);
    rdof_bl_input(bl, c, line, strlen(line), clock_s);
    if (getenv("SIG_DEBUG")) {
        size_t k = rdof_bl_output(bl, c, (uint8_t *)text + ntext, sizeof text - 1 - ntext);
        printf("ask %s -> %.*s\n", req, (int)k, text + ntext);
        ntext += k;
    }
    return c;
}

/* The lines of the output so far, binary payloads skipped: how many start with prefix; the last into last. */
static int lines(const char *prefix, char *last, size_t cap)
{
    int n = 0;
    const char *p = text, *end = text + ntext;
    while (p < end) {
        const char *e = memchr(p, '\n', (size_t)(end - p));
        if (!e)
            break;
        if (!strncmp(p, prefix, strlen(prefix))) {
            n++;
            if (last) {
                const size_t k = (size_t)(e - p) < cap - 1 ? (size_t)(e - p) : cap - 1;
                memcpy(last, p, k), last[k] = 0;
            }
        }
        const char *b = strstr(p, "\"bytes\":");
        long skip = b && b < e ? strtol(b + 8, NULL, 10) : 0;
        p = e + 1 + skip;
    }
    return n;
}

static double num(const char *s, const char *key)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = s ? strstr(s, pat) : NULL;
    return p ? strtod(p + strlen(pat), NULL) : NAN;
}

/* the PCM frames of a stream: samples in all, the largest |value| */
static long pcm(const char *stream, int *peak)
{
    long n = 0;
    *peak = 0;
    char pat[64];
    snprintf(pat, sizeof pat, "{\"pcm\":{\"stream\":\"%s\"", stream);
    const char *p = text, *end = text + ntext;
    while (p < end) {
        const char *e = memchr(p, '\n', (size_t)(end - p));
        if (!e)
            break;
        const char *b = strstr(p, "\"bytes\":");
        long skip = b && b < e ? strtol(b + 8, NULL, 10) : 0;
        if (!strncmp(p, pat, strlen(pat))) {
            for (long i = 0; i < skip / 2; i++) {
                int16_t v;
                memcpy(&v, e + 1 + 2 * i, 2);
                if (abs(v) > *peak)
                    *peak = abs(v);
            }
            n += skip / 2;
        }
        p = e + 1 + skip;
    }
    return n;
}

/* FM both ways. LOOP_CHAN: the receiver on the channel's own outputs (fake_chs.h: no wideband history), its NCO
 * stepped 300 Hz mid-signal as the AFC would; LOOP_ANY: the same with the wideband history there too (ch_any: the
 * rx takes the channel); LOOP_NONE_FREE: ch_any, but no channel there: the rx cuts its own from the history;
 * LOOP_RUN: LOOP_ANY read as even runs (ch_read_run, the W12's). */
enum { LOOP_HIST, LOOP_CHAN, LOOP_ANY, LOOP_NONE_FREE, LOOP_RUN };
static void test_loop(int how)
{
    rdof_bl_platform_t pl = PLAT;
    if (how != LOOP_HIST) {
        static const double f[] = {FREQ}, elsewhere[] = {FREQ + 50000};
        pl.ch_open = fchs_open, pl.ch_read = fchs_read, pl.ch_close = fchs_close;
        pl.ch_read_run = how == LOOP_RUN ? fchs_read_run : NULL;
        pl.iq_read = how == LOOP_CHAN ? NULL : pl.iq_read;
        pl.ch_any = how != LOOP_CHAN;
        fchs_init(&ring, FS, CENTER, how == LOOP_NONE_FREE ? elsewhere : f, 1);
        fchs.step_hz[0] = 300, fchs.step_sc[0] = ring.w + (uint64_t)(FS * 1.5);
    }
    rdof_bl_t *bl = rdof_bl_new(&pl);
    char l[4096];
    ntext = 0;
    /* info names the modes */
    int c = ask(bl, "{\"op\":\"info\"}");
    take(bl, c);
    CHECK(strstr(text, "\"signal_modes\":{\"tx\":[\"fm\",\"am\",\"fsk4\"],\"rx\":[\"fm\",\"am\",\"fsk4\"]}") ||
              strstr(text, "\"signal_modes\":{\"tx\":[\"fm\",\"am\",\"fsk4\",\"iq\"],\"rx\":[\"fm\",\"am\",\"fsk4\"]}"),
          "%.300s", text);
    rdof_bl_closed(bl, c, clock_s);
    ntext = 0;

    air(96000, false); /* half a second of the floor in the history */
    const int rx = ask(bl, "{\"op\":\"rx\",\"mode\":\"fm\",\"freq\":438.075,\"seconds\":2.5,\"streams\":\"audio\","
                           "\"block_s\":0.25}");
    const int tc = ask(bl, "{\"op\":\"tx\",\"mode\":\"fm\",\"freq\":438.075,\"power\":3,\"dev_hz\":2500,"
                           "\"ctcss\":100,\"lead_s\":0.2,\"tail_s\":0.1,\"program\":[{\"kind\":\"tone\",\"hz\":1000,\"s\":1.2}]}");
    for (int i = 0; i < 4; i++) {
        rdof_bl_tick(bl, clock_s);
        air(1920, false);
    }
    CHECK(tx.open && tx.freq == 438075000 && tx.power == 3, "sig_open: %d %u %d", tx.open, tx.freq, tx.power);
    for (int i = 0; i < 300 && !tx.done; i++) { /* 10 ms a step, the source on the air */
        air(1920, true);
        rdof_bl_tick(bl, clock_s);
        take(bl, rx);
    }
    CHECK(tx.done, "the source never finished");
    for (int i = 0; i < 400 && !strstr(text, "{\"done\""); i++) {
        air(1920, false);
        rdof_bl_tick(bl, clock_s);
        take(bl, rx);
    }
    CHECK(lines("{\"rx\":", l, sizeof l) == 1, "no rx header");
    CHECK(fabs(num(l, "floor_dbfs") + 59) < 6, "floor %s", l); /* -50 dBFS in 192 kHz, about 16 kHz of it */
    CHECK(fabs(num(l, "squelch_dbfs") - num(l, "floor_dbfs") - 10) < 0.01, "squelch %s", l);
    CHECK(strstr(l, "\"streams\":[{\"name\":\"audio\""), "%s", l);
    CHECK(lines("{\"rec\":{\"block\"", NULL, 0) >= 8, "blocks %d", lines("{\"rec\":{\"block\"", NULL, 0));
    int peak = 0;
    const long na = pcm("audio", &peak);
    CHECK(na > 2.4 * num(strstr(l, "\"audio\""), "rate") * 0.9, "audio samples %ld", na);
    CHECK(peak > 32767 * 2500 / 5000 * 0.7 && peak < 32767, "audio peak %d", peak);
    CHECK(lines("{\"done\":", l, sizeof l) == 1, "no done");
    CHECK(strstr(l, "\"kind\":\"ctcss\",\"hz\":100.0"), "summary %.600s", l);
    CHECK(num(l, "gap_samples") == 0, "%s", l);
    CHECK(rdof_bl_finished(bl, rx), "rx not finished");
    rdof_bl_closed(bl, rx, clock_s);
    CHECK(how == LOOP_HIST || fchs.opened == (how != LOOP_NONE_FREE), "loop %d: the channel opened %d times", how,
          fchs.opened);
    CHECK(how == LOOP_HIST || fchs.closed == fchs.opened, "loop %d: opened %d, closed %d", how, fchs.opened,
          fchs.closed);

    ntext = 0;
    rdof_bl_tick(bl, clock_s);
    take(bl, tc);
    CHECK(tx.closed, "sig_close never called");
    CHECK(lines("{\"tx\":", l, sizeof l) == 1 && fabs(num(l, "seconds") - 1.65) < 0.01, "%s", l);
    CHECK(lines("{\"started\":", l, sizeof l) == 1 && strstr(l, "power 3 as 21 dBm"), "%s", l);
    CHECK(lines("{\"done\":", l, sizeof l) == 1 && strstr(l, "\"ctcss\":100.0") && strstr(l, "\"cut\":false"), "%s",
          l);
    rdof_bl_closed(bl, tc, clock_s);
    rdof_bl_free(bl);
}

/* AM both ways through the line protocol: the airband receiver test's 1 kHz at 30%, its audio and envelope. */
static void test_am(void)
{
    rdof_bl_t *bl = rdof_bl_new(&PLAT);
    char l[4096];
    ntext = 0;
    memset(&tx, 0, sizeof tx);
    air(96000, false);
    const int rx = ask(bl, "{\"op\":\"rx\",\"mode\":\"am\",\"freq\":438.075,\"seconds\":2.5,\"streams\":\"audio,env\","
                           "\"audio_hi_hz\":2500,\"block_s\":0.25}");
    const int tc = ask(bl, "{\"op\":\"tx\",\"mode\":\"am\",\"freq\":438.075,\"power_dbm\":-12.5,\"depth\":0.3,\"lead_s\":0,"
                           "\"tail_s\":0,\"program\":[{\"kind\":\"tone\",\"hz\":1000,\"s\":1.5}]}");
    for (int i = 0; i < 4; i++) {
        rdof_bl_tick(bl, clock_s);
        air(1920, false);
    }
    CHECK(tx.open && tx.freq == 438075000 && tx.src.iq && !tx.src.freq && tx.power_dbm == -12.5,
          "sig_open, an I/Q source: %d %u, %.1f dBm", tx.open, tx.freq, tx.power_dbm);
    for (int i = 0; i < 300 && !tx.done; i++) {
        air(1920, true);
        rdof_bl_tick(bl, clock_s);
        take(bl, rx);
    }
    CHECK(tx.done, "the source never finished");
    for (int i = 0; i < 400 && !strstr(text, "{\"done\""); i++) {
        air(1920, false);
        rdof_bl_tick(bl, clock_s);
        take(bl, rx);
    }
    CHECK(lines("{\"rx\":", l, sizeof l) == 1, "no rx header");
    CHECK(strstr(l, "{\"name\":\"audio\",\"unit\":\"depth\"") && strstr(l, "{\"name\":\"env\",\"unit\":\"FS\""), "%s",
          l);
    int peak = 0;
    CHECK(pcm("audio", &peak) > 0 && peak > 32767 * 0.3 * 0.8 && peak < 32767 * 0.3 * 1.2, "audio peak %d", peak);
    CHECK(pcm("env", &peak) > 2.4 * FS / 4 * 0.9, "the envelope at the channel's rate");
    CHECK(lines("{\"done\":", l, sizeof l) == 1, "no done");
    const char *t = strstr(l, "\"tone\":{");
    CHECK(t && fabs(num(t, "hz") - 1000) < 1 && fabs(num(t, "depth_pct") - 30) < 1.5, "the tone: %.600s", l);
    CHECK(fabs(num(l, "carrier_dbfs") - 20 * log10(0.5 / 1.3)) < 0.5, "the carrier %.600s", l);
    rdof_bl_closed(bl, rx, clock_s);

    ntext = 0;
    rdof_bl_tick(bl, clock_s);
    take(bl, tc);
    CHECK(lines("{\"done\":", l, sizeof l) == 1 && strstr(l, "\"depth\":0.300") && strstr(l, "\"cut\":false"), "%s",
          l);
    rdof_bl_closed(bl, tc, clock_s);
    rdof_bl_free(bl);
}

/* PCM after the request line: taken as the mode has room, the rest left in the socket */
static void test_pcm(void)
{
    rdof_bl_t *bl = rdof_bl_new(&PLAT);
    char l[2048];
    ntext = 0;
    tx = (typeof(tx)){0};
    /* 10 s of 8 kHz PCM, more than the mode's ring: the first bytes in the request's own read */
    char req[300];
    const int n = snprintf(req, sizeof req, "{\"op\":\"tx\",\"mode\":\"fm\",\"freq\":438.075,\"pcm\":{\"rate\":8000,"
                                            "\"samples\":80000}}\n");
    const int c = rdof_bl_open(bl, clock_s);
    static uint8_t buf[160000 + 400];
    memcpy(buf, req, (size_t)n);
    for (int i = 0; i < 80000; i++) {
        const int16_t s = (int16_t)(8000 * sin(2 * M_PI * 440 * i / 8000.0));
        memcpy(buf + n + 2 * i, &s, 2);
    }
    rdof_bl_input(bl, c, (const char *)buf, (size_t)n + 333, clock_s); /* the line and 333 bytes (odd) */
    take(bl, c);
    CHECK(lines("{\"tx\":", l, sizeof l) == 1 && num(l, "input_bytes") == 160000, "%s", l);
    size_t sent = 333, room;
    int stalls = 0;
    for (int i = 0; i < 20000 && sent < 160000; i++) {
        room = rdof_bl_input_room(bl, c);
        if (!room) {
            stalls++;
            rdof_bl_tick(bl, clock_s);
            air(1920, true);
            continue;
        }
        const size_t k = room < 160000 - sent ? room : 160000 - sent;
        rdof_bl_input(bl, c, (const char *)buf + n + sent, k, clock_s);
        sent += k;
    }
    CHECK(sent == 160000, "sent %zu", sent);
    CHECK(stalls > 0, "the stream never waited for the mode");
    for (int i = 0; i < 1400 && !tx.done; i++) {
        rdof_bl_tick(bl, clock_s);
        air(1920, true);
    }
    rdof_bl_tick(bl, clock_s);
    take(bl, c);
    CHECK(lines("{\"done\":", l, sizeof l) == 1, "no done: %.400s", text);
    CHECK(num(l, "input_bytes") == 160000 && num(l, "pcm_samples") == 80000 && num(l, "underruns") == 0, "%s", l);
    CHECK(rdof_bl_input_room(bl, c) > 0, "a finished job reads on (a close counts)");
    rdof_bl_closed(bl, c, clock_s);
    rdof_bl_free(bl);
}

/* fsk4 streamed: open-ended, an underrun reported as it happens, the client's half-close its end */
static void test_fsk4_stream(void)
{
    rdof_bl_t *bl = rdof_bl_new(&PLAT);
    char l[2048];
    ntext = 0;
    tx = (typeof(tx)){0};
    const double t0 = clock_s;
    const int c = ask(bl, "{\"op\":\"tx\",\"mode\":\"fsk4\",\"freq\":438.075,\"stream\":true,\"seconds\":20,"
                          "\"ahead_s\":0.05,\"idle_hex\":\"55ff\"}");
    static uint8_t buf[2400];
    for (size_t i = 0; i < sizeof buf; i++)
        buf[i] = (uint8_t)(i * 37 + 11);
    size_t sent = 0;
    for (int i = 0; i < 4000 && !tx.done; i++) {
        const bool pause = i >= 20 && i < 40; /* 20 ticks of 40 ms with nothing: dry */
        size_t room = rdof_bl_input_room(bl, c);
        if (!pause && room && sent < sizeof buf) {
            const size_t k = room < sizeof buf - sent ? room : sizeof buf - sent;
            rdof_bl_input(bl, c, (const char *)buf + sent, k, clock_s);
            sent += k;
            if (sent == sizeof buf)
                CHECK(rdof_bl_input_end(bl, c, clock_s), "a half-close ends the stream, the job goes on");
        }
        rdof_bl_tick(bl, clock_s);
        air(1920, true);
    }
    rdof_bl_tick(bl, clock_s);
    take(bl, c);
    CHECK(sent == sizeof buf, "sent %zu", sent);
    CHECK(lines("{\"underrun\":", l, sizeof l) == 1 && strstr(l, "\"fill\":\"idle\""), "underrun: %.400s", text);
    CHECK(lines("{\"done\":", l, sizeof l) == 1, "no done: %.400s", text);
    CHECK(num(l, "symbols_in") == 4 * sizeof buf && num(l, "underruns") == 1 && num(l, "idle_symbols") > 0, "%s", l);
    CHECK(clock_s - t0 < 20, "ended at the stream's end, not at seconds (%.1f s)", clock_s - t0);
    CHECK(rdof_bl_input_room(bl, c) == 0, "nothing more read after the half-close");
    rdof_bl_closed(bl, c, clock_s);

    /* without a stream to end, a half-close closes */
    const int c2 = ask(bl, "{\"op\":\"info\"}");
    CHECK(!rdof_bl_input_end(bl, c2, clock_s), "info: a half-close closes it");
    rdof_bl_free(bl);
}

/* ── iq: the client's complex baseband, two C4FM carriers 50 kHz apart in one job ── */

static int f_bands(void *ctx, double (*out)[2], int max)
{
    out[0][0] = 438.10e6, out[0][1] = 438.13e6;
    return 1;
}

#define IQ_NSYM 624 /* the P25 sync's 24 symbols, then 600 random */
static uint8_t iq_frame[2][IQ_NSYM / 4];

/* Carrier k's frame through fsk4's own C4FM transmitter at FS: the frequency (Hz) and envelope, how many samples */
static long iq_render_c4fm(int k, float *hz, float *env, long cap)
{
    static rdof_jnode_t nodes[64];
    static char sbuf[4096];
    static rdof_jdoc_t doc = {nodes, 64, 0, sbuf, sizeof sbuf, 0, ""};
    char job[1024] = "{\"lead_s\":0.04,\"tail_s\":0.02,\"repeat\":2,\"hex\":\"";
    for (int i = 0; i < IQ_NSYM / 4; i++)
        snprintf(job + strlen(job), 3, "%02x", iq_frame[k][i]);
    strcat(job, "\"}");
    const int root = rdof_jparse(&doc, job, strlen(job));
    double secs = 0;
    char err[200] = "";
    void *t = rdof_mode_fsk4.tx_new(&doc, root, FS, &secs, err, sizeof err);
    CHECK(t != NULL, "fsk4 tx_new: %s", err);
    if (!t)
        return 0;
    const rdof_sig_src_t src = rdof_mode_fsk4.tx_src(t);
    long n = 0;
    for (int got; n < cap && (got = src.freq(src.ctx, hz + n, env + n, (int)(cap - n < 4096 ? cap - n : 4096))) > 0;)
        n += got;
    rdof_mode_fsk4.tx_free(t);
    return n;
}

/* One run: the job on the air, an fsk4 rx on carrier `listen` (0: 438.075, 1: 438.125): symbol errors from its first
 * sync on against the frame sent there (-1: no dibits) */
static double f_sig_rate48(void *ctx) { return 48000; }

static int iq_run(const char *txreq, const char *rxx, const int16_t *x, long n, int listen, char *done, size_t dcap)
{
    rdof_bl_platform_t pl = PLAT;
    pl.tx_bands = f_bands;
    pl.sig_rate = f_sig_rate48; /* a mode's sources at 48 kS/s, spread 4 to 1 over the 192 kS/s stream */
    rdof_bl_t *bl = rdof_bl_new(&pl);
    ntext = 0;
    tx = (typeof(tx)){0};
    air(96000, false);
    char req[400];
    snprintf(req, sizeof req, "{\"op\":\"rx\",\"mode\":\"fsk4\",\"freq\":%s,\"seconds\":2,%s\"sync\":\"5575f5ff77ff\"}",
             listen ? "438.125" : "438.075", rxx);
    const int rx = ask(bl, req);
    const int c = ask(bl, txreq);
    size_t sent = 0;
    const size_t bytes = (size_t)n * 4;
    for (int i = 0; i < 3000 && !tx.done; i++) {
        size_t room; /* as a socket's reader: all the engine takes */
        while (sent < bytes && (room = rdof_bl_input_room(bl, c)) > 0) {
            const size_t k = room < bytes - sent ? room : bytes - sent;
            rdof_bl_input(bl, c, (const char *)x + sent, k, clock_s);
            sent += k;
            if (sent == bytes)
                CHECK(rdof_bl_input_end(bl, c, clock_s), "a half-close ends the samples");
        }
        rdof_bl_tick(bl, clock_s);
        air(1920, true);
        take(bl, rx);
    }
    CHECK(tx.done && sent == bytes, "done %d, sent %zu of %zu: %.300s", tx.done, sent, bytes, text);
    CHECK(tx.src.rate == 192000 && tx.src.span_lo_hz == -6250 && tx.src.span_hi_hz == 56250,
          "the source: rate %.0f, span %.0f to %.0f", tx.src.rate, tx.src.span_lo_hz, tx.src.span_hi_hz);
    for (int i = 0; i < 400 && !strstr(text, "{\"done\""); i++) {
        air(1920, false);
        rdof_bl_tick(bl, clock_s);
        take(bl, rx);
    }
    /* the dibits from the first sync on */
    static uint8_t got[4096];
    long ng = 0;
    for (const char *p = text; (p = strstr(p, "{\"dibits\":")) && ng < (long)sizeof got; p++) {
        const char *h = strstr(p, "\"hex\":\"");
        if (!h)
            break;
        for (h += 7; h[0] != '"' && h[1] && ng < (long)sizeof got; h += 2) {
            unsigned v;
            if (sscanf(h, "%2x", &v) != 1)
                break;
            got[ng++] = (uint8_t)v;
        }
    }
    int errors = ng >= IQ_NSYM / 4 ? 0 : -1;
    for (int k = 0; errors >= 0 && k < IQ_NSYM; k++)
        errors += (got[k / 4] >> (6 - 2 * (k % 4)) & 3) != (iq_frame[listen][k / 4] >> (6 - 2 * (k % 4)) & 3);
    rdof_bl_closed(bl, rx, clock_s);
    ntext = 0;
    rdof_bl_tick(bl, clock_s);
    take(bl, c);
    char l[2048] = "";
    lines("{\"done\":", l, sizeof l);
    snprintf(done, dcap, "%s", l);
    rdof_bl_closed(bl, c, clock_s);
    rdof_bl_free(bl);
    return errors;
}

static void test_iq(void)
{
    if (!rdof_sig_find(rdof_sdr_modes, "iq")) {
        printf("benchsig: no iq mode in this build (the C modules): skipped\n");
        return;
    }
    static const uint8_t sync[6] = {0x55, 0x75, 0xf5, 0xff, 0x77, 0xff};
    for (int k = 0; k < 2; k++) {
        memcpy(iq_frame[k], sync, 6);
        for (int i = 6; i < IQ_NSYM / 4; i++)
            seed = seed * 1664525u + 1013904223u, iq_frame[k][i] = (uint8_t)(seed >> 13);
    }
    /* the two carriers summed: A at the job's freq, B 50 kHz above, each 0.45 of full scale */
    enum { MAXN = 192000 };
    static float hz[2][MAXN], env[2][MAXN];
    static int16_t x[2 * MAXN];
    const long na = iq_render_c4fm(0, hz[0], env[0], MAXN), nb = iq_render_c4fm(1, hz[1], env[1], MAXN);
    const long n = na > nb ? na : nb;
    double pa = 0, pb = 0;
    for (long i = 0; i < n; i++) {
        float re = 0, im = 0;
        if (i < na) {
            pa += 2 * M_PI * hz[0][i] / FS;
            re += 0.45f * env[0][i] * (float)cos(pa), im += 0.45f * env[0][i] * (float)sin(pa);
        }
        if (i < nb) {
            pb += 2 * M_PI * (hz[1][i] + 50000) / FS;
            re += 0.45f * env[1][i] * (float)cos(pb), im += 0.45f * env[1][i] * (float)sin(pb);
        }
        x[2 * i] = (int16_t)lrintf(re * 32767), x[2 * i + 1] = (int16_t)lrintf(im * 32767);
    }
    char done[2048];
    for (int k = 0; k < 2; k++) {
        const int e = iq_run("{\"op\":\"tx\",\"mode\":\"iq\",\"freq\":438.075,\"carriers\":[438.075,438.125],"
                             "\"rate\":192000,\"seconds\":5,\"ahead_s\":0.05}", "", x, n, k, done, sizeof done);
        CHECK(e == 0, "carrier %c: %d symbol errors of %d", "AB"[k], e, IQ_NSYM);
        CHECK(num(done, "samples_in") == n && num(done, "underruns") == 0 && num(done, "clipped") == 0, "%s", done);
    }

    /* the same two frames as fsk4 carriers: the gateway renders each (48 kS/s), sums them at 192 kS/s; B keys 0.1 s
     * later at its own level */
    static char req[4096];
    int at = snprintf(req, sizeof req, "{\"op\":\"tx\",\"mode\":\"fsk4\",\"freq\":438.075,\"lead_s\":0.04,"
                                       "\"tail_s\":0.02,\"repeat\":2,\"carriers\":[");
    for (int k = 0; k < 2; k++) {
        at += snprintf(req + at, sizeof req - (size_t)at, "%s{\"freq\":%s,%s\"hex\":\"", k ? "," : "",
                       k ? "438.125" : "438.075", k ? "\"start_s\":0.1,\"level\":0.4," : "");
        for (int i = 0; i < IQ_NSYM / 4; i++)
            at += snprintf(req + at, sizeof req - (size_t)at, "%02x", iq_frame[k][i]);
        at += snprintf(req + at, sizeof req - (size_t)at, "\"}");
    }
    snprintf(req + at, sizeof req - (size_t)at, "]}");
    for (int k = 0; k < 2; k++) {
        const int e = iq_run(req, "", NULL, 0, k, done, sizeof done);
        CHECK(e == 0, "fsk4 carrier %c: %d symbol errors of %d", "AB"[k], e, IQ_NSYM);
        CHECK(strstr(done, "\"mode\":\"fsk4\"") && strstr(done, "\"carriers\":[{\"freq\":438.075000,\"start_s\":0.000,"
                                                                "\"level\":0.500,\"shape\":\"c4fm\"") &&
                  strstr(done, "{\"freq\":438.125000,\"start_s\":0.100,\"level\":0.400,"),
              "%s", done);
    }
    /* a DMR Tier III site's control channel and a traffic channel: the job's fields (DMR's pulse and deviation) are
     * both carriers', the traffic channel keys 60 ms in */
    at = snprintf(req, sizeof req, "{\"op\":\"tx\",\"mode\":\"fsk4\",\"freq\":438.075,\"shape\":\"rrc\","
                                   "\"dev_hz\":1944,\"repeat\":2,\"carriers\":[");
    for (int k = 0; k < 2; k++) {
        at += snprintf(req + at, sizeof req - (size_t)at, "%s{\"freq\":%s,%s\"hex\":\"", k ? "," : "",
                       k ? "438.125" : "438.075", k ? "\"start_s\":0.06," : "");
        for (int i = 0; i < IQ_NSYM / 4; i++)
            at += snprintf(req + at, sizeof req - (size_t)at, "%02x", iq_frame[k][i]);
        at += snprintf(req + at, sizeof req - (size_t)at, "\"}");
    }
    snprintf(req + at, sizeof req - (size_t)at, "]}");
    for (int k = 0; k < 2; k++) {
        const int e = iq_run(req, "\"shape\":\"rrc\",", NULL, 0, k, done, sizeof done);
        CHECK(e == 0, "dmr carrier %c: %d symbol errors of %d", "AB"[k], e, IQ_NSYM);
        CHECK(strstr(done, "\"level\":0.500,\"shape\":\"rrc\"") && strstr(done, "\"dev_hz\":1944.0"), "%s", done);
    }
    {
        static const char *bad[][2] = {
            {"{\"op\":\"tx\",\"mode\":\"fsk4\",\"freq\":438.075,\"carriers\":[{\"freq\":438.075,\"hex\":\"00\"},"
             "{\"freq\":438.125,\"hex\":\"00\",\"level\":0.8}]}", "levels"},
            {"{\"op\":\"tx\",\"mode\":\"fsk4\",\"freq\":438.075,\"carriers\":[{\"freq\":438.125,\"symbols\":8}]}",
             "in the request"},
            {"{\"op\":\"tx\",\"mode\":\"fsk4\",\"freq\":438.075,\"carriers\":[{\"freq\":438.15,\"hex\":\"00\"}]}",
             "isn't a transmit frequency"},
            {"{\"op\":\"tx\",\"mode\":\"fsk4\",\"freq\":438.075,\"carriers\":[{\"hex\":\"00\"}]}", "freq"},
            {"{\"op\":\"tx\",\"mode\":\"fsk4\",\"freq\":438.075,\"carriers\":[438.125],\"hex\":\"00\"}",
             "carriers as frequencies"},
        };
        rdof_bl_platform_t pl = PLAT;
        pl.tx_bands = f_bands;
        rdof_bl_t *bl = rdof_bl_new(&pl);
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            ntext = 0;
            const int c = ask(bl, bad[i][0]);
            take(bl, c);
            CHECK(strstr(text, "\"error\"") && strstr(text, bad[i][1]), "%s -> %s", bad[i][0], text);
            rdof_bl_closed(bl, c, clock_s);
        }
        rdof_bl_free(bl);
    }

    /* refused: a carrier the bench doesn't send on, a rate that isn't given */
    rdof_bl_platform_t pl = PLAT;
    pl.tx_bands = f_bands;
    rdof_bl_t *bl = rdof_bl_new(&pl);
    static const struct {
        const char *req, *code, *has;
    } R[] = {
        {"{\"op\":\"tx\",\"mode\":\"iq\",\"freq\":438.075,\"carriers\":[438.15],\"rate\":192000,\"seconds\":1}",
         "refused", "carriers"},
        {"{\"op\":\"tx\",\"mode\":\"iq\",\"freq\":438.075,\"carriers\":438.125,\"rate\":192000,\"seconds\":1}",
         "bad_request", "carriers"},
        {"{\"op\":\"tx\",\"mode\":\"iq\",\"freq\":438.075,\"seconds\":1}", "bad_request", "rate"},
        {"{\"op\":\"tx\",\"mode\":\"iq\",\"freq\":438.075,\"rate\":192000}", "bad_request", "seconds"},
    };
    for (size_t i = 0; i < sizeof R / sizeof R[0]; i++) {
        ntext = 0;
        const int c = ask(bl, R[i].req);
        take(bl, c);
        char code[80];
        snprintf(code, sizeof code, "\"code\":\"%s\"", R[i].code);
        CHECK(strstr(text, code) && strstr(text, R[i].has), "%s -> %s", R[i].req, text);
        rdof_bl_closed(bl, c, clock_s);
    }
    rdof_bl_free(bl);
}

static void test_refusals(void)
{
    rdof_bl_t *bl = rdof_bl_new(&PLAT);
    static const struct {
        const char *req, *code, *has;
    } R[] = {
        {"{\"op\":\"tx\",\"freq\":438.075}", "bad_request", "no mode"},
        {"{\"op\":\"tx\",\"mode\":\"p25\",\"freq\":438.075}", "unsupported", "it does"},
        {"{\"op\":\"tx\",\"mode\":\"fm\",\"freq\":438.15}", "refused", "transmit frequency"},
        {"{\"op\":\"tx\",\"mode\":\"fm\",\"freq\":438.075,\"ctcss\":100,\"dcs\":\"023N\"}", "bad_request", "not both"},
        {"{\"op\":\"rx\",\"mode\":\"fm\",\"freq\":447}", "refused", "window"},
        {"{\"op\":\"rx\",\"mode\":\"fm\",\"freq\":438.075,\"bw\":90000}", "bad_request", "bw"},
        {"{\"op\":\"rx\",\"mode\":\"fm\",\"freq\":438.075,\"block_s\":50}", "bad_request", "block_s"},
    };
    for (size_t i = 0; i < sizeof R / sizeof R[0]; i++) {
        ntext = 0;
        const int c = ask(bl, R[i].req);
        take(bl, c);
        char code[80];
        snprintf(code, sizeof code, "\"code\":\"%s\"", R[i].code);
        CHECK(strstr(text, code) && strstr(text, R[i].has), "%s -> %s", R[i].req, text);
        CHECK(rdof_bl_finished(bl, c), "%s: not finished", R[i].req);
        rdof_bl_closed(bl, c, clock_s);
    }
    /* a transmission cut short by the client: the transmitter goes back */
    tx = (typeof(tx)){0};
    const int c = ask(bl, "{\"op\":\"tx\",\"mode\":\"fm\",\"freq\":438.075,\"program\":[{\"kind\":\"tone\",\"hz\":1000,\"s\":5}]}");
    rdof_bl_tick(bl, clock_s);
    CHECK(tx.open, "not keyed");
    rdof_bl_closed(bl, c, clock_s);
    CHECK(tx.closed && !tx.open, "the transmitter wasn't given back");
    /* a roamer transmitting: asked again each tick, then keyed */
    static const char *tone = "{\"op\":\"tx\",\"mode\":\"fm\",\"freq\":438.075,\"program\":[{\"kind\":\"tone\",\"hz\":1000,\"s\":1}]}";
    tx = (typeof(tx)){0}, busy_n = 2, ntext = 0;
    const int w = ask(bl, tone);
    rdof_bl_tick(bl, clock_s);
    take(bl, w);
    CHECK(!tx.open && strstr(text, "waiting to transmit: the roamer is transmitting"), "not waiting: %s", text);
    for (int i = 0; i < 3; i++)
        clock_s += 0.05, rdof_bl_tick(bl, clock_s);
    take(bl, w);
    CHECK(tx.open && strstr(text, "\"started\":"), "not keyed after the roamer: %s", text);
    rdof_bl_closed(bl, w, clock_s);
    /* and one that never ends: busy after 10 s */
    tx = (typeof(tx)){0}, busy_n = 1000000, ntext = 0;
    const int z = ask(bl, tone);
    for (int i = 0; i < 120 && !rdof_bl_finished(bl, z); i++)
        clock_s += 0.1, rdof_bl_tick(bl, clock_s);
    take(bl, z);
    CHECK(!tx.open && strstr(text, "\"code\":\"busy\"") && rdof_bl_finished(bl, z), "no busy: %s", text);
    rdof_bl_closed(bl, z, clock_s);
    busy_n = 0;
    rdof_bl_free(bl);
}

int main(void)
{
    if (!rdof_bl_ring_init(&ring, (long)(FS * 4))) {
        printf("no memory\n");
        return 1;
    }
    test_loop(LOOP_HIST);
    test_loop(LOOP_CHAN);
    test_loop(LOOP_ANY);
    test_loop(LOOP_NONE_FREE);
    test_loop(LOOP_RUN);
    test_pcm();
    test_fsk4_stream();
    test_iq();
    test_refusals();
    test_am();
    rdof_bl_ring_free(&ring);
    printf("benchsig: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
