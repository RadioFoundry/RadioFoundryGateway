// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The web API's shared part (rdof/webapi.h) with fake platforms: /api/status's layout (the firmware's, byte for
 * byte, and a host's), the capability flags, each route, the settings POST's refusals and its save and restart,
 * the log ring, last heard's query. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/lastheard.h"
#include "rdof/webapi.h"

static int fails, checks;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(c)) {                                                                                                    \
            fails++;                                                                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                        \
        }                                                                                                              \
    } while (0)

/* ── a board, as the firmware fills it ── */

static struct {
    char stored[256];
    int saves, restarts, allocs, frees;
    uint32_t since;
    bool cfg_ok;
} F;

static void b_board(void *ctx, rdof_webapi_board_t *b)
{
    b->name = "w12";
    b->version = "v1-3-gabc";
    b->idf = "v5.5";
    b->uptime_s = 42;
    b->reset = "power on";
}
static bool b_memory(void *ctx, rdof_webapi_mem_t *m)
{
    m->has_heap = true;
    m->heap_free = 10240;
    m->psram_free = 2000000;
    return true;
}
static bool b_clock(void *ctx, rdof_webapi_clock_t *c)
{
    c->source = "ntp";
    return true;
}
static bool b_ok(void *ctx, const char **e)
{
    *e = F.cfg_ok ? "" : "radios: none";
    return F.cfg_ok;
}
static void b_status(void *ctx, rdof_json_t *j, rdof_webapi_at_t at)
{
    switch (at) {
    case RDOF_WEBAPI_AT_BOARD: rdof_json_bool(j, "safe_mode", false); break;
    case RDOF_WEBAPI_AT_MEMORY: rdof_json_raw(j, "cpu", "[0.5,0.25]"); break;
    case RDOF_WEBAPI_AT_NETWORK:
        rdof_json_bool(j, "wifi", true);
        rdof_json_bool(j, "ap_mode", false);
        rdof_json_str(j, "ip", "10.0.0.2");
        break;
    case RDOF_WEBAPI_AT_RADIOS: rdof_json_bool(j, "build_sdr", true); break;
    }
}
static void *b_alloc(size_t n);
static char *b_get(void *ctx)
{
    char *t = b_alloc(strlen(F.stored) + 1); /* freed with free_fn */
    strcpy(t, F.stored);
    return t;
}
/* the firmware's rule here: an object with "name" */
static bool b_save(void *ctx, char *text, size_t len, char *err, size_t cap)
{
    if (text[len] != 0 || !strstr(text, "\"name\"")) {
        snprintf(err, cap, "name: missing");
        return false;
    }
    snprintf(F.stored, sizeof F.stored, "%s", text);
    F.saves++;
    return true;
}
static size_t b_resolved(void *ctx, char *out, size_t cap)
{
    memset(out, 'r', cap); /* more than fits: the answer is cut to cap - 1 */
    return cap + 100;
}
static size_t b_log(void *ctx, char *out, size_t cap)
{
    return (size_t)snprintf(out, cap, "I (1) boot\n");
}
static size_t b_heard(void *ctx, uint32_t since, char *out, size_t cap)
{
    F.since = since;
    return (size_t)snprintf(out, cap, "{\"seq\":%u}", (unsigned)since);
}
static void b_restart(void *ctx) { F.restarts++; }
static void *b_alloc(size_t n)
{
    F.allocs++;
    return malloc(n);
}
static void b_free(void *p)
{
    F.frees++;
    free(p);
}

static const rdof_webapi_platform_t BOARD = {
    .caps = RDOF_WEBAPI_CAP_RESTART | RDOF_WEBAPI_CAP_WIFI | RDOF_WEBAPI_CAP_COREDUMP | RDOF_WEBAPI_CAP_BOARD_MEM |
            RDOF_WEBAPI_CAP_NETCLOCK | RDOF_WEBAPI_CAP_LINKS | RDOF_WEBAPI_CAP_SETTINGS |
            RDOF_WEBAPI_CAP_SETTINGS_EDIT | RDOF_WEBAPI_CAP_OTA,
    .board = b_board,
    .memory = b_memory,
    .clock = b_clock,
    .status_json = b_status,
    .settings_ok = b_ok,
    .settings_get = b_get,
    .settings_save = b_save,
    .settings_resolved = b_resolved,
    .log_cap = 4097,
    .log_read = b_log,
    .lastheard = b_heard,
    .restart = b_restart,
    .alloc_fn = b_alloc,
    .free_fn = b_free,
};

