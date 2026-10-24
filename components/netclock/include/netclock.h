// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#pragma once
// netclock: a UTC clock for ESP-IDF, disciplined by NTP (several servers,
// chrony-style selection) or, with no NTP, by a web browser's clock. It
// learns the crystal's rate error (kept in NVS across restarts), slews
// offsets in rather than stepping, and ignores outliers. See README.md.
//
//   nvs_flash_init();
//   netclock_init();                 // loads its settings and the stored rate
//   ...network up...                 // polling starts on IP (CONFIG_NETCLOCK_START_ON_IP)
//   int64_t ns = netclock_now_ns();  // UTC ns
//
// Thread safe. Settings live in its own NVS namespace (CONFIG_NETCLOCK_NVS_NAMESPACE).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "netclock_status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NETCLOCK_SERVERS_LEN 128

typedef struct {
    char servers[NETCLOCK_SERVERS_LEN];  // names or IPs, space or comma separated; "" = no NTP
    uint32_t poll_s;                     // poll interval once the rate is learned, 16 to 3600 s
} netclock_settings_t;

// After nvs_flash_init(). Creates the default event loop if there is none.
esp_err_t netclock_init(void);
// Start polling NTP. Called on the first IP address unless CONFIG_NETCLOCK_START_ON_IP is off.
void netclock_start(void);

// UTC now, in ns. Before any reference: the system clock (1970 at boot, unless something set it).
int64_t netclock_now_ns(void);
// UTC ns at esp_timer time t_us. *synced (optional): some reference has set the clock.
int64_t netclock_utc_ns(int64_t t_us, bool *synced);
netclock_source_t netclock_source(void);
bool netclock_synced(void);
// The local clock's rate error being corrected (ppm, + = local runs slow:
// true elapsed = local elapsed * (1 + ppm * 1e-6)) and its standard error
// (0 when not fitted this run). *learned: fitted this run, else the stored
// rate. False while there's neither. For timing something else against the
// local clock (another device's crystal).
bool netclock_rate(double *ppm, double *skew_ppm, bool *learned);
// For timing slots on UTC: the source now and how far off UTC may be, us (the fit's rms, an offset still
// slewing in, the rate's error since the last reference; nc_clock_quality). False while not synced.
bool netclock_quality(netclock_source_t *source, double *err_us);
// The chip's die temperature (C), NAN without CONFIG_NETCLOCK_DIE_TEMP. A
// crystal drifts as the board warms: a rate learned before it settles moves.
float netclock_die_c(void);

// A client's clock (the web page's "set from browser"), in UTC us. Ignored
// once NTP has synced, so NTP always wins. Returns true if applied.
bool netclock_set_from_client(int64_t utc_us);

// A GNSS receiver's time: the UTC second (whole, from its NMEA) of the burst
// of sentences that began at local start_us (esp_timer us). With a pulse
// (NETCLOCK_PPS_GPIO) it numbers the edge before the burst, so the pulse
// sets the clock with no NTP or browser; with none, every
// NETCLOCK_GNSS_EVERY-th is a reference itself, less the burst's lag behind
// the second (measured against the pulse when there was one, else
// NETCLOCK_GNSS_LAG_MS). Give only times from a receiver with a fix.
void netclock_gnss_time(int64_t start_us, int64_t utc_s);

// Everything for a status page (about 4 KB: not on a small stack).
void netclock_status(netclock_status_t *out);

void netclock_get_settings(netclock_settings_t *out);
// Check, store and apply (the server list takes effect on the next poll).
esp_err_t netclock_set_settings(const netclock_settings_t *s, char *err, size_t err_cap);
// Whether settings were ever stored (else the Kconfig defaults apply). Lets an
// app migrate its own older setting once.
bool netclock_settings_stored(void);
// {"servers":"...","poll_s":300}
size_t netclock_settings_json(char *buf, size_t cap);
// The same object, or a plain string of servers. Keys left out keep their value.
esp_err_t netclock_settings_from_json(const char *json, size_t len, char *err, size_t err_cap);

// Optional: map a configured server name before DNS (e.g. "usb" to the host
// at the other end of a USB network link). Return the name to use.
void netclock_set_name_map(const char *(*map)(const char *name));

#ifdef __cplusplus
}
#endif
