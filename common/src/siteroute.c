// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The router on a running box (rdof/siteroute.h). */
#include "rdof/siteroute.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "rdof/features.h"
#include "rdof/mmdvm.h"
#include "rdof/pdu.h"
#include "rdof/repeater.h"
#include "rdof/txvr.h"

void rdof_siteroute_init(rdof_siteroute_t *sr, const rdof_site_t *site)
{
    memset(sr, 0, sizeof *sr);
    rdof_router_init(&sr->router, site);
    rdof_txpool_init(&sr->pool, RDOF_ROUTER_IDLE_S);
}

bool rdof_siteroute_add_repeater(rdof_siteroute_t *sr, int app, struct rdof_repeater *r)
{
    const rdof_site_t *s = sr->router.site;
    if (app < 0 || app >= s->napp || s->app[app].type != RDOF_APP_REPEATER)
        return false;
    sr->rpt[app] = r;
    return true;
}

bool rdof_siteroute_add_repeater_freq(rdof_siteroute_t *sr, int freq, struct rdof_repeater *r)
{
    const rdof_site_t *s = sr->router.site;
    if (freq < 0 || freq >= s->nfreq || s->freq[freq].mode == RDOF_FMODE_SIMPLEX)
        return false;
    sr->rpt_freq[freq] = r;
    return true;
}

bool rdof_siteroute_add_txvr(rdof_siteroute_t *sr, struct rdof_txvr *t, uint64_t freqs, const rdof_txmap_t *map)
{
#if RDOF_FEATURE_TX
    const double answer_s = t ? t->answer_s : 0;
#else
    const double answer_s = 0;
#endif
    const int i = rdof_txpool_add(&sr->pool, freqs, answer_s);
    if (i < 0)
        return false;
    sr->tx[i] = t;
    sr->txmap[i] = map ? *map : (rdof_txmap_t){0, 0};
    return true;
}

int rdof_siteroute_rf(rdof_siteroute_t *sr, uint32_t hz, int slot, uint32_t src, uint32_t dst, bool group, double now)
{
    for (int k = 0; k < RDOF_SITEROUTE_MEMO; k++) {
        const rdof_siteroute_memo_t *m = &sr->memo[k];
        if (m->at > 0 && now - m->at < 1.0 && m->hz == hz && m->slot == slot && m->src == src && m->dst == dst)
            return m->net;
    }
    const rdof_rcall_t c = {.src = src, .dst = dst, .group = group};
    const rdof_route_t r = rdof_router_rf(&sr->router, rdof_site_path_rx(sr->router.site, hz, slot), &c, now);
    sr->why = r.kind == RDOF_ROUTE_NONE ? r.why : NULL;
    const int net = r.kind == RDOF_ROUTE_NET ? r.net : -1;
    sr->memo[sr->memo_next].hz = hz, sr->memo[sr->memo_next].src = src, sr->memo[sr->memo_next].dst = dst;
    sr->memo[sr->memo_next].slot = (int8_t)slot, sr->memo[sr->memo_next].net = (int8_t)net;
    sr->memo[sr->memo_next].at = now > 0 ? now : 1e-9;
    sr->memo_next = (sr->memo_next + 1) % RDOF_SITEROUTE_MEMO;
    return net;
}

bool rdof_siteroute_reaches(const rdof_siteroute_t *sr, uint32_t hz, int slot, int app)
{
    const int p = rdof_site_path_rx(sr->router.site, hz, slot);
    return p >= 0 && app >= 0 && (sr->router.site->path[p].pol.apps >> app & 1);
}

static uint32_t sid32(const uint8_t sid[4])
{
    return (uint32_t)sid[0] << 24 | (uint32_t)sid[1] << 16 | (uint32_t)sid[2] << 8 | sid[3];
}

bool rdof_siteroute_tuned(const rdof_siteroute_t *sr, const uint8_t sid[4])
{
    const uint32_t id = sid32(sid);
    for (int k = 0; k < RDOF_SITEROUTE_TUNED; k++)
        if (id && sr->tuned[k] == id)
            return true;
    return false;
}

#if RDOF_FEATURE_TX
/* The RF level for the frequency's power_dbm on transmitter i, at cap_dbm at most (INFINITY: none). A transmitter
   without a map sends at its own power (255), or under a cap at its least (0): it can't tell dBm. */
