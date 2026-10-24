// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The bench radio link's guard (rdof/benchguard.h) against a fake RadioFoundry radio and an MMDVM modem. */
#include <stdio.h>
#include <string.h>

#include "rdof/benchcfg.h"
#include "rdof/benchguard.h"

extern int checks, failures;

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

/* ── a fake RadioFoundry radio on the debug link, and the tool's side ─────── */

typedef struct {
    rdof_guard_t g;
    /* the radio */
    int power, aprs_power;
    bool tx, answers;
    unsigned tx_hz, rx_hz;
    int mode, cc, ts, busy_left; /* busy_left: VFO_SET answers busy this many times */
    unsigned target;
    bool hotspot, refuse_fields, ignore_vfo;
    bool refuse_view, refuse_power, stuck_power, no_aprs; /* a radio that misbehaves */
    uint8_t rxbuf[4096]; /* what the guard sent the radio */
    size_t nrx;
    uint8_t pending[4096]; /* the radio's answers, not yet delivered */
    size_t npend;
    uint8_t order[32]; /* commands the radio got, in order */
    int norder;
    /* the tool */
    uint8_t tool[4096];
    size_t ntool;
    char events[512];
} bench_t;

static size_t odr(uint8_t cmd, uint8_t seq, const uint8_t *p, size_t n, uint8_t *out)
{
    return rdof_guard_frame(cmd, seq, p, n, out);
}

static void radio_answer(bench_t *b, uint8_t cmd, uint8_t seq, const uint8_t *p, size_t n)
{
    b->npend += odr(cmd | 0x80, seq, p, n, b->pending + b->npend);
}

static void radio_view(bench_t *b, uint8_t seq, int section)
{
    uint8_t p[200] = {0, section, 0, 0, 0, 0};
    int n = section == 14 ? snprintf((char *)p + 6, sizeof p - 6, "enabled=1\npower=%d\n", b->aprs_power)
                          : snprintf((char *)p + 6, sizeof p - 6, "state=%s\npower=%d\npower_count=4\nrx_hz=%u\ntx_hz=%u\n",
                                     b->tx ? "tx" : "rx", b->power, b->rx_hz, b->tx_hz);
    radio_answer(b, 0x09, seq, p, 6 + (size_t)n);
}

/* The radio takes a whole debug link frame at a time (the guard writes whole frames). */
static void to_radio(void *ctx, const uint8_t *f, size_t n)
{
    bench_t *b = ctx;
    memcpy(b->rxbuf + b->nrx, f, n);
    b->nrx += n;
    if (n < 8 || f[0] != 0xA5)
        return;
    const uint8_t cmd = f[2], seq = f[3], *p = f + 6;
    if (b->norder < 32)
        b->order[b->norder++] = cmd;
    if (!b->answers)
        return;
    const uint8_t ok = 0, no = 5;
    if (cmd == 0x09 && (b->refuse_view || (b->no_aprs && p[0] == 14))) {
        if (b->refuse_view)
            radio_answer(b, cmd, seq, &no, 1);
    } else if (cmd == 0x09)
        radio_view(b, seq, p[0]);
    else if (cmd == 0x0A && p[0] == 7) {
        if (!b->stuck_power && !b->refuse_power)
            b->power += (int8_t)p[1];
        radio_answer(b, cmd, seq, b->refuse_power ? &no : &ok, 1);
    } else if (cmd == 0x0A) {
        uint8_t st = 0;
        if (p[0] == 14) {
            st = b->hotspot ? 0 : 4;
            b->hotspot = false;
        } else if (p[0] == 5 && b->busy_left > 0) {
            b->busy_left--;
            st = 3;
        } else if (p[0] == 5 && !b->ignore_vfo) {
            b->rx_hz = b->tx_hz = p[1] | p[2] << 8 | p[3] << 16 | (unsigned)p[4] << 24;
        } else if (p[0] == 31 && b->refuse_fields) {
            st = 7;
        } else if (p[0] == 31) {
            b->mode = p[1] == 0xFF ? b->mode : p[1];
            b->cc = p[2] == 0xFF ? b->cc : p[2];
            b->ts = p[3] ? p[3] : b->ts;
        } else if (p[0] == 15) {
            b->target = p[1] | p[2] << 8 | p[3] << 16 | (unsigned)p[4] << 24;
        }
        radio_answer(b, cmd, seq, &st, 1);
    } else if (cmd == 0x11) {
        b->aprs_power = p[2];
        radio_answer(b, cmd, seq, &ok, 1);
    } else if (cmd == 0x07) {
        b->tx = p[0];
        radio_answer(b, cmd, seq, &ok, 1);
    } else
        radio_answer(b, cmd, seq, &ok, 1);
}

