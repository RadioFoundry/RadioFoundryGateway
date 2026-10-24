// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The IP gateway: DMR packet data to and from the IP network (docs/spec/fw/ipgw.md).
 *
 * Radios send IPv4 datagrams as DMR packet data (ETSI TS 102 361-1 clause 9 and 361-3: a data header, then rate
 * 1/2, 3/4 or 1 blocks, unconfirmed or confirmed, SAP 4 "IP based packet data"). The gateway puts them back
 * together from the bursts it hears, and relays them to the network over UDP from the board's own address:
 *
 *   - routes: rules on the DMR addressing (a private call to an id or a range, a talkgroup, a source radio, or
 *     all) that send the datagram's UDP payload (or the whole datagram) to a set address and port;
 *   - NAT: any UDP datagram, to the address and port its own IPv4 header names (source NAT: each flow gets a
 *     local port of its own), within an allow / deny list.
 *
 * Replies on a flow's local port go back down to the radio as unconfirmed rate 1/2 packet data, with the inner
 * addresses restored, so the radio sees an answer from the address it sent to.
 *
 * Portable C: no sockets here. The platform owns one UDP socket per flow (open / close), sends what send_udp
 * hands it, passes what arrives to rdof_ipgw_from_net, and transmits what send_dmr builds. Everything is called
 * from one task; nothing here locks.
 */
#ifndef RDOF_IPGW_H
#define RDOF_IPGW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/features.h"
#include "rdof/fec.h"
#include "rdof/json.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_IPGW_MAX_ROUTES 8
#define RDOF_IPGW_MAX_NETS 8     /* allow and deny entries, each */
#define RDOF_IPGW_MAX_FLOWS 16   /* the table; max_flows is at most this */
#define RDOF_IPGW_MAX_ASM 4      /* messages being put together at once (a source and slot each) */
#define RDOF_IPGW_DEDUP 16       /* messages remembered, to drop the same one heard twice */
#define RDOF_IPGW_DEDUP_S 5.0
#define RDOF_IPGW_ASM_TIMEOUT_S 2.0 /* a message whose next block doesn't come in this long is dropped */
#define RDOF_IPGW_ACK_AGAIN_S 0.5   /* the same confirmed message again: answered again after this long */
/* The longest datagram either way: 127 blocks (the header's 7-bit count) of 12 octets, less the CRC-32. */
#define RDOF_IPGW_MAX_PACKET 1520
#define RDOF_IPGW_MAX_BURSTS 128 /* a header and up to 127 blocks */

#define RDOF_SAP_IP 4
#define RDOF_SAP_UDP_COMPRESSED 3 /* UDP/IPv4 with the compressed header (ETSI TS 102 361-3 7.2.3, table 7.14) */
#define RDOF_IPGW_ICMP_ID_BASE 0x5D00 /* NAT'd echo requests go out with this identifier plus their flow's index */
#define RDOF_IPGW_GROUP_NET 0xE1000000u /* 225.0.0.0: a talkgroup's address, the id in the low 24 bits */
/* The id every gateway answers, besides its own: a radio's ipGatewayId set to it reaches the Internet through any
 * gateway that hears it. (NAT takes off-net traffic whatever the DMR destination; replies come from the id the
 * radio sent to.) */
#define RDOF_IPGW_ANY_GATEWAY 9999999u

/* An IPv4 network, host byte order. */
typedef struct {
    uint32_t net, mask;
} rdof_ipgw_net_t;

typedef enum { RDOF_IPGW_PAYLOAD_UDP = 0, RDOF_IPGW_PAYLOAD_IP } rdof_ipgw_payload_t;

typedef enum { RDOF_IPGW_DST_ANY = 0, RDOF_IPGW_DST_PRIVATE, RDOF_IPGW_DST_GROUP } rdof_ipgw_dst_kind_t;

typedef struct {
    char name[24];
    bool all;
    rdof_ipgw_dst_kind_t dst_kind;
    uint32_t dst_lo, dst_hi; /* the DMR destination, inclusive (dst_kind not ANY) */
    bool has_from;
    uint32_t from_lo, from_hi; /* the DMR source radio, inclusive */
    char host[64];             /* "to": an IPv4 address or a name the platform resolves */
    uint32_t ip;               /* host order; 0 until resolved */
    int port;                  /* 0: the datagram's own destination port (payload udp only) */
    rdof_ipgw_payload_t payload;
} rdof_ipgw_route_t;

