// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* rdofradio's settings (rdofradio_cfg.h). */
#include "rdofradio_cfg.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/jitter.h"
#include "rdof/jsonp.h"
#include "rdof/siteplan.h"

/* ntp: a string of servers, or {servers, poll_s, clock}. The clock is netclock's when servers are set, unless
   clock is "os"; "netclock" without servers runs it on the browser's time alone. */
static bool clock_settings(const rdof_plan_t *p, rdofradio_cfg_t *o, char *err, size_t cap)
{
    const rdof_jdoc_t *d = rdof_plan_doc(p);
    const int ntp = rdof_jget(d, rdof_plan_root(p), "ntp");
    const rdof_jnode_t *n = ntp >= 0 ? rdof_jnode(d, ntp) : NULL;
    const char *mode = NULL;
    if (n && n->type == RDOF_JSTR) {
        snprintf(o->ntp_servers, sizeof o->ntp_servers, "%s", n->str);
    } else if (n && n->type == RDOF_JOBJ) {
        const int sv = rdof_jget(d, ntp, "servers"), ps = rdof_jget(d, ntp, "poll_s"), ck = rdof_jget(d, ntp, "clock");
        if (sv >= 0 && rdof_jnode(d, sv)->type == RDOF_JSTR)
            snprintf(o->ntp_servers, sizeof o->ntp_servers, "%s", rdof_jnode(d, sv)->str);
        if (ps >= 0 && rdof_jnode(d, ps)->type == RDOF_JNUM && rdof_jnode(d, ps)->num > 0) {
            const double v = rdof_jnode(d, ps)->num;
            o->ntp_poll_s = v < 16 ? 16 : v > 3600 ? 3600 : (unsigned)v;
        }
        if (ck >= 0 && rdof_jnode(d, ck)->type == RDOF_JSTR)
            mode = rdof_jnode(d, ck)->str;
    }
    if (mode && strcmp(mode, "netclock") && strcmp(mode, "os")) {
        snprintf(err, cap, "ntp.clock: netclock or os");
        return false;
    }
    o->own_clock = mode ? !strcmp(mode, "netclock") : o->ntp_servers[0] != 0;
    return true;
}

/* The SDR's channels as the board's SDR resolves them (rdof_plan_sdr): its receiver's frequencies, then the ones it
   hears for the units on it (the transceiver's, a repeater's input). Not a repeater's own output (full duplex: the
   SDR would hear what it sends). */
static bool channels(const rdof_plan_t *p, rdofradio_cfg_t *o, char *err, size_t cap)
{
    rdof_sdr_config_t *s = malloc(sizeof *s);
    if (!s) {
        snprintf(err, cap, "no memory");
        return false;
    }
    const bool ok = rdof_plan_sdr(p, NULL, s, err, cap);
    if (ok) { /* the SDR's MQTT names, as the board's: {root}/{channel}/{app}, {root}/_sdr/{name}/status */
        snprintf(o->sdr_name, sizeof o->sdr_name, "%s", s->name);
        snprintf(o->sdr_app, sizeof o->sdr_app, "%s", s->mqtt_app);
        o->sdr_status_s = s->status_seconds;
    }
    for (int i = 0; ok && i < s->nch; i++) {
        const uint32_t f = (uint32_t)llround(s->ch[i].freq);
        bool out = o->full && f == o->tx_hz && f != o->rx_hz;
        for (int k = 1; k < o->nsplit; k++) /* another split's output: its radio sends it */
            out |= f == o->split[k].tx_hz;
        if (!out)
            o->chan[o->nchan++] = s->ch[i];
    }
    free(s);
    return ok;
}

