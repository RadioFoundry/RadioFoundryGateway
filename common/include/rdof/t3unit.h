// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * A trunked site, joined up (docs/spec/fw/tier3.md sections 2, 5 and 9): the controller (rdof/t3.h), its radio layer
 * (rdof/t3rf.h) and its part of the MQTT network (rdof/mqttnet.h, rdof/mnctl.h). The platform gives the slot
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
 * The platform publishes online and info itself (rdof_t3unit_trunk_json gives info's trunk object) and keeps the
 * provisioning cache (rdof_mn_cfg_cache on rdof_t3unit_mc, rdof_t3unit_apply after loading it).
 *
 * Rust (rust/src/t3unit.rs, the tier3 feature of rdof_rust, in rdof_common). No I/O, no allocation, no locking: the
 * platform serializes the calls. About 200 KB (the provisioning and the uplinks): allocate it.
 */
#ifndef RDOF_T3UNIT_H
#define RDOF_T3UNIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/features.h"

#include "rdof/mnctl.h"
#include "rdof/mqttnet.h"
#include "rdof/t3.h"
#include "rdof/t3rf.h"

#ifdef __cplusplus
extern "C" {
#endif

#if RDOF_FEATURE_REPEATER

#define RDOF_T3UNIT_RING 16    /* slots of the controller's output held: the most a carrier asks ahead, plus one */
#define RDOF_T3UNIT_STREAMS 16 /* network streams in progress */
#define RDOF_T3UNIT_HOLD 12    /* a network stream's bursts held until it has a channel (720 ms) */
#define RDOF_T3UNIT_TGS 20     /* talkgroups subscribed one by one; more: {root}/tg/+/+, filtered here */
#define RDOF_T3UNIT_FILTERS (10 + RDOF_T3UNIT_TGS)
#define RDOF_T3UNIT_PEERS 32   /* other trunked sites seen in info (the neighbor list) */
#define RDOF_T3UNIT_STATUS_S 10

/* another node as its info and online show it */
typedef struct {
    char node[RDOF_MN_NODE_MAX + 1];
    bool trunk, online;            /* trunk: its info has a trunk object with a valid system */
    uint16_t syscode, chan, site;  /* its C_SYScode, TSCC channel, SITE */
} rdof_t3unit_peer_t;

typedef struct {
    /* An MQTT publish (qos 0 or 1). */
    void (*publish)(void *ctx, const char *topic, const void *data, size_t len, int qos, bool retain);
    /* The topic filters to hold, all of them (those not listed any more dropped). */
    void (*subscribe)(void *ctx, const char *const *filters, int n);
    /* A line for the log (no newline). */
    void (*log)(void *ctx, const char *text);
    /* Provisioning changed: write the cache (rdof_mn_cfg_cache on rdof_t3unit_mc). NULL: none kept. */
    void (*cache)(void *ctx);
    uint32_t (*rand32)(void *ctx);
    void *ctx;
} rdof_t3unit_io_t;

/* counters */
typedef struct {
    uint32_t net_in, net_held, net_dropped, setups_out, setups_in, records_out, records_in, cfg_applied, cfg_bad,
        cmds, tg_filtered;
} rdof_t3unit_counts_t;

/* Opaque (Rust, rust/src/t3unit.rs): rdof_t3unit_size() bytes, aligned to 8 (malloc's), kept by the caller. It
   holds the mqtt-net uplinks and downlink (rdof/mqttnet.h) and the provisioning (rdof/mnctl.h). */
typedef struct rdof_t3unit rdof_t3unit_t;

size_t rdof_t3unit_size(void);

/* site_mem: rdof_t3_site_size() bytes, aligned to 8, kept by the caller. cfg, pol: the settings file's (rdof/t3cfg.h
   rdof_t3cfg_plan). root and node: the MQTT network's (NULL: none, the site alone). path: the trunk's path on
   the site (-1 none). firmware: for status. False with err set (the controller refused the settings). */
bool rdof_t3unit_init(rdof_t3unit_t *u, void *site_mem, const rdof_t3_cfg_t *cfg, const rdof_t3_policy_t *pol,
                         const rdof_t3unit_io_t *io, const char *root, const char *node, int path,
                         const char *firmware, char *err, size_t cap);

/* The controller up to slot k_upto (each slot once, in order; the first call starts at k_upto), now_ms the wall clock
   in ms. Call before asking for a slot: rdof_t3unit_slot needs its k done and held. */
void rdof_t3unit_run(rdof_t3unit_t *u, int64_t k_upto, int64_t now_ms);
/* Carrier c's burst for slot k (rdof_t3rf_slot), the controller run up to k first. */
bool rdof_t3unit_slot(rdof_t3unit_t *u, int c, int64_t k, uint8_t burst[RDOF_BURST_BYTES], uint8_t *control,
                         int *at, int32_t *slc28);
/* A payload carrier on a split TX half: the controller run to k_now + 4, then rdof_t3rf_split_pump (k_now: the UTC
   slot on air now). */
void rdof_t3unit_split_pump(rdof_t3unit_t *u, rdof_t3rf_split_t *o, struct rdof_split *s, int64_t k_now,
                               double now);
/* The settings that run (the settings file's, provisioning over them). */
const rdof_t3_cfg_t *rdof_t3unit_cfg(const rdof_t3unit_t *u);
/* The AT bit of the TSCC carrier's CACH after slot k (before k + 1): 0, 1; -1 not held. */
int rdof_t3unit_at(const rdof_t3unit_t *u, int64_t k);
/* A burst heard on carrier c's inbound under slot k. */
void rdof_t3unit_rx(rdof_t3unit_t *u, int c, int64_t k, const rdof_burst_t *b, int rssi);

/* An MQTT message (any topic the subscriptions bring). */
void rdof_t3unit_message(rdof_t3unit_t *u, const char *topic, const void *data, size_t len);
/* The link came up (after each connect: subscriptions, status, the records again) or went down. */
void rdof_t3unit_link(rdof_t3unit_t *u, bool up);
/* Timers: the network's streams and uplinks, status every RDOF_T3UNIT_STATUS_S. now_ms: the wall clock. */
void rdof_t3unit_tick(rdof_t3unit_t *u, int64_t now_ms);

/* Provisioning (rdof_t3unit_mc) over the settings again, into the controller: after loading the cache. False with the
   reason logged (the controller keeps what it had). */
bool rdof_t3unit_apply(rdof_t3unit_t *u);
/* info's trunk object (rdof_mn_info_t.trunk). */
size_t rdof_t3unit_trunk_json(const rdof_t3unit_t *u, char *out, size_t cap);
/* The status document (node/{node}/status), as published. */
size_t rdof_t3unit_status_json(const rdof_t3unit_t *u, char *out, size_t cap);

/* The wall clock in ms for what follows (rdof_t3unit_slot and rdof_t3unit_split_pump keep the last one given). */
void rdof_t3unit_set_now(rdof_t3unit_t *u, int64_t now_ms);
/* The settings that run (provisioning over the settings file's). */
const rdof_t3_cfg_t *rdof_t3unit_cfg(const rdof_t3unit_t *u);
/* Provisioning as received: for rdof_mn_cfg_cache and rdof_mn_cfg_cache_line (rdof_t3unit_apply after loading). */
rdof_mn_cfg_t *rdof_t3unit_mc(rdof_t3unit_t *u);
const rdof_t3rf_t *rdof_t3unit_rf(const rdof_t3unit_t *u);
const rdof_t3unit_counts_t *rdof_t3unit_counts(const rdof_t3unit_t *u);
/* The other nodes seen in info and online (*n of them). */
const rdof_t3unit_peer_t *rdof_t3unit_peers(const rdof_t3unit_t *u, int *n);
/* The sites in the controller's adjacent-site list. */
int rdof_t3unit_neighbors(const rdof_t3unit_t *u);
/* The cfg/tg documents applied: their talkgroups (*ids, at most RDOF_T3_TG_POLICIES). */
int rdof_t3unit_tg_policies(const rdof_t3unit_t *u, const uint32_t **ids);

/* The structs the Rust shares with C, by name, with their Rust size and alignment (i from 0; NULL name at the end);
   host-tests/test_t3unit.c compares them with this header's and mqttnet.h's, mnctl.h's. */
const rdof_t3_layout_t *rdof_t3unit_layout(size_t i);

#endif /* RDOF_FEATURE_REPEATER */

#ifdef __cplusplus
}
#endif

#endif
