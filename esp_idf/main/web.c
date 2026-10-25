// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The web page and its API (esp_http_server):
 *   GET  /                the page (gzipped, embedded)
 *   GET  /api/status      what the board is doing (JSON): the board, then a "roamer" and an "sdr" object for
 *                         the radios it runs
 *   GET  /api/config      the settings: the box config JSON as stored plus wifi (no passwords) and ntp
 *                         (export; ?download=1 to save it)
 *   POST /api/config      new settings JSON (import): checked, wifi and ntp to their
 *                         components, the rest stored, then a restart
 *   GET  /api/resolved    the settings as resolved (one line per setting group), and the SDR's tuner plan
 *   GET  /api/log         the log ring (text)
 *   GET  /api/lastheard   ?since=N: the recent calls, RF and NET, newest first (JSON; heard.c)
 *   GET  /api/tasks       each task's core, priority and share of a core since the last ask (text; with the FreeRTOS
 *                         run-time stats built in)
 *   GET  /api/coredump    the core dump in flash; DELETE erases it (crash.c, with /api/crash in a test build)
 *   POST /api/selftest    restart and run the SDR's CPU self-test first (CONFIG_RDOF_SDR)
 *   POST /api/spike       restart and run the USB spike once the RTL-SDR is in (CONFIG_RDOF_SDR)
 *   GET  /api/spectrum    ?since=N: the SDR's display spectrum rows newer than row N (JSON, each row base64,
 *                         one byte a bin, -2 x dBFS, lowest frequency first); asking keeps it running 10 s
 *   POST /api/ota         a new app image as the body (CONFIG_RDOF_OTA)
 *   POST /api/restart
 * and the components' own: /api/clock, /api/time, /api/clock/settings and /netclock.js (netclock_http.h),
 * /api/wifi, /api/wifi/networks, /api/wifi/scan and /wifilist.js (wifilist_http.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#include "app.h"
#include "mem.h"
#include "heard.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "gnss.h"
#if CONFIG_RDOF_SDR
#include "sdr_fe.h"
#endif
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "netclock.h"
#include "netclock_http.h"
#include "rdof/json.h"
#include "wifilist_http.h"
#if CONFIG_RDOF_OTA
#include "esp_ota_ops.h"
#endif
#if CONFIG_RDOF_SDR
#include "mbedtls/base64.h"
#endif

static const char *TAG = "web";

#define MAX_CONFIG 16384

extern const uint8_t page_start[] asm("_binary_index_html_gz_start");
extern const uint8_t page_end[] asm("_binary_index_html_gz_end");

static esp_err_t get_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, (const char *)page_start, page_end - page_start);
}

static esp_err_t send_json(httpd_req_t *req, const char *buf, size_t len)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, (ssize_t)len);
}

static const char *reset_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_EXT: return "reset pin";
    case ESP_RST_SW: return "restart";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    case ESP_RST_USB: return "usb";
    case ESP_RST_JTAG: return "jtag";
    default: return "unknown";
    }
}

