// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The transmitter pool (docs/spec/fw/transmitters.md §3, §4): which transmitter sends a stream on a path. Each
 * transmitter reaches a set of frequencies and is free, sending one stream on one path, or holding that path after
 * its stream ended, for answer_s, so a reply goes out where the call was. A request (stream S on path P, at
 * frequency F) takes, in order:
 *
 *   1. the transmitter already sending S on P;
 *   2. one holding P, when S answers what it held for (S's dst is that stream's src, or both are the same group);
 *   3. a free one that reaches F (a hold that's over is free);
 *   4. one holding another path, when S is urgent (an emergency or priority stream) or may take a hold (a
 *      confirmed-data answer);
 *   5. one sending a stream that isn't urgent, when S is urgent: that stream is preempted.
 *
 * A stream ends at its terminator (rdof_txpool_end) or after idle_s with no burst; either way the transmitter then
 * holds its path. The caller says what to do when nothing is found (a network stream is dropped, an app waits).
 * Portable C, no allocation, no locking: call it from one thread.
 */
#ifndef RDOF_TXPOOL_H
#define RDOF_TXPOOL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_TXPOOL_MAX 4

typedef enum { RDOF_TXP_FREE, RDOF_TXP_SENDING, RDOF_TXP_HOLDING } rdof_txp_state_t;

/* What a request is for: the stream, its addresses and where it goes. */
typedef struct {
    uint32_t stream; /* nonzero */
    uint32_t src, dst;
    bool group;
    int path, freq;  /* the talk path, and its transmit frequency's index (bit in a transmitter's freqs) */
    bool urgent;     /* emergency or priority: takes a hold anywhere, and preempts a stream that isn't urgent */
    bool take_hold;  /* may take a transmitter holding another path (a confirmed-data answer) */
} rdof_txp_req_t;

typedef struct {
    uint64_t freqs;       /* the frequencies it reaches (bit per frequency) */
    double answer_s;      /* how long it holds a path after a stream */
    rdof_txp_state_t state;
    rdof_txp_req_t on;    /* the stream sending, or the one it holds for */
    double last;          /* that stream's last burst */
    double until;         /* holding: when the hold ends */
} rdof_txp_tx_t;

typedef struct {
    rdof_txp_tx_t tx[RDOF_TXPOOL_MAX];
    int n;
    double idle_s;        /* a stream with no burst this long has ended */
    unsigned preempted, held_taken; /* streams preempted; holds taken for another path */
} rdof_txpool_t;

/* What a take found. */
typedef struct {
    int i;           /* the transmitter, -1 none */
    bool fresh;      /* a stream new to it: tune it (the path's frequency, cc, power, slot) */
    bool preempt;    /* it took a transmitter from another stream: end that one first */
    uint32_t ended;  /* that stream (preempted, or the one whose hold was taken), 0 none */
    int ended_path;
} rdof_txp_take_t;

void rdof_txpool_init(rdof_txpool_t *p, double idle_s);
/* A transmitter reaching freqs (0: every frequency), holding answer_s after a stream. Its index, -1 no room. */
int rdof_txpool_add(rdof_txpool_t *p, uint64_t freqs, double answer_s);
/* The transmitter for r at now (the rules above). A found one is marked sending r, its last burst now. */
rdof_txp_take_t rdof_txpool_take(rdof_txpool_t *p, const rdof_txp_req_t *r, double now);
/* What take would find, changing nothing. */
rdof_txp_take_t rdof_txpool_peek(const rdof_txpool_t *p, const rdof_txp_req_t *r, double now);
/* Transmitter i sent a burst of its stream at now (keeps it from ending idle). */
void rdof_txpool_burst(rdof_txpool_t *p, int i, double now);
/* Transmitter i's stream ended (its terminator) at now: it holds the path for its answer_s. */
void rdof_txpool_end(rdof_txpool_t *p, int i, double now);
/* Transmitter i's state at now (a stream gone idle is holding; a hold that's over is free). */
rdof_txp_state_t rdof_txpool_state(rdof_txpool_t *p, int i, double now);
/* Whether any transmitter reaches freq (a path's tx_ok, busy or not). */
bool rdof_txpool_reaches(const rdof_txpool_t *p, int freq);
const char *rdof_txpool_state_name(rdof_txp_state_t s);

#ifdef __cplusplus
}
#endif

#endif
