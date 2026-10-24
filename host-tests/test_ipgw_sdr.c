// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The IP gateway on an sdr_transceiver, end to end over the air: a radio (a modulator) sends a packet; the SDR's
 * receiver hears it and passes it both to its own channel's sink and, through the SDR modem, to the transceiver
 * (half duplex: a roamer) or the repeater (full duplex), whose receiver passes it on too, as on the board; the
 * gateway sends it to the internet once. The reply goes down through the SDR modem's modulator into the receiver
 * again: on the radio's frequency and slot, with the sync the mode puts on air; heard back, it isn't relayed up.
 * The repeater repeats the radio's packet on its outbound as before. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/features.h"
#include "rdof/sdr/features.h"

static int fails, checks;

#define CHECK(cond, ...)                                                                                           \
    do {                                                                                                           \
        checks++;                                                                                                  \
        if (!(cond)) {                                                                                             \
            fails++;                                                                                               \
            printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                                 \
            printf(__VA_ARGS__);                                                                                   \
            printf("\n");                                                                                          \
        }                                                                                                          \
    } while (0)

#if RDOF_FEATURE_REPEATER && RDOF_FEATURE_SDR_RECEIVER && RDOF_FEATURE_TRACKER && RDOF_FEATURE_DATA_BLOCKS

#include "rdof/ipgw.h"
#include "rdof/ipgwtx.h"
#include "rdof/layer2.h"
#include "rdof/modem.h"
#include "rdof/pdu.h"
#include "rdof/repeater.h"
#include "rdof/sdr/mod.h"
#include "rdof/sdr/receiver.h"
#include "rdof/sdr/sdrmodem.h"
#include "rdof/tracker.h"
#include "rdof/txvr.h"

#define FS 288000
#define CENTER 461000000.0
#define RX_CH 461037500u
#define TX_CH 461087500u
#define CHUNK 4608
#define CC 1
#define IP(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define RADIO 9994567u
#define RADIO_IP IP(12, 152, 129, 71)
#define GW_ID 9990u
#define PEER_IP IP(8, 8, 8, 8)
#define MAXAIR 256

typedef struct sx sx_t;

typedef struct {
    sx_t *e;
    int c;
} chan_ctx_t;

typedef struct {
    uint8_t burst[RDOF_BURST_BYTES];
    int dt, slot;
    rdof_family_t family;
} air_t;

struct sx {
    bool full;
    rdof_sdrmodem_t m;
    rdof_modem_t client;
    rdof_radio_t radio;
    rdof_txvr_t t;
    rdof_repeater_t r;
    rdof_tracker_t tr; /* the roamer's or repeater's own (rx_path.c) */
    rdof_sdr_rx_t rx;
    rdof_ipgw_t g;
    rdof_ipgw_tx_t x;
    chan_ctx_t cc[2];
    double t_now;
    uint32_t sid;
    air_t air[2][MAXAIR]; /* what the receiver heard, by channel */
    int nair[2];
    int udp;
    uint32_t udp_ip;
    uint8_t udp_data[256];
    size_t udp_len;
};

/* the SDR modem's io and the client's port, synchronous */
static void io_reply(void *ctx, const uint8_t *f, size_t n) { rdof_modem_feed(&((sx_t *)ctx)->client, f, n); }
static int io_channel(void *ctx, uint32_t hz) { return hz == RX_CH ? 0 : -1; }
static bool io_tune(void *ctx, uint32_t hz, double *off)
{
    *off = (double)hz - CENTER;
    return fabs(*off) < FS * 0.4;
}
static bool p_write(void *ctx, const uint8_t *d, size_t n)
{
    rdof_sdrmodem_write(&((sx_t *)ctx)->m, d, n);
    return true;
}
static bool p_wait(void *ctx, int ms) { return ((sx_t *)ctx)->client.reply_ready; }
static double p_clock(void *ctx) { return ((sx_t *)ctx)->t_now; }
static void p_sleep(void *ctx, int ms) { ((sx_t *)ctx)->t_now += ms / 1e3; }

