// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The repeater (duplex and SFR) against the golden scenarios (scenarios.h). */
#include <math.h>

#include "harness.h"
#include "rdof/features.h"

#if RDOF_FEATURE_REPEATER && RDOF_FEATURE_TRACKER

#include "rdof/hblink.h"
#include "rdof/json.h"
#include "rdof/netcalls.h"
#include "rdof/repeater.h"
#include "rdof/tracker.h"
#include "scenarios.h"

#define RPEER 9990601

typedef struct {
    fake_rig_t rig;
    expect_t e;
    rdof_radio_t radio;
    rdof_repeater_t rpt;
    rdof_tracker_t tracker;
    rdof_hb_sink_t hb;
    rdof_netcalls_t calls;
    int rand_k;
    char state[64];
} rpt_harness_t;

static void out_json(void *ctx, const char *ch, const char *json) { expect(&((rpt_harness_t *)ctx)->e, "json", json); }
static void out_raw(void *ctx, const char *ch, const char *json) { expect(&((rpt_harness_t *)ctx)->e, "raw", json); }

static void send_pkt(void *ctx, const uint8_t *d, size_t n)
{
    char hex[400];
    hexs(d, n, hex);
    expect(&((rpt_harness_t *)ctx)->e, "send", hex);
}

static uint32_t hb_rand(void *ctx)
{
    rpt_harness_t *h = ctx;
    return (uint32_t)sc_stream_ids[h->rand_k++] - 1;
}

static void rx_hear(void *ctx, int slot, uint8_t control, const uint8_t *burst, double dbm, int cc, uint32_t freq)
{
    rpt_harness_t *h = ctx;
    rdof_burst_t b;
    if (freq)
        h->tracker.freq_hz = freq;
    rdof_modem_burst(control, burst, dbm, cc, slot, &b);
    rdof_tracker_feed(&h->tracker, &b, h->rig.clock);
}

static void rx_tick(void *ctx)
{
    rpt_harness_t *h = ctx;
    rdof_tracker_tick(&h->tracker, h->rig.clock);
}

static void on_tx(void *ctx, const char *action, const rdof_tx_call_info_t *c, double duration)
{
    rpt_harness_t *h = ctx;
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
    rpt_harness_t *h = ctx;
    char json[1024];
    if (rdof_netcalls_burst(&h->calls, c, b->control, b->burst, h->rig.clock, json, sizeof json))
        expect(&h->e, "nraw", json);
}

static void on_key(void *ctx, bool keyed)
{
    expect(&((rpt_harness_t *)ctx)->e, "key", keyed ? "1" : "0");
}

static void modem_rx(void *ctx, int slot, uint8_t control, const uint8_t *burst, int rssi)
{
    rdof_repeater_heard(&((rpt_harness_t *)ctx)->rpt, slot, control, burst, rssi);
}

static void modem_lost(void *ctx, int slot)
{
    rdof_repeater_lost(&((rpt_harness_t *)ctx)->rpt, slot);
}

static void modem_nak(void *ctx, uint8_t cmd, int err)
{
    rdof_repeater_nak(&((rpt_harness_t *)ctx)->rpt, cmd, err);
}

static double clock_of(void *ctx)
{
    return ((rpt_harness_t *)ctx)->rig.clock;
}

static void state(rpt_harness_t *h)
{
    char s[64];
    rdof_repeater_t *r = &h->rpt;
    snprintf(s, sizeof s, "%d,%d,%d,%d,%d", r->keyed, r->transmitting, rdof_repeater_busy(r, 1), rdof_repeater_busy(r, 2),
             r->ready);
    if (strcmp(s, h->state)) {
        strcpy(h->state, s);
        expect(&h->e, "state", s);
    }
}

static void run(const struct sc_rpt *sc)
{
    static rpt_harness_t h;
    memset(&h, 0, sizeof h);
    h.e.name = sc->name;
    h.e.outs = (const struct expect_out *)sc->outs;
    h.e.nouts = sc->nouts;
    static rdof_split_t split;
    if (!strcmp(sc->kind, "split"))
        rig_init_split(&h.rig, &split, &h.e);
    else
        rig_init(&h.rig, sc->kind, &h.e);
    rdof_modem_start(&h.rig.modem, 1);
    rdof_radio_init(&h.radio, &h.rig.modem, kind_of(sc->rkind));
    h.radio.fixed_hz = (uint32_t)sc->tx;
    rdof_repeater_t *r = &h.rpt;
    rdof_repeater_init(r, &h.radio, RPEER, (uint32_t)sc->rx, (uint32_t)sc->tx, 1);
    r->enabled[1] = sc->slot1;
    r->enabled[2] = sc->slot2;
    r->inactivity_s = sc->inactivity_s;
    r->short_lc_ts1_low = sc->short_lc_ts1_low;
    rdof_repeater_set_call_hang(r, sc->call_hang_s);
    r->prebuffer = sc->prebuffer;
    snprintf(r->cwid, sizeof r->cwid, "%s", sc->cwid);
    r->cwid_every_s = sc->cwid_every_s;
    if (sc->sfr)
        rdof_repeater_set_sfr(r, sc->inbound);
    r->on_tx = on_tx;
    r->on_sent = on_sent;
    r->on_key = on_key;
    r->clock = clock_of;
    r->ctx = &h;
    rdof_netcalls_init(&h.calls);
    if (sc->receiver) {
        rdof_tracker_init(&h.tracker, "RPT1", 0.0, 0.0);
        rdof_sink_t out = {out_json, out_raw, NULL, &h, NULL};
        rdof_tracker_add_sink(&h.tracker, out);
        rdof_hb_sink_init(&h.hb, RPEER);
        h.hb.send = send_pkt;
        h.hb.rand32 = hb_rand;
        h.hb.ctx = &h;
        rdof_tracker_add_sink(&h.tracker, rdof_hb_sink_as_sink(&h.hb));
        r->receiver.hear = rx_hear;
        r->receiver.tick = rx_tick;
        r->receiver.ctx = &h;
    }
    h.rig.modem.on_rx = modem_rx;
    h.rig.modem.on_lost = modem_lost;
    h.rig.modem.on_nak = modem_nak;
    h.rig.modem.cb_ctx = &h;
    if (!rdof_repeater_start(r))
        expect(&h.e, "error", "");
    state(&h);
    for (int i = 0; i < sc->nsteps; i++) {
        const struct sc_step *st = &sc->steps[i];
        uint8_t buf[256];
        size_t n;
        switch (st->op) {
        case 'D': {
            rdof_net_burst_t b;
            n = unhexs(st->a, buf);
            if (rdof_parse_dmrd(buf, n, &b))
                rdof_repeater_on_burst(r, &b);
            break;
        }
        case 'P':
            if (!rdof_repeater_poll(r))
                expect(&h.e, "error", "");
            state(&h);
            break;
        case 'A':
            h.rig.clock += st->d;
            rig_tick(&h.rig);
            break;
        case 'h':
            unhexs(st->a, buf);
            fake_hear(h.rig.split ? &h.rig.rx : &h.rig.radio, (uint8_t)st->x, buf, (int)st->y, (int)st->z);
            rig_pump(&h.rig);
            break;
        case 'l':
            fake_lose(h.rig.split ? &h.rig.rx : &h.rig.radio, (int)st->x);
            rig_pump(&h.rig);
            break;
        case 'd': fake_drain(&h.rig.radio); break;
        case 'r': h.rig.radio.rf_busy = st->x != 0; break;
        case 'c': h.rig.radio.carrier = st->x != 0; break;
        case 's': h.rig.radio.drop_status = (int)st->x; break;
        default: printf("unknown step %c\n", st->op);
        }
    }
    expect_done(&h.e);
}

/* ── edges: what the scenarios don't reach ── */

#include <limits.h>
#include <stdarg.h>

#include "rdof/fec.h"
#include "rdof/layer2.h"
#include "rdof/pdu.h"

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

/* A repeater on a fake radio. The log: each modem write ("st" GET_STATUS, "d1:41" DMR_DATA1 with its control,
 * "key1", "ab2", "lc", "mode0", "cw", "freq", "cfg"; "x1d" a write refused) and each hook ("K1" on_key,
 * "tx:start:1>2" on_tx, "sent:41" on_sent), space separated. */
typedef struct {
    fake_radio_t f;
    rdof_modem_t m;
    rdof_radio_t radio;
    rdof_repeater_t r;
    double clock;
    char log[16384];
    int fail_cmd, fail_skip; /* writes of fail_cmd refused once fail_skip more have gone through; -1 none */
    int locks, depth, bad_lock;
    int sleeps;
    bool drain_on_sleep;
    rdof_mmdvm_status_t heard_status;
    int heard;
    double duration; /* the last on_tx's */
} ed_t;

static void ed_log(ed_t *e, const char *fmt, ...)
{
    size_t n = strlen(e->log);
    if (n && n < sizeof e->log - 1)
        e->log[n++] = ' ';
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->log + n, sizeof e->log - n, fmt, ap);
    va_end(ap);
}

/* What the repeater wrote last: the SET_CONFIG flags, and per slot its DMR_DATA bursts (control, burst). */
static uint8_t ed_cfg_flags;
static struct {
    uint8_t control, burst[RDOF_BURST_BYTES];
} ed_sent[3][64];
static int ed_nsent[3];

static bool ed_write(void *ctx, const uint8_t *d, size_t n)
{
    ed_t *e = ctx;
    uint8_t cmd = d[2];
    if (cmd == RDOF_MMDVM_SET_CONFIG)
        ed_cfg_flags = d[3];
    if ((cmd == RDOF_MMDVM_DMR_DATA1 || cmd == RDOF_MMDVM_DMR_DATA2) && n >= 4 + RDOF_BURST_BYTES) {
        int sl = cmd == RDOF_MMDVM_DMR_DATA1 ? 1 : 2;
        if (ed_nsent[sl] < 64) {
            ed_sent[sl][ed_nsent[sl]].control = d[3];
            memcpy(ed_sent[sl][ed_nsent[sl]].burst, d + 4, RDOF_BURST_BYTES);
        }
        ed_nsent[sl]++;
    }
    if (cmd == e->fail_cmd && e->fail_skip-- <= 0) {
        ed_log(e, "x%02x", cmd);
        return false;
    }
    switch (cmd) {
    case RDOF_MMDVM_GET_STATUS: ed_log(e, "st"); break;
    case RDOF_MMDVM_DMR_DATA1: ed_log(e, "d1:%02x", d[3]); break;
    case RDOF_MMDVM_DMR_DATA2: ed_log(e, "d2:%02x", d[3]); break;
    case RDOF_MMDVM_DMR_START: ed_log(e, "key%d", d[3]); break;
    case RDOF_MMDVM_DMR_ABORT: ed_log(e, "ab%d", d[3]); break;
    case RDOF_MMDVM_DMR_SHORTLC: ed_log(e, "lc"); break;
    case RDOF_MMDVM_SET_MODE: ed_log(e, "mode%d", d[3]); break;
    case RDOF_MMDVM_SEND_CWID: ed_log(e, "cw"); break;
    case RDOF_MMDVM_SET_FREQ: ed_log(e, "freq"); break;
    case RDOF_MMDVM_SET_CONFIG: ed_log(e, "cfg"); break;
    default: ed_log(e, "c%02x", cmd);
    }
    fake_write(&e->f, d, n);
    uint8_t buf[4096];
    size_t k;
    while ((k = fake_take(&e->f, buf, sizeof buf)) > 0)
        rdof_modem_feed(&e->m, buf, k);
    return true;
}

static bool ed_wait(void *ctx, int ms) { return ((ed_t *)ctx)->m.reply_ready; }
static double ed_clock(void *ctx) { return ((ed_t *)ctx)->clock; }

static void ed_sleep(void *ctx, int ms)
{
    ed_t *e = ctx;
    e->sleeps++;
    if (e->drain_on_sleep)
        fake_drain(&e->f);
}

static void ed_on_tx(void *ctx, const char *action, const rdof_tx_call_info_t *c, double d)
{
    ((ed_t *)ctx)->duration = d;
    ed_log(ctx, "tx:%s:%lu>%lu", action, (unsigned long)c->src, (unsigned long)c->dst);
}

static void ed_on_sent(void *ctx, const rdof_tx_call_info_t *c, const rdof_net_burst_t *b)
{
    ed_log(ctx, "sent:%02x", b->control);
}

static void ed_on_key(void *ctx, bool keyed) { ed_log(ctx, "K%d", keyed); }

static void ed_lock(void *ctx)
{
    ed_t *e = ctx;
    e->locks++;
    if (e->depth++)
        e->bad_lock++;
}

static void ed_unlock(void *ctx)
{
    ed_t *e = ctx;
    if (--e->depth)
        e->bad_lock++;
}

static void ed_hear(void *ctx, int slot, uint8_t control, const uint8_t *burst, double dbm, int cc, uint32_t freq)
{
    ((ed_t *)ctx)->heard++;
}

static ed_t *ed_new(const char *kind)
{
    static ed_t e;
    memset(&e, 0, sizeof e);
    fake_init(&e.f, kind);
    e.clock = 1000.0;
    e.fail_cmd = -1;
    rdof_modem_port_t port = {ed_write, ed_wait, NULL, ed_clock, ed_sleep, &e};
    rdof_modem_init(&e.m, &port);
    rdof_modem_start(&e.m, 1);
    rdof_radio_init(&e.radio, &e.m, kind_of(kind));
    e.radio.fixed_hz = 462550000;
    rdof_repeater_t *r = &e.r;
    rdof_repeater_init(r, &e.radio, RPEER, 467550000, 462550000, 1);
    r->on_tx = ed_on_tx;
    r->on_sent = ed_on_sent;
    r->on_key = ed_on_key;
    r->clock = ed_clock;
    r->ctx = &e;
    e.log[0] = 0;
    return &e;
}

/* The log since the last check is want, whole; then it's cleared. */
#define LOG_IS(e, want)                                                                                      \
    do {                                                                                                     \
        CHECK(!strcmp((e)->log, want), "log\n  want %s\n  got  %s", want, (e)->log);                         \
        (e)->log[0] = 0;                                                                                     \
    } while (0)

