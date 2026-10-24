// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The bench API's transmit (docs/spec/bench-api.md section 7): WS /tx, one job at a time. The job itself (the
 * gates, the queue, pacing, the key-time limit, the dead-man rule) is common's rdof/benchtx.h; this file opens its
 * path, feeds it the tool's frames and the modem's bytes, and ticks it from a task of its own.
 *
 *   via=modem        the board's SDR modem (sdrmodem), when the SDR's front end can transmit and no roamer owns it
 *   via=radio:NAME   a bench radio's MMDVM modem, through the link's guard (lowest power, TX only on the lease)
 *
 * What the bench may transmit comes from bench.tx in the settings (rdof/benchcfg.h), never from the API. Every keying
 * is logged: the board's log, and the last few in /status.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app.h"

#if CONFIG_RDOF_BENCH

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "netclock.h"
#include "rdof/benchcfg.h"
#include "rdof/benchtx.h"

static const char *TAG = "bench";

static rdof_benchtx_policy_t policy = {.arm_gpio = -1};
static char policy_err[160];

#if CONFIG_RDOF_BENCH_TX

#define KEYLOG 8
#define TICK_MS 20

typedef struct {
    bool on, started;
    int fd;
    int64_t opened_us, lease_us;
    char session[16], label[32], via[32];
    uint32_t freq;
    bool radio;           /* via=radio:NAME, else the SDR modem */
    const char *code;     /* refused at the handshake: sent once the socket is up, then closed */
    char why[128];
    rdof_benchtx_t *job;
    StreamBufferHandle_t from_modem;
} txjob_t;

typedef struct {
    double t, seconds;
    uint32_t freq;
    int bursts;
    char why[10], label[32], via[32];
} keying_t;

static httpd_handle_t hd;
static SemaphoreHandle_t mu;
EXT_RAM_BSS_ATTR static txjob_t J;
EXT_RAM_BSS_ATTR static keying_t keylog[KEYLOG];
static int nkeys;

static double now_s(void) { return esp_timer_get_time() / 1e6; }

static bool armed(void) { return policy.arm_gpio < 0 || gpio_get_level(policy.arm_gpio) == 0; }

/* ── the job's ends ───────────────────────────────────────────────────────── */

static void ws_text(int fd, const char *s, size_t n)
{
    httpd_ws_frame_t f = {.final = true, .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)s, .len = n};
    httpd_ws_send_frame_async(hd, fd, &f);
}

static void job_to_tool(void *ctx, const char *js, size_t n) { ws_text(J.fd, js, n); }

static void job_to_modem(void *ctx, const uint8_t *b, size_t n)
{
    if (J.radio)
        bench_radio_tx_write(J.via + 6, b, n);
    else
        sdr_modem_bench_write(b, n);
}

/* From the modem (the SDR modem's lock or the radio's link task): queued, never waiting. */
static void modem_bytes(void *ctx, const uint8_t *b, size_t n)
{
    if (J.from_modem)
        xStreamBufferSend(J.from_modem, b, n, 0);
}

static void job_keying(void *ctx, double start, double seconds, int bursts, const char *why)
{
    keying_t *k = &keylog[nkeys++ % KEYLOG];
    *k = (keying_t){.t = start, .seconds = seconds, .freq = J.freq, .bursts = bursts};
    snprintf(k->why, sizeof k->why, "%s", why);
    snprintf(k->label, sizeof k->label, "%s", J.label);
    snprintf(k->via, sizeof k->via, "%s", J.via);
    ESP_LOGW(TAG, "TX: %s (%s) keyed %.3f s on %.5f MHz via %s, %d bursts, %s", J.session, J.label, seconds,
             J.freq / 1e6, J.via, bursts, why);
}

static double job_utc(void *ctx)
{
    bool synced;
    return netclock_utc_ns(esp_timer_get_time(), &synced) / 1e9;
}

/* Let the path go and close the socket. Under mu. */
static void job_end(void)
{
    if (!J.code) { /* the job was started and its path opened */
        if (J.job->state != RDOF_TXS_DONE)
            rdof_benchtx_stop(J.job, NULL, "closed", now_s());
        if (J.radio)
            bench_radio_tx_close(J.via + 6);
        else
            sdr_modem_bench_close();
    }
    httpd_sess_trigger_close(hd, J.fd);
    J.on = false;
}

/* ── the task ─────────────────────────────────────────────────────────────── */