static esp_err_t get_status(httpd_req_t *req)
{
    const size_t cap = 14336; /* two roamers with SCTP and jitter status, the SDR, the IP gateway, the memory plan */
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf)
        buf = malloc(cap);
    if (!buf)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    rdof_json_t j;
    rdof_json_init(&j, buf, cap);
    rdof_json_open(&j, NULL);
    rdof_json_str(&j, "name", g_roles.box);
    const esp_app_desc_t *app = esp_app_get_description();
    rdof_json_str(&j, "version", app->version); /* git describe --always --tags --dirty, from ESP-IDF */
    rdof_json_str(&j, "idf", app->idf_ver);
    rdof_json_int(&j, "uptime_s", esp_timer_get_time() / 1000000);
    rdof_json_str(&j, "reset", reset_name(esp_reset_reason()));
    rdof_json_bool(&j, "safe_mode", g_board.safe_mode);
    crash_status_json(&j); /* the core dump in flash, if any (crash.c) */
    rdof_json_int(&j, "heap_free", (long long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    rdof_json_int(&j, "psram_free", (long long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    mem_status_json(&j); /* the plan: each role's claims, internal / DMA / PSRAM free and largest, the margin */
    rdof_json_bool(&j, "settings_ok", g_cfg_ok);
    rdof_json_str(&j, "settings_error", g_cfg_error);
    rdof_json_bool(&j, "wifi", g_board.wifi_up);
    rdof_json_bool(&j, "ap_mode", g_board.ap_mode);
    rdof_json_str(&j, "ip", g_board.ip);
    rdof_json_str(&j, "clock", netclock_source_name(netclock_source()));
#if CONFIG_GNSS_UART_RX_GPIO >= 0
    {
        EXT_RAM_BSS_ATTR static char g[900]; /* the GNSS receiver's status (gnss.h); one status at a time (the web server's task) */
        if (gnss_status_json(g, sizeof g))
            rdof_json_raw(&j, "gnss", g);
    }
#endif
    rdof_json_bool(&j, "usb_either", g_board.usb_either);
#if CONFIG_RDOF_ROAMER
    rdof_json_bool(&j, "build_roamer", true);
#endif
#if CONFIG_RDOF_SDR
    rdof_json_bool(&j, "build_sdr", true);
    rdof_json_str(&j, "selftest", g_sstat.selftest);
    rdof_json_str(&j, "spike", g_sstat.spike);
#endif
#if CONFIG_RDOF_ROAMER
    if (g_roles.has_radio)
        roamer_status_json(&j);
#endif
#if CONFIG_RDOF_SDR
    if (g_roles.has_sdr)
        sdr_status_json(&j);
#endif
#if CONFIG_RDOF_IPGW
    ipgw_status_json(&j);
#endif
    rdof_json_close(&j);
    esp_err_t e = send_json(req, buf, j.len);
    free(buf);
    return e;
}

static esp_err_t get_config(httpd_req_t *req)
{
    char q[32];
    char *text = settings_export();
    if (!text)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK && strstr(q, "download")) {
        static char disp[80];
        snprintf(disp, sizeof disp, "attachment; filename=\"%s.json\"", g_roles.box);
        httpd_resp_set_hdr(req, "Content-Disposition", disp);
    }
    esp_err_t e = send_json(req, text, strlen(text));
    free(text);
    return e;
}

static esp_err_t get_resolved(httpd_req_t *req)
{
    const size_t cap = 5120;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf)
        buf = malloc(cap);
    if (!buf)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    size_t n = (size_t)snprintf(buf, cap, "%s runs: ", g_roles.box);
    for (int k = 0; k < g_roles.nradios; k++)
        n += (size_t)snprintf(buf + n, cap - n, "%s%s", k ? ", " : "", g_roles.radios[k]);
    n += (size_t)snprintf(buf + n, cap - n, "%s%s%s\n", g_roles.nradios && g_roles.has_sdr ? ", " : "",
                          g_roles.has_sdr ? g_roles.sdr : "",
                          g_board.usb_either ? " (both on USB: whichever is plugged in)" : "");
#if CONFIG_RDOF_ROAMER
    if (g_roles.has_radio && n < cap - 2) {
        n += (size_t)snprintf(buf + n, cap - n, "\n%s:\n", g_roles.radio);
        n += roamer_resolved(buf + n, cap - n);
    }
#endif
#if CONFIG_RDOF_SDR
    if (g_roles.has_sdr && n < cap - 2) {
        n += (size_t)snprintf(buf + n, cap - n, "\n%s:\n", g_roles.sdr);
        n += sdr_resolved(buf + n, cap - n);
    }
#endif
    httpd_resp_set_type(req, "text/plain");
    esp_err_t e = httpd_resp_send(req, buf, (ssize_t)(n < cap ? n : cap - 1));
    free(buf);
    return e;
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static esp_err_t post_config(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > MAX_CONFIG)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "settings: 1 to 16384 bytes of JSON");
    char *body = heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body)
        body = malloc(req->content_len + 1);
    if (!body)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    size_t total = 0;
    while (total < req->content_len) {
        int n = httpd_req_recv(req, body + total, req->content_len - total);
        if (n == HTTPD_SOCK_ERR_TIMEOUT)
            continue;
        if (n <= 0) {
            free(body);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "read failed");
        }
        total += (size_t)n;
    }
    body[total] = 0;
    char error[160];
    bool ok = settings_save(body, total, error, sizeof error);
    free(body);
    if (!ok) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, error);
    }
    httpd_resp_sendstr(req, "saved; restarting");
    xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t post_restart(httpd_req_t *req)
{
    httpd_resp_sendstr(req, "restarting");
    xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

#if CONFIG_RDOF_SDR
#if CONFIG_RDOF_SDR_LR2021
/* The LR2021's memory, read only: ?addr=0x...&len=bytes (a multiple of 4, at most 256 KB), as the CPU's
 * little-endian words. For the ROM dump (docs/spec/lr2021-iq.md); the reader loses samples meanwhile. */
static esp_err_t get_lr2021_mem(httpd_req_t *req)
{
    char q[64], v[16];
    uint32_t addr = 0, len = 256;
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK) {
        if (httpd_query_key_value(q, "addr", v, sizeof v) == ESP_OK)
            addr = (uint32_t)strtoul(v, NULL, 0);
        if (httpd_query_key_value(q, "len", v, sizeof v) == ESP_OK)
            len = (uint32_t)strtoul(v, NULL, 0);
    }
    if (len == 0 || len % 4 || len > 256 * 1024 || addr % 4 || addr + len > 0x1000000)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "addr and len: multiples of 4, len 4 to 256 KB, under 16 MB");
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    static uint32_t w[256]; /* httpd's one task */
    static uint8_t b[1024];
    for (uint32_t off = 0; off < len; off += sizeof b) {
        int n = (int)((len - off < sizeof b ? len - off : sizeof b) / 4);
        if (fe_lr2021_read_mem(addr + off, w, n)) {
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_FAIL; /* the client sees a short body */
        }
        for (int i = 0; i < n; i++)
            for (int k = 0; k < 4; k++)
                b[4 * i + k] = (uint8_t)(w[i] >> (8 * k));
        if (httpd_resp_send_chunk(req, (const char *)b, 4 * n) != ESP_OK)
            return ESP_FAIL;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}
#endif

#if CONFIG_RDOF_SDR && CONFIG_RDOF_SDR_LR2021
static int unhex(const char *s, uint8_t *out, int cap)
{
    int n = 0;
    for (; s[0] && s[1] && n < cap; s += 2) {
        char t[3] = {s[0], s[1], 0}, *e;
        out[n++] = (uint8_t)strtoul(t, &e, 16);
        if (*e)
            return -1;
    }
    return *s ? -1 : n;
}

/* POST /api/lr2021/hold?on=1[&keep=1][&patch=1|2] or on=0: lend the chip out for experiments (the reader stops; keep
 * leaves it receiving as it was, else it's reset to standby; patch=1 resets it and loads Semtech's patch RAM, patch=2
 * ours in its place, lr2021_patch_load_rdof) or give
 * it back (reset, and a new session starts). */
