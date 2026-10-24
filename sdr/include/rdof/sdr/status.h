// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The SDR's retained status ({root}/_sdr/{name}/status) and each channel's
 * retained Receiver message ({root}/{id}/sdr/json): the receiver's status plus the source's overflows and level_status, and the
 * HBlink peers). Compact JSON (no spaces after , and :).
 */
#ifndef RDOF_SDR_STATUS_H
#define RDOF_SDR_STATUS_H

#include <stdbool.h>
#include <stddef.h>

#include "rdof/hblink.h"
#include "rdof/sdr/features.h"
#include "rdof/sdr/receiver.h"

#ifdef __cplusplus
extern "C" {
#endif

#if RDOF_FEATURE_SDR_RECEIVER

typedef struct {
    const char *name;          /* the SDR (box radio) name */
    double at;                 /* Unix time now */
    double cpu_load;           /* NAN: null */
    long dropped_chunks, overflows;
    const char *const *names;  /* channel display names (NULL entries: none) */
    int leave_out;             /* the receiver's last this many channels aren't listed */
    /* source.level_status(): has_level false leaves the keys out (a file source) */
    bool has_level;
    double clip_pct, peak_dbfs; /* NAN: null */
    bool has_gain;
    double gain_db;
    bool has_agc;
    double agc_ceiling_db, agc_min_db;
    int agc_changes;
    /* HBlink4: n_hblink peers, {channel id: connected} in channel order; 0 leaves "hblink" out */
    int n_hblink;
    const char *const *hblink_ids;
    const bool *hblink_up;
} rdof_sdr_status_t;

/* Returns the length, or 0 if it didn't fit. */
size_t rdof_sdr_status_json(const rdof_sdr_rx_t *rx, const rdof_sdr_status_t *st, char *out, size_t cap);

/* {"Receiver": {name, frequency, colorCode, source, device, tg, [latitude, longitude, height]}}.
 * cc / tg < 0: null. */
size_t rdof_sdr_receiver_json(const char *name, double freq_hz, int cc, const char *device, long tg,
                            const rdof_location_t *loc, char *out, size_t cap);

#endif

/* v rounded to ndigits decimals, as the shortest text that reads back the same, for the values the status carries (ndigits 0..6). */
void rdof_format_round(double v, int ndigits, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
