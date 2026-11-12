// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * An HBlink4 peer for the host tools: one UDP socket to the master, polled from the caller's loop (no thread),
 * around the board's peer state (common/src/hbpeer.c): the keepalive, DMRP, the refusals. Packets other than those
 * go to on_packet. With a login (BrandMeister), the login's steps and pings in place of the DMRC, and no DMRD up
 * until logged in. UDP only (no SCTP yet). POSIX sockets.
 */
#ifndef RDOFRADIO_NET_H
#define RDOFRADIO_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/hbpeer.h"

typedef struct {
    char host[64];
    int port;
    char label[24];                /* for the messages: the role's or channel's name */
    void (*on_packet)(void *ctx, const uint8_t *d, size_t n);
    void (*say)(const char *line); /* a status line (registered, refused, gone quiet); NULL: stderr */
    void *ctx;
    rdof_hb_peer_t peer;
    int sock;                      /* -1: not open */
    double next_open;
    unsigned errors;
} rdofnet_peer_t;

void rdofnet_peer_init(rdofnet_peer_t *p, const char *host, int port, uint32_t peer_id, double keepalive_s,
                       const char *label);
/* Log in (a brandmeister network, after init): rdof_hb_peer_set_login. */
void rdofnet_peer_set_login(rdofnet_peer_t *p, const char *password, const char *location, const char *description,
                            const char *url);
/* The login's dialect: rdof_hb_peer_set_dl5di. */
void rdofnet_peer_set_dl5di(rdofnet_peer_t *p, bool on);
/* The DMRC to keep alive with (with a login: the RPTC's source); sent at once when it changes. */
void rdofnet_peer_set_config(rdofnet_peer_t *p, const uint8_t *dmrc, size_t n, double now);
void rdofnet_peer_send(rdofnet_peer_t *p, const uint8_t *d, size_t n);
/* Open (again, every 5 s while it fails), keep alive, and take every packet waiting. Never blocks, but for the
   name lookup when it opens. */
void rdofnet_peer_poll(rdofnet_peer_t *p, double now);
void rdofnet_peer_close(rdofnet_peer_t *p);

#endif
