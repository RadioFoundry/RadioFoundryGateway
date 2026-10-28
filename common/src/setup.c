// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The settings, version 2: devices, roles, run; the upgrade from version 1 and the view the role parsers read
   (rdof/setup.h, docs/spec/fw/rxtx-combinations.md section 4). */
#include "rdof/setup.h"

#include <ctype.h>
#include <stddef.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/broker.h"
#include "rdof/config.h"
#include "rdof/jsonp.h"
#include "rdof/status.h"

#define MAX_NODES 2000
#define STR_BUF 16384

typedef struct {
    rdof_jdoc_t doc;
    rdof_jnode_t nodes[MAX_NODES];
    char strs[STR_BUF];
    char *clean;
    int root;
    char *error;
    size_t cap;
} ctx_t;

static bool fail(ctx_t *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->error, c->cap, fmt, ap);
    va_end(ap);
    return false;
}

static const rdof_jnode_t *N(ctx_t *c, int i)
{
    return rdof_jnode(&c->doc, i);
}

static int get(ctx_t *c, int obj, const char *key)
{
    return obj < 0 ? -1 : rdof_jget(&c->doc, obj, key);
}

static int at(ctx_t *c, int node, int i)
{
    return rdof_jat(&c->doc, node, i);
}

static bool is_type(ctx_t *c, int i, rdof_jtype_t t)
{
    return i >= 0 && N(c, i)->type == t;
}

static const char *str_of(ctx_t *c, int i)
{
    return is_type(c, i, RDOF_JSTR) ? N(c, i)->str : NULL;
}

static bool num_of(ctx_t *c, int i, double *v)
{
    if (!is_type(c, i, RDOF_JNUM))
        return false;
    *v = N(c, i)->num;
    return true;
}

/* A frequency: MHz below 1e5, else Hz (box._hz). */
static uint32_t hz(double f)
{
    double v = rint(f < 1e5 ? f * 1e6 : f);
    return v <= 0 ? 0 : v >= 4294967295.0 ? UINT32_MAX : (uint32_t)v;
}

static ctx_t *open_doc(const char *text, size_t len, char *error, size_t cap)
{
    ctx_t *c = malloc(sizeof *c);
    char *clean = c ? malloc(len + 1) : NULL;
    if (!clean) {
        free(c);
        if (error && cap)
            snprintf(error, cap, "no memory");
        return NULL;
    }
    static char dummy[8];
    c->error = error && cap ? error : dummy;
    c->cap = error && cap ? cap : sizeof dummy;
    c->error[0] = 0;
    c->clean = clean;
    c->doc = (rdof_jdoc_t){.nodes = c->nodes, .max = MAX_NODES, .buf = c->strs, .cap = STR_BUF};
    size_t n = rdof_jsonc_strip(text, len, clean);
    clean[n] = 0;
    c->root = rdof_jparse(&c->doc, clean, n);
    return c;
}

static void close_doc(ctx_t *c)
{
    if (c) {
        free(c->clean);
        free(c);
    }
}

/* ── the version ───────────────────────────────────────────────────────────── */

static int version_of(ctx_t *c)
{
    if (c->root < 0 || N(c, c->root)->type != RDOF_JOBJ)
        return -1;
    int v = get(c, c->root, "version");
    if (v >= 0) {
        double d;
        if (!num_of(c, v, &d) || (d != 1 && d != 2))
            return -1;
        return (int)d;
    }
    if (get(c, c->root, "devices") >= 0 || get(c, c->root, "roles") >= 0)
        return -1; /* version 2's keys without its version */
    if (get(c, c->root, "radios") < 0 && get(c, c->root, "device") >= 0)
        return 0;
    return 1;
}

int rdof_config_version(const char *text, size_t len)
{
    ctx_t *c = open_doc(text, len, NULL, 0);
    if (!c)
        return -1;
    int v = version_of(c);
    close_doc(c);
    return v;
}

/* ── the keys each place takes (as the schema) ─────────────────────────────── */

static const char *const TOP_KEYS[] = {"version", "name", "location", "cc", "channels", "mqtt", "hblink",
                                       "slot_timing", "oled", "ip_gateway", "wifi", "ntp", "usb_hub",
                                       "selftest_load", "devices", "roles", "run", "bench", NULL};
static const char *const SDR_KEYS[] = {"kind", "driver", "match", "rx", "tx", "bench", "usb", "dsp_cores", "spike",
                                       "selftest", "sim", "idle_gate", "learn_ppm", "sim_ppm", "afc", "rssi_offset",
                                       "status_seconds", "selftest_load", NULL};
/* An SDR's own keys a version 1 SDR section had at its top level. */
static const char *const SDR_BOARD_KEYS[] = {"usb", "dsp_cores", "spike", "selftest", "sim", "idle_gate",
                                             "learn_ppm", "sim_ppm", "afc", "rssi_offset", "status_seconds",
                                             "selftest_load", NULL};
static const char *const SDR_RX_KEYS[] = {"rate", "center_hz", "gain_db", "agc", "ppm", "biastee", "iq_bits", NULL};
static const char *const SDR_TX_KEYS[] = {"carriers", "power_dbm", "center_hz", "bench", NULL};
static const char *const MATCH_KEYS[] = {"serial", "usb_port", "uri", "args", "index", NULL};
static const char *const MODEM_KEYS[] = {"kind", "type", "port", "needs", "mmdvm", "bench", NULL};
static const char *const RADIO_KEYS[] = {"kind", "port", "bench", NULL};
static const char *const PORT_KEYS[] = {"uart", "tx", "rx", "rts", "cts", "baud", "usb", "vid", "pid", "serial",
                                        "usb_port", "path", NULL};
static const char *const DEV_BENCH_KEYS[] = {"freqs_hz", "max_key_s", NULL};
/* A modem's MMDVM settings: a version 1 radio's "radio" keys (config.c radio_settings). */
static const char *const MMDVM_KEYS[] = {"rx_invert", "tx_invert", "ptt_invert", "tx_delay_ms", "rx_level",
                                         "tx_level", "cwid_level", "rx_dc_offset", "tx_dc_offset", "dmr_delay",
                                         "rx_offset_hz", "tx_offset_hz", "headers", "terminators", "tuner", "rssi",
                                         "protocol", "modem_address", NULL};
static const char *const SPLIT_KEYS[] = {"power_cap", "prebuffer", "lease_s", "cooldown_s", "max_key_s", NULL};

/* Keys every role but a monitor takes. */
#define TALKER_KEYS                                                                                                \
    "type", "enabled", "modem", "rx", "tx", "hblink", "location", "peer_id", "callsign", "cwid", "cwid_every_s",   \
        "parrot", "prebuffer", "jitter_buffer", "jitter_min_ms", "jitter_max_ms"
static const char *const ROAMER_KEYS[] = {TALKER_KEYS, "idle", "channels", "listen_s", "clear_s", "idle_after_s",
                                          "scan", "csbk_preambles", "answer_s", "answer_max_s", NULL};
static const char *const REPEATER_KEYS[] = {TALKER_KEYS, "rx_freq", "tx_freq", "cc", "power", "slots", "hang_s",
                                            "call_hang_s", "short_lc_ts1_low", "slot_timing", "split", NULL};
static const char *const SFR_KEYS[] = {TALKER_KEYS, "freq", "cc", "power", "hang_s", "call_hang_s",
                                       "inbound_slot", "short_lc_ts1_low", "slot_timing", NULL};
static const char *const DCDM_KEYS[] = {TALKER_KEYS, "freq", "cc", "power", "slots", "hang_s", "call_hang_s",
                                        "short_lc_ts1_low", "slot_timing", "duplex", NULL};
static const char *const MONITOR_KEYS[] = {"type", "enabled", "rx", "channels", "receiver_ids", "peer_ids",
                                           "callsign", "hblink", "location", "slot_timing", NULL};
static const char *const DEVREF_KEYS[] = {"device", NULL};

/* The box's own sections. */
static const char *const MQTT_KEYS[] = {"host", "port", "username", "password", "root", "tls", NULL};
static const char *const HBLINK_KEYS[] = {"host", "port", "transport", "sctp_encap_port", "sctp_heartbeat_ms",
                                          "sctp_max_retransmit", "sctp_ttl_ms", NULL};
static const char *const LOCATION_KEYS[] = {"latitude", "lat", "longitude", "lon", "height", NULL};
static const char *const OLED_KEYS[] = {"address", "sh1106", "rotate180", "refresh_s", "sda", "scl", "i2c", NULL};
static const char *const I2C_KEYS[] = {"address", "sh1106", "rotate180", NULL};
static const char *const TIMING_KEYS[] = {"mode", "offset_us", "max_err_us", "on_loss", "holdover_s", NULL};
static const char *const PLAN_KEYS[] = {"freq", "cc", "power", "name", "slot", "tg", NULL};
static const char *const WIFI_KEYS[] = {"networks", NULL};
static const char *const NET_KEYS[] = {"ssid", "password", "priority", "hidden", NULL};
static const char *const NTP_KEYS[] = {"servers", "poll_s", NULL};
static const char *const LOAD_KEYS[] = {"channels", "rates", NULL};
static const char *const BENCH_KEYS[] = {"devices", "tx", NULL};
static const char *const BENCH_TX_KEYS[] = {"freqs_hz", "max_key_s", "rf_level", "arm_gpio", NULL};
static const char *const ROAM_CH_KEYS[] = {"ch", "cc", "power", NULL};
static const char *const MON_CH_KEYS[] = {"ch", "id", "freq", "name", "cc", "slot", "tg", "hblink", "peer_id", NULL};
static const char *const IDLE_KEYS[] = {"freq", "cc", NULL};
static const char *const SCAN_KEYS[] = {"channels", "dwell_s", "hang_s", "carrier_s", NULL};
static const char *const TUNER_KEYS[] = {"type", "freq", "settle_ms", NULL};
static const char *const AGC_KEYS[] = {"min", "clip", "headroom_db", "hold_s", "relax_s", "settle_s", NULL};
static const char *const USB_KEYS[] = {"transfers", "transfer_kb", NULL};

static bool in_list(const char *const *keys, const char *k)
{
    for (; *keys; keys++)
        if (!strcmp(*keys, k))
            return true;
    return false;
}

/* obj (an object, or nothing) takes only these keys. */
static bool known(ctx_t *c, int obj, const char *path, const char *const *keys)
{
    if (!is_type(c, obj, RDOF_JOBJ))
        return true;
    for (int k = 0; k < N(c, obj)->count; k++) {
        const char *key = N(c, at(c, obj, k))->key;
        if (!in_list(keys, key))
            return fail(c, "%s%s%s: not a key here", path, path[0] ? "." : "", key);
    }
    return true;
}

