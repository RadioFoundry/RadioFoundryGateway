// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The IP gateway's settings ("ip_gateway") and its address helpers (rdof/ipgw.h). */
#include "rdof/ipgw.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/jsonp.h"

#define MAX_NODES 1200 /* the whole settings document: as config.c */
#define STR_BUF 8192
#define MAX_ID 16777215u

/* The private and shared ranges, and 13.0.0.0/8 (the dispatch side of the 12/13 convention): NAT is for the
 * Internet. A "deny" in the settings replaces these. */
static const char *const DEFAULT_DENY[] = {"10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16", "100.64.0.0/10",
                                           "13.0.0.0/8"};

/* ── addresses ─────────────────────────────────────────────────────────────── */

bool rdof_ipgw_parse_ip(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        if (!isdigit((unsigned char)*s))
            return false;
        int n = 0, digits = 0;
        while (isdigit((unsigned char)*s)) {
            n = n * 10 + (*s++ - '0');
            if (++digits > 3 || n > 255)
                return false;
        }
        v = (v << 8) | (uint32_t)n;
        if (i < 3 && *s++ != '.')
            return false;
    }
    if (*s)
        return false;
    *out = v;
    return true;
}

bool rdof_ipgw_parse_cidr(const char *s, rdof_ipgw_net_t *out)
{
    char ip[20];
    const char *slash = strchr(s, '/');
    size_t n = slash ? (size_t)(slash - s) : strlen(s);
    if (n >= sizeof ip)
        return false;
    memcpy(ip, s, n);
    ip[n] = 0;
    uint32_t a;
    if (!rdof_ipgw_parse_ip(ip, &a))
        return false;
    int bits = 32;
    if (slash) {
        const char *p = slash + 1;
        if (!*p || strlen(p) > 2)
            return false;
        bits = 0;
        for (; *p; p++) {
            if (!isdigit((unsigned char)*p))
                return false;
            bits = bits * 10 + (*p - '0');
        }
        if (bits > 32)
            return false;
    }
    uint32_t mask = bits ? 0xFFFFFFFFu << (32 - bits) : 0;
    if (a & ~mask)
        return false; /* host bits set: a typo more often than not */
    out->net = a;
    out->mask = mask;
    return true;
}

bool rdof_ipgw_in_net(const rdof_ipgw_net_t *n, uint32_t ip)
{
    return (ip & n->mask) == n->net;
}

uint32_t rdof_ipgw_radio_ip(const rdof_ipgw_net_t *net, uint32_t id)
{
    return net->net | (id & ~net->mask);
}

bool rdof_ipgw_ip_radio(const rdof_ipgw_net_t *net, uint32_t ip, uint32_t *id)
{
    if (!rdof_ipgw_in_net(net, ip) || !(ip & ~net->mask))
        return false;
    *id = ip & ~net->mask & 0xFFFFFF;
    return *id != 0;
}

bool rdof_ipgw_reserved(const rdof_ipgw_config_t *cfg, uint32_t ip)
{
    static const rdof_ipgw_net_t builtin[] = {
        {0x00000000u, 0xFF000000u}, /* 0.0.0.0/8 */
        {0x7F000000u, 0xFF000000u}, /* 127.0.0.0/8 */
        {0xA9FE0000u, 0xFFFF0000u}, /* 169.254.0.0/16 */
        {0xE0000000u, 0xF0000000u}, /* 224.0.0.0/4 */
        {0xF0000000u, 0xF0000000u}, /* 240.0.0.0/4, 255.255.255.255 in it */
    };
    for (size_t i = 0; i < sizeof builtin / sizeof builtin[0]; i++)
        if (rdof_ipgw_in_net(&builtin[i], ip))
            return true;
    return rdof_ipgw_in_net(&cfg->radio_net, ip);
}

bool rdof_ipgw_nat_allowed(const rdof_ipgw_config_t *cfg, uint32_t ip)
{
    if (rdof_ipgw_reserved(cfg, ip))
        return false;
    for (int i = 0; i < cfg->ndeny; i++)
        if (rdof_ipgw_in_net(&cfg->deny[i], ip))
            return false;
    if (!cfg->nallow)
        return true;
    for (int i = 0; i < cfg->nallow; i++)
        if (rdof_ipgw_in_net(&cfg->allow[i], ip))
            return true;
    return false;
}

