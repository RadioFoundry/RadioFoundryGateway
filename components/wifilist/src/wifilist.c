// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
// wifilist on ESP-IDF: the station task (scan, rank, connect, fall back,
// rescan on loss), the setup access point, NVS. The choices are in
// wifilist_core.c.
#include "wifilist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "wifilist";

#define BIT_SCAN_DONE BIT0
#define BIT_GOT_IP    BIT1
#define BIT_DISC      BIT2
#define BIT_LIST      BIT3   // the list changed
#define BIT_SCAN_REQ  BIT4
#define BIT_RECONNECT BIT5
#define BIT_AP        BIT6   // time for the setup access point
#define SCAN_MAX      24     // access point records read from a scan
#define NVS_KEY       "nets"

static EventGroupHandle_t s_ev;
static SemaphoreHandle_t s_lock;  // everything below that the status reads
static bool s_loaded, s_stored;
static wifilist_net_t s_nets[WIFILIST_MAX];
static int s_n;
static wifilist_config_t s_cfg;
static char s_host[33], s_ap_ssid[33], s_ap_pass[65];
static esp_netif_t *s_sta;
static esp_timer_handle_t s_ap_timer;
static volatile uint8_t s_disc_reason;
static volatile int8_t s_disc_rssi;

static const char *s_state = "starting";
static bool s_connected, s_ap_up;
static char s_ssid[33], s_conn_pass[65], s_ip[16];
static uint8_t s_bssid[6], s_chan;
static int8_t s_rssi_at_connect;
static int64_t s_conn_t;
static uint32_t s_scans, s_connects;
static wifilist_ap_t s_aps[SCAN_MAX];  // the last scan, every record (task only)
static int s_naps;
static wifilist_ap_t s_seen[WIFILIST_SEEN];
static int s_nseen;
static wifilist_fail_t s_fails[WIFILIST_FAILS];
static int s_nfail;

static void lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

// ── storage: a blob of fixed records ─────────────────────────────────────────

