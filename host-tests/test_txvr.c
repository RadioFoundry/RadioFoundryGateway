// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The roaming transceiver against the golden scenarios (scenarios.h). */
#include <math.h>

#include "harness.h"
#include "rdof/features.h"

#if RDOF_FEATURE_TX && RDOF_FEATURE_TRACKER

#include "rdof/bits.h"
#include "rdof/fec.h"
#include "rdof/hblink.h"
#include "rdof/json.h"
#include "rdof/layer2.h"
#include "rdof/mmdvm.h"
#include "rdof/netcalls.h"
#include "rdof/pdu.h"
#include "rdof/tracker.h"
#include "rdof/txvr.h"
#include "scenarios.h"

#define PEER 9990301

typedef struct {
    fake_rig_t rig;
    expect_t e;
    rdof_radio_t radio;
    rdof_txvr_t txvr;
    rdof_tracker_t tracker;
    rdof_hb_sink_t hb;
    rdof_netcalls_t calls;
    int rand_k;
    char state[96];
} tx_harness_t;

static void out_json(void *ctx, const char *ch, const char *json) { expect(&((tx_harness_t *)ctx)->e, "json", json); }
static void out_raw(void *ctx, const char *ch, const char *json) { expect(&((tx_harness_t *)ctx)->e, "raw", json); }

static void out_call(void *ctx, const char *ch, const char *action, uint32_t src, uint32_t dst)
{
    rdof_txvr_heard_call(&((tx_harness_t *)ctx)->txvr, action, src, dst);
}

static void send_pkt(void *ctx, const uint8_t *d, size_t n)
{
    char hex[400];
    hexs(d, n, hex);
    expect(&((tx_harness_t *)ctx)->e, "send", hex);
}

static uint32_t hb_rand(void *ctx)
{
    tx_harness_t *h = ctx;
    return (uint32_t)sc_stream_ids[h->rand_k++] - 1;
}

static void rx_hear(void *ctx, int slot, uint8_t control, const uint8_t *burst, double dbm, int cc, uint32_t freq)
{
    tx_harness_t *h = ctx;
    rdof_burst_t b;
    if (freq)
        h->tracker.freq_hz = freq;
    rdof_modem_burst(control, burst, dbm, cc, slot, &b);
    rdof_tracker_feed(&h->tracker, &b, h->rig.clock);
}

static void rx_tick(void *ctx)
{
    tx_harness_t *h = ctx;
    rdof_tracker_tick(&h->tracker, h->rig.clock);
}

static void on_retune(void *ctx, uint32_t freq, int cc)
{
    char s[40];
    snprintf(s, sizeof s, "%lu,%d", (unsigned long)freq, cc);
    expect(&((tx_harness_t *)ctx)->e, "retune", s);
}

static void on_scan(void *ctx, bool holding)
{
    expect(&((tx_harness_t *)ctx)->e, "scan", holding ? "1" : "0");
}

static void on_tx(void *ctx, const char *action, const rdof_tx_call_info_t *c, double duration)
{
    tx_harness_t *h = ctx;
    char s[200], d[32], json[1024];
    rdof_format_float3(duration, d, sizeof d);
    snprintf(s, sizeof s, "%s,%lu,%lu,%d,%lu,%d,%s", action, (unsigned long)c->src, (unsigned long)c->dst, c->group,
             (unsigned long)c->freq, c->cc, d);
    expect(&h->e, "tx", s);
    rdof_tx_json(action, c, duration, h->rig.clock, json, sizeof json);
    expect(&h->e, "txjson", json);
    if (rdof_netcalls_event(&h->calls, action, c, duration, h->rig.clock, json, sizeof json))
        expect(&h->e, "njson", json);
}

static void on_sent(void *ctx, const rdof_tx_call_info_t *c, const rdof_net_burst_t *b)
{
    tx_harness_t *h = ctx;
    char json[1024];
    if (rdof_netcalls_burst(&h->calls, c, b->control, b->burst, h->rig.clock, json, sizeof json))
        expect(&h->e, "nraw", json);
}

static void modem_rx(void *ctx, int slot, uint8_t control, const uint8_t *burst, int rssi)
{
    rdof_txvr_heard(&((tx_harness_t *)ctx)->txvr, slot, control, burst, rssi);
}

static double txvr_clock(void *ctx)
{
    return ((tx_harness_t *)ctx)->rig.clock;
}

static void state(tx_harness_t *h)
{
    char s[96];
    const rdof_txvr_t *t = &h->txvr;
    snprintf(s, sizeof s, "%s,%lu,%d,%d,%d,%d", rdof_txvr_state_name(t), (unsigned long)(t->has_tuned ? t->tuned_freq : 0),
             t->has_tuned ? t->tuned_cc : 0, rdof_txvr_listening(t), rdof_txvr_scanning(t), t->ready);
    if (strcmp(s, h->state)) {
        strcpy(h->state, s);
        expect(&h->e, "state", s);
    }
}

static int parse_channels(const char *s, rdof_channel_t *out, int max)
{
    int n = 0;
    while (*s && n < max) {
        unsigned long f;
        int cc, p = 255, used = 0;
        if (sscanf(s, "%lu:%d:%d%n", &f, &cc, &p, &used) < 3) {
            p = 255;
            if (sscanf(s, "%lu:%d%n", &f, &cc, &used) < 2)
                break;
        }
        out[n].freq = (uint32_t)f;
        out[n].cc = cc;
        out[n].power = p;
        n++;
        s += used;
        if (*s == ';')
            s++;
    }
    return n;
}

static void run(const struct sc_txvr *sc)
{
    static tx_harness_t h;
    memset(&h, 0, sizeof h);
    h.e.name = sc->name;
    h.e.outs = (const struct expect_out *)sc->outs;
    h.e.nouts = sc->nouts;
    rig_init(&h.rig, sc->kind, &h.e);
    rdof_modem_start(&h.rig.modem, 1);
    rdof_radio_init(&h.radio, &h.rig.modem, kind_of(sc->rkind));
    h.radio.fixed_hz = (uint32_t)sc->fixed_hz;
    rdof_txvr_t *t = &h.txvr;
    rdof_txvr_init(t, &h.radio, PEER);
    t->nchannels = parse_channels(sc->channels, t->channels, RDOF_TXVR_MAX_CHANNELS);
    if (sc->idle_freq) {
        t->has_idle = true;
        t->idle.freq = (uint32_t)sc->idle_freq;
        t->idle.cc = sc->idle_cc;
        t->idle.power = 255;
    }
    t->idle_after_s = sc->idle_after_s;
    t->listen_s = sc->listen_s;
    t->clear_s = sc->clear_s;
    t->prebuffer = sc->prebuffer;
    t->csbk_preambles = sc->csbk_preambles;
    t->answer_s = sc->answer_s;
    t->answer_max_s = sc->answer_max_s;
    snprintf(t->cwid, sizeof t->cwid, "%s", sc->cwid);
    t->cwid_every_s = sc->cwid_every_s;
    if (sc->scan[0]) {
        t->has_scan = true;
        rdof_scan_defaults(&t->scan);
        t->scan.nchannels = parse_channels(sc->scan, t->scan.channels, RDOF_TXVR_MAX_CHANNELS);
        t->scan.dwell_s = sc->dwell_s;
        t->scan.hang_s = sc->hang_s;
        t->scan.carrier_s = sc->carrier_s;
    }
    t->send = send_pkt;
    t->on_retune = on_retune;
    t->on_scan = on_scan;
    t->on_tx = on_tx;
    t->on_sent = on_sent;
    t->clock = txvr_clock;
    t->ctx = &h;
    rdof_netcalls_init(&h.calls);
    if (sc->receiver) {
        rdof_tracker_init(&h.tracker, "ROAM1", 0.0, 0.0);
        rdof_sink_t out = {out_json, out_raw, NULL, &h, out_call};
        rdof_tracker_add_sink(&h.tracker, out);
        rdof_hb_sink_init(&h.hb, PEER);
        h.hb.send = send_pkt;
        h.hb.rand32 = hb_rand;
        h.hb.ctx = &h;
        rdof_tracker_add_sink(&h.tracker, rdof_hb_sink_as_sink(&h.hb));
        t->receiver.hear = rx_hear;
        t->receiver.tick = rx_tick;
        t->receiver.ctx = &h;
    }
    h.rig.modem.on_rx = modem_rx;
    h.rig.modem.cb_ctx = &h;
    if (!rdof_txvr_start(t))
        expect(&h.e, "error", "");
    state(&h);
    for (int i = 0; i < sc->nsteps; i++) {
        const struct sc_step *st = &sc->steps[i];
        uint8_t buf[256];
        size_t n;
        switch (st->op) {
        case 'T': {
            rdof_tune_t tune;
            n = unhexs(st->a, buf);
            if (rdof_parse_dmrt(buf, n, &tune))
                rdof_txvr_on_tune(t, &tune);
            state(&h);
            break;
        }
        case 'D': {
            rdof_net_burst_t b;
            n = unhexs(st->a, buf);
            if (rdof_parse_dmrd(buf, n, &b))
                rdof_txvr_on_burst(t, &b);
            break;
        }
        case 'P':
            if (!rdof_txvr_poll(t))
                expect(&h.e, "error", "");
            state(&h);
            break;
        case 'A': h.rig.clock += st->d; break;
        case 'h':
            unhexs(st->a, buf);
            fake_hear(&h.rig.radio, (uint8_t)st->x, buf, (int)st->y, 2);
            rig_pump(&h.rig);
            state(&h);
            break;
        case 'H':
            unhexs(st->a, buf);
            rdof_txvr_heard(t, 2, (uint8_t)st->x, buf, -1);
            break;
        case 'd': fake_drain(&h.rig.radio); break;
        case 'c': h.rig.radio.carrier = st->x != 0; break;
        case 's': h.rig.radio.drop_status = (int)st->x; break;
        case 'b':
            h.rig.radio.band_lo = (uint32_t)st->x;
            h.rig.radio.band_hi = (uint32_t)st->y;
            break;
        case 'i':
            t->has_idle = true;
            t->idle.freq = (uint32_t)st->x;
            t->idle.cc = (int)st->y;
            t->idle.power = 255;
            t->idle_after_s = st->d;
            break;
        case 'v':
            t->covered[0] = (uint32_t)st->x;
            t->ncovered = 1;
            break;
        case 'm': t->answer_max_s = st->d; break;
        default: printf("unknown step %c\n", st->op);
        }
    }
    expect_done(&h.e);
}

