// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The transmit stream's timing through a stall (rdof/sdr/txguard.h). */
#include "rdof/sdr/txguard.h"

#include <math.h>
#include <string.h>

void rdof_txguard_init(rdof_txguard_t *g, double rate, int block, int nbuf)
{
    memset(g, 0, sizeof *g);
    g->rate = rate, g->block = block, g->nbuf = nbuf;
    g->dry_at = NAN;
}

void rdof_txguard_pushed(rdof_txguard_t *g, double t0, double t1)
{
    const double one = (double)g->block / g->rate;
    if (t1 - t0 >= RDOF_TXGUARD_WAITED_S) {
        g->dry_at = t1 + g->nbuf * one; /* a block just played out: the queue is full again */
    } else if (isnan(g->dry_at)) {
        /* the queue filling at the start: not full yet, so no deadline until a push waits */
    } else if (t0 > g->dry_at) {
        g->dry++; /* it had run out: this block starts playing now */
        g->dry_at = t1 + one;
    } else {
        g->dry_at += one;
    }
}

double rdof_txguard_deadline(const rdof_txguard_t *g, double margin_s) { return g->dry_at - margin_s; }

void rdof_txguard_padded(rdof_txguard_t *g, int n)
{
    g->skip += n, g->padded += n;
    g->pads++;
}

int rdof_txguard_drop(rdof_txguard_t *g, int n)
{
    const int k = g->skip < n ? (int)g->skip : n;
    g->skip -= k;
    return k;
}
