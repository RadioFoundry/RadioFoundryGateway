// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The HAL on Linux (lr2021_hal_linux.h): spidev for the bus, the GPIO character device's v2 line API for BUSY,
 * NRESET, the IRQ (rising-edge events) and a front-end supply. One line request a pin.
 */
#define _GNU_SOURCE
#include "lr2021_hal_linux.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/spi/spidev.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

typedef struct {
    int spi;
    int busy, nreset, irq, pa_en; /* line request fds, -1 none */
    uint32_t speed_hz;
    uint8_t scratch[1028];        /* rx for a frame whose answer is shorter than it */
} linux_hal_t;

static int line_request(int chip, int offset, bool output, int value, bool edge, const char *label)
{
    struct gpio_v2_line_request r;
    memset(&r, 0, sizeof r);
    r.offsets[0] = (uint32_t)offset;
    r.num_lines = 1;
    snprintf(r.consumer, sizeof r.consumer, "%s", label);
    r.config.flags = output ? GPIO_V2_LINE_FLAG_OUTPUT : GPIO_V2_LINE_FLAG_INPUT;
    if (edge)
        r.config.flags |= GPIO_V2_LINE_FLAG_EDGE_RISING;
    if (output) {
        r.config.num_attrs = 1;
        r.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
        r.config.attrs[0].attr.values = value ? 1 : 0;
        r.config.attrs[0].mask = 1;
    }
    if (ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &r) < 0)
        return -1;
    return r.fd;
}

static int line_get(int fd)
{
    struct gpio_v2_line_values v = {.bits = 0, .mask = 1};
    if (ioctl(fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &v) < 0)
        return -1;
    return (int)(v.bits & 1);
}

static int line_set(int fd, int level)
{
    struct gpio_v2_line_values v = {.bits = level ? 1 : 0, .mask = 1};
    return ioctl(fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v);
}

static lr2021_err_t xfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len, size_t rx_len)
{
    linux_hal_t *h = ctx;
    struct spi_ioc_transfer t;
    memset(&t, 0, sizeof t);
    t.tx_buf = (uintptr_t)tx;
    t.len = (uint32_t)len;
    t.speed_hz = h->speed_hz;
    t.bits_per_word = 8;
    /* spidev fills rx for the whole frame: a shorter answer goes through scratch */
    const bool direct = rx && rx_len == len;
    if (rx && !direct && len > sizeof h->scratch)
        return LR2021_ERR_INVALID_SIZE;
    t.rx_buf = rx ? (uintptr_t)(direct ? rx : h->scratch) : 0;
    if (ioctl(h->spi, SPI_IOC_MESSAGE(1), &t) < 0)
        return LR2021_FAIL;
    if (rx && !direct)
        memcpy(rx, h->scratch, rx_len);
    return LR2021_OK;
}

static int busy(void *ctx) { return line_get(((linux_hal_t *)ctx)->busy); }

static lr2021_err_t nreset(void *ctx, int level)
{
    return line_set(((linux_hal_t *)ctx)->nreset, level) < 0 ? LR2021_FAIL : LR2021_OK;
}

static int irq_level(void *ctx)
{
    linux_hal_t *h = ctx;
    return h->irq >= 0 ? line_get(h->irq) : -1;
}

static bool wait_irq(void *ctx, int timeout_ms)
{
    linux_hal_t *h = ctx;
    struct gpio_v2_line_event ev;
    /* an edge from before: the level says what's pending now */
    while (read(h->irq, &ev, sizeof ev) == (ssize_t)sizeof ev) {
    }
    if (line_get(h->irq) == 1)
        return true;
    struct pollfd p = {.fd = h->irq, .events = POLLIN};
    if (poll(&p, 1, timeout_ms) > 0) {
        while (read(h->irq, &ev, sizeof ev) == (ssize_t)sizeof ev) {
        }
        return true;
    }
    return line_get(h->irq) == 1;
}

static lr2021_err_t set_clock(void *ctx, int hz, int input_delay_ns)
{
    (void)input_delay_ns;
    ((linux_hal_t *)ctx)->speed_hz = (uint32_t)hz;
    return LR2021_OK;
}

