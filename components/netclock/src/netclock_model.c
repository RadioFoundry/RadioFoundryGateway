// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#include "netclock_model.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define NC_STEP_US      200000   // a reference this far off the model is an outlier...
#define NC_STEP_CONFIRM 3        // ...unless this many in a row agree: then the clock really jumped
#define NC_SLEW_FAST    500e-6   // offset corrections over NC_SLEW_BIG_US go in at 0.5 ms per s...
#define NC_SLEW_FINE    2e-6     // ...smaller ones at 2 us/s, so a 60 s interval absorbs <= 120 us
#define NC_SLEW_BIG_US  100000   // NTP over WiFi scatters by tens of ms: only a clock this far off is wrong
#define NC_PHASE_GAIN   0.3      // fraction of the fitted-line offset taken per reference (averages the fit's own noise)
#define NC_MIN_SPAN_S   600.0    // references must span this long before the rate is fitted
#define NC_MAX_AGE_S    10800.0  // and are dropped after this (crystal rate wanders with temperature;
                                 // shorter tracks that better, longer averages WiFi NTP noise better)
#define NC_MAX_RATE     500e-6   // a fit beyond this is nonsense (crystals are tens of ppm)
#define NC_RESID_MIN_US 3000.0   // never reject a reference closer than this to the fit

void nc_model_init(nc_model_t *m, double rate)
{
    memset(m, 0, sizeof(*m));
    m->rate = fabs(rate) < NC_MAX_RATE ? rate : 0;
}

static int64_t slew_applied(const nc_model_t *m, int64_t t_us)
{
    int64_t dt = t_us - m->t0;
    if (dt <= 0 || !m->slew_us) {
        return 0;
    }
    int64_t lim = (int64_t)((double)dt * (llabs(m->slew_us) > NC_SLEW_BIG_US ? NC_SLEW_FAST : NC_SLEW_FINE));
    return llabs(m->slew_us) <= lim ? m->slew_us : (m->slew_us > 0 ? lim : -lim);
}

int64_t nc_model_utc(const nc_model_t *m, int64_t t_us)
{
    int64_t dt = t_us - m->t0;
    return m->u0 + dt + (int64_t)llround((double)dt * m->rate) + slew_applied(m, t_us);
}

void nc_model_set_rate(nc_model_t *m, int64_t t_us, double rate)
{
    if (!(fabs(rate) < NC_MAX_RATE)) {  // also NaN
        return;
    }
    if (m->valid) {
        int64_t applied = slew_applied(m, t_us);
        m->u0 = nc_model_utc(m, t_us);
        m->slew_us -= applied;
        m->t0 = t_us;
    }
    m->rate = rate;
}

int64_t nc_model_slew_left(const nc_model_t *m, int64_t t_us)
{
    return m->slew_us - slew_applied(m, t_us);
}

bool nc_model_fit_resid(const nc_model_t *m, int64_t t_us, int64_t utc_us, double *resid_us)
{
    if (!m->fit_ok) {
        return false;
    }
    *resid_us = (double)(utc_us - t_us - m->fit_off) - m->fit_rate * (double)(t_us - m->fit_t);
    return true;
}

bool nc_model_in_fit(const nc_model_t *m, int64_t t_us)
{
    if (!m->fit_ok) {
        return false;
    }
    for (int j = 0; j < m->n; j++) {
        int i = (m->head - m->n + j + NC_HIST) % NC_HIST;
        if (m->ref_t[i] == t_us) {
            return m->ref_in[i];
        }
    }
    return false;
}

static void hist_clear(nc_model_t *m)
{
    m->fit_ok = false;
    m->n = 0;
    m->head = 0;
    m->big_run = 0;
}

