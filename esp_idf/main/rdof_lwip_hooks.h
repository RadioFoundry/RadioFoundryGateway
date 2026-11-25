// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * lwIP hooks of our own (ESP_IDF_LWIP_HOOK_FILENAME, set on lwIP by the project's CMakeLists.txt with
 * CONFIG_RDOF_IPGW). The IP gateway's routed mode takes IPv4 datagrams to radio_net before lwIP drops them as not
 * its own (ipgw_task.c).
 */
#pragma once

#include "lwip/netif.h"
#include "lwip/pbuf.h"

/* On lwIP's thread, for every IPv4 datagram in: 1 when it took p (and freed it). */
int rdof_ip4_input_hook(struct pbuf *p, struct netif *inp);
#define LWIP_HOOK_IP4_INPUT(p, inp) rdof_ip4_input_hook((p), (inp))
