// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* Two trunked sites (rdof/trunkunit.h) with simulated MSs over the full burst path (rdof/trunksim.h), joined by an
 * in-process broker (retained messages, + wildcards, no local echo) with a stand-in router (pc/{dst} to dl/{the node
 * whose record has dst}): registration and its records, a wide group call heard at the other site, an individual
 * call across the sites through the setup, provisioning (cfg/net), commands, and a restart that reads the records
 * back. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/trunkcfg.h"
#include "rdof/trunksim.h"
#include "rdof/trunkunit.h"

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
    rdof_trunkunit_t *unit[CLIENTS];
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
            rdof_trunkunit_message(B.unit[B.q[i].to], B.q[i].m.topic, B.q[i].m.data, B.q[i].m.len);
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
    rdof_trunkunit_t *u;
    void *mem;
    rdof_trunksim_t sim;
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
    rdof_trunk_cfg_default(&cfg, &pol);
    cfg.site = (uint16_t)site_no;
    pol.wide_tg.all = 1;
    s->u = calloc(1, sizeof *s->u);
    s->mem = aligned_alloc(8, (rdof_t3_site_size() + 7) / 8 * 8);
    const rdof_trunkunit_io_t io = {io_publish, io_subscribe, io_log, NULL, io_rand, s};
    char err[120];
    CHECK(rdof_trunkunit_init(s->u, s->mem, &cfg, &pol, &io, "DMR", node, 0, "test", err, sizeof err), "init: %s",
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
    rdof_trunksim_init(&s->sim, s->u, 2, s->ms, s->id);
    rdof_trunkunit_run(s->u, (at_ms - T0) / 30, at_ms);
    rdof_trunkunit_link(s->u, true);
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
        rdof_trunksim_slot(&A.sim, K, now);
        rdof_trunksim_slot(&Bs.sim, K, now);
        broker_deliver();
        if (K % 10 == 0) {
            rdof_trunkunit_tick(A.u, now);
            rdof_trunkunit_tick(Bs.u, now);
        }
    }
}

static rdof_t3ms_status_t ms_status(site_t *s, int i)
{
    rdof_t3ms_status_t st;
    rdof_t3ms_status(s->ms[i], &st);
    return st;
}

int main(void)
{
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

    site_close(&A);
    site_close(&Bs);
    for (int i = 0; i < B.nret; i++)
        free(B.retained[i].data);
    if (fails)
        printf("rdof_trunkunit_tests: %d failed\n", fails);
    else
        printf("rdof_trunkunit_tests: all passed\n");
    return fails != 0;
}
