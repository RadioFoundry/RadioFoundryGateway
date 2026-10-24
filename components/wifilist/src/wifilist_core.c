// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
// wifilist without the platform (see wifilist_core.h).
#include "wifilist_core.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

int wifilist_rank(const wifilist_net_t *nets, int n, const wifilist_ap_t *aps, int na, wifilist_cand_t *out, int max)
{
    int k = 0;
    int rssi[WIFILIST_MAX];
    for (int i = 0; i < n && i < WIFILIST_MAX && k < max; i++) {
        int best = -1;
        bool want_open = !nets[i].password[0];
        for (int j = 0; j < na; j++) {
            if (strcmp(aps[j].ssid, nets[i].ssid) || aps[j].open != want_open) {
                continue;
            }
            if (best < 0 || aps[j].rssi > aps[best].rssi) {
                best = j;
            }
        }
        if (best < 0 && !nets[i].hidden) {
            continue;
        }
        // Insert by priority, then signal (a hidden one not seen counts weakest), then list order.
        int r = best >= 0 ? aps[best].rssi : -128;
        int at = k;
        while (at > 0) {
            const wifilist_net_t *o = &nets[out[at - 1].net];
            if (o->priority > nets[i].priority || (o->priority == nets[i].priority && rssi[at - 1] >= r)) {
                break;
            }
            at--;
        }
        for (int m = k; m > at; m--) {
            out[m] = out[m - 1];
            rssi[m] = rssi[m - 1];
        }
        out[at] = (wifilist_cand_t){.net = i, .ap = best};
        rssi[at] = r;
        k++;
    }
    return k;
}

void wifilist_sort(wifilist_net_t *nets, int n)
{
    for (int i = 1; i < n; i++) {
        wifilist_net_t t = nets[i];
        int j = i;
        while (j > 0 && nets[j - 1].priority < t.priority) {
            nets[j] = nets[j - 1];
            j--;
        }
        nets[j] = t;
    }
}

static bool fail(char *err, size_t cap, const char *fmt, ...)
{
    if (err && cap) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, cap, fmt, ap);
        va_end(ap);
    }
    return false;
}

bool wifilist_check(const wifilist_net_t *nets, int n, char *err, size_t cap)
{
    if (n < 0 || n > WIFILIST_MAX) {
        return fail(err, cap, "at most %d networks", WIFILIST_MAX);
    }
    for (int i = 0; i < n; i++) {
        const wifilist_net_t *w = &nets[i];
        size_t sl = strnlen(w->ssid, sizeof(w->ssid)), pl = strnlen(w->password, sizeof(w->password));
        if (sl < 1 || sl > 32) {
            return fail(err, cap, "network %d: a name of 1 to 32 bytes", i + 1);
        }
        if (pl == 64) {
            for (size_t j = 0; j < 64; j++) {
                if (!isxdigit((unsigned char)w->password[j])) {
                    return fail(err, cap, "%s: a 64 character password must be hex (a PSK)", w->ssid);
                }
            }
        } else if (pl && (pl < 8 || pl > 63)) {
            return fail(err, cap, "%s: a password of 8 to 63 characters (or none: open)", w->ssid);
        }
        if (w->priority < WIFILIST_PRIORITY_MIN || w->priority > WIFILIST_PRIORITY_MAX) {
            return fail(err, cap, "%s: priority %d to %d", w->ssid, WIFILIST_PRIORITY_MIN, WIFILIST_PRIORITY_MAX);
        }
        for (int j = 0; j < i; j++) {
            if (!strcmp(nets[j].ssid, w->ssid)) {
                return fail(err, cap, "%s is in the list twice", w->ssid);
            }
        }
    }
    return true;
}

int wifilist_merge(const wifilist_net_t *old, int n_old, const wifilist_edit_t *in, int n_in, wifilist_net_t *out,
                   char *err, size_t cap)
{
    if (n_in > WIFILIST_MAX) {
        fail(err, cap, "at most %d networks", WIFILIST_MAX);
        return -1;
    }
    for (int i = 0; i < n_in; i++) {
        out[i] = in[i].net;
        if (in[i].has_password) {
            continue;
        }
        const char *name = in[i].was[0] ? in[i].was : in[i].net.ssid;
        out[i].password[0] = 0;
        for (int j = 0; j < n_old; j++) {
            if (!strcmp(old[j].ssid, name)) {
                memcpy(out[i].password, old[j].password, sizeof(out[i].password));
                break;
            }
        }
    }
    if (!wifilist_check(out, n_in, err, cap)) {
        return -1;
    }
    wifilist_sort(out, n_in);
    return n_in;
}

