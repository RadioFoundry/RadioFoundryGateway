// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* A trunked site's settings into the controller's (rdof/t3cfg.h, docs/spec/fw/tier3.md sections 3.8 and 4). */
#include "rdof/t3cfg.h"

#include <math.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "rdof/idset.h"
#include "rdof/mnctl.h"
#include "rdof/siteplan.h"

typedef struct {
    const rdof_jdoc_t *doc;
    char *err;
    size_t cap;
} ctx_t;

static bool fail(ctx_t *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->err, c->cap, fmt, ap);
    va_end(ap);
    return false;
}

static const rdof_jnode_t *N(ctx_t *c, int i) { return rdof_jnode(c->doc, i); }
static int get(ctx_t *c, int obj, const char *key) { return obj < 0 ? -1 : rdof_jget(c->doc, obj, key); }
static bool is_type(ctx_t *c, int i, rdof_jtype_t t) { return i >= 0 && N(c, i)->type == t; }
static const char *str_of(ctx_t *c, int i) { return is_type(c, i, RDOF_JSTR) ? N(c, i)->str : NULL; }

/* MODEL's NET and SITE bits (361-4 6.3.2.2.1.1) */
static void model_bits(int model, int *nb, int *sb)
{
    static const int n[4] = {9, 7, 4, 2}, st[4] = {3, 5, 8, 10};
    model = model & 3;
    *nb = n[model];
    *sb = st[model];
}

static const char *const MODELS[] = {"tiny", "small", "large", "huge"};
static const char *const SYSTEM_KEYS[] = {"model", "net", "site", "par", "area_bits", NULL};
static const char *const BROADCAST_KEYS[] = {"adjacent_s", "chan_freq_s", "local_time_s", NULL};

static bool known(ctx_t *c, int obj, const char *path, const char *const *keys)
{
    if (obj < 0)
        return true;
    if (!is_type(c, obj, RDOF_JOBJ))
        return fail(c, "%s: an object", path);
    for (int i = N(c, obj)->child; i >= 0; i = N(c, i)->next) {
        bool ok = false;
        for (int k = 0; keys[k] && !ok; k++)
            ok = !strcmp(keys[k], N(c, i)->key);
        if (!ok)
            return fail(c, "%s.%s: not a key here", path, N(c, i)->key);
    }
    return true;
}

/* A number in [lo, hi], integral unless frac; absent: *out untouched. */
static bool num_in(ctx_t *c, int obj, const char *key, const char *path, double lo, double hi, bool frac, double *out,
                   bool *had)
{
    int i = get(c, obj, key);
    if (had)
        *had = i >= 0;
    if (i < 0)
        return true;
    double v = is_type(c, i, RDOF_JNUM) ? N(c, i)->num : NAN;
    if (!(v >= lo && v <= hi) || (!frac && v != floor(v))) {
        if (frac)
            return fail(c, "%s.%s: %g to %g", path, key, lo, hi);
        return fail(c, "%s.%s: %.0f to %.0f", path, key, lo, hi);
    }
    *out = v;
    return true;
}

static bool u8_in(ctx_t *c, int obj, const char *key, const char *path, int lo, int hi, uint8_t *out)
{
    double v = *out;
    if (!num_in(c, obj, key, path, lo, hi, false, &v, NULL))
        return false;
    *out = (uint8_t)v;
    return true;
}

/* seconds into ms */
static bool ms_in(ctx_t *c, int obj, const char *key, const char *path, double lo, double hi, bool frac, uint32_t *out)
{
    double v = *out / 1000.0;
    if (!num_in(c, obj, key, path, lo, hi, frac, &v, NULL))
        return false;
    *out = (uint32_t)llround(v * 1000.0);
    return true;
}

static bool bool_of(ctx_t *c, int obj, const char *key, const char *path, uint8_t *out)
{
    int i = get(c, obj, key);
    if (i < 0)
        return true;
    if (!is_type(c, i, RDOF_JBOOL))
        return fail(c, "%s.%s: true or false", path, key);
    *out = N(c, i)->b;
    return true;
}