static int tune_level(const rdof_siteroute_t *sr, int i, double power_dbm, double cap_dbm)
{
    const rdof_txmap_t *m = &sr->txmap[i];
    if (!rdof_txmap_valid(m))
        return isnan(cap_dbm) || cap_dbm == INFINITY ? 255 : 0;
    if (!isnan(cap_dbm) && (isnan(power_dbm) || cap_dbm < power_dbm))
        power_dbm = cap_dbm;
    if (power_dbm == INFINITY)
        power_dbm = NAN;
    if (!isnan(power_dbm) && power_dbm > m->max_dbm)
        power_dbm = m->max_dbm;
    /* a level never over what was asked: round toward the least */
    const int l = rdof_txpower_level(m, power_dbm);
    return l > 0 && rdof_txpower_dbm(m, l) > power_dbm + 1e-9 ? l - 1 : l;
}

/* Burst o of a stream on path k, through the pool: its transceiver, tuned at the stream's first burst, at cap_dbm
   at most. False: none free. */
static bool send_txvr(rdof_siteroute_t *sr, int k, const rdof_net_burst_t *o, bool urgent, bool take_hold,
                      bool path_preempt, bool local, double cap_dbm, double now)
{
    const rdof_site_t *s = sr->router.site;
    const rdof_path_t *p = &s->path[k];
    const uint32_t stream = sid32(o->stream_id);
    const rdof_txp_req_t rq = {.stream = stream ? stream : 1, .src = o->src, .dst = o->dst, .group = o->group,
                               .path = k, .freq = p->tx_freq, .urgent = urgent, .take_hold = take_hold};
    const rdof_txp_take_t got = rdof_txpool_take(&sr->pool, &rq, now);
    if (got.i < 0)
        return false;
    if (got.preempt && got.ended_path >= 0)
        rdof_router_drop(&sr->router, got.ended_path, got.ended);
    if (got.fresh) {
        const rdof_freq_t *f = &s->freq[p->tx_freq];
        rdof_tune_t tu = {.freq = f->hz, .cc = f->cc, .power = tune_level(sr, got.i, f->power_dbm, cap_dbm),
                          .preempt = got.preempt || path_preempt, .local = local};
        memcpy(tu.stream_id, o->stream_id, 4);
        sr->tuned[sr->tuned_next] = stream;
        sr->tuned_next = (sr->tuned_next + 1) % RDOF_SITEROUTE_TUNED;
        rdof_txvr_on_tune(sr->tx[got.i], &tu);
        if (local && rdof_txvr_refused(sr->tx[got.i], o->stream_id)) {
            /* an app's stream (a bench job's) the transceiver refused (busy receiving elsewhere, not ready, not
               allowed): not sent, so the app hears it now and not as a job that went nowhere */
            rdof_txpool_release(&sr->pool, got.i);
            return false;
        }
    }
    rdof_txvr_on_burst(sr->tx[got.i], o);
    if (o->terminator)
        rdof_txpool_end(&sr->pool, got.i, now);
    return true;
}
#endif

uint64_t rdof_siteroute_burst(rdof_siteroute_t *sr, int net, uint32_t peer, const rdof_net_burst_t *b, double now)
{
    const rdof_site_t *s = sr->router.site;
    uint64_t only = ~0ull;
    if (peer && net >= 0 && net < s->nnet && s->net[net].per_path) {
        only = 0;
        for (int k = 0; k < s->npath; k++)
            if (s->path[k].peer_id == peer)
                only |= 1ull << k;
    }
    return rdof_siteroute_burst_on(sr, net, only, false, b, now);
}

uint64_t rdof_siteroute_burst_on(rdof_siteroute_t *sr, int net, uint64_t only, bool emergency,
                                 const rdof_net_burst_t *b, double now)
{
    const rdof_site_t *s = sr->router.site;
    const uint32_t stream = sid32(b->stream_id);
    if (emergency && stream)
        sr->stream_emergency = stream;
    const bool data_sync = (b->control & RDOF_SYNC_DATA) != 0;
    if (data_sync && (b->control & 0x0F) == RDOF_DT_VOICE_LC_HEADER) {
        rdof_full_lc_t lc;
        if (rdof_decode_voice_lc(b->burst, RDOF_DT_VOICE_LC_HEADER, &lc) && (lc.service_options & 0x80))
            sr->stream_emergency = stream;
    }
    const rdof_rcall_t c = {.src = b->src, .dst = b->dst, .group = b->group,
                            .emergency = stream && sr->stream_emergency == stream};
    uint64_t preempt = 0;
    const uint64_t mask = rdof_router_net(&sr->router, net, only, stream, &c, b->terminator, now, &preempt);
    uint64_t out = 0;
    for (int k = 0; k < s->npath; k++) {
        if (!(mask >> k & 1))
            continue;
        const rdof_path_t *p = &s->path[k];
        rdof_net_burst_t o = *b;
        o.slot = p->tx_slot ? p->tx_slot : b->slot;
        const bool by_freq = p->app < 0 && s->freq[p->tx_freq].mode != RDOF_FMODE_SIMPLEX;
        if (p->app >= 0 || by_freq) {
#if RDOF_FEATURE_REPEATER
            struct rdof_repeater *r = by_freq ? sr->rpt_freq[p->tx_freq] : sr->rpt[p->app];
            if (r) {
                if (preempt >> k & 1)
                    rdof_repeater_preempt(r, b->stream_id);
                rdof_repeater_on_burst(r, &o);
                out |= 1ull << k;
                continue;
            }
#endif
            rdof_router_drop(&sr->router, k, stream);
            continue;
        }
#if RDOF_FEATURE_TX
        if (!send_txvr(sr, k, &o, c.emergency, true, (preempt >> k & 1) != 0, false, INFINITY, now)) {
            sr->no_txvr++;
            rdof_router_drop(&sr->router, k, stream);
            continue;
        }
        out |= 1ull << k;
#else
        rdof_router_drop(&sr->router, k, stream);
#endif
    }
    return out;
}

