// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The MQTT network (rdof/mqttnet.h, docs/spec/fw/mqtt-net.md). */
#include "rdof/mqttnet.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "rdof/bits.h"
#include "rdof/json.h"
#include "rdof/layer2.h"
#include "rdof/mmdvm.h"
#include "rdof/pdu.h"
#include "rdof/router.h"
#include "rdof/ambe.h"

/* ── the call message ─────────────────────────────────────────────────────── */

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put_le24(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16);
}
static uint32_t le24(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16; }

void rdof_mn_begin(rdof_mn_writer_t *w, uint8_t *buf, size_t cap, const rdof_mn_hdr_t *h)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->items = 0;
    w->overflow = cap < RDOF_MN_HEADER;
    if (w->overflow)
        return;
    buf[0] = RDOF_MN_VERSION;
    buf[1] = h->flags;
    buf[2] = h->path;
    buf[3] = h->seq;
    put_le32(buf + 4, h->stream);
    put_le24(buf + 8, h->src);
    put_le24(buf + 11, h->dst);
    w->len = RDOF_MN_HEADER;
}

bool rdof_mn_item(rdof_mn_writer_t *w, int type, const void *value, size_t len)
{
    const size_t head = len >= 255 ? 4 : 2;
    if (w->overflow || len > 0xFFFF || w->len + head + len > w->cap) {
        w->overflow = true;
        return false;
    }
    uint8_t *p = w->buf + w->len;
    p[0] = (uint8_t)type;
    if (len >= 255) {
        p[1] = 255;
        p[2] = (uint8_t)len, p[3] = (uint8_t)(len >> 8);
    } else {
        p[1] = (uint8_t)len;
    }
    if (len)
        memcpy(p + head, value, len);
    w->len += head + len;
    w->items++;
    return true;
}

bool rdof_mn_voice(rdof_mn_writer_t *w, int index, const uint8_t ambe27[27])
{
    uint8_t v[28];
    v[0] = (uint8_t)index;
    memcpy(v + 1, ambe27, 27);
    return rdof_mn_item(w, RDOF_MN_VOICE, v, sizeof v);
}

bool rdof_mn_lc(rdof_mn_writer_t *w, int type, const uint8_t lc9[9]) { return rdof_mn_item(w, type, lc9, 9); }

bool rdof_mn_csbk(rdof_mn_writer_t *w, const uint8_t csbk10[10], int count)
{
    uint8_t v[11];
    memcpy(v, csbk10, 10);
    v[10] = (uint8_t)(count < 1 ? 1 : count > 255 ? 255 : count);
    return rdof_mn_item(w, RDOF_MN_CSBK, v, sizeof v);
}

bool rdof_mn_quality(rdof_mn_writer_t *w, int bits, int rssi_dbm)
{
    const int r = rssi_dbm < 0 ? -rssi_dbm : 0;
    const uint8_t v[2] = {(uint8_t)(bits < 0 ? 0 : bits > 255 ? 255 : bits), (uint8_t)(r > 255 ? 255 : r)};
    return rdof_mn_item(w, RDOF_MN_QUALITY, v, sizeof v);
}

bool rdof_mn_data_pdu(rdof_mn_writer_t *w, int part, bool more, int rate, int nheaders, const uint8_t *headers10,
                      const uint8_t *data, size_t len)
{
    const size_t n = 3 + 10 * (size_t)nheaders + len;
    const size_t head = n >= 255 ? 4 : 2;
    if (w->overflow || n > 0xFFFF || w->len + head + n > w->cap || nheaders < 0) {
        w->overflow = true;
        return false;
    }
    uint8_t *p = w->buf + w->len;
    p[0] = RDOF_MN_DATA_PDU;
    if (n >= 255) {
        p[1] = 255;
        p[2] = (uint8_t)n, p[3] = (uint8_t)(n >> 8);
    } else {
        p[1] = (uint8_t)n;
    }
    p += head;
    p[0] = (uint8_t)((part & 0x7F) | (more ? 0x80 : 0));
    p[1] = (uint8_t)rate;
    p[2] = (uint8_t)nheaders;
    if (nheaders)
        memcpy(p + 3, headers10, 10 * (size_t)nheaders);
    if (len)
        memcpy(p + 3 + 10 * nheaders, data, len);
    w->len += head + n;
    w->items++;
    return true;
}

size_t rdof_mn_end(rdof_mn_writer_t *w) { return w->overflow ? 0 : w->len; }

bool rdof_mn_parse(const uint8_t *msg, size_t len, rdof_mn_hdr_t *h)
{
    if (len < RDOF_MN_HEADER || msg[0] != RDOF_MN_VERSION)
        return false;
    h->flags = msg[1];
    h->path = msg[2];
    h->seq = msg[3];
    h->stream = le32(msg + 4);
    h->src = le24(msg + 8);
    h->dst = le24(msg + 11);
    return true;
}

void rdof_mn_reader(rdof_mn_reader_t *r, const uint8_t *msg, size_t len)
{
    r->msg = msg;
    r->len = len;
    r->at = RDOF_MN_HEADER;
    r->count = 0;
    r->bad = len < RDOF_MN_HEADER;
}

bool rdof_mn_next(rdof_mn_reader_t *r, rdof_mn_item_t *it)
{
    if (r->bad || r->at >= r->len)
        return false;
    const uint8_t *p = r->msg + r->at;
    const size_t left = r->len - r->at;
    if (left < 2) {
        r->bad = true;
        return false;
    }
    size_t n = p[1], head = 2;
    if (n == 255) {
        if (left < 4) {
            r->bad = true;
            return false;
        }
        n = (size_t)p[2] | (size_t)p[3] << 8;
        head = 4;
    }
    if (head + n > left) {
        r->bad = true;
        return false;
    }
    it->type = p[0];
    it->value = p + head;
    it->len = n;
    r->at += head + n;
    r->count++;
    return true;
}

bool rdof_mn_item_ok(const rdof_mn_item_t *it)
{
    switch (it->type) {
    case RDOF_MN_VOICE: return it->len == 28 && it->value[0] < 6;
    case RDOF_MN_LC_HEADER:
    case RDOF_MN_TERMINATOR:
    case RDOF_MN_EMBEDDED_LC:
    case RDOF_MN_GPS:
    case RDOF_MN_ORIGIN: return it->len == 9;
    case RDOF_MN_PI_HEADER: return it->len == 10;
    case RDOF_MN_CSBK: return it->len == 11;
    case RDOF_MN_QUALITY: return it->len == 2;
    case RDOF_MN_TALKER_ALIAS: return it->len >= 1;
    case RDOF_MN_RAW: return it->len == 1 + RDOF_BURST_BYTES;
    case RDOF_MN_DATA_PDU: {
        rdof_mn_pdu_t p;
        return rdof_mn_pdu(it, &p);
    }
    default: return true;
    }
}

int rdof_mn_count(const uint8_t *msg, size_t len)
{
    rdof_mn_reader_t r;
    rdof_mn_item_t it;
    rdof_mn_reader(&r, msg, len);
    while (rdof_mn_next(&r, &it))
        ;
    return r.bad ? -1 : r.count;
}

bool rdof_mn_pdu(const rdof_mn_item_t *it, rdof_mn_pdu_t *o)
{
    if (it->type != RDOF_MN_DATA_PDU || it->len < 3)
        return false;
    const uint8_t *v = it->value;
    o->part = v[0] & 0x7F;
    o->more = (v[0] & 0x80) != 0;
    o->rate = v[1];
    o->nheaders = v[2];
    if (o->rate > RDOF_MN_RATE_1 || o->nheaders > RDOF_MN_DATA_HEADERS || 3 + 10 * (size_t)o->nheaders > it->len)
        return false;
    if (o->part == 0 && o->nheaders < 1)
        return false;
    o->headers = v + 3;
    o->data = v + 3 + 10 * o->nheaders;
    o->len = it->len - 3 - 10 * (size_t)o->nheaders;
    return true;
}

int rdof_mn_rate_of_dt(int dt)
{
    return dt == RDOF_DT_RATE_12 ? RDOF_MN_RATE_12 : dt == RDOF_DT_RATE_34 ? RDOF_MN_RATE_34
                                                   : dt == RDOF_DT_RATE_1  ? RDOF_MN_RATE_1
                                                                            : -1;
}

int rdof_mn_dt_of_rate(int rate)
{
    return rate == RDOF_MN_RATE_12 ? RDOF_DT_RATE_12 : rate == RDOF_MN_RATE_34 ? RDOF_DT_RATE_34
                                                       : rate == RDOF_MN_RATE_1  ? RDOF_DT_RATE_1
                                                                                  : -1;
}

/* ── topics ──────────────────────────────────────────────────────────────── */

bool rdof_mn_node_ok(const char *n)
{
    size_t k = 0;
    if (!n)
        return false;
    for (; n[k]; k++) {
        const char c = n[k];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
              c == '-'))
            return false;
    }
    return k >= 1 && k <= RDOF_MN_NODE_MAX;
}

static size_t fit(int n, size_t cap) { return n > 0 && (size_t)n < cap ? (size_t)n : 0; }

size_t rdof_mn_topic(char *out, size_t cap, const char *root, int kind, uint32_t id, const char *node)
{
    if (kind == RDOF_MN_T_DL)
        return fit(snprintf(out, cap, "%s/dl/%s", root, node), cap);
    if (kind != RDOF_MN_T_TG && kind != RDOF_MN_T_PC)
        return 0;
    return fit(snprintf(out, cap, "%s/%s/%lu/%s", root, kind == RDOF_MN_T_TG ? "tg" : "pc", (unsigned long)id, node),
               cap);
}

