// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* What runs from the settings, version 3: the devices' own keys, the run units and what they need (rdof/siteplan.h,
   docs/spec/fw/config-v3.md section 12). */
#include "rdof/siteplan.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "site_ctx.h"

#define MAX_EXTRA 64

struct rdof_plan {
    rdof_site_ctx_t *c;
    rdof_site_t site;
    rdof_plan_dev_t dev[RDOF_SITE_MAX_DEVICES];
    int nunit;
    rdof_unit_t unit[RDOF_PLAN_MAX_UNITS];
    int nextra;
    rdof_plan_extra_t extra[MAX_EXTRA];
    int nbench;
    int bench[RDOF_SITE_MAX_DEVICES];
};

typedef struct rdof_plan plan_t;

static bool fail(plan_t *p, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->c->error, p->c->cap, fmt, ap);
    va_end(ap);
    return false;
}

static const rdof_jnode_t *N(plan_t *p, int i) { return rdof_jnode(&p->c->doc, i); }
static int get(plan_t *p, int obj, const char *key) { return obj < 0 ? -1 : rdof_jget(&p->c->doc, obj, key); }
static int at(plan_t *p, int node, int i) { return rdof_jat(&p->c->doc, node, i); }
static bool is_type(plan_t *p, int i, rdof_jtype_t t) { return i >= 0 && N(p, i)->type == t; }
static const char *str_of(plan_t *p, int i) { return is_type(p, i, RDOF_JSTR) ? N(p, i)->str : NULL; }
static int count(plan_t *p, int i) { return i >= 0 ? N(p, i)->count : 0; }

static bool num_of(plan_t *p, int i, double *v)
{
    if (!is_type(p, i, RDOF_JNUM))
        return false;
    *v = N(p, i)->num;
    return true;
}

/* ── the keys each place takes (as the schema) ─────────────────────────────── */

/* A device's own keys (rdof/site.h checks name, hears, transmits and the talker keys' use). */
#define DEV_COMMON "kind", "name", "hears", "transmits", "listen_s", "clear_s", "idle_after_s", "csbk_preambles", \
                   "answer_s", "answer_max_s", "prebuffer", "jitter_buffer", "jitter_min_ms", "jitter_max_ms",  \
                   "cwid", "cwid_every_s"
static const char *const SDR_KEYS[] = {DEV_COMMON, "driver", "match", "rx", "tx", "bench", "usb", "dsp_cores",
                                       "spike", "selftest", "sim", "idle_gate", "learn_ppm", "sim_ppm", "afc",
                                       "rssi_offset", "status_seconds", NULL};
static const char *const MODEM_KEYS[] = {DEV_COMMON, "type", "port", "needs", "mmdvm", "tx", "bench", NULL};
static const char *const RADIO_KEYS[] = {DEV_COMMON, "port", "tx", "bench", NULL};
static const char *const MODEM_TX_KEYS[] = {"min_dbm", "max_dbm", NULL};
static const char *const SDR_RX_KEYS[] = {"rate", "center_hz", "gain_db", "agc", "ppm", "biastee", "iq_bits", NULL};
static const char *const SDR_TX_KEYS[] = {"carriers", "power_dbm", "center_hz", "bench", NULL};
static const char *const MATCH_KEYS[] = {"serial", "usb_port", "uri", "args", "index", NULL};
static const char *const PORT_KEYS[] = {"uart", "tx", "rx", "rts", "cts", "baud", "usb", "vid", "pid", "serial",
                                        "usb_port", "path", NULL};
static const char *const DEV_BENCH_KEYS[] = {"freqs_hz", "max_key_s", NULL};
/* A modem's MMDVM settings (config.c radio_settings). */
static const char *const MMDVM_KEYS[] = {"rx_invert", "tx_invert", "ptt_invert", "tx_delay_ms", "rx_level",
                                         "tx_level", "cwid_level", "rx_dc_offset", "tx_dc_offset", "dmr_delay",
                                         "rx_offset_hz", "tx_offset_hz", "headers", "terminators", "tuner", "rssi",
                                         "protocol", "modem_address", NULL};
static const char *const TUNER_KEYS[] = {"type", "freq", "settle_ms", NULL};
static const char *const AGC_KEYS[] = {"min", "clip", "headroom_db", "hold_s", "relax_s", "settle_s", NULL};
static const char *const USB_KEYS[] = {"transfers", "transfer_kb", NULL};
static const char *const SPLIT_KEYS[] = {"power_cap", "prebuffer", "lease_s", "cooldown_s", "max_key_s", NULL};
/* The board's own sections. */
static const char *const LOCATION_KEYS[] = {"latitude", "lat", "longitude", "lon", "height", NULL};
static const char *const OLED_KEYS[] = {"address", "sh1106", "rotate180", "refresh_s", "sda", "scl", "i2c", NULL};
static const char *const I2C_KEYS[] = {"address", "sh1106", "rotate180", NULL};
static const char *const TIMING_KEYS[] = {"mode", "offset_us", "max_err_us", "on_loss", "holdover_s", NULL};
static const char *const WIFI_KEYS[] = {"networks", NULL};
static const char *const NET_KEYS[] = {"ssid", "password", "priority", "hidden", NULL};
static const char *const NTP_KEYS[] = {"servers", "poll_s", "clock", NULL};
static const char *const LOAD_KEYS[] = {"channels", "rates", NULL};
static const char *const BENCH_KEYS[] = {"devices", "tx", NULL};
static const char *const BENCH_TX_KEYS[] = {"freqs_hz", "bands_hz", "max_key_s", "max_dbm", "rf_level", "arm_gpio",
                                            NULL};

/* The most carriers an SDR transmits at once. */
#define MAX_CARRIERS 4

static bool in_list(const char *const *keys, const char *k)
{
    for (; *keys; keys++)
        if (!strcmp(*keys, k))
            return true;
    return false;
}

/* obj (an object, or nothing) takes only these keys. */
static bool known(plan_t *p, int obj, const char *path, const char *const *keys)
{
    if (!is_type(p, obj, RDOF_JOBJ))
        return true;
    for (int k = 0; k < count(p, obj); k++) {
        const char *key = N(p, at(p, obj, k))->key;
        if (!in_list(keys, key))
            return fail(p, "%s%s%s: not a key here", path, path[0] ? "." : "", key);
    }
    return true;
}

