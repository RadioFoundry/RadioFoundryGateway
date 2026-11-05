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
    *w = (rdof_bl_window_t){.running = true, .center_hz = CENTER, .fs = FS, .lo_hz = 438.01e6, .hi_hz = 438.19e6};
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

/* FM both ways: with chan, the receiver on the channel's own outputs (fake_chs.h: no wideband history), its NCO
 * stepped 300 Hz mid-signal as the AFC would */
static void test_loop(bool chan)
{
    rdof_bl_platform_t pl = PLAT;
    if (chan) {
        static const double f[] = {FREQ};
        pl.iq_read = NULL, pl.ch_open = fchs_open, pl.ch_read = fchs_read, pl.ch_close = fchs_close;
        fchs_init(&ring, FS, CENTER, f, 1);
        fchs.step_hz[0] = 300, fchs.step_sc[0] = ring.w + (uint64_t)(FS * 1.5);
    }
    rdof_bl_t *bl = rdof_bl_new(&pl);
    char l[4096];
    ntext = 0;
    /* info names the modes */
    int c = ask(bl, "{\"op\":\"info\"}");
    take(bl, c);
    CHECK(strstr(text, "\"signal_modes\":{\"tx\":[\"fm\",\"am\"],\"rx\":[\"fm\",\"am\"]}"), "%.300s", text);
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
    test_loop(false);
    test_loop(true);
    test_pcm();
    test_refusals();
    test_am();
    rdof_bl_ring_free(&ring);
    printf("benchsig: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
