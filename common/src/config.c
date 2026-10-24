// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* Box config JSON -> one roamer or repeater (rdof/config.h, docs/spec/fw/config.md). */
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
#include "rdof/status.h"

#define MAX_NODES 1200
#define STR_BUF 8192

typedef struct {
    rdof_jdoc_t doc;
    rdof_jnode_t nodes[MAX_NODES];
    char strs[STR_BUF];
    char *text; /* comments stripped */
    char *error;
    size_t error_cap;
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
    return rdof_jnode(&c->doc, i);
}

static int get(ctx_t *c, int obj, const char *key)
{
    return rdof_jget(&c->doc, obj, key);
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

/* box._hz: MHz below 1e5 */
static bool box_hz(ctx_t *c, int i, uint32_t *out, const char *what)
{
    double f;
    if (!num(c, i, &f))
        return err(c, "%s: bad frequency", what);
    *out = to_u32(rint(f < 1e5 ? f * 1e6 : f));
    return true;
}

/* mmdvm.config.freq_hz: MHz below 10000 */
static uint32_t freq_hz(double f)
{
    return to_u32(rint(f < 10000 ? f * 1e6 : f));
}

/* ── the plan ──────────────────────────────────────────────────────────────── */

typedef struct {
    int number;
    uint32_t freq;
    int cc, power;
} plan_t;

typedef struct {
    plan_t ch[RDOF_CFG_CHANNELS * 2];
    int n;
} plan_list_t;

static const plan_t *plan_find(const plan_list_t *p, long number)
{
    for (int i = 0; i < p->n; i++)
        if (p->ch[i].number == number)
            return &p->ch[i];
    return NULL;
}

/* A radio's channel entry -> (plan channel, override object or -1). */
static bool plan_ref(ctx_t *c, const plan_list_t *plan, int ref, const plan_t **pc, int *over)
{
    long n;
    *over = -1;
    if (N(c, ref)->type == RDOF_JOBJ) {
        *over = ref;
        if (!iget(c, ref, "ch", 0, &n, "channel") || get(c, ref, "ch") < 0)
            return err(c, "a channel entry needs ch");
    } else {
        double v;
        if (!num(c, ref, &v))
            return err(c, "bad channel reference");
        n = to_long(v);
    }
    *pc = plan_find(plan, n);
    if (!*pc)
        return err(c, "channel %ld isn't in the plan (channels:)", n);
    return true;
}

static int cmp_plan(const void *a, const void *b)
{
    int x = ((const plan_t *)a)->number, y = ((const plan_t *)b)->number;
    return (x > y) - (x < y);
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

static const char *const ROAMER_KEYS[] = {"type", "name", "peer_id", "callsign", "idle", "channels", "listen_s",
                                          "clear_s", "prebuffer", "jitter_buffer", "jitter_min_ms", "jitter_max_ms", "idle_after_s", "cwid", "cwid_every_s", "hblink",
                                          "location", "radio", "scan", "csbk_preambles", "answer_s", "answer_max_s",
                                          NULL};
static const char *const REPEATER_KEYS[] = {"type", "name", "peer_id", "callsign", "rx_freq", "tx_freq", "cc",
                                            "power", "slots", "hang_s", "call_hang_s", "prebuffer", "jitter_buffer",
                                            "jitter_min_ms", "jitter_max_ms", "cwid",
                                            "cwid_every_s", "hblink", "location", "radio", "mode", "freq",
                                            "inbound_slot", "short_lc_ts1_low", "slot_timing", NULL};

/* The jitter buffer: jitter_buffer "auto" (the default) or a fixed size in ms, rounded up to whole 60 ms bursts;
 * prebuffer, the older key, a fixed size in bursts. jitter_min_ms and jitter_max_ms bound auto. */
static bool jitter(ctx_t *c, int r, rdof_config_t *cfg)
{
    long lv;
    int jb = get(c, r, "jitter_buffer"), pb = get(c, r, "prebuffer");
    if (jb >= 0 && pb >= 0)
        return err(c, "radio: prebuffer and jitter_buffer: set one (prebuffer is a fixed size in bursts)");
    if (!iget(c, r, "prebuffer", 6, &lv, "radio"))
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
            return err(c, "radio: jitter_buffer: \"auto\" or milliseconds, 0 to %d", RDOF_JB_MAX_MS);
        }
    }
    double lo, hi;
    if (!fget(c, r, "jitter_min_ms", RDOF_JB_MIN_MS, &lo, "radio") ||
        !fget(c, r, "jitter_max_ms", RDOF_JB_MAX_DEFAULT_MS, &hi, "radio"))
        return false;
    if (!(lo >= RDOF_JB_BURST_MS && lo <= hi && hi <= RDOF_JB_MAX_MS))
        return err(c, "radio: jitter_min_ms and jitter_max_ms: %d <= min <= max <= %d", RDOF_JB_BURST_MS, RDOF_JB_MAX_MS);
    cfg->jitter_min_ms = rdof_jb_bursts(lo) * RDOF_JB_BURST_MS;
    cfg->jitter_max_ms = rdof_jb_bursts(hi) * RDOF_JB_BURST_MS;
    return true;
}

static bool own_key(const char *const *keys, const char *k)
{
    for (; *keys; keys++)
        if (!strcmp(*keys, k))
            return true;
    return false;
}

/* A radio setting: the section's own key (not one of its section keys) wins over radio.{key}. */
static int rget(ctx_t *c, int r, const char *const *keys, const char *key)
{
    if (!own_key(keys, key)) {
        int i = get(c, r, key);
        if (i >= 0)
            return i;
    }
    return get(c, get(c, r, "radio"), key);
}