static void load(void)
{
    if (s_loaded) {
        return;
    }
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    s_loaded = true;
    nvs_handle_t h;
    if (nvs_open(CONFIG_WIFILIST_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = 0;
    uint8_t *blob = NULL;
    if (nvs_get_blob(h, NVS_KEY, NULL, &len) == ESP_OK && len >= 2 && (blob = malloc(len)) &&
        nvs_get_blob(h, NVS_KEY, blob, &len) == ESP_OK) {
        int n = wifilist_blob_decode(blob, len, s_nets, WIFILIST_MAX);
        if (n >= 0) {  // a damaged blob counts as nothing stored
            s_n = n;
            s_stored = true;
        }
    }
    free(blob);
    nvs_close(h);
}

static esp_err_t store(const wifilist_net_t *nets, int n)
{
    size_t len = wifilist_blob_size(n);
    uint8_t *blob = malloc(len);
    if (!blob) {
        return ESP_ERR_NO_MEM;
    }
    wifilist_blob_encode(nets, n, blob);
    nvs_handle_t h;
    esp_err_t e = nvs_open(CONFIG_WIFILIST_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = nvs_set_blob(h, NVS_KEY, blob, len);
        if (e == ESP_OK) {
            e = nvs_commit(h);
        }
        nvs_close(h);
    }
    free(blob);
    return e;
}

int wifilist_get(wifilist_net_t *out, int max)
{
    load();
    lock();
    int n = s_n < max ? s_n : max;
    memcpy(out, s_nets, sizeof(s_nets[0]) * n);
    unlock();
    return n;
}

esp_err_t wifilist_set(const wifilist_net_t *nets, int n, char *err, size_t cap)
{
    load();
    wifilist_net_t *t = malloc(sizeof(wifilist_net_t) * WIFILIST_MAX);
    if (!t) {
        return ESP_ERR_NO_MEM;
    }
    lock();
    if (!wifilist_check(nets, n, err, cap)) {
        unlock();
        free(t);
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(t, nets, sizeof(t[0]) * n);
    wifilist_sort(t, n);
    esp_err_t e = store(t, n);
    if (e == ESP_OK) {
        memcpy(s_nets, t, sizeof(t[0]) * n);
        s_n = n;
        s_stored = true;
    } else if (err && cap) {
        snprintf(err, cap, "NVS: %s", esp_err_to_name(e));
    }
    unlock();
    free(t);
    if (e == ESP_OK) {
        ESP_LOGI(TAG, "%d networks stored", n);
        if (s_ev) {
            xEventGroupSetBits(s_ev, BIT_LIST);
        }
    }
    return e;
}

bool wifilist_stored(void)
{
    load();
    return s_stored;
}

esp_err_t wifilist_migrate(const char *ssid, const char *password)
{
    if (wifilist_stored()) {
        return ESP_OK;
    }
    wifilist_net_t w = {0};
    int n = 0;
    if (ssid && ssid[0]) {
        snprintf(w.ssid, sizeof(w.ssid), "%s", ssid);
        snprintf(w.password, sizeof(w.password), "%s", password ? password : "");
        n = 1;
    }
    char err[96];
    esp_err_t e = wifilist_set(&w, n, err, sizeof(err));
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "migrating \"%s\": %s", w.ssid, err);
    } else if (n) {
        ESP_LOGI(TAG, "migrated the single network \"%s\" to the list", w.ssid);
    }
    return e;
}

// ── status ───────────────────────────────────────────────────────────────────

static void add_fail(const char *ssid, uint8_t reason, int8_t rssi, bool lost)
{
    lock();
    if (s_nfail == WIFILIST_FAILS) {
        memmove(&s_fails[0], &s_fails[1], sizeof(s_fails) - sizeof(s_fails[0]));
        s_nfail--;
    }
    wifilist_fail_t *f = &s_fails[s_nfail++];
    *f = (wifilist_fail_t){.t_s = (uint32_t)(esp_timer_get_time() / 1000000), .reason = reason, .rssi = rssi,
                           .lost = lost};
    snprintf(f->ssid, sizeof(f->ssid), "%s", ssid);
    unlock();
    ESP_LOGW(TAG, "%s \"%s\": %s (reason %u, rssi %d)", lost ? "lost" : "couldn't connect to", ssid,
             wifilist_reason(reason), reason, rssi);
}

void wifilist_status(wifilist_status_t *out)
{
    load();
    memset(out, 0, sizeof(*out));
    wifi_ap_record_t ap;
    lock();
    bool live = s_connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    out->state = s_state;
    out->connected = s_connected;
    if (s_connected) {
        memcpy(out->ssid, s_ssid, sizeof(out->ssid));
        memcpy(out->bssid, s_bssid, 6);
        out->rssi = live ? ap.rssi : s_rssi_at_connect;
        out->channel = live ? ap.primary : s_chan;
        out->connected_s = (uint32_t)((esp_timer_get_time() - s_conn_t) / 1000000);
    }
    memcpy(out->ip, s_ip, sizeof(out->ip));
    out->ap_up = s_ap_up;
    if (s_ap_up) {
        memcpy(out->ap_ssid, s_ap_ssid, sizeof(out->ap_ssid));
    }
    out->scans = s_scans;
    out->connects = s_connects;
    out->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    out->n_nets = s_n;
    for (int i = 0; i < s_n; i++) {
        memcpy(out->nets[i].ssid, s_nets[i].ssid, sizeof(out->nets[i].ssid));
        out->nets[i].priority = s_nets[i].priority;
        out->nets[i].hidden = s_nets[i].hidden;
        out->nets[i].password_set = s_nets[i].password[0] != 0;
        out->nets[i].rssi = -128;
        for (int j = 0; j < s_nseen; j++) {
            if (!strcmp(s_seen[j].ssid, s_nets[i].ssid)) {
                out->nets[i].rssi = s_seen[j].rssi;
                break;
            }
        }
    }
    out->n_seen = s_nseen;
    memcpy(out->seen, s_seen, sizeof(s_seen[0]) * s_nseen);
    out->n_fail = s_nfail;
    memcpy(out->fails, s_fails, sizeof(s_fails[0]) * s_nfail);
    unlock();
}

bool wifilist_connected(void)
{
    load();
    lock();
    bool c = s_connected;
    unlock();
    return c;
}

esp_netif_t *wifilist_sta_netif(void)
{
    return s_sta;
}

void wifilist_scan(void)
{
    if (s_ev) {
        xEventGroupSetBits(s_ev, BIT_SCAN_REQ);
    }
}

void wifilist_reconnect(void)
{
    if (s_ev) {
        xEventGroupSetBits(s_ev, BIT_RECONNECT);
    }
}

size_t wifilist_export_json(char *buf, size_t cap)
{
    load();
    lock();
    size_t len = wifilist_networks_json(s_nets, s_n, buf, cap);
    unlock();
    return len;
}

// ── the setup access point ───────────────────────────────────────────────────

static void ap_up(void)
{
#if CONFIG_WIFILIST_SOFTAP
    if (s_ap_up || !s_cfg.ap_ssid) {
        return;
    }
    wifi_config_t ap = {0};
    memcpy(ap.ap.ssid, s_ap_ssid, strlen(s_ap_ssid));
    ap.ap.ssid_len = (uint8_t)strlen(s_ap_ssid);
    memcpy(ap.ap.password, s_ap_pass, strlen(s_ap_pass));
    ap.ap.authmode = s_ap_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ap.ap.max_connection = s_cfg.ap_max_conn ? s_cfg.ap_max_conn : 2;
    ap.ap.channel = s_connected && s_chan ? s_chan : (s_cfg.ap_channel ? s_cfg.ap_channel : 6);
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_err_t e = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "setup access point not configured: %s", esp_err_to_name(e));
        esp_wifi_set_mode(WIFI_MODE_STA);
        return;
    }
    lock();
    s_ap_up = true;
    unlock();
    ESP_LOGW(TAG, "setup access point %s%s%s: http://192.168.4.1/", s_ap_ssid, s_ap_pass[0] ? " (password " : " (open",
             s_ap_pass[0] ? s_ap_pass : "");
