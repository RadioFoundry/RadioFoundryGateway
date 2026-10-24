// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The bench API's transmit job (rdof/benchtx.h) against a fake MMDVM modem, and bench.tx from the settings. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "rdof/benchcfg.h"
#include "rdof/benchtx.h"
#include "rdof/bits.h"
#include "rdof/layer2.h"
#include "rdof/pdu.h"

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

/* ── bursts ───────────────────────────────────────────────────────────────── */

static void hex(const uint8_t b[33], char out[67])
{
    for (int i = 0; i < 33; i++)
        snprintf(out + 2 * i, 3, "%02x", b[i]);
}

static void lc_burst(int dt, uint8_t out[33])
{
    uint8_t lc[9];
    rdof_group_lc(9990003, 9, false, 0, lc);
    rdof_lc_burst(lc, dt, 1, rdof_data_sync(RDOF_MS_VOICE), out);
}

static void voice_burst(int n, uint8_t out[33])
{
    uint8_t voice[216] = {0}, middle[48], frag[32] = {0};
    if (n == 0)
        rdof_u64_to_bits(RDOF_MS_VOICE, 48, middle);
    else
        rdof_voice_middle(1, n == 1 ? 1 : n == 4 ? 2 : n == 5 ? 0 : 3, frag, 0, middle);
    rdof_voice_burst(voice, middle, out);
}

/* A call as a bursts frame: header, superframes of voice A-F, terminator (each optional). */
static size_t call_frame(char *out, size_t cap, bool header, int voice, bool term)
{
    size_t n = (size_t)snprintf(out, cap, "{\"type\":\"bursts\",\"items\":[");
    uint8_t b[33];
    char h[67];
    bool first = true;
    if (header) {
        lc_burst(RDOF_DT_VOICE_LC_HEADER, b);
        hex(b, h);
        n += (size_t)snprintf(out + n, cap - n, "{\"raw_hex\":\"%s\",\"slot\":1}", h);
        first = false;
    }
    for (int i = 0; i < voice; i++) {
        voice_burst(i % 6, b);
        hex(b, h);
        n += (size_t)snprintf(out + n, cap - n, "%s{\"raw_hex\":\"%s\"}", first ? "" : ",", h);
        first = false;
    }
    if (term) {
        lc_burst(RDOF_DT_TERMINATOR_WITH_LC, b);
        hex(b, h);
        n += (size_t)snprintf(out + n, cap - n, "%s{\"raw_hex\":\"%s\"}", first ? "" : ",", h);
    }
    n += (size_t)snprintf(out + n, cap - n, "]}");
    return n;
}

/* ── a fake MMDVM modem and the tool ──────────────────────────────────────── */

typedef struct {
    rdof_benchtx_t t;
    /* the modem */
    bool silent, nak_freq, nak_data;
    uint8_t pending[256];
    size_t npend;
    uint8_t cmds[256];  /* commands it got, in order */
    int ncmds;
    uint8_t controls[128]; /* each DMR_DATA2's control byte */
    int ndata;
    int modes[16], nmodes;
    size_t config_len;     /* SET_CONFIG's payload */
    uint8_t freq_payload[14];
    bool version2, data_nak_bare, data_nak_zero;
    int drop_first;        /* each setup command's first this many requests unanswered */
    uint8_t seen[256];
    double key_start, key_secs; /* the last keying logged */
    double handed[128];
    /* the tool */
    char frames[8192];
    size_t nframes;
    int keyings;
    char last_why[16];
} txb_t;

static txb_t tb;
static double clock_now;

static void reply(txb_t *b, uint8_t cmd, const uint8_t *p, size_t n)
{
    b->npend += rdof_mmdvm_frame(cmd, p, n, b->pending + b->npend, sizeof b->pending - b->npend);
}

static void to_modem(void *ctx, const uint8_t *f, size_t n)
{
    txb_t *b = ctx;
    if (n < 3 || f[0] != 0xE0)
        return;
    uint8_t cmd = f[2];
    if (b->ncmds < (int)sizeof b->cmds)
        b->cmds[b->ncmds++] = cmd;
    if (cmd == RDOF_MMDVM_DMR_DATA2) {
        if (b->ndata < 128) {
            b->handed[b->ndata] = clock_now;
            b->controls[b->ndata++] = f[3];
        }
        if (b->nak_data) {
            uint8_t p[2] = {cmd, 4};
            reply(b, RDOF_MMDVM_NAK, p, 2);
        } else if (b->data_nak_bare) {
            reply(b, RDOF_MMDVM_NAK, &cmd, 1);
        } else if (b->data_nak_zero) {
            uint8_t p[2] = {cmd, 0};
            reply(b, RDOF_MMDVM_NAK, p, 2);
        }
        return;
    }
    if (cmd == RDOF_MMDVM_SET_MODE && b->nmodes < 16)
        b->modes[b->nmodes++] = f[3];
    if (cmd == RDOF_MMDVM_SET_CONFIG)
        b->config_len = n - 3;
    if (cmd == RDOF_MMDVM_SET_FREQ && n == 17)
        memcpy(b->freq_payload, f + 3, 14);
    if (b->silent || b->seen[cmd]++ < b->drop_first)
        return;
    if (cmd == RDOF_MMDVM_GET_VERSION) {
        uint8_t p[] = {1, 'M', 'M', 'D', 'V', 'M', '_', 'H', 'S'};
        p[0] = b->version2 ? 2 : 1;
        reply(b, cmd, p, sizeof p);
    } else if (cmd == RDOF_MMDVM_SET_FREQ && b->nak_freq) {
        uint8_t p[2] = {cmd, 4};
        reply(b, RDOF_MMDVM_NAK, p, 2);
    } else {
        reply(b, RDOF_MMDVM_ACK, &cmd, 1);
    }
}

