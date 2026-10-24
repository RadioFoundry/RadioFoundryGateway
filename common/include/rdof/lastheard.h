// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Last heard: a ring of the most recent calls, for the web page. Each call is
 * RF (heard on the air by an SDR channel, a roamer's radio or a repeater) or
 * NET (from the network, sent out over RF). RF calls come from a call
 * tracker's JSON (rdof_lh_tracker_json), NET calls from a transceiver's or
 * repeater's on_tx (rdof_lh_tx). The caller locks: nothing here is thread safe.
 */
#ifndef RDOF_LASTHEARD_H
#define RDOF_LASTHEARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/jsonp.h"
#include "rdof/netcalls.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_LH_DEFAULT 50

typedef enum { RDOF_LH_RF = 0, RDOF_LH_NET = 1 } rdof_lh_dir_t;
typedef enum { RDOF_LH_VOICE = 0, RDOF_LH_DATA, RDOF_LH_CSBK } rdof_lh_kind_t;

typedef struct {
    uint32_t id;  /* the ring's seq when the call started */
    uint32_t seq; /* the ring's seq when it last changed */
    rdof_lh_dir_t dir;
    rdof_lh_kind_t kind;
    char role[10];    /* "sdr", "roamer", "repeater" */
    char radio[24];   /* the radio or SDR that heard or sent it */
    char channel[24]; /* an SDR's channel id; "" for a radio */
    int slot;
    int cc;        /* -1: not known */
    uint32_t freq; /* Hz; 0: not known */
    uint32_t src, dst;
    bool group, active, late, emergency, encrypted;
    double start;     /* monotonic seconds */
    int64_t start_ms; /* wall clock, ms since the epoch; 0: not known */
    double duration;  /* seconds, once ended */
    char end[8];      /* "end" (terminator), "lost" (timed out); "" while active */
    bool has_ber, has_rssi;
    float ber; /* % */
    int16_t rssi, rssi_min, rssi_max; /* dBm */
    int frames; /* a data call's blocks; -1 */
    char desc[24]; /* a CSBK's description */
} rdof_lh_entry_t;

typedef struct {
    rdof_lh_entry_t *e;
    int cap, n, head; /* head: the next one written */
    uint32_t seq;
    rdof_jnode_t nodes[40]; /* the tracker JSON's parse */
    char sbuf[320];
} rdof_lh_t;

void rdof_lh_init(rdof_lh_t *lh, rdof_lh_entry_t *buf, int cap);
/* A tracker's JSON line (call start, late entry, end, lost, CSBK, BER, RSSI); others are ignored. role and radio
 * name who heard it, channel the SDR channel ("" or NULL for a radio). now: monotonic s; now_ms: wall clock ms
 * (0 unknown). True if the ring changed. */
bool rdof_lh_tracker_json(rdof_lh_t *lh, const char *role, const char *radio, const char *channel, const char *json,
                          double now, int64_t now_ms);
/* A PI header heard on a slot: the call there is encrypted. */
bool rdof_lh_privacy(rdof_lh_t *lh, const char *radio, const char *channel, int slot);
/* A NET call going out (action "start" or "end", as on_tx gives them). */
bool rdof_lh_tx(rdof_lh_t *lh, const char *role, const char *radio, const char *action,
                const rdof_tx_call_info_t *call, double duration, double now, int64_t now_ms);
/* The calls that changed after seq `since`, newest first, as JSON:
 * {"seq":N,"now_ms":T,"max":50,"calls":[{...}]}. Entries that don't fit are left out ("more":true).
 * Returns the length (out NUL terminated). */
size_t rdof_lh_json(const rdof_lh_t *lh, uint32_t since, double now, int64_t now_ms, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
