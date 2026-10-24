// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The memory plan (mem.h, rdof/memplan.h): each role's claims, declared at boot by the role's own file, solved
 * against the heap with CONFIG_RDOF_MEM_MARGIN_KB kept for Wi-Fi and lwIP, then allocated, the largest internal
 * region first, before Wi-Fi and the roles carve the heap up. docs/spec/fw/memory.md.
 */
#include "mem.h"

#include <stdio.h>
#include <string.h>

#include "app.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/semphr.h"

static const char *TAG = "mem";

#if CONFIG_RDOF_MEM_STRICT
#define STRICT true
#else
#define STRICT false
#endif

EXT_RAM_BSS_ATTR static rdof_memplan_t P;
EXT_RAM_BSS_ATTR static void *holds[RDOF_MEM_MAX_CLAIMS][RDOF_MEM_MAX_COUNT];
static SemaphoreHandle_t lock;
static bool declaring;

static void take_lock(void)
{
    if (lock)
        xSemaphoreTake(lock, portMAX_DELAY);
}

static void give_lock(void)
{
    if (lock)
        xSemaphoreGive(lock);
}

static uint32_t heap_caps_of(int where)
{
    switch (where) {
    case RDOF_MEM_IN_DMA: return MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
    case RDOF_MEM_IN_PSRAM: return MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    default: return MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    }
}

static void avail(rdof_mem_avail_t *a)
{
    a->internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    a->internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    a->dma_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    a->dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    a->psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    a->psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
}

int mem_add(const char *role, const char *name, size_t size, size_t align, uint8_t caps, rdof_mem_how_t how)
{
    if (!declaring)
        return -1;
    return rdof_memplan_add(&P, role, name, (uint32_t)size, (uint16_t)align, caps, how);
}

void mem_add_task(const char *role, const char *name, uint32_t stack, uint8_t caps)
{
    char t[24];
    snprintf(t, sizeof t, "%s.tcb", name);
    if (rdof_memplan_find(&P, role, name) < 0 && mem_add(role, name, stack, 16, caps, RDOF_MEM_ALLOC) >= 0)
        mem_add(role, t, sizeof(StaticTask_t), 8, RDOF_MEM_INTERNAL, RDOF_MEM_ALLOC);
}

void mem_add_hold(const char *role, const char *name, size_t size, int count, uint8_t caps)
{
    if (rdof_memplan_find(&P, role, name) < 0)
        rdof_memplan_count(&P, mem_add(role, name, size, 4, caps, RDOF_MEM_HOLD), count);
}

void mem_elastic(int claim, size_t max, size_t unit) { rdof_memplan_elastic(&P, claim, (uint32_t)max, (uint32_t)unit); }

/* The board's own: tasks and buffers that start after the plan and that aren't any role's. Already running when
 * the plan is made (the status LED, the GNSS reader, netclock's PPS task): in the heap it's measured against. */
static void board_claims(void)
{
    mem_add("board", "httpd", 8192 + 1024, 4, RDOF_MEM_INTERNAL, RDOF_MEM_BUDGET); /* web.c's stack_size, its state */
    mem_add("board", "wifilist", 4096, 4, RDOF_MEM_INTERNAL, RDOF_MEM_BUDGET);
    mem_add("board", "netclock", 6144, 4, RDOF_MEM_INTERNAL, RDOF_MEM_BUDGET); /* its NTP task, once Wi-Fi is up */
#if CONFIG_RDOF_OLED
    if (g_roles.has_sdr || g_roles.has_radio)
        mem_add("board", "oled", 4096, 4, RDOF_MEM_INTERNAL, RDOF_MEM_BUDGET);
#endif
}

/* The USB host's tasks (usb_host.c) and what the host stack allocates of DMA memory, when anything planned is on
 * USB. */
static void usb_claims(bool usb)
{
    if (!usb)
        return;
    mem_add_task("usb", "usb_lib", 4096, RDOF_MEM_INTERNAL);
    mem_add_task("usb", "usb_kick", 2560, RDOF_MEM_INTERNAL);
    mem_add("usb", "host", 4096, 4, RDOF_MEM_DMA, RDOF_MEM_BUDGET);
}

