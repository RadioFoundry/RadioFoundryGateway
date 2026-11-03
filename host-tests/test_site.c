// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The settings, version 3 (rdof/site.h) and ID sets (rdof/idset.h): every fixture in fixtures/config/v3 is taken or
 * refused as its name says; the spec's four sites resolve to the paths and policies it describes; each good one's
 * units and SDR parse. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/config.h"
#include "rdof/hblink.h"
#include "rdof/idset.h"
#include "rdof/layer2.h"
#include "rdof/mmdvm.h"
#include "rdof/pdu.h"
#include "rdof/repeater.h"
#include "rdof/router.h"
#include "rdof/sdr/config.h"
#include "rdof/site.h"
#include "rdof/siteroute.h"
#include "rdof/txpool.h"
#include "rdof/txvr.h"

static int fails, checks;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(c)) {                                                                                                    \
            fails++;                                                                                                   \
            printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #c);                                                        \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *t = malloc((size_t)n + 1);
    *len = fread(t, 1, (size_t)n, f);
    t[*len] = 0;
    fclose(f);
    return t;
}

static rdof_idset_t set_of(const char *items)
{
    rdof_idset_t s;
    char err[96];
    rdof_idset_clear(&s);
    if (!rdof_idset_add(&s, items, err, sizeof err))
        printf("(%s)\n", err);
    rdof_idset_finish(&s);
    return s;
}

static void test_idset(void)
{
    char t[128], err[96];
    rdof_idset_t s = set_of("*,!9,!4000-4099");
    CHECK(rdof_idset_has(&s, 1) && rdof_idset_has(&s, 3100) && !rdof_idset_has(&s, 9), "star less 9");
    CHECK(!rdof_idset_has(&s, 4000) && !rdof_idset_has(&s, 4099) && rdof_idset_has(&s, 4100), "less a range");
    CHECK(!rdof_idset_listed(&s, 3100), "* isn't listed");
    rdof_idset_format(&s, t, sizeof t);
    CHECK(!strcmp(t, "*,!9,!4000-4099"), "%s", t);

    s = set_of("!350");
    CHECK(s.star && rdof_idset_has(&s, 349) && !rdof_idset_has(&s, 350), "only ! starts from *");

    s = set_of("202, 300-399,!350");
    CHECK(rdof_idset_has(&s, 202) && rdof_idset_listed(&s, 202) && rdof_idset_has(&s, 399), "plain");
    CHECK(!rdof_idset_has(&s, 350) && !rdof_idset_has(&s, 201), "plain less");
    rdof_idset_format(&s, t, sizeof t);
    CHECK(!strcmp(t, "202,300-349,351-399"), "%s", t);
    rdof_idrange_t r[4];
    CHECK(rdof_idset_ranges(&s, r, 4) == 3 && r[1].lo == 300 && r[1].hi == 349 && r[2].lo == 351, "ranges");

    s = set_of("100-199,150-250,251");
    rdof_idset_format(&s, t, sizeof t);
    CHECK(!strcmp(t, "100-251"), "merged: %s", t);

    s = set_of("*,3100");
    CHECK(rdof_idset_listed(&s, 3100) && !rdof_idset_listed(&s, 3101) && rdof_idset_has(&s, 3101), "* and listed");
    rdof_idset_format(&s, t, sizeof t);
    CHECK(!strcmp(t, "*,3100"), "%s", t);

    s = set_of("9990");
    CHECK(rdof_idset_single(&s) == 9990, "single");
    s = set_of("9990-9991");
    CHECK(rdof_idset_single(&s) == 0, "not single");
    rdof_idset_clear(&s);
    CHECK(rdof_idset_empty(&s) && !rdof_idset_has(&s, 1), "empty");

    static const char *const bad[] = {"0", "16777216", "5-3", "!*", "x", "1-", "-1", ",", NULL};
    for (int k = 0; bad[k]; k++) {
        rdof_idset_clear(&s);
        CHECK(!rdof_idset_add(&s, bad[k], err, sizeof err), "refused: %s", bad[k]);
    }
}

static rdof_site_t S;

static const rdof_path_t *path(const char *name)
{
    int k = rdof_site_path(&S, name);
    return k >= 0 ? &S.path[k] : NULL;
}

static bool load(const char *dir, const char *file)
{
    char p[512], err[200];
    size_t n;
    snprintf(p, sizeof p, "%s/v3/%s", dir, file);
    char *t = slurp(p, &n);
    bool ok = t && rdof_site_parse(t, n, &S, err, sizeof err);
    CHECK(ok, "%s: %s", file, t ? err : "can't read");
    free(t);
    return ok;
}

