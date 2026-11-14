// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* Where a long key-up's seams fall (fe_lr2021.c tx_run_long): portable, so the host tests run it too.
 *
 * A long key-up goes out in runs of a fixed length (the chip counts a run in 16 bits, lr2021-iq.md R3.43), and each
 * seam between runs skips the samples the chip's restart takes (R3.49), so the stream keeps its time. With runs of
 * a whole number of slots, each seam fell that many skipped samples (about 8, 0.8 of a symbol) later in the slot than
 * the one before: over a long key-up the seams walk through every part of the slot, the sync included. Runs that
 * many samples shorter keep every seam at one place in the slot, and the first run's start puts that place where a
 * receiver minds a seam least. */
#pragma once

#include <stdint.h>

/* The run length for a key-up whose seams each skip seam samples (fractional): the longest whole number of periods,
 * less the skip rounded, within max_run. So a run and its seam's skip span whole periods, within half a sample a
 * seam. 0 if no period fits. */
int64_t rdof_seam_run(double seam, int64_t max_run, int64_t period);

/* The first run's start: at k_go (the first sample there's still time for), unless that puts the seams more than tol
 * samples from at (samples after a period's start) in the period; then the first sample after k_go that puts them
 * there. ref is a period's start in the stream (any). A seam's place is its first skipped sample: start + run, then
 * every run plus skip after. */
int64_t rdof_seam_start(int64_t k_go, int64_t run, int64_t period, int64_t ref, int64_t at, int64_t tol);

/* The seam's place (its first skipped sample) for a run starting at start: samples after a period's start. */
int64_t rdof_seam_phase(int64_t start, int64_t run, int64_t period, int64_t ref);

/* The first seams restart later than the rest (lr2021-iq.md R3.49: 190-230 us, then 161-180), so a skip sized for
 * the rest left the stream 20-60 us behind its time after them. RDOF_SEAM_FIRST seams get their own skip, learned
 * from the key-ups before. */
#define RDOF_SEAM_FIRST 2

/* The skip at seam n (1: the first), us: gap[n - 1], learned, for the first RDOF_SEAM_FIRST seams (first when not
 * learned yet), else steady. */
int rdof_seam_skip_us(const int gap[RDOF_SEAM_FIRST], int n, int first, int steady);

/* Learns the first seams' gaps from a key-up's TX_DONE times, each run's (done_us[0..n-1]; the first run's from its
 * key-up, so not a gap), runs of run_us: each gap past the run's length, averaged a quarter at a time. */
void rdof_seam_learn(int gap[RDOF_SEAM_FIRST], const int64_t *done_us, int n, double run_us);
