// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The repeater (rdof/repeater.h, docs/spec/repeater.md); the scenario tests check it. */
#include "rdof/repeater.h"

#if RDOF_FEATURE_REPEATER

#include <math.h>
#include <string.h>

#include "rdof/bits.h"
#include "rdof/layer2.h"
#include "rdof/pdu.h"

#define BS_DWN_ACT 0x38
#define HEADER (RDOF_SYNC_DATA | RDOF_DT_VOICE_LC_HEADER)
#define TERMINATOR (RDOF_SYNC_DATA | RDOF_DT_TERMINATOR_WITH_LC)

static double now_of(rdof_repeater_t *r)
{
    return r->clock ? r->clock(r->ctx) : 0.0;
}

static void lock(rdof_repeater_t *r)
{
    if (r->lock)
        r->lock(r->ctx);
}

static void unlock(rdof_repeater_t *r)
{
    if (r->unlock)
        r->unlock(r->ctx);
}

static rdof_modem_t *modem(rdof_repeater_t *r)
{
    return r->radio->modem;
}

static bool is_block(int dt)
{
    return dt == RDOF_DT_RATE_12 || dt == RDOF_DT_RATE_34 || dt == RDOF_DT_RATE_1;
}

void rdof_repeater_set_call_hang(rdof_repeater_t *r, double call_hang_s)
{
    r->call_hang_s = call_hang_s > 0 ? call_hang_s : 0.0;
}

void rdof_repeater_init(rdof_repeater_t *r, rdof_radio_t *radio, uint32_t peer_id, uint32_t rx_freq, uint32_t tx_freq,
                      int cc)
{
    memset(r, 0, sizeof *r);
    r->radio = radio;
    r->peer_id = peer_id;
    r->rx_freq = rx_freq;
    r->tx_freq = tx_freq;
    r->cc = cc;
    r->power = 255;
    r->enabled[1] = r->enabled[2] = true;
    r->inactivity_s = 5.0;
    rdof_repeater_set_call_hang(r, 3.0);
    r->prebuffer = 6;
    r->cwid_every_s = 900.0;
    r->inbound = 1;
    r->outbound = 2;
    for (int n = 1; n <= 2; n++) {
        r->slots[n].n = n;
        r->slots[n].net = -1;
    }
    r->tx_until = -INFINITY;
    r->lead_until = -INFINITY;
    r->last_status = -INFINITY;
    r->last_id = -INFINITY;
    r->inbound_last = -INFINITY;
    rdof_jb_init(&r->jb);
}

void rdof_repeater_set_sfr(rdof_repeater_t *r, int inbound_slot)
{
    int inbound = inbound_slot == 2 ? 2 : 1; /* the slots are 1 and 2 */
    r->sfr = true;
    r->dcdm = false;
    r->inbound = inbound;
    r->outbound = 3 - inbound;
}

void rdof_repeater_set_dcdm(rdof_repeater_t *r)
{
    r->dcdm = true;
    r->sfr = false;
}

/* The modem keys itself for what it's given: no DMR_START, no lead, no short LC (SFR, DCDM). */
static bool keys_itself(const rdof_repeater_t *r)
{
    return r->sfr || r->dcdm;
}

/* ── queues ────────────────────────────────────────────────────────────────── */

void rdof_repeater_on_burst(rdof_repeater_t *r, const rdof_net_burst_t *b)
{
    lock(r);
    if (r->net_count < RDOF_RPT_IN) {
        int i = (r->net_head + r->net_count) % RDOF_RPT_IN;
        r->net_in[i] = *b;
        r->net_at[i] = now_of(r);
        r->net_count++;
    } else {
        r->dropped++;
    }
    unlock(r);
}

static void rf_push(rdof_repeater_t *r, const rdof_rpt_rf_in_t *in)
{
    lock(r);
    if (r->rf_count < RDOF_RPT_IN) {
        r->rf_in[(r->rf_head + r->rf_count) % RDOF_RPT_IN] = *in;
        r->rf_count++;
    } else {
        r->dropped++;
    }
    unlock(r);
}

void rdof_repeater_heard(rdof_repeater_t *r, int slot, uint8_t control, const uint8_t burst[RDOF_BURST_BYTES], int rssi)
{
    rdof_rpt_rf_in_t in = {.slot = slot, .lost = false, .control = control, .rssi = rssi};
    memcpy(in.burst, burst, RDOF_BURST_BYTES);
    rf_push(r, &in);
}

void rdof_repeater_lost(rdof_repeater_t *r, int slot)
{
    rdof_rpt_rf_in_t in = {.slot = slot, .lost = true, .rssi = -1};
    rf_push(r, &in);
}

void rdof_repeater_nak(rdof_repeater_t *r, uint8_t command, int err)
{
    lock(r);
    if (r->nak_count < 16) {
        int i = (r->nak_head + r->nak_count) % 16;
        r->naks[i].command = command;
        r->naks[i].err = err;
        r->nak_count++;
    }
    unlock(r);
}

/* An input or burst dropped for want of room; under the lock, as the other tasks count theirs. */
static void drop_one(rdof_repeater_t *r)
{
    lock(r);
    r->dropped++;
    unlock(r);
}

static void out_push(rdof_repeater_t *r, rdof_rpt_slot_t *s, const rdof_rpt_out_t *o)
{
    if (s->count >= RDOF_RPT_OUT) {
        drop_one(r);
        return;
    }
    s->out[(s->head + s->count) % RDOF_RPT_OUT] = *o;
    s->count++;
}

static rdof_rpt_out_t *out_at(rdof_rpt_slot_t *s, int i)
{
    return &s->out[(s->head + i) % RDOF_RPT_OUT];
}

static void out_pop(rdof_rpt_slot_t *s)
{
    s->head = (s->head + 1) % RDOF_RPT_OUT;
    s->count--;
}

static void pending_push(rdof_repeater_t *r, rdof_rpt_call_t *c, const rdof_rpt_out_t *o)
{
    if (c->npending >= RDOF_RPT_PENDING) {
        drop_one(r);
        return;
    }
    c->pending[c->npending++] = *o;
}

static void out_item(rdof_rpt_out_t *o, uint8_t control, const uint8_t *burst, int call, bool report)
{
    memset(o, 0, sizeof *o);
    o->control = control;
    if (burst)
        memcpy(o->burst, burst, RDOF_BURST_BYTES);
    o->call = (int8_t)call;
    o->report = report;
}

static void end_item(rdof_rpt_out_t *o, int call)
{
    memset(o, 0, sizeof *o);
    o->end = true;
    o->call = (int8_t)call;
}

static void done_add(rdof_rpt_slot_t *s, const uint8_t sid[4])
{
    memcpy(s->done[s->done_next], sid, 4);
    s->done_next = (s->done_next + 1) % RDOF_RPT_DONE;
    if (s->ndone < RDOF_RPT_DONE)
        s->ndone++;
}