void rdof_ipgw_config_default(rdof_ipgw_config_t *c)
{
    memset(c, 0, sizeof *c);
    rdof_ipgw_parse_cidr("12.0.0.0/8", &c->radio_net);
    c->ack = true;
    c->max_bytes = 1500;
    c->up_pps = 10;
    c->down_pps = 2;
    c->idle_s = 120;
    c->max_flows = 8;
    for (size_t i = 0; i < sizeof DEFAULT_DENY / sizeof DEFAULT_DENY[0]; i++)
        rdof_ipgw_parse_cidr(DEFAULT_DENY[i], &c->deny[c->ndeny++]);
}

/* ── the settings ──────────────────────────────────────────────────────────── */

typedef struct {
    rdof_jdoc_t doc;
    rdof_jnode_t nodes[MAX_NODES];
    char strs[STR_BUF];
    char *error;
    size_t cap;
} ctx_t;

static bool fail(ctx_t *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = snprintf(c->error, c->cap, "ip_gateway: ");
    if (n > 0 && (size_t)n < c->cap)
        vsnprintf(c->error + n, c->cap - (size_t)n, fmt, ap);
    va_end(ap);
    return false;
}

static const rdof_jnode_t *N(ctx_t *c, int i)
{
    return rdof_jnode(&c->doc, i);
}

static bool only_keys(ctx_t *c, int obj, const char *where, const char *const *keys)
{
    for (int i = 0; i < N(c, obj)->count; i++) {
        const char *k = N(c, rdof_jat(&c->doc, obj, i))->key;
        bool known = false;
        for (const char *const *p = keys; *p && !known; p++)
            known = !strcmp(*p, k);
        if (!known)
            return fail(c, "%s%sunknown key \"%s\"", where, where[0] ? ": " : "", k);
    }
    return true;
}

/* An optional bool; absent or null: dflt. */
static bool boolean(ctx_t *c, int obj, const char *key, const char *where, bool dflt, bool *out)
{
    const rdof_jnode_t *n = N(c, rdof_jget(&c->doc, obj, key));
    *out = dflt;
    if (!n || n->type == RDOF_JNULL)
        return true;
    if (n->type != RDOF_JBOOL)
        return fail(c, "%s%s%s: true or false", where, where[0] ? "." : "", key);
    *out = n->b;
    return true;
}

static bool number(ctx_t *c, int obj, const char *key, double lo, double hi, bool whole, double *out)
{
    const rdof_jnode_t *n = N(c, rdof_jget(&c->doc, obj, key));
    if (!n || n->type == RDOF_JNULL)
        return true;
    if (n->type != RDOF_JNUM || (whole && !n->integer) || n->num < lo || n->num > hi)
        return fail(c, "%s: %s %g to %g", key, whole ? "a whole number" : "a number", lo, hi);
    *out = n->num;
    return true;
}

/* A DMR id, or "lo-hi". */
static bool id_range(ctx_t *c, const rdof_jnode_t *n, const char *what, uint32_t *lo, uint32_t *hi)
{
    if (n->type == RDOF_JNUM && n->integer && n->num >= 1 && n->num <= MAX_ID) {
        *lo = *hi = (uint32_t)n->num;
        return true;
    }
    if (n->type == RDOF_JSTR) {
        char *end;
        unsigned long a = strtoul(n->str, &end, 10), b = a;
        if (end != n->str && *end == '-') {
            const char *p = end + 1;
            b = strtoul(p, &end, 10);
            if (end == p)
                b = 0;
        }
        if (end != n->str && !*end && a >= 1 && b >= a && b <= MAX_ID && isdigit((unsigned char)n->str[0])) {
            *lo = (uint32_t)a;
            *hi = (uint32_t)b;
            return true;
        }
    }
    return fail(c, "%s: a DMR id 1 to %u, or a range \"lo-hi\"", what, MAX_ID);
}

static bool host_ok(const char *h)
{
    if (!*h)
        return false;
    for (const char *p = h; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '-')
            return false;
    return true;
}

