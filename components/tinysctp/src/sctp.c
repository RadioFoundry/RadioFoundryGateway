// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The SCTP association: RFC 9260 for one peer over one path, with PR-SCTP
 * (RFC 3758, timed lifetime). What it does and leaves out is in README.md.
 *
 * Send side: messages are split into DATA chunks on the queue (q); a chunk
 * gets its TSN when it first goes out and moves to the outstanding list (o,
 * in TSN order) until the peer's cumulative ack passes it. Receive side: a
 * ring of TSCTP_RX_WINDOW slots indexed by TSN, from rx_floor (the oldest
 * TSN still held: a fragment of a message not complete yet, or cum_in + 1).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinysctp.h"

#ifndef TSCTP_MAX_STREAMS
#define TSCTP_MAX_STREAMS 16
#endif
#ifndef TSCTP_RX_WINDOW
#define TSCTP_RX_WINDOW 64
#endif

enum {
    CT_DATA = 0,
    CT_INIT = 1,
    CT_INIT_ACK = 2,
    CT_SACK = 3,
    CT_HEARTBEAT = 4,
    CT_HEARTBEAT_ACK = 5,
    CT_ABORT = 6,
    CT_SHUTDOWN = 7,
    CT_SHUTDOWN_ACK = 8,
    CT_ERROR = 9,
    CT_COOKIE_ECHO = 10,
    CT_COOKIE_ACK = 11,
    CT_SHUTDOWN_COMPLETE = 14,
    CT_FORWARD_TSN = 192,
};
enum { DF_E = 0x01, DF_B = 0x02, DF_U = 0x04 };
#define T_BIT 0x01
enum { P_HB_INFO = 1, P_COOKIE = 7, P_SUPPORTED_EXT = 0x8008, P_FWD_TSN_SUPPORTED = 0xC000 };
enum { E_INVALID_STREAM = 1, E_STALE_COOKIE = 3, E_UNRECOGNIZED_CHUNK = 6 };

#define HDR 12        /* common header */
#define DATA_HDR 16   /* DATA chunk header */
#define CTL_MAX 256   /* control chunks waiting for the next packet */
#define MAX_GAPS 16   /* gap blocks reported in a SACK */
#define MAX_DUPS 8
#define COOKIE_LEN 60 /* our state cookie: 52 bytes of state and an 8-byte MAC */
#define HB_INFO_LEN 20
#define NEVER UINT64_MAX
#define W TSCTP_RX_WINDOW

/* ── chunks and slots ─────────────────────────────────────────────────────── */

enum { CH_QUEUED, CH_FLIGHT, CH_ACKED, CH_ABANDONED };
enum { RTX_NO, RTX_T3, RTX_FAST };

typedef struct chunk {
    struct chunk *next;
    uint64_t sent_at, expires; /* expires: 0 = reliable */
    uint32_t tsn, ppid, msg;
    uint16_t sid, ssn, len;
    uint8_t flags, tx, miss, st, rtx;
    bool fast_done;            /* fast retransmitted once already */
    uint8_t data[];
} chunk_t;

enum { RX_EMPTY, RX_HELD, RX_DONE };

typedef struct {
    uint8_t *data;
    uint32_t tsn, ppid;
    uint16_t sid, ssn, len;
    uint8_t flags, st;
} rxslot_t;

struct tsctp {
    tsctp_config_t cfg;
    tsctp_state_t st;
    bool listening, connect_role, local_close;
    int busy;
    uint32_t epoch; /* counts closes: a packet's chunks stop if a callback closed or reconnected */
    uint8_t key[16];
    uint16_t lport, pport;
    uint32_t my_vtag, peer_vtag, my_itsn;
    uint16_t nout, nin;
    bool peer_pr;
    uint8_t *cookie; /* the INIT ACK's cookie, echoed until COOKIE ACK */
    uint16_t cookie_len;

    /* timers: absolute ms, NEVER when off */
    uint64_t t1, t2, t3, tsack, thb, thb_timeout, treconnect;
    int init_tries;
    uint32_t reconnect_ms;

    /* the path */
    uint32_t rto, srtt, rttvar, cwnd, ssthresh, pba, peer_rwnd, errors, fr_exit, hb_nonce;
    bool have_rtt, fast_recovery, hb_out;
    uint64_t last_tx;

    /* sending */
    uint32_t next_tsn, cum_ack, adv_ack, msg_seq, rtt_tsn;
    bool rtt_pending, fwd_pending, rtx_force;
    uint16_t out_ssn[TSCTP_MAX_STREAMS];
    chunk_t *q_head, *q_tail, *o_head, *o_tail;
    uint32_t q_bytes, o_bytes, q_msgs;

    /* receiving */
    uint32_t cum_in, rx_floor, rx_high, rx_bytes;
    uint16_t in_ssn[TSCTP_MAX_STREAMS];
    rxslot_t rx[W];
    int data_pkts, ndups;
    bool sack_now;
    uint32_t dups[MAX_DUPS];

    uint8_t ctl[CTL_MAX];
    uint16_t ctl_len;

    uint8_t *tx;           /* the packet being built */
    uint16_t tx_len, tx_cap, tx_limit;

    tsctp_stats_t stats;
};

/* ── little helpers ───────────────────────────────────────────────────────── */

static void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)(v >> 32)); put32(p + 4, (uint32_t)v); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t get64(const uint8_t *p) { return (uint64_t)get32(p) << 32 | get32(p + 4); }

/* serial number arithmetic (RFC 1982) */
static bool lt32(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static bool le32(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }
static bool lt16(uint16_t a, uint16_t b) { return (int16_t)(a - b) < 0; }

static size_t pad4(size_t n) { return (n + 3) & ~(size_t)3; }
static uint32_t min32(uint32_t a, uint32_t b) { return a < b ? a : b; }
static uint32_t max32(uint32_t a, uint32_t b) { return a > b ? a : b; }
static uint64_t min64(uint64_t a, uint64_t b) { return a < b ? a : b; }

static void *xalloc(tsctp_t *s, size_t n) { return s->cfg.alloc ? s->cfg.alloc(n) : malloc(n); }
static void xfree(tsctp_t *s, void *p)
{
    if (!p)
        return;
    if (s->cfg.free)
        s->cfg.free(p);
    else
        free(p);
}
static uint32_t rnd(tsctp_t *s) { return s->cfg.random(s->cfg.random_ctx); }
static uint32_t rnd_tag(tsctp_t *s)
{
    uint32_t v;
    while ((v = rnd(s)) == 0) {
    }
    return v;
}

static uint32_t csize(const chunk_t *c) { return (uint32_t)c->len + DATA_HDR; }

/* SipHash-2-4, the state cookie's MAC. */
#define ROTL(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))
#define SIPROUND                                                                                    \
    do {                                                                                            \
        v0 += v1; v1 = ROTL(v1, 13); v1 ^= v0; v0 = ROTL(v0, 32);                                   \
        v2 += v3; v3 = ROTL(v3, 16); v3 ^= v2;                                                      \
        v0 += v3; v3 = ROTL(v3, 21); v3 ^= v0;                                                      \
        v2 += v1; v1 = ROTL(v1, 17); v1 ^= v2; v2 = ROTL(v2, 32);                                   \
    } while (0)

static uint64_t le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}

static uint64_t siphash(const uint8_t key[16], const uint8_t *in, size_t len)
{
    uint64_t k0 = le64(key), k1 = le64(key + 8);
    uint64_t v0 = 0x736f6d6570736575ULL ^ k0, v1 = 0x646f72616e646f6dULL ^ k1;
    uint64_t v2 = 0x6c7967656e657261ULL ^ k0, v3 = 0x7465646279746573ULL ^ k1;
    uint64_t b = (uint64_t)len << 56;
    size_t i = 0;
    for (; i + 8 <= len; i += 8) {
        uint64_t m = le64(in + i);
        v3 ^= m;
        SIPROUND;
        SIPROUND;
        v0 ^= m;
    }
    for (size_t j = 0; i + j < len; j++)
        b |= (uint64_t)in[i + j] << (8 * j);
    v3 ^= b;
    SIPROUND;
    SIPROUND;
    v0 ^= b;
    v2 ^= 0xff;
    SIPROUND;
    SIPROUND;
    SIPROUND;
    SIPROUND;
    return v0 ^ v1 ^ v2 ^ v3;
}

static void emit_event(tsctp_t *s, tsctp_event_t ev, tsctp_reason_t why)
{
    if (s->cfg.on_event)
        s->cfg.on_event(s->cfg.user, ev, why);
}

/* ── building packets ─────────────────────────────────────────────────────── */

static void pk_begin_ports(tsctp_t *s, uint32_t vtag, uint16_t sport, uint16_t dport)
{
    put16(s->tx, sport);
    put16(s->tx + 2, dport);
    put32(s->tx + 4, vtag);
    put32(s->tx + 8, 0);
    s->tx_len = HDR;
    s->tx_limit = s->cfg.mtu;
}

static void pk_begin(tsctp_t *s, uint32_t vtag) { pk_begin_ports(s, vtag, s->lport, s->pport); }

static bool pk_fits(const tsctp_t *s, size_t body) { return s->tx_len + pad4(4 + body) <= s->tx_limit; }

