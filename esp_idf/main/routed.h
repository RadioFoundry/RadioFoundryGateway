// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Routed IP to the radios (docs/spec/fw/ipgw.md 9.2): LAN datagrams to radio_net, taken from lwIP before it drops
 * them as not its own, and whole datagrams out as they are. One owner at a time: the ipgw app or the bench's ipgw op.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "rdof/ipgw.h"

/* Take radio_net for who ("ipgw", "bench"); rip: announce it by RIPv2 every 30 s (from routed_recv). False with why
 * when another owner has it or there's no memory. */
bool routed_open(const rdof_ipgw_net_t *net, const char *who, bool rip, char *why, size_t cap);
/* The next datagram in (0: none), any task; the owner's. */
size_t routed_recv(uint8_t *buf, size_t cap);
/* A whole IPv4 datagram out to the LAN. */
bool routed_send(const uint8_t *data, size_t len);
void routed_close(void);
/* The station's subnet and its own address now; false with no address. */
bool routed_lan(rdof_ipgw_net_t *out, uint32_t *ip);
/* Datagrams dropped (queue full, no memory, too long). */
uint32_t routed_dropped(void);