static void to_tool(void *ctx, const uint8_t *f, size_t n)
{
    bench_t *b = ctx;
    memcpy(b->tool + b->ntool, f, n);
    b->ntool += n;
}

static void on_event(void *ctx, const char *kind, const char *detail)
{
    bench_t *b = ctx;
    size_t l = strlen(b->events);
    snprintf(b->events + l, sizeof b->events - l, "%s;", kind);
}

/* Deliver the radio's answers until it has nothing more to say. */
static void pump(bench_t *b, double now)
{
    for (int i = 0; i < 50 && b->npend; i++) {
        uint8_t f[4096];
        size_t n = b->npend;
        memcpy(f, b->pending, n);
        b->npend = 0;
        rdof_guard_radio(&b->g, f, n, now);
    }
}

/* Run the clock from t0 to t1 in 20 ms ticks, answering as the radio goes. */
static double run(bench_t *b, double t0, double t1)
{
    for (double t = t0; t <= t1; t += 0.02) {
        rdof_guard_tick(&b->g, t);
        pump(b, t);
    }
    return t1;
}

static void setup(bench_t *b, rdof_guard_proto_t proto, const uint32_t *tx, int ntx)
{
    memset(b, 0, sizeof *b);
    b->answers = true;
    rdof_guard_io_t io = {to_radio, to_tool, on_event, b};
    rdof_guard_init(&b->g, proto, &io, tx, ntx);
}

/* The tool's frames back from the guard: is there one with this cmd and seq? */
static bool tool_got(const bench_t *b, uint8_t cmd, uint8_t seq)
{
    for (size_t i = 0; i + 8 <= b->ntool; i++)
        if (b->tool[i] == 0xA5 && b->tool[i + 1] == 0x5A && b->tool[i + 2] == cmd && b->tool[i + 3] == seq)
            return true;
    return false;
}