/* Room for a chunk of `body` bytes: its body, or NULL when the packet is full. */
static uint8_t *pk_chunk(tsctp_t *s, uint8_t type, uint8_t flags, size_t body)
{
    size_t n = pad4(4 + body);
    if (s->tx_len + n > s->tx_limit)
        return NULL;
    uint8_t *c = s->tx + s->tx_len;
    c[0] = type;
    c[1] = flags;
    put16(c + 2, (uint16_t)(4 + body));
    memset(c + 4 + body, 0, n - 4 - body);
    s->tx_len += (uint16_t)n;
    return c + 4;
}

static void pk_flush(tsctp_t *s)
{
    if (s->tx_len > HDR) {
        uint32_t crc = tsctp_crc32c(0, s->tx, s->tx_len);
        s->tx[8] = (uint8_t)crc; /* the CRC goes out least significant byte first */
        s->tx[9] = (uint8_t)(crc >> 8);
        s->tx[10] = (uint8_t)(crc >> 16);
        s->tx[11] = (uint8_t)(crc >> 24);
        s->cfg.output(s->cfg.output_ctx, s->tx, s->tx_len);
        s->stats.pkts_out++;
    }
    s->tx_len = 0;
}

/* A packet of one chunk with no body (ABORT, SHUTDOWN ACK / COMPLETE, COOKIE ACK). */
static void send_bare(tsctp_t *s, uint32_t vtag, uint8_t type, uint8_t flags, uint16_t sport, uint16_t dport)
{
    pk_begin_ports(s, vtag, sport, dport);
    pk_chunk(s, type, flags, 0);
    pk_flush(s);
}

/* A control chunk for the next packet out (dropped if there's no room; the peer will ask again). */
static uint8_t *ctl_chunk(tsctp_t *s, uint8_t type, uint8_t flags, size_t body)
{
    size_t n = pad4(4 + body);
    if (s->ctl_len + n > CTL_MAX)
        return NULL;
    uint8_t *c = s->ctl + s->ctl_len;
    c[0] = type;
    c[1] = flags;
    put16(c + 2, (uint16_t)(4 + body));
    memset(c + 4 + body, 0, n - 4 - body);
    s->ctl_len += (uint16_t)n;
    return c + 4;
}

/* An ERROR chunk with one cause. */
static void ctl_error(tsctp_t *s, uint16_t cause, const uint8_t *info, size_t len)
{
    if (len > CTL_MAX - 16)
        len = CTL_MAX - 16;
    uint8_t *b = ctl_chunk(s, CT_ERROR, 0, 4 + len);
    if (!b)
        return;
    put16(b, cause);
    put16(b + 2, (uint16_t)(4 + len));
    if (len)
        memcpy(b + 4, info, len);
}

/* ── the path: RTO, congestion window, heartbeat ──────────────────────────── */

static void rto_clamp(tsctp_t *s)
{
    if (s->rto < s->cfg.rto_min_ms)
        s->rto = s->cfg.rto_min_ms;
    if (s->rto > s->cfg.rto_max_ms)
        s->rto = s->cfg.rto_max_ms;
}

static void rtt_sample(tsctp_t *s, uint32_t r)
{
    if (!s->have_rtt) {
        s->srtt = r;
        s->rttvar = r / 2;
        s->have_rtt = true;
    } else {
        uint32_t d = s->srtt > r ? s->srtt - r : r - s->srtt;
        s->rttvar = (3 * s->rttvar + d) / 4;
        s->srtt = (7 * s->srtt + r) / 8;
    }
    s->rto = s->srtt + max32(4 * s->rttvar, 1);
    rto_clamp(s);
}

static void rto_backoff(tsctp_t *s)
{
    s->rto = s->rto > s->cfg.rto_max_ms / 2 ? s->cfg.rto_max_ms : s->rto * 2;
    rto_clamp(s);
}

static uint32_t initial_cwnd(const tsctp_t *s) { return min32(4u * s->cfg.mtu, max32(2u * s->cfg.mtu, 4380)); }

static void path_reset(tsctp_t *s)
{
    s->rto = s->cfg.rto_initial_ms;
    rto_clamp(s);
    s->have_rtt = false;
    s->srtt = s->rttvar = 0;
    s->cwnd = initial_cwnd(s);
    s->pba = 0;
    s->errors = 0;
    s->fast_recovery = false;
    s->hb_out = false;
    s->thb_timeout = NEVER;
}

static void hb_schedule(tsctp_t *s, uint64_t now)
{
    if (!s->cfg.hb_interval_ms) {
        s->thb = NEVER;
        return;
    }
    int64_t jitter = (int64_t)(rnd(s) % (s->rto + 1)) - (int64_t)(s->rto / 2);
    s->thb = now + s->cfg.hb_interval_ms + s->rto + jitter;
}

/* ── queues ───────────────────────────────────────────────────────────────── */

static void free_list(tsctp_t *s, chunk_t *c)
{
    while (c) {
        chunk_t *n = c->next;
        xfree(s, c);
        c = n;
    }
}

static void drop_queues(tsctp_t *s)
{
    free_list(s, s->q_head);
    free_list(s, s->o_head);
    s->q_head = s->q_tail = s->o_head = s->o_tail = NULL;
    s->q_bytes = s->o_bytes = s->q_msgs = 0;
    for (int i = 0; i < W; i++) {
        xfree(s, s->rx[i].data);
        memset(&s->rx[i], 0, sizeof s->rx[i]);
    }
    s->rx_bytes = 0;
    s->ndups = 0;
    s->data_pkts = 0;
    s->sack_now = s->fwd_pending = s->rtx_force = s->rtt_pending = false;
    s->ctl_len = 0;
    s->t3 = s->tsack = NEVER;
}

static uint32_t flight(const tsctp_t *s)
{
    uint32_t f = 0;
    for (const chunk_t *c = s->o_head; c; c = c->next)
        if (c->st == CH_FLIGHT && c->rtx == RTX_NO)
            f += csize(c);
    return f;
}

static rxslot_t *slot(tsctp_t *s, uint32_t tsn) { return &s->rx[tsn % W]; }
static bool held(tsctp_t *s, uint32_t tsn)
{
    const rxslot_t *r = slot(s, tsn);
    return r->st != RX_EMPTY && r->tsn == tsn;
}

static uint32_t rwnd(const tsctp_t *s) { return s->cfg.rcvbuf > s->rx_bytes ? s->cfg.rcvbuf - s->rx_bytes : 0; }

/* Both ends' tags and TSNs are known: set up sending and receiving. */
static void tcb_init(tsctp_t *s, uint32_t my_vtag, uint32_t peer_vtag, uint32_t my_itsn, uint32_t peer_itsn,
                     uint32_t peer_rwnd, uint16_t peer_os, uint16_t peer_mis, bool pr)
{
    s->my_vtag = my_vtag;
    s->peer_vtag = peer_vtag;
    s->my_itsn = my_itsn;
    s->nout = (uint16_t)min32(min32(s->cfg.streams_out, peer_mis), TSCTP_MAX_STREAMS);
    s->nin = (uint16_t)min32(min32(s->cfg.streams_in, peer_os), TSCTP_MAX_STREAMS);
    s->peer_pr = pr;
    s->next_tsn = my_itsn;
    s->cum_ack = s->adv_ack = my_itsn - 1;
    s->cum_in = peer_itsn - 1;
    s->rx_floor = peer_itsn;
    s->rx_high = peer_itsn - 1;
    memset(s->out_ssn, 0, sizeof s->out_ssn);
    memset(s->in_ssn, 0, sizeof s->in_ssn);
    s->peer_rwnd = peer_rwnd;
    s->ssthresh = peer_rwnd;
}

/* ── closing ──────────────────────────────────────────────────────────────── */

static void go_closed(tsctp_t *s, tsctp_reason_t why, uint64_t now)
{
    s->epoch++;
    drop_queues(s);
    xfree(s, s->cookie);
    s->cookie = NULL;
    s->t1 = s->t2 = s->t3 = s->tsack = s->thb = s->thb_timeout = NEVER;
    s->st = s->listening ? TSCTP_LISTEN : TSCTP_CLOSED;
    s->my_vtag = s->peer_vtag = 0;
    if (why == TSCTP_R_PEER_LOST || why == TSCTP_R_INIT_TIMEOUT)
        s->stats.failures++;
    if (s->connect_role && !s->local_close && s->cfg.reconnect_min_ms && why != TSCTP_R_LOCAL_ABORT) {
        s->treconnect = now + s->reconnect_ms;
        s->reconnect_ms = min32(s->reconnect_ms * 2, max32(s->cfg.reconnect_max_ms, s->cfg.reconnect_min_ms));
    }
    emit_event(s, TSCTP_EV_DOWN, why);
}

/* The peer is gone (too many retransmissions): tell it, in case it can hear us, and close. */
static void fail(tsctp_t *s, tsctp_reason_t why, uint64_t now)
{
    if (s->peer_vtag)
        send_bare(s, s->peer_vtag, CT_ABORT, 0, s->lport, s->pport);
    go_closed(s, why, now);
}

static bool count_error(tsctp_t *s, uint64_t now)
{
    if (++s->errors > s->cfg.max_retrans) {
        fail(s, TSCTP_R_PEER_LOST, now);
        return false;
    }
    return true;
}

/* ── association setup ────────────────────────────────────────────────────── */

