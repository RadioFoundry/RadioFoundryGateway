// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * tinysctp edge cases: hand-built packets fed to one or two endpoints, a
 * counting allocator that can fail, and the calls' error returns. The
 * simulated-link tests in test_sctp.c cover the protocol's normal life; this
 * covers what a peer, a bad packet or a full heap can do to it: malformed and
 * unexpected INIT / INIT ACK / COOKIE ECHO, the INIT collision cases, DATA the
 * receiver refuses, SACK and FORWARD TSN oddities, shutdown races, timer
 * expiry, config clamps and out-of-memory.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinysctp.h"

static int checks, failures;

#define CHECK(cond, ...)                                                                                     \
    do {                                                                                                     \
        checks++;                                                                                            \
        if (!(cond)) {                                                                                       \
            failures++;                                                                                      \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                      \
            printf(__VA_ARGS__);                                                                             \
            printf("\n");                                                                                    \
        }                                                                                                    \
    } while (0)

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; }
static void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = (uint8_t)v; }

/* Chunk types and causes, as on the wire. */
enum { T_DATA = 0, T_INIT = 1, T_INIT_ACK = 2, T_SACK = 3, T_HB = 4, T_HB_ACK = 5, T_ABORT = 6, T_SHUTDOWN = 7,
       T_SHUTDOWN_ACK = 8, T_ERROR = 9, T_COOKIE_ECHO = 10, T_COOKIE_ACK = 11, T_SHUTDOWN_COMPLETE = 14,
       T_FWD_TSN = 192 };

static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;
static int g_zero_draws; /* the next draws return 0 */
static uint32_t rng_cb(void *ctx)
{
    (void)ctx;
    if (g_zero_draws > 0) {
        g_zero_draws--;
        return 0;
    }
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 16);
}

/* ── a counting allocator that can be told to fail ────────────────────────── */

static int g_allocs, g_frees, g_fail_at; /* g_fail_at: the call number (1-based, since reset) to fail; 0 never */
static int g_fail_from;                  /* fail every call from this number on; 0 never */
static void *count_alloc(size_t n)
{
    g_allocs++;
    if (g_allocs == g_fail_at || (g_fail_from && g_allocs >= g_fail_from))
        return NULL;
    return malloc(n);
}
static void count_free(void *p)
{
    if (p)
        g_frees++;
    free(p);
}
static void alloc_reset(void) { g_allocs = g_frees = g_fail_at = g_fail_from = 0; }

/* ── endpoints ────────────────────────────────────────────────────────────── */

#define MAXOUT 64
typedef struct {
    uint8_t d[1700];
    size_t n;
} pk_t;

typedef struct {
    tsctp_t *s;
    pk_t out[MAXOUT];
    int nout;       /* packets queued to go to the peer */
    int total_out;  /* every packet this end ever sent */
    int ups, downs, restarts;
    tsctp_reason_t reason;
    int msgs;
    uint8_t last[4096];
    size_t last_len, last_total; /* last_total: the message's real length (last holds up to 4096 bytes) */
    uint16_t last_stream;
    uint32_t last_ppid;
    bool last_unordered;
    int on_msg_close; /* abort / shutdown from inside on_message */
    int on_msg_send;  /* send a reply from inside on_message */
    bool sacked;      /* a SACK has gone out: sack_cum is its cumulative TSN ack */
    uint32_t sack_cum;
} E;

static uint64_t now = 1000;

static void out_cb(void *ctx, const uint8_t *pkt, size_t len)
{
    E *e = ctx;
    e->total_out++;
    for (size_t off = 12; off + 4 <= len; off += (size_t)(get16(pkt + off + 2) + 3u) & ~3u) {
        if (get16(pkt + off + 2) < 4)
            break;
        if (pkt[off] == 3 && off + 8 <= len) {
            e->sacked = true;
            e->sack_cum = get32(pkt + off + 4);
        }
    }
    if (e->nout < MAXOUT && len <= sizeof e->out[0].d) {
        memcpy(e->out[e->nout].d, pkt, len);
        e->out[e->nout++].n = len;
    }
}

static void msg_cb(void *user, const tsctp_msg_t *m)
{
    E *e = user;
    e->msgs++;
    e->last_total = m->len;
    e->last_len = m->len < sizeof e->last ? m->len : sizeof e->last;
    memcpy(e->last, m->data, e->last_len);
    e->last_stream = m->stream;
    e->last_ppid = m->ppid;
    e->last_unordered = m->unordered;
    if (e->on_msg_close == 1)
        tsctp_abort(e->s, now);
    if (e->on_msg_send)
        tsctp_send(e->s, "ok", 2, NULL, now);
}

static void ev_cb(void *user, tsctp_event_t ev, tsctp_reason_t why)
{
    E *e = user;
    if (ev == TSCTP_EV_UP)
        e->ups++;
    else if (ev == TSCTP_EV_RESTART)
        e->restarts++;
    else {
        e->downs++;
        e->reason = why;
    }
}

static void e_init(E *e, uint16_t lport, uint16_t pport, void (*tweak)(tsctp_config_t *))
{
    memset(e, 0, sizeof *e);
    tsctp_config_t cfg;
    tsctp_config_defaults(&cfg);
    cfg.local_port = lport;
    cfg.peer_port = pport;
    cfg.rto_initial_ms = 300;
    cfg.rto_min_ms = 100;
    cfg.rto_max_ms = 2000;
    cfg.hb_interval_ms = 0;
    cfg.sack_delay_ms = 50;
    cfg.output = out_cb;
    cfg.output_ctx = e;
    cfg.random = rng_cb;
    cfg.on_message = msg_cb;
    cfg.on_event = ev_cb;
    cfg.user = e;
    if (tweak)
        tweak(&cfg);
    e->s = tsctp_new(&cfg, now);
}

static void e_free(E *e)
{
    tsctp_free(e->s);
    e->s = NULL;
}

static void e_clear(E *e) { e->nout = 0; }

static tsctp_stats_t stats(E *e)
{
    tsctp_stats_t st;
    tsctp_get_stats(e->s, &st);
    return st;
}

/* Deliver what each end has queued to the other, until nothing moves. */
static void pump(E *a, E *b)
{
    for (int round = 0; round < 50 && (a->nout || b->nout); round++) {
        pk_t pa[MAXOUT], pb[MAXOUT];
        int na = a->nout, nb = b->nout;
        memcpy(pa, a->out, sizeof(pk_t) * (size_t)na);
        memcpy(pb, b->out, sizeof(pk_t) * (size_t)nb);
        a->nout = b->nout = 0;
        for (int i = 0; i < na; i++)
            tsctp_input(b->s, pa[i].d, pa[i].n, now);
        for (int i = 0; i < nb; i++)
            tsctp_input(a->s, pb[i].d, pb[i].n, now);
    }
}

/* Move the clock forward a millisecond at a time, pumping and polling. */
static void run(E *a, E *b, uint64_t ms)
{
    uint64_t end = now + ms;
    while (now < end) {
        pump(a, b);
        if (a->s)
            tsctp_poll(a->s, now);
        if (b->s)
            tsctp_poll(b->s, now);
        now++;
    }
    pump(a, b);
}

static bool connect_pair(E *a, E *b)
{
    tsctp_listen(b->s);
    tsctp_connect(a->s, now);
    run(a, b, 100);
    return tsctp_state(a->s) == TSCTP_ESTABLISHED && tsctp_state(b->s) == TSCTP_ESTABLISHED;
}

/* ── hand-built packets ───────────────────────────────────────────────────── */

typedef struct {
    uint8_t b[2000];
    size_t n;
} P;

static void p_begin(P *p, uint16_t sport, uint16_t dport, uint32_t vtag)
{
    memset(p, 0, sizeof *p);
    put16(p->b, sport);
    put16(p->b + 2, dport);
    put32(p->b + 4, vtag);
    p->n = 12;
}

/* A chunk with `len` in its length field (UINT16_MAX: the real one) and `blen` body bytes written. */
static void p_chunk_l(P *p, uint8_t type, uint8_t flags, const void *body, size_t blen, uint32_t len)
{
    p->b[p->n] = type;
    p->b[p->n + 1] = flags;
    put16(p->b + p->n + 2, (uint16_t)(len == UINT16_MAX ? 4 + blen : len));
    if (blen)
        memcpy(p->b + p->n + 4, body, blen);
    p->n += (4 + blen + 3) & ~(size_t)3;
}
static void p_chunk(P *p, uint8_t type, uint8_t flags, const void *body, size_t blen)
{
    p_chunk_l(p, type, flags, body, blen, UINT16_MAX);
}

static void p_end(P *p)
{
    memset(p->b + 8, 0, 4);
    uint32_t crc = tsctp_crc32c(0, p->b, p->n);
    p->b[8] = (uint8_t)crc;
    p->b[9] = (uint8_t)(crc >> 8);
    p->b[10] = (uint8_t)(crc >> 16);
    p->b[11] = (uint8_t)(crc >> 24);
}

static void inject(E *e, P *p)
{
    p_end(p);
    tsctp_input(e->s, p->b, p->n, now);
}

/* An INIT / INIT ACK body: tag, a_rwnd, streams out, streams in, initial TSN, then `params`. */
static size_t init_body(uint8_t *b, uint32_t tag, uint16_t os, uint16_t mis, uint32_t itsn, const uint8_t *params,
                        size_t plen)
{
    put32(b, tag);
    put32(b + 4, 65536);
    put16(b + 8, os);
    put16(b + 10, mis);
    put32(b + 12, itsn);
    if (plen)
        memcpy(b + 16, params, plen);
    return 16 + plen;
}

static void send_init(E *e, uint16_t sport, uint16_t dport, uint32_t hdr_vtag, uint32_t tag, uint16_t os, uint16_t mis,
                      const uint8_t *params, size_t plen)
{
    P p;
    uint8_t body[256];
    p_begin(&p, sport, dport, hdr_vtag);
    size_t n = init_body(body, tag, os, mis, 5000, params, plen);
    p_chunk(&p, T_INIT, 0, body, n);
    inject(e, &p);
}

