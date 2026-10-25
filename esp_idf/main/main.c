// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * RadioFoundry Gateway on an ESP32-S3: the radios from one box config.
 * The board runs an SDR (an RTL-SDR on USB), a roamer or repeater (an MMDVM
 * modem on USB or a UART), or one of each, as the settings say. See
 * docs/reference.md.
 */
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "mem.h"
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "gnss.h"
#include "netclock.h"
#include "nvs_flash.h"
#include "rdof/rust.h"

static const char *TAG = "main";

board_status_t g_board;

double now_unix(void)
{
    return netclock_now_ns() / 1e9;
}

/* Safe mode: SAFE_CRASHES crashes in a row, each within SAFE_UP_S of its boot, and the next boot starts no roles,
 * only Wi-Fi, the web server and OTA, so settings that crash the board can be changed without a cable. The count
 * lives in RTC memory a reset keeps; a boot that stays up SAFE_UP_S clears it. */
#define SAFE_CRASHES 3
#define SAFE_UP_S 60
#define SAFE_MAGIC 0x5AFE0001u
static RTC_NOINIT_ATTR uint32_t safe_magic, safe_crashes;

static void safe_clear(void *arg) { safe_crashes = 0; }

static bool safe_check(void)
{
    const esp_reset_reason_t r = esp_reset_reason();
    const bool crashed = r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT;
    if (safe_magic != SAFE_MAGIC || !crashed)
        safe_crashes = 0;
    safe_magic = SAFE_MAGIC;
    if (crashed)
        safe_crashes++;
    const esp_timer_create_args_t a = {.callback = safe_clear, .name = "safe_clear"};
    esp_timer_handle_t t;
    if (esp_timer_create(&a, &t) == ESP_OK)
        esp_timer_start_once(t, SAFE_UP_S * 1000000LL);
    if (safe_crashes < SAFE_CRASHES)
        return false;
    ESP_LOGE(TAG, "safe mode: %lu crashes in a row within %d s of boot; no roles started (fix the settings, then "
                  "restart)", (unsigned long)safe_crashes, SAFE_UP_S);
    safe_crashes = 0; /* the next restart tries the roles again */
    return true;
}

#if CONFIG_GNSS_UART_RX_GPIO >= 0
/* The GNSS receiver's NMEA: its fix and position for the status, and each second's time to netclock (it numbers the
 * pulse's edges, so the pulse sets the clock with no NTP). Not when a radio's modem has its UART. */
static void gnss_begin(void)
{
#if CONFIG_RDOF_ROAMER
    for (int i = 0; i < g_roles.nradios; i++) {
        rdof_cfg_port_t p[2];
        const int n = g_rcfg[i] ? rdof_config_ports(g_rcfg[i], p) : 0;
        for (int k = 0; k < n; k++)
            if (!strcmp(p[k].modem_port, "uart") && p[k].uart_num == CONFIG_GNSS_UART_NUM) {
                ESP_LOGW(TAG, "GNSS: UART%d is %s's modem: no NMEA", CONFIG_GNSS_UART_NUM, g_rcfg[i]->name);
                return;
            }
    }
#endif
    gnss_start(netclock_gnss_time);
}
#endif

void app_main(void)
{
    status_led_start(); /* red until Wi-Fi is up, then orange or green by the devices */
#if CONFIG_RDOF_GNSS_EN_GPIO >= 0
    /* the GNSS receiver on: its pulse per second disciplines the clock (netclock) */
    gpio_set_level(CONFIG_RDOF_GNSS_EN_GPIO, CONFIG_RDOF_GNSS_EN_LEVEL);
    gpio_set_direction(CONFIG_RDOF_GNSS_EN_GPIO, GPIO_MODE_OUTPUT);
#endif
    logring_init();
    crash_init(); /* a core dump in flash: one line in the log */
#if CONFIG_RDOF_RUST
    const char *mismatch = rdof_rust_layout_mismatch();
    if (mismatch) {
        ESP_LOGE(TAG, "rust's mirror of %s differs from the C header: stopping", mismatch);
        abort();
    }
#endif
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);
    netclock_init();
    settings_load();
    if ((g_board.safe_mode = safe_check())) {
        g_roles.has_radio = false;
        g_roles.has_sdr = false;
    }
#if CONFIG_GNSS_UART_RX_GPIO >= 0
    gnss_begin();
#endif
    ESP_LOGI(TAG, "RadioFoundry Gateway %s (IDF %s) %s", esp_app_get_description()->version, esp_app_get_description()->idf_ver,
             g_roles.box);
    bool either = false; /* one USB port decides the role: neither is planned */
#if CONFIG_RDOF_SDR
    sdr_boot_selftest(); /* before anything else loads the CPU */
#if CONFIG_RDOF_ROAMER
    either = g_roles.has_radio && g_roles.has_sdr && roamer_uses_usb() && g_scfg.frontend == RDOF_FE_RTLSDR &&
             !g_roles.usb_hub;
#endif
#endif
    /* every role's memory before the network carves the heap up (mem.c, docs/spec/fw/memory.md) */
    mem_plan_boot(either);
    wifi_start();
    web_start();
    oled_start(); /* it shows the roles as they start */
#if CONFIG_RDOF_IPGW
    ipgw_start(); /* before the roles: their receivers hand it data bursts */
#endif
#if CONFIG_RDOF_ROAMER && CONFIG_RDOF_SDR
    if (g_roles.has_radio && g_roles.has_sdr && roamer_uses_usb() && g_scfg.frontend == RDOF_FE_RTLSDR &&
        !g_roles.usb_hub) {
        usb_host_either(roamers_start, sdr_role_start); /* one USB port: what's plugged in decides */
        return;
    }
#endif
#if CONFIG_RDOF_ROAMER
    if (g_roles.has_radio)
        roamers_start();
#endif
#if CONFIG_RDOF_SDR
    if (g_roles.has_sdr)
        sdr_role_start();
#endif
}
