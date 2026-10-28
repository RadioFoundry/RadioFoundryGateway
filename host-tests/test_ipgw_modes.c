// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The IP gateway end to end in each way the board transmits (rdof/ipgwtx.h), on fake radios: a roamer's transceiver
 * (an OpenGD77 hotspot and an MMDVM_HS, simplex), a duplex repeater on both slots, an SFR repeater and a DCDM
 * repeater. Each: a radio's packet heard on RF goes up through the tracker to the gateway and out to the internet;
 * the reply comes down on the slot and frequency the radio is on, with the sync that mode puts on air; a repeater
 * repeats the radio's packet as before; what the gateway sent, heard again, isn't relayed up. The SDR's
 * sdr_transceiver (half and full duplex) is in test_ipgw_sdr.c. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "rdof/features.h"

#if RDOF_FEATURE_REPEATER && RDOF_FEATURE_TRACKER && RDOF_FEATURE_DATA_BLOCKS

#include "rdof/ipgw.h"
#include "rdof/ipgwtx.h"
#include "rdof/lastheard.h"
#include "rdof/layer2.h"
#include "rdof/mmdvm.h"
#include "rdof/pdu.h"
#include "rdof/repeater.h"
#include "rdof/tracker.h"
#include "rdof/txvr.h"

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

#define IP(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define RADIO 9994567u
#define RADIO_IP IP(12, 152, 129, 71)
#define GW_ID 9990u
#define PEER_IP IP(8, 8, 8, 8)
#define RX_HZ 467550000u
#define TX_HZ 462550000u
#define CH_A 462562500u
#define CH_B 462587500u
#define MAXW 512

typedef enum { M_TXVR, M_DUPLEX, M_SFR, M_DCDM } mode_t_;

typedef struct {
    mode_t_ mode;
    fake_radio_t f;
    rdof_modem_t m;
    rdof_radio_t radio;
    rdof_txvr_t t;
    rdof_repeater_t r;
    rdof_tracker_t tr;
    rdof_ipgw_t g;
    rdof_ipgw_tx_t x;
    double clock;
    uint32_t sid;
    /* the DMR_DATA writes, by slot, in order */
    struct {
        uint8_t control, burst[RDOF_BURST_BYTES];
    } w[3][MAXW];
    int nw[3];
    int heard; /* the receiver's hear calls */
    /* the internet side */
    int udp, opens;
    uint32_t udp_ip;
    uint16_t udp_port;
    uint8_t udp_data[2048];
    size_t udp_len;
} md_t;

static bool md_write(void *ctx, const uint8_t *d, size_t n)
{
    md_t *e = ctx;
    if (n >= 4 + RDOF_BURST_BYTES && (d[2] == RDOF_MMDVM_DMR_DATA1 || d[2] == RDOF_MMDVM_DMR_DATA2)) {
        int sl = d[2] == RDOF_MMDVM_DMR_DATA1 ? 1 : 2;
        if (e->nw[sl] < MAXW) {
            e->w[sl][e->nw[sl]].control = d[3];
            memcpy(e->w[sl][e->nw[sl]].burst, d + 4, RDOF_BURST_BYTES);
        }
        e->nw[sl]++;
    }
    fake_write(&e->f, d, n);
    uint8_t buf[4096];
    size_t k;
    while ((k = fake_take(&e->f, buf, sizeof buf)) > 0)
        rdof_modem_feed(&e->m, buf, k);
    return true;
}

static bool md_wait(void *ctx, int ms) { return ((md_t *)ctx)->m.reply_ready; }
static double md_clock(void *ctx) { return ((md_t *)ctx)->clock; }
static void md_sleep(void *ctx, int ms) {}

/* the receiver: what the radio hears goes to the tracker, as rx_path.c does */
static void md_hear(void *ctx, int slot, uint8_t control, const uint8_t *burst, double dbm, int cc, uint32_t freq)
{
    md_t *e = ctx;
    e->heard++;
    rdof_burst_t b;
    if (freq)
        e->tr.freq_hz = freq;
    rdof_modem_burst(control, burst, dbm, cc, slot, &b);
    rdof_tracker_feed(&e->tr, &b, e->clock);
}

/* the tracker's sink: data bursts to the gateway, as ipgw_task.c's on_frame does */
static void md_frame(void *ctx, const char *channel, const rdof_frame_t *f)
{
    md_t *e = ctx;
    if (f->dt != RDOF_DT_DATA_HEADER && f->dt != RDOF_DT_RATE_12 && f->dt != RDOF_DT_RATE_34 &&
        f->dt != RDOF_DT_RATE_1)
        return;
    rdof_ipgw_where_t w = {.source = 100, .slot = f->slot, .freq = (uint32_t)e->tr.freq_hz, .cc = -1};
    rdof_ipgw_burst(&e->g, &w, f->dt, f->burst, e->clock);
}