static bool radio_settings(ctx_t *c, int r, const char *const *keys, rdof_config_t *cfg)
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
        i = rget(c, r, keys, f[k].k);
        if (i < 0)
            continue;
        if (f[k].t == 0) {
            *(bool *)f[k].p = truthy(c, i);
        } else {
            if (!num(c, i, &v))
                return err(c, "radio.%s isn't a number", f[k].k);
            if (f[k].t == 1)
                *(int *)f[k].p = to_int(v);
            else
                *(double *)f[k].p = v;
        }
    }
    cfg->rx_offset_hz = cfg->tx_offset_hz = 0;
    if ((i = rget(c, r, keys, "rx_offset_hz")) >= 0 && num(c, i, &v))
        cfg->rx_offset_hz = to_int(v);
    if ((i = rget(c, r, keys, "tx_offset_hz")) >= 0 && num(c, i, &v))
        cfg->tx_offset_hz = to_int(v);
    cfg->headers = cfg->terminators = -1;
    if ((i = rget(c, r, keys, "headers")) >= 0 && N(c, i)->type != RDOF_JNULL && num(c, i, &v))
        cfg->headers = to_int(v);
    if ((i = rget(c, r, keys, "terminators")) >= 0 && N(c, i)->type != RDOF_JNULL && num(c, i, &v))
        cfg->terminators = to_int(v);
    /* tuner: "fixed" / {type, freq, settle_ms}; rigctld and command tuners need a computer */
    cfg->tuner = RDOF_TUNER_NONE; /* the kind's default */
    cfg->tuner_freq = 0;
    cfg->settle_ms = 0;
    i = rget(c, r, keys, "tuner");
    if (truthy(c, i)) {
        char t[16] = "";
        if (N(c, i)->type == RDOF_JSTR) {
            snprintf(t, sizeof t, "%s", N(c, i)->str);
        } else {
            sget(c, i, "type", "", t, sizeof t);
            if (get(c, i, "freq") >= 0) {
                if (!num(c, get(c, i, "freq"), &v))
                    return err(c, "radio.tuner.freq");
                cfg->tuner_freq = freq_hz(v);
            }
            if (!fget(c, i, "settle_ms", 0, &v, "tuner"))
                return false;
            cfg->settle_ms = to_int(v);
        }
        if (!strcmp(t, "modem"))
            cfg->tuner = RDOF_TUNER_MODEM;
        else if (!strcmp(t, "fixed"))
            cfg->tuner = RDOF_TUNER_FIXED;
        else if (!strcmp(t, "rigctld") || !strcmp(t, "command"))
            return err(c, "radio.tuner %s: not on this board (modem or fixed only)", t);
        else if (t[0])
            return err(c, "radio.tuner %s", t);
    }
    cfg->rssi.n = 0;
    i = rget(c, r, keys, "rssi");
    if (truthy(c, i)) {
        if (N(c, i)->type != RDOF_JARR)
            return err(c, "radio.rssi wants [[raw, dBm], ...]");
        for (int k = 0; k < N(c, i)->count && k < RDOF_RSSI_POINTS; k++) {
            int p = rdof_jat(&c->doc, i, k);
            double a, b;
            if (N(c, p)->type != RDOF_JARR || N(c, p)->count != 2 || !num(c, rdof_jat(&c->doc, p, 0), &a) ||
                !num(c, rdof_jat(&c->doc, p, 1), &b))
                return err(c, "radio.rssi wants [[raw, dBm], ...]");
            cfg->rssi.raw[k] = to_int(a);
            cfg->rssi.dbm[k] = to_int(b);
            cfg->rssi.n = k + 1;
        }
        rdof_rssi_map_sort(&cfg->rssi);
    }
    /* check_radio */
    char proto[12];
    i = rget(c, r, keys, "protocol");
    if (i >= 0)
        text_of(c, i, proto, sizeof proto);
    else
        strcpy(proto, "uart");
    for (char *p = proto; *p; p++)
        if (*p >= 'A' && *p <= 'Z')
            *p += 32;
    if (strcmp(proto, "uart") && strcmp(proto, "udp"))
        return err(c, "radio.protocol %s: uart or udp", proto);
    if (!strcmp(proto, "udp")) {
        if (!truthy(c, rget(c, r, keys, "modem_address")))
            return err(c, "radio.protocol udp needs radio.modem_address");
        return err(c, "radio.protocol udp: not on this board");
    }
    return true;
}

/* A radio type, or opengd77, radio_hotspot's old name (canonical_type). */
static bool known_type(const char *t);
static void canonical_type(char *t, size_t cap);

/* A split's halves (radio.tx, radio.rx): a radio's checks on each. Their board ports come later. */
static bool split_halves(ctx_t *c, int r)
{
    static const char *const names[2] = {"tx", "rx"};
    for (int k = 0; k < 2; k++) {
        int h = rget(c, r, REPEATER_KEYS, names[k]);
        if (h < 0 || N(c, h)->type != RDOF_JOBJ)
            return err(c, "radio type split needs radio.%s: the %s half's port", names[k], k ? "RX" : "TX");
        char t[16];
        sget(c, h, "type", "auto", t, sizeof t);
        canonical_type(t, sizeof t);
        if (strcmp(t, "auto") && strcmp(t, "radio_hotspot"))
            return err(c, "radio.%s.type %s: a split half is a radio in hotspot mode with the split duplex extension "
                          "(OpenDigitalRadio)", names[k], t);
        char proto[12] = "uart";
        int i = get(c, h, "protocol");
        if (i >= 0)
            text_of(c, i, proto, sizeof proto);
        for (char *p = proto; *p; p++)
            if (*p >= 'A' && *p <= 'Z')
                *p += 32;
        if (strcmp(proto, "uart") && strcmp(proto, "udp"))
            return err(c, "radio.%s.protocol %s: uart or udp", names[k], proto);
        if (!strcmp(proto, "udp")) {
            if (!truthy(c, get(c, h, "modem_address")))
                return err(c, "radio.%s.protocol udp needs radio.%s.modem_address", names[k], names[k]);
            return err(c, "radio.%s.protocol udp: not on this board", names[k]);
        }
    }
    return true;
}

/* Firmware only: each half's port on the board (modem, uart, speed, usb, as a radio's own) and SET_ROLE's
 * parameters. */