/* ── edges the scenarios don't reach ─────────────────────────────────────────── */

#define CHECK(cond, ...)                                                                                     \
    do {                                                                                                     \
        checks++;                                                                                            \
        if (!(cond)) {                                                                                       \
            failures++;                                                                                      \
            printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                          \
            printf(__VA_ARGS__);                                                                             \
            printf("\n");                                                                                    \
        }                                                                                                    \
    } while (0)

#define CH_A 462562500u
#define CH_B 462587500u
#define SRC 9992001u
#define DST 91u

/* A transceiver on a fake OpenGD77 whose writes can fail, and what came out of it. */
typedef struct {
    fake_rig_t rig; /* first: the modem port's ctx */
    rdof_radio_t radio;
    rdof_txvr_t t;
    bool fail_writes;
    bool fail_bursts; /* DMR_DATA2 only */
    bool cw_keys;     /* SEND_CWID keys the fake up, as a real CW ID does */
    int cw_drop;      /* after SEND_CWID, this many status replies lost */
    int drain_after;  /* sleep_ms drains the fake on this sleep (0: never) */
    int sleeps;
    int locks, unlocks, depth, max_depth;
    int nsent;
    uint8_t pkt[64];
    size_t pkt_len;
    int retunes, scans, starts, ends, nbursts, nterms, nheard;
    double duration; /* the last end's */
    rdof_net_burst_t last;
} ex_t;

static bool ex_write(void *ctx, const uint8_t *d, size_t n)
{
    ex_t *e = ctx;
    if (e->fail_writes || (e->fail_bursts && n > 2 && d[2] == RDOF_MMDVM_DMR_DATA2))
        return false;
    bool cw = n > 2 && d[2] == RDOF_MMDVM_SEND_CWID;
    fake_write(&e->rig.radio, d, n);
    rig_pump(&e->rig);
    if (cw && e->cw_keys)
        e->rig.radio.transmitting = true;
    if (cw)
        e->rig.radio.drop_status = e->cw_drop;
    return true;
}

static void ex_sleep(void *ctx, int ms)
{
    ex_t *e = ctx;
    if (++e->sleeps == e->drain_after)
        fake_drain(&e->rig.radio);
}

static void ex_lock(void *ctx)
{
    ex_t *e = ctx;
    e->locks++;
    if (++e->depth > e->max_depth)
        e->max_depth = e->depth;
}

static void ex_unlock(void *ctx)
{
    ex_t *e = ctx;
    e->unlocks++;
    e->depth--;
}

static void ex_send(void *ctx, const uint8_t *d, size_t n)
{
    ex_t *e = ctx;
    e->nsent++;
    e->pkt_len = n < sizeof e->pkt ? n : sizeof e->pkt;
    memcpy(e->pkt, d, e->pkt_len);
}

static void ex_retune(void *ctx, uint32_t freq, int cc) { ((ex_t *)ctx)->retunes++; }
static void ex_scan(void *ctx, bool holding) { ((ex_t *)ctx)->scans++; }

static void ex_tx(void *ctx, const char *action, const rdof_tx_call_info_t *c, double duration)
{
    ex_t *e = ctx;
    if (!strcmp(action, "start"))
        e->starts++;
    else if (!strcmp(action, "end")) {
        e->ends++;
        e->duration = duration;
    }
}

static void ex_sent(void *ctx, const rdof_tx_call_info_t *c, const rdof_net_burst_t *b)
{
    ex_t *e = ctx;
    e->nbursts++;
    if (b->control == (RDOF_SYNC_DATA | RDOF_DT_TERMINATOR_WITH_LC))
        e->nterms++;
    e->last = *b;
}

static void ex_hear(void *ctx, int slot, uint8_t control, const uint8_t *burst, double dbm, int cc, uint32_t freq)
{
    ((ex_t *)ctx)->nheard++;
}

static void ex_rx(void *ctx, int slot, uint8_t control, const uint8_t *burst, int rssi)
{
    rdof_txvr_heard(&((ex_t *)ctx)->t, slot, control, burst, rssi);
}

static double ex_clock(void *ctx) { return ((ex_t *)ctx)->rig.clock; }

static ex_t *ex_new(bool idle)
{
    static ex_t e;
    memset(&e, 0, sizeof e);
    rig_init(&e.rig, "opengd77", NULL);
    rdof_modem_port_t port = {ex_write, rig_wait, NULL, rig_clock, ex_sleep, &e.rig};
    rdof_modem_init(&e.rig.modem, &port);
    rdof_modem_start(&e.rig.modem, 1);
    e.rig.modem.on_rx = ex_rx;
    e.rig.modem.cb_ctx = &e;
    rdof_radio_init(&e.radio, &e.rig.modem, RDOF_RADIO_HOTSPOT);
    rdof_txvr_t *t = &e.t;
    rdof_txvr_init(t, &e.radio, PEER);
    t->channels[0] = (rdof_channel_t){CH_A, 1, 255};
    t->channels[1] = (rdof_channel_t){CH_B, 1, 255};
    t->nchannels = 2;
    if (idle) {
        t->has_idle = true;
        t->idle = (rdof_channel_t){CH_B, 1, 255};
    }
    t->send = ex_send;
    t->on_retune = ex_retune;
    t->on_scan = ex_scan;
    t->on_tx = ex_tx;
    t->on_sent = ex_sent;
    t->clock = ex_clock;
    t->ctx = &e;
    return &e;
}

static rdof_tune_t ex_tune(uint32_t sid, uint32_t freq)
{
    rdof_tune_t tu;
    memset(&tu, 0, sizeof tu);
    rdof_put_be32(tu.stream_id, sid);
    tu.freq = freq;
    tu.cc = 1;
    tu.power = 255;
    return tu;
}

