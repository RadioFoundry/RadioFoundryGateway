// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The self-test's output check: portable, so the host tests run it too. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool pass;
    const char *name;           /* "clean", or the fault (clip_variants) */
    int messages, expect_messages;
    uint32_t crc, expect_crc;
    long bursts, expect_bursts;
} sdr_check_t;

/* Run the receiver over the embedded clip (clip.cu8) as the board would, in CLIP_CHUNK pieces, and compare the
 * tracker output with the expected (selftest/clip.h). split > 0: demodulate channels [0, split) and [split, n) as two steps,
 * the way the two-core mode does. */
bool sdr_selftest_check(const uint8_t *clip, size_t len, int split, sdr_check_t *out);
/* The same with fault v (0 .. CLIP_VARIANTS-1) put into the clip as it's read: a tuner offset, noise, overload.
 * Passes on the expected message and burst counts; the CRC can differ in noise (the board's int16 filter may
 * slice a marginal voice bit the other way) and is only reported. */
bool sdr_selftest_check_variant(const uint8_t *clip, size_t len, int v, sdr_check_t *out);
/* Fault v on n bytes of the clip starting at byte offset `at` (even), into out; *noise is the noise generator's
 * state, carried from one piece to the next (start it at CLIP_NOISE_SEED). */
void sdr_clip_impair(int v, const uint8_t *in, size_t n, size_t at, uint32_t *noise, uint8_t *out);

/* How every chunk is run when set (the firmware hands in its own step, one core or two, so it can profile them;
 * split is then whatever it takes); NULL: one core, or both halves in turn for split > 0. */
struct rdof_sdr_rx;
typedef struct rdof_sdr_rx rdof_sdr_rx_t;
#include "rdof/sdr/fft.h"
extern void (*sdr_selftest_step)(rdof_sdr_rx_t *rx, const rdof_cf_t *iq, int n, int split);

/* zlib's CRC-32. */
uint32_t sdr_crc32(uint32_t crc, const void *data, size_t n);