static void to_tool(void *ctx, const char *js, size_t n)
{
    txb_t *b = ctx;
    if (b->nframes + n + 2 < sizeof b->frames) {
        memcpy(b->frames + b->nframes, js, n);
        b->nframes += n;
        b->frames[b->nframes++] = '\n';
        b->frames[b->nframes] = 0;
    }
}

static void keying(void *ctx, double start, double secs, int bursts, const char *why)
{
    txb_t *b = ctx;
    b->keyings++;
    b->key_start = start;
    b->key_secs = secs;
    snprintf(b->last_why, sizeof b->last_why, "%s", why);
}

static void deliver(txb_t *b)
{
    if (b->npend) {
        uint8_t tmp[256];
        size_t n = b->npend;
        memcpy(tmp, b->pending, n);
        b->npend = 0;
        rdof_benchtx_modem(&b->t, tmp, n, clock_now);
    }
}

static void start(double max_key_s)
{
    memset(&tb, 0, sizeof tb);
    clock_now = 100.0;
    const rdof_benchtx_io_t io = {to_modem, to_tool, keying, NULL, &tb};
    rdof_benchtx_init(&tb.t, &io, 461137500, 1, 0, max_key_s, clock_now);
}

/* Time passing in 20 ms ticks, the modem answering; the tool pings every 0.5 s when ping. */
static void run(double seconds, bool ping)
{
    double end = clock_now + seconds, last_ping = clock_now;
    while (clock_now < end - 1e-9) {
        clock_now += 0.020;
        deliver(&tb);
        if (ping && clock_now - last_ping >= 0.5) {
            rdof_benchtx_tool(&tb.t, "{\"type\":\"ping\"}", 15, clock_now);
            last_ping = clock_now;
        }
        rdof_benchtx_tick(&tb.t, clock_now);
    }
}

static void tool_says(const char *js) { rdof_benchtx_tool(&tb.t, js, strlen(js), clock_now); }

static bool said(const char *needle) { return strstr(tb.frames, needle) != NULL; }

/* ── the tests ────────────────────────────────────────────────────────────── */

static void t_gates(void)
{
    rdof_benchtx_policy_t p = {.freqs_hz = {461137500}, .nfreqs = 1, .max_key_s = 10, .arm_gpio = -1};
    const uint32_t lease[] = {461137500}, other[] = {461087500};
    CHECK(!rdof_benchtx_gates(&p, true, true, 461137500, lease, 1, false), "all open");
    CHECK(rdof_benchtx_gates(&p, false, true, 461137500, lease, 1, false), "built without TX");
    CHECK(rdof_benchtx_gates(&p, true, false, 461137500, lease, 1, false), "the path can't");
    CHECK(rdof_benchtx_gates(&p, true, true, 461087500, other, 1, false), "not in the list");
    CHECK(rdof_benchtx_gates(&p, true, true, 461137500, other, 1, false), "the lease doesn't cover it");
    CHECK(rdof_benchtx_gates(&p, true, true, 461137500, lease, 0, false), "no lease");
    p.arm_gpio = 5;
    CHECK(rdof_benchtx_gates(&p, true, true, 461137500, lease, 1, false), "the jumper is out");
    CHECK(!rdof_benchtx_gates(&p, true, true, 461137500, lease, 1, true), "the jumper is in");
    rdof_benchtx_policy_t none = {.arm_gpio = -1};
    CHECK(rdof_benchtx_gates(&none, true, true, 461137500, lease, 1, true), "no bench.tx");
}

static void t_control(void)
{
    uint8_t b[33];
    int vn = -1;
    lc_burst(RDOF_DT_VOICE_LC_HEADER, b);
    CHECK(rdof_benchtx_control(b, -1, &vn) == (RDOF_SYNC_DATA | 1), "voice LC header");
    voice_burst(1, b);
    CHECK(rdof_benchtx_control(b, -1, &vn) == -1, "voice B with no voice sync before it");
    voice_burst(0, b);
    CHECK(rdof_benchtx_control(b, -1, &vn) == RDOF_SYNC_AUDIO, "voice A");
    for (int n = 1; n <= 5; n++) {
        voice_burst(n, b);
        int c = rdof_benchtx_control(b, -1, &vn);
        CHECK(c == n, "voice %c: %d", 'A' + n, c);
    }
    voice_burst(1, b);
    CHECK(rdof_benchtx_control(b, 3, &vn) == 3, "n given");
    lc_burst(RDOF_DT_TERMINATOR_WITH_LC, b);
    CHECK(rdof_benchtx_control(b, -1, &vn) == (RDOF_SYNC_DATA | 2), "terminator");
}

