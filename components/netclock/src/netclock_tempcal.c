// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
// A crystal's error against temperature (see netclock_tempcal.h).
#include "netclock_tempcal.h"

#include <math.h>
#include <string.h>

void nc_settle_init(nc_settle_t *s, double every_s, double window_s, double tol)
{
    memset(s, 0, sizeof(*s));
    s->every_s = every_s;
    s->window_s = window_s;
    s->tol = tol;
    s->last_t = NAN;
}

void nc_settle_add(nc_settle_t *s, double t, double v)
{
    if (isnan(v)) {
        s->count = 0;  // a gap: what came before doesn't say the value held through it
        s->last_t = NAN;
        return;
    }
    if (!isnan(s->last_t) && t - s->last_t < s->every_s) {
        return;
    }
    s->last_t = t;
    int k = (s->head + s->count) % NC_SETTLE_N;
    s->t[k] = t;
    s->v[k] = v;
    if (s->count < NC_SETTLE_N) {
        s->count++;
    } else {
        s->head = (s->head + 1) % NC_SETTLE_N;
    }
}

double nc_settle_range(const nc_settle_t *s, double t)
{
    // The window covered: the oldest sample at its start (or within one interval of it).
    if (!s->count || s->t[s->head] > t - s->window_s + s->every_s) {
        return NAN;
    }
    double lo = INFINITY, hi = -INFINITY;
    for (int i = 0; i < s->count; i++) {
        int k = (s->head + i) % NC_SETTLE_N;
        if (s->t[k] >= t - s->window_s) {
            lo = fmin(lo, s->v[k]);
            hi = fmax(hi, s->v[k]);
        }
    }
    return hi < lo ? NAN : hi - lo;
}

bool nc_settle_ok(const nc_settle_t *s, double t)
{
    double r = nc_settle_range(s, t);
    return !isnan(r) && r <= s->tol;
}

void nc_tcal_init(nc_tcal_t *c)
{
    memset(c, 0, sizeof(*c));
    c->version = NC_TCAL_VERSION;
}

bool nc_tcal_valid(const nc_tcal_t *c)
{
    if (c->version != NC_TCAL_VERSION || c->count > NC_TCAL_N) {
        return false;
    }
    for (int i = 0; i < c->count; i++) {
        if (!isfinite(c->e[i].v) || (i && c->e[i].temp_c <= c->e[i - 1].temp_c)) {
            return false;
        }
    }
    return true;
}

int nc_tcal_bin(double temp_c)
{
    return (int)lrint(temp_c / NC_TCAL_BIN_C) * NC_TCAL_BIN_C;
}

const nc_tcal_entry_t *nc_tcal_find(const nc_tcal_t *c, double temp_c)
{
    if (isnan(temp_c)) {
        return NULL;
    }
    int b = nc_tcal_bin(temp_c);
    for (int i = 0; i < c->count; i++) {
        if (c->e[i].temp_c == b) {
            return &c->e[i];
        }
    }
    return NULL;
}

void nc_tcal_put(nc_tcal_t *c, double temp_c, double v)
{
    if (isnan(temp_c) || !isfinite(v)) {
        return;
    }
    int b = nc_tcal_bin(temp_c);
    if (b < -128 || b > 127) {
        return;
    }
    c->stamp++;
    int i = 0;
    while (i < c->count && c->e[i].temp_c < b) {
        i++;
    }
    if (i < c->count && c->e[i].temp_c == b) {
        nc_tcal_entry_t *e = &c->e[i];
        int w = e->n + 1 < NC_TCAL_WEIGHT ? e->n + 1 : NC_TCAL_WEIGHT;
        e->v += (float)((v - e->v) / w);
        if (e->n < 255) {
            e->n++;
        }
        e->stamp = c->stamp;
        return;
    }
    if (c->count == NC_TCAL_N) {
        int old = 0;
        for (int k = 1; k < c->count; k++) {
            if (c->e[k].stamp < c->e[old].stamp) {
                old = k;
            }
        }
        memmove(&c->e[old], &c->e[old + 1], sizeof(c->e[0]) * (size_t)(c->count - old - 1));
        c->count--;
        if (old < i) {
            i--;
        }
    }
    memmove(&c->e[i + 1], &c->e[i], sizeof(c->e[0]) * (size_t)(c->count - i));
    c->e[i] = (nc_tcal_entry_t){.temp_c = (int8_t)b, .n = 1, .v = (float)v, .stamp = c->stamp};
    c->count++;
}

bool nc_tcal_get(const nc_tcal_t *c, double temp_c, double *v)
{
    if (!c->count || isnan(temp_c)) {
        return false;
    }
    const nc_tcal_entry_t *lo = &c->e[0], *hi = &c->e[c->count - 1];
    if (temp_c <= lo->temp_c) {
        *v = lo->v;
        return true;
    }
    if (temp_c >= hi->temp_c) {
        *v = hi->v;  // beyond what's been seen: the nearest, not a line that runs away
        return true;
    }
    int i = 1;
    while (c->e[i].temp_c < temp_c) {
        i++;
    }
    const nc_tcal_entry_t *a = &c->e[i - 1], *b = &c->e[i];
    double f = (temp_c - a->temp_c) / (double)(b->temp_c - a->temp_c);
    *v = a->v + f * (b->v - a->v);
    return true;
}
