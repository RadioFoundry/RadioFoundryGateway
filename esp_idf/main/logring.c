// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The log, kept in a ring (rdof_bring_t, rdof/webapi.h) as well as printed, so the web page can show it with no
 * serial adapter. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "rdof/webapi.h"

#if CONFIG_RDOF_LOG_RING_KB > 0

static rdof_bring_t ring;
static vprintf_like_t original;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

static int ring_vprintf(const char *fmt, va_list args)
{
    char line[256];
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(line, sizeof line, fmt, copy);
    va_end(copy);
    if (n > 0) {
        size_t len = (size_t)n < sizeof line ? (size_t)n : sizeof line - 1;
        portENTER_CRITICAL(&lock);
        rdof_bring_put(&ring, line, len);
        portEXIT_CRITICAL(&lock);
    }
    return original(fmt, args);
}

void logring_init(void)
{
    const size_t size = (size_t)CONFIG_RDOF_LOG_RING_KB * 1024;
    char *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf)
        buf = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    if (!buf)
        return;
    rdof_bring_init(&ring, buf, size);
    original = esp_log_set_vprintf(ring_vprintf);
}

size_t logring_read(char *out, size_t cap)
{
    if (!ring.buf || cap == 0)
        return 0;
    portENTER_CRITICAL(&lock);
    size_t n = rdof_bring_read(&ring, out, cap);
    portEXIT_CRITICAL(&lock);
    return n;
}

#else

void logring_init(void) {}

size_t logring_read(char *out, size_t cap)
{
    if (cap)
        out[0] = 0;
    return 0;
}

#endif