static bool is_done(const rdof_rpt_slot_t *s, const uint8_t sid[4])
{
    for (int i = 0; i < s->ndone; i++)
        if (!memcmp(s->done[i], sid, 4))
            return true;
    return false;
}

static void activity_set(rdof_rpt_slot_t *s, rdof_rpt_kind_t kind, uint32_t dst, bool group)
{
    s->activity.kind = kind == RDOF_KIND_VOICE ? RDOF_ACT_VOICE : RDOF_ACT_DATA;
    s->activity.dst = dst;
    s->activity.group = group;
}

static void activity_clear(rdof_rpt_slot_t *s)
{
    memset(&s->activity, 0, sizeof s->activity);
}

/* ── hooks ─────────────────────────────────────────────────────────────────── */

static void call_info(const rdof_rpt_call_t *c, rdof_tx_call_info_t *info)
{
    memset(info, 0, sizeof *info);
    memcpy(info->stream_id, c->tune.stream_id, 4);
    info->src = c->src;
    info->dst = c->dst;
    info->group = c->group;
    info->freq = c->tune.freq;
    info->cc = c->tune.cc;
    info->slot = c->slot;
    info->has_csbk = c->has_csbk;
    memcpy(info->csbk, c->csbk, RDOF_BURST_BYTES);
    info->frames = c->kind == RDOF_KIND_DATA ? c->blocks : -1;
}

static void call_end(rdof_repeater_t *r, int idx)
{
    if (idx < 0)
        return;
    rdof_rpt_call_t *c = &r->calls[idx];
    if (c->finished)
        return;
    c->finished = true;
    if (!c->on_air)
        return;
    c->on_air = false;
    if (r->on_tx) {
        rdof_tx_call_info_t info;
        call_info(c, &info);
        double d = now_of(r) - c->on_air_at;
        r->on_tx(r->ctx, "end", &info, d > 0 ? d : 0.0);
    }
}

static void call_sent(rdof_repeater_t *r, int idx, const rdof_rpt_out_t *item, int slot)
{
    rdof_rpt_call_t *c = &r->calls[idx];
    if (c->finished)
        return;
    rdof_tx_call_info_t info;
    if (!c->on_air) {
        c->on_air = true;
        c->on_air_at = now_of(r);
        if (r->on_tx) {
            call_info(c, &info);
            r->on_tx(r->ctx, "start", &info, 0.0);
        }
    }
    if (item->report && r->on_sent) {
        rdof_net_burst_t b;
        memset(&b, 0, sizeof b);
        memcpy(b.stream_id, c->tune.stream_id, 4);
        b.control = item->control;
        memcpy(b.burst, item->burst, RDOF_BURST_BYTES);
        b.src = c->src;
        b.dst = c->dst;
        b.group = c->group;
        b.slot = slot;
        call_info(c, &info);
        r->on_sent(r->ctx, &info, &b);
    }
}

/* ── the voice stream out (section 6) ──────────────────────────────────────── */

static void vout_init(rdof_voice_out_t *v, const uint8_t lc9[9], int cc)
{
    memset(v, 0, sizeof *v);
    rdof_voice_regen_init(&v->regen, lc9, cc);
    v->cc = cc;
    rdof_duplex_lc_burst(lc9, RDOF_DT_VOICE_LC_HEADER, cc, v->header);
    v->wait_a = true;
}

/* The underrun fill (R6.3): silence for the next position. grow: a network stream's jitter buffer grows by it, so
 * the stream's later bursts go out a position on, not dropped (R6.4). */
static uint8_t vout_silence(rdof_voice_out_t *v, uint8_t out[RDOF_BURST_BYTES], bool grow)
{
    uint8_t raw[RDOF_BURST_BYTES];
    uint8_t control = rdof_duplex_silence(v->next, v->cc, raw);
    v->next = (v->next + 1) % 6;
    if (grow)
        v->shift = (v->shift + 1) % 6;
    else
        v->filled++;
    rdof_voice_regen_burst(&v->regen, control, raw, out);
    return control;
}

/* What to queue for a voice burst (R6.1-R6.5, R4.6, R7.6): none, or gap fill and it. Returns the count;
 * the burst itself is the last. jb / js: a network stream's jitter buffer (NULL for RF), with `ahead` bursts in
 * hand: positions moved by shift, the gap filled only up to the target, a burst dropped when it's ahead. */
static int vout_take(rdof_voice_out_t *v, uint8_t control, const uint8_t *burst, uint8_t controls[6],
                     uint8_t out[6][RDOF_BURST_BYTES], rdof_jb_t *jb, rdof_jb_stream_t *js, double ahead, double now)
{
    int pos = (control & RDOF_SYNC_AUDIO) ? 0 : (control & 0x0F), gap;
    if (v->wait_a) {
        if (pos != 0)
            return 0;
        v->wait_a = false;
        v->next = v->filled = 0;
    }
    int tpos = (pos + v->shift) % 6; /* where it goes out */
    if (v->filled >= 6) {
        gap = ((tpos - v->next) % 6 + 6) % 6; /* a superframe or more filled: can't tell late from early */
    } else {
        int d = ((tpos - (v->next - v->filled)) % 6 + 12) % 6;
        if (d < v->filled)
            return 0; /* R6.4: its position went out as silence */
        gap = d - v->filled;
    }
    if (jb && jb->adaptive) {
        int keep = rdof_jb_fill(jb, js, gap, ahead);
        if (keep < gap) { /* the silence past the target isn't needed */
            v->shift = ((v->shift - (gap - keep)) % 6 + 6) % 6;
            tpos = ((tpos - (gap - keep)) % 6 + 6) % 6;
            gap = keep;
        }
        if (!gap && rdof_jb_drop(jb, js, ahead, rdof_jb_silent(burst), now)) {
            v->shift = (v->shift + 5) % 6; /* its position goes to the next one */
            return 0;
        }
    }
    int n = 0;
    for (int i = 0; i < gap; i++, n++)
        controls[n] = vout_silence(v, out[n], false);
    if (tpos != pos) {
        uint8_t moved[RDOF_BURST_BYTES];
        int cc, pi, lcss;
        if (pos && rdof_decode_emb(burst, &cc, &pi, &lcss, NULL))
            v->pi = pi;
        /* the call's own LC fragment for its new position: regen reads it as heard, and passes the LC on */
        const uint8_t *frag = tpos >= 1 && tpos <= 4 ? v->regen.lc_raw + 32 * (tpos - 1) : NULL;
        controls[n] = rdof_jb_reframe(burst, tpos, v->cc, true, v->pi, frag, moved);
        rdof_voice_regen_burst(&v->regen, controls[n], moved, out[n]);
    } else {
        controls[n] = control;
        rdof_voice_regen_burst(&v->regen, control, burst, out[n]);
    }
    n++;
    v->next = (tpos + 1) % 6;
    v->filled = 0;
    return n;
}