typedef struct {
    bool on;
    uint32_t id;               /* the gateway's DMR id: the source of replies to talkgroup data (0: the talkgroup) */
    rdof_ipgw_net_t radio_net; /* radio id <-> IPv4 (12.0.0.0/8: id 9994567 is 12.152.129.71) */
    char tx[24];               /* the roamer or repeater that transmits replies ("": the first that runs) */
    bool ack;                  /* answer confirmed data the gateway takes */
    int max_bytes;             /* the longest datagram either way */
    double up_pps, down_pps;   /* datagrams a second, each way (0: no limit); bursts of twice that */
    double idle_s;             /* a flow with nothing either way this long is closed */
    int max_flows;
    rdof_ipgw_route_t routes[RDOF_IPGW_MAX_ROUTES];
    int nroutes;
    bool nat;
    rdof_ipgw_net_t allow[RDOF_IPGW_MAX_NETS];
    int nallow; /* 0: any */
    rdof_ipgw_net_t deny[RDOF_IPGW_MAX_NETS];
    int ndeny;
} rdof_ipgw_config_t;

/* The defaults: off, radio_net 12.0.0.0/8, ack on, max_bytes 1500, up 10 / down 2 a second, idle 120 s, 8 flows,
 * no routes, NAT off, deny the private and shared ranges (RDOF_IPGW_DEFAULT_DENY). */
void rdof_ipgw_config_default(rdof_ipgw_config_t *cfg);

/* "ip_gateway" from the board's settings (JSONC). Absent, null or false: off (true with cfg->on false). false with
 * error set when it's there and wrong. */
bool rdof_ipgw_settings_parse(const char *text, size_t len, rdof_ipgw_config_t *out, char *error, size_t cap);
/* One line for /api/resolved and the log. */
size_t rdof_ipgw_describe(const rdof_ipgw_config_t *cfg, char *out, size_t cap);

/* "a.b.c.d" or "a.b.c.d/n" (host bits must be zero). */
bool rdof_ipgw_parse_cidr(const char *s, rdof_ipgw_net_t *out);
/* "a.b.c.d" alone. */
bool rdof_ipgw_parse_ip(const char *s, uint32_t *out);
bool rdof_ipgw_in_net(const rdof_ipgw_net_t *n, uint32_t ip);
/* A radio's address in radio_net: the id in the host bits (the Motorola convention with 12.0.0.0/8). */
uint32_t rdof_ipgw_radio_ip(const rdof_ipgw_net_t *net, uint32_t id);
/* The radio id an address in radio_net stands for; false outside it, or for id 0. */
bool rdof_ipgw_ip_radio(const rdof_ipgw_net_t *net, uint32_t ip, uint32_t *id);
/* Built in and always denied to NAT: 0/8, 127/8, 169.254/16, 224/4, 240/4 (broadcast in it), and radio_net. */
bool rdof_ipgw_reserved(const rdof_ipgw_config_t *cfg, uint32_t ip);
/* NAT may send to ip: not reserved, in allow (when it has any) and not in deny. */
bool rdof_ipgw_nat_allowed(const rdof_ipgw_config_t *cfg, uint32_t ip);

#if RDOF_FEATURE_DATA_BLOCKS

/* Where a message was heard: the platform's source (an SDR channel, a radio), the slot, and the channel. */
typedef struct {
    int source, slot;
    uint32_t freq; /* Hz; 0 unknown */
    int cc;        /* -1: from the bursts */
} rdof_ipgw_where_t;

/* A DMR packet to transmit: data header and blocks, as MMDVM DMR_DATA control bytes and bursts. */
typedef struct {
    uint32_t dst, src; /* the radio, and the id it comes from */
    int slot;          /* where the radio was heard */
    uint32_t freq;
    int cc;
    bool response; /* a data response (an ACK), one burst */
    int nbursts;
    uint8_t control[RDOF_IPGW_MAX_BURSTS];
    uint8_t bursts[RDOF_IPGW_MAX_BURSTS][RDOF_BURST_BYTES];
} rdof_ipgw_dmr_t;

