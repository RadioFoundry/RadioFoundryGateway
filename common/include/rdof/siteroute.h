// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The router (rdof/router.h) on a running box: what the platform's receivers hear, asked at each stream's start
 * (the HBlink sink's and the parrot's admit), and the network's bursts, handed to the transmitters the router
 * picks. The platform registers its transmitters: a repeater for each repeater app, a transceiver (rdof/txvr.h)
 * for the simplex frequencies it may transmit on.
 *
 *   A network stream to a repeater's path (or an SFR or DCDM frequency's) goes to that repeater, on the path's slot; an emergency that takes the
 *   slot from another network stream ends that one (rdof_repeater_preempt).
 *   A network stream to a simplex path tunes a transceiver that may transmit on its frequency (as a DMRT would:
 *   the frequency's cc, and its power_dbm as the transceiver's RF level), then sends its bursts on the frequency's slot. The pool (rdof/txpool.h) picks
 *   it: a transceiver sends one stream at a time, then holds the path for its answer_s; a network stream may take
 *   a hold, and an emergency takes one from a stream that isn't (the tune's preempt; the router lets the other
 *   path go). A path no transceiver is free for is let go (rdof_router_drop).
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
#include "rdof/txpool.h"
#include "rdof/txpower.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_SITEROUTE_TXVRS RDOF_TXPOOL_MAX
#define RDOF_SITEROUTE_TUNED 16
#define RDOF_SITEROUTE_MEMO 8 /* RF streams decided lately: another sink asking about one gets the same answer */

struct rdof_repeater;
struct rdof_txvr;


typedef struct {
    uint32_t hz, src, dst;
    int8_t slot, net;
    double at;
} rdof_siteroute_memo_t;

typedef struct {
    rdof_router_t router;
    struct rdof_repeater *rpt[RDOF_SITE_MAX_APPS]; /* by app: a repeater app's */
    struct rdof_repeater *rpt_freq[RDOF_SITE_MAX_FREQS]; /* by frequency: an SFR or DCDM frequency's */
    struct rdof_txvr *tx[RDOF_SITEROUTE_TXVRS]; /* by the pool's index */
    rdof_txmap_t txmap[RDOF_SITEROUTE_TXVRS];   /* each one's RF levels in dBm (none: it can't tell dBm) */
    rdof_txpool_t pool;                         /* which one sends what (rdof/txpool.h) */
    uint32_t tuned[RDOF_SITEROUTE_TUNED]; /* the streams tuned here, newest last */
    int tuned_next;
    uint32_t stream_emergency; /* the last stream whose header said emergency */
    const char *why;           /* the last RF call's: why it wasn't sent (rdof_router_rf) */
    rdof_siteroute_memo_t memo[RDOF_SITEROUTE_MEMO];
    int memo_next;
    unsigned no_txvr;          /* network streams with no transceiver free for a path */
    unsigned app_no_txvr;      /* apps' bursts with none free */
} rdof_siteroute_t;

void rdof_siteroute_init(rdof_siteroute_t *sr, const rdof_site_t *site);
/* Registered transmitters. A repeater for app (a repeater app); a transceiver for freqs (bit per frequency; 0: every
   frequency it could, the device's transmits); map: its RF levels in dBm (NULL: none, rdof/txpower.h). False: no
   room, or not such an app. */
bool rdof_siteroute_add_repeater(rdof_siteroute_t *sr, int app, struct rdof_repeater *r);
/* The repeater running an SFR or DCDM frequency (its paths' bursts go to it, on their slot). */
bool rdof_siteroute_add_repeater_freq(rdof_siteroute_t *sr, int freq, struct rdof_repeater *r);
bool rdof_siteroute_add_txvr(rdof_siteroute_t *sr, struct rdof_txvr *t, uint64_t freqs, const rdof_txmap_t *map);

/* A stream heard at hz on slot, at its start: the network it goes to (rdof_router_rf), -1 none (why says why, or
   it went to an app). Each receiver's sinks (HBlink, MQTT) ask in turn: the same stream asked again within a second
   gets the first answer, and the router counts it once. */
int rdof_siteroute_rf(rdof_siteroute_t *sr, uint32_t hz, int slot, uint32_t src, uint32_t dst, bool group, double now);
/* Whether a call heard at hz on slot may reach app (the path's apps). */
bool rdof_siteroute_reaches(const rdof_siteroute_t *sr, uint32_t hz, int slot, int app);

/* A burst from network net, sent to peer (identity per_path: only that peer's paths take it; 0 any): to the
   transmitters the router picks. Returns the paths it went out on. */
uint64_t rdof_siteroute_burst(rdof_siteroute_t *sr, int net, uint32_t peer, const rdof_net_burst_t *b, double now);
/* A DMRD packet from network net to peer (other packets: false, the caller's). */
bool rdof_siteroute_dmrd(rdof_siteroute_t *sr, int net, uint32_t peer, const uint8_t *d, size_t n, double now);

/* Apps on the box (the parrot, the IP gateway; docs/spec/fw/transmitters.md §5) send as a network does, through
   the pool, on a path: the one a call was heard on (rdof_siteroute_path_at), or where a radio was last heard.
   The path heard at hz on slot, -1 none. */
int rdof_siteroute_path_at(const rdof_siteroute_t *sr, uint32_t hz, int slot);
/* Whether a transmitter would take an app's new stream on path now (the parrot waits while not). */
bool rdof_siteroute_app_ready(const rdof_siteroute_t *sr, int path, double now);
/* A burst of an app's stream on path: the pool's transceiver, tuned at the stream's first burst (the path's
   frequency, cc and power_dbm), the burst on the path's slot. take_hold: may take a transceiver holding another
   path (a confirmed-data answer). cap_dbm: the stream's power at most this, dBm at the antenna (a bench job's;
   INFINITY: the frequency's own). A transceiver without a map (rdof/txpower.h) sends under a cap at its least.
   False: none free (app_no_txvr). */
bool rdof_siteroute_app_burst(rdof_siteroute_t *sr, int path, const rdof_net_burst_t *b, bool take_hold,
                              double cap_dbm, double now);
/* Whether a transceiver still holds a burst of the stream sid (rdof_txvr_holds): an app's stream not all on the air. */
bool rdof_siteroute_holds(const rdof_siteroute_t *sr, const uint8_t sid[4]);

/* MQTT calls (docs/spec/fw/mqtt.md): a DMRD packet goes out on {root}/calls/{tg|pc}/{dst}/{box}; a box takes
   {root}/calls/# but its own. The topic for a DMRD (0: not one, or no room). */
size_t rdof_siteroute_calls_topic(const char *root, const char *box, const uint8_t *dmrd, size_t n, char *out,
                                  size_t cap);
/* Whether topic is a call another box sent (not box's, and the shape above). */
bool rdof_siteroute_calls_theirs(const char *topic, const char *root, const char *box);

/* Whether stream sid was tuned here (the transceiver's DMRK for it goes nowhere). */
bool rdof_siteroute_tuned(const rdof_siteroute_t *sr, const uint8_t sid[4]);

#ifdef __cplusplus
}
#endif

#endif
