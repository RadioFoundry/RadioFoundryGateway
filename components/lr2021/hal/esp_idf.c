// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The HAL on ESP-IDF (lr2021_hal_esp_idf.h): ESP-IDF's SPI master with DMA, NSS driven by the peripheral; BUSY and
 * NRESET plain GPIOs; the IRQ a GPIO with a rising-edge interrupt.
 */
#include "lr2021_hal_esp_idf.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define MAX_FIFO_FRAME 1028 /* a 1 KB FIFO transfer and its two status bytes, to 4 */
#define DMA_MIN 32          /* frames this long or longer go by interrupt: the task sleeps while DMA moves them */

typedef struct {
    lr2021_esp_cfg_t cfg;
    spi_device_handle_t spi;
    bool own_bus;
    SemaphoreHandle_t irq_sem; /* given by the IRQ pin's rising edge */
    uint32_t bounces;          /* transfers whose rx the SPI driver had to bounce */
} esp_hal_t;

static IRAM_ATTR lr2021_err_t xfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len, size_t rx_len)
{
    esp_hal_t *h = ctx;
    if (!h->spi)
        return LR2021_ERR_INVALID_STATE; /* a clock change failed */
    if (rx_len && (((uintptr_t)rx | rx_len) & 3)) {
        /* The driver allocates an internal DMA buffer for this one, and IDF 5.5's clean-up copies from NULL when
         * that allocation fails: refuse it while internal DMA memory is short. (The largest block costs a heap
         * walk: callers that read often keep their frames a multiple of 4.) */
        if (heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL) < rx_len + 64)
            return LR2021_ERR_NO_MEM;
        h->bounces++;
    }
    spi_transaction_t t = {.length = len * 8, .rxlength = rx_len * 8, .tx_buffer = tx, .rx_buffer = rx_len ? rx : NULL};
    /* Short frames poll (an interrupt's latency is longer than the frame); long ones block on the transfer's
     * interrupt while the SPI's DMA fills rx. */
    esp_err_t e = len >= DMA_MIN ? spi_device_transmit(h->spi, &t) : spi_device_polling_transmit(h->spi, &t);
    return e;
}

static IRAM_ATTR int busy(void *ctx) { return gpio_get_level(((esp_hal_t *)ctx)->cfg.busy); }

static lr2021_err_t nreset(void *ctx, int level) { return gpio_set_level(((esp_hal_t *)ctx)->cfg.nreset, level); }

static void IRAM_ATTR irq_isr(void *arg)
{
    BaseType_t woke = pdFALSE;
    xSemaphoreGiveFromISR(((esp_hal_t *)arg)->irq_sem, &woke);
    if (woke)
        portYIELD_FROM_ISR();
}

static IRAM_ATTR bool wait_irq(void *ctx, int timeout_ms)
{
    esp_hal_t *h = ctx;
    if (!h->irq_sem)
        return false;
    xSemaphoreTake(h->irq_sem, 0); /* an edge from before: the level says what's pending now */
    if (gpio_get_level(h->cfg.irq))
        return true;
    TickType_t t = pdMS_TO_TICKS(timeout_ms);
    if (!t && timeout_ms > 0)
        t = 1; /* under a tick: one tick */
    return xSemaphoreTake(h->irq_sem, t) == pdTRUE || gpio_get_level(h->cfg.irq);
}

static IRAM_ATTR int irq(void *ctx)
{
    esp_hal_t *h = ctx;
    return h->cfg.irq >= 0 ? gpio_get_level(h->cfg.irq) : -1;
}

static esp_err_t add_device(esp_hal_t *h, int hz, int input_delay_ns)
{
    spi_device_interface_config_t dev = {.mode = 0, .clock_speed_hz = hz, .spics_io_num = h->cfg.nss,
                                         .queue_size = 1, .cs_ena_pretrans = 1, .input_delay_ns = input_delay_ns};
    return spi_bus_add_device(h->cfg.spi_host, &dev, &h->spi);
}

