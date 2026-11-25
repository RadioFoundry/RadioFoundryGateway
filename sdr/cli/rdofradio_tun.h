// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * rdofradio's route of radio_net (docs/spec/fw/ipgw.md 9.2): a TUN device with radio_net routed to it (Linux; root or
 * CAP_NET_ADMIN), the board's lwIP hook's twin. One owner at a time: the ipgw app or the bench's ipgw op.
 */
#ifndef RDOFRADIO_TUN_H
#define RDOFRADIO_TUN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/ipgw.h"

/* Take radio_net for who; rip: announce it by RIPv2 every 30 s (from rtun_recv; port 520: root or
 * CAP_NET_BIND_SERVICE). False with why when another owner has it, or no TUN device. */
bool rtun_open(const rdof_ipgw_net_t *net, const char *who, bool rip, char *why, size_t cap);
/* The next datagram from the LAN (0: none); the owner's thread. */
size_t rtun_recv(uint8_t *buf, size_t cap);
bool rtun_send(const uint8_t *data, size_t len);
void rtun_close(void);
/* The host's subnet: the first interface up with an IPv4 address that isn't the loopback or the TUN device. */
bool rtun_lan(rdof_ipgw_net_t *out);

#endif
