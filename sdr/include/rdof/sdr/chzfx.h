// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The FPGA channelizer's bit-exact model: what an FPGA block computes, in integers, so a host test can check it
 * against the float channelizer (channelizer.h) and an HLS testbench against the same golden vectors.
 *
 * One wideband stream of 12-bit I/Q in (int16, the AD9361's samples as the DMA carries them), C channels of int16
 * I/Q out at fs / D. Per channel, per input sample: a 32-bit NCO picks e^{-j theta} from a 4096-point cosine table
 * (a quarter of it stored), the sample is mixed down and kept in that channel's history. Every D-th input sample
 * (the first included) every channel puts out one sample: its L-tap FIR over the history, rounded and saturated.
 * A direct-form DDC, where the float channelizer is overlap-save: the same filter (rdof_chz_lowpass, 16 D + 1
 * taps) on the same output grid, so the outputs agree within the filter's stopband and the fixed point.
 *
 * Every step is integer and exact, so the order of the sums (lanes, folding) doesn't change the result:
 *   lo     = qcos table, index phase >> 20; cos at k, sin at k - 1024 (mod 4096)
 *   mixed  = sat18(round((x_i cos + x_q sin) / 2^11)), sat18(round((x_q cos - x_i sin) / 2^11))
 *   acc    = sum over m < L of coef[m] * mixed[n - m] (45 bits at most: inside the DSP48's 48)
 *   y      = sat16(round(acc / 2^out_shift))
 *   phase += inc after each input sample
 * round() is half up (add half, shift right arithmetically). A 12-bit input comes out 16 times larger after the
 * mixer (4 fraction bits) and, with rdof_chzfx_design's shift, 8 times larger at the output (3 fraction bits, one
 * bit of headroom for a full-scale tone).
 */
#ifndef RDOF_SDR_CHZFX_H
#define RDOF_SDR_CHZFX_H

#include <stddef.h>
#include <stdint.h>

#include "rdof/sdr/features.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_CHZFX_MAX_CH 16      /* channels */
#define RDOF_CHZFX_HIST 1024      /* history per channel: taps + decimation must fit */
#define RDOF_CHZFX_MAX_TAPS 1024
#define RDOF_CHZFX_MAX_DECIM 60   /* 2.88 MS/s at 48 kS/s: 961 taps */
#define RDOF_CHZFX_COEF_BITS 18   /* the DSP48's B port */
#define RDOF_CHZFX_MIX_BITS 18
#define RDOF_CHZFX_MIX_SHIFT 11
#define RDOF_CHZFX_OUT_FRAC 3     /* output LSB: an eighth of the input's */
#define RDOF_CHZFX_LO_AMP 32767   /* the cosine table's peak */

/* The quarter-wave table: qcos[i] = round(32767 cos(2 pi (i + 0.5) / 4096)). The half step keeps the quarter
 * symmetric: the full table at k = 1024 q + i is qcos[i], -qcos[1023 - i], -qcos[i], qcos[1023 - i] for quadrants
 * q = 0..3. */
extern const int16_t rdof_chzfx_qcos[1024];

typedef struct {
    int16_t i, q;
} rdof_iq16_t;

/* What the registers hold. */
typedef struct {
    int decim;                          /* D, 1..RDOF_CHZFX_MAX_DECIM */
    int taps;                           /* L, 1..RDOF_CHZFX_MAX_TAPS, L + D <= RDOF_CHZFX_HIST */
    int channels;                       /* C, 1..RDOF_CHZFX_MAX_CH */
    int out_shift;                      /* 1..47 */
    int32_t coef[RDOF_CHZFX_MAX_TAPS];  /* signed RDOF_CHZFX_COEF_BITS bits */
    uint32_t inc[RDOF_CHZFX_MAX_CH];    /* NCO step per input sample, 2^32 a turn: offset / fs */
    uint32_t phase0[RDOF_CHZFX_MAX_CH]; /* NCO phase at the first input sample */
} rdof_chzfx_cfg_t;

typedef struct {
    rdof_chzfx_cfg_t cfg;
    uint32_t phase[RDOF_CHZFX_MAX_CH];
    uint32_t pending[RDOF_CHZFX_MAX_CH]; /* a step set since the last output */
    uint32_t pending_mask;
    int32_t hist_i[RDOF_CHZFX_MAX_CH][RDOF_CHZFX_HIST], hist_q[RDOF_CHZFX_MAX_CH][RDOF_CHZFX_HIST];
    unsigned wr;                        /* where the next input goes */
    int count;                          /* inputs until the next output, 0: this one */
    uint32_t seq;                       /* outputs so far (the frame number) */
} rdof_chzfx_t;

/* The cosine of NCO phase p, as the table gives it (sin p is rdof_chzfx_lo(p - 2^30)). */
int32_t rdof_chzfx_lo(uint32_t phase);

/* The NCO step for a channel offset_hz from the center at fs, rounded, wrapped to 32 bits. */
uint32_t rdof_chzfx_inc(double offset_hz, double fs);

/* A configuration for the float channelizer's filter (rdof_chz_lowpass, 16 D + 1 taps, cutoff_hz) at fs, scaled
 * so its largest tap fills RDOF_CHZFX_COEF_BITS, with the shift that gives RDOF_CHZFX_OUT_FRAC fraction bits out;
 * each channel's step from offsets_hz, phases 0. Returns 0, or -1 (bad arguments, a filter that doesn't fit). */
int rdof_chzfx_design(rdof_chzfx_cfg_t *cfg, double fs, int decim, const double *offsets_hz, int channels,
                      double cutoff_hz);

/* 0 when the hardware takes cfg, else -1. */
int rdof_chzfx_check(const rdof_chzfx_cfg_t *cfg);

/* A model in its reset state (histories zero, phases at phase0): NULL for a bad cfg or no memory. */
rdof_chzfx_t *rdof_chzfx_new(const rdof_chzfx_cfg_t *cfg);
void rdof_chzfx_free(rdof_chzfx_t *s);
void rdof_chzfx_reset(rdof_chzfx_t *s);

/* A new step for channel c (AFC, a retune), as the shadow register and APPLY: the step changes right after the
 * next output, from the input sample that follows it. */
void rdof_chzfx_set_inc(rdof_chzfx_t *s, int c, uint32_t inc);

/* Output instants that n_in more input samples complete. */
int rdof_chzfx_out_len(const rdof_chzfx_t *s, int n_in);
/* Feed n_in samples; each output instant writes C samples, channel order, to out (rdof_chzfx_out_len(n_in) * C
 * of room). Returns the output instants written. */
int rdof_chzfx_process(rdof_chzfx_t *s, const rdof_iq16_t *in, int n_in, rdof_iq16_t *out);

/*
 * The frames the block writes to the RX DMA, 64-bit little-endian words. A header, then two samples a word (the
 * first in bits 31..0: I in 15..0, Q in 31..16), the last word zero-padded:
 *   bits 63..48  RDOF_CHZFX_MAGIC
 *   bits 47..44  type: RDOF_CHZFX_FRAME_CH (one output instant, a sample per channel) or RDOF_CHZFX_FRAME_RAW
 *                (the D input samples since the last output instant, for the noise floor and the spectrum)
 *   bits 43..32  samples in the frame
 *   bits 31..0   the output instant's number (both types)
 * Read as the IIO buffer of cf-ad9361-lpc with its four channels on, a word is one sample of voltage0..3.
 */
#define RDOF_CHZFX_MAGIC 0xC5A7u
#define RDOF_CHZFX_FRAME_CH 0
#define RDOF_CHZFX_FRAME_RAW 1

typedef struct {
    int type, n;
    uint32_t seq;
    const uint64_t *data; /* (n + 1) / 2 words */
} rdof_chzfx_frame_t;

/* Words a frame of n samples takes. */
static inline size_t rdof_chzfx_frame_words(int n) { return 1 + ((size_t)n + 1) / 2; }
/* Writes the frame to out (rdof_chzfx_frame_words(n)); returns the words written. */
size_t rdof_chzfx_frame(int type, uint32_t seq, const rdof_iq16_t *x, int n, uint64_t *out);
/* The frame at w (avail words): the words it takes, 0 if it runs past avail, -1 if w[0] isn't a header. */
int rdof_chzfx_unframe(const uint64_t *w, size_t avail, rdof_chzfx_frame_t *f);
/* Sample k of a parsed frame. */
rdof_iq16_t rdof_chzfx_frame_sample(const rdof_chzfx_frame_t *f, int k);

#ifdef __cplusplus
}
#endif

#endif
