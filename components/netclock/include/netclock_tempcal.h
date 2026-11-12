// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#pragma once
// A crystal's error against temperature, for this board's crystal (the
// clock's own rate, netclock_core.h) or another device's timed against it
// (an SDR's). Two parts, plain C (no ESP-IDF):
//
// - nc_settle_t: whether a value (a die temperature, a measured rate) has
//   settled: sampled at most every every_s, settled once its samples over
//   the last window_s, covering all of it, stay within tol. A crystal drifts
//   while its board warms (minutes, from room temperature to wherever it
//   sits): what's learned meanwhile holds only for that moment.
// - nc_tcal_t: a small table built up over runs: a settled, confident value
//   goes into the bin for the temperature it was learned at
//   (NC_TCAL_BIN_C wide), averaged with what the bin had. Asked for a
//   temperature, it interpolates between the nearest bins on either side,
//   and gives the nearest bin's value beyond them. Plain data, fixed size:
//   the caller stores it (a blob) and checks it with nc_tcal_valid on load.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NC_SETTLE_N 16

typedef struct {
    double t[NC_SETTLE_N], v[NC_SETTLE_N];
    int head, count;
    double every_s, window_s, tol;
    double last_t;
} nc_settle_t;

// window_s must fit NC_SETTLE_N samples every_s apart.
void nc_settle_init(nc_settle_t *s, double every_s, double window_s, double tol);
// A sample at t (s); NAN: none to be had, which starts the history over.
void nc_settle_add(nc_settle_t *s, double t, double v);
// The samples' spread over the window at t; NAN until they cover it.
double nc_settle_range(const nc_settle_t *s, double t);
bool nc_settle_ok(const nc_settle_t *s, double t);

#define NC_TCAL_N 24      // bins kept; when full, the least recently learned goes
#define NC_TCAL_BIN_C 2   // bin width (C)
#define NC_TCAL_WEIGHT 4  // a bin averages about this many learnings: new ones count, a bad one fades
#define NC_TCAL_VERSION 1

typedef struct {
    int8_t temp_c;   // the bin's center
    uint8_t n;       // learnings in it (saturates at 255)
    uint16_t pad;
    float v;         // the value there (ppm, say)
    uint32_t stamp;  // when it was last learned, by the table's own count
} nc_tcal_entry_t;

typedef struct {
    uint8_t version;
    uint8_t count;
    uint16_t pad;
    uint32_t stamp;  // learnings in all
    nc_tcal_entry_t e[NC_TCAL_N];  // by temperature, coldest first
} nc_tcal_t;

void nc_tcal_init(nc_tcal_t *c);
// A table as stored is usable (its version, count and order make sense).
bool nc_tcal_valid(const nc_tcal_t *c);
// The bin's center for a temperature.
int nc_tcal_bin(double temp_c);
// A settled, confident value learned at temp_c, into its bin.
void nc_tcal_put(nc_tcal_t *c, double temp_c, double v);
// What to expect at temp_c; false while the table is empty (or temp_c is NAN).
bool nc_tcal_get(const nc_tcal_t *c, double temp_c, double *v);
// temp_c's bin, NULL: none yet.
const nc_tcal_entry_t *nc_tcal_find(const nc_tcal_t *c, double temp_c);

// When each bin was last learned this run, so that a bin is learned (and the table stored) only every so often
// however the temperature moves between bins. Remembering the last bin alone isn't enough: a die that sits on
// the edge of two bins goes back and forth between them, and each change looked like a new bin (a W12 stored
// its table 1340 times in 3 hours, each store stopping its receiver for up to 50 ms, 2026-11-11).
typedef struct {
    uint32_t base_min;  // the marks count minutes from here
    uint16_t at[128];   // a bin's last learning, minutes from base_min, plus 1; 0: not this run
} nc_tcal_seen_t;

void nc_tcal_seen_init(nc_tcal_seen_t *s);
// temp_c's bin hasn't been learned this run, or not for again_s. t_s: seconds on any clock that only goes forward.
bool nc_tcal_seen_due(const nc_tcal_seen_t *s, double temp_c, double t_s, double again_s);
void nc_tcal_seen_mark(nc_tcal_seen_t *s, double temp_c, double t_s);

#ifdef __cplusplus
}
#endif
