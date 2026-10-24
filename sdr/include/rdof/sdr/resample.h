// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * A rational resampler for front ends whose rates aren't a multiple of 48 kS/s
 * (chips clocked from 32 MHz: 250 kS/s in, 240 kS/s out is 24 / 25). A
 * polyphase FIR on complex float: the prototype is a Kaiser-windowed sinc
 * at L times the input rate, cut off halfway between the output's usable band
 * (usable times its rate, two-sided) and the nearest frequency that would
 * alias into it, so the band the channelizer uses stays flat. Written from the
 * textbook polyphase structure (Crochiere and Rabiner); portable C.
 */
#ifndef RDOF_SDR_RESAMPLE_H
#define RDOF_SDR_RESAMPLE_H

#include <stddef.h>

#include "rdof/sdr/fft.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int L, M;            /* out / in = L / M, in lowest terms */
    int K;               /* taps a phase */
    float *h;            /* L phases of K taps, each phase reversed: h[p * K + K - 1 - j] is its tap j */
    float *buf;          /* the last K - 1 inputs, then the block being worked on: I, then Q, K - 1 + cap each */
    int cap;             /* inputs buf takes after the history */
    int phase;           /* the next output's phase */
    int next;            /* the next output's newest input, counted from buf's first history sample */
} rdof_resamp_t;

/* in_rate to out_rate (whole S/s), K taps a phase (10 to 64; more is flatter and steeper), passing usable x
 * out_rate (0.8 for the channelizer); blocks of at most max_in inputs. 0, or -1 (no memory, or a ratio past
 * 1000 / 1000). */
int rdof_resamp_init(rdof_resamp_t *r, long in_rate, long out_rate, int K, double usable, int max_in);
void rdof_resamp_free(rdof_resamp_t *r);
/* At most rdof_resamp_max_out(r, n) outputs for n inputs. */
int rdof_resamp_max_out(const rdof_resamp_t *r, int n);
/* n inputs (n <= max_in) in, the outputs they complete into out; returns how many. */
int rdof_resamp_run(rdof_resamp_t *r, const rdof_cf_t *in, int n, rdof_cf_t *out);

#ifdef __cplusplus
}
#endif

#endif
