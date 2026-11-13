// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* Two trunked sites (rdof/t3unit.h) with simulated MSs over the full burst path (rdof/t3sim.h), joined by an
 * in-process broker (retained messages, + wildcards, no local echo) with a stand-in router (pc/{dst} to dl/{the node
 * whose record has dst}): registration and its records, a wide group call heard at the other site, an individual
 * call across the sites through the setup, provisioning (cfg/net), commands, and a restart that reads the records
 * back. */
#include <stdalign.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/fec.h"
#include "rdof/ipgw.h"
#include "rdof/t3cfg.h"
#include "rdof/t3sim.h"
#include "rdof/t3unit.h"

static int fails;

#define CHECK(cond, ...)                                                                                           \
    do {                                                                                                           \
        if (!(cond)) {                                                                                             \
            fails++;                                                                                               \
            printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                                 \
            printf(__VA_ARGS__);                                                                                   \
            printf("\n");                                                                                          \
        }                                                                                                          \
    } while (0)

/* ── the broker ─────────────────────────────────────────────────────────── */

#define CLIENTS 3
#define FILTERS 32
#define RETAINED 256
#define QUEUE 4096

typedef struct {
    char topic[128];
    uint8_t *data;
    size_t len;
} msg_t;

static struct {
    char filt[CLIENTS][FILTERS][128];
    int nfilt[CLIENTS];
    msg_t retained[RETAINED];
    int nret;
    struct {
        int to;
        msg_t m;
    } q[QUEUE];
    int qn;
    rdof_t3unit_t *unit[CLIENTS];
    int published, pc_routed;
} B;

static bool match(const char *f, const char *t)
{
    while (*f && *t) {
        if (*f == '+') {
            while (*t && *t != '/')
                t++;
            f++;
            continue;
        }
        if (*f == '#')
            return true;
        if (*f != *t)
            return false;
        f++, t++;
    }
    return !*f && !*t;
}

static msg_t copy_msg(const char *topic, const void *data, size_t len)
{
    msg_t m;
    snprintf(m.topic, sizeof m.topic, "%s", topic);
    m.data = malloc(len + 1);
    memcpy(m.data, data, len);
    m.data[len] = 0;
    m.len = len;
    return m;
}

static void enqueue(int to, const char *topic, const void *data, size_t len)
{
    if (B.qn < QUEUE) {
        B.q[B.qn].to = to;
        B.q[B.qn++].m = copy_msg(topic, data, len);
    }
}

static int record_node(uint32_t id, char *node, size_t cap)
{
    char suffix[32];
    snprintf(suffix, sizeof suffix, "/ms/%u", (unsigned)id);
    for (int i = 0; i < B.nret; i++) {
        const char *t = B.retained[i].topic;
        const size_t n = strlen(t), sn = strlen(suffix);
        if (n > sn && !strcmp(t + n - sn, suffix) && !strncmp(t, "DMR/node/", 9) && B.retained[i].len) {
            snprintf(node, cap, "%.*s", (int)(n - sn - 9), t + 9);
            return 1;
        }
    }
    return 0;
}

static void broker_publish(int from, const char *topic, const void *data, size_t len, bool retain)
{
    B.published++;
    if (retain) {
        int i = 0;
        while (i < B.nret && strcmp(B.retained[i].topic, topic))
            i++;
        if (i < B.nret)
            free(B.retained[i].data);
        if (!len) {
            if (i < B.nret)
                B.retained[i] = B.retained[--B.nret];
        } else if (i < RETAINED) {
            B.retained[i] = copy_msg(topic, data, len);
            if (i == B.nret)
                B.nret++;
        }
    }
    for (int c = 0; c < CLIENTS; c++) {
        if (c == from || !B.unit[c])
            continue;
        for (int k = 0; k < B.nfilt[c]; k++)
            if (match(B.filt[c][k], topic)) {
                enqueue(c, topic, data, len);
                break;
            }
    }
    /* the router: pc/{dst}/{node} to dl/{where dst is registered} */
    unsigned dst;
    char node[48], dl[128];
    if (sscanf(topic, "DMR/pc/%u/", &dst) == 1 && record_node(dst, node, sizeof node)) {
        snprintf(dl, sizeof dl, "DMR/dl/%s", node);
        B.pc_routed++;
        broker_publish(-1, dl, data, len, false);
    }
}

static void broker_deliver(void)
{
    for (int i = 0; i < B.qn; i++) {
        if (B.unit[B.q[i].to])
            rdof_t3unit_message(B.unit[B.q[i].to], B.q[i].m.topic, B.q[i].m.data, B.q[i].m.len);
        free(B.q[i].m.data);
    }
    B.qn = 0;
}

static const msg_t *retained(const char *topic)
{
    for (int i = 0; i < B.nret; i++)
        if (!strcmp(B.retained[i].topic, topic))
            return &B.retained[i];
    return NULL;
}

/* ── a site ─────────────────────────────────────────────────────────────── */

typedef struct {
    int client;
    rdof_t3unit_t *u;
    void *mem;
    rdof_t3sim_t sim;
    rdof_t3ms_t *ms[2];
    uint32_t id[2];
    int logs;
} site_t;

static void io_publish(void *ctx, const char *topic, const void *data, size_t len, int qos, bool retain)
{
    site_t *s = ctx;
    broker_publish(s->client, topic, data, len, retain);
}

static void io_subscribe(void *ctx, const char *const *f, int n)
{
    site_t *s = ctx;
    B.nfilt[s->client] = 0;
    for (int i = 0; i < n && i < FILTERS; i++)
        snprintf(B.filt[s->client][B.nfilt[s->client]++], 128, "%s", f[i]);
    for (int r = 0; r < B.nret; r++) /* the retained ones, as a broker sends them on a subscribe */
        for (int i = 0; i < B.nfilt[s->client]; i++)
            if (match(B.filt[s->client][i], B.retained[r].topic)) {
                enqueue(s->client, B.retained[r].topic, B.retained[r].data, B.retained[r].len);
                break;
            }
}

static void io_log(void *ctx, const char *text)
{
    site_t *s = ctx;
    s->logs++;
    if (getenv("RDOF_TEST_VERBOSE"))
        printf("[%d] %s\n", s->client, text);
}

