// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The MQTT network's control documents: see rdof/mnctl.h. */
#include "rdof/mnctl.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/json.h"
#include "rdof/jsonp.h"
#include "rdof/tracker.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif

#define NODES 160  /* a document's JSON nodes at most */
#define STRBUF 2048

static size_t fit(int n, size_t cap) { return n > 0 && (size_t)n < cap ? (size_t)n : 0; }

static void seterr(char *err, size_t cap, const char *msg)
{
    if (err && cap)
        snprintf(err, cap, "%s", msg);
}

/* ── time ─────────────────────────────────────────────────────────────────── */

/* days from 1970-01-01 to y-m-d (proleptic Gregorian) */
static int64_t days_from_civil(int64_t y, int m, int d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static bool digits(const char *s, int n, int *v)
{
    *v = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return false;
        *v = *v * 10 + (s[i] - '0');
    }
    return true;
}

bool rdof_mn_iso_parse(const char *s, int64_t *ms)
{
    int y, mo, d, h, mi, se;
    if (!s || strlen(s) < 20 || !digits(s, 4, &y) || s[4] != '-' || !digits(s + 5, 2, &mo) || s[7] != '-' ||
        !digits(s + 8, 2, &d) || s[10] != 'T' || !digits(s + 11, 2, &h) || s[13] != ':' || !digits(s + 14, 2, &mi) ||
        s[16] != ':' || !digits(s + 17, 2, &se))
        return false;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 60)
        return false;
    const char *p = s + 19;
    int frac = 0;
    if (*p == '.') {
        p++;
        int scale = 100, n = 0;
        while (*p >= '0' && *p <= '9') {
            if (n < 3)
                frac += (*p - '0') * scale;
            scale /= 10;
            p++;
            n++;
        }
        if (!n)
            return false;
    }
    if (p[0] != 'Z' || p[1])
        return false;
    *ms = ((days_from_civil(y, mo, d) * 24 + h) * 60 + mi) * 60000LL + se * 1000LL + frac;
    return true;
}

static void iso(int64_t ms, char out[32]) { rdof_iso_ms(ms, out); }

/* ── topics ──────────────────────────────────────────────────────────────── */

size_t rdof_mn_ms_topic(char *out, size_t cap, const char *root, const char *node, uint32_t id)
{
    return fit(snprintf(out, cap, "%s/node/%s/ms/%lu", root, node, (unsigned long)id), cap);
}

/* topic starts with root + "/"; the rest */
static const char *under(const char *topic, const char *root)
{
    const size_t n = strlen(root);
    return strncmp(topic, root, n) || topic[n] != '/' ? NULL : topic + n + 1;
}

/* a decimal ID that is the whole string */
static bool id_of(const char *s, uint32_t *id)
{
    if (!*s || (s[0] == '0' && s[1]))
        return false;
    unsigned long v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > RDOF_ID_MAX)
            return false;
        v = v * 10 + (unsigned long)(*s - '0');
    }
    if (v > RDOF_ID_MAX)
        return false;
    *id = (uint32_t)v;
    return true;
}

bool rdof_mn_ms_topic_parse(const char *topic, const char *root, char *node, size_t cap, uint32_t *id)
{
    const char *r = under(topic, root);
    if (!r || strncmp(r, "node/", 5))
        return false;
    r += 5;
    const char *slash = strchr(r, '/');
    if (!slash || strncmp(slash, "/ms/", 4))
        return false;
    const size_t n = (size_t)(slash - r);
    if (!n || n > RDOF_MN_NODE_MAX || !id_of(slash + 4, id))
        return false;
    if (node && cap) {
        const size_t k = n < cap - 1 ? n : cap - 1;
        memcpy(node, r, k);
        node[k] = 0;
    }
    return true;
}

size_t rdof_mn_where_topic(char *out, size_t cap, const char *root, uint32_t id)
{
    return fit(snprintf(out, cap, "%s/where/%lu", root, (unsigned long)id), cap);
}

bool rdof_mn_where_topic_parse(const char *topic, const char *root, uint32_t *id)
{
    const char *r = under(topic, root);
    return r && !strncmp(r, "where/", 6) && id_of(r + 6, id);
}

/* ── JSON reading helpers ────────────────────────────────────────────────── */

typedef struct {
    rdof_jdoc_t doc;
    rdof_jnode_t nodes[NODES];
    char buf[STRBUF];
} jp_t;

/* the one parse in progress (nothing here is reentrant: one thread calls these) */
static EXT_RAM_BSS_ATTR jp_t P;

static int jp_parse(jp_t *p, const char *json, size_t len, char *err, size_t cap)
{
    p->doc = (rdof_jdoc_t){.nodes = p->nodes, .max = NODES, .buf = p->buf, .cap = sizeof p->buf};
    const int root = rdof_jparse(&p->doc, json, len);
    if (root < 0) {
        seterr(err, cap, p->doc.error[0] ? p->doc.error : "not JSON");
        return -1;
    }
    if (rdof_jnode(&p->doc, root)->type != RDOF_JOBJ) {
        seterr(err, cap, "not a JSON object");
        return -1;
    }
    return root;
}

static const rdof_jnode_t *jget(const jp_t *p, int obj, const char *key)
{
    return rdof_jnode(&p->doc, rdof_jget(&p->doc, obj, key));
}

static bool jint(const rdof_jnode_t *n, long lo, long hi, long *v)
{
    if (!n || n->type != RDOF_JNUM || n->num != floor(n->num) || n->num < lo || n->num > hi)
        return false;
    *v = (long)n->num;
    return true;
}

