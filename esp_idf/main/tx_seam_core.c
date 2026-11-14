// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* Where a long key-up's seams fall (tx_seam_core.h). */
#include "tx_seam_core.h"

#include <math.h>

static int64_t mod(int64_t a, int64_t m) { return ((a % m) + m) % m; }

int64_t rdof_seam_run(double seam, int64_t max_run, int64_t period)
{
    if (period <= 0 || !(seam >= 0.0) || max_run <= 0)
        return 0;
    const int64_t skip = (int64_t)llround(seam);
    int64_t n = (max_run + skip) / period;
    while (n > 0 && n * period - skip > max_run)
        n--;
    return n > 0 && n * period - skip > 0 ? n * period - skip : 0;
}

int64_t rdof_seam_phase(int64_t start, int64_t run, int64_t period, int64_t ref)
{
    return mod(start + run - ref, period);
}

int64_t rdof_seam_start(int64_t k_go, int64_t run, int64_t period, int64_t ref, int64_t at, int64_t tol)
{
    if (period <= 0)
        return k_go;
    const int64_t ph = rdof_seam_phase(k_go, run, period, ref);
    int64_t d = mod(ph - mod(at, period), period); /* how far past at, in the period */
    if (d > period / 2)
        d -= period;
    if (d >= -tol && d <= tol)
        return k_go;
    return k_go + mod(mod(at, period) - ph, period); /* later, to at */
}

int rdof_seam_skip_us(const int gap[RDOF_SEAM_FIRST], int n, int first, int steady)
{
    if (n < 1 || n > RDOF_SEAM_FIRST)
        return steady;
    return gap && gap[n - 1] >= 0 ? gap[n - 1] : first;
}

void rdof_seam_learn(int gap[RDOF_SEAM_FIRST], const int64_t *done_us, int n, double run_us)
{
    for (int i = 1; i < n && i <= RDOF_SEAM_FIRST; i++) {
        const double a = (double)(done_us[i] - done_us[i - 1]) - run_us;
        if (!(a >= 0.0 && a < 1000.0))
            continue; /* not a seam's restart: a run lost, or the clock read wrong */
        const int g = (int)lround(a);
        gap[i - 1] = gap[i - 1] < 0 ? g : (3 * gap[i - 1] + g + 2) / 4;
    }
}
