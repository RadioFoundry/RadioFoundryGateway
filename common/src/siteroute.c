// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The router on a running box (rdof/siteroute.h). */
#include "rdof/siteroute.h"

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
}

bool rdof_siteroute_add_repeater(rdof_siteroute_t *sr, int app, struct rdof_repeater *r)
{
    const rdof_site_t *s = sr->router.site;
    if (app < 0 || app >= s->napp || s->app[app].type != RDOF_APP_REPEATER)
        return false;
    sr->rpt[app] = r;
    return true;
}

bool rdof_siteroute_add_txvr(rdof_siteroute_t *sr, struct rdof_txvr *t, uint64_t freqs)
{
    if (sr->ntx >= RDOF_SITEROUTE_TXVRS)
        return false;
    sr->tx[sr->ntx++] = (rdof_siteroute_txvr_t){.t = t, .freqs = freqs ? freqs : ~0ull};
    return true;
}

int rdof_siteroute_rf(rdof_siteroute_t *sr, uint32_t hz, int slot, uint32_t src, uint32_t dst, bool group, double now)
{
    const rdof_rcall_t c = {.src = src, .dst = dst, .group = group};
    const rdof_route_t r = rdof_router_rf(&sr->router, rdof_site_path_rx(sr->router.site, hz, slot), &c, now);
    sr->why = r.kind == RDOF_ROUTE_NONE ? r.why : NULL;
    return r.kind == RDOF_ROUTE_NET ? r.net : -1;
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
/* The transceiver for path k sending stream (already, or free now), or -1. */
static int txvr_for(rdof_siteroute_t *sr, int k, uint32_t stream, double now)
{
    const rdof_path_t *p = &sr->router.site->path[k];
    int free = -1;
    for (int i = 0; i < sr->ntx; i++) {
        rdof_siteroute_txvr_t *x = &sr->tx[i];
        if (!(x->freqs >> p->tx_freq & 1))
            continue;
        if (x->stream == stream && x->path == k)
            return i;
        if (x->stream && now - x->last > RDOF_ROUTER_IDLE_S)
            x->stream = 0;
        if (!x->stream && free < 0)
            free = i;
    }
    return free;
}
#endif

uint64_t rdof_siteroute_burst(rdof_siteroute_t *sr, int net, uint32_t peer, const rdof_net_burst_t *b, double now)
{
    const rdof_site_t *s = sr->router.site;
    const uint32_t stream = sid32(b->stream_id);
    const bool data_sync = (b->control & RDOF_SYNC_DATA) != 0;
    if (data_sync && (b->control & 0x0F) == RDOF_DT_VOICE_LC_HEADER) {
        rdof_full_lc_t lc;
        if (rdof_decode_voice_lc(b->burst, RDOF_DT_VOICE_LC_HEADER, &lc) && (lc.service_options & 0x80))
            sr->stream_emergency = stream;
    }
    const rdof_rcall_t c = {.src = b->src, .dst = b->dst, .group = b->group,
                            .emergency = stream && sr->stream_emergency == stream};
    uint64_t only = ~0ull, preempt = 0;
    if (peer && net >= 0 && net < s->nnet && s->net[net].per_path) {
        only = 0;
        for (int k = 0; k < s->npath; k++)
            if (s->path[k].peer_id == peer)
                only |= 1ull << k;
    }
    const uint64_t mask = rdof_router_net(&sr->router, net, only, stream, &c, b->terminator, now, &preempt);
    uint64_t out = 0;
    for (int k = 0; k < s->npath; k++) {
        if (!(mask >> k & 1))
            continue;
        const rdof_path_t *p = &s->path[k];
        rdof_net_burst_t o = *b;
        o.slot = p->tx_slot ? p->tx_slot : b->slot;
        if (p->app >= 0) {
#if RDOF_FEATURE_REPEATER
            struct rdof_repeater *r = sr->rpt[p->app];
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
        const int i = txvr_for(sr, k, stream, now);
        if (i < 0) {
            sr->no_txvr++;
            rdof_router_drop(&sr->router, k, stream);
            continue;
        }
        rdof_siteroute_txvr_t *x = &sr->tx[i];
        if (x->stream != stream) {
            const rdof_freq_t *f = &s->freq[p->tx_freq];
            rdof_tune_t tu = {.freq = f->hz, .cc = f->cc, .power = f->power >= 0 ? f->power : 255,
                              .preempt = (preempt >> k & 1) != 0};
            memcpy(tu.stream_id, b->stream_id, 4);
            sr->tuned[sr->tuned_next] = stream;
            sr->tuned_next = (sr->tuned_next + 1) % RDOF_SITEROUTE_TUNED;
            x->stream = stream;
            x->path = k;
            rdof_txvr_on_tune(x->t, &tu);
        }
        x->last = now;
        rdof_txvr_on_burst(x->t, &o);
        if (b->terminator)
            x->stream = 0;
        out |= 1ull << k;
#else
        rdof_router_drop(&sr->router, k, stream);
#endif
    }
    return out;
}

bool rdof_siteroute_dmrd(rdof_siteroute_t *sr, int net, uint32_t peer, const uint8_t *d, size_t n, double now)
{
    rdof_net_burst_t b;
    if (rdof_hb_kind(d, n) != 'D' || !rdof_parse_dmrd(d, n, &b))
        return false;
    rdof_siteroute_burst(sr, net, peer, &b, now);
    return true;
}
