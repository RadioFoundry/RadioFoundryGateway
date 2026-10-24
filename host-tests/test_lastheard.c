// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* Last heard (rdof/lastheard.h): the ring, the tracker's JSON, NET calls, the JSON out; then every golden
 * tracker scenario through it. */
#include <stdio.h>
#include <string.h>

#include "rdof/jsonp.h"
#include "rdof/lastheard.h"
#include "rdof/tracker.h"
#include "vectors.h"

static int checks, failures;

#define CHECK(c)                                                                                                     \
    do {                                                                                                             \
        checks++;                                                                                                    \
        if (!(c)) {                                                                                                  \
            failures++;                                                                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                      \
        }                                                                                                            \
    } while (0)

static rdof_lh_entry_t ring[RDOF_LH_DEFAULT];
static char out[32768];

/* The JSON out parses, and has n calls. Returns the doc's calls array node. */
static rdof_jnode_t nodes[4096];
static char sbuf[32768];
static rdof_jdoc_t doc = {nodes, 4096, 0, sbuf, sizeof sbuf, 0, {0}};

static int parse_calls(const rdof_lh_t *lh, uint32_t since, double now, size_t cap, int *ncalls)
{
    rdof_lh_json(lh, since, now, 1700000000000LL, out, cap);
    int root = rdof_jparse(&doc, out, strlen(out));
    CHECK(root >= 0);
    if (root < 0) {
        printf("  bad JSON: %.300s (%s)\n", out, doc.error);
        *ncalls = 0;
        return -1;
    }
    int calls = rdof_jget(&doc, root, "calls");
    *ncalls = calls >= 0 ? rdof_jnode(&doc, calls)->count : 0;
    return calls;
}

static double fnum(int obj, const char *key)
{
    const rdof_jnode_t *n = rdof_jnode(&doc, rdof_jget(&doc, obj, key));
    return n && n->type == RDOF_JNUM ? n->num : -99999;
}

static const char *fstr(int obj, const char *key)
{
    const rdof_jnode_t *n = rdof_jnode(&doc, rdof_jget(&doc, obj, key));
    return n && n->type == RDOF_JSTR ? n->str : "";
}

static bool fbool(int obj, const char *key)
{
    const rdof_jnode_t *n = rdof_jnode(&doc, rdof_jget(&doc, obj, key));
    return n && n->type == RDOF_JBOOL && n->b;
}

static const char *START = "{\"DMR\":{\"timestamp\":\"2025-10-09T08:53:20.000Z\",\"source\":\"rf\",\"action\":\"start\","
                           "\"slot\":1,\"src_id\":9990070,\"dst_id\":202,\"group\":\"yes\",\"src_info\":\"9990070\","
                           "\"frequency\":\"461.46250\",\"colorCode\":2}}";
static const char *END = "{\"DMR\":{\"timestamp\":\"2025-10-09T08:53:21.140Z\",\"action\":\"end\",\"slot\":1,"
                         "\"duration\":1.14,\"ber\":0.25,\"rssi\":{\"min\":-101,\"max\":-95,\"ave\":-98}}}";

