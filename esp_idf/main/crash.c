// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The core dump in flash (CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH, ELF): on a panic the IDF writes every task's TCB and
 * stack to the coredump partition and reboots. The dump stays until the next crash overwrites it or it's erased, so
 * it may come from an earlier image: "elf" says which (the start of its ELF's SHA-256).
 *   GET    /api/coredump   the dump as stored (application/octet-stream), for esp-coredump -t raw on the Mac;
 *                          404 "no core dump" when there is none
 *   DELETE /api/coredump   erase it
 *   POST   /api/crash      crash now: a store to address 0 (CONFIG_RDOF_CRASH_TEST, debug builds only)
 * Its summary is read once at boot, logged in one line, and kept for /api/status.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
#include "esp_core_dump.h"
#include "esp_flash.h"
#endif

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH

static const char *TAG = "crash";

typedef struct {
    size_t size;    /* the dump's bytes in flash; 0: none */
    esp_err_t check; /* esp_core_dump_image_check: ESP_OK, or why the dump is bad (then nothing else is set) */
#if CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    bool summary;
    char elf[APP_ELF_SHA256_SZ], task[16], details[128];
    bool this_image, bt_corrupted;
    uint32_t pc, cause, vaddr, bt[16], depth;
#endif
} crash_t;

EXT_RAM_BSS_ATTR static crash_t s_crash;

#if CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
/* EXCCAUSE as the IDF's panic handler names it (panic_arch.c); 64 and up are its own causes (esp-coredump's names). */
static const char *cause_name(uint32_t c)
{
    static const char *const hw[] = {
        "IllegalInstruction", "Syscall", "InstructionFetchError", "LoadStoreError", "Level1Interrupt", "Alloca",
        "IntegerDivideByZero", "PCValue", "Privileged", "LoadStoreAlignment", NULL, NULL, "InstrPDAddrError",
        "LoadStorePIFDataError", "InstrPIFAddrError", "LoadStorePIFAddrError", "InstTLBMiss", "InstTLBMultiHit",
        "InstFetchPrivilege", NULL, "InstrFetchProhibited", NULL, NULL, NULL, "LoadStoreTLBMiss",
        "LoadStoreTLBMultihit", "LoadStorePrivilege", NULL, "LoadProhibited", "StoreProhibited", NULL, NULL,
        "Cp0Dis", "Cp1Dis", "Cp2Dis", "Cp3Dis", "Cp4Dis", "Cp5Dis", "Cp6Dis", "Cp7Dis"};
    static const char *const esp[] = {"UnknownException", "DebugException", "DoubleException", "KernelException",
                                      "CoprocessorException", "InterruptWDTTimeoutCPU0", "InterruptWDTTimeoutCPU1",
                                      "CacheError"};
    const char *n = NULL;
    if (c < sizeof hw / sizeof hw[0])
        n = hw[c];
    else if (c >= 64 && c - 64 < sizeof esp / sizeof esp[0])
        n = esp[c - 64];
    return n ? n : "unknown";
}

static void read_summary(void)
{
    esp_core_dump_summary_t *s = heap_caps_malloc(sizeof *s, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s || esp_core_dump_get_summary(s) != ESP_OK) {
        free(s);
        return;
    }
    char mine[APP_ELF_SHA256_SZ];
    esp_app_get_elf_sha256(mine, sizeof mine);
    snprintf(s_crash.elf, sizeof s_crash.elf, "%s", (const char *)s->app_elf_sha256);
    s_crash.this_image = !strncmp(s_crash.elf, mine, sizeof mine - 1);
    snprintf(s_crash.task, sizeof s_crash.task, "%s", s->exc_task);
    s_crash.pc = s->exc_pc;
    s_crash.cause = s->ex_info.exc_cause;
    s_crash.vaddr = s->ex_info.exc_vaddr;
    s_crash.depth = s->exc_bt_info.depth < 16 ? s->exc_bt_info.depth : 16;
    memcpy(s_crash.bt, s->exc_bt_info.bt, sizeof s_crash.bt);
    s_crash.bt_corrupted = s->exc_bt_info.corrupted;
    free(s);
    /* abort()'s and the task watchdog's message; none for an exception */
    if (esp_core_dump_get_panic_reason(s_crash.details, sizeof s_crash.details) != ESP_OK)
        s_crash.details[0] = 0;
    s_crash.summary = true;
}
#endif

void crash_init(void)
{
    size_t addr, size;
    memset(&s_crash, 0, sizeof s_crash);
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK)
        return; /* none (a blank partition), or no partition */
    s_crash.size = size;
    s_crash.check = esp_core_dump_image_check();
    if (s_crash.check != ESP_OK) {
        ESP_LOGW(TAG, "core dump in flash (%u bytes) is bad: %s; GET /api/coredump", (unsigned)size,
                 esp_err_to_name(s_crash.check));
        return;
    }
#if CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    read_summary();
    if (s_crash.summary) {
        ESP_LOGW(TAG, "core dump in flash (%u bytes, %s%s): task %s, PC 0x%08lx, %s (%lu), EXCVADDR 0x%08lx%s%s; "
                 "GET /api/coredump",
                 (unsigned)size, s_crash.this_image ? "this image" : "image ", s_crash.this_image ? "" : s_crash.elf,
                 s_crash.task, (unsigned long)s_crash.pc, cause_name(s_crash.cause), (unsigned long)s_crash.cause,
                 (unsigned long)s_crash.vaddr, s_crash.details[0] ? ": " : "", s_crash.details);
        return;
    }
#endif
    ESP_LOGW(TAG, "core dump in flash (%u bytes); GET /api/coredump", (unsigned)size);
}

