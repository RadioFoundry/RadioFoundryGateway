// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* Splits on one receiver (rdof/sdr/sdrsplit.h). */
#include "rdof/sdr/sdrsplit.h"

#if RDOF_FEATURE_REPEATER

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void flush(rdof_sdrsplits_t *set);

/* The RX half's modem answers the split at once, from inside rdof_sdrmodem_write: its frame goes in, and the flush
 * that's running takes what it gives. */
static void m_reply(void *ctx, const uint8_t *f, size_t n)
{
    rdof_sdrsplit_t *u = ctx;
    rdof_split_from_half(&u->s, RDOF_SPLIT_RX, f, n, u->now);
    u->set->again = true;
}

static int m_channel(void *ctx, uint32_t hz)
{
    const rdof_sdrsplit_t *u = ctx;
    return u->set->io.rx_channel(u->set->io.ctx, hz);
}

static int64_t m_now48(void *ctx)
{
    const rdof_sdrsplit_t *u = ctx;
    return u->set->io.now48(u->set->io.ctx);
}

void rdof_sdrsplits_init(rdof_sdrsplits_t *set, const rdof_sdrsplits_io_t *io, uint32_t rx_fs)
{
    memset(set, 0, sizeof *set);
    set->io = *io;
    set->rx_fs = rx_fs;
}

int rdof_sdrsplits_add(rdof_sdrsplits_t *set, const rdof_split_params_t *p, double now)
{
    if (set->n >= RDOF_SDRSPLITS_MAX) {
        snprintf(set->error, sizeof set->error, "%d splits on one receiver at most", RDOF_SDRSPLITS_MAX);
        return -1;
    }
    rdof_sdrsplit_t *u = calloc(1, sizeof *u);
    if (!u) {
        snprintf(set->error, sizeof set->error, "split %d: no memory", set->n + 1);
        return -1;
    }
    u->set = set, u->i = set->n, u->now = now;
    const rdof_sdrmodem_io_t mio = {m_reply, m_channel, NULL, u, set->io.now48 ? m_now48 : NULL};
    /* receive only: the modulator is never used, so the least rate it takes */
    if (rdof_sdrmodem_init(&u->m, &mio, set->rx_fs, 48000, false)) {
        snprintf(set->error, sizeof set->error, "split %d: %s", set->n + 1, u->m.error);
        free(u);
        return -1;
    }
    set->u[set->n++] = u;
    rdof_split_init(&u->s, now, p);
    flush(set);
    return u->i;
}

void rdof_sdrsplits_free(rdof_sdrsplits_t *set)
{
    for (int i = 0; i < set->n; i++) {
        rdof_sdrmodem_free(&set->u[i]->m);
        free(set->u[i]);
        set->u[i] = NULL;
    }
    set->n = 0;
}

/* What each split has for its halves and its repeater, until nothing moves. A write to the RX half's modem answers
 * at once (m_reply), and to_radio or to_host may call back in: that sets again, and the running flush goes round. */
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
            rdof_sdrsplit_t *u = set->u[i];
            while ((n = rdof_split_take(&u->s, RDOF_SPLIT_TX, buf, sizeof buf)) > 0) {
                set->io.to_radio(set->io.ctx, i, buf, n);
                moved = true;
            }
            while ((n = rdof_split_take(&u->s, RDOF_SPLIT_RX, buf, sizeof buf)) > 0) {
                rdof_sdrmodem_write(&u->m, buf, n);
                moved = true;
            }
            while ((n = rdof_split_take(&u->s, -1, buf, sizeof buf)) > 0) {
                set->io.to_host(set->io.ctx, i, buf, n);
                moved = true;
            }
        }
    } while (moved || set->again);
    set->busy = false;
}

void rdof_sdrsplits_upstream(rdof_sdrsplits_t *set, int i, const uint8_t *d, size_t n, double now)
{
    if (i < 0 || i >= set->n)
        return;
    set->u[i]->now = now;
    rdof_split_upstream(&set->u[i]->s, d, n, now);
    flush(set);
}

void rdof_sdrsplits_from_radio(rdof_sdrsplits_t *set, int i, const uint8_t *d, size_t n, double now)
{
    if (i < 0 || i >= set->n)
        return;
    set->u[i]->now = now;
    rdof_split_from_half(&set->u[i]->s, RDOF_SPLIT_TX, d, n, now);
    flush(set);
}

void rdof_sdrsplits_heard(rdof_sdrsplits_t *set, int channel, const rdof_sdr_burst_t *b)
{
    for (int i = 0; i < set->n; i++)
        rdof_sdrmodem_heard(&set->u[i]->m, channel, b);
    flush(set);
}

void rdof_sdrsplits_tick(rdof_sdrsplits_t *set, double now)
{
    for (int i = 0; i < set->n; i++) {
        set->u[i]->now = now;
        rdof_split_tick(&set->u[i]->s, now);
    }
    flush(set);
}

rdof_split_t *rdof_sdrsplits_split(rdof_sdrsplits_t *set, int i)
{
    return i >= 0 && i < set->n ? &set->u[i]->s : NULL;
}

rdof_sdrmodem_t *rdof_sdrsplits_modem(rdof_sdrsplits_t *set, int i)
{
    return i >= 0 && i < set->n ? &set->u[i]->m : NULL;
}

bool rdof_sdrsplits_ready(const rdof_sdrsplits_t *set)
{
    for (int i = 0; i < set->n; i++)
        if (!set->u[i]->s.ready)
            return false;
    return set->n > 0;
}

int rdof_sdrsplits_failed(const rdof_sdrsplits_t *set)
{
    for (int i = 0; i < set->n; i++)
        if (set->u[i]->s.failed[0])
            return i;
    return -1;
}

#endif /* RDOF_FEATURE_REPEATER */
