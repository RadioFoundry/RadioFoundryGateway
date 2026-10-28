// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The call router: where a call goes, from the talk paths' policies (rdof/site.h, docs/spec/fw/config-v3.md
 * section 9). No I/O: the platform asks, then sends.
 *
 *   RF to network (rdof_router_rf): a call heard on a path goes to an app that claims its ID (when the path may
 *   reach it), else to the path's network when its tg, ptt (which starts or renews the hold) or pc says so and dir
 *   allows, else nowhere (not routed). Every call heard sets where its caller was last heard.
 *
 *   Network to RF (rdof_router_net, at each burst of a stream): a group call goes to every path on that network
 *   that carries its talkgroup (tg, or a held ptt) and may transmit; a private call to the path where the called
 *   radio was last heard (within pc_hold_s), when that path's pc holds the caller. A path two calls want: an
 *   emergency beats a listed talkgroup, which beats one matched only by "*"; between equals the call already there
 *   keeps it. The decision is made at a stream's first burst and kept for the stream.
 *
 * Portable C, no allocation. Times are seconds on any steady clock.
 */
#ifndef RDOF_ROUTER_H
#define RDOF_ROUTER_H

#include <stdbool.h>
#include <stdint.h>

#include "rdof/site.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_ROUTER_PTT 4         /* talkgroups held by PTT at once, a path */
#define RDOF_ROUTER_HEARD 256     /* radios in the last-heard table */
#define RDOF_ROUTER_IDLE_S 1.5    /* a path's network call ends this long after its last burst */

typedef struct {
    uint32_t src, dst;
    bool group;
    bool emergency;
} rdof_rcall_t;

typedef enum { RDOF_ROUTE_NONE = 0, RDOF_ROUTE_APP, RDOF_ROUTE_NET } rdof_route_kind_t;

typedef struct {
    uint8_t kind; /* rdof_route_kind_t */
    int app;      /* RDOF_ROUTE_APP: the app */
    int net;      /* RDOF_ROUTE_NET: the network */
    const char *why; /* RDOF_ROUTE_NONE: why not (for the log and last heard) */
} rdof_route_t;

/* Ranks of a network call on a path. */
enum { RDOF_RANK_STAR = 1, RDOF_RANK_LISTED = 2, RDOF_RANK_EMERGENCY = 3 };

typedef struct {
    const rdof_site_t *site;
    struct {
        uint32_t tg;
        double until;
    } ptt[RDOF_SITE_MAX_PATHS][RDOF_ROUTER_PTT];
    struct {
        uint32_t id;
        int16_t path;
        double at;
    } heard[RDOF_ROUTER_HEARD];
    struct {
        uint32_t stream; /* 0: free */
        uint8_t rank;
        double last;
    } busy[RDOF_SITE_MAX_PATHS];
    uint32_t unplaced[8]; /* streams that went nowhere at their first burst (their later bursts don't count again) */
    int unplaced_next;
    /* counters */
    unsigned to_net, to_app, not_routed; /* RF calls */
    unsigned from_net, refused, pc_dropped, no_path; /* network streams */
} rdof_router_t;

void rdof_router_init(rdof_router_t *r, const rdof_site_t *site);

/* A call heard on path (at its first burst). */
rdof_route_t rdof_router_rf(rdof_router_t *r, int path, const rdof_rcall_t *c, double now);

/* A burst of stream from network net: the paths it goes out on (bit per path). The first burst decides; later
   ones keep the decision and keep the paths busy; end (the terminator) frees them. *preempt (optional): the paths
   taken from another stream (the transmitter should drop that one). only: the paths it may go to (identity
   per_path: the peer's that it came to), ~0 any. A private call sent to a peer goes to its path whether or not
   the called radio was heard here: the master chose it. */
uint64_t rdof_router_net(rdof_router_t *r, int net, uint64_t only, uint32_t stream, const rdof_rcall_t *c, bool end,
                         double now, uint64_t *preempt);
/* The platform couldn't send on path (no transmitter free): let the path go. */
void rdof_router_drop(rdof_router_t *r, int path, uint32_t stream);

/* Whether path carries talkgroup tg now (tg, or a held ptt), and how: 0 no, else a rank. */
int rdof_router_carries(const rdof_router_t *r, int path, uint32_t tg, double now);
/* Where radio id was last heard (within the site's pc_hold_s): a path, or -1. */
int rdof_router_heard(const rdof_router_t *r, uint32_t id, double now);

#ifdef __cplusplus
}
#endif

#endif