static void tx_task(void *arg)
{
    EXT_RAM_BSS_ATTR static uint8_t buf[512];
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        xSemaphoreTake(mu, portMAX_DELAY);
        if (!J.on) {
            xSemaphoreGive(mu);
            continue;
        }
        const double now = now_s();
        const bool up = httpd_ws_get_fd_info(hd, J.fd) == HTTPD_WS_CLIENT_WEBSOCKET;
        if (!up) {
            if (esp_timer_get_time() - J.opened_us > 2000000) /* not still finishing its handshake */
                job_end();
            xSemaphoreGive(mu);
            continue;
        }
        if (!J.started) {
            char js[256];
            int n = snprintf(js, sizeof js,
                             "{\"type\":\"start\",\"stream\":\"tx\",\"freq_hz\":%lu,\"via\":\"%s\",\"max_key_s\":%.1f,"
                             "\"timed\":false}",
                             (unsigned long)J.freq, J.via, policy.max_key_s);
            ws_text(J.fd, js, (size_t)n);
            J.started = true;
            if (J.code) {
                n = snprintf(js, sizeof js, "{\"type\":\"error\",\"code\":\"%s\",\"error\":\"%s\"}", J.code, J.why);
                ws_text(J.fd, js, (size_t)n);
                n = snprintf(js, sizeof js, "{\"type\":\"done\",\"why\":\"%s\",\"bursts\":0}", J.code);
                ws_text(J.fd, js, (size_t)n);
                job_end();
                xSemaphoreGive(mu);
                continue;
            }
        }
        size_t n;
        while ((n = xStreamBufferReceive(J.from_modem, buf, sizeof buf, 0)) > 0)
            rdof_benchtx_modem(J.job, buf, n, now);
        if (esp_timer_get_time() - J.lease_us > 1000000) { /* the lease as it is now (the tool renews it) */
            J.lease_us = esp_timer_get_time();
            uint32_t tx[RDOF_BENCHTX_MAX_FREQS];
            int k = bench_session_tx(J.session, tx, RDOF_BENCHTX_MAX_FREQS, false);
            bool covered = false;
            for (int i = 0; i < k; i++)
                covered |= tx[i] == J.freq;
            if (!covered)
                rdof_benchtx_stop(J.job, "refused", "the session's TX lease on this frequency ended", now);
        }
        if (!armed())
            rdof_benchtx_stop(J.job, "refused", "the TX arm jumper came out", now);
        rdof_benchtx_tick(J.job, now);
        if (rdof_benchtx_done(J.job))
            job_end();
        xSemaphoreGive(mu);
    }
}

/* ── WS /tx ───────────────────────────────────────────────────────────────── */

/* The handshake: refuse it when a job is open (the log says so), else take the one slot; a gate that's closed is an
 * error frame once the socket is up. */
static esp_err_t tx_open(httpd_req_t *req)
{
    char session[16] = "", via[32] = "modem", v[24] = "";
    bench_query(req, "session", session, sizeof session);
    bench_query(req, "via", via, sizeof via);
    double freq = bench_query(req, "freq_hz", v, sizeof v) ? strtod(v, NULL) : NAN;
    int cc = bench_query(req, "cc", v, sizeof v) ? atoi(v) : 1;
    xSemaphoreTake(mu, portMAX_DELAY);
    if (J.on) {
        xSemaphoreGive(mu);
        ESP_LOGW(TAG, "tx: one job at a time (session %s has it)", J.session);
        return ESP_FAIL;
    }
    StreamBufferHandle_t sb = J.from_modem;
    rdof_benchtx_t *job = J.job;
    J = (txjob_t){.on = true, .fd = httpd_req_to_sockfd(req), .opened_us = esp_timer_get_time(),
                  .lease_us = esp_timer_get_time(), .freq = isfinite(freq) ? (uint32_t)llround(freq) : 0,
                  .radio = !strncmp(via, "radio:", 6), .job = job, .from_modem = sb};
    snprintf(J.session, sizeof J.session, "%s", session);
    snprintf(J.via, sizeof J.via, "%s", via);
    bench_session_label(session, J.label, sizeof J.label);
    xStreamBufferReset(J.from_modem);
    uint32_t lease[RDOF_BENCHTX_MAX_FREQS];
    int nlease = session[0] ? bench_session_tx(session, lease, RDOF_BENCHTX_MAX_FREQS, true) : -1;
    const char *why = NULL, *code = "refused";
    if (!session[0] || nlease < 0) {
        why = session[0] ? "no such session (expired?)" : "TX needs a session (session=ID) with a TX lease";
        code = "bad_request";
    } else if (!J.freq || cc < 0 || cc > 15) {
        why = "freq_hz (Hz), and cc 0 to 15";
        code = "bad_request";
    } else if (strcmp(via, "modem") && !J.radio) {
        why = "via is modem or radio:NAME (no SDR I/Q transmit on this board)";
        code = "unsupported";
    } else if ((why = rdof_benchtx_gates(&policy, true, true, J.freq, lease, nlease, armed())) != NULL) {
    } else if ((why = J.radio ? bench_radio_tx_why(via + 6) : sdr_modem_bench_why()) != NULL) {
        code = J.radio && strstr(why, "no such") ? "bad_request"
             : J.radio && strstr(why, "ODR") ? "unsupported"
             : strstr(why, "another") || strstr(why, "linked") || strstr(why, "roamer") ? "busy"
             : J.radio ? "unavailable" : "refused";
    } else {
        const rdof_fe_caps_t *c = sdr_fe_caps();
        const double dbm = c ? c->tx_power_min + (c->tx_power_max - c->tx_power_min) * policy.rf_level / 255.0 : 0;
        why = J.radio ? bench_radio_tx_open(via + 6, session, J.freq, modem_bytes, NULL)
                      : sdr_modem_bench_open(modem_bytes, NULL, dbm);
        code = "unavailable";
    }
    if (why) {
        J.code = code;
        snprintf(J.why, sizeof J.why, "%s", why);
        ESP_LOGW(TAG, "tx: %s (%s): %s", session, via, why);
    } else {
        const rdof_benchtx_io_t io = {job_to_modem, job_to_tool, job_keying, job_utc, NULL};
        rdof_benchtx_init(J.job, &io, J.freq, cc, policy.rf_level, policy.max_key_s, now_s());
        ESP_LOGI(TAG, "tx: session %s (%s) on %.5f MHz via %s, cc %d", session, J.label, J.freq / 1e6, via, cc);
    }
    xSemaphoreGive(mu);
    return ESP_OK;
}