/* A parsed ID set into the controller's ranges. */
static bool to_t3(ctx_t *c, const char *path, const char *key, const rdof_idset_t *s, rdof_t3_idset_t *out)
{
    memset(out, 0, sizeof *out);
    if (s->star && !s->nout) {
        out->all = 1;
        return true;
    }
    rdof_idrange_t r[RDOF_T3_IDSET_RANGES];
    int n = rdof_idset_ranges(s, r, RDOF_T3_IDSET_RANGES);
    if (n > RDOF_T3_IDSET_RANGES)
        return fail(c, "%s.%s: %d ranges, the site holds %d", path, key, n, RDOF_T3_IDSET_RANGES);
    for (int k = 0; k < n; k++)
        out->r[k] = (rdof_t3_range_t){r[k].lo, r[k].hi};
    out->n = (uint8_t)n;
    return true;
}

/* An ID set (config-v3 section 3) into the controller's ranges. */
static bool idset_of(ctx_t *c, int obj, const char *key, const char *path, rdof_t3_idset_t *out)
{
    int i = get(c, obj, key);
    if (i < 0)
        return true;
    char why[96];
    rdof_idset_t s;
    rdof_idset_clear(&s);
    if (is_type(c, i, RDOF_JBOOL) && !N(c, i)->b) {
        /* false: none */
    } else if (is_type(c, i, RDOF_JNUM)) {
        double v = N(c, i)->num;
        if (v != floor(v) || v < 1 || v > RDOF_ID_MAX || !rdof_idset_add_id(&s, (uint32_t)v, why, sizeof why))
            return fail(c, "%s.%s: an ID from 1 to 16777215", path, key);
    } else if (str_of(c, i)) {
        if (!rdof_idset_add(&s, str_of(c, i), why, sizeof why))
            return fail(c, "%s.%s: %s", path, key, why);
    } else if (is_type(c, i, RDOF_JARR)) {
        int k = 0;
        for (int it = N(c, i)->child; it >= 0; it = N(c, it)->next, k++) {
            if (is_type(c, it, RDOF_JNUM)) {
                double v = N(c, it)->num;
                if (v != floor(v) || v < 1 || v > RDOF_ID_MAX || !rdof_idset_add_id(&s, (uint32_t)v, why, sizeof why))
                    return fail(c, "%s.%s[%d]: an ID from 1 to 16777215", path, key, k);
            } else if (!str_of(c, it) || !rdof_idset_add(&s, str_of(c, it), why, sizeof why)) {
                return fail(c, "%s.%s[%d]: %s", path, key, k,
                            str_of(c, it) ? why : "an ID, a range \"A-B\", \"*\" or \"!...\"");
            }
        }
    } else {
        return fail(c, "%s.%s: an ID set: 202, \"3100-3199\", \"*\", \"!350\", a list of those, or false", path, key);
    }
    rdof_idset_finish(&s);
    return to_t3(c, path, key, &s, out);
}

void rdof_t3cfg_default(rdof_t3_cfg_t *cfg, rdof_t3_policy_t *pol)
{
    if (cfg) {
        memset(cfg, 0, sizeof *cfg);
        cfg->model = 1; /* small */
        cfg->par = 3;
        cfg->area_bits = 5;
        cfg->version = 4;
        cfg->net = 1;
        cfg->site = 1;
        cfg->ncarriers = 1;
        cfg->cc = 1;
        cfg->reg_required = 1;
        cfg->nrand_wait = 6;
        cfg->backoff_min = 3;
        cfg->backoff_max = 9;
        cfg->clear_repeats = 4;
        cfg->warmup_cach = 4;
        cfg->ahoy_tries = 3;
        cfg->queue_max = 16;
        cfg->tp_ms = 20000;
        cfg->tnp_ms = 10000;
        cfg->hang_ms = 3000;
        cfg->call_ms = 180000;
        cfg->emergency_call_ms = 300000;
        cfg->ahoy_every_ms = 1000;
        cfg->grant_every_ms = 1200;
        cfg->queue_ms = 10000;
        cfg->net_queue_ms = 2000;
        cfg->setup_ms = 5000;
        cfg->start_grace_ms = 2000;
        cfg->reg_hold_s = 14400;
        cfg->adjacent_ms = 5000;
        cfg->chan_freq_ms = 10000;
        cfg->local_time_ms = 6000;
        cfg->gen_site_ms = 10000;
        cfg->stream_seed = 1;
        cfg->carriers[0] = (rdof_t3_carrier_t){.tx_hz = 438750000, .rx_hz = 438250000, .chan = 1, .tscc = 1,
                                               .enabled = 1};
    }
    if (pol) {
        memset(pol, 0, sizeof *pol);
        pol->open = 1;
        pol->ms.all = pol->private_ms.all = pol->tg.all = 1;
    }
}