#endif
}

static void ap_down(void)
{
#if CONFIG_WIFILIST_SOFTAP
    if (s_ap_timer) {
        esp_timer_stop(s_ap_timer);
    }
    xEventGroupClearBits(s_ev, BIT_AP);
    if (!s_ap_up || s_cfg.ap_always) {
        return;
    }
    esp_wifi_set_mode(WIFI_MODE_STA);
    lock();
    s_ap_up = false;
    unlock();
    ESP_LOGI(TAG, "setup access point off");
#endif
}

// No connection: the access point after ap_after_ms (at once with no networks).
static void ap_arm(bool now)
{
#if CONFIG_WIFILIST_SOFTAP
    if (s_ap_up || !s_cfg.ap_ssid) {
        return;
    }
    if (now || !s_cfg.ap_after_ms) {
        xEventGroupSetBits(s_ev, BIT_AP);
    } else if (s_ap_timer && !esp_timer_is_active(s_ap_timer)) {
        esp_timer_start_once(s_ap_timer, (uint64_t)s_cfg.ap_after_ms * 1000);
    }
#endif
}

static void ap_timer_cb(void *arg)
{
    xEventGroupSetBits(s_ev, BIT_AP);
}

// ── the station task ─────────────────────────────────────────────────────────

// Wait for any of `bits` (cleared on return; 0 on timeout). Raises the
// access point when it's due, except during a scan.
static EventBits_t wait(EventBits_t bits, TickType_t timeout, bool scanning)
{
    TickType_t start = xTaskGetTickCount();
    for (;;) {
        TickType_t el = xTaskGetTickCount() - start;
        if (timeout != portMAX_DELAY && el >= timeout) {
            return 0;
        }
        EventBits_t got = xEventGroupWaitBits(s_ev, bits | (scanning ? 0 : BIT_AP), pdFALSE, pdFALSE,
                                              timeout == portMAX_DELAY ? portMAX_DELAY : timeout - el);
        if ((got & BIT_AP) && !scanning) {
            xEventGroupClearBits(s_ev, BIT_AP);
            ap_up();
        }
        got &= bits;
        if (got) {
            xEventGroupClearBits(s_ev, got);
            return got;
        }
    }
}