static rdof_net_burst_t ex_burst(uint32_t sid, uint8_t control, bool terminator)
{
    rdof_net_burst_t b;
    memset(&b, 0, sizeof b);
    rdof_put_be32(b.stream_id, sid);
    b.control = control;
    b.terminator = terminator;
    b.src = SRC;
    b.dst = DST;
    b.group = true;
    b.slot = 2;
    for (int i = 0; i < RDOF_BURST_BYTES; i++)
        b.burst[i] = (uint8_t)(i * 7 + control);
    return b;
}

static void ex_voice(ex_t *e, uint32_t sid, int superframes)
{
    for (int s = 0; s < superframes; s++)
        for (int p = 0; p < 6; p++) {
            rdof_net_burst_t b = ex_burst(sid, p ? (uint8_t)p : RDOF_SYNC_AUDIO, false);
            rdof_txvr_on_burst(&e->t, &b);
        }
}

/* The DMRK last sent is this answer. */
static bool ex_answered(const ex_t *e, uint32_t sid, int status, uint32_t freq)
{
    uint8_t want[RDOF_DMRK_LEN], s[4];
    rdof_put_be32(s, sid);
    rdof_dmrk_packet(PEER, s, status, freq, 1, want);
    return e->pkt_len == RDOF_DMRK_LEN && !memcmp(e->pkt, want, RDOF_DMRK_LEN);
}

/* Tuned, listened and keyed: on the air. */
static void ex_to_air(ex_t *e)
{
    rdof_txvr_poll(&e->t);
    e->rig.clock += 0.31;
    rdof_txvr_poll(&e->t);
}

/* Polls every 20 ms, the fake sending what it holds, until idle again (or n polls). */
static void ex_run(ex_t *e, int n)
{
    for (int i = 0; i < n && e->t.has_call; i++) {
        e->rig.clock += 0.02;
        rdof_txvr_poll(&e->t);
        fake_drain(&e->rig.radio);
    }
}

static int ex_orphan(const rdof_txvr_t *t, uint32_t sid)
{
    uint8_t s[4];
    rdof_put_be32(s, sid);
    for (int i = 0; i < RDOF_TXVR_ORPHANS; i++)
        if (t->orphans[i].used && !memcmp(t->orphans[i].stream_id, s, 4))
            return i;
    return -1;
}

