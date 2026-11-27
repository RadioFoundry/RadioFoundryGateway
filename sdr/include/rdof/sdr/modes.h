// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The signal modes sdr has (the interface: rdof/sigmode.h). A platform hands rdof_sdr_modes to the bench line
 * (rdof_bl_platform_t.modes). A new mode: its file in src/ (mode_<name>.c), its table here and in the list.
 */
#ifndef RDOF_SDR_MODES_H
#define RDOF_SDR_MODES_H

#include "rdof/sigmode.h"

#ifdef __cplusplus
extern "C" {
#endif

/* NULL-terminated; empty without RDOF_FEATURE_SDR. */
extern const rdof_sig_mode_t *const rdof_sdr_modes[];
/* The same and the noise mode, for a platform that adds noise (rdof_bl_platform_t.tx_noise); without the Rust, the
 * same as rdof_sdr_modes. */
extern const rdof_sig_mode_t *const rdof_sdr_modes_noise[];

extern const rdof_sig_mode_t rdof_mode_fm;   /* analog FM (mode_fm.c) */
extern const rdof_sig_mode_t rdof_mode_am;   /* analog AM (mode_am.c) */
extern const rdof_sig_mode_t rdof_mode_fsk4; /* 4-level FSK as raw dibits (mode_fsk4.c) */
extern const rdof_sig_mode_t rdof_mode_iq;   /* the client's I/Q (rust's sigiq.rs; with RDOF_RUST only) */
extern const rdof_sig_mode_t rdof_mode_noise; /* noise alone (rust's signoise.rs; with RDOF_RUST only) */

#ifdef __cplusplus
}
#endif

#endif