static bool ed_start(ed_t *e)
{
    bool ok = rdof_repeater_start(&e->r);
    e->log[0] = 0;
    return ok;
}

#define CC 1
#define C_HEADER (RDOF_SYNC_DATA | RDOF_DT_VOICE_LC_HEADER)
#define C_TERM (RDOF_SYNC_DATA | RDOF_DT_TERMINATOR_WITH_LC)
#define C_CSBK (RDOF_SYNC_DATA | RDOF_DT_CSBK)
#define C_DATA_HEADER (RDOF_SYNC_DATA | RDOF_DT_DATA_HEADER)
#define C_PI (RDOF_SYNC_DATA | RDOF_DT_PI_HEADER)

static void b_lc(const uint8_t lc9[9], int dt, uint8_t out[RDOF_BURST_BYTES])
{
    rdof_lc_burst(lc9, dt, CC, rdof_data_sync(RDOF_MS_VOICE), out);
}

static void b_header(uint32_t src, uint32_t dst, int dt, uint8_t out[RDOF_BURST_BYTES])
{
    uint8_t lc9[9];
    rdof_group_lc(src, dst, true, 0, lc9);
    b_lc(lc9, dt, out);
}

/* Voice burst n (0 A ... 5 F), with lc9's embedded fragment on B-E when lc9 is given; returns its control. */
static uint8_t b_voice(int n, const uint8_t *lc9, int cc, uint8_t out[RDOF_BURST_BYTES])
{
    uint8_t raw[128];
    const uint8_t *emb = NULL;
    if (lc9 && n >= 1 && n <= 4) {
        rdof_embedded_lc_encode(lc9, raw);
        emb = raw + 32 * (n - 1);
    }
    return rdof_silence_burst(n, cc, false, emb, out);
}

static void b_payload(uint8_t pl[12], uint16_t mask, int dt, uint8_t out[RDOF_BURST_BYTES])
{
    uint16_t c = rdof_crc16_ccitt(pl, 10) ^ mask;
    pl[10] = (uint8_t)(c >> 8);
    pl[11] = (uint8_t)c;
    rdof_data_burst(pl, dt, CC, rdof_data_sync(RDOF_MS_VOICE), out);
}

static void b_csbk(uint8_t op, uint32_t src, uint32_t dst, bool good, uint8_t out[RDOF_BURST_BYTES])
{
    uint8_t pl[12] = {op, 0, 0, 0, (uint8_t)(dst >> 16), (uint8_t)(dst >> 8), (uint8_t)dst,
                      (uint8_t)(src >> 16), (uint8_t)(src >> 8), (uint8_t)src};
    b_payload(pl, good ? RDOF_MASK_CSBK : 0, RDOF_DT_CSBK, out);
}

static void b_data_header(uint32_t src, uint32_t dst, int blocks, bool good, uint8_t out[RDOF_BURST_BYTES])
{
    uint8_t pl[12] = {0x82, 0x40, (uint8_t)(dst >> 16), (uint8_t)(dst >> 8), (uint8_t)dst,
                      (uint8_t)(src >> 16), (uint8_t)(src >> 8), (uint8_t)src, (uint8_t)(0x80 | blocks)};
    b_payload(pl, good ? RDOF_MASK_DATA_HEADER : 0, RDOF_DT_DATA_HEADER, out);
}

