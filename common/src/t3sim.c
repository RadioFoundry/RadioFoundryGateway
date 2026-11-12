// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* A trunked site's air, simulated (rdof/t3sim.h). */
#include "rdof/t3sim.h"

#include <string.h>

#include "rdof/duplex.h"
#include "rdof/layer2.h"
#include "rdof/pdu.h"

#if RDOF_FEATURE_REPEATER

void rdof_t3sim_init(rdof_t3sim_t *s, rdof_t3unit_t *u, int n, rdof_t3ms_t *const *ms, const uint32_t *id)
{
    memset(s, 0, sizeof *s);
    s->u = u;
    s->n = n < RDOF_T3SIM_MS ? n : RDOF_T3SIM_MS;
    for (int c = 0; c < RDOF_T3_CARRIERS; c++)
        s->slc[c] = -1;
    for (int i = 0; i < s->n; i++) {
        s->ms[i] = ms[i];
        s->id[i] = id[i];
    }
}

int rdof_t3sim_find(const rdof_t3sim_t *s, uint32_t id)
{
    for (int i = 0; i < s->n; i++)
        if (s->id[i] == id)
            return i;
    return -1;
}

bool rdof_t3sim_ms_burst(const rdof_t3ms_tx_t *tx, int n, int cc, rdof_burst_t *b)
{
    memset(b, 0, sizeof *b);
    b->family = RDOF_FAMILY_MS;
    b->cc = cc;
    b->slot = (tx->ch & 1) + 1;
    if (tx->kind == 1) {
        if (!rdof_t3rf_pdu_burst(tx->dt, tx->pdu, cc, RDOF_FAMILY_MS, b->burst))
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
        return true;
    }
    case RDOF_T3_VOICE_FRAME:
        n = (n % 6 + 6) % 6;
        rdof_duplex_silence(n, cc, b->burst);
        b->dt = n ? RDOF_DT_VOICE : RDOF_DT_VOICE_SYNC;
        b->n = n;
        return true;
    default:
        return false;
    }
}

/* What MS i sends, as its burst, its voice counter kept. */
static bool ms_burst(rdof_t3sim_t *s, int i, const rdof_t3ms_tx_t *tx, int cc, rdof_burst_t *b)
{
    const int n = s->voice_n[i];
    if (tx->kind != 1 && tx->what == RDOF_T3_VOICE_FRAME)
        s->voice_n[i] = (n + 1) % 6;
    else if (tx->kind != 1)
        s->voice_n[i] = 0;
    return rdof_t3sim_ms_burst(tx, n, cc, b);
}

static bool is_voice(int dt) { return dt == RDOF_DT_VOICE || dt == RDOF_DT_VOICE_SYNC; }

