// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#pragma once
// netclock's web pieces for an esp_http_server the app runs. Paths are under
// CONFIG_NETCLOCK_HTTP_BASE ("/api" by default):
//
//   GET  /api/clock           netclock_status_t as JSON
//   POST /api/time            {"utc_ms": <the browser's Date.now()>}: set from the browser
//   GET  /api/clock/settings  {"servers": "...", "poll_s": 300}
//   POST /api/clock/settings  the same (keys left out keep their value)
//   GET  /netclock.js         the page section (gzipped): put
//                             <div data-netclock></div><script src="/netclock.js" defer></script>
//                             in a page and it renders there
//
// Five URI handlers: leave room in httpd_config_t.max_uri_handlers.

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t netclock_http_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
