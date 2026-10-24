// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The IP gateway's packets on the air (docs/spec/fw/ipgw.md, section 6): what
 * rdof_ipgw's send_dmr hands over goes out as a network call, through a
 * roamer's transceiver (a simplex hotspot, or an sdr_transceiver half duplex)
 * or a repeater (duplex, SFR or DCDM, or an sdr_transceiver full duplex).
 * Portable: the board's ipgw task and the host tests both use it.
 *
 * A transceiver takes the whole packet at once (its queue holds 256 bursts).
 * A repeater's network input holds RDOF_RPT_IN (64) bursts between polls, and
 * a packet is up to RDOF_IPGW_MAX_BURSTS (128): what doesn't fit waits here
 * and goes in as the repeater takes it (rdof_ipgw_tx_more). One packet at a
 * time: another is refused until it's all in.
 *
 * A repeater refuses a network call on a slot that's busy (an RF call there,
 * another network call; SFR: the inbound heard in the last 0.3 s). A reply
 * comes right after the radio's packet, an ACK at once: it waits here until
 * the slot is free, RDOF_IPGW_TX_WAIT_S at most.
 */
#ifndef RDOF_IPGWTX_H
#define RDOF_IPGWTX_H

#include <stdbool.h>
#include <stdint.h>

#include "rdof/features.h"
#include "rdof/hblink.h"
#include "rdof/ipgw.h"

#ifdef __cplusplus
extern "C" {
#endif

#if RDOF_FEATURE_DATA_BLOCKS && RDOF_FEATURE_TX

#define RDOF_IPGW_TX_WAIT_S 2.0

struct rdof_txvr;
struct rdof_repeater;

typedef struct {
    rdof_ipgw_dmr_t p; /* the packet still going in */
    int next;          /* bursts handed over */
    int slot;
    uint8_t sid[4];
    uint8_t seq;
    bool busy;
    double since;       /* when it was handed over */
    uint32_t gave_up;   /* packets dropped: the slot stayed busy */
} rdof_ipgw_tx_t;

/* Burst i of p as a network burst of stream sid on slot (the last one the terminator). */
void rdof_ipgw_net_burst(const rdof_ipgw_dmr_t *p, int i, const uint8_t sid[4], int slot, uint8_t seq,
                         rdof_net_burst_t *b);

/* To a transceiver: on the roamer's channel the radio was heard on (within 100 Hz), else the one it's on now,
 * with that channel's color code; slot 2. false: no such channel, or the transceiver refused the call (busy with
 * another, not ready). */
bool rdof_ipgw_tx_txvr(rdof_ipgw_tx_t *x, struct rdof_txvr *t, const rdof_ipgw_dmr_t *p, const uint8_t sid[4]);

/* To a repeater: on the slot the radio was heard on (SFR: the repeater sends it on its outbound). false: a packet
 * still going in, or the slot is off. The repeater may still refuse the call when it polls (the slot busy). */
bool rdof_ipgw_tx_repeater(rdof_ipgw_tx_t *x, struct rdof_repeater *r, const rdof_ipgw_dmr_t *p,
                           const uint8_t sid[4]);
/* More of the packet, as the repeater's input has room (and once the slot is free); call it every poll or so
 * (at least every 0.5 s) while x->busy. */
void rdof_ipgw_tx_more(rdof_ipgw_tx_t *x, struct rdof_repeater *r);

#endif

#ifdef __cplusplus
}
#endif

#endif
