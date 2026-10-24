// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#pragma once
#include <stdint.h>
typedef uint32_t TickType_t;
#define pdMS_TO_TICKS(ms) ((TickType_t)((ms) / 10)) /* 100 Hz ticks */
typedef int BaseType_t;
#define pdTRUE 1
#define pdFALSE 0
#define portYIELD_FROM_ISR() ((void)0)
#define portTICK_PERIOD_MS 10