static uint32_t rng = 12345;
static uint32_t io_rand(void *ctx)
{
    rng ^= rng << 13, rng ^= rng >> 17, rng ^= rng << 5;
    return rng;
}

#define TG 9
#define T0 1793912651000LL

/* site A's IP gateway (tier3.md 3.16): packet data to the site goes in, what it sends goes down as the site's data */
static struct {
    rdof_ipgw_t *g;
    site_t *site;
    int bursts, sent, udp;
} GW;

static void io_data(void *ctx, int ch, int dt, const uint8_t *burst, uint32_t src, uint32_t dst)
{
    site_t *s = ctx;
    if (!GW.g || s != GW.site)
        return;
    GW.bursts++;
    const rdof_ipgw_where_t w = {.source = 0, .slot = (ch & 1) + 1, .freq = 0, .cc = 1};
    rdof_ipgw_burst(GW.g, &w, dt, burst, 1.0);
}

static bool gw_send_dmr(void *ctx, const rdof_ipgw_dmr_t *p)
{
    site_t *s = ctx;
    GW.sent++;
    return rdof_t3unit_data_tx(s->u, p->src, p->dst, false, p->nbursts, p->control, &p->bursts[0][0]);
}

static bool gw_open(void *ctx, int flow) { return true; }
static bool gw_send_udp(void *ctx, int flow, uint32_t ip, uint16_t port, const uint8_t *data, size_t len, int ttl)
{
    GW.udp++;
    return true;
}

static void site_open(site_t *s, int client, const char *node, int site_no, uint32_t id0, int64_t at_ms)
{
    memset(s, 0, sizeof *s);
    s->client = client;
    rdof_t3_cfg_t cfg;
    rdof_t3_policy_t pol;
    rdof_t3cfg_default(&cfg, &pol);
    cfg.site = (uint16_t)site_no;
    pol.wide_tg.all = 1;
    s->u = calloc(1, rdof_t3unit_size());
    s->mem = aligned_alloc(8, (rdof_t3_site_size() + 7) / 8 * 8);
    const rdof_t3unit_io_t io = {io_publish, io_subscribe, io_log, NULL, io_rand, s, io_data};
    char err[120];
    CHECK(rdof_t3unit_init(s->u, s->mem, &cfg, &pol, &io, "DMR", node, 0, "test", err, sizeof err), "init: %s",
          err);
    B.unit[client] = s->u;
    const uint32_t tg[1] = {TG};
    const uint16_t chans[1] = {1};
    for (int i = 0; i < 2; i++) {
        s->id[i] = id0 + (uint32_t)i;
        s->ms[i] = malloc(rdof_t3ms_size());
        rdof_t3ms_init(s->ms[i], s->id[i], tg, 1, chans, 1, 7 + (uint32_t)client * 2 + (uint32_t)i);
        rdof_t3ms_do(s->ms[i], RDOF_T3MS_DO_REGISTER, TG, 0, 0, at_ms + 300 + 400 * i);
    }
    rdof_t3sim_init(&s->sim, s->u, 2, s->ms, s->id);
    rdof_t3unit_run(s->u, (at_ms - T0) / 30, at_ms);
    rdof_t3unit_link(s->u, true);
}

static void site_close(site_t *s)
{
    B.unit[s->client] = NULL;
    for (int i = 0; i < 2; i++)
        free(s->ms[i]);
    free(s->mem);
    free(s->u);
}

static site_t A, Bs;
static int64_t K;

static void run(int ms)
{
    for (int64_t end = K + ms / 30; K < end; K++) {
        const int64_t now = T0 + K * 30;
        rdof_t3sim_slot(&A.sim, K, now);
        rdof_t3sim_slot(&Bs.sim, K, now);
        broker_deliver();
        if (K % 10 == 0) {
            rdof_t3unit_tick(A.u, now);
            rdof_t3unit_tick(Bs.u, now);
        }
    }
}

static rdof_t3ms_status_t ms_status(site_t *s, int i)
{
    rdof_t3ms_status_t st;
    rdof_t3ms_status(s->ms[i], &st);
    return st;
}

/* A group call on TG from A's first MS, talk_ms long: how long A's other MS stays on the payload channel. */
static int hang_ms(int talk_ms)
{
    rdof_t3ms_do(A.ms[0], RDOF_T3MS_DO_GROUP, TG, talk_ms, 0, T0 + K * 30);
    int waited = 0;
    while (ms_status(&A, 1).state != RDOF_T3MS_PAYLOAD && waited < 3000)
        run(30), waited += 30;
    int held = 0;
    while (ms_status(&A, 1).state == RDOF_T3MS_PAYLOAD && held < 20000)
        run(30), held += 30;
    run(1000);
    return held;
}


/* ── an external MS, through the sim's hooks only (rdofradio --sim-ext) ── */

static struct {
    rdof_t3ms_t *ms;
    int voice_n, dup, stray;
    uint32_t cach_bad, cach_ok, heard, fates[3];
} X;

static int ext_in(void *ctx, int64_t k, rdof_t3sim_in_t *in, int max)
{
    (void)ctx;
    int n = 0;
    rdof_t3ms_tx_t tx;
    if (X.stray && max > n) { /* a burst on the slot not on air */
        rdof_t3ms_tx_t t = {.ch = (uint8_t)(1 - (k & 1)), .kind = 2, .what = RDOF_T3_VOICE_FRAME};
        in[n].ch = t.ch;
        rdof_t3sim_ms_burst(&t, 1, 1, &in[n].b);
        n++;
    }
    if (!rdof_t3ms_tx(X.ms, k, T0 + k * 30, &tx))
        return n;
    int vn = X.voice_n;
    if (tx.kind != 1)
        X.voice_n = tx.what == RDOF_T3_VOICE_FRAME ? (vn + 1) % 6 : 0;
    for (int d = 0; d <= (X.dup && tx.kind == 1) && n < max; d++) {
        in[n].ch = tx.ch;
        if (rdof_t3sim_ms_burst(&tx, vn, rdof_t3unit_cfg(A.u)->cc, &in[n].b))
            n++;
    }
    return n;
}