/* The first chunk of type `type` in a captured packet, or NULL. `len` is its length field. */
static const uint8_t *find_chunk(const pk_t *k, uint8_t type, size_t *len)
{
    size_t off = 12;
    while (off + 4 <= k->n) {
        size_t l = get16(k->d + off + 2);
        if (l < 4)
            return NULL;
        if (k->d[off] == type) {
            if (len)
                *len = l;
            return k->d + off;
        }
        off += (l + 3) & ~(size_t)3;
    }
    return NULL;
}

/* Does any queued packet of `e` have a chunk of this type? */
static const uint8_t *out_has(E *e, uint8_t type)
{
    for (int i = 0; i < e->nout; i++) {
        const uint8_t *c = find_chunk(&e->out[i], type, NULL);
        if (c)
            return c;
    }
    return NULL;
}

/* The INIT ACK a listener answers an INIT with: tag it must be echoed under, and the cookie. */
static bool take_init_ack(E *e, uint32_t *my_tag, uint8_t cookie[60])
{
    for (int i = 0; i < e->nout; i++) {
        size_t len;
        const uint8_t *c = find_chunk(&e->out[i], T_INIT_ACK, &len);
        if (!c)
            continue;
        if (my_tag)
            *my_tag = get32(c + 4);
        const uint8_t *prm = c + 4 + 16;
        while (prm < c + len && get16(prm) != 7)
            prm += (get16(prm + 2) + 3) & ~3u;
        if (prm >= c + len)
            return false;
        if (cookie)
            memcpy(cookie, prm + 4, 60);
        return true;
    }
    return false;
}

static void send_cookie_echo(E *e, uint16_t sport, uint16_t dport, uint32_t vtag, const uint8_t *cookie, size_t clen)
{
    P p;
    p_begin(&p, sport, dport, vtag);
    p_chunk(&p, T_COOKIE_ECHO, 0, cookie, clen);
    inject(e, &p);
}

static void send_data(E *e, uint16_t sport, uint16_t dport, uint32_t vtag, uint8_t flags, uint32_t tsn, uint16_t sid,
                      uint16_t ssn, const void *data, size_t len)
{
    P p;
    uint8_t body[2000];
    p_begin(&p, sport, dport, vtag);
    put32(body, tsn);
    put16(body + 4, sid);
    put16(body + 6, ssn);
    put32(body + 8, 0);
    memcpy(body + 12, data, len);
    p_chunk(&p, T_DATA, flags, body, 12 + len);
    inject(e, &p);
}

/* ── config clamps, API returns, allocation ───────────────────────────────── */

static void tweak_clamps(tsctp_config_t *c)
{
    c->mtu = 100;
    c->streams_out = 0;
    c->streams_in = 0;
    c->rto_initial_ms = 0;
    c->rto_min_ms = 0;
    c->rto_max_ms = 0;
    c->rcvbuf = 10;
    c->max_message = 100000;
}

static void test_config(void)
{
    tsctp_config_t cfg;
    tsctp_config_defaults(&cfg);
    cfg.output = out_cb;
    cfg.random = rng_cb;
    tsctp_config_t bad = cfg;
    bad.output = NULL;
    CHECK(tsctp_new(&bad, now) == NULL, "no output, no instance");
    bad = cfg;
    bad.random = NULL;
    CHECK(tsctp_new(&bad, now) == NULL, "no random, no instance");

    E a, b;
    e_init(&a, 0, 200, tweak_clamps);
    e_init(&b, 200, 100, tweak_clamps);
    CHECK(a.s && b.s, "clamped configs are accepted");
    tsctp_stats_t st = stats(&a);
    CHECK(st.local_port >= 49152, "port 0 draws one from 49152-65535 (%u)", st.local_port);
    CHECK(st.rto_ms == 1, "RTO limits clamp to 1 ms at least (%u)", st.rto_ms);
    /* the ports have to match for the pair: rebuild with the drawn port */
    uint16_t drawn = st.local_port;
    e_free(&a);
    e_free(&b);
    e_init(&a, drawn, 200, tweak_clamps);
    e_init(&b, 200, drawn, tweak_clamps);
    tsctp_listen(b.s);
    tsctp_connect(a.s, now);
    const uint8_t *init = out_has(&a, T_INIT);
    CHECK(init && get32(init + 8) == 1500, "rcvbuf of 10 is raised to 1500 (a_rwnd %u)", init ? get32(init + 8) : 0);
    CHECK(init && get16(init + 12) == 1 && get16(init + 14) == 1, "zero streams become one");
    run(&a, &b, 200);
    CHECK(tsctp_state(a.s) == TSCTP_ESTABLISHED, "established with the clamped settings");
    st = stats(&a);
    CHECK(st.streams_out == 1 && st.streams_in == 1, "one stream each way");

    /* mtu 100 becomes 512: nothing bigger goes out, and a message is split */
    static uint8_t big[1400];
    for (size_t i = 0; i < sizeof big; i++)
        big[i] = (uint8_t)i;
    int before = a.total_out;
    CHECK(tsctp_send(a.s, big, sizeof big, NULL, now) == TSCTP_OK, "send 1400");
    size_t maxpkt = 0;
    for (int i = 0; i < a.nout; i++)
        if (a.out[i].n > maxpkt)
            maxpkt = a.out[i].n;
    CHECK(a.total_out - before >= 2 && maxpkt <= 512 && maxpkt > 400, "MTU raised to 512 (largest packet %zu)", maxpkt);
    run(&a, &b, 500);
    CHECK(b.msgs == 1 && b.last_len == 1400 && memcmp(b.last, big, 1400) == 0, "1400 bytes fit the raised 1500 buffer");
    /* a message above rcvbuf can't be held: max_message is clamped to it */
    static uint8_t huge[1600];
    memset(huge, 7, sizeof huge);
    CHECK(tsctp_send(a.s, huge, sizeof huge, NULL, now) == TSCTP_OK, "send 1600");
    run(&a, &b, 1000);
    st = stats(&b);
    CHECK(b.msgs == 1 && st.chunks_dropped_window > 0, "1600 bytes don't fit and are never delivered (%u drops)",
          st.chunks_dropped_window);
    e_free(&a);
    e_free(&b);

    /* too many streams are cut to TSCTP_MAX_STREAMS (16) */
    E c;
    tsctp_config_t big_streams;
    tsctp_config_defaults(&big_streams);
    big_streams.output = out_cb;
    big_streams.output_ctx = &c;
    big_streams.random = rng_cb;
    big_streams.local_port = 100;
    big_streams.peer_port = 200;
    big_streams.streams_out = 100;
    big_streams.streams_in = 100;
    memset(&c, 0, sizeof c);
    c.s = tsctp_new(&big_streams, now);
    tsctp_connect(c.s, now);
    init = out_has(&c, T_INIT);
    CHECK(init && get16(init + 12) == 16 && get16(init + 14) == 16, "100 streams asked, 16 offered");
    e_free(&c);
}

static void test_alloc_failures(void)
{
    tsctp_config_t cfg;
    tsctp_config_defaults(&cfg);
    cfg.output = out_cb;
    cfg.random = rng_cb;
    cfg.alloc = count_alloc;
    cfg.free = count_free;
    cfg.output_ctx = NULL;
    alloc_reset();
    g_fail_at = 1;
    CHECK(tsctp_new(&cfg, now) == NULL, "no memory for the state");
    alloc_reset();
    g_fail_at = 2;
    CHECK(tsctp_new(&cfg, now) == NULL, "no memory for the packet buffer");
    CHECK(g_allocs == 2 && g_frees == 1, "and the state is given back (%d allocs, %d frees)", g_allocs, g_frees);
}

static void tweak_counting(tsctp_config_t *c)
{
    c->alloc = count_alloc;
    c->free = count_free;
}

/* A full session with everything freed through the caller's allocator: nothing leaks. */
static void test_no_leaks(void)
{
    alloc_reset();
    E a, b;
    e_init(&a, 100, 200, tweak_counting);
    e_init(&b, 200, 100, tweak_counting);
    CHECK(connect_pair(&a, &b), "up");
    static uint8_t big[3000];
    memset(big, 1, sizeof big);
    tsctp_send(a.s, big, sizeof big, NULL, now);
    tsctp_send(a.s, big, 50, &(tsctp_sendinfo_t){1, 0, true, 0}, now);
    tsctp_send(b.s, "x", 1, NULL, now);
    run(&a, &b, 20);
    /* leave data queued and unacknowledged, a gap held at the receiver and a cookie out */
    tsctp_send(a.s, big, 2000, NULL, now);
    e_clear(&a);
    for (int i = 0; i < 3; i++)
        tsctp_send(a.s, big, 2000, NULL, now);
    e_clear(&a);
    e_free(&a);
    e_free(&b);
    CHECK(g_allocs > 10 && g_allocs == g_frees, "every allocation freed (%d / %d)", g_allocs, g_frees);

    /* a half-open client frees its cookie */
    alloc_reset();
    E c, d;
    e_init(&c, 100, 200, tweak_counting);
    e_init(&d, 200, 100, tweak_counting);
    tsctp_listen(d.s);
    tsctp_connect(c.s, now);
    tsctp_input(d.s, c.out[0].d, c.out[0].n, now); /* INIT -> INIT ACK */
    c.nout = 0;
    tsctp_input(c.s, d.out[0].d, d.out[0].n, now);
    d.nout = 0;
    CHECK(tsctp_state(c.s) == TSCTP_COOKIE_ECHOED, "cookie echoed");
    e_free(&c);
    e_free(&d);
    CHECK(g_allocs == g_frees, "the held cookie is freed (%d / %d)", g_allocs, g_frees);

    /* a send that fails halfway frees the fragments made so far */
    alloc_reset();
    e_init(&c, 100, 200, tweak_counting);
    e_init(&d, 200, 100, tweak_counting);
    connect_pair(&c, &d);
    int base = g_allocs;
    g_fail_at = base + 3; /* the third fragment of 4 */
    int r = tsctp_send(c.s, big, 3000, NULL, now);
    CHECK(r == TSCTP_ERR_NOMEM, "out of memory is reported (%d)", r);
    tsctp_stats_t st = stats(&c);
    CHECK(st.queued_msgs == 0 && st.queued_bytes == 0 && st.msgs_sent == 0, "and nothing is queued");
    CHECK(tsctp_send(c.s, big, 3000, NULL, now) == TSCTP_OK, "the next send works");
    e_free(&c);
    e_free(&d);
    CHECK(g_frees == g_allocs - 1, "the failed call is the only allocation not freed (%d / %d)", g_allocs, g_frees);
}