static void set_state(const char *st)
{
    lock();
    s_state = st;
    unlock();
}

static bool do_scan(void)
{
    const char *was = s_state;
    set_state("scanning");
    wifi_scan_config_t sc = {.show_hidden = true};
    xEventGroupClearBits(s_ev, BIT_SCAN_DONE);
    bool ok = esp_wifi_scan_start(&sc, false) == ESP_OK;
    if (ok && !wait(BIT_SCAN_DONE, pdMS_TO_TICKS(10000), true)) {
        esp_wifi_scan_stop();
        ok = false;
    }
    wifi_ap_record_t *recs = ok ? calloc(SCAN_MAX, sizeof(*recs)) : NULL;
    uint16_t n = SCAN_MAX;
    if (!recs || esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) {
        n = 0;
    }
    esp_wifi_clear_ap_list();
    s_naps = 0;
    for (int i = 0; i < n; i++) {
        wifilist_ap_t *a = &s_aps[s_naps++];
        memcpy(a->ssid, recs[i].ssid, 32);
        a->ssid[32] = 0;
        memcpy(a->bssid, recs[i].bssid, 6);
        a->rssi = recs[i].rssi;
        a->channel = recs[i].primary;
        a->open = recs[i].authmode == WIFI_AUTH_OPEN;
    }
    free(recs);
    lock();
    s_scans++;
    s_nseen = 0;
    for (int i = 0; i < s_naps && s_nseen < WIFILIST_SEEN; i++) {
        bool dup = !s_aps[i].ssid[0];
        for (int j = 0; j < s_nseen && !dup; j++) {
            dup = !strcmp(s_seen[j].ssid, s_aps[i].ssid);
        }
        if (!dup) {
            s_seen[s_nseen++] = s_aps[i];  // records come strongest first
        }
    }
    s_state = was;
    unlock();
    ESP_LOGI(TAG, "scan: %d access points", s_naps);
    return ok;
}

static void quiet_disconnect(void)
{
    xEventGroupClearBits(s_ev, BIT_DISC);
    if (esp_wifi_disconnect() == ESP_OK) {
        wait(BIT_DISC, pdMS_TO_TICKS(2000), false);
    }
}

static void set_disconnected(void)
{
    lock();
    s_connected = false;
    s_ip[0] = 0;
    unlock();
}

