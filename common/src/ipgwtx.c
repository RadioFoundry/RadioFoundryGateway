// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The IP gateway's packets on the air (rdof/ipgwtx.h). */
#include "rdof/ipgwtx.h"

#if RDOF_FEATURE_DATA_BLOCKS && RDOF_FEATURE_TX

#include <string.h>

#include "rdof/txvr.h"
#if RDOF_FEATURE_REPEATER
#include "rdof/repeater.h"
#endif

void rdof_ipgw_net_burst(const rdof_ipgw_dmr_t *p, int i, const uint8_t sid[4], int slot, uint8_t seq,
                         rdof_net_burst_t *b)
{
    memset(b, 0, sizeof *b);
    memcpy(b->stream_id, sid, 4);
    b->control = p->control[i];
    memcpy(b->burst, p->bursts[i], RDOF_BURST_BYTES);
    b->terminator = i == p->nbursts - 1;
    b->seq = seq;
    b->src = p->src;
    b->dst = p->dst;
    b->group = false;
    b->slot = slot;
}

static const rdof_channel_t *channel_at(const rdof_txvr_t *t, uint32_t freq)
{
    if (!freq)
        return NULL;
    for (int i = 0; i < t->nchannels && i < RDOF_TXVR_MAX_CHANNELS; i++) {
        uint32_t f = t->channels[i].freq;
        if ((f > freq ? f - freq : freq - f) <= RDOF_TXVR_FREQ_TOLERANCE_HZ)
            return &t->channels[i];
    }
    return NULL;
}

bool rdof_ipgw_tx_txvr(rdof_ipgw_tx_t *x, struct rdof_txvr *t, const rdof_ipgw_dmr_t *p, const uint8_t sid[4])
{
    if (p->nbursts <= 0)
        return false;
    uint32_t now_freq;
    int now_cc;
    rdof_txvr_channel_now(t, &now_freq, &now_cc);
    const rdof_channel_t *ch = channel_at(t, p->freq);
    if (!ch)
        ch = channel_at(t, now_freq); /* heard where it isn't a channel of ours: where we are */
    if (!ch)
        return false;
    rdof_tune_t tune;
    memset(&tune, 0, sizeof tune);
    memcpy(tune.stream_id, sid, 4);
    tune.freq = ch->freq;
    tune.cc = ch->cc;
    tune.power = 255;
    rdof_txvr_on_tune(t, &tune);
    if (t->lock)
        t->lock(t->ctx);
    bool ours = t->has_call && !memcmp(t->call.tune.stream_id, sid, 4);
    if (t->unlock)
        t->unlock(t->ctx);
    if (!ours)
        return false; /* busy with another call, or not ready */
    for (int i = 0; i < p->nbursts; i++) {
        rdof_net_burst_t b;
        rdof_ipgw_net_burst(p, i, sid, 2, x->seq++, &b);
        rdof_txvr_on_burst(t, &b);
    }
    return true;
}

#if RDOF_FEATURE_REPEATER

static double now_of(const rdof_repeater_t *r) { return r->clock ? r->clock(r->ctx) : 0.0; }

/* The slot the packet goes out on is someone else's now: the repeater would refuse the call. */
static bool slot_busy(const rdof_repeater_t *r, int slot, double now)
{
    if (r->sfr)
        return r->inbound_flag || now - r->inbound_last < RDOF_RPT_SFR_QUIET_S || r->slots[r->outbound].net >= 0;
    return r->slots[slot].rf.active || r->slots[slot].net >= 0;
}

void rdof_ipgw_tx_more(rdof_ipgw_tx_t *x, struct rdof_repeater *r)
{
    if (!x->busy)
        return;
    double now = now_of(r);
    if (r->lock)
        r->lock(r->ctx);
    int room = RDOF_RPT_IN - r->net_count;
    bool wait = x->next == 0 && slot_busy(r, x->slot, now);
    if (r->unlock)
        r->unlock(r->ctx);
    if (wait) {
        if (now - x->since >= RDOF_IPGW_TX_WAIT_S) {
            x->busy = false;
            x->gave_up++;
        }
        return;
    }
    for (; room > 0 && x->next < x->p.nbursts; room--, x->next++) {
        rdof_net_burst_t b;
        rdof_ipgw_net_burst(&x->p, x->next, x->sid, x->slot, x->seq++, &b);
        rdof_repeater_on_burst(r, &b);
    }
    x->busy = x->next < x->p.nbursts;
}

bool rdof_ipgw_tx_repeater(rdof_ipgw_tx_t *x, struct rdof_repeater *r, const rdof_ipgw_dmr_t *p,
                           const uint8_t sid[4])
{
    if (x->busy || p->nbursts <= 0 || p->nbursts > RDOF_IPGW_MAX_BURSTS)
        return false;
    int slot = p->slot == 2 ? 2 : 1;
    if (!r->sfr && !r->enabled[slot])
        return false;
    x->p = *p;
    x->next = 0;
    x->slot = slot;
    memcpy(x->sid, sid, 4);
    x->busy = true;
    x->since = now_of(r);
    rdof_ipgw_tx_more(x, r);
    return true;
}

#else

bool rdof_ipgw_tx_repeater(rdof_ipgw_tx_t *x, struct rdof_repeater *r, const rdof_ipgw_dmr_t *p,
                           const uint8_t sid[4])
{
    return false;
}

void rdof_ipgw_tx_more(rdof_ipgw_tx_t *x, struct rdof_repeater *r) {}

#endif

#endif