static void t_txvr_call_edges(void)
{
    /* channel_now, and the lock taken and given back the same number of times, never nested */
    ex_t *e = ex_new(true);
    rdof_txvr_t *t = &e->t;
    uint32_t f;
    int cc;
    rdof_txvr_channel_now(t, &f, &cc);
    CHECK(f == 0 && cc == 0, "%lu %d", (unsigned long)f, cc);
    CHECK(rdof_txvr_start(t), "start");
    rdof_txvr_channel_now(t, &f, &cc);
    CHECK(f == CH_B && cc == 1, "%lu %d", (unsigned long)f, cc);
    t->lock = ex_lock;
    t->unlock = ex_unlock;
    rdof_tune_t tu = ex_tune(1, CH_A);
    rdof_txvr_on_tune(t, &tu);
    rdof_net_burst_t hdr = ex_burst(1, RDOF_SYNC_DATA | RDOF_DT_VOICE_LC_HEADER, false);
    uint8_t lc9[9];
    rdof_group_lc(SRC, DST, true, 0, lc9);
    rdof_lc_burst(lc9, RDOF_DT_VOICE_LC_HEADER, 1, rdof_data_sync(RDOF_MS_VOICE), hdr.burst);
    rdof_txvr_on_burst(t, &hdr);
    ex_voice(e, 1, 2);
    rdof_net_burst_t term = ex_burst(1, RDOF_SYNC_DATA | RDOF_DT_TERMINATOR_WITH_LC, true);
    rdof_txvr_on_burst(t, &term);
    ex_to_air(e);
    CHECK(!strcmp(rdof_txvr_state_name(t), "on_air"), "%s", rdof_txvr_state_name(t));
    rdof_txvr_channel_now(t, &f, &cc);
    CHECK(f == CH_A && cc == 1, "%lu %d", (unsigned long)f, cc);
    ex_run(e, 200);
    CHECK(!t->has_call && e->starts == 1 && e->ends == 1, "%d %d %d", t->has_call, e->starts, e->ends);
    CHECK(e->locks == e->unlocks && e->locks > 10 && e->max_depth == 1, "%d %d %d", e->locks, e->unlocks,
          e->max_depth);
    CHECK(e->nterms == e->radio.terminators, "%d terminators, want %d", e->nterms, e->radio.terminators);

    /* "done" while still the call: only the state name's default */
    t->has_call = true;
    t->call.state = RDOF_CALL_DONE;
    CHECK(!strcmp(rdof_txvr_state_name(t), "done"), "%s", rdof_txvr_state_name(t));
    t->has_call = false;

    /* the queue: 256 kept, the rest dropped and counted */
    e = ex_new(false);
    t = &e->t;
    rdof_txvr_start(t);
    tu = ex_tune(2, CH_A);
    rdof_txvr_on_tune(t, &tu);
    for (int i = 0; i < RDOF_TXVR_QUEUE + 44; i++) {
        rdof_net_burst_t b = ex_burst(2, RDOF_SYNC_DATA | RDOF_DT_RATE_12, false);
        rdof_txvr_on_burst(t, &b);
    }
    CHECK(t->call.count == RDOF_TXVR_QUEUE && t->call.dropped == 44, "%d %d", t->call.count, t->call.dropped);

    /* a voice control past F is queued as it is, before an A and after one, the position unchanged */
    e = ex_new(false);
    t = &e->t;
    rdof_txvr_start(t);
    tu = ex_tune(3, CH_A);
    rdof_txvr_on_tune(t, &tu);
    rdof_net_burst_t odd = ex_burst(3, 0x07, false);
    rdof_txvr_on_burst(t, &odd);
    CHECK(t->call.count == 1 && t->call.vpos == -1, "%d %d", t->call.count, t->call.vpos);
    rdof_net_burst_t a = ex_burst(3, RDOF_SYNC_AUDIO, false);
    rdof_txvr_on_burst(t, &a);
    odd.control = 0x06;
    rdof_txvr_on_burst(t, &odd);
    CHECK(t->call.count == 3 && t->call.vpos == 1, "%d %d", t->call.count, t->call.vpos);
    CHECK(t->call.bursts[2].control == 0x06, "%02x", t->call.bursts[2].control);

    /* after six or more silences, the next voice burst is placed from where the silence got to */
    e = ex_new(false);
    t = &e->t;
    rdof_txvr_start(t);
    t->prebuffer = 1;
    tu = ex_tune(4, CH_A);
    rdof_txvr_on_tune(t, &tu);
    a = ex_burst(4, RDOF_SYNC_AUDIO, false);
    rdof_txvr_on_burst(t, &a);
    ex_to_air(e);
    for (int i = 0; i < 10; i++) { /* dry: silence each burst time */
        e->rig.clock += RDOF_TXVR_BURST_S;
        rdof_txvr_poll(t);
    }
    CHECK(t->call.vfilled >= 6, "%d", t->call.vfilled);
    int v = t->call.vpos, before = t->call.count;
    rdof_net_burst_t c3 = ex_burst(4, 3, false); /* D */
    rdof_txvr_on_burst(t, &c3);
    int gap = ((3 - v) % 6 + 6) % 6;
    CHECK(t->call.count == before + gap + 1 && t->call.vpos == 4 && t->call.vfilled == 0, "%d %d %d %d (v %d)",
          t->call.count, before, t->call.vpos, t->call.vfilled, v);

    /* dry and the silence's write failing: an error, the call over (no on_tx to tell) */
    t->on_tx = NULL;
    e->fail_bursts = true;
    e->rig.clock += RDOF_TXVR_BURST_S;
    CHECK(!rdof_txvr_poll(t) && !t->has_call && e->ends == 0, "%d %d", t->has_call, e->ends);

    /* already on the channel: quiet counted from clear_s ago, not the tune */
    e = ex_new(true);
    t = &e->t;
    rdof_txvr_start(t);
    e->rig.clock += 10;
    tu = ex_tune(13, CH_B);
    rdof_txvr_on_tune(t, &tu);
    rdof_txvr_poll(t);
    CHECK(e->retunes == 1 && t->call.quiet_since == e->rig.clock - t->clear_s, "%d %f", e->retunes,
          t->call.quiet_since - e->rig.clock);
    /* back to the idle channel after a call elsewhere, and that refused by the radio */
    e = ex_new(true);
    t = &e->t;
    rdof_txvr_start(t);
    tu = ex_tune(14, CH_A);
    rdof_txvr_on_tune(t, &tu);
    ex_voice(e, 14, 1);
    rdof_net_burst_t end = ex_burst(14, RDOF_SYNC_DATA | RDOF_DT_TERMINATOR_WITH_LC, true);
    rdof_txvr_on_burst(t, &end);
    ex_to_air(e);
    ex_run(e, 200);
    CHECK(!t->has_call && t->tuned_freq == CH_A, "%lu", (unsigned long)t->tuned_freq);
    e->rig.radio.band_hi = 462570000;
    e->rig.clock += 0.02;
    CHECK(!rdof_txvr_poll(t) && !t->ready && !t->has_tuned, "idle retune");

    /* a voice call that times out with no header: terminators from its addressing, at least one */
    e = ex_new(false);
    t = &e->t;
    rdof_txvr_start(t);
    e->radio.terminators = 0;
    tu = ex_tune(5, CH_A);
    rdof_txvr_on_tune(t, &tu);
    ex_voice(e, 5, 1);
    ex_to_air(e);
    e->rig.clock += RDOF_TXVR_STREAM_TIMEOUT_S + 0.1;
    ex_run(e, 200);
    uint8_t want[RDOF_BURST_BYTES];
    rdof_lc_burst(lc9, RDOF_DT_TERMINATOR_WITH_LC, 1, rdof_data_sync(RDOF_MS_VOICE), want);
    CHECK(e->nterms == 1 && !memcmp(e->last.burst, want, RDOF_BURST_BYTES), "%d", e->nterms);
    CHECK(!t->has_call && e->ends == 1, "%d %d", t->has_call, e->ends);

    /* and far too many: only what the queue holds */
    e = ex_new(false);
    t = &e->t;
    rdof_txvr_start(t);
    e->radio.terminators = 100000;
    tu = ex_tune(6, CH_A);
    rdof_txvr_on_tune(t, &tu);
    ex_voice(e, 6, 1);
    ex_to_air(e);
    e->rig.clock += RDOF_TXVR_STREAM_TIMEOUT_S + 0.1;
    rdof_txvr_poll(t);
    CHECK(t->call.terminators == RDOF_TXVR_QUEUE && t->call.count + t->call.dropped + e->nbursts == 6 + RDOF_TXVR_QUEUE,
          "%d %d %d %d", t->call.terminators, t->call.count, t->call.dropped, e->nbursts);

    /* headers: as many as the radio wants, the queue at most, and none from a second */
    e = ex_new(false);
    t = &e->t;
    rdof_txvr_start(t);
    e->radio.headers = 100000;
    tu = ex_tune(7, CH_A);
    rdof_txvr_on_tune(t, &tu);
    hdr.stream_id[3] = 7;
    rdof_txvr_on_burst(t, &hdr);
    rdof_txvr_on_burst(t, &hdr);
    CHECK(t->call.headers == RDOF_TXVR_QUEUE && t->call.count == RDOF_TXVR_QUEUE && t->call.dropped == 0, "%d %d %d",
          t->call.headers, t->call.count, t->call.dropped);
    CHECK(t->call.extra == RDOF_TXVR_QUEUE - 1, "%d", t->call.extra);

    /* a lone CSBK's preambles: 16 at most */
    e = ex_new(false);
    t = &e->t;
    rdof_txvr_start(t);
    t->csbk_preambles = 100;
    tu = ex_tune(8, CH_A);
    rdof_txvr_on_tune(t, &tu);
    rdof_net_burst_t cs = ex_burst(8, RDOF_SYNC_DATA | RDOF_DT_CSBK, true);
    rdof_txvr_on_burst(t, &cs);
    CHECK(t->call.count == 17 && t->call.has_csbk, "%d", t->call.count);
    t->csbk_preambles = -3;
    tu = ex_tune(9, CH_A);
    t->has_call = false;
    rdof_txvr_on_tune(t, &tu);
    cs.stream_id[3] = 9;
    rdof_txvr_on_burst(t, &cs);
    CHECK(t->call.count == 1, "%d", t->call.count);

    /* the prebuffer: nothing goes out until it's there */
    e = ex_new(false);
    t = &e->t;
    rdof_txvr_start(t);
    t->prebuffer = 6;
    tu = ex_tune(10, CH_A);
    rdof_txvr_on_tune(t, &tu);
    for (int p = 0; p < 3; p++) {
        rdof_net_burst_t b = ex_burst(10, p ? (uint8_t)p : RDOF_SYNC_AUDIO, false);
        rdof_txvr_on_burst(t, &b);
    }
    ex_to_air(e);
    e->rig.clock += 0.1;
    rdof_txvr_poll(t);
    CHECK(e->nbursts == 0 && !t->call.started, "%d", e->nbursts);
    for (int p = 3; p < 6; p++) {
        rdof_net_burst_t b = ex_burst(10, (uint8_t)p, false);
        rdof_txvr_on_burst(t, &b);
    }
    rdof_txvr_poll(t);
    CHECK(e->nbursts == 6 && t->call.started, "%d", e->nbursts);

    /* channel lists out of range: as far as the arrays go */
    e = ex_new(false);
    t = &e->t;
    rdof_txvr_start(t);
    t->nchannels = 100000;
    tu = ex_tune(11, CH_B);
    rdof_txvr_on_tune(t, &tu);
    CHECK(t->has_call, "a channel in the list");
    e = ex_new(false);
    t = &e->t;
    rdof_txvr_start(t);
    t->nchannels = -5;
    tu = ex_tune(12, CH_A);
    rdof_txvr_on_tune(t, &tu);
    CHECK(!t->has_call && ex_answered(e, 12, RDOF_DMRK_NOT_ALLOWED, CH_A), "refused");
}

