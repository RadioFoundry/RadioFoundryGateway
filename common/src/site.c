// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The settings, version 3: the parse, the talk paths and their policies, and the resolved view (rdof/site.h,
   docs/spec/fw/config-v3.md). */
#include "rdof/site.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/jsonp.h"
#include "site_ctx.h"

#define MAX_NODES RDOF_SITE_MAX_NODES
#define STR_BUF RDOF_SITE_STR_BUF

typedef rdof_site_ctx_t ctx_t;

static bool fail(ctx_t *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->error, c->cap, fmt, ap);
    va_end(ap);
    return false;
}

static const rdof_jnode_t *N(ctx_t *c, int i) { return rdof_jnode(&c->doc, i); }
static int get(ctx_t *c, int obj, const char *key) { return obj < 0 ? -1 : rdof_jget(&c->doc, obj, key); }
static int at(ctx_t *c, int node, int i) { return rdof_jat(&c->doc, node, i); }
static bool is_type(ctx_t *c, int i, rdof_jtype_t t) { return i >= 0 && N(c, i)->type == t; }
static const char *str_of(ctx_t *c, int i) { return is_type(c, i, RDOF_JSTR) ? N(c, i)->str : NULL; }
static int count(ctx_t *c, int i) { return i >= 0 ? N(c, i)->count : 0; }
static bool is_false(ctx_t *c, int i) { return is_type(c, i, RDOF_JBOOL) && !N(c, i)->b; }

static bool num_of(ctx_t *c, int i, double *v)
{
    if (!is_type(c, i, RDOF_JNUM))
        return false;
    *v = N(c, i)->num;
    return true;
}

/* A frequency: MHz below 1e5, else Hz. */
static uint32_t hz_of(double f)
{
    double v = rint(f < 1e5 ? f * 1e6 : f);
    return v <= 0 ? 0 : v >= 4294967295.0 ? UINT32_MAX : (uint32_t)v;
}

/* A frequency written as text: "439", "439.0", "438.9625", or Hz. 0: not one. */
static uint32_t hz_text(const char *s)
{
    char *e;
    double v = strtod(s, &e);
    if (e == s || *e || !(v > 0))
        return 0;
    return hz_of(v);
}

static ctx_t *open_doc(const char *text, size_t len, char *error, size_t cap)
{
    ctx_t *c = calloc(1, sizeof *c);
    char *clean = c ? malloc(len + 1) : NULL;
    if (!clean) {
        free(c);
        if (error && cap)
            snprintf(error, cap, "no memory");
        return NULL;
    }
    static char dummy[8];
    c->error = error && cap ? error : dummy;
    c->cap = error && cap ? cap : sizeof dummy;
    c->error[0] = 0;
    c->clean = clean;
    c->doc = (rdof_jdoc_t){.nodes = c->nodes, .max = MAX_NODES, .buf = c->strs, .cap = STR_BUF};
    size_t n = rdof_jsonc_strip(text, len, clean);
    clean[n] = 0;
    c->root = rdof_jparse(&c->doc, clean, n);
    return c;
}

static void close_doc(ctx_t *c)
{
    if (c) {
        free(c->clean);
        free(c);
    }
}

rdof_site_ctx_t *rdof_site_ctx_open(const char *text, size_t len, char *error, size_t cap)
{
    return open_doc(text, len, error, cap);
}

void rdof_site_ctx_close(rdof_site_ctx_t *c) { close_doc(c); }

/* ── the keys each place takes (as the schema) ─────────────────────────────── */

#define POLICY_KEYS "tg", "ptt", "ptt_hold_s", "pc", "apps", "net", "dir"
static const char *const TOP_KEYS[] = {"version", "name", "id", "callsign", "location", "defaults", "devices",
                                       "frequencies", "apps", "networks", "policies", "pc_hold_s", "wifi", "ntp",
                                       "oled", "slot_timing", "usb_hub", "selftest_load", "bench", NULL};
/* A frequency's own; the SFR / DCDM keys go with mode sfr or dcdm (inbound_slot with sfr; duplex and active_slots
   with dcdm). */
#define FREQ_RADIO_KEYS                                                                                                \
    "hang_s", "call_hang_s", "inbound_slot", "short_lc_ts1_low", "slot_timing", "duplex", "active_slots", "cwid",   \
        "cwid_every_s", "prebuffer", "jitter_buffer", "jitter_min_ms", "jitter_max_ms"
static const char *const FREQ_KEYS[] = {"freq", "name", "cc", "mode", "ts", "tx", "power_dbm", "devices", "slots",
                                        "policy", POLICY_KEYS, FREQ_RADIO_KEYS, NULL};
static const char *const RADIO_FREQ_KEYS[] = {FREQ_RADIO_KEYS, NULL};
static const char *const DEFAULT_KEYS[] = {"cc", "mode", "ts", "tx", "power_dbm", "policy", POLICY_KEYS, NULL};
static const char *const POL_KEYS[] = {"name", POLICY_KEYS, NULL};
static const char *const SLOT_KEYS[] = {"policy", POLICY_KEYS, NULL};
/* The talker keys: how a device that transmits on several frequencies waits and answers. A device's other keys
   are checked in siteplan.c. */
#define TALKER_KEYS                                                                                                \
    "listen_s", "clear_s", "idle_after_s", "csbk_preambles", "answer_s", "answer_max_s", "prebuffer",             \
        "jitter_buffer", "jitter_min_ms", "jitter_max_ms", "cwid", "cwid_every_s"
static const char *const DEV_TALKER_KEYS[] = {TALKER_KEYS, NULL};
static const char *const HEARS_KEYS[] = {"idle", "scan", "dwell_s", "hang_s", "carrier_s", NULL};
static const char *const REPEATER_KEYS[] = {"name", "type", "in", "out", "slots", "policy", POLICY_KEYS, "hang_s",
                                            "call_hang_s", "cwid", "cwid_every_s", "short_lc_ts1_low",
                                            "slot_timing", "split", "prebuffer", "jitter_buffer", "jitter_min_ms",
                                            "jitter_max_ms", "active_slots", NULL};
static const char *const PARROT_KEYS[] = {"name", "type", "tg", "pc", NULL};
static const char *const IPGW_KEYS[] = {"name", "type", "tg", "pc", "radio_net", "tx", "ack", "max_bytes",
                                        "up_pps", "down_pps", "idle_s", "max_flows", "routes", "nat", NULL};
static const char *const HBLINK_KEYS[] = {"name", "type", "host", "port", "transport", "sctp_encap_port",
                                          "sctp_heartbeat_ms", "sctp_max_retransmit", "sctp_ttl_ms", "identity",
                                          "peer_ids", "roaming", NULL};
static const char *const BM_KEYS[] = {"name", "type", "host", "port", "password", "peer_ids", "location",
                                      "description", "url", NULL};
static const char *const MQTT_KEYS[] = {"name", "type", "host", "port", "username", "password", "root", "tls",
                                        "report", "calls", "format", "bundle", "raw_tx", "transport", NULL};

static bool in_list(const char *const *keys, const char *k)
{
    for (; *keys; keys++)
        if (!strcmp(*keys, k))
            return true;
    return false;
}

static bool known(ctx_t *c, int obj, const char *path, const char *const *keys)
{
    if (!is_type(c, obj, RDOF_JOBJ))
        return true;
    for (int k = 0; k < count(c, obj); k++) {
        const char *key = N(c, at(c, obj, k))->key;
        if (!in_list(keys, key))
            return fail(c, "%s%s%s: not a key here", path, path[0] ? "." : "", key);
    }
    return true;
}

static bool name_ok(const char *s)
{
    if (!s[0] || strlen(s) >= RDOF_SITE_NAME)
        return false;
    for (; *s; s++)
        if ((unsigned char)*s < 0x20 || *s == '"' || *s == '\\' || *s == '/')
            return false;
    return true;
}

/* active_slots: [1], [2] or [1, 2], the slots a repeater or a DCDM gateway carries. */
static bool active_slots_ok(ctx_t *c, int node, const char *path)
{
    if (node < 0)
        return true;
    bool ok = is_type(c, node, RDOF_JARR) && count(c, node) >= 1 && count(c, node) <= 2;
    for (int k = 0; ok && k < count(c, node); k++) {
        double v;
        ok = num_of(c, at(c, node, k), &v) && (v == 1 || v == 2);
    }
    if (ok && count(c, node) == 2) {
        double a, b;
        num_of(c, at(c, node, 0), &a);
        num_of(c, at(c, node, 1), &b);
        ok = a != b;
    }
    return ok || fail(c, "%s.active_slots: [1], [2] or [1, 2]", path);
}

/* ── lookups ───────────────────────────────────────────────────────────────── */

int rdof_site_freq_hz(const rdof_site_t *s, uint32_t hz)
{
    for (int k = 0; k < s->nfreq; k++)
        if (s->freq[k].hz == hz)
            return k;
    return -1;
}

int rdof_site_freq(const rdof_site_t *s, const char *ref)
{
    for (int k = 0; k < s->nfreq; k++)
        if (!strcmp(s->freq[k].name, ref))
            return k;
    uint32_t hz = hz_text(ref);
    if (!hz)
        return -1;
    int found = -1;
    for (int k = 0; k < s->nfreq; k++)
        if (s->freq[k].hz == hz) {
            if (found >= 0)
                return -1; /* two at that frequency: by name only */
            found = k;
        }
    return found;
}

int rdof_site_path(const rdof_site_t *s, const char *name)
{
    for (int k = 0; k < s->npath; k++)
        if (!strcmp(s->path[k].name, name))
            return k;
    return -1;
}

int rdof_site_net(const rdof_site_t *s, const char *name)
{
    for (int k = 0; k < s->nnet; k++)
        if (!strcmp(s->net[k].name, name))
            return k;
    return -1;
}