size_t rdof_mn_node_topic(char *out, size_t cap, const char *root, const char *node, const char *leaf)
{
    return fit(snprintf(out, cap, "%s/node/%s/%s", root, node, leaf), cap);
}

int rdof_mn_topic_parse(const char *topic, const char *root, uint32_t *id, char *node, size_t cap)
{
    const size_t rl = strlen(root);
    if (strncmp(topic, root, rl) || topic[rl] != '/')
        return RDOF_MN_T_NONE;
    const char *t = topic + rl + 1, *n;
    int kind;
    *id = 0;
    if (!strncmp(t, "dl/", 3)) {
        kind = RDOF_MN_T_DL;
        n = t + 3;
    } else if (!strncmp(t, "tg/", 3) || !strncmp(t, "pc/", 3)) {
        kind = t[0] == 't' ? RDOF_MN_T_TG : RDOF_MN_T_PC;
        const char *d = t + 3;
        uint32_t v = 0;
        int digits = 0;
        for (; *d >= '0' && *d <= '9'; d++, digits++) {
            v = v * 10 + (uint32_t)(*d - '0');
            if (digits > 8)
                return RDOF_MN_T_NONE;
        }
        if (!digits || *d != '/' || v > RDOF_ID_MAX || (digits > 1 && t[3] == '0'))
            return RDOF_MN_T_NONE;
        *id = v;
        n = d + 1;
    } else {
        return RDOF_MN_T_NONE;
    }
    if (strchr(n, '/') || !rdof_mn_node_ok(n))
        return RDOF_MN_T_NONE;
    if (node && cap)
        snprintf(node, cap, "%s", n);
    return kind;
}

size_t rdof_mn_call_key(uint32_t src, uint32_t dst, bool group, double wall, char *out, size_t cap)
{
    const long long t = (long long)floor(wall / 2.0) * 2;
    return fit(snprintf(out, cap, "%lu-%lu-%c-%lld", (unsigned long)src, (unsigned long)dst, group ? 'g' : 'p', t),
               cap);
}

/* ── event JSON ──────────────────────────────────────────────────────────── */

static void ev_head(rdof_json_t *j, char *buf, size_t cap, const char *ev, const char *node, double wall, int path,
                    int slot, const char *source, uint32_t stream)
{
    char ts[32];
    rdof_iso_time(wall, ts);
    rdof_json_init(j, buf, cap);
    rdof_json_open(j, NULL);
    rdof_json_str(j, "ev", ev);
    rdof_json_str(j, "at", ts);
    rdof_json_str(j, "node", node);
    rdof_json_int(j, "path", path);
    rdof_json_int(j, "slot", slot);
    rdof_json_str(j, "source", source);
    rdof_json_int(j, "stream_id", stream);
}

static size_t ev_done(rdof_json_t *j)
{
    rdof_json_close(j);
    return rdof_json_done(j) ? j->len : 0;
}

size_t rdof_mn_tx_event(const char *node, double wall, int path, int slot, uint32_t stream, const char *action,
                        uint32_t src, uint32_t dst, const char *from, char *out, size_t cap)
{
    rdof_json_t j;
    ev_head(&j, out, cap, "tx", node, wall, path, slot, "net", stream);
    rdof_json_str(&j, "action", action);
    rdof_json_int(&j, "src", src);
    rdof_json_int(&j, "dst", dst);
    if (from)
        rdof_json_str(&j, "from", from);
    return ev_done(&j);
}

size_t rdof_mn_data_result_event(const char *node, double wall, int path, int slot, uint32_t stream, uint32_t src,
                                 uint32_t dst, const char *from, uint32_t from_stream, const char *result, int tries,
                                 char *out, size_t cap)
{
    rdof_json_t j;
    ev_head(&j, out, cap, "data_result", node, wall, path, slot, "net", stream);
    rdof_json_int(&j, "src", src);
    rdof_json_int(&j, "dst", dst);
    rdof_json_str(&j, "from", from ? from : "");
    rdof_json_int(&j, "from_stream", from_stream);
    rdof_json_str(&j, "result", result);
    rdof_json_int(&j, "tries", tries);
    return ev_done(&j);
}

/* ── data_result ─────────────────────────────────────────────────────────── */

void rdof_mn_results_add(rdof_mn_results_t *r, const rdof_mn_hdr_t *h, bool confirmed, const char *from, int path,
                         double now)
{
    int k, use = 0;
    for (k = 0; k < RDOF_MN_PENDING; k++) {
        if (!r->p[k].used) {
            use = k;
            break;
        }
        if (r->p[k].at < r->p[use].at)
            use = k;
    }
    rdof_mn_pending_t *p = &r->p[use];
    memset(p, 0, sizeof *p);
    p->used = true;
    p->confirmed = confirmed;
    p->stream = h->stream;
    p->src = h->src;
    p->dst = h->dst;
    p->path = path;
    p->slot = (h->flags & RDOF_MN_F_SLOT2) ? 2 : 1;
    snprintf(p->from, sizeof p->from, "%s", from ? from : "");
    p->at = now;
}

static const char *result_done(rdof_mn_pending_t *p, rdof_mn_pending_t *out, const char *result)
{
    *out = *p;
    p->used = false;
    return result;
}

const char *rdof_mn_results_sent(rdof_mn_results_t *r, uint32_t stream, double now, rdof_mn_pending_t *out)
{
    for (int k = 0; k < RDOF_MN_PENDING; k++) {
        rdof_mn_pending_t *p = &r->p[k];
        if (!p->used || p->sent || p->stream != stream)
            continue;
        if (!p->confirmed)
            return result_done(p, out, "unconfirmed");
        p->sent = true;
        p->at = now;
    }
    return NULL;
}

const char *rdof_mn_results_response(rdof_mn_results_t *r, uint32_t src, uint32_t dst, int klass, int type,
                                     rdof_mn_pending_t *out)
{
    for (int k = 0; k < RDOF_MN_PENDING; k++) {
        rdof_mn_pending_t *p = &r->p[k];
        if (p->used && p->confirmed && p->dst == src && p->src == dst)
            return result_done(p, out, klass == 0 && type == 1 ? "delivered" : "failed");
    }
    return NULL;
}

const char *rdof_mn_results_tick(rdof_mn_results_t *r, double now, rdof_mn_pending_t *out)
{
    for (int k = 0; k < RDOF_MN_PENDING; k++) {
        rdof_mn_pending_t *p = &r->p[k];
        if (p->used && p->sent && now - p->at > RDOF_MN_RESPONSE_S)
            return result_done(p, out, "no_response");
        if (p->used && !p->sent && now - p->at > RDOF_MN_ON_AIR_S)
            return result_done(p, out, "failed");
    }
    return NULL;
}

/* ── the uplink ──────────────────────────────────────────────────────────── */

void rdof_mn_up_init(rdof_mn_up_t *u, const char *node)
{
    memset(u, 0, sizeof *u);
    u->node = node;
}

static double up_wall(rdof_mn_up_t *u) { return u->wall ? u->wall(u->ctx) : 0.0; }

static void up_event(rdof_mn_up_t *u, rdof_json_t *j)
{
    if (ev_done(j) && u->activity)
        u->activity(u->ctx, j->buf);
}

static uint32_t new_stream(rdof_mn_up_t *u)
{
    uint32_t r = u->rand32 ? u->rand32(u->ctx) : 0x12345678u;
    return r ? r : 1;
}

static int up_path(rdof_mn_up_t *u, int slot) { return u->path_of ? u->path_of(u->ctx, slot) : -1; }

static void up_publish(rdof_mn_up_t *u, rdof_mn_upslot_t *s, size_t len)
{
    if (!len) {
        u->too_big++;
        return;
    }
    if (u->publish)
        u->publish(u->ctx, (s->flags & RDOF_MN_F_GROUP) ? RDOF_MN_T_TG : RDOF_MN_T_PC, s->dst, u->out, len);
    u->messages++;
}

static void up_begin(rdof_mn_up_t *u, rdof_mn_upslot_t *s, rdof_mn_writer_t *w)
{
    const rdof_mn_hdr_t h = {.flags = s->flags,
                             .path = (uint8_t)(s->path >= 0 && s->path < 255 ? s->path : RDOF_MN_PATH_NONE),
                             .seq = s->seq, .stream = s->stream, .src = s->src, .dst = s->dst};
    rdof_mn_begin(w, u->out, u->max_msg && u->max_msg < sizeof u->out ? u->max_msg : sizeof u->out, &h);
}

static void up_end(rdof_mn_up_t *u, rdof_mn_upslot_t *s, rdof_mn_writer_t *w)
{
    const size_t n = rdof_mn_end(w);
    s->seq = (uint8_t)(s->seq + w->items);
    if (s->sending)
        up_publish(u, s, n);
}

static void lc9_of(const rdof_frame_t *f, uint8_t lc9[9], bool *ok)
{
    rdof_full_lc_t lc;
    *ok = rdof_decode_voice_lc(f->burst, f->dt, &lc);
    if (*ok)
        memcpy(lc9, lc.raw, 9);
    else
        rdof_group_lc(f->src, f->dst, f->group, 0, lc9);
}

