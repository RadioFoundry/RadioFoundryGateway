// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "rdofradio_ipgw.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define RESOLVE_EVERY_S 30.0
#define TICK_EVERY_S 0.25

static rdof_ipgw_t *G;
static int g_sock[RDOF_IPGW_MAX_FLOWS];
static int g_ttl[RDOF_IPGW_MAX_FLOWS];       /* the IP_TTL each socket has */
static uint16_t g_port[RDOF_IPGW_MAX_FLOWS]; /* the local port each is bound to */
static int g_icmp = -1, g_icmp_ttl;          /* NAT's ICMP: one raw socket (it hands over the IPv4 header too) */
static double g_next_tick;

static void nonblock(int s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }

static bool open_flow(void *ctx, int flow)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return false;
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = 0, .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) {
        close(s);
        return false;
    }
    nonblock(s);
    socklen_t al = sizeof a;
    g_port[flow] = getsockname(s, (struct sockaddr *)&a, &al) == 0 ? ntohs(a.sin_port) : 0;
    g_sock[flow] = s;
    g_ttl[flow] = 0;
    return true;
}

static void close_flow(void *ctx, int flow)
{
    if (g_sock[flow] >= 0)
        close(g_sock[flow]);
    g_sock[flow] = -1;
}

static bool send_udp(void *ctx, int flow, uint32_t ip, uint16_t port, const uint8_t *data, size_t len, int ttl)
{
    if (g_sock[flow] < 0)
        return false;
    /* the radio's TTL less one, as a router would; a whole datagram (0) goes in UDP with the stack's own */
    int want = ttl > 0 ? ttl : 64;
    if (want != g_ttl[flow] && setsockopt(g_sock[flow], IPPROTO_IP, IP_TTL, &want, sizeof want) == 0)
        g_ttl[flow] = want;
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(ip)};
    return sendto(g_sock[flow], data, len, 0, (struct sockaddr *)&a, sizeof a) == (ssize_t)len;
}

static uint16_t local_port(void *ctx, int flow) { return g_port[flow]; }

static bool send_icmp(void *ctx, uint32_t ip, const uint8_t *m, size_t len, int ttl)
{
    if (g_icmp < 0)
        return false;
    if (ttl != g_icmp_ttl && setsockopt(g_icmp, IPPROTO_IP, IP_TTL, &ttl, sizeof ttl) == 0)
        g_icmp_ttl = ttl;
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(ip)};
    return sendto(g_icmp, m, len, 0, (struct sockaddr *)&a, sizeof a) == (ssize_t)len;
}

/* The routes' host names, on a thread of its own (a lookup can take seconds, and the main loop has the radio): each
   route's address is one word, written once here and read by the main loop. */
static void *resolver(void *arg)
{
    for (;; sleep((unsigned)RESOLVE_EVERY_S)) {
        bool left = false;
        for (int i = 0; i < G->cfg.nroutes; i++) {
            rdof_ipgw_route_t *r = &G->cfg.routes[i];
            if (r->ip)
                continue;
            struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM}, *res = NULL;
            if (getaddrinfo(r->host, NULL, &hints, &res) == 0 && res) {
                const uint32_t ip = ntohl(((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr);
                fprintf(stderr, "ip gateway: route %s: %s is %u.%u.%u.%u\n", r->name, r->host, (unsigned)(ip >> 24),
                        (unsigned)(ip >> 16 & 255), (unsigned)(ip >> 8 & 255), (unsigned)(ip & 255));
                r->ip = ip;
            } else {
                left = true;
            }
            if (res)
                freeaddrinfo(res);
        }
        if (!left)
            return NULL;
    }
}

bool rdofgw_start(const rdof_ipgw_config_t *cfg, bool (*send)(void *ctx, const rdof_ipgw_dmr_t *p), void *ctx)
{
    for (int i = 0; i < RDOF_IPGW_MAX_FLOWS; i++)
        g_sock[i] = -1;
    rdof_ipgw_t *g = calloc(1, sizeof *g);
    if (!g)
        return false;
    rdof_ipgw_init(g, cfg);
    g->open = open_flow;
    g->close = close_flow;
    g->send_udp = send_udp;
    g->send_dmr = send;
    g->local_port = local_port;
    g->ctx = ctx;
    if (cfg->nat) {
        g_icmp = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP); /* root or CAP_NET_RAW */
        if (g_icmp >= 0) {
            nonblock(g_icmp);
            g->send_icmp = send_icmp;
        } else {
            fprintf(stderr, "ip gateway: no ICMP socket (it needs root or CAP_NET_RAW): NAT is UDP only\n");
        }
    }
    char d[320];
    rdof_ipgw_describe(cfg, d, sizeof d);
    fprintf(stderr, "ip gateway: %s\n", d);
    G = g;
    bool names = false;
    for (int i = 0; i < g->cfg.nroutes; i++)
        names |= !g->cfg.routes[i].ip;
    pthread_t th;
    if (names && pthread_create(&th, NULL, resolver, NULL) == 0)
        pthread_detach(th);
    return true;
}

bool rdofgw_on(void) { return G != NULL; }

void rdofgw_heard(int source, int slot, uint32_t freq, int cc, int dt, const uint8_t *burst, double now)
{
    if (!G)
        return;
    const rdof_ipgw_where_t w = {.source = source, .slot = slot, .freq = freq, .cc = cc >= 0 && cc <= 15 ? cc : -1};
    rdof_ipgw_burst(G, &w, dt, burst, now);
}

void rdofgw_poll(double now, uint64_t sync48)
{
    static uint8_t buf[RDOF_IPGW_MAX_PACKET + 64];
    if (!G)
        return;
    G->sync48 = sync48;
    if (g_icmp >= 0) {
        ssize_t n;
        while ((n = recv(g_icmp, buf, sizeof buf, 0)) > 0)
            rdof_ipgw_icmp_from_net(G, buf, (size_t)n, now);
    }
    for (int i = 0; i < RDOF_IPGW_MAX_FLOWS; i++) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        ssize_t n;
        while (g_sock[i] >= 0 &&
               (n = recvfrom(g_sock[i], buf, sizeof buf, 0, (struct sockaddr *)&from, &fl)) >= 0) {
            rdof_ipgw_from_net(G, i, ntohl(from.sin_addr.s_addr), ntohs(from.sin_port), buf, (size_t)n, now);
            fl = sizeof from;
        }
    }
    if (now >= g_next_tick) {
        rdof_ipgw_tick(G, now);
        g_next_tick = now + TICK_EVERY_S;
    }
}

void rdofgw_tx_busy(uint32_t n)
{
    if (G)
        G->n.tx_busy = n;
}

void rdofgw_status_json(rdof_json_t *j)
{
    if (G)
        rdof_ipgw_status_json(G, j);
}

void rdofgw_status_text(char *out, size_t cap)
{
    if (cap)
        out[0] = 0;
    if (!G || cap < 16)
        return;
    /* {"ip_gateway":{...}} with the wrapper cut away */
    static char buf[2048];
    rdof_json_t j;
    rdof_json_init(&j, buf, sizeof buf);
    rdof_json_open(&j, NULL);
    rdof_ipgw_status_json(G, &j);
    rdof_json_close(&j);
    if (!rdof_json_done(&j))
        return;
    const char *a = strchr(buf + 1, '{');
    const char *z = strrchr(buf, '}');
    if (!a || !z || z <= a)
        return;
    const size_t n = (size_t)(z - a); /* up to, not with, the wrapper's closing brace */
    if (n < cap) {
        memcpy(out, a, n);
        out[n] = 0;
    }
}