int rdof_site_app(const rdof_site_t *s, const char *name)
{
    for (int k = 0; k < s->napp; k++)
        if (!strcmp(s->app[k].name, name))
            return k;
    return -1;
}

int rdof_site_app_of(const rdof_site_t *s, int type)
{
    for (int k = 0; k < s->napp; k++)
        if (s->app[k].type == type)
            return k;
    return -1;
}

int rdof_site_path_rx(const rdof_site_t *s, uint32_t hz, int slot)
{
    int any = -1;
    for (int k = 0; k < s->npath; k++) {
        const rdof_path_t *p = &s->path[k];
        if (s->freq[p->freq].hz != hz)
            continue;
        if (!p->slot)
            any = k;
        else if (p->slot == slot)
            return k;
    }
    return any;
}

/* A frequency reference in the document: a name, or a frequency (number or text). */
static int freq_ref(ctx_t *c, const rdof_site_t *s, int node, const char *path)
{
    int f = -1;
    double v;
    if (str_of(c, node))
        f = rdof_site_freq(s, str_of(c, node));
    else if (num_of(c, node, &v))
        f = rdof_site_freq_hz(s, hz_of(v));
    else
        return fail(c, "%s: a frequency's name, or its frequency", path), -2;
    if (f < 0) {
        char t[48];
        if (str_of(c, node))
            snprintf(t, sizeof t, "\"%.40s\"", str_of(c, node));
        else
            snprintf(t, sizeof t, "%.10g", v);
        return fail(c, "%s: %s isn't in frequencies", path, t), -2;
    }
    return f;
}

/* ── ID sets ───────────────────────────────────────────────────────────────── */

static bool idset_of(ctx_t *c, int node, const char *path, rdof_idset_t *out)
{
    char why[96];
    rdof_idset_clear(out);
    if (is_false(c, node))
        return true;
    double v;
    if (num_of(c, node, &v)) {
        if (v != floor(v) || !rdof_idset_add_id(out, v > 0 && v <= RDOF_ID_MAX ? (uint32_t)v : 0, why, sizeof why))
            return fail(c, "%s: an ID from 1 to 16777215", path);
    } else if (str_of(c, node)) {
        if (!rdof_idset_add(out, str_of(c, node), why, sizeof why))
            return fail(c, "%s: %s", path, why);
    } else if (is_type(c, node, RDOF_JARR)) {
        for (int k = 0; k < count(c, node); k++) {
            int it = at(c, node, k);
            if (num_of(c, it, &v)) {
                if (v != floor(v) || !rdof_idset_add_id(out, v > 0 && v <= RDOF_ID_MAX ? (uint32_t)v : 0, why,
                                                        sizeof why))
                    return fail(c, "%s[%d]: an ID from 1 to 16777215", path, k);
            } else if (!str_of(c, it) || !rdof_idset_add(out, str_of(c, it), why, sizeof why)) {
                return fail(c, "%s[%d]: %s", path, k, str_of(c, it) ? why : "an ID, a range \"A-B\", \"*\" or \"!...\"");
            }
        }
    } else {
        return fail(c, "%s: an ID set: 202, \"3100-3199\", \"*\", \"!350\", a list of those, or false", path);
    }
    rdof_idset_finish(out);
    return true;
}

static bool overlap(const rdof_idset_t *a, const rdof_idset_t *b, uint32_t *at_id)
{
    rdof_idrange_t ra[64], rb[64];
    int na = rdof_idset_ranges(a, ra, 64), nb = rdof_idset_ranges(b, rb, 64);
    for (int i = 0; i < na && i < 64; i++)
        for (int j = 0; j < nb && j < 64; j++)
            if (ra[i].lo <= rb[j].hi && rb[j].lo <= ra[i].hi) {
                *at_id = ra[i].lo > rb[j].lo ? ra[i].lo : rb[j].lo;
                return true;
            }
    return false;
}

/* ── policies ──────────────────────────────────────────────────────────────── */

static const char *const PK_NAMES[RDOF_PK_N] = {"tg", "ptt", "ptt_hold_s", "pc", "apps", "net", "dir"};

/* The answering apps (parrot, IP gateway): those "*" reaches. */
static uint32_t answering(const rdof_site_t *s)
{
    uint32_t m = 0;
    for (int k = 0; k < s->napp; k++)
        if (s->app[k].type != RDOF_APP_REPEATER)
            m |= 1u << k;
    return m;
}

/* One key from obj into pol, when obj has it and pol doesn't yet. */
static bool pol_key(ctx_t *c, const rdof_site_t *s, int obj, int key, const char *path, rdof_policy_t *pol,
                    bool *have, int src)
{
    int i = get(c, obj, PK_NAMES[key]);
    if (i < 0 || have[key])
        return true;
    char p[96];
    snprintf(p, sizeof p, "%s.%s", path, PK_NAMES[key]);
    double v;
    switch (key) {
    case RDOF_PK_TG:
        if (!idset_of(c, i, p, &pol->tg))
            return false;
        break;
    case RDOF_PK_PTT:
        if (!idset_of(c, i, p, &pol->ptt))
            return false;
        break;
    case RDOF_PK_PC:
        if (!idset_of(c, i, p, &pol->pc))
            return false;
        break;
    case RDOF_PK_PTT_HOLD:
        if (!num_of(c, i, &v) || v < 1 || v > 86400)
            return fail(c, "%s: seconds, 1 to 86400", p);
        pol->ptt_hold_s = v;
        break;
    case RDOF_PK_APPS:
        if (str_of(c, i) && !strcmp(str_of(c, i), "*")) {
            pol->apps = answering(s);
        } else if (is_false(c, i)) {
            pol->apps = 0;
        } else if (is_type(c, i, RDOF_JARR)) {
            pol->apps = 0;
            for (int k = 0; k < count(c, i); k++) {
                const char *n = str_of(c, at(c, i, k));
                int a = n ? rdof_site_app(s, n) : -1;
                if (a < 0)
                    return fail(c, "%s[%d]: %s isn't in apps", p, k, n ? n : "(not a name)");
                if (s->app[a].type == RDOF_APP_REPEATER)
                    return fail(c, "%s[%d]: %s is a repeater: a path reaches apps that answer calls (parrot, ipgw)",
                                p, k, n);
                pol->apps |= 1u << a;
            }
        } else {
            return fail(c, "%s: \"*\", a list of app names, or false", p);
        }
        break;
    case RDOF_PK_NET:
        if (is_false(c, i)) {
            pol->net = -1;
        } else {
            const char *n = str_of(c, i);
            pol->net = n ? rdof_site_net(s, n) : -1;
            if (pol->net < 0)
                return fail(c, "%s: %s isn't in networks", p, n ? n : "(not a name)");
        }
        break;
    case RDOF_PK_DIR: {
        const char *d = str_of(c, i);
        if (d && !strcmp(d, "both"))
            pol->dir = RDOF_FLOW_BOTH;
        else if (d && !strcmp(d, "rf>net"))
            pol->dir = RDOF_FLOW_RF_NET;
        else if (d && !strcmp(d, "net>rf"))
            pol->dir = RDOF_FLOW_NET_RF;
        else
            return fail(c, "%s: both, rf>net or net>rf", p);
        break;
    }
    }
    have[key] = true;
    pol->src[key] = (int8_t)src;
    return true;
}

static bool pol_layer(ctx_t *c, const rdof_site_t *s, int obj, const char *path, rdof_policy_t *pol, bool *have,
                      int src)
{
    for (int k = 0; k < RDOF_PK_N; k++)
        if (!pol_key(c, s, obj, k, path, pol, have, src))
            return false;
    return true;
}

static int policy_index(const rdof_site_t *s, const char *name)
{
    for (int k = 0; k < s->npol; k++)
        if (!strcmp(s->pol_name[k], name))
            return k;
    return -1;
}

/* A layer that may name a policy: its own keys, then the policy's. */
static bool pol_named(ctx_t *c, const rdof_site_t *s, int obj, const char *path, rdof_policy_t *pol, bool *have,
                      int own_src)
{
    if (!pol_layer(c, s, obj, path, pol, have, own_src))
        return false;
    int pn = get(c, obj, "policy");
    if (pn < 0)
        return true;
    const char *n = str_of(c, pn);
    int k = n ? policy_index(s, n) : -1;
    if (k < 0)
        return fail(c, "%s.policy: %s isn't in policies", path, n ? n : "(not a name)");
    char p[96];
    snprintf(p, sizeof p, "policies.%s", s->pol_name[k]);
    return pol_layer(c, s, c->pnode[k], p, pol, have, k);
}

/* What's left from the table. */
static void pol_table(const rdof_site_t *s, rdof_policy_t *pol, bool *have, bool rx_only)
{
    if (!have[RDOF_PK_PTT_HOLD])
        pol->ptt_hold_s = 600;
    if (!have[RDOF_PK_PC]) {
        rdof_idset_clear(&pol->pc);
        pol->pc.star = true;
    }
    if (!have[RDOF_PK_APPS])
        pol->apps = answering(s);
    if (!have[RDOF_PK_NET])
        pol->net = s->nnet ? 0 : -1;
    if (!have[RDOF_PK_DIR])
        pol->dir = rx_only ? RDOF_FLOW_RF_NET : RDOF_FLOW_BOTH;
    for (int k = 0; k < RDOF_PK_N; k++)
        if (!have[k])
            pol->src[k] = RDOF_SRC_TABLE;
}

/* ── the parse ─────────────────────────────────────────────────────────────── */

static void format_mhz(uint32_t hz, char *out, size_t cap)
{
    snprintf(out, cap, "%.10g", hz / 1e6);
}

