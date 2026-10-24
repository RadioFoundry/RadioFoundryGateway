// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The roaming transceiver (rdof/txvr.h); the scenario tests check it. */
#include "rdof/txvr.h"

#if RDOF_FEATURE_TX

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/bits.h"
#include "rdof/fec.h"
#include "rdof/layer2.h"
#include "rdof/pdu.h"

#define TMS_PORT 4007
#define TMS_ACK_REQUESTED 0x40
#define TMS_CONTROL 0x10
#define CSBK_CONTROL (RDOF_SYNC_DATA | RDOF_DT_CSBK)
#define HEADER_CONTROL (RDOF_SYNC_DATA | RDOF_DT_VOICE_LC_HEADER)
#define TERMINATOR_CONTROL (RDOF_SYNC_DATA | RDOF_DT_TERMINATOR_WITH_LC)

static double now(rdof_txvr_t *t)
{
    return t->clock ? t->clock(t->ctx) : 0.0;
}

static void lock(rdof_txvr_t *t)
{
    if (t->lock)
        t->lock(t->ctx);
}

static void unlock(rdof_txvr_t *t)
{
    if (t->unlock)
        t->unlock(t->ctx);
}

static bool near(uint32_t a, uint32_t b)
{
    return (a > b ? a - b : b - a) <= RDOF_TXVR_FREQ_TOLERANCE_HZ;
}

void rdof_scan_defaults(rdof_scan_t *s)
{
    memset(s, 0, sizeof *s);
    s->dwell_s = 0.45;
    s->hang_s = 3.0;
}

void rdof_txvr_init(rdof_txvr_t *t, rdof_radio_t *radio, uint32_t peer_id)
{
    memset(t, 0, sizeof *t);
    t->radio = radio;
    t->peer_id = peer_id;
    t->listen_s = 0.3;
    t->clear_s = 2.0;
    t->prebuffer = 6;
    t->csbk_preambles = 2;
    t->answer_s = 0.75;
    t->answer_max_s = 5.0;
    t->cwid_every_s = 900.0;
    t->tuned_at = -INFINITY;
    t->air_until = -INFINITY;
    t->last_status = -INFINITY;
    rdof_jb_init(&t->jb);
}

static rdof_modem_t *modem(rdof_txvr_t *t)
{
    return t->radio->modem;
}

/* ── queue ─────────────────────────────────────────────────────────────────── */

static void push(rdof_txcall_t *c, const rdof_net_burst_t *b)
{
    if (c->count >= RDOF_TXVR_QUEUE) {
        c->dropped++;
        return;
    }
    c->bursts[(c->head + c->count) % RDOF_TXVR_QUEUE] = *b;
    c->count++;
}

static void pop(rdof_txcall_t *c, rdof_net_burst_t *out)
{
    *out = c->bursts[c->head];
    c->head = (c->head + 1) % RDOF_TXVR_QUEUE;
    c->count--;
}

/* ── from the HBlink4 task ─────────────────────────────────────────────────── */

void rdof_txvr_channel_now(const rdof_txvr_t *t, uint32_t *freq, int *cc)
{
    *freq = t->has_tuned ? t->tuned_freq : 0;
    *cc = t->has_tuned ? t->tuned_cc : 0;
}

static void answer(rdof_txvr_t *t, const rdof_tune_t *tune, int status)
{
    if (status != RDOF_DMRK_ON_AIR) {
        memcpy(t->refused[t->refused_next], tune->stream_id, 4);
        t->refused_next = (t->refused_next + 1) % RDOF_TXVR_REFUSED;
        if (t->nrefused < RDOF_TXVR_REFUSED)
            t->nrefused++;
        for (int i = 0; i < RDOF_TXVR_ORPHANS; i++)
            if (t->orphans[i].used && !memcmp(t->orphans[i].stream_id, tune->stream_id, 4))
                t->orphans[i].used = false;
    }
    uint8_t pkt[RDOF_DMRK_LEN];
    rdof_dmrk_packet(t->peer_id, tune->stream_id, status, tune->freq, tune->cc, pkt);
    if (t->send)
        t->send(t->ctx, pkt, sizeof pkt);
}

/* A count from the struct, as far as its array goes. */
static int upto(int n, int max)
{
    return n < 0 ? 0 : n > max ? max : n;
}

static const rdof_channel_t *allowed(const rdof_txvr_t *t, uint32_t freq, int cc)
{
    for (int i = 0; i < upto(t->nchannels, RDOF_TXVR_MAX_CHANNELS); i++)
        if (near(t->channels[i].freq, freq) && t->channels[i].cc == cc)
            return &t->channels[i];
    return NULL;
}

