// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * A burst's PA ramp (bench-api.md 14.2, a bursts stream's "ramp"): one channel's I/Q outputs kept for a moment, and each
 * decoded burst measured from them against its own settled level and payload. t = 0 is the leading edge of the first
 * symbol (for the falling edge, the trailing edge of the last); negative is before. The burst's symbols, from the
 * decoder, place it: its position is refined against the discriminator, for voice B-F as for a burst with a sync.
 *
 * Levels are compared in linear power, logs only at a crossing and for the few figures that go out: a log an output was
 * a fifth of a W12 core.
 */
#ifndef RDOF_BENCHRAMP_H
#define RDOF_BENCHRAMP_H

#include <stdbool.h>
#include <stdint.h>

#define RDOF_BRAMP_RISES 7 /* -40, -30, -20, -10, -6, -3, -1 dB */
#define RDOF_BRAMP_FALLS 3 /* -3, -10, -30 dB */
#define RDOF_BRAMP_BINS 10 /* 0.5 ms each */

/* A figure that can't be claimed is NAN. */
typedef struct {
    float p0_ms;                                 /* the first symbol's leading edge, from the sync center given */
    float align_ms;                              /* how far the symbols moved the position given */
    float settled_db, ripple_db;                 /* the median 0.1 ms level 4 ms in from either end of the payload; its spread */
    float rise_ms[RDOF_BRAMP_RISES], rise_30_3_ms;
    float overshoot_db, overshoot_t_ms;          /* the 0.1 ms envelope's peak in the 4 ms from the -3 dB point */
    float settle_1db_ms, settle_0p5db_ms;        /* comes within the band and stays 2 ms */
    float sym1_db, err5_mean_db, err5_abs_db, err12_mean_db, err12_abs_db; /* against settled, no lower than the floor */
    float fall_ms[RDOF_BRAMP_FALLS], fall_3_30_ms;
    float pre_db[RDOF_BRAMP_BINS];               /* over the quiet floor: the 5 ms before t = 0 */
    float empty_db[RDOF_BRAMP_BINS];             /* and 5 ms around the middle of the slot before; NAN: a burst is there */
    float f_first1ms_hz, f_next2ms_hz;           /* the residual, from the burst's own mean, over the first symbols with carrier */
    bool f_first1ms_unreliable, f_next2ms_unreliable; /* a symbol there is over 20 dB down */
    float dev3_hz;
    long clipped;                                /* inputs at the rails over the payload */
} rdof_bramp_res_t;

enum { RDOF_BRAMP_OK = 0, RDOF_BRAMP_WAIT, RDOF_BRAMP_LOST };

typedef struct rdof_bramp rdof_bramp_t;

/* rate: the outputs' (about 48 kS/s); d: wideband samples an output. NULL without memory. */
rdof_bramp_t *rdof_bramp_new(double rate, int d);
void rdof_bramp_free(rdof_bramp_t *r);
/* One output, centered on wideband sample sc; its inputs at the rails. An sc that isn't the last one's plus d is a gap:
 * what came before it is gone. */
void rdof_bramp_push(rdof_bramp_t *r, uint64_t sc, float i, float q, int clipped);
/* The burst whose sync center is wideband sample sync_sc, its 33 bytes as decoded; quiet_db the channel's quiet floor (NAN:
 * not known). RDOF_BRAMP_WAIT: what follows it isn't in yet. last: the stream has ended, measure what there is.
 * RDOF_BRAMP_LOST: its samples are gone (or never came). */
int rdof_bramp_measure(rdof_bramp_t *r, double sync_sc, const uint8_t raw[33], double quiet_db, bool last,
                       rdof_bramp_res_t *out);

#endif