static bool box_keys(plan_t *p)
{
    static const struct {
        const char *key;
        const char *const *keys;
    } sec[] = {{"location", LOCATION_KEYS}, {"oled", OLED_KEYS}, {"slot_timing", TIMING_KEYS},
               {"wifi", WIFI_KEYS},         {"ntp", NTP_KEYS},   {"selftest_load", LOAD_KEYS},
               {"bench", BENCH_KEYS}};
    int root = p->c->root;
    for (size_t k = 0; k < sizeof sec / sizeof sec[0]; k++)
        if (!known(p, get(p, root, sec[k].key), sec[k].key, sec[k].keys))
            return false;
    if (!known(p, get(p, get(p, root, "oled"), "i2c"), "oled.i2c", I2C_KEYS) ||
        !known(p, get(p, get(p, root, "bench"), "tx"), "bench.tx", BENCH_TX_KEYS))
        return false;
    int nets = get(p, get(p, root, "wifi"), "networks");
    for (int k = 0; is_type(p, nets, RDOF_JARR) && k < count(p, nets); k++)
        if (!known(p, at(p, nets, k), "wifi.networks[]", NET_KEYS))
            return false;
    int hub = get(p, root, "usb_hub");
    if (hub >= 0 && !is_type(p, hub, RDOF_JBOOL))
        return fail(p, "usb_hub: true or false");
    return true;
}

/* ── devices ───────────────────────────────────────────────────────────────── */

static bool hex16(plan_t *p, int i, uint16_t *out)
{
    double d;
    *out = 0;
    if (i < 0)
        return true;
    if (num_of(p, i, &d) && d >= 0 && d <= 0xFFFF && d == (int)d) {
        *out = (uint16_t)d;
        return true;
    }
    const char *s = str_of(p, i);
    char *e;
    long v = s && s[0] ? strtol(s, &e, 16) : -1;
    if (!s || !s[0] || *e || v < 0 || v > 0xFFFF)
        return false;
    *out = (uint16_t)v;
    return true;
}

static bool int_in(plan_t *p, int i, long lo, long hi, long dflt, long *out)
{
    double d;
    if (i < 0) {
        *out = dflt;
        return true;
    }
    if (!num_of(p, i, &d) || d != floor(d) || d < lo || d > hi)
        return false;
    *out = (long)d;
    return true;
}

static bool port(plan_t *p, int o, const char *path, rdof_port_t *out)
{
    memset(out, 0, sizeof *out);
    out->rts = out->cts = -1;
    out->baud = 115200;
    if (!is_type(p, o, RDOF_JOBJ))
        return fail(p, "%s: a port {\"uart\": ...}, {\"usb\": ...} or {\"path\": ...}", path);
    if (!known(p, o, path, PORT_KEYS))
        return false;
    int u = get(p, o, "uart"), b = get(p, o, "usb"), f = get(p, o, "path");
    if ((u >= 0) + (b >= 0) + (f >= 0) != 1)
        return fail(p, "%s: one of uart, usb or path", path);
    long v;
    if (!int_in(p, get(p, o, "baud"), 1200, 6000000, 115200, &out->baud))
        return fail(p, "%s.baud: 1200 to 6000000", path);
    if (u >= 0) {
        out->kind = RDOF_PORT_UART;
        if (!int_in(p, u, 1, 2, 1, &v))
            return fail(p, "%s.uart: 1 or 2 (UART0 is the console)", path);
        out->uart = (int)v;
        static const char *const pins[] = {"tx", "rx", "rts", "cts"};
        int *dst[] = {&out->tx, &out->rx, &out->rts, &out->cts};
        for (int k = 0; k < 4; k++) {
            int i = get(p, o, pins[k]);
            if (k < 2 && i < 0)
                return fail(p, "%s.%s: the GPIO", path, pins[k]);
            if (!int_in(p, i, k < 2 ? 0 : -1, 48, -1, &v))
                return fail(p, "%s.%s: a GPIO 0 to 48", path, pins[k]);
            *dst[k] = (int)v;
        }
        for (int a = 0; a < 4; a++)
            for (int z = a + 1; z < 4; z++)
                if (*dst[a] >= 0 && *dst[a] == *dst[z])
                    return fail(p, "%s: %s and %s on the same GPIO", path, pins[a], pins[z]);
        for (int k = 6; PORT_KEYS[k]; k++)
            if (get(p, o, PORT_KEYS[k]) >= 0)
                return fail(p, "%s.%s: a USB device's key, not a UART's", path, PORT_KEYS[k]);
        return true;
    }
    if (f >= 0) {
        out->kind = RDOF_PORT_PATH;
        const char *s = str_of(p, f);
        if (!s || !s[0] || strlen(s) >= sizeof out->path)
            return fail(p, "%s.path: a serial device's path", path);
        snprintf(out->path, sizeof out->path, "%s", s);
        return true;
    }
    out->kind = RDOF_PORT_USB;
    const char *s = str_of(p, b);
    if (!s || (strcmp(s, "auto") && strcmp(s, "cdc-acm") && strcmp(s, "uart")))
        return fail(p, "%s.usb: auto, cdc-acm or uart", path);
    snprintf(out->usb, sizeof out->usb, "%s", s);
    if (!hex16(p, get(p, o, "vid"), &out->vid) || !hex16(p, get(p, o, "pid"), &out->pid))
        return fail(p, "%s: vid and pid, four hex digits", path);
    static const struct {
        const char *k;
        size_t off, cap;
    } strs[] = {{"serial", offsetof(rdof_port_t, serial), sizeof out->serial},
                {"usb_port", offsetof(rdof_port_t, usb_port), sizeof out->usb_port}};
    for (size_t k = 0; k < 2; k++) {
        int i = get(p, o, strs[k].k);
        if (i < 0)
            continue;
        const char *t = str_of(p, i);
        if (!t || strlen(t) >= strs[k].cap)
            return fail(p, "%s.%s: a string", path, strs[k].k);
        snprintf((char *)out + strs[k].off, strs[k].cap, "%s", t);
    }
    return true;
}

static bool dev_bench(plan_t *p, int b, const char *path, rdof_dev_bench_t *out)
{
    memset(out, 0, sizeof *out);
    out->max_key_s = -1;
    if (b < 0)
        return true;
    if (!is_type(p, b, RDOF_JOBJ))
        return fail(p, "%s: an object", path);
    if (!known(p, b, path, DEV_BENCH_KEYS))
        return false;
    out->on = true;
    int f = get(p, b, "freqs_hz");
    if (f >= 0 && !is_type(p, f, RDOF_JARR))
        return fail(p, "%s.freqs_hz: a list of frequencies in Hz", path);
    for (int k = 0; f >= 0 && k < count(p, f); k++) {
        double v;
        if (k >= (int)(sizeof out->freqs_hz / sizeof out->freqs_hz[0]))
            return fail(p, "%s.freqs_hz: at most %d", path, (int)(sizeof out->freqs_hz / sizeof out->freqs_hz[0]));
        if (!num_of(p, at(p, f, k), &v) || v < 30e6 || v > 3e9 || v != floor(v))
            return fail(p, "%s.freqs_hz[%d]: a frequency in whole Hz", path, k);
        out->freqs_hz[out->nfreqs++] = (uint32_t)v;
    }
    int m = get(p, b, "max_key_s");
    if (m >= 0 && (!num_of(p, m, &out->max_key_s) || out->max_key_s < 0 || out->max_key_s > 60))
        return fail(p, "%s.max_key_s: 0 to 60 seconds", path);
    return true;
}