/* _tms_wants_ack */
static bool tms_wants_ack(const uint8_t *ip, int len)
{
    if (len < 20 || ip[0] >> 4 != 4 || ip[9] != 17)
        return false;
    int ihl = (ip[0] & 0x0F) * 4;
    if (len < ihl + 8 + 3)
        return false;
    if (((ip[ihl + 2] << 8) | ip[ihl + 3]) != TMS_PORT)
        return false;
    uint8_t first = ip[ihl + 8 + 2];
    return (first & TMS_ACK_REQUESTED) && !(first & TMS_CONTROL);
}

/* _asks_reply */
static bool asks_reply(rdof_txcall_t *c, const rdof_net_burst_t *b)
{
    if (!(b->control & RDOF_SYNC_DATA))
        return false;
    int dt = b->control & 0x0F;
    if (dt == RDOF_DT_CSBK) {
        rdof_csbk_t cs;
        rdof_decode_csbk(b->burst, &cs);
        return cs.crc_ok && (cs.fid == 0x00 || cs.fid == 0x10) &&
               ((cs.opcode == 0x24 && cs.raw[3] == 0x80) || cs.opcode == 0x1F);
    }
    if (dt == RDOF_DT_DATA_HEADER) {
        rdof_data_header_t h;
        rdof_decode_data_header(b->burst, &h);
        if (!h.crc_ok)
            return false;
        if (h.dpf == RDOF_DPF_CONFIRMED && h.response_requested)
            return true;
        if (h.dpf == RDOF_DPF_UNCONFIRMED && h.sap == 4 /* SAP_IP */) {
            c->collecting_udata = true;
            c->udata_len = 0;
        }
        return false;
    }
    if (dt == RDOF_DT_RATE_12 && c->collecting_udata && c->udata_len < 48) {
        uint8_t block[12];
        rdof_bptc196_decode(b->burst, block);
        int n = c->udata_len + 12 <= 48 ? 12 : 48 - c->udata_len;
        memcpy(c->udata + c->udata_len, block, (size_t)n);
        c->udata_len += 12; /* the length grows by 12 each time */
        return tms_wants_ack(c->udata, c->udata_len < 48 ? c->udata_len : 48);
    }
    return false;
}

/* The embedded field last heard for voice position n, or NULL. */
static const uint8_t *frag(const rdof_txcall_t *c, int n)
{
    return n >= 1 && n <= 4 && ((c->has_frag >> n) & 1) ? c->frags[n] : NULL;
}

/* Silence for the call's next voice position (R5.10, simplex framing). grow: an underrun the jitter buffer grows by,
 * so the stream's later bursts go out a position on, not dropped (R6.4). */
static uint8_t silence(rdof_txcall_t *c, uint8_t out[RDOF_BURST_BYTES], bool grow)
{
    int n = c->vpos;
    uint8_t control = rdof_silence_burst(n, c->tune.cc, false, frag(c, n), out);
    c->vpos = (n + 1) % 6;
    if (grow)
        c->vshift = (c->vshift + 1) % 6;
    else
        c->vfilled++;
    return control;
}

/* Bursts in hand ahead of the next one: queued, and what the radio still holds. Called locked. */
static double ahead(const rdof_txcall_t *c, double n)
{
    double held = c->air_seen - n;
    return c->count + (held > 0 ? held / RDOF_TXVR_BURST_S : 0.0);
}

/* A voice burst's place in the call (section 6): from the first A (R6.5), a gap filled with silence (R6.2), a
 * burst whose position went out as silence dropped (R6.4). Adaptive (jitter buffer): positions moved by vshift, the
 * gap filled only up to the target, a burst dropped when the stream is ahead of it. */
