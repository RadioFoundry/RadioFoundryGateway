// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The SDR board's settings, as JSON (rdof/sdr/config.h). */
#include "rdof/sdr/config.h"

#if RDOF_FEATURE_SDR

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/jsonp.h"
#include "rdof/sdr/frontend.h"
#include "rdof/status.h"
#include "sdr_private.h"

#define MAX_NODES 1200 /* a box config, as common's */
#define STR_BUF 8192

typedef struct {
    rdof_jdoc_t doc;
    rdof_jnode_t nodes[MAX_NODES];
    char strs[STR_BUF];
    char *error;
    size_t cap;
} ctx_t;

static bool fail(ctx_t *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->error, c->cap, fmt, ap);
    va_end(ap);
    return false;
}

/* JSONC to JSON: comments out (kept as spaces), trailing commas before ] or } dropped. */
static size_t strip_jsonc(const char *in, size_t n, char *out)
{
    size_t o = 0;
    bool str = false;
    for (size_t i = 0; i < n; i++) {
        char ch = in[i];
        if (str) {
            out[o++] = ch;
            if (ch == '\\' && i + 1 < n)
                out[o++] = in[++i];
            else if (ch == '"')
                str = false;
        } else if (ch == '"') {
            str = true;
            out[o++] = ch;
        } else if (ch == '/' && i + 1 < n && in[i + 1] == '/') {
            while (i < n && in[i] != '\n')
                i++;
            if (i < n)
                out[o++] = '\n';
        } else if (ch == '/' && i + 1 < n && in[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(in[i] == '*' && in[i + 1] == '/'))
                i++;
            i++;
            out[o++] = ' ';
        } else if (ch == ']' || ch == '}') {
            size_t k = o;
            while (k > 0 && (out[k - 1] == ' ' || out[k - 1] == '\n' || out[k - 1] == '\t' || out[k - 1] == '\r'))
                k--;
            if (k > 0 && out[k - 1] == ',')
                out[k - 1] = ' ';
            out[o++] = ch;
        } else {
            out[o++] = ch;
        }
    }
    out[o] = 0;
    return o;
}

static const rdof_jnode_t *get(ctx_t *c, int obj, const char *key)
{
    if (obj < 0)
        return NULL;
    int k = rdof_jget(&c->doc, obj, key);
    const rdof_jnode_t *n = k >= 0 ? rdof_jnode(&c->doc, k) : NULL;
    return n && n->type != RDOF_JNULL ? n : NULL;
}

static int obj_of(ctx_t *c, int obj, const char *key)
{
    if (obj < 0)
        return -1;
    int k = rdof_jget(&c->doc, obj, key);
    return k >= 0 && rdof_jnode(&c->doc, k)->type == RDOF_JOBJ ? k : -1;
}

static void str_or(ctx_t *c, int obj, const char *key, char *out, size_t cap, const char *dflt)
{
    const rdof_jnode_t *n = get(c, obj, key);
    if (n && n->type == RDOF_JSTR)
        snprintf(out, cap, "%s", n->str);
    else if (n && n->type == RDOF_JNUM)
        snprintf(out, cap, "%.15g", n->num);
    else
        snprintf(out, cap, "%s", dflt);
}

static bool num(const rdof_jnode_t *n, double *v)
{
    if (!n)
        return false;
    if (n->type == RDOF_JNUM) {
        *v = n->num;
        return true;
    }
    if (n->type == RDOF_JSTR) {
        char *end;
        *v = strtod(n->str, &end);
        return end != n->str && *end == 0;
    }
    if (n->type == RDOF_JBOOL) {
        *v = n->b;
        return true;
    }
    return false;
}

static double num_or(ctx_t *c, int obj, const char *key, double dflt)
{
    double v;
    return num(get(c, obj, key), &v) ? v : dflt;
}

static bool truthy(const rdof_jnode_t *n)
{
    if (!n)
        return false;
    switch (n->type) {
    case RDOF_JBOOL: return n->b;
    case RDOF_JNUM: return n->num != 0;
    case RDOF_JSTR: return n->str[0] != 0;
    case RDOF_JARR:
    case RDOF_JOBJ: return n->count > 0;
    default: return false;
    }
}