/* The spec's section 10 sites. */
static void test_scenarios(const char *dir)
{
    char t[128];
    const rdof_path_t *p;
    if (load(dir, "good-commercial.json")) {
        CHECK(S.npath == 16 && S.napp == 2 && S.nnet == 1, "16 paths");
        p = path("438.0875");
        CHECK(p && rdof_idset_single(&p->pol.tg) == 202 && p->pol.dir == RDOF_FLOW_BOTH, "TG 202 both ways");
        CHECK(p && rdof_idset_has(&p->pol.pc, 1234567) && p->pol.apps == 3 && p->pol.net == 0, "pc, apps, net");
        CHECK(p && p->slot == 0 && p->tx_slot == 1 && p->tx_freq >= 0 && p->peer_id == 9800100, "simplex path");
        CHECK(p && p->pol.src[RDOF_PK_TG] == RDOF_SRC_OWN && p->pol.src[RDOF_PK_PC] == RDOF_SRC_TABLE, "sources");
        CHECK(S.ndev == 1 && !strcmp(S.dev[0].name, "sdr0") && S.dev[0].can_tx && S.dev[0].transmits == 0xffff,
              "the SDR transmits on all 16");
        CHECK(rdof_site_freq(&S, "438.4875") == 14 && rdof_site_freq(&S, "438.48750") == 14, "by frequency");
    }
    if (load(dir, "good-two-repeaters.json")) {
        CHECK(S.npath == 4, "two repeaters, two slots each: %d", S.npath);
        p = path("R1/1");
        CHECK(p && p->slot == 1 && !rdof_idset_has(&p->pol.tg, 9) && rdof_idset_has(&p->pol.tg, 3100), "open");
        CHECK(p && !rdof_idset_has(&p->pol.tg, 4050) && p->pol.apps == 0, "open: deny list, no apps");
        CHECK(p && p->pol.src[RDOF_PK_TG] == 0 && p->pol.src[RDOF_PK_APPS] == 0, "from policy open");
        p = path("R2/2");
        CHECK(p && rdof_idset_has(&p->pol.tg, 3120) && !rdof_idset_has(&p->pol.tg, 3121), "closed");
        CHECK(p && p->pol.apps == (1u << 2 | 1u << 3) && rdof_idset_has(&p->pol.pc, 9800999), "closed: apps, pc");
        CHECK(S.dev[0].hears == RDOF_HEARS_IDLE && !S.dev[0].can_tx && S.dev[1].hears == RDOF_HEARS_NONE,
              "split halves");
        CHECK(rdof_site_path_rx(&S, 438750000, 2) == rdof_site_path(&S, "R1/2"), "heard on R1's input, slot 2");
    }
    if (load(dir, "good-sdr-repeater-forwarders.json")) {
        CHECK(S.npath == 6, "a repeater's two and four forwarders: %d", S.npath);
        p = path("fwd C");
        CHECK(p && p->tx_freq < 0 && p->pol.dir == RDOF_FLOW_RF_NET && p->pol.net == 1 && p->pol.tg.star,
              "receive only, to mqtt, every TG");
        p = path("R1/2");
        CHECK(p && rdof_idset_has(&p->pol.ptt, 3250) && p->pol.ptt_hold_s == 600 && p->pol.net == 0, "ptt range");
        CHECK(S.dev[0].transmits == 1u << 1, "the SDR transmits only on rpt out: %llx",
              (unsigned long long)S.dev[0].transmits);
        CHECK(S.net[1].calls && S.net[1].report, "mqtt carries calls");
    }
    if (load(dir, "good-monitor.json")) {
        CHECK(S.npath == 8, "8 paths");
        p = path("yard 2");
        CHECK(p && S.freq[p->freq].cc == -1 && p->tx_freq < 0 && p->pol.dir == RDOF_FLOW_RF_NET, "any cc, rx only");
        CHECK(p && p->pol.tg.star && p->pol.src[RDOF_PK_TG] == RDOF_SRC_DEFAULTS, "every TG, from defaults");
        CHECK(!S.dev[0].can_tx, "an RTL-SDR doesn't transmit");
    }
    if (load(dir, "good-dcdm-per-path.json")) {
        p = path("dc/2");
        CHECK(p && p->slot == 2 && p->tx_slot == 2 && rdof_idset_empty(&p->pol.pc), "dcdm slot 2, no pc");
        CHECK(path("dc/1") && path("dc/1")->peer_id == 9800501 && p && p->peer_id == 9800501, "per_path: %u %u",
              path("dc/1") ? path("dc/1")->peer_id : 0, p ? p->peer_id : 0);
        p = path("mon/2");
        CHECK(p && p->peer_id == 9800599 && rdof_idset_single(&p->pol.tg) == 3120, "peer_ids, rx-only slots");
        CHECK(S.dev[1].transmits == 1, "pinned");
    }
    /* the resolved view */
    if (load(dir, "good-two-repeaters.json")) {
        static char out[16384];
        size_t n = rdof_site_resolved(&S, out, sizeof out);
        CHECK(n > 100 && n < sizeof out - 1, "resolved: %zu", n);
        CHECK(strstr(out, "\"name\":\"R1/1\"") && strstr(out, "\"tg\":\"*,!9,!4000-4099\""), "%.300s", out);
        CHECK(strstr(out, "\"tg\":\"3100,3120,3177\",\"path\":\"R1/2\",\"how\":\"static\""), "reverse view");
        CHECK(strstr(out, "\"tg\":\"policy open\""), "sources");
        snprintf(t, sizeof t, "%zu", n);
        /* indented: the same JSON, a line per top key and per list entry */
        static char txt[16384], back[16384];
        size_t m = rdof_site_resolved_text(&S, txt, sizeof txt), k = 0;
        CHECK(m > n && !strncmp(txt, "{\n  \"version\": 3,\n", 18), "%.60s", txt);
        CHECK(strstr(txt, "\n    {\"tg\": \"3100,3120,3177\", \"path\": \"R1/2\", \"how\": \"static\"}"), "a line an entry");
        bool str = false;
        for (const char *c = txt; *c; c++) { /* less the added whitespace, as it was */
            if (*c == '"' && (c == txt || c[-1] != '\\'))
                str = !str;
            if (str || (*c != ' ' && *c != '\n'))
                back[k++] = *c;
        }
        back[k] = 0;
        CHECK(!strcmp(back, out), "indented reads back the same");
        CHECK(rdof_site_resolved_text(&S, txt, 40) == 39 && strlen(txt) == 39, "cut at cap");
    }
}

