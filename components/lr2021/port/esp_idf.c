// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The library's clock and waits on ESP-IDF (lr2021_port.h). */
#include "esp_attr.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lr2021_port.h"

#define SLEEP_MIN_US 30 /* shorter: a spin (the timer's interrupt and the task switch back cost about 10 us) */

IRAM_ATTR int64_t lr2021_now_us(void) { return esp_timer_get_time(); }
IRAM_ATTR void lr2021_delay_us(uint32_t us) { esp_rom_delay_us(us); }
void lr2021_sleep_tick(void) { vTaskDelay(1); }

void lr2021_sleep_ms(int ms)
{
    TickType_t t = pdMS_TO_TICKS(ms);
    vTaskDelay(t ? t : 1);
}

/* Under a tick, a one-shot esp_timer wakes the task: the reader sleeps between FIFO reads instead of spinning. Made
 * on first use; a caller that comes while another is making it spins that once. */
static esp_timer_handle_t timer;
static SemaphoreHandle_t woken, one_at_a_time;
static StaticSemaphore_t woken_buf, one_buf;
static volatile int timer_state; /* 0 not made, 1 being made, 2 ready, -1 failed */

static void IRAM_ATTR wake(void *arg)
{
    BaseType_t w = pdFALSE;
    xSemaphoreGiveFromISR(woken, &w);
#if CONFIG_ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD
    if (w)
        esp_timer_isr_dispatch_need_yield();
#else
    (void)w;
#endif
}

static IRAM_ATTR bool timer_ready(void)
{
    int expect = 0;
    if (timer_state == 0 && __atomic_compare_exchange_n(&timer_state, &expect, 1, false, __ATOMIC_ACQ_REL,
                                                        __ATOMIC_ACQUIRE)) {
        woken = xSemaphoreCreateBinaryStatic(&woken_buf);
        one_at_a_time = xSemaphoreCreateMutexStatic(&one_buf);
        const esp_timer_create_args_t a = {.callback = wake, .name = "lr2021_sleep",
#if CONFIG_ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD
                                           .dispatch_method = ESP_TIMER_ISR,
#endif
        };
        __atomic_store_n(&timer_state, esp_timer_create(&a, &timer) == ESP_OK ? 2 : -1, __ATOMIC_RELEASE);
    }
    return __atomic_load_n(&timer_state, __ATOMIC_ACQUIRE) == 2;
}

IRAM_ATTR void lr2021_sleep_us(uint32_t us)
{
    const uint32_t tick_us = (uint32_t)portTICK_PERIOD_MS * 1000;
    if (us >= tick_us) {
        vTaskDelay((TickType_t)(us / tick_us)); /* at most us: the next tick may come at once */
        return;
    }
    if (us < SLEEP_MIN_US || xPortInIsrContext() || !timer_ready()) {
        esp_rom_delay_us(us);
        return;
    }
    xSemaphoreTake(one_at_a_time, portMAX_DELAY);
    xSemaphoreTake(woken, 0);
    if (esp_timer_start_once(timer, us) == ESP_OK)
        xSemaphoreTake(woken, 2); /* two ticks: the timer can't be later than that */
    else
        esp_rom_delay_us(us);
    xSemaphoreGive(one_at_a_time);
}