static void test_guard_odr(void)
{
    static bench_t b;
    const uint32_t tx[] = {446100000};
    uint8_t f[64];

    /* the lease's check: power down to the lowest, APRS too; the tool hears none of it */
    setup(&b, RDOF_GUARD_ODR, tx, 1);
    b.power = 3;
    b.aprs_power = 5;
    rdof_guard_check(&b.g, 0);
    run(&b, 0, 1);
    CHECK(b.power == 0 && b.aprs_power == 1, "power %d, aprs %d", b.power, b.aprs_power);
    CHECK(b.g.stats.power_steps == 4 && b.ntool == 0, "steps %ld, tool got %zu bytes", b.g.stats.power_steps, b.ntool);
    CHECK(!rdof_guard_busy(&b.g), "done");

    /* a power step up and an APRS power above the lowest are refused, with an error frame on the tool's seq */
    const uint8_t up[2] = {7, 1};
    rdof_guard_tool(&b.g, f, odr(0x0A, 5, up, 2, f), 2);
    CHECK(tool_got(&b, 0xFF, 5) && b.g.stats.refused == 1, "power up refused");
    const uint8_t aprs[6] = {5, 24, 3, 0, 0, 0};
    rdof_guard_tool(&b.g, f, odr(0x11, 6, aprs, 6, f), 2);
    CHECK(tool_got(&b, 0xFF, 6), "APRS power refused");
    /* a power step down passes */
    b.ntool = 0;
    const uint8_t down[2] = {7, 0xFF};
    rdof_guard_tool(&b.g, f, odr(0x0A, 7, down, 2, f), 2);
    pump(&b, 2);
    CHECK(tool_got(&b, 0x8A, 7) && !tool_got(&b, 0xFF, 7), "power down passes and is answered");

    /* PTT on the declared frequency: held for the check, then passed; what came after it waits its turn */
    b.tx_hz = 446100000;
    b.power = 0;
    b.norder = 0;
    b.ntool = 0;
    const uint8_t ptt[3] = {1, 0, 0}, view[1] = {0};
    size_t n = odr(0x07, 8, ptt, 3, f);
    n += odr(0x09, 9, view, 1, f + n);
    rdof_guard_tool(&b.g, f, n, 3);
    CHECK(rdof_guard_busy(&b.g) && b.order[0] == 0x09, "PTT held while the guard looks");
    run(&b, 3, 3.5);
    CHECK(b.tx, "keyed");
    CHECK(tool_got(&b, 0x87, 8) && tool_got(&b, 0x89, 9), "the tool's answers");
    int ptt_at = -1, view_at = -1;
    for (int i = 0; i < b.norder; i++) {
        if (b.order[i] == 0x07)
            ptt_at = i;
        if (b.order[i] == 0x09)
            view_at = i; /* the last one: the tool's */
    }
    CHECK(ptt_at >= 0 && view_at > ptt_at, "the tool's view after its PTT (%d, %d)", ptt_at, view_at);

    /* PTT on a frequency the lease didn't declare */
    b.tx = false;
    b.tx_hz = 446200000;
    b.ntool = 0;
    rdof_guard_tool(&b.g, f, odr(0x07, 10, ptt, 3, f), 4);
    run(&b, 4, 4.5);
    CHECK(!b.tx && tool_got(&b, 0xFF, 10), "refused off the lease");

    /* receive only: no PTT at all */
    setup(&b, RDOF_GUARD_ODR, NULL, 0);
    rdof_guard_tool(&b.g, f, odr(0x07, 1, ptt, 3, f), 0);
    CHECK(tool_got(&b, 0xFF, 1) && b.norder == 0, "receive-only lease");

    /* a transmission the guard didn't key, found by the periodic check while the tool uses the link */
    setup(&b, RDOF_GUARD_ODR, tx, 1);
    b.tx = true;
    b.tx_hz = 445000000;
    rdof_guard_tool(&b.g, f, odr(0x09, 1, view, 1, f), 0);
    pump(&b, 0);
    run(&b, 0, 3);
    CHECK(b.g.stats.violations >= 1 && !b.tx, "violation %ld, released %d", b.g.stats.violations, !b.tx);
    CHECK(strstr(b.events, "tx_violation"), "%s", b.events);
    /* no periodic checks once the tool stops using the debug link */
    long c0 = b.g.stats.checks;
    run(&b, 3, 12);
    CHECK(b.g.stats.checks - c0 <= 2, "checks after the link went quiet: %ld", b.g.stats.checks - c0);

    /* a radio that doesn't answer: the held PTT is refused once the answer is overdue */
    setup(&b, RDOF_GUARD_ODR, tx, 1);
    b.answers = false;
    rdof_guard_tool(&b.g, f, odr(0x07, 3, ptt, 3, f), 0);
    run(&b, 0, 1.5);
    CHECK(tool_got(&b, 0xFF, 3) && strstr(b.events, "check_failed"), "%s", b.events);

    /* the splitter: a frame across writes; a bad CRC is just bytes; other bytes pass both ways */
    setup(&b, RDOF_GUARD_ODR, tx, 1);
    n = odr(0x09, 4, view, 1, f);
    rdof_guard_tool(&b.g, f, 3, 0);
    CHECK(b.nrx == 0, "half a frame waits");
    rdof_guard_tool(&b.g, f + 3, n - 3, 0);
    CHECK(b.nrx == n && !memcmp(b.rxbuf, f, n), "then goes whole");
    b.nrx = 0;
    f[n - 1] ^= 1;
    rdof_guard_tool(&b.g, f, n, 0);
    CHECK(b.nrx == n && !memcmp(b.rxbuf, f, n), "bad CRC: the same bytes");
    b.nrx = 0;
    const uint8_t mm[] = {0xE0, 0x04, 0x01, 0x00, 'h', 'i'};
    rdof_guard_tool(&b.g, mm, sizeof mm, 0);
    CHECK(b.nrx == sizeof mm, "a RadioFoundry link passes MMDVM unguarded");
    b.ntool = 0;
    rdof_guard_radio(&b.g, mm, sizeof mm, 0);
    CHECK(b.ntool == sizeof mm, "and the radio's bytes back");

    /* the lease ends mid-transmission: the check finds it and releases PTT; then PTT is refused */
    setup(&b, RDOF_GUARD_ODR, tx, 1);
    b.tx = true;
    b.tx_hz = 446100000;
    rdof_guard_set_tx(&b.g, NULL, 0, 0);
    run(&b, 0, 1);
    CHECK(!b.tx && b.g.stats.violations == 1, "lease gone: released (%ld)", b.g.stats.violations);
    b.ntool = 0;
    rdof_guard_tool(&b.g, f, odr(0x07, 2, ptt, 3, f), 1);
    CHECK(tool_got(&b, 0xFF, 2), "then PTT refused");
    /* a lease that still covers it: no violation */
    setup(&b, RDOF_GUARD_ODR, NULL, 0);
    b.tx = true;
    b.tx_hz = 446100000;
    rdof_guard_set_tx(&b.g, tx, 1, 0);
    run(&b, 0, 1);
    CHECK(b.tx && !b.g.stats.violations, "lease covers it");

    /* the tool goes away while keyed: PTT released, held bytes dropped */
    b.norder = 0;
    rdof_guard_end(&b.g, 2);
    run(&b, 2, 3);
    CHECK(!b.tx && b.norder == 1 && b.order[0] == 0x07, "tool gone: released (%d commands)", b.norder);
    /* even when the radio doesn't answer the release, the guard comes back to idle */
    setup(&b, RDOF_GUARD_ODR, tx, 1);
    b.answers = false;
    rdof_guard_end(&b.g, 0);
    run(&b, 0, 2);
    CHECK(!rdof_guard_busy(&b.g) && b.ntool == 0, "idle again, nothing to the tool");
}