/* ── to the modem ──────────────────────────────────────────────────────────── */

static double inactivity(const rdof_repeater_t *r)
{
    return r->inactivity_s > r->call_hang_s ? r->inactivity_s : r->call_hang_s; /* R3.6 */
}

static void restart(rdof_repeater_t *r, double t)
{
    if (t + inactivity(r) > r->tx_until) /* R3.4 */
        r->tx_until = t + inactivity(r);
}

static double held(const rdof_rpt_slot_t *s, double now)
{
    return s->air_until > now ? s->air_until - now : 0.0;
}

static void drop_queue(rdof_repeater_t *r, rdof_rpt_slot_t *s)
{
    int ended[RDOF_RPT_CALLS], n = 0;
    for (int i = 0; i < s->count; i++) {
        int c = out_at(s, i)->call;
        if (c < 0 || !r->calls[c].on_air)
            continue;
        bool seen = false;
        for (int k = 0; k < n; k++)
            seen |= ended[k] == c;
        if (!seen && n < RDOF_RPT_CALLS)
            ended[n++] = c;
    }
    s->count = 0;
    s->head = 0;
    for (int k = 0; k < n; k++)
        call_end(r, ended[k]);
}

static void aborted(rdof_repeater_t *r, rdof_rpt_slot_t *s, double now)
{
    s->space = 0;
    s->air_until = now;
    s->net_until = -INFINITY;
    r->last_status = -INFINITY;
}

static bool abort_slot(rdof_repeater_t *r, rdof_rpt_slot_t *s, double now)
{
    drop_queue(r, s);
    if (!rdof_modem_dmr_abort(modem(r), s->n))
        return false;
    aborted(r, s, now);
    return true;
}

static bool status(rdof_repeater_t *r, rdof_mmdvm_status_t *st)
{
    if (!rdof_modem_status(modem(r), st))
        return false;
    r->transmitting = st->transmitting != 0;
    r->inbound_flag = r->sfr && st->inbound_busy;
    return true;
}

static bool inbound_busy(rdof_repeater_t *r, double now)
{
    return r->inbound_flag || now - r->inbound_last < RDOF_RPT_SFR_QUIET_S;
}

bool rdof_repeater_busy(rdof_repeater_t *r, int slot)
{
    if (slot < 1 || slot > 2)
        return false;
    rdof_rpt_slot_t *s = &r->slots[slot];
    if (r->sfr && slot == r->outbound && inbound_busy(r, now_of(r)))
        return true;
    return s->rf.active || s->net >= 0 || s->count > 0;
}

bool rdof_repeater_on_air(const rdof_repeater_t *r)
{
    return r->keyed || r->transmitting;
}

static bool wait_until_quiet(rdof_repeater_t *r)
{
    rdof_modem_t *m = modem(r);
    for (int i = 0; i < 150; i++) {
        rdof_mmdvm_status_t st;
        if (!rdof_modem_status(m, &st))
            return false;
        if (!st.transmitting)
            return true;
        if (m->port.sleep_ms)
            m->port.sleep_ms(m->port.ctx, 100);
    }
    return true;
}

static bool key(rdof_repeater_t *r)
{
    if (r->keyed)
        return true;
    if (!keys_itself(r) && !rdof_modem_dmr_start(modem(r), true))
        return false;
    r->keyed = true;
    if (r->on_key)
        r->on_key(r->ctx, true);
    return true;
}

static bool unkey(rdof_repeater_t *r, double now)
{
    if (!keys_itself(r) && !rdof_modem_dmr_start(modem(r), false))
        return false;
    r->keyed = false;
    for (int n = 1; n <= 2; n++) {
        rdof_rpt_slot_t *s = &r->slots[n];
        s->has_hang = false;
        s->air_until = -INFINITY;
        if (!s->rf.active && s->net < 0)
            activity_clear(s);
    }
    if (r->on_key)
        r->on_key(r->ctx, false);
    if (r->cwid[0] && r->sent_since_id && now - r->last_id >= r->cwid_every_s) {
        if (!wait_until_quiet(r) || !rdof_modem_set_mode(modem(r), RDOF_MODE_IDLE) ||
            !rdof_modem_send_cwid(modem(r), r->cwid) || !wait_until_quiet(r) ||
            !rdof_modem_set_mode(modem(r), RDOF_MODE_DMR))
            return false;
        r->last_id = now_of(r);
        r->sent_since_id = false;
    }
    return true;
}

/* R9.4: a new short LC whenever either slot's activity changes, the Null one when both are idle. */
static bool update_short_lc(rdof_repeater_t *r)
{
    rdof_activity_t a1 = r->slots[1].activity, a2 = r->slots[2].activity;
    if (r->has_short_lc && !memcmp(&a1, &r->short_lc[0], sizeof a1) && !memcmp(&a2, &r->short_lc[1], sizeof a2))
        return true;
    r->has_short_lc = true;
    r->short_lc[0] = a1;
    r->short_lc[1] = a2;
    uint8_t lc[9];
    rdof_activity_lc(&a1, &a2, r->short_lc_ts1_low, lc);
    return rdof_modem_dmr_short_lc(modem(r), lc);
}

/* ── calls ─────────────────────────────────────────────────────────────────── */

static int new_call(rdof_repeater_t *r)
{
    for (int i = 0; i < RDOF_RPT_CALLS; i++) {
        bool referenced = false;
        for (int n = 1; n <= 2 && !referenced; n++) {
            rdof_rpt_slot_t *s = &r->slots[n];
            if (s->net == i)
                referenced = true;
            for (int k = 0; k < s->count && !referenced; k++)
                referenced = out_at(s, k)->call == i;
        }
        if (!referenced) {
            memset(&r->calls[i], 0, sizeof r->calls[i]);
            r->calls[i].used = true;
            r->calls[i].group = true;
            return i;
        }
    }
    return -1;
}

static void end_marker(rdof_rpt_out_t *o, int call, rdof_rpt_kind_t kind, uint32_t seq, const uint8_t *term)
{
    end_item(o, call);
    o->end_kind = (uint8_t)kind;
    o->seq = seq;
    if (term)
        memcpy(o->burst, term, RDOF_BURST_BYTES);
}

static void rf_end(rdof_repeater_t *r, rdof_rpt_slot_t *s, double now)
{
    rdof_rpt_rf_t *rf = &s->rf;
    rf->active = false;
    s->has_last_rf = true;
    s->last_src = rf->src;
    s->last_dst = rf->dst;
    s->last_at = now;
    if (r->dcdm)
        return; /* nothing repeated (R13.3) */
    rdof_rpt_out_t o;
    if (rf->kind == RDOF_KIND_VOICE) {
        uint8_t term[RDOF_BURST_BYTES];
        rdof_duplex_lc_burst(rf->lc9, RDOF_DT_TERMINATOR_WITH_LC, r->cc, term);
        out_item(&o, TERMINATOR, term, -1, false);
        out_push(r, s, &o);
        end_marker(&o, -1, RDOF_KIND_VOICE, s->seq, term);
    } else {
        end_marker(&o, -1, RDOF_KIND_DATA, s->seq, NULL);
    }
    out_push(r, s, &o);
}