static void t_call(void)
{
    start(10);
    run(0.2, true);
    CHECK(tb.ncmds >= 4 && tb.cmds[0] == RDOF_MMDVM_GET_VERSION && tb.cmds[1] == RDOF_MMDVM_SET_CONFIG &&
              tb.cmds[2] == RDOF_MMDVM_SET_FREQ && tb.cmds[3] == RDOF_MMDVM_SET_MODE,
          "setup order");
    CHECK(said("\"type\":\"ready\""), "ready: %s", tb.frames);
    CHECK(!strcmp(rdof_benchtx_state(&tb.t), "armed"), "armed: %s", rdof_benchtx_state(&tb.t));
    static char f[8192];
    call_frame(f, sizeof f, true, 12, true); /* 14 bursts, 0.84 s */
    tool_says(f);
    CHECK(said("\"type\":\"ack\",\"bursts\":14") && said("\"timed\":false"), "ack: %s", tb.frames);
    run(0.1, true);
    CHECK(!strcmp(rdof_benchtx_state(&tb.t), "keyed") && said("\"type\":\"keyed\""), "keyed");
    run(1.5, true);
    CHECK(tb.ndata == 14, "14 bursts handed over: %d", tb.ndata);
    CHECK(tb.controls[0] == (RDOF_SYNC_DATA | 1) && tb.controls[1] == RDOF_SYNC_AUDIO && tb.controls[2] == 1 &&
              tb.controls[6] == 5 && tb.controls[7] == RDOF_SYNC_AUDIO && tb.controls[13] == (RDOF_SYNC_DATA | 2),
          "control bytes");
    /* paced: never more than LEAD + 1 bursts ahead of the first one's 60 ms grid */
    bool paced = true;
    for (int i = 0; i < tb.ndata; i++)
        paced &= tb.handed[i] >= tb.handed[0] + (i - RDOF_BENCHTX_LEAD - 1) * RDOF_BENCHTX_BURST_S - 1e-6;
    CHECK(paced, "pacing");
    CHECK(said("\"type\":\"unkeyed\",\"why\":\"end\"") && !said("underrun"), "unkeyed at the end: %s", tb.frames);
    CHECK(tb.keyings == 1 && !strcmp(tb.last_why, "end"), "one keying logged");
    CHECK(tb.nmodes == 1, "no SET_MODE idle after a call that ended: %d", tb.nmodes);
    rdof_benchtx_stop(&tb.t, NULL, "closed", clock_now);
    CHECK(rdof_benchtx_done(&tb.t) && tb.modes[tb.nmodes - 1] == RDOF_MODE_IDLE && said("\"type\":\"done\""), "closed");
}

static void t_underrun(void)
{
    start(10);
    run(0.2, true);
    static char f[8192];
    call_frame(f, sizeof f, true, 5, false);
    tool_says(f);
    run(1.0, true);
    CHECK(said("\"type\":\"underrun\"") && said("\"why\":\"underrun\""), "underrun: %s", tb.frames);
    CHECK(tb.t.underruns == 1, "counted");
    /* the call goes on: keyed again */
    call_frame(f, sizeof f, false, 6, true);
    tool_says(f);
    run(1.0, true);
    CHECK(tb.t.keyings == 2 && tb.ndata == 13, "keyed again: %ld keyings, %d bursts", tb.t.keyings, tb.ndata);
}

static void t_max_key(void)
{
    start(0.5);
    run(0.2, true);
    static char f[8192];
    call_frame(f, sizeof f, true, 12, true); /* 0.84 s */
    tool_says(f);
    CHECK(rdof_benchtx_done(&tb.t) && said("\"code\":\"refused\"") && tb.ndata == 0, "refused upfront: %s", tb.frames);
    start(0.5);
    run(0.2, true);
    call_frame(f, sizeof f, true, 5, false); /* 0.36 s */
    tool_says(f);
    run(0.1, true);
    call_frame(f, sizeof f, false, 3, false); /* 0.18 more, while keyed: 0.54 s */
    tool_says(f);
    CHECK(rdof_benchtx_done(&tb.t) && said("\"code\":\"refused\""), "refused over the keying: %s", tb.frames);
    CHECK(tb.modes[tb.nmodes - 1] == RDOF_MODE_IDLE, "the modem idle");
}

static void t_deadman(void)
{
    start(10);
    run(0.2, true);
    static char f[8192];
    call_frame(f, sizeof f, true, 40, true); /* 2.5 s */
    tool_says(f);
    run(2.5, false);
    CHECK(rdof_benchtx_done(&tb.t) && said("\"why\":\"deadman\"") && said("\"code\":\"timeout\""), "dead-man: %s",
          tb.frames);
    CHECK(tb.ndata < 42 && tb.modes[tb.nmodes - 1] == RDOF_MODE_IDLE, "stopped mid-call: %d bursts", tb.ndata);
    CHECK(!strcmp(tb.last_why, "deadman"), "logged");
    /* idle, no pings: the job ends too */
    start(10);
    run(2.5, false);
    CHECK(rdof_benchtx_done(&tb.t) && said("\"code\":\"timeout\"") && tb.keyings == 0, "idle dead-man");
}

