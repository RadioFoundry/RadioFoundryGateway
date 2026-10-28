// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The broker (docs/spec/fw/rxtx-combinations.md section 3): what each device gives (an SDR's RX window and channels
 * and its TX carriers, a whole modem, a radio's link), who owns each, and what a platform can run. Roles claim at
 * start-up; bench sessions claim and release around their jobs. Checking settings is the broker run dry: every role
 * run names claims what it needs, and the first thing that runs out is the error.
 *
 * No I/O: the runtime gives each device's caps (from its front end or its modem's description) and the platform's
 * limits; without them each device has its driver's defaults (rdof_broker_default_caps). Portable C.
 */
#ifndef RDOF_BROKER_H
#define RDOF_BROKER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/setup.h"

#ifdef __cplusplus
extern "C" {
#endif

/* An SDR's TX carriers, the most the format and the code take (rdof/sdr/sdrtx.h); a front end says how many it
   runs (rdof_dev_caps_t.tx_carriers, 1 for now). */
#define RDOF_BROKER_MAX_CARRIERS 4
#define RDOF_BROKER_MAX_CHANNELS 16 /* an SDR's receive channels (RDOF_SDR_MAX_CHANNELS) */

typedef struct {
    bool rx, tx;
    bool full_duplex;     /* RX while it transmits */
    int rx_channels;      /* receive channels it can run at once */
    int tx_carriers;      /* carriers it can send at once */
    double rx_rate_max;   /* 0: not known */
    uint32_t freq_min, freq_max; /* 0, 0: not known */
} rdof_dev_caps_t;

/* What a platform runs: the board from its RAM, UARTs and USB port; a host from its compile-time caps. */
typedef struct {
    const char *name;     /* "this board", "this host": for the messages */
    int max_talkers;      /* roamers, repeaters, SFR and DCDM roles at once */
    int max_sdrs;         /* SDR devices in use at once */
    bool path_ports;      /* a serial device by path (a host) */
    bool uart_ports;      /* the MCU's UARTs (the board) */
    bool sdr_split_half;  /* an SDR as a split's RX half (the split's clock and the SDR's tied together) */
    bool sdr_drivers_host;/* soapy, iio */
} rdof_platform_t;

/* The board's and a host's defaults. */
extern const rdof_platform_t rdof_platform_board, rdof_platform_host;
/* A device's caps by its driver, until the runtime knows better. */
void rdof_broker_default_caps(const rdof_device_t *dev, rdof_dev_caps_t *caps);

/* Who owns a resource: nobody, a role (its index), or a bench session (its id). */
#define RDOF_OWNER_NONE 0
#define RDOF_OWNER_ROLE(i) (1 + (i))
#define RDOF_OWNER_BENCH(id) (0x10000 + (id))
#define RDOF_OWNER_IS_ROLE(o) ((o) >= 1 && (o) < 0x10000)

typedef enum { RDOF_DIR_RX = 1, RDOF_DIR_TX = 2 } rdof_dir_t;

typedef struct {
    rdof_dev_caps_t caps;
    bool unavailable;     /* gone (unplugged, an error): its owners are told, auto claims can move */
    int owner;            /* a modem's or a radio's: one owner */
    int carrier_owner[RDOF_BROKER_MAX_CARRIERS];
    uint32_t carrier_freq[RDOF_BROKER_MAX_CARRIERS];
    int channels;         /* receive channels in use (shared: listeners don't add) */
    uint32_t chan_freq[RDOF_BROKER_MAX_CHANNELS];
    int exclusive;        /* an owner holding the RX window (a retune); 0 none */
} rdof_broker_dev_t;

typedef struct {
    const rdof_setup_t *setup;
    rdof_platform_t plat;
    rdof_broker_dev_t dev[RDOF_SETUP_MAX_DEVICES];
} rdof_broker_t;

/* A request: a device by index, or -1 for auto (the broker picks by frequency and caps). */
typedef struct {
    rdof_dir_t dir;
    int device;
    uint32_t freq;
    bool exclusive;       /* RX: retune the window (a capture off the channels) */
} rdof_claim_t;

void rdof_broker_init(rdof_broker_t *b, const rdof_setup_t *setup, const rdof_platform_t *plat);
void rdof_broker_set_caps(rdof_broker_t *b, int dev, const rdof_dev_caps_t *caps);
/* Claim what role i needs. false with error set ("devices.hs1: both R1 and R2 take it"). */
bool rdof_broker_claim_role(rdof_broker_t *b, int role, char *error, size_t cap);
/* One resource for owner: the device it's on (>= 0), -1 busy (someone has it), -2 nothing could ever (no device
   fits; error set). */
int rdof_broker_claim(rdof_broker_t *b, const rdof_claim_t *req, int owner, char *error, size_t cap);
/* Everything owner holds, back. */
void rdof_broker_release(rdof_broker_t *b, int owner);
/* Every role run names, claimed in order, on this platform: the settings check. */
bool rdof_broker_check(const rdof_setup_t *setup, const rdof_platform_t *plat, char *error, size_t cap);
/* "auto" devices in roles, picked (rdof_setup_parse calls it). false with error set when none fits. */
bool rdof_broker_resolve_auto(rdof_setup_t *setup, char *error, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
