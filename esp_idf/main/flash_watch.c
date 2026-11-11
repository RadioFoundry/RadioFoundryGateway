// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* Who writes the flash, and for how long. A flash write or erase turns the cache off, and every task on both cores
 * waits (a sector erase about 35 ms): the LR2021's reader loses its FIFO meanwhile. The linker wraps the two calls
 * around each flash operation (CMakeLists.txt); /api/status "flash" gives the count, the longest, and the task and
 * time of the last one that took over a millisecond (a read is far shorter). The count is also how often the flash
 * is written: long_ops is the writes and erases. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "xtensa_context.h"
#include "esp_freertos_hooks.h"

#include "app.h"

void __real_spi_flash_disable_interrupts_caches_and_other_cpu(void);
void __real_spi_flash_enable_interrupts_caches_and_other_cpu(void);

static DRAM_ATTR struct {
    uint32_t ops, long_ops;
    int64_t t0, last_at;
    uint32_t worst_us, last_us;
    TaskHandle_t last_by;
} fw;

void IRAM_ATTR __wrap_spi_flash_disable_interrupts_caches_and_other_cpu(void)
{
    __real_spi_flash_disable_interrupts_caches_and_other_cpu();
    fw.t0 = esp_timer_get_time();
}

void IRAM_ATTR __wrap_spi_flash_enable_interrupts_caches_and_other_cpu(void)
{
    const int64_t now = esp_timer_get_time();
    const uint32_t us = (uint32_t)(now - fw.t0);
    fw.ops++;
    if (us > fw.worst_us)
        fw.worst_us = us;
    if (us >= 1000) {
        fw.long_ops++;
        fw.last_us = us;
        fw.last_at = now;
        fw.last_by = xTaskGetCurrentTaskHandle();
    }
    __real_spi_flash_enable_interrupts_caches_and_other_cpu();
}


/* Stalls: each core's tick notes a gap over 5 ms between two ticks (interrupts off, or the cache off, for that
 * long). /api/status "stalls" lists the last four on each core. */
static DRAM_ATTR uint32_t stall_tick_gap_us;
static DRAM_ATTR int64_t stall_last_us;

/* The last tick gap over 5 ms on each core: its length, where the tick came in (the interrupted task's saved PC:
 * just past where interrupts came back on), and the task on each core then. */
typedef struct {
    uint32_t n, us, pc, a0;
    int64_t at;
    char t0[12], t1[12];
} stall_ev_t;
static DRAM_ATTR stall_ev_t stall_ev[2][4];
static DRAM_ATTR int64_t stall_last1_us;

static void IRAM_ATTR stall_note(int core, uint32_t us, int64_t now)
{
    static DRAM_ATTR uint32_t n[2];
    stall_ev_t *e = &stall_ev[core][n[core]++ % 4];
    TaskHandle_t me = xTaskGetCurrentTaskHandleForCore(core);
    const XtExcFrame *f = me ? *(XtExcFrame **)me : NULL; /* the TCB's first field: the saved stack top */
    e->n = n[core];
    e->us = us;
    e->at = now;
    e->pc = f ? (uint32_t)f->pc : 0;
    e->a0 = f ? (uint32_t)f->a0 : 0;
    for (int c = 0; c < 2; c++) {
        TaskHandle_t h = xTaskGetCurrentTaskHandleForCore(c);
        const char *nm = h ? pcTaskGetName(h) : "";
        char *d = c ? e->t1 : e->t0;
        int i = 0;
        for (; i < 11 && nm[i]; i++)
            d[i] = nm[i];
        d[i] = 0;
    }
}

static void IRAM_ATTR stall_tick1(void)
{
    const int64_t now = esp_timer_get_time();
    if (stall_last1_us && now - stall_last1_us > 5000)
        stall_note(1, (uint32_t)(now - stall_last1_us), now);
    stall_last1_us = now;
}

static void IRAM_ATTR stall_tick(void)
{
    const int64_t now = esp_timer_get_time();
    if (stall_last_us && now - stall_last_us > stall_tick_gap_us)
        stall_tick_gap_us = (uint32_t)(now - stall_last_us);
    if (stall_last_us && now - stall_last_us > 5000)
        stall_note(0, (uint32_t)(now - stall_last_us), now);
    stall_last_us = now;
}

void stall_watch_start(void)
{
    esp_register_freertos_tick_hook_for_cpu(stall_tick, 0);
    esp_register_freertos_tick_hook_for_cpu(stall_tick1, 1);
}

/* The longest time between two of core 0's ticks since the last call. */
int stall_watch_take(void)
{
    const int us = (int)stall_tick_gap_us;
    stall_tick_gap_us = 0;
    return us;
}

void flash_watch_json(rdof_json_t *j)
{
    rdof_json_open(j, "flash");
    rdof_json_int(j, "ops", fw.ops);
    rdof_json_int(j, "long_ops", fw.long_ops);
    rdof_json_int(j, "worst_us", fw.worst_us);
    if (fw.long_ops) {
        /* the name of a task that has since ended isn't read: only one still listed */
        const char *name = "";
        TaskHandle_t h = fw.last_by;
        if (h && eTaskGetState(h) != eDeleted)
            name = pcTaskGetName(h);
        rdof_json_int(j, "last_us", fw.last_us);
        rdof_json_int(j, "last_ago_ms", (long long)((esp_timer_get_time() - fw.last_at) / 1000));
        rdof_json_str(j, "last_by", name);
    }
    rdof_json_close(j);
    rdof_json_open(j, "stalls");
    for (int c = 0; c < 2; c++)
        for (int k = 0; k < 4; k++) {
            const stall_ev_t *e = &stall_ev[c][k];
            if (!e->n)
                continue;
            char key[12], pc[12], a0[12];
            snprintf(key, sizeof key, "core%d_%d", c, k);
            snprintf(pc, sizeof pc, "%08x", (unsigned)e->pc);
            snprintf(a0, sizeof a0, "%08x", (unsigned)e->a0);
            rdof_json_open(j, key);
            rdof_json_int(j, "n", e->n);
            rdof_json_int(j, "us", e->us);
            rdof_json_int(j, "ago_ms", (long long)((esp_timer_get_time() - e->at) / 1000));
            rdof_json_str(j, "pc", pc);
            rdof_json_str(j, "a0", a0);
            rdof_json_str(j, "core0", e->t0);
            rdof_json_str(j, "core1", e->t1);
            rdof_json_close(j);
        }
    rdof_json_close(j);
}
