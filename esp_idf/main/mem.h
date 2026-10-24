// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The memory plan on the board (mem.c, over rdof/memplan.h): at boot, before Wi-Fi and the roles start, each role
 * declares what it needs of internal, DMA and PSRAM memory; the plan places it, allocates the regions and keeps them
 * for the roles to take. See docs/spec/fw/memory.md.
 *
 * A role that isn't in the plan (an "either" board, where what's plugged in decides) gets NULL from mem_take and
 * allocates as before. A role the plan refused doesn't start: mem_role_why says why.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rdof/json.h"
#include "rdof/memplan.h"

/* Declaring, from each role's *_mem_claims (called by mem_plan_boot). Returns the claim's index or -1. */
int mem_add(const char *role, const char *name, size_t size, size_t align, uint8_t caps, rdof_mem_how_t how);
/* A task's stack (stack bytes, caps) and its control block (internal), as name and name.tcb. Declared already
 * (a split's two halves): nothing more. */
void mem_add_task(const char *role, const char *name, uint32_t stack, uint8_t caps);
/* A HOLD of count blocks of size (declared already: nothing more). */
void mem_add_hold(const char *role, const char *name, size_t size, int count, uint8_t caps);
/* An elastic claim: from size up to max, in steps of unit, from what's spare above the margin. */
void mem_elastic(int claim, size_t max, size_t unit);

/* Declare every role's claims, solve the plan against the heap and allocate it. usb_either: one USB port decides
 * the role, so neither the SDR nor the radios are planned. */
void mem_plan_boot(bool usb_either);

/* 1: the role is planned; 0: it isn't in the plan (allocate as before); -1: refused or released. */
int mem_role_state(const char *role);
const char *mem_role_why(const char *role);
/* A planned ALLOC claim's memory, once (NULL if it isn't planned, or taken already); *size its bytes. */
void *mem_take(const char *role, const char *name, size_t *size);
/* A HOLD's next block freed, just before its driver allocates the same. */
void mem_hand_over(const char *role, const char *name);
/* A task on its planned stack and control block (mem_add_task), or, not planned, on a heap stack of stack bytes.
 * core: a core, or tskNO_AFFINITY. */
bool mem_task(const char *role, const char *name, TaskFunction_t fn, uint32_t stack, void *arg, UBaseType_t prio,
              BaseType_t core, TaskHandle_t *out);

/* The same, on its planned stack only: false, and no task, if it has none. */
bool mem_task_planned(const char *role, const char *name, TaskFunction_t fn, void *arg, UBaseType_t prio,
                      BaseType_t core, TaskHandle_t *out);

/* A stopped role gives its memory back: its tasks deleted, its DMA idle, its pointers dropped first. Returns the
 * internal bytes freed. */
size_t mem_release(const char *role);
/* A claim at run time, against the heap now (the margin kept): its memory, or NULL with why. */
void *mem_claim(const char *role, const char *name, size_t size, size_t align, uint8_t caps, char *why, size_t cap);

/* The status's "memory" object. */
void mem_status_json(rdof_json_t *j);