/* R8.1: an RF call has the slot. The network stream (and anything of the network's queued here or held by the
 * modem) is dropped; RF traffic queued before it stays. */
static bool network_yields(rdof_repeater_t *r, rdof_rpt_slot_t *s, double now)
{
    bool theirs = false;
    for (int i = 0; i < s->count && !theirs; i++)
        theirs = out_at(s, i)->call >= 0;
    if (s->net < 0 && !theirs && now >= s->net_until)
        return true;
    int ended[RDOF_RPT_CALLS + 1], n = 0;
    if (s->net >= 0) {
        ended[n++] = s->net;
        done_add(s, r->calls[s->net].tune.stream_id);
        rdof_jb_end(&r->jb, &r->calls[s->net].jbs);
        s->net = -1;
    }
    int kept = 0, count = s->count;
    for (int i = 0; i < count; i++) {
        rdof_rpt_out_t *o = out_at(s, i);
        if (o->call >= 0) {
            bool seen = false;
            for (int k = 0; k < n; k++)
                seen |= ended[k] == o->call;
            if (!seen && n < RDOF_RPT_CALLS + 1)
                ended[n++] = o->call;
            continue;
        }
        if (kept != i)
            *out_at(s, kept) = *o; /* in place: kept <= i */
        kept++;
    }
    s->count = kept;
    for (int k = 0; k < n; k++)
        call_end(r, ended[k]);
    if (!rdof_modem_dmr_abort(modem(r), s->n))
        return false;
    aborted(r, s, now);
    return true;
}

static bool rf_start(rdof_repeater_t *r, rdof_rpt_slot_t *s, rdof_rpt_kind_t kind, uint32_t src, uint32_t dst, bool group,
                     const uint8_t *lc9, double now)
{
    if (!network_yields(r, s, now))
        return false;
    s->seq++;
    s->has_hang = false; /* R3.7 */
    memset(&s->rf, 0, sizeof s->rf);
    s->rf.active = true;
    s->rf.kind = kind;
    s->rf.src = src;
    s->rf.dst = dst;
    s->rf.group = group;
    if (lc9)
        memcpy(s->rf.lc9, lc9, 9);
    s->rf.last = now;
    activity_set(s, kind, dst, group);
    s->late_have = 0;
    return true;
}

static bool rf_voice(rdof_repeater_t *r, rdof_rpt_slot_t *s, uint32_t src, uint32_t dst, bool group, const uint8_t lc9[9],
                     double now)
{
    if (!rf_start(r, s, RDOF_KIND_VOICE, src, dst, group, lc9, now))
        return false;
    vout_init(&s->rf.voice, lc9, r->cc);
    rdof_rpt_out_t o;
    out_item(&o, HEADER, s->rf.voice.header, -1, false);
    for (int i = 0; i < RDOF_RPT_HEADERS && !r->dcdm; i++)
        out_push(r, s, &o);
    return true;
}

static void queue_rf(rdof_repeater_t *r, rdof_rpt_slot_t *s, uint8_t control, const uint8_t *burst)
{
    if (r->dcdm)
        return; /* RF is only followed, to know who has the slot (R13.3) */
    rdof_rpt_out_t o;
    out_item(&o, control, burst, -1, false);
    out_push(r, s, &o);
}

/* R4.6: late entry. Returns 1 if the call started, 0 if not, -1 on a modem error. */
static int late_entry(rdof_repeater_t *r, rdof_rpt_slot_t *s, uint8_t control, const uint8_t *burst, double now)
{
    static const int lcss_of[5] = {0, 1, 3, 3, 2};
    if (control & RDOF_SYNC_AUDIO) {
        s->late_have = 0;
        return 0;
    }
    int n = control & 0x0F, cc = -1, lcss = -1;
    if (!rdof_decode_emb(burst, &cc, NULL, &lcss, NULL))
        cc = -1;
    if (n < 1 || n > 4 || cc != r->cc || lcss != lcss_of[n])
        return 0;
    rdof_embedded_signalling(burst, s->late[n]);
    s->late_have |= (uint8_t)(1 << n);
    if (n != 4 || s->late_have != 0x1E)
        return 0;
    uint8_t raw[128], lc9[9];
    for (int i = 0; i < 4; i++)
        memcpy(raw + 32 * i, s->late[i + 1], 32);
    s->late_have = 0;
    if (!rdof_embedded_lc_decode(raw, lc9))
        return 0;
    rdof_full_lc_t lc;
    rdof_parse_full_lc(lc9, &lc);
    if (lc.src_id == RDOF_NONE)
        return 0;
    return rf_voice(r, s, (uint32_t)lc.src_id, (uint32_t)lc.dst_id, lc.group, lc9, now) ? 1 : -1;
}

/* The slot carries a network voice call (streaming, or its bursts still queued here or in the modem). */
static bool net_voice_on(rdof_repeater_t *r, rdof_rpt_slot_t *s, double now)
{
    if ((s->net >= 0 && r->calls[s->net].kind == RDOF_KIND_VOICE) || now < s->net_until)
        return true;
    for (int i = 0; i < s->count; i++) {
        rdof_rpt_out_t *o = out_at(s, i);
        if (o->call >= 0 && !o->end && r->calls[o->call].kind == RDOF_KIND_VOICE)
            return true;
    }
    return false;
}

static int rf_block(rdof_repeater_t *r, rdof_rpt_slot_t *s, uint8_t control, const uint8_t *burst, double now)
{
    uint8_t out[RDOF_BURST_BYTES];
    if (!rdof_duplex_data(control, burst, r->cc, out))
        return 0;
    s->rf.blocks--;
    s->rf.last = now;
    queue_rf(r, s, control, out);
    if (s->rf.blocks <= 0)
        rf_end(r, s, now);
    return 1;
}

