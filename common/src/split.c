// SPDX-License-Identifier: MIT
/*
 * Split duplex: two MMDVM modems as one duplex modem (rdof/split.h). The clock
 * filter, its error bound, the epoch translation and the epoch trust rules
 * (rdof_clock_sync_*, rdof_split_epoch_for_rx, rdof_wrap_diff, rdof_epoch_relay_*)
 * are ported from RadioFoundry tools/split_duplex_host.py (MIT; the trust rules
 * as of RadioFoundry PR #109); PROVENANCE.md has the row.
 *
 * The arithmetic follows the reference operation for operation (built with
 * -ffp-contract=off, common/CMakeLists.txt; rint() for round()), so the frames match the generated scenarios to the
 * byte.
 */
#include "rdof/split.h"

#if RDOF_FEATURE_REPEATER

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define WRAP 4294967296.0
#define SLOT_NS 30000000.0
#define PERIOD_MIN 29990000
#define PERIOD_MAX 30010000
#define TERMINATOR (RDOF_SYNC_DATA | 0x02)
#define VOICE_HEADER (RDOF_SYNC_DATA | 0x01)
#define OFFERS_TX 0x01
#define OFFERS_RX 0x02

/* s into out[cap], cut to cap - 1 bytes (as snprintf "%s" cuts it, without its truncation warning). */
static void copy_str(char *out, size_t cap, const char *s)
{
    if (!cap)
        return;
    size_t n = strlen(s);
    if (n >= cap)
        n = cap - 1;
    memcpy(out, s, n);
    out[n] = '\0';
}

static const char *const HALF_NAMES[2] = {"TX half", "RX half"};

/* ── the clocks ──────────────────────────────────────────────────────────── */

int32_t rdof_wrap_diff(uint32_t a, uint32_t b)
{
    return (int32_t)(uint32_t)(a - b);
}

/* x rounded half to even, mod 2**32. */
static uint32_t round_u32(double x)
{
    int64_t v = (int64_t)rint(x);
    return (uint32_t)(uint64_t)v;
}

static int64_t host_us(double now)
{
    return (int64_t)rint(now * 1e6);
}

void rdof_clock_sync_init(rdof_clock_sync_t *c)
{
    memset(c, 0, sizeof *c);
}

int64_t rdof_clock_sync_add(rdof_clock_sync_t *c, int64_t t1, int64_t t4, uint32_t t_rx, uint32_t t_tx)
{
    int64_t rtt = (t4 - t1) - rdof_wrap_diff(t_tx, t_rx);
    double raw = (double)(((int64_t)t_rx - t1) + ((int64_t)t_tx - t4)) / 2.0;
    if (!c->has_base) {
        c->has_base = true;
        c->base = raw;
    }
    double off = c->base + (double)rdof_wrap_diff(round_u32(raw), round_u32(c->base));
    double mid = (double)(t1 + t4) / 2.0;
    rdof_clock_sample_t s = {rtt, mid, off};
    if (c->nsamples == RDOF_CLOCK_KEEP) {
        memmove(c->samples, c->samples + 1, (RDOF_CLOCK_KEEP - 1) * sizeof c->samples[0]);
        c->nsamples--;
    }
    c->samples[c->nsamples++] = s;
    c->group[c->ngroup++] = s;
    if (c->ngroup >= RDOF_CLOCK_GROUP) {
        int best = 0;
        for (int i = 1; i < c->ngroup; i++)
            if (c->group[i].rtt < c->group[best].rtt)
                best = i;
        if (c->npts == RDOF_CLOCK_POINTS) { /* can't happen at a sample a second; drop the oldest */
            memmove(c->pts_h, c->pts_h + 1, (RDOF_CLOCK_POINTS - 1) * sizeof(double));
            memmove(c->pts_o, c->pts_o + 1, (RDOF_CLOCK_POINTS - 1) * sizeof(double));
            memmove(c->pts_r, c->pts_r + 1, (RDOF_CLOCK_POINTS - 1) * sizeof(int64_t));
            c->npts--;
        }
        c->pts_h[c->npts] = c->group[best].mid;
        c->pts_o[c->npts] = c->group[best].off;
        c->pts_r[c->npts] = c->group[best].rtt;
        c->npts++;
        c->ngroup = 0;
        double horizon = mid - RDOF_CLOCK_FIT_S * 1e6;
        int k = 0;
        for (int i = 0; i < c->npts; i++)
            if (c->pts_h[i] >= horizon) {
                c->pts_h[k] = c->pts_h[i];
                c->pts_o[k] = c->pts_o[i];
                c->pts_r[k] = c->pts_r[i];
                k++;
            }
        c->npts = k;
    }
    c->count++;
    return rtt;
}

void rdof_clock_sync_fit(const rdof_clock_sync_t *c, double *rate, double *icpt, double *h0)
{
    int n = c->npts;
    if (n < 3 || c->pts_h[n - 1] - c->pts_h[0] < 5e6) {
        int best = 0;
        for (int i = 1; i < c->nsamples; i++)
            if (c->samples[i].rtt < c->samples[best].rtt)
                best = i;
        *rate = 0.0;
        *icpt = c->nsamples ? c->samples[best].off : 0.0;
        *h0 = c->nsamples ? c->samples[best].mid : 0.0;
        return;
    }
    double sh = 0.0, so = 0.0;
    for (int i = 0; i < n; i++)
        sh += c->pts_h[i];
    for (int i = 0; i < n; i++)
        so += c->pts_o[i];
    double mh = sh / n, mo = so / n;
    double sxx = 0.0, sxy = 0.0;
    for (int i = 0; i < n; i++) {
        double d = c->pts_h[i] - mh;
        sxx += d * d;
    }
    for (int i = 0; i < n; i++)
        sxy += (c->pts_h[i] - mh) * (c->pts_o[i] - mo);
    *rate = sxx > 0 ? sxy / sxx : 0.0;
    *icpt = mo;
    *h0 = mh;
}

