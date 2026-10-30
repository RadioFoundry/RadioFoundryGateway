// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The power key and the battery. A board on a battery with no power switch (the W12: its 3.3 V regulator is on
 * whenever there's a battery) turns "off" in deep sleep: the key held RDOF_POWER_KEY_OFF_MS stops the radios, puts
 * the LR2021 to sleep, turns the board's switched supplies off (the front end's, the OLED's, the GNSS receiver's),
 * holds those pins through the sleep and sleeps until the key goes down again. Woken, the key must stay down
 * RDOF_POWER_KEY_ON_MS, or the board goes back to sleep (a bump in a bag). The battery's voltage is read through
 * its divider every 2 s; under RDOF_BATT_CUTOFF_MV for 30 s the board turns off the same way. The key's holds and
 * the battery's numbers are power_core.c's.
 */
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "power_core.h"
#if CONFIG_RDOF_SDR_LR2021
#include "sdr_fe.h"
#endif
#if CONFIG_RDOF_BATT_ADC_GPIO >= 0
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#endif

static const char *TAG = "power";

#define KEY CONFIG_RDOF_POWER_KEY_GPIO
#define BATT CONFIG_RDOF_BATT_ADC_GPIO
#define BATT_EVERY_MS 2000
#define BATT_LOW_READS 15 /* 30 s */

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

/* ── the pins held through the sleep ─────────────────────────────────────── */

typedef struct {
    int gpio, level;
} held_t;

static held_t held[16];
static int nheld;

static void hold_add(int gpio, int level)
{
    if (gpio < 0 || nheld >= (int)(sizeof held / sizeof held[0]))
        return;
    for (int i = 0; i < nheld; i++)
        if (held[i].gpio == gpio)
            return;
    held[nheld++] = (held_t){gpio, level};
}

/* Each supply off and the LR2021's lines quiet, as the board's Kconfig names them. */
static void hold_list(void)
{
    nheld = 0;
#if CONFIG_RDOF_SDR_LR2021
    hold_add(CONFIG_RDOF_LR2021_PA_EN_GPIO, 0);
    hold_add(CONFIG_RDOF_LR2021_NSS, 1);    /* NSS low would wake the chip */
    hold_add(CONFIG_RDOF_LR2021_NRESET, 1); /* low holds it in reset, which draws more than its sleep */
#endif
#if CONFIG_RDOF_OLED && CONFIG_RDOF_OLED_POWER_GPIO >= 0
    hold_add(CONFIG_RDOF_OLED_POWER_GPIO, !CONFIG_RDOF_OLED_POWER_LEVEL);
#endif
#if CONFIG_RDOF_GNSS_EN_GPIO >= 0
    hold_add(CONFIG_RDOF_GNSS_EN_GPIO, !CONFIG_RDOF_GNSS_EN_LEVEL);
#endif
#if CONFIG_RDOF_STATUS_LED_GPIO >= 0
    hold_add(CONFIG_RDOF_STATUS_LED_GPIO, 0); /* a WS2812's data line idles low */
#endif
#if BATT >= 0 && CONFIG_RDOF_BATT_CTRL_GPIO >= 0
    hold_add(CONFIG_RDOF_BATT_CTRL_GPIO, !CONFIG_RDOF_BATT_CTRL_LEVEL);
#endif
#if KEY >= 0
    const char *s = CONFIG_RDOF_POWER_OFF_LOW_GPIOS;
    while (*s) {
        char *end;
        const long g = strtol(s, &end, 10);
        if (end == s) {
            s++;
            continue;
        }
        hold_add((int)g, 0);
        s = end;
    }
#endif
}

static void hold_release(void)
{
    hold_list();
    for (int i = 0; i < nheld; i++)
        gpio_hold_dis(held[i].gpio);
    gpio_deep_sleep_hold_dis();
}

/* Sleep until the key (or, with none, until a reset); the pins already held. */
static void sleep_now(void)
{
    gpio_deep_sleep_hold_en();
#if KEY >= 0
    esp_sleep_enable_ext0_wakeup(KEY, 0);
    rtc_gpio_pullup_en(KEY); /* the W12 has its own 10k; a board without one keeps the key high */
    rtc_gpio_pulldown_dis(KEY);
#endif
    esp_deep_sleep_start();
}

/* ── the key ─────────────────────────────────────────────────────────────── */

#if KEY >= 0
static rdof_pk_t pk;

static bool key_down(void) { return gpio_get_level(KEY) == 0; }
#endif