static void voice(rdof_txvr_t *t, rdof_txcall_t *c, const rdof_net_burst_t *q)
{
    int pos = (q->control & RDOF_SYNC_AUDIO) ? 0 : (q->control & 0x0F), gap;
    if (pos > 5) {
        push(c, q);
        return;
    }
    if (c->vpos < 0) {
        if (pos != 0)
            return;
        c->vpos = c->vfilled = 0;
    }
    if (pos >= 1 && pos <= 4) {
        for (int i = 0; i < 32; i++)
            c->frags[pos][i] = (uint8_t)rdof_get_field(q->burst, 116 + i, 1);
        c->has_frag |= (uint8_t)(1 << pos);
    }
    int tpos = (pos + c->vshift) % 6; /* where it goes out */
    if (c->vfilled >= 6) {
        gap = ((tpos - c->vpos) % 6 + 6) % 6;
    } else {
        int d = ((tpos - (c->vpos - c->vfilled)) % 6 + 12) % 6;
        if (d < c->vfilled)
            return;
        gap = d - c->vfilled;
    }
    if (t->jb.adaptive) {
        double n = now(t), a = ahead(c, n);
        int keep = rdof_jb_fill(&t->jb, &c->jbs, gap, a);
        if (keep < gap) { /* the silence past the target isn't needed */
            c->vshift = ((c->vshift - (gap - keep)) % 6 + 6) % 6;
            tpos = ((tpos - (gap - keep)) % 6 + 6) % 6;
            gap = keep;
        }
        if (!gap && rdof_jb_drop(&t->jb, &c->jbs, a, rdof_jb_silent(q->burst), n)) {
            c->vshift = (c->vshift + 5) % 6; /* its position goes to the next one */
            return;
        }
    }
    for (int i = 0; i < gap; i++) {
        rdof_net_burst_t s = *q;
        s.control = silence(c, s.burst, false);
        s.terminator = false;
        push(c, &s);
    }
    rdof_net_burst_t v = *q;
    v.terminator = false;
    if (tpos != pos) {
        int cc, pi, lcss;
        if (pos && rdof_decode_emb(q->burst, &cc, &pi, &lcss, NULL))
            c->vpi = pi;
        v.control = rdof_jb_reframe(q->burst, tpos, c->tune.cc, false, c->vpi, frag(c, tpos), v.burst);
    }
    push(c, &v);
    c->vpos = (tpos + 1) % 6;
    c->vfilled = 0;
}

/* at: when it came, to measure the network by; < 0 not measured (held for its DMRT). */
static void add(rdof_txvr_t *t, rdof_txcall_t *c, const rdof_net_burst_t *b, double at)
{
    rdof_jb_key(&t->jb, &c->jbs, b->dst, b->group, b->slot, c->started);
    if (at >= 0)
        rdof_jb_arrival(&t->jb, &c->jbs, at, b->seq);
    if (!c->src && b->src) {
        c->src = b->src;
        c->dst = b->dst;
        c->group = b->group;
    }
    if (!c->wants_reply && asks_reply(c, b))
        c->wants_reply = true;
    rdof_net_burst_t q = *b;
    rdof_regenerate(b->control, b->burst, c->tune.cc, false, q.burst);
    if (b->control == CSBK_CONTROL && b->terminator && c->count == 0 && !c->started) {
        /* A CSBK on its own: a short lead-in of preamble CSBKs, then it. */
        c->has_csbk = true;
        memcpy(c->csbk, b->burst, RDOF_BURST_BYTES);
        int n = upto(t->csbk_preambles, 16);
        uint8_t pre[16][RDOF_BURST_BYTES];
        rdof_csbk_preambles(b->src, b->dst, b->group, n, c->tune.cc, pre);
        for (int i = 0; i < n; i++) {
            rdof_net_burst_t p = *b;
            p.control = CSBK_CONTROL;
            memcpy(p.burst, pre[i], RDOF_BURST_BYTES);
            p.terminator = false;
            push(c, &p);
        }
    }
    if (b->control == HEADER_CONTROL && !c->has_lc) {
        uint8_t lc12[12];
        if (rdof_bptc196_decode(b->burst, lc12)) {
            memcpy(c->lc9, lc12, 9);
            c->has_lc = true;
        }
    }
    int copies = 1; /* R11.4: Hs headers and Ts terminators a call, however many come */
    if (b->control == HEADER_CONTROL) {
        int hs = upto(t->radio->headers, RDOF_TXVR_QUEUE); /* past the queue: a loop that only drops */
        hs = hs > 1 ? hs : 1;
        copies = hs > c->headers ? hs - c->headers : 0;
        c->headers += copies;
        if (!c->started && copies)
            c->extra += copies - 1;
    } else if (b->control == TERMINATOR_CONTROL) {
        int ts = upto(t->radio->terminators, RDOF_TXVR_QUEUE);
        ts = ts > 1 ? ts : 1;
        copies = ts > c->terminators ? ts - c->terminators : 0;
        c->terminators += copies;
    }
    if (!(b->control & RDOF_SYNC_DATA)) { /* section 6 (R11.5) */
        voice(t, c, &q);
        copies = 0;
    }
    for (int i = 0; i < copies; i++)
        push(c, &q);
    c->last_burst = now(t);
    if (b->terminator)
        c->ended = true;
}

