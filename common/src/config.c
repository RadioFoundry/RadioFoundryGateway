// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The settings (version 3) -> one repeater, SFR or DCDM gateway, or transceiver (rdof/config.h,
   docs/spec/fw/config-v3.md section 12). */
#include "rdof/config.h"
#include "rdof/slotgrid.h"

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/jitter.h"
#include "rdof/json.h"
#include "rdof/jsonp.h"
#include "rdof/siteplan.h"
#include "rdof/status.h"
#include "rdof/txpower.h"

typedef struct {
    const rdof_jdoc_t *doc;
    char *error;
    size_t error_cap;
    int mm[2];       /* the modem's MMDVM settings: a split's TX half's, then its RX half's */
    const char *mmw; /* where they are: devices.NAME.mmdvm */
    rdof_txmap_t map; /* the unit's transmitter's levels in dBm (rdof/txpower.h) */
} ctx_t;

/* s into out[cap], cut to cap - 1 bytes (as snprintf "%s" cuts it, without its truncation warning). */
static void copy_str(char *out, size_t cap, const char *s)
{
    if (!cap)
        return;
    size_t n = strlen(s);
    if (n >= cap)
        n = cap - 1;
    memcpy(out, s, n);
    out[n] = '\0';
}

static bool err(ctx_t *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->error, c->error_cap, fmt, ap);
    va_end(ap);
    return false;
}

/* ── value helpers (int, float, string, bool) ──────────────────────────────── */

/* (long)v, (int)v and (uint32_t)v are undefined out of range: these saturate, NaN 0 (as Rust's casts). */
static long to_long(double v)
{
    return v != v ? 0 : v >= (double)LONG_MAX ? LONG_MAX : v <= (double)LONG_MIN ? LONG_MIN : (long)v;
}

static int to_int(double v)
{
    return v != v ? 0 : v >= (double)INT_MAX ? INT_MAX : v <= (double)INT_MIN ? INT_MIN : (int)v;
}

static uint32_t to_u32(double v)
{
    return !(v > 0) ? 0 : v >= 4294967295.0 ? UINT32_MAX : (uint32_t)v;
}

static const rdof_jnode_t *N(ctx_t *c, int i)
{
    return rdof_jnode(c->doc, i);
}

static int get(ctx_t *c, int obj, const char *key)
{
    return rdof_jget(c->doc, obj, key);
}

static bool is_env(const rdof_jnode_t *n)
{
    size_t l = n && n->type == RDOF_JSTR ? strlen(n->str) : 0;
    return l >= 3 && n->str[0] == '$' && n->str[1] == '{' && n->str[l - 1] == '}';
}

/* "Truthy": present, not null/false/0/""/{}/[] (env refs expand to ""). */
static bool truthy(ctx_t *c, int i)
{
    const rdof_jnode_t *n = N(c, i);
    if (!n)
        return false;
    switch (n->type) {
    case RDOF_JNULL: return false;
    case RDOF_JBOOL: return n->b;
    case RDOF_JNUM: return n->num != 0;
    case RDOF_JSTR: return n->str[0] && !is_env(n);
    default: return n->count > 0;
    }
}

static bool num(ctx_t *c, int i, double *out)
{
    const rdof_jnode_t *n = N(c, i);
    if (!n)
        return false;
    if (n->type == RDOF_JNUM) {
        *out = n->num;
        return true;
    }
    if (n->type == RDOF_JBOOL) {
        *out = n->b;
        return true;
    }
    if (n->type == RDOF_JSTR) {
        char *e;
        double v = strtod(n->str, &e);
        while (*e == ' ')
            e++;
        if (n->str[0] && !*e && !strpbrk(n->str, "xX")) { /* not hex */
            *out = v;
            return true;
        }
    }
    return false;
}

/* float(d.get(key, dflt)) */
static bool fget(ctx_t *c, int obj, const char *key, double dflt, double *out, const char *what)
{
    int i = get(c, obj, key);
    if (i < 0) {
        *out = dflt;
        return true;
    }
    if (!num(c, i, out))
        return err(c, "%s: %s isn't a number", what, key);
    return true;
}

/* int(d.get(key, dflt)) */
static bool iget(ctx_t *c, int obj, const char *key, long dflt, long *out, const char *what)
{
    double v;
    int i = get(c, obj, key);
    if (i < 0) {
        *out = dflt;
        return true;
    }
    const rdof_jnode_t *n = N(c, i);
    if (n->type == RDOF_JSTR) {
        char *e;
        long x = strtol(n->str, &e, 10);
        if (!n->str[0] || *e)
            return err(c, "%s: %s isn't a whole number", what, key);
        *out = x;
        return true;
    }
    if (!num(c, i, &v))
        return err(c, "%s: %s isn't a number", what, key);
    *out = to_long(v); /* int() truncates */
    return true;
}

static void text_of(ctx_t *c, int i, char *out, size_t cap)
{
    const rdof_jnode_t *n = N(c, i);
    if (!n || n->type == RDOF_JNULL) {
        snprintf(out, cap, "None");
    } else if (n->type == RDOF_JSTR) {
        snprintf(out, cap, "%s", is_env(n) ? "" : n->str);
    } else if (n->type == RDOF_JNUM) {
        if (n->integer)
            snprintf(out, cap, "%.0f", n->num + 0.0); /* -0 is 0 */
        else
            rdof_format_repr(n->num, out, cap); /* a float as text */
    } else if (n->type == RDOF_JBOOL) {
        snprintf(out, cap, "%s", n->b ? "True" : "False");
    } else {
        snprintf(out, cap, "?");
    }
}

/* str(d.get(key, dflt)) */
static void sget(ctx_t *c, int obj, const char *key, const char *dflt, char *out, size_t cap)
{
    int i = get(c, obj, key);
    if (i < 0)
        copy_str(out, cap, dflt);
    else
        text_of(c, i, out, cap);
}

/* d.get(key) or dflt (falsy -> dflt) as text */
static void sor(ctx_t *c, int obj, const char *key, const char *dflt, char *out, size_t cap)
{
    int i = get(c, obj, key);
    if (truthy(c, i))
        text_of(c, i, out, cap);
    else
        copy_str(out, cap, dflt);
}

static bool bget(ctx_t *c, int obj, const char *key, bool dflt)
{
    int i = get(c, obj, key);
    return i < 0 ? dflt : truthy(c, i);
}

/* mmdvm.config.freq_hz: MHz below 10000 */
static uint32_t freq_hz(double f)
{
    return to_u32(rint(f < 10000 ? f * 1e6 : f));
}

/* ── the location ──────────────────────────────────────────────────────────── */

static bool location(ctx_t *c, int i, rdof_config_t *cfg)
{
    cfg->has_location = false;
    if (!truthy(c, i))
        return true;
    double lat, lon, h;
    int la = get(c, i, "latitude"), lo = get(c, i, "longitude");
    if (la < 0)
        la = get(c, i, "lat");
    if (lo < 0)
        lo = get(c, i, "lon");
    if (!num(c, la, &lat) || !num(c, lo, &lon))
        return err(c, "location: latitude and longitude");
    if (!(lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180))
        return err(c, "location %g, %g out of range", lat, lon);
    if (!fget(c, i, "height", 0, &h, "location"))
        return false;
    long hh = to_long(rint(h));
    cfg->location.latitude = lat;
    cfg->location.longitude = lon;
    cfg->location.height_m = (int)(hh < 0 ? 0 : hh > 999 ? 999 : hh);
    cfg->has_location = true;
    return true;
}

