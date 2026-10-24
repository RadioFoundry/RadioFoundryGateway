// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* See esp_shim.h. */
#include "esp_shim.h"

#include <stdlib.h>
#include <string.h>

#undef free
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#undef free
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

shim_t F;
static struct spi_dev {
    int host;
} the_dev;

void shim_reset(void)
{
    memset(&F, 0, sizeof F);
    F.busy_pin = -1;
    F.nreset_pin = -1;
    F.irq_pin = -1;
}

int gpio_config(const gpio_config_t *cfg)
{
    if (F.gpio_configs < 4) {
        F.gpio_masks[F.gpio_configs] = cfg->pin_bit_mask;
        F.gpio_modes[F.gpio_configs] = cfg->mode;
    }
    if (F.irq_pin >= 0 && cfg->pin_bit_mask == 1ULL << F.irq_pin)
        F.irq_intr_type = cfg->intr_type;
    F.gpio_configs++;
    return ESP_OK;
}

int gpio_get_level(int pin)
{
    if (pin == F.irq_pin)
        return F.irq_level;
    if (pin != F.busy_pin) {
        return 0;
    }
    F.busy_polls++;
    return F.busy_stuck || F.now_us < F.busy_until_us;
}

int gpio_set_level(int pin, uint32_t level)
{
    if (pin == F.nreset_pin && F.n_nreset < 16) {
        F.nreset_levels[F.n_nreset] = (int)level;
        F.nreset_times[F.n_nreset++] = F.now_us;
    }
    return ESP_OK;
}

esp_err_t spi_bus_initialize(int host, const spi_bus_config_t *cfg, int dma)
{
    F.bus_inits++;
    F.last_host = host;
    F.mosi = cfg->mosi_io_num;
    F.miso = cfg->miso_io_num;
    F.sclk = cfg->sclk_io_num;
    F.max_transfer_sz = cfg->max_transfer_sz;
    F.dma = dma;
    return F.bus_init_err;
}

esp_err_t spi_bus_free(int host)
{
    F.bus_frees++;
    F.last_host = host;
    return ESP_OK;
}

esp_err_t spi_bus_add_device(int host, const spi_device_interface_config_t *cfg, spi_device_handle_t *out)
{
    F.adds++;
    if (F.add_err) {
        return F.add_err;
    }
    F.spi_mode = cfg->mode;
    F.clock_hz = cfg->clock_speed_hz;
    F.spics_io_num = cfg->spics_io_num;
    F.cs_pretrans = cfg->cs_ena_pretrans;
    the_dev.host = host;
    *out = &the_dev;
    return ESP_OK;
}

esp_err_t spi_bus_remove_device(spi_device_handle_t h)
{
    (void)h;
    F.removes++;
    return ESP_OK;
}

esp_err_t spi_device_transmit(spi_device_handle_t h, spi_transaction_t *t)
{
    F.dma_xfers++;
    return spi_device_polling_transmit(h, t);
}

int gpio_install_isr_service(int flags)
{
    (void)flags;
    F.isr_installs++;
    return F.isr_install_err;
}

int gpio_isr_handler_add(int pin, gpio_isr_t isr, void *arg)
{
    F.isr_adds++;
    F.isr_pin = pin;
    F.isr = isr;
    F.isr_arg = arg;
    return ESP_OK;
}

int gpio_isr_handler_remove(int pin)
{
    F.isr_removes++;
    F.isr = NULL;
    return ESP_OK;
}

static struct shim_sem {
    int unused;
} the_sem;

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    F.sems++;
    F.sem_count = 0;
    return &the_sem;
}

void vSemaphoreDelete(SemaphoreHandle_t s)
{
    (void)s;
    F.sem_deletes++;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks)
{
    (void)s;
    if (F.sem_count) {
        F.sem_count = 0;
        return pdTRUE;
    }
    if (ticks) {
        F.sem_waits++;
        F.now_us += (int64_t)ticks * 10000;
    }
    return pdFALSE;
}

BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t s, BaseType_t *woke)
{
    (void)s;
    F.sem_count = 1;
    *woke = pdTRUE;
    return pdTRUE;
}

esp_err_t spi_device_polling_transmit(spi_device_handle_t h, spi_transaction_t *t)
{
    (void)h;
    int idx = F.xfers++;
    size_t len = t->length / 8, rxl = t->rx_buffer ? (t->rxlength ? t->rxlength : t->length) / 8 : 0;
    if (idx < SHIM_MAX_XFER) {
        memcpy(F.tx[idx], t->tx_buffer, len);
        F.tx_len[idx] = len;
        F.rx_len[idx] = rxl;
    }
    if (rxl && (((uintptr_t)t->rx_buffer | rxl) & 3)) {
        F.bounces++; /* ESP-IDF would allocate an internal DMA buffer for this one (setup_dma_priv_buffer) */
    }
    if (F.fail_xfer_at && idx + 1 == F.fail_xfer_at) {
        return F.xfer_err;
    }
    static uint8_t frame[1030];
    memset(frame, 0, len);
    if (F.respond) {
        F.respond(idx, t->tx_buffer, len, frame);
    }
    if (rxl) {
        memcpy(t->rx_buffer, frame, rxl);
    }
    return ESP_OK;
}

void *heap_caps_calloc(size_t n, size_t size, int caps)
{
    (void)caps;
    if (F.alloc_fail) {
        return NULL;
    }
    F.allocs++;
    return calloc(n, size);
}

size_t heap_caps_get_largest_free_block(int caps)
{
    (void)caps;
    return F.dma_free ? F.dma_free : (size_t)1 << 20;
}

void shim_free(void *p)
{
    if (p) {
        F.frees++;
    }
    free(p);
}

void esp_rom_delay_us(uint32_t us) { F.now_us += us; }
int64_t esp_timer_get_time(void) { return F.now_us; }
void vTaskDelay(TickType_t ticks) { F.now_us += (int64_t)ticks * 10000; }