void rdof_txvr_on_tune(rdof_txvr_t *t, const rdof_tune_t *in)
{
    rdof_tune_t tune = *in;
    lock(t);
    if (t->has_call && !memcmp(t->call.tune.stream_id, tune.stream_id, 4)) {
        unlock(t);
        return; /* a repeat */
    }
    if (t->has_call) {
        answer(t, &tune, RDOF_DMRK_TXVR_BUSY);
        unlock(t);
        return;
    }
    if (!t->ready) {
        answer(t, &tune, RDOF_DMRK_RADIO_ERROR);
        unlock(t);
        return;
    }
    if (now(t) - modem(t)->last_rx < RDOF_TXVR_RX_HOLD_S && t->has_tuned && !near(t->tuned_freq, tune.freq)) {
        if (!tune.preempt) {
            answer(t, &tune, RDOF_DMRK_TXVR_BUSY); /* receiving a call here */
            unlock(t);
            return;
        }
    }
    const rdof_channel_t *ch = allowed(t, tune.freq, tune.cc);
    if (!ch) {
        answer(t, &tune, RDOF_DMRK_NOT_ALLOWED);
        unlock(t);
        return;
    }
    /* HBlink4 sends power 255 for "the radio's own"; a channel's own setting wins. */
    if (tune.power == 255 && ch->power != 255)
        tune.power = ch->power;
    memset(&t->call, 0, sizeof t->call);
    t->call.tune = tune;
    t->call.vpos = -1;
    t->call.state = RDOF_CALL_PENDING;
    t->call.last_burst = now(t);
    t->call.group = true;
    t->has_call = true;
    rdof_jb_begin(&t->jb, &t->call.jbs);
    for (int i = 0; i < RDOF_TXVR_ORPHANS; i++)
        if (t->orphans[i].used && !memcmp(t->orphans[i].stream_id, tune.stream_id, 4)) {
            t->orphans[i].used = false;
            for (int k = 0; k < upto(t->orphans[i].n, RDOF_TXVR_ORPHAN_BURSTS); k++)
                add(t, &t->call, &t->orphans[i].bursts[k], -1);
        }
    unlock(t);
}

static bool was_refused(const rdof_txvr_t *t, const uint8_t sid[4])
{
    for (int i = 0; i < upto(t->nrefused, RDOF_TXVR_REFUSED); i++)
        if (!memcmp(t->refused[i], sid, 4))
            return true;
    return false;
}

void rdof_txvr_on_burst(rdof_txvr_t *t, const rdof_net_burst_t *b)
{
    lock(t);
    if (t->has_call && !memcmp(t->call.tune.stream_id, b->stream_id, 4)) {
        if (t->call.state != RDOF_CALL_DONE)
            add(t, &t->call, b, now(t));
        unlock(t);
        return;
    }
    if (was_refused(t, b->stream_id)) {
        unlock(t);
        return;
    }
    int slot = -1, free_slot = -1, oldest = 0;
    for (int i = 0; i < RDOF_TXVR_ORPHANS; i++) {
        if (t->orphans[i].used && !memcmp(t->orphans[i].stream_id, b->stream_id, 4))
            slot = i;
        else if (!t->orphans[i].used && free_slot < 0)
            free_slot = i;
        if (t->orphans[i].at < t->orphans[oldest].at)
            oldest = i;
    }
    if (slot < 0) {
        slot = free_slot >= 0 ? free_slot : oldest;
        t->orphans[slot].used = true;
        memcpy(t->orphans[slot].stream_id, b->stream_id, 4);
        t->orphans[slot].at = now(t);
        t->orphans[slot].n = 0;
    }
    if (t->orphans[slot].n < 0)
        t->orphans[slot].n = 0;
    if (t->orphans[slot].n < RDOF_TXVR_ORPHAN_BURSTS)
        t->orphans[slot].bursts[t->orphans[slot].n++] = *b;
    unlock(t);
}

void rdof_txvr_heard_call(rdof_txvr_t *t, const char *action, uint32_t src, uint32_t dst)
{
    if (!strcmp(action, "start") || !strcmp(action, "late_entry")) {
        t->has_heard_call = true;
        t->heard_call_ended = false;
        t->heard_src = src;
        t->heard_dst = dst;
    } else if ((!strcmp(action, "end") || !strcmp(action, "lost")) && t->has_heard_call) {
        t->heard_call_ended = true;
        t->heard_at = now(t);
    }
}

/* ── where it is ───────────────────────────────────────────────────────────── */

bool rdof_txvr_on_idle_channel(const rdof_txvr_t *t)
{
    return t->has_tuned && t->has_idle && near(t->tuned_freq, t->idle.freq) && t->tuned_cc == t->idle.cc;
}

static int scan_index(const rdof_txvr_t *t)
{
    if (!t->has_scan || !t->has_tuned)
        return -1;
    for (int i = 0; i < upto(t->scan.nchannels, RDOF_TXVR_MAX_CHANNELS); i++)
        if (near(t->scan.channels[i].freq, t->tuned_freq) && t->scan.channels[i].cc == t->tuned_cc)
            return i;
    return -1;
}

static bool awaiting_answer(rdof_txvr_t *t)
{
    return t->has_answer_window && t->has_tuned && now(t) < t->aw_until && near(t->tuned_freq, t->aw_freq) &&
           t->tuned_cc == t->aw_cc;
}

