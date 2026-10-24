// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The bench API's transmit job (rdof/benchtx.h). */
#include "rdof/benchtx.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "rdof/layer2.h"
#include "rdof/pdu.h"

enum { STEP_VERSION = 0, STEP_CONFIG, STEP_FREQ, STEP_MODE, STEP_DONE };

const char *rdof_benchtx_gates(const rdof_benchtx_policy_t *p, bool built, bool can_tx, uint32_t freq,
                             const uint32_t *lease, int nlease, bool armed)
{
    if (!built)
        return "this firmware was built without bench TX (CONFIG_RDOF_BENCH_TX)";
    if (!can_tx)
        return "this path can't transmit";
    if (p->max_key_s <= 0 || !p->nfreqs)
        return "the settings allow no TX (bench.tx: freqs_hz and max_key_s)";
    bool listed = false;
    for (int i = 0; i < p->nfreqs; i++)
        listed |= p->freqs_hz[i] == freq;
    if (!listed)
        return "that frequency isn't in the bench's TX list (bench.tx.freqs_hz)";
    bool leased = false;
    for (int i = 0; i < nlease; i++)
        leased |= lease[i] == freq;
    if (!leased)
        return "the session holds no TX lease on that frequency";
    if (p->arm_gpio >= 0 && !armed)
        return "the TX arm jumper is out";
    return NULL;
}

int rdof_benchtx_control(const uint8_t burst[33], int n, int *voice_n)
{
    rdof_family_t fam;
    bool data;
    int err;
    if (rdof_detect_sync(burst, 4, &fam, &data, &err)) {
        if (!data) {
            *voice_n = 0;
            return RDOF_SYNC_AUDIO;
        }
        int cc, dt, e2;
        if (!rdof_decode_slot_type(burst, &cc, &dt, &e2))
            return -1;
        return RDOF_SYNC_DATA | (dt & 0x0F);
    }
    if (n < 1 || n > 5) {
        if (*voice_n < 0 || *voice_n >= 5)
            return -1; /* no sync, and no voice superframe to place it in */
        n = *voice_n + 1;
    }
    *voice_n = n;
    return n;
}

/* ── frames ───────────────────────────────────────────────────────────────── */

static void tool(rdof_benchtx_t *t, const char *fmt, ...)
{
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > 0 && t->io.to_tool)
        t->io.to_tool(t->io.ctx, b, (size_t)n < sizeof b ? (size_t)n : sizeof b - 1);
}

static double utc(rdof_benchtx_t *t, double now) { return t->io.utc ? t->io.utc(t->io.ctx) : now; }

static void modem(rdof_benchtx_t *t, uint8_t cmd, const uint8_t *p, size_t n)
{
    uint8_t f[64];
    size_t k = rdof_mmdvm_frame(cmd, p, n, f, sizeof f);
    if (k && t->io.to_modem)
        t->io.to_modem(t->io.ctx, f, k);
}

static void request(rdof_benchtx_t *t, double now)
{
    uint8_t p[40];
    size_t n = 0;
    rdof_modem_config_t c;
    switch (t->step) {
    case STEP_VERSION:
        t->want = RDOF_MMDVM_GET_VERSION;
        break;
    case STEP_CONFIG:
        rdof_modem_config_default(&c);
        n = t->protocol >= 2 ? rdof_mmdvm_config_v2(&c, t->cc, RDOF_MODE_DMR, false, p)
                             : rdof_mmdvm_config_v1(&c, t->cc, RDOF_MODE_DMR, false, p);
        t->want = RDOF_MMDVM_SET_CONFIG;
        break;
    case STEP_FREQ:
        n = rdof_mmdvm_set_freq_payload(t->freq, t->freq, t->rf_level, p);
        t->want = RDOF_MMDVM_SET_FREQ;
        break;
    case STEP_MODE:
        p[0] = RDOF_MODE_DMR;
        n = 1;
        t->want = RDOF_MMDVM_SET_MODE;
        break;
    default:
        return;
    }
    t->replied = t->nak = false;
    t->deadline = now + RDOF_BENCHTX_REPLY_S;
    modem(t, t->want, p, n);
}

static void finish(rdof_benchtx_t *t, const char *code, const char *why)
{
    if (t->state == RDOF_TXS_DONE)
        return;
    if (code) {
        snprintf(t->error, sizeof t->error, "%s", why);
        char esc[120];
        size_t k = 0;
        for (const char *s = why; *s && k + 2 < sizeof esc; s++)
            if (*s != '"' && *s != '\\')
                esc[k++] = *s;
        esc[k] = 0;
        tool(t, "{\"type\":\"error\",\"code\":\"%s\",\"error\":\"%s\"}", code, esc);
    }
    tool(t, "{\"type\":\"done\",\"why\":\"%s\",\"bursts\":%ld,\"keyings\":%ld,\"underruns\":%ld}",
         code ? code : "closed", t->sent, t->keyings, t->underruns);
    t->state = RDOF_TXS_DONE;
}