/* A stream starts on the slot (its addresses known). */
static void stream_start(rdof_mn_up_t *u, rdof_mn_upslot_t *s, int slot, uint32_t src, uint32_t dst, bool group,
                         uint8_t extra_flags, double now)
{
    s->active = true;
    s->stream = new_stream(u);
    s->seq = 0;
    s->src = src;
    s->dst = dst;
    s->flags = (uint8_t)((slot == 2 ? RDOF_MN_F_SLOT2 : 0) | (group ? RDOF_MN_F_GROUP : 0) | extra_flags);
    s->path = up_path(u, slot);
    s->sending = u->admit ? u->admit(u->ctx, slot, src, dst, group) : true;
    if (!s->sending)
        u->not_sent++;
    s->start = s->last = now;
    s->start_wall = up_wall(u);
    s->bursts = s->lost = 0;
    s->bits = s->errs = 0;
    s->rssi_n = s->rssi_min = s->rssi_max = 0;
    s->rssi_sum = 0;
    s->voice = false;
    s->ta_have = s->ta_len = 0;
    s->ta_done = false;
}

static void call_start_event(rdof_mn_up_t *u, rdof_mn_upslot_t *s, int slot, int rssi)
{
    char buf[512], key[48];
    rdof_json_t j;
    if (s->path < 0 || !u->activity)
        return;
    rdof_mn_call_key(s->src, s->dst, (s->flags & RDOF_MN_F_GROUP) != 0, s->start_wall, key, sizeof key);
    ev_head(&j, buf, sizeof buf, "call_start", u->node, s->start_wall, s->path, slot, "rf", s->stream);
    rdof_json_int(&j, "src", s->src);
    rdof_json_int(&j, "dst", s->dst);
    rdof_json_bool(&j, "group", (s->flags & RDOF_MN_F_GROUP) != 0);
    rdof_json_bool(&j, "emergency", (s->flags & RDOF_MN_F_EMERGENCY) != 0);
    rdof_json_bool(&j, "privacy", (s->flags & RDOF_MN_F_PRIVACY) != 0);
    if (s->flags & RDOF_MN_F_LATE)
        rdof_json_bool(&j, "late_entry", true);
    rdof_json_str(&j, "call_key", key);
    rdof_json_int(&j, "rssi", rssi);
    rdof_json_bool(&j, "tx_ok", s->path < 64 && (u->tx_ok >> s->path & 1));
    up_event(u, &j);
}

static void call_end_event(rdof_mn_up_t *u, rdof_mn_upslot_t *s, int slot, bool lost)
{
    char buf[512];
    rdof_json_t j;
    if (s->path < 0 || !u->activity)
        return;
    const double wall = up_wall(u);
    ev_head(&j, buf, sizeof buf, "call_end", u->node, wall, s->path, slot, "rf", s->stream);
    rdof_json_str(&j, "reason", lost ? "lost" : "end");
    const double d = s->last - s->start;
    rdof_json_float3(&j, "duration", d > 0 ? d : 0);
    rdof_json_float3(&j, "ber", s->bits ? 100.0 * (double)s->errs / (double)s->bits : 0.0);
    if (s->rssi_n) {
        rdof_json_open(&j, "rssi");
        rdof_json_int(&j, "min", s->rssi_min);
        rdof_json_int(&j, "max", s->rssi_max);
        rdof_json_int(&j, "avg", (long long)lrint((double)s->rssi_sum / s->rssi_n));
        rdof_json_close(&j);
    }
    rdof_json_int(&j, "bursts", s->bursts);
    rdof_json_int(&j, "lost_bursts", s->lost);
    up_event(u, &j);
}

static void measure(rdof_mn_upslot_t *s, int errs, int rssi)
{
    s->bits += 141;
    s->errs += errs;
    if (rssi) {
        if (!s->rssi_n || rssi < s->rssi_min)
            s->rssi_min = rssi;
        if (!s->rssi_n || rssi > s->rssi_max)
            s->rssi_max = rssi;
        s->rssi_sum += rssi;
        s->rssi_n++;
    }
}

static int ambe_errors(const uint8_t burst[RDOF_BURST_BYTES], uint8_t ambe27[27])
{
    uint8_t fr[3][9];
    int e = 0;
    rdof_ambe_frames(burst, fr);
    for (int i = 0; i < 3; i++) {
        e += rdof_ambe_decode_frame(fr[i], NULL);
        memcpy(ambe27 + 9 * i, fr[i], 9);
    }
    return e;
}