void rdof_t3sim_slot(rdof_t3sim_t *s, int64_t k, int64_t now_ms)
{
    rdof_t3unit_t *u = s->u;
    const int ncar = rdof_t3unit_cfg(u)->ncarriers, cc = rdof_t3unit_cfg(u)->cc;
    rdof_t3unit_run(u, k, now_ms);

    /* the outbound of slot k, read back as an MS would */
    int dt[RDOF_T3_CARRIERS];
    uint8_t pdu[RDOF_T3_CARRIERS][10];
    rdof_t3sim_out_t out[RDOF_T3_CARRIERS];
    for (int c = 0; c < ncar; c++) {
        rdof_t3sim_out_t *o = &out[c];
        uint8_t control;
        int at = -1, rdt = -1, rcc = -1;
        int32_t slc = -1;
        memset(o, 0, sizeof *o);
        o->dt = o->cc = -1;
        dt[c] = 0;
        if (s->outside >> c & 1)
            o->aired = s->outbound && s->outbound(s->outbound_ctx, c, k, o->burst);
        else
            o->aired = rdof_t3unit_slot(u, c, k, o->burst, &control, &at, &slc);
        /* the CACH before slot k: the short LC changes on a 4-slot cycle's first */
        if (k % 4 == 0 && slc >= 0)
            s->slc[c] = slc;
        int at4[4] = {0, 0, 0, 0};
        uint8_t c4[4][3];
        o->at = at4[k & 3] = at > 0;
        rdof_t3rf_cach4(s->slc[c] >= 0 ? (uint32_t)s->slc[c] : 0, at4, k & ~(int64_t)3, c4);
        memcpy(o->cach, c4[k & 3], 3);
        if (!o->aired)
            continue;
        const rdof_trunk_pdu_t r = rdof_t3rf_burst_pdu(o->burst, &rdt, &rcc, o->pdu);
        o->dt = rdt;
        o->cc = rcc;
        if (r == RDOF_T3RF_PDU_OK) {
            s->out_pdus++;
            o->pdu_ok = true;
            dt[c] = rdt;
            memcpy(pdu[c], o->pdu, 10);
        } else {
            s->out_bad += r == RDOF_T3RF_PDU_BAD;
        }
    }

    /* the inbound of slot k: what each MS sends, the site hears one burst a channel */
    rdof_t3ms_tx_t tx[RDOF_T3SIM_MS];
    bool has[RDOF_T3SIM_MS];
    for (int i = 0; i < s->n; i++)
        has[i] = rdof_t3ms_tx(s->ms[i], k, now_ms, &tx[i]);
    rdof_t3sim_in_t ext[RDOF_T3SIM_EXT];
    const int next = s->ext_in ? s->ext_in(s->ext_ctx, k, ext, RDOF_T3SIM_EXT) : 0;
    for (int e = 0; e < next; e++) {
        s->ext_in_n++;
        ext[e].fate = RDOF_T3SIM_HEARD;
        if (ext[e].ch < 0 || (ext[e].ch & 1) != (int)(k & 1) || ext[e].ch / 2 >= ncar) {
            ext[e].ch = -1;
            ext[e].fate = RDOF_T3SIM_DROPPED;
            s->ext_drop++;
        }
    }
    for (int ch = (int)(k & 1); ch < 2 * ncar; ch += 2) {
        int first = -1, count = 0;
        for (int i = 0; i < s->n; i++)
            if (has[i] && tx[i].ch == ch && count++ == 0)
                first = i;
        for (int e = 0; e < next; e++)
            count += ext[e].ch == ch;
        if (count == 0)
            continue;
        rdof_burst_t b;
        bool ok = false;
        if (first >= 0) {
            ok = ms_burst(s, first, &tx[first], cc, &b);
            for (int i = first + 1; i < s->n; i++) /* the others' voice counters move on as if heard */
                if (has[i] && tx[i].ch == ch) {
                    rdof_burst_t other;
                    ms_burst(s, i, &tx[i], cc, &other);
                }
        }
        if (!ok) {
            int efirst = -1;
            for (int e = 0; e < next && efirst < 0; e++)
                if (ext[e].ch == ch)
                    efirst = e;
            if (efirst < 0)
                continue;
            b = ext[efirst].b;
            ok = true;
        }
        b.slot = (ch & 1) + 1;
        if (count > 1) { /* a collision: the strongest is unreadable too (the site counts it) */
            s->collisions++;
            for (int e = 0; e < next; e++)
                if (ext[e].ch == ch)
                    ext[e].fate = RDOF_T3SIM_COLLIDED;
            for (int j = 14; j < 20; j++)
                b.burst[j] ^= 0x5A;
        }
        if (is_voice(b.dt) || b.dt == RDOF_DT_VOICE_LC_HEADER || b.dt == RDOF_DT_TERMINATOR_WITH_LC)
            s->in_voice++;
        else
            s->in_pdus++;
        rdof_t3unit_rx(u, ch / 2, k, &b, -60);
    }

    /* the MSs hear slot k on the channel each is on */
    const int at = rdof_t3unit_at(u, k);
    for (int i = 0; i < s->n; i++) {
        rdof_t3ms_status_t st;
        rdof_t3ms_status(s->ms[i], &st);
        const int ch = st.ch;
        if ((ch & 1) != (int)(k & 1) || ch / 2 >= ncar)
            continue;
        rdof_t3ms_rx(s->ms[i], ch, k, (uint8_t)dt[ch / 2], pdu[ch / 2], ch == 0 && at > 0);
    }
    if (s->ext_out)
        s->ext_out(s->ext_ctx, k, out, ncar, at, ext, next);
}

#endif /* RDOF_FEATURE_REPEATER */
