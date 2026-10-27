// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The IP gateway (rdof/ipgw.h): its settings, the address helpers, the packets it builds, and the gateway fed
 * bursts as a receiver hears them, with fake sockets and a fake transmitter. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/bits.h"
#include "rdof/features.h"
#include "rdof/fec.h"
#include "rdof/ipgw.h"
#include "rdof/json.h"
#include "rdof/layer2.h"
#include "rdof/mmdvm.h"
#include "rdof/pdu.h"

extern int checks, failures;

#define CHECK(cond, ...)                                                                                     \
    do {                                                                                                     \
        checks++;                                                                                            \
        if (!(cond)) {                                                                                       \
            failures++;                                                                                      \
            printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                          \
            printf(__VA_ARGS__);                                                                             \
            printf("\n");                                                                                    \
        }                                                                                                    \
    } while (0)

#define IP(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define RADIO 9994567u
#define RADIO_IP IP(12, 152, 129, 71)

static bool parse(const char *json, rdof_ipgw_config_t *c, char *err)
{
    return rdof_ipgw_settings_parse(json, strlen(json), c, err, 160);
}

static void bad(const char *json, const char *want)
{
    rdof_ipgw_config_t c;
    char err[160];
    bool ok = parse(json, &c, err);
    CHECK(!ok && strstr(err, want) && !c.on, "%s: got %s (%s), want %s", json, ok ? "ok" : "refused", err, want);
}

static void t_settings(void)
{
    rdof_ipgw_config_t c;
    char err[160];
    CHECK(parse("{}", &c, err) && !c.on, "absent: off");
    CHECK(parse("{\"ip_gateway\": false}", &c, err) && !c.on, "false: off");
    CHECK(parse("{\"ip_gateway\": null}", &c, err) && !c.on, "null: off");
    CHECK(parse("{\"ip_gateway\": {\"enabled\": false, \"nat\": true}}", &c, err) && !c.on, "enabled false");
    bad("{\"ip_gateway\": true}", "nothing to do");
    bad("{\"ip_gateway\": {}}", "nothing to do");
    bad("{\"ip_gateway\": 3}", "an object");

    const char *full = "{\"radios\": {}, // a comment\n"
                       " \"ip_gateway\": {\"id\": 9990, \"tx\": \"hs1\", \"ack\": false, \"max_bytes\": 600,\n"
                       "  \"up_pps\": 4, \"down_pps\": 1.5, \"idle_s\": 60, \"max_flows\": 3,\n"
                       "  \"radio_net\": \"12.0.0.0/8\",\n"
                       "  \"routes\": [{\"name\": \"ars\", \"private\": 9990, \"to\": \"192.0.2.20:5005\"},\n"
                       "             {\"group\": \"9-10\", \"from\": \"1000-1999\", \"to\": \"10.0.0.5\"},\n"
                       "             {\"all\": true, \"to\": \"tunnel.example.com:7000\", \"payload\": \"ip\"}],\n"
                       "  \"nat\": {\"allow\": [\"0.0.0.0/0\"], \"deny\": [\"8.8.4.0/24\"]}}}";
    CHECK(parse(full, &c, err), "full: %s", err);
    CHECK(c.on && c.id == 9990 && !strcmp(c.tx, "hs1") && !c.ack && c.max_bytes == 600 && c.up_pps == 4 &&
              c.down_pps == 1.5 && c.idle_s == 60 && c.max_flows == 3,
          "full: the scalars");
    CHECK(c.nroutes == 3 && !strcmp(c.routes[0].name, "ars") && c.routes[0].dst_kind == RDOF_IPGW_DST_PRIVATE &&
              c.routes[0].dst_lo == 9990 && c.routes[0].dst_hi == 9990 && c.routes[0].ip == IP(192, 0, 2, 20) &&
              c.routes[0].port == 5005 && c.routes[0].payload == RDOF_IPGW_PAYLOAD_UDP,
          "route 0");
    CHECK(!strcmp(c.routes[1].name, "route2") && c.routes[1].dst_kind == RDOF_IPGW_DST_GROUP &&
              c.routes[1].dst_lo == 9 && c.routes[1].dst_hi == 10 && c.routes[1].has_from &&
              c.routes[1].from_lo == 1000 && c.routes[1].from_hi == 1999 && c.routes[1].port == 0,
          "route 1");
    CHECK(c.routes[2].all && c.routes[2].ip == 0 && !strcmp(c.routes[2].host, "tunnel.example.com") &&
              c.routes[2].port == 7000 && c.routes[2].payload == RDOF_IPGW_PAYLOAD_IP,
          "route 2");
    CHECK(c.nat && c.nallow == 1 && c.allow[0].mask == 0 && c.ndeny == 1 && c.deny[0].net == IP(8, 8, 4, 0) &&
              c.deny[0].mask == 0xFFFFFF00u,
          "nat lists");
    char d[600];
    rdof_ipgw_describe(&c, d, sizeof d);
    CHECK(!strcmp(d, "id 9990, radios 12.0.0.0/8, tx hs1, ack off, max 600 B, up 4/s, down 1.5/s, idle 60 s, flows 3; "
                     "ars: private 9990-9990 -> 192.0.2.20:5005 udp; route2: group 9-10 from 1000-1999 -> 10.0.0.5 "
                     "udp; route3: all -> tunnel.example.com:7000 ip; nat on, allow 0.0.0.0/0, deny 8.8.4.0/24"),
          "describe: %s", d);
    rdof_ipgw_config_t off;
    rdof_ipgw_config_default(&off);
    rdof_ipgw_describe(&off, d, sizeof d);
    CHECK(!strcmp(d, "off"), "describe off: %s", d);

    /* nat on its own: the default deny list */
    CHECK(parse("{\"ip_gateway\": {\"nat\": true}}", &c, err) && c.on && c.nat && c.ndeny == 5 && c.nroutes == 0 &&
              c.ack && c.max_bytes == 1500 && c.max_flows == 8,
          "nat true: %s", err);
    CHECK(parse("{\"ip_gateway\": {\"nat\": {\"deny\": []}}}", &c, err) && c.nat && c.ndeny == 0, "deny []: %s", err);
    CHECK(parse("{\"ip_gateway\": {\"nat\": {\"enabled\": false}, \"routes\": [{\"all\": true, \"to\": \"1.2.3.4\"}]}}",
                &c, err) &&
              !c.nat && c.nroutes == 1,
          "nat enabled false: %s", err);

    /* the examples in docs/spec/fw/config.md and docs/reference.md */
    const char *doc = "{\"ip_gateway\": {\n  \"id\": 9990,\n  \"routes\": [\n"
                      "    {\"name\": \"ars\", \"private\": 9990, \"to\": \"192.0.2.20:4005\"},  // ARS\n"
                      "    {\"name\": \"tms\", \"private\": \"9991-9999\", \"to\": \"192.0.2.20\"},\n"
                      "    {\"name\": \"fleet\", \"from\": \"9995000-9995999\", \"to\": \"gw.example.com:7000\", "
                      "\"payload\": \"ip\"}\n  ],\n  \"nat\": {\"allow\": [\"0.0.0.0/0\"]}  // the rest\n}}";
    CHECK(parse(doc, &c, err) && c.nroutes == 3 && c.nat && c.ndeny == 5 && c.routes[2].has_from, "config.md: %s",
          err);
    CHECK(parse("{\"ip_gateway\": {\n  \"routes\": [{\"name\": \"ars\", \"private\": 9990, \"to\": "
                "\"192.0.2.20:4005\"}],\n  \"nat\": true\n}}",
                &c, err) &&
              c.nroutes == 1 && c.nat,
          "reference.md: %s", err);

    bad("{\"ip_gateway\": {\"nat\": true, \"bogus\": 1}}", "unknown key \"bogus\"");
    bad("{\"ip_gateway\": {\"routes\": [{\"to\": \"1.2.3.4\"}]}}", "which traffic");
    bad("{\"ip_gateway\": {\"routes\": [{\"private\": 1, \"group\": 2, \"to\": \"1.2.3.4\"}]}}", "not both");
    bad("{\"ip_gateway\": {\"routes\": [{\"all\": true, \"from\": 2, \"to\": \"1.2.3.4\"}]}}", "not both");
    bad("{\"ip_gateway\": {\"routes\": [{\"private\": \"5-3\", \"to\": \"1.2.3.4\"}]}}", "routes[0].private: a DMR id");
    bad("{\"ip_gateway\": {\"routes\": [{\"private\": 0, \"to\": \"1.2.3.4\"}]}}", "a DMR id");
    bad("{\"ip_gateway\": {\"routes\": [{\"group\": 16777216, \"to\": \"1.2.3.4\"}]}}", "a DMR id");
    bad("{\"ip_gateway\": {\"routes\": [{\"from\": \"x\", \"to\": \"1.2.3.4\"}]}}", "routes[0].from");
    bad("{\"ip_gateway\": {\"routes\": [{\"all\": true}]}}", "to, \"host:port\"");
    bad("{\"ip_gateway\": {\"routes\": [{\"all\": true, \"to\": \"1.2.3.4:0\"}]}}", "port 1 to 65535");
    bad("{\"ip_gateway\": {\"routes\": [{\"all\": true, \"to\": \"1.2.3.4:x\"}]}}", "port 1 to 65535");
    bad("{\"ip_gateway\": {\"routes\": [{\"all\": true, \"to\": \"300.1.1.1\"}]}}", "isn't an IPv4 address");
    bad("{\"ip_gateway\": {\"routes\": [{\"all\": true, \"to\": \"224.0.0.1:5\"}]}}", "isn't one host's address");
    bad("{\"ip_gateway\": {\"routes\": [{\"all\": true, \"to\": \"a b:5\"}]}}", "an IPv4 address or a host name");
    bad("{\"ip_gateway\": {\"routes\": [{\"all\": true, \"to\": \"1.2.3.4\", \"payload\": \"ip\"}]}}",
        "payload ip needs a port");
    bad("{\"ip_gateway\": {\"routes\": [{\"all\": true, \"to\": \"1.2.3.4\", \"payload\": \"tcp\"}]}}",
        "payload udp or ip");
    bad("{\"ip_gateway\": {\"routes\": [{\"all\": true, \"to\": \"1.2.3.4\", \"oops\": 1}]}}",
        "routes[0]: unknown key \"oops\"");
    bad("{\"ip_gateway\": {\"routes\": [{\"name\": \"a\", \"all\": true, \"to\": \"1.2.3.4\"}, {\"name\": \"a\", "
        "\"all\": true, \"to\": \"1.2.3.4\"}]}}",
        "name a is taken");
    bad("{\"ip_gateway\": {\"routes\": [1]}}", "routes[0]: an object");
    bad("{\"ip_gateway\": {\"routes\": {}}}", "a list of up to 8 routes");
    bad("{\"ip_gateway\": {\"nat\": {\"deny\": [\"10.0.0.1/8\"]}}}", "nat.deny[0]");
    bad("{\"ip_gateway\": {\"nat\": {\"allow\": \"0.0.0.0/0\"}}}", "nat.allow: a list");
    bad("{\"ip_gateway\": {\"nat\": 1}}", "nat: true, false");
    bad("{\"ip_gateway\": {\"nat\": true, \"radio_net\": \"12.0.0.0/4\"}}", "radio_net");
    bad("{\"ip_gateway\": {\"nat\": true, \"max_flows\": 17}}", "max_flows: a whole number 1 to 16");
    bad("{\"ip_gateway\": {\"nat\": true, \"max_bytes\": 2000}}", "max_bytes");
    bad("{\"ip_gateway\": {\"nat\": true, \"up_pps\": -1}}", "up_pps");
    bad("{\"ip_gateway\": {\"nat\": true, \"ack\": 1}}", "ack: true or false");
    bad("{\"ip_gateway\": {\"nat\": true, \"tx\": 5}}", "tx: a radio's name");
    bad("{\"ip_gateway\": {\"nat\": true, \"id\": 1.5}}", "id: a whole number");
    bad("[1]", "a JSON object");
    bad("{", "JSON:");
}

