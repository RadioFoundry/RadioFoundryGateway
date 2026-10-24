// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Last heard: the ring of recent calls (rdof/lastheard.h).
 */
#include "rdof/lastheard.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "rdof/json.h"

static void copy(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src ? src : "");
}

void rdof_lh_init(rdof_lh_t *lh, rdof_lh_entry_t *buf, int cap)
{
    memset(lh, 0, sizeof *lh);
    lh->e = buf;
    lh->cap = buf && cap > 0 ? cap : 0;
}

/* The i-th newest (0 = the newest). */
static rdof_lh_entry_t *nth(const rdof_lh_t *lh, int i)
{
    return &lh->e[((lh->head - 1 - i) % lh->cap + lh->cap) % lh->cap];
}

static void touch(rdof_lh_t *lh, rdof_lh_entry_t *e)
{
    e->seq = ++lh->seq;
}

static rdof_lh_entry_t *find_active(const rdof_lh_t *lh, rdof_lh_dir_t dir, const char *radio, const char *channel,
                                    int slot)
{
    for (int i = 0; i < lh->n; i++) {
        rdof_lh_entry_t *e = nth(lh, i);
        if (e->active && e->dir == dir && e->slot == slot && !strncmp(e->radio, radio, sizeof e->radio - 1) &&
            !strncmp(e->channel, channel, sizeof e->channel - 1))
            return e;
    }
    return NULL;
}

static void finish(rdof_lh_t *lh, rdof_lh_entry_t *e, const char *reason, double duration, double now)
{
    e->active = false;
    copy(e->end, sizeof e->end, reason);
    if (duration < 0 || isnan(duration))
        duration = now - e->start;
    e->duration = duration > 0 ? duration : 0;
    touch(lh, e);
}

/* A new entry. A call: any still active on that radio, channel and slot ends as lost. */
static rdof_lh_entry_t *add(rdof_lh_t *lh, rdof_lh_dir_t dir, const char *role, const char *radio,
                            const char *channel, int slot, bool call, double now, int64_t now_ms)
{
    if (!lh->cap)
        return NULL;
    rdof_lh_entry_t *old = call ? find_active(lh, dir, radio, channel, slot) : NULL;
    if (old)
        finish(lh, old, "lost", -1, now);
    rdof_lh_entry_t *e = &lh->e[lh->head];
    lh->head = (lh->head + 1) % lh->cap;
    if (lh->n < lh->cap)
        lh->n++;
    memset(e, 0, sizeof *e);
    e->dir = dir;
    copy(e->role, sizeof e->role, role);
    copy(e->radio, sizeof e->radio, radio);
    copy(e->channel, sizeof e->channel, channel);
    e->slot = slot;
    e->cc = -1;
    e->frames = -1;
    e->start = now;
    e->start_ms = now_ms > 0 ? now_ms : 0;
    e->active = call;
    touch(lh, e);
    e->id = e->seq;
    return e;
}

/* ── RF: the tracker's JSON ───────────────────────────────────────────────── */

typedef struct {
    rdof_jdoc_t doc;
    int obj;
} jctx_t;

static const rdof_jnode_t *field(const jctx_t *j, const char *key)
{
    return rdof_jnode(&j->doc, rdof_jget(&j->doc, j->obj, key));
}

static bool num(const jctx_t *j, const char *key, double *v)
{
    const rdof_jnode_t *n = field(j, key);
    if (!n || n->type != RDOF_JNUM)
        return false;
    *v = n->num;
    return true;
}

static const char *str(const jctx_t *j, const char *key)
{
    const rdof_jnode_t *n = field(j, key);
    return n && n->type == RDOF_JSTR ? n->str : NULL;
}

