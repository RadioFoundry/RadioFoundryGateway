// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * A trunked site's radio layer (rdof/trunkrf.h; docs/spec/fw/trunk.md section 9). Written from ETSI TS 102 361-1
 * (bursts, CACH, short LC, CRC masks) and -4 (the TSCC and payload channels' use of them).
 */
#include "rdof/trunkrf.h"

#if RDOF_FEATURE_REPEATER

#include <string.h>

#include "radiofoundry/dmrCach.h"
#include "rdof/bits.h"
#include "rdof/mmdvm.h"
#include "rdof/pdu.h"
#include "rdof/split.h"

/* ── control blocks ───────────────────────────────────────────────────────── */

int rdof_trunk_mask(int dt)
{
    switch (dt) {
    case RDOF_DT_CSBK: return RDOF_MASK_CSBK;
    case RDOF_DT_MBC_HEADER:
    case RDOF_DT_MBC_CONTINUATION: return RDOF_MASK_MBC_HEADER; /* an MBC's last block takes the header's mask */
    case RDOF_DT_DATA_HEADER: return RDOF_MASK_DATA_HEADER;
    default: return -1;
    }
}

bool rdof_trunk_pdu_burst(int dt, const uint8_t pdu[10], int cc, rdof_family_t family, uint8_t out[RDOF_BURST_BYTES])
{
    const int mask = rdof_trunk_mask(dt);
    if (mask < 0)
        return false;
    uint8_t b12[12];
    memcpy(b12, pdu, 10);
    const uint16_t crc = (uint16_t)(rdof_crc16_ccitt(b12, 10) ^ (uint16_t)mask);
    b12[10] = (uint8_t)(crc >> 8);
    b12[11] = (uint8_t)crc;
    rdof_data_burst(b12, dt, cc & 0x0F, rdof_data_sync(rdof_family_voice_sync(family)), out);
    return true;
}

rdof_trunk_pdu_t rdof_trunk_burst_pdu(const uint8_t burst[RDOF_BURST_BYTES], int *dt, int *cc, uint8_t pdu[10])
{
    *dt = *cc = -1;
    rdof_family_t fam;
    bool data;
    int e;
    if (!rdof_detect_sync(burst, 4, &fam, &data, &e) || !data)
        return RDOF_TRUNK_NOT_PDU;
    if (!rdof_decode_slot_type(burst, cc, dt, &e)) {
        *dt = *cc = -1;
        return RDOF_TRUNK_PDU_BAD; /* a data sync with an unreadable slot type: two radios at once, most likely */
    }
    const int mask = rdof_trunk_mask(*dt);
    if (mask < 0)
        return RDOF_TRUNK_NOT_PDU;
    uint8_t b12[12];
    if (!rdof_bptc196_decode(burst, b12) || !rdof_check_crc16(b12, (uint16_t)mask))
        return RDOF_TRUNK_PDU_BAD;
    memcpy(pdu, b12, 10);
    return RDOF_TRUNK_PDU_OK;
}

static void pack24(uint32_t bits, uint8_t out[3])
{
    out[0] = (uint8_t)(bits >> 16);
    out[1] = (uint8_t)(bits >> 8);
    out[2] = (uint8_t)bits;
}

void rdof_trunk_cach4(uint32_t slc28, const int at[4], int64_t k, uint8_t out[4][3])
{
    static const uint8_t lcss[4] = {DMR_LCSS_FIRST, DMR_LCSS_CONT, DMR_LCSS_CONT, DMR_LCSS_LAST};
    uint32_t rows[4];
    dmrShortLcEncode(slc28 & 0x0FFFFFFFu, rows);
    for (int q = 0; q < 4; q++)
        pack24(dmrCachBuild((uint8_t)(at[q] & 1), (uint8_t)((k + q) & 1), lcss[q], rows[q]), out[q]);
}

bool rdof_trunk_cach_read(const uint8_t c3[3], int *at, int *slot, int *lcss, uint32_t *payload17)
{
    uint8_t tact, a, tc, ls;
    uint32_t payload;
    dmrCachDeinterleave((uint32_t)c3[0] << 16 | (uint32_t)c3[1] << 8 | c3[2], &tact, &payload);
    dmrCachTactDecode(tact, &a, &tc, &ls); /* Hamming(7,4) always decodes */
    *at = a;
    *slot = tc ? 2 : 1;
    *lcss = ls;
    *payload17 = payload;
    return true;
}

bool rdof_trunk_slc_join(const uint32_t rows[4], uint32_t *slc28) { return dmrShortLcDecode(rows, slc28); }

/* ── payload channels ─────────────────────────────────────────────────────── */

static bool is_payload(const rdof_trunkrf_t *t, int ch)
{
    return ch >= 0 && ch < 2 * t->ncarriers && !(t->tscc && ch == 0);
}

rdof_trunkrf_ch_t *rdof_trunkrf_channel(rdof_trunkrf_t *t, int ch) { return is_payload(t, ch) ? &t->ch[ch] : NULL; }

bool rdof_trunkrf_init(rdof_trunkrf_t *t, const rdof_trunkrf_io_t *io, int ncarriers, int cc, bool tscc)
{
    memset(t, 0, sizeof *t);
    if (ncarriers < 1 || ncarriers > RDOF_TRUNKRF_CARRIERS)
        return false;
    if (io)
        t->io = *io;
    t->ncarriers = ncarriers;
    t->cc = cc & 0x0F;
    t->tscc = tscc;
    for (int i = 0; i < RDOF_TRUNKRF_CHANNELS; i++) {
        t->ch[i].index = i;
        t->ch[i].cc = t->cc;
    }
    return true;
}

static void flush(rdof_trunkrf_ch_t *c)
{
    c->qh = c->qn = 0;
    c->net = c->net_started = c->net_regen_on = false;
    c->net_held = 0;
}

static void push(rdof_trunkrf_ch_t *c, uint8_t control, bool net, const uint8_t b[RDOF_BURST_BYTES])
{
    if (c->qn == RDOF_TRUNKRF_QUEUE) {
        c->overflows++;
        return;
    }
    rdof_trunkrf_burst_t *q = &c->q[(c->qh + c->qn) % RDOF_TRUNKRF_QUEUE];
    q->control = control;
    q->net = net;
    memcpy(q->b, b, RDOF_BURST_BYTES);
    c->qn++;
}

bool rdof_trunkrf_open(rdof_trunkrf_t *t, int ch, uint32_t src, uint32_t dst, bool group, int warmup)
{
    rdof_trunkrf_ch_t *c = rdof_trunkrf_channel(t, ch);
    if (!c)
        return false;
    if (!c->open) {
        flush(c);
        c->rf = c->rf_regen_on = false;
        c->ctl_left = 0;
        c->close_after = false;
        c->warmup = warmup < 0 ? 0 : warmup;
    }
    c->open = true;
    c->src = src;
    c->dst = dst;
    c->group = group;
    return true;
}

void rdof_trunkrf_close(rdof_trunkrf_t *t, int ch)
{
    rdof_trunkrf_ch_t *c = rdof_trunkrf_channel(t, ch);
    if (!c)
        return;
    flush(c);
    c->open = c->rf = c->rf_regen_on = false;
    c->ctl_left = c->warmup = 0;
    c->close_after = false;
}

bool rdof_trunkrf_control(rdof_trunkrf_t *t, int ch, int dt, const uint8_t pdu[10], int repeats, bool close_after)
{
    rdof_trunkrf_ch_t *c = rdof_trunkrf_channel(t, ch);
    if (!c || repeats < 1 || !rdof_trunk_pdu_burst(dt, pdu, c->cc, RDOF_FAMILY_BS, c->ctl))
        return false;
    c->ctl_left = repeats;
    c->close_after = close_after;
    if (close_after) /* the channel is being cleared: nothing after the PDU */
        flush(c);
    return true;
}

static void lc_of(uint32_t src, uint32_t dst, bool group, uint8_t lc9[9]) { rdof_group_lc(src, dst, group, 0, lc9); }

static void up(rdof_trunkrf_t *t, rdof_trunkrf_ch_t *c, int dt, int n, const uint8_t *b, bool new_stream)
{
    if (!t->io.uplink)
        return;
    rdof_frame_t f;
    memset(&f, 0, sizeof f);
    f.slot = (c->index & 1) + 1;
    f.dt = dt;
    f.n = n;
    memcpy(f.burst, b, RDOF_BURST_BYTES);
    f.has_addr = true;
    f.src = c->rf_src;
    f.dst = c->rf_dst;
    f.group = c->rf_group;
    f.new_stream = new_stream;
    t->io.uplink(t->io.ctx, c->index, &f);
}

static void activity(rdof_trunkrf_t *t, rdof_trunkrf_ch_t *c, int64_t k, int what)
{
    if (t->io.activity)
        t->io.activity(t->io.ctx, c->index, k, what, c->rf_src, c->rf_dst, c->rf_group);
}

/* RF beats the network (trunk.md 3.6): a network stream on the channel is dropped. */
static void rf_takes(rdof_trunkrf_ch_t *c)
{
    if (c->net) {
        c->net_dropped++;
        flush(c);
    }
}

/* An inbound burst on payload channel c, repeated (regenerated) on its outbound when it is open. */
static void payload_rx(rdof_trunkrf_t *t, rdof_trunkrf_ch_t *c, int64_t k, const rdof_burst_t *b, int rssi)
{
    c->rx_bursts++;
    uint8_t out[RDOF_BURST_BYTES], pdu[10];
    int dt, cc;
    if (b->dt != RDOF_DT_VOICE && b->dt != RDOF_DT_VOICE_SYNC && rdof_trunk_mask(b->dt) >= 0) {
        if (rdof_trunk_burst_pdu(b->burst, &dt, &cc, pdu) == RDOF_TRUNK_PDU_OK) {
            c->rx_pdus++;
            if (t->io.pdu)
                t->io.pdu(t->io.ctx, c->index, k, dt, pdu, rssi);
        }
        return; /* the controller answers control blocks itself: nothing is repeated */
    }
    if (!c->open)
        return;
    if (b->dt == RDOF_DT_VOICE_LC_HEADER || b->dt == RDOF_DT_TERMINATOR_WITH_LC) {
        rdof_full_lc_t lc;
        if (!rdof_decode_voice_lc(b->burst, b->dt, &lc))
            return;
        const bool header = b->dt == RDOF_DT_VOICE_LC_HEADER;
        if (header) {
            rf_takes(c);
            const bool fresh = !c->rf;
            c->rf = true;
            c->rf_src = lc.src_id >= 0 ? (uint32_t)lc.src_id : c->src;
            c->rf_dst = lc.dst_id >= 0 ? (uint32_t)lc.dst_id : c->dst;
            c->rf_group = lc.src_id >= 0 ? lc.group : c->group;
            rdof_voice_regen_init(&c->rf_regen, lc.raw, c->cc);
            c->rf_regen_on = true;
            c->rf_last = k;
            rdof_duplex_lc_burst(lc.raw, RDOF_DT_VOICE_LC_HEADER, c->cc, out);
            push(c, RDOF_SYNC_DATA | RDOF_DT_VOICE_LC_HEADER, false, out);
            if (fresh)
                activity(t, c, k, RDOF_TRUNKRF_HEADER);
            up(t, c, b->dt, 0, out, fresh);
            return;
        }
        if (!c->rf)
            return; /* a terminator of nothing heard */
        rdof_duplex_lc_burst(lc.raw, RDOF_DT_TERMINATOR_WITH_LC, c->cc, out);
        push(c, RDOF_SYNC_DATA | RDOF_DT_TERMINATOR_WITH_LC, false, out);
        up(t, c, b->dt, 0, out, false);
        activity(t, c, k, RDOF_TRUNKRF_TERM);
        c->rf = c->rf_regen_on = false;
        return;
    }
    if (b->dt != RDOF_DT_VOICE && b->dt != RDOF_DT_VOICE_SYNC)
        return; /* data blocks: not carried (packet data is later work) */
    bool late = false;
    if (!c->rf_regen_on) { /* late entry: the channel's call */
        rf_takes(c);
        uint8_t lc9[9];
        c->rf_src = c->src;
        c->rf_dst = c->dst;
        c->rf_group = c->group;
        lc_of(c->src, c->dst, c->group, lc9);
        rdof_voice_regen_init(&c->rf_regen, lc9, c->cc);
        c->rf_regen_on = c->rf = late = true;
    }
    c->rf_last = k;
    const uint8_t control = b->dt == RDOF_DT_VOICE_SYNC ? RDOF_SYNC_AUDIO : (uint8_t)(b->n & 0x0F);
    rdof_voice_regen_burst(&c->rf_regen, control, b->burst, out);
    push(c, control, false, out);
    if (late)
        activity(t, c, k, RDOF_TRUNKRF_VOICE);
    up(t, c, b->dt, b->n, out, late);
}

void rdof_trunkrf_rx(rdof_trunkrf_t *t, int c, int64_t k, const rdof_burst_t *b, int rssi)
{
    if (c < 0 || c >= t->ncarriers || !b) {
        t->rx_ignored++;
        return;
    }
    const int ch = 2 * c + (int)(k & 1);
    if (t->tscc && ch == 0) {
        uint8_t pdu[10];
        int dt, cc;
        switch (rdof_trunk_burst_pdu(b->burst, &dt, &cc, pdu)) {
        case RDOF_TRUNK_PDU_OK:
            if (cc != t->cc) { /* another site's, or a stray: not ours */
                t->rx_ignored++;
                return;
            }
            t->rx_tscc++;
            if (t->io.pdu)
                t->io.pdu(t->io.ctx, 0, k, dt, pdu, rssi);
            return;
        case RDOF_TRUNK_PDU_BAD:
            t->rx_tscc_bad++;
            if (t->io.bad)
                t->io.bad(t->io.ctx, 0, k);
            return;
        default: t->rx_ignored++; return; /* voice on the TSCC: nothing to do with it */
        }
    }
    if (b->cc >= 0 && b->cc != t->cc) { /* the payload channels use the TSCC's color code (trunk.md 3.1) */
        t->rx_ignored++;
        return;
    }
    payload_rx(t, &t->ch[ch], k, b, rssi);
}

bool rdof_trunkrf_net(rdof_trunkrf_t *t, int ch, const rdof_frame_t *f)
{
    rdof_trunkrf_ch_t *c = rdof_trunkrf_channel(t, ch);
    if (!c || !c->open || c->rf || c->close_after) {
        if (c)
            c->net_dropped++;
        return false;
    }
    uint8_t out[RDOF_BURST_BYTES];
    uint8_t control;
    if (f->dt == RDOF_DT_VOICE_LC_HEADER || f->dt == RDOF_DT_TERMINATOR_WITH_LC) {
        rdof_full_lc_t lc;
        uint8_t lc9[9];
        if (rdof_decode_voice_lc(f->burst, f->dt, &lc))
            memcpy(lc9, lc.raw, 9);
        else if (f->has_addr)
            lc_of(f->src, f->dst, f->group, lc9);
        else
            lc_of(c->src, c->dst, c->group, lc9);
        if (f->dt == RDOF_DT_VOICE_LC_HEADER) {
            if (!c->net) {
                c->net = true;
                c->net_started = false;
                c->net_held = 0;
            }
            rdof_voice_regen_init(&c->net_regen, lc9, c->cc);
            c->net_regen_on = true;
        } else if (!c->net) {
            c->net_dropped++;
            return false;
        }
        rdof_duplex_lc_burst(lc9, f->dt, c->cc, out);
        control = (uint8_t)(RDOF_SYNC_DATA | f->dt);
    } else if (f->dt == RDOF_DT_VOICE || f->dt == RDOF_DT_VOICE_SYNC) {
        if (!c->net_regen_on) { /* joined late: the addresses it gives, else the channel's */
            uint8_t lc9[9];
            if (f->has_addr)
                lc_of(f->src, f->dst, f->group, lc9);
            else
                lc_of(c->src, c->dst, c->group, lc9);
            rdof_voice_regen_init(&c->net_regen, lc9, c->cc);
            c->net_regen_on = c->net = true;
            c->net_started = false;
            c->net_held = 0;
        }
        control = f->dt == RDOF_DT_VOICE_SYNC ? RDOF_SYNC_AUDIO : (uint8_t)(f->n & 0x0F);
        rdof_voice_regen_burst(&c->net_regen, control, f->burst, out);
    } else {
        c->net_dropped++; /* data and control blocks from the network: not carried here */
        return false;
    }
    if (c->qn == RDOF_TRUNKRF_QUEUE) {
        c->overflows++;
        return false;
    }
    push(c, control, true, out);
    c->net_bursts++;
    c->net_held++;
    if (f->dt == RDOF_DT_TERMINATOR_WITH_LC || c->net_held >= RDOF_TRUNKRF_NET_PREBUFFER)
        c->net_started = true;
    if (f->dt == RDOF_DT_TERMINATOR_WITH_LC)
        c->net = c->net_regen_on = false; /* what's queued plays out */
    return true;
}

void rdof_trunkrf_net_end(rdof_trunkrf_t *t, int ch)
{
    rdof_trunkrf_ch_t *c = rdof_trunkrf_channel(t, ch);
    if (!c || !c->net)
        return;
    c->net_started = true;
    c->net = c->net_regen_on = false;
}

void rdof_trunkrf_tick(rdof_trunkrf_t *t, int64_t k)
{
    for (int i = 0; i < 2 * t->ncarriers; i++) {
        rdof_trunkrf_ch_t *c = rdof_trunkrf_channel(t, i);
        if (c && c->rf && k - c->rf_last > RDOF_TRUNKRF_LOST_K) {
            c->rf = c->rf_regen_on = false;
            c->lost++;
            activity(t, c, k, RDOF_TRUNKRF_LOST);
        }
    }
}

rdof_trunkrf_out_t rdof_trunkrf_ch_next(rdof_trunkrf_t *t, int ch, int64_t k, uint8_t *control,
                                      uint8_t out[RDOF_BURST_BYTES])
{
    rdof_trunkrf_ch_t *c = rdof_trunkrf_channel(t, ch);
    *control = RDOF_SYNC_DATA | RDOF_DT_IDLE;
    if (c && c->ctl_left > 0) {
        memcpy(out, c->ctl, RDOF_BURST_BYTES);
        int cc, dt, e;
        *control = (uint8_t)(RDOF_SYNC_DATA | (rdof_decode_slot_type(out, &cc, &dt, &e) ? dt : RDOF_DT_CSBK));
        c->tx_control++;
        if (--c->ctl_left == 0 && c->close_after)
            rdof_trunkrf_close(t, ch);
        return RDOF_TRUNKRF_CONTROL;
    }
    if (c && c->open && c->warmup > 0) {
        c->warmup--;
    } else if (c && c->qn && (!c->q[c->qh].net || c->net_started)) {
        const rdof_trunkrf_burst_t *q = &c->q[c->qh];
        memcpy(out, q->b, RDOF_BURST_BYTES);
        *control = q->control;
        c->qh = (c->qh + 1) % RDOF_TRUNKRF_QUEUE;
        c->qn--;
        if (!c->qn && !c->net)
            c->net_started = false;
        c->tx_traffic++;
        return RDOF_TRUNKRF_TRAFFIC;
    } else if (c && c->net_started && !c->qn) {
        c->net_started = c->net; /* an underrun mid stream: it carries on as bursts come */
    }
    if (c)
        c->tx_idle++;
    rdof_duplex_idle(t->cc, out);
    (void)k;
    return RDOF_TRUNKRF_IDLE;
}

bool rdof_trunkrf_slot(rdof_trunkrf_t *t, int c, int64_t k, uint8_t burst[RDOF_BURST_BYTES], uint8_t *control, int *at,
                     int32_t *slc28)
{
    uint8_t ctl = RDOF_SYNC_DATA | RDOF_DT_IDLE;
    *at = -1;
    *slc28 = -1;
    if (t->io.cach)
        t->io.cach(t->io.ctx, c, k, at, slc28);
    if (k % 4 != 0)
        *slc28 = -1;
    if (t->tscc && c == 0 && (k & 1) == 0) {
        int dt = -1;
        uint8_t pdu[10];
        if (t->io.tscc && t->io.tscc(t->io.ctx, k, &dt, pdu) &&
            rdof_trunk_pdu_burst(dt, pdu, t->cc, RDOF_FAMILY_BS, burst)) {
            ctl = (uint8_t)(RDOF_SYNC_DATA | dt);
            t->tscc_sent++;
        } else {
            rdof_duplex_idle(t->cc, burst);
            t->tscc_idle++;
        }
    } else if (c >= 0 && c < t->ncarriers) {
        rdof_trunkrf_ch_next(t, 2 * c + (int)(k & 1), k, &ctl, burst);
    } else {
        rdof_duplex_idle(t->cc, burst);
    }
    if (control)
        *control = ctl;
    return true;
}

bool rdof_trunkrf_carrier_busy(const rdof_trunkrf_t *t, int c)
{
    if (c < 0 || c >= t->ncarriers)
        return false;
    for (int s = 0; s < 2; s++) {
        const int ch = 2 * c + s;
        if (t->tscc && ch == 0)
            return true; /* a TSCC's carrier is always on */
        const rdof_trunkrf_ch_t *x = &t->ch[ch];
        if (x->open || x->ctl_left || x->qn)
            return true;
    }
    return false;
}

/* ── a payload carrier on a split TX half ─────────────────────────────────── */

void rdof_trunkrf_split_init(rdof_trunkrf_split_t *o, int carrier)
{
    memset(o, 0, sizeof *o);
    o->carrier = carrier;
    o->slc_sent = -1;
}

static void upstream(rdof_split_t *s, uint8_t cmd, const uint8_t *p, size_t n, double now)
{
    uint8_t f[RDOF_MMDVM_MAX_FRAME];
    const size_t len = rdof_mmdvm_frame(cmd, p, n, f, sizeof f);
    if (len)
        rdof_split_upstream(s, f, len, now);
}

void rdof_trunkrf_split_pump(rdof_trunkrf_t *t, rdof_trunkrf_split_t *o, struct rdof_split *s, int64_t k_now, int ahead,
                           double now)
{
    const bool busy = rdof_trunkrf_carrier_busy(t, o->carrier);
    if (!s->ready) {
        o->keyed = false;
        return;
    }
    if (!busy) {
        if (o->keyed) {
            const uint8_t off = 0;
            upstream(s, RDOF_MMDVM_DMR_START, &off, 1, now);
            o->keyed = false;
        }
        o->next_k = k_now + 1;
        return;
    }
    const bool sched = rdof_split_sched_ok(s);
    if (ahead <= 0)
        ahead = sched ? 4 : 1;
    if (!o->keyed) {
        const uint8_t on = 1;
        upstream(s, RDOF_MMDVM_DMR_START, &on, 1, now);
        o->keyed = true;
        o->keyups++;
        if (o->next_k < k_now + 1)
            o->next_k = k_now + 1;
    } else if (o->next_k < k_now + 1) {
        o->next_k = k_now + 1; /* fell behind: those slots are gone */
    }
    o->scheduled = sched;
    for (; o->next_k <= k_now + ahead; o->next_k++) {
        const int64_t k = o->next_k;
        uint8_t burst[RDOF_BURST_BYTES], control;
        int at;
        int32_t slc;
        rdof_trunkrf_slot(t, o->carrier, k, burst, &control, &at, &slc);
        if (slc >= 0 && slc != o->slc_sent) {
            uint8_t lc5[5] = {(uint8_t)(slc >> 24 & 0x0F), (uint8_t)(slc >> 16), (uint8_t)(slc >> 8), (uint8_t)slc, 0};
            uint8_t sl9[9];
            rdof_short_lc(lc5, sl9);
            upstream(s, RDOF_MMDVM_DMR_SHORTLC, sl9, 9, now);
            o->slc_sent = slc;
            o->short_lcs++;
        }
        if (sched) {
            if ((control & 0x0F) == RDOF_DT_IDLE && (control & RDOF_SYNC_DATA))
                continue; /* the half sends idle between scheduled bursts */
            if (rdof_split_sched(s, k, control, burst))
                o->sched_bursts++;
        } else {
            uint8_t p[1 + RDOF_BURST_BYTES];
            p[0] = control;
            memcpy(p + 1, burst, RDOF_BURST_BYTES);
            upstream(s, (k & 1) ? RDOF_MMDVM_DMR_DATA2 : RDOF_MMDVM_DMR_DATA1, p, sizeof p, now);
            o->fifo_bursts++;
        }
    }
}

#endif /* RDOF_FEATURE_REPEATER */
