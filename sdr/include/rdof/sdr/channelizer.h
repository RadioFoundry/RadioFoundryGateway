// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Fast-convolution (overlap-save) channelizer. One wideband IQ stream in, one narrowband
 * complex stream per channel out at sample_rate / decimation.
 *
 * Each block of N = decimation * out_fft input samples is FFT'd once (hop
 * 3N/4, a filter of N/4 + 1 taps). Every channel takes the out_fft bins around
 * its own center, applies the filter and inverse-FFTs them: filter, shift to
 * baseband and decimate in one step. The bin-shift phase is undone per block,
 * and a per-channel NCO removes the residual offset plus any correction (ppm,
 * AFC).
 */
#ifndef RDOF_SDR_CHANNELIZER_H
#define RDOF_SDR_CHANNELIZER_H

#include <stdint.h>

#include "rdof/sdr/features.h"
#include "rdof/sdr/fft.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_CHZ_OUT_FFT 64
#define RDOF_CHZ_CUTOFF_HZ 7500.0

typedef struct {
    double fs, out_rate;
    int N, M, D, hop, C, discard, out_per_hop;
    int *k;               /* bin offset per channel, 0..N-1 */
    double *residual_hz;  /* channel offset minus its bin */
    double *correction_hz;/* extra shift per channel (ppm, AFC); the caller may change it between calls */
    double *phase;        /* NCO phase per channel (radians) */
    double *step_cps, *step_rad; /* the NCO step it was last computed for */
    double *corr_used;    /* the correction_hz step_cps was computed from (NAN: not yet) */
    double *block_rad;    /* step_rad * out_per_hop: the phase one block moves */
    rdof_cf_t *stepr;       /* e^{-j step} */
    rdof_cf_t *H;           /* M filter bins, scaled by 1 / (decimation * M) */
    rdof_cf_t *win, *X, *Y, *y;
    rdof_cf_t *twiddle;     /* e^{-j 2 pi m / N} */
    rdof_fft_t *fwd, *inv;
    int fill;
    long long block;
    unsigned boff;        /* (block * hop) % N */
    /* Activity, for the receiver's idle gate: each call, level[c] is the highest ratio, over its blocks, of the
     * channel's output power to what noise alone gives there: the band's noise through the channel filter. The
     * band's noise is the 25th percentile, as a mean, of every 4th bin of a call's first block every 8th call,
     * Hann-windowed (the overlap-save FFT is unwindowed, and a strong signal's leakage fills its empty bins; a
     * periodic Hann window is X[k]/2 - (X[k-1] + X[k+1])/4, so it costs no FFT and no buffer). About 1 on noise;
     * 2 is a signal 0 dB over the noise in the filter. */
    float *level;
    float hnoise;         /* sum h^2: the filter's noise gain (output power per unit of input noise power) */
    float noise_in;       /* input noise power per sample; 0 until known */
    int noise_age;        /* calls since */
    /* the two-step form (rdof_chz_forward, rdof_chz_channels) */
    rdof_cf_t *Xs;          /* each block's spectrum, N apiece */
    unsigned *xs_boff;    /* and its offset */
    int xs_cap, nblk;
    float xs_noise;
    rdof_fft_t *inv2;       /* a second core's inverse FFT and scratch: a plan isn't safe on two cores at once */
    rdof_cf_t *Y2, *y2;
} rdof_chz_t;

/* Kaiser-windowed sinc low-pass (beta 5), unity DC gain: channelizer._lowpass. out has num_taps. */
void rdof_chz_lowpass(int num_taps, double cutoff_hz, double fs, double *out);

/* offsets_hz: each channel's frequency minus the tuner center. Returns 0, or -1 (bad
 * parameters, unsupported FFT size, out of memory; then call rdof_chz_free anyway). */
int rdof_chz_init(rdof_chz_t *s, double sample_rate, const double *offsets_hz, int channels, int decimation,
                int out_fft, double cutoff_hz);
void rdof_chz_free(rdof_chz_t *s);
/* Output samples per channel that n_in more input samples will produce. */
int rdof_chz_out_len(const rdof_chz_t *s, int n_in);
/* Feed n_in samples. Channel c's output goes to out + c * stride (stride >= rdof_chz_out_len).
 * Returns the samples written per channel. */
int rdof_chz_process(rdof_chz_t *s, const rdof_cf_t *in, int n_in, rdof_cf_t *out, int stride);

/* The same in two steps, so two cores can share the per-channel part (bins, filter, inverse FFT, NCO): first
 * the forward FFTs of every block (rdof_chz_reserve sizes their store for up to max_in samples a call; 0, or -1
 * out of memory), then rdof_chz_channels over disjoint channel ranges, worker 0 on one core and 1 on the other.
 * Returns the samples per channel, as rdof_chz_process; the output is the same. */
int rdof_chz_reserve(rdof_chz_t *s, int max_in);

/* Where the forward step's time goes, in a clock's ticks over every channelizer, while a clock is set (NULL: off,
 * the default): the forward FFTs (into the block store for rdof_chz_forward), the input copy and window move, the
 * noise floor. For a board's profile; nothing reads it here. */
typedef struct {
    uint64_t fft, copy, noise, blocks;
} rdof_chz_prof_t;
extern rdof_chz_prof_t rdof_chz_prof;
void rdof_chz_set_clock(uint32_t (*ticks)(void));
int rdof_chz_forward(rdof_chz_t *s, const rdof_cf_t *in, int n_in);
void rdof_chz_channels(rdof_chz_t *s, int c0, int c1, int worker, rdof_cf_t *out, int stride);

/* channelizer.plan_center: a tuner center keeping every channel in the usable band, away from DC and
 * from the other channels' IQ images. Returns 0 and sets *center, or -1 if there is none. */
int rdof_plan_center(const double *freqs_hz, int n, double sample_rate, double *center);

/* The tuner window when the channels may not all fit (a narrow rate such as 288 kS/s): the center
 * that takes the most channels inside the usable band (80% of the rate) and clear of the DC guard
 * (25 kHz). Ties go to the window plan_center scores best. When every channel fits it is plan_center's
 * answer. inside[i] (may be NULL): channel i is in the window. Returns how many are, 0 if none. */
int rdof_plan_window(const double *freqs_hz, int n, double sample_rate, double *center, bool *inside);

/* The two for a front end whose band isn't the RTL-SDR's: usable, the share of the rate channels go within (as
 * rdof_fe_caps_t's), and dc_guard, Hz. The two above are these with 0.8 and 25 kHz. */
int rdof_plan_center_band(const double *freqs_hz, int n, double sample_rate, double usable, double dc_guard,
                          double *center);
int rdof_plan_window_band(const double *freqs_hz, int n, double sample_rate, double usable, double dc_guard,
                          double *center, bool *inside);

#ifdef __cplusplus
}
#endif

#endif
