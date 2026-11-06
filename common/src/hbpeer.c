// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* An HBlink4 peer's state (hbpeer.h). */
#include "rdof/hbpeer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/repeater.h"
#include "rdof/sha256.h"
#include "rdof/txvr.h"

void rdof_hb_peer_init(rdof_hb_peer_t *p, uint32_t peer_id, double keepalive_s)
{
    memset(p, 0, sizeof *p);
    p->peer_id = peer_id;
    p->keepalive_s = keepalive_s;
}

static void copy_cut(char *dst, size_t cap, const char *src) { snprintf(dst, cap, "%s", src ? src : ""); }

void rdof_hb_peer_set_login(rdof_hb_peer_t *p, const char *password, const char *location, const char *description,
                            const char *url)
{
    p->login = true;
    copy_cut(p->password, sizeof p->password, password);
    copy_cut(p->location, sizeof p->location, location);
    copy_cut(p->description, sizeof p->description, description);
    copy_cut(p->url, sizeof p->url, url);
    p->state = RDOF_HBL_LOGIN;
    p->pending = true;
    p->backoff_s = 1.0;
}

/* The login from the top: RPTL now. */
static void login_restart(rdof_hb_peer_t *p, double now)
{
    p->relogin = false;
    p->state = RDOF_HBL_LOGIN;
    p->pending = true;
    p->next_ka = now;
}

/* Give up this try: start again after the back-off, which doubles up to RDOF_HB_BACKOFF_MAX_S. */
static void login_backoff(rdof_hb_peer_t *p, double now, bool longest)
{
    if (longest || p->backoff_s > RDOF_HB_BACKOFF_MAX_S)
        p->backoff_s = RDOF_HB_BACKOFF_MAX_S;
    if (p->backoff_s < 1.0)
        p->backoff_s = 1.0;
    p->up = false;
    p->state = RDOF_HBL_BACKOFF;
    p->pending = false;
    p->next_ka = now + p->backoff_s;
    p->backoff_s *= 2;
}

void rdof_hb_peer_link_up(rdof_hb_peer_t *p)
{
    p->next_ka = 0;
    p->last_pong = 0;
    if (p->login) {
        p->state = RDOF_HBL_LOGIN;
        p->pending = true;
    }
}

void rdof_hb_peer_link_down(rdof_hb_peer_t *p)
{
    p->up = false;
    if (p->login) {
        p->state = RDOF_HBL_LOGIN;
        p->pending = true;
    }
}

void rdof_hb_peer_set_power(rdof_hb_peer_t *p, double dbm, double now)
{
    const int v = !(dbm >= 0) ? 0 : dbm >= 99 ? 99 : (int)lround(dbm);
    if (v == p->power_dbm)
        return;
    p->power_dbm = v;
    if (p->login && p->dmrc_len && (p->state == RDOF_HBL_CONFIG || p->state == RDOF_HBL_UP)) {
        p->relogin = true;
        p->next_ka = now;
    }
}

bool rdof_hb_peer_set_config(rdof_hb_peer_t *p, const uint8_t *dmrc, size_t n, bool force, double now)
{
    if (n > sizeof p->dmrc)
        n = sizeof p->dmrc;
    const bool had = p->dmrc_len > 0;
    const bool changed = n != p->dmrc_len || memcmp(dmrc, p->dmrc, n);
    memcpy(p->dmrc, dmrc, n);
    p->dmrc_len = n;
    if (p->login) {
        /* the master took the old RPTC, or is about to: log in again with the new one, at the next tick (this call
           may be inside one, whose packet goes now) */
        if (changed && had && (p->state == RDOF_HBL_CONFIG || p->state == RDOF_HBL_UP)) {
            p->relogin = true;
            p->next_ka = now;
        }
        return false;
    }
    if (!(changed || force))
        return false;
    p->next_ka = now + p->keepalive_s;
    return true;
}

static bool login_tick(rdof_hb_peer_t *p, double now, rdof_hbp_event_t *ev)
{
    if (now < p->next_ka)
        return false;
    if (p->relogin) {
        p->relogin = false;
        p->up = false;
        if (p->state == RDOF_HBL_CONFIG || p->state == RDOF_HBL_UP)
            login_restart(p, now);
    }
    switch (p->state) {
    case RDOF_HBL_BACKOFF:
        login_restart(p, now);
        /* fall through */
    case RDOF_HBL_LOGIN:
    case RDOF_HBL_AUTH:
    case RDOF_HBL_CONFIG:
        if (p->pending) {
            p->pending = false;
            p->next_ka = now + RDOF_HB_LOGIN_TIMEOUT_S;
            if (p->state == RDOF_HBL_LOGIN)
                p->logins++;
            return true;
        }
        login_backoff(p, now, false); /* no answer to this step */
        return false;
    case RDOF_HBL_UP:
    default:
        if (now - p->last_pong > 3 * p->keepalive_s + 0.5) {
            p->pings_missed++;
            *ev = RDOF_HBP_QUIET;
            login_backoff(p, now, false);
            return false;
        }
        p->next_ka = now + p->keepalive_s;
        return true;
    }
}