/* INIT / INIT ACK's optional parameters: PR-SCTP (Forward-TSN-Supported and Supported Extensions). */
static size_t put_pr_params(uint8_t *b)
{
    put16(b, P_FWD_TSN_SUPPORTED);
    put16(b + 2, 4);
    put16(b + 4, P_SUPPORTED_EXT);
    put16(b + 6, 5);
    b[8] = CT_FORWARD_TSN;
    b[9] = b[10] = b[11] = 0;
    return 12;
}

static void send_init(tsctp_t *s)
{
    pk_begin(s, 0);
    uint8_t *b = pk_chunk(s, CT_INIT, 0, 16 + 12);
    put32(b, s->my_vtag);
    put32(b + 4, rwnd(s));
    put16(b + 8, s->cfg.streams_out);
    put16(b + 10, (uint16_t)min32(s->cfg.streams_in, TSCTP_MAX_STREAMS));
    put32(b + 12, s->my_itsn);
    put_pr_params(b + 16);
    pk_flush(s);
}

static void send_cookie_echo(tsctp_t *s)
{
    pk_begin(s, s->peer_vtag);
    s->tx_limit = s->tx_cap; /* a cookie may be bigger than our MTU: IP fragments it */
    uint8_t *b = pk_chunk(s, CT_COOKIE_ECHO, 0, s->cookie_len);
    if (b) {
        memcpy(b, s->cookie, s->cookie_len);
        pk_flush(s);
    }
    s->tx_len = 0;
}

static void start_assoc(tsctp_t *s, uint64_t now)
{
    drop_queues(s);
    path_reset(s);
    s->my_vtag = rnd_tag(s);
    s->my_itsn = rnd(s);
    s->peer_vtag = 0;
    s->st = TSCTP_COOKIE_WAIT;
    s->init_tries = 0;
    s->treconnect = NEVER;
    send_init(s);
    s->t1 = now + s->rto;
}

static void established(tsctp_t *s, uint64_t now, tsctp_event_t ev)
{
    s->st = TSCTP_ESTABLISHED;
    s->t1 = NEVER;
    xfree(s, s->cookie);
    s->cookie = NULL;
    s->reconnect_ms = s->cfg.reconnect_min_ms;
    s->last_tx = now;
    hb_schedule(s, now);
    if (ev == TSCTP_EV_RESTART)
        s->stats.restarts++;
    else
        s->stats.established++;
    emit_event(s, ev, TSCTP_R_NONE);
}

/* PR-SCTP support from an INIT / INIT ACK's parameters; the cookie too, if there is one. */
static bool parse_params(const uint8_t *p, size_t n, const uint8_t **cookie, uint16_t *cookie_len)
{
    bool pr = false;
    while (n >= 4) {
        uint16_t type = get16(p), len = get16(p + 2);
        if (len < 4 || len > n)
            break;
        if (type == P_FWD_TSN_SUPPORTED)
            pr = true;
        else if (type == P_SUPPORTED_EXT)
            for (int i = 4; i < len; i++)
                if (p[i] == CT_FORWARD_TSN)
                    pr = true;
        if (type == P_COOKIE && cookie) {
            *cookie = p + 4;
            *cookie_len = len - 4;
        }
        size_t step = pad4(len);
        if (step >= n)
            break;
        p += step;
        n -= step;
    }
    return pr;
}

/* An INIT: answer with an INIT ACK carrying a state cookie (RFC 9260 5.1, 5.2.1, 5.2.2). Nothing is kept. */
static void handle_init(tsctp_t *s, const uint8_t *c, size_t clen, uint32_t vtag, uint16_t sport, uint16_t dport,
                        uint64_t now)
{
    if (vtag != 0 || clen < 20)
        return;
    const uint8_t *b = c + 4;
    uint32_t init_tag = get32(b), a_rwnd = get32(b + 4), itsn = get32(b + 12);
    uint16_t os = get16(b + 8), mis = get16(b + 10);
    if (!init_tag || !os || !mis)
        return;
    bool pr = parse_params(b + 16, clen - 20, NULL, NULL);
    if (s->st == TSCTP_CLOSED) { /* out of the blue, nobody listening */
        s->stats.ootb++;
        send_bare(s, init_tag, CT_ABORT, 0, dport, sport);
        return;
    }
    if (s->st != TSCTP_LISTEN && sport != s->pport)
        return; /* one association: someone else waits until this one is gone */
    uint32_t my_tag, my_itsn, ltie = 0, ptie = 0;
    if (s->st == TSCTP_LISTEN) {
        my_tag = rnd_tag(s);
        my_itsn = rnd(s);
    } else if (s->st == TSCTP_COOKIE_WAIT || s->st == TSCTP_COOKIE_ECHOED) { /* INIT collision */
        my_tag = s->my_vtag;
        my_itsn = s->my_itsn;
        if (s->st == TSCTP_COOKIE_ECHOED) {
            ltie = s->my_vtag;
            ptie = s->peer_vtag;
        }
    } else { /* the peer may have restarted: new tags, ours as tie-tags */
        my_tag = rnd_tag(s);
        my_itsn = rnd(s);
        ltie = s->my_vtag;
        ptie = s->peer_vtag;
    }
    uint8_t ck[COOKIE_LEN];
    memcpy(ck, "TSC1", 4);
    put32(ck + 4, my_tag);
    put32(ck + 8, init_tag);
    put32(ck + 12, ltie);
    put32(ck + 16, ptie);
    put32(ck + 20, my_itsn);
    put32(ck + 24, itsn);
    put32(ck + 28, a_rwnd);
    put16(ck + 32, os);
    put16(ck + 34, mis);
    ck[36] = pr;
    ck[37] = ck[38] = ck[39] = 0;
    put16(ck + 40, sport);
    put16(ck + 42, dport);
    put64(ck + 44, now);
    put64(ck + 52, siphash(s->key, ck, 52));

    pk_begin_ports(s, init_tag, dport, sport);
    uint8_t *a = pk_chunk(s, CT_INIT_ACK, 0, 16 + 4 + COOKIE_LEN + 12);
    put32(a, my_tag);
    put32(a + 4, rwnd(s));
    put16(a + 8, s->cfg.streams_out);
    put16(a + 10, (uint16_t)min32(s->cfg.streams_in, TSCTP_MAX_STREAMS));
    put32(a + 12, my_itsn);
    put16(a + 16, P_COOKIE);
    put16(a + 18, 4 + COOKIE_LEN);
    memcpy(a + 20, ck, COOKIE_LEN);
    put_pr_params(a + 20 + COOKIE_LEN);
    pk_flush(s);
}

static void handle_init_ack(tsctp_t *s, const uint8_t *b, size_t blen, uint64_t now)
{
    if (s->st != TSCTP_COOKIE_WAIT || blen < 16)
        return;
    uint32_t peer_tag = get32(b), a_rwnd = get32(b + 4), itsn = get32(b + 12);
    uint16_t os = get16(b + 8), mis = get16(b + 10);
    if (!peer_tag || !os || !mis)
        return;
    const uint8_t *ck = NULL;
    uint16_t cklen = 0;
    bool pr = parse_params(b + 16, blen - 16, &ck, &cklen);
    if (!ck || !cklen || (size_t)cklen + 16 + HDR > s->tx_cap)
        return; /* no usable cookie: T1 sends the INIT again */
    s->cookie = xalloc(s, cklen);
    if (!s->cookie)
        return;
    memcpy(s->cookie, ck, cklen);
    s->cookie_len = cklen;
    tcb_init(s, s->my_vtag, peer_tag, s->my_itsn, itsn, a_rwnd, os, mis, pr);
    s->st = TSCTP_COOKIE_ECHOED;
    s->init_tries = 0;
    send_cookie_echo(s);
    s->t1 = now + s->rto;
}

/* A COOKIE ECHO: a new association, a restart, or a collision (RFC 9260 5.1 D, 5.2.4). True if it's
   now established, and the chunks after it can be taken. */
static bool handle_cookie_echo(tsctp_t *s, const uint8_t *ck, size_t len, uint32_t vtag, uint16_t sport,
                               uint64_t now)
{
    if (len != COOKIE_LEN || memcmp(ck, "TSC1", 4) || get64(ck + 52) != siphash(s->key, ck, 52))
        return false;
    uint32_t my_tag = get32(ck + 4), peer_tag = get32(ck + 8), ltie = get32(ck + 12), ptie = get32(ck + 16);
    uint32_t my_itsn = get32(ck + 20), peer_itsn = get32(ck + 24), a_rwnd = get32(ck + 28);
    uint16_t os = get16(ck + 32), mis = get16(ck + 34);
    bool pr = ck[36] != 0;
    if (my_tag != vtag || get16(ck + 40) != sport || get16(ck + 42) != s->lport)
        return false;
    uint64_t made = get64(ck + 44);
    if (now < made || now - made > s->cfg.cookie_life_ms) {
        uint8_t stale[4];
        put32(stale, (uint32_t)min64((now - made - s->cfg.cookie_life_ms) * 1000, UINT32_MAX));
        pk_begin_ports(s, peer_tag, s->lport, sport);
        uint8_t *b = pk_chunk(s, CT_ERROR, 0, 8);
        put16(b, E_STALE_COOKIE);
        put16(b + 2, 8);
        memcpy(b + 4, stale, 4);
        pk_flush(s);
        return false;
    }
    if (s->st == TSCTP_LISTEN) {
        drop_queues(s);
        path_reset(s);
        s->pport = sport;
        tcb_init(s, my_tag, peer_tag, my_itsn, peer_itsn, a_rwnd, os, mis, pr);
        established(s, now, TSCTP_EV_UP);
    } else {
        bool lm = my_tag == s->my_vtag, pm = peer_tag == s->peer_vtag;
        if (!lm && !pm && ltie == s->my_vtag && ptie == s->peer_vtag) { /* A: the peer restarted */
            drop_queues(s);
            path_reset(s);
            tcb_init(s, my_tag, peer_tag, my_itsn, peer_itsn, a_rwnd, os, mis, pr);
            s->t2 = NEVER;
            established(s, now, TSCTP_EV_RESTART);
        } else if (lm && !pm) { /* B: setup collision */
            if (s->st < TSCTP_ESTABLISHED) {
                tcb_init(s, my_tag, peer_tag, my_itsn, peer_itsn, a_rwnd, os, mis, pr);
                established(s, now, TSCTP_EV_UP);
            } else {
                s->peer_vtag = peer_tag;
            }
        } else if (lm && pm) { /* D: both ends sent a cookie, or ours was echoed again */
            if (s->st < TSCTP_ESTABLISHED)
                established(s, now, TSCTP_EV_UP);
        } else {
            return false; /* C, or anything else: discard */
        }
    }
    ctl_chunk(s, CT_COOKIE_ACK, 0, 0);
    return true;
}

