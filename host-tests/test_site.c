// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The settings, version 3 (rdof/site.h) and ID sets (rdof/idset.h): every fixture in fixtures/config/v3 is taken or
 * refused as its name says; the spec's four sites resolve to the paths and policies it describes; each good one's
 * units and SDR parse. */
#include <dirent.h>
#include <math.h>
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
#include "rdof/siteplan.h"
#include "rdof/siteroute.h"
#include "rdof/t3cfg.h"
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
    if (load(dir, "good-bm-simplex.json")) { /* BrandMeister: the login from peer_ids, no top-level id */
        p = path("hs");
        CHECK(p && p->peer_id == 310012301 && S.net[0].login && S.net[0].type == RDOF_NET_HBLINK, "bm id %u",
              p ? p->peer_id : 0);
        static char out[16384];
        rdof_site_resolved(&S, out, sizeof out);
        CHECK(strstr(out, "\"type\":\"brandmeister\"") && strstr(out, "\"peer_id\":310012301"), "%.200s", out);
        CHECK(!strstr(out, "example\""), "no password in the resolved view");
        CHECK(p && !p->peer_name[0], "a login's RPTC callsign is the owner's, not the path's name");
    }
    if (load(dir, "good-homebrew-simplex.json")) { /* the same login to any Homebrew master, written back as it was */
        p = path("hs");
        CHECK(p && p->peer_id == 310012301 && S.net[0].login && S.net[0].homebrew, "homebrew login");
        static char out[16384];
        rdof_site_resolved(&S, out, sizeof out);
        CHECK(strstr(out, "\"type\":\"homebrew\""), "%.200s", out);
    }
    if (load(dir, "good-bm-per-path.json")) {
        CHECK(path("s1") && path("s1")->peer_id == 310012301 && path("s2") && path("s2")->peer_id == 310012302,
              "bm per_path");
        CHECK(path("R1/1") && path("R1/1")->peer_id == 310012303 && path("R1/2") && path("R1/2")->peer_id == 310012303,
              "bm repeater: one login, both slots");
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
        CHECK(rdof_siteroute_add_txvr(&sr, &t, 0, NULL), "a transceiver");
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
        CHECK(!rdof_siteroute_app_burst(&sr, ap, &a, false, INFINITY, 5.2) && sr.app_no_txvr == 1, "not sent while held");
        /* the transceiver isn't ready: it refuses the stream at its tune, so the app's burst isn't sent (a bench job
           fails at once, not after its bursts went nowhere) and the pool is free again */
        CHECK(rdof_siteroute_app_ready(&sr, ap, 9) && !rdof_siteroute_app_burst(&sr, ap, &a, false, INFINITY, 9) &&
                  sr.app_no_txvr == 2 && cap.n == n0 + 1 && cap.freq == 438112500 && rdof_txvr_refused(&t, a.stream_id) &&
                  rdof_siteroute_refused(&sr, a.stream_id) &&
                  rdof_siteroute_app_ready(&sr, ap, 9.01),
              "refused at its tune: not sent, the pool free (%d %u %u)", cap.n, cap.freq, sr.app_no_txvr);
        static rdof_modem_t md;
        static rdof_radio_t rd;
        rd.modem = &md;
        t.radio = &rd;
        t.ready = true;
        t.nchannels = 1;
        t.channels[0] = (rdof_channel_t){438112500, S.freq[S.path[ap].tx_freq].cc, 255, 1};
        a = net_burst(0x556, 9990, 9800009, false, 1, false, false);
        CHECK(rdof_siteroute_app_burst(&sr, ap, &a, false, INFINITY, 9.02) && rdof_siteroute_tuned(&sr, a.stream_id) &&
                  t.has_call && !rdof_txvr_refused(&t, a.stream_id) && !rdof_siteroute_refused(&sr, a.stream_id),
              "ready: taken (%d)", t.has_call);
        CHECK(rdof_siteroute_app_burst(&sr, ap, &a, false, INFINITY, 9.06) && cap.n == n0 + 1 && !rdof_siteroute_app_ready(&sr, ap, 9.1),
              "tuned once; busy for another");
        static const uint8_t none[4] = {0, 0, 0x77, 0x77};
        CHECK(rdof_siteroute_holds(&sr, a.stream_id) == (t.has_call || t.next.used) && !rdof_siteroute_holds(&sr, none),
              "holds its stream while the transceiver does (%d), not another", t.has_call);
        c = net_burst(0x666, 9800009, 204, true, 1, false, false);
        CHECK(rdof_siteroute_burst(&sr, 0, 0, &c, 9.1) == 0, "a network stream doesn't preempt an app's");
        a = net_burst(0x556, 9990, 9800009, false, 1, false, true);
        rdof_siteroute_app_burst(&sr, ap, &a, false, INFINITY, 9.12);
        CHECK(rdof_siteroute_burst(&sr, 0, 0, &c, 9.2) != 0, "but takes its hold");
        CHECK(!rdof_siteroute_app_ready(&sr, -1, 11) && !rdof_siteroute_app_burst(&sr, 99, &a, false, INFINITY, 11), "no path");
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
    rdof_siteroute_add_txvr(&sr, &t, 0, NULL);
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

    /* power: a frequency's power_dbm is an RF level on the transceiver's map; an app's burst at cap_dbm at most */
    static const char pw[] = "{\"version\": 3, \"id\": 9800700, \"defaults\": {\"cc\": 1, \"tg\": 3100}, "
                             "\"devices\": [{\"name\": \"M\", \"kind\": \"modem\", \"type\": \"radio_hotspot\", "
                             "\"port\": {\"usb\": \"auto\"}, \"tx\": {\"min_dbm\": -10, \"max_dbm\": 41}}], "
                             "\"frequencies\": [{\"freq\": 438.0625, \"power_dbm\": 0}, 438.0875], "
                             "\"networks\": [{\"type\": \"hblink\", \"host\": \"h\"}]}";
    CHECK(rdof_site_parse(pw, sizeof pw - 1, &S, err, sizeof err) && S.freq[0].power_dbm == 0 &&
              isnan(S.freq[1].power_dbm),
          "power_dbm: %s", err);
    const rdof_txmap_t map = {-10, 41};
    const int k0 = rdof_site_path(&S, "438.0625"), k1 = rdof_site_path(&S, "438.0875");
    static const struct {
        bool map;
        int path;
        double cap; /* NAN: a network stream */
        int level;
    } pl[] = {
        {true, 0, NAN, 50},       /* 0 dBm: 255 * 10 / 51 */
        {true, 1, INFINITY, 255}, /* none: the device's own */
        {true, 0, -5, 25},        /* under the cap */
        {true, 0, 3, 50},         /* the cap over it: its own */
        {true, 1, -5, 25},        /* none, a cap: the cap */
        {true, 0, -5.1, 24},      /* between levels: the one under */
        {true, 0, -INFINITY, 0},  /* the least */
        {true, 0, INFINITY, 50},  /* no cap */
        {false, 0, NAN, 255},     /* no map: its own power */
        {false, 0, INFINITY, 255},
        {false, 0, -5, 0},        /* no map under a cap: the least, it can't tell dBm */
    };
    for (size_t i = 0; i < sizeof pl / sizeof pl[0]; i++) {
        static rdof_txvr_t tp;
        static rdof_modem_t md; /* a radio that's never asked anything: the call is only taken here */
        static rdof_radio_t rd;
        rd.modem = &md;
        rdof_txvr_init(&tp, &rd, S.id);
        tp.ready = true;
        tp.nchannels = 2;
        tp.channels[0] = (rdof_channel_t){438062500, 1, 255, 1};
        tp.channels[1] = (rdof_channel_t){438087500, 1, 255, 1};
        rdof_siteroute_init(&sr, &S);
        rdof_siteroute_add_txvr(&sr, &tp, 0, pl[i].map ? &map : NULL);
        rdof_net_burst_t a = net_burst(0x900 + (int)i, 9990, 3100, true, 1, false, false);
        const int k = pl[i].path ? k1 : k0;
        const bool sent = isnan(pl[i].cap) ? rdof_siteroute_burst(&sr, 0, 0, &a, 1) != 0
                                           : rdof_siteroute_app_burst(&sr, k, &a, false, pl[i].cap, 1);
        const int level = tp.has_call ? tp.call.tune.power : tp.next.used ? tp.next.tune.power : -1;
        CHECK(sent && level == pl[i].level, "power %zu: sent %d at %d (want %d)", i, sent, level, pl[i].level);
    }
}

