// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The router (rdof/router.h) on a running box: what the platform's receivers hear, asked at each stream's start
 * (the HBlink sink's and the parrot's admit), and the network's bursts, handed to the transmitters the router
 * picks. The platform registers its transmitters: a repeater for each repeater app, a transceiver (rdof/txvr.h)
 * for the simplex frequencies it may transmit on.
 *
 *   A network stream to a repeater's path goes to that repeater, on the path's slot; an emergency that takes the
 *   slot from another network stream ends that one (rdof_repeater_preempt).
 *   A network stream to a simplex path tunes a transceiver that may transmit on its frequency (as a DMRT would:
 *   the frequency's cc and power), then sends its bursts on the frequency's slot. A transceiver sends one stream
 *   at a time; an emergency takes it from another (the tune's preempt). A path no transceiver is free for is let
 *   go (rdof_router_drop).
 *
 * A transceiver's answers (DMRK) to a tune made here go nowhere: the master sent no DMRT (rdof_siteroute_tuned).
 * Portable C, no allocation, no locking: call it from one thread.
 */
#ifndef RDOF_SITEROUTE_H
#define RDOF_SITEROUTE_H

#include <stdbool.h>
#include <stdint.h>

#include "rdof/hblink.h"
#include "rdof/router.h"
#include "rdof/site.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_SITEROUTE_TXVRS 4
#define RDOF_SITEROUTE_TUNED 16

struct rdof_repeater;
struct rdof_txvr;

typedef struct {
    struct rdof_txvr *t;
    uint64_t freqs;  /* the frequencies it may transmit on (bit per frequency) */
    uint32_t stream; /* the network stream it's sending, 0: none */
    int path;        /* on that path */
    double last;     /* that stream's last burst */
} rdof_siteroute_txvr_t;

typedef struct {
    rdof_router_t router;
    struct rdof_repeater *rpt[RDOF_SITE_MAX_APPS]; /* by app: a repeater app's */
    rdof_siteroute_txvr_t tx[RDOF_SITEROUTE_TXVRS];
    int ntx;
    uint32_t tuned[RDOF_SITEROUTE_TUNED]; /* the streams tuned here, newest last */
    int tuned_next;
    uint32_t stream_emergency; /* the last stream whose header said emergency */
    const char *why;           /* the last RF call's: why it wasn't sent (rdof_router_rf) */
    unsigned no_txvr;          /* network streams with no transceiver free for a path */
} rdof_siteroute_t;

void rdof_siteroute_init(rdof_siteroute_t *sr, const rdof_site_t *site);
/* Registered transmitters. A repeater for app (a repeater app); a transceiver for freqs (bit per frequency; 0: every
   frequency it could, the device's transmits). False: no room, or not such an app. */
bool rdof_siteroute_add_repeater(rdof_siteroute_t *sr, int app, struct rdof_repeater *r);
bool rdof_siteroute_add_txvr(rdof_siteroute_t *sr, struct rdof_txvr *t, uint64_t freqs);

/* A stream heard at hz on slot, at its start: true when it goes to the network (rdof_router_rf). */
bool rdof_siteroute_rf(rdof_siteroute_t *sr, uint32_t hz, int slot, uint32_t src, uint32_t dst, bool group, double now);
/* Whether a call heard at hz on slot may reach app (the path's apps). */
bool rdof_siteroute_reaches(const rdof_siteroute_t *sr, uint32_t hz, int slot, int app);

/* A burst from network net: to the transmitters the router picks. Returns the paths it went out on. */
uint64_t rdof_siteroute_burst(rdof_siteroute_t *sr, int net, const rdof_net_burst_t *b, double now);
/* A DMRD packet from network net (other packets: false, the caller's). */
bool rdof_siteroute_dmrd(rdof_siteroute_t *sr, int net, const uint8_t *d, size_t n, double now);

/* Whether stream sid was tuned here (the transceiver's DMRK for it goes nowhere). */
bool rdof_siteroute_tuned(const rdof_siteroute_t *sr, const uint8_t sid[4]);

#ifdef __cplusplus
}
#endif

#endif