static double hz(double v) { return v < 1e5 ? v * 1e6 : v; }

static bool device(ctx_t *c, int dev, rdof_sdr_config_t *g)
{
    g->sample_rate = num_or(c, dev, "sample_rate", 1.536e6);
    const rdof_jnode_t *n = get(c, dev, "center");
    double v;
    g->center = n && !(n->type == RDOF_JSTR && !strcmp(n->str, "auto")) && num(n, &v) ? hz(v) : 0.0;
    n = get(c, dev, "gain");
    g->has_gain = n && !(n->type == RDOF_JSTR && !strcmp(n->str, "auto")) && num(n, &g->gain);
    g->ppm = num_or(c, dev, "ppm", 0.0);
    g->biastee = truthy(get(c, obj_of(c, dev, "settings"), "biastee"));
    n = get(c, obj_of(c, dev, "settings"), "iq_bits");
    g->iq_bits = 0;
    if (n && (!num(n, &v) || (v != 8 && v != 16)))
        return fail(c, "device.settings.iq_bits must be 8 or 16");
    if (n)
        g->iq_bits = (int)v;
    n = get(c, dev, "type");
    g->frontend = rdof_fe_type_parse(n && n->type == RDOF_JSTR ? n->str : NULL);
    if (n && (n->type != RDOF_JSTR || g->frontend < 0))
        return fail(c, "device.type must be rtlsdr, lr2021, sx1255 or sim");
    n = get(c, dev, "agc");
    g->agc = truthy(n);
    static const char *const agc_keys[6] = {"min", "clip", "headroom_db", "hold_s", "relax_s", "settle_s"};
    static const double agc_dflt[6] = {0.0, 1e-4, 6.0, 10.0, 300.0, 0.3};
    int agc = obj_of(c, dev, "agc");
    for (int i = 0; i < 6; i++)
        g->agc_cfg[i] = num_or(c, agc, agc_keys[i], agc_dflt[i]);
    if (g->agc && !g->has_gain)
        return fail(c, "device.agc needs a fixed device.gain (the most it may use)");
    double d = g->sample_rate / 48000.0;
    if (fabs(d - nearbyint(d)) > 1e-6)
        return fail(c, "sample_rate must be a multiple of 48000 (got %.0f)", g->sample_rate);
    return true;
}

static void mqtt(ctx_t *c, int mq, const char *app, rdof_sdr_config_t *g)
{
    g->has_mqtt = mq >= 0;
    str_or(c, mq, "host", g->mqtt_host, sizeof g->mqtt_host, "localhost");
    g->mqtt_port = (int)num_or(c, mq, "port", 1883);
    str_or(c, mq, "username", g->mqtt_user, sizeof g->mqtt_user, "");
    str_or(c, mq, "password", g->mqtt_pass, sizeof g->mqtt_pass, "");
    str_or(c, mq, "root", g->mqtt_root, sizeof g->mqtt_root, "DMR");
    if (app)
        snprintf(g->mqtt_app, sizeof g->mqtt_app, "%s", app);
    else
        str_or(c, mq, "app", g->mqtt_app, sizeof g->mqtt_app, "sdr");
    g->mqtt_tls = truthy(get(c, mq, "tls"));
    /* the root's slashes are stripped, both ends */
    size_t rl = strlen(g->mqtt_root), lead = strspn(g->mqtt_root, "/");
    while (rl > lead && g->mqtt_root[rl - 1] == '/')
        rl--;
    memmove(g->mqtt_root, g->mqtt_root + lead, rl - lead);
    g->mqtt_root[rl - lead] = 0;
}

static bool location(ctx_t *c, int loc, rdof_sdr_config_t *g)
{
    if (loc < 0 || rdof_jnode(&c->doc, loc)->count == 0)
        return true;
    double lat, lon;
    if (!num(get(c, loc, "latitude"), &lat) && !num(get(c, loc, "lat"), &lat))
        return fail(c, "location needs latitude");
    if (!num(get(c, loc, "longitude"), &lon) && !num(get(c, loc, "lon"), &lon))
        return fail(c, "location needs longitude");
    if (!(-90 <= lat && lat <= 90 && -180 <= lon && lon <= 180))
        return fail(c, "location %g, %g out of range", lat, lon);
    double h = nearbyint(num_or(c, loc, "height", 0.0));
    g->has_location = true;
    g->location.latitude = lat;
    g->location.longitude = lon;
    g->location.height_m = (int)(h < 0 ? 0 : h > 999 ? 999 : h);
    return true;
}