int rdof_siteroute_path_at(const rdof_siteroute_t *sr, uint32_t hz, int slot)
{
    return rdof_site_path_rx(sr->router.site, hz, slot);
}

/* An app's path: one a transceiver may serve (a simplex frequency's). */
static bool app_path(const rdof_siteroute_t *sr, int path)
{
    const rdof_site_t *s = sr->router.site;
    return path >= 0 && path < s->npath && s->path[path].app < 0 &&
           s->freq[s->path[path].tx_freq].mode == RDOF_FMODE_SIMPLEX && s->freq[s->path[path].tx_freq].tx;
}

bool rdof_siteroute_app_ready(const rdof_siteroute_t *sr, int path, double now)
{
    if (!app_path(sr, path))
        return false;
    const rdof_path_t *p = &sr->router.site->path[path];
    const rdof_txp_req_t rq = {.stream = 1, .path = path, .freq = p->tx_freq};
    return rdof_txpool_peek(&sr->pool, &rq, now).i >= 0;
}

bool rdof_siteroute_holds(const rdof_siteroute_t *sr, const uint8_t sid[4])
{
#if RDOF_FEATURE_TX
    for (int i = 0; i < RDOF_SITEROUTE_TXVRS; i++)
        if (sr->tx[i] && rdof_txvr_holds(sr->tx[i], sid))
            return true;
#endif
    return false;
}

bool rdof_siteroute_refused(const rdof_siteroute_t *sr, const uint8_t sid[4])
{
#if RDOF_FEATURE_TX
    for (int i = 0; i < RDOF_SITEROUTE_TXVRS; i++)
        if (sr->tx[i] && rdof_txvr_refused(sr->tx[i], sid))
            return true;
#endif
    return false;
}

bool rdof_siteroute_app_burst(rdof_siteroute_t *sr, int path, const rdof_net_burst_t *b, bool take_hold,
                              double cap_dbm, double now)
{
#if RDOF_FEATURE_TX
    if (app_path(sr, path)) {
        const rdof_path_t *p = &sr->router.site->path[path];
        rdof_net_burst_t o = *b;
        o.slot = p->tx_slot ? p->tx_slot : b->slot;
        if (send_txvr(sr, path, &o, false, take_hold, false, true, cap_dbm, now))
            return true;
    }
#endif
    sr->app_no_txvr++;
    return false;
}

bool rdof_siteroute_dmrd(rdof_siteroute_t *sr, int net, uint32_t peer, const uint8_t *d, size_t n, double now)
{
    rdof_net_burst_t b;
    if (rdof_hb_kind(d, n) != 'D' || !rdof_parse_dmrd(d, n, &b))
        return false;
    rdof_siteroute_burst(sr, net, peer, &b, now);
    return true;
}

size_t rdof_siteroute_calls_topic(const char *root, const char *box, const uint8_t *d, size_t n, char *out, size_t cap)
{
    if (n < RDOF_DMRD_LEN || memcmp(d, "DMRD", 4))
        return 0;
    const unsigned dst = (unsigned)d[8] << 16 | (unsigned)d[9] << 8 | d[10];
    const int k = snprintf(out, cap, "%s/calls/%s/%u/%s", root, (d[15] & 0x40) ? "pc" : "tg", dst, box);
    return k > 0 && (size_t)k < cap ? (size_t)k : 0;
}

bool rdof_siteroute_calls_theirs(const char *topic, const char *root, const char *box)
{
    const size_t rl = strlen(root);
    if (strncmp(topic, root, rl) || strncmp(topic + rl, "/calls/", 7))
        return false;
    const char *t = topic + rl + 7;
    if (strncmp(t, "tg/", 3) && strncmp(t, "pc/", 3))
        return false;
    const char *b = strchr(t + 3, '/');
    return b && b > t + 3 && b[1] && strcmp(b + 1, box);
}