static bool parse_freqs(ctx_t *c, rdof_site_t *s)
{
    int fl = get(c, c->root, "frequencies");
    if (fl >= 0 && !is_type(c, fl, RDOF_JARR))
        return fail(c, "frequencies: a list");
    int d = c->defaults;
    for (int k = 0; k < count(c, fl); k++) {
        int e = at(c, fl, k);
        char path[48];
        snprintf(path, sizeof path, "frequencies[%d]", k);
        if (s->nfreq >= RDOF_SITE_MAX_FREQS)
            return fail(c, "frequencies: %d at most", RDOF_SITE_MAX_FREQS);
        rdof_freq_t *f = &s->freq[s->nfreq];
        c->fnode[s->nfreq] = is_type(c, e, RDOF_JOBJ) ? e : -1;
        double v;
        int fv = is_type(c, e, RDOF_JOBJ) ? get(c, e, "freq") : e;
        if (!num_of(c, fv, &v) || !(f->hz = hz_of(v)) || f->hz < 1000000)
            return fail(c, "%s%s: a frequency (MHz, or Hz)", path, is_type(c, e, RDOF_JOBJ) ? ".freq" : "");
        int o = c->fnode[s->nfreq];
        if (get(c, o, "power") >= 0)
            return fail(c, "%s.power: power_dbm, in dBm at the antenna", path);
        if (!known(c, o, path, FREQ_KEYS))
            return false;
        const char *nm = str_of(c, get(c, o, "name"));
        if (get(c, o, "name") >= 0 && (!nm || !name_ok(nm)))
            return fail(c, "%s.name: a name under %d characters, without '/', '\"' or '\\'", path, RDOF_SITE_NAME);
        if (nm)
            snprintf(f->name, sizeof f->name, "%s", nm);
        else
            format_mhz(f->hz, f->name, sizeof f->name);
        /* each key: its own, else defaults */
#define PICK(key) (get(c, o, key) >= 0 ? get(c, o, key) : get(c, d, key))
        int i = PICK("tx");
        if (i >= 0 && !is_type(c, i, RDOF_JBOOL))
            return fail(c, "%s.tx: true or false", path);
        f->tx = i < 0 || N(c, i)->b;
        i = PICK("cc");
        f->cc = -1;
        if (i >= 0) {
            if (!num_of(c, i, &v) || v != floor(v) || v < 0 || v > 15)
                return fail(c, "%s.cc: 0 to 15", path);
            f->cc = (int)v;
        } else if (f->tx) {
            return fail(c, "%s.cc: the color code (or defaults.cc); only a frequency with \"tx\": false may leave it "
                           "out", path);
        }
        i = PICK("mode");
        const char *m = str_of(c, i);
        if (i >= 0 && (!m || (strcmp(m, "simplex") && strcmp(m, "sfr") && strcmp(m, "dcdm"))))
            return fail(c, "%s.mode: simplex, sfr or dcdm", path);
        f->mode = !m || !strcmp(m, "simplex") ? RDOF_FMODE_SIMPLEX : !strcmp(m, "sfr") ? RDOF_FMODE_SFR : RDOF_FMODE_DCDM;
        if (f->mode != RDOF_FMODE_SIMPLEX && !f->tx)
            return fail(c, "%s.mode: %s is a radio transmitting here: not with \"tx\": false", path, m);
        for (int x = 0; f->mode == RDOF_FMODE_SIMPLEX && RADIO_FREQ_KEYS[x]; x++)
            if (get(c, o, RADIO_FREQ_KEYS[x]) >= 0)
                return fail(c, "%s.%s: for mode sfr or dcdm", path, RADIO_FREQ_KEYS[x]);
        if (f->mode == RDOF_FMODE_SFR && get(c, o, "duplex") >= 0)
            return fail(c, "%s.duplex: for mode dcdm (an SFR repeater is half duplex)", path);
        if (f->mode == RDOF_FMODE_SFR && get(c, o, "active_slots") >= 0)
            return fail(c, "%s.active_slots: for mode dcdm (an SFR repeater's slots are inbound_slot's)", path);
        if (f->mode == RDOF_FMODE_DCDM && get(c, o, "inbound_slot") >= 0)
            return fail(c, "%s.inbound_slot: for mode sfr", path);
        const char *dx = str_of(c, get(c, o, "duplex"));
        if (get(c, o, "duplex") >= 0 && (!dx || (strcmp(dx, "half") && strcmp(dx, "full"))))
            return fail(c, "%s.duplex: half or full", path);
        if (!active_slots_ok(c, get(c, o, "active_slots"), path))
            return false;
        i = PICK("ts");
        f->ts = 1;
        if (i >= 0) {
            if (!num_of(c, i, &v) || (v != 1 && v != 2))
                return fail(c, "%s.ts: 1 or 2", path);
            f->ts = (uint8_t)v;
        }
        i = PICK("power_dbm");
        f->power_dbm = NAN;
        if (i >= 0) {
            if (!num_of(c, i, &v) || v < -60 || v > 60)
                return fail(c, "%s.power_dbm: -60 to 60 (dBm at the antenna)", path);
            f->power_dbm = v;
        }
#undef PICK
        f->app = -1;
        s->nfreq++;
    }
    /* names: unique, and none another entry's frequency */
    for (int a = 0; a < s->nfreq; a++)
        for (int b = 0; b < s->nfreq; b++) {
            if (a < b && !strcmp(s->freq[a].name, s->freq[b].name))
                return fail(c, "frequencies[%d].name: %s is frequencies[%d]'s name too%s", b, s->freq[b].name, a,
                            s->freq[a].hz == s->freq[b].hz ? " (the same frequency twice needs a name on each)" : "");
            uint32_t h = hz_text(s->freq[a].name);
            if (a != b && h && h == s->freq[b].hz && h != s->freq[a].hz)
                return fail(c, "frequencies[%d].name: %s is frequencies[%d]'s frequency", a, s->freq[a].name, b);
        }
    return true;
}

static bool parse_nets(ctx_t *c, rdof_site_t *s)
{
    int nl = get(c, c->root, "networks");
    if (nl >= 0 && !is_type(c, nl, RDOF_JARR))
        return fail(c, "networks: a list");
    int nh = 0, nm = 0;
    for (int k = 0; k < count(c, nl); k++) {
        int e = at(c, nl, k);
        char path[48];
        snprintf(path, sizeof path, "networks[%d]", k);
        if (s->nnet >= RDOF_SITE_MAX_NETS)
            return fail(c, "networks: %d at most", RDOF_SITE_MAX_NETS);
        if (!is_type(c, e, RDOF_JOBJ))
            return fail(c, "%s: an object", path);
        rdof_net3_t *n = &s->net[s->nnet];
        c->nnode[s->nnet] = e;
        const char *t = str_of(c, get(c, e, "type"));
        if (t && !strcmp(t, "hblink"))
            n->type = RDOF_NET_HBLINK, nh++;
        else if (t && !strcmp(t, "brandmeister")) /* an HBlink peer with a login (brandmeister.md) */
            n->type = RDOF_NET_HBLINK, n->login = true, nh++;
        else if (t && !strcmp(t, "mqtt"))
            n->type = RDOF_NET_MQTT, nm++;
        else
            return fail(c, "%s.type: hblink, brandmeister or mqtt", path);
        if (!known(c, e, path, n->login ? BM_KEYS : n->type == RDOF_NET_HBLINK ? HBLINK_KEYS : MQTT_KEYS))
            return false;
        const char *nmv = str_of(c, get(c, e, "name"));
        if (get(c, e, "name") >= 0 && (!nmv || !name_ok(nmv)))
            return fail(c, "%s.name: a name under %d characters", path, RDOF_SITE_NAME);
        snprintf(n->name, sizeof n->name, "%s", nmv ? nmv : t);
        if (rdof_site_net(s, n->name) >= 0)
            return fail(c, "%s.name: two networks called %s (two of one type need names)", path, n->name);
        if (!str_of(c, get(c, e, "host")) || !str_of(c, get(c, e, "host"))[0])
            return fail(c, "%s.host: the %s's host name or address", path,
                        n->login ? "BrandMeister master" : n->type == RDOF_NET_HBLINK ? "HBlink4 master" : "MQTT broker");
        if (n->login) {
            const char *pw = str_of(c, get(c, e, "password"));
            if (!pw || !pw[0] || strlen(pw) > 63)
                return fail(c, "%s.password: the hotspot security password (BrandMeister SelfCare), 1 to 63 characters",
                            path);
            static const char *const TEXT[] = {"location", "description", "url"};
            static const int MAXLEN[] = {20, 19, 124};
            for (int x = 0; x < 3; x++) {
                int ti = get(c, e, TEXT[x]);
                if (ti >= 0 && (!str_of(c, ti) || strlen(str_of(c, ti)) > (size_t)MAXLEN[x]))
                    return fail(c, "%s.%s: text, %d characters at most", path, TEXT[x], MAXLEN[x]);
            }
        }
        int i;
        if (n->login) { /* one login per frequency or repeater, always: peer_ids */
            n->per_path = true;
            i = get(c, e, "peer_ids");
            if (i < 0 || !is_type(c, i, RDOF_JOBJ))
                return fail(c, "%s.peer_ids: {\"<frequency or repeater>\": id, ...}, a registered ID for each", path);
        } else if (n->type == RDOF_NET_HBLINK) {
            i = get(c, e, "identity");
            const char *id = str_of(c, i);
            if (i >= 0 && (!id || (strcmp(id, "single") && strcmp(id, "per_path"))))
                return fail(c, "%s.identity: single or per_path", path);
            n->per_path = id && !strcmp(id, "per_path");
            i = get(c, e, "peer_ids");
            if (i >= 0 && !n->per_path)
                return fail(c, "%s.peer_ids: with identity per_path", path);
            if (i >= 0 && !is_type(c, i, RDOF_JOBJ))
                return fail(c, "%s.peer_ids: {\"<path>\": id, ...}", path);
            i = get(c, e, "roaming");
            if (i >= 0 && !is_type(c, i, RDOF_JBOOL))
                return fail(c, "%s.roaming: true or false", path);
            n->roaming = i >= 0 && N(c, i)->b;
        } else {
            i = get(c, e, "report");
            if (i >= 0 && !is_type(c, i, RDOF_JBOOL))
                return fail(c, "%s.report: true or false", path);
            n->report = i < 0 || N(c, i)->b;
            i = get(c, e, "calls");
            if (i >= 0 && !is_type(c, i, RDOF_JBOOL))
                return fail(c, "%s.calls: true or false", path);
            n->calls = i >= 0 && N(c, i)->b;
            /* the MQTT network's keys (docs/spec/fw/mqtt-net.md section 10) */
            i = get(c, e, "format");
            const char *fmt = str_of(c, i);
            if (i >= 0 && (!fmt || (strcmp(fmt, "dmrd") && strcmp(fmt, "v3"))))
                return fail(c, "%s.format: dmrd or v3", path);
            if (i >= 0 && !n->calls)
                return fail(c, "%s.format: with calls true", path);
            n->format = fmt && !strcmp(fmt, "v3") ? RDOF_MQTT_FORMAT_V3 : RDOF_MQTT_FORMAT_DMRD;
            double v = 1;
            i = get(c, e, "bundle");
            if (i >= 0 && (!num_of(c, i, &v) || !N(c, i)->integer || v < 1 || v > 3))
                return fail(c, "%s.bundle: 1 to 3", path);
            if (i >= 0 && n->format != RDOF_MQTT_FORMAT_V3)
                return fail(c, "%s.bundle: with format v3", path);
            n->bundle = (uint8_t)v;
            i = get(c, e, "raw_tx");
            if (i >= 0 && !is_type(c, i, RDOF_JBOOL))
                return fail(c, "%s.raw_tx: true or false", path);
            if (i >= 0 && n->format != RDOF_MQTT_FORMAT_V3)
                return fail(c, "%s.raw_tx: with format v3", path);
            n->raw_tx = i >= 0 && N(c, i)->b;
            i = get(c, e, "tls"); /* the radio's settings take any truthy value (rdof_plan_radio) */
            const bool tls = is_type(c, i, RDOF_JBOOL) && N(c, i)->b;
            i = get(c, e, "transport");
            const char *tr = str_of(c, i);
            if (i >= 0 && (!tr || (strcmp(tr, "mqtt/tcp") && strcmp(tr, "mqtt/tls"))))
                return fail(c, "%s.transport: mqtt/tcp or mqtt/tls (the others aren't built yet)", path);
            if (tr && !strcmp(tr, "mqtt/tcp") && tls)
                return fail(c, "%s.transport: mqtt/tcp with tls true (mqtt/tls, or leave one out)", path);
            n->transport = (tr && !strcmp(tr, "mqtt/tls")) || tls ? RDOF_MQTT_TLS : RDOF_MQTT_TCP;
        }
        s->nnet++;
    }
    if (nh > 1 || nm > 1)
        return fail(c, "networks: one hblink (or brandmeister) and one mqtt network for now");
    return true;
}