/* rssi_offset, afc, status_seconds and the board's own keys, from obj (the settings, or a box's SDR section). */
static void rest(ctx_t *c, int obj, rdof_sdr_config_t *g)
{
    g->has_rssi_offset = num(get(c, obj, "rssi_offset"), &g->rssi_offset);
    const rdof_jnode_t *n = get(c, obj, "afc");
    g->afc = n ? truthy(n) : true;
    g->status_seconds = num_or(c, obj, "status_seconds", 30.0);
    int usb = obj_of(c, obj, "usb");
    g->usb_transfers = (int)num_or(c, usb, "transfers", 8);
    g->usb_transfer_kb = (int)num_or(c, usb, "transfer_kb", 8);
    g->dsp_cores = (int)num_or(c, obj, "dsp_cores", 1) >= 2 ? 2 : 1;
    g->spike = truthy(get(c, obj, "spike"));
    g->selftest = truthy(get(c, obj, "selftest"));
    g->sim = truthy(get(c, obj, "sim")) || g->frontend == RDOF_FE_SIM;
    if (g->sim)
        g->frontend = RDOF_FE_SIM;
    n = get(c, obj, "idle_gate");
    g->idle_gate = n ? truthy(n) : true;
    n = get(c, obj, "learn_ppm");
    g->learn_ppm = n ? truthy(n) : true;
    g->sim_ppm = num_or(c, obj, "sim_ppm", 0.0);
}

/* The channel just filled in (g->ch[g->nch]): no frequency twice. */
static bool add_channel(ctx_t *c, rdof_sdr_config_t *g)
{
    const rdof_sdr_cfg_channel_t *ch = &g->ch[g->nch];
    for (int k = 0; k < g->nch; k++)
        if (g->ch[k].freq == ch->freq)
            return fail(c, "channels %s and %s are both on %.5f MHz", g->ch[k].id, ch->id, ch->freq / 1e6);
    g->nch++;
    return true;
}