static bool is_covered(const rdof_txvr_t *t, uint32_t freq)
{
    for (int i = 0; i < upto(t->ncovered, RDOF_TXVR_MAX_COVERED); i++)
        if (near(freq, t->covered[i]))
            return true;
    return false;
}

bool rdof_txvr_listening(const rdof_txvr_t *t)
{
    return rdof_txvr_on_idle_channel(t) || scan_index(t) >= 0 || awaiting_answer((rdof_txvr_t *)t);
}

bool rdof_txvr_scanning(const rdof_txvr_t *t)
{
    return t->has_scan && !t->has_call && !t->stopped;
}

const char *rdof_txvr_state_name(const rdof_txvr_t *t)
{
    if (!t->has_call)
        return "idle";
    switch (t->call.state) {
    case RDOF_CALL_PENDING: return "pending";
    case RDOF_CALL_LISTENING: return "listening";
    case RDOF_CALL_ON_AIR: return "on_air";
    default: return "done";
    }
}

void rdof_txvr_heard(rdof_txvr_t *t, int slot, uint8_t control, const uint8_t burst[RDOF_BURST_BYTES], int rssi)
{
    if (slot != 2)
        return; /* a simplex modem passes up slot 2 (on_rx) */
    if (t->tuning || !t->receiver.hear || !rdof_txvr_listening(t) || (t->has_call && t->call.state == RDOF_CALL_ON_AIR))
        return;
    uint32_t freq = t->tuned_freq;
    int cc = t->tuned_cc;
    if (t->has_answer_window && awaiting_answer(t)) { /* a reply (or more of one): stay a little longer */
        double until = now(t) + t->answer_s;
        t->aw_until = until < t->aw_cap ? until : t->aw_cap;
    }
    int dbm;
    double d = rdof_radio_rssi(t->radio, rssi, freq, &dbm) ? (double)dbm : NAN;
    t->receiver.hear(t->receiver.ctx, 2, control, burst, d, cc, freq);
}

/* ── the worker ────────────────────────────────────────────────────────────── */

static bool tune_to(rdof_txvr_t *t, uint32_t freq, int cc, int power, bool hop, bool *retuned)
{
    *retuned = false;
    if (t->has_tuned && t->tuned_freq == freq && t->tuned_cc == cc && t->tuned_power == power)
        return true;
    t->tuning = true;
    bool ok = rdof_radio_tune(t->radio, freq, cc, power);
    if (ok) {
        t->has_tuned = true;
        t->tuned_freq = freq;
        t->tuned_cc = cc;
        t->tuned_power = power;
        t->tuned_at = now(t);
    }
    t->tuning = false;
    if (!ok)
        return false;
    t->carrier_checked = false;
    *retuned = true;
    if (!hop && t->on_retune)
        t->on_retune(t->ctx, freq, cc);
    return true;
}

bool rdof_txvr_start(rdof_txvr_t *t)
{
    bool r;
    t->has_tuned = false;
    if (!rdof_modem_set_mode(modem(t), RDOF_MODE_DMR))
        return false;
    if (t->has_idle && !tune_to(t, t->idle.freq, t->idle.cc, t->idle.power, false, &r))
        return false;
    t->ready = true;
    return true;
}

static void tx_event(rdof_txvr_t *t, const char *action, rdof_txcall_t *c)
{
    if (!t->on_tx)
        return;
    rdof_tx_call_info_t info;
    rdof_txvr_call_info(t, c, &info);
    double d = 0.0;
    if (!strcmp(action, "end")) {
        d = now(t) - c->on_air_at;
        if (d < 0)
            d = 0;
    }
    t->on_tx(t->ctx, action, &info, d);
}

static void finish(rdof_txvr_t *t)
{
    t->idle_since = now(t);
    if (t->has_call) {
        bool was_on_air = t->call.state == RDOF_CALL_ON_AIR;
        t->call.state = RDOF_CALL_DONE;
        rdof_jb_end(&t->jb, &t->call.jbs);
        t->has_call = false;
        if (was_on_air)
            tx_event(t, "end", &t->call);
    }
}