static void t_status_board(void)
{
    static char buf[2048];
    F.cfg_ok = true;
    size_t n = rdof_webapi_status(&BOARD, buf, sizeof buf);
    /* the firmware's order, as it was before the shared code, then caps */
    const char *want =
        "{\"name\":\"w12\",\"version\":\"v1-3-gabc\",\"idf\":\"v5.5\",\"uptime_s\":42,\"reset\":\"power on\","
        "\"safe_mode\":false,\"heap_free\":10240,\"psram_free\":2000000,\"cpu\":[0.5,0.25],\"settings_ok\":true,"
        "\"settings_error\":\"\",\"wifi\":true,\"ap_mode\":false,\"ip\":\"10.0.0.2\",\"clock\":\"ntp\","
        "\"build_sdr\":true,\"caps\":{\"ota\":true,\"restart\":true,\"wifi\":true,\"tasks\":false,"
        "\"coredump\":true,\"board_mem\":true,\"sys_mem\":false,\"netclock\":true,\"os_clock\":false,"
        "\"links\":true,\"settings\":true,\"settings_edit\":true}}";
    CHECK(n == strlen(want) && !strcmp(buf, want));
    if (strcmp(buf, want))
        printf("  got  %s\n  want %s\n", buf, want);
    F.cfg_ok = false;
    rdof_webapi_status(&BOARD, buf, sizeof buf);
    CHECK(strstr(buf, "\"settings_ok\":false,\"settings_error\":\"radios: none\""));
    /* too small a buffer: cut, NUL terminated, no overrun */
    char small[40];
    n = rdof_webapi_status(&BOARD, small, sizeof small);
    CHECK(n < sizeof small && small[n] == 0);
}

/* ── a host: process and system memory, the OS's clock, read-only settings ── */

static void h_board(void *ctx, rdof_webapi_board_t *b)
{
    b->name = "rdofradio";
    b->version = "host";
    b->idf = NULL;
    b->uptime_s = 7;
    b->reset = "restart";
}
static bool h_memory(void *ctx, rdof_webapi_mem_t *m)
{
    m->has_proc = m->has_sys = true;
    m->rss = 3 << 20;
    m->rss_peak = 4 << 20;
    m->sys_total = 1LL << 34;
    m->sys_avail = 1LL << 33;
    return true;
}
static bool h_clock(void *ctx, rdof_webapi_clock_t *c)
{
    c->source = "os";
    c->has_os = true;
    c->os_source = "system clock (OS NTP)";
    c->known = true;
    c->synced = true;
    c->est_error_us = 120;
    c->max_error_us = 5000;
    c->offset_us = -1.25;
    c->freq_ppm = 3.5;
    return true;
}

static void t_status_host(void)
{
    static char buf[2048];
    rdof_webapi_platform_t p = {.caps = RDOF_WEBAPI_CAP_SYS_MEM | RDOF_WEBAPI_CAP_OS_CLOCK | RDOF_WEBAPI_CAP_SETTINGS,
                                .board = h_board,
                                .memory = h_memory,
                                .clock = h_clock,
                                .settings_get = b_get,
                                .settings_source = "command line flags"};
    rdof_webapi_status(&p, buf, sizeof buf);
    const char *want =
        "{\"name\":\"rdofradio\",\"version\":\"host\",\"uptime_s\":7,\"reset\":\"restart\",\"rss\":3145728,"
        "\"rss_peak\":4194304,\"mem_total\":17179869184,\"mem_avail\":8589934592,\"settings_ok\":true,"
        "\"settings_error\":\"\",\"clock\":\"os\",\"clock_os\":{\"source\":\"system clock (OS NTP)\",\"known\":true,"
        "\"synced\":true,\"est_error_us\":120,\"max_error_us\":5000,\"offset_us\":-1.25,\"freq_ppm\":3.5},"
        "\"settings_source\":\"command line flags\",\"caps\":{\"ota\":false,\"restart\":false,\"wifi\":false,"
        "\"tasks\":false,\"coredump\":false,\"board_mem\":false,\"sys_mem\":true,\"netclock\":false,"
        "\"os_clock\":true,\"links\":false,\"settings\":true,\"settings_edit\":false}}";
    CHECK(!strcmp(buf, want));
    if (strcmp(buf, want))
        printf("  got  %s\n  want %s\n", buf, want);
    /* nothing but the board: no memory, no clock keys */
    rdof_webapi_platform_t bare = {.board = h_board};
    rdof_webapi_status(&bare, buf, sizeof buf);
    CHECK(!strstr(buf, "rss") && !strstr(buf, "\"clock\"") && !strstr(buf, "heap_free") &&
          strstr(buf, "\"settings_ok\":true"));
}