/* A unit that's a split repeater with the SDR as its RX half: into out, its TX half's port path from its device. */
static bool split_on_sdr(const rdof_plan_t *p, const char *name, rdofradio_split_t *out, char *err, size_t cap)
{
    memset(out, 0, sizeof *out);
    if (!rdof_plan_radio(p, name, &out->cfg, err, cap))
        return false;
    const rdof_config_t *c = &out->cfg;
    if (c->mode != RDOF_CFG_REPEATER || strcmp(c->modem_port, "split") || strcmp(c->split[1].modem_port, "sdr"))
        return false;
    snprintf(out->name, sizeof out->name, "%s", c->name);
    out->peer_id = c->peer_id, out->rx_hz = c->rx_freq, out->tx_hz = c->tx_freq, out->cc = c->cc;
    const int k = rdof_plan_unit_find(p, name);
    const rdof_unit_t *u = k >= 0 ? rdof_plan_unit(p, k) : NULL;
    const rdof_plan_dev_t *d = u && u->tx >= 0 ? rdof_plan_dev(p, u->tx) : NULL;
    if (d && d->port.kind == RDOF_PORT_PATH) {
        snprintf(out->port, sizeof out->port, "%s", d->port.path);
        out->baud = d->port.baud;
    }
    return true;
}

static bool from_plan(const rdof_plan_t *p, rdofradio_cfg_t *o, char *err, size_t cap)
{
    rdof_roles_t roles;
    if (!rdof_plan_check(p, &rdof_plan_host, err, cap) || !rdof_plan_roles_on(p, &rdof_plan_host, &roles, err, cap))
        return false;
    if (!roles.has_radio) {
        snprintf(err, cap, "nothing transmits: rdofradio runs a repeater, or a transceiver on the SDR");
        return false;
    }
    /* split repeaters on the SDR: as many as there are; anything else, one unit */
    for (int k = 0; k < roles.nradios; k++) {
        char why[160] = "";
        if (split_on_sdr(p, roles.radios[k], &o->split[o->nsplit], why, sizeof why)) {
            o->nsplit++;
        } else if (roles.nradios > 1) {
            snprintf(err, cap, "%s: rdofradio runs one unit that transmits, or split repeaters on the SDR (%s here)%s%s",
                     roles.radios[k ? k : 1], roles.radios[0], why[0] ? ": " : "", why);
            return false;
        }
    }
    for (int k = 1; k < o->nsplit; k++)
        if (!o->split[k].port[0]) {
            snprintf(err, cap, "%s: its radio needs a port path (port: {\"path\": ...}): --split-tx is the first's",
                     o->split[k].name);
            return false;
        }
    rdof_config_t *c = &o->cfg;
    if (!rdof_plan_radio(p, roles.radio, c, err, cap))
        return false;
    /* a unit on a modem (an MMDVM_HS hat, a hotspot) runs with --mmdvm PORT; a split's halves are the SDR and a
       radio (--split-tx) */
    o->on_modem = c->sdr_duplex == RDOF_SDR_DUPLEX_NONE && strcmp(c->modem_port, "split");
    if (c->sfr || c->dcdm) {
        snprintf(err, cap, "%s: mode %s isn't on rdofradio (duplex only)", c->name, c->sfr ? "sfr" : "dcdm");
        return false;
    }
    {
        /* rdofradio runs no IP gateway: the router would still take its ID's calls, and they'd go nowhere */
        const rdof_site_t *st = rdof_plan_site(p);
        const int ig = rdof_site_app_of(st, RDOF_APP_IPGW);
        if (ig >= 0) {
            snprintf(err, cap, "apps.%s: the IP gateway isn't on rdofradio (a board's app)", st->app[ig].name);
            return false;
        }
    }
    snprintf(o->box, sizeof o->box, "%s", roles.box);
    snprintf(o->name, sizeof o->name, "%s", c->name);
    o->peer_id = c->peer_id;
    o->has_parrot = c->has_parrot;
    o->parrot = c->parrot;
    if (c->mode == RDOF_CFG_REPEATER) {
        o->full = true;
        o->rx_hz = c->rx_freq;
        o->tx_hz = c->tx_freq;
        o->cc = c->cc;
        o->rx_slot = 1;
    } else {
        /* a half duplex transceiver: listens and sends on its idle frequency */
        o->full = false;
        o->rx_hz = o->tx_hz = c->idle.freq;
        o->cc = c->idle.cc;
        o->rx_slot = c->idle.slot == 2 ? 2 : 1;
    }
    o->has_hblink = rdof_plan_net_of(p, RDOF_NET_HBLINK) >= 0;
    {
        /* the unit's own peer: with one identity the box's peer carries every path; with a peer a path (identity
           per_path, a BrandMeister login) only when the unit's own path is on that network, or a unit on MQTT or on
           no network would log in beside them with the box's ID */
        const rdof_site_t *st = rdof_plan_site(p);
        const int hb = rdof_plan_net_of(p, RDOF_NET_HBLINK);
        o->unit_hblink = hb >= 0 && (c->receiver_hblink || !(st->net[hb].per_path || st->net[hb].login));
    }
    o->tx_power_dbm = NAN;
    const int uk = rdof_plan_unit_find(p, roles.radio);
    const rdof_unit_t *u = uk >= 0 ? rdof_plan_unit(p, uk) : NULL;
    const rdof_plan_dev_t *dv = u && c->sdr_duplex != RDOF_SDR_DUPLEX_NONE ? rdof_plan_dev(p, u->rx) : NULL;
    if (dv && dv->has_tx_power)
        o->tx_power_dbm = dv->tx_power_dbm;
    if (!clock_settings(p, o, err, cap))
        return false;
    if (roles.has_sdr && !channels(p, o, err, cap))
        return false;
    o->site = *rdof_plan_site(p);
    return true;
}

