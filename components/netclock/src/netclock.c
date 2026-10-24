// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
// netclock on ESP-IDF: the clock behind a lock, NVS, the NTP task (sockets,
// DNS) and the die temperature. The maths are in netclock_core.c,
// netclock_ntp.c, netclock_model.c and netclock_select.c. Ported from
// smartlaps' timebase.c and ntp_client.c.
#include "netclock.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "netclock_core.h"
#include "netclock_ntp.h"
#include "nvs.h"
#include "sdkconfig.h"
#if CONFIG_NETCLOCK_DIE_TEMP
#include "driver/temperature_sensor.h"
#endif
#if CONFIG_NETCLOCK_PPS_GPIO >= 0
#include "driver/gpio.h"
#include "esp_attr.h"
#endif

static const char *TAG = "netclock";

#define NTP_PORT      123
#define ROUND_WAIT_MS 1500  // replies later than this are dropped
#define IBURST_GAP_MS 2000
#define RESOLVE_EVERY 48    // re-resolve names every this many polls (pool members come and go)
#define KEY_SERVERS   "servers"
#define KEY_POLL      "poll_s"
#define KEY_RATE      "rate_ppb"
#define KEY_TCAL      "tcal"          // nc_tcal_t: the rate against the die's temperature
#define TEMP_EVERY_US 30000000        // the rate follows the die this often between references

// The clock: readers take s_mux only (a copy of a few hundred bytes at most);
// a writer works on a scratch copy under s_wlock, then swaps it in under s_mux.
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static nc_clock_t s_clock;
static nc_clock_t *s_scratch_p;  // in PSRAM when there is some
#define s_scratch (*s_scratch_p)
static SemaphoreHandle_t s_wlock;
static volatile netclock_source_t s_source;

// NTP: s_src belongs to the task; s_view is what the status shows (under s_mux).
static nc_src_t s_src[NETCLOCK_MAX_SRC];
static int s_n;
static netclock_src_view_t s_view[NETCLOCK_MAX_SRC];
static int s_nview;
static int64_t s_view_t;
static netclock_settings_t s_set;
static bool s_set_stored;
static volatile bool s_servers_changed;
static volatile uint32_t s_poll_s, s_rounds, s_no_majority;
static TaskHandle_t s_task;
static const char *(*s_name_map)(const char *);

#if CONFIG_NETCLOCK_DIE_TEMP
static temperature_sensor_handle_t s_tsens;
#endif

static float die_temp(void)
{
#if CONFIG_NETCLOCK_DIE_TEMP
    float c;
    return s_tsens && temperature_sensor_get_celsius(s_tsens, &c) == ESP_OK ? c : NAN;
#else
    return NAN;
#endif
}

