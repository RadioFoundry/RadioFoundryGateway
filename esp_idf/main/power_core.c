// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The power key and the battery's numbers (power_core.h). */
#include "power_core.h"

#include <string.h>

void rdof_pk_init(rdof_pk_t *k, int off_ms, int on_ms, bool woke_by_key, int64_t now_ms)
{
    memset(k, 0, sizeof *k);
    k->off_ms = off_ms, k->on_ms = on_ms;
    k->waking = woke_by_key;
    k->down = woke_by_key;
    k->since_ms = now_ms;
    k->up_ms = -1;
}

rdof_pk_event_t rdof_pk_step(rdof_pk_t *k, bool down, int64_t now_ms)
{
    if (down) {
        k->up_ms = -1;
        if (!k->down)
            k->down = true, k->since_ms = now_ms;
    } else {
        if (k->up_ms < 0)
            k->up_ms = now_ms;
        if (k->down && now_ms - k->up_ms >= RDOF_PK_DEBOUNCE_MS)
            k->down = false;
    }
    if (k->waking) {
        if (!k->down)
            return RDOF_PK_SLEEP;
        if (now_ms - k->since_ms < k->on_ms)
            return RDOF_PK_NONE;
        k->waking = false; /* on; the key must be let go before a hold turns it off */
        return RDOF_PK_ON;
    }
    if (!k->down) {
        k->armed = true, k->fired = false;
        return RDOF_PK_NONE;
    }
    if (k->armed && !k->fired && now_ms - k->since_ms >= k->off_ms) {
        k->fired = true;
        return RDOF_PK_OFF;
    }
    return RDOF_PK_NONE;
}

int rdof_batt_mv(int adc_mv, int ratio_x1000) { return (int)(((int64_t)adc_mv * ratio_x1000 + 500) / 1000); }

int rdof_batt_percent(int mv)
{
    static const struct {
        int mv, pct;
    } t[] = {{4200, 100}, {4100, 90}, {4000, 78}, {3900, 65}, {3800, 50}, {3750, 40}, {3700, 30},
             {3650, 20},  {3600, 12}, {3500, 6},  {3400, 3},  {3300, 0}};
    const int n = (int)(sizeof t / sizeof t[0]);
    if (mv < RDOF_BATT_PRESENT_MV)
        return -1;
    if (mv >= t[0].mv)
        return 100;
    for (int i = 1; i < n; i++)
        if (mv >= t[i].mv)
            return t[i].pct + (mv - t[i].mv) * (t[i - 1].pct - t[i].pct) / (t[i - 1].mv - t[i].mv);
    return 0;
}

bool rdof_batt_low_step(rdof_batt_low_t *l, int mv, int cutoff_mv, int n)
{
    if (mv < RDOF_BATT_PRESENT_MV || mv >= cutoff_mv)
        l->under = 0;
    else if (l->under < n)
        l->under++;
    return l->under >= n;
}