/* The box's own sections: their keys, as the schema. */
static bool box_keys(ctx_t *c, int box)
{
    static const struct {
        const char *key;
        const char *const *keys;
    } sec[] = {{"mqtt", MQTT_KEYS},   {"hblink", HBLINK_KEYS}, {"location", LOCATION_KEYS},
               {"oled", OLED_KEYS},   {"slot_timing", TIMING_KEYS}, {"wifi", WIFI_KEYS},
               {"ntp", NTP_KEYS},     {"selftest_load", LOAD_KEYS}};
    if (!known(c, box, "", TOP_KEYS))
        return false;
    for (size_t k = 0; k < sizeof sec / sizeof sec[0]; k++)
        if (!known(c, get(c, box, sec[k].key), sec[k].key, sec[k].keys))
            return false;
    if (!known(c, get(c, get(c, box, "oled"), "i2c"), "oled.i2c", I2C_KEYS))
        return false;
    int nets = get(c, get(c, box, "wifi"), "networks");
    for (int k = 0; is_type(c, nets, RDOF_JARR) && k < N(c, nets)->count; k++)
        if (!known(c, at(c, nets, k), "wifi.networks[]", NET_KEYS))
            return false;
    int plan = get(c, box, "channels");
    for (int k = 0; is_type(c, plan, RDOF_JOBJ) && k < N(c, plan)->count; k++) {
        char path[48];
        snprintf(path, sizeof path, "channels.%s", N(c, at(c, plan, k))->key);
        if (!known(c, at(c, plan, k), path, PLAN_KEYS))
            return false;
    }
    return true;
}

/* ── the plan ──────────────────────────────────────────────────────────────── */

typedef struct {
    int number;
    uint32_t freq;
    int cc;
} pch_t;

typedef struct {
    pch_t ch[128];
    int n;
    int dflt_cc; /* -1 none */
} plan_t;

static void read_plan(ctx_t *c, int box, plan_t *p)
{
    p->n = 0;
    double d;
    p->dflt_cc = num_of(c, get(c, box, "cc"), &d) ? (int)d : -1;
    int chans = get(c, box, "channels");
    for (int k = 0; is_type(c, chans, RDOF_JOBJ) && k < N(c, chans)->count && p->n < 128; k++) {
        int ch = at(c, chans, k);
        char *e;
        long number = strtol(N(c, ch)->key, &e, 10);
        if (!N(c, ch)->key[0] || *e)
            continue; /* the role parsers say what's wrong with it */
        int f = is_type(c, ch, RDOF_JOBJ) ? get(c, ch, "freq") : ch;
        double v;
        if (!num_of(c, f, &v))
            continue;
        pch_t *x = &p->ch[p->n++];
        x->number = (int)number;
        x->freq = hz(v);
        x->cc = num_of(c, get(c, ch, "cc"), &d) ? (int)d : p->dflt_cc;
    }
}

static const pch_t *plan_find(const plan_t *p, long n)
{
    for (int k = p->n - 1; k >= 0; k--) /* "1" and "01": the last */
        if (p->ch[k].number == n)
            return &p->ch[k];
    return NULL;
}

static int plan_max(const plan_t *p)
{
    int m = 0;
    for (int k = 0; k < p->n; k++)
        if (p->ch[k].number > m)
            m = p->ch[k].number;
    return m;
}

/* ── version 2: devices ────────────────────────────────────────────────────── */

static bool name_ok(const char *s)
{
    if (!s[0] || strlen(s) >= RDOF_SETUP_NAME)
        return false;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && *s != '-' && *s != '_' && *s != '.')
            return false;
    return true;
}

static bool hex16(ctx_t *c, int i, uint16_t *out)
{
    double d;
    *out = 0;
    if (i < 0)
        return true;
    if (num_of(c, i, &d) && d >= 0 && d <= 0xFFFF && d == (int)d) {
        *out = (uint16_t)d;
        return true;
    }
    const char *s = str_of(c, i);
    char *e;
    long v = s && s[0] ? strtol(s, &e, 16) : -1;
    if (!s || !s[0] || *e || v < 0 || v > 0xFFFF)
        return false;
    *out = (uint16_t)v;
    return true;
}

static bool int_in(ctx_t *c, int i, long lo, long hi, long dflt, long *out)
{
    double d;
    if (i < 0) {
        *out = dflt;
        return true;
    }
    if (!num_of(c, i, &d) || d != floor(d) || d < lo || d > hi)
        return false;
    *out = (long)d;
    return true;
}

static bool port(ctx_t *c, int p, const char *path, rdof_port_t *out)
{
    memset(out, 0, sizeof *out);
    out->rts = out->cts = -1;
    out->baud = 115200;
    if (!is_type(c, p, RDOF_JOBJ))
        return fail(c, "%s: a port {\"uart\": ...}, {\"usb\": ...} or {\"path\": ...}", path);
    if (!known(c, p, path, PORT_KEYS))
        return false;
    int u = get(c, p, "uart"), b = get(c, p, "usb"), f = get(c, p, "path");
    if ((u >= 0) + (b >= 0) + (f >= 0) != 1)
        return fail(c, "%s: one of uart, usb or path", path);
    long v;
    if (!int_in(c, get(c, p, "baud"), 1200, 6000000, 115200, &out->baud))
        return fail(c, "%s.baud: 1200 to 6000000", path);
    if (u >= 0) {
        out->kind = RDOF_PORT_UART;
        if (!int_in(c, u, 1, 2, 1, &v))
            return fail(c, "%s.uart: 1 or 2 (UART0 is the console)", path);
        out->uart = (int)v;
        static const char *const pins[] = {"tx", "rx", "rts", "cts"};
        int *dst[] = {&out->tx, &out->rx, &out->rts, &out->cts};
        for (int k = 0; k < 4; k++) {
            int i = get(c, p, pins[k]);
            if (k < 2 && i < 0)
                return fail(c, "%s.%s: the GPIO", path, pins[k]);
            if (!int_in(c, i, k < 2 ? 0 : -1, 48, -1, &v))
                return fail(c, "%s.%s: a GPIO 0 to 48", path, pins[k]);
            *dst[k] = (int)v;
        }
        for (int a = 0; a < 4; a++)
            for (int z = a + 1; z < 4; z++)
                if (*dst[a] >= 0 && *dst[a] == *dst[z])
                    return fail(c, "%s: %s and %s on the same GPIO", path, pins[a], pins[z]);
        for (int k = 0; k < (int)(sizeof PORT_KEYS / sizeof PORT_KEYS[0]) - 1; k++)
            if (k >= 6 && get(c, p, PORT_KEYS[k]) >= 0)
                return fail(c, "%s.%s: a USB device's key, not a UART's", path, PORT_KEYS[k]);
        return true;
    }
    if (f >= 0) {
        out->kind = RDOF_PORT_PATH;
        const char *s = str_of(c, f);
        if (!s || !s[0] || strlen(s) >= sizeof out->path)
            return fail(c, "%s.path: a serial device's path", path);
        snprintf(out->path, sizeof out->path, "%s", s);
        return true;
    }
    out->kind = RDOF_PORT_USB;
    const char *s = str_of(c, b);
    if (!s || (strcmp(s, "auto") && strcmp(s, "cdc-acm") && strcmp(s, "uart")))
        return fail(c, "%s.usb: auto, cdc-acm or uart", path);
    snprintf(out->usb, sizeof out->usb, "%s", s);
    if (!hex16(c, get(c, p, "vid"), &out->vid) || !hex16(c, get(c, p, "pid"), &out->pid))
        return fail(c, "%s: vid and pid, four hex digits", path);
    static const struct {
        const char *k;
        size_t off, cap;
    } strs[] = {{"serial", offsetof(rdof_port_t, serial), sizeof out->serial},
                {"usb_port", offsetof(rdof_port_t, usb_port), sizeof out->usb_port}};
    for (size_t k = 0; k < 2; k++) {
        int i = get(c, p, strs[k].k);
        if (i < 0)
            continue;
        const char *t = str_of(c, i);
        if (!t || strlen(t) >= strs[k].cap)
            return fail(c, "%s.%s: a string", path, strs[k].k);
        snprintf((char *)out + strs[k].off, strs[k].cap, "%s", t);
    }
    return true;
}

static bool dev_bench(ctx_t *c, int b, const char *path, rdof_dev_bench_t *out)
{
    memset(out, 0, sizeof *out);
    out->max_key_s = -1;
    if (b < 0)
        return true;
    if (!is_type(c, b, RDOF_JOBJ))
        return fail(c, "%s: an object", path);
    if (!known(c, b, path, DEV_BENCH_KEYS))
        return false;
    out->on = true;
    int f = get(c, b, "freqs_hz");
    if (f >= 0 && !is_type(c, f, RDOF_JARR))
        return fail(c, "%s.freqs_hz: a list of frequencies in Hz", path);
    for (int k = 0; f >= 0 && k < N(c, f)->count; k++) {
        double v;
        if (k >= (int)(sizeof out->freqs_hz / sizeof out->freqs_hz[0]))
            return fail(c, "%s.freqs_hz: at most %d", path, (int)(sizeof out->freqs_hz / sizeof out->freqs_hz[0]));
        if (!num_of(c, at(c, f, k), &v) || v < 30e6 || v > 3e9 || v != floor(v))
            return fail(c, "%s.freqs_hz[%d]: a frequency in whole Hz", path, k);
        out->freqs_hz[out->nfreqs++] = (uint32_t)v;
    }
    int m = get(c, b, "max_key_s");
    if (m >= 0 && (!num_of(c, m, &out->max_key_s) || out->max_key_s < 0 || out->max_key_s > 60))
        return fail(c, "%s.max_key_s: 0 to 60 seconds", path);
    return true;
}

