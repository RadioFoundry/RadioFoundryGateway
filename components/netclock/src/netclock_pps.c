// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#include "netclock_pps.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void nc_pps_init(nc_pps_t *p)
{
    memset(p, 0, sizeof(*p));
}

void nc_pps_hint(nc_pps_t *p, int64_t edge_t_us, int64_t utc_sec)
{
    p->hint_t = edge_t_us;
    p->hint_sec = utc_sec;
}

// The edge's second from the receiver's label: whole seconds after the named edge, by the local clock.
static bool by_hint(const nc_pps_t *p, int64_t t_us, int64_t *sec)
{
    int64_t dt = t_us - p->hint_t;
    if (!p->hint_t || dt < 0 || dt > NC_PPS_HINT_S * 1000000LL) {
        return false;
    }
    int64_t k = (dt + 500000) / 1000000;
    if (llabs(dt - k * 1000000) > NC_PPS_LABEL_US) {
        return false;
    }
    *sec = p->hint_sec + k;
    return true;
}

bool nc_pps_edge(nc_pps_t *p, int64_t t_us, bool clock_ok, int64_t clock_utc_us, int64_t *ref_utc_us)
{
    p->edges++;
    int64_t sec = 0;
    if (by_hint(p, t_us, &sec)) {
        p->by_hint++;  // the receiver's number wins over the clock's
    } else if (!clock_ok || llabs(clock_utc_us - (sec = (clock_utc_us + 500000) / 1000000) * 1000000) >
                                NC_PPS_LABEL_US) {
        // No clock, or too far from a whole second for it (or the pulse) to be trusted to name it.
        p->unlabeled++;
        p->last_t = 0;
        p->good = 0;
        return false;
    }
    if (p->last_t) {
        // Whole seconds since the last good edge, by both the local clock and the labels.
        int64_t dt = t_us - p->last_t;
        int64_t n = sec - p->last_sec;
        if (n < 1 || fabs((double)dt - (double)n * 1e6) > (double)n * 1e6 * NC_PPS_TOL) {
            // A glitch, a missed second the labels don't agree on, or a jump: start over from this edge.
            p->bad++;
            p->good = 0;
        }
    }
    p->last_t = t_us;
    p->last_sec = sec;
    if (++p->good < NC_PPS_EVERY) {
        return false;
    }
    p->good = 0;
    *ref_utc_us = sec * 1000000;
    return true;
}
