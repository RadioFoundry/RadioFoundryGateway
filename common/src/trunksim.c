// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* A trunked site's air, simulated (rdof/trunksim.h). */
#include "rdof/trunksim.h"

#include <string.h>

#include "rdof/duplex.h"
#include "rdof/layer2.h"
#include "rdof/pdu.h"

#if RDOF_FEATURE_REPEATER

void rdof_trunksim_init(rdof_trunksim_t *s, rdof_trunkunit_t *u, int n, rdof_t3ms_t *const *ms, const uint32_t *id)
{
    memset(s, 0, sizeof *s);
    s->u = u;
    s->n = n < RDOF_TRUNKSIM_MS ? n : RDOF_TRUNKSIM_MS;
    for (int i = 0; i < s->n; i++) {
        s->ms[i] = ms[i];
        s->id[i] = id[i];
    }
}

int rdof_trunksim_find(const rdof_trunksim_t *s, uint32_t id)
{
    for (int i = 0; i < s->n; i++)
        if (s->id[i] == id)
            return i;
    return -1;
}

/* What MS i sends, as its burst; false: nothing on air (a lost call's end). */
static bool ms_burst(rdof_trunksim_t *s, int i, const rdof_t3ms_tx_t *tx, int cc, rdof_burst_t *b)
{
    memset(b, 0, sizeof *b);
    b->family = RDOF_FAMILY_MS;
    b->cc = cc;
    b->slot = (tx->ch & 1) + 1;
    if (tx->kind == 1) {
        if (!rdof_trunk_pdu_burst(tx->dt, tx->pdu, cc, RDOF_FAMILY_MS, b->burst))
            return false;
        b->dt = tx->dt;
        return true;
    }
    uint8_t lc9[9];
    switch (tx->what) {
    case RDOF_T3_VOICE_HEADER:
    case RDOF_T3_VOICE_TERM: {
        const int dt = tx->what == RDOF_T3_VOICE_HEADER ? RDOF_DT_VOICE_LC_HEADER : RDOF_DT_TERMINATOR_WITH_LC;
        rdof_group_lc(tx->src, tx->dst, tx->group != 0, 0, lc9);
        rdof_lc_burst(lc9, dt, cc, rdof_data_sync(RDOF_MS_VOICE), b->burst);
        b->dt = dt;
        s->voice_n[i] = 0;
        return true;
    }
    case RDOF_T3_VOICE_FRAME: {
        const int n = s->voice_n[i];
        s->voice_n[i] = (n + 1) % 6;
        rdof_duplex_silence(n, cc, b->burst);
        b->dt = n ? RDOF_DT_VOICE : RDOF_DT_VOICE_SYNC;
        b->n = n;
        return true;
    }
    default:
        return false;
    }
}

void rdof_trunksim_slot(rdof_trunksim_t *s, int64_t k, int64_t now_ms)
{
    rdof_trunkunit_t *u = s->u;
    const int ncar = u->cfg.ncarriers, cc = u->cfg.cc;
    rdof_trunkunit_run(u, k, now_ms);

    /* the outbound of slot k, read back as an MS would */
    int dt[RDOF_T3_CARRIERS];
    uint8_t pdu[RDOF_T3_CARRIERS][10];
    for (int c = 0; c < ncar; c++) {
        uint8_t burst[RDOF_BURST_BYTES], control;
        int at, rcc;
        int32_t slc;
        dt[c] = 0;
        if (s->outside >> c & 1) {
            if (!s->outbound || !s->outbound(s->outbound_ctx, c, k, burst))
                continue;
        } else {
            rdof_trunkunit_slot(u, c, k, burst, &control, &at, &slc);
        }
        const rdof_trunk_pdu_t r = rdof_trunk_burst_pdu(burst, &dt[c], &rcc, pdu[c]);
        if (r == RDOF_TRUNK_PDU_OK)
            s->out_pdus++;
        else {
            s->out_bad += r == RDOF_TRUNK_PDU_BAD;
            dt[c] = 0;
        }
    }

    /* the inbound of slot k: what each MS sends, the site hears one burst a channel */
    rdof_t3ms_tx_t tx[RDOF_TRUNKSIM_MS];
    bool has[RDOF_TRUNKSIM_MS];
    for (int i = 0; i < s->n; i++)
        has[i] = rdof_t3ms_tx(s->ms[i], k, now_ms, &tx[i]);
    for (int ch = (int)(k & 1); ch < 2 * ncar; ch += 2) {
        int first = -1, count = 0;
        for (int i = 0; i < s->n; i++)
            if (has[i] && tx[i].ch == ch && count++ == 0)
                first = i;
        if (first < 0)
            continue;
        rdof_burst_t b;
        if (!ms_burst(s, first, &tx[first], cc, &b))
            continue;
        for (int i = first + 1; i < s->n; i++) /* the others' voice counters move on as if heard */
            if (has[i] && tx[i].ch == ch) {
                rdof_burst_t other;
                ms_burst(s, i, &tx[i], cc, &other);
            }
        if (count > 1) { /* a collision: the strongest is unreadable too (the site counts it) */
            s->collisions++;
            for (int j = 14; j < 20; j++)
                b.burst[j] ^= 0x5A;
        }
        if (tx[first].kind == 1)
            s->in_pdus++;
        else
            s->in_voice++;
        rdof_trunkunit_rx(u, ch / 2, k, &b, -60);
    }

    /* the MSs hear slot k on the channel each is on */
    const int at = rdof_trunkunit_at(u, k);
    for (int i = 0; i < s->n; i++) {
        rdof_t3ms_status_t st;
        rdof_t3ms_status(s->ms[i], &st);
        const int ch = st.ch;
        if ((ch & 1) != (int)(k & 1) || ch / 2 >= ncar)
            continue;
        rdof_t3ms_rx(s->ms[i], ch, k, (uint8_t)dt[ch / 2], pdu[ch / 2], ch == 0 && at > 0);
    }
}

#endif /* RDOF_FEATURE_REPEATER */
