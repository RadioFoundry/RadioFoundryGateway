// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The settings, version 2 (docs/spec/fw/rxtx-combinations.md section 4): the devices a box has, the roles it runs
 * on them, and which run. Its JSON Schema is config/rdof-config.schema.json.
 *
 *   "devices": {NAME: {"kind": "sdr" | "modem" | "radio", ...}}
 *   "roles":   {NAME: {"type": "roamer" | "repeater" | "sfr" | "dcdm" | "monitor", "rx" / "tx" / "modem": {"device"}}}
 *   "run":     [NAME, ...]
 *
 * Version 1 settings ("radios", no "version") still load: rdof_config_upgrade writes them as version 2. The parsers
 * of each role's own keys (rdof/config.h, rdof/sdr/config.h, rdof/benchcfg.h, the IP gateway and the screen) read
 * version 2 through rdof_config_view, which gives each the role's section in the shape they take; what version 2
 * adds over that (devices, ownership, more than one SDR) is checked here and by the broker (rdof/broker.h).
 *
 * Portable C. Version 1's "plain SDR config" (one SDR's settings at the top level, no "radios") isn't a box config;
 * it loads as it is and isn't upgraded.
 */
#ifndef RDOF_SETUP_H
#define RDOF_SETUP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The format's own caps: a platform runs fewer (the board's RAM, UARTs and USB ports; rdof/broker.h). */
#define RDOF_SETUP_MAX_DEVICES 16
#define RDOF_SETUP_MAX_ROLES 16
#define RDOF_SETUP_MAX_FREQS 33 /* a roamer's channels and its idle channel */
#define RDOF_SETUP_NAME 40

typedef enum { RDOF_DEV_SDR = 1, RDOF_DEV_MODEM, RDOF_DEV_RADIO } rdof_dev_kind_t;
typedef enum {
    RDOF_ROLE_ROAMER = 1,
    RDOF_ROLE_REPEATER,
    RDOF_ROLE_SFR,
    RDOF_ROLE_DCDM,
    RDOF_ROLE_MONITOR,
} rdof_role_type_t;

/* A modem's "needs": what its description must say, checked when it's opened. */
#define RDOF_NEED_DUPLEX 1u
#define RDOF_NEED_SPLIT 2u
#define RDOF_NEED_DCDM 4u
#define RDOF_NEED_SFR 8u

typedef enum { RDOF_PORT_NONE = 0, RDOF_PORT_UART, RDOF_PORT_USB, RDOF_PORT_PATH } rdof_port_kind_t;

/* Where a modem or a radio is: one of the board's UARTs, a USB device (on the board's OTG port, or the host's), or
   a serial device's path (the host tool). */
typedef struct {
    rdof_port_kind_t kind;
    int uart, tx, rx, rts, cts; /* RDOF_PORT_UART; rts / cts -1 none */
    long baud;
    char usb[8];                /* auto (a modem: found by its kind), cdc-acm, uart (a USB-serial bridge) */
    uint16_t vid, pid;
    char serial[32], usb_port[16];
    char path[64];
} rdof_port_t;

/* A device's "bench": what the bench may transmit on it (docs/spec/bench-api.md section 7). */
typedef struct {
    bool on;
    int nfreqs;
    uint32_t freqs_hz[8];
    double max_key_s; /* -1: the box's bench.tx */
} rdof_dev_bench_t;

typedef struct {
    char name[RDOF_SETUP_NAME];
    rdof_dev_kind_t kind;
    char driver[16];  /* sdr: rtlsdr, lr2021, sx1255, sim, soapy, iio; modem: auto, radio_hotspot, mmdvm_hs, mmdvm */
    rdof_port_t port; /* modem, radio */
    unsigned needs;   /* modem: RDOF_NEED_* */
    bool has_mmdvm;   /* modem: its own MMDVM settings ("mmdvm") */
    /* sdr */
    double rx_rate;   /* 0: the front end's default */
    uint32_t rx_center; /* 0: auto */
    bool tx;          /* has "tx": it transmits */
    int tx_carriers;  /* 1 unless said */
    uint32_t tx_center; /* 0: auto */
    double tx_power_dbm;
    bool has_tx_power;
    rdof_dev_bench_t bench;
} rdof_device_t;

typedef struct {
    char name[RDOF_SETUP_NAME];
    rdof_role_type_t type;
    bool enabled;     /* "enabled": false leaves it out, unless run names it */
    bool run;
    int rx, tx;       /* device indexes; -1 none. A role on one modem or on one SDR has rx == tx */
    bool split;       /* rx and tx are two devices */
    bool sdr_duplex_full; /* dcdm on an SDR: "duplex": "full" */
    uint32_t rx_freq, tx_freq; /* repeater, sfr, dcdm (sfr, dcdm: both "freq") */
    int cc;           /* -1 none */
    int nfreqs;       /* every frequency it receives on: a roamer's channels and idle, a monitor's channels */
    uint32_t freqs[RDOF_SETUP_MAX_FREQS];
    int fcc[RDOF_SETUP_MAX_FREQS]; /* each one's color code, -1 none */
} rdof_role_t;

typedef struct {
    char name[32];
    bool usb_hub;     /* "usb_hub": true */
    int ndev, nrole;
    rdof_device_t dev[RDOF_SETUP_MAX_DEVICES];
    rdof_role_t role[RDOF_SETUP_MAX_ROLES];
    int nbench;       /* bench.devices */
    int bench[RDOF_SETUP_MAX_DEVICES];
} rdof_setup_t;

/* 3 for version 3 settings (rdof/site.h), 2 for version 2, 1 for a version 1 box config, 0 for a plain SDR config
   (v1, not a box config), -1 for anything else (not JSON, not an object, an unknown version). Everything here takes
   version 3 as the version 2 settings it runs as (rdof_site_lower): its errors name version 3's places. */
int rdof_config_version(const char *text, size_t len);

/* Version 2 settings into setup: every key known (as the schema), names that exist, devices of the kind each role
   needs, one monitor a SDR, what run names. Who owns what, and whether it fits the platform, is the broker's.
   false with error set: "path: message", path as roles.R1.rx.device. */
bool rdof_setup_parse(const char *text, size_t len, rdof_setup_t *setup, char *error, size_t cap);
/* A device's or role's index by name, or -1. */
int rdof_setup_device(const rdof_setup_t *s, const char *name);
int rdof_setup_role(const rdof_setup_t *s, const char *name);

/* Version 1 settings written as version 2 (malloc'd, NUL terminated; *out_len its length), or NULL with error set.
   Keys version 1 ignored are left out; comments go. Version 2 comes back as it is (comments stripped). */
char *rdof_config_upgrade(const char *text, size_t len, size_t *out_len, char *error, size_t cap);

/* Version 2 settings as the version 1 box config the role parsers read (malloc'd; NULL with error set). focus: a
   role, whose section and its SDR's are the ones run names; NULL: every role run names. Version 1 comes back as it
   is. Each SDR's section is named after its monitor role, else after the device. */
char *rdof_config_view(const char *text, size_t len, const char *focus, size_t *out_len, char *error, size_t cap);
/* The view's name for the SDR section of device dev (its monitor role's, else the device's). */
const char *rdof_setup_sdr_section(const rdof_setup_t *s, int dev);

/* For a parser that takes version 1: text itself, or (version 2) its view with focus, in *owned (free it). false
   with error set when the view can't be made. */
bool rdof_config_as_v1(const char **text, size_t *len, const char *focus, char **owned, char *error, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
