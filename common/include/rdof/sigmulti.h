// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Several carriers in one transmit job (docs/spec/bench-api.md 14.6): a signal mode's transmitter once a carrier, each
 * from an object in the job's `carriers` (its freq, start_s, level and the mode's fields over the job's), summed into
 * one complex baseband at the transmit stream's rate. In rust (src/sigmulti.rs), with no C twin: a build without the
 * Rust (RDOF_RUST off) sends one carrier at a time.
 */
#ifndef RDOF_SIGMULTI_H
#define RDOF_SIGMULTI_H

#include "rdof/sigmode.h"

#ifdef __cplusplus
extern "C" {
#endif

/* NULL with error. carriers: the job's array node; base_mhz: the job's freq (0 Hz); fs: the platform's rate for a
 * mode's source; rate: the output's (the transmit stream's, which each source's rate must divide). */
void *rdof_sigmulti_tx_new(const rdof_sig_mode_t *m, const rdof_jdoc_t *d, int job, int carriers, double base_mhz,
                           double fs, double rate, double *seconds, char *error, size_t ecap);
void rdof_sigmulti_tx_free(void *tx);
rdof_sig_src_t rdof_sigmulti_tx_src(void *tx);
/* ,"carriers":[{"freq","start_s","level", the mode's report}, ...] */
size_t rdof_sigmulti_tx_report(void *tx, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
