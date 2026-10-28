// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* rdofradio's settings (rdofradio_cfg.h). */
#include "rdofradio_cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/jitter.h"
#include "rdof/jsonp.h"

/* What rdofradio applies: the box's keys, and the radio's */
static const char *const BOX_USED[] = {"name", "cc", "channels", "radios", "run", NULL};
static const char *const RADIO_USED[] = {"type",          "name",        "peer_id",       "rx_freq",
                                         "tx_freq",       "cc",          "parrot",        "slots",
                                         "hang_s",        "call_hang_s", "short_lc_ts1_low", "prebuffer",
                                         "jitter_buffer", "jitter_min_ms", "jitter_max_ms", "cwid",
                                         "cwid_every_s",  "mode",        "duplex",        "enabled",
                                         "idle",          "channels",    NULL};

static bool listed(const char *const *l, const char *k)
{
    for (; *l; l++)
        if (!strcmp(*l, k))
            return true;
    return false;
}

static void add(char *out, size_t cap, const char *what)
{
    size_t n = strlen(out);
    if (n + strlen(what) + 3 < cap)
        snprintf(out + n, cap - n, "%s%s", n ? ", " : "", what);
    else if (n + 5 < cap && strcmp(out + (n > 3 ? n - 3 : 0), "..."))
        snprintf(out + n, cap - n, ", ...");
}

/* The keys set that rdofradio doesn't use: the box's, the radio's, and every other radio's section. */
static void unused(const char *text, size_t len, const char *radio, char *out, size_t cap)
{
    out[0] = 0;
    enum { NODES = 2048 };
    rdof_jdoc_t d = {0};
    d.nodes = malloc(NODES * sizeof *d.nodes);
    d.max = NODES;
    char *clean = malloc(len + 1);
    d.buf = malloc(len + 1);
    d.cap = len + 1;
    if (!d.nodes || !clean || !d.buf)
        goto done;
    const size_t n = rdof_jsonc_strip(text, len, clean);
    const int box = rdof_jparse(&d, clean, n);
    if (box < 0)
        goto done;
    for (int k = 0; k < rdof_jnode(&d, box)->count; k++) {
        const rdof_jnode_t *e = rdof_jnode(&d, rdof_jat(&d, box, k));
        if (e && e->key && !listed(BOX_USED, e->key))
            add(out, cap, e->key);
    }
    const int radios = rdof_jget(&d, box, "radios");
    for (int k = 0; radios >= 0 && k < rdof_jnode(&d, radios)->count; k++) {
        const int s = rdof_jat(&d, radios, k);
        const rdof_jnode_t *nm = rdof_jnode(&d, rdof_jget(&d, s, "name"));
        const char *name = nm && nm->type == RDOF_JSTR ? nm->str : "";
        if (strcmp(name, radio)) {
            char t[48];
            snprintf(t, sizeof t, "radios %s", *name ? name : "(no name)");
            add(out, cap, t);
            continue;
        }
        for (int i = 0; rdof_jnode(&d, s)->type == RDOF_JOBJ && i < rdof_jnode(&d, s)->count; i++) {
            const rdof_jnode_t *e = rdof_jnode(&d, rdof_jat(&d, s, i));
            if (e && e->key && !listed(RADIO_USED, e->key)) {
                char t[48];
                snprintf(t, sizeof t, "%s.%s", radio, e->key);
                add(out, cap, t);
            }
        }
    }
done:
    free(d.nodes);
    free(d.buf);
    free(clean);
}

bool rdofradio_cfg_parse(const char *text, size_t len, rdofradio_cfg_t *o, char *err, size_t cap)
{
    memset(o, 0, sizeof *o);
    rdof_roles_t roles;
    if (!rdof_box_roles(text, len, &roles, err, cap))
        return false;
    if (!roles.has_radio) {
        snprintf(err, cap, "no repeater or sdr_transceiver in radios: rdofradio runs one");
        return false;
    }
    rdof_config_t *c = &o->cfg;
    if (!rdof_config_parse_radio(text, len, roles.radio, c, err, cap))
        return false;
    if (c->sdr_duplex == RDOF_SDR_DUPLEX_NONE && c->mode != RDOF_CFG_REPEATER) {
        snprintf(err, cap, "%s: a roamer on a radio; rdofradio runs a repeater, or an sdr_transceiver", c->name);
        return false;
    }
    if (c->sfr || c->dcdm) {
        snprintf(err, cap, "%s: mode %s isn't on rdofradio (duplex only)", c->name, c->sfr ? "sfr" : "dcdm");
        return false;
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
    } else {
        /* a half duplex transceiver: listens and sends on its idle channel */
        if (!c->idle.freq) {
            snprintf(err, cap, "%s: half duplex needs an idle channel (rdofradio listens on it)", c->name);
            return false;
        }
        o->full = false;
        o->rx_hz = o->tx_hz = c->idle.freq;
        o->cc = c->idle.cc;
    }
    unused(text, len, roles.radio, o->unused, sizeof o->unused);
    return true;
}