static bool system_keys(ctx_t *c, int o, const char *path, bool top, rdof_t3_cfg_t *cfg)
{
    char p[96];
    snprintf(p, sizeof p, top ? "%s" : "%s.system", path);
    if (!top && !known(c, o, p, SYSTEM_KEYS))
        return false;
    int m = get(c, o, "model");
    if (!top && m >= 0) {
        int k = 0;
        while (k < 4 && !(str_of(c, m) && !strcmp(str_of(c, m), MODELS[k])))
            k++;
        if (k == 4)
            return fail(c, "%s.model: tiny, small, large or huge", p);
        cfg->model = (uint8_t)k;
        int nb, sb;
        model_bits(k, &nb, &sb);
        if (get(c, o, "area_bits") < 0)
            cfg->area_bits = (uint8_t)sb; /* every site its own registration area */
    }
    int nb, sb;
    model_bits(cfg->model, &nb, &sb);
    double v = cfg->net;
    if (!top && !num_in(c, o, "net", p, 0, (1 << nb) - 1, false, &v, NULL))
        return false;
    cfg->net = (uint16_t)v;
    v = cfg->site;
    if (!num_in(c, o, "site", p, 1, (1 << sb) - 1, false, &v, NULL))
        return false;
    cfg->site = (uint16_t)v;
    if (!u8_in(c, o, "area_bits", p, 1, sb, &cfg->area_bits))
        return false;
    int pi = get(c, o, "par");
    if (!top && pi >= 0) {
        const char *s = str_of(c, pi);
        if (s && !strcmp(s, "a"))
            cfg->par = 1;
        else if (s && !strcmp(s, "b"))
            cfg->par = 2;
        else if (s && !strcmp(s, "ab"))
            cfg->par = 3;
        else
            return fail(c, "%s.par: a, b or ab", p);
    }
    return true;
}

/* The timers (tier3.md 3.8): each a cfg field in its unit (ms from seconds, a code, seconds), its range. */
typedef enum { T_U8, T_MS, T_S } tkind_t;
typedef struct {
    const char *name;
    size_t off;
    tkind_t kind;
    double lo, hi;
    bool frac;
} timer_t_;

#define TM(n, f, k, lo, hi, fr) {n, offsetof(rdof_t3_cfg_t, f), k, lo, hi, fr}
static const timer_t_ TIMERS[] = {
    TM("nrand_wait", nrand_wait, T_U8, 0, 15, false),
    TM("backoff_min", backoff_min, T_U8, 1, 15, false),
    TM("backoff_max", backoff_max, T_U8, 1, 15, false),
    TM("tp_s", tp_ms, T_MS, 4, 60, false),
    TM("tnp_s", tnp_ms, T_MS, 2, 60, false),
    TM("hang_s", hang_ms, T_MS, 1, 60, true),
    TM("call_s", call_ms, T_MS, 1, 4094, false),
    TM("emergency_call_s", emergency_call_ms, T_MS, 1, 510, false),
    TM("ahoy_tries", ahoy_tries, T_U8, 1, 10, false),
    TM("ahoy_every_s", ahoy_every_ms, T_MS, 0.3, 5, true),
    TM("grant_every_s", grant_every_ms, T_MS, 0.3, 10, true),
    TM("clear_repeats", clear_repeats, T_U8, 1, 8, false),
    TM("warmup_cach", warmup_cach, T_U8, 2, 4, false),
    TM("reg_hold_s", reg_hold_s, T_S, 60, 604800, false),
    TM("queue_max", queue_max, T_U8, 1, 16, false),
    TM("queue_s", queue_ms, T_MS, 1, 60, false),
    TM("net_queue_s", net_queue_ms, T_MS, 0, 10, true),
    TM("setup_s", setup_ms, T_MS, 1, 30, true),
    TM("start_grace_s", start_grace_ms, T_MS, 0, 10, true),
};
#define NTIMERS (sizeof TIMERS / sizeof TIMERS[0])

