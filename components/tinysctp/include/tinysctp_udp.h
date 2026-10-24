// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * SCTP over UDP (RFC 6951) with BSD sockets: lwIP on ESP-IDF, or any POSIX
 * host. One UDP socket carries one association; the SCTP packet is the UDP
 * payload, sent to the peer's encapsulation port (9899 is usual).
 *
 * Single-threaded use: open, put tsctp_udp_output and `u` in the tsctp
 * config, then loop on tsctp_udp_step(). For a task that others send from,
 * see tinysctp_esp.h (or use tsctp_udp_wait / _drain around a lock).
 */
#ifndef TINYSCTP_UDP_H
#define TINYSCTP_UDP_H

#include <stdbool.h>
#include <stdint.h>

#include "tinysctp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int fd;
    uint8_t peer[28];   /* a sockaddr_in or sockaddr_in6 */
    uint32_t peer_len;
    bool learn_peer;    /* listening: answer whoever sent the packet until established */
    uint32_t sent, send_errors, received;
    uint8_t buf[1500];
} tsctp_udp_t;

/* A UDP socket on `local_port` (0: any) sending to `host`:`port` (an encapsulation port), or with
   host NULL, an IPv4 one that answers whoever writes to it (for tsctp_listen). 0, or -1. */
int tsctp_udp_open(tsctp_udp_t *u, const char *host, uint16_t port, uint16_t local_port);
void tsctp_udp_close(tsctp_udp_t *u);

/* For tsctp_config_t: output (ctx = the tsctp_udp_t), random (ctx unused), and a clock. */
void tsctp_udp_output(void *ctx, const uint8_t *pkt, size_t len);
uint32_t tsctp_udp_random(void *ctx);
uint64_t tsctp_udp_now_ms(void);

/* Wait up to `ms` for a packet. True if one is waiting. */
bool tsctp_udp_wait(tsctp_udp_t *u, uint32_t ms);
/* Read every waiting packet into the association. */
void tsctp_udp_drain(tsctp_udp_t *u, tsctp_t *s);
/* One turn of the loop: wait for a packet or the next timer (at most max_wait_ms), take packets in,
   run the timers. */
void tsctp_udp_step(tsctp_udp_t *u, tsctp_t *s, uint32_t max_wait_ms);

#ifdef __cplusplus
}
#endif

#endif
