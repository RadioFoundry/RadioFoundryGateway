// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The CPU budget per stage, the same way on a host (rdofsdr --bench) and on a
 * board (the firmware's self-test): each stage run on its own over a cu8 clip,
 * best of reps. Times come from the caller's clock, in seconds.
 */
#ifndef RDOF_SDR_BENCH_H
#define RDOF_SDR_BENCH_H

#include <stdint.h>

#include "rdof/sdr/features.h"
#include "rdof/sdr/receiver.h"

#ifdef __cplusplus
extern "C" {
#endif

#if RDOF_FEATURE_SDR_RECEIVER

enum {
    RDOF_BENCH_CONVERT,   /* cu8 to float */
    RDOF_BENCH_FFT,       /* the channelizer's forward FFTs alone */
    RDOF_BENCH_CHANNEL,   /* the rest of the channelizer: bins, filter, IFFT, NCO */
    RDOF_BENCH_DISC,      /* discriminator */
    RDOF_BENCH_RRC,       /* matched filter and power */
    RDOF_BENCH_SEARCH,    /* squelch, sync search, slicing, flywheel */
    RDOF_BENCH_TRACKER,   /* the call trackers (with no sinks) */
    RDOF_BENCH_STAGES,
};

typedef struct {
    double seconds[RDOF_BENCH_STAGES]; /* per stage, for the whole clip */
    double search_no_squelch;        /* the search stage with the squelch off, for comparison */
    double clip_seconds;             /* how long the clip lasts */
    long bursts;
    long long ffts;
    int fft_points;
} rdof_sdr_bench_t;

extern const char *const rdof_bench_stage_names[RDOF_BENCH_STAGES];

/* Runs the stages over n complex samples of cu8 (cfg->max_chunk at a time). now(): a clock in seconds.
 * Returns 0, or -1 (out of memory, bad plan). */
int rdof_sdr_bench(const uint8_t *cu8, int n, const rdof_sdr_rx_config_t *cfg, const rdof_sdr_channel_t *chans, int nch,
                 double (*now)(void), int reps, rdof_sdr_bench_t *out);

#endif

#ifdef __cplusplus
}
#endif

#endif