static bool center(plan_t *p, int i, const char *path)
{
    double v;
    if (i < 0 || (str_of(p, i) && !strcmp(str_of(p, i), "auto")))
        return true;
    return (num_of(p, i, &v) && v > 0) || fail(p, "%s: a frequency, or \"auto\"", path);
}

static bool sdr_device(plan_t *p, int d, int e, const char *path, rdof_plan_dev_t *dev)
{
    char q[96];
    if (!known(p, e, path, SDR_KEYS))
        return false;
    int i = get(p, e, "driver");
    if (i >= 0) {
        static const char *const drivers[] = {"rtlsdr", "lr2021", "sx1255", "sim", "soapy", "iio", NULL};
        if (!str_of(p, i) || !in_list(drivers, str_of(p, i)))
            return fail(p, "%s.driver: rtlsdr, lr2021, sx1255, sim, soapy or iio", path);
    }
    snprintf(q, sizeof q, "%s.match", path);
    if (!known(p, get(p, e, "match"), q, MATCH_KEYS))
        return false;
    snprintf(q, sizeof q, "%s.usb", path);
    if (!known(p, get(p, e, "usb"), q, USB_KEYS))
        return false;
    int rx = get(p, e, "rx"), tx = get(p, e, "tx");
    if (rx >= 0 && !is_type(p, rx, RDOF_JOBJ))
        return fail(p, "%s.rx: an object", path);
    snprintf(q, sizeof q, "%s.rx", path);
    if (!known(p, rx, q, SDR_RX_KEYS))
        return false;
    snprintf(q, sizeof q, "%s.rx.agc", path);
    if (!known(p, get(p, rx, "agc"), q, AGC_KEYS))
        return false;
    double v;
    if (get(p, rx, "rate") >= 0 && (!num_of(p, get(p, rx, "rate"), &v) || v <= 0))
        return fail(p, "%s.rx.rate: samples a second", path);
    snprintf(q, sizeof q, "%s.rx.center_hz", path);
    if (!center(p, get(p, rx, "center_hz"), q))
        return false;
    dev->tx_carriers = 1;
    if (tx >= 0) {
        if (!is_type(p, tx, RDOF_JOBJ))
            return fail(p, "%s.tx: an object", path);
        snprintf(q, sizeof q, "%s.tx", path);
        if (!known(p, tx, q, SDR_TX_KEYS))
            return false;
        long n;
        if (!int_in(p, get(p, tx, "carriers"), 1, MAX_CARRIERS, 1, &n))
            return fail(p, "%s.tx.carriers: 1 to %d", path, MAX_CARRIERS);
        dev->tx_carriers = (int)n;
        i = get(p, tx, "power_dbm");
        if (i >= 0 && (!num_of(p, i, &dev->tx_power_dbm) || dev->tx_power_dbm < -60 || dev->tx_power_dbm > 40))
            return fail(p, "%s.tx.power_dbm: -60 to 40 (dBm at the antenna)", path);
        dev->has_tx_power = i >= 0;
        snprintf(q, sizeof q, "%s.tx.center_hz", path);
        if (!center(p, get(p, tx, "center_hz"), q))
            return false;
        snprintf(q, sizeof q, "%s.tx.bench", path);
        if (!dev_bench(p, get(p, tx, "bench"), q, &dev->bench))
            return false;
    }
    dev->txmap = rdof_txpower_sdr_map(dev->has_tx_power ? dev->tx_power_dbm : 10);
    /* it transmits: it says so ("tx"), or a frequency has it */
    dev->tx = tx >= 0;
    for (int f = 0; f < p->site.nfreq; f++)
        dev->tx |= rdof_site_dev_sends(&p->site, d, f);
    snprintf(q, sizeof q, "%s.bench", path);
    if (get(p, e, "bench") >= 0 && get(p, tx, "bench") >= 0)
        return fail(p, "%s: bench and tx.bench: one or the other", path);
    if (get(p, e, "bench") >= 0 && !dev_bench(p, get(p, e, "bench"), q, &dev->bench))
        return false;
    return true;
}

static bool modem_device(plan_t *p, int e, const char *path, rdof_plan_dev_t *dev, bool radio)
{
    char q[96];
    if (!known(p, e, path, radio ? RADIO_KEYS : MODEM_KEYS))
        return false;
    snprintf(q, sizeof q, "%s.port", path);
    if (!port(p, get(p, e, "port"), q, &dev->port))
        return false;
    snprintf(q, sizeof q, "%s.bench", path);
    if (!dev_bench(p, get(p, e, "bench"), q, &dev->bench))
        return false;
    /* tx: its power at RF level 0 and 255, dBm at the antenna */
    int t = get(p, e, "tx");
    if (t >= 0) {
        if (!is_type(p, t, RDOF_JOBJ))
            return fail(p, "%s.tx: an object", path);
        snprintf(q, sizeof q, "%s.tx", path);
        if (!known(p, t, q, MODEM_TX_KEYS))
            return false;
        const int lo = get(p, t, "min_dbm"), hi = get(p, t, "max_dbm");
        if ((lo >= 0) != (hi >= 0))
            return fail(p, "%s.tx: min_dbm and max_dbm, both", path);
        if (lo >= 0 && (!num_of(p, lo, &dev->txmap.min_dbm) || !num_of(p, hi, &dev->txmap.max_dbm) ||
                        dev->txmap.min_dbm < -60 || dev->txmap.max_dbm > 60 || !rdof_txmap_valid(&dev->txmap)))
            return fail(p, "%s.tx: min_dbm under max_dbm, -60 to 60 (dBm at the antenna)", path);
    }
    if (radio)
        return true;
    int i = get(p, e, "type");
    if (i >= 0) {
        static const char *const types[] = {"auto", "radio_hotspot", "mmdvm_hs", "mmdvm", NULL};
        if (!str_of(p, i) || !in_list(types, str_of(p, i)))
            return fail(p, "%s.type: auto, radio_hotspot, mmdvm_hs or mmdvm", path);
    }
    int n = get(p, e, "needs");
    if (n >= 0 && !is_type(p, n, RDOF_JARR))
        return fail(p, "%s.needs: a list of duplex, split, dcdm, sfr", path);
    for (int k = 0; k < count(p, n); k++) {
        const char *s = str_of(p, at(p, n, k));
        unsigned bit = !s ? 0 : !strcmp(s, "duplex") ? RDOF_NEED_DUPLEX : !strcmp(s, "split") ? RDOF_NEED_SPLIT
                              : !strcmp(s, "dcdm")   ? RDOF_NEED_DCDM   : !strcmp(s, "sfr")   ? RDOF_NEED_SFR : 0;
        if (!bit)
            return fail(p, "%s.needs[%d]: duplex, split, dcdm or sfr", path, k);
        dev->needs |= bit;
    }
    int m = get(p, e, "mmdvm");
    if (m >= 0 && !is_type(p, m, RDOF_JOBJ))
        return fail(p, "%s.mmdvm: an object", path);
    snprintf(q, sizeof q, "%s.mmdvm", path);
    if (!known(p, m, q, MMDVM_KEYS))
        return false;
    snprintf(q, sizeof q, "%s.mmdvm.tuner", path);
    return known(p, get(p, m, "tuner"), q, TUNER_KEYS);
}