static void test_api_errors(void)
{
    tsctp_free(NULL); /* a no-op */
    E a;
    e_init(&a, 100, 0, NULL);
    CHECK(tsctp_connect(a.s, now) == TSCTP_ERR_STATE, "connect needs a peer port");
    CHECK(tsctp_state(a.s) == TSCTP_CLOSED, "still closed");
    CHECK(tsctp_shutdown(a.s, now) == TSCTP_ERR_STATE, "shutdown of a closed association");
    tsctp_abort(a.s, now); /* nothing to abort */
    CHECK(tsctp_state(a.s) == TSCTP_CLOSED && a.downs == 0 && a.total_out == 0, "abort of nothing: no event, no packet");
    CHECK(tsctp_listen(a.s) == TSCTP_OK && tsctp_state(a.s) == TSCTP_LISTEN, "listen");
    CHECK(tsctp_listen(a.s) == TSCTP_ERR_STATE, "listen twice");
    CHECK(tsctp_connect(a.s, now) == TSCTP_ERR_STATE, "connect while listening");
    CHECK(tsctp_shutdown(a.s, now) == TSCTP_ERR_STATE, "shutdown while listening");
    CHECK(tsctp_send(a.s, "x", 1, NULL, now) == TSCTP_ERR_STATE, "send before established");
    tsctp_stats_t st = stats(&a);
    CHECK(st.send_rejected == 1, "rejected sends are counted");
    tsctp_abort(a.s, now);
    CHECK(tsctp_state(a.s) == TSCTP_CLOSED, "abort stops listening");
    e_free(&a);

    E c;
    e_init(&c, 100, 200, NULL);
    CHECK(tsctp_connect(c.s, now) == TSCTP_OK && tsctp_state(c.s) == TSCTP_COOKIE_WAIT, "connect");
    CHECK(tsctp_connect(c.s, now) == TSCTP_ERR_STATE, "connect twice");
    CHECK(tsctp_listen(c.s) == TSCTP_ERR_STATE, "listen while connecting");
    CHECK(tsctp_shutdown(c.s, now) == TSCTP_OK && tsctp_state(c.s) == TSCTP_CLOSED, "shutdown while connecting is an abort");
    CHECK(c.downs == 1 && c.reason == TSCTP_R_LOCAL_ABORT, "and says so");
    e_free(&c);

    CHECK(!strcmp(tsctp_reason_name(TSCTP_R_PEER_LOST), "peer lost") && !strcmp(tsctp_reason_name(TSCTP_R_NONE), "none"),
          "reason names");
    CHECK(!strcmp(tsctp_reason_name((tsctp_reason_t)99), "?"), "unknown reason");
    CHECK(!strcmp(tsctp_state_name((tsctp_state_t)99), "?") && !strcmp(tsctp_state_name(TSCTP_ESTABLISHED), "established"),
          "state names");

    /* the JSON is cut to the buffer, and an empty buffer gets nothing */
    e_init(&a, 100, 200, NULL);
    char j[1024];
    size_t n = tsctp_status_json(a.s, j, sizeof j);
    CHECK(n > 100 && j[n] == 0 && j[n - 1] == '}', "full JSON (%zu)", n);
    char small[32];
    n = tsctp_status_json(a.s, small, sizeof small);
    CHECK(n == 31 && small[31] == 0 && strncmp(small, "{\"state\":\"closed\"", 16) == 0, "cut to cap - 1 (%zu)", n);
    CHECK(tsctp_status_json(a.s, small, 0) == 0, "no room, no length");
    e_free(&a);
}

/* ── INIT ─────────────────────────────────────────────────────────────────── */

static void test_init_rejects(void)
{
    E l;
    e_init(&l, 200, 0, NULL);
    tsctp_listen(l.s);
    /* a bad header tag, a short chunk, a zero initiate tag, zero streams: all dropped silently */
    send_init(&l, 100, 200, 0x1234, 0x1111, 4, 4, NULL, 0);
    CHECK(l.nout == 0, "INIT with a verification tag");
    P p;
    uint8_t body[16];
    p_begin(&p, 100, 200, 0);
    init_body(body, 0x1111, 4, 4, 5000, NULL, 0);
    p_chunk(&p, T_INIT, 0, body, 12);
    inject(&l, &p);
    CHECK(l.nout == 0, "INIT of 16 bytes");
    send_init(&l, 100, 200, 0, 0, 4, 4, NULL, 0);
    CHECK(l.nout == 0, "initiate tag 0");
    send_init(&l, 100, 200, 0, 0x1111, 0, 4, NULL, 0);
    CHECK(l.nout == 0, "no outbound streams");
    send_init(&l, 100, 200, 0, 0x1111, 4, 0, NULL, 0);
    CHECK(l.nout == 0, "no inbound streams");
    send_init(&l, 100, 200, 0, 0x1111, 4, 4, NULL, 0);
    CHECK(l.nout == 1 && out_has(&l, T_INIT_ACK), "and a good one is answered");
    CHECK(get32(l.out[0].d + 4) == 0x1111, "under the INIT's tag");
    CHECK(get16(l.out[0].d) == 200 && get16(l.out[0].d + 2) == 100, "ports swapped");
    e_free(&l);

    /* an INIT to an endpoint that does nothing: ABORT under its tag */
    E c;
    e_init(&c, 200, 0, NULL);
    send_init(&c, 100, 200, 0, 0x4321, 4, 4, NULL, 0);
    const uint8_t *ab = out_has(&c, T_ABORT);
    CHECK(c.nout == 1 && ab && ab[1] == 0 && get32(c.out[0].d + 4) == 0x4321, "ABORT, no T bit, the INIT's tag");
    CHECK(stats(&c).ootb == 1, "counted");
    e_free(&c);
}

/* PR-SCTP is taken from either parameter, the others are ignored whatever their high bits. */
static void init_with_params(const uint8_t *params, size_t plen, bool expect_pr, bool expect_up, const char *what)
{
    E l;
    e_init(&l, 200, 0, NULL);
    tsctp_listen(l.s);
    send_init(&l, 100, 200, 0, 0x1111, 4, 4, params, plen);
    uint32_t tag;
    uint8_t ck[60];
    bool got = take_init_ack(&l, &tag, ck);
    CHECK(got, "%s: INIT ACK", what);
    if (got) {
        e_clear(&l);
        send_cookie_echo(&l, 100, 200, tag, ck, 60);
        CHECK(tsctp_state(l.s) == (expect_up ? TSCTP_ESTABLISHED : TSCTP_LISTEN), "%s: state %s", what,
              tsctp_state_name(tsctp_state(l.s)));
        if (expect_up) {
            CHECK(out_has(&l, T_COOKIE_ACK) != NULL, "%s: COOKIE ACK", what);
            CHECK(stats(&l).peer_prsctp == expect_pr, "%s: PR-SCTP %d", what, expect_pr);
        }
    }
    e_free(&l);
}

static void test_init_params(void)
{
    static const uint8_t none[1] = {0};
    init_with_params(none, 0, false, true, "no parameters");
    static const uint8_t fwd[4] = {0xC0, 0x00, 0x00, 0x04};
    init_with_params(fwd, 4, true, true, "Forward-TSN-Supported");
    static const uint8_t ext_fwd[8] = {0x80, 0x08, 0x00, 0x07, 0x82, 0xC0, 0x00, 0x00}; /* two extensions, one is 0xC0 */
    init_with_params(ext_fwd, 8, true, true, "Supported Extensions with FORWARD TSN");
    static const uint8_t ext_other[8] = {0x80, 0x08, 0x00, 0x06, 0x82, 0x83, 0x00, 0x00};
    init_with_params(ext_other, 8, false, true, "Supported Extensions without it");
    /* unknown parameters, one for each pair of high bits: none is reported or acted on (README: Not implemented) */
    static const uint8_t unk[16] = {0x00, 0x77, 0x00, 0x08, 1, 2, 3, 4,   /* 00: stop */
                                    0x40, 0x77, 0x00, 0x04, 0x80, 0x78, 0x00, 0x04}; /* 01: stop and report; 10: skip */
    init_with_params(unk, 16, false, true, "unknown parameters");
    static const uint8_t unk_skip_report[8] = {0xC0, 0x79, 0x00, 0x04, 0xC0, 0x00, 0x00, 0x04}; /* 11, then PR */
    init_with_params(unk_skip_report, 8, true, true, "skip-and-report unknown, then PR-SCTP");
    /* a parameter whose length is under 4, or past the end, ends the list */
    static const uint8_t short_len[8] = {0xC0, 0x7A, 0x00, 0x02, 0xC0, 0x00, 0x00, 0x04};
    init_with_params(short_len, 8, false, true, "a length under 4 ends the list");
    static const uint8_t long_len[8] = {0x80, 0x7B, 0x00, 0x40, 0xC0, 0x00, 0x00, 0x04};
    init_with_params(long_len, 8, false, true, "a length past the end ends the list");
    static const uint8_t trailing[7] = {0xC0, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00}; /* three bytes after */
    init_with_params(trailing, 7, true, true, "trailing bytes shorter than a parameter header");
}

/* ── INIT ACK ─────────────────────────────────────────────────────────────── */

static void send_init_ack(E *e, uint16_t sport, uint16_t dport, uint32_t vtag, uint32_t tag, uint16_t os, uint16_t mis,
                          size_t cookie_len, bool with_cookie, size_t chunk_body_len)
{
    P p;
    uint8_t body[2000];
    size_t n = init_body(body, tag, os, mis, 7000, NULL, 0);
    if (with_cookie) {
        put16(body + n, 7);
        put16(body + n + 2, (uint16_t)(4 + cookie_len));
        memset(body + n + 4, 0xCC, cookie_len);
        n += 4 + ((cookie_len + 3) & ~(size_t)3);
    }
    if (chunk_body_len)
        n = chunk_body_len;
    p_begin(&p, sport, dport, vtag);
    p_chunk(&p, T_INIT_ACK, 0, body, n);
    inject(e, &p);
}