static esp_err_t post_lr2021_hold(httpd_req_t *req)
{
    char q[48], v[8];
    bool on = true, keep = false;
    int patch = 0;
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK) {
        if (httpd_query_key_value(q, "on", v, sizeof v) == ESP_OK)
            on = atoi(v) != 0;
        if (httpd_query_key_value(q, "keep", v, sizeof v) == ESP_OK)
            keep = atoi(v) != 0;
        if (httpd_query_key_value(q, "patch", v, sizeof v) == ESP_OK)
            patch = atoi(v);
    }
    if (fe_lr2021_hold(on, keep, patch))
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, fe_lr2021_error());
    return httpd_resp_sendstr(req, on ? "held\n" : "released\n");
}

/* POST /api/lr2021/dmr_rx?mhz=F[&family=bs|ms] (held, with patch=2): set the chip up to receive DMR bursts of a
 * family with its own 4-FSK modem (lr2021_fsk4_dmr_rx); then /api/lr2021/packet?op=rx&max=13 takes them. */
static esp_err_t post_lr2021_dmr_rx(httpd_req_t *req)
{
    char q[64], v[16];
    double mhz = 0;
    int family = LR2021_DMR_BS;
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK) {
        if (httpd_query_key_value(q, "mhz", v, sizeof v) == ESP_OK)
            mhz = strtod(v, NULL);
        if (httpd_query_key_value(q, "family", v, sizeof v) == ESP_OK)
            family = strcmp(v, "ms") == 0 ? LR2021_DMR_MS : strcmp(v, "bs") == 0 ? LR2021_DMR_BS : -1;
    }
    if (mhz < 100 || mhz > 1100 || family < 0)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mhz: the frequency; family: bs or ms");
    if (!fe_lr2021_held())
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "not held");
    if (fe_lr2021_dmr_rx(mhz * 1e6, family))
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, fe_lr2021_error());
    return httpd_resp_sendstr(req, "ok\n");
}

/* POST /api/lr2021/wake (held only): wake the chip from SetSleep, which /cmd can't (it waits for BUSY low first). */
static esp_err_t post_lr2021_wake(httpd_req_t *req)
{
    if (!fe_lr2021_held())
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "not held");
    if (fe_lr2021_wake())
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, fe_lr2021_error());
    return httpd_resp_sendstr(req, "awake\n");
}

/* POST /api/lr2021/cmd?f=HEX[&n=N] or ?fifo=1&n=N: a command frame (held only), and for n > 0 its n-byte answer;
 * fifo=1 reads n bytes of the RX FIFO directly. JSON back:
 * ok, the answer, the status after it (cmd code, mode), the error. */
static esp_err_t post_lr2021_cmd(httpd_req_t *req)
{
    EXT_RAM_BSS_ATTR static char q[600], v[560]; /* httpd's one task */
    EXT_RAM_BSS_ATTR static uint8_t f[260], out[256];
    char nv[8];
    int n = 0, len = -1;
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK) {
        if (httpd_query_key_value(q, "f", v, sizeof v) == ESP_OK)
            len = unhex(v, f, sizeof f);
        if (httpd_query_key_value(q, "n", nv, sizeof nv) == ESP_OK)
            n = atoi(nv);
        if (httpd_query_key_value(q, "fifo", nv, sizeof nv) == ESP_OK && atoi(nv)) /* a direct RX FIFO read */
            len = unhex("0001", f, sizeof f);
    }
    if (len < 2 || n < 0 || n > 256)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "f: a frame in hex (2 to 260 bytes); n: 0 to 256");
    uint8_t status[2] = {0xFF, 0xFF};
    int r = fe_lr2021_cmd(f, (size_t)len, out, (size_t)n, status);
    EXT_RAM_BSS_ATTR static char j[700];
    int o = snprintf(j, sizeof j, "{\"ok\":%s,\"held\":%s,\"rx\":\"", r ? "false" : "true",
                     fe_lr2021_held() ? "true" : "false");
    for (int i = 0; i < n && o < (int)sizeof j - 3; i++)
        o += snprintf(j + o, sizeof j - o, "%02X", out[i]);
    snprintf(j + o, sizeof j - o, "\",\"cmd\":%u,\"mode\":%u,\"error\":\"%s\"}\n", status[0], status[1],
             r ? fe_lr2021_error() : "");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, j);
}

/* GET /api/lr2021/capture?n=BYTES (held only, 4 MB at most): the RX FIFO read flat out, as the chip makes it.
 * Headers: X-Overflows, X-Microseconds (the capture's time). */
static esp_err_t get_lr2021_capture(httpd_req_t *req)
{
    char q[48], v[16];
    size_t n = 65536;
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK &&
        httpd_query_key_value(q, "n", v, sizeof v) == ESP_OK)
        n = strtoul(v, NULL, 0) & ~3u;
    if (!n || n > 4 * 1024 * 1024)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "n: 4 bytes to 4 MB");
    uint8_t *buf = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (!buf)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    size_t got = 0;
    unsigned ovf = 0;
    int64_t us = 0;
    if (fe_lr2021_capture(buf, n, &got, &ovf, &us)) {
        free(buf);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "not held, or the SPI failed");
    }
    static char h1[16], h2[24];
    snprintf(h1, sizeof h1, "%u", ovf);
    snprintf(h2, sizeof h2, "%lld", (long long)us);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "X-Overflows", h1);
    httpd_resp_set_hdr(req, "X-Microseconds", h2);
    esp_err_t e = httpd_resp_send(req, (const char *)buf, got);
    free(buf);
    return e;
}

