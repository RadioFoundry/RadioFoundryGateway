// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * tinysctp UDP glue tests (src/udp.c) over real loopback sockets: open and
 * its failures, output, the clock and random source, wait / drain / step, two
 * associations talking through the glue, port learning on a listener and the
 * 1500-byte receive buffer.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include "tinysctp.h"
#include "tinysctp_udp.h"

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

/* The port a udp glue socket is bound to. */
static uint16_t bound_port(const tsctp_udp_t *u)
{
    struct sockaddr_storage a;
    socklen_t l = sizeof a;
    if (getsockname(u->fd, (struct sockaddr *)&a, &l) != 0)
        return 0;
    return ntohs(a.ss_family == AF_INET6 ? ((struct sockaddr_in6 *)&a)->sin6_port : ((struct sockaddr_in *)&a)->sin_port);
}

/* A plain IPv4 / IPv6 loopback UDP socket on a free port, for sending from and receiving on. */
static int plain_socket(int family, uint16_t *port)
{
    int fd = socket(family, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_storage a;
    socklen_t l;
    memset(&a, 0, sizeof a);
    if (family == AF_INET6) {
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)&a;
        s->sin6_family = AF_INET6;
        s->sin6_addr = in6addr_loopback;
        l = sizeof *s;
    } else {
        struct sockaddr_in *s = (struct sockaddr_in *)&a;
        s->sin_family = AF_INET;
        s->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        l = sizeof *s;
    }
    if (bind(fd, (struct sockaddr *)&a, l) != 0) {
        close(fd);
        return -1;
    }
    getsockname(fd, (struct sockaddr *)&a, &l);
    *port = ntohs(family == AF_INET6 ? ((struct sockaddr_in6 *)&a)->sin6_port : ((struct sockaddr_in *)&a)->sin_port);
    return fd;
}

static void send_to(int fd, uint16_t port, const void *data, size_t len)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(fd, data, len, 0, (struct sockaddr *)&a, sizeof a);
}