static void test_init_ack(void)
{
    E a;
    e_init(&a, 100, 200, NULL);
    tsctp_connect(a.s, now);
    uint32_t tag = stats(&a).local_vtag;
    CHECK(tag != 0 && tsctp_state(a.s) == TSCTP_COOKIE_WAIT, "connecting");
    e_clear(&a);
    send_init_ack(&a, 200, 100, tag, 0x2222, 4, 4, 8, true, 12); /* only 12 body bytes */
    CHECK(tsctp_state(a.s) == TSCTP_COOKIE_WAIT && a.nout == 0, "INIT ACK under 16 bytes");
    send_init_ack(&a, 200, 100, tag, 0, 4, 4, 8, true, 0);
    CHECK(tsctp_state(a.s) == TSCTP_COOKIE_WAIT && a.nout == 0, "initiate tag 0");
    send_init_ack(&a, 200, 100, tag, 0x2222, 0, 4, 8, true, 0);
    CHECK(tsctp_state(a.s) == TSCTP_COOKIE_WAIT && a.nout == 0, "no outbound streams");
    send_init_ack(&a, 200, 100, tag, 0x2222, 4, 0, 8, true, 0);
    CHECK(tsctp_state(a.s) == TSCTP_COOKIE_WAIT && a.nout == 0, "no inbound streams");
    send_init_ack(&a, 200, 100, tag, 0x2222, 4, 4, 0, false, 0);
    CHECK(tsctp_state(a.s) == TSCTP_COOKIE_WAIT && a.nout == 0, "no cookie");
    send_init_ack(&a, 200, 100, tag, 0x2222, 4, 4, 0, true, 0);
    CHECK(tsctp_state(a.s) == TSCTP_COOKIE_WAIT && a.nout == 0, "an empty cookie");
    send_init_ack(&a, 200, 100, tag, 0x2222, 4, 4, 1500, true, 0);
    CHECK(tsctp_state(a.s) == TSCTP_COOKIE_WAIT && a.nout == 0, "a cookie too big to echo");
    send_init_ack(&a, 200, 100, tag ^ 1, 0x2222, 4, 4, 8, true, 0);
    CHECK(tsctp_state(a.s) == TSCTP_COOKIE_WAIT && a.nout == 0 && stats(&a).bad_vtag == 1, "wrong verification tag");
    send_init_ack(&a, 200, 100, tag, 0x2222, 4, 4, 8, true, 0);
    CHECK(tsctp_state(a.s) == TSCTP_COOKIE_ECHOED, "a good one is taken");
    const uint8_t *ce = out_has(&a, T_COOKIE_ECHO);
    CHECK(ce && get16(ce + 2) == 12 && ce[4] == 0xCC, "and its cookie echoed");
    e_clear(&a);
    send_init_ack(&a, 200, 100, tag, 0x3333, 4, 4, 8, true, 0);
    CHECK(tsctp_state(a.s) == TSCTP_COOKIE_ECHOED && a.nout == 0, "a second INIT ACK is ignored");
    e_free(&a);

    /* no memory for the cookie: the INIT is repeated by T1 and the next INIT ACK works */
    alloc_reset();
    e_init(&a, 100, 200, tweak_counting);
    tsctp_connect(a.s, now);
    tag = stats(&a).local_vtag;
    e_clear(&a);
    g_fail_at = g_allocs + 1;
    send_init_ack(&a, 200, 100, tag, 0x2222, 4, 4, 8, true, 0);
    CHECK(tsctp_state(a.s) == TSCTP_COOKIE_WAIT && a.nout == 0, "no memory for the cookie");
    send_init_ack(&a, 200, 100, tag, 0x2222, 4, 4, 8, true, 0);
    CHECK(tsctp_state(a.s) == TSCTP_COOKIE_ECHOED, "and the next try works");
    e_free(&a);
    CHECK(g_frees == g_allocs - 1, "nothing leaked (%d / %d, one call failed)", g_allocs, g_frees);
}

/* ── COOKIE ECHO ──────────────────────────────────────────────────────────── */

static void test_cookie_echo_rejects(void)
{
    E l;
    e_init(&l, 200, 0, NULL);
    tsctp_listen(l.s);
    send_init(&l, 100, 200, 0, 0x1111, 4, 4, NULL, 0);
    uint32_t tag;
    uint8_t ck[60];
    CHECK(take_init_ack(&l, &tag, ck), "cookie");
    e_clear(&l);
    send_cookie_echo(&l, 100, 200, tag, ck, 59);
    CHECK(tsctp_state(l.s) == TSCTP_LISTEN, "cookie of 59 bytes");
    send_cookie_echo(&l, 100, 200, tag, ck, 60 - 4); /* shorter still */
    CHECK(tsctp_state(l.s) == TSCTP_LISTEN, "cookie of 56 bytes");
    uint8_t bad[60];
    memcpy(bad, ck, 60);
    bad[0] = 'X';
    send_cookie_echo(&l, 100, 200, tag, bad, 60);
    CHECK(tsctp_state(l.s) == TSCTP_LISTEN, "wrong magic");
    memcpy(bad, ck, 60);
    bad[30] ^= 1;
    send_cookie_echo(&l, 100, 200, tag, bad, 60);
    CHECK(tsctp_state(l.s) == TSCTP_LISTEN, "altered cookie fails its MAC");
    memcpy(bad, ck, 60);
    bad[59] ^= 1;
    send_cookie_echo(&l, 100, 200, tag, bad, 60);
    CHECK(tsctp_state(l.s) == TSCTP_LISTEN, "altered MAC");
    send_cookie_echo(&l, 100, 200, tag ^ 0x100, ck, 60);
    CHECK(tsctp_state(l.s) == TSCTP_LISTEN, "wrong verification tag");
    send_cookie_echo(&l, 101, 200, tag, ck, 60);
    CHECK(tsctp_state(l.s) == TSCTP_LISTEN, "wrong source port");
    CHECK(l.nout == 0 && l.ups == 0, "nothing answered");
    /* a cookie from another instance (another key) is refused */
    E other;
    e_init(&other, 200, 0, NULL);
    tsctp_listen(other.s);
    send_cookie_echo(&other, 100, 200, tag, ck, 60);
    CHECK(tsctp_state(other.s) == TSCTP_LISTEN, "a cookie made by another instance");
    e_free(&other);
    /* a cookie dated in the future (the clock went back) is stale too */
    uint64_t saved = now;
    now -= 5000;
    send_cookie_echo(&l, 100, 200, tag, ck, 60);
    now = saved;
    const uint8_t *er = out_has(&l, T_ERROR);
    CHECK(tsctp_state(l.s) == TSCTP_LISTEN && er && get16(er + 4) == 3, "cookie from the future: stale cookie error");
    e_clear(&l);
    /* the real one still works */
    send_cookie_echo(&l, 100, 200, tag, ck, 60);
    CHECK(tsctp_state(l.s) == TSCTP_ESTABLISHED && l.ups == 1, "the good cookie");
    e_free(&l);
}

/* The setup collision cases (RFC 9260 5.2.4) with a cookie made while we were connecting ourselves. */
static void collide(E *x, uint8_t K[60], uint32_t T, uint32_t Pt, bool complete)
{
    e_init(x, 100, 200, NULL);
    tsctp_connect(x->s, now);
    uint32_t xt = stats(x).local_vtag;
    e_clear(x);
    send_init(x, 200, 100, 0, T, 4, 4, NULL, 0); /* the peer's own INIT: a collision */
    take_init_ack(x, NULL, K);
    e_clear(x);
    send_init_ack(x, 200, 100, xt, Pt, 4, 4, 8, true, 0);
    if (complete) {
        P pk;
        p_begin(&pk, 200, 100, xt);
        p_chunk(&pk, T_COOKIE_ACK, 0, NULL, 0);
        inject(x, &pk);
    }
}

static void test_cookie_collisions(void)
{
    uint8_t K[60];
    E x;
    /* B while we are still echoing: established with the cookie's tags */
    collide(&x, K, 0x1111, 0x2222, false);
    uint32_t xt = stats(&x).local_vtag;
    CHECK(tsctp_state(x.s) == TSCTP_COOKIE_ECHOED, "echoing");
    e_clear(&x);
    send_cookie_echo(&x, 200, 100, xt, K, 60);
    CHECK(tsctp_state(x.s) == TSCTP_ESTABLISHED && x.ups == 1, "B: up from the cookie");
    CHECK(stats(&x).peer_vtag == 0x1111 && out_has(&x, T_COOKIE_ACK), "with the cookie's peer tag, and a COOKIE ACK");
    e_free(&x);

    /* B when already established through the other exchange: only the peer tag changes */
    collide(&x, K, 0x1111, 0x2222, true);
    CHECK(tsctp_state(x.s) == TSCTP_ESTABLISHED && stats(&x).peer_vtag == 0x2222, "established the other way");
    e_clear(&x);
    send_cookie_echo(&x, 200, 100, xt = stats(&x).local_vtag, K, 60);
    CHECK(tsctp_state(x.s) == TSCTP_ESTABLISHED && x.ups == 1 && stats(&x).peer_vtag == 0x1111,
          "B: the peer tag is replaced, no second UP");
    CHECK(out_has(&x, T_COOKIE_ACK), "and acknowledged");
    e_clear(&x);
    /* D: the same cookie again is acknowledged and changes nothing */
    send_cookie_echo(&x, 200, 100, xt, K, 60);
    CHECK(tsctp_state(x.s) == TSCTP_ESTABLISHED && x.ups == 1 && x.restarts == 0 && out_has(&x, T_COOKIE_ACK),
          "D: a repeat is acknowledged");
    e_free(&x);

    /* D while echoing: both tags already match */
    collide(&x, K, 0x2222, 0x2222, false);
    e_clear(&x);
    send_cookie_echo(&x, 200, 100, stats(&x).local_vtag, K, 60);
    CHECK(tsctp_state(x.s) == TSCTP_ESTABLISHED && x.ups == 1, "D: up");
    e_free(&x);

    /* C: a cookie made for a new INIT with the tag we already have: neither restart nor collision, dropped */
    collide(&x, K, 0x1111, 0x2222, true);
    e_clear(&x);
    send_init(&x, 200, 100, 0, 0x2222, 4, 4, NULL, 0);
    uint32_t newtag;
    uint8_t K2[60];
    CHECK(take_init_ack(&x, &newtag, K2), "INIT ACK for a repeated INIT");
    e_clear(&x);
    send_cookie_echo(&x, 200, 100, newtag, K2, 60);
    CHECK(x.restarts == 0 && x.ups == 1 && !out_has(&x, T_COOKIE_ACK) && stats(&x).peer_vtag == 0x2222,
          "C: discarded, the association is as it was");
    e_free(&x);
}

