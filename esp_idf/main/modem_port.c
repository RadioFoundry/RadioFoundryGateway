// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The port under each radio's rdof_modem (common): bytes from its transport
 * (USB or UART) go through a stream buffer to a reader task that feeds the
 * modem; a request waits on a semaphore the reader gives when its reply is in.
 */
#include <string.h>

#include "app.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

static const char *TAG = "modem";

double now_mono(void)
{
    return esp_timer_get_time() / 1e6;
}

static bool port_write(void *ctx, const uint8_t *data, size_t len)
{
    roam_t *r = ctx;
    const modem_transport_t *t = r->transport;
    return t && t->write(t->ctx, data, len);
}

static bool port_wait(void *ctx, int timeout_ms)
{
    roam_t *r = ctx;
    TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (!r->modem.reply_ready) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(until - now) <= 0)
            return r->modem.reply_ready;
        xSemaphoreTake((SemaphoreHandle_t)r->reply_sem, until - now);
    }
    return true;
}

static void port_on_reply(void *ctx)
{
    roam_t *r = ctx;
    xSemaphoreGive((SemaphoreHandle_t)r->reply_sem);
}

static double port_clock(void *ctx)
{
    return now_mono();
}

static void port_sleep(void *ctx, int ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

bool modem_transport_ready(roam_t *r)
{
    return r->transport != NULL;
}

void modem_transport_up(roam_t *r, const modem_transport_t *t)
{
    if (split_half_up(r, t))
        return;
    xStreamBufferReset((StreamBufferHandle_t)r->rx_bytes);
    rdof_modem_port_t port = {port_write, port_wait, port_on_reply, port_clock, port_sleep, r};
    void (*on_rx)(void *, int, uint8_t, const uint8_t *, int) = r->modem.on_rx;
    void (*on_lost)(void *, int) = r->modem.on_lost;
    void (*on_nak)(void *, uint8_t, int) = r->modem.on_nak;
    void *cb = r->modem.cb_ctx;
    rdof_modem_init(&r->modem, &port);
    r->modem.on_rx = on_rx;
    r->modem.on_lost = on_lost;
    r->modem.on_nak = on_nak;
    r->modem.cb_ctx = cb;
    r->transport = t;
    r->st.modem_open = true;
    ESP_LOGI(TAG, "%s%stransport %s up", r->i ? r->cfg->name : "", r->i ? ": " : "", t->name);
}

void modem_transport_down(roam_t *r)
{
    if (split_half_down(r))
        return;
    r->transport = NULL;
    rdof_modem_fail(&r->modem, "the radio went away");
    r->st.modem_open = false;
    r->st.radio_ready = false;
}

void modem_on_bytes(roam_t *r, const uint8_t *data, size_t len)
{
    if (split_half_bytes(r, data, len))
        return;
    if (r->rx_bytes && xStreamBufferSend((StreamBufferHandle_t)r->rx_bytes, data, len, 0) != len)
        ESP_LOGW(TAG, "receive buffer full: bytes dropped");
}

static void reader_task(void *arg)
{
    roam_t *r = arg;
    uint8_t buf[256];
    for (;;) {
        size_t n = xStreamBufferReceive((StreamBufferHandle_t)r->rx_bytes, buf, sizeof buf, portMAX_DELAY);
        if (n && r->transport)
            rdof_modem_feed(&r->modem, buf, n);
    }
}

void modem_port_init(roam_t *r)
{
    r->rx_bytes = xStreamBufferCreate(4096, 1);
    r->reply_sem = xSemaphoreCreateBinary();
    rdof_modem_port_t port = {port_write, port_wait, port_on_reply, port_clock, port_sleep, r};
    rdof_modem_init(&r->modem, &port);
    roam_task(r, reader_task, "modem_rx", 6144, 12);
}