void rdof_txvr_call_info(const rdof_txvr_t *t, const rdof_txcall_t *c, rdof_tx_call_info_t *info)
{
    memset(info, 0, sizeof *info);
    memcpy(info->stream_id, c->tune.stream_id, 4);
    info->src = c->src;
    info->dst = c->dst;
    info->group = c->group;
    info->freq = c->tune.freq;
    info->cc = c->tune.cc;
    info->slot = 2;
    info->has_csbk = c->has_csbk;
    memcpy(info->csbk, c->csbk, RDOF_BURST_BYTES);
    info->frames = -1; /* data_frames(): from the bursts still queued */
    for (int i = 0; i < c->count; i++) {
        const rdof_net_burst_t *b = &c->bursts[(c->head + i) % RDOF_TXVR_QUEUE];
        if (!(b->control & RDOF_SYNC_DATA))
            break;
        int dt = b->control & 0x0F;
        if (dt == RDOF_DT_DATA_HEADER) {
            rdof_data_header_t h;
            rdof_decode_data_header(b->burst, &h);
            info->frames = h.crc_ok ? h.blocks : 0;
            break;
        }
        if (dt == RDOF_DT_VOICE_LC_HEADER)
            break;
    }
}

static bool status(rdof_txvr_t *t, rdof_mmdvm_status_t *st)
{
    return rdof_modem_status(modem(t), st);
}

static void start_call(rdof_txvr_t *t, rdof_txcall_t *c, bool *err)
{
    bool retuned;
    t->has_answer_window = false;
    if (!tune_to(t, c->tune.freq, c->tune.cc, c->tune.power, false, &retuned)) {
        *err = true;
        return;
    }
    double n = now(t);
    c->state = RDOF_CALL_LISTENING;
    c->listen_until = n + t->listen_s;
    /* Already on the channel: what we heard lately counts too. */
    if (retuned)
        c->quiet_since = t->tuned_at;
    else
        c->quiet_since = n - t->clear_s > t->tuned_at ? n - t->clear_s : t->tuned_at;
}

static double quiet_since(const rdof_txvr_t *t, const rdof_txcall_t *c)
{
    /* A reply to the call just heard here (its target answering its source) doesn't count that call. */
    if (t->has_heard_call && t->heard_call_ended && c->src && t->heard_src == c->dst && t->heard_dst == c->src) {
        double tail = t->heard_at + RDOF_TXVR_CALL_TAIL_S;
        return c->quiet_since > tail ? c->quiet_since : tail;
    }
    return c->quiet_since;
}

static void listen(rdof_txvr_t *t, rdof_txcall_t *c, double n)
{
    bool busy = modem(t)->last_rx > quiet_since(t, c);
    if (!busy && n - t->last_status >= RDOF_TXVR_STATUS_EVERY_S) {
        /* Carrier detect, where the modem has it: anything on the channel, DMR or not. */
        rdof_mmdvm_status_t st;
        t->last_status = n;
        if (status(t, &st))
            busy = st.carrier != 0;
    }
    if (busy) {
        lock(t);
        answer(t, &c->tune, RDOF_DMRK_CHANNEL_BUSY);
        finish(t);
        unlock(t);
        return;
    }
    if (n < c->listen_until)
        return;
    lock(t);
    answer(t, &c->tune, RDOF_DMRK_ON_AIR);
    c->state = RDOF_CALL_ON_AIR;
    c->on_air_at = n;
    unlock(t);
    tx_event(t, "start", c);
}

static bool after_call(rdof_txvr_t *t, double n);

static bool write_burst(rdof_txvr_t *t, rdof_txcall_t *c, const rdof_net_burst_t *b, double n)
{
    if (!rdof_modem_write_dmr(modem(t), b->control, b->burst, 2))
        return false;
    t->space--;
    t->air_until = (t->air_until > n ? t->air_until : n) + RDOF_TXVR_BURST_S;
    c->started = true;
    if (t->on_sent) {
        rdof_tx_call_info_t info;
        rdof_txvr_call_info(t, c, &info);
        t->on_sent(t->ctx, &info, b);
    }
    return true;
}

/* R7.8, R11.5: a stream lost before its terminator ends as if it had come, so the radios leave the call now and not
 * on their own fade timers. Only a voice call: other streams have no terminator. Called locked. */
static void timed_out(rdof_txvr_t *t, rdof_txcall_t *c)
{
    c->ended = true;
    if (c->vpos < 0 || c->terminators)
        return;
    uint8_t lc9[9];
    if (c->has_lc)
        memcpy(lc9, c->lc9, 9);
    else
        rdof_group_lc(c->src, c->dst, c->group, 0, lc9);
    rdof_net_burst_t term = {.control = TERMINATOR_CONTROL, .terminator = true};
    memcpy(term.stream_id, c->tune.stream_id, sizeof term.stream_id);
    rdof_lc_burst(lc9, RDOF_DT_TERMINATOR_WITH_LC, c->tune.cc, rdof_data_sync(RDOF_MS_VOICE), term.burst);
    c->terminators = upto(t->radio->terminators, RDOF_TXVR_QUEUE);
    if (c->terminators < 1)
        c->terminators = 1;
    for (int i = 0; i < c->terminators; i++)
        push(c, &term);
}