/* MHz with the trailing zeros off, 438.25 */
static void fmhz(char *out, size_t cap, uint32_t hz)
{
    snprintf(out, cap, "%.6f", hz / 1e6);
    char *e = out + strlen(out) - 1;
    while (*e == '0' && e[-1] != '.')
        *e-- = 0;
}

size_t rdofradio_cfg_from_flags(char *out, size_t cap, bool full, uint32_t rx_hz, uint32_t tx_hz, int cc,
                                bool has_parrot, uint32_t parrot, uint32_t peer_id)
{
    char rx[24], tx[24], p[32] = "";
    fmhz(rx, sizeof rx, rx_hz);
    fmhz(tx, sizeof tx, tx_hz);
    if (has_parrot)
        snprintf(p, sizeof p, ", \"parrot\": %u", (unsigned)parrot);
    int n;
    if (full)
        n = snprintf(out, cap,
                     "{\n  \"name\": \"rdofradio\",\n  \"radios\": [\n    {\"type\": \"repeater\", \"name\": \"rdofradio\", "
                     "\"peer_id\": %u, \"rx_freq\": %s, \"tx_freq\": %s, \"cc\": %d%s}\n  ]\n}\n",
                     (unsigned)peer_id, rx, tx, cc, p);
    else
        n = snprintf(out, cap,
                     "{\n  \"name\": \"rdofradio\",\n  \"cc\": %d,\n  \"channels\": {\"1\": %s},\n  \"radios\": [\n"
                     "    {\"type\": \"sdr_transceiver\", \"name\": \"rdofradio\", \"duplex\": \"half\", \"peer_id\": %u, "
                     "\"idle\": 1%s},\n    {\"type\": \"sdr\", \"name\": \"sdr\", \"channels\": [1]}\n  ],\n"
                     "  \"run\": \"rdofradio\"\n}\n",
                     cc, rx, (unsigned)peer_id, p);
    return n < 0 ? 0 : (size_t)n < cap ? (size_t)n : cap - 1;
}

void rdofradio_cfg_apply_repeater(const rdofradio_cfg_t *o, rdof_repeater_t *r)
{
    const rdof_config_t *c = &o->cfg;
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
    PUT("%s runs: %s (rdofradio, a SoapySDR device)\nsettings: %s\n\n%s:\n", o->box, o->name, source, o->name);
    PUT("%s, RX %s MHz, TX %s MHz, CC %d, peer id %u", o->full ? "repeater (full duplex)" : "transceiver (half duplex)",
        rx, tx, o->cc, (unsigned)o->peer_id);
    if (o->has_parrot)
        PUT(", parrot %u%s", (unsigned)o->parrot, o->parrot ? "" : " (everything)");
    PUT("\n");
    if (o->full)
        PUT("slots%s%s, hang %.1f s, call hang %.1f s, prebuffer %d%s%s%s\n", o->cfg.slot1 ? " 1" : "",
            o->cfg.slot2 ? " 2" : "", o->cfg.hang_s, o->cfg.call_hang_s, o->cfg.prebuffer,
            o->cfg.jitter_auto ? ", jitter buffer auto" : "", o->cfg.cwid[0] ? ", CW ID " : "", o->cfg.cwid);
    PUT("not used on this host: %s\n", o->unused[0] ? o->unused : "none");
    if (device && *device)
        PUT("\ndevice (command line): %s\n", device);
#undef PUT
    if (n >= cap)
        n = cap ? cap - 1 : 0;
    return n;
}
