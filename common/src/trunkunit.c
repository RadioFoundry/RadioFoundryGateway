// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* A trunked site, joined up (rdof/trunkunit.h). */
#include "rdof/trunkunit.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "rdof/bits.h"
#include "rdof/mmdvm.h"
#include "rdof/trunkcfg.h"

#if RDOF_FEATURE_REPEATER

static const char *const MODELS[] = {"tiny", "small", "large", "huge"};

static void logf_(rdof_trunkunit_t *u, const char *fmt, ...)
{
    if (!u->io.log)
        return;
    char t[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    u->io.log(u->io.ctx, t);
}

static void pub(rdof_trunkunit_t *u, const char *topic, const void *data, size_t len, int qos, bool retain)
{
    if (u->net && u->io.publish)
        u->io.publish(u->io.ctx, topic, data, len, qos, retain);
}

static void pub_str(rdof_trunkunit_t *u, const char *topic, const char *s, int qos, bool retain)
{
    pub(u, topic, s, strlen(s), qos, retain);
}

static double wall_s(const rdof_trunkunit_t *u) { return (double)u->now_ms / 1000.0; }

static uint32_t rnd(rdof_trunkunit_t *u)
{
    uint32_t r = u->io.rand32 ? u->io.rand32(u->io.ctx) : 0x9E3779B9u * (uint32_t)(u->now_ms + 1);
    return r ? r : 1;
}

/* ── the subscriptions ───────────────────────────────────────────────────── */

static bool tg_in(const rdof_t3_idset_t *s, uint32_t tg)
{
    if (s->all)
        return true;
    for (int i = 0; i < s->n; i++)
        if (tg >= s->r[i].lo && tg <= s->r[i].hi)
            return true;
    return false;
}

/* The talkgroups the site needs: its MSs' (the controller's) and static_tg; all when they're too many to list. */
static void want_tgs(rdof_trunkunit_t *u, uint32_t *tgs, int *n, bool *all)
{
    uint32_t t[RDOF_TRUNKUNIT_TGS + 1];
    size_t m = rdof_t3_subs(u->site, t, RDOF_TRUNKUNIT_TGS + 1);
    *all = m > RDOF_TRUNKUNIT_TGS || u->pol.static_tg.all;
    *n = 0;
    for (size_t i = 0; i < m && i < RDOF_TRUNKUNIT_TGS; i++)
        tgs[(*n)++] = t[i];
    const rdof_t3_idset_t *s = &u->pol.static_tg;
    for (int i = 0; i < s->n && !*all; i++)
        for (uint64_t g = s->r[i].lo; g <= s->r[i].hi && !*all; g++) {
            bool have = false;
            for (int j = 0; j < *n; j++)
                have |= tgs[j] == g;
            if (have)
                continue;
            if (*n == RDOF_TRUNKUNIT_TGS)
                *all = true;
            else
                tgs[(*n)++] = (uint32_t)g;
        }
}

static void subscribe(rdof_trunkunit_t *u, bool force)
{
    uint32_t tgs[RDOF_TRUNKUNIT_TGS];
    int n;
    bool all;
    want_tgs(u, tgs, &n, &all);
    bool same = !force && all == u->tg_all && n == u->ntgs;
    for (int i = 0; same && i < n; i++)
        same = tgs[i] == u->tgs[i];
    memcpy(u->tgs, tgs, sizeof tgs);
    u->ntgs = n;
    u->tg_all = all;
    if (same || !u->net || !u->io.subscribe)
        return;
    char f[RDOF_TRUNKUNIT_FILTERS][128];
    const char *fp[RDOF_TRUNKUNIT_FILTERS];
    int k = 0;
    rdof_mn_topic(f[k++], sizeof f[0], u->root, RDOF_MN_T_DL, 0, u->node);
    snprintf(f[k++], sizeof f[0], "%s/cfg/net", u->root);
    snprintf(f[k++], sizeof f[0], "%s/cfg/node/%s", u->root, u->node);
    snprintf(f[k++], sizeof f[0], "%s/cfg/tg/+", u->root);
    snprintf(f[k++], sizeof f[0], "%s/cfg/ms/+", u->root);
    snprintf(f[k++], sizeof f[0], "%s/node/%s/ms/+", u->root, u->node);
    snprintf(f[k++], sizeof f[0], "%s/node/%s/cmd", u->root, u->node);
    snprintf(f[k++], sizeof f[0], "%s/where/+", u->root);
    if (all)
        snprintf(f[k++], sizeof f[0], "%s/tg/+/+", u->root);
    else
        for (int i = 0; i < n; i++)
            snprintf(f[k++], sizeof f[0], "%s/tg/%" PRIu32 "/+", u->root, tgs[i]);
    for (int i = 0; i < k; i++)
        fp[i] = f[i];
    u->io.subscribe(u->io.ctx, fp, k);
}

/* ── events out ──────────────────────────────────────────────────────────── */

static void activity_json(rdof_trunkunit_t *u, const char *json)
{
    char t[128];
    if (rdof_mn_node_topic(t, sizeof t, u->root, u->node, "activity"))
        pub_str(u, t, json, 1, false);
}

static rdof_mn_ev_t ev_of(const rdof_trunkunit_t *u, int ch, bool net, uint32_t stream)
{
    rdof_mn_ev_t e = {.node = u->node, .wall = wall_s(u), .path = u->path, .slot = ch >= 0 ? (ch & 1) + 1 : 0,
                      .source = net ? "net" : "rf", .stream = stream};
    return e;
}

static void record_out(rdof_trunkunit_t *u, uint32_t id, bool del)
{
    char t[128], j[512];
    if (!rdof_mn_ms_topic(t, sizeof t, u->root, u->node, id))
        return;
    rdof_t3_record_t r;
    if (del || !rdof_t3_record(u->site, id, &r)) {
        pub(u, t, "", 0, 1, true);
        u->records_out++;
        return;
    }
    rdof_mn_record_t m = {.id = r.id, .at_ms = r.at_ms, .exp_ms = r.exp_ms, .implicit = r.how == RDOF_T3_HOW_CALL,
                          .sub = r.sub_tg, .area = r.area};
    m.ntg = r.ntg < RDOF_MN_REC_TG ? r.ntg : RDOF_MN_REC_TG;
    memcpy(m.tg, r.tg, (size_t)m.ntg * sizeof m.tg[0]);
    if (rdof_mn_record_json(&m, j, sizeof j)) {
        pub_str(u, t, j, 1, true);
        u->records_out++;
    }
}

/* The called party's side answers (mqtt-net.md 9.4): its messages come from the called radio to the caller. The
   controller names both ways as caller (src) and called (dst). */
static bool answer_op(int op)
{
    return op == RDOF_MN_SETUP_ACCEPT || op == RDOF_MN_SETUP_REJECT || op == RDOF_MN_SETUP_ALERTING ||
           op == RDOF_MN_SETUP_ANSWERED;
}

static void setup_out(rdof_trunkunit_t *u, const rdof_t3_event_t *e)
{
    const rdof_mn_setup_t s = {.op = e->op, .reason = e->reason, .flags = e->flags, .options = e->svc, .ref = e->ref};
    const bool ans = answer_op(e->op);
    const uint32_t src = ans ? e->dst : e->src, dst = ans ? e->src : e->dst;
    uint8_t m[RDOF_MN_HEADER + 2 + RDOF_MN_SETUP_LEN];
    const size_t n = rdof_mn_setup_msg(m, sizeof m, e->stream, src, dst,
                                       u->path >= 0 ? (uint8_t)u->path : RDOF_MN_PATH_NONE, &s);
    char t[128];
    if (n && rdof_mn_topic(t, sizeof t, u->root, RDOF_MN_T_PC, dst, u->node)) {
        pub(u, t, m, n, 1, false);
        u->setups_out++;
    }
}

static void activity_out(rdof_trunkunit_t *u, const rdof_t3_event_t *e)
{
    static const char *const REG[] = {"?", "register", "deregister", "expire", "deny", "refuse"};
    static const char *const QUEUE[] = {"?", "in", "out", "drop"};
    char j[512], why[8];
    size_t n = 0;
    const bool net = (e->flags & RDOF_T3_F_NET) != 0;
    rdof_mn_ev_t ev = ev_of(u, e->type == RDOF_T3_EV_ACTIVITY && e->kind == RDOF_T3_ACT_GRANT ? e->lcn : -1, net,
                            e->stream);
    switch (e->kind) {
    case RDOF_T3_ACT_REG: {
        rdof_t3_record_t r;
        const bool rec = rdof_t3_record(u->site, e->src, &r);
        n = rdof_mn_reg_event(&ev, e->src, REG[e->op <= 5 ? e->op : 0], rec ? r.tg : NULL, rec ? r.ntg : 0, j,
                              sizeof j);
        break;
    }
    case RDOF_T3_ACT_GRANT:
        n = rdof_mn_grant_event(&ev, e->src, e->dst, (e->flags & RDOF_T3_F_GROUP) != 0, e->chan, e->lcn,
                                (e->flags & RDOF_T3_F_LATE) != 0, j, sizeof j);
        break;
    case RDOF_T3_ACT_QUEUE:
        snprintf(why, sizeof why, "0x%02x", e->reason);
        n = rdof_mn_queue_event(&ev, e->src, e->dst, (e->flags & RDOF_T3_F_GROUP) != 0, QUEUE[e->op <= 3 ? e->op : 0],
                                e->op == 3 ? why : NULL, j, sizeof j);
        break;
    case RDOF_T3_ACT_SETUP:
        n = rdof_mn_setup_event(&ev, e->src, e->dst, e->op, e->reason, NULL, j, sizeof j);
        break;
    case RDOF_T3_ACT_REFUSE:
        snprintf(why, sizeof why, "0x%02x", e->reason);
        n = rdof_mn_queue_event(&ev, e->src, e->dst, (e->flags & RDOF_T3_F_GROUP) != 0, "refuse", why, j, sizeof j);
        break;
    default:
        break;
    }
    if (n)
        activity_json(u, j);
}

/* ── network streams ─────────────────────────────────────────────────────── */

static rdof_trunkunit_stream_t *stream_find(rdof_trunkunit_t *u, uint32_t id)
{
    for (int i = 0; i < RDOF_TRUNKUNIT_STREAMS; i++)
        if (u->st[i].used && u->st[i].stream == id)
            return &u->st[i];
    return NULL;
}

static void stream_feed(rdof_trunkunit_t *u, rdof_trunkunit_stream_t *s, const rdof_frame_t *f)
{
    if (s->mapped) {
        if (!rdof_trunkrf_net(&u->rf, s->ch, f))
            u->net_dropped++;
        return;
    }
    if (s->nhold == RDOF_TRUNKUNIT_HOLD) { /* keep the header (the first), drop the oldest after it */
        memmove(&s->hold[1], &s->hold[2], (RDOF_TRUNKUNIT_HOLD - 2) * sizeof s->hold[0]);
        s->nhold--;
        u->net_dropped++;
    }
    s->hold[s->nhold++] = *f;
    u->net_held++;
}

static void stream_map(rdof_trunkunit_t *u, uint32_t id, int ch)
{
    rdof_trunkunit_stream_t *s = stream_find(u, id);
    if (!s || s->mapped)
        return;
    s->mapped = true;
    s->ch = ch;
    for (int i = 0; i < s->nhold; i++)
        if (!rdof_trunkrf_net(&u->rf, ch, &s->hold[i]))
            u->net_dropped++;
    s->nhold = 0;
}

static void stream_end(rdof_trunkunit_t *u, rdof_trunkunit_stream_t *s, bool term)
{
    if (s->mapped && !term)
        rdof_trunkrf_net_end(&u->rf, s->ch);
    if (s->group)
        rdof_t3_net_group_end(u->site, s->stream);
    else
        rdof_t3_net_private_end(u->site, s->stream);
    memset(s, 0, sizeof *s);
}

/* ── the controller's events ─────────────────────────────────────────────── */

static void drain(rdof_trunkunit_t *u)
{
    rdof_t3_event_t e;
    bool subs = false;
    while (rdof_t3_event(u->site, &e)) {
        switch (e.type) {
        case RDOF_T3_EV_CH_OPEN:
            if (e.ch < RDOF_T3_CHANNELS) {
                u->ch_flags[e.ch] = e.flags; /* the call's, for the uplink */
                rdof_trunkrf_open(&u->rf, e.ch, e.src, e.dst, (e.flags & RDOF_T3_F_GROUP) != 0, RDOF_TRUNKRF_WARMUP);
                if (e.stream)
                    stream_map(u, e.stream, e.ch);
            }
            break;
        case RDOF_T3_EV_CH_CLOSE: {
            rdof_trunkrf_ch_t *c = rdof_trunkrf_channel(&u->rf, e.ch);
            if (c && c->ctl_left > 0)
                c->close_after = true; /* after its last P_CLEAR */
            else if (c)
                rdof_trunkrf_close(&u->rf, e.ch);
            if (e.ch < RDOF_T3_CHANNELS)
                u->ch_flags[e.ch] = 0;
            for (int i = 0; i < RDOF_TRUNKUNIT_STREAMS; i++)
                if (u->st[i].used && u->st[i].mapped && u->st[i].ch == e.ch)
                    u->st[i].mapped = false, u->st[i].ended = true;
            break;
        }
        case RDOF_T3_EV_CH_STREAM:
            stream_map(u, e.stream, e.ch);
            break;
        case RDOF_T3_EV_NET_SETUP:
            setup_out(u, &e);
            break;
        case RDOF_T3_EV_NET_SUBS:
            subs = true;
            break;
        case RDOF_T3_EV_RECORD:
            record_out(u, e.src, e.why == 1);
            break;
        case RDOF_T3_EV_ACTIVITY:
            activity_out(u, &e);
            break;
        case RDOF_T3_EV_NET_DROP:
            logf_(u, "trunk: network stream %08" PRIx32 " not carried (%u)", e.stream, e.why);
            break;
        case RDOF_T3_EV_LOG:
            logf_(u, "trunk: %.*s", (int)sizeof e.text, e.text);
            break;
        default:
            break;
        }
    }
    if (subs)
        subscribe(u, false);
}

/* ── the radio layer's callbacks ─────────────────────────────────────────── */

static rdof_t3_slot_out_t *held(rdof_trunkunit_t *u, int64_t k)
{
    if (k < 0)
        return NULL;
    rdof_trunkunit_held_t *r = &u->ring[k % RDOF_TRUNKUNIT_RING];
    return r->k == k ? &r->o : NULL;
}

static bool io_tscc(void *ctx, int64_t k, int *dt, uint8_t pdu[10])
{
    rdof_t3_slot_out_t *o = held(ctx, k);
    if (!o || !o->tscc_dt)
        return false;
    *dt = o->tscc_dt;
    memcpy(pdu, o->tscc_pdu, 10);
    return true;
}

static void io_cach(void *ctx, int carrier, int64_t k, int *at, int32_t *slc28)
{
    rdof_trunkunit_t *u = ctx;
    const rdof_t3_slot_out_t *o = held(u, k), *prev = held(u, k - 1);
    const bool tscc = carrier == 0 && u->rf.tscc;
    if (tscc && prev)
        *at = prev->at; /* the controller's for the CACH before slot k is slot k - 1's */
    if (o && o->has_slc)
        *slc28 = (int32_t)(tscc ? o->slc_tscc : o->slc_payload);
}

static void io_pdu(void *ctx, int ch, int64_t k, int dt, const uint8_t pdu[10], int rssi)
{
    rdof_trunkunit_t *u = ctx;
    rdof_t3_rx_pdu(u->site, ch, k, (uint8_t)dt, pdu, rssi);
}

static void io_bad(void *ctx, int ch, int64_t k)
{
    rdof_trunkunit_t *u = ctx;
    rdof_t3_rx_bad(u->site, ch, k);
}

static void io_activity(void *ctx, int ch, int64_t k, int what, uint32_t src, uint32_t dst, bool group)
{
    rdof_trunkunit_t *u = ctx;
    static const uint8_t MAP[] = {0, RDOF_T3_VOICE_HEADER, RDOF_T3_VOICE_FRAME, RDOF_T3_VOICE_TERM,
                                  RDOF_T3_VOICE_LOST};
    if (what >= 1 && what <= 4)
        rdof_t3_rx_voice(u->site, ch, k, MAP[what], src, dst, group);
}

static void io_uplink(void *ctx, int ch, const rdof_frame_t *f)
{
    rdof_trunkunit_t *u = ctx;
    if (u->net && ch / 2 < RDOF_T3_CARRIERS)
        rdof_mn_up_frame(&u->up[ch / 2], f, wall_s(u));
}

/* ── the uplinks: a carrier each, its slots its LCNs ─────────────────────── */

static rdof_trunkunit_t *up_unit(void *ctx, int *c)
{
    const rdof_trunkunit_upctx_t *x = ctx;
    *c = x->c;
    return x->u;
}

static int up_path_of(void *ctx, int slot)
{
    int c;
    rdof_trunkunit_t *u = up_unit(ctx, &c);
    const int ch = 2 * c + (slot == 2);
    return u->path >= 0 && (u->ch_flags[ch] & (RDOF_T3_F_WIDE | RDOF_T3_F_PRIVATE_NET)) ? u->path : -1;
}

static bool up_admit(void *ctx, int slot, uint32_t src, uint32_t dst, bool group)
{
    int c;
    rdof_trunkunit_t *u = up_unit(ctx, &c);
    return (u->ch_flags[2 * c + (slot == 2)] & (RDOF_T3_F_WIDE | RDOF_T3_F_PRIVATE_NET)) != 0;
}

static void up_publish(void *ctx, int kind, uint32_t dst, const uint8_t *msg, size_t len)
{
    int c;
    rdof_trunkunit_t *u = up_unit(ctx, &c);
    char t[128];
    if (rdof_mn_topic(t, sizeof t, u->root, kind, dst, u->node))
        pub(u, t, msg, len, 0, false);
}

static void up_activity(void *ctx, const char *json)
{
    int c;
    activity_json(up_unit(ctx, &c), json);
}

static uint32_t up_rand(void *ctx)
{
    int c;
    return rnd(up_unit(ctx, &c));
}

static double up_wall(void *ctx)
{
    int c;
    return wall_s(up_unit(ctx, &c));
}

/* ── the downlink ────────────────────────────────────────────────────────── */

static void down_burst(void *ctx, const rdof_net_burst_t *b, int path, bool dl, uint8_t flags, const char *from)
{
    rdof_trunkunit_t *u = ctx;
    const uint32_t id = rdof_be32(b->stream_id);
    rdof_frame_t f;
    memset(&f, 0, sizeof f);
    rdof_control_to_dt(b->control, &f.dt, &f.n);
    memcpy(f.burst, b->burst, RDOF_BURST_BYTES);
    f.slot = b->slot ? b->slot : 1;
    f.has_addr = true;
    f.src = b->src;
    f.dst = b->dst;
    f.group = b->group;
    u->net_in++;
    rdof_trunkunit_stream_t *s = stream_find(u, id);
    if (!s) {
        if (b->terminator)
            return; /* the end of a stream not carried */
        if (b->group && u->tg_all && !tg_in(&u->pol.static_tg, b->dst)) {
            /* {root}/tg/+/+: only the talkgroups the site's MSs need (all of them when it has too many to tell) */
            uint32_t t[RDOF_TRUNKUNIT_TGS + 1];
            const size_t m = rdof_t3_subs(u->site, t, RDOF_TRUNKUNIT_TGS + 1);
            bool want = m > RDOF_TRUNKUNIT_TGS;
            for (size_t i = 0; i < m; i++)
                want |= t[i] == b->dst;
            if (!want) {
                u->tg_filtered++;
                return;
            }
        }
        for (int i = 0; i < RDOF_TRUNKUNIT_STREAMS && !s; i++)
            if (!u->st[i].used)
                s = &u->st[i];
        if (!s) {
            u->net_dropped++;
            return;
        }
        *s = (rdof_trunkunit_stream_t){.used = true, .group = b->group, .stream = id, .src = b->src, .dst = b->dst,
                                       .last_ms = u->now_ms};
        if (b->group)
            rdof_t3_net_group(u->site, id, b->src, b->dst,
                              (flags & RDOF_MN_F_EMERGENCY) ? RDOF_T3_F_EMERGENCY : 0, u->now_ms);
        else
            rdof_t3_net_private(u->site, id, b->src, b->dst, u->now_ms);
        drain(u); /* a free channel: CH_OPEN at once */
        s = stream_find(u, id);
        if (!s)
            return;
    }
    s->last_ms = u->now_ms;
    if (!s->ended)
        stream_feed(u, s, &f);
    if (b->terminator)
        stream_end(u, s, true);
    drain(u);
}

static void down_setup(void *ctx, const rdof_mn_hdr_t *h, const rdof_mn_setup_t *s, bool dl, const char *from)
{
    rdof_trunkunit_t *u = ctx;
    if (!dl)
        return;
    const bool ans = answer_op(s->op);
    const rdof_t3_setup_t m = {.op = s->op, .reason = s->reason, .flags = s->flags, .svc = s->options,
                               .src = ans ? h->dst : h->src, .dst = ans ? h->src : h->dst, .stream = h->stream,
                               .ref = s->ref};
    u->setups_in++;
    rdof_t3_net_setup(u->site, &m, u->now_ms);
    drain(u);
}

/* ── provisioning ────────────────────────────────────────────────────────── */

static void ms_policies(rdof_trunkunit_t *u)
{
    uint32_t now[RDOF_T3_MS_POLICIES];
    int n = 0;
    for (int i = 0; i < u->mc.nms && n < RDOF_T3_MS_POLICIES; i++) {
        const rdof_mn_ms_cfg_t *m = &u->mc.ms[i];
        rdof_t3_ms_policy_t p = {.enabled = m->enabled, .private_ok = m->private_, .wide_private = m->wide_private,
                                 .emergency_ok = m->emergency, .priority = (uint8_t)m->priority};
        p.ntg = (uint8_t)(m->ntg < RDOF_T3_REG_TGS ? m->ntg : RDOF_T3_REG_TGS);
        memcpy(p.tg, m->tg, p.ntg * sizeof p.tg[0]);
        if (rdof_t3_ms_policy(u->site, m->id, &p))
            now[n++] = m->id;
    }
    for (int i = 0; i < u->nms; i++) { /* documents deleted */
        bool still = false;
        for (int j = 0; j < n && !still; j++)
            still = now[j] == u->ms_ids[i];
        if (!still)
            rdof_t3_ms_policy(u->site, u->ms_ids[i], NULL);
    }
    memcpy(u->ms_ids, now, (size_t)n * sizeof now[0]);
    u->nms = n;
}

bool rdof_trunkunit_apply(rdof_trunkunit_t *u)
{
    rdof_mn_site_cfg_t sc;
    rdof_mn_cfg_site(&u->mc, NULL, &sc);
    rdof_t3_cfg_t cfg = u->base;
    rdof_t3_policy_t pol = u->base_pol;
    char err[120];
    if (!rdof_trunk_cfg_site(&sc, &cfg, &pol, err, sizeof err) || !rdof_trunk_cfg_check(&cfg, err, sizeof err)) {
        logf_(u, "trunk: provisioning refused: %s", err);
        u->cfg_bad++;
        return false;
    }
    if (memcmp(&cfg, &u->cfg, sizeof cfg)) {
        if (!rdof_t3_reconfigure(u->site, &cfg, err, sizeof err)) {
            logf_(u, "trunk: provisioning refused by the controller: %s", err);
            u->cfg_bad++;
            return false;
        }
        u->cfg = cfg;
    }
    if (memcmp(&pol, &u->pol, sizeof pol)) {
        rdof_t3_policy(u->site, &pol);
        u->pol = pol;
    }
    const bool off = (sc.has & RDOF_MN_HAS_ENABLED) && !sc.enabled;
    if (off != u->withdrawn_cfg) {
        rdof_t3_withdraw(u->site, off);
        u->withdrawn_cfg = off;
        logf_(u, "trunk: %s by provisioning", off ? "withdrawn" : "back on the air");
    }
    ms_policies(u);
    u->cfg_applied++;
    drain(u);
    subscribe(u, false);
    return true;
}

/* ── commands ────────────────────────────────────────────────────────────── */

static void publish_status(rdof_trunkunit_t *u)
{
    char t[128], j[2048];
    if (rdof_mn_node_topic(t, sizeof t, u->root, u->node, "status") && rdof_trunkunit_status_json(u, j, sizeof j))
        pub_str(u, t, j, 1, true);
    u->status_ms = u->now_ms;
}

static void command(rdof_trunkunit_t *u, const char *json, size_t len)
{
    rdof_mn_cmd_t c;
    char err[96] = "", t[128], j[256];
    bool ok = rdof_mn_cmd_parse(json, len, &c, err, sizeof err);
    u->cmds++;
    if (ok) {
        switch (c.cmd) {
        case RDOF_MN_CMD_DEREGISTER:
            ok = rdof_t3_command(u->site, "deregister", c.ms, 0);
            break;
        case RDOF_MN_CMD_CLEAR:
            ok = rdof_t3_command(u->site, "clear", (uint32_t)c.chan, (uint32_t)c.lcn);
            break;
        case RDOF_MN_CMD_WITHDRAW:
            ok = rdof_t3_command(u->site, "withdraw", 0, 0);
            break;
        case RDOF_MN_CMD_RESTORE:
            ok = rdof_t3_command(u->site, "restore", 0, 0);
            break;
        case RDOF_MN_CMD_RESEND_RECORDS:
            ok = rdof_t3_command(u->site, "resend_records", 0, 0);
            break;
        case RDOF_MN_CMD_STATUS:
            ok = true;
            break;
        default:
            ok = false;
            break;
        }
        if (!ok)
            snprintf(err, sizeof err, "%s: refused", c.name[0] ? c.name : "command");
        drain(u);
        if (c.cmd == RDOF_MN_CMD_STATUS)
            publish_status(u);
    }
    if (rdof_mn_node_topic(t, sizeof t, u->root, u->node, "cmd/res") &&
        rdof_mn_cmd_result_json(c.id, ok, ok ? NULL : err, j, sizeof j))
        pub_str(u, t, j, 1, false);
}

/* ── the API ─────────────────────────────────────────────────────────────── */

bool rdof_trunkunit_init(rdof_trunkunit_t *u, void *site_mem, const rdof_t3_cfg_t *cfg, const rdof_t3_policy_t *pol,
                         const rdof_trunkunit_io_t *io, const char *root, const char *node, int path,
                         const char *firmware, char *err, size_t cap)
{
    memset(u, 0, sizeof *u);
    u->io = *io;
    u->site = site_mem;
    u->base = u->cfg = *cfg;
    u->base_pol = u->pol = *pol;
    u->path = path;
    u->k_next = -1;
    for (int i = 0; i < RDOF_TRUNKUNIT_RING; i++)
        u->ring[i].k = -1;
    snprintf(u->firmware, sizeof u->firmware, "%s", firmware ? firmware : "rdofradio");
    u->net = root && node && root[0] && rdof_mn_node_ok(node);
    snprintf(u->root, sizeof u->root, "%s", root ? root : "");
    snprintf(u->node, sizeof u->node, "%s", node ? node : "");
    if (!rdof_t3_init(u->site, cfg, err, cap))
        return false;
    rdof_t3_policy(u->site, pol);
    const rdof_trunkrf_io_t rio = {io_tscc, io_cach, io_pdu, io_bad, io_activity, io_uplink, u};
    if (!rdof_trunkrf_init(&u->rf, &rio, cfg->ncarriers, cfg->cc, cfg->carriers[0].tscc != 0)) {
        snprintf(err, cap, "trunk: %d carriers: the radio layer takes 1 to %d", cfg->ncarriers,
                 RDOF_TRUNKRF_CARRIERS);
        return false;
    }
    rdof_mn_cfg_init(&u->mc, u->node);
    for (int c = 0; c < RDOF_T3_CARRIERS; c++) {
        rdof_mn_up_t *x = &u->up[c];
        rdof_mn_up_init(x, u->node);
        u->upctx[c].u = u;
        u->upctx[c].c = c;
        x->path_of = up_path_of;
        x->admit = up_admit;
        x->publish = up_publish;
        x->activity = up_activity;
        x->rand32 = up_rand;
        x->wall = up_wall;
        x->ctx = &u->upctx[c];
        if (path >= 0 && path < 64)
            x->tx_ok = 1ull << path;
    }
    rdof_mn_down_init(&u->down);
    u->down.burst = down_burst;
    u->down.setup = down_setup;
    u->down.ctx = u;
    drain(u);
    return true;
}

void rdof_trunkunit_run(rdof_trunkunit_t *u, int64_t k_upto, int64_t now_ms)
{
    u->now_ms = now_ms;
    if (!u->start_ms)
        u->start_ms = now_ms;
    if (u->k_next < 0 || k_upto - u->k_next > RDOF_TRUNKUNIT_RING * 8)
        u->k_next = k_upto; /* the first slot, or the clock jumped: start again there */
    for (; u->k_next <= k_upto; u->k_next++) {
        const int64_t k = u->k_next;
        rdof_trunkunit_held_t *r = &u->ring[k % RDOF_TRUNKUNIT_RING];
        rdof_t3_slot(u->site, k, now_ms, &r->o);
        r->k = k;
        for (int c = 0; c < u->cfg.ncarriers; c++) {
            const int ch = 2 * c + (int)(k & 1);
            if (ch == 0 && u->rf.tscc)
                continue;
            if (r->o.ch[ch].dt)
                rdof_trunkrf_control(&u->rf, ch, r->o.ch[ch].dt, r->o.ch[ch].pdu, 1, false);
        }
        drain(u);
        rdof_trunkrf_tick(&u->rf, k);
        drain(u);
    }
}

bool rdof_trunkunit_slot(rdof_trunkunit_t *u, int c, int64_t k, uint8_t burst[RDOF_BURST_BYTES], uint8_t *control,
                         int *at, int32_t *slc28)
{
    if (u->k_next <= k)
        rdof_trunkunit_run(u, k, u->now_ms);
    const bool ok = rdof_trunkrf_slot(&u->rf, c, k, burst, control, at, slc28);
    drain(u);
    return ok;
}

void rdof_trunkunit_split_pump(rdof_trunkunit_t *u, rdof_trunkrf_split_t *o, struct rdof_split *s, int64_t k_now,
                               double now)
{
    if (u->k_next <= k_now + 4)
        rdof_trunkunit_run(u, k_now + 4, u->now_ms);
    rdof_trunkrf_split_pump(&u->rf, o, s, k_now, 0, now);
    drain(u);
}

int rdof_trunkunit_at(const rdof_trunkunit_t *u, int64_t k)
{
    const rdof_trunkunit_held_t *r = k >= 0 ? &u->ring[k % RDOF_TRUNKUNIT_RING] : NULL;
    return r && r->k == k ? r->o.at : -1;
}

void rdof_trunkunit_rx(rdof_trunkunit_t *u, int c, int64_t k, const rdof_burst_t *b, int rssi)
{
    rdof_trunkrf_rx(&u->rf, c, k, b, rssi);
    drain(u);
}

void rdof_trunkunit_message(rdof_trunkunit_t *u, const char *topic, const void *data, size_t len)
{
    if (!u->net)
        return;
    const char *text = data;
    uint32_t id;
    char node[RDOF_MN_NODE_MAX + 1], note[96], err[96];
    const int kind = rdof_mn_topic_parse(topic, u->root, &id, node, sizeof node);
    if (kind == RDOF_MN_T_DL) {
        if (!strcmp(node, u->node))
            rdof_mn_down_message(&u->down, "router", true, data, len, wall_s(u));
        return;
    }
    if (kind == RDOF_MN_T_TG) {
        if (strcmp(node, u->node))
            rdof_mn_down_message(&u->down, node, false, data, len, wall_s(u));
        return;
    }
    if (kind != RDOF_MN_T_NONE)
        return;
    const size_t rl = strlen(u->root);
    if (strncmp(topic, u->root, rl) || topic[rl] != '/')
        return;
    const char *leaf = topic + rl + 1;
    if (!strncmp(leaf, "cfg/", 4)) {
        const rdof_mn_cfg_result_t r = rdof_mn_cfg_message(&u->mc, u->root, topic, text, len, note, sizeof note);
        if (r == RDOF_MN_CFG_APPLIED || r == RDOF_MN_CFG_DELETED) {
            if (note[0])
                logf_(u, "trunk: %s: %s", leaf, note);
            rdof_trunkunit_apply(u);
            if (u->io.cache)
                u->io.cache(u->io.ctx);
        } else if (r == RDOF_MN_CFG_BAD || r == RDOF_MN_CFG_FULL) {
            logf_(u, "trunk: %s: %s", leaf, r == RDOF_MN_CFG_FULL ? "no room" : "malformed");
            u->cfg_bad++;
        }
        return;
    }
    if (rdof_mn_ms_topic_parse(topic, u->root, node, sizeof node, &id)) {
        if (strcmp(node, u->node) || !len)
            return; /* another node's, or a deletion */
        rdof_mn_record_t m;
        if (!rdof_mn_record_parse(text, len, &m, err, sizeof err)) {
            logf_(u, "trunk: %s: %s", leaf, err);
            return;
        }
        rdof_t3_record_t r = {.id = m.id, .sub_tg = m.sub, .how = RDOF_T3_HOW_LOADED,
                              .area = (uint8_t)(m.area >= 0 ? m.area : 0), .at_ms = m.at_ms, .exp_ms = m.exp_ms};
        r.ntg = (uint8_t)(m.ntg < RDOF_T3_REG_TGS ? m.ntg : RDOF_T3_REG_TGS);
        memcpy(r.tg, m.tg, r.ntg * sizeof r.tg[0]);
        rdof_t3_record_t have;
        if (rdof_t3_record(u->site, m.id, &have) && have.at_ms >= m.at_ms)
            return; /* ours is as new: this is the one we published */
        if (rdof_t3_load_record(u->site, &r))
            u->records_in++;
        drain(u);
        subscribe(u, false);
        return;
    }
    if (rdof_mn_where_topic_parse(topic, u->root, &id)) {
        rdof_mn_where_t w;
        if (len && rdof_mn_where_parse(text, len, &w, err, sizeof err)) {
            rdof_t3_where(u->site, id, !strcmp(w.node, u->node), w.at_ms);
            drain(u);
        }
        return;
    }
    char cmd[128];
    snprintf(cmd, sizeof cmd, "node/%s/cmd", u->node);
    if (!strcmp(leaf, cmd))
        command(u, text, len);
}

void rdof_trunkunit_link(rdof_trunkunit_t *u, bool up)
{
    u->connected = up;
    rdof_t3_net_link(u->site, up);
    if (up) {
        subscribe(u, true);
        rdof_t3_command(u->site, "resend_records", 0, 0);
        drain(u);
        publish_status(u);
    } else {
        drain(u);
    }
}

void rdof_trunkunit_tick(rdof_trunkunit_t *u, int64_t now_ms)
{
    u->now_ms = now_ms;
    if (!u->start_ms)
        u->start_ms = now_ms;
    const double now = wall_s(u);
    rdof_mn_down_tick(&u->down, now);
    for (int c = 0; c < u->cfg.ncarriers; c++)
        rdof_mn_up_tick(&u->up[c], now);
    for (int i = 0; i < RDOF_TRUNKUNIT_STREAMS; i++) /* a stream gone quiet without its terminator */
        if (u->st[i].used && now_ms - u->st[i].last_ms > 3000)
            stream_end(u, &u->st[i], false);
    drain(u);
    if (u->connected && now_ms - u->status_ms >= RDOF_TRUNKUNIT_STATUS_S * 1000)
        publish_status(u);
}

size_t rdof_trunkunit_trunk_json(const rdof_trunkunit_t *u, char *out, size_t cap)
{
    static const char *const PAR[] = {"?", "a", "b", "ab"};
    static const char *const SERVICES[] = {"reg", "group", "private", "allcall"};
    const rdof_t3_cfg_t *c = &u->cfg;
    rdof_mn_carrier_t car[RDOF_T3_CARRIERS], tscc[1];
    int ntscc = 0;
    for (int i = 0; i < c->ncarriers; i++) {
        car[i] = (rdof_mn_carrier_t){c->carriers[i].chan, c->carriers[i].tx_hz, c->carriers[i].rx_hz, -1};
        if (c->carriers[i].tscc && !ntscc) {
            tscc[0] = car[i];
            tscc[0].cc = c->cc;
            ntscc = 1;
        }
    }
    const rdof_mn_trunk_info_t t = {.model = MODELS[c->model & 3], .net = c->net, .site = c->site,
                                    .area_bits = c->area_bits, .par = PAR[c->par & 3], .tscc = tscc, .ntscc = ntscc,
                                    .carriers = car, .ncarriers = c->ncarriers, .services = SERVICES,
                                    .nservices = 4, .reg = c->reg_required != 0};
    return rdof_mn_trunk_info_json(&t, out, cap);
}

size_t rdof_trunkunit_status_json(const rdof_trunkunit_t *u, char *out, size_t cap)
{
    static const char *const USE[] = {"off", "idle", "tscc", "call", "hang", "hang"};
    rdof_t3_status_t s;
    rdof_t3_status(u->site, &s);
    rdof_mn_chan_status_t ch[RDOF_T3_CHANNELS];
    const int n = s.nch < RDOF_T3_CHANNELS ? s.nch : RDOF_T3_CHANNELS;
    for (int i = 0; i < n; i++) {
        const rdof_t3_ch_status_t *x = &s.ch[i];
        const bool busy = x->use == RDOF_T3_USE_CALL || x->use == RDOF_T3_USE_HANG || x->use == RDOF_T3_USE_CLEAR;
        ch[i] = (rdof_mn_chan_status_t){.chan = x->chan, .lcn = x->lcn, .use = USE[x->use <= 5 ? x->use : 0],
                                        .src = busy ? x->src : 0, .dst = busy ? x->dst : 0,
                                        .group = (x->flags & RDOF_T3_F_GROUP) != 0,
                                        .since_ms = busy ? x->since_ms : 0};
    }
    const rdof_mn_trunk_status_t t = {.connected = s.connected, .regs = (int)s.regs, .queue = (int)s.queue,
                                      .ch = ch, .nch = n, .aloha = s.aloha, .rand = s.rand,
                                      .collisions = s.collisions, .grants = s.grants, .refused = s.refused,
                                      .queued = s.queued};
    const rdof_mn_status_t st = {.node = u->node, .firmware = u->firmware, .wall = wall_s(u),
                                 .uptime_s = (double)(u->now_ms - u->start_ms) / 1000.0, .trunk = &t};
    return rdof_mn_status_json(&st, out, cap);
}

#endif /* RDOF_FEATURE_REPEATER */
