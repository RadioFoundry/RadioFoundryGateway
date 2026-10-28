// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The web page's API, shared by the firmware (esp_http_server, esp_idf/main/web.c) and the host tools (hostweb/).
 * The HTTP servers stay with their platforms; for the routes in rdof_webapi_routes they hand the request to
 * rdof_webapi_handle and send what it answers. The platform gives the data and the actions through a
 * rdof_webapi_platform_t: each part is optional, and what it has shows in /api/status as "caps", which the page
 * keys off. So one page serves the ESP32, Linux and macOS.
 *
 *   GET  /api/status      the board, then the platform's own fields, then "caps" (docs/spec/fw/interfaces-http.md)
 *   GET  /api/config      the settings text (?download: as a file)
 *   POST /api/config      new settings: checked and stored, then a restart
 *   GET  /api/resolved    the settings as resolved (text)
 *   GET  /api/log         the log (text)
 *   GET  /api/lastheard   ?since=N: the recent calls (JSON)
 *   POST /api/restart
 *
 * Plain C: no threads, no I/O, no locks (the platform's callbacks lock what they share). Also here: the byte ring
 * the log is kept in.
 */
#ifndef RDOF_WEBAPI_H
#define RDOF_WEBAPI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/json.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_WEBAPI_MAX_CONFIG 16384 /* the settings document, bytes */

/* What a platform has. /api/status carries each as "caps": {name: bool}; the page shows a part only when it's set. */
enum {
    RDOF_WEBAPI_CAP_OTA = 1 << 0,           /* "ota": a firmware upload */
    RDOF_WEBAPI_CAP_RESTART = 1 << 1,       /* "restart": POST /api/restart */
    RDOF_WEBAPI_CAP_WIFI = 1 << 2,          /* "wifi": Wi-Fi networks and the setup access point */
    RDOF_WEBAPI_CAP_TASKS = 1 << 3,         /* "tasks": GET /api/tasks (FreeRTOS) */
    RDOF_WEBAPI_CAP_COREDUMP = 1 << 4,      /* "coredump": a saved core dump and the crash card */
    RDOF_WEBAPI_CAP_BOARD_MEM = 1 << 5,     /* "board_mem": internal and PSRAM free, the memory plan */
    RDOF_WEBAPI_CAP_SYS_MEM = 1 << 6,       /* "sys_mem": the process's and the system's memory */
    RDOF_WEBAPI_CAP_NETCLOCK = 1 << 7,      /* "netclock": the board's own clock servo (/api/clock, /netclock.js) */
    RDOF_WEBAPI_CAP_OS_CLOCK = 1 << 8,      /* "os_clock": the OS keeps the time ("clock_os" in status) */
    RDOF_WEBAPI_CAP_LINKS = 1 << 9,         /* "links": HBlink4 and MQTT */
    RDOF_WEBAPI_CAP_SETTINGS = 1 << 10,     /* "settings": GET /api/config and /api/resolved */
    RDOF_WEBAPI_CAP_SETTINGS_EDIT = 1 << 11, /* "settings_edit": POST /api/config */
    RDOF_WEBAPI_CAP_SPECTRUM = 1 << 12,     /* "spectrum": GET /api/spectrum (a board's SDR shows it by its card) */
    RDOF_WEBAPI_CAP_CRASH = 1 << 13,        /* "crash": a host tool's last exit ("last_exit"; GET / DELETE /api/crash) */
};

/* The board, first in /api/status. */
typedef struct {
    const char *name, *version;
    const char *idf; /* the ESP-IDF version; NULL: left out */
    long long uptime_s;
    const char *reset; /* why it last started */
} rdof_webapi_board_t;

/* Memory, as numbers; each group only when its has_ is set. */
typedef struct {
    bool has_heap; /* a board: "heap_free" (internal), "psram_free" */
    long long heap_free, psram_free;
    bool has_proc; /* a process: "rss", "rss_peak" */
    long long rss, rss_peak;
    bool has_sys; /* the system: "mem_total", "mem_avail" */
    long long sys_total, sys_avail;
} rdof_webapi_mem_t;

/* The clock: "clock" is the source's name (pps, ntp, browser, none; os for the OS's clock); has_os adds
 * "clock_os" {source, known, and when known synced, est_error_us, max_error_us, offset_us, freq_ppm}. */
typedef struct {
    const char *source;
    bool has_os;
    const char *os_source; /* "system clock (OS NTP)" */
    bool known, synced;
    long long est_error_us, max_error_us;
    double offset_us, freq_ppm;
} rdof_webapi_clock_t;

/* Where in /api/status the platform's own fields go (status_json). */
typedef enum {
    RDOF_WEBAPI_AT_BOARD,   /* after "reset" */
    RDOF_WEBAPI_AT_MEMORY,  /* after the memory numbers */
    RDOF_WEBAPI_AT_NETWORK, /* after "settings_error" */
    RDOF_WEBAPI_AT_RADIOS,  /* after "clock": the radios and the rest */
} rdof_webapi_at_t;

typedef struct {
    uint32_t caps; /* RDOF_WEBAPI_CAP_* */
    void *ctx;     /* every callback's first argument */

    void (*board)(void *ctx, rdof_webapi_board_t *out);       /* required */
    bool (*memory)(void *ctx, rdof_webapi_mem_t *out);        /* NULL or false: none */
    bool (*clock)(void *ctx, rdof_webapi_clock_t *out);       /* NULL or false: none */
    void (*status_json)(void *ctx, rdof_json_t *j, rdof_webapi_at_t at); /* NULL: none */
    size_t status_cap; /* the /api/status buffer; 0: 8192 */

    /* settings (RDOF_WEBAPI_CAP_SETTINGS) */
    bool (*settings_ok)(void *ctx, const char **error); /* NULL: ok; error: the reason ("" when ok) */
    char *(*settings_get)(void *ctx);                   /* the text (freed with free_fn), NULL: no memory */
    /* Check and store (RDOF_WEBAPI_CAP_SETTINGS_EDIT). False: err says why, nothing was stored. text is NUL
     * terminated and may be changed. */
    bool (*settings_save)(void *ctx, char *text, size_t len, char *err, size_t err_cap);
    size_t (*settings_resolved)(void *ctx, char *out, size_t cap); /* NULL: no /api/resolved */
    const char *settings_source; /* where the settings live, for the page ("file /x.json"); NULL: left out */

    /* The board's clock servo (RDOF_WEBAPI_CAP_NETCLOCK) for a server that doesn't answer its routes itself (the
     * board's netclock_http does; these aren't in rdof_webapi_routes): GET /api/clock (netclock_status_json), POST
     * /api/time {"utc_ms"} (the browser's clock), GET /api/clock/settings; a POST there is refused (the settings
     * file has them). */
    size_t (*clock_json)(void *ctx, char *out, size_t cap);
    bool (*clock_set)(void *ctx, int64_t utc_us, const char **source); /* source: the clock's after it */
    size_t (*clock_settings_json)(void *ctx, char *out, size_t cap);

    /* the log and last heard */
    size_t log_cap; /* the most /api/log sends, NUL included */
    size_t (*log_read)(void *ctx, char *out, size_t cap); /* NULL: no /api/log */
    size_t (*lastheard)(void *ctx, uint32_t since, char *out, size_t cap); /* NULL: no /api/lastheard */

    /* Restart (RDOF_WEBAPI_CAP_RESTART, and after settings are saved): start it and return at once; the answer
     * goes out first. */
    void (*restart)(void *ctx);

    /* the answers' buffers; NULL: malloc and free */
    void *(*alloc_fn)(size_t n);
    void (*free_fn)(void *p);
} rdof_webapi_platform_t;

/* The routes rdof_webapi_handle answers, for a server that registers each (the last one's path is NULL). */
typedef struct {
    const char *method, *path;
} rdof_webapi_route_t;
extern const rdof_webapi_route_t rdof_webapi_routes[];

typedef struct {
    int status;         /* 200, 400, 403, 404, 500 */
    const char *type;   /* Content-Type */
    char *body;         /* NUL terminated; send len bytes */
    size_t len;
    bool owned;         /* body is the platform's allocation: rdof_webapi_done frees it */
    bool restart;       /* after sending, call rdof_webapi_restart */
    char filename[64];  /* a download: Content-Disposition attachment with this name; "" none */
    char msg[192];      /* a short answer's text (body points here) */
} rdof_webapi_resp_t;

/* Answer a request. path without the query; query without the '?' (NULL none). A POST's body is NUL terminated
 * (body[len] == 0) and may be changed; for a length the route can't take (rdof_webapi_body_ok false) the server
 * passes NULL and the length, and the answer is the refusal. False: not a route this platform has (the server
 * answers as it would for any other path); r is untouched then. */
bool rdof_webapi_handle(const rdof_webapi_platform_t *p, const char *method, const char *path, const char *query,
                        char *body, size_t len, rdof_webapi_resp_t *r);
/* Whether a server should read a body of len bytes for this route (else it passes NULL). */
bool rdof_webapi_body_ok(const char *path, size_t len);
/* Free what the answer holds. */
void rdof_webapi_done(const rdof_webapi_platform_t *p, rdof_webapi_resp_t *r);
void rdof_webapi_restart(const rdof_webapi_platform_t *p);

/* /api/status into buf; returns the length (cut short if it didn't fit: check j's overflow with a larger cap). */
size_t rdof_webapi_status(const rdof_webapi_platform_t *p, char *buf, size_t cap);
/* A query's value for key into out (cut to fit); false if it's not there. */
bool rdof_webapi_query(const char *query, const char *key, char *out, size_t cap);

/* A byte ring (the log): put appends, dropping the oldest; read copies the newest bytes that fit in cap - 1 and
 * NUL terminates. The caller locks. */
typedef struct {
    char *buf;
    size_t size, head; /* head: where the next byte goes */
    bool wrapped;
} rdof_bring_t;

void rdof_bring_init(rdof_bring_t *r, char *buf, size_t size);
void rdof_bring_put(rdof_bring_t *r, const char *data, size_t n);
size_t rdof_bring_read(const rdof_bring_t *r, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
