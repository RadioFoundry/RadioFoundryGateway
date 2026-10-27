// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The HAL on ESP-IDF (hal/esp_idf.c): ESP-IDF's SPI master with DMA (NSS driven by the peripheral), BUSY and
 * NRESET plain GPIOs, the IRQ a GPIO with a rising-edge interrupt.
 *
 * The handle (lr2021_config_t.mem) must be internal, DMA-capable RAM: MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA.
 */
#ifndef LR2021_HAL_ESP_IDF_H
#define LR2021_HAL_ESP_IDF_H

#include <stdbool.h>

#include "lr2021_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int spi_host;          /* SPI2_HOST or SPI3_HOST */
    int sck, mosi, miso, nss, busy, nreset;
    int irq;               /* the GPIO a DIO with IRQs drives, -1 for none */
    int pa_en;             /* a GPIO driven high while open (a front-end supply), -1 for none */
    int clock_hz;          /* 16 MHz at most (3.7.2); 0: 16 MHz */
    bool bus_initialized;  /* the app set up the bus already (shared with other devices) */
} lr2021_esp_cfg_t;

/* Set up the bus and pins into *hal. */
lr2021_err_t lr2021_hal_esp_open(const lr2021_esp_cfg_t *cfg, lr2021_hal_t *hal);
/* Transfers whose answer wasn't 4-aligned or a multiple of 4 long, so ESP-IDF's SPI driver allocated an internal DMA
 * buffer for it (the IRQ word, a register read). hal is one lr2021_hal_esp_open filled. */
uint32_t lr2021_hal_esp_bounces(const lr2021_hal_t *hal);

#ifdef __cplusplus
}
#endif

#endif