/* ── DATA the receiver refuses or reorders ────────────────────────────────── */

static void tweak_small_msg(tsctp_config_t *c) { c->max_message = 100; }

static void test_data_refused(void)
{
    E a, b;
    e_init(&a, 100, 200, NULL);
    e_init(&b, 200, 100, tweak_small_msg);
    CHECK(connect_pair(&a, &b), "up");
    tsctp_stats_t sa = stats(&a), sb = stats(&b);
    uint32_t btag = sb.local_vtag;
    (void)sa;
    tsctp_send(a.s, "first", 5, NULL, now);
    run(&a, &b, 300);
    CHECK(b.msgs == 1 && b.sacked, "first message");
    uint32_t cum = b.sack_cum; /* b's cumulative TSN: the next fresh one is cum + 1 */
    e_clear(&b);
    /* a repeat of it is answered with a SACK that names the duplicate */
    send_data(&b, 100, 200, btag, 3, cum, 0, 0, "first", 5);
    const uint8_t *sk = out_has(&b, T_SACK);
    CHECK(sk != NULL, "a duplicate is answered with a SACK");
    CHECK(sk && get16(sk + 4 + 10) == 1 && get32(sk + 16) == cum, "naming the duplicate");
    e_clear(&b);

    /* DATA with no user data (a 12-byte body): ignored */
    uint32_t n0 = stats(&b).chunks_received;
    send_data(&b, 100, 200, btag, 3, cum + 1, 0, 0, "", 0);
    CHECK(stats(&b).chunks_received == n0 && b.msgs == 1, "DATA without data");

    /* a stream the association doesn't have: ERROR (invalid stream), but acknowledged */
    send_data(&b, 100, 200, btag, 3, cum + 1, 9, 0, "stray", 5);
    const uint8_t *er = out_has(&b, T_ERROR);
    CHECK(er && get16(er + 4) == 1 && get16(er + 6) == 8 && get16(er + 8) == 9, "invalid stream error for stream 9");
    sk = out_has(&b, T_SACK);
    CHECK(sk && get32(sk + 4) == cum + 1 && b.msgs == 1, "acknowledged and not delivered");
    e_clear(&b);

    /* beyond the window: not acknowledged, counted */
    uint32_t drops = stats(&b).chunks_dropped_window;
    send_data(&b, 100, 200, btag, 3, cum + 2 + 64 + 5, 0, 0, "far", 3);
    CHECK(stats(&b).chunks_dropped_window == drops + 1, "TSN beyond the window dropped");
    /* over max_message */
    static uint8_t blob[200];
    memset(blob, 3, sizeof blob);
    send_data(&b, 100, 200, btag, 3, cum + 2, 0, 0, blob, sizeof blob);
    CHECK(stats(&b).chunks_dropped_window == drops + 2 && b.msgs == 1, "a message over max_message dropped");
    /* an ordered message ahead of its SSN, behind a gap, waits; the gap filled, both come out in order */
    send_data(&b, 100, 200, btag, 3, cum + 3, 0, 2, "later", 5);
    CHECK(b.msgs == 1, "an ordered message ahead of its SSN is held");
    send_data(&b, 100, 200, btag, 3, cum + 2, 0, 1, "mid", 3);
    CHECK(b.msgs == 3 && memcmp(b.last, "later", 5) == 0, "and delivered after the gap is filled (%d)", b.msgs);
    e_free(&a);
    e_free(&b);
}

static void test_data_alloc_fail(void)
{
    alloc_reset();
    E a, b;
    e_init(&a, 100, 200, NULL);
    e_init(&b, 200, 100, tweak_counting);
    connect_pair(&a, &b);
    uint32_t btag = stats(&b).local_vtag;
    tsctp_send(a.s, "hello", 5, NULL, now);
    run(&a, &b, 300);
    uint32_t itsn = b.sack_cum;
    e_clear(&b);
    g_fail_at = g_allocs + 1;
    uint32_t drops = stats(&b).chunks_dropped_window;
    send_data(&b, 100, 200, btag, 3, itsn + 1, 0, 1, "nomem", 5);
    CHECK(stats(&b).chunks_dropped_window == drops + 1 && b.msgs == 1, "no memory for the chunk: dropped, not acknowledged");
    send_data(&b, 100, 200, btag, 3, itsn + 1, 0, 1, "again", 5);
    CHECK(b.msgs == 2 && memcmp(b.last, "again", 5) == 0, "and it is taken when sent again");
    /* a message of two fragments, with no memory to join them: dropped, and the stack goes on */
    e_clear(&b);
    g_fail_at = g_allocs + 1;
    send_data(&b, 100, 200, btag, 2, itsn + 2, 0, 2, "AA", 2);
    send_data(&b, 100, 200, btag, 1, itsn + 3, 0, 2, "BB", 2);
    CHECK(b.msgs == 2, "a message that cannot be joined is not delivered (%d)", b.msgs);
    e_free(&a);
    e_free(&b);
}

/* ── control chunks ───────────────────────────────────────────────────────── */

static void test_control_overflow(void)
{
    E a, b;
    e_init(&a, 100, 200, NULL);
    e_init(&b, 200, 100, NULL);
    connect_pair(&a, &b);
    uint32_t btag = stats(&b).local_vtag;
    e_clear(&b);
    /* more HEARTBEATs in one packet than the 256 bytes of waiting control chunks hold */
    P p;
    uint8_t info[20];
    p_begin(&p, 100, 200, btag);
    for (int i = 0; i < 14; i++) {
        memset(info, i, sizeof info);
        put16(info, 1);
        put16(info + 2, 20);
        p_chunk(&p, T_HB, 0, info, 20);
    }
    inject(&b, &p);
    tsctp_stats_t st = stats(&b);
    CHECK(st.hb_received == 14, "all 14 counted (%u)", st.hb_received);
    int acks = 0;
    for (size_t off = 12; b.nout && off + 4 <= b.out[0].n; off += (get16(b.out[0].d + off + 2) + 3) & ~3u)
        acks += b.out[0].d[off] == T_HB_ACK;
    CHECK(acks == 10, "the ones that fit are answered (%d)", acks);
    e_clear(&b);

    /* an unknown chunk with the "report" bit and an oversized body: the report is cut */
    uint8_t blob[400];
    memset(blob, 0x5A, sizeof blob);
    p_begin(&p, 100, 200, btag);
    p_chunk(&p, 0x45, 0, blob, 300);
    inject(&b, &p);
    const uint8_t *er = out_has(&b, T_ERROR);
    CHECK(er && get16(er + 4) == 6 && get16(er + 6) <= 256 - 4 && get16(er + 6) > 200, "unrecognized chunk report cut to fit (%u)",
          er ? get16(er + 6) : 0);
    e_clear(&b);
    /* a report that finds the queue full is dropped: fill it with heartbeats first */
    p_begin(&p, 100, 200, btag);
    for (int i = 0; i < 10; i++) {
        memset(info, i, sizeof info);
        put16(info, 1);
        put16(info + 2, 20);
        p_chunk(&p, T_HB, 0, info, 20);
    }
    p_chunk(&p, 0xC5, 0, "abcdefgh", 8); /* skip and report, but no room */
    inject(&b, &p);
    CHECK(out_has(&b, T_HB_ACK) && !out_has(&b, T_ERROR), "no room for the report: dropped");
    e_free(&a);
    e_free(&b);
}

/* ── timers: RTO limits, retransmission, error counts ─────────────────────── */

static void tweak_tight(tsctp_config_t *c)
{
    c->rto_initial_ms = 5000; /* above the maximum */
    c->rto_min_ms = 100;
    c->rto_max_ms = 400;
    c->max_retrans = 3;
}

static void test_rto_limits(void)
{
    E a;
    e_init(&a, 100, 200, tweak_tight);
    CHECK(stats(&a).rto_ms == 400, "an initial RTO above the maximum is cut to it (%u)", stats(&a).rto_ms);
    e_free(&a);
}

/* ── the lot of paths a live association is pushed through ────────────────── */


/* ── more hand-built input ────────────────────────────────────────────────── */

static void send_chunk(E *e, uint16_t sport, uint16_t dport, uint32_t vtag, uint8_t type, uint8_t flags,
                       const void *body, size_t blen)
{
    P p;
    p_begin(&p, sport, dport, vtag);
    p_chunk(&p, type, flags, body, blen);
    inject(e, &p);
}

/* Move what `from` has queued into `to`. */
static void deliver(E *from, E *to)
{
    pk_t q[MAXOUT];
    int n = from->nout;
    memcpy(q, from->out, sizeof(pk_t) * (size_t)n);
    from->nout = 0;
    for (int i = 0; i < n; i++)
        tsctp_input(to->s, q[i].d, q[i].n, now);
}

/* Poll one end alone, a millisecond at a time. */
static void tick(E *e, uint64_t ms)
{
    uint64_t end = now + ms;
    while (now < end) {
        tsctp_poll(e->s, now);
        now++;
    }
}

static int count_chunks(E *e, uint8_t type)
{
    int n = 0;
    for (int i = 0; i < e->nout; i++)
        for (size_t off = 12; off + 4 <= e->out[i].n; off += (size_t)(get16(e->out[i].d + off + 2) + 3u) & ~3u) {
            if (get16(e->out[i].d + off + 2) < 4)
                break;
            n += e->out[i].d[off] == type;
        }
    return n;
}

