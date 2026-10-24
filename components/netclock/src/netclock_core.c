// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
// The clock without the platform (see netclock_core.h). Ported from
// smartlaps' timebase.c, less NVS, locks and logging.
#include "netclock_core.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

void nc_clock_init(nc_clock_t *c, double stored_rate, bool have_stored)
{
    memset(c, 0, sizeof(*c));
    c->rate_stored = have_stored ? stored_rate : 0;
    c->rate_from_store = have_stored;
    nc_model_init(&c->m, c->rate_stored);
    nc_tcal_init(&c->tcal);
    c->base_rate = c->m.rate;
    c->base_die_c = NAN;
    c->die_c = NAN;
    c->tcal_bin = INT32_MIN;
}

void nc_clock_set_tcal(nc_clock_t *c, const nc_tcal_t *t, float die_c)
{
    double ppm;
    if (!nc_tcal_valid(t)) {
        return;
    }
    c->tcal = *t;
    if (c->m.valid || !nc_tcal_get(&c->tcal, die_c, &ppm)) {
        return;
    }
    nc_model_init(&c->m, ppm * 1e-6);
    c->rate_from_store = true;
    c->base_rate = c->m.rate;
    c->base_die_c = die_c;
    c->die_c = die_c;
}

void nc_clock_temp(nc_clock_t *c, int64_t t_us, float die_c)
{
    c->die_c = die_c;
    double now, then;
    c->comp = 0;
    if (!isnan(c->base_die_c) && nc_tcal_get(&c->tcal, die_c, &now) && nc_tcal_get(&c->tcal, c->base_die_c, &then)) {
        c->comp = (now - then) * 1e-6;
    }
    double rate = c->base_rate + c->comp;
    if (fabs(rate - c->m.rate) > 1e-10) {
        nc_model_set_rate(&c->m, t_us, rate);
    }
}

// The die over the last fit's references (from the log): their mean and spread, false if too few had one.
static bool fit_die(const nc_clock_t *c, double *mean, double *spread)
{
    const nc_model_t *m = &c->m;
    int64_t from = m->ref_t[(m->head - m->n + NC_HIST) % NC_HIST];
    int n = c->log_n < NETCLOCK_LOG ? (int)c->log_n : NETCLOCK_LOG, k = 0;
    double sum = 0, lo = INFINITY, hi = -INFINITY;
    for (int j = 0; j < n; j++) {
        const netclock_ref_t *e = &c->log[(c->log_n - n + j) % NETCLOCK_LOG];
        if (e->t_us < from || e->result == NC_OUTLIER || isnan(e->die_c)) {
            continue;
        }
        sum += e->die_c;
        lo = fmin(lo, e->die_c);
        hi = fmax(hi, e->die_c);
        k++;
    }
    if (k < NC_TCAL_MIN_REFS) {
        return false;
    }
    *mean = sum / k;
    *spread = hi - lo;
    return true;
}

// A fit just made: what it holds at, and into the table when it's good and the die held.
static void fit_learn(nc_clock_t *c, int64_t t_us)
{
    const nc_model_t *m = &c->m;
    double mean = NAN, spread = NAN;
    bool die = fit_die(c, &mean, &spread);
    c->base_rate = m->fit_rate;
    c->base_die_c = die && spread <= NC_TCAL_DIE_C ? mean : NAN;
    if (isnan(c->base_die_c) || m->rate_n < NC_RATE_SAVE_MIN_N || m->rate_skew > NC_TCAL_SKEW) {
        return;
    }
    int bin = nc_tcal_bin(mean);
    if (bin == c->tcal_bin && t_us - c->tcal_us < NC_TCAL_AGAIN_S * 1000000LL) {
        return;
    }
    nc_tcal_put(&c->tcal, mean, m->fit_rate * 1e6);
    c->tcal_bin = bin;
    c->tcal_us = t_us;
    c->tcal_changed = true;
}

nc_result_t nc_clock_add(nc_clock_t *c, netclock_source_t src, int64_t t_us, int64_t utc_us, const char *peer,
                         float die_c, bool *store_rate)
{
    nc_result_t r = nc_model_add(&c->m, t_us, utc_us, src != c->source);
    const nc_model_t *m = &c->m;
    netclock_ref_t *e = &c->log[c->log_n++ % NETCLOCK_LOG];
    *e = (netclock_ref_t){
        .t_us = t_us, .utc_us = utc_us, .err_us = (int32_t)(r == NC_STEP ? 0 : m->last_err_us),
        .src = (uint8_t)src, .result = (uint8_t)r, .rate_ppm = (float)(m->rate * 1e6), .die_c = die_c,
    };
    snprintf(e->peer, sizeof(e->peer), "%s", peer ? peer : "");
    c->source = src;
    if (r != NC_OUTLIER) {
        c->ref_utc_us = utc_us;
        c->last_sync_us = t_us;
    }
    if (m->fit_ok && m->fit_t == t_us) {
        fit_learn(c, t_us);
    }
    nc_clock_temp(c, t_us, die_c);
    if (store_rate) {
        *store_rate = (m->rate_learned && m->rate_n >= NC_RATE_SAVE_MIN_N &&
                       fabs(m->fit_rate - c->rate_stored) > NC_RATE_SAVE_DIFF &&
                       (!c->rate_saved_us || t_us - c->rate_saved_us > NC_RATE_SAVE_EVERY_S * 1000000LL)) ||
                      c->tcal_changed;
    }
    return r;
}

