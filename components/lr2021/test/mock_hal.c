// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* See mock_hal.h. */
#include "mock_hal.h"

#include <string.h>

shim_t F;

void shim_reset(void) { memset(&F, 0, sizeof F); }

static lr2021_err_t xfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len, size_t rx_len)
{
    (void)ctx;
    int idx = F.xfers++;
    if (idx < SHIM_MAX_XFER) {
        memcpy(F.tx[idx], tx, len);
        F.tx_len[idx] = len;
        F.rx_len[idx] = rx_len;
    }
    if (rx_len && (((uintptr_t)rx | rx_len) & 3))
        F.unaligned++;
    if (F.fail_xfer_at && idx + 1 == F.fail_xfer_at)
        return F.xfer_err;
    static uint8_t frame[1030];
    memset(frame, 0, len);
    if (F.respond)
        F.respond(idx, tx, len, frame);
    if (rx_len)
        memcpy(rx, frame, rx_len);
    return LR2021_OK;
}

static int busy(void *ctx)
{
    (void)ctx;
    F.busy_polls++;
    return F.busy_stuck || F.now_us < F.busy_until_us;
}

static lr2021_err_t nreset(void *ctx, int level)
{
    (void)ctx;
    if (F.n_nreset < 16) {
        F.nreset_levels[F.n_nreset] = level;
        F.nreset_times[F.n_nreset++] = F.now_us;
    }
    return LR2021_OK;
}

static void moved(void)
{
    if (F.on_time)
        F.on_time();
}

static bool wait_irq(void *ctx, int timeout_ms)
{
    (void)ctx;
    F.irq_waits++;
    moved();
    /* in 50 us steps, so a model raising the pin wakes it about when it would */
    for (int64_t end = F.now_us + (int64_t)timeout_ms * 1000; !F.irq_level && F.now_us < end;) {
        F.now_us += 50;
        moved();
    }
    return F.irq_level;
}

static int irq(void *ctx)
{
    (void)ctx;
    return F.irq_level;
}

static lr2021_err_t set_clock(void *ctx, int hz, int input_delay_ns)
{
    (void)ctx;
    F.clock_sets++;
    F.clock_hz = hz;
    F.input_delay_ns = input_delay_ns;
    return LR2021_OK;
}

static void hal_close(void *ctx)
{
    (void)ctx;
    F.closes++;
}

lr2021_config_t mock_config(void)
{
    lr2021_config_t c = {.hal = {.xfer = xfer, .busy = busy, .nreset = nreset, .set_clock = set_clock,
                                 .close = hal_close}};
    if (F.has_irq) {
        c.hal.wait_irq = wait_irq;
        c.hal.irq = irq;
    }
    return c;
}

/* The fake clock (lr2021_port.h). */
int64_t lr2021_now_us(void) { return F.now_us; }
void lr2021_delay_us(uint32_t us)
{
    F.now_us += us;
    moved();
}
void lr2021_sleep_tick(void)
{
    F.now_us += 10000;
    moved();
}
void lr2021_sleep_ms(int ms)
{
    F.now_us += (int64_t)ms * 1000;
    moved();
}
void lr2021_sleep_us(uint32_t us)
{
    F.now_us += us;
    moved();
}