static void test_guard_mmdvm(void)
{
    static bench_t b;
    const uint32_t tx[] = {446100000};
    uint8_t f[32] = {0xE0, 17, 0x04, 0};
    setup(&b, RDOF_GUARD_MMDVM, tx, 1);
    b.answers = false;

    const uint8_t data[] = {0xE0, 37, 0x18, 0x00};
    uint8_t d[37] = {0};
    memcpy(d, data, sizeof data);
    rdof_guard_tool(&b.g, d, sizeof d, 0);
    CHECK(b.nrx == 0 && b.ntool == 5 && b.tool[2] == 0x7F && b.tool[3] == 0x18, "DMR data before SET_FREQ: NAK");

    /* SET_FREQ off the lease: NAK */
    b.ntool = 0;
    uint32_t rx_hz = 446100000, bad = 446200000;
    memcpy(f + 4, &rx_hz, 4);
    memcpy(f + 8, &bad, 4);
    f[12] = 255;
    rdof_guard_tool(&b.g, f, 17, 0);
    CHECK(b.nrx == 0 && b.ntool == 5 && b.tool[3] == 0x04, "SET_FREQ off the lease: NAK");

    /* on the lease: through, at power 0 */
    memcpy(f + 8, &rx_hz, 4);
    rdof_guard_tool(&b.g, f, 17, 0);
    CHECK(b.nrx == 17 && b.rxbuf[12] == 0 && !memcmp(b.rxbuf, f, 12), "SET_FREQ through at power 0");

    b.nrx = 0;
    rdof_guard_tool(&b.g, d, sizeof d, 0);
    CHECK(b.nrx == sizeof d, "DMR data after it");

    /* a frame split across writes */
    b.nrx = 0;
    rdof_guard_tool(&b.g, d, 10, 0);
    CHECK(b.nrx == 0, "half waits");
    rdof_guard_tool(&b.g, d + 10, sizeof d - 10, 0);
    CHECK(b.nrx == sizeof d, "then whole");

    /* receive only: SET_FREQ goes (at power 0), TX doesn't */
    setup(&b, RDOF_GUARD_MMDVM, NULL, 0);
    rdof_guard_tool(&b.g, f, 17, 0);
    rdof_guard_tool(&b.g, d, sizeof d, 0);
    CHECK(b.nrx == 17 && b.ntool == 5, "receive only");

    /* the lease changes: TX goes on while the last SET_FREQ is still declared, stops when it isn't */
    setup(&b, RDOF_GUARD_MMDVM, tx, 1);
    b.answers = false;
    rdof_guard_tool(&b.g, f, 17, 0);
    const uint32_t both[] = {446100000, 446200000}, other[] = {446200000};
    rdof_guard_set_tx(&b.g, both, 2, 0);
    b.nrx = b.ntool = 0;
    rdof_guard_tool(&b.g, d, sizeof d, 0);
    CHECK(b.nrx == sizeof d, "still declared");
    rdof_guard_set_tx(&b.g, other, 1, 0);
    b.nrx = b.ntool = 0;
    rdof_guard_tool(&b.g, d, sizeof d, 0);
    CHECK(b.nrx == 0 && b.ntool == 5, "not any more: NAK");

    /* the tool goes away: the modem to idle, TX off */
    setup(&b, RDOF_GUARD_MMDVM, tx, 1);
    rdof_guard_tool(&b.g, f, 17, 0);
    b.nrx = 0;
    rdof_guard_end(&b.g, 0);
    CHECK(b.nrx == 4 && b.rxbuf[2] == 0x03 && b.rxbuf[3] == 0, "SET_MODE idle");
    b.nrx = 0;
    rdof_guard_tool(&b.g, d, sizeof d, 0);
    CHECK(b.nrx == 0, "no TX after");
}