bool rdof_hb_peer_tick(rdof_hb_peer_t *p, double now, rdof_hbp_event_t *ev)
{
    *ev = RDOF_HBP_NONE;
    if (p->login)
        return login_tick(p, now, ev);
    if (now < p->next_ka)
        return false;
    p->next_ka = now + p->keepalive_s;
    if (p->up && now - p->last_pong > 3 * p->keepalive_s) {
        p->up = false;
        *ev = RDOF_HBP_QUIET;
    }
    return true;
}

static void put_be32(uint8_t *o, uint32_t v)
{
    o[0] = (uint8_t)(v >> 24);
    o[1] = (uint8_t)(v >> 16);
    o[2] = (uint8_t)(v >> 8);
    o[3] = (uint8_t)v;
}

size_t rdof_hb_peer_packet(const rdof_hb_peer_t *p, uint8_t out[RDOF_HB_OUT_MAX])
{
    if (!p->login) {
        memcpy(out, p->dmrc, p->dmrc_len);
        return p->dmrc_len;
    }
    switch (p->state) {
    case RDOF_HBL_AUTH: {
        uint8_t hash[32];
        rdof_sha256_t h;
        rdof_sha256_init(&h);
        rdof_sha256_update(&h, p->salt, sizeof p->salt);
        rdof_sha256_update(&h, p->password, strlen(p->password));
        rdof_sha256_final(&h, hash);
        memcpy(out, "RPTK", 4);
        put_be32(out + 4, p->peer_id);
        memcpy(out + 8, hash, 32);
        return 40;
    }
    case RDOF_HBL_CONFIG: {
        size_t n = rdof_rptc_from_dmrc(p->dmrc, p->dmrc_len, p->power_dbm, p->location, p->description, p->url, out);
        put_be32(out + 4, p->peer_id);
        return n;
    }
    case RDOF_HBL_UP:
        memcpy(out, "RPTPING", 7);
        put_be32(out + 7, p->peer_id);
        return 11;
    default:
        memcpy(out, "RPTL", 4);
        put_be32(out + 4, p->peer_id);
        return 8;
    }
}

size_t rdof_hb_peer_close_packet(const rdof_hb_peer_t *p, uint8_t out[9])
{
    if (!p->login)
        return 0;
    memcpy(out, "RPTCL", 5);
    put_be32(out + 5, p->peer_id);
    return 9;
}

bool rdof_hb_peer_may_send(const rdof_hb_peer_t *p) { return !p->login || p->state == RDOF_HBL_UP; }

const char *rdof_hb_peer_state_name(const rdof_hb_peer_t *p)
{
    if (!p->login)
        return p->up ? "up" : "down";
    switch (p->state) {
    case RDOF_HBL_UP:
        return "up";
    case RDOF_HBL_AUTH:
        return "auth";
    case RDOF_HBL_CONFIG:
        return "config";
    case RDOF_HBL_BACKOFF:
        return p->bad_password ? "bad_password" : p->refused && p->refused_at != RDOF_HBL_UP ? "refused" : "backoff";
    default:
        return "login";
    }
}

static rdof_hbp_event_t login_received(rdof_hb_peer_t *p, const uint8_t *d, size_t n, double now)
{
    switch (rdof_hb_kind(d, n)) {
    case 'A': /* RPTACK: the step before was taken */
        if (p->pending)
            return RDOF_HBP_NONE; /* an old answer: this step isn't sent yet */
        if (p->state == RDOF_HBL_LOGIN) {
            if (n < 10)
                return RDOF_HBP_NONE;
            memcpy(p->salt, d + 6, 4);
            p->state = RDOF_HBL_AUTH;
        } else if (p->state == RDOF_HBL_AUTH) {
            p->state = RDOF_HBL_CONFIG;
        } else if (p->state == RDOF_HBL_CONFIG) {
            p->state = RDOF_HBL_UP;
            p->up = true;
            p->last_pong = now;
            p->next_ka = now + p->keepalive_s;
            p->backoff_s = 1.0;
            p->bad_password = false;
            return RDOF_HBP_REGISTERED;
        } else {
            return RDOF_HBP_NONE;
        }
        p->pending = true;
        p->next_ka = now;
        return RDOF_HBP_NONE;
    case 'G': /* MSTPONG */
        if (p->state == RDOF_HBL_UP)
            p->last_pong = now;
        return RDOF_HBP_NONE;
    case 'N': /* MSTNAK, MSTCL */
        if (p->state == RDOF_HBL_BACKOFF)
            return RDOF_HBP_NONE; /* already given up */
        p->refused++;
        p->refused_at = p->state;
        p->bad_password = p->state == RDOF_HBL_AUTH;
        login_backoff(p, now, p->bad_password);
        return RDOF_HBP_REFUSED;
    case 'D':
        return p->state == RDOF_HBL_UP ? RDOF_HBP_PACKET : RDOF_HBP_NONE;
    default:
        return RDOF_HBP_NONE; /* RPTSBKN and the rest */
    }
}

