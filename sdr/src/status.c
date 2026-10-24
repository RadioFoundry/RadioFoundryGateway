// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The SDR status and Receiver messages (rdof/sdr/status.h). */
#include "rdof/sdr/status.h"

#if RDOF_FEATURE_SDR

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/json.h"
#include "rdof/status.h"
#include "rdof/tracker.h"

void rdof_format_round(double v, int ndigits, char *out, size_t cap)
{
    /* printf rounds the exact binary value half to even */
    char tmp[64];
    snprintf(tmp, sizeof tmp, "%.*f", ndigits, v);
    rdof_format_repr(strtod(tmp, NULL), out, cap);
}

#if RDOF_FEATURE_SDR_RECEIVER

static void jround(rdof_json_t *j, const char *key, double v, int nd)
{
    char s[64];
    if (isnan(v)) {
        rdof_json_raw(j, key, "null");
        return;
    }
    rdof_format_round(v, nd, s, sizeof s);
    rdof_json_raw(j, key, s);
}

static void jrepr(rdof_json_t *j, const char *key, double v)
{
    char s[64];
    rdof_format_repr(v, s, sizeof s);
    rdof_json_raw(j, key, s);
}


size_t rdof_sdr_status_json(const rdof_sdr_rx_t *rx, const rdof_sdr_status_t *st, char *out, size_t cap)
{
    rdof_json_t j;
    char ts[32];
    rdof_json_init(&j, out, cap);
    rdof_json_open(&j, NULL);
    rdof_json_str(&j, "state", "online");
    rdof_json_str(&j, "name", st->name);
    rdof_iso_time(st->at, ts);
    rdof_json_str(&j, "at", ts);
    jrepr(&j, "centerHz", rx->center_hz);
    jrepr(&j, "sampleRate", rx->cfg.sample_rate);
    jround(&j, "cpuLoad", st->cpu_load, 3);
    rdof_json_int(&j, "workers", 0);
    rdof_json_int(&j, "droppedChunks", st->dropped_chunks);
    /* "channels": a list, which rdof_json has no call for: written raw */
    rdof_json_raw(&j, "channels", "[");
    const int nc = st->leave_out > 0 ? rx->C - st->leave_out : rx->C;
    for (int c = 0; c < nc; c++) {
        const rdof_tracker_t *tr = &rx->trackers[c];
        char item[512], v[64];
        rdof_json_t k;
        rdof_json_init(&k, item, sizeof item);
        rdof_json_open(&k, NULL);
        rdof_json_str(&k, "id", tr->channel);
        rdof_json_str(&k, "name", st->names && st->names[c] ? st->names[c] : "");
        rdof_format_repr(tr->freq_hz, v, sizeof v);
        rdof_json_raw(&k, "freqHz", v);
        rdof_json_int(&k, "bursts", rx->bursts[c]);
        if (rx->last_burst[c] != 0.0) {
            rdof_iso_time(rx->last_burst[c], ts);
            rdof_json_str(&k, "lastBurstAt", ts);
        } else {
            rdof_json_raw(&k, "lastBurstAt", "null");
        }
        if (rx->rssi[c] != INT32_MIN)
            rdof_json_int(&k, "rssi", rx->rssi[c]);
        else
            rdof_json_raw(&k, "rssi", "null");
        jround(&k, "afcHz", rx->chz.correction_hz[c], 1);
        rdof_json_close(&k);
        if (!rdof_json_done(&k))
            return 0;
        /* append to the list by hand: the writer's raw value is the item text */
        size_t need = strlen(item) + 2;
        if (j.len + need >= j.cap) {
            j.overflow = true;
            break;
        }
        if (c)
            j.buf[j.len++] = ',';
        memcpy(j.buf + j.len, item, strlen(item));
        j.len += strlen(item);
        j.buf[j.len] = 0;
    }
    if (j.len + 1 < j.cap) {
        j.buf[j.len++] = ']';
        j.buf[j.len] = 0;
    } else {
        j.overflow = true;
    }
    rdof_json_int(&j, "overflows", st->overflows);
    if (st->has_level) {
        jround(&j, "clipPct", st->clip_pct, 3);
        jround(&j, "peakDbfs", st->peak_dbfs, 1);
        if (st->has_gain)
            jround(&j, "gainDb", st->gain_db, 1);
        if (st->has_agc) {
            rdof_json_open(&j, "agc");
            jrepr(&j, "ceilingDb", st->agc_ceiling_db);
            jrepr(&j, "minDb", st->agc_min_db);
            rdof_json_int(&j, "changes", st->agc_changes);
            rdof_json_close(&j);
        }
    }
    if (st->n_hblink > 0) {
        rdof_json_open(&j, "hblink");
        for (int i = 0; i < st->n_hblink; i++)
            rdof_json_bool(&j, st->hblink_ids[i], st->hblink_up[i]);
        rdof_json_close(&j);
    }
    rdof_json_close(&j);
    return rdof_json_done(&j) ? j.len : 0;
}

size_t rdof_sdr_receiver_json(const char *name, double freq_hz, int cc, const char *device, long tg,
                            const rdof_location_t *loc, char *out, size_t cap)
{
    rdof_json_t j;
    char f[32];
    rdof_json_init(&j, out, cap);
    rdof_json_open(&j, NULL);
    rdof_json_open(&j, "Receiver");
    rdof_json_str(&j, "name", name);
    snprintf(f, sizeof f, "%.5f", freq_hz / 1e6);
    rdof_json_str(&j, "frequency", f);
    if (cc >= 0)
        rdof_json_int(&j, "colorCode", cc);
    else
        rdof_json_raw(&j, "colorCode", "null");
    rdof_json_str(&j, "source", "sdr");
    rdof_json_str(&j, "device", device);
    if (tg >= 0)
        rdof_json_int(&j, "tg", tg);
    else
        rdof_json_raw(&j, "tg", "null");
    if (loc) {
        jrepr(&j, "latitude", loc->latitude);
        jrepr(&j, "longitude", loc->longitude);
        rdof_json_int(&j, "height", loc->height_m);
    }
    rdof_json_close(&j);
    rdof_json_close(&j);
    return rdof_json_done(&j) ? j.len : 0;
}

#endif
#endif