static void test_rf(void)
{
    rdof_lh_t lh;
    rdof_lh_init(&lh, ring, RDOF_LH_DEFAULT);
    int n, calls = parse_calls(&lh, 0, 10, sizeof out, &n);
    CHECK(calls >= 0 && n == 0);
    CHECK(strstr(out, "\"seq\":0,") && strstr(out, "\"max\":50"));

    CHECK(rdof_lh_tracker_json(&lh, "sdr", "sdr1", "ch1", START, 100.0, 1700000000000LL));
    CHECK(lh.n == 1 && ring[0].active && ring[0].freq == 461462500 && ring[0].cc == 2 && ring[0].group);
    calls = parse_calls(&lh, 0, 102.5, sizeof out, &n);
    CHECK(n == 1);
    int c = rdof_jat(&doc, calls, 0);
    CHECK(!strcmp(fstr(c, "dir"), "rf") && !strcmp(fstr(c, "kind"), "voice") && !strcmp(fstr(c, "role"), "sdr"));
    CHECK(!strcmp(fstr(c, "radio"), "sdr1") && !strcmp(fstr(c, "channel"), "ch1"));
    CHECK(fnum(c, "src") == 9990070 && fnum(c, "dst") == 202 && fbool(c, "group") && fnum(c, "slot") == 1);
    CHECK(fnum(c, "freq") == 461462500 && fnum(c, "cc") == 2 && fnum(c, "start_ms") == 1700000000000.0);
    CHECK(fnum(c, "age_s") == 2.5 && fbool(c, "active") && fnum(c, "duration") == -99999);

    /* metrics while it runs; others' slot or radio don't touch it */
    CHECK(rdof_lh_tracker_json(&lh, "sdr", "sdr1", "ch1",
                               "{\"RSSI\":{\"timestamp\":\"x\",\"mode\":\"DMR\",\"slot\":1,\"value\":-100}}", 101, 0));
    CHECK(rdof_lh_tracker_json(&lh, "sdr", "sdr1", "ch1",
                               "{\"BER\":{\"timestamp\":\"x\",\"mode\":\"DMR\",\"slot\":1,\"value\":1.5}}", 101, 0));
    CHECK(!rdof_lh_tracker_json(&lh, "sdr", "sdr1", "ch1",
                                "{\"BER\":{\"timestamp\":\"x\",\"mode\":\"DMR\",\"slot\":2,\"value\":9}}", 101, 0));
    CHECK(!rdof_lh_tracker_json(&lh, "sdr", "sdr1", "ch2",
                                "{\"BER\":{\"timestamp\":\"x\",\"mode\":\"DMR\",\"slot\":1,\"value\":9}}", 101, 0));
    CHECK(ring[0].has_rssi && ring[0].rssi == -100 && ring[0].has_ber && ring[0].ber == 1.5f);
    uint32_t seq = lh.seq;
    CHECK(rdof_lh_privacy(&lh, "sdr1", "ch1", 1) && ring[0].encrypted && lh.seq == seq + 1);
    CHECK(!rdof_lh_privacy(&lh, "sdr1", "ch1", 1));  /* once */
    CHECK(!rdof_lh_privacy(&lh, "sdr1", "ch1", 2));  /* no call there */

    /* the end: its duration, BER and RSSI */
    CHECK(rdof_lh_tracker_json(&lh, "sdr", "sdr1", "ch1", END, 101.2, 0));
    CHECK(!ring[0].active && !strcmp(ring[0].end, "end") && ring[0].duration == 1.14 && ring[0].ber == 0.25f);
    CHECK(ring[0].rssi == -98 && ring[0].rssi_min == -101 && ring[0].rssi_max == -95);
    calls = parse_calls(&lh, 0, 103, sizeof out, &n);
    c = rdof_jat(&doc, calls, 0);
    CHECK(!fbool(c, "active") && fnum(c, "duration") == 1.14 && !strcmp(fstr(c, "end"), "end"));
    CHECK(fnum(c, "rssi") == -98 && fnum(c, "rssi_min") == -101 && fnum(c, "rssi_max") == -95 && fnum(c, "ber") == 0.25);
    CHECK(fbool(c, "encrypted") && !fbool(c, "late") && !fbool(c, "emergency"));
    CHECK(!rdof_lh_tracker_json(&lh, "sdr", "sdr1", "ch1", END, 101.3, 0)); /* nothing left to end */

    /* late entry: a new call, then the same one again is no new entry; another one loses the first */
    const char *late = "{\"DMR\":{\"timestamp\":\"x\",\"source\":\"rf\",\"action\":\"late_entry\",\"slot\":2,"
                       "\"src_id\":9993141,\"dst_id\":203,\"group\":\"no\",\"src_info\":\"9993141\"}}";
    CHECK(rdof_lh_tracker_json(&lh, "roamer", "r1", NULL, late, 110, 0));
    CHECK(lh.n == 2 && ring[1].late && !ring[1].group && ring[1].cc == -1 && ring[1].freq == 0);
    CHECK(!rdof_lh_tracker_json(&lh, "roamer", "r1", "", late, 111, 0));
    CHECK(lh.n == 2);
    const char *other = "{\"DMR\":{\"timestamp\":\"x\",\"source\":\"rf\",\"action\":\"start\",\"slot\":2,"
                        "\"src_id\":7,\"dst_id\":9,\"group\":\"yes\",\"src_info\":\"7\",\"emergency\":true}}";
    CHECK(rdof_lh_tracker_json(&lh, "roamer", "r1", "", other, 112, 0));
    CHECK(lh.n == 3 && !ring[1].active && !strcmp(ring[1].end, "lost") && ring[1].duration == 2.0);
    CHECK(ring[2].active && ring[2].emergency);
    /* lost, with no duration given: from the start */
    CHECK(rdof_lh_tracker_json(&lh, "roamer", "r1", "",
                               "{\"DMR\":{\"timestamp\":\"x\",\"action\":\"lost\",\"slot\":2}}", 112.5, 0));
    CHECK(!ring[2].active && !strcmp(ring[2].end, "lost") && ring[2].duration == 0.5 && !ring[2].has_ber);

    /* a data call: frames */
    CHECK(rdof_lh_tracker_json(&lh, "repeater", "rpt", "",
                               "{\"DMR\":{\"timestamp\":\"x\",\"source\":\"rf\",\"action\":\"start\",\"slot\":1,"
                               "\"src_id\":5,\"dst_id\":6,\"group\":\"no\",\"src_info\":\"5\",\"frames\":3}}",
                               120, 0));
    CHECK(ring[3].kind == RDOF_LH_DATA && ring[3].frames == 3);

    /* CSBKs: one of their own, never active, the data call goes on; preambles left out */
    const char *check = "{\"DMR\":{\"timestamp\":\"x\",\"source\":\"rf\",\"action\":\"csbk\",\"slot\":1,"
                        "\"src_id\":5,\"dst_id\":6,\"group\":\"no\",\"src_info\":\"5\",\"csbk_desc\":\"Radio Check\"}}";
    CHECK(rdof_lh_tracker_json(&lh, "repeater", "rpt", "", check, 120.1, 0));
    CHECK(lh.n == 5 && ring[4].kind == RDOF_LH_CSBK && !ring[4].active && !strcmp(ring[4].desc, "Radio Check"));
    CHECK(ring[3].active);
    CHECK(!rdof_lh_tracker_json(&lh, "repeater", "rpt", "",
                                "{\"DMR\":{\"timestamp\":\"x\",\"source\":\"rf\",\"action\":\"csbk\",\"slot\":1,"
                                "\"src_id\":5,\"dst_id\":6,\"group\":\"no\",\"src_info\":\"5\",\"csbk_desc\":\"Preamble\"}}",
                                120.2, 0));
    calls = parse_calls(&lh, 0, 121, sizeof out, &n);
    c = rdof_jat(&doc, calls, 0);
    CHECK(!strcmp(fstr(c, "kind"), "csbk") && !strcmp(fstr(c, "desc"), "Radio Check") && fnum(c, "duration") == -99999);
    c = rdof_jat(&doc, calls, 1);
    CHECK(!strcmp(fstr(c, "kind"), "data") && fnum(c, "frames") == 3);

    /* what isn't a call: ignored */
    CHECK(!rdof_lh_tracker_json(&lh, "sdr", "s", "", "{\"MMDVM\":{\"timestamp\":\"x\",\"mode\":\"DMR\"}}", 130, 0));
    CHECK(!rdof_lh_tracker_json(&lh, "sdr", "s", "", "{\"DMR\":{\"action\":\"start\"}}", 130, 0)); /* no slot */
    CHECK(!rdof_lh_tracker_json(&lh, "sdr", "s", "", "{\"DMR\":{\"slot\":1}}", 130, 0));            /* no action */
    CHECK(!rdof_lh_tracker_json(&lh, "sdr", "s", "", "{\"DMR\":{\"slot\":1,\"action\":\"x\"}}", 130, 0));
    CHECK(!rdof_lh_tracker_json(&lh, "sdr", "s", "", "not json", 130, 0));
    CHECK(!rdof_lh_tracker_json(&lh, "sdr", "s", "", "[1]", 130, 0));
    CHECK(!rdof_lh_tracker_json(&lh, "sdr", "s", "", "{}", 130, 0));
    CHECK(!rdof_lh_tracker_json(&lh, "sdr", "s", "", NULL, 130, 0));
    CHECK(lh.n == 5);

    /* names are escaped */
    CHECK(rdof_lh_tracker_json(&lh, "sdr", "a\"b", "", START, 140, 0));
    calls = parse_calls(&lh, 0, 141, sizeof out, &n);
    CHECK(!strcmp(fstr(rdof_jat(&doc, calls, 0), "radio"), "a\"b"));
}