double rdof_clock_sync_bound_us(const rdof_clock_sync_t *c)
{
    int n = c->npts;
    if (n < 3 || c->pts_h[n - 1] - c->pts_h[0] < 5e6) {
        if (!c->nsamples)
            return INFINITY;
        int64_t m = c->samples[0].rtt;
        for (int i = 1; i < c->nsamples; i++)
            if (c->samples[i].rtt < m)
                m = c->samples[i].rtt;
        return (double)m / 2.0;
    }
    int64_t m = c->pts_r[0];
    for (int i = 1; i < n; i++)
        if (c->pts_r[i] > m)
            m = c->pts_r[i];
    return (double)m / 2.0;
}

static double clock_rate(const rdof_clock_sync_t *c)
{
    double rate, icpt, h0;
    rdof_clock_sync_fit(c, &rate, &icpt, &h0);
    return rate;
}

uint32_t rdof_clock_sync_to_modem(const rdof_clock_sync_t *c, double h)
{
    double rate, icpt, h0;
    rdof_clock_sync_fit(c, &rate, &icpt, &h0);
    double offset = icpt + rate * (h - h0);
    return round_u32(h + offset);
}

double rdof_clock_sync_to_host(const rdof_clock_sync_t *c, uint32_t m, double near_h)
{
    uint32_t guess = rdof_clock_sync_to_modem(c, near_h);
    return near_h + (double)rdof_wrap_diff(m, guess) / (1.0 + clock_rate(c));
}

void rdof_split_epoch_for_rx(const rdof_clock_sync_t *tx, const rdof_clock_sync_t *rx, uint32_t t_us, int ts, double now_h,
                           uint32_t *t_rx, uint32_t *period_ns)
{
    uint32_t t_tx = t_us + (ts == 2 ? 30000u : 0u);
    double h = rdof_clock_sync_to_host(tx, t_tx, now_h);
    *t_rx = rdof_clock_sync_to_modem(rx, h);
    double p = rint(SLOT_NS * (1.0 + clock_rate(rx) - clock_rate(tx)));
    long period = (long)p;
    if (period < PERIOD_MIN)
        period = PERIOD_MIN;
    if (period > PERIOD_MAX)
        period = PERIOD_MAX;
    *period_ns = (uint32_t)period;
}

/* ── the epoch's trust rules (split-duplex.md 6.3, PR #109) ──────────────── */

void rdof_epoch_relay_init(rdof_epoch_relay_t *r, double limit_us, double agree_us)
{
    memset(r, 0, sizeof *r);
    r->limit_us = limit_us;
    r->agree_us = agree_us;
}

void rdof_epoch_relay_reset(rdof_epoch_relay_t *r)
{
    r->has_sent = false;
    r->nheld = 0;
}

double rdof_frame_rem(uint32_t t_a, uint32_t t_b, uint32_t period_ns)
{
    double frame_us = 2.0 * period_ns / 1000.0;
    double d = rdof_wrap_diff(t_a, t_b);
    return d - rint(d / frame_us) * frame_us; /* half to even, as rint */
}

bool rdof_epoch_relay_decide(rdof_epoch_relay_t *r, uint32_t t, uint32_t period_ns, double bound_us)
{
    double rem = r->has_sent ? rdof_frame_rem(t, r->sent, period_ns) : NAN;
    rdof_relay_action_t a;
    if (!(bound_us <= r->limit_us)) {
        a = RDOF_RELAY_SKIP;
        r->skipped++;
    } else if (!r->has_sent || fabs(rem) <= r->agree_us) {
        a = RDOF_RELAY_SEND;
        r->nheld = 0;
    } else {
        uint32_t keep[RDOF_RELAY_REPLACE];
        int k = 0;
        for (int i = 0; i < r->nheld; i++)
            if (fabs(rdof_frame_rem(t, r->held[i], period_ns)) <= r->agree_us && k < RDOF_RELAY_REPLACE - 1)
                keep[k++] = r->held[i];
        keep[k++] = t;
        memcpy(r->held, keep, sizeof keep[0] * (size_t)k);
        r->nheld = k;
        if (k >= RDOF_RELAY_REPLACE) {
            a = RDOF_RELAY_REPLACED;
            r->nheld = 0;
        } else {
            a = RDOF_RELAY_HOLD;
            r->rejected++;
        }
    }
    if (a == RDOF_RELAY_SEND || a == RDOF_RELAY_REPLACED) {
        r->has_sent = true;
        r->sent = t;
    }
    rdof_relay_entry_t *e = &r->log[r->decisions++ % RDOF_RELAY_LOG];
    *e = (rdof_relay_entry_t){t, period_ns, bound_us, rem, a};
    return a == RDOF_RELAY_SEND || a == RDOF_RELAY_REPLACED;
}

/* ── names ───────────────────────────────────────────────────────────────── */

const char *rdof_align_state_name(rdof_align_state_t a)
{
    static const char *const names[] = {"off", "unsupported", "waiting", "snapping", "held", "slewing", "locked"};
    return (unsigned)a < 6 ? names[a] : "?";
}