static void t_addresses(void)
{
    rdof_ipgw_net_t n;
    uint32_t ip, id;
    CHECK(rdof_ipgw_parse_ip("12.152.129.71", &ip) && ip == RADIO_IP, "parse ip");
    CHECK(!rdof_ipgw_parse_ip("1.2.3", &ip) && !rdof_ipgw_parse_ip("1.2.3.4.5", &ip) &&
              !rdof_ipgw_parse_ip("1.2.3.256", &ip) && !rdof_ipgw_parse_ip("1..2.3", &ip) &&
              !rdof_ipgw_parse_ip("01234.1.1.1", &ip) && !rdof_ipgw_parse_ip("", &ip),
          "bad addresses");
    CHECK(rdof_ipgw_parse_cidr("10.0.0.0/8", &n) && n.net == IP(10, 0, 0, 0) && n.mask == 0xFF000000u, "cidr");
    CHECK(rdof_ipgw_parse_cidr("1.2.3.4", &n) && n.mask == 0xFFFFFFFFu, "a host");
    CHECK(rdof_ipgw_parse_cidr("0.0.0.0/0", &n) && n.mask == 0 && rdof_ipgw_in_net(&n, IP(8, 8, 8, 8)), "any");
    CHECK(!rdof_ipgw_parse_cidr("10.0.0.1/8", &n) && !rdof_ipgw_parse_cidr("10.0.0.0/33", &n) &&
              !rdof_ipgw_parse_cidr("10.0.0.0/", &n) && !rdof_ipgw_parse_cidr("10.0.0.0/x", &n) &&
              !rdof_ipgw_parse_cidr("10.0.0.0/100", &n),
          "bad cidrs");

    rdof_ipgw_config_t c;
    rdof_ipgw_config_default(&c);
    CHECK(rdof_ipgw_radio_ip(&c.radio_net, RADIO) == RADIO_IP, "radio ip %08x",
          rdof_ipgw_radio_ip(&c.radio_net, RADIO));
    CHECK(rdof_ipgw_ip_radio(&c.radio_net, RADIO_IP, &id) && id == RADIO, "ip radio");
    CHECK(!rdof_ipgw_ip_radio(&c.radio_net, IP(13, 0, 0, 1), &id), "outside radio_net");
    CHECK(!rdof_ipgw_ip_radio(&c.radio_net, IP(12, 0, 0, 0), &id), "id 0");
    rdof_ipgw_net_t small;
    rdof_ipgw_parse_cidr("10.10.0.0/16", &small);
    CHECK(rdof_ipgw_radio_ip(&small, 0x010203) == IP(10, 10, 2, 3), "a /16 keeps the low bits");
    CHECK(rdof_ipgw_ip_radio(&small, IP(10, 10, 2, 3), &id) && id == 0x0203, "and back");

    CHECK(rdof_ipgw_nat_allowed(&c, IP(8, 8, 8, 8)), "public: allowed");
    const uint32_t no[] = {IP(10, 1, 2, 3),     IP(192, 168, 1, 1), IP(172, 20, 0, 1),     IP(100, 64, 0, 1),
                           IP(13, 0, 0, 1),     RADIO_IP,           IP(127, 0, 0, 1),      IP(0, 1, 2, 3),
                           IP(169, 254, 1, 1),  IP(224, 0, 0, 1),   IP(255, 255, 255, 255), IP(240, 0, 0, 1)};
    for (size_t i = 0; i < sizeof no / sizeof no[0]; i++)
        CHECK(!rdof_ipgw_nat_allowed(&c, no[i]), "%08x denied", no[i]);
    c.ndeny = 0;
    CHECK(rdof_ipgw_nat_allowed(&c, IP(10, 1, 2, 3)), "deny emptied: the LAN");
    CHECK(!rdof_ipgw_nat_allowed(&c, RADIO_IP) && !rdof_ipgw_nat_allowed(&c, IP(127, 0, 0, 1)), "built in stays");
    rdof_ipgw_parse_cidr("203.0.113.0/24", &c.allow[0]);
    c.nallow = 1;
    CHECK(rdof_ipgw_nat_allowed(&c, IP(203, 0, 113, 9)) && !rdof_ipgw_nat_allowed(&c, IP(8, 8, 8, 8)), "allow list");
}

#if RDOF_FEATURE_DATA_BLOCKS

/* ── a fake platform ───────────────────────────────────────────────────────── */

typedef struct {
    bool open[RDOF_IPGW_MAX_FLOWS];
    int opens, closes;
    bool refuse_open, refuse_send, no_tx;
    int sent;
    int last_flow;
    uint32_t last_ip;
    uint16_t last_port;
    int last_ttl;
    uint8_t last[2048];
    size_t last_len;
    int dmr;
    rdof_ipgw_dmr_t out;
    int icmps, icmp_ttl;
    uint32_t icmp_ip;
    uint8_t icmp[2048];
    size_t icmp_len;
} fake_t;

static bool f_icmp(void *ctx, uint32_t ip, const uint8_t *m, size_t len, int ttl)
{
    fake_t *f = ctx;
    f->icmps++;
    f->icmp_ip = ip;
    f->icmp_ttl = ttl;
    memcpy(f->icmp, m, len);
    f->icmp_len = len;
    return true;
}

static uint16_t f_port(void *ctx, int flow)
{
    return (uint16_t)(40000 + flow);
}

static bool f_open(void *ctx, int flow)
{
    fake_t *f = ctx;
    if (f->refuse_open)
        return false;
    f->open[flow] = true;
    f->opens++;
    return true;
}

static void f_close(void *ctx, int flow)
{
    fake_t *f = ctx;
    f->open[flow] = false;
    f->closes++;
}

static bool f_send(void *ctx, int flow, uint32_t ip, uint16_t port, const uint8_t *data, size_t len, int ttl)
{
    fake_t *f = ctx;
    f->last_ttl = ttl;
    if (f->refuse_send)
        return false;
    f->sent++;
    f->last_flow = flow;
    f->last_ip = ip;
    f->last_port = port;
    memcpy(f->last, data, len);
    f->last_len = len;
    return true;
}

static bool f_dmr(void *ctx, const rdof_ipgw_dmr_t *p)
{
    fake_t *f = ctx;
    if (f->no_tx)
        return false;
    f->dmr++;
    f->out = *p;
    return true;
}

static rdof_ipgw_t *gw(fake_t *f, const char *json)
{
    rdof_ipgw_config_t c;
    char err[160];
    if (!rdof_ipgw_settings_parse(json, strlen(json), &c, err, sizeof err)) {
        printf("FAIL settings %s: %s\n", json, err);
        failures++;
    }
    rdof_ipgw_t *g = calloc(1, sizeof *g);
    rdof_ipgw_init(g, &c);
    memset(f, 0, sizeof *f);
    g->open = f_open;
    g->close = f_close;
    g->send_udp = f_send;
    g->send_dmr = f_dmr;
    g->send_icmp = f_icmp;
    g->local_port = f_port;
    g->ctx = f;
    return g;
}

static const rdof_ipgw_where_t W = {.source = 0, .slot = 1, .freq = 462562500, .cc = 1};

/* A radio's UDP datagram. */
static size_t datagram(uint32_t sip, uint16_t sport, uint32_t dip, uint16_t dport, const char *payload, uint8_t *out)
{
    return rdof_ipgw_udp_datagram(sip, sport, dip, dport, 1, (const uint8_t *)payload, strlen(payload), out, 2048);
}

/* Bursts heard: each of a packet's, as a receiver would pass them on. */
static void hear(rdof_ipgw_t *g, const rdof_ipgw_where_t *w, const rdof_ipgw_dmr_t *p, double now)
{
    for (int i = 0; i < p->nbursts; i++)
        rdof_ipgw_burst(g, w, p->control[i] & 0x0F, p->bursts[i], now);
}

/* A radio sends data (unconfirmed rate 1/2) from src to dst. */
static void radio_sends(rdof_ipgw_t *g, uint32_t src, uint32_t dst, bool group, const uint8_t *d, size_t n,
                        double now)
{
    static rdof_ipgw_dmr_t p;
    CHECK(rdof_ipgw_build_dmr(src, dst, RDOF_SAP_IP, d, n, 1, rdof_data_sync(RDOF_MS_VOICE), &p), "build");
    if (group) { /* the same with G set */
        uint8_t h[12];
        rdof_bptc196_decode(p.bursts[0], h);
        h[0] |= 0x80;
        uint16_t crc = rdof_crc16_ccitt(h, 10) ^ RDOF_MASK_DATA_HEADER;
        h[10] = (uint8_t)(crc >> 8);
        h[11] = (uint8_t)crc;
        rdof_data_burst(h, RDOF_DT_DATA_HEADER, 1, rdof_data_sync(RDOF_MS_VOICE), p.bursts[0]);
    }
    hear(g, &W, &p, now);
}