/* the tracker's calls: listen-before-talk's last call heard, as rx_path.c's on_call */
static void md_call(void *ctx, const char *channel, const char *action, uint32_t src, uint32_t dst)
{
    md_t *e = ctx;
    if (e->mode == M_TXVR)
        rdof_txvr_heard_call(&e->t, action, src, dst);
}

static bool g_open(void *ctx, int flow)
{
    ((md_t *)ctx)->opens++;
    return true;
}
static void g_close(void *ctx, int flow) {}
static bool g_send(void *ctx, int flow, uint32_t ip, uint16_t port, const uint8_t *data, size_t len, int ttl)
{
    md_t *e = ctx;
    e->udp++;
    e->udp_ip = ip;
    e->udp_port = port;
    e->udp_len = len < sizeof e->udp_data ? len : sizeof e->udp_data;
    memcpy(e->udp_data, data, e->udp_len);
    return true;
}

/* send_dmr, as ipgw_task.c's */
static bool g_dmr(void *ctx, const rdof_ipgw_dmr_t *p)
{
    md_t *e = ctx;
    uint8_t sid[4];
    e->sid++;
    sid[0] = 0xA5, sid[1] = (uint8_t)(e->sid >> 16), sid[2] = (uint8_t)(e->sid >> 8), sid[3] = (uint8_t)e->sid;
    if (e->mode == M_TXVR)
        return rdof_ipgw_tx_txvr(&e->x, &e->t, p, sid);
    return rdof_ipgw_tx_repeater(&e->x, &e->r, p, sid);
}

static void md_txvr_rx(void *ctx, int slot, uint8_t control, const uint8_t *burst, int rssi)
{
    rdof_txvr_heard(&((md_t *)ctx)->t, slot, control, burst, rssi);
}
static void md_rpt_rx(void *ctx, int slot, uint8_t control, const uint8_t *burst, int rssi)
{
    rdof_repeater_heard(&((md_t *)ctx)->r, slot, control, burst, rssi);
}
static void md_rpt_lost(void *ctx, int slot) { rdof_repeater_lost(&((md_t *)ctx)->r, slot); }
static void md_rpt_nak(void *ctx, uint8_t cmd, int err) { rdof_repeater_nak(&((md_t *)ctx)->r, cmd, err); }

static md_t *md_new(mode_t_ mode, const char *kind)
{
    static md_t e;
    memset(&e, 0, sizeof e);
    e.mode = mode;
    e.clock = 1000.0;
    fake_init(&e.f, kind);
    rdof_modem_port_t port = {md_write, md_wait, NULL, md_clock, md_sleep, &e};
    rdof_modem_init(&e.m, &port);
    rdof_modem_start(&e.m, 1);
    e.m.cb_ctx = &e;
    rdof_radio_init(&e.radio, &e.m, kind_of(kind));
    rdof_tracker_init(&e.tr, "radio", 0.0, 0.0);
    rdof_sink_t s = {.on_frame = md_frame, .on_call = md_call, .ctx = &e};
    rdof_tracker_add_sink(&e.tr, s);
    rdof_ipgw_config_t c;
    const char *json = "{\"ip_gateway\": {\"id\": 9990, \"nat\": true, \"up_pps\": 0, \"down_pps\": 0}}";
    char err[160];
    CHECK(rdof_ipgw_settings_parse(json, strlen(json), &c, err, sizeof err), "settings: %s", err);
    rdof_ipgw_init(&e.g, &c);
    e.g.open = g_open;
    e.g.close = g_close;
    e.g.send_udp = g_send;
    e.g.send_dmr = g_dmr;
    e.g.ctx = &e;
    if (mode == M_TXVR) {
        e.m.on_rx = md_txvr_rx;
        rdof_txvr_t *t = &e.t;
        rdof_txvr_init(t, &e.radio, 9990601);
        t->channels[0] = (rdof_channel_t){CH_A, 1, 255};
        t->channels[1] = (rdof_channel_t){CH_B, 1, 255};
        t->nchannels = 2;
        t->has_idle = true;
        t->idle = (rdof_channel_t){CH_B, 1, 255};
        t->clock = md_clock;
        t->ctx = &e;
        t->receiver.hear = md_hear;
        t->receiver.ctx = &e;
        e.g.sync48 = rdof_data_sync(RDOF_MS_VOICE); /* ipgw_task.c's tx_sync */
        CHECK(rdof_txvr_start(t), "txvr start: %s", e.m.error);
        rdof_txvr_poll(t);
    } else {
        e.m.on_rx = md_rpt_rx;
        e.m.on_lost = md_rpt_lost;
        e.m.on_nak = md_rpt_nak;
        e.radio.fixed_hz = TX_HZ;
        rdof_repeater_t *r = &e.r;
        rdof_repeater_init(r, &e.radio, 9990601, mode == M_DCDM ? TX_HZ : RX_HZ, TX_HZ, 1);
        if (mode == M_SFR)
            rdof_repeater_set_sfr(r, 1);
        if (mode == M_DCDM)
            rdof_repeater_set_dcdm(r);
        r->clock = md_clock;
        r->ctx = &e;
        r->receiver.hear = md_hear;
        r->receiver.ctx = &e;
        e.g.sync48 = rdof_data_sync(RDOF_BS_VOICE);
        CHECK(rdof_repeater_start(r), "repeater start: %s", e.m.error);
    }
    memset(e.nw, 0, sizeof e.nw);
    return &e;
}