/* ── the radio section ─────────────────────────────────────────────────────── */

/* The jitter buffer: jitter_buffer "auto" (the default) or a fixed size in ms, rounded up to whole 60 ms bursts;
 * prebuffer, the older key, a fixed size in bursts. jitter_min_ms and jitter_max_ms bound auto. */
static bool jitter(ctx_t *c, int r, const char *what, rdof_config_t *cfg)
{
    long lv;
    int jb = get(c, r, "jitter_buffer"), pb = get(c, r, "prebuffer");
    if (jb >= 0 && pb >= 0)
        return err(c, "%s: prebuffer and jitter_buffer: set one (prebuffer is a fixed size in bursts)", what);
    if (!iget(c, r, "prebuffer", 6, &lv, what))
        return false;
    cfg->prebuffer = (int)lv;
    cfg->jitter_auto = pb < 0;
    if (jb >= 0) {
        const rdof_jnode_t *n = N(c, jb);
        double ms;
        if (n->type == RDOF_JSTR && !strcmp(n->str, "auto")) {
            cfg->jitter_auto = true;
        } else if (n->type != RDOF_JBOOL && num(c, jb, &ms) && ms >= 0 && ms <= RDOF_JB_MAX_MS) {
            cfg->jitter_auto = false;
            cfg->prebuffer = rdof_jb_bursts(ms);
        } else {
            return err(c, "%s: jitter_buffer: \"auto\" or milliseconds, 0 to %d", what, RDOF_JB_MAX_MS);
        }
    }
    double lo, hi;
    if (!fget(c, r, "jitter_min_ms", RDOF_JB_MIN_MS, &lo, what) ||
        !fget(c, r, "jitter_max_ms", RDOF_JB_MAX_DEFAULT_MS, &hi, what))
        return false;
    if (!(lo >= RDOF_JB_BURST_MS && lo <= hi && hi <= RDOF_JB_MAX_MS))
        return err(c, "%s: jitter_min_ms and jitter_max_ms: %d <= min <= max <= %d", what, RDOF_JB_BURST_MS, RDOF_JB_MAX_MS);
    cfg->jitter_min_ms = rdof_jb_bursts(lo) * RDOF_JB_BURST_MS;
    cfg->jitter_max_ms = rdof_jb_bursts(hi) * RDOF_JB_BURST_MS;
    return true;
}

/* A radio setting: the TX half's, else the RX half's (one modem: both are its own). */
static int rget(ctx_t *c, const char *key)
{
    int i = get(c, c->mm[0], key);
    return i >= 0 ? i : get(c, c->mm[1], key);
}

static bool radio_settings(ctx_t *c, rdof_config_t *cfg)
{
    double v;
    int i;
    rdof_modem_config_default(&cfg->modem);
    struct {
        const char *k;
        int t; /* 0 bool, 1 int, 2 double */
        void *p;
    } f[] = {
        {"rx_invert", 0, &cfg->modem.rx_invert},     {"tx_invert", 0, &cfg->modem.tx_invert},
        {"ptt_invert", 0, &cfg->modem.ptt_invert},   {"tx_delay_ms", 1, &cfg->modem.tx_delay_ms},
        {"rx_level", 2, &cfg->modem.rx_level},       {"tx_level", 2, &cfg->modem.tx_level},
        {"cwid_level", 2, &cfg->modem.cwid_level},   {"rx_dc_offset", 1, &cfg->modem.rx_dc_offset},
        {"tx_dc_offset", 1, &cfg->modem.tx_dc_offset}, {"dmr_delay", 1, &cfg->modem.dmr_delay},
    };
    for (size_t k = 0; k < sizeof f / sizeof f[0]; k++) {
        i = rget(c, f[k].k);
        if (i < 0)
            continue;
        if (f[k].t == 0) {
            *(bool *)f[k].p = truthy(c, i);
        } else {
            if (!num(c, i, &v))
                return err(c, "%s.%s isn't a number", c->mmw, f[k].k);
            if (f[k].t == 1)
                *(int *)f[k].p = to_int(v);
            else
                *(double *)f[k].p = v;
        }
    }
    cfg->rx_offset_hz = cfg->tx_offset_hz = 0;
    if ((i = rget(c, "rx_offset_hz")) >= 0 && num(c, i, &v))
        cfg->rx_offset_hz = to_int(v);
    if ((i = rget(c, "tx_offset_hz")) >= 0 && num(c, i, &v))
        cfg->tx_offset_hz = to_int(v);
    cfg->headers = cfg->terminators = -1;
    if ((i = rget(c, "headers")) >= 0 && N(c, i)->type != RDOF_JNULL && num(c, i, &v))
        cfg->headers = to_int(v);
    if ((i = rget(c, "terminators")) >= 0 && N(c, i)->type != RDOF_JNULL && num(c, i, &v))
        cfg->terminators = to_int(v);
    /* tuner: "fixed" / {type, freq, settle_ms}; rigctld and command tuners need a computer */
    cfg->tuner = RDOF_TUNER_NONE; /* the kind's default */
    cfg->tuner_freq = 0;
    cfg->settle_ms = 0;
    i = rget(c, "tuner");
    if (truthy(c, i)) {
        char t[16] = "";
        if (N(c, i)->type == RDOF_JSTR) {
            snprintf(t, sizeof t, "%s", N(c, i)->str);
        } else {
            sget(c, i, "type", "", t, sizeof t);
            if (get(c, i, "freq") >= 0) {
                if (!num(c, get(c, i, "freq"), &v))
                    return err(c, "%s.tuner.freq", c->mmw);
                cfg->tuner_freq = freq_hz(v);
            }
            char tw[96];
            snprintf(tw, sizeof tw, "%s.tuner", c->mmw);
            if (!fget(c, i, "settle_ms", 0, &v, tw))
                return false;
            cfg->settle_ms = to_int(v);
        }
        if (!strcmp(t, "modem"))
            cfg->tuner = RDOF_TUNER_MODEM;
        else if (!strcmp(t, "fixed"))
            cfg->tuner = RDOF_TUNER_FIXED;
        else if (!strcmp(t, "rigctld") || !strcmp(t, "command"))
            return err(c, "%s.tuner %s: not on this board (modem or fixed only)", c->mmw, t);
        else if (t[0])
            return err(c, "%s.tuner %s", c->mmw, t);
    }
    cfg->rssi.n = 0;
    i = rget(c, "rssi");
    if (truthy(c, i)) {
        if (N(c, i)->type != RDOF_JARR)
            return err(c, "%s.rssi wants [[raw, dBm], ...]", c->mmw);
        for (int k = 0; k < N(c, i)->count && k < RDOF_RSSI_POINTS; k++) {
            int p = rdof_jat(c->doc, i, k);
            double a, b;
            if (N(c, p)->type != RDOF_JARR || N(c, p)->count != 2 || !num(c, rdof_jat(c->doc, p, 0), &a) ||
                !num(c, rdof_jat(c->doc, p, 1), &b))
                return err(c, "%s.rssi wants [[raw, dBm], ...]", c->mmw);
            cfg->rssi.raw[k] = to_int(a);
            cfg->rssi.dbm[k] = to_int(b);
            cfg->rssi.n = k + 1;
        }
        rdof_rssi_map_sort(&cfg->rssi);
    }
    /* check_radio */
    char proto[12];
    i = rget(c, "protocol");
    if (i >= 0)
        text_of(c, i, proto, sizeof proto);
    else
        strcpy(proto, "uart");
    for (char *p = proto; *p; p++)
        if (*p >= 'A' && *p <= 'Z')
            *p += 32;
    if (strcmp(proto, "uart") && strcmp(proto, "udp"))
        return err(c, "%s.protocol %s: uart or udp", c->mmw, proto);
    if (!strcmp(proto, "udp")) {
        if (!truthy(c, rget(c, "modem_address")))
            return err(c, "%s.protocol udp needs %s.modem_address", c->mmw, c->mmw);
        return err(c, "%s.protocol udp: not on this board", c->mmw);
    }
    return true;
}