static lr2021_err_t set_clock(void *ctx, int hz, int input_delay_ns)
{
    esp_hal_t *h = ctx;
    if (h->spi) {
        esp_err_t e = spi_bus_remove_device(h->spi);
        if (e != ESP_OK)
            return e;
        h->spi = NULL;
    }
    h->cfg.clock_hz = hz;
    return add_device(h, hz, input_delay_ns);
}

static void hal_close(void *ctx)
{
    esp_hal_t *h = ctx;
    if (h->irq_sem) {
        gpio_isr_handler_remove(h->cfg.irq);
        vSemaphoreDelete(h->irq_sem);
    }
    if (h->spi)
        spi_bus_remove_device(h->spi);
    if (h->own_bus)
        spi_bus_free(h->cfg.spi_host);
    if (h->cfg.pa_en >= 0)
        gpio_set_level(h->cfg.pa_en, 0);
    free(h);
}

static esp_err_t irq_setup(esp_hal_t *h)
{
    gpio_config_t g = {.pin_bit_mask = 1ULL << h->cfg.irq, .mode = GPIO_MODE_INPUT, .intr_type = GPIO_INTR_POSEDGE};
    esp_err_t e = gpio_config(&g);
    if (e != ESP_OK)
        return e;
    e = gpio_install_isr_service(0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) /* another driver installed it */
        return e;
    if (!(h->irq_sem = xSemaphoreCreateBinary()))
        return ESP_ERR_NO_MEM;
    return gpio_isr_handler_add(h->cfg.irq, irq_isr, h);
}

lr2021_err_t lr2021_hal_esp_open(const lr2021_esp_cfg_t *cfg, lr2021_hal_t *hal)
{
    esp_hal_t *h = heap_caps_calloc(1, sizeof *h, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!h)
        return LR2021_ERR_NO_MEM;
    h->cfg = *cfg;
    if (h->cfg.clock_hz <= 0 || h->cfg.clock_hz > 16000000)
        h->cfg.clock_hz = 16000000;
    if (cfg->pa_en >= 0) {
        gpio_config_t o = {.pin_bit_mask = 1ULL << cfg->pa_en, .mode = GPIO_MODE_OUTPUT};
        gpio_config(&o);
        gpio_set_level(cfg->pa_en, 1);
    }
    gpio_config_t in = {.pin_bit_mask = 1ULL << cfg->busy, .mode = GPIO_MODE_INPUT};
    gpio_config(&in);
    gpio_config_t rst = {.pin_bit_mask = 1ULL << cfg->nreset, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&rst);
    gpio_set_level(cfg->nreset, 1);
    esp_err_t e = ESP_OK;
    if (!cfg->bus_initialized) {
        spi_bus_config_t bus = {.mosi_io_num = cfg->mosi, .miso_io_num = cfg->miso, .sclk_io_num = cfg->sck,
                                .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = MAX_FIFO_FRAME};
        e = spi_bus_initialize(cfg->spi_host, &bus, SPI_DMA_CH_AUTO);
        h->own_bus = e == ESP_OK;
    }
    if (e == ESP_OK)
        e = add_device(h, h->cfg.clock_hz, 0);
    if (e == ESP_OK && cfg->irq >= 0)
        e = irq_setup(h);
    if (e != ESP_OK) {
        hal_close(h);
        return e;
    }
    *hal = (lr2021_hal_t){.ctx = h, .xfer = xfer, .busy = busy, .nreset = nreset, .set_clock = set_clock,
                          .close = hal_close};
    if (cfg->irq >= 0) {
        hal->wait_irq = wait_irq;
        hal->irq = irq;
    }
    return LR2021_OK;
}

uint32_t lr2021_hal_esp_bounces(const lr2021_hal_t *hal) { return ((const esp_hal_t *)hal->ctx)->bounces; }