void crash_status_json(rdof_json_t *j)
{
    if (!s_crash.size)
        return;
    rdof_json_open(j, "crash");
    rdof_json_int(j, "size", (long long)s_crash.size);
    if (s_crash.check != ESP_OK) {
        rdof_json_str(j, "error", esp_err_to_name(s_crash.check));
        rdof_json_close(j);
        return;
    }
#if CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    if (s_crash.summary) {
        char hex[12];
        rdof_json_str(j, "elf", s_crash.elf);
        rdof_json_bool(j, "this_image", s_crash.this_image);
        rdof_json_str(j, "task", s_crash.task);
        snprintf(hex, sizeof hex, "0x%08lx", (unsigned long)s_crash.pc);
        rdof_json_str(j, "pc", hex);
        rdof_json_int(j, "exccause", s_crash.cause);
        rdof_json_str(j, "cause", cause_name(s_crash.cause));
        snprintf(hex, sizeof hex, "0x%08lx", (unsigned long)s_crash.vaddr);
        rdof_json_str(j, "excvaddr", hex);
        char bt[16 * 11 + 1];
        size_t n = 0;
        bt[0] = 0;
        for (uint32_t i = 0; i < s_crash.depth; i++)
            n += (size_t)snprintf(bt + n, sizeof bt - n, "%s0x%08lx", i ? " " : "", (unsigned long)s_crash.bt[i]);
        rdof_json_str(j, "backtrace", bt);
        rdof_json_bool(j, "backtrace_corrupted", s_crash.bt_corrupted);
        if (s_crash.details[0])
            rdof_json_str(j, "details", s_crash.details);
    }
#endif
    rdof_json_close(j);
}

static esp_err_t get_coredump(httpd_req_t *req)
{
    size_t addr, size;
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no core dump");
    static char disp[96];
    snprintf(disp, sizeof disp, "attachment; filename=\"%s-coredump%s%s.bin\"", g_roles.box,
#if CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
             s_crash.summary ? "-" : "", s_crash.summary ? s_crash.elf : ""
#else
             "", ""
#endif
    );
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    static char buf[4096]; /* httpd's one task */
    for (size_t off = 0; off < size;) {
        size_t n = size - off < sizeof buf ? size - off : sizeof buf;
        if (esp_flash_read(NULL, buf, addr + off, n) != ESP_OK || httpd_resp_send_chunk(req, buf, (ssize_t)n) != ESP_OK) {
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_FAIL;
        }
        off += n;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t delete_coredump(httpd_req_t *req)
{
    esp_err_t e = esp_core_dump_image_erase();
    if (e != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(e));
    memset(&s_crash, 0, sizeof s_crash);
    ESP_LOGI(TAG, "core dump erased");
    return httpd_resp_sendstr(req, "erased");
}

#if CONFIG_RDOF_CRASH_TEST
static void crash_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500)); /* the reply goes out first */
    ESP_LOGW(TAG, "crashing on purpose (POST /api/crash)");
    *(volatile uint32_t *)arg = 0xdead; /* StoreProhibited at address 0 */
    vTaskDelete(NULL);
}

static esp_err_t post_crash(httpd_req_t *req)
{
    httpd_resp_sendstr(req, "crashing: the board saves a core dump and restarts");
    xTaskCreate(crash_task, "crash_test", 2048, NULL, 5, NULL);
    return ESP_OK;
}
#endif

void crash_register(void *server)
{
    const httpd_uri_t uris[] = {
        {.uri = "/api/coredump", .method = HTTP_GET, .handler = get_coredump},
        {.uri = "/api/coredump", .method = HTTP_DELETE, .handler = delete_coredump},
#if CONFIG_RDOF_CRASH_TEST
        {.uri = "/api/crash", .method = HTTP_POST, .handler = post_crash},
#endif
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++)
        httpd_register_uri_handler((httpd_handle_t)server, &uris[i]);
}

#else

void crash_init(void) {}
void crash_status_json(rdof_json_t *j) {}

static esp_err_t no_coredump(httpd_req_t *req)
{
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no core dump (not in this build)");
}

void crash_register(void *server)
{
    const httpd_uri_t uri = {.uri = "/api/coredump", .method = HTTP_GET, .handler = no_coredump};
    httpd_register_uri_handler((httpd_handle_t)server, &uri);
}

#endif
