// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Per-channel DMR burst receiver. Complex
 * baseband at 48 kS/s (10 samples per symbol) in, 33-byte bursts out.
 *
 * A squelch gate (the discriminator's spread over 48 symbols) picks where to
 * look; the matched-filter output is correlated there against the four sync
 * families (a data sync is the voice sync inverted, so one correlation covers
 * both); a hit gives the symbol timing and the 4FSK levels. Voice bursts B-F
 * have no sync: a per-slot flywheel predicts them at 60 ms steps and checks
 * each by its EMB.
 *
 * Bursts, positions, slots and error counts match the golden vectors exactly for the
 * same input fed in the same chunk sizes (the golden tests check it).
 */
#ifndef RDOF_SDR_DEMOD_H
#define RDOF_SDR_DEMOD_H

#include <stdbool.h>
#include <stdint.h>

#include "rdof/sdr/features.h"
#include "rdof/sdr/fft.h"
#include "rdof/sdr/fm.h"
#include "rdof/tracker.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_DEMOD_FS 48000.0
#define RDOF_DEMOD_SYNC_THRESHOLD 0.85
#define RDOF_DEMOD_SEARCH_STRIDE 2
#define RDOF_DEMOD_SQUELCH_HZ 1950.0
#define RDOF_DEMOD_MAX_SYNC_BIT_ERRORS 4
#define RDOF_DEMOD_WEAKER_DB 15.0 /* a voice sync this much weaker than a slot's call can't take it over */

/* One burst from the demod: what the tracker reads, plus where and how it was heard. */
typedef struct {
    rdof_burst_t b;
    int64_t position;      /* absolute sample index (48 kS/s) of the burst start */
    float freq_offset_hz;  /* center of the 4FSK levels; drives AFC */
    float deviation_hz;    /* outer (+-3) level */
} rdof_sdr_burst_t;

typedef struct {
    bool active;
    int64_t anchor;        /* burst-start sample of burst A */
    int n;                 /* last burst index delivered */
    rdof_family_t family;
    float center, outer;
    int misses;
    bool has_pending;      /* burst A, released once B's EMB checks out */
    rdof_sdr_burst_t pending;
    unsigned order;        /* insertion order: tracks are visited in it */
    double power;          /* dBFS over the middle (sync / EMB) of the last burst it delivered */
} rdof_demod_track_t;

typedef struct {
    int cc;                /* -1: any */
    int default_slot;
    double squelch_hz;     /* <= 0: no squelch */
    rdof_fm_t fm;
    float *buf, *pow;      /* matched-filter output and power, from absolute index base */
    int len, cap, max_feed;
    int64_t base, next_search;
    /* The oldest sample in use: what base would be if every feed trimmed. The samples between base and
     * lbase are left in place until the buffer is short of room, so the history moves once every few
     * feeds rather than every feed (in PSRAM that move was a third of a core at five channels). */
    int64_t lbase;
    int keep;              /* most samples in use: max_feed + RETAIN (cap has room for a few feeds more) */
    rdof_demod_track_t tracks[2];   /* slot 1, slot 2 */
    unsigned order_next;
    int last_slot;         /* 0: none */
    int64_t last_pos;
    float unit[4][24];     /* zero-mean unit-length sync templates */
    int8_t templ[4][24];   /* +-3 */
    /* scratch for one feed */
    int64_t *pos;          /* sync candidates of one search */
    int cand_cap;
    float *best;
    int8_t *best_fam;
    rdof_sdr_burst_t *out;
    int out_n, out_cap;
    int64_t samples_in;
    /* rdof_demod_skip: the last samples skipped, to warm the FM stage up with when feeding starts again */
    rdof_cf_t *tail;         /* RDOF_FM_TAPS + 1 */
    int tail_n;
    bool skipped;
} rdof_demod_t;

/* Where the demods' time goes, in a clock's ticks summed over every demod, while a clock is set (NULL: off,
 * the default). For a board's status page; nothing reads it here. */
typedef struct {
    uint64_t fm, search, fly, compact;
    uint64_t pre, sync;                /* search's steps: the squelch, prefilter and scoring; the syncs found */
    uint64_t sync_fly, at_sync;        /* of sync: the flywheel up to it, and the burst at it */
    uint64_t positions, scored, syncs; /* positions searched, scored (the prefilter let through), syncs tried */
} rdof_demod_prof_t;
extern rdof_demod_prof_t rdof_demod_prof;
void rdof_demod_set_clock(uint32_t (*ticks)(void));


/* cc: color code to accept (-1 any). max_feed: the most samples one rdof_demod_feed call gets.
 * Returns 0, or -1 out of memory (then call rdof_demod_free). */
int rdof_demod_init(rdof_demod_t *d, int cc, int default_slot, double squelch_hz, int max_feed);
void rdof_demod_free(rdof_demod_t *d);
/* Feed n (<= max_feed) samples. Returns the bursts found, sorted by position, in a buffer the demod
 * owns until the next call; *count is set. */
const rdof_sdr_burst_t *rdof_demod_feed(rdof_demod_t *d, const rdof_cf_t *iq, int n, int *count);
/* A voice call is in progress (a flywheel, or a burst A waiting for its B). */
bool rdof_demod_busy(const rdof_demod_t *d);
/* n samples (<= max_feed) with nothing in them, as the receiver's idle gate judges: they go in as zeros, with
 * no FM stage and no sync search. Only when the samples before them held nothing either (the search there
 * would have needed these), and the demod isn't busy. The next rdof_demod_feed warms the FM stage up on the
 * last of them first, so its output is what it would have been. */
void rdof_demod_skip(rdof_demod_t *d, const rdof_cf_t *iq, int n);

#ifdef __cplusplus
}
#endif

#endif