/* ── receiving data ───────────────────────────────────────────────────────── */

static void note_dup(tsctp_t *s, uint32_t tsn)
{
    s->stats.dup_chunks++;
    if (s->ndups < MAX_DUPS)
        s->dups[s->ndups++] = tsn;
}

static void rx_advance(tsctp_t *s)
{
    while (held(s, s->cum_in + 1))
        s->cum_in++;
}

static void rx_data(tsctp_t *s, uint8_t flags, const uint8_t *b, size_t n)
{
    if (n <= 12)
        return; /* no user data */
    uint32_t tsn = get32(b), ppid = get32(b + 8);
    uint16_t sid = get16(b + 4), ssn = get16(b + 6);
    size_t len = n - 12;
    s->stats.chunks_received++;
    if (le32(tsn, s->cum_in) || held(s, tsn)) {
        note_dup(s, tsn);
        return;
    }
    if (tsn - s->rx_floor >= W || s->rx_bytes + len > s->cfg.rcvbuf || len > s->cfg.max_message) {
        s->stats.chunks_dropped_window++; /* not acknowledged: the peer sends it again */
        return;
    }
    rxslot_t *r = slot(s, tsn);
    memset(r, 0, sizeof *r);
    r->tsn = tsn;
    if (sid >= s->nin) {
        uint8_t info[4];
        put16(info, sid);
        put16(info + 2, 0);
        ctl_error(s, E_INVALID_STREAM, info, 4);
        r->st = RX_DONE; /* acknowledged and dropped */
    } else {
        r->data = xalloc(s, len);
        if (!r->data) {
            s->stats.chunks_dropped_window++;
            return;
        }
        memcpy(r->data, b + 12, len);
        r->len = (uint16_t)len;
        r->sid = sid;
        r->ssn = ssn;
        r->ppid = ppid;
        r->flags = flags & (DF_B | DF_E | DF_U);
        r->st = RX_HELD;
        s->rx_bytes += (uint32_t)len;
    }
    if (lt32(s->rx_high, tsn))
        s->rx_high = tsn;
    rx_advance(s);
}

/* Hand the message in TSNs [t, u] (all held) to the application. */
static void rx_emit(tsctp_t *s, uint32_t t, uint32_t u, size_t total)
{
    rxslot_t *first = slot(s, t);
    tsctp_msg_t m = {NULL, total, first->sid, first->ppid, (first->flags & DF_U) != 0};
    uint8_t *buf = NULL;
    if (t == u) {
        buf = first->data;
        first->data = NULL;
    } else if (total <= s->cfg.max_message && (buf = xalloc(s, total))) {
        size_t off = 0;
        for (uint32_t i = t;; i++) {
            rxslot_t *r = slot(s, i);
            memcpy(buf + off, r->data, r->len);
            off += r->len;
            if (i == u)
                break;
        }
    }
    for (uint32_t i = t;; i++) {
        rxslot_t *r = slot(s, i);
        s->rx_bytes -= r->len;
        xfree(s, r->data);
        r->data = NULL;
        r->len = 0;
        r->st = RX_DONE;
        if (i == u)
            break;
    }
    if (buf) {
        m.data = buf;
        s->stats.msgs_received++;
        s->stats.bytes_received += (uint32_t)total;
        if (s->cfg.on_message)
            s->cfg.on_message(s->cfg.user, &m);
        xfree(s, buf);
    }
}

/* Deliver every complete message that may go now: anything at or below the cumulative TSN (in TSN
   order), an unordered one, or an ordered one that's next on its stream. */
static void rx_deliver(tsctp_t *s)
{
    bool again = true;
    while (again) {
        again = false;
        for (uint32_t t = s->rx_floor; le32(t, s->rx_high) && t - s->rx_floor < W;) {
            rxslot_t *r = slot(s, t);
            if (r->st != RX_HELD || r->tsn != t || !(r->flags & DF_B)) {
                t++;
                continue;
            }
            uint32_t u = t;
            size_t total = r->len;
            bool whole = false;
            for (;;) {
                rxslot_t *q = slot(s, u);
                if (q->st != RX_HELD || q->tsn != u || (u != t && (q->flags & DF_B)))
                    break;
                if (u != t)
                    total += q->len;
                if (q->flags & DF_E) {
                    whole = true;
                    break;
                }
                if (u - t + 1 >= W)
                    break;
                u++;
            }
            if (!whole) {
                t++;
                continue;
            }
            uint16_t sid = r->sid, ssn = r->ssn;
            bool ordered = !(r->flags & DF_U);
            if (ordered && !le32(u, s->cum_in) && ssn != s->in_ssn[sid]) {
                t = u + 1;
                continue;
            }
            uint32_t epoch = s->epoch;
            rx_emit(s, t, u, total);
            if (s->epoch != epoch)
                return; /* closed from the callback */
            if (ordered && !lt16(ssn, s->in_ssn[sid]))
                s->in_ssn[sid] = ssn + 1;
            again = true;
            t = u + 1;
        }
    }
}

/* Free what's delivered at or below the cumulative TSN, and move rx_floor up. */
static void rx_reclaim(tsctp_t *s)
{
    uint32_t floor = s->cum_in + 1;
    for (int i = 0; i < W; i++) {
        rxslot_t *r = &s->rx[i];
        if (r->st == RX_EMPTY || !le32(r->tsn, s->cum_in))
            continue;
        if (r->st == RX_DONE)
            r->st = RX_EMPTY;
        else if (lt32(r->tsn, floor))
            floor = r->tsn;
    }
    s->rx_floor = floor;
}

/* FORWARD TSN: the peer gave up on everything up to `f` (RFC 3758 3.6). */
static void rx_forward(tsctp_t *s, const uint8_t *b, size_t n)
{
    if (n < 4)
        return;
    uint32_t f = get32(b);
    s->stats.fwd_tsn_received++;
    s->sack_now = true;
    if (le32(f, s->cum_in))
        return;
    s->cum_in = f;
    if (lt32(s->rx_high, f))
        s->rx_high = f;
    uint32_t epoch = s->epoch;
    rx_deliver(s);
    if (s->epoch != epoch)
        return;
    for (int i = 0; i < W; i++) { /* fragments of abandoned messages */
        rxslot_t *r = &s->rx[i];
        if (r->st == RX_HELD && le32(r->tsn, f)) {
            s->rx_bytes -= r->len;
            xfree(s, r->data);
            memset(r, 0, sizeof *r);
        }
    }
    for (size_t i = 4; i + 4 <= n; i += 4) {
        uint16_t sid = get16(b + i), ssn = get16(b + i + 2);
        if (sid < s->nin && !lt16(ssn, s->in_ssn[sid])) {
            s->in_ssn[sid] = ssn + 1;
            s->stats.skipped_msgs++;
        }
    }
    rx_advance(s);
}

static void add_sack(tsctp_t *s)
{
    uint16_t gaps[2 * MAX_GAPS];
    int ng = 0;
    uint32_t t = s->cum_in + 1;
    while (ng < MAX_GAPS && le32(t, s->rx_high)) {
        while (le32(t, s->rx_high) && !held(s, t))
            t++;
        if (!le32(t, s->rx_high))
            break;
        uint32_t start = t;
        while (le32(t, s->rx_high) && held(s, t))
            t++;
        gaps[2 * ng] = (uint16_t)(start - s->cum_in);
        gaps[2 * ng + 1] = (uint16_t)(t - 1 - s->cum_in);
        ng++;
    }
    uint8_t *b = pk_chunk(s, CT_SACK, 0, 12 + 4 * ng + 4 * s->ndups);
    if (!b)
        return;
    put32(b, s->cum_in);
    put32(b + 4, rwnd(s));
    put16(b + 8, (uint16_t)ng);
    put16(b + 10, (uint16_t)s->ndups);
    for (int i = 0; i < 2 * ng; i++)
        put16(b + 12 + 2 * i, gaps[i]);
    for (int i = 0; i < s->ndups; i++)
        put32(b + 12 + 4 * ng + 4 * i, s->dups[i]);
    s->ndups = 0;
    s->data_pkts = 0;
    s->sack_now = false;
    s->tsack = NEVER;
    s->stats.sacks_sent++;
}

