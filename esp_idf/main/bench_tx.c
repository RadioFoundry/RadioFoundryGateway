// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The bench API's transmit (docs/spec/bench-api.md section 7): WS /tx, one job at a time. The job itself (the
 * gates, the queue, pacing, the key-time limit, the dead-man rule) is common's rdof/benchtx.h; this file opens its
 * path, feeds it the tool's frames and the modem's bytes, and ticks it from a task of its own.
 *
 *   via=modem        the board's SDR modem (sdrmodem), when the SDR's front end can transmit and no roamer owns it;
 *                    under version 3, when a roamer owns it, the router's transmitter pool (rdof/benchroute.h), on
 *                    the path at the job's frequency, as the parrot's answers go
 *   via=radio:NAME   a bench radio's MMDVM modem, through the link's guard (lowest power, TX only on the lease)
 *
 * The line protocol's outbound job (bench_tx outbound) takes the SDR modem itself, never through the router: a
 * roamer's modem is parked for it (modem_sdr.c) and comes back after.
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
#include "rdof/benchroute.h"
#include "rdof/benchtx.h"
#include "rdof/layer2.h"
#include "rdof/mmdvm.h"
#include "rdof/sdr/sdrmodem.h"

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
    bool routed;          /* via=modem through the router's pool (version 3, a roamer owns the modem) */
    int path;             /* routed: the path at freq */
    bool line;            /* the line protocol's job (bench_line.c): no socket of its own, no lease */
    bool ob;              /* line, an outbound: the modem keyed by its own frames, no rdof_benchtx job */
    bool ob_unkeyed;      /* outbound: DMR_START 00 sent */
    bool sig;             /* line, a signal mode's transmission (with ob: the modem the job's alone) */
    double ob_start;      /* outbound: opened (UTC) */
    int ob_naks;          /* outbound: the modem's NAKs */
    uint8_t ob_nak[2];    /* the last one: its command and reason */
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
EXT_RAM_BSS_ATTR static rdof_benchroute_t route;

static double now_s(void) { return esp_timer_get_time() / 1e6; }

static bool armed(void) { return policy.arm_gpio < 0 || gpio_get_level(policy.arm_gpio) == 0; }

/* ── the job's ends ───────────────────────────────────────────────────────── */

static void ws_text(int fd, const char *s, size_t n)
{
    httpd_ws_frame_t f = {.final = true, .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)s, .len = n};
    httpd_ws_send_frame_async(hd, fd, &f);
}

static void job_to_tool(void *ctx, const char *js, size_t n)
{
    if (!J.line)
        ws_text(J.fd, js, n);
}

/* From the modem (the SDR modem's lock or the radio's link task): queued, never waiting. */
static void modem_bytes(void *ctx, const uint8_t *b, size_t n)
{
    if (J.from_modem)
        xStreamBufferSend(J.from_modem, b, n, 0);
}

static void job_to_modem(void *ctx, const uint8_t *b, size_t n)
{
    if (J.radio)
        bench_radio_tx_write(J.via + 6, b, n);
    else if (J.routed)
        rdof_benchroute_write(&route, b, n);
    else
        sdr_modem_bench_write(b, n);
}

/* ── via=modem through the router ─────────────────────────────────────────── */

/* Version 3 with a roamer on the SDR: the modem is the router's, so a job's bursts are an app's stream for the pool. */
static bool via_router(void)
{
#if RDOF_FEATURE_TX
    const char *why = sdr_modem_bench_why();
    return site_pool_on() && why && strstr(why, "roamer");
#else
    return false;
#endif
}

/* A burst to the pool (under mu, then the site's lock); false: no transmitter free, the burst is NAKed. */
static bool route_burst(void *ctx, const rdof_net_burst_t *b)
{
#if RDOF_FEATURE_TX
    return site_app_burst(J.path, b, false);
#else
    return false;
#endif
}