static void b_block(int dt, uint8_t out[RDOF_BURST_BYTES])
{
    uint8_t pl[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    b_payload(pl, 0, dt, out);
}

static void heard(ed_t *e, int slot, uint8_t control, const uint8_t burst[RDOF_BURST_BYTES])
{
    rdof_repeater_heard(&e->r, slot, control, burst, 0);
}

static void net_send(ed_t *e, uint32_t sid, int slot, uint8_t control, const uint8_t burst[RDOF_BURST_BYTES], bool term,
                     uint32_t src, uint32_t dst)
{
    rdof_net_burst_t b = {.control = control, .terminator = term, .src = src, .dst = dst, .group = true, .slot = slot};
    b.stream_id[0] = (uint8_t)(sid >> 24), b.stream_id[1] = (uint8_t)(sid >> 16);
    b.stream_id[2] = (uint8_t)(sid >> 8), b.stream_id[3] = (uint8_t)sid;
    memcpy(b.burst, burst, RDOF_BURST_BYTES);
    rdof_repeater_on_burst(&e->r, &b);
}

/* A network voice call's header and voice A..F (voice from the given position count). */
static void net_voice_call(ed_t *e, uint32_t sid, int slot, uint32_t src, uint32_t dst, int nvoice, bool end)
{
    uint8_t b[RDOF_BURST_BYTES], lc9[9];
    rdof_group_lc(src, dst, true, 0, lc9);
    b_lc(lc9, RDOF_DT_VOICE_LC_HEADER, b);
    net_send(e, sid, slot, C_HEADER, b, false, src, dst);
    for (int i = 0; i < nvoice; i++) {
        uint8_t c = b_voice(i % 6, lc9, CC, b);
        net_send(e, sid, slot, c, b, false, src, dst);
    }
    if (end) {
        b_lc(lc9, RDOF_DT_TERMINATOR_WITH_LC, b);
        net_send(e, sid, slot, C_TERM, b, true, src, dst);
    }
}

static bool poll_at(ed_t *e, double dt)
{
    e->clock += dt;
    return rdof_repeater_poll(&e->r);
}

/* Setters and the small queries; the C fixes for out-of-range slots. */
static void ed_basics(void)
{
    ed_t *e = ed_new("mmdvm_hs");
    rdof_repeater_t *r = &e->r;
    CHECK(!rdof_repeater_busy(r, 0) && !rdof_repeater_busy(r, 3) && !rdof_repeater_busy(r, -1) &&
              !rdof_repeater_busy(r, INT_MIN),
          "a slot that isn't 1 or 2 is never busy (it read past slots[])");
    CHECK(!rdof_repeater_busy(r, 1) && !rdof_repeater_busy(r, 2), "idle");
    rdof_repeater_set_call_hang(r, -1.0);
    CHECK(r->call_hang_s == 0.0, "negative call hang: %g", r->call_hang_s);
    rdof_repeater_set_call_hang(r, NAN);
    CHECK(r->call_hang_s == 0.0, "NaN call hang: %g", r->call_hang_s);
    rdof_repeater_set_call_hang(r, 2.5);
    CHECK(r->call_hang_s == 2.5, "call hang: %g", r->call_hang_s);
    CHECK(!rdof_repeater_on_air(r), "not on air");
    r->transmitting = true;
    CHECK(rdof_repeater_on_air(r), "transmitting");
    r->transmitting = false, r->keyed = true;
    CHECK(rdof_repeater_on_air(r), "keyed");
    r->keyed = false;
    /* stop: unkeys only when keyed */
    rdof_repeater_stop(r);
    LOG_IS(e, "");
    r->keyed = true;
    rdof_repeater_stop(r);
    LOG_IS(e, "key0");
    int slots[][3] = {{1, 1, 2}, {2, 2, 1}, {0, 1, 2}, {3, 1, 2}, {-1, 1, 2}, {INT_MAX, 1, 2}};
    for (size_t i = 0; i < sizeof slots / sizeof slots[0]; i++) {
        rdof_repeater_set_sfr(r, slots[i][0]);
        CHECK(r->sfr && r->inbound == slots[i][1] && r->outbound == slots[i][2],
              "set_sfr(%d): inbound %d outbound %d (anything but 2 is 1; 0 and 3 read past slots[])", slots[i][0],
              r->inbound, r->outbound);
    }

    /* polled before start: the Null short LC goes once; no clock: the time is 0 */
    e = ed_new("mmdvm_hs");
    e->r.clock = NULL;
    CHECK(rdof_repeater_poll(&e->r), "poll before start");
    LOG_IS(e, "st lc");
    CHECK(e->r.last_status == 0.0, "the status at time 0: %g", e->r.last_status);
    CHECK(rdof_repeater_poll(&e->r), "again");
    LOG_IS(e, "");

    /* start: the counts cleared, the radio tuned; a tune refused */
    e = ed_new("mmdvm_hs");
    e->r.rf_count = 5, e->r.net_count = 6, e->r.nak_count = 7, e->r.inbound_flag = e->r.transmitting = true;
    CHECK(rdof_repeater_start(&e->r) && e->r.ready, "start");
    CHECK(!e->r.rf_count && !e->r.net_count && !e->r.nak_count && !e->r.inbound_flag && !e->r.transmitting,
          "cleared");
    LOG_IS(e, "freq cfg mode2");
    e = ed_new("mmdvm_hs");
    e->fail_cmd = RDOF_MMDVM_SET_CONFIG;
    CHECK(!rdof_repeater_start(&e->r) && !e->r.ready, "the radio refused");
    LOG_IS(e, "freq x02");
}

/* The input queues: their bounds, and every touch under the lock. */
static void ed_queues(void)
{
    ed_t *e = ed_new("mmdvm_hs");
    rdof_repeater_t *r = &e->r;
    r->lock = ed_lock, r->unlock = ed_unlock;
    CHECK(ed_start(e), "start");
    uint8_t b[RDOF_BURST_BYTES] = {0};
    for (int i = 0; i < RDOF_RPT_IN + 1; i++)
        net_send(e, 100 + i, 1, 0x20, b, false, 1, 2);
    CHECK(r->net_count == RDOF_RPT_IN && r->dropped == 1, "network: %d queued, %u dropped", r->net_count, r->dropped);
    for (int i = 0; i < RDOF_RPT_IN; i++)
        heard(e, 1, 0x20, b);
    rdof_repeater_lost(r, 1);
    CHECK(r->rf_count == RDOF_RPT_IN && r->dropped == 2, "RF: %d queued, %u dropped", r->rf_count, r->dropped);
    for (int i = 0; i < 17; i++)
        rdof_repeater_nak(r, RDOF_MMDVM_DMR_DATA2, i);
    CHECK(r->nak_count == 16 && r->dropped == 2, "NAKs: %d queued, %u dropped", r->nak_count, r->dropped);
    CHECK(r->naks[15].err == 15, "the last kept: %d", r->naks[15].err);
    int before = e->locks;
    CHECK(rdof_repeater_poll(r), "poll");
    CHECK(!r->rf_count && !r->net_count && !r->nak_count, "drained");
    CHECK(e->locks - before >= RDOF_RPT_IN * 2 + 16 + 3, "locks per item: %d", e->locks - before);
    CHECK(!e->bad_lock && !e->depth, "balanced: %d, depth %d", e->bad_lock, e->depth);
    /* the heads wrap */
    for (int i = 0; i < 3; i++)
        heard(e, 3, 0x20, b), rdof_repeater_nak(r, 0, 0), net_send(e, 900 + i, 3, 0x20, b, false, 1, 2);
    CHECK(rdof_repeater_poll(r) && !r->rf_count && !r->net_count && !r->nak_count, "wrapped");
    CHECK(r->rf_head == 3 && r->net_head == 3 && r->nak_head == 3, "heads %d %d %d", r->rf_head, r->net_head,
          r->nak_head);
}

/* An RF voice call: headers again, PI headers, positions out of range, another talker, terminators. */
static void ed_rf_voice(void)
{
    ed_t *e = ed_new("mmdvm_hs");
    rdof_repeater_t *r = &e->r;
    r->receiver.hear = ed_hear, r->receiver.ctx = e;
    CHECK(ed_start(e), "start");
    uint8_t b[RDOF_BURST_BYTES], lc9[9], zero[RDOF_BURST_BYTES] = {0};
    rdof_group_lc(9992345, 9, true, 0, lc9);
    b_lc(lc9, RDOF_DT_TERMINATOR_WITH_LC, b);
    heard(e, 1, C_TERM, b); /* a terminator with no call: nothing */
    CHECK(poll_at(e, 0) && !r->slots[1].count && r->tx_until == -INFINITY, "a lone terminator");
    LOG_IS(e, "st");
    b_lc(lc9, RDOF_DT_VOICE_LC_HEADER, b);
    heard(e, 1, C_HEADER, b);
    CHECK(poll_at(e, 0) && r->slots[1].rf.active && r->slots[1].rf.src == 9992345, "the call");
    LOG_IS(e, "key1 K1 st d1:41 d1:41 d1:41 lc");
    CHECK(e->heard == 2, "heard %d", e->heard);
    heard(e, 1, C_HEADER, b);    /* its header again */
    heard(e, 1, C_HEADER, zero); /* a header that doesn't decode, in the call */
    uint8_t c = b_voice(5, lc9, CC, b);
    heard(e, 1, c, b); /* F before A: waits for A */
    CHECK(poll_at(e, 0.1) && !r->slots[1].count && r->slots[1].rf.last == e->clock, "absorbed");
    LOG_IS(e, "st d1:41");
    rdof_data_burst((const uint8_t[12]){0x05}, RDOF_DT_PI_HEADER, CC, rdof_data_sync(RDOF_MS_VOICE), b);
    heard(e, 1, C_PI, b); /* R4.4: after the headers, before A */
    c = b_voice(0, lc9, CC, b);
    heard(e, 1, c, b);
    rdof_data_burst((const uint8_t[12]){0x05}, RDOF_DT_PI_HEADER, CC, rdof_data_sync(RDOF_MS_VOICE), b);
    heard(e, 1, C_PI, b); /* after A: not */
    heard(e, 1, 0x06, zero); /* no position 6 */
    CHECK(poll_at(e, 0.1), "PI");
    LOG_IS(e, "st d1:40 d1:20");
    /* another talker's header: the call ends (R4.3), the new one starts */
    b_header(9992346, 9, RDOF_DT_VOICE_LC_HEADER, b);
    heard(e, 1, C_HEADER, b);
    CHECK(poll_at(e, 0.1) && r->slots[1].rf.src == 9992346, "the new talker");
    LOG_IS(e, "st d1:42 d1:41 d1:41 d1:41");
    b_header(9992346, 9, RDOF_DT_TERMINATOR_WITH_LC, b);
    heard(e, 1, C_TERM, b);
    rdof_repeater_lost(r, 1); /* lost with no call: nothing */
    CHECK(poll_at(e, 0.1) && !r->slots[1].rf.active, "ended");
    LOG_IS(e, "st d1:42");
}

/* Late entry (R4.6): the embedded LC from B-E. */
static void ed_late_entry(void)
{
    ed_t *e = ed_new("mmdvm_hs");
    rdof_repeater_t *r = &e->r;
    CHECK(ed_start(e), "start");
    rdof_rpt_slot_t *s = &r->slots[2];
    uint8_t b[RDOF_BURST_BYTES], lc9[9], other[9], odd[9] = {0x3F, 0, 0, 0, 0, 9, 0x98, 0x78, 0x99};
    rdof_group_lc(9992345, 9, true, 0, lc9);
    rdof_group_lc(7, 8, false, 0, other);
    uint8_t c = b_voice(4, lc9, CC, b);
    heard(e, 2, c, b); /* E alone */
    c = b_voice(5, lc9, CC, b);
    heard(e, 2, c, b); /* F: no fragment */
    c = b_voice(1, lc9, 2, b);
    heard(e, 2, c, b); /* another color code */
    c = b_voice(2, lc9, CC, b);
    b[13] ^= 0x0F, b[18] ^= 0x0F; /* its EMB broken */
    heard(e, 2, c, b);
    CHECK(poll_at(e, 0) && !s->rf.active && s->late_have == 0x10, "nothing yet: %02x", s->late_have);
    for (int n = 1; n <= 3; n++) {
        c = b_voice(n, lc9, CC, b);
        heard(e, 2, c, b);
    }
    c = b_voice(0, lc9, CC, b);
    heard(e, 2, c, b); /* A: starts over */
    CHECK(poll_at(e, 0) && !s->rf.active && s->late_have == 0, "A resets: %02x", s->late_have);
    /* B, C from one LC and D, E from another: no LC */
    for (int n = 1; n <= 4; n++) {
        c = b_voice(n, n <= 2 ? lc9 : other, CC, b);
        heard(e, 2, c, b);
    }
    CHECK(poll_at(e, 0) && !s->rf.active && s->late_have == 0, "mixed fragments");
    /* an LC with no source (not a voice LC) */
    for (int n = 1; n <= 4; n++) {
        c = b_voice(n, odd, CC, b);
        heard(e, 2, c, b);
    }
    CHECK(poll_at(e, 0) && !s->rf.active, "no source");
    e->log[0] = 0;
    for (int n = 1; n <= 4; n++) {
        c = b_voice(n, lc9, CC, b);
        heard(e, 2, c, b);
    }
    CHECK(poll_at(e, 0) && s->rf.active && s->rf.src == 9992345 && s->rf.dst == 9 && s->rf.group, "late entry");
    LOG_IS(e, "key1 K1 st d2:41 d2:41 d2:41 lc");
    /* E's own burst waits for A */
    CHECK(s->rf.voice.wait_a && !s->count, "waits for A");
}

/* RF data: headers, blocks of each rate, a header in the call, no blocks, a bad CRC. */
static void ed_rf_data(void)
{
    ed_t *e = ed_new("mmdvm_hs");
    rdof_repeater_t *r = &e->r;
    CHECK(ed_start(e), "start");
    rdof_rpt_slot_t *s = &r->slots[1];
    uint8_t b[RDOF_BURST_BYTES];
    b_block(RDOF_DT_RATE_12, b);
    heard(e, 1, RDOF_SYNC_DATA | RDOF_DT_RATE_12, b); /* a block with no call */
    b_data_header(5, 6, 3, false, b);
    heard(e, 1, C_DATA_HEADER, b); /* a bad CRC */
    heard(e, 1, RDOF_SYNC_DATA | RDOF_DT_MBC_HEADER, b);
    heard(e, 1, RDOF_SYNC_DATA | 9, b); /* idle */
    CHECK(poll_at(e, 0) && !s->rf.active && !s->count, "nothing");
    LOG_IS(e, "st");
    b_data_header(5, 6, 3, true, b);
    heard(e, 1, C_DATA_HEADER, b);
    CHECK(poll_at(e, 0) && s->rf.active && s->rf.kind == RDOF_KIND_DATA && s->rf.blocks == 3, "a data call");
    LOG_IS(e, "key1 K1 st d1:46 lc");
    CHECK(r->slots[1].activity.kind == RDOF_ACT_DATA && r->slots[1].activity.dst == 6, "data activity");
    uint8_t c = b_voice(0, NULL, CC, b);
    heard(e, 1, c, b); /* voice in a data call: only late entry */
    b_data_header(5, 6, 3, true, b);
    heard(e, 1, C_DATA_HEADER, b); /* R4.8: a header that checks: the next packet (a chain, or blocks lost) */
    b_data_header(5, 6, 3, false, b);
    heard(e, 1, C_DATA_HEADER, b); /* one that doesn't: nothing */
    {
        uint8_t pl[12] = {0x0F, 0x40, 1, 2, 3, 4, 5, 6, 7, 8}; /* a proprietary header (DPF 15): one of its blocks */
        b_payload(pl, RDOF_MASK_DATA_HEADER, RDOF_DT_DATA_HEADER, b);
    }
    heard(e, 1, C_DATA_HEADER, b);
    b_block(RDOF_DT_RATE_34, b);
    heard(e, 1, RDOF_SYNC_DATA | RDOF_DT_RATE_34, b);
    CHECK(poll_at(e, 0.06) && s->rf.active && s->rf.blocks == 1, "blocks left %d", s->rf.blocks);
    LOG_IS(e, "d1:46 d1:46 d1:48");
    b_block(RDOF_DT_RATE_1, b);
    heard(e, 1, RDOF_SYNC_DATA | RDOF_DT_RATE_1, b);
    CHECK(poll_at(e, 0.06) && !s->rf.active, "the last block ends it");
    LOG_IS(e, "st d1:4a lc");
    CHECK(r->slots[1].activity.kind == RDOF_ACT_NONE, "the activity goes with a data call's end");
    /* no blocks: over at once */
    b_data_header(5, 6, 0, true, b);
    heard(e, 1, C_DATA_HEADER, b);
    CHECK(poll_at(e, 0.06) && !s->rf.active, "no blocks");
    LOG_IS(e, "d1:46");
    /* a data header in a voice call starts a data call */
    b_header(1, 2, RDOF_DT_VOICE_LC_HEADER, b);
    heard(e, 1, C_HEADER, b);
    b_data_header(5, 6, 2, true, b);
    heard(e, 1, C_DATA_HEADER, b);
    CHECK(poll_at(e, 0.06) && s->rf.active && s->rf.kind == RDOF_KIND_DATA, "data after voice");
}

/* RF CSBKs: BS_Dwn_Act, a bad CRC, unaddressed, and not into the network's voice (R8.3). */
static void ed_rf_csbk(void)
{
    ed_t *e = ed_new("mmdvm_hs");
    rdof_repeater_t *r = &e->r;
    CHECK(ed_start(e), "start");
    rdof_rpt_slot_t *s = &r->slots[1];
    uint8_t b[RDOF_BURST_BYTES];
    b_csbk(RDOF_CSBK_PREAMBLE, 5, 6, false, b);
    heard(e, 1, C_CSBK, b);
    b_csbk(0x38, 5, 6, true, b);
    heard(e, 1, C_CSBK, b); /* BS_Dwn_Act with the transmitter on */
    CHECK(poll_at(e, 0) && !s->count && !s->has_last_rf, "neither");
    b_csbk(0x01, 5, 6, true, b);
    heard(e, 1, C_CSBK, b); /* unaddressed */
    CHECK(poll_at(e, 0) && !s->has_last_rf, "unaddressed");
    b_csbk(RDOF_CSBK_PREAMBLE, 5, 6, true, b);
    heard(e, 1, C_CSBK, b);
    CHECK(poll_at(e, 0) && s->has_last_rf && s->last_src == 5 && s->last_dst == 6 && s->last_at == e->clock,
          "addressed");
    e->log[0] = 0;
    /* a network voice call on slot 2, prebuffering (unkeyed: the key lead holds its bursts): an RF CSBK there
     * isn't repeated */
    e = ed_new("mmdvm_hs");
    r = &e->r;
    CHECK(ed_start(e), "start");
    net_voice_call(e, 0x1001, 2, 11, 12, 1, false);
    b_csbk(RDOF_CSBK_PREAMBLE, 5, 6, true, b);
    CHECK(poll_at(e, 0), "network");
    heard(e, 2, C_CSBK, b);
    CHECK(poll_at(e, 0) && !r->slots[2].count, "streaming");
    /* its bursts queued (the key lead), the stream ended */
    net_send(e, 0x1001, 2, C_TERM, b, true, 11, 12);
    CHECK(poll_at(e, 0.02) && r->slots[2].net < 0 && r->slots[2].count, "queued: %d", r->slots[2].count);
    int n = r->slots[2].count;
    heard(e, 2, C_CSBK, b);
    CHECK(poll_at(e, 0) && r->slots[2].count == n, "queued");
    /* gone to the modem, until it's aired */
    CHECK(poll_at(e, 0.5) && !r->slots[2].count, "sent");
    heard(e, 2, C_CSBK, b);
    CHECK(poll_at(e, 0) && !r->slots[2].count, "held by the modem");
}

/* The idle receiver: only BS_Dwn_Act wakes it (R3.1). */
static void ed_wakeup(void)
{
    ed_t *e = ed_new("mmdvm_hs");
    rdof_repeater_t *r = &e->r;
    CHECK(ed_start(e), "start");
    uint8_t b[RDOF_BURST_BYTES];
    b_header(1, 2, RDOF_DT_VOICE_LC_HEADER, b);
    heard(e, 1, RDOF_CONTROL_IDLE | C_HEADER, b);
    b_csbk(0x38, 5, 6, false, b);
    heard(e, 1, RDOF_CONTROL_IDLE | C_CSBK, b);
    b_csbk(RDOF_CSBK_PREAMBLE, 5, 6, true, b);
    heard(e, 1, RDOF_CONTROL_IDLE | C_CSBK, b);
    heard(e, 1, RDOF_CONTROL_IDLE | 0x20, b);
    CHECK(poll_at(e, 0) && !r->keyed, "not woken");
    LOG_IS(e, "st");
    b_csbk(0x38, 5, 6, true, b);
    heard(e, 1, RDOF_CONTROL_IDLE | C_CSBK, b);
    CHECK(poll_at(e, 0) && r->keyed && r->tx_until == e->clock + 5.0, "woken: %g", r->tx_until - e->clock);
    LOG_IS(e, "key1 K1");
    /* T_MSInactiv the longer of it and the call hang (R3.6) */
    r->inactivity_s = 1.0;
    heard(e, 1, RDOF_CONTROL_IDLE | C_CSBK, b);
    CHECK(poll_at(e, 10) && r->tx_until == e->clock + 3.0, "the call hang: %g", r->tx_until - e->clock);
    LOG_IS(e, "st");
    CHECK(poll_at(e, 3), "unkeys");
    LOG_IS(e, "key0 K0");
    /* the modem refuses the key */
    e->fail_cmd = RDOF_MMDVM_DMR_START;
    heard(e, 1, RDOF_CONTROL_IDLE | C_CSBK, b);
    CHECK(!poll_at(e, 0) && !r->ready, "a fault");
    LOG_IS(e, "x1d");
}

/* Network streams: ignored slots, headers that don't decode, PI headers, data calls, other streams, echoes. */
static void ed_network(void)
{
    ed_t *e = ed_new("mmdvm_hs");
    rdof_repeater_t *r = &e->r;
    CHECK(ed_start(e), "start");
    uint8_t b[RDOF_BURST_BYTES], zero[RDOF_BURST_BYTES] = {0}, lc9[9];
    r->enabled[2] = false;
    net_voice_call(e, 0x2001, 0, 1, 2, 1, true);
    net_voice_call(e, 0x2002, 3, 1, 2, 1, true);
    net_voice_call(e, 0x2003, 2, 1, 2, 1, true);
    CHECK(poll_at(e, 0) && r->slots[1].net < 0 && r->slots[2].net < 0 && !r->keyed, "slots 0, 3, and one disabled");
    CHECK(!r->slots[1].ndone && !r->slots[2].ndone, "not even marked done");
    LOG_IS(e, "st");
    r->enabled[2] = true;

    /* a header that doesn't decode: the LC from the addressing; later headers ignored (R7.2) */
    net_send(e, 0x2004, 1, C_HEADER, zero, false, 31, 32);
    b_header(41, 42, RDOF_DT_VOICE_LC_HEADER, b);
    net_send(e, 0x2004, 1, C_HEADER, b, false, 31, 32);
    net_send(e, 0x2004, 1, 0x07, zero, false, 31, 32); /* no position 7 */
    rdof_data_burst((const uint8_t[12]){0x05}, RDOF_DT_PI_HEADER, CC, rdof_data_sync(RDOF_MS_VOICE), b);
    net_send(e, 0x2004, 1, C_PI, b, false, 31, 32); /* before A: taken */
    uint8_t c = b_voice(0, NULL, CC, b);
    net_send(e, 0x2004, 1, c, b, false, 31, 32);
    rdof_data_burst((const uint8_t[12]){0x05}, RDOF_DT_PI_HEADER, CC, rdof_data_sync(RDOF_MS_VOICE), b);
    net_send(e, 0x2004, 1, C_PI, b, false, 31, 32); /* after A: not (R7.6) */
    /* another stream while it streams (R8.5) */
    net_voice_call(e, 0x2005, 1, 51, 52, 1, false);
    CHECK(poll_at(e, 0), "network");
    int idx = r->slots[1].net;
    CHECK(idx >= 0 && r->calls[idx].kind == RDOF_KIND_VOICE && r->calls[idx].received == 6, "the call: %d",
          idx >= 0 ? r->calls[idx].received : -1);
    rdof_group_lc(31, 32, true, 0, lc9);
    CHECK(idx >= 0 && !memcmp(r->calls[idx].lc9, lc9, 9), "the LC from its addressing");
    CHECK(idx >= 0 && !r->calls[idx].npending && r->slots[1].count == 5, "received 6 of 6, started: header x3, PI, A: %d",
          r->slots[1].count);
    CHECK(r->slots[1].ndone == 1 && r->slots[1].done[0][3] == 0x05, "the other stream refused");
    LOG_IS(e, "key1 K1 st lc");
    CHECK(poll_at(e, 0.5) && r->slots[1].net == idx, "after the key lead");
    LOG_IS(e, "st d1:41 tx:start:31>32 sent:41 d1:41 d1:41 d1:40 sent:40 d1:20 sent:20");
    CHECK(poll_at(e, 2.0) && r->slots[1].net < 0, "timed out");

    /* an RF call heard has the slot: the network stream is refused (R8.2), and its repeat comes back (R8.4) */
    e = ed_new("mmdvm_hs");
    r = &e->r;
    CHECK(ed_start(e), "start");
    b_header(61, 62, RDOF_DT_VOICE_LC_HEADER, b);
    heard(e, 1, C_HEADER, b);
    CHECK(poll_at(e, 0), "RF");
    net_voice_call(e, 0x3001, 1, 1, 2, 1, true);
    CHECK(poll_at(e, 0) && r->slots[1].net < 0 && r->slots[1].ndone == 1, "refused for RF");
    b_header(61, 62, RDOF_DT_TERMINATOR_WITH_LC, b);
    heard(e, 1, C_TERM, b);
    CHECK(poll_at(e, 0.1), "RF ends");
    net_voice_call(e, 0x3002, 1, 61, 62, 1, false);
    CHECK(poll_at(e, 1.0) && r->slots[1].net < 0 && r->slots[1].ndone == 2, "our own repeat");
    net_voice_call(e, 0x3003, 1, 61, 63, 1, false);
    net_voice_call(e, 0x3004, 2, 61, 62, 1, false);
    CHECK(poll_at(e, 0) && r->slots[1].net >= 0 && r->slots[2].net >= 0, "another group; the other slot");
    CHECK(poll_at(e, 2.0) && r->slots[1].net < 0 && r->slots[2].net < 0, "timed out");
    net_voice_call(e, 0x3005, 1, 61, 62, 1, false);
    CHECK(poll_at(e, 0) && r->slots[1].net >= 0, "an echo only within 2 s");

    /* network data: blocks, a header with a bad CRC or no blocks, a CSBK in a data call, PI headers */
    e = ed_new("mmdvm_hs");
    r = &e->r;
    CHECK(ed_start(e), "start");
    rdof_rpt_slot_t *s = &r->slots[2];
    b_data_header(71, 72, 2, true, b);
    net_send(e, 0x4001, 2, C_DATA_HEADER, b, false, 71, 72);
    rdof_data_burst((const uint8_t[12]){0x05}, RDOF_DT_PI_HEADER, CC, rdof_data_sync(RDOF_MS_VOICE), b);
    net_send(e, 0x4001, 2, C_PI, b, false, 71, 72); /* not in a data call */
    b_csbk(RDOF_CSBK_PREAMBLE, 71, 72, true, b);
    net_send(e, 0x4001, 2, C_CSBK, b, true, 71, 72); /* a CSBK ending a data call isn't one on its own */
    b_block(RDOF_DT_RATE_12, b);
    net_send(e, 0x4001, 2, RDOF_SYNC_DATA | RDOF_DT_RATE_12, b, false, 71, 72);
    CHECK(poll_at(e, 0) && s->net >= 0 && r->calls[s->net].blocks == 1, "one block left");
    CHECK(!r->calls[s->net].has_csbk, "no lone CSBK");
    b_block(RDOF_DT_RATE_34, b);
    net_send(e, 0x4001, 2, RDOF_SYNC_DATA | RDOF_DT_RATE_34, b, false, 71, 72);
    CHECK(poll_at(e, 0) && s->net < 0 && !s->ndone, "data: the stream isn't done (R7.9)");
    b_data_header(71, 72, 2, false, b);
    net_send(e, 0x4001, 2, C_DATA_HEADER, b, false, 71, 72);
    CHECK(poll_at(e, 0) && s->net < 0, "a bad CRC: no blocks to wait for");
    b_data_header(71, 72, 0, true, b);
    net_send(e, 0x4002, 2, C_DATA_HEADER, b, false, 71, 72);
    CHECK(poll_at(e, 0) && s->net < 0, "no blocks");
    /* a terminator in a call that isn't voice; it times out with a plain end */
    b_csbk(RDOF_CSBK_PREAMBLE, 71, 72, true, b);
    net_send(e, 0x4003, 2, C_CSBK, b, false, 71, 72);
    b_header(71, 72, RDOF_DT_TERMINATOR_WITH_LC, b);
    net_send(e, 0x4003, 2, C_TERM, b, true, 71, 72);
    CHECK(poll_at(e, 0) && s->net >= 0 && r->calls[s->net].kind == RDOF_KIND_NONE, "still open");
    idx = s->net;
    CHECK(poll_at(e, 2.0) && s->net < 0 && r->calls[idx].finished, "timed out");
}

/* Every call in use: the next stream is dropped; the out queue and a call's prebuffer full. */
static void ed_net_bounds(void)
{
    ed_t *e = ed_new("mmdvm_hs");
    rdof_repeater_t *r = &e->r;
    r->lock = ed_lock, r->unlock = ed_unlock;
    CHECK(ed_start(e), "start");
    e->f.held = 47; /* slot 2 has room for 1: nothing goes */
    for (int i = 0; i < RDOF_RPT_CALLS; i++)
        net_voice_call(e, 0x5001 + i, 2, 100 + i, 9, 0, true);
    CHECK(poll_at(e, 0), "calls");
    for (int i = 0; i < RDOF_RPT_CALLS; i++)
        CHECK(r->calls[i].used && r->calls[i].src == (uint32_t)(100 + i), "call %d: %lu", i,
              (unsigned long)r->calls[i].src);
    CHECK(r->slots[2].count == RDOF_RPT_CALLS * 5 && r->slots[2].net < 0, "queued: %d", r->slots[2].count);
    net_voice_call(e, 0x5100, 2, 200, 9, 0, true);
    CHECK(poll_at(e, 0) && r->dropped == 1 && r->slots[2].net < 0, "dropped: %u", r->dropped);
    CHECK(r->slots[2].ndone == RDOF_RPT_CALLS + 1, "marked done: %d", r->slots[2].ndone);
    /* a call held by the other slot's stream only */
    e = ed_new("mmdvm_hs");
    r = &e->r;
    CHECK(ed_start(e), "start");
    net_voice_call(e, 0x5200, 1, 1, 9, 0, false);
    net_voice_call(e, 0x5201, 2, 2, 9, 0, false);
    CHECK(poll_at(e, 0) && r->slots[1].net == 0 && r->slots[2].net == 1, "slots %d %d", r->slots[1].net,
          r->slots[2].net);

    /* the out queue full */
    e = ed_new("mmdvm_hs");
    r = &e->r;
    r->lock = ed_lock, r->unlock = ed_unlock;
    CHECK(ed_start(e), "start");
    e->f.held = 47;
    uint8_t b[RDOF_BURST_BYTES], lc9[9];
    rdof_group_lc(1, 9, true, 0, lc9);
    net_voice_call(e, 0x5300, 2, 1, 9, 5, false);
    CHECK(poll_at(e, 0.01), "poll");
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i < 60; i++) {
            uint8_t c = b_voice((5 + i) % 6, lc9, CC, b);
            net_send(e, 0x5300, 2, c, b, false, 1, 9);
        }
        CHECK(poll_at(e, 0.01), "poll");
    }
    CHECK(r->slots[2].count == RDOF_RPT_OUT && r->dropped == 3 + 5 + 240 - RDOF_RPT_OUT,
          "out: %d queued, %u dropped", r->slots[2].count, r->dropped);
    CHECK(!e->bad_lock && !e->depth, "the drops counted under the lock");

    /* a call's prebuffer full */
    e = ed_new("mmdvm_hs");
    r = &e->r;
    CHECK(ed_start(e), "start");
    r->prebuffer = 1000;
    net_voice_call(e, 0x5400, 2, 1, 9, 50, false);
    CHECK(poll_at(e, 0), "poll");
    rdof_group_lc(1, 9, true, 0, lc9);
    for (int i = 50; i < 100; i++) {
        uint8_t c = b_voice(i % 6, lc9, CC, b);
        net_send(e, 0x5400, 2, c, b, false, 1, 9);
    }
    CHECK(poll_at(e, 0) && r->calls[0].npending == RDOF_RPT_PENDING && r->dropped == 3 + 100 - RDOF_RPT_PENDING,
          "pending: %d, %u dropped", r->calls[0].npending, r->dropped);
}

