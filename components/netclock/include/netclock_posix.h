// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#pragma once
// netclock on Linux and macOS: the board's disciplined UTC clock for host
// tools. The same core, poll and selection as netclock.c; CLOCK_MONOTONIC (ns)
// plays esp_timer's part, a state file plays NVS's, and a pthread polls NTP.
// See README.md "POSIX".
//
//   netclock_posix_opts_t o = {.servers = "pool.ntp.org", .state_path = "/var/lib/x/clock"};
//   netclock_posix_init(&o);
//   netclock_posix_start();
//   int64_t ns = netclock_posix_now_ns();  // UTC ns
//
// Two modes. NETCLOCK_POSIX_DISCIPLINE (the default) runs the board's clock.
// NETCLOCK_POSIX_OS trusts the OS clock (chrony, ntpd or whatever else sets
// it): no thread, no state file, and the calls below report the OS clock with
// source NETCLOCK_SRC_OS. A program has one call path either way.
//
// Nothing here touches the OS clock unless set_os_clock asks. Thread safe.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "netclock_status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NETCLOCK_POSIX_DISCIPLINE = 0,  // our own NTP client and model
    NETCLOCK_POSIX_OS,              // the OS clock as it is
} netclock_posix_mode_t;

typedef struct {
    netclock_posix_mode_t mode;
    // NTP servers, as the board's settings `servers` (names or IPv4, space or comma separated; a name ending in
    // pool.ntp.org counts 3). Here a server may also carry a port: "127.0.0.1:12300". NULL or "": no NTP.
    const char *servers;
    uint32_t poll_s;          // once the rate is learned, 16 to 3600 s; 0: 300. While learning: min(120, poll_s)
    // Where the learned rate and the temperature table are kept (what NVS holds on the board). NULL: nowhere.
    // Written to "<path>.tmp", then renamed over it.
    const char *state_path;
    // The crystal's temperature, C (NAN: unknown), read at each reference and every 30 s. NULL: none.
    double (*temp_c)(void *ctx);
    void *temp_ctx;
    // Messages (no newline), as the board logs them. NULL: silent.
    void (*log)(void *ctx, const char *msg);
    void *log_ctx;
    // Discipline mode: step the OS clock (clock_settime) to the model when synced and the two differ by more than
    // os_step_ms (0: 100 ms). Needs root or CAP_SYS_TIME; a failure is logged once and ignored. Off by default.
    // Never slews: adjtime would bend CLOCK_MONOTONIC on Linux, the model's own timebase.
    bool set_os_clock;
    uint32_t os_step_ms;
    // For tests: the gap between the first polls (0: 2000 ms), the poll interval in ms in place of poll_s (0: off),
    // and how long a poll waits for replies (0: 1500 ms).
    uint32_t test_gap_ms, test_poll_ms, test_wait_ms;
} netclock_posix_opts_t;

// What the OS says of its own clock (Linux: ntp_adjtime; elsewhere known is false).
typedef struct {
    bool known;              // the kernel answered
    bool synced;             // a daemon disciplines it (STA_UNSYNC clear, state not TIME_ERROR)
    double esterror_us, maxerror_us;
} netclock_posix_os_t;

// Once, before the rest. Loads the state file. Repeat calls (after netclock_posix_stop) start over. Returns 0, or
// -1 (errno: EINVAL for a bad poll_s or servers string, ENOMEM).
int netclock_posix_init(const netclock_posix_opts_t *o);
// Start / stop the NTP thread (discipline mode; in OS mode they do nothing). Stop returns once it has ended. The
// model is kept across a stop: a later start polls again (iburst) on top of it. Start returns 0 or an errno.
int netclock_posix_start(void);
void netclock_posix_stop(void);

// Write the state file now: the rate learned this run (else what the file had) and the table. For a program's
// exit: the clock stores on its own only when the rate moved enough, at most hourly. Returns 0 or an errno.
int netclock_posix_save(void);

// The local timebase: CLOCK_MONOTONIC, ns. Stamp events with this.
int64_t netclock_posix_mono_ns(void);
// UTC ns at local mono_ns. *synced (optional): discipline mode, some reference has set the clock; OS mode, the
// kernel says it's synchronized (true when it can't say). Before any reference: CLOCK_REALTIME, less the time
// since mono_ns.
int64_t netclock_posix_utc_ns(int64_t mono_ns, bool *synced);
int64_t netclock_posix_now_ns(void);
netclock_source_t netclock_posix_source(void);  // NETCLOCK_SRC_OS in OS mode
bool netclock_posix_synced(void);
// As netclock_quality. OS mode: the kernel's esterror; NAN when the OS doesn't say (macOS); false when it says
// it's not synchronized.
bool netclock_posix_quality(netclock_source_t *source, double *err_us);
// As netclock_rate. False in OS mode.
bool netclock_posix_rate(double *ppm, double *skew_ppm, bool *learned);
// As netclock_set_from_client: a browser's clock (UTC us), stamped now. False: not taken (NTP leads, or OS mode).
bool netclock_posix_set_from_client(int64_t utc_us);
// The OS clock's own state, in either mode.
void netclock_posix_os(netclock_posix_os_t *out);

// As netclock_status (about 4 KB: not on a small stack). OS mode: source, valid and utc_us only.
void netclock_posix_status(netclock_status_t *out);
// The board's /api/clock JSON, plus "mode" ("discipline" or "os") and "os": {known, synced, esterror_us,
// maxerror_us}. Returns the length, or 0 if it didn't fit (about 10 KB at most).
size_t netclock_posix_status_json(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
