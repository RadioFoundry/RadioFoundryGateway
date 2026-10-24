// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* SCTP over UDP with BSD sockets (lwIP or POSIX). See tinysctp_udp.h. */
#include "tinysctp_udp.h"

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#if defined(ESP_PLATFORM)
#include "esp_random.h"
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <stdlib.h>
#define HAVE_ARC4RANDOM 1
#endif

int tsctp_udp_open(tsctp_udp_t *u, const char *host, uint16_t port, uint16_t local_port)
{
    memset(u, 0, sizeof *u);
    u->fd = -1;
    int family = AF_INET;
    if (host) {
        char ps[8];
        snprintf(ps, sizeof ps, "%u", port);
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof hints);
        hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(host, ps, &hints, &res) != 0 || !res)
            return -1;
        if (res->ai_addrlen <= sizeof u->peer) {
            memcpy(u->peer, res->ai_addr, res->ai_addrlen);
            u->peer_len = (uint32_t)res->ai_addrlen;
            family = res->ai_family;
        }
        freeaddrinfo(res);
        if (!u->peer_len)
            return -1;
    } else {
        u->learn_peer = true;
    }
    u->fd = socket(family, SOCK_DGRAM, 0);
    if (u->fd < 0)
        return -1;
    struct sockaddr_storage a;
    socklen_t alen;
    memset(&a, 0, sizeof a);
    if (family == AF_INET6) {
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&a;
        s6->sin6_family = AF_INET6;
        s6->sin6_port = htons(local_port);
        alen = sizeof *s6;
    } else {
        struct sockaddr_in *s4 = (struct sockaddr_in *)&a;
        s4->sin_family = AF_INET;
        s4->sin_port = htons(local_port);
        alen = sizeof *s4;
    }
    if (bind(u->fd, (struct sockaddr *)&a, alen) != 0) {
        close(u->fd);
        u->fd = -1;
        return -1;
    }
    fcntl(u->fd, F_SETFL, fcntl(u->fd, F_GETFL, 0) | O_NONBLOCK);
    return 0;
}

void tsctp_udp_close(tsctp_udp_t *u)
{
    if (u->fd >= 0)
        close(u->fd);
    u->fd = -1;
}

void tsctp_udp_output(void *ctx, const uint8_t *pkt, size_t len)
{
    tsctp_udp_t *u = ctx;
    if (u->fd < 0 || !u->peer_len)
        return;
    struct sockaddr_storage a;
    memcpy(&a, u->peer, u->peer_len);
    if (sendto(u->fd, pkt, len, 0, (struct sockaddr *)&a, u->peer_len) == (ssize_t)len)
        u->sent++;
    else
        u->send_errors++;
}

uint32_t tsctp_udp_random(void *ctx)
{
    (void)ctx;
#if defined(ESP_PLATFORM)
    return esp_random();
#elif defined(HAVE_ARC4RANDOM)
    return arc4random();
#else
    static FILE *f;
    uint32_t v = 0;
    if (!f)
        f = fopen("/dev/urandom", "rb");
    if (!f || fread(&v, sizeof v, 1, f) != 1) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        v ^= (uint32_t)ts.tv_nsec * 2654435761u;
    }
    return v;
#endif
}

uint64_t tsctp_udp_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

bool tsctp_udp_wait(tsctp_udp_t *u, uint32_t ms)
{
    if (u->fd < 0)
        return false;
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(u->fd, &rd);
    struct timeval tv;
    tv.tv_sec = (time_t)(ms / 1000);
    tv.tv_usec = (suseconds_t)(ms % 1000) * 1000;
    return select(u->fd + 1, &rd, NULL, NULL, &tv) > 0;
}

void tsctp_udp_drain(tsctp_udp_t *u, tsctp_t *s)
{
    for (;;) {
        struct sockaddr_storage from;
        socklen_t flen = sizeof from;
        ssize_t n = recvfrom(u->fd, u->buf, sizeof u->buf, 0, (struct sockaddr *)&from, &flen);
        if (n < 0)
            return;
        u->received++;
        if (u->learn_peer && tsctp_state(s) < TSCTP_ESTABLISHED && flen <= sizeof u->peer) {
            memcpy(u->peer, &from, flen);
            u->peer_len = flen;
        }
        tsctp_input(s, u->buf, (size_t)n, tsctp_udp_now_ms());
    }
}

void tsctp_udp_step(tsctp_udp_t *u, tsctp_t *s, uint32_t max_wait_ms)
{
    uint64_t now = tsctp_udp_now_ms(), next = tsctp_poll(s, now);
    uint32_t wait = next <= now ? 0 : next - now < max_wait_ms ? (uint32_t)(next - now) : max_wait_ms;
    if (tsctp_udp_wait(u, wait))
        tsctp_udp_drain(u, s);
    tsctp_poll(s, tsctp_udp_now_ms());
}