uint32_t wifilist_backoff_s(int failed_rounds, uint32_t max_s, bool ap_up)
{
    if (ap_up) {
        return max_s;
    }
    uint32_t s = 5;
    for (int i = 0; i < failed_rounds && s < max_s && s <= UINT32_MAX / 2; i++) {
        s *= 2;
    }
    return s < max_s ? s : max_s;
}

bool wifilist_ap_password_ok(const char *password)
{
    size_t n = password ? strlen(password) : 0;
    return n == 0 || (n >= 8 && n <= 63);
}

size_t wifilist_blob_size(int n)
{
    return 2 + (size_t)(n > 0 ? n : 0) * WIFILIST_BLOB_REC;
}

size_t wifilist_blob_encode(const wifilist_net_t *nets, int n, uint8_t *out)
{
    if (n < 0) {
        n = 0;
    }
    memset(out, 0, wifilist_blob_size(n));
    out[0] = WIFILIST_BLOB_VERSION;
    out[1] = (uint8_t)n;
    for (int i = 0; i < n; i++) {
        uint8_t *r = out + 2 + (size_t)i * WIFILIST_BLOB_REC;
        memcpy(r, nets[i].ssid, sizeof(nets[i].ssid));
        memcpy(r + 33, nets[i].password, sizeof(nets[i].password));
        r[98] = (uint8_t)nets[i].priority;
        r[99] = nets[i].hidden ? 1 : 0;
    }
    return wifilist_blob_size(n);
}

int wifilist_blob_decode(const uint8_t *blob, size_t len, wifilist_net_t *out, int max)
{
    if (!blob || len < 2 || blob[0] != WIFILIST_BLOB_VERSION) {
        return -1;
    }
    int n = blob[1];
    if (n > WIFILIST_MAX || len < wifilist_blob_size(n)) {
        return -1;
    }
    if (n > max) {
        n = max;
    }
    for (int i = 0; i < n; i++) {
        const uint8_t *r = blob + 2 + (size_t)i * WIFILIST_BLOB_REC;
        wifilist_net_t *w = &out[i];
        memcpy(w->ssid, r, sizeof(w->ssid));
        memcpy(w->password, r + 33, sizeof(w->password));
        w->ssid[32] = w->password[64] = 0;
        w->priority = (int8_t)r[98];
        w->hidden = r[99] & 1;
    }
    return n;
}

const char *wifilist_reason(uint8_t r)
{
    switch (r) {
    case 0: return "timed out";
    case 1: return "unspecified";
    case 2: return "authentication expired";
    case 3: return "deauthenticated (AP leaving)";
    case 4: return "disassociated (inactive)";
    case 5: return "AP full";
    case 6: return "not authenticated";
    case 7: return "not associated";
    case 8: return "disassociated (leaving)";
    case 14: return "MIC failure";
    case 15: return "4-way handshake timeout (wrong password?)";
    case 16: return "group key timeout";
    case 23: return "802.1X authentication failed";
    case 200: return "beacon timeout";
    case 201: return "no AP found";
    case 202: return "authentication failed";
    case 203: return "association failed";
    case 204: return "handshake timeout (wrong password?)";
    case 205: return "connection failed";
    case 206: return "AP TSF reset";
    case 207: return "roaming";
    case 208: return "association comeback too long";
    case 209: return "SA query timeout";
    case 210: return "no AP with compatible security";
    case 211: return "no AP within the auth threshold";
    case 212: return "no AP within the RSSI threshold";
    default: return "other";
    }
}

// ── JSON ──────────────────────────────────────────────────────────────────────

typedef struct {
    char *buf;
    size_t cap, len;
    bool over, first;
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

static void open_(jw_t *w, const char *k, char c)
{
    if (k) {
        key(w, k);
    } else if (!w->first) {
        put(w, ",");
    }
    put(w, "%c", c);
    w->first = true;
}

static void close_(jw_t *w, char c)
{
    put(w, "%c", c);
    w->first = false;
}

// Length of the well-formed UTF-8 sequence at p (1 to 4), or 0 (RFC 3629: no overlong forms,
// surrogates or code points past U+10FFFF).
static int utf8_len(const unsigned char *p)
{
    if (p[0] < 0x80) {
        return 1;
    }
    int n = p[0] >= 0xC2 && p[0] <= 0xDF ? 2 : (p[0] >= 0xE0 && p[0] <= 0xEF) ? 3 : (p[0] >= 0xF0 && p[0] <= 0xF4) ? 4 : 0;
    for (int i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            return 0;
        }
    }
    if ((p[0] == 0xE0 && p[1] < 0xA0) || (p[0] == 0xED && p[1] > 0x9F) || (p[0] == 0xF0 && p[1] < 0x90) ||
        (p[0] == 0xF4 && p[1] > 0x8F)) {
        return 0;
    }
    return n;
}

