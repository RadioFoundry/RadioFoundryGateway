// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
// netclock's HTTP handlers and its page section (see netclock_http.h).
#include "netclock_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "netclock.h"
#include "sdkconfig.h"

#define BASE CONFIG_NETCLOCK_HTTP_BASE

extern const uint8_t netclock_js_start[] asm("_binary_netclock_js_gz_start");
extern const uint8_t netclock_js_end[] asm("_binary_netclock_js_gz_end");

static void *big_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

static esp_err_t send_json(httpd_req_t *req, const char *buf, size_t len)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, (ssize_t)len);
}

// The body, NUL terminated, in buf (up to cap - 1 bytes). Returns its length or -1.
static int read_body(httpd_req_t *req, char *buf, size_t cap)
{
    if (req->content_len >= cap) {
        return -1;
    }
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, buf + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            return -1;
        }
        got += (size_t)n;
    }
    buf[got] = 0;
    return (int)got;
}

static esp_err_t get_clock(httpd_req_t *req)
{
    const size_t cap = 12288;
    netclock_status_t *st = big_alloc(sizeof(*st));
    char *buf = big_alloc(cap);
    if (!st || !buf) {
        free(st);
        free(buf);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }
    netclock_status(st);
    size_t n = netclock_status_json(st, buf, cap);
    esp_err_t e = n ? send_json(req, buf, n) : httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "too big");
    free(st);
    free(buf);
    return e;
}

// {"utc_ms": <the browser's Date.now() at send>}. The page sends its clock
// plus half the quickest of a few round trips, so it lands near our receive time.
static esp_err_t post_time(httpd_req_t *req)
{
    char body[96];
    if (read_body(req, body, sizeof(body)) < 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "{\"utc_ms\": ms}");
    }
    cJSON *j = cJSON_Parse(body);
    const cJSON *ms = cJSON_GetObjectItemCaseSensitive(j, "utc_ms");
    bool applied = false;
    if (cJSON_IsNumber(ms) && ms->valuedouble > 1.6e12) {  // sanity: after 2020
        applied = netclock_set_from_client((int64_t)(ms->valuedouble * 1000.0));
    }
    cJSON_Delete(j);
    char out[64];
    int n = snprintf(out, sizeof(out), "{\"applied\":%s,\"source\":\"%s\"}", applied ? "true" : "false",
                     netclock_source_name(netclock_source()));
    return send_json(req, out, (size_t)n);
}

static esp_err_t get_settings(httpd_req_t *req)
{
    char buf[NETCLOCK_SERVERS_LEN + 64];
    size_t n = netclock_settings_json(buf, sizeof(buf));
    return send_json(req, buf, n);
}

static esp_err_t post_settings(httpd_req_t *req)
{
    char body[NETCLOCK_SERVERS_LEN + 128], err[96];
    int n = read_body(req, body, sizeof(body));
    if (n < 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too long");
    }
    if (netclock_settings_from_json(body, (size_t)n, err, sizeof(err)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, err);
    }
    return get_settings(req);
}

static esp_err_t get_js(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/javascript");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)netclock_js_start, netclock_js_end - netclock_js_start);
}

esp_err_t netclock_http_register(httpd_handle_t server)
{
    static const httpd_uri_t uris[] = {
        {.uri = BASE "/clock", .method = HTTP_GET, .handler = get_clock},
        {.uri = BASE "/time", .method = HTTP_POST, .handler = post_time},
        {.uri = BASE "/clock/settings", .method = HTTP_GET, .handler = get_settings},
        {.uri = BASE "/clock/settings", .method = HTTP_POST, .handler = post_settings},
        {.uri = "/netclock.js", .method = HTTP_GET, .handler = get_js},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t e = httpd_register_uri_handler(server, &uris[i]);
        if (e != ESP_OK) {
            return e;
        }
    }
    return ESP_OK;
}