static bool parse_apps(ctx_t *c, rdof_site_t *s)
{
    int al = get(c, c->root, "apps");
    if (al >= 0 && !is_type(c, al, RDOF_JARR))
        return fail(c, "apps: a list");
    for (int k = 0; k < count(c, al); k++) {
        int e = at(c, al, k);
        char path[48];
        snprintf(path, sizeof path, "apps[%d]", k);
        if (s->napp >= RDOF_SITE_MAX_APPS)
            return fail(c, "apps: %d at most", RDOF_SITE_MAX_APPS);
        if (!is_type(c, e, RDOF_JOBJ))
            return fail(c, "%s: an object", path);
        rdof_app3_t *a = &s->app[s->napp];
        a->in = a->out = a->path[0] = a->path[1] = -1;
        c->anode[s->napp] = e;
        const char *t = str_of(c, get(c, e, "type"));
        static const char *const types[] = {"", "repeater", "parrot", "ipgw"};
        for (int x = 1; t && x <= RDOF_APP_IPGW; x++)
            if (!strcmp(t, types[x]))
                a->type = (uint8_t)x;
        if (!a->type)
            return fail(c, "%s.type: repeater, parrot or ipgw", path);
        const char *nmv = str_of(c, get(c, e, "name"));
        if (get(c, e, "name") >= 0 && (!nmv || !name_ok(nmv)))
            return fail(c, "%s.name: a name under %d characters, without '/'", path, RDOF_SITE_NAME);
        snprintf(a->name, sizeof a->name, "%s", nmv ? nmv : t);
        if (rdof_site_app(s, a->name) >= 0)
            return fail(c, "%s.name: two apps called %s (two of one type need names)", path, a->name);
        snprintf(path, sizeof path, "apps.%s", a->name);
        if (!known(c, e, path, a->type == RDOF_APP_REPEATER ? REPEATER_KEYS
                                : a->type == RDOF_APP_PARROT ? PARROT_KEYS : IPGW_KEYS))
            return false;
        char p[80];
        if (a->type == RDOF_APP_REPEATER) {
            for (int w = 0; w < 2; w++) {
                const char *key = w ? "out" : "in";
                snprintf(p, sizeof p, "%s.%s", path, key);
                int i = get(c, e, key);
                if (i < 0)
                    return fail(c, "%s: the frequency it %s on", p, w ? "transmits" : "hears");
                int f = freq_ref(c, s, i, p);
                if (f < 0)
                    return false;
                if (s->freq[f].app >= 0)
                    return fail(c, "%s: %s is %s's already", p, s->freq[f].name, s->app[s->freq[f].app].name);
                if (w && !s->freq[f].tx)
                    return fail(c, "%s: %s has \"tx\": false", p, s->freq[f].name);
                if (s->freq[f].mode != RDOF_FMODE_SIMPLEX)
                    return fail(c, "%s: %s has mode %s: a repeater's frequencies are its own", p, s->freq[f].name,
                                s->freq[f].mode == RDOF_FMODE_SFR ? "sfr" : "dcdm");
                if (w && s->freq[f].cc < 0)
                    return fail(c, "%s: %s needs a color code", p, s->freq[f].name);
                s->freq[f].app = s->napp;
                *(w ? &a->out : &a->in) = f;
            }
            if (a->in == a->out)
                return fail(c, "%s: in and out are one frequency (an SFR repeater is a frequency with mode sfr)",
                            path);
            if (!active_slots_ok(c, get(c, e, "active_slots"), path))
                return false;
        } else {
            int tg = get(c, e, "tg"), pc = get(c, e, "pc");
            if (tg < 0 && pc < 0)
                return fail(c, "%s: tg or pc, the IDs it answers", path);
            snprintf(p, sizeof p, "%s.tg", path);
            if (tg >= 0 && !idset_of(c, tg, p, &a->tg))
                return false;
            snprintf(p, sizeof p, "%s.pc", path);
            if (pc >= 0 && !idset_of(c, pc, p, &a->pc))
                return false;
            if (a->type == RDOF_APP_PARROT || a->type == RDOF_APP_IPGW) {
                uint32_t one = rdof_idset_single(tg >= 0 ? &a->tg : &a->pc);
                if (!one || (tg >= 0 && pc >= 0))
                    return fail(c, "%s: one ID, as tg or pc (%s answers one ID for now)", path, t);
                if (a->type == RDOF_APP_PARROT && rdof_site_app_of(s, RDOF_APP_PARROT) >= 0)
                    return fail(c, "%s: one parrot for now", path);
                if (a->type == RDOF_APP_IPGW && rdof_site_app_of(s, RDOF_APP_IPGW) >= 0)
                    return fail(c, "%s: one IP gateway for now", path);
            }
            for (int b = 0; b < s->napp; b++) {
                uint32_t id;
                if (s->app[b].type == RDOF_APP_REPEATER)
                    continue;
                if (overlap(&a->tg, &s->app[b].tg, &id) || overlap(&a->pc, &s->app[b].pc, &id))
                    return fail(c, "%s: %s claims %lu too: two apps may not answer one ID", path, s->app[b].name,
                                (unsigned long)id);
            }
        }
        s->napp++;
    }
    return true;
}

static bool parse_policies(ctx_t *c, rdof_site_t *s)
{
    int pl = get(c, c->root, "policies");
    if (pl >= 0 && !is_type(c, pl, RDOF_JARR))
        return fail(c, "policies: a list");
    for (int k = 0; k < count(c, pl); k++) {
        int e = at(c, pl, k);
        char path[48];
        snprintf(path, sizeof path, "policies[%d]", k);
        if (s->npol >= RDOF_SITE_MAX_POLICIES)
            return fail(c, "policies: %d at most", RDOF_SITE_MAX_POLICIES);
        if (!is_type(c, e, RDOF_JOBJ))
            return fail(c, "%s: an object", path);
        const char *n = str_of(c, get(c, e, "name"));
        if (!n || !name_ok(n))
            return fail(c, "%s.name: a policy needs a name", path);
        if (policy_index(s, n) >= 0)
            return fail(c, "%s.name: two policies called %s", path, n);
        if (!known(c, e, path, POL_KEYS))
            return false;
        /* check its keys now, so a mistake says where it is */
        rdof_policy_t pol;
        bool have[RDOF_PK_N] = {0};
        snprintf(path, sizeof path, "policies.%s", n);
        if (!pol_layer(c, s, e, path, &pol, have, 0))
            return false;
        snprintf(s->pol_name[s->npol], sizeof s->pol_name[0], "%s", n);
        c->pnode[s->npol++] = e;
    }
    return true;
}

/* An SDR driver that transmits. */
static bool sdr_transmits(const char *driver)
{
    return !strcmp(driver, "lr2021") || !strcmp(driver, "sx1255") || !strcmp(driver, "iio") ||
           !strcmp(driver, "soapy") || !strcmp(driver, "sim");
}

