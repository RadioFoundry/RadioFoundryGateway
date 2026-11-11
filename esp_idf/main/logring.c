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

/* The copy runs outside the lock: the ring is up to 256 KB of PSRAM, and a copy under the lock kept interrupts off
 * for 13 ms on the caller's core (the LR2021's reader lost ten FIFOs to each /api/log, W12 #3, 2026-11-10). Lines
 * logged during the copy overwrite the ring's oldest bytes, so what they took is cut from the front after it. */
size_t logring_read(char *out, size_t cap)
{
    if (!ring.buf || cap == 0)
        return 0;
    portENTER_CRITICAL(&lock);
    const size_t h1 = ring.head;
    const bool w1 = ring.wrapped;
    portEXIT_CRITICAL(&lock);
    size_t total = w1 ? ring.size : h1, start = w1 ? h1 : 0, skip = 0; /* skip: the oldest bytes left out */
    if (total > cap - 1) {
        skip = total - (cap - 1);
        start = (start + skip) % ring.size;
        total = cap - 1;
    }
    const size_t a = total < ring.size - start ? total : ring.size - start;
    memcpy(out, ring.buf + start, a);
    memcpy(out + a, ring.buf, total - a);
    portENTER_CRITICAL(&lock);
    const size_t h2 = ring.head;
    const bool w2 = ring.wrapped;
    portEXIT_CRITICAL(&lock);
    /* the oldest bytes written over meanwhile (fewer than a ring's worth are logged during one copy) */
    const size_t over = w1 ? (h2 + ring.size - h1) % ring.size : w2 ? h2 : 0;
    if (over > skip) {
        size_t drop = over - skip < total ? over - skip : total;
        while (drop < total && out[drop - 1] != '\n')
            drop++; /* to the next line's start */
        memmove(out, out + drop, total - drop);
        total -= drop;
    }
    out[total] = 0;
    return total;
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