static bool parse(ctx_t *c, int root, rdof_sdr_config_t *g)
{
    str_or(c, root, "name", g->name, sizeof g->name, "sdr");
    if (!device(c, obj_of(c, root, "device"), g))
        return false;
    mqtt(c, obj_of(c, root, "mqtt"), NULL, g);

    int hb = obj_of(c, root, "hblink");
    g->hb_enabled = truthy(get(c, hb, "enabled"));
    str_or(c, hb, "host", g->hb_host, sizeof g->hb_host, "localhost");
    g->hb_port = (int)num_or(c, hb, "port", 62031);
    str_or(c, hb, "callsign", g->hb_callsign, sizeof g->hb_callsign, "SDR");
    double peer_base;
    bool has_base = num(get(c, hb, "peer_id_base"), &peer_base);

    if (!location(c, obj_of(c, root, "location"), g))
        return false;
    rest(c, root, g);
    {
        char why[96];
        if (!rdof_slot_timing_parse(&c->doc, -1, rdof_jget(&c->doc, root, "slot_timing"), &g->slot_timing, why,
                                  sizeof why))
            return fail(c, "%s", why);
    }

    int defs = obj_of(c, root, "defaults");
    int chs = rdof_jget(&c->doc, root, "channels");
    const rdof_jnode_t *cn = chs >= 0 ? rdof_jnode(&c->doc, chs) : NULL;
    if (!cn || cn->type != RDOF_JARR || cn->count == 0)
        return fail(c, "no channels configured");
    if (cn->count > RDOF_SDR_MAX_CHANNELS)
        return fail(c, "%d channels: this board takes %d", cn->count, RDOF_SDR_MAX_CHANNELS);
    for (int i = 0; i < cn->count; i++) {
        int e = rdof_jat(&c->doc, chs, i);
        if (rdof_jnode(&c->doc, e)->type != RDOF_JOBJ)
            return fail(c, "channels[%d] isn't an object", i);
        rdof_sdr_cfg_channel_t *ch = &g->ch[g->nch];
        double v;
        /* a channel's own keys over the defaults, as {**defaults, **c} */
#define PICK(key) (rdof_jget(&c->doc, e, key) >= 0 ? e : defs)
        const rdof_jnode_t *idn = get(c, PICK("id"), "id");
        if (!idn)
            return fail(c, "channels[%d] has no id", i);
        str_or(c, PICK("id"), "id", ch->id, sizeof ch->id, "");
        double f;
        if (!num(get(c, PICK("freq"), "freq"), &f))
            return fail(c, "channel %s has no freq", ch->id);
        ch->freq = hz(f);
        str_or(c, PICK("name"), "name", ch->name, sizeof ch->name, "");
        ch->cc = num(get(c, PICK("cc"), "cc"), &v) ? (int)v : -1;
        ch->slot = (int)num_or(c, PICK("slot"), "slot", 1);
        ch->tg = num(get(c, PICK("tg"), "tg"), &v) ? (long)v : -1;
        const rdof_jnode_t *hn = rdof_jget(&c->doc, PICK("hblink"), "hblink") >= 0
                                   ? rdof_jnode(&c->doc, rdof_jget(&c->doc, PICK("hblink"), "hblink"))
                                   : NULL;
        ch->hblink = hn ? truthy(hn) : true;
        ch->peer_id = num(get(c, PICK("peer_id"), "peer_id"), &v) ? (long)v : (has_base ? (long)peer_base + i : -1);
#undef PICK
        if (!add_channel(c, g))
            return false;
    }
    return true;
}

/* ── a box config's SDR section ───────────────────────────────────── */

/* Is `name` in "run" (a name or a list of names)? */
static bool in_run(ctx_t *c, int run, const char *name)
{
    const rdof_jnode_t *n = rdof_jnode(&c->doc, run);
    if (n->type == RDOF_JSTR)
        return !strcmp(n->str, name);
    for (int k = 0; n->type == RDOF_JARR && k < n->count; k++) {
        const rdof_jnode_t *e = rdof_jnode(&c->doc, rdof_jat(&c->doc, run, k));
        if (e->type == RDOF_JSTR && !strcmp(e->str, name))
            return true;
    }
    return false;
}

/* receiver_ids for channel n: str.format with {ch}, {ch:d}, {ch:3d} or {ch:02d}, and {{ }}. */
static bool format_id(ctx_t *c, const char *fmt, int n, char *out, size_t cap)
{
    size_t o = 0;
    for (const char *p = fmt; *p && o + 1 < cap; p++) {
        if ((p[0] == '{' && p[1] == '{') || (p[0] == '}' && p[1] == '}')) {
            out[o++] = *p++;
            continue;
        }
        if (*p != '{') {
            out[o++] = *p;
            continue;
        }
        const char *close = strchr(p, '}');
        char spec[16] = "";
        int width = 0;
        bool zero = false;
        if (!close || strncmp(p, "{ch", 3) || (p[3] != '}' && p[3] != ':'))
            return fail(c, "receiver_ids %s: only {ch} or {ch:02d}", fmt);
        if (p[3] == ':') {
            snprintf(spec, sizeof spec, "%.*s", (int)(close - p - 4), p + 4);
            char *end;
            zero = spec[0] == '0';
            width = (int)strtol(spec, &end, 10);
            if ((*end && strcmp(end, "d")) || width > 9)
                return fail(c, "receiver_ids %s: only {ch} or {ch:02d}", fmt);
        }
        int w = snprintf(out + o, cap - o, zero ? "%0*d" : "%*d", width, n);
        o += w > 0 ? (size_t)w : 0;
        if (o >= cap)
            o = cap - 1;
        p = close;
    }
    out[o] = 0;
    return true;
}