static void on_rx(void *ctx, int slot, uint8_t control, const uint8_t *burst, int rssi)
{
    sx_t *e = ctx;
    if (e->full)
        rdof_repeater_heard(&e->r, slot, control, burst, rssi);
    else
        rdof_txvr_heard(&e->t, slot, control, burst, rssi);
}
static void on_lost(void *ctx, int slot)
{
    sx_t *e = ctx;
    if (e->full)
        rdof_repeater_lost(&e->r, slot);
}
static void on_nak(void *ctx, uint8_t cmd, int err)
{
    sx_t *e = ctx;
    if (e->full)
        rdof_repeater_nak(&e->r, cmd, err);
}

/* the roamer's / repeater's receiver: its tracker, then the gateway (source 100) */
static void hear(void *ctx, int slot, uint8_t control, const uint8_t *burst, double dbm, int cc, uint32_t freq)
{
    sx_t *e = ctx;
    rdof_burst_t b;
    if (freq)
        e->tr.freq_hz = freq;
    rdof_modem_burst(control, burst, dbm, cc, slot, &b);
    rdof_tracker_feed(&e->tr, &b, e->t_now);
}

/* the tracker's calls to the transceiver: listen-before-talk's last call heard (rx_path.c's on_call) */
static void radio_call(void *ctx, const char *channel, const char *action, uint32_t src, uint32_t dst)
{
    sx_t *e = ctx;
    if (!e->full)
        rdof_txvr_heard_call(&e->t, action, src, dst);
}

static bool data_dt(int dt)
{
    return dt == RDOF_DT_DATA_HEADER || dt == RDOF_DT_RATE_12 || dt == RDOF_DT_RATE_34 || dt == RDOF_DT_RATE_1;
}

static void radio_frame(void *ctx, const char *channel, const rdof_frame_t *f)
{
    sx_t *e = ctx;
    if (!data_dt(f->dt))
        return;
    rdof_ipgw_where_t w = {.source = 100, .slot = f->slot, .freq = (uint32_t)e->tr.freq_hz, .cc = -1};
    rdof_ipgw_burst(&e->g, &w, f->dt, f->burst, e->t_now);
}

/* the SDR channel's own sink (sdr_task.c's), source = the channel */
static void chan_frame(void *ctx, const char *channel, const rdof_frame_t *f)
{
    chan_ctx_t *c = ctx;
    if (!data_dt(f->dt))
        return;
    rdof_ipgw_where_t w = {.source = c->c, .slot = f->slot, .freq = c->c ? TX_CH : RX_CH, .cc = CC};
    rdof_ipgw_burst(&c->e->g, &w, f->dt, f->burst, c->e->t_now);
}

static void on_burst(void *ctx, int c, const rdof_sdr_burst_t *b, double t)
{
    sx_t *e = ctx;
    rdof_sdrmodem_heard(&e->m, c, b);
    if (b->b.dt == RDOF_DT_IDLE || c < 0 || c > 1 || e->nair[c] >= MAXAIR)
        return;
    air_t *a = &e->air[c][e->nair[c]++];
    memcpy(a->burst, b->b.burst, RDOF_BURST_BYTES);
    a->dt = b->b.dt;
    a->slot = b->b.slot;
    a->family = b->b.family;
}
static bool muted(void *ctx, int64_t p) { return rdof_sdrmodem_muted_at(&((sx_t *)ctx)->m, p); }

static bool g_open(void *ctx, int flow) { return true; }
static void g_close(void *ctx, int flow) {}
static bool g_send(void *ctx, int flow, uint32_t ip, uint16_t port, const uint8_t *data, size_t len, int ttl)
{
    sx_t *e = ctx;
    e->udp++;
    e->udp_ip = ip;
    e->udp_len = len < sizeof e->udp_data ? len : sizeof e->udp_data;
    memcpy(e->udp_data, data, e->udp_len);
    return true;
}
static bool g_dmr(void *ctx, const rdof_ipgw_dmr_t *p)
{
    sx_t *e = ctx;
    uint8_t sid[4] = {0xA5, 0, 0, (uint8_t)++e->sid};
    return e->full ? rdof_ipgw_tx_repeater(&e->x, &e->r, p, sid) : rdof_ipgw_tx_txvr(&e->x, &e->t, p, sid);
}