/* Off the air: the queue dropped, the modem idle (its buffer goes with the mode), the keying logged. */
static void unkey(rdof_benchtx_t *t, const char *why, double now)
{
    t->qn = 0;
    t->voice_n = -1;
    if (!t->keyed)
        return;
    const uint8_t idle = RDOF_MODE_IDLE;
    if (strcmp(why, "end") && strcmp(why, "underrun")) {
        modem(t, RDOF_MMDVM_SET_MODE, &idle, 1);
        t->need_mode = true;
    }
    double end = now < t->air_free ? now : t->air_free;
    double secs = end - t->key_start;
    if (t->io.keying)
        t->io.keying(t->io.ctx, utc(t, now) - (now - t->key_start), secs, t->key_bursts, why);
    tool(t, "{\"type\":\"unkeyed\",\"why\":\"%s\",\"t\":%.6f,\"seconds\":%.3f,\"bursts\":%d}", why, utc(t, now),
         secs, t->key_bursts);
    t->keyed = false;
    t->unkey_asked = false;
}

void rdof_benchtx_init(rdof_benchtx_t *t, const rdof_benchtx_io_t *io, uint32_t freq, int cc, int rf_level,
                     double max_key_s, double now)
{
    memset(t, 0, sizeof *t);
    t->io = *io;
    t->freq = freq;
    t->cc = cc;
    t->rf_level = rf_level < 0 ? 0 : rf_level > 255 ? 255 : rf_level;
    t->max_key_s = max_key_s;
    t->state = RDOF_TXS_SETUP;
    t->protocol = 1;
    t->voice_n = -1;
    t->want = 0xFF;
    t->last_tool = now;
    rdof_mmdvm_reader_init(&t->reader);
    t->step = STEP_VERSION;
    request(t, now);
}

void rdof_benchtx_stop(rdof_benchtx_t *t, const char *code, const char *why, double now);

/* ── the tool's frames ────────────────────────────────────────────────────── */

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static void bursts(rdof_benchtx_t *t, rdof_jdoc_t *d, int root, double now)
{
    int items = rdof_jget(d, root, "items");
    const rdof_jnode_t *in = items >= 0 ? rdof_jnode(d, items) : NULL;
    if (!in || in->type != RDOF_JARR) {
        rdof_benchtx_stop(t, "bad_request", "a bursts frame has an items list", now);
        return;
    }
    if (t->qn + in->count > RDOF_BENCHTX_QUEUE) {
        rdof_benchtx_stop(t, "bad_request", "more bursts than the queue holds: wait for queued_s to fall before sending more", now);
        return;
    }
    /* key time: what's on the air or handed over, what's queued, and these */
    double ahead = t->keyed ? (t->air_free > now ? t->air_free - now : 0) + (now - t->key_start) : 0;
    double total = ahead + (t->qn + in->count) * RDOF_BENCHTX_BURST_S;
    if (total > t->max_key_s + 1e-9) {
        char why[96];
        snprintf(why, sizeof why, "that's %.2f s of key time; the limit is %.1f s", total, t->max_key_s);
        rdof_benchtx_stop(t, "refused", why, now);
        return;
    }
    bool timed = false;
    int voice_n = t->voice_n;
    rdof_benchtx_item_t add[RDOF_BENCHTX_QUEUE];
    int k = 0;
    for (int i = 0; i < in->count; i++) {
        int it = rdof_jat(d, items, i);
        int hx = rdof_jget(d, it, "raw_hex"), nn = rdof_jget(d, it, "n");
        const rdof_jnode_t *h = hx >= 0 ? rdof_jnode(d, hx) : NULL;
        if (!h || h->type != RDOF_JSTR || strlen(h->str) != 66) {
            rdof_benchtx_stop(t, "bad_request", "each item has raw_hex: the 33-byte burst", now);
            return;
        }
        for (int b = 0; b < 33; b++) {
            int hi = hexval(h->str[2 * b]), lo = hexval(h->str[2 * b + 1]);
            if (hi < 0 || lo < 0) {
                rdof_benchtx_stop(t, "bad_request", "raw_hex isn't hex", now);
                return;
            }
            add[k].burst[b] = (uint8_t)(hi << 4 | lo);
        }
        timed |= rdof_jget(d, it, "at_sc") >= 0;
        /* n only as a number in range ((int) of a huge one is undefined); else counted from the voice sync */
        const rdof_jnode_t *nv = nn >= 0 ? rdof_jnode(d, nn) : NULL;
        const int pos = nv && nv->type == RDOF_JNUM && nv->num >= -1 && nv->num <= 5 ? (int)nv->num : -1;
        int c = rdof_benchtx_control(add[k].burst, pos, &voice_n);
        if (c < 0) {
            char why[96];
            snprintf(why, sizeof why, "item %d has no sync and isn't voice B-F after a voice sync", i);
            rdof_benchtx_stop(t, "bad_request", why, now);
            return;
        }
        add[k++].control = (uint8_t)c;
    }
    for (int i = 0; i < k; i++)
        t->q[(t->qh + t->qn++) % RDOF_BENCHTX_QUEUE] = add[i];
    t->voice_n = voice_n;
    (void)timed; /* a modem can't place a burst on the sample clock */
    double queued = t->qn * RDOF_BENCHTX_BURST_S + (t->keyed && t->air_free > now ? t->air_free - now : 0);
    tool(t, "{\"type\":\"ack\",\"bursts\":%d,\"queued\":%d,\"queued_s\":%.3f,\"timed\":false}", k, t->qn, queued);
}