bool rdofradio_cfg_parse(const char *text, size_t len, rdofradio_cfg_t *o, char *err, size_t cap)
{
    memset(o, 0, sizeof *o);
    rdof_plan_t *p = rdof_plan_open(text, len, err, cap);
    if (!p)
        return false;
    const bool ok = from_plan(p, o, err, cap);
    rdof_plan_close(p);
    return ok;
}

int rdofradio_cfg_channel_at(const rdofradio_cfg_t *o, uint32_t hz)
{
    for (int i = 0; i < o->nchan; i++)
        if (fabs(o->chan[i].freq - (double)hz) <= 100)
            return i;
    return -1;
}

/* MHz with the trailing zeros off, 438.25 */
static void fmhz(char *out, size_t cap, uint32_t hz)
{
    snprintf(out, cap, "%.6f", hz / 1e6);
    char *e = out + strlen(out) - 1;
    while (*e == '0' && e[-1] != '.')
        *e-- = 0;
}

size_t rdofradio_cfg_from_flags(char *out, size_t cap, const char *driver, bool full, uint32_t rx_hz, uint32_t tx_hz,
                                int cc, bool has_parrot, uint32_t parrot, uint32_t peer_id)
{
    char rx[24], tx[24], app[160] = "", par[64] = "";
    fmhz(rx, sizeof rx, rx_hz);
    fmhz(tx, sizeof tx, tx_hz);
    /* a parrot app answers one ID: --parrot 0 (everything) has none */
    if (has_parrot && parrot)
        snprintf(par, sizeof par, "{\"type\": \"parrot\", \"tg\": %u}", (unsigned)parrot);
    if (full)
        snprintf(app, sizeof app, "{\"type\": \"repeater\", \"name\": \"rdofradio\", \"in\": \"in\", \"out\": \"out\"}%s%s",
                 par[0] ? ",\n    " : "", par);
    else
        snprintf(app, sizeof app, "%s", par);
    int n;
    if (full)
        n = snprintf(out, cap,
                     "{\n  \"version\": 3,\n  \"name\": \"rdofradio\",\n  \"id\": %u,\n"
                     "  \"devices\": [{\"name\": \"sdr\", \"kind\": \"sdr\", \"driver\": \"%s\"}],\n"
                     "  \"frequencies\": [{\"freq\": %s, \"name\": \"in\", \"cc\": %d}, {\"freq\": %s, \"name\": \"out\", "
                     "\"cc\": %d}],\n  \"apps\": [\n    %s\n  ]\n}\n",
                     (unsigned)peer_id, driver, rx, cc, tx, cc, app);
    else
        n = snprintf(out, cap,
                     "{\n  \"version\": 3,\n  \"name\": \"rdofradio\",\n  \"id\": %u,\n"
                     "  \"devices\": [{\"name\": \"rdofradio\", \"kind\": \"sdr\", \"driver\": \"%s\"}],\n"
                     "  \"frequencies\": [{\"freq\": %s, \"cc\": %d}]%s%s%s\n}\n",
                     (unsigned)peer_id, driver, rx, cc, app[0] ? ",\n  \"apps\": [\n    " : "", app, app[0] ? "\n  ]" : "");
    return n < 0 ? 0 : (size_t)n < cap ? (size_t)n : cap - 1;
}