/* A request's body into PSRAM (at most cap bytes, NUL after it): NULL if it's empty, too big or didn't come. */
static uint8_t *recv_body(httpd_req_t *req, size_t cap)
{
    if (req->content_len == 0 || req->content_len > cap)
        return NULL;
    uint8_t *b = heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    for (size_t got = 0; b && got < req->content_len;) {
        int n = httpd_req_recv(req, (char *)b + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT)
            continue;
        if (n <= 0) {
            free(b);
            return NULL;
        }
        got += (size_t)n;
    }
    if (b)
        b[req->content_len] = 0;
    return b;
}

/* POST /api/lr2021/seq (held only), body: one frame a line in hex, or RESET (NRESET and the chip's boot): sent in
 * order, each timed on the board to BUSY low. JSON back: per frame us, cmd, mode; stops at the first that fails. */
static esp_err_t post_lr2021_seq(httpd_req_t *req)
{
    enum { MAXF = 64 };
    char *b = (char *)recv_body(req, 16384);
    if (!b)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body: frames in hex, one a line (16 KB at most)");
    EXT_RAM_BSS_ATTR static uint8_t fr[MAXF][260];
    static const uint8_t *fp[MAXF];
    static size_t len[MAXF];
    static int64_t us[MAXF];
    static uint8_t stat[MAXF][2];
    int n = 0;
    for (char *save = NULL, *l = strtok_r(b, "\r\n", &save); l && n < MAXF; l = strtok_r(NULL, "\r\n", &save)) {
        while (*l == ' ')
            l++;
        if (!*l || *l == '#')
            continue;
        int k = strcmp(l, "RESET") == 0 ? 0 : unhex(l, fr[n], sizeof fr[n]);
        if (k < 0 || k == 1) {
            free(b);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "a line isn't a frame in hex or RESET");
        }
        fp[n] = fr[n];
        len[n++] = (size_t)k;
    }
    free(b);
    int ran = fe_lr2021_seq(fp, len, n, us, stat);
    if (ran < 0)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "not held");
    char *j = heap_caps_malloc(64 + 48 * (size_t)n, MALLOC_CAP_SPIRAM);
    if (!j)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    int o = sprintf(j, "{\"ran\":%d,\"of\":%d,\"error\":\"%s\",\"frames\":[", ran, n, ran < n ? fe_lr2021_error() : "");
    for (int i = 0; i < n && i <= ran && i < n; i++)
        o += sprintf(j + o, "%s{\"us\":%lld,\"cmd\":%u,\"mode\":%u}", i ? "," : "", (long long)us[i], stat[i][0], stat[i][1]);
    sprintf(j + o, "]}\n");
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_sendstr(req, j);
    free(j);
    return e;
}

/* POST /api/lr2021/tx?prefill=B&total=B&chunk=B&timeout=T&max_ms=M&poll_us=U&keep_fifo=1&no_settx=1&trace_all=1
 * (held only), body: the bytes for the TX FIFO (256 KB at most): a raw transmit run (fe_lr2021_tx). The setup is
 * the caller's (/cmd, /seq). JSON back: the timings, the flags, and the trace of the FIFO level and IRQs. */
