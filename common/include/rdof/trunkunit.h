// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * A trunked site, joined up (docs/spec/fw/trunk.md sections 2, 5 and 9): the controller (rdof/t3.h), its radio layer
 * (rdof/trunkrf.h) and its part of the MQTT network (rdof/mqttnet.h, rdof/mnctl.h). The platform gives the slot
 * clock, the bursts heard, the MQTT messages and a way to publish; this does the rest:
 *
 *   per slot, the controller's blocks (held a few slots ahead, so carriers with different leads can ask), the
 *   payload channels opened and closed from its events, the payload PDUs queued;
 *   heard bursts to the radio layer, its PDUs and voice activity to the controller, regenerated voice up to the
 *   network (a carrier's mqtt-net uplink, its two slots its two LCNs) when the controller says the call is wide;
 *   call messages down (group streams for the talkgroups the site's MSs need, private streams on dl) to the
 *   controller and, once it gives a channel, the channel's outbound;
 *   setup messages both ways, registration records written and read back, where/{id}, provisioning (cfg/...) over
 *   the settings, commands and their results, status every 10 s, the activity events.
 *
 * The platform publishes online and info itself (rdof_trunkunit_trunk_json gives info's trunk object) and keeps the
 * provisioning cache (rdof_mn_cfg_cache on u->mc, rdof_trunkunit_apply after loading it).
 *
 * Needs the controller (rust's t3 feature): not part of rdof_common; rdofradio and its test build it beside
 * rdof_rust_t3. No I/O, no allocation, no locking: the platform serializes the calls. About 200 KB (the provisioning
 * and the uplinks): allocate it.
 */
#ifndef RDOF_TRUNKUNIT_H
#define RDOF_TRUNKUNIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/features.h"

#include "rdof/mnctl.h"
#include "rdof/mqttnet.h"
#include "rdof/t3.h"
#include "rdof/trunkrf.h"

#ifdef __cplusplus
extern "C" {
#endif

#if RDOF_FEATURE_REPEATER

#define RDOF_TRUNKUNIT_RING 16    /* slots of the controller's output held: the most a carrier asks ahead, plus one */
#define RDOF_TRUNKUNIT_STREAMS 16 /* network streams in progress */
#define RDOF_TRUNKUNIT_HOLD 12    /* a network stream's bursts held until it has a channel (720 ms) */
#define RDOF_TRUNKUNIT_TGS 20     /* talkgroups subscribed one by one; more: {root}/tg/+/+, filtered here */
#define RDOF_TRUNKUNIT_FILTERS (10 + RDOF_TRUNKUNIT_TGS)
#define RDOF_TRUNKUNIT_PEERS 32   /* other trunked sites seen in info (the neighbor list) */
#define RDOF_TRUNKUNIT_STATUS_S 10

/* another node as its info and online show it */
typedef struct {
    char node[RDOF_MN_NODE_MAX + 1];
    bool trunk, online;            /* trunk: its info has a trunk object with a valid system */
    uint16_t syscode, chan, site;  /* its C_SYScode, TSCC channel, SITE */
} rdof_trunkunit_peer_t;

typedef struct {
    /* An MQTT publish (qos 0 or 1). */
    void (*publish)(void *ctx, const char *topic, const void *data, size_t len, int qos, bool retain);
    /* The topic filters to hold, all of them (those not listed any more dropped). */
    void (*subscribe)(void *ctx, const char *const *filters, int n);
    /* A line for the log (no newline). */
    void (*log)(void *ctx, const char *text);
    /* Provisioning changed: write the cache (rdof_mn_cfg_cache on u->mc). NULL: none kept. */
    void (*cache)(void *ctx);
    uint32_t (*rand32)(void *ctx);
    void *ctx;
} rdof_trunkunit_io_t;

typedef struct {
    bool used, mapped, group, ended;
    uint32_t stream, src, dst;
    int ch;
    int64_t last_ms;
    rdof_frame_t hold[RDOF_TRUNKUNIT_HOLD];
    int nhold;
} rdof_trunkunit_stream_t;

typedef struct {
    int64_t k; /* -1: none */
    rdof_t3_slot_out_t o;
} rdof_trunkunit_held_t;

struct rdof_trunkunit;
typedef struct {
    struct rdof_trunkunit *u;
    int c;
} rdof_trunkunit_upctx_t;

typedef struct rdof_trunkunit {
    rdof_trunkunit_io_t io;
    bool net;                       /* on the MQTT network (a root and node given) */
    char root[32], node[RDOF_MN_NODE_MAX + 1], firmware[48];
    int path;                       /* the trunk's path on the site (events, the uplink's path byte); -1 none */
    rdof_t3_site_t *site;
    rdof_t3_cfg_t base, cfg;        /* the settings file's; what runs (provisioning over it) */
    rdof_t3_policy_t base_pol, pol;
    bool withdrawn_cfg;             /* cfg/node's enabled false */
    rdof_trunkrf_t rf;
    rdof_mn_cfg_t mc;               /* provisioning as received */
    uint32_t ms_ids[RDOF_T3_MS_POLICIES];
    int nms;
    uint32_t tg_ids[RDOF_T3_TG_POLICIES]; /* cfg/tg documents applied */
    int ntgp;
    rdof_trunkunit_peer_t peers[RDOF_TRUNKUNIT_PEERS]; /* the other nodes' trunk info and presence */
    int npeers;
    int nneighbors;                 /* in the controller's adjacent-site list */
    rdof_mn_up_t up[RDOF_T3_CARRIERS];
    rdof_trunkunit_upctx_t upctx[RDOF_T3_CARRIERS];
    rdof_mn_down_t down;
    rdof_trunkunit_held_t ring[RDOF_TRUNKUNIT_RING];
    int64_t k_next;                 /* the next slot for the controller; -1 before the first */
    uint8_t ch_flags[RDOF_T3_CHANNELS]; /* RDOF_T3_F_* of the call on each channel (0: none) */
    rdof_trunkunit_stream_t st[RDOF_TRUNKUNIT_STREAMS];
    uint32_t tgs[RDOF_TRUNKUNIT_TGS];
    int ntgs;
    bool tg_all;                    /* subscribed {root}/tg/+/+ */
    bool connected;
    int64_t now_ms, start_ms, status_ms;
    /* counters */
    uint32_t net_in, net_held, net_dropped, setups_out, setups_in, records_out, records_in, cfg_applied, cfg_bad,
        cmds, tg_filtered;
} rdof_trunkunit_t;

/* site_mem: rdof_t3_site_size() bytes, aligned to 8, kept by the caller. cfg, pol: the settings file's (rdof/trunkcfg.h
   rdof_trunk_cfg_plan). root and node: the MQTT network's (NULL: none, the site alone). path: the trunk's path on
   the site (-1 none). firmware: for status. False with err set (the controller refused the settings). */
bool rdof_trunkunit_init(rdof_trunkunit_t *u, void *site_mem, const rdof_t3_cfg_t *cfg, const rdof_t3_policy_t *pol,
                         const rdof_trunkunit_io_t *io, const char *root, const char *node, int path,
                         const char *firmware, char *err, size_t cap);

/* The controller up to slot k_upto (each slot once, in order; the first call starts at k_upto), now_ms the wall clock
   in ms. Call before asking for a slot: rdof_trunkunit_slot needs its k done and held. */
void rdof_trunkunit_run(rdof_trunkunit_t *u, int64_t k_upto, int64_t now_ms);
/* Carrier c's burst for slot k (rdof_trunkrf_slot), the controller run up to k first. */
bool rdof_trunkunit_slot(rdof_trunkunit_t *u, int c, int64_t k, uint8_t burst[RDOF_BURST_BYTES], uint8_t *control,
                         int *at, int32_t *slc28);
/* A payload carrier on a split TX half: the controller run to k_now + 4, then rdof_trunkrf_split_pump (k_now: the UTC
   slot on air now). */
void rdof_trunkunit_split_pump(rdof_trunkunit_t *u, rdof_trunkrf_split_t *o, struct rdof_split *s, int64_t k_now,
                               double now);
/* The AT bit of the TSCC carrier's CACH after slot k (before k + 1): 0, 1; -1 not held. */
int rdof_trunkunit_at(const rdof_trunkunit_t *u, int64_t k);
/* A burst heard on carrier c's inbound under slot k. */
void rdof_trunkunit_rx(rdof_trunkunit_t *u, int c, int64_t k, const rdof_burst_t *b, int rssi);

/* An MQTT message (any topic the subscriptions bring). */
void rdof_trunkunit_message(rdof_trunkunit_t *u, const char *topic, const void *data, size_t len);
/* The link came up (after each connect: subscriptions, status, the records again) or went down. */
void rdof_trunkunit_link(rdof_trunkunit_t *u, bool up);
/* Timers: the network's streams and uplinks, status every RDOF_TRUNKUNIT_STATUS_S. now_ms: the wall clock. */
void rdof_trunkunit_tick(rdof_trunkunit_t *u, int64_t now_ms);

/* Provisioning (u->mc) over the settings again, into the controller: after loading the cache. False with the
   reason logged (the controller keeps what it had). */
bool rdof_trunkunit_apply(rdof_trunkunit_t *u);
/* info's trunk object (rdof_mn_info_t.trunk). */
size_t rdof_trunkunit_trunk_json(const rdof_trunkunit_t *u, char *out, size_t cap);
/* The status document (node/{node}/status), as published. */
size_t rdof_trunkunit_status_json(const rdof_trunkunit_t *u, char *out, size_t cap);

#endif /* RDOF_FEATURE_REPEATER */

#ifdef __cplusplus
}
#endif

#endif