/* The box's channel plan: channel n's object (or bare frequency), or -1. */
static int plan_channel(ctx_t *c, int box, int n)
{
    int plan = obj_of(c, box, "channels");
    for (int k = 0; plan >= 0 && k < rdof_jnode(&c->doc, plan)->count; k++) {
        int e = rdof_jat(&c->doc, plan, k);
        char *end;
        long key = strtol(rdof_jnode(&c->doc, e)->key, &end, 10);
        if (!*end && key == n)
            return e;
    }
    return -1;
}

static bool parse_box(ctx_t *c, int box, rdof_sdr_config_t *g)
{
    int radios = rdof_jget(&c->doc, box, "radios"), run = rdof_jget(&c->doc, box, "run"), r = -1, k = 0;
    char box_name[24], name[48];
    str_or(c, box, "name", box_name, sizeof box_name, "radiofoundry");
    if (!truthy(get(c, box, "name")))
        snprintf(box_name, sizeof box_name, "radiofoundry");
    const rdof_jnode_t *rn = radios >= 0 ? rdof_jnode(&c->doc, radios) : NULL;
    for (; rn && rn->type == RDOF_JARR && k < rn->count; k++) {
        int e = rdof_jat(&c->doc, radios, k);
        const rdof_jnode_t *t = get(c, e, "type"), *en = get(c, e, "enabled");
        if (!t || t->type != RDOF_JSTR || strcmp(t->str, "sdr") || (en && en->type == RDOF_JBOOL && !en->b))
            continue; /* not an SDR, or "enabled": false (the box config refuses an enabled that isn't a bool) */
        char dflt[48];
        snprintf(dflt, sizeof dflt, "%s-sdr%d", box_name, k + 1);
        str_or(c, e, "name", name, sizeof name, dflt);
        if (run < 0 || in_run(c, run, name)) {
            r = e;
            break;
        }
    }
    if (r < 0)
        return fail(c, run >= 0 ? "run names no SDR in radios" : "no SDR in radios");
    snprintf(g->name, sizeof g->name, "%.*s", (int)sizeof g->name - 1, name);
    if (!device(c, obj_of(c, r, "device"), g))
        return false;
    int mq = obj_of(c, box, "mqtt");
    mqtt(c, mq, "sdr", g);

    /* HBlink4 when the box has an hblink section and this SDR doesn't say hblink: false */
    int hb = obj_of(c, box, "hblink");
    const rdof_jnode_t *off = get(c, r, "hblink");
    g->hb_enabled = hb >= 0 && rdof_jnode(&c->doc, hb)->count > 0 && !(off && off->type == RDOF_JBOOL && !off->b);
    str_or(c, hb, "host", g->hb_host, sizeof g->hb_host, "localhost");
    g->hb_port = (int)num_or(c, hb, "port", 62031);
    str_or(c, r, "callsign", g->hb_callsign, sizeof g->hb_callsign, "SDR");

    /* r.get("location", box location) */
    int loc = rdof_jget(&c->doc, r, "location") >= 0 ? obj_of(c, r, "location") : obj_of(c, box, "location");
    if (!location(c, loc, g))
        return false;
    rest(c, r, g);
    {
        char why[96];
        rdof_slot_timing_t box_t;
        if (!rdof_slot_timing_parse(&c->doc, -1, rdof_jget(&c->doc, box, "slot_timing"), &box_t, why, sizeof why))
            return fail(c, "box: %s", why);
        if (!rdof_slot_timing_parse(&c->doc, rdof_jget(&c->doc, box, "slot_timing"), rdof_jget(&c->doc, r, "slot_timing"),
                                  &g->slot_timing, why, sizeof why))
            return fail(c, "%s", why);
    }

    const rdof_jnode_t *dcc = get(c, box, "cc");
    /* every plan channel has a frequency and a color code */
    int plan = obj_of(c, box, "channels");
    for (int i = 0; plan >= 0 && i < rdof_jnode(&c->doc, plan)->count; i++) {
        int e = rdof_jat(&c->doc, plan, i);
        int obj = rdof_jnode(&c->doc, e)->type == RDOF_JOBJ ? e : -1;
        double f;
        if (!num(obj >= 0 ? get(c, obj, "freq") : rdof_jnode(&c->doc, e), &f))
            return fail(c, "channel %s: no freq", rdof_jnode(&c->doc, e)->key);
        if (!(obj >= 0 && rdof_jget(&c->doc, obj, "cc") >= 0 ? get(c, obj, "cc") : dcc))
            return fail(c, "channel %s: no color code (set cc on it or at the top)", rdof_jnode(&c->doc, e)->key);
    }
    char ids[32];
    str_or(c, r, "receiver_ids", ids, sizeof ids, "SDR{ch:02d}");
    double base;
    bool has_base = num(get(c, r, "peer_ids"), &base);
    int chs = rdof_jget(&c->doc, r, "channels");
    const rdof_jnode_t *cn = chs >= 0 ? rdof_jnode(&c->doc, chs) : NULL;
    if (!cn || cn->type != RDOF_JARR || cn->count == 0)
        return fail(c, "no channels configured");
    if (cn->count > RDOF_SDR_MAX_CHANNELS)
        return fail(c, "%d channels: this board takes %d", cn->count, RDOF_SDR_MAX_CHANNELS);
    for (int i = 0; i < cn->count; i++) {
        int e = rdof_jat(&c->doc, chs, i), over = -1;
        double v;
        const rdof_jnode_t *ref = rdof_jnode(&c->doc, e);
        if (ref->type == RDOF_JOBJ) {
            over = e;
            if (!num(get(c, e, "ch"), &v))
                return fail(c, "radio %s: channels[%d] has no ch", g->name, i);
        } else if (!num(ref, &v)) {
            return fail(c, "radio %s: channels[%d] isn't a channel number", g->name, i);
        }
        int n = (int)v, pc = plan_channel(c, box, n);
        if (pc < 0)
            return fail(c, "radio %s: channel %d isn't in the plan (channels:)", g->name, n);
        int pobj = rdof_jnode(&c->doc, pc)->type == RDOF_JOBJ ? pc : -1;
        rdof_sdr_cfg_channel_t *ch = &g->ch[g->nch];
        /* the plan's channel, then the radio's overrides for it */
#define PICK(key) (over >= 0 && rdof_jget(&c->doc, over, key) >= 0 ? over : pobj)
        if (over >= 0 && rdof_jget(&c->doc, over, "id") >= 0)
            str_or(c, over, "id", ch->id, sizeof ch->id, "");
        else if (!format_id(c, ids, n, ch->id, sizeof ch->id))
            return false;
        double f;
        const rdof_jnode_t *fn = over >= 0 && rdof_jget(&c->doc, over, "freq") >= 0 ? get(c, over, "freq")
                               : pobj >= 0                                      ? get(c, pobj, "freq")
                                                                                : rdof_jnode(&c->doc, pc);
        if (!num(fn, &f))
            return fail(c, "radio %s: channel %d: bad frequency", g->name, n);
        /* the plan's frequency is whole Hz (box._hz); an override goes through as written */
        ch->freq = over >= 0 && rdof_jget(&c->doc, over, "freq") >= 0 ? hz(f) : nearbyint(hz(f));
        str_or(c, PICK("name"), "name", ch->name, sizeof ch->name, "");
        const rdof_jnode_t *ccn = PICK("cc") >= 0 && rdof_jget(&c->doc, PICK("cc"), "cc") >= 0 ? get(c, PICK("cc"), "cc")
                                                                                          : dcc;
        ch->cc = num(ccn, &v) ? (int)v : -1;
        ch->slot = (int)num_or(c, PICK("slot"), "slot", 1);
        ch->tg = num(get(c, PICK("tg"), "tg"), &v) ? (long)v : -1;
        const rdof_jnode_t *hn = over >= 0 && rdof_jget(&c->doc, over, "hblink") >= 0
                                   ? rdof_jnode(&c->doc, rdof_jget(&c->doc, over, "hblink"))
                                   : NULL;
        ch->hblink = hn ? truthy(hn) : true;
        if (over >= 0 && rdof_jget(&c->doc, over, "peer_id") >= 0)
            ch->peer_id = num(get(c, over, "peer_id"), &v) ? (long)v : -1;
        else
            ch->peer_id = has_base ? (long)base + n : -1;
#undef PICK
        if (!add_channel(c, g))
            return false;
    }
    return true;
}

