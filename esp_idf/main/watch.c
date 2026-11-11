// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The watchdog on tasks that hang. The task watchdog panics when one of its tasks goes WATCH_S unfed
 * (CONFIG_ESP_TASK_WDT_TIMEOUT_S, CONFIG_ESP_TASK_WDT_PANIC): a core dump, then a restart, and /api/status `crash`
 * says which task. Its tasks:
 *
 *   - a canary on each core, which only sleeps a second and feeds. A task above it that never yields starves it.
 *     Core 0's sits above the tasks that may be busy for as long as a client asks (the web server, the streams'
 *     senders, MQTT) and below those that must sleep (USB, the links, the LR2021's reader); core 1's sits above the
 *     DSP, which may use the whole core.
 *   - the tasks that carry the receiver, which feed as they loop (hang_watch_me, hang_watch_feed): the LR2021's reader and
 *     the DSP. One stuck on a lock or a dead bus stops feeding.
 *
 * The idle tasks aren't watched: a wideband stream keeps core 0's from running for seconds, which is load, not a
 * hang (lr2021-iq.md R3.59). */
#include "app.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "watch";

void hang_watch_me(void)
{
#if CONFIG_ESP_TASK_WDT_INIT
    if (esp_task_wdt_status(NULL) != ESP_OK)
        esp_task_wdt_add(NULL);
#endif
}

void hang_watch_feed(void)
{
#if CONFIG_ESP_TASK_WDT_INIT
    esp_task_wdt_reset(); /* an error (not watched) is nothing to do */
#endif
}

#if CONFIG_ESP_TASK_WDT_INIT
static void canary(void *arg)
{
    hang_watch_me();
    for (;;) {
        hang_watch_feed();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
#endif

void hang_watch_start(void)
{
#if CONFIG_ESP_TASK_WDT_INIT
    static const struct {
        const char *name;
        int prio, core;
    } c[] = {{"watch0", 6, 0}, {"watch1", 13, 1}};
    for (int i = 0; i < 2 && i < CONFIG_FREERTOS_NUMBER_OF_CORES; i++)
        if (xTaskCreatePinnedToCore(canary, c[i].name, 2048, NULL, (UBaseType_t)c[i].prio, NULL, c[i].core) != pdPASS)
            ESP_LOGW(TAG, "no memory for %s: core %d isn't watched", c[i].name, c[i].core);
#endif
}
