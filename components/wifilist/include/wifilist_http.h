// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#pragma once
// wifilist's web pieces for an esp_http_server the app runs. Paths are under
// CONFIG_WIFILIST_HTTP_BASE ("/api" by default):
//
//   GET  /api/wifi            wifilist_status_t as JSON (no passwords: "password_set")
//   POST /api/wifi/networks   {"networks":[...]}: the whole list (wifilist_update_json); the status back
//   POST /api/wifi/scan       scan now; the page reads the result from /api/wifi
//   GET  /wifilist.js         the page section (gzipped): put
//                             <div data-wifilist></div><script src="/wifilist.js" defer></script>
//                             in a page and it renders there
//
// Four URI handlers: leave room in httpd_config_t.max_uri_handlers.

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifilist_http_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