static esp_err_t post_lr2021_tx(httpd_req_t *req)
{
    EXT_RAM_BSS_ATTR static char q[1024], av[800]; /* httpd's one task */
    char v[40];
    static fe_lr2021_tx_opts_t o;
    o = (fe_lr2021_tx_opts_t){.prefill = 1020, .max_ms = 2000};
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK) {
#define OPT(name, field) if (httpd_query_key_value(q, name, v, sizeof v) == ESP_OK) o.field = strtoul(v, NULL, 0)
        OPT("prefill", prefill);
        OPT("total", total);
        OPT("chunk", chunk);
        OPT("timeout", timeout);
        OPT("max_ms", max_ms);
        OPT("poll_us", poll_us);
        OPT("keep_fifo", keep_fifo);
        OPT("no_settx", no_settx);
        OPT("trace_all", trace_all);
        OPT("runs", runs);
#undef OPT
        int k;
        if (httpd_query_key_value(q, "start", v, sizeof v) == ESP_OK &&
            (k = unhex(v, o.start, sizeof o.start)) >= 2)
            o.start_len = (size_t)k;
        /* at=US:HEX,US:HEX (time order) */
        if (httpd_query_key_value(q, "at", av, sizeof av) == ESP_OK) {
            for (char *save = NULL, *it = strtok_r(av, ",", &save); it && o.n_at < FE_LR2021_TX_AT;
                 it = strtok_r(NULL, ",", &save)) {
                char *c = strchr(it, ':');
                if (!c)
                    break;
                *c = 0;
                k = unhex(c + 1, o.at[o.n_at].frame, sizeof o.at[o.n_at].frame);
                if (k < 2)
                    break;
                o.at[o.n_at].us = (uint32_t)strtoul(it, NULL, 0);
                o.at[o.n_at++].len = (size_t)k;
            }
        }
    }
    if (o.max_ms > 30000)
        o.max_ms = 30000;
    uint8_t *b = recv_body(req, 256 * 1024);
    if (!b)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body: 1 byte to 256 KB for the TX FIFO");
    fe_lr2021_tx_result_t *r = heap_caps_malloc(sizeof *r, MALLOC_CAP_SPIRAM);
    char *j = heap_caps_malloc(2048 + 48 * FE_LR2021_TX_TRACE, MALLOC_CAP_SPIRAM);
    if (!r || !j) {
        free(b);
        free(r);
        free(j);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }
    int rc = fe_lr2021_tx(b, req->content_len, &o, r);
    free(b);
    int k = sprintf(j,
                    "{\"ok\":%s,\"error\":\"%s\",\"settx_us\":%lld,\"done_us\":%lld,\"left_tx_us\":%lld,\"all_in_us\":%lld,"
                    "\"end_us\":%lld,\"sent\":%u,\"refills\":%u,\"timed_out\":%s,\"rx_flags\":%u,\"tx_flags\":%u,"
                    "\"irq\":%lu,\"mode\":%u,\"start_cmd\":%u,\"runs\":%d,\"trace\":[",
                    rc ? "false" : "true", rc ? fe_lr2021_error() : "", (long long)r->settx_us, (long long)r->done_us,
                    (long long)r->left_tx_us, (long long)r->all_in_us, (long long)r->end_us, (unsigned)r->sent,
                    r->refills, r->timed_out ? "true" : "false", r->rx_flags, r->tx_flags, (unsigned long)r->irq,
                    r->mode, r->start_cmd, r->runs);
    for (int i = 0; i < r->n_trace; i++)
        k += sprintf(j + k, "%s[%lu,%u,%u,%lu]", i ? "," : "", (unsigned long)r->trace[i].us, r->trace[i].level,
                     r->trace[i].mode, (unsigned long)r->trace[i].irq);
    k += sprintf(j + k, "],\"run_us\":[");
    for (int i = 0; i < r->runs; i++)
        k += sprintf(j + k, "%s[%lld,%lld]", i ? "," : "", (long long)r->run_start_us[i], (long long)r->run_done_us[i]);
    k += sprintf(j + k, "],\"at\":[");
    for (int i = 0; i < o.n_at; i++)
        k += sprintf(j + k, "%s[%lld,%u,%u]", i ? "," : "", (long long)r->at_us[i], r->at_status[i][0], r->at_status[i][1]);
    sprintf(j + k, "]}\n");
    free(r);
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_sendstr(req, j);
    free(j);
    return e;
}

/* POST /api/lr2021/packet?op=tx|rx&timeout=&max_ms=&fifo=&poll_us=&max=  One packet with the chip's modem (set up
 * first by /api/lr2021/seq): tx sends the body; rx answers with the packet in hex. */
static esp_err_t post_lr2021_packet(httpd_req_t *req)
{
    char q[160], v[16];
    lr2021_packet_cfg_t c = {.max_ms = 5000};
    bool tx = true;
    size_t max = 1024;
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK) {
        if (httpd_query_key_value(q, "op", v, sizeof v) == ESP_OK)
            tx = strcmp(v, "rx") != 0;
        if (httpd_query_key_value(q, "timeout", v, sizeof v) == ESP_OK)
            c.timeout = (uint32_t)strtoul(v, NULL, 0);
        if (httpd_query_key_value(q, "max_ms", v, sizeof v) == ESP_OK)
            c.max_ms = atoi(v);
        if (httpd_query_key_value(q, "fifo", v, sizeof v) == ESP_OK)
            c.fifo_bytes = strtoul(v, NULL, 0);
        if (httpd_query_key_value(q, "poll_us", v, sizeof v) == ESP_OK)
            c.poll_us = atoi(v);
        if (httpd_query_key_value(q, "max", v, sizeof v) == ESP_OK)
            max = strtoul(v, NULL, 0);
    }
    if (c.max_ms > 30000)
        c.max_ms = 30000;
    if (max > 4096)
        max = 4096;
    uint8_t *b;
    size_t n;
    if (tx) {
        if (!(b = recv_body(req, 4096)))
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body: the payload, 1 byte to 4 KB");
        n = req->content_len;
    } else {
        if (!(b = heap_caps_malloc(max, MALLOC_CAP_SPIRAM)))
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        n = max;
    }
    lr2021_packet_result_t r = {0};
    esp_err_t e = ESP_OK;
    size_t got = 0;
    int rc = fe_lr2021_packet(tx, b, n, &got, &c, &r, &e);
    char *j = heap_caps_malloc(256 + 2 * got, MALLOC_CAP_SPIRAM);
    if (!j) {
        free(b);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }
    int k = sprintf(j, "{\"ok\":%s,\"error\":\"%s\",\"irq\":%lu,\"len\":%u,\"done_us\":%lld,\"timed_out\":%s,"
                       "\"crc_error\":%s,\"data\":\"",
                    rc == 0 && e == ESP_OK ? "true" : "false", rc || e != ESP_OK ? fe_lr2021_error() : "",
                    (unsigned long)r.irq, (unsigned)r.len, (long long)r.done_us, r.timed_out ? "true" : "false",
                    r.crc_error ? "true" : "false");
    for (size_t i = 0; !tx && i < got; i++)
        k += sprintf(j + k, "%02x", b[i]);
    sprintf(j + k, "\"}\n");
    free(b);
    httpd_resp_set_type(req, "application/json");
    esp_err_t se = httpd_resp_sendstr(req, j);
    free(j);
    return se;
}

/* POST /api/lr2021/txmode?mode=cw|freq|phase|iqphase|polar|fsk4&hz=&calib=&dbm=&ramp=&duty=&slices=&fallback=
 *   &rate=&fdev=&shape=&rxbw=&n=&timeout=&max_ms=  plus, for polar, float=1&rmin=&peak=&table=w12|none, and for
 *   fsk4, dmr=1. The driver's TX modes (lr2021_tx.h) on the held chip; the body is the FIFO bytes (whole runs of n
 *   samples), or for float=1 float32 I then Q that the board turns into polar samples itself. */
