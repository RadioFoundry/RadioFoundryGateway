// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The LR2021 over ESP-IDF's SPI master (lr2021.h). NSS is driven by the SPI
 * peripheral; BUSY and NRESET are plain GPIOs. Every frame waits for BUSY low
 * first (5.2: a command sent while BUSY is high may not run).
 */
#include "lr2021.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define BUSY_TIMEOUT_US 200000 /* calibrations take a few ms; a stuck BUSY means a dead or unpowered chip */
#define MAX_FRAME 260          /* a command frame, or an answer and its two status bytes */
#define MAX_FIFO_FRAME 1026    /* a 1 KB FIFO transfer (the extended FIFO, 5.3.3) and its two status bytes */
#define DMA_MIN 32             /* frames this long or longer go by interrupt: the task sleeps while DMA moves them */

struct lr2021 {
    lr2021_config_t cfg;
    spi_device_handle_t spi;
    /* DMA-able: the handle is in internal RAM. Aligned to 4, the S3's DMA receive alignment: with the received
     * length a multiple of 4 too, the driver uses rx as it is (else it allocates a bounce buffer for the transfer,
     * and when that allocation fails IDF 5.5's clean-up copies from NULL: xfer checks first). */
    uint8_t tx[MAX_FIFO_FRAME] __attribute__((aligned(4))), rx[MAX_FIFO_FRAME] __attribute__((aligned(4)));
    char error[96];
    uint32_t failures;
    uint32_t bounces;          /* transfers whose rx the driver had to bounce (lr2021_bounces) */
    bool own_mem;              /* allocated here, not the caller's cfg.mem */
    SemaphoreHandle_t irq_sem; /* given by the IRQ pin's rising edge */
};

size_t lr2021_handle_size(void) { return sizeof(struct lr2021); }

static esp_err_t fail(lr2021_t *d, esp_err_t e, const char *what)
{
    snprintf(d->error, sizeof d->error, "%s", what);
    d->failures++;
    return e;
}

static esp_err_t wait_busy(lr2021_t *d)
{
    int64_t t0 = esp_timer_get_time();
    while (gpio_get_level(d->cfg.busy)) {
        int64_t dt = esp_timer_get_time() - t0;
        if (dt > BUSY_TIMEOUT_US)
            return fail(d, ESP_ERR_TIMEOUT, "BUSY stayed high");
        if (dt > 2000)
            vTaskDelay(1);
        else
            esp_rom_delay_us(5);
    }
    return ESP_OK;
}

/* A frame of len bytes, of which the first want come back in rx (0: none). The bus has DMA, so every transfer
 * goes through it, short ones too: its rx must be 4-byte aligned and a multiple of 4 long, or the driver
 * allocates an internal bounce buffer for it (and IDF 5.5's clean-up copies from NULL when that allocation
 * fails). So rx is received rounded up to 4 where the frame is that long (the status of a 6-byte command: 4),
 * and not at all when nothing is wanted. What's left over (an answer that ends off a multiple of 4: the IRQ
 * word, a register read) is counted in bounces and refused while internal DMA memory is short. */
static esp_err_t xfer_rx(lr2021_t *d, size_t len, size_t want)
{
    if (!d->spi)
        return fail(d, ESP_ERR_INVALID_STATE, "no SPI device (a clock change failed)");
    esp_err_t e = wait_busy(d);
    if (e != ESP_OK)
        return e;
    size_t rx = want ? (want + 3) & ~(size_t)3 : 0;
    if (rx > len)
        rx = want; /* can't round up past the frame */
    if (rx & 3) {
        /* (The largest block costs a heap walk: callers that read often keep their frames a multiple of 4.) */
        if (heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL) < rx + 64)
            return fail(d, ESP_ERR_NO_MEM, "no internal DMA memory for an unaligned transfer");
        d->bounces++;
    }
    spi_transaction_t t = {.length = len * 8, .rxlength = rx * 8, .tx_buffer = d->tx, .rx_buffer = rx ? d->rx : NULL};
    /* Short frames poll (an interrupt's latency is longer than the frame); long ones block on the
     * transfer's interrupt while the SPI's DMA fills rx. */
    e = len >= DMA_MIN ? spi_device_transmit(d->spi, &t) : spi_device_polling_transmit(d->spi, &t);
    return e == ESP_OK ? ESP_OK : fail(d, e, "SPI transfer failed");
}