static bool sdr_device(ctx_t *c, int d, const char *path, rdof_device_t *dev)
{
    char p[64];
    if (!known(c, d, path, SDR_KEYS))
        return false;
    const char *drv = "rtlsdr";
    int i = get(c, d, "driver");
    if (i >= 0) {
        drv = str_of(c, i);
        static const char *const drivers[] = {"rtlsdr", "lr2021", "sx1255", "sim", "soapy", "iio", NULL};
        if (!drv || !in_list(drivers, drv))
            return fail(c, "%s.driver: rtlsdr, lr2021, sx1255, sim, soapy or iio", path);
    }
    snprintf(dev->driver, sizeof dev->driver, "%s", drv);
    snprintf(p, sizeof p, "%s.match", path);
    if (!known(c, get(c, d, "match"), p, MATCH_KEYS))
        return false;
    snprintf(p, sizeof p, "%s.usb", path);
    if (!known(c, get(c, d, "usb"), p, USB_KEYS))
        return false;
    int rx = get(c, d, "rx"), tx = get(c, d, "tx");
    if (rx >= 0 && !is_type(c, rx, RDOF_JOBJ))
        return fail(c, "%s.rx: an object", path);
    snprintf(p, sizeof p, "%s.rx", path);
    if (!known(c, rx, p, SDR_RX_KEYS))
        return false;
    snprintf(p, sizeof p, "%s.rx.agc", path);
    if (!known(c, get(c, rx, "agc"), p, AGC_KEYS))
        return false;
    double v;
    if (get(c, rx, "rate") >= 0 && (!num_of(c, get(c, rx, "rate"), &dev->rx_rate) || dev->rx_rate <= 0))
        return fail(c, "%s.rx.rate: samples a second", path);
    i = get(c, rx, "center_hz");
    if (i >= 0 && !(str_of(c, i) && !strcmp(str_of(c, i), "auto"))) {
        if (!num_of(c, i, &v) || v <= 0)
            return fail(c, "%s.rx.center_hz: a frequency, or \"auto\"", path);
        dev->rx_center = hz(v);
    }
    if (tx >= 0) {
        if (!is_type(c, tx, RDOF_JOBJ))
            return fail(c, "%s.tx: an object", path);
        snprintf(p, sizeof p, "%s.tx", path);
        if (!known(c, tx, p, SDR_TX_KEYS))
            return false;
        dev->tx = true;
        long n;
        if (!int_in(c, get(c, tx, "carriers"), 1, RDOF_BROKER_MAX_CARRIERS, 1, &n))
            return fail(c, "%s.tx.carriers: 1 to %d", path, RDOF_BROKER_MAX_CARRIERS);
        dev->tx_carriers = (int)n;
        i = get(c, tx, "power_dbm");
        if (i >= 0 && (!num_of(c, i, &dev->tx_power_dbm) || dev->tx_power_dbm < -60 || dev->tx_power_dbm > 30))
            return fail(c, "%s.tx.power_dbm: -60 to 30", path);
        dev->has_tx_power = i >= 0;
        i = get(c, tx, "center_hz");
        if (i >= 0 && !(str_of(c, i) && !strcmp(str_of(c, i), "auto"))) {
            if (!num_of(c, i, &v) || v <= 0)
                return fail(c, "%s.tx.center_hz: a frequency, or \"auto\"", path);
            dev->tx_center = hz(v);
        }
        snprintf(p, sizeof p, "%s.tx.bench", path);
        if (!dev_bench(c, get(c, tx, "bench"), p, &dev->bench))
            return false;
    }
    snprintf(p, sizeof p, "%s.bench", path);
    if (get(c, d, "bench") >= 0 && !dev_bench(c, get(c, d, "bench"), p, &dev->bench))
        return false;
    return true;
}

static bool modem_device(ctx_t *c, int d, const char *path, rdof_device_t *dev, bool radio)
{
    char p[64];
    if (!known(c, d, path, radio ? RADIO_KEYS : MODEM_KEYS))
        return false;
    snprintf(p, sizeof p, "%s.port", path);
    if (!port(c, get(c, d, "port"), p, &dev->port))
        return false;
    snprintf(p, sizeof p, "%s.bench", path);
    if (!dev_bench(c, get(c, d, "bench"), p, &dev->bench))
        return false;
    if (radio) {
        snprintf(dev->driver, sizeof dev->driver, "odr-debug");
        return true;
    }
    const char *t = "auto";
    int i = get(c, d, "type");
    if (i >= 0) {
        t = str_of(c, i);
        static const char *const types[] = {"auto", "radio_hotspot", "mmdvm_hs", "mmdvm", NULL};
        if (!t || !in_list(types, t))
            return fail(c, "%s.type: auto, radio_hotspot, mmdvm_hs or mmdvm", path);
    }
    snprintf(dev->driver, sizeof dev->driver, "%s", t);
    int n = get(c, d, "needs");
    if (n >= 0 && !is_type(c, n, RDOF_JARR))
        return fail(c, "%s.needs: a list of duplex, split, dcdm, sfr", path);
    for (int k = 0; n >= 0 && k < N(c, n)->count; k++) {
        const char *s = str_of(c, at(c, n, k));
        unsigned bit = !s ? 0 : !strcmp(s, "duplex") ? RDOF_NEED_DUPLEX : !strcmp(s, "split") ? RDOF_NEED_SPLIT
                              : !strcmp(s, "dcdm")   ? RDOF_NEED_DCDM   : !strcmp(s, "sfr")   ? RDOF_NEED_SFR : 0;
        if (!bit)
            return fail(c, "%s.needs[%d]: duplex, split, dcdm or sfr", path, k);
        dev->needs |= bit;
    }
    int m = get(c, d, "mmdvm");
    if (m >= 0 && !is_type(c, m, RDOF_JOBJ))
        return fail(c, "%s.mmdvm: an object", path);
    snprintf(p, sizeof p, "%s.mmdvm", path);
    if (!known(c, m, p, MMDVM_KEYS))
        return false;
    snprintf(p, sizeof p, "%s.mmdvm.tuner", path);
    if (!known(c, get(c, m, "tuner"), p, TUNER_KEYS))
        return false;
    dev->has_mmdvm = m >= 0 && N(c, m)->count > 0;
    return true;
}

/* ── version 2: roles ──────────────────────────────────────────────────────── */

#define AUTO_DEV (-2)

/* rx / tx / modem: {"device": NAME or "auto"} -> its index, AUTO_DEV, or -1 when absent. */
static bool devref(ctx_t *c, rdof_setup_t *s, int obj, const char *key, const char *path, int *out)
{
    int r = get(c, obj, key);
    *out = -1;
    if (r < 0)
        return true;
    char p[64];
    snprintf(p, sizeof p, "%s.%s", path, key);
    if (!is_type(c, r, RDOF_JOBJ))
        return fail(c, "%s: {\"device\": a device's name, or \"auto\"}", p);
    if (!known(c, r, p, DEVREF_KEYS))
        return false;
    const char *name = str_of(c, get(c, r, "device"));
    if (!name)
        return fail(c, "%s.device: a device's name, or \"auto\"", p);
    if (!strcmp(name, "auto")) {
        *out = AUTO_DEV;
        return true;
    }
    *out = rdof_setup_device(s, name);
    if (*out < 0)
        return fail(c, "%s.device: %s isn't in devices", p, name);
    return true;
}

static bool add_freq(ctx_t *c, rdof_role_t *r, uint32_t f, int cc, const char *path)
{
    for (int k = 0; k < r->nfreqs; k++)
        if (r->freqs[k] == f)
            return true;
    if (r->nfreqs >= RDOF_SETUP_MAX_FREQS)
        return fail(c, "%s: too many channels", path);
    r->fcc[r->nfreqs] = cc;
    r->freqs[r->nfreqs++] = f;
    return true;
}

/* A channel reference: n, or {"ch": n, ...}; a monitor's may give its own freq. */
static bool chan_ref(ctx_t *c, const plan_t *plan, int ref, const char *path, rdof_role_t *r, bool monitor)
{
    double v;
    int over = -1;
    if (is_type(c, ref, RDOF_JOBJ)) {
        over = ref;
        if (!known(c, ref, path, monitor ? MON_CH_KEYS : ROAM_CH_KEYS))
            return false;
        if (!num_of(c, get(c, ref, "ch"), &v))
            return fail(c, "%s: a channel entry needs ch", path);
    } else if (!num_of(c, ref, &v)) {
        return fail(c, "%s: a channel number", path);
    }
    const pch_t *pc = plan_find(plan, (long)v);
    if (!pc)
        return fail(c, "%s: channel %ld isn't in the plan (channels)", path, (long)v);
    uint32_t f = pc->freq;
    double fo, cc;
    if (monitor && num_of(c, get(c, over, "freq"), &fo))
        f = hz(fo);
    int ccv = num_of(c, get(c, over, "cc"), &cc) ? (int)cc : pc->cc;
    return add_freq(c, r, f, ccv, path);
}

static bool role_freqs(ctx_t *c, const plan_t *plan, int ro, const char *path, rdof_role_t *r)
{
    char p[80];
    double v;
    r->cc = num_of(c, get(c, ro, "cc"), &v) ? (int)v : plan->dflt_cc;
    switch (r->type) {
    case RDOF_ROLE_ROAMER: {
        int refs = get(c, ro, "channels");
        bool all = refs < 0 || (str_of(c, refs) && !strcmp(str_of(c, refs), "all"));
        if (!all && !is_type(c, refs, RDOF_JARR))
            return fail(c, "%s.channels: a list of channel numbers, or \"all\"", path);
        for (int k = 0; all && k < plan->n; k++)
            if (!plan_find(plan, plan->ch[k].number) || plan_find(plan, plan->ch[k].number) == &plan->ch[k])
                if (!add_freq(c, r, plan->ch[k].freq, plan->ch[k].cc, path))
                    return false;
        for (int k = 0; !all && k < N(c, refs)->count; k++) {
            snprintf(p, sizeof p, "%s.channels[%d]", path, k);
            if (!chan_ref(c, plan, at(c, refs, k), p, r, false))
                return false;
        }
        int idle = get(c, ro, "idle");
        snprintf(p, sizeof p, "%s.idle", path);
        if (idle < 0)
            return fail(c, "%s: the idle channel (a channel number)", p);
        if (is_type(c, idle, RDOF_JOBJ)) {
            if (!known(c, idle, p, IDLE_KEYS))
                return false;
            double cc;
            if (!num_of(c, get(c, idle, "freq"), &v) || !num_of(c, get(c, idle, "cc"), &cc))
                return fail(c, "%s: freq and cc", p);
            if (!add_freq(c, r, hz(v), (int)cc, p))
                return false;
        } else if (!chan_ref(c, plan, idle, p, r, false)) {
            return false;
        }
        int sc = get(c, ro, "scan");
        snprintf(p, sizeof p, "%s.scan", path);
        if (is_type(c, sc, RDOF_JOBJ) && !known(c, sc, p, SCAN_KEYS))
            return false;
        return true;
    }
    case RDOF_ROLE_REPEATER:
        if (!num_of(c, get(c, ro, "rx_freq"), &v))
            return fail(c, "%s.rx_freq: the frequency it hears on (MHz or Hz)", path);
        r->rx_freq = hz(v);
        if (!num_of(c, get(c, ro, "tx_freq"), &v))
            return fail(c, "%s.tx_freq: the frequency it transmits on (MHz or Hz)", path);
        r->tx_freq = hz(v);
        return add_freq(c, r, r->rx_freq, r->cc, path);
    case RDOF_ROLE_SFR:
    case RDOF_ROLE_DCDM:
        if (!num_of(c, get(c, ro, "freq"), &v))
            return fail(c, "%s.freq: its frequency (MHz or Hz)", path);
        r->rx_freq = r->tx_freq = hz(v);
        return add_freq(c, r, r->rx_freq, r->cc, path);
    case RDOF_ROLE_MONITOR: {
        int chs = get(c, ro, "channels");
        if (!is_type(c, chs, RDOF_JARR) || !N(c, chs)->count)
            return fail(c, "%s.channels: the channels it hears, a list of channel numbers", path);
        for (int k = 0; k < N(c, chs)->count; k++) {
            snprintf(p, sizeof p, "%s.channels[%d]", path, k);
            if (!chan_ref(c, plan, at(c, chs, k), p, r, true))
                return false;
        }
        return true;
    }
    }
    return true;
}

static const char *const TYPE_NAMES[] = {"", "roamer", "repeater", "sfr", "dcdm", "monitor"};