static esp_err_t post_lr2021_txmode(httpd_req_t *req)
{
    EXT_RAM_BSS_ATTR static char q[512];
    char v[24];
    lr2021_tx_setup_t s = {.mode = LR2021_TX_FREQ, .standby_first = true, .ramp_code = 4, .pa_lf_duty = 4,
                           .pa_lf_slices = 2, .fallback = LR2021_FALLBACK_STBY_RC, .rate = 48000, .fdev_hz = 2048,
                           .rx_bw = 0x1C};
    double dbm = -9.5, rmin = 0.2, peak = 0;
    uint32_t timeout = 64000;
    int max_ms = 5000, is_float = 0, dmr = 0, table = 1;
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK) {
#define NUM(name) (httpd_query_key_value(q, name, v, sizeof v) == ESP_OK)
        if (NUM("mode")) {
            static const char *const names[] = {"cw", "freq", "phase", "iqphase", "polar", "fsk4"};
            for (int i = 0; i < 6; i++)
                if (!strcmp(v, names[i]))
                    s.mode = (lr2021_tx_mode_t)i;
        }
        if (NUM("hz")) s.freq_hz = (uint32_t)strtoul(v, NULL, 0);
        if (NUM("calib")) s.calib_fe = atoi(v);
        if (NUM("dbm")) dbm = atof(v);
        if (NUM("ramp")) s.ramp_code = (uint8_t)atoi(v);
        if (NUM("duty")) s.pa_lf_duty = (uint8_t)atoi(v);
        if (NUM("slices")) s.pa_lf_slices = (uint8_t)atoi(v);
        if (NUM("fallback")) s.fallback = (uint8_t)atoi(v);
        if (NUM("rate")) s.rate = (uint32_t)strtoul(v, NULL, 0);
        if (NUM("fdev")) s.fdev_hz = (uint32_t)strtoul(v, NULL, 0);
        if (NUM("shape")) s.shape = (uint8_t)strtoul(v, NULL, 0);
        if (NUM("rxbw")) s.rx_bw = (uint8_t)strtoul(v, NULL, 0);
        if (NUM("n")) s.n = (uint16_t)strtoul(v, NULL, 0);
        if (NUM("timeout")) timeout = (uint32_t)strtoul(v, NULL, 0);
        if (NUM("max_ms")) max_ms = atoi(v);
        if (NUM("float")) is_float = atoi(v);
        if (NUM("rmin")) rmin = atof(v);
        if (NUM("peak")) peak = atof(v);
        if (NUM("table")) table = strcmp(v, "none") != 0;
        if (NUM("dmr")) dmr = atoi(v);
#undef NUM
    }
    s.power_code = lr2021_tx_power_code(dbm);
    if (max_ms > 30000)
        max_ms = 30000;
    if (!s.freq_hz)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "hz: the (corrected) frequency");
    uint8_t *b = NULL;
    size_t n = 0;
    if (s.mode != LR2021_TX_CW) {
        b = recv_body(req, 256 * 1024);
        if (!b)
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body: 1 byte to 256 KB");
        n = req->content_len;
        if (is_float && s.mode == LR2021_TX_IQ_POLAR) { /* float32 I, Q -> int8 I, Q in place (8 bytes to 2) */
            size_t ns = n / 8;
            float *iq = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
            if (!iq) {
                free(b);
                return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
            }
            memcpy(iq, b, n);
            lr2021_polar_iq8(iq, ns, (float)peak, (float)rmin, table ? &lr2021_am_w12_920 : NULL, (int8_t *)b);
            free(iq);
            n = 2 * ns;
        }
        if (dmr && s.mode == LR2021_TX_FSK4)
            lr2021_fsk4_from_dmr(b, n);
        if (!s.n) {
            const int bps = lr2021_tx_sample_bytes(s.mode);
            size_t samples = n / (size_t)(bps ? bps : 1);
            s.n = (uint16_t)(samples > LR2021_TX_MAX_RUN ? LR2021_TX_MAX_RUN : samples);
        }
    }
    lr2021_tx_stream_stats_t st = {0};
    int setup_ok = 0;
    int rc = fe_lr2021_tx_mode(&s, b, n, timeout, max_ms, &st, &setup_ok);
    free(b);
    char j[512];
    snprintf(j, sizeof j,
             "{\"ok\":%s,\"error\":\"%s\",\"setup_ok\":%d,\"n\":%u,\"bytes\":%u,\"runs\":%d,\"sent\":%u,\"refills\":%u,"
             "\"tx_flags\":%u,\"early\":%s,\"timed_out\":%s,\"air_us\":%lld}\n",
             rc ? "false" : "true", rc ? fe_lr2021_error() : "", setup_ok, s.n, (unsigned)n, st.runs, (unsigned)st.sent,
             st.refills, st.tx_flags, st.early ? "true" : "false", st.timed_out ? "true" : "false",
             (long long)(st.end_us ? st.end_us - st.key_us : 0));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, j);
}