/* The single frequency repeater: the inbound slot heard, the network yielding to it (section 12). */
static void ed_sfr(void)
{
    ed_t *e = ed_new("odr_sfr");
    rdof_repeater_t *r = &e->r;
    rdof_repeater_set_sfr(r, 1);
    r->receiver.hear = ed_hear, r->receiver.ctx = e;
    CHECK(ed_start(e), "start");
    uint8_t b[RDOF_BURST_BYTES], lc9[9];
    rdof_group_lc(1, 2, true, 0, lc9);
    uint8_t c = b_voice(0, lc9, CC, b);
    heard(e, 2, c, b); /* not the inbound slot */
    heard(e, 1, RDOF_CONTROL_IDLE | c, b);
    CHECK(poll_at(e, 0) && !e->heard && r->inbound_last == -INFINITY, "neither heard");
    LOG_IS(e, "st");
    heard(e, 1, c, b);
    CHECK(poll_at(e, 0) && e->heard == 1 && r->inbound_last == e->clock, "heard");
    CHECK(rdof_repeater_busy(r, 2) && !rdof_repeater_busy(r, 1), "the outbound busy while the inbound is");
    rdof_repeater_lost(r, 1);
    CHECK(poll_at(e, 0) && r->inbound_last == -INFINITY && !rdof_repeater_busy(r, 2), "lost");
    heard(e, 1, c, b);
    b_lc(lc9, RDOF_DT_TERMINATOR_WITH_LC, b);
    heard(e, 1, C_TERM, b);
    CHECK(poll_at(e, 0) && r->inbound_last == -INFINITY, "a terminator: over");
    LOG_IS(e, "");

    /* the network on the outbound: refused while the status says the inbound is busy, asked again */
    e->f.rf_busy = true;
    CHECK(poll_at(e, 2.0) && r->inbound_flag, "the flag");
    LOG_IS(e, "st");
    net_voice_call(e, 0x6001, 1, 1, 2, 1, false); /* slot 1 from the network: SFR sends it on 2 */
    CHECK(poll_at(e, 0) && r->slots[2].net < 0 && r->slots[2].ndone == 1, "refused");
    LOG_IS(e, "st");
    e->f.rf_busy = false;
    net_voice_call(e, 0x6002, 1, 1, 2, 1, false);
    CHECK(poll_at(e, 0.3) && r->slots[2].net >= 0, "taken");
    LOG_IS(e, "st K1");
    /* heard on the inbound: the network stream goes (R12.3) */
    c = b_voice(1, lc9, CC, b);
    heard(e, 1, c, b);
    CHECK(poll_at(e, 0.06) && r->slots[2].net < 0 && !r->slots[2].count, "RF takes it");
    LOG_IS(e, "ab2 st");
    CHECK(r->slots[2].ndone == 2, "done");
    /* within Q of the inbound: refused without asking */
    net_voice_call(e, 0x6003, 1, 1, 2, 1, false);
    CHECK(poll_at(e, 0.1) && r->slots[2].net < 0, "quiet not yet");
    LOG_IS(e, "");

    /* NAKs: RF busy on the outbound marks the inbound busy, and takes the slot; any other NAK, nothing */
    e = ed_new("odr_sfr");
    r = &e->r;
    rdof_repeater_set_sfr(r, 1);
    CHECK(ed_start(e), "start");
    CHECK(poll_at(e, 0), "the idle status"); /* it would clear the flag */
    LOG_IS(e, "st");
    rdof_repeater_nak(r, RDOF_MMDVM_DMR_DATA2, RDOF_NAK_RF_BUSY);
    CHECK(poll_at(e, 0) && r->inbound_flag, "flagged, nothing to take");
    LOG_IS(e, "");
    r->inbound_flag = false;
    rdof_repeater_nak(r, RDOF_MMDVM_DMR_DATA1, RDOF_NAK_RF_BUSY);
    rdof_repeater_nak(r, RDOF_MMDVM_DMR_DATA2, RDOF_NAK_RF_BUSY + 1);
    CHECK(poll_at(e, 0) && !r->inbound_flag, "not those");
    e->f.held = 47;
    net_voice_call(e, 0x6101, 2, 1, 2, 1, true);
    CHECK(poll_at(e, 0) && r->slots[2].net < 0 && r->slots[2].count, "queued");
    LOG_IS(e, "K1 st");
    rdof_repeater_nak(r, RDOF_MMDVM_DMR_DATA2, RDOF_NAK_RF_BUSY);
    CHECK(poll_at(e, 0) && !r->slots[2].count, "taken");
    LOG_IS(e, "ab2 st"); /* the abort asks for a status */
    e->f.held = 0;
    /* the abort refused */
    net_voice_call(e, 0x6102, 2, 1, 2, 1, false);
    CHECK(poll_at(e, 0) && r->slots[2].net >= 0, "streaming");
    e->fail_cmd = RDOF_MMDVM_DMR_ABORT;
    rdof_repeater_nak(r, RDOF_MMDVM_DMR_DATA2, RDOF_NAK_RF_BUSY);
    CHECK(!poll_at(e, 0) && !r->ready && r->slots[2].net < 0, "a fault");
    /* the outbound on slot 1; a non-SFR repeater ignores RF busy */
    e = ed_new("odr_sfr");
    r = &e->r;
    rdof_repeater_set_sfr(r, 2);
    CHECK(ed_start(e), "start");
    CHECK(poll_at(e, 0), "the idle status");
    rdof_repeater_nak(r, RDOF_MMDVM_DMR_DATA1, RDOF_NAK_RF_BUSY);
    CHECK(poll_at(e, 0) && r->inbound_flag, "outbound 1");
    e = ed_new("mmdvm_hs");
    CHECK(ed_start(e), "start");
    rdof_repeater_nak(&e->r, RDOF_MMDVM_DMR_DATA2, RDOF_NAK_RF_BUSY);
    CHECK(poll_at(e, 0) && !e->r.inbound_flag, "duplex");

    /* the status asked again, and not answered */
    e = ed_new("odr_sfr");
    r = &e->r;
    rdof_repeater_set_sfr(r, 1);
    CHECK(ed_start(e), "start");
    r->inbound_flag = true;
    e->f.drop_status = 1;
    net_voice_call(e, 0x6201, 2, 1, 2, 1, false);
    CHECK(!poll_at(e, 0) && !r->ready, "a fault");
}