static bool parse_role(ctx_t *c, rdof_setup_t *s, const plan_t *plan, int ro, rdof_role_t *r)
{
    char path[48];
    snprintf(path, sizeof path, "roles.%s", r->name);
    if (!is_type(c, ro, RDOF_JOBJ))
        return fail(c, "%s: an object", path);
    const char *t = str_of(c, get(c, ro, "type"));
    int type = 0;
    for (int k = 1; t && k <= RDOF_ROLE_MONITOR; k++)
        if (!strcmp(t, TYPE_NAMES[k]))
            type = k;
    if (!type)
        return fail(c, "%s.type: roamer, repeater, sfr, dcdm or monitor", path);
    r->type = (rdof_role_type_t)type;
    static const char *const *const keys[] = {NULL, ROAMER_KEYS, REPEATER_KEYS, SFR_KEYS, DCDM_KEYS, MONITOR_KEYS};
    if (!known(c, ro, path, keys[type]))
        return false;
    char p[80];
    snprintf(p, sizeof p, "%s.split", path);
    if (!known(c, get(c, ro, "split"), p, SPLIT_KEYS))
        return false;
    snprintf(p, sizeof p, "%s.location", path);
    if (!known(c, get(c, ro, "location"), p, LOCATION_KEYS))
        return false;
    snprintf(p, sizeof p, "%s.slot_timing", path);
    if (!known(c, get(c, ro, "slot_timing"), p, TIMING_KEYS))
        return false;
    int e = get(c, ro, "enabled");
    if (e >= 0 && !is_type(c, e, RDOF_JBOOL))
        return fail(c, "%s.enabled: true or false", path);
    r->enabled = e < 0 || N(c, e)->b;
    int modem, rx, tx;
    if (!devref(c, s, ro, "modem", path, &modem) || !devref(c, s, ro, "rx", path, &rx) ||
        !devref(c, s, ro, "tx", path, &tx))
        return false;
    if (r->type == RDOF_ROLE_MONITOR) {
        if (rx == -1)
            return fail(c, "%s.rx: the SDR it hears on ({\"device\": NAME})", path);
        r->rx = rx;
        r->tx = -1;
    } else if (modem != -1) {
        if (rx != -1 || tx != -1)
            return fail(c, "%s: modem, or rx and tx, not both", path);
        r->rx = r->tx = modem;
    } else {
        if (rx == -1 || tx == -1)
            return fail(c, "%s: modem ({\"device\": NAME}), or rx and tx", path);
        r->rx = rx;
        r->tx = tx;
    }
    const char *d = str_of(c, get(c, ro, "duplex"));
    if (get(c, ro, "duplex") >= 0 && (!d || (strcmp(d, "half") && strcmp(d, "full"))))
        return fail(c, "%s.duplex: half or full", path);
    r->sdr_duplex_full = d && !strcmp(d, "full");
    return role_freqs(c, plan, ro, path, r);
}

/* What a role's devices must be, once auto is resolved. */
static bool role_devices(ctx_t *c, const rdof_setup_t *s, rdof_role_t *r)
{
    char path[48];
    snprintf(path, sizeof path, "roles.%s", r->name);
    const rdof_device_t *rx = r->rx >= 0 ? &s->dev[r->rx] : NULL, *tx = r->tx >= 0 ? &s->dev[r->tx] : NULL;
    if (r->type == RDOF_ROLE_MONITOR) {
        if (rx->kind != RDOF_DEV_SDR)
            return fail(c, "%s.rx: %s is a %s: a monitor hears on an SDR", path, rx->name,
                        rx->kind == RDOF_DEV_MODEM ? "modem" : "radio");
        return true;
    }
    r->split = r->rx != r->tx;
    for (int k = 0; k < 2; k++) {
        const rdof_device_t *dv = k ? tx : rx;
        if (dv->kind == RDOF_DEV_RADIO)
            return fail(c, "%s: %s is a radio on its debug link (a bench device), not a modem", path, dv->name);
    }
    if (!r->split) {
        if (rx->kind == RDOF_DEV_SDR && !rx->tx)
            return fail(c, "devices.%s: no tx: %s transmits on it (give it \"tx\": {})", rx->name, r->name);
        if (rx->kind == RDOF_DEV_SDR && r->type == RDOF_ROLE_SFR)
            return fail(c, "%s: an SFR repeater on an SDR isn't done yet (a radio in hotspot mode times its slots)",
                        path);
        if (rx->kind == RDOF_DEV_MODEM && r->type == RDOF_ROLE_SFR && strcmp(rx->driver, "auto") &&
            strcmp(rx->driver, "radio_hotspot"))
            return fail(c, "%s: devices.%s is a %s: a single frequency repeater is a radio_hotspot (a radio in "
                           "hotspot mode with SFR firmware)", path, rx->name, rx->driver);
        if (r->sdr_duplex_full && rx->kind != RDOF_DEV_SDR)
            return fail(c, "%s.duplex: for a role on an SDR", path);
        return true;
    }
    if (r->type == RDOF_ROLE_ROAMER)
        return fail(c, "%s: a roamer on two devices isn't done yet (one modem, or one SDR)", path);
    if (r->type != RDOF_ROLE_REPEATER)
        return fail(c, "%s: a%s is one radio on one frequency, not a split (rx and tx on two devices)", path,
                    r->type == RDOF_ROLE_SFR ? "n SFR repeater" : " DCDM gateway");
    if (tx->kind == RDOF_DEV_SDR)
        return fail(c, "%s.tx: an SDR as a split's TX half isn't done yet (its sample clock and the split's need "
                       "tying together; docs/spec/fw/rxtx-combinations.md section 6)", path);
    if (rx->kind == RDOF_DEV_SDR && tx->kind == RDOF_DEV_SDR)
        return fail(c, "%s: two SDRs as a split aren't done yet", path);
    for (int k = 0; k < 2; k++) {
        const rdof_device_t *dv = k ? tx : rx;
        if (dv->kind == RDOF_DEV_MODEM && strcmp(dv->driver, "auto") && strcmp(dv->driver, "radio_hotspot"))
            return fail(c, "%s.%s: devices.%s is a %s: a split half is a radio in hotspot mode with the split "
                           "duplex extension", path, k ? "tx" : "rx", dv->name, dv->driver);
    }
    return true;
}

int rdof_setup_device(const rdof_setup_t *s, const char *name)
{
    for (int k = 0; k < s->ndev; k++)
        if (!strcmp(s->dev[k].name, name))
            return k;
    return -1;
}

int rdof_setup_role(const rdof_setup_t *s, const char *name)
{
    for (int k = 0; k < s->nrole; k++)
        if (!strcmp(s->role[k].name, name))
            return k;
    return -1;
}

static bool parse_setup(ctx_t *c, rdof_setup_t *s)
{
    memset(s, 0, sizeof *s);
    int box = c->root;
    if (box < 0)
        return fail(c, "JSON: %s", c->doc.error);
    if (N(c, box)->type != RDOF_JOBJ)
        return fail(c, "the settings are a JSON object");
    if (version_of(c) != 2)
        return fail(c, "version: 2 (version 1 settings go through rdof_config_upgrade)");
    if (!box_keys(c, box))
        return false;
    const char *bn = str_of(c, get(c, box, "name"));
    snprintf(s->name, sizeof s->name, "%s", bn && bn[0] ? bn : "radiofoundry");
    s->usb_hub = is_type(c, get(c, box, "usb_hub"), RDOF_JBOOL) && N(c, get(c, box, "usb_hub"))->b;
    plan_t *plan = malloc(sizeof *plan);
    if (!plan)
        return fail(c, "no memory");
    read_plan(c, box, plan);
    bool ok = false;
    int devs = get(c, box, "devices"), roles = get(c, box, "roles");
    if (devs >= 0 && !is_type(c, devs, RDOF_JOBJ)) {
        fail(c, "devices: an object, a device by name");
        goto done;
    }
    for (int k = 0; devs >= 0 && k < N(c, devs)->count; k++) {
        int d = at(c, devs, k);
        const char *name = N(c, d)->key;
        char path[48];
        snprintf(path, sizeof path, "devices.%s", name);
        if (!name_ok(name)) {
            fail(c, "%s: a name of letters, digits, '-', '_' and '.', under %d characters", path, RDOF_SETUP_NAME);
            goto done;
        }
        if (rdof_setup_device(s, name) >= 0) {
            fail(c, "%s: two devices of that name", path);
            goto done;
        }
        if (s->ndev >= RDOF_SETUP_MAX_DEVICES) {
            fail(c, "devices: %d at most", RDOF_SETUP_MAX_DEVICES);
            goto done;
        }
        rdof_device_t *dev = &s->dev[s->ndev];
        memset(dev, 0, sizeof *dev);
        snprintf(dev->name, sizeof dev->name, "%s", name);
        if (!is_type(c, d, RDOF_JOBJ)) {
            fail(c, "%s: an object", path);
            goto done;
        }
        const char *kind = str_of(c, get(c, d, "kind"));
        bool okd;
        if (kind && !strcmp(kind, "sdr")) {
            dev->kind = RDOF_DEV_SDR;
            okd = sdr_device(c, d, path, dev);
        } else if (kind && !strcmp(kind, "modem")) {
            dev->kind = RDOF_DEV_MODEM;
            okd = modem_device(c, d, path, dev, false);
        } else if (kind && !strcmp(kind, "radio")) {
            dev->kind = RDOF_DEV_RADIO;
            okd = modem_device(c, d, path, dev, true);
        } else {
            okd = fail(c, "%s.kind: sdr, modem or radio", path);
        }
        if (!okd)
            goto done;
        s->ndev++;
    }
    if (roles >= 0 && !is_type(c, roles, RDOF_JOBJ)) {
        fail(c, "roles: an object, a role by name");
        goto done;
    }
    for (int k = 0; roles >= 0 && k < N(c, roles)->count; k++) {
        int ro = at(c, roles, k);
        const char *name = N(c, ro)->key;
        if (!name_ok(name)) {
            fail(c, "roles.%s: a name of letters, digits, '-', '_' and '.', under %d characters", name,
                 RDOF_SETUP_NAME);
            goto done;
        }
        if (rdof_setup_role(s, name) >= 0) {
            fail(c, "roles.%s: two roles of that name", name);
            goto done;
        }
        if (s->nrole >= RDOF_SETUP_MAX_ROLES) {
            fail(c, "roles: %d at most", RDOF_SETUP_MAX_ROLES);
            goto done;
        }
        rdof_role_t *r = &s->role[s->nrole];
        memset(r, 0, sizeof *r);
        snprintf(r->name, sizeof r->name, "%s", name);
        if (!parse_role(c, s, plan, ro, r))
            goto done;
        s->nrole++;
    }
    if (!s->nrole) {
        fail(c, "roles: none");
        goto done;
    }
    /* run: names, else every enabled role */
    int run = get(c, box, "run");
    if (run >= 0 && !is_type(c, run, RDOF_JSTR) && !is_type(c, run, RDOF_JARR)) {
        fail(c, "run: a role's name or a list of names");
        goto done;
    }
    if (run < 0) {
        for (int k = 0; k < s->nrole; k++)
            s->role[k].run = s->role[k].enabled;
    } else {
        int n = is_type(c, run, RDOF_JSTR) ? 1 : N(c, run)->count;
        for (int k = 0; k < n; k++) {
            const char *name = str_of(c, is_type(c, run, RDOF_JSTR) ? run : at(c, run, k));
            int i = name ? rdof_setup_role(s, name) : -1;
            if (i < 0) {
                fail(c, "run[%d]: %s isn't in roles", k, name ? name : "(not a name)");
                goto done;
            }
            s->role[i].run = true;
        }
    }
    /* bench */
    int bench = get(c, box, "bench");
    if (!known(c, bench, "bench", BENCH_KEYS) || !known(c, get(c, bench, "tx"), "bench.tx", BENCH_TX_KEYS))
        goto done;
    int bd = get(c, bench, "devices");
    if (bd >= 0 && !is_type(c, bd, RDOF_JARR)) {
        fail(c, "bench.devices: a list of device names");
        goto done;
    }
    for (int k = 0; bd >= 0 && k < N(c, bd)->count; k++) {
        const char *name = str_of(c, at(c, bd, k));
        int i = name ? rdof_setup_device(s, name) : -1;
        if (i < 0) {
            fail(c, "bench.devices[%d]: %s isn't in devices", k, name ? name : "(not a name)");
            goto done;
        }
        for (int m = 0; m < s->nbench; m++)
            if (s->bench[m] == i) {
                fail(c, "bench.devices[%d]: %s twice", k, name);
                goto done;
            }
        const rdof_device_t *dv = &s->dev[i];
        if (dv->kind != RDOF_DEV_SDR && dv->port.kind == RDOF_PORT_USB && !strcmp(dv->port.usb, "auto")) {
            fail(c, "devices.%s.port.usb: a bench device is cdc-acm (with vid and pid) or uart", name);
            goto done;
        }
        s->bench[s->nbench++] = i;
    }
    ok = true;
done:
    free(plan);
    return ok;
}