/* Timer name to value v (checked); false with err set ("path.name: range"). */
static bool set_timer(ctx_t *c, const char *path, const char *name, double v, rdof_t3_cfg_t *cfg)
{
    for (size_t k = 0; k < NTIMERS; k++) {
        const timer_t_ *t = &TIMERS[k];
        if (strcmp(t->name, name))
            continue;
        if (!(v >= t->lo && v <= t->hi) || (!t->frac && v != floor(v)) || (t->off == offsetof(rdof_t3_cfg_t, warmup_cach) && v == 3)) {
            if (t->off == offsetof(rdof_t3_cfg_t, warmup_cach))
                return fail(c, "%s.%s: 2 or 4", path, name);
            return t->frac ? fail(c, "%s.%s: %g to %g", path, name, t->lo, t->hi)
                           : fail(c, "%s.%s: %.0f to %.0f", path, name, t->lo, t->hi);
        }
        uint8_t *at = (uint8_t *)cfg + t->off;
        if (t->kind == T_U8)
            *at = (uint8_t)v;
        else {
            uint32_t u = t->kind == T_MS ? (uint32_t)llround(v * 1000.0) : (uint32_t)v;
            memcpy(at, &u, sizeof u);
        }
        return true;
    }
    return fail(c, "%s.%s: not a key here", path, name);
}

static bool timer_keys(ctx_t *c, int t, const char *path, rdof_t3_cfg_t *cfg)
{
    char p[96];
    snprintf(p, sizeof p, "%s.timers", path);
    if (!is_type(c, t, RDOF_JOBJ))
        return fail(c, "%s: an object", p);
    for (int i = N(c, t)->child; i >= 0; i = N(c, i)->next) {
        double v = is_type(c, i, RDOF_JNUM) ? N(c, i)->num : NAN;
        if (!set_timer(c, p, N(c, i)->key, v, cfg))
            return false;
    }
    return true;
}

bool rdof_t3cfg_apply(const rdof_jdoc_t *doc, int obj, const char *path, rdof_t3_cfg_t *cfg,
                          rdof_t3_policy_t *pol, char *err, size_t cap)
{
    char dummy[8];
    ctx_t cx = {.doc = doc, .err = err && cap ? err : dummy, .cap = err && cap ? cap : sizeof dummy}, *c = &cx;
    c->err[0] = 0;
    if (!is_type(c, obj, RDOF_JOBJ))
        return fail(c, "%s: an object", path);
    int sys = get(c, obj, "system");
    if (sys >= 0 && !system_keys(c, sys, path, false, cfg))
        return false;
    /* cfg/node: site and area_bits at the top */
    if ((get(c, obj, "site") >= 0 || get(c, obj, "area_bits") >= 0) && !system_keys(c, obj, path, true, cfg))
        return false;
    int r = get(c, obj, "registration");
    if (r >= 0) {
        const char *s = str_of(c, r);
        if (!s || (strcmp(s, "open") && strcmp(s, "listed")))
            return fail(c, "%s.registration: open or listed", path);
        pol->open = !strcmp(s, "open");
        if (!pol->open && get(c, obj, "ms") < 0 && pol->ms.all) /* listed: the list, none until given */
            memset(&pol->ms, 0, sizeof pol->ms);
    }
    int ru = get(c, obj, "refuse_unlisted");
    if (ru >= 0) {
        const char *s = str_of(c, ru);
        if (!s || (strcmp(s, "denied") && strcmp(s, "refused")))
            return fail(c, "%s.refuse_unlisted: denied or refused", path);
        pol->refuse_unlisted = !strcmp(s, "refused");
    }
    if (!idset_of(c, obj, "ms", path, &pol->ms) || !idset_of(c, obj, "private", path, &pol->private_ms) ||
        !idset_of(c, obj, "tg", path, &pol->tg) || !idset_of(c, obj, "wide_tg", path, &pol->wide_tg) ||
        !idset_of(c, obj, "static_tg", path, &pol->static_tg))
        return false;
    if (!bool_of(c, obj, "require_registration", path, &cfg->reg_required) ||
        !bool_of(c, obj, "foacsu", path, &cfg->foacsu))
        return false;
    int t = get(c, obj, "timers");
    if (t >= 0 && !timer_keys(c, t, path, cfg))
        return false;
    int b = get(c, obj, "broadcast");
    if (b >= 0) {
        char p[96];
        snprintf(p, sizeof p, "%s.broadcast", path);
        if (!known(c, b, p, BROADCAST_KEYS) || !ms_in(c, b, "adjacent_s", p, 0, 600, true, &cfg->adjacent_ms) ||
            !ms_in(c, b, "chan_freq_s", p, 0, 600, true, &cfg->chan_freq_ms) ||
            !ms_in(c, b, "local_time_s", p, 0, 600, true, &cfg->local_time_ms))
            return false;
        /* 0 is off; anything else at least every TSCC slot pair */
        if ((cfg->adjacent_ms && cfg->adjacent_ms < 1000) || (cfg->chan_freq_ms && cfg->chan_freq_ms < 1000) ||
            (cfg->local_time_ms && cfg->local_time_ms < 1000))
            return fail(c, "%s: each 0 (off) or 1 to 600 s", p);
    }
    if (cfg->backoff_min > cfg->backoff_max)
        return fail(c, "%s.timers.backoff_min: %d, over backoff_max %d", path, cfg->backoff_min, cfg->backoff_max);
    return true;
}