static bool route(ctx_t *c, int node, int i, rdof_ipgw_route_t *r)
{
    static const char *const keys[] = {"name", "all", "private", "group", "from", "to", "payload", NULL};
    char where[32];
    snprintf(where, sizeof where, "routes[%d]", i);
    if (N(c, node)->type != RDOF_JOBJ)
        return fail(c, "%s: an object", where);
    if (!only_keys(c, node, where, keys))
        return false;
    memset(r, 0, sizeof *r);
    const rdof_jnode_t *n = N(c, rdof_jget(&c->doc, node, "name"));
    if (n && n->type != RDOF_JNULL) {
        if (n->type != RDOF_JSTR || strlen(n->str) >= sizeof r->name)
            return fail(c, "%s: name, up to %d characters", where, (int)sizeof r->name - 1);
        strcpy(r->name, n->str);
    }
    if (!r->name[0])
        snprintf(r->name, sizeof r->name, "route%d", i + 1);
    if (!boolean(c, node, "all", where, false, &r->all))
        return false;
    const rdof_jnode_t *pv = N(c, rdof_jget(&c->doc, node, "private")), *gr = N(c, rdof_jget(&c->doc, node, "group")),
                       *fr = N(c, rdof_jget(&c->doc, node, "from"));
    char what[48];
    if (pv && gr)
        return fail(c, "%s: private or group, not both", where);
    if (pv || gr) {
        snprintf(what, sizeof what, "%s.%s", where, pv ? "private" : "group");
        if (!id_range(c, pv ? pv : gr, what, &r->dst_lo, &r->dst_hi))
            return false;
        r->dst_kind = pv ? RDOF_IPGW_DST_PRIVATE : RDOF_IPGW_DST_GROUP;
    }
    if (fr) {
        snprintf(what, sizeof what, "%s.from", where);
        if (!id_range(c, fr, what, &r->from_lo, &r->from_hi))
            return false;
        r->has_from = true;
    }
    if (r->all && (pv || gr || fr))
        return fail(c, "%s: all, or private / group / from: not both", where);
    if (!r->all && !pv && !gr && !fr)
        return fail(c, "%s: which traffic: all: true, private, group or from", where);
    n = N(c, rdof_jget(&c->doc, node, "payload"));
    if (n && n->type != RDOF_JNULL) {
        if (n->type != RDOF_JSTR || (strcmp(n->str, "udp") && strcmp(n->str, "ip")))
            return fail(c, "%s: payload udp or ip", where);
        r->payload = strcmp(n->str, "ip") ? RDOF_IPGW_PAYLOAD_UDP : RDOF_IPGW_PAYLOAD_IP;
    }
    n = N(c, rdof_jget(&c->doc, node, "to"));
    if (!n || n->type != RDOF_JSTR)
        return fail(c, "%s: to, \"host:port\" (or \"host\" to keep each datagram's port)", where);
    char host[80];
    snprintf(host, sizeof host, "%s", n->str);
    char *colon = strrchr(host, ':');
    if (colon) {
        *colon = 0;
        char *end;
        long port = strtol(colon + 1, &end, 10);
        if (!colon[1] || *end || port < 1 || port > 65535)
            return fail(c, "%s: to: port 1 to 65535", where);
        r->port = (int)port;
    }
    if (strlen(host) >= sizeof r->host || !host_ok(host))
        return fail(c, "%s: to: an IPv4 address or a host name", where);
    strcpy(r->host, host);
    uint32_t ip;
    if (rdof_ipgw_parse_ip(host, &ip)) {
        if (ip == 0 || ip >= 0xE0000000u)
            return fail(c, "%s: to: %s isn't one host's address", where, host);
        r->ip = ip;
    } else if (isdigit((unsigned char)host[strlen(host) - 1])) {
        return fail(c, "%s: to: %s isn't an IPv4 address", where, host);
    }
    if (r->payload == RDOF_IPGW_PAYLOAD_IP && !r->port)
        return fail(c, "%s: payload ip needs a port in to", where);
    return true;
}

static bool nets(ctx_t *c, int obj, const char *key, rdof_ipgw_net_t *out, int *n)
{
    int a = rdof_jget(&c->doc, obj, key);
    if (a < 0 || N(c, a)->type == RDOF_JNULL)
        return true;
    if (N(c, a)->type != RDOF_JARR || N(c, a)->count > RDOF_IPGW_MAX_NETS)
        return fail(c, "nat.%s: a list of up to %d networks (\"a.b.c.d/n\")", key, RDOF_IPGW_MAX_NETS);
    *n = 0;
    for (int i = 0; i < N(c, a)->count; i++) {
        const rdof_jnode_t *e = N(c, rdof_jat(&c->doc, a, i));
        if (e->type != RDOF_JSTR || !rdof_ipgw_parse_cidr(e->str, &out[*n]))
            return fail(c, "nat.%s[%d]: a network \"a.b.c.d/n\" (host bits zero) or an address", key, i);
        (*n)++;
    }
    return true;
}

