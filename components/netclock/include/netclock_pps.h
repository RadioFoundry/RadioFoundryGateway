// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#pragma once
// A GNSS receiver's pulse per second as a clock reference. Each edge is
// stamped on the local clock; the clock as it stands (from NTP or a browser,
// so within a few hundred ms) says which UTC second the edge starts. Edges
// are checked against each other (one second apart, give or take the
// crystal) and every NC_PPS_EVERY good ones the latest becomes a reference.
// The pulse gives the phase and the rate to a few us; numbering the seconds
// needs the clock already set, or the receiver's own time for an edge
// (nc_pps_hint: from its NMEA), which then numbers the edges after it.
// Pure C (no ESP-IDF).

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NC_PPS_EVERY    20       // good edges per reference (the model's 40 references then span 13 min)
#define NC_PPS_LABEL_US 300000   // the clock must put an edge within this of a whole second
#define NC_PPS_TOL      1000e-6  // edge spacing: whole seconds within this (crystal error and stamp jitter)
#define NC_PPS_HOLD_S   60       // the pulse leads the clock until this long without a reference from it
#define NC_PPS_HINT_S   30       // a receiver's label numbers edges up to this long after its edge

typedef struct {
    int64_t last_t;    // local us of the last good edge, 0 = none
    int64_t last_sec;  // its UTC second
    int good;          // good edges in a row
    uint32_t edges, bad, unlabeled;
    int64_t hint_t;    // local us of the edge the receiver last named, 0 = none
    int64_t hint_sec;  // the UTC second it named
    uint32_t by_hint;  // edges numbered by it rather than the clock
} nc_pps_t;

void nc_pps_init(nc_pps_t *p);

// An edge at local t_us. clock_ok and clock_utc_us: the clock's UTC at t_us
// (clock_ok false: not set yet). Returns true with *ref_utc_us (the edge's
// whole second) when the edge should become a reference.
// The receiver says the edge at local edge_t_us started UTC second utc_sec.
// Later edges within NC_PPS_HINT_S are numbered from it, over the clock.
void nc_pps_hint(nc_pps_t *p, int64_t edge_t_us, int64_t utc_sec);

bool nc_pps_edge(nc_pps_t *p, int64_t t_us, bool clock_ok, int64_t clock_utc_us, int64_t *ref_utc_us);

#ifdef __cplusplus
}
#endif
