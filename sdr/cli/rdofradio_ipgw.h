// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * rdofradio's IP gateway (docs/spec/fw/ipgw.md): rdof_ipgw on the host's sockets, as the board's ipgw task. The main
 * loop's thread calls all of it: the trackers' data bursts go in (rdofgw_heard), each flow has a UDP socket of its
 * own on an ephemeral port, and what comes back goes to the caller's send hook, which transmits it.
 */
#ifndef RDOFRADIO_IPGW_H
#define RDOFRADIO_IPGW_H

#include <stdbool.h>
#include <stdint.h>

#include "rdof/ipgw.h"
#include "rdof/json.h"

/* Start it on cfg (copied). send: a packet to transmit (rdof_ipgw's send_dmr). False: no memory. */
bool rdofgw_start(const rdof_ipgw_config_t *cfg, bool (*send)(void *ctx, const rdof_ipgw_dmr_t *p), void *ctx);
bool rdofgw_on(void);
/* A data burst a tracker decoded (dt: a data header or a rate 1/2, 3/4 or 1 block; others are ignored). source: the
 * caller's number for the receiver; cc -1: from the bursts. */
void rdofgw_heard(int source, int slot, uint32_t freq, int cc, int dt, const uint8_t *burst, double now);
/* The main loop's turn: the sockets, the timers, the routes' names. sync48: the sync of what it sends (MS data for
 * a transceiver, BS data for a repeater). */
void rdofgw_poll(double now, uint64_t sync48);
/* Packets dropped because the transmitter stayed busy (the caller's count, shown with the rest). */
void rdofgw_tx_busy(uint32_t n);
/* "ip_gateway": {...} into j; nothing when it isn't running. */
void rdofgw_status_json(rdof_json_t *j);
/* The same as text for a raw JSON member ("{...}"), "" when off. */
void rdofgw_status_text(char *out, size_t cap);

#endif
