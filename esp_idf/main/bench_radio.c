// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The bench API's radios (docs/spec/bench-api.md section 8): radios wired to the bench, from "bench": {"radios"}
 * in the settings (rdof/benchcfg.h), on one of the MCU's UARTs or on the USB host port (CDC-ACM, or a USB-serial
 * bridge: FTDI, CP210x, CH34x through Espressif's drivers). A tool reaches one over WS /radios/{name}/link: binary
 * frames are the radio's serial bytes, both ways, through the guard (rdof/benchguard.h); text frames from the board
 * are the guard's events. One link a radio at a time.
 *
 * A transmit job (bench_tx.c, via=radio:NAME) holds an MMDVM radio the same way, through the same guard, in place of
 * a link: its frames go to the guard as a tool's would, and what the guard passes back goes to the job.
 */
#include <stdio.h>
#include <string.h>

#include "app.h"

#if CONFIG_RDOF_BENCH

#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "rdof/benchcfg.h"
#include "rdof/benchguard.h"
#include "usb/cdc_acm_host.h"
#include "usb/vcp_ch34x.h"
#include "usb/vcp_cp210x.h"
#include "usb/vcp_ftdi.h"

static const char *TAG = "bench";

typedef struct {
    rdof_bench_radio_t cfg;
    int i;
    SemaphoreHandle_t mu; /* the guard and the link */
    StreamBufferHandle_t in; /* the radio's bytes, for the link task (never wait on mu where the USB driver runs) */
    long in_dropped;
    volatile bool up;     /* the port is open */
    char why[64];         /* why it isn't */
    cdc_acm_dev_hdl_t cdc;
    SemaphoreHandle_t gone;
    rdof_guard_t *g;
    int fd;               /* the link's socket, -1: none */
    int64_t opened_us, lease_us;
    uint32_t tx[RDOF_GUARD_MAX_TX]; /* the lease's TX frequencies as the link has them */
    int ntx;
    char session[16];
    long to_radio, from_radio;
    /* a transmit job holding the radio (bench_tx.c): what the guard passes back goes to it */
    void (*tx_cb)(void *ctx, const uint8_t *b, size_t n);
    void *tx_ctx;
    volatile bool tuning; /* POST /tune has the radio */
    uint32_t tuned_hz;    /* where the last tune left it, 0: never tuned */
} bradio_t;

EXT_RAM_BSS_ATTR static bradio_t radios[RDOF_BENCH_MAX_RADIOS];
static int nradios;
static httpd_handle_t hd;

static double now_s(void) { return esp_timer_get_time() / 1e6; }

/* ── the link's two ends ──────────────────────────────────────────────────── */

static void link_to_radio(void *ctx, const uint8_t *b, size_t n)
{
    bradio_t *r = ctx;
    if (!r->up)
        return;
    r->to_radio += (long)n;
    if (r->cfg.port == RDOF_BPORT_UART)
        uart_write_bytes(r->cfg.uart, b, n);
    else if (r->cdc)
        cdc_acm_host_data_tx_blocking(r->cdc, b, n, 200);
}

static void link_to_tool(void *ctx, const uint8_t *b, size_t n)
{
    bradio_t *r = ctx;
    if (r->tx_cb) {
        r->tx_cb(r->tx_ctx, b, n);
        return;
    }
    if (r->fd < 0)
        return;
    httpd_ws_frame_t f = {.final = true, .type = HTTPD_WS_TYPE_BINARY, .payload = (uint8_t *)b, .len = n};
    httpd_ws_send_frame_async(hd, r->fd, &f);
}

static void link_event(void *ctx, const char *kind, const char *detail)
{
    bradio_t *r = ctx;
    ESP_LOGW(TAG, "radio %s: %s: %s", r->cfg.name, kind, detail);
    if (r->fd < 0)
        return;
    char js[256];
    int n = snprintf(js, sizeof js, "{\"type\":\"guard\",\"event\":\"%s\",\"detail\":\"", kind);
    for (const char *p = detail; *p && n < (int)sizeof js - 4; p++)
        if (*p != '"' && *p != '\\' && (unsigned char)*p >= 0x20)
            js[n++] = *p;
    n += snprintf(js + n, sizeof js - (size_t)n, "\"}");
    httpd_ws_frame_t f = {.final = true, .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)js, .len = (size_t)n};
    httpd_ws_send_frame_async(hd, r->fd, &f);
}