static bool split_ports(ctx_t *c, int r, rdof_config_t *cfg)
{
    static const char *const names[2] = {"tx", "rx"};
    int usb = 0, uart = 0, sdr = 0;
    for (int k = 0; k < 2; k++) {
        int h = rget(c, r, REPEATER_KEYS, names[k]);
        rdof_cfg_port_t *p = &cfg->split[k];
        long v;
        sget(c, h, "modem", "usb", p->modem_port, sizeof p->modem_port);
        if (strcmp(p->modem_port, "usb") && strcmp(p->modem_port, "uart") && strcmp(p->modem_port, "sdr"))
            return err(c, "radio.%s.modem %s: usb, uart, or sdr (the board's SDR modem)", names[k], p->modem_port);
        int u = get(c, h, "uart");
        if (!iget(c, u, "tx", 5, &v, "uart"))
            return false;
        p->uart_tx = (int)v;
        if (!iget(c, u, "rx", 6, &v, "uart"))
            return false;
        p->uart_rx = (int)v;
        if (!iget(c, u, "num", 1, &v, "uart"))
            return false;
        if (v != 1 && v != 2)
            return err(c, "radio.%s.uart.num %ld: 1 or 2 (UART0 is the console)", names[k], v);
        p->uart_num = (int)v;
        if (!iget(c, h, "speed", 115200, &p->uart_speed, "radio"))
            return false;
        int ub = get(c, h, "usb");
        sget(c, ub, "serial", "", p->usb_serial, sizeof p->usb_serial);
        sget(c, ub, "port", "", p->usb_port, sizeof p->usb_port);
        sdr += !strcmp(p->modem_port, "sdr");
        if (!strcmp(p->modem_port, "usb")) {
            usb++;
            snprintf(cfg->usb_serial, sizeof cfg->usb_serial, "%s", p->usb_serial);
            snprintf(cfg->usb_port, sizeof cfg->usb_port, "%s", p->usb_port);
        } else if (!strcmp(p->modem_port, "uart") && !uart++) {
            cfg->uart_tx = p->uart_tx, cfg->uart_rx = p->uart_rx, cfg->uart_num = p->uart_num;
            cfg->uart_speed = p->uart_speed;
        }
    }
    if (sdr > 1)
        return err(c, "radio.tx and radio.rx are both the SDR: the board runs one SDR modem (two SDRs as a split "
                      "need two boards, or a radio for one half)");
    if (usb > 1)
        return err(c, "radio.tx and radio.rx are both on USB: one half on USB at most (the other on a UART, or the "
                      "SDR as the RX half)");
    if (uart > 1) {
        const rdof_cfg_port_t *a = &cfg->split[0], *b = &cfg->split[1];
        if (a->uart_num == b->uart_num)
            return err(c, "radio.tx and radio.rx are both on UART%d: give one \"uart\": {\"num\": 2, ...}", a->uart_num);
        if (a->uart_tx == b->uart_tx || a->uart_rx == b->uart_rx || a->uart_tx == b->uart_rx || a->uart_rx == b->uart_tx)
            return err(c, "radio.tx and radio.rx share a UART pin");
    }
    struct {
        const char *k;
        int *p;
    } params[] = {{"power_cap", &cfg->split_power_cap}, {"prebuffer", &cfg->split_prebuffer},
                  {"lease_s", &cfg->split_lease_s}, {"cooldown_s", &cfg->split_cooldown_s}};
    for (size_t k = 0; k < sizeof params / sizeof params[0]; k++) {
        double v;
        int i = rget(c, r, REPEATER_KEYS, params[k].k);
        if (i < 0)
            continue;
        if (!num(c, i, &v) || !(v >= 0 && v <= 255)) /* NaN too */
            return err(c, "radio.%s: 0 to 255", params[k].k);
        *params[k].p = (int)v;
    }
    int i = rget(c, r, REPEATER_KEYS, "max_key_s");
    if (i >= 0 && (!num(c, i, &cfg->split_max_key_s) || !(cfg->split_max_key_s >= 0 && cfg->split_max_key_s <= 600)))
        return err(c, "radio.max_key_s: 0 to 600");
    snprintf(cfg->modem_port, sizeof cfg->modem_port, "split");
    return true;
}

static bool known_type(const char *t)
{
    return !strcmp(t, "auto") || !strcmp(t, "radio_hotspot") || !strcmp(t, "opengd77") || !strcmp(t, "mmdvm_hs") ||
           !strcmp(t, "mmdvm");
}

/* The old name opengd77 becomes radio_hotspot. */
static void canonical_type(char *t, size_t cap)
{
    if (!strcmp(t, "opengd77"))
        snprintf(t, cap, "radio_hotspot");
}

/* A radio's name: its own, else the default "{box}-{type}{n}". */
static void radio_name(ctx_t *c, int r, int k, const char *box_name, char *out, size_t cap)
{
    int t = get(c, r, "type");
    char dflt[96];
    snprintf(dflt, sizeof dflt, "%s-%s%d", box_name, t >= 0 && N(c, t)->type == RDOF_JSTR ? N(c, t)->str : "", k + 1);
    sget(c, r, "name", dflt, out, cap);
}

/* A radio's "enabled": 1 (true or not there), 0 (false: left out, as if it weren't in radios), -1 (not a bool). */
static int enabled(ctx_t *c, int r)
{
    int e = get(c, r, "enabled");
    if (e < 0)
        return 1;
    return N(c, e)->type == RDOF_JBOOL ? N(c, e)->b : -1;
}

/* Is `name` in "run" (a name or a list of names)? */
static bool in_run(ctx_t *c, int run, const char *name)
{
    const rdof_jnode_t *n = N(c, run);
    if (n->type == RDOF_JSTR)
        return !strcmp(n->str, name);
    for (int k = 0; n->type == RDOF_JARR && k < n->count; k++) {
        const rdof_jnode_t *e = N(c, rdof_jat(&c->doc, run, k));
        if (e->type == RDOF_JSTR && !strcmp(e->str, name))
            return true;
    }
    return false;
}

/*
 * The radios this board runs (indexes into radios, -1 for none): with the
 * firmware key "run", the ones it names (up to RDOF_MAX_RADIOS roamers or
 * repeaters and one SDR); without it, the first roamer or repeater, or failing
 * that the first SDR. radio[] and radio_k[] get *nradio entries. A radio with
 * "enabled": false is left out; "run" may name it.
 */
static bool pick_radios(ctx_t *c, int box, const char *box_name, int *radio, int *radio_k, int *nradio, int *sdr,
                        int *sdr_k)
{
    int radios = get(c, box, "radios"), run = get(c, box, "run");
    *nradio = 0;
    *sdr = -1;
    *sdr_k = 0;
    int on = 0, off_in_run = 0; /* radios enabled; disabled ones "run" names */
    if (radios >= 0 && N(c, radios)->type != RDOF_JARR && N(c, radios)->type != RDOF_JNULL)
        return err(c, "radios: a list");
    for (int k = 0; radios >= 0 && k < N(c, radios)->count; k++) {
        int s = rdof_jat(&c->doc, radios, k);
        const int en = enabled(c, s);
        if (en < 0)
            return err(c, "radios[%d]: enabled: true or false", k);
        if (!en) {
            char name[96];
            radio_name(c, s, k, box_name, name, sizeof name);
            off_in_run += run >= 0 && in_run(c, run, name);
            continue;
        }
        on++;
        int t = get(c, s, "type");
        const char *type = t >= 0 && N(c, t)->type == RDOF_JSTR ? N(c, t)->str : "";
        if (strcmp(type, "sdr") && strcmp(type, "repeater") && strcmp(type, "sdr_transceiver") && !known_type(type))
            return err(c, "radios[%d]: type %s: sdr, repeater, sdr_transceiver or a roamer (auto, mmdvm_hs, "
                          "radio_hotspot, mmdvm; opengd77 = radio_hotspot)", k, type);
        bool is_sdr = !strcmp(type, "sdr");
        if (run >= 0) {
            char name[96];
            radio_name(c, s, k, box_name, name, sizeof name);
            if (!in_run(c, run, name))
                continue;
            if (is_sdr && *sdr >= 0)
                return err(c, "run: one SDR a board");
            if (!is_sdr && *nradio >= RDOF_MAX_RADIOS)
                return err(c, "run: %d roamers or repeaters a board at most", RDOF_MAX_RADIOS);
        } else if (is_sdr ? *sdr >= 0 || *nradio : *nradio) {
            continue;
        }
        if (is_sdr) {
            *sdr = s;
            *sdr_k = k;
        } else {
            radio[*nradio] = s;
            radio_k[(*nradio)++] = k;
            if (run < 0)
                *sdr = -1; /* without run, a roamer or repeater wins */
        }
    }
    if (!on)
        return err(c, "no radios");
    if (run >= 0) {
        const rdof_jnode_t *n = N(c, run);
        if (n->type != RDOF_JSTR && n->type != RDOF_JARR)
            return err(c, "run: a radio's name or a list of names");
        for (int k = 0; n->type == RDOF_JARR && k < n->count; k++)
            if (N(c, rdof_jat(&c->doc, run, k))->type != RDOF_JSTR)
                return err(c, "run: a radio's name or a list of names");
        int names = n->type == RDOF_JSTR ? 1 : n->count;
        if (names != *nradio + (*sdr >= 0) + off_in_run)
            return err(c, "run: a name that isn't in radios");
    }
    return true;
}