static bool play(rdof_txvr_t *t, rdof_txcall_t *c, double n)
{
    lock(t);
    if (!c->ended && n - c->last_burst > RDOF_TXVR_STREAM_TIMEOUT_S)
        timed_out(t, c);
    int waiting = c->count;
    bool ended = c->ended;
    unlock(t);
    if (!c->started && waiting < rdof_jb_prebuffer(&t->jb, &c->jbs, t->prebuffer) + c->extra && !ended)
        return true; /* a little in hand, so the radio doesn't run dry */
    /* R6.3: the stream ran dry mid-call: silence at the next position while the radio holds one burst or less. */
    bool underrun = !waiting && !ended && c->started && c->vpos >= 0 && t->air_until - n <= RDOF_TXVR_BURST_S + 1e-6;
    if (waiting || underrun) {
        if (n - t->last_status >= RDOF_TXVR_STATUS_EVERY_S || t->space <= 1) {
            rdof_mmdvm_status_t st;
            if (status(t, &st)) {
                t->space = st.space;
                t->last_status = n;
                t->has_status_missed = false;
            } else {
                t->status_misses++;
                if (!t->has_status_missed) {
                    t->has_status_missed = true;
                    t->status_missed = n;
                } else if (n - t->status_missed > RDOF_TXVR_STATUS_GIVE_UP_S) {
                    return false;
                }
                t->space = 0; /* try again next poll */
                return true;
            }
        }
        if (underrun) {
            if (t->space > 1) {
                rdof_net_burst_t b;
                memset(&b, 0, sizeof b);
                memcpy(b.stream_id, c->tune.stream_id, 4);
                b.group = true; /* a network burst's defaults */
                b.slot = 2;
                lock(t);
                b.control = silence(c, b.burst, rdof_jb_underrun(&t->jb, &c->jbs));
                c->air_seen = (t->air_until > n ? t->air_until : n) + RDOF_TXVR_BURST_S;
                unlock(t);
                if (!write_burst(t, c, &b, n))
                    return false;
            }
            return true;
        }
        while (t->space > 1) { /* R10.2: the modem's last free place is never used */
            rdof_net_burst_t b;
            lock(t);
            if (!c->count) {
                unlock(t);
                break;
            }
            pop(c, &b);
            c->air_seen = (t->air_until > n ? t->air_until : n) + RDOF_TXVR_BURST_S;
            unlock(t);
            if (!write_burst(t, c, &b, n))
                return false;
        }
        return true;
    }
    if (ended) {
        /* All handed over, but the radio still has up to 48 bursts (~3 s) to send: retuning now would cut the end off. */
        if (n - t->last_status < RDOF_TXVR_STATUS_EVERY_S)
            return true;
        t->last_status = n;
        rdof_mmdvm_status_t st;
        if (!status(t, &st))
            return false;
        if (st.transmitting) {
            c->has_quiet_at = false;
            return true;
        }
        if (!c->has_quiet_at) {
            c->has_quiet_at = true;
            c->quiet_at = n;
        }
        if (!c->has_csbk && !c->wants_reply && n - c->quiet_at < RDOF_TXVR_TX_TAIL_S)
            return true; /* a voice call's terminators may still be going out */
        bool wants = c->wants_reply;
        uint32_t freq = c->tune.freq;
        int cc = c->tune.cc;
        lock(t);
        finish(t);
        unlock(t);
        if (wants && t->answer_s > 0 && !is_covered(t, freq)) {
            /* Back to receive at once, on its channel, for the reply; no CW ID now (the next call's). */
            double tt = now(t);
            t->has_answer_window = true;
            t->aw_freq = freq;
            t->aw_cc = cc;
            t->aw_until = tt + t->answer_s;
            t->aw_cap = tt + t->answer_max_s;
            return true;
        }
        return after_call(t, n);
    }
    return true;
}

static bool wait_until_quiet(rdof_txvr_t *t)
{
    rdof_modem_t *m = modem(t);
    for (int i = 0; i < 150; i++) { /* 15 s at most */
        rdof_mmdvm_status_t st;
        if (!status(t, &st))
            return false;
        if (!st.transmitting)
            return true;
        if (m->port.sleep_ms)
            m->port.sleep_ms(m->port.ctx, 100);
    }
    return true;
}