static void ext_out(void *ctx, int64_t k, const rdof_t3sim_out_t *out, int ncar, int at_next,
                    const rdof_t3sim_in_t *in, int nin)
{
    (void)ctx;
    for (int e = 0; e < nin; e++)
        X.fates[in[e].fate]++;
    int at, slot, lcss;
    uint32_t p17;
    if (rdof_t3rf_cach_read(out[0].cach, &at, &slot, &lcss, &p17) && at == out[0].at && slot == (int)(k & 1) + 1)
        X.cach_ok++;
    else
        X.cach_bad++;
    rdof_t3ms_status_t st;
    rdof_t3ms_status(X.ms, &st);
    if ((st.ch & 1) != (int)(k & 1) || st.ch / 2 >= ncar)
        return;
    const rdof_t3sim_out_t *o = &out[st.ch / 2];
    X.heard += o->pdu_ok;
    rdof_t3ms_rx(X.ms, st.ch, k, (uint8_t)(o->pdu_ok ? o->dt : 0), o->pdu, st.ch == 0 && at_next > 0);
}

/* ── the Rust's mirrors of the C structs, and its memory ─────────────────── */

#define LAYOUT(t) {#t, sizeof(t), _Alignof(t)}
#define HEAD(t) {#t " head", offsetof(t, ctx) + sizeof(void *), _Alignof(t)}

static void test_layouts(void)
{
    static const rdof_t3_layout_t c[] = {
        LAYOUT(rdof_t3unit_peer_t),     LAYOUT(rdof_t3unit_io_t),     LAYOUT(rdof_t3unit_counts_t),
        LAYOUT(rdof_mn_hdr_t),          LAYOUT(rdof_mn_setup_t),      LAYOUT(rdof_mn_ev_t),
        LAYOUT(rdof_mn_record_t),       LAYOUT(rdof_mn_where_t),      LAYOUT(rdof_mn_chan_status_t),
        LAYOUT(rdof_mn_trunk_status_t), LAYOUT(rdof_mn_status_t),     LAYOUT(rdof_mn_carrier_t),
        LAYOUT(rdof_mn_trunk_info_t),   LAYOUT(rdof_mn_timer_t),      LAYOUT(rdof_mn_site_cfg_t),
        LAYOUT(rdof_mn_tg_cfg_t),       LAYOUT(rdof_mn_ms_cfg_t),     LAYOUT(rdof_mn_cfg_t),
        LAYOUT(rdof_mn_cmd_t),          HEAD(rdof_mn_up_t),           HEAD(rdof_mn_down_t),
    };
    const size_t n = sizeof c / sizeof c[0];
    size_t i = 0;
    for (const rdof_t3_layout_t *r; (r = rdof_t3unit_layout(i)) && r->name; i++) {
        CHECK(i < n, "rust lists %s, C doesn't", r->name);
        if (i >= n)
            break;
        CHECK(strcmp(r->name, c[i].name) == 0, "%zu: rust %s, C %s", i, r->name, c[i].name);
        CHECK(r->size == c[i].size && r->align == c[i].align, "%s: rust %zu/%zu, C %zu/%zu", c[i].name, r->size,
              r->align, c[i].size, c[i].align);
    }
    CHECK(i == n, "rust lists %zu structs, C %zu", i, n);
    CHECK(rdof_mn_up_size() == sizeof(rdof_mn_up_t) && rdof_mn_up_align() == _Alignof(rdof_mn_up_t) &&
              rdof_mn_down_size() == sizeof(rdof_mn_down_t) && rdof_mn_down_align() == _Alignof(rdof_mn_down_t),
          "mqtt-net's sizes");
}

/* The unit's memory: room for the uplinks and the downlink, malloc's alignment enough, less refused; a unit off the
   network publishes nothing. */
static int quiet_publishes;
static void quiet_publish(void *ctx, const char *topic, const void *data, size_t len, int qos, bool retain)
{
    quiet_publishes++;
}

static void test_memory(void)
{
    const size_t size = rdof_t3unit_size();
    CHECK(size > 4 * sizeof(rdof_mn_up_t) + sizeof(rdof_mn_down_t) + sizeof(rdof_mn_cfg_t), "size %zu", size);
    rdof_t3_cfg_t cfg;
    rdof_t3_policy_t pol;
    rdof_t3cfg_default(&cfg, &pol);
    const rdof_t3unit_io_t io = {quiet_publish, NULL, NULL, NULL, NULL, NULL};
    void *mem = aligned_alloc(8, (rdof_t3_site_size() + 7) / 8 * 8);
    uint8_t *raw = malloc(size + 16);
    char err[120] = "";
    CHECK(!rdof_t3unit_init((rdof_t3unit_t *)(raw + 1), mem, &cfg, &pol, &io, "DMR", "x", 0, NULL, err, sizeof err) &&
              strstr(err, "aligned"),
          "misaligned memory refused: %s", err);
    rdof_t3unit_t *u = (rdof_t3unit_t *)(raw + 8);
    memset(raw, 0xA5, size + 16);
    CHECK(rdof_t3unit_init(u, mem, &cfg, &pol, &io, NULL, NULL, -1, NULL, err, sizeof err), "init: %s", err);
    CHECK(raw[8 + size] == 0xA5 && raw[7] == 0xA5, "init stays in its %zu bytes", size);
    CHECK(rdof_t3unit_cfg(u)->cc == cfg.cc && rdof_t3unit_cfg(u)->ncarriers == cfg.ncarriers, "cfg");
    CHECK(rdof_t3unit_rf(u)->tscc && rdof_t3unit_rf(u)->ncarriers == cfg.ncarriers, "the radio layer");
    int np = -1;
    const uint32_t *ids = NULL;
    CHECK(rdof_t3unit_peers(u, &np) && np == 0 && rdof_t3unit_neighbors(u) == 0 &&
              rdof_t3unit_tg_policies(u, &ids) == 0 && ids,
          "no peers, neighbors or cfg/tg (%d)", np);
    rdof_t3unit_run(u, 0, T0);
    rdof_t3unit_run(u, 40, T0 + 1200);
    rdof_t3unit_message(u, "DMR/cfg/net", "{\"rev\":1}", 9);
    rdof_t3unit_link(u, true);
    rdof_t3unit_tick(u, T0 + 20000);
    CHECK(quiet_publishes == 0 && rdof_t3unit_counts(u)->cfg_applied == 0, "off the network: %d published",
          quiet_publishes);
    char st[2048];
    CHECK(rdof_t3unit_status_json(u, st, sizeof st) > 0 && strstr(st, "\"uptime_s\":20"), "status: %s", st);
    CHECK(rdof_t3unit_at(u, 40) >= 0 && rdof_t3unit_at(u, 40 - RDOF_T3UNIT_RING) == -1 && rdof_t3unit_at(u, -1) == -1,
          "the held slots");
    free(raw);
    free(mem);
}

