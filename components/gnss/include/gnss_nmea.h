// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#pragma once
// NMEA 0183 from a GNSS receiver: one line at a time into a running state (the
// fix, the time, position, satellites). RMC, GGA, GSA, GSV, ZDA and TXT from
// any talker (GP, GL, GA, GB, BD, GQ, GN). Pure C (no ESP-IDF).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GNSS_NMEA_MAX_LINE 100  // NMEA's 82, with room for receivers that run over
#define GNSS_NMEA_TALKERS  6    // GSV counts kept per constellation
#define GNSS_NMEA_MIN_YEAR 2025 // an earlier date is a receiver's placeholder or a week-number rollover

typedef enum {
    GNSS_NMEA_BAD = -1,  // no '$', bad checksum, too long or a field that doesn't parse
    GNSS_NMEA_OTHER = 0, // a good sentence this doesn't use
    GNSS_NMEA_RMC,
    GNSS_NMEA_GGA,
    GNSS_NMEA_GSA,
    GNSS_NMEA_GSV,
    GNSS_NMEA_ZDA,
    GNSS_NMEA_TXT,
} gnss_nmea_kind_t;

typedef struct {
    char talker[3];  // "GP", "GL", ...
    int in_view;     // from its last GSV
} gnss_nmea_talker_t;

typedef struct {
    // Time: of the last RMC, GGA or ZDA. The date comes from RMC or ZDA only.
    bool time_ok, date_ok;
    int year, month, day, hour, min;
    double sec;
    // The fix: RMC's status A, GGA's quality (0 none, 1 GPS, 2 DGPS, 4 RTK fixed, 5 RTK float, 6 estimated), GSA's
    // mode (1 none, 2 2D, 3 3D).
    bool valid;
    int quality, mode;
    int sats_used;         // GGA's count
    double hdop, pdop, vdop;  // NAN until given
    // Position (degrees, north and east positive), altitude above mean sea level and the geoid's height (m),
    // speed (knots) and course (degrees true). pos_ok: the last RMC or GGA had a fix and a position.
    bool pos_ok, alt_ok;
    double lat, lon, alt_m, geoid_m, speed_kn, course;
    gnss_nmea_talker_t talkers[GNSS_NMEA_TALKERS];
    int n_talkers;
    char txt[64];  // the last TXT (the receiver's own messages: its model, antenna state)
    uint32_t sentences, bad, other;
} gnss_nmea_t;

void gnss_nmea_init(gnss_nmea_t *n);

// One line, with or without its CR LF. Updates n and returns what it was.
gnss_nmea_kind_t gnss_nmea_line(gnss_nmea_t *n, const char *line, size_t len);

// The UTC second the last time sentence named (its whole second), when the last RMC had a fix (A) and a date from
// GNSS_NMEA_MIN_YEAR: a receiver without a fix may give a time from its own clock, possibly wrong.
bool gnss_nmea_utc(const gnss_nmea_t *n, int64_t *utc_s);

// Satellites in view, summed over the constellations' GSV.
int gnss_nmea_in_view(const gnss_nmea_t *n);

// The receiver sends one burst of sentences a second, after the second starts (and after its pulse). The burst's
// first byte is the time reference: bytes are read in chunks, so it's worked back from when a chunk ended, its size
// and the baud rate. A gap of GNSS_BURST_GAP_US starts a new burst.
#define GNSS_BURST_GAP_US 100000

typedef struct {
    int64_t start_us;    // the burst's first byte, local us; 0: none yet
    int64_t last_us;     // the last byte's
    bool told;           // its second already given
    int64_t told_s;      // the second given
    uint32_t bursts, no_gap;  // bursts seen; times seen with no gap before them (a link too slow for its sentences)
} gnss_burst_t;

void gnss_burst_init(gnss_burst_t *b);
// n bytes whose read ended at end_us, at baud (10 bits a character).
void gnss_burst_bytes(gnss_burst_t *b, int64_t end_us, size_t n, int baud);
// After each sentence: true once a burst, with its start and the UTC second the receiver gave (gnss_nmea_utc).
bool gnss_burst_time(gnss_burst_t *b, const gnss_nmea_t *n, int64_t *start_us, int64_t *utc_s);

// Days since 1970-01-01 for a civil date (any year from 1970).
int64_t gnss_days_from_civil(int y, int m, int d);

#ifdef __cplusplus
}
#endif