static sx_t *sx_new(bool full, int inbound_slot)
{
    sx_t *e = calloc(1, sizeof *e);
    e->full = full;
    const rdof_sdrmodem_io_t io = {io_reply, io_channel, io_tune, e, NULL};
    CHECK(rdof_sdrmodem_init(&e->m, &io, FS, FS, full) == 0, "modem init");
    const rdof_modem_port_t port = {p_write, p_wait, NULL, p_clock, p_sleep, e};
    rdof_modem_init(&e->client, &port);
    e->client.on_rx = on_rx;
    e->client.on_lost = on_lost;
    e->client.on_nak = on_nak;
    e->client.cb_ctx = e;
    CHECK(rdof_modem_start(&e->client, 1), "version");
    rdof_radio_init(&e->radio, &e->client, rdof_radio_detect(e->client.version));
    rdof_tracker_init(&e->tr, "radio", 0.0, 0.0);
    rdof_sink_t rs = {.on_frame = radio_frame, .on_call = radio_call, .ctx = e};
    rdof_tracker_add_sink(&e->tr, rs);

    rdof_ipgw_config_t c;
    const char *json = "{\"ip_gateway\": {\"id\": 9990, \"nat\": true, \"up_pps\": 0, \"down_pps\": 0}}";
    char err[160];
    CHECK(rdof_ipgw_settings_parse(json, strlen(json), &c, err, sizeof err), "settings: %s", err);
    rdof_ipgw_init(&e->g, &c);
    e->g.open = g_open;
    e->g.close = g_close;
    e->g.send_udp = g_send;
    e->g.send_dmr = g_dmr;
    e->g.ctx = e;

    /* the receiver: the RX channel, and (full duplex) the outbound, as a listener on it would hear it */
    const rdof_sdr_rx_config_t rc = {FS, CENTER, 0, 0, true, CHUNK};
    const rdof_sdr_channel_t ch[2] = {{"rx", RX_CH, CC, inbound_slot}, {"out", TX_CH, CC, 1}};
    CHECK(rdof_sdr_rx_init(&e->rx, &rc, ch, full ? 2 : 1, NULL, 0) == 0, "receiver");
    e->rx.on_burst = on_burst;
    e->rx.muted = muted;
    e->rx.burst_ctx = e;
    for (int i = 0; i < (full ? 2 : 1); i++) {
        e->cc[i] = (chan_ctx_t){e, i};
        rdof_sink_t s = {.on_frame = chan_frame, .ctx = &e->cc[i]};
        CHECK(rdof_sdr_rx_add_sink(&e->rx, i, &s), "sink");
    }

    if (full) {
        rdof_repeater_init(&e->r, &e->radio, 9990601, RX_CH, TX_CH, CC);
        e->r.clock = p_clock;
        e->r.ctx = e;
        e->r.receiver.hear = hear;
        e->r.receiver.ctx = e;
        e->g.sync48 = rdof_data_sync(RDOF_BS_VOICE);
        CHECK(rdof_repeater_start(&e->r) && e->m.duplex, "repeater start: %s", e->client.error);
    } else {
        rdof_txvr_init(&e->t, &e->radio, 9990601);
        e->t.channels[0] = (rdof_channel_t){RX_CH, CC, 255};
        e->t.nchannels = 1;
        e->t.has_idle = true;
        e->t.idle = (rdof_channel_t){RX_CH, CC, 255};
        e->t.clock = p_clock;
        e->t.ctx = e;
        e->t.receiver.hear = hear;
        e->t.receiver.ctx = e;
        e->g.sync48 = rdof_data_sync(RDOF_MS_VOICE);
        CHECK(rdof_txvr_start(&e->t) && !e->m.duplex, "txvr start: %s", e->client.error);
    }
    return e;
}

static void sx_free(sx_t *e)
{
    rdof_sdr_rx_free(&e->rx);
    rdof_sdrmodem_free(&e->m);
    free(e);
}

static uint32_t rng = 4242;
static float noise(void)
{
    float s = 0;
    for (int i = 0; i < 4; i++) {
        rng = rng * 1664525u + 1013904223u;
        s += (float)(rng >> 8) / 16777216.0f - 0.5f;
    }
    return s * 1.732f;
}

/* s seconds: polled every chunk; the modem's signal and the station's into the receiver. reply_at: when the
 * internet answers (once), -1 never. */
