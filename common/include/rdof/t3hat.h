// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * A Tier III carrier on a stock duplex MMDVM modem (docs/spec/fw/tier3-hat.md).
 *
 * The modem keeps its own slot clock and CACH and plays the host's bursts from a queue per timeslot, filling an empty
 * slot with its own idle burst; the host only chooses when to write. Keyed once (by a write, not DMR_START 1), it
 * stays keyed on its idle fill while the host polls GET_STATUS (rdof_t3hat_status). A receiver beside it hears the
 * carrier: its bursts give the modem's slot grid on the receiver's sample clock (rdof_t3hat_heard), the receiver's
 * clock maps to the host's (rdof_t3hat_clock), and each trunk slot k is written a lead before its air time
 * (rdof_t3hat_next): after the modem took the burst before it on that timeslot and before it takes this one (about
 * 106 ms ahead of the air). The lead also sets the AT of the CACH before the other timeslot's slot: lead_idle (late
 * in that window) leaves it idle, lead_busy (early) busy. Trunk slot k is modem slot k - d, d even; the modem's short
 * LC cycle starts where the receiver hears LCSS 1 (rdof_t3hat_lc_start). Each burst heard is matched to the one
 * written for it; a timeslot whose bursts air a slot late, or whose queue is deeper than the leads allow, is flushed
 * with DMR_ABORT before its next write (rdof_t3hat_abort).
 *
 * The simulated modem (rdof_t3hatsim) is the stock one as expected (tier3-hat.md section 4): its clock with a ppm
 * error, a queue per timeslot taken fetch_s before each slot's air time, the idle fill, the AT from the other
 * timeslot's queue, the short LC latched by its own never-reset count, its watchdog, DMR_START 1's 20-slot hold,
 * DMR_ABORT, GET_STATUS as v1.5.2 has it, and its reports of what it hears on the inbound.
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
#define RDOF_T3HAT_GRID 5  /* m: the grid (re)started; v: position, slot length (48 kS/s samples), host time (NaN) */
#define RDOF_T3HAT_PLACE 6 /* k, m: trunk slot k is modem slot m from now; v[0]: d, v[1]: the short LC's k % 4 */
#define RDOF_T3HAT_SKIP 7  /* k, ts: not written; v[0]: why (0 too late for it, 1 its timeslot airs early) */
#define RDOF_T3HAT_REKEY 8 /* the modem's status: unkeyed under the site; placed again on its next key-up */
#define RDOF_T3HAT_ABORT 9 /* k, ts: DMR_ABORT before slot k's write; v[0]: why (0 late, 1 status, 2 NAK, 3 placed, 4 asked) */

