// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * DMR 4FSK modulator, the transmit side of the demod.
 * Bursts in, complex baseband (or the shaped frequency) out, pulled N samples
 * at a time by a front end.
 *
 * Written from ETSI TS 102 361-1, clause 10.2 (modulation): 4800 symbols a
 * second; dibits 01 00 10 11 as +3 +1 -1 -3 at +-1944 / +-648 Hz (648 Hz of
 * deviation per unit); a square-root raised-cosine filter, roll-off 0.2. The
 * pulse here is 8 symbols long, the demod's matched filter, so the pair is
 * raised-cosine end to end. The 27.5 ms burst in a 30 ms slot is the TDMA
 * timing of the same spec's physical layer. FM: the shaped frequency goes into a 32-bit phase
 * accumulator; sin / cos come from a 256-entry table and a second-order step.
 *
 * Timing is in output samples. A segment's start is the sample where its
 * symbol 0 peaks; symbol k peaks at start + k * fs / 4800 (whole samples at
 * every front-end rate, a multiple of 48 kS/s; exact fractions at any other
 * whole rate). A segment is keyed from half a symbol before its first symbol
 * to half a symbol after its last, plus a raised-cosine ramp each side where
 * it asks for one. Nothing queued: zeros, with the phase still running. The
 * output doesn't depend on how it's pulled.
 *
 * The demod reports a burst at position = symbol 0's peak + RDOF_FM_DELAY (48 kS/s).
 */
#ifndef RDOF_SDR_MOD_H
#define RDOF_SDR_MOD_H

#include <stdbool.h>
#include <stdint.h>

#include "rdof/sdr/features.h"
#include "rdof/sdr/fft.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_MOD_SYMBOL_RATE 4800
#define RDOF_MOD_DEVIATION_HZ 648.0
#define RDOF_MOD_TAPS 9               /* pulse taps per output sample (8 symbols) */
#define RDOF_MOD_MAX_PHASES 1024      /* fs / gcd(fs, 4800) at most */
#define RDOF_MOD_MAX_RAMP 6.0         /* symbols */
#define RDOF_MOD_MAX_SYMBOLS 144      /* per segment: CACH + burst */
#define RDOF_MOD_SLOT_SYMBOLS 144     /* 30 ms */
#define RDOF_MOD_PAD 12               /* zero symbols kept each side of a segment's */

enum { RDOF_MOD_RAMP_UP = 1, RDOF_MOD_RAMP_DOWN = 2, RDOF_MOD_RAMPS = 3 };

typedef struct {
    int64_t start;                  /* sample where symbol 0 peaks */
    int64_t lo, hi;                 /* the samples it touches: [lo, hi) */
    int16_t first, count;           /* sym[0] is symbol first (-12 with a CACH in front) */
    uint8_t flags;                  /* RDOF_MOD_RAMP_* */
    float sym[RDOF_MOD_PAD + RDOF_MOD_MAX_SYMBOLS + RDOF_MOD_PAD]; /* as floats: no conversions per tap */
} rdof_mod_seg_t;

typedef struct {
    uint32_t fs, P, Q;              /* fs / 4800 = P / Q in lowest terms */
    float *taps;                    /* P x RDOF_MOD_TAPS */
    rdof_cf_t *lut;                   /* e^{j 2 pi i / 256}, 256 entries */
    float ramp;                     /* symbols */
    float inv_ramp, inv_p;
    float amplitude;
    float dev_inc;                  /* one unit of deviation in phase-accumulator steps per sample */
    double offset_hz;
    uint32_t base_inc;              /* the offset, in the same */
    uint32_t phase;
    int64_t pos;                    /* sample index of the next output sample */
    rdof_mod_seg_t *q;                /* ring of queued segments, in start order */
    int qcap, qhead, qn;
} rdof_mod_t;

/* fs: whole samples a second, above 9600, with fs / gcd(fs, 4800) <= RDOF_MOD_MAX_PHASES. ramp_symbols 0..6 (4 is
 * 0.83 ms; a 132-symbol burst with both ramps then keys 29.2 ms of the 30 ms slot). queue: segments it can hold
 * (4 is plenty: two slots ahead). position: the first sample's index. Returns 0, or -1 (bad settings, out of memory:
 * then rdof_mod_free). Allocates through rdof_sdr_alloc. */
int rdof_mod_init(rdof_mod_t *m, uint32_t fs, double freq_offset_hz, double ramp_symbols, float amplitude, int queue,
                int64_t position);
void rdof_mod_free(rdof_mod_t *m);
/* Where the channel sits from the front end's center; from the next sample on. */
void rdof_mod_set_offset(rdof_mod_t *m, double hz);

/* count symbols (+-1, +-3) whose symbol first peaks at start + first * fs / 4800. Returns 0; -1 if it is too long,
 * the queue is full, it's before the last one queued, or it would touch a sample already pulled (late). */
int rdof_mod_queue(rdof_mod_t *m, int64_t start, const int8_t *sym, int first, int count, unsigned flags);
/* A 33-byte burst whose first symbol peaks at start; cach: its 24 CACH bits (3 bytes, MSB first) in front, or NULL. */
int rdof_mod_queue_burst(rdof_mod_t *m, int64_t start, const uint8_t burst[33], const uint8_t *cach, unsigned flags);
/* nbits bits (MSB first) as nbits / 2 symbols. */
void rdof_mod_symbols(const uint8_t *bits, int nbits, int8_t *sym);

/* Samples in symbols symbols, or -1 if that isn't whole. rdof_mod_slot_samples: 30 ms. */
int64_t rdof_mod_samples(const rdof_mod_t *m, int symbols);
int64_t rdof_mod_slot_samples(const rdof_mod_t *m);
/* The first sample with power in it for a segment at start (first symbol first) with a ramp up. */
int64_t rdof_mod_key_on(const rdof_mod_t *m, int64_t start, int first);
/* Nothing queued or still sounding. */
bool rdof_mod_idle(const rdof_mod_t *m);

/* The next n samples of complex baseband: |x| = amplitude while keyed. */
void rdof_mod_iq(rdof_mod_t *m, rdof_cf_t *out, int n);
/* Or the next n samples' instantaneous frequency (Hz, offset included) and envelope (0..1, may be NULL), for a chip
 * with its own FM modulator. Either advances the position and the phase. */
void rdof_mod_freq(rdof_mod_t *m, float *hz, float *env, int n);

#ifdef __cplusplus
}
#endif

#endif