/* RF on a slot whose queue holds the network's: those go, RF queued before stays (R8.1); the abort refused. */
static void ed_yield(void)
{
    ed_t *e = ed_new("mmdvm_hs");
    rdof_repeater_t *r = &e->r;
    CHECK(ed_start(e), "start");
    rdof_rpt_slot_t *s = &r->slots[1];
    uint8_t b[RDOF_BURST_BYTES];
    b_data_header(71, 72, 1, true, b);
    net_send(e, 0x7001, 1, C_DATA_HEADER, b, false, 71, 72);
    b_block(RDOF_DT_RATE_12, b);
    net_send(e, 0x7001, 1, RDOF_SYNC_DATA | RDOF_DT_RATE_12, b, false, 71, 72);
    CHECK(poll_at(e, 0) && s->net < 0 && s->count == 3, "network data queued (the key lead): %d", s->count);
    b_csbk(RDOF_CSBK_PREAMBLE, 5, 6, true, b);
    heard(e, 1, C_CSBK, b);
    CHECK(poll_at(e, 0) && s->count == 4, "an RF CSBK after it: %d", s->count);
    e->log[0] = 0;
    b_header(5, 6, RDOF_DT_VOICE_LC_HEADER, b);
    heard(e, 1, C_HEADER, b);
    CHECK(poll_at(e, 0), "RF voice");
    LOG_IS(e, "ab1 st d1:43 d1:41 d1:41 d1:41 lc");
    CHECK(s->ndone == 0, "data: not marked done");
    /* the same with the abort refused: by a voice header, a data header, late entry */
    for (int k = 0; k < 3; k++) {
        e = ed_new("mmdvm_hs");
        r = &e->r;
        CHECK(ed_start(e), "start");
        net_voice_call(e, 0x7002, 1, 1, 2, 1, false);
        CHECK(poll_at(e, 0) && r->slots[1].net >= 0, "streaming");
        LOG_IS(e, "key1 K1 st lc");
        e->fail_cmd = RDOF_MMDVM_DMR_ABORT;
        uint8_t lc9[9];
        rdof_group_lc(5, 6, true, 0, lc9);
        if (k == 0) {
            b_lc(lc9, RDOF_DT_VOICE_LC_HEADER, b);
            heard(e, 1, C_HEADER, b);
        } else if (k == 1) {
            b_data_header(5, 6, 1, true, b);
            heard(e, 1, C_DATA_HEADER, b);
        } else {
            for (int n = 1; n <= 4; n++) {
                uint8_t c = b_voice(n, lc9, CC, b);
                heard(e, 1, c, b);
            }
        }
        CHECK(!poll_at(e, 0) && !r->ready && r->slots[1].net < 0 && !r->slots[1].rf.active, "a fault (%d)", k);
        LOG_IS(e, "x1e");
    }
}

/* What goes to the modem: the status missed, the modem full, writes refused, fill, the call hang. */
static void ed_transmit(void)
{
    ed_t *e = ed_new("mmdvm_hs");
    rdof_repeater_t *r = &e->r;
    CHECK(ed_start(e), "start");
    uint8_t b[RDOF_BURST_BYTES], lc9[9];
    rdof_group_lc(5, 6, true, 0, lc9);
    b_lc(lc9, RDOF_DT_VOICE_LC_HEADER, b);
    heard(e, 1, C_HEADER, b);
    CHECK(poll_at(e, 0), "RF");
    e->log[0] = 0;
    /* status missed: carry on, until 2 s of it */
    e->f.drop_status = 1000;
    uint8_t c = b_voice(0, lc9, CC, b);
    heard(e, 1, c, b);
    CHECK(poll_at(e, 0.125) && r->has_status_missed && r->status_missed == e->clock, "missed");
    LOG_IS(e, "st");
    CHECK(poll_at(e, 1.0) && r->ready, "still");
    CHECK(poll_at(e, 0.5) && r->ready, "still");
    CHECK(poll_at(e, 0.5) && r->ready, "2 s exactly");
    CHECK(!poll_at(e, 0.25) && !r->ready && !r->slots[1].rf.active, "given up");
    e->f.drop_status = 0;

    /* the modem with room for one: nothing written */
    e = ed_new("mmdvm_hs");
    r = &e->r;
    CHECK(ed_start(e), "start");
    e->f.held1 = 47;
    b_lc(lc9, RDOF_DT_VOICE_LC_HEADER, b);
    heard(e, 1, C_HEADER, b);
    CHECK(poll_at(e, 0) && r->slots[1].count == 3, "held back: %d", r->slots[1].count);
    LOG_IS(e, "key1 K1 st lc");
    e->f.held1 = 0;
    CHECK(poll_at(e, 0.06) && !r->slots[1].count, "room");
    LOG_IS(e, "st d1:41 d1:41 d1:41");
    /* voice runs dry: silence (R6.3); a burst after a superframe of it goes where it says */
    c = b_voice(0, lc9, CC, b);
    heard(e, 1, c, b);
    CHECK(poll_at(e, 0.06), "A");
    for (int i = 0; i < 8; i++)
        CHECK(poll_at(e, 0.06), "dry");
    CHECK(r->slots[1].rf.voice.filled >= 6, "filled %d", r->slots[1].rf.voice.filled);
    int next = r->slots[1].rf.voice.next;
    c = b_voice((next + 2) % 6, lc9, CC, b);
    heard(e, 1, c, b);
    int before = r->slots[1].count;
    CHECK(poll_at(e, 0) && r->slots[1].rf.voice.next == (next + 3) % 6 && !r->slots[1].rf.voice.filled,
          "next %d", r->slots[1].rf.voice.next);
    CHECK(before == 0, "queued %d", before);

    /* each write refused: the queue, header fill, silence fill, the call hang's terminators */
    for (int k = 0; k < 4; k++) {
        e = ed_new("mmdvm_hs");
        r = &e->r;
        CHECK(ed_start(e), "start");
        b_lc(lc9, RDOF_DT_VOICE_LC_HEADER, b);
        heard(e, 1, C_HEADER, b);
        if (k == 0) {
            e->fail_cmd = RDOF_MMDVM_DMR_DATA1;
            CHECK(!poll_at(e, 0) && !r->ready, "queue (%d)", k);
            continue;
        }
        CHECK(poll_at(e, 0), "headers");
        if (k == 2) {
            c = b_voice(0, lc9, CC, b);
            heard(e, 1, c, b);
            CHECK(poll_at(e, 0.06), "A");
        } else if (k == 3) {
            b_lc(lc9, RDOF_DT_TERMINATOR_WITH_LC, b);
            heard(e, 1, C_TERM, b);
            CHECK(poll_at(e, 0.06) && r->slots[1].has_hang, "the hang");
        }
        e->fail_cmd = RDOF_MMDVM_DMR_DATA1;
        CHECK(!poll_at(e, 0.12) && !r->ready, "fill (%d)", k);
    }

    /* the call hang: terminators until T_CallHt, then the activity goes; the inactivity timer, then unkey */
    e = ed_new("mmdvm_hs");
    r = &e->r;
    CHECK(ed_start(e), "start");
    rdof_repeater_set_call_hang(r, 0.3);
    r->inactivity_s = 0.5;
    b_lc(lc9, RDOF_DT_VOICE_LC_HEADER, b);
    heard(e, 1, C_HEADER, b);
    b_lc(lc9, RDOF_DT_TERMINATOR_WITH_LC, b);
    heard(e, 1, C_TERM, b);
    CHECK(poll_at(e, 0) && r->slots[1].has_hang, "hang");
    LOG_IS(e, "key1 K1 st d1:41 d1:41 d1:41 d1:42 lc");
    /* from the terminator the modem holds last: 4 bursts out, less one */
    CHECK(fabs(r->slots[1].hang_until - (e->clock + 0.18 + 0.3)) < 1e-9, "until %g", r->slots[1].hang_until - e->clock);
    CHECK(poll_at(e, 0.2), "hang");
    LOG_IS(e, "st d1:42 d1:42");
    CHECK(poll_at(e, 0.2) && r->slots[1].has_hang, "the last one");
    LOG_IS(e, "st d1:42");
    CHECK(poll_at(e, 0.1) && !r->slots[1].has_hang && r->slots[1].activity.kind == RDOF_ACT_NONE, "hang over");
    LOG_IS(e, "st lc");
    CHECK(poll_at(e, 0.2) && r->keyed, "inactivity");
    LOG_IS(e, "");
    CHECK(poll_at(e, 0.05) && !r->keyed, "unkeyed");
    LOG_IS(e, "key0 K0");

    /* not unkeyed while an RF data call or a network stream is open (no traffic) */
    for (int k = 0; k < 4; k++) {
        e = ed_new("mmdvm_hs");
        r = &e->r;
        CHECK(ed_start(e), "start");
        r->inactivity_s = 0, rdof_repeater_set_call_hang(r, 0);
        int n = k < 2 ? 2 : 1;
        if (k % 2 == 0) {
            b_data_header(5, 6, 2, true, b);
            heard(e, n, C_DATA_HEADER, b);
        } else {
            r->prebuffer = 100;
            net_voice_call(e, 0x8001, n, 1, 2, 1, false);
        }
        CHECK(poll_at(e, 0) && r->keyed, "keyed (%d)", k);
        CHECK(poll_at(e, 0.5) && r->keyed, "held (%d)", k);
    }

    /* the key refused for RF traffic; the unkey refused */
    e = ed_new("mmdvm_hs");
    r = &e->r;
    CHECK(ed_start(e), "start");
    e->fail_cmd = RDOF_MMDVM_DMR_START;
    b_lc(lc9, RDOF_DT_VOICE_LC_HEADER, b);
    heard(e, 1, C_HEADER, b);
    CHECK(!poll_at(e, 0) && !r->ready && !r->slots[1].rf.active, "key refused");
    e = ed_new("mmdvm_hs");
    r = &e->r;
    CHECK(ed_start(e), "start");
    e->fail_cmd = RDOF_MMDVM_DMR_START, e->fail_skip = 1;
    b_csbk(0x38, 5, 6, true, b);
    heard(e, 1, RDOF_CONTROL_IDLE | C_CSBK, b);
    CHECK(poll_at(e, 0) && r->keyed, "keyed");
    CHECK(!poll_at(e, 6) && !r->ready, "unkey refused");
    LOG_IS(e, "key1 K1 st x1d");

    /* a data call's end with another call on the slot keeps the activity */
    e = ed_new("mmdvm_hs");
    r = &e->r;
    CHECK(ed_start(e), "start");
    e->f.held1 = 47;
    b_data_header(5, 6, 0, true, b);
    heard(e, 1, C_DATA_HEADER, b);
    b_data_header(7, 8, 3, true, b);
    heard(e, 1, C_DATA_HEADER, b);
    CHECK(poll_at(e, 0), "two");
    e->f.held1 = 0;
    CHECK(poll_at(e, 0.1) && r->slots[1].activity.kind == RDOF_ACT_DATA && r->slots[1].activity.dst == 8, "kept");
    /* the short LC refused */
    e = ed_new("mmdvm_hs");
    r = &e->r;
    CHECK(ed_start(e), "start");
    e->fail_cmd = RDOF_MMDVM_DMR_SHORTLC;
    b_lc(lc9, RDOF_DT_VOICE_LC_HEADER, b);
    heard(e, 1, C_HEADER, b);
    CHECK(!poll_at(e, 0) && !r->ready, "short LC refused");
}

/* A fault: the slots' queues dropped, the calls on air ended once each. */
static void ed_fault(void)
{
    ed_t *e = ed_new("mmdvm_hs");
    rdof_repeater_t *r = &e->r;
    CHECK(ed_start(e), "start");
    net_voice_call(e, 0x9001, 2, 1, 2, 5, false);
    CHECK(poll_at(e, 0), "network");
    CHECK(poll_at(e, 0.4), "on air");
    net_voice_call(e, 0x9001, 2, 1, 2, 6, false);
    e->log[0] = 0;
    e->fail_cmd = RDOF_MMDVM_DMR_DATA2;
    CHECK(!poll_at(e, 0.1) && !r->ready, "a fault");
    LOG_IS(e, "st x1a tx:end:1>2");
    CHECK(!r->slots[2].count && r->slots[2].net < 0 && !r->keyed, "dropped");
    /* a stream that keyed nothing yet: marked done */
    e = ed_new("mmdvm_hs");
    r = &e->r;
    CHECK(ed_start(e), "start");
    e->fail_cmd = RDOF_MMDVM_DMR_START;
    net_voice_call(e, 0x9002, 1, 1, 2, 1, false);
    CHECK(!poll_at(e, 0) && r->slots[1].net < 0 && r->slots[1].ndone == 1, "the key refused");
    LOG_IS(e, "x1d");
}

/* The CW ID after unkey: waiting for the transmitter, and each step refused. */
static bool ed_tx_on(ed_t *e)
{
    return e->f.transmitting;
}

static void ed_key_hook(void *ctx, bool keyed)
{
    ed_t *e = ctx;
    ed_log(e, "K%d", keyed);
    if (!keyed)
        e->f.transmitting = true; /* still sending when unkeyed */
}

static ed_t *ed_cw(int fail, int skip, bool transmitting, bool drain)
{
    ed_t *e = ed_new("mmdvm_hs");
    rdof_repeater_t *r = &e->r;
    snprintf(r->cwid, sizeof r->cwid, "RP01");
    CHECK(ed_start(e), "start");
    uint8_t b[RDOF_BURST_BYTES];
    b_csbk(RDOF_CSBK_PREAMBLE, 5, 6, true, b);
    heard(e, 1, C_CSBK, b);
    CHECK(poll_at(e, 0), "sent");
    if (transmitting)
        r->on_key = ed_key_hook;
    e->drain_on_sleep = drain;
    e->fail_cmd = fail, e->fail_skip = skip;
    e->log[0] = 0;
    return e;
}