static void test_input_filters(void)
{
    E a, b;
    e_init(&a, 100, 200, NULL);
    e_init(&b, 200, 100, NULL);
    CHECK(connect_pair(&a, &b), "up");
    uint32_t btag = stats(&b).local_vtag, atag = stats(&a).local_vtag;
    e_clear(&b);
    uint8_t hb[20] = {0, 1, 0, 20};

    uint32_t ootb0 = stats(&b).ootb;
    send_chunk(&b, 100, 201, btag, T_HB, 0, hb, 20);
    CHECK(stats(&b).ootb == ootb0 + 1 && b.nout == 0 && stats(&b).hb_received == 0, "wrong destination port");
    send_chunk(&b, 101, 200, btag, T_HB, 0, hb, 20);
    CHECK(stats(&b).ootb == ootb0 + 2 && b.nout == 0 && stats(&b).hb_received == 0, "wrong source port");

    /* a chunk length under 4 or past the end stops the packet there */
    P p;
    p_begin(&p, 100, 200, btag);
    p_chunk_l(&p, T_HB, 0, hb, 20, 3);
    p_chunk(&p, T_HB, 0, hb, 20);
    inject(&b, &p);
    CHECK(stats(&b).hb_received == 0, "length 3");
    p_begin(&p, 100, 200, btag);
    p_chunk(&p, T_HB, 0, hb, 20);
    p_chunk_l(&p, T_HB, 0, hb, 20, 200);
    inject(&b, &p);
    CHECK(stats(&b).hb_received == 1, "the chunk before one that runs off the end is taken, the rest is not");
    e_clear(&b);

    /* ABORT and SHUTDOWN COMPLETE need the right tag (T bit: the sender's) */
    uint32_t bad0 = stats(&b).bad_vtag;
    send_chunk(&b, 100, 200, btag ^ 1, T_ABORT, 0, NULL, 0);
    send_chunk(&b, 100, 200, atag ^ 1, T_ABORT, 1, NULL, 0);
    send_chunk(&b, 100, 200, 0, T_ABORT, 1, NULL, 0);
    send_chunk(&b, 100, 200, btag ^ 1, T_SHUTDOWN_COMPLETE, 0, NULL, 0);
    CHECK(stats(&b).bad_vtag == bad0 + 4 && tsctp_state(b.s) == TSCTP_ESTABLISHED && b.downs == 0,
          "four ABORT / COMPLETE with the wrong tag are counted and ignored (%u)", stats(&b).bad_vtag - bad0);
    send_chunk(&b, 100, 200, btag, T_SHUTDOWN_COMPLETE, 0, NULL, 0);
    CHECK(tsctp_state(b.s) == TSCTP_ESTABLISHED && b.downs == 0, "SHUTDOWN COMPLETE when not waiting for one");
    send_chunk(&b, 100, 200, atag, T_ABORT, 1, NULL, 0);
    CHECK(tsctp_state(b.s) == TSCTP_LISTEN && b.downs == 1 && b.reason == TSCTP_R_ABORTED, "ABORT with the T bit and our peer's tag");
    CHECK(stats(&b).aborts_received == 1, "counted");
    e_free(&a);
    e_free(&b);

    /* SACK too short, or counting more blocks than it holds, is dropped; HEARTBEAT ACK likewise */
    e_init(&a, 100, 200, NULL);
    e_init(&b, 200, 100, NULL);
    connect_pair(&a, &b);
    btag = stats(&b).local_vtag;
    atag = stats(&a).local_vtag;
    uint8_t sk[16] = {0};
    send_chunk(&a, 200, 100, atag, T_SACK, 0, sk, 8);
    send_chunk(&a, 200, 100, atag, T_SACK, 0, (put16(sk + 8, 3), sk), 12);
    CHECK(stats(&a).sacks_received == 0, "a short SACK and one with missing blocks");
    memset(sk, 0, sizeof sk);
    put32(sk, 0x7FFFFFFF); /* acknowledges what was never sent */
    send_chunk(&a, 200, 100, atag, T_SACK, 0, sk, 12);
    CHECK(stats(&a).sacks_received == 1 && stats(&a).flight == 0, "a SACK beyond what was sent: counted, no effect");

    e_clear(&a);
    send_chunk(&a, 200, 100, atag, T_HB_ACK, 0, hb, 8);
    hb[1] = 2;
    send_chunk(&a, 200, 100, atag, T_HB_ACK, 0, hb, 20);
    hb[1] = 1;
    send_chunk(&a, 200, 100, atag, T_HB_ACK, 0, hb, 16 + 3); /* info param shorter than a full one */
    put16(hb + 2, 12);
    send_chunk(&a, 200, 100, atag, T_HB_ACK, 0, hb, 20);
    put16(hb + 2, 20);
    send_chunk(&a, 200, 100, atag, T_HB_ACK, 0, hb, 20); /* well formed, but none was sent */
    CHECK(stats(&a).hb_acked == 0 && stats(&a).error_count == 0, "bad or unsolicited HEARTBEAT ACKs");
    e_free(&a);
    e_free(&b);

    /* an association-less end: nothing to answer for ABORT, COOKIE ACK, ERROR, COMPLETE; the rest get an ABORT */
    e_init(&a, 200, 100, NULL);
    tsctp_listen(a.s);
    const uint8_t types[] = {T_ABORT, T_SHUTDOWN_COMPLETE, T_COOKIE_ACK, T_ERROR};
    for (size_t i = 0; i < sizeof types; i++)
        send_chunk(&a, 100, 200, 0x777, types[i], 0, NULL, 0);
    CHECK(a.nout == 0 && stats(&a).ootb == 4, "no reply to ABORT / COMPLETE / COOKIE ACK / ERROR");
    send_chunk(&a, 100, 200, 0x777, T_SHUTDOWN_ACK, 0, NULL, 0);
    const uint8_t *sc = out_has(&a, T_SHUTDOWN_COMPLETE);
    CHECK(sc && (sc[1] & 1) && get32(a.out[0].d + 4) == 0x777, "SHUTDOWN ACK is answered with SHUTDOWN COMPLETE, T bit");
    e_clear(&a);
    send_chunk(&a, 100, 200, 0x777, T_HB, 0, hb, 20);
    const uint8_t *ab = out_has(&a, T_ABORT);
    CHECK(ab && (ab[1] & 1), "anything else with an ABORT, T bit");
    e_free(&a);

    /* connecting: a SHUTDOWN ACK is out of the blue as well */
    e_init(&a, 100, 200, NULL);
    tsctp_connect(a.s, now);
    atag = stats(&a).local_vtag;
    e_clear(&a);
    send_chunk(&a, 200, 100, atag, T_SHUTDOWN_ACK, 0, NULL, 0);
    sc = out_has(&a, T_SHUTDOWN_COMPLETE);
    CHECK(sc && tsctp_state(a.s) == TSCTP_COOKIE_WAIT, "SHUTDOWN ACK while connecting");
    e_free(&a);
}

/* ── INIT in odd places ───────────────────────────────────────────────────── */

static void test_init_in_states(void)
{
    E a, b;
    e_init(&a, 100, 200, NULL);
    e_init(&b, 200, 100, NULL);
    connect_pair(&a, &b);
    e_clear(&b);
    send_init(&b, 999, 200, 0, 0x1111, 4, 4, NULL, 0);
    CHECK(b.nout == 0 && tsctp_state(b.s) == TSCTP_ESTABLISHED, "an INIT from another port is ignored while associated");
    e_free(&a);
    e_free(&b);

    /* an INIT while echoing a cookie: the cookie carries both tie-tags */
    E x;
    e_init(&x, 100, 200, NULL);
    tsctp_connect(x.s, now);
    uint32_t xt = stats(&x).local_vtag;
    e_clear(&x);
    send_init_ack(&x, 200, 100, xt, 0x2222, 4, 4, 8, true, 0);
    CHECK(tsctp_state(x.s) == TSCTP_COOKIE_ECHOED, "echoing");
    e_clear(&x);
    send_init(&x, 200, 100, 0, 0x1111, 4, 4, NULL, 0);
    uint8_t K[60];
    uint32_t t;
    CHECK(take_init_ack(&x, &t, K), "answered");
    CHECK(t == xt && get32(K + 4) == xt && get32(K + 8) == 0x1111 && get32(K + 12) == xt && get32(K + 16) == 0x2222,
          "cookie: our tag, theirs, and tie-tags %08x %08x", get32(K + 12), get32(K + 16));
    e_free(&x);

    /* the random source returns 0: tags are drawn again until they are not */
    e_init(&x, 100, 200, NULL);
    g_zero_draws = 3;
    tsctp_connect(x.s, now);
    const uint8_t *init = out_has(&x, T_INIT);
    CHECK(init && get32(init + 4) != 0 && g_zero_draws == 0, "a zero initiate tag is never used");
    e_free(&x);
}

/* ── receive side: window, callbacks that close, FORWARD TSN ──────────────── */

static void pair_up(E *a, E *b, void (*tb)(tsctp_config_t *))
{
    e_init(a, 100, 200, NULL);
    e_init(b, 200, 100, tb);
    connect_pair(a, b);
    tsctp_send(a->s, "z", 1, NULL, now); /* so b has a cumulative TSN to build on */
    run(a, b, 300);
    e_clear(a);
    e_clear(b);
}