/* The checks after auto is resolved (rdof_broker_plan). */
static bool check_setup(ctx_t *c, rdof_setup_t *s)
{
    for (int k = 0; k < s->nrole; k++)
        if (!role_devices(c, s, &s->role[k]))
            return false;
    for (int a = 0; a < s->nrole; a++)
        for (int b = a + 1; b < s->nrole; b++)
            if (s->role[a].type == RDOF_ROLE_MONITOR && s->role[b].type == RDOF_ROLE_MONITOR &&
                s->role[a].rx == s->role[b].rx)
                return fail(c, "roles.%s: devices.%s has a monitor already (%s): one monitor an SDR, with all its "
                               "channels", s->role[b].name, s->dev[s->role[b].rx].name, s->role[a].name);
    return true;
}

bool rdof_setup_parse(const char *text, size_t len, rdof_setup_t *s, char *error, size_t cap)
{
    ctx_t *c = open_doc(text, len, error, cap);
    if (!c) {
        memset(s, 0, sizeof *s);
        return false;
    }
    bool ok = parse_setup(c, s);
    if (ok) {
        char why[160];
        ok = rdof_broker_resolve_auto(s, why, sizeof why) || fail(c, "%s", why);
    }
    ok = ok && check_setup(c, s);
    close_doc(c);
    return ok;
}

const char *rdof_setup_sdr_section(const rdof_setup_t *s, int dev)
{
    for (int k = 0; k < s->nrole; k++)
        if (s->role[k].type == RDOF_ROLE_MONITOR && s->role[k].rx == dev)
            return s->role[k].name;
    return s->dev[dev].name;
}

/* ── writing JSON ──────────────────────────────────────────────────────────── */

typedef struct {
    char *s;
    size_t len, cap;
    bool oom;
    int depth;
    bool first[24];
} jw_t;

static void put(jw_t *w, const char *p, size_t n)
{
    if (w->oom)
        return;
    if (w->len + n + 1 > w->cap) {
        size_t cap = w->cap ? w->cap * 2 : 4096;
        while (cap < w->len + n + 1)
            cap *= 2;
        char *s = realloc(w->s, cap);
        if (!s) {
            w->oom = true;
            return;
        }
        w->s = s;
        w->cap = cap;
    }
    memcpy(w->s + w->len, p, n);
    w->len += n;
    w->s[w->len] = 0;
}

static void puts_(jw_t *w, const char *p)
{
    put(w, p, strlen(p));
}

static void put_str(jw_t *w, const char *s)
{
    put(w, "\"", 1);
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        char esc[8];
        if (ch == '"' || ch == '\\') {
            esc[0] = '\\', esc[1] = (char)ch;
            put(w, esc, 2);
        } else if (ch < 0x20) {
            snprintf(esc, sizeof esc, "\\u%04x", ch);
            put(w, esc, 6);
        } else {
            put(w, s, 1);
        }
    }
    put(w, "\"", 1);
}

/* A member's key (in an object) or an element's comma (in an array). */
static void key(jw_t *w, const char *k)
{
    if (w->depth > 0) {
        if (!w->first[w->depth])
            put(w, ",", 1);
        w->first[w->depth] = false;
    }
    if (k) {
        put_str(w, k);
        put(w, ":", 1);
    }
}

static void open_(jw_t *w, const char *k, char brace)
{
    key(w, k);
    put(w, &brace, 1);
    if (w->depth < 23)
        w->first[++w->depth] = true;
}

static void close_(jw_t *w, char brace)
{
    put(w, &brace, 1);
    if (w->depth > 0)
        w->depth--;
}

static void emit_value(jw_t *w, ctx_t *c, int i);

/* A node as JSON. */
static void emit(jw_t *w, ctx_t *c, const char *k, int i)
{
    key(w, k);
    emit_value(w, c, i);
}

static void emit_value(jw_t *w, ctx_t *c, int i)
{
    const rdof_jnode_t *n = N(c, i);
    char t[48];
    switch (n->type) {
    case RDOF_JNULL: puts_(w, "null"); break;
    case RDOF_JBOOL: puts_(w, n->b ? "true" : "false"); break;
    case RDOF_JNUM:
        if (n->integer && fabs(n->num) < 1e15)
            snprintf(t, sizeof t, "%.0f", n->num + 0.0);
        else
            rdof_format_repr(n->num, t, sizeof t);
        puts_(w, t);
        break;
    case RDOF_JSTR: put_str(w, n->str); break;
    case RDOF_JARR:
    case RDOF_JOBJ: {
        bool obj = n->type == RDOF_JOBJ;
        put(w, obj ? "{" : "[", 1);
        if (w->depth < 23)
            w->first[++w->depth] = true;
        for (int k = 0; k < n->count; k++) {
            int ch = at(c, i, k);
            emit(w, c, obj ? N(c, ch)->key : NULL, ch);
        }
        close_(w, obj ? '}' : ']');
        break;
    }
    }
}

static void w_str(jw_t *w, const char *k, const char *v)
{
    key(w, k);
    put_str(w, v);
}

static void w_num(jw_t *w, const char *k, double v)
{
    char t[48];
    if (v == floor(v) && fabs(v) < 1e15)
        snprintf(t, sizeof t, "%.0f", v + 0.0);
    else
        rdof_format_repr(v, t, sizeof t);
    key(w, k);
    puts_(w, t);
}

static void w_raw(jw_t *w, const char *k, const char *v)
{
    key(w, k);
    puts_(w, v);
}

/* Each member of obj whose key is (in) or isn't (out) in keys. */
static void emit_keys(jw_t *w, ctx_t *c, int obj, const char *const *keys, bool in)
{
    for (int k = 0; is_type(c, obj, RDOF_JOBJ) && k < N(c, obj)->count; k++) {
        int m = at(c, obj, k);
        if (in_list(keys, N(c, m)->key) == in)
            emit(w, c, N(c, m)->key, m);
    }
}

/* obj's member key as newkey, if it's there. */
static void emit_as(jw_t *w, ctx_t *c, int obj, const char *key_, const char *newkey)
{
    int i = get(c, obj, key_);
    if (i >= 0)
        emit(w, c, newkey, i);
}

static char *finish(jw_t *w, size_t *out_len, ctx_t *c)
{
    if (w->oom || !w->s) {
        free(w->s);
        fail(c, "no memory");
        return NULL;
    }
    if (out_len)
        *out_len = w->len;
    return w->s;
}

/* ── version 1 -> 2 ────────────────────────────────────────────────────────── */

static const char *const V1_ROAMER_TYPES[] = {"auto", "radio_hotspot", "opengd77", "mmdvm_hs", "mmdvm", NULL};
/* A version 1 roamer's and repeater's own keys (config.c ROAMER_KEYS, REPEATER_KEYS): radio.<key> doesn't reach
   past them. */
static const char *const V1_ROAMER_OWN[] = {"type", "name", "peer_id", "callsign", "idle", "channels", "listen_s",
                                            "clear_s", "prebuffer", "jitter_buffer", "jitter_min_ms",
                                            "jitter_max_ms", "idle_after_s", "cwid", "cwid_every_s", "hblink",
                                            "location", "radio", "scan", "csbk_preambles", "answer_s",
                                            "answer_max_s", "parrot", NULL};
static const char *const V1_REPEATER_OWN[] = {"type", "name", "peer_id", "callsign", "rx_freq", "tx_freq", "cc",
                                              "power", "slots", "hang_s", "call_hang_s", "prebuffer",
                                              "jitter_buffer", "jitter_min_ms", "jitter_max_ms", "cwid",
                                              "cwid_every_s", "hblink", "location", "radio", "mode", "freq",
                                              "inbound_slot", "short_lc_ts1_low", "slot_timing", "parrot", NULL};

typedef struct {
    char name[RDOF_SETUP_NAME * 2];
    char dev[RDOF_SETUP_NAME * 2]; /* an SDR's device */
    int node;
    const char *type;
} v1radio_t;

/* radio.<key>, or the section's own <key> when it isn't one of its own (config.c rget). */
static int v1_rget(ctx_t *c, int r, const char *const *own, const char *key)
{
    if (!in_list(own, key)) {
        int i = get(c, r, key);
        if (i >= 0)
            return i;
    }
    return get(c, get(c, r, "radio"), key);
}

/* A modem's port from a version 1 section (modem / uart / usb / speed, the box's modem / uart behind them). */
static void v1_port(jw_t *w, ctx_t *c, int box, int sec, const char *type, bool box_defaults)
{
    const char *dflt = !strcmp(type, "radio_hotspot") || !strcmp(type, "auto") ? "usb" : "uart";
    const char *m = str_of(c, get(c, sec, "modem"));
    if (!m && box_defaults)
        m = str_of(c, get(c, box, "modem"));
    if (!m)
        m = dflt;
    int u = get(c, sec, "uart");
    if (u < 0 && box_defaults)
        u = get(c, box, "uart");
    open_(w, "port", '{');
    if (!strcmp(m, "uart")) {
        double v;
        w_num(w, "uart", num_of(c, get(c, u, "num"), &v) ? v : 1);
        w_num(w, "tx", num_of(c, get(c, u, "tx"), &v) ? v : 5);
        w_num(w, "rx", num_of(c, get(c, u, "rx"), &v) ? v : 6);
    } else {
        w_str(w, "usb", "auto");
        int usb = get(c, sec, "usb");
        emit_as(w, c, usb, "serial", "serial");
        emit_as(w, c, usb, "port", "usb_port");
    }
    emit_as(w, c, sec, "speed", "baud");
    close_(w, '}');
}

