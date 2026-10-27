// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The chip's pins, as the driver (lr2021.h) uses them: an SPI bus with the chip's NSS, BUSY, NRESET and optionally
 * a DIO wired as an IRQ. A backend fills an lr2021_hal_t: hal/linux.c (spidev and the GPIO character
 * device), hal/esp_idf.c (ESP-IDF's SPI master), or the tests' mock.
 */
#ifndef LR2021_HAL_H
#define LR2021_HAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lr2021_port.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *ctx;
    /* One frame, NSS low for all of it: len bytes out of tx, and the first rx_len of the bytes clocked in at the same
     * time into rx (rx_len 0: rx unused, may be NULL). rx_len <= len. The driver keeps tx and rx 4-byte aligned and
     * rounds rx_len up to 4 where the frame allows (ESP32 DMA needs both, or it allocates a bounce buffer). */
    lr2021_err_t (*xfer)(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len, size_t rx_len);
    /* BUSY's level: 1 high (the chip is working), 0 low, < 0 an error. */
    int (*busy)(void *ctx);
    /* Drive NRESET (1 high, 0 low: in reset). */
    lr2021_err_t (*nreset)(void *ctx, int level);
    /* Optional. Wait until the IRQ pin is high: true at once if it is, false after timeout_ms. NULL: no IRQ pin. */
    bool (*wait_irq)(void *ctx, int timeout_ms);
    /* Optional. The IRQ pin's level now (1, 0; < 0 none). */
    int (*irq)(void *ctx);
    /* Optional. Change the SPI clock (the chip takes 16 MHz at most, DS 3.7.2); input_delay_ns as ESP-IDF's
     * MISO allowance, 0 for none. NULL: fixed. */
    lr2021_err_t (*set_clock)(void *ctx, int hz, int input_delay_ns);
    /* Optional. Release the bus and pins (lr2021_close calls it). */
    void (*close)(void *ctx);
} lr2021_hal_t;

#ifdef __cplusplus
}
#endif

#endif