static bool freq_list(ctx_t *c, const rdof_site_t *s, int node, const char *path, uint64_t *out)
{
    *out = 0;
    if (!is_type(c, node, RDOF_JARR))
        return fail(c, "%s: a list of frequencies (names, or frequencies)", path);
    for (int k = 0; k < count(c, node); k++) {
        char p[96];
        snprintf(p, sizeof p, "%s[%d]", path, k);
        int f = freq_ref(c, s, at(c, node, k), p);
        if (f < 0)
            return false;
        *out |= 1ull << f;
    }
    return true;
}

/* A device bench.devices lists. */
static bool on_bench(ctx_t *c, const char *name)
{
    int bd = get(c, get(c, c->root, "bench"), "devices");
    for (int k = 0; is_type(c, bd, RDOF_JARR) && k < count(c, bd); k++)
        if (str_of(c, at(c, bd, k)) && !strcmp(str_of(c, at(c, bd, k)), name))
            return true;
    return false;
}

static bool parse_devices(ctx_t *c, rdof_site_t *s)
{
    uint32_t bench = 0; /* bench.devices with neither hears nor transmits: the bench's alone, unless pinned */
    uint32_t dflt = 0;  /* modems with no "hears": on the first frequency they transmit on */
    int dl = get(c, c->root, "devices");
    if (dl >= 0 && !is_type(c, dl, RDOF_JARR))
        return fail(c, "devices: a list");
    int nkind[4] = {0};
    for (int k = 0; k < count(c, dl); k++) {
        int e = at(c, dl, k);
        char path[48];
        snprintf(path, sizeof path, "devices[%d]", k);
        if (s->ndev >= RDOF_SITE_MAX_DEVICES)
            return fail(c, "devices: %d at most", RDOF_SITE_MAX_DEVICES);
        if (!is_type(c, e, RDOF_JOBJ))
            return fail(c, "%s: an object", path);
        rdof_device3_t *d = &s->dev[s->ndev];
        c->dnode[s->ndev] = e;
        const char *kind = str_of(c, get(c, e, "kind"));
        d->kind = kind && !strcmp(kind, "sdr") ? 1 : kind && !strcmp(kind, "modem") ? 2 : kind && !strcmp(kind, "radio") ? 3 : 0;
        if (!d->kind)
            return fail(c, "%s.kind: sdr, modem or radio", path);
        const char *nmv = str_of(c, get(c, e, "name"));
        if (get(c, e, "name") >= 0 && (!nmv || !name_ok(nmv)))
            return fail(c, "%s.name: a name under %d characters", path, RDOF_SITE_NAME);
        if (nmv) {
            for (const char *q = nmv; *q; q++)
                if (!isalnum((unsigned char)*q) && *q != '-' && *q != '_' && *q != '.')
                    return fail(c, "%s.name: letters, digits, '-', '_' and '.'", path);
            snprintf(d->name, sizeof d->name, "%s", nmv);
        } else {
            snprintf(d->name, sizeof d->name, "%s%d", kind, nkind[d->kind]);
        }
        nkind[d->kind]++;
        for (int b = 0; b < s->ndev; b++)
            if (!strcmp(s->dev[b].name, d->name))
                return fail(c, "%s.name: two devices called %s", path, d->name);
        snprintf(path, sizeof path, "devices.%s", d->name);
        /* an SDR's driver; a modem's is its "type" */
        const char *drv = str_of(c, get(c, e, d->kind == 1 ? "driver" : "type"));
        snprintf(d->driver, sizeof d->driver, "%s", drv ? drv : d->kind == 1 ? "rtlsdr" : "auto");
        int h = get(c, e, "hears"), t = get(c, e, "transmits");
        char p[80];
        d->idle = -1;
        if (d->kind == 2 && h < 0 && t < 0 && on_bench(c, d->name)) {
            bench |= 1u << s->ndev;
            d->hears = RDOF_HEARS_NONE;
            d->can_tx = true; /* on what pins it, below */
            s->ndev++;
            continue;
        }
        if (d->kind == 3) {
            if (h >= 0 || t >= 0)
                return fail(c, "%s: a radio is a bench device on its debug link: no hears or transmits", path);
            d->hears = RDOF_HEARS_NONE;
        } else if (d->kind == 1) {
            if (h >= 0 && !is_false(c, h))
                return fail(c, "%s.hears: an SDR hears every frequency in its window (false: it never receives)",
                            path);
            d->hears = h >= 0 ? RDOF_HEARS_NONE : RDOF_HEARS_WINDOW;
        } else {
            snprintf(p, sizeof p, "%s.hears", path);
            if (is_false(c, h)) {
                d->hears = RDOF_HEARS_NONE;
            } else if (h >= 0) {
                if (!is_type(c, h, RDOF_JOBJ))
                    return fail(c, "%s: {\"idle\": frequency}, {\"scan\": [frequencies]} or false", p);
                if (!known(c, h, p, HEARS_KEYS))
                    return false;
                int idle = get(c, h, "idle"), scan = get(c, h, "scan");
                if (idle < 0 && scan < 0)
                    return fail(c, "%s: idle or scan", p);
                if (scan >= 0) {
                    char q[96];
                    snprintf(q, sizeof q, "%s.scan", p);
                    if (!freq_list(c, s, scan, q, &d->scan))
                        return false;
                    d->hears = RDOF_HEARS_SCAN;
                } else {
                    d->hears = RDOF_HEARS_IDLE;
                    for (int x = 0; x < 3; x++)
                        if (get(c, h, HEARS_KEYS[2 + x]) >= 0)
                            return fail(c, "%s.%s: with scan", p, HEARS_KEYS[2 + x]);
                }
                if (idle >= 0) {
                    char q[96];
                    snprintf(q, sizeof q, "%s.idle", p);
                    if ((d->idle = freq_ref(c, s, idle, q)) < 0)
                        return false;
                }
            } else {
                d->hears = RDOF_HEARS_IDLE; /* on what it transmits on first, below */
                dflt |= 1u << s->ndev;
            }
        }
        snprintf(p, sizeof p, "%s.transmits", path);
        if (d->kind == 3 || is_false(c, t)) {
            d->can_tx = false;
        } else {
            d->can_tx = d->kind == 2 || get(c, e, "tx") >= 0 || sdr_transmits(d->driver);
            if (t >= 0 && !d->can_tx)
                return fail(c, "%s: devices.%s is an %s SDR: it doesn't transmit", p, d->name, d->driver);
            if (t >= 0) {
                if (!freq_list(c, s, t, p, &d->transmits))
                    return false;
                for (int f = 0; f < s->nfreq; f++)
                    if ((d->transmits >> f & 1) && !s->freq[f].tx)
                        return fail(c, "%s: %s has \"tx\": false", p, s->freq[f].name);
            } else if (d->can_tx) {
                for (int f = 0; f < s->nfreq; f++)
                    if (s->freq[f].tx && !(s->freq[f].app >= 0 && s->app[s->freq[f].app].in == f))
                        d->transmits |= 1ull << f;
            }
        }
        if (d->kind != 1 && d->kind != 3 && !(d->can_tx && d->transmits) && d->hears != RDOF_HEARS_NONE &&
            !is_type(c, h, RDOF_JOBJ))
            return fail(c, "%s: a modem that doesn't transmit needs hears (its frequency)", path);
        for (int x = 0; DEV_TALKER_KEYS[x]; x++)
            if (get(c, e, DEV_TALKER_KEYS[x]) >= 0 && !(d->can_tx && d->transmits))
                return fail(c, "%s.%s: for a device that transmits", path, DEV_TALKER_KEYS[x]);
        if (d->hears == RDOF_HEARS_IDLE && d->idle < 0) {
            for (int f = 0; f < s->nfreq && d->idle < 0; f++)
                if (d->transmits >> f & 1)
                    d->idle = f;
        }
        if (d->hears == RDOF_HEARS_SCAN && d->idle < 0)
            for (int f = 0; f < s->nfreq && d->idle < 0; f++)
                if (d->scan >> f & 1)
                    d->idle = f;
        s->ndev++;
    }
    /* each frequency's devices */
    for (int f = 0; f < s->nfreq; f++) {
        int o = c->fnode[f];
        int i = get(c, o, "devices");
        if (i < 0)
            continue;
        char p[80];
        snprintf(p, sizeof p, "frequencies.%s.devices", s->freq[f].name);
        if (!is_type(c, i, RDOF_JARR) || !count(c, i))
            return fail(c, "%s: a list of device names", p);
        for (int k = 0; k < count(c, i); k++) {
            const char *n = str_of(c, at(c, i, k));
            int dv = -1;
            for (int x = 0; n && x < s->ndev; x++)
                if (!strcmp(s->dev[x].name, n))
                    dv = x;
            if (dv < 0)
                return fail(c, "%s[%d]: %s isn't in devices", p, k, n ? n : "(not a name)");
            s->freq[f].devices |= 1u << dv;
        }
    }
    /* a pinned frequency: only its devices transmit there */
    for (int f = 0; f < s->nfreq; f++)
        for (int dv = 0; s->freq[f].devices && dv < s->ndev; dv++)
            if (!(s->freq[f].devices >> dv & 1))
                s->dev[dv].transmits &= ~(1ull << f);
    /* a modem with no "hears" whose first frequency is a repeater's output hears that repeater's input: one duplex
       modem is the whole repeater. Not a radio_hotspot (half duplex: a split's TX half), and a modem of type auto
       only when no other device hears the input (then it's a split's TX half too) */
    for (int dv = 0; dv < s->ndev; dv++) {
        rdof_device3_t *d = &s->dev[dv];
        const int out = d->idle;
        if (!(dflt >> dv & 1) || out < 0 || s->freq[out].app < 0 || s->app[s->freq[out].app].out != out ||
            !strcmp(d->driver, "radio_hotspot"))
            continue;
        const int in = s->app[s->freq[out].app].in;
        if (s->freq[in].devices && !(s->freq[in].devices >> dv & 1))
            continue;
        bool other = false;
        for (int x = 0; x < s->ndev && !other; x++)
            other = x != dv && rdof_site_dev_hears(s, x, in);
        if (!other || strcmp(d->driver, "auto"))
            d->idle = in;
    }
    /* a bench modem: only what pins it (and a unit that takes it is refused, rdof/siteplan.h) */
    for (int dv = 0; dv < s->ndev; dv++) {
        if (!(bench >> dv & 1))
            continue;
        rdof_device3_t *d = &s->dev[dv];
        for (int f = 0; f < s->nfreq; f++)
            if ((s->freq[f].devices >> dv & 1) && s->freq[f].tx)
                d->transmits |= 1ull << f;
        for (int f = 0; f < s->nfreq && d->idle < 0; f++)
            if (d->transmits >> f & 1)
                d->idle = f;
        if (d->idle >= 0)
            d->hears = RDOF_HEARS_IDLE;
    }
    return true;
}