static void free_role(int r)
{
    for (int k = 0; k < P.nr; k++)
        if (P.r[k].role == r && P.r[k].base && !P.r[k].released) {
            heap_caps_free(P.r[k].base);
            P.r[k].base = NULL;
        }
    for (int i = 0; i < P.n; i++)
        if (P.c[i].role == r && P.c[i].how == RDOF_MEM_HOLD)
            for (int b = P.c[i].handed; b < P.c[i].count; b++) {
                heap_caps_free(holds[i][b]);
                holds[i][b] = NULL;
            }
}

static void allocate(void)
{
    /* the regions, largest first (internal ones before PSRAM) */
    bool done[RDOF_MEM_MAX_REGIONS] = {0};
    for (int n = 0; n < P.nr; n++) {
        int best = -1;
        for (int k = 0; k < P.nr; k++) {
            if (done[k])
                continue;
            const bool ps = P.r[k].where == RDOF_MEM_IN_PSRAM, bps = best >= 0 && P.r[best].where == RDOF_MEM_IN_PSRAM;
            if (best < 0 || (bps && !ps) || (ps == bps && P.r[k].bytes > P.r[best].bytes))
                best = k;
        }
        done[best] = true;
        rdof_mem_region_t *g = &P.r[best];
        if (!rdof_memplan_role_ok(&P, P.role[g->role].name))
            continue;
        g->base = heap_caps_aligned_calloc(g->align, 1, g->bytes, heap_caps_of(g->where));
        if (!g->base) {
            char why[112];
            snprintf(why, sizeof why, "%s: its %s region (%u bytes) wasn't allocated", P.role[g->role].name,
                     rdof_mem_where_name(g->where), (unsigned)g->bytes);
            free_role(g->role);
            rdof_memplan_refuse(&P, g->role, why);
        }
    }
    for (int i = 0; i < P.n; i++) {
        rdof_mem_claim_t *c = &P.c[i];
        if (c->how != RDOF_MEM_HOLD || c->where == RDOF_MEM_NOWHERE)
            continue;
        for (int b = 0; b < c->count; b++)
            if (!(holds[i][b] = heap_caps_malloc(c->got, heap_caps_of(c->where)))) {
                char why[112];
                snprintf(why, sizeof why, "%s: %s's hold (%u bytes) wasn't allocated", P.role[c->role].name, c->name,
                         (unsigned)c->got);
                free_role(c->role);
                rdof_memplan_refuse(&P, c->role, why);
                break;
            }
    }
}

void mem_plan_boot(bool usb_either)
{
    lock = xSemaphoreCreateMutex();
    rdof_memplan_init(&P, CONFIG_RDOF_MEM_MARGIN_KB * 1024, STRICT);
    declaring = true;
    bool usb = false;
#if CONFIG_RDOF_SDR
    if (g_roles.has_sdr && !usb_either)
        usb |= sdr_mem_claims(); /* first: the DSP's buffers want the heap in one piece */
#endif
#if CONFIG_RDOF_ROAMER
    if (g_roles.has_radio && !usb_either)
        usb |= roamer_mem_claims();
#endif
#if CONFIG_RDOF_IPGW
    ipgw_mem_claims();
#endif
    usb_claims(usb);
    board_claims();
    declaring = false;
    rdof_mem_avail_t a;
    avail(&a);
    rdof_memplan_solve(&P, &a);
    allocate();
    EXT_RAM_BSS_ATTR static char d[768]; /* not on the main task's stack: its peak is the planning, under esp-dsp's setup */
    rdof_memplan_describe(&P, d, sizeof d);
    for (char *line = strtok(d, "\n"); line; line = strtok(NULL, "\n"))
        ESP_LOGI(TAG, "%s", line);
    rdof_mem_avail_t now;
    avail(&now);
    ESP_LOGI(TAG, "planned with %u KB internal free (%u KB in one piece), %u KB margin: %u KB free now, %lld KB spare",
             (unsigned)(a.internal_free / 1024), (unsigned)(a.internal_largest / 1024), CONFIG_RDOF_MEM_MARGIN_KB,
             (unsigned)(now.internal_free / 1024), (long long)(P.spare / 1024));
    if (!P.ok)
        ESP_LOGE(TAG, "%s", P.error);
}

int mem_role_state(const char *role)
{
    const int r = rdof_memplan_role(&P, role);
    return r < 0 ? 0 : P.role[r].state == RDOF_ROLE_PLANNED ? 1 : -1;
}

