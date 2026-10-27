// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#pragma once
/* The fake chip and board behind a mock HAL (lr2021_hal.h) and a fake clock (lr2021_port.h), for the host tests. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lr2021.h"

#define SHIM_MAX_XFER 128

typedef struct {
    /* time: advanced by lr2021_delay_us, lr2021_sleep_tick / _ms and wait_irq only */
    int64_t now_us;
    /* BUSY: high until busy_until_us, or for good */
    int64_t busy_until_us;
    bool busy_stuck;
    int busy_polls;
    /* NRESET history: levels set, in order, with the time */
    int nreset_levels[16];
    int64_t nreset_times[16];
    int n_nreset;
    /* SPI */
    int clock_hz, input_delay_ns, clock_sets;
    int xfers;               /* transfers so far */
    int fail_xfer_at;        /* the Nth transfer (1-based) returns xfer_err; 0 never */
    lr2021_err_t xfer_err;
    uint8_t tx[SHIM_MAX_XFER][1030];
    size_t tx_len[SHIM_MAX_XFER];
    size_t rx_len[SHIM_MAX_XFER]; /* bytes received into rx (0: none) */
    int unaligned;           /* transfers whose rx wasn't 4-aligned or a multiple of 4 long (an ESP32 would bounce) */
    /* The chip: fills rx for transfer `idx` (0-based) given what was sent. Default: zeros. */
    void (*respond)(int idx, const uint8_t *tx, size_t len, uint8_t *rx);
    /* the IRQ pin: present, its level, how often it was waited on */
    bool has_irq;
    int irq_level, irq_waits;
    /* the HAL's close */
    int closes;
    /* called after the clock moves (a model of the chip catching up) */
    void (*on_time)(void);
} shim_t;

extern shim_t F;
void shim_reset(void);
/* A config with the mock HAL (and an IRQ pin when F.has_irq is set before the call). */
lr2021_config_t mock_config(void);