// The rate, and the table when it was learned into (tcal: NULL to leave it).
static void rate_store(double rate, const nc_tcal_t *tcal)
{
    nvs_handle_t h;
    if (nvs_open(CONFIG_NETCLOCK_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_i32(h, KEY_RATE, (int32_t)lround(rate * 1e9)) == ESP_OK &&
        (!tcal || nvs_set_blob(h, KEY_TCAL, tcal, sizeof(*tcal)) == ESP_OK) && nvs_commit(h) == ESP_OK) {
        ESP_LOGI(TAG, "stored clock rate %+.2f ppm%s", rate * 1e6, tcal ? " and the temperature table" : "");
    }
    nvs_close(h);
}

// A reference from src: the clock read utc_us at esp_timer time t_us. *ignored (if given): a better source
// leads (nc_clock_takes), so it wasn't taken.
static nc_result_t add_reference(netclock_source_t src, int64_t t_us, int64_t utc_us, const char *peer, bool *ignored)
{
    float die = die_temp();
    xSemaphoreTake(s_wlock, portMAX_DELAY);
    taskENTER_CRITICAL(&s_mux);
    s_scratch = s_clock;
    taskEXIT_CRITICAL(&s_mux);
    bool skip = !nc_clock_takes(&s_scratch, src, t_us);
    if (ignored) {
        *ignored = skip;
    }
    if (skip) {
        xSemaphoreGive(s_wlock);
        return NC_OUTLIER;
    }
    bool store = false;
    nc_result_t r = nc_clock_add(&s_scratch, src, t_us, utc_us, peer, die, &store);
    bool tcal = s_scratch.tcal_changed;
    static nc_tcal_t tcal_copy;  // under s_wlock
    if (tcal) {
        tcal_copy = s_scratch.tcal;
        const nc_tcal_entry_t *e = nc_tcal_find(&tcal_copy, s_scratch.base_die_c);
        ESP_LOGI(TAG, "learned %+.2f ppm at %.1f C: the %d C bin is %+.2f over %d, %d temperatures in the table",
                 s_scratch.m.fit_rate * 1e6, s_scratch.base_die_c, e ? e->temp_c : 0, e ? e->v : NAN, e ? e->n : 0,
                 tcal_copy.count);
    }
    if (store) {
        nc_clock_stored(&s_scratch, s_scratch.m.fit_rate, t_us);
    }
    const nc_model_t m = s_scratch.m;
    bool from_store = s_scratch.rate_from_store;
    taskENTER_CRITICAL(&s_mux);
    s_clock = s_scratch;
    taskEXIT_CRITICAL(&s_mux);
    s_source = src;
    ESP_LOG_LEVEL(src == NETCLOCK_SRC_PPS && r == NC_SLEW ? ESP_LOG_DEBUG : ESP_LOG_INFO, TAG,
                  "%s%s%s ref: %s, err %+.1f ms, rate %+.2f ppm (%s, %d refs over %.0f min, rms %.1f ms), die %.1f C",
             netclock_source_name(src), peer ? " " : "", peer ? peer : "",
             r == NC_STEP ? "step" : r == NC_SLEW ? "slew" : "OUTLIER, ignored", m.last_err_us / 1000.0,
             m.rate * 1e6, m.rate_learned ? "learned" : from_store ? "stored" : "none", m.n, m.rate_span_s / 60,
             m.resid_rms_us / 1000, die);
    if (store) {
        rate_store(m.fit_rate, tcal ? &tcal_copy : NULL);
    }
    xSemaphoreGive(s_wlock);
#if CONFIG_NETCLOCK_SET_SYSTEM_TIME
    if (r == NC_STEP) {
        // Keep the system clock (log stamps, time()) roughly right; stamps come from the model.
        struct timeval tv = {.tv_sec = utc_us / 1000000, .tv_usec = utc_us % 1000000};
        settimeofday(&tv, NULL);
    }
#endif
    return r;
}

// Between references: the rate follows the die's temperature (nc_clock_temp). Skipped while a reference is going in.
static void temp_tick(void *arg)
{
    float die = die_temp();
    if (isnan(die) || xSemaphoreTake(s_wlock, 0) != pdTRUE) {
        return;
    }
    taskENTER_CRITICAL(&s_mux);
    s_scratch = s_clock;
    taskEXIT_CRITICAL(&s_mux);
    nc_clock_temp(&s_scratch, esp_timer_get_time(), die);
    taskENTER_CRITICAL(&s_mux);
    s_clock = s_scratch;
    taskEXIT_CRITICAL(&s_mux);
    xSemaphoreGive(s_wlock);
}

static bool offset_vs_model(void *ctx, int64_t t_us, int64_t utc_us, double *off_us)
{
    taskENTER_CRITICAL(&s_mux);
    bool ok = nc_clock_offset(&s_clock, t_us, utc_us, off_us);
    taskEXIT_CRITICAL(&s_mux);
    return ok;
}

int64_t netclock_utc_ns(int64_t t_us, bool *synced)
{
    int64_t u = 0;
    taskENTER_CRITICAL(&s_mux);
    bool valid = nc_clock_utc(&s_clock, t_us, &u);
    taskEXIT_CRITICAL(&s_mux);
    if (synced) {
        *synced = valid;
    }
    if (valid) {
        return u * 1000;
    }
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t now_us = esp_timer_get_time();
    return ((int64_t)tv.tv_sec * 1000000 + tv.tv_usec - (now_us - t_us)) * 1000;
}

int64_t netclock_now_ns(void)
{
    return netclock_utc_ns(esp_timer_get_time(), NULL);
}

bool netclock_rate(double *ppm, double *skew_ppm, bool *learned)
{
    double r = 0, k = 0;
    taskENTER_CRITICAL(&s_mux);
    bool ok = nc_clock_rate(&s_clock, &r, &k, learned);
    taskEXIT_CRITICAL(&s_mux);
    *ppm = r * 1e6;
    *skew_ppm = k * 1e6;
    return ok;
}

bool netclock_quality(netclock_source_t *source, double *err_us)
{
    const int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_mux);
    bool ok = nc_clock_quality(&s_clock, now, err_us);
    netclock_source_t src = s_clock.source;
    taskEXIT_CRITICAL(&s_mux);
    if (source) {
        *source = src;
    }
    return ok;
}