static bool known_type(const char *t)
{
    return !strcmp(t, "auto") || !strcmp(t, "radio_hotspot") || !strcmp(t, "mmdvm_hs") || !strcmp(t, "mmdvm");
}

static bool channel_entry(ctx_t *c, uint32_t freq, double cc, double power, int slot, rdof_cfg_channel_t *out)
{
    if (!(cc >= 0 && cc <= 15) || (long)cc != cc)
        return err(c, "color code %g", cc);
    long p = to_long(power);
    out->freq = freq;
    out->cc = (int)cc;
    out->power = (int)(p < 0 ? 0 : p > 255 ? 255 : p);
    out->slot = slot == 2 ? 2 : 1;
    return true;
}

static bool oled(ctx_t *c, int box, rdof_config_t *cfg)
{
    int o = get(c, box, "oled");
    cfg->has_oled = o >= 0 && !(N(c, o)->type == RDOF_JNULL) && !(N(c, o)->type == RDOF_JBOOL && !N(c, o)->b);
    cfg->oled_address = 0x3C;
    cfg->oled_refresh_s = 0.5;
    cfg->oled_sda = 8;
    cfg->oled_scl = 9;
    if (!cfg->has_oled || N(c, o)->type != RDOF_JOBJ)
        return true;
    int i2c = get(c, o, "i2c");
    long v;
    int src = get(c, o, "address") >= 0 ? o : i2c;
    if (!iget(c, src, "address", 0x3C, &v, "oled"))
        return false;
    cfg->oled_address = (int)v;
    cfg->oled_sh1106 = get(c, o, "sh1106") >= 0 ? bget(c, o, "sh1106", false) : bget(c, i2c, "sh1106", false);
    cfg->oled_rotate180 = get(c, o, "rotate180") >= 0 ? bget(c, o, "rotate180", false) : bget(c, i2c, "rotate180", false);
    if (!fget(c, o, "refresh_s", 0.5, &cfg->oled_refresh_s, "oled"))
        return false;
    if (!iget(c, o, "sda", 8, &v, "oled"))
        return false;
    cfg->oled_sda = (int)v;
    if (!iget(c, o, "scl", 9, &v, "oled"))
        return false;
    cfg->oled_scl = (int)v;
    return true;
}

/* The hblink network's sctp_*: the firmware's SCTP (over UDP); defaults as MMDVMHost's and HBlink4's. what: where
   it is (networks.NAME). */
static bool sctp_settings(ctx_t *c, int hb, const char *what, rdof_config_t *cfg)
{
    static const struct {
        const char *key;
        long dflt, lo, hi;
    } keys[] = {{"sctp_encap_port", 9899, 1, 65535},
                {"sctp_heartbeat_ms", 10000, 0, 600000},
                {"sctp_max_retransmit", 3, 1, 20},
                {"sctp_ttl_ms", 0, 0, 60000}};
    int *out[] = {&cfg->sctp_encap_port, &cfg->sctp_heartbeat_ms, &cfg->sctp_max_retransmit, &cfg->sctp_ttl_ms};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        long v;
        if (!iget(c, hb, keys[i].key, keys[i].dflt, &v, what))
            return false;
        if (v < keys[i].lo || v > keys[i].hi)
            return err(c, "%s: %s %ld out of range (%ld to %ld)", what, keys[i].key, v, keys[i].lo, keys[i].hi);
        *out[i] = (int)v;
    }
    return true;
}

/* ── version 3: a unit of the plan (rdof/siteplan.h) ───────────────────────── */

static uint32_t first_id(const rdof_idset_t *set)
{
    rdof_idrange_t r[1];
    return rdof_idset_ranges(set, r, 1) ? r[0].lo : 0;
}

/* A frequency's color code: its own, else the defaults' (-1: any). */
static int freq_cc(ctx_t *c, const rdof_plan_t *p, int f)
{
    const rdof_site_t *s = rdof_plan_site(p);
    double v;
    if (s->freq[f].cc >= 0)
        return s->freq[f].cc;
    int d = get(c, get(c, rdof_plan_root(p), "defaults"), "cc");
    return d >= 0 && N(c, d)->type != RDOF_JNULL && num(c, d, &v) ? to_int(v) : -1;
}

/* A frequency's channel number on the OLED: its place in frequencies, from 1. */
static int oled_number(const rdof_site_t *s, uint32_t hz)
{
    for (int f = 0; f < s->nfreq; f++) {
        int64_t d = (int64_t)s->freq[f].hz - hz;
        if (d >= -100 && d <= 100)
            return f + 1;
    }
    return 0;
}

/* The HBlink peer a unit is (its path's), and its callsign: the path's peer name, else the top-level callsign, else
   its name's first 8 characters. */
static bool unit_peer(ctx_t *c, const rdof_plan_t *p, int path, const char *where, rdof_config_t *cfg)
{
    const rdof_site_t *s = rdof_plan_site(p);
    int hb = rdof_plan_net_of(p, RDOF_NET_HBLINK);
    cfg->receiver_hblink = path >= 0 && hb >= 0 && s->path[path].pol.net == hb;
    cfg->peer_id = path >= 0 ? s->path[path].peer_id : 0;
    if (!cfg->peer_id && hb >= 0 && (cfg->receiver_hblink || !s->net[hb].login)) /* only for an hblink network */
        return err(c, "%s: no HBlink peer ID (the top-level id)", where);
    if (path >= 0 && s->path[path].peer_name[0]) {
        copy_str(cfg->callsign, sizeof cfg->callsign, s->path[path].peer_name);
    } else {
        char name8[9];
        memcpy(name8, cfg->name, 8); /* name[:8] */
        name8[8] = 0;
        sor(c, rdof_plan_root(p), "callsign", name8, cfg->callsign, sizeof cfg->callsign);
    }
    return true;
}

/* The parrot app's ID when one of the unit's paths may reach it (reach: the apps they reach). */
static bool unit_parrot(ctx_t *c, const rdof_site_t *s, uint32_t reach, rdof_config_t *cfg)
{
    int a = rdof_site_app_of(s, RDOF_APP_PARROT);
    cfg->has_parrot = a >= 0 && (reach >> a & 1);
    cfg->parrot = 0;
    if (!cfg->has_parrot)
        return true;
    uint32_t id = first_id(&s->app[a].tg);
    cfg->parrot = id ? id : first_id(&s->app[a].pc);
    if (cfg->parrot > 0xFFFFFF)
        return err(c, "apps.%s: %lu isn't a DMR id", s->app[a].name, (unsigned long)cfg->parrot);
    return true;
}