static void t_unkey(void)
{
    start(10);
    run(0.2, true);
    static char f[8192];
    call_frame(f, sizeof f, true, 30, true);
    tool_says(f);
    run(0.3, true);
    tool_says("{\"type\":\"unkey\"}");
    CHECK(said("\"why\":\"unkey\"") && tb.modes[tb.nmodes - 1] == RDOF_MODE_IDLE, "unkeyed: %s", tb.frames);
    int handed = tb.ndata;
    run(0.5, true);
    CHECK(tb.ndata == handed, "nothing more handed over");
    call_frame(f, sizeof f, true, 6, true);
    tool_says(f);
    run(1.0, true);
    CHECK(tb.modes[tb.nmodes - 1] == RDOF_MODE_DMR && tb.ndata == handed + 8, "DMR mode again, then the call: %d",
          tb.ndata - handed);
    CHECK(!rdof_benchtx_done(&tb.t), "still open");
}

static void t_refusals(void)
{
    start(10);
    tb.nak_freq = true;
    run(0.3, true);
    CHECK(rdof_benchtx_done(&tb.t) && said("\"code\":\"refused\"") && said("SET_FREQ"), "SET_FREQ refused: %s",
          tb.frames);
    start(10);
    tb.silent = true;
    tb.npend = 0; /* GET_VERSION's answer, queued before the modem went silent */
    run(4.0, true);
    CHECK(rdof_benchtx_done(&tb.t) && said("\"code\":\"unavailable\"") && tb.ncmds == RDOF_BENCHTX_TRIES + 1 &&
              tb.cmds[RDOF_BENCHTX_TRIES] == RDOF_MMDVM_SET_MODE,
          "no answer: %d tries, %s", tb.ncmds, tb.frames);
    start(10);
    tb.nak_data = true;
    run(0.2, true);
    static char f[8192];
    call_frame(f, sizeof f, true, 6, true);
    tool_says(f);
    run(0.3, true);
    CHECK(rdof_benchtx_done(&tb.t) && said("refused a burst") && tb.modes[tb.nmodes - 1] == RDOF_MODE_IDLE,
          "a burst refused: %s", tb.frames);
    start(10);
    tool_says("{\"type\":\"bursts\",\"items\":[{\"raw_hex\":\"zz\"}]}");
    CHECK(rdof_benchtx_done(&tb.t) && said("bad_request"), "bad hex");
    start(10);
    tool_says("{not json");
    CHECK(rdof_benchtx_done(&tb.t) && said("bad_request"), "not JSON");
    start(10);
    tool_says("{\"type\":\"fm\",\"tone_hz\":1000}");
    CHECK(rdof_benchtx_done(&tb.t) && said("unsupported"), "no FM through a modem");
    start(10);
    run(0.2, true);
    uint8_t b[33]; /* voice C with no voice A before it: it can't be placed */
    char h[67];
    voice_burst(2, b);
    hex(b, h);
    snprintf(f, sizeof f, "{\"type\":\"bursts\",\"items\":[{\"raw_hex\":\"%s\"}]}", h);
    tool_says(f);
    CHECK(rdof_benchtx_done(&tb.t) && said("no sync"), "voice C alone: %s", tb.frames);
}

static double fixed_utc(void *ctx) { return 1700000000.25; }

/* The first frame to the tool that starts with prefix, whole; "" when there's none. */
static const char *frame(const char *prefix)
{
    static char line[300];
    const char *s = strstr(tb.frames, prefix);
    line[0] = 0;
    if (s) {
        size_t n = strcspn(s, "\n");
        snprintf(line, sizeof line, "%.*s", (int)n, s);
    }
    return line;
}

static void one_burst(char *f, size_t cap, const uint8_t b[33], const char *extra)
{
    char h[67];
    hex(b, h);
    snprintf(f, cap, "{\"type\":\"bursts\",\"items\":[{\"raw_hex\":\"%s\"%s}]}", h, extra);
}