/* _repeat (section 4): 1 queued, 0 nothing, -1 a modem error. */
static int repeat(rdof_repeater_t *r, rdof_rpt_slot_t *s, uint8_t control, const uint8_t *burst, double now)
{
    rdof_rpt_rf_t *rf = &s->rf;
    uint8_t out[RDOF_BURST_BYTES];
    if (control & RDOF_SYNC_DATA) {
        int dt = control & 0x0F;
        if (dt == RDOF_DT_VOICE_LC_HEADER) {
            rdof_full_lc_t lc;
            bool ok = rdof_decode_voice_lc(burst, dt, &lc) && lc.src_id != RDOF_NONE;
            if (rf->active && rf->kind == RDOF_KIND_VOICE) {
                if (!ok || ((uint32_t)lc.src_id == rf->src && (uint32_t)lc.dst_id == rf->dst)) {
                    rf->last = now;
                    return 1; /* R4.2: its header again, absorbed */
                }
                rf_end(r, s, now); /* R4.3 */
            }
            if (!ok)
                return 0;
            return rf_voice(r, s, (uint32_t)lc.src_id, (uint32_t)lc.dst_id, lc.group, lc.raw, now) ? 1 : -1;
        }
        if (dt == RDOF_DT_TERMINATOR_WITH_LC) {
            if (!rf->active || rf->kind != RDOF_KIND_VOICE)
                return 0;
            rf_end(r, s, now);
            return 1;
        }
        if (dt == RDOF_DT_PI_HEADER) {
            if (!rf->active || rf->kind != RDOF_KIND_VOICE || !rf->voice.wait_a)
                return 0; /* R4.4: after the headers, before A */
            if (!rdof_duplex_data(control, burst, r->cc, out))
                return 0;
            queue_rf(r, s, control, out);
            return 1;
        }
        if (dt == RDOF_DT_DATA_HEADER) {
            rdof_data_header_t h;
            rdof_decode_data_header(burst, &h);
            if (rf->active && rf->kind == RDOF_KIND_DATA) {
                if (!h.crc_ok || h.dpf == RDOF_DPF_PROPRIETARY)
                    return rf_block(r, s, control, burst, now); /* R4.8: part of the open data call */
                rf_end(r, s, now); /* R4.8: the next packet (a chain, or a block of this one lost) */
            }
            if (!h.crc_ok)
                return 0;
            if (!rf_start(r, s, RDOF_KIND_DATA, h.src_id, h.dst_id, h.group, NULL, now))
                return -1;
            s->rf.blocks = h.blocks;
            if (rdof_duplex_data(control, burst, r->cc, out))
                queue_rf(r, s, control, out);
            if (h.blocks == 0)
                rf_end(r, s, now);
            return 1;
        }
        if (dt == RDOF_DT_CSBK) {
            rdof_csbk_t c;
            rdof_decode_csbk(burst, &c);
            if (!c.crc_ok || c.opcode == BS_DWN_ACT)
                return 0;
            if (c.src_id != RDOF_NONE) {
                s->has_last_rf = true;
                s->last_src = (uint32_t)c.src_id;
                s->last_dst = (uint32_t)c.dst_id;
                s->last_at = now;
            }
            if (net_voice_on(r, s, now))
                return 0; /* R8.3: not into the network's superframes */
            if (!rdof_duplex_data(control, burst, r->cc, out))
                return 0;
            queue_rf(r, s, control, out);
            return 1;
        }
        if (is_block(dt)) {
            if (!rf->active || rf->kind != RDOF_KIND_DATA || rf->blocks <= 0)
                return 0;
            return rf_block(r, s, control, burst, now);
        }
        return 0; /* MBC, USBD (R4.10, not yet), idle (R4.11) */
    }
    if (rf->active && rf->kind == RDOF_KIND_VOICE) {
        if (!(control & RDOF_SYNC_AUDIO) && (control & 0x0F) > 5)
            return 0;
        rf->last = now;
        uint8_t controls[6], bursts[6][RDOF_BURST_BYTES];
        int n = vout_take(&rf->voice, control, burst, controls, bursts, NULL, NULL, 0, now);
        for (int i = 0; i < n; i++)
            queue_rf(r, s, controls[i], bursts[i]);
        return 1;
    }
    return late_entry(r, s, control, burst, now);
}

static bool rf_takes(rdof_repeater_t *r, rdof_rpt_slot_t *s, double now)
{
    int call = s->net;
    s->net = -1;
    if (call >= 0) {
        done_add(s, r->calls[call].tune.stream_id);
        rdof_jb_end(&r->jb, &r->calls[call].jbs);
    }
    if (!abort_slot(r, s, now))
        return false;
    activity_clear(s);
    s->has_hang = false;
    call_end(r, call);
    return true;
}

static void hear(rdof_repeater_t *r, int slot, uint8_t control, const uint8_t *burst, int rssi)
{
    if (!r->receiver.hear)
        return;
    int dbm;
    double d = rdof_radio_rssi(r->radio, rssi, r->rx_freq, &dbm) ? (double)dbm : NAN;
    r->receiver.hear(r->receiver.ctx, slot, control, burst, d, r->cc, r->rx_freq);
}

static bool rf_sfr(rdof_repeater_t *r, const rdof_rpt_rf_in_t *in, double now)
{
    if (in->slot != r->inbound)
        return true;
    if (in->lost) {
        r->inbound_last = -INFINITY;
        return true;
    }
    if (in->control & RDOF_CONTROL_IDLE) /* a wakeup: the radio keys itself */
        return true;
    hear(r, in->slot, in->control, in->burst, in->rssi);
    if ((in->control & RDOF_SYNC_DATA) && (in->control & 0x0F) == RDOF_DT_TERMINATOR_WITH_LC) {
        r->inbound_last = -INFINITY; /* over: the network may have the slot */
        return true;
    }
    r->inbound_last = now;
    rdof_rpt_slot_t *out = &r->slots[r->outbound];
    if (out->net >= 0 || out->count > 0)
        return rf_takes(r, out, now);
    return true;
}

static bool rf(rdof_repeater_t *r, const rdof_rpt_rf_in_t *in, double now)
{
    if (r->sfr)
        return rf_sfr(r, in, now);
    if (in->slot < 1 || in->slot > 2)
        return true;
    rdof_rpt_slot_t *s = &r->slots[in->slot];
    if (in->lost) {
        if (s->rf.active)
            rf_end(r, s, now);
        return true;
    }
    uint8_t control = in->control;
    if (control & RDOF_CONTROL_IDLE) {
        /* The idle receiver (transmitter off) only passes data sync: BS_Dwn_Act wakes us (R3.1). */
        if ((control & RDOF_SYNC_DATA) && (control & 0x0F) == RDOF_DT_CSBK) {
            rdof_csbk_t c;
            rdof_decode_csbk(in->burst, &c);
            if (c.crc_ok && c.opcode == BS_DWN_ACT) {
                if (!key(r))
                    return false;
                restart(r, now);
            }
        }
        return true;
    }
    hear(r, in->slot, control, in->burst, in->rssi);
    if (r->enabled[in->slot]) {
        int q = repeat(r, s, control, in->burst, now);
        if (q < 0)
            return false;
        if (q > 0)
            restart(r, now); /* R3.4 */
    }
    return true;
}

static bool refused(rdof_repeater_t *r, uint8_t command, int err)
{
    uint8_t outbound = r->outbound == 1 ? RDOF_MMDVM_DMR_DATA1 : RDOF_MMDVM_DMR_DATA2;
    if (r->sfr && command == outbound && err == RDOF_NAK_RF_BUSY) {
        r->inbound_flag = true;
        rdof_rpt_slot_t *s = &r->slots[r->outbound];
        if (s->net >= 0 || s->count > 0)
            return rf_takes(r, s, now_of(r));
    }
    return true;
}

