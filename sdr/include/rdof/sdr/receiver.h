// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The SDR receive loop for one tuner, all its channels in one process. Wideband IQ chunks in; per channel a demod
 * and a call tracker, whose JSON / raw / frame output goes to the sinks; AFC
 * fed back into the channelizer after each chunk.
 */
#ifndef RDOF_SDR_RECEIVER_H
#define RDOF_SDR_RECEIVER_H

#include <stdbool.h>

#include "rdof/sdr/channelizer.h"
#include "rdof/sdr/demod.h"
#include "rdof/sdr/features.h"
#include "rdof/tracker.h"

#ifdef __cplusplus
extern "C" {
#endif

/* AFC: after each chunk, each channel moves RDOF_AFC_GAIN of the way to where its bursts say it
 * should be, at most RDOF_AFC_STEP_HZ, both for each RDOF_AFC_PERIOD channel samples the chunk held (in proportion
 * for a shorter one), so the correction moves slowly enough inside a burst for the demod and the flywheel to
 * follow at any chunk size. A burst's offset was measured against the correction its samples went through
 * (a chunk or two old: the demod looks ahead), so where the correction should be is that one plus the offset:
 * the last RDOF_AFC_HISTORY corrections are kept per channel to look it up. */
#define RDOF_AFC_GAIN 0.5
#define RDOF_AFC_STEP_HZ 300.0
#define RDOF_AFC_LIMIT_HZ 5000.0
#define RDOF_AFC_PERIOD 4800
#define RDOF_AFC_HISTORY 32
#define RDOF_AFC_AT (54 * 10 + 24 * 10 / 2) /* where in a burst its offset is measured: the middle of the sync */
/* The idle gate: a channel skips its FM stage and sync search for a chunk when that chunk and the one before
 * never rose above RDOF_GATE_LEVEL (rdof_chz_t level: 3 is a signal 3 dB over the noise in the channel filter, 4 or
 * 5 dB under what DMR decodes at; noise alone reads 1 to 2 over a chunk), it isn't in a call, it has had no
 * burst in RDOF_GATE_HOLD channel samples (a keyed repeater's idle bursts keep it open, whatever its level
 * reads), and it isn't in its probe: RDOF_GATE_PROBE samples in every RDOF_GATE_EVERY,
 * staggered over the channels, when it runs regardless, so a signal the level misses is still found and then
 * holds the channel open. It changes no output (the host tests run with it on and off), only
 * the cost of empty channels. */
#define RDOF_GATE_LEVEL 3.0f
#define RDOF_GATE_HOLD 96000  /* 2 s */
#define RDOF_GATE_EVERY 48000 /* 1 s */
#define RDOF_GATE_PROBE 4800  /* 0.1 s */

#if RDOF_FEATURE_SDR_RECEIVER

typedef struct {
    const char *id;        /* the receiver id: the tracker's channel */
    double freq_hz;
    int cc;                /* -1: any */
    int slot;              /* reported for MS / direct syncs */
} rdof_sdr_channel_t;

typedef struct {
    double sample_rate;    /* a multiple of 48 kS/s */
    double center_hz;      /* 0: plan it (rdof_plan_center) */
    double rssi_offset_db;
    double software_ppm;
    bool afc;
    int max_chunk;         /* most wideband samples per rdof_sdr_rx_process call */
} rdof_sdr_rx_config_t;

typedef struct rdof_sdr_rx rdof_sdr_rx_t;
typedef void (*rdof_sdr_burst_cb)(void *ctx, int channel, const rdof_sdr_burst_t *b, double t);

struct rdof_sdr_rx {
    rdof_sdr_rx_config_t cfg;
    double center_hz;
    int C;
    rdof_chz_t chz;
    rdof_demod_t *demods;
    rdof_tracker_t *trackers;
    rdof_cf_t *y;            /* channel output of one chunk: C rows of stride */
    int stride;
    double t0;             /* stream time of sample 0 */
    long long samples, out_index;
    long *bursts;          /* per channel, excluding idle */
    double *last_burst;    /* per channel: stream time of the last non-idle burst, 0 = none */
    int *rssi;             /* per channel: its RSSI (dBm), INT32_MIN = none yet */
    rdof_sdr_burst_cb on_burst;
    void *burst_ctx;
    /* Optional, with burst_ctx: true for a 48 kS/s position the platform's own transmitter keyed (half duplex):
     * a burst there gives no AFC measurement (so no ppm learning either). */
    bool (*muted)(void *ctx, int64_t position);
    double *afc_pending;   /* this chunk's AFC measurements: afc_cap a channel */
    int64_t *afc_at;       /* the channel sample each was measured at */
    int64_t *hist_at;      /* per channel, RDOF_AFC_HISTORY: the first channel sample a correction applied to */
    double *hist_corr;     /* and the correction */
    int *hist_n;
    int *afc_chan;         /* how many each channel has */
    int afc_cap;
    int got;               /* channel samples this chunk */
    bool gate;             /* the idle gate (on after init) */
    bool *was_hot;         /* per channel: the last chunk rose above RDOF_GATE_LEVEL */
    int64_t *last_pos;     /* per channel: the channel sample of its last burst (any kind); 0 at the start */
    long *gated;           /* per channel: chunks skipped */
    long chunks;           /* chunks that gave channel output */
    bool fused;            /* rdof_sdr_rx_forward had no memory for the blocks: it channelized them all itself */
    /* Slot timing utc (rdof_sdr_rx_utc_map): channel sample utc_i0 is UTC utc_t0 ns, utc_rate samples a UTC second */
    bool utc_ok;
    int64_t utc_i0, utc_t0;
    double utc_rate;
    int utc_offset_us;
};

/* Returns 0, or -1 (bad plan, out of memory; call rdof_sdr_rx_free anyway). Each tracker gets sink. */
int rdof_sdr_rx_init(rdof_sdr_rx_t *rx, const rdof_sdr_rx_config_t *cfg, const rdof_sdr_channel_t *chans, int n,
                   const rdof_sink_t *sink, double t0);
void rdof_sdr_rx_free(rdof_sdr_rx_t *rx);
/* The correction (Hz) channel c's sample pos went through: the AFC's history (the oldest kept, if older). */
double rdof_sdr_rx_correction_at(const rdof_sdr_rx_t *rx, int c, int64_t pos);
/* Set channel c's correction from the next chunk on, and keep it in the history. */
void rdof_sdr_rx_set_correction(rdof_sdr_rx_t *rx, int c, double hz);
/* Another sink for one channel's tracker (an HBlink4 peer per channel). False if it has no room. */
bool rdof_sdr_rx_add_sink(rdof_sdr_rx_t *rx, int channel, const rdof_sink_t *sink);
/* Slot timing utc: while ok, each BS / MS burst gets its UTC start and grid slot n,
 * and an MS burst its slot from the grid. Channel sample i0 (48 kS/s) is UTC t0_ns, rate samples a UTC second.
 * Set between chunks, not while rdof_sdr_rx_demod / rdof_sdr_rx_channels run. */
void rdof_sdr_rx_utc_map(rdof_sdr_rx_t *rx, bool ok, int64_t i0, int64_t t0_ns, double rate, int offset_us);
/* One chunk of wideband IQ (n <= max_chunk). */
void rdof_sdr_rx_process(rdof_sdr_rx_t *rx, const rdof_cf_t *iq, int n);

/* The same in three steps, so two cores can share a chunk: channelize, then rdof_sdr_rx_demod over disjoint
 * channel ranges (each in its own task; sinks must then be safe to call from both), then finish (trackers'
 * ticks and AFC, in that order). The result is the same as rdof_sdr_rx_process. */
int rdof_sdr_rx_channelize(rdof_sdr_rx_t *rx, const rdof_cf_t *iq, int n);
void rdof_sdr_rx_demod(rdof_sdr_rx_t *rx, int c0, int c1);
void rdof_sdr_rx_finish(rdof_sdr_rx_t *rx);

/* Or split by channel end to end, so two cores share the channelizer's per-channel part too: forward (the
 * forward FFTs only, on one core), then rdof_sdr_rx_channels over disjoint channel ranges, each a channel's
 * channelizer and demod (worker 0 on one core, 1 on the other), then finish. Same result. */
int rdof_sdr_rx_forward(rdof_sdr_rx_t *rx, const rdof_cf_t *iq, int n);
void rdof_sdr_rx_channels(rdof_sdr_rx_t *rx, int c0, int c1, int worker);

#endif /* RDOF_FEATURE_SDR_RECEIVER */

#ifdef __cplusplus
}
#endif

#endif