const char *mem_role_why(const char *role)
{
    const int r = rdof_memplan_role(&P, role);
    return r < 0 ? "" : P.role[r].why;
}

void *mem_take(const char *role, const char *name, size_t *size)
{
    void *p = NULL;
    take_lock();
    const int i = rdof_memplan_find(&P, role, name);
    if (i >= 0) {
        rdof_mem_claim_t *c = &P.c[i];
        if (c->how == RDOF_MEM_ALLOC && c->region >= 0 && !c->taken && P.r[c->region].base &&
            !P.r[c->region].released && P.role[c->role].state == RDOF_ROLE_PLANNED) {
            p = (uint8_t *)P.r[c->region].base + c->offset;
            c->taken = true;
            if (size)
                *size = (size_t)c->got * c->count;
        }
    }
    give_lock();
    return p;
}

void mem_hand_over(const char *role, const char *name)
{
    take_lock();
    const int i = rdof_memplan_find(&P, role, name);
    if (i >= 0 && P.c[i].how == RDOF_MEM_HOLD && P.c[i].where != RDOF_MEM_NOWHERE && P.c[i].handed < P.c[i].count) {
        heap_caps_free(holds[i][P.c[i].handed]);
        holds[i][P.c[i].handed] = NULL;
        rdof_memplan_hand_over(&P, i);
    }
    give_lock();
}

bool mem_task_planned(const char *role, const char *name, TaskFunction_t fn, void *arg, UBaseType_t prio,
                      BaseType_t core, TaskHandle_t *out)
{
    char t[24];
    snprintf(t, sizeof t, "%s.tcb", name);
    size_t n = 0;
    StackType_t *st = mem_take(role, name, &n);
    StaticTask_t *tcb = st ? mem_take(role, t, NULL) : NULL;
    TaskHandle_t h = st && tcb ? xTaskCreateStaticPinnedToCore(fn, name, (uint32_t)n, arg, prio, st, tcb, core) : NULL;
    if (out)
        *out = h;
    return h != NULL;
}

bool mem_task(const char *role, const char *name, TaskFunction_t fn, uint32_t stack, void *arg, UBaseType_t prio,
              BaseType_t core, TaskHandle_t *out)
{
    TaskHandle_t h = NULL;
    if (!mem_task_planned(role, name, fn, arg, prio, core, &h)) {
        if (mem_role_state(role) == 1)
            ESP_LOGW(TAG, "%s %s: no planned stack, %u bytes from the heap", role, name, (unsigned)stack);
        if (xTaskCreatePinnedToCore(fn, name, stack, arg, prio, &h, core) != pdPASS)
            h = NULL;
    }
    if (out)
        *out = h;
    return h != NULL;
}

size_t mem_release(const char *role)
{
    take_lock();
    const int r = rdof_memplan_role(&P, role);
    size_t n = 0;
    if (r >= 0 && P.role[r].state == RDOF_ROLE_PLANNED) {
        free_role(r);
        n = rdof_memplan_release(&P, role);
    }
    give_lock();
    if (n)
        ESP_LOGI(TAG, "%s released: %u KB of internal RAM given back", role, (unsigned)(n / 1024));
    return n;
}

void *mem_claim(const char *role, const char *name, size_t size, size_t align, uint8_t caps, char *why, size_t cap)
{
    rdof_mem_avail_t now;
    avail(&now);
    take_lock();
    void *p = NULL;
    const int i = rdof_memplan_add_now(&P, role, name, (uint32_t)size, (uint16_t)align, caps, &now, why, cap);
    if (i >= 0) {
        rdof_mem_claim_t *c = &P.c[i];
        rdof_mem_region_t *g = &P.r[c->region];
        g->base = heap_caps_aligned_calloc(g->align, 1, g->bytes, heap_caps_of(g->where));
        if (g->base) {
            c->taken = true;
            p = g->base;
        } else {
            g->released = true;
            c->where = RDOF_MEM_NOWHERE;
            c->region = -1;
            if (cap)
                snprintf(why, cap, "%s %s: %u bytes not allocated", role, name, (unsigned)size);
        }
    }
    give_lock();
    return p;
}

void mem_status_json(rdof_json_t *j)
{
    rdof_mem_avail_t now;
    avail(&now);
    take_lock();
    rdof_memplan_json(&P, &now, j);
    give_lock();
}