/* The router (rdof/router.h) on the spec's sites. */
static void test_router(const char *dir)
{
    static rdof_router_t r;
    rdof_route_t rt;
    uint64_t pre;
    int k;
#define CALL(s, d, g) (&(rdof_rcall_t){.src = (s), .dst = (d), .group = (g)})
    if (load(dir, "good-commercial.json")) {
        rdof_router_init(&r, &S);
        k = rdof_site_path(&S, "438.0875");
        rt = rdof_router_rf(&r, k, CALL(9800001, 202, true), 0);
        CHECK(rt.kind == RDOF_ROUTE_NET && rt.net == 0, "TG 202 up");
        rt = rdof_router_rf(&r, k, CALL(9800001, 203, true), 1);
        CHECK(rt.kind == RDOF_ROUTE_NONE && r.not_routed == 1, "TG 203 isn't this path's");
        rt = rdof_router_rf(&r, k, CALL(9800001, 9990, true), 2);
        CHECK(rt.kind == RDOF_ROUTE_APP && rt.app == rdof_site_app(&S, "parrot"), "the parrot from any path");
        rt = rdof_router_rf(&r, k, CALL(9800001, 9800002, false), 3);
        CHECK(rt.kind == RDOF_ROUTE_NET, "private calls up");
        /* down: TG 203 to its path only */
        uint64_t m = rdof_router_net(&r, 0, ~0ull, 0x111, CALL(9800009, 203, true), false, 4, &pre);
        CHECK(m == 1ull << rdof_site_path(&S, "438.1125"), "TG 203 down: %llx", (unsigned long long)m);
        CHECK(rdof_router_net(&r, 0, ~0ull, 0x111, CALL(9800009, 203, true), false, 4.06, NULL) == m, "kept");
        CHECK(rdof_router_net(&r, 0, ~0ull, 0x111, CALL(9800009, 203, true), true, 4.12, NULL) == m, "end");
        CHECK(!r.busy[rdof_site_path(&S, "438.1125")].stream, "freed");
        /* a private call down: where the radio was last heard */
        m = rdof_router_net(&r, 0, ~0ull, 0x222, CALL(9800009, 9800001, false), false, 5, &pre);
        CHECK(m == 1ull << k, "pc down to where 9800001 was heard: %llx", (unsigned long long)m);
        m = rdof_router_net(&r, 0, ~0ull, 0x333, CALL(9800009, 9800077, false), false, 5, &pre);
        CHECK(m == 0 && r.pc_dropped == 1, "never heard: dropped");
        m = rdof_router_net(&r, 0, ~0ull, 0x444, CALL(9800009, 9800001, false), false, 2000, &pre);
        CHECK(m == 0 && r.pc_dropped == 2, "heard too long ago");
        CHECK(rdof_router_net(&r, 0, ~0ull, 0x444, CALL(9800009, 9800001, false), false, 2000.06, &pre) == 0 &&
              r.pc_dropped == 2, "counted once a stream");
    }
    if (load(dir, "good-two-repeaters.json")) {
        rdof_router_init(&r, &S);
        int r11 = rdof_site_path(&S, "R1/1"), r12 = rdof_site_path(&S, "R1/2"), r21 = rdof_site_path(&S, "R2/1"),
            r22 = rdof_site_path(&S, "R2/2");
        rt = rdof_router_rf(&r, r11, CALL(1, 9990, true), 0);
        CHECK(rt.kind == RDOF_ROUTE_NET, "TS1: 9990 is a TG (apps false)");
        rt = rdof_router_rf(&r, r11, CALL(1, 9, true), 0);
        CHECK(rt.kind == RDOF_ROUTE_NONE, "TS1: 9 denied");
        rt = rdof_router_rf(&r, r12, CALL(1, 9990, true), 0);
        CHECK(rt.kind == RDOF_ROUTE_APP, "TS2: the parrot");
        rt = rdof_router_rf(&r, r12, CALL(1, 3121, true), 0);
        CHECK(rt.kind == RDOF_ROUTE_NONE, "TS2: not listed");
        /* TG 3100 down: all four slots; the listed slots beat "*" */
        uint64_t m = rdof_router_net(&r, 0, ~0ull, 0x10, CALL(7, 3100, true), false, 10, &pre);
        CHECK(m == (1ull << r11 | 1ull << r12 | 1ull << r21 | 1ull << r22), "fan-out: %llx", (unsigned long long)m);
        m = rdof_router_net(&r, 0, ~0ull, 0x30, CALL(7, 3177, true), false, 10.6, &pre);
        CHECK(m == 0 && r.refused == 4, "equals: the one there keeps it (%u)", r.refused);
        m = rdof_router_net(&r, 0, ~0ull, 0x40, &(rdof_rcall_t){.src = 7, .dst = 555, .group = true, .emergency = true},
                            false, 10.7, &pre);
        CHECK(m == (1ull << r11 | 1ull << r21) && pre == m, "an emergency on TS1");
        m = rdof_router_net(&r, 0, ~0ull, 0x50, CALL(7, 3177, true), false, 20, &pre);
        CHECK(m == (1ull << r11 | 1ull << r12 | 1ull << r21 | 1ull << r22) && !pre, "idle again after %g s",
              RDOF_ROUTER_IDLE_S);
        rdof_router_drop(&r, r22, 0x50);
        CHECK(!r.busy[r22].stream, "dropped");
    }
    if (load(dir, "good-sdr-repeater-forwarders.json")) {
        rdof_router_init(&r, &S);
        int r12 = rdof_site_path(&S, "R1/2"), fa = rdof_site_path(&S, "fwd A");
        CHECK(!rdof_router_net(&r, 0, ~0ull, 1, CALL(7, 3250, true), false, 0, &pre), "PTT TG not held yet");
        rt = rdof_router_rf(&r, r12, CALL(1, 3250, true), 1);
        CHECK(rt.kind == RDOF_ROUTE_NET && rt.net == 0, "PTT keys it up");
        CHECK(rdof_router_net(&r, 0, ~0ull, 2, CALL(7, 3250, true), false, 2, &pre) == 1ull << r12, "now carried");
        CHECK(rdof_router_carries(&r, r12, 3250, 700) == 0, "the hold ends");
        rt = rdof_router_rf(&r, fa, CALL(1, 31337, true), 3);
        CHECK(rt.kind == RDOF_ROUTE_NET && rt.net == 1, "a forwarder: to mqtt");
        CHECK(!rdof_router_net(&r, 1, ~0ull, 3, CALL(7, 31337, true), false, 4, &pre), "a forwarder never transmits");
    }
    if (load(dir, "good-monitor.json")) {
        rdof_router_init(&r, &S);
        for (int p = 0; p < S.npath; p++) {
            rt = rdof_router_rf(&r, p, CALL(1, 100 + (uint32_t)p, p & 1), p);
            CHECK(rt.kind == RDOF_ROUTE_NET && rt.net == 0, "%s: everything to mqtt", S.path[p].name);
        }
    }
    /* a listed talkgroup beats one matched only by "*" */
    static const char star[] = "{\"version\": 3, \"defaults\": {\"cc\": 1}, \"networks\": [{\"type\": \"mqtt\", "
                               "\"host\": \"b\"}], \"frequencies\": [{\"freq\": 438.0625, \"tg\": \"*,3100\"}]}";
    char err[160];
    CHECK(rdof_site_parse(star, sizeof star - 1, &S, err, sizeof err), "%s", err);
    rdof_router_init(&r, &S);
    CHECK(rdof_router_net(&r, 0, ~0ull, 1, CALL(7, 3101, true), false, 0, &pre) == 1 && !pre, "* takes it");
    CHECK(rdof_router_net(&r, 0, ~0ull, 2, CALL(7, 3102, true), false, 0.1, &pre) == 0, "another * waits");
    CHECK(rdof_router_net(&r, 0, ~0ull, 3, CALL(7, 3100, true), false, 0.2, &pre) == 1 && pre == 1, "listed takes it over");
    CHECK(rdof_router_net(&r, 0, ~0ull, 1, CALL(7, 3101, true), false, 0.3, &pre) == 0, "the first lost it");
#undef CALL
}

