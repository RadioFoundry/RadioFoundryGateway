// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * A Tier III carrier on a stock duplex MMDVM modem (docs/spec/fw/tier3-hat.md).
 *
 * The modem keeps its own slot clock and CACH and plays the host's bursts from a queue per timeslot, filling an empty
 * slot with its own idle burst; the host only chooses when to write. A receiver beside it hears the carrier: its
 * bursts give the modem's slot grid on the receiver's sample clock (rdof_t3hat_heard), the receiver's clock maps to
 * the host's (rdof_t3hat_clock), and each trunk slot k is written a lead before its air time (rdof_t3hat_next): late
 * enough that it is the only one queued when the modem takes it, which also sets the AT of the CACH before the other
 * timeslot's slot: lead_idle leaves it idle, lead_busy (a slot more) busy. Trunk slot k is modem slot k - d, d even
 * and chosen so the modem's short LC cycle starts on k % 4 == 0. Each burst heard is matched to the one written for
 * it; a timeslot whose bursts air a slot late drops a write to catch up.
 *
 * The simulated modem (rdof_t3hatsim) is the stock one as measured: its clock with a ppm error, a queue per
 * timeslot taken fetch_s before each slot's air time, the idle fill, the AT from the other timeslot's queue, the
 * short LC latched at each 4-slot cycle's start, the carrier dropped after 6 slots filled in a row.
 *
 * Rust (rust/src/t3hat.rs, the t3 feature of rdof_rust). No I/O, no allocation, no locking.
 */
#ifndef RDOF_T3HAT_H
#define RDOF_T3HAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rdof_t3hat rdof_t3hat_t;
typedef struct rdof_t3hatsim rdof_t3hatsim_t;

/* events (rdof_t3hat_event) */
#define RDOF_T3HAT_WRITE 1 /* k, ts; v: written at, lead, air time */
#define RDOF_T3HAT_AIR 2   /* k, m, ts; v: e (slots late), air time less written at, lead, the AT before it */
#define RDOF_T3HAT_FILL 3  /* m, ts: the modem's own burst (none written matched); v[0]: the AT before it */
#define RDOF_T3HAT_LOST 4  /* k, ts: written, never heard */
#define RDOF_T3HAT_GRID 5  /* m: the grid (re)started; v: position, slot length (48 kS/s samples) */
#define RDOF_T3HAT_PLACE 6 /* k, m: trunk slot k is modem slot m from now; v[0]: d */
#define RDOF_T3HAT_SKIP 7  /* k, ts: not written (too late for it, or its timeslot runs behind) */

typedef struct {
    uint32_t written, matched, on_time, late, early, lost, fills, skipped, grids;
    bool placed;
    int64_t d;
    double ppm; /* the modem's slot clock against the receiver's sample clock (+: slots shorter), 0 unknown */
    double delay_min, delay_max, delay_sum; /* air time less write time of the bursts on time, s */
} rdof_t3hat_stats_t;

size_t rdof_t3hat_size(void);
void rdof_t3hat_init(rdof_t3hat_t *h, double lead_idle_s, double lead_busy_s);
/* The leads, and the least lead a slot is still written with (less: skipped). */
void rdof_t3hat_set_leads(rdof_t3hat_t *h, double lead_idle_s, double lead_busy_s, double min_lead_s);
/* The lead for a slot whose write sets the AT of the CACH before the other timeslot's slot to at (0, 1). */
double rdof_t3hat_lead(const rdof_t3hat_t *h, int at);
/* The receiver's 48 kS/s position now48 is the host's now, s (each turn of the loop that reads it). */
void rdof_t3hat_clock(rdof_t3hat_t *h, double now, int64_t now48);
/* An outbound burst the receiver heard: its position, timeslot (the CACH's TC), the CACH's AT and LCSS (-1 none read),
 * its 33 bytes. Returns its trunk slot (-1: not placed yet, or off the grid); *m its modem slot (may be NULL). */
int64_t rdof_t3hat_heard(rdof_t3hat_t *h, int64_t pos48, int slot, int at, int lcss, const uint8_t burst[33],
                         int64_t *m);
/* The trunk slot an inbound burst at pos48 is under, -1 before the grid is placed. */
int64_t rdof_t3hat_inbound_k(const rdof_t3hat_t *h, int64_t pos48);
/* The next trunk slot to write (k_min the first it may place on) and its air time, host s; false while the grid
 * isn't known. Slots too late to make are skipped. */
bool rdof_t3hat_next(rdof_t3hat_t *h, double now, int64_t k_min, int64_t *k, double *air);
/* Whether slot k goes unwritten (its timeslot runs a slot behind): true, and it's taken as done. */
bool rdof_t3hat_dropping(rdof_t3hat_t *h, int64_t k);
/* Slot k's burst written at now, lead before its air time. */
void rdof_t3hat_wrote(rdof_t3hat_t *h, int64_t k, const uint8_t burst[33], double now, double lead);
/* Slot k's air time, host s (NAN before the grid is placed). */
double rdof_t3hat_air(const rdof_t3hat_t *h, int64_t k);
/* Start over (a bench job had the modem): a new grid, placed again. */
void rdof_t3hat_reset(rdof_t3hat_t *h);
void rdof_t3hat_stats(const rdof_t3hat_t *h, rdof_t3hat_stats_t *out);
size_t rdof_t3hat_stats_size(void);
bool rdof_t3hat_event(rdof_t3hat_t *h, int *kind, int *ts, int64_t *k, int64_t *m, double v[4]);

/* The simulated modem. ppm: its clock's error; fetch_s: how long before a slot's air time it takes the slot's burst;
 * cap: each queue's room (bursts). */
size_t rdof_t3hatsim_size(void);
void rdof_t3hatsim_init(rdof_t3hatsim_t *s, double ppm, double fetch_s, int cap);
/* Frames from the host (any split), at now. */
void rdof_t3hatsim_input(rdof_t3hatsim_t *s, double now, const uint8_t *d, size_t n);
/* Its replies to the host, up to cap bytes; returns how many. */
size_t rdof_t3hatsim_output(rdof_t3hatsim_t *s, uint8_t *out, size_t cap);
/* Its clock to now. */
void rdof_t3hatsim_run(rdof_t3hatsim_t *s, double now);
/* The next slot sent by now: its modem slot, timeslot, air time, burst, whether the host's, the AT and LCSS of the
 * CACH before it, the short LC (9 bytes, as DMR_SHORTLC took it) and which of its rows (0-3) that CACH carries. */
bool rdof_t3hatsim_aired(rdof_t3hatsim_t *s, double now, int64_t *m, int *slot, double *t, uint8_t burst[33],
                         bool *host, int *at, int *lcss, uint8_t slc9[9], int *frag);

#ifdef __cplusplus
}
#endif

#endif
