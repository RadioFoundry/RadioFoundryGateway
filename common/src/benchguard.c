/* SPDX-License-Identifier: MIT */
/* The bench radio link's guard (rdof/benchguard.h): a port of the authors' bench daemon's station guard (MIT; PROVENANCE.md). */
#include "rdof/benchguard.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* RadioFoundry debug link (RadioFoundry docs/spec/debuglink.md) */
#define HDR 6
#define CMD_PTT 0x07
#define CMD_GET_VIEW 0x09
#define CMD_RADIO 0x0A
#define CMD_APRS 0x11
#define CMD_ERROR 0xFF
#define OP_VFO 4
#define OP_VFO_SET 5
#define OP_POWER 7
#define OP_HOTSPOT_EXIT 14
#define OP_DMR_TARGET 15
#define OP_DMR_FIELDS 31
#define ST_BUSY 3
#define ST_UNAVAILABLE 4
#define APRS_OP_SET 5
#define APRS_F_POWER 24
#define ST_REFUSED 7
#define VIEW_CORE 0
#define VIEW_APRS_SET 14
#define POWER_PAUSE_S 0.08
#define OUTSTANDING_S 3.0
#define TUNE_GAP_S 0.03    /* RadioFoundry takes one RADIO request per 20 ms */
#define TUNE_BUSY_S 0.06
#define TUNE_BUSY_TRIES 20
#define TUNE_SETTLE_S 0.25 /* the radio core applies a request after answering it */

/* MMDVM */
#define MM_START 0xE0
#define MM_SET_MODE 0x03
#define MM_SET_FREQ 0x04
#define MM_CWID 0x0A
#define MM_DMR_DATA1 0x18
#define MM_DMR_DATA2 0x1A
#define MM_DMR_START 0x1D
#define MM_NAK 0x7F
#define MM_NAK_REFUSED 4

enum { K_RAW, K_ODR, K_MMDVM };
enum { S_IDLE, S_VIEW, S_POWER, S_PAUSE, S_APRS_VIEW, S_APRS_SET, S_RELEASE, S_TUNE, S_TUNE_PAUSE };

uint16_t rdof_guard_crc16(const uint8_t *b, size_t n)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)(b[i] << 8);
        for (int k = 0; k < 8; k++)
            crc = crc & 0x8000 ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

