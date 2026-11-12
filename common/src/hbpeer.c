// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* An HBlink4 peer's state (hbpeer.h). */
#include "rdof/hbpeer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/bits.h"
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

void rdof_hb_peer_set_dl5di(rdof_hb_peer_t *p, bool on) { p->dl5di = on; }

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

/* The peer ID after a login packet's command: 4 bytes, or 8 hex characters (dl5di). Returns the length. */
static size_t put_id(const rdof_hb_peer_t *p, uint8_t *o)
{
    if (!p->dl5di) {
        put_be32(o, p->peer_id);
        return 4;
    }
    char h[9];
    snprintf(h, sizeof h, "%08lX", (unsigned long)p->peer_id);
    memcpy(o, h, 8);
    return 8;
}

/* command + the peer ID; returns the length */
static size_t cmd_id(const rdof_hb_peer_t *p, const char *cmd, uint8_t *o)
{
    const size_t k = strlen(cmd);
    memcpy(o, cmd, k);
    return k + put_id(p, o + k);
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
        rdof_sha256_update(&h, p->salt, p->salt_len); /* dl5di: the 8 hex characters as received */
        rdof_sha256_update(&h, p->password, strlen(p->password));
        rdof_sha256_final(&h, hash);
        size_t k = cmd_id(p, "RPTK", out);
        if (!p->dl5di) {
            memcpy(out + k, hash, 32);
            return k + 32;
        }
        for (int i = 0; i < 32; i++) {
            static const char hx[] = "0123456789abcdef";
            out[k + 2 * i] = (uint8_t)hx[hash[i] >> 4];
            out[k + 2 * i + 1] = (uint8_t)hx[hash[i] & 15];
        }
        return k + 64;
    }
    case RDOF_HBL_CONFIG: {
        if (p->dl5di)
            return rdof_rptc_dl5di_from_dmrc(p->dmrc, p->dmrc_len, p->peer_id, p->power_dbm, p->location,
                                             p->description, p->url, out);
        size_t n = rdof_rptc_from_dmrc(p->dmrc, p->dmrc_len, p->power_dbm, p->location, p->description, p->url, out);
        put_be32(out + 4, p->peer_id);
        return n;
    }
    case RDOF_HBL_UP:
        return cmd_id(p, p->dl5di ? "MSTPING" : "RPTPING", out);
    default:
        return cmd_id(p, "RPTL", out);
    }
}

size_t rdof_hb_peer_close_packet(const rdof_hb_peer_t *p, uint8_t out[RDOF_HB_CLOSE_MAX])
{
    if (!p->login)
        return 0;
    return cmd_id(p, "RPTCL", out);
}

/* bit 7 slot, 6 call type, 5-4 frame type, 3-0 data type  <->  bit 0 slot, 1 call type, 3-2 frame type, 7-4 data
   type (the 2015 specification's DMRD table) */
uint8_t rdof_dmrd_flags_to_dl5di(uint8_t f)
{
    return (uint8_t)(((f >> 7) & 1) | (((f >> 6) & 1) << 1) | (((f >> 4) & 3) << 2) | ((f & 15) << 4));
}

uint8_t rdof_dmrd_flags_from_dl5di(uint8_t f)
{
    return (uint8_t)(((f & 1) << 7) | (((f >> 1) & 1) << 6) | (((f >> 2) & 3) << 4) | (f >> 4));
}

const uint8_t *rdof_hb_peer_up(const rdof_hb_peer_t *p, const uint8_t *d, size_t *n, uint8_t tmp[RDOF_HB_OUT_MAX])
{
    if (!p->login || *n < RDOF_DMRD_DL5DI_LEN || *n > RDOF_HB_OUT_MAX || memcmp(d, "DMRD", 4))
        return d;
    const bool simplex = p->dmrc_len > 38 && p->dmrc[38] == '4';
    if (!simplex && !p->dl5di)
        return d;
    memcpy(tmp, d, *n);
    if (simplex)
        tmp[15] |= 0x80;
    if (p->dl5di) {
        tmp[15] = rdof_dmrd_flags_to_dl5di(tmp[15]);
        *n = RDOF_DMRD_DL5DI_LEN; /* no BER, RSSI */
    }
    return tmp;
}