static void test_net(void)
{
    rdof_lh_t lh;
    rdof_lh_init(&lh, ring, RDOF_LH_DEFAULT);
    rdof_tx_call_info_t call = {.src = 9990003, .dst = 91, .group = true, .freq = 446006250, .cc = 1, .slot = 2,
                                .frames = -1};
    CHECK(rdof_lh_tx(&lh, "roamer", "r1", "start", &call, 0, 50, 1700000000000LL));
    CHECK(lh.n == 1 && ring[0].dir == RDOF_LH_NET && ring[0].active && ring[0].kind == RDOF_LH_VOICE);
    int n, calls = parse_calls(&lh, 0, 51, sizeof out, &n);
    int c = rdof_jat(&doc, calls, 0);
    CHECK(!strcmp(fstr(c, "dir"), "net") && fnum(c, "freq") == 446006250 && fnum(c, "cc") == 1 && fnum(c, "slot") == 2);
    CHECK(fbool(c, "active") && rdof_jget(&doc, c, "channel") < 0);
    CHECK(!rdof_lh_tx(&lh, "roamer", "r2", "end", &call, 3.5, 54, 0)); /* another radio's */
    CHECK(!rdof_lh_tx(&lh, "roamer", "r1", "other", &call, 3.5, 54, 0));
    CHECK(rdof_lh_tx(&lh, "roamer", "r1", "end", &call, 3.5, 54, 0));
    CHECK(!ring[0].active && ring[0].duration == 3.5 && !strcmp(ring[0].end, "end"));
    CHECK(!rdof_lh_tx(&lh, "roamer", "r1", "end", &call, 3.5, 54, 0));
    /* an RF call on the same radio and slot is a separate entry: a NET call doesn't end it */
    CHECK(rdof_lh_tx(&lh, "repeater", "r1", "start", &call, 0, 60, 0));
    CHECK(rdof_lh_tracker_json(&lh, "repeater", "r1", "",
                               "{\"DMR\":{\"action\":\"start\",\"slot\":2,\"src_id\":1,\"dst_id\":2,\"group\":\"yes\"}}",
                               60.5, 0));
    CHECK(ring[1].active && ring[2].active);
    /* data and a lone CSBK */
    call.frames = 4;
    call.slot = 1;
    CHECK(rdof_lh_tx(&lh, "repeater", "r1", "start", &call, 0, 61, 0));
    CHECK(ring[3].kind == RDOF_LH_DATA && ring[3].frames == 4 && ring[3].active);
    call.has_csbk = true;
    call.frames = -1;
    call.csbk[0] = 0x24; /* last block, opcode radio check */
    CHECK(rdof_lh_tx(&lh, "repeater", "r1", "start", &call, 0, 62, 0));
    CHECK(ring[4].kind == RDOF_LH_CSBK && !ring[4].active && ring[3].active && ring[4].desc[0]);
    CHECK(!rdof_lh_tx(&lh, "repeater", "r1", "start", NULL, 0, 62, 0));
    CHECK(!rdof_lh_tx(&lh, "repeater", "r1", NULL, &call, 0, 62, 0));
}