/* rdof/t3sim.h's Rust (rust/src/t3sim.rs) mirrors its structs: the same layouts. */
const rdof_t3_layout_t *rdof_t3sim_layout(size_t i);

static void test_sim_layouts(void)
{
    static const rdof_t3_layout_t c[] = {LAYOUT(rdof_t3sim_in_t), LAYOUT(rdof_t3sim_out_t), LAYOUT(rdof_t3sim_t)};
    const size_t n = sizeof c / sizeof c[0];
    size_t i = 0;
    for (const rdof_t3_layout_t *r; (r = rdof_t3sim_layout(i)) && r->name; i++) {
        CHECK(i < n, "rust lists %s, C doesn't", r->name);
        if (i >= n)
            break;
        CHECK(strcmp(r->name, c[i].name) == 0, "%zu: rust %s, C %s", i, r->name, c[i].name);
        CHECK(r->size == c[i].size && r->align == c[i].align, "%s: rust %zu/%zu, C %zu/%zu", c[i].name, r->size,
              r->align, c[i].size, c[i].align);
    }
    CHECK(i == n, "rust lists %zu structs, C %zu", i, n);
}

/* A simulated MS's voice bursts B-E carry the call's LC embedded (361-1 7.1.4): a radio that hears the call late
 * still learns who talks to whom. */
static void test_sim_embedded(void)
{
    const rdof_t3ms_tx_t tx = {.ch = 1, .kind = 2, .what = RDOF_T3_VOICE_FRAME, .group = 0, .src = 1000002,
                               .dst = 1000001};
    uint8_t raw[128];
    for (int n = 1; n <= 4; n++) {
        rdof_burst_t b;
        CHECK(rdof_t3sim_ms_burst(&tx, n, 2, &b), "voice burst %d", n);
        rdof_embedded_signalling(b.burst, raw + (n - 1) * 32);
    }
    uint8_t lc[9];
    CHECK(rdof_embedded_lc_decode(raw, lc), "the embedded LC decodes");
    const uint32_t dst = (uint32_t)lc[3] << 16 | lc[4] << 8 | lc[5], src = (uint32_t)lc[6] << 16 | lc[7] << 8 | lc[8];
    CHECK(lc[0] == 3 && dst == 1000001 && src == 1000002, "unit to unit LC 1000002 -> 1000001: flco %d, %u -> %u",
          lc[0] & 0x3f, src, dst);
}

/* Four carriers (7 payload channels) through the radio layer and the burst path: seven group calls fill every
   payload channel; an emergency then pre-empts one, its talker de-keyed by a Reverse Channel burst on the other
   slot of its carrier, read back from the air; and a status to a talkgroup. */