/* ── the network ───────────────────────────────────────────────────────────── */

/* DCDM: a network voice call's headers and terminators, as a simplex hotspot's (R11.4). */
static int headers(const rdof_repeater_t *r)
{
    return r->radio->headers > 0 ? r->radio->headers : 1;
}

static int terminators(const rdof_repeater_t *r)
{
    return r->radio->terminators > 0 ? r->radio->terminators : 1;
}

static void net_end(rdof_repeater_t *r, rdof_rpt_slot_t *s, int idx)
{
    rdof_rpt_call_t *c = &r->calls[idx];
    rdof_rpt_out_t o;
    if (c->kind == RDOF_KIND_VOICE) {
        uint8_t term[RDOF_BURST_BYTES];
        rdof_duplex_lc_burst(c->lc9, RDOF_DT_TERMINATOR_WITH_LC, r->cc, term);
        for (int i = 0; i < (r->dcdm ? terminators(r) : 1); i++) { /* DCDM: Ts of them, no hang (R13.5) */
            out_item(&o, TERMINATOR, term, idx, i == 0);
            pending_push(r, c, &o);
        }
        end_marker(&o, idx, RDOF_KIND_VOICE, s->seq, term);
    } else {
        end_marker(&o, idx, c->kind, s->seq, NULL);
    }
    pending_push(r, c, &o);
    for (int i = 0; i < c->npending; i++)
        out_push(r, s, &c->pending[i]);
    c->npending = 0;
    c->started = true;
    rdof_jb_end(&r->jb, &c->jbs);
    if (c->kind != RDOF_KIND_DATA) /* data: the stream may carry another packet (R7.9) */
        done_add(s, c->tune.stream_id);
    s->net = -1;
}

/* A network voice or data call: the slot's call hang ends (R3.7, R8.7); the previous call's bursts go first. */
static void net_call_starts(rdof_rpt_slot_t *s, rdof_rpt_kind_t kind, const rdof_rpt_call_t *c)
{
    s->seq++;
    s->has_hang = false;
    activity_set(s, kind, c->dst, c->group);
}

static void net_voice(rdof_repeater_t *r, rdof_rpt_slot_t *s, int idx, const uint8_t lc9[9])
{
    rdof_rpt_call_t *c = &r->calls[idx];
    c->kind = RDOF_KIND_VOICE;
    memcpy(c->lc9, lc9, 9);
    vout_init(&c->voice, lc9, r->cc);
    c->has_voice = true;
    net_call_starts(s, RDOF_KIND_VOICE, c);
    rdof_rpt_out_t o;
    for (int i = 0; i < (r->dcdm ? headers(r) : RDOF_RPT_HEADERS); i++) {
        out_item(&o, HEADER, c->voice.header, idx, i == 0);
        pending_push(r, c, &o);
    }
}

static void net_add(rdof_repeater_t *r, rdof_rpt_slot_t *s, int idx, const rdof_net_burst_t *b, double at, double now)
{
    rdof_rpt_call_t *c = &r->calls[idx];
    rdof_rpt_out_t o;
    c->last_burst = now;
    c->received++;
    rdof_jb_key(&r->jb, &c->jbs, b->dst, b->group, b->slot, c->started);
    rdof_jb_arrival(&r->jb, &c->jbs, at, b->seq);
    uint8_t control = b->control;
    if (control & RDOF_SYNC_DATA) {
        int dt = control & 0x0F;
        if (dt == RDOF_DT_VOICE_LC_HEADER) {
            if (c->kind != RDOF_KIND_VOICE) {
                rdof_full_lc_t lc;
                uint8_t lc9[9];
                if (rdof_decode_voice_lc(b->burst, dt, &lc) && lc.src_id != RDOF_NONE)
                    memcpy(lc9, lc.raw, 9);
                else
                    rdof_group_lc(c->src, c->dst, c->group, 0, lc9);
                net_voice(r, s, idx, lc9);
            }
            return; /* R7.2: later headers ignored */
        }
        if (dt == RDOF_DT_TERMINATOR_WITH_LC) {
            if (c->kind == RDOF_KIND_VOICE)
                net_end(r, s, idx);
            return;
        }
        if (dt == RDOF_DT_PI_HEADER && (c->kind != RDOF_KIND_VOICE || !c->voice.wait_a))
            return; /* R7.6: only between the headers and A */
        uint8_t data[RDOF_BURST_BYTES];
        bool have = rdof_duplex_data(control, b->burst, r->cc, data);
        if (dt == RDOF_DT_DATA_HEADER && c->kind != RDOF_KIND_DATA) {
            rdof_data_header_t h;
            rdof_decode_data_header(b->burst, &h);
            c->kind = RDOF_KIND_DATA;
            c->blocks = h.crc_ok ? h.blocks : 0;
            net_call_starts(s, RDOF_KIND_DATA, c);
        }
        if (have) {
            out_item(&o, control, data, idx, true);
            pending_push(r, c, &o);
        }
        if (dt == RDOF_DT_CSBK && b->terminator && c->kind == RDOF_KIND_NONE) {
            c->has_csbk = true; /* a CSBK on its own (a radio check) */
            memcpy(c->csbk, b->burst, RDOF_BURST_BYTES);
            net_end(r, s, idx);
            return;
        }
        if (c->kind == RDOF_KIND_DATA) {
            if (dt == RDOF_DT_DATA_HEADER && c->blocks <= 0) {
                net_end(r, s, idx);
            } else if (dt != RDOF_DT_DATA_HEADER && is_block(dt)) {
                c->blocks--;
                if (c->blocks <= 0)
                    net_end(r, s, idx);
            }
        }
        return;
    }
    if (c->kind != RDOF_KIND_VOICE) { /* voice with no header: one from its addressing */
        uint8_t lc9[9];
        rdof_group_lc(c->src, c->dst, c->group, 0, lc9);
        net_voice(r, s, idx, lc9);
    }
    if (!(control & RDOF_SYNC_AUDIO) && (control & 0x0F) > 5)
        return;
    uint8_t controls[6], bursts[6][RDOF_BURST_BYTES];
    double ahead = s->count + c->npending + held(s, now) / RDOF_RPT_BURST_S;
    int n = vout_take(&c->voice, control, b->burst, controls, bursts, &r->jb, &c->jbs, ahead, now); /* R7.6, R7.7 */
    for (int i = 0; i < n; i++) {
        out_item(&o, controls[i], bursts[i], idx, i == n - 1);
        pending_push(r, c, &o);
    }
}

