// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#include "rdof/sdr/iq.h"
#include "rdof/sdr/features.h"

#include <stdlib.h>
#include <string.h>

#include "sdr_private.h"

#if RDOF_FEATURE_SDR

static void *(*alloc_fn)(size_t) = malloc;
static void (*free_fn)(void *) = free;
static void *(*hot_fn)(size_t);

void rdof_sdr_set_hot_allocator(void *(*alloc)(size_t n)) { hot_fn = alloc; }

void *rdof_sdr_alloc_hot(size_t n) { return (hot_fn ? hot_fn : alloc_fn)(n ? n : 1); }

void rdof_sdr_set_allocator(void *(*alloc)(size_t n), void (*release)(void *p))
{
    alloc_fn = alloc ? alloc : malloc;
    free_fn = release ? release : free;
}

void *rdof_sdr_alloc(size_t n) { return alloc_fn(n ? n : 1); }

void *rdof_sdr_zalloc(size_t n)
{
    void *p = alloc_fn(n ? n : 1);
    if (p)
        memset(p, 0, n);
    return p;
}

void rdof_sdr_free(void *p)
{
    if (p)
        free_fn(p);
}

/* (v - 127.5) / 127.5 for every byte, divided once: the same floats, no divide per sample. Filled on
 * first use; two threads racing to fill it write the same values. */
static float cu8_lut[256];
static volatile int cu8_ready;

void rdof_iq_from_cu8(const uint8_t *in, size_t n, rdof_cf_t *out)
{
    if (!cu8_ready) {
        for (int v = 0; v < 256; v++)
            cu8_lut[v] = ((float)v - 127.5f) / 127.5f;
        cu8_ready = 1;
    }
    for (size_t i = 0; i < n; i++) {
        out[i].re = cu8_lut[in[2 * i]];
        out[i].im = cu8_lut[in[2 * i + 1]];
    }
}

void rdof_iq_from_cs8(const int8_t *in, size_t n, rdof_cf_t *out)
{
    for (size_t i = 0; i < n; i++) {
        out[i].re = (float)in[2 * i] / 128.0f;
        out[i].im = (float)in[2 * i + 1] / 128.0f;
    }
}

void rdof_iq_from_cs16(const int16_t *in, size_t n, rdof_cf_t *out)
{
    for (size_t i = 0; i < n; i++) {
        out[i].re = (float)in[2 * i] / 32768.0f;
        out[i].im = (float)in[2 * i + 1] / 32768.0f;
    }
}

#endif