static bool devices(plan_t *p)
{
    const rdof_site_t *s = &p->site;
    for (int d = 0; d < s->ndev; d++) {
        char path[64];
        snprintf(path, sizeof path, "devices.%s", s->dev[d].name);
        int e = p->c->dnode[d];
        rdof_plan_dev_t *dev = &p->dev[d];
        memset(dev, 0, sizeof *dev);
        bool ok = s->dev[d].kind == RDOF_DEV_SDR ? sdr_device(p, d, e, path, dev)
                                                 : modem_device(p, e, path, dev, s->dev[d].kind == RDOF_DEV_RADIO);
        if (!ok)
            return false;
    }
    /* bench.devices */
    int bd = get(p, get(p, p->c->root, "bench"), "devices");
    if (bd >= 0 && !is_type(p, bd, RDOF_JARR))
        return fail(p, "bench.devices: a list of device names");
    for (int k = 0; k < count(p, bd); k++) {
        const char *name = str_of(p, at(p, bd, k));
        int i = -1;
        for (int d = 0; name && d < s->ndev && i < 0; d++)
            if (!strcmp(s->dev[d].name, name))
                i = d;
        if (i < 0)
            return fail(p, "bench.devices[%d]: %s isn't in devices", k, name ? name : "(not a name)");
        for (int m = 0; m < p->nbench; m++)
            if (p->bench[m] == i)
                return fail(p, "bench.devices[%d]: %s twice", k, name);
        const rdof_plan_dev_t *dv = &p->dev[i];
        if (s->dev[i].kind != RDOF_DEV_SDR && dv->port.kind == RDOF_PORT_USB && !strcmp(dv->port.usb, "auto"))
            return fail(p, "devices.%s.port.usb: a bench device is cdc-acm (with vid and pid) or uart", name);
        p->bench[p->nbench++] = i;
    }
    return true;
}

/* ── units ─────────────────────────────────────────────────────────────────── */

static bool unit_name_ok(const char *s)
{
    if (!s[0] || strlen(s) >= RDOF_SITE_NAME)
        return false;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && *s != '-' && *s != '_' && *s != '.')
            return false;
    return true;
}

/* A unit's place in the document, for its errors. */
static void unit_path(const plan_t *p, const rdof_unit_t *u, char *out, size_t cap)
{
    const rdof_site_t *s = &p->site;
    switch (u->kind) {
    case RDOF_UNIT_REPEATER: snprintf(out, cap, "apps.%s", s->app[u->app].name); break;
    case RDOF_UNIT_SFR:
    case RDOF_UNIT_DCDM: snprintf(out, cap, "frequencies.%s", s->freq[u->freq].name); break;
    default: snprintf(out, cap, "devices.%s", s->dev[u->rx].name); break;
    }
}

static rdof_unit_t *add_unit(plan_t *p, const char *name, int kind, const char *where)
{
    if (p->nunit >= RDOF_PLAN_MAX_UNITS) {
        fail(p, "%d units at most (repeaters, SFR and DCDM frequencies, transceivers, receivers)",
             RDOF_PLAN_MAX_UNITS);
        return NULL;
    }
    if (!unit_name_ok(name)) {
        fail(p, "%s: it runs as %s, which needs a name of letters, digits, '-', '_' and '.', under %d characters",
             where, name, RDOF_SITE_NAME);
        return NULL;
    }
    for (int k = 0; k < p->nunit; k++)
        if (!strcmp(p->unit[k].name, name)) {
            char other[RDOF_SITE_NAME + 16];
            unit_path(p, &p->unit[k], other, sizeof other);
            fail(p, "%s: it runs as %s, and so does %s: rename one", where, name, other);
            return NULL;
        }
    rdof_unit_t *u = &p->unit[p->nunit++];
    memset(u, 0, sizeof *u);
    snprintf(u->name, sizeof u->name, "%s", name);
    u->kind = (uint8_t)kind;
    u->rx = u->tx = u->app = u->freq = -1;
    return u;
}

/* Distinct frequencies in a set (bit per frequency). */
static int distinct_hz(const rdof_site_t *s, uint64_t set)
{
    int n = 0;
    for (int f = 0; f < s->nfreq; f++) {
        if (!(set >> f & 1))
            continue;
        bool dup = false;
        for (int g = 0; g < f && !dup; g++)
            dup = (set >> g & 1) && s->freq[g].hz == s->freq[f].hz;
        n += !dup;
    }
    return n;
}

