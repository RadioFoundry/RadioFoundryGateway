// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The bench's iq signal mode, transmit only (docs/spec/bench-api.md 14.6): the client's complex baseband (cs16 LE, I
 * then Q) at its own rate, sent as it comes. In rust (src/sigiq.rs), with no C twin: a build without the Rust
 * (RDOF_RUST off) has no iq mode. sdr's mode list (rdof/sdr/modes.h) makes the table from these.
 */
#ifndef RDOF_SIGIQ_H
#define RDOF_SIGIQ_H

#include "rdof/sigmode.h"

#ifdef __cplusplus
extern "C" {
#endif

void *rdof_sigiq_tx_new(const rdof_jdoc_t *d, int job, double fs, double *seconds, char *error, size_t ecap);
void rdof_sigiq_tx_free(void *tx);
rdof_sig_src_t rdof_sigiq_tx_src(void *tx);
size_t rdof_sigiq_tx_in_room(void *tx);
size_t rdof_sigiq_tx_in(void *tx, const uint8_t *b, size_t n);
void rdof_sigiq_tx_in_end(void *tx, bool gone);
uint64_t rdof_sigiq_tx_in_want(void *tx);
size_t rdof_sigiq_tx_report(void *tx, char *out, size_t cap);
size_t rdof_sigiq_tx_record(void *tx, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
