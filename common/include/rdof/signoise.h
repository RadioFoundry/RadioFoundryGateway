// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Noise on a bench transmit job (docs/spec/bench-api.md 14.6): complex Gaussian noise, band-limited to +-noise_bw/2
 * around the carrier, added to the job's complex baseband before the transmitter; and the `noise` signal mode, the
 * noise with no carrier. In rust (src/signoise.rs), with no C twin: a build without the Rust (RDOF_RUST off) adds no
 * noise.
 *
 * Levels: the carrier's amplitude 1 (a constant-envelope mode's, an I/Q mode's |x| = 1) is the job's power_dbm.
 * With noise the samples are scaled down so the carrier and the noise's 4-sigma peaks stay within full scale, and the
 * attenuator takes off that many dB less (rdof_signoise_atten): the carrier stays at power_dbm. The noise alone has
 * an in-band RMS of 1/4, and power_dbm is its level.
 */
#ifndef RDOF_SIGNOISE_H
#define RDOF_SIGNOISE_H

#include <stdbool.h>
#include <stddef.h>

#include "rdof/jsonp.h"
#include "rdof/sigmode.h"
#include "rdof/txpower.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A job's noise fields (noise_bw, cn_db or noise_dbm, noise_offset_hz, noise_seed) into nz; power_dbm is the job's
 * (alone: the noise mode's, the noise's level). nz->bw_hz 0: none asked. false with why: a bad request. */
bool rdof_signoise_parse(const rdof_jdoc_t *d, int obj, double power_dbm, bool alone, rdof_bench_noise_t *nz,
                         char *why, size_t cap);
/* dB the samples sit under full scale for the noise's room (0: no noise). */
double rdof_signoise_scale_db(const rdof_bench_noise_t *nz);
/* How far from the carrier's frequency the noise reaches, Hz: |offset_hz| and its band out to its low-pass's stop band
 * (0.7 noise_bw). */
double rdof_signoise_reach_hz(const rdof_bench_noise_t *nz);
/* The attenuation, dB under full_dbm (the transmitter's most) and at most span_db, that puts level_dbm (the
 * carrier's, or the noise's alone) on the air with the noise's room taken off. false with why: noise_bw over a
 * quarter of the stream's stream_fs, the noise reaching past room_hz from the carrier (0: not checked), or the level
 * over what the transmitter reaches with the noise. why may be NULL. */
bool rdof_signoise_atten(const rdof_bench_noise_t *nz, double level_dbm, double full_dbm, double span_db,
                         double stream_fs, double room_hz, double *atten_db, char *why, size_t cap);
/* The noise as JSON members for a job's header and summary (",\"noise_bw\":12500,\"noise_offset_hz\":0,
 * \"cn_db\":..,\"noise_dbm\":..,\"carrier_dbm\":.."): its length, 0 (out "") with no noise. level_dbm: the
 * carrier's (alone: the noise's). */
size_t rdof_signoise_json(const rdof_bench_noise_t *nz, double level_dbm, char *out, size_t cap);

/* A generator for a transmit stream of stream_fs samples a second: NULL with no noise, noise_bw over a quarter of
 * stream_fs, or no memory. The noise is made at about 4 * noise_bw (stream_fs divided by a whole number). */
void *rdof_signoise_new(const rdof_bench_noise_t *nz, double stream_fs);
void rdof_signoise_free(void *g);
/* The rate the noise is made at. */
double rdof_signoise_rate(const void *g);
/* n stream samples (I, Q interleaved), in place: the signal scaled for the noise's room and the noise added around
 * off_hz (the carrier, from the stream's center) plus its own offset_hz. */
void rdof_signoise_add(void *g, float *iq, int n, double off_hz);

/* The noise mode's transmitter (rdof/sdr/modes.h): `seconds` of nothing for the noise to fill. */
void *rdof_signoise_tx_new(const rdof_jdoc_t *d, int job, double fs, double *seconds, char *error, size_t ecap);
void rdof_signoise_tx_free(void *tx);
rdof_sig_src_t rdof_signoise_tx_src(void *tx);

#ifdef __cplusplus
}
#endif

#endif
