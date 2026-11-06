// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* A trunked site's settings into the controller's (rdof/trunkcfg.h, docs/spec/fw/trunk.md sections 3.8 and 4). */
#include "rdof/trunkcfg.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "rdof/idset.h"
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
static const char *const TIMER_KEYS[] = {"nrand_wait", "backoff_min", "backoff_max", "tp_s", "tnp_s", "hang_s",
                                         "call_s", "emergency_call_s", "ahoy_tries", "ahoy_every_s", "grant_every_s",
                                         "clear_repeats", "warmup_cach", "reg_hold_s", "queue_max", "queue_s",
                                         "net_queue_s", "setup_s", "start_grace_s", NULL};
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
    memset(out, 0, sizeof *out);
    if (s.star && !s.nout) {
        out->all = 1;
        return true;
    }
    rdof_idrange_t r[RDOF_T3_IDSET_RANGES];
    int n = rdof_idset_ranges(&s, r, RDOF_T3_IDSET_RANGES);
    if (n > RDOF_T3_IDSET_RANGES)
        return fail(c, "%s.%s: %d ranges, the site holds %d", path, key, n, RDOF_T3_IDSET_RANGES);
    for (int k = 0; k < n; k++)
        out->r[k] = (rdof_t3_range_t){r[k].lo, r[k].hi};
    out->n = (uint8_t)n;
    return true;
}

void rdof_trunk_cfg_default(rdof_t3_cfg_t *cfg, rdof_t3_policy_t *pol)
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

static bool reg_hold(ctx_t *c, int t, const char *path, rdof_t3_cfg_t *cfg)
{
    double v = cfg->reg_hold_s;
    if (!num_in(c, t, "reg_hold_s", path, 60, 604800, false, &v, NULL))
        return false;
    cfg->reg_hold_s = (uint32_t)v;
    return true;
}

static bool timer_keys(ctx_t *c, int t, const char *path, rdof_t3_cfg_t *cfg)
{
    char p[96];
    snprintf(p, sizeof p, "%s.timers", path);
    if (!known(c, t, p, TIMER_KEYS))
        return false;
    return u8_in(c, t, "nrand_wait", p, 0, 15, &cfg->nrand_wait) &&
           u8_in(c, t, "backoff_min", p, 1, 15, &cfg->backoff_min) &&
           u8_in(c, t, "backoff_max", p, 1, 15, &cfg->backoff_max) && ms_in(c, t, "tp_s", p, 4, 60, false, &cfg->tp_ms) &&
           ms_in(c, t, "tnp_s", p, 2, 60, false, &cfg->tnp_ms) && ms_in(c, t, "hang_s", p, 1, 60, true, &cfg->hang_ms) &&
           ms_in(c, t, "call_s", p, 1, 4094, false, &cfg->call_ms) &&
           ms_in(c, t, "emergency_call_s", p, 1, 510, false, &cfg->emergency_call_ms) &&
           u8_in(c, t, "ahoy_tries", p, 1, 10, &cfg->ahoy_tries) &&
           ms_in(c, t, "ahoy_every_s", p, 0.3, 5, true, &cfg->ahoy_every_ms) &&
           ms_in(c, t, "grant_every_s", p, 0.3, 10, true, &cfg->grant_every_ms) &&
           u8_in(c, t, "clear_repeats", p, 1, 8, &cfg->clear_repeats) &&
           u8_in(c, t, "warmup_cach", p, 2, 4, &cfg->warmup_cach) &&
           (cfg->warmup_cach != 3 || fail(c, "%s.warmup_cach: 2 or 4", p)) &&
           u8_in(c, t, "queue_max", p, 1, 16, &cfg->queue_max) &&
           ms_in(c, t, "queue_s", p, 1, 60, false, &cfg->queue_ms) &&
           ms_in(c, t, "net_queue_s", p, 0, 10, true, &cfg->net_queue_ms) &&
           ms_in(c, t, "setup_s", p, 1, 30, true, &cfg->setup_ms) &&
           ms_in(c, t, "start_grace_s", p, 0, 10, true, &cfg->start_grace_ms) && reg_hold(c, t, p, cfg);
}

bool rdof_trunk_cfg_apply(const rdof_jdoc_t *doc, int obj, const char *path, rdof_t3_cfg_t *cfg,
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

bool rdof_trunk_cfg_check(const rdof_t3_cfg_t *cfg, char *err, size_t cap)
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

int rdof_trunk_unit(const struct rdof_plan *p)
{
    for (int k = 0; k < rdof_plan_nunits(p); k++)
        if (rdof_plan_unit(p, k)->kind == RDOF_UNIT_TRUNK)
            return k;
    return -1;
}

bool rdof_trunk_cfg_plan(const struct rdof_plan *p, int unit, rdof_t3_cfg_t *cfg, rdof_t3_policy_t *pol, char *err,
                         size_t cap)
{
    char dummy[8];
    if (!err || !cap)
        err = dummy, cap = sizeof dummy;
    err[0] = 0;
    if (unit < 0)
        unit = rdof_trunk_unit(p);
    if (unit < 0 || unit >= rdof_plan_nunits(p) || rdof_plan_unit(p, unit)->kind != RDOF_UNIT_TRUNK) {
        snprintf(err, cap, "no trunked site (an app of type trunk)");
        return false;
    }
    const rdof_site_t *s = rdof_plan_site(p);
    const rdof_unit_t *u = rdof_plan_unit(p, unit);
    const rdof_app3_t *ap = &s->app[u->app];
    rdof_trunk_cfg_default(cfg, pol);
    char path[RDOF_SITE_NAME + 8];
    snprintf(path, sizeof path, "apps.%s", ap->name);
    if (!rdof_trunk_cfg_apply(rdof_plan_doc(p), rdof_plan_app_node(p, u->app), path, cfg, pol, err, cap))
        return false;
    memset(cfg->carriers, 0, sizeof cfg->carriers);
    cfg->ncarriers = ap->ncarrier;
    for (int k = 0; k < ap->ncarrier; k++) {
        const rdof_carrier3_t *cr = &ap->carrier[k];
        cfg->carriers[k] = (rdof_t3_carrier_t){.tx_hz = s->freq[cr->out].hz, .rx_hz = s->freq[cr->in].hz,
                                               .chan = cr->chan, .tscc = k == 0, .enabled = 1};
    }
    cfg->cc = (uint8_t)s->freq[ap->carrier[0].out].cc;
    if (!rdof_trunk_cfg_check(cfg, err, cap)) {
        char why[160];
        snprintf(why, sizeof why, "%s", err);
        snprintf(err, cap, "%s.%s", path, why);
        return false;
    }
    return true;
}