size_t rdof_guard_frame(uint8_t cmd, uint8_t seq, const uint8_t *payload, size_t n, uint8_t *out)
{
    out[0] = 0xA5;
    out[1] = 0x5A;
    out[2] = cmd;
    out[3] = seq;
    out[4] = (uint8_t)n;
    out[5] = (uint8_t)(n >> 8);
    if (n)
        memcpy(out + HDR, payload, n);
    uint16_t c = rdof_guard_crc16(out + 2, 4 + n);
    out[HDR + n] = (uint8_t)c;
    out[HDR + n + 1] = (uint8_t)(c >> 8);
    return HDR + n + 2;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* ── splitting a byte stream into frames and the rest ─────────────────────── */

typedef void (*emit_fn)(rdof_guard_t *g, int kind, const uint8_t *b, size_t n, double now);

static void drain(rdof_guard_t *g, rdof_guard_split_t *s, bool mmdvm, emit_fn emit, double now)
{
    size_t p = 0;
    while (p < s->n) {
        const uint8_t *b = s->buf + p;
        const size_t a = s->n - p;
        size_t i = 0;
        while (i < a && !(b[i] == 0xA5 && (i + 1 == a || b[i + 1] == 0x5A)) && !(mmdvm && b[i] == MM_START))
            i++;
        if (i) {
            emit(g, K_RAW, b, i, now);
            p += i;
            continue;
        }
        if (b[0] == 0xA5) {
            if (a < HDR)
                break;
            const size_t len = b[4] | (size_t)b[5] << 8;
            if (len > RDOF_GUARD_MAX_FRAME ||
                (a >= HDR + len + 2 && rdof_guard_crc16(b + 2, 4 + len) != (b[HDR + len] | b[HDR + len + 1] << 8))) {
                emit(g, K_RAW, b, 1, now); /* not a frame after all */
                p++;
                continue;
            }
            if (a < HDR + len + 2)
                break;
            emit(g, K_ODR, b, HDR + len + 2, now);
            p += HDR + len + 2;
            continue;
        }
        if (a < 2)
            break;
        if (b[1] < 3) {
            emit(g, K_RAW, b, 1, now);
            p++;
            continue;
        }
        if (a < b[1])
            break;
        emit(g, K_MMDVM, b, b[1], now);
        p += b[1];
    }
    memmove(s->buf, s->buf + p, s->n - p);
    s->n -= p;
}

static void split(rdof_guard_t *g, rdof_guard_split_t *s, bool mmdvm, const uint8_t *in, size_t n, emit_fn emit,
                  double now)
{
    while (n) { /* a partial frame always fits, so a full buffer drains */
        size_t room = sizeof s->buf - s->n, k = n < room ? n : room;
        memcpy(s->buf + s->n, in, k);
        s->n += k;
        in += k;
        n -= k;
        drain(g, s, mmdvm, emit, now);
    }
}

/* ── helpers ──────────────────────────────────────────────────────────────── */

static void event(rdof_guard_t *g, const char *kind, const char *fmt, ...)
{
    if (!g->io.event)
        return;
    char d[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(d, sizeof d, fmt, ap);
    va_end(ap);
    g->io.event(g->io.ctx, kind, d);
}

static bool declared(const rdof_guard_t *g, uint32_t hz)
{
    for (int i = 0; i < g->ntx; i++)
        if ((hz > g->tx_hz[i] ? hz - g->tx_hz[i] : g->tx_hz[i] - hz) < 100)
            return true;
    return false;
}

static void refuse(rdof_guard_t *g, uint8_t seq, const char *why)
{
    g->stats.refused++;
    event(g, "refused", "%s", why);
    uint8_t f[16], st = ST_REFUSED;
    g->io.to_tool(g->io.ctx, f, rdof_guard_frame(CMD_ERROR, seq, &st, 1, f));
}

static void nak(rdof_guard_t *g, uint8_t cmd, const char *why)
{
    g->stats.refused++;
    event(g, "refused", "%s", why);
    const uint8_t f[5] = {MM_START, 5, MM_NAK, cmd, MM_NAK_REFUSED};
    g->io.to_tool(g->io.ctx, f, sizeof f);
}

/* One of the guard's own requests, with a sequence number the tool isn't using. */
static void request(rdof_guard_t *g, int step, uint8_t cmd, const uint8_t *p, size_t n, double now)
{
    uint8_t f[HDR + 8 + 2];
    g->gseq = (uint8_t)(g->last_seq + 128);
    if (!g->gseq)
        g->gseq = 0x80;
    g->gcmd = cmd;
    g->step = step;
    g->deadline = now + RDOF_GUARD_ANSWER_S;
    g->io.to_radio(g->io.ctx, f, rdof_guard_frame(cmd, g->gseq, p, n, f));
}

static void view(rdof_guard_t *g, int step, uint8_t section, double now) { request(g, step, CMD_GET_VIEW, &section, 1, now); }

/* key=value lines in a view's text: the value of key as a number, dflt when it isn't there or is empty. */
static long kv(const uint8_t *t, size_t n, const char *key, long dflt)
{
    const size_t k = strlen(key);
    for (size_t i = 0; i < n;) {
        size_t e = i;
        while (e < n && t[e] != '\n')
            e++;
        if (e - i > k && !memcmp(t + i, key, k) && t[i + k] == '=') {
            char v[24];
            size_t m = e - i - k - 1 < sizeof v - 1 ? e - i - k - 1 : sizeof v - 1;
            memcpy(v, t + i + k + 1, m);
            v[m] = 0;
            if (!m)
                return dflt;
            if (!strcmp(v, "tx"))
                return 1;
            return strtol(v, NULL, 10);
        }
        i = e + 1;
    }
    return dflt;
}

static void tool_item(rdof_guard_t *g, int kind, const uint8_t *b, size_t n, double now);

/* Tool bytes held during a check, now that it's over: through a splitter of their own, as they came. */
static void replay(rdof_guard_t *g, double now)
{
    while (g->step == S_IDLE && g->nheld) {
        size_t n = g->nheld;
        memcpy(g->again, g->held, n);
        g->nheld = 0;
        g->replay.n = 0;
        split(g, &g->replay, g->proto == RDOF_GUARD_MMDVM, g->again, n, tool_item, now);
        if (g->replay.n) /* a frame start left at the end: it came as bytes */
            tool_item(g, K_RAW, g->replay.buf, g->replay.n, now);
    }
}

/* The check is over: the PTT it held goes, or is refused, then whatever the tool sent meanwhile. */
static void complete(rdof_guard_t *g, double now)
{
    g->step = S_IDLE;
    if (g->tune == RDOF_TUNE_RUNNING) { /* the check after a tune: is the radio where it was sent? */
        uint32_t d = g->view_rx_hz > g->tune_hz ? g->view_rx_hz - g->tune_hz : g->tune_hz - g->view_rx_hz;
        if (d < 100) {
            g->tune = RDOF_TUNE_OK;
        } else {
            g->tune = RDOF_TUNE_FAILED;
            snprintf(g->tune_error, sizeof g->tune_error, "the radio is on %.5f MHz after the tune, not %.5f",
                     g->view_rx_hz / 1e6, g->tune_hz / 1e6);
        }
    }
    if (g->held_ptt) {
        g->held_ptt = false;
        if (declared(g, g->view_tx_hz)) {
            g->io.to_radio(g->io.ctx, g->ptt_raw, g->ptt_n);
            g->outstanding = true;
            g->out_seq = g->ptt_seq;
            g->out_t = now;
        } else {
            char why[96];
            snprintf(why, sizeof why, "PTT on %.5f MHz, not a declared TX frequency", g->view_tx_hz / 1e6);
            refuse(g, g->ptt_seq, why);
        }
    }
    replay(g, now);
}

static void fail(rdof_guard_t *g, const char *why, double now)
{
    event(g, "check_failed", "%s", why);
    g->step = S_IDLE;
    if (g->tune == RDOF_TUNE_RUNNING) {
        g->tune = RDOF_TUNE_FAILED;
        snprintf(g->tune_error, sizeof g->tune_error, "%s", why);
    }
    if (g->held_ptt) {
        g->held_ptt = false;
        char w[160];
        snprintf(w, sizeof w, "PTT: %s", why);
        refuse(g, g->ptt_seq, w);
    }
    replay(g, now);
}

/* The radio's view is in: anything on the air that the lease didn't declare is a violation, its PTT released. */
static void finish(rdof_guard_t *g, double now)
{
    if (g->transmitting && !declared(g, g->view_tx_hz)) {
        g->stats.violations++;
        event(g, "tx_violation", "transmitting on %.5f MHz (%s), not a declared TX frequency", g->view_tx_hz / 1e6,
              g->why);
        const uint8_t release[3] = {0, 0, 0};
        request(g, S_RELEASE, CMD_PTT, release, sizeof release, now);
        return;
    }
    complete(g, now);
}

static void start_check(rdof_guard_t *g, const char *why, double now)
{
    g->stats.checks++;
    g->last_check = now;
    g->tries = 0;
    snprintf(g->why, sizeof g->why, "%s", why);
    view(g, S_VIEW, VIEW_CORE, now);
}

/* An answer to the guard's request. */
static void answer(rdof_guard_t *g, uint8_t cmd, const uint8_t *p, size_t n, double now)
{
    const bool ok = cmd != CMD_ERROR && n > 0 && p[0] == 0;
    switch (g->step) {
    case S_VIEW: {
        if (!ok) {
            fail(g, "the radio refused its view (not RadioFoundry firmware?)", now);
            return;
        }
        const uint8_t *t = n > 6 ? p + 6 : p;
        const size_t tn = n > 6 ? n - 6 : 0;
        g->power = (int)kv(t, tn, "power", -1);
        g->power_count = (int)kv(t, tn, "power_count", 9);
        g->transmitting = kv(t, tn, "state", 0) == 1;
        g->view_tx_hz = (uint32_t)kv(t, tn, "tx_hz", 0);
        g->view_rx_hz = (uint32_t)kv(t, tn, "rx_hz", 0);
        if (g->lowest && g->power != 0) {
            if (g->tries > g->power_count) {
                char why[80];
                snprintf(why, sizeof why, "can't bring the power down to the lowest (now %d)", g->power);
                fail(g, why, now);
                return;
            }
            g->tries++;
            g->stats.power_steps++;
            event(g, "power_step", "power %d: one step down", g->power);
            const uint8_t step[2] = {OP_POWER, 0xFF};
            request(g, S_POWER, CMD_RADIO, step, sizeof step, now);
            return;
        }
        if (g->lowest)
            view(g, S_APRS_VIEW, VIEW_APRS_SET, now);
        else
            finish(g, now);
        return;
    }
    case S_POWER:
        if (!ok) {
            fail(g, "the radio refused a power step down", now);
            return;
        }
        g->step = S_PAUSE;
        g->resume = now + POWER_PAUSE_S;
        return;
    case S_APRS_VIEW: {
        long power = ok ? kv(n > 6 ? p + 6 : p, n > 6 ? n - 6 : 0, "power", 0) : 0; /* no APRS: nothing to do */
        if (power > 1) { /* 0: the radio's level, 1: the lowest */
            g->stats.power_steps++;
            event(g, "aprs_power_lowered", "APRS power %ld: set to the lowest", power);
            const uint8_t set[6] = {APRS_OP_SET, APRS_F_POWER, 1, 0, 0, 0};
            request(g, S_APRS_SET, CMD_APRS, set, sizeof set, now);
            return;
        }
        finish(g, now);
        return;
    }
    case S_APRS_SET:
        finish(g, now);
        return;
    case S_RELEASE:
        complete(g, now);
        return;
    case S_TUNE: {
        const uint8_t op = g->tune_req[g->tune_at][0];
        const int st = cmd == CMD_ERROR ? -1 : n > 0 ? p[0] : -1;
        if (st == ST_BUSY && ++g->tune_busy < TUNE_BUSY_TRIES) { /* its rate limit: the same request again */
            g->step = S_TUNE_PAUSE;
            g->resume = now + TUNE_BUSY_S;
            return;
        }
        if (st != 0 && !(op == OP_HOTSPOT_EXIT && st == ST_UNAVAILABLE)) { /* not in hotspot mode: fine */
            static const char *const names[] = {[OP_VFO] = "VFO on", [OP_VFO_SET] = "the frequency",
                                                [OP_HOTSPOT_EXIT] = "leaving hotspot mode",
                                                [OP_DMR_TARGET] = "the talk target", [OP_DMR_FIELDS] = "mode / cc / slot"};
            char why[80];
            snprintf(why, sizeof why, "the radio refused %s (status %d)", op < 32 && names[op] ? names[op] : "a request",
                     st);
            fail(g, why, now);
            return;
        }
        g->tune_at++;
        g->tune_busy = 0;
        g->step = S_TUNE_PAUSE;
        g->resume = now + (g->tune_at < g->tune_n ? TUNE_GAP_S : TUNE_SETTLE_S);
        return;
    }
    }
}

/* ── the two directions ───────────────────────────────────────────────────── */

static void tool_odr(rdof_guard_t *g, const uint8_t *raw, size_t n, double now)
{
    const uint8_t cmd = raw[2], seq = raw[3], *p = raw + HDR;
    const size_t pn = n - HDR - 2;
    g->last_seq = seq;
    g->last_link = now;
    if (g->lowest && cmd == CMD_RADIO && pn >= 2 && p[0] == OP_POWER && (int8_t)p[1] > 0) {
        refuse(g, seq, "power step up (the bench keeps radios at the lowest power)");
        return;
    }
    if (g->lowest && cmd == CMD_APRS && pn >= 6 && p[0] == APRS_OP_SET && p[1] == APRS_F_POWER && le32(p + 2) > 1) {
        refuse(g, seq, "APRS power above the lowest");
        return;
    }
    if (cmd == CMD_PTT && pn >= 1 && p[0] == 1) {
        if (!g->ntx) {
            refuse(g, seq, "PTT on a receive-only lease (declare TX frequencies)");
            return;
        }
        if (n > sizeof g->ptt_raw) {
            refuse(g, seq, "PTT request too long");
            return;
        }
        memcpy(g->ptt_raw, raw, n);
        g->ptt_n = n;
        g->ptt_seq = seq;
        g->held_ptt = true;
        start_check(g, "ptt", now);
        return;
    }
    g->io.to_radio(g->io.ctx, raw, n);
    g->outstanding = true;
    g->out_seq = seq;
    g->out_t = now;
}

static bool mmdvm_tx(uint8_t cmd, const uint8_t *p, size_t n)
{
    return cmd == MM_CWID || cmd == MM_DMR_DATA1 || cmd == MM_DMR_DATA2 || (cmd == MM_DMR_START && n >= 1 && p[0]) ||
           (cmd >= 0x10 && cmd <= 0x17) || (cmd >= 0x20 && cmd <= 0x6F);
}

static void tool_mmdvm(rdof_guard_t *g, const uint8_t *raw, size_t n, double now)
{
    const uint8_t cmd = raw[2], *p = raw + 3;
    const size_t pn = n - 3;
    if (cmd == MM_SET_FREQ && pn >= 10) {
        const uint32_t tx = le32(p + 5);
        if (g->ntx && !declared(g, tx)) {
            g->mmdvm_tx_ok = false;
            char why[80];
            snprintf(why, sizeof why, "SET_FREQ TX %.5f MHz, not a declared TX frequency", tx / 1e6);
            nak(g, cmd, why);
            return;
        }
        g->mmdvm_tx_ok = g->ntx && declared(g, tx);
        g->mmdvm_tx_hz = tx;
        uint8_t f[256];
        memcpy(f, raw, n);
        if (f[3 + 9]) {
            event(g, "power_step", "SET_FREQ power %d: sent as 0", f[3 + 9]);
            g->stats.power_steps++;
            f[3 + 9] = 0;
        }
        g->io.to_radio(g->io.ctx, f, n);
        return;
    }
    if (mmdvm_tx(cmd, p, pn) && !g->mmdvm_tx_ok) {
        nak(g, cmd, g->ntx ? "TX before a SET_FREQ to a declared TX frequency" : "TX on a receive-only lease");
        return;
    }
    g->io.to_radio(g->io.ctx, raw, n);
}

static void tool_item(rdof_guard_t *g, int kind, const uint8_t *b, size_t n, double now)
{
    if (g->step != S_IDLE) { /* checking the radio: the tool waits */
        size_t k = n < RDOF_GUARD_HELD - g->nheld ? n : RDOF_GUARD_HELD - g->nheld;
        memcpy(g->held + g->nheld, b, k);
        g->nheld += k;
        g->stats.held_dropped += (long)(n - k);
        return;
    }
    if (kind == K_ODR)
        tool_odr(g, b, n, now);
    else if (kind == K_MMDVM)
        tool_mmdvm(g, b, n, now);
    else
        g->io.to_radio(g->io.ctx, b, n);
}

static void radio_item(rdof_guard_t *g, int kind, const uint8_t *b, size_t n, double now)
{
    if (kind == K_ODR) {
        const uint8_t cmd = b[2], seq = b[3];
        if (g->step != S_IDLE && g->step != S_PAUSE && seq == g->gseq && (cmd & 0x80)) {
            answer(g, cmd, b + HDR, n - HDR - 2, now); /* the guard's: the tool never sees it */
            return;
        }
        if (g->outstanding && seq == g->out_seq)
            g->outstanding = false;
    }
    g->io.to_tool(g->io.ctx, b, n);
}

/* ── the API ──────────────────────────────────────────────────────────────── */

void rdof_guard_init(rdof_guard_t *g, rdof_guard_proto_t proto, const rdof_guard_io_t *io, const uint32_t *tx_hz, int ntx)
{
    memset(g, 0, sizeof *g);
    g->proto = proto;
    g->io = *io;
    g->lowest = true;
    g->last_link = g->last_check = -1e9;
    for (int i = 0; i < ntx && i < RDOF_GUARD_MAX_TX; i++)
        g->tx_hz[g->ntx++] = tx_hz[i];
}

void rdof_guard_tool(rdof_guard_t *g, const uint8_t *b, size_t n, double now)
{
    split(g, &g->tool, g->proto == RDOF_GUARD_MMDVM, b, n, tool_item, now);
}

void rdof_guard_radio(rdof_guard_t *g, const uint8_t *b, size_t n, double now)
{
    split(g, &g->radio, false, b, n, radio_item, now);
}

void rdof_guard_tick(rdof_guard_t *g, double now)
{
    if (g->step == S_PAUSE) {
        if (now >= g->resume)
            view(g, S_VIEW, VIEW_CORE, now);
        return;
    }
    if (g->step == S_TUNE_PAUSE) {
        if (now < g->resume)
            return;
        if (g->tune_at < g->tune_n)
            request(g, S_TUNE, CMD_RADIO, g->tune_req[g->tune_at], g->tune_len[g->tune_at], now);
        else
            start_check(g, "tune", now); /* power to the lowest, and where the radio is now */
        return;
    }
    if (g->step != S_IDLE) {
        if (now > g->deadline) {
            char why[80];
            snprintf(why, sizeof why, "no debug link answer to 0x%02X", g->gcmd);
            if (g->step == S_APRS_VIEW || g->step == S_APRS_SET)
                finish(g, now); /* firmware without APRS */
            else if (g->step == S_RELEASE)
                complete(g, now);
            else
                fail(g, why, now);
        }
        return;
    }
    if (g->outstanding && now - g->out_t > OUTSTANDING_S)
        g->outstanding = false; /* its answer never came */
    if (!g->outstanding && now - g->last_link < RDOF_GUARD_LINK_IDLE_S && now - g->last_check > RDOF_GUARD_CHECK_S)
        start_check(g, "periodic", now);
}

void rdof_guard_check(rdof_guard_t *g, double now)
{
    if (g->step == S_IDLE)
        start_check(g, "lease", now);
}

void rdof_guard_set_tx(rdof_guard_t *g, const uint32_t *tx_hz, int ntx, double now)
{
    g->ntx = 0;
    for (int i = 0; i < ntx && i < RDOF_GUARD_MAX_TX; i++)
        g->tx_hz[g->ntx++] = tx_hz[i];
    if (g->proto == RDOF_GUARD_MMDVM)
        g->mmdvm_tx_ok = g->mmdvm_tx_ok && declared(g, g->mmdvm_tx_hz);
    else if (g->step == S_IDLE)
        start_check(g, "lease", now); /* (one under way judges by the list as it finishes) */
}

void rdof_guard_end(rdof_guard_t *g, double now)
{
    g->ntx = 0;
    g->nheld = 0;
    g->held_ptt = false;
    g->mmdvm_tx_ok = false;
    if (g->proto == RDOF_GUARD_MMDVM) {
        const uint8_t idle[4] = {MM_START, 4, MM_SET_MODE, 0};
        g->io.to_radio(g->io.ctx, idle, sizeof idle);
        return;
    }
    const uint8_t release[3] = {0, 0, 0};
    request(g, S_RELEASE, CMD_PTT, release, sizeof release, now);
}

bool rdof_guard_busy(const rdof_guard_t *g) { return g->step != S_IDLE; }

/* ── tune ─────────────────────────────────────────────────────────────────── */

static void tune_add(rdof_guard_t *g, uint8_t op, const uint8_t *args, size_t n)
{
    g->tune_req[g->tune_n][0] = op;
    if (n)
        memcpy(g->tune_req[g->tune_n] + 1, args, n);
    g->tune_len[g->tune_n++] = (uint8_t)(1 + n);
}

bool rdof_guard_tune(rdof_guard_t *g, const rdof_guard_tune_t *t, double now)
{
    if (g->proto != RDOF_GUARD_ODR || g->step != S_IDLE) {
        snprintf(g->tune_error, sizeof g->tune_error, "%s",
                 g->proto != RDOF_GUARD_ODR ? "only an ODR radio is tuned over its link" : "the guard is busy");
        return false;
    }
    g->tune_n = g->tune_at = g->tune_busy = 0;
    g->tune_hz = t->freq_hz;
    g->tune_error[0] = 0;
    const uint8_t on = 1;
    const uint8_t hz[4] = {(uint8_t)t->freq_hz, (uint8_t)(t->freq_hz >> 8), (uint8_t)(t->freq_hz >> 16),
                           (uint8_t)(t->freq_hz >> 24)};
    const uint8_t fields[3] = {t->fm ? 0 : 1, t->fm || t->cc < 0 ? 0xFF : (uint8_t)t->cc,
                               t->fm ? 0 : (uint8_t)t->slot};
    tune_add(g, OP_HOTSPOT_EXIT, NULL, 0); /* the debug link's radio requests need the radio itself */
    tune_add(g, OP_VFO, &on, 1);
    tune_add(g, OP_VFO_SET, hz, 4);
    tune_add(g, OP_DMR_FIELDS, fields, 3);
    if (!t->fm && t->tg > 0) {
        const uint32_t v = ((uint32_t)t->tg & 0xFFFFFF) | (t->private_call ? 1u << 24 : 0);
        const uint8_t target[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
        tune_add(g, OP_DMR_TARGET, target, 4);
    }
    g->tune = RDOF_TUNE_RUNNING;
    g->last_seq = 0;
    request(g, S_TUNE, CMD_RADIO, g->tune_req[0], g->tune_len[0], now);
    return true;
}

rdof_tune_state_t rdof_guard_tune_state(const rdof_guard_t *g) { return g->tune; }