static void t_edges(void)
{
    static char f[8192];
    uint8_t b[33];
    /* each frame's whole text */
    start(10);
    CHECK(!strcmp(rdof_benchtx_state(&tb.t), "setup"), "setup first");
    run(0.2, true);
    CHECK(!strcmp(frame("{\"type\":\"ready\""), "{\"type\":\"ready\",\"freq_hz\":461137500,\"protocol\":1}"), "ready: %s",
          frame("{\"type\":\"ready\""));
    CHECK(tb.config_len == 23 && tb.freq_payload[9] == 0, "protocol 1: the short config; RF level 0");
    call_frame(f, sizeof f, true, 12, true);
    tool_says(f);
    CHECK(!strcmp(frame("{\"type\":\"ack\""),
                  "{\"type\":\"ack\",\"bursts\":14,\"queued\":14,\"queued_s\":0.840,\"timed\":false}"),
          "ack: %s", frame("{\"type\":\"ack\""));
    run(1.5, true);
    CHECK(!strcmp(frame("{\"type\":\"unkeyed\""),
                  "{\"type\":\"unkeyed\",\"why\":\"end\",\"t\":101.080000,\"seconds\":0.840,\"bursts\":14}"),
          "unkeyed: %s", frame("{\"type\":\"unkeyed\""));
    rdof_benchtx_stop(&tb.t, NULL, "closed", clock_now);
    CHECK(!strcmp(frame("{\"type\":\"done\""), "{\"type\":\"done\",\"why\":\"closed\",\"bursts\":14,\"keyings\":1,\"underruns\":0}"),
          "done: %s", frame("{\"type\":\"done\""));
    CHECK(!strcmp(rdof_benchtx_state(&tb.t), "idle") && !tb.t.error[0], "idle after, no error");
    long frames = tb.t.frames;
    tool_says("{\"type\":\"ping\"}");
    rdof_benchtx_tick(&tb.t, clock_now + 10);
    CHECK(tb.t.frames == frames, "done: frames ignored");
    /* the error frame: quotes and backslashes out, cut at 117; t->error cut at 95 */
    start(10);
    rdof_benchtx_stop(&tb.t, "x", "a\"b\\c", clock_now);
    CHECK(!strcmp(frame("{\"type\":\"error\""), "{\"type\":\"error\",\"code\":\"x\",\"error\":\"abc\"}") &&
              !strcmp(tb.t.error, "a\"b\\c"),
          "escaped: %s", frame("{\"type\":\"error\""));
    CHECK(tb.modes[tb.nmodes - 1] == RDOF_MODE_IDLE && tb.nmodes == 1, "stopped unkeyed: idle once");
    rdof_benchtx_stop(&tb.t, "y", "again", clock_now);
    CHECK(tb.nmodes == 1 && !strstr(tb.frames, "again"), "a second stop: nothing");
    char longwhy[200];
    memset(longwhy, 'w', sizeof longwhy - 1);
    longwhy[sizeof longwhy - 1] = 0;
    start(10);
    rdof_benchtx_stop(&tb.t, "x", longwhy, clock_now);
    CHECK(strlen(frame("{\"type\":\"error\"")) == strlen("{\"type\":\"error\",\"code\":\"x\",\"error\":\"\"}") + 118 &&
              strlen(tb.t.error) == 95,
          "cut: %zu, %zu", strlen(frame("{\"type\":\"error\"")), strlen(tb.t.error));
    /* a frame past 255 bytes is cut there, as snprintf into 256 */
    char longcode[300];
    memset(longcode, 'c', sizeof longcode - 1);
    longcode[sizeof longcode - 1] = 0;
    start(10);
    rdof_benchtx_stop(&tb.t, longcode, "x", clock_now);
    CHECK(strlen(frame("{\"type\":\"error\"")) == 255 && strlen(frame("{\"type\":\"done\"")) == 255, "cut at 255: %zu",
          strlen(frame("{\"type\":\"error\"")));
    /* an underrun leaves the modem in DMR */
    start(10);
    run(0.2, true);
    call_frame(f, sizeof f, true, 5, false);
    tool_says(f);
    run(1.0, true);
    CHECK(said("\"why\":\"underrun\"") && tb.nmodes == 1 && !tb.t.need_mode, "underrun: no SET_MODE idle");
    /* unkeyed early: the keying's start, and its length to now, not to the queue's end */
    start(10);
    run(0.2, true);
    call_frame(f, sizeof f, true, 30, true);
    tool_says(f);
    run(0.3, true);
    const double ks = tb.t.key_start;
    int nm = tb.nmodes;
    tool_says("{\"type\":\"unkey\"}");
    CHECK(tb.key_start == ks && fabs(tb.key_secs - (clock_now - ks)) < 1e-9 && tb.nmodes == nm + 1,
          "unkey: %f %f", tb.key_start, tb.key_secs);
    /* after an unkey, an empty queue leaves the modem idle; the next bursts set DMR first */
    run(0.5, true);
    CHECK(tb.modes[tb.nmodes - 1] == RDOF_MODE_IDLE && tb.nmodes == nm + 1 && !strcmp(rdof_benchtx_state(&tb.t), "armed"),
          "idle until there's something to send");
    call_frame(f, sizeof f, true, 0, true);
    tool_says(f);
    run(0.3, true);
    CHECK(tb.modes[tb.nmodes - 1] == RDOF_MODE_DMR && tb.nmodes == nm + 2, "DMR again for the next call");
    /* stopped while keyed: SET_MODE idle once */
    start(10);
    run(0.2, true);
    call_frame(f, sizeof f, true, 30, true);
    tool_says(f);
    run(0.3, true);
    nm = tb.nmodes;
    rdof_benchtx_stop(&tb.t, NULL, "closed", clock_now);
    CHECK(tb.nmodes == nm + 1 && tb.modes[nm] == RDOF_MODE_IDLE, "keyed stop: idle once (%d)", tb.nmodes - nm);
    /* the ack's queued_s counts what's on the air ahead */
    start(10);
    run(0.2, true);
    call_frame(f, sizeof f, true, 12, true);
    tool_says(f);
    run(0.3, true);
    tb.nframes = 0;
    tb.frames[0] = 0;
    call_frame(f, sizeof f, false, 2, false);
    tool_says(f);
    char want[64];
    snprintf(want, sizeof want, "\"queued_s\":%.3f,", tb.t.qn * RDOF_BENCHTX_BURST_S + (tb.t.air_free - clock_now));
    CHECK(tb.t.air_free > clock_now && said(want), "%s in %s", want, tb.frames);
    /* raw_hex longer than a burst */
    start(10);
    voice_burst(0, b);
    char hx[67];
    hex(b, hx);
    snprintf(f, sizeof f, "{\"type\":\"bursts\",\"items\":[{\"raw_hex\":\"%s00\"}]}", hx);
    tool_says(f);
    CHECK(!strcmp(tb.t.error, "each item has raw_hex: the 33-byte burst"), "68 digits: %s", tb.t.error);
    /* an ACK for another command isn't the reply */
    start(10);
    tb.npend = 0;
    rdof_benchtx_modem(&tb.t, (const uint8_t[]){0xE0, 4, RDOF_MMDVM_ACK, RDOF_MMDVM_SET_MODE}, 4, clock_now);
    rdof_benchtx_tick(&tb.t, clock_now);
    CHECK(tb.t.step == 0 && !tb.t.replied, "another command's ACK");
    /* each step gets its own tries */
    start(10);
    tb.drop_first = RDOF_BENCHTX_TRIES - 1;
    tb.npend = 0;
    tb.seen[RDOF_MMDVM_GET_VERSION] = 1;
    run(4 * RDOF_BENCHTX_TRIES * RDOF_BENCHTX_REPLY_S, true);
    CHECK(said("\"type\":\"ready\"") && !rdof_benchtx_done(&tb.t), "two misses a step: %s", tb.frames);
    /* the dead-man: 2 s is fine, past it isn't */
    start(10);
    rdof_benchtx_tick(&tb.t, 100.0 + RDOF_BENCHTX_DEADMAN_S);
    CHECK(!rdof_benchtx_done(&tb.t), "2 s");
    rdof_benchtx_tick(&tb.t, 100.0 + RDOF_BENCHTX_DEADMAN_S + 0.01);
    CHECK(rdof_benchtx_done(&tb.t) && said("\"code\":\"timeout\""), "2.01 s: %s", tb.frames);
    /* UTC from the platform; protocol 2; the RF level clamped */
    memset(&tb, 0, sizeof tb);
    tb.version2 = true;
    clock_now = 100.0;
    const rdof_benchtx_io_t io = {to_modem, to_tool, keying, fixed_utc, &tb};
    rdof_benchtx_init(&tb.t, &io, 461137500, 1, 300, 10, clock_now);
    run(0.2, true);
    CHECK(tb.config_len == 37 && tb.freq_payload[9] == 255 && said("\"protocol\":2"), "protocol 2, RF 255: %zu",
          tb.config_len);
    call_frame(f, sizeof f, true, 0, true);
    tool_says(f);
    run(0.5, true);
    CHECK(!strcmp(frame("{\"type\":\"keyed\""), "{\"type\":\"keyed\",\"t\":1700000000.250000,\"freq_hz\":461137500}"),
          "keyed: %s", frame("{\"type\":\"keyed\""));
    memset(&tb, 0, sizeof tb);
    rdof_benchtx_init(&tb.t, &io, 461137500, 1, -5, 10, clock_now);
    run(0.2, true);
    CHECK(tb.freq_payload[9] == 0, "RF -5: 0");
    /* a burst NAKed with reason 0, and with none */
    start(10);
    tb.data_nak_zero = true;
    run(0.2, true);
    call_frame(f, sizeof f, true, 0, true);
    tool_says(f);
    run(0.3, true);
    CHECK(rdof_benchtx_done(&tb.t) && said("refused a burst (NAK reason 0)"), "reason 0: %s", tb.frames);
    start(10);
    tb.data_nak_bare = true;
    run(0.2, true);
    tool_says(f);
    run(0.3, true);
    CHECK(rdof_benchtx_done(&tb.t) && said("refused a burst (no NAK reason)"), "no reason: %s", tb.frames);
    /* unkey while not keyed drops the voice count too: voice B can't follow */
    start(10);
    voice_burst(0, b);
    one_burst(f, sizeof f, b, "");
    tool_says(f);
    tool_says("{\"type\":\"unkey\"}");
    CHECK(tb.t.qn == 0 && tb.t.voice_n == -1, "dropped");
    voice_burst(1, b);
    one_burst(f, sizeof f, b, "");
    tool_says(f);
    CHECK(rdof_benchtx_done(&tb.t) && said("no sync"), "voice B after an unkey: %s", tb.frames);
    /* n: a number in range places it; anything else counts from the sync */
    static const struct {
        const char *n;
        int control;
    } ns[] = {{",\"n\":3", 3}, {",\"n\":3.9", 3}, {",\"n\":1e300", 1}, {",\"n\":-1e300", 1}, {",\"n\":\"4\"", 1},
              {",\"n\":null", 1}, {",\"n\":6", 1}, {",\"n\":0", 1}, {",\"n\":5", 5}};
    for (size_t i = 0; i < sizeof ns / sizeof ns[0]; i++) {
        start(10);
        voice_burst(0, b);
        one_burst(f, sizeof f, b, "");
        tool_says(f);
        voice_burst(1, b);
        one_burst(f, sizeof f, b, ns[i].n);
        tool_says(f);
        CHECK(!rdof_benchtx_done(&tb.t) && tb.t.qn == 2 && tb.t.q[1].control == ns[i].control, "%s: %d", ns[i].n,
              tb.t.q[1].control);
    }
    /* after voice F, a burst with no sync has nowhere to go */
    start(10);
    for (int i = 0; i < 6; i++) {
        voice_burst(i, b);
        one_burst(f, sizeof f, b, "");
        tool_says(f);
    }
    voice_burst(1, b);
    one_burst(f, sizeof f, b, "");
    tool_says(f);
    CHECK(rdof_benchtx_done(&tb.t) && !strcmp(tb.t.error, "item 0 has no sync and isn't voice B-F after a voice sync"),
          "past F: %s", tb.t.error);
    /* bad frames, each one's message */
    static const struct {
        const char *frame, *error;
    } badf[] = {
        {"{\"type\":\"bursts\"}", "a bursts frame has an items list"},
        {"{\"type\":\"bursts\",\"items\":{}}", "a bursts frame has an items list"},
        {"{\"type\":\"bursts\",\"items\":[{}]}", "each item has raw_hex: the 33-byte burst"},
        {"{\"type\":\"bursts\",\"items\":[{\"raw_hex\":5}]}", "each item has raw_hex: the 33-byte burst"},
        {"{\"type\":5}", "unknown frame type"},
        {"{}", "unknown frame type"},
        {"[]", "unknown frame type"},
        {"", "not JSON"},
        {"{\"type\":\"iq\"}", "a modem sends DMR bursts only: no FM tone or I/Q through it"},
    };
    for (size_t i = 0; i < sizeof badf / sizeof badf[0]; i++) {
        start(10);
        tool_says(badf[i].frame);
        CHECK(rdof_benchtx_done(&tb.t) && !strcmp(tb.t.error, badf[i].error), "%s: %s", badf[i].frame, tb.t.error);
    }
    start(10);
    voice_burst(0, b);
    char h[67];
    hex(b, h);
    h[65] = 'g';
    snprintf(f, sizeof f, "{\"type\":\"bursts\",\"items\":[{\"raw_hex\":\"%s\"}]}", h);
    tool_says(f);
    CHECK(!strcmp(tb.t.error, "raw_hex isn't hex"), "the last digit: %s", tb.t.error);
    hex(b, h);
    for (int i = 0; i < 66; i++)
        if (h[i] >= 'a' && h[i] <= 'f')
            h[i] = (char)(h[i] - 'a' + 'A');
    start(10);
    snprintf(f, sizeof f, "{\"type\":\"bursts\",\"items\":[{\"raw_hex\":\"%s\"}]}", h);
    tool_says(f);
    CHECK(!rdof_benchtx_done(&tb.t) && tb.t.qn == 1 && tb.t.q[0].burst[32] == b[32], "upper case hex");
    /* the queue's size: 64 fit, 65 don't */
    start(100);
    call_frame(f, sizeof f, true, 62, true);
    tool_says(f);
    CHECK(!rdof_benchtx_done(&tb.t) && tb.t.qn == 64, "64 queued");
    start(100);
    call_frame(f, sizeof f, true, 63, true);
    tool_says(f);
    CHECK(rdof_benchtx_done(&tb.t) && strstr(tb.t.error, "more bursts than the queue holds"), "65: %s", tb.t.error);
    /* the key time message */
    start(0.5);
    call_frame(f, sizeof f, true, 12, true);
    tool_says(f);
    CHECK(!strcmp(tb.t.error, "that's 0.84 s of key time; the limit is 0.5 s"), "%s", tb.t.error);
    /* a refused SET_CONFIG: unavailable */
    start(10);
    tb.t.step = 1;
    tb.t.want = RDOF_MMDVM_SET_CONFIG;
    const uint8_t nak[2] = {RDOF_MMDVM_SET_CONFIG, 1};
    reply(&tb, RDOF_MMDVM_NAK, nak, 2);
    tb.npend = 0;
    rdof_benchtx_modem(&tb.t, (const uint8_t[]){0xE0, 5, RDOF_MMDVM_NAK, RDOF_MMDVM_SET_CONFIG, 1}, 5, clock_now);
    rdof_benchtx_tick(&tb.t, clock_now);
    CHECK(said("\"code\":\"unavailable\"") && !strcmp(tb.t.error, "the modem refused SET_CONFIG"), "%s", tb.t.error);
    /* replies nobody waits for change nothing */
    start(10);
    run(0.2, true);
    const int p0 = tb.t.protocol;
    rdof_benchtx_modem(&tb.t, (const uint8_t[]){0xE0, 4, RDOF_MMDVM_GET_VERSION, 2}, 4, clock_now);
    rdof_benchtx_modem(&tb.t, (const uint8_t[]){0xE0, 5, RDOF_MMDVM_NAK, RDOF_MMDVM_SET_FREQ, 1}, 5, clock_now);
    rdof_benchtx_modem(&tb.t, (const uint8_t[]){0xE0, 3, RDOF_MMDVM_NAK}, 3, clock_now);
    CHECK(!rdof_benchtx_done(&tb.t) && tb.t.protocol == p0 && !tb.t.nak, "unawaited replies");
    /* alive keeps the dead-man off */
    start(10);
    for (int i = 0; i < 20; i++) {
        clock_now += 0.2;
        deliver(&tb);
        rdof_benchtx_alive(&tb.t, clock_now);
        rdof_benchtx_tick(&tb.t, clock_now);
    }
    CHECK(!rdof_benchtx_done(&tb.t), "alive for 4 s");
    /* no callbacks but to_tool: nothing breaks */
    memset(&tb, 0, sizeof tb);
    const rdof_benchtx_io_t bare = {NULL, to_tool, NULL, NULL, &tb};
    rdof_benchtx_init(&tb.t, &bare, 461137500, 1, 0, 10, clock_now);
    for (int i = 0; i < 200; i++) {
        rdof_benchtx_alive(&tb.t, clock_now += 0.02);
        rdof_benchtx_tick(&tb.t, clock_now);
    }
    CHECK(rdof_benchtx_done(&tb.t) && said("didn't answer GET_VERSION"), "no modem: %s", tb.t.error);
    /* gates at their edges */
    rdof_benchtx_policy_t p = {.freqs_hz = {461137500}, .nfreqs = 1, .max_key_s = 10, .arm_gpio = -1};
    const uint32_t lease[] = {461137500};
    CHECK(!strcmp(rdof_benchtx_gates(&p, true, true, 461137500, lease, -1, true),
                  "the session holds no TX lease on that frequency"),
          "a negative lease count");
    p.max_key_s = 0;
    CHECK(!strcmp(rdof_benchtx_gates(&p, true, true, 461137500, lease, 1, true),
                  "the settings allow no TX (bench.tx: freqs_hz and max_key_s)"),
          "no key time");
    p.max_key_s = 10;
    p.nfreqs = 0;
    CHECK(rdof_benchtx_gates(&p, true, true, 461137500, lease, 1, true), "no frequencies");
    p.nfreqs = 2;
    p.freqs_hz[1] = 461087500;
    const uint32_t lease2[] = {1, 461087500};
    CHECK(!rdof_benchtx_gates(&p, true, true, 461087500, lease2, 2, true), "the second of each");
    p.arm_gpio = 0;
    CHECK(!strcmp(rdof_benchtx_gates(&p, true, true, 461087500, lease2, 2, false), "the TX arm jumper is out"), "GPIO 0");
}