/* The table's keys for what's left; a receive-only path can't take calls from a network. */
static bool finish_path(ctx_t *c, rdof_site_t *s, rdof_path_t *p, bool *have, const char *path)
{
    const bool rx_only = p->tx_freq < 0;
    pol_table(s, &p->pol, have, rx_only);
    if (rx_only && (p->pol.dir & RDOF_FLOW_NET_RF))
        return fail(c, "%s.dir: %s is receive only (\"tx\": false): rf>net", path, p->name);
    s->npath++;
    return true;
}

/* A path's policy, from its layers. */
static bool make_path(ctx_t *c, rdof_site_t *s, const char *name, int freq, int tx_freq, int app, int slot,
                      int own, const char *own_path, int base, const char *base_path, bool use_defaults)
{
    if (s->npath >= RDOF_SITE_MAX_PATHS)
        return fail(c, "%d talk paths at most", RDOF_SITE_MAX_PATHS);
    rdof_path_t *p = &s->path[s->npath];
    memset(p, 0, sizeof *p);
    snprintf(p->name, sizeof p->name, "%s", name);
    p->freq = freq;
    p->tx_freq = tx_freq;
    p->app = app;
    p->slot = (uint8_t)slot;
    p->tx_slot = (uint8_t)(slot ? slot : s->freq[freq].ts);
    bool have[RDOF_PK_N] = {0};
    if (own >= 0 && !pol_named(c, s, own, own_path, &p->pol, have, RDOF_SRC_OWN))
        return false;
    if (base >= 0 && !pol_named(c, s, base, base_path, &p->pol, have, RDOF_SRC_OWN))
        return false;
    if (use_defaults && !pol_named(c, s, c->defaults, "defaults", &p->pol, have, RDOF_SRC_DEFAULTS))
        return false;
    return finish_path(c, s, p, have, own_path);
}

/* A slots object: "1" and "2", each a policy name or an inline policy. Into node indexes (-1 none) and whether
   each is a bare name (then it's in name_of). */
static bool slots_of(ctx_t *c, const rdof_site_t *s, int obj, const char *path, int node[2], const char *name[2])
{
    node[0] = node[1] = -1;
    name[0] = name[1] = NULL;
    if (obj < 0)
        return true;
    if (!is_type(c, obj, RDOF_JOBJ))
        return fail(c, "%s: {\"1\": policy, \"2\": policy}", path);
    for (int k = 0; k < count(c, obj); k++) {
        int e = at(c, obj, k);
        const char *key = N(c, e)->key;
        if (strcmp(key, "1") && strcmp(key, "2"))
            return fail(c, "%s.%s: slots \"1\" and \"2\"", path, key);
        int sl = key[0] - '1';
        char p[96];
        snprintf(p, sizeof p, "%s.%s", path, key);
        if (str_of(c, e)) {
            if (policy_index(s, str_of(c, e)) < 0)
                return fail(c, "%s: %s isn't in policies", p, str_of(c, e));
            name[sl] = str_of(c, e);
        } else if (is_type(c, e, RDOF_JOBJ)) {
            if (!known(c, e, p, SLOT_KEYS))
                return false;
            node[sl] = e;
        } else {
            return fail(c, "%s: a policy's name, or a policy ({\"tg\": ...})", p);
        }
    }
    return true;
}

/* A slot's own layer: an inline policy, or a named one (a bare name is that policy's keys, as a source). */
static bool slot_path(ctx_t *c, rdof_site_t *s, const char *name, int freq, int tx_freq, int app, int slot,
                      int node, const char *pname, const char *slot_path_, int base, const char *base_path,
                      bool use_defaults)
{
    if (pname) {
        /* a bare policy name: as if {"policy": name} */
        int k = policy_index(s, pname);
        if (s->npath >= RDOF_SITE_MAX_PATHS)
            return fail(c, "%d talk paths at most", RDOF_SITE_MAX_PATHS);
        rdof_path_t *p = &s->path[s->npath];
        memset(p, 0, sizeof *p);
        snprintf(p->name, sizeof p->name, "%s", name);
        p->freq = freq, p->tx_freq = tx_freq, p->app = app, p->slot = (uint8_t)slot;
        p->tx_slot = (uint8_t)slot;
        bool have[RDOF_PK_N] = {0};
        char pp[64];
        snprintf(pp, sizeof pp, "policies.%s", pname);
        if (!pol_layer(c, s, c->pnode[k], pp, &p->pol, have, k))
            return false;
        if (base >= 0 && !pol_named(c, s, base, base_path, &p->pol, have, RDOF_SRC_OWN))
            return false;
        if (use_defaults && !pol_named(c, s, c->defaults, "defaults", &p->pol, have, RDOF_SRC_DEFAULTS))
            return false;
        return finish_path(c, s, p, have, slot_path_);
    }
    return make_path(c, s, name, freq, tx_freq, app, slot, node, slot_path_, base, base_path, use_defaults);
}

static bool make_paths(ctx_t *c, rdof_site_t *s)
{
    char name[RDOF_SITE_NAME + 4], p[96], bp[80];
    for (int f = 0; f < s->nfreq; f++) {
        rdof_freq_t *fr = &s->freq[f];
        if (fr->app >= 0)
            continue;
        int o = c->fnode[f];
        snprintf(bp, sizeof bp, "frequencies.%s", fr->name);
        snprintf(p, sizeof p, "%s.slots", bp);
        int sn[2];
        const char *pn[2];
        int slots = get(c, o, "slots");
        if (!slots_of(c, s, slots, p, sn, pn))
            return false;
        const int txf = fr->tx ? f : -1;
        if (fr->mode == RDOF_FMODE_DCDM || (slots >= 0 && !fr->tx)) {
            for (int sl = 1; sl <= 2; sl++) {
                snprintf(name, sizeof name, "%s/%d", fr->name, sl);
                char sp[112];
                snprintf(sp, sizeof sp, "%s.%d", p, sl);
                if (!slot_path(c, s, name, f, txf, -1, sl, sn[sl - 1], pn[sl - 1], sp, o, bp, true))
                    return false;
            }
        } else {
            if (slots >= 0)
                return fail(c, "%s: per-slot policy is for mode dcdm or a frequency with \"tx\": false", p);
            if (!make_path(c, s, fr->name, f, txf, -1, 0, o, bp, -1, NULL, true))
                return false;
        }
    }
    for (int a = 0; a < s->napp; a++) {
        rdof_app3_t *ap = &s->app[a];
        if (ap->type != RDOF_APP_REPEATER)
            continue;
        int o = c->anode[a];
        snprintf(bp, sizeof bp, "apps.%s", ap->name);
        snprintf(p, sizeof p, "%s.slots", bp);
        int sn[2];
        const char *pn[2];
        if (!slots_of(c, s, get(c, o, "slots"), p, sn, pn))
            return false;
        for (int sl = 1; sl <= 2; sl++) {
            snprintf(name, sizeof name, "%s/%d", ap->name, sl);
            char sp[112];
            snprintf(sp, sizeof sp, "%s.%d", p, sl);
            ap->path[sl - 1] = s->npath;
            if (!slot_path(c, s, name, ap->in, ap->out, a, sl, sn[sl - 1], pn[sl - 1], sp, o, bp, false))
                return false;
        }
    }
    return true;
}