/* ── the box ───────────────────────────────────────────────────────────────── */

static bool channel_entry(ctx_t *c, uint32_t freq, double cc, double power, rdof_cfg_channel_t *out)
{
    if (!(cc >= 0 && cc <= 15) || (long)cc != cc)
        return err(c, "color code %g", cc);
    long p = to_long(power);
    out->freq = freq;
    out->cc = (int)cc;
    out->power = (int)(p < 0 ? 0 : p > 255 ? 255 : p);
    return true;
}

static bool roamer(ctx_t *c, int box, int r, const plan_list_t *plan, rdof_config_t *cfg, const char *box_name)
{
    const char *const *K = ROAMER_KEYS;
    cfg->mode = RDOF_CFG_ROAMER;
    double v;
    const plan_t *pc;
    int over;
    /* channels */
    int refs = get(c, r, "channels");
    cfg->nchannels = 0;
    bool all = refs < 0 || (N(c, refs)->type == RDOF_JSTR && !strcmp(N(c, refs)->str, "all"));
    int count = all ? plan->n : (truthy(c, refs) ? N(c, refs)->count : 0);
    if (!all && truthy(c, refs) && N(c, refs)->type != RDOF_JARR)
        return err(c, "channels: numbers, or all");
    for (int k = 0; k < count; k++) {
        double cc, power;
        if (all) {
            pc = &plan->ch[k];
            over = -1;
        } else if (!plan_ref(c, plan, rdof_jat(&c->doc, refs, k), &pc, &over)) {
            return false;
        }
        if (!fget(c, over, "cc", pc->cc, &cc, "channel") || !fget(c, over, "power", pc->power, &power, "channel"))
            return false;
        if (cfg->nchannels >= RDOF_CFG_CHANNELS)
            return err(c, "too many channels (%d at most)", RDOF_CFG_CHANNELS);
        if (!channel_entry(c, freq_hz(pc->freq), cc, power, &cfg->channels[cfg->nchannels++]))
            return false;
    }
    /* idle */
    int idle = get(c, r, "idle");
    if (idle < 0)
        return err(c, "no idle channel (idle: a channel number)");
    if (N(c, idle)->type == RDOF_JOBJ && get(c, idle, "freq") >= 0) {
        uint32_t f;
        double cc;
        if (!box_hz(c, get(c, idle, "freq"), &f, "idle") || !num(c, get(c, idle, "cc"), &cc))
            return err(c, "idle: freq and cc");
        if (!channel_entry(c, freq_hz(f), (double)to_long(cc), 255, &cfg->idle))
            return false;
    } else {
        if (!plan_ref(c, plan, idle, &pc, &over))
            return false;
        if (!channel_entry(c, freq_hz(pc->freq), pc->cc, 255, &cfg->idle))
            return false;
        bool have = false;
        for (int k = 0; k < cfg->nchannels; k++)
            have |= cfg->channels[k].freq == pc->freq;
        if (!have) {
            if (cfg->nchannels >= RDOF_CFG_CHANNELS)
                return err(c, "too many channels");
            if (!channel_entry(c, pc->freq, pc->cc, pc->power, &cfg->channels[cfg->nchannels++]))
                return false;
        }
        cfg->oled_channel = pc->number;
    }
    /* the radio */
    snprintf(cfg->radio_type, sizeof cfg->radio_type, "%s", N(c, get(c, r, "type"))->str);
    canonical_type(cfg->radio_type, sizeof cfg->radio_type);
    if (!strcmp(cfg->radio_type, "sdr_transceiver"))
        snprintf(cfg->radio_type, sizeof cfg->radio_type, "auto"); /* the SDR's emulated modem answers as one */
    if (!radio_settings(c, r, K, cfg))
        return false;
    if (!known_type(cfg->radio_type))
        return err(c, "radio type %s", cfg->radio_type);
    long lv;
    if (!iget(c, r, "peer_id", 0, &lv, "radio") || get(c, r, "peer_id") < 0)
        return err(c, "no peer_id");
    cfg->peer_id = (uint32_t)lv;
    char name8[9];
    memcpy(name8, cfg->name, 8); /* name[:8] */
    name8[8] = 0;
    sor(c, r, "callsign", name8, cfg->callsign, sizeof cfg->callsign);
    /* Config.from_dict's numbers */
    struct {
        const char *k;
        double d;
        double *p;
    } fl[] = {{"listen_s", 0.3, &cfg->listen_s}, {"clear_s", 2.0, &cfg->clear_s}, {"answer_s", 0.75, &cfg->answer_s},
              {"answer_max_s", 5.0, &cfg->answer_max_s}, {"idle_after_s", 0, &cfg->idle_after_s},
              {"cwid_every_s", 900, &cfg->cwid_every_s}};
    for (size_t k = 0; k < sizeof fl / sizeof fl[0]; k++)
        if (!fget(c, r, fl[k].k, fl[k].d, fl[k].p, "radio"))
            return false;
    if (!jitter(c, r, cfg))
        return false;
    if (!iget(c, r, "csbk_preambles", 2, &lv, "radio"))
        return false;
    cfg->csbk_preambles = (int)lv;
    sget(c, r, "cwid", "", cfg->cwid, sizeof cfg->cwid);
    /* covered: this box's SDR channels */
    cfg->ncovered = 0;
    int radios = get(c, box, "radios");
    for (int k = 0; k < (radios >= 0 ? N(c, radios)->count : 0); k++) {
        int s = rdof_jat(&c->doc, radios, k);
        int t = get(c, s, "type");
        if (t < 0 || N(c, t)->type != RDOF_JSTR || strcmp(N(c, t)->str, "sdr") || enabled(c, s) != 1)
            continue;
        int chs = get(c, s, "channels");
        for (int m = 0; chs >= 0 && m < N(c, chs)->count; m++) {
            if (!plan_ref(c, plan, rdof_jat(&c->doc, chs, m), &pc, &over))
                return false;
            uint32_t f = pc->freq;
            bool dup = false;
            for (int q = 0; q < cfg->ncovered; q++)
                dup |= cfg->covered[q] == f;
            if (!dup && cfg->ncovered < RDOF_CFG_CHANNELS)
                cfg->covered[cfg->ncovered++] = f;
        }
    }
    for (int a = 1; a < cfg->ncovered; a++) /* sorted */
        for (int b = a; b > 0 && cfg->covered[b - 1] > cfg->covered[b]; b--) {
            uint32_t t = cfg->covered[b];
            cfg->covered[b] = cfg->covered[b - 1];
            cfg->covered[b - 1] = t;
        }
    /* scan */
    cfg->has_scan = false;
    int sc = get(c, r, "scan");
    if (truthy(c, sc)) {
        cfg->has_scan = true;
        bool is_obj = N(c, sc)->type == RDOF_JOBJ;
        int sref = is_obj ? get(c, sc, "channels") : -1;
        bool sall = sref < 0 || (N(c, sref)->type == RDOF_JSTR && !strcmp(N(c, sref)->str, "all"));
        cfg->nscan = 0;
        if (sall) {
            memcpy(cfg->scan_channels, cfg->channels, sizeof cfg->channels);
            cfg->nscan = cfg->nchannels;
        } else {
            for (int k = 0; k < N(c, sref)->count; k++) {
                double cc;
                if (!plan_ref(c, plan, rdof_jat(&c->doc, sref, k), &pc, &over) ||
                    !fget(c, over, "cc", pc->cc, &cc, "scan"))
                    return false;
                if (cfg->nscan >= RDOF_CFG_CHANNELS)
                    return err(c, "scan: too many channels");
                if (!channel_entry(c, freq_hz(pc->freq), cc, 255, &cfg->scan_channels[cfg->nscan++]))
                    return false;
            }
        }
        if (!cfg->nscan)
            return err(c, "scan: no channels");
        int so = is_obj ? sc : -1;
        if (!fget(c, so, "dwell_s", 0.45, &cfg->dwell_s, "scan") ||
            !fget(c, so, "hang_s", 3.0, &cfg->hang_s_scan, "scan") || !fget(c, so, "carrier_s", 0, &cfg->carrier_s, "scan"))
            return false;
        if (cfg->dwell_s <= 0 || cfg->hang_s_scan < 0 || !(0 <= cfg->carrier_s && cfg->carrier_s < cfg->dwell_s))
            return err(c, "scan: dwell_s > 0, hang_s >= 0, and carrier_s (0 = off) under dwell_s");
    }
    if (!cfg->nchannels)
        return err(c, "no channels: list the simplex channels this radio may use");
    (void)v;
    (void)box_name;
    return true;
}