typedef struct {
    uint32_t written, matched, on_time, late, early, lost, fills, skipped, grids;
    bool placed;
    int64_t d;
    double ppm; /* the modem's slot clock against the receiver's sample clock (+: slots shorter), 0 unknown */
    double delay_min, delay_max, delay_sum; /* air time less write time of the bursts on time, s */
    /* GET_STATUS: replies, in IDLE (its watchdog fired), the transmitter off while placed (each a re-key), an
     * overflow flag; the most bursts each timeslot's queue held (since rdof_t3hat_window); DMR_ABORTs sent; writes
     * NAKed */
    uint32_t polls, idle, unkeyed, overflows, depth_max[2], aborts, naks;
    /* the modem's own inbound reports: matched to a burst the receiver placed, tagged with its timeslot or the
     * other; none matched; report time less that burst's end, s */
    uint32_t rx_n, rx_same, rx_other, rx_unmatched;
    double rx_delay_min, rx_delay_max;
    /* since the last rdof_t3hat_window, per timeslot */
    uint32_t e_n[2][5];    /* bursts matched by where they aired: -4, -2, 0, +2, +4 slots (beyond: the ends) */
    uint32_t fills_ts[2];  /* the modem's own bursts */
    uint32_t ambiguous;    /* matched among alike bursts written (the oldest taken) */
    uint32_t at_n[2][2];   /* the AT heard 0 / 1 in the CACH before each slot heard */
    uint32_t depth_n[2][4]; /* the status's queue depth 0, 1, 2, 3 or more */
    int space_min[2], space_max[2]; /* the status's free space as read */
    double ts_delay_min[2], ts_delay_max[2]; /* write to air of the bursts on their slot */
    uint32_t slc_on, slc_late, slc_null, slc_other; /* short LC cycles heard: as written, the cycle before's, Null, else */
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
/* An inbound burst the receiver placed under trunk slot k, for the modem's own report of it (rdof_t3hat_modem_rx). */
void rdof_t3hat_inbound(rdof_t3hat_t *h, int64_t k, const uint8_t burst[33]);
/* The modem's own DMR_DATA1/2 (slot its tag) at now: matched to an inbound burst placed, a cross-check of its
 * timeslot and a coarse one of the phase. Returns that burst's trunk slot, -1 none. */
int64_t rdof_t3hat_modem_rx(rdof_t3hat_t *h, double now, int slot, const uint8_t burst[33]);
/* A GET_STATUS reply: the modem's state (2 DMR, 0 IDLE, -1 not known), its flags (bit 0 the transmitter on, 2 and 3
 * an overflow), each timeslot's free space (bursts; 28 empty). */
void rdof_t3hat_status(rdof_t3hat_t *h, int state, int flags, int space1, int space2);
/* A write to timeslot slot NAKed (its queue full). */
void rdof_t3hat_nak(rdof_t3hat_t *h, int slot);
/* Start the stats' per-window detail (from e_n on, and depth_max) over. */
void rdof_t3hat_window(rdof_t3hat_t *h);
/* DMR_ABORT when a timeslot runs behind: on by default; off to measure where bursts air at a fixed lead. */
void rdof_t3hat_set_catchup(rdof_t3hat_t *h, bool on);
/* Both queues flushed (DMR_ABORT) before each timeslot's next write, catch-up on or not. */
void rdof_t3hat_flush(rdof_t3hat_t *h);
/* The short LC (DMR_SHORTLC's 9 bytes) for the modem's cycle whose LCSS 1 CACH comes before trunk slot k; a whole
 * cycle heard, its LCSS 1 CACH before modem slot m (null: the Null short LC), checked against it. */
void rdof_t3hat_slc_wrote(rdof_t3hat_t *h, int64_t k, const uint8_t s9[9]);
void rdof_t3hat_slc_heard(rdof_t3hat_t *h, int64_t m, const uint8_t s9[9], bool null);
/* Whether to write an idle burst on TS1 now to key the modem (not placed, not keyed; at most every 0.5 s). */
bool rdof_t3hat_keep(rdof_t3hat_t *h, double now);
/* The next trunk slot to write (k_min the first it may place on) and its air time; false while the grid isn't
 * known. Slots too late to make are skipped. */
bool rdof_t3hat_next(rdof_t3hat_t *h, double now, int64_t k_min, int64_t *k, double *air);
/* Whether the CACH before trunk slot k starts the modem's short LC cycle: DMR_SHORTLC goes before slot k's write. */
bool rdof_t3hat_lc_start(const rdof_t3hat_t *h, int64_t k);
/* Whether DMR_ABORT for slot k's timeslot goes just before its write (that timeslot ran behind); true once. */
bool rdof_t3hat_abort(rdof_t3hat_t *h, int64_t k);
/* Slot k's burst written at now, lead before its air time. */
void rdof_t3hat_wrote(rdof_t3hat_t *h, int64_t k, const uint8_t burst[33], double now, double lead);
/* Slot k's air time, host s (NAN before the grid is placed). */
double rdof_t3hat_air(const rdof_t3hat_t *h, int64_t k);
/* Start over (a bench job had the modem): a new grid, placed again. */
void rdof_t3hat_reset(rdof_t3hat_t *h);
void rdof_t3hat_stats(const rdof_t3hat_t *h, rdof_t3hat_stats_t *out);
size_t rdof_t3hat_stats_size(void);
bool rdof_t3hat_event(rdof_t3hat_t *h, int *kind, int *ts, int64_t *k, int64_t *m, double v[4]);

/* The simulated modem. ppm: its clock's error; fetch_s: how long before a slot's air time it takes the slot's burst
 * (about 0.106); cap: each queue's room (bursts, 0: 30). */
size_t rdof_t3hatsim_size(void);
void rdof_t3hatsim_init(rdof_t3hatsim_t *s, double ppm, double fetch_s, int cap);
/* Frames from the host (any split), sent at now: each takes effect when its last byte is in (115200 baud). */
void rdof_t3hatsim_input(rdof_t3hatsim_t *s, double now, const uint8_t *d, size_t n);
/* Its replies to the host (and what it heard), up to cap bytes; returns how many. */
size_t rdof_t3hatsim_output(rdof_t3hatsim_t *s, uint8_t *out, size_t cap);
/* An MS's burst on the inbound, its sync ending at t_sync: reported as DMR_DATA1/2 once the burst ends. */
void rdof_t3hatsim_hear(rdof_t3hatsim_t *s, double t_sync, uint8_t control, const uint8_t burst[33]);
/* Its counts: key-ups, unkeys, watchdog drops while keyed, short LC cycles latched Null while written, NAKs. */
void rdof_t3hatsim_counts(const rdof_t3hatsim_t *s, uint32_t out[5]);
/* Its clock to now. */
void rdof_t3hatsim_run(rdof_t3hatsim_t *s, double now);
/* The next slot sent by now: its modem slot, timeslot, air time, burst, whether the host's, the AT and LCSS of the
 * CACH before it (-1: none), the short LC (9 bytes, as DMR_SHORTLC took it) and which of its rows (0-3) that CACH
 * carries. */
bool rdof_t3hatsim_aired(rdof_t3hatsim_t *s, double now, int64_t *m, int *slot, double *t, uint8_t burst[33],
                         bool *host, int *at, int *lcss, uint8_t slc9[9], int *frag);

#ifdef __cplusplus
}
#endif

#endif
