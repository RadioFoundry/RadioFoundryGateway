// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The bench API's radios and TX policy from the settings (rdof/benchcfg.h). */
#include "rdof/benchcfg.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/jsonp.h"
#include "rdof/siteplan.h"


char rdof_bench_usb_bridge(uint16_t vid, uint16_t pid)
{
    if (vid == 0x0403)
        return 'f'; /* FTDI FT232R, FT2232, FT4232, FT232H, FT-X */
    if (vid == 0x10C4 && (pid == 0xEA60 || pid == 0xEA70 || pid == 0xEA71))
        return 'c'; /* Silicon Labs CP210x */
    if (vid == 0x1A86 && (pid == 0x7523 || pid == 0x7522 || pid == 0x5523))
        return 'h'; /* WCH CH340 / CH341 (the CH343 and CH9102 are CDC-ACM) */
    return 0;
}

typedef struct {
    const rdof_jdoc_t *doc;
    char *error;
    size_t cap;
} ctx_t;

static int fail(ctx_t *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->error, c->cap, fmt, ap);
    va_end(ap);
    return -1;
}

static const rdof_jnode_t *at(ctx_t *c, int obj, const char *key)
{
    return rdof_jnode(c->doc, rdof_jget(c->doc, obj, key));
}

/* A number, or a hex string for vid / pid; dflt when absent. false: there but not one. */
static bool num(const rdof_jnode_t *n, bool hex, long dflt, long *out)
{
    if (!n || n->type == RDOF_JNULL) {
        *out = dflt;
        return true;
    }
    if (n->type == RDOF_JNUM && n->integer) {
        if (n->num < -2147483648.0 || n->num > 2147483647.0)
            return false; /* past any value here, and (long) of it undefined where long is 32 bits */
        *out = (long)n->num;
        return true;
    }
    if (n->type == RDOF_JSTR && n->str[0]) {
        char *end;
        long v = strtol(n->str, &end, hex ? 16 : 10);
        if (*end)
            return false;
        *out = v;
        return true;
    }
    return false;
}

static bool str(const rdof_jnode_t *n, char *out, size_t cap)
{
    out[0] = 0;
    if (!n || n->type == RDOF_JNULL)
        return true;
    if (n->type != RDOF_JSTR || strlen(n->str) >= cap)
        return false;
    strcpy(out, n->str);
    return true;
}

/* r's port (its name and proto set). */
static int radio_port(ctx_t *c, int port, rdof_bench_radio_t *r)
{
    if (port < 0 || rdof_jnode(c->doc, port)->type != RDOF_JOBJ)
        return fail(c, "devices.%s.port: a port {\"uart\": ...} or {\"usb\": ...}", r->name);
    const rdof_jnode_t *uart = at(c, port, "uart"), *usb = at(c, port, "usb");
    if (!!uart == !!usb)
        return fail(c, "devices.%s.port: the port is a uart or usb, one of them", r->name);
    long v;
    if (!num(at(c, port, "baud"), false, 115200, &v) || v < 1200 || v > 6000000)
        return fail(c, "devices.%s.port: baud 1200 to 6000000", r->name);
    r->baud = v;
    if (uart) {
        r->port = RDOF_BPORT_UART;
        if (!num(uart, false, 0, &v) || (v != 1 && v != 2))
            return fail(c, "devices.%s.port: uart 1 or 2 (UART0 is the console)", r->name);
        r->uart = (int)v;
        static const char *const pins[] = {"tx", "rx", "rts", "cts"};
        int *dst[] = {&r->tx, &r->rx, &r->rts, &r->cts};
        for (int k = 0; k < 4; k++) {
            if (!num(at(c, port, pins[k]), false, -1, &v) || v < -1 || v > 48 || (k < 2 && v < 0))
                return fail(c, "devices.%s.port: %s, a GPIO 0 to 48%s", r->name, pins[k], k < 2 ? "" : " (or none)");
            *dst[k] = (int)v;
        }
        for (int a = 0; a < 4; a++)
            for (int b = a + 1; b < 4; b++)
                if (*dst[a] >= 0 && *dst[a] == *dst[b])
                    return fail(c, "devices.%s.port: %s and %s on the same GPIO", r->name, pins[a], pins[b]);
        return 0;
    }
    char kind[12];
    if (!str(usb, kind, sizeof kind) || (strcmp(kind, "cdc-acm") && strcmp(kind, "uart")))
        return fail(c, "devices.%s.port: usb cdc-acm or uart", r->name);
    r->port = strcmp(kind, "uart") ? RDOF_BPORT_CDC_ACM : RDOF_BPORT_USB_UART;
    if (!num(at(c, port, "vid"), true, 0, &v) || v < 0 || v > 0xFFFF)
        return fail(c, "devices.%s.port: vid, four hex digits", r->name);
    r->vid = (uint16_t)v;
    if (!num(at(c, port, "pid"), true, 0, &v) || v < 0 || v > 0xFFFF)
        return fail(c, "devices.%s.port: pid, four hex digits", r->name);
    r->pid = (uint16_t)v;
    if (r->pid && !r->vid)
        return fail(c, "devices.%s.port: a pid needs its vid", r->name);
    if (r->port == RDOF_BPORT_CDC_ACM && (!r->vid || !r->pid))
        return fail(c, "devices.%s.port: a cdc-acm radio needs its vid and pid", r->name);
    if (r->vid == 0x0BDA)
        return fail(c, "devices.%s.port: %04X is the RTL-SDR's vendor", r->name, r->vid);
    if (r->port == RDOF_BPORT_USB_UART && r->vid && r->pid && !rdof_bench_usb_bridge(r->vid, r->pid))
        return fail(c, "devices.%s.port: %04X:%04X isn't a USB-serial bridge this firmware knows (FTDI, CP210x, CH34x)",
                    r->name, r->vid, r->pid);
    if (!str(at(c, port, "serial"), r->serial, sizeof r->serial) ||
        !str(at(c, port, "usb_port"), r->usb_port, sizeof r->usb_port))
        return fail(c, "devices.%s.port: serial and usb_port are strings", r->name);
    return 0;
}