static esp_err_t ws_tx(httpd_req_t *req)
{
    if (req->method == HTTP_GET)
        return tx_open(req);
    httpd_ws_frame_t f = {0};
    if (httpd_ws_recv_frame(req, &f, 0) != ESP_OK)
        return ESP_FAIL;
    EXT_RAM_BSS_ATTR static char buf[8192]; /* the web server's one task */
    if (f.len >= sizeof buf)
        return ESP_FAIL;
    f.payload = (uint8_t *)buf;
    if (f.len && httpd_ws_recv_frame(req, &f, f.len) != ESP_OK)
        return ESP_FAIL;
    buf[f.len] = 0;
    const int fd = httpd_req_to_sockfd(req);
    if (f.type == HTTPD_WS_TYPE_PING) { /* control frames come here (handle_ws_control_frames): answer, count it */
        httpd_ws_frame_t pong = {.final = true, .type = HTTPD_WS_TYPE_PONG, .payload = f.payload, .len = f.len};
        httpd_ws_send_frame(req, &pong);
    }
    xSemaphoreTake(mu, portMAX_DELAY);
    if (J.on && J.fd == fd && !J.code) {
        if (f.type == HTTPD_WS_TYPE_TEXT)
            rdof_benchtx_tool(J.job, buf, f.len, now_s());
        else if (f.type == HTTPD_WS_TYPE_PING || f.type == HTTPD_WS_TYPE_PONG)
            rdof_benchtx_alive(J.job, now_s());
        else if (f.type == HTTPD_WS_TYPE_BINARY)
            rdof_benchtx_stop(J.job, "unsupported", "no I/Q transmit on this board", now_s());
        else if (f.type == HTTPD_WS_TYPE_CLOSE)
            rdof_benchtx_stop(J.job, NULL, "closed", now_s());
    }
    xSemaphoreGive(mu);
    if (f.type == HTTPD_WS_TYPE_CLOSE) {
        httpd_ws_frame_t c = {.final = true, .type = HTTPD_WS_TYPE_CLOSE};
        httpd_ws_send_frame(req, &c);
        return ESP_FAIL; /* closes the session */
    }
    return ESP_OK;
}

static const char *state(void)
{
    if (!J.on || J.code)
        return "idle";
    return rdof_benchtx_state(J.job);
}

size_t bench_tx_status_json(char *buf, size_t cap)
{
    size_t n = 0;
#define ADD(...) (n += n < cap ? (size_t)snprintf(buf + n, cap - n, __VA_ARGS__) : 0)
    xSemaphoreTake(mu, portMAX_DELAY);
    ADD("{\"state\":\"%s\"", state());
    if (J.on && !J.code)
        ADD(",\"job\":{\"session\":\"%s\",\"label\":\"%s\",\"freq_hz\":%lu,\"via\":\"%s\",\"bursts\":%ld,\"keyings\":%ld,"
            "\"underruns\":%ld,\"queued\":%d}",
            J.session, J.label, (unsigned long)J.freq, J.via, J.job->sent, J.job->keyings, J.job->underruns, J.job->qn);
    ADD(",\"log\":[");
    for (int i = nkeys > KEYLOG ? nkeys - KEYLOG : 0, k = 0; i < nkeys; i++, k++) {
        const keying_t *e = &keylog[i % KEYLOG];
        ADD("%s{\"t\":%.3f,\"freq_hz\":%lu,\"seconds\":%.3f,\"bursts\":%d,\"why\":\"%s\",\"label\":\"%s\",\"via\":\"%s\"}",
            k ? "," : "", e->t, (unsigned long)e->freq, e->seconds, e->bursts, e->why, e->label, e->via);
    }
    ADD("]}");
    xSemaphoreGive(mu);
#undef ADD
    return n < cap ? n : 0;
}