/* A transceiver's answers (DMRK) */
typedef struct {
    int n;
    uint8_t sid[4];
    uint32_t freq;
} dmrk_cap_t;
static void dmrk_cap(void *ctx, const uint8_t *d, size_t n)
{
    dmrk_cap_t *c = ctx;
    if (n >= 13 && !memcmp(d, "DMRK", 4)) {
        c->n++;
        memcpy(c->sid, d + 8, 4);
        c->freq = (uint32_t)d[13] << 24 | (uint32_t)d[14] << 16 | (uint32_t)d[15] << 8 | d[16];
    }
}

/* a network burst: a voice header, or a terminator */
static rdof_net_burst_t net_burst(uint32_t sid, uint32_t src, uint32_t dst, bool group, int slot, bool emergency,
                                  bool end)
{
    rdof_net_burst_t b = {.src = src, .dst = dst, .group = group, .slot = slot, .terminator = end};
    b.stream_id[0] = (uint8_t)(sid >> 24), b.stream_id[1] = (uint8_t)(sid >> 16);
    b.stream_id[2] = (uint8_t)(sid >> 8), b.stream_id[3] = (uint8_t)sid;
    const int dt = end ? RDOF_DT_TERMINATOR_WITH_LC : RDOF_DT_VOICE_LC_HEADER;
    b.control = RDOF_SYNC_DATA | dt;
    uint8_t lc[9];
    rdof_group_lc(src, dst, group, emergency ? 0x80 : 0, lc);
    rdof_lc_burst(lc, dt, 1, RDOF_MS_VOICE ^ RDOF_DATA_XOR, b.burst);
    return b;
}