typedef struct {
    bool used;
    int route; /* -1: NAT */
    rdof_ipgw_payload_t payload;
    uint32_t radio;      /* the DMR source */
    uint32_t dmr_dst;    /* what it addressed on DMR */
    bool group;
    uint32_t inner_ip;   /* the radio's own address and port, from its datagram */
    uint16_t inner_port;
    uint32_t orig_ip;    /* where its datagram was addressed: restored as the source of replies */
    uint16_t orig_port;
    uint8_t proto;       /* 17 UDP, 1 ICMP echo (inner_port the echo identifier), 0 a whole datagram */
    uint32_t peer_ip;    /* where it goes */
    uint16_t peer_port;
    double opened, last;
    rdof_ipgw_where_t where; /* where the radio was last heard: replies go there */
    uint32_t up, down;
} rdof_ipgw_flow_t;

typedef struct {
    uint32_t rx_messages;   /* packet data messages put together, CRCs good */
    uint32_t rx_bad;        /* a block that didn't decode, a CRC-9 or the CRC-32 failed */
    uint32_t rx_incomplete; /* blocks missing (timed out or another header first) */
    uint32_t rx_dup;        /* the same message again within RDOF_IPGW_DEDUP_S, or our own heard back */
    uint32_t not_ip;        /* not SAP 4, or not IPv4 */
    uint32_t bad_ip;        /* IPv4 / UDP header wrong (lengths, checksum) */
    uint32_t bad_src;       /* a source in radio_net that isn't the sending radio's own address */
    uint32_t fragment;      /* IPv4 fragments: not put together */
    uint32_t too_big;       /* past max_bytes */
    uint32_t not_udp;       /* NAT, or a udp route: another protocol (ICMP, TCP, ...) */
    uint32_t no_route;      /* no route took it and NAT is off */
    uint32_t denied;        /* NAT: the destination is reserved, not allowed, or denied */
    uint32_t up_rate;       /* over up_pps */
    uint32_t flows_full;    /* no free flow (max_flows), or the platform had no socket */
    uint32_t unresolved;    /* the route's host has no address yet */
    uint32_t send_failed;
    uint32_t up_packets, up_bytes;
    uint32_t down_packets, down_bytes;
    uint32_t down_rate;     /* over down_pps */
    uint32_t down_too_big;
    uint32_t down_stranger; /* from an address the flow didn't send to */
    uint32_t down_bad_ip;   /* payload ip: not an IPv4 datagram */
    uint32_t no_tx;         /* nothing to transmit with, or it refused */
    uint32_t acks;
    uint32_t flows_opened, flows_expired;
    uint32_t queue_full;    /* the platform's: bursts it couldn't queue for the gateway */
    uint32_t sap3;          /* SAP 3 (compressed header) messages expanded */
    uint32_t ttl_expired;   /* TTL 1 or 0: not passed on */
    uint32_t down_ttl_expired; /* a whole datagram from the network (payload ip, ICMP) with TTL 1 or 0 */
    uint32_t icmp_up;       /* echo requests NAT'd */
    uint32_t icmp_down;     /* echo replies sent down */
    uint32_t icmp_errors;   /* unreachable, time exceeded, parameter problem: sent down, the quote rewritten */
    uint32_t icmp_unmatched; /* echo replies and errors that are no flow's */
    uint32_t down_own_source; /* down packets from the radio's own address (it would drop them): not sent */
    uint32_t tx_busy;         /* down packets dropped: the repeater's slot stayed busy (the platform's, rdof/ipgwtx.h) */
} rdof_ipgw_counters_t;

typedef struct {
    bool used;
    int source, slot;
    uint32_t src, dst;
    bool group, confirmed;
    int sap, pad, ns;
    bool response_requested;
    int expect, got;
    bool bad, overflow;
    int len;
    double last;
    rdof_ipgw_where_t where;
    uint8_t data[RDOF_IPGW_MAX_PACKET + 32];
} rdof_ipgw_asm_t;

