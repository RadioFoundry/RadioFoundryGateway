// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Compile-time switches for sdr, the SDR receive path. Each defaults to on.
 * The ESP-IDF component sets them from Kconfig (CONFIG_RDOF_FEATURE_SDR*); the
 * host build tests with all on and with all off.
 *
 * RDOF_FEATURE_SDR              the whole SDR core; off, the library builds empty
 * RDOF_FEATURE_SDR_AGC          the front-end gain control law and level stats (agc.c)
 * RDOF_FEATURE_SDR_BUILTIN_FFT  the built-in mixed-radix FFT; off when the platform
 *                             supplies rdof_fft_* itself (esp-dsp on the P4)
 * RDOF_FEATURE_SDR_RECEIVER     the multi-channel receiver (receiver.c): channelizer,
 *                             demods, AFC and a call tracker per channel; needs
 *                             RDOF_FEATURE_TRACKER from common
 *
 * RDOF_SDR_FAST_ATAN2 is not a feature but a choice: the discriminator's
 * polynomial atan2, or libm's atan2f when 0.
 */
#ifndef RDOF_SDR_FEATURES_H
#define RDOF_SDR_FEATURES_H

#include "rdof/features.h"

#ifndef RDOF_FEATURE_SDR
#define RDOF_FEATURE_SDR 1
#endif
#ifndef RDOF_FEATURE_SDR_AGC
#define RDOF_FEATURE_SDR_AGC 1
#endif
#ifndef RDOF_FEATURE_SDR_BUILTIN_FFT
#define RDOF_FEATURE_SDR_BUILTIN_FFT 1
#endif
#ifndef RDOF_FEATURE_SDR_RECEIVER
#define RDOF_FEATURE_SDR_RECEIVER RDOF_FEATURE_TRACKER
#endif
#if RDOF_FEATURE_SDR_RECEIVER && !RDOF_FEATURE_TRACKER
#error "RDOF_FEATURE_SDR_RECEIVER needs RDOF_FEATURE_TRACKER"
#endif
#ifndef RDOF_SDR_FAST_ATAN2
#define RDOF_SDR_FAST_ATAN2 1
#endif

#endif
