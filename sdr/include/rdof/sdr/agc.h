// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Front-end level: clipping measurement and a slow, bounded gain control.
 * Pure logic, no device: feed
 * it each chunk's measurement and it says when to change the gain.
 */
#ifndef RDOF_SDR_AGC_H
#define RDOF_SDR_AGC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/sdr/features.h"
#include "rdof/sdr/fft.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_AGC_FULL_SCALE 0.99f   /* compared with float samples, as numpy compares float32 */
#define RDOF_AGC_FULL_SCALE_D 0.99  /* in the gain law (doubles) */
#define RDOF_AGC_MAX_STEPS 64
#define RDOF_AGC_MAX_PEAKS 256

/* (peak of |I|, |Q| in full scale units, fraction of them clipped) of one chunk. */
void rdof_agc_measure(const rdof_cf_t *x, size_t n, double *peak, double *clipped);
/* The same straight from rtl_sdr bytes (n complex samples), without converting. */
void rdof_agc_measure_cu8(const uint8_t *x, size_t n, double *peak, double *clipped);

#if RDOF_FEATURE_SDR_AGC
typedef struct {
    double min;            /* dB; the least gain the control may use */
    double clip;           /* fraction of samples at full scale that counts as clipping */
    double headroom_db;    /* turn up only if the peak stays this far under full scale afterwards */
    double hold_s;         /* peak window, and the quiet time before each step up */
    double relax_s;        /* without clipping this long, the ceiling goes up one step */
    double settle_s;       /* ignore samples this long after a change */
} rdof_agc_config_t;

void rdof_agc_config_default(rdof_agc_config_t *c);

typedef struct {
    rdof_agc_config_t cfg;
    double steps[RDOF_AGC_MAX_STEPS];
    int nsteps;
    double max, gain, ceiling;
    int changes;
    double settle_until, last_clip, relaxed;
    bool quiet;            /* quiet_since is set */
    double quiet_since;
    /* (t, peak) over hold_s, as a max-queue: same window maximum as a plain list */
    double peak_t[RDOF_AGC_MAX_PEAKS];
    double peak_v[RDOF_AGC_MAX_PEAKS];
    int peak_head, peak_n;
} rdof_agc_t;

/* steps: the gains the device can set (any order). gain: the start gain, or NAN for max_gain. */
void rdof_agc_init(rdof_agc_t *a, const rdof_agc_config_t *cfg, const double *steps, int nsteps, double max_gain,
                 double gain);
/* One chunk's measurement at time t (s). Returns true and sets *new_gain to change the gain. */
bool rdof_agc_update(rdof_agc_t *a, double peak, double clipped, double t, double *new_gain);
#endif

/* Clipping and peak since the last status (agc.LevelStats). */
typedef struct {
    double samples, clipped, peak;
} rdof_level_stats_t;

void rdof_level_stats_add(rdof_level_stats_t *s, size_t n, double peak, double clipped);
/* clip percent (NAN if no samples) and peak dBFS (NAN if 0), rounded as the status reports them; then reset. */
void rdof_level_stats_take(rdof_level_stats_t *s, double *clip_pct, double *peak_dbfs);

#ifdef __cplusplus
}
#endif

#endif