/* ── the routes ── */

static rdof_webapi_resp_t R;

static bool req(const rdof_webapi_platform_t *p, const char *m, const char *path, const char *q, const char *body)
{
    static char b[RDOF_WEBAPI_MAX_CONFIG + 2];
    size_t len = body ? strlen(body) : 0;
    if (body)
        memcpy(b, body, len + 1);
    return rdof_webapi_handle(p, m, path, q, body && rdof_webapi_body_ok(path, len) ? b : NULL, len, &R);
}

static void t_routes(void)
{
    memset(&F, 0, sizeof F);
    F.cfg_ok = true;
    snprintf(F.stored, sizeof F.stored, "{\"name\":\"old\"}");

    CHECK(req(&BOARD, "GET", "/api/status", NULL, NULL) && R.status == 200 && R.owned &&
          !strcmp(R.type, "application/json") && R.len == strlen(R.body) && strstr(R.body, "\"caps\":{"));
    rdof_webapi_done(&BOARD, &R);
    CHECK(F.allocs == 1 && F.frees == 1);

    CHECK(req(&BOARD, "GET", "/api/config", NULL, NULL) && R.status == 200 && !strcmp(R.body, "{\"name\":\"old\"}") &&
          !R.filename[0]);
    rdof_webapi_done(&BOARD, &R);
    CHECK(req(&BOARD, "GET", "/api/config", "download=1", NULL) && !strcmp(R.filename, "w12.json"));
    rdof_webapi_done(&BOARD, &R);
    CHECK(req(&BOARD, "GET", "/api/config", "x=1&download", NULL) && !strcmp(R.filename, "w12.json"));
    rdof_webapi_done(&BOARD, &R);

    /* the settings POST: empty and too long refused before the platform sees them; a bad one with its reason */
    CHECK(req(&BOARD, "POST", "/api/config", NULL, "") && R.status == 400 &&
          !strcmp(R.body, "settings: 1 to 16384 bytes of JSON") && !R.restart);
    static char big[RDOF_WEBAPI_MAX_CONFIG + 2];
    memset(big, ' ', sizeof big - 1);
    big[sizeof big - 1] = 0;
    CHECK(!rdof_webapi_body_ok("/api/config", sizeof big - 1) && rdof_webapi_body_ok("/api/config", sizeof big - 2));
    CHECK(req(&BOARD, "POST", "/api/config", NULL, big) && R.status == 400 && F.saves == 0);
    CHECK(req(&BOARD, "POST", "/api/config", NULL, "{\"radios\": 7}") && R.status == 400 &&
          !strcmp(R.body, "name: missing") && !R.restart && F.saves == 0);
    CHECK(req(&BOARD, "POST", "/api/config", NULL, "{\"name\":\"new\"}") && R.status == 200 &&
          !strcmp(R.body, "saved; restarting") && R.restart && F.saves == 1 && F.restarts == 0);
    rdof_webapi_done(&BOARD, &R);
    rdof_webapi_restart(&BOARD);
    CHECK(F.restarts == 1 && !strcmp(F.stored, "{\"name\":\"new\"}"));

    /* read only: refused with where they come from, nothing stored */
    rdof_webapi_platform_t ro = BOARD;
    ro.caps &= ~(uint32_t)RDOF_WEBAPI_CAP_SETTINGS_EDIT;
    ro.settings_source = "command line flags";
    CHECK(req(&ro, "POST", "/api/config", NULL, "{\"name\":\"x\"}") && R.status == 403 &&
          strstr(R.body, "read only here: command line flags") && F.saves == 1);

    CHECK(req(&BOARD, "GET", "/api/resolved", NULL, NULL) && R.status == 200 && R.len == 5119 &&
          !strcmp(R.type, "text/plain") && R.body[5119] == 0);
    rdof_webapi_done(&BOARD, &R);
    CHECK(req(&BOARD, "GET", "/api/log", NULL, NULL) && R.status == 200 && !strcmp(R.body, "I (1) boot\n"));
    rdof_webapi_done(&BOARD, &R);
    CHECK(req(&BOARD, "GET", "/api/lastheard", "since=12", NULL) && F.since == 12 && !strcmp(R.body, "{\"seq\":12}"));
    rdof_webapi_done(&BOARD, &R);
    CHECK(req(&BOARD, "GET", "/api/lastheard", NULL, NULL) && F.since == 0);
    rdof_webapi_done(&BOARD, &R);
    CHECK(req(&BOARD, "POST", "/api/restart", NULL, NULL) && R.status == 200 && !strcmp(R.body, "restarting") &&
          R.restart);
    CHECK(F.allocs == F.frees);

    /* not routes, or not this platform's: untouched */
    CHECK(!req(&BOARD, "GET", "/api/clock", NULL, NULL));
    CHECK(!req(&BOARD, "POST", "/api/status", NULL, NULL));
    CHECK(!req(&BOARD, "DELETE", "/api/config", NULL, NULL));
    rdof_webapi_platform_t none = {.board = h_board};
    CHECK(req(&none, "GET", "/api/status", NULL, NULL));
    rdof_webapi_done(&none, &R);
    CHECK(!req(&none, "GET", "/api/config", NULL, NULL) && !req(&none, "POST", "/api/config", NULL, "{}") &&
          !req(&none, "GET", "/api/resolved", NULL, NULL) && !req(&none, "GET", "/api/log", NULL, NULL) &&
          !req(&none, "GET", "/api/lastheard", NULL, NULL) && !req(&none, "POST", "/api/restart", NULL, NULL));

    /* the route table: each one answered by the board */
    int k = 0;
    for (const rdof_webapi_route_t *w = rdof_webapi_routes; w->path; w++, k++) {
        const bool post = !strcmp(w->method, "POST");
        CHECK(req(&BOARD, w->method, w->path, NULL, post ? "{\"name\":\"r\"}" : NULL));
        rdof_webapi_done(&BOARD, &R);
    }
    CHECK(k == 7);
}

