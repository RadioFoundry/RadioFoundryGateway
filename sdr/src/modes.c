// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The list of signal modes (rdof/sdr/modes.h). */
#include "rdof/sdr/modes.h"

#include <stddef.h>

#include "rdof/sdr/features.h"

#if RDOF_FEATURE_SDR && defined(RDOF_RUST) && RDOF_RUST
#include "rdof/sigiq.h"
#include "rdof/signoise.h"

/* The client's I/Q, sent as it comes (rust's sigiq.rs): host builds with the Rust (the PlutoSDR's, rdofradio's) */
const rdof_sig_mode_t rdof_mode_iq = {
    .name = "iq",
    .about = "the client's complex baseband (cs16) at its own rate: several carriers at once, any modulation",
    .tx_new = rdof_sigiq_tx_new,
    .tx_free = rdof_sigiq_tx_free,
    .tx_src = rdof_sigiq_tx_src,
    .tx_in_room = rdof_sigiq_tx_in_room,
    .tx_in = rdof_sigiq_tx_in,
    .tx_in_end = rdof_sigiq_tx_in_end,
    .tx_in_want = rdof_sigiq_tx_in_want,
    .tx_report = rdof_sigiq_tx_report,
    .tx_record = rdof_sigiq_tx_record,
};
/* Noise alone, no carrier (rust's signoise.rs): listed by a platform that adds noise (rdof_sdr_modes_noise) */
const rdof_sig_mode_t rdof_mode_noise = {
    .name = "noise",
    .about = "noise alone, no carrier: power_dbm is its level in noise_bw",
    .tx_new = rdof_signoise_tx_new,
    .tx_free = rdof_signoise_tx_free,
    .tx_src = rdof_signoise_tx_src,
};
const rdof_sig_mode_t *const rdof_sdr_modes[] = {&rdof_mode_fm, &rdof_mode_am, &rdof_mode_fsk4, &rdof_mode_iq, NULL};
const rdof_sig_mode_t *const rdof_sdr_modes_noise[] = {&rdof_mode_fm, &rdof_mode_am,    &rdof_mode_fsk4,
                                                       &rdof_mode_iq, &rdof_mode_noise, NULL};
#elif RDOF_FEATURE_SDR
const rdof_sig_mode_t *const rdof_sdr_modes[] = {&rdof_mode_fm, &rdof_mode_am, &rdof_mode_fsk4, NULL};
const rdof_sig_mode_t *const rdof_sdr_modes_noise[] = {&rdof_mode_fm, &rdof_mode_am, &rdof_mode_fsk4, NULL};
#else
const rdof_sig_mode_t *const rdof_sdr_modes[] = {NULL};
const rdof_sig_mode_t *const rdof_sdr_modes_noise[] = {NULL};
#endif