static bool gateway(ctx_t *c, int o, rdof_ipgw_config_t *cfg)
{
    static const char *const keys[] = {"enabled", "id",     "radio_net", "tx",        "ack",    "max_bytes",
                                       "up_pps",  "down_pps", "idle_s",  "max_flows", "routes", "nat", NULL};
    const rdof_jnode_t *on = N(c, o);
    if (on->type == RDOF_JNULL || (on->type == RDOF_JBOOL && !on->b))
        return true;
    if (on->type != RDOF_JBOOL && on->type != RDOF_JOBJ)
        return fail(c, "an object (or false)");
    cfg->on = true;
    if (on->type == RDOF_JBOOL)
        return fail(c, "nothing to do: give it routes or nat");
    if (!only_keys(c, o, "", keys) || !boolean(c, o, "enabled", "", true, &cfg->on))
        return false;
    if (!cfg->on)
        return true;
    double v = 0;
    if (!number(c, o, "id", 0, MAX_ID, true, &v))
        return false;
    cfg->id = (uint32_t)v;
    const rdof_jnode_t *n = N(c, rdof_jget(&c->doc, o, "radio_net"));
    if (n && n->type != RDOF_JNULL) {
        rdof_ipgw_net_t net;
        if (n->type != RDOF_JSTR || !rdof_ipgw_parse_cidr(n->str, &net) || net.mask < 0xFF000000u ||
            net.mask > 0xFFFFFFFCu)
            return fail(c, "radio_net: a network \"a.b.c.d/n\", n 8 to 30 (12.0.0.0/8: an id's 24 bits fit)");
        cfg->radio_net = net;
    }
    n = N(c, rdof_jget(&c->doc, o, "tx"));
    if (n && n->type != RDOF_JNULL) {
        if (n->type != RDOF_JSTR || strlen(n->str) >= sizeof cfg->tx)
            return fail(c, "tx: a radio's name in run, up to %d characters", (int)sizeof cfg->tx - 1);
        strcpy(cfg->tx, n->str);
    }
    if (!boolean(c, o, "ack", "", true, &cfg->ack))
        return false;
    v = cfg->max_bytes;
    if (!number(c, o, "max_bytes", 68, RDOF_IPGW_MAX_PACKET, true, &v))
        return false;
    cfg->max_bytes = (int)v;
    if (!number(c, o, "up_pps", 0, 1000, false, &cfg->up_pps) ||
        !number(c, o, "down_pps", 0, 1000, false, &cfg->down_pps) ||
        !number(c, o, "idle_s", 5, 86400, false, &cfg->idle_s))
        return false;
    v = cfg->max_flows;
    if (!number(c, o, "max_flows", 1, RDOF_IPGW_MAX_FLOWS, true, &v))
        return false;
    cfg->max_flows = (int)v;
    int rs = rdof_jget(&c->doc, o, "routes");
    if (rs >= 0 && N(c, rs)->type != RDOF_JNULL) {
        if (N(c, rs)->type != RDOF_JARR || N(c, rs)->count > RDOF_IPGW_MAX_ROUTES)
            return fail(c, "routes: a list of up to %d routes", RDOF_IPGW_MAX_ROUTES);
        for (int i = 0; i < N(c, rs)->count; i++) {
            if (!route(c, rdof_jat(&c->doc, rs, i), i, &cfg->routes[i]))
                return false;
            for (int k = 0; k < i; k++)
                if (!strcmp(cfg->routes[k].name, cfg->routes[i].name))
                    return fail(c, "routes[%d]: name %s is taken", i, cfg->routes[i].name);
            cfg->nroutes++;
        }
    }
    int nat = rdof_jget(&c->doc, o, "nat");
    const rdof_jnode_t *nn = N(c, nat);
    if (nn && nn->type == RDOF_JBOOL) {
        cfg->nat = nn->b;
    } else if (nn && nn->type == RDOF_JOBJ) {
        static const char *const nat_keys[] = {"enabled", "allow", "deny", NULL};
        if (!only_keys(c, nat, "nat", nat_keys) || !boolean(c, nat, "enabled", "nat", true, &cfg->nat) ||
            !nets(c, nat, "allow", cfg->allow, &cfg->nallow) || !nets(c, nat, "deny", cfg->deny, &cfg->ndeny))
            return false;
    } else if (nn && nn->type != RDOF_JNULL) {
        return fail(c, "nat: true, false or {\"allow\": [...], \"deny\": [...]}");
    }
    if (!cfg->nroutes && !cfg->nat)
        return fail(c, "nothing to do: give it routes or nat");
    return true;
}