static bool net(rdof_repeater_t *r, const rdof_net_burst_t *b, double at, double now)
{
    int slot = r->sfr ? r->outbound : b->slot; /* SFR: everything goes out on the outbound slot */
    if (slot < 1 || slot > 2 || !r->enabled[slot])
        return true;
    rdof_rpt_slot_t *s = &r->slots[slot];
    if (is_done(s, b->stream_id))
        return true;
    if (s->net >= 0 && !memcmp(r->calls[s->net].tune.stream_id, b->stream_id, 4)) {
        net_add(r, s, s->net, b, at, now);
        return true;
    }
    if (s->net >= 0 || s->rf.active) {
        done_add(s, b->stream_id); /* another stream (R8.5), or an RF call (R8.2), has the slot */
        return true;
    }
    if (r->sfr && inbound_busy(r, now)) {
        if (now - r->inbound_last >= RDOF_RPT_SFR_QUIET_S) {
            rdof_mmdvm_status_t st; /* busy only by the flag, which may be old: ask again */
            if (!status(r, &st))
                return false;
        }
        if (inbound_busy(r, now)) {
            done_add(s, b->stream_id);
            return true;
        }
    }
    if (s->has_last_rf && s->last_src == b->src && s->last_dst == b->dst && now - s->last_at < RDOF_RPT_ECHO_S) {
        done_add(s, b->stream_id); /* our own repeat of it (R8.4) */
        return true;
    }
    int idx = new_call(r);
    if (idx < 0) {
        drop_one(r);
        done_add(s, b->stream_id);
        return true;
    }
    rdof_rpt_call_t *c = &r->calls[idx];
    memcpy(c->tune.stream_id, b->stream_id, 4);
    c->tune.freq = r->tx_freq;
    c->tune.cc = r->cc;
    c->tune.power = r->power;
    c->slot = s->n;
    c->src = b->src;
    c->dst = b->dst;
    c->group = b->group;
    c->last_burst = now;
    rdof_jb_begin(&r->jb, &c->jbs);
    s->net = idx;
    if (!r->keyed && !keys_itself(r))
        r->lead_until = now + RDOF_RPT_KEY_LEAD_S; /* R3.3 */
    if (!key(r)) /* R7.5: keyed while it prebuffers */
        return false;
    restart(r, now);
    net_add(r, s, idx, b, at, now);
    return true;
}

static void timeouts(rdof_repeater_t *r, rdof_rpt_slot_t *s, double now)
{
    if (s->rf.active && now - s->rf.last > RDOF_RPT_RF_TIMEOUT_S)
        rf_end(r, s, now);
    if (s->net >= 0 && now - r->calls[s->net].last_burst > RDOF_RPT_NET_TIMEOUT_S)
        net_end(r, s, s->net);
}

static void feed(rdof_repeater_t *r, rdof_rpt_slot_t *s)
{
    if (s->net < 0)
        return;
    rdof_rpt_call_t *c = &r->calls[s->net];
    if (!c->started && c->received >= rdof_jb_prebuffer(&r->jb, &c->jbs, r->prebuffer))
        c->started = true; /* R7.4 */
    if (c->started && c->npending) {
        for (int i = 0; i < c->npending; i++)
            out_push(r, s, &c->pending[i]);
        c->npending = 0;
    }
}

/* The voice stream going out on the slot, if one is open (RF, or the network's past its prebuffer). */
static rdof_voice_out_t *open_voice(rdof_repeater_t *r, rdof_rpt_slot_t *s)
{
    if (s->rf.active && s->rf.kind == RDOF_KIND_VOICE)
        return r->dcdm ? NULL : &s->rf.voice; /* DCDM: RF isn't repeated */
    if (s->net >= 0) {
        rdof_rpt_call_t *c = &r->calls[s->net];
        if (c->kind == RDOF_KIND_VOICE && c->started)
            return &c->voice;
    }
    return NULL;
}

static bool write(rdof_repeater_t *r, rdof_rpt_slot_t *s, uint8_t control, const uint8_t *burst, double now)
{
    uint8_t framed[RDOF_BURST_BYTES];
    if (r->dcdm && (control & (RDOF_SYNC_AUDIO | RDOF_SYNC_DATA))) { /* R13.4: the slot's direct mode sync */
        uint64_t v = s->n == 1 ? RDOF_TS1_VOICE : RDOF_TS2_VOICE;
        memcpy(framed, burst, RDOF_BURST_BYTES);
        rdof_set_field(framed, 108, 48, (control & RDOF_SYNC_DATA) ? rdof_data_sync(v) : v);
        burst = framed;
    }
    if (!rdof_modem_write_dmr(modem(r), control, burst, s->n))
        return false;
    s->space--;
    s->air_until = (s->air_until > now ? s->air_until : now) + RDOF_RPT_BURST_S;
    r->sent_since_id = true;
    return true;
}

#define EPS 1e-6

/* Filler, just in time (R10.3): headers while a call waits for voice (R4.2), silence when its voice runs dry
 * (R6.3), the call hang's terminators (R3.5, R3.8). */
static bool fill(rdof_repeater_t *r, rdof_rpt_slot_t *s, double now)
{
    rdof_voice_out_t *v = open_voice(r, s);
    if (v) {
        if (v->wait_a) {
            while (s->space > 1 && held(s, now) < RDOF_RPT_FILL_AHEAD_S - EPS)
                if (!write(r, s, HEADER, v->header, now))
                    return false;
        } else if (s->space > 1 && held(s, now) <= RDOF_RPT_BURST_S + EPS) {
            uint8_t b[RDOF_BURST_BYTES];
            rdof_rpt_call_t *c = s->net >= 0 && v == &r->calls[s->net].voice ? &r->calls[s->net] : NULL;
            uint8_t control = vout_silence(v, b, c && rdof_jb_underrun(&r->jb, &c->jbs));
            if (!write(r, s, control, b, now))
                return false;
        }
        return true;
    }
    if (!s->has_hang)
        return true;
    while (s->space > 1 && held(s, now) < RDOF_RPT_FILL_AHEAD_S - EPS &&
           (s->air_until > now ? s->air_until : now) + RDOF_RPT_BURST_S <= s->hang_until + EPS)
        if (!write(r, s, TERMINATOR, s->hang_term, now))
            return false;
    if ((s->air_until > now ? s->air_until : now) + RDOF_RPT_BURST_S > s->hang_until + EPS &&
        now >= s->air_until - EPS) {
        s->has_hang = false; /* channel hang: the activity goes (R3.6, R9.3) */
        if (!s->rf.active && s->net < 0)
            activity_clear(s);
    }
    return true;
}