/* an ID set from a string, a number or an array of them (as the settings take them) */
static bool jidset(const jp_t *p, int node, rdof_idset_t *s, char *err, size_t cap)
{
    const rdof_jnode_t *n = rdof_jnode(&p->doc, node);
    rdof_idset_clear(s);
    char t[24];
    if (n->type == RDOF_JSTR)
        return rdof_idset_add(s, n->str, err, cap) ? (rdof_idset_finish(s), true) : false;
    if (n->type == RDOF_JNUM) {
        long v;
        if (!jint(n, 0, RDOF_ID_MAX, &v)) {
            seterr(err, cap, "an ID is 0 to 16777215");
            return false;
        }
        snprintf(t, sizeof t, "%ld", v);
        return rdof_idset_add(s, t, err, cap) ? (rdof_idset_finish(s), true) : false;
    }
    if (n->type == RDOF_JBOOL && !n->b) /* false: the empty set */
        return true;
    if (n->type != RDOF_JARR) {
        seterr(err, cap, "an ID set is a string, a number or an array");
        return false;
    }
    for (int i = 0; i < n->count; i++) {
        const rdof_jnode_t *e = rdof_jnode(&p->doc, rdof_jat(&p->doc, node, i));
        long v;
        const char *item = NULL;
        if (e->type == RDOF_JSTR)
            item = e->str;
        else if (jint(e, 0, RDOF_ID_MAX, &v)) {
            snprintf(t, sizeof t, "%ld", v);
            item = t;
        }
        if (!item) {
            seterr(err, cap, "an ID set's items are strings or IDs");
            return false;
        }
        if (!rdof_idset_add(s, item, err, cap))
            return false;
    }
    rdof_idset_finish(s);
    return true;
}

/* an array of IDs */
static bool jids(const jp_t *p, int node, uint32_t *ids, int max, int *n, char *err, size_t cap)
{
    const rdof_jnode_t *a = rdof_jnode(&p->doc, node);
    *n = 0;
    if (a->type != RDOF_JARR) {
        seterr(err, cap, "expected an array of IDs");
        return false;
    }
    for (int i = 0; i < a->count; i++) {
        long v;
        if (!jint(rdof_jnode(&p->doc, rdof_jat(&p->doc, node, i)), 1, RDOF_ID_MAX, &v)) {
            seterr(err, cap, "an ID is 1 to 16777215");
            return false;
        }
        if (*n < max)
            ids[(*n)++] = (uint32_t)v;
    }
    return true;
}

static void note_add(char *note, size_t cap, const char *key)
{
    if (!note || !cap)
        return;
    const size_t n = strlen(note);
    snprintf(note + n, cap - n, "%s%s", n ? ", " : "ignored: ", key);
}

/* ── registration records ────────────────────────────────────────────────── */

static void ids_raw(rdof_json_t *j, const char *key, const uint32_t *ids, int n)
{
    char t[RDOF_MN_CFG_NEIGHBORS * 12 + 4];
    size_t k = 0;
    t[k++] = '[';
    for (int i = 0; i < n && k + 12 < sizeof t; i++)
        k += (size_t)snprintf(t + k, sizeof t - k, "%s%lu", i ? "," : "", (unsigned long)ids[i]);
    t[k++] = ']';
    t[k] = 0;
    rdof_json_raw(j, key, t);
}

size_t rdof_mn_record_json(const rdof_mn_record_t *r, char *out, size_t cap)
{
    rdof_json_t j;
    char t[32];
    rdof_json_init(&j, out, cap);
    rdof_json_open(&j, NULL);
    rdof_json_int(&j, "id", r->id);
    iso(r->at_ms, t);
    rdof_json_str(&j, "at", t);
    iso(r->exp_ms, t);
    rdof_json_str(&j, "exp", t);
    rdof_json_str(&j, "how", r->implicit ? "call" : "register");
    ids_raw(&j, "tg", r->tg, r->ntg);
    if (r->sub)
        rdof_json_int(&j, "sub", r->sub);
    if (r->area >= 0)
        rdof_json_int(&j, "area", r->area);
    rdof_json_close(&j);
    return rdof_json_done(&j) ? j.len : 0;
}

bool rdof_mn_record_parse(const char *json, size_t len, rdof_mn_record_t *r, char *err, size_t cap)
{
    memset(r, 0, sizeof *r);
    r->area = -1;
    const int root = jp_parse(&P, json, len, err, cap);
    if (root < 0)
        return false;
    long v;
    if (!jint(jget(&P, root, "id"), 1, RDOF_ID_MAX, &v)) {
        seterr(err, cap, "id: 1 to 16777215");
        return false;
    }
    r->id = (uint32_t)v;
    const rdof_jnode_t *at = jget(&P, root, "at"), *exp = jget(&P, root, "exp");
    if (!at || at->type != RDOF_JSTR || !rdof_mn_iso_parse(at->str, &r->at_ms)) {
        seterr(err, cap, "at: a UTC time");
        return false;
    }
    if (!exp || exp->type != RDOF_JSTR || !rdof_mn_iso_parse(exp->str, &r->exp_ms)) {
        seterr(err, cap, "exp: a UTC time");
        return false;
    }
    const rdof_jnode_t *how = jget(&P, root, "how");
    if (how && how->type == RDOF_JSTR)
        r->implicit = !strcmp(how->str, "call");
    const int tg = rdof_jget(&P.doc, root, "tg");
    if (tg >= 0 && !jids(&P, tg, r->tg, RDOF_MN_REC_TG, &r->ntg, err, cap))
        return false;
    if (jint(jget(&P, root, "sub"), 1, RDOF_ID_MAX, &v))
        r->sub = (uint32_t)v;
    if (jint(jget(&P, root, "area"), 0, 1 << 16, &v))
        r->area = (int)v;
    return true;
}

/* ── where ───────────────────────────────────────────────────────────────── */

bool rdof_mn_where_parse(const char *json, size_t len, rdof_mn_where_t *w, char *err, size_t cap)
{
    memset(w, 0, sizeof *w);
    w->path = -1;
    const int root = jp_parse(&P, json, len, err, cap);
    if (root < 0)
        return false;
    const rdof_jnode_t *node = jget(&P, root, "node"), *at = jget(&P, root, "at");
    if (!node || node->type != RDOF_JSTR || !rdof_mn_node_ok(node->str)) {
        seterr(err, cap, "node: a node name");
        return false;
    }
    snprintf(w->node, sizeof w->node, "%s", node->str);
    if (!at || at->type != RDOF_JSTR || !rdof_mn_iso_parse(at->str, &w->at_ms)) {
        seterr(err, cap, "at: a UTC time");
        return false;
    }
    const int path = rdof_jget(&P.doc, root, "path");
    long v;
    if (path >= 0) {
        const rdof_jnode_t *pn = rdof_jnode(&P.doc, path);
        if (pn->type == RDOF_JOBJ && jint(jget(&P, path, "i"), 0, 254, &v))
            w->path = (int)v;
        else if (jint(pn, 0, 254, &v))
            w->path = (int)v;
    }
    const rdof_jnode_t *src = jget(&P, root, "source");
    w->reg = src && src->type == RDOF_JSTR && !strcmp(src->str, "reg");
    return true;
}