bool rdof_t3cfg_check(const rdof_t3_cfg_t *cfg, char *err, size_t cap)
{
    char dummy[8];
    ctx_t cx = {.err = err && cap ? err : dummy, .cap = err && cap ? cap : sizeof dummy}, *c = &cx;
    c->err[0] = 0;
    if (cfg->model > 3)
        return fail(c, "system.model: tiny, small, large or huge");
    int nb, sb;
    model_bits(cfg->model, &nb, &sb);
    if (cfg->net >= 1u << nb)
        return fail(c, "system.net: 0 to %d for model %s", (1 << nb) - 1, MODELS[cfg->model]);
    if (!cfg->site || cfg->site >= 1u << sb)
        return fail(c, "system.site: 1 to %d for model %s", (1 << sb) - 1, MODELS[cfg->model]);
    if (!cfg->area_bits || cfg->area_bits > sb)
        return fail(c, "system.area_bits: 1 to %d for model %s", sb, MODELS[cfg->model]);
    if (cfg->par < 1 || cfg->par > 3)
        return fail(c, "system.par: a, b or ab");
    if (cfg->cc > 15)
        return fail(c, "cc: 0 to 15");
    if (!cfg->ncarriers || cfg->ncarriers > RDOF_T3_CARRIERS)
        return fail(c, "carriers: 1 to %d", RDOF_T3_CARRIERS);
    int tscc = 0;
    for (int i = 0; i < cfg->ncarriers; i++) {
        const rdof_t3_carrier_t *a = &cfg->carriers[i];
        if (!a->chan || a->chan > 4094)
            return fail(c, "carriers[%d].chan: 1 to 4094", i);
        for (int j = 0; j < i; j++)
            if (cfg->carriers[j].chan == a->chan)
                return fail(c, "carriers[%d].chan: %d is another carrier's", i, a->chan);
        tscc += a->tscc != 0;
    }
    if (tscc != 1)
        return fail(c, "carriers: exactly one with tscc");
    if (cfg->backoff_min < 1 || cfg->backoff_max > 15 || cfg->backoff_min > cfg->backoff_max)
        return fail(c, "timers.backoff_min, backoff_max: 1 to 15, min not over max");
    if (cfg->warmup_cach != 2 && cfg->warmup_cach != 4)
        return fail(c, "timers.warmup_cach: 2 or 4");
    return true;
}

int rdof_t3cfg_unit(const struct rdof_plan *p)
{
    for (int k = 0; k < rdof_plan_nunits(p); k++)
        if (rdof_plan_unit(p, k)->kind == RDOF_UNIT_TIER3)
            return k;
    return -1;
}