esp_err_t lr2021_wake(lr2021_t *d)
{
    if (!d->spi)
        return fail(d, ESP_ERR_INVALID_STATE, "no SPI device (a clock change failed)");
    /* NSS low for 100 us wakes it (4.4.1): a frame of zeros that long, with a margin */
    size_t n = ((size_t)((int64_t)d->cfg.clock_hz * 120 / 8000000) + 4) & ~(size_t)3;
    if (n > MAX_FIFO_FRAME)
        n = MAX_FIFO_FRAME & ~(size_t)3;
    memset(d->tx, 0, n);
    spi_transaction_t t = {.length = n * 8, .tx_buffer = d->tx};
    esp_err_t e = spi_device_transmit(d->spi, &t);
    if (e != ESP_OK)
        return fail(d, e, "SPI transfer failed");
    return wait_busy(d);
}

/* A frame whose answer is all of it. */
static esp_err_t xfer(lr2021_t *d, size_t len) { return xfer_rx(d, len, len); }

static esp_err_t check(lr2021_t *d)
{
    lr2021_status_t s = lr2021_status(d->rx[0], d->rx[1]);
    if (s.cmd == LR2021_CMD_FAIL || s.cmd == LR2021_CMD_PERR) {
        snprintf(d->error, sizeof d->error, "command %s (chip in %s)", s.cmd ? "not understood" : "failed",
                 lr2021_mode_name(s.mode));
        d->failures++;
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t lr2021_get_status(lr2021_t *d, lr2021_status_t *st)
{
    memset(d->tx, 0, 6);
    lr2021_cmd(d->tx, LR2021_GET_STATUS);
    esp_err_t e = xfer_rx(d, 6, 2);
    if (e == ESP_OK)
        *st = lr2021_status(d->rx[0], d->rx[1]);
    return e;
}

esp_err_t lr2021_send(lr2021_t *d, const uint8_t *frame, size_t len)
{
    if (len > MAX_FRAME)
        return ESP_ERR_INVALID_SIZE;
    memcpy(d->tx, frame, len);
    return xfer_rx(d, len, 0);
}

esp_err_t lr2021_write(lr2021_t *d, const uint8_t *frame, size_t len)
{
    if (len > MAX_FRAME)
        return ESP_ERR_INVALID_SIZE;
    memcpy(d->tx, frame, len);
    esp_err_t e = xfer_rx(d, len, 0);
    if (e != ESP_OK)
        return e;
    /* the status a frame starts with is the previous command's: ask for this one's (6.7.1) */
    lr2021_status_t s;
    e = lr2021_get_status(d, &s);
    return e == ESP_OK ? check(d) : e;
}

esp_err_t lr2021_read(lr2021_t *d, const uint8_t *frame, size_t len, uint8_t *out, size_t n)
{
    if (len > MAX_FRAME || n + 2 > MAX_FRAME)
        return ESP_ERR_INVALID_SIZE;
    memcpy(d->tx, frame, len);
    esp_err_t e = xfer_rx(d, len, 0);
    if (e != ESP_OK)
        return e;
    memset(d->tx, 0, n + 2);
    e = xfer(d, n + 2);
    if (e != ESP_OK)
        return e;
    lr2021_status_t s = lr2021_status(d->rx[0], d->rx[1]);
    if (s.cmd != LR2021_CMD_DATA && s.cmd != LR2021_CMD_OK) {
        e = check(d);
        return e != ESP_OK ? e : fail(d, ESP_FAIL, "unexpected command status"); /* a reserved code: no answer */
    }
    memcpy(out, d->rx + 2, n);
    return ESP_OK;
}

esp_err_t lr2021_read_fifo(lr2021_t *d, uint8_t *out, size_t n)
{
    if (n + 2 > MAX_FIFO_FRAME)
        return ESP_ERR_INVALID_SIZE;
    memset(d->tx, 0, n + 2);
    lr2021_cmd(d->tx, LR2021_READ_RX_FIFO);
    esp_err_t e = xfer(d, n + 2);
    if (e == ESP_OK)
        memcpy(out, d->rx + 2, n);
    return e;
}

esp_err_t lr2021_version(lr2021_t *d, uint8_t *major, uint8_t *minor)
{
    uint8_t f[2], r[2] = {0};
    esp_err_t e = lr2021_read(d, f, lr2021_cmd(f, LR2021_GET_VERSION), r, 2);
    *major = r[0];
    *minor = r[1];
    return e;
}

esp_err_t lr2021_errors(lr2021_t *d, uint16_t *errors)
{
    uint8_t f[2], r[2] = {0};
    esp_err_t e = lr2021_read(d, f, lr2021_cmd(f, LR2021_GET_ERRORS), r, 2);
    *errors = (uint16_t)(r[0] << 8 | r[1]);
    return e;
}

esp_err_t lr2021_rx_fifo_level(lr2021_t *d, uint16_t *level)
{
    uint8_t f[2], r[2] = {0};
    esp_err_t e = lr2021_read(d, f, lr2021_cmd(f, LR2021_GET_RX_FIFO_LEVEL), r, 2);
    *level = (uint16_t)(r[0] << 8 | r[1]);
    return e;
}

esp_err_t lr2021_fifo_flags(lr2021_t *d, uint8_t *rx, uint8_t *tx)
{
    uint8_t f[2], r[2] = {0};
    esp_err_t e = lr2021_read(d, f, lr2021_cmd(f, LR2021_GET_AND_CLEAR_FIFO_IRQ_FLAGS), r, 2);
    *rx = r[0];
    *tx = r[1];
    return e;
}

esp_err_t lr2021_fifo_flags_peek(lr2021_t *d, uint8_t *rx, uint8_t *tx)
{
    uint8_t f[2], r[2] = {0};
    esp_err_t e = lr2021_read(d, f, lr2021_cmd(f, LR2021_GET_FIFO_IRQ_FLAGS), r, 2);
    *rx = r[0];
    *tx = r[1];
    return e;
}

esp_err_t lr2021_tx_fifo_level(lr2021_t *d, uint16_t *level)
{
    uint8_t f[2], r[2] = {0};
    esp_err_t e = lr2021_read(d, f, lr2021_cmd(f, LR2021_GET_TX_FIFO_LEVEL), r, 2);
    *level = (uint16_t)(r[0] << 8 | r[1]);
    return e;
}

esp_err_t lr2021_write_tx_fifo(lr2021_t *d, const uint8_t *data, size_t n)
{
    if (n + 2 > MAX_FIFO_FRAME)
        return ESP_ERR_INVALID_SIZE;
    lr2021_cmd(d->tx, LR2021_WRITE_TX_FIFO);
    memcpy(d->tx + 2, data, n);
    return xfer_rx(d, n + 2, 0);
}

esp_err_t lr2021_read32(lr2021_t *d, uint32_t addr, uint32_t *value)
{
    uint8_t f[8], r[4] = {0};
    esp_err_t e = lr2021_read(d, f, lr2021_cmd_read_reg32(f, addr, 1), r, 4);
    *value = (uint32_t)r[0] << 24 | (uint32_t)r[1] << 16 | (uint32_t)r[2] << 8 | r[3];
    return e;
}

esp_err_t lr2021_read_mem(lr2021_t *d, uint32_t addr, uint32_t *w, int n)
{
    uint8_t f[8], r[256];
    if (n < 1 || n > 64)
        return ESP_ERR_INVALID_SIZE;
    esp_err_t e = lr2021_read(d, f, lr2021_cmd_read_reg32(f, addr, (uint8_t)n), r, 4 * (size_t)n);
    for (int i = 0; i < n; i++)
        w[i] = (uint32_t)r[4 * i] << 24 | (uint32_t)r[4 * i + 1] << 16 | (uint32_t)r[4 * i + 2] << 8 | r[4 * i + 3];
    return e;
}

esp_err_t lr2021_irq(lr2021_t *d, uint32_t *irq)
{
    memset(d->tx, 0, 6);
    lr2021_cmd(d->tx, LR2021_GET_STATUS);
    esp_err_t e = xfer(d, 6);
    *irq = e == ESP_OK ? (uint32_t)d->rx[2] << 24 | (uint32_t)d->rx[3] << 16 | (uint32_t)d->rx[4] << 8 | d->rx[5] : 0;
    return e;
}

esp_err_t lr2021_reset(lr2021_t *d)
{
    gpio_set_level(d->cfg.nreset, 0);
    vTaskDelay(pdMS_TO_TICKS(2) ? pdMS_TO_TICKS(2) : 1);
    gpio_set_level(d->cfg.nreset, 1);
    vTaskDelay(1);
    return wait_busy(d);
}

static void IRAM_ATTR irq_isr(void *arg)
{
    BaseType_t woke = pdFALSE;
    xSemaphoreGiveFromISR(((lr2021_t *)arg)->irq_sem, &woke);
    if (woke)
        portYIELD_FROM_ISR();
}

static esp_err_t irq_setup(lr2021_t *d)
{
    gpio_config_t g = {.pin_bit_mask = 1ULL << d->cfg.irq, .mode = GPIO_MODE_INPUT, .intr_type = GPIO_INTR_POSEDGE};
    esp_err_t e = gpio_config(&g);
    if (e != ESP_OK)
        return e;
    e = gpio_install_isr_service(0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) /* another driver installed it */
        return e;
    if (!(d->irq_sem = xSemaphoreCreateBinary()))
        return ESP_ERR_NO_MEM;
    return gpio_isr_handler_add(d->cfg.irq, irq_isr, d);
}

bool lr2021_wait_irq(lr2021_t *d, int timeout_ms)
{
    if (!d->irq_sem)
        return false;
    xSemaphoreTake(d->irq_sem, 0); /* an edge from before: the level says what's pending now */
    if (gpio_get_level(d->cfg.irq))
        return true;
    TickType_t t = pdMS_TO_TICKS(timeout_ms);
    if (!t && timeout_ms > 0)
        t = 1; /* under a tick: one tick */
    return xSemaphoreTake(d->irq_sem, t) == pdTRUE || gpio_get_level(d->cfg.irq);
}

esp_err_t lr2021_open(const lr2021_config_t *cfg, lr2021_t **out)
{
    if (cfg->mem && ((uintptr_t)cfg->mem & 3))
        return ESP_ERR_INVALID_ARG;
    lr2021_t *d = cfg->mem ? cfg->mem : heap_caps_calloc(1, sizeof *d, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!d)
        return ESP_ERR_NO_MEM;
    if (cfg->mem)
        memset(d, 0, sizeof *d);
    d->own_mem = !cfg->mem;
    d->cfg = *cfg;
    if (d->cfg.clock_hz <= 0 || d->cfg.clock_hz > 16000000)
        d->cfg.clock_hz = 16000000;
    gpio_config_t in = {.pin_bit_mask = 1ULL << cfg->busy, .mode = GPIO_MODE_INPUT};
    gpio_config(&in);
    gpio_config_t rst = {.pin_bit_mask = 1ULL << cfg->nreset, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&rst);
    gpio_set_level(cfg->nreset, 1);
    esp_err_t e = ESP_OK;
    bool own_bus = false;
    if (!cfg->bus_initialized) {
        spi_bus_config_t bus = {.mosi_io_num = cfg->mosi, .miso_io_num = cfg->miso, .sclk_io_num = cfg->sck,
                                .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = MAX_FIFO_FRAME};
        e = spi_bus_initialize(cfg->spi_host, &bus, SPI_DMA_CH_AUTO);
        own_bus = e == ESP_OK;
    }
    spi_device_interface_config_t dev = {.mode = 0, .clock_speed_hz = d->cfg.clock_hz, .spics_io_num = cfg->nss,
                                         .queue_size = 1, .cs_ena_pretrans = 1};
    if (e == ESP_OK)
        e = spi_bus_add_device(cfg->spi_host, &dev, &d->spi);
    if (e == ESP_OK)
        e = lr2021_reset(d);
    if (e == ESP_OK && cfg->irq >= 0)
        e = irq_setup(d);
    if (e != ESP_OK) {
        if (d->irq_sem) {
            gpio_isr_handler_remove(cfg->irq);
            vSemaphoreDelete(d->irq_sem);
        }
        if (d->spi)
            spi_bus_remove_device(d->spi);
        if (own_bus)
            spi_bus_free(cfg->spi_host);
        if (d->own_mem)
            free(d);
        return e;
    }
    *out = d;
    return ESP_OK;
}

esp_err_t lr2021_set_clock(lr2021_t *d, int hz, int input_delay_ns)
{
    if (hz <= 0 || hz > 80000000)
        return ESP_ERR_INVALID_ARG;
    esp_err_t e = spi_bus_remove_device(d->spi);
    if (e != ESP_OK)
        return e;
    d->spi = NULL;
    d->cfg.clock_hz = hz;
    spi_device_interface_config_t dev = {.mode = 0, .clock_speed_hz = hz, .spics_io_num = d->cfg.nss, .queue_size = 1,
                                         .cs_ena_pretrans = 1, .input_delay_ns = input_delay_ns};
    e = spi_bus_add_device(d->cfg.spi_host, &dev, &d->spi);
    return e == ESP_OK ? ESP_OK : fail(d, e, "SPI device not added back");
}

int lr2021_clock(const lr2021_t *d) { return d->cfg.clock_hz; }

void lr2021_close(lr2021_t *d)
{
    if (!d)
        return;
    gpio_set_level(d->cfg.nreset, 0);
    if (d->irq_sem) {
        gpio_isr_handler_remove(d->cfg.irq);
        vSemaphoreDelete(d->irq_sem);
    }
    if (d->spi)
        spi_bus_remove_device(d->spi);
    if (!d->cfg.bus_initialized)
        spi_bus_free(d->cfg.spi_host);
    if (d->own_mem)
        free(d);
}

const char *lr2021_last_error(const lr2021_t *d) { return d->error; }
uint32_t lr2021_failures(const lr2021_t *d) { return d->failures; }
uint32_t lr2021_bounces(const lr2021_t *d) { return d->bounces; }
