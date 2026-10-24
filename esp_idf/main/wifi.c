// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Wi-Fi and the clock: the wifilist component runs the station over the
 * board's list of networks (components/wifilist), and netclock keeps UTC
 * from NTP (components/netclock). With no networks, or no connection
 * within 20 s, the setup access point comes up (radiofoundry-XXXX, password
 * radiofoundry, page at 192.168.4.1) while the station keeps trying. Power save
 * is off (the modem link and the call JSON can't wait out DTIM sleeps). This file keeps
 * g_board in step. Both components keep their settings in their own NVS
 * namespaces; settings.c moves them in and out of the settings JSON.
 */
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "wifilist.h"

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        snprintf(g_board.ip, sizeof g_board.ip, IPSTR, IP2STR(&e->ip_info.ip));
        g_board.wifi_up = true;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        g_board.wifi_up = false;
        g_board.ip[0] = 0;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        g_board.ap_mode = true;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STOP) {
        g_board.ap_mode = false;
    }
}

void wifi_start(void)
{
    wifilist_config_t c = WIFILIST_CONFIG_DEFAULT();
    c.hostname = g_roles.box;
#if CONFIG_RDOF_SOFTAP
    c.ap_ssid = "radiofoundry-%s";
    c.ap_password = "radiofoundry";
    c.ap_after_ms = 20000;
#endif
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE)
        ESP_ERROR_CHECK(e);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    ESP_ERROR_CHECK(wifilist_start(&c));
#if !CONFIG_RDOF_SOFTAP
    wifilist_net_t one;
    if (!wifilist_get(&one, 1))
        ESP_LOGE("wifi", "no Wi-Fi networks and the softAP is built out: nothing to connect to");
#endif
}