static void t_query(void)
{
    char v[8];
    CHECK(rdof_webapi_query("since=5", "since", v, sizeof v) && !strcmp(v, "5"));
    CHECK(rdof_webapi_query("a=1&since=77&b", "since", v, sizeof v) && !strcmp(v, "77"));
    CHECK(rdof_webapi_query("a=1&b", "b", v, sizeof v) && !strcmp(v, ""));
    CHECK(!rdof_webapi_query("sinced=5", "since", v, sizeof v) && !rdof_webapi_query(NULL, "since", v, sizeof v));
    CHECK(rdof_webapi_query("since=123456789", "since", v, sizeof v) && !strcmp(v, "1234567"));
}

static void t_ring(void)
{
    char buf[8], out[16];
    rdof_bring_t r;
    rdof_bring_init(&r, buf, sizeof buf);
    CHECK(rdof_bring_read(&r, out, sizeof out) == 0 && out[0] == 0);
    rdof_bring_put(&r, "abc", 3);
    CHECK(rdof_bring_read(&r, out, sizeof out) == 3 && !strcmp(out, "abc"));
    CHECK(rdof_bring_read(&r, out, 3) == 2 && !strcmp(out, "bc")); /* the newest that fit */
    rdof_bring_put(&r, "defghij", 7);                                /* 10 bytes in 8: the oldest two go */
    CHECK(rdof_bring_read(&r, out, sizeof out) == 8 && !strcmp(out, "cdefghij"));
    CHECK(rdof_bring_read(&r, out, 4) == 3 && !strcmp(out, "hij"));
    rdof_bring_put(&r, "0123456789ABCDEFGHIJ", 20);
    CHECK(rdof_bring_read(&r, out, sizeof out) == 8 && !strcmp(out, "CDEFGHIJ"));
    CHECK(rdof_bring_read(&r, out, 0) == 0);
    rdof_bring_init(&r, NULL, 100); /* no buffer: nothing kept */
    rdof_bring_put(&r, "x", 1);
    CHECK(rdof_bring_read(&r, out, sizeof out) == 0 && out[0] == 0);
}

