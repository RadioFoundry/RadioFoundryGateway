// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#pragma once
// The clock itself without the platform: the model, which source it follows
// (NTP wins over a browser), when to store the learned rate, and the log of
// references. The ESP-IDF side (netclock.c) holds one of these behind a lock.
// Pure C (no ESP-IDF).

#include <stdbool.h>
#include <stdint.h>

#include "netclock_model.h"
#include "netclock_pps.h"
#include "netclock_status.h"
#include "netclock_tempcal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NC_RATE_SAVE_MIN_N   8        // store a learned rate once it rests on this many references
#define NC_RATE_SAVE_DIFF    0.3e-6   // ...and moved this much from the stored one
#define NC_RATE_SAVE_EVERY_S 3600     // ...at most this often (flash wear)

// The crystal's rate against the die's temperature (netclock_tempcal.h): a
// fitted rate goes into the table when it's this good...
#define NC_TCAL_SKEW     0.3e-6
#define NC_TCAL_DIE_C    2.0     // ...and the die held within this over its references
#define NC_TCAL_MIN_REFS 4       // (as many with a temperature, at least)
#define NC_TCAL_AGAIN_S  7200    // the same bin again in one run at most this often

typedef struct {
    nc_model_t m;
    netclock_source_t source;
    int64_t last_sync_us;       // local time of the last accepted reference, 0 = never
    double rate_stored;         // what storage holds (0 = nothing)
    bool rate_from_store;
    int64_t rate_saved_us;
    netclock_ref_t log[NETCLOCK_LOG];  // ring by log_n
    uint32_t log_n;
    int64_t ref_utc_us;

    // Temperature: the table, and the rate the model would run at without it
    // (the last fit's, else the prior) with the die's temperature it holds at.
    nc_tcal_t tcal;
    double base_rate;
    double base_die_c;          // NAN: unknown (no compensation)
    double comp;                // what the table adds to base_rate now (s/s)
    float die_c;                // the last reading
    int tcal_bin;               // the bin last learned this run, and when
    int64_t tcal_us;
    bool tcal_changed;          // learned since it was stored
} nc_clock_t;

// `stored_rate`: the rate saved by the last run (have_stored false: none).
void nc_clock_init(nc_clock_t *c, double stored_rate, bool have_stored);
// The temperature table saved by the last run (ignored unless nc_tcal_valid)
// and the die's temperature now: the prior becomes what the table expects
// there. Call before the first reference.
void nc_clock_set_tcal(nc_clock_t *c, const nc_tcal_t *t, float die_c);
// The die read die_c at local t_us: the rate follows the table between
// references (every half minute or so; nc_clock_add does it too).
void nc_clock_temp(nc_clock_t *c, int64_t t_us, float die_c);

// A reference from src: the clock read utc_us at local t_us. *store_rate is
// set when the learned rate, or the table (c->tcal, c->tcal_changed), should
// be saved now (then call nc_clock_stored).
nc_result_t nc_clock_add(nc_clock_t *c, netclock_source_t src, int64_t t_us, int64_t utc_us, const char *peer,
                         float die_c, bool *store_rate);
void nc_clock_stored(nc_clock_t *c, double rate, int64_t t_us);

// Whether a reference from src at local t_us should be taken: a pulse per
// second leads (until NC_PPS_HOLD_S without one), then NTP, then a browser.
bool nc_clock_takes(const nc_clock_t *c, netclock_source_t src, int64_t t_us);

// UTC us at local t_us; false (and *utc_us untouched) before any reference.
bool nc_clock_utc(const nc_clock_t *c, int64_t t_us, int64_t *utc_us);
// utc_us minus the model at t_us; false before any reference.
bool nc_clock_offset(const nc_clock_t *c, int64_t t_us, int64_t utc_us, double *off_us);
// The rate rests on enough references that polling can slow down.
bool nc_clock_rate_settled(const nc_clock_t *c);
// The rate being corrected (s/s, + = local runs slow) and its standard error
// (0 when not fitted this run). *learned: fitted from this run's references,
// else the stored one. False while there's neither.
bool nc_clock_rate(const nc_clock_t *c, double *rate, double *skew, bool *learned);
// How far off UTC may be at local now_us, us: the fit's rms about its line, an offset still slewing in, and the
// rate's standard error over the time since the last reference. False before any reference.
bool nc_clock_quality(const nc_clock_t *c, int64_t now_us, double *err_us);

// The tracking fields and the log of *st (not the NTP ones, not die_c).
void nc_clock_status(const nc_clock_t *c, int64_t now_us, netclock_status_t *st);

#ifdef __cplusplus
}
#endif
