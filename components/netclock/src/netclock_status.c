// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
// The status as JSON, into a caller's buffer (see netclock_status.h).
#include "netclock_status.h"

#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

const char *netclock_source_name(netclock_source_t s)
{
    switch (s) {
    case NETCLOCK_SRC_NTP: return "ntp";
    case NETCLOCK_SRC_BROWSER: return "browser";
    case NETCLOCK_SRC_PPS: return "pps";
    case NETCLOCK_SRC_GNSS: return "gnss";
    default: return "none";
    }
}

typedef struct {
    char *buf;
    size_t cap, len;
    bool over;
    bool first;
} jw_t;

static void put(jw_t *w, const char *fmt, ...)
{
    if (w->over) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(w->buf + w->len, w->cap - w->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= w->cap - w->len) {
        w->over = true;
        return;
    }
    w->len += (size_t)n;
}

static void key(jw_t *w, const char *k)
{
    put(w, w->first ? "\"%s\":" : ",\"%s\":", k);
    w->first = false;
}

static void open_obj(jw_t *w, const char *k)
{
    if (k) {
        key(w, k);
    } else if (!w->first) {
        put(w, ",");
    }
    put(w, "{");
    w->first = true;
}

static void close_obj(jw_t *w)
{
    put(w, "}");
    w->first = false;
}

static void str(jw_t *w, const char *k, const char *v)
{
    key(w, k);
    put(w, "\"");
    for (const unsigned char *p = (const unsigned char *)v; *p; p++) {
        if (*p == '"' || *p == '\\') {
            put(w, "\\%c", *p);
        } else if (*p < 0x20) {
            put(w, "\\u%04x", *p);
        } else {
            put(w, "%c", *p);
        }
    }
    put(w, "\"");
}

static void num_i(jw_t *w, const char *k, long long v)
{
    key(w, k);
    put(w, "%lld", v);
}

// Rounded to `dec` places, trailing zeros dropped; NaN as null.
static void num_f(jw_t *w, const char *k, double v, int dec)
{
    key(w, k);
    if (!isfinite(v)) {
        put(w, "null");
        return;
    }
    char t[40];
    snprintf(t, sizeof(t), "%.*f", dec, v);
    char *dot = strchr(t, '.');
    if (dot) {
        char *e = t + strlen(t) - 1;
        while (e > dot && *e == '0') {
            *e-- = 0;
        }
        if (e == dot) {
            *e = 0;
        }
    }
    if (!strcmp(t, "-0")) {
        strcpy(t, "0");
    }
    put(w, "%s", t);
}

static void boolean(jw_t *w, const char *k, bool v)
{
    key(w, k);
    put(w, v ? "true" : "false");
}

static const char *const RESULT[] = {"step", "slew", "outlier"};

