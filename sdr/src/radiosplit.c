// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* A split with both halves radios (rdof/sdr/t3split.h, rdof_t3split_init_radios): the set of rdof/sdr/sdrsplit.h
 * holding splits with no SDR modem. Split i's radio 0 is its TX half and radio 1 its RX half; the RX half hears the
 * inbound itself, so nothing comes from a receiver. */
#include "rdof/sdr/t3split.h"

#if RDOF_FEATURE_REPEATER

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void rdof_radiosplits_init(rdof_sdrsplits_t *set, const rdof_sdrsplits_io_t *io, uint32_t rx_fs)
{
    memset(set, 0, sizeof *set);
    set->io = *io;
    set->rx_fs = rx_fs;
}

/* What each split has for its radios and its host, until nothing moves (to_radio or to_host may call back in). */
static void flush(rdof_sdrsplits_t *set)
{
    if (set->busy) {
        set->again = true;
        return;
    }
    set->busy = true;
    uint8_t buf[1024];
    size_t n;
    bool moved;
    do {
        moved = set->again = false;
        for (int i = 0; i < set->n; i++) {
            rdof_split_t *s = &set->u[i]->s;
            while ((n = rdof_split_take(s, RDOF_SPLIT_TX, buf, sizeof buf)) > 0) {
                set->io.to_radio(set->io.ctx, 2 * i, buf, n);
                moved = true;
            }
            while ((n = rdof_split_take(s, RDOF_SPLIT_RX, buf, sizeof buf)) > 0) {
                set->io.to_radio(set->io.ctx, 2 * i + 1, buf, n);
                moved = true;
            }
            while ((n = rdof_split_take(s, -1, buf, sizeof buf)) > 0) {
                set->io.to_host(set->io.ctx, i, buf, n);
                moved = true;
            }
        }
    } while (moved || set->again);
    set->busy = false;
}

int rdof_radiosplits_add(rdof_sdrsplits_t *set, const rdof_split_params_t *p, double now)
{
    if (set->n >= RDOF_SDRSPLITS_MAX) {
        snprintf(set->error, sizeof set->error, "%d splits at most", RDOF_SDRSPLITS_MAX);
        return -1;
    }
    rdof_sdrsplit_t *u = calloc(1, sizeof *u);
    if (!u) {
        snprintf(set->error, sizeof set->error, "split %d: no memory", set->n + 1);
        return -1;
    }
    u->set = set, u->i = set->n, u->now = now;
    set->u[set->n++] = u;
    rdof_split_init(&u->s, now, p);
    flush(set);
    return u->i;
}

void rdof_radiosplits_free(rdof_sdrsplits_t *set)
{
    for (int i = 0; i < set->n; i++) {
        free(set->u[i]);
        set->u[i] = NULL;
    }
    set->n = 0;
}

void rdof_radiosplits_upstream(rdof_sdrsplits_t *set, int i, const uint8_t *d, size_t n, double now)
{
    if (i < 0 || i >= set->n)
        return;
    set->u[i]->now = now;
    rdof_split_upstream(&set->u[i]->s, d, n, now);
    flush(set);
}

/* r: 2 i the TX half of split i, 2 i + 1 its RX half. */
void rdof_radiosplits_from_radio(rdof_sdrsplits_t *set, int r, const uint8_t *d, size_t n, double now)
{
    int i = r / 2;
    if (r < 0 || i >= set->n)
        return;
    set->u[i]->now = now;
    rdof_split_from_half(&set->u[i]->s, r & 1 ? RDOF_SPLIT_RX : RDOF_SPLIT_TX, d, n, now);
    flush(set);
}

void rdof_radiosplits_heard(rdof_sdrsplits_t *set, int channel, const rdof_sdr_burst_t *b)
{
    (void)set, (void)channel, (void)b; /* no receiver: the RX half hears for itself */
}

void rdof_radiosplits_tick(rdof_sdrsplits_t *set, double now)
{
    for (int i = 0; i < set->n; i++) {
        set->u[i]->now = now;
        rdof_split_tick(&set->u[i]->s, now);
    }
    flush(set);
}

rdof_split_t *rdof_radiosplits_split(rdof_sdrsplits_t *set, int i)
{
    return i >= 0 && i < set->n ? &set->u[i]->s : NULL;
}

#endif /* RDOF_FEATURE_REPEATER */