void power_boot(void)
{
    g_board.batt_mv = g_board.batt_pct = -1;
#if KEY >= 0
    const bool woke = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0;
    if (woke)
        rtc_gpio_deinit(KEY); /* back from the sleep's RTC IO to a GPIO */
    const gpio_config_t c = {.pin_bit_mask = 1ULL << KEY, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    gpio_config(&c);
    rdof_pk_init(&pk, CONFIG_RDOF_POWER_KEY_OFF_MS, CONFIG_RDOF_POWER_KEY_ON_MS, woke, 0); /* down since the boot */
    while (woke) {
        const rdof_pk_event_t e = rdof_pk_step(&pk, key_down(), now_ms());
        if (e == RDOF_PK_SLEEP)
            sleep_now(); /* the pins are still held from the last sleep */
        if (e == RDOF_PK_ON)
            break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
#endif
    hold_release();
}

/* ── the battery ─────────────────────────────────────────────────────────── */

#if BATT >= 0
static adc_oneshot_unit_handle_t adc;
static adc_channel_t adc_ch;
static adc_cali_handle_t cali;
static rdof_batt_low_t low;

static bool batt_init(void)
{
    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(BATT, &unit, &adc_ch) != ESP_OK)
        return false;
    const adc_oneshot_unit_init_cfg_t u = {.unit_id = unit};
    /* 2.5 dB: up to about 1.25 V at the pin, a 6 V battery through the W12's 4.9 */
    const adc_oneshot_chan_cfg_t cc = {.atten = ADC_ATTEN_DB_2_5, .bitwidth = ADC_BITWIDTH_DEFAULT};
    const adc_cali_curve_fitting_config_t k = {
        .unit_id = unit, .chan = adc_ch, .atten = ADC_ATTEN_DB_2_5, .bitwidth = ADC_BITWIDTH_DEFAULT};
    if (adc_oneshot_new_unit(&u, &adc) != ESP_OK || adc_oneshot_config_channel(adc, adc_ch, &cc) != ESP_OK ||
        adc_cali_create_scheme_curve_fitting(&k, &cali) != ESP_OK) {
        ESP_LOGW(TAG, "no battery reading on GPIO%d", BATT);
        return false;
    }
#if CONFIG_RDOF_BATT_CTRL_GPIO >= 0
    gpio_set_level(CONFIG_RDOF_BATT_CTRL_GPIO, !CONFIG_RDOF_BATT_CTRL_LEVEL);
    gpio_set_direction(CONFIG_RDOF_BATT_CTRL_GPIO, GPIO_MODE_OUTPUT);
#endif
    return true;
}

/* The divider on, 8 readings averaged, the divider off (it draws about 9 uA on). */
static void batt_read(void)
{
#if CONFIG_RDOF_BATT_CTRL_GPIO >= 0
    gpio_set_level(CONFIG_RDOF_BATT_CTRL_GPIO, CONFIG_RDOF_BATT_CTRL_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(5));
#endif
    int sum = 0, n = 0;
    for (int i = 0; i < 8; i++) {
        int mv;
        if (adc_oneshot_get_calibrated_result(adc, cali, adc_ch, &mv) == ESP_OK)
            sum += mv, n++;
    }
#if CONFIG_RDOF_BATT_CTRL_GPIO >= 0
    gpio_set_level(CONFIG_RDOF_BATT_CTRL_GPIO, !CONFIG_RDOF_BATT_CTRL_LEVEL);
#endif
    if (!n)
        return;
    const int mv = rdof_batt_mv(sum / n, CONFIG_RDOF_BATT_RATIO_X1000);
    const int was = g_board.batt_mv;
    const int now = was < 0 ? mv : (was * 3 + mv) / 4; /* smoothed: a burst of load sags it for a moment */
    g_board.batt_mv = now;
    g_board.batt_pct = rdof_batt_percent(now);
}
#endif

/* ── off ─────────────────────────────────────────────────────────────────── */

void power_off(const char *why)
{
    static volatile bool going;
    if (going) /* one turning off is enough */
        for (;;)
            vTaskDelay(portMAX_DELAY);
    going = true;
    ESP_LOGW(TAG, "turning off: %s", why);
    g_board.powering_off = why; /* the OLED says so, the LED goes dark */
    status_led_off();
#if CONFIG_RDOF_SDR
    if (g_sstat.started && !sdr_pause(true))
        ESP_LOGW(TAG, "the SDR didn't stop in 3 s");
#endif
#if CONFIG_RDOF_SDR_LR2021
    fe_lr2021_power_off(); /* sleeping, its front end's supply off: no transmit after this */
#endif
    vTaskDelay(pdMS_TO_TICKS(700)); /* the OLED's last screen and the LED's last write */
#if KEY >= 0
    /* the key is still down from the hold: let go first, or it would wake the board at once */
    for (int64_t up = -1;;) {
        if (key_down())
            up = -1;
        else if (up < 0)
            up = now_ms();
        else if (now_ms() - up >= RDOF_PK_DEBOUNCE_MS)
            break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
#endif
    hold_list();
    for (int i = 0; i < nheld; i++) {
        gpio_set_level(held[i].gpio, held[i].level);
        gpio_set_direction(held[i].gpio, GPIO_MODE_OUTPUT);
        gpio_hold_en(held[i].gpio);
    }
    ESP_LOGI(TAG, "off (%d pins held)", nheld);
    vTaskDelay(pdMS_TO_TICKS(20)); /* the log line out */
    sleep_now();
}

static void power_task(void *arg)
{
#if BATT >= 0
    const bool batt = batt_init();
    int64_t next = 0;
#endif
    for (;;) {
#if KEY >= 0
        if (rdof_pk_step(&pk, key_down(), now_ms()) == RDOF_PK_OFF)
            power_off("the power key");
#endif
#if BATT >= 0
        if (batt && now_ms() >= next) {
            next = now_ms() + BATT_EVERY_MS;
            batt_read();
            if (CONFIG_RDOF_BATT_CUTOFF_MV > 0 &&
                rdof_batt_low_step(&low, g_board.batt_mv, CONFIG_RDOF_BATT_CUTOFF_MV, BATT_LOW_READS))
                power_off("the battery is low");
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void power_start(void)
{
#if KEY >= 0 || BATT >= 0
    /* core 0, low priority: a poll every 20 ms */
    xTaskCreatePinnedToCore(power_task, "power", 3072, NULL, 2, NULL, 0);
#endif
}

void power_status_json(rdof_json_t *j)
{
#if KEY >= 0 || BATT >= 0
    rdof_json_open(j, "power");
    rdof_json_bool(j, "key", KEY >= 0);
    if (g_board.batt_mv >= 0) {
        rdof_json_int(j, "battery_mv", g_board.batt_mv);
        rdof_json_int(j, "battery_pct", g_board.batt_pct);
    }
    rdof_json_close(j);
#endif
}