size_t netclock_status_json(const netclock_status_t *c, char *buf, size_t cap)
{
    jw_t w = {.buf = buf, .cap = cap, .first = true};
    if (!cap) {
        return 0;
    }
    buf[0] = 0;
    open_obj(&w, NULL);
    str(&w, "source", netclock_source_name(c->source));
    boolean(&w, "synced", c->source != NETCLOCK_SRC_NONE);
    num_i(&w, "since_sync_s", c->since_sync_s);
    num_i(&w, "poll_s", c->poll_s);
    num_i(&w, "polls", c->polls);
    num_i(&w, "no_majority", c->no_majority);
    num_i(&w, "pps_gpio", c->pps_gpio);
    num_i(&w, "pps_edges", c->pps_edges);
    num_i(&w, "pps_bad", c->pps_bad);
    num_i(&w, "pps_unlabeled", c->pps_unlabeled);
    num_i(&w, "pps_last_s", c->pps_last_s);
    num_i(&w, "pps_by_gnss", c->pps_by_gnss);
    num_i(&w, "gnss_times", c->gnss_times);
    num_i(&w, "gnss_refs", c->gnss_refs);
    num_f(&w, "gnss_lag_ms", c->gnss_lag_ms, 1);
    num_i(&w, "gnss_last_s", c->gnss_last_s);
    key(&w, "sources");
    put(&w, "[");
    w.first = true;
    for (int i = 0; i < c->n_src; i++) {
        const netclock_src_view_t *v = &c->src[i];
        char m[2] = {v->mark ? v->mark : '?', 0};
        open_obj(&w, NULL);
        str(&w, "mark", m);
        str(&w, "name", v->name);
        str(&w, "ip", v->ip);
        num_i(&w, "stratum", v->stratum);
        str(&w, "refid", v->refid);
        num_i(&w, "reach", v->reach);
        num_i(&w, "last_rx_s", v->last_rx_s);
        num_f(&w, "offset_ms", v->offset_ms, 3);
        num_f(&w, "delay_ms", v->delay_ms, 3);
        num_f(&w, "min_delay_ms", v->min_delay_ms, 3);
        num_f(&w, "lambda_ms", v->lambda_ms, 3);
        num_f(&w, "jitter_ms", v->jitter_ms, 3);
        num_f(&w, "root_delay_ms", v->root_delay_ms, 3);
        num_f(&w, "root_disp_ms", v->root_disp_ms, 3);
        num_i(&w, "np", v->np);
        boolean(&w, "kod", v->kod);
        close_obj(&w);
    }
    put(&w, "]");
    w.first = false;
    num_i(&w, "ref_utc_ms", c->ref_utc_us / 1000);
    num_i(&w, "utc_ms", c->utc_us / 1000);
    boolean(&w, "fit_ok", c->fit_ok);
    num_f(&w, "rate_ppm", c->rate_ppm, 3);
    num_f(&w, "skew_ppm", c->skew_ppm, 3);
    str(&w, "rate_src", c->rate_src ? c->rate_src : "none");
    num_f(&w, "stored_ppm", c->stored_ppm, 3);
    num_i(&w, "refs", c->refs);
    num_f(&w, "span_s", c->span_s, 0);
    num_f(&w, "last_err_ms", c->last_err_ms, 3);
    num_f(&w, "resid_rms_ms", c->resid_rms_ms, 3);
    num_f(&w, "slew_left_ms", c->slew_left_ms, 3);
    num_i(&w, "outliers", c->outliers);
    if (!isnan(c->die_c)) {
        num_f(&w, "die_c", c->die_c, 1);
    }
    num_f(&w, "temp_comp_ppm", c->temp_comp_ppm, 3);
    if (!isnan(c->base_die_c)) {
        num_f(&w, "base_die_c", c->base_die_c, 1);
    }
    key(&w, "tcal");
    put(&w, "[");
    w.first = true;
    for (int i = 0; i < c->tcal.count; i++) {
        open_obj(&w, NULL);
        num_i(&w, "c", c->tcal.e[i].temp_c);
        num_f(&w, "ppm", c->tcal.e[i].v, 3);
        num_i(&w, "n", c->tcal.e[i].n);
        close_obj(&w);
    }
    put(&w, "]");
    w.first = false;
    key(&w, "log");
    put(&w, "[");
    w.first = true;
    for (int i = 0; i < c->n_log; i++) {
        const netclock_ref_t *e = &c->log[i];
        open_obj(&w, NULL);
        num_i(&w, "utc_ms", e->utc_us / 1000);
        num_f(&w, "t_s", e->t_us / 1e6, 3);
        str(&w, "src", netclock_source_name((netclock_source_t)e->src));
        str(&w, "peer", e->peer);
        str(&w, "result", e->result < 3 ? RESULT[e->result] : "?");
        num_f(&w, "err_ms", e->err_us / 1000.0, 3);
        if (!isnan(c->resid_us[i])) {
            num_f(&w, "resid_ms", c->resid_us[i] / 1000.0, 3);
        }
        boolean(&w, "in_fit", c->in_fit[i]);
        num_f(&w, "rate_ppm", e->rate_ppm, 3);
        if (!isnan(e->die_c)) {
            num_f(&w, "die_c", e->die_c, 1);
        }
        close_obj(&w);
    }
    put(&w, "]");
    close_obj(&w);
    if (w.over) {
        buf[0] = 0;
        return 0;
    }
    return w.len;
}