/* One poll 60 ms on: the radio sends what it holds, the gateway's ticks and its packet's rest go in. */
static void md_step(md_t *e)
{
    e->clock += 0.06;
    fake_drain(&e->f);
    if (e->mode == M_TXVR) {
        rdof_txvr_poll(&e->t);
    } else {
        rdof_ipgw_tx_more(&e->x, &e->r);
        rdof_repeater_poll(&e->r);
    }
    rdof_tracker_tick(&e->tr, e->clock);
    rdof_ipgw_tick(&e->g, e->clock);
}

static void md_run(md_t *e, double s)
{
    for (double t = 0; t < s; t += 0.06)
        md_step(e);
}

/* The radio's packet, framed as it puts it on air (sync48), heard on slot: through the modem's callback. */
static void md_radio_sends(md_t *e, int slot, uint64_t sync48, const char *payload, rdof_ipgw_dmr_t *p)
{
    uint8_t d[2048];
    size_t n = rdof_ipgw_udp_datagram(RADIO_IP, 5000, PEER_IP, 53, 7, (const uint8_t *)payload, strlen(payload), d,
                                      sizeof d);
    CHECK(rdof_ipgw_build_dmr(RADIO, GW_ID, RDOF_SAP_IP, d, n, 1, sync48, p), "build");
    for (int i = 0; i < p->nbursts; i++) {
        fake_hear(&e->f, p->control[i], p->bursts[i], -60, slot); /* up from the radio, as DMR_DATA */
        uint8_t buf[4096];
        size_t k;
        while ((k = fake_take(&e->f, buf, sizeof buf)) > 0)
            rdof_modem_feed(&e->m, buf, k);
        md_step(e);
    }
}

/* The packet in the writes on slot from index from: its header's ids and the datagram; returns its length, -1 if
 * none checks. *sync gets the header's sync; *last the index past the packet. */