/* ── status, trunk info ──────────────────────────────────────────────────── */

size_t rdof_mn_status_json(const rdof_mn_status_t *s, char *out, size_t cap)
{
    rdof_json_t j;
    char t[32];
    rdof_json_init(&j, out, cap);
    rdof_json_open(&j, NULL);
    rdof_iso_time(s->wall, t);
    rdof_json_str(&j, "at", t);
    rdof_json_str(&j, "node", s->node);
    if (s->firmware)
        rdof_json_str(&j, "firmware", s->firmware);
    rdof_json_int(&j, "uptime_s", (long long)s->uptime_s);
    const rdof_mn_trunk_status_t *tr = s->trunk;
    if (tr) {
        rdof_json_open(&j, "trunk");
        rdof_json_bool(&j, "connected", tr->connected);
        rdof_json_int(&j, "regs", tr->regs);
        rdof_json_int(&j, "queue", tr->queue);
        /* channels: an array by hand */
        rdof_json_raw(&j, "channels", "[");
        for (int i = 0; i < tr->nch && !j.overflow; i++) {
            const rdof_mn_chan_status_t *c = &tr->ch[i];
            char item[256];
            rdof_json_t q;
            rdof_json_init(&q, item, sizeof item);
            rdof_json_open(&q, NULL);
            rdof_json_int(&q, "chan", c->chan);
            rdof_json_int(&q, "lcn", c->lcn);
            rdof_json_str(&q, "use", c->use ? c->use : "idle");
            if (c->src)
                rdof_json_int(&q, "src", c->src);
            if (c->dst) {
                rdof_json_int(&q, "dst", c->dst);
                rdof_json_bool(&q, "group", c->group);
            }
            if (c->since_ms) {
                iso(c->since_ms, t);
                rdof_json_str(&q, "since", t);
            }
            rdof_json_close(&q);
            if (!rdof_json_done(&q) || j.len + q.len + 2 >= j.cap) {
                j.overflow = true;
                break;
            }
            if (i)
                j.buf[j.len++] = ',';
            memcpy(j.buf + j.len, item, q.len);
            j.len += q.len;
            j.buf[j.len] = 0;
        }
        if (j.len + 2 >= j.cap)
            j.overflow = true;
        else {
            j.buf[j.len++] = ']';
            j.buf[j.len] = 0;
        }
        rdof_json_open(&j, "counters");
        rdof_json_int(&j, "aloha", tr->aloha);
        rdof_json_int(&j, "rand", tr->rand);
        rdof_json_int(&j, "collisions", tr->collisions);
        rdof_json_int(&j, "grants", tr->grants);
        rdof_json_int(&j, "refused", tr->refused);
        rdof_json_int(&j, "queued", tr->queued);
        rdof_json_close(&j);
        rdof_json_close(&j);
    }
    rdof_json_close(&j);
    return rdof_json_done(&j) ? j.len : 0;
}

static bool carriers_raw(rdof_json_t *j, const char *key, const rdof_mn_carrier_t *c, int n)
{
    char t[512];
    size_t k = 0;
    t[k++] = '[';
    for (int i = 0; i < n; i++) {
        int m = snprintf(t + k, sizeof t - k, "%s{\"chan\":%d,\"tx_hz\":%lu,\"rx_hz\":%lu", i ? "," : "", c[i].chan,
                         (unsigned long)c[i].tx_hz, (unsigned long)c[i].rx_hz);
        if (m <= 0 || (size_t)m >= sizeof t - k)
            return false;
        k += (size_t)m;
        m = c[i].cc >= 0 ? snprintf(t + k, sizeof t - k, ",\"cc\":%d}", c[i].cc) : snprintf(t + k, sizeof t - k, "}");
        if (m <= 0 || (size_t)m >= sizeof t - k)
            return false;
        k += (size_t)m;
    }
    if (k + 2 > sizeof t)
        return false;
    t[k++] = ']';
    t[k] = 0;
    rdof_json_raw(j, key, t);
    return true;
}

size_t rdof_mn_trunk_info_json(const rdof_mn_trunk_info_t *t, char *out, size_t cap)
{
    rdof_json_t j;
    rdof_json_init(&j, out, cap);
    rdof_json_open(&j, NULL);
    rdof_json_open(&j, "system");
    rdof_json_str(&j, "model", t->model ? t->model : "small");
    rdof_json_int(&j, "net", t->net);
    rdof_json_int(&j, "site", t->site);
    rdof_json_str(&j, "par", t->par ? t->par : "ab");
    rdof_json_int(&j, "area_bits", t->area_bits);
    rdof_json_close(&j);
    if (!carriers_raw(&j, "tscc", t->tscc, t->ntscc) || !carriers_raw(&j, "carriers", t->carriers, t->ncarriers))
        return 0;
    char s[256];
    size_t k = 0;
    s[k++] = '[';
    for (int i = 0; i < t->nservices && k + 24 < sizeof s; i++)
        k += (size_t)snprintf(s + k, sizeof s - k, "%s\"%s\"", i ? "," : "", t->services[i]);
    s[k++] = ']';
    s[k] = 0;
    rdof_json_raw(&j, "services", s);
    rdof_json_bool(&j, "reg", t->reg);
    rdof_json_close(&j);
    return rdof_json_done(&j) ? j.len : 0;
}

/* ── provisioning ────────────────────────────────────────────────────────── */

static const char *const MODELS[] = {"tiny", "small", "large", "huge"};
static const char *const PARS[] = {"", "a", "b", "ab"};
/* NET and SITE bits per model (361-4 table: the 14-bit system code less the 2-bit model) */
static const int NET_BITS[] = {9, 7, 4, 2};

void rdof_mn_cfg_init(rdof_mn_cfg_t *c, const char *node)
{
    memset(c, 0, sizeof *c);
    snprintf(c->node, sizeof c->node, "%s", node ? node : "");
}

size_t rdof_mn_cfg_topic(char *out, size_t cap, const char *root, const char *kind, const char *node, uint32_t id)
{
    if (!strcmp(kind, "net"))
        return fit(snprintf(out, cap, "%s/cfg/net", root), cap);
    if (!strcmp(kind, "node"))
        return fit(snprintf(out, cap, "%s/cfg/node/%s", root, node), cap);
    return fit(snprintf(out, cap, "%s/cfg/%s/%lu", root, kind, (unsigned long)id), cap);
}