static void hal_close(void *ctx)
{
    linux_hal_t *h = ctx;
    if (h->pa_en >= 0) {
        line_set(h->pa_en, 0);
        close(h->pa_en);
    }
    const int fds[4] = {h->irq, h->nreset, h->busy, h->spi};
    for (int i = 0; i < 4; i++)
        if (fds[i] >= 0)
            close(fds[i]);
    free(h);
}

lr2021_err_t lr2021_hal_linux_open(const lr2021_linux_cfg_t *cfg, lr2021_hal_t *hal, char *err, size_t errlen)
{
    linux_hal_t *h = calloc(1, sizeof *h);
    if (!h)
        return LR2021_ERR_NO_MEM;
    h->spi = h->busy = h->nreset = h->irq = h->pa_en = -1;
    h->speed_hz = cfg->speed_hz > 0 ? (uint32_t)cfg->speed_hz : 16000000u;
    lr2021_err_t e = LR2021_OK;
    int chip = -1, offset = -1, fl;
    uint8_t mode = SPI_MODE_0, bits = 8;
    const char *line = NULL;
    if (cfg->busy < 0 || cfg->nreset < 0) {
        snprintf(err, errlen, "busy and nreset need GPIO line offsets");
        e = LR2021_ERR_INVALID_ARG;
        goto fail;
    }
    if ((h->spi = open(cfg->spidev, O_RDWR | O_CLOEXEC)) < 0) {
        int n = errno;
        snprintf(err, errlen, "%s: %s", cfg->spidev, strerror(n));
        e = n == ENOENT ? LR2021_ERR_NOT_FOUND : LR2021_FAIL;
        goto fail;
    }
    if (ioctl(h->spi, SPI_IOC_WR_MODE, &mode) < 0 || ioctl(h->spi, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(h->spi, SPI_IOC_WR_MAX_SPEED_HZ, &h->speed_hz) < 0) {
        snprintf(err, errlen, "%s: mode 0, 8 bits, %u Hz: %s", cfg->spidev, h->speed_hz, strerror(errno));
        e = LR2021_FAIL;
        goto fail;
    }
    if ((chip = open(cfg->gpiochip, O_RDWR | O_CLOEXEC)) < 0) {
        int n = errno;
        snprintf(err, errlen, "%s: %s", cfg->gpiochip, strerror(n));
        e = n == ENOENT ? LR2021_ERR_NOT_FOUND : LR2021_FAIL;
        goto fail;
    }
    if ((h->busy = line_request(chip, offset = cfg->busy, false, 0, false, line = "lr2021 busy")) < 0 ||
        (h->nreset = line_request(chip, offset = cfg->nreset, true, 1, false, line = "lr2021 nreset")) < 0 ||
        (cfg->irq >= 0 && (h->irq = line_request(chip, offset = cfg->irq, false, 0, true, line = "lr2021 irq")) < 0) ||
        (cfg->pa_en >= 0 &&
         (h->pa_en = line_request(chip, offset = cfg->pa_en, true, 1, false, line = "lr2021 pa_en")) < 0)) {
        /* EBUSY: another driver or program holds the line */
        snprintf(err, errlen, "%s: line %d (%s): %s", cfg->gpiochip, offset, line, strerror(errno));
        e = LR2021_FAIL;
        goto fail;
    }
    /* wait_irq drains the edge events without blocking */
    if (h->irq >= 0 && ((fl = fcntl(h->irq, F_GETFL)) < 0 || fcntl(h->irq, F_SETFL, fl | O_NONBLOCK) < 0)) {
        snprintf(err, errlen, "%s: line %d (irq): %s", cfg->gpiochip, cfg->irq, strerror(errno));
        e = LR2021_FAIL;
        goto fail;
    }
    close(chip);
    *hal = (lr2021_hal_t){
        .ctx = h,
        .xfer = xfer,
        .busy = busy,
        .nreset = nreset,
        .wait_irq = h->irq >= 0 ? wait_irq : NULL,
        .irq = h->irq >= 0 ? irq_level : NULL,
        .set_clock = set_clock,
        .close = hal_close,
    };
    return LR2021_OK;

fail:
    if (chip >= 0)
        close(chip);
    hal_close(h);
    return e;
}