static void t_txvr_orphans(void)
{
    ex_t *e = ex_new(false);
    rdof_txvr_t *t = &e->t;
    rdof_txvr_start(t);
    /* bursts ahead of their DMRT: one slot per stream */
    rdof_net_burst_t b = ex_burst(20, RDOF_SYNC_AUDIO, false);
    rdof_txvr_on_burst(t, &b);
    rdof_txvr_on_burst(t, &b);
    int k = ex_orphan(t, 20);
    CHECK(k >= 0 && t->orphans[k].n == 2, "%d", k);
    /* 16 at most */
    for (int i = 0; i < 20; i++)
        rdof_txvr_on_burst(t, &b);
    CHECK(t->orphans[k].n == RDOF_TXVR_ORPHAN_BURSTS, "%d", t->orphans[k].n);
    /* a count gone negative starts again */
    t->orphans[k].n = -3;
    rdof_txvr_on_burst(t, &b);
    CHECK(t->orphans[k].n == 1, "%d", t->orphans[k].n);
    /* a fifth stream takes the oldest's place */
    for (uint32_t s = 21; s <= 24; s++) {
        e->rig.clock += 0.01;
        rdof_net_burst_t o = ex_burst(s, RDOF_SYNC_AUDIO, false);
        rdof_txvr_on_burst(t, &o);
    }
    CHECK(ex_orphan(t, 20) < 0 && ex_orphan(t, 24) == k, "%d %d", ex_orphan(t, 20), ex_orphan(t, 24));
    /* refused: its orphan goes, and its bursts are ignored from then on */
    rdof_tune_t tu = ex_tune(21, 450000000);
    rdof_txvr_on_tune(t, &tu);
    CHECK(ex_orphan(t, 21) < 0 && ex_answered(e, 21, RDOF_DMRK_NOT_ALLOWED, 450000000), "refused");
    rdof_net_burst_t o = ex_burst(21, RDOF_SYNC_AUDIO, false);
    rdof_txvr_on_burst(t, &o);
    CHECK(ex_orphan(t, 21) < 0, "an orphan again");
    /* the DMRT comes: its bursts join the call, as many as there are room for */
    k = ex_orphan(t, 22);
    for (int i = 1; i < RDOF_TXVR_ORPHAN_BURSTS; i++) { /* B ... D, 16 in all */
        o = ex_burst(22, i % 6 ? (uint8_t)(i % 6) : RDOF_SYNC_AUDIO, false);
        rdof_txvr_on_burst(t, &o);
    }
    t->orphans[k].n = 100000;
    tu = ex_tune(22, CH_A);
    rdof_txvr_on_tune(t, &tu);
    CHECK(t->has_call && ex_orphan(t, 22) < 0, "joined");
    CHECK(t->call.count == RDOF_TXVR_ORPHAN_BURSTS && t->call.vpos == 4, "%d %d", t->call.count, t->call.vpos);
    /* the rest age out */
    CHECK(ex_orphan(t, 23) >= 0, "23");
    e->rig.clock += RDOF_TXVR_ORPHAN_S + 0.01;
    rdof_txvr_poll(t);
    CHECK(ex_orphan(t, 23) < 0 && ex_orphan(t, 24) < 0, "aged");

    /* the refused list's count out of range */
    e = ex_new(false);
    t = &e->t;
    rdof_txvr_start(t);
    t->nrefused = -1;
    rdof_put_be32(t->refused[0], 30);
    b = ex_burst(30, RDOF_SYNC_AUDIO, false);
    rdof_txvr_on_burst(t, &b);
    CHECK(ex_orphan(t, 30) >= 0, "not refused");
    t->nrefused = 100000;
    rdof_put_be32(t->refused[RDOF_TXVR_REFUSED - 1], 31);
    b = ex_burst(31, RDOF_SYNC_AUDIO, false);
    rdof_txvr_on_burst(t, &b);
    CHECK(ex_orphan(t, 31) < 0, "refused");
    /* busy: another stream while there's a call */
    tu = ex_tune(32, CH_A);
    rdof_txvr_on_tune(t, &tu);
    tu = ex_tune(33, CH_A);
    rdof_txvr_on_tune(t, &tu);
    CHECK(ex_answered(e, 33, RDOF_DMRK_TXVR_BUSY, CH_A), "busy");
}

static void t_txvr_heard(void)
{
    ex_t *e = ex_new(true);
    rdof_txvr_t *t = &e->t;
    rdof_txvr_start(t);
    /* an end with no start before it */
    rdof_txvr_heard_call(t, "end", 1, 2);
    CHECK(!t->has_heard_call && !t->heard_call_ended, "heard");
    rdof_txvr_heard_call(t, "late_entry", 1, 2);
    CHECK(t->has_heard_call && !t->heard_call_ended && t->heard_src == 1 && t->heard_dst == 2, "late entry");
    rdof_txvr_heard_call(t, "data", 3, 4);
    CHECK(t->heard_src == 1 && !t->heard_call_ended, "data");
    e->rig.clock += 1;
    rdof_txvr_heard_call(t, "lost", 5, 6);
    CHECK(t->heard_call_ended && t->heard_at == e->rig.clock && t->heard_src == 1, "lost");

    /* slot 1, retuning, no receiver: not passed on */
    uint8_t burst[RDOF_BURST_BYTES] = {0};
    rdof_txvr_heard(t, 2, RDOF_SYNC_AUDIO, burst, -1);
    CHECK(e->nheard == 0, "no receiver");
    t->receiver.hear = ex_hear;
    t->receiver.ctx = e;
    rdof_txvr_heard(t, 1, RDOF_SYNC_AUDIO, burst, -1);
    CHECK(e->nheard == 0, "slot 1");
    t->tuning = true;
    rdof_txvr_heard(t, 2, RDOF_SYNC_AUDIO, burst, -1);
    t->tuning = false;
    CHECK(e->nheard == 0, "tuning");
    rdof_txvr_heard(t, 2, RDOF_SYNC_AUDIO, burst, -1);
    CHECK(e->nheard == 1, "%d", e->nheard);
    /* on the air: nothing heard */
    rdof_tune_t tu = ex_tune(40, CH_B);
    rdof_txvr_on_tune(t, &tu);
    ex_voice(e, 40, 2);
    ex_to_air(e);
    CHECK(!strcmp(rdof_txvr_state_name(t), "on_air"), "%s", rdof_txvr_state_name(t));
    rdof_txvr_heard(t, 2, RDOF_SYNC_AUDIO, burst, -1);
    CHECK(e->nheard == 1, "%d", e->nheard);
}

static void t_txvr_modem_errors(void)
{
    /* start: the mode, then the idle channel */
    ex_t *e = ex_new(true);
    rdof_txvr_t *t = &e->t;
    e->fail_writes = true;
    CHECK(!rdof_txvr_start(t) && !t->ready, "set_mode");
    e = ex_new(true);
    t = &e->t;
    e->rig.radio.band_lo = 470000000;
    CHECK(!rdof_txvr_start(t) && !t->ready && !t->has_tuned, "idle tune");
    /* not ready: refused as a radio error */
    rdof_tune_t tu = ex_tune(50, CH_A);
    rdof_txvr_on_tune(t, &tu);
    CHECK(!t->has_call && ex_answered(e, 50, RDOF_DMRK_RADIO_ERROR, CH_A), "not ready");

    /* idle's status lost */
    e = ex_new(true);
    t = &e->t;
    rdof_txvr_start(t);
    e->rig.radio.drop_status = 1;
    CHECK(!rdof_txvr_poll(t) && !t->ready && !t->has_tuned, "idle status");

    /* the call's channel refused by the radio: answered, back to idle */
    e = ex_new(false);
    t = &e->t;
    rdof_txvr_start(t);
    e->rig.radio.band_hi = 462570000;
    tu = ex_tune(51, CH_B);
    rdof_txvr_on_tune(t, &tu);
    CHECK(!rdof_txvr_poll(t) && !t->has_call && ex_answered(e, 51, RDOF_DMRK_RADIO_ERROR, CH_B), "tune");

    /* a status lost while listening: no carrier seen, so on the air */
    e = ex_new(false);
    t = &e->t;
    rdof_txvr_start(t);
    tu = ex_tune(52, CH_A);
    rdof_txvr_on_tune(t, &tu);
    ex_voice(e, 52, 1);
    rdof_txvr_poll(t);
    e->rig.radio.drop_status = 1;
    e->rig.clock += 0.31;
    CHECK(rdof_txvr_poll(t) && !strcmp(rdof_txvr_state_name(t), "on_air"), "%s", rdof_txvr_state_name(t));

    /* a burst's write failing on the air: the call ends there */
    e->fail_bursts = true;
    e->rig.clock += 0.1;
    CHECK(!rdof_txvr_poll(t) && !t->has_call && e->ends == 1 && !t->ready, "%d %d", t->has_call, e->ends);

    /* statuses lost while playing: ridden out for 2 s, then an error */
    e = ex_new(false);
    t = &e->t;
    rdof_txvr_start(t);
    tu = ex_tune(53, CH_A);
    rdof_txvr_on_tune(t, &tu);
    ex_voice(e, 53, 3);
    ex_to_air(e);
    e->rig.radio.drop_status = 1000;
    e->rig.clock += 0.1;
    CHECK(rdof_txvr_poll(t) && t->space == 0 && t->status_misses == 1, "%d %lu", t->space,
          (unsigned long)t->status_misses);
    e->rig.clock += RDOF_TXVR_STATUS_GIVE_UP_S;
    CHECK(rdof_txvr_poll(t) && t->status_misses == 2, "%lu", (unsigned long)t->status_misses);
    e->rig.clock += 0.01;
    CHECK(!rdof_txvr_poll(t) && !t->has_call, "gave up");

    /* the end's status lost */
    e = ex_new(false);
    t = &e->t;
    rdof_txvr_start(t);
    tu = ex_tune(54, CH_A);
    rdof_txvr_on_tune(t, &tu);
    ex_voice(e, 54, 1);
    rdof_net_burst_t term = ex_burst(54, RDOF_SYNC_DATA | RDOF_DT_TERMINATOR_WITH_LC, true);
    rdof_txvr_on_burst(t, &term);
    ex_to_air(e);
    e->rig.clock += 0.1;
    rdof_txvr_poll(t);
    CHECK(t->call.count == 0 && t->call.ended, "%d", t->call.count);
    e->rig.radio.drop_status = 1;
    e->rig.clock += 0.1;
    CHECK(!rdof_txvr_poll(t), "end status");
}