static bool make_units(plan_t *p)
{
    const rdof_site_t *s = &p->site;
    uint64_t done = 0, mine = 0;
    /* repeaters: the device that transmits on out; on in, that device when it's an SDR that hears it, else the
       first that hears it */
    for (int a = 0; a < s->napp; a++) {
        const rdof_app3_t *ap = &s->app[a];
        if (ap->type != RDOF_APP_REPEATER)
            continue;
        int rx = -1, tx = -1;
        for (int d = 0; d < s->ndev && tx < 0; d++)
            if (rdof_site_dev_sends(s, d, ap->out))
                tx = d;
        if (tx >= 0 && rdof_site_dev_hears(s, tx, ap->in) && s->dev[tx].kind == RDOF_DEV_SDR)
            rx = tx;
        for (int d = 0; d < s->ndev && rx < 0; d++)
            if (rdof_site_dev_hears(s, d, ap->in))
                rx = d;
        if (tx < 0)
            return fail(p, "apps.%s.out: no device transmits on %s", ap->name, s->freq[ap->out].name);
        if (rx < 0)
            return fail(p, "apps.%s.in: no device hears %s", ap->name, s->freq[ap->in].name);
        char where[64];
        snprintf(where, sizeof where, "apps.%s", ap->name);
        rdof_unit_t *u = add_unit(p, ap->name, RDOF_UNIT_REPEATER, where);
        if (!u)
            return false;
        u->rx = rx;
        u->tx = tx;
        u->app = a;
        done |= 1ull << ap->in | 1ull << ap->out;
        mine |= 1ull << ap->in | 1ull << ap->out;
    }
    /* SFR and DCDM frequencies: the first device that transmits there */
    for (int f = 0; f < s->nfreq; f++) {
        const rdof_freq_t *fr = &s->freq[f];
        if (fr->mode == RDOF_FMODE_SIMPLEX)
            continue;
        int d = -1;
        for (int x = 0; x < s->ndev && d < 0; x++)
            if (rdof_site_dev_sends(s, x, f))
                d = x;
        if (d < 0)
            return fail(p, "frequencies.%s: no device transmits on it", fr->name);
        char nm[RDOF_SITE_NAME + 8], where[64];
        snprintf(nm, sizeof nm, "%s", fr->name);
        for (char *q = nm; *q; q++)
            if (!isalnum((unsigned char)*q) && *q != '-' && *q != '_' && *q != '.')
                *q = '_';
        snprintf(where, sizeof where, "frequencies.%s", fr->name);
        rdof_unit_t *u = add_unit(p, nm, fr->mode == RDOF_FMODE_SFR ? RDOF_UNIT_SFR : RDOF_UNIT_DCDM, where);
        if (!u)
            return false;
        u->rx = u->tx = d;
        u->freq = f;
        const char *dx = str_of(p, get(p, p->c->fnode[f], "duplex"));
        u->duplex_full = dx && !strcmp(dx, "full");
        done |= 1ull << f;
    }
    /* a transceiver: each device that transmits on simplex frequencies no app or gateway has */
    for (int d = 0; d < s->ndev; d++) {
        const rdof_device3_t *dv = &s->dev[d];
        uint64_t ch = 0;
        for (int f = 0; f < s->nfreq; f++)
            if (rdof_site_dev_sends(s, d, f) && !(done >> f & 1) && s->freq[f].mode == RDOF_FMODE_SIMPLEX &&
                s->freq[f].app < 0)
                ch |= 1ull << f;
        if (!ch)
            continue;
        int idle = dv->idle >= 0 && (ch >> dv->idle & 1) ? dv->idle : -1;
        for (int f = 0; f < s->nfreq && idle < 0; f++)
            if (ch >> f & 1)
                idle = f;
        char where[64];
        snprintf(where, sizeof where, "devices.%s", dv->name);
        int n = 0;
        for (int f = 0; f < s->nfreq; f++)
            n += (int)(ch >> f & 1);
        if (n > RDOF_PLAN_MAX_FREQS) /* each a channel of its own (rdof_config_t.channels) */
            return fail(p, "%s.transmits: %d frequencies, and a transceiver holds %d", where, n,
                        RDOF_PLAN_MAX_FREQS);
        rdof_unit_t *u = add_unit(p, dv->name, RDOF_UNIT_TRANSCEIVER, where);
        if (!u)
            return false;
        u->rx = u->tx = d;
        u->freq = idle;
        u->freqs = ch;
    }
    /* a receiver: each SDR that hears a frequency past its repeaters' own (a repeater reports its own input) */
    for (int d = 0; d < s->ndev; d++) {
        const rdof_device3_t *dv = &s->dev[d];
        if (dv->kind != RDOF_DEV_SDR || dv->hears != RDOF_HEARS_WINDOW)
            continue;
        uint64_t heard = 0;
        for (int f = 0; f < s->nfreq; f++)
            if (rdof_site_dev_hears(s, d, f) && !(mine >> f & 1))
                heard |= 1ull << f;
        if (!heard)
            continue;
        char nm[RDOF_SITE_NAME + 8], where[64];
        snprintf(nm, sizeof nm, "%s-rx", dv->name);
        snprintf(where, sizeof where, "devices.%s", dv->name);
        if (distinct_hz(s, heard) > RDOF_PLAN_MAX_FREQS)
            return fail(p, "%s: it hears %d frequencies, %d at most", where, distinct_hz(s, heard),
                        RDOF_PLAN_MAX_FREQS);
        rdof_unit_t *u = add_unit(p, nm, RDOF_UNIT_RECEIVER, where);
        if (!u)
            return false;
        u->rx = d;
        u->freqs = heard;
    }
    return true;
}

static bool hotspot(const rdof_site_t *s, int d)
{
    return !strcmp(s->dev[d].driver, "auto") || !strcmp(s->dev[d].driver, "radio_hotspot");
}

/* What each unit's devices must be; a modem or a radio serves one unit. */
static bool check_units(plan_t *p)
{
    const rdof_site_t *s = &p->site;
    for (int k = 0; k < p->nunit; k++) {
        const rdof_unit_t *u = &p->unit[k];
        char where[64];
        unit_path(p, u, where, sizeof where);
        if (u->kind == RDOF_UNIT_RECEIVER)
            continue;
        const rdof_device3_t *rx = &s->dev[u->rx], *tx = &s->dev[u->tx];
        if (u->rx == u->tx) {
            if (u->kind == RDOF_UNIT_SFR && rx->kind == RDOF_DEV_SDR)
                return fail(p, "%s: devices.%s is an SDR: an SFR repeater on an SDR isn't done yet (a radio in "
                               "hotspot mode times its slots)", where, rx->name);
            if (u->kind == RDOF_UNIT_SFR && !hotspot(s, u->rx))
                return fail(p, "%s: devices.%s is a %s: a single frequency repeater is a radio_hotspot (a radio in "
                               "hotspot mode with SFR firmware)", where, rx->name, rx->driver);
            if (u->duplex_full && rx->kind != RDOF_DEV_SDR)
                return fail(p, "%s.duplex: full is for a DCDM gateway on an SDR (devices.%s is a modem)", where,
                            rx->name);
        } else {
            if (tx->kind == RDOF_DEV_SDR)
                return fail(p, "%s: devices.%s is an SDR: an SDR as a split's TX half isn't done yet (its sample "
                               "clock and the split's need tying together)", where, tx->name);
            for (int h = 0; h < 2; h++) {
                int d = h ? u->tx : u->rx;
                if (s->dev[d].kind == RDOF_DEV_MODEM && !hotspot(s, d))
                    return fail(p, "%s: devices.%s is a %s: a split half is a radio in hotspot mode with the split "
                                   "duplex extension", where, s->dev[d].name, s->dev[d].driver);
            }
        }
    }
    /* a modem or a radio: one unit */
    for (int d = 0; d < s->ndev; d++) {
        if (s->dev[d].kind == RDOF_DEV_SDR)
            continue;
        int first = -1;
        for (int k = 0; k < p->nunit; k++) {
            const rdof_unit_t *u = &p->unit[k];
            if (u->rx != d && u->tx != d)
                continue;
            if (first >= 0) {
                char a[64], b[64];
                unit_path(p, &p->unit[first], a, sizeof a);
                unit_path(p, u, b, sizeof b);
                return fail(p, "devices.%s: %s and %s both need it: a modem serves one (pin a frequency's devices, or "
                               "set its transmits)", s->dev[d].name, a, b);
            }
            first = k;
        }
    }
    return true;
}

