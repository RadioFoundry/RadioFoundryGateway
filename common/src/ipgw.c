// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The IP gateway (rdof/ipgw.h): DMR packet data put back together, relayed to the network by route or by NAT,
 * and replies sent back down as packet data. Message format: ETSI TS 102 361-1 clause 9.2 (data header, blocks,
 * the CRC-9 and CRC-32) and 361-3 (SAP 4 carries IPv4); IPv4 and UDP: RFC 791 and RFC 768.
 */
#include "rdof/ipgw.h"

#if RDOF_FEATURE_DATA_BLOCKS

#include <string.h>

#include "rdof/bits.h"
#include "rdof/layer2.h"
#include "rdof/mmdvm.h"
#include "rdof/pdu.h"

#define IP_UDP 17
#define IP_ICMP 1
#define FSN_LAST 0x8 /* the fragment sequence number of a message sent whole */
#define ICMP_ECHO_REPLY 0
#define ICMP_UNREACHABLE 3 /* fragmentation needed is code 4 of it */
#define ICMP_ECHO 8
#define ICMP_TIME_EXCEEDED 11
#define ICMP_PARAMETER 12

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v >> 16);
    put16(p + 2, v);
}

/* The Internet checksum's running sum (RFC 1071). */
static uint32_t sum16(uint32_t s, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i + 1 < n; i += 2)
        s += be16(p + i);
    if (n & 1)
        s += (uint32_t)p[n - 1] << 8;
    return s;
}

static uint16_t fold(uint32_t s)
{
    while (s >> 16)
        s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}

