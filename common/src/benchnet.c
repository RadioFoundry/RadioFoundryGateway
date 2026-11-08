// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The bench's line protocol server (rdof/benchnet.h).
 */
#if !defined(ESP_PLATFORM) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE /* glibc: the BSD socket names with -std=c11 */
#endif
#include "rdof/benchnet.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>

#define SEND_CHUNK 16384

struct rdof_bnet {
    rdof_bl_t *bl;
    double (*now)(void);
    int lfd, port;
    int fd[RDOF_BL_CONNS];          /* by the engine's slot, -1 none */
    volatile bool stop;
    pthread_t thread;
    uint8_t buf[SEND_CHUNK];        /* what the engine gives, before the socket takes it */
    size_t pend[RDOF_BL_CONNS], off[RDOF_BL_CONNS];
    uint8_t *pbuf[RDOF_BL_CONNS];   /* a connection's bytes the socket didn't take yet */
};

static void nonblock(int fd)
{
    const int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void drop(rdof_bnet_t *n, int c)
{
    rdof_bl_closed(n->bl, c, n->now());
    close(n->fd[c]);
    n->fd[c] = -1;
    n->pend[c] = n->off[c] = 0;
}

/* Send what the connection has: true while it lives. */
static bool flush(rdof_bnet_t *n, int c)
{
    for (;;) {
        if (n->pend[c] == n->off[c]) {
            n->pend[c] = rdof_bl_output(n->bl, c, n->pbuf[c], SEND_CHUNK);
            n->off[c] = 0;
            if (!n->pend[c])
                return true;
        }
        const ssize_t k = send(n->fd[c], n->pbuf[c] + n->off[c], n->pend[c] - n->off[c], 0);
        if (k < 0)
            return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
        n->off[c] += (size_t)k;
        if (n->off[c] < n->pend[c])
            return true; /* the socket is full: later */
    }
}

static void *serve(void *arg)
{
    rdof_bnet_t *n = arg;
    char in[1024];
    while (!n->stop) {
        fd_set rd, wr;
        FD_ZERO(&rd);
        FD_ZERO(&wr);
        FD_SET(n->lfd, &rd);
        int top = n->lfd;
        for (int c = 0; c < RDOF_BL_CONNS; c++) {
            if (n->fd[c] < 0)
                continue;
            if (rdof_bl_input_room(n->bl, c)) /* a tx stream the mode has no room for waits in the socket */
                FD_SET(n->fd[c], &rd);
            if (n->pend[c] > n->off[c])
                FD_SET(n->fd[c], &wr);
            if (n->fd[c] > top)
                top = n->fd[c];
        }
        struct timeval tv = {0, 10000};
        const int r = select(top + 1, &rd, &wr, NULL, &tv);
        if (r < 0 && errno != EINTR)
            break;
        const double now = n->now();
        if (r > 0 && FD_ISSET(n->lfd, &rd)) {
            const int fd = accept(n->lfd, NULL, NULL);
            if (fd >= 0)
                fcntl(fd, F_SETFD, FD_CLOEXEC);
            if (fd >= 0) {
                const int c = rdof_bl_open(n->bl, now);
                if (c < 0 || c >= RDOF_BL_CONNS || n->fd[c] >= 0) {
                    const char *msg = rdof_bl_refusal();
                    (void)!send(fd, msg, strlen(msg), 0);
                    close(fd);
                    if (c >= 0)
                        rdof_bl_closed(n->bl, c, now);
                } else {
                    const int one = 1;
                    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                    nonblock(fd);
                    n->fd[c] = fd;
                }
            }
        }
        for (int c = 0; r > 0 && c < RDOF_BL_CONNS; c++) {
            if (n->fd[c] < 0 || !FD_ISSET(n->fd[c], &rd))
                continue;
            size_t want = rdof_bl_input_room(n->bl, c);
            if (!want)
                continue;
            const ssize_t k = recv(n->fd[c], in, want < sizeof in ? want : sizeof in, 0);
            if (k > 0)
                rdof_bl_input(n->bl, c, in, (size_t)k, now);
            else if (k == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
                drop(n, c); /* the client closed: its request is cancelled */
        }
        rdof_bl_tick(n->bl, n->now());
        for (int c = 0; c < RDOF_BL_CONNS; c++) {
            if (n->fd[c] < 0)
                continue;
            if (!flush(n, c))
                drop(n, c);
            else if (n->pend[c] == n->off[c] && rdof_bl_finished(n->bl, c))
                drop(n, c);
        }
    }
    for (int c = 0; c < RDOF_BL_CONNS; c++)
        if (n->fd[c] >= 0)
            drop(n, c);
    return NULL;
}

rdof_bnet_t *rdof_bnet_start(rdof_bl_t *bl, const char *addr, int port, double (*now)(void), size_t stack,
                             char *error, size_t cap)
{
    rdof_bnet_t *n = calloc(1, sizeof *n);
    if (!n) {
        snprintf(error, cap, "no memory");
        return NULL;
    }
    n->bl = bl, n->now = now;
    n->lfd = -1;
    for (int c = 0; c < RDOF_BL_CONNS; c++) {
        n->fd[c] = -1;
        n->pbuf[c] = malloc(SEND_CHUNK);
        if (!n->pbuf[c]) {
            snprintf(error, cap, "no memory");
            goto fail;
        }
    }
    n->lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (n->lfd < 0) {
        snprintf(error, cap, "socket: %s", strerror(errno));
        goto fail;
    }
    /* a child the program starts (the DNS-SD publisher) mustn't keep the port: across a restart (exec) it would
     * hold it and the new program couldn't listen */
    fcntl(n->lfd, F_SETFD, FD_CLOEXEC);
    const int one = 1;
    setsockopt(n->lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (addr && addr[0] && inet_pton(AF_INET, addr, &sa.sin_addr) != 1) {
        snprintf(error, cap, "not an IPv4 address: %s", addr);
        goto fail;
    }
    if (bind(n->lfd, (struct sockaddr *)&sa, sizeof sa) || listen(n->lfd, 4)) {
        snprintf(error, cap, "port %d: %s", port, strerror(errno));
        goto fail;
    }
    socklen_t sl = sizeof sa;
    getsockname(n->lfd, (struct sockaddr *)&sa, &sl);
    n->port = ntohs(sa.sin_port);
    nonblock(n->lfd);
    pthread_attr_t at;
    pthread_attr_init(&at);
    if (stack)
        pthread_attr_setstacksize(&at, stack);
    const int e = pthread_create(&n->thread, &at, serve, n);
    pthread_attr_destroy(&at);
    if (e) {
        snprintf(error, cap, "thread: %s", strerror(e));
        goto fail;
    }
    return n;
fail:
    if (n->lfd >= 0)
        close(n->lfd);
    for (int c = 0; c < RDOF_BL_CONNS; c++)
        free(n->pbuf[c]);
    free(n);
    return NULL;
}

int rdof_bnet_port(const rdof_bnet_t *n) { return n ? n->port : 0; }

void rdof_bnet_stop(rdof_bnet_t *n)
{
    if (!n)
        return;
    n->stop = true;
    pthread_join(n->thread, NULL);
    close(n->lfd);
    for (int c = 0; c < RDOF_BL_CONNS; c++)
        free(n->pbuf[c]);
    free(n);
}
