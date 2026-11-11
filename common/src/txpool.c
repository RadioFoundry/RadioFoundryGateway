// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The transmitter pool (rdof/txpool.h). */
#include "rdof/txpool.h"

#include <string.h>

void rdof_txpool_init(rdof_txpool_t *p, double idle_s)
{
    memset(p, 0, sizeof *p);
    p->idle_s = idle_s;
}

int rdof_txpool_add(rdof_txpool_t *p, uint64_t freqs, double answer_s)
{
    if (p->n >= RDOF_TXPOOL_MAX)
        return -1;
    p->tx[p->n] = (rdof_txp_tx_t){.freqs = freqs ? freqs : ~0ull, .answer_s = answer_s > 0 ? answer_s : 0};
    return p->n++;
}

/* Brought up to now: a stream gone idle holds its path from its last burst; a hold that's over is free. */
static void age(rdof_txpool_t *p, rdof_txp_tx_t *t, double now)
{
    if (t->state == RDOF_TXP_SENDING && now - t->last > p->idle_s) {
        t->state = RDOF_TXP_HOLDING;
        t->until = t->last + p->idle_s + t->answer_s;
    }
    if (t->state == RDOF_TXP_HOLDING && now >= t->until)
        t->state = RDOF_TXP_FREE;
}

/* r answers the stream t holds for: back to its talker, or the same group. */
static bool answers(const rdof_txp_tx_t *t, const rdof_txp_req_t *r)
{
    return r->dst == t->on.src || (r->group && t->on.group && r->dst == t->on.dst);
}

static rdof_txp_take_t took(rdof_txpool_t *p, int i, const rdof_txp_req_t *r, double now, bool preempt)
{
    rdof_txp_tx_t *t = &p->tx[i];
    rdof_txp_take_t k = {.i = i, .fresh = true, .preempt = preempt, .ended_path = -1};
    if (t->state != RDOF_TXP_FREE && (preempt || t->on.path != r->path)) { /* what it leaves */
        k.ended = t->on.stream;
        k.ended_path = t->on.path;
        if (preempt)
            p->preempted++;
        else
            p->held_taken++;
    }
    t->state = RDOF_TXP_SENDING;
    t->on = *r;
    t->last = now;
    return k;
}

rdof_txp_take_t rdof_txpool_take(rdof_txpool_t *p, const rdof_txp_req_t *r, double now)
{
    const rdof_txp_take_t none = {.i = -1, .ended_path = -1};
    if (!r->stream || r->freq < 0 || r->freq >= 64)
        return none;
    for (int i = 0; i < p->n; i++)
        age(p, &p->tx[i], now);
    /* 1. already sending it */
    for (int i = 0; i < p->n; i++) {
        rdof_txp_tx_t *t = &p->tx[i];
        if (t->state == RDOF_TXP_SENDING && t->on.stream == r->stream && t->on.path == r->path) {
            t->last = now;
            return (rdof_txp_take_t){.i = i, .ended_path = -1};
        }
    }
    int free = -1, other_hold = -1, victim = -1;
    for (int i = 0; i < p->n; i++) {
        const rdof_txp_tx_t *t = &p->tx[i];
        if (!(t->freqs >> r->freq & 1))
            continue;
        /* 2. holding this path for the call this answers */
        if (t->state == RDOF_TXP_HOLDING && t->on.path == r->path && answers(t, r))
            return took(p, i, r, now, false);
        if (t->state == RDOF_TXP_FREE && free < 0)
            free = i;
        else if (t->state == RDOF_TXP_HOLDING && other_hold < 0)
            other_hold = i;
        else if (t->state == RDOF_TXP_SENDING && !t->on.urgent && victim < 0)
            victim = i;
    }
    /* 3. free */
    if (free >= 0)
        return took(p, free, r, now, false);
    /* 4. a hold, for an urgent stream or a confirmed-data answer */
    if (other_hold >= 0 && (r->urgent || r->take_hold))
        return took(p, other_hold, r, now, false);
    /* 5. preempt */
    if (victim >= 0 && r->urgent)
        return took(p, victim, r, now, true);
    return none;
}

rdof_txp_take_t rdof_txpool_peek(const rdof_txpool_t *p, const rdof_txp_req_t *r, double now)
{
    rdof_txpool_t q = *p;
    return rdof_txpool_take(&q, r, now);
}

void rdof_txpool_burst(rdof_txpool_t *p, int i, double now)
{
    if (i >= 0 && i < p->n && p->tx[i].state == RDOF_TXP_SENDING)
        p->tx[i].last = now;
}

void rdof_txpool_end(rdof_txpool_t *p, int i, double now)
{
    if (i < 0 || i >= p->n || p->tx[i].state != RDOF_TXP_SENDING)
        return;
    rdof_txp_tx_t *t = &p->tx[i];
    t->last = now;
    t->state = t->answer_s > 0 ? RDOF_TXP_HOLDING : RDOF_TXP_FREE;
    t->until = now + t->answer_s;
}

void rdof_txpool_release(rdof_txpool_t *p, int i)
{
    if (i >= 0 && i < p->n)
        p->tx[i].state = RDOF_TXP_FREE;
}

rdof_txp_state_t rdof_txpool_state(rdof_txpool_t *p, int i, double now)
{
    if (i < 0 || i >= p->n)
        return RDOF_TXP_FREE;
    age(p, &p->tx[i], now);
    return p->tx[i].state;
}

bool rdof_txpool_reaches(const rdof_txpool_t *p, int freq)
{
    if (freq < 0 || freq >= 64)
        return false;
    for (int i = 0; i < p->n; i++)
        if (p->tx[i].freqs >> freq & 1)
            return true;
    return false;
}

const char *rdof_txpool_state_name(rdof_txp_state_t s)
{
    return s == RDOF_TXP_SENDING ? "sending" : s == RDOF_TXP_HOLDING ? "holding" : "free";
}
