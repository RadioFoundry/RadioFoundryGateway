// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#pragma once
// gnss: a GNSS receiver's NMEA on a UART, for ESP-IDF. Reads its sentences
// (gnss_nmea.h), keeps the fix, position and satellites for a status, and
// gives the time of each second's burst of sentences to a callback (for a
// clock: netclock_gnss_time). See README.md.
//
//   gnss_start(netclock_gnss_time);   // pins and baud from Kconfig (GNSS_*)
//   gnss_status_json(buf, sizeof buf);
//
// With GNSS_AUTODETECT, a receiver that gives no good sentence in 3 s is
// tried with the pins swapped and at the other usual baud rates.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gnss_nmea.h"
#ifdef ESP_PLATFORM
#include "esp_err.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

// The UTC second (whole) of the burst of sentences that began at local start_us (esp_timer us). Only times from a
// receiver with a fix are given. Called from the reader task.
typedef void (*gnss_time_cb_t)(int64_t start_us, int64_t utc_s);

typedef struct {
    bool running;       // the reader is up
    bool found;         // good sentences on the current pins and baud
    int rx_gpio, tx_gpio, baud;  // what it's on now (autodetect may have swapped them)
    uint32_t bytes, lines, overflows, tries;
    int32_t last_s;     // since the last good sentence, -1 = never
    gnss_nmea_t nmea;
    gnss_burst_t burst;
} gnss_status_t;

#ifdef ESP_PLATFORM
// Starts the reader when GNSS_UART_RX_GPIO >= 0; ESP_ERR_NOT_SUPPORTED without one.
esp_err_t gnss_start(gnss_time_cb_t on_time);
void gnss_status(gnss_status_t *out);
// The status as one JSON object; returns its length, 0 if it didn't fit (about 700 bytes).
size_t gnss_status_json(char *buf, size_t cap);
#endif
// The same from a status (pure: for host tests and other formats).
size_t gnss_json(const gnss_status_t *s, char *buf, size_t cap);

// The short form for another status message: ,"gnss":{"fix","sats","lat","lon","alt_m","hdop"} put inside the JSON
// object in json (length len, room cap) before its closing brace; nothing when the receiver isn't found. Returns the
// new length (len unchanged when nothing was added or it didn't fit).
size_t gnss_json_append(const gnss_status_t *s, char *json, size_t len, size_t cap);
#ifdef ESP_PLATFORM
size_t gnss_status_append(char *json, size_t len, size_t cap);  // the reader's status
#endif

#ifdef __cplusplus
}
#endif
