// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The UTC slot grid (docs/spec/slot-timing.md): checked
 * against the golden vectors in host-tests/vectors.h.
 *
 * Slot n's boundary is b(n) = n * 30 ms + offset, UTC (POSIX: at a leap second
 * every site steps together, so the grid stays common). Outbound TS1 is n
 * even. Outbound: the CACH starts at b(n), the burst at + 2.5 ms. Inbound: a
 * burst starts at b(n) + 1.25 ms and is TS2 when n is even. n is always the
 * whole 64 bits here; a radio's slot counter is n mod 2^32 (rdof/split.h).
 * Times that don't fit in int64 (before 1677, after 2262) wrap.
 *
 * Also: the slot_timing setting, the clock gate and a stream's sample clock.
 * Plain C11, no I/O.
 */
#ifndef RDOF_SLOTGRID_H
#define RDOF_SLOTGRID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/jsonp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_SLOT_NS 30000000LL
#define RDOF_CACH_NS 2500000LL
#define RDOF_IN_START_NS 1250000LL
#define RDOF_SLOT_MAX_OFFSET_US 15000

int64_t rdof_grid_boundary(int64_t n, int offset_us);
/* n, outbound slot (1 / 2) and ns since b(n) of the slot holding t_ns. */
int64_t rdof_grid_slot_at(int64_t t_ns, int offset_us, int *ts, int64_t *phase_ns);
/* The first boundary at or after t_ns: n, and b(n) in *b. */
int64_t rdof_grid_next_boundary(int64_t t_ns, int offset_us, int64_t *b);
/* An inbound (MS) burst that started at start_ns: the nearest slot n, its slot (TS2 when n is even), how far off. */
int64_t rdof_grid_inbound(int64_t start_ns, int offset_us, int *ts, int64_t *err_ns);
/* An outbound (BS) burst (its first symbol after the CACH) that started at start_ns. */
int64_t rdof_grid_outbound(int64_t start_ns, int offset_us, int *ts, int64_t *err_ns);

/* ── the setting ── */
typedef enum { RDOF_LOSS_HOLDOVER = 0, RDOF_LOSS_FREE, RDOF_LOSS_STOP } rdof_slot_loss_t;
typedef struct {
    bool utc;
    int offset_us;
    int max_err_pps_us, max_err_ntp_us;
    rdof_slot_loss_t on_loss;
    double holdover_s;
} rdof_slot_timing_t;

void rdof_slot_timing_default(rdof_slot_timing_t *t);
/* slot_timing from JSON: `own` over `base` (either -1, null or a node the parse didn't make for none), key by key
 * (a string is {"mode": it}), over the defaults. False with error set when it's refused. */
bool rdof_slot_timing_parse(const rdof_jdoc_t *doc, int base, int own, rdof_slot_timing_t *t, char *error, size_t cap);
/* "mode,offset,pps,ntp,on_loss,holdover" (SlotTiming.describe). */
size_t rdof_slot_timing_describe(const rdof_slot_timing_t *t, char *out, size_t cap);

/* ── the gate ── */
typedef enum { RDOF_GATE_FREE = 0, RDOF_GATE_ALIGNED, RDOF_GATE_HOLDOVER, RDOF_GATE_STOPPED } rdof_gate_state_t;
typedef struct {
    rdof_slot_timing_t timing;
    rdof_gate_state_t state;
    bool has_last_ok;
    double last_ok;
    char source[8];
    double err_us; /* NAN: none */
    char why[64];
} rdof_slot_gate_t;

void rdof_slot_gate_init(rdof_slot_gate_t *g, const rdof_slot_timing_t *t);
/* source: "pps", "ntp" (the ones with a bound), anything else can't time slots (NULL: "none"). err_us NAN: no
 * estimate. */
rdof_gate_state_t rdof_slot_gate_update(rdof_slot_gate_t *g, double now_s, bool synced, const char *source, double err_us);
static inline bool rdof_slot_gate_on_grid(const rdof_slot_gate_t *g)
{
    return g->state == RDOF_GATE_ALIGNED || g->state == RDOF_GATE_HOLDOVER;
}
static inline bool rdof_slot_gate_may_transmit(const rdof_slot_gate_t *g) { return g->state != RDOF_GATE_STOPPED; }
double rdof_slot_gate_holdover_age(const rdof_slot_gate_t *g, double now_s);
const char *rdof_gate_state_name(rdof_gate_state_t s);

/* ── a stream's sample clock against UTC (SampleClock) ── */
typedef struct {
    double nominal, ppm, window_s;
    bool stamped;
    bool has_anchor;
    int64_t i0, t0; /* index, UTC ns */
    bool has_err;
    int64_t err_ns;
    bool has_best;
    int64_t best_r, best_i, best_t;
    int64_t win_start, moved;
    uint32_t windows;
} rdof_sample_clock_t;

void rdof_sample_clock_init(rdof_sample_clock_t *c, double rate_hz, double window_s, bool stamped);
static inline double rdof_sample_clock_rate(const rdof_sample_clock_t *c) { return c->nominal * (1.0 + c->ppm * 1e-6); }
/* UTC ns of a sample index; false before the first stamp. */
bool rdof_sample_clock_utc(const rdof_sample_clock_t *c, double index, int64_t *t_ns);
bool rdof_sample_clock_index(const rdof_sample_clock_t *c, int64_t t_ns, double *index);
/* Samples up to end_index (exclusive) arrived at arrival_ns. */
void rdof_sample_clock_add(rdof_sample_clock_t *c, int64_t end_index, int64_t arrival_ns);

#ifdef __cplusplus
}
#endif

#endif
