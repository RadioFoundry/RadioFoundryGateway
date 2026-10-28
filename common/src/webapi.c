// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The web page's API, shared by the firmware and the host tools (rdof/webapi.h). */
#include "rdof/webapi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const rdof_webapi_route_t rdof_webapi_routes[] = {
    {"GET", "/api/status"},   {"GET", "/api/config"},    {"POST", "/api/config"}, {"GET", "/api/resolved"},
    {"GET", "/api/log"},      {"GET", "/api/lastheard"}, {"POST", "/api/restart"}, {NULL, NULL},
};

static const struct {
    uint32_t cap;
    const char *name;
} CAPS[] = {
    {RDOF_WEBAPI_CAP_OTA, "ota"},
    {RDOF_WEBAPI_CAP_RESTART, "restart"},
    {RDOF_WEBAPI_CAP_WIFI, "wifi"},
    {RDOF_WEBAPI_CAP_TASKS, "tasks"},
    {RDOF_WEBAPI_CAP_COREDUMP, "coredump"},
    {RDOF_WEBAPI_CAP_BOARD_MEM, "board_mem"},
    {RDOF_WEBAPI_CAP_SYS_MEM, "sys_mem"},
    {RDOF_WEBAPI_CAP_NETCLOCK, "netclock"},
    {RDOF_WEBAPI_CAP_OS_CLOCK, "os_clock"},
    {RDOF_WEBAPI_CAP_LINKS, "links"},
    {RDOF_WEBAPI_CAP_SETTINGS, "settings"},
    {RDOF_WEBAPI_CAP_SETTINGS_EDIT, "settings_edit"},
};

static void *alloc(const rdof_webapi_platform_t *p, size_t n)
{
    return p->alloc_fn ? p->alloc_fn(n) : malloc(n);
}

static void release(const rdof_webapi_platform_t *p, void *b)
{
    if (p->free_fn)
        p->free_fn(b);
    else
        free(b);
}

static void at(const rdof_webapi_platform_t *p, rdof_json_t *j, rdof_webapi_at_t where)
{
    if (p->status_json)
        p->status_json(p->ctx, j, where);
}

size_t rdof_webapi_status(const rdof_webapi_platform_t *p, char *buf, size_t cap)
{
    rdof_json_t j;
    rdof_json_init(&j, buf, cap);
    rdof_json_open(&j, NULL);
    rdof_webapi_board_t b = {"", "", NULL, 0, ""};
    if (p->board)
        p->board(p->ctx, &b);
    rdof_json_str(&j, "name", b.name ? b.name : "");
    rdof_json_str(&j, "version", b.version ? b.version : "");
    if (b.idf)
        rdof_json_str(&j, "idf", b.idf);
    rdof_json_int(&j, "uptime_s", b.uptime_s);
    rdof_json_str(&j, "reset", b.reset ? b.reset : "");
    at(p, &j, RDOF_WEBAPI_AT_BOARD);

    rdof_webapi_mem_t m;
    memset(&m, 0, sizeof m);
    if (p->memory && p->memory(p->ctx, &m)) {
        if (m.has_heap) {
            rdof_json_int(&j, "heap_free", m.heap_free);
            rdof_json_int(&j, "psram_free", m.psram_free);
        }
        if (m.has_proc) {
            rdof_json_int(&j, "rss", m.rss);
            rdof_json_int(&j, "rss_peak", m.rss_peak);
        }
        if (m.has_sys) {
            rdof_json_int(&j, "mem_total", m.sys_total);
            rdof_json_int(&j, "mem_avail", m.sys_avail);
        }
    }
    at(p, &j, RDOF_WEBAPI_AT_MEMORY);

    const char *error = "";
    const bool ok = !p->settings_ok || p->settings_ok(p->ctx, &error);
    rdof_json_bool(&j, "settings_ok", ok);
    rdof_json_str(&j, "settings_error", error ? error : "");
    at(p, &j, RDOF_WEBAPI_AT_NETWORK);

    rdof_webapi_clock_t c;
    memset(&c, 0, sizeof c);
    if (p->clock && p->clock(p->ctx, &c)) {
        rdof_json_str(&j, "clock", c.source ? c.source : "none");
        if (c.has_os) {
            rdof_json_open(&j, "clock_os");
            rdof_json_str(&j, "source", c.os_source ? c.os_source : "system clock");
            rdof_json_bool(&j, "known", c.known);
            if (c.known) {
                rdof_json_bool(&j, "synced", c.synced);
                rdof_json_int(&j, "est_error_us", c.est_error_us);
                rdof_json_int(&j, "max_error_us", c.max_error_us);
                rdof_json_float3(&j, "offset_us", c.offset_us);
                rdof_json_float3(&j, "freq_ppm", c.freq_ppm);
            }
            rdof_json_close(&j);
        }
    }
    at(p, &j, RDOF_WEBAPI_AT_RADIOS);

    if (p->settings_source)
        rdof_json_str(&j, "settings_source", p->settings_source);
    rdof_json_open(&j, "caps");
    for (size_t i = 0; i < sizeof CAPS / sizeof CAPS[0]; i++)
        rdof_json_bool(&j, CAPS[i].name, (p->caps & CAPS[i].cap) != 0);
    rdof_json_close(&j);
    rdof_json_close(&j);
    return j.len;
}