/* A modem's port as the radio's: a UART, or USB (a path is the host's: USB too). */
static void unit_port(const rdof_port_t *pt, rdof_cfg_port_t *out)
{
    memset(out, 0, sizeof *out);
    out->uart_tx = 5, out->uart_rx = 6, out->uart_num = 1;
    out->uart_speed = pt->baud;
    if (pt->kind == RDOF_PORT_UART) {
        snprintf(out->modem_port, sizeof out->modem_port, "uart");
        out->uart_num = pt->uart, out->uart_tx = pt->tx, out->uart_rx = pt->rx;
    } else {
        snprintf(out->modem_port, sizeof out->modem_port, "usb");
        copy_str(out->usb_serial, sizeof out->usb_serial, pt->serial);
        copy_str(out->usb_port, sizeof out->usb_port, pt->usb_port);
    }
}

static void use_port(rdof_config_t *cfg, const rdof_cfg_port_t *pt)
{
    snprintf(cfg->modem_port, sizeof cfg->modem_port, "%s", pt->modem_port);
    cfg->uart_tx = pt->uart_tx, cfg->uart_rx = pt->uart_rx, cfg->uart_num = pt->uart_num;
    cfg->uart_speed = pt->uart_speed;
    copy_str(cfg->usb_serial, sizeof cfg->usb_serial, pt->usb_serial);
    copy_str(cfg->usb_port, sizeof cfg->usb_port, pt->usb_port);
}

/* A split repeater's halves ([0] TX, [1] RX): each one's port, and app.split's SET_ROLE parameters. */
static bool unit_split(ctx_t *c, const rdof_plan_t *p, const rdof_unit_t *u, const char *where, rdof_config_t *cfg)
{
    const rdof_site_t *s = rdof_plan_site(p);
    int usb = 0, uart = 0;
    for (int k = 0; k < 2; k++) {
        int d = k ? u->rx : u->tx;
        rdof_cfg_port_t *pt = &cfg->split[k];
        if (s->dev[d].kind == RDOF_DEV_SDR) {
            rdof_port_t none = {.baud = 115200};
            unit_port(&none, pt);
            snprintf(pt->modem_port, sizeof pt->modem_port, "sdr");
        } else {
            unit_port(&rdof_plan_dev(p, d)->port, pt);
        }
        bool path = s->dev[d].kind != RDOF_DEV_SDR && rdof_plan_dev(p, d)->port.kind == RDOF_PORT_PATH;
        if (!strcmp(pt->modem_port, "usb")) {
            usb += !path; /* a path (the host's) isn't the board's one USB port */
            copy_str(cfg->usb_serial, sizeof cfg->usb_serial, pt->usb_serial);
            copy_str(cfg->usb_port, sizeof cfg->usb_port, pt->usb_port);
        } else if (!strcmp(pt->modem_port, "uart") && !uart++) {
            cfg->uart_tx = pt->uart_tx, cfg->uart_rx = pt->uart_rx, cfg->uart_num = pt->uart_num;
            cfg->uart_speed = pt->uart_speed;
        }
    }
    const char *tx = s->dev[u->tx].name, *rx = s->dev[u->rx].name;
    if (usb > 1)
        return err(c, "%s: devices.%s and devices.%s are both on USB: one half on USB at most (the other on a UART, "
                      "or the SDR as the RX half)", where, tx, rx);
    if (uart > 1) {
        const rdof_cfg_port_t *a = &cfg->split[0], *b = &cfg->split[1];
        if (a->uart_num == b->uart_num)
            return err(c, "%s: devices.%s and devices.%s are both on UART%d", where, tx, rx, a->uart_num);
        if (a->uart_tx == b->uart_tx || a->uart_rx == b->uart_rx || a->uart_tx == b->uart_rx || a->uart_rx == b->uart_tx)
            return err(c, "%s: devices.%s and devices.%s share a UART pin", where, tx, rx);
    }
    int sp = get(c, rdof_plan_app_node(p, u->app), "split");
    struct {
        const char *k;
        int *p;
    } params[] = {{"power_cap", &cfg->split_power_cap}, {"prebuffer", &cfg->split_prebuffer},
                  {"lease_s", &cfg->split_lease_s}, {"cooldown_s", &cfg->split_cooldown_s}};
    for (size_t k = 0; k < sizeof params / sizeof params[0]; k++) {
        double v;
        int i = get(c, sp, params[k].k);
        if (i < 0)
            continue;
        if (!num(c, i, &v) || !(v >= 0 && v <= 255)) /* NaN too */
            return err(c, "%s.split.%s: 0 to 255", where, params[k].k);
        *params[k].p = (int)v;
    }
    int i = get(c, sp, "max_key_s");
    if (i >= 0 && (!num(c, i, &cfg->split_max_key_s) || !(cfg->split_max_key_s >= 0 && cfg->split_max_key_s <= 600)))
        return err(c, "%s.split.max_key_s: 0 to 600", where);
    snprintf(cfg->modem_port, sizeof cfg->modem_port, "split");
    return true;
}

/* A plan channel: a frequency of the site as a roamer's channel, its power_dbm as the unit's transmitter's level
   (255 when it has none). */
static bool unit_channel(ctx_t *c, const rdof_plan_t *p, int f, bool with_power, const char *where,
                         rdof_cfg_channel_t *out)
{
    const rdof_freq_t *fr = &rdof_plan_site(p)->freq[f];
    int cc = freq_cc(c, p, f);
    if (cc < 0 || cc > 15)
        return err(c, "%s: frequencies.%s has no color code", where, fr->name);
    return channel_entry(c, fr->hz, cc, with_power ? rdof_txpower_level(&c->map, fr->power_dbm) : 255, fr->ts, out);
}

/* The frequencies the board's SDRs hear (its receivers' and the units' on them), sorted: a roamer leaves those to
   the SDR. */
static void unit_covered(const rdof_plan_t *p, rdof_config_t *cfg)
{
    const rdof_site_t *s = rdof_plan_site(p);
    cfg->ncovered = 0;
    for (int d = 0; d < s->ndev; d++) {
        if (s->dev[d].kind != RDOF_DEV_SDR || !rdof_plan_sdr_used(p, d))
            continue;
        uint32_t f[RDOF_SITE_MAX_FREQS + 64];
        int n = 0, r = rdof_plan_receiver(p, d);
        for (int x = 0; r >= 0 && x < s->nfreq; x++)
            if (rdof_plan_unit(p, r)->freqs >> x & 1)
                f[n++] = s->freq[x].hz;
        for (int x = 0; x < rdof_plan_nextra(p); x++)
            if (rdof_plan_extra(p, x)->dev == d)
                f[n++] = rdof_plan_extra(p, x)->hz;
        for (int k = 0; k < n; k++) {
            bool dup = false;
            for (int q = 0; q < cfg->ncovered; q++)
                dup |= cfg->covered[q] == f[k];
            if (!dup && cfg->ncovered < RDOF_CFG_CHANNELS)
                cfg->covered[cfg->ncovered++] = f[k];
        }
    }
    for (int a = 1; a < cfg->ncovered; a++) /* sorted */
        for (int b = a; b > 0 && cfg->covered[b - 1] > cfg->covered[b]; b--) {
            uint32_t t = cfg->covered[b];
            cfg->covered[b] = cfg->covered[b - 1];
            cfg->covered[b - 1] = t;
        }
}