float netclock_die_c(void)
{
    return die_temp();
}

netclock_source_t netclock_source(void)
{
    return s_source;
}

bool netclock_synced(void)
{
    return s_source != NETCLOCK_SRC_NONE;
}

bool netclock_set_from_client(int64_t utc_us)
{
    bool ignored = false;
    add_reference(NETCLOCK_SRC_BROWSER, esp_timer_get_time(), utc_us, NULL, &ignored);
    return !ignored;
}

// ── PPS ──────────────────────────────────────────────────────────────────────

#if CONFIG_NETCLOCK_PPS_GPIO >= 0
#ifdef CONFIG_NETCLOCK_PPS_FALLING
#define PPS_FALLING 1
#else
#define PPS_FALLING 0
#endif

static QueueHandle_t s_pps_q;
static nc_pps_t s_pps_view;          // the task's counters, for the status (under s_mux)
static volatile int64_t s_pps_last_us;
static int64_t s_hint_t, s_hint_sec; // the receiver's latest label for an edge, for the task (under s_mux)
static bool s_hint_new;

// Stamped in the interrupt: esp_timer is good to a microsecond or two here.
static void IRAM_ATTR pps_isr(void *arg)
{
    int64_t t = esp_timer_get_time();
    BaseType_t woke = pdFALSE;
    xQueueSendFromISR(s_pps_q, &t, &woke);
    if (woke) {
        portYIELD_FROM_ISR();
    }
}