/* The key checks a unit's own object takes past the parse's (rdof/site.h). */
static bool unit_keys(plan_t *p)
{
    const rdof_site_t *s = &p->site;
    char q[96];
    for (int a = 0; a < s->napp; a++) {
        if (s->app[a].type != RDOF_APP_REPEATER)
            continue;
        int e = p->c->anode[a];
        snprintf(q, sizeof q, "apps.%s.split", s->app[a].name);
        if (!known(p, get(p, e, "split"), q, SPLIT_KEYS))
            return false;
        snprintf(q, sizeof q, "apps.%s.slot_timing", s->app[a].name);
        if (!known(p, get(p, e, "slot_timing"), q, TIMING_KEYS))
            return false;
    }
    for (int f = 0; f < s->nfreq; f++) {
        snprintf(q, sizeof q, "frequencies.%s.slot_timing", s->freq[f].name);
        if (!known(p, get(p, p->c->fnode[f], "slot_timing"), q, TIMING_KEYS))
            return false;
    }
    return true;
}

/* Each frequency an SDR hears for a unit on it that its receiver doesn't: an extra channel. */
static bool make_extras(plan_t *p)
{
    const rdof_site_t *s = &p->site;
    for (int k = 0; k < p->nunit; k++) {
        const rdof_unit_t *u = &p->unit[k];
        if (u->kind == RDOF_UNIT_RECEIVER || s->dev[u->rx].kind != RDOF_DEV_SDR)
            continue;
        /* its frequencies (in order), each with its color code */
        uint32_t f[RDOF_PLAN_MAX_FREQS];
        int cc[RDOF_PLAN_MAX_FREQS], n = 0;
        if (u->kind == RDOF_UNIT_TRANSCEIVER) {
            for (int x = 0; x < s->nfreq; x++) {
                if (!(u->freqs >> x & 1))
                    continue;
                bool dup = false;
                for (int i = 0; i < n; i++)
                    dup |= f[i] == s->freq[x].hz;
                if (!dup && n < RDOF_PLAN_MAX_FREQS) {
                    f[n] = s->freq[x].hz;
                    cc[n++] = s->freq[x].cc;
                }
            }
        } else if (u->kind == RDOF_UNIT_REPEATER) {
            f[n] = s->freq[s->app[u->app].in].hz;
            cc[n++] = s->freq[s->app[u->app].out].cc; /* the repeater's color code: its out's */
        } else {
            f[n] = s->freq[u->freq].hz;
            cc[n++] = s->freq[u->freq].cc;
        }
        int rcv = rdof_plan_receiver(p, u->rx);
        for (int i = 0; i < n; i++) {
            bool heard = false;
            for (int x = 0; rcv >= 0 && x < s->nfreq && !heard; x++)
                heard = (p->unit[rcv].freqs >> x & 1) && llabs((long long)s->freq[x].hz - (long long)f[i]) <= 100;
            for (int x = 0; x < p->nextra && !heard; x++)
                heard = p->extra[x].dev == u->rx && llabs((long long)p->extra[x].hz - (long long)f[i]) <= 100;
            if (heard)
                continue;
            if (p->nextra >= MAX_EXTRA)
                return fail(p, "devices.%s: too many frequencies to hear", s->dev[u->rx].name);
            rdof_plan_extra_t *x = &p->extra[p->nextra++];
            x->hz = f[i];
            x->cc = cc[i] >= 0 ? cc[i] : 1;
            x->dev = u->rx;
            snprintf(x->id, sizeof x->id, "%s-%d", u->name, i + 1);
        }
    }
    return true;
}

/* ── what the devices can do ───────────────────────────────────────────────── */

/* An SDR's receive channels (RDOF_SDR_MAX_CHANNELS); the rate it runs at unless rx.rate says (rdof_plan_sdr's). */
#define SDR_CHANNELS 16
#define SDR_RATE 1.536e6

/* What an SDR's driver gives: the rtlsdr receives 24 to 1766 MHz at up to 3.2 MS/s; the rest transmit one carrier
   each until more are tried on the air, and the LR2021 is half duplex (one chain, switched). */
typedef struct {
    bool tx, full_duplex;
    int carriers;
    double rate_max;             /* 0: not known */
    uint32_t freq_min, freq_max; /* 0, 0: not known */
} sdr_caps_t;

static sdr_caps_t sdr_caps(const char *driver)
{
    if (!strcmp(driver, "rtlsdr"))
        return (sdr_caps_t){.rate_max = 3.2e6, .freq_min = 24000000, .freq_max = 1766000000};
    return (sdr_caps_t){.tx = true, .full_duplex = strcmp(driver, "lr2021") != 0, .carriers = 1};
}

/* The frequencies an SDR hears for unit u on it (bit per frequency). */
static uint64_t unit_hears(const plan_t *p, const rdof_unit_t *u)
{
    switch (u->kind) {
    case RDOF_UNIT_RECEIVER:
    case RDOF_UNIT_TRANSCEIVER: return u->freqs;
    case RDOF_UNIT_REPEATER: return 1ull << p->site.app[u->app].in;
    default: return 1ull << u->freq;
    }
}

