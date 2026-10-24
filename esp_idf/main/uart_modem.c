// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * A modem on a UART: an MMDVM_HS HAT, or a RadioFoundry radio in hotspot mode on its
 * programming cable's serial port (radio_hotspot; the same protocol as over
 * USB). 3.3 V, 8N1 at the radio's speed (115200 by default), wired straight
 * to the S3: UART1 by default, UART2 for a second radio (uart.num). UART0 is
 * the console.
 */
#include <stdio.h>

#include "app.h"
#include "mem.h"

#if CONFIG_RDOF_MODEM_UART

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "uart";

static uart_port_t port_of(const roam_t *r) { return r->cfg->uart_num == 2 ? UART_NUM_2 : UART_NUM_1; }

static bool uart_write(void *ctx, const uint8_t *data, size_t len)
{
    return uart_write_bytes(port_of(ctx), data, len) == (int)len;
}

static void reader_task(void *arg)
{
    roam_t *r = arg;
    uint8_t buf[256];
    for (;;) {
        int n = uart_read_bytes(port_of(r), buf, sizeof buf, pdMS_TO_TICKS(5));
        if (n > 0)
            modem_on_bytes(r, buf, (size_t)n);
    }
}

void uart_modem_start(roam_t *r)
{
    const uart_port_t port = port_of(r);
    uart_config_t c = {
        .baud_rate = (int)r->cfg->uart_speed,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    r->uart = (modem_transport_t){"uart", uart_write, r};
    char role[12];
    snprintf(role, sizeof role, "radio%d", (r->i & 7) + 1);
    mem_hand_over(role, "uart_rx_buf"); /* the plan kept these for the driver's rings */
    mem_hand_over(role, "uart_tx_buf");
    ESP_ERROR_CHECK(uart_driver_install(port, 4096, 2048, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(port, &c));
    ESP_ERROR_CHECK(uart_set_pin(port, r->cfg->uart_tx, r->cfg->uart_rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    roam_task(r, reader_task, "uart_rx", 3072, 12);
    modem_transport_up(r, &r->uart);
    if (port == UART_NUM_1 && r->cfg->uart_speed == 115200)
        ESP_LOGI(TAG, "modem on UART1 (TX GPIO%d, RX GPIO%d)", r->cfg->uart_tx, r->cfg->uart_rx);
    else
        ESP_LOGI(TAG, "%s: modem on UART%d (TX GPIO%d, RX GPIO%d) at %ld", r->cfg->name, port, r->cfg->uart_tx,
                 r->cfg->uart_rx, r->cfg->uart_speed);
}

#else

void uart_modem_start(roam_t *r) {}

#endif
