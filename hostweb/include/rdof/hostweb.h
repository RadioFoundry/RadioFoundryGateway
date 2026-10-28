// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The firmware's web page on a host (Linux / macOS) tool: a small HTTP server in its own thread, for watching a
 * running rdofradio (or any host tool built on rdof_common) live in a browser. POSIX sockets and pthreads only.
 *
 * The page is the firmware's own (esp_idf/main/web/index.html, with the netclock and wifilist scripts), embedded
 * at build time. The API is the firmware's too: the server hands /api/status, /api/config, /api/resolved,
 * /api/log, /api/lastheard and /api/restart to the shared code (rdof/webapi.h), with the host as the platform:
 * the process's and the system's memory, the OS's clock, the log and last heard kept here, and the settings and
 * the restart from the tool's hooks. Every other /api/ path answers 501 with {"error":"not on this platform"}.
 *
 * Publishing is cheap and never waits on the server: rdof_hostweb_status copies one struct and drops the update
 * when the server holds the lock (the next one replaces it); the log and last-heard calls hold a lock for
 * microseconds (the server copies under it and does its I/O outside). All calls are safe from any thread, and do
 * nothing before rdof_hostweb_start or after rdof_hostweb_stop (but the log, which keeps a tool's startup lines).
 */
#ifndef RDOF_HOSTWEB_H
#define RDOF_HOSTWEB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/netcalls.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { RDOF_HOSTWEB_REPEATER = 0, RDOF_HOSTWEB_TRANSCEIVER } rdof_hostweb_role_t;

/* One slot's call now. */
typedef struct {
    bool active;
    bool group;
    uint32_t src, dst;
    bool has_ber, has_rssi;
    float ber; /* % */
    int rssi;  /* dBm */
} rdof_hostweb_slot_t;

/* An SDR the tool runs: a card on the page as the board's SDR has, and its rows in the Channels table. */
#define RDOF_HOSTWEB_SDRS 4
typedef struct {
    char name[24];
    char device[48];  /* the front end, "libiio AD9361 local:" */
    char error[96];   /* non-empty: the page shows it as not receiving */
    bool open;        /* the device is open */
    bool receiving;   /* samples came in the last second */
    bool calibrating; /* the receiver is off its channels for a while (a transceiver's loop delay) */
    double rate, center_hz;
    double gain_db;   /* RX gain now; NAN: not known */
    bool agc;         /* the device's AGC sets it */
    double samples_per_s; /* measured over the last seconds; < 0: not yet */
    uint64_t overflows, read_errors;
    double lost_ms;   /* samples the device dropped, made up */
    double load;      /* 0..1 of the receive loop's time spent working, or -1 */
    int nstage;       /* where that time went: shares of the loop's time */
    struct {
        char name[16];
        float share;
    } stage[6];
    bool tx;          /* it transmits too */
    uint32_t tx_hz, tx_lo_hz;
    double tx_gain_db;
    bool tx_on;       /* keyed: the TX gain up */
    uint64_t tx_errors;
    bool has_mqtt, mqtt; /* its own MQTT connection, and its state */
    uint32_t mqtt_dropped;
    int hblink_peers, hblink_up; /* its channels' own HBlink4 peers */
    /* every receiver channel (a transceiver's own first), as the board's SDR lists them */
    int nchan;
    struct {
        char id[24], name[32];
        uint32_t freq;
        bool in;        /* inside the receiver's span */
        uint64_t bursts;
        int rssi;       /* dBm, 0: none yet */
        float afc_hz;   /* the channel's frequency correction */
        float idle_pct; /* the share of chunks the idle gate skipped */
    } chan[16];
} rdof_hostweb_sdr_t;

/* A radio or modem the tool drives on a port: a RadioFoundry radio's hotspot (a split's TX half), an MMDVM. */
#define RDOF_HOSTWEB_MODEMS 4
typedef struct {
    char name[24];
    char kind[24];     /* "RadioFoundry radio", "MMDVM" */
    char role[48];     /* what it does here, "transmits (the split's TX half)" */
    char port[64];
    char version[96];  /* what it answers to the version request */
    char state[32];    /* free text, "epoch aligned" */
    char error[96];    /* non-empty: failed */
    bool open, ready;
    int nextra;        /* its counters, shown as they come */
    struct {
        char name[24];
        int64_t value;
    } extra[8];
} rdof_hostweb_modem_t;

/* What the page shows. Zero it, fill what the tool knows, publish. Strings are cut to fit. */
typedef struct {
    rdof_hostweb_role_t role;
    char name[24];     /* the radio's name; "" uses the name given to start */
    char radio[48];    /* the front end, "SoapySDR PlutoSDR" */
    char modem[48];    /* what answers as the modem, "SDR modem" */
    char protocol[16]; /* "" shows "host" */
    char state[16];    /* free text, "idle", "hang" */
    char error[96];    /* a radio error; non-empty shows the radio as not ready */
    char heard[48];    /* "src to dst" now heard on the air, or "" */
    char sending[48];  /* the same for what's going out */
    bool ready;
    bool keyed;
    bool links;        /* the tool has a network (HBlink4): the page shows hblink, and mqtt when has_mqtt */
    bool has_mqtt;
    bool hblink, mqtt; /* the links' state (the page warns when down) */
    unsigned hblink_tx, hblink_rx; /* HBlink4 packets, as the board counts them */
    uint32_t rx_hz, tx_hz;
    int cc;
    double cpu_load; /* 0..1 of the tool's real-time loop, or -1 */
    uint64_t bursts_heard, bursts_sent, dropped;
    rdof_hostweb_slot_t slot[2];
    /* the parrot, when one runs */
    bool parrot_on;
    char parrot_state[16]; /* "idle", "recording", "waiting", "replaying" */
    int parrot_slot;
    uint32_t parrot_id, parrot_src, parrot_dst;
    uint32_t parrot_replayed, parrot_ignored, parrot_cut;
    /* a free-form counters block shown as "tool" on the page: up to 8 name / value pairs */
    /* the channels it also monitors (monitor roles), receive only; the page lists them on the radio's card */
    int nchan;
    struct {
        char id[24];
        uint32_t freq;
        uint64_t bursts;
        int rssi;   /* dBm, 0: none yet */
        bool peer;  /* an HBlink4 peer of its own */
        bool up;
    } chan[16];
    int nextra;
    struct {
        char name[24];
        int64_t value;
    } extra[8];
    bool spectrum; /* the tool makes the page's spectrum (rdof_hostweb_spectrum_row) */
    uint32_t mqtt_dropped; /* the radio's MQTT posts dropped (has_mqtt) */
    /* the SDRs it runs and the radios or modems it drives on a port (below) */
    int nsdr;
    rdof_hostweb_sdr_t sdrs[RDOF_HOSTWEB_SDRS];
    int nmodem;
    rdof_hostweb_modem_t modems[RDOF_HOSTWEB_MODEMS];
} rdof_hostweb_status_t;

/* The tool's settings, for the page's Settings tab (GET / POST /api/config, GET /api/resolved), and its restart
 * (POST /api/restart, and after a save). Called from the server's thread: the callbacks must be safe to call while
 * the tool runs. */
typedef struct {
    bool writable;    /* false: the page shows them read-only (say why in source) */
    char source[160]; /* where they come from: "file /etc/x.json", "command line flags" */
    /* the settings text: malloc'd, the server frees it; NULL if there is none */
    char *(*get)(void *ctx);
    /* Validate and store a new document (text is NUL terminated). True: stored; the server answers "saved;
     * restarting" and calls restart. False: err holds the reason, nothing was stored (the answer is 400 with it). */
    bool (*save)(void *ctx, const char *text, size_t len, char *err, size_t err_cap);
    /* A text description of the settings as the tool resolved them (GET /api/resolved); may be NULL. */
    size_t (*resolved)(void *ctx, char *out, size_t cap);
    /* Restart the tool (rdof_hostweb_reexec, once it has stopped cleanly): ask for it and return at once; the
     * answer goes out first. NULL: no restart (and saved settings take effect at the next start). */
    void (*restart)(void *ctx);
    void *ctx;
} rdof_hostweb_settings_t;

/* Start the server on bind_addr (NULL: 127.0.0.1) and port (0: any free one). name is the box name the page shows.
 * Returns the port, or -1 (errno set; say why with strerror). A second start while running returns -1 / EALREADY. */
int rdof_hostweb_start(const char *bind_addr, int port, const char *name);
/* Stop the server and join its thread. */
void rdof_hostweb_stop(void);
bool rdof_hostweb_running(void);

/* The clock when the tool runs the board's (netclock): /api/status names its source and the page's clock card
 * reads /api/clock. Without it (or NULL) the page shows the OS's clock (ntp_adjtime, read only). The callbacks run
 * on the server's thread. */
typedef struct {
    const char *(*source)(void);                   /* "ntp", "browser", "none" */
    size_t (*json)(char *out, size_t cap);          /* /api/clock: netclock_status_json */
    bool (*set)(int64_t utc_us);                    /* the browser's clock; false: not taken */
    size_t (*settings_json)(char *out, size_t cap); /* {"servers": ..., "poll_s": ...} */
} rdof_hostweb_clock_t;
void rdof_hostweb_clock(const rdof_hostweb_clock_t *c);
/* The version /api/status shows (default "host"). The string is kept, not copied. */
void rdof_hostweb_version(const char *v);

/* The file where the tool's supervisor records how it last ended, when that wasn't a clean stop (the Pluto bundle's
 * run.sh): lines "rc=N" (the shell's exit status: 128 + the signal when a signal ended it) and "at=UNIX_S", a line
 * "---", then the log's last lines. The status shows it as "exit" (the crash card), GET /api/crash answers the file,
 * DELETE /api/crash removes it. Read when asked; the path is kept, not copied. NULL: none. */
void rdof_hostweb_last_exit(const char *path);

/* Set the settings hooks (a copy is taken). Before start or after; without them /api/config answers 501. */
void rdof_hostweb_settings(const rdof_hostweb_settings_t *s);
/* Write a file whole or not at all: a temporary file beside it, flushed, then renamed over it. False: err says
 * why, and the file is as it was. */
bool rdof_hostweb_save_file(const char *path, const char *text, size_t len, char *err, size_t err_cap);
/* Start this program again with argv (argv[0] found as the shell would): the page then says the last start was a
 * restart. Stop the server and close the devices first. Returns only if the exec failed (errno set). */
int rdof_hostweb_reexec(char *const argv[]);
/* Publish the current state (a copy is taken). */
void rdof_hostweb_status(const rdof_hostweb_status_t *s);
/* A line for /api/log, stamped with the time of day (UTC). printf style; cut at 191 bytes. Kept from before the
 * server starts. */
void rdof_hostweb_logf(const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
/* A call tracker's JSON line (the SDR modem's tracker output), for the last heard list: role "repeater" or
 * "roamer", radio the radio's name, channel NULL for a radio. */
void rdof_hostweb_tracker_json(const char *role, const char *radio, const char *channel, const char *json);
/* The page's spectrum and waterfall (GET /api/spectrum, as the board's): rows of up to RDOF_HOSTWEB_SPECTRUM_BINS
 * bytes on rdof/sdr/spectrum.h's scale (dBFS times -2), the receiver's whole span. Make rows only while
 * rdof_hostweb_spectrum_wanted (the page asked in the last 10 s); a row is copied in under a short lock. The page's
 * bands are the status's receive, transmit and monitored channels that fall in the span. */
#define RDOF_HOSTWEB_SPECTRUM_BINS 512
bool rdof_hostweb_spectrum_wanted(void);
void rdof_hostweb_spectrum_row(const uint8_t *row, int bins, double center_hz, double rate, int row_ms);
/* The same for the status's SDR sdr (0 .. RDOF_HOSTWEB_SDRS - 1; the calls above are SDR 0's): the page shows one
 * SDR's at a time (GET /api/spectrum?sdr=N), and asks only for that one's. */
bool rdof_hostweb_spectrum_wanted_sdr(int sdr);
void rdof_hostweb_spectrum_row_sdr(int sdr, const uint8_t *row, int bins, double center_hz, double rate, int row_ms);
/* A call going out ("start" or "end"; the on_tx hook's arguments), for the last heard list. */
void rdof_hostweb_net_call(const char *role, const char *radio, const char *action, const rdof_tx_call_info_t *call,
                           double duration);

#ifdef __cplusplus
}
#endif

#endif
