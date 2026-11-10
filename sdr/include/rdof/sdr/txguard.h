// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The transmit stream's timing through a stall. A device that takes its transmit samples in blocks (a PlutoSDR's
 * IIO buffers) plays its queue out and, when the queue runs dry, starts again with the next block: everything after
 * goes out that much later, for good (a repeater's outbound moved 4.85 ms, twice in one call, on the bench,
 * 2026-10-29). The guard keeps the time instead. The driver says when each block went in and how long the push
 * took: a push that waited found the queue full, so the queue is full again and runs dry nbuf blocks later; a push
 * that didn't adds a block to the time left. So the queue must be shorter than what the stream runs ahead (a
 * PlutoSDR: 2 blocks of 14.2 ms queued and one being filled, under rdofradio's 50 ms), or no push waits and there's
 * no deadline. When the writer has had nothing to finish a block with by rdof_txguard_deadline, it fills the block
 * with zeros (rdof_txguard_padded), and as many of the samples that come late are dropped (rdof_txguard_drop): a gap
 * on the air, the timing kept. While the caller stays that far behind, every block goes out as zeros: one that
 * fell behind for good (its receiver lost samples) says so with rdof_txguard_slipped. Portable C, no clock of its
 * own (the caller's seconds, monotonic).
 */
#ifndef RDOF_SDR_TXGUARD_H
#define RDOF_SDR_TXGUARD_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_TXGUARD_WAITED_S 0.0003 /* a push this long found the queue full */

typedef struct {
    double rate;      /* S/s */
    int block, nbuf;  /* samples a push, blocks the device's queue holds when a push returns */
    double dry_at;    /* when the queue runs out (the caller's seconds); NAN: not known yet */
    int64_t skip;     /* samples still to drop for zeros sent in their place */
    int64_t padded;   /* zeros sent in all */
    uint32_t pads;    /* blocks finished with zeros */
    uint32_t dry;     /* pushes after the queue had run out: a slip the guard didn't catch */
} rdof_txguard_t;

void rdof_txguard_init(rdof_txguard_t *g, double rate, int block, int nbuf);
/* The driver: a block pushed, the push from t0 to t1. */
void rdof_txguard_pushed(rdof_txguard_t *g, double t0, double t1);
/* The writer: a block must go in by then, margin_s before the queue runs out (NAN: no deadline yet). */
double rdof_txguard_deadline(const rdof_txguard_t *g, double margin_s);
/* The writer: n zeros sent to finish a block; as many samples that follow are dropped. */
void rdof_txguard_padded(rdof_txguard_t *g, int n);
/* The writer: of the next n samples, how many to drop (they're taken off the skip). */
int rdof_txguard_drop(rdof_txguard_t *g, int n);
/* The caller: its stream fell n samples behind for good (the receiver that paces it lost them): up to n of the
 * zeros sent stood for those, not for samples still to come, so as many fewer are dropped. Without it, a caller
 * that far behind never catches up, and every block from then on goes out as zeros. */
void rdof_txguard_slipped(rdof_txguard_t *g, int64_t n);

#ifdef __cplusplus
}
#endif

#endif
