// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* An HBlink4 peer's state (hbpeer.h). */
#include "rdof/hbpeer.h"

#include <stdio.h>
#include <string.h>

#include "rdof/repeater.h"
#include "rdof/txvr.h"

void rdof_hb_peer_init(rdof_hb_peer_t *p, uint32_t peer_id, double keepalive_s)
{
    memset(p, 0, sizeof *p);
    p->peer_id = peer_id;
    p->keepalive_s = keepalive_s;
}

void rdof_hb_peer_link_up(rdof_hb_peer_t *p)
{
    p->next_ka = 0;
    p->last_pong = 0;
}

void rdof_hb_peer_link_down(rdof_hb_peer_t *p) { p->up = false; }

bool rdof_hb_peer_set_config(rdof_hb_peer_t *p, const uint8_t *dmrc, size_t n, bool force, double now)
{
    if (n > sizeof p->dmrc)
        n = sizeof p->dmrc;
    const bool changed = n != p->dmrc_len || memcmp(dmrc, p->dmrc, n);
    memcpy(p->dmrc, dmrc, n);
    p->dmrc_len = n;
    if (!(changed || force))
        return false;
    p->next_ka = now + p->keepalive_s;
    return true;
}

bool rdof_hb_peer_tick(rdof_hb_peer_t *p, double now, rdof_hbp_event_t *ev)
{
    *ev = RDOF_HBP_NONE;
    if (now < p->next_ka)
        return false;
    p->next_ka = now + p->keepalive_s;
    if (p->up && now - p->last_pong > 3 * p->keepalive_s) {
        p->up = false;
        *ev = RDOF_HBP_QUIET;
    }
    return true;
}

rdof_hbp_event_t rdof_hb_peer_received(rdof_hb_peer_t *p, const uint8_t *d, size_t n, double now)
{
    p->rx++;
    switch (rdof_hb_kind(d, n)) {
    case 'P': {
        const bool was = p->up;
        p->up = true;
        p->last_pong = now;
        return was ? RDOF_HBP_NONE : RDOF_HBP_REGISTERED;
    }
    case 'N':
        p->up = false;
        p->refused++;
        return RDOF_HBP_REFUSED;
    default:
        return RDOF_HBP_PACKET;
    }
}

size_t rdof_hb_role_dmrc(const rdof_config_t *c, uint32_t freq_now, int cc_now, uint8_t out[RDOF_DMRC_MAX])
{
    const rdof_location_t *loc = c->has_location ? &c->location : NULL;
    if (c->mode == RDOF_CFG_REPEATER) {
        const char slots = c->slot1 && c->slot2 ? '3' : c->slot1 ? '1' : c->slot2 ? '2' : '0';
        return rdof_dmrc_packet(c->peer_id, c->callsign, c->rx_freq, c->tx_freq, c->cc, slots, c->power,
                                c->sfr    ? "RadioFoundry Gateway SFR repeater"
                                : c->dcdm ? "RadioFoundry Gateway DCDM gateway"
                                          : "RadioFoundry Gateway repeater",
                                c->sfr    ? "radiofoundry_gateway sfr repeater"
                                : c->dcdm ? "radiofoundry_gateway dcdm"
                                          : "radiofoundry_gateway duplex repeater",
                                loc, NULL, NULL, out);
    }
    const uint32_t idle = c->idle.freq;
    const int idle_cc = c->idle.cc;
    return rdof_dmrc_packet(c->peer_id, c->callsign, freq_now, freq_now, cc_now, '4', 0, "RadioFoundry Gateway roaming",
                            "radiofoundry_gateway simplex", loc, &idle, &idle_cc, out);
}

size_t rdof_hb_channel_dmrc(uint32_t peer_id, const char *id, uint32_t freq, int cc, const rdof_location_t *loc,
                            uint8_t out[RDOF_DMRC_MAX])
{
    char callsign[9];
    snprintf(callsign, sizeof callsign, "%.8s", id);
    return rdof_dmrc_packet(peer_id, callsign, freq, freq, cc >= 0 ? cc : 1, '3', 0, "radiofoundry-gateway",
                            "RadioFoundry_Gateway_SDR", loc, NULL, NULL, out);
}

bool rdof_hb_dispatch(const uint8_t *d, size_t n, struct rdof_repeater *rpt, struct rdof_txvr *txvr,
                      uint32_t peer_id, uint8_t dmrk[RDOF_DMRK_LEN])
{
    const char kind = rdof_hb_kind(d, n);
    rdof_net_burst_t b;
    rdof_tune_t t;
#if RDOF_FEATURE_TX
    if (txvr) {
        if (kind == 'T' && rdof_parse_dmrt(d, n, &t))
            rdof_txvr_on_tune(txvr, &t);
        else if (kind == 'D' && rdof_parse_dmrd(d, n, &b))
            rdof_txvr_on_burst(txvr, &b);
        return false;
    }
#else
    (void)txvr;
#endif
#if RDOF_FEATURE_REPEATER
    if (rpt && kind == 'D' && rdof_parse_dmrd(d, n, &b))
        rdof_repeater_on_burst(rpt, &b);
#else
    (void)rpt;
    (void)b;
#endif
    if (kind == 'T' && !rpt && rdof_parse_dmrt(d, n, &t)) { /* nothing here transmits: refused */
        rdof_dmrk_packet(peer_id, t.stream_id, RDOF_DMRK_RADIO_ERROR, t.freq, t.cc, dmrk);
        return true;
    }
    return false;
}
