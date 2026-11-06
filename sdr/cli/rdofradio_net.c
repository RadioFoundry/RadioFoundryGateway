// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* An HBlink4 peer for the host tools (rdofradio_net.h). */
#include "rdofradio_net.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void sayf(rdofnet_peer_t *p, const char *fmt, ...)
{
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (p->say)
        p->say(line);
    else
        fprintf(stderr, "%s\n", line);
}

void rdofnet_peer_init(rdofnet_peer_t *p, const char *host, int port, uint32_t peer_id, double keepalive_s,
                       const char *label)
{
    memset(p, 0, sizeof *p);
    snprintf(p->host, sizeof p->host, "%s", host);
    snprintf(p->label, sizeof p->label, "%s", label);
    p->port = port;
    rdof_hb_peer_init(&p->peer, peer_id, keepalive_s);
    p->sock = -1;
}

static int open_udp(const char *host, int port)
{
    char ps[8];
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res)
        return -1;
    int s = -1;
    for (const struct addrinfo *a = res; a && s < 0; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s >= 0 && connect(s, a->ai_addr, a->ai_addrlen) != 0) {
            close(s);
            s = -1;
        }
    }
    freeaddrinfo(res);
    if (s >= 0)
        fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
    return s;
}

void rdofnet_peer_set_login(rdofnet_peer_t *p, const char *password, const char *location, const char *description,
                            const char *url)
{
    rdof_hb_peer_set_login(&p->peer, password, location, description, url);
}

void rdofnet_peer_send(rdofnet_peer_t *p, const uint8_t *d, size_t n)
{
    if (p->sock < 0)
        return;
    if (n >= 4 && !memcmp(d, "DMRD", 4) && !rdof_hb_peer_may_send(&p->peer))
        return; /* not logged in yet */
    if (send(p->sock, d, n, 0) == (ssize_t)n)
        p->peer.tx++;
    else
        p->errors++; /* no route yet, or the master's port refused: the keepalives keep trying */
}

void rdofnet_peer_set_config(rdofnet_peer_t *p, const uint8_t *dmrc, size_t n, double now)
{
    if (rdof_hb_peer_set_config(&p->peer, dmrc, n, false, now))
        rdofnet_peer_send(p, p->peer.dmrc, p->peer.dmrc_len);
}

void rdofnet_peer_poll(rdofnet_peer_t *p, double now)
{
    const unsigned id = (unsigned)p->peer.peer_id;
    if (p->sock < 0) {
        if (now < p->next_open)
            return;
        p->next_open = now + 5.0;
        p->sock = open_udp(p->host, p->port);
        if (p->sock < 0)
            return;
        rdof_hb_peer_link_up(&p->peer);
    }
    rdof_hbp_event_t ev;
    const char *net = p->peer.login ? "brandmeister" : "hblink";
    if (rdof_hb_peer_tick(&p->peer, now, &ev) && p->peer.dmrc_len) {
        uint8_t out[RDOF_HB_OUT_MAX];
        rdofnet_peer_send(p, out, rdof_hb_peer_packet(&p->peer, out));
    }
    if (ev == RDOF_HBP_QUIET)
        sayf(p, "%s: %s (peer %u): no %s from %s:%d", net, p->label, id, p->peer.login ? "MSTPONG" : "DMRP", p->host,
             p->port);
    uint8_t buf[1500];
    for (;;) {
        const ssize_t n = recv(p->sock, buf, sizeof buf, 0);
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR && errno != ECONNREFUSED)
                p->errors++;
            return;
        }
        const unsigned refused = p->peer.refused;
        switch (rdof_hb_peer_received(&p->peer, buf, (size_t)n, now)) {
        case RDOF_HBP_REGISTERED:
            sayf(p, "%s: %s (peer %u) %s %s:%d", net, p->label, id, p->peer.login ? "logged in to" : "registered with",
                 p->host, p->port);
            break;
        case RDOF_HBP_REFUSED:
            if (p->peer.login) /* the login backs off: each refusal is worth a line */
                sayf(p, "%s: %s (peer %u) refused by the master: %.6s%s, next try in %.0f s", net, p->label, id,
                     (const char *)buf, p->peer.bad_password ? " (the password)" : "", p->peer.next_ka - now);
            else if (!refused) /* it refuses every keepalive: say it once */
                sayf(p, "%s: %s (peer %u) refused by the master: %.6s", net, p->label, id, (const char *)buf);
            break;
        case RDOF_HBP_PACKET:
            if (p->on_packet)
                p->on_packet(p->ctx, buf, (size_t)n);
            break;
        default:
            break;
        }
    }
}

void rdofnet_peer_close(rdofnet_peer_t *p)
{
    uint8_t cl[9];
    const size_t n = rdof_hb_peer_close_packet(&p->peer, cl);
    if (p->sock >= 0 && n && p->peer.state == RDOF_HBL_UP)
        rdofnet_peer_send(p, cl, n); /* RPTCL: the master drops the login now, not after its timeout */
    if (p->sock >= 0)
        close(p->sock);
    p->sock = -1;
    rdof_hb_peer_link_down(&p->peer);
}