/* Two radios with one name, or on one UART: refused (out[0..n-1] checked, out[n] new). */
static int radio_apart(ctx_t *c, const rdof_bench_radio_t *out, int n)
{
    for (int k = 0; k < n; k++) {
        if (!strcmp(out[k].name, out[n].name))
            return fail(c, "bench radios: two named %s", out[n].name);
        if (out[k].port == RDOF_BPORT_UART && out[n].port == RDOF_BPORT_UART && out[k].uart == out[n].uart)
            return fail(c, "bench radios %s and %s: both on UART%d", out[k].name, out[n].name, out[n].uart);
    }
    return 0;
}

int rdof_plan_bench_radios(const rdof_plan_t *p, rdof_bench_radio_t *out, int max, char *error, size_t cap)
{
    char dummy[8];
    if (!error) {
        error = dummy;
        cap = sizeof dummy;
    }
    error[0] = 0;
    ctx_t cx = {.doc = rdof_plan_doc(p), .error = error, .cap = cap}, *c = &cx;
    const rdof_site_t *s = rdof_plan_site(p);
    int n = 0;
    for (int k = 0; k < rdof_plan_nbench(p); k++) {
        int d = rdof_plan_bench(p, k);
        if (s->dev[d].kind == RDOF_DEV_SDR)
            continue;
        if (n >= max)
            return fail(c, "bench.devices: %d radios and modems at most", max);
        rdof_bench_radio_t *r = &out[n];
        memset(r, 0, sizeof *r);
        r->rts = r->cts = -1;
        if (strlen(s->dev[d].name) >= sizeof r->name)
            return fail(c, "bench.devices[%d]: a name up to %d characters", k, (int)sizeof r->name - 1);
        strcpy(r->name, s->dev[d].name);
        for (const char *q = r->name; *q; q++)
            if (!isalnum((unsigned char)*q) && *q != '-' && *q != '_')
                return fail(c, "bench.devices[%d]: %s: a name of letters, digits, - and _", k, r->name);
        strcpy(r->proto, s->dev[d].kind == RDOF_DEV_RADIO ? "odr-debug" : "mmdvm");
        if (rdof_plan_dev(p, d)->port.kind == RDOF_PORT_PATH)
            return fail(c, "devices.%s.port: a bench device is on a UART or USB, not a path", r->name);
        if (radio_port(c, rdof_jget(c->doc, rdof_plan_dev_node(p, d), "port"), r) < 0 || radio_apart(c, out, n) < 0)
            return -1;
        n++;
    }
    return n;
}