static uint32_t fnv(uint32_t h, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

static uint32_t message_hash(uint32_t src, uint32_t dst, const uint8_t *data, size_t len)
{
    uint8_t ids[6];
    rdof_put_be24(ids, src);
    rdof_put_be24(ids + 3, dst);
    return fnv(fnv(2166136261u, ids, 6), data, len);
}

/* A token bucket of twice rate (at least one); rate 0: no limit. */
static bool take(double *tokens, double *at, double rate, double now)
{
    if (rate <= 0)
        return true;
    double cap = rate * 2 < 1 ? 1 : rate * 2;
    if (now > *at)
        *tokens += (now - *at) * rate;
    if (*tokens > cap)
        *tokens = cap;
    *at = now;
    if (*tokens < 1)
        return false;
    *tokens -= 1;
    return true;
}

void rdof_ipgw_init(rdof_ipgw_t *g, const rdof_ipgw_config_t *cfg)
{
    memset(g, 0, sizeof *g);
    g->cfg = *cfg;
    g->sync48 = rdof_data_sync(RDOF_MS_VOICE);
    g->up_tokens = cfg->up_pps * 2 < 1 ? 1 : cfg->up_pps * 2;
    g->down_tokens = cfg->down_pps * 2 < 1 ? 1 : cfg->down_pps * 2;
}

/* ── building packets ──────────────────────────────────────────────────────── */

void rdof_ipgw_data_header(bool group, bool confirmed, bool response_requested, int sap, int pad, uint32_t dst,
                           uint32_t src, int blocks, int ns, uint8_t d[12])
{
    memset(d, 0, 12);
    d[0] = (uint8_t)((group ? 0x80 : 0) | (response_requested ? 0x40 : 0) | ((pad & 0x10) ? 0x10 : 0) |
                     (confirmed ? RDOF_DPF_CONFIRMED : RDOF_DPF_UNCONFIRMED));
    d[1] = (uint8_t)((sap & 0x0F) << 4 | (pad & 0x0F));
    rdof_put_be24(d + 2, dst);
    rdof_put_be24(d + 5, src);
    d[8] = (uint8_t)(0x80 | (blocks & 0x7F)); /* F: the whole message */
    d[9] = (uint8_t)((confirmed ? (ns & 7) << 4 : 0) | FSN_LAST);
    put16(d + 10, rdof_crc16_ccitt(d, 10) ^ RDOF_MASK_DATA_HEADER);
}

bool rdof_ipgw_build_dmr(uint32_t src, uint32_t dst, int sap, const uint8_t *data, size_t len, int cc,
                         uint64_t sync48, rdof_ipgw_dmr_t *out)
{
    size_t total = len + 4; /* the CRC-32 ends the last block */
    int blocks = (int)((total + 11) / 12);
    if (len > RDOF_IPGW_MAX_PACKET || blocks > 127 || blocks + 1 > RDOF_IPGW_MAX_BURSTS)
        return false;
    int pad = blocks * 12 - (int)total;
    memset(out, 0, sizeof *out - sizeof out->bursts);
    out->dst = dst;
    out->src = src;
    out->cc = cc;
    uint8_t d[12];
    rdof_ipgw_data_header(false, false, false, sap, pad, dst, src, blocks, 0, d);
    rdof_data_burst(d, RDOF_DT_DATA_HEADER, cc, sync48, out->bursts[0]);
    out->control[0] = RDOF_SYNC_DATA | RDOF_DT_DATA_HEADER;
    /* user data, pad octets of zero, the CRC-32 over both (little endian, as rdof_check_crc32 reads it) */
    uint8_t buf[127 * 12];
    memcpy(buf, data, len);
    memset(buf + len, 0, (size_t)pad);
    uint32_t crc = rdof_crc32_dmr(buf, len + (size_t)pad);
    for (int i = 0; i < 4; i++)
        buf[len + (size_t)pad + (size_t)i] = (uint8_t)(crc >> (8 * i));
    for (int b = 0; b < blocks; b++) {
        rdof_data_burst(buf + 12 * b, RDOF_DT_RATE_12, cc, sync48, out->bursts[1 + b]);
        out->control[1 + b] = RDOF_SYNC_DATA | RDOF_DT_RATE_12;
    }
    out->nbursts = 1 + blocks;
    return true;
}

void rdof_ipgw_build_ack(uint32_t src, uint32_t dst, int sap, int ns, int cc, uint64_t sync48, rdof_ipgw_dmr_t *out)
{
    memset(out, 0, sizeof *out - sizeof out->bursts);
    out->dst = dst;
    out->src = src;
    out->cc = cc;
    out->response = true;
    uint8_t d[12] = {0};
    d[0] = RDOF_DPF_RESPONSE;
    d[1] = (uint8_t)((sap & 0x0F) << 4);
    rdof_put_be24(d + 2, dst);
    rdof_put_be24(d + 5, src);
    d[9] = (uint8_t)(0 << 6 | 1 << 3 | (ns & 7)); /* class 0, type 1: ACK; status: the message's N(S) */
    put16(d + 10, rdof_crc16_ccitt(d, 10) ^ RDOF_MASK_DATA_HEADER);
    rdof_data_burst(d, RDOF_DT_DATA_HEADER, cc, sync48, out->bursts[0]);
    out->control[0] = RDOF_SYNC_DATA | RDOF_DT_DATA_HEADER;
    out->nbursts = 1;
}

size_t rdof_ipgw_udp_datagram(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, uint16_t id,
                              const uint8_t *payload, size_t len, uint8_t *out, size_t cap)
{
    size_t total = 28 + len;
    if (total > cap || total > 0xFFFF)
        return 0;
    uint8_t *ip = out, *udp = out + 20;
    memset(ip, 0, 28);
    ip[0] = 0x45;
    put16(ip + 2, (uint32_t)total);
    put16(ip + 4, id);
    ip[8] = 64; /* TTL */
    ip[9] = IP_UDP;
    put32(ip + 12, src);
    put32(ip + 16, dst);
    put16(ip + 10, fold(sum16(0, ip, 20)));
    put16(udp, sport);
    put16(udp + 2, dport);
    put16(udp + 4, (uint32_t)(8 + len));
    memmove(udp + 8, payload, len);
    uint32_t s = sum16(0, ip + 12, 8) + IP_UDP + (uint32_t)(8 + len); /* the pseudo-header */
    uint16_t c = fold(sum16(s, udp, 8 + len));
    put16(udp + 6, c ? c : 0xFFFF);
    return total;
}

/* ── flows ─────────────────────────────────────────────────────────────────── */

int rdof_ipgw_flows(const rdof_ipgw_t *g)
{
    int n = 0;
    for (int i = 0; i < RDOF_IPGW_MAX_FLOWS; i++)
        n += g->flows[i].used;
    return n;
}

static void close_flow(rdof_ipgw_t *g, int i)
{
    g->flows[i].used = false;
    if (g->flows[i].proto != IP_ICMP && g->close)
        g->close(g->ctx, i);
}

/* The flow for this key, opened when there's none; -1: none free, or no socket. */
static int flow_for(rdof_ipgw_t *g, const rdof_ipgw_flow_t *key, double now)
{
    int free_i = -1;
    int max = g->cfg.max_flows < RDOF_IPGW_MAX_FLOWS ? g->cfg.max_flows : RDOF_IPGW_MAX_FLOWS;
    for (int i = 0; i < max; i++) {
        const rdof_ipgw_flow_t *f = &g->flows[i];
        if (!f->used) {
            if (free_i < 0)
                free_i = i;
            continue;
        }
        if (f->route == key->route && f->proto == key->proto && f->radio == key->radio && f->inner_ip == key->inner_ip &&
            f->inner_port == key->inner_port && f->peer_ip == key->peer_ip && f->peer_port == key->peer_port &&
            f->orig_ip == key->orig_ip && f->orig_port == key->orig_port)
            return i;
    }
    if (free_i < 0)
        return -1;
    if (key->proto != IP_ICMP && g->open && !g->open(g->ctx, free_i))
        return -1; /* an echo flow needs no socket of its own: the platform's one ICMP socket */
    g->flows[free_i] = *key;
    g->flows[free_i].used = true;
    g->flows[free_i].opened = now;
    g->flows[free_i].up = g->flows[free_i].down = 0;
    g->n.flows_opened++;
    return free_i;
}

/* ── radio to network ──────────────────────────────────────────────────────── */

static int match_route(const rdof_ipgw_t *g, uint32_t src, uint32_t dst, bool group)
{
    for (int i = 0; i < g->cfg.nroutes; i++) {
        const rdof_ipgw_route_t *r = &g->cfg.routes[i];
        if (r->all)
            return i;
        if (r->dst_kind != RDOF_IPGW_DST_ANY &&
            (group != (r->dst_kind == RDOF_IPGW_DST_GROUP) || dst < r->dst_lo || dst > r->dst_hi))
            continue;
        if (r->has_from && (src < r->from_lo || src > r->from_hi))
            continue;
        return i;
    }
    return -1;
}

/* A port from a compressed header's 7-bit port ID (ETSI TS 102 361-3 table 7.14 as radios use it); 0: unknown. */
static uint16_t port_of(int id)
{
    return id == 1 ? 5016 : id == 2 ? 5017 : id == 98 ? 4007 : 0;
}

/* SAP 3: the compressed UDP/IPv4 header, expanded to a whole datagram in g->up. The addresses come from the DMR
 * ids: the source radio's in radio_net, the destination's too, or 225.x.y.z for a talkgroup. 0 when it can't be. */
static size_t expand_sap3(rdof_ipgw_t *g, uint32_t src, uint32_t dst, bool group, const uint8_t *d, size_t len)
{
    if (len < 5)
        return 0;
    int said = d[2] >> 4, daid = d[2] & 0x0F, opcode = (d[3] >> 7) << 1 | d[4] >> 7;
    int spid = d[3] & 0x7F, dpid = d[4] & 0x7F;
    if (opcode != 0 || said != 0 || (daid != 0 && daid != 2))
        return 0;
    size_t p = 5;
    uint16_t sport = port_of(spid), dport = port_of(dpid);
    if (!spid && !dpid) {
        if (len < 9)
            return 0;
        sport = be16(d + 5);
        dport = be16(d + 7);
        p = 9;
    } else if (!spid || !dpid) {
        if (len < 7)
            return 0;
        if (!spid)
            sport = be16(d + 5);
        else
            dport = be16(d + 5);
        p = 7;
    }
    if (!sport || !dport)
        return 0;
    uint32_t sip = rdof_ipgw_radio_ip(&g->cfg.radio_net, src);
    uint32_t dip = group || daid == 2 ? RDOF_IPGW_GROUP_NET | (dst & 0xFFFFFF)
                                      : rdof_ipgw_radio_ip(&g->cfg.radio_net, dst);
    size_t n = rdof_ipgw_udp_datagram(sip, sport, dip, dport, be16(d), d + p, len - p, g->up, sizeof g->up);
    if (n && (group || daid == 2)) { /* a talkgroup's TTL is 1 on the air: it stops at the first router */
        g->up[8] = 1;
        g->up[10] = g->up[11] = 0;
        put16(g->up + 10, fold(sum16(0, g->up, 20)));
    }
    return n;
}

/* One datagram from a radio. Returns whether it's the gateway's to answer for (a route took it, or NAT did). */
static bool relay_up(rdof_ipgw_t *g, const rdof_ipgw_where_t *w, uint32_t src, uint32_t dst, bool group, int sap,
                     const uint8_t *d, size_t len, double now)
{
    if (sap == RDOF_SAP_UDP_COMPRESSED) {
        len = expand_sap3(g, src, dst, group, d, len);
        if (!len) {
            g->n.bad_ip++;
            return false;
        }
        d = g->up;
        g->n.sap3++;
    } else if (sap != RDOF_SAP_IP || len < 20 || d[0] >> 4 != 4) {
        g->n.not_ip++;
        return false;
    }
    size_t ihl = (size_t)(d[0] & 0x0F) * 4, total = be16(d + 2);
    if (ihl < 20 || total < ihl || total > len || fold(sum16(0, d, ihl)) != 0) {
        g->n.bad_ip++;
        return false;
    }
    len = total; /* past it: the message's own padding */
    if ((be16(d + 6) & 0x3FFF) != 0) { /* MF set, or an offset */
        g->n.fragment++;
        return false;
    }
    if (len > (size_t)g->cfg.max_bytes) {
        g->n.too_big++;
        return false;
    }
    int proto = d[9];
    uint32_t sip = be32(d + 12), dip = be32(d + 16);
    /* A source in radio_net is a radio's: only the sender's own (any other source is a host off the radio net) */
    if (rdof_ipgw_in_net(&g->cfg.radio_net, sip) && sip != rdof_ipgw_radio_ip(&g->cfg.radio_net, src)) {
        g->n.bad_src++;
        return false;
    }
    const uint8_t *udp = d + ihl;
    size_t ulen = 0;
    if (proto == IP_UDP) {
        ulen = len - ihl >= 8 ? be16(udp + 4) : 0;
        if (ulen < 8 || ulen > len - ihl) {
            g->n.bad_ip++;
            return false;
        }
    }
    rdof_ipgw_flow_t key = {0};
    key.radio = src;
    key.dmr_dst = dst;
    key.group = group;
    key.inner_ip = sip;
    key.orig_ip = dip;
    key.where = *w;
    int r = match_route(g, src, dst, group);
    key.route = r;
    if (r >= 0) {
        const rdof_ipgw_route_t *rt = &g->cfg.routes[r];
        key.payload = rt->payload;
        if (rt->payload == RDOF_IPGW_PAYLOAD_UDP) {
            if (proto != IP_UDP) {
                g->n.not_udp++;
                return true;
            }
            key.proto = IP_UDP;
            key.inner_port = be16(udp);
            key.orig_port = be16(udp + 2);
            key.peer_port = (uint16_t)(rt->port ? rt->port : key.orig_port);
        } else {
            key.peer_port = (uint16_t)rt->port;
        }
        key.peer_ip = rt->ip;
        if (!key.peer_ip) {
            g->n.unresolved++;
            return true;
        }
    } else if (g->cfg.nat) {
        /* UDP, and ICMP echo requests (the only ICMP a host behind a radio starts) */
        bool echo = proto == IP_ICMP && len - ihl >= 8 && udp[0] == ICMP_ECHO && udp[1] == 0;
        if (proto != IP_UDP && !echo) {
            g->n.not_udp++;
            return false;
        }
        if (!rdof_ipgw_nat_allowed(&g->cfg, dip)) {
            g->n.denied++;
            return false;
        }
        key.payload = RDOF_IPGW_PAYLOAD_UDP;
        key.proto = (uint8_t)proto;
        key.peer_ip = dip;
        if (echo) {
            key.inner_port = be16(udp + 4); /* the echo identifier stands for the port */
        } else {
            key.inner_port = be16(udp);
            key.orig_port = key.peer_port = be16(udp + 2);
        }
    } else {
        g->n.no_route++;
        return false;
    }
    if (!key.peer_port && key.proto != IP_ICMP) {
        g->n.bad_ip++;
        return true;
    }
    /* a router doesn't pass these on; a udp route ends the datagram here (a radio's own to a talkgroup has TTL 1) */
    if (d[8] <= 1 && !(r >= 0 && key.payload == RDOF_IPGW_PAYLOAD_UDP)) {
        g->n.ttl_expired++;
        return true;
    }
    if (!take(&g->up_tokens, &g->up_at, g->cfg.up_pps, now)) {
        g->n.up_rate++;
        return true;
    }
    int fi = flow_for(g, &key, now);
    if (fi < 0) {
        g->n.flows_full++;
        return true;
    }
    rdof_ipgw_flow_t *f = &g->flows[fi];
    f->last = now;
    f->where = *w;
    if (key.proto == IP_ICMP) { /* the echo identifier ours, the checksum again */
        size_t il = len - ihl;
        memmove(g->up, udp, il);
        put16(g->up + 4, (uint32_t)(RDOF_IPGW_ICMP_ID_BASE + fi));
        g->up[2] = g->up[3] = 0;
        put16(g->up + 2, fold(sum16(0, g->up, il)));
        if (g->send_icmp && g->send_icmp(g->ctx, f->peer_ip, g->up, il, d[8] - 1)) {
            f->up++;
            g->n.icmp_up++;
            g->n.up_packets++;
            g->n.up_bytes += (uint32_t)il;
        } else {
            g->n.send_failed++;
        }
        return true;
    }
    const uint8_t *out = udp + 8;
    size_t n = ulen - 8;
    if (key.payload == RDOF_IPGW_PAYLOAD_IP) { /* the whole datagram, one hop on */
        if (d != g->up)
            memcpy(g->up, d, len);
        g->up[8]--;
        g->up[10] = g->up[11] = 0;
        put16(g->up + 10, fold(sum16(0, g->up, ihl)));
        out = g->up;
        n = len;
    }
    /* NAT: the payload goes from our socket with the datagram's TTL less one; a route's is a datagram of the
     * gateway's own (0: the platform's default), or the whole datagram as it is */
    int ttl = r < 0 ? d[8] - 1 : 0;
    if (g->send_udp && g->send_udp(g->ctx, fi, f->peer_ip, f->peer_port, out, n, ttl)) {
        f->up++;
        g->n.up_packets++;
        g->n.up_bytes += (uint32_t)n;
    } else {
        g->n.send_failed++;
    }
    return true;
}

static void transmit(rdof_ipgw_t *g, const rdof_ipgw_where_t *w)
{
    g->out.slot = w->slot;
    g->out.freq = w->freq;
    if (!g->send_dmr || !g->send_dmr(g->ctx, &g->out))
        g->n.no_tx++;
}

typedef struct {
    bool confirmed, wants_ack;
    int ns;
} ack_t;

static void deliver(rdof_ipgw_t *g, const rdof_ipgw_where_t *w, uint32_t src, uint32_t dst, bool group, int sap,
                    const uint8_t *data, size_t len, const ack_t *ack, double now)
{
    uint32_t h = message_hash(src, dst, data, len);
    bool want_ack = ack && ack->wants_ack && g->cfg.ack && !group;
    for (int i = 0; i < RDOF_IPGW_DEDUP; i++) {
        if (g->dedup[i].hash != h || now - g->dedup[i].at > RDOF_IPGW_DEDUP_S || g->dedup[i].at == 0)
            continue;
        g->n.rx_dup++;
        /* the radio sent it again: our answer may not have got there */
        if (want_ack && g->dedup[i].acked > 0 && now - g->dedup[i].acked >= RDOF_IPGW_ACK_AGAIN_S) {
            g->dedup[i].acked = now;
            rdof_ipgw_build_ack(dst, src, sap, ack->ns, w->cc, g->sync48, &g->out);
            g->n.acks++;
            transmit(g, w);
        }
        return;
    }
    int k = g->dedup_next;
    g->dedup_next = (k + 1) % RDOF_IPGW_DEDUP;
    g->dedup[k].hash = h;
    g->dedup[k].at = now > 0 ? now : 1e-9;
    g->dedup[k].acked = 0;
    g->n.rx_messages++;
    bool ours = relay_up(g, w, src, dst, group, sap, data, len, now);
    if (ours && want_ack) {
        g->dedup[k].acked = now > 0 ? now : 1e-9;
        rdof_ipgw_build_ack(dst, src, sap, ack->ns, w->cc, g->sync48, &g->out);
        g->n.acks++;
        transmit(g, w);
    }
}

void rdof_ipgw_message(rdof_ipgw_t *g, const rdof_ipgw_where_t *w, uint32_t src, uint32_t dst, bool group, int sap,
                       const uint8_t *data, size_t len, double now)
{
    if (!g->cfg.on)
        return;
    deliver(g, w, src, dst, group, sap, data, len, NULL, now);
}

/* ── putting messages together ─────────────────────────────────────────────── */

static rdof_ipgw_asm_t *find_asm(rdof_ipgw_t *g, int source, int slot)
{
    for (int i = 0; i < RDOF_IPGW_MAX_ASM; i++)
        if (g->asms[i].used && g->asms[i].source == source && g->asms[i].slot == slot)
            return &g->asms[i];
    return NULL;
}

static void finish(rdof_ipgw_t *g, rdof_ipgw_asm_t *a, double now)
{
    a->used = false;
    if (a->overflow) {
        g->n.too_big++;
        return;
    }
    int user = a->len - 4 - a->pad;
    if (a->bad || user < 0 || !rdof_check_crc32(a->data, (size_t)a->len)) {
        g->n.rx_bad++;
        return;
    }
    ack_t ack = {a->confirmed, a->confirmed && a->response_requested, a->ns};
    deliver(g, &a->where, a->src, a->dst, a->group, a->sap, a->data, (size_t)user, &ack, now);
}

static void append(rdof_ipgw_asm_t *a, const uint8_t *p, int n)
{
    if (a->len + n > (int)sizeof a->data) {
        a->overflow = true;
        return;
    }
    memcpy(a->data + a->len, p, (size_t)n);
    a->len += n;
}

void rdof_ipgw_burst(rdof_ipgw_t *g, const rdof_ipgw_where_t *w, int dt, const uint8_t burst[RDOF_BURST_BYTES],
                     double now)
{
    if (!g->cfg.on)
        return;
    rdof_ipgw_asm_t *a = find_asm(g, w->source, w->slot);
    if (dt == RDOF_DT_DATA_HEADER) {
        rdof_data_header_t h;
        rdof_decode_data_header(burst, &h);
        if (!h.crc_ok)
            return;
        if (h.dpf == RDOF_DPF_PROPRIETARY) { /* a second header: one of the blocks announced */
            if (a && ++a->got >= a->expect)
                finish(g, a, now);
            else if (a)
                a->last = now;
            return;
        }
        if (a) { /* a new header with blocks still to come */
            g->n.rx_incomplete++;
            a->used = false;
        }
        if ((h.dpf != RDOF_DPF_UNCONFIRMED && h.dpf != RDOF_DPF_CONFIRMED) || h.blocks <= 0)
            return; /* responses, short data, UDT: not IP */
        int k = 0;
        for (int i = 0; i < RDOF_IPGW_MAX_ASM; i++) {
            if (!g->asms[i].used) {
                k = i;
                break;
            }
            if (g->asms[i].last < g->asms[k].last)
                k = i;
        }
        if (g->asms[k].used)
            g->n.rx_incomplete++; /* the oldest gives way */
        a = &g->asms[k];
        memset(a, 0, offsetof(rdof_ipgw_asm_t, data));
        a->used = true;
        a->source = w->source;
        a->slot = w->slot;
        a->where = *w;
        if (a->where.cc < 0) { /* the channel's color code, from the header's slot type */
            int cc, sdt, errs;
            a->where.cc = rdof_decode_slot_type(burst, &cc, &sdt, &errs) ? cc : 0;
        }
        a->src = h.src_id;
        a->dst = h.dst_id;
        a->group = h.group;
        a->confirmed = h.dpf == RDOF_DPF_CONFIRMED;
        a->response_requested = h.response_requested;
        a->sap = h.sap;
        a->pad = h.pad_octets;
        a->ns = h.ns < 0 ? 0 : h.ns;
        a->expect = h.blocks;
        a->last = now;
        return;
    }
    if (!a || (dt != RDOF_DT_RATE_12 && dt != RDOF_DT_RATE_34 && dt != RDOF_DT_RATE_1))
        return;
    rdof_data_block_t b;
    rdof_decode_data_block(burst, dt, &b);
    if (!b.fec_ok)
        a->bad = true;
    if (a->confirmed) {
        int dbsn, n;
        uint8_t body[24];
        if (!rdof_split_confirmed(&b, &dbsn, body, &n))
            a->bad = true;
        append(a, body, n);
    } else {
        append(a, b.payload, b.len);
    }
    a->last = now;
    if (++a->got >= a->expect)
        finish(g, a, now);
}

/* ── network to radio ──────────────────────────────────────────────────────── */

/* d (n octets, in g->scratch) down to radio on flow f: the TTL, the rate, the size, then the packet. */
static void send_down(rdof_ipgw_t *g, rdof_ipgw_flow_t *f, uint32_t radio, uint8_t *d, size_t n, double now)
{
    if (n > (size_t)g->cfg.max_bytes) {
        g->n.down_too_big++;
        return;
    }
    if (n >= 20) { /* a radio drops a packet from its own address (or its host's): never send one */
        uint32_t src = (uint32_t)d[12] << 24 | (uint32_t)d[13] << 16 | (uint32_t)d[14] << 8 | d[15];
        uint32_t dst = (uint32_t)d[16] << 24 | (uint32_t)d[17] << 16 | (uint32_t)d[18] << 8 | d[19];
        if (src == dst || src == f->inner_ip || src == rdof_ipgw_radio_ip(&g->cfg.radio_net, radio)) {
            g->n.down_own_source++;
            return;
        }
    }
    if (!take(&g->down_tokens, &g->down_at, g->cfg.down_pps, now)) {
        g->n.down_rate++;
        return;
    }
    g->ip_id++;
    f->last = now;
    /* from the id the radio sent to; talkgroup data is answered from the gateway's own id */
    uint32_t from = f->group && g->cfg.id ? g->cfg.id : f->dmr_dst;
    if (!rdof_ipgw_build_dmr(from, radio, RDOF_SAP_IP, d, n, f->where.cc < 0 ? 0 : f->where.cc, g->sync48, &g->out)) {
        g->n.down_too_big++;
        return;
    }
    /* heard back by a receiver here, it's ours: not relayed again */
    int k = g->dedup_next;
    g->dedup_next = (k + 1) % RDOF_IPGW_DEDUP;
    g->dedup[k].hash = message_hash(from, radio, d, n);
    g->dedup[k].at = now > 0 ? now : 1e-9;
    g->dedup[k].acked = 0;
    uint32_t before = g->n.no_tx;
    transmit(g, &f->where);
    if (g->n.no_tx == before) {
        f->down++;
        g->n.down_packets++;
        g->n.down_bytes += (uint32_t)n;
    }
}

/* One hop on for a whole datagram in place: false (counted) when its TTL is spent. */
static bool hop(rdof_ipgw_t *g, uint8_t *d, size_t ihl)
{
    if (d[8] <= 1) {
        g->n.down_ttl_expired++;
        return false;
    }
    d[8]--;
    d[10] = d[11] = 0;
    put16(d + 10, fold(sum16(0, d, ihl)));
    return true;
}

void rdof_ipgw_from_net(rdof_ipgw_t *g, int fi, uint32_t ip, uint16_t port, const uint8_t *data, size_t len,
                        double now)
{
    if (!g->cfg.on || fi < 0 || fi >= RDOF_IPGW_MAX_FLOWS || !g->flows[fi].used || g->flows[fi].proto == IP_ICMP)
        return;
    rdof_ipgw_flow_t *f = &g->flows[fi];
    if (ip != f->peer_ip || port != f->peer_port) {
        g->n.down_stranger++;
        return;
    }
    uint8_t *d = g->scratch;
    size_t n;
    uint32_t radio = f->radio;
    if (f->payload == RDOF_IPGW_PAYLOAD_UDP) {
        if (28 + len > (size_t)g->cfg.max_bytes) {
            g->n.down_too_big++;
            return;
        }
        n = rdof_ipgw_udp_datagram(f->orig_ip, f->orig_port, f->inner_ip, f->inner_port, g->ip_id, data, len, d,
                                   sizeof g->scratch);
    } else {
        if (len < 20 || data[0] >> 4 != 4 || (data[0] & 0x0F) < 5 || be16(data + 2) < 20 || be16(data + 2) > len) {
            g->n.down_bad_ip++;
            return;
        }
        n = be16(data + 2);
        if (n > (size_t)g->cfg.max_bytes) {
            g->n.down_too_big++;
            return;
        }
        memcpy(d, data, n);
        size_t ihl = (size_t)(d[0] & 0x0F) * 4;
        if (ihl > n || fold(sum16(0, d, ihl)) != 0) {
            g->n.down_bad_ip++;
            return;
        }
        if (!hop(g, d, ihl))
            return;
        uint32_t id;
        if (rdof_ipgw_ip_radio(&g->cfg.radio_net, be32(d + 16), &id))
            radio = id; /* a whole datagram names its radio */
    }
    send_down(g, f, radio, d, n, now);
}

/* RFC 1624: a checksum field after a 16-bit word under it changed from old to now. */
static void adjust(uint8_t *sum, uint16_t old, uint16_t now)
{
    uint32_t s = (uint16_t)~be16(sum) + (uint32_t)(uint16_t)~old + now;
    put16(sum, fold(s));
}

static void adjust32(uint8_t *sum, uint32_t old, uint32_t now)
{
    adjust(sum, (uint16_t)(old >> 16), (uint16_t)(now >> 16));
    adjust(sum, (uint16_t)old, (uint16_t)now);
}

/* The flow an ICMP error's quoted datagram (q, ql octets; ours as it went out) belongs to, or -1. */
static int quoted_flow(const rdof_ipgw_t *g, const uint8_t *q, size_t ql)
{
    if (ql < 20 || q[0] >> 4 != 4)
        return -1;
    size_t qihl = (size_t)(q[0] & 0x0F) * 4;
    if (qihl < 20 || ql < qihl + 8)
        return -1;
    const uint8_t *t = q + qihl;
    uint32_t dst = be32(q + 16);
    for (int i = 0; i < RDOF_IPGW_MAX_FLOWS; i++) {
        const rdof_ipgw_flow_t *f = &g->flows[i];
        if (!f->used || f->peer_ip != dst)
            continue;
        if (q[9] == IP_UDP && f->proto == IP_UDP && f->peer_port == be16(t + 2) &&
            (!g->local_port || g->local_port(g->ctx, i) == be16(t)))
            return i;
        if (q[9] == IP_ICMP && f->proto == IP_ICMP && t[0] == ICMP_ECHO && be16(t + 4) == RDOF_IPGW_ICMP_ID_BASE + i)
            return i;
    }
    return -1;
}

void rdof_ipgw_icmp_from_net(rdof_ipgw_t *g, const uint8_t *pkt, size_t len, double now)
{
    if (!g->cfg.on)
        return;
    if (len < 20 || pkt[0] >> 4 != 4 || (pkt[0] & 0x0F) < 5 || be16(pkt + 2) > len || pkt[9] != IP_ICMP) {
        g->n.down_bad_ip++;
        return;
    }
    size_t ihl = (size_t)(pkt[0] & 0x0F) * 4;
    len = be16(pkt + 2);
    if (len < ihl + 8) {
        g->n.down_bad_ip++;
        return;
    }
    const uint8_t *icmp = pkt + ihl;
    size_t il = len - ihl;
    uint32_t from = be32(pkt + 12);
    int type = icmp[0], fi = -1;
    if (type == ICMP_ECHO_REPLY) {
        fi = be16(icmp + 4) - RDOF_IPGW_ICMP_ID_BASE;
        if (fi < 0 || fi >= RDOF_IPGW_MAX_FLOWS || !g->flows[fi].used || g->flows[fi].proto != IP_ICMP) {
            g->n.icmp_unmatched++;
            return;
        }
        if (from != g->flows[fi].peer_ip) {
            g->n.down_stranger++;
            return;
        }
    } else if (type == ICMP_UNREACHABLE || type == ICMP_TIME_EXCEEDED || type == ICMP_PARAMETER) {
        fi = quoted_flow(g, icmp + 8, il - 8);
        if (fi < 0) {
            g->n.icmp_unmatched++;
            return;
        }
    } else {
        return; /* echo requests to the board and the rest: lwIP's, not ours */
    }
    rdof_ipgw_flow_t *f = &g->flows[fi];
    if (20 + il > sizeof g->scratch) {
        g->n.down_too_big++;
        return;
    }
    uint8_t *d = g->scratch, *m = d + 20;
    memcpy(m, icmp, il);
    if (type == ICMP_ECHO_REPLY) {
        put16(m + 4, f->inner_port);
    } else { /* the quoted datagram back to what the radio's host sent: addresses, the port or echo id */
        uint8_t *q = m + 8, *t = q + (size_t)(q[0] & 0x0F) * 4;
        adjust32(q[9] == IP_UDP && be16(t + 6) ? t + 6 : q + 10, be32(q + 12), f->inner_ip);
        if (q[9] == IP_UDP && be16(t + 6)) {
            adjust32(t + 6, be32(q + 16), f->orig_ip);
            adjust(t + 6, be16(t), f->inner_port);
            adjust(t + 6, be16(t + 2), f->orig_port);
        } else if (q[9] == IP_ICMP) {
            adjust(t + 2, be16(t + 4), f->inner_port);
        }
        put32(q + 12, f->inner_ip);
        put32(q + 16, f->orig_ip);
        if (q[9] == IP_UDP) {
            put16(t, f->inner_port);
            put16(t + 2, f->orig_port);
        } else {
            put16(t + 4, f->inner_port);
        }
        q[10] = q[11] = 0;
        put16(q + 10, fold(sum16(0, q, (size_t)(q[0] & 0x0F) * 4)));
        g->n.icmp_errors++;
    }
    m[2] = m[3] = 0;
    put16(m + 2, fold(sum16(0, m, il)));
    /* the outer header: from whoever sent it, to the radio's host, one hop on */
    memset(d, 0, 20);
    d[0] = 0x45;
    put16(d + 2, (uint32_t)(20 + il));
    put16(d + 4, g->ip_id);
    d[8] = pkt[8];
    d[9] = IP_ICMP;
    put32(d + 12, from);
    put32(d + 16, f->inner_ip);
    if (!hop(g, d, 20))
        return;
    if (type == ICMP_ECHO_REPLY)
        g->n.icmp_down++;
    send_down(g, f, f->radio, d, 20 + il, now);
}

void rdof_ipgw_tick(rdof_ipgw_t *g, double now)
{
    for (int i = 0; i < RDOF_IPGW_MAX_ASM; i++)
        if (g->asms[i].used && now - g->asms[i].last > RDOF_IPGW_ASM_TIMEOUT_S) {
            g->asms[i].used = false;
            g->n.rx_incomplete++;
        }
    for (int i = 0; i < RDOF_IPGW_MAX_FLOWS; i++)
        if (g->flows[i].used && now - g->flows[i].last > g->cfg.idle_s) {
            close_flow(g, i);
            g->n.flows_expired++;
        }
}

void rdof_ipgw_status_json(const rdof_ipgw_t *g, rdof_json_t *j)
{
    const rdof_ipgw_counters_t *n = &g->n;
    rdof_json_open(j, "ip_gateway");
    rdof_json_int(j, "routes", g->cfg.nroutes);
    rdof_json_bool(j, "nat", g->cfg.nat);
    rdof_json_int(j, "flows", rdof_ipgw_flows(g));
    rdof_json_int(j, "max_flows", g->cfg.max_flows);
    struct {
        const char *key;
        uint32_t v;
    } c[] = {
        {"rx_messages", n->rx_messages}, {"rx_bad", n->rx_bad},         {"rx_incomplete", n->rx_incomplete},
        {"rx_dup", n->rx_dup},           {"not_ip", n->not_ip},         {"bad_ip", n->bad_ip},         {"bad_src", n->bad_src},
        {"fragment", n->fragment},       {"too_big", n->too_big},       {"not_udp", n->not_udp},
        {"no_route", n->no_route},       {"denied", n->denied},         {"up_rate", n->up_rate},
        {"flows_full", n->flows_full},   {"unresolved", n->unresolved}, {"send_failed", n->send_failed},
        {"up_packets", n->up_packets},   {"up_bytes", n->up_bytes},     {"down_packets", n->down_packets},
        {"down_bytes", n->down_bytes},   {"down_rate", n->down_rate},   {"down_too_big", n->down_too_big},
        {"down_stranger", n->down_stranger}, {"down_bad_ip", n->down_bad_ip}, {"no_tx", n->no_tx},
        {"acks", n->acks},               {"flows_opened", n->flows_opened}, {"flows_expired", n->flows_expired},
        {"queue_full", n->queue_full},   {"sap3", n->sap3},             {"ttl_expired", n->ttl_expired},
        {"down_ttl_expired", n->down_ttl_expired},
        {"icmp_up", n->icmp_up},         {"icmp_down", n->icmp_down},   {"icmp_errors", n->icmp_errors},
        {"icmp_unmatched", n->icmp_unmatched},
        {"down_own_source", n->down_own_source}, {"tx_busy", n->tx_busy},
    };
    for (size_t i = 0; i < sizeof c / sizeof c[0]; i++)
        rdof_json_int(j, c[i].key, c[i].v);
    rdof_json_close(j);
}

#endif /* RDOF_FEATURE_DATA_BLOCKS */
