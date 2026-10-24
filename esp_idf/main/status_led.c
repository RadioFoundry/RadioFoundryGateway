// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The board's RGB LED (a WS2812 on CONFIG_RDOF_STATUS_LED_GPIO; GPIO48 on the SuperMini): red without Wi-Fi, orange
 * with Wi-Fi but a configured device not working (the SDR not receiving, a radio's modem not ready), green when all
 * is well. A task looks twice a second and writes on a change. RMT with a bytes encoder; dim, so it doesn't load
 * the SuperMini's regulator.
 */
#include "app.h"

#if CONFIG_RDOF_STATUS_LED_GPIO >= 0

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LEVEL 24 /* of 255 */

enum { LED_RED, LED_ORANGE, LED_GREEN };

static rmt_channel_handle_t chan;
static rmt_encoder_handle_t enc;

static void show(int color)
{
    static const uint8_t grb[][3] = {
        [LED_RED] = {0, LEVEL, 0},
        [LED_ORANGE] = {LEVEL / 3, LEVEL, 0},
        [LED_GREEN] = {LEVEL, 0, 0},
    };
    const rmt_transmit_config_t tc = {.loop_count = 0};
    rmt_transmit(chan, enc, grb[color], 3, &tc);
    rmt_tx_wait_all_done(chan, 50);
}

#if CONFIG_RDOF_SDR
static bool sdr_ok(void)
{
    return g_sstat.started && g_sstat.rtl_open && g_sstat.receiving;
}
#endif

#if CONFIG_RDOF_ROAMER
/* Every radio started and its modem ready; *any: at least one started. */
static bool radios_ok(bool *any)
{
    *any = false;
    for (int k = 0; k < g_nroam; k++) {
        if (!g_roam[k]->st.started || !g_roam[k]->st.radio_ready)
            return false;
        *any = true;
    }
    return g_nroam > 0;
}
#endif

/* The devices the settings name are all there and working. With both roles on one USB port only the one plugged
 * in runs, so that one must be working (nothing plugged in isn't). */
static bool devices_ok(void)
{
    bool radios = true, radio_started = false, sdr = true;
#if CONFIG_RDOF_ROAMER
    if (g_roles.has_radio)
        radios = radios_ok(&radio_started);
#endif
#if CONFIG_RDOF_SDR
    if (g_roles.has_sdr)
        sdr = sdr_ok();
    if (g_board.usb_either)
        return g_sstat.started ? sdr : radio_started && radios;
#endif
    return radios && sdr;
}

static void led_task(void *arg)
{
    int shown = -1;
    for (;;) {
        const int c = !g_board.wifi_up ? LED_RED : devices_ok() ? LED_GREEN : LED_ORANGE;
        if (c != shown)
            show(shown = c);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void status_led_start(void)
{
    const rmt_tx_channel_config_t cc = {
        .gpio_num = CONFIG_RDOF_STATUS_LED_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10000000, /* 0.1 us ticks */
        .mem_block_symbols = 48,
        .trans_queue_depth = 2,
    };
    /* WS2812: a 0 is 0.3 us high, 0.9 low; a 1 is 0.9 high, 0.3 low; MSB first */
    const rmt_bytes_encoder_config_t ec = {
        .bit0 = {.level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9},
        .bit1 = {.level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3},
        .flags.msb_first = 1,
    };
    if (rmt_new_tx_channel(&cc, &chan) != ESP_OK || rmt_new_bytes_encoder(&ec, &enc) != ESP_OK ||
        rmt_enable(chan) != ESP_OK) {
        ESP_LOGW("led", "no status LED on GPIO%d", CONFIG_RDOF_STATUS_LED_GPIO);
        return;
    }
    show(LED_RED);
    xTaskCreate(led_task, "led", 2048, NULL, 1, NULL);
}

#else

void status_led_start(void) {}

#endif