/* A talker alias block (FLCO 4-7) into s; true when the alias is complete (the text in out, UTF-8). */
static bool alias_block(rdof_mn_upslot_t *s, const rdof_full_lc_t *lc, char *out, size_t cap)
{
    int at;
    if (lc->flco == RDOF_FLCO_TA_HEADER) {
        s->ta_format = lc->ta_format;
        s->ta_len = lc->ta_length;
        s->ta_have = 1;
        memset(s->ta, 0, sizeof s->ta);
        at = 0;
    } else {
        if (!(s->ta_have & 1))
            return false;
        at = 49 + 56 * (lc->flco - RDOF_FLCO_TA_BLOCK1);
        s->ta_have |= 1 << (lc->flco - RDOF_FLCO_TA_HEADER);
    }
    for (int i = 0; i < lc->ta_nbits && at + i < 8 * (int)sizeof s->ta; i++)
        rdof_set_field(s->ta, at + i, 1, lc->ta_bits[i]);
    if (s->ta_done)
        return false;
    const int bits_per = s->ta_format == 0 ? 7 : s->ta_format == 3 ? 16 : 8;
    const int need = s->ta_len * bits_per;
    int have = 0;
    for (int k = 0; k < 4 && (s->ta_have >> k & 1); k++)
        have += k == 0 ? 49 : 56;
    if (need <= 0 || have < need)
        return false;
    size_t o = 0;
    for (int c = 0; c < s->ta_len && o + 4 < cap; c++) {
        unsigned cp = (unsigned)rdof_get_field(s->ta, c * bits_per, bits_per);
        if (s->ta_format == 2) { /* UTF-8 already: the octets as they are */
            out[o++] = (char)cp;
            continue;
        }
        if (cp < 0x20)
            cp = '?';
        if (cp < 0x80) {
            out[o++] = (char)cp;
        } else if (cp < 0x800) {
            out[o++] = (char)(0xC0 | cp >> 6);
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[o++] = (char)(0xE0 | cp >> 12);
            out[o++] = (char)(0x80 | (cp >> 6 & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    while (o && out[o - 1] == ' ')
        o--;
    out[o] = 0;
    s->ta_done = true;
    return true;
}

static int error_m(int code)
{
    static const int m[] = {2, 20, 200, 2000, 20000, 200000};
    return code >= 0 && code < 6 ? m[code] : -1;
}

/* The embedded LC that completed with burst E: items for F's message, events. */
static void embedded_items(rdof_mn_up_t *u, rdof_mn_upslot_t *s, int slot, const uint8_t lc9[9], rdof_mn_writer_t *w)
{
    rdof_full_lc_t lc;
    char text[96], buf[512];
    rdof_json_t j;
    rdof_mn_lc(w, RDOF_MN_EMBEDDED_LC, lc9);
    rdof_parse_full_lc(lc9, &lc);
    if (lc.fid == 0 && lc.flco >= RDOF_FLCO_TA_HEADER && lc.flco <= RDOF_FLCO_TA_BLOCK3 &&
        alias_block(s, &lc, text, sizeof text)) {
        uint8_t v[1 + sizeof text];
        const size_t n = strlen(text);
        v[0] = (uint8_t)s->ta_format;
        memcpy(v + 1, text, n);
        rdof_mn_item(w, RDOF_MN_TALKER_ALIAS, v, 1 + n);
        if (s->path >= 0 && u->activity) {
            ev_head(&j, buf, sizeof buf, "talker_alias", u->node, up_wall(u), s->path, slot, "rf", s->stream);
            rdof_json_int(&j, "src", s->src);
            rdof_json_str(&j, "alias", text);
            up_event(u, &j);
        }
    } else if (lc.has_gps) {
        uint8_t v[9];
        put_le32(v, (uint32_t)(int32_t)lrint(lc.lat * 1e7));
        put_le32(v + 4, (uint32_t)(int32_t)lrint(lc.lon * 1e7));
        v[8] = (uint8_t)lc.position_error;
        rdof_mn_item(w, RDOF_MN_GPS, v, sizeof v);
        if (s->path >= 0 && u->activity) {
            ev_head(&j, buf, sizeof buf, "gps", u->node, up_wall(u), s->path, slot, "rf", s->stream);
            rdof_json_int(&j, "src", s->src);
            char num[32];
            snprintf(num, sizeof num, "%.7f", lc.lat);
            rdof_json_raw(&j, "lat", num);
            snprintf(num, sizeof num, "%.7f", lc.lon);
            rdof_json_raw(&j, "lon", num);
            const int m = error_m(lc.position_error);
            if (m < 0)
                rdof_json_raw(&j, "error_m", "null");
            else
                rdof_json_int(&j, "error_m", m);
            up_event(u, &j);
        }
    }
}

static const int LCSS_OF[6] = {0, 1, 3, 3, 2, 0};

/* A voice burst of the slot's stream: its message. late_lc: the late entry's LC, put first. */
static void voice_message(rdof_mn_up_t *u, rdof_mn_upslot_t *s, const rdof_frame_t *f, const uint8_t *late_lc)
{
    uint8_t ambe[27];
    rdof_mn_writer_t w;
    const int errs = ambe_errors(f->burst, ambe);
    const int n = f->n >= 0 && f->n < 6 ? f->n : 0;
    measure(s, errs, f->rssi);
    if (s->bursts)
        s->lost += (n - s->next_n + 6) % 6; /* bursts the voice index skipped (whole superframes aren't seen) */
    s->next_n = (n + 1) % 6;
    s->bursts++;
    up_begin(u, s, &w);
    if (late_lc)
        rdof_mn_lc(&w, RDOF_MN_EMBEDDED_LC, late_lc);
    rdof_mn_voice(&w, n, ambe);
    if (n == 5 && s->have_frag == 0x0F) {
        uint8_t raw[128], lc9[9];
        memcpy(raw, s->frags, 128);
        if (rdof_embedded_lc_decode(raw, lc9))
            embedded_items(u, s, f->slot, lc9, &w);
    }
    rdof_mn_quality(&w, errs, f->rssi);
    up_end(u, s, &w);
}

/* The embedded fragments, whatever the stream: B-E (n 1-4) with the right LCSS; A clears them. */
static void collect_frag(rdof_mn_upslot_t *s, const rdof_frame_t *f)
{
    if (f->dt != RDOF_DT_VOICE_SYNC && f->dt != RDOF_DT_VOICE)
        return;
    if (f->n == 0 || f->n == 5) {
        if (f->n == 0)
            s->have_frag = 0;
        return;
    }
    int cc, pi, lcss;
    if (f->n < 1 || f->n > 4 || !rdof_decode_emb(f->burst, &cc, &pi, &lcss, NULL) || lcss != LCSS_OF[f->n]) {
        s->have_frag = 0;
        return;
    }
    rdof_embedded_signalling(f->burst, s->frags[f->n - 1]);
    if (f->n == 1)
        s->have_frag = 1;
    else if (s->have_frag == (1 << (f->n - 1)) - 1)
        s->have_frag |= (uint8_t)(1 << (f->n - 1));
    else
        s->have_frag = 0;
}

static void stream_end(rdof_mn_up_t *u, rdof_mn_upslot_t *s, int slot, bool lost)
{
    if (s->voice)
        call_end_event(u, s, slot, lost);
    s->active = false;
    s->voice = false;
}

/* ── CSBKs ── */

static void csbk_desc10(const uint8_t *d, char *out, size_t cap)
{
    const int op = d[0] & 0x3F;
    const char *t = NULL;
    switch (op) {
    case 0x04: t = "Unit to Unit Voice Service Request"; break;
    case 0x05: t = "Unit to Unit Voice Answer Response"; break;
    case 0x26: t = "Negative Acknowledgment Response"; break;
    case 0x3D: t = "Preamble"; break;
    case 0x1F: t = "Call Alert"; break;
    case 0x20: t = "Call Alert Ack"; break;
    case 0x24: t = d[3] == 0x80 ? "Radio Check" : "Radio Check Ack"; break;
    case 0x27: t = "Call Emergency"; break;
    }
    if (t)
        snprintf(out, cap, "%s", t);
    else
        snprintf(out, cap, "CSBK 0x%02X", op);
}

static void csbk_event(rdof_mn_up_t *u, rdof_mn_upslot_t *s, int slot)
{
    char buf[512], desc[48];
    rdof_json_t j;
    if (s->path < 0 || !u->activity)
        return;
    csbk_desc10(s->csbk10, desc, sizeof desc);
    ev_head(&j, buf, sizeof buf, "csbk", u->node, up_wall(u), s->path, slot, "rf", s->stream);
    rdof_json_int(&j, "src", s->csbk_src);
    rdof_json_int(&j, "dst", s->csbk_dst);
    rdof_json_bool(&j, "group", s->csbk_group);
    rdof_json_int(&j, "opcode", s->csbk10[0] & 0x3F);
    rdof_json_str(&j, "desc", desc);
    if (s->csbk_count > 1)
        rdof_json_int(&j, "count", s->csbk_count);
    up_event(u, &j);
}

/* The folded CSBK goes out on its own (no data call took it). */
static void csbk_flush(rdof_mn_up_t *u, rdof_mn_upslot_t *s, int slot)
{
    (void)slot;
    if (!s->csbk)
        return;
    s->csbk = false;
    if (!s->active || s->data)
        return;
    rdof_mn_writer_t w;
    up_begin(u, s, &w);
    rdof_mn_csbk(&w, s->csbk10, s->csbk_count);
    up_end(u, s, &w);
    s->active = false;
}

static bool is_preamble(const uint8_t *d) { return (d[0] & 0x3F) == RDOF_CSBK_PREAMBLE; }

/* A CSBK the tracker passed (CRC good; its addresses in the frame). */
static void on_csbk(rdof_mn_up_t *u, rdof_mn_upslot_t *s, const rdof_frame_t *f, double now)
{
    uint8_t d[12];
    rdof_bptc196_decode(f->burst, d);
    if (!rdof_check_crc16(d, RDOF_MASK_CSBK))
        return;
    const bool pre = is_preamble(d);
    if (s->csbk && s->csbk_src == f->src && s->csbk_dst == f->dst && now - s->csbk_at < 1.0) {
        /* the same CSBK again, or the next preamble (its blocks to follow, octet 3, one less) */
        if ((pre && is_preamble(s->csbk10) && !memcmp(d, s->csbk10, 3) && !memcmp(d + 4, s->csbk10 + 4, 6)) ||
            (!pre && !memcmp(d, s->csbk10, 10))) {
            memcpy(s->csbk10, d, 10);
            s->csbk_count++;
            s->csbk_at = now;
            return;
        }
    }
    csbk_flush(u, s, f->slot);
    if (s->active && s->voice)
        stream_end(u, s, f->slot, true);
    stream_start(u, s, f->slot, f->src, f->dst, f->group, 0, now);
    s->csbk = true;
    memcpy(s->csbk10, d, 10);
    s->csbk_count = 1;
    s->csbk_src = f->src;
    s->csbk_dst = f->dst;
    s->csbk_group = f->group;
    s->csbk_at = now;
    s->data = false;
    if (!pre) { /* a CSBK on its own (a radio check): at once; repeats are folded into it */
        csbk_event(u, s, f->slot);
        rdof_mn_writer_t w;
        up_begin(u, s, &w);
        rdof_mn_csbk(&w, d, 1);
        rdof_mn_quality(&w, f->errors, f->rssi);
        up_end(u, s, &w);
        s->active = false; /* the CSBK stays for folding (s->csbk) */
    } else {
        csbk_event(u, s, f->slot);
    }
}

/* ── data ── */

#if RDOF_FEATURE_DATA_BLOCKS
static uint32_t fnv(const uint8_t *p, size_t n, uint32_t h)
{
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

static const char *sap_name(int sap)
{
    switch (sap) {
    case 0: return "udt";
    case 2: return "tcp_compressed";
    case 3: return "udp_compressed";
    case 4: return "ip";
    case 5: return "arp";
    case 9: return "proprietary";
    case 10: return "short";
    default: return "other";
    }
}

static void data_event(rdof_mn_up_t *u, rdof_mn_upslot_t *s, int slot, const uint8_t *data, size_t len)
{
    static char buf[1200];
    char b64[700];
    rdof_json_t j;
    if (s->path < 0 || !u->activity)
        return;
    ev_head(&j, buf, sizeof buf, "data", u->node, up_wall(u), s->path, slot, "rf", s->stream);
    rdof_json_int(&j, "src", s->src);
    rdof_json_int(&j, "dst", s->dst);
    rdof_json_bool(&j, "group", (s->flags & RDOF_MN_F_GROUP) != 0);
    rdof_json_str(&j, "sap", sap_name(s->sap));
    rdof_json_bool(&j, "confirmed", s->confirmed);
    rdof_json_int(&j, "bytes", (long long)len);
    rdof_base64(data, len < 512 ? len : 512, b64);
    rdof_json_str(&j, "payload", b64);
    if (s->sap == 4 && len >= 20 && (data[0] >> 4) == 4) {
        char a[16], z[16];
        const size_t ihl = (size_t)(data[0] & 0x0F) * 4;
        snprintf(a, sizeof a, "%u.%u.%u.%u", data[12], data[13], data[14], data[15]);
        snprintf(z, sizeof z, "%u.%u.%u.%u", data[16], data[17], data[18], data[19]);
        rdof_json_open(&j, "ip");
        rdof_json_str(&j, "src", a);
        rdof_json_str(&j, "dst", z);
        rdof_json_int(&j, "proto", data[9]);
        if ((data[9] == 17 || data[9] == 6) && len >= ihl + 4) {
            rdof_json_int(&j, "sport", data[ihl] << 8 | data[ihl + 1]);
            rdof_json_int(&j, "dport", data[ihl + 2] << 8 | data[ihl + 3]);
        }
        rdof_json_close(&j);
    }
    up_event(u, &j);
}

static void data_fail_event(rdof_mn_up_t *u, rdof_mn_upslot_t *s, int slot, const char *reason)
{
    char buf[400];
    rdof_json_t j;
    u->data_bad++;
    if (s->path < 0 || !u->activity)
        return;
    ev_head(&j, buf, sizeof buf, "data_fail", u->node, up_wall(u), s->path, slot, "rf", s->stream);
    rdof_json_int(&j, "src", s->src);
    rdof_json_int(&j, "dst", s->dst);
    rdof_json_str(&j, "reason", reason);
    up_event(u, &j);
}

/* The data call is in: checked, published (in parts past max_msg), answered. */
static void data_finish(rdof_mn_up_t *u, rdof_mn_upslot_t *s, int slot, double now)
{
    s->data = false;
    const uint8_t *data = s->buf;
    size_t len = s->len;
    const bool crc32 = s->dpf == RDOF_DPF_UNCONFIRMED || s->dpf == RDOF_DPF_CONFIRMED;
    if (s->bad) {
        data_fail_event(u, s, slot, "block");
        s->active = false;
        s->csbk = false;
        return;
    }
    if (crc32) {
        rdof_data_header_t h;
        uint8_t h12[12];
        memcpy(h12, s->headers[0], 10);
        h12[10] = h12[11] = 0;
        rdof_parse_data_header(h12, &h);
        const int pad = h.pad_octets;
        if (len < 4 || (int)len - 4 - pad < 0 || !rdof_check_crc32(s->buf, len)) {
            data_fail_event(u, s, slot, "crc");
            s->active = false;
            s->csbk = false;
            return;
        }
        len = len - 4 - (size_t)pad;
    }
    /* a repeat of a message answered within 5 s: answered again, not published */
    const uint32_t hash = fnv(data, len, fnv(s->headers[0], 10, 2166136261u));
    bool repeat = false;
    for (int k = 0; k < 4; k++)
        if (u->recent[k].hash == hash && u->recent[k].at > 0 && now - u->recent[k].at < 5.0)
            repeat = true;
    u->recent[u->recent_next].hash = hash;
    u->recent[u->recent_next].at = now;
    u->recent_next = (u->recent_next + 1) % 4;
    if (!repeat) {
        data_event(u, s, slot, data, len);
        if (s->sending) {
            const size_t cap = u->max_msg && u->max_msg < sizeof u->out ? u->max_msg : sizeof u->out;
            size_t at = 0;
            int part = 0;
            for (;;) {
                rdof_mn_writer_t w;
                up_begin(u, s, &w);
                if (part == 0 && s->csbk)
                    rdof_mn_csbk(&w, s->csbk10, s->csbk_count);
                const size_t room = w.len + 4 + 3 + 10 * (part == 0 ? (size_t)s->nheaders : 0) + 4;
                size_t chunk = len - at;
                if (room >= cap) {
                    u->too_big++;
                    break;
                }
                if (chunk > cap - room)
                    chunk = cap - room;
                const bool more = at + chunk < len;
                rdof_mn_data_pdu(&w, part, more, s->rate, part == 0 ? s->nheaders : 0, s->headers[0], data + at,
                                 chunk);
                if (!more)
                    rdof_mn_quality(&w, s->q_bits, s->q_n ? s->q_rssi_sum / s->q_n : 0);
                up_end(u, s, &w);
                at += chunk;
                if (!more || ++part > 127)
                    break;
            }
        }
    }
    if (u->data_done) {
        const rdof_mn_data_t d = {.slot = slot, .path = s->path, .src = s->src, .dst = s->dst,
                                  .group = (s->flags & RDOF_MN_F_GROUP) != 0, .confirmed = s->confirmed,
                                  .response_requested = s->response_requested, .dpf = s->dpf, .sap = s->sap,
                                  .ns = s->ns, .rate = s->rate, .data = data, .len = len, .sent = s->sending,
                                  .repeat = repeat};
        u->data_done(u->ctx, &d);
    }
    s->active = false;
    s->csbk = false;
}

static void on_data_header(rdof_mn_up_t *u, rdof_mn_upslot_t *s, const rdof_frame_t *f, double now)
{
    uint8_t d[12];
    rdof_data_header_t h;
    rdof_bptc196_decode(f->burst, d);
    if (!rdof_check_crc16(d, RDOF_MASK_DATA_HEADER))
        return;
    rdof_parse_data_header(d, &h);
    if (h.dpf == RDOF_DPF_PROPRIETARY && s->data) { /* the second header: one of the blocks announced */
        if (s->nheaders < RDOF_MN_DATA_HEADERS)
            memcpy(s->headers[s->nheaders++], d, 10);
        if (--s->blocks_left <= 0)
            data_finish(u, s, f->slot, now);
        return;
    }
    if (s->data) {
        data_fail_event(u, s, f->slot, "incomplete");
        s->data = false;
        s->active = false;
    }
    if (h.dpf == RDOF_DPF_RESPONSE) { /* a radio's answer to data: this box's business, not published */
        if (u->response)
            u->response(u->ctx, f->slot, h.src_id, h.dst_id, h.response_class, h.response_type, h.response_status);
        return;
    }
    const bool with_csbk = s->csbk && s->csbk_src == h.src_id && s->csbk_dst == h.dst_id && now - s->csbk_at < 2.0;
    if (!with_csbk) {
        csbk_flush(u, s, f->slot);
        if (s->active && s->voice)
            stream_end(u, s, f->slot, true);
        stream_start(u, s, f->slot, h.src_id, h.dst_id, h.group, 0, now);
    } else {
        s->active = true;
        s->dst = h.dst_id;
    }
    s->data = true;
    s->dpf = h.dpf;
    s->sap = h.sap;
    s->confirmed = h.dpf == RDOF_DPF_CONFIRMED;
    s->response_requested = h.response_requested;
    s->ns = h.ns < 0 ? 0 : h.ns;
    s->nheaders = 1;
    memcpy(s->headers[0], d, 10);
    s->blocks_left = h.blocks;
    s->rate = RDOF_MN_RATE_12;
    s->len = 0;
    s->bad = false;
    s->q_bits = f->errors;
    s->q_rssi_sum = f->rssi;
    s->q_n = f->rssi ? 1 : 0;
    s->last = now;
    if (s->blocks_left <= 0)
        data_finish(u, s, f->slot, now);
}

static void on_data_block(rdof_mn_up_t *u, rdof_mn_upslot_t *s, const rdof_frame_t *f, double now)
{
    if (!s->data)
        return;
    rdof_data_block_t b;
    if (!rdof_decode_data_block(f->burst, f->dt, &b) || !b.fec_ok)
        s->bad = true;
    s->rate = rdof_mn_rate_of_dt(f->dt);
    s->q_bits += b.errors > 0 ? b.errors : 0;
    if (f->rssi) {
        s->q_rssi_sum += f->rssi;
        s->q_n++;
    }
    const uint8_t *p = b.payload;
    int n = b.len;
    uint8_t body[24];
    if (s->confirmed) {
        int dbsn;
        if (!rdof_split_confirmed(&b, &dbsn, body, &n))
            s->bad = true;
        p = body;
    }
    if (n > 0 && s->len + (size_t)n <= RDOF_MN_DATA_MAX + 4) {
        memcpy(s->buf + s->len, p, (size_t)n);
        s->len += (size_t)n;
    } else if (n > 0) {
        s->bad = true;
    }
    s->last = now;
    if (--s->blocks_left <= 0)
        data_finish(u, s, f->slot, now);
}
#endif /* RDOF_FEATURE_DATA_BLOCKS */

static void raw_message(rdof_mn_up_t *u, rdof_mn_upslot_t *s, const rdof_frame_t *f)
{
    uint8_t v[1 + RDOF_BURST_BYTES];
    rdof_mn_writer_t w;
    v[0] = (uint8_t)f->dt;
    memcpy(v + 1, f->burst, RDOF_BURST_BYTES);
    up_begin(u, s, &w);
    rdof_mn_item(&w, RDOF_MN_RAW, v, sizeof v);
    up_end(u, s, &w);
}

void rdof_mn_up_frame(rdof_mn_up_t *u, const rdof_frame_t *f, double now)
{
    if (f->slot < 1 || f->slot > 2)
        return;
    rdof_mn_upslot_t *s = &u->slot[f->slot - 1];
    collect_frag(s, f);
    const bool voice = f->dt == RDOF_DT_VOICE_SYNC || f->dt == RDOF_DT_VOICE;
    if (!f->has_addr) {
        if (voice) { /* before the late entry's addresses: held */
            if (s->nheld == RDOF_MN_UP_HOLD) {
                memmove(&s->held[0], &s->held[1], sizeof s->held[0] * (RDOF_MN_UP_HOLD - 1));
                s->nheld--;
            }
            s->held[s->nheld++] = *f;
            s->held_at = now;
        }
        return;
    }
    switch (f->dt) {
    case RDOF_DT_VOICE_LC_HEADER: {
        if (!f->new_stream && s->active && s->voice)
            return; /* a repeat of the header */
        uint8_t lc9[9];
        bool ok;
        lc9_of(f, lc9, &ok);
        csbk_flush(u, s, f->slot);
        if (s->active && s->voice)
            stream_end(u, s, f->slot, true);
        rdof_full_lc_t lc;
        rdof_parse_full_lc(lc9, &lc);
        uint8_t extra = (lc.service_options & RDOF_SO_EMERGENCY) ? RDOF_MN_F_EMERGENCY : 0;
        if (f->synthetic)
            extra |= RDOF_MN_F_LATE;
        stream_start(u, s, f->slot, f->src, f->dst, f->group, extra, now);
        s->voice = true;
        s->data = false;
        const int rssi = f->synthetic && s->nheld ? s->held[0].rssi : f->rssi;
        call_start_event(u, s, f->slot, rssi);
        if (!f->synthetic) {
            rdof_mn_writer_t w;
            up_begin(u, s, &w);
            rdof_mn_lc(&w, RDOF_MN_LC_HEADER, lc9);
            rdof_mn_quality(&w, f->errors, f->rssi);
            up_end(u, s, &w);
            s->nheld = 0;
        } else {
            /* late entry: the held voice, the LC that gave the addresses first */
            for (int k = 0; k < s->nheld; k++) {
                rdof_frame_t h = s->held[k];
                h.has_addr = true, h.src = f->src, h.dst = f->dst, h.group = f->group;
                voice_message(u, s, &h, k == 0 ? lc9 : NULL);
            }
            if (!s->nheld) { /* nothing held: the LC on its own */
                rdof_mn_writer_t w;
                up_begin(u, s, &w);
                rdof_mn_lc(&w, RDOF_MN_EMBEDDED_LC, lc9);
                up_end(u, s, &w);
            }
            s->nheld = 0;
        }
        return;
    }
    case RDOF_DT_TERMINATOR_WITH_LC: {
        if (!s->active || !s->voice)
            return;
        uint8_t lc9[9];
        bool ok;
        lc9_of(f, lc9, &ok);
        rdof_mn_writer_t w;
        s->last = now;
        up_begin(u, s, &w);
        rdof_mn_lc(&w, RDOF_MN_TERMINATOR, lc9);
        if (!f->synthetic)
            rdof_mn_quality(&w, f->errors, f->rssi);
        up_end(u, s, &w);
        stream_end(u, s, f->slot, f->synthetic);
        return;
    }
    case RDOF_DT_VOICE_SYNC:
    case RDOF_DT_VOICE: {
        if (!s->active || !s->voice)
            return;
        s->last = now;
        voice_message(u, s, f, NULL);
        return;
    }
    case RDOF_DT_CSBK: on_csbk(u, s, f, now); return;
#if RDOF_FEATURE_DATA_BLOCKS
    case RDOF_DT_DATA_HEADER: on_data_header(u, s, f, now); return;
    case RDOF_DT_RATE_12:
    case RDOF_DT_RATE_34:
    case RDOF_DT_RATE_1: on_data_block(u, s, f, now); return;
#endif
    case RDOF_DT_PI_HEADER: {
        if (!s->active || !s->voice)
            return;
        uint8_t d[12];
        rdof_bptc196_decode(f->burst, d);
        if (!rdof_check_crc16(d, RDOF_MASK_PI_HEADER))
            return;
        s->flags |= RDOF_MN_F_PRIVACY;
        rdof_mn_writer_t w;
        up_begin(u, s, &w);
        rdof_mn_item(&w, RDOF_MN_PI_HEADER, d, 10);
        rdof_mn_quality(&w, f->errors, f->rssi);
        up_end(u, s, &w);
        return;
    }
    default:
        if (s->active)
            raw_message(u, s, f);
        return;
    }
}

void rdof_mn_up_tick(rdof_mn_up_t *u, double now)
{
    for (int i = 0; i < 2; i++) {
        rdof_mn_upslot_t *s = &u->slot[i];
        if (s->csbk && !s->data && now - s->csbk_at > 1.0)
            csbk_flush(u, s, i + 1);
        if (s->csbk && now - s->csbk_at > 3.0)
            s->csbk = false;
#if RDOF_FEATURE_DATA_BLOCKS
        if (s->data && now - s->last > RDOF_DATA_LOST_SECONDS) {
            data_fail_event(u, s, i + 1, "incomplete");
            s->data = false;
            s->active = false;
        }
#endif
        if (s->active && s->voice && now - s->last > 2.0) /* the tracker sends a terminator long before */
            stream_end(u, s, i + 1, true);
        if (s->nheld && now - s->held_at > 0.5) /* no late entry came: the held voice goes */
            s->nheld = 0;
    }
}

/* ── data calls rebuilt ──────────────────────────────────────────────────── */

static void put_crc16(uint8_t d[12], uint16_t mask)
{
    const uint16_t c = rdof_crc16_ccitt(d, 10) ^ mask;
    d[10] = (uint8_t)(c >> 8);
    d[11] = (uint8_t)c;
}

#if RDOF_FEATURE_DATA_BLOCKS
static void block_burst(int rate, const uint8_t *payload, uint64_t sync48, uint8_t out[RDOF_BURST_BYTES])
{
    uint8_t info[196];
    if (rate == RDOF_MN_RATE_12) {
        rdof_data_burst(payload, RDOF_DT_RATE_12, 0, sync48, out);
        return;
    }
    if (rate == RDOF_MN_RATE_34)
        rdof_trellis34_encode(payload, info);
    else
        rdof_rate1_encode(payload, info);
    rdof_info_burst(info, rdof_mn_dt_of_rate(rate), 0, sync48, out);
}

int rdof_mn_data_bursts(int rate, int nheaders, const uint8_t *headers10, const uint8_t *data, size_t len,
                        uint64_t sync48, uint8_t controls[RDOF_MN_DATA_BURSTS],
                        uint8_t bursts[RDOF_MN_DATA_BURSTS][RDOF_BURST_BYTES])
{
    static const int per[3] = {12, 18, 24};
    if (rate < 0 || rate > 2 || nheaders < 1 || nheaders > RDOF_MN_DATA_HEADERS || len > RDOF_MN_DATA_MAX)
        return 0;
    uint8_t h[12];
    memcpy(h, headers10, 10);
    const int dpf = h[0] & 0x0F;
    const bool confirmed = dpf == RDOF_DPF_CONFIRMED, crc32 = confirmed || dpf == RDOF_DPF_UNCONFIRMED;
    const int body = per[rate] - (confirmed ? 2 : 0);
    static uint8_t buf[RDOF_MN_DATA_MAX + 64]; /* one caller at a time (the downlink's thread) */
    size_t total = len + (crc32 ? 4 : 0);
    const int blocks = (int)((total + (size_t)body - 1) / (size_t)body);
    if (blocks + nheaders - 1 > 127 || blocks + nheaders > RDOF_MN_DATA_BURSTS)
        return 0;
    const size_t pad = (size_t)blocks * (size_t)body - total;
    memcpy(buf, data, len);
    memset(buf + len, 0, pad + 4);
    if (crc32) {
        const uint32_t crc = rdof_crc32_dmr(buf, len + pad);
        for (int i = 0; i < 4; i++)
            buf[len + pad + (size_t)i] = (uint8_t)(crc >> (8 * i));
        h[0] = (uint8_t)((h[0] & ~0x10) | ((pad & 0x10) ? 0x10 : 0));
        h[1] = (uint8_t)((h[1] & 0xF0) | (pad & 0x0F));
    }
    h[8] = (uint8_t)((h[8] & 0x80) | ((blocks + nheaders - 1) & 0x7F));
    put_crc16(h, RDOF_MASK_DATA_HEADER);
    int n = 0;
    rdof_data_burst(h, RDOF_DT_DATA_HEADER, 0, sync48, bursts[n]);
    controls[n++] = RDOF_SYNC_DATA | RDOF_DT_DATA_HEADER;
    for (int k = 1; k < nheaders; k++) {
        memcpy(h, headers10 + 10 * k, 10);
        put_crc16(h, RDOF_MASK_DATA_HEADER);
        rdof_data_burst(h, RDOF_DT_DATA_HEADER, 0, sync48, bursts[n]);
        controls[n++] = RDOF_SYNC_DATA | RDOF_DT_DATA_HEADER;
    }
    static const int masks[3] = {RDOF_MASK_CRC9_RATE12, RDOF_MASK_CRC9_RATE34, RDOF_MASK_CRC9_RATE1};
    for (int b = 0; b < blocks; b++) {
        uint8_t p[24] = {0};
        const uint8_t *src = buf + (size_t)b * (size_t)body;
        if (confirmed) {
            uint8_t bits[8 * 22 + 7];
            rdof_bytes_to_bits(src, (size_t)body, bits);
            rdof_u64_to_bits((uint64_t)(b & 0x7F), 7, bits + 8 * body);
            const int crc9 = (int)(rdof_crc9_dmr(bits, (size_t)(8 * body + 7)) ^ masks[rate]);
            rdof_set_field(p, 0, 7, (uint64_t)(b & 0x7F));
            rdof_set_field(p, 7, 9, (uint64_t)crc9);
            memcpy(p + 2, src, (size_t)body);
        } else {
            memcpy(p, src, (size_t)body);
        }
        block_burst(rate, p, sync48, bursts[n]);
        controls[n++] = (uint8_t)(RDOF_SYNC_DATA | rdof_mn_dt_of_rate(rate));
    }
    return n;
}
#endif

/* ── the downlink ────────────────────────────────────────────────────────── */

void rdof_mn_down_init(rdof_mn_down_t *d)
{
    memset(d, 0, sizeof *d);
    d->max_preambles = 8;
}

const char *rdof_mn_down_from(const rdof_mn_down_t *d, uint32_t stream)
{
    for (int k = 0; k < RDOF_MN_DOWN_STREAMS; k++)
        if (d->s[k].stream == stream && d->s[k].node[0])
            return d->s[k].node;
    return NULL;
}

static uint64_t bs_data_sync(void) { return rdof_data_sync(RDOF_BS_VOICE); }

static void emit(rdof_mn_down_t *d, rdof_mn_dstream_t *s, uint8_t control, const uint8_t burst[RDOF_BURST_BYTES],
                 bool terminator, bool dl)
{
    rdof_net_burst_t b;
    memset(&b, 0, sizeof b);
    rdof_put_be32(b.stream_id, s->stream);
    b.control = control;
    memcpy(b.burst, burst, RDOF_BURST_BYTES);
    b.terminator = terminator;
    b.seq = s->burst_seq++;
    b.src = s->h.src;
    b.dst = s->h.dst;
    b.group = (s->h.flags & RDOF_MN_F_GROUP) != 0;
    b.slot = (s->h.flags & RDOF_MN_F_SLOT2) ? 2 : 1;
    if (d->burst)
        d->burst(d->ctx, &b, s->h.path, dl, s->h.flags, s->node);
}

static void emit_lc(rdof_mn_down_t *d, rdof_mn_dstream_t *s, int dt, bool dl)
{
    uint8_t burst[RDOF_BURST_BYTES];
    rdof_lc_burst(s->lc, dt, 0, bs_data_sync(), burst);
    emit(d, s, (uint8_t)(RDOF_SYNC_DATA | dt), burst, dt == RDOF_DT_TERMINATOR_WITH_LC, dl);
    if (dt == RDOF_DT_TERMINATOR_WITH_LC)
        s->ended = true;
}

static void default_lc(rdof_mn_dstream_t *s)
{
    if (s->has_lc)
        return;
    rdof_group_lc(s->h.src, s->h.dst, (s->h.flags & RDOF_MN_F_GROUP) != 0,
                  (s->h.flags & RDOF_MN_F_EMERGENCY) ? RDOF_SO_EMERGENCY : 0, s->lc);
    memcpy(s->emb, s->lc, 9);
    s->has_lc = true;
}

static void down_voice(rdof_mn_down_t *d, rdof_mn_dstream_t *s, const uint8_t *v, int gap_items, bool dl)
{
    const int n = v[0];
    if (!s->voice) {
        default_lc(s);
        emit_lc(d, s, RDOF_DT_VOICE_LC_HEADER, dl); /* a late entry, or a lost header: one from the LC known */
        s->voice = true;
        s->next_voice = n;
    }
    /* bursts lost before this one: the voice index's gap, plus whole superframes the items say */
    int lost = (n - s->next_voice + 6) % 6;
    if (gap_items > 0 && s->items_per_burst > 0) {
        const int est = gap_items * 16 / s->items_per_burst;
        while (lost + 6 <= est)
            lost += 6;
    }
    s->burst_seq = (uint8_t)(s->burst_seq + lost);
    uint8_t voice[216], middle[48], burst[RDOF_BURST_BYTES];
    rdof_bytes_to_bits(v + 1, 27, voice);
    const int pi = (s->h.flags & RDOF_MN_F_PRIVACY) ? 1 : 0;
    if (n == 0) {
        rdof_u64_to_bits(RDOF_BS_VOICE, 48, middle);
    } else {
        uint8_t raw[128], zero[32] = {0};
        rdof_embedded_lc_encode(s->emb, raw);
        rdof_voice_middle(0, LCSS_OF[n], n <= 4 ? raw + 32 * (n - 1) : zero, pi, middle);
    }
    rdof_voice_burst(voice, middle, burst);
    emit(d, s, n == 0 ? RDOF_SYNC_AUDIO : (uint8_t)n, burst, false, dl);
    s->next_voice = (n + 1) % 6;
}

static void down_csbk(rdof_mn_down_t *d, rdof_mn_dstream_t *s, const uint8_t *v, bool before_data, bool dl)
{
    uint8_t c[12], burst[RDOF_BURST_BYTES];
    int count = before_data && is_preamble(v) ? v[10] : 1;
    if (count > d->max_preambles)
        count = d->max_preambles;
    if (count < 1)
        count = 1;
    for (int i = 0; i < count; i++) {
        memcpy(c, v, 10);
        if (before_data && is_preamble(v))
            c[3] = (uint8_t)(v[3] + (count - 1 - i)); /* blocks to follow, counting down to the item's */
        put_crc16(c, RDOF_MASK_CSBK);
        rdof_data_burst(c, RDOF_DT_CSBK, 0, bs_data_sync(), burst);
        emit(d, s, RDOF_SYNC_DATA | RDOF_DT_CSBK, burst, !before_data && !is_preamble(v), dl);
    }
}

#if RDOF_FEATURE_DATA_BLOCKS
static void down_data(rdof_mn_down_t *d, rdof_mn_dstream_t *s, int rate, int nh, const uint8_t *headers,
                      const uint8_t *data, size_t len, bool dl)
{
    static uint8_t controls[RDOF_MN_DATA_BURSTS];
    static uint8_t bursts[RDOF_MN_DATA_BURSTS][RDOF_BURST_BYTES];
    const int n = rdof_mn_data_bursts(rate, nh, headers, data, len, bs_data_sync(), controls, bursts);
    if (!n) {
        d->bad++;
        return;
    }
    if (s->has_pending_csbk) {
        down_csbk(d, s, s->pending_csbk, true, dl);
        s->has_pending_csbk = false;
    }
    for (int k = 0; k < n; k++)
        emit(d, s, controls[k], bursts[k], false, dl);
    s->ended = true;
    if (d->data)
        d->data(d->ctx, &s->h, nh > 0 && (headers[0] & 0x0F) == RDOF_DPF_CONFIRMED, s->node);
}
#endif

static rdof_mn_dstream_t *find_stream(rdof_mn_down_t *d, const char *node, uint32_t stream, bool *fresh)
{
    int k, free_k = -1, old = 0;
    *fresh = false;
    for (k = 0; k < RDOF_MN_DOWN_STREAMS; k++) {
        rdof_mn_dstream_t *s = &d->s[k];
        if (s->used && s->stream == stream && !strcmp(s->node, node))
            return s;
        if (!s->used && free_k < 0)
            free_k = k;
        if (d->s[k].last < d->s[old].last)
            old = k;
    }
    if (free_k < 0) {
        free_k = old;
        d->replaced++;
    }
    rdof_mn_dstream_t *s = &d->s[free_k];
    memset(s, 0, offsetof(rdof_mn_dstream_t, buf));
    s->used = true;
    snprintf(s->node, sizeof s->node, "%s", node);
    s->stream = stream;
    *fresh = true;
    d->streams++;
    return s;
}

bool rdof_mn_down_message(rdof_mn_down_t *d, const char *node, bool dl, const uint8_t *msg, size_t len, double now)
{
    rdof_mn_hdr_t h;
    if (!rdof_mn_parse(msg, len, &h) || rdof_mn_count(msg, len) < 1) { /* one or more items (section 4) */
        d->bad++;
        return false;
    }
    d->messages++;
    bool fresh;
    rdof_mn_dstream_t *s = find_stream(d, node, h.stream, &fresh);
    int gap = 0;
    if (fresh) {
        s->h = h;
        s->next_seq = h.seq;
    } else {
        const int diff = (uint8_t)(h.seq - s->next_seq);
        if (diff >= 128) { /* older than what came: late, on a transport that reorders */
            d->late++;
            return true;
        }
        if (diff) {
            d->gaps++;
            gap = diff;
        }
        /* flags may gain privacy and the like mid-stream */
        s->h.flags = h.flags;
        s->h.path = h.path;
    }
    s->last = now;
    const int count = rdof_mn_count(msg, len);
    s->next_seq = (uint8_t)(h.seq + count);
    if (s->ended)
        return true;
    /* the stream's items per voice burst (x16), from messages that came whole */
    int nvoice = 0;
    rdof_mn_reader_t r;
    rdof_mn_item_t it;
    rdof_mn_reader(&r, msg, len);
    while (rdof_mn_next(&r, &it))
        nvoice += it.type == RDOF_MN_VOICE;
    if (nvoice && !gap) {
        const int ipb = count * 16 / nvoice;
        s->items_per_burst = s->items_per_burst ? (s->items_per_burst * 3 + ipb) / 4 : ipb;
    }
    bool has_data = false;
    rdof_mn_reader(&r, msg, len);
    while (rdof_mn_next(&r, &it))
        has_data |= it.type == RDOF_MN_DATA_PDU;
    rdof_mn_reader(&r, msg, len);
    while (rdof_mn_next(&r, &it) && !s->ended) {
        if (!rdof_mn_item_ok(&it))
            continue;
        switch (it.type) {
        case RDOF_MN_LC_HEADER:
            memcpy(s->lc, it.value, 9);
            memcpy(s->emb, it.value, 9);
            s->has_lc = true;
            if (!s->voice) {
                emit_lc(d, s, RDOF_DT_VOICE_LC_HEADER, dl);
                s->voice = true;
                s->next_voice = 0;
            }
            break;
        case RDOF_MN_EMBEDDED_LC: {
            const int flco = it.value[0] & 0x3F;
            if (!s->has_lc && (flco == RDOF_FLCO_GROUP || flco == RDOF_FLCO_PRIVATE)) {
                memcpy(s->lc, it.value, 9);
                s->has_lc = true;
            }
            memcpy(s->emb, it.value, 9);
            break;
        }
        case RDOF_MN_VOICE: down_voice(d, s, it.value, gap, dl); gap = 0; break;
        case RDOF_MN_TERMINATOR:
            memcpy(s->lc, it.value, 9);
            s->has_lc = true;
            emit_lc(d, s, RDOF_DT_TERMINATOR_WITH_LC, dl);
            break;
        case RDOF_MN_PI_HEADER: {
            uint8_t c[12], burst[RDOF_BURST_BYTES];
            memcpy(c, it.value, 10);
            put_crc16(c, RDOF_MASK_PI_HEADER);
            rdof_data_burst(c, RDOF_DT_PI_HEADER, 0, bs_data_sync(), burst);
            emit(d, s, RDOF_SYNC_DATA | RDOF_DT_PI_HEADER, burst, false, dl);
            break;
        }
        case RDOF_MN_CSBK:
            if (has_data) {
                memcpy(s->pending_csbk, it.value, 11);
                s->has_pending_csbk = true;
            } else {
                down_csbk(d, s, it.value, false, dl);
                if (!is_preamble(it.value))
                    s->ended = true;
            }
            break;
#if RDOF_FEATURE_DATA_BLOCKS
        case RDOF_MN_DATA_PDU: {
            rdof_mn_pdu_t p;
            rdof_mn_pdu(&it, &p);
            if (p.part == 0 && !p.more) {
                down_data(d, s, p.rate, p.nheaders, p.headers, p.data, p.len, dl);
                break;
            }
            if (p.part == 0) {
                s->parts = true;
                s->next_part = 0;
                s->rate = p.rate;
                s->nheaders = p.nheaders;
                memcpy(s->headers, p.headers, 10 * (size_t)p.nheaders);
                s->len = 0;
            }
            if (!s->parts || p.part != s->next_part || s->len + p.len > sizeof s->buf) {
                if (s->parts)
                    d->parts_lost++;
                s->parts = false;
                s->ended = true; /* the call is dropped */
                break;
            }
            memcpy(s->buf + s->len, p.data, p.len);
            s->len += p.len;
            s->next_part++;
            if (!p.more) {
                s->parts = false;
                down_data(d, s, s->rate, s->nheaders, &s->headers[0][0], s->buf, s->len, dl);
            }
            break;
        }
#endif
        case RDOF_MN_RAW:
            if (d->raw_tx)
                emit(d, s, (uint8_t)(RDOF_SYNC_DATA | (it.value[0] & 0x0F)), it.value + 1, false, dl);
            else
                d->raw_dropped++;
            break;
        default: break; /* talker_alias, gps, quality, origin: for apps; unknown types: skipped */
        }
    }
    return true;
}

void rdof_mn_down_tick(rdof_mn_down_t *d, double now)
{
    for (int k = 0; k < RDOF_MN_DOWN_STREAMS; k++) {
        rdof_mn_dstream_t *s = &d->s[k];
        if (!s->used || now - s->last < RDOF_MN_DOWN_IDLE_S)
            continue;
        if (s->voice && !s->ended) {
            default_lc(s);
            emit_lc(d, s, RDOF_DT_TERMINATOR_WITH_LC, false);
        }
        if (s->parts)
            d->parts_lost++;
        s->used = false; /* the node and stream stay for rdof_mn_down_from */
    }
}

/* ── the node ────────────────────────────────────────────────────────────── */

static bool path_on(const rdof_site_t *s, int k, int net) { return s->path[k].pol.net == net; }

uint16_t rdof_mn_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)(data[i] << 8);
        for (int b = 0; b < 8; b++)
            crc = (uint16_t)(crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    return crc;
}

uint16_t rdof_mn_paths_rev(const rdof_site_t *s, int net)
{
    uint8_t buf[RDOF_SITE_MAX_PATHS * (RDOF_SITE_NAME + 6)];
    size_t n = 0;
    for (int k = 0; k < s->npath; k++) {
        if (!path_on(s, k, net))
            continue;
        buf[n++] = (uint8_t)k;
        const size_t l = strlen(s->path[k].name);
        memcpy(buf + n, s->path[k].name, l + 1);
        n += l + 1;
    }
    return rdof_mn_crc16(buf, n);
}

static void mhz(rdof_json_t *j, const char *key, uint32_t hz)
{
    char t[24];
    snprintf(t, sizeof t, "%.6f", hz / 1e6);
    size_t l = strlen(t);
    while (l > 1 && t[l - 1] == '0' && t[l - 2] != '.')
        t[--l] = 0;
    rdof_json_raw(j, key, t);
}

size_t rdof_mn_info_json(const rdof_site_t *s, int net, const rdof_mn_info_t *in, char *out, size_t cap)
{
    rdof_json_t j;
    char t[256];
    rdof_json_init(&j, out, cap);
    rdof_json_open(&j, NULL);
    rdof_json_int(&j, "schema", 1);
    rdof_json_str(&j, "node", in->node);
    rdof_json_str(&j, "kind", "box");
    rdof_json_str(&j, "name", in->name ? in->name : in->node);
    rdof_json_str(&j, "firmware", in->firmware ? in->firmware : "");
    if (in->dmr_id)
        rdof_json_int(&j, "dmr_id", in->dmr_id);
    rdof_json_str(&j, "transport", in->transport ? in->transport : "mqtt/tcp");
    rdof_json_int(&j, "bundle", in->bundle > 0 ? in->bundle : 1);
    if (in->has_location) {
        rdof_json_open(&j, "location");
        snprintf(t, sizeof t, "%.5f", in->lat);
        rdof_json_raw(&j, "lat", t);
        snprintf(t, sizeof t, "%.5f", in->lon);
        rdof_json_raw(&j, "lon", t);
        rdof_json_str(&j, "source", in->location_source ? in->location_source : "config");
        rdof_json_close(&j);
    }
    rdof_json_int(&j, "paths_rev", rdof_mn_paths_rev(s, net));
    rdof_json_raw(&j, "paths", "["); /* an array by hand: its items follow, then "]" */
    {
        bool first = true;
        for (int k = 0; k < s->npath; k++) {
            if (!path_on(s, k, net))
                continue;
            const rdof_path_t *p = &s->path[k];
            const rdof_freq_t *rx = &s->freq[p->freq];
            char item[1024];
            rdof_json_t q;
            rdof_json_init(&q, item, sizeof item);
            rdof_json_open(&q, NULL);
            rdof_json_int(&q, "i", k);
            rdof_json_str(&q, "name", p->name);
            if (p->app >= 0)
                rdof_json_str(&q, "app", "repeater");
            mhz(&q, "rx", rx->hz);
            if (p->tx_freq >= 0 && p->tx_freq != p->freq)
                mhz(&q, "tx", s->freq[p->tx_freq].hz);
            if (rx->cc >= 0)
                rdof_json_int(&q, "cc", rx->cc);
            else
                rdof_json_raw(&q, "cc", "null");
            const bool tx_ok = p->tx_freq >= 0 && (p->pol.dir & RDOF_FLOW_NET_RF) && s->freq[p->tx_freq].tx;
            if (p->slot)
                rdof_json_int(&q, "ts", p->slot);
            else if (tx_ok && p->tx_slot)
                rdof_json_int(&q, "ts", p->tx_slot);
            else
                rdof_json_raw(&q, "ts", "null");
            rdof_json_bool(&q, "tx_ok", tx_ok);
            rdof_idset_format(&p->pol.tg, t, sizeof t);
            rdof_json_str(&q, "tg", t);
            rdof_idset_format(&p->pol.ptt, t, sizeof t);
            rdof_json_str(&q, "ptt", t);
            rdof_idset_format(&p->pol.pc, t, sizeof t);
            rdof_json_str(&q, "pc", t);
            rdof_json_close(&q);
            if (!rdof_json_done(&q)) {
                j.overflow = true;
                break;
            }
            if (j.len + q.len + 3 > j.cap) {
                j.overflow = true;
                break;
            }
            if (!first)
                j.buf[j.len++] = ',';
            memcpy(j.buf + j.len, item, q.len);
            j.len += q.len;
            j.buf[j.len] = 0;
            first = false;
        }
        if (j.len + 2 > j.cap)
            j.overflow = true;
        else {
            j.buf[j.len++] = ']';
            j.buf[j.len] = 0;
        }
    }
    /* claims: the box's own apps' IDs (single IDs, and the ranges as they're written) */
    {
        char claims[512];
        size_t n = 0;
        claims[n++] = '[';
        for (int a = 0; a < s->napp; a++) {
            const rdof_app3_t *ap = &s->app[a];
            if (ap->type == RDOF_APP_REPEATER)
                continue;
            for (int g = 0; g < 2; g++) {
                rdof_idrange_t rr[RDOF_IDSET_MAX];
                const int m = rdof_idset_ranges(g ? &ap->tg : &ap->pc, rr, RDOF_IDSET_MAX);
                for (int k = 0; k < m && k < RDOF_IDSET_MAX; k++) {
                    if (rr[k].lo == 0 && rr[k].hi >= RDOF_ID_MAX)
                        continue; /* "*": not a claim */
                    for (uint32_t id = rr[k].lo; id <= rr[k].hi && id - rr[k].lo < 16 && n + 12 < sizeof claims; id++)
                        n += (size_t)snprintf(claims + n, sizeof claims - n, "%s%lu", n > 1 ? "," : "",
                                              (unsigned long)id);
                }
            }
        }
        claims[n++] = ']';
        claims[n] = 0;
        /* a claim listed twice (an app's tg and pc) once */
        rdof_json_raw(&j, "claims", claims);
    }
    rdof_json_close(&j);
    return rdof_json_done(&j) ? j.len : 0;
}

static bool sub_add(uint32_t *tgs, int *n, int max, uint32_t tg)
{
    for (int q = 0; q < *n; q++)
        if (tgs[q] == tg)
            return true;
    if (*n >= max)
        return false;
    tgs[(*n)++] = tg;
    return true;
}

int rdof_mn_sub_tgs(const rdof_site_t *s, int net, const struct rdof_router *r, double now, uint32_t *tgs, int max,
                    bool *all)
{
    int n = 0;
    *all = false;
    for (int k = 0; k < s->npath && !*all; k++) {
        const rdof_path_t *p = &s->path[k];
        if (p->pol.net != net || p->tx_freq < 0 || !(p->pol.dir & RDOF_FLOW_NET_RF))
            continue;
        /* the tg set: each TG, or everything */
        const rdof_idset_t *set = &p->pol.tg;
        if (set->star) {
            *all = true;
            break;
        }
        rdof_idrange_t rr[RDOF_IDSET_MAX];
        const int m = rdof_idset_ranges(set, rr, RDOF_IDSET_MAX);
        uint32_t count = 0;
        for (int i = 0; i < m && i < RDOF_IDSET_MAX; i++)
            count += rr[i].hi - rr[i].lo + 1;
        if (count > RDOF_MN_SUBS_EXPAND) {
            *all = true;
            break;
        }
        for (int i = 0; i < m && i < RDOF_IDSET_MAX && !*all; i++)
            for (uint32_t tg = rr[i].lo; tg <= rr[i].hi && !*all; tg++)
                *all = !sub_add(tgs, &n, max, tg);
        /* the ptt set: the TGs an RF user holds now */
        if (!r || *all)
            continue;
        for (int i = 0; i < RDOF_ROUTER_PTT && !*all; i++)
            if (r->ptt[k][i].tg && r->ptt[k][i].until > now)
                *all = !sub_add(tgs, &n, max, r->ptt[k][i].tg);
    }
    return n;
}