static void test_rx_edges(void)
{
    E a, b;
    /* a message of more fragments than the receive window can hold can never be completed */
    pair_up(&a, &b, NULL);
    uint32_t btag = stats(&b).local_vtag, cum = b.sack_cum;
    for (int i = 1; i <= 64; i++)
        send_data(&b, 100, 200, btag, i == 1 ? 2 : 0, cum + (uint32_t)i, 0, 1, "f", 1);
    CHECK(b.msgs == 1 && stats(&b).msgs_received == 1, "64 fragments without an end: nothing delivered");
    uint32_t drops = stats(&b).chunks_dropped_window;
    send_data(&b, 100, 200, btag, 1, cum + 65, 0, 1, "e", 1);
    CHECK(stats(&b).chunks_dropped_window == drops + 1 && b.msgs == 1, "the 65th has no slot and is refused");
    e_free(&a);
    e_free(&b);

    /* a callback that closes the association stops the rest of the packet and the delivery loop */
    pair_up(&a, &b, NULL);
    btag = stats(&b).local_vtag;
    cum = b.sack_cum;
    b.on_msg_close = 1;
    send_data(&b, 100, 200, btag, 3, cum + 2, 0, 5, "second", 6); /* ahead of its SSN, behind a gap: held */
    CHECK(b.msgs == 1, "held");
    send_data(&b, 100, 200, btag, 3, cum + 1, 0, 1, "first!", 6);
    CHECK(b.msgs == 2 && b.downs == 1 && b.reason == TSCTP_R_LOCAL_ABORT && tsctp_state(b.s) == TSCTP_CLOSED,
          "closed from on_message: the next message is not delivered (%d msgs)", b.msgs);
    e_free(&a);
    e_free(&b);

    /* FORWARD TSN: too short; at or below what we have; skipping a gap closes it and frees dead fragments */
    pair_up(&a, &b, NULL);
    btag = stats(&b).local_vtag;
    cum = b.sack_cum;
    send_chunk(&b, 100, 200, btag, T_FWD_TSN, 0, "abc", 3);
    CHECK(stats(&b).fwd_tsn_received == 0, "FORWARD TSN of 3 bytes");
    uint8_t f[8];
    put32(f, cum);
    send_chunk(&b, 100, 200, btag, T_FWD_TSN, 0, f, 4);
    CHECK(stats(&b).fwd_tsn_received == 1 && out_has(&b, T_SACK), "FORWARD TSN at the cumulative ack: counted, answered with a SACK");
    e_clear(&b);
    send_data(&b, 100, 200, btag, 2, cum + 3, 0, 1, "head", 4); /* a first fragment, its end never comes */
    CHECK(out_has(&b, T_SACK) && get32(out_has(&b, T_SACK) + 8) == 65536 - 4, "held (4 bytes off the window)");
    e_clear(&b);
    tsctp_stats_t st0 = stats(&b);
    put32(f, cum + 4);
    put16(f + 4, 0);
    put16(f + 6, 1);
    send_chunk(&b, 100, 200, btag, T_FWD_TSN, 0, f, 8);
    tsctp_stats_t st1 = stats(&b);
    CHECK(st1.fwd_tsn_received == st0.fwd_tsn_received + 1 && st1.skipped_msgs == st0.skipped_msgs + 1,
          "the stream's SSN is moved past the abandoned message");
    const uint8_t *sk = out_has(&b, T_SACK);
    CHECK(sk && get32(sk + 4) == cum + 4 && get16(sk + 12) == 0, "cumulative ack jumps to the forwarded TSN, no gaps");
    /* the fragment's buffer was released: a_rwnd is full again */
    CHECK(sk && get32(sk + 8) == 65536, "and its bytes are no longer held (a_rwnd %u)", sk ? get32(sk + 8) : 0);
    e_free(&a);
    e_free(&b);

    /* a FORWARD TSN that releases a complete message whose callback closes the association */
    pair_up(&a, &b, NULL);
    btag = stats(&b).local_vtag;
    cum = b.sack_cum;
    b.on_msg_close = 1;
    send_data(&b, 100, 200, btag, 3, cum + 2, 0, 4, "late", 4); /* waits for cum + 1 */
    put32(f, cum + 2);
    send_chunk(&b, 100, 200, btag, T_FWD_TSN, 0, f, 4);
    CHECK(b.msgs == 2 && b.downs == 1 && tsctp_state(b.s) == TSCTP_CLOSED, "delivered by the FORWARD TSN, which closed it");
    e_free(&a);
    e_free(&b);
}

/* ── the sender: window, expiry and FORWARD TSN generation ────────────────── */

static void tweak_small_rcv(tsctp_config_t *c) { c->rcvbuf = 1500; }

static void test_send_edges(void)
{
    /* the peer's window closes: the sender holds back and goes on as it opens */
    E a, b;
    e_init(&a, 100, 200, NULL);
    e_init(&b, 200, 100, tweak_small_rcv);
    connect_pair(&a, &b);
    static uint8_t blob[1100];
    memset(blob, 9, sizeof blob);
    int before = a.total_out;
    for (int i = 0; i < 3; i++)
        tsctp_send(a.s, blob, sizeof blob, NULL, now);
    CHECK(a.total_out - before == 1 && stats(&a).queued_msgs == 2, "only what the 1500-byte window allows goes at once (%d packets)",
          a.total_out - before);
    run(&a, &b, 2000);
    CHECK(b.msgs == 3 && stats(&a).queued_msgs == 0, "the rest follows as the window opens (%d)", b.msgs);
    e_free(&a);
    e_free(&b);

    /* PR-SCTP: an unsent message of several fragments that has outlived its lifetime is dropped whole */
    e_init(&a, 100, 200, NULL);
    e_init(&b, 200, 100, NULL);
    connect_pair(&a, &b);
    static uint8_t big[8000];
    memset(big, 1, sizeof big);
    tsctp_send(a.s, big, 5000, NULL, now); /* fills the congestion window */
    tsctp_send(a.s, big, 3000, &(tsctp_sendinfo_t){0, 0, false, 10}, now); /* 3 fragments, 10 ms */
    tsctp_send(a.s, "tail", 4, NULL, now);
    tsctp_stats_t st = stats(&a);
    CHECK(st.queued_msgs == 3, "three messages waiting (%u)", st.queued_msgs);
    e_clear(&a);
    tick(&a, 30);
    st = stats(&a);
    CHECK(st.abandoned_unsent == 1 && st.queued_msgs == 2, "the expired one is dropped whole, the others stay (%u / %u)",
          st.abandoned_unsent, st.queued_msgs);
    e_free(&a);
    e_free(&b);

    /* sent and abandoned: FORWARD TSN lists ordered streams only, once each, and the peer follows it */
    e_init(&a, 100, 200, NULL);
    e_init(&b, 200, 100, NULL);
    connect_pair(&a, &b);
    tsctp_send(a.s, "u", 1, &(tsctp_sendinfo_t){0, 0, true, 50}, now);
    tsctp_send(a.s, "o1", 2, &(tsctp_sendinfo_t){1, 0, false, 50}, now);
    tsctp_send(a.s, "o1", 2, &(tsctp_sendinfo_t){1, 0, false, 50}, now);
    tsctp_send(a.s, "o2", 2, &(tsctp_sendinfo_t){2, 0, false, 50}, now);
    e_clear(&a); /* the link eats them */
    tick(&a, 80);
    const uint8_t *fw = out_has(&a, T_FWD_TSN);
    CHECK(fw != NULL, "FORWARD TSN sent");
    if (fw) {
        size_t len = get16(fw + 2);
        CHECK(len == 4 + 4 + 2 * 4, "two ordered streams listed (length %zu)", len);
        CHECK(get16(fw + 8) == 1 && get16(fw + 10) == 1 && get16(fw + 12) == 2 && get16(fw + 14) == 0,
              "stream 1 with its last SSN (1), stream 2 with 0");
    }
    st = stats(&a);
    CHECK(st.abandoned_msgs == 4 && st.fwd_tsn_sent >= 1, "four abandoned (%u)", st.abandoned_msgs);
    deliver(&a, &b);
    tsctp_stats_t sb = stats(&b);
    CHECK(sb.fwd_tsn_received == 1 && sb.skipped_msgs == 2, "the receiver skips them (%u)", sb.skipped_msgs);
    run(&a, &b, 500);
    tsctp_send(a.s, "next", 4, &(tsctp_sendinfo_t){1, 0, false, 0}, now);
    run(&a, &b, 500);
    CHECK(b.msgs == 1 && memcmp(b.last, "next", 4) == 0, "and stream 1 goes on in order");
    e_free(&a);
    e_free(&b);
}

/* A link with a delay: what an end sends reaches the other `delay` ms later. */
typedef struct {
    pk_t pk;
    uint64_t due;
    int to;
} dl_t;
static dl_t dl[4096];
static int dl_n;

static void run_delayed(E *e[2], uint64_t ms, uint32_t delay)
{
    uint64_t end = now + ms;
    while (now < end) {
        for (int i = 0; i < 2; i++) {
            for (int k = 0; k < e[i]->nout && dl_n < 4096; k++) {
                dl[dl_n].pk = e[i]->out[k];
                dl[dl_n].due = now + delay;
                dl[dl_n++].to = 1 - i;
            }
            e[i]->nout = 0;
        }
        for (int k = 0; k < dl_n;) {
            if (dl[k].due <= now) {
                dl_t d = dl[k];
                memmove(&dl[k], &dl[k + 1], sizeof(dl_t) * (size_t)(dl_n - k - 1));
                dl_n--;
                tsctp_input(e[d.to]->s, d.pk.d, d.pk.n, now);
            } else {
                k++;
            }
        }
        tsctp_poll(e[0]->s, now);
        tsctp_poll(e[1]->s, now);
        now++;
    }
}

static void tweak_slow_rto(tsctp_config_t *c)
{
    c->rto_initial_ms = 3000;
    c->rto_min_ms = 3000; /* no timer interferes while the test steers the loss by hand */
    c->rto_max_ms = 8000;
}

