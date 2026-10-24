// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#pragma once
#include <stddef.h>
#include <stdlib.h>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_DMA 2
void *heap_caps_calloc(size_t n, size_t size, int caps);
size_t heap_caps_get_largest_free_block(int caps);
void shim_free(void *p);
#define free(p) shim_free(p) /* src/lr2021.c frees its handle with free(): count it */
