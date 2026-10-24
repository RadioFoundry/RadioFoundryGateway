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

#define MAX_NODES 1024
#define STR_BUF 8192

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
    rdof_jdoc_t doc;
    rdof_jnode_t nodes[MAX_NODES];
    char strs[STR_BUF];
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
    return rdof_jnode(&c->doc, rdof_jget(&c->doc, obj, key));
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

static int radio(ctx_t *c, int node, int i, rdof_bench_radio_t *r)
{
    const rdof_jnode_t *n = rdof_jnode(&c->doc, node);
    if (n->type != RDOF_JOBJ)
        return fail(c, "bench.radios[%d]: an object", i);
    memset(r, 0, sizeof *r);
    r->rts = r->cts = -1;
    if (!str(at(c, node, "name"), r->name, sizeof r->name) || !r->name[0])
        return fail(c, "bench.radios[%d]: a name (up to %d characters)", i, (int)sizeof r->name - 1);
    for (const char *p = r->name; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_')
            return fail(c, "bench radio %s: a name of letters, digits, - and _", r->name);
    if (!str(at(c, node, "proto"), r->proto, sizeof r->proto))
        return fail(c, "bench radio %s: proto odr-debug or mmdvm", r->name);
    if (!r->proto[0])
        strcpy(r->proto, "odr-debug");
    if (strcmp(r->proto, "odr-debug") && strcmp(r->proto, "mmdvm"))
        return fail(c, "bench radio %s: proto %s: odr-debug or mmdvm", r->name, r->proto);
    int port = rdof_jget(&c->doc, node, "port");
    if (port < 0 || rdof_jnode(&c->doc, port)->type != RDOF_JOBJ)
        return fail(c, "bench radio %s: a port {\"uart\": ...} or {\"usb\": ...}", r->name);
    const rdof_jnode_t *uart = at(c, port, "uart"), *usb = at(c, port, "usb");
    if (!!uart == !!usb)
        return fail(c, "bench radio %s: the port is a uart or usb, one of them", r->name);
    long v;
    if (!num(at(c, port, "baud"), false, 115200, &v) || v < 1200 || v > 6000000)
        return fail(c, "bench radio %s: baud 1200 to 6000000", r->name);
    r->baud = v;
    if (uart) {
        r->port = RDOF_BPORT_UART;
        if (!num(uart, false, 0, &v) || (v != 1 && v != 2))
            return fail(c, "bench radio %s: uart 1 or 2 (UART0 is the console)", r->name);
        r->uart = (int)v;
        static const char *const pins[] = {"tx", "rx", "rts", "cts"};
        int *dst[] = {&r->tx, &r->rx, &r->rts, &r->cts};
        for (int k = 0; k < 4; k++) {
            if (!num(at(c, port, pins[k]), false, -1, &v) || v < -1 || v > 48 || (k < 2 && v < 0))
                return fail(c, "bench radio %s: %s, a GPIO 0 to 48%s", r->name, pins[k], k < 2 ? "" : " (or none)");
            *dst[k] = (int)v;
        }
        for (int a = 0; a < 4; a++)
            for (int b = a + 1; b < 4; b++)
                if (*dst[a] >= 0 && *dst[a] == *dst[b])
                    return fail(c, "bench radio %s: %s and %s on the same GPIO", r->name, pins[a], pins[b]);
        return 0;
    }
    char kind[12];
    if (!str(usb, kind, sizeof kind) || (strcmp(kind, "cdc-acm") && strcmp(kind, "uart")))
        return fail(c, "bench radio %s: usb cdc-acm or uart", r->name);
    r->port = strcmp(kind, "uart") ? RDOF_BPORT_CDC_ACM : RDOF_BPORT_USB_UART;
    if (!num(at(c, port, "vid"), true, 0, &v) || v < 0 || v > 0xFFFF)
        return fail(c, "bench radio %s: vid, four hex digits", r->name);
    r->vid = (uint16_t)v;
    if (!num(at(c, port, "pid"), true, 0, &v) || v < 0 || v > 0xFFFF)
        return fail(c, "bench radio %s: pid, four hex digits", r->name);
    r->pid = (uint16_t)v;
    if (r->pid && !r->vid)
        return fail(c, "bench radio %s: a pid needs its vid", r->name);
    if (r->port == RDOF_BPORT_CDC_ACM && (!r->vid || !r->pid))
        return fail(c, "bench radio %s: a cdc-acm radio needs its vid and pid", r->name);
    if (r->vid == 0x0BDA)
        return fail(c, "bench radio %s: %04X is the RTL-SDR's vendor", r->name, r->vid);
    if (r->port == RDOF_BPORT_USB_UART && r->vid && r->pid && !rdof_bench_usb_bridge(r->vid, r->pid))
        return fail(c, "bench radio %s: %04X:%04X isn't a USB-serial bridge this firmware knows (FTDI, CP210x, CH34x)",
                    r->name, r->vid, r->pid);
    if (!str(at(c, port, "serial"), r->serial, sizeof r->serial) ||
        !str(at(c, port, "usb_port"), r->usb_port, sizeof r->usb_port))
        return fail(c, "bench radio %s: serial and usb_port are strings", r->name);
    return 0;
}

int rdof_bench_radios_parse(const char *text, size_t len, rdof_bench_radio_t *out, int max, char *error, size_t cap)
{
    ctx_t *c = calloc(1, sizeof *c);
    char *clean = malloc(len + 1);
    char dummy[8];
    if (!error) {
        error = dummy;
        cap = sizeof dummy;
    }
    error[0] = 0;
    if (!c || !clean) {
        free(c);
        free(clean);
        snprintf(error, cap, "no memory");
        return -1;
    }
    c->error = error;
    c->cap = cap;
    c->doc = (rdof_jdoc_t){.nodes = c->nodes, .max = MAX_NODES, .buf = c->strs, .cap = STR_BUF};
    int n = -1, root = rdof_jparse(&c->doc, clean, rdof_jsonc_strip(text, len, clean));
    if (root < 0) {
        fail(c, "JSON: %s", c->doc.error);
        goto done;
    }
    int bench = rdof_jget(&c->doc, root, "bench");
    int radios = bench >= 0 ? rdof_jget(&c->doc, bench, "radios") : -1;
    const rdof_jnode_t *rn = rdof_jnode(&c->doc, radios);
    if (!rn || rn->type == RDOF_JNULL) {
        n = 0;
        goto done;
    }
    if (rn->type != RDOF_JARR) {
        fail(c, "bench.radios: a list");
        goto done;
    }
    if (rn->count > max) {
        fail(c, "bench.radios: at most %d", max);
        goto done;
    }
    for (int i = 0; i < rn->count; i++) {
        if (radio(c, rdof_jat(&c->doc, radios, i), i, &out[i]) < 0)
            goto done;
        for (int k = 0; k < i; k++) {
            if (!strcmp(out[k].name, out[i].name)) {
                fail(c, "bench radios: two named %s", out[i].name);
                goto done;
            }
            if (out[k].port == RDOF_BPORT_UART && out[i].port == RDOF_BPORT_UART && out[k].uart == out[i].uart) {
                fail(c, "bench radios %s and %s: both on UART%d", out[k].name, out[i].name, out[i].uart);
                goto done;
            }
        }
    }
    n = rn->count;
done:
    free(clean);
    free(c);
    return n;
}

int rdof_bench_tx_parse(const char *text, size_t len, rdof_benchtx_policy_t *out, char *error, size_t cap)
{
    memset(out, 0, sizeof *out);
    out->arm_gpio = -1;
    ctx_t *c = calloc(1, sizeof *c);
    char *clean = malloc(len + 1);
    char dummy[8];
    if (!error) {
        error = dummy;
        cap = sizeof dummy;
    }
    error[0] = 0;
    if (!c || !clean) {
        free(c);
        free(clean);
        snprintf(error, cap, "no memory");
        return -1;
    }
    c->error = error;
    c->cap = cap;
    c->doc = (rdof_jdoc_t){.nodes = c->nodes, .max = MAX_NODES, .buf = c->strs, .cap = STR_BUF};
    int rc = -1, root = rdof_jparse(&c->doc, clean, rdof_jsonc_strip(text, len, clean));
    if (root < 0) {
        fail(c, "JSON: %s", c->doc.error);
        goto done;
    }
    int bench = rdof_jget(&c->doc, root, "bench");
    int tx = bench >= 0 ? rdof_jget(&c->doc, bench, "tx") : -1;
    const rdof_jnode_t *t = rdof_jnode(&c->doc, tx);
    if (!t || t->type == RDOF_JNULL) {
        rc = 0;
        goto done;
    }
    if (t->type != RDOF_JOBJ) {
        fail(c, "bench.tx: an object");
        goto done;
    }
    int fl = rdof_jget(&c->doc, tx, "freqs_hz");
    const rdof_jnode_t *f = rdof_jnode(&c->doc, fl);
    if (f && f->type == RDOF_JNULL)
        f = NULL; /* none, as no freqs_hz */
    if (f && f->type != RDOF_JARR) {
        fail(c, "bench.tx.freqs_hz: a list of frequencies in Hz");
        goto done;
    }
    for (int i = 0; f && i < f->count; i++) {
        const rdof_jnode_t *v = rdof_jnode(&c->doc, rdof_jat(&c->doc, fl, i));
        if (i >= RDOF_BENCHTX_MAX_FREQS) {
            fail(c, "bench.tx.freqs_hz: at most %d", RDOF_BENCHTX_MAX_FREQS);
            goto done;
        }
        if (v->type != RDOF_JNUM || v->num < 30e6 || v->num > 3e9 || v->num != (double)(uint32_t)v->num) {
            fail(c, "bench.tx.freqs_hz[%d]: a frequency in whole Hz", i);
            goto done;
        }
        out->freqs_hz[out->nfreqs++] = (uint32_t)v->num;
    }
    const rdof_jnode_t *k = at(c, tx, "max_key_s");
    if (k && k->type == RDOF_JNULL)
        k = NULL; /* the default, as null is for the others */
    if (k && (k->type != RDOF_JNUM || k->num < 0 || k->num > 60)) {
        fail(c, "bench.tx.max_key_s: 0 to 60 seconds");
        goto done;
    }
    out->max_key_s = k ? k->num : 10;
    long v;
    if (!num(at(c, tx, "rf_level"), false, 0, &v) || v < 0 || v > 255) {
        fail(c, "bench.tx.rf_level: 0 to 255 (0: the lowest)");
        goto done;
    }
    out->rf_level = (int)v;
    if (!num(at(c, tx, "arm_gpio"), false, -1, &v) || v < -1 || v > 48) {
        fail(c, "bench.tx.arm_gpio: a GPIO 0 to 48 (a jumper to ground), or none");
        goto done;
    }
    out->arm_gpio = (int)v;
    rc = 0;
done:
    if (rc < 0)
        *out = (rdof_benchtx_policy_t){.arm_gpio = -1}; /* no TX, not half a policy */
    free(clean);
    free(c);
    return rc;
}
