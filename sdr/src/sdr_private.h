// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* Shared bits for sdr sources. */
#ifndef RDOF_SDR_PRIVATE_H
#define RDOF_SDR_PRIVATE_H

#include <stddef.h>

#define RDOF_PI 3.14159265358979323846

/* Every allocation goes through these (rdof_sdr_set_allocator), so a firmware can put DSP state in fast RAM. */
void *rdof_sdr_alloc(size_t n);
void *rdof_sdr_alloc_hot(size_t n); /* rdof_sdr_set_hot_allocator */
void *rdof_sdr_zalloc(size_t n);
void rdof_sdr_free(void *p);

/* The k-th smallest of v[0..n-1] (reorders v): channelizer.c's. */
float rdof_sdr_select_kth(float *v, int n, int k);

/* The analog modes' audio (mode_fm.c): a program of rdof/sdr/fmgen.h's with its PCM input, as FM sends it as
 * deviation and AM as the envelope. A mode's transmitter starts with an rdof_prog_tx_t, so the rdof_prog_tx_in*
 * functions take it as their tx (the signal mode table's stream input). */
#include <stdbool.h>
#include <stdint.h>

#include "rdof/jsonp.h"
#include "rdof/sdr/fmgen.h"

typedef struct {
    rdof_fmg_t g;
    int16_t *ring;
    uint64_t pushed;             /* PCM samples taken */
    int carry;                   /* a PCM sample's low byte, waiting for its high one (-1 none) */
} rdof_prog_tx_t;

/* The job's program, pcm, repeat and seed into c (its other fields the mode's): NULL, or why not (in why, or a
 * constant). */
const char *rdof_prog_parse(const rdof_jdoc_t *d, int job, rdof_fmg_cfg_t *c, char *why, size_t cap);
/* The generator at fs, with a PCM ring when the program has PCM: 0, or -1 with error. */
int rdof_prog_start(rdof_prog_tx_t *t, const rdof_fmg_cfg_t *c, double fs, char *error, size_t ecap);
void rdof_prog_end(rdof_prog_tx_t *t);
size_t rdof_prog_tx_in_room(void *tx);
size_t rdof_prog_tx_in(void *tx, const uint8_t *b, size_t n);
void rdof_prog_tx_in_end(void *tx, bool gone);
uint64_t rdof_prog_tx_in_want(void *tx);

#endif