static void str(jw_t *w, const char *k, const char *v)
{
    key(w, k);
    put(w, "\"");
    for (const unsigned char *p = (const unsigned char *)v; *p;) {
        if (*p == '"' || *p == '\\') {
            put(w, "\\%c", *p++);
        } else if (*p < 0x20) {
            put(w, "\\u%04x", *p++);
        } else {
            int n = utf8_len(p);
            if (!n) {  // an SSID is any 32 bytes: a byte that isn't UTF-8 becomes U+FFFD
                put(w, "\\ufffd");
                p++;
                continue;
            }
            for (int i = 0; i < n; i++) {
                put(w, "%c", *p++);
            }
        }
    }
    put(w, "\"");
}

static void num(jw_t *w, const char *k, long long v)
{
    key(w, k);
    put(w, "%lld", v);
}

static void boolean(jw_t *w, const char *k, bool v)
{
    key(w, k);
    put(w, v ? "true" : "false");
}

static void mac(jw_t *w, const char *k, const uint8_t *b)
{
    char t[18];
    snprintf(t, sizeof(t), "%02x:%02x:%02x:%02x:%02x:%02x", b[0], b[1], b[2], b[3], b[4], b[5]);
    str(w, k, t);
}

static size_t finish(jw_t *w)
{
    if (w->over) {
        w->buf[0] = 0;
        return 0;
    }
    return w->len;
}

size_t wifilist_status_json(const wifilist_status_t *s, char *buf, size_t cap)
{
    jw_t w = {.buf = buf, .cap = cap, .first = true};
    if (!cap) {
        return 0;
    }
    open_(&w, NULL, '{');
    str(&w, "state", s->state ? s->state : "");
    boolean(&w, "connected", s->connected);
    str(&w, "ssid", s->connected ? s->ssid : "");
    if (s->connected) {
        mac(&w, "bssid", s->bssid);
        num(&w, "rssi", s->rssi);
        num(&w, "channel", s->channel);
        num(&w, "connected_s", s->connected_s);
    }
    str(&w, "ip", s->ip);
    boolean(&w, "ap_up", s->ap_up);
    str(&w, "ap_ssid", s->ap_ssid);
    num(&w, "scans", s->scans);
    num(&w, "connects", s->connects);
    num(&w, "uptime_s", s->uptime_s);
    open_(&w, "networks", '[');
    for (int i = 0; i < s->n_nets; i++) {
        open_(&w, NULL, '{');
        str(&w, "ssid", s->nets[i].ssid);
        num(&w, "priority", s->nets[i].priority);
        boolean(&w, "hidden", s->nets[i].hidden);
        boolean(&w, "password_set", s->nets[i].password_set);
        if (s->nets[i].rssi != -128) {
            num(&w, "rssi", s->nets[i].rssi);
        }
        close_(&w, '}');
    }
    close_(&w, ']');
    open_(&w, "seen", '[');
    for (int i = 0; i < s->n_seen; i++) {
        open_(&w, NULL, '{');
        str(&w, "ssid", s->seen[i].ssid);
        mac(&w, "bssid", s->seen[i].bssid);
        num(&w, "rssi", s->seen[i].rssi);
        num(&w, "channel", s->seen[i].channel);
        boolean(&w, "open", s->seen[i].open);
        close_(&w, '}');
    }
    close_(&w, ']');
    open_(&w, "fails", '[');
    for (int i = 0; i < s->n_fail; i++) {
        const wifilist_fail_t *f = &s->fails[i];
        open_(&w, NULL, '{');
        num(&w, "t_s", f->t_s);
        str(&w, "ssid", f->ssid);
        num(&w, "reason", f->reason);
        str(&w, "why", wifilist_reason(f->reason));
        num(&w, "rssi", f->rssi);
        boolean(&w, "lost", f->lost);
        close_(&w, '}');
    }
    close_(&w, ']');
    close_(&w, '}');
    return finish(&w);
}

size_t wifilist_networks_json(const wifilist_net_t *nets, int n, char *buf, size_t cap)
{
    jw_t w = {.buf = buf, .cap = cap, .first = true};
    if (!cap) {
        return 0;
    }
    open_(&w, NULL, '{');
    open_(&w, "networks", '[');
    for (int i = 0; i < n; i++) {
        open_(&w, NULL, '{');
        str(&w, "ssid", nets[i].ssid);
        num(&w, "priority", nets[i].priority);
        if (nets[i].hidden) {
            boolean(&w, "hidden", true);
        }
        close_(&w, '}');
    }
    close_(&w, ']');
    close_(&w, '}');
    return finish(&w);
}
