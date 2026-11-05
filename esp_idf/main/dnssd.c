// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * mDNS and DNS-SD (espressif/mdns): the board answers as BOX.local and lists its services, the page
 * (_http._tcp) and, when it runs, the bench's line protocol (_rfgbench._tcp, TXT kind, ver and the Wi-Fi MAC), so
 * `rfgbench find` finds it whatever address DHCP gave it. BOX is the box's name as a host name (lowercase,
 * letters, digits and '-'); with no name of its own ("radiofoundry"), the MAC's last two bytes are added, as the
 * setup AP's name has them. A name another device has already gets a suffix from the responder.
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mdns.h"

static const char *TAG = "dnssd";
static bool up;

void dnssd_start(void)
{
    char host[33];
    size_t n = 0;
    for (const char *s = g_roles.box; *s && n < 24; s++) {
        const char c = (char)tolower((unsigned char)*s);
        if (isalnum((unsigned char)c) || c == '-')
            host[n++] = c;
        else if (n && host[n - 1] != '-')
            host[n++] = '-';
    }
    host[n] = 0;
    if (!n || !strcmp(host, "radiofoundry")) {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(host, sizeof host, "radiofoundry-%02x%02x", mac[4], mac[5]);
    }
    esp_err_t e = mdns_init();
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "mDNS: %s", esp_err_to_name(e));
        return;
    }
    mdns_hostname_set(host);
    mdns_instance_name_set(g_roles.box);
    mdns_txt_item_t txt[] = {{"path", "/"}};
    mdns_service_add(NULL, "_http", "_tcp", 80, txt, 1);
    up = true;
    ESP_LOGI(TAG, "mDNS: %s.local", host);
}

void dnssd_bench(const char *name, int port)
{
    if (!up)
        return;
    uint8_t m[6] = {0};
    char mac[18];
    esp_read_mac(m, ESP_MAC_WIFI_STA);
    snprintf(mac, sizeof mac, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    mdns_txt_item_t txt[] = {{"kind", "esp32s3-gateway"}, {"ver", esp_app_get_description()->version}, {"mac", mac}};
    if (mdns_service_add(name, "_rfgbench", "_tcp", (uint16_t)port, txt, 3) != ESP_OK)
        ESP_LOGW(TAG, "mDNS: can't add the bench's service");
}