static int md_packet(const md_t *e, int slot, int from, rdof_data_header_t *h, uint8_t *out, uint64_t *sync,
                     int *last)
{
    int n = e->nw[slot] < MAXW ? e->nw[slot] : MAXW;
    for (int i = from; i < n; i++) {
        if (e->w[slot][i].control != (RDOF_SYNC_DATA | RDOF_DT_DATA_HEADER))
            continue;
        rdof_decode_data_header(e->w[slot][i].burst, h);
        if (!h->crc_ok || h->dpf != RDOF_DPF_UNCONFIRMED)
            continue;
        *sync = rdof_middle_field(e->w[slot][i].burst);
        int len = 0, k = i + 1, got = 0;
        for (; k < n && got < h->blocks; k++) {
            if (e->w[slot][k].control != (RDOF_SYNC_DATA | RDOF_DT_RATE_12))
                continue;
            if (rdof_middle_field(e->w[slot][k].burst) != *sync)
                return -1; /* every block with the header's sync */
            rdof_data_block_t b;
            if (!rdof_decode_data_block(e->w[slot][k].burst, RDOF_DT_RATE_12, &b))
                return -1;
            memcpy(out + len, b.payload, 12);
            len += 12;
            got++;
        }
        if (got < h->blocks || !rdof_check_crc32(out, (size_t)len))
            return -1;
        *last = k;
        return len - 4 - h->pad_octets;
    }
    return -1;
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* What went down on slot heard again (another receiver on the outbound, or an SFR's own repeat): never up again. */
static void md_hear_own(md_t *e, int slot, int from)
{
    int up = e->udp;
    uint32_t dup = e->g.n.rx_dup;
    int n = e->nw[slot] < MAXW ? e->nw[slot] : MAXW;
    for (int i = from; i < n; i++) {
        int dt = e->w[slot][i].control & 0x0F;
        rdof_ipgw_where_t w = {.source = 7, .slot = slot, .freq = TX_HZ, .cc = -1};
        if (e->w[slot][i].control & RDOF_SYNC_DATA)
            rdof_ipgw_burst(&e->g, &w, dt, e->w[slot][i].burst, e->clock);
    }
    CHECK(e->udp == up && e->g.n.rx_dup == dup + 1, "%d: our own packet heard again: %d up, dup %u", e->mode,
          e->udp - up, e->g.n.rx_dup - dup);
}

/* Uplink and downlink in one mode: the radio on slot (sending with up_sync), the reply wanted on out_slot with
 * down_sync; repeated: how many of the radio's bursts the repeater sends on out_slot. */
static void md_round_trip(md_t *e, const char *name, int slot, uint64_t up_sync, int out_slot, uint64_t down_sync,
                          int repeated)
{
    static rdof_ipgw_dmr_t p;
    int before = e->nw[out_slot];
    md_radio_sends(e, slot, up_sync, "query", &p);
    md_run(e, 0.5);
    CHECK(e->udp == 1 && e->udp_ip == PEER_IP && e->udp_port == 53 && e->udp_len == 5 &&
              !memcmp(e->udp_data, "query", 5),
          "%s: up to the internet: %d", name, e->udp);
    CHECK(e->g.n.up_packets == 1 && e->heard >= p.nbursts, "%s: heard %d of %d", name, e->heard, p.nbursts);
    int rep = 0;
    for (int i = before; i < e->nw[out_slot] && i < MAXW; i++)
        for (int k = 0; k < p.nbursts; k++)
            if (e->w[out_slot][i].control == p.control[k] && (e->w[out_slot][i].control & RDOF_SYNC_DATA)) {
                uint8_t pa[12], pb[12];
                rdof_bptc196_decode(e->w[out_slot][i].burst, pa);
                rdof_bptc196_decode(p.bursts[k], pb);
                if (!memcmp(pa, pb, 10)) {
                    rep++;
                    break;
                }
            }
    if (repeated < 0)
        repeated = p.nbursts;
    CHECK(rep == repeated, "%s: %d of the radio's bursts repeated, want %d", name, rep, repeated);

    /* the reply */
    int heard = e->heard, from = e->nw[out_slot], other = e->nw[3 - out_slot];
    rdof_ipgw_from_net(&e->g, 0, PEER_IP, 53, (const uint8_t *)"answer", 6, e->clock);
    CHECK(e->g.n.no_tx == 0, "%s: handed to the transmitter", name);
    md_run(e, 3.0);
    rdof_data_header_t h;
    uint8_t d[2048];
    uint64_t sync = 0;
    int last = 0;
    int len = md_packet(e, out_slot, from, &h, d, &sync, &last);
    CHECK(len == 34 && h.dst_id == RADIO && h.src_id == GW_ID && rd32(d + 12) == PEER_IP && rd32(d + 16) == RADIO_IP &&
              !memcmp(d + 28, "answer", 6),
          "%s: the reply on slot %d: len %d, dst %lu", name, out_slot, len, (unsigned long)h.dst_id);
    CHECK(sync == down_sync, "%s: sync %012llx, want %012llx", name, (unsigned long long)sync,
          (unsigned long long)down_sync);
    CHECK(e->nw[3 - out_slot] == other || e->mode == M_DUPLEX, "%s: nothing on the other slot", name);
    CHECK(e->heard == heard, "%s: the radio's own transmission not heard (%d)", name, e->heard - heard);
    CHECK(e->g.n.down_packets == 1 && e->udp == 1, "%s: one down, nothing up again", name);
    md_hear_own(e, out_slot, from);
}

/* The reply at once, as an ACK goes: it still gets on air (on a busy slot it waits). */
static void md_quick(md_t *e, const char *name, int slot, uint64_t up_sync, int out_slot)
{
    static rdof_ipgw_dmr_t p;
    md_radio_sends(e, slot, up_sync, "again", &p);
    int from = e->nw[out_slot];
    rdof_ipgw_from_net(&e->g, 0, PEER_IP, 53, (const uint8_t *)"quick", 5, e->clock);
    md_run(e, 3.0);
    rdof_data_header_t h;
    uint8_t d[256];
    uint64_t sync;
    int last;
    int len = md_packet(e, out_slot, from, &h, d, &sync, &last);
    CHECK(len == 33 && !memcmp(d + 28, "quick", 5) && e->x.gave_up == 0 && e->g.n.no_tx == 0,
          "%s: the quick reply on slot %d: %d", name, out_slot, len);
}

static void t_txvr(const char *kind)
{
    md_t *e = md_new(M_TXVR, kind);
    /* the radio heard on the idle channel, B; slot 2 */
    md_round_trip(e, kind, 2, rdof_data_sync(RDOF_MS_VOICE), 2, rdof_data_sync(RDOF_MS_VOICE), 0);
    CHECK(e->f.freq == CH_B && e->g.n.no_tx == 0, "%s: tuned %lu", kind, (unsigned long)e->f.freq);
    md_quick(e, kind, 2, rdof_data_sync(RDOF_MS_VOICE), 2);
    CHECK(e->nw[1] == 0, "%s: nothing on slot 1", kind);
}

/* the frequency it transmits on: where the radio was heard */
static void t_txvr_channel(void)
{
    md_t *e = md_new(M_TXVR, "mmdvm_hs");
    static rdof_ipgw_dmr_t p;
    uint8_t d[64];
    size_t n = rdof_ipgw_udp_datagram(PEER_IP, 53, RADIO_IP, 5000, 1, (const uint8_t *)"x", 1, d, sizeof d);
    CHECK(rdof_ipgw_build_dmr(GW_ID, RADIO, RDOF_SAP_IP, d, n, 1, rdof_data_sync(RDOF_MS_VOICE), &p), "build");
    p.freq = CH_A + 60; /* within 100 Hz */
    uint8_t sid[4] = {1, 2, 3, 4};
    CHECK(rdof_ipgw_tx_txvr(&e->x, &e->t, &p, sid) && e->t.call.tune.freq == CH_A && e->t.call.tune.cc == 1,
          "on channel A: %lu", (unsigned long)e->t.call.tune.freq);
    sid[3] = 5;
    CHECK(!rdof_ipgw_tx_txvr(&e->x, &e->t, &p, sid), "a call already: refused");
    md_run(e, 3.0);
    int on_a = e->nw[2];
    CHECK(on_a == p.nbursts && e->f.freq != 0, "sent %d of %d", on_a, p.nbursts);
    p.freq = 470000000; /* not a channel of ours: where it is now */
    sid[3] = 6;
    uint32_t fn;
    int cn;
    rdof_txvr_channel_now(&e->t, &fn, &cn);
    bool ok = rdof_ipgw_tx_txvr(&e->x, &e->t, &p, sid);
    CHECK(ok == (fn == CH_A || fn == CH_B) && (!ok || e->t.call.tune.freq == fn), "where it is: %lu",
          (unsigned long)fn);
    md_run(e, 3.0);
    e->t.has_tuned = false;
    p.freq = 0;
    sid[3] = 7;
    CHECK(!rdof_ipgw_tx_txvr(&e->x, &e->t, &p, sid), "nowhere to send it");
}

static void t_duplex(void)
{
    static const char *const kinds[] = {"mmdvm", "mmdvm_hs"}; /* a fixed frequency board, a duplex hat */
    for (int k = 0; k < 2; k++)
        for (int slot = 1; slot <= 2; slot++) {
            md_t *e = md_new(M_DUPLEX, kinds[k]);
            char name[40];
            snprintf(name, sizeof name, "duplex %s slot %d", kinds[k], slot);
            /* the radio's packet repeated whole on its slot, the reply after it */
            md_round_trip(e, name, slot, rdof_data_sync(RDOF_MS_VOICE), slot, rdof_data_sync(RDOF_BS_VOICE), -1);
            md_quick(e, name, slot, rdof_data_sync(RDOF_MS_VOICE), slot);
            CHECK(!e->f.has_freq || (e->f.freq == RX_HZ && e->f.tx_freq == TX_HZ), "%s: on the pair: %lu %lu", name,
                  (unsigned long)e->f.freq, (unsigned long)e->f.tx_freq);
        }
}

static void t_sfr(void)
{
    md_t *e = md_new(M_SFR, "odr_sfr");
    /* the inbound slot 1; the radio's own firmware repeats it, the host doesn't; the reply on the outbound, 2 */
    md_round_trip(e, "sfr", 1, rdof_data_sync(RDOF_MS_VOICE), 2, rdof_data_sync(RDOF_BS_VOICE), 0);
    /* a reply at once, as an ACK is: the repeater would refuse it while the inbound is busy; it waits */
    static rdof_ipgw_dmr_t p;
    md_radio_sends(e, 1, rdof_data_sync(RDOF_MS_VOICE), "again", &p);
    int from = e->nw[2];
    rdof_ipgw_from_net(&e->g, 0, PEER_IP, 53, (const uint8_t *)"quick", 5, e->clock);
    CHECK(e->x.busy && e->x.next == 0, "sfr: held for the inbound's quiet");
    md_run(e, 3.0);
    rdof_data_header_t h;
    uint8_t d[256];
    uint64_t sync;
    int last;
    CHECK(md_packet(e, 2, from, &h, d, &sync, &last) == 33 && !memcmp(d + 28, "quick", 5) && e->x.gave_up == 0,
          "sfr: the quick reply on the outbound");
}

static void t_dcdm(void)
{
    for (int slot = 1; slot <= 2; slot++) {
        md_t *e = md_new(M_DCDM, "odr_dcdm");
        char name[32];
        snprintf(name, sizeof name, "dcdm TS%d", slot);
        const uint64_t v = slot == 1 ? RDOF_TS1_VOICE : RDOF_TS2_VOICE;
        md_round_trip(e, name, slot, rdof_data_sync(v), slot, rdof_data_sync(v), 0);
        md_quick(e, name, slot, rdof_data_sync(v), slot);
        CHECK(e->f.freq == TX_HZ && e->f.tx_freq == TX_HZ, "%s: one frequency", name);
    }
}

/* A reply bigger than the repeater's input queue (64): it goes in as the repeater takes it. */
static void t_big(void)
{
    md_t *e = md_new(M_DUPLEX, "mmdvm");
    static rdof_ipgw_dmr_t p;
    md_radio_sends(e, 1, rdof_data_sync(RDOF_MS_VOICE), "q", &p);
    md_run(e, 0.5);
    static uint8_t big[1400];
    for (size_t i = 0; i < sizeof big; i++)
        big[i] = (uint8_t)(i * 13);
    int from = e->nw[1];
    rdof_ipgw_from_net(&e->g, 0, PEER_IP, 53, big, 1200, e->clock);
    CHECK(e->x.busy && e->x.next == RDOF_RPT_IN, "64 in at once, the rest waiting: %d", e->x.next);
    rdof_ipgw_from_net(&e->g, 0, PEER_IP, 53, (const uint8_t *)"next", 4, e->clock);
    CHECK(e->g.n.no_tx == 1, "another refused meanwhile");
    md_run(e, 12.0);
    rdof_data_header_t h;
    static uint8_t d[2048];
    uint64_t sync;
    int last;
    int len = md_packet(e, 1, from, &h, d, &sync, &last);
    CHECK(!e->x.busy && len == 1228 && h.blocks > RDOF_RPT_IN && !memcmp(d + 28, big, 1200),
          "the big one whole: %d, %d blocks", len, h.blocks);
}

/* An ACK for a confirmed packet goes down at once: on a duplex slot it waits for nothing; on SFR for the quiet. */
static void t_slot_busy(void)
{
    md_t *e = md_new(M_DUPLEX, "mmdvm");
    /* RF on slot 1 (a voice call): the reply waits for it to end */
    uint8_t b[RDOF_BURST_BYTES], lc9[9];
    rdof_group_lc(9990055, 66, true, 0, lc9);
    rdof_lc_burst(lc9, RDOF_DT_VOICE_LC_HEADER, 1, rdof_data_sync(RDOF_MS_VOICE), b);
    e->m.on_rx(e, 1, RDOF_SYNC_DATA | RDOF_DT_VOICE_LC_HEADER, b, 0);
    md_step(e);
    CHECK(e->r.slots[1].rf.active, "RF on slot 1");
    static rdof_ipgw_dmr_t p;
    uint8_t d[64];
    size_t n = rdof_ipgw_udp_datagram(PEER_IP, 53, RADIO_IP, 5000, 1, (const uint8_t *)"x", 1, d, sizeof d);
    rdof_ipgw_build_dmr(GW_ID, RADIO, RDOF_SAP_IP, d, n, 1, rdof_data_sync(RDOF_BS_VOICE), &p);
    p.slot = 1;
    uint8_t sid[4] = {9, 9, 9, 1};
    CHECK(rdof_ipgw_tx_repeater(&e->x, &e->r, &p, sid) && e->x.busy && e->x.next == 0, "held while RF has the slot");
    md_run(e, 0.3);
    CHECK(e->x.busy && e->x.next == 0, "still held");
    rdof_lc_burst(lc9, RDOF_DT_TERMINATOR_WITH_LC, 1, rdof_data_sync(RDOF_MS_VOICE), b);
    e->m.on_rx(e, 1, RDOF_SYNC_DATA | RDOF_DT_TERMINATOR_WITH_LC, b, 0);
    int from = e->nw[1];
    md_run(e, 4.0);
    rdof_data_header_t h;
    uint8_t out[256];
    uint64_t sync;
    int last;
    CHECK(!e->x.busy && md_packet(e, 1, from, &h, out, &sync, &last) == (int)n, "sent after the RF call");
    /* busy past RDOF_IPGW_TX_WAIT_S: given up */
    e->m.on_rx(e, 1, RDOF_SYNC_DATA | RDOF_DT_VOICE_LC_HEADER, b, 0);
    rdof_lc_burst(lc9, RDOF_DT_VOICE_LC_HEADER, 1, rdof_data_sync(RDOF_MS_VOICE), b);
    e->m.on_rx(e, 1, RDOF_SYNC_DATA | RDOF_DT_VOICE_LC_HEADER, b, 0);
    md_step(e);
    sid[3] = 2;
    CHECK(rdof_ipgw_tx_repeater(&e->x, &e->r, &p, sid), "taken");
    for (int i = 0; i < 40; i++) { /* the radio keeps talking */
        uint8_t v[RDOF_BURST_BYTES];
        uint8_t c = rdof_silence_burst(i % 6, 1, false, NULL, v);
        e->m.on_rx(e, 1, c, v, 0);
        md_step(e);
    }
    CHECK(!e->x.busy && e->x.gave_up == 1, "given up: %u", e->x.gave_up);
    /* a slot that's off */
    e->r.enabled[2] = false;
    p.slot = 2;
    sid[3] = 3;
    CHECK(!rdof_ipgw_tx_repeater(&e->x, &e->r, &p, sid), "slot 2 off");
}

/* A chain (as RadioFoundry radios send unconfirmed data): preamble CSBKs once, then packets to the same destination
 * back to back, one terminator. The tracker reports each packet (start, end), last heard gets an entry each, the
 * gateway sends each up; a block of packet 1 lost: packet 2's header ends it (lost, incomplete) and the rest go up.
 * Dedup: the hash covers the whole datagram, its IPv4 identification too, so an app sending the same payload twice
 * gets both through, while the same packet heard again (a retransmission, a second receiver) is a duplicate. */
typedef struct {
    rdof_tracker_t tr;
    rdof_ipgw_t g;
    rdof_lh_t lh;
    rdof_lh_entry_t ring[16];
    int starts, ends, losts, udp;
    double t;
} ch_t;

static void ch_json(void *ctx, const char *channel, const char *json)
{
    ch_t *c = ctx;
    c->starts += strstr(json, "\"action\":\"start\"") != NULL;
    c->ends += strstr(json, "\"action\":\"end\"") != NULL;
    c->losts += strstr(json, "\"action\":\"lost\"") != NULL;
    rdof_lh_tracker_json(&c->lh, "sdr", "box", channel, json, c->t, 0);
}

static void ch_frame(void *ctx, const char *channel, const rdof_frame_t *f)
{
    ch_t *c = ctx;
    rdof_ipgw_where_t w = {.source = 0, .slot = f->slot, .freq = CH_A, .cc = 1};
    rdof_ipgw_burst(&c->g, &w, f->dt, f->burst, c->t);
}

/* open takes a ch_t here: g_open's is an md_t */
static bool ch_open(void *ctx, int flow) { return true; }

static bool ch_send(void *ctx, int flow, uint32_t ip, uint16_t port, const uint8_t *data, size_t len, int ttl)
{
    ((ch_t *)ctx)->udp++;
    return true;
}

static void ch_burst(ch_t *c, uint8_t control, const uint8_t *burst)
{
    rdof_burst_t b;
    rdof_modem_burst(control, burst, -70, 1, 1, &b);
    rdof_tracker_feed(&c->tr, &b, c->t);
    c->t += 0.06;
}

/* t_chain's own: g_open counts opens in an md_t, and its ctx is a ch_t */
static bool ch_open(void *ctx, int flow) { return true; }

static void t_chain(void)
{
    for (int lose = 0; lose < 2; lose++) {
        static ch_t c;
        memset(&c, 0, sizeof c);
        c.t = 100.0;
        rdof_tracker_init(&c.tr, "A", 0.0, CH_A);
        rdof_sink_t s = {.on_json = ch_json, .on_frame = ch_frame, .ctx = &c};
        rdof_tracker_add_sink(&c.tr, s);
        rdof_lh_init(&c.lh, c.ring, 16);
        rdof_ipgw_config_t cfg;
        const char *json = "{\"ip_gateway\": {\"nat\": true, \"up_pps\": 0}}";
        char err[160];
        CHECK(rdof_ipgw_settings_parse(json, strlen(json), &cfg, err, sizeof err), "settings: %s", err);
        rdof_ipgw_init(&c.g, &cfg);
        c.g.open = ch_open;
        c.g.send_udp = ch_send;
        c.g.ctx = &c;
        uint8_t pre[2][RDOF_BURST_BYTES];
        rdof_csbk_preambles(RADIO, GW_ID, false, 2, 1, pre);
        for (int i = 0; i < 2; i++)
            ch_burst(&c, RDOF_SYNC_DATA | RDOF_DT_CSBK, pre[i]);
        /* three packets: "same" twice (an app repeating itself: IP ids 2 and 3), then "other" */
        static const char *const pl[3] = {"same payload", "same payload", "other"};
        for (int k = 0; k < 3; k++) {
            static rdof_ipgw_dmr_t p;
            uint8_t d[128];
            size_t n = rdof_ipgw_udp_datagram(RADIO_IP, 5000, PEER_IP, 53, (uint16_t)(2 + k), (const uint8_t *)pl[k],
                                              strlen(pl[k]), d, sizeof d);
            rdof_ipgw_build_dmr(RADIO, GW_ID, RDOF_SAP_IP, d, n, 1, rdof_data_sync(RDOF_MS_VOICE), &p);
            for (int i = 0; i < p.nbursts; i++)
                if (!(lose && k == 0 && i == p.nbursts - 1))
                    ch_burst(&c, p.control[i], p.bursts[i]);
        }
        uint8_t lc9[9], term[RDOF_BURST_BYTES];
        rdof_group_lc(RADIO, GW_ID, false, 0, lc9);
        rdof_lc_burst(lc9, RDOF_DT_TERMINATOR_WITH_LC, 1, rdof_data_sync(RDOF_MS_VOICE), term);
        ch_burst(&c, RDOF_SYNC_DATA | RDOF_DT_TERMINATOR_WITH_LC, term);
        for (int i = 0; i < 40; i++) {
            c.t += 0.06;
            rdof_tracker_tick(&c.tr, c.t);
            rdof_ipgw_tick(&c.g, c.t);
        }
        int data = 0, ended = 0, lost = 0;
        for (int i = 0; i < c.lh.n; i++) {
            const rdof_lh_entry_t *e = &c.ring[i];
            if (e->kind != RDOF_LH_DATA)
                continue;
            data++;
            ended += !strcmp(e->end, "end");
            lost += !strcmp(e->end, "lost");
        }
        CHECK(c.starts == 3 && c.ends == 3 - lose && c.losts == lose, "chain %d: tracker starts %d ends %d lost %d", lose,
              c.starts, c.ends, c.losts);
        CHECK(data == 3 && ended == 3 - lose && lost == lose, "chain %d: last heard %d data, %d ended, %d lost", lose,
              data, ended, lost);
        CHECK(c.udp == 3 - lose && c.g.n.up_packets == (uint32_t)(3 - lose) && c.g.n.rx_incomplete == (uint32_t)lose &&
                  c.g.n.rx_dup == 0,
              "chain %d: %d up, incomplete %u, dup %u", lose, c.udp, c.g.n.rx_incomplete, c.g.n.rx_dup);
        /* the chain's last packet heard again (another receiver, a retransmission): a duplicate */
        static rdof_ipgw_dmr_t p;
        uint8_t d[128];
        size_t n = rdof_ipgw_udp_datagram(RADIO_IP, 5000, PEER_IP, 53, 4, (const uint8_t *)"other", 5, d, sizeof d);
        rdof_ipgw_build_dmr(RADIO, GW_ID, RDOF_SAP_IP, d, n, 1, rdof_data_sync(RDOF_MS_VOICE), &p);
        for (int i = 0; i < p.nbursts; i++)
            ch_burst(&c, p.control[i], p.bursts[i]);
        CHECK(c.udp == 3 - lose && c.g.n.rx_dup == 1, "chain %d: heard again: dup %u", lose, c.g.n.rx_dup);
    }
}

void test_ipgw_modes(void)
{
    t_chain();
    t_txvr("opengd77");
    t_txvr("mmdvm_hs");
    t_txvr_channel();
    t_duplex();
    t_sfr();
    t_dcdm();
    t_big();
    t_slot_busy();
}

#else

void test_ipgw_modes(void) {}

#endif