/* One datagram from a plain socket, waiting up to a second for it. */
static ssize_t recv_wait(int fd, void *buf, size_t len)
{
    struct timeval tv = {1, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return recv(fd, buf, len, 0);
}

static void test_open(void)
{
    tsctp_udp_t u;
    memset(&u, 0x55, sizeof u);
    CHECK(tsctp_udp_open(&u, "127.0.0.1", 9899, 0) == 0, "open to an IPv4 literal");
    CHECK(u.fd >= 0 && u.peer_len == sizeof(struct sockaddr_in) && !u.learn_peer, "fd %d, peer_len %u", u.fd, u.peer_len);
    CHECK(u.sent == 0 && u.send_errors == 0 && u.received == 0, "the rest of the struct is cleared");
    struct sockaddr_in p;
    memcpy(&p, u.peer, sizeof p);
    CHECK(p.sin_family == AF_INET && ntohs(p.sin_port) == 9899 && p.sin_addr.s_addr == htonl(INADDR_LOOPBACK),
          "peer is 127.0.0.1:9899");
    CHECK(bound_port(&u) != 0, "bound to an ephemeral port");
    tsctp_udp_close(&u);
    CHECK(u.fd == -1, "close sets fd -1");
    tsctp_udp_close(&u); /* a second close does nothing */
    CHECK(u.fd == -1, "closed twice");

    /* the local port is honored, and a port in use makes bind fail */
    uint16_t held;
    int fd = plain_socket(AF_INET, &held);
    CHECK(fd >= 0, "a socket to hold a port");
    tsctp_udp_t v;
    CHECK(tsctp_udp_open(&v, "127.0.0.1", 9899, held) == -1, "bind to a port in use fails");
    CHECK(v.fd == -1, "and leaves no socket");
    close(fd);
    CHECK(tsctp_udp_open(&v, "localhost", 9899, held) == 0 && bound_port(&v) == held, "the freed port binds, by name");
    tsctp_udp_close(&v);

    /* no resolution */
    CHECK(tsctp_udp_open(&v, "999.999.999.999", 9899, 0) == -1, "a bad address does not resolve");
    CHECK(v.fd == -1 && v.peer_len == 0, "and leaves nothing open");

    /* listening: IPv4, learns the peer */
    CHECK(tsctp_udp_open(&v, NULL, 0, 0) == 0 && v.learn_peer && v.peer_len == 0 && v.fd >= 0, "listener");
    struct sockaddr_storage a;
    socklen_t l = sizeof a;
    getsockname(v.fd, (struct sockaddr *)&a, &l);
    CHECK(a.ss_family == AF_INET, "a listener is IPv4");
    tsctp_udp_close(&v);

    /* no descriptor to be had: the socket call fails, and nothing is left open */
    struct rlimit lim = {0, 0}, low;
    int probe = dup(0);
    CHECK(probe >= 0 && getrlimit(RLIMIT_NOFILE, &lim) == 0, "descriptor limit");
    close(probe);
    low = lim;
    low.rlim_cur = (rlim_t)probe;
    CHECK(setrlimit(RLIMIT_NOFILE, &low) == 0, "lower the limit");
    CHECK(tsctp_udp_open(&v, "127.0.0.1", 9899, 0) == -1 && v.fd == -1, "socket() failing: -1");
    CHECK(tsctp_udp_open(&v, NULL, 0, 0) == -1 && v.fd == -1, "and for a listener");
    setrlimit(RLIMIT_NOFILE, &lim);

    /* IPv6 */
    uint16_t p6;
    int fd6 = plain_socket(AF_INET6, &p6);
    if (fd6 < 0) {
        printf("tinysctp udp: no IPv6 loopback here, skipping that part\n");
    } else {
        tsctp_udp_t w;
        CHECK(tsctp_udp_open(&w, "::1", p6, 0) == 0, "open to an IPv6 literal");
        CHECK(w.peer_len == sizeof(struct sockaddr_in6), "peer_len %u", w.peer_len);
        l = sizeof a;
        getsockname(w.fd, (struct sockaddr *)&a, &l);
        CHECK(a.ss_family == AF_INET6, "an IPv6 socket");
        static const uint8_t pkt[5] = {1, 2, 3, 4, 5};
        tsctp_udp_output(&w, pkt, sizeof pkt);
        uint8_t rx[16];
        ssize_t n = recv_wait(fd6, rx, sizeof rx);
        CHECK(n == 5 && memcmp(rx, pkt, 5) == 0 && w.sent == 1, "a packet reaches the IPv6 peer (%zd)", n);
        tsctp_udp_close(&w);
        close(fd6);
    }
}

static void test_output(void)
{
    uint16_t port;
    int rx = plain_socket(AF_INET, &port);
    tsctp_udp_t u;
    CHECK(rx >= 0 && tsctp_udp_open(&u, "127.0.0.1", port, 0) == 0, "set up");
    uint8_t pkt[64];
    for (int i = 0; i < 64; i++)
        pkt[i] = (uint8_t)(i * 3);
    tsctp_udp_output(&u, pkt, sizeof pkt);
    uint8_t got[128];
    ssize_t n = recv_wait(rx, got, sizeof got);
    CHECK(n == 64 && memcmp(got, pkt, 64) == 0, "the packet is the datagram (%zd)", n);
    CHECK(u.sent == 1 && u.send_errors == 0, "sent counted");

    /* too big for a datagram: counted as an error */
    static uint8_t big[70000];
    tsctp_udp_output(&u, big, sizeof big);
    CHECK(u.sent == 1 && u.send_errors == 1, "an oversized packet is a send error (%u/%u)", u.sent, u.send_errors);

    /* nothing to send to, or no socket: silent */
    tsctp_udp_t l;
    tsctp_udp_open(&l, NULL, 0, 0);
    tsctp_udp_output(&l, pkt, sizeof pkt);
    CHECK(l.sent == 0 && l.send_errors == 0, "a listener with no peer yet sends nothing");
    tsctp_udp_close(&l);
    tsctp_udp_output(&l, pkt, sizeof pkt);
    CHECK(l.sent == 0 && l.send_errors == 0, "a closed socket sends nothing");
    tsctp_udp_close(&u);
    tsctp_udp_output(&u, pkt, sizeof pkt);
    CHECK(u.sent == 1, "nor does a closed connector");
    close(rx);
}

static void test_clock_random(void)
{
    uint64_t a = tsctp_udp_now_ms();
    usleep(30000);
    uint64_t b = tsctp_udp_now_ms();
    CHECK(b >= a + 25 && b < a + 2000, "the clock moves with real time (%llu -> %llu)", (unsigned long long)a,
          (unsigned long long)b);
    uint32_t first = tsctp_udp_random(NULL), v = first;
    int differ = 0;
    for (int i = 0; i < 16; i++) {
        uint32_t r = tsctp_udp_random(NULL);
        differ += r != v;
        v = r;
    }
    CHECK(differ >= 14, "the random source varies (%d of 16 changed)", differ);
}

static void test_wait_drain(void)
{
    tsctp_udp_t u;
    tsctp_udp_open(&u, NULL, 0, 0);
    uint16_t port = bound_port(&u);
    CHECK(!tsctp_udp_wait(&u, 0), "nothing waiting, no wait");
    uint64_t t0 = tsctp_udp_now_ms();
    CHECK(!tsctp_udp_wait(&u, 40), "nothing waiting");
    CHECK(tsctp_udp_now_ms() - t0 >= 30, "it did wait");
    uint16_t sp;
    int s = plain_socket(AF_INET, &sp);
    send_to(s, port, "x", 1);
    CHECK(tsctp_udp_wait(&u, 1000), "a datagram is waiting");
    CHECK(tsctp_udp_wait(&u, 1000), "and stays waiting until read");
    tsctp_udp_close(&u);
    CHECK(!tsctp_udp_wait(&u, 1000), "a closed socket never waits");
    close(s);
}

/* ── two associations through the glue ────────────────────────────────────── */

typedef struct {
    tsctp_t *s;
    tsctp_udp_t *u;
    int up, got, restart;
    uint8_t last[4000];
    size_t last_len;
} end_t;

static void on_msg(void *user, const tsctp_msg_t *m)
{
    end_t *e = user;
    e->got++;
    e->last_len = m->len < sizeof e->last ? m->len : sizeof e->last;
    memcpy(e->last, m->data, e->last_len);
}

static void on_ev(void *user, tsctp_event_t ev, tsctp_reason_t why)
{
    end_t *e = user;
    if (ev == TSCTP_EV_UP)
        e->up++;
    if (ev == TSCTP_EV_RESTART)
        e->restart++;
}

static void end_init(end_t *e, tsctp_udp_t *u, uint16_t local_port, uint16_t peer_port)
{
    memset(e, 0, sizeof *e);
    e->u = u;
    tsctp_config_t cfg;
    tsctp_config_defaults(&cfg);
    cfg.local_port = local_port;
    cfg.peer_port = peer_port;
    cfg.rto_initial_ms = 100;
    cfg.rto_min_ms = 50;
    cfg.hb_interval_ms = 0;
    cfg.sack_delay_ms = 10;
    cfg.output = tsctp_udp_output;
    cfg.output_ctx = u;
    cfg.random = tsctp_udp_random;
    cfg.on_message = on_msg;
    cfg.on_event = on_ev;
    cfg.user = e;
    e->s = tsctp_new(&cfg, tsctp_udp_now_ms());
}

/* Step both ends until `done` or a deadline. */
static bool run_until(end_t *a, end_t *b, bool (*done)(end_t *, end_t *), int ms)
{
    uint64_t t0 = tsctp_udp_now_ms();
    while (tsctp_udp_now_ms() - t0 < (uint64_t)ms) {
        tsctp_udp_step(a->u, a->s, 5);
        tsctp_udp_step(b->u, b->s, 5);
        if (done(a, b))
            return true;
    }
    return false;
}

static bool both_up(end_t *a, end_t *b)
{
    return tsctp_state(a->s) == TSCTP_ESTABLISHED && tsctp_state(b->s) == TSCTP_ESTABLISHED;
}
static bool both_got(end_t *a, end_t *b) { return a->got >= 1 && b->got >= 1; }
/* the listener goes back to listening */
static bool a_closed(end_t *a, end_t *b) { return tsctp_state(a->s) == TSCTP_CLOSED && tsctp_state(b->s) == TSCTP_LISTEN; }

static void test_association(void)
{
    tsctp_udp_t ua, ub;
    CHECK(tsctp_udp_open(&ub, NULL, 0, 0) == 0, "listener");
    uint16_t bport = bound_port(&ub);
    CHECK(tsctp_udp_open(&ua, "127.0.0.1", bport, 0) == 0, "connector");
    end_t a, b;
    end_init(&a, &ua, 5001, 5002);
    end_init(&b, &ub, 5002, 0);
    CHECK(a.s && b.s, "associations");

    /* a stray datagram before anything starts: the listener answers whoever wrote last, so far */
    uint16_t sp;
    int stray = plain_socket(AF_INET, &sp);
    tsctp_listen(b.s);
    send_to(stray, bport, "junkjunkjunkjunk", 16);
    tsctp_udp_wait(&ub, 1000);
    tsctp_udp_drain(&ub, b.s);
    struct sockaddr_in p;
    memcpy(&p, ub.peer, sizeof p);
    CHECK(ub.received == 1 && ub.peer_len == sizeof p && ntohs(p.sin_port) == sp, "the stray's address is learned");
    tsctp_stats_t st;
    tsctp_get_stats(b.s, &st);
    CHECK(st.pkts_in == 1, "and the junk reached the core (%u)", st.pkts_in);

    tsctp_connect(a.s, tsctp_udp_now_ms());
    CHECK(run_until(&a, &b, both_up, 5000), "established over loopback (a %s, b %s)", tsctp_state_name(tsctp_state(a.s)),
          tsctp_state_name(tsctp_state(b.s)));
    CHECK(a.up == 1 && b.up == 1, "UP once each");
    uint16_t aport = bound_port(&ua);
    memcpy(&p, ub.peer, sizeof p);
    CHECK(ntohs(p.sin_port) == aport, "the listener's peer is now the connector's port (%u vs %u)", ntohs(p.sin_port), aport);
    CHECK(ua.sent > 0 && ub.sent > 0 && ua.received > 0 && ub.received > 1, "counters moved (%u/%u, %u/%u)", ua.sent,
          ua.received, ub.sent, ub.received);
    CHECK(ua.send_errors == 0 && ub.send_errors == 0, "no send errors");

    /* messages both ways */
    uint8_t m[300];
    for (int i = 0; i < 300; i++)
        m[i] = (uint8_t)i;
    CHECK(tsctp_send(a.s, m, 300, &(tsctp_sendinfo_t){0}, tsctp_udp_now_ms()) == TSCTP_OK, "a sends");
    CHECK(tsctp_send(b.s, "hello", 5, &(tsctp_sendinfo_t){0}, tsctp_udp_now_ms()) == TSCTP_OK, "b sends");
    CHECK(run_until(&a, &b, both_got, 3000), "both messages arrive");
    CHECK(b.last_len == 300 && memcmp(b.last, m, 300) == 0, "300 bytes intact");
    CHECK(a.last_len == 5 && memcmp(a.last, "hello", 5) == 0, "5 bytes intact");

    /* a large message needs several datagrams */
    static uint8_t big[4000];
    for (size_t i = 0; i < sizeof big; i++)
        big[i] = (uint8_t)(i * 7);
    b.got = 0;
    CHECK(tsctp_send(a.s, big, sizeof big, &(tsctp_sendinfo_t){0}, tsctp_udp_now_ms()) == TSCTP_OK, "a sends 4000");
    uint64_t t0 = tsctp_udp_now_ms();
    while (!b.got && tsctp_udp_now_ms() - t0 < 3000) {
        tsctp_udp_step(&ua, a.s, 5);
        tsctp_udp_step(&ub, b.s, 5);
    }
    CHECK(b.got == 1 && b.last_len == 4000 && memcmp(b.last, big, 4000) == 0, "fragments reassembled (%zu)", b.last_len);

    /* once established the listener keeps its peer, whoever writes */
    uint32_t pin = 0;
    tsctp_get_stats(b.s, &st);
    pin = st.pkts_in;
    uint32_t recv_before = ub.received;
    send_to(stray, bport, "more junk 123456", 16);
    tsctp_udp_wait(&ub, 1000);
    tsctp_udp_drain(&ub, b.s);
    memcpy(&p, ub.peer, sizeof p);
    CHECK(ntohs(p.sin_port) == aport, "the peer address is not changed after establishment");
    tsctp_get_stats(b.s, &st);
    CHECK(ub.received == recv_before + 1 && st.pkts_in == pin + 1, "but the datagram is still counted and passed in");
    CHECK(tsctp_state(b.s) == TSCTP_ESTABLISHED, "and the association stands");

    /* the 1500-byte buffer: a longer datagram is cut, fails the CRC, and is counted */
    static uint8_t huge[3000];
    memset(huge, 0xA5, sizeof huge);
    uint32_t bad_before = st.bad_checksum;
    send_to(stray, bport, huge, sizeof huge);
    tsctp_udp_wait(&ub, 1000);
    tsctp_udp_drain(&ub, b.s);
    tsctp_get_stats(b.s, &st);
    CHECK(st.bad_checksum == bad_before + 1 && st.pkts_in == pin + 2, "a 3000-byte datagram is cut to the buffer and fails the CRC");
    CHECK(tsctp_state(b.s) == TSCTP_ESTABLISHED, "the association survives it");

    /* several datagrams are all taken in one drain */
    uint32_t r0 = ub.received;
    for (int i = 0; i < 5; i++)
        send_to(stray, bport, "xxxxxxxxxxxxxxxx", 16);
    tsctp_udp_wait(&ub, 1000);
    usleep(100000); /* readable at the first: loopback can still be delivering the rest */
    tsctp_udp_drain(&ub, b.s);
    CHECK(ub.received == r0 + 5, "drain empties the socket (%u)", ub.received - r0);
    CHECK(!tsctp_udp_wait(&ub, 0), "and nothing is left");

    /* graceful close through the glue */
    tsctp_shutdown(a.s, tsctp_udp_now_ms());
    CHECK(run_until(&a, &b, a_closed, 5000), "shutdown completes");

    tsctp_free(a.s);
    tsctp_free(b.s);
    tsctp_udp_close(&ua);
    tsctp_udp_close(&ub);
    close(stray);
}

/* step: with nothing to wait for it returns after at most max_wait_ms; with a timer due it does not oversleep. */
static void test_step(void)
{
    tsctp_udp_t u;
    uint16_t dead;
    int hold = plain_socket(AF_INET, &dead);
    tsctp_udp_open(&u, "127.0.0.1", dead, 0);
    end_t e;
    end_init(&e, &u, 6001, 6002);
    uint64_t t0 = tsctp_udp_now_ms();
    tsctp_udp_step(&u, e.s, 60); /* idle: no timers, so the cap applies */
    uint64_t dt = tsctp_udp_now_ms() - t0;
    CHECK(dt >= 50 && dt < 500, "idle step waits the cap (%llu ms)", (unsigned long long)dt);

    tsctp_connect(e.s, tsctp_udp_now_ms()); /* INIT out; T1 due in rto_initial (100 ms) */
    tsctp_udp_step(&u, e.s, 0);
    uint8_t rx[200];
    CHECK(recv_wait(hold, rx, sizeof rx) > 0, "the INIT went out on the wire");
    t0 = tsctp_udp_now_ms();
    tsctp_udp_step(&u, e.s, 5000);
    dt = tsctp_udp_now_ms() - t0;
    CHECK(dt >= 60 && dt < 1000, "a step ends at the timer, not the cap (%llu ms)", (unsigned long long)dt);
    CHECK(recv_wait(hold, rx, sizeof rx) > 0, "and the timer's INIT retransmission was sent");
    tsctp_stats_t st;
    tsctp_get_stats(e.s, &st);
    CHECK(st.t3_expiries + st.pkts_out >= 2, "two INITs out (%u)", st.pkts_out);

    /* a datagram wakes the step early */
    uint16_t sp;
    int s = plain_socket(AF_INET, &sp);
    send_to(s, bound_port(&u), "zz", 2);
    t0 = tsctp_udp_now_ms();
    tsctp_udp_step(&u, e.s, 5000);
    dt = tsctp_udp_now_ms() - t0;
    CHECK(dt < 1000 && u.received == 1, "a packet is taken in (%u, %llu ms)", u.received, (unsigned long long)dt);

    tsctp_free(e.s);
    tsctp_udp_close(&u);
    close(s);
    close(hold);
}

int main(void)
{
    test_open();
    test_output();
    test_clock_random();
    test_wait_drain();
    test_association();
    test_step();
    printf("tinysctp udp: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