/* A call on CH_A run to its end with a CW ID due. */
static ex_t *ex_cw_call(void)
{
    ex_t *e = ex_new(false);
    rdof_txvr_t *t = &e->t;
    rdof_txvr_start(t);
    snprintf(t->cwid, sizeof t->cwid, "N0CALL");
    rdof_tune_t tu = ex_tune(60, CH_A);
    rdof_txvr_on_tune(t, &tu);
    ex_voice(e, 60, 1);
    rdof_net_burst_t term = ex_burst(60, RDOF_SYNC_DATA | RDOF_DT_TERMINATOR_WITH_LC, true);
    rdof_txvr_on_burst(t, &term);
    ex_to_air(e);
    return e;
}

static void t_txvr_cwid(void)
{
    /* the CW keys the radio: waited for, a sleep at a time */
    ex_t *e = ex_cw_call();
    e->cw_keys = true;
    e->drain_after = 3;
    ex_run(e, 200);
    CHECK(e->rig.radio.ncw == 1 && e->sleeps == 3 && e->t.ready, "%d %d", e->rig.radio.ncw, e->sleeps);
    CHECK(e->t.nlast_id == 1 && e->t.last_id[0].freq == CH_A, "%d", e->t.nlast_id);

    /* never unkeys: 15 s of sleeps, then on regardless */
    e = ex_cw_call();
    e->cw_keys = true;
    ex_run(e, 200);
    CHECK(e->sleeps == 150 && e->t.ready && e->t.nlast_id == 1, "%d", e->sleeps);

    /* a status lost while waiting: an error */
    e = ex_cw_call();
    e->cw_drop = 1;
    ex_run(e, 200);
    CHECK(e->rig.radio.ncw == 1 && !e->t.ready && e->t.nlast_id == 0, "%d %d", e->t.ready, e->t.nlast_id);

    /* the table full: the channel identified longest ago makes room; a count past it is the table */
    for (int n = RDOF_TXVR_IDS; n <= 1000; n += 1000 - RDOF_TXVR_IDS) {
        e = ex_cw_call();
        for (int i = 0; i < RDOF_TXVR_IDS; i++) {
            e->t.last_id[i].freq = 440000000u + (uint32_t)i;
            e->t.last_id[i].at = i == 5 ? -1e9 : 100.0 + i;
        }
        e->t.nlast_id = n;
        ex_run(e, 200);
        CHECK(e->rig.radio.ncw == 1 && e->t.last_id[5].freq == CH_A && e->t.last_id[4].freq == 440000004u,
              "n %d: %d %lu", n, e->rig.radio.ncw, (unsigned long)e->t.last_id[5].freq);
        CHECK(e->t.nlast_id == n, "%d", e->t.nlast_id);
    }

    /* identified lately here: not again */
    e = ex_cw_call();
    e->t.last_id[0].freq = CH_A;
    e->t.last_id[0].at = e->rig.clock;
    e->t.nlast_id = 1;
    ex_run(e, 200);
    CHECK(e->rig.radio.ncw == 0 && !e->t.has_call, "%d", e->rig.radio.ncw);
}

static void t_txvr_scan_edges(void)
{
    /* an empty list: nothing to tune */
    ex_t *e = ex_new(false);
    rdof_txvr_t *t = &e->t;
    rdof_txvr_start(t);
    t->has_scan = true;
    rdof_scan_defaults(&t->scan);
    CHECK(rdof_txvr_poll(t) && !t->has_tuned, "empty");
    t->scan.nchannels = -2;
    CHECK(rdof_txvr_poll(t) && !t->has_tuned, "negative");
    /* the next channel out of range: from the first */
    t->scan.channels[0] = (rdof_channel_t){CH_A, 1, 255};
    t->scan.channels[1] = (rdof_channel_t){CH_B, 2, 255};
    t->scan.nchannels = 2;
    t->scan_next = 50;
    CHECK(rdof_txvr_poll(t) && t->tuned_freq == CH_A && t->scan_next == 1 && e->retunes == 0, "%lu %d",
          (unsigned long)t->tuned_freq, t->scan_next);
    t->scan_next = -1;
    t->has_tuned = false;
    CHECK(rdof_txvr_poll(t) && t->tuned_freq == CH_A && t->scan_next == 1, "%lu %d", (unsigned long)t->tuned_freq,
          t->scan_next);
    /* the carrier check's status lost */
    t->scan.carrier_s = 0.1;
    e->rig.clock += 0.2;
    e->rig.radio.drop_status = 1;
    CHECK(!rdof_txvr_poll(t), "carrier status");
    /* a count past the array scans the array */
    e = ex_new(false);
    t = &e->t;
    rdof_txvr_start(t);
    t->has_scan = true;
    rdof_scan_defaults(&t->scan);
    for (int i = 0; i < RDOF_TXVR_MAX_CHANNELS; i++)
        t->scan.channels[i] = (rdof_channel_t){CH_A + 25000u * (uint32_t)i, 1, 255};
    t->scan.nchannels = 100000;
    t->scan_next = RDOF_TXVR_MAX_CHANNELS - 1;
    rdof_txvr_poll(t);
    CHECK(t->tuned_freq == CH_A + 25000u * (RDOF_TXVR_MAX_CHANNELS - 1) && t->scan_next == 0, "%lu %d",
          (unsigned long)t->tuned_freq, t->scan_next);
    /* covered channels past the array: as far as it goes */
    t->ncovered = 100000;
    t->covered[RDOF_TXVR_MAX_COVERED - 1] = 1;
    CHECK(rdof_txvr_poll(t), "covered");
}


/* A data block (CSBK, data header) with its CRC: d[0..9] given. */
static rdof_net_burst_t ex_block(uint32_t sid, uint8_t d[12], uint16_t mask, int dt, bool terminator)
{
    rdof_net_burst_t b = ex_burst(sid, (uint8_t)(RDOF_SYNC_DATA | dt), terminator);
    uint16_t crc = (uint16_t)(rdof_crc16_ccitt(d, 10) ^ mask);
    d[10] = (uint8_t)(crc >> 8);
    d[11] = (uint8_t)crc;
    rdof_data_burst(d, dt, 1, rdof_data_sync(RDOF_MS_VOICE), b.burst);
    return b;
}

static rdof_net_burst_t ex_csbk(uint32_t sid, int opcode, uint8_t b3, bool terminator)
{
    uint8_t d[12] = {(uint8_t)(0x80 | opcode), 0x00, 0, b3, 0, 0, 0, 0, 0, 0};
    return ex_block(sid, d, RDOF_MASK_CSBK, RDOF_DT_CSBK, terminator);
}

static rdof_net_burst_t ex_data_header(uint32_t sid, int dpf, bool response, int blocks, uint16_t mask)
{
    uint8_t d[12] = {(uint8_t)((response ? 0x40 : 0) | dpf), 0, 0, 0, 0, 0, 0, 0, (uint8_t)(blocks & 0x7F), 0};
    return ex_block(sid, d, mask, RDOF_DT_DATA_HEADER, false);
}

/* A fresh transceiver with a call on CH_A for stream sid. */
static ex_t *ex_call(bool idle, uint32_t sid)
{
    ex_t *e = ex_new(idle);
    rdof_txvr_start(&e->t);
    rdof_tune_t tu = ex_tune(sid, CH_A);
    rdof_txvr_on_tune(&e->t, &tu);
    return e;
}

