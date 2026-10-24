// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#pragma once
// wifilist without the platform: the network list (check, sort, edit with
// passwords kept), which network to try first, disconnect reason names, and
// the status with its JSON writer. Pure C (no ESP-IDF): the host tests build it.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef WIFILIST_MAX
#define WIFILIST_MAX 8          // networks in the list
#endif
#define WIFILIST_SEEN 16        // access points kept from the last scan
#define WIFILIST_FAILS 8        // failures kept for the status
#define WIFILIST_PRIORITY_MIN (-100)
#define WIFILIST_PRIORITY_MAX 100

typedef struct {
    char ssid[33];
    char password[65];   // "" = open
    int8_t priority;     // higher first; equal: the stronger signal first
    bool hidden;         // doesn't broadcast its name: tried even when a scan doesn't show it
} wifilist_net_t;

// An access point a scan saw.
typedef struct {
    char ssid[33];
    uint8_t bssid[6];
    int8_t rssi;
    uint8_t channel;
    bool open;           // no password
} wifilist_ap_t;

// A network to try: nets[net], at aps[ap] (-1: hidden, not seen: by name only).
typedef struct {
    int net;
    int ap;
} wifilist_cand_t;

// Networks to try, best first: those the scan saw (the strongest access
// point of each), plus hidden ones it didn't, by priority, then signal, then
// list order. A protected network seen only as an open access point (or the
// other way round) is left out. Returns the count.
int wifilist_rank(const wifilist_net_t *nets, int n, const wifilist_ap_t *aps, int na, wifilist_cand_t *out, int max);

// Highest priority first; equal priorities keep their order.
void wifilist_sort(wifilist_net_t *nets, int n);

// Names 1 to 32 bytes and unique, passwords empty, 8 to 63 characters or 64
// hex digits, priorities in range, at most WIFILIST_MAX.
bool wifilist_check(const wifilist_net_t *nets, int n, char *err, size_t cap);

// An edited entry: has_password false keeps the stored password of the
// network named `was` (or of the same name when `was` is empty); a new
// network without a password is open.
typedef struct {
    wifilist_net_t net;
    bool has_password;
    char was[33];
} wifilist_edit_t;

// The new list from the edits and the old list: checked and sorted. Returns
// the count, or -1 with err set.
int wifilist_merge(const wifilist_net_t *old, int n_old, const wifilist_edit_t *in, int n_in, wifilist_net_t *out,
                   char *err, size_t cap);

// The wait before scanning again after `failed_rounds` rounds in a row with no
// connection: 5 s, doubled each round, up to max_s. With the setup access point
// up it's always max_s (a scan takes the radio off the AP's channel).
uint32_t wifilist_backoff_s(int failed_rounds, uint32_t max_s, bool ap_up);

// The setup access point's password: none (NULL or ""), or 8 to 63 characters.
bool wifilist_ap_password_ok(const char *password);

// The list as NVS keeps it: {version 1, n, n records of 100 bytes}.
#define WIFILIST_BLOB_VERSION 1
#define WIFILIST_BLOB_REC 100
size_t wifilist_blob_size(int n);
// Writes wifilist_blob_size(n) bytes into out. Returns that size.
size_t wifilist_blob_encode(const wifilist_net_t *nets, int n, uint8_t *out);
// Reads at most `max` networks. Returns the count, or -1 when it isn't a list:
// shorter than its header or its records, another version, or more than
// WIFILIST_MAX networks. Names and passwords are always NUL-terminated.
int wifilist_blob_decode(const uint8_t *blob, size_t len, wifilist_net_t *out, int max);

// An 802.11 / ESP-IDF disconnect reason in words (0: our own timeout).
const char *wifilist_reason(uint8_t reason);

typedef struct {
    uint32_t t_s;        // seconds since boot
    char ssid[33];
    uint8_t reason;
    int8_t rssi;
    bool lost;           // it was connected (else: a connect attempt failed)
} wifilist_fail_t;

typedef struct {
    const char *state;   // "no networks", "scanning", "connecting", "connected", "waiting"
    bool connected;
    char ssid[33];
    uint8_t bssid[6];
    int8_t rssi;
    uint8_t channel;
    char ip[16];
    uint32_t connected_s;  // for how long
    bool ap_up;
    char ap_ssid[33];
    uint32_t scans, connects;
    uint32_t uptime_s;   // the clock fails[].t_s counts on
    int n_nets;
    struct {
        char ssid[33];
        int8_t priority;
        bool hidden, password_set;
        int8_t rssi;     // in the last scan, -128 = not seen
    } nets[WIFILIST_MAX];
    int n_seen;
    wifilist_ap_t seen[WIFILIST_SEEN];  // strongest first
    int n_fail;
    wifilist_fail_t fails[WIFILIST_FAILS];  // oldest first
} wifilist_status_t;

// The status as JSON (no passwords: "password_set" instead). Returns the
// length, or 0 if it didn't fit.
size_t wifilist_status_json(const wifilist_status_t *st, char *buf, size_t cap);

// {"networks":[{"ssid":...,"priority":...,"hidden":true}]}: the list
// without passwords, for an export. Returns the length, or 0.
size_t wifilist_networks_json(const wifilist_net_t *nets, int n, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