static int rank(netclock_source_t s)
{
    return s == NETCLOCK_SRC_PPS ? 4 : s == NETCLOCK_SRC_NTP ? 3 : s == NETCLOCK_SRC_GNSS ? 2
         : s == NETCLOCK_SRC_BROWSER ? 1 : 0;
}

bool nc_clock_takes(const nc_clock_t *c, netclock_source_t src, int64_t t_us)
{
    if (rank(src) >= rank(c->source)) {
        return true;
    }
    // A lower source than the one leading: only once the pulse has gone quiet (NTP, or the receiver's NMEA time
    // alone). A browser never displaces NTP or GNSS, nor GNSS NTP.
    return c->source == NETCLOCK_SRC_PPS && (src == NETCLOCK_SRC_NTP || src == NETCLOCK_SRC_GNSS) &&
           t_us - c->last_sync_us > NC_PPS_HOLD_S * 1000000LL;
}

void nc_clock_stored(nc_clock_t *c, double rate, int64_t t_us)
{
    c->rate_stored = rate;
    c->rate_saved_us = t_us;
    c->tcal_changed = false;
}

bool nc_clock_utc(const nc_clock_t *c, int64_t t_us, int64_t *utc_us)
{
    if (!c->m.valid) {
        return false;
    }
    *utc_us = nc_model_utc(&c->m, t_us);
    return true;
}

bool nc_clock_offset(const nc_clock_t *c, int64_t t_us, int64_t utc_us, double *off_us)
{
    if (!c->m.valid) {
        return false;
    }
    *off_us = (double)(utc_us - nc_model_utc(&c->m, t_us));
    return true;
}

bool nc_clock_rate_settled(const nc_clock_t *c)
{
    return c->m.rate_learned && c->m.rate_n >= NC_RATE_SAVE_MIN_N;
}

bool nc_clock_rate(const nc_clock_t *c, double *rate, double *skew, bool *learned)
{
    *learned = c->m.rate_learned;
    if (!*learned && !c->rate_from_store) {
        return false;
    }
    *rate = c->m.rate;
    *skew = *learned ? c->m.rate_skew : 0;
    return true;
}

bool nc_clock_quality(const nc_clock_t *c, int64_t now_us, double *err_us)
{
    if (c->source == NETCLOCK_SRC_NONE || !c->last_sync_us) {
        return false;
    }
    const double since = (double)(now_us - c->last_sync_us), slew = (double)nc_model_slew_left(&c->m, now_us);
    *err_us = c->m.resid_rms_us + fabs(slew) + (c->m.rate_learned ? c->m.rate_skew : 1e-6) * (since > 0 ? since : 0);
    return true;
}

void nc_clock_status(const nc_clock_t *c, int64_t now_us, netclock_status_t *st)
{
    const nc_model_t *m = &c->m;
    st->source = c->source;
    st->valid = m->valid;
    st->ref_utc_us = c->ref_utc_us;
    st->since_sync_s = c->last_sync_us ? (int32_t)((now_us - c->last_sync_us) / 1000000) : -1;
    st->fit_ok = m->fit_ok;
    st->rate_ppm = m->rate * 1e6;
    st->rate_src = m->rate_learned ? "learned" : c->rate_from_store ? "stored" : "none";
    st->stored_ppm = c->rate_stored * 1e6;
    st->skew_ppm = m->rate_skew * 1e6;
    st->refs = m->n;
    st->span_s = m->rate_span_s;
    st->last_err_ms = m->last_err_us / 1000.0;
    st->resid_rms_ms = m->resid_rms_us / 1000.0;
    st->slew_left_ms = m->valid ? nc_model_slew_left(m, now_us) / 1000.0 : 0;
    st->outliers = m->outliers;
    st->temp_comp_ppm = c->comp * 1e6;
    st->base_die_c = c->base_die_c;
    st->tcal = c->tcal;
    if (m->valid) {
        st->utc_us = nc_model_utc(m, now_us);
    }
    st->n_log = c->log_n < NETCLOCK_LOG ? (int)c->log_n : NETCLOCK_LOG;
    for (int j = 0; j < st->n_log; j++) {
        const netclock_ref_t *e = &c->log[(c->log_n - st->n_log + j) % NETCLOCK_LOG];
        st->log[j] = *e;
        double r;
        st->resid_us[j] = nc_model_fit_resid(m, e->t_us, e->utc_us, &r) ? (float)r : NAN;
        st->in_fit[j] = nc_model_in_fit(m, e->t_us);
    }
}