static void timer_set(rdof_mn_site_cfg_t *s, const char *name, double v)
{
    for (int i = 0; i < s->ntimers; i++)
        if (!strcmp(s->timers[i].name, name)) {
            s->timers[i].value = v;
            return;
        }
    if (s->ntimers < RDOF_MN_CFG_TIMERS) {
        snprintf(s->timers[s->ntimers].name, sizeof s->timers[0].name, "%s", name);
        s->timers[s->ntimers++].value = v;
    }
}

static bool site_from(const jp_t *p, int root, rdof_mn_site_cfg_t *o, long *rev, char *note, size_t ncap, char *err,
                      size_t cap)
{
    memset(o, 0, sizeof *o);
    if (rev)
        *rev = 0;
    if (note && ncap)
        note[0] = 0;
    const rdof_jnode_t *r = rdof_jnode(&p->doc, root);
    char e[96];
    for (int k = r->child; k >= 0; k = rdof_jnode(&p->doc, k)->next) {
        const rdof_jnode_t *n = rdof_jnode(&p->doc, k);
        const char *key = n->key;
        long v;
        e[0] = 0;
        if (!strcmp(key, "rev")) {
            if (!jint(n, 1, 0x7FFFFFFF, &v)) {
                seterr(err, cap, "rev: a positive integer");
                return false;
            }
            if (rev)
                *rev = v;
        } else if (!strcmp(key, "at") || !strcmp(key, "sig")) {
            /* when, and room for a signature later */
        } else if (!strcmp(key, "system")) {
            if (n->type != RDOF_JOBJ) {
                seterr(err, cap, "system: an object");
                return false;
            }
            for (int q = n->child; q >= 0; q = rdof_jnode(&p->doc, q)->next) {
                const rdof_jnode_t *m = rdof_jnode(&p->doc, q);
                if (!strcmp(m->key, "model")) {
                    int i = 0;
                    while (i < 4 && !(m->type == RDOF_JSTR && !strcmp(m->str, MODELS[i])))
                        i++;
                    if (i == 4) {
                        seterr(err, cap, "system.model: tiny, small, large or huge");
                        return false;
                    }
                    o->model = i;
                    o->has |= RDOF_MN_HAS_MODEL;
                } else if (!strcmp(m->key, "net")) {
                    if (!jint(m, 0, 511, &v)) {
                        seterr(err, cap, "system.net: 0 to 511");
                        return false;
                    }
                    o->net = (int)v;
                    o->has |= RDOF_MN_HAS_NET;
                } else if (!strcmp(m->key, "site")) {
                    if (!jint(m, 0, 1023, &v)) {
                        seterr(err, cap, "system.site: 0 to 1023");
                        return false;
                    }
                    o->site = (int)v;
                    o->has |= RDOF_MN_HAS_SITE;
                } else if (!strcmp(m->key, "par")) {
                    int i = 1;
                    while (i < 4 && !(m->type == RDOF_JSTR && !strcmp(m->str, PARS[i])))
                        i++;
                    if (i == 4) {
                        seterr(err, cap, "system.par: a, b or ab");
                        return false;
                    }
                    o->par = i;
                    o->has |= RDOF_MN_HAS_PAR;
                } else if (!strcmp(m->key, "area_bits")) {
                    if (!jint(m, 0, 10, &v)) {
                        seterr(err, cap, "system.area_bits: 0 to 10");
                        return false;
                    }
                    o->area_bits = (int)v;
                    o->has |= RDOF_MN_HAS_AREA_BITS;
                } else
                    note_add(note, ncap, m->key);
            }
        } else if (!strcmp(key, "site")) {
            if (!jint(n, 0, 1023, &v)) {
                seterr(err, cap, "site: 0 to 1023");
                return false;
            }
            o->site = (int)v;
            o->has |= RDOF_MN_HAS_SITE;
        } else if (!strcmp(key, "area_bits")) {
            if (!jint(n, 0, 10, &v)) {
                seterr(err, cap, "area_bits: 0 to 10");
                return false;
            }
            o->area_bits = (int)v;
            o->has |= RDOF_MN_HAS_AREA_BITS;
        } else if (!strcmp(key, "registration")) {
            if (n->type != RDOF_JSTR || (strcmp(n->str, "open") && strcmp(n->str, "listed"))) {
                seterr(err, cap, "registration: open or listed");
                return false;
            }
            o->listed = !strcmp(n->str, "listed");
            o->has |= RDOF_MN_HAS_REGISTRATION;
        } else if (!strcmp(key, "refuse_unlisted")) {
            if (n->type != RDOF_JSTR || (strcmp(n->str, "denied") && strcmp(n->str, "refused"))) {
                seterr(err, cap, "refuse_unlisted: denied or refused");
                return false;
            }
            o->refused = !strcmp(n->str, "refused");
            o->has |= RDOF_MN_HAS_REFUSE;
        } else if (!strcmp(key, "require_registration") || !strcmp(key, "foacsu") || !strcmp(key, "enabled")) {
            if (n->type != RDOF_JBOOL) {
                snprintf(e, sizeof e, "%s: true or false", key);
                seterr(err, cap, e);
                return false;
            }
            if (key[0] == 'r') {
                o->require_registration = n->b;
                o->has |= RDOF_MN_HAS_REQUIRE_REG;
            } else if (key[0] == 'f') {
                o->foacsu = n->b;
                o->has |= RDOF_MN_HAS_FOACSU;
            } else {
                o->enabled = n->b;
                o->has |= RDOF_MN_HAS_ENABLED;
            }
        } else if (!strcmp(key, "ms") || !strcmp(key, "private") || !strcmp(key, "tg") || !strcmp(key, "wide_tg") ||
                   !strcmp(key, "static_tg")) {
            rdof_idset_t *s = !strcmp(key, "ms")        ? &o->ms
                              : !strcmp(key, "private") ? &o->private_
                              : !strcmp(key, "tg")      ? &o->tg
                              : !strcmp(key, "wide_tg") ? &o->wide_tg
                                                        : &o->static_tg;
            const uint32_t bit = !strcmp(key, "ms")        ? RDOF_MN_HAS_MS
                                 : !strcmp(key, "private") ? RDOF_MN_HAS_PRIVATE
                                 : !strcmp(key, "tg")      ? RDOF_MN_HAS_TG
                                 : !strcmp(key, "wide_tg") ? RDOF_MN_HAS_WIDE_TG
                                                           : RDOF_MN_HAS_STATIC_TG;
            char ie[64];
            if (!jidset(p, k, s, ie, sizeof ie)) {
                snprintf(e, sizeof e, "%s: %s", key, ie);
                seterr(err, cap, e);
                return false;
            }
            o->has |= bit;
        } else if (!strcmp(key, "neighbors")) {
            if (n->type != RDOF_JARR) {
                seterr(err, cap, "neighbors: an array of site numbers");
                return false;
            }
            o->nneighbors = 0;
            for (int i = 0; i < n->count; i++) {
                if (!jint(rdof_jnode(&p->doc, rdof_jat(&p->doc, k, i)), 0, 1023, &v)) {
                    seterr(err, cap, "neighbors: site numbers 0 to 1023");
                    return false;
                }
                if (o->nneighbors < RDOF_MN_CFG_NEIGHBORS)
                    o->neighbors[o->nneighbors++] = (int)v;
            }
            o->has |= RDOF_MN_HAS_NEIGHBORS;
        } else if (!strcmp(key, "timers")) {
            if (n->type != RDOF_JOBJ) {
                seterr(err, cap, "timers: an object");
                return false;
            }
            for (int q = n->child; q >= 0; q = rdof_jnode(&p->doc, q)->next) {
                const rdof_jnode_t *m = rdof_jnode(&p->doc, q);
                if (m->type != RDOF_JNUM || !(m->num >= 0) || strlen(m->key) >= sizeof o->timers[0].name) {
                    snprintf(e, sizeof e, "timers.%.40s: a number, 0 or more", m->key);
                    seterr(err, cap, e);
                    return false;
                }
                timer_set(o, m->key, m->num);
            }
        } else
            note_add(note, ncap, key);
    }
    if ((o->has & RDOF_MN_HAS_MODEL) && (o->has & RDOF_MN_HAS_NET) && o->net >= 1 << NET_BITS[o->model]) {
        seterr(err, cap, "system.net: too large for the model");
        return false;
    }
    if ((o->has & RDOF_MN_HAS_MODEL) && (o->has & RDOF_MN_HAS_SITE) && o->site >= 1 << (12 - NET_BITS[o->model])) {
        seterr(err, cap, "site: too large for the model");
        return false;
    }
    return true;
}