static void t_txvr_more(void)
{
    /* untuned after an error: no channel, whatever was tuned before */
    ex_t *e = ex_new(true);
    rdof_txvr_t *t = &e->t;
    rdof_txvr_start(t);
    e->rig.radio.drop_status = 1;
    rdof_txvr_poll(t);
    uint32_t f;
    int cc;
    rdof_txvr_channel_now(t, &f, &cc);
    CHECK(t->tuned_freq == CH_B && f == 0 && cc == 0, "%lu %d", (unsigned long)f, cc);
    /* start again: tuned again, whatever it was before */
    e = ex_new(true);
    t = &e->t;
    rdof_txvr_start(t);
    rdof_txvr_start(t);
    CHECK(e->retunes == 2, "%d", e->retunes);

    /* 100 Hz off a channel is the channel; 101 isn't */
    e = ex_new(false);
    t = &e->t;
    rdof_txvr_start(t);
    rdof_tune_t tu = ex_tune(70, CH_A + 101);
    rdof_txvr_on_tune(t, &tu);
    CHECK(!t->has_call && ex_answered(e, 70, RDOF_DMRK_NOT_ALLOWED, CH_A + 101), "101");
    tu = ex_tune(71, CH_A - 100);
    rdof_txvr_on_tune(t, &tu);
    CHECK(t->has_call, "100");

    /* receiving: busy for another channel, not this one */
    e = ex_new(true);
    t = &e->t;
    rdof_txvr_start(t);
    e->rig.modem.last_rx = e->rig.clock;
    tu = ex_tune(72, CH_A);
    rdof_txvr_on_tune(t, &tu);
    CHECK(!t->has_call && ex_answered(e, 72, RDOF_DMRK_TXVR_BUSY, CH_A), "other channel");
    tu = ex_tune(73, CH_B);
    rdof_txvr_on_tune(t, &tu);
    CHECK(t->has_call, "this channel");

    /* the call's addressing is its first burst's */
    e = ex_call(false, 74);
    t = &e->t;
    rdof_net_burst_t b = ex_burst(74, RDOF_SYNC_AUDIO, false);
    rdof_txvr_on_burst(t, &b);
    b.src = 777;
    b.control = 1;
    rdof_txvr_on_burst(t, &b);
    CHECK(t->call.src == SRC, "%lu", (unsigned long)t->call.src);
    /* a CSBK after other bursts: no preambles */
    rdof_net_burst_t cs = ex_csbk(74, 0x1F, 0, true);
    rdof_txvr_on_burst(t, &cs);
    CHECK(t->call.count == 3 && !t->call.has_csbk, "%d", t->call.count);

    /* headers and terminators: one at least */
    e = ex_call(false, 75);
    t = &e->t;
    e->radio.headers = 0;
    e->radio.terminators = 0;
    rdof_net_burst_t hdr = ex_burst(75, RDOF_SYNC_DATA | RDOF_DT_VOICE_LC_HEADER, false);
    rdof_txvr_on_burst(t, &hdr);
    rdof_net_burst_t term = ex_burst(75, RDOF_SYNC_DATA | RDOF_DT_TERMINATOR_WITH_LC, true);
    rdof_txvr_on_burst(t, &term);
    CHECK(t->call.count == 2 && t->call.headers == 1 && t->call.terminators == 1, "%d %d %d", t->call.count,
          t->call.headers, t->call.terminators);
    /* fewer wanted than already queued: none more, the count kept */
    e = ex_call(false, 76);
    t = &e->t;
    e->radio.headers = 3;
    hdr.stream_id[3] = 76;
    rdof_txvr_on_burst(t, &hdr);
    e->radio.headers = 1;
    rdof_txvr_on_burst(t, &hdr);
    CHECK(t->call.count == 3 && t->call.headers == 3 && t->call.extra == 2, "%d %d %d", t->call.count,
          t->call.headers, t->call.extra);
    /* the extra headers wait in the prebuffer too: 3 headers and 4 voice aren't 6 + 2 */
    e->radio.headers = 3;
    for (int p = 0; p < 4; p++) { /* A-D */
        rdof_net_burst_t vb = ex_burst(76, p ? (uint8_t)p : RDOF_SYNC_AUDIO, false);
        rdof_txvr_on_burst(t, &vb);
    }
    CHECK(t->call.count == 7, "%d", t->call.count);
    ex_to_air(e);
    e->rig.clock += 0.1;
    rdof_txvr_poll(t);
    CHECK(!t->call.started && e->nbursts == 0, "%d", e->nbursts);
    /* headers after the call started: no more extra */
    e = ex_call(false, 77);
    t = &e->t;
    t->prebuffer = 1;
    b = ex_burst(77, RDOF_SYNC_AUDIO, false);
    rdof_txvr_on_burst(t, &b);
    ex_to_air(e);
    e->rig.clock += 0.1;
    rdof_txvr_poll(t);
    CHECK(t->call.started, "started");
    e->radio.headers = 3;
    hdr.stream_id[3] = 77;
    rdof_txvr_on_burst(t, &hdr);
    CHECK(t->call.extra == 0 && t->call.headers == 3, "%d", t->call.extra);

    /* the radio's space: writes stop one short of full */
    e = ex_call(false, 78);
    t = &e->t;
    for (int i = 0; i < 60; i++) {
        b = ex_burst(78, RDOF_SYNC_DATA | RDOF_DT_RATE_12, false);
        rdof_txvr_on_burst(t, &b);
    }
    ex_to_air(e);
    e->rig.clock += 0.1;
    rdof_txvr_poll(t);
    CHECK(e->nbursts == 47 && t->space == 1, "%d %d", e->nbursts, t->space);
    /* and one free place: none written */
    e = ex_call(false, 79);
    t = &e->t;
    ex_voice(e, 79, 1);
    ex_to_air(e);
    e->rig.radio.held = 47;
    e->rig.clock += 0.1;
    rdof_txvr_poll(t);
    CHECK(e->nbursts == 0, "%d", e->nbursts);
    /* nor a silence */
    e = ex_call(false, 80);
    t = &e->t;
    t->prebuffer = 1;
    b = ex_burst(80, RDOF_SYNC_AUDIO, false);
    rdof_txvr_on_burst(t, &b);
    ex_to_air(e);
    e->rig.clock += 0.1;
    rdof_txvr_poll(t);
    CHECK(e->nbursts == 1, "%d", e->nbursts);
    e->rig.radio.held = 47;
    e->rig.clock += 0.1;
    rdof_txvr_poll(t);
    CHECK(e->nbursts == 1 && t->space == 1, "%d %d", e->nbursts, t->space);

    /* exactly six silences: the next voice burst still placed */
    e = ex_call(false, 81);
    t = &e->t;
    t->prebuffer = 1;
    b = ex_burst(81, RDOF_SYNC_AUDIO, false);
    rdof_txvr_on_burst(t, &b);
    ex_to_air(e);
    for (int i = 0; i < 50 && t->call.vfilled < 6; i++) {
        e->rig.clock += RDOF_TXVR_BURST_S;
        rdof_txvr_poll(t);
    }
    CHECK(t->call.vfilled == 6 && t->call.count == 0, "%d %d", t->call.vfilled, t->call.count);
    int v = t->call.vpos;
    b = ex_burst(81, 2, false); /* C */
    rdof_txvr_on_burst(t, &b);
    CHECK(t->call.count == ((2 - v) % 6 + 6) % 6 + 1 && t->call.vpos == 3, "%d %d (v %d)", t->call.count,
          t->call.vpos, v);

    /* a timed-out call's terminators carry its header's LC */
    e = ex_call(false, 82);
    t = &e->t;
    uint8_t lc9[9], want[RDOF_BURST_BYTES];
    rdof_group_lc(SRC, 555, true, 0, lc9);
    hdr.stream_id[3] = 82;
    rdof_lc_burst(lc9, RDOF_DT_VOICE_LC_HEADER, 1, rdof_data_sync(RDOF_MS_VOICE), hdr.burst);
    rdof_txvr_on_burst(t, &hdr);
    ex_voice(e, 82, 1);
    ex_to_air(e);
    e->rig.clock += RDOF_TXVR_STREAM_TIMEOUT_S + 0.1;
    ex_run(e, 200);
    rdof_lc_burst(lc9, RDOF_DT_TERMINATOR_WITH_LC, 1, rdof_data_sync(RDOF_MS_VOICE), want);
    CHECK(e->nterms == e->radio.terminators && !memcmp(e->last.burst, want, RDOF_BURST_BYTES), "%d", e->nterms);

    /* the clock going back: an end's duration 0, not less */
    e = ex_call(false, 83);
    t = &e->t;
    ex_voice(e, 83, 3);
    ex_to_air(e);
    e->rig.clock -= 0.5;
    e->fail_bursts = true;
    rdof_txvr_poll(t);
    CHECK(e->ends == 1 && e->duration == 0.0, "%d %f", e->ends, e->duration);

    /* carrier while listening: busy */
    e = ex_call(false, 84);
    t = &e->t;
    rdof_txvr_poll(t);
    e->rig.radio.carrier = true;
    e->rig.clock += 0.1;
    rdof_txvr_poll(t);
    CHECK(!t->has_call && ex_answered(e, 84, RDOF_DMRK_CHANNEL_BUSY, CH_A), "carrier");

    /* a reply to the call just heard: that call's tail isn't busy; anyone else's is */
    for (int reply = 1; reply >= 0; reply--) {
        e = ex_new(false);
        t = &e->t;
        rdof_txvr_start(t);
        tu = ex_tune(85, CH_A);
        rdof_txvr_on_tune(t, &tu);
        rdof_txvr_poll(t); /* tuned */
        b = ex_burst(85, RDOF_SYNC_AUDIO, false);
        rdof_txvr_on_burst(t, &b);
        e->rig.clock += 1;
        rdof_txvr_heard_call(t, "start", DST, reply ? SRC : 999);
        rdof_txvr_heard_call(t, "end", 0, 0);
        e->rig.modem.last_rx = e->rig.clock + 0.2;
        e->rig.clock += 0.31;
        t->call.listen_until = e->rig.clock;
        rdof_txvr_poll(t);
        if (reply)
            CHECK(!strcmp(rdof_txvr_state_name(t), "on_air"), "%s", rdof_txvr_state_name(t));
        else
            CHECK(!t->has_call && ex_answered(e, 85, RDOF_DMRK_CHANNEL_BUSY, CH_A), "busy");
    }

    /* keyed again after the quiet began: the tail counted from the new quiet */
    e = ex_call(false, 86);
    t = &e->t;
    ex_voice(e, 86, 1);
    term = ex_burst(86, RDOF_SYNC_DATA | RDOF_DT_TERMINATOR_WITH_LC, true);
    rdof_txvr_on_burst(t, &term);
    ex_to_air(e);
    e->rig.clock += 0.1;
    rdof_txvr_poll(t); /* all written */
    fake_drain(&e->rig.radio);
    e->rig.clock += 0.1;
    rdof_txvr_poll(t); /* quiet */
    CHECK(t->call.has_quiet_at, "quiet");
    e->rig.radio.transmitting = true;
    e->rig.clock += 0.3;
    rdof_txvr_poll(t);
    CHECK(!t->call.has_quiet_at, "keyed");
    e->rig.radio.transmitting = false;
    e->rig.clock += 0.1;
    rdof_txvr_poll(t); /* quiet again */
    e->rig.clock += 0.3;
    rdof_txvr_poll(t);
    CHECK(t->has_call, "ended too soon");
    e->rig.clock += 0.3;
    rdof_txvr_poll(t);
    CHECK(!t->has_call, "never ended");

    /* a CSBK asking for a reply: the answer window, unless answer_s is 0 */
    for (int zero = 0; zero < 2; zero++) {
        e = ex_call(false, 87);
        t = &e->t;
        if (zero)
            t->answer_s = 0;
        cs = ex_csbk(87, 0x1F, 0, true);
        rdof_txvr_on_burst(t, &cs);
        CHECK(t->call.wants_reply && t->call.has_csbk, "wants");
        ex_to_air(e);
        ex_run(e, 200);
        CHECK(!t->has_call && t->has_answer_window == !zero, "zero %d: %d", zero, t->has_answer_window);
    }
    /* back to the idle channel idle_after_s from the window's end, not the call's */
    e = ex_call(true, 87);
    t = &e->t;
    t->idle_after_s = 1.0;
    cs = ex_csbk(87, 0x1F, 0, true);
    rdof_txvr_on_burst(t, &cs);
    ex_to_air(e);
    ex_run(e, 200);
    CHECK(t->has_answer_window && t->tuned_freq == CH_A, "window");
    double ended = e->rig.clock;
    e->rig.clock = ended + t->answer_s + 0.05;
    rdof_txvr_poll(t);
    CHECK(!t->has_answer_window && t->tuned_freq == CH_A, "closed");
    e->rig.clock = ended + 1.2;
    rdof_txvr_poll(t);
    CHECK(t->tuned_freq == CH_A, "too soon");
    e->rig.clock = ended + t->answer_s + 1.1;
    rdof_txvr_poll(t);
    CHECK(t->tuned_freq == CH_B, "%lu", (unsigned long)t->tuned_freq);
    /* which CSBKs and headers ask */
    struct {
        rdof_net_burst_t b;
        bool wants;
    } asks[] = {
        {ex_csbk(88, 0x24, 0x80, false), true},
        {ex_csbk(88, 0x24, 0x00, false), false},
        {ex_data_header(88, RDOF_DPF_CONFIRMED, true, 3, RDOF_MASK_DATA_HEADER), true},
        {ex_data_header(88, RDOF_DPF_CONFIRMED, false, 3, RDOF_MASK_DATA_HEADER), false},
        {ex_data_header(88, RDOF_DPF_CONFIRMED, true, 3, 0), false},
    };
    for (size_t i = 0; i < sizeof asks / sizeof asks[0]; i++) {
        e = ex_call(false, 88);
        rdof_txvr_on_burst(&e->t, &asks[i].b);
        CHECK(e->t.call.wants_reply == asks[i].wants, "%zu", i);
    }

    /* data_frames: a header's blocks; a bad CRC 0; a voice header first, none */
    e = ex_call(false, 89);
    t = &e->t;
    rdof_tx_call_info_t info;
    b = ex_data_header(89, RDOF_DPF_UNCONFIRMED, false, 5, RDOF_MASK_DATA_HEADER);
    rdof_txvr_on_burst(t, &b);
    rdof_txvr_call_info(t, &t->call, &info);
    CHECK(info.frames == 5, "%d", info.frames);
    e = ex_call(false, 89);
    t = &e->t;
    b = ex_data_header(89, RDOF_DPF_UNCONFIRMED, false, 5, 0);
    rdof_txvr_on_burst(t, &b);
    rdof_txvr_call_info(t, &t->call, &info);
    CHECK(info.frames == 0, "%d", info.frames);
    e = ex_call(false, 89);
    t = &e->t;
    hdr.stream_id[3] = 89;
    rdof_txvr_on_burst(t, &hdr);
    b = ex_data_header(89, RDOF_DPF_UNCONFIRMED, false, 5, RDOF_MASK_DATA_HEADER);
    rdof_txvr_on_burst(t, &b);
    rdof_txvr_call_info(t, &t->call, &info);
    CHECK(info.frames == -1, "%d", info.frames);

    /* scanning: a burst heard at the very moment of the tune isn't heard on this channel */
    e = ex_new(false);
    t = &e->t;
    rdof_txvr_start(t);
    t->has_scan = true;
    rdof_scan_defaults(&t->scan);
    t->scan.channels[0] = (rdof_channel_t){CH_A, 1, 255};
    t->scan.channels[1] = (rdof_channel_t){CH_B, 1, 255};
    t->scan.nchannels = 2;
    rdof_txvr_poll(t);
    CHECK(t->tuned_freq == CH_A, "%lu", (unsigned long)t->tuned_freq);
    e->rig.modem.last_rx = t->tuned_at;
    e->rig.clock += 0.5;
    rdof_txvr_poll(t);
    CHECK(t->tuned_freq == CH_B && e->scans == 0, "%lu %d", (unsigned long)t->tuned_freq, e->scans);
}

void test_txvr(void)
{
    for (size_t i = 0; i < SC_TXVR_N; i++)
        run(&sc_txvr[i]);
    t_txvr_call_edges();
    t_txvr_orphans();
    t_txvr_heard();
    t_txvr_modem_errors();
    t_txvr_cwid();
    t_txvr_scan_edges();
    t_txvr_more();
}

#else

void test_txvr(void) {}

#endif