/* A transceiver: the frequencies its device transmits on; the talker keys on the device. */
static bool unit_roamer(ctx_t *c, const rdof_plan_t *p, const rdof_unit_t *u, const char *where, rdof_config_t *cfg)
{
    const rdof_site_t *s = rdof_plan_site(p);
    const rdof_device3_t *dv = &s->dev[u->rx];
    int r = rdof_plan_dev_node(p, u->rx);
    cfg->mode = RDOF_CFG_ROAMER;
    cfg->nchannels = 0;
    uint32_t reach = 0;
    for (int f = 0; f < s->nfreq; f++) {
        if (!(u->freqs >> f & 1))
            continue;
        if (cfg->nchannels >= RDOF_CFG_CHANNELS)
            return err(c, "%s.transmits: %d frequencies at most", where, RDOF_CFG_CHANNELS);
        if (!unit_channel(c, p, f, true, where, &cfg->channels[cfg->nchannels++]))
            return false;
        int pa = rdof_site_path_rx(s, s->freq[f].hz, s->freq[f].ts);
        if (pa >= 0)
            reach |= s->path[pa].pol.apps;
    }
    if (!cfg->nchannels)
        return err(c, "%s: no frequencies to transmit on", where);
    if (!unit_channel(c, p, u->freq, false, where, &cfg->idle))
        return false;
    cfg->oled_channel = u->freq + 1;
    if (!unit_peer(c, p, rdof_site_path_rx(s, s->freq[u->freq].hz, s->freq[u->freq].ts), where, cfg))
        return false;
    struct {
        const char *k;
        double d;
        double *p;
    } fl[] = {{"listen_s", 0.3, &cfg->listen_s}, {"clear_s", 2.0, &cfg->clear_s}, {"answer_s", 0.75, &cfg->answer_s},
              {"answer_max_s", 5.0, &cfg->answer_max_s}, {"idle_after_s", 0, &cfg->idle_after_s},
              {"cwid_every_s", 900, &cfg->cwid_every_s}};
    for (size_t k = 0; k < sizeof fl / sizeof fl[0]; k++)
        if (!fget(c, r, fl[k].k, fl[k].d, fl[k].p, where))
            return false;
    if (!jitter(c, r, where, cfg))
        return false;
    long lv;
    if (!iget(c, r, "csbk_preambles", 2, &lv, where))
        return false;
    cfg->csbk_preambles = (int)lv;
    sget(c, r, "cwid", "", cfg->cwid, sizeof cfg->cwid);
    if (!unit_parrot(c, s, reach, cfg))
        return false;
    unit_covered(p, cfg);
    cfg->has_scan = dv->hears == RDOF_HEARS_SCAN;
    if (cfg->has_scan) {
        cfg->nscan = 0;
        for (int f = 0; f < s->nfreq; f++) {
            if (!(dv->scan >> f & 1))
                continue;
            if (cfg->nscan >= RDOF_CFG_CHANNELS)
                return err(c, "%s.hears.scan: %d frequencies at most", where, RDOF_CFG_CHANNELS);
            if (!unit_channel(c, p, f, false, where, &cfg->scan_channels[cfg->nscan++]))
                return false;
        }
        char sw[80];
        snprintf(sw, sizeof sw, "%s.hears", where);
        int h = get(c, r, "hears");
        if (!cfg->nscan)
            return err(c, "%s.scan: no frequencies", sw);
        if (!fget(c, h, "dwell_s", 0.45, &cfg->dwell_s, sw) || !fget(c, h, "hang_s", 3.0, &cfg->hang_s_scan, sw) ||
            !fget(c, h, "carrier_s", 0, &cfg->carrier_s, sw))
            return false;
        if (cfg->dwell_s <= 0 || cfg->hang_s_scan < 0 || !(0 <= cfg->carrier_s && cfg->carrier_s < cfg->dwell_s))
            return err(c, "%s: dwell_s > 0, hang_s >= 0, and carrier_s (0 = off) under dwell_s", sw);
    }
    return true;
}

/* A repeater app, or an SFR or DCDM frequency: the repeater's code. r: the app's or the frequency's object. */
static bool unit_repeater(ctx_t *c, const rdof_plan_t *p, const rdof_unit_t *u, const char *where, rdof_config_t *cfg)
{
    const rdof_site_t *s = rdof_plan_site(p);
    bool sfr = u->kind == RDOF_UNIT_SFR, dcdm = u->kind == RDOF_UNIT_DCDM;
    int r, in, out, p0, p1;
    if (u->kind == RDOF_UNIT_REPEATER) {
        const rdof_app3_t *ap = &s->app[u->app];
        r = rdof_plan_app_node(p, u->app);
        in = ap->in, out = ap->out, p0 = ap->path[0], p1 = ap->path[1];
    } else {
        r = rdof_plan_freq_node(p, u->freq);
        in = out = u->freq;
        p0 = rdof_site_path_rx(s, s->freq[in].hz, 1);
        p1 = rdof_site_path_rx(s, s->freq[in].hz, 2);
    }
    cfg->mode = RDOF_CFG_REPEATER;
    cfg->rx_freq = s->freq[in].hz;
    cfg->tx_freq = s->freq[out].hz;
    if (s->freq[out].cc < 0 || s->freq[out].cc > 15)
        return err(c, "%s: frequencies.%s has no color code", where, s->freq[out].name);
    cfg->cc = s->freq[out].cc;
    if (!unit_peer(c, p, p0, where, cfg))
        return false;
    for (int f = 0; f < s->nfreq; f++)
        if (!s->freq[f].tx && s->freq[f].hz == cfg->tx_freq)
            return err(c, "%s: frequencies.%s has tx false: nothing transmits on it", where, s->freq[f].name);
    cfg->power = rdof_txpower_level(&c->map, s->freq[out].power_dbm);
    if (!fget(c, r, "hang_s", 5.0, &cfg->hang_s, where) || !fget(c, r, "call_hang_s", 3.0, &cfg->call_hang_s, where) ||
        !fget(c, r, "cwid_every_s", 900, &cfg->cwid_every_s, where))
        return false;
    if (!jitter(c, r, where, cfg))
        return false;
    sget(c, r, "cwid", "", cfg->cwid, sizeof cfg->cwid);
    if (!unit_parrot(c, s, (p0 >= 0 ? s->path[p0].pol.apps : 0) | (p1 >= 0 ? s->path[p1].pol.apps : 0), cfg))
        return false;
    cfg->short_lc_ts1_low = bget(c, r, "short_lc_ts1_low", false);
    cfg->sfr = sfr;
    cfg->dcdm = dcdm;
    cfg->inbound_slot = 1;
    if (sfr) {
        long inbound;
        if (!iget(c, r, "inbound_slot", 1, &inbound, where))
            return false;
        if (inbound != 1 && inbound != 2)
            return err(c, "%s.inbound_slot: 1 or 2", where);
        cfg->inbound_slot = (int)inbound;
        cfg->slot1 = inbound == 2;
        cfg->slot2 = inbound == 1;
    } else {
        int a = get(c, r, "active_slots");
        cfg->slot1 = cfg->slot2 = a < 0;
        for (int k = 0; a >= 0 && k < N(c, a)->count; k++) { /* checked in the parse: [1], [2] or [1, 2] */
            double v;
            if (num(c, rdof_jat(c->doc, a, k), &v))
                *(to_long(v) == 1 ? &cfg->slot1 : &cfg->slot2) = true;
        }
        if (!sfr && !dcdm && !strcmp(cfg->radio_type, "radio_hotspot"))
            return err(c, "%s: devices.%s is a radio_hotspot (a radio in hotspot mode), which is simplex: a repeater "
                          "needs a duplex MMDVM_HS board, an MMDVM board or two radios as a split (or a frequency "
                          "with mode sfr)", where, s->dev[u->rx].name);
    }
    if (cfg->hang_s <= 0 || cfg->call_hang_s < 0)
        return err(c, "%s: hang_s > 0 and call_hang_s >= 0", where);
    /* slot timing (docs/spec/slot-timing.md): the board's reaches a duplex repeater on a split or the SDR; a modem
       or an SFR radio keeps its own slot clock, so utc is refused there */
    bool timed = !sfr && !dcdm && (!strcmp(cfg->radio_type, "split") || !strcmp(cfg->radio_type, "auto"));
    char why[96];
    if (!rdof_slot_timing_parse(c->doc, timed ? get(c, rdof_plan_root(p), "slot_timing") : -1,
                                get(c, r, "slot_timing"), &cfg->slot_timing, why, sizeof why))
        return err(c, "%s.%s", where, why);
    if (cfg->slot_timing.utc && sfr)
        return err(c, "%s.slot_timing: utc: a single frequency repeater's radio times its own slots", where);
    if (cfg->slot_timing.utc && dcdm)
        return err(c, "%s.slot_timing: utc: in dual capacity direct mode the modem times its slots from the channel",
                   where);
    if (cfg->slot_timing.utc && !timed)
        return err(c, "%s.slot_timing: utc: a %s keeps its own slot clock (its firmware times the slots, and MMDVM "
                      "has no command to move it): only a split or the board's SDR can follow the UTC grid",
                   where, cfg->radio_type);
    cfg->oled_channel = oled_number(s, cfg->rx_freq);
    return true;
}