// 1: connected; 0: failed; -1: interrupted (the list changed, or asked to reconnect).
static int try_connect(const wifilist_net_t *net, const wifilist_ap_t *ap)
{
    wifi_config_t c = {0};
    memcpy(c.sta.ssid, net->ssid, strlen(net->ssid));
    memcpy(c.sta.password, net->password, strlen(net->password));
    c.sta.threshold.authmode = net->password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    c.sta.pmf_cfg.capable = true;
    c.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;  // WPA2, WPA2/WPA3 transition and WPA3-only access points
    c.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    c.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    if (ap) {
        c.sta.bssid_set = true;
        memcpy(c.sta.bssid, ap->bssid, 6);
        c.sta.channel = ap->channel;
    }
    esp_wifi_set_config(WIFI_IF_STA, &c);
    set_state("connecting");
    ESP_LOGI(TAG, "connecting to \"%s\" (priority %d, %s)", net->ssid, net->priority, ap ? "seen" : "hidden");
    xEventGroupClearBits(s_ev, BIT_GOT_IP | BIT_DISC);
    s_disc_reason = 0;
    s_disc_rssi = ap ? ap->rssi : 0;
    if (esp_wifi_connect() != ESP_OK) {
        add_fail(net->ssid, 205, s_disc_rssi, false);
        return 0;
    }
    EventBits_t b = wait(BIT_GOT_IP | BIT_DISC | BIT_LIST | BIT_RECONNECT,
                         pdMS_TO_TICKS(CONFIG_WIFILIST_CONNECT_TIMEOUT_S * 1000), false);
    if (b & BIT_GOT_IP) {
        wifi_ap_record_t info;
        bool have = esp_wifi_sta_get_ap_info(&info) == ESP_OK;
        lock();
        s_connected = true;
        s_conn_t = esp_timer_get_time();
        s_connects++;
        memcpy(s_ssid, net->ssid, sizeof(s_ssid));
        memcpy(s_conn_pass, net->password, sizeof(s_conn_pass));
        memcpy(s_bssid, have ? info.bssid : (ap ? ap->bssid : (const uint8_t[6]){0}), 6);
        s_chan = have ? info.primary : (ap ? ap->channel : 0);
        s_rssi_at_connect = have ? info.rssi : (ap ? ap->rssi : 0);
        s_state = "connected";
        unlock();
        ESP_LOGI(TAG, "connected to \"%s\" as %s (rssi %d, channel %u)", net->ssid, s_ip, s_rssi_at_connect, s_chan);
        ap_down();
        return 1;
    }
    if (b & BIT_DISC) {
        add_fail(net->ssid, s_disc_reason, s_disc_rssi, false);
        return 0;
    }
    quiet_disconnect();
    if (b & (BIT_LIST | BIT_RECONNECT)) {
        return -1;
    }
    add_fail(net->ssid, 0, ap ? ap->rssi : 0, false);  // timed out (association or DHCP)
    return 0;
}

// The network we're on is still in the list with the same password.
static bool still_listed(void)
{
    bool ok = false;
    lock();
    for (int i = 0; i < s_n && !ok; i++) {
        ok = !strcmp(s_nets[i].ssid, s_ssid) && !strcmp(s_nets[i].password, s_conn_pass);
    }
    unlock();
    return ok;
}

static void sta_task(void *arg)
{
    static wifilist_net_t nets[WIFILIST_MAX];
    static wifilist_cand_t cands[WIFILIST_MAX];
    int failed_rounds = 0;
    for (;;) {
        lock();
        int n = s_n;
        memcpy(nets, s_nets, sizeof(nets[0]) * n);
        unlock();
        if (!n) {
            set_state("no networks");
            ap_arm(true);
            if (wait(BIT_LIST | BIT_SCAN_REQ | BIT_RECONNECT, portMAX_DELAY, false) & BIT_SCAN_REQ) {
                do_scan();
            }
            continue;
        }
        do_scan();
        int nc = wifilist_rank(nets, n, s_aps, s_naps, cands, WIFILIST_MAX);
        int r = 0;
        for (int i = 0; i < nc && r == 0; i++) {
            r = try_connect(&nets[cands[i].net], cands[i].ap >= 0 ? &s_aps[cands[i].ap] : NULL);
        }
        if (r == 1) {
            failed_rounds = 0;
            for (;;) {
                EventBits_t b = wait(BIT_DISC | BIT_LIST | BIT_SCAN_REQ | BIT_RECONNECT, portMAX_DELAY, false);
                if (b & BIT_DISC) {
                    add_fail(s_ssid, s_disc_reason, s_disc_rssi, true);
                    break;
                }
                if ((b & BIT_RECONNECT) || ((b & BIT_LIST) && !still_listed())) {
                    quiet_disconnect();
                    break;
                }
                if (b & BIT_SCAN_REQ) {
                    do_scan();
                }
            }
            set_disconnected();
            ap_arm(false);
            continue;
        }
        if (r < 0) {
            continue;
        }
        // Nothing in sight connected: wait, longer each round (always the longest
        // with the access point up: a scan takes the radio off its channel).
        set_state("waiting");
        ap_arm(false);
        uint32_t s = wifilist_backoff_s(failed_rounds, CONFIG_WIFILIST_RESCAN_MAX_S, s_ap_up);
        failed_rounds++;
        ESP_LOGI(TAG, "no network in the list connected (%d in sight); scanning again in %lu s", nc, (unsigned long)s);
        wait(BIT_LIST | BIT_SCAN_REQ | BIT_RECONNECT, pdMS_TO_TICKS(s * 1000), false);
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        xEventGroupSetBits(s_ev, BIT_SCAN_DONE);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        s_disc_reason = d->reason;
        s_disc_rssi = d->rssi;
        xEventGroupSetBits(s_ev, BIT_DISC);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        lock();
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        unlock();
        xEventGroupSetBits(s_ev, BIT_GOT_IP);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        const wifi_event_ap_staconnected_t *e = data;
        ESP_LOGI(TAG, "setup access point: " MACSTR " joined", MAC2STR(e->mac));
    }
}