void rdofradio_cfg_apply_repeater(const rdofradio_cfg_t *o, rdof_repeater_t *r) { rdofradio_cfg_apply_unit(&o->cfg, r); }

void rdofradio_cfg_apply_unit(const rdof_config_t *c, rdof_repeater_t *r)
{
    r->enabled[1] = c->slot1;
    r->enabled[2] = c->slot2;
    r->inactivity_s = c->hang_s; /* T_MSInactiv */
    rdof_repeater_set_call_hang(r, c->call_hang_s);
    r->short_lc_ts1_low = c->short_lc_ts1_low;
    r->prebuffer = c->prebuffer > 1 ? c->prebuffer : 1;
    r->jb.adaptive = c->jitter_auto;
    r->jb.min = rdof_jb_bursts(c->jitter_min_ms);
    r->jb.max = rdof_jb_bursts(c->jitter_max_ms);
    snprintf(r->cwid, sizeof r->cwid, "%s", c->cwid);
    r->cwid_every_s = c->cwid_every_s;
}

void rdofradio_cfg_apply_txvr(const rdofradio_cfg_t *o, rdof_txvr_t *t)
{
    const rdof_config_t *c = &o->cfg;
    t->nchannels = 0;
    for (int i = 0; i < c->nchannels && t->nchannels < RDOF_TXVR_MAX_CHANNELS; i++)
        t->channels[t->nchannels++] = (rdof_channel_t){c->channels[i].freq, c->channels[i].cc, c->channels[i].power, c->channels[i].slot};
    t->listen_s = c->listen_s;
    t->clear_s = c->clear_s;
    t->prebuffer = c->prebuffer;
    t->jb.adaptive = c->jitter_auto;
    t->jb.min = rdof_jb_bursts(c->jitter_min_ms);
    t->jb.max = rdof_jb_bursts(c->jitter_max_ms);
    t->csbk_preambles = c->csbk_preambles;
    t->answer_s = c->answer_s;
    t->answer_max_s = c->answer_max_s;
    t->idle_after_s = c->idle_after_s;
    t->has_idle = true;
    t->idle = (rdof_channel_t){c->idle.freq, c->idle.cc, c->idle.power, c->idle.slot};
    snprintf(t->cwid, sizeof t->cwid, "%s", c->cwid);
    t->cwid_every_s = c->cwid_every_s;
}

bool rdofradio_cfg_radio(const rdofradio_cfg_t *o, rdof_radio_t *r, rdof_modem_t *modem, const char *description,
                         char *err, size_t cap)
{
    const rdof_config_t *c = &o->cfg;
    rdof_radio_kind_t kind = RDOF_RADIO_UNKNOWN;
    if (!c->radio_type[0] || !strcmp(c->radio_type, "auto")) {
        kind = rdof_radio_detect(description);
        if (kind == RDOF_RADIO_UNKNOWN) {
            snprintf(err, cap, "can't tell what '%.40s' is: set the radio's type", description);
            return false;
        }
    } else {
        kind = rdof_radio_kind_of(c->radio_type);
        if (kind == RDOF_RADIO_UNKNOWN)
            kind = RDOF_RADIO_MMDVM;
    }
    rdof_radio_init(r, modem, kind);
    r->config = c->modem;
    r->rx_offset_hz = c->rx_offset_hz;
    r->tx_offset_hz = c->tx_offset_hz;
    const uint32_t fixed = c->mode == RDOF_CFG_REPEATER ? c->tx_freq : 0;
    if (c->tuner == RDOF_TUNER_MODEM || (c->tuner == RDOF_TUNER_NONE && kind != RDOF_RADIO_MMDVM)) {
        r->tuner = RDOF_TUNER_MODEM;
    } else if (c->tuner == RDOF_TUNER_FIXED || fixed) {
        r->tuner = RDOF_TUNER_FIXED;
        r->fixed_hz = c->tuner_freq ? c->tuner_freq : fixed;
        if (!r->fixed_hz) {
            snprintf(err, cap, "radio.tuner fixed needs its freq");
            return false;
        }
    } else {
        snprintf(err, cap, "an MMDVM board needs radio.tuner: fixed");
        return false;
    }
    if (c->rssi.n) {
        r->rssi_map = c->rssi;
        r->hotspot_rssi = false;
    }
    if (c->headers >= 0)
        r->headers = c->headers;
    if (c->terminators >= 0)
        r->terminators = c->terminators;
    r->settle_ms = c->settle_ms;
    return true;
}