#if RDOF_RUST_T3 /* the Tier III unit's settings (rdof/t3cfg.h) are rust's (tier3) */
/* A trunked site's file: its plan, what a host runs, the Tier III unit's settings and the SDR's. */
static bool trunk_taken(const char *t, size_t len, char *err, size_t cap)
{
    rdof_plan_t *p = rdof_plan_open(t, len, err, cap);
    if (!p)
        return false;
    static rdof_roles_t roles;
    static rdof_t3_cfg_t cfg;
    static rdof_t3_policy_t pol;
    static rdof_sdr_config_t sc;
    const rdof_site_t *s = rdof_plan_site(p);
    bool sdr = false; /* a site of radios alone (split TX and RX halves) has no SDR settings */
    for (int d = 0; d < s->ndev; d++)
        sdr |= s->dev[d].kind == RDOF_DEV_SDR;
    bool ok = rdof_plan_roles_on(p, &rdof_plan_host, &roles, err, cap) &&
              rdof_t3cfg_plan(p, -1, &cfg, &pol, err, cap) && (!sdr || rdof_plan_sdr(p, NULL, &sc, err, cap));
    rdof_plan_close(p);
    return ok;
}

/* A JSON text's top object over cfg and pol (rdof_t3cfg_apply). */
static bool apply_text(const char *text, const char *path, rdof_t3_cfg_t *cfg, rdof_t3_policy_t *pol, char *err,
                       size_t cap)
{
    static rdof_jnode_t nodes[256];
    static char buf[2048];
    rdof_jdoc_t doc = {.nodes = nodes, .max = 256, .buf = buf, .cap = sizeof buf};
    int root = rdof_jparse(&doc, text, strlen(text));
    if (root < 0) {
        snprintf(err, cap, "json: %s", doc.error);
        return false;
    }
    return rdof_t3cfg_apply(&doc, root, path, cfg, pol, err, cap) && rdof_t3cfg_check(cfg, err, cap);
}
#endif