bool rdof_webapi_query(const char *query, const char *key, char *out, size_t cap)
{
    if (!query || !cap)
        return false;
    const size_t kl = strlen(key);
    for (const char *s = query; *s;) {
        const char *amp = strchr(s, '&'), *end = amp ? amp : s + strlen(s);
        if ((size_t)(end - s) >= kl && !strncmp(s, key, kl) && (s[kl] == '=' || s + kl == end)) {
            const char *v = s[kl] == '=' ? s + kl + 1 : end;
            size_t n = (size_t)(end - v);
            if (n > cap - 1)
                n = cap - 1;
            memcpy(out, v, n);
            out[n] = 0;
            return true;
        }
        s = amp ? amp + 1 : end;
    }
    return false;
}

static void say(rdof_webapi_resp_t *r, int status, const char *text)
{
    r->status = status;
    r->type = "text/plain";
    snprintf(r->msg, sizeof r->msg, "%s", text);
    r->body = r->msg;
    r->len = strlen(r->msg);
}

/* An allocated answer of up to cap bytes; false (and a 500) when there's no memory. */
static bool take(const rdof_webapi_platform_t *p, rdof_webapi_resp_t *r, size_t cap, const char *type)
{
    r->body = alloc(p, cap);
    if (!r->body) {
        say(r, 500, "no memory");
        return false;
    }
    r->body[0] = 0;
    r->owned = true;
    r->status = 200;
    r->type = type;
    return true;
}

bool rdof_webapi_body_ok(const char *path, size_t len)
{
    return !strcmp(path, "/api/config") && len > 0 && len <= RDOF_WEBAPI_MAX_CONFIG;
}