const uint8_t *rdof_hb_peer_down(rdof_hb_peer_t *p, const uint8_t *d, size_t *n, double now,
                                 uint8_t tmp[RDOF_DMRD_LEN])
{
    if (!p->login || !p->dl5di || *n < RDOF_DMRD_DL5DI_LEN || memcmp(d, "DMRD", 4))
        return d;
    memcpy(tmp, d, RDOF_DMRD_DL5DI_LEN);
    tmp[53] = tmp[54] = 0;
    const uint8_t f = rdof_dmrd_flags_from_dl5di(d[15]);
    tmp[15] = f;
    *n = RDOF_DMRD_LEN;
    if (rdof_be32(d + 16) != 0)
        return tmp;
    /* the master's stream ID is 0: make one per call */
    const int s = (f >> 7) & 1;
    const uint32_t src = ((uint32_t)d[5] << 16) | ((uint32_t)d[6] << 8) | d[7];
    const uint32_t dst = ((uint32_t)d[8] << 16) | ((uint32_t)d[9] << 8) | d[10];
    if (!p->down_stream[s] || p->down_ended[s] || src != p->down_src[s] || dst != p->down_dst[s] ||
        now - p->down_last[s] > 2.0) {
        p->down_count++;
        p->down_stream[s] = (p->peer_id ^ (p->down_count * 0x9E3779B9u)) | 1u;
        p->down_src[s] = src;
        p->down_dst[s] = dst;
    }
    p->down_last[s] = now;
    p->down_ended[s] = ((f >> 4) & 3) == 2 && (f & 15) == 2; /* a terminator: the next packet starts a call */
    put_be32(tmp + 16, p->down_stream[s]);
    return tmp;
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

size_t rdof_hb_peer_login_status(const rdof_hb_peer_t *p, char *out, size_t cap)
{
    static const char *step[] = {"login", "auth", "config", "up", "backoff"};
    if (!cap)
        return 0;
    out[0] = 0;
    if (!p->login)
        return 0;
    const char *at = p->refused && p->refused_at < 5 ? step[p->refused_at] : "";
    char hex[2 * sizeof p->refusal + 1];
    for (size_t i = 0; i < p->refusal_len; i++)
        snprintf(hex + 2 * i, 3, "%02x", p->refusal[i]);
    hex[2 * p->refusal_len] = 0;
    int n = snprintf(out, cap,
                     "{\"state\":\"%s\",\"logins\":%u,\"refusals\":%u,\"refused_at\":\"%s\",\"went_quiet\":%u,"
                     "\"backoff_s\":%g,\"refusal\":\"%s\"}",
                     rdof_hb_peer_state_name(p), p->logins, p->refused, at, p->pings_missed, p->backoff_s, hex);
    return n < 0 ? 0 : (size_t)n >= cap ? cap - 1 : (size_t)n;
}

static rdof_hbp_event_t login_received(rdof_hb_peer_t *p, const uint8_t *d, size_t n, double now)
{
    switch (rdof_hb_kind(d, n)) {
    case 'A': /* RPTACK (dl5di: MSTACK): the step before was taken */
        if (p->pending)
            return RDOF_HBP_NONE; /* an old answer: this step isn't sent yet */
        if (p->state == RDOF_HBL_LOGIN) {
            /* RPTACK + 4 bytes; dl5di: MSTACK + the ID and the salt, 8 hex characters each */
            const size_t at = p->dl5di ? 14 : 6, len = p->dl5di ? 8 : 4;
            if (n < at + len)
                return RDOF_HBP_NONE;
            memcpy(p->salt, d + at, len);
            p->salt_len = (uint8_t)len;
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
    case 'G': /* MSTPONG (dl5di: RPTPONG) */
        if (p->state == RDOF_HBL_UP)
            p->last_pong = now;
        return RDOF_HBP_NONE;
    case 'N': /* MSTNAK, MSTCL */
        if (p->state == RDOF_HBL_BACKOFF)
            return RDOF_HBP_NONE; /* already given up */
        p->refused++;
        p->refused_at = p->state;
        p->refusal_len = (uint8_t)(n < sizeof p->refusal ? n : sizeof p->refusal);
        memcpy(p->refusal, d, p->refusal_len);
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

/* The DMRC's coordinate at off, len (HBlink4's, "33.6198 "), as hotspots send it to BrandMeister: "%0*f" (6 decimals,
   zero padded to len) cut to len characters: 0 is "0.000000" / "00.000000", 33.6198 "33.61980", -86.6089 "-86.60890" */
static void coord_field(const uint8_t *d, size_t n, size_t off, size_t len, uint8_t *out)
{
    char s[16] = {0}, w[24];
    for (size_t i = 0; i < len && off + i < n; i++)
        s[i] = (char)d[off + i];
    double v = atof(s);
    const double lim = len == 8 ? 90.0 : 180.0;
    if (!(v >= -lim && v <= lim))
        v = 0.0;
    snprintf(w, sizeof w, "%0*f", (int)len, v);
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
        memcpy(out + 38, "0.00000000.000000000", 20);
    }
    text_field(out + 58, 20, location);
    text_field(out + 78, 19, description);
    dmrc_field(d, n, 38, 1, out + 97);    /* slots */
    text_field(out + 98, 124, url);
    dmrc_field(d, n, 39, 40, out + 222);  /* software ID: the DMRC's version */
    text_field(out + 262, 40, "MMDVM");   /* package ID: BrandMeister refuses the RPTC with any other we tried */
    return RDOF_RPTC_LEN;
}

size_t rdof_rptc_dl5di_from_dmrc(const uint8_t *d, size_t n, uint32_t peer_id, int power_dbm, const char *location,
                                 const char *description, const char *url, uint8_t out[RDOF_RPTC_DL5DI_LEN])
{
    char w[24];
    memcpy(out, "RPTC", 4);
    dmrc_field(d, n, 8, 8, out + 4);   /* callsign */
    snprintf(w, sizeof w, "%08lX", (unsigned long)peer_id);
    memcpy(out + 12, w, 8);            /* ID, 8 hex characters as the specification's examples */
    dmrc_field(d, n, 16, 9, out + 20); /* RX Hz */
    dmrc_field(d, n, 25, 9, out + 29); /* TX Hz */
    snprintf(w, sizeof w, "%02d", power_dbm < 0 ? 0 : power_dbm > 99 ? 99 : power_dbm);
    memcpy(out + 38, w, 2);            /* power, dBm */
    dmrc_field(d, n, 36, 2, out + 40); /* color code */
    double lat = 0, lon = 0;
    int height = 0;
    if (n >= 139) {
        char s[16] = {0};
        memcpy(s, d + 119, 8);
        lat = atof(s);
        memset(s, 0, sizeof s);
        memcpy(s, d + 127, 9);
        lon = atof(s);
        memset(s, 0, sizeof s);
        memcpy(s, d + 136, 3);
        height = atoi(s);
    }
    if (!(lat >= -90.0 && lat <= 90.0))
        lat = 0;
    if (!(lon >= -180.0 && lon <= 180.0))
        lon = 0;
    snprintf(w, sizeof w, "%+08.4f", lat);
    memcpy(out + 42, w, 8);            /* latitude, "+33.6198" */
    snprintf(w, sizeof w, "%+09.4f", lon);
    memcpy(out + 50, w, 9);            /* longitude, "-086.6089" */
    snprintf(w, sizeof w, "%03d", height < 0 ? 0 : height > 999 ? 999 : height);
    memcpy(out + 59, w, 3);            /* height, m */
    text_field(out + 62, 20, location);
    text_field(out + 82, 20, description);
    text_field(out + 102, 124, url);
    dmrc_field(d, n, 39, 40, out + 226); /* software ID: the DMRC's version */
    dmrc_field(d, n, 79, 40, out + 266); /* package ID: the DMRC's software */
    return RDOF_RPTC_DL5DI_LEN;
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
    if (c->hb_login) /* BrandMeister: a hotspot on its one frequency */
        return rdof_dmrc_packet(c->peer_id, c->callsign, freq_now, freq_now, cc_now, '4', 0,
                                "RadioFoundry Gateway hotspot", "radiofoundry_gateway simplex", loc, NULL, NULL, out);
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
