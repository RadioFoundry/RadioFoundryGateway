// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * A channel's carrier envelope, measured as it streams: keyed segments with their edges, ramps, plateau level,
 * level steps inside and clipping (bench-api.md 14.2, a bursts stream's "envelope"). One output of the channel at a
 * time, its power (|y|^2, full scale 1.0) and the wideband sample it is centered on; segments come out once their
 * key-down's ramp is in (about 15 ms later).
 *
 * The figures are the bench analyzer's: the 0.1 ms power envelope (a moving mean, centered), the carrier on at
 * on_db over the floor and off under off_db, the edges interpolated between two outputs; a ramp from the envelope
 * leaving the noise (edge_db over the floor) to 3 dB under the plateau (the median 1-3 ms in from the edge), and
 * back at the key-down; a step where the 1 ms envelope moves over step_db in 1 ms, 2 ms in from either edge.
 */
#ifndef RDOF_BENCHENV_H
#define RDOF_BENCHENV_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    double on_db, off_db, edge_db, step_db; /* 10, 7, 6, 3 */
} rdof_benv_opts_t;

#define RDOF_BENV_STEPS 8 /* steps kept a segment (all counted) */

typedef struct {
    double up_sc, down_sc;     /* the edges' wideband samples (fractional); down_sc NAN while still keyed */
    bool open_start;           /* keyed when the envelope began (or after a gap): no key-up edge */
    bool open_end;             /* still keyed at the end (or a gap): no key-down edge */
    bool gap;                  /* ended by samples lost */
    float ramp_up_ms, ramp_down_ms; /* NAN where the carrier didn't come up from (or drop to) the noise */
    float level_dbfs;          /* the plateau after the key-up (or the segment's first 2 ms, open_start) */
    float end_dbfs;            /* and before the key-down */
    float peak_dbfs, min_dbfs; /* the 1 ms envelope's highest and lowest inside, 2 ms in from either edge */
    int steps;                 /* level steps inside */
    struct { double sc; float db; } step[RDOF_BENV_STEPS];
    long clipped, inputs;      /* I/Q components at the ADC's rails over the segment, components */
} rdof_benv_seg_t;

typedef struct rdof_benv rdof_benv_t;

/* rate: the outputs' (about 48 kS/s); d: wideband samples an output. NULL without memory. */
rdof_benv_t *rdof_benv_new(double rate, int d, const rdof_benv_opts_t *o);
void rdof_benv_free(rdof_benv_t *e);
/* The floor (dB of power, full scale 0): nothing is measured until it is known. */
void rdof_benv_floor(rdof_benv_t *e, double floor_db);
/* One output: its power, the wideband sample it is centered on, its inputs at the rails (components). An sc that
 * isn't the last one's plus d is a gap: what was measured so far ends there. */
void rdof_benv_push(rdof_benv_t *e, uint64_t sc, float p, int clipped);
/* The stream's end: what's left is measured, a segment still keyed comes out open. */
void rdof_benv_finish(rdof_benv_t *e);
/* The next finished segment: false none. */
bool rdof_benv_take(rdof_benv_t *e, rdof_benv_seg_t *out);
/* The 1 ms envelope's 10th percentile since the start (dB; NAN: none yet), the bench analyzer's noise figure. */
double rdof_benv_noise_db(const rdof_benv_t *e);
/* Wideband samples the envelope lags the newest output by (its centered means, and the look-ahead). */
uint64_t rdof_benv_lag(const rdof_benv_t *e);

#endif
