// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The UTC slot grid (the grid, slot timing, the slot gate, the sample clock);
 * vectors.h checks it. rust/src/slotgrid.rs
 * is the same in Rust.
 *
 * The int64 arithmetic wraps (two's
 * complement, as the Rust's), which only matters outside 1677 to 2262.
 */
#include "rdof/slotgrid.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int64_t add64(int64_t a, int64_t b) { return (int64_t)((uint64_t)a + (uint64_t)b); }
static int64_t sub64(int64_t a, int64_t b) { return (int64_t)((uint64_t)a - (uint64_t)b); }
static int64_t mul64(int64_t a, int64_t b) { return (int64_t)((uint64_t)a * (uint64_t)b); }

static int64_t floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

/* b > 0 */
static int64_t ceil_div(int64_t a, int64_t b)
{
    return a / b + (a % b > 0);
}

int64_t rdof_grid_boundary(int64_t n, int offset_us)
{
    return add64(mul64(n, RDOF_SLOT_NS), (int64_t)offset_us * 1000);
}

int64_t rdof_grid_slot_at(int64_t t_ns, int offset_us, int *ts, int64_t *phase_ns)
{
    int64_t x = sub64(t_ns, (int64_t)offset_us * 1000);
    int64_t n = floor_div(x, RDOF_SLOT_NS);
    *ts = (n & 1) == 0 ? 1 : 2;
    *phase_ns = sub64(x, mul64(n, RDOF_SLOT_NS));
    return n;
}

int64_t rdof_grid_next_boundary(int64_t t_ns, int offset_us, int64_t *b)
{
    int64_t x = sub64(t_ns, (int64_t)offset_us * 1000);
    int64_t n = ceil_div(x, RDOF_SLOT_NS); /* -(-x // SLOT_NS): -x overflows at INT64_MIN */
    *b = add64(mul64(n, RDOF_SLOT_NS), (int64_t)offset_us * 1000);
    return n;
}

int64_t rdof_grid_inbound(int64_t start_ns, int offset_us, int *ts, int64_t *err_ns)
{
    int64_t x = sub64(sub64(start_ns, RDOF_IN_START_NS), (int64_t)offset_us * 1000);
    int64_t n = floor_div(add64(x, RDOF_SLOT_NS / 2), RDOF_SLOT_NS);
    *ts = (n & 1) == 0 ? 2 : 1;
    *err_ns = sub64(x, mul64(n, RDOF_SLOT_NS));
    return n;
}

int64_t rdof_grid_outbound(int64_t start_ns, int offset_us, int *ts, int64_t *err_ns)
{
    int64_t x = sub64(sub64(start_ns, RDOF_CACH_NS), (int64_t)offset_us * 1000);
    int64_t n = floor_div(add64(x, RDOF_SLOT_NS / 2), RDOF_SLOT_NS);
    *ts = (n & 1) == 0 ? 1 : 2;
    *err_ns = sub64(x, mul64(n, RDOF_SLOT_NS));
    return n;
}

/* ── the setting ── */

void rdof_slot_timing_default(rdof_slot_timing_t *t)
{
    memset(t, 0, sizeof *t);
    t->max_err_pps_us = 20;
    t->max_err_ntp_us = 2000;
    t->on_loss = RDOF_LOSS_HOLDOVER;
    t->holdover_s = 60.0;
}

static const char *const KEYS[] = {"mode", "offset_us", "max_err_us", "on_loss", "holdover_s"};
#define NKEYS 5

/* The node for key in a slot_timing value (a string is {"mode": it}); -1 none. *str_mode: the string itself. */
static int key_of(const rdof_jdoc_t *doc, int v, const char *key, bool *as_string)
{
    *as_string = false;
    if (v < 0)
        return -1;
    const rdof_jnode_t *n = rdof_jnode(doc, v);
    if (n->type == RDOF_JSTR) {
        if (!strcmp(key, "mode")) {
            *as_string = true;
            return v;
        }
        return -1;
    }
    if (n->type != RDOF_JOBJ)
        return -1;
    return rdof_jget(doc, v, key);
}

static bool int_us(const rdof_jdoc_t *doc, int i, const char *what, int *out, char *error, size_t cap)
{
    const rdof_jnode_t *n = rdof_jnode(doc, i);
    if (n->type != RDOF_JNUM || n->num != n->num || n->num != floor(n->num) || fabs(n->num) > 2e9) {
        snprintf(error, cap, "slot_timing.%s: whole microseconds", what);
        return false;
    }
    *out = (int)n->num;
    return true;
}

static bool checked(const rdof_jdoc_t *doc, int v, char *error, size_t cap)
{
    if (v < 0)
        return true;
    const rdof_jnode_t *n = rdof_jnode(doc, v);
    if (n->type == RDOF_JNULL || n->type == RDOF_JSTR)
        return true;
    if (n->type != RDOF_JOBJ) {
        snprintf(error, cap, "slot_timing: free, utc, or {mode, offset_us, max_err_us, on_loss, holdover_s}");
        return false;
    }
    for (int c = n->child; c >= 0; c = rdof_jnode(doc, c)->next) {
        bool known = false;
        for (int k = 0; k < NKEYS; k++)
            known |= !strcmp(rdof_jnode(doc, c)->key, KEYS[k]);
        if (!known) {
            snprintf(error, cap, "slot_timing: unknown key '%s'", rdof_jnode(doc, c)->key);
            return false;
        }
    }
    return true;
}

bool rdof_slot_timing_parse(const rdof_jdoc_t *doc, int base, int own, rdof_slot_timing_t *t, char *error, size_t cap)
{
    rdof_slot_timing_default(t);
    /* null, or a node the parse didn't make: none */
    if (base >= 0 && (!rdof_jnode(doc, base) || rdof_jnode(doc, base)->type == RDOF_JNULL))
        base = -1;
    if (own >= 0 && (!rdof_jnode(doc, own) || rdof_jnode(doc, own)->type == RDOF_JNULL))
        own = -1;
    if (!checked(doc, base, error, cap) || !checked(doc, own, error, cap))
        return false;
    bool s;
    int i;
    /* each key: the radio's, else the box's ({**box, **own}) */
#define PICK(key) ((i = key_of(doc, own, key, &s)) >= 0 ? i : (i = key_of(doc, base, key, &s)))
    if (PICK("mode") >= 0) {
        const rdof_jnode_t *n = rdof_jnode(doc, i);
        if (n->type != RDOF_JSTR || (strcmp(n->str, "free") && strcmp(n->str, "utc"))) {
            snprintf(error, cap, "slot_timing.mode: free or utc");
            return false;
        }
        t->utc = !strcmp(n->str, "utc");
    }
    if (PICK("offset_us") >= 0) {
        if (!int_us(doc, i, "offset_us", &t->offset_us, error, cap))
            return false;
        if (t->offset_us > RDOF_SLOT_MAX_OFFSET_US || t->offset_us < -RDOF_SLOT_MAX_OFFSET_US) {
            snprintf(error, cap, "slot_timing.offset_us %d: within +-%d (half a slot)", t->offset_us,
                     RDOF_SLOT_MAX_OFFSET_US);
            return false;
        }
    }
    if (PICK("max_err_us") >= 0) {
        const rdof_jnode_t *n = rdof_jnode(doc, i);
        if (n->type == RDOF_JOBJ) {
            for (int c = n->child; c >= 0; c = rdof_jnode(doc, c)->next) {
                const char *k = rdof_jnode(doc, c)->key;
                if (strcmp(k, "pps") && strcmp(k, "ntp")) {
                    snprintf(error, cap, "slot_timing.max_err_us: unknown source '%s' (pps, ntp)", k);
                    return false;
                }
            }
            /* a source given twice: the last one */
            int c = rdof_jget(doc, i, "pps");
            if (c >= 0 && !int_us(doc, c, "max_err_us.pps", &t->max_err_pps_us, error, cap))
                return false;
            c = rdof_jget(doc, i, "ntp");
            if (c >= 0 && !int_us(doc, c, "max_err_us.ntp", &t->max_err_ntp_us, error, cap))
                return false;
        } else {
            if (!int_us(doc, i, "max_err_us", &t->max_err_pps_us, error, cap))
                return false;
            t->max_err_ntp_us = t->max_err_pps_us;
        }
        const int b[2] = {t->max_err_pps_us, t->max_err_ntp_us};
        for (int k = 0; k < 2; k++)
            if (!(b[k] > 0 && b[k] <= RDOF_SLOT_MAX_OFFSET_US)) {
                snprintf(error, cap, "slot_timing.max_err_us %d: above 0, at most %d", b[k], RDOF_SLOT_MAX_OFFSET_US);
                return false;
            }
    }
    if (PICK("on_loss") >= 0) {
        const rdof_jnode_t *n = rdof_jnode(doc, i);
        if (n->type == RDOF_JSTR && !strcmp(n->str, "holdover"))
            t->on_loss = RDOF_LOSS_HOLDOVER;
        else if (n->type == RDOF_JSTR && !strcmp(n->str, "free"))
            t->on_loss = RDOF_LOSS_FREE;
        else if (n->type == RDOF_JSTR && !strcmp(n->str, "stop"))
            t->on_loss = RDOF_LOSS_STOP;
        else {
            snprintf(error, cap, "slot_timing.on_loss: holdover, free or stop");
            return false;
        }
    }
    if (PICK("holdover_s") >= 0) {
        const rdof_jnode_t *n = rdof_jnode(doc, i);
        if (n->type != RDOF_JNUM || !(n->num >= 0 && n->num <= 86400)) {
            snprintf(error, cap, "slot_timing.holdover_s: 0 to 86400 s");
            return false;
        }
        t->holdover_s = n->num;
    }
#undef PICK
    return true;
}

static const char *const LOSS_NAMES[] = {"holdover", "free", "stop"};

size_t rdof_slot_timing_describe(const rdof_slot_timing_t *t, char *out, size_t cap)
{
    /* repr(round(holdover_s, 3)): at most three decimals, at least one. %.3f rounds the exact binary value, half
     * to even. round(h * 1000) / 1000 rounded twice: 1.0005 (1.000499...) came out 1.001, not 1.0. */
    char h[32];
    snprintf(h, sizeof h, "%.3f", t->holdover_s);
    size_t n = strlen(h);
    while (n > 0 && h[n - 1] == '0' && h[n - 2] != '.')
        h[--n] = 0;
    int w = snprintf(out, cap, "%s,%d,%d,%d,%s,%s", t->utc ? "utc" : "free", t->offset_us, t->max_err_pps_us,
                     t->max_err_ntp_us, (unsigned)t->on_loss < 3 ? LOSS_NAMES[t->on_loss] : "?", h);
    return w < 0 ? 0 : (size_t)w;
}

/* ── the gate ── */

void rdof_slot_gate_init(rdof_slot_gate_t *g, const rdof_slot_timing_t *t)
{
    memset(g, 0, sizeof *g);
    g->timing = *t;
    g->state = !t->utc ? RDOF_GATE_FREE : (t->on_loss == RDOF_LOSS_STOP ? RDOF_GATE_STOPPED : RDOF_GATE_FREE);
    snprintf(g->source, sizeof g->source, "none");
    g->err_us = NAN;
    snprintf(g->why, sizeof g->why, "%s", t->utc ? "no clock yet" : "slot timing free");
}

rdof_gate_state_t rdof_slot_gate_update(rdof_slot_gate_t *g, double now_s, bool synced, const char *source, double err_us)
{
    const rdof_slot_timing_t *t = &g->timing;
    if (!source)
        source = "none";
    snprintf(g->source, sizeof g->source, "%s", source);
    g->err_us = err_us;
    if (!t->utc) {
        g->state = RDOF_GATE_FREE;
        snprintf(g->why, sizeof g->why, "slot timing free");
        return g->state;
    }
    int bound = !strcmp(source, "pps") ? t->max_err_pps_us : !strcmp(source, "ntp") ? t->max_err_ntp_us : -1;
    if (!synced)
        snprintf(g->why, sizeof g->why, "the clock isn't synced");
    else if (bound < 0)
        snprintf(g->why, sizeof g->why, "a %s clock can't time slots", source);
    else if (err_us != err_us)
        snprintf(g->why, sizeof g->why, "no error estimate");
    else if (err_us > bound)
        snprintf(g->why, sizeof g->why, "clock error over %d us", bound);
    else {
        g->state = RDOF_GATE_ALIGNED;
        g->why[0] = 0;
        g->has_last_ok = true;
        g->last_ok = now_s;
        return g->state;
    }
    if (t->on_loss == RDOF_LOSS_STOP)
        g->state = RDOF_GATE_STOPPED;
    else if (t->on_loss == RDOF_LOSS_HOLDOVER && g->has_last_ok && now_s - g->last_ok <= t->holdover_s)
        g->state = RDOF_GATE_HOLDOVER;
    else
        g->state = RDOF_GATE_FREE;
    return g->state;
}

double rdof_slot_gate_holdover_age(const rdof_slot_gate_t *g, double now_s)
{
    if (g->state == RDOF_GATE_ALIGNED || !g->has_last_ok)
        return 0.0;
    return now_s - g->last_ok;
}

const char *rdof_gate_state_name(rdof_gate_state_t s)
{
    static const char *const names[] = {"free", "aligned", "holdover", "stopped"};
    return (unsigned)s < 4 ? names[s] : "?";
}

/* ── the sample clock ── */

void rdof_sample_clock_init(rdof_sample_clock_t *c, double rate_hz, double window_s, bool stamped)
{
    memset(c, 0, sizeof *c);
    c->nominal = rate_hz;
    c->window_s = window_s;
    c->stamped = stamped;
}

/* (int64_t) of a double out of range is undefined: saturated here, NaN 0 (as Rust's `as`) */
static int64_t to_i64(double x)
{
    if (x != x)
        return 0;
    if (x >= 9223372036854775808.0)
        return INT64_MAX;
    if (x < -9223372036854775808.0)
        return INT64_MIN;
    return (int64_t)x;
}

static int64_t round_half_up(double x)
{
    return to_i64(floor(x + 0.5));
}

bool rdof_sample_clock_utc(const rdof_sample_clock_t *c, double index, int64_t *t_ns)
{
    if (!c->has_anchor)
        return false;
    *t_ns = add64(c->t0, round_half_up((index - (double)c->i0) * 1e9 / rdof_sample_clock_rate(c)));
    return true;
}

bool rdof_sample_clock_index(const rdof_sample_clock_t *c, int64_t t_ns, double *index)
{
    if (!c->has_anchor)
        return false;
    *index = (double)c->i0 + (double)sub64(t_ns, c->t0) * rdof_sample_clock_rate(c) / 1e9;
    return true;
}

void rdof_sample_clock_add(rdof_sample_clock_t *c, int64_t end_index, int64_t arrival_ns)
{
    if (c->stamped) {
        c->has_anchor = true;
        c->i0 = end_index, c->t0 = arrival_ns;
        c->has_err = true;
        c->err_ns = 0;
        return;
    }
    if (!c->has_anchor) {
        c->has_anchor = true;
        c->i0 = end_index, c->t0 = arrival_ns;
        c->has_best = true;
        c->best_r = 0, c->best_i = end_index, c->best_t = arrival_ns;
        c->win_start = end_index;
        c->moved = 0;
        return;
    }
    int64_t line;
    rdof_sample_clock_utc(c, (double)end_index, &line);
    int64_t resid = sub64(arrival_ns, line);
    if (resid < 0) { /* earlier than the line allows: it was late */
        c->i0 = end_index, c->t0 = arrival_ns;
        if (sub64(0, resid) > c->moved)
            c->moved = sub64(0, resid);
        resid = 0;
    }
    if (!c->has_best || resid <= c->best_r) {
        c->has_best = true;
        c->best_r = resid, c->best_i = end_index, c->best_t = arrival_ns;
    }
    if ((double)sub64(end_index, c->win_start) >= c->window_s * c->nominal) {
        c->i0 = c->best_i, c->t0 = c->best_t;
        c->has_err = true;
        c->err_ns = c->best_r > c->moved ? c->best_r : c->moved;
        c->windows++;
        c->has_best = false;
        c->win_start = end_index;
        c->moved = 0;
    }
}