static void test_ring_and_since(void)
{
    rdof_lh_t lh;
    rdof_lh_init(&lh, ring, RDOF_LH_DEFAULT);
    rdof_tx_call_info_t call = {.src = 1, .dst = 2, .group = true, .slot = 1, .frames = -1};
    for (int i = 0; i < 60; i++) {
        call.src = (uint32_t)(1000 + i);
        rdof_lh_tx(&lh, "roamer", "r", "start", &call, 0, i, 0);
        rdof_lh_tx(&lh, "roamer", "r", "end", &call, 0.5, i + 0.5, 0);
    }
    CHECK(lh.n == 50 && lh.seq == 120);
    int n, calls = parse_calls(&lh, 0, 100, sizeof out, &n);
    CHECK(n == 50);
    CHECK(fnum(rdof_jat(&doc, calls, 0), "src") == 1059 && fnum(rdof_jat(&doc, calls, 49), "src") == 1010);
    CHECK(fnum(rdof_jat(&doc, calls, 0), "id") == 119 && fnum(rdof_jat(&doc, calls, 0), "seq") == 120);

    /* since: only what changed after it */
    calls = parse_calls(&lh, 117, 100, sizeof out, &n);
    CHECK(n == 2 && fnum(rdof_jat(&doc, calls, 0), "src") == 1059 && fnum(rdof_jat(&doc, calls, 1), "src") == 1058);
    calls = parse_calls(&lh, 120, 100, sizeof out, &n);
    CHECK(n == 0);
    /* the board restarted (since past seq): all of it */
    calls = parse_calls(&lh, 5000, 100, sizeof out, &n);
    CHECK(n == 50);
    /* a change brings an old entry back */
    rdof_lh_tx(&lh, "roamer", "r", "start", &call, 0, 200, 0);
    rdof_lh_tracker_json(&lh, "roamer", "r", "", START, 200, 0);
    rdof_lh_tracker_json(&lh, "roamer", "r", "", "{\"RSSI\":{\"slot\":1,\"value\":-50}}", 201, 0);
    calls = parse_calls(&lh, 122, 201, sizeof out, &n);
    CHECK(n == 1 && !strcmp(fstr(rdof_jat(&doc, calls, 0), "dir"), "rf") && fnum(rdof_jat(&doc, calls, 0), "rssi") == -50);

    /* too small: whole entries only, "more" said, still JSON */
    size_t len = rdof_lh_json(&lh, 0, 201, 0, out, 700);
    CHECK(len < 700 && strstr(out, "\"more\":true}"));
    calls = parse_calls(&lh, 0, 201, 700, &n);
    CHECK(calls >= 0 && n >= 1 && n < 50);
    calls = parse_calls(&lh, 0, 201, 100, &n);
    CHECK(calls >= 0 && n == 0);
    CHECK(rdof_lh_json(&lh, 0, 1, 0, out, 10) == 0 && out[0] == 0);
    CHECK(rdof_lh_json(&lh, 0, 1, 0, NULL, 0) == 0);

    /* no ring: nothing kept */
    rdof_lh_init(&lh, NULL, 50);
    CHECK(!rdof_lh_tx(&lh, "roamer", "r", "start", &call, 0, 1, 0));
    CHECK(!rdof_lh_tracker_json(&lh, "roamer", "r", "", START, 1, 0));
    calls = parse_calls(&lh, 0, 1, sizeof out, &n);
    CHECK(calls >= 0 && n == 0);

    /* a ring of one */
    rdof_lh_init(&lh, ring, 1);
    rdof_lh_tracker_json(&lh, "roamer", "r", "", START, 1, 0);
    rdof_lh_tracker_json(&lh, "roamer", "r", "", START, 2, 0); /* the first, lost, then pushed out */
    CHECK(lh.n == 1 && ring[0].active && ring[0].start == 2);
}

