// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The bench API's radios (docs/spec/bench-api.md section 8): the modems and radios that "bench": {"devices": [...]}
 * in the settings names (version 3, docs/spec/fw/config-v3.md), each a device with its port:
 *
 *   {"kind": "radio", "name": "odr1", "port": {"uart": 1, "tx": 17, "rx": 18, "baud": 921600}}
 *   {"kind": "modem", "name": "hs1",  "port": {"usb": "cdc-acm", "vid": "0483", "pid": "5740"}}
 *   {"kind": "radio", "name": "odr2", "port": {"usb": "uart", "serial": "FT1A2B3C", "baud": 921600}}
 *
 * A radio speaks odr-debug, a modem mmdvm. uart: one of the MCU's UARTs (1 or 2) with its pins, optional "rts" /
 * "cts". usb: a device on the OTG port, alone or behind a hub: "cdc-acm" (vid and pid, hex strings or numbers), or
 * "uart" for a USB-serial bridge (FTDI, CP210x, CH34x; vid / pid optional: any of that chip). "serial" and
 * "usb_port" (hub ports, "1.3") are kept for telling two identical devices apart; the firmware doesn't use them yet
 * (it opens the first match). A bench SDR isn't one of these.
 * Portable C.
 */
#ifndef RDOF_BENCHCFG_H
#define RDOF_BENCHCFG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/benchtx.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_BENCH_MAX_RADIOS 4

typedef enum { RDOF_BPORT_UART = 1, RDOF_BPORT_CDC_ACM, RDOF_BPORT_USB_UART } rdof_bench_port_t;

typedef struct {
    char name[24];
    char proto[12];     /* "odr-debug" or "mmdvm" */
    rdof_bench_port_t port;
    int uart, tx, rx, rts, cts; /* RDOF_BPORT_UART; rts / cts -1: none */
    long baud;          /* UART and USB-serial bridges */
    uint16_t vid, pid;  /* USB; 0: any of the kind */
    char serial[32];
    char usb_port[16];
} rdof_bench_radio_t;

/* The bench's radios and modems: how many (0 when there's no "bench"), or -1 with error set. */
int rdof_bench_radios_parse(const char *text, size_t len, rdof_bench_radio_t *out, int max, char *error, size_t cap);

/* bench.tx, what the bench may transmit (section 7), with each bench device's own "bench": {"freqs_hz": [...]}
 * after its own:
 *
 *   "tx": {"freqs_hz": [438162500], "bands_hz": [[144000000, 148000000]], "max_key_s": 10, "rf_level": 0,
 *          "arm_gpio": 5}
 *
 * bands_hz: up to 8 [low, high], any frequency from one to the other. No bench.tx (or no freqs_hz and no bands_hz):
 * no TX. max_key_s defaults to 10 (at most 60), rf_level to 0 (the lowest), arm_gpio
 * to none. 0, or -1 with error set. */
int rdof_bench_tx_parse(const char *text, size_t len, rdof_benchtx_policy_t *out, char *error, size_t cap);

/* The same from a plan (rdof/siteplan.h); the text forms above open one each. */
struct rdof_plan;
int rdof_plan_bench_radios(const struct rdof_plan *p, rdof_bench_radio_t *out, int max, char *error, size_t cap);
int rdof_plan_bench_tx(const struct rdof_plan *p, rdof_benchtx_policy_t *out, char *error, size_t cap);

/* A USB-serial bridge's chip by its VID / PID: 'f' FTDI, 'c' CP210x, 'h' CH34x, 0 none. */
char rdof_bench_usb_bridge(uint16_t vid, uint16_t pid);

#ifdef __cplusplus
}
#endif

#endif