/* last heard behind the platform's callback, as both platforms do it */
static rdof_lh_t LH;
static rdof_lh_entry_t LHE[RDOF_LH_DEFAULT];
static size_t lh_json(void *ctx, uint32_t since, char *out, size_t cap)
{
    return rdof_lh_json(&LH, since, 100.0, 1700000000000LL, out, cap);
}

static void t_lastheard(void)
{
    rdof_lh_init(&LH, LHE, RDOF_LH_DEFAULT);
    rdof_webapi_platform_t p = {.board = h_board, .lastheard = lh_json};
    CHECK(req(&p, "GET", "/api/lastheard", NULL, NULL) && strstr(R.body, "\"calls\":[]"));
    rdof_webapi_done(&p, &R);
    rdof_tx_call_info_t c;
    memset(&c, 0, sizeof c);
    c.src = 9800001, c.dst = 9, c.group = true, c.slot = 2, c.cc = 1, c.frames = -1;
    rdof_lh_tx(&LH, "repeater", "example", "start", &c, 0, 99.0, 1700000000000LL);
    CHECK(req(&p, "GET", "/api/lastheard", "since=0", NULL) && strstr(R.body, "\"src\":9800001") &&
          strstr(R.body, "\"active\":true"));
    const char *seq = strstr(R.body, "\"seq\":");
    char q[32];
    snprintf(q, sizeof q, "since=%ld", seq ? strtol(seq + 6, NULL, 10) : -1L);
    rdof_webapi_done(&p, &R);
    CHECK(req(&p, "GET", "/api/lastheard", q, NULL) && !strstr(R.body, "9800001")); /* nothing new */
    rdof_webapi_done(&p, &R);
}

/* ── the board's clock on a host (netclock's routes, which the board's own server answers there) ── */

static int64_t clock_us;
static bool clk_status(void *ctx, rdof_webapi_clock_t *c)
{
    c->source = "ntp";
    return true;
}
static size_t clk_json(void *ctx, char *out, size_t cap) { return (size_t)snprintf(out, cap, "{\"source\":\"ntp\"}"); }
static bool clk_set(void *ctx, int64_t utc_us, const char **source)
{
    clock_us = utc_us;
    *source = "browser";
    return true;
}
static size_t clk_settings(void *ctx, char *out, size_t cap)
{
    return (size_t)snprintf(out, cap, "{\"servers\":\"pool.ntp.org\",\"poll_s\":0}");
}

static void t_clock(void)
{
    rdof_webapi_platform_t p = BOARD;
    CHECK(!req(&p, "GET", "/api/clock", NULL, NULL)); /* the board's server answers these itself */
    p.caps |= RDOF_WEBAPI_CAP_NETCLOCK;
    p.clock = clk_status;
    p.clock_json = clk_json;
    p.clock_set = clk_set;
    p.clock_settings_json = clk_settings;
    CHECK(req(&p, "GET", "/api/clock", NULL, NULL) && R.status == 200 && !strcmp(R.body, "{\"source\":\"ntp\"}"));
    rdof_webapi_done(&p, &R);
    CHECK(req(&p, "POST", "/api/time", NULL, "{\"utc_ms\": 1793206505123}") && R.status == 200 &&
          !strcmp(R.body, "{\"applied\":true,\"source\":\"browser\"}") && clock_us == 1793206505123000LL);
    rdof_webapi_done(&p, &R);
    clock_us = 0;
    CHECK(req(&p, "POST", "/api/time", NULL, "{\"utc_ms\": 5}") && !strcmp(R.body, "{\"applied\":false,\"source\":\"ntp\"}") &&
          !clock_us); /* before 2020: not a clock */
    rdof_webapi_done(&p, &R);
    CHECK(req(&p, "GET", "/api/clock/settings", NULL, NULL) && strstr(R.body, "pool.ntp.org"));
    rdof_webapi_done(&p, &R);
    CHECK(req(&p, "POST", "/api/clock/settings", NULL, "{}") && R.status == 403);
    rdof_webapi_done(&p, &R);
}

int main(void)
{
    t_status_board();
    t_clock();
    t_status_host();
    t_routes();
    t_query();
    t_ring();
    t_lastheard();
    printf("webapi: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