static void v1_mmdvm(jw_t *w, ctx_t *c, int sec, const char *const *own)
{
    bool any = false;
    for (int k = 0; MMDVM_KEYS[k]; k++)
        any |= v1_rget(c, sec, own, MMDVM_KEYS[k]) >= 0;
    if (!any)
        return;
    open_(w, "mmdvm", '{');
    for (int k = 0; MMDVM_KEYS[k]; k++) {
        int i = v1_rget(c, sec, own, MMDVM_KEYS[k]);
        if (i >= 0)
            emit(w, c, MMDVM_KEYS[k], i);
    }
    close_(w, '}');
}

static void unique_name(char *out, size_t cap, const char *want, const v1radio_t *taken, int n, ctx_t *c, int bench)
{
    snprintf(out, cap, "%s", want);
    for (int k = 2;; k++) {
        bool clash = false;
        for (int i = 0; i < n; i++)
            clash |= taken[i].dev != out && !strcmp(taken[i].dev, out);
        int radios = get(c, bench, "radios");
        for (int i = 0; is_type(c, radios, RDOF_JARR) && i < N(c, radios)->count; i++) {
            const char *bn = str_of(c, get(c, at(c, radios, i), "name"));
            clash |= bn && !strcmp(bn, out);
        }
        if (!clash)
            return;
        snprintf(out, cap, "%s-%d", want, k);
    }
}

char *rdof_config_upgrade(const char *text, size_t len, size_t *out_len, char *error, size_t cap)
{
    ctx_t *c = open_doc(text, len, error, cap);
    if (!c)
        return NULL;
    jw_t w = {0};
    char *out = NULL;
    int box = c->root, v = version_of(c);
    if (box < 0) {
        fail(c, "JSON: %s", c->doc.error);
        goto done;
    }
    if (v == 2) {
        size_t n = strlen(c->clean);
        out = malloc(n + 1);
        if (!out) {
            fail(c, "no memory");
            goto done;
        }
        memcpy(out, c->clean, n + 1);
        if (out_len)
            *out_len = n;
        goto done;
    }
    if (v == 0) {
        fail(c, "a plain SDR config (no radios) isn't a box config: it loads as it is");
        goto done;
    }
    if (v < 0) {
        fail(c, N(c, box)->type != RDOF_JOBJ ? "the settings are a JSON object" : "version: 1 or 2");
        goto done;
    }
    /* What version 1 runs, by name, as config.c picks it. */
    rdof_roles_t *roles = malloc(sizeof *roles);
    if (!roles) {
        fail(c, "no memory");
        goto done;
    }
    char why[160];
    if (!rdof_box_roles(text, len, roles, why, sizeof why)) {
        fail(c, "%s", why);
        free(roles);
        goto done;
    }
    int radios = get(c, box, "radios");
    int nr = is_type(c, radios, RDOF_JARR) ? N(c, radios)->count : 0;
    v1radio_t *rs = calloc((size_t)(nr ? nr : 1), sizeof *rs);
    if (!rs) {
        free(roles);
        fail(c, "no memory");
        goto done;
    }
    const char *bn = str_of(c, get(c, box, "name"));
    char box_name[64];
    snprintf(box_name, sizeof box_name, "%s", bn && bn[0] ? bn : "radiofoundry");
    int bench = get(c, box, "bench");
    int sdr_tx = -1; /* the SDR an sdr_transceiver is on: the one run names, else the first enabled */
    for (int k = 0; k < nr; k++) {
        int r = at(c, radios, k);
        rs[k].node = r;
        rs[k].type = str_of(c, get(c, r, "type"));
        if (!rs[k].type)
            rs[k].type = "";
        const char *nm = str_of(c, get(c, r, "name"));
        if (nm)
            snprintf(rs[k].name, sizeof rs[k].name, "%s", nm);
        else
            snprintf(rs[k].name, sizeof rs[k].name, "%s-%s%d", box_name, rs[k].type, k + 1);
    }
    int sdrs = 0;
    for (int k = 0; k < nr; k++) {
        if (strcmp(rs[k].type, "sdr"))
            continue;
        char want[16];
        snprintf(want, sizeof want, sdrs ? "sdr%d" : "sdr", sdrs + 1);
        unique_name(rs[k].dev, sizeof rs[k].dev, want, rs, nr, c, bench);
        sdrs++;
        int en = get(c, rs[k].node, "enabled");
        bool on = !(is_type(c, en, RDOF_JBOOL) && !N(c, en)->b);
        if (roles->has_sdr && !strcmp(roles->sdr, rs[k].name))
            sdr_tx = k;
        else if (sdr_tx < 0 && on && !(roles->has_sdr))
            sdr_tx = k;
    }
    for (int k = 0; sdr_tx < 0 && k < nr; k++)
        if (!strcmp(rs[k].type, "sdr"))
            sdr_tx = k;
    for (int k = 0; k < nr; k++) {
        if (!strcmp(rs[k].type, "sdr") || !strcmp(rs[k].type, "sdr_transceiver"))
            continue;
        const char *rt = !strcmp(rs[k].type, "repeater") ? str_of(c, get(c, get(c, rs[k].node, "radio"), "type"))
                                                        : NULL;
        char want[RDOF_SETUP_NAME * 2];
        snprintf(want, sizeof want, "%s-%s", rs[k].name, rt && !strcmp(rt, "split") ? "tx" : "modem");
        unique_name(rs[k].dev, sizeof rs[k].dev, want, rs, nr, c, bench);
    }

    open_(&w, NULL, '{');
    w_num(&w, "version", 2);
    for (int k = 0; k < N(c, box)->count; k++) {
        int m = at(c, box, k);
        const char *key_ = N(c, m)->key;
        if (in_list(TOP_KEYS, key_) && strcmp(key_, "version") && strcmp(key_, "run") && strcmp(key_, "bench"))
            emit(&w, c, key_, m);
    }
    /* devices */
    open_(&w, "devices", '{');
    for (int k = 0; k < nr; k++) {
        int r = rs[k].node;
        const char *t = rs[k].type;
        if (!strcmp(t, "sdr")) {
            int dv = get(c, r, "device");
            open_(&w, rs[k].dev, '{');
            w_str(&w, "kind", "sdr");
            emit_as(&w, c, dv, "type", "driver");
            bool rx = false;
            static const char *const rxmap[][2] = {{"sample_rate", "rate"}, {"center", "center_hz"},
                                                   {"gain", "gain_db"}, {"agc", "agc"}, {"ppm", "ppm"}};
            for (int i = 0; i < 5; i++)
                rx |= get(c, dv, rxmap[i][0]) >= 0;
            int st = get(c, dv, "settings");
            rx |= get(c, st, "biastee") >= 0 || get(c, st, "iq_bits") >= 0;
            if (rx) {
                open_(&w, "rx", '{');
                for (int i = 0; i < 5; i++)
                    emit_as(&w, c, dv, rxmap[i][0], rxmap[i][1]);
                emit_as(&w, c, st, "biastee", "biastee");
                emit_as(&w, c, st, "iq_bits", "iq_bits");
                close_(&w, '}');
            }
            if (k == sdr_tx) {
                bool any = false;
                for (int i = 0; i < nr; i++)
                    any |= !strcmp(rs[i].type, "sdr_transceiver");
                if (any) {
                    open_(&w, "tx", '{');
                    for (int i = 0; i < nr; i++)
                        if (!strcmp(rs[i].type, "sdr_transceiver") && get(c, rs[i].node, "tx_power_dbm") >= 0) {
                            emit_as(&w, c, rs[i].node, "tx_power_dbm", "power_dbm");
                            break;
                        }
                    close_(&w, '}');
                }
            }
            emit_keys(&w, c, r, SDR_BOARD_KEYS, true);
            close_(&w, '}');
        } else if (!strcmp(t, "repeater") || in_list(V1_ROAMER_TYPES, t)) {
            bool rep = !strcmp(t, "repeater");
            const char *const *own = rep ? V1_REPEATER_OWN : V1_ROAMER_OWN;
            const char *rt = rep ? str_of(c, get(c, get(c, r, "radio"), "type")) : t;
            if (!rt)
                rt = "auto";
            if (!strcmp(rt, "opengd77"))
                rt = "radio_hotspot";
            if (!strcmp(rt, "split")) {
                static const char *const halves[2] = {"tx", "rx"};
                for (int h = 0; h < 2; h++) {
                    int hs = v1_rget(c, r, own, halves[h]);
                    const char *hm = str_of(c, get(c, hs, "modem"));
                    if (hm && !strcmp(hm, "sdr"))
                        continue; /* the SDR's device */
                    const char *ht = str_of(c, get(c, hs, "type"));
                    if (!ht || !strcmp(ht, "opengd77"))
                        ht = ht ? "radio_hotspot" : "auto";
                    char hn[RDOF_SETUP_NAME * 2];
                    if (h == 0) {
                        snprintf(hn, sizeof hn, "%s", rs[k].dev);
                    } else {
                        char want[RDOF_SETUP_NAME * 2];
                        snprintf(want, sizeof want, "%s-rx", rs[k].name);
                        unique_name(hn, sizeof hn, want, rs, nr, c, bench);
                    }
                    open_(&w, hn, '{');
                    w_str(&w, "kind", "modem");
                    w_str(&w, "type", ht);
                    v1_port(&w, c, box, hs, "radio_hotspot", false);
                    w_raw(&w, "needs", "[\"split\"]");
                    v1_mmdvm(&w, c, r, own);
                    close_(&w, '}');
                }
            } else {
                open_(&w, rs[k].dev, '{');
                w_str(&w, "kind", "modem");
                w_str(&w, "type", rt);
                v1_port(&w, c, box, r, rt, true);
                v1_mmdvm(&w, c, r, own);
                close_(&w, '}');
            }
        }
    }
    int bradios = get(c, bench, "radios");
    for (int k = 0; is_type(c, bradios, RDOF_JARR) && k < N(c, bradios)->count; k++) {
        int b = at(c, bradios, k);
        const char *nm = str_of(c, get(c, b, "name"));
        const char *proto = str_of(c, get(c, b, "proto"));
        if (!nm)
            continue;
        open_(&w, nm, '{');
        bool mmdvm = proto && !strcmp(proto, "mmdvm");
        w_str(&w, "kind", mmdvm ? "modem" : "radio");
        emit_as(&w, c, b, "port", "port");
        close_(&w, '}');
    }
    close_(&w, '}');
    /* roles */
    open_(&w, "roles", '{');
    for (int k = 0; k < nr; k++) {
        int r = rs[k].node;
        const char *t = rs[k].type;
        if (!strcmp(t, "sdr")) {
            open_(&w, rs[k].name, '{');
            w_str(&w, "type", "monitor");
            emit_keys(&w, c, r, (const char *const[]){"enabled", NULL}, true);
            open_(&w, "rx", '{');
            w_str(&w, "device", rs[k].dev);
            close_(&w, '}');
            emit_keys(&w, c, r,
                      (const char *const[]){"channels", "receiver_ids", "peer_ids", "callsign", "hblink",
                                            "location", "slot_timing", NULL},
                      true);
            close_(&w, '}');
            continue;
        }
        bool sdrtx = !strcmp(t, "sdr_transceiver"), rep = !strcmp(t, "repeater");
        if (!sdrtx && !rep && !in_list(V1_ROAMER_TYPES, t))
            continue;
        const char *mode = str_of(c, get(c, r, "mode"));
        const char *duplex = str_of(c, get(c, r, "duplex"));
        const char *type;
        if (mode && !strcmp(mode, "dcdm"))
            type = "dcdm";
        else if (sdrtx)
            type = duplex && !strcmp(duplex, "full") ? "repeater" : "roamer";
        else if (rep)
            type = mode && !strcmp(mode, "sfr") ? "sfr" : "repeater";
        else
            type = "roamer";
        static const char *const *const keyset[] = {ROAMER_KEYS, REPEATER_KEYS, SFR_KEYS, DCDM_KEYS};
        int ti = !strcmp(type, "roamer") ? 0 : !strcmp(type, "repeater") ? 1 : !strcmp(type, "sfr") ? 2 : 3;
        open_(&w, rs[k].name, '{');
        w_str(&w, "type", type);
        emit_keys(&w, c, r, (const char *const[]){"enabled", NULL}, true);
        const char *rt = rep ? str_of(c, get(c, get(c, r, "radio"), "type")) : NULL;
        if (sdrtx) {
            open_(&w, "modem", '{');
            w_str(&w, "device", sdr_tx >= 0 ? rs[sdr_tx].dev : "auto");
            close_(&w, '}');
        } else if (rt && !strcmp(rt, "split")) {
            const char *const *own = V1_REPEATER_OWN;
            int rx = v1_rget(c, r, own, "rx");
            const char *rm = str_of(c, get(c, rx, "modem"));
            char rxn[RDOF_SETUP_NAME * 2];
            if (rm && !strcmp(rm, "sdr")) {
                snprintf(rxn, sizeof rxn, "%s", sdr_tx >= 0 ? rs[sdr_tx].dev : "auto");
            } else {
                char want[RDOF_SETUP_NAME * 2];
                snprintf(want, sizeof want, "%s-rx", rs[k].name);
                unique_name(rxn, sizeof rxn, want, rs, nr, c, bench);
            }
            open_(&w, "rx", '{');
            w_str(&w, "device", rxn);
            close_(&w, '}');
            open_(&w, "tx", '{');
            w_str(&w, "device", rs[k].dev);
            close_(&w, '}');
            bool any = false;
            for (int i = 0; SPLIT_KEYS[i]; i++)
                any |= (!strcmp(SPLIT_KEYS[i], "prebuffer") ? get(c, get(c, r, "radio"), "prebuffer")
                                                            : v1_rget(c, r, own, SPLIT_KEYS[i])) >= 0;
            if (any) {
                open_(&w, "split", '{');
                for (int i = 0; SPLIT_KEYS[i]; i++) {
                    int si = !strcmp(SPLIT_KEYS[i], "prebuffer") ? get(c, get(c, r, "radio"), "prebuffer")
                                                                  : v1_rget(c, r, own, SPLIT_KEYS[i]);
                    if (si >= 0)
                        emit(&w, c, SPLIT_KEYS[i], si);
                }
                close_(&w, '}');
            }
        } else {
            open_(&w, "modem", '{');
            w_str(&w, "device", rs[k].dev);
            close_(&w, '}');
        }
        /* the role's own keys; a dcdm on the SDR keeps its duplex */
        for (int i = 0; i < N(c, r)->count; i++) {
            int m = at(c, r, i);
            const char *key_ = N(c, m)->key;
            if (!in_list(keyset[ti], key_) || !strcmp(key_, "type") || !strcmp(key_, "enabled") ||
                !strcmp(key_, "modem") || !strcmp(key_, "rx") || !strcmp(key_, "tx") || !strcmp(key_, "split") ||
                in_list(MMDVM_KEYS, key_))
                continue;
            if (!strcmp(key_, "duplex") && !sdrtx)
                continue;
            emit(&w, c, key_, m);
        }
        close_(&w, '}');
    }
    close_(&w, '}');
    /* run */
    open_(&w, "run", '[');
    int run = get(c, box, "run");
    if (run >= 0) {
        int n = is_type(c, run, RDOF_JSTR) ? 1 : is_type(c, run, RDOF_JARR) ? N(c, run)->count : 0;
        for (int k = 0; k < n; k++)
            emit(&w, c, NULL, is_type(c, run, RDOF_JSTR) ? run : at(c, run, k));
    } else {
        for (int k = 0; k < roles->nradios; k++)
            w_str(&w, NULL, roles->radios[k]);
        if (roles->has_sdr)
            w_str(&w, NULL, roles->sdr);
    }
    close_(&w, ']');
    /* bench */
    if (bench >= 0) {
        if (!is_type(c, bench, RDOF_JOBJ)) {
            emit(&w, c, "bench", bench);
        } else {
            open_(&w, "bench", '{');
            if (is_type(c, bradios, RDOF_JARR)) {
                open_(&w, "devices", '[');
                for (int k = 0; k < N(c, bradios)->count; k++)
                    emit_as(&w, c, at(c, bradios, k), "name", NULL);
                close_(&w, ']');
            }
            emit_as(&w, c, bench, "tx", "tx");
            close_(&w, '}');
        }
    }
    close_(&w, '}');
    free(rs);
    free(roles);
    out = finish(&w, out_len, c);
done:
    close_doc(c);
    return out;
}