#if RDOF_FEATURE_TRACKER
/* Every golden tracker scenario through the ring: each end matches a start, and nothing's left active. */
struct lh_cap {
    rdof_lh_t *lh;
    double t;
    int ends, unmatched, starts;
};

static void lh_json(void *ctx, const char *channel, const char *json)
{
    struct lh_cap *c = ctx;
    bool end = strstr(json, "\"action\":\"end\"") || strstr(json, "\"action\":\"lost\"");
    bool start = strstr(json, "\"action\":\"start\"") != NULL;
    bool changed = rdof_lh_tracker_json(c->lh, "sdr", "sdr", channel, json, c->t, 0);
    c->ends += end;
    c->starts += start;
    if (end && !changed)
        c->unmatched++;
    if (start)
        CHECK(changed);
}

static void test_scenarios(void)
{
    int total = 0;
    for (size_t s = 0; s < V_SCENARIOS_N; s++) {
        const struct v_scenario *sc = &v_scenarios[s];
        static rdof_tracker_t tr;
        rdof_lh_t lh;
        rdof_lh_init(&lh, ring, RDOF_LH_DEFAULT);
        struct lh_cap cap = {&lh, 0, 0, 0, 0};
        rdof_tracker_init(&tr, sc->channel, sc->rssi_offset, sc->freq_hz);
        rdof_sink_t sink = {.on_json = lh_json, .ctx = &cap};
        rdof_tracker_add_sink(&tr, sink);
        double last = 0;
        for (int i = 0; i < sc->nsteps; i++) {
            const struct v_step *st = &sc->steps[i];
            cap.t = last = st->t;
            if (st->op == 't') {
                rdof_tracker_tick(&tr, st->t);
                continue;
            }
            rdof_burst_t b;
            memset(&b, 0, sizeof b);
            for (int k = 0; k < RDOF_BURST_BYTES; k++) {
                unsigned v;
                sscanf(st->burst + 2 * k, "%2x", &v);
                b.burst[k] = (uint8_t)v;
            }
            b.dt = st->dt;
            b.n = st->n;
            b.slot = st->slot;
            b.family = (rdof_family_t)st->family;
            b.cc = st->cc;
            b.sync_errors = st->sync_errors;
            b.power_dbfs = st->power;
            rdof_tracker_feed(&tr, &b, st->t);
        }
        cap.t = last + 60;
        rdof_tracker_tick(&tr, last + 60);
        checks++;
        if (cap.unmatched) {
            failures++;
            printf("FAIL %s: %d of %d ends with no call\n", sc->name, cap.unmatched, cap.ends);
        }
        for (int i = 0; i < lh.n; i++)
            CHECK(!ring[i].active);
        int n;
        CHECK(parse_calls(&lh, 0, last + 60, sizeof out, &n) >= 0 && n == lh.n);
        CHECK(lh.n >= (cap.starts < RDOF_LH_DEFAULT ? cap.starts : RDOF_LH_DEFAULT));
        total += lh.n;
    }
    CHECK(total > 0);
}
#endif

int main(void)
{
    test_rf();
    test_net();
    test_ring_and_since();
#if RDOF_FEATURE_TRACKER
    test_scenarios();
#endif
    printf("lastheard: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
