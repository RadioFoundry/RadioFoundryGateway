// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#pragma once
#include "FreeRTOS.h"
/* A binary semaphore: the fake has one, its count in F.sem_count; a take with a timeout advances the clock. */
typedef struct shim_sem *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateBinary(void);
void vSemaphoreDelete(SemaphoreHandle_t s);
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks);
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t s, BaseType_t *woke);