/* ── sending data ─────────────────────────────────────────────────────────── */

/* The Advanced.Peer.Ack.Point up over abandoned chunks (RFC 3758 3.5 C1). */
static void advance_adv(tsctp_t *s)
{
    if (lt32(s->adv_ack, s->cum_ack))
        s->adv_ack = s->cum_ack;
    for (chunk_t *c = s->o_head; c; c = c->next) {
        if (le32(c->tsn, s->adv_ack))
            continue;
        if (c->tsn == s->adv_ack + 1 && c->st == CH_ABANDONED)
            s->adv_ack = c->tsn;
        else
            break;
    }
    if (lt32(s->cum_ack, s->adv_ack))
        s->fwd_pending = true;
}

static void q_remove_msg(tsctp_t *s, uint32_t msg)
{
    chunk_t **pp = &s->q_head, *prev = NULL;
    bool any = false;
    while (*pp) {
        chunk_t *c = *pp;
        if (c->msg == msg) {
            *pp = c->next;
            s->q_bytes -= c->len;
            any = true;
            xfree(s, c);
        } else {
            prev = c;
            pp = &c->next;
        }
    }
    s->q_tail = prev;
    if (any && s->q_msgs)
        s->q_msgs--;
}

static void abandon_msg(tsctp_t *s, uint32_t msg)
{
    for (chunk_t *c = s->o_head; c; c = c->next)
        if (c->msg == msg) {
            c->st = CH_ABANDONED;
            c->rtx = RTX_NO;
        }
    q_remove_msg(s, msg);
    s->stats.abandoned_msgs++;
}

/* PR-SCTP: give up on messages past their lifetime. A message none of which has gone out is simply
   dropped; one that has is abandoned (only if the peer takes FORWARD TSN). */
static void expire(tsctp_t *s, uint64_t now)
{
    bool changed = false;
    if (s->peer_pr)
        for (chunk_t *c = s->o_head; c; c = c->next)
            if (c->expires && c->expires <= now && c->st == CH_FLIGHT) {
                abandon_msg(s, c->msg);
                changed = true;
            }
    for (chunk_t *c = s->q_head; c;) {
        chunk_t *next = c->next;
        if (c->expires && c->expires <= now && (c->flags & DF_B)) {
            uint32_t msg = c->msg;
            while (next && next->msg == msg)
                next = next->next;
            q_remove_msg(s, msg);
            s->stats.abandoned_unsent++;
        }
        c = next;
    }
    if (changed)
        advance_adv(s);
}

static uint64_t next_expiry(const tsctp_t *s)
{
    uint64_t t = NEVER;
    for (const chunk_t *c = s->q_head; c; c = c->next)
        if (c->expires && (c->flags & DF_B))
            t = min64(t, c->expires);
    if (s->peer_pr)
        for (const chunk_t *c = s->o_head; c; c = c->next)
            if (c->expires && c->st == CH_FLIGHT)
                t = min64(t, c->expires);
    return t;
}

static void add_fwd(tsctp_t *s)
{
    uint16_t sids[TSCTP_MAX_STREAMS], ssns[TSCTP_MAX_STREAMS];
    int n = 0;
    for (chunk_t *c = s->o_head; c && le32(c->tsn, s->adv_ack); c = c->next) {
        if (c->st != CH_ABANDONED || (c->flags & DF_U))
            continue;
        int i = 0;
        while (i < n && sids[i] != c->sid)
            i++;
        if (i == n) {
            if (n == TSCTP_MAX_STREAMS)
                continue;
            sids[n] = c->sid;
            ssns[n++] = c->ssn;
        } else if (lt16(ssns[i], c->ssn)) {
            ssns[i] = c->ssn;
        }
    }
    uint8_t *b = pk_chunk(s, CT_FORWARD_TSN, 0, 4 + 4 * n);
    if (!b)
        return;
    put32(b, s->adv_ack);
    for (int i = 0; i < n; i++) {
        put16(b + 4 + 4 * i, sids[i]);
        put16(b + 6 + 4 * i, ssns[i]);
    }
    s->fwd_pending = false;
    s->stats.fwd_tsn_sent++;
}

static void put_data(tsctp_t *s, chunk_t *c)
{
    uint8_t *b = pk_chunk(s, CT_DATA, c->flags, 12 + c->len);
    put32(b, c->tsn);
    put16(b + 4, c->sid);
    put16(b + 6, c->ssn);
    put32(b + 8, c->ppid);
    memcpy(b + 12, c->data, c->len);
    s->stats.chunks_sent++;
}

static bool sending_state(const tsctp_t *s)
{
    return s->st == TSCTP_ESTABLISHED || s->st == TSCTP_SHUTDOWN_PENDING || s->st == TSCTP_SHUTDOWN_RECEIVED;
}

static bool any_rtx(const tsctp_t *s)
{
    for (const chunk_t *c = s->o_head; c; c = c->next)
        if (c->rtx != RTX_NO)
            return true;
    return false;
}

/* Send what's waiting: control chunks, a SACK, FORWARD TSN, retransmissions, then new data as the
   congestion and receive windows allow, bundled into as few packets as fit. */
static void transmit(tsctp_t *s, uint64_t now)
{
    if (s->busy || s->st < TSCTP_ESTABLISHED)
        return;
    expire(s, now);
    bool data = sending_state(s) && s->q_head;
    bool rtx = any_rtx(s);
    if (!s->ctl_len && !s->sack_now && !(s->fwd_pending && s->peer_pr) && !data && !rtx)
        return;
    uint32_t fl = flight(s);
    if (fl == 0 && now - s->last_tx > s->rto) /* idle: decay the window (RFC 9260 7.2.1) */
        s->cwnd = max32(min32(s->cwnd, max32(s->cwnd / 2, 4u * s->cfg.mtu)), initial_cwnd(s));
    pk_begin(s, s->peer_vtag);
    if (s->ctl_len) {
        memcpy(s->tx + s->tx_len, s->ctl, s->ctl_len);
        s->tx_len += s->ctl_len;
        s->ctl_len = 0;
    }
    if (s->sack_now || (s->tsack != NEVER && (data || rtx)))
        add_sack(s);
    if (s->fwd_pending && s->peer_pr) {
        if (!pk_fits(s, 4 + 4 * TSCTP_MAX_STREAMS)) {
            pk_flush(s);
            pk_begin(s, s->peer_vtag);
        }
        add_fwd(s);
    }
    bool force = s->rtx_force, sent_new = false, sent_rtx = false;
    int npk = 0;
    s->rtx_force = false;
    for (chunk_t *c = s->o_head; c; c = c->next) {
        if (c->rtx == RTX_NO || c->st != CH_FLIGHT)
            continue;
        if (!(fl < s->cwnd || (force && npk == 0)))
            break;
        if (!pk_fits(s, 12 + c->len)) {
            pk_flush(s);
            npk++;
            pk_begin(s, s->peer_vtag);
            if (!(fl < s->cwnd))
                break;
        }
        if (c->rtx == RTX_FAST)
            s->stats.retransmits_fast++;
        else
            s->stats.retransmits_t3++;
        c->rtx = RTX_NO;
        c->tx++;
        c->sent_at = now;
        put_data(s, c);
        fl += csize(c);
        sent_rtx = true;
    }
    while (sending_state(s) && s->q_head) {
        chunk_t *c = s->q_head;
        if (!(fl < s->cwnd || (force && npk == 0)))
            break;
        if (s->peer_rwnd < csize(c) && fl > 0)
            break;
        if (!pk_fits(s, 12 + c->len)) {
            pk_flush(s);
            npk++;
            pk_begin(s, s->peer_vtag);
            continue;
        }
        s->q_head = c->next;
        if (!s->q_head)
            s->q_tail = NULL;
        s->q_bytes -= c->len;
        if (c->flags & DF_E)
            s->q_msgs--;
        c->next = NULL;
        if (s->o_tail)
            s->o_tail->next = c;
        else
            s->o_head = c;
        s->o_tail = c;
        s->o_bytes += c->len;
        c->tsn = s->next_tsn++;
        c->st = CH_FLIGHT;
        c->tx = 1;
        c->sent_at = now;
        put_data(s, c);
        fl += csize(c);
        s->peer_rwnd = s->peer_rwnd > csize(c) ? s->peer_rwnd - csize(c) : 0;
        if (!s->rtt_pending) {
            s->rtt_pending = true;
            s->rtt_tsn = c->tsn;
        }
        sent_new = true;
    }
    pk_flush(s);
    if (sent_rtx || (sent_new && s->t3 == NEVER))
        s->t3 = now + s->rto;
    if (sent_new || sent_rtx) {
        s->last_tx = now;
        if (!s->hb_out)
            hb_schedule(s, now);
    }
}

/* SHUTDOWN once everything sent is acknowledged; SHUTDOWN ACK once the peer's SHUTDOWN is answered. */
static void send_shutdown(tsctp_t *s)
{
    pk_begin(s, s->peer_vtag);
    uint8_t *b = pk_chunk(s, CT_SHUTDOWN, 0, 4);
    put32(b, s->cum_in);
    pk_flush(s);
    s->sack_now = false;
    s->tsack = NEVER;
}

