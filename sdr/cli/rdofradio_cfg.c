// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* rdofradio's settings (rdofradio_cfg.h). */
#include "rdofradio_cfg.h"
#include "rdof/ipgw.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/benchcfg.h"
#include "rdof/jitter.h"
#include "rdof/jsonp.h"
#include "rdof/siteplan.h"
#ifdef RDOF_TIER3
#include "rdof/t3cfg.h"
#endif

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

/* bench: rdofradio's bench is the line protocol (--bench) on its own transmitter; bench.tx says where its jobs may
   transmit (with --bench-tx), how long they may key and how strong. No bench devices: nothing on a box's UART or USB
   is lent to the bench. No arm jumper: rdofradio has no GPIO, and a gate it can't see would be a gate left open. */
static bool bench_settings(const rdof_plan_t *p, rdofradio_cfg_t *o, char *err, size_t cap)
{
    const rdof_jdoc_t *d = rdof_plan_doc(p);
    const int b = rdof_jget(d, rdof_plan_root(p), "bench");
    if (b < 0 || rdof_jnode(d, b)->type == RDOF_JNULL)
        return true;
    if (rdof_plan_nbench(p) > 0) {
        snprintf(err, cap, "bench.devices: a board's; rdofradio's bench is its own transmitter (--bench)");
        return false;
    }
    if (rdof_plan_bench_tx(p, &o->bench_tx, err, cap) < 0)
        return false;
    if (o->bench_tx.arm_gpio >= 0) {
        snprintf(err, cap, "bench.tx.arm_gpio: a board's jumper; rdofradio has no GPIO to read it");
        return false;
    }
    o->has_bench_tx = rdof_jget(d, b, "tx") >= 0;
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

#ifdef RDOF_TIER3
static void str_key(const rdof_jdoc_t *d, int obj, const char *key, const char *dflt, char *out, size_t cap)
{
    const int i = obj >= 0 ? rdof_jget(d, obj, key) : -1;
    const rdof_jnode_t *n = i >= 0 ? rdof_jnode(d, i) : NULL;
    snprintf(out, cap, "%s", n && n->type == RDOF_JSTR && n->str[0] ? n->str : dflt);
}

/* A trunked site: the controller's settings from its app, each carrier's devices, the mqtt network (as the board's
   unit_mqtt reads it: host, port, username, password, root without its slashes, tls). */
static bool from_trunk(const rdof_plan_t *p, int unit, rdofradio_cfg_t *o, char *err, size_t cap)
{
    rdofradio_t3_cfg_t *t = &o->t;
    const rdof_site_t *s = rdof_plan_site(p);
    const rdof_unit_t *u = rdof_plan_unit(p, unit);
    if (!rdof_t3cfg_plan(p, unit, &t->cfg, &t->pol, err, cap))
        return false;
    o->tier3 = true;
    snprintf(t->name, sizeof t->name, "%.*s", (int)sizeof t->name - 1, u->name); /* cut, as rdof_config_t's */
    snprintf(o->name, sizeof o->name, "%.*s", (int)sizeof o->name - 1, u->name);
    t->path = s->app[u->app].path[0];
    t->ncar = t->cfg.ncarriers;
    for (int k = 0; k < t->ncar; k++) {
        int rx = -1, tx = -1;
        if (!rdof_plan_t3_carrier(p, unit, k, &rx, &tx)) {
            snprintf(err, cap, "apps.%s: carrier %d has no device", s->app[u->app].name, k + 1);
            return false;
        }
        t->car[k].rx_hz = t->cfg.carriers[k].rx_hz, t->car[k].tx_hz = t->cfg.carriers[k].tx_hz;
        t->car[k].tx_sdr = tx >= 0 && s->dev[tx].kind == 1;
        t->car[k].tx_dev = tx;
        const rdof_plan_dev_t *d = tx >= 0 ? rdof_plan_dev(p, tx) : NULL;
        if (d && d->port.kind == RDOF_PORT_PATH) {
            snprintf(t->car[k].port, sizeof t->car[k].port, "%s", d->port.path);
            t->car[k].baud = d->port.baud;
        }
        t->car[k].rx_sdr = rx >= 0 && s->dev[rx].kind == 1;
        const rdof_plan_dev_t *r = rx >= 0 && !t->car[k].rx_sdr ? rdof_plan_dev(p, rx) : NULL;
        if (r && r->port.kind == RDOF_PORT_PATH) {
            snprintf(t->car[k].rx_port, sizeof t->car[k].rx_port, "%s", r->port.path);
            t->car[k].rx_baud = r->port.baud;
        }
    }
    t->net = rdof_plan_net_of(p, RDOF_NET_MQTT);
    if (t->net >= 0) {
        const rdof_jdoc_t *d = rdof_plan_doc(p);
        const int m = rdof_plan_net_node(p, t->net);
        str_key(d, m, "host", "localhost", t->host, sizeof t->host);
        str_key(d, m, "username", "", t->user, sizeof t->user);
        str_key(d, m, "password", "", t->pass, sizeof t->pass);
        char root[64];
        str_key(d, m, "root", "DMR", root, sizeof root);
        size_t rl = strlen(root), lead = strspn(root, "/");
        while (rl > lead && root[rl - 1] == '/')
            rl--;
        snprintf(t->root, sizeof t->root, "%.*s", (int)(rl - lead), root + lead);
        const int pi = m >= 0 ? rdof_jget(d, m, "port") : -1, ti = m >= 0 ? rdof_jget(d, m, "tls") : -1;
        t->port = pi >= 0 && rdof_jnode(d, pi)->type == RDOF_JNUM ? (int)rdof_jnode(d, pi)->num : 1883;
        if (t->port <= 0 || t->port > 65535) {
            snprintf(err, cap, "networks.%s.port: 1-65535", s->net[t->net].name);
            return false;
        }
        t->tls = (ti >= 0 && rdof_jnode(d, ti)->type == RDOF_JBOOL && rdof_jnode(d, ti)->b) || s->net[t->net].transport == RDOF_MQTT_TLS;
    }
    /* the site's own login to a BrandMeister or Homebrew master (tier3.md 5.11) */
    t->bm = -1;
    for (int k = 0; k < s->nnet; k++)
        if (s->net[k].type == RDOF_NET_HBLINK && s->net[k].login && t->path >= 0 && s->path[t->path].pol.net == k)
            t->bm = k;
    for (int x = 0; x < 2; x++) {
        const int k = x ? t->bm : t->net;
        rdof_idset_clear(&t->deny_tg[x]);
        rdof_idset_clear(&t->deny_pc[x]);
        if (k >= 0)
            t->deny_tg[x] = s->net_deny_tg[k], t->deny_pc[x] = s->net_deny_pc[k];
    }
    if (t->bm >= 0) {
        const rdof_jdoc_t *d = rdof_plan_doc(p);
        const int m = rdof_plan_net_node(p, t->bm);
        const rdof_net3_t *n = &s->net[t->bm];
        t->bm_peer = s->path[t->path].peer_id;
        t->bm_dl5di = !n->homebrew;
        str_key(d, m, "host", "", t->bm_host, sizeof t->bm_host);
        str_key(d, m, "password", "", t->bm_pass, sizeof t->bm_pass);
        str_key(d, m, "location", "", t->bm_location, sizeof t->bm_location);
        str_key(d, m, "description", "", t->bm_description, sizeof t->bm_description);
        str_key(d, m, "url", "", t->bm_url, sizeof t->bm_url);
        str_key(d, 0 /* the root */, "callsign", "", t->callsign, sizeof t->callsign);
        const int pi = m >= 0 ? rdof_jget(d, m, "port") : -1, ai = m >= 0 ? rdof_jget(d, m, "activate") : -1,
                  ri = m >= 0 ? rdof_jget(d, m, "renew_s") : -1;
        t->bm_port = pi >= 0 && rdof_jnode(d, pi)->type == RDOF_JNUM ? (int)rdof_jnode(d, pi)->num
                                                                     : (t->bm_dl5di ? 62030 : 62031);
        t->bm_activate = ai < 0 || rdof_jnode(d, ai)->b;
        t->bm_renew_s = ri >= 0 && rdof_jnode(d, ri)->type == RDOF_JNUM ? rdof_jnode(d, ri)->num : 840;
        const int ni = m >= 0 ? rdof_jget(d, m, "announce") : -1;
        t->bm_announce = ni < 0 || rdof_jnode(d, ni)->b;
        const int ui = m >= 0 ? rdof_jget(d, m, "unlink_s") : -1;
        t->bm_unlink_s = ui >= 0 && rdof_jnode(d, ui)->type == RDOF_JNUM ? rdof_jnode(d, ui)->num : 120;
        t->bm_npin = 0;
        for (int sl = 1; sl <= 2 && m >= 0; sl++) {
            const int ai = rdof_jget(d, m, sl == 1 ? "ts1_tg" : "ts2_tg");
            for (int k = 0; ai >= 0 && k < rdof_jnode(d, ai)->count && t->bm_npin < 32; k++) {
                const int vi = rdof_jat(d, ai, k);
                if (vi >= 0 && rdof_jnode(d, vi)->type == RDOF_JNUM)
                    t->bm_pin_tg[t->bm_npin] = (uint32_t)rdof_jnode(d, vi)->num, t->bm_pin_slot[t->bm_npin++] = sl;
            }
        }
        const int li = rdof_jget(d, 0 /* the root */, "location");
        t->has_location = li >= 0 && rdof_jnode(d, li)->type == RDOF_JOBJ;
        if (t->has_location) {
            int la = rdof_jget(d, li, "latitude"), lo = rdof_jget(d, li, "longitude");
            const int h = rdof_jget(d, li, "height");
            la = la >= 0 ? la : rdof_jget(d, li, "lat"), lo = lo >= 0 ? lo : rdof_jget(d, li, "lon");
            t->latitude = la >= 0 && rdof_jnode(d, la)->type == RDOF_JNUM ? rdof_jnode(d, la)->num : 0;
            t->longitude = lo >= 0 && rdof_jnode(d, lo)->type == RDOF_JNUM ? rdof_jnode(d, lo)->num : 0;
            t->height_m = h >= 0 && rdof_jnode(d, h)->type == RDOF_JNUM ? (int)rdof_jnode(d, h)->num : 0;
        }
    }
    /* "standby": true or {"takeover_s", "fence_s"} (tier3.md 6.2) */
    t->standby = false, t->takeover_s = 13, t->fence_s = 9;
    {
        const rdof_jdoc_t *d = rdof_plan_doc(p);
        const int a = rdof_plan_app_node(p, u->app), si = a >= 0 ? rdof_jget(d, a, "standby") : -1;
        const rdof_jnode_t *n = si >= 0 ? rdof_jnode(d, si) : NULL;
        const char *where = s->app[u->app].name;
        if (n && n->type == RDOF_JBOOL) {
            t->standby = n->b;
        } else if (n && n->type == RDOF_JOBJ) {
            t->standby = true;
            for (int k = n->child; k >= 0; k = rdof_jnode(d, k)->next) {
                const rdof_jnode_t *v = rdof_jnode(d, k);
                double *to = !strcmp(v->key, "takeover_s") ? &t->takeover_s
                             : !strcmp(v->key, "fence_s")  ? &t->fence_s
                                                           : NULL;
                if (!to || v->type != RDOF_JNUM) {
                    snprintf(err, cap, "apps.%s.standby.%s: %s", where, v->key,
                             to ? "a number of seconds" : "not a setting (takeover_s, fence_s)");
                    return false;
                }
                *to = v->num;
            }
        } else if (n && n->type != RDOF_JNULL) {
            snprintf(err, cap, "apps.%s.standby: true, false or {\"takeover_s\", \"fence_s\"}", where);
            return false;
        }
        /* a standby gone without a word shows (its will) 7.5 s on: fence_s past it; the takeover past fence_s */
        if (t->standby &&
            (t->fence_s < 9 || t->fence_s > 60 || t->takeover_s < t->fence_s + 3 || t->takeover_s > 120)) {
            snprintf(err, cap, "apps.%s.standby: fence_s 9-60, takeover_s at least fence_s + 3, at most 120", where);
            return false;
        }
        if (t->standby && t->net < 0) {
            snprintf(err, cap, "apps.%s.standby: needs the site's mqtt network", where);
            return false;
        }
    }
    /* the IP gateway app beside the site: the site's packet data goes to it (tier3.md 3.16) */
    o->has_ipgw = false;
    if (rdof_site_app_of(s, RDOF_APP_IPGW) >= 0) {
        if (!rdof_plan_ipgw(p, &o->ipgw, err, cap))
            return false;
        o->has_ipgw = o->ipgw.on;
    }
    o->rx_hz = t->car[0].rx_hz, o->tx_hz = t->car[0].tx_hz, o->cc = t->cfg.cc;
    o->full = true;
    o->rx_slot = 1;
    o->tx_power_dbm = NAN;
    o->site = *s;
    return clock_settings(p, o, err, cap);
}
#endif

/* The plan's Tier III unit (a tier3 app), -1 none. */
static int trunk_unit(const rdof_plan_t *p)
{
#ifdef RDOF_TIER3
    return rdof_t3cfg_unit(p);
#else
    for (int k = 0; k < rdof_plan_nunits(p); k++)
        if (rdof_plan_unit(p, k)->kind == RDOF_UNIT_TIER3)
            return k;
    return -1;
#endif
}

static bool from_plan(const rdof_plan_t *p, rdofradio_cfg_t *o, char *err, size_t cap)
{
    snprintf(o->web_hosts, sizeof o->web_hosts, "%s", rdof_plan_web_hosts(p));
    const int tu = trunk_unit(p);
    if (tu >= 0) {
#ifdef RDOF_TIER3
        if (!rdof_plan_check(p, &rdof_plan_host, err, cap))
            return false;
        rdof_roles_t roles;
        if (rdof_plan_roles_on(p, &rdof_plan_host, &roles, err, cap))
            snprintf(o->box, sizeof o->box, "%s", roles.box);
        err[0] = 0;
        return from_trunk(p, tu, o, err, cap);
#else /* the Tier III site is rust's (RDOF_TIER3): none here */
        snprintf(err, cap, "%s is a trunked site: this rdofradio was built without it (RDOF_TIER3, cargo)",
                 rdof_plan_unit(p, tu)->name);
        return false;
#endif
    }
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
    /* the IP gateway app, when there is one (rdofradio_ipgw.c runs it) */
    o->has_ipgw = false;
    if (rdof_site_app_of(rdof_plan_site(p), RDOF_APP_IPGW) >= 0) {
        if (!rdof_plan_ipgw(p, &o->ipgw, err, cap))
            return false;
        o->has_ipgw = o->ipgw.on;
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
    if (dv) /* the device's tx.power_dbm, 10 dBm unless set (config-v3 §4.7), as config.c has it: never the gain's floor */
        o->tx_power_dbm = dv->has_tx_power ? dv->tx_power_dbm : 10;
    if (!clock_settings(p, o, err, cap) || !bench_settings(p, o, err, cap))
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
    if (o->has_bench_tx) {
        const rdof_benchtx_policy_t *b = &o->bench_tx;
        if (!b->nfreqs && !b->nbands)
            PUT("bench TX (bench.tx): no frequencies here (--bench-tx)");
        else
            PUT("bench TX (bench.tx): %d frequencies, %d bands", b->nfreqs, b->nbands);
        if (b->has_max_dbm)
            PUT(", at most %.1f dBm, key-ups to %.1f s\n", b->max_dbm, b->max_key_s);
        else
            PUT(", RF level %d at most, key-ups to %.1f s\n", b->rf_level, b->max_key_s);
    }
    if (device && *device)
        PUT("\ndevice (command line): %s\n", device);
#undef PUT
    if (n >= cap)
        n = cap ? cap - 1 : 0;
    return n;
}