rdof_hbp_event_t rdof_hb_peer_received(rdof_hb_peer_t *p, const uint8_t *d, size_t n, double now)
{
    p->rx++;
    if (p->login)
        return login_received(p, d, n, now);
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

/* field of DMRC at off, len (space when the DMRC is shorter) */
static void dmrc_field(const uint8_t *d, size_t n, size_t off, size_t len, uint8_t *out)
{
    for (size_t i = 0; i < len; i++)
        out[i] = off + i < n ? d[off + i] : ' ';
}

static void text_field(uint8_t *out, size_t len, const char *s)
{
    const size_t k = s ? strlen(s) : 0;
    for (size_t i = 0; i < len; i++)
        out[i] = i < k ? (uint8_t)s[i] : ' ';
}

/* The DMRC's coordinate at off, len (HBlink4's, "33.6198 "), as the spec writes it: a sign, zero padded, 4 decimals */
static void coord_field(const uint8_t *d, size_t n, size_t off, size_t len, uint8_t *out)
{
    char s[16] = {0}, w[24];
    for (size_t i = 0; i < len && off + i < n; i++)
        s[i] = (char)d[off + i];
    double v = atof(s);
    const double lim = len == 8 ? 90.0 : 180.0;
    if (!(v >= -lim && v <= lim))
        v = 0.0;
    snprintf(w, sizeof w, "%+0*.4f", (int)len, v);
    memcpy(out, w, len);
}

size_t rdof_rptc_from_dmrc(const uint8_t *d, size_t n, int power_dbm, const char *location, const char *description,
                           const char *url, uint8_t out[RDOF_RPTC_LEN])
{
    char pw[4];
    memcpy(out, "RPTC", 4);
    dmrc_field(d, n, 4, 4, out + 4);      /* ID */
    dmrc_field(d, n, 8, 8, out + 8);      /* callsign */
    dmrc_field(d, n, 16, 9, out + 16);    /* RX Hz */
    dmrc_field(d, n, 25, 9, out + 25);    /* TX Hz */
    snprintf(pw, sizeof pw, "%02d", power_dbm < 0 ? 0 : power_dbm > 99 ? 99 : power_dbm);
    memcpy(out + 34, pw, 2);              /* power, dBm */
    dmrc_field(d, n, 36, 2, out + 36);    /* color code */
    if (n >= 139) {
        coord_field(d, n, 119, 8, out + 38); /* latitude */
        coord_field(d, n, 127, 9, out + 46); /* longitude */
        dmrc_field(d, n, 136, 3, out + 55);  /* height, m */
    } else {
        memcpy(out + 38, "+00.0000+000.0000000", 20);
    }
    text_field(out + 58, 20, location);
    text_field(out + 78, 19, description);
    dmrc_field(d, n, 38, 1, out + 97);    /* slots */
    text_field(out + 98, 124, url);
    dmrc_field(d, n, 39, 40, out + 222);  /* software ID: the DMRC's version */
    dmrc_field(d, n, 79, 40, out + 262);  /* package ID: the DMRC's software */
    return RDOF_RPTC_LEN;
}

double rdof_hb_role_power_dbm(const rdof_config_t *c, uint32_t freq_now)
{
    if (!rdof_txmap_valid(&c->tx_map))
        return NAN;
    int level = c->power;
    if (c->mode != RDOF_CFG_REPEATER && c->nchannels > 0) {
        level = c->channels[0].power;
        for (int i = 0; i < c->nchannels; i++)
            if (c->channels[i].freq == freq_now)
                level = c->channels[i].power;
    }
    return rdof_txpower_dbm(&c->tx_map, level);
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
