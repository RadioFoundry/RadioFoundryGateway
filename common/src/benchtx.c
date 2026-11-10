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

bool rdof_benchtx_allowed(const rdof_benchtx_policy_t *p, uint32_t freq)
{
    for (int i = 0; i < p->nfreqs; i++)
        if (p->freqs_hz[i] == freq)
            return true;
    for (int i = 0; i < p->nbands; i++)
        if (freq >= p->bands_hz[i][0] && freq <= p->bands_hz[i][1])
            return true;
    return false;
}

const char *rdof_benchtx_gates(const rdof_benchtx_policy_t *p, bool built, bool can_tx, uint32_t freq,
                             const uint32_t *lease, int nlease, bool armed)
{
    if (!built)
        return "this firmware was built without bench TX (CONFIG_RDOF_BENCH_TX)";
    if (!can_tx)
        return "this path can't transmit";
    if (p->max_key_s <= 0 || (!p->nfreqs && !p->nbands))
        return "the settings allow no TX (bench.tx: freqs_hz or bands_hz, and max_key_s)";
    if (!rdof_benchtx_allowed(p, freq))
        return "that frequency isn't in the bench's TX list or bands (bench.tx.freqs_hz, bands_hz)";
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
    int cc, pi, lcss;
    if ((n < 1 || n > 5) && rdof_decode_emb(burst, &cc, &pi, &lcss, NULL) && lcss >= 0 && lcss <= 3 &&
        *voice_n < 5) {
        /* By its EMB's LCSS (first B, last E, single F; continuation C, or D after C), so a late entry with no voice
         * sync yet goes too. A late entry starting at D is taken as C: give n. */
        static const int at[4] = {5, 1, 4, 2};
        n = lcss == 3 && *voice_n == 2 ? 3 : at[lcss];
    } else if (n < 1 || n > 5) {
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

/* ── what's measured ──────────────────────────────────────────────────────── */

/* A measurement frame: to the tool, and kept for a reader without one (the bench line). */
static void record(rdof_benchtx_t *t, const char *fmt, ...)
{
    char b[RDOF_BENCHTX_EVENT_LEN];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    if (t->io.to_tool)
        t->io.to_tool(t->io.ctx, b, (size_t)n < sizeof b ? (size_t)n : sizeof b - 1);
    if (t->evn == RDOF_BENCHTX_EVENTS) {
        t->ev_lost++;
        return;
    }
    memcpy(t->ev[(t->evh + t->evn++) % RDOF_BENCHTX_EVENTS], b, sizeof b);
}

/* The bursts the model says the modem still has to send. */
static int model_left(const rdof_benchtx_t *t, double now)
{
    return t->air_free > now ? (int)((t->air_free - now) / RDOF_BENCHTX_BURST_S + 0.5) : 0;
}

/* A keying's account: what the modem said and, with a receiver, what was on the air. */
static void tx_check(rdof_benchtx_t *t, double now)
{
    t->tailing = false;
    const bool held = t->tail_held || (t->st_seen && !t->st_tx && t->idle_space >= 0 && t->st_space < t->idle_space);
    if (held)
        record(t, "{\"type\":\"modem_flag\",\"t\":%.6f,\"flag\":\"held\",\"space\":%d,\"idle_space\":%d}", utc(t, now),
               t->tail_held ? t->held_space : t->st_space, t->idle_space);
    char tx[32] = "null";
    if (t->was_on && !t->st_tx && t->tx_off >= t->tx_on)
        snprintf(tx, sizeof tx, "%.3f", t->tx_off - t->tx_on);
    int n = 0, heard = 0;
    double first = 0, latest = 0;
    if (t->track) {
        const unsigned end = t->track->n, oldest = end > RDOF_BENCHTX_TRACK ? end - RDOF_BENCHTX_TRACK : 0;
        for (unsigned i = t->track_first > oldest ? t->track_first : oldest; i < end; i++, n++) {
            const double late = t->track->s[i % RDOF_BENCHTX_TRACK].heard_at - t->track->s[i % RDOF_BENCHTX_TRACK].at;
            if (!t->track->s[i % RDOF_BENCHTX_TRACK].heard)
                continue;
            if (!heard++)
                first = late;
            if (late > latest)
                latest = late;
        }
        record(t, "{\"type\":\"tx_check\",\"t\":%.6f,\"handed\":%d,\"timing\":\"heard\",\"compared\":%d,\"heard\":%d,"
                  "\"never\":%d,\"first_s\":%.3f,\"latest_s\":%.3f,\"modem_tx_s\":%s,\"quiet\":%d,\"late\":%d,\"held\":%s}",
               utc(t, now), t->key_bursts, n, heard, n - heard, first, latest, tx, t->n_quiet, t->n_late,
               held ? "true" : "false");
    } else {
        record(t, "{\"type\":\"tx_check\",\"t\":%.6f,\"handed\":%d,\"timing\":\"estimate\",\"why\":\"%s\",\"modem_tx_s\":%s,"
                  "\"quiet\":%d,\"late\":%d,\"held\":%s}",
               utc(t, now), t->key_bursts, t->no_heard ? t->no_heard : "no receiver compares it", tx, t->n_quiet, t->n_late,
               held ? "true" : "false");
    }
}

/* The modem's GET_STATUS. */
static void on_status(rdof_benchtx_t *t, const rdof_mmdvm_status_t *st, double now)
{
    const bool tx = st->transmitting != 0;
    t->answers++;
    t->st_space = st->space;
    if (!t->keyed && !t->tailing && !tx && st->space > t->idle_space)
        t->idle_space = st->space; /* an empty buffer: what "held" is measured against */
    if (t->keyed && st->space < t->space_min)
        t->space_min = st->space;
    if (!t->st_seen || tx != t->st_tx) {
        record(t, "{\"type\":\"modem\",\"t\":%.6f,\"tx\":%s,\"space\":%d,\"model_left\":%d}", utc(t, now),
               tx ? "true" : "false", st->space, model_left(t, now));
        if (tx && !t->was_on) {
            t->was_on = true;
            t->tx_on = now;
        }
        if (!tx && t->st_seen && t->st_tx) {
            t->tx_off = now;
            if (t->keyed && t->air_free - now > 1.5 * RDOF_BENCHTX_BURST_S) {
                t->n_quiet++;
                record(t, "{\"type\":\"modem_flag\",\"t\":%.6f,\"flag\":\"quiet\",\"outstanding\":%d}", utc(t, now),
                       model_left(t, now));
            }
        }
    }
    t->st_seen = true;
    t->st_tx = tx;
    if (tx && t->tailing && !t->n_late && now > t->tail_model + RDOF_BENCHTX_LATE_S) {
        t->n_late++;
        record(t, "{\"type\":\"modem_flag\",\"t\":%.6f,\"flag\":\"late\",\"late_s\":%.3f}", utc(t, now), now - t->tail_model);
    }
}

/* GET_STATUS while there's something to see, and the end of a keying's watch. */
static void watch(rdof_benchtx_t *t, double now)
{
    if ((t->keyed || t->tailing || !t->polls) && now >= t->next_poll) {
        t->next_poll = now + RDOF_BENCHTX_POLL_S;
        t->polls++;
        modem(t, RDOF_MMDVM_GET_STATUS, NULL, 0);
    }
    if (!t->tailing)
        return;
    const bool mute = t->polls >= 4 && !t->answers; /* a modem that doesn't answer it */
    /* TX off with bursts still in its buffer: an MMDVM HAT holds a call's last bursts until more comes on its port (the
     * polls are that), and SET_MODE idle would drop them. Done once its buffer is empty, or at tail_until. */
    const bool fresh = t->st_seen && t->answers > t->tail_answers;
    const bool holding = fresh && !t->st_tx && t->idle_space >= 0 && t->st_space < t->idle_space;
    if (holding && !t->tail_held)
        t->tail_held = true, t->held_space = t->st_space;
    const bool idle = fresh && !t->st_tx && !holding;
    const bool waited = !t->track || now >= t->tail_start + RDOF_BENCHTX_HEARD_S;
    if (mute || now >= t->tail_until || (idle && waited))
        tx_check(t, now);
}

void rdof_benchtx_track(rdof_benchtx_t *t, rdof_benchtx_track_t *track, const char *why)
{
    t->track = track;
    t->no_heard = why;
    if (track)
        track->n = 0;
}

void rdof_benchtx_heard(rdof_benchtx_t *t, const uint8_t burst[33], double now)
{
    if (!t->track)
        return;
    const unsigned end = t->track->n, oldest = end > RDOF_BENCHTX_TRACK ? end - RDOF_BENCHTX_TRACK : 0;
    for (unsigned i = t->track_first > oldest ? t->track_first : oldest; i < end; i++) {
        if (t->track->s[i % RDOF_BENCHTX_TRACK].heard)
            continue;
        int d = 0;
        for (int k = 0; k < 33 && d <= RDOF_BENCHTX_SAME_BITS; k++)
            for (uint8_t x = burst[k] ^ t->track->s[i % RDOF_BENCHTX_TRACK].burst[k]; x; x &= (uint8_t)(x - 1))
                d++;
        if (d <= RDOF_BENCHTX_SAME_BITS) {
            t->track->s[i % RDOF_BENCHTX_TRACK].heard = true;
            t->track->s[i % RDOF_BENCHTX_TRACK].heard_at = now;
            return;
        }
    }
}

size_t rdof_benchtx_next_event(rdof_benchtx_t *t, char *out, size_t cap)
{
    if (!t->evn || !cap)
        return 0;
    size_t n = strnlen(t->ev[t->evh], RDOF_BENCHTX_EVENT_LEN - 1);
    if (n >= cap)
        n = cap - 1;
    memcpy(out, t->ev[t->evh], n);
    out[n] = 0;
    t->evh = (t->evh + 1) % RDOF_BENCHTX_EVENTS;
    t->evn--;
    return n;
}

bool rdof_benchtx_settled(const rdof_benchtx_t *t) { return !t->tailing; }

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
    tool(t, "{\"type\":\"unkeyed\",\"why\":\"%s\",\"t\":%.6f,\"seconds\":%.3f,\"bursts\":%d,\"estimated\":true}", why,
         utc(t, now), secs, t->key_bursts);
    t->keyed = false;
    t->unkey_asked = false;
    t->tailing = true;
    t->tail_model = end;
    t->tail_start = now;
    t->tail_until = now + RDOF_BENCHTX_TAIL_S;
    t->tail_answers = t->answers; /* an answer from after this */
    t->tail_held = false;
    t->next_poll = now; /* straight away: is it done? */
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
    t->idle_space = -1;
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
    if (cmd == RDOF_MMDVM_GET_STATUS) {
        rdof_mmdvm_status_t st;
        if (t->state != RDOF_TXS_SETUP && rdof_mmdvm_parse_status(t->protocol, p, len, &st))
            on_status(t, &st, t->rx_now);
    } else if (cmd == RDOF_MMDVM_GET_VERSION && t->want == RDOF_MMDVM_GET_VERSION) {
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
    t->rx_now = now;
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
    watch(t, now);
    while (t->qn && t->air_free - now < RDOF_BENCHTX_LEAD * RDOF_BENCHTX_BURST_S) {
        if (t->keyed && t->air_free + RDOF_BENCHTX_BURST_S - t->key_start > t->max_key_s + 1e-9) {
            unkey(t, "max_key", now);
            return;
        }
        const rdof_benchtx_item_t *it = &t->q[t->qh];
        if (!t->keyed) {
            if (t->tailing)
                tx_check(t, now); /* the next keying is starting: what the last one came to */
            t->keyed = true;
            t->was_on = false;
            t->n_quiet = t->n_late = 0;
            t->space_min = 255;
            t->track_first = t->track ? t->track->n : 0;
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
        if (t->track) {
            const unsigned i = t->track->n++ % RDOF_BENCHTX_TRACK;
            memcpy(t->track->s[i].burst, it->burst, 33);
            t->track->s[i].heard = false;
            t->track->s[i].at = t->air_free > now ? t->air_free : now;
        }
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
    if (t->tailing)
        tx_check(t, now);
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