/* The transmitter pool (rdof/txpool.h, docs/spec/fw/transmitters.md §4): every rule in turn. */
static void test_txpool(void)
{
    static rdof_txpool_t p;
    rdof_txpool_init(&p, 1.5);
    CHECK(rdof_txpool_add(&p, 0x7, 0.75) == 0 && rdof_txpool_add(&p, 0x4, 0.75) == 1, "two transmitters");
    CHECK(rdof_txpool_reaches(&p, 2) && !rdof_txpool_reaches(&p, 3), "reaches");
    const rdof_txp_req_t a = {.stream = 0xA, .src = 100, .dst = 9, .group = true, .path = 0, .freq = 0};
    rdof_txp_take_t k = rdof_txpool_take(&p, &a, 10);
    CHECK(k.i == 0 && k.fresh && !k.preempt && !k.ended, "a free one: the first that reaches it");
    k = rdof_txpool_take(&p, &a, 10.06);
    CHECK(k.i == 0 && !k.fresh, "1. the same stream: the same transmitter, no retune");
    const rdof_txp_req_t b = {.stream = 0xB, .src = 101, .dst = 9, .group = true, .path = 1, .freq = 1};
    CHECK(rdof_txpool_take(&p, &b, 10.1).i < 0, "frequency 1: only transmitter 0 reaches it, and it's sending");
    const rdof_txp_req_t c = {.stream = 0xC, .src = 102, .dst = 7, .group = true, .path = 2, .freq = 2};
    CHECK(rdof_txpool_take(&p, &c, 10.1).i == 1, "3. frequency 2: the other one, free");
    /* the terminator: it holds path 0 */
    rdof_txpool_end(&p, 0, 11);
    CHECK(rdof_txpool_state(&p, 0, 11.2) == RDOF_TXP_HOLDING, "holding after the terminator");
    CHECK(rdof_txpool_take(&p, &b, 11.2).i < 0, "a hold isn't taken for another path");
    const rdof_txp_req_t reply = {.stream = 0xD, .src = 300, .dst = 100, .group = false, .path = 0, .freq = 0};
    k = rdof_txpool_take(&p, &reply, 11.3);
    CHECK(k.i == 0 && k.fresh && !k.ended, "2. a reply to the talker: the holding transmitter");
    rdof_txpool_end(&p, 0, 11.5);
    const rdof_txp_req_t same_tg = {.stream = 0xE, .src = 301, .dst = 9, .group = true, .path = 0, .freq = 0};
    rdof_txpool_end(&p, 0, 11.5);
    CHECK(rdof_txpool_take(&p, &same_tg, 11.6).i < 0, "a private reply's hold: a group call isn't its answer");
    CHECK(rdof_txpool_state(&p, 0, 12.25) == RDOF_TXP_FREE, "the hold over: free");
    k = rdof_txpool_take(&p, &b, 12.3);
    CHECK(k.i == 0 && k.fresh && !k.ended, "free again: frequency 1");
    rdof_txpool_end(&p, 0, 12.5);
    CHECK(rdof_txpool_take(&p, &same_tg, 12.6).i < 0, "another path's hold");
    const rdof_txp_req_t same_grp = {.stream = 0xF, .src = 400, .dst = 9, .group = true, .path = 1, .freq = 1};
    CHECK(rdof_txpool_take(&p, &same_grp, 12.6).i == 0, "2. the same group on the held path");
    rdof_txpool_end(&p, 0, 13);
    /* 4. a hold taken by a stream that may */
    rdof_txp_req_t data = {.stream = 0x10, .src = 500, .dst = 600, .path = 0, .freq = 0, .take_hold = true};
    k = rdof_txpool_take(&p, &data, 13.1);
    CHECK(k.i == 0 && k.ended == 0xF && k.ended_path == 1 && !k.preempt && p.held_taken == 1,
          "4. a confirmed-data answer takes another path's hold");
    /* idle: a stream with no burst for idle_s ends and holds */
    CHECK(rdof_txpool_state(&p, 0, 14.5) == RDOF_TXP_SENDING && rdof_txpool_state(&p, 0, 14.7) == RDOF_TXP_HOLDING,
          "no burst for 1.5 s: holding");
    CHECK(rdof_txpool_state(&p, 0, 15.4) == RDOF_TXP_FREE, "its hold counted from the last burst");
    /* 5. an urgent stream preempts one that isn't, never one that is */
    k = rdof_txpool_take(&p, &a, 20);
    rdof_txp_req_t emerg = {.stream = 0x11, .src = 700, .dst = 9, .group = true, .path = 1, .freq = 1, .urgent = true};
    k = rdof_txpool_take(&p, &emerg, 20.1);
    CHECK(k.i == 0 && k.preempt && k.ended == 0xA && k.ended_path == 0 && p.preempted == 1, "5. the emergency preempts");
    rdof_txp_req_t emerg2 = emerg;
    emerg2.stream = 0x12;
    emerg2.path = 0;
    emerg2.freq = 0;
    CHECK(rdof_txpool_take(&p, &emerg2, 20.2).i < 0, "an urgent stream isn't preempted");
    rdof_txpool_end(&p, 0, 21);
    CHECK(rdof_txpool_take(&p, &emerg2, 21.1).i == 0, "4. an urgent stream takes a hold");
    /* no answer_s: free at once */
    rdof_txpool_init(&p, 1.5);
    rdof_txpool_add(&p, 0, 0);
    rdof_txpool_take(&p, &a, 1);
    rdof_txpool_end(&p, 0, 2);
    CHECK(rdof_txpool_state(&p, 0, 2) == RDOF_TXP_FREE && rdof_txpool_take(&p, &b, 2).i == 0,
          "no hold: free after the terminator");
    CHECK(!strcmp(rdof_txpool_state_name(RDOF_TXP_HOLDING), "holding"), "names");
    const rdof_txp_req_t bad = {.stream = 0, .path = 0, .freq = 0};
    CHECK(rdof_txpool_take(&p, &bad, 3).i < 0, "no stream: refused");
    for (int i = 0; i < RDOF_TXPOOL_MAX; i++)
        rdof_txpool_add(&p, 0, 0);
    CHECK(rdof_txpool_add(&p, 0, 0) < 0, "full");
}