bool rdof_mn_site_cfg_parse(const char *json, size_t len, rdof_mn_site_cfg_t *out, long *rev, char *note, size_t ncap,
                            char *err, size_t cap)
{
    const int root = jp_parse(&P, json, len, err, cap);
    return root >= 0 && site_from(&P, root, out, rev, note, ncap, err, cap);
}

static bool tg_from(const jp_t *p, int root, rdof_mn_tg_cfg_t *o, char *note, size_t ncap, char *err, size_t cap)
{
    const uint32_t id = o->tg;
    memset(o, 0, sizeof *o);
    o->tg = id;
    rdof_idset_add(&o->sites, "*", NULL, 0);
    rdof_idset_finish(&o->sites);
    rdof_idset_add(&o->ms, "*", NULL, 0);
    rdof_idset_finish(&o->ms);
    const rdof_jnode_t *r = rdof_jnode(&p->doc, root);
    for (int k = r->child; k >= 0; k = rdof_jnode(&p->doc, k)->next) {
        const rdof_jnode_t *n = rdof_jnode(&p->doc, k);
        long v;
        char ie[64], e[96];
        if (!strcmp(n->key, "rev")) {
            if (!jint(n, 1, 0x7FFFFFFF, &v)) {
                seterr(err, cap, "rev: a positive integer");
                return false;
            }
            o->rev = v;
        } else if (!strcmp(n->key, "at") || !strcmp(n->key, "sig")) {
        } else if (!strcmp(n->key, "name")) {
            if (n->type != RDOF_JSTR) {
                seterr(err, cap, "name: a string");
                return false;
            }
            snprintf(o->name, sizeof o->name, "%s", n->str);
        } else if (!strcmp(n->key, "wide")) {
            if (n->type != RDOF_JBOOL) {
                seterr(err, cap, "wide: true or false");
                return false;
            }
            o->has_wide = true;
            o->wide = n->b;
        } else if (!strcmp(n->key, "sites") || !strcmp(n->key, "ms")) {
            if (!jidset(p, k, n->key[0] == 's' ? &o->sites : &o->ms, ie, sizeof ie)) {
                snprintf(e, sizeof e, "%s: %s", n->key, ie);
                seterr(err, cap, e);
                return false;
            }
        } else if (!strcmp(n->key, "priority")) {
            if (!jint(n, 0, 3, &v)) {
                seterr(err, cap, "priority: 0 to 3");
                return false;
            }
            o->priority = (int)v;
        } else if (!strcmp(n->key, "hang_s")) {
            if (n->type != RDOF_JNUM || !(n->num >= 1 && n->num <= 60)) {
                seterr(err, cap, "hang_s: 1 to 60");
                return false;
            }
            o->hang_s = n->num;
        } else
            note_add(note, ncap, n->key);
    }
    if (!o->rev) {
        seterr(err, cap, "rev: required");
        return false;
    }
    return true;
}

static bool ms_from(const jp_t *p, int root, rdof_mn_ms_cfg_t *o, char *note, size_t ncap, char *err, size_t cap)
{
    const uint32_t id = o->id;
    memset(o, 0, sizeof *o);
    o->id = id;
    o->enabled = o->private_ = o->wide_private = o->emergency = true;
    const rdof_jnode_t *r = rdof_jnode(&p->doc, root);
    for (int k = r->child; k >= 0; k = rdof_jnode(&p->doc, k)->next) {
        const rdof_jnode_t *n = rdof_jnode(&p->doc, k);
        long v;
        char e[96];
        if (!strcmp(n->key, "rev")) {
            if (!jint(n, 1, 0x7FFFFFFF, &v)) {
                seterr(err, cap, "rev: a positive integer");
                return false;
            }
            o->rev = v;
        } else if (!strcmp(n->key, "at") || !strcmp(n->key, "sig")) {
        } else if (!strcmp(n->key, "name")) {
            if (n->type != RDOF_JSTR) {
                seterr(err, cap, "name: a string");
                return false;
            }
            snprintf(o->name, sizeof o->name, "%s", n->str);
        } else if (!strcmp(n->key, "enabled") || !strcmp(n->key, "private") || !strcmp(n->key, "wide_private") ||
                   !strcmp(n->key, "emergency")) {
            if (n->type != RDOF_JBOOL) {
                snprintf(e, sizeof e, "%s: true or false", n->key);
                seterr(err, cap, e);
                return false;
            }
            bool *b = !strcmp(n->key, "enabled")   ? &o->enabled
                      : !strcmp(n->key, "private") ? &o->private_
                      : n->key[0] == 'w'           ? &o->wide_private
                                                   : &o->emergency;
            *b = n->b;
        } else if (!strcmp(n->key, "priority")) {
            if (!jint(n, 0, 3, &v)) {
                seterr(err, cap, "priority: 0 to 3");
                return false;
            }
            o->priority = (int)v;
        } else if (!strcmp(n->key, "tg")) {
            char ie[64];
            if (!jids(p, k, o->tg, RDOF_MN_REC_TG, &o->ntg, ie, sizeof ie)) {
                snprintf(e, sizeof e, "tg: %s", ie);
                seterr(err, cap, e);
                return false;
            }
        } else
            note_add(note, ncap, n->key);
    }
    if (!o->rev) {
        seterr(err, cap, "rev: required");
        return false;
    }
    return true;
}