/* ── version 2 -> the view (version 1's shape) ─────────────────────────────── */

static void view_port(jw_t *w, const rdof_port_t *p)
{
    char t[24];
    if (p->kind == RDOF_PORT_UART) {
        w_str(w, "modem", "uart");
        open_(w, "uart", '{');
        w_num(w, "num", p->uart);
        w_num(w, "tx", p->tx);
        w_num(w, "rx", p->rx);
        close_(w, '}');
    } else {
        w_str(w, "modem", "usb");
        if (p->serial[0] || p->usb_port[0]) {
            open_(w, "usb", '{');
            if (p->serial[0])
                w_str(w, "serial", p->serial);
            if (p->usb_port[0])
                w_str(w, "port", p->usb_port);
            close_(w, '}');
        }
        if (p->kind == RDOF_PORT_PATH)
            w_str(w, "path", p->path);
    }
    snprintf(t, sizeof t, "%ld", p->baud);
    w_raw(w, "speed", t);
}

/* A device's node in the document. */
static int dev_node(ctx_t *c, const char *name)
{
    return get(c, get(c, c->root, "devices"), name);
}

static int role_node(ctx_t *c, const char *name)
{
    return get(c, get(c, c->root, "roles"), name);
}

/* The synthetic plan numbers: a role's frequency an SDR hears for it that its monitor doesn't. */
typedef struct {
    int number;
    uint32_t freq;
    int cc;
    int dev;
    char id[RDOF_SETUP_NAME + 4];
} synth_t;

static bool heard(const rdof_setup_t *s, int dev, uint32_t f, const synth_t *sy, int nsy)
{
    for (int k = 0; k < s->nrole; k++) {
        const rdof_role_t *r = &s->role[k];
        if (r->type != RDOF_ROLE_MONITOR || r->rx != dev)
            continue;
        for (int i = 0; i < r->nfreqs; i++)
            if (llabs((long long)r->freqs[i] - (long long)f) <= 100)
                return true;
    }
    for (int k = 0; k < nsy; k++)
        if (sy[k].dev == dev && llabs((long long)sy[k].freq - (long long)f) <= 100)
            return true;
    return false;
}

/* Does role r use SDR dev (to hear on, or to transmit)? */
static bool uses(const rdof_role_t *r, int dev)
{
    return r->rx == dev || r->tx == dev;
}