static void pps_task(void *arg)
{
    nc_pps_t pps;
    nc_pps_init(&pps);
    for (;;) {
        int64_t t;
        if (xQueueReceive(s_pps_q, &t, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        s_pps_last_us = t;
        int64_t u = 0;
        taskENTER_CRITICAL(&s_mux);
        if (s_hint_new) {
            nc_pps_hint(&pps, s_hint_t, s_hint_sec);
            s_hint_new = false;
        }
        bool ok = nc_clock_utc(&s_clock, t, &u);
        taskEXIT_CRITICAL(&s_mux);
        int64_t ref;
        bool take = nc_pps_edge(&pps, t, ok, u, &ref);
        taskENTER_CRITICAL(&s_mux);
        s_pps_view = pps;
        taskEXIT_CRITICAL(&s_mux);
        if (take) {
            add_reference(NETCLOCK_SRC_PPS, t, ref, NULL, NULL);
        }
    }
}

static void pps_start(void)
{
    s_pps_q = xQueueCreate(4, sizeof(int64_t));
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_NETCLOCK_PPS_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,  // no receiver fitted: no edges, not noise
        .intr_type = PPS_FALLING ? GPIO_INTR_NEGEDGE : GPIO_INTR_POSEDGE,
    };
    esp_err_t e = gpio_config(&io);
    if (e == ESP_OK) {
        e = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
        if (e == ESP_ERR_INVALID_STATE) {
            e = ESP_OK;  // the app installed it
        }
    }
    if (e == ESP_OK) {
        e = gpio_isr_handler_add(CONFIG_NETCLOCK_PPS_GPIO, pps_isr, NULL);
    }
    if (e != ESP_OK || !s_pps_q) {
        ESP_LOGE(TAG, "PPS on GPIO%d: %s", CONFIG_NETCLOCK_PPS_GPIO, esp_err_to_name(e));
        return;
    }
#if CONFIG_NETCLOCK_TASK_CORE < 0
    BaseType_t core = tskNO_AFFINITY;
#else
    BaseType_t core = CONFIG_NETCLOCK_TASK_CORE;
#endif
    xTaskCreatePinnedToCore(pps_task, "netclock_pps", 4096, NULL, CONFIG_NETCLOCK_TASK_PRIORITY, NULL, core);
    ESP_LOGI(TAG, "PPS on GPIO%d (%s edge)", CONFIG_NETCLOCK_PPS_GPIO, PPS_FALLING ? "falling" : "rising");
}
#endif

// ── GNSS NMEA time ───────────────────────────────────────────────────────────

static uint32_t s_gnss_times, s_gnss_refs;
static int64_t s_gnss_last_us;
static double s_gnss_lag_us = NAN;  // the burst's start after its pulse, averaged

void netclock_gnss_time(int64_t start_us, int64_t utc_s)
{
    s_gnss_times++;
    s_gnss_last_us = start_us;
#if CONFIG_NETCLOCK_PPS_GPIO >= 0
    // The burst follows the pulse that started its second: that edge is utc_s.
    const int64_t e = s_pps_last_us, lag = start_us - e;
    if (e && lag > 0 && lag < 1000000) {
        s_gnss_lag_us = isnan(s_gnss_lag_us) ? (double)lag : s_gnss_lag_us + ((double)lag - s_gnss_lag_us) / 16;
        taskENTER_CRITICAL(&s_mux);
        s_hint_t = e;
        s_hint_sec = utc_s;
        s_hint_new = true;
        taskEXIT_CRITICAL(&s_mux);
        return;
    }
#endif
    // No pulse: the burst itself, less its usual lag behind the second, every NETCLOCK_GNSS_EVERY seconds.
    if ((s_gnss_times - 1) % CONFIG_NETCLOCK_GNSS_EVERY) {
        return;
    }
    const double lag_us = isnan(s_gnss_lag_us) ? CONFIG_NETCLOCK_GNSS_LAG_MS * 1000.0 : s_gnss_lag_us;
    bool ignored = false;
    add_reference(NETCLOCK_SRC_GNSS, start_us - (int64_t)lag_us, utc_s * 1000000, NULL, &ignored);
    if (!ignored) {
        s_gnss_refs++;
    }
}

void netclock_status(netclock_status_t *out)
{
    int64_t now = esp_timer_get_time();
    memset(out, 0, sizeof(*out));
    xSemaphoreTake(s_wlock, portMAX_DELAY);
    taskENTER_CRITICAL(&s_mux);
    s_scratch = s_clock;
    out->n_src = s_nview;
    memcpy(out->src, s_view, sizeof(s_view[0]) * s_nview);
    int64_t view_t = s_view_t;
    taskEXIT_CRITICAL(&s_mux);
    nc_clock_status(&s_scratch, now, out);
    xSemaphoreGive(s_wlock);
    if (!out->valid) {
        out->utc_us = netclock_utc_ns(now, NULL) / 1000;
    }
    for (int i = 0; i < out->n_src; i++) {
        // The view was taken at the end of the last poll: age "last rx" to now.
        if (out->src[i].last_rx_s >= 0) {
            out->src[i].last_rx_s += (int32_t)((now - view_t) / 1000000);
        }
    }
    out->die_c = die_temp();
    out->poll_s = s_poll_s;
    out->polls = s_rounds;
    out->no_majority = s_no_majority;
    out->gnss_times = s_gnss_times;
    out->gnss_refs = s_gnss_refs;
    out->gnss_lag_ms = (float)(s_gnss_lag_us / 1000);
    out->gnss_last_s = s_gnss_last_us ? (int32_t)((now - s_gnss_last_us) / 1000000) : -1;
    out->pps_gpio = -1;
#if CONFIG_NETCLOCK_PPS_GPIO >= 0
    out->pps_gpio = CONFIG_NETCLOCK_PPS_GPIO;
    taskENTER_CRITICAL(&s_mux);
    nc_pps_t pps = s_pps_view;
    taskEXIT_CRITICAL(&s_mux);
    out->pps_edges = pps.edges;
    out->pps_bad = pps.bad;
    out->pps_unlabeled = pps.unlabeled;
    out->pps_last_s = s_pps_last_us ? (int32_t)((now - s_pps_last_us) / 1000000) : -1;
    out->pps_by_gnss = pps.by_hint;
#endif
}

// ── NTP ──────────────────────────────────────────────────────────────────────

static void load_servers(void)
{
    char list[NETCLOCK_SERVERS_LEN];
    taskENTER_CRITICAL(&s_mux);
    memcpy(list, s_set.servers, sizeof(list));
    taskEXIT_CRITICAL(&s_mux);
    s_n = nc_parse_servers(list, s_src, NETCLOCK_MAX_SRC);
    if (s_name_map) {
        for (int i = 0; i < s_n; i++) {
            const char *m = s_name_map(s_src[i].name);
            if (m && m != s_src[i].name) {
                snprintf(s_src[i].name, sizeof(s_src[i].name), "%s", m);
            }
        }
    }
    ESP_LOGI(TAG, "%d NTP servers from \"%s\"", s_n, list);
}

static void resolve(nc_src_t *s)
{
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM}, *res = NULL;
    if (getaddrinfo(s->name, "123", &hints, &res) != 0 || !res) {
        ESP_LOGW(TAG, "%s: DNS failed", s->name);
        s->resolved = false;
        return;
    }
    uint8_t ip[4];
    memcpy(ip, &((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr, 4);
    freeaddrinfo(res);
    if (!s->resolved || memcmp(ip, s->ip, 4)) {
        s->hn = s->hh = 0;  // a different server now: its history isn't this one's
    }
    memcpy(s->ip, ip, 4);
    s->resolved = true;
}

static void send_req(int sock, nc_src_t *s, uint32_t poll_s)
{
    uint8_t p[48];
    s->cookie = ((uint64_t)esp_random() << 32) | esp_random();
    nc_ntp_request(p, s->cookie, poll_s);
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(NTP_PORT)};
    memcpy(&to.sin_addr.s_addr, s->ip, 4);
    s->got = false;
    s->t1 = esp_timer_get_time();
    if (sendto(sock, p, sizeof(p), 0, (struct sockaddr *)&to, sizeof(to)) != sizeof(p)) {
        s->t1 = 0;
    }
}

static void publish_view(void)
{
    int64_t now = esp_timer_get_time();
    static netclock_src_view_t v[NETCLOCK_MAX_SRC];
    for (int i = 0; i < s_n; i++) {
        nc_src_view(&s_src[i], now, &v[i]);
    }
    taskENTER_CRITICAL(&s_mux);
    memcpy(s_view, v, sizeof(v[0]) * s_n);
    s_nview = s_n;
    s_view_t = now;
    taskEXIT_CRITICAL(&s_mux);
}

static uint32_t poll_interval(void)
{
    uint32_t slow = s_set.poll_s ? s_set.poll_s : CONFIG_NETCLOCK_POLL_S;
    uint32_t fast = CONFIG_NETCLOCK_POLL_FAST_S < slow ? CONFIG_NETCLOCK_POLL_FAST_S : slow;
    taskENTER_CRITICAL(&s_mux);
    bool settled = nc_clock_rate_settled(&s_clock);
    taskEXIT_CRITICAL(&s_mux);
    return settled ? slow : fast;
}

static void ntp_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket failed");
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    for (;;) {
        if (s_servers_changed) {
            s_servers_changed = false;
            load_servers();
            s_rounds = 0;  // iburst again
            publish_view();
        }
        for (int i = 0; i < s_n; i++) {
            nc_src_t *s = &s_src[i];
            if (!s->resolved || s->misses >= 4 || s_rounds % RESOLVE_EVERY == 0) {
                resolve(s);
                s->misses = 0;
            }
        }
        // Two names on one address are one server.
        for (int i = 0; i < s_n; i++) {
            s_src[i].dup = false;
            for (int k = 0; k < i; k++) {
                if (s_src[k].resolved && s_src[i].resolved && !memcmp(s_src[k].ip, s_src[i].ip, 4)) {
                    s_src[i].dup = true;
                }
            }
        }
        int sent = 0;
        for (int i = 0; i < s_n; i++) {
            nc_src_t *s = &s_src[i];
            s->got = false;
            s->t1 = 0;
            if (!s->resolved || s->dup) {
                continue;
            }
            if (s->kod_skip > 0) {
                s->kod_skip--;
                continue;
            }
            send_req(sock, s, s_poll_s);
            sent += s->t1 != 0;
        }
        int64_t deadline = esp_timer_get_time() + ROUND_WAIT_MS * 1000LL;
        int got = 0;
        bool iburst = s_rounds > 0 && s_rounds < NC_NTP_IBURST;
        while (sent && got < sent && esp_timer_get_time() < deadline) {
            uint8_t p[68];
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            int len = recvfrom(sock, p, sizeof(p), 0, (struct sockaddr *)&from, &fl);
            int64_t t4 = esp_timer_get_time();
            if (len <= 0) {
                continue;
            }
            for (int i = 0; i < s_n; i++) {
                nc_src_t *s = &s_src[i];
                if (s->resolved && !s->dup && !s->got && !memcmp(&from.sin_addr.s_addr, s->ip, 4)) {
                    if (nc_src_reply(s, p, len, t4, iburst) == NC_REPLY_KOD) {
                        ESP_LOGW(TAG, "%s: kiss-o'-death, backing off", s->name);
                    }
                    got += s->got;
                    break;
                }
            }
        }
        for (int i = 0; i < s_n; i++) {
            nc_src_poll_end(&s_src[i]);
        }
        if (s_rounds >= NC_NTP_IBURST - 1) {
            int64_t t = 0, u = 0;
            bool had = false;
            int bi = nc_combine(s_src, s_n, offset_vs_model, NULL, &t, &u, &had);
            if (bi >= 0) {
                add_reference(NETCLOCK_SRC_NTP, t, u, s_src[bi].name, NULL);
            } else if (had) {
                s_no_majority++;
                ESP_LOGW(TAG, "no majority among NTP servers; clock not updated this poll");
            } else if (s_n) {
                ESP_LOGI(TAG, "no usable NTP sample this poll (delay spikes or no replies)");
            }
        }
        publish_view();
        s_rounds++;
        s_poll_s = poll_interval();
        // Sleep the poll, waking early for a new server list.
        ulTaskNotifyTake(pdTRUE, s_rounds < NC_NTP_IBURST ? pdMS_TO_TICKS(IBURST_GAP_MS) : pdMS_TO_TICKS(s_poll_s * 1000));
    }
}