/* Open the routed path at freq: NULL, or why not. */
static const char *route_open(uint32_t freq)
{
#if RDOF_FEATURE_TX
    J.path = site_app_path(freq, 2);
    if (J.path < 0)
        J.path = site_app_path(freq, 1);
    if (J.path < 0)
        return "no talk path at that frequency (the site's frequencies)";
    J.routed = true;
    rdof_benchroute_init(&route, modem_bytes, route_burst, NULL);
    return NULL;
#else
    return "no router in this build";
#endif
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
    if (J.ob) {
        sdr_modem_bench_close();
        J.on = false;
        return;
    }
    if (!J.code) { /* the job was started and its path opened */
        if (J.job->state != RDOF_TXS_DONE)
            rdof_benchtx_stop(J.job, NULL, "closed", now_s());
        if (J.radio)
            bench_radio_tx_close(J.via + 6);
        else if (!J.routed)
            sdr_modem_bench_close();
    }
    if (!J.line)
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
        if (J.ob) { /* the modem's answers: a NAK fails the job */
            size_t n;
            while ((n = xStreamBufferReceive(J.from_modem, buf, sizeof buf, 0)) > 0)
                for (size_t i = 0; i + 4 < n; i++)
                    if (buf[i] == RDOF_MMDVM_FRAME_START && buf[i + 1] == 5 && buf[i + 2] == RDOF_MMDVM_NAK) {
                        J.ob_naks++;
                        J.ob_nak[0] = buf[i + 3], J.ob_nak[1] = buf[i + 4];
                    }
            if (!armed() && J.sig && !J.why[0]) {
                char w[8];
                sdr_modem_bench_signal(0, NULL, w, sizeof w);
                snprintf(J.why, sizeof J.why, "the TX arm jumper came out");
            } else if (!armed() && !J.ob_unkeyed && !J.sig) {
                const uint8_t off = 0;
                uint8_t f[8];
                sdr_modem_bench_write(f, rdof_mmdvm_frame(RDOF_MMDVM_DMR_START, &off, 1, f, sizeof f));
                J.ob_unkeyed = true;
                snprintf(J.why, sizeof J.why, "the TX arm jumper came out");
            }
            xSemaphoreGive(mu);
            continue;
        }
        if (J.line) { /* its connection is the engine's; bench_tx_line_close ends it */
            size_t n;
            while ((n = xStreamBufferReceive(J.from_modem, buf, sizeof buf, 0)) > 0)
                rdof_benchtx_modem(J.job, buf, n, now);
            if (!armed())
                rdof_benchtx_stop(J.job, "refused", "the TX arm jumper came out", now);
            rdof_benchtx_alive(J.job, now);
            rdof_benchtx_tick(J.job, now);
            xSemaphoreGive(mu);
            continue;
        }
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
    } else if (!J.radio && via_router()) {
        why = route_open(J.freq);
        code = "unavailable";
    } else if ((why = J.radio ? bench_radio_tx_why(via + 6) : sdr_modem_bench_why()) != NULL) {
        code = J.radio && strstr(why, "no such") ? "bad_request"
             : J.radio && strstr(why, "ODR") ? "unsupported"
             : strstr(why, "another") || strstr(why, "linked") || strstr(why, "roamer") ? "busy"
             : J.radio ? "unavailable" : "refused";
    } else {
        const rdof_fe_caps_t *c = sdr_fe_caps();
        const double dbm = c ? c->tx_power_min + (c->tx_power_max - c->tx_power_min) * policy.rf_level / 255.0 : 0;
        why = J.radio ? bench_radio_tx_open(via + 6, session, J.freq, modem_bytes, NULL)
                      : sdr_modem_bench_open(modem_bytes, NULL, dbm, false);
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

/* ── the line protocol's jobs (bench_line.c, bench-api.md 14.3) ─────────────
 * The engine's tx hooks, from its thread: the SDR modem in DMO under bench.tx's gates, in the slot WS /tx uses (one
 * job at a time between them). */

int bench_tx_freqs(uint32_t *out, int max)
{
    int k = 0;
    for (; k < policy.nfreqs && k < max; k++)
        out[k] = policy.freqs_hz[k];
    return k;
}

bool bench_tx_listed(uint32_t freq)
{
    for (int i = 0; i < policy.nfreqs; i++)
        if (policy.freqs_hz[i] == freq)
            return true;
    return false;
}

const char *bench_tx_line_why(void)
{
    if (!bench_tx_enabled())
        return policy_err[0] ? policy_err : "no transmit frequencies (bench.tx in the settings)";
    if (!J.job || !J.from_modem)
        return "no memory for a transmit job";
    if (via_router())
        return NULL;
    const char *why = sdr_modem_bench_why();
    return why && strstr(why, "another") ? NULL : why; /* busy is the job's to wait out, not a refusal */
}

bool bench_tx_line_open(uint32_t freq, int cc, int power, char *why, size_t cap)
{
    why[0] = 0;
    xSemaphoreTake(mu, portMAX_DELAY);
    const char *no = NULL;
    if (J.on)
        no = "the transmitter is busy with another bench job";
    else if ((no = rdof_benchtx_gates(&policy, true, true, freq, &freq, 1, armed())) == NULL) {
        const int level = power < policy.rf_level ? power : policy.rf_level;
        const rdof_fe_caps_t *c = sdr_fe_caps();
        const double dbm = c ? c->tx_power_min + (c->tx_power_max - c->tx_power_min) * level / 255.0 : 0;
        StreamBufferHandle_t sb = J.from_modem;
        rdof_benchtx_t *job = J.job;
        J = (txjob_t){.on = true, .line = true, .fd = -1, .freq = freq, .job = job, .from_modem = sb};
        snprintf(J.session, sizeof J.session, "line");
        snprintf(J.label, sizeof J.label, "line protocol");
        snprintf(J.via, sizeof J.via, "modem");
        xStreamBufferReset(J.from_modem);
        if ((no = via_router() ? route_open(freq) : sdr_modem_bench_open(modem_bytes, NULL, dbm, false)) != NULL) {
            J.on = false;
        } else {
            const rdof_benchtx_io_t io = {job_to_modem, job_to_tool, job_keying, job_utc, NULL};
            rdof_benchtx_init(J.job, &io, freq, cc, level, policy.max_key_s, now_s());
            if (J.routed)
                snprintf(why, cap, "through the router: its transmitter sends at the frequency's power");
            else if (power > level)
                snprintf(why, cap, "--power %d is over bench.tx's rf_level: %d (%.0f dBm)", power, level, dbm);
            ESP_LOGI(TAG, "tx: a line protocol job on %.5f MHz, cc %d, level %d", freq / 1e6, cc, level);
        }
    }
    xSemaphoreGive(mu);
    if (no)
        snprintf(why, cap, "%s", no);
    return !no;
}

int bench_tx_line_space(char *why, size_t cap)
{
    xSemaphoreTake(mu, portMAX_DELAY);
    int n = 0;
    if (!J.on || !J.line || J.job->state == RDOF_TXS_DONE) {
        snprintf(why, cap, "%s", J.on && J.job->error[0] ? J.job->error : "the modem stopped");
        n = -1;
    } else if (J.job->state == RDOF_TXS_READY) {
        n = RDOF_BENCHTX_QUEUE - J.job->qn;
    }
    xSemaphoreGive(mu);
    return n;
}

void bench_tx_line_push(uint8_t control, const uint8_t burst[33])
{
    xSemaphoreTake(mu, portMAX_DELAY);
    rdof_benchtx_t *t = J.job;
    if (J.on && J.line && t->qn < RDOF_BENCHTX_QUEUE) {
        rdof_benchtx_item_t *it = &t->q[(t->qh + t->qn++) % RDOF_BENCHTX_QUEUE];
        it->control = control;
        memcpy(it->burst, burst, 33);
    }
    xSemaphoreGive(mu);
}

bool bench_tx_line_idle(void)
{
    xSemaphoreTake(mu, portMAX_DELAY);
    const bool idle = !J.on || !J.line || J.job->state == RDOF_TXS_DONE ||
                      (J.job->state == RDOF_TXS_READY && !J.job->keyed && !J.job->qn);
    xSemaphoreGive(mu);
    return idle;
}

void bench_tx_line_close(void)
{
    xSemaphoreTake(mu, portMAX_DELAY);
    if (J.on && J.line)
        job_end();
    xSemaphoreGive(mu);
}

/* ── the line protocol's outbound (bench_tx outbound, bench-api.md 14.3) ───
 * The SDR modem as the TX half of a repeater, keyed for the job by its own frames (rdof_sdrmodem_outbound_frames):
 * the modem sends the idle bursts and the CACH. bench.tx's gates and power apply; its max_key_s doesn't (the job's
 * --seconds, at most RDOF_BJ_OUTBOUND_MAX_S). */

const char *bench_tx_ob_why(void)
{
    if (!bench_tx_enabled())
        return policy_err[0] ? policy_err : "no transmit frequencies (bench.tx in the settings)";
    if (!J.job || !J.from_modem)
        return "no memory for a transmit job";
    const char *why = sdr_modem_bench_borrow_why(true);
    return why && (strstr(why, "another") || strstr(why, "roamer")) ? NULL : why; /* busy: the job's to wait out */
}

bool bench_tx_ob_open(uint32_t freq, int cc, int power, double seconds, char *why, size_t cap)
{
    why[0] = 0;
    xSemaphoreTake(mu, portMAX_DELAY);
    const char *no = NULL;
    const int level = power < policy.rf_level ? power : policy.rf_level;
    const rdof_fe_caps_t *c = sdr_fe_caps();
    const double dbm = c ? c->tx_power_min + (c->tx_power_max - c->tx_power_min) * level / 255.0 : 0;
    if (!bench_tx_enabled())
        no = policy_err[0] ? policy_err : "no transmit frequencies (bench.tx in the settings)";
    else if (!J.job || !J.from_modem)
        no = "no memory for a transmit job";
    else if (J.on)
        no = "the transmitter is busy with another bench job";
    else if ((no = rdof_benchtx_gates(&policy, true, true, freq, &freq, 1, armed())) == NULL &&
             (no = sdr_modem_bench_open(modem_bytes, NULL, dbm, true)) == NULL) {
        StreamBufferHandle_t sb = J.from_modem;
        rdof_benchtx_t *job = J.job;
        J = (txjob_t){.on = true, .line = true, .ob = true, .fd = -1, .freq = freq, .job = job, .from_modem = sb,
                      .ob_start = job_utc(NULL)};
        snprintf(J.session, sizeof J.session, "line");
        snprintf(J.label, sizeof J.label, "outbound");
        snprintf(J.via, sizeof J.via, "modem");
        xStreamBufferReset(J.from_modem);
        const int s = (int)ceil(seconds);
        uint8_t f[160];
        const size_t n = rdof_sdrmodem_outbound_frames(freq, cc, level, s + 15, s + 10, f, sizeof f);
        sdr_modem_bench_write(f, n);
        if (power > level)
            snprintf(why, cap, "--power %d is over bench.tx's rf_level: %d (%.0f dBm)", power, level, dbm);
        ESP_LOGW(TAG, "TX: outbound on %.5f MHz, cc %d, %.1f s, level %d (%.0f dBm)", freq / 1e6, cc, seconds, level,
                 dbm);
    }
    xSemaphoreGive(mu);
    if (no)
        snprintf(why, cap, "%s", no);
    return !no;
}

/* 0, or -1 with why: the job's modem stopped (a NAK, the arm jumper, the job gone). */
int bench_tx_ob_check(char *why, size_t cap)
{
    xSemaphoreTake(mu, portMAX_DELAY);
    int r = 0;
    if (!J.on || !J.ob) {
        snprintf(why, cap, "the outbound's modem was let go");
        r = -1;
    } else if (J.ob_naks) {
        snprintf(why, cap, "the modem refused command 0x%02X (reason %u)", J.ob_nak[0], J.ob_nak[1]);
        r = -1;
    } else if (J.why[0]) {
        snprintf(why, cap, "%s", J.why);
        r = -1;
    }
    xSemaphoreGive(mu);
    return r;
}

void bench_tx_ob_unkey(void)
{
    xSemaphoreTake(mu, portMAX_DELAY);
    if (J.on && J.ob && !J.ob_unkeyed) {
        const uint8_t off = 0;
        uint8_t f[8];
        sdr_modem_bench_write(f, rdof_mmdvm_frame(RDOF_MMDVM_DMR_START, &off, 1, f, sizeof f));
        J.ob_unkeyed = true;
    }
    xSemaphoreGive(mu);
}

void bench_tx_ob_close(void)
{
    xSemaphoreTake(mu, portMAX_DELAY);
    if (J.on && J.ob) {
        const double t = job_utc(NULL);
        job_keying(NULL, J.ob_start, t - J.ob_start, (int)((t - J.ob_start) / 0.03), "outbound");
        job_end();
    }
    xSemaphoreGive(mu);
}

/* A signal mode's transmission: the outbound's way to the modem (a roamer's parked), then the source keyed. */
bool bench_tx_sig_open(uint32_t freq, int power, double seconds, const rdof_sig_src_t *src, char *why, size_t cap)
{
    why[0] = 0;
    xSemaphoreTake(mu, portMAX_DELAY);
    const char *no = NULL;
    const int level = power < policy.rf_level ? power : policy.rf_level;
    const rdof_fe_caps_t *c = sdr_fe_caps();
    const double dbm = c ? c->tx_power_min + (c->tx_power_max - c->tx_power_min) * level / 255.0 : 0;
    char w[160] = "";
    if (!bench_tx_enabled())
        no = policy_err[0] ? policy_err : "no transmit frequencies (bench.tx in the settings)";
    else if (!J.job || !J.from_modem)
        no = "no memory for a transmit job";
    else if (J.on)
        no = "the transmitter is busy with another bench job";
    else if (seconds > policy.max_key_s)
        snprintf(w, sizeof w, "%.1f s is longer than bench.tx's max_key_s %.1f s", seconds, policy.max_key_s), no = w;
    else if ((no = rdof_benchtx_gates(&policy, true, true, freq, &freq, 1, armed())) == NULL &&
             (no = sdr_modem_bench_open(modem_bytes, NULL, dbm, true)) == NULL) {
        if (sdr_modem_bench_signal(freq, src, w, sizeof w)) {
            no = w;
            sdr_modem_bench_close();
        } else {
            StreamBufferHandle_t sb = J.from_modem;
            rdof_benchtx_t *job = J.job;
            J = (txjob_t){.on = true, .line = true, .ob = true, .sig = true, .fd = -1, .freq = freq, .job = job,
                          .from_modem = sb, .ob_start = job_utc(NULL)};
            snprintf(J.session, sizeof J.session, "line");
            snprintf(J.label, sizeof J.label, "signal");
            snprintf(J.via, sizeof J.via, "modem");
            xStreamBufferReset(J.from_modem);
            if (power > level)
                snprintf(why, cap, "power %d is over bench.tx's rf_level: %d (%.0f dBm)", power, level, dbm);
            ESP_LOGW(TAG, "TX: a signal on %.5f MHz, %.1f s, level %d (%.0f dBm)", freq / 1e6, seconds, level, dbm);
        }
    }
    xSemaphoreGive(mu);
    if (no)
        snprintf(why, cap, "%s", no);
    return !no;
}

int bench_tx_sig_state(char *why, size_t cap)
{
    xSemaphoreTake(mu, portMAX_DELAY);
    int r;
    if (!J.on || !J.sig)
        r = -1, snprintf(why, cap, "the signal's modem was let go");
    else if (J.why[0])
        r = -1, snprintf(why, cap, "%s", J.why);
    else if ((r = sdr_modem_bench_signal_state()) < 1)
        r = -1, snprintf(why, cap, "the SDR modem stopped sending");
    xSemaphoreGive(mu);
    return r;
}

void bench_tx_sig_close(void)
{
    xSemaphoreTake(mu, portMAX_DELAY);
    if (J.on && J.sig) {
        char w[8];
        sdr_modem_bench_signal(0, NULL, w, sizeof w);
        const double t = job_utc(NULL);
        job_keying(NULL, J.ob_start, t - J.ob_start, 0, "signal");
        job_end();
    }
    xSemaphoreGive(mu);
}

double bench_tx_sig_rate(void)
{
    const rdof_fe_caps_t *c = sdr_fe_caps();
    return c && c->tx_rate > 0 ? c->tx_rate : g_scfg.sample_rate;
}

/* A voice, csbk or data job's --family: through the router the pool's transmitter frames each burst with MS sync, so
 * only MS goes as asked; the SDR modem itself sends the burst as built. */
const char *bench_tx_why_family(int family)
{
    static char why[128];
    if (family == RDOF_FAMILY_MS || !via_router())
        return NULL;
    snprintf(why, sizeof why, "the router's transmitter frames the bursts with MS sync (%s isn't done through it)",
             family == RDOF_FAMILY_BS ? "bs" : "a direct mode family");
    return why;
}

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
bool bench_tx_listed(uint32_t freq) { return false; }
int bench_tx_freqs(uint32_t *out, int max) { return 0; }
const char *bench_tx_line_why(void) { return "this build has no bench TX (CONFIG_RDOF_BENCH_TX)"; }
bool bench_tx_line_open(uint32_t freq, int cc, int power, char *why, size_t cap)
{
    snprintf(why, cap, "%s", bench_tx_line_why());
    return false;
}
int bench_tx_line_space(char *why, size_t cap) { return -1; }
void bench_tx_line_push(uint8_t control, const uint8_t burst[33]) {}
bool bench_tx_line_idle(void) { return true; }
void bench_tx_line_close(void) {}
int bench_tx_register(void *server) { return 0; }
bool bench_tx_ob_open(uint32_t freq, int cc, int power, double seconds, char *why, size_t cap)
{
    snprintf(why, cap, "%s", bench_tx_line_why());
    return false;
}
const char *bench_tx_ob_why(void) { return bench_tx_line_why(); }
int bench_tx_ob_check(char *why, size_t cap) { return -1; }
bool bench_tx_sig_open(uint32_t freq, int power, double seconds, const rdof_sig_src_t *src, char *why, size_t cap)
{
    snprintf(why, cap, "%s", bench_tx_line_why());
    return false;
}
int bench_tx_sig_state(char *why, size_t cap) { return -1; }
void bench_tx_sig_close(void) {}
double bench_tx_sig_rate(void) { return 48000; }
void bench_tx_ob_unkey(void) {}
void bench_tx_ob_close(void) {}
const char *bench_tx_why_family(int family) { return NULL; }

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
    const bool routed = via_router(); /* version 3: through the router's pool */
    ADD(",\"built\":true,\"via\":[");
#else
    const bool routed = false;
    ADD(",\"built\":false,\"via\":[");
#endif
    const char *why = routed ? NULL : sdr_modem_bench_why();
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
    ADD(",\"router\":%s", routed ? "true" : "false");
    if (policy_err[0])
        ADD(",\"error\":\"%s\"", policy_err);
    ADD("}");
#undef ADD
    return n < cap ? n : 0;
}

#endif