/* A call's last burst has gone to the modem. */
static void ended(rdof_repeater_t *r, rdof_rpt_slot_t *s, const rdof_rpt_out_t *item, double now)
{
    call_end(r, item->call);
    if (item->seq != s->seq)
        return; /* a later call has the slot */
    if (item->end_kind == RDOF_KIND_VOICE && !r->dcdm) {
        /* R3.5: T_CallHt from its terminator, which the modem holds as its last burst. */
        s->has_hang = true;
        memcpy(s->hang_term, item->burst, RDOF_BURST_BYTES);
        s->hang_until = (s->air_until > now ? s->air_until : now) - RDOF_RPT_BURST_S + r->call_hang_s;
    } else if (item->end_kind != RDOF_KIND_NONE && !s->rf.active && s->net < 0) {
        activity_clear(s); /* data, or DCDM voice: no hang */
    }
}

static bool transmit(rdof_repeater_t *r, double now)
{
    bool active[3] = {false, false, false}, any = false, traffic = false;
    for (int n = 1; n <= 2; n++) {
        rdof_rpt_slot_t *s = &r->slots[n];
        active[n] = s->count > 0 || s->has_hang || open_voice(r, s) != NULL;
        any |= active[n];
        traffic |= s->count > 0;
    }
    if (!any) {
        if (r->keyed && now >= r->tx_until && !r->slots[1].rf.active && !r->slots[2].rf.active &&
            r->slots[1].net < 0 && r->slots[2].net < 0)
            return unkey(r, now); /* R3.4 */
        if (now - r->last_status >= RDOF_RPT_IDLE_STATUS_EVERY_S) {
            rdof_mmdvm_status_t st;
            if (!status(r, &st))
                return false;
            r->last_status = now;
        }
        return true;
    }
    if (traffic && !key(r))
        return false;
    bool all_low = true;
    for (int n = 1; n <= 2; n++)
        if (active[n] && r->slots[n].space > 1)
            all_low = false;
    if (now - r->last_status >= RDOF_RPT_STATUS_EVERY_S || all_low) {
        rdof_mmdvm_status_t st;
        if (!status(r, &st)) {
            if (!r->has_status_missed) {
                r->has_status_missed = true;
                r->status_missed = now;
            } else if (now - r->status_missed > RDOF_RPT_STATUS_GIVE_UP_S) {
                return false;
            }
            return true;
        }
        r->last_status = now;
        r->has_status_missed = false;
        r->slots[1].space = st.space1;
        r->slots[2].space = st.space;
    }
    for (int n = 1; n <= 2; n++) {
        if (!active[n])
            continue;
        rdof_rpt_slot_t *s = &r->slots[n];
        while (s->count) {
            rdof_rpt_out_t item = *out_at(s, 0);
            if (item.end) {
                out_pop(s);
                ended(r, s, &item, now);
                continue;
            }
            if (s->space <= 1) /* R10.2: write while the modem has room for 2 */
                break;
            if (item.call >= 0 && now < r->lead_until)
                break; /* R3.3: the outbound runs a while before the call */
            out_pop(s);
            if (!write(r, s, item.control, item.burst, now))
                return false;
            if (s->air_until + inactivity(r) > r->tx_until) /* R3.4 */
                r->tx_until = s->air_until + inactivity(r);
            if (item.call >= 0) {
                s->net_until = s->air_until;
                call_sent(r, item.call, &item, s->n);
            }
        }
        if (!s->count && !fill(r, s, now))
            return false;
    }
    return true;
}

static void fault(rdof_repeater_t *r)
{
    r->keyed = r->ready = false;
    for (int n = 1; n <= 2; n++) {
        rdof_rpt_slot_t *s = &r->slots[n];
        drop_queue(r, s);
        if (s->net >= 0) {
            int c = s->net;
            done_add(s, r->calls[c].tune.stream_id);
            rdof_jb_end(&r->jb, &r->calls[c].jbs);
            call_end(r, c);
        }
        s->rf.active = false;
        s->net = -1;
        s->has_hang = false;
        s->air_until = s->net_until = -INFINITY;
        activity_clear(s);
    }
}

bool rdof_repeater_start(rdof_repeater_t *r)
{
    lock(r);
    r->rf_count = r->net_count = r->nak_count = 0;
    unlock(r);
    r->inbound_last = -INFINITY;
    r->inbound_flag = false;
    r->transmitting = false;
    if (r->dcdm ? !rdof_radio_tune_dcdm(r->radio, r->tx_freq, r->cc, r->power)
                : !rdof_radio_tune_duplex(r->radio, r->rx_freq, r->tx_freq, r->cc, r->power))
        return false;
    r->keyed = false;
    r->has_short_lc = true; /* as (idle, idle): the Null short LC only once something changes */
    memset(r->short_lc, 0, sizeof r->short_lc);
    for (int n = 1; n <= 2; n++) {
        r->slots[n].has_hang = false;
        r->slots[n].air_until = r->slots[n].net_until = -INFINITY;
    }
    r->last_status = -INFINITY;
    r->ready = true;
    return true;
}

bool rdof_repeater_poll(rdof_repeater_t *r)
{
    double now = now_of(r);
    bool ok = true;
    for (;;) {
        rdof_rpt_rf_in_t in;
        lock(r);
        bool have = r->rf_count > 0;
        if (have) {
            in = r->rf_in[r->rf_head];
            r->rf_head = (r->rf_head + 1) % RDOF_RPT_IN;
            r->rf_count--;
        }
        unlock(r);
        if (!have)
            break;
        if (!(ok = rf(r, &in, now)))
            goto fail;
    }
    for (;;) {
        uint8_t command = 0;
        int err = 0;
        lock(r);
        bool have = r->nak_count > 0;
        if (have) {
            command = r->naks[r->nak_head].command;
            err = r->naks[r->nak_head].err;
            r->nak_head = (r->nak_head + 1) % 16;
            r->nak_count--;
        }
        unlock(r);
        if (!have)
            break;
        if (!(ok = refused(r, command, err)))
            goto fail;
    }
    for (int n = 1; n <= 2; n++)
        timeouts(r, &r->slots[n], now);
    for (;;) {
        rdof_net_burst_t b;
        double at = now;
        lock(r);
        bool have = r->net_count > 0;
        if (have) {
            b = r->net_in[r->net_head];
            at = r->net_at[r->net_head];
            r->net_head = (r->net_head + 1) % RDOF_RPT_IN;
            r->net_count--;
        }
        unlock(r);
        if (!have)
            break;
        if (!(ok = net(r, &b, at, now)))
            goto fail;
    }
    for (int n = 1; n <= 2; n++)
        feed(r, &r->slots[n]);
    if (!(ok = transmit(r, now)))
        goto fail;
    if (!keys_itself(r) && !(ok = update_short_lc(r))) /* no CACH from us (R9.5) */
        goto fail;
    if (r->receiver.tick)
        r->receiver.tick(r->receiver.ctx);
    return true;
fail:
    fault(r);
    return false;
}

void rdof_repeater_stop(rdof_repeater_t *r)
{
    if (r->keyed && !r->dcdm)
        rdof_modem_dmr_start(modem(r), false);
}

#endif /* RDOF_FEATURE_REPEATER */