/* Each path's HBlink peer: the site's id, or (per_path) its own: one a frequency or a repeater, both slots. */
static bool peer_ids(ctx_t *c, rdof_site_t *s)
{
    int hb = -1;
    for (int k = 0; k < s->nnet; k++)
        if (s->net[k].type == RDOF_NET_HBLINK)
            hb = k;
    const bool login = hb >= 0 && s->net[hb].login;
    if (hb >= 0 && !s->id && !login)
        return fail(c, "id: the box's DMR ID (its HBlink peer ID), 1 to 16777215");
    if (login) {
        const char *cs = str_of(c, get(c, c->root, "callsign"));
        if (!cs || !cs[0] || strlen(cs) > 8)
            return fail(c, "callsign: the owner's callsign, required with a brandmeister network (BrandMeister checks "
                           "it against the logins' IDs), 8 characters at most");
    }
    uint32_t next = s->id;
    int ids = hb >= 0 ? get(c, c->nnode[hb], "peer_ids") : -1;
    for (int k = 0; k < count(c, ids); k++) {
        const char *key = N(c, at(c, ids, k))->key;
        int a = rdof_site_app(s, key), f = rdof_site_freq(s, key);
        bool found = (a >= 0 && s->app[a].type == RDOF_APP_REPEATER) || (f >= 0 && s->freq[f].app < 0);
        double v;
        if (!found)
            return fail(c, "networks.%s.peer_ids.%s: not a frequency's or a repeater's name", s->net[hb].name, key);
        if (!num_of(c, at(c, ids, k), &v) || v < 1 || v > 4294967295.0 || v != floor(v))
            return fail(c, "networks.%s.peer_ids.%s: a peer ID", s->net[hb].name, key);
    }
    for (int k = 0; k < s->npath; k++) {
        rdof_path_t *p = &s->path[k];
        p->peer_id = s->id;
        if (hb < 0 || !s->net[hb].per_path || p->pol.net != hb)
            continue;
        /* the other slot of the same frequency or repeater already has one */
        int same = -1;
        for (int j = 0; j < k && same < 0; j++)
            if (s->path[j].freq == p->freq && s->path[j].app == p->app && s->path[j].pol.net == hb)
                same = j;
        if (same >= 0) {
            p->peer_id = s->path[same].peer_id;
            memcpy(p->peer_name, s->path[same].peer_name, sizeof p->peer_name);
            continue;
        }
        const char *key = p->app >= 0 ? s->app[p->app].name : s->freq[p->freq].name;
        double v;
        if (login && !num_of(c, get(c, ids, key), &v))
            return fail(c, "networks.%s.peer_ids.%s: each path on BrandMeister logs in with its own registered ID",
                        s->net[hb].name, key);
        p->peer_id = num_of(c, get(c, ids, key), &v) ? (uint32_t)v : ++next;
        for (int j = 0; login && j < k; j++)
            if (s->path[j].pol.net == hb && s->path[j].peer_id == p->peer_id)
                return fail(c, "networks.%s.peer_ids.%s: %lu is %s's too (one login per ID)", s->net[hb].name, key,
                            (unsigned long)p->peer_id, s->path[j].name);
        if (!login) /* HBlink4: the master's callsign field names the path (8 characters); BrandMeister checks it is
                       the owner's callsign, the top level's */
            snprintf(p->peer_name, sizeof p->peer_name, "%.8s", key);
    }
    /* BrandMeister: a frequency or a repeater is a login, so a device that transmits there transmits nowhere else
       (no roaming transceiver: the master can't move it) */
    for (int d = 0; login && d < s->ndev; d++) {
        const uint64_t t = s->dev[d].can_tx ? s->dev[d].transmits : 0;
        if (!t || !(t & (t - 1)))
            continue;
        for (int k = 0; k < s->npath; k++)
            if (s->path[k].pol.net == hb && s->path[k].tx_freq >= 0 && (t >> s->path[k].tx_freq & 1))
                return fail(c, "devices.%s.transmits: on BrandMeister each frequency or repeater is its own login, so "
                               "a device transmits on one frequency (%s is on %s)",
                            s->dev[d].name, s->path[k].name, s->net[hb].name);
    }
    return true;
}

static bool parse_site(ctx_t *c, rdof_site_t *s)
{
    memset(s, 0, sizeof *s);
    if (c->root < 0)
        return fail(c, "JSON: %s", c->doc.error);
    if (!is_type(c, c->root, RDOF_JOBJ))
        return fail(c, "the settings are a JSON object");
    double v;
    if (!num_of(c, get(c, c->root, "version"), &v) || v != 3)
        return fail(c, "version: 3");
    if (!known(c, c->root, "", TOP_KEYS))
        return false;
    const char *bn = str_of(c, get(c, c->root, "name"));
    snprintf(s->name, sizeof s->name, "%s", bn && bn[0] ? bn : "radiofoundry");
    int i = get(c, c->root, "id");
    if (i >= 0) {
        if (!num_of(c, i, &v) || v != floor(v) || v < 1 || v > RDOF_ID_MAX)
            return fail(c, "id: a DMR ID, 1 to 16777215");
        s->id = (uint32_t)v;
    }
    s->pc_hold_s = 900;
    i = get(c, c->root, "pc_hold_s");
    if (i >= 0) {
        if (!num_of(c, i, &v) || v < 1 || v > 86400)
            return fail(c, "pc_hold_s: seconds, 1 to 86400");
        s->pc_hold_s = v;
    }
    c->defaults = get(c, c->root, "defaults");
    if (c->defaults >= 0 && !is_type(c, c->defaults, RDOF_JOBJ))
        return fail(c, "defaults: an object of frequency keys");
    if (get(c, c->defaults, "power") >= 0)
        return fail(c, "defaults.power: power_dbm, in dBm at the antenna");
    if (!known(c, c->defaults, "defaults", DEFAULT_KEYS))
        return false;
    return parse_freqs(c, s) && parse_nets(c, s) && parse_apps(c, s) && parse_policies(c, s) &&
           parse_devices(c, s) && make_paths(c, s) && peer_ids(c, s);
}

bool rdof_site_ctx_parse(rdof_site_ctx_t *c, rdof_site_t *s) { return parse_site(c, s); }

bool rdof_site_parse(const char *text, size_t len, rdof_site_t *s, char *err, size_t cap)
{
    ctx_t *c = open_doc(text, len, err, cap);
    if (!c) {
        memset(s, 0, sizeof *s);
        return false;
    }
    bool ok = parse_site(c, s);
    close_doc(c);
    return ok;
}

/* ── writing JSON ──────────────────────────────────────────────────────────── */

typedef struct {
    char *s;
    size_t len, cap;
    bool oom;
    int depth;
    bool first[24];
} jw_t;

static void put(jw_t *w, const char *p, size_t n)
{
    if (w->oom)
        return;
    if (w->len + n + 1 > w->cap) {
        size_t cap = w->cap ? w->cap * 2 : 4096;
        while (cap < w->len + n + 1)
            cap *= 2;
        char *s = realloc(w->s, cap);
        if (!s) {
            w->oom = true;
            return;
        }
        w->s = s;
        w->cap = cap;
    }
    memcpy(w->s + w->len, p, n);
    w->len += n;
    w->s[w->len] = 0;
}

static void puts_(jw_t *w, const char *p) { put(w, p, strlen(p)); }

static void put_str(jw_t *w, const char *s)
{
    put(w, "\"", 1);
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        char esc[8];
        if (ch == '"' || ch == '\\') {
            esc[0] = '\\', esc[1] = (char)ch;
            put(w, esc, 2);
        } else if (ch < 0x20) {
            snprintf(esc, sizeof esc, "\\u%04x", ch);
            put(w, esc, 6);
        } else {
            put(w, s, 1);
        }
    }
    put(w, "\"", 1);
}

static void key(jw_t *w, const char *k)
{
    if (w->depth > 0) {
        if (!w->first[w->depth])
            put(w, ",", 1);
        w->first[w->depth] = false;
    }
    if (k) {
        put_str(w, k);
        put(w, ":", 1);
    }
}

static void open_(jw_t *w, const char *k, char brace)
{
    key(w, k);
    put(w, &brace, 1);
    if (w->depth < 23)
        w->first[++w->depth] = true;
}

static void close_(jw_t *w, char brace)
{
    put(w, &brace, 1);
    if (w->depth > 0)
        w->depth--;
}

static void w_num(jw_t *w, const char *k, double v)
{
    char t[48];
    if (v == floor(v) && fabs(v) < 1e15)
        snprintf(t, sizeof t, "%.0f", v + 0.0);
    else
        snprintf(t, sizeof t, "%.17g", v);
    key(w, k);
    puts_(w, t);
}

static void w_str(jw_t *w, const char *k, const char *v)
{
    key(w, k);
    put_str(w, v);
}

static void w_bool(jw_t *w, const char *k, bool v)
{
    key(w, k);
    puts_(w, v ? "true" : "false");
}

/* ── what the plan shares ──────────────────────────────────────────────────── */

/* Device d hears (receives) / transmits on frequency f: its hears and transmits, and the frequency's devices. */
bool rdof_site_dev_hears(const rdof_site_t *s, int d, int f)
{
    const rdof_device3_t *dv = &s->dev[d];
    if (s->freq[f].devices && !(s->freq[f].devices >> d & 1))
        return false;
    switch (dv->hears) {
    case RDOF_HEARS_WINDOW: return true;
    case RDOF_HEARS_IDLE: return dv->idle == f;
    case RDOF_HEARS_SCAN: return (dv->scan >> f & 1) || dv->idle == f;
    default: return false;
    }
}

bool rdof_site_dev_sends(const rdof_site_t *s, int d, int f)
{
    return s->dev[d].can_tx && (s->dev[d].transmits >> f & 1);
}

#define dev_hears rdof_site_dev_hears
#define dev_sends rdof_site_dev_sends

/* The first ID of a set, 0 if it's empty. */
uint32_t rdof_site_first_id(const rdof_idset_t *set)
{
    rdof_idrange_t r[1];
    return rdof_idset_ranges(set, r, 1) ? r[0].lo : 0;
}

/* The hblink network, -1 none. */
int rdof_site_hb_net(const rdof_site_t *s)
{
    for (int k = 0; k < s->nnet; k++)
        if (s->net[k].type == RDOF_NET_HBLINK)
            return k;
    return -1;
}


/* ── the resolved view ─────────────────────────────────────────────────────── */

static void w_idset(jw_t *w, const char *k, const rdof_idset_t *set)
{
    char t[256];
    rdof_idset_format(set, t, sizeof t);
    if (t[0])
        w_str(w, k, t);
    else
        w_bool(w, k, false);
}

static void w_src(jw_t *w, const rdof_site_t *s, const char *k, int src)
{
    if (src >= 0) {
        char t[RDOF_SITE_NAME + 8];
        snprintf(t, sizeof t, "policy %s", s->pol_name[src]);
        w_str(w, k, t);
    } else {
        w_str(w, k, src == RDOF_SRC_OWN ? "own" : src == RDOF_SRC_DEFAULTS ? "defaults" : "default");
    }
}

static const char *const DIR_NAMES[] = {"", "rf>net", "net>rf", "both"};