void netclock_start(void)
{
    if (s_task || !s_wlock) {
        return;
    }
    s_servers_changed = true;
#if CONFIG_NETCLOCK_TASK_CORE < 0
    BaseType_t core = tskNO_AFFINITY;
#else
    BaseType_t core = CONFIG_NETCLOCK_TASK_CORE;
#endif
    // 6 KB: getaddrinfo and float logging are stack hungry.
    xTaskCreatePinnedToCore(ntp_task, "netclock", 6144, NULL, CONFIG_NETCLOCK_TASK_PRIORITY, &s_task, core);
}

#if CONFIG_NETCLOCK_START_ON_IP
static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_STA_GOT_IP || id == IP_EVENT_ETH_GOT_IP || id == IP_EVENT_PPP_GOT_IP) {
        netclock_start();
    }
}
#endif

// ── settings ─────────────────────────────────────────────────────────────────

static bool check_servers(const char *s, char *err, size_t cap)
{
    if (strlen(s) >= NETCLOCK_SERVERS_LEN) {
        snprintf(err, cap, "servers: at most %d characters", NETCLOCK_SERVERS_LEN - 1);
        return false;
    }
    for (const char *p = s; *p; p++) {
        if (!isalnum((unsigned char)*p) && !strchr(".-_:, ;", *p)) {
            snprintf(err, cap, "servers: names or addresses, space or comma separated");
            return false;
        }
    }
    return true;
}