static void hist_push(nc_model_t *m, int64_t t_us, int64_t utc_us)
{
    m->ref_t[m->head] = t_us;
    m->ref_u[m->head] = utc_us;
    m->head = (m->head + 1) % NC_HIST;
    if (m->n < NC_HIST) {
        m->n++;
    }
    // Drop references too old to say anything about today's rate.
    while (m->n > 2) {
        int oldest = (m->head - m->n + NC_HIST) % NC_HIST;
        if ((t_us - m->ref_t[oldest]) / 1e6 <= NC_MAX_AGE_S) {
            break;
        }
        m->n--;
    }
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

// Least squares of offset (utc - local) against time over the references
// marked in `use`, time in s from t_ref, offset in us relative to y0 (the raw
// offset is ~1.8e15 us: summing squares of that would lose the microseconds).
// Returns false if degenerate.
static bool fit_line(const nc_model_t *m, const bool *use, int64_t t_ref, int64_t y0, double *a, double *b)
{
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int k = 0;
    for (int j = 0; j < m->n; j++) {
        if (!use[j]) {
            continue;
        }
        int i = (m->head - m->n + j + NC_HIST) % NC_HIST;
        double x = (m->ref_t[i] - t_ref) / 1e6;
        double y = (double)(m->ref_u[i] - m->ref_t[i] - y0);
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
        k++;
    }
    double den = k * sxx - sx * sx;
    if (k < 2 || den <= 0) {
        return false;
    }
    *b = (k * sxy - sx * sy) / den;  // us per s = ppm
    *a = (sy - *b * sx) / k;         // offset at t_ref, minus y0
    return true;
}

// Fit the rate over the history, rejecting references off the line by more
// than ~3 sigma (a slow round trip). Returns false until the span is enough.
static bool fit_rate(nc_model_t *m, int64_t t_now, int64_t *offset_now)
{
    if (m->n < 3) {
        return false;
    }
    int oldest = (m->head - m->n + NC_HIST) % NC_HIST;
    double span = (t_now - m->ref_t[oldest]) / 1e6;
    if (span < NC_MIN_SPAN_S) {
        return false;
    }
    int newest = (m->head - 1 + NC_HIST) % NC_HIST;
    int64_t y0 = m->ref_u[newest] - m->ref_t[newest];
    bool use[NC_HIST];
    for (int j = 0; j < m->n; j++) {
        use[j] = true;
    }
    double a, b;
    if (!fit_line(m, use, t_now, y0, &a, &b)) {
        return false;
    }
    if (m->n >= 5) {
        double res[NC_HIST], ab[NC_HIST];
        for (int j = 0; j < m->n; j++) {
            int i = (m->head - m->n + j + NC_HIST) % NC_HIST;
            double x = (m->ref_t[i] - t_now) / 1e6;
            res[j] = (double)(m->ref_u[i] - m->ref_t[i] - y0) - (a + b * x);
            ab[j] = fabs(res[j]);
        }
        qsort(ab, m->n, sizeof(double), cmp_double);
        double lim = fmax(NC_RESID_MIN_US, 3.0 * 1.4826 * ab[m->n / 2]);
        int k = 0;
        for (int j = 0; j < m->n; j++) {
            use[j] = fabs(res[j]) <= lim;
            k += use[j];
        }
        if (k >= 3 && k < m->n && !fit_line(m, use, t_now, y0, &a, &b)) {
            return false;
        }
    }
    if (fabs(b) * 1e-6 >= NC_MAX_RATE) {
        return false;
    }
    double ss = 0, sx = 0, sxx = 0;
    int k = 0;
    for (int j = 0; j < m->n; j++) {
        int i = (m->head - m->n + j + NC_HIST) % NC_HIST;
        m->ref_in[i] = use[j];
        if (use[j]) {
            double x = (m->ref_t[i] - t_now) / 1e6;
            double r = (double)(m->ref_u[i] - m->ref_t[i] - y0) - (a + b * x);
            ss += r * r;
            sx += x;
            sxx += x * x;
            k++;
        }
    }
    double sxx_c = sxx - sx * sx / k;  // spread of the fitted references in time
    m->rate_skew = k > 2 && sxx_c > 0 ? sqrt(ss / (k - 2) / sxx_c) * 1e-6 : 0;
    m->fit_ok = true;
    m->fit_t = t_now;
    m->fit_off = y0 + (int64_t)llround(a);
    m->rate = b * 1e-6;
    m->fit_rate = m->rate;
    m->rate_learned = true;
    m->rate_n = k;
    m->rate_span_s = span;
    m->resid_rms_us = sqrt(ss / k);
    *offset_now = y0 + (int64_t)llround(a);
    return true;
}

nc_result_t nc_model_add(nc_model_t *m, int64_t t_us, int64_t utc_us, bool new_source)
{
    if (!m->valid || new_source) {
        hist_clear(m);
        m->valid = true;
        m->t0 = t_us;
        m->u0 = utc_us;
        m->slew_us = 0;
        m->last_err_us = 0;
        hist_push(m, t_us, utc_us);
        return NC_STEP;
    }
    int64_t p = nc_model_utc(m, t_us);
    int64_t err = utc_us - p;
    m->last_err_us = err;
    if (llabs(err) > NC_STEP_US) {
        if (++m->big_run < NC_STEP_CONFIRM) {
            m->outliers++;
            return NC_OUTLIER;
        }
        // Several in a row agree: the reference (or our clock) really moved.
        hist_clear(m);
        m->t0 = t_us;
        m->u0 = utc_us;
        m->slew_us = 0;
        hist_push(m, t_us, utc_us);
        return NC_STEP;
    }
    m->big_run = 0;
    hist_push(m, t_us, utc_us);
    // Steer the phase toward the fitted line (which averages out the jitter
    // of single references) once there is one, else toward the reference.
    // Phase target: the fitted line once there is one (it averages the
    // jitter), else the reference. A gross error is taken whole, the rest a
    // fraction at a time, so one noisy reference can't swing a short interval.
    int64_t off;
    int64_t to = fit_rate(m, t_us, &off) ? t_us + off - p : err;
    int64_t corr = llabs(to) > NC_SLEW_BIG_US ? to : (int64_t)llround(NC_PHASE_GAIN * (double)to);
    m->t0 = t_us;
    m->u0 = p;        // continuous at the reference...
    m->slew_us = corr;  // ...then slewed toward it
    return NC_SLEW;
}