size_t rdof_site_resolved(const rdof_site_t *s, char *out, size_t cap)
{
    jw_t w = {0};
    char t[64];
    open_(&w, NULL, '{');
    w_num(&w, "version", 3);
    w_str(&w, "name", s->name);
    w_num(&w, "id", s->id);
    w_num(&w, "pc_hold_s", s->pc_hold_s);
    open_(&w, "frequencies", '[');
    for (int f = 0; f < s->nfreq; f++) {
        const rdof_freq_t *fr = &s->freq[f];
        open_(&w, NULL, '{');
        w_str(&w, "name", fr->name);
        format_mhz(fr->hz, t, sizeof t);
        key(&w, "freq");
        puts_(&w, t);
        if (fr->cc >= 0)
            w_num(&w, "cc", fr->cc);
        else
            w_str(&w, "cc", "any");
        static const char *const modes[] = {"simplex", "sfr", "dcdm"};
        w_str(&w, "mode", modes[fr->mode]);
        w_num(&w, "ts", fr->ts);
        w_bool(&w, "tx", fr->tx);
        if (!isnan(fr->power_dbm))
            w_num(&w, "power_dbm", fr->power_dbm);
        if (fr->app >= 0)
            w_str(&w, "app", s->app[fr->app].name);
        open_(&w, "heard_by", '[');
        for (int d = 0; d < s->ndev; d++)
            if (dev_hears(s, d, f))
                w_str(&w, NULL, s->dev[d].name);
        close_(&w, ']');
        open_(&w, "sent_by", '[');
        for (int d = 0; d < s->ndev; d++)
            if (dev_sends(s, d, f))
                w_str(&w, NULL, s->dev[d].name);
        close_(&w, ']');
        close_(&w, '}');
    }
    close_(&w, ']');
    open_(&w, "paths", '[');
    for (int k = 0; k < s->npath; k++) {
        const rdof_path_t *p = &s->path[k];
        const rdof_policy_t *pol = &p->pol;
        open_(&w, NULL, '{');
        w_str(&w, "name", p->name);
        w_str(&w, "rx", s->freq[p->freq].name);
        if (p->tx_freq >= 0)
            w_str(&w, "tx", s->freq[p->tx_freq].name);
        else
            w_bool(&w, "tx", false);
        if (p->slot)
            w_num(&w, "slot", p->slot);
        else
            w_str(&w, "slot", "both");
        if (p->tx_freq >= 0)
            w_num(&w, "tx_slot", p->tx_slot);
        w_idset(&w, "tg", &pol->tg);
        w_idset(&w, "ptt", &pol->ptt);
        w_num(&w, "ptt_hold_s", pol->ptt_hold_s);
        w_idset(&w, "pc", &pol->pc);
        open_(&w, "apps", '[');
        for (int a = 0; a < s->napp; a++)
            if (pol->apps >> a & 1)
                w_str(&w, NULL, s->app[a].name);
        close_(&w, ']');
        if (pol->net >= 0)
            w_str(&w, "net", s->net[pol->net].name);
        else
            w_bool(&w, "net", false);
        w_str(&w, "dir", DIR_NAMES[pol->dir & 3]);
        if (pol->net >= 0 && s->net[pol->net].type == RDOF_NET_HBLINK)
            w_num(&w, "peer_id", p->peer_id);
        if (pol->net >= 0 && s->net[pol->net].type == RDOF_NET_HBLINK && p->peer_name[0])
            w_str(&w, "peer_name", p->peer_name);
        open_(&w, "from", '{');
        for (int x = 0; x < RDOF_PK_N; x++)
            w_src(&w, s, PK_NAMES[x], pol->src[x]);
        close_(&w, '}');
        close_(&w, '}');
    }
    close_(&w, ']');
    /* the reverse view: each talkgroup range, the paths that carry it and how */
    open_(&w, "talkgroups", '[');
    for (int k = 0; k < s->npath; k++) {
        const rdof_policy_t *pol = &s->path[k].pol;
        for (int which = 0; which < 2; which++) {
            const rdof_idset_t *set = which ? &pol->ptt : &pol->tg;
            if (rdof_idset_empty(set))
                continue;
            char ids[256];
            /* the listed ranges, then "*" as one entry */
            rdof_idset_t listed = *set;
            listed.star = false;
            rdof_idset_format(&listed, ids, sizeof ids);
            if (ids[0]) {
                open_(&w, NULL, '{');
                w_str(&w, "tg", ids);
                w_str(&w, "path", s->path[k].name);
                w_str(&w, "how", which ? "ptt" : "static");
                close_(&w, '}');
            }
            if (set->star) { /* "*" and its exclusions, the listed ranges above */
                rdof_idset_t star = *set;
                memmove(star.r, star.r + star.nin, star.nout * sizeof star.r[0]);
                star.nin = 0;
                rdof_idset_format(&star, ids, sizeof ids);
                open_(&w, NULL, '{');
                w_str(&w, "tg", ids);
                w_str(&w, "path", s->path[k].name);
                w_str(&w, "how", which ? "ptt *" : "*");
                close_(&w, '}');
            }
        }
    }
    close_(&w, ']');
    open_(&w, "apps", '[');
    for (int a = 0; a < s->napp; a++) {
        const rdof_app3_t *ap = &s->app[a];
        static const char *const types[] = {"", "repeater", "parrot", "ipgw"};
        open_(&w, NULL, '{');
        w_str(&w, "name", ap->name);
        w_str(&w, "type", types[ap->type]);
        if (ap->type == RDOF_APP_REPEATER) {
            w_str(&w, "in", s->freq[ap->in].name);
            w_str(&w, "out", s->freq[ap->out].name);
        } else {
            if (!rdof_idset_empty(&ap->tg))
                w_idset(&w, "tg", &ap->tg);
            if (!rdof_idset_empty(&ap->pc))
                w_idset(&w, "pc", &ap->pc);
        }
        close_(&w, '}');
    }
    close_(&w, ']');
    open_(&w, "networks", '[');
    for (int k = 0; k < s->nnet; k++) {
        const rdof_net3_t *n = &s->net[k];
        open_(&w, NULL, '{');
        w_str(&w, "name", n->name);
        w_str(&w, "type", n->login ? "brandmeister" : n->type == RDOF_NET_HBLINK ? "hblink" : "mqtt");
        if (n->type == RDOF_NET_HBLINK && !n->login) {
            w_str(&w, "identity", n->per_path ? "per_path" : "single");
            w_bool(&w, "roaming", n->roaming);
        } else {
            w_bool(&w, "report", n->report);
            w_bool(&w, "calls", n->calls);
            if (n->format == RDOF_MQTT_FORMAT_V3) {
                w_str(&w, "format", "v3");
                w_num(&w, "bundle", n->bundle);
                w_bool(&w, "raw_tx", n->raw_tx);
                w_str(&w, "transport", n->transport == RDOF_MQTT_TLS ? "mqtt/tls" : "mqtt/tcp");
            }
        }
        close_(&w, '}');
    }
    close_(&w, ']');
    open_(&w, "devices", '[');
    for (int d = 0; d < s->ndev; d++) {
        const rdof_device3_t *dv = &s->dev[d];
        static const char *const kinds[] = {"", "sdr", "modem", "radio"};
        static const char *const hears[] = {"window", "idle", "scan", "none"};
        open_(&w, NULL, '{');
        w_str(&w, "name", dv->name);
        w_str(&w, "kind", kinds[dv->kind]);
        w_str(&w, "driver", dv->driver);
        w_str(&w, "hears", hears[dv->hears]);
        if (dv->idle >= 0)
            w_str(&w, "idle", s->freq[dv->idle].name);
        open_(&w, "transmits", '[');
        for (int f = 0; f < s->nfreq; f++)
            if (dev_sends(s, d, f))
                w_str(&w, NULL, s->freq[f].name);
        close_(&w, ']');
        close_(&w, '}');
    }
    close_(&w, ']');
    close_(&w, '}');
    size_t n = 0;
    if (cap) {
        n = w.s && !w.oom ? w.len : 0;
        if (n >= cap)
            n = cap - 1;
        if (n)
            memcpy(out, w.s, n);
        out[n] = 0;
    }
    free(w.s);
    return n;
}

size_t rdof_site_resolved_text(const rdof_site_t *s, char *out, size_t cap)
{
    char *in = cap ? malloc(cap) : NULL;
    if (!in) {
        if (cap)
            out[0] = 0;
        return 0;
    }
    rdof_site_resolved(s, in, cap);
    bool brk[16] = {false}, str = false;
    int depth = 0;
    size_t n = 0;
#define PUT(ch) (n + 1 < cap ? (void)(out[n++] = (ch)) : (void)0)
#define LINE(d) do { PUT('\n'); for (int i_ = 0; i_ < 2 * (d); i_++) PUT(' '); } while (0)
    for (const char *c = in; *c; c++) {
        if (str) {
            PUT(*c);
            if (*c == '\\' && c[1])
                PUT(*++c);
            else if (*c == '"')
                str = false;
            continue;
        }
        switch (*c) {
        case '"':
            str = true;
            PUT(*c);
            break;
        case '{':
        case '[':
            PUT(*c);
            if (depth < 15) {
                /* the top object a line per key, a list of objects a line per entry, the rest on one line */
                const bool b = *c == '{' ? depth == 0 : c[1] == '{';
                brk[++depth] = b;
                if (brk[depth])
                    LINE(depth);
            }
            break;
        case '}':
        case ']':
            if (depth > 0 && brk[depth])
                LINE(depth - 1);
            PUT(*c);
            if (depth > 0)
                depth--;
            break;
        case ',':
            PUT(',');
            if (brk[depth])
                LINE(depth);
            else
                PUT(' ');
            break;
        case ':':
            PUT(':');
            PUT(' ');
            break;
        default:
            PUT(*c);
        }
    }
#undef PUT
#undef LINE
    out[n] = 0;
    free(in);
    return n;
}