/* A forced fast retransmit that cannot share a packet with a pending SACK, with the window full: it waits. */
static void test_fast_retransmit_squeeze(void)
{
    E a, b;
    e_init(&a, 100, 200, tweak_slow_rto);
    e_init(&b, 200, 100, tweak_slow_rto);
    E *pair[2] = {&a, &b};
    dl_n = 0;
    connect_pair(&a, &b);
    static uint8_t blob[1168 * 40];
    memset(blob, 3, sizeof blob);
    for (int i = 0; i < 3000 && stats(&a).cwnd < 24000; i++) { /* slow start up to a big window, with a 10 ms trip */
        if (stats(&a).queued_bytes < 30000)
            tsctp_send(a.s, blob, 1168 * 20, NULL, now);
        run_delayed(pair, 5, 10);
    }
    tsctp_stats_t st = stats(&a);
    CHECK(st.cwnd >= 24000, "window grown to %u", st.cwnd);
    /* drain what is queued so that the burst is the only thing in flight */
    run_delayed(pair, 300, 10);
    int msgs0 = b.msgs;
    CHECK(stats(&a).flight == 0 && stats(&a).queued_bytes == 0, "drained");
    e_clear(&a);
    /* a burst; its first packet is lost */
    tsctp_send(a.s, blob, 1168 * 20, NULL, now);
    int n = a.nout;
    CHECK(n >= 18, "a burst of %d packets", n);
    memmove(&a.out[0], &a.out[1], sizeof(pk_t) * (size_t)(a.nout - 1));
    a.nout--;
    /* b has data for a, so a owes a SACK when the retransmission comes */
    tsctp_send(b.s, "chatter", 7, NULL, now);
    deliver(&b, &a);
    CHECK(a.msgs >= 1, "a took b's data");
    run_delayed(pair, 3000, 10);
    CHECK(b.msgs > msgs0 && b.last_total == 1168 * 20, "and the message arrives whole after repair (%d, %zu)", b.msgs, b.last_total);
    st = stats(&a);
    CHECK(st.retransmits_fast >= 1, "by fast retransmit (%u)", st.retransmits_fast);
    e_free(&a);
    e_free(&b);
}

/* ── ERROR handling while connecting ──────────────────────────────────────── */

static void send_error_causes(E *e, uint32_t vtag, int ncauses, const uint16_t *causes)
{
    uint8_t body[64];
    size_t n = 0;
    for (int i = 0; i < ncauses; i++) {
        put16(body + n, causes[i]);
        put16(body + n + 2, 8);
        put32(body + n + 4, 0);
        n += 8;
    }
    send_chunk(e, 200, 100, vtag, T_ERROR, 0, body, n);
}

static void tweak_init2(tsctp_config_t *c) { c->max_init_retrans = 2; }

static void test_error_chunks(void)
{
    E x;
    e_init(&x, 100, 200, NULL);
    tsctp_connect(x.s, now);
    uint32_t xt = stats(&x).local_vtag;
    send_init_ack(&x, 200, 100, xt, 0x2222, 4, 4, 8, true, 0);
    CHECK(tsctp_state(x.s) == TSCTP_COOKIE_ECHOED, "echoing");
    e_clear(&x);
    /* a cause that isn't stale cookie is skipped, the next one is looked at */
    const uint16_t causes[2] = {1, 3};
    send_error_causes(&x, xt, 2, causes);
    CHECK(tsctp_state(x.s) == TSCTP_COOKIE_WAIT && out_has(&x, T_INIT), "stale cookie after another cause: new INIT");
    e_free(&x);

    /* a cause with a broken length ends the list */
    e_init(&x, 100, 200, NULL);
    tsctp_connect(x.s, now);
    xt = stats(&x).local_vtag;
    send_init_ack(&x, 200, 100, xt, 0x2222, 4, 4, 8, true, 0);
    uint8_t body[16] = {0, 3, 0, 2}; /* length 2 */
    send_chunk(&x, 200, 100, xt, T_ERROR, 0, body, 8);
    CHECK(tsctp_state(x.s) == TSCTP_COOKIE_ECHOED, "length 2");
    put16(body + 2, 40); /* longer than the chunk */
    send_chunk(&x, 200, 100, xt, T_ERROR, 0, body, 8);
    CHECK(tsctp_state(x.s) == TSCTP_COOKIE_ECHOED, "length past the chunk");
    send_chunk(&x, 200, 100, xt, T_ERROR, 0, "\0\1", 2); /* less than a cause header */
    CHECK(tsctp_state(x.s) == TSCTP_COOKIE_ECHOED, "two bytes");
    const uint16_t other[1] = {6};
    send_error_causes(&x, xt, 1, other);
    CHECK(tsctp_state(x.s) == TSCTP_COOKIE_ECHOED, "other causes do nothing");
    e_free(&x);

    /* the same error once the INIT budget is spent closes the association */
    e_init(&x, 100, 200, tweak_init2);
    tsctp_connect(x.s, now);
    xt = stats(&x).local_vtag;
    send_init_ack(&x, 200, 100, xt, 0x2222, 4, 4, 8, true, 0);
    tick(&x, 1500); /* T1 repeats the COOKIE ECHO twice */
    CHECK(tsctp_state(x.s) == TSCTP_COOKIE_ECHOED, "still echoing after two retries");
    const uint16_t stale[1] = {3};
    send_error_causes(&x, xt, 1, stale);
    CHECK(tsctp_state(x.s) == TSCTP_CLOSED && x.downs == 1 && x.reason == TSCTP_R_INIT_TIMEOUT,
          "stale cookie with no tries left: INIT timeout (%s)", tsctp_reason_name(x.reason));
    CHECK(stats(&x).failures == 1, "counted as a failure");
    e_free(&x);

    /* an ERROR in any other state is ignored */
    E a, b;
    e_init(&a, 100, 200, NULL);
    e_init(&b, 200, 100, NULL);
    connect_pair(&a, &b);
    xt = stats(&a).local_vtag;
    send_error_causes(&a, xt, 1, stale);
    CHECK(tsctp_state(a.s) == TSCTP_ESTABLISHED, "stale cookie error when established");
    e_free(&a);
    e_free(&b);
}

/* ── shutdown races and timers ────────────────────────────────────────────── */

static void test_shutdown_races(void)
{
    E a, b;
    /* both ends shut down at once: each gets the other's SHUTDOWN while waiting for the ACK */
    e_init(&a, 100, 200, NULL);
    e_init(&b, 200, 100, NULL);
    connect_pair(&a, &b);
    tsctp_shutdown(a.s, now);
    tsctp_shutdown(b.s, now);
    CHECK(tsctp_state(a.s) == TSCTP_SHUTDOWN_SENT && tsctp_state(b.s) == TSCTP_SHUTDOWN_SENT, "both sent SHUTDOWN");
    pk_t sa = a.out[0], sb = b.out[0];
    e_clear(&a);
    e_clear(&b);
    tsctp_input(a.s, sb.d, sb.n, now);
    tsctp_input(b.s, sa.d, sa.n, now);
    CHECK(tsctp_state(a.s) == TSCTP_SHUTDOWN_ACK_SENT && tsctp_state(b.s) == TSCTP_SHUTDOWN_ACK_SENT,
          "each answers with SHUTDOWN ACK");
    CHECK(out_has(&a, T_SHUTDOWN_ACK) && out_has(&b, T_SHUTDOWN_ACK), "which are sent");
    run(&a, &b, 100);
    CHECK(a.downs == 1 && b.downs == 1 && a.reason == TSCTP_R_SHUTDOWN && b.reason == TSCTP_R_SHUTDOWN, "both end");
    e_free(&a);
    e_free(&b);

    /* data arriving while we wait for the SHUTDOWN ACK is taken and answered, and SHUTDOWN is sent again */
    e_init(&a, 100, 200, NULL);
    e_init(&b, 200, 100, NULL);
    connect_pair(&a, &b);
    tsctp_shutdown(a.s, now); /* a is in SHUTDOWN-SENT, its SHUTDOWN not yet delivered */
    e_clear(&a);
    tsctp_send(b.s, "late news", 9, NULL, now);
    CHECK(b.nout == 1, "b sent data");
    deliver(&b, &a);
    CHECK(a.msgs == 1 && memcmp(a.last, "late news", 9) == 0, "a takes it while in SHUTDOWN-SENT");
    CHECK(out_has(&a, T_SACK) && out_has(&a, T_SHUTDOWN), "answers with a SACK and SHUTDOWN again");
    run(&a, &b, 200);
    CHECK(tsctp_state(a.s) == TSCTP_CLOSED && a.downs == 1, "and the close completes");
    e_free(&a);
    e_free(&b);

    /* SHUTDOWN sent into silence is repeated with backoff until the peer is declared lost */
    e_init(&a, 100, 200, NULL);
    e_init(&b, 200, 100, NULL);
    connect_pair(&a, &b);
    tsctp_shutdown(a.s, now);
    e_clear(&a);
    int shutdowns = 0;
    for (int i = 0; i < 40000 && a.downs == 0; i++) {
        tick(&a, 1);
        shutdowns += count_chunks(&a, T_SHUTDOWN);
        e_clear(&a);
    }
    CHECK(shutdowns >= 5 && a.downs == 1 && a.reason == TSCTP_R_PEER_LOST, "SHUTDOWN repeated %d times, then peer lost", shutdowns);
    e_free(&a);
    e_free(&b);

    /* the same for SHUTDOWN ACK */
    e_init(&a, 100, 200, NULL);
    e_init(&b, 200, 100, NULL);
    connect_pair(&a, &b);
    tsctp_shutdown(a.s, now);
    deliver(&a, &b); /* b -> SHUTDOWN-ACK-SENT; its ACK is lost */
    CHECK(tsctp_state(b.s) == TSCTP_SHUTDOWN_ACK_SENT, "b waits for COMPLETE");
    e_clear(&b);
    int acks = 0;
    for (int i = 0; i < 40000 && b.downs == 0; i++) {
        tick(&b, 1);
        acks += count_chunks(&b, T_SHUTDOWN_ACK);
        e_clear(&b);
    }
    CHECK(acks >= 5 && b.downs == 1 && b.reason == TSCTP_R_PEER_LOST, "SHUTDOWN ACK repeated %d times, then peer lost", acks);
    e_free(&a);
    e_free(&b);
}

int main(void)
{
    test_config();
    test_alloc_failures();
    test_no_leaks();
    test_api_errors();
    test_init_rejects();
    test_init_params();
    test_init_ack();
    test_cookie_echo_rejects();
    test_cookie_collisions();
    test_data_refused();
    test_data_alloc_fail();
    test_control_overflow();
    test_rto_limits();
    test_input_filters();
    test_init_in_states();
    test_rx_edges();
    test_send_edges();
    test_fast_retransmit_squeeze();
    test_error_chunks();
    test_shutdown_races();
    printf("tinysctp edges: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