static void ed_cwid(void)
{
    ed_t *e = ed_cw(-1, 0, false, false);
    CHECK(poll_at(e, 6) && !e->r.keyed && !e->r.sent_since_id && e->r.last_id == e->clock, "CW ID");
    LOG_IS(e, "key0 K0 st mode0 cw st mode2");
    CHECK(e->f.ncw == 1 && !strcmp(e->f.cw[0], "RP01"), "sent %s", e->f.cw[0]);
    e = ed_cw(-1, 0, true, true);
    CHECK(poll_at(e, 6) && e->sleeps == 1, "waited %d", e->sleeps);
    LOG_IS(e, "key0 K0 st st mode0 cw st mode2");
    e = ed_cw(-1, 0, true, false);
    CHECK(poll_at(e, 6) && e->sleeps == 300 && ed_tx_on(e), "gave up waiting: %d", e->sleeps);
    e = ed_cw(-1, 0, true, false);
    e->m.port.sleep_ms = NULL;
    CHECK(poll_at(e, 6) && e->sleeps == 0, "no sleep");
    const struct {
        int cmd, skip;
        const char *log;
    } fails[] = {
        {RDOF_MMDVM_GET_STATUS, 0, "key0 K0 x01"},
        {RDOF_MMDVM_SET_MODE, 0, "key0 K0 st x03"},
        {RDOF_MMDVM_SEND_CWID, 0, "key0 K0 st mode0 x0a"},
        {RDOF_MMDVM_GET_STATUS, 1, "key0 K0 st mode0 cw x01"},
        {RDOF_MMDVM_SET_MODE, 1, "key0 K0 st mode0 cw st x03"},
    };
    for (size_t i = 0; i < sizeof fails / sizeof fails[0]; i++) {
        e = ed_cw(fails[i].cmd, fails[i].skip, false, false);
        CHECK(!poll_at(e, 6) && !e->r.ready, "refused %zu", i);
        LOG_IS(e, fails[i].log);
    }
    /* not again until cwid_every_s, nor with nothing sent since */
    e = ed_cw(-1, 0, false, false);
    CHECK(poll_at(e, 6), "CW ID");
    b_csbk(RDOF_CSBK_PREAMBLE, 5, 6, true, (uint8_t[RDOF_BURST_BYTES]){0});
    e->log[0] = 0;
    e->r.keyed = true, e->r.sent_since_id = true;
    CHECK(poll_at(e, 1) && !e->r.keyed, "unkey");
    LOG_IS(e, "key0 K0");
    e->r.keyed = true, e->r.sent_since_id = false, e->r.last_id = -INFINITY;
    CHECK(poll_at(e, 1) && !e->r.keyed, "unkey");
    LOG_IS(e, "key0 K0");
}

/* done[]: the last RDOF_RPT_DONE stream ids. */
static void ed_done_ring(void)
{
    ed_t *e = ed_new("mmdvm_hs");
    rdof_repeater_t *r = &e->r;
    CHECK(ed_start(e), "start");
    uint8_t b[RDOF_BURST_BYTES];
    b_header(5, 6, RDOF_DT_VOICE_LC_HEADER, b);
    heard(e, 1, C_HEADER, b);
    CHECK(poll_at(e, 0), "RF");
    for (int i = 0; i < RDOF_RPT_DONE + 1; i++) {
        net_send(e, 0xA000 + i, 1, 0x20, b, false, 1, 2);
        if (i % 32 == 31)
            CHECK(poll_at(e, 0), "poll");
    }
    CHECK(poll_at(e, 0) && r->slots[1].ndone == RDOF_RPT_DONE && r->slots[1].done_next == 1, "ndone %d next %d",
          r->slots[1].ndone, r->slots[1].done_next);
    CHECK(r->slots[1].done[0][2] == 0xA0 && r->slots[1].done[0][3] == RDOF_RPT_DONE, "the oldest replaced");
}

/* No hooks: a network call on air and ended, then unkeyed. */
static void ed_no_hooks(void)
{
    ed_t *e = ed_new("mmdvm_hs");
    rdof_repeater_t *r = &e->r;
    r->on_tx = NULL, r->on_sent = NULL, r->on_key = NULL;
    CHECK(ed_start(e), "start");
    net_voice_call(e, 0xB001, 1, 1, 2, 6, true);
    CHECK(poll_at(e, 0) && poll_at(e, 0.5), "on air");
    CHECK(r->calls[0].finished && !r->calls[0].on_air, "ended");
    CHECK(poll_at(e, 6) && r->keyed && !r->slots[1].has_hang, "the hang over");
    CHECK(poll_at(e, 0) && !r->keyed, "unkeyed");
    LOG_IS(e, "key1 st lc st d1:41 d1:41 d1:41 d1:20 d1:01 d1:02 d1:03 d1:04 d1:05 d1:42 st lc key0");
}

/* SFR: unkeyed without DMR_START; the network's bursts still queued when the inbound is heard. */
static void ed_sfr_more(void)
{
    ed_t *e = ed_new("odr_sfr");
    rdof_repeater_t *r = &e->r;
    rdof_repeater_set_sfr(r, 1);
    CHECK(ed_start(e), "start");
    e->f.held = 47;
    net_voice_call(e, 0xC001, 2, 1, 2, 6, true);
    CHECK(poll_at(e, 0) && r->slots[2].net < 0 && r->slots[2].count == 11, "queued: %d", r->slots[2].count);
    LOG_IS(e, "K1 st");
    uint8_t b[RDOF_BURST_BYTES], lc9[9];
    rdof_group_lc(5, 6, true, 0, lc9);
    uint8_t c = b_voice(0, lc9, CC, b);
    heard(e, 1, c, b);
    CHECK(poll_at(e, 0) && !r->slots[2].count, "taken");
    LOG_IS(e, "ab2 st");
    e->f.held = 0;
    CHECK(poll_at(e, 6) && !r->keyed, "unkeyed");
    LOG_IS(e, "K0");
}

/* RF on a slot: the network's bursts the modem still holds (R8.1); RF queued ahead of the network's stays. */
static void ed_yield_more(void)
{
    ed_t *e = ed_new("mmdvm_hs");
    rdof_repeater_t *r = &e->r;
    CHECK(ed_start(e), "start");
    uint8_t b[RDOF_BURST_BYTES];
    net_voice_call(e, 0xD001, 1, 1, 2, 6, true);
    CHECK(poll_at(e, 0) && poll_at(e, 0.4), "network sent");
    CHECK(r->slots[1].net < 0 && !r->slots[1].count && r->slots[1].net_until > e->clock, "the modem holds it");
    e->log[0] = 0;
    b_header(5, 6, RDOF_DT_VOICE_LC_HEADER, b);
    heard(e, 1, C_HEADER, b);
    CHECK(poll_at(e, 0) && r->slots[1].rf.active, "RF");
    LOG_IS(e, "ab1 st d1:41 d1:41 d1:41 lc");

    e = ed_new("mmdvm_hs");
    r = &e->r;
    CHECK(ed_start(e), "start");
    e->f.held1 = 47;
    b_csbk(RDOF_CSBK_PREAMBLE, 5, 6, true, b);
    heard(e, 1, C_CSBK, b);
    CHECK(poll_at(e, 0) && r->slots[1].count == 1, "RF first");
    b_csbk(RDOF_CSBK_PREAMBLE, 5, 6, true, b);
    heard(e, 1, C_CSBK, b); /* one RF burst queued ahead: still not the network's */
    CHECK(poll_at(e, 0) && r->slots[1].count == 2, "RF again");
    b_data_header(71, 72, 1, true, b);
    net_send(e, 0xD002, 1, C_DATA_HEADER, b, false, 71, 72);
    CHECK(poll_at(e, 0) && r->slots[1].count == 2 && r->slots[1].net >= 0, "the network's prebuffering");
    b_csbk(RDOF_CSBK_PREAMBLE, 5, 6, true, b);
    heard(e, 1, C_CSBK, b); /* a network data stream on the slot: not voice */
    CHECK(poll_at(e, 0) && r->slots[1].count == 3, "repeated");
    r->prebuffer = 1;
    b_block(RDOF_DT_RATE_12, b);
    net_send(e, 0xD002, 1, RDOF_SYNC_DATA | RDOF_DT_RATE_12, b, false, 71, 72);
    CHECK(poll_at(e, 0) && r->slots[1].count == 6, "the network's after: %d", r->slots[1].count);
    e->log[0] = 0;
    b_header(5, 6, RDOF_DT_VOICE_LC_HEADER, b);
    heard(e, 1, C_HEADER, b);
    CHECK(poll_at(e, 0) && r->slots[1].rf.active, "RF");
    LOG_IS(e, "ab1 st d1:43 d1:43 d1:43 d1:41 d1:41 d1:41 lc"); /* the RF CSBKs kept, the network's data gone */
}

/* RF bursts that don't start or belong to a call. */
static void ed_rf_odd(void)
{
    ed_t *e = ed_new("mmdvm_hs");
    rdof_repeater_t *r = &e->r;
    CHECK(ed_start(e), "start");
    rdof_rpt_slot_t *s = &r->slots[1];
    uint8_t b[RDOF_BURST_BYTES], lc9[9];
    const uint8_t odd[9] = {0x3F}; /* an FLCO with no addressing */
    b_lc(odd, RDOF_DT_VOICE_LC_HEADER, b);
    heard(e, 1, C_HEADER, b);
    heard(e, 0, C_HEADER, b);
    rdof_data_burst((const uint8_t[12]){0x05}, RDOF_DT_PI_HEADER, CC, rdof_data_sync(RDOF_MS_VOICE), b);
    heard(e, 1, C_PI, b);
    b_block(RDOF_DT_RATE_12, b);
    heard(e, 1, RDOF_SYNC_DATA | RDOF_DT_RATE_12, b);
    rdof_group_lc(5, 6, true, 0, lc9);
    uint8_t c = b_voice(0, lc9, CC, b);
    heard(e, 1, c & 0x0F, b); /* A without its sync: not late entry */
    c = b_voice(1, lc9, CC, b);
    heard(e, 1, 0x02, b); /* B's fragment as C */
    CHECK(poll_at(e, 0) && !s->rf.active && !s->count && !s->late_have, "nothing");
    LOG_IS(e, "st");

    /* in a data call: a terminator, a PI header, a data header with a bad CRC; a voice header starts voice */
    b_data_header(5, 6, 2, true, b);
    heard(e, 1, C_DATA_HEADER, b);
    b_lc(lc9, RDOF_DT_TERMINATOR_WITH_LC, b);
    heard(e, 1, C_TERM, b);
    rdof_data_burst((const uint8_t[12]){0x05}, RDOF_DT_PI_HEADER, CC, rdof_data_sync(RDOF_MS_VOICE), b);
    heard(e, 1, C_PI, b);
    b_data_header(5, 6, 2, false, b);
    heard(e, 1, C_DATA_HEADER, b);
    CHECK(poll_at(e, 0) && s->rf.active && s->rf.blocks == 2, "the data call: %d", s->rf.blocks);
    LOG_IS(e, "key1 K1 st d1:46 lc");
    b_header(5, 7, RDOF_DT_VOICE_LC_HEADER, b);
    heard(e, 1, C_HEADER, b);
    CHECK(poll_at(e, 0) && s->rf.active && s->rf.kind == RDOF_KIND_VOICE && s->rf.dst == 7, "voice");
    /* a block in a voice call; the same talker to another group: a new call */
    b_block(RDOF_DT_RATE_12, b);
    heard(e, 1, RDOF_SYNC_DATA | RDOF_DT_RATE_12, b);
    b_header(5, 8, RDOF_DT_VOICE_LC_HEADER, b);
    heard(e, 1, C_HEADER, b);
    CHECK(poll_at(e, 0) && s->rf.active && s->rf.dst == 8, "a new call");
    LOG_IS(e, "d1:41 d1:41 d1:41 lc d1:42 d1:41 d1:41 d1:41 lc");
}

/* The network: a header with no addressing in it, a second data header. */
static void ed_net_odd(void)
{
    ed_t *e = ed_new("mmdvm_hs");
    rdof_repeater_t *r = &e->r;
    CHECK(ed_start(e), "start");
    uint8_t b[RDOF_BURST_BYTES], lc9[9];
    const uint8_t odd[9] = {0x3F};
    b_lc(odd, RDOF_DT_VOICE_LC_HEADER, b);
    net_send(e, 0xE001, 1, C_HEADER, b, false, 31, 32);
    CHECK(poll_at(e, 0) && r->slots[1].net == 0, "streaming");
    rdof_group_lc(31, 32, true, 0, lc9);
    CHECK(!memcmp(r->calls[0].lc9, lc9, 9), "the LC from its addressing");
    b_data_header(71, 72, 2, true, b);
    net_send(e, 0xE002, 2, C_DATA_HEADER, b, false, 71, 72);
    net_send(e, 0xE002, 2, C_DATA_HEADER, b, false, 71, 72);
    CHECK(poll_at(e, 0) && r->slots[2].net == 1 && r->calls[1].blocks == 2 && r->calls[1].npending == 2,
          "a second header: %d blocks, %d pending", r->calls[1].blocks, r->calls[1].npending);
}

