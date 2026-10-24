// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Where the sample stream lost bytes, so the reader can put filler in their
 * place and the receiver keeps its timeline: a voice superframe's flywheel
 * then misses only the bursts that were really lost, rather than the rest of
 * the superframe on every channel. Portable C; the caller locks.
 *
 * The writer notes each dropped transfer at its stream offset (bytes written
 * before it). The reader asks how much to take next: data up to the next gap,
 * then the gap's filler. Back-to-back drops merge; a gap longer than max_fill
 * is filled only that far (a long stall can't be bridged, and filling it all
 * would only put the reader further behind).
 */
#ifndef RTLSDR_GAPS_H
#define RTLSDR_GAPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTL_GAPS_MAX 16

typedef struct {
    uint64_t at[RTL_GAPS_MAX];  /* stream offset of the gap: bytes written before it */
    uint32_t len[RTL_GAPS_MAX]; /* filler still to give */
    int head, count;
    uint32_t max_fill;
    uint64_t lost, filled;      /* bytes dropped, bytes of filler given */
} rtl_gaps_t;

void rtl_gaps_reset(rtl_gaps_t *g, uint32_t max_fill);
/* The writer dropped len bytes after writing `at` bytes in all. */
void rtl_gaps_note(rtl_gaps_t *g, uint64_t at, uint32_t len);
/* The reader has taken `pos` bytes of data from the ring (filler not counted) and wants up to `want` more: how
 * many to take, and whether they are filler (true) or data from the ring (false). */
size_t rtl_gaps_next(rtl_gaps_t *g, uint64_t pos, size_t want, bool *fill);
/* The reader took n bytes of filler (n <= what rtl_gaps_next allowed). */
void rtl_gaps_took(rtl_gaps_t *g, size_t n);

#ifdef __cplusplus
}
#endif

#endif