static bool unit_mqtt(ctx_t *c, const rdof_plan_t *p, rdof_config_t *cfg)
{
    int k = rdof_plan_net_of(p, RDOF_NET_MQTT);
    const rdof_net3_t *n = k >= 0 ? &rdof_plan_site(p)->net[k] : NULL;
    cfg->has_mqtt = n && (n->report || n->calls);
    if (!cfg->has_mqtt)
        return true;
    int m = rdof_plan_net_node(p, k);
    char what[RDOF_SITE_NAME + 16];
    snprintf(what, sizeof what, "networks.%s", n->name);
    long port;
    sget(c, m, "host", "localhost", cfg->mqtt_host, sizeof cfg->mqtt_host);
    if (!iget(c, m, "port", 1883, &port, what))
        return false;
    cfg->mqtt_port = (int)port;
    sor(c, m, "username", "", cfg->mqtt_user, sizeof cfg->mqtt_user);
    sor(c, m, "password", "", cfg->mqtt_pass, sizeof cfg->mqtt_pass);
    sget(c, m, "root", "DMR", cfg->mqtt_root, sizeof cfg->mqtt_root);
    size_t rl = strlen(cfg->mqtt_root), lead = strspn(cfg->mqtt_root, "/"); /* slashes stripped, both ends */
    while (rl > lead && cfg->mqtt_root[rl - 1] == '/')
        rl--;
    memmove(cfg->mqtt_root, cfg->mqtt_root + lead, rl - lead);
    cfg->mqtt_root[rl - lead] = 0;
    snprintf(cfg->mqtt_app, sizeof cfg->mqtt_app, "%s", cfg->mode == RDOF_CFG_ROAMER ? "roaming" : "repeater");
    cfg->mqtt_tls = bget(c, m, "tls", false) || n->transport == RDOF_MQTT_TLS;
    return true;
}

/* A unit's place in the settings, for its errors. */
static void unit_where(const rdof_plan_t *p, const rdof_unit_t *u, char *out, size_t cap)
{
    const rdof_site_t *s = rdof_plan_site(p);
    if (u->kind == RDOF_UNIT_REPEATER)
        snprintf(out, cap, "apps.%s", s->app[u->app].name);
    else if (u->kind == RDOF_UNIT_SFR || u->kind == RDOF_UNIT_DCDM)
        snprintf(out, cap, "frequencies.%s", s->freq[u->freq].name);
    else
        snprintf(out, cap, "devices.%s", s->dev[u->rx].name);
}

