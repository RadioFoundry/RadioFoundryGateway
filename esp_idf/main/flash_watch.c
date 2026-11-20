// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* Who writes the flash, and for how long. A flash write or erase turns the cache off, and every task on both cores
 * waits (a sector erase about 35 ms): the LR2021's reader loses its FIFO meanwhile. The linker wraps the two calls
 * around each flash operation (CMakeLists.txt); /api/status "flash" gives the count, the longest, and the task and
 * time of the last one that took over a millisecond (a read is far shorter). The count is also how often the flash
 * is written: long_ops is the writes and erases. */
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_cpu_utils.h"
#include "esp_debug_helpers.h"
#include "esp_memory_utils.h"
#include "xtensa_context.h"
#include "esp_freertos_hooks.h"

#include "app.h"

void __real_spi_flash_disable_interrupts_caches_and_other_cpu(void);
void __real_spi_flash_enable_interrupts_caches_and_other_cpu(void);

static DRAM_ATTR struct {
    volatile bool cache_off; /* a flash operation is on: PSRAM is out of reach (the profiler's ring is there) */
    uint32_t ops, long_ops;
    int64_t t0, last_at;
    uint32_t worst_us, last_us;
    TaskHandle_t last_by;
} fw;

void IRAM_ATTR __wrap_spi_flash_disable_interrupts_caches_and_other_cpu(void)
{
    __real_spi_flash_disable_interrupts_caches_and_other_cpu();
    fw.cache_off = true;
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
    fw.cache_off = false;
}


/* Stalls: each core's tick notes a gap over 5 ms between two ticks (interrupts off, or the cache off, for that
 * long). /api/status "stalls" lists the last four on each core. */
/* A stall worth noting: longer than the LR2021 FIFO rides through (about 2 ms), with room for one late tick. */
#define STALL_US 2500
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

/* The profiler: each core's tick (1 kHz) notes the PC its running task was interrupted at, and the task. In PSRAM
 * (no internal RAM spent), skipped while a flash operation has the cache off. */
#define PROF_N 16384
#define PROF_DEPTH 4
#define PROF_TASKS 48
typedef struct {
    uint32_t pc[PROF_DEPTH]; /* where it was, then its callers (0: not known) */
    uint8_t task, core;      /* prof_task[task] (PROF_TASKS: not noted), the core it ran on */
} prof_ev_t;
/* The tasks seen this run, each name taken when first seen: a task may be gone by the output (PSRAM: the tick
 * skips while the cache is off) */
EXT_RAM_BSS_ATTR static struct {
    TaskHandle_t h;
    char name[configMAX_TASK_NAME_LEN];
} prof_task[PROF_TASKS];
static DRAM_ATTR uint32_t prof_tasks;
static portMUX_TYPE prof_mux = portMUX_INITIALIZER_UNLOCKED;

static uint8_t IRAM_ATTR prof_task_of(TaskHandle_t me)
{
    uint8_t k = PROF_TASKS;
    portENTER_CRITICAL_ISR(&prof_mux); /* both cores' ticks */
    for (uint32_t i = 0; i < prof_tasks; i++)
        if (prof_task[i].h == me) {
            k = (uint8_t)i;
            break;
        }
    if (k == PROF_TASKS && prof_tasks < PROF_TASKS) {
        k = (uint8_t)prof_tasks++;
        prof_task[k].h = me;
        const char *nm = pcTaskGetName(me);
        int j = 0;
        for (; j < configMAX_TASK_NAME_LEN - 1 && nm[j]; j++)
            prof_task[k].name[j] = nm[j];
        prof_task[k].name[j] = 0;
    }
    portEXIT_CRITICAL_ISR(&prof_mux);
    return k;
}
EXT_RAM_BSS_ATTR static prof_ev_t prof_ev[PROF_N];
static DRAM_ATTR volatile uint32_t prof_n;
static DRAM_ATTR volatile bool prof_on;

static void IRAM_ATTR prof_note(int core)
{
    if (!prof_on || fw.cache_off)
        return;
    TaskHandle_t me = xTaskGetCurrentTaskHandleForCore(core);
    const XtExcFrame *f = me ? *(XtExcFrame **)me : NULL; /* the TCB's first field: the saved stack top */
    const uint32_t i = __atomic_fetch_add(&prof_n, 1, __ATOMIC_RELAXED);
    if (i >= PROF_N)
        return;
    prof_ev_t e = {.task = me ? prof_task_of(me) : PROF_TASKS, .core = (uint8_t)core};
    if (f) {
        /* the callers from the stack's base save areas (the interrupt's entry spilled the register windows); each
           step checked, so a bad frame ends the walk rather than faulting */
        esp_backtrace_frame_t fr = {.pc = (uint32_t)f->pc, .sp = (uint32_t)f->a1, .next_pc = (uint32_t)f->a0};
        e.pc[0] = fr.pc;
        for (int d = 1; d < PROF_DEPTH && fr.next_pc && esp_stack_ptr_is_sane(fr.sp); d++) {
            const bool more = esp_backtrace_get_next_frame(&fr);
            const uint32_t pc = esp_cpu_process_stack_pc(fr.pc);
            if (!esp_ptr_executable((void *)pc))
                break;
            e.pc[d] = pc;
            if (!more)
                break;
        }
    }
    prof_ev[i] = e;
}