static bool repeater(ctx_t *c, int box, int r, const plan_list_t *plan, rdof_config_t *cfg, long default_cc,
                     bool has_default_cc)
{
    const char *const *K = REPEATER_KEYS;
    cfg->mode = RDOF_CFG_REPEATER;
    char mode[12];
    sget(c, r, "mode", "duplex", mode, sizeof mode);
    bool sfr = !strcmp(mode, "sfr"), dcdm = !strcmp(mode, "dcdm");
    uint32_t rx, tx;
    if (sfr || dcdm) {
        if (get(c, r, "freq") < 0)
            return err(c, "no freq (MHz)");
        if (!box_hz(c, get(c, r, "freq"), &rx, "freq"))
            return false;
        tx = rx;
    } else {
        if (get(c, r, "rx_freq") < 0)
            return err(c, "no rx_freq (MHz)");
        if (get(c, r, "tx_freq") < 0)
            return err(c, "no tx_freq (MHz)");
        if (!box_hz(c, get(c, r, "rx_freq"), &rx, "rx_freq") || !box_hz(c, get(c, r, "tx_freq"), &tx, "tx_freq"))
            return false;
    }
    long lv;
    if (get(c, r, "cc") >= 0) {
        if (!iget(c, r, "cc", 0, &lv, "radio"))
            return false;
    } else if (has_default_cc) {
        lv = default_cc;
    } else {
        return err(c, "no color code (set cc on it or at the top)");
    }
    if (strcmp(mode, "duplex") && strcmp(mode, "sfr") && !dcdm)
        return err(c, "mode %s: duplex, sfr or dcdm", mode);
    if (lv < 0 || lv > 15)
        return err(c, "color code %ld", lv);
    cfg->cc = (int)lv;
    if (!iget(c, r, "peer_id", 0, &lv, "radio") || get(c, r, "peer_id") < 0)
        return err(c, "no peer_id");
    cfg->peer_id = (uint32_t)lv;
    char name8[9];
    memcpy(name8, cfg->name, 8); /* name[:8] */
    name8[8] = 0;
    sor(c, r, "callsign", name8, cfg->callsign, sizeof cfg->callsign);
    cfg->rx_freq = freq_hz(rx);
    cfg->tx_freq = freq_hz(tx);
    if (!iget(c, r, "power", 255, &lv, "radio"))
        return false;
    cfg->power = (int)(lv < 0 ? 0 : lv > 255 ? 255 : lv);
    if (!fget(c, r, "hang_s", 5.0, &cfg->hang_s, "radio") || !fget(c, r, "call_hang_s", 3.0, &cfg->call_hang_s, "radio") ||
        !fget(c, r, "cwid_every_s", 900, &cfg->cwid_every_s, "radio"))
        return false;
    if (!jitter(c, r, cfg))
        return false;
    sget(c, r, "cwid", "", cfg->cwid, sizeof cfg->cwid);
    cfg->short_lc_ts1_low = bget(c, r, "short_lc_ts1_low", false);
    if (!radio_settings(c, r, K, cfg))
        return false;
    sget(c, get(c, r, "radio"), "type", "auto", cfg->radio_type, sizeof cfg->radio_type);
    canonical_type(cfg->radio_type, sizeof cfg->radio_type);
    if (sfr) {
        long inbound;
        if (!iget(c, r, "inbound_slot", 1, &inbound, "radio"))
            return false;
        if (inbound != 1 && inbound != 2)
            return err(c, "inbound_slot: 1 or 2");
        if (strcmp(cfg->radio_type, "auto") && strcmp(cfg->radio_type, "radio_hotspot"))
            return err(c, "radio.type %s: a single frequency repeater is a radio_hotspot (a radio in hotspot mode "
                          "with SFR firmware)", cfg->radio_type);
        cfg->sfr = true;
        cfg->inbound_slot = (int)inbound;
        cfg->slot1 = inbound == 2;
        cfg->slot2 = inbound == 1;
    } else {
        cfg->sfr = false;
        cfg->dcdm = dcdm;
        cfg->inbound_slot = 1;
        int s = get(c, r, "slots");
        if (s < 0) {
            cfg->slot1 = cfg->slot2 = true;
        } else {
            cfg->slot1 = cfg->slot2 = false;
            bool bad = N(c, s)->type != RDOF_JARR || N(c, s)->count == 0;
            for (int k = 0; !bad && k < N(c, s)->count; k++) {
                double v;
                if (!num(c, rdof_jat(&c->doc, s, k), &v) || (to_long(v) != 1 && to_long(v) != 2))
                    bad = true;
                else if (to_long(v) == 1)
                    cfg->slot1 = true;
                else
                    cfg->slot2 = true;
            }
            if (bad)
                return err(c, "slots: 1, 2 or both");
        }
        if (dcdm && !strcmp(cfg->radio_type, "split"))
            return err(c, "radio.type split: a split is a duplex pair; dual capacity direct mode is one radio on one "
                          "frequency");
        if (!strcmp(cfg->radio_type, "split")) {
            if (!split_halves(c, r))
                return false;
        } else if (!known_type(cfg->radio_type)) {
            return err(c, "radio type %s: auto or one of radio_hotspot, mmdvm_hs, mmdvm, split", cfg->radio_type);
        }
        if (!dcdm && !strcmp(cfg->radio_type, "radio_hotspot"))
            return err(c, "a radio_hotspot (a radio in hotspot mode) is simplex: a repeater needs a duplex MMDVM_HS "
                          "board, an MMDVM board or two radios as a split (but see mode: sfr)");
    }
    if (cfg->hang_s <= 0 || cfg->call_hang_s < 0)
        return err(c, "hang_s > 0 and call_hang_s >= 0");
    /* slot timing (docs/spec/slot-timing.md): the box's reaches a duplex repeater on a split or auto radio (the SDR
       transceiver's is auto); a HAT or an SFR radio keeps its own slot clock, so utc is refused there */
    {
        bool timed = !sfr && !dcdm && (!strcmp(cfg->radio_type, "split") || !strcmp(cfg->radio_type, "auto"));
        char why[96];
        if (!rdof_slot_timing_parse(&c->doc, timed ? get(c, box, "slot_timing") : -1, get(c, r, "slot_timing"),
                                  &cfg->slot_timing, why, sizeof why))
            return err(c, "%s", why);
        if (cfg->slot_timing.utc && sfr)
            return err(c, "slot_timing utc: a single frequency repeater's radio times its own slots");
        if (cfg->slot_timing.utc && dcdm)
            return err(c, "slot_timing utc: in dual capacity direct mode the modem times its slots from the channel");
        if (cfg->slot_timing.utc && !timed)
            return err(c, "slot_timing utc: a %s keeps its own slot clock (its firmware times the slots, and MMDVM "
                          "has no command to move it): only a split or the board's SDR can follow the UTC grid",
                       cfg->radio_type);
    }
    /* The OLED's channel number: the plan's, by frequency. */
    cfg->oled_channel = 0;
    for (int k = 0; k < plan->n; k++) {
        int64_t d = (int64_t)plan->ch[k].freq - cfg->rx_freq;
        if (d >= -100 && d <= 100) {
            cfg->oled_channel = plan->ch[k].number;
            break;
        }
    }
    (void)box;
    return true;
}