/* The router on a running box (rdof/siteroute.h): network bursts to the repeaters and transceivers it picks. */
static void test_siteroute(const char *dir)
{
    static rdof_siteroute_t sr;
    if (load(dir, "good-two-repeaters.json")) {
        static rdof_repeater_t r1, r2;
        rdof_repeater_init(&r1, NULL, S.id, 438750000, 438500000, 1);
        rdof_repeater_init(&r2, NULL, S.id, 438175000, 438550000, 1);
        rdof_siteroute_init(&sr, &S);
        CHECK(rdof_siteroute_add_repeater(&sr, rdof_site_app(&S, "R1"), &r1) &&
                  rdof_siteroute_add_repeater(&sr, rdof_site_app(&S, "R2"), &r2) &&
                  !rdof_siteroute_add_repeater(&sr, rdof_site_app(&S, "parrot"), &r1),
              "repeaters registered");
        rdof_net_burst_t b = net_burst(0x10, 7, 3100, true, 1, false, false);
        uint64_t m = rdof_siteroute_burst(&sr, 0, 0, &b, 10);
        CHECK(__builtin_popcountll(m) == 4 && r1.net_count == 2 && r2.net_count == 2, "TG 3100: both slots of both");
        CHECK(r1.net_in[0].slot + r1.net_in[1].slot == 3, "on each slot: %d %d", r1.net_in[0].slot, r1.net_in[1].slot);
        b = net_burst(0x40, 7, 555, true, 2, true, false); /* an emergency to TG 555: TS1 ("*") of each */
        m = rdof_siteroute_burst(&sr, 0, 0, &b, 10.5);
        CHECK(m == (1ull << rdof_site_path(&S, "R1/1") | 1ull << rdof_site_path(&S, "R2/1")), "emergency: TS1");
        CHECK(r1.has_preempt && !memcmp(r1.preempt, b.stream_id, 4) && r1.net_in[2].slot == 1,
              "the repeater told: slot %d", r1.net_in[2].slot);
        /* RF: TS2 reaches the parrot, TS1 doesn't (apps false) */
        const int parrot = rdof_site_app(&S, "parrot");
        CHECK(rdof_siteroute_reaches(&sr, 438750000, 2, parrot) && !rdof_siteroute_reaches(&sr, 438750000, 1, parrot),
              "the parrot: TS2 only");
        CHECK(rdof_siteroute_rf(&sr, 438750000, 1, 1, 9990, true, 11) == 0 && !sr.why, "TS1: 9990 up");
        CHECK(rdof_siteroute_rf(&sr, 438750000, 1, 1, 9, true, 11) < 0 && sr.why, "TS1: 9 not");
        CHECK(rdof_siteroute_rf(&sr, 438750000, 2, 1, 9990, true, 11) < 0 && !sr.why, "TS2: 9990 to the parrot, not up");
        CHECK(rdof_siteroute_rf(&sr, 438025000, 1, 1, 3100, true, 11) < 0, "not a path");
        const unsigned before = sr.router.to_net;
        CHECK(rdof_siteroute_rf(&sr, 438750000, 1, 1, 9990, true, 11.5) == 0 && sr.router.to_net == before,
              "asked again (the MQTT sink): the same answer, counted once");
        uint8_t d[RDOF_DMRD_LEN], burst[RDOF_BURST_BYTES] = {0};
        char t[96];
        rdof_dmrd_packet(0, 9800001, 3100, S.id, 1, true, RDOF_DT_VOICE_LC_HEADER, 0, 9, burst, 0, 0, d);
        CHECK(rdof_siteroute_calls_topic("DMR", "hilltop", d, sizeof d, t, sizeof t) && !strcmp(t, "DMR/calls/tg/3100/hilltop"),
              "%s", t);
        rdof_dmrd_packet(0, 9800001, 9800002, S.id, 2, false, RDOF_DT_VOICE_LC_HEADER, 0, 9, burst, 0, 0, d);
        CHECK(rdof_siteroute_calls_topic("DMR", "hilltop", d, sizeof d, t, sizeof t) &&
                  !strcmp(t, "DMR/calls/pc/9800002/hilltop"), "%s", t);
        CHECK(rdof_siteroute_calls_theirs("DMR/calls/tg/3100/plant", "DMR", "hilltop") &&
                  !rdof_siteroute_calls_theirs("DMR/calls/tg/3100/hilltop", "DMR", "hilltop") &&
                  !rdof_siteroute_calls_theirs("DMR/x/calls/tg/3100/a", "DMR", "hilltop") &&
                  !rdof_siteroute_calls_theirs("DMR/calls/tg/3100", "DMR", "hilltop"), "theirs");
    }
    if (load(dir, "good-commercial.json")) {
        static rdof_txvr_t t;
        static dmrk_cap_t cap;
        rdof_txvr_init(&t, NULL, S.id);
        t.send = dmrk_cap;
        t.ctx = &cap;
        rdof_siteroute_init(&sr, &S);
        CHECK(rdof_siteroute_add_txvr(&sr, &t, 0), "a transceiver");
        rdof_net_burst_t b = net_burst(0x111, 9800009, 203, true, 2, false, false);
        uint64_t m = rdof_siteroute_burst(&sr, 0, 0, &b, 1);
        CHECK(m == 1ull << rdof_site_path(&S, "438.1125"), "TG 203: its frequency");
        CHECK(cap.n == 1 && cap.freq == 438112500 && rdof_siteroute_tuned(&sr, b.stream_id),
              "tuned there (not ready: refused): %d %u", cap.n, cap.freq);
        CHECK(rdof_siteroute_burst(&sr, 0, 0, &b, 1.06) == m && cap.n == 1, "tuned once a stream");
        rdof_net_burst_t c = net_burst(0x222, 9800009, 204, true, 1, false, false);
        CHECK(rdof_siteroute_burst(&sr, 0, 0, &c, 1.1) == 0 && sr.no_txvr == 1, "one transceiver: busy");
        b = net_burst(0x111, 9800009, 203, true, 2, false, true);
        rdof_siteroute_burst(&sr, 0, 0, &b, 1.2);
        c = net_burst(0x333, 9800009, 204, true, 1, false, false);
        CHECK(rdof_siteroute_burst(&sr, 0, 0, &c, 1.3) == 1ull << rdof_site_path(&S, "438.1375") && cap.n == 2,
              "free after the terminator");
        uint8_t d[RDOF_DMRD_LEN];
        rdof_dmrd_packet(0, 9800009, 205, S.id, 1, true, RDOF_DT_VOICE_LC_HEADER, 0, 0x444, c.burst, 0, 0, d);
        CHECK(rdof_siteroute_dmrd(&sr, 0, 0, d, sizeof d, 5) && !rdof_siteroute_dmrd(&sr, 0, 0, (const uint8_t *)"DMRP", 4, 5),
              "DMRD taken, others not");
        /* an app (the parrot) answers on the path a call was heard on, through the pool */
        c = net_burst(0x333, 9800009, 204, true, 1, false, true);
        rdof_siteroute_burst(&sr, 0, 0, &c, 5.1);
        const int ap = rdof_siteroute_path_at(&sr, 438112500, 1);
        CHECK(ap == rdof_site_path(&S, "438.1125") && rdof_siteroute_path_at(&sr, 438975000, 1) < 0, "heard at: %d", ap);
        CHECK(!rdof_siteroute_app_ready(&sr, ap, 5.2), "held for an answer on another path: an app waits");
        const int n0 = cap.n;
        rdof_net_burst_t a = net_burst(0x555, 9990, 9800009, false, 1, false, false);
        CHECK(!rdof_siteroute_app_burst(&sr, ap, &a, false, 5.2) && sr.app_no_txvr == 1, "not sent while held");
        CHECK(rdof_siteroute_app_ready(&sr, ap, 9) && rdof_siteroute_app_burst(&sr, ap, &a, false, 9) && cap.n == n0 + 1 &&
                  cap.freq == 438112500 && rdof_siteroute_tuned(&sr, a.stream_id), "free: tuned to it (%d %u)", cap.n, cap.freq);
        CHECK(rdof_siteroute_app_burst(&sr, ap, &a, false, 9.06) && cap.n == n0 + 1 && !rdof_siteroute_app_ready(&sr, ap, 9.1),
              "tuned once; busy for another");
        static const uint8_t none[4] = {0, 0, 0x77, 0x77};
        CHECK(rdof_siteroute_holds(&sr, a.stream_id) == (t.has_call || t.next.used) && !rdof_siteroute_holds(&sr, none),
              "holds its stream while the transceiver does (%d), not another", t.has_call);
        c = net_burst(0x666, 9800009, 204, true, 1, false, false);
        CHECK(rdof_siteroute_burst(&sr, 0, 0, &c, 9.1) == 0, "a network stream doesn't preempt an app's");
        a = net_burst(0x555, 9990, 9800009, false, 1, false, true);
        rdof_siteroute_app_burst(&sr, ap, &a, false, 9.12);
        CHECK(rdof_siteroute_burst(&sr, 0, 0, &c, 9.2) != 0, "but takes its hold");
        CHECK(!rdof_siteroute_app_ready(&sr, -1, 11) && !rdof_siteroute_app_burst(&sr, 99, &a, false, 11), "no path");
    }
    if (load(dir, "good-dcdm-per-path.json")) { /* a DCDM frequency: its repeater, on the path's slot */
        static rdof_repeater_t d;
        rdof_repeater_init(&d, NULL, S.id, 438487500, 438487500, 1);
        rdof_siteroute_init(&sr, &S);
        CHECK(rdof_siteroute_add_repeater_freq(&sr, rdof_site_freq(&S, "dc"), &d) &&
                  !rdof_siteroute_add_repeater_freq(&sr, rdof_site_freq(&S, "mon"), &d), "DCDM repeater");
        rdof_net_burst_t b = net_burst(0x99, 7, 3105, true, 1, false, false);
        CHECK(rdof_siteroute_burst(&sr, 0, path("dc/2")->peer_id, &b, 1) == 1ull << rdof_site_path(&S, "dc/2") &&
                  d.net_count == 1 && d.net_in[0].slot == 2, "TG 3105: slot 2");
    }
    /* identity per_path: a stream goes only to the paths of the peer it came to */
    static const char pp[] = "{\"version\": 3, \"id\": 9800700, \"defaults\": {\"cc\": 1, \"tg\": 3100}, "
                             "\"devices\": [{\"kind\": \"sdr\", \"driver\": \"sim\"}], "
                             "\"frequencies\": [438.0625, 438.0875], \"networks\": [{\"type\": \"hblink\", "
                             "\"host\": \"h\", \"identity\": \"per_path\"}]}";
    char err[160];
    CHECK(rdof_site_parse(pp, sizeof pp - 1, &S, err, sizeof err), "%s", err);
    static rdof_txvr_t t;
    rdof_txvr_init(&t, NULL, S.id);
    rdof_siteroute_init(&sr, &S);
    rdof_siteroute_add_txvr(&sr, &t, 0);
    rdof_net_burst_t b = net_burst(0x77, 7, 3100, true, 1, false, false);
    CHECK(rdof_siteroute_burst(&sr, 0, 9800702, &b, 1) == 2, "per_path: the second frequency's peer");
    b = net_burst(0x77, 7, 3100, true, 1, false, true);
    rdof_siteroute_burst(&sr, 0, 9800702, &b, 1.1);
    /* a private call to a radio never heard here: the master chose the peer, so its path */
    b = net_burst(0x78, 7, 9809999, false, 1, false, false);
    CHECK(rdof_siteroute_burst(&sr, 0, 9800701, &b, 2) == 1 && sr.router.pc_dropped == 0,
          "per_path: a private call to the peer's path, never heard (dropped %u)", sr.router.pc_dropped);
    b = net_burst(0x78, 7, 9809999, false, 1, false, true);
    rdof_siteroute_burst(&sr, 0, 9800701, &b, 2.1);
    /* heard on the first frequency, sent to the second's peer: the master's choice wins */
    rdof_siteroute_rf(&sr, 438062500, 1, 9809998, 3100, true, 3);
    b = net_burst(0x7a, 7, 9809998, false, 1, false, false);
    CHECK(rdof_siteroute_burst(&sr, 0, 9800702, &b, 4) == 2 && sr.router.pc_dropped == 0,
          "per_path: a private call to the peer's path, heard on another (dropped %u)", sr.router.pc_dropped);
    b = net_burst(0x7a, 7, 9809998, false, 1, false, true);
    rdof_siteroute_burst(&sr, 0, 9800702, &b, 4.1);
    b = net_burst(0x79, 7, 9809999, false, 1, false, false);
    CHECK(rdof_siteroute_burst(&sr, 0, 0, &b, 3) == 0 && sr.router.pc_dropped == 1,
          "single identity: a private call to a radio never heard is dropped");

    /* identity per_path: each peer's name for the master is its frequency's, on the roamer (its idle frequency's)
       and on the receiver's channel */
    static const char pn[] = "{\"version\": 3, \"name\": \"box\", \"id\": 9800700, \"callsign\": \"N0CALL\", "
                             "\"defaults\": {\"cc\": 1, \"tg\": 3100}, "
                             "\"devices\": [{\"name\": \"sdr\", \"kind\": \"sdr\", \"driver\": \"sim\", \"tx\": {}}], "
                             "\"frequencies\": [{\"name\": \"01-SIM\", \"freq\": 438.0625}, {\"name\": \"02-SIM\", \"freq\": 438.0875}], "
                             "\"networks\": [{\"type\": \"hblink\", \"host\": \"h\", \"identity\": \"per_path\"}]}";
    CHECK(rdof_site_parse(pn, sizeof pn - 1, &S, err, sizeof err), "per_path names: %s", err);
    CHECK(!strcmp(S.path[0].peer_name, "01-SIM") && !strcmp(S.path[1].peer_name, "02-SIM"), "per_path names: %s %s",
          S.path[0].peer_name, S.path[1].peer_name);
    static rdof_config_t rc;
    CHECK(rdof_config_parse_radio(pn, sizeof pn - 1, "sdr", &rc, err, sizeof err) && !strcmp(rc.callsign, "01-SIM"),
          "per_path names: the roamer is its idle frequency's peer: %s", rc.callsign);
    static rdof_sdr_config_t sc;
    CHECK(rdof_sdr_config_parse(pn, sizeof pn - 1, &sc, err, sizeof err) && sc.nch == 2 &&
              !strcmp(sc.ch[1].id, "02-SIM"),
          "per_path names: the receiver's channel: %s", err);
}

