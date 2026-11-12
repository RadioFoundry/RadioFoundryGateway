// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The SDR's settings, from the settings (version 3; rdof/sdr/config.h). */
#include "rdof/sdr/config.h"

#if RDOF_FEATURE_SDR

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/jsonp.h"
#include "rdof/siteplan.h"
#include "rdof/sdr/frontend.h"
#include "rdof/status.h"
#include "sdr_private.h"

typedef struct {
    const rdof_jdoc_t *doc;
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

static const rdof_jnode_t *get(ctx_t *c, int obj, const char *key)
{
    if (obj < 0)
        return NULL;
    int k = rdof_jget(c->doc, obj, key);
    const rdof_jnode_t *n = k >= 0 ? rdof_jnode(c->doc, k) : NULL;
    return n && n->type != RDOF_JNULL ? n : NULL;
}

static int obj_of(ctx_t *c, int obj, const char *key)
{
    if (obj < 0)
        return -1;
    int k = rdof_jget(c->doc, obj, key);
    return k >= 0 && rdof_jnode(c->doc, k)->type == RDOF_JOBJ ? k : -1;
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
    if (loc < 0 || rdof_jnode(c->doc, loc)->count == 0)
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

/* ── the plan (rdof/siteplan.h) ───────────────────────────────────────────── */

/* The front end: the device's driver and its rx keys. */
static bool device(ctx_t *c, int d, const char *where, rdof_sdr_config_t *g)
{
    int rx = obj_of(c, d, "rx");
    g->sample_rate = num_or(c, rx, "rate", 1.536e6);
    const rdof_jnode_t *n = get(c, rx, "center_hz");
    double v;
    g->center = n && !(n->type == RDOF_JSTR && !strcmp(n->str, "auto")) && num(n, &v) ? hz(v) : 0.0;
    n = get(c, d, "driver");
    const char *drv = n && n->type == RDOF_JSTR ? n->str : NULL;
    if (drv && (!strcmp(drv, "soapy") || !strcmp(drv, "iio")))
        drv = NULL; /* the host tool's: the front end's default here */
    g->frontend = rdof_fe_type_parse(drv);
    if (g->frontend < 0)
        return fail(c, "%s.driver: rtlsdr, lr2021, sx1255 or sim", where);
    n = get(c, rx, "gain_db");
    g->has_gain = n && !(n->type == RDOF_JSTR && !strcmp(n->str, "auto")) && num(n, &g->gain);
    if (!n && g->frontend == RDOF_FE_LR2021) { /* a fixed step unless "auto" asks for the chip's AGC */
        g->has_gain = true;
        g->gain = RDOF_LR2021_GAIN_DEFAULT_DB;
    }
    g->ppm = num_or(c, rx, "ppm", 0.0);
    g->biastee = truthy(get(c, rx, "biastee"));
    n = get(c, rx, "iq_bits");
    g->iq_bits = 0;
    if (n && (!num(n, &v) || (v != 8 && v != 16)))
        return fail(c, "%s.rx.iq_bits: 8 or 16", where);
    if (n)
        g->iq_bits = (int)v;
    n = get(c, rx, "agc");
    g->agc = truthy(n);
    static const char *const agc_keys[6] = {"min", "clip", "headroom_db", "hold_s", "relax_s", "settle_s"};
    static const double agc_dflt[6] = {0.0, 1e-4, 6.0, 10.0, 300.0, 0.3};
    int agc = obj_of(c, rx, "agc");
    for (int i = 0; i < 6; i++)
        g->agc_cfg[i] = num_or(c, agc, agc_keys[i], agc_dflt[i]);
    if (g->agc && !g->has_gain)
        return fail(c, "%s.rx.agc needs a fixed rx.gain_db (the most it may use)", where);
    double q = g->sample_rate / 48000.0;
    if (fabs(q - nearbyint(q)) > 1e-6)
        return fail(c, "%s.rx.rate: a multiple of 48000 (got %.0f)", where, g->sample_rate);
    return true;
}

/* The SDR a name means: a unit's (its lowest-numbered SDR), or an SDR a unit uses. NULL: the first a unit uses. */
static int sdr_of(ctx_t *c, const rdof_plan_t *p, const char *name)
{
    const rdof_site_t *s = rdof_plan_site(p);
    if (!name) {
        for (int k = 0; k < rdof_plan_nunits(p); k++)
            for (int h = 0; h < 2; h++) {
                int d = h ? rdof_plan_unit(p, k)->tx : rdof_plan_unit(p, k)->rx;
                if (d >= 0 && s->dev[d].kind == RDOF_DEV_SDR)
                    return d;
            }
        fail(c, "nothing uses an SDR");
        return -1;
    }
    int k = rdof_plan_unit_find(p, name);
    if (k >= 0) {
        const rdof_unit_t *u = rdof_plan_unit(p, k);
        int d = -1;
        for (int h = 0; h < 2; h++) {
            int x = h ? u->tx : u->rx;
            if (x >= 0 && s->dev[x].kind == RDOF_DEV_SDR && (d < 0 || x < d))
                d = x;
        }
        if (d < 0)
            fail(c, "%s uses no SDR", name);
        return d;
    }
    for (int d = 0; d < s->ndev; d++)
        if (!strcmp(s->dev[d].name, name) && s->dev[d].kind == RDOF_DEV_SDR) {
            if (!rdof_plan_sdr_used(p, d)) {
                fail(c, "devices.%s: nothing uses it", name);
                return -1;
            }
            return d;
        }
    fail(c, "%s isn't a unit or an SDR in these settings", name);
    return -1;
}

static int freq_cc(const rdof_plan_t *p, int f)
{
    const rdof_site_t *s = rdof_plan_site(p);
    if (s->freq[f].cc >= 0)
        return s->freq[f].cc;
    const rdof_jdoc_t *doc = rdof_plan_doc(p);
    int d = rdof_jget(doc, rdof_jget(doc, rdof_plan_root(p), "defaults"), "cc");
    double v;
    return d >= 0 && num(rdof_jnode(doc, d), &v) ? (int)v : -1;
}

bool rdof_plan_sdr(const rdof_plan_t *p, const char *name, rdof_sdr_config_t *g, char *error, size_t cap)
{
    char dummy[8];
    ctx_t cx = {.doc = rdof_plan_doc(p), .error = error && cap ? error : dummy, .cap = error && cap ? cap : sizeof dummy};
    ctx_t *c = &cx;
    c->error[0] = 0;
    memset(g, 0, sizeof *g);
    const rdof_site_t *s = rdof_plan_site(p);
    int d = sdr_of(c, p, name);
    if (d < 0)
        return false;
    int root = rdof_plan_root(p), dn = rdof_plan_dev_node(p, d), rcv = rdof_plan_receiver(p, d);
    char where[RDOF_SITE_NAME + 16];
    snprintf(where, sizeof where, "devices.%s", s->dev[d].name);
    snprintf(g->name, sizeof g->name, "%.*s", (int)sizeof g->name - 1, rdof_plan_sdr_name(p, d));
    if (!device(c, dn, where, g))
        return false;
    int mk = rdof_plan_net_of(p, RDOF_NET_MQTT);
    mqtt(c, mk >= 0 && (s->net[mk].report || s->net[mk].calls) ? rdof_plan_net_node(p, mk) : -1, "sdr", g);
    /* HBlink4 when there's an hblink network and the SDR has a receiver */
    int hk = rdof_plan_net_of(p, RDOF_NET_HBLINK), hb = rdof_plan_net_node(p, hk);
    g->hb_enabled = hk >= 0 && rcv >= 0;
    str_or(c, hb, "host", g->hb_host, sizeof g->hb_host, "localhost");
    g->hb_login = hk >= 0 && s->net[hk].login; /* brandmeister or homebrew */
    g->hb_dl5di = g->hb_login && !s->net[hk].homebrew;
    g->hb_port = (int)num_or(c, hb, "port", g->hb_dl5di ? 62030 : 62031);
    str_or(c, hb, "password", g->hb_password, sizeof g->hb_password, "");
    str_or(c, hb, "location", g->hb_location, sizeof g->hb_location, "");
    str_or(c, hb, "description", g->hb_description, sizeof g->hb_description, "");
    str_or(c, hb, "url", g->hb_url, sizeof g->hb_url, "");
    str_or(c, rcv >= 0 ? root : -1, "callsign", g->hb_callsign, sizeof g->hb_callsign, "SDR");
    if (!location(c, obj_of(c, root, "location"), g))
        return false;
    rest(c, dn, g);
    {
        char why[96];
        if (!rdof_slot_timing_parse(c->doc, -1, rdof_jget(c->doc, root, "slot_timing"), &g->slot_timing, why,
                                    sizeof why))
            return fail(c, "%s", why);
    }
    for (int f = 0; f < s->nfreq; f++)
        if (freq_cc(p, f) < 0 && s->freq[f].tx)
            return fail(c, "frequencies.%s: no color code (set cc on it or in defaults)", s->freq[f].name);
    /* its receiver's frequencies, then the ones it hears for the units on it */
    int hbn = rdof_plan_net_of(p, RDOF_NET_HBLINK);
    for (int f = 0; rcv >= 0 && f < s->nfreq; f++) {
        if (!(rdof_plan_unit(p, rcv)->freqs >> f & 1))
            continue;
        if (g->nch >= RDOF_SDR_MAX_CHANNELS)
            return fail(c, "%s: it hears more than %d frequencies", where, RDOF_SDR_MAX_CHANNELS);
        const rdof_freq_t *fr = &s->freq[f];
        int pa = rdof_site_path_rx(s, fr->hz, fr->ts);
        /* its own peer only with identity per_path; with single the board's peer carries it (rdof/router.h) */
        bool own = pa >= 0 && hbn >= 0 && s->net[hbn].per_path && s->path[pa].pol.net == hbn;
        rdof_sdr_cfg_channel_t *ch = &g->ch[g->nch];
        if (own && s->path[pa].peer_name[0])
            snprintf(ch->id, sizeof ch->id, "%s", s->path[pa].peer_name);
        else
            snprintf(ch->id, sizeof ch->id, "SDR%02d", f + 1);
        ch->freq = fr->hz;
        snprintf(ch->name, sizeof ch->name, "%s", fr->name);
        ch->cc = freq_cc(p, f);
        ch->slot = fr->ts;
        uint32_t tg = fr->app < 0 && pa >= 0 ? rdof_idset_single(&s->path[pa].pol.tg) : 0;
        ch->tg = tg ? (long)tg : -1;
        ch->hblink = own;
        ch->peer_id = own ? (long)s->path[pa].peer_id : -1;
        if (!add_channel(c, g))
            return false;
    }
    for (int k = 0; k < rdof_plan_nextra(p); k++) {
        const rdof_plan_extra_t *x = rdof_plan_extra(p, k);
        if (x->dev != d)
            continue;
        if (g->nch >= RDOF_SDR_MAX_CHANNELS)
            return fail(c, "%s: it hears more than %d frequencies", where, RDOF_SDR_MAX_CHANNELS);
        rdof_sdr_cfg_channel_t *ch = &g->ch[g->nch];
        snprintf(ch->id, sizeof ch->id, "%.*s", (int)sizeof ch->id - 1, x->id); /* cut to the field */
        ch->freq = x->hz;
        ch->cc = x->cc;
        ch->slot = 1;
        ch->tg = -1;
        ch->hblink = false;
        ch->peer_id = -1;
        if (!add_channel(c, g))
            return false;
    }
    if (!g->nch)
        return fail(c, "%s: no frequencies to hear", where);
    return true;
}

/* The text forms: a plan each. */
bool rdof_sdr_config_parse_device(const char *text, size_t len, const char *device, rdof_sdr_config_t *cfg,
                                  char *error, size_t cap)
{
    rdof_plan_t *p = rdof_plan_open(text, len, error, cap);
    if (!p) {
        memset(cfg, 0, sizeof *cfg);
        return false;
    }
    bool ok = rdof_plan_sdr(p, device, cfg, error, cap);
    rdof_plan_close(p);
    return ok;
}

bool rdof_sdr_config_parse(const char *text, size_t len, rdof_sdr_config_t *cfg, char *error, size_t cap)
{
    return rdof_sdr_config_parse_device(text, len, NULL, cfg, error, cap);
}

double rdof_sdr_config_rssi_offset(const rdof_sdr_config_t *g)
{
    if (g->has_rssi_offset)
        return g->rssi_offset;
    if (g->frontend == RDOF_FE_LR2021) /* the step's gain; under the chip's AGC the default step's, a guess */
        return RDOF_LR2021_RSSI_DBM -
               rdof_lr2021_gain_db[rdof_lr2021_gain_step(g->has_gain ? g->gain : RDOF_LR2021_GAIN_DEFAULT_DB) - 1];
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
