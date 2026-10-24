// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#pragma once
// wifilist: a Wi-Fi station for ESP-IDF that knows several networks, each
// with a priority. It scans, connects to the highest priority network in
// sight (the stronger signal first among equals), falls back down the list,
// and scans again when the connection drops. An optional setup access point
// comes up while there's no connection. The list lives in its own NVS
// namespace (CONFIG_WIFILIST_NVS_NAMESPACE); passwords never leave the board
// through its JSON. See README.md.
//
//   nvs_flash_init();
//   wifilist_config_t c = WIFILIST_CONFIG_DEFAULT();
//   c.hostname = "mybox";
//   c.ap_ssid = "mybox-%s";          // %s: the last 4 hex digits of the MAC
//   c.ap_password = "setup123";
//   wifilist_start(&c);

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"
#include "wifilist_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *hostname;     // DHCP hostname, NULL: the default
    bool power_save;          // false: WIFI_PS_NONE (no DTIM sleep: prompt replies)
    // The setup access point (CONFIG_WIFILIST_SOFTAP). ap_ssid NULL: none.
    const char *ap_ssid;      // "%s" in it becomes the last 4 hex digits of the AP's MAC
    const char *ap_password;  // 8 to 63 characters (WPA2), or NULL / "" for an open AP
    uint32_t ap_after_ms;     // up after this long without a connection (at once with no networks)
    bool ap_always;           // keep it up while connected too
    uint8_t ap_channel;       // until the station connects (then it follows the station's channel)
    uint8_t ap_max_conn;
} wifilist_config_t;

#define WIFILIST_CONFIG_DEFAULT() \
    { .hostname = NULL, .power_save = false, .ap_ssid = NULL, .ap_password = NULL, .ap_after_ms = 20000, \
      .ap_always = false, .ap_channel = 6, .ap_max_conn = 2 }

// After nvs_flash_init(). Initializes esp_netif, the default event loop (if
// not yet), Wi-Fi and the station netif (and the AP's), then runs.
esp_err_t wifilist_start(const wifilist_config_t *cfg);

// The list, highest priority first, with passwords: for the app only.
int wifilist_get(wifilist_net_t *out, int max);
// Replace the list (checked, sorted, stored). The connection is kept if its
// network is still there with the same password.
esp_err_t wifilist_set(const wifilist_net_t *nets, int n, char *err, size_t err_cap);
// Whether a list was ever stored. Lets an app migrate an older single network.
bool wifilist_stored(void);
// Store a one-network list, if no list was ever stored (ssid "" stores an empty list).
esp_err_t wifilist_migrate(const char *ssid, const char *password);

// Edit from JSON: {"networks":[{"ssid","priority","hidden","password","was"}]}
// (the whole list; "password" left out keeps the stored one of "was", or of
// the same name), or {"ssid","password"} for a list of one.
esp_err_t wifilist_update_json(const char *json, size_t len, char *err, size_t err_cap);
// The list as JSON without passwords (wifilist_networks_json).
size_t wifilist_export_json(char *buf, size_t cap);

void wifilist_status(wifilist_status_t *out);
bool wifilist_connected(void);
// Scan now to refresh what the status shows (doesn't change networks).
void wifilist_scan(void);
// Drop the connection and pick again.
void wifilist_reconnect(void);
esp_netif_t *wifilist_sta_netif(void);

#ifdef __cplusplus
}
#endif