static int prof_cmp(const void *a, const void *b)
{
    const prof_ev_t *x = a, *y = b;
    if (x->task != y->task)
        return x->task < y->task ? -1 : 1;
    if (x->core != y->core)
        return x->core < y->core ? -1 : 1;
    for (int d = 0; d < PROF_DEPTH; d++)
        if (x->pc[d] != y->pc[d])
            return x->pc[d] < y->pc[d] ? -1 : 1;
    return 0;
}

/* Sample both cores for ms (at most 8 s: PROF_N ticks), then each task's samples and its PCs, grouped by task: "n
 * task core pc" lines (the core it ran on then), for addr2line on the running image's ELF. NULL when one is already running. */
char *prof_run(int ms, size_t *len)
{
    static bool busy;
    if (busy)
        return NULL;
    busy = true;
    if (ms < 100)
        ms = 100;
    if (ms > 8000)
        ms = 8000;
    prof_n = 0;
    prof_tasks = 0;
    prof_on = true;
    vTaskDelay(pdMS_TO_TICKS(ms));
    prof_on = false;
    vTaskDelay(2); /* a tick still noting finishes */
    const uint32_t n = prof_n < PROF_N ? prof_n : PROF_N;
    qsort(prof_ev, n, sizeof prof_ev[0], prof_cmp);
    const size_t cap = 160 * 1024;
    char *out = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t o = 0;
    if (out) {
        o += (size_t)snprintf(out, cap, "%u samples in %d ms (both cores' ticks): n task core pc caller caller caller\n", (unsigned)n, ms);
        for (uint32_t i = 0; i < n && o + 80 < cap;) {
            uint32_t j = i;
            while (j < n && !prof_cmp(&prof_ev[j], &prof_ev[i]))
                j++;
            const prof_ev_t *e = &prof_ev[i];
            o += (size_t)snprintf(out + o, cap - o, "%u %s %u %08x %08x %08x %08x\n", (unsigned)(j - i),
                                  e->task < PROF_TASKS ? prof_task[e->task].name : "?", (unsigned)e->core, (unsigned)e->pc[0],
                                  (unsigned)e->pc[1], (unsigned)e->pc[2], (unsigned)e->pc[3]);
            i = j;
        }
    }
    busy = false;
    *len = o;
    return out;
}

static void IRAM_ATTR stall_tick1(void)
{
    prof_note(1);
    const int64_t now = esp_timer_get_time();
    if (stall_last1_us && now - stall_last1_us > STALL_US)
        stall_note(1, (uint32_t)(now - stall_last1_us), now);
    stall_last1_us = now;
}

static void IRAM_ATTR stall_tick(void)
{
    prof_note(0);
    const int64_t now = esp_timer_get_time();
    if (stall_last_us && now - stall_last_us > stall_tick_gap_us)
        stall_tick_gap_us = (uint32_t)(now - stall_last_us);
    if (stall_last_us && now - stall_last_us > STALL_US)
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

/* What's written: every program and erase the flash driver is asked for, wrapped too. A sector lasts about
 * 100,000 erases; NVS erases one of its pages once its 126 entries are used, so writes / 126 is about its erases. */
#include "esp_flash.h"
esp_err_t __real_esp_flash_write(esp_flash_t *chip, const void *buf, uint32_t addr, uint32_t len);
esp_err_t __real_esp_flash_erase_region(esp_flash_t *chip, uint32_t start, uint32_t len);
static DRAM_ATTR uint32_t fw_writes, fw_write_bytes, fw_erases, fw_erase_kb;

esp_err_t __wrap_esp_flash_write(esp_flash_t *chip, const void *buf, uint32_t addr, uint32_t len)
{
    fw_writes++;
    fw_write_bytes += len;
    return __real_esp_flash_write(chip, buf, addr, len);
}

esp_err_t __wrap_esp_flash_erase_region(esp_flash_t *chip, uint32_t start, uint32_t len)
{
    fw_erases++;
    fw_erase_kb += len / 1024;
    return __real_esp_flash_erase_region(chip, start, len);
}

void flash_watch_json(rdof_json_t *j)
{
    rdof_json_open(j, "flash");
    rdof_json_int(j, "ops", fw.ops);
    rdof_json_int(j, "long_ops", fw.long_ops);
    rdof_json_int(j, "worst_us", fw.worst_us);
    rdof_json_int(j, "writes", fw_writes); /* since boot */
    rdof_json_int(j, "write_bytes", fw_write_bytes);
    rdof_json_int(j, "erases", fw_erases);
    rdof_json_int(j, "erase_kb", fw_erase_kb);
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