/* The user data of a packet the gateway built (unconfirmed rate 1/2): its length, or -1 if it doesn't check. */
static int unpack(const rdof_ipgw_dmr_t *p, rdof_data_header_t *h, uint8_t *out)
{
    rdof_decode_data_header(p->bursts[0], h);
    if (!h->crc_ok || p->nbursts != 1 + h->blocks)
        return -1;
    int len = 0;
    for (int i = 1; i < p->nbursts; i++) {
        rdof_data_block_t b;
        if ((p->control[i] & 0x0F) != RDOF_DT_RATE_12 || !rdof_decode_data_block(p->bursts[i], RDOF_DT_RATE_12, &b))
            return -1;
        memcpy(out + len, b.payload, 12);
        len += 12;
    }
    if (!rdof_check_crc32(out, (size_t)len))
        return -1;
    return len - 4 - h->pad_octets;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t rd32(const uint8_t *p)
{
    return rdof_be32(p);
}

/* The datagram's checksums hold. */
static bool sums_ok(const uint8_t *d, size_t n)
{
    uint32_t s = 0;
    for (int i = 0; i < 20; i += 2)
        s += rd16(d + i);
    while (s >> 16)
        s = (s & 0xFFFF) + (s >> 16);
    if (s != 0xFFFF)
        return false;
    s = rd16(d + 12) + rd16(d + 14) + rd16(d + 16) + rd16(d + 18) + 17 + (uint32_t)(n - 20);
    for (size_t i = 20; i + 1 < n; i += 2)
        s += rd16(d + i);
    if (n & 1)
        s += (uint32_t)d[n - 1] << 8;
    while (s >> 16)
        s = (s & 0xFFFF) + (s >> 16);
    return s == 0xFFFF;
}

static void t_packets(void)
{
    uint8_t d[2048];
    char hex[200];
    size_t n = rdof_ipgw_udp_datagram(IP(13, 0, 0, 1), 4005, RADIO_IP, 4001, 7, (const uint8_t *)"abc", 3, d, sizeof d);
    for (size_t i = 0; i < n; i++)
        snprintf(hex + 2 * i, 3, "%02x", d[i]);
    /* Python's struct and the RFC 1071 sum, written apart from this code */
    CHECK(n == 31 && !strcmp(hex, "4500001f000700004011dfe70d0000010c9881470fa50fa1000b814f616263"), "datagram %s",
          hex);
    CHECK(rdof_ipgw_udp_datagram(1, 2, 3, 4, 0, d, 10, d, 37) == 0, "doesn't fit");

    /* a whole packet, header and blocks, read back */
    static rdof_ipgw_dmr_t p;
    CHECK(rdof_ipgw_build_dmr(9990, RADIO, 4, d, n, 3, rdof_data_sync(RDOF_MS_VOICE), &p), "build");
    rdof_data_header_t h;
    uint8_t back[2048];
    int len = unpack(&p, &h, back);
    CHECK(len == 31 && !memcmp(back, d, 31), "read back: %d", len);
    CHECK(h.dpf == RDOF_DPF_UNCONFIRMED && h.sap == 4 && h.dst_id == RADIO && h.src_id == 9990 && !h.group &&
              h.blocks == 3 && h.pad_octets == 1 && h.full_message && h.fsn == 8,
          "header: dpf %d sap %d blocks %d pad %d fsn %d", h.dpf, h.sap, h.blocks, h.pad_octets, h.fsn);
    int cc, dt, errs;
    CHECK(rdof_decode_slot_type(p.bursts[1], &cc, &dt, &errs) && cc == 3 && dt == RDOF_DT_RATE_12, "slot type");
    CHECK(rdof_sync_errors(p.bursts[0], rdof_data_sync(RDOF_MS_VOICE)) == 0, "MS data sync");
    /* exactly 8 octets of data and CRC fill one block; the longest fills 127 */
    CHECK(rdof_ipgw_build_dmr(1, 2, 4, d, 8, 0, 0, &p) && p.nbursts == 2, "one block");
    CHECK(rdof_ipgw_build_dmr(1, 2, 4, d, RDOF_IPGW_MAX_PACKET, 0, 0, &p) && p.nbursts == 128, "127 blocks");
    CHECK(!rdof_ipgw_build_dmr(1, 2, 4, d, RDOF_IPGW_MAX_PACKET + 1, 0, 0, &p), "too long");
    /* a pad of 16 or more uses the header's high bit */
    uint8_t hd[12];
    rdof_ipgw_data_header(true, true, true, 4, 17, 5, 6, 9, 5, hd);
    rdof_parse_data_header(hd, &h);
    CHECK(h.crc_ok && h.group && h.response_requested && h.dpf == RDOF_DPF_CONFIRMED && h.pad_octets == 17 &&
              h.blocks == 9 && h.ns == 5 && h.dst_id == 5 && h.src_id == 6,
          "confirmed header");
    rdof_ipgw_build_ack(9990, RADIO, 4, 6, 1, 0, &p);
    rdof_decode_data_header(p.bursts[0], &h);
    CHECK(p.nbursts == 1 && p.response && h.crc_ok && h.dpf == RDOF_DPF_RESPONSE && h.response_class == 0 &&
              h.response_type == 1 && h.response_status == 6 && h.dst_id == RADIO && h.src_id == 9990 &&
              h.blocks == 0,
          "ack: class %d type %d status %d", h.response_class, h.response_type, h.response_status);
}

static void t_route(void)
{
    fake_t f;
    rdof_ipgw_t *g = gw(&f, "{\"ip_gateway\": {\"id\": 7, \"routes\": ["
                            "{\"name\": \"ars\", \"private\": 9990, \"to\": \"192.0.2.20:5005\"},"
                            "{\"group\": 9, \"to\": \"192.0.2.21\"}]}}");
    uint8_t d[2048], back[2048];
    size_t n = datagram(RADIO_IP, 4001, IP(13, 0, 0, 1), 4005, "hello ars", d);
    radio_sends(g, RADIO, 9990, false, d, n, 10.0);
    CHECK(g->n.rx_messages == 1 && f.opens == 1 && f.sent == 1, "forwarded: messages %u opens %d sent %d",
          g->n.rx_messages, f.opens, f.sent);
    CHECK(f.last_ip == IP(192, 0, 2, 20) && f.last_port == 5005 && f.last_len == 9 && !memcmp(f.last, "hello ars", 9),
          "to the route: %08x:%u", f.last_ip, f.last_port);
    CHECK(g->n.up_packets == 1 && g->n.up_bytes == 9 && rdof_ipgw_flows(g) == 1, "counted");

    /* the reply goes back to the radio, from where it sent to */
    rdof_ipgw_from_net(g, f.last_flow, IP(192, 0, 2, 20), 5005, (const uint8_t *)"ok", 2, 10.5);
    CHECK(f.dmr == 1 && f.out.slot == 1 && f.out.freq == 462562500 && f.out.cc == 1, "sent down");
    rdof_data_header_t h;
    int len = unpack(&f.out, &h, back);
    CHECK(len == 30 && h.dst_id == RADIO && h.src_id == 9990 && !h.group && h.sap == 4, "reply header: len %d", len);
    CHECK(sums_ok(back, 30) && rd32(back + 12) == IP(13, 0, 0, 1) && rd16(back + 20) == 4005 &&
              rd32(back + 16) == RADIO_IP && rd16(back + 22) == 4001 && !memcmp(back + 28, "ok", 2),
          "reply datagram");
    CHECK(g->n.down_packets == 1 && g->n.down_bytes == 30, "down counted");

    /* from anyone else: dropped */
    rdof_ipgw_from_net(g, f.last_flow, IP(192, 0, 2, 20), 5006, (const uint8_t *)"x", 1, 10.6);
    rdof_ipgw_from_net(g, f.last_flow, IP(192, 0, 2, 99), 5005, (const uint8_t *)"x", 1, 10.6);
    CHECK(g->n.down_stranger == 2 && f.dmr == 1, "strangers");
    rdof_ipgw_from_net(g, 5, IP(192, 0, 2, 20), 5005, (const uint8_t *)"x", 1, 10.6);
    rdof_ipgw_from_net(g, -1, IP(192, 0, 2, 20), 5005, (const uint8_t *)"x", 1, 10.6);
    CHECK(f.dmr == 1, "no such flow");

    /* the reply heard back by a receiver here isn't relayed again */
    hear(g, &W, &f.out, 10.7);
    CHECK(g->n.rx_dup == 1 && f.sent == 1, "our own: dup %u", g->n.rx_dup);

    /* a talkgroup: the second route, the datagram's own port; answered from the gateway's id */
    n = datagram(RADIO_IP, 4001, IP(13, 0, 0, 9), 4007, "tg", d);
    radio_sends(g, RADIO, 9, true, d, n, 11.0);
    CHECK(f.sent == 2 && f.last_ip == IP(192, 0, 2, 21) && f.last_port == 4007, "group route: %u", f.last_port);
    rdof_ipgw_from_net(g, f.last_flow, IP(192, 0, 2, 21), 4007, (const uint8_t *)"r", 1, 11.5);
    len = unpack(&f.out, &h, back);
    CHECK(f.dmr == 2 && len == 29 && h.src_id == 7 && h.dst_id == RADIO && rd32(back + 12) == IP(13, 0, 0, 9),
          "group reply from id 7: %u", h.src_id);

    /* a private call no route has, and NAT off */
    n = datagram(RADIO_IP, 4001, IP(13, 0, 0, 1), 4005, "nobody", d);
    radio_sends(g, RADIO, 1234, false, d, n, 12.0);
    CHECK(g->n.no_route == 1 && f.sent == 2, "no route");
    /* ICMP on a udp route: matched, not sent */
    n = datagram(RADIO_IP, 4001, IP(13, 0, 0, 1), 4005, "ping", d);
    d[9] = 1;
    d[10] = d[11] = 0;
    uint32_t s = 0;
    for (int i = 0; i < 20; i += 2)
        s += rd16(d + i);
    while (s >> 16)
        s = (s & 0xFFFF) + (s >> 16);
    d[10] = (uint8_t)(~s >> 8);
    d[11] = (uint8_t)~s;
    radio_sends(g, RADIO, 9990, false, d, n, 12.5);
    CHECK(g->n.not_udp == 1 && f.sent == 2, "not udp on a route");

    /* idle: closed */
    rdof_ipgw_tick(g, 100.0);
    CHECK(rdof_ipgw_flows(g) == 2, "not yet idle");
    rdof_ipgw_tick(g, 140.0);
    CHECK(rdof_ipgw_flows(g) == 0 && f.closes == 2 && g->n.flows_expired == 2, "idle flows closed");

    /* status */
    char buf[1200];
    rdof_json_t j;
    rdof_json_init(&j, buf, sizeof buf);
    rdof_json_open(&j, NULL);
    rdof_ipgw_status_json(g, &j);
    rdof_json_close(&j);
    CHECK(rdof_json_done(&j) && strstr(buf, "\"ip_gateway\":{\"routes\":2,\"nat\":false,\"flows\":0,\"max_flows\":8,") &&
              strstr(buf, "\"up_packets\":2,") && strstr(buf, "\"flows_expired\":2,\"queue_full\":0,\"sap3\":0,\"ttl_expired\":0,\"down_ttl_expired\":0,\"icmp_up\":0,\"icmp_down\":0,\"icmp_errors\":0,\"icmp_unmatched\":0,\"down_own_source\":0,\"tx_busy\":0}"),
          "status: %s", buf);
    free(g);
}

static void t_nat(void)
{
    fake_t f;
    rdof_ipgw_t *g = gw(&f, "{\"ip_gateway\": {\"nat\": true, \"max_flows\": 2, \"up_pps\": 0, \"down_pps\": 0}}");
    uint8_t d[2048], back[2048];
    size_t n = datagram(RADIO_IP, 5000, IP(8, 8, 8, 8), 53, "query", d);
    radio_sends(g, RADIO, 9990, false, d, n, 1.0);
    CHECK(f.sent == 1 && f.last_ip == IP(8, 8, 8, 8) && f.last_port == 53 && f.last_len == 5, "nat: sent");
    int flow = f.last_flow;
    /* the same flow again: no new socket */
    radio_sends(g, RADIO, 9990, false, d, n, 6.5); /* past the dedup window: a new message */
    CHECK(f.sent == 2 && f.opens == 1 && f.last_flow == flow, "same flow");
    rdof_ipgw_from_net(g, flow, IP(8, 8, 8, 8), 53, (const uint8_t *)"answer", 6, 7.0);
    rdof_data_header_t h;
    int len = unpack(&f.out, &h, back);
    CHECK(len == 34 && h.dst_id == RADIO && h.src_id == 9990 && rd32(back + 12) == IP(8, 8, 8, 8) &&
              rd16(back + 20) == 53 && rd32(back + 16) == RADIO_IP && rd16(back + 22) == 5000 && sums_ok(back, 34),
          "nat reply");

    /* another port: a second flow; a third destination finds the table full */
    n = datagram(RADIO_IP, 5001, IP(8, 8, 8, 8), 53, "q2", d);
    radio_sends(g, RADIO, 9990, false, d, n, 8.0);
    n = datagram(RADIO_IP, 5000, IP(1, 1, 1, 1), 53, "q3", d);
    radio_sends(g, RADIO, 9990, false, d, n, 8.1);
    CHECK(f.opens == 2 && g->n.flows_full == 1 && f.sent == 3, "full: opens %d full %u", f.opens, g->n.flows_full);

    /* denied: the LAN, the radios, the dispatch side, loopback, broadcast */
    const uint32_t no[] = {IP(192, 168, 1, 1), IP(12, 0, 0, 5), IP(13, 0, 0, 1), IP(127, 0, 0, 1),
                           IP(255, 255, 255, 255)};
    for (size_t i = 0; i < sizeof no / sizeof no[0]; i++) {
        n = datagram(RADIO_IP, 5000, no[i], 53, "x", d);
        d[28] = (uint8_t)i; /* each a different message */
        radio_sends(g, RADIO, 9990, false, d, n, 9.0);
    }
    CHECK(g->n.denied == 5 && f.sent == 3, "denied %u", g->n.denied);

    /* not UDP, not IPv4, a bad checksum, a fragment, too short, port 0 */
    n = datagram(RADIO_IP, 5000, IP(8, 8, 8, 8), 53, "tcp?", d);
    d[9] = 6;
    radio_sends(g, RADIO, 9990, false, d, n, 9.5);
    CHECK(g->n.bad_ip == 1, "the protocol changed, the checksum didn't");
    n = datagram(RADIO_IP, 5000, IP(8, 8, 8, 8), 53, "frag", d);
    d[6] = 0x20; /* MF */
    d[10] = d[11] = 0;
    uint32_t s = 0;
    for (int i = 0; i < 20; i += 2)
        s += rd16(d + i);
    while (s >> 16)
        s = (s & 0xFFFF) + (s >> 16);
    d[10] = (uint8_t)(~s >> 8);
    d[11] = (uint8_t)~s;
    radio_sends(g, RADIO, 9990, false, d, n, 9.6);
    CHECK(g->n.fragment == 1, "fragment");
    d[0] = 0x60;
    radio_sends(g, RADIO, 9990, false, d, n, 9.7);
    CHECK(g->n.not_ip == 1, "IPv6");
    radio_sends(g, RADIO, 9990, false, d, 10, 9.8);
    CHECK(g->n.not_ip == 2, "too short");
    n = datagram(RADIO_IP, 5000, IP(8, 8, 8, 8), 0, "port0", d);
    radio_sends(g, RADIO, 9990, false, d, n, 9.9);
    CHECK(g->n.bad_ip == 2, "port 0");
    n = datagram(RADIO_IP, 5000, IP(8, 8, 8, 8), 53, "udp", d);
    d[24] = 0xFF; /* the UDP length past the datagram */
    radio_sends(g, RADIO, 9990, false, d, n, 9.95);
    CHECK(g->n.bad_ip == 3, "udp length");
    rdof_ipgw_where_t w = W;
    d[28] ^= 0x55; /* not the message just heard */
    rdof_ipgw_message(g, &w, RADIO, 9990, false, 9, d, n, 9.97);
    CHECK(g->n.not_ip == 3, "SAP 9");
    CHECK(f.sent == 3, "none of them sent");

    /* ICMP: dropped by NAT */
    n = datagram(RADIO_IP, 5000, IP(8, 8, 8, 8), 53, "icmp", d);
    d[9] = 1;
    d[10] = d[11] = 0;
    s = 0;
    for (int i = 0; i < 20; i += 2)
        s += rd16(d + i);
    while (s >> 16)
        s = (s & 0xFFFF) + (s >> 16);
    d[10] = (uint8_t)(~s >> 8);
    d[11] = (uint8_t)~s;
    radio_sends(g, RADIO, 9990, false, d, n, 10.0);
    CHECK(g->n.not_udp == 1, "icmp");
    free(g);
}

/* Confirmed rate 1/2 blocks for data: 10 octets each, the CRC-9 over each with its serial number. */
static int confirmed(uint32_t src, uint32_t dst, int ns, const uint8_t *data, size_t len, rdof_ipgw_dmr_t *p,
                     bool spoil)
{
    int blocks = (int)((len + 4 + 9) / 10), pad = blocks * 10 - (int)len - 4;
    uint64_t sync = rdof_data_sync(RDOF_MS_VOICE);
    uint8_t h[12], buf[1400];
    rdof_ipgw_data_header(false, true, true, 4, pad, dst, src, blocks, ns, h);
    rdof_data_burst(h, RDOF_DT_DATA_HEADER, 1, sync, p->bursts[0]);
    p->control[0] = RDOF_SYNC_DATA | RDOF_DT_DATA_HEADER;
    memcpy(buf, data, len);
    memset(buf + len, 0, (size_t)pad);
    uint32_t crc = rdof_crc32_dmr(buf, len + (size_t)pad);
    for (int i = 0; i < 4; i++)
        buf[len + (size_t)pad + (size_t)i] = (uint8_t)(crc >> (8 * i));
    for (int b = 0; b < blocks; b++) {
        uint8_t blk[12], bits[87];
        memcpy(blk + 2, buf + 10 * b, 10);
        rdof_bytes_to_bits(blk + 2, 10, bits);
        rdof_u64_to_bits((uint64_t)b, 7, bits + 80);
        int c9 = rdof_crc9_dmr(bits, 87) ^ RDOF_MASK_CRC9_RATE12;
        if (spoil && b == 0)
            c9 ^= 1;
        blk[0] = (uint8_t)(b << 1 | c9 >> 8);
        blk[1] = (uint8_t)c9;
        rdof_data_burst(blk, RDOF_DT_RATE_12, 1, sync, p->bursts[1 + b]);
        p->control[1 + b] = RDOF_SYNC_DATA | RDOF_DT_RATE_12;
    }
    p->nbursts = 1 + blocks;
    return blocks;
}

static void t_confirmed(void)
{
    fake_t f;
    rdof_ipgw_t *g = gw(&f, "{\"ip_gateway\": {\"routes\": [{\"private\": 9990, \"to\": \"192.0.2.20:4005\"}]}}");
    uint8_t d[2048];
    static rdof_ipgw_dmr_t p;
    size_t n = datagram(RADIO_IP, 4005, IP(13, 0, 0, 1), 4005, "ARS registration", d);
    confirmed(RADIO, 9990, 3, d, n, &p, false);
    hear(g, &W, &p, 1.0);
    rdof_data_header_t h;
    rdof_decode_data_header(f.out.bursts[0], &h);
    CHECK(f.sent == 1 && f.last_len == 16 && !memcmp(f.last, "ARS registration", 16), "confirmed: forwarded");
    CHECK(f.dmr == 1 && f.out.response && h.dpf == RDOF_DPF_RESPONSE && h.response_type == 1 &&
              h.response_status == 3 && h.dst_id == RADIO && h.src_id == 9990 && g->n.acks == 1,
          "ACKed: status %d", h.response_status);
    /* the radio didn't hear it and sends again: answered again, not forwarded */
    hear(g, &W, &p, 1.2);
    CHECK(f.dmr == 1 && g->n.rx_dup == 1, "too soon to answer again");
    hear(g, &W, &p, 1.8);
    CHECK(f.dmr == 2 && f.sent == 1 && g->n.acks == 2 && g->n.rx_dup == 2, "answered again");
    /* a block's CRC-9 wrong: nothing */
    n = datagram(RADIO_IP, 4005, IP(13, 0, 0, 1), 4005, "spoiled", d);
    confirmed(RADIO, 9990, 4, d, n, &p, true);
    hear(g, &W, &p, 2.0);
    CHECK(g->n.rx_bad == 1 && f.dmr == 2 && f.sent == 1, "bad CRC-9");
    /* ack off */
    g->cfg.ack = false;
    n = datagram(RADIO_IP, 4005, IP(13, 0, 0, 1), 4005, "no ack", d);
    confirmed(RADIO, 9990, 5, d, n, &p, false);
    hear(g, &W, &p, 3.0);
    CHECK(f.sent == 2 && f.dmr == 2, "ack off");
    /* not ours (no route): no answer */
    g->cfg.ack = true;
    confirmed(RADIO, 4444, 6, d, n, &p, false);
    hear(g, &W, &p, 3.5);
    CHECK(f.dmr == 2 && g->n.no_route == 1, "not ours: not answered");
    free(g);
}

static void t_assembly(void)
{
    fake_t f;
    rdof_ipgw_t *g = gw(&f, "{\"ip_gateway\": {\"nat\": {\"deny\": []}, \"up_pps\": 0}}");
    uint8_t d[2048];
    static rdof_ipgw_dmr_t p;
    uint64_t sync = rdof_data_sync(RDOF_MS_VOICE);
    size_t n = datagram(RADIO_IP, 5000, IP(10, 0, 0, 1), 9000, "a longer payload, three blocks or more", d);

    /* rate 3/4, unconfirmed: 18 octets a block */
    int blocks = (int)((n + 4 + 17) / 18), pad = blocks * 18 - (int)n - 4;
    uint8_t h[12], buf[200], info[196];
    rdof_ipgw_data_header(false, false, false, 4, pad, 9990, RADIO, blocks, 0, h);
    memcpy(buf, d, n);
    memset(buf + n, 0, (size_t)pad);
    uint32_t crc = rdof_crc32_dmr(buf, n + (size_t)pad);
    for (int i = 0; i < 4; i++)
        buf[n + (size_t)pad + (size_t)i] = (uint8_t)(crc >> (8 * i));
    rdof_data_burst(h, RDOF_DT_DATA_HEADER, 2, sync, p.bursts[0]);
    rdof_ipgw_where_t w = {.source = 3, .slot = 2, .freq = 0, .cc = -1};
    rdof_ipgw_burst(g, &w, RDOF_DT_DATA_HEADER, p.bursts[0], 1.0);
    for (int b = 0; b < blocks; b++) {
        rdof_trellis34_encode(buf + 18 * b, info);
        rdof_info_burst(info, RDOF_DT_RATE_34, 2, sync, p.bursts[1]);
        rdof_ipgw_burst(g, &w, RDOF_DT_RATE_34, p.bursts[1], 1.0 + 0.06 * b);
        /* a voice burst or a CSBK on another slot in between changes nothing */
        rdof_ipgw_where_t other = w;
        other.slot = 1;
        rdof_ipgw_burst(g, &other, RDOF_DT_RATE_34, p.bursts[1], 1.0);
        rdof_ipgw_burst(g, &w, RDOF_DT_CSBK, p.bursts[1], 1.0);
    }
    CHECK(f.sent == 1 && f.last_ip == IP(10, 0, 0, 1) && f.last_port == 9000 && g->n.rx_messages == 1,
          "rate 3/4: sent %d", f.sent);
    /* the reply goes where the radio was heard: slot 2, cc 2 from the bursts */
    rdof_ipgw_from_net(g, f.last_flow, IP(10, 0, 0, 1), 9000, (const uint8_t *)"r", 1, 1.5);
    CHECK(f.dmr == 1 && f.out.slot == 2 && f.out.cc == 2 && f.out.freq == 0, "where: slot %d cc %d", f.out.slot,
          f.out.cc);

    /* a header, one block, then another header: the first is incomplete */
    n = datagram(RADIO_IP, 5000, IP(10, 0, 0, 1), 9000, "first message, never finished", d);
    CHECK(rdof_ipgw_build_dmr(RADIO, 9990, 4, d, n, 1, sync, &p), "build");
    rdof_ipgw_burst(g, &W, RDOF_DT_DATA_HEADER, p.bursts[0], 2.0);
    rdof_ipgw_burst(g, &W, RDOF_DT_RATE_12, p.bursts[1], 2.06);
    rdof_ipgw_burst(g, &W, RDOF_DT_DATA_HEADER, p.bursts[0], 2.2);
    CHECK(g->n.rx_incomplete == 1, "a new header");
    rdof_ipgw_tick(g, 3.0);
    CHECK(g->n.rx_incomplete == 1, "not timed out yet");
    rdof_ipgw_tick(g, 4.5);
    CHECK(g->n.rx_incomplete == 2, "timed out");
    /* a block with nothing before it, a bad header CRC, a response header: ignored */
    rdof_ipgw_burst(g, &W, RDOF_DT_RATE_12, p.bursts[1], 5.0);
    uint8_t broken[RDOF_BURST_BYTES];
    memcpy(broken, p.bursts[0], sizeof broken);
    broken[3] ^= 0xFF;
    broken[4] ^= 0xFF;
    rdof_ipgw_burst(g, &W, RDOF_DT_DATA_HEADER, broken, 5.0);
    rdof_ipgw_build_ack(1, 2, 4, 0, 1, sync, &p);
    rdof_ipgw_burst(g, &W, RDOF_DT_DATA_HEADER, p.bursts[0], 5.0);
    CHECK(g->n.rx_messages == 1 && g->n.rx_bad == 0 && g->n.rx_incomplete == 2, "ignored");
    /* the CRC-32 wrong */
    n = datagram(RADIO_IP, 5000, IP(10, 0, 0, 1), 9000, "crc", d);
    CHECK(rdof_ipgw_build_dmr(RADIO, 9990, 4, d, n, 1, sync, &p), "build");
    uint8_t blk[12];
    rdof_bptc196_decode(p.bursts[1], blk);
    blk[0] ^= 1;
    rdof_data_burst(blk, RDOF_DT_RATE_12, 1, sync, p.bursts[1]);
    hear(g, &W, &p, 6.0);
    CHECK(g->n.rx_bad == 1 && g->n.rx_messages == 1, "CRC-32");
    /* five at once on four assemblers: the oldest gives way */
    for (int k = 0; k < 5; k++) {
        rdof_ipgw_where_t wk = {.source = 10 + k, .slot = 1, .cc = 1};
        n = datagram(RADIO_IP, 5000, IP(10, 0, 0, 1), 9000, "parallel", d);
        CHECK(rdof_ipgw_build_dmr(RADIO, 9990, 4, d, n, 1, sync, &p), "build");
        rdof_ipgw_burst(g, &wk, RDOF_DT_DATA_HEADER, p.bursts[0], 7.0 + 0.01 * k);
    }
    CHECK(g->n.rx_incomplete == 3, "the oldest gave way: %u", g->n.rx_incomplete);

    /* a proprietary second header counts as one of the blocks announced */
    rdof_ipgw_tick(g, 20.0);
    uint32_t inc = g->n.rx_incomplete, not_ip = g->n.not_ip;
    uint8_t one[12] = {1, 2, 3, 4, 5, 6, 7, 8};
    crc = rdof_crc32_dmr(one, 8);
    for (int i = 0; i < 4; i++)
        one[8 + i] = (uint8_t)(crc >> (8 * i));
    rdof_ipgw_data_header(false, false, false, 4, 0, 9990, RADIO, 2, 0, h);
    rdof_data_burst(h, RDOF_DT_DATA_HEADER, 1, sync, p.bursts[0]);
    uint8_t prop[12] = {0};
    prop[0] = RDOF_DPF_PROPRIETARY;
    prop[1] = 0x10; /* MFID */
    uint16_t c16 = rdof_crc16_ccitt(prop, 10) ^ RDOF_MASK_DATA_HEADER;
    prop[10] = (uint8_t)(c16 >> 8);
    prop[11] = (uint8_t)c16;
    rdof_data_burst(prop, RDOF_DT_DATA_HEADER, 1, sync, p.bursts[1]);
    rdof_data_burst(one, RDOF_DT_RATE_12, 1, sync, p.bursts[2]);
    rdof_ipgw_burst(g, &W, RDOF_DT_DATA_HEADER, p.bursts[1], 21.0); /* nothing started: ignored */
    for (int i = 0; i < 3; i++)
        rdof_ipgw_burst(g, &W, i < 2 ? RDOF_DT_DATA_HEADER : RDOF_DT_RATE_12, p.bursts[i], 21.0 + 0.06 * i);
    CHECK(g->n.not_ip == not_ip + 1 && g->n.rx_incomplete == inc, "proprietary header: complete (8 octets: not IP)");

    /* 127 rate 3/4 blocks: more than the longest message */
    rdof_ipgw_data_header(false, false, false, 4, 0, 9990, RADIO, 127, 0, h);
    rdof_data_burst(h, RDOF_DT_DATA_HEADER, 1, sync, p.bursts[0]);
    rdof_ipgw_burst(g, &W, RDOF_DT_DATA_HEADER, p.bursts[0], 30.0);
    memset(buf, 0x55, 18);
    rdof_trellis34_encode(buf, info);
    rdof_info_burst(info, RDOF_DT_RATE_34, 1, sync, p.bursts[1]);
    for (int b = 0; b < 127; b++)
        rdof_ipgw_burst(g, &W, RDOF_DT_RATE_34, p.bursts[1], 30.0 + 0.01 * b);
    CHECK(g->n.too_big == 1, "too long to hold");
    free(g);
}

static void t_limits(void)
{
    fake_t f;
    rdof_ipgw_t *g = gw(&f, "{\"ip_gateway\": {\"nat\": true, \"up_pps\": 1, \"down_pps\": 0.5, \"max_bytes\": 100}}");
    uint8_t d[2048], big[200];
    for (int k = 0; k < 4; k++) {
        size_t n = datagram(RADIO_IP, 5000, IP(8, 8, 8, 8), 53, "rate", d);
        d[28] = (uint8_t)k;
        radio_sends(g, RADIO, 9990, false, d, n, 1.0);
    }
    CHECK(f.sent == 2 && g->n.up_rate == 2, "up: a burst of 2, then limited: sent %d", f.sent);
    size_t n = datagram(RADIO_IP, 5000, IP(8, 8, 8, 8), 53, "later", d);
    radio_sends(g, RADIO, 9990, false, d, n, 2.0);
    CHECK(f.sent == 3, "a second on: one more");
    int flow = f.last_flow;
    for (int k = 0; k < 3; k++)
        rdof_ipgw_from_net(g, flow, IP(8, 8, 8, 8), 53, (const uint8_t *)"r", 1, 2.0);
    CHECK(f.dmr == 1 && g->n.down_rate == 2, "down: one (the bucket holds 1)");
    rdof_ipgw_from_net(g, flow, IP(8, 8, 8, 8), 53, (const uint8_t *)"r", 1, 4.0);
    CHECK(f.dmr == 2, "two seconds on: one more");
    memset(big, 'x', sizeof big);
    rdof_ipgw_from_net(g, flow, IP(8, 8, 8, 8), 53, big, 73, 10.0);
    CHECK(g->n.down_too_big == 1 && f.dmr == 2, "down too big");
    rdof_ipgw_from_net(g, flow, IP(8, 8, 8, 8), 53, big, 72, 10.0);
    CHECK(f.dmr == 3, "100 octets fit");
    n = rdof_ipgw_udp_datagram(RADIO_IP, 5000, IP(8, 8, 8, 8), 53, 9, big, 73, d, sizeof d);
    radio_sends(g, RADIO, 9990, false, d, n, 20.0);
    CHECK(g->n.too_big == 1 && f.sent == 3, "up too big");
    /* no transmitter */
    f.no_tx = true;
    rdof_ipgw_from_net(g, flow, IP(8, 8, 8, 8), 53, (const uint8_t *)"r", 1, 30.0);
    CHECK(g->n.no_tx == 1 && g->n.down_packets == 3, "no tx");
    /* the socket refused, a send failed */
    f.no_tx = false;
    f.refuse_send = true;
    n = datagram(RADIO_IP, 5000, IP(8, 8, 8, 8), 53, "fail", d);
    radio_sends(g, RADIO, 9990, false, d, n, 40.0);
    CHECK(g->n.send_failed == 1, "send failed");
    f.refuse_open = true;
    n = datagram(RADIO_IP, 5009, IP(8, 8, 8, 8), 53, "nosock", d);
    radio_sends(g, RADIO, 9990, false, d, n, 50.0);
    CHECK(g->n.flows_full == 1, "no socket");
    /* off: nothing at all */
    g->cfg.on = false;
    radio_sends(g, RADIO, 9990, false, d, n, 60.0);
    rdof_ipgw_from_net(g, flow, IP(8, 8, 8, 8), 53, (const uint8_t *)"r", 1, 60.0);
    CHECK(g->n.rx_messages == 8 && f.dmr == 3, "off: messages %u", g->n.rx_messages);
    free(g);
}

static void t_ip_payload(void)
{
    fake_t f;
    rdof_ipgw_t *g = gw(&f, "{\"ip_gateway\": {\"routes\": [{\"all\": true, \"to\": \"tunnel.example.com:7000\", "
                            "\"payload\": \"ip\"}]}}");
    uint8_t d[2048], back[2048];
    size_t n = datagram(RADIO_IP, 4001, IP(13, 0, 0, 1), 4005, "tunneled", d);
    radio_sends(g, RADIO, 9990, false, d, n, 1.0);
    CHECK(g->n.unresolved == 1 && f.sent == 0, "a name not resolved yet");
    g->cfg.routes[0].ip = IP(198, 51, 100, 7); /* the platform resolved it */
    radio_sends(g, RADIO, 9990, false, d, n, 7.0);
    CHECK(f.sent == 1 && f.last_ip == IP(198, 51, 100, 7) && f.last_port == 7000 && f.last_len == n &&
              f.last[8] == 63 && !memcmp(f.last + 12, d + 12, n - 12),
          "the whole datagram, one hop on");
    /* ICMP goes too: anything IPv4 */
    d[9] = 1;
    radio_sends(g, RADIO, 9990, false, d, n, 7.5);
    CHECK(g->n.bad_ip == 1, "(its checksum is checked)");
    /* a whole datagram back, to another radio by its address */
    size_t m = rdof_ipgw_udp_datagram(IP(13, 0, 0, 1), 4005, IP(12, 0, 0, 77), 4001, 3, (const uint8_t *)"hi", 2, back,
                                      sizeof back);
    rdof_ipgw_from_net(g, f.last_flow, IP(198, 51, 100, 7), 7000, back, m + 5, 8.0); /* the length field rules */
    rdof_data_header_t h;
    uint8_t got[2048];
    int len = unpack(&f.out, &h, got);
    CHECK(f.dmr == 1 && len == (int)m && h.dst_id == 77 && got[8] == 63 && !memcmp(got + 12, back + 12, m - 12),
          "to radio 77: len %d", len);
    rdof_ipgw_from_net(g, f.last_flow, IP(198, 51, 100, 7), 7000, (const uint8_t *)"not ip at all", 13, 9.0);
    CHECK(g->n.down_bad_ip == 1 && f.dmr == 1, "not IPv4");
    /* outside radio_net: the flow's radio */
    m = rdof_ipgw_udp_datagram(IP(13, 0, 0, 1), 4005, IP(10, 0, 0, 1), 4001, 3, (const uint8_t *)"hi", 2, back,
                               sizeof back);
    rdof_ipgw_from_net(g, f.last_flow, IP(198, 51, 100, 7), 7000, back, m, 12.0);
    len = unpack(&f.out, &h, got);
    CHECK(f.dmr == 2 && h.dst_id == RADIO, "the flow's radio");
    /* never a packet from the radio's own address: it would drop it */
    m = rdof_ipgw_udp_datagram(IP(12, 0, 0, 77), 4005, IP(12, 0, 0, 77), 4001, 3, (const uint8_t *)"hi", 2, back,
                               sizeof back);
    rdof_ipgw_from_net(g, f.last_flow, IP(198, 51, 100, 7), 7000, back, m, 13.0);
    m = rdof_ipgw_udp_datagram(RADIO_IP, 4005, IP(10, 0, 0, 1), 4001, 3, (const uint8_t *)"hi", 2, back, sizeof back);
    rdof_ipgw_from_net(g, f.last_flow, IP(198, 51, 100, 7), 7000, back, m, 14.0);
    CHECK(f.dmr == 2 && g->n.down_own_source == 2, "from the radio's own address: %u", g->n.down_own_source);
    free(g);
}

/* The IPv4 header checksum again, after a field changed. */
static void fix_sum(uint8_t *d)
{
    d[10] = d[11] = 0;
    uint32_t s = 0;
    for (int i = 0; i < 20; i += 2)
        s += rd16(d + i);
    while (s >> 16)
        s = (s & 0xFFFF) + (s >> 16);
    d[10] = (uint8_t)(~s >> 8);
    d[11] = (uint8_t)~s;
}

/* A compressed (SAP 3) header and payload: id, SAID / DAID, the port IDs, then the extended headers. */
static size_t sap3(uint8_t daid, uint8_t spid, uint8_t dpid, const uint16_t *ext, int next, const char *payload,
                   uint8_t *out)
{
    out[0] = 0x12;
    out[1] = 0x34;
    out[2] = daid;
    out[3] = spid;
    out[4] = dpid;
    size_t n = 5;
    for (int i = 0; i < next; i++) {
        out[n++] = (uint8_t)(ext[i] >> 8);
        out[n++] = (uint8_t)ext[i];
    }
    memcpy(out + n, payload, strlen(payload));
    return n + strlen(payload);
}

/* The well-known gateway id, and the source check: a radio sends off-net traffic to 9999999 or the board's own id */
static void t_any_gateway(void)
{
    fake_t f;
    rdof_ipgw_t *g = gw(&f, "{\"ip_gateway\": {\"id\": 9990, \"up_pps\": 0, \"down_pps\": 0, \"nat\": true}}");
    uint8_t d[2048], back[2048];
    rdof_data_header_t h;
    size_t n = datagram(IP(12, 0, 0, 2), 6001, IP(203, 0, 113, 6), 7000, "any gw", d);
    radio_sends(g, 2, RDOF_IPGW_ANY_GATEWAY, false, d, n, 1.0);
    CHECK(f.sent == 1 && f.last_ip == IP(203, 0, 113, 6), "to 9999999: NAT");
    rdof_ipgw_from_net(g, f.last_flow, IP(203, 0, 113, 6), 7000, (const uint8_t *)"back", 4, 1.5);
    int len = unpack(&f.out, &h, back);
    CHECK(len == 32 && h.dst_id == 2 && h.src_id == RDOF_IPGW_ANY_GATEWAY && rd32(back + 16) == IP(12, 0, 0, 2),
          "to 9999999: reply from 9999999 (%u)", h.src_id);
    n = datagram(IP(12, 0, 0, 2), 6002, IP(203, 0, 113, 6), 7000, "own id", d);
    radio_sends(g, 2, 9990, false, d, n, 2.0);
    rdof_ipgw_from_net(g, f.last_flow, IP(203, 0, 113, 6), 7000, (const uint8_t *)"back", 4, 2.5);
    len = unpack(&f.out, &h, back);
    CHECK(f.sent == 2 && len == 32 && h.src_id == 9990, "to the board's id: reply from it");

    /* a source in the radio net that isn't the sender's own: dropped; one off it (a host behind the radio): NATed */
    n = datagram(IP(12, 0, 0, 5), 6000, IP(203, 0, 113, 5), 7000, "not mine", d);
    radio_sends(g, 2, 9990, false, d, n, 3.0);
    CHECK(g->n.bad_src == 1 && f.sent == 2, "another radio's source: %u", g->n.bad_src);
    n = datagram(IP(10, 0, 39, 21), 6000, IP(203, 0, 113, 5), 7001, "host", d);
    radio_sends(g, 2, 9990, false, d, n, 3.5);
    CHECK(g->n.bad_src == 1 && f.sent == 3 && f.last_port == 7001, "an off-net source");
    free(g);
}

/* As radios with the RadioFoundry firmware send it: SAP 3 when both addresses are the ids' (12.x.y.z, 225.x.y.z
 * for a talkgroup), SAP 4 with the real destination to the gateway's id (off-net); TTL kept less one. */
static void t_radios(void)
{
    fake_t f;
    rdof_ipgw_t *g = gw(&f, "{\"ip_gateway\": {\"id\": 9990, \"up_pps\": 0, \"down_pps\": 0, \"routes\": ["
                            "{\"private\": 9990, \"from\": 1, \"to\": \"192.0.2.20\"},"
                            "{\"group\": 9, \"to\": \"192.0.2.21:6000\"}], \"nat\": true}}");
    uint8_t d[2048], back[2048];
    static rdof_ipgw_dmr_t p;
    uint64_t sync = rdof_data_sync(RDOF_MS_VOICE);
    rdof_data_header_t h;

    /* SAP 3, TMS (98 both ways) from radio 1 to 9990: rebuilt 12.0.0.1:4007 -> 12.0.39.6:4007 */
    size_t n = sap3(0, 98, 98, NULL, 0, "tms text", d);
    CHECK(rdof_ipgw_build_dmr(1, 9990, RDOF_SAP_UDP_COMPRESSED, d, n, 1, sync, &p), "build");
    hear(g, &W, &p, 1.0);
    CHECK(g->n.sap3 == 1 && f.sent == 1 && f.last_ip == IP(192, 0, 2, 20) && f.last_port == 4007 &&
              f.last_len == 8 && !memcmp(f.last, "tms text", 8) && f.last_ttl == 0,
          "sap 3: sent %d port %u ttl %d", f.sent, f.last_port, f.last_ttl);
    rdof_ipgw_from_net(g, f.last_flow, IP(192, 0, 2, 20), 4007, (const uint8_t *)"ack", 3, 1.5);
    int len = unpack(&f.out, &h, back);
    CHECK(len == 31 && h.sap == RDOF_SAP_IP && h.dst_id == 1 && h.src_id == 9990 && rd32(back + 12) == IP(12, 0, 39, 6) &&
              rd16(back + 20) == 4007 && rd32(back + 16) == IP(12, 0, 0, 1) && rd16(back + 22) == 4007 &&
              back[8] == 64 && sums_ok(back, 31),
          "sap 3 reply: full header, to 12.0.0.1, TTL 64");

    /* the extended headers: both ports, the source only, the destination only */
    const uint16_t both[2] = {40001, 5555}, one[1] = {40002};
    n = sap3(0, 0, 0, both, 2, "x1", d);
    rdof_ipgw_message(g, &W, 1, 9990, false, 3, d, n, 2.0);
    CHECK(f.sent == 2 && f.last_port == 5555, "both extended: %u", f.last_port);
    n = sap3(0, 0, 2, one, 1, "x2", d);
    rdof_ipgw_message(g, &W, 1, 9990, false, 3, d, n, 2.1);
    CHECK(f.sent == 3 && f.last_port == 5017 && g->flows[f.last_flow].inner_port == 40002, "source extended");
    n = sap3(0, 1, 0, one, 1, "x3", d);
    rdof_ipgw_message(g, &W, 1, 9990, false, 3, d, n, 2.2);
    CHECK(f.sent == 4 && f.last_port == 40002 && g->flows[f.last_flow].inner_port == 5016, "destination extended");

    /* a talkgroup: 225.0.0.9 */
    n = sap3(2, 2, 2, NULL, 0, "lip", d);
    rdof_ipgw_message(g, &W, 1, 9, true, 3, d, n, 3.0);
    CHECK(f.sent == 5 && f.last_ip == IP(192, 0, 2, 21) && f.last_port == 6000 &&
              g->flows[f.last_flow].orig_ip == IP(225, 0, 0, 9),
          "group: 225.0.0.9");

    /* a talkgroup's SAP 3 is rebuilt with TTL 1 (a radio's own is 64); with no route, NAT doesn't pass it on */
    CHECK(g->up[8] == 1 && sums_ok(g->up, 28 + 3), "group sap 3: TTL %u", g->up[8]);
    n = sap3(2, 2, 2, NULL, 0, "tg nat", d);
    rdof_ipgw_message(g, &W, 1, 77, true, 3, d, n, 3.5);
    CHECK(f.sent == 5 && g->n.denied + g->n.ttl_expired == 1, "group sap 3 not NATed");
    g->n.denied = g->n.ttl_expired = 0;

    /* what can't be rebuilt */
    uint32_t bad = g->n.bad_ip;
    n = sap3(0, 0x80 | 98, 98, NULL, 0, "op", d); /* opcode bits set */
    rdof_ipgw_message(g, &W, 1, 9990, false, 3, d, n, 4.0);
    n = sap3(0, 5, 98, NULL, 0, "id5", d); /* a port ID we don't know */
    rdof_ipgw_message(g, &W, 1, 9990, false, 3, d, n, 4.1);
    n = sap3(0x10, 98, 98, NULL, 0, "said", d); /* SAID 1 */
    rdof_ipgw_message(g, &W, 1, 9990, false, 3, d, n, 4.2);
    n = sap3(0, 0, 0, one, 1, "", d); /* short of the second extended header */
    rdof_ipgw_message(g, &W, 1, 9990, false, 3, d, 7, 4.3);
    rdof_ipgw_message(g, &W, 1, 9990, false, 3, d, 4, 4.4);
    CHECK(g->n.bad_ip == bad + 5 && f.sent == 5, "refused: %u", g->n.bad_ip - bad);

    /* SAP 3 from radio 2 to radio 3 (no route): NAT sees 12.0.0.3, the radios' own network: denied */
    n = sap3(0, 98, 98, NULL, 0, "radio to radio", d);
    rdof_ipgw_message(g, &W, 2, 3, false, 3, d, n, 5.0);
    CHECK(g->n.denied == 1 && f.sent == 5, "radio to radio");

    /* off-net: radio 2 (no route takes it) to the gateway's id with the real destination; NAT by it */
    n = datagram(IP(12, 0, 0, 2), 6000, IP(203, 0, 113, 5), 7000, "off-net", d);
    radio_sends(g, 2, 9990, false, d, n, 6.0);
    CHECK(f.sent == 6 && f.last_ip == IP(203, 0, 113, 5) && f.last_port == 7000 && f.last_ttl == 63, "off-net NAT");
    rdof_ipgw_from_net(g, f.last_flow, IP(203, 0, 113, 5), 7000, (const uint8_t *)"back", 4, 6.5);
    len = unpack(&f.out, &h, back);
    CHECK(len == 32 && h.dst_id == 2 && h.src_id == 9990 && rd32(back + 16) == IP(12, 0, 0, 2) &&
              rd16(back + 22) == 6000 && rd32(back + 12) == IP(203, 0, 113, 5),
          "off-net reply: to radio 2 at 12.0.0.2, from the gateway's id");


    /* TTL: 1 is not passed on; a host's TTL 9 goes on as 8 */
    n = datagram(IP(12, 0, 0, 2), 6000, IP(203, 0, 113, 5), 7000, "ttl one", d);
    d[8] = 1;
    fix_sum(d);
    radio_sends(g, 2, 9990, false, d, n, 7.0);
    CHECK(g->n.ttl_expired == 1 && f.sent == 6, "TTL 1");
    n = datagram(IP(12, 0, 0, 2), 6000, IP(203, 0, 113, 5), 7000, "ttl nine", d);
    d[8] = 9;
    fix_sum(d);
    radio_sends(g, 2, 9990, false, d, n, 7.5);
    CHECK(f.sent == 7 && f.last_ttl == 8, "TTL 9 -> %d", f.last_ttl);
    /* a radio's own packet to a talkgroup has TTL 1: a udp route ends it here, so it goes (a new datagram) */
    n = datagram(IP(12, 0, 0, 2), 6000, IP(225, 0, 0, 9), 6000, "group ttl one", d);
    d[8] = 1;
    fix_sum(d);
    radio_sends(g, 2, 9, true, d, n, 8.0);
    CHECK(f.sent == 8 && f.last_ip == IP(192, 0, 2, 21) && f.last_port == 6000 && f.last_ttl == 0 &&
              g->n.ttl_expired == 1,
          "group TTL 1 by a udp route: sent %d", f.sent);
    free(g);

    /* payload ip: the datagram goes on one hop down, its checksum redone; and so does what comes back */
    g = gw(&f, "{\"ip_gateway\": {\"down_pps\": 0, \"routes\": [{\"all\": true, \"to\": \"198.51.100.7:7000\", "
               "\"payload\": \"ip\"}]}}");
    n = datagram(RADIO_IP, 4001, IP(13, 0, 0, 1), 4005, "tunnel", d);
    radio_sends(g, RADIO, 9990, false, d, n, 1.0);
    CHECK(f.sent == 1 && f.last_ttl == 0 && f.last[8] == 63 && sums_ok(f.last, n) && !memcmp(f.last + 20, d + 20, n - 20),
          "ip: TTL 63, checksum good");
    size_t m = rdof_ipgw_udp_datagram(IP(13, 0, 0, 1), 4005, RADIO_IP, 4001, 3, (const uint8_t *)"hi", 2, back,
                                      sizeof back);
    rdof_ipgw_from_net(g, f.last_flow, IP(198, 51, 100, 7), 7000, back, m, 2.0);
    uint8_t got[2048];
    len = unpack(&f.out, &h, got);
    CHECK(f.dmr == 1 && len == (int)m && got[8] == 63 && sums_ok(got, m), "down: TTL 63");
    back[8] = 1;
    fix_sum(back);
    rdof_ipgw_from_net(g, f.last_flow, IP(198, 51, 100, 7), 7000, back, m, 3.0);
    CHECK(g->n.down_ttl_expired == 1 && f.dmr == 1, "down: TTL 1");
    back[8] = 30; /* the checksum not redone */
    rdof_ipgw_from_net(g, f.last_flow, IP(198, 51, 100, 7), 7000, back, m, 4.0);
    CHECK(g->n.down_bad_ip == 1 && f.dmr == 1, "down: bad checksum");
    free(g);
}

/* The Internet checksum over n octets folds to 0 (a message with its checksum in it). */
static bool sum_ok(const uint8_t *p, size_t n)
{
    uint32_t s = 0;
    for (size_t i = 0; i + 1 < n; i += 2)
        s += rd16(p + i);
    if (n & 1)
        s += (uint32_t)p[n - 1] << 8;
    while (s >> 16)
        s = (s & 0xFFFF) + (s >> 16);
    return s == 0xFFFF;
}

/* An IPv4 datagram carrying an ICMP message: type, code, the 4 octets after the checksum, then body. */
static size_t icmp_dgram(uint32_t src, uint32_t dst, int ttl, int type, int code, uint32_t rest, const uint8_t *body,
                         size_t bl, uint8_t *out)
{
    size_t n = 28 + bl;
    memset(out, 0, 28);
    out[0] = 0x45;
    out[2] = (uint8_t)(n >> 8);
    out[3] = (uint8_t)n;
    out[5] = 9;
    out[8] = (uint8_t)ttl;
    out[9] = 1;
    rdof_put_be32(out + 12, src);
    rdof_put_be32(out + 16, dst);
    fix_sum(out);
    uint8_t *m = out + 20;
    m[0] = (uint8_t)type;
    m[1] = (uint8_t)code;
    rdof_put_be32(m + 4, rest);
    if (bl)
        memcpy(m + 8, body, bl);
    uint32_t s = 0;
    for (size_t i = 0; i + 1 < 8 + bl; i += 2)
        s += rd16(m + i);
    if ((8 + bl) & 1)
        s += (uint32_t)m[7 + bl] << 8;
    while (s >> 16)
        s = (s & 0xFFFF) + (s >> 16);
    m[2] = (uint8_t)(~s >> 8);
    m[3] = (uint8_t)~s;
    return n;
}

#define BOARD IP(192, 168, 1, 50)
#define ROUTER IP(203, 0, 113, 1)

static void t_icmp(void)
{
    fake_t f;
    rdof_ipgw_t *g = gw(&f, "{\"ip_gateway\": {\"id\": 9990, \"up_pps\": 0, \"down_pps\": 0, \"nat\": true, "
                            "\"routes\": [{\"private\": 5555, \"to\": \"192.0.2.20:5005\"}]}}");
    uint8_t d[2048], net[2048], got[2048];
    rdof_data_header_t h;

    /* an echo request through NAT: our identifier out, the radio's back */
    size_t n = icmp_dgram(RADIO_IP, IP(8, 8, 8, 8), 64, 8, 0, 0x01010001, (const uint8_t *)"ping", 4, d);
    radio_sends(g, RADIO, 9990, false, d, n, 1.0);
    int fi = 0;
    CHECK(f.icmps == 1 && f.icmp_ip == IP(8, 8, 8, 8) && f.icmp_ttl == 63 && f.icmp_len == 12 &&
              rd16(f.icmp + 4) == RDOF_IPGW_ICMP_ID_BASE + fi && rd16(f.icmp + 6) == 1 && sum_ok(f.icmp, 12) &&
              f.opens == 0 && g->n.icmp_up == 1,
          "echo out: id %04x", rd16(f.icmp + 4));
    size_t m = icmp_dgram(IP(8, 8, 8, 8), BOARD, 57, 0, 0, (uint32_t)(RDOF_IPGW_ICMP_ID_BASE + fi) << 16 | 1,
                          (const uint8_t *)"ping", 4, net);
    rdof_ipgw_icmp_from_net(g, net, m, 1.2);
    int len = unpack(&f.out, &h, got);
    CHECK(f.dmr == 1 && len == 32 && h.dst_id == RADIO && h.src_id == 9990 && rd32(got + 12) == IP(8, 8, 8, 8) &&
              rd32(got + 16) == RADIO_IP && got[8] == 56 && got[9] == 1 && sum_ok(got, 20) && got[20] == 0 &&
              rd16(got + 24) == 0x0101 && sum_ok(got + 20, 12) && g->n.icmp_down == 1,
          "echo reply down: len %d", len);
    /* the same from someone else, an identifier that's no flow's, TTL 1 */
    m = icmp_dgram(IP(1, 1, 1, 1), BOARD, 57, 0, 0, (uint32_t)RDOF_IPGW_ICMP_ID_BASE << 16 | 1, NULL, 0, net);
    rdof_ipgw_icmp_from_net(g, net, m, 1.3);
    m = icmp_dgram(IP(8, 8, 8, 8), BOARD, 57, 0, 0, (uint32_t)(RDOF_IPGW_ICMP_ID_BASE + 9) << 16, NULL, 0, net);
    rdof_ipgw_icmp_from_net(g, net, m, 1.4);
    m = icmp_dgram(IP(8, 8, 8, 8), BOARD, 1, 0, 0, (uint32_t)RDOF_IPGW_ICMP_ID_BASE << 16 | 1, NULL, 0, net);
    rdof_ipgw_icmp_from_net(g, net, m, 1.5);
    m = icmp_dgram(IP(8, 8, 8, 8), BOARD, 57, 8, 0, 0, NULL, 0, net); /* a request to the board: lwIP's */
    rdof_ipgw_icmp_from_net(g, net, m, 1.6);
    rdof_ipgw_icmp_from_net(g, net, 10, 1.7);
    CHECK(f.dmr == 1 && g->n.down_stranger == 1 && g->n.icmp_unmatched == 1 && g->n.down_ttl_expired == 1 &&
              g->n.down_bad_ip == 1,
          "echo replies refused");

    /* up: TTL 1, denied, ICMP that isn't an echo request */
    n = icmp_dgram(RADIO_IP, IP(8, 8, 8, 8), 1, 8, 0, 0x02020001, NULL, 0, d);
    radio_sends(g, RADIO, 9990, false, d, n, 2.0);
    n = icmp_dgram(RADIO_IP, IP(10, 0, 0, 1), 64, 8, 0, 0x03030001, NULL, 0, d);
    radio_sends(g, RADIO, 9990, false, d, n, 2.1);
    n = icmp_dgram(RADIO_IP, IP(8, 8, 8, 8), 64, 0, 0, 0x04040001, NULL, 0, d);
    radio_sends(g, RADIO, 9990, false, d, n, 2.2);
    CHECK(f.icmps == 1 && g->n.ttl_expired == 1 && g->n.denied == 1 && g->n.not_udp == 1, "echo up refused");

    /* port unreachable for a UDP flow: the quote back to the radio's addresses and port, both checksums right */
    uint8_t radio_dg[64], board_dg[64];
    size_t rn = rdof_ipgw_udp_datagram(RADIO_IP, 5000, IP(8, 8, 8, 8), 53, 1, (const uint8_t *)"q", 1, radio_dg, 64);
    radio_sends(g, RADIO, 9990, false, radio_dg, rn, 3.0);
    int uf = f.last_flow;
    rdof_ipgw_udp_datagram(BOARD, (uint16_t)(40000 + uf), IP(8, 8, 8, 8), 53, 77, (const uint8_t *)"q", 1, board_dg,
                           64);
    m = icmp_dgram(ROUTER, BOARD, 250, 3, 3, 0, board_dg, 28, net);
    rdof_ipgw_icmp_from_net(g, net, m, 3.1);
    len = unpack(&f.out, &h, got);
    const uint8_t *q = got + 28;
    CHECK(f.dmr == 2 && len == 56 && h.dst_id == RADIO && rd32(got + 12) == ROUTER && rd32(got + 16) == RADIO_IP &&
              got[8] == 249 && sum_ok(got, 20) && got[20] == 3 && got[21] == 3 && sum_ok(got + 20, 36) &&
              rd32(q + 12) == RADIO_IP && rd32(q + 16) == IP(8, 8, 8, 8) && rd16(q + 20) == 5000 &&
              rd16(q + 22) == 53 && sum_ok(q, 20) && rd16(q + 26) == rd16(radio_dg + 26) && g->n.icmp_errors == 1,
          "port unreachable: quoted udp sum %04x want %04x", rd16(q + 26), rd16(radio_dg + 26));
    /* fragmentation needed for the echo flow: the quoted echo back to the radio's identifier and checksum */
    uint8_t echo_out[64];
    icmp_dgram(BOARD, IP(8, 8, 8, 8), 63, 8, 0, (uint32_t)(RDOF_IPGW_ICMP_ID_BASE + fi) << 16 | 1,
               (const uint8_t *)"ping", 4, echo_out);
    m = icmp_dgram(ROUTER, BOARD, 250, 3, 4, 576, echo_out, 28, net);
    rdof_ipgw_icmp_from_net(g, net, m, 3.2);
    len = unpack(&f.out, &h, got);
    icmp_dgram(RADIO_IP, IP(8, 8, 8, 8), 63, 8, 0, 0x01010001, (const uint8_t *)"ping", 4, radio_dg);
    CHECK(f.dmr == 3 && len == 56 && rd32(q + 12) == RADIO_IP && rd16(q + 24) == 0x0101 &&
              rd16(q + 22) == rd16(radio_dg + 22) && sum_ok(q, 20) && sum_ok(got + 20, 36),
          "frag needed for an echo");
    /* time exceeded for the route's flow: the quote back to where the radio sent (13.0.0.1:4005) */
    rn = rdof_ipgw_udp_datagram(RADIO_IP, 4001, IP(13, 0, 0, 1), 4005, 1, (const uint8_t *)"r", 1, radio_dg, 64);
    radio_sends(g, RADIO, 5555, false, radio_dg, rn, 4.0);
    rdof_ipgw_udp_datagram(BOARD, (uint16_t)(40000 + f.last_flow), IP(192, 0, 2, 20), 5005, 78,
                           (const uint8_t *)"r", 1, board_dg, 64);
    m = icmp_dgram(ROUTER, BOARD, 250, 11, 0, 0, board_dg, 28, net);
    rdof_ipgw_icmp_from_net(g, net, m, 4.1);
    len = unpack(&f.out, &h, got);
    CHECK(f.dmr == 4 && h.src_id == 5555 && rd32(q + 16) == IP(13, 0, 0, 1) && rd16(q + 22) == 4005 &&
              rd16(q + 20) == 4001 && rd16(q + 26) == rd16(radio_dg + 26),
          "time exceeded on a route");
    /* an error quoting a port that's no flow's, and a short quote */
    board_dg[20] ^= 1;
    m = icmp_dgram(ROUTER, BOARD, 250, 3, 3, 0, board_dg, 28, net);
    rdof_ipgw_icmp_from_net(g, net, m, 5.0);
    m = icmp_dgram(ROUTER, BOARD, 250, 3, 3, 0, board_dg, 20, net);
    rdof_ipgw_icmp_from_net(g, net, m, 5.1);
    CHECK(f.dmr == 4 && g->n.icmp_unmatched == 3, "unmatched errors: %u", g->n.icmp_unmatched);
    /* idle: the echo flow closes without a socket to close */
    int closes = f.closes;
    rdof_ipgw_tick(g, 200.0);
    CHECK(rdof_ipgw_flows(g) == 0 && f.closes == closes + 2, "closed: %d", f.closes - closes);
    free(g);
}

#endif /* RDOF_FEATURE_DATA_BLOCKS */

void test_ipgw(void)
{
    t_settings();
    t_addresses();
#if RDOF_FEATURE_DATA_BLOCKS
    t_packets();
    t_route();
    t_nat();
    t_confirmed();
    t_assembly();
    t_limits();
    t_ip_payload();
    t_radios();
    t_any_gateway();
    t_icmp();
#endif
}