static bool sdr_hears(plan_t *p, int d, const sdr_caps_t *c)
{
    const rdof_site_t *s = &p->site;
    const char *name = s->dev[d].name;
    int rx = get(p, p->c->dnode[d], "rx");
    double rate = SDR_RATE, center = 0;
    num_of(p, get(p, rx, "rate"), &rate);
    if (num_of(p, get(p, rx, "center_hz"), &center) && center < 1e5)
        center *= 1e6;
    if (c->rate_max && rate > c->rate_max)
        return fail(p, "devices.%s.rx.rate: %.0f, and the %s samples %.0f a second at most", name, rate,
                    s->dev[d].driver, c->rate_max);
    for (int k = 0; k < p->nunit; k++) {
        const rdof_unit_t *u = &p->unit[k];
        if (u->rx != d)
            continue;
        uint64_t set = unit_hears(p, u);
        for (int f = 0; f < s->nfreq; f++) {
            if (!(set >> f & 1))
                continue;
            uint32_t hz = s->freq[f].hz;
            if (c->freq_max && (hz < c->freq_min || hz > c->freq_max))
                return fail(p, "frequencies.%s: devices.%s can't hear %.5f MHz (the %s tunes %.0f to %.0f MHz)",
                            s->freq[f].name, name, hz / 1e6, s->dev[d].driver, c->freq_min / 1e6,
                            c->freq_max / 1e6);
            double off = (double)hz - center;
            if (center > 0 && (off < -rate / 2 || off > rate / 2))
                return fail(p, "frequencies.%s: %.5f MHz is outside devices.%s's window (%.5f MHz, %.0f S/s)",
                            s->freq[f].name, hz / 1e6, name, center / 1e6, rate);
        }
    }
    /* its channels: its receiver's frequencies, then the extra ones (rdof_plan_sdr's) */
    int n = 0, rcv = rdof_plan_receiver(p, d);
    for (int f = 0; rcv >= 0 && f < s->nfreq; f++)
        if ((p->unit[rcv].freqs >> f & 1) && ++n > SDR_CHANNELS)
            return fail(p, "devices.%s: no receive channel left for frequencies.%s (%.5f MHz, %d at most)", name,
                        s->freq[f].name, s->freq[f].hz / 1e6, SDR_CHANNELS);
    for (int x = 0; x < p->nextra; x++)
        if (p->extra[x].dev == d && ++n > SDR_CHANNELS) {
            int f = rdof_site_freq_hz(s, p->extra[x].hz);
            return fail(p, "devices.%s: no receive channel left for frequencies.%s (%.5f MHz, %d at most)", name,
                        f >= 0 ? s->freq[f].name : "?", p->extra[x].hz / 1e6, SDR_CHANNELS);
        }
    return true;
}

static bool sdr_sends(plan_t *p, int d, const sdr_caps_t *c)
{
    const rdof_site_t *s = &p->site;
    const rdof_device3_t *dv = &s->dev[d];
    const rdof_plan_dev_t *pd = &p->dev[d];
    if (!pd->tx)
        return true;
    if (!c->tx)
        return fail(p, "devices.%s: the %s can't transmit", dv->name, dv->driver);
    if (pd->tx_carriers > c->carriers)
        return fail(p, "devices.%s.tx.carriers: %d, and the %s sends %d (one carrier a transmitter for now)",
                    dv->name, pd->tx_carriers, dv->driver, c->carriers);
    int carriers = pd->tx_carriers, used = 0;
    char names[96] = "";
    for (int k = 0; k < p->nunit; k++) {
        const rdof_unit_t *u = &p->unit[k];
        if (u->tx != d || u->kind == RDOF_UNIT_RECEIVER)
            continue;
        char where[64];
        unit_path(p, u, where, sizeof where);
        if (u->kind == RDOF_UNIT_REPEATER && !c->full_duplex)
            return fail(p, "%s: devices.%s (%s) is half duplex: one chain, switched; a duplex repeater needs RX "
                           "while it transmits", where, dv->name, dv->driver);
        if (u->kind == RDOF_UNIT_DCDM && u->duplex_full && !c->full_duplex)
            return fail(p, "%s.duplex: full, and devices.%s (%s) is half duplex", where, dv->name, dv->driver);
        if (used == carriers)
            return fail(p, "devices.%s: carrier %d of %d (%s transmits on it already)", dv->name, carriers + 1,
                        carriers, names);
        size_t o = strlen(names);
        snprintf(names + o, sizeof names - o, "%s%s", used ? ", " : "", u->name);
        used++;
    }
    return true;
}

/* What each SDR's driver can do, and the bench's devices kept to the bench. */
static bool check_caps(plan_t *p)
{
    const rdof_site_t *s = &p->site;
    for (int d = 0; d < s->ndev; d++) {
        if (s->dev[d].kind != RDOF_DEV_SDR)
            continue;
        sdr_caps_t c = sdr_caps(s->dev[d].driver);
        if (!sdr_sends(p, d, &c) || !sdr_hears(p, d, &c))
            return false;
    }
    /* a modem on the bench serves no unit (an SDR's bench takes a carrier through the router's pool) */
    for (int b = 0; b < p->nbench; b++) {
        int d = p->bench[b];
        for (int k = 0; s->dev[d].kind != RDOF_DEV_SDR && k < p->nunit; k++)
            if (p->unit[k].rx == d || p->unit[k].tx == d) {
                char where[64];
                unit_path(p, &p->unit[k], where, sizeof where);
                return fail(p, "devices.%s: on the bench and %s takes it: one or the other", s->dev[d].name,
                            where);
            }
    }
    return true;
}

/* ── the platform ──────────────────────────────────────────────────────────── */

const rdof_plan_platform_t rdof_plan_board = {.name = "this board", .uart_ports = true, .max_radios = RDOF_MAX_RADIOS};
const rdof_plan_platform_t rdof_plan_host = {.name = "this host", .path_ports = true, .host_drivers = true,
                                             .max_radios = RDOF_ROLES_MAX};

bool rdof_plan_check(const rdof_plan_t *p, const rdof_plan_platform_t *plat, char *err, size_t cap)
{
    char dummy[8];
    if (!err || !cap)
        err = dummy, cap = sizeof dummy;
    const rdof_site_t *s = &p->site;
    for (int d = 0; d < s->ndev; d++) {
        const rdof_device3_t *dv = &s->dev[d];
        const rdof_port_t *pt = &p->dev[d].port;
        if (dv->kind == RDOF_DEV_SDR && !plat->host_drivers &&
            (!strcmp(dv->driver, "soapy") || !strcmp(dv->driver, "iio"))) {
            snprintf(err, cap, "devices.%s.driver: %s is the host tool's, not %s's", dv->name, dv->driver, plat->name);
            return false;
        }
        if (dv->kind != RDOF_DEV_SDR && pt->kind == RDOF_PORT_PATH && !plat->path_ports) {
            snprintf(err, cap, "devices.%s.port: a serial path is for a host; %s has uart and usb ports", dv->name,
                     plat->name);
            return false;
        }
        if (dv->kind != RDOF_DEV_SDR && pt->kind == RDOF_PORT_UART && !plat->uart_ports) {
            snprintf(err, cap, "devices.%s.port: %s has no UARTs of its own (give the serial device's path)",
                     dv->name, plat->name);
            return false;
        }
    }
    return true;
}