bool rdof_ipgw_settings_parse(const char *text, size_t len, rdof_ipgw_config_t *out, char *error, size_t cap)
{
    char dummy[8];
    if (!error || !cap) {
        error = dummy;
        cap = sizeof dummy;
    }
    error[0] = 0;
    rdof_ipgw_config_default(out);
    ctx_t *c = malloc(sizeof *c);
    char *clean = c ? malloc(len + 1) : NULL;
    if (!clean) {
        free(c);
        snprintf(error, cap, "no memory");
        return false;
    }
    c->error = error;
    c->cap = cap;
    size_t n = rdof_jsonc_strip(text, len, clean);
    c->doc = (rdof_jdoc_t){.nodes = c->nodes, .max = MAX_NODES, .buf = c->strs, .cap = STR_BUF};
    int root = rdof_jparse(&c->doc, clean, n);
    bool ok = true;
    if (root < 0) {
        snprintf(error, cap, "JSON: %s", c->doc.error);
        ok = false;
    } else if (N(c, root)->type != RDOF_JOBJ) {
        snprintf(error, cap, "the settings are a JSON object");
        ok = false;
    } else {
        int o = rdof_jget(&c->doc, root, "ip_gateway");
        if (o >= 0)
            ok = gateway(c, o, out);
    }
    if (!ok)
        out->on = false;
    free(clean);
    free(c);
    return ok;
}

/* ── describe ──────────────────────────────────────────────────────────────── */

static void ip_text(uint32_t ip, char out[16])
{
    snprintf(out, 16, "%u.%u.%u.%u", (unsigned)(ip >> 24), (unsigned)(ip >> 16 & 255), (unsigned)(ip >> 8 & 255),
             (unsigned)(ip & 255));
}

static int prefix(uint32_t mask)
{
    int n = 0;
    while (mask & 0x80000000u) {
        n++;
        mask <<= 1;
    }
    return n;
}

static void add(char *out, size_t cap, size_t *len, const char *fmt, ...)
{
    if (*len >= cap)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + *len, cap - *len, fmt, ap);
    va_end(ap);
    if (n > 0)
        *len = *len + (size_t)n < cap ? *len + (size_t)n : cap - 1;
}

size_t rdof_ipgw_describe(const rdof_ipgw_config_t *c, char *out, size_t cap)
{
    size_t len = 0;
    if (!cap)
        return 0;
    out[0] = 0;
    if (!c->on) {
        add(out, cap, &len, "off");
        return len;
    }
    char a[16];
    ip_text(c->radio_net.net, a);
    add(out, cap, &len, "id %lu, radios %s/%d, tx %s, ack %s, max %d B, up %g/s, down %g/s, idle %g s, flows %d",
        (unsigned long)c->id, a, prefix(c->radio_net.mask), c->tx[0] ? c->tx : "(first)", c->ack ? "on" : "off",
        c->max_bytes, c->up_pps, c->down_pps, c->idle_s, c->max_flows);
    for (int i = 0; i < c->nroutes; i++) {
        const rdof_ipgw_route_t *r = &c->routes[i];
        add(out, cap, &len, "; %s: ", r->name);
        if (r->all)
            add(out, cap, &len, "all");
        if (r->dst_kind != RDOF_IPGW_DST_ANY)
            add(out, cap, &len, "%s %lu-%lu", r->dst_kind == RDOF_IPGW_DST_PRIVATE ? "private" : "group",
                (unsigned long)r->dst_lo, (unsigned long)r->dst_hi);
        if (r->has_from)
            add(out, cap, &len, "%sfrom %lu-%lu", r->dst_kind != RDOF_IPGW_DST_ANY ? " " : "",
                (unsigned long)r->from_lo, (unsigned long)r->from_hi);
        if (r->port)
            add(out, cap, &len, " -> %s:%d %s", r->host, r->port, r->payload == RDOF_IPGW_PAYLOAD_IP ? "ip" : "udp");
        else
            add(out, cap, &len, " -> %s %s", r->host, r->payload == RDOF_IPGW_PAYLOAD_IP ? "ip" : "udp");
    }
    add(out, cap, &len, "; nat %s", c->nat ? "on" : "off");
    if (c->nat) {
        add(out, cap, &len, ", allow");
        if (!c->nallow)
            add(out, cap, &len, " any");
        for (int i = 0; i < c->nallow; i++) {
            ip_text(c->allow[i].net, a);
            add(out, cap, &len, " %s/%d", a, prefix(c->allow[i].mask));
        }
        add(out, cap, &len, ", deny");
        if (!c->ndeny)
            add(out, cap, &len, " none");
        for (int i = 0; i < c->ndeny; i++) {
            ip_text(c->deny[i].net, a);
            add(out, cap, &len, " %s/%d", a, prefix(c->deny[i].mask));
        }
    }
    return len;
}