void rdof_benchtx_alive(rdof_benchtx_t *t, double now) { t->last_tool = now; }

void rdof_benchtx_tool(rdof_benchtx_t *t, const char *json, size_t n, double now)
{
    if (t->state == RDOF_TXS_DONE)
        return;
    t->last_tool = now;
    t->frames++;
    rdof_jdoc_t d = {.nodes = t->nodes, .max = RDOF_BENCHTX_NODES, .buf = t->strbuf, .cap = sizeof t->strbuf};
    int root = rdof_jparse(&d, json, n);
    int ty = root >= 0 ? rdof_jget(&d, root, "type") : -1;
    const char *type = ty >= 0 && rdof_jnode(&d, ty)->type == RDOF_JSTR ? rdof_jnode(&d, ty)->str : "";
    if (!strcmp(type, "bursts"))
        bursts(t, &d, root, now);
    else if (!strcmp(type, "unkey"))
        unkey(t, "unkey", now); /* not keyed: the queue and the voice count dropped */
    else if (!strcmp(type, "ping"))
        ;
    else if (!strcmp(type, "fm") || !strcmp(type, "iq"))
        rdof_benchtx_stop(t, "unsupported", "a modem sends DMR bursts only: no FM tone or I/Q through it", now);
    else
        rdof_benchtx_stop(t, "bad_request", root < 0 ? "not JSON" : "unknown frame type", now);
}

/* ── the modem's frames ───────────────────────────────────────────────────── */

static void on_frame(void *ctx, uint8_t cmd, const uint8_t *p, size_t len)
{
    rdof_benchtx_t *t = ctx;
    if (cmd == RDOF_MMDVM_GET_VERSION && t->want == RDOF_MMDVM_GET_VERSION) {
        t->protocol = rdof_mmdvm_parse_version(p, len, NULL, 0);
        t->replied = true;
    } else if (cmd == RDOF_MMDVM_ACK && len >= 1 && p[0] == t->want) {
        t->replied = true;
    } else if (cmd == RDOF_MMDVM_NAK && len >= 1) {
        if (p[0] == t->want) {
            t->replied = t->nak = true;
        } else if (p[0] == RDOF_MMDVM_DMR_DATA2 || p[0] == RDOF_MMDVM_DMR_DATA1) {
            t->data_nak = len >= 2 ? 0x200 | p[1] : 0x100; /* a reason of 0 still counts */
        }
    }
}

void rdof_benchtx_modem(rdof_benchtx_t *t, const uint8_t *b, size_t n, double now)
{
    rdof_mmdvm_reader_feed(&t->reader, b, n, on_frame, t);
    if (t->data_nak && t->state != RDOF_TXS_DONE) {
        char why[96];
        if (t->data_nak & 0x200)
            snprintf(why, sizeof why, "the modem refused a burst (NAK reason %d)", t->data_nak & 0xFF);
        else
            snprintf(why, sizeof why, "the modem refused a burst (no NAK reason)");
        rdof_benchtx_stop(t, "refused", why, now);
    }
}

/* ── time ─────────────────────────────────────────────────────────────────── */