bool rdof_plan_radio(const rdof_plan_t *p, const char *name, rdof_config_t *cfg, char *error, size_t error_cap)
{
    char dummy[8];
    ctx_t cx = {.doc = rdof_plan_doc(p), .error = error ? error : dummy, .error_cap = error ? error_cap : sizeof dummy,
                .mm = {-1, -1}};
    ctx_t *c = &cx;
    if (c->error_cap)
        c->error[0] = 0;
    memset(cfg, 0, sizeof *cfg);
    const rdof_site_t *s = rdof_plan_site(p);
    int k = -1;
    for (int i = 0; i < rdof_plan_nunits(p) && k < 0; i++)
        if (rdof_plan_unit(p, i)->kind != RDOF_UNIT_RECEIVER &&
            (!name || !strncmp(rdof_plan_unit(p, i)->name, name, sizeof cfg->name - 1)))
            k = i;
    if (k < 0)
        return name ? err(c, "%s isn't a unit that transmits (a repeater, an SFR or DCDM frequency, or a device "
                             "transmitting on simplex frequencies)", name)
                    : err(c, "nothing transmits: no repeater, SFR or DCDM frequency, or device transmitting");
    const rdof_unit_t *u = rdof_plan_unit(p, k);
    int root = rdof_plan_root(p);
    char where[RDOF_SITE_NAME + 16];
    unit_where(p, u, where, sizeof where);
    copy_str(cfg->name, sizeof cfg->name, u->name);
    {
        rdof_slot_timing_t t; /* the board's, checked whether or not anything inherits it */
        char why[96];
        if (!rdof_slot_timing_parse(c->doc, -1, get(c, root, "slot_timing"), &t, why, sizeof why))
            return err(c, "%s", why);
    }
    /* the network */
    int hbk = rdof_plan_net_of(p, RDOF_NET_HBLINK);
    int hb = rdof_plan_net_node(p, hbk);
    long port;
    char hbw[RDOF_SITE_NAME + 16];
    snprintf(hbw, sizeof hbw, "networks.%s", hbk >= 0 ? s->net[hbk].name : "hblink");
    sget(c, hb, "host", "localhost", cfg->master_host, sizeof cfg->master_host);
    if (!iget(c, hb, "port", 62031, &port, hbw))
        return false;
    cfg->master_port = (int)port;
    sget(c, hb, "transport", "udp", cfg->transport, sizeof cfg->transport);
    cfg->hb_login = hbk >= 0 && s->net[hbk].login; /* brandmeister: RPTL and the rest (rdof/hbpeer.h) */
    sget(c, hb, "password", "", cfg->hb_password, sizeof cfg->hb_password);
    sget(c, hb, "location", "", cfg->hb_location, sizeof cfg->hb_location);
    sget(c, hb, "description", "", cfg->hb_description, sizeof cfg->hb_description);
    sget(c, hb, "url", "", cfg->hb_url, sizeof cfg->hb_url);
    if (!sctp_settings(c, hb, hbw, cfg) || !location(c, get(c, root, "location"), cfg))
        return false;
    /* the radio: the board's SDR (an sdr_transceiver), one modem, or a split's two */
    bool split = u->rx != u->tx, on_sdr = !split && s->dev[u->rx].kind == RDOF_DEV_SDR;
    c->mm[0] = get(c, rdof_plan_dev_node(p, u->tx), "mmdvm");
    c->mm[1] = get(c, rdof_plan_dev_node(p, u->rx), "mmdvm");
    char mmw[RDOF_SITE_NAME + 24];
    snprintf(mmw, sizeof mmw, "devices.%s.mmdvm", s->dev[c->mm[0] >= 0 ? u->tx : u->rx].name);
    c->mmw = mmw;
    if (!radio_settings(c, cfg))
        return false;
    const char *drv = s->dev[u->rx].driver;
    snprintf(cfg->radio_type, sizeof cfg->radio_type, "%s", on_sdr ? "auto" : split ? "split" : drv);
    if (!split && !known_type(cfg->radio_type))
        return err(c, "devices.%s.type %s", s->dev[u->rx].name, cfg->radio_type);
    cfg->sdr_duplex = RDOF_SDR_DUPLEX_NONE;
    cfg->tx_power_dbm = 10;
    if (on_sdr) {
        const rdof_plan_dev_t *dv = rdof_plan_dev(p, u->rx);
        cfg->sdr_duplex = u->kind == RDOF_UNIT_REPEATER || u->duplex_full ? RDOF_SDR_DUPLEX_FULL
                                                                          : RDOF_SDR_DUPLEX_HALF;
        if (dv->has_tx_power)
            cfg->tx_power_dbm = dv->tx_power_dbm;
    }
    cfg->tx_map = c->map = rdof_plan_dev(p, u->tx >= 0 ? u->tx : u->rx)->txmap;
    if (u->kind == RDOF_UNIT_TRANSCEIVER ? !unit_roamer(c, p, u, where, cfg) : !unit_repeater(c, p, u, where, cfg))
        return false;
    if (!unit_mqtt(c, p, cfg) || !oled(c, root, cfg))
        return false;
    /* the port */
    rdof_cfg_port_t pt;
    rdof_port_t none = {.baud = 115200};
    unit_port(&none, &pt);
    if (on_sdr)
        snprintf(pt.modem_port, sizeof pt.modem_port, "sdr");
    else if (split)
        snprintf(pt.modem_port, sizeof pt.modem_port, "uart");
    else
        unit_port(&rdof_plan_dev(p, u->rx)->port, &pt);
    use_port(cfg, &pt);
    if (split && !unit_split(c, p, u, where, cfg))
        return false;
    return true;
}

/* The text forms: a plan each. */
bool rdof_config_parse_radio(const char *text, size_t len, const char *name, rdof_config_t *cfg, char *error,
                             size_t error_cap)
{
    rdof_plan_t *p = rdof_plan_open(text, len, error, error_cap);
    if (!p) {
        memset(cfg, 0, sizeof *cfg);
        return false;
    }
    bool ok = rdof_plan_radio(p, name, cfg, error, error_cap);
    rdof_plan_close(p);
    return ok;
}

bool rdof_config_parse(const char *text, size_t len, rdof_config_t *cfg, char *error, size_t error_cap)
{
    return rdof_config_parse_radio(text, len, NULL, cfg, error, error_cap);
}

bool rdof_box_roles(const char *text, size_t len, rdof_roles_t *roles, char *error, size_t error_cap)
{
    rdof_plan_t *p = rdof_plan_open(text, len, error, error_cap);
    if (!p) {
        memset(roles, 0, sizeof *roles);
        return false;
    }
    bool ok = rdof_plan_roles(p, roles, error, error_cap);
    rdof_plan_close(p);
    return ok;
}

int rdof_config_ports(const rdof_config_t *cfg, rdof_cfg_port_t out[2])
{
    if (!strcmp(cfg->modem_port, "split")) {
        out[0] = cfg->split[0];
        out[1] = cfg->split[1];
        return 2;
    }
    memset(&out[0], 0, sizeof out[0]);
    snprintf(out[0].modem_port, sizeof out[0].modem_port, "%s", cfg->modem_port);
    out[0].uart_tx = cfg->uart_tx, out[0].uart_rx = cfg->uart_rx, out[0].uart_num = cfg->uart_num;
    out[0].uart_speed = cfg->uart_speed;
    snprintf(out[0].usb_serial, sizeof out[0].usb_serial, "%s", cfg->usb_serial);
    snprintf(out[0].usb_port, sizeof out[0].usb_port, "%s", cfg->usb_port);
    return 1;
}

bool rdof_config_uses_usb(const rdof_config_t *cfg)
{
    rdof_cfg_port_t p[2];
    int n = rdof_config_ports(cfg, p);
    for (int k = 0; k < n; k++)
        if (!strcmp(p[k].modem_port, "usb"))
            return true;
    return false;
}

/* Two ports (of units a and b) that can't both be: false with error set. */
static bool ports_apart(const char *a_name, const rdof_cfg_port_t *a, const char *b_name, const rdof_cfg_port_t *b,
                        char *error, size_t cap)
{
    if (strcmp(a->modem_port, b->modem_port))
        return true;
    if (!strcmp(a->modem_port, "sdr")) {
        snprintf(error, cap, "%s and %s both use the SDR's modem: the board has one SDR", a_name, b_name);
        return false;
    }
    if (!strcmp(a->modem_port, "uart")) {
        if (a->uart_num == b->uart_num) {
            snprintf(error, cap, "%s and %s are both on UART%d: give one's device \"port\": {\"uart\": %d, ...}",
                     a_name, b_name, a->uart_num, 3 - a->uart_num);
            return false;
        }
        if (a->uart_tx == b->uart_tx || a->uart_rx == b->uart_rx || a->uart_tx == b->uart_rx ||
            a->uart_rx == b->uart_tx) {
            snprintf(error, cap, "%s and %s share a UART pin", a_name, b_name);
            return false;
        }
    } else if ((!a->usb_serial[0] && !a->usb_port[0]) || (!b->usb_serial[0] && !b->usb_port[0])) {
        snprintf(error, cap, "%s and %s are both on USB: tell their devices apart with \"serial\" or \"usb_port\" in "
                 "each one's port", a_name, b_name);
        return false;
    } else if ((a->usb_serial[0] && !strcmp(a->usb_serial, b->usb_serial) &&
                (!a->usb_port[0] || !b->usb_port[0] || !strcmp(a->usb_port, b->usb_port))) ||
               (!a->usb_serial[0] && !b->usb_serial[0] && !strcmp(a->usb_port, b->usb_port))) {
        snprintf(error, cap, "%s and %s would match the same USB device", a_name, b_name);
        return false;
    }
    return true;
}

