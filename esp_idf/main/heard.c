// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Last heard: the ring of recent calls (rdof/lastheard.h) behind a mutex, fed by the receive paths and the
 * transmitters, read by /api/lastheard.
 */
#include "heard.h"

#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "netclock.h"
#include "rdof/lastheard.h"
#include "rdof/pdu.h"

EXT_RAM_BSS_ATTR static rdof_lh_t lh;
static SemaphoreHandle_t lock;
static portMUX_TYPE once = portMUX_INITIALIZER_UNLOCKED;

static bool take(void)
{
    if (!lock) {
        static StaticSemaphore_t buf;
        taskENTER_CRITICAL(&once);
        if (!lock) {
            lock = xSemaphoreCreateMutexStatic(&buf);
            rdof_lh_init(&lh, NULL, 0); /* the ring comes next, outside the critical section */
        }
        taskEXIT_CRITICAL(&once);
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!lh.cap) {
        rdof_lh_entry_t *e = heap_caps_calloc(RDOF_LH_DEFAULT, sizeof *e, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!e)
            e = calloc(RDOF_LH_DEFAULT, sizeof *e);
        if (e)
            rdof_lh_init(&lh, e, RDOF_LH_DEFAULT);
    }
    return lh.cap > 0;
}

static double mono(void)
{
    return esp_timer_get_time() / 1e6;
}

/* Wall clock ms, 0 until the clock is set. */
static int64_t wall_ms(void)
{
    int64_t ms = netclock_now_ns() / 1000000;
    return ms > 1600000000000LL ? ms : 0;
}

void heard_rf_json(const char *role, const char *radio, const char *channel, const char *json)
{
    /* only call lines: the mode and the receiver messages are skipped unparsed */
    if (!json || (strncmp(json, "{\"DMR\"", 6) && strncmp(json, "{\"BER\"", 6) && strncmp(json, "{\"RSSI\"", 7)))
        return;
    if (!take()) {
        xSemaphoreGive(lock);
        return;
    }
    rdof_lh_tracker_json(&lh, role, radio, channel, json, mono(), wall_ms());
    xSemaphoreGive(lock);
}

void heard_burst(const char *radio, const char *channel, int slot, int dt)
{
    if (dt != RDOF_DT_PI_HEADER)
        return;
    if (take())
        rdof_lh_privacy(&lh, radio, channel, slot);
    xSemaphoreGive(lock);
}

void heard_tx(const char *role, const char *radio, const char *action, const rdof_tx_call_info_t *call,
              double duration)
{
    if (take())
        rdof_lh_tx(&lh, role, radio, action, call, duration, mono(), wall_ms());
    xSemaphoreGive(lock);
}

size_t heard_json(uint32_t since, char *out, size_t cap)
{
    take();
    size_t n = rdof_lh_json(&lh, since, mono(), wall_ms(), out, cap);
    xSemaphoreGive(lock);
    return n;
}