/* POST /api/lr2021/spi?hz=N[&delay=NS]: the SPI clock (past 16 MHz is out of spec); without hz, just say it. */
static esp_err_t post_lr2021_spi(httpd_req_t *req)
{
    char q[48], v[16];
    int hz = 0, delay = 0;
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK) {
        if (httpd_query_key_value(q, "hz", v, sizeof v) == ESP_OK)
            hz = atoi(v);
        if (httpd_query_key_value(q, "delay", v, sizeof v) == ESP_OK)
            delay = atoi(v);
    }
    int r = fe_lr2021_spi(hz, delay);
    char out[48];
    snprintf(out, sizeof out, r < 0 ? "failed\n" : "%d Hz\n", r);
    return httpd_resp_sendstr(req, out);
}
#endif

static esp_err_t post_selftest(httpd_req_t *req)
{
    settings_set_flag("selftest");
    httpd_resp_sendstr(req, "restarting to run the self-test (about 20 s; then see the log)");
    xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t get_rtl_regs(httpd_req_t *req)
{
    EXT_RAM_BSS_ATTR static char buf[4800]; /* httpd's one task */
    size_t n = fe_rtlsdr_regs_json(buf, sizeof buf);
    if (!n)
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no dongle");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, (ssize_t)n);
}

static esp_err_t get_spectrum(httpd_req_t *req)
{
    enum { MAX_ROWS = 16, MAX_BINS = 512 };
    char q[32], v[16];
    uint32_t since = 0;
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK &&
        httpd_query_key_value(q, "since", v, sizeof v) == ESP_OK)
        since = (uint32_t)strtoul(v, NULL, 10);
    uint8_t *rows = heap_caps_malloc(MAX_ROWS * MAX_BINS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t cap = 256 + MAX_ROWS * (MAX_BINS * 4 / 3 + 8);
    char *out = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rows || !out) {
        free(rows);
        free(out);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }
    uint32_t seq;
    sdr_spectrum_info_t info;
    int n = sdr_spectrum_rows(since, rows, MAX_ROWS, &seq, &info, NULL);
    size_t len = (size_t)snprintf(out, cap, "{\"seq\":%lu,\"bins\":%d,\"row_ms\":%d,\"rate\":%.0f,\"center\":%.0f,"
                                  "\"rows\":[", (unsigned long)seq, info.bins, info.row_ms, info.rate, info.center_hz);
    for (int i = 0; i < n; i++) {
        size_t w = 0;
        if (i)
            out[len++] = ',';
        out[len++] = '"';
        mbedtls_base64_encode((unsigned char *)out + len, cap - len - 8, &w, rows + (size_t)i * info.bins,
                              (size_t)info.bins);
        len += w;
        out[len++] = '"';
    }
    len += (size_t)snprintf(out + len, cap - len, "]}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_send(req, out, (ssize_t)len);
    free(rows);
    free(out);
    return e;
}

static esp_err_t post_spike(httpd_req_t *req)
{
    settings_set_flag("spike");
    httpd_resp_sendstr(req, "restarting to run the USB spike once the RTL-SDR is in (about a minute; see the log)");
    xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}
#endif

static esp_err_t get_log(httpd_req_t *req)
{
    size_t cap = (size_t)CONFIG_RDOF_LOG_RING_KB * 1024 + 1;
    if (cap < 64)
        cap = 64;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf)
        buf = malloc(cap);
    if (!buf)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    size_t n = logring_read(buf, cap);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_send(req, buf, (ssize_t)n);
    free(buf);
    return e;
}

/* The recent calls changed after ?since=N (heard.c). */
static esp_err_t get_lastheard(httpd_req_t *req)
{
    char q[32], v[16];
    uint32_t since = 0;
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK &&
        httpd_query_key_value(q, "since", v, sizeof v) == ESP_OK)
        since = (uint32_t)strtoul(v, NULL, 10);
    const size_t cap = 20480; /* 50 calls at about 380 bytes at most */
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf)
        buf = malloc(cap);
    if (!buf)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    size_t n = heard_json(since, buf, cap);
    esp_err_t e = send_json(req, buf, n);
    free(buf);
    return e;
}

