// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The log, kept in a ring as well as printed, so the web page can show it with no serial adapter. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#if CONFIG_RDOF_LOG_RING_KB > 0

static char *ring;
static size_t ring_size, head; /* head: where the next byte goes */
static bool wrapped;
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
        for (size_t i = 0; i < len; i++) {
            ring[head++] = line[i];
            if (head == ring_size) {
                head = 0;
                wrapped = true;
            }
        }
        portEXIT_CRITICAL(&lock);
    }
    return original(fmt, args);
}

void logring_init(void)
{
    ring_size = (size_t)CONFIG_RDOF_LOG_RING_KB * 1024;
    ring = heap_caps_malloc(ring_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ring)
        ring = heap_caps_malloc(ring_size, MALLOC_CAP_8BIT);
    if (ring)
        original = esp_log_set_vprintf(ring_vprintf);
}

size_t logring_read(char *out, size_t cap)
{
    size_t n = 0;
    if (!ring || cap == 0)
        return 0;
    portENTER_CRITICAL(&lock);
    size_t total = wrapped ? ring_size : head;
    size_t start = wrapped ? head : 0;
    if (total > cap - 1) {
        start = (start + total - (cap - 1)) % ring_size;
        total = cap - 1;
    }
    for (; n < total; n++)
        out[n] = ring[(start + n) % ring_size];
    portEXIT_CRITICAL(&lock);
    out[n] = 0;
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