typedef struct rdof_ipgw {
    rdof_ipgw_config_t cfg;
    /* hooks */
    bool (*open)(void *ctx, int flow);  /* a UDP socket on a local port of its own for flow; false: none */
    void (*close)(void *ctx, int flow);
    /* ttl: what the payload goes with (the radio's datagram's, less one); 0: data is a whole datagram */
    bool (*send_udp)(void *ctx, int flow, uint32_t ip, uint16_t port, const uint8_t *data, size_t len, int ttl);
    bool (*send_dmr)(void *ctx, const rdof_ipgw_dmr_t *p); /* false: nothing to transmit with */
    /* NAT'd ICMP: the message (no IP header) to ip, with this TTL; NULL: no ICMP */
    bool (*send_icmp)(void *ctx, uint32_t ip, const uint8_t *icmp, size_t len, int ttl);
    /* the local port flow's socket is bound to, for matching ICMP errors to it (NULL: not checked) */
    uint16_t (*local_port)(void *ctx, int flow);
    void *ctx;
    uint64_t sync48; /* the sync on bursts sent: MS data for a simplex radio, BS data for a repeater */
    /* state */
    rdof_ipgw_asm_t asms[RDOF_IPGW_MAX_ASM];
    rdof_ipgw_flow_t flows[RDOF_IPGW_MAX_FLOWS];
    struct {
        uint32_t hash;
        double at, acked;
    } dedup[RDOF_IPGW_DEDUP];
    int dedup_next;
    double up_tokens, up_at, down_tokens, down_at;
    uint16_t ip_id;
    rdof_ipgw_dmr_t out; /* what send_dmr gets */
    uint8_t scratch[RDOF_IPGW_MAX_PACKET + 32];
    uint8_t up[RDOF_IPGW_MAX_PACKET + 32]; /* a datagram going up, rebuilt (SAP 3) or its TTL changed */
    rdof_ipgw_counters_t n;
} rdof_ipgw_t;

/* cfg copied; the hooks set after. sync48 starts as MS data sync. */
void rdof_ipgw_init(rdof_ipgw_t *g, const rdof_ipgw_config_t *cfg);
/* A burst heard (data headers and blocks; anything else ends nothing and is ignored). */
void rdof_ipgw_burst(rdof_ipgw_t *g, const rdof_ipgw_where_t *w, int dt, const uint8_t burst[RDOF_BURST_BYTES],
                     double now);
/* A whole message's user data (CRC checked), as if put together from bursts. */
void rdof_ipgw_message(rdof_ipgw_t *g, const rdof_ipgw_where_t *w, uint32_t src, uint32_t dst, bool group, int sap,
                       const uint8_t *data, size_t len, double now);
/* A datagram that arrived on flow's socket from ip:port. */
void rdof_ipgw_from_net(rdof_ipgw_t *g, int flow, uint32_t ip, uint16_t port, const uint8_t *data, size_t len,
                        double now);
/* An ICMP packet (its IPv4 header included) that arrived on the platform's ICMP socket: echo replies to NAT'd
 * echo requests, and errors quoting a flow's datagram, go down to the radio rewritten to its addresses. */
void rdof_ipgw_icmp_from_net(rdof_ipgw_t *g, const uint8_t *packet, size_t len, double now);
/* Close idle flows and drop stale half messages; call a few times a second. */
void rdof_ipgw_tick(rdof_ipgw_t *g, double now);
int rdof_ipgw_flows(const rdof_ipgw_t *g);
/* "ip_gateway": {...} for /api/status. */
void rdof_ipgw_status_json(const rdof_ipgw_t *g, rdof_json_t *j);

/* Unconfirmed rate 1/2 packet data for data (len <= RDOF_IPGW_MAX_PACKET): header and blocks into out (dst, src,
 * the bursts). false when it doesn't fit. */
bool rdof_ipgw_build_dmr(uint32_t src, uint32_t dst, int sap, const uint8_t *data, size_t len, int cc,
                         uint64_t sync48, rdof_ipgw_dmr_t *out);
/* A data response header (class 0 type 1: ACK, status N(S)) for a confirmed message. */
void rdof_ipgw_build_ack(uint32_t src, uint32_t dst, int sap, int ns, int cc, uint64_t sync48, rdof_ipgw_dmr_t *out);
/* A 12-octet data header: unconfirmed (dpf 2) or confirmed (3), with its CRC. */
void rdof_ipgw_data_header(bool group, bool confirmed, bool response_requested, int sap, int pad, uint32_t dst,
                           uint32_t src, int blocks, int ns, uint8_t out[12]);
/* IPv4 + UDP around payload (the checksums set): the length, or 0 when it doesn't fit cap. */
size_t rdof_ipgw_udp_datagram(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, uint16_t id,
                              const uint8_t *payload, size_t len, uint8_t *out, size_t cap);

#endif /* RDOF_FEATURE_DATA_BLOCKS */

#ifdef __cplusplus
}
#endif

#endif