#if CONFIG_FREERTOS_USE_TRACE_FACILITY && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
/* Each task's share of a core since the last ask, by its task number (the run-time counter is esp_timer us). */
static esp_err_t get_tasks(httpd_req_t *req)
{
    enum { MAXT = 64 };
    static uint32_t last_run[MAXT], last_num[MAXT];
    static int64_t last_us;
    TaskStatus_t *ts = heap_caps_malloc(MAXT * sizeof *ts, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *buf = heap_caps_malloc(MAXT * 64 + 64, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ts || !buf) {
        free(ts);
        free(buf);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }
    const int n = (int)uxTaskGetSystemState(ts, MAXT, NULL);
    const int64_t now = esp_timer_get_time();
    const double w = last_us ? (double)(now - last_us) / 100.0 : 0;
    size_t len = (size_t)snprintf(buf, 64, "%.1f s\ntask             core prio   %%  stack free\n", w / 1e4);
    for (int i = 0; i < n; i++) {
        const BaseType_t core = xTaskGetCoreID(ts[i].xHandle);
        uint32_t prev = 0;
        for (int k = 0; k < MAXT; k++)
            if (last_num[k] == ts[i].xTaskNumber) {
                prev = last_run[k];
                break;
            }
        /* stack free: the high-water mark, the least the task has had left since it started (bytes) */
        len += (size_t)snprintf(buf + len, 64, "%-16s %4s %4u %5.1f %6u\n", ts[i].pcTaskName,
                                core == tskNO_AFFINITY ? "any" : core ? "1" : "0", (unsigned)ts[i].uxCurrentPriority,
                                w > 0 ? (double)(uint32_t)(ts[i].ulRunTimeCounter - prev) / w : 0.0,
                                (unsigned)ts[i].usStackHighWaterMark);
    }
    memset(last_num, 0, sizeof last_num);
    for (int i = 0; i < n && i < MAXT; i++) {
        last_num[i] = ts[i].xTaskNumber;
        last_run[i] = (uint32_t)ts[i].ulRunTimeCounter;
    }
    last_us = now;
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_send(req, buf, (ssize_t)len);
    free(ts);
    free(buf);
    return e;
}
#endif

#if CONFIG_RDOF_OTA
static esp_err_t post_ota(httpd_req_t *req)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t ota;
    if (!part || esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
    static char buf[2048];
    int left = (int)req->content_len;
    ESP_LOGI(TAG, "OTA: %d bytes to %s", left, part->label);
#if CONFIG_RDOF_SDR
    /* the SDR's front end and DSP out of the way while the image comes in (a loaded board starved the upload);
     * a good image restarts the board, a bad one resumes them */
    if (!sdr_pause(true))
        ESP_LOGW(TAG, "OTA: the SDR didn't stop in 2 s");
#define OTA_FAILED() sdr_pause(false)
#else
#define OTA_FAILED() ((void)0)
#endif
    while (left > 0) {
        int n = httpd_req_recv(req, buf, left < (int)sizeof buf ? left : (int)sizeof buf);
        if (n == HTTPD_SOCK_ERR_TIMEOUT)
            continue;
        if (n <= 0 || esp_ota_write(ota, buf, (size_t)n) != ESP_OK) {
            esp_ota_abort(ota);
            OTA_FAILED();
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
        }
        left -= n;
    }
    if (esp_ota_end(ota) != ESP_OK || esp_ota_set_boot_partition(part) != ESP_OK) {
        OTA_FAILED();
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "image rejected");
    }
    httpd_resp_sendstr(req, "updated; restarting");
    xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}
#endif

void web_start(void)
{
    httpd_handle_t server;
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.max_uri_handlers = 64;
#if CONFIG_RDOF_BENCH
    c.uri_match_fn = bench_uri_match; /* wildcards, and the bench API's /radios/{name}/link */
#else
    c.uri_match_fn = httpd_uri_match_wildcard;
#endif
    c.max_open_sockets = 8;    /* a page keeps several keep-alive connections open... */
    c.lru_purge_enable = true; /* ...so when they run out, the oldest idle one goes: a second viewer still gets in */
    c.stack_size = 8192;
    c.core_id = 0;
    if (httpd_start(&server, &c) != ESP_OK) {
        ESP_LOGE(TAG, "can't start the web server");
        return;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = get_page},
        {.uri = "/api/status", .method = HTTP_GET, .handler = get_status},
        {.uri = "/api/config", .method = HTTP_GET, .handler = get_config},
        {.uri = "/api/config", .method = HTTP_POST, .handler = post_config},
        {.uri = "/api/resolved", .method = HTTP_GET, .handler = get_resolved},
        {.uri = "/api/log", .method = HTTP_GET, .handler = get_log},
        {.uri = "/api/lastheard", .method = HTTP_GET, .handler = get_lastheard},
#if CONFIG_FREERTOS_USE_TRACE_FACILITY && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
        {.uri = "/api/tasks", .method = HTTP_GET, .handler = get_tasks},
#endif
        {.uri = "/api/restart", .method = HTTP_POST, .handler = post_restart},
#if CONFIG_RDOF_SDR
        {.uri = "/api/selftest", .method = HTTP_POST, .handler = post_selftest},
        {.uri = "/api/spike", .method = HTTP_POST, .handler = post_spike},
        {.uri = "/api/spectrum", .method = HTTP_GET, .handler = get_spectrum},
        {.uri = "/api/rtl/regs", .method = HTTP_GET, .handler = get_rtl_regs},
#if CONFIG_RDOF_SDR_LR2021
        {.uri = "/api/lr2021/mem", .method = HTTP_GET, .handler = get_lr2021_mem},
        {.uri = "/api/lr2021/hold", .method = HTTP_POST, .handler = post_lr2021_hold},
        {.uri = "/api/lr2021/cmd", .method = HTTP_POST, .handler = post_lr2021_cmd},
        {.uri = "/api/lr2021/capture", .method = HTTP_GET, .handler = get_lr2021_capture},
        {.uri = "/api/lr2021/spi", .method = HTTP_POST, .handler = post_lr2021_spi},
        {.uri = "/api/lr2021/seq", .method = HTTP_POST, .handler = post_lr2021_seq},
        {.uri = "/api/lr2021/tx", .method = HTTP_POST, .handler = post_lr2021_tx},
        {.uri = "/api/lr2021/txmode", .method = HTTP_POST, .handler = post_lr2021_txmode},
        {.uri = "/api/lr2021/packet", .method = HTTP_POST, .handler = post_lr2021_packet},
        {.uri = "/api/lr2021/dmr_rx", .method = HTTP_POST, .handler = post_lr2021_dmr_rx},
        {.uri = "/api/lr2021/wake", .method = HTTP_POST, .handler = post_lr2021_wake},
#endif
#endif
#if CONFIG_RDOF_OTA
        {.uri = "/api/ota", .method = HTTP_POST, .handler = post_ota},
#endif
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++)
        httpd_register_uri_handler(server, &uris[i]);
    crash_register(server);
#if CONFIG_RDOF_BENCH
    bench_api_register(server);
#endif
    netclock_http_register(server);
    wifilist_http_register(server);
}
