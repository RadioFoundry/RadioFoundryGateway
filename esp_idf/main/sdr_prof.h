// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Where the DSP's time goes on the board (sdr_task.c): per chunk, each core's
 * cycles by step, the time the DSP core waits on the helper and the helper
 * takes to wake, and where each of a receiver's buffers was put. The
 * self-test prints it after each load run; sdr_prof_reset starts it over.
 */
#pragma once

#include <stdint.h>

#include "rdof/sdr/receiver.h"

typedef struct {
    uint32_t chunks;
    int64_t samples;           /* wideband samples in */
    int64_t wall_us;           /* sdr_process_chunk, start to end */
    uint64_t fwd;              /* cycles: the forward FFTs (core 1), or the whole channelizer on one core */
    uint64_t chz[2], demod[2]; /* cycles by core: [0] the DSP core (1), [1] the helper (core 0) */
    uint64_t finish;           /* trackers' ticks and AFC */
    int64_t join_us;           /* the DSP core waiting for the helper to finish */
    int64_t wake_us;           /* from "go" to the helper starting */
    int split;                 /* channels on the DSP core at the end (-1: one core) */
} sdr_prof_t;

void sdr_prof_reset(void);
/* Two-core runs with the cores taking turns (the helper's share, then core 1's): each share's cost without the
 * other core running, to compare with them running together (shared cache and PSRAM). Self-test only. */
void sdr_prof_serial(bool on);
void sdr_prof_get(sdr_prof_t *out);
/* "y PSRAM 49 KB, Xs internal 43 KB, ...": where rx's buffers are. */
void sdr_prof_placement(const rdof_sdr_rx_t *rx, char *out, size_t cap);
