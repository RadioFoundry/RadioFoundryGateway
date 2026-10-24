// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The jitter buffer for network voice going out over the air (the roamer's transceiver and the repeater).
 *
 * Bursts come from HBlink4 one per 60 ms per stream, late by however much the network holds them. A stream is held
 * for a prebuffer of bursts before it goes out, so a late burst is still in hand when its turn comes. Fixed, that
 * prebuffer is a setting. Adaptive, it is sized from what the network did: each stream's arrivals are measured
 * against the 60 ms cadence, and what a stream needed is remembered per talkgroup and slot (and for the link as a
 * whole) for the next call. Mid-call the buffer grows on an underrun (the silence filled in moves the stream back a
 * burst instead of costing a late burst) and shrinks slowly when the stream has been ahead of its target for a
 * while: silence first, a voice burst only after RDOF_JB_SHRINK_S. docs/spec/fw/roamer.md, "Jitter buffer".
 *
 * Sizes are in bursts (60 ms each). Nothing here blocks; the caller holds its lock.
 */
#ifndef RDOF_JITTER_H
#define RDOF_JITTER_H

#include <stdbool.h>
#include <stdint.h>

#include "rdof/features.h"
#include "rdof/fec.h"
#include "rdof/json.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_JB_BURST_MS 60
#define RDOF_JB_WINDOW 64      /* arrivals the estimate looks back over (3.84 s) */
#define RDOF_JB_KEYS 8         /* talkgroups and slots remembered */
#define RDOF_JB_LEAD 2         /* bursts the radio needs in hand besides the jitter */
#define RDOF_JB_MARGIN_MS 10   /* added to the lateness before rounding up to bursts */
#define RDOF_JB_UNKNOWN 6      /* the prebuffer before anything is known about the link (360 ms) */
#define RDOF_JB_SAMPLES 16     /* arrivals before a stream's own estimate sets its target */
#define RDOF_JB_REMEMBER 6     /* arrivals a stream needs to be remembered */
#define RDOF_JB_SHRINK_S 2.0   /* ahead of target this long: one voice burst dropped */
#define RDOF_JB_MAX_MS 3000    /* the most a setting may ask for */
#define RDOF_JB_MIN_MS 180     /* the defaults for auto */
#define RDOF_JB_MAX_DEFAULT_MS 480

/* Milliseconds to whole bursts, rounded up (at most RDOF_JB_MAX_MS). */
int rdof_jb_bursts(double ms);

#if RDOF_FEATURE_TX

/* One talkgroup and slot's remembered need. */
typedef struct {
    uint32_t used; /* 0: free; else when last used (rdof_jb_t.stamp) */
    uint32_t dst;
    int slot;
    bool group;
    double late_ms;
} rdof_jb_key_t;

/* A transceiver's or repeater's jitter buffer: its settings, its memory and what it reports. */
typedef struct {
    bool adaptive;    /* false: the prebuffer is the caller's fixed count, and nothing adapts */
    int min, max;     /* adaptive: the prebuffer's bounds, in bursts */
    rdof_jb_key_t keys[RDOF_JB_KEYS];
    uint32_t stamp;
    bool has_link;
    double link_ms;   /* the last calls' need on any key */
    /* the stream going out now, or the last one */
    int target;       /* bursts */
    double jitter_ms; /* RFC 3550 interarrival jitter */
    double late_ms;   /* the window's second worst lateness */
    uint32_t streams, underruns, grown, shrunk;
} rdof_jb_t;

/* One stream's measure. */
typedef struct {
    bool keyed;       /* its talkgroup and slot are known */
    uint32_t dst;
    int slot;
    bool group;
    bool measuring;   /* t0 and seq set */
    double t0;        /* the first measured arrival */
    int idx;          /* the last arrival's burst number from t0 */
    int seq;          /* its DMRD sequence number */
    double last_transit;
    int16_t transit[RDOF_JB_WINDOW]; /* ms: arrival - t0 - idx * 60, the last RDOF_JB_WINDOW */
    int nwin, wnext;
    int n;            /* arrivals measured */
    double jitter_ms, late_ms, peak_ms;
    int start;        /* the prebuffer it started with, bursts */
    int target;       /* bursts, now */
    int shift;        /* bursts grown less bursts shrunk */
    bool above;       /* ahead of its target since above_since */
    double above_since;
} rdof_jb_stream_t;

/* Fixed (adaptive false), bounds 180 and 480 ms. */
void rdof_jb_init(rdof_jb_t *jb);
/* A stream starts (its talkgroup not known yet): its target from the link's memory. */
void rdof_jb_begin(rdof_jb_t *jb, rdof_jb_stream_t *s);
/* Its talkgroup and slot, from its first burst: the target from their memory, if it hasn't started (started false). */
void rdof_jb_key(rdof_jb_t *jb, rdof_jb_stream_t *s, uint32_t dst, bool group, int slot, bool started);
/* A burst arrived at `at` (seconds) with DMRD sequence number seq (0-255; the same as the last one: one burst on). */
void rdof_jb_arrival(rdof_jb_t *jb, rdof_jb_stream_t *s, double at, int seq);
/* The prebuffer for this stream: its target when adaptive, else fixed. */
int rdof_jb_prebuffer(const rdof_jb_t *jb, const rdof_jb_stream_t *s, int fixed);
/* An underrun: true if the silence filled in may move the stream back a burst (adaptive, under the maximum); then
 * it counts as grown. Either way it counts as an underrun. */
bool rdof_jb_underrun(rdof_jb_t *jb, rdof_jb_stream_t *s);
/* Of a gap of `gap` lost bursts, how many silences to fill in with `ahead` bursts in hand (the radio's and the
 * queue's): all when fixed, else only up to the target. The rest count as shrunk. */
int rdof_jb_fill(rdof_jb_t *jb, rdof_jb_stream_t *s, int gap, double ahead);
/* A voice burst with `ahead` bursts in hand before it: true to drop it (adaptive only): silence while ahead of the
 * target, any voice once ahead for RDOF_JB_SHRINK_S. A drop counts as shrunk. */
bool rdof_jb_drop(rdof_jb_t *jb, rdof_jb_stream_t *s, double ahead, bool silent, double now);
/* The stream is over: what it needed is remembered. */
void rdof_jb_end(rdof_jb_t *jb, rdof_jb_stream_t *s);

/* A voice burst (in, as regenerated) moved to position n (0 = A ... 5 = F): its AMBE kept, and the sync (A) or the EMB
 * for cc, pi and the position's LCSS and the 32-bit embedded field (one bit a byte; NULL: zeros). bs: BS voice sync,
 * else MS. Returns its control byte. */
uint8_t rdof_jb_reframe(const uint8_t in[RDOF_BURST_BYTES], int n, int cc, bool bs, int pi, const uint8_t *embedded32,
                        uint8_t out[RDOF_BURST_BYTES]);
/* Its three AMBE frames are the silence frame. */
bool rdof_jb_silent(const uint8_t burst[RDOF_BURST_BYTES]);

/* {"mode":"auto"|"fixed","target_ms","jitter_ms","late_ms","min_ms","max_ms","streams","underruns","grown","shrunk"}
 * under key; fixed: the caller's fixed prebuffer as target_ms. */
void rdof_jb_json(const rdof_jb_t *jb, int fixed, rdof_json_t *j, const char *key);

#endif /* RDOF_FEATURE_TX */

#ifdef __cplusplus
}
#endif

#endif