/* Filler when the modem has no room for two (R10.2): headers, silence, the call hang's terminators. */
static void ed_fill_full(void)
{
    for (int k = 0; k < 3; k++) {
        ed_t *e = ed_new("mmdvm_hs");
        rdof_repeater_t *r = &e->r;
        CHECK(ed_start(e), "start");
        uint8_t b[RDOF_BURST_BYTES], lc9[9];
        rdof_group_lc(5, 6, true, 0, lc9);
        b_lc(lc9, RDOF_DT_VOICE_LC_HEADER, b);
        heard(e, 1, C_HEADER, b);
        if (k == 1) {
            uint8_t c = b_voice(0, lc9, CC, b);
            heard(e, 1, c, b);
        } else if (k == 2) {
            b_lc(lc9, RDOF_DT_TERMINATOR_WITH_LC, b);
            heard(e, 1, C_TERM, b);
        }
        e->f.held1 = k ? 43 : 44; /* room for the queue and one more */
        CHECK(poll_at(e, 0) && !r->slots[1].count && r->slots[1].space == 1, "space %d", r->slots[1].space);
        static const char *const want[] = {"key1 K1 st d1:41 d1:41 d1:41 lc", "key1 K1 st d1:41 d1:41 d1:41 d1:20 lc",
                                           "key1 K1 st d1:41 d1:41 d1:41 d1:42 lc"};
        LOG_IS(e, want[k]);
        CHECK(poll_at(e, 0.2) && r->slots[1].space == 1, "still full");
        LOG_IS(e, "st"); /* no filler with room for one */
    }
}

/* The call hang ends while a network stream that's no call yet holds the slot: the activity stays. */
static void ed_hang_net(void)
{
    ed_t *e = ed_new("mmdvm_hs");
    rdof_repeater_t *r = &e->r;
    CHECK(ed_start(e), "start");
    rdof_repeater_set_call_hang(r, 0.3);
    uint8_t b[RDOF_BURST_BYTES], lc9[9];
    rdof_group_lc(5, 6, true, 0, lc9);
    b_lc(lc9, RDOF_DT_VOICE_LC_HEADER, b);
    heard(e, 1, C_HEADER, b);
    b_lc(lc9, RDOF_DT_TERMINATOR_WITH_LC, b);
    heard(e, 1, C_TERM, b);
    CHECK(poll_at(e, 0) && r->slots[1].has_hang, "hang");
    b_csbk(RDOF_CSBK_PREAMBLE, 9, 6, true, b);
    net_send(e, 0xF001, 1, C_CSBK, b, false, 9, 6);
    CHECK(poll_at(e, 0.2) && r->slots[1].net >= 0 && r->slots[1].has_hang, "the stream");
    CHECK(poll_at(e, 0.2) && poll_at(e, 0.1) && !r->slots[1].has_hang, "hang over");
    CHECK(r->slots[1].activity.kind == RDOF_ACT_VOICE && r->slots[1].activity.dst == 6, "kept");
}

/* A data call's end: after its last burst has gone out; with a network stream on the slot. */
static void ed_data_end(void)
{
    ed_t *e = ed_new("mmdvm_hs");
    rdof_repeater_t *r = &e->r;
    CHECK(ed_start(e), "start");
    uint8_t b[RDOF_BURST_BYTES];
    b_data_header(5, 6, 2, true, b);
    heard(e, 1, C_DATA_HEADER, b);
    CHECK(poll_at(e, 0) && r->slots[1].activity.kind == RDOF_ACT_DATA, "data");
    CHECK(poll_at(e, 1.5) && !r->slots[1].rf.active && r->slots[1].activity.kind == RDOF_ACT_NONE, "timed out");
    LOG_IS(e, "key1 K1 st d1:46 lc st lc");

    e = ed_new("mmdvm_hs");
    r = &e->r;
    CHECK(ed_start(e), "start");
    e->f.held1 = 47;
    b_data_header(5, 6, 1, true, b);
    heard(e, 1, C_DATA_HEADER, b);
    b_block(RDOF_DT_RATE_12, b);
    heard(e, 1, RDOF_SYNC_DATA | RDOF_DT_RATE_12, b);
    CHECK(poll_at(e, 0) && !r->slots[1].rf.active && r->slots[1].count == 3, "ended, queued");
    b_csbk(RDOF_CSBK_PREAMBLE, 9, 6, true, b);
    net_send(e, 0xF101, 1, C_CSBK, b, false, 9, 6);
    CHECK(poll_at(e, 0) && r->slots[1].net >= 0, "the stream");
    e->f.held1 = 0;
    CHECK(poll_at(e, 0.1) && !r->slots[1].count, "sent");
    CHECK(r->slots[1].activity.kind == RDOF_ACT_DATA, "kept");
}

/* The end's duration: from the call's first burst, never negative. */
static void ed_duration(void)
{
    for (int back = 0; back < 2; back++) {
        ed_t *e = ed_new("mmdvm_hs");
        rdof_repeater_t *r = &e->r;
        CHECK(ed_start(e), "start");
        net_voice_call(e, 0x1101, 2, 1, 2, 6, false);
        CHECK(poll_at(e, 0) && poll_at(e, 0.375), "on air");
        if (back)
            e->clock -= 0.13; /* before its first burst, after the key lead */
        net_voice_call(e, 0x1101, 2, 1, 2, 1, false);
        e->fail_cmd = RDOF_MMDVM_DMR_DATA2;
        CHECK(!poll_at(e, 0.125) && !r->ready, "a fault");
        CHECK(e->duration == (back ? 0.0 : 0.125), "duration %g", e->duration);
    }
}

/* Voice before A is dropped (R6.1); a superframe of silence exactly, then a burst: taken. */
static void ed_voice_out(void)
{
    ed_t *e = ed_new("mmdvm_hs");
    rdof_repeater_t *r = &e->r;
    CHECK(ed_start(e), "start");
    uint8_t b[RDOF_BURST_BYTES], lc9[9];
    rdof_group_lc(1, 2, true, 0, lc9);
    b_lc(lc9, RDOF_DT_VOICE_LC_HEADER, b);
    net_send(e, 0x1201, 1, C_HEADER, b, false, 1, 2);
    uint8_t c = b_voice(1, lc9, CC, b);
    net_send(e, 0x1201, 1, c, b, false, 1, 2);
    c = b_voice(0, lc9, CC, b);
    net_send(e, 0x1201, 1, c, b, false, 1, 2);
    net_send(e, 0x1201, 1, 0x06, b, false, 1, 2); /* no position 6 */
    c = b_voice(2, lc9, CC, b);
    net_send(e, 0x1201, 1, c, b, false, 1, 2); /* B missing: silence, unreported */
    CHECK(poll_at(e, 0) && r->slots[1].net == 0, "streaming");
    rdof_rpt_call_t *k = &r->calls[0];
    CHECK(k->npending == 6, "header x3, A, silence, C: %d", k->npending);
    CHECK(k->pending[3].control == 0x20 && k->pending[3].report, "A");
    CHECK(k->pending[4].control == 0x01 && !k->pending[4].report, "silence");
    CHECK(k->pending[5].control == 0x02 && k->pending[5].report, "C");

    e = ed_new("mmdvm_hs");
    r = &e->r;
    CHECK(ed_start(e), "start");
    b_lc(lc9, RDOF_DT_VOICE_LC_HEADER, b);
    heard(e, 1, C_HEADER, b);
    c = b_voice(0, lc9, CC, b);
    heard(e, 1, c, b);
    CHECK(poll_at(e, 0), "A");
    rdof_voice_out_t *v = &r->slots[1].rf.voice;
    for (int i = 0; i < 40 && v->filled < 6; i++)
        CHECK(poll_at(e, 0.06), "dry");
    CHECK(v->filled == 6, "filled %d", v->filled);
    c = b_voice(v->next, lc9, CC, b);
    heard(e, 1, c, b);
    CHECK(poll_at(e, 0) && !v->filled, "taken: filled %d", v->filled);
}

/* The inactivity timer restarted by each wakeup; the CW ID at cwid_every_s exactly. */
static void ed_timers(void)
{
    ed_t *e = ed_new("mmdvm_hs");
    rdof_repeater_t *r = &e->r;
    CHECK(ed_start(e), "start");
    uint8_t b[RDOF_BURST_BYTES];
    b_csbk(0x38, 5, 6, true, b);
    heard(e, 1, RDOF_CONTROL_IDLE | C_CSBK, b);
    CHECK(poll_at(e, 0) && r->keyed, "keyed");
    heard(e, 1, RDOF_CONTROL_IDLE | C_CSBK, b);
    CHECK(poll_at(e, 4) && r->keyed, "again");
    CHECK(poll_at(e, 2) && r->keyed, "restarted");
    CHECK(poll_at(e, 3) && !r->keyed, "unkeyed");

    e = ed_cw(-1, 0, false, false);
    e->r.last_id = 106.0; /* 900 s before the unkey at 1006 */
    CHECK(poll_at(e, 6) && e->f.ncw == 1, "CW ID at 900 s");
}

/* The quiet time after the inbound (Q, R12.3) and the network's echo (R8.4) end at their times exactly; the
 * timeouts don't. */
static void ed_edges(void)
{
    ed_t *e = ed_new("odr_sfr");
    rdof_repeater_t *r = &e->r;
    rdof_repeater_set_sfr(r, 1);
    CHECK(ed_start(e), "start");
    e->clock = 0;
    uint8_t b[RDOF_BURST_BYTES], lc9[9];
    rdof_group_lc(5, 6, true, 0, lc9);
    uint8_t c = b_voice(0, lc9, CC, b);
    heard(e, 1, c, b);
    CHECK(poll_at(e, 0) && r->inbound_last == 0, "heard");
    e->clock = 0.25;
    CHECK(rdof_repeater_busy(r, 2), "busy");
    e->clock = 0.3;
    CHECK(!rdof_repeater_busy(r, 2), "quiet");
    r->inbound_flag = true; /* old: asked again at Q exactly */
    net_voice_call(e, 0x1301, 2, 1, 2, 1, false);
    CHECK(poll_at(e, 0) && r->slots[2].net >= 0, "taken");

    e = ed_new("mmdvm_hs");
    r = &e->r;
    CHECK(ed_start(e), "start");
    b_csbk(RDOF_CSBK_PREAMBLE, 5, 6, true, b);
    heard(e, 1, C_CSBK, b);
    CHECK(poll_at(e, 0) && r->slots[1].last_at == 1000.0, "RF");
    e->clock = 1002.0;
    net_voice_call(e, 0x1302, 1, 5, 6, 1, false);
    CHECK(poll_at(e, 0) && r->slots[1].net >= 0, "not an echo at 2 s");
    CHECK(poll_at(e, 1.5) && r->slots[1].net >= 0, "1.5 s: not yet timed out");
    CHECK(poll_at(e, 0.25) && r->slots[1].net < 0, "timed out");

    e = ed_new("mmdvm_hs");
    r = &e->r;
    CHECK(ed_start(e), "start");
    b_lc(lc9, RDOF_DT_VOICE_LC_HEADER, b);
    heard(e, 1, C_HEADER, b);
    CHECK(poll_at(e, 0) && poll_at(e, 1.0) && r->slots[1].rf.active, "1 s: not yet timed out");
    CHECK(poll_at(e, 0.25) && !r->slots[1].rf.active, "timed out");
}

/* The network's last burst still on air until net_until: RF after it is clear of it. */
static void ed_net_until(void)
{
    for (int k = 0; k < 2; k++) {
        ed_t *e = ed_new("mmdvm_hs");
        rdof_repeater_t *r = &e->r;
        CHECK(ed_start(e), "start");
        net_voice_call(e, 0x1401, 1, 1, 2, 6, true);
        CHECK(poll_at(e, 0) && poll_at(e, 0.4) && r->calls[0].finished, "sent");
        e->clock = r->slots[1].net_until;
        e->log[0] = 0;
        uint8_t b[RDOF_BURST_BYTES];
        if (k == 0) {
            b_header(5, 6, RDOF_DT_VOICE_LC_HEADER, b);
            heard(e, 1, C_HEADER, b);
            CHECK(poll_at(e, 0), "RF voice");
            LOG_IS(e, "st d1:41 d1:41 d1:41 lc"); /* no abort */
        } else {
            b_csbk(RDOF_CSBK_PREAMBLE, 5, 6, true, b);
            heard(e, 1, C_CSBK, b);
            CHECK(poll_at(e, 0), "RF CSBK");
            LOG_IS(e, "st d1:43 d1:42"); /* repeated; the call hang after */
        }
    }
}

/* Calls starting while an earlier one's bursts wait: the seq keeps its end from setting the hang (R3.7). RF at
 * the start of a call ends the hang; fragments heard before it don't make a late entry after it. */
static void ed_seq(void)
{
    ed_t *e = ed_new("mmdvm_hs");
    rdof_repeater_t *r = &e->r;
    CHECK(ed_start(e), "start");
    e->f.held1 = 47;
    uint8_t b[RDOF_BURST_BYTES], lc9[9];
    rdof_group_lc(5, 6, true, 0, lc9);
    b_lc(lc9, RDOF_DT_VOICE_LC_HEADER, b);
    heard(e, 1, C_HEADER, b);
    b_lc(lc9, RDOF_DT_TERMINATOR_WITH_LC, b);
    heard(e, 1, C_TERM, b);
    b_data_header(9, 6, 1, true, b);
    net_send(e, 0x1501, 1, C_DATA_HEADER, b, false, 9, 6);
    b_block(RDOF_DT_RATE_12, b);
    net_send(e, 0x1501, 1, RDOF_SYNC_DATA | RDOF_DT_RATE_12, b, false, 9, 6);
    CHECK(poll_at(e, 0) && r->slots[1].count == 8, "queued: %d", r->slots[1].count);
    e->f.held1 = 0;
    e->log[0] = 0;
    CHECK(poll_at(e, 0.4) && !r->slots[1].has_hang, "no hang");
    LOG_IS(e, "st d1:41 d1:41 d1:41 d1:42 d1:46 tx:start:9>6 sent:46 d1:47 sent:47 tx:end:9>6 lc");
    CHECK(poll_at(e, 0.1), "after");
    LOG_IS(e, ""); /* no terminators */

    e = ed_new("mmdvm_hs");
    r = &e->r;
    CHECK(ed_start(e), "start");
    b_lc(lc9, RDOF_DT_VOICE_LC_HEADER, b);
    heard(e, 1, C_HEADER, b);
    b_lc(lc9, RDOF_DT_TERMINATOR_WITH_LC, b);
    heard(e, 1, C_TERM, b);
    CHECK(poll_at(e, 0) && r->slots[1].has_hang, "hang");
    b_data_header(5, 6, 2, true, b);
    heard(e, 1, C_DATA_HEADER, b);
    CHECK(poll_at(e, 0.1) && !r->slots[1].has_hang, "RF data ends it");

    e = ed_new("mmdvm_hs");
    r = &e->r;
    CHECK(ed_start(e), "start");
    for (int n = 1; n <= 3; n++) {
        uint8_t c = b_voice(n, lc9, CC, b);
        heard(e, 1, c, b);
    }
    b_lc(lc9, RDOF_DT_VOICE_LC_HEADER, b);
    heard(e, 1, C_HEADER, b);
    b_lc(lc9, RDOF_DT_TERMINATOR_WITH_LC, b);
    heard(e, 1, C_TERM, b);
    uint8_t c = b_voice(4, lc9, CC, b);
    heard(e, 1, c, b);
    CHECK(poll_at(e, 0) && !r->slots[1].rf.active && r->slots[1].late_have == 0x10, "only E: %02x",
          r->slots[1].late_have);
}