static int16_t clamp16(double v)
{
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

static bool rf_start(rdof_lh_t *lh, const jctx_t *j, const char *role, const char *radio, const char *channel,
                     int slot, bool late, double now, int64_t now_ms)
{
    double v;
    uint32_t src = num(j, "src_id", &v) ? (uint32_t)v : 0, dst = num(j, "dst_id", &v) ? (uint32_t)v : 0;
    const char *g = str(j, "group");
    bool group = g && !strcmp(g, "yes");
    rdof_lh_entry_t *e = find_active(lh, RDOF_LH_RF, radio, channel, slot);
    if (late && e && e->src == src && e->dst == dst && e->group == group)
        return false; /* the same call, picked up again */
    e = add(lh, RDOF_LH_RF, role, radio, channel, slot, true, now, now_ms);
    if (!e)
        return false;
    e->src = src;
    e->dst = dst;
    e->group = group;
    e->late = late;
    const char *f = str(j, "frequency");
    if (f) {
        double mhz = 0;
        if (sscanf(f, "%lf", &mhz) == 1 && mhz > 0 && mhz < 4295)
            e->freq = (uint32_t)llround(mhz * 1e6);
    }
    if (num(j, "colorCode", &v))
        e->cc = (int)v;
    if (num(j, "frames", &v)) {
        e->kind = RDOF_LH_DATA;
        e->frames = (int)v;
    }
    const rdof_jnode_t *em = field(j, "emergency");
    e->emergency = em && em->type == RDOF_JBOOL && em->b;
    return true;
}

static bool rf_end(rdof_lh_t *lh, const jctx_t *j, const char *radio, const char *channel, int slot,
                   const char *action, double now)
{
    rdof_lh_entry_t *e = find_active(lh, RDOF_LH_RF, radio, channel, slot);
    if (!e)
        return false;
    double v, dur = -1;
    if (num(j, "duration", &v))
        dur = v;
    if (num(j, "ber", &v)) {
        e->has_ber = true;
        e->ber = (float)v;
    }
    int r = rdof_jget(&j->doc, j->obj, "rssi");
    if (r >= 0) {
        jctx_t k = {j->doc, r};
        if (num(&k, "ave", &v)) {
            e->has_rssi = true;
            e->rssi = clamp16(v);
            if (num(&k, "min", &v))
                e->rssi_min = clamp16(v);
            if (num(&k, "max", &v))
                e->rssi_max = clamp16(v);
        }
    }
    finish(lh, e, action, dur, now);
    return true;
}

static bool rf_csbk(rdof_lh_t *lh, const jctx_t *j, const char *role, const char *radio, const char *channel,
                    int slot, double now, int64_t now_ms)
{
    const char *desc = str(j, "csbk_desc");
    if (!desc || !strcmp(desc, "Preamble")) /* a data call's preambles: the call itself shows */
        return false;
    double v;
    uint32_t src = num(j, "src_id", &v) ? (uint32_t)v : 0, dst = num(j, "dst_id", &v) ? (uint32_t)v : 0;
    const char *g = str(j, "group");
    /* a CSBK doesn't end a call: it's an entry of its own, never active */
    rdof_lh_entry_t *e = add(lh, RDOF_LH_RF, role, radio, channel, slot, false, now, now_ms);
    if (!e)
        return false;
    e->kind = RDOF_LH_CSBK;
    e->src = src;
    e->dst = dst;
    e->group = g && !strcmp(g, "yes");
    copy(e->desc, sizeof e->desc, desc);
    return true;
}

bool rdof_lh_tracker_json(rdof_lh_t *lh, const char *role, const char *radio, const char *channel, const char *json,
                          double now, int64_t now_ms)
{
    if (!lh->cap || !json)
        return false;
    radio = radio ? radio : "";
    channel = channel ? channel : "";
    jctx_t j = {{lh->nodes, 40, 0, lh->sbuf, sizeof lh->sbuf, 0, {0}}, -1};
    int root = rdof_jparse(&j.doc, json, strlen(json));
    const rdof_jnode_t *r = rdof_jnode(&j.doc, root);
    if (!r || r->type != RDOF_JOBJ || r->child < 0)
        return false;
    const rdof_jnode_t *top = rdof_jnode(&j.doc, r->child);
    if (!top || top->type != RDOF_JOBJ || !top->key)
        return false;
    j.obj = r->child;
    double v;
    if (!num(&j, "slot", &v))
        return false;
    int slot = (int)v;
    if (!strcmp(top->key, "BER") || !strcmp(top->key, "RSSI")) { /* a call's metrics, every second */
        rdof_lh_entry_t *e = find_active(lh, RDOF_LH_RF, radio, channel, slot);
        if (!e || !num(&j, "value", &v))
            return false;
        if (top->key[0] == 'B') {
            e->has_ber = true;
            e->ber = (float)v;
        } else {
            e->has_rssi = true;
            e->rssi = clamp16(v);
        }
        touch(lh, e);
        return true;
    }
    if (strcmp(top->key, "DMR"))
        return false;
    const char *action = str(&j, "action");
    if (!action)
        return false;
    if (!strcmp(action, "start"))
        return rf_start(lh, &j, role, radio, channel, slot, false, now, now_ms);
    if (!strcmp(action, "late_entry"))
        return rf_start(lh, &j, role, radio, channel, slot, true, now, now_ms);
    if (!strcmp(action, "end") || !strcmp(action, "lost"))
        return rf_end(lh, &j, radio, channel, slot, action, now);
    if (!strcmp(action, "csbk"))
        return rf_csbk(lh, &j, role, radio, channel, slot, now, now_ms);
    return false;
}

bool rdof_lh_privacy(rdof_lh_t *lh, const char *radio, const char *channel, int slot)
{
    rdof_lh_entry_t *e = find_active(lh, RDOF_LH_RF, radio ? radio : "", channel ? channel : "", slot);
    if (!e || e->encrypted)
        return false;
    e->encrypted = true;
    touch(lh, e);
    return true;
}

/* ── NET: what goes out ────────────────────────────────────────────────────── */

bool rdof_lh_tx(rdof_lh_t *lh, const char *role, const char *radio, const char *action,
                const rdof_tx_call_info_t *call, double duration, double now, int64_t now_ms)
{
    if (!lh->cap || !call || !action)
        return false;
    radio = radio ? radio : "";
    if (!strcmp(action, "start")) {
        rdof_lh_entry_t *e = add(lh, RDOF_LH_NET, role, radio, "", call->slot, !call->has_csbk, now, now_ms);
        if (!e)
            return false;
        e->src = call->src;
        e->dst = call->dst;
        e->group = call->group;
        e->freq = call->freq;
        e->cc = call->cc;
        if (call->has_csbk) {
            char d[48];
            rdof_csbk_desc(call->csbk, d, sizeof d);
            e->kind = RDOF_LH_CSBK;
            copy(e->desc, sizeof e->desc, d);
        } else if (call->frames >= 0) {
            e->kind = RDOF_LH_DATA;
            e->frames = call->frames;
        }
        return true;
    }
    if (!strcmp(action, "end")) {
        rdof_lh_entry_t *e = find_active(lh, RDOF_LH_NET, radio, "", call->slot);
        if (!e)
            return false;
        finish(lh, e, "end", duration, now);
        return true;
    }
    return false;
}

/* ── JSON ──────────────────────────────────────────────────────────────────── */

static const char *const kinds[] = {"voice", "data", "csbk"};

static bool entry_json(const rdof_lh_entry_t *e, double now, char *out, size_t cap, size_t *len)
{
    rdof_json_t j;
    rdof_json_init(&j, out, cap);
    rdof_json_open(&j, NULL);
    rdof_json_int(&j, "id", e->id);
    rdof_json_int(&j, "seq", e->seq);
    rdof_json_str(&j, "dir", e->dir == RDOF_LH_NET ? "net" : "rf");
    rdof_json_str(&j, "kind", kinds[e->kind]);
    rdof_json_str(&j, "role", e->role);
    rdof_json_str(&j, "radio", e->radio);
    if (e->channel[0])
        rdof_json_str(&j, "channel", e->channel);
    rdof_json_int(&j, "slot", e->slot);
    rdof_json_int(&j, "src", e->src);
    rdof_json_int(&j, "dst", e->dst);
    rdof_json_bool(&j, "group", e->group);
    if (e->freq)
        rdof_json_int(&j, "freq", e->freq);
    if (e->cc >= 0)
        rdof_json_int(&j, "cc", e->cc);
    if (e->start_ms)
        rdof_json_int(&j, "start_ms", e->start_ms);
    double age = now - e->start;
    rdof_json_float3(&j, "age_s", age > 0 ? age : 0);
    rdof_json_bool(&j, "active", e->active);
    if (!e->active && e->kind != RDOF_LH_CSBK) {
        rdof_json_float3(&j, "duration", e->duration);
        rdof_json_str(&j, "end", e->end);
    }
    if (e->has_ber)
        rdof_json_float3(&j, "ber", e->ber);
    if (e->has_rssi) {
        rdof_json_int(&j, "rssi", e->rssi);
        if (!e->active && (e->rssi_min || e->rssi_max)) {
            rdof_json_int(&j, "rssi_min", e->rssi_min);
            rdof_json_int(&j, "rssi_max", e->rssi_max);
        }
    }
    if (e->frames >= 0)
        rdof_json_int(&j, "frames", e->frames);
    if (e->desc[0])
        rdof_json_str(&j, "desc", e->desc);
    if (e->late)
        rdof_json_bool(&j, "late", true);
    if (e->emergency)
        rdof_json_bool(&j, "emergency", true);
    if (e->encrypted)
        rdof_json_bool(&j, "encrypted", true);
    rdof_json_close(&j);
    *len = j.len;
    return rdof_json_done(&j);
}

size_t rdof_lh_json(const rdof_lh_t *lh, uint32_t since, double now, int64_t now_ms, char *out, size_t cap)
{
    if (!out || cap < 64) {
        if (out && cap)
            out[0] = 0;
        return 0;
    }
    if (since > lh->seq) /* the board restarted: everything again */
        since = 0;
    size_t n = (size_t)snprintf(out, cap, "{\"seq\":%lu,\"now_ms\":%lld,\"max\":%d,\"calls\":[",
                                (unsigned long)lh->seq, (long long)(now_ms > 0 ? now_ms : 0), lh->cap);
    bool first = true, more = false;
    const size_t tail = 16; /* ],"more":true}  */
    for (int i = 0; i < lh->n; i++) {
        const rdof_lh_entry_t *e = nth(lh, i);
        if (e->seq <= since)
            continue;
        size_t len = 0;
        if (n + 1 + tail >= cap || !entry_json(e, now, out + n + !first, cap - n - !first - tail, &len)) {
            more = true;
            out[n] = 0;
            break;
        }
        if (!first)
            out[n] = ',';
        n += len + !first;
        first = false;
    }
    n += (size_t)snprintf(out + n, cap - n, more ? "],\"more\":true}" : "]}");
    return n < cap ? n : cap - 1;
}