static bool mqtt(ctx_t *c, int box, rdof_config_t *cfg)
{
    int m = get(c, box, "mqtt");
    cfg->has_mqtt = truthy(c, m);
    if (!cfg->has_mqtt)
        return true;
    long port;
    sget(c, m, "host", "localhost", cfg->mqtt_host, sizeof cfg->mqtt_host);
    if (!iget(c, m, "port", 1883, &port, "mqtt"))
        return false;
    cfg->mqtt_port = (int)port;
    sor(c, m, "username", "", cfg->mqtt_user, sizeof cfg->mqtt_user);
    sor(c, m, "password", "", cfg->mqtt_pass, sizeof cfg->mqtt_pass);
    sget(c, m, "root", "DMR", cfg->mqtt_root, sizeof cfg->mqtt_root);
    /* the root's slashes are stripped, both ends */
    size_t rl = strlen(cfg->mqtt_root), lead = strspn(cfg->mqtt_root, "/");
    while (rl > lead && cfg->mqtt_root[rl - 1] == '/')
        rl--;
    memmove(cfg->mqtt_root, cfg->mqtt_root + lead, rl - lead);
    cfg->mqtt_root[rl - lead] = 0;
    snprintf(cfg->mqtt_app, sizeof cfg->mqtt_app, "%s", cfg->mode == RDOF_CFG_ROAMER ? "roaming" : "repeater");
    cfg->mqtt_tls = bget(c, m, "tls", false);
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

/* hblink.sctp_*: the firmware's SCTP (over UDP); defaults as MMDVMHost's and HBlink4's. */
static bool sctp_settings(ctx_t *c, int hb, rdof_config_t *cfg)
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
        if (!iget(c, hb, keys[i].key, keys[i].dflt, &v, "hblink"))
            return false;
        if (v < keys[i].lo || v > keys[i].hi)
            return err(c, "hblink: %s %ld out of range (%ld to %ld)", keys[i].key, v, keys[i].lo, keys[i].hi);
        *out[i] = (int)v;
    }
    return true;
}

bool rdof_config_parse(const char *text, size_t len, rdof_config_t *cfg, char *error, size_t error_cap)
{
    return rdof_config_parse_radio(text, len, NULL, cfg, error, error_cap);
}