static void setup_tick(rdof_benchtx_t *t, double now)
{
    static const char *const names[] = {"GET_VERSION", "SET_CONFIG", "SET_FREQ", "SET_MODE"};
    if (t->replied && !t->nak) {
        t->want = 0xFF;
        if (++t->step == STEP_DONE) {
            t->state = RDOF_TXS_READY;
            t->need_mode = false;
            tool(t, "{\"type\":\"ready\",\"freq_hz\":%lu,\"protocol\":%d}", (unsigned long)t->freq, t->protocol);
            return;
        }
        t->tries = 0;
        request(t, now);
        return;
    }
    char why[96];
    if (t->nak) {
        snprintf(why, sizeof why, "the modem refused %s", names[t->step]);
        rdof_benchtx_stop(t, t->step == STEP_FREQ ? "refused" : "unavailable", why, now);
    } else if (now >= t->deadline) {
        if (++t->tries < RDOF_BENCHTX_TRIES) {
            request(t, now);
            return;
        }
        snprintf(why, sizeof why, "the modem didn't answer %s", names[t->step]);
        rdof_benchtx_stop(t, "unavailable", why, now);
    }
}

void rdof_benchtx_tick(rdof_benchtx_t *t, double now)
{
    if (t->state == RDOF_TXS_DONE)
        return;
    if (now - t->last_tool > RDOF_BENCHTX_DEADMAN_S) {
        unkey(t, "deadman", now);
        rdof_benchtx_stop(t, "timeout", "no frame from the tool for 2 s (dead-man)", now);
        return;
    }
    if (t->state == RDOF_TXS_SETUP) {
        setup_tick(t, now);
        return;
    }
    if (t->need_mode && t->qn) { /* back to DMR after a stop, before the next burst */
        t->state = RDOF_TXS_SETUP;
        t->step = STEP_MODE;
        t->tries = 0;
        request(t, now);
        return;
    }
    if (t->keyed && t->air_free - t->key_start > t->max_key_s + 1e-9) {
        unkey(t, "max_key", now);
        return;
    }
    while (t->qn && t->air_free - now < RDOF_BENCHTX_LEAD * RDOF_BENCHTX_BURST_S) {
        if (t->keyed && t->air_free + RDOF_BENCHTX_BURST_S - t->key_start > t->max_key_s + 1e-9) {
            unkey(t, "max_key", now);
            return;
        }
        const rdof_benchtx_item_t *it = &t->q[t->qh];
        if (!t->keyed) {
            t->keyed = true;
            t->key_start = now;
            t->air_free = now;
            t->key_bursts = 0;
            t->keyings++;
            tool(t, "{\"type\":\"keyed\",\"t\":%.6f,\"freq_hz\":%lu}", utc(t, now), (unsigned long)t->freq);
        }
        uint8_t p[34];
        p[0] = it->control;
        memcpy(p + 1, it->burst, 33);
        modem(t, RDOF_MMDVM_DMR_DATA2, p, sizeof p);
        t->last_voice = it->control == RDOF_SYNC_AUDIO || (it->control & 0xF0) == 0;
        if ((it->control & 0xF0) == RDOF_SYNC_DATA && (it->control & 0x0F) == RDOF_DT_TERMINATOR_WITH_LC)
            t->last_voice = false;
        t->qh = (t->qh + 1) % RDOF_BENCHTX_QUEUE;
        t->qn--;
        t->air_free = (t->air_free > now ? t->air_free : now) + RDOF_BENCHTX_BURST_S;
        t->key_bursts++;
        t->sent++;
    }
    if (t->keyed && !t->qn && now >= t->air_free) {
        if (t->last_voice) { /* a call that stopped without its terminator: the tool fell behind */
            t->underruns++;
            tool(t, "{\"type\":\"underrun\",\"t\":%.6f}", utc(t, now));
            unkey(t, "underrun", now);
        } else
            unkey(t, "end", now);
    }
}

void rdof_benchtx_stop(rdof_benchtx_t *t, const char *code, const char *why, double now)
{
    if (t->state == RDOF_TXS_DONE)
        return;
    const bool keyed = t->keyed;
    unkey(t, "stopped", now);
    if (!keyed) { /* nothing on the air, but leave the modem idle */
        const uint8_t idle = RDOF_MODE_IDLE;
        modem(t, RDOF_MMDVM_SET_MODE, &idle, 1);
    }
    finish(t, code, why);
}

bool rdof_benchtx_done(const rdof_benchtx_t *t) { return t->state == RDOF_TXS_DONE; }

const char *rdof_benchtx_state(const rdof_benchtx_t *t)
{
    return t->state == RDOF_TXS_DONE ? "idle" : t->state == RDOF_TXS_SETUP ? "setup" : t->keyed ? "keyed" : "armed";
}
