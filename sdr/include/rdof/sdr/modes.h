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

extern const rdof_sig_mode_t rdof_mode_fm;   /* analog FM (mode_fm.c) */
extern const rdof_sig_mode_t rdof_mode_am;   /* analog AM (mode_am.c) */

#ifdef __cplusplus
}
#endif

#endif