rdof_mn_cfg_result_t rdof_mn_cfg_message(rdof_mn_cfg_t *c, const char *root, const char *topic, const char *payload,
                                         size_t len, char *note, size_t cap)
{
    if (note && cap)
        note[0] = 0;
    const char *r = under(topic, root);
    if (!r || strncmp(r, "cfg/", 4))
        return RDOF_MN_CFG_NOT_CFG;
    r += 4;
    int kind; /* 0 net, 1 node, 2 tg, 3 ms */
    uint32_t id = 0;
    if (!strcmp(r, "net"))
        kind = 0;
    else if (!strncmp(r, "node/", 5)) {
        if (strcmp(r + 5, c->node))
            return RDOF_MN_CFG_NOT_CFG;
        kind = 1;
    } else if (!strncmp(r, "tg/", 3) && id_of(r + 3, &id))
        kind = 2;
    else if (!strncmp(r, "ms/", 3) && id_of(r + 3, &id) && id)
        kind = 3;
    else
        return RDOF_MN_CFG_NOT_CFG;

    if (!len) { /* deleted (an empty retained message) */
        bool had = false;
        if (kind == 0) {
            had = c->net_rev != 0;
            c->net_rev = 0;
            memset(&c->net, 0, sizeof c->net);
        } else if (kind == 1) {
            had = c->node_rev != 0;
            c->node_rev = 0;
            memset(&c->nodecfg, 0, sizeof c->nodecfg);
        } else if (kind == 2) {
            for (int i = 0; i < c->ntg; i++)
                if (c->tg[i].tg == id) {
                    c->tg[i] = c->tg[--c->ntg];
                    had = true;
                    break;
                }
        } else {
            for (int i = 0; i < c->nms; i++)
                if (c->ms[i].id == id) {
                    c->ms[i] = c->ms[--c->nms];
                    had = true;
                    break;
                }
        }
        if (had)
            c->changes++;
        return had ? RDOF_MN_CFG_DELETED : RDOF_MN_CFG_OLD;
    }
    const int jr = jp_parse(&P, payload, len, note, cap);
    if (jr < 0)
        return RDOF_MN_CFG_BAD;
    if (kind <= 1) {
        rdof_mn_site_cfg_t s;
        long rev;
        char err[96];
        if (!site_from(&P, jr, &s, &rev, note, cap, err, sizeof err)) {
            if (note && cap)
                snprintf(note, cap, "%s", err);
            return RDOF_MN_CFG_BAD;
        }
        if (!rev) {
            if (note && cap)
                snprintf(note, cap, "rev: required");
            return RDOF_MN_CFG_BAD;
        }
        long *held = kind == 0 ? &c->net_rev : &c->node_rev;
        if (rev <= *held)
            return RDOF_MN_CFG_OLD;
        *held = rev;
        if (kind == 0)
            c->net = s;
        else
            c->nodecfg = s;
        c->changes++;
        return RDOF_MN_CFG_APPLIED;
    }
    char err[96];
    if (kind == 2) {
        rdof_mn_tg_cfg_t t = {.tg = id};
        if (!tg_from(&P, jr, &t, note, cap, err, sizeof err)) {
            if (note && cap)
                snprintf(note, cap, "%s", err);
            return RDOF_MN_CFG_BAD;
        }
        rdof_mn_tg_cfg_t *slot = (rdof_mn_tg_cfg_t *)rdof_mn_cfg_tg(c, id);
        if (slot && t.rev <= slot->rev)
            return RDOF_MN_CFG_OLD;
        if (!slot) {
            if (c->ntg >= RDOF_MN_CFG_TG)
                return RDOF_MN_CFG_FULL;
            slot = &c->tg[c->ntg++];
        }
        *slot = t;
    } else {
        rdof_mn_ms_cfg_t m = {.id = id};
        if (!ms_from(&P, jr, &m, note, cap, err, sizeof err)) {
            if (note && cap)
                snprintf(note, cap, "%s", err);
            return RDOF_MN_CFG_BAD;
        }
        rdof_mn_ms_cfg_t *slot = (rdof_mn_ms_cfg_t *)rdof_mn_cfg_ms(c, id);
        if (slot && m.rev <= slot->rev)
            return RDOF_MN_CFG_OLD;
        if (!slot) {
            if (c->nms >= RDOF_MN_CFG_MS)
                return RDOF_MN_CFG_FULL;
            slot = &c->ms[c->nms++];
        }
        *slot = m;
    }
    c->changes++;
    return RDOF_MN_CFG_APPLIED;
}