size_t rdofradio_cfg_describe(const rdofradio_cfg_t *o, const char *source, const char *device, char *out, size_t cap)
{
    char rx[24], tx[24];
    fmhz(rx, sizeof rx, o->rx_hz);
    fmhz(tx, sizeof tx, o->tx_hz);
    size_t n = 0;
#define PUT(...)                                                                                                       \
    do {                                                                                                               \
        if (n < cap)                                                                                                   \
            n += (size_t)snprintf(out + n, cap - n, __VA_ARGS__);                                                      \
    } while (0)
    PUT("%s runs: %s (rdofradio; its device below)\nsettings: %s\n\n%s:\n", o->box, o->name, source, o->name);
    PUT("%s, RX %s MHz, TX %s MHz, CC %d, peer id %u", o->full ? "repeater (full duplex)" : "transceiver (half duplex)",
        rx, tx, o->cc, (unsigned)o->peer_id);
    if (o->has_parrot)
        PUT(", parrot %u%s", (unsigned)o->parrot, o->parrot ? "" : " (everything)");
    PUT("\n");
    if (o->full)
        PUT("slots%s%s, hang %.1f s, call hang %.1f s, prebuffer %d%s%s%s\n", o->cfg.slot1 ? " 1" : "",
            o->cfg.slot2 ? " 2" : "", o->cfg.hang_s, o->cfg.call_hang_s, o->cfg.prebuffer,
            o->cfg.jitter_auto ? ", jitter buffer auto" : "", o->cfg.cwid[0] ? ", CW ID " : "", o->cfg.cwid);
    if (o->has_hblink)
        PUT("HBlink4: %s:%d, peer %u%s\n", o->cfg.master_host, o->cfg.master_port, (unsigned)o->peer_id,
            strcmp(o->cfg.transport, "udp") ? " (UDP: SCTP isn't on this host yet)" : "");
    else
        PUT("HBlink4: off (no hblink network)\n");
    for (int i = 0; i < o->nchan; i++) {
        const rdof_sdr_cfg_channel_t *m = &o->chan[i];
        char f[24];
        fmhz(f, sizeof f, (uint32_t)llround(m->freq));
        PUT("channel %s: %s MHz", m->id, f);
        if (m->cc >= 0)
            PUT(", CC %d", m->cc);
        if (o->has_hblink && m->hblink && m->peer_id >= 0)
            PUT(", peer %ld", m->peer_id);
        PUT("\n");
    }
    if (o->cfg.has_mqtt)
        PUT("MQTT: %s:%d, root %s%s\n", o->cfg.mqtt_host, o->cfg.mqtt_port, o->cfg.mqtt_root, o->cfg.mqtt_tls ? ", TLS" : "");
    else
        PUT("MQTT: off (no mqtt network)\n");
    if (o->own_clock)
        PUT("clock: netclock, from %s\n", o->ntp_servers[0] ? o->ntp_servers : "the browser only (no servers)");
    else
        PUT("clock: the OS's\n");
    if (device && *device)
        PUT("\ndevice (command line): %s\n", device);
#undef PUT
    if (n >= cap)
        n = cap ? cap - 1 : 0;
    return n;
}
