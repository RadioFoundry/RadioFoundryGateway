// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * One channel's call tracker. It
 * turns decoded bursts into what MMDVMHost would publish (call start, late
 * entry, end, lost, CSBK events, BER / RSSI, mode) as JSON on "json", every
 * non-idle burst on "raw", and hands each burst to network sinks with the
 * call's addressing (plus synthesized headers and terminators).
 *
 * JSON text is compact (no spaces after , and :) and matches the golden
 * vectors byte for byte; the golden tests check it.
 */
#ifndef RDOF_TRACKER_H
#define RDOF_TRACKER_H

#include <stdbool.h>
#include <stdint.h>

#include "rdof/fec.h"
#include "rdof/features.h"
#include "rdof/layer2.h"
#include "rdof/rawburst.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_VOICE_LOST_SECONDS 0.5
#define RDOF_DATA_LOST_SECONDS 2.0
#define RDOF_IDLE_HANG_SECONDS 3.0
#define RDOF_METRIC_SECONDS 1.0
#define RDOF_PREAMBLE_REPEAT_SECONDS 1.0
#define RDOF_REPEAT_TERMINATOR_SECONDS 1.0

#define RDOF_JSON_MAX 640
#define RDOF_TRACKER_MAX_SINKS 4

/* One received burst (layer2.Burst; only the fields the tracker reads). */
typedef struct {
    uint8_t burst[RDOF_BURST_BYTES];
    int dt;        /* 0..11, or RDOF_DT_VOICE_SYNC / RDOF_DT_VOICE */
    int n;         /* voice burst index A = 0 .. F = 5 */
    int slot;      /* 1 / 2 */
    rdof_family_t family;
    int cc;        /* -1 if unknown */
    int sync_errors;
    double power_dbfs; /* NAN when the receiver can't measure it */
    bool utc;          /* utc_slot / utc_us set (slot timing utc, the clock on the grid; layer2.Burst's) */
    int64_t utc_slot;  /* UTC grid slot n */
    int64_t utc_us;    /* the burst's start, UTC µs */
} rdof_burst_t;

/* One burst for a network sink, with the addressing a DMRD packet needs. */
typedef struct {
    int slot, dt, n;
    uint8_t burst[RDOF_BURST_BYTES];
    bool has_addr; /* src / dst present */
    uint32_t src, dst;
    bool group, new_stream, synthetic;
    int errors, rssi;
} rdof_frame_t;

typedef struct {
    void (*on_json)(void *ctx, const char *channel, const char *json);
    void (*on_raw)(void *ctx, const char *channel, const char *json);
    void (*on_frame)(void *ctx, const char *channel, const rdof_frame_t *f);
    void *ctx;
    /* A call start / late entry / end / lost (src, dst known at the start only): what the roamer's
     * listen-before-talk needs from its receiver (txvr._HeardCalls). May be NULL. */
    void (*on_call)(void *ctx, const char *channel, const char *action, uint32_t src, uint32_t dst);
    /* Each burst on_raw has, as fields: what the firmware publishes (rdof_rawburst_encode). May be NULL; on_raw's
     * JSON is only built for a sink that has on_raw. */
    void (*on_raw_burst)(void *ctx, const char *channel, const rdof_rawburst_t *r);
} rdof_sink_t;

typedef enum { RDOF_CALL_VOICE, RDOF_CALL_DATA } rdof_call_kind_t;

typedef struct {
    bool active;
    rdof_call_kind_t kind;
    uint32_t src, dst;
    bool group;
    double start, last;
    int cc;
    rdof_family_t family;
    long bits, errs;
    int rssi_n;
    long rssi_sum;
    int rssi_min, rssi_max;
    int blocks_left;
    double metrics_at;
    long m_bits, m_errs;
    bool has_lc;
    uint8_t lc[9];
    bool emergency;
} rdof_call_t;

typedef struct {
    rdof_call_t call;
    uint8_t frags[5][32];
    uint8_t have_frag; /* bit n set: frags[n] holds burst n's fragment (1..4) */
    bool has_preamble;
    uint32_t preamble_src, preamble_dst;
    double preamble_at;
    bool has_ended;
    uint32_t ended_src, ended_dst;
    double ended_at;
} rdof_slot_state_t;

typedef struct {
    char channel[24];
    rdof_sink_t sinks[RDOF_TRACKER_MAX_SINKS];
    int nsinks;
    double rssi_offset;
    double freq_hz; /* 0 = unknown */
    rdof_slot_state_t slots[2];
    int mode; /* 0 = none yet, 1 = DMR, 2 = idle */
    double last_activity;
} rdof_tracker_t;

#if RDOF_FEATURE_TRACKER
void rdof_tracker_init(rdof_tracker_t *tr, const char *channel, double rssi_offset, double freq_hz);
bool rdof_tracker_add_sink(rdof_tracker_t *tr, rdof_sink_t sink);
void rdof_tracker_feed(rdof_tracker_t *tr, const rdof_burst_t *b, double t);
void rdof_tracker_tick(rdof_tracker_t *tr, double t);
#endif /* RDOF_FEATURE_TRACKER */


/* A modem's DMR_DATA control byte and burst, RSSI in dBm (NAN if none). */
void rdof_modem_burst(uint8_t control, const uint8_t burst[RDOF_BURST_BYTES], double dbm, int cc, int slot,
                    rdof_burst_t *out);

/* tracker.iso(): "YYYY-MM-DDTHH:MM:SS.mmmZ" for a Unix time. out needs 25 bytes. */
void rdof_iso_time(double t, char out[32]);
/* The ms since the epoch that timestamp says (rawburst.time_ms(iso(t))), and its text again. */
int64_t rdof_time_ms(double t);
void rdof_iso_ms(int64_t ms, char out[32]);

#ifdef __cplusplus
}
#endif

#endif