static void shutdown_check(tsctp_t *s, uint64_t now)
{
    if (s->busy || s->q_head || s->o_head)
        return;
    if (s->st == TSCTP_SHUTDOWN_PENDING) {
        send_shutdown(s);
        s->st = TSCTP_SHUTDOWN_SENT;
        s->t2 = now + s->rto;
        s->thb = NEVER;
    } else if (s->st == TSCTP_SHUTDOWN_RECEIVED) {
        send_bare(s, s->peer_vtag, CT_SHUTDOWN_ACK, 0, s->lport, s->pport);
        s->st = TSCTP_SHUTDOWN_ACK_SENT;
        s->t2 = now + s->rto;
        s->thb = NEVER;
    }
}

/* ── SACK ─────────────────────────────────────────────────────────────────── */

static void rtt_check(tsctp_t *s, const chunk_t *c, uint64_t now)
{
    if (s->rtt_pending && c->tsn == s->rtt_tsn) {
        s->rtt_pending = false;
        if (c->tx == 1) /* Karn: never from a retransmission */
            rtt_sample(s, (uint32_t)(now - c->sent_at));
    }
}

static void sack_in(tsctp_t *s, const uint8_t *b, size_t n, uint64_t now)
{
    if (n < 12)
        return;
    uint32_t cum = get32(b), a_rwnd = get32(b + 4);
    uint16_t ng = get16(b + 8), nd = get16(b + 10);
    if (12 + 4 * ((size_t)ng + nd) > n)
        return;
    s->stats.sacks_received++;
    if (lt32(cum, s->cum_ack) || !lt32(cum, s->next_tsn))
        return; /* old, or acknowledges what was never sent */
    uint32_t fl_before = flight(s), acked = 0, htna = 0;
    bool have_htna = false, cum_moved = lt32(s->cum_ack, cum);
    while (s->o_head && le32(s->o_head->tsn, cum)) {
        chunk_t *c = s->o_head;
        s->o_head = c->next;
        if (!s->o_head)
            s->o_tail = NULL;
        if (c->st == CH_FLIGHT) {
            if (c->rtx == RTX_NO)
                acked += csize(c);
            rtt_check(s, c, now);
            htna = c->tsn;
            have_htna = true;
        }
        s->o_bytes -= c->len;
        xfree(s, c);
    }
    s->cum_ack = cum;
    for (chunk_t *c = s->o_head; c; c = c->next) {
        uint32_t off = c->tsn - cum;
        bool in = false;
        for (int g = 0; g < ng && !in; g++)
            in = off >= get16(b + 12 + 4 * g) && off <= get16(b + 14 + 4 * g);
        if (in && c->st == CH_FLIGHT) {
            if (c->rtx == RTX_NO)
                acked += csize(c);
            c->st = CH_ACKED;
            c->rtx = RTX_NO;
            rtt_check(s, c, now);
            htna = c->tsn;
            have_htna = true;
        } else if (!in && c->st == CH_ACKED) { /* the peer reneged */
            c->st = CH_FLIGHT;
            c->rtx = RTX_T3;
        }
    }
    if (s->fast_recovery && !lt32(cum, s->fr_exit))
        s->fast_recovery = false;
    if (have_htna && ng > 0) /* miss indications below the highest TSN newly acked (RFC 9260 7.2.4) */
        for (chunk_t *c = s->o_head; c && lt32(c->tsn, htna); c = c->next) {
            if (c->st != CH_FLIGHT || c->rtx != RTX_NO || c->fast_done)
                continue;
            if (++c->miss >= 3) {
                c->rtx = RTX_FAST;
                c->fast_done = true;
                c->miss = 0;
                s->rtx_force = true;
                s->rtt_pending = false;
                if (!s->fast_recovery) {
                    s->ssthresh = max32(s->cwnd / 2, 4u * s->cfg.mtu);
                    s->cwnd = s->ssthresh;
                    s->pba = 0;
                    s->fast_recovery = true;
                    s->fr_exit = s->next_tsn - 1;
                }
            }
        }
    if (acked)
        s->errors = 0;
    if (cum_moved && !s->fast_recovery) {
        bool full = fl_before + s->cfg.mtu >= s->cwnd;
        if (s->cwnd <= s->ssthresh) {
            if (full)
                s->cwnd += min32(acked, s->cfg.mtu);
        } else {
            s->pba += acked;
            if (s->pba >= s->cwnd && full) {
                s->pba -= s->cwnd;
                s->cwnd += s->cfg.mtu;
            }
        }
    }
    uint32_t fl = flight(s);
    if (fl == 0)
        s->pba = 0;
    s->peer_rwnd = a_rwnd > fl ? a_rwnd - fl : 0;
    if (!s->o_head)
        s->t3 = NEVER;
    else if (cum_moved || s->t3 == NEVER)
        s->t3 = now + s->rto;
    advance_adv(s);
}

static void t3_expired(tsctp_t *s, uint64_t now)
{
    s->stats.t3_expiries++;
    if (!count_error(s, now))
        return;
    rto_backoff(s);
    s->ssthresh = max32(s->cwnd / 2, 4u * s->cfg.mtu);
    s->cwnd = s->cfg.mtu;
    s->pba = 0;
    s->fast_recovery = false;
    s->rtt_pending = false;
    for (chunk_t *c = s->o_head; c; c = c->next)
        if (c->st == CH_FLIGHT) {
            c->rtx = RTX_T3;
            c->miss = 0;
        }
    expire(s, now);
    advance_adv(s);
    s->rtx_force = true;
    s->t3 = s->o_head ? now + s->rto : NEVER;
}

/* ── packets in ───────────────────────────────────────────────────────────── */

static void hb_ack_in(tsctp_t *s, const uint8_t *b, size_t n, uint64_t now)
{
    if (n < 4 + HB_INFO_LEN - 4 || get16(b) != P_HB_INFO || get16(b + 2) < HB_INFO_LEN)
        return;
    uint64_t t = get64(b + 4);
    if (!s->hb_out || get32(b + 12) != s->hb_nonce)
        return;
    s->hb_out = false;
    s->thb_timeout = NEVER;
    s->errors = 0;
    s->stats.hb_acked++;
    if (t <= now)
        rtt_sample(s, (uint32_t)(now - t));
}

static void send_heartbeat(tsctp_t *s, uint64_t now)
{
    s->hb_nonce = rnd(s);
    pk_begin(s, s->peer_vtag);
    uint8_t *b = pk_chunk(s, CT_HEARTBEAT, 0, HB_INFO_LEN);
    put16(b, P_HB_INFO);
    put16(b + 2, HB_INFO_LEN);
    put64(b + 4, now);
    put32(b + 12, s->hb_nonce);
    put32(b + 16, 0);
    pk_flush(s);
    s->hb_out = true;
    s->thb_timeout = now + s->rto;
    s->stats.hb_sent++;
}

/* A packet for no association we have (RFC 9260 8.4). */
static void ootb(tsctp_t *s, uint8_t type, uint32_t vtag, uint16_t sport, uint16_t dport)
{
    s->stats.ootb++;
    if (type == CT_ABORT || type == CT_SHUTDOWN_COMPLETE || type == CT_COOKIE_ACK || type == CT_ERROR)
        return;
    send_bare(s, vtag, type == CT_SHUTDOWN_ACK ? CT_SHUTDOWN_COMPLETE : CT_ABORT, T_BIT, dport, sport);
}

static bool has_tcb(const tsctp_t *s) { return s->st >= TSCTP_COOKIE_WAIT; }

static void error_in(tsctp_t *s, const uint8_t *b, size_t n, uint64_t now)
{
    while (n >= 4) {
        uint16_t cause = get16(b), len = get16(b + 2);
        if (len < 4 || len > n)
            break;
        if (cause == E_STALE_COOKIE && s->st == TSCTP_COOKIE_ECHOED) {
            xfree(s, s->cookie);
            s->cookie = NULL;
            if (++s->init_tries > s->cfg.max_init_retrans) {
                go_closed(s, TSCTP_R_INIT_TIMEOUT, now);
                return;
            }
            s->st = TSCTP_COOKIE_WAIT;
            s->peer_vtag = 0;
            send_init(s);
            s->t1 = now + s->rto;
            return;
        }
        if (pad4(len) >= n)
            break;
        b += pad4(len);
        n -= pad4(len);
    }
}