static void layer(rdof_mn_site_cfg_t *o, const rdof_mn_site_cfg_t *l)
{
    const uint32_t h = l->has;
    if (h & RDOF_MN_HAS_MODEL)
        o->model = l->model;
    if (h & RDOF_MN_HAS_NET)
        o->net = l->net;
    if (h & RDOF_MN_HAS_SITE)
        o->site = l->site;
    if (h & RDOF_MN_HAS_PAR)
        o->par = l->par;
    if (h & RDOF_MN_HAS_AREA_BITS)
        o->area_bits = l->area_bits;
    if (h & RDOF_MN_HAS_REGISTRATION)
        o->listed = l->listed;
    if (h & RDOF_MN_HAS_MS)
        o->ms = l->ms;
    if (h & RDOF_MN_HAS_REFUSE)
        o->refused = l->refused;
    if (h & RDOF_MN_HAS_PRIVATE)
        o->private_ = l->private_;
    if (h & RDOF_MN_HAS_TG)
        o->tg = l->tg;
    if (h & RDOF_MN_HAS_WIDE_TG)
        o->wide_tg = l->wide_tg;
    if (h & RDOF_MN_HAS_STATIC_TG)
        o->static_tg = l->static_tg;
    if (h & RDOF_MN_HAS_REQUIRE_REG)
        o->require_registration = l->require_registration;
    if (h & RDOF_MN_HAS_FOACSU)
        o->foacsu = l->foacsu;
    if (h & RDOF_MN_HAS_ENABLED)
        o->enabled = l->enabled;
    if (h & RDOF_MN_HAS_NEIGHBORS) {
        memcpy(o->neighbors, l->neighbors, sizeof o->neighbors);
        o->nneighbors = l->nneighbors;
    }
    o->has |= h;
    for (int i = 0; i < l->ntimers; i++)
        timer_set(o, l->timers[i].name, l->timers[i].value);
}

void rdof_mn_cfg_site(const rdof_mn_cfg_t *c, const rdof_mn_site_cfg_t *base, rdof_mn_site_cfg_t *out)
{
    memset(out, 0, sizeof *out);
    /* defaults under everything: open, registration required, enabled, every ID */
    out->model = RDOF_MN_MODEL_SMALL;
    out->par = RDOF_MN_PAR_AB;
    out->require_registration = out->enabled = true;
    rdof_idset_add(&out->ms, "*", NULL, 0);
    rdof_idset_finish(&out->ms);
    out->private_ = out->ms;
    out->tg = out->ms;
    if (base)
        layer(out, base);
    if (c && c->net_rev)
        layer(out, &c->net);
    if (c && c->node_rev)
        layer(out, &c->nodecfg);
}

double rdof_mn_cfg_timer(const rdof_mn_site_cfg_t *s, const char *name, double dflt)
{
    for (int i = 0; i < s->ntimers; i++)
        if (!strcmp(s->timers[i].name, name))
            return s->timers[i].value;
    return dflt;
}

const rdof_mn_tg_cfg_t *rdof_mn_cfg_tg(const rdof_mn_cfg_t *c, uint32_t tg)
{
    for (int i = 0; i < c->ntg; i++)
        if (c->tg[i].tg == tg)
            return &c->tg[i];
    return NULL;
}

const rdof_mn_ms_cfg_t *rdof_mn_cfg_ms(const rdof_mn_cfg_t *c, uint32_t id)
{
    for (int i = 0; i < c->nms; i++)
        if (c->ms[i].id == id)
            return &c->ms[i];
    return NULL;
}

/* ── the cache ───────────────────────────────────────────────────────────── */

static void idset_str(rdof_json_t *j, const char *key, const rdof_idset_t *s)
{
    char t[256];
    rdof_idset_format(s, t, sizeof t);
    rdof_json_str(j, key, t);
}

static size_t site_json(const rdof_mn_site_cfg_t *s, long rev, char *out, size_t cap)
{
    rdof_json_t j;
    rdof_json_init(&j, out, cap);
    rdof_json_open(&j, NULL);
    rdof_json_int(&j, "rev", rev);
    const uint32_t h = s->has;
    if (h & (RDOF_MN_HAS_MODEL | RDOF_MN_HAS_NET | RDOF_MN_HAS_PAR)) {
        rdof_json_open(&j, "system");
        if (h & RDOF_MN_HAS_MODEL)
            rdof_json_str(&j, "model", MODELS[s->model]);
        if (h & RDOF_MN_HAS_NET)
            rdof_json_int(&j, "net", s->net);
        if (h & RDOF_MN_HAS_PAR)
            rdof_json_str(&j, "par", PARS[s->par]);
        rdof_json_close(&j);
    }
    if (h & RDOF_MN_HAS_SITE)
        rdof_json_int(&j, "site", s->site);
    if (h & RDOF_MN_HAS_AREA_BITS)
        rdof_json_int(&j, "area_bits", s->area_bits);
    if (h & RDOF_MN_HAS_REGISTRATION)
        rdof_json_str(&j, "registration", s->listed ? "listed" : "open");
    if (h & RDOF_MN_HAS_MS)
        idset_str(&j, "ms", &s->ms);
    if (h & RDOF_MN_HAS_REFUSE)
        rdof_json_str(&j, "refuse_unlisted", s->refused ? "refused" : "denied");
    if (h & RDOF_MN_HAS_PRIVATE)
        idset_str(&j, "private", &s->private_);
    if (h & RDOF_MN_HAS_TG)
        idset_str(&j, "tg", &s->tg);
    if (h & RDOF_MN_HAS_WIDE_TG)
        idset_str(&j, "wide_tg", &s->wide_tg);
    if (h & RDOF_MN_HAS_STATIC_TG)
        idset_str(&j, "static_tg", &s->static_tg);
    if (h & RDOF_MN_HAS_REQUIRE_REG)
        rdof_json_bool(&j, "require_registration", s->require_registration);
    if (h & RDOF_MN_HAS_FOACSU)
        rdof_json_bool(&j, "foacsu", s->foacsu);
    if (h & RDOF_MN_HAS_ENABLED)
        rdof_json_bool(&j, "enabled", s->enabled);
    if (h & RDOF_MN_HAS_NEIGHBORS) {
        uint32_t n[RDOF_MN_CFG_NEIGHBORS];
        for (int i = 0; i < s->nneighbors; i++)
            n[i] = (uint32_t)s->neighbors[i];
        ids_raw(&j, "neighbors", n, s->nneighbors);
    }
    if (s->ntimers) {
        rdof_json_open(&j, "timers");
        for (int i = 0; i < s->ntimers; i++)
            rdof_json_float3(&j, s->timers[i].name, s->timers[i].value);
        rdof_json_close(&j);
    }
    rdof_json_close(&j);
    return rdof_json_done(&j) ? j.len : 0;
}

