// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* Two trunked sites (rdof/t3unit.h) with simulated MSs over the full burst path (rdof/t3sim.h), joined by an
 * in-process broker (retained messages, + wildcards, no local echo) with a stand-in router (pc/{dst} to dl/{the node
 * whose record has dst}): registration and its records, a wide group call heard at the other site, an individual
 * call across the sites through the setup, provisioning (cfg/net), commands, and a restart that reads the records
 * back. */
#include <stdalign.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void site_open(site_t *s, int client, const char *node, int site_no, uint32_t id0, int64_t at_ms)
{
    memset(s, 0, sizeof *s);
    s->client = client;
    rdof_t3_cfg_t cfg;
    rdof_t3_policy_t pol;
    rdof_t3cfg_default(&cfg, &pol);
    cfg.site = (uint16_t)site_no;
    pol.wide_tg.all = 1;
    s->u = calloc(1, sizeof *s->u);
    s->mem = aligned_alloc(8, (rdof_t3_site_size() + 7) / 8 * 8);
    const rdof_t3unit_io_t io = {io_publish, io_subscribe, io_log, NULL, io_rand, s};
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
        if (rdof_t3sim_ms_burst(&tx, vn, A.u->cfg.cc, &in[n].b))
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

/* rdof/t3sim.h's Rust (rust/src/t3sim.rs) mirrors its structs: the same layouts. */
const rdof_t3_layout_t *rdof_t3sim_layout(size_t i);

static void test_sim_layouts(void)
{
#define LAYOUT(t) {#t, sizeof(t), alignof(t)}
    static const rdof_t3_layout_t c[] = {LAYOUT(rdof_t3sim_in_t), LAYOUT(rdof_t3sim_out_t), LAYOUT(rdof_t3sim_t)};
#undef LAYOUT
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

int main(void)
{
    test_sim_layouts();
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
    CHECK(Bs.u->net_in > 0 && Bs.u->rf.ch[1].net_bursts > 0, "B's payload channel carried the network's bursts (%u, %u)",
          Bs.u->net_in, Bs.u->rf.ch[1].net_bursts);
    run(8000);
    CHECK(ms_status(&A, 0).calls_done == 1, "the call ended");
    CHECK(ms_status(&Bs, 0).state == RDOF_T3MS_IDLE, "B's MS back on the TSCC (%d)", ms_status(&Bs, 0).state);

    /* an individual call from A's MS to B's, through the setup */
    const int routed = B.pc_routed;
    rdof_t3ms_do(A.ms[1], RDOF_T3MS_DO_PRIVATE, 3100101, 2000, 0, T0 + K * 30);
    run(4000);
    CHECK(B.pc_routed > routed, "the setup went through the router");
    CHECK(A.u->setups_out > 0 && Bs.u->setups_in > 0 && Bs.u->setups_out > 0 && A.u->setups_in > 0,
          "setups: A out %u, B in %u, B out %u, A in %u", A.u->setups_out, Bs.u->setups_in, Bs.u->setups_out,
          A.u->setups_in);
    rdof_t3ms_status_t called = ms_status(&Bs, 0);
    CHECK(called.ahoys >= 1, "B's MS was asked (ahoys %u)", called.ahoys);
    CHECK(ms_status(&A, 1).grants >= 1, "A's caller granted (%u)", ms_status(&A, 1).grants);
    run(10000);
    CHECK(ms_status(&A, 1).calls_done == 1, "the individual call done (failed %u)", ms_status(&A, 1).calls_failed);

    /* provisioning: cfg/net's hang time reaches both sites */
    const char *net = "{\"rev\":2,\"at\":\"2026-11-06T10:00:00Z\",\"timers\":{\"hang_s\":5}}";
    broker_publish(2, "DMR/cfg/net", net, strlen(net), true);
    broker_deliver();
    CHECK(A.u->cfg.hang_ms == 5000 && Bs.u->cfg.hang_ms == 5000, "hang from cfg/net: %u %u", A.u->cfg.hang_ms,
          Bs.u->cfg.hang_ms);

    /* cfg/tg: talkgroup 9 hangs 8 s (the network's 5 s for the rest); a call's channel held about talk + 8 s */
    const char *tgdoc = "{\"rev\":1,\"at\":\"2026-11-06T10:00:00Z\",\"name\":\"Ops\",\"hang_s\":8,\"priority\":2}";
    broker_publish(2, "DMR/cfg/tg/9", tgdoc, strlen(tgdoc), true);
    broker_deliver();
    CHECK(A.u->ntgp == 1 && A.u->tg_ids[0] == TG && Bs.u->ntgp == 1, "cfg/tg applied: %d %d", A.u->ntgp, Bs.u->ntgp);
    int held = hang_ms(1500);
    CHECK(held > 9000 && held < 11000, "tg 9's channel held %d ms (1.5 s talk, 8 s hang)", held);
    broker_publish(2, "DMR/cfg/tg/9", "", 0, true); /* deleted: the network's again */
    broker_deliver();
    CHECK(A.u->ntgp == 0, "cfg/tg deleted");
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
    CHECK(A.u->nneighbors == 1 && Bs.u->nneighbors == 1, "neighbors: A %d, B %d", A.u->nneighbors, Bs.u->nneighbors);
    const rdof_t3unit_peer_t *pb = &A.u->peers[0];
    CHECK(A.u->npeers >= 1 && !strcmp(pb->node, "site-b") && pb->site == 4 && pb->chan == 1 && pb->online &&
              pb->syscode == rdof_t3_syscode(A.u->cfg.model, A.u->cfg.net, 4, A.u->cfg.par),
          "A's neighbor: %s site %u chan %u syscode 0x%04x", pb->node, pb->site, pb->chan, pb->syscode);
    broker_publish(1, "DMR/node/site-b/online", "0", 1, true);
    broker_deliver();
    CHECK(!A.u->peers[0].online && A.u->nneighbors == 1, "B offline: still listed, inactive");
    const char *nodedoc = "{\"rev\":1,\"at\":\"2026-11-06T10:00:00Z\",\"neighbors\":[7]}";
    broker_publish(2, "DMR/cfg/node/site-a", nodedoc, strlen(nodedoc), true);
    broker_deliver();
    CHECK(A.u->nneighbors == 0 && Bs.u->nneighbors == 1, "cfg/node's neighbors [7]: A %d, B %d", A.u->nneighbors,
          Bs.u->nneighbors);
    broker_publish(2, "DMR/cfg/node/site-a", "", 0, true);
    broker_deliver();
    CHECK(A.u->nneighbors == 1, "the list from info again (%d)", A.u->nneighbors);

    /* commands: status, and deregister (the record goes) */
    const char *cmd = "{\"id\":\"c-1\",\"cmd\":\"deregister\",\"ms\":3100002}";
    broker_publish(2, "DMR/node/site-a/cmd", cmd, strlen(cmd), false);
    broker_deliver();
    CHECK(retained("DMR/node/site-a/ms/3100002") == NULL, "the deregistered MS's record deleted");
    rdof_t3_record_t rec;
    CHECK(!rdof_t3_record(A.u->site, 3100002, &rec), "and gone from the controller");

    /* a restart of A: the records come back from the broker */
    site_close(&A);
    site_open(&A, 0, "site-a", 3, 3100001, T0 + K * 30);
    broker_deliver();
    CHECK(rdof_t3_record(A.u->site, 3100001, &rec) && rec.sub_tg == TG, "A read its record back after a restart");
    CHECK(A.u->cfg.hang_ms == 5000, "and the retained provisioning (%u)", A.u->cfg.hang_ms);


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
        CHECK(xs.registered && rdof_t3_record(A.u->site, 3100009, &rec), "the external MS registered");
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