bool bench_tx_enabled(void) { return policy.nfreqs > 0 && policy.max_key_s > 0; }

int bench_tx_register(void *server)
{
    hd = server;
    if (!mu) {
        mu = xSemaphoreCreateMutex();
        J.job = heap_caps_calloc(1, sizeof *J.job, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        J.from_modem = xStreamBufferCreateWithCaps(2048, 1, MALLOC_CAP_SPIRAM);
        char *text = settings_text();
        if (rdof_bench_tx_parse(text, strlen(text), &policy, policy_err, sizeof policy_err) < 0) {
            ESP_LOGE(TAG, "%s: no TX", policy_err);
            policy = (rdof_benchtx_policy_t){.arm_gpio = -1};
        }
        free(text);
        if (policy.arm_gpio >= 0) {
            const gpio_config_t g = {.pin_bit_mask = 1ULL << policy.arm_gpio, .mode = GPIO_MODE_INPUT,
                                     .pull_up_en = GPIO_PULLUP_ENABLE};
            gpio_config(&g);
        }
        if (bench_tx_enabled())
            ESP_LOGW(TAG, "bench TX allowed on %d frequencies, %.1f s a keying, RF level %d%s", policy.nfreqs,
                     policy.max_key_s, policy.rf_level, policy.arm_gpio >= 0 ? ", with the arm jumper" : "");
        if (J.job && J.from_modem)
            xTaskCreatePinnedToCoreWithCaps(tx_task, "bench_txj", 4096, NULL, 4, NULL, 0, MALLOC_CAP_SPIRAM);
    }
    if (!J.job || !J.from_modem)
        return 0;
    const httpd_uri_t uri = {.uri = "/api/bench/v1/tx", .method = HTTP_GET, .handler = ws_tx, .is_websocket = true,
                             .handle_ws_control_frames = true};
    return httpd_register_uri_handler(server, &uri) == ESP_OK;
}

#else /* no CONFIG_RDOF_BENCH_TX */

size_t bench_tx_status_json(char *buf, size_t cap) { return (size_t)snprintf(buf, cap, "{\"state\":\"idle\"}"); }
bool bench_tx_enabled(void) { return false; }
int bench_tx_register(void *server) { return 0; }

#endif

size_t bench_tx_info_json(char *buf, size_t cap)
{
    size_t n = 0;
#define ADD(...) (n += n < cap ? (size_t)snprintf(buf + n, cap - n, __VA_ARGS__) : 0)
    ADD("{\"freqs_hz\":[");
    for (int i = 0; i < policy.nfreqs; i++)
        ADD("%s%lu", i ? "," : "", (unsigned long)policy.freqs_hz[i]);
    ADD("],\"max_key_s\":%.1f,\"power\":%d,\"arm\":", policy.max_key_s, policy.rf_level);
    if (policy.arm_gpio >= 0)
        ADD("{\"gpio\":%d,\"armed\":%s}", policy.arm_gpio, gpio_get_level(policy.arm_gpio) == 0 ? "true" : "false");
    else
        ADD("null");
#if CONFIG_RDOF_BENCH_TX
    ADD(",\"built\":true,\"via\":[");
#else
    ADD(",\"built\":false,\"via\":[");
#endif
    const char *why = sdr_modem_bench_why();
    size_t first = n;
    if (!why || !strstr(why, "can't transmit"))
        ADD("\"modem\"");
    if (n < cap) {
        size_t k = bench_radio_tx_vias(buf + n, cap - n);
        if (k && n == first) { /* drop the leading comma */
            memmove(buf + n, buf + n + 1, k);
            k--;
        }
        n += k;
    }
    ADD("],\"modem\":");
    if (why) {
        ADD("\"");
        for (const char *p = why; *p && n + 2 < cap; p++)
            if (*p != '"')
                buf[n++] = *p;
        ADD("\"");
    } else
        ADD("\"ready\"");
    if (policy_err[0])
        ADD(",\"error\":\"%s\"", policy_err);
    ADD("}");
#undef ADD
    return n < cap ? n : 0;
}

#endif