/* The Tier III unit: its devices, its carriers, its settings (rdof/t3cfg.h), and the manager's documents over them. */
static void test_trunk(const char *dir)
{
    char p[512], err[200] = "";
    size_t n;
    snprintf(p, sizeof p, "%s/v3/good-tier3.json", dir);
    char *t = slurp(p, &n);
    rdof_plan_t *pl = t ? rdof_plan_open(t, n, err, sizeof err) : NULL;
    CHECK(pl, "good-trunk: %s", err);
    free(t);
    if (!pl)
        return;
    const rdof_site_t *s = rdof_plan_site(pl);
#if RDOF_RUST_T3
    int u = rdof_t3cfg_unit(pl);
#else
    int u = -1;
    for (int k = 0; u < 0 && k < rdof_plan_nunits(pl); k++)
        u = rdof_plan_unit(pl, k)->kind == RDOF_UNIT_TIER3 ? k : -1;
#endif
    CHECK(u >= 0 && !strcmp(rdof_plan_unit(pl, u)->name, "site"), "the Tier III unit");
    int rx = -1, tx = -1, d0 = -1, d1 = -1;
    for (int d = 0; d < s->ndev; d++) {
        if (!strcmp(s->dev[d].name, "pluto"))
            d0 = d;
        if (!strcmp(s->dev[d].name, "txb"))
            d1 = d;
    }
    CHECK(rdof_plan_t3_carrier(pl, u, 0, &rx, &tx) && rx == d0 && tx == d0, "the TSCC on the Pluto: %d %d", rx, tx);
    CHECK(rdof_plan_t3_carrier(pl, u, 1, &rx, &tx) && rx == d0 && tx == d1, "B heard on the Pluto, sent by txb");
    CHECK(!rdof_plan_t3_carrier(pl, u, 2, &rx, &tx), "two carriers");
    CHECK(rdof_plan_unit_uses(pl, u, d1) && rdof_plan_sdr_used(pl, d0), "uses");
    CHECK(rdof_plan_nextra(pl) == 2 && rdof_plan_extra(pl, 0)->hz == 438250000 &&
              rdof_plan_extra(pl, 1)->hz == 438500000 && !strcmp(rdof_plan_extra(pl, 1)->id, "site-2"),
          "the SDR hears both ins: %d", rdof_plan_nextra(pl));
    static rdof_roles_t roles;
    CHECK(!rdof_plan_roles(pl, &roles, err, sizeof err) && strstr(err, "runs on a host"), "not on a board: %s", err);
    CHECK(rdof_plan_roles_on(pl, &rdof_plan_host, &roles, err, sizeof err) && roles.nradios == 1 && roles.has_sdr,
          "a host: %s", err);
    static rdof_config_t rc;
    CHECK(!rdof_plan_radio(pl, "site", &rc, err, sizeof err) && strstr(err, "trunked site"), "no radio: %s", err);
#if RDOF_RUST_T3
    static rdof_t3_cfg_t c;
    static rdof_t3_policy_t pol;
    CHECK(rdof_t3cfg_plan(pl, -1, &c, &pol, err, sizeof err), "cfg: %s", err);
    CHECK(c.model == 1 && c.net == 5 && c.site == 3 && c.par == 3 && c.area_bits == 5 && c.cc == 1, "system");
    CHECK(c.ncarriers == 2 && c.carriers[0].tscc && c.carriers[0].chan == 1 && c.carriers[0].tx_hz == 438750000 &&
              c.carriers[0].rx_hz == 438250000 && !c.carriers[1].tscc && c.carriers[1].chan == 2 &&
              c.carriers[1].tx_hz == 438600000 && c.carriers[1].enabled,
          "carriers, the TSCC's first");
    CHECK(c.hang_ms == 3000 && c.tp_ms == 20000 && c.reg_required && !c.foacsu && c.nrand_wait == 6 && !c.dgna_ms,
          "timers");
    CHECK(!c.als && c.divert, "no ALS, diversion on");
    CHECK(pol.open && pol.ms.all && pol.tg.all && pol.private_ms.all, "open");
    CHECK(!pol.wide_tg.all && pol.wide_tg.n == 1 && pol.wide_tg.r[0].lo == 1 && pol.wide_tg.r[0].hi == 99999,
          "wide_tg");
    CHECK(!pol.static_tg.all && !pol.static_tg.n, "no static_tg");
#endif
    rdof_plan_close(pl);
#if RDOF_RUST_T3

    /* cfg/net over it, then cfg/node */
    CHECK(apply_text("{\"rev\": 12, \"system\": {\"model\": \"large\", \"net\": 9}, \"registration\": \"listed\", "
                     "\"ms\": \"3100000-3199999,!3100500\", \"refuse_unlisted\": \"refused\", \"timers\": "
                     "{\"hang_s\": 4.5, \"grant_every_s\": 0.6}, \"broadcast\": {\"local_time_s\": 0}, "
                     "\"dgna_from_ms\": true, \"ambient_listening\": true, \"call_diversion\": false}",
                     "cfg/net", &c, &pol, err, sizeof err),
          "cfg/net: %s", err);
    CHECK(c.model == 2 && c.net == 9 && c.area_bits == 8 && c.hang_ms == 4500 && c.grant_every_ms == 600 &&
              !c.local_time_ms && c.adjacent_ms == 5000 && c.dgna_ms && c.als && !c.divert,
          "cfg/net applied");
    CHECK(!pol.open && pol.refuse_unlisted && pol.ms.n == 2 && pol.ms.r[0].hi == 3100499 &&
              pol.ms.r[1].lo == 3100501,
          "listed less one");
    CHECK(apply_text("{\"rev\": 3, \"site\": 200, \"area_bits\": 6, \"static_tg\": [91, \"3100-3101\"], "
                     "\"neighbors\": [4], \"enabled\": true}",
                     "cfg/node", &c, &pol, err, sizeof err),
          "cfg/node: %s", err);
    CHECK(c.site == 200 && c.area_bits == 6 && pol.static_tg.n == 2, "cfg/node applied");
    CHECK(!apply_text("{\"site\": 256}", "cfg/node", &c, &pol, err, sizeof err) && !strcmp(err, "cfg/node.site: 0 to 255"),
          "site range: %s", err);
    /* SITE 0 is one of a network's sites (361-4 table 6.5), the one a standard site in the off-air captures uses */
    CHECK(apply_text("{\"site\": 0}", "cfg/node", &c, &pol, err, sizeof err) && c.site == 0, "site 0: %s", err);
    CHECK(apply_text("{\"site\": 200}", "cfg/node", &c, &pol, err, sizeof err) && c.site == 200, "site back: %s", err);
    CHECK(!apply_text("{\"system\": {\"model\": \"medium\"}}", "cfg/net", &c, &pol, err, sizeof err) &&
              strstr(err, "cfg/net.system.model"),
          "%s", err);
    CHECK(!apply_text("{\"timers\": {\"hang\": 3}}", "cfg/net", &c, &pol, err, sizeof err) &&
              !strcmp(err, "cfg/net.timers.hang: not a key here"),
          "%s", err);
    CHECK(!apply_text("{\"timers\": {\"backoff_min\": 10, \"backoff_max\": 4}}", "x", &c, &pol, err, sizeof err),
          "backoff order");
    rdof_t3cfg_default(&c, &pol); /* a refused text leaves cfg half applied */
    CHECK(!c.preempt && apply_text("{\"services\": {\"emergency_preempt\": true}}", "x", &c, &pol, err, sizeof err) &&
              c.preempt == 1,
          "services.emergency_preempt: %s", err);
    CHECK(!c.auth && apply_text("{\"auth\": \"required\"}", "x", &c, &pol, err, sizeof err) &&
              c.auth == RDOF_T3_AUTH_REQUIRED &&
              apply_text("{\"auth\": \"registration\"}", "x", &c, &pol, err, sizeof err) &&
              c.auth == RDOF_T3_AUTH_REGISTRATION,
          "auth: %s", err);
    CHECK(!apply_text("{\"auth\": true}", "x", &c, &pol, err, sizeof err) &&
              !strcmp(err, "x.auth: off, registration or required"),
          "%s", err);
    CHECK(!apply_text("{\"services\": {\"preempt\": true}}", "x", &c, &pol, err, sizeof err) &&
              !strcmp(err, "x.services.preempt: not a key here"),
          "%s", err);
    CHECK(c.awake_ms == 1000 &&
              apply_text("{\"services\": {\"power_save\": 3, \"power_save_awake_s\": 2.5, \"mass_reg_at_start_s\": 60,"
                         " \"hibernate_s\": 20, \"duplex\": true}, \"broadcast\": {\"call_timers_s\": 30, \"vote_now_s\": 20, \"ann_tscc_s\": 40}}",
                         "x", &c, &pol, err, sizeof err) &&
              c.power_save == 3 && c.awake_ms == 2500 && c.mass_reg_start == 8 && c.call_timers_ms == 30000 &&
              c.vote_now_ms == 20000 && c.ann_tscc_ms == 40000 && c.hibernate_ms == 20000 && c.duplex == 1,
          "power save, mass registration, broadcasts: %s", err);
    CHECK(!apply_text("{\"services\": {\"power_save\": 8}}", "x", &c, &pol, err, sizeof err), "power_save 8");
    CHECK(c.ps_hold_ms == 2000 &&
              apply_text("{\"services\": {\"power_save_group_hold_s\": 0}}", "x", &c, &pol, err, sizeof err) &&
              c.ps_hold_ms == 0 &&
              !apply_text("{\"services\": {\"power_save_group_hold_s\": 11}}", "x", &c, &pol, err, sizeof err),
          "power_save_group_hold_s: %s", err);
    /* VOTE_BLK (361-4 A.2: 2 to 10 frames, default 6) and the site's priority (table 6.81: 0 to 7, default 0) */
    rdof_t3cfg_default(&c, &pol);
    CHECK(c.vote_blk == 6 && c.priority == 0 &&
              apply_text("{\"broadcast\": {\"vote_now_s\": 30, \"vote_blk\": 8, \"priority\": 3}}", "x", &c, &pol, err,
                         sizeof err) &&
              c.vote_now_ms == 30000 && c.vote_blk == 8 && c.priority == 3,
          "vote_blk, priority: %s", err);
    CHECK(!apply_text("{\"broadcast\": {\"vote_blk\": 1}}", "x", &c, &pol, err, sizeof err) &&
              !strcmp(err, "x.broadcast.vote_blk: 2 to 10"),
          "%s", err);
    CHECK(!apply_text("{\"broadcast\": {\"vote_blk\": 11}}", "x", &c, &pol, err, sizeof err), "vote_blk 11");
    CHECK(!apply_text("{\"broadcast\": {\"priority\": 8}}", "x", &c, &pol, err, sizeof err) &&
              !strcmp(err, "x.broadcast.priority: 0 to 7"),
          "%s", err);
    CHECK(!apply_text("{\"services\": {\"hibernate_s\": 0.5}}", "x", &c, &pol, err, sizeof err), "hibernate_s 0.5");
    rdof_t3cfg_default(&c, &pol);
    CHECK(c.composite == 0 && apply_text("{\"services\": {\"composite_tscc\": true}}", "x", &c, &pol, err, sizeof err) &&
              c.composite == 1,
          "composite_tscc: %s", err);
    CHECK(!apply_text("{\"services\": {\"composite_tscc\": 1}}", "x", &c, &pol, err, sizeof err), "composite_tscc 1");
    rdof_t3cfg_default(&c, &pol);
    CHECK(c.composite_le == 0 &&
              apply_text("{\"services\": {\"composite_late_entry\": true}}", "x", &c, &pol, err, sizeof err) &&
              c.composite_le == 1,
          "composite_late_entry: %s", err);
    rdof_t3cfg_default(&c, &pol);
    CHECK(c.intr == 0 && apply_text("{\"services\": {\"transmit_interrupt\": true}}", "x", &c, &pol, err, sizeof err) &&
              c.intr == 1,
          "transmit_interrupt: %s", err);
    CHECK(!apply_text("{\"services\": {\"transmit_interrupt\": \"on\"}}", "x", &c, &pol, err, sizeof err),
          "transmit_interrupt on");
    rdof_t3cfg_default(&c, &pol);
    CHECK(!apply_text("{\"timers\": {\"warmup_cach\": 3}}", "x", &c, &pol, err, sizeof err), "warmup 3");
    CHECK(!apply_text("{\"wide_tg\": \"1,3,5,7,9,11,13,15,17,19,21,23,25,27,29,31,33\"}", "x", &c, &pol, err,
                      sizeof err),
          "too many ranges: %s", err);
    /* the same through the provisioning's resolved form (rdof/mnctl.h) */
    {
        static rdof_mn_site_cfg_t sc;
        long rev = 0;
        char note[96];
        const char *doc = "{\"rev\": 4, \"system\": {\"model\": \"small\", \"net\": 7}, \"site\": 9, "
                          "\"registration\": \"listed\", \"timers\": {\"hang_s\": 2, \"queue_max\": 8}}";
        CHECK(rdof_mn_site_cfg_parse(doc, strlen(doc), &sc, &rev, note, sizeof note, err, sizeof err), "%s", err);
        rdof_t3cfg_default(&c, &pol);
        CHECK(rdof_t3cfg_site(&sc, &c, &pol, err, sizeof err), "site cfg: %s", err);
        CHECK(c.net == 7 && c.site == 9 && c.hang_ms == 2000 && c.queue_max == 8 && !pol.open && !pol.ms.all &&
                  !pol.ms.n,
              "site cfg applied");
        sc.timers[0].value = 99;
        CHECK(!rdof_t3cfg_site(&sc, &c, &pol, err, sizeof err) && !strcmp(err, "cfg.timers.hang_s: 1 to 60"),
              "%s", err);
    }
    rdof_t3cfg_default(&c, &pol);
    CHECK(apply_text("{\"system\": {\"model\": \"tiny\", \"net\": 511, \"site\": 7}}", "x", &c, &pol, err, sizeof err) &&
              c.area_bits == 3,
          "tiny: %s", err);
#endif
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
        const bool trunk = ok && rdof_site_app_of(&S, RDOF_APP_TIER3) >= 0;
#if RDOF_RUST_T3
        if (trunk)
            ok = trunk_taken(t, len, err, sizeof err);
#else
        if (trunk) { /* the Tier III unit's settings are rust's (tier3): not checked here */
            free(t);
            continue;
        }
#endif
        CHECK(ok == good, "%s: %s", e->d_name, ok ? "taken" : err);
        if (!good)
            printf("  %s: %s\n", e->d_name, err);
        if (good && ok && !trunk) {
            static rdof_roles_t roles;
            static rdof_config_t cfg;
            bool rok = rdof_box_roles(t, len, &roles, err, sizeof err);
            CHECK(rok, "%s: roles: %s", e->d_name, err);
            if (rok) {
                for (int k = 0; k < roles.nradios; k++) {
                    CHECK(rdof_config_parse_radio(t, len, roles.radios[k], &cfg, err, sizeof err),
                          "%s: radio %s: %s", e->d_name, roles.radios[k], err);
                    if (strstr(e->d_name, "-bm-"))
                        CHECK(cfg.hb_login && !strcmp(cfg.hb_password, "example") && cfg.peer_id >= 310012301 &&
                                  !strcmp(cfg.master_host, "bm.example.net"),
                              "%s: %s: login %d %u", e->d_name, roles.radios[k], cfg.hb_login, cfg.peer_id);
                }
                static rdof_sdr_config_t sc;
                if (roles.has_sdr) {
                    CHECK(rdof_sdr_config_parse(t, len, &sc, err, sizeof err), "%s: sdr: %s", e->d_name, err);
                    if (strstr(e->d_name, "-bm-"))
                        CHECK(sc.hb_login && !strcmp(sc.hb_password, "example") &&
                                  !strcmp(sc.hb_description, "RadioFoundry test"),
                              "%s: sdr login", e->d_name);
                }
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
    test_trunk(dir);
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
