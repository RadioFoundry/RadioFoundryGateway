// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#define SPI_DMA_CH_AUTO 3
typedef struct spi_dev *spi_device_handle_t;
typedef struct {
    int mosi_io_num, miso_io_num, sclk_io_num, quadwp_io_num, quadhd_io_num;
    int max_transfer_sz;
} spi_bus_config_t;
typedef struct {
    uint8_t mode;
    int clock_speed_hz;
    int spics_io_num;
    int queue_size;
    int input_delay_ns;
    uint8_t cs_ena_pretrans;
} spi_device_interface_config_t;
typedef struct {
    size_t length;   /* bits */
    size_t rxlength; /* bits; 0: length */
    const void *tx_buffer;
    void *rx_buffer;
} spi_transaction_t;
esp_err_t spi_bus_initialize(int host, const spi_bus_config_t *cfg, int dma);
esp_err_t spi_bus_free(int host);
esp_err_t spi_bus_add_device(int host, const spi_device_interface_config_t *cfg, spi_device_handle_t *out);
esp_err_t spi_bus_remove_device(spi_device_handle_t h);
esp_err_t spi_device_polling_transmit(spi_device_handle_t h, spi_transaction_t *t);
esp_err_t spi_device_transmit(spi_device_handle_t h, spi_transaction_t *t);
