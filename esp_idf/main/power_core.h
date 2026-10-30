// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The power key and the battery's numbers: portable, so the host tests run them too (power.c is the board's side). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RDOF_PK_DEBOUNCE_MS 30 /* a release counts once the key has been up this long */

typedef enum {
    RDOF_PK_NONE,
    RDOF_PK_OFF,   /* held off_ms while on: turn the board off */
    RDOF_PK_ON,    /* woken by the key and held on_ms: stay on */
    RDOF_PK_SLEEP, /* woken by the key and let go too soon: back to sleep */
} rdof_pk_event_t;

typedef struct {
    int off_ms, on_ms;
    bool waking;      /* woken by the key: it must stay down on_ms */
    bool armed;       /* the key has been up since it turned the board on: a hold now turns it off */
    bool down, fired; /* the debounced level; OFF said for this hold */
    int64_t since_ms, up_ms; /* down since; up since (-1: not up) */
} rdof_pk_t;

/* woke_by_key: the board came out of sleep by the key, which is down since (about) now_ms. */
void rdof_pk_init(rdof_pk_t *k, int off_ms, int on_ms, bool woke_by_key, int64_t now_ms);
/* The key's level, polled (every 10 to 20 ms). */
rdof_pk_event_t rdof_pk_step(rdof_pk_t *k, bool down, int64_t now_ms);

/* The battery's millivolts from the ADC's, through a divider of ratio_x1000 (the W12's 390k over 100k: 4900). */
int rdof_batt_mv(int adc_mv, int ratio_x1000);
/* No battery below this: the divider reads the charger's output or nothing. */
#define RDOF_BATT_PRESENT_MV 2500
/* A Li-ion cell's charge, 0 to 100, from its voltage (a coarse table, for a cell at rest); -1 for no battery. */
int rdof_batt_percent(int mv);

/* The low-battery shutdown: true once n readings in a row are under cutoff_mv with a battery there. */
typedef struct {
    int under;
} rdof_batt_low_t;
bool rdof_batt_low_step(rdof_batt_low_t *l, int mv, int cutoff_mv, int n);