static void t_policy(void)
{
    rdof_benchtx_policy_t p;
    char err[160];
    const char *a = "{\"bench\": {\"tx\": {\"freqs_hz\": [461137500, 461087500], \"max_key_s\": 5, \"arm_gpio\": 4}}}";
    CHECK(rdof_bench_tx_parse(a, strlen(a), &p, err, sizeof err) == 0 && p.nfreqs == 2 && p.freqs_hz[1] == 461087500 &&
              p.max_key_s == 5 && p.rf_level == 0 && p.arm_gpio == 4,
          "parsed: %s", err);
    const char *b = "{\"bench\": {\"radios\": []}}";
    CHECK(rdof_bench_tx_parse(b, strlen(b), &p, err, sizeof err) == 0 && p.nfreqs == 0 && p.arm_gpio == -1, "none");
    const char *c = "{\"bench\": {\"tx\": {\"freqs_hz\": [461.1375]}}}";
    CHECK(rdof_bench_tx_parse(c, strlen(c), &p, err, sizeof err) < 0 && strstr(err, "whole Hz"), "MHz: %s", err);
    const char *d = "{\"bench\": {\"tx\": {\"freqs_hz\": [461137500], \"max_key_s\": 120}}}";
    CHECK(rdof_bench_tx_parse(d, strlen(d), &p, err, sizeof err) < 0 && strstr(err, "max_key_s"), "too long: %s", err);
    const char *e = "{\"bench\": {\"tx\": {\"freqs_hz\": [461137500]}}}";
    CHECK(rdof_bench_tx_parse(e, strlen(e), &p, err, sizeof err) == 0 && p.max_key_s == 10, "default key time");
}

void test_benchtx(void)
{
    t_gates();
    t_control();
    t_call();
    t_underrun();
    t_max_key();
    t_deadman();
    t_unkey();
    t_refusals();
    t_policy();
    t_edges();
}
