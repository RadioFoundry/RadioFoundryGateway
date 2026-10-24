// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#pragma once
// What netclock reports: the tracking summary, one row per NTP server
// (chronyc sources style) and the last references, plus a JSON writer for it.
// Pure C (no ESP-IDF): the host tests build it.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "netclock_tempcal.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef NETCLOCK_MAX_SRC
#define NETCLOCK_MAX_SRC 6    // NTP servers polled (a pool name counts 3)
#endif
#define NETCLOCK_LOG 32       // references kept for the status

typedef enum {
    NETCLOCK_SRC_NONE = 0,    // never set: UTC counts from 1970 at boot
    NETCLOCK_SRC_BROWSER,     // set from a web client's clock (no NTP reachable)
    NETCLOCK_SRC_NTP,
    NETCLOCK_SRC_PPS,         // a GNSS pulse per second (seconds numbered by the receiver's NMEA, or the clock)
    NETCLOCK_SRC_GNSS,        // a GNSS receiver's NMEA time alone (no pulse): to tens of ms; above a browser, below NTP
} netclock_source_t;

const char *netclock_source_name(netclock_source_t s);  // "none", "browser", "ntp", "pps", "gnss"

// One NTP server, as `chronyc sources` shows it.
typedef struct {
    char name[40];
    char ip[16];         // dotted quad, "" until resolved
    char mark;           // '*' best, '+' combined, 'x' falseticker, '~' delay spike (skipped),
                         // '?' no sample, '-' duplicate address or unresolved
    uint8_t stratum;
    uint8_t reach;       // last 8 polls answered, newest in bit 0
    char refid[16];      // 4 ASCII chars (stratum 1) or the server's upstream, dotted
    int32_t last_rx_s;   // since its last good reply, -1 = never
    float offset_ms;     // last sample minus the clock model (+ = we are behind it)
    float delay_ms;      // round trip, less the server's processing time
    float min_delay_ms;  // quickest of its last 8 (the delay filter's yardstick)
    float lambda_ms;     // root distance used for selection
    float jitter_ms;     // std dev of its recent offsets (vs the model)
    float root_delay_ms, root_disp_ms;
    uint8_t np;          // samples in its history
    bool kod;            // it sent a kiss-o'-death; backing off
} netclock_src_view_t;

// One reference the clock took (or ignored).
typedef struct {
    int64_t t_us, utc_us;  // local time and what the reference said
    int32_t err_us;        // it minus the model, before correction
    uint8_t src;           // netclock_source_t
    uint8_t result;        // nc_result_t: 0 step, 1 slew, 2 outlier
    float rate_ppm;        // rate after it
    float die_c;           // chip temperature, NAN if unknown
    char peer[24];         // NTP: the best server that round
} netclock_ref_t;

typedef struct {
    netclock_source_t source;
    bool valid;             // some reference has set the clock
    int64_t utc_us;         // now
    int64_t ref_utc_us;     // the last accepted reference, 0 = none
    int32_t since_sync_s;   // since it, -1 = never
    bool fit_ok;
    double rate_ppm;        // local clock error being corrected (+ = local runs slow)
    const char *rate_src;   // "learned" (this run), "stored" (from the last run) or "none"
    double stored_ppm;
    double skew_ppm;        // standard error of the rate
    int refs;               // references in the fit window...
    double span_s;          // ...and the time they span
    double last_err_ms;     // last reference minus the model, before correction
    double resid_rms_ms;    // rms of the references about the fitted line
    double slew_left_ms;    // offset still being slewed in (+ = behind the reference)
    uint32_t outliers;      // references ignored as too far off
    float die_c;            // NAN if unknown
    // Temperature (netclock_tempcal.h): the rate against the die's temperature
    double temp_comp_ppm;   // what the table adds to the fitted (or prior) rate now
    double base_die_c;      // the die's temperature that rate holds at, NAN: unknown (no compensation)
    nc_tcal_t tcal;
    // NTP
    uint32_t poll_s, polls, no_majority;
    // PPS: pps_gpio -1 = none
    int pps_gpio;
    uint32_t pps_edges, pps_bad, pps_unlabeled;
    int32_t pps_last_s;     // since the last edge, -1 = never
    uint32_t pps_by_gnss;   // edges numbered by the receiver's NMEA
    // GNSS NMEA time (netclock_gnss_time)
    uint32_t gnss_times;    // seconds the receiver gave
    uint32_t gnss_refs;     // taken as references on their own (no pulse)
    float gnss_lag_ms;      // the NMEA burst's start after the pulse, NAN: not measured
    int32_t gnss_last_s;    // since the last, -1 = never
    int n_src;
    netclock_src_view_t src[NETCLOCK_MAX_SRC];
    // References, oldest first, with their residual about the current fit.
    int n_log;
    netclock_ref_t log[NETCLOCK_LOG];
    float resid_us[NETCLOCK_LOG];  // NAN: no fit
    bool in_fit[NETCLOCK_LOG];
} netclock_status_t;

// The status as JSON (the field names smartlaps' /api/clock uses, plus
// "synced" and "since_sync_s"). Returns the length, or 0 if it didn't fit
// (about 10 KB with a full log and 6 servers).
size_t netclock_status_json(const netclock_status_t *st, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
