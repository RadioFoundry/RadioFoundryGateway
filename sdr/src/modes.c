// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The list of signal modes (rdof/sdr/modes.h). */
#include "rdof/sdr/modes.h"

#include <stddef.h>

#include "rdof/sdr/features.h"

#if RDOF_FEATURE_SDR
const rdof_sig_mode_t *const rdof_sdr_modes[] = {&rdof_mode_fm, NULL};
#else
const rdof_sig_mode_t *const rdof_sdr_modes[] = {NULL};
#endif