static void sx_run(sx_t *e, rdof_mod_t *st, double s, double reply_at)
{
    static rdof_cf_t iq[CHUNK], o[CHUNK];
    const int chunks = (int)(s * FS / CHUNK);
    for (int c = 0; c < chunks; c++) {
        if (reply_at >= 0 && e->t_now >= reply_at) {
            rdof_ipgw_from_net(&e->g, 0, PEER_IP, 53, (const uint8_t *)"answer", 6, e->t_now);
            reply_at = -1;
        }
        if (e->full) {
            rdof_ipgw_tx_more(&e->x, &e->r);
            CHECK(rdof_repeater_poll(&e->r), "poll");
        } else {
            CHECK(rdof_txvr_poll(&e->t), "poll");
        }
        rdof_ipgw_tick(&e->g, e->t_now);
        rdof_tracker_tick(&e->tr, e->t_now);
        rdof_sdrmodem_pull_iq(&e->m, e->rx.samples, iq, CHUNK);
        for (int i = 0; i < CHUNK; i++)
            iq[i].re *= 0.5f, iq[i].im *= 0.5f;
        if (st) {
            rdof_mod_iq(st, o, CHUNK);
            for (int i = 0; i < CHUNK; i++)
                iq[i].re += 0.4f * o[i].re, iq[i].im += 0.4f * o[i].im;
        }
        for (int i = 0; i < CHUNK; i++)
            iq[i].re += 0.02f * noise(), iq[i].im += 0.02f * noise();
        rdof_sdr_rx_process(&e->rx, iq, CHUNK);
        e->t_now += (double)CHUNK / FS;
    }
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* The reply on air on channel c: header to the radio and its blocks; its length, -1 none. */
static int on_air(const sx_t *e, int c, uint8_t *out, rdof_family_t *fam, int *slot)
{
    for (int i = 0; i < e->nair[c]; i++) {
        const air_t *a = &e->air[c][i];
        if (a->dt != RDOF_DT_DATA_HEADER)
            continue;
        rdof_data_header_t h;
        rdof_decode_data_header(a->burst, &h);
        if (!h.crc_ok || h.dst_id != RADIO || h.src_id != GW_ID)
            continue;
        *fam = a->family;
        *slot = a->slot;
        int len = 0, got = 0;
        for (int k = i + 1; k < e->nair[c] && got < h.blocks; k++) {
            const air_t *b = &e->air[c][k];
            if (b->dt != RDOF_DT_RATE_12 || b->slot != a->slot)
                continue;
            if (b->family != a->family)
                return -1;
            rdof_data_block_t d;
            if (!rdof_decode_data_block(b->burst, RDOF_DT_RATE_12, &d))
                return -1;
            memcpy(out + len, d.payload, 12);
            len += 12;
            got++;
        }
        if (got < h.blocks || !rdof_check_crc32(out, (size_t)len))
            return -1;
        return len - 4 - h.pad_octets;
    }
    return -1;
}

/* the radio's packet from a station on RX_CH, MS framed, one burst every 60 ms from 50 ms on */
static int station(rdof_mod_t *st, rdof_ipgw_dmr_t *p)
{
    uint8_t d[128];
    size_t n = rdof_ipgw_udp_datagram(RADIO_IP, 5000, PEER_IP, 53, 7, (const uint8_t *)"query", 5, d, sizeof d);
    CHECK(rdof_ipgw_build_dmr(RADIO, GW_ID, RDOF_SAP_IP, d, n, CC, rdof_data_sync(RDOF_MS_VOICE), p), "build");
    rdof_mod_init(st, FS, (double)RX_CH - CENTER, 4.0, 1.0f, 64, 0);
    const int64_t two = 2 * rdof_mod_slot_samples(st);
    for (int k = 0; k < p->nbursts; k++)
        rdof_mod_queue_burst(st, FS / 20 + k * two, p->bursts[k], NULL, RDOF_MOD_RAMPS);
    return p->nbursts;
}

static void t_half(void)
{
    sx_t *e = sx_new(false, 2);
    static rdof_ipgw_dmr_t p;
    rdof_mod_t st;
    int nb = station(&st, &p);
    sx_run(e, &st, 0.05 + 0.06 * nb + 0.04, -1); /* to just after the last block: the reply as quick as an ACK */
    CHECK(e->udp == 1 && e->udp_ip == PEER_IP && e->udp_len == 5 && !memcmp(e->udp_data, "query", 5) &&
              e->g.n.up_packets == 1 && e->g.n.rx_dup == 1,
          "half: up once (heard by the channel and the roamer): %d, dup %u", e->udp, e->g.n.rx_dup);
    int before = e->nair[0];
    sx_run(e, &st, 2.5, e->t_now);
    uint8_t d[256];
    rdof_family_t fam = RDOF_FAMILY_COUNT;
    int slot = 0;
    int len = on_air(e, 0, d, &fam, &slot);
    CHECK(len == 34 && rd32(d + 12) == PEER_IP && rd32(d + 16) == RADIO_IP && !memcmp(d + 28, "answer", 6),
          "half: the reply on the radio's frequency: %d", len);
    CHECK(fam == RDOF_FAMILY_MS, "half: MS data sync: family %d", fam);
    CHECK(e->nair[0] > before && e->m.tx_bursts >= (uint32_t)(1 + (34 + 4 + 11) / 12), "half: on air: %u",
          (unsigned)e->m.tx_bursts);
    CHECK(e->udp == 1 && e->g.n.down_packets == 1 && e->g.n.rx_dup >= 2, "half: our reply heard back, not up: %d %u",
          e->udp, e->g.n.rx_dup);
    printf("ipgw sdr_transceiver half duplex: %d bursts up, the reply on air (%u bursts, MS sync), dup %u\n", nb,
           (unsigned)e->m.tx_bursts, e->g.n.rx_dup);
    rdof_mod_free(&st);
    sx_free(e);
}

static void t_full(int slot)
{
    sx_t *e = sx_new(true, slot);
    static rdof_ipgw_dmr_t p;
    rdof_mod_t st;
    int nb = station(&st, &p);
    sx_run(e, &st, 0.05 + 0.06 * nb + 0.04, -1);
    CHECK(e->udp == 1 && e->udp_ip == PEER_IP && !memcmp(e->udp_data, "query", 5) && e->g.n.up_packets == 1,
          "full slot %d: up once: %d", slot, e->udp);
    sx_run(e, &st, 2.5, e->t_now);
    uint8_t d[256];
    rdof_family_t fam = RDOF_FAMILY_COUNT;
    int s = 0;
    int len = on_air(e, 1, d, &fam, &s);
    CHECK(len == 34 && rd32(d + 16) == RADIO_IP && !memcmp(d + 28, "answer", 6), "full slot %d: the reply: %d", slot,
          len);
    CHECK(fam == RDOF_FAMILY_BS && s == slot, "full slot %d: BS sync on slot %d: family %d slot %d", slot, slot, fam,
          s);
    CHECK(e->udp == 1 && e->g.n.down_packets == 1, "full slot %d: the reply heard on the outbound, not up: %d", slot,
          e->udp);
    /* repeated on the outbound, BS framed, on its slot */
    int rep = 0;
    for (int i = 0; i < e->nair[1]; i++)
        for (int k = 0; k < nb; k++) {
            uint8_t a[12], b[12];
            rdof_bptc196_decode(e->air[1][i].burst, a);
            rdof_bptc196_decode(p.bursts[k], b);
            if (e->air[1][i].dt == (p.control[k] & 0x0F) && !memcmp(a, b, 10) &&
                e->air[1][i].family == RDOF_FAMILY_BS && e->air[1][i].slot == slot) {
                rep++;
                break;
            }
        }
    CHECK(rep == nb, "full slot %d: the radio's packet repeated: %d of %d", slot, rep, nb);
    /* heard three times (the RX channel, the repeater, the outbound's listener), the reply once more: up once */
    CHECK(e->g.n.rx_dup == 3, "full slot %d: dup %u", slot, e->g.n.rx_dup);
    printf("ipgw sdr_transceiver full duplex slot %d: up once, repeated (%d bursts, BS), the reply on the outbound\n",
           slot, rep);
    rdof_mod_free(&st);
    sx_free(e);
}

#endif

int main(void)
{
#if RDOF_FEATURE_REPEATER && RDOF_FEATURE_SDR_RECEIVER && RDOF_FEATURE_TRACKER && RDOF_FEATURE_DATA_BLOCKS
    t_half();
    t_full(1);
    t_full(2);
#endif
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