bool rdof_webapi_handle(const rdof_webapi_platform_t *p, const char *method, const char *path, const char *query,
                        char *body, size_t len, rdof_webapi_resp_t *r)
{
    const bool get = !strcmp(method, "GET") || !strcmp(method, "HEAD"), post = !strcmp(method, "POST");
    const bool settings = (p->caps & RDOF_WEBAPI_CAP_SETTINGS) != 0;
    const bool clock = (p->caps & RDOF_WEBAPI_CAP_NETCLOCK) && p->clock_json;
    enum { NONE, STATUS, CONFIG, SAVE, RESOLVED, LOG, HEARD, RESTART, CLOCK, TIME, CLOCK_SET, CLOCK_SAVE } route = NONE;
    if (get && !strcmp(path, "/api/status"))
        route = STATUS;
    else if (get && !strcmp(path, "/api/config") && settings && p->settings_get)
        route = CONFIG;
    else if (post && !strcmp(path, "/api/config") && settings && p->settings_save)
        route = SAVE;
    else if (get && !strcmp(path, "/api/resolved") && settings && p->settings_resolved)
        route = RESOLVED;
    else if (get && !strcmp(path, "/api/log") && p->log_read)
        route = LOG;
    else if (get && !strcmp(path, "/api/lastheard") && p->lastheard)
        route = HEARD;
    else if (post && !strcmp(path, "/api/restart") && (p->caps & RDOF_WEBAPI_CAP_RESTART) && p->restart)
        route = RESTART;
    else if (get && !strcmp(path, "/api/clock") && clock)
        route = CLOCK;
    else if (post && !strcmp(path, "/api/time") && clock && p->clock_set)
        route = TIME;
    else if (get && !strcmp(path, "/api/clock/settings") && clock && p->clock_settings_json)
        route = CLOCK_SET;
    else if (post && !strcmp(path, "/api/clock/settings") && clock)
        route = CLOCK_SAVE;
    if (route == NONE)
        return false;

    memset(r, 0, sizeof *r);
    switch (route) {
    case STATUS: {
        const size_t cap = p->status_cap ? p->status_cap : 8192;
        if (take(p, r, cap, "application/json"))
            r->len = rdof_webapi_status(p, r->body, cap);
        break;
    }
    case CONFIG: {
        r->body = p->settings_get(p->ctx);
        if (!r->body) {
            say(r, 500, "no memory");
            break;
        }
        r->owned = true;
        r->status = 200;
        r->type = "application/json";
        r->len = strlen(r->body);
        char v[8];
        if (rdof_webapi_query(query, "download", v, sizeof v)) {
            rdof_webapi_board_t b = {"", "", NULL, 0, ""};
            if (p->board)
                p->board(p->ctx, &b);
            snprintf(r->filename, sizeof r->filename, "%s.json", b.name && *b.name ? b.name : "settings");
        }
        break;
    }
    case SAVE: {
        if (!(p->caps & RDOF_WEBAPI_CAP_SETTINGS_EDIT)) {
            char t[sizeof r->msg];
            snprintf(t, sizeof t, "the settings are read only here%s%s", p->settings_source ? ": " : "",
                     p->settings_source ? p->settings_source : "");
            say(r, 403, t);
            break;
        }
        if (!body || len == 0 || len > RDOF_WEBAPI_MAX_CONFIG) {
            say(r, 400, "settings: 1 to 16384 bytes of JSON");
            break;
        }
        char error[160] = "";
        if (!p->settings_save(p->ctx, body, len, error, sizeof error)) {
            say(r, 400, error[0] ? error : "refused");
            break;
        }
        say(r, 200, "saved; restarting");
        r->restart = p->restart != NULL;
        break;
    }
    case RESOLVED: {
        const size_t cap = 5120;
        if (take(p, r, cap, "text/plain")) {
            r->len = p->settings_resolved(p->ctx, r->body, cap);
            if (r->len >= cap)
                r->len = cap - 1;
            r->body[r->len] = 0;
        }
        break;
    }
    case LOG: {
        const size_t cap = p->log_cap > 64 ? p->log_cap : 64;
        if (take(p, r, cap, "text/plain"))
            r->len = p->log_read(p->ctx, r->body, cap);
        break;
    }
    case HEARD: {
        char v[16];
        const uint32_t since = rdof_webapi_query(query, "since", v, sizeof v) ? (uint32_t)strtoul(v, NULL, 10) : 0;
        const size_t cap = 20480; /* 50 calls at about 380 bytes at most */
        if (take(p, r, cap, "application/json"))
            r->len = p->lastheard(p->ctx, since, r->body, cap);
        break;
    }
    case RESTART:
        say(r, 200, "restarting");
        r->restart = true;
        break;
    case CLOCK: {
        const size_t cap = 12288; /* as the board's */
        if (take(p, r, cap, "application/json"))
            r->len = p->clock_json(p->ctx, r->body, cap);
        if (r->owned && !r->len)
            say(r, 500, "too big");
        break;
    }
    case TIME: {
        /* {"utc_ms": the browser's Date.now() at send}: the page adds half the quickest round trip */
        const char *k = body && len < 96 ? strstr(body, "\"utc_ms\"") : NULL;
        const char *c = k ? strchr(k, ':') : NULL;
        const double ms = c ? strtod(c + 1, NULL) : 0;
        const char *source = "none";
        const bool applied = ms > 1.6e12 && p->clock_set(p->ctx, (int64_t)(ms * 1000.0), &source); /* after 2020 */
        if (take(p, r, 64, "application/json"))
            r->len = (size_t)snprintf(r->body, 64, "{\"applied\":%s,\"source\":\"%s\"}", applied ? "true" : "false",
                                      source ? source : "none");
        break;
    }
    case CLOCK_SET:
        if (take(p, r, 512, "application/json"))
            r->len = p->clock_settings_json(p->ctx, r->body, 512);
        break;
    case CLOCK_SAVE:
        say(r, 403, "the clock's servers are in the settings file here");
        break;
    default:
        break;
    }
    return true;
}

void rdof_webapi_done(const rdof_webapi_platform_t *p, rdof_webapi_resp_t *r)
{
    if (r->owned && r->body)
        release(p, r->body);
    r->body = NULL;
    r->owned = false;
}

void rdof_webapi_restart(const rdof_webapi_platform_t *p)
{
    if (p->restart)
        p->restart(p->ctx);
}

/* ── the byte ring ─────────────────────────────────────────────────────────── */

void rdof_bring_init(rdof_bring_t *r, char *buf, size_t size)
{
    r->buf = buf;
    r->size = buf ? size : 0;
    r->head = 0;
    r->wrapped = false;
}

void rdof_bring_put(rdof_bring_t *r, const char *data, size_t n)
{
    if (!r->size)
        return;
    for (size_t i = 0; i < n; i++) {
        r->buf[r->head++] = data[i];
        if (r->head == r->size) {
            r->head = 0;
            r->wrapped = true;
        }
    }
}

size_t rdof_bring_read(const rdof_bring_t *r, char *out, size_t cap)
{
    size_t n = 0;
    if (cap == 0)
        return 0;
    if (!r->size) {
        out[0] = 0;
        return 0;
    }
    size_t total = r->wrapped ? r->size : r->head;
    size_t start = r->wrapped ? r->head : 0;
    if (total > cap - 1) {
        start = (start + total - (cap - 1)) % r->size;
        total = cap - 1;
    }
    for (; n < total; n++)
        out[n] = r->buf[(start + n) % r->size];
    out[n] = 0;
    return n;
}