static void test_carriers4(void)
{
    rdof_t3_cfg_t cfg;
    rdof_t3_policy_t pol;
    rdof_t3cfg_default(&cfg, &pol);
    cfg.ncarriers = 4;
    for (int c = 1; c < 4; c++)
        cfg.carriers[c] = (rdof_t3_carrier_t){438600000u + 25000u * (uint32_t)c, 431000000u + 25000u * (uint32_t)c,
                                              (uint16_t)(c + 1), 0, 1};
    cfg.preempt = 1;
    const rdof_t3unit_io_t io = {quiet_publish, NULL, NULL, NULL, NULL, NULL};
    rdof_t3unit_t *u = calloc(1, rdof_t3unit_size());
    void *mem = aligned_alloc(8, (rdof_t3_site_size() + 7) / 8 * 8);
    char err[120] = "";
    CHECK(rdof_t3unit_init(u, mem, &cfg, &pol, &io, NULL, NULL, -1, NULL, err, sizeof err), "init: %s", err);
    CHECK(rdof_t3unit_rf(u)->ncarriers == 4, "4 carriers in the radio layer");
    enum { N = 9 };
    rdof_t3ms_t *ms[N];
    uint32_t id[N];
    const uint16_t chans[4] = {1, 2, 3, 4};
    for (int i = 0; i < N; i++) {
        const uint32_t tg[1] = {i == 8 ? 11u : 10u + (uint32_t)i};
        id[i] = 3100200 + (uint32_t)i;
        ms[i] = malloc(rdof_t3ms_size());
        rdof_t3ms_init(ms[i], id[i], tg, 1, chans, 4, 50 + (uint32_t)i);
        rdof_t3ms_do(ms[i], RDOF_T3MS_DO_REGISTER, tg[0], 0, 0, T0 + 300 + 250 * i);
    }
    static rdof_t3sim_t sim;
    rdof_t3sim_init(&sim, u, N, ms, id);
    int64_t k = 0;
#define RUN(msec)                                                                                                  \
    for (int64_t end_ = k + (msec) / 30; k < end_; k++)                                                            \
    rdof_t3sim_slot(&sim, k, T0 + k * 30)
    RUN(8000);
    rdof_t3ms_status_t st;
    int regs = 0;
    for (int i = 0; i < N; i++)
        rdof_t3ms_status(ms[i], &st), regs += st.registered;
    CHECK(regs == N, "all registered (%d)", regs);
    /* seven group calls, on seven talkgroups: every payload channel, one each */
    for (int i = 0; i < 7; i++)
        rdof_t3ms_do(ms[i], RDOF_T3MS_DO_GROUP, 10 + (uint32_t)i, 40000, 0, T0 + k * 30 + 200 * i);
    RUN(6000);
    unsigned used = 0;
    int before[7];
    for (int i = 0; i < 7; i++) {
        rdof_t3ms_status(ms[i], &st);
        before[i] = st.ch;
        if (st.state == RDOF_T3MS_PAYLOAD)
            used |= 1u << st.ch;
    }
    CHECK(used == 0xFE, "channels 1-7 in use: 0x%02x", used);
    for (int c = 1; c < 4; c++)
        CHECK(rdof_t3unit_rf(u)->ch[2 * c].tx_traffic > 0 && rdof_t3unit_rf(u)->ch[2 * c + 1].tx_traffic > 0,
              "carrier %d repeats on both slots", c);
    rdof_t3ms_status(ms[8], &st);
    CHECK(st.state == RDOF_T3MS_PAYLOAD && st.ch == before[1], "MS 8 follows talkgroup 11 (ch %d)", st.ch);
    /* the emergency: no channel free, the call on a payload carrier's channel cleared, its talker told by RC */
    rdof_t3ms_do(ms[7], RDOF_T3MS_DO_GROUP, 99, 2000, RDOF_T3MS_F_EMERGENCY, T0 + k * 30);
    int waited = 0;
    do {
        RUN(30);
        waited += 30;
        rdof_t3ms_status(ms[7], &st);
    } while (st.state != RDOF_T3MS_PAYLOAD && waited < 10000);
    CHECK(st.state == RDOF_T3MS_PAYLOAD, "the emergency call got a channel (%d ms)", waited);
    int cleared = -1, ceased_ch = st.ch;
    for (int i = 0; i < 7; i++) {
        rdof_t3ms_status(ms[i], &st);
        if (st.clears)
            cleared = i;
    }
    /* the first call granted went on the TSCC carrier's LCN 1 (a keyed carrier first); the oldest after it goes */
    CHECK(before[0] == 1 && cleared == 1 && ceased_ch == before[1], "MS %d pre-empted, its channel %d to the emergency",
          cleared, ceased_ch);
    CHECK(sim.out_rc >= 1 && rdof_t3unit_rf(u)->ch[before[1] ^ 1].rc_sent >= 1, "RC on the other slot: %u %u",
          sim.out_rc, rdof_t3unit_rf(u)->ch[before[1] ^ 1].rc_sent);
    CHECK(sim.out_bad == 0, "no bad outbound blocks (%u)", sim.out_bad);
    rdof_t3ms_status(ms[8], &st);
    CHECK(st.clears == 1, "MS 8, a listener, back on the TSCC too");
    /* an emergency alarm to talkgroup 11, whose members are back on the TSCC */
    rdof_t3ms_status(ms[1], &st);
    const uint32_t done0 = st.calls_done;
    rdof_t3ms_do(ms[1], RDOF_T3MS_DO_STATUS, 11, 126, RDOF_T3MS_F_TO_GROUP, T0 + k * 30);
    RUN(2000);
    rdof_t3ms_status(ms[1], &st);
    CHECK(st.calls_done == done0 + 1 && st.last_reason == 0x60, "the alarm taken: done %u, reason 0x%02x", st.calls_done,
          st.last_reason);
#undef RUN
    for (int i = 0; i < N; i++)
        free(ms[i]);
    free(mem);
    free(u);
}

/* A full-duplex call through the radio layer: carrier 2 in offset timing, each party's inbound repeated on the
 * other's slot (361-4 6.6.9). */
static void test_duplex(void)
{
    rdof_t3_cfg_t cfg;
    rdof_t3_policy_t pol;
    rdof_t3cfg_default(&cfg, &pol);
    cfg.ncarriers = 2;
    cfg.carriers[1] = (rdof_t3_carrier_t){438625000u, 431025000u, 2, 0, 1};
    cfg.duplex = 1;
    const rdof_t3unit_io_t io = {quiet_publish, NULL, NULL, NULL, NULL, NULL};
    rdof_t3unit_t *u = calloc(1, rdof_t3unit_size());
    void *mem = aligned_alloc(8, (rdof_t3_site_size() + 7) / 8 * 8);
    char err[120] = "";
    CHECK(rdof_t3unit_init(u, mem, &cfg, &pol, &io, NULL, NULL, -1, NULL, err, sizeof err), "init: %s", err);
    rdof_t3ms_t *ms[2];
    uint32_t id[2] = {3100301, 3100302};
    const uint16_t chans[2] = {1, 2};
    const uint32_t tg[1] = {10};
    for (int i = 0; i < 2; i++) {
        ms[i] = malloc(rdof_t3ms_size());
        rdof_t3ms_init(ms[i], id[i], tg, 1, chans, 2, 70 + (uint32_t)i);
        rdof_t3ms_do(ms[i], RDOF_T3MS_DO_REGISTER, 10, 0, 0, T0 + 300 + 250 * i);
    }
    static rdof_t3sim_t sim;
    rdof_t3sim_init(&sim, u, 2, ms, id);
    int64_t k = 0;
#define RUN(msec)                                                                                                  \
    for (int64_t end_ = k + (msec) / 30; k < end_; k++)                                                            \
    rdof_t3sim_slot(&sim, k, T0 + k * 30)
    RUN(5000);
    rdof_t3ms_do(ms[0], RDOF_T3MS_DO_DUPLEX, id[1], 3000, 0, T0 + k * 30);
    RUN(3000);
    rdof_t3ms_status_t a, b;
    rdof_t3ms_status(ms[0], &a);
    rdof_t3ms_status(ms[1], &b);
    CHECK(a.state == RDOF_T3MS_PAYLOAD && b.state == RDOF_T3MS_PAYLOAD && a.ch == 2 && b.ch == 3,
          "both on carrier 2: %d/%d %d/%d", a.state, a.ch, b.state, b.ch);
    const rdof_t3rf_t *rf = rdof_t3unit_rf(u);
    CHECK(rf->ch[2].duplex && rf->ch[3].duplex, "offset timing on carrier 2");
    CHECK(rf->ch[2].rx_bursts > 20 && rf->ch[3].rx_bursts > 20, "each party heard on its own channel: %u %u",
          rf->ch[2].rx_bursts, rf->ch[3].rx_bursts);
    CHECK(rf->ch[2].tx_traffic > 20 && rf->ch[3].tx_traffic > 20, "each repeated on the other's slot: %u %u",
          rf->ch[2].tx_traffic, rf->ch[3].tx_traffic);
    RUN(5000);
    rdof_t3ms_status(ms[0], &a);
    rdof_t3ms_status(ms[1], &b);
    CHECK(a.calls_done == 1 && a.state == RDOF_T3MS_IDLE && b.state == RDOF_T3MS_IDLE, "cleared: %u %d %d",
          a.calls_done, a.state, b.state);
    CHECK(!rf->ch[2].open && !rf->ch[3].open && !rf->ch[2].duplex && !rf->ch[3].duplex, "both closed");
    CHECK(sim.out_bad == 0, "no bad outbound blocks (%u)", sim.out_bad);
#undef RUN
    for (int i = 0; i < 2; i++)
        free(ms[i]);
    free(mem);
    free(u);
}