/* bench.tx: tx's freqs_hz, then more (each not there already), and its other keys. */
static int policy(ctx_t *c, int tx, const uint32_t *more, int nmore, rdof_benchtx_policy_t *out)
{
    const rdof_jnode_t *t = rdof_jnode(c->doc, tx);
    if (t && t->type == RDOF_JNULL)
        t = NULL;
    if (!t && !nmore)
        return 0;
    if (t && t->type != RDOF_JOBJ)
        return fail(c, "bench.tx: an object");
    int fl = t ? rdof_jget(c->doc, tx, "freqs_hz") : -1;
    const rdof_jnode_t *f = rdof_jnode(c->doc, fl);
    if (f && f->type == RDOF_JNULL)
        f = NULL; /* none, as no freqs_hz */
    if (f && f->type != RDOF_JARR)
        return fail(c, "bench.tx.freqs_hz: a list of frequencies in Hz");
    for (int i = 0; f && i < f->count; i++) {
        const rdof_jnode_t *v = rdof_jnode(c->doc, rdof_jat(c->doc, fl, i));
        if (i >= RDOF_BENCHTX_MAX_FREQS)
            return fail(c, "bench.tx.freqs_hz: at most %d", RDOF_BENCHTX_MAX_FREQS);
        if (v->type != RDOF_JNUM || v->num < 30e6 || v->num > 3e9 || v->num != (double)(uint32_t)v->num)
            return fail(c, "bench.tx.freqs_hz[%d]: a frequency in whole Hz", i);
        out->freqs_hz[out->nfreqs++] = (uint32_t)v->num;
    }
    for (int i = 0; i < nmore; i++) {
        bool dup = false;
        for (int k = 0; k < out->nfreqs; k++)
            dup |= out->freqs_hz[k] == more[i];
        if (dup)
            continue;
        if (out->nfreqs >= RDOF_BENCHTX_MAX_FREQS)
            return fail(c, "bench.tx.freqs_hz and the bench devices' freqs_hz: at most %d", RDOF_BENCHTX_MAX_FREQS);
        out->freqs_hz[out->nfreqs++] = more[i];
    }
    const rdof_jnode_t *k = t ? at(c, tx, "max_key_s") : NULL;
    if (k && k->type == RDOF_JNULL)
        k = NULL; /* the default, as null is for the others */
    if (k && (k->type != RDOF_JNUM || k->num < 0 || k->num > 60))
        return fail(c, "bench.tx.max_key_s: 0 to 60 seconds");
    out->max_key_s = k ? k->num : 10;
    long v;
    if (!num(t ? at(c, tx, "rf_level") : NULL, false, 0, &v) || v < 0 || v > 255)
        return fail(c, "bench.tx.rf_level: 0 to 255 (0: the lowest)");
    out->rf_level = (int)v;
    if (!num(t ? at(c, tx, "arm_gpio") : NULL, false, -1, &v) || v < -1 || v > 48)
        return fail(c, "bench.tx.arm_gpio: a GPIO 0 to 48 (a jumper to ground), or none");
    out->arm_gpio = (int)v;
    return 0;
}

int rdof_plan_bench_tx(const rdof_plan_t *p, rdof_benchtx_policy_t *out, char *error, size_t cap)
{
    memset(out, 0, sizeof *out);
    out->arm_gpio = -1;
    char dummy[8];
    if (!error) {
        error = dummy;
        cap = sizeof dummy;
    }
    error[0] = 0;
    ctx_t c = {.doc = rdof_plan_doc(p), .error = error, .cap = cap};
    int bench = rdof_jget(c.doc, rdof_plan_root(p), "bench");
    if (bench < 0)
        return 0;
    /* the bench devices' own frequencies, after bench.tx's */
    uint32_t more[RDOF_SITE_MAX_DEVICES * 8];
    int nmore = 0;
    for (int k = 0; k < rdof_plan_nbench(p); k++) {
        const rdof_dev_bench_t *b = &rdof_plan_dev(p, rdof_plan_bench(p, k))->bench;
        for (int i = 0; i < b->nfreqs; i++)
            more[nmore++] = b->freqs_hz[i];
    }
    int rc = policy(&c, rdof_jget(c.doc, bench, "tx"), more, nmore, out);
    if (rc < 0)
        *out = (rdof_benchtx_policy_t){.arm_gpio = -1}; /* no TX, not half a policy */
    return rc;
}

/* The text forms: a plan each. */
int rdof_bench_radios_parse(const char *text, size_t len, rdof_bench_radio_t *out, int max, char *error, size_t cap)
{
    rdof_plan_t *p = rdof_plan_open(text, len, error, cap);
    if (!p)
        return -1;
    int n = rdof_plan_bench_radios(p, out, max, error, cap);
    rdof_plan_close(p);
    return n;
}

int rdof_bench_tx_parse(const char *text, size_t len, rdof_benchtx_policy_t *out, char *error, size_t cap)
{
    rdof_plan_t *p = rdof_plan_open(text, len, error, cap);
    if (!p) {
        *out = (rdof_benchtx_policy_t){.arm_gpio = -1};
        return -1;
    }
    int rc = rdof_plan_bench_tx(p, out, error, cap);
    rdof_plan_close(p);
    return rc;
}