static int parse(const char *json, rdof_bench_radio_t *r, char *err)
{
    return rdof_bench_radios_parse(json, strlen(json), r, RDOF_BENCH_MAX_RADIOS, err, 160);
}

static void test_bench_radios(void)
{
    rdof_bench_radio_t r[RDOF_BENCH_MAX_RADIOS];
    char err[160];
    CHECK(parse("{\"name\": \"box\"}", r, err) == 0, "no bench: none");
    CHECK(parse("{\n // the spec's three\n \"bench\": {\"radios\": [\n"
                "{\"name\": \"odr1\", \"proto\": \"odr-debug\", \"port\": {\"uart\": 1, \"tx\": 17, \"rx\": 18, \"baud\": 921600}},\n"
                "{\"name\": \"hs1\", \"proto\": \"mmdvm\", \"port\": {\"usb\": \"cdc-acm\", \"vid\": \"0483\", \"pid\": \"5740\"}},\n"
                "{\"name\": \"odr2\", \"port\": {\"usb\": \"uart\", \"serial\": \"FT1A2B3C\", \"baud\": 921600}},]}}",
                r, err) == 3, "%s", err);
    CHECK(r[0].port == RDOF_BPORT_UART && r[0].uart == 1 && r[0].tx == 17 && r[0].rx == 18 && r[0].rts == -1 &&
              r[0].baud == 921600, "uart");
    CHECK(r[1].port == RDOF_BPORT_CDC_ACM && r[1].vid == 0x0483 && r[1].pid == 0x5740 && !strcmp(r[1].proto, "mmdvm"),
          "cdc-acm");
    CHECK(r[2].port == RDOF_BPORT_USB_UART && !r[2].vid && !strcmp(r[2].serial, "FT1A2B3C") &&
              !strcmp(r[2].proto, "odr-debug"), "usb uart, proto by default");
    static const char *const bad[] = {
        "{\"bench\": {\"radios\": {}}}",
        "{\"bench\": {\"radios\": [{\"port\": {\"uart\": 1, \"tx\": 1, \"rx\": 2}}]}}",
        "{\"bench\": {\"radios\": [{\"name\": \"a b\", \"port\": {\"uart\": 1, \"tx\": 1, \"rx\": 2}}]}}",
        "{\"bench\": {\"radios\": [{\"name\": \"a\", \"proto\": \"p25\", \"port\": {\"uart\": 1, \"tx\": 1, \"rx\": 2}}]}}",
        "{\"bench\": {\"radios\": [{\"name\": \"a\", \"port\": {\"uart\": 0, \"tx\": 1, \"rx\": 2}}]}}",
        "{\"bench\": {\"radios\": [{\"name\": \"a\", \"port\": {\"uart\": 1, \"tx\": 1}}]}}",
        "{\"bench\": {\"radios\": [{\"name\": \"a\", \"port\": {\"uart\": 1, \"tx\": 1, \"rx\": 1}}]}}",
        "{\"bench\": {\"radios\": [{\"name\": \"a\", \"port\": {\"uart\": 1, \"usb\": \"cdc-acm\", \"tx\": 1, \"rx\": 2}}]}}",
        "{\"bench\": {\"radios\": [{\"name\": \"a\", \"port\": {\"usb\": \"hid\"}}]}}",
        "{\"bench\": {\"radios\": [{\"name\": \"a\", \"port\": {\"usb\": \"uart\", \"vid\": \"0483\", \"pid\": \"5740\"}}]}}",
        "{\"bench\": {\"radios\": [{\"name\": \"a\", \"port\": {\"usb\": \"cdc-acm\", \"pid\": \"5740\"}}]}}",
        "{\"bench\": {\"radios\": [{\"name\": \"a\", \"port\": {\"usb\": \"uart\"}},"
        " {\"name\": \"a\", \"port\": {\"usb\": \"uart\"}}]}}",
        "{\"bench\": {\"radios\": [{\"name\": \"a\", \"port\": {\"usb\": \"cdc-acm\"}}]}}",
        "{\"bench\": {\"radios\": [{\"name\": \"a\", \"port\": {\"usb\": \"cdc-acm\", \"vid\": \"0bda\", \"pid\": \"2838\"}}]}}",
        "{\"bench\": {\"radios\": [{\"name\": \"a\", \"port\": {\"uart\": 1, \"tx\": 1, \"rx\": 2}},"
        " {\"name\": \"b\", \"port\": {\"uart\": 1, \"tx\": 3, \"rx\": 4}}]}}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        CHECK(parse(bad[i], r, err) == -1 && err[0], "bad %zu: %s", i, bad[i]);
    CHECK(rdof_bench_usb_bridge(0x0403, 0x6001) == 'f' && rdof_bench_usb_bridge(0x10C4, 0xEA60) == 'c' &&
              rdof_bench_usb_bridge(0x1A86, 0x7523) == 'h' && !rdof_bench_usb_bridge(0x1A86, 0x55D4), "bridges");
}

static void test_guard_tune(void)
{
    bench_t b;
    setup(&b, RDOF_GUARD_ODR, NULL, 0);
    b.power = 2;
    b.hotspot = true;
    b.busy_left = 2;
    rdof_guard_tune_t t = {.freq_hz = 451775000, .cc = 3, .slot = 1, .tg = 9};
    CHECK(rdof_guard_tune(&b.g, &t, 0), "%s", b.g.tune_error);
    pump(&b, 0);
    CHECK(rdof_guard_tune_state(&b.g) == RDOF_TUNE_RUNNING, "running");
    run(&b, 0, 2);
    CHECK(rdof_guard_tune_state(&b.g) == RDOF_TUNE_OK, "tuned: %s", b.g.tune_error);
    CHECK(b.rx_hz == 451775000 && b.mode == 1 && b.cc == 3 && b.ts == 1 && b.target == 9 && !b.hotspot,
          "rx %u mode %d cc %d ts %d target %u", b.rx_hz, b.mode, b.cc, b.ts, b.target);
    CHECK(b.power == 0 && !rdof_guard_busy(&b.g) && b.ntool == 0, "power %d after the tune's check", b.power);
    CHECK(b.order[0] == 0x0A && b.g.stats.checks == 1, "requests first, then one check");

    setup(&b, RDOF_GUARD_ODR, NULL, 0); /* FM: no color code, slot or target */
    t = (rdof_guard_tune_t){.freq_hz = 146520000, .fm = true, .cc = 3, .slot = 1, .tg = 9};
    rdof_guard_tune(&b.g, &t, 0);
    run(&b, 0, 2);
    CHECK(rdof_guard_tune_state(&b.g) == RDOF_TUNE_OK && b.mode == 0 && b.cc == 0 && b.target == 0, "fm");

    setup(&b, RDOF_GUARD_ODR, NULL, 0);
    b.refuse_fields = true;
    rdof_guard_tune(&b.g, &t, 0);
    run(&b, 0, 2);
    CHECK(rdof_guard_tune_state(&b.g) == RDOF_TUNE_FAILED && strstr(b.g.tune_error, "mode / cc / slot"), "%s",
          b.g.tune_error);

    setup(&b, RDOF_GUARD_ODR, NULL, 0); /* answers OK but stays where it was */
    b.ignore_vfo = true;
    b.rx_hz = 461000000;
    rdof_guard_tune(&b.g, &t, 0);
    run(&b, 0, 2);
    CHECK(rdof_guard_tune_state(&b.g) == RDOF_TUNE_FAILED && strstr(b.g.tune_error, "461.00000"), "%s", b.g.tune_error);

    setup(&b, RDOF_GUARD_ODR, NULL, 0); /* a radio that never answers */
    b.answers = false;
    rdof_guard_tune(&b.g, &t, 0);
    run(&b, 0, 3);
    CHECK(rdof_guard_tune_state(&b.g) == RDOF_TUNE_FAILED && strstr(b.g.tune_error, "no debug link answer"), "%s",
          b.g.tune_error);

    setup(&b, RDOF_GUARD_MMDVM, NULL, 0);
    CHECK(!rdof_guard_tune(&b.g, &t, 0) && strstr(b.g.tune_error, "ODR"), "mmdvm: %s", b.g.tune_error);
}

/* What goes wrong: a radio that won't give its view, won't step down, has no APRS; a PTT too long to hold; MMDVM
   bytes that aren't a frame; an answer that never comes. */
static void test_guard_faults(void)
{
    static bench_t b;
    const uint32_t tx[] = {446100000};
    uint8_t f[64];
    const uint8_t on = 1;
    /* no view: the check fails, and a PTT it held is refused */
    setup(&b, RDOF_GUARD_ODR, tx, 1);
    b.refuse_view = true;
    rdof_guard_tool(&b.g, f, odr(0x07, 3, &on, 1, f), 0);
    pump(&b, 0);
    CHECK(strstr(b.events, "check_failed;refused;") && tool_got(&b, 0xFF, 3) && !b.tx, "no view: %s", b.events);
    /* power that won't come down: a step past power_count + 1 tries, then the check fails */
    setup(&b, RDOF_GUARD_ODR, tx, 1);
    b.power = 3;
    b.stuck_power = true;
    rdof_guard_check(&b.g, 0);
    run(&b, 0, 2);
    CHECK(strstr(b.events, "check_failed;") && b.g.stats.power_steps == 5, "stuck power: %s, %ld steps", b.events,
          (long)b.g.stats.power_steps);
    setup(&b, RDOF_GUARD_ODR, tx, 1);
    b.power = 3;
    b.refuse_power = true;
    rdof_guard_check(&b.g, 0);
    run(&b, 0, 1);
    CHECK(strstr(b.events, "power_step;check_failed;"), "power step refused: %s", b.events);
    /* no APRS: its view never answers, and the check still finishes */
    setup(&b, RDOF_GUARD_ODR, tx, 1);
    b.no_aprs = true;
    rdof_guard_check(&b.g, 0);
    run(&b, 0, 1.5);
    CHECK(!strstr(b.events, "check_failed") && !rdof_guard_busy(&b.g), "no APRS: %s", b.events);
    /* a PTT longer than the guard holds */
    setup(&b, RDOF_GUARD_ODR, tx, 1);
    uint8_t big[40] = {1}, fb[64];
    rdof_guard_tool(&b.g, fb, odr(0x07, 4, big, sizeof big, fb), 0);
    CHECK(strstr(b.events, "refused;") && tool_got(&b, 0xFF, 4), "long PTT: %s", b.events);
    /* the guard's own sequence numbers stay clear of the tool's, and never 0 */
    setup(&b, RDOF_GUARD_ODR, tx, 1);
    rdof_guard_tool(&b.g, f, odr(0x01, 128, NULL, 0, f), 0);
    pump(&b, 0);
    b.nrx = 0;
    rdof_guard_check(&b.g, 0.1);
    CHECK(b.nrx >= 8 && b.rxbuf[2] == 0x09 && b.rxbuf[3] == 0x80, "guard seq %02x", b.rxbuf[3]);
    /* an answer that never comes stops blocking the periodic check */
    setup(&b, RDOF_GUARD_ODR, tx, 1);
    b.answers = false;
    rdof_guard_tool(&b.g, f, odr(0x01, 9, NULL, 0, f), 0);
    b.norder = 0;
    run(&b, 0, 2.9);
    CHECK(b.norder == 0, "outstanding: no check yet");
    run(&b, 3.0, 3.2);
    CHECK(b.norder == 1 && b.order[0] == 0x09, "then the periodic check (%d)", b.norder);
    /* not at the lowest power by choice: the view is the whole check */
    setup(&b, RDOF_GUARD_ODR, tx, 1);
    b.g.lowest = false;
    b.power = 3;
    rdof_guard_check(&b.g, 0);
    pump(&b, 0);
    CHECK(!rdof_guard_busy(&b.g) && b.power == 3 && b.norder == 1, "no power steps: %d views", b.norder);
    /* MMDVM: a length under 3 isn't a frame; its first byte goes on as a byte */
    setup(&b, RDOF_GUARD_MMDVM, tx, 1);
    const uint8_t junk[3] = {0xE0, 0x02, 0x41};
    rdof_guard_tool(&b.g, junk, sizeof junk, 0);
    CHECK(b.nrx == 3 && !memcmp(b.rxbuf, junk, 3), "short MMDVM length: %zu bytes on", b.nrx);
    /* more than it holds through a check: the rest dropped and counted; the frame cut short goes on as bytes */
    setup(&b, RDOF_GUARD_ODR, tx, 1);
    b.answers = false;
    rdof_guard_check(&b.g, 0);
    uint8_t pay[100] = {0}, fr[120];
    const size_t frn = odr(0x01, 5, pay, sizeof pay, fr);
    size_t sent = 0;
    while (sent < RDOF_GUARD_HELD + frn) {
        rdof_guard_tool(&b.g, fr, frn, 0.1);
        sent += frn;
    }
    b.nrx = 0;
    run(&b, 0.2, 1.3); /* the view times out */
    CHECK(b.g.stats.held_dropped == (long)(sent - RDOF_GUARD_HELD) && b.nrx == RDOF_GUARD_HELD,
          "held: %ld dropped, %zu bytes on", (long)b.g.stats.held_dropped, b.nrx);
}

void test_guard(void)
{
    test_guard_faults();
    test_guard_odr();
    test_guard_tune();
    test_guard_mmdvm();
    test_bench_radios();
}