/* ── the plan ──────────────────────────────────────────────────────────────── */

rdof_plan_t *rdof_plan_open(const char *text, size_t len, char *err, size_t cap)
{
    plan_t *p = calloc(1, sizeof *p);
    rdof_site_ctx_t *c = p ? rdof_site_ctx_open(text, len, err, cap) : NULL;
    if (!c) {
        free(p);
        if (err && cap)
            snprintf(err, cap, "no memory");
        return NULL;
    }
    p->c = c;
    bool ok = rdof_site_ctx_parse(c, &p->site) && box_keys(p) && devices(p) && unit_keys(p) && make_units(p) &&
              check_units(p) && make_extras(p) && check_caps(p);
    if (!ok) {
        rdof_plan_close(p);
        return NULL;
    }
    return p;
}

void rdof_plan_close(rdof_plan_t *p)
{
    if (p) {
        rdof_site_ctx_close(p->c);
        free(p);
    }
}

const rdof_site_t *rdof_plan_site(const rdof_plan_t *p) { return &p->site; }
const rdof_jdoc_t *rdof_plan_doc(const rdof_plan_t *p) { return &p->c->doc; }
int rdof_plan_root(const rdof_plan_t *p) { return p->c->root; }

int rdof_plan_dev_node(const rdof_plan_t *p, int dev)
{
    return dev >= 0 && dev < p->site.ndev ? p->c->dnode[dev] : -1;
}

int rdof_plan_freq_node(const rdof_plan_t *p, int freq)
{
    return freq >= 0 && freq < p->site.nfreq ? p->c->fnode[freq] : -1;
}

int rdof_plan_app_node(const rdof_plan_t *p, int app)
{
    return app >= 0 && app < p->site.napp ? p->c->anode[app] : -1;
}

int rdof_plan_net_node(const rdof_plan_t *p, int net)
{
    return net >= 0 && net < p->site.nnet ? p->c->nnode[net] : -1;
}

int rdof_plan_net_of(const rdof_plan_t *p, int type)
{
    for (int k = 0; k < p->site.nnet; k++)
        if (p->site.net[k].type == type)
            return k;
    return -1;
}

const rdof_plan_dev_t *rdof_plan_dev(const rdof_plan_t *p, int dev) { return &p->dev[dev]; }
int rdof_plan_nunits(const rdof_plan_t *p) { return p->nunit; }
const rdof_unit_t *rdof_plan_unit(const rdof_plan_t *p, int k) { return &p->unit[k]; }

int rdof_plan_unit_find(const rdof_plan_t *p, const char *name)
{
    for (int k = 0; name && k < p->nunit; k++)
        if (!strcmp(p->unit[k].name, name))
            return k;
    return -1;
}

int rdof_plan_nextra(const rdof_plan_t *p) { return p->nextra; }
const rdof_plan_extra_t *rdof_plan_extra(const rdof_plan_t *p, int k) { return &p->extra[k]; }
int rdof_plan_nbench(const rdof_plan_t *p) { return p->nbench; }
int rdof_plan_bench(const rdof_plan_t *p, int k) { return p->bench[k]; }

int rdof_plan_receiver(const rdof_plan_t *p, int dev)
{
    for (int k = 0; k < p->nunit; k++)
        if (p->unit[k].kind == RDOF_UNIT_RECEIVER && p->unit[k].rx == dev)
            return k;
    return -1;
}

bool rdof_plan_sdr_used(const rdof_plan_t *p, int dev)
{
    for (int k = 0; k < p->nunit; k++)
        if (p->unit[k].rx == dev || p->unit[k].tx == dev)
            return true;
    return false;
}

const char *rdof_plan_sdr_name(const rdof_plan_t *p, int dev)
{
    int r = rdof_plan_receiver(p, dev);
    return r >= 0 ? p->unit[r].name : p->site.dev[dev].name;
}

bool rdof_plan_roles_on(const rdof_plan_t *p, const rdof_plan_platform_t *plat, rdof_roles_t *roles, char *err,
                        size_t cap)
{
    char dummy[8];
    if (!err || !cap)
        err = dummy, cap = sizeof dummy;
    memset(roles, 0, sizeof *roles);
    snprintf(roles->box, sizeof roles->box, "%.*s", (int)sizeof roles->box - 1, p->site.name);
    int hub = rdof_jget(&p->c->doc, p->c->root, "usb_hub");
    roles->usb_hub = hub >= 0 && rdof_jnode(&p->c->doc, hub)->type == RDOF_JBOOL && rdof_jnode(&p->c->doc, hub)->b;
    int sdr = -1;
    for (int k = 0; k < p->nunit; k++) {
        const rdof_unit_t *u = &p->unit[k];
        if (u->kind != RDOF_UNIT_RECEIVER) {
            const int max = plat->max_radios < RDOF_ROLES_MAX ? plat->max_radios : RDOF_ROLES_MAX;
            if (roles->nradios >= max) {
                if (max == 2)
                    snprintf(err, cap, "a %s runs 2 units that transmit at most: %s, %s and %s", plat->name + 5,
                             roles->radios[0], roles->radios[1], u->name);
                else
                    snprintf(err, cap, "a %s runs %d units that transmit at most: %s is one more", plat->name + 5,
                             max, u->name);
                return false;
            }
            snprintf(roles->radios[roles->nradios++], sizeof roles->radios[0], "%s", u->name);
        }
        for (int h = 0; h < 2; h++) {
            int d = h ? u->tx : u->rx;
            if (d < 0 || p->site.dev[d].kind != RDOF_DEV_SDR || d == sdr)
                continue;
            if (sdr >= 0) {
                snprintf(err, cap, "devices.%s and devices.%s: a board runs one SDR", p->site.dev[sdr].name,
                         p->site.dev[d].name);
                return false;
            }
            sdr = d;
        }
    }
    if ((roles->has_radio = roles->nradios > 0))
        snprintf(roles->radio, sizeof roles->radio, "%s", roles->radios[0]);
    if ((roles->has_sdr = sdr >= 0))
        snprintf(roles->sdr, sizeof roles->sdr, "%s", rdof_plan_sdr_name(p, sdr));
    if (!roles->nradios && sdr < 0) {
        snprintf(err, cap, "nothing to run: no device transmits or hears");
        return false;
    }
    return true;
}

bool rdof_plan_roles(const rdof_plan_t *p, rdof_roles_t *roles, char *err, size_t cap)
{
    return rdof_plan_roles_on(p, &rdof_plan_board, roles, err, cap);
}