bool rdof_sdr_config_parse(const char *text, size_t len, rdof_sdr_config_t *cfg, char *error, size_t cap)
{
    char dummy[8];
    memset(cfg, 0, sizeof *cfg);
    ctx_t *c = rdof_sdr_alloc(sizeof *c);
    char *clean = rdof_sdr_alloc(len + 1);
    if (!c || !clean) {
        rdof_sdr_free(c);
        rdof_sdr_free(clean);
        if (error && cap)
            snprintf(error, cap, "no memory");
        return false;
    }
    c->error = error && cap ? error : dummy;
    c->cap = error && cap ? cap : sizeof dummy;
    c->error[0] = 0;
    c->doc.nodes = c->nodes;
    c->doc.max = MAX_NODES;
    c->doc.buf = c->strs;
    c->doc.cap = STR_BUF;
    size_t n = strip_jsonc(text, len, clean);
    int root = rdof_jparse(&c->doc, clean, n);
    bool ok;
    if (root < 0)
        ok = fail(c, "JSON: %s", c->doc.error);
    else if (rdof_jnode(&c->doc, root)->type != RDOF_JOBJ)
        ok = fail(c, "the settings must be a JSON object");
    else if (rdof_jget(&c->doc, root, "radios") >= 0)
        ok = parse_box(c, root, cfg);
    else
        ok = parse(c, root, cfg);
    rdof_sdr_free(clean);
    rdof_sdr_free(c);
    return ok;
}