static size_t tg_json(const rdof_mn_tg_cfg_t *t, char *out, size_t cap)
{
    rdof_json_t j;
    rdof_json_init(&j, out, cap);
    rdof_json_open(&j, NULL);
    rdof_json_int(&j, "rev", t->rev);
    if (t->name[0])
        rdof_json_str(&j, "name", t->name);
    if (t->has_wide)
        rdof_json_bool(&j, "wide", t->wide);
    idset_str(&j, "sites", &t->sites);
    idset_str(&j, "ms", &t->ms);
    rdof_json_int(&j, "priority", t->priority);
    if (t->hang_s > 0)
        rdof_json_float3(&j, "hang_s", t->hang_s);
    rdof_json_close(&j);
    return rdof_json_done(&j) ? j.len : 0;
}

static size_t ms_json(const rdof_mn_ms_cfg_t *m, char *out, size_t cap)
{
    rdof_json_t j;
    rdof_json_init(&j, out, cap);
    rdof_json_open(&j, NULL);
    rdof_json_int(&j, "rev", m->rev);
    if (m->name[0])
        rdof_json_str(&j, "name", m->name);
    rdof_json_bool(&j, "enabled", m->enabled);
    rdof_json_bool(&j, "private", m->private_);
    rdof_json_bool(&j, "wide_private", m->wide_private);
    rdof_json_bool(&j, "emergency", m->emergency);
    rdof_json_int(&j, "priority", m->priority);
    ids_raw(&j, "tg", m->tg, m->ntg);
    rdof_json_close(&j);
    return rdof_json_done(&j) ? j.len : 0;
}

static void cache_out(void (*line)(void *, const char *, size_t), void *ctx, const char *topic, const char *doc,
                      size_t n)
{
    char t[2400];
    const int k = snprintf(t, sizeof t, "%s\t%.*s", topic, (int)n, doc);
    if (n && k > 0 && (size_t)k < sizeof t)
        line(ctx, t, (size_t)k);
}

void rdof_mn_cfg_cache(const rdof_mn_cfg_t *c, void (*line)(void *ctx, const char *text, size_t len), void *ctx)
{
    char doc[2048], topic[80];
    if (c->net_rev)
        cache_out(line, ctx, "cfg/net", doc, site_json(&c->net, c->net_rev, doc, sizeof doc));
    if (c->node_rev) {
        snprintf(topic, sizeof topic, "cfg/node/%s", c->node);
        cache_out(line, ctx, topic, doc, site_json(&c->nodecfg, c->node_rev, doc, sizeof doc));
    }
    for (int i = 0; i < c->ntg; i++) {
        snprintf(topic, sizeof topic, "cfg/tg/%lu", (unsigned long)c->tg[i].tg);
        cache_out(line, ctx, topic, doc, tg_json(&c->tg[i], doc, sizeof doc));
    }
    for (int i = 0; i < c->nms; i++) {
        snprintf(topic, sizeof topic, "cfg/ms/%lu", (unsigned long)c->ms[i].id);
        cache_out(line, ctx, topic, doc, ms_json(&c->ms[i], doc, sizeof doc));
    }
}

rdof_mn_cfg_result_t rdof_mn_cfg_cache_line(rdof_mn_cfg_t *c, const char *text, size_t len)
{
    const char *tab = memchr(text, '\t', len);
    if (!tab || tab == text || (size_t)(tab - text) > 70)
        return RDOF_MN_CFG_BAD;
    char topic[80];
    snprintf(topic, sizeof topic, "_/%.*s", (int)(tab - text), text);
    const size_t n = len - (size_t)(tab + 1 - text);
    if (!n)
        return RDOF_MN_CFG_BAD; /* the cache holds no deletions */
    return rdof_mn_cfg_message(c, "_", topic, tab + 1, n, NULL, 0);
}

/* ── commands ────────────────────────────────────────────────────────────── */

bool rdof_mn_cmd_parse(const char *json, size_t len, rdof_mn_cmd_t *c, char *err, size_t cap)
{
    static const struct {
        const char *name;
        rdof_mn_cmd_kind_t kind;
    } cmds[] = {{"deregister", RDOF_MN_CMD_DEREGISTER},   {"clear", RDOF_MN_CMD_CLEAR},
                {"withdraw", RDOF_MN_CMD_WITHDRAW},       {"restore", RDOF_MN_CMD_RESTORE},
                {"resend_records", RDOF_MN_CMD_RESEND_RECORDS}, {"status", RDOF_MN_CMD_STATUS}};
    memset(c, 0, sizeof *c);
    c->chan = c->lcn = -1;
    const int root = jp_parse(&P, json, len, err, cap);
    if (root < 0)
        return false;
    const rdof_jnode_t *id = jget(&P, root, "id"), *cmd = jget(&P, root, "cmd");
    if (id && id->type == RDOF_JSTR)
        snprintf(c->id, sizeof c->id, "%s", id->str);
    if (!c->id[0]) {
        seterr(err, cap, "id: required");
        return false;
    }
    if (!cmd || cmd->type != RDOF_JSTR) {
        seterr(err, cap, "cmd: required");
        return false;
    }
    snprintf(c->name, sizeof c->name, "%s", cmd->str);
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++)
        if (!strcmp(cmd->str, cmds[i].name))
            c->cmd = cmds[i].kind;
    long v;
    switch (c->cmd) {
    case RDOF_MN_CMD_UNKNOWN: seterr(err, cap, "unknown command"); return false;
    case RDOF_MN_CMD_DEREGISTER:
        if (!jint(jget(&P, root, "ms"), 1, RDOF_ID_MAX, &v)) {
            seterr(err, cap, "ms: 1 to 16777215");
            return false;
        }
        c->ms = (uint32_t)v;
        break;
    case RDOF_MN_CMD_CLEAR:
        if (!jint(jget(&P, root, "chan"), 1, 4094, &v)) {
            seterr(err, cap, "chan: 1 to 4094");
            return false;
        }
        c->chan = (int)v;
        if (!jint(jget(&P, root, "lcn"), 0, 1, &v)) {
            seterr(err, cap, "lcn: 0 or 1");
            return false;
        }
        c->lcn = (int)v;
        break;
    default: break;
    }
    return true;
}

size_t rdof_mn_cmd_result_json(const char *id, bool ok, const char *error, char *out, size_t cap)
{
    rdof_json_t j;
    rdof_json_init(&j, out, cap);
    rdof_json_open(&j, NULL);
    rdof_json_str(&j, "id", id ? id : "");
    rdof_json_bool(&j, "ok", ok);
    if (!ok)
        rdof_json_str(&j, "error", error ? error : "");
    rdof_json_close(&j);
    return rdof_json_done(&j) ? j.len : 0;
}