/* Every fixture: good- taken, bad- refused; a good one's units and SDR parse. */
static void test_fixtures(const char *dir)
{
    char p[512], err[200];
    snprintf(p, sizeof p, "%s/v3", dir);
    DIR *d = opendir(p);
    CHECK(d, "%s", p);
    struct dirent *e;
    int n = 0;
    while (d && (e = readdir(d))) {
        if (!strstr(e->d_name, ".json"))
            continue;
        snprintf(p, sizeof p, "%s/v3/%s", dir, e->d_name);
        size_t len;
        char *t = slurp(p, &len);
        bool good = !strncmp(e->d_name, "good-", 5);
        bool ok = rdof_site_parse(t, len, &S, err, sizeof err);
        CHECK(ok == good, "%s: %s", e->d_name, ok ? "taken" : err);
        if (!good)
            printf("  %s: %s\n", e->d_name, err);
        if (good && ok) {
            static rdof_roles_t roles;
            static rdof_config_t cfg;
            bool rok = rdof_box_roles(t, len, &roles, err, sizeof err);
            CHECK(rok, "%s: roles: %s", e->d_name, err);
            if (rok) {
                for (int k = 0; k < roles.nradios; k++)
                    CHECK(rdof_config_parse_radio(t, len, roles.radios[k], &cfg, err, sizeof err),
                          "%s: radio %s: %s", e->d_name, roles.radios[k], err);
                static rdof_sdr_config_t sc;
                if (roles.has_sdr)
                    CHECK(rdof_sdr_config_parse(t, len, &sc, err, sizeof err), "%s: sdr: %s", e->d_name, err);
            }
        }
        free(t);
        n++;
    }
    if (d)
        closedir(d);
    CHECK(n >= 10, "fixtures: %d", n);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "fixtures/config";
    test_idset();
    test_scenarios(dir);
    test_router(dir);
    test_txpool();
    test_siteroute(dir);
    test_fixtures(dir);
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