/* Bytes from the radio, from its reader (UART) or the CDC-ACM driver's task (USB): queued, never waiting. */
static void radio_bytes(bradio_t *r, const uint8_t *b, size_t n)
{
    r->from_radio += (long)n;
    if (r->fd < 0 && !r->tx_cb && !r->tuning)
        return;
    size_t k = xStreamBufferSend(r->in, b, n, 0);
    r->in_dropped += (long)(n - k);
}

/* ── UART ─────────────────────────────────────────────────────────────────── */

static void uart_reader(void *arg)
{
    bradio_t *r = arg;
    EXT_RAM_BSS_ATTR static uint8_t bufs[RDOF_BENCH_MAX_RADIOS][256];
    uint8_t *buf = bufs[r->i];
    for (;;) {
        int n = uart_read_bytes(r->cfg.uart, buf, 256, pdMS_TO_TICKS(20));
        if (n > 0)
            radio_bytes(r, buf, (size_t)n);
    }
}

static void uart_start(bradio_t *r)
{
    const rdof_bench_radio_t *c = &r->cfg;
#if CONFIG_RDOF_ROAMER /* g_rcfg: the roamers' and repeaters' settings */
    for (int k = 0; k < g_roles.nradios; k++)
        if (g_rcfg[k] && !strcmp(g_rcfg[k]->modem_port, "uart") && g_rcfg[k]->uart_num == c->uart) {
            snprintf(r->why, sizeof r->why, "UART%d is %s's modem", c->uart, g_rcfg[k]->name);
            return;
        }
#endif
    uart_config_t u = {.baud_rate = (int)c->baud, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
                       .stop_bits = UART_STOP_BITS_1,
                       .flow_ctrl = c->rts >= 0 && c->cts >= 0 ? UART_HW_FLOWCTRL_CTS_RTS : UART_HW_FLOWCTRL_DISABLE,
                       .source_clk = UART_SCLK_DEFAULT};
    esp_err_t e = uart_driver_install(c->uart, 4096, 2048, 0, NULL, 0);
    if (e == ESP_OK)
        e = uart_param_config(c->uart, &u);
    if (e == ESP_OK)
        e = uart_set_pin(c->uart, c->tx, c->rx, c->rts >= 0 ? c->rts : UART_PIN_NO_CHANGE,
                         c->cts >= 0 ? c->cts : UART_PIN_NO_CHANGE);
    if (e != ESP_OK) {
        snprintf(r->why, sizeof r->why, "UART%d: %s", c->uart, esp_err_to_name(e));
        return;
    }
    r->up = true;
    xTaskCreatePinnedToCoreWithCaps(uart_reader, "bench_uart", 3072, r, 5, NULL, 0, MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "radio %s on UART%d (TX %d, RX %d) at %ld", c->name, c->uart, c->tx, c->rx, c->baud);
}

/* ── USB ──────────────────────────────────────────────────────────────────── */

static bool usb_data(const uint8_t *data, size_t len, void *arg)
{
    radio_bytes(arg, data, len);
    return true;
}

static void usb_event(const cdc_acm_host_dev_event_data_t *ev, void *arg)
{
    bradio_t *r = arg;
    if (ev->type == CDC_ACM_HOST_DEVICE_DISCONNECTED) {
        ESP_LOGW(TAG, "radio %s unplugged", r->cfg.name);
        r->up = false;
        snprintf(r->why, sizeof r->why, "unplugged");
        xSemaphoreGive(r->gone);
    }
}