int main(void)
{
    test_sim_embedded();
    test_layouts();
    test_sim_layouts();
    test_memory();
    test_carriers4();
    test_duplex();
    site_open(&A, 0, "site-a", 3, 3100001, T0);
    site_open(&Bs, 1, "site-b", 4, 3100101, T0);
    run(6000);

    /* registration: each MS registered, its record retained under its site */
    for (int i = 0; i < 2; i++) {
        CHECK(ms_status(&A, i).registered && ms_status(&Bs, i).registered, "MS %d registered", i);
    }
    const msg_t *r = retained("DMR/node/site-a/ms/3100001");
    CHECK(r && strstr((char *)r->data, "\"sub\":9"), "site A's record: %s", r ? (char *)r->data : "none");
    CHECK(retained("DMR/node/site-b/ms/3100102") != NULL, "site B's record");
    CHECK(retained("DMR/node/site-a/status") != NULL, "status published");
    bool tg_sub = false;
    for (int k = 0; k < B.nfilt[1]; k++)
        tg_sub |= !strcmp(B.filt[1][k], "DMR/tg/9/+");
    CHECK(tg_sub, "site B subscribes the talkgroup its MSs registered with");

    /* a wide group call at A, carried to B: B's MSs follow it */
    rdof_t3ms_do(A.ms[0], RDOF_T3MS_DO_GROUP, TG, 2500, 0, T0 + K * 30);
    run(2000);
    rdof_t3ms_status_t b0 = ms_status(&Bs, 0), a1 = ms_status(&A, 1);
    CHECK(a1.state == RDOF_T3MS_PAYLOAD && a1.heard_src == 3100001, "A's other MS follows (%d, %u)", a1.state,
          (unsigned)a1.heard_src);
    CHECK(b0.state == RDOF_T3MS_PAYLOAD && b0.heard_src == 3100001 && b0.heard_dst == TG,
          "B's MS follows the call from A (state %d, heard %u -> %u)", b0.state, (unsigned)b0.heard_src,
          (unsigned)b0.heard_dst);
    CHECK(rdof_t3unit_counts(Bs.u)->net_in > 0 && rdof_t3unit_rf(Bs.u)->ch[1].net_bursts > 0, "B's payload channel carried the network's bursts (%u, %u)",
          rdof_t3unit_counts(Bs.u)->net_in, rdof_t3unit_rf(Bs.u)->ch[1].net_bursts);
    run(8000);
    CHECK(ms_status(&A, 0).calls_done == 1, "the call ended");
    CHECK(ms_status(&Bs, 0).state == RDOF_T3MS_IDLE, "B's MS back on the TSCC (%d)", ms_status(&Bs, 0).state);

    /* an individual call from A's MS to B's, through the setup */
    const int routed = B.pc_routed;
    rdof_t3ms_do(A.ms[1], RDOF_T3MS_DO_PRIVATE, 3100101, 2000, 0, T0 + K * 30);
    run(4000);
    CHECK(B.pc_routed > routed, "the setup went through the router");
    CHECK(rdof_t3unit_counts(A.u)->setups_out > 0 && rdof_t3unit_counts(Bs.u)->setups_in > 0 && rdof_t3unit_counts(Bs.u)->setups_out > 0 && rdof_t3unit_counts(A.u)->setups_in > 0,
          "setups: A out %u, B in %u, B out %u, A in %u", rdof_t3unit_counts(A.u)->setups_out, rdof_t3unit_counts(Bs.u)->setups_in, rdof_t3unit_counts(Bs.u)->setups_out,
          rdof_t3unit_counts(A.u)->setups_in);
    rdof_t3ms_status_t called = ms_status(&Bs, 0);
    CHECK(called.ahoys >= 1, "B's MS was asked (ahoys %u)", called.ahoys);
    CHECK(ms_status(&A, 1).grants >= 1, "A's caller granted (%u)", ms_status(&A, 1).grants);
    run(10000);
    CHECK(ms_status(&A, 1).calls_done == 1, "the individual call done (failed %u)", ms_status(&A, 1).calls_failed);

    /* short data across the network (tier3.md 5.9): A's MS sends UDT short data and a status to B's, each over the
       air on both sites (upload, download as rate 1/2 bursts) and mqtt-net's sdata; the caller gets B's MS's
       C_ACKU mirrored */
    static const uint8_t text[] = "Short data from site A to site B";
    CHECK(rdof_t3ms_sdm(A.ms[1], 3100101, 0, 4, text, sizeof text - 1, T0 + K * 30), "short data queued");
    run(6000);
    rdof_t3ms_status_t sd = ms_status(&A, 1);
    CHECK(sd.calls_done == 2 && sd.last_reason == 0x44, "short data delivered (done %u, failed %u, reason 0x%02x)",
          sd.calls_done, sd.calls_failed, sd.last_reason);
    rdof_t3ms_do(A.ms[1], RDOF_T3MS_DO_STATUS, 3100101, 42, 0, T0 + K * 30);
    run(5000);
    sd = ms_status(&A, 1);
    CHECK(sd.calls_done == 3 && sd.last_reason == 0x44, "status delivered (done %u, failed %u, reason 0x%02x)",
          sd.calls_done, sd.calls_failed, sd.last_reason);

    /* packet data (tier3.md 3.16) over the burst path: A's MS to A's other MS (confirmed: the called MS's ACK
       comes back through the site's repeat), then to the site itself, whose IP gateway takes the datagram and
       answers the confirmed message with an ACK the site sends down on the data channel */
    static const uint8_t pd[] = "Packet data from one MS to another, 52 octets long..";
    CHECK(rdof_t3ms_data(A.ms[1], 3100001, RDOF_T3MS_F_CONFIRMED, pd, sizeof pd - 1, T0 + K * 30), "data queued");
    run(6000);
    uint8_t got[160];
    uint32_t from = 0;
    int n = rdof_t3ms_data_inbox(A.ms[0], got, sizeof got, &from);
    CHECK(n == (int)sizeof pd - 1 && from == 3100002 && !memcmp(got, pd, sizeof pd - 1), "MS to MS data (%d from %u)",
          n, (unsigned)from);
    sd = ms_status(&A, 1);
    CHECK(sd.calls_done == 4 && sd.state == RDOF_T3MS_IDLE, "confirmed data acknowledged (done %u, failed %u)",
          sd.calls_done, sd.calls_failed);
    run(4000);

    rdof_ipgw_config_t gc;
    rdof_ipgw_config_default(&gc);
    gc.on = true;
    gc.nat = true;
    GW.g = calloc(1, sizeof *GW.g);
    rdof_ipgw_init(GW.g, &gc);
    GW.g->send_dmr = gw_send_dmr;
    GW.g->open = gw_open;
    GW.g->send_udp = gw_send_udp;
    GW.g->ctx = &A;
    GW.g->sync48 = 0xDFF57D75DF5DULL; /* BS data */
    GW.site = &A;
    uint8_t dg[64], hello[] = "datagram up";
    const size_t dlen = rdof_ipgw_udp_datagram(rdof_ipgw_radio_ip(&gc.radio_net, 3100001), 4000, 0x09090909, 53, 1,
                                               hello, sizeof hello - 1, dg, sizeof dg);
    CHECK(dlen == 28 + sizeof hello - 1, "datagram built");
    const uint32_t done0 = ms_status(&A, 0).calls_done;
    CHECK(rdof_t3ms_data(A.ms[0], 0xFFFECA, RDOF_T3MS_F_CONFIRMED, dg, dlen, T0 + K * 30), "data to the site queued");
    run(6000);
    CHECK(GW.g->n.rx_messages == 1 && GW.udp == 1 && GW.g->n.acks == 1 && GW.sent == 1,
          "the IP gateway took it (bursts %d, messages %u, bad %u, udp %d, acks %u)", GW.bursts,
          GW.g->n.rx_messages, GW.g->n.rx_bad, GW.udp, GW.g->n.acks);
    rdof_t3ms_status_t pd0 = ms_status(&A, 0);
    CHECK(pd0.calls_done == done0 + 1 && pd0.state == RDOF_T3MS_IDLE,
          "the site's ACK reached the MS (done %u, failed %u)", pd0.calls_done, pd0.calls_failed);
    run(4000);
    GW.site = NULL;
    free(GW.g);
    GW.g = NULL;

    /* provisioning: cfg/net's hang time reaches both sites */
    const char *net = "{\"rev\":2,\"at\":\"2026-11-06T10:00:00Z\",\"timers\":{\"hang_s\":5}}";
    broker_publish(2, "DMR/cfg/net", net, strlen(net), true);
    broker_deliver();
    CHECK(rdof_t3unit_cfg(A.u)->hang_ms == 5000 && rdof_t3unit_cfg(Bs.u)->hang_ms == 5000, "hang from cfg/net: %u %u", rdof_t3unit_cfg(A.u)->hang_ms,
          rdof_t3unit_cfg(Bs.u)->hang_ms);

    /* cfg/tg: talkgroup 9 hangs 8 s (the network's 5 s for the rest); a call's channel held about talk + 8 s */
    const char *tgdoc = "{\"rev\":1,\"at\":\"2026-11-06T10:00:00Z\",\"name\":\"Ops\",\"hang_s\":8,\"priority\":2}";
    broker_publish(2, "DMR/cfg/tg/9", tgdoc, strlen(tgdoc), true);
    broker_deliver();
    const uint32_t *tgids = NULL;
    CHECK(rdof_t3unit_tg_policies(A.u, &tgids) == 1 && tgids[0] == TG && rdof_t3unit_tg_policies(Bs.u, NULL) == 1, "cfg/tg applied: %d %d", rdof_t3unit_tg_policies(A.u, NULL), rdof_t3unit_tg_policies(Bs.u, NULL));
    int held = hang_ms(1500);
    CHECK(held > 9000 && held < 11000, "tg 9's channel held %d ms (1.5 s talk, 8 s hang)", held);
    broker_publish(2, "DMR/cfg/tg/9", "", 0, true); /* deleted: the network's again */
    broker_deliver();
    CHECK(rdof_t3unit_tg_policies(A.u, NULL) == 0, "cfg/tg deleted");
    held = hang_ms(1500);
    CHECK(held > 6000 && held < 8000, "then held %d ms (1.5 s talk, 5 s hang)", held);

    /* neighbors: each site's info names the other as an adjacent site; online 0 makes it inactive; cfg/node's
       neighbors list leaves out the ones it doesn't name */
    char tj[1024], info[1200];
    rdof_t3unit_trunk_json(Bs.u, tj, sizeof tj);
    snprintf(info, sizeof info, "{\"schema\":1,\"trunk\":%s}", tj);
    broker_publish(1, "DMR/node/site-b/info", info, strlen(info), true);
    broker_publish(1, "DMR/node/site-b/online", "1", 1, true);
    rdof_t3unit_trunk_json(A.u, tj, sizeof tj);
    snprintf(info, sizeof info, "{\"schema\":1,\"trunk\":%s}", tj);
    broker_publish(0, "DMR/node/site-a/info", info, strlen(info), true);
    broker_deliver();
    CHECK(rdof_t3unit_neighbors(A.u) == 1 && rdof_t3unit_neighbors(Bs.u) == 1, "neighbors: A %d, B %d", rdof_t3unit_neighbors(A.u), rdof_t3unit_neighbors(Bs.u));
    int npeers = 0;
    const rdof_t3unit_peer_t *pb = rdof_t3unit_peers(A.u, &npeers);
    CHECK(npeers >= 1 && !strcmp(pb->node, "site-b") && pb->site == 4 && pb->chan == 1 && pb->online &&
              pb->syscode == rdof_t3_syscode(rdof_t3unit_cfg(A.u)->model, rdof_t3unit_cfg(A.u)->net, 4, rdof_t3unit_cfg(A.u)->par),
          "A's neighbor: %s site %u chan %u syscode 0x%04x", pb->node, pb->site, pb->chan, pb->syscode);
    broker_publish(1, "DMR/node/site-b/online", "0", 1, true);
    broker_deliver();
    CHECK(!rdof_t3unit_peers(A.u, NULL)[0].online && rdof_t3unit_neighbors(A.u) == 1, "B offline: still listed, inactive");
    const char *nodedoc = "{\"rev\":1,\"at\":\"2026-11-06T10:00:00Z\",\"neighbors\":[7]}";
    broker_publish(2, "DMR/cfg/node/site-a", nodedoc, strlen(nodedoc), true);
    broker_deliver();
    CHECK(rdof_t3unit_neighbors(A.u) == 0 && rdof_t3unit_neighbors(Bs.u) == 1, "cfg/node's neighbors [7]: A %d, B %d", rdof_t3unit_neighbors(A.u),
          rdof_t3unit_neighbors(Bs.u));
    broker_publish(2, "DMR/cfg/node/site-a", "", 0, true);
    broker_deliver();
    CHECK(rdof_t3unit_neighbors(A.u) == 1, "the list from info again (%d)", rdof_t3unit_neighbors(A.u));

    /* commands: status, and deregister (the record goes) */
    const char *cmd = "{\"id\":\"c-1\",\"cmd\":\"deregister\",\"ms\":3100002}";
    broker_publish(2, "DMR/node/site-a/cmd", cmd, strlen(cmd), false);
    broker_deliver();
    CHECK(retained("DMR/node/site-a/ms/3100002") == NULL, "the deregistered MS's record deleted");
    rdof_t3_record_t rec;
    CHECK(!rdof_t3_record(A.mem, 3100002, &rec), "and gone from the controller");

    /* a restart of A: the records come back from the broker */
    site_close(&A);
    site_open(&A, 0, "site-a", 3, 3100001, T0 + K * 30);
    broker_deliver();
    CHECK(rdof_t3_record(A.mem, 3100001, &rec) && rec.sub_tg == TG, "A read its record back after a restart");
    CHECK(rdof_t3unit_cfg(A.u)->hang_ms == 5000, "and the retained provisioning (%u)", rdof_t3unit_cfg(A.u)->hang_ms);


    /* an external MS on A, through the hooks: a forced collision first (its C_RAND twice), then it registers */
    {
        const uint32_t tg[1] = {TG};
        const uint16_t chans[1] = {1};
        X.ms = malloc(rdof_t3ms_size());
        rdof_t3ms_init(X.ms, 3100009, tg, 1, chans, 1, 99);
        rdof_t3ms_do(X.ms, RDOF_T3MS_DO_REGISTER, TG, 0, 0, T0 + K * 30);
        A.sim.ext_in = ext_in;
        A.sim.ext_out = ext_out;
        X.dup = 1;
        const uint32_t coll = A.sim.collisions;
        run(1500);
        rdof_t3ms_status_t xs;
        rdof_t3ms_status(X.ms, &xs);
        CHECK(A.sim.collisions > coll && !xs.registered && X.fates[RDOF_T3SIM_COLLIDED] >= 2,
              "the doubled request collided (%u, fates %u)", A.sim.collisions - coll, X.fates[RDOF_T3SIM_COLLIDED]);
        X.dup = 0;
        run(8000);
        rdof_t3ms_status(X.ms, &xs);
        CHECK(xs.registered && rdof_t3_record(A.mem, 3100009, &rec), "the external MS registered");
        CHECK(X.cach_ok > 100 && X.cach_bad == 0, "its CACH: %u read, %u wrong", X.cach_ok, X.cach_bad);
        /* a group call from it: A's own MS follows it */
        rdof_t3ms_do(X.ms, RDOF_T3MS_DO_GROUP, TG, 1500, 0, T0 + K * 30);
        int waited = 0;
        while (ms_status(&A, 0).heard_src != 3100009 && waited < 4000)
            run(30), waited += 30;
        CHECK(ms_status(&A, 0).heard_src == 3100009, "A's MS heard the external MS's call (%u)", ms_status(&A, 0).heard_src);
        run(8000);
        rdof_t3ms_status(X.ms, &xs);
        CHECK(xs.calls_done == 1 && xs.state == RDOF_T3MS_IDLE, "its call ended (%u, state %d)", xs.calls_done, xs.state);
        /* a burst on the slot not on air is dropped */
        const uint32_t drop = A.sim.ext_drop;
        X.stray = 1;
        run(300);
        X.stray = 0;
        CHECK(A.sim.ext_drop >= drop + 10 && X.fates[RDOF_T3SIM_DROPPED] == A.sim.ext_drop - drop,
              "stray bursts dropped (%u)", A.sim.ext_drop - drop);
        CHECK(X.fates[RDOF_T3SIM_HEARD] > 5, "its bursts heard (%u)", X.fates[RDOF_T3SIM_HEARD]);
        A.sim.ext_in = NULL;
        A.sim.ext_out = NULL;
        free(X.ms);
    }

    site_close(&A);
    site_close(&Bs);
    for (int i = 0; i < B.nret; i++)
        free(B.retained[i].data);
    if (fails)
        printf("rdof_t3unit_tests: %d failed\n", fails);
    else
        printf("rdof_t3unit_tests: all passed\n");
    return fails != 0;
}
