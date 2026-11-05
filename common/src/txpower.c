// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* Transmit power in dBm at the antenna (rdof/txpower.h). */
#include "rdof/txpower.h"

#include <math.h>
#include <stddef.h>

bool rdof_txmap_valid(const rdof_txmap_t *m)
{
    return m && m->max_dbm > m->min_dbm;
}

int rdof_txpower_level(const rdof_txmap_t *m, double dbm)
{
    if (isnan(dbm) || !rdof_txmap_valid(m))
        return 255;
    const double l = round(255.0 * (dbm - m->min_dbm) / (m->max_dbm - m->min_dbm));
    return l < 0 ? 0 : l > 255 ? 255 : (int)l;
}

double rdof_txpower_dbm(const rdof_txmap_t *m, int level)
{
    if (!rdof_txmap_valid(m))
        return NAN;
    level = level < 0 ? 0 : level > 255 ? 255 : level;
    return m->min_dbm + (m->max_dbm - m->min_dbm) * level / 255.0;
}

rdof_txmap_t rdof_txpower_sdr_map(double device_dbm)
{
    return (rdof_txmap_t){device_dbm - RDOF_TXPOWER_SDR_SPAN_DB, device_dbm};
}

static double chip_of(const rdof_txpower_fe_t *fe, double chip)
{
    if (fe->chip_step > 0)
        chip = floor(chip / fe->chip_step + 1e-9) * fe->chip_step; /* a step down: never over what was asked */
    return chip < fe->chip_min ? fe->chip_min : chip > fe->chip_max ? fe->chip_max : chip;
}

rdof_txpower_path_t rdof_txpower_path(const rdof_txpower_fe_t *fe, double want, bool pa_ok)
{
    rdof_txpower_path_t r = {.pa = pa_ok && want > fe->pa_above};
    if (r.pa && want > fe->pa_max)
        want = fe->pa_max;
    const double gain = r.pa ? fe->pa_gain : fe->bypass_gain;
    r.chip_dbm = chip_of(fe, want - gain);
    r.out_dbm = r.chip_dbm + gain;
    return r;
}

double rdof_bench_power_dbm(const rdof_bench_limit_t *lim, rdof_bench_power_t want, double lo, double hi,
                            double *asked)
{
    const int wl = want.level < 0 ? 0 : want.level > 255 ? 255 : want.level;
    const double a = !isnan(want.dbm) ? want.dbm : lo + (hi - lo) * wl / 255.0;
    const double l = lim->has_max_dbm ? lim->max_dbm : lo + (hi - lo) * lim->rf_level / 255.0;
    if (asked)
        *asked = a;
    const double d = a < l ? a : l;
    return d < lo ? lo : d > hi ? hi : d;
}

int rdof_bench_power_level(const rdof_bench_limit_t *lim, rdof_bench_power_t want, const rdof_txmap_t *map)
{
    if (!rdof_txmap_valid(map)) {
        if (lim->has_max_dbm || !isnan(want.dbm))
            return 0;
        return want.level < lim->rf_level ? (want.level < 0 ? 0 : want.level) : lim->rf_level;
    }
    const double d = rdof_bench_power_dbm(lim, want, map->min_dbm, map->max_dbm, NULL);
    const int l = rdof_txpower_level(map, d);
    return l > 0 && rdof_txpower_dbm(map, l) > d + 1e-9 ? l - 1 : l; /* never over */
}

void rdof_txpower_range(const rdof_txpower_fe_t *fe, bool pa_ok, double *min_dbm, double *max_dbm)
{
    *min_dbm = fe->chip_min + fe->bypass_gain;
    const double pa_top = fe->chip_max + fe->pa_gain < fe->pa_max ? fe->chip_max + fe->pa_gain : fe->pa_max;
    *max_dbm = pa_ok ? pa_top : fe->chip_max + fe->bypass_gain;
}