static bool word_char(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

bool rdof_has_split(const char *d)
{
    for (const char *p = d; (p = strstr(p, "SPLIT")) != NULL; p++) {
        if (p > d && word_char(p[-1]))
            continue;
        const char *q = p + 5;
        if (!isdigit((unsigned char)*q))
            continue;
        long v = 0;
        while (isdigit((unsigned char)*q))
            v = v * 10 + (*q++ - '0');
        if (word_char(*q))
            continue;
        if (v >= 1)
            return true;
    }
    return false;
}

/* ── output ──────────────────────────────────────────────────────────────── */

static void put(uint8_t *buf, size_t *len, size_t cap, uint32_t *overflow, uint8_t cmd, const uint8_t *p, size_t n)
{
    uint8_t fr[RDOF_MMDVM_MAX_FRAME];
    size_t flen = rdof_mmdvm_frame(cmd, p, n, fr, sizeof fr);
    if (*len + flen > cap) {
        if (overflow)
            *overflow += (uint32_t)flen;
        return;
    }
    memcpy(buf + *len, fr, flen);
    *len += flen;
}

static void up(rdof_split_t *s, uint8_t cmd, const uint8_t *p, size_t n)
{
    put(s->up, &s->up_len, sizeof s->up, NULL, cmd, p, n);
}

static void to_half(rdof_split_t *s, int which, uint8_t cmd, const uint8_t *p, size_t n)
{
    rdof_split_half_t *h = &s->halves[which];
    put(h->out, &h->out_len, sizeof h->out, &h->overflow, cmd, p, n);
}

size_t rdof_split_take(rdof_split_t *s, int which, uint8_t *buf, size_t cap)
{
    uint8_t *src = which < 0 ? s->up : s->halves[which].out;
    size_t *len = which < 0 ? &s->up_len : &s->halves[which].out_len;
    size_t n = *len < cap ? *len : cap;
    memcpy(buf, src, n);
    memmove(src, src + n, *len - n);
    *len -= n;
    return n;
}

static void logf_(rdof_split_t *s, const char *msg)
{
    if (s->log)
        s->log(s->log_ctx, msg);
}

static void fail(rdof_split_t *s, const char *why)
{
    if (s->failed[0])
        return;
    copy_str(s->failed, sizeof s->failed, why);
    s->ready = false;
    logf_(s, why);
}

/* ── requests to a half ──────────────────────────────────────────────────── */

static void done(rdof_split_t *s, int which, const rdof_split_req_t *r, bool ok, const uint8_t *data, size_t len,
                 bool has_data, double now);

static void pump(rdof_split_t *s, int which, double now)
{
    rdof_split_half_t *h = &s->halves[which];
    if (h->has_inflight || !h->qcount || s->failed[0])
        return;
    h->inflight = h->queue[h->qhead];
    h->qhead = (h->qhead + 1) % RDOF_SPLIT_QUEUE;
    h->qcount--;
    h->has_inflight = true;
    rdof_split_req_t *r = &h->inflight;
    r->sent_at = now;
    if (r->key == (0xD600 | RDOF_SPLIT_TIME)) { /* the token is the send time: t1 */
        uint64_t t1 = (uint64_t)host_us(now);
        for (int i = 0; i < 8; i++)
            r->frame[4 + i] = (uint8_t)(t1 >> (8 * i));
    }
    if (h->out_len + r->len <= sizeof h->out) {
        memcpy(h->out + h->out_len, r->frame, r->len);
        h->out_len += r->len;
    } else {
        h->overflow += (uint32_t)r->len;
    }
}

static void request(rdof_split_t *s, int which, uint8_t command, const uint8_t *payload, size_t n, int key,
                    rdof_split_done_t d, int arg, double now)
{
    rdof_split_half_t *h = &s->halves[which];
    if (h->qcount == RDOF_SPLIT_QUEUE) {
        fail(s, which == RDOF_SPLIT_TX ? "the TX half's request queue is full" : "the RX half's request queue is full");
        return;
    }
    rdof_split_req_t *r = &h->queue[(h->qhead + h->qcount) % RDOF_SPLIT_QUEUE];
    memset(r, 0, sizeof *r);
    r->key = key;
    r->len = rdof_mmdvm_frame(command, payload, n, r->frame, sizeof r->frame);
    r->done = d;
    r->arg = arg;
    h->qcount++;
    pump(s, which, now);
}

static void split_req(rdof_split_t *s, int which, uint8_t sub, const uint8_t *data, size_t n, rdof_split_done_t d, int arg,
                      double now)
{
    uint8_t p[RDOF_SPLIT_REQ_FRAME];
    p[0] = sub;
    if (n)
        memcpy(p + 1, data, n);
    request(s, which, RDOF_ODR_SPLIT, p, n + 1, 0xD600 | sub, d, arg, now);
}

static void reply(rdof_split_t *s, int which, int key, bool ok, const uint8_t *data, size_t len, double now)
{
    rdof_split_half_t *h = &s->halves[which];
    if (!h->has_inflight || h->inflight.key != key)
        return;
    rdof_split_req_t r = h->inflight;
    h->has_inflight = false;
    h->last_reply = now;
    h->misses = 0;
    done(s, which, &r, ok, data, len, true, now);
    pump(s, which, now);
}

/* ── bring-up, time, epoch ───────────────────────────────────────────────── */

static void time_req(rdof_split_t *s, int which, double now)
{
    rdof_split_half_t *h = &s->halves[which];
    if (h->timing)
        return;
    h->timing = true;
    uint8_t token[8] = {0};
    split_req(s, which, RDOF_SPLIT_TIME, token, 8, RDOF_SPLIT_DONE_TIME, 0, now);
}

static void version_reply(rdof_split_t *s)
{
    uint8_t p[1 + sizeof RDOF_SPLIT_DESCRIPTION];
    p[0] = 1;
    memcpy(p + 1, RDOF_SPLIT_DESCRIPTION, sizeof RDOF_SPLIT_DESCRIPTION - 1);
    up(s, RDOF_MMDVM_GET_VERSION, p, sizeof RDOF_SPLIT_DESCRIPTION);
}

static void ack_up(rdof_split_t *s, uint8_t command)
{
    up(s, RDOF_MMDVM_ACK, &command, 1);
}

static void epoch_waited(rdof_split_t *s)
{
    uint8_t w[RDOF_SPLIT_WAITERS];
    int n = s->nwaiters;
    memcpy(w, s->waiters, (size_t)n);
    s->nwaiters = 0;
    for (int i = 0; i < n; i++)
        ack_up(s, w[i]);
}

/* then_ack >= 0: ACK that command upstream once the epoch is done (or failed). */
static void epoch(rdof_split_t *s, double now, int then_ack)
{
    if (then_ack >= 0) {
        if (!s->tx_role) {
            ack_up(s, (uint8_t)then_ack);
            return;
        }
        if (s->nwaiters < RDOF_SPLIT_WAITERS)
            s->waiters[s->nwaiters++] = (uint8_t)then_ack;
    }
    if (s->epoch_busy || !s->tx_role)
        return;
    s->epoch_busy = true;
    split_req(s, RDOF_SPLIT_TX, RDOF_SPLIT_GET_EPOCH, NULL, 0, RDOF_SPLIT_DONE_GET_EPOCH, 0, now);
}

static void become_ready(rdof_split_t *s, double now)
{
    s->ready = true;
    s->next_time = now + RDOF_SPLIT_TIME_EVERY_S;
    s->has_epoch_due = true;
    s->epoch_due = now;
    s->has_align_due = true;
    s->align_due = now;
    char msg[300];
    snprintf(msg, sizeof msg, "Split duplex modem ready (TX: %s; RX: %s)", s->halves[0].description,
             s->halves[1].description);
    logf_(s, msg);
    if (s->version_asked) {
        s->version_asked = false;
        version_reply(s);
    }
    epoch(s, now, -1);
}

static uint32_t u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void le32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static int join_new(rdof_split_t *s, uint8_t command, uint8_t mode)
{
    for (int i = 0; i < RDOF_SPLIT_JOINS; i++)
        if (!s->joins[i].used) {
            rdof_split_join_t *j = &s->joins[i];
            memset(j, 0, sizeof *j);
            j->used = true;
            j->command = command;
            j->left = 2;
            j->mode = mode;
            return i;
        }
    return -1;
}

static void done(rdof_split_t *s, int which, const rdof_split_req_t *r, bool ok, const uint8_t *data, size_t len,
                 bool has_data, double now)
{
    rdof_split_half_t *h = &s->halves[which];
    char msg[300];
    switch (r->done) {
    case RDOF_SPLIT_DONE_NONE: break;
    case RDOF_SPLIT_DONE_VERSION: {
        if (!ok || !has_data) {
            snprintf(msg, sizeof msg, "the %s didn't answer GET_VERSION", HALF_NAMES[which]);
            fail(s, msg);
            return;
        }
        h->protocol = rdof_mmdvm_parse_version(data, len, h->description, sizeof h->description);
        if (!rdof_has_split(h->description)) {
            snprintf(msg, sizeof msg,
                     "the %s (%s) can't be one: it needs RadioFoundry's split duplex extension (SPLIT1 in its "
                     "description)",
                     HALF_NAMES[which], h->description[0] ? h->description : "no description");
            fail(s, msg);
            return;
        }
        split_req(s, which, RDOF_SPLIT_INFO, NULL, 0, RDOF_SPLIT_DONE_INFO, 0, now);
        break;
    }
    case RDOF_SPLIT_DONE_INFO: {
        if (!ok || !has_data || len < 10) {
            snprintf(msg, sizeof msg, "the %s didn't answer the split duplex INFO", HALF_NAMES[which]);
            fail(s, msg);
            return;
        }
        int want = which == RDOF_SPLIT_TX ? OFFERS_TX : OFFERS_RX;
        if (!(data[1] & want)) {
            snprintf(msg, sizeof msg, "the %s (%s) can't take that role (it offers 0x%02x)", HALF_NAMES[which],
                     h->description, data[1]);
            fail(s, msg);
            return;
        }
        h->version = data[0];
        h->caps = rdof_ops_caps(data, len);
        h->info_len = len < sizeof h->info ? len : sizeof h->info;
        memcpy(h->info, data, h->info_len);
        if (which == RDOF_SPLIT_TX && data[4] == 0)
            logf_(s, "The TX half sends no CACH (INFO says 0): radios may not tell the slots apart");
        uint8_t p[7];
        p[0] = which == RDOF_SPLIT_TX ? RDOF_SPLIT_ROLE_TX : RDOF_SPLIT_ROLE_RX;
        memcpy(p + 1, s->role_params, 5);
        p[RDOF_OPS_STREAM_MODE_AT] = (uint8_t)s->stream_mode;
        bool mode = which == RDOF_SPLIT_TX && s->stream_mode != RDOF_OPS_STREAM_FIFO; /* the byte only when it isn't 0 */
        split_req(s, which, RDOF_SPLIT_SET_ROLE, p, mode ? 7 : 6, RDOF_SPLIT_DONE_ROLE, 0, now);
        break;
    }
    case RDOF_SPLIT_DONE_ROLE:
        if (!ok) {
            snprintf(msg, sizeof msg, "the %s refused its role (SET_ROLE)", HALF_NAMES[which]);
            fail(s, msg);
            return;
        }
        if (which == RDOF_SPLIT_TX)
            s->tx_role = true;
        time_req(s, which, now);
        break;
    case RDOF_SPLIT_DONE_TIME: {
        h->timing = false;
        if (ok && has_data && len >= 16) {
            uint64_t t1 = 0;
            for (int i = 0; i < 8; i++)
                t1 |= (uint64_t)data[i] << (8 * i);
            rdof_clock_sync_add(&h->sync, (int64_t)t1, host_us(now), u32le(data + 8), u32le(data + 12));
        }
        if (s->ready || s->failed[0])
            return;
        if (h->sync.count < RDOF_SPLIT_SYNC_SAMPLES) {
            time_req(s, which, now);
            return;
        }
        if (s->halves[0].sync.count >= RDOF_SPLIT_SYNC_SAMPLES && s->halves[1].sync.count >= RDOF_SPLIT_SYNC_SAMPLES &&
            s->tx_role)
            become_ready(s, now);
        break;
    }
    case RDOF_SPLIT_DONE_GET_EPOCH: {
        if (!ok || !has_data || len < 10) {
            s->epoch_busy = false;
            s->has_epoch_due = true;
            s->epoch_due = now + RDOF_SPLIT_EPOCH_RETRY_S;
            epoch_waited(s);
            return;
        }
        uint32_t t_us = u32le(data + 4);
        int ts = data[8];
        uint32_t t_rx, period;
        rdof_split_epoch_for_rx(&s->halves[0].sync, &s->halves[1].sync, t_us, ts, (double)host_us(now), &t_rx, &period);
        double bound = rdof_clock_sync_bound_us(&s->halves[0].sync) + rdof_clock_sync_bound_us(&s->halves[1].sync);
        if (!rdof_epoch_relay_decide(&s->relay, t_rx, period, bound)) {
            const rdof_relay_entry_t *e = &s->relay.log[(s->relay.decisions - 1) % RDOF_RELAY_LOG];
            snprintf(msg, sizeof msg, "Split duplex: %s epoch %" PRIu32 " (bound %.0f us, %.0f us from the last)",
                     e->action == RDOF_RELAY_SKIP ? "skip" : "hold", t_rx, bound, e->rem_us);
            logf_(s, msg);
            s->epoch_busy = false; /* held or skipped: again after the next TIME round */
            s->has_epoch_due = true;
            s->epoch_due = now + RDOF_SPLIT_TIME_EVERY_S;
            epoch_waited(s);
            return;
        }
        uint8_t p[8];
        le32(p, t_rx);
        le32(p + 4, period);
        split_req(s, RDOF_SPLIT_RX, RDOF_SPLIT_SET_EPOCH, p, 8, RDOF_SPLIT_DONE_SET_EPOCH, 0, now);
        break;
    }
    case RDOF_SPLIT_DONE_SET_EPOCH: {
        s->epoch_busy = false;
        if (!ok) {
            s->has_epoch_due = true;
            s->epoch_due = now + RDOF_SPLIT_EPOCH_RETRY_S;
            epoch_waited(s);
            return;
        }
        if (!s->epoch_ok)
            logf_(s, "Split duplex: the RX half has the TX half's slot clock");
        s->epoch_ok = true;
        if (!s->has_first_epoch) {
            s->has_first_epoch = true;
            s->first_epoch = now;
        }
        bool early = now - s->first_epoch < RDOF_SPLIT_EPOCH_EARLY_S;
        s->has_epoch_due = true;
        s->epoch_due = now + (early ? RDOF_SPLIT_EPOCH_EARLY_EVERY_S : RDOF_SPLIT_EPOCH_EVERY_S);
        epoch_waited(s);
        break;
    }
    case RDOF_SPLIT_DONE_JOIN: {
        if (r->arg < 0)
            return;
        rdof_split_join_t *j = &s->joins[r->arg];
        if (!has_data)
            j->timeout = true;
        else if (!ok && !j->has_err) {
            j->has_err = true;
            j->err = len ? data[0] : 0;
        }
        if (--j->left)
            return;
        j->used = false;
        if (j->timeout)
            return; /* no reply: upstream times out, as with a dead modem */
        if (j->has_err) {
            uint8_t p[2] = {j->command, j->err};
            up(s, RDOF_MMDVM_NAK, p, 2);
            return;
        }
        if (j->command == RDOF_MMDVM_SET_MODE)
            s->mode = j->mode;
        if (j->command == RDOF_MMDVM_SET_FREQ || j->command == RDOF_MMDVM_SET_MODE) {
            /* TX's slot clock may restart: the epoch again, and the ACK once the RX half has it */
            s->epoch_ok = false;
            rdof_epoch_relay_reset(&s->relay); /* the next epoch is a first */
            if (j->command == RDOF_MMDVM_SET_FREQ) {
                s->has_align_due = true;
                s->align_due = now;
            }
            epoch(s, now, j->command);
            return;
        }
        ack_up(s, j->command);
        break;
    }
    case RDOF_SPLIT_DONE_RESTORE: s->tx_role = ok; break;
    case RDOF_SPLIT_DONE_CWID: {
        if (!has_data)
            return;
        uint8_t p[2] = {RDOF_MMDVM_SEND_CWID, len ? data[0] : 0};
        up(s, ok ? RDOF_MMDVM_ACK : RDOF_MMDVM_NAK, p, ok ? 1 : 2);
        break;
    }
    case RDOF_SPLIT_DONE_ALIGN: {
        s->align_busy = false;
        int32_t err;
        int flags;
        if (!has_data || !rdof_ops_aligned(ok, data, len, &err, &flags)) {
            s->align_state = RDOF_ALIGN_WAITING;
            s->align_due = now + RDOF_SPLIT_TIME_EVERY_S;
            return;
        }
        s->align_err_us = err;
        s->align_flags = flags;
        s->has_align_err = true;
        if (flags & RDOF_OPS_NOT_APPLIED) { /* keyed and a millisecond (or a parity) off: held until unkeyed */
            s->align_state = RDOF_ALIGN_HELD;
            return;
        }
        if (flags & RDOF_OPS_SNAPPING) {
            /* unkeyed: its slot clock restarts on the grid over the next slots; STATUS 46 says when it's done */
            s->align_state = RDOF_ALIGN_SNAPPING;
            s->align_jump = err > RDOF_SPLIT_ALIGN_JUMP_QUIET_US || err < -RDOF_SPLIT_ALIGN_JUMP_QUIET_US;
            s->snap_polls = 0;
            s->has_snap_due = true;
            s->snap_due = now + RDOF_SPLIT_SNAP_POLL_S;
            return;
        }
        s->align_state = flags & RDOF_OPS_SLEWING  ? RDOF_ALIGN_SLEWING
                         : flags & RDOF_OPS_ALIGNED ? RDOF_ALIGN_LOCKED
                                                  : RDOF_ALIGN_WAITING;
        break;
    }
    case RDOF_SPLIT_DONE_SNAP: {
        s->align_busy = false;
        if (s->align_state != RDOF_ALIGN_SNAPPING)
            return;
        rdof_ops_status_t st;
        const bool got = has_data && ok && rdof_ops_status_ext(data, len, &st);
        const int flags = got ? st.flags : 0;
        if (got)
            s->align_err_us = st.phase_us;
        if ((flags & RDOF_OPS_SNAPPING) && s->snap_polls < RDOF_SPLIT_SNAP_POLLS) {
            s->snap_polls++;
            s->has_snap_due = true;
            s->snap_due = now + RDOF_SPLIT_SNAP_POLL_S;
            return;
        }
        s->align_flags = flags;
        if (flags & RDOF_OPS_ALIGNED)
            s->align_state = RDOF_ALIGN_LOCKED;
        else { /* it stayed off (or never said): again soon */
            s->align_state = RDOF_ALIGN_WAITING;
            s->align_due = now + RDOF_SPLIT_TIME_EVERY_S;
        }
        if (s->align_jump || !(flags & RDOF_OPS_ALIGNED)) {
            /* its slot clock jumped: what the RX half labels from the old epoch is wrong until it has the new one */
            s->epoch_ok = false;
            rdof_epoch_relay_reset(&s->relay);
            s->has_epoch_due = true;
            s->epoch_due = now;
        }
        break;
    }
    case RDOF_SPLIT_DONE_STATUS: {
        if (!has_data)
            return;
        if (!ok || len < 6) {
            uint8_t p[2] = {RDOF_MMDVM_GET_STATUS, len ? data[0] : 4};
            up(s, RDOF_MMDVM_NAK, p, 2);
            return;
        }
        uint8_t tx_flags = s->halves[0].protocol >= 2 ? data[1] : data[2];
        uint8_t carrier = rdof_split_busy_mask(s, now) ? 0x40 : 0;
        uint8_t p[10] = {0x02, (uint8_t)s->mode, (uint8_t)((tx_flags & 0x09) | carrier), 0, data[4], data[5], 0, 0, 0, 0};
        up(s, RDOF_MMDVM_GET_STATUS, p, sizeof p);
        break;
    }
    }
}

/* ── frames from a half ──────────────────────────────────────────────────── */

static bool unanswered(uint8_t c)
{
    return c == RDOF_MMDVM_DMR_DATA1 || c == RDOF_MMDVM_DMR_DATA2 || c == RDOF_MMDVM_DMR_START ||
           c == RDOF_MMDVM_DMR_SHORTLC || c == RDOF_MMDVM_DMR_ABORT;
}

int rdof_split_busy_mask(const rdof_split_t *s, double now)
{
    int m = 0;
    for (int slot = 1; slot <= 2; slot++)
        if (s->open[slot] || now < s->data_until[slot])
            m |= 1 << (slot - 1);
    return m;
}

static void uplink(rdof_split_t *s, uint8_t command, const uint8_t *p, size_t n, double now)
{
    int slot = command == RDOF_MMDVM_DMR_DATA1 || command == RDOF_MMDVM_DMR_LOST1 ? 1 : 2;
    if (command == RDOF_MMDVM_DMR_LOST1 || command == RDOF_MMDVM_DMR_LOST2) {
        s->open[slot] = false;
        if (s->epoch_ok)
            up(s, command, p, n);
        return;
    }
    if (!n)
        return;
    uint8_t control = p[0];
    if (control == TERMINATOR)
        s->open[slot] = false;
    else if (control == VOICE_HEADER || (control & RDOF_SYNC_AUDIO) || !(control & RDOF_SYNC_DATA))
        s->open[slot] = true;
    else
        s->data_until[slot] = now + RDOF_SPLIT_DATA_BUSY_S;
    if (!s->epoch_ok) {
        s->dropped++; /* its slot is a guess until the RX half has the epoch */
        return;
    }
    up(s, command, p, n);
}

typedef struct {
    rdof_split_t *s;
    int which;
    double now;
} half_ctx_t;

static void on_half_frame(void *ctx, uint8_t command, const uint8_t *p, size_t n)
{
    half_ctx_t *c = ctx;
    rdof_split_t *s = c->s;
    rdof_split_half_t *h = &s->halves[c->which];
    if (command == RDOF_MMDVM_DMR_DATA1 || command == RDOF_MMDVM_DMR_DATA2 || command == RDOF_MMDVM_DMR_LOST1 ||
        command == RDOF_MMDVM_DMR_LOST2) {
        if (c->which == RDOF_SPLIT_RX)
            uplink(s, command, p, n, c->now);
        return;
    }
    if (command == RDOF_MMDVM_ACK || command == RDOF_MMDVM_NAK) {
        if (!n)
            return;
        int key = p[0];
        if (unanswered((uint8_t)key)) {
            if (command == RDOF_MMDVM_NAK && c->which == RDOF_SPLIT_TX)
                up(s, RDOF_MMDVM_NAK, p, n < 2 ? n : 2); /* the repeater's to judge (buffer full ...) */
            return;
        }
        if (key == RDOF_ODR_SPLIT && h->has_inflight && (h->inflight.key >> 8) == RDOF_ODR_SPLIT)
            key = h->inflight.key; /* NAK 1: no extension */
        reply(s, c->which, key, command == RDOF_MMDVM_ACK, p + 1, n - 1, c->now);
        return;
    }
    if (command == RDOF_ODR_SPLIT) {
        if (n < 2)
            return;
        reply(s, c->which, 0xD600 | p[0], p[1] == 0, p + 2, n - 2, c->now);
        return;
    }
    reply(s, c->which, command, true, p, n, c->now);
}

void rdof_split_from_half(rdof_split_t *s, int which, const uint8_t *data, size_t n, double now)
{
    half_ctx_t c = {s, which, now};
    rdof_mmdvm_reader_feed(&s->halves[which].reader, data, n, on_half_frame, &c);
}

/* ── upstream ────────────────────────────────────────────────────────────── */

static void both(rdof_split_t *s, uint8_t command, const uint8_t *p, size_t n, double now)
{
    if (command == RDOF_MMDVM_SET_MODE && n >= 1 && p[0] == RDOF_MODE_DMR && !s->tx_role) {
        /* back from a CW ID: the TX half into its role again first */
        uint8_t r[7];
        r[0] = RDOF_SPLIT_ROLE_TX;
        memcpy(r + 1, s->role_params, 5);
        r[RDOF_OPS_STREAM_MODE_AT] = (uint8_t)s->stream_mode;
        split_req(s, RDOF_SPLIT_TX, RDOF_SPLIT_SET_ROLE, r, s->stream_mode != RDOF_OPS_STREAM_FIFO ? 7 : 6,
                  RDOF_SPLIT_DONE_RESTORE, 0, now);
    }
    int j = join_new(s, command, n ? p[0] : 0);
    request(s, RDOF_SPLIT_TX, command, p, n, command, RDOF_SPLIT_DONE_JOIN, j, now);
    request(s, RDOF_SPLIT_RX, command, p, n, command, RDOF_SPLIT_DONE_JOIN, j, now);
}

static void cwid(rdof_split_t *s, const uint8_t *p, size_t n, double now)
{
    /* CW ID only from the normal role (split-duplex.md 8.2): the TX half out of its role, then the ID */
    s->tx_role = false;
    s->epoch_ok = false;
    rdof_epoch_relay_reset(&s->relay);
    uint8_t role = RDOF_SPLIT_ROLE_NORMAL;
    split_req(s, RDOF_SPLIT_TX, RDOF_SPLIT_SET_ROLE, &role, 1, RDOF_SPLIT_DONE_NONE, 0, now);
    request(s, RDOF_SPLIT_TX, RDOF_MMDVM_SEND_CWID, p, n, RDOF_MMDVM_SEND_CWID, RDOF_SPLIT_DONE_CWID, 0, now);
}

typedef struct {
    rdof_split_t *s;
    double now;
} up_ctx_t;

static void on_up_frame(void *ctx, uint8_t command, const uint8_t *p, size_t n)
{
    up_ctx_t *c = ctx;
    rdof_split_t *s = c->s;
    if (s->failed[0])
        return;
    if (command == RDOF_MMDVM_GET_VERSION) {
        if (s->ready)
            version_reply(s);
        else
            s->version_asked = true;
        return;
    }
    if (!s->ready)
        return;
    switch (command) {
    case RDOF_MMDVM_GET_STATUS: {
        /* one at a time: a status the TX half never answers mustn't queue up asks ahead of TIME */
        rdof_split_half_t *tx = &s->halves[RDOF_SPLIT_TX];
        bool asked = tx->has_inflight && tx->inflight.key == RDOF_MMDVM_GET_STATUS;
        for (int i = 0; i < tx->qcount && !asked; i++)
            asked = tx->queue[(tx->qhead + i) % RDOF_SPLIT_QUEUE].key == RDOF_MMDVM_GET_STATUS;
        if (!asked)
            request(s, RDOF_SPLIT_TX, RDOF_MMDVM_GET_STATUS, NULL, 0, RDOF_MMDVM_GET_STATUS, RDOF_SPLIT_DONE_STATUS, 0,
                    c->now);
        break;
    }
    case RDOF_MMDVM_SET_FREQ:
    case RDOF_MMDVM_SET_CONFIG:
    case RDOF_MMDVM_SET_MODE: both(s, command, p, n, c->now); break;
    case RDOF_MMDVM_SEND_CWID: cwid(s, p, n, c->now); break;
    case RDOF_MMDVM_DMR_DATA1:
    case RDOF_MMDVM_DMR_DATA2:
        if (!s->transmit) { /* the slot clock gate says stop (on_loss: stop) */
            uint8_t nak[2] = {command, 2};
            up(s, RDOF_MMDVM_NAK, nak, 2);
            break;
        }
        to_half(s, RDOF_SPLIT_TX, command, p, n);
        break;
    case RDOF_MMDVM_DMR_START:
        s->keyed_up = n && p[0];
        if (!s->keyed_up && s->align_state == RDOF_ALIGN_HELD) { /* unkeyed: it can snap now */
            s->has_align_due = true;
            s->align_due = c->now;
        }
        if (s->keyed_up && !s->transmit)
            break;
        to_half(s, RDOF_SPLIT_TX, command, p, n);
        break;
    case RDOF_MMDVM_DMR_SHORTLC:
    case RDOF_MMDVM_DMR_ABORT: to_half(s, RDOF_SPLIT_TX, command, p, n); break;
    default: {
        uint8_t nak[2] = {command, 1};
        up(s, RDOF_MMDVM_NAK, nak, 2);
    }
    }
}

void rdof_split_upstream(rdof_split_t *s, const uint8_t *data, size_t n, double now)
{
    up_ctx_t c = {s, now};
    rdof_mmdvm_reader_feed(&s->up_reader, data, n, on_up_frame, &c);
}

/* ── UTC slot timing: align (docs/spec/slot-timing.md) ───────────────────── */

#define SLOT_NS_I 30000000LL

static int64_t floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

void rdof_split_set_utc(rdof_split_t *s, bool utc, double utc_minus_host_us, double bound_us, int offset_us, bool transmit,
                      double now)
{
    s->utc = utc;
    s->utc_minus_host_us = utc_minus_host_us;
    s->utc_bound_us = bound_us;
    s->offset_us = offset_us;
    if (transmit != s->transmit) {
        s->transmit = transmit;
        if (s->ready && s->keyed_up) {
            uint8_t k = transmit ? 1 : 0;
            to_half(s, RDOF_SPLIT_TX, RDOF_MMDVM_DMR_START, &k, 1);
        }
    }
    if (!utc && (s->align_state == RDOF_ALIGN_SLEWING || s->align_state == RDOF_ALIGN_LOCKED))
        s->align_state = RDOF_ALIGN_WAITING;
    else if (utc && s->align_state == RDOF_ALIGN_OFF) {
        s->align_state = RDOF_ALIGN_WAITING;
        if (s->ready && !s->has_align_due) {
            s->has_align_due = true;
            s->align_due = now;
        }
    }
}

/* The next even UTC grid boundary after now as a time on the TX half's clock, its period
 * in the TX half's units, and the grid's n. */
static void align_target(const rdof_clock_sync_t *tx, double utc_minus_host_us, int offset_us, double now_h,
                         uint32_t *t, uint32_t *period, int64_t *n_out)
{
    int64_t utc_ns = (int64_t)llrint((now_h + utc_minus_host_us) * 1000);
    int64_t x = utc_ns - (int64_t)offset_us * 1000;
    int64_t n = -floor_div(-x, SLOT_NS_I);
    int64_t b = n * SLOT_NS_I + (int64_t)offset_us * 1000;
    if (n & 1) {
        n++;
        b += SLOT_NS_I;
    }
    double h = now_h + (double)(b - utc_ns) / 1000.0;
    double p = rint(SLOT_NS * (1.0 + clock_rate(tx)));
    long per = (long)p;
    per = per < PERIOD_MIN ? PERIOD_MIN : per > PERIOD_MAX ? PERIOD_MAX : per;
    *t = rdof_clock_sync_to_modem(tx, h);
    *period = (uint32_t)per;
    *n_out = n;
}

static void align(rdof_split_t *s, double now)
{
    rdof_split_half_t *tx = &s->halves[RDOF_SPLIT_TX];
    if (!s->has_align_first) {
        s->has_align_first = true;
        s->align_first = now;
    }
    bool early = now - s->align_first < RDOF_SPLIT_EPOCH_EARLY_S; /* the rates are still being learned */
    s->align_due = now + (early ? RDOF_SPLIT_EPOCH_EARLY_EVERY_S : RDOF_SPLIT_ALIGN_EVERY_S);
    if (!s->utc || !s->tx_role) {
        if (s->align_state == RDOF_ALIGN_SLEWING || s->align_state == RDOF_ALIGN_LOCKED)
            s->align_state = RDOF_ALIGN_WAITING; /* the host's clock is off the grid: the TX half free runs */
        return;
    }
    if (!rdof_ops_can_align(tx->caps, s->stream_mode)) {
        if (s->align_state != RDOF_ALIGN_UNSUPPORTED) {
            char msg[300];
            snprintf(msg, sizeof msg,
                     "Split duplex: the TX half (%s) can't be put on the UTC slot grid: no ALIGN in its INFO "
                     "capabilities",
                     tx->description);
            logf_(s, msg);
        }
        s->align_state = RDOF_ALIGN_UNSUPPORTED;
        return;
    }
    double h = (double)host_us(now);
    uint32_t t, period;
    int64_t n;
    align_target(&tx->sync, s->utc_minus_host_us, s->offset_us, h, &t, &period, &n);
    /* its slot clock that much early: the air on the grid */
    t -= (uint32_t)rdof_ops_air_delay_us(tx->info, tx->info_len);
    double bound = rdof_clock_sync_bound_us(&tx->sync) + s->utc_bound_us;
    if (!rdof_epoch_relay_decide(&s->align_relay, t, period, bound)) {
        s->align_due = now + RDOF_SPLIT_TIME_EVERY_S;
        return;
    }
    s->align_busy = true;
    s->align_n = n;
    uint8_t body[16];
    size_t len = rdof_ops_align(body, t, period, n, -1);
    split_req(s, RDOF_SPLIT_TX, RDOF_OPS_SUB_ALIGN, body, len, RDOF_SPLIT_DONE_ALIGN, 0, now);
}

/* ── time ────────────────────────────────────────────────────────────────── */

static void at(rdof_split_t *s, double now)
{
    if (!s->tx_role)
        return;
    int mask = rdof_split_busy_mask(s, now);
    if (mask == s->at_sent && now - s->at_at < RDOF_SPLIT_AT_EVERY_S)
        return;
    s->at_sent = mask;
    s->at_at = now;
    uint8_t p[2] = {(uint8_t)mask, 0};
    split_req(s, RDOF_SPLIT_TX, RDOF_SPLIT_SET_AT, p, 2, RDOF_SPLIT_DONE_NONE, 0, now);
}

void rdof_split_tick(rdof_split_t *s, double now)
{
    if (s->failed[0])
        return;
    for (int i = 0; i < 2; i++) {
        rdof_split_half_t *h = &s->halves[i];
        if (h->has_inflight && now - h->inflight.sent_at > RDOF_SPLIT_REQUEST_TIMEOUT_S) {
            rdof_split_req_t r = h->inflight;
            h->has_inflight = false;
            h->misses++;
            done(s, i, &r, false, NULL, 0, false, now);
            if (s->failed[0])
                return;
            if (s->ready && h->misses >= RDOF_SPLIT_DEAD_MISSES) {
                char msg[80];
                snprintf(msg, sizeof msg, "the %s hasn't answered %d requests in a row", HALF_NAMES[i],
                         RDOF_SPLIT_DEAD_MISSES);
                fail(s, msg);
                return;
            }
            pump(s, i, now);
        }
    }
    if (!s->ready)
        return;
    if (now >= s->next_time) {
        s->next_time = now + RDOF_SPLIT_TIME_EVERY_S;
        for (int i = 0; i < 2; i++)
            time_req(s, i, now);
    }
    if (s->has_epoch_due && now >= s->epoch_due && !s->epoch_busy)
        epoch(s, now, -1);
    if (s->has_snap_due && now >= s->snap_due && !s->align_busy) {
        s->has_snap_due = false;
        s->align_busy = true;
        split_req(s, RDOF_SPLIT_TX, RDOF_SPLIT_STATUS, NULL, 0, RDOF_SPLIT_DONE_SNAP, 0, now);
    }
    if (s->has_align_due && now >= s->align_due && !s->align_busy)
        align(s, now);
    at(s, now);
}

/* ── setup ───────────────────────────────────────────────────────────────── */

static int clampi(double v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : (int)v;
}

void rdof_split_params_default(rdof_split_params_t *p)
{
    memset(p, 0, sizeof *p);
}

void rdof_split_init(rdof_split_t *s, double now, const rdof_split_params_t *p)
{
    memset(s, 0, sizeof *s);
    rdof_split_params_t d;
    if (!p) {
        rdof_split_params_default(&d);
        p = &d;
    }
    s->role_params[0] = (uint8_t)clampi(p->lease_s, 0, 255);
    s->role_params[1] = (uint8_t)clampi(p->power_cap, 0, 255);
    s->role_params[2] = (uint8_t)clampi(p->prebuffer, 0, 12);
    s->role_params[3] = (uint8_t)clampi(rint(p->max_key_s / 10), 0, 60);
    s->role_params[4] = (uint8_t)clampi(p->cooldown_s, 0, 255);
    rdof_mmdvm_reader_init(&s->up_reader);
    for (int i = 0; i < 2; i++) {
        rdof_mmdvm_reader_init(&s->halves[i].reader);
        rdof_clock_sync_init(&s->halves[i].sync);
        s->halves[i].protocol = 1;
        s->halves[i].last_reply = -INFINITY;
    }
    s->at_sent = -1;
    s->at_at = -INFINITY;
    s->data_until[0] = s->data_until[1] = s->data_until[2] = -INFINITY;
    s->next_time = now;
    rdof_epoch_relay_init(&s->relay, RDOF_RELAY_LIMIT_US, RDOF_RELAY_AGREE_US);
    rdof_epoch_relay_init(&s->align_relay, RDOF_SPLIT_ALIGN_LIMIT_US, RDOF_SPLIT_ALIGN_AGREE_US);
    s->transmit = true;
    for (int i = 0; i < 2; i++)
        request(s, i, RDOF_MMDVM_GET_VERSION, NULL, 0, RDOF_MMDVM_GET_VERSION, RDOF_SPLIT_DONE_VERSION, 0, now);
}

#endif /* RDOF_FEATURE_REPEATER */
