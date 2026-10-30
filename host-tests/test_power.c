// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The power key's holds and the battery's numbers (esp_idf/main/power_core.c). */
#include <stdio.h>

#include "power_core.h"

static int failures;

#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                        \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

/* Hold the key at level down from t to t + ms, polled every 10 ms: the first event other than NONE, and when. */
static rdof_pk_event_t run(rdof_pk_t *k, bool down, int64_t *t, int ms, int64_t *at)
{
    rdof_pk_event_t got = RDOF_PK_NONE;
    for (int64_t end = *t + ms; *t < end; *t += 10) {
        rdof_pk_event_t e = rdof_pk_step(k, down, *t);
        if (e != RDOF_PK_NONE && got == RDOF_PK_NONE) {
            got = e;
            if (at)
                *at = *t;
        }
    }
    return got;
}

static void key_off(void)
{
    rdof_pk_t k;
    int64_t t = 0, at = 0;
    rdof_pk_init(&k, 2000, 1000, false, t);
    CHECK(run(&k, false, &t, 100, NULL) == RDOF_PK_NONE);
    CHECK(run(&k, true, &t, 1500, NULL) == RDOF_PK_NONE); /* a short hold does nothing */
    CHECK(run(&k, false, &t, 100, NULL) == RDOF_PK_NONE);
    int64_t from = t;
    CHECK(run(&k, true, &t, 3000, &at) == RDOF_PK_OFF); /* once, at 2 s */
    CHECK(at - from >= 2000 && at - from < 2020);
    /* a bounce while held: up for less than the debounce doesn't restart the hold */
    rdof_pk_init(&k, 2000, 1000, false, t = 0);
    run(&k, false, &t, 100, NULL);
    from = t;
    run(&k, true, &t, 1000, NULL);
    CHECK(run(&k, false, &t, 20, NULL) == RDOF_PK_NONE);
    CHECK(run(&k, true, &t, 1500, &at) == RDOF_PK_OFF && at - from < 2030);
}

static void key_at_boot(void)
{
    /* held at a normal boot (never let go since): no OFF until it has been up */
    rdof_pk_t k;
    int64_t t = 0;
    rdof_pk_init(&k, 2000, 1000, false, t);
    CHECK(run(&k, true, &t, 5000, NULL) == RDOF_PK_NONE);
    run(&k, false, &t, 100, NULL);
    CHECK(run(&k, true, &t, 2100, NULL) == RDOF_PK_OFF);
}

static void key_wake(void)
{
    rdof_pk_t k;
    int64_t t = 0, at = 0;
    /* held through on_ms: ON; held on past off_ms: still on, until let go and held again */
    rdof_pk_init(&k, 2000, 1000, true, t);
    CHECK(run(&k, true, &t, 1500, &at) == RDOF_PK_ON && at >= 1000 && at < 1020);
    CHECK(run(&k, true, &t, 3000, NULL) == RDOF_PK_NONE);
    run(&k, false, &t, 100, NULL);
    CHECK(run(&k, true, &t, 2100, NULL) == RDOF_PK_OFF);
    /* a bump: let go before on_ms, back to sleep (after the debounce) */
    rdof_pk_init(&k, 2000, 1000, true, t = 0);
    CHECK(run(&k, true, &t, 300, NULL) == RDOF_PK_NONE);
    CHECK(run(&k, false, &t, 100, &at) == RDOF_PK_SLEEP && at - 300 >= RDOF_PK_DEBOUNCE_MS);
    /* a bounce at the start of the wake doesn't send it back */
    rdof_pk_init(&k, 2000, 1000, true, t = 0);
    CHECK(run(&k, false, &t, 20, NULL) == RDOF_PK_NONE);
    CHECK(run(&k, true, &t, 1100, NULL) == RDOF_PK_ON);
}

static void battery(void)
{
    CHECK(rdof_batt_mv(800, 4900) == 3920);
    CHECK(rdof_batt_percent(4250) == 100);
    CHECK(rdof_batt_percent(4200) == 100);
    CHECK(rdof_batt_percent(3800) == 50);
    CHECK(rdof_batt_percent(3850) == 57);
    CHECK(rdof_batt_percent(3300) == 0);
    CHECK(rdof_batt_percent(3000) == 0);
    CHECK(rdof_batt_percent(100) == -1); /* no battery */
    for (int mv = 2500; mv <= 4300; mv += 10) /* never goes down as the voltage goes up */
        CHECK(rdof_batt_percent(mv) >= rdof_batt_percent(mv - 10));
    rdof_batt_low_t l = {0};
    for (int i = 0; i < 14; i++)
        CHECK(!rdof_batt_low_step(&l, 3200, 3300, 15));
    CHECK(rdof_batt_low_step(&l, 3200, 3300, 15));
    CHECK(!rdof_batt_low_step(&l, 3400, 3300, 15)); /* back over the cutoff (charging): the count starts again */
    for (int i = 0; i < 20; i++)
        CHECK(!rdof_batt_low_step(&l, 0, 3300, 15)); /* no battery: never low */
}

int main(void)
{
    key_off();
    key_at_boot();
    key_wake();
    battery();
    printf("%s\n", failures ? "power: FAILED" : "power: all passed");
    return failures ? 1 : 0;
}
