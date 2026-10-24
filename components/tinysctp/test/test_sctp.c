// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * tinysctp tests: CRC32c against known vectors, the packet formats, and two
 * instances over a simulated link that drops, duplicates, reorders, delays
 * and goes out, checking delivery, timeouts, PR-SCTP and the association's
 * life (shutdown, abort, restart, stale cookies, collisions).
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

/* ── a deterministic generator ────────────────────────────────────────────── */

static uint64_t rng_state = 88172645463325252ULL;
static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 16);
}
static uint32_t rng_cb(void *ctx) { (void)ctx; return rng(); }
static bool chance(double p) { return (rng() % 1000000) < p * 1000000; }

/* ── CRC32c ───────────────────────────────────────────────────────────────── */

static void test_crc32c(void)
{
    CHECK(tsctp_crc32c(0, "123456789", 9) == 0xE3069283, "check value");
    uint8_t b[32];
    memset(b, 0, 32); /* RFC 3720 B.4 */
    CHECK(tsctp_crc32c(0, b, 32) == 0x8A9136AA, "32 zeros");
    memset(b, 0xFF, 32);
    CHECK(tsctp_crc32c(0, b, 32) == 0x62A8AB43, "32 ones");
    for (int i = 0; i < 32; i++)
        b[i] = (uint8_t)i;
    CHECK(tsctp_crc32c(0, b, 32) == 0x46DD794E, "incrementing");
    for (int i = 0; i < 32; i++)
        b[i] = (uint8_t)(31 - i);
    CHECK(tsctp_crc32c(0, b, 32) == 0x113FDB5C, "decrementing");
    /* RFC 3720 B.4's iSCSI read command PDU */
    static const uint8_t pdu[48] = {0x01, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                    0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
                                    0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x18, 0x28, 0x00, 0x00, 0x00,
                                    0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    CHECK(tsctp_crc32c(0, pdu, 48) == 0xD9963A56, "iSCSI PDU");
    CHECK(tsctp_crc32c(tsctp_crc32c(0, "1234", 4), "56789", 5) == 0xE3069283, "chained");
}

/* ── a simulated link ─────────────────────────────────────────────────────── */

typedef struct pkt {
    struct pkt *next;
    uint64_t due;
    int to;
    size_t len;
    uint8_t data[];
} pkt_t;

typedef struct {
    double drop, dup, reorder; /* probabilities */
    uint32_t delay_ms, jitter_ms;
    uint64_t out_from, out_to; /* an outage: everything dropped (both ways) */
    bool block[2];             /* drop everything to this end */
} link_t;

typedef struct peer peer_t;
typedef struct {
    pkt_t *q;
    uint64_t now;
    link_t link;
    peer_t *p[2];
    /* hooks: capture or rewrite packets */
    void (*tap)(int from, uint8_t *data, size_t *len, bool *drop);
} sim_t;

#define MAXMSG 8000
struct peer {
    sim_t *sim;
    int id;
    tsctp_t *s;
    tsctp_config_t cfg;
    /* what arrived */
    uint32_t got, dupes, out_of_order, bad;
    uint32_t next_seq[4];
    uint64_t max_latency;
    int ups, downs, restarts;
    tsctp_reason_t last_reason, first_reason;
    bool echo;     /* send every message back */
    uint32_t big_ok;
    uint8_t seen[MAXMSG];
};

static sim_t S;

static void link_send(sim_t *sim, int to, const uint8_t *data, size_t len)
{
    link_t *l = &sim->link;
    if (sim->now >= l->out_from && sim->now < l->out_to)
        return;
    if (l->block[to] || chance(l->drop))
        return;
    int copies = chance(l->dup) ? 2 : 1;
    for (int i = 0; i < copies; i++) {
        pkt_t *p = malloc(sizeof *p + len);
        p->due = sim->now + l->delay_ms + (l->jitter_ms ? rng() % (l->jitter_ms + 1) : 0);
        if (chance(l->reorder))
            p->due += l->delay_ms + 20;
        p->to = to;
        p->len = len;
        memcpy(p->data, data, len);
        pkt_t **pp = &sim->q; /* sorted by due time */
        while (*pp && (*pp)->due <= p->due)
            pp = &(*pp)->next;
        p->next = *pp;
        *pp = p;
    }
}

static void out_cb(void *ctx, const uint8_t *pkt, size_t len)
{
    peer_t *me = ctx;
    uint8_t buf[2048];
    memcpy(buf, pkt, len);
    bool drop = false;
    if (me->sim->tap)
        me->sim->tap(me->id, buf, &len, &drop);
    if (!drop)
        link_send(me->sim, 1 - me->id, buf, len);
}

static void msg_cb(void *user, const tsctp_msg_t *m)
{
    peer_t *me = user;
    if (m->len >= 1000) { /* a big one: a pattern from its first byte */
        bool ok = true;
        for (size_t i = 0; i < m->len; i++)
            if (m->data[i] != (uint8_t)(m->data[0] + i))
                ok = false;
        me->big_ok += ok;
        if (!ok)
            me->bad++;
        me->got++;
        return;
    }
    if (m->len < 12) {
        me->bad++;
        return;
    }
    uint32_t seq = get32(m->data);
    uint64_t sent = (uint64_t)get32(m->data + 4) << 32 | get32(m->data + 8);
    if (seq >= MAXMSG) {
        me->bad++;
        return;
    }
    if (me->seen[seq])
        me->dupes++;
    me->seen[seq] = 1;
    if (!m->unordered && m->stream < 4) {
        if (seq < me->next_seq[m->stream])
            me->out_of_order++;
        me->next_seq[m->stream] = seq + 1;
    }
    uint64_t lat = me->sim->now - sent;
    if (lat > me->max_latency)
        me->max_latency = lat;
    me->got++;
    if (me->echo) {
        tsctp_sendinfo_t si = {m->stream, m->ppid, m->unordered, 0};
        tsctp_send(me->s, m->data, m->len, &si, me->sim->now);
    }
}

static void ev_cb(void *user, tsctp_event_t ev, tsctp_reason_t why)
{
    peer_t *me = user;
    if (ev == TSCTP_EV_UP)
        me->ups++;
    else if (ev == TSCTP_EV_DOWN) {
        me->downs++;
        me->last_reason = why;
        if (me->downs == 1)
            me->first_reason = why;
    } else if (ev == TSCTP_EV_RESTART)
        me->restarts++;
}

static void peer_init(peer_t *p, sim_t *sim, int id, void (*tweak)(tsctp_config_t *))
{
    memset(p, 0, sizeof *p);
    p->sim = sim;
    p->id = id;
    tsctp_config_defaults(&p->cfg);
    p->cfg.local_port = id ? 9000 : 5000;
    p->cfg.peer_port = id ? 5000 : 9000;
    p->cfg.rto_initial_ms = 300;
    p->cfg.rto_min_ms = 100;
    p->cfg.rto_max_ms = 2000;
    p->cfg.hb_interval_ms = 2000;
    p->cfg.sack_delay_ms = 50;
    p->cfg.output = out_cb;
    p->cfg.output_ctx = p;
    p->cfg.random = rng_cb;
    p->cfg.on_message = msg_cb;
    p->cfg.on_event = ev_cb;
    p->cfg.user = p;
    if (tweak)
        tweak(&p->cfg);
    p->s = tsctp_new(&p->cfg, sim->now);
    sim->p[id] = p;
}

static void sim_reset(void)
{
    while (S.q) {
        pkt_t *n = S.q->next;
        free(S.q);
        S.q = n;
    }
    memset(&S, 0, sizeof S);
    S.now = 1000;
    S.link.delay_ms = 10;
}

/* Run the clock `ms` forward, a millisecond at a time. */
static void sim_run(uint64_t ms)
{
    uint64_t end = S.now + ms;
    while (S.now < end) {
        while (S.q && S.q->due <= S.now) {
            pkt_t *p = S.q;
            S.q = p->next;
            peer_t *dst = S.p[p->to];
            if (dst && dst->s)
                tsctp_input(dst->s, p->data, p->len, S.now);
            free(p);
        }
        for (int i = 0; i < 2; i++)
            if (S.p[i] && S.p[i]->s)
                tsctp_poll(S.p[i]->s, S.now);
        S.now++;
    }
}

static void sim_until_up(peer_t *a, peer_t *b, uint64_t max_ms)
{
    uint64_t end = S.now + max_ms;
    while (S.now < end && !(tsctp_state(a->s) == TSCTP_ESTABLISHED && tsctp_state(b->s) == TSCTP_ESTABLISHED))
        sim_run(1);
}

static int try_send_seq(peer_t *p, uint32_t seq, uint16_t stream, bool unordered, uint32_t lifetime, size_t len)
{
    uint8_t m[64];
    memset(m, 0xA5, sizeof m);
    put32(m, seq);
    put32(m + 4, (uint32_t)(S.now >> 32));
    put32(m + 8, (uint32_t)S.now);
    tsctp_sendinfo_t si = {stream, 51, unordered, lifetime};
    return tsctp_send(p->s, m, len < 12 ? 12 : len, &si, S.now);
}

static void send_seq(peer_t *p, uint32_t seq, uint16_t stream, bool unordered, uint32_t lifetime, size_t len)
{
    int r = try_send_seq(p, seq, stream, unordered, lifetime, len);
    CHECK(r == TSCTP_OK, "send %u: %d", seq, r);
}

static void free_peers(peer_t *a, peer_t *b)
{
    tsctp_free(a->s);
    tsctp_free(b->s);
    a->s = b->s = NULL;
    sim_reset();
}

/* ── packet formats ───────────────────────────────────────────────────────── */

static uint8_t cap[16][1600];
static size_t cap_len[16];
static int ncap;
static void cap_tap(int from, uint8_t *d, size_t *len, bool *drop)
{
    (void)from;
    (void)drop;
    if (ncap < 16) {
        memcpy(cap[ncap], d, *len);
        cap_len[ncap++] = *len;
    }
}

static bool crc_ok(const uint8_t *p, size_t n)
{
    uint8_t b[1600];
    memcpy(b, p, n);
    uint32_t got = b[8] | b[9] << 8 | b[10] << 16 | (uint32_t)b[11] << 24;
    memset(b + 8, 0, 4);
    return tsctp_crc32c(0, b, n) == got;
}

static void test_formats(void)
{
    sim_reset();
    S.tap = cap_tap;
    ncap = 0;
    peer_t a, b;
    peer_init(&a, &S, 0, NULL);
    peer_init(&b, &S, 1, NULL);
    tsctp_listen(b.s);
    tsctp_connect(a.s, S.now);
    CHECK(ncap == 1, "INIT out at once");
    const uint8_t *p = cap[0];
    CHECK(cap_len[0] == 12 + 20 + 12, "INIT length %zu", cap_len[0]);
    CHECK(get16(p) == 5000 && get16(p + 2) == 9000, "ports");
    CHECK(get32(p + 4) == 0, "INIT has verification tag 0");
    CHECK(crc_ok(p, cap_len[0]), "INIT checksum");
    CHECK(p[12] == 1 && p[13] == 0 && get16(p + 14) == 32, "INIT chunk header");
    CHECK(get32(p + 16) != 0, "initiate tag");
    CHECK(get32(p + 20) == 65536, "a_rwnd");
    CHECK(get16(p + 24) == 4 && get16(p + 26) == 4, "streams");
    CHECK(get16(p + 32) == 0xC000 && get16(p + 34) == 4, "Forward-TSN-Supported");
    CHECK(get16(p + 36) == 0x8008 && get16(p + 38) == 5 && p[40] == 0xC0, "Supported Extensions: FORWARD TSN");
    sim_run(15);
    CHECK(ncap >= 2, "INIT ACK");
    p = cap[1];
    CHECK(get16(p) == 9000 && get16(p + 2) == 5000, "INIT ACK ports");
    CHECK(get32(p + 4) == get32(cap[0] + 16), "INIT ACK tag is the INIT's");
    CHECK(p[12] == 2 && crc_ok(p, cap_len[1]), "INIT ACK chunk");
    CHECK(get16(p + 32) == 7 && get16(p + 34) == 64, "state cookie parameter");
    sim_run(15);
    CHECK(ncap >= 3 && cap[2][12] == 10 && get16(cap[2] + 14) == 64, "COOKIE ECHO echoes the cookie");
    CHECK(get32(cap[2] + 4) == get32(cap[1] + 16), "COOKIE ECHO tag is the peer's");
    CHECK(memcmp(cap[2] + 16, cap[1] + 36, 60) == 0, "cookie bytes");
    sim_run(15);
    CHECK(ncap >= 4 && cap[3][12] == 11 && get16(cap[3] + 14) == 4, "COOKIE ACK");
    CHECK(tsctp_state(a.s) == TSCTP_ESTABLISHED && tsctp_state(b.s) == TSCTP_ESTABLISHED, "established");
    CHECK(a.ups == 1 && b.ups == 1, "up events");

    ncap = 0;
    uint8_t msg[5] = {1, 2, 3, 4, 5};
    tsctp_sendinfo_t si = {2, 0x01020304, false, 0};
    tsctp_send(a.s, msg, 5, &si, S.now);
    CHECK(ncap == 1, "DATA out at once");
    p = cap[0];
    CHECK(p[12] == 0 && p[13] == 0x03 && get16(p + 14) == 21, "DATA chunk: B E, length 21");
    CHECK(cap_len[0] == 12 + 24, "DATA padded");
    CHECK(get16(p + 20) == 2 && get16(p + 22) == 0 && get32(p + 24) == 0x01020304, "stream, SSN, PPID");
    CHECK(memcmp(p + 28, msg, 5) == 0 && p[33] == 0 && p[34] == 0 && p[35] == 0, "payload and padding");
    uint32_t tsn = get32(p + 16);
    sim_run(100); /* delayed SACK */
    CHECK(ncap == 2 && cap[1][12] == 3, "SACK");
    CHECK(get32(cap[1] + 16) == tsn && get16(cap[1] + 24) == 0, "SACK cum TSN, no gaps");
    CHECK(b.got == 0 && b.bad == 1, "5-byte message delivered (too short for the test's format)");

    /* unordered, fragmented */
    ncap = 0;
    uint8_t big[2500];
    for (int i = 0; i < 2500; i++)
        big[i] = (uint8_t)(7 + i);
    si.unordered = true;
    tsctp_send(a.s, big, sizeof big, &si, S.now);
    CHECK(ncap == 3, "three fragments (%d)", ncap);
    CHECK(cap[0][13] == 0x06 && cap[1][13] == 0x04 && cap[2][13] == 0x05, "U+B, U, U+E");
    CHECK(get32(cap[1] + 16) == get32(cap[0] + 16) + 1, "consecutive TSNs");
    sim_run(200);
    CHECK(b.big_ok == 1, "reassembled");

    /* heartbeat */
    ncap = 0;
    sim_run(4000);
    bool hb = false, hback = false;
    for (int i = 0; i < ncap; i++) {
        if (cap[i][12] == 4) {
            hb = get16(cap[i] + 14) == 24 && get16(cap[i] + 16) == 1;
        }
        if (cap[i][12] == 5)
            hback = true;
    }
    CHECK(hb && hback, "HEARTBEAT with info, and its ACK");
    tsctp_stats_t st;
    tsctp_get_stats(a.s, &st);
    CHECK(st.hb_acked >= 1 && st.srtt_ms >= 15 && st.srtt_ms <= 80, "RTT measured (%u: 20 ms, plus the delayed SACK on data)", st.srtt_ms);

    /* stream out of range, empty message, not established */
    CHECK(tsctp_send(a.s, msg, 5, &(tsctp_sendinfo_t){4, 0, false, 0}, S.now) == TSCTP_ERR_INVAL, "stream 4");
    CHECK(tsctp_send(a.s, msg, 0, NULL, S.now) == TSCTP_ERR_INVAL, "empty");

    /* a corrupt packet is dropped */
    uint8_t bad[48];
    memcpy(bad, cap[0], 48);
    bad[30] ^= 1;
    tsctp_get_stats(b.s, &st);
    uint32_t badck = st.bad_checksum;
    tsctp_input(b.s, bad, 48, S.now);
    tsctp_get_stats(b.s, &st);
    CHECK(st.bad_checksum == badck + 1, "bad checksum counted");

    char json[1024];
    size_t n = tsctp_status_json(a.s, json, sizeof json);
    CHECK(n > 100 && json[0] == '{' && json[n - 1] == '}' && strstr(json, "\"state\":\"established\""), "status JSON");
    free_peers(&a, &b);
}

/* Unknown chunk types: 0x80 skipped, 0xC0 skipped and reported, 0x40 stops the packet and is reported. */
static void test_unknown_chunks(void)
{
    sim_reset();
    peer_t a, b;
    peer_init(&a, &S, 0, NULL);
    peer_init(&b, &S, 1, NULL);
    tsctp_listen(b.s);
    tsctp_connect(a.s, S.now);
    sim_until_up(&a, &b, 1000);
    S.tap = cap_tap;
    ncap = 0;
    send_seq(&a, 0, 0, false, 0, 20);
    S.tap = NULL;
    sim_run(200);
    CHECK(b.got == 1, "first");
    /* the same DATA with TSN + 1, after a chunk of type 0x85 (skip) */
    for (int variant = 0; variant < 2; variant++) {
        uint8_t p[128];
        size_t dlen = cap_len[0] - 12;
        memcpy(p, cap[0], 12);
        p[12] = variant ? 0x45 : 0x85;
        p[13] = 0;
        put16(p + 14, 8);
        memset(p + 16, 0, 4);
        memcpy(p + 20, cap[0] + 12, dlen);
        put32(p + 20 + 4, get32(cap[0] + 16) + 1 + variant);
        put32(p + 20 + 4 + 4, 0);
        put16(p + 20 + 12, 0);
        put16(p + 20 + 10, (uint16_t)(1 + variant));
        p[20 + 16 + 3] = (uint8_t)(1 + variant); /* seq */
        memset(p + 8, 0, 4);
        uint32_t crc = tsctp_crc32c(0, p, 20 + dlen);
        p[8] = (uint8_t)crc;
        p[9] = (uint8_t)(crc >> 8);
        p[10] = (uint8_t)(crc >> 16);
        p[11] = (uint8_t)(crc >> 24);
        tsctp_input(b.s, p, 20 + dlen, S.now);
        sim_run(100);
    }
    CHECK(b.got == 2, "DATA after a skipped chunk delivered, DATA after a stopping one not (%u)", b.got);
    CHECK(b.bad == 0, "no bad");
    free_peers(&a, &b);
}

/* ── delivery over a bad link ─────────────────────────────────────────────── */

static void pr_tweak(tsctp_config_t *c);

static void lossy_run(const char *name, double drop, double dup, double reorder, uint32_t jitter, bool unordered,
                      int count, int every_ms)
{
    sim_reset();
    S.link.drop = drop;
    S.link.dup = dup;
    S.link.reorder = reorder;
    S.link.jitter_ms = jitter;
    peer_t a, b;
    peer_init(&a, &S, 0, pr_tweak);
    peer_init(&b, &S, 1, pr_tweak);
    tsctp_listen(b.s);
    tsctp_connect(a.s, S.now);
    sim_until_up(&a, &b, 20000);
    CHECK(tsctp_state(a.s) == TSCTP_ESTABLISHED, "%s: up", name);
    for (int i = 0; i < count; i++) {
        int r;
        while ((r = try_send_seq(&a, (uint32_t)i, (uint16_t)(i % 3), unordered, 0, 55)) == TSCTP_ERR_NOBUFS)
            sim_run(1); /* the send buffer is full: wait as an application would */
        CHECK(r == TSCTP_OK, "%s: send %d: %d", name, i, r);
        sim_run((uint64_t)every_ms);
    }
    sim_run(20000);
    CHECK(b.got == (uint32_t)count, "%s: all %d delivered (%u)", name, count, b.got);
    CHECK(b.dupes == 0 && b.bad == 0, "%s: once each (%u dupes, %u bad)", name, b.dupes, b.bad);
    if (!unordered)
        CHECK(b.out_of_order == 0, "%s: in order per stream (%u)", name, b.out_of_order);
    tsctp_stats_t st;
    tsctp_get_stats(a.s, &st);
    CHECK(st.outstanding_chunks == 0 && st.queued_msgs == 0, "%s: nothing left", name);
    printf("  %s: %d msgs, srtt %u ms, rto %u, cwnd %u, %u T3 / %u fast retransmits, %u dup chunks at the "
           "receiver, max latency %llu ms\n",
           name, count, st.srtt_ms, st.rto_ms, st.cwnd, st.retransmits_t3, st.retransmits_fast,
           (tsctp_get_stats(b.s, &st), st.dup_chunks), (unsigned long long)b.max_latency);
    free_peers(&a, &b);
}

static void test_big_lossy(void)
{
    sim_reset();
    S.link.drop = 0.1;
    S.link.reorder = 0.05;
    S.link.jitter_ms = 5;
    peer_t a, b;
    peer_init(&a, &S, 0, NULL);
    peer_init(&b, &S, 1, NULL);
    tsctp_listen(b.s);
    tsctp_connect(a.s, S.now);
    sim_until_up(&a, &b, 20000);
    uint8_t big[6000];
    for (int k = 0; k < 20; k++) {
        for (int i = 0; i < 6000; i++)
            big[i] = (uint8_t)(k + i);
        CHECK(tsctp_send(a.s, big, sizeof big, NULL, S.now) == TSCTP_OK, "big send");
        sim_run(50);
    }
    sim_run(30000);
    CHECK(b.big_ok == 20 && b.bad == 0, "20 6000-byte messages over 10%% loss (%u ok, %u bad)", b.big_ok, b.bad);
    free_peers(&a, &b);
}

/* Echo both ways over a lossy link: the server sends every message back. */
static void test_echo(void)
{
    sim_reset();
    S.link.drop = 0.05;
    S.link.jitter_ms = 3;
    peer_t a, b;
    peer_init(&a, &S, 0, NULL);
    peer_init(&b, &S, 1, NULL);
    b.echo = true;
    tsctp_listen(b.s);
    tsctp_connect(a.s, S.now);
    sim_until_up(&a, &b, 20000);
    for (int i = 0; i < 500; i++) {
        send_seq(&a, (uint32_t)i, 0, false, 0, 55);
        sim_run(30);
    }
    sim_run(20000);
    CHECK(a.got == 500 && b.got == 500 && a.out_of_order == 0 && a.dupes == 0, "echoed (%u, %u)", b.got, a.got);
    free_peers(&a, &b);
}

/* ── PR-SCTP ──────────────────────────────────────────────────────────────── */

static void pr_tweak(tsctp_config_t *c)
{
    c->max_retrans = 30;
}

static void test_prsctp(void)
{
    sim_reset();
    S.link.drop = 0.3;
    S.link.jitter_ms = 4;
    peer_t a, b;
    peer_init(&a, &S, 0, pr_tweak);
    peer_init(&b, &S, 1, pr_tweak);
    tsctp_listen(b.s);
    tsctp_connect(a.s, S.now);
    sim_until_up(&a, &b, 30000);
    CHECK(tsctp_state(a.s) == TSCTP_ESTABLISHED, "PR: up");
    tsctp_stats_t st;
    tsctp_get_stats(a.s, &st);
    CHECK(st.peer_prsctp, "PR-SCTP negotiated");
    int n = 1500;
    for (int i = 0; i < n; i++) {
        send_seq(&a, (uint32_t)i, 0, false, 250, 55);
        sim_run(20);
    }
    sim_run(5000);
    tsctp_get_stats(a.s, &st);
    tsctp_stats_t sb;
    tsctp_get_stats(b.s, &sb);
    printf("  PR-SCTP at 30%% loss, 250 ms lifetime: %u of %d delivered, %u abandoned, %u dropped unsent, "
           "%u FORWARD TSN sent, %u received, max latency %llu ms\n",
           b.got, n, st.abandoned_msgs, st.abandoned_unsent, st.fwd_tsn_sent, sb.fwd_tsn_received,
           (unsigned long long)b.max_latency);
    CHECK(b.got > (uint32_t)n / 2 && b.got < (uint32_t)n, "PR: most delivered, some given up (%u)", b.got);
    CHECK(st.abandoned_msgs + st.abandoned_unsent > 0 && st.fwd_tsn_sent > 0 && sb.fwd_tsn_received > 0,
          "PR: abandoned and forwarded");
    CHECK(b.out_of_order == 0 && b.dupes == 0 && b.bad == 0, "PR: still ordered, once each");
    CHECK(b.max_latency < 250 + 400, "PR: nothing arrives long after its lifetime (%llu ms)",
          (unsigned long long)b.max_latency);
    CHECK(st.outstanding_chunks == 0 && st.queued_msgs == 0, "PR: nothing stuck");
    CHECK(tsctp_state(a.s) == TSCTP_ESTABLISHED, "PR: still up");
    /* Reliable messages after it all still arrive. */
    uint32_t before = b.got;
    S.link.drop = 0.1;
    for (int i = 0; i < 100; i++) {
        send_seq(&a, (uint32_t)(n + i), 0, false, 0, 55);
        sim_run(20);
    }
    sim_run(20000);
    CHECK(b.got == before + 100, "PR: then 100 reliable ones (%u)", b.got - before);
    free_peers(&a, &b);
}

/* Unsent messages expire too, even when the peer has no PR-SCTP; sent ones then stay reliable. */
static void test_expire_unsent(void)
{
    sim_reset();
    peer_t a, b;
    peer_init(&a, &S, 0, NULL);
    peer_init(&b, &S, 1, NULL);
    tsctp_listen(b.s);
    tsctp_connect(a.s, S.now);
    sim_until_up(&a, &b, 1000);
    S.link.block[1] = true; /* nothing reaches b: the window fills, the rest waits */
    for (int i = 0; i < 300; i++)
        send_seq(&a, (uint32_t)i, 0, false, 100, 55);
    sim_run(300);
    tsctp_stats_t st;
    tsctp_get_stats(a.s, &st);
    CHECK(st.abandoned_unsent > 0, "unsent expired (%u)", st.abandoned_unsent);
    CHECK(st.abandoned_msgs > 0, "sent abandoned (%u)", st.abandoned_msgs);
    S.link.block[1] = false;
    sim_run(5000);
    tsctp_get_stats(a.s, &st);
    CHECK(st.outstanding_chunks == 0 && st.queued_msgs == 0 && tsctp_state(a.s) == TSCTP_ESTABLISHED,
          "recovered after the block");
    free_peers(&a, &b);
}

/* ── outages, failure, reconnect ──────────────────────────────────────────── */

static void reconnect_tweak(tsctp_config_t *c)
{
    c->max_retrans = 3;
    c->hb_interval_ms = 1000;
    c->reconnect_min_ms = 500;
    c->reconnect_max_ms = 4000;
}

static void test_outage(void)
{
    /* a short outage is ridden out */
    sim_reset();
    peer_t a, b;
    peer_init(&a, &S, 0, pr_tweak);
    peer_init(&b, &S, 1, pr_tweak);
    tsctp_listen(b.s);
    tsctp_connect(a.s, S.now);
    sim_until_up(&a, &b, 1000);
    S.link.out_from = S.now + 500;
    S.link.out_to = S.now + 3500;
    for (int i = 0; i < 200; i++) {
        send_seq(&a, (uint32_t)i, 0, false, 0, 55);
        sim_run(30);
    }
    sim_run(30000);
    CHECK(b.got == 200 && b.out_of_order == 0 && a.downs == 0, "3 s outage ridden out (%u, %d downs)", b.got,
          a.downs);
    free_peers(&a, &b);

    /* a long one: the peer is lost, then found again */
    sim_reset();
    peer_init(&a, &S, 0, reconnect_tweak);
    peer_init(&b, &S, 1, reconnect_tweak);
    tsctp_listen(b.s);
    tsctp_connect(a.s, S.now);
    sim_until_up(&a, &b, 1000);
    S.link.out_from = S.now;
    S.link.out_to = S.now + 60000;
    for (int i = 0; i < 10; i++) {
        send_seq(&a, (uint32_t)i, 0, false, 0, 55);
        sim_run(100);
    }
    sim_run(20000);
    CHECK(a.downs >= 1 && a.first_reason == TSCTP_R_PEER_LOST, "client: peer lost (%d, %s)", a.downs,
          tsctp_reason_name(a.first_reason));
    CHECK(a.downs < 2 || a.last_reason == TSCTP_R_INIT_TIMEOUT, "client: reconnects time out during the outage");
    CHECK(b.downs == 1 && b.first_reason == TSCTP_R_PEER_LOST, "server: peer lost by heartbeat (%d, %s)", b.downs,
          tsctp_reason_name(b.first_reason));
    CHECK(tsctp_state(b.s) == TSCTP_LISTEN, "server listens again");
    sim_run(45000);
    CHECK(tsctp_state(a.s) == TSCTP_ESTABLISHED && a.ups == 2 && b.ups == 2, "reconnected after the outage");
    send_seq(&a, 100, 0, false, 0, 55);
    sim_run(500);
    CHECK(b.seen[100], "and data flows");
    free_peers(&a, &b);
}

static void test_init_timeout(void)
{
    sim_reset();
    peer_t a, b;
    peer_init(&a, &S, 0, NULL);
    peer_init(&b, &S, 1, NULL);
    S.link.block[1] = true;
    tsctp_listen(b.s);
    tsctp_connect(a.s, S.now);
    sim_run(200000);
    CHECK(a.downs == 1 && a.last_reason == TSCTP_R_INIT_TIMEOUT && tsctp_state(a.s) == TSCTP_CLOSED,
          "INIT timeout (%s)", tsctp_reason_name(a.last_reason));
    free_peers(&a, &b);
}

/* ── closing ──────────────────────────────────────────────────────────────── */

static void test_shutdown_abort(void)
{
    sim_reset();
    S.link.drop = 0.1;
    peer_t a, b;
    peer_init(&a, &S, 0, pr_tweak);
    peer_init(&b, &S, 1, pr_tweak);
    tsctp_listen(b.s);
    tsctp_connect(a.s, S.now);
    sim_until_up(&a, &b, 20000);
    for (int i = 0; i < 50; i++)
        send_seq(&a, (uint32_t)i, 0, false, 0, 55);
    CHECK(tsctp_shutdown(a.s, S.now) == TSCTP_OK, "shutdown");
    CHECK(tsctp_send(a.s, "x", 1, NULL, S.now) == TSCTP_ERR_STATE, "no sends while shutting down");
    sim_run(30000);
    CHECK(b.got == 50, "everything delivered before the close (%u)", b.got);
    CHECK(a.downs == 1 && a.last_reason == TSCTP_R_SHUTDOWN && b.downs == 1 && b.last_reason == TSCTP_R_SHUTDOWN,
          "both closed by shutdown (%s, %s)", tsctp_reason_name(a.last_reason), tsctp_reason_name(b.last_reason));
    CHECK(tsctp_state(a.s) == TSCTP_CLOSED && tsctp_state(b.s) == TSCTP_LISTEN, "closed / listening");
    free_peers(&a, &b);

    /* shutdown from the server side */
    sim_reset();
    peer_init(&a, &S, 0, NULL);
    peer_init(&b, &S, 1, NULL);
    tsctp_listen(b.s);
    tsctp_connect(a.s, S.now);
    sim_until_up(&a, &b, 1000);
    tsctp_shutdown(b.s, S.now);
    sim_run(1000);
    CHECK(a.last_reason == TSCTP_R_SHUTDOWN && b.last_reason == TSCTP_R_SHUTDOWN, "server-side shutdown");
    free_peers(&a, &b);

    /* abort */
    sim_reset();
    peer_init(&a, &S, 0, NULL);
    peer_init(&b, &S, 1, NULL);
    tsctp_listen(b.s);
    tsctp_connect(a.s, S.now);
    sim_until_up(&a, &b, 1000);
    tsctp_abort(a.s, S.now);
    sim_run(100);
    CHECK(a.last_reason == TSCTP_R_LOCAL_ABORT && b.last_reason == TSCTP_R_ABORTED, "abort (%s)",
          tsctp_reason_name(b.last_reason));
    CHECK(tsctp_state(b.s) == TSCTP_LISTEN, "server listens again");
    free_peers(&a, &b);
}

/* ── restarts ─────────────────────────────────────────────────────────────── */

static void test_restart(void)
{
    /* the client reboots and connects again from the same port: a restart for the server */
    sim_reset();
    peer_t a, b;
    peer_init(&a, &S, 0, NULL);
    peer_init(&b, &S, 1, NULL);
    tsctp_listen(b.s);
    tsctp_connect(a.s, S.now);
    sim_until_up(&a, &b, 1000);
    send_seq(&a, 0, 0, false, 0, 55);
    sim_run(200);
    tsctp_free(a.s);
    peer_init(&a, &S, 0, NULL);
    tsctp_connect(a.s, S.now);
    sim_until_up(&a, &b, 2000);
    CHECK(b.restarts == 1 && b.downs == 0, "server saw a restart (%d restarts, %d downs)", b.restarts, b.downs);
    memset(b.next_seq, 0, sizeof b.next_seq);
    send_seq(&a, 1, 0, false, 0, 55);
    sim_run(200);
    CHECK(b.seen[1], "data after the restart");
    free_peers(&a, &b);

    /* the server restarts: the client's next packet gets an ABORT (T bit), and it reconnects */
    sim_reset();
    peer_init(&a, &S, 0, reconnect_tweak);
    peer_init(&b, &S, 1, NULL);
    tsctp_listen(b.s);
    tsctp_connect(a.s, S.now);
    sim_until_up(&a, &b, 1000);
    tsctp_free(b.s);
    peer_init(&b, &S, 1, NULL);
    tsctp_listen(b.s);
    send_seq(&a, 0, 0, false, 0, 55);
    sim_run(100);
    CHECK(a.downs == 1 && a.last_reason == TSCTP_R_ABORTED, "client: aborted by the new server (%s)",
          tsctp_reason_name(a.last_reason));
    sim_run(2000);
    CHECK(tsctp_state(a.s) == TSCTP_ESTABLISHED && a.ups == 2, "client reconnected");
    send_seq(&a, 1, 0, false, 0, 55);
    sim_run(200);
    CHECK(b.seen[1], "data to the new server");
    free_peers(&a, &b);
}

/* A COOKIE ECHO held past the cookie's life: ERROR (stale cookie), and the client starts over. */
static int hold_cookie;
static pkt_t *held_pkt;
static void stale_tap(int from, uint8_t *d, size_t *len, bool *drop)
{
    if (from == 0 && d[12] == 10 && hold_cookie) {
        hold_cookie--;
        held_pkt = malloc(sizeof *held_pkt + *len);
        held_pkt->len = *len;
        memcpy(held_pkt->data, d, *len);
        *drop = true;
    }
}

static void cookie_tweak(tsctp_config_t *c)
{
    c->cookie_life_ms = 500;
    c->rto_initial_ms = 2000;
    c->rto_min_ms = 2000;
}

static void test_stale_cookie(void)
{
    sim_reset();
    peer_t a, b;
    peer_init(&a, &S, 0, cookie_tweak);
    peer_init(&b, &S, 1, cookie_tweak);
    S.tap = stale_tap;
    hold_cookie = 1;
    tsctp_listen(b.s);
    tsctp_connect(a.s, S.now);
    sim_run(1000);
    CHECK(held_pkt != NULL, "cookie held");
    if (held_pkt) {
        tsctp_input(b.s, held_pkt->data, held_pkt->len, S.now); /* 1 s later */
        free(held_pkt);
        held_pkt = NULL;
    }
    S.tap = NULL;
    sim_until_up(&a, &b, 5000);
    CHECK(tsctp_state(a.s) == TSCTP_ESTABLISHED && a.ups == 1, "up after a stale cookie, without waiting for T1");
    CHECK(S.now < 1000 + 1000 + 1000, "quickly (%llu ms)", (unsigned long long)(S.now - 1000));
    free_peers(&a, &b);
}

/* Both ends connect at once: INIT collision (RFC 9260 5.2.1, 5.2.4 B / D). */
static void test_collision(void)
{
    sim_reset();
    peer_t a, b;
    peer_init(&a, &S, 0, NULL);
    peer_init(&b, &S, 1, NULL);
    tsctp_connect(a.s, S.now);
    tsctp_connect(b.s, S.now);
    sim_until_up(&a, &b, 5000);
    tsctp_stats_t sa, sb;
    tsctp_get_stats(a.s, &sa);
    tsctp_get_stats(b.s, &sb);
    CHECK(tsctp_state(a.s) == TSCTP_ESTABLISHED && tsctp_state(b.s) == TSCTP_ESTABLISHED, "both up");
    CHECK(sa.local_vtag == sb.peer_vtag && sb.local_vtag == sa.peer_vtag, "tags agree");
    send_seq(&a, 0, 0, false, 0, 55);
    send_seq(&b, 0, 0, false, 0, 55);
    sim_run(300);
    CHECK(a.got == 1 && b.got == 1, "data both ways");
    free_peers(&a, &b);
}

/* Out of the blue: a packet for no association gets an ABORT with the T bit; wrong tags are dropped. */
static void test_ootb(void)
{
    sim_reset();
    S.tap = cap_tap;
    ncap = 0;
    peer_t a, b;
    peer_init(&a, &S, 0, NULL);
    peer_init(&b, &S, 1, NULL);
    tsctp_listen(b.s);
    tsctp_connect(a.s, S.now);
    sim_until_up(&a, &b, 1000);
    ncap = 0;
    send_seq(&a, 0, 0, false, 0, 55);
    uint8_t d[128];
    size_t n = cap_len[0];
    memcpy(d, cap[0], n);
    S.tap = NULL;
    sim_run(200);
    /* to a fresh listener: OOTB */
    peer_t c;
    sim_t keep = S;
    peer_init(&c, &S, 1, NULL);
    tsctp_listen(c.s);
    S.tap = cap_tap;
    ncap = 0;
    tsctp_input(c.s, d, n, S.now);
    CHECK(ncap == 1 && cap[0][12] == 6 && cap[0][13] == 1 && get32(cap[0] + 4) == get32(d + 4),
          "ABORT with T bit and the packet's tag");
    tsctp_free(c.s);
    /* back to the saved run: free what was queued since (the ABORT) */
    for (pkt_t **pp = &S.q; *pp;) {
        bool kept = false;
        for (pkt_t *k = keep.q; k && !kept; k = k->next)
            kept = k == *pp;
        if (kept) {
            pp = &(*pp)->next;
        } else {
            pkt_t *gone = *pp;
            *pp = gone->next;
            free(gone);
        }
    }
    S = keep;
    S.p[1] = &b;
    /* wrong tag to the live server: dropped */
    tsctp_stats_t st;
    tsctp_get_stats(b.s, &st);
    uint32_t bv = st.bad_vtag;
    d[7] ^= 0x55;
    memset(d + 8, 0, 4);
    uint32_t crc = tsctp_crc32c(0, d, n);
    d[8] = (uint8_t)crc;
    d[9] = (uint8_t)(crc >> 8);
    d[10] = (uint8_t)(crc >> 16);
    d[11] = (uint8_t)(crc >> 24);
    tsctp_input(b.s, d, n, S.now);
    tsctp_get_stats(b.s, &st);
    CHECK(st.bad_vtag == bv + 1 && tsctp_state(b.s) == TSCTP_ESTABLISHED, "wrong tag dropped");
    S.tap = NULL;
    free_peers(&a, &b);
}

/* Callbacks may send: a reply from on_message and a greeting from on_event. */
static void greet_ev(void *user, tsctp_event_t ev, tsctp_reason_t why)
{
    peer_t *me = user;
    ev_cb(user, ev, why);
    if (ev == TSCTP_EV_UP) {
        uint8_t m[12] = {0};
        put32(m, 4000);
        tsctp_send(me->s, m, 12, NULL, me->sim->now);
    }
}
static void greet_tweak(tsctp_config_t *c) { c->on_event = greet_ev; }

static void test_callbacks_send(void)
{
    sim_reset();
    peer_t a, b;
    peer_init(&a, &S, 0, greet_tweak);
    peer_init(&b, &S, 1, greet_tweak);
    b.echo = true;
    tsctp_listen(b.s);
    tsctp_connect(a.s, S.now);
    sim_run(2000);
    CHECK(a.seen[4000] && b.seen[4000], "greetings from on_event both ways");
    CHECK(a.got == 2, "and b's echo of a's (%u)", a.got);
    free_peers(&a, &b);
}

int main(void)
{
    test_crc32c();
    test_formats();
    test_unknown_chunks();
    lossy_run("clean", 0, 0, 0, 0, false, 500, 20);
    lossy_run("10% loss", 0.10, 0, 0, 0, false, 2000, 20);
    lossy_run("20% loss, dup, reorder", 0.20, 0.05, 0.10, 8, false, 2000, 20);
    lossy_run("burst sending, 10% loss", 0.10, 0.02, 0.05, 5, false, 3000, 1);
    lossy_run("unordered, 15% loss", 0.15, 0.05, 0.10, 8, true, 1500, 20);
    test_big_lossy();
    test_echo();
    test_prsctp();
    test_expire_unsent();
    test_outage();
    test_init_timeout();
    test_shutdown_abort();
    test_restart();
    test_stale_cookie();
    test_collision();
    test_ootb();
    test_callbacks_send();
    printf("tinysctp: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
