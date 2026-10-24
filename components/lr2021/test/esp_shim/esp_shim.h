// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#pragma once
/* The fake chip and board behind the shimmed ESP-IDF calls. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SHIM_MAX_XFER 128

typedef struct {
    /* time: advanced by esp_rom_delay_us and vTaskDelay only */
    int64_t now_us;
    /* BUSY: high until busy_until_us, or for good */
    int busy_pin, nreset_pin;
    int64_t busy_until_us;
    bool busy_stuck;
    int busy_polls;
    /* NRESET history: levels set, in order, with the time */
    int nreset_levels[16];
    int64_t nreset_times[16];
    int n_nreset;
    int gpio_configs;
    uint64_t gpio_masks[4];
    int gpio_modes[4];
    /* SPI */
    int bus_inits, bus_frees, adds, removes;
    int last_host;
    esp_err_t bus_init_err, add_err;
    int clock_hz, spics_io_num, mosi, miso, sclk, max_transfer_sz, dma;
    uint8_t spi_mode, cs_pretrans;
    int xfers;               /* transactions so far */
    int dma_xfers;           /* of them, by interrupt (spi_device_transmit) rather than polled */
    int fail_xfer_at;        /* the Nth transaction (1-based) returns xfer_err; 0 never */
    esp_err_t xfer_err;
    uint8_t tx[SHIM_MAX_XFER][1030];
    size_t tx_len[SHIM_MAX_XFER];
    size_t rx_len[SHIM_MAX_XFER]; /* bytes received into rx_buffer (0: none) */
    int bounces;             /* transactions whose rx IDF would bounce: not 4-aligned or not a multiple of 4 long */
    /* The chip: fills rx for transaction `idx` (0-based) given what was sent. Default: zeros. */
    void (*respond)(int idx, const uint8_t *tx, size_t len, uint8_t *rx);
    /* the IRQ pin: its level, the ISR service and handler, the handler's semaphore */
    int irq_pin, irq_level;
    int isr_installs, isr_adds, isr_removes, isr_pin;
    esp_err_t isr_install_err;
    int irq_intr_type;
    void (*isr)(void *);
    void *isr_arg;
    int sems, sem_deletes, sem_count, sem_waits;
    /* heap */
    bool alloc_fail;
    int allocs, frees;
    size_t dma_free;         /* the largest internal DMA block; 0: plenty */
} shim_t;

extern shim_t F;
void shim_reset(void);
