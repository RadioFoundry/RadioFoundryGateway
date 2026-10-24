// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * rust's modules (CONFIG_RDOF_RUST, rdof_tests_rust): the check that its struct mirrors match the C headers.
 */
#ifndef RDOF_RUST_H
#define RDOF_RUST_H

#ifdef __cplusplus
extern "C" {
#endif

/* NULL if every struct the Rust shares with C has the C's size and alignment, else the first one that doesn't. */
const char *rdof_rust_layout_mismatch(void);

#ifdef __cplusplus
}
#endif

#endif