double rdof_sdr_config_rssi_offset(const rdof_sdr_config_t *g)
{
    if (g->has_rssi_offset)
        return g->rssi_offset;
    return -(g->has_gain ? g->gain : 30.0) - 25.0;
}

size_t rdof_sdr_config_describe(const rdof_sdr_config_t *g, char *out, size_t cap)
{
    char a[32], b[32], e[32];
    size_t o = 0;
#define PUT(...) o += (size_t)snprintf(out + (o < cap ? o : cap), o < cap ? cap - o : 0, __VA_ARGS__)
    rdof_format_repr(g->sample_rate, a, sizeof a);
    rdof_format_repr(rdof_sdr_config_rssi_offset(g), b, sizeof b);
    rdof_format_repr(g->ppm, e, sizeof e);
    PUT("name %s; rate %s; center ", g->name, a);
    if (g->center) {
        rdof_format_repr(g->center, a, sizeof a);
        PUT("%s", a);
    } else {
        PUT("auto");
    }
    if (g->has_gain) {
        rdof_format_repr(g->gain, a, sizeof a);
        PUT("; gain %s", a);
    } else {
        PUT("; gain auto");
    }
    PUT("; agc %d; ppm %s; biastee %d; rssi_offset %s; afc %d\n", g->agc, e, g->biastee, b, g->afc);
    PUT("mqtt %s:%d root %s app %s user %s tls %d\n", g->mqtt_host, g->mqtt_port, g->mqtt_root, g->mqtt_app,
        g->mqtt_user, g->mqtt_tls);
    PUT("hblink %d %s:%d %s\n", g->hb_enabled, g->hb_host, g->hb_port, g->hb_callsign);
    if (g->has_location) {
        rdof_format_repr(g->location.latitude, a, sizeof a);
        rdof_format_repr(g->location.longitude, b, sizeof b);
        PUT("location %s %s %d\n", a, b, g->location.height_m);
    }
    if (g->slot_timing.utc) {
        rdof_slot_timing_describe(&g->slot_timing, a, sizeof a);
        PUT("timing %s\n", a);
    }
    for (int i = 0; i < g->nch; i++) {
        const rdof_sdr_cfg_channel_t *ch = &g->ch[i];
        rdof_format_repr(ch->freq, a, sizeof a);
        PUT("channel %s %s \"%s\" cc %d slot %d tg %ld peer %ld hblink %d\n", ch->id, a, ch->name, ch->cc, ch->slot,
            ch->tg, ch->peer_id, ch->hblink);
    }
#undef PUT
    return o < cap ? o : cap ? cap - 1 : 0;
}

#endif
