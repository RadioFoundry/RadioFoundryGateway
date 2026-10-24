// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Compile-time switches for common. Each defaults to on. The ESP-IDF
 * component sets them from Kconfig (CONFIG_RDOF_FEATURE_*); the host build
 * tests with all on and with all off.
 *
 * RDOF_FEATURE_TX           transmit-side burst work: regenerate, CSBK preambles (mmdvm.c)
 * RDOF_FEATURE_DATA_BLOCKS  data block decode: trellis 3/4, rate 1, CRC-9, CRC-32, confirmed blocks
 * RDOF_FEATURE_TRACKER      the call tracker (tracker.c) and the HBlink sink that feeds on it
 * RDOF_FEATURE_REPEATER     the duplex / single frequency repeater (repeater.c, duplex.c); needs RDOF_FEATURE_TX
 */
#ifndef RDOF_FEATURES_H
#define RDOF_FEATURES_H

#ifndef RDOF_FEATURE_TX
#define RDOF_FEATURE_TX 1
#endif
#ifndef RDOF_FEATURE_DATA_BLOCKS
#define RDOF_FEATURE_DATA_BLOCKS 1
#endif
#ifndef RDOF_FEATURE_TRACKER
#define RDOF_FEATURE_TRACKER 1
#endif
#ifndef RDOF_FEATURE_REPEATER
#define RDOF_FEATURE_REPEATER RDOF_FEATURE_TX
#endif

#endif