bool rdof_t3cfg_plan(const struct rdof_plan *p, int unit, rdof_t3_cfg_t *cfg, rdof_t3_policy_t *pol, char *err,
                         size_t cap)
{
    char dummy[8];
    if (!err || !cap)
        err = dummy, cap = sizeof dummy;
    err[0] = 0;
    if (unit < 0)
        unit = rdof_t3cfg_unit(p);
    if (unit < 0 || unit >= rdof_plan_nunits(p) || rdof_plan_unit(p, unit)->kind != RDOF_UNIT_TIER3) {
        snprintf(err, cap, "no trunked site (an app of type tier3)");
        return false;
    }
    const rdof_site_t *s = rdof_plan_site(p);
    const rdof_unit_t *u = rdof_plan_unit(p, unit);
    const rdof_app3_t *ap = &s->app[u->app];
    rdof_t3cfg_default(cfg, pol);
    char path[RDOF_SITE_NAME + 8];
    snprintf(path, sizeof path, "apps.%s", ap->name);
    if (!rdof_t3cfg_apply(rdof_plan_doc(p), rdof_plan_app_node(p, u->app), path, cfg, pol, err, cap))
        return false;
    memset(cfg->carriers, 0, sizeof cfg->carriers);
    cfg->ncarriers = ap->ncarrier;
    for (int k = 0; k < ap->ncarrier; k++) {
        const rdof_carrier3_t *cr = &ap->carrier[k];
        cfg->carriers[k] = (rdof_t3_carrier_t){.tx_hz = s->freq[cr->out].hz, .rx_hz = s->freq[cr->in].hz,
                                               .chan = cr->chan, .tscc = k == 0, .enabled = 1};
    }
    cfg->cc = (uint8_t)s->freq[ap->carrier[0].out].cc;
    if (!rdof_t3cfg_check(cfg, err, cap)) {
        char why[160];
        snprintf(why, sizeof why, "%s", err);
        snprintf(err, cap, "%s.%s", path, why);
        return false;
    }
    return true;
}

bool rdof_t3cfg_site(const rdof_mn_site_cfg_t *sc, rdof_t3_cfg_t *cfg, rdof_t3_policy_t *pol, char *err,
                         size_t cap)
{
    char dummy[8];
    ctx_t cx = {.err = err && cap ? err : dummy, .cap = err && cap ? cap : sizeof dummy}, *c = &cx;
    c->err[0] = 0;
    const uint32_t h = sc->has;
    if (h & RDOF_MN_HAS_MODEL) {
        if (sc->model < 0 || sc->model > 3)
            return fail(c, "cfg.system.model: tiny, small, large or huge");
        cfg->model = (uint8_t)sc->model;
        int nb, sb;
        model_bits(sc->model, &nb, &sb);
        if (!(h & RDOF_MN_HAS_AREA_BITS))
            cfg->area_bits = (uint8_t)sb;
    }
    if (h & RDOF_MN_HAS_NET)
        cfg->net = (uint16_t)sc->net;
    if (h & RDOF_MN_HAS_SITE)
        cfg->site = (uint16_t)sc->site;
    if (h & RDOF_MN_HAS_PAR)
        cfg->par = (uint8_t)sc->par;
    if (h & RDOF_MN_HAS_AREA_BITS)
        cfg->area_bits = (uint8_t)sc->area_bits;
    if (h & RDOF_MN_HAS_REGISTRATION) {
        pol->open = !sc->listed;
        if (sc->listed && !(h & RDOF_MN_HAS_MS) && pol->ms.all)
            memset(&pol->ms, 0, sizeof pol->ms);
    }
    if (h & RDOF_MN_HAS_REFUSE)
        pol->refuse_unlisted = sc->refused;
    if (((h & RDOF_MN_HAS_MS) && !to_t3(c, "cfg", "ms", &sc->ms, &pol->ms)) ||
        ((h & RDOF_MN_HAS_PRIVATE) && !to_t3(c, "cfg", "private", &sc->private_, &pol->private_ms)) ||
        ((h & RDOF_MN_HAS_TG) && !to_t3(c, "cfg", "tg", &sc->tg, &pol->tg)) ||
        ((h & RDOF_MN_HAS_WIDE_TG) && !to_t3(c, "cfg", "wide_tg", &sc->wide_tg, &pol->wide_tg)) ||
        ((h & RDOF_MN_HAS_STATIC_TG) && !to_t3(c, "cfg", "static_tg", &sc->static_tg, &pol->static_tg)))
        return false;
    if (h & RDOF_MN_HAS_REQUIRE_REG)
        cfg->reg_required = sc->require_registration;
    if (h & RDOF_MN_HAS_FOACSU)
        cfg->foacsu = sc->foacsu;
    for (int i = 0; i < sc->ntimers; i++)
        if (!set_timer(c, "cfg.timers", sc->timers[i].name, sc->timers[i].value, cfg))
            return false;
    return rdof_t3cfg_check(cfg, c->err, c->cap);
}