void tsctp_input(tsctp_t *s, const uint8_t *p, size_t n, uint64_t now)
{
    if (n < HDR + 4)
        return;
    s->stats.pkts_in++;
    static const uint8_t zero[4];
    uint32_t got = (uint32_t)p[8] | (uint32_t)p[9] << 8 | (uint32_t)p[10] << 16 | (uint32_t)p[11] << 24;
    uint32_t crc = tsctp_crc32c(tsctp_crc32c(tsctp_crc32c(0, p, 8), zero, 4), p + HDR, n - HDR);
    if (crc != got) {
        s->stats.bad_checksum++;
        return;
    }
    uint16_t sport = get16(p), dport = get16(p + 2);
    uint32_t vtag = get32(p + 4);
    const uint8_t *c = p + HDR, *end = p + n;
    uint8_t first = c[0];
    if (dport != s->lport) {
        s->stats.ootb++;
        return;
    }
    if (first == CT_INIT) {
        handle_init(s, c, min32(get16(c + 2), (uint32_t)(end - c)), vtag, sport, dport, now);
        return;
    }
    if (!has_tcb(s) && !(first == CT_COOKIE_ECHO && s->st == TSCTP_LISTEN)) {
        ootb(s, first, vtag, sport, dport);
        return;
    }
    if (has_tcb(s) && sport != s->pport) {
        s->stats.ootb++;
        return;
    }
    s->busy++;
    bool got_data = false, checked = false;
    uint32_t epoch = s->epoch;
    while (end - c >= 4) {
        uint8_t type = c[0], flags = c[1];
        uint16_t clen = get16(c + 2);
        if (clen < 4 || clen > end - c)
            break;
        const uint8_t *b = c + 4;
        size_t blen = clen - 4u;
        if (type == CT_COOKIE_ECHO) {
            if (!handle_cookie_echo(s, b, blen, vtag, sport, now))
                break;
            checked = true;
        } else if (type == CT_ABORT || type == CT_SHUTDOWN_COMPLETE) {
            if (!((flags & T_BIT) ? vtag == s->peer_vtag && vtag : vtag == s->my_vtag)) {
                s->stats.bad_vtag++;
                break;
            }
            if (type == CT_ABORT) {
                s->stats.aborts_received++;
                go_closed(s, TSCTP_R_ABORTED, now);
            } else if (s->st == TSCTP_SHUTDOWN_ACK_SENT) {
                go_closed(s, TSCTP_R_SHUTDOWN, now);
            }
            break;
        } else if (type == CT_SHUTDOWN_ACK && s->st < TSCTP_ESTABLISHED) {
            ootb(s, type, vtag, sport, dport);
            break;
        } else if (!checked) {
            if (vtag != s->my_vtag) {
                s->stats.bad_vtag++;
                break;
            }
            checked = true;
        }
        switch (type) {
        case CT_COOKIE_ECHO:
        case CT_ABORT:
        case CT_SHUTDOWN_COMPLETE:
            break;
        case CT_DATA:
            if (s->st == TSCTP_ESTABLISHED || s->st == TSCTP_SHUTDOWN_PENDING || s->st == TSCTP_SHUTDOWN_SENT) {
                rx_data(s, flags, b, blen);
                got_data = true;
            }
            break;
        case CT_INIT_ACK:
            handle_init_ack(s, b, blen, now);
            break;
        case CT_SACK:
            if (s->st >= TSCTP_ESTABLISHED)
                sack_in(s, b, blen, now);
            break;
        case CT_HEARTBEAT: {
            s->stats.hb_received++;
            uint8_t *a = ctl_chunk(s, CT_HEARTBEAT_ACK, 0, blen);
            if (a)
                memcpy(a, b, blen);
            break;
        }
        case CT_HEARTBEAT_ACK:
            hb_ack_in(s, b, blen, now);
            break;
        case CT_COOKIE_ACK:
            if (s->st == TSCTP_COOKIE_ECHOED)
                established(s, now, TSCTP_EV_UP);
            break;
        case CT_SHUTDOWN:
            if (blen >= 4 && s->st >= TSCTP_ESTABLISHED) {
                uint8_t sack[12];
                put32(sack, get32(b));
                put32(sack + 4, s->peer_rwnd + flight(s));
                put32(sack + 8, 0);
                sack_in(s, sack, sizeof sack, now);
                if (s->st == TSCTP_ESTABLISHED || s->st == TSCTP_SHUTDOWN_PENDING) {
                    s->st = TSCTP_SHUTDOWN_RECEIVED;
                    s->thb = NEVER;
                } else if (s->st == TSCTP_SHUTDOWN_SENT || s->st == TSCTP_SHUTDOWN_ACK_SENT) {
                    send_bare(s, s->peer_vtag, CT_SHUTDOWN_ACK, 0, s->lport, s->pport);
                    s->st = TSCTP_SHUTDOWN_ACK_SENT;
                    s->t2 = now + s->rto;
                }
            }
            break;
        case CT_SHUTDOWN_ACK:
            if (s->st == TSCTP_SHUTDOWN_SENT || s->st == TSCTP_SHUTDOWN_ACK_SENT) {
                send_bare(s, s->peer_vtag, CT_SHUTDOWN_COMPLETE, 0, s->lport, s->pport);
                go_closed(s, TSCTP_R_SHUTDOWN, now);
            }
            break;
        case CT_ERROR:
            error_in(s, b, blen, now);
            break;
        case CT_FORWARD_TSN:
            if (s->st >= TSCTP_ESTABLISHED)
                rx_forward(s, b, blen);
            break;
        default: /* unknown: the top two bits of the type say what to do (RFC 9260 3.2) */
            if (type & 0x40)
                ctl_error(s, E_UNRECOGNIZED_CHUNK, c, clen);
            if (!(type & 0x80))
                goto done;
            break;
        }
        if (!has_tcb(s) || s->epoch != epoch)
            break;
        if ((size_t)(end - c) <= pad4(clen))
            break;
        c += pad4(clen);
    }
done:
    if (has_tcb(s) && s->epoch == epoch) {
        rx_deliver(s);
        if (has_tcb(s) && s->epoch == epoch) {
            rx_reclaim(s);
            if (got_data) {
                if (s->rx_high != s->cum_in || s->ndups || ++s->data_pkts >= 2)
                    s->sack_now = true;
                else if (s->tsack == NEVER)
                    s->tsack = now + s->cfg.sack_delay_ms;
                if (s->st == TSCTP_SHUTDOWN_SENT) { /* answer data with SACK and SHUTDOWN again */
                    s->sack_now = true;
                    s->t2 = now + s->rto;
                }
            }
        }
    }
    s->busy--;
    bool resend_shutdown = got_data && s->st == TSCTP_SHUTDOWN_SENT;
    transmit(s, now);
    if (resend_shutdown)
        send_shutdown(s);
    shutdown_check(s, now);
}

/* ── timers ───────────────────────────────────────────────────────────────── */

uint64_t tsctp_poll(tsctp_t *s, uint64_t now)
{
    s->busy++;
    if (s->st == TSCTP_CLOSED && s->treconnect <= now)
        start_assoc(s, now);
    if (s->t1 <= now) {
        if (++s->init_tries > s->cfg.max_init_retrans) {
            go_closed(s, TSCTP_R_INIT_TIMEOUT, now);
        } else {
            rto_backoff(s);
            if (s->st == TSCTP_COOKIE_WAIT)
                send_init(s);
            else if (s->st == TSCTP_COOKIE_ECHOED)
                send_cookie_echo(s);
            s->t1 = now + s->rto;
        }
    }
    if (s->t3 <= now)
        t3_expired(s, now);
    if (s->t2 <= now && count_error(s, now)) {
        rto_backoff(s);
        if (s->st == TSCTP_SHUTDOWN_SENT)
            send_shutdown(s);
        else if (s->st == TSCTP_SHUTDOWN_ACK_SENT)
            send_bare(s, s->peer_vtag, CT_SHUTDOWN_ACK, 0, s->lport, s->pport);
        s->t2 = now + s->rto;
    }
    if (s->tsack <= now) {
        s->sack_now = true;
        s->tsack = NEVER;
    }
    if (s->thb_timeout <= now) { /* a heartbeat unanswered for an RTO */
        s->hb_out = false;
        s->thb_timeout = NEVER;
        if (count_error(s, now))
            rto_backoff(s);
    }
    if (s->thb <= now && s->st == TSCTP_ESTABLISHED) {
        if (!s->hb_out)
            send_heartbeat(s, now);
        hb_schedule(s, now);
    }
    s->busy--;
    transmit(s, now);
    shutdown_check(s, now);
    uint64_t t = min64(min64(min64(s->t1, s->t2), min64(s->t3, s->tsack)),
                       min64(min64(s->thb, s->thb_timeout), s->treconnect));
    if (s->st >= TSCTP_ESTABLISHED)
        t = min64(t, next_expiry(s));
    return t;
}

/* ── the API ──────────────────────────────────────────────────────────────── */

void tsctp_config_defaults(tsctp_config_t *c)
{
    memset(c, 0, sizeof *c);
    c->streams_out = c->streams_in = 4;
    c->mtu = 1200;
    c->rto_initial_ms = 1000;
    c->rto_min_ms = 1000;
    c->rto_max_ms = 60000;
    c->hb_interval_ms = 30000;
    c->max_retrans = 10;
    c->max_init_retrans = 8;
    c->sack_delay_ms = 200;
    c->sndbuf = 65536;
    c->rcvbuf = 65536;
    c->max_message = 65536;
    c->cookie_life_ms = 60000;
}

tsctp_t *tsctp_new(const tsctp_config_t *cfg, uint64_t now)
{
    if (!cfg->output || !cfg->random)
        return NULL;
    tsctp_t *s = cfg->alloc ? cfg->alloc(sizeof *s) : malloc(sizeof *s);
    if (!s)
        return NULL;
    memset(s, 0, sizeof *s);
    s->cfg = *cfg;
    tsctp_config_t *c = &s->cfg;
    if (c->mtu < 512)
        c->mtu = 512;
    if (!c->streams_out)
        c->streams_out = 1;
    if (!c->streams_in)
        c->streams_in = 1;
    if (c->streams_out > TSCTP_MAX_STREAMS)
        c->streams_out = TSCTP_MAX_STREAMS;
    if (c->rto_min_ms < 1)
        c->rto_min_ms = 1;
    if (c->rto_max_ms < c->rto_min_ms)
        c->rto_max_ms = c->rto_min_ms;
    if (c->rcvbuf < 1500)
        c->rcvbuf = 1500;
    if (c->max_message > c->rcvbuf)
        c->max_message = c->rcvbuf;
    s->tx_cap = c->mtu > 1500 ? c->mtu : 1500;
    s->tx = xalloc(s, s->tx_cap);
    if (!s->tx) {
        xfree(s, s);
        return NULL;
    }
    for (int i = 0; i < 16; i += 4) {
        uint32_t r = rnd(s);
        memcpy(s->key + i, &r, 4);
    }
    s->lport = c->local_port ? c->local_port : (uint16_t)(49152 + rnd(s) % 16384);
    s->pport = c->peer_port;
    s->st = TSCTP_CLOSED;
    s->t1 = s->t2 = s->t3 = s->tsack = s->thb = s->thb_timeout = s->treconnect = NEVER;
    s->reconnect_ms = c->reconnect_min_ms;
    path_reset(s);
    (void)now;
    return s;
}

