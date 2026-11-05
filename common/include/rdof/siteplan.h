// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * What runs from the settings (version 3, rdof/site.h; docs/spec/fw/config-v3.md section 12): the document and its
 * parse kept together, every device's own keys checked, and the run units it makes:
 *
 *   a repeater      each repeater app: the device that transmits on its out, and the one that hears its in
 *   an SFR gateway  each frequency with mode sfr: the device that transmits there
 *   a DCDM gateway  each frequency with mode dcdm: the same
 *   a transceiver   each device that transmits on simplex frequencies (no app's, no SFR's or DCDM's): named after it
 *   a receiver      each SDR that hears a frequency past its repeaters' own: "<device>-rx"
 *
 * The role parsers read a plan: rdof_plan_radio (rdof/config.h), rdof_plan_sdr (rdof/sdr/config.h),
 * rdof_plan_ipgw (rdof/ipgw.h), rdof_plan_bench_radios and rdof_plan_bench_tx (rdof/benchcfg.h). Their text forms
 * open a plan each; the firmware opens one and reads it for each.
 *
 * Portable C. A plan is large (the document's nodes and rdof_site_t, about 150 KB): it's allocated.
 */
#ifndef RDOF_SITEPLAN_H
#define RDOF_SITEPLAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/config.h"
#include "rdof/jsonp.h"
#include "rdof/site.h"
#include "rdof/txpower.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Units at most; a unit's frequencies at most (a transceiver's, each a channel of rdof_config_t; a receiver's). */
#define RDOF_PLAN_MAX_UNITS 16
#define RDOF_PLAN_MAX_FREQS RDOF_CFG_CHANNELS

typedef enum { RDOF_DEV_SDR = 1, RDOF_DEV_MODEM, RDOF_DEV_RADIO } rdof_dev_kind_t; /* rdof_device3_t.kind */

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

/* A device's own keys, checked. */
typedef struct {
    rdof_port_t port;   /* a modem, a radio */
    unsigned needs;     /* a modem: RDOF_NEED_* */
    bool tx;            /* an SDR: it transmits ("tx", or a frequency it transmits on) */
    int tx_carriers;    /* an SDR: tx.carriers, 1 unless said */
    double tx_power_dbm; /* an SDR: tx.power_dbm, at the antenna */
    bool has_tx_power;
    rdof_txmap_t txmap; /* its RF levels in dBm: a modem's or radio's tx.min_dbm / max_dbm (none if not given); an
                           SDR's rdof_txpower_sdr_map of its power */
    rdof_dev_bench_t bench;
} rdof_plan_dev_t;

typedef enum {
    RDOF_UNIT_REPEATER = 1,
    RDOF_UNIT_SFR,
    RDOF_UNIT_DCDM,
    RDOF_UNIT_TRANSCEIVER,
    RDOF_UNIT_RECEIVER,
} rdof_unit_kind_t;

typedef struct {
    char name[RDOF_SITE_NAME + 4];
    uint8_t kind;      /* rdof_unit_kind_t */
    int rx, tx;        /* device indexes: one device has rx == tx; a receiver's tx is -1 */
    int app;           /* a repeater: its app; else -1 */
    int freq;          /* an SFR or DCDM gateway: its frequency; a transceiver: its idle frequency; else -1 */
    uint64_t freqs;    /* a transceiver: the frequencies it transmits on; a receiver: the ones it hears (bit each) */
    bool duplex_full;  /* a DCDM gateway: "duplex": "full" */
} rdof_unit_t;

/* A frequency an SDR hears for a unit on it that its receiver doesn't (or that has no receiver): a channel of the
   SDR's of its own, reported to no one. */
typedef struct {
    uint32_t hz;
    int cc;            /* the unit's color code there, else 1 */
    int dev;           /* the SDR */
    char id[RDOF_SITE_NAME + 8]; /* "<unit>-<n>" */
} rdof_plan_extra_t;

typedef struct rdof_plan rdof_plan_t;

/* The settings, version 3, parsed and checked: NULL with err set ("path: message", path as devices.w12.rx.rate). */
rdof_plan_t *rdof_plan_open(const char *text, size_t len, char *err, size_t cap);
void rdof_plan_close(rdof_plan_t *p);

const rdof_site_t *rdof_plan_site(const rdof_plan_t *p);
/* The document (as rdof/jsonp.h reads it) and its top object. */
const rdof_jdoc_t *rdof_plan_doc(const rdof_plan_t *p);
int rdof_plan_root(const rdof_plan_t *p);
/* Each entry's object in the document, -1 none (a frequency written as a bare number). */
int rdof_plan_dev_node(const rdof_plan_t *p, int dev);
int rdof_plan_freq_node(const rdof_plan_t *p, int freq);
int rdof_plan_app_node(const rdof_plan_t *p, int app);
int rdof_plan_net_node(const rdof_plan_t *p, int net);
/* The first network of a type (RDOF_NET_HBLINK, RDOF_NET_MQTT), -1 none. */
int rdof_plan_net_of(const rdof_plan_t *p, int type);
const rdof_plan_dev_t *rdof_plan_dev(const rdof_plan_t *p, int dev);

int rdof_plan_nunits(const rdof_plan_t *p);
const rdof_unit_t *rdof_plan_unit(const rdof_plan_t *p, int k);
/* By name, -1 none. */
int rdof_plan_unit_find(const rdof_plan_t *p, const char *name);
/* The extra channels, in unit order. */
int rdof_plan_nextra(const rdof_plan_t *p);
const rdof_plan_extra_t *rdof_plan_extra(const rdof_plan_t *p, int k);
/* bench.devices: how many, and each one's device index. */
int rdof_plan_nbench(const rdof_plan_t *p);
int rdof_plan_bench(const rdof_plan_t *p, int k);
/* An SDR's receiver unit, -1 none; whether any unit uses the SDR. */
int rdof_plan_receiver(const rdof_plan_t *p, int dev);
bool rdof_plan_sdr_used(const rdof_plan_t *p, int dev);
/* An SDR's name for its settings and the status: its receiver's, else the device's. */
const char *rdof_plan_sdr_name(const rdof_plan_t *p, int dev);

/* What a platform opens: the board its UARTs and USB port, a host serial paths and USB devices and its own SDR
   drivers (soapy, iio). The open checks what each SDR's driver can do; this, what the platform can. */
typedef struct {
    const char *name; /* "this board", "this host": for the messages */
    bool uart_ports, path_ports;
    bool host_drivers;
    int max_radios;   /* units that transmit at most */
} rdof_plan_platform_t;

extern const rdof_plan_platform_t rdof_plan_board, rdof_plan_host;

/* The plan's devices on plat: false with err set. */
bool rdof_plan_check(const rdof_plan_t *p, const rdof_plan_platform_t *plat, char *err, size_t cap);

/* What a platform runs: every unit, at most its max_radios that transmit, and one SDR. false with err set.
   rdof_plan_roles is the board's (RDOF_MAX_RADIOS). */
bool rdof_plan_roles_on(const rdof_plan_t *p, const rdof_plan_platform_t *plat, rdof_roles_t *roles, char *err,
                        size_t cap);
bool rdof_plan_roles(const rdof_plan_t *p, rdof_roles_t *roles, char *err, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