static bool after_call(rdof_txvr_t *t, double n)
{
    if (!t->cwid[0] || !t->has_tuned)
        return true;
    uint32_t freq = t->tuned_freq;
    const int nid = upto(t->nlast_id, RDOF_TXVR_IDS);
    int k;
    for (k = 0; k < nid; k++)
        if (t->last_id[k].freq == freq)
            break;
    if (k < nid && n - t->last_id[k].at < t->cwid_every_s)
        return true;
    if (!wait_until_quiet(t) || !rdof_modem_set_mode(modem(t), RDOF_MODE_IDLE) || /* the radio sends CW only from idle */
        !rdof_modem_send_cwid(modem(t), t->cwid) || !wait_until_quiet(t) || !rdof_modem_set_mode(modem(t), RDOF_MODE_DMR))
        return false;
    if (k == nid) {
        if (nid < RDOF_TXVR_IDS)
            t->nlast_id = nid + 1;
        else /* full: the channel identified longest ago */
            for (int i = k = 0; i < RDOF_TXVR_IDS; i++)
                if (t->last_id[i].at < t->last_id[k].at)
                    k = i;
    }
    t->last_id[k].freq = freq;
    t->last_id[k].at = now(t);
    return true;
}

static bool scan_step(rdof_txvr_t *t, double n)
{
    rdof_scan_t *sc = &t->scan;
    const int nch = upto(sc->nchannels, RDOF_TXVR_MAX_CHANNELS);
    if (!nch)
        return true; /* nothing to scan (the % below divided by 0) */
    int here = scan_index(t);
    bool r;
    if (here >= 0) {
        rdof_modem_t *m = modem(t);
        bool heard = m->last_rx > t->tuned_at;
        if (heard && n - m->last_rx < sc->hang_s) {
            if (!t->stopped) {
                t->stopped = true;
                if (t->on_scan)
                    t->on_scan(t->ctx, true);
            }
            return true;
        }
        if (t->stopped) {
            t->stopped = false;
            if (t->on_scan)
                t->on_scan(t->ctx, false);
        } else if (!heard) {
            double dwell = n - t->tuned_at;
            if (sc->carrier_s > 0 && !t->carrier_checked && dwell >= sc->carrier_s) {
                rdof_mmdvm_status_t st;
                t->carrier_checked = true;
                t->last_status = n;
                if (!status(t, &st))
                    return false;
                if (!st.carrier)
                    dwell = sc->dwell_s; /* nothing there: move on now */
            }
            if (dwell < sc->dwell_s)
                return true;
        }
        t->scan_next = (here + 1) % nch;
    }
    if (t->scan_next < 0 || t->scan_next >= nch)
        t->scan_next = 0;
    const rdof_channel_t *ch = &sc->channels[t->scan_next];
    t->scan_next = (t->scan_next + 1) % nch;
    return tune_to(t, ch->freq, ch->cc, ch->power, true, &r);
}

static bool idle(rdof_txvr_t *t, double n)
{
    bool r;
    if (t->has_answer_window && !awaiting_answer(t)) {
        t->has_answer_window = false;
        t->idle_since = n; /* back to the idle channel / scan from here */
    }
    if (t->has_answer_window) {
        /* listening for the answer to a lone CSBK */
    } else if (t->has_scan) {
        if (n - t->idle_since >= t->idle_after_s && !scan_step(t, n))
            return false;
    } else if (t->has_idle && !rdof_txvr_on_idle_channel(t) && n - t->idle_since >= t->idle_after_s) {
        if (!tune_to(t, t->idle.freq, t->idle.cc, t->idle.power, false, &r))
            return false;
    }
    if (n - t->last_status >= RDOF_TXVR_IDLE_STATUS_EVERY_S) {
        rdof_mmdvm_status_t st;
        if (!status(t, &st))
            return false;
        t->last_status = n;
    }
    return true;
}

bool rdof_txvr_poll(rdof_txvr_t *t)
{
    double n = now(t);
    lock(t);
    for (int i = 0; i < RDOF_TXVR_ORPHANS; i++)
        if (t->orphans[i].used && n - t->orphans[i].at > RDOF_TXVR_ORPHAN_S)
            t->orphans[i].used = false;
    bool has_call = t->has_call;
    rdof_txcall_state_t state = t->call.state;
    unlock(t);
    if (t->receiver.tick)
        t->receiver.tick(t->receiver.ctx);
    bool ok = true, err = false;
    if (!has_call)
        ok = idle(t, n);
    else if (state == RDOF_CALL_PENDING) {
        start_call(t, &t->call, &err);
        ok = !err;
    } else if (state == RDOF_CALL_LISTENING)
        listen(t, &t->call, n);
    else if (state == RDOF_CALL_ON_AIR)
        ok = play(t, &t->call, n);
    if (!ok) {
        t->has_tuned = false; /* unknown now: retune next time */
        t->ready = false;
        lock(t);
        if (t->has_call && (t->call.state == RDOF_CALL_PENDING || t->call.state == RDOF_CALL_LISTENING))
            answer(t, &t->call.tune, RDOF_DMRK_RADIO_ERROR);
        finish(t);
        unlock(t);
    }
    return ok;
}

#endif /* RDOF_FEATURE_TX */