bool rdof_config_check_ports(const rdof_config_t *const *cfgs, int n, char *error, size_t cap)
{
    if (!error) /* none wanted, as rdof_config_parse's */
        cap = 0;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) {
            rdof_cfg_port_t a[2], b[2];
            int na = rdof_config_ports(cfgs[i], a), nb = rdof_config_ports(cfgs[j], b);
            for (int x = 0; x < na; x++)
                for (int y = 0; y < nb; y++)
                    if (!ports_apart(cfgs[i]->name, &a[x], cfgs[j]->name, &b[y], error, cap))
                        return false;
        }
    return true;
}

/* ── describe ──────────────────────────────────────────────────────────────── */

typedef struct {
    char *out;
    size_t cap, len;
} sb_t;

static void add(sb_t *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s->out + s->len, s->len < s->cap ? s->cap - s->len : 0, fmt, ap);
    va_end(ap);
    if (n > 0)
        s->len += (size_t)n;
    if (s->len >= s->cap)
        s->len = s->cap ? s->cap - 1 : 0;
}

static const char *f3(double v, char *buf)
{
    rdof_format_float3(v, buf, 40);
    return buf;
}

/* jitter=auto:min-max (ms) or fixed (at prebuffer) */
static void add_jitter(sb_t *s, const rdof_config_t *c)
{
    if (c->jitter_auto)
        add(s, "jitter=auto:%d-%d;", c->jitter_min_ms, c->jitter_max_ms);
    else
        add(s, "jitter=fixed;");
}

size_t rdof_config_describe(const rdof_config_t *c, char *out, size_t cap)
{
    sb_t s = {out, cap, 0};
    char a[40], b[40], d[40];
    if (cap)
        out[0] = 0;
    add(&s, "mode=%s;name=%s;peer=%lu;callsign=%s;master=%s:%d;transport=%s;hblink=%d;type=%s;",
        c->mode == RDOF_CFG_ROAMER ? "roamer" : "repeater", c->name, (unsigned long)c->peer_id, c->callsign,
        c->master_host, c->master_port, c->transport, c->receiver_hblink, c->radio_type);
    if (!strcmp(c->transport, "sctp")) /* the SCTP settings */
        add(&s, "sctp=encap:%d,heartbeat:%d,retransmit:%d,ttl:%d;", c->sctp_encap_port, c->sctp_heartbeat_ms,
            c->sctp_max_retransmit, c->sctp_ttl_ms);
    add(&s, "modem=%d,%d,%d,%d,%s,%s,%s,%d,%d,%d;offsets=%ld,%ld;", c->modem.rx_invert, c->modem.tx_invert,
        c->modem.ptt_invert, c->modem.tx_delay_ms, f3(c->modem.rx_level, a), f3(c->modem.tx_level, b),
        f3(c->modem.cwid_level, d), c->modem.rx_dc_offset, c->modem.tx_dc_offset, c->modem.dmr_delay,
        (long)c->rx_offset_hz, (long)c->tx_offset_hz);
    add(&s, "tuner=%s:%lu:%d;rssi=", c->tuner == RDOF_TUNER_MODEM ? "modem" : c->tuner == RDOF_TUNER_FIXED ? "fixed" : "default",
        (unsigned long)c->tuner_freq, c->settle_ms);
    for (int i = 0; i < c->rssi.n && i < RDOF_RSSI_POINTS; i++) /* a count past its array: as far as it goes */
        add(&s, "%s%d:%d", i ? "," : "", c->rssi.raw[i], c->rssi.dbm[i]);
    add(&s, ";headers=%d;terminators=%d;", c->headers, c->terminators);
    if (c->mode == RDOF_CFG_ROAMER) {
        add(&s, "channels=");
        for (int i = 0; i < c->nchannels && i < RDOF_CFG_CHANNELS; i++)
            add(&s, "%s%lu:%d:%d", i ? "," : "", (unsigned long)c->channels[i].freq, c->channels[i].cc,
                c->channels[i].power);
        add(&s, ";idle=%lu:%d:%d;", (unsigned long)c->idle.freq, c->idle.cc, c->idle.power);
        add(&s, "listen=%s;", f3(c->listen_s, a));
        add(&s, "clear=%s;prebuffer=%d;", f3(c->clear_s, a), c->prebuffer);
        add_jitter(&s, c);
        add(&s, "preambles=%d;", c->csbk_preambles);
        add(&s, "answer=%s,", f3(c->answer_s, a));
        add(&s, "%s;idle_after=", f3(c->answer_max_s, a));
        add(&s, "%s;covered=", f3(c->idle_after_s, a));
        for (int i = 0; i < c->ncovered && i < RDOF_CFG_CHANNELS; i++)
            add(&s, "%s%lu", i ? "," : "", (unsigned long)c->covered[i]);
        add(&s, ";scan=");
        if (c->has_scan) {
            for (int i = 0; i < c->nscan && i < RDOF_CFG_CHANNELS; i++)
                add(&s, "%s%lu:%d", i ? "," : "", (unsigned long)c->scan_channels[i].freq, c->scan_channels[i].cc);
            add(&s, "|%s,", f3(c->dwell_s, a));
            add(&s, "%s,", f3(c->hang_s_scan, a));
            add(&s, "%s", f3(c->carrier_s, a));
        } else {
            add(&s, "none");
        }
        add(&s, ";");
    } else {
        char tm[64];
        rdof_slot_timing_describe(&c->slot_timing, tm, sizeof tm);
        add(&s, "rx=%lu;tx=%lu;cc=%d;power=%d;slots=%d%d;hang=%s,", (unsigned long)c->rx_freq,
            (unsigned long)c->tx_freq, c->cc, c->power, c->slot1, c->slot2, f3(c->hang_s, a));
        add(&s, "%s;prebuffer=%d;", f3(c->call_hang_s, a), c->prebuffer);
        add_jitter(&s, c);
        add(&s, "sfr=%d;inbound=%d;ts1low=%d;timing=%s;", c->sfr, c->inbound_slot, c->short_lc_ts1_low, tm);
    }
    add(&s, "cwid=%s,%s;", c->cwid, f3(c->cwid_every_s, a));
    if (c->has_parrot)
        add(&s, "parrot=%lu;", (unsigned long)c->parrot);
    if (c->has_location)
        add(&s, "location=%.6f,%.6f,%d;", c->location.latitude, c->location.longitude, c->location.height_m);
    else
        add(&s, "location=none;");
    if (c->has_mqtt)
        add(&s, "mqtt=%s:%d:%s:%s:%s:%s:%d;", c->mqtt_host, c->mqtt_port, c->mqtt_user, c->mqtt_pass, c->mqtt_root,
            c->mqtt_app, c->mqtt_tls);
    else
        add(&s, "mqtt=none;");
    if (c->has_oled)
        add(&s, "oled=%d,%d,%d,%s,%d", c->oled_address, c->oled_sh1106, c->oled_rotate180, f3(c->oled_refresh_s, a),
            c->oled_channel);
    else
        add(&s, "oled=none");
    return s.len;
}
