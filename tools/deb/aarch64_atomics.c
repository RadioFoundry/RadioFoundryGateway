// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The arm64 package's outline atomics: Rust's std calls __aarch64_ldadd4_relax and the like, which libgcc has from
 * GCC 9.4 on; buster's libgcc 8 doesn't. Each one here is the plain ARMv8.0 sequence (built with
 * -mno-outline-atomics, so the compiler inlines the exclusive loads and stores), which runs on every arm64 CPU.
 *
 * The names: __aarch64_<op><bytes>_<order>. cas(expected, desired, ptr) and the others (value, ptr) return the old
 * value. ldclr clears the value's bits, ldeor flips them, ldset sets them.
 */
#include <stdint.h>

#define ORD_relax __ATOMIC_RELAXED
#define ORD_acq __ATOMIC_ACQUIRE
#define ORD_rel __ATOMIC_RELEASE
#define ORD_acq_rel __ATOMIC_ACQ_REL
/* a failed compare-and-swap can't release */
#define FAIL_relax __ATOMIC_RELAXED
#define FAIL_acq __ATOMIC_ACQUIRE
#define FAIL_rel __ATOMIC_RELAXED
#define FAIL_acq_rel __ATOMIC_ACQUIRE

#define OPS(T, N, O)                                                                                                  \
    T __aarch64_cas##N##_##O(T e, T d, T *p);                                                                         \
    T __aarch64_cas##N##_##O(T e, T d, T *p)                                                                          \
    {                                                                                                                 \
        __atomic_compare_exchange_n(p, &e, d, 0, ORD_##O, FAIL_##O);                                                  \
        return e;                                                                                                     \
    }                                                                                                                 \
    T __aarch64_swp##N##_##O(T v, T *p);                                                                              \
    T __aarch64_swp##N##_##O(T v, T *p) { return __atomic_exchange_n(p, v, ORD_##O); }                                \
    T __aarch64_ldadd##N##_##O(T v, T *p);                                                                            \
    T __aarch64_ldadd##N##_##O(T v, T *p) { return __atomic_fetch_add(p, v, ORD_##O); }                               \
    T __aarch64_ldclr##N##_##O(T v, T *p);                                                                            \
    T __aarch64_ldclr##N##_##O(T v, T *p) { return __atomic_fetch_and(p, (T)~v, ORD_##O); }                           \
    T __aarch64_ldeor##N##_##O(T v, T *p);                                                                            \
    T __aarch64_ldeor##N##_##O(T v, T *p) { return __atomic_fetch_xor(p, v, ORD_##O); }                               \
    T __aarch64_ldset##N##_##O(T v, T *p);                                                                            \
    T __aarch64_ldset##N##_##O(T v, T *p) { return __atomic_fetch_or(p, v, ORD_##O); }

#define SIZES(O) OPS(uint8_t, 1, O) OPS(uint16_t, 2, O) OPS(uint32_t, 4, O) OPS(uint64_t, 8, O)                       \
    unsigned __int128 __aarch64_cas16_##O(unsigned __int128 e, unsigned __int128 d, unsigned __int128 *p);             \
    unsigned __int128 __aarch64_cas16_##O(unsigned __int128 e, unsigned __int128 d, unsigned __int128 *p)              \
    {                                                                                                                 \
        __atomic_compare_exchange_n(p, &e, d, 0, ORD_##O, FAIL_##O);                                                  \
        return e;                                                                                                     \
    }

SIZES(relax)
SIZES(acq)
SIZES(rel)
SIZES(acq_rel)