bool rdof_config_parse_radio(const char *text, size_t len, const char *want, rdof_config_t *cfg, char *error,
                           size_t error_cap)
{
    char dummy[8];
    ctx_t *c = malloc(sizeof *c); /* large (the node array): from the heap, not the stack */
    if (!c) {
        if (error && error_cap)
            snprintf(error, error_cap, "no memory");
        return false;
    }
    c->error = error ? error : dummy;
    c->error_cap = error ? error_cap : sizeof dummy;
    if (c->error_cap)
        c->error[0] = 0;
    memset(cfg, 0, sizeof *cfg);
    char *clean = malloc(len + 1);
    if (!clean) {
        free(c);
        if (error && error_cap)
            snprintf(error, error_cap, "no memory");
        return false;
    }
    size_t n = rdof_jsonc_strip(text, len, clean);
    c->doc.nodes = c->nodes;
    c->doc.max = MAX_NODES;
    c->doc.buf = c->strs;
    c->doc.cap = STR_BUF;
    int box = rdof_jparse(&c->doc, clean, n);
    bool ok = false;
    if (box < 0) {
        err(c, "JSON: %s", c->doc.error);
        goto done;
    }
    if (N(c, box)->type != RDOF_JOBJ) {
        err(c, "the settings are a JSON object");
        goto done;
    }
    {
        rdof_slot_timing_t t; /* the box's, checked whether or not anything inherits it */
        char why[96];
        if (!rdof_slot_timing_parse(&c->doc, -1, get(c, box, "slot_timing"), &t, why, sizeof why)) {
            err(c, "box: %s", why);
            goto done;
        }
    }
    /* the plan */
    plan_list_t plan;
    plan.n = 0;
    int cc_node = get(c, box, "cc");
    bool has_default_cc = cc_node >= 0 && N(c, cc_node)->type != RDOF_JNULL;
    double dcc = 0;
    if (has_default_cc && !num(c, cc_node, &dcc)) {
        err(c, "cc isn't a number");
        goto done;
    }
    int chans = get(c, box, "channels");
    for (int k = 0; chans >= 0 && N(c, chans)->type == RDOF_JOBJ && k < N(c, chans)->count; k++) {
        int ch = rdof_jat(&c->doc, chans, k);
        plan_t *p;
        if (plan.n >= (int)(sizeof plan.ch / sizeof plan.ch[0])) {
            err(c, "too many channels in the plan");
            goto done;
        }
        p = &plan.ch[plan.n];
        char *e;
        long number = strtol(N(c, ch)->key, &e, 10);
        p->number = (int)number;
        if (!N(c, ch)->key[0] || *e || number != p->number) { /* "" isn't 0; nor 2^32 + 1 channel 1 */
            err(c, "channel %s: not a number", N(c, ch)->key);
            goto done;
        }
        int f = N(c, ch)->type == RDOF_JOBJ ? get(c, ch, "freq") : ch;
        if (f < 0) {
            err(c, "channel %d: no freq", p->number);
            goto done;
        }
        if (!box_hz(c, f, &p->freq, "channel"))
            goto done;
        double cc, power;
        int obj = N(c, ch)->type == RDOF_JOBJ ? ch : -1;
        int ccn = get(c, obj, "cc");
        if (ccn >= 0 ? !num(c, ccn, &cc) : !has_default_cc) {
            err(c, "channel %d: no color code (set cc on it or at the top)", p->number);
            goto done;
        }
        if (ccn < 0)
            cc = dcc;
        if (!fget(c, obj, "power", 255, &power, "channel"))
            goto done;
        p->cc = to_int(cc);
        p->power = to_int(power);
        plan_t *same = (plan_t *)plan_find(&plan, p->number);
        if (same) /* "1" and "01": the last one */
            *same = *p;
        else
            plan.n++;
    }
    qsort(plan.ch, (size_t)plan.n, sizeof plan.ch[0], cmp_plan);
    /* the radio: the one "run" names, else the first that isn't an SDR */
    char box_name[64];
    sor(c, box, "name", "radiofoundry", box_name, sizeof box_name);
    int rs[RDOF_MAX_RADIOS], ks[RDOF_MAX_RADIOS], nr, sdr_unused, sdr_index_unused;
    if (!pick_radios(c, box, box_name, rs, ks, &nr, &sdr_unused, &sdr_index_unused))
        goto done;
    if (!nr) {
        err(c, "no roamer or repeater in radios");
        goto done;
    }
    int r = rs[0], index = ks[0];
    if (want) {
        int k = 0;
        for (; k < nr; k++) {
            char name[96];
            radio_name(c, rs[k], ks[k], box_name, name, sizeof name);
            if (!strncmp(name, want, sizeof cfg->name - 1)) /* as far as rdof_roles_t and cfg->name keep it */
                break;
        }
        if (k == nr) {
            err(c, "%s isn't a roamer or repeater this board runs", want);
            goto done;
        }
        r = rs[k];
        index = ks[k];
    }
    const char *type = N(c, get(c, r, "type"))->str;
    char dflt_name[96];
    snprintf(dflt_name, sizeof dflt_name, "%s-%s%d", box_name, type, index + 1);
    sget(c, r, "name", dflt_name, cfg->name, sizeof cfg->name);
    int hb = get(c, box, "hblink");
    long port;
    sget(c, hb, "host", "localhost", cfg->master_host, sizeof cfg->master_host);
    if (!iget(c, hb, "port", 62031, &port, "hblink"))
        goto done;
    cfg->master_port = (int)port;
    sget(c, hb, "transport", "udp", cfg->transport, sizeof cfg->transport);
    if (!sctp_settings(c, hb, cfg))
        goto done;
    cfg->receiver_hblink = bget(c, r, "hblink", true);
    int loc = get(c, r, "location");
    if (!location(c, loc >= 0 ? loc : get(c, box, "location"), cfg))
        goto done;
    /* sdr_transceiver: the board's SDR front end as the radio (docs/reference.md, "Transmit"): half duplex is a roamer,
       full a duplex repeater; its modem is the SDR's emulated one. */
    const bool sdrtx = !strcmp(type, "sdr_transceiver");
    bool sdr_dcdm = false; /* mode dcdm: a DCDM gateway (the repeater's code), half or full duplex */
    cfg->sdr_duplex = RDOF_SDR_DUPLEX_NONE;
    cfg->tx_power_dbm = 10;
    if (sdrtx) {
        char duplex[8];
        sget(c, r, "duplex", "half", duplex, sizeof duplex);
        if (strcmp(duplex, "half") && strcmp(duplex, "full")) {
            err(c, "duplex %s: half (a roamer) or full (a duplex repeater)", duplex);
            goto done;
        }
        cfg->sdr_duplex = !strcmp(duplex, "full") ? RDOF_SDR_DUPLEX_FULL : RDOF_SDR_DUPLEX_HALF;
        if (!fget(c, r, "tx_power_dbm", 10, &cfg->tx_power_dbm, "radio"))
            goto done;
        char mode[8];
        sget(c, r, "mode", "duplex", mode, sizeof mode);
        if (cfg->sdr_duplex == RDOF_SDR_DUPLEX_FULL && strcmp(mode, "duplex") && strcmp(mode, "dcdm")) {
            err(c, "an sdr_transceiver with duplex: full is a duplex repeater (no mode: sfr)");
            goto done;
        }
        sdr_dcdm = !strcmp(mode, "dcdm");
        bool any_sdr = false;
        int radios = get(c, box, "radios");
        for (int k = 0; radios >= 0 && k < N(c, radios)->count; k++) {
            int s = rdof_jat(&c->doc, radios, k), t = get(c, s, "type");
            any_sdr |= t >= 0 && N(c, t)->type == RDOF_JSTR && !strcmp(N(c, t)->str, "sdr") && enabled(c, s) == 1;
        }
        if (!any_sdr) {
            err(c, "an sdr_transceiver needs an SDR in radios: its front end is the radio");
            goto done;
        }
    }
    if (!strcmp(type, "repeater") || (sdrtx && (cfg->sdr_duplex == RDOF_SDR_DUPLEX_FULL || sdr_dcdm))) {
        if (!repeater(c, box, r, &plan, cfg, to_long(dcc), has_default_cc))
            goto done;
    } else if (!roamer(c, box, r, &plan, cfg, box_name)) {
        goto done;
    }
    if (!mqtt(c, box, cfg) || !oled(c, box, cfg))
        goto done;
    /* firmware only (wifi and ntp belong to the board's Wi-Fi and clock, not here) */
    const char *dflt_port = !strcmp(cfg->radio_type, "radio_hotspot") || !strcmp(cfg->radio_type, "auto") ? "usb" : "uart";
    char box_port[8];
    sget(c, box, "modem", dflt_port, box_port, sizeof box_port);
    sget(c, r, "modem", box_port, cfg->modem_port, sizeof cfg->modem_port); /* the radio's own, else the box's */
    if (sdrtx)
        snprintf(cfg->modem_port, sizeof cfg->modem_port, "sdr");
    else if (strcmp(cfg->modem_port, "usb") && strcmp(cfg->modem_port, "uart")) {
        err(c, "modem %s: usb or uart", cfg->modem_port);
        goto done;
    }
    int u = get(c, r, "uart") >= 0 ? get(c, r, "uart") : get(c, box, "uart");
    long v;
    if (!iget(c, u, "tx", 5, &v, "uart"))
        goto done;
    cfg->uart_tx = (int)v;
    if (!iget(c, u, "rx", 6, &v, "uart"))
        goto done;
    cfg->uart_rx = (int)v;
    if (!iget(c, u, "num", 1, &v, "uart"))
        goto done;
    if (v != 1 && v != 2) {
        err(c, "uart.num %ld: 1 or 2 (UART0 is the console)", v);
        goto done;
    }
    cfg->uart_num = (int)v;
    if (!iget(c, r, "speed", 115200, &cfg->uart_speed, "radio"))
        goto done;
    int usb = get(c, r, "usb");
    sget(c, usb, "serial", "", cfg->usb_serial, sizeof cfg->usb_serial);
    sget(c, usb, "port", "", cfg->usb_port, sizeof cfg->usb_port);
    if (!strcmp(cfg->radio_type, "split") && !split_ports(c, r, cfg))
        goto done;
    ok = true;
done:
    free(clean);
    free(c);
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

/* Two ports (of radios a and b) that can't both be: false with error set. */
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
            snprintf(error, cap, "%s and %s are both on UART%d: give one \"uart\": {\"num\": 2, ...}", a_name, b_name,
                     a->uart_num);
            return false;
        }
        if (a->uart_tx == b->uart_tx || a->uart_rx == b->uart_rx || a->uart_tx == b->uart_rx ||
            a->uart_rx == b->uart_tx) {
            snprintf(error, cap, "%s and %s share a UART pin", a_name, b_name);
            return false;
        }
    } else if ((!a->usb_serial[0] && !a->usb_port[0]) || (!b->usb_serial[0] && !b->usb_port[0])) {
        snprintf(error, cap, "%s and %s are both radios on USB: tell them apart with \"usb\": {\"serial\": ...} "
                 "or {\"port\": ...} on each", a_name, b_name);
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

bool rdof_box_roles(const char *text, size_t len, rdof_roles_t *roles, char *error, size_t error_cap)
{
    char dummy[8];
    memset(roles, 0, sizeof *roles);
    ctx_t *c = malloc(sizeof *c);
    char *clean = c ? malloc(len + 1) : NULL;
    if (!clean) {
        free(c);
        if (error && error_cap)
            snprintf(error, error_cap, "no memory");
        return false;
    }
    c->error = error ? error : dummy;
    c->error_cap = error ? error_cap : sizeof dummy;
    if (c->error_cap)
        c->error[0] = 0;
    size_t n = rdof_jsonc_strip(text, len, clean);
    c->doc.nodes = c->nodes;
    c->doc.max = MAX_NODES;
    c->doc.buf = c->strs;
    c->doc.cap = STR_BUF;
    int box = rdof_jparse(&c->doc, clean, n);
    bool ok = false;
    if (box < 0) {
        err(c, "JSON: %s", c->doc.error);
    } else if (N(c, box)->type != RDOF_JOBJ) {
        err(c, "the settings are a JSON object");
    } else if (get(c, box, "radios") < 0 && get(c, box, "device") >= 0) {
        roles->has_sdr = roles->sdr_plain = true; /* a plain SDR config on its own */
        sor(c, box, "name", "sdr", roles->sdr, sizeof roles->sdr);
        snprintf(roles->box, sizeof roles->box, "%s", roles->sdr);
        ok = true;
    } else {
        char box_name[64];
        sor(c, box, "name", "radiofoundry", box_name, sizeof box_name);
        snprintf(roles->box, sizeof roles->box, "%.*s", (int)sizeof roles->box - 1, box_name);
        int r[RDOF_MAX_RADIOS], rk[RDOF_MAX_RADIOS], nr, s, sk;
        roles->usb_hub = bget(c, box, "usb_hub", false);
        if (pick_radios(c, box, box_name, r, rk, &nr, &s, &sk)) {
            roles->nradios = nr;
            for (int k = 0; k < nr; k++)
                radio_name(c, r[k], rk[k], box_name, roles->radios[k], sizeof roles->radios[k]);
            if ((roles->has_radio = nr > 0))
                snprintf(roles->radio, sizeof roles->radio, "%s", roles->radios[0]);
            if ((roles->has_sdr = s >= 0))
                radio_name(c, s, sk, box_name, roles->sdr, sizeof roles->sdr);
            ok = true;
        }
    }
    free(clean);
    free(c);
    return ok;
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