void tsctp_free(tsctp_t *s)
{
    if (!s)
        return;
    drop_queues(s);
    xfree(s, s->cookie);
    xfree(s, s->tx);
    xfree(s, s);
}

int tsctp_connect(tsctp_t *s, uint64_t now)
{
    if (s->st != TSCTP_CLOSED || !s->pport)
        return TSCTP_ERR_STATE;
    s->connect_role = true;
    s->local_close = false;
    s->reconnect_ms = s->cfg.reconnect_min_ms;
    start_assoc(s, now);
    return TSCTP_OK;
}

int tsctp_listen(tsctp_t *s)
{
    if (s->st != TSCTP_CLOSED)
        return TSCTP_ERR_STATE;
    s->listening = true;
    s->connect_role = false;
    s->st = TSCTP_LISTEN;
    return TSCTP_OK;
}

int tsctp_shutdown(tsctp_t *s, uint64_t now)
{
    s->local_close = true;
    s->treconnect = NEVER;
    if (s->st == TSCTP_COOKIE_WAIT || s->st == TSCTP_COOKIE_ECHOED) {
        tsctp_abort(s, now);
        return TSCTP_OK;
    }
    if (s->st != TSCTP_ESTABLISHED)
        return TSCTP_ERR_STATE;
    s->st = TSCTP_SHUTDOWN_PENDING;
    shutdown_check(s, now);
    return TSCTP_OK;
}

void tsctp_abort(tsctp_t *s, uint64_t now)
{
    s->local_close = true;
    s->treconnect = NEVER;
    s->listening = false;
    if (!has_tcb(s)) {
        s->st = TSCTP_CLOSED;
        return;
    }
    if (s->peer_vtag)
        send_bare(s, s->peer_vtag, CT_ABORT, 0, s->lport, s->pport);
    go_closed(s, TSCTP_R_LOCAL_ABORT, now);
}

int tsctp_send(tsctp_t *s, const void *data, size_t len, const tsctp_sendinfo_t *info, uint64_t now)
{
    static const tsctp_sendinfo_t dflt;
    if (!info)
        info = &dflt;
    if (s->st != TSCTP_ESTABLISHED) {
        s->stats.send_rejected++;
        return TSCTP_ERR_STATE;
    }
    if (!len || info->stream >= s->nout)
        return TSCTP_ERR_INVAL;
    if (s->q_bytes + s->o_bytes + len > s->cfg.sndbuf) {
        s->stats.send_rejected++;
        return TSCTP_ERR_NOBUFS;
    }
    size_t frag = (s->cfg.mtu - HDR - DATA_HDR) & ~(size_t)3;
    uint32_t msg = s->msg_seq++;
    uint16_t ssn = info->unordered ? 0 : s->out_ssn[info->stream];
    const uint8_t *p = data;
    chunk_t *head = NULL, *tail = NULL;
    for (size_t off = 0; off < len; off += frag) {
        size_t n = len - off < frag ? len - off : frag;
        chunk_t *c = xalloc(s, sizeof *c + n);
        if (!c) {
            free_list(s, head);
            return TSCTP_ERR_NOMEM;
        }
        memset(c, 0, sizeof *c);
        memcpy(c->data, p + off, n);
        c->len = (uint16_t)n;
        c->sid = info->stream;
        c->ssn = ssn;
        c->ppid = info->ppid;
        c->msg = msg;
        c->expires = info->lifetime_ms ? now + info->lifetime_ms : 0;
        c->flags = (uint8_t)((info->unordered ? DF_U : 0) | (off == 0 ? DF_B : 0) | (off + n == len ? DF_E : 0));
        c->st = CH_QUEUED;
        if (tail)
            tail->next = c;
        else
            head = c;
        tail = c;
    }
    if (s->q_tail)
        s->q_tail->next = head;
    else
        s->q_head = head;
    s->q_tail = tail;
    s->q_bytes += (uint32_t)len;
    s->q_msgs++;
    if (!info->unordered)
        s->out_ssn[info->stream]++;
    s->stats.msgs_sent++;
    s->stats.bytes_sent += (uint32_t)len;
    transmit(s, now);
    return TSCTP_OK;
}

tsctp_state_t tsctp_state(const tsctp_t *s) { return s->st; }

const char *tsctp_state_name(tsctp_state_t st)
{
    static const char *const names[] = {"closed",      "listen",        "cookie-wait",       "cookie-echoed",
                                        "established", "shutdown-pending", "shutdown-sent", "shutdown-received",
                                        "shutdown-ack-sent"};
    return (unsigned)st < sizeof names / sizeof names[0] ? names[st] : "?";
}

const char *tsctp_reason_name(tsctp_reason_t r)
{
    static const char *const names[] = {"none", "shutdown", "aborted", "init timeout", "peer lost", "local abort"};
    return (unsigned)r < sizeof names / sizeof names[0] ? names[r] : "?";
}

void tsctp_get_stats(const tsctp_t *s, tsctp_stats_t *o)
{
    *o = s->stats;
    o->srtt_ms = s->srtt;
    o->rttvar_ms = s->rttvar;
    o->rto_ms = s->rto;
    o->cwnd = s->cwnd;
    o->ssthresh = s->ssthresh;
    o->flight = flight(s);
    o->peer_rwnd = s->peer_rwnd;
    o->queued_bytes = s->q_bytes;
    o->queued_msgs = s->q_msgs;
    uint32_t n = 0;
    for (const chunk_t *c = s->o_head; c; c = c->next)
        n++;
    o->outstanding_chunks = n;
    o->error_count = s->errors;
    o->local_vtag = s->my_vtag;
    o->peer_vtag = s->peer_vtag;
    o->streams_out = s->nout;
    o->streams_in = s->nin;
    o->local_port = s->lport;
    o->peer_port = s->pport;
    o->peer_prsctp = s->peer_pr;
}

size_t tsctp_status_json(const tsctp_t *s, char *buf, size_t cap)
{
    tsctp_stats_t t;
    tsctp_get_stats(s, &t);
    int n = snprintf(buf, cap,
                     "{\"state\":\"%s\",\"srtt_ms\":%lu,\"rto_ms\":%lu,\"cwnd\":%lu,\"flight\":%lu,\"peer_rwnd\":%lu,"
                     "\"queued\":%lu,\"outstanding\":%lu,\"errors\":%lu,\"prsctp\":%s,\"streams\":[%u,%u],"
                     "\"pkts_in\":%lu,\"pkts_out\":%lu,\"msgs_sent\":%lu,\"msgs_received\":%lu,"
                     "\"retransmits\":%lu,\"fast_retransmits\":%lu,\"t3_expiries\":%lu,\"abandoned\":%lu,"
                     "\"dropped_unsent\":%lu,\"fwd_tsn_sent\":%lu,\"fwd_tsn_received\":%lu,\"dups\":%lu,"
                     "\"bad_checksum\":%lu,\"hb_sent\":%lu,\"hb_acked\":%lu,\"established\":%lu,\"restarts\":%lu,"
                     "\"failures\":%lu,\"aborts\":%lu,\"send_rejected\":%lu}",
                     tsctp_state_name(s->st), (unsigned long)t.srtt_ms, (unsigned long)t.rto_ms,
                     (unsigned long)t.cwnd, (unsigned long)t.flight, (unsigned long)t.peer_rwnd,
                     (unsigned long)t.queued_msgs, (unsigned long)t.outstanding_chunks, (unsigned long)t.error_count,
                     t.peer_prsctp ? "true" : "false", t.streams_out, t.streams_in, (unsigned long)t.pkts_in,
                     (unsigned long)t.pkts_out, (unsigned long)t.msgs_sent, (unsigned long)t.msgs_received,
                     (unsigned long)t.retransmits_t3, (unsigned long)t.retransmits_fast,
                     (unsigned long)t.t3_expiries, (unsigned long)t.abandoned_msgs,
                     (unsigned long)t.abandoned_unsent, (unsigned long)t.fwd_tsn_sent,
                     (unsigned long)t.fwd_tsn_received, (unsigned long)t.dup_chunks,
                     (unsigned long)t.bad_checksum, (unsigned long)t.hb_sent, (unsigned long)t.hb_acked,
                     (unsigned long)t.established, (unsigned long)t.restarts, (unsigned long)t.failures,
                     (unsigned long)t.aborts_received, (unsigned long)t.send_rejected);
    if (n < 0)
        return 0;
    return (size_t)n < cap ? (size_t)n : (cap ? cap - 1 : 0);
}