static esp_err_t usb_open(bradio_t *r, cdc_acm_dev_hdl_t *d)
{
    const rdof_bench_radio_t *c = &r->cfg;
    const char chip = c->vid ? rdof_bench_usb_bridge(c->vid, c->pid) : 0;
    cdc_acm_host_device_config_t dc = {.connection_timeout_ms = 1000, .out_buffer_size = 512,
                                       .in_buffer_size = 512, .event_cb = usb_event, .data_cb = usb_data,
                                       .user_arg = r};
    if (c->port == RDOF_BPORT_CDC_ACM)
        return cdc_acm_host_open(c->vid, c->pid, 0, &dc, d);
    /* a bridge: the chip the vid / pid name, else each in turn (each open waits connection_timeout_ms) */
    esp_err_t e = ESP_ERR_NOT_FOUND;
    if (!chip || chip == 'f') {
        dc.in_buffer_size = 0; /* the FTDI driver strips each packet's two status bytes itself, and wants 0 here */
        if ((e = ftdi_vcp_open(c->vid ? c->pid : FTDI_PID_AUTO, 0, &dc, d)) == ESP_OK)
            return e;
        dc.in_buffer_size = 512;
    }
    if ((!chip || chip == 'c') && (e = cp210x_vcp_open(c->vid ? c->pid : CP210X_PID_AUTO, 0, &dc, d)) == ESP_OK)
        return e;
    if ((!chip || chip == 'h') && (e = ch34x_vcp_open(c->vid ? c->pid : CH34X_PID_AUTO, 0, &dc, d)) == ESP_OK)
        return e;
    return e;
}

