// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
// wifilist's HTTP handlers and its page section (see wifilist_http.h).
#include "wifilist_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include "wifilist.h"

#define BASE CONFIG_WIFILIST_HTTP_BASE
#define MAX_BODY 4096

extern const uint8_t wifilist_js_start[] asm("_binary_wifilist_js_gz_start");
extern const uint8_t wifilist_js_end[] asm("_binary_wifilist_js_gz_end");

static void *big_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

static esp_err_t get_wifi(httpd_req_t *req)
{
    const size_t cap = 6144;
    wifilist_status_t *st = big_alloc(sizeof(*st));
    char *buf = big_alloc(cap);
    if (!st || !buf) {
        free(st);
        free(buf);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }
    wifilist_status(st);
    size_t n = wifilist_status_json(st, buf, cap);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t e = n ? httpd_resp_send(req, buf, (ssize_t)n)
                    : httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "too big");
    free(st);
    free(buf);
    return e;
}

static esp_err_t post_networks(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > MAX_BODY) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "{\"networks\": [...]}, at most 4 KB");
    }
    char *body = big_alloc(req->content_len + 1);
    if (!body) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            free(body);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "read failed");
        }
        got += (size_t)n;
    }
    body[got] = 0;
    char err[128];
    esp_err_t e = wifilist_update_json(body, got, err, sizeof(err));
    free(body);
    if (e != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, err);
    }
    return get_wifi(req);
}

static esp_err_t post_scan(httpd_req_t *req)
{
    wifilist_scan();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"scanning\":true}");
}

static esp_err_t get_js(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/javascript");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)wifilist_js_start, wifilist_js_end - wifilist_js_start);
}

esp_err_t wifilist_http_register(httpd_handle_t server)
{
    static const httpd_uri_t uris[] = {
        {.uri = BASE "/wifi", .method = HTTP_GET, .handler = get_wifi},
        {.uri = BASE "/wifi/networks", .method = HTTP_POST, .handler = post_networks},
        {.uri = BASE "/wifi/scan", .method = HTTP_POST, .handler = post_scan},
        {.uri = "/wifilist.js", .method = HTTP_GET, .handler = get_js},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t e = httpd_register_uri_handler(server, &uris[i]);
        if (e != ESP_OK) {
            return e;
        }
    }
    return ESP_OK;
}