esp_err_t wifilist_start(const wifilist_config_t *cfg)
{
    if (s_ev) {
        return ESP_ERR_INVALID_STATE;
    }
    load();
    s_cfg = *cfg;
    if (cfg->ap_ssid && !wifilist_ap_password_ok(cfg->ap_password)) {
        /* No setup AP rather than no Wi-Fi: the station side still joins the stored networks. */
        ESP_LOGE(TAG, "ap_password must be empty or 8 to 63 characters: no setup access point");
        s_cfg.ap_ssid = NULL;
    }
    snprintf(s_host, sizeof(s_host), "%s", cfg->hostname ? cfg->hostname : "");
    snprintf(s_ap_pass, sizeof(s_ap_pass), "%s", cfg->ap_password ? cfg->ap_password : "");
    s_cfg.hostname = s_host;
#if CONFIG_WIFILIST_SOFTAP
    if (s_cfg.ap_ssid) {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
        char suffix[5];
        snprintf(suffix, sizeof(suffix), "%02X%02X", mac[4], mac[5]);
        const char *p = strstr(cfg->ap_ssid, "%s");
        if (p) {
            snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%.*s%s%s", (int)(p - cfg->ap_ssid), cfg->ap_ssid, suffix, p + 2);
        } else {
            snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s", cfg->ap_ssid);
        }
    }
#else
    s_cfg.ap_ssid = NULL;
#endif
    s_ev = xEventGroupCreate();
    esp_err_t e = esp_netif_init();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        return e;
    }
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        return e;
    }
    s_sta = esp_netif_create_default_wifi_sta();
#if CONFIG_WIFILIST_SOFTAP
    if (s_cfg.ap_ssid) {
        esp_netif_create_default_wifi_ap();
        const esp_timer_create_args_t ta = {.callback = ap_timer_cb, .name = "wifilist_ap"};
        esp_timer_create(&ta, &s_ap_timer);
    }
#endif
    if (s_host[0]) {
        esp_netif_set_hostname(s_sta, s_host);
    }
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if ((e = esp_wifi_init(&init)) != ESP_OK) {
        return e;
    }
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    if ((e = esp_wifi_start()) != ESP_OK) {
        return e;
    }
    if (!s_cfg.power_save) {
        esp_wifi_set_ps(WIFI_PS_NONE);
    }
    if (s_cfg.ap_ssid && s_cfg.ap_always) {
        xEventGroupSetBits(s_ev, BIT_AP);
    } else {
        ap_arm(false);
    }
    ESP_LOGI(TAG, "%d networks in the list", s_n);
    BaseType_t ok = xTaskCreate(sta_task, "wifilist", 4096, NULL, CONFIG_WIFILIST_TASK_PRIORITY, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