static void usb_task(void *arg)
{
    bradio_t *r = arg;
    const rdof_bench_radio_t *c = &r->cfg;
    if (c->serial[0] || c->usb_port[0])
        ESP_LOGW(TAG, "radio %s: serial / usb_port aren't used yet: the first matching device", c->name);
    for (;;) {
        cdc_acm_dev_hdl_t d;
        esp_err_t e = usb_open(r, &d);
        if (e != ESP_OK) {
            snprintf(r->why, sizeof r->why, "not plugged in (%s)", esp_err_to_name(e));
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        cdc_acm_line_coding_t lc = {.dwDTERate = (uint32_t)c->baud, .bCharFormat = 0, .bParityType = 0,
                                    .bDataBits = 8};
        cdc_acm_host_line_coding_set(d, &lc);
        cdc_acm_host_set_control_line_state(d, true, false);
        xSemaphoreTake(r->gone, 0);
        r->cdc = d;
        r->up = true;
        r->why[0] = 0;
        ESP_LOGI(TAG, "radio %s on USB, %ld baud", c->name, c->baud);
        xSemaphoreTake(r->gone, portMAX_DELAY);
        xSemaphoreTake(r->mu, portMAX_DELAY);
        r->cdc = NULL;
        xSemaphoreGive(r->mu);
        cdc_acm_host_close(d);
    }
}

/* ── the link ─────────────────────────────────────────────────────────────── */

static bradio_t *by_uri(const char *uri)
{
    const char *p = strstr(uri, "/radios/");
    if (!p)
        return NULL;
    p += 8;
    size_t n = strcspn(p, "/?");
    for (int i = 0; i < nradios; i++)
        if (strlen(radios[i].cfg.name) == n && !strncmp(radios[i].cfg.name, p, n))
            return &radios[i];
    return NULL;
}

/* The tool is gone: the guard unkeys the radio (it goes on ticking until that's answered). */
static void unlink_radio(bradio_t *r)
{
    if (r->fd >= 0) {
        ESP_LOGI(TAG, "radio %s: link closed (%ld bytes to it, %ld from it)", r->cfg.name, r->to_radio,
                 r->from_radio);
        r->fd = -1;
        rdof_guard_end(r->g, now_s());
    }
    r->session[0] = 0;
}

/* WS /radios/{name}/link?session=ID&proto=odr-debug|mmdvm */
static esp_err_t ws_link(httpd_req_t *req)
{
    bradio_t *r = NULL;
    if (req->method == HTTP_GET) {
        if (!(r = by_uri(req->uri)))
            return ESP_FAIL;
        char session[16] = "", proto[12] = "";
        bench_query(req, "session", session, sizeof session);
        bench_query(req, "proto", proto, sizeof proto);
        uint32_t tx[RDOF_GUARD_MAX_TX];
        int ntx = 0;
        if (session[0] && (ntx = bench_session_tx(session, tx, RDOF_GUARD_MAX_TX, true)) < 0) {
            ESP_LOGW(TAG, "radio %s: no session %s", r->cfg.name, session);
            return ESP_FAIL;
        }
        if (!r->up) {
            ESP_LOGW(TAG, "radio %s: unavailable (%s)", r->cfg.name, r->why);
            return ESP_FAIL;
        }
        xSemaphoreTake(r->mu, portMAX_DELAY);
        if ((r->fd >= 0 && httpd_ws_get_fd_info(hd, r->fd) == HTTPD_WS_CLIENT_WEBSOCKET) || r->tx_cb || r->tuning) {
            xSemaphoreGive(r->mu);
            ESP_LOGW(TAG, "radio %s: already %s (session %s)", r->cfg.name,
                     r->tx_cb ? "transmitting" : r->tuning ? "tuning" : "linked", r->session);
            return ESP_FAIL;
        }
        if (!r->g)
            r->g = heap_caps_malloc(sizeof *r->g, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!r->g) {
            xSemaphoreGive(r->mu);
            return ESP_FAIL;
        }
        const bool odr = strcmp(proto[0] ? proto : r->cfg.proto, "mmdvm") != 0;
        rdof_guard_io_t io = {link_to_radio, link_to_tool, link_event, r};
        rdof_guard_init(r->g, odr ? RDOF_GUARD_ODR : RDOF_GUARD_MMDVM, &io, tx, ntx);
        xStreamBufferReset(r->in);
        r->fd = httpd_req_to_sockfd(req);
        r->opened_us = esp_timer_get_time();
        snprintf(r->session, sizeof r->session, "%s", session);
        r->to_radio = r->from_radio = 0;
        memcpy(r->tx, tx, sizeof tx);
        r->ntx = ntx;
        r->lease_us = r->opened_us;
        if (odr)
            rdof_guard_check(r->g, now_s()); /* the radio at its lowest power before the tool's first byte */
        xSemaphoreGive(r->mu);
        ESP_LOGI(TAG, "radio %s: linked (%s, %d TX frequencies)", r->cfg.name, odr ? "odr-debug" : "mmdvm", ntx);
        return ESP_OK;
    }
    /* a frame: the server doesn't parse the URI after the handshake, so the radio is the one linked to this socket */
    const int fd = httpd_req_to_sockfd(req);
    for (int i = 0; i < nradios && !r; i++)
        if (radios[i].fd == fd)
            r = &radios[i];
    if (!r)
        return ESP_FAIL;
    httpd_ws_frame_t f = {0};
    if (httpd_ws_recv_frame(req, &f, 0) != ESP_OK)
        return ESP_FAIL;
    if (!f.len)
        return ESP_OK;
    if (f.len > 2048)
        return ESP_FAIL;
    EXT_RAM_BSS_ATTR static uint8_t buf[2048]; /* the web server's one task */
    f.payload = buf;
    if (httpd_ws_recv_frame(req, &f, f.len) != ESP_OK)
        return ESP_FAIL;
    if (f.type != HTTPD_WS_TYPE_BINARY)
        return ESP_OK; /* text from the tool: nothing yet */
    xSemaphoreTake(r->mu, portMAX_DELAY);
    if (r->fd == httpd_req_to_sockfd(req))
        rdof_guard_tool(r->g, buf, f.len, now_s());
    xSemaphoreGive(r->mu);
    return ESP_OK;
}

/* Each radio's link: its bytes into the guard, and the guard's clock. */
static void link_task(void *arg)
{
    bradio_t *r = arg;
    EXT_RAM_BSS_ATTR static uint8_t bufs[RDOF_BENCH_MAX_RADIOS][512];
    uint8_t *buf = bufs[r->i];
    for (;;) {
        size_t n = xStreamBufferReceive(r->in, buf, 512, pdMS_TO_TICKS(20));
        /* once a second, the lease as it is now (not renewing it: the tool does that, or it lapses) */
        uint32_t tx[RDOF_GUARD_MAX_TX];
        int ntx = -2;
        char session[sizeof r->session];
        snprintf(session, sizeof session, "%s", r->session);
        if (session[0] && esp_timer_get_time() - r->lease_us > 1000000) {
            r->lease_us = esp_timer_get_time();
            ntx = bench_session_tx(session, tx, RDOF_GUARD_MAX_TX, false);
        }
        xSemaphoreTake(r->mu, portMAX_DELAY);
        if (r->fd >= 0 && httpd_ws_get_fd_info(hd, r->fd) != HTTPD_WS_CLIENT_WEBSOCKET &&
            esp_timer_get_time() - r->opened_us > 2000000)
            unlink_radio(r);
        if (r->fd >= 0 && ntx > -2 && !strcmp(session, r->session) &&
            (ntx < 0 ? r->ntx > 0 : ntx != r->ntx || memcmp(tx, r->tx, sizeof tx[0] * (size_t)ntx))) {
            if (ntx < 0)
                ntx = 0;
            ESP_LOGW(TAG, "radio %s: session %s's TX frequencies now %d", r->cfg.name, session, ntx);
            link_event(r, "lease", ntx ? "the lease's TX frequencies changed" : "the lease ended: receive only");
            memcpy(r->tx, tx, sizeof tx[0] * (size_t)ntx);
            r->ntx = ntx;
            rdof_guard_set_tx(r->g, tx, ntx, now_s());
        }
        if ((r->fd >= 0 || r->tx_cb || r->tuning) && n)
            rdof_guard_radio(r->g, buf, n, now_s());
        if (r->g && (r->fd >= 0 || r->tx_cb || rdof_guard_busy(r->g)))
            rdof_guard_tick(r->g, now_s());
        xSemaphoreGive(r->mu);
    }
}

/* ── status and start ─────────────────────────────────────────────────────── */

size_t bench_radios_json(char *buf, size_t cap, const char *only)
{
    size_t n = 0;
    n += (size_t)snprintf(buf + n, cap - n, "[");
    for (int i = 0, k = 0; i < nradios && n < cap; i++) {
        const bradio_t *r = &radios[i];
        const rdof_bench_radio_t *c = &r->cfg;
        if (only && strcmp(only, c->name))
            continue;
        char port[64];
        if (c->port == RDOF_BPORT_UART)
            snprintf(port, sizeof port, "uart%d tx %d rx %d, %ld baud", c->uart, c->tx, c->rx, c->baud);
        else
            snprintf(port, sizeof port, "usb %s %04X:%04X, %ld baud", c->port == RDOF_BPORT_CDC_ACM ? "cdc-acm" : "uart",
                     c->vid, c->pid, c->baud);
        const bool linked = r->fd >= 0 || r->tx_cb;
        n += (size_t)snprintf(buf + n, cap - n,
                              "%s{\"name\":\"%s\",\"proto\":\"%s\",\"port\":\"%s\",\"state\":\"%s\",\"why\":\"%s\","
                              "\"held_by\":%s%s%s,\"bytes_to\":%ld,\"bytes_from\":%ld,\"dropped\":%ld",
                              k++ ? "," : "", c->name, c->proto, port,
                              r->tx_cb ? "transmitting" : linked ? "linked" : r->up ? "ready" : "unavailable",
                              r->up ? "" : r->why,
                              linked ? "\"" : "", linked ? (r->session[0] ? r->session : "anonymous") : "null",
                              linked ? "\"" : "", r->to_radio, r->from_radio, r->in_dropped);
        if (n < cap && r->tuned_hz)
            n += (size_t)snprintf(buf + n, cap - n, ",\"tuned_hz\":%lu", (unsigned long)r->tuned_hz);
        if (n < cap && r->g && linked)
            n += (size_t)snprintf(buf + n, cap - n,
                                  ",\"guard\":{\"checks\":%ld,\"power_steps\":%ld,\"refused\":%ld,\"violations\":%ld}",
                                  r->g->stats.checks, r->g->stats.power_steps, r->g->stats.refused,
                                  r->g->stats.violations);
        if (n < cap)
            n += (size_t)snprintf(buf + n, cap - n, "}");
    }
    if (n < cap)
        n += (size_t)snprintf(buf + n, cap - n, "]");
    return n < cap ? n : 0;
}

int bench_radio_count(void) { return nradios; }

/* ── tune (POST /radios/{name}/tune) ──────────────────────────────────────── */

const char *bench_radio_tune(const char *name, const char *session, const rdof_guard_tune_t *tune, bool odr, int *http,
                             uint32_t *rx_hz, int *power)
{
    bradio_t *r = NULL;
    for (int i = 0; i < nradios && !r; i++)
        if (!strcmp(radios[i].cfg.name, name))
            r = &radios[i];
    *http = 409;
    if (!r) {
        *http = 404;
        return "no such bench radio";
    }
    if (!odr && strcmp(r->cfg.proto, "odr-debug")) {
        *http = 501;
        return "only an ODR radio is tuned (proto=odr-debug for an ODR radio set up as mmdvm); an MMDVM modem is "
               "tuned by each transmit job";
    }
    if (!r->up) {
        *http = 503;
        return r->why[0] ? r->why : "the radio isn't there";
    }
    uint32_t tx[RDOF_GUARD_MAX_TX];
    int ntx = session[0] ? bench_session_tx(session, tx, RDOF_GUARD_MAX_TX, true) : 0;
    xSemaphoreTake(r->mu, portMAX_DELAY);
    if (r->fd >= 0 || r->tx_cb || r->tuning) {
        xSemaphoreGive(r->mu);
        return r->tx_cb ? "the radio is transmitting" : r->tuning ? "the radio is being tuned" : "the radio is linked";
    }
    if (!r->g)
        r->g = heap_caps_malloc(sizeof *r->g, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!r->g) {
        xSemaphoreGive(r->mu);
        *http = 500;
        return "no memory for the guard";
    }
    const rdof_guard_io_t io = {link_to_radio, link_to_tool, link_event, r};
    rdof_guard_init(r->g, RDOF_GUARD_ODR, &io, tx, ntx > 0 ? ntx : 0);
    xStreamBufferReset(r->in);
    r->to_radio = r->from_radio = 0;
    r->tuning = true;
    const bool started = rdof_guard_tune(r->g, tune, now_s());
    xSemaphoreGive(r->mu);
    rdof_tune_state_t st = started ? RDOF_TUNE_RUNNING : RDOF_TUNE_FAILED;
    for (int i = 0; st == RDOF_TUNE_RUNNING && i < 250; i++) { /* the link task feeds and ticks the guard */
        vTaskDelay(pdMS_TO_TICKS(20));
        xSemaphoreTake(r->mu, portMAX_DELAY);
        st = rdof_guard_tune_state(r->g);
        xSemaphoreGive(r->mu);
    }
    xSemaphoreTake(r->mu, portMAX_DELAY);
    r->tuning = false;
    *rx_hz = r->g->view_rx_hz;
    *power = r->g->power;
    static char why[96];
    if (st == RDOF_TUNE_OK)
        r->tuned_hz = tune->freq_hz;
    else
        snprintf(why, sizeof why, "%s", st == RDOF_TUNE_RUNNING ? "the radio took too long" : r->g->tune_error);
    xSemaphoreGive(r->mu);
    if (st == RDOF_TUNE_OK) {
        ESP_LOGI(TAG, "radio %s: tuned to %.5f MHz (%s)", r->cfg.name, tune->freq_hz / 1e6, tune->fm ? "fm" : "dmr");
        return NULL;
    }
    ESP_LOGW(TAG, "radio %s: tune to %.5f MHz failed: %s", r->cfg.name, tune->freq_hz / 1e6, why);
    *http = 503;
    return why;
}

/* ── transmit jobs (bench_tx.c) ───────────────────────────────────────────── */

static bradio_t *by_name(const char *name)
{
    for (int i = 0; i < nradios; i++)
        if (!strcmp(radios[i].cfg.name, name))
            return &radios[i];
    return NULL;
}

const char *bench_radio_tx_why(const char *name)
{
    bradio_t *r = by_name(name);
    if (!r)
        return "no such bench radio";
    if (strcmp(r->cfg.proto, "mmdvm"))
        return "bursts go out through an MMDVM radio; an ODR radio's debug link has no burst transmit";
    if (!r->up)
        return r->why[0] ? r->why : "the radio isn't there";
    if (r->fd >= 0 || r->tx_cb || r->tuning)
        return r->tx_cb ? "the radio is transmitting for another job" : r->tuning ? "the radio is being tuned"
                                                                                : "the radio is linked";
    return NULL;
}

const char *bench_radio_tx_open(const char *name, const char *session, uint32_t freq,
                                void (*cb)(void *ctx, const uint8_t *b, size_t n), void *ctx)
{
    bradio_t *r = by_name(name);
    const char *why = bench_radio_tx_why(name);
    if (why)
        return why;
    xSemaphoreTake(r->mu, portMAX_DELAY);
    if (!r->g)
        r->g = heap_caps_malloc(sizeof *r->g, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!r->g) {
        xSemaphoreGive(r->mu);
        return "no memory for the guard";
    }
    const rdof_guard_io_t io = {link_to_radio, link_to_tool, link_event, r};
    rdof_guard_init(r->g, RDOF_GUARD_MMDVM, &io, &freq, 1); /* the one frequency this job may key on */
    xStreamBufferReset(r->in);
    r->tx_cb = cb;
    r->tx_ctx = ctx;
    snprintf(r->session, sizeof r->session, "%s", session);
    r->to_radio = r->from_radio = 0;
    xSemaphoreGive(r->mu);
    ESP_LOGI(TAG, "radio %s: transmitting for session %s on %.5f MHz", r->cfg.name, session, freq / 1e6);
    return NULL;
}

void bench_radio_tx_write(const char *name, const uint8_t *b, size_t n)
{
    bradio_t *r = by_name(name);
    if (!r)
        return;
    xSemaphoreTake(r->mu, portMAX_DELAY);
    if (r->tx_cb)
        rdof_guard_tool(r->g, b, n, now_s());
    xSemaphoreGive(r->mu);
}

/* The job is over: the guard puts the modem idle (it goes on ticking until that's answered). */
void bench_radio_tx_close(const char *name)
{
    bradio_t *r = by_name(name);
    if (!r)
        return;
    xSemaphoreTake(r->mu, portMAX_DELAY);
    if (r->tx_cb) {
        r->tx_cb = NULL;
        r->tx_ctx = NULL;
        r->session[0] = 0;
        rdof_guard_end(r->g, now_s());
        ESP_LOGI(TAG, "radio %s: transmit job over (%ld bytes to it, %ld from it)", r->cfg.name, r->to_radio,
                 r->from_radio);
    }
    xSemaphoreGive(r->mu);
}

/* The MMDVM radios, as "radio:NAME" items of a JSON list (for info's tx.via). */
size_t bench_radio_tx_vias(char *buf, size_t cap)
{
    size_t n = 0;
    for (int i = 0; i < nradios && n < cap; i++)
        if (!strcmp(radios[i].cfg.proto, "mmdvm"))
            n += (size_t)snprintf(buf + n, cap - n, ",\"radio:%s\"", radios[i].cfg.name);
    return n < cap ? n : 0;
}

int bench_radio_register(void *server)
{
    hd = server;
    char *text = settings_text();
    rdof_bench_radio_t cfg[RDOF_BENCH_MAX_RADIOS];
    char error[160];
    int n = rdof_bench_radios_parse(text, strlen(text), cfg, RDOF_BENCH_MAX_RADIOS, error, sizeof error);
    free(text);
    if (n < 0) {
        ESP_LOGE(TAG, "%s", error);
        n = 0;
    }
    bool usb = false;
    for (int i = 0; i < n; i++) {
        bradio_t *r = &radios[i];
        *r = (bradio_t){.cfg = cfg[i], .i = i, .fd = -1, .mu = xSemaphoreCreateMutex(),
                        .in = xStreamBufferCreate(4096, 1)};
        snprintf(r->why, sizeof r->why, "starting");
        if (cfg[i].port == RDOF_BPORT_UART)
            uart_start(r);
        else
            usb = true;
    }
    nradios = n;
    if (usb) {
        usb_host_start();
        usb_host_cdc_install();
        for (int i = 0; i < n; i++)
            if (radios[i].cfg.port != RDOF_BPORT_UART) {
                radios[i].gone = xSemaphoreCreateBinary();
                xTaskCreatePinnedToCoreWithCaps(usb_task, "bench_usb", 4096, &radios[i], 9, NULL, 0, MALLOC_CAP_SPIRAM);
            }
    }
    for (int i = 0; i < n; i++)
        xTaskCreatePinnedToCoreWithCaps(link_task, "bench_link", 3072, &radios[i], 4, NULL, 0, MALLOC_CAP_SPIRAM);
    const httpd_uri_t link = {.uri = "/api/bench/v1/radios/*/link", .method = HTTP_GET, .handler = ws_link,
                              .is_websocket = true};
    return httpd_register_uri_handler(server, &link) == ESP_OK;
}

#endif
