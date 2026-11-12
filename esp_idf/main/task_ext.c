// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * esp-mqtt's task with its stack in PSRAM. Each MQTT connection has a task of esp-mqtt's own, 6 KB of stack, and
 * esp-mqtt asks for it with xTaskCreatePinnedToCore, which takes it from internal RAM. A W12 with the SDR, a
 * transceiver and the MQTT network has three: 18 KB of the internal RAM it's shortest of (79 bytes of DMA-capable
 * memory were left, and a task's stack was refused; 2026-11-11). The task waits on its socket and its lock, parses
 * and copies: nothing that needs a fast stack, and nothing that writes the flash (a task whose stack is in PSRAM
 * can't: the cache is off then).
 *
 * The linker wraps the two calls (CMakeLists.txt): a task named "mqtt_task" is created on a stack from PSRAM with
 * a control block of ours, and its vTaskDelete marks them free again. FreeRTOS doesn't free a static task's
 * memory, and ESP-IDF's vTaskDeleteWithCaps aborts when it can't create the helper task a self-deleting one
 * needs, so the stacks are kept and used again: SLOTS of them at most, each allocated the first time. A slot is
 * taken again only RETIRE_MS after its task was deleted (the idle task has let go of it long before). No slot, no
 * PSRAM: the call goes through as it was, to internal RAM.
 */
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SLOTS 4 /* three connections, and one being replaced (a fall back to MQTT 3.1.1 stops and starts one) */
#define RETIRE_MS 5000

BaseType_t __real_xTaskCreatePinnedToCore(TaskFunction_t fn, const char *const name, const uint32_t depth,
                                          void *const arg, UBaseType_t prio, TaskHandle_t *const out,
                                          const BaseType_t core);
void __real_vTaskDelete(TaskHandle_t h);

static struct {
    StaticTask_t tcb; /* internal (BSS): the kernel's lists run through it */
    StackType_t *stack;
    uint32_t depth;
    TaskHandle_t task;   /* running on it */
    TickType_t retired;  /* when its task was deleted */
    bool used;           /* ever */
} slot[SLOTS];
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

BaseType_t __wrap_xTaskCreatePinnedToCore(TaskFunction_t fn, const char *const name, const uint32_t depth,
                                          void *const arg, UBaseType_t prio, TaskHandle_t *const out,
                                          const BaseType_t core)
{
    if (!name || strcmp(name, "mqtt_task"))
        return __real_xTaskCreatePinnedToCore(fn, name, depth, arg, prio, out, core);
    int k = -1;
    const TickType_t now = xTaskGetTickCount();
    portENTER_CRITICAL(&mux);
    for (int i = 0; i < SLOTS && k < 0; i++)
        if (!slot[i].task && (!slot[i].used || (TickType_t)(now - slot[i].retired) >= pdMS_TO_TICKS(RETIRE_MS)) &&
            (!slot[i].stack || slot[i].depth == depth))
            k = i;
    if (k >= 0)
        slot[k].task = (TaskHandle_t)&slot[k].tcb; /* taken; the handle is the control block */
    portEXIT_CRITICAL(&mux);
    if (k >= 0 && !slot[k].stack) {
        slot[k].stack = heap_caps_malloc(depth * sizeof(StackType_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        slot[k].depth = depth;
    }
    TaskHandle_t h = k >= 0 && slot[k].stack
                         ? xTaskCreateStaticPinnedToCore(fn, name, depth, arg, prio, slot[k].stack, &slot[k].tcb, core)
                         : NULL;
    if (!h) {
        if (k >= 0)
            slot[k].task = NULL;
        return __real_xTaskCreatePinnedToCore(fn, name, depth, arg, prio, out, core);
    }
    slot[k].used = true;
    if (out)
        *out = h;
    return pdPASS;
}

void __wrap_vTaskDelete(TaskHandle_t h)
{
    const TaskHandle_t t = h ? h : xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&mux);
    for (int i = 0; i < SLOTS; i++)
        if (slot[i].task == t) {
            slot[i].task = NULL;
            slot[i].retired = xTaskGetTickCount();
        }
    portEXIT_CRITICAL(&mux);
    __real_vTaskDelete(h);
}

/* How many of esp-mqtt's tasks run on a PSRAM stack now, for the status. */
int task_ext_running(void)
{
    int n = 0;
    for (int i = 0; i < SLOTS; i++)
        n += slot[i].task != NULL;
    return n;
}