esp_err_t netclock_set_settings(const netclock_settings_t *in, char *err, size_t cap)
{
    char dummy[8];
    if (!err) {
        err = dummy;
        cap = sizeof(dummy);
    }
    netclock_settings_t s = *in;
    s.servers[sizeof(s.servers) - 1] = 0;
    if (!check_servers(s.servers, err, cap)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s.poll_s && (s.poll_s < 16 || s.poll_s > 3600)) {
        snprintf(err, cap, "poll_s: 16 to 3600");
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t e = nvs_open(CONFIG_NETCLOCK_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = nvs_set_str(h, KEY_SERVERS, s.servers);
        if (e == ESP_OK) {
            e = nvs_set_u32(h, KEY_POLL, s.poll_s);
        }
        if (e == ESP_OK) {
            e = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (e != ESP_OK) {
        snprintf(err, cap, "NVS: %s", esp_err_to_name(e));
        return e;
    }
    taskENTER_CRITICAL(&s_mux);
    bool same = !strcmp(s_set.servers, s.servers);
    s_set = s;
    taskEXIT_CRITICAL(&s_mux);
    s_set_stored = true;
    ESP_LOGI(TAG, "settings: servers \"%s\", poll %lu s", s.servers, (unsigned long)s.poll_s);
    if (!same) {
        s_servers_changed = true;
        if (s_task) {
            xTaskNotifyGive(s_task);
        }
    }
    return ESP_OK;
}

void netclock_get_settings(netclock_settings_t *out)
{
    taskENTER_CRITICAL(&s_mux);
    *out = s_set;
    taskEXIT_CRITICAL(&s_mux);
    if (!out->poll_s) {
        out->poll_s = CONFIG_NETCLOCK_POLL_S;
    }
}

bool netclock_settings_stored(void)
{
    return s_set_stored;
}

void netclock_set_name_map(const char *(*map)(const char *name))
{
    s_name_map = map;
}

esp_err_t netclock_init(void)
{
    if (s_wlock) {
        return ESP_OK;
    }
    s_scratch_p = heap_caps_malloc(sizeof(nc_clock_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_scratch_p) {
        s_scratch_p = malloc(sizeof(nc_clock_t));
    }
    s_wlock = xSemaphoreCreateMutex();
    if (!s_wlock || !s_scratch_p) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(s_set.servers, sizeof(s_set.servers), "%s", CONFIG_NETCLOCK_DEFAULT_SERVERS);
    s_set.poll_s = CONFIG_NETCLOCK_POLL_S;
    s_poll_s = CONFIG_NETCLOCK_POLL_FAST_S;
    nvs_handle_t h;
    int32_t ppb = 0;
    bool have_rate = false;
    static nc_tcal_t tcal;  // not on the caller's stack
    nc_tcal_init(&tcal);
    if (nvs_open(CONFIG_NETCLOCK_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        have_rate = nvs_get_i32(h, KEY_RATE, &ppb) == ESP_OK;
        size_t tlen = sizeof(tcal);
        if (nvs_get_blob(h, KEY_TCAL, &tcal, &tlen) != ESP_OK || tlen != sizeof(tcal) || !nc_tcal_valid(&tcal)) {
            nc_tcal_init(&tcal);
        }
        size_t len = sizeof(s_set.servers);
        if (nvs_get_str(h, KEY_SERVERS, s_set.servers, &len) == ESP_OK) {
            s_set_stored = true;
        }
        uint32_t poll;
        if (nvs_get_u32(h, KEY_POLL, &poll) == ESP_OK && poll) {
            s_set.poll_s = poll;
        }
        nvs_close(h);
    }
#if CONFIG_NETCLOCK_DIE_TEMP
    temperature_sensor_config_t tc = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&tc, &s_tsens) != ESP_OK || temperature_sensor_enable(s_tsens) != ESP_OK) {
        s_tsens = NULL;
    }
#endif
    nc_clock_init(&s_clock, ppb / 1e9, have_rate);
    float die = die_temp();
    nc_clock_set_tcal(&s_clock, &tcal, die);
    if (!isnan(s_clock.base_die_c)) {
        ESP_LOGI(TAG, "clock rate prior %+.2f ppm (the temperature table, %d temperatures, at %.1f C); servers \"%s\"",
                 s_clock.m.rate * 1e6, tcal.count, die, s_set.servers);
    } else {
        ESP_LOGI(TAG, "clock rate prior %+.2f ppm (%s); servers \"%s\"", ppb / 1e3, have_rate ? "stored" : "none",
                 s_set.servers);
    }
    static esp_timer_handle_t temp_timer;
    if (!isnan(die) && !temp_timer) {
        const esp_timer_create_args_t ta = {.callback = temp_tick, .name = "nc_temp"};
        if (esp_timer_create(&ta, &temp_timer) == ESP_OK) {
            esp_timer_start_periodic(temp_timer, TEMP_EVERY_US);
        }
    }
#if CONFIG_NETCLOCK_PPS_GPIO >= 0
    pps_start();
#endif
#if CONFIG_NETCLOCK_START_ON_IP
    esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        return e;
    }
    return esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_ip, NULL);
#else
    return ESP_OK;
#endif
}