/* A duplex repeater ignores RF busy NAKs with a stream on; a key with no traffic waits for traffic. */
static void ed_misc(void)
{
    ed_t *e = ed_new("mmdvm_hs");
    rdof_repeater_t *r = &e->r;
    CHECK(ed_start(e), "start");
    net_voice_call(e, 0x1601, 2, 1, 2, 1, false);
    CHECK(poll_at(e, 0) && r->slots[2].net >= 0, "streaming");
    e->log[0] = 0;
    rdof_repeater_nak(r, RDOF_MMDVM_DMR_DATA2, RDOF_NAK_RF_BUSY);
    CHECK(poll_at(e, 0) && r->slots[2].net >= 0, "still");
    LOG_IS(e, "");

    /* started again under an open RF voice call: silence goes out unkeyed until traffic keys it */
    e = ed_new("mmdvm_hs");
    r = &e->r;
    CHECK(ed_start(e), "start");
    uint8_t b[RDOF_BURST_BYTES], lc9[9];
    rdof_group_lc(5, 6, true, 0, lc9);
    b_lc(lc9, RDOF_DT_VOICE_LC_HEADER, b);
    heard(e, 1, C_HEADER, b);
    uint8_t c = b_voice(0, lc9, CC, b);
    heard(e, 1, c, b);
    CHECK(poll_at(e, 0) && poll_at(e, 0.25), "voice");
    CHECK(ed_start(e) && !r->keyed && r->slots[1].rf.active, "started");
    CHECK(poll_at(e, 0.06) && !r->keyed, "not keyed");
    LOG_IS(e, "st d1:02 lc");

    /* a missed status forgotten once one comes */
    e = ed_new("mmdvm_hs");
    r = &e->r;
    CHECK(ed_start(e), "start");
    b_lc(lc9, RDOF_DT_VOICE_LC_HEADER, b);
    heard(e, 1, C_HEADER, b);
    b_lc(lc9, RDOF_DT_TERMINATOR_WITH_LC, b);
    heard(e, 1, C_TERM, b);
    CHECK(poll_at(e, 0), "hang");
    e->f.drop_status = 1;
    CHECK(poll_at(e, 0.1) && r->has_status_missed, "missed");
    CHECK(poll_at(e, 0.1) && !r->has_status_missed, "answered");
    e->f.drop_status = 1;
    CHECK(poll_at(e, 2.5) && r->ready && r->has_status_missed, "missed again, from now");
    /* a fault ends the hang */
    e->fail_cmd = RDOF_MMDVM_DMR_DATA1;
    CHECK(!poll_at(e, 0.1) && !r->slots[1].has_hang, "a fault");
}

/* Modem space after an abort: none, until a status says. */
static void ed_abort_space(void)
{
    ed_t *e = ed_new("odr_sfr");
    rdof_repeater_t *r = &e->r;
    rdof_repeater_set_sfr(r, 1);
    CHECK(ed_start(e), "start");
    net_voice_call(e, 0x1701, 2, 1, 2, 6, false);
    CHECK(poll_at(e, 0) && r->slots[2].net >= 0 && !r->slots[2].count, "sent");
    uint8_t b[RDOF_BURST_BYTES], lc9[9];
    rdof_group_lc(5, 6, true, 0, lc9);
    uint8_t c = b_voice(0, lc9, CC, b);
    heard(e, 1, c, b);
    CHECK(poll_at(e, 0) && r->slots[2].net < 0 && !r->slots[2].space, "taken");
    b_lc(lc9, RDOF_DT_TERMINATOR_WITH_LC, b);
    heard(e, 1, C_TERM, b);
    e->f.held = 47;
    net_voice_call(e, 0x1702, 2, 1, 2, 6, false);
    e->log[0] = 0;
    CHECK(poll_at(e, 0) && r->slots[2].net >= 0 && r->slots[2].count == 9, "queued: %d", r->slots[2].count);
    LOG_IS(e, "st");
}

/* Dual capacity direct mode (repeater.md section 13): two network calls at once, one a slot, each framed with its
 * slot's direct mode sync; RF heard on a slot holds it and is never repeated; no DMR_START, short LC or hang. */
static int ed_count(int sl, uint8_t control)
{
    int k = 0;
    for (int i = 0; i < ed_nsent[sl] && i < 64; i++)
        k += ed_sent[sl][i].control == control;
    return k;
}

static void ed_dcdm(void)
{
    ed_t *e = ed_new("odr_dcdm");
    rdof_repeater_t *r = &e->r;
    e->radio.headers = 4;
    e->radio.terminators = 2;
    rdof_repeater_set_dcdm(r);
    CHECK(r->dcdm && !r->sfr, "set");
    rdof_repeater_set_sfr(r, 1);
    CHECK(r->sfr && !r->dcdm, "sfr clears it");
    rdof_repeater_set_dcdm(r);
    r->receiver.hear = ed_hear, r->receiver.ctx = e;
    ed_cfg_flags = 0;
    CHECK(ed_start(e), "start");
    CHECK((ed_cfg_flags & (RDOF_MMDVM_SIMPLEX | RDOF_MMDVM_DCDM)) == (RDOF_MMDVM_SIMPLEX | RDOF_MMDVM_DCDM) &&
              e->f.simplex && e->f.freq == 462550000 && e->f.tx_freq == 462550000,
          "tuned simplex with the DCDM flag on tx_freq: %02x", ed_cfg_flags);

    /* two network calls at once, one on each slot */
    memset(ed_nsent, 0, sizeof ed_nsent);
    net_voice_call(e, 0xD001, 1, 11, 91, 6, true);
    net_voice_call(e, 0xD002, 2, 12, 92, 6, true);
    CHECK(poll_at(e, 0), "poll");
    CHECK(!strstr(e->log, "key") && !strstr(e->log, "lc") && strstr(e->log, "K1"), "no DMR_START or short LC: %s",
          e->log);
    for (int i = 0; i < 40; i++) {
        fake_drain(&e->f);
        CHECK(poll_at(e, 0.06), "poll %d", i);
    }
    for (int sl = 1; sl <= 2; sl++) {
        const uint64_t v = sl == 1 ? RDOF_TS1_VOICE : RDOF_TS2_VOICE;
        CHECK(ed_count(sl, C_HEADER) == 4 && ed_count(sl, C_TERM) == 2 && ed_count(sl, RDOF_SYNC_AUDIO) == 1 &&
                  ed_nsent[sl] == 4 + 6 + 2,
              "slot %d: %d headers, %d terminators, %d sent", sl, ed_count(sl, C_HEADER), ed_count(sl, C_TERM),
              ed_nsent[sl]);
        for (int i = 0; i < ed_nsent[sl] && i < 64; i++) {
            uint8_t c = ed_sent[sl][i].control;
            if (c & RDOF_SYNC_DATA)
                CHECK(rdof_middle_field(ed_sent[sl][i].burst) == rdof_data_sync(v), "slot %d burst %d: data sync", sl, i);
            else if (c & RDOF_SYNC_AUDIO)
                CHECK(rdof_middle_field(ed_sent[sl][i].burst) == v, "slot %d burst %d: voice sync", sl, i);
            else {
                int cc;
                CHECK(rdof_decode_emb(ed_sent[sl][i].burst, &cc, NULL, NULL, NULL) && cc == CC, "slot %d burst %d: EMB",
                      sl, i);
            }
        }
        CHECK(!r->slots[sl].has_hang && !r->slots[sl].count && r->slots[sl].net < 0, "slot %d: no call hang", sl);
    }
    CHECK(e->f.ncw == 0, "no CW");

    /* RF on slot 2: heard and followed, never repeated; the network keeps slot 1 and is refused slot 2 (R8.2) */
    memset(ed_nsent, 0, sizeof ed_nsent);
    e->log[0] = 0;
    uint8_t b[RDOF_BURST_BYTES], lc9[9];
    rdof_group_lc(21, 93, true, 0, lc9);
    b_lc(lc9, RDOF_DT_VOICE_LC_HEADER, b);
    int heard0 = e->heard;
    heard(e, 2, C_HEADER, b);
    net_voice_call(e, 0xD003, 1, 13, 94, 6, false);
    net_voice_call(e, 0xD004, 2, 14, 95, 6, false);
    CHECK(poll_at(e, 0.06), "poll");
    CHECK(e->heard == heard0 + 1 && r->slots[2].rf.active && r->slots[2].net < 0 && r->slots[1].net >= 0,
          "RF has slot 2, the network slot 1");
    for (int i = 1; i <= 5; i++) {
        uint8_t c = b_voice(i, lc9, CC, b);
        heard(e, 2, c, b);
        fake_drain(&e->f);
        CHECK(poll_at(e, 0.06), "poll");
    }
    CHECK(ed_nsent[2] == 0 && ed_nsent[1] > 0 && !r->slots[2].count, "nothing repeated on slot 2 (%d), slot 1 sent %d",
          ed_nsent[2], ed_nsent[1]);
    /* RF on slot 1 now: the network call there yields (R8.1) */
    e->log[0] = 0;
    rdof_group_lc(22, 96, true, 0, lc9);
    b_lc(lc9, RDOF_DT_VOICE_LC_HEADER, b);
    heard(e, 1, C_HEADER, b);
    CHECK(poll_at(e, 0.06) && r->slots[1].net < 0 && r->slots[1].rf.active && strstr(e->log, "ab1"),
          "RF takes slot 1: %s", e->log);
    b_lc(lc9, RDOF_DT_TERMINATOR_WITH_LC, b);
    heard(e, 1, C_TERM, b);
    rdof_repeater_lost(r, 2);
    CHECK(poll_at(e, 0.06) && !r->slots[1].rf.active && !r->slots[2].rf.active && !r->slots[1].count &&
              !r->slots[2].count,
          "both RF calls over, nothing queued");
    e->log[0] = 0;
    rdof_repeater_stop(r);
    CHECK(!strstr(e->log, "key"), "stop: no DMR_START: %s", e->log);
}

/* A chain (ODR): preamble CSBKs once, then packets back to back (header and blocks each), one terminator. Every
 * header opens a data call of its own (R4.8), every burst repeated, keyed throughout; a block of packet 1 lost,
 * packet 2's header ends it and opens its own. */
static void ed_chain(void)
{
    for (int lose = 0; lose < 2; lose++) {
        ed_t *e = ed_new("mmdvm_hs");
        rdof_repeater_t *r = &e->r;
        CHECK(ed_start(e), "start");
        rdof_rpt_slot_t *s = &r->slots[2];
        uint8_t b[RDOF_BURST_BYTES];
        memset(ed_nsent, 0, sizeof ed_nsent);
        for (int i = 0; i < 2; i++) {
            b_csbk(0x3D, 5, 6, true, b); /* preambles */
            heard(e, 2, C_CSBK, b);
        }
        int sent = 2, starts = 0;
        for (int k = 0; k < 3; k++) {
            b_data_header(5, 6, 2, true, b);
            heard(e, 2, C_DATA_HEADER, b);
            CHECK(poll_at(e, 0.06), "poll");
            starts += s->rf.active && s->rf.kind == RDOF_KIND_DATA;
            sent++;
            for (int i = 0; i < 2; i++) {
                if (lose && k == 0 && i == 1)
                    continue; /* packet 1's last block never heard */
                b_block(RDOF_DT_RATE_12, b);
                heard(e, 2, RDOF_SYNC_DATA | RDOF_DT_RATE_12, b);
                CHECK(poll_at(e, 0.06), "poll");
                sent++;
            }
            CHECK(s->rf.active == (lose && k == 0), "chain %d packet %d: over at its last block", lose, k);
        }
        b_header(5, 6, RDOF_DT_TERMINATOR_WITH_LC, b);
        heard(e, 2, C_TERM, b);
        for (int i = 0; i < 10; i++) {
            fake_drain(&e->f);
            CHECK(poll_at(e, 0.06), "poll");
        }
        CHECK(starts == 3 && ed_nsent[2] == sent && ed_count(2, C_DATA_HEADER) == 3, "chain %d: %d calls, %d of %d sent",
              lose, starts, ed_nsent[2], sent);
        CHECK(!strstr(e->log, "key0") && !strstr(e->log, "K0") && r->keyed, "chain %d: keyed throughout: %s", lose,
              e->log);
    }
}

void test_repeater_edges(void)
{
    ed_chain();
    ed_dcdm();
    ed_duration();
    ed_voice_out();
    ed_timers();
    ed_edges();
    ed_net_until();
    ed_seq();
    ed_misc();
    ed_abort_space();
    ed_no_hooks();
    ed_sfr_more();
    ed_yield_more();
    ed_rf_odd();
    ed_net_odd();
    ed_fill_full();
    ed_hang_net();
    ed_data_end();
    ed_network();
    ed_net_bounds();
    ed_sfr();
    ed_yield();
    ed_transmit();
    ed_fault();
    ed_cwid();
    ed_done_ring();
    ed_basics();
    ed_queues();
    ed_rf_voice();
    ed_late_entry();
    ed_rf_data();
    ed_rf_csbk();
    ed_wakeup();
}

void test_repeater(void)
{
    for (size_t i = 0; i < SC_RPT_N; i++)
        run(&sc_rpt[i]);
    test_repeater_edges();
}

#else

void test_repeater(void) {}

#endif