char *rdof_config_view(const char *text, size_t len, const char *focus, size_t *out_len, char *error, size_t cap)
{
    ctx_t *c = open_doc(text, len, error, cap);
    if (!c)
        return NULL;
    char *out = NULL;
    jw_t w = {0};
    rdof_setup_t *s = NULL;
    synth_t *sy = NULL;
    plan_t *plan = NULL;
    int v = version_of(c);
    if (v != 2) {
        if (c->root < 0) {
            fail(c, "JSON: %s", c->doc.error);
        } else if (v < 0) {
            fail(c, N(c, c->root)->type != RDOF_JOBJ ? "the settings are a JSON object" : "version: 1 or 2");
        } else {
            out = malloc(len + 1);
            if (!out) {
                fail(c, "no memory");
            } else {
                memcpy(out, text, len);
                out[len] = 0;
                if (out_len)
                    *out_len = len;
            }
        }
        goto done;
    }
    s = malloc(sizeof *s);
    plan = malloc(sizeof *plan);
    sy = calloc(RDOF_SETUP_MAX_ROLES * RDOF_SETUP_MAX_FREQS, sizeof *sy);
    if (!s || !plan || !sy) {
        fail(c, "no memory");
        goto done;
    }
    if (!parse_setup(c, s))
        goto done;
    {
        char why[160];
        if (!rdof_broker_resolve_auto(s, why, sizeof why)) {
            fail(c, "%s", why);
            goto done;
        }
    }
    if (!check_setup(c, s))
        goto done;
    int fr = focus ? rdof_setup_role(s, focus) : -1;
    int fd = focus && fr < 0 ? rdof_setup_device(s, focus) : -1;
    if (focus && fr < 0 && (fd < 0 || s->dev[fd].kind != RDOF_DEV_SDR)) {
        fail(c, "%s isn't a role (or an SDR) in these settings", focus);
        goto done;
    }
    int box = c->root;
    read_plan(c, box, plan);
    /* every frequency a role on an SDR hears on that its monitor doesn't: a plan channel of its own */
    int nsy = 0, next = plan_max(plan) + 1;
    if (next < 1000)
        next = 1000; /* clear of the plan's own numbers */
    for (int k = 0; k < s->nrole; k++) {
        const rdof_role_t *r = &s->role[k];
        if (r->type == RDOF_ROLE_MONITOR || r->rx < 0 || s->dev[r->rx].kind != RDOF_DEV_SDR)
            continue;
        for (int i = 0; i < r->nfreqs; i++) {
            if (heard(s, r->rx, r->freqs[i], sy, nsy))
                continue;
            synth_t *x = &sy[nsy++];
            x->number = next++;
            x->freq = r->freqs[i];
            x->cc = r->fcc[i] >= 0 ? r->fcc[i] : 1;
            x->dev = r->rx;
            snprintf(x->id, sizeof x->id, "%s-%d", r->name, i + 1);
        }
    }

    open_(&w, NULL, '{');
    for (int k = 0; k < N(c, box)->count; k++) {
        int m = at(c, box, k);
        const char *key_ = N(c, m)->key;
        if (!strcmp(key_, "version") || !strcmp(key_, "devices") || !strcmp(key_, "roles") || !strcmp(key_, "run") ||
            !strcmp(key_, "bench") || !strcmp(key_, "channels"))
            continue;
        emit(&w, c, key_, m);
    }
    /* the plan, and the synthetic channels after it */
    int chans = get(c, box, "channels");
    if (chans >= 0 || nsy) {
        open_(&w, "channels", '{');
        for (int k = 0; is_type(c, chans, RDOF_JOBJ) && k < N(c, chans)->count; k++) {
            int m = at(c, chans, k);
            emit(&w, c, N(c, m)->key, m);
        }
        for (int k = 0; k < nsy; k++) {
            char num_[16];
            snprintf(num_, sizeof num_, "%d", sy[k].number);
            open_(&w, num_, '{');
            w_num(&w, "freq", sy[k].freq);
            w_num(&w, "cc", sy[k].cc);
            close_(&w, '}');
        }
        close_(&w, '}');
    }
    open_(&w, "radios", '[');
    for (int k = 0; k < s->nrole; k++) {
        const rdof_role_t *r = &s->role[k];
        if (r->type == RDOF_ROLE_MONITOR)
            continue;
        int ro = role_node(c, r->name);
        const rdof_device_t *rx = &s->dev[r->rx], *tx = &s->dev[r->tx];
        open_(&w, NULL, '{');
        w_str(&w, "name", r->name);
        bool talker_on_sdr = !r->split && rx->kind == RDOF_DEV_SDR;
        static const char *const SKIP[] = {"type", "rx", "tx", "modem", "split", "duplex", "channels", NULL};
        if (talker_on_sdr) {
            w_str(&w, "type", "sdr_transceiver");
            w_str(&w, "duplex", r->type == RDOF_ROLE_REPEATER ? "full"
                               : r->type == RDOF_ROLE_DCDM && r->sdr_duplex_full ? "full" : "half");
            if (r->type == RDOF_ROLE_DCDM)
                w_str(&w, "mode", "dcdm");
            if (rx->has_tx_power)
                w_num(&w, "tx_power_dbm", rx->tx_power_dbm);
        } else if (r->type == RDOF_ROLE_ROAMER) {
            w_str(&w, "type", rx->driver);
        } else {
            w_str(&w, "type", "repeater");
            if (r->type != RDOF_ROLE_REPEATER)
                w_str(&w, "mode", TYPE_NAMES[r->type]);
        }
        emit_keys(&w, c, ro, SKIP, false);
        /* a roamer's "all" is the plan's own channels, not the synthetic ones */
        int chs = get(c, ro, "channels");
        if (r->type == RDOF_ROLE_ROAMER) {
            if (chs < 0 || (str_of(c, chs) && !strcmp(str_of(c, chs), "all"))) {
                if (nsy) {
                    open_(&w, "channels", '[');
                    for (int i = 0; i < plan->n; i++)
                        if (plan_find(plan, plan->ch[i].number) == &plan->ch[i])
                            w_num(&w, NULL, plan->ch[i].number);
                    close_(&w, ']');
                } else if (chs >= 0) {
                    emit(&w, c, "channels", chs);
                }
            } else {
                emit(&w, c, "channels", chs);
            }
        }
        if (talker_on_sdr)
            goto section_done;
        if (!r->split) {
            int dn = dev_node(c, rx->name);
            view_port(&w, &rx->port);
            int mm = get(c, dn, "mmdvm");
            if (r->type != RDOF_ROLE_ROAMER || mm >= 0) {
                open_(&w, "radio", '{');
                if (r->type != RDOF_ROLE_ROAMER)
                    w_str(&w, "type", rx->driver);
                emit_keys(&w, c, mm, MMDVM_KEYS, true);
                close_(&w, '}');
            }
        } else {
            open_(&w, "radio", '{');
            w_str(&w, "type", "split");
            for (int h = 0; h < 2; h++) {
                const rdof_device_t *dv = h ? rx : tx;
                open_(&w, h ? "rx" : "tx", '{');
                if (dv->kind == RDOF_DEV_SDR) {
                    w_str(&w, "modem", "sdr");
                } else {
                    w_str(&w, "type", dv->driver);
                    view_port(&w, &dv->port);
                }
                close_(&w, '}');
            }
            /* the halves' MMDVM settings: the RX half's, then the TX half's over them */
            for (int h = 0; h < 2; h++) {
                const rdof_device_t *dv = h ? tx : rx;
                if (dv->kind == RDOF_DEV_MODEM)
                    emit_keys(&w, c, get(c, dev_node(c, dv->name), "mmdvm"), MMDVM_KEYS, true);
            }
            emit_keys(&w, c, get(c, ro, "split"), SPLIT_KEYS, true);
            close_(&w, '}');
        }
    section_done:
        close_(&w, '}');
    }
    /* an SDR section for each SDR a role uses */
    for (int d = 0; d < s->ndev; d++) {
        const rdof_device_t *dv = &s->dev[d];
        if (dv->kind != RDOF_DEV_SDR)
            continue;
        int mon = -1;
        bool used = false, on = false;
        for (int k = 0; k < s->nrole; k++) {
            if (!uses(&s->role[k], d))
                continue;
            used = true;
            on |= s->role[k].enabled;
            if (s->role[k].type == RDOF_ROLE_MONITOR)
                mon = k;
        }
        if (!used)
            continue;
        int dn = dev_node(c, dv->name), mn = mon >= 0 ? role_node(c, s->role[mon].name) : -1;
        int rxn = get(c, dn, "rx");
        open_(&w, NULL, '{');
        w_str(&w, "name", rdof_setup_sdr_section(s, d));
        w_str(&w, "type", "sdr");
        if (mon < 0) {
            w_raw(&w, "hblink", "false");
            if (!on)
                w_raw(&w, "enabled", "false");
        }
        emit_keys(&w, c, mn, (const char *const[]){"type", "rx", "channels", NULL}, false);
        open_(&w, "device", '{');
        if (strcmp(dv->driver, "soapy") && strcmp(dv->driver, "iio"))
            emit_as(&w, c, dn, "driver", "type");
        emit_as(&w, c, rxn, "rate", "sample_rate");
        emit_as(&w, c, rxn, "center_hz", "center");
        emit_as(&w, c, rxn, "gain_db", "gain");
        emit_as(&w, c, rxn, "agc", "agc");
        emit_as(&w, c, rxn, "ppm", "ppm");
        if (get(c, rxn, "biastee") >= 0 || get(c, rxn, "iq_bits") >= 0) {
            open_(&w, "settings", '{');
            emit_as(&w, c, rxn, "biastee", "biastee");
            emit_as(&w, c, rxn, "iq_bits", "iq_bits");
            close_(&w, '}');
        }
        close_(&w, '}');
        emit_keys(&w, c, dn, SDR_BOARD_KEYS, true);
        open_(&w, "channels", '[');
        int chs = get(c, mn, "channels");
        for (int k = 0; is_type(c, chs, RDOF_JARR) && k < N(c, chs)->count; k++)
            emit(&w, c, NULL, at(c, chs, k));
        for (int k = 0; k < nsy; k++) {
            if (sy[k].dev != d)
                continue;
            open_(&w, NULL, '{');
            w_num(&w, "ch", sy[k].number);
            w_str(&w, "id", sy[k].id);
            w_raw(&w, "hblink", "false");
            close_(&w, '}');
        }
        close_(&w, ']');
        close_(&w, '}');
    }
    close_(&w, ']');
    /* run: the focus (and its SDR), or every role run names */
    open_(&w, "run", '[');
    for (int k = 0; k < s->nrole; k++) {
        const rdof_role_t *r = &s->role[k];
        bool in = fr >= 0 ? k == fr : fd >= 0 ? false : r->run;
        if (in && r->type != RDOF_ROLE_MONITOR)
            w_str(&w, NULL, r->name);
    }
    for (int d = 0; d < s->ndev; d++) {
        if (s->dev[d].kind != RDOF_DEV_SDR)
            continue;
        bool in = false;
        for (int k = 0; k < s->nrole; k++) {
            const rdof_role_t *r = &s->role[k];
            if (uses(r, d) && (fr >= 0 ? k == fr : fd >= 0 ? d == fd : r->run))
                in = true;
        }
        if (in)
            w_str(&w, NULL, rdof_setup_sdr_section(s, d));
    }
    close_(&w, ']');
    /* the bench: its radios and modems, and what it may transmit (the box's, and each device's) */
    int bench = get(c, box, "bench");
    if (bench >= 0) {
        open_(&w, "bench", '{');
        open_(&w, "radios", '[');
        for (int k = 0; k < s->nbench; k++) {
            const rdof_device_t *dv = &s->dev[s->bench[k]];
            if (dv->kind == RDOF_DEV_SDR)
                continue;
            open_(&w, NULL, '{');
            w_str(&w, "name", dv->name);
            w_str(&w, "proto", dv->kind == RDOF_DEV_RADIO ? "odr-debug" : "mmdvm");
            emit_as(&w, c, dev_node(c, dv->name), "port", "port");
            close_(&w, '}');
        }
        close_(&w, ']');
        int tx = get(c, bench, "tx");
        bool dev_freqs = false;
        for (int k = 0; k < s->nbench; k++)
            dev_freqs |= s->dev[s->bench[k]].bench.nfreqs > 0;
        if (tx >= 0 && !dev_freqs) {
            emit(&w, c, "tx", tx);
        } else if (tx >= 0 || dev_freqs) {
            open_(&w, "tx", '{');
            emit_keys(&w, c, tx, (const char *const[]){"freqs_hz", NULL}, false);
            open_(&w, "freqs_hz", '[');
            uint32_t seen[64];
            int ns = 0;
            int fl = get(c, tx, "freqs_hz");
            for (int k = 0; is_type(c, fl, RDOF_JARR) && k < N(c, fl)->count; k++)
                emit(&w, c, NULL, at(c, fl, k));
            for (int k = 0; is_type(c, fl, RDOF_JARR) && k < N(c, fl)->count && ns < 64; k++) {
                double f;
                if (num_of(c, at(c, fl, k), &f))
                    seen[ns++] = (uint32_t)f;
            }
            for (int k = 0; k < s->nbench; k++) {
                const rdof_dev_bench_t *b = &s->dev[s->bench[k]].bench;
                for (int i = 0; i < b->nfreqs; i++) {
                    bool dup = false;
                    for (int m = 0; m < ns; m++)
                        dup |= seen[m] == b->freqs_hz[i];
                    if (dup || ns >= 64)
                        continue;
                    seen[ns++] = b->freqs_hz[i];
                    w_num(&w, NULL, b->freqs_hz[i]);
                }
            }
            close_(&w, ']');
            close_(&w, '}');
        }
        close_(&w, '}');
    }
    close_(&w, '}');
    out = finish(&w, out_len, c);
    w.s = NULL;
done:
    if (!out)
        free(w.s);
    free(s);
    free(sy);
    free(plan);
    close_doc(c);
    return out;
}

bool rdof_config_as_v1(const char **text, size_t *len, const char *focus, char **owned, char *error, size_t cap)
{
    *owned = NULL;
    if (rdof_config_version(*text, *len) != 2)
        return true;
    size_t n;
    char *v = rdof_config_view(*text, *len, focus, &n, error, cap);
    if (!v)
        return false;
    *owned = v;
    *text = v;
    *len = n;
    return true;
}
