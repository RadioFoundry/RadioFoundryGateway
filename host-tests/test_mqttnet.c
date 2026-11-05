// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The MQTT network (rdof/mqttnet.h): section 13's vectors, the fixture (fixtures/mqtt-net/vectors.json), the reader
 * on bad input, topics, the downlink rebuilding what the uplink heard, data calls against the IP gateway's encoder,
 * the info JSON and the subscriptions. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mqttnet_scenes.h"
#include "rdof/jsonp.h"
#include "rdof/router.h"
#include "rdof/site.h"

static int checks, failures;

#define CHECK(c)                                                                                                     \
    do {                                                                                                             \
        checks++;                                                                                                    \
        if (!(c)) {                                                                                                  \
            failures++;                                                                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                      \
        }                                                                                                            \
    } while (0)

static size_t unhex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0;
    for (; s[0] && s[1] && n < cap; s += 2) {
        while (*s == ' ')
            s++;
        if (!s[0] || !s[1])
            break;
        unsigned v;
        if (sscanf(s, "%2x", &v) != 1)
            break;
        out[n++] = (uint8_t)v;
    }
    return n;
}

/* ── section 13, as the spec writes it ── */

static void test_spec(void)
{
    static const char *const want[4] = {
        "03 02 01 00 78 56 34 12 12 8E 95 5B 00 00 02 09 00 00 00 00 00 5B 95 8E 12 0A 02 03 57",
        "03 02 01 02 78 56 34 12 12 8E 95 5B 00 00 01 1C 00 B9 E8 81 52 61 73 00 2A 6B B9 E8 81 52 61 73 00 2A 6B B9 "
        "E8 81 52 61 73 00 2A 6B 0A 02 03 57",
        "03 00 01 00 78 56 34 12 12 8E 95 06 27 00 02 09 03 00 00 00 27 06 95 8E 12 0A 02 03 57",
        "03 00 00 00 78 56 34 12 12 8E 95 06 27 00 02 09 03 00 00 00 27 06 95 8E 12 0A 02 03 57",
    };
    const int nh = sc_make_hands();
    CHECK(nh >= 4);
    for (int k = 0; k < 4; k++) {
        uint8_t b[64];
        const size_t n = unhex(want[k], b, sizeof b);
        CHECK(sc_hands[k].len == n && !memcmp(sc_hands[k].msg, b, n));
    }
    /* the start's next message is seq 2: two items */
    CHECK(rdof_mn_count(sc_hands[0].msg, sc_hands[0].len) == 2);
}

/* ── the fixture ── */

static rdof_jnode_t nodes[1 << 16];
static char sbuf[1 << 21];
static rdof_jdoc_t doc = {nodes, 1 << 16, 0, sbuf, sizeof sbuf, 0, {0}};

static const char *jstr(int obj, const char *key)
{
    const rdof_jnode_t *n = rdof_jnode(&doc, rdof_jget(&doc, obj, key));
    return n && n->type == RDOF_JSTR ? n->str : "";
}
static double jnum(int obj, const char *key)
{
    const rdof_jnode_t *n = rdof_jnode(&doc, rdof_jget(&doc, obj, key));
    return n && n->type == RDOF_JNUM ? n->num : -1e9;
}

/* A fixture message object against bytes: its hex, and its header and items decoded again from the hex. */
static void check_message(int m, const uint8_t *msg, size_t len, const char *name)
{
    static uint8_t b[RDOF_MN_MSG_MAX];
    static char hx[2 * RDOF_MN_MSG_MAX + 1];
    sc_hex(hx, sizeof hx, msg, len);
    const bool same = !strcmp(hx, jstr(m, "hex"));
    CHECK(same);
    if (!same)
        printf("  %s:\n  want %.200s\n  got  %.200s\n", name, jstr(m, "hex"), hx);
    const size_t n = unhex(jstr(m, "hex"), b, sizeof b);
    rdof_mn_hdr_t h;
    CHECK(rdof_mn_parse(b, n, &h));
    const int hd = rdof_jget(&doc, m, "header");
    CHECK(h.flags == jnum(hd, "flags") && h.path == jnum(hd, "path") && h.seq == jnum(hd, "seq"));
    CHECK(h.stream == jnum(hd, "stream") && h.src == jnum(hd, "src") && h.dst == jnum(hd, "dst"));
    CHECK(rdof_mn_count(b, n) == jnum(hd, "items"));
    const int items = rdof_jget(&doc, m, "items");
    rdof_mn_reader_t r;
    rdof_mn_item_t it;
    rdof_mn_reader(&r, b, n);
    int i = 0;
    while (rdof_mn_next(&r, &it)) {
        const int o = rdof_jat(&doc, items, i++);
        CHECK(o >= 0 && it.type == jnum(o, "type"));
        sc_hex(hx, sizeof hx, it.value, it.len);
        CHECK(!strcmp(hx, jstr(o, "value")));
        CHECK(rdof_mn_item_ok(&it));
        if (it.type == RDOF_MN_QUALITY)
            CHECK(-(int)it.value[1] == jnum(o, "rssi"));
        if (it.type == RDOF_MN_DATA_PDU) {
            rdof_mn_pdu_t p;
            CHECK(rdof_mn_pdu(&it, &p));
            sc_hex(hx, sizeof hx, p.data, p.len);
            CHECK(!strcmp(hx, jstr(o, "data")) && p.part == jnum(o, "part"));
        }
    }
    CHECK(!r.bad && i == rdof_jnode(&doc, items)->count);
}

static void test_fixture(void)
{
    FILE *f = fopen(RDOF_MQTTNET_VECTORS, "rb");
    CHECK(f != NULL);
    if (!f)
        return;
    static char text[1 << 21];
    const size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    const int root = rdof_jparse(&doc, text, n);
    CHECK(root >= 0);
    if (root < 0) {
        printf("  %s\n", doc.error);
        return;
    }
    /* the hand-made vectors */
    const int vs = rdof_jget(&doc, root, "vectors");
    const int nh = sc_make_hands();
    CHECK(rdof_jnode(&doc, vs)->count == nh);
    for (int k = 0; k < nh; k++) {
        const int v = rdof_jat(&doc, vs, k);
        CHECK(!strcmp(jstr(v, "name"), sc_hands[k].name));
        check_message(rdof_jget(&doc, v, "message"), sc_hands[k].msg, sc_hands[k].len, sc_hands[k].name);
    }
    /* the scenes, run again */
    const int ss = rdof_jget(&doc, root, "scenes");
    const int ns = sc_run_all();
    CHECK(rdof_jnode(&doc, ss)->count == ns);
    for (int s = 0; s < ns && s < rdof_jnode(&doc, ss)->count; s++) {
        const sc_out_t *o = &sc_outs[s];
        const int sj = rdof_jat(&doc, ss, s);
        CHECK(!strcmp(jstr(sj, "name"), o->name));
        const int ms = rdof_jget(&doc, sj, "messages");
        CHECK(rdof_jnode(&doc, ms)->count == o->nmsg);
        if (rdof_jnode(&doc, ms)->count != o->nmsg)
            printf("  %s: %d messages, want %d\n", o->name, o->nmsg, rdof_jnode(&doc, ms)->count);
        for (int m = 0; m < o->nmsg && m < rdof_jnode(&doc, ms)->count; m++) {
            const int mj = rdof_jat(&doc, ms, m);
            char topic[96];
            rdof_mn_topic(topic, sizeof topic, "DMR", o->msg[m].kind, o->msg[m].dst, "box-a");
            CHECK(!strcmp(topic, jstr(mj, "topic")));
            check_message(rdof_jget(&doc, mj, "message"), o->msg[m].msg, o->msg[m].len, o->name);
        }
        /* the events: the same count, and each one's ev in order (their text is the generator's) */
        const int es = rdof_jget(&doc, sj, "activity");
        CHECK(rdof_jnode(&doc, es)->count == o->nev);
        for (int e = 0; e < o->nev && e < rdof_jnode(&doc, es)->count; e++) {
            char want[32];
            snprintf(want, sizeof want, "{\"ev\":\"%s\"", jstr(rdof_jat(&doc, es, e), "ev"));
            CHECK(!strncmp(o->ev[e], want, strlen(want)));
        }
    }
}

/* ── what the scenes must say, checked here by hand ── */

static const sc_out_t *scene(const char *name)
{
    for (int k = 0; k < SC_SCENES; k++)
        if (sc_outs[k].name && !strcmp(sc_outs[k].name, name))
            return &sc_outs[k];
    return NULL;
}

/* Item k of message m, its type (-1 none). */
static int item_at(const sc_msg_t *m, int k, rdof_mn_item_t *out)
{
    rdof_mn_reader_t r;
    rdof_mn_reader(&r, m->msg, m->len);
    for (int i = 0; rdof_mn_next(&r, out); i++)
        if (i == k)
            return out->type;
    return -1;
}

static void test_scenes(void)
{
    sc_run_all();
    rdof_mn_item_t it;
    rdof_mn_hdr_t h;
    /* the group call: start, 19 voice, terminator; seq counts items */
    const sc_out_t *g = scene("group_call");
    CHECK(g && g->nmsg == 1 + 19 + 1);
    if (g && g->nmsg == 21) {
        CHECK(item_at(&g->msg[0], 0, &it) == RDOF_MN_LC_HEADER);
        CHECK(item_at(&g->msg[6], 0, &it) == RDOF_MN_VOICE && it.value[0] == 5);
        CHECK(item_at(&g->msg[6], 1, &it) == RDOF_MN_EMBEDDED_LC);
        CHECK(item_at(&g->msg[12], 2, &it) == RDOF_MN_TALKER_ALIAS && it.len == 7 && !memcmp(it.value + 1, "N0CALL", 6));
        CHECK(item_at(&g->msg[18], 2, &it) == RDOF_MN_GPS);
        CHECK(item_at(&g->msg[20], 0, &it) == RDOF_MN_TERMINATOR);
        uint8_t seq = 0;
        for (int m = 0; m < g->nmsg; m++) {
            rdof_mn_parse(g->msg[m].msg, g->msg[m].len, &h);
            CHECK(h.seq == seq && h.path == 1 && h.src == 9990001 && h.dst == 91 && h.flags == RDOF_MN_F_GROUP);
            CHECK(g->msg[m].kind == RDOF_MN_T_TG && g->msg[m].dst == 91);
            seq = (uint8_t)(seq + rdof_mn_count(g->msg[m].msg, g->msg[m].len));
        }
        CHECK(g->msg[1].len == 48 && g->msg[6].len == 59 && g->msg[0].len == 29); /* section 4.5 */
        CHECK(g->nev >= 4 && strstr(g->ev[0], "\"ev\":\"call_start\"") && strstr(g->ev[0], "\"tx_ok\":true"));
        CHECK(strstr(g->ev[g->nev - 1], "\"ev\":\"call_end\"") && strstr(g->ev[g->nev - 1], "\"reason\":\"end\""));
    }
    /* the late entry: every message late; the first has the LC, then the held voice from E */
    const sc_out_t *l = scene("late_entry");
    CHECK(l && l->nmsg > 8);
    if (l && l->nmsg > 8) {
        rdof_mn_parse(l->msg[0].msg, l->msg[0].len, &h);
        CHECK(h.flags == (RDOF_MN_F_LATE | RDOF_MN_F_EMERGENCY) && h.src == 9990001 && h.dst == 9990002 && h.seq == 0);
        CHECK(l->msg[0].kind == RDOF_MN_T_PC && l->msg[0].dst == 9990002);
        CHECK(item_at(&l->msg[0], 0, &it) == RDOF_MN_EMBEDDED_LC);
        CHECK(item_at(&l->msg[0], 1, &it) == RDOF_MN_VOICE && it.value[0] == 4); /* the held: E F A B C D */
        CHECK(item_at(&l->msg[l->nmsg - 1], 0, &it) == RDOF_MN_TERMINATOR);
        CHECK(strstr(l->ev[l->nev - 1], "\"reason\":\"lost\""));
    }
    /* PI */
    const sc_out_t *p = scene("pi_call");
    CHECK(p && p->nmsg >= 3 && item_at(&p->msg[1], 0, &it) == RDOF_MN_PI_HEADER && it.len == 10);
    if (p && p->nmsg >= 3) {
        rdof_mn_parse(p->msg[2].msg, p->msg[2].len, &h);
        CHECK(h.flags & RDOF_MN_F_PRIVACY);
    }
    /* the radio check: one message */
    const sc_out_t *rc = scene("radio_check");
    CHECK(rc && rc->nmsg == 1 && item_at(&rc->msg[0], 0, &it) == RDOF_MN_CSBK && it.value[10] == 1);
#if RDOF_FEATURE_DATA_BLOCKS
    const sc_out_t *u = scene("unconfirmed_data");
    CHECK(u && u->nmsg == 1);
    if (u && u->nmsg == 1) {
        CHECK(item_at(&u->msg[0], 0, &it) == RDOF_MN_CSBK && it.value[10] == 4);
        CHECK(item_at(&u->msg[0], 1, &it) == RDOF_MN_DATA_PDU);
        rdof_mn_pdu_t pd;
        CHECK(rdof_mn_pdu(&it, &pd) && pd.nheaders == 1 && pd.len == 28 + 9 && pd.rate == RDOF_MN_RATE_12);
        CHECK(item_at(&u->msg[0], 2, &it) == RDOF_MN_QUALITY);
        CHECK(u->ndata == 1 && !u->data[0].confirmed && u->data[0].sap == 4);
    }
    const sc_out_t *cp = scene("confirmed_parts");
    CHECK(cp && cp->nmsg == 2);
    if (cp && cp->nmsg == 2) {
        rdof_mn_pdu_t a, b;
        item_at(&cp->msg[0], 0, &it);
        CHECK(rdof_mn_pdu(&it, &a) && a.part == 0 && a.more && a.nheaders == 1);
        item_at(&cp->msg[1], 0, &it);
        CHECK(rdof_mn_pdu(&it, &b) && b.part == 1 && !b.more && b.nheaders == 0 && a.len + b.len == 300);
        CHECK(cp->msg[0].len <= 200 && cp->msg[1].len <= 200);
        CHECK(cp->ndata == 1 && cp->data[0].confirmed && cp->data[0].response_requested && cp->data[0].ns == 3);
    }
    const sc_out_t *c1 = scene("confirmed_rate1");
    CHECK(c1 && c1->nmsg == 1);
    if (c1 && c1->nmsg == 1) {
        CHECK(c1->msg[0].msg[14] == RDOF_MN_DATA_PDU && c1->msg[0].msg[15] == 255); /* the u16 length */
        rdof_mn_pdu_t a;
        item_at(&c1->msg[0], 0, &it);
        CHECK(rdof_mn_pdu(&it, &a) && a.len == 280 && a.rate == RDOF_MN_RATE_1);
        for (size_t i = 0; i < a.len; i++)
            if (a.data[i] != (uint8_t)(i * 7 + 1)) {
                CHECK(!"rate 1 data");
                break;
            }
    }
#endif
}

/* ── the reader on what it shouldn't take ── */

static void test_reader(void)
{
    uint8_t m[64];
    rdof_mn_hdr_t h;
    memcpy(m, sc_hands[0].msg, sc_hands[0].len);
    const size_t n = sc_hands[0].len;
    CHECK(rdof_mn_parse(m, n, &h) && !rdof_mn_parse(m, 13, &h));
    m[0] = 2;
    CHECK(!rdof_mn_parse(m, n, &h));
    m[0] = 3;
    CHECK(rdof_mn_count(m, n - 1) == -1); /* the quality runs past the end */
    CHECK(rdof_mn_count(m, 14) == 0);
    m[15] = 255; /* a u16 length with no room */
    CHECK(rdof_mn_count(m, 17) == -1);
    m[15] = 9;
    /* wrong lengths for known types */
    rdof_mn_item_t it = {RDOF_MN_VOICE, m, 27};
    CHECK(!rdof_mn_item_ok(&it));
    it.type = RDOF_MN_CSBK, it.len = 10;
    CHECK(!rdof_mn_item_ok(&it));
    it.type = 0x91, it.len = 0;
    CHECK(rdof_mn_item_ok(&it));
    uint8_t v[28] = {6};
    it = (rdof_mn_item_t){RDOF_MN_VOICE, v, 28};
    CHECK(!rdof_mn_item_ok(&it)); /* voice index past F */
    uint8_t d[3] = {0, 0, 0};
    it = (rdof_mn_item_t){RDOF_MN_DATA_PDU, d, 3};
    CHECK(!rdof_mn_item_ok(&it)); /* part 0 with no header */
    /* a writer that runs out of room */
    rdof_mn_writer_t w;
    uint8_t small[20];
    rdof_mn_begin(&w, small, sizeof small, &SC_SPEC_HDR);
    CHECK(rdof_mn_quality(&w, 0, 0)); /* 14 + 4 fits in 20 */
    CHECK(!rdof_mn_csbk(&w, small, 1) && rdof_mn_end(&w) == 0);
}

/* ── topics ── */

static void test_topics(void)
{
    char t[96], node[48];
    uint32_t id;
    CHECK(rdof_mn_topic(t, sizeof t, "DMR", RDOF_MN_T_TG, 91, "box-a") && !strcmp(t, "DMR/tg/91/box-a"));
    CHECK(rdof_mn_topic(t, sizeof t, "DMR", RDOF_MN_T_PC, 9990002, "box-a") && !strcmp(t, "DMR/pc/9990002/box-a"));
    CHECK(rdof_mn_topic(t, sizeof t, "DMR", RDOF_MN_T_DL, 0, "box-a") && !strcmp(t, "DMR/dl/box-a"));
    CHECK(rdof_mn_node_topic(t, sizeof t, "DMR", "box-a", "online") && !strcmp(t, "DMR/node/box-a/online"));
    CHECK(rdof_mn_topic(t, 10, "DMR", RDOF_MN_T_TG, 91, "box-a") == 0);
    CHECK(rdof_mn_topic_parse("DMR/tg/91/box-b", "DMR", &id, node, sizeof node) == RDOF_MN_T_TG && id == 91 &&
          !strcmp(node, "box-b"));
    CHECK(rdof_mn_topic_parse("DMR/pc/16777215/x", "DMR", &id, node, sizeof node) == RDOF_MN_T_PC && id == 16777215);
    CHECK(rdof_mn_topic_parse("DMR/dl/box-a", "DMR", &id, node, sizeof node) == RDOF_MN_T_DL);
    CHECK(rdof_mn_topic_parse("DMR/tg/091/box-b", "DMR", &id, node, sizeof node) == RDOF_MN_T_NONE);
    CHECK(rdof_mn_topic_parse("DMR/tg/16777216/b", "DMR", &id, node, sizeof node) == RDOF_MN_T_NONE);
    CHECK(rdof_mn_topic_parse("DMR/tg/91/b/c", "DMR", &id, node, sizeof node) == RDOF_MN_T_NONE);
    CHECK(rdof_mn_topic_parse("DMRX/tg/91/b", "DMR", &id, node, sizeof node) == RDOF_MN_T_NONE);
    CHECK(rdof_mn_topic_parse("DMR/calls/tg/91/b", "DMR", &id, node, sizeof node) == RDOF_MN_T_NONE);
    CHECK(rdof_mn_node_ok("w12-lab.2_a") && !rdof_mn_node_ok("") && !rdof_mn_node_ok("a b") &&
          !rdof_mn_node_ok("a/b") && !rdof_mn_node_ok("a+") &&
          !rdof_mn_node_ok("0123456789012345678901234567890123456789"));
    char key[48];
    CHECK(rdof_mn_call_key(9990001, 91, true, 1793000001.9, key, sizeof key) && !strcmp(key, "9990001-91-g-1793000000"));
    CHECK(rdof_mn_call_key(1, 2, false, 1793000003.0, key, sizeof key) && !strcmp(key, "1-2-p-1793000002"));
}

/* ── the downlink: the scenes' messages back to bursts, and heard again ── */

typedef struct {
    int n;
    uint8_t control[400];
    uint8_t burst[400][RDOF_BURST_BYTES];
    int path[400];
    bool dl[400];
    uint8_t seq[400];
    uint32_t src, dst;
    bool terminators;
} down_cap_t;

static void on_down(void *ctx, const rdof_net_burst_t *b, int path, bool dl, uint8_t flags, const char *from)
{
    down_cap_t *c = ctx;
    if (c->n >= 400)
        return;
    c->control[c->n] = b->control;
    memcpy(c->burst[c->n], b->burst, RDOF_BURST_BYTES);
    c->path[c->n] = path;
    c->dl[c->n] = dl;
    c->seq[c->n] = b->seq;
    c->src = b->src;
    c->dst = b->dst;
    c->terminators |= b->terminator;
    c->n++;
}

/* The scene's messages through a downlink (dl as given), then its bursts through a tracker and an uplink again. */
static void round_trip(const sc_out_t *o, bool dl, down_cap_t *cap, sc_out_t *again)
{
    static rdof_mn_down_t d;
    static sc_run_t r;
    rdof_mn_down_init(&d);
    memset(cap, 0, sizeof *cap);
    d.burst = on_down;
    d.ctx = cap;
    double t = 100;
    for (int m = 0; m < o->nmsg; m++, t += 0.06)
        CHECK(rdof_mn_down_message(&d, "box-b", dl, o->msg[m].msg, o->msg[m].len, t));
    rdof_mn_down_tick(&d, t + 1.0);
    sc_begin(&r, again, o->name, o->what);
    for (int k = 0; k < cap->n; k++)
        sc_hear(&r, cap->control[k], cap->burst[k]);
    sc_idle(&r, 2.0);
}

/* The items of a type in a scene's messages, their values joined in hex (index for voice). */
static void items_of(const sc_out_t *o, int type, char *out, size_t cap)
{
    size_t k = 0;
    out[0] = 0;
    for (int m = 0; m < o->nmsg; m++) {
        rdof_mn_reader_t r;
        rdof_mn_item_t it;
        rdof_mn_reader(&r, o->msg[m].msg, o->msg[m].len);
        while (rdof_mn_next(&r, &it) && k + 8 < cap) {
            if (it.type != type)
                continue;
            if (type == RDOF_MN_VOICE) {
                k += (size_t)snprintf(out + k, cap - k, "%d", it.value[0]);
            } else if (type == RDOF_MN_DATA_PDU) {
                rdof_mn_pdu_t p;
                rdof_mn_pdu(&it, &p);
                k += sc_hex(out + k, cap - k, p.data, p.len);
                if (p.more)
                    continue; /* the parts joined */
            } else {
                k += sc_hex(out + k, cap - k, it.value, it.len);
            }
            out[k++] = '|';
            out[k] = 0;
        }
    }
}

static void test_downlink(void)
{
    static down_cap_t cap;
    static sc_out_t again;
    static char a[16384], b[16384];
    sc_run_all();
    static const int types[] = {RDOF_MN_VOICE, RDOF_MN_TERMINATOR, RDOF_MN_PI_HEADER, RDOF_MN_CSBK, RDOF_MN_DATA_PDU};
    for (int s = 0; s < SC_SCENES; s++) {
        const sc_out_t *o = &sc_outs[s];
        if (!o->name)
            continue;
        round_trip(o, false, &cap, &again);
        CHECK(cap.n > 0);
        for (int k = 0; k < cap.n; k++)
            CHECK(cap.path[k] == 1 && !cap.dl[k]);
        for (unsigned t = 0; t < sizeof types / sizeof types[0]; t++) {
            items_of(o, types[t], a, sizeof a);
            items_of(&again, types[t], b, sizeof b);
            const bool same = !strcmp(a, b);
            CHECK(same);
            if (!same)
                printf("  %s type %d:\n  sent  %.300s\n  again %.300s\n", o->name, types[t], a, b);
        }
        /* addresses, and the late entry's LC as a header now */
        rdof_mn_hdr_t h0, h1;
        rdof_mn_parse(o->msg[0].msg, o->msg[0].len, &h0);
        rdof_mn_parse(again.msg[0].msg, again.msg[0].len, &h1);
        CHECK(h0.src == h1.src && h0.dst == h1.dst && (h0.flags & RDOF_MN_F_GROUP) == (h1.flags & RDOF_MN_F_GROUP));
        CHECK((h0.flags & RDOF_MN_F_EMERGENCY) == (h1.flags & RDOF_MN_F_EMERGENCY));
    }
    /* dl: the path byte and the flag reach the hook */
    const sc_out_t *g = scene("group_call");
    round_trip(g, true, &cap, &again);
    CHECK(cap.n > 0 && cap.dl[0]);
    /* the burst sequence numbers count bursts: a header, then voice */
    CHECK(cap.seq[0] == 0 && cap.seq[1] == 1);
}

/* Lost messages: the burst numbers skip the bursts they held; a late message is dropped; a lone message ends. */
static void test_downlink_loss(void)
{
    static down_cap_t cap;
    static rdof_mn_down_t d;
    sc_run_all();
    const sc_out_t *g = scene("group_call");
    rdof_mn_down_init(&d);
    memset(&cap, 0, sizeof cap);
    d.burst = on_down;
    d.ctx = &cap;
    double t = 0;
    /* the start, A B, (C D lost), E F ... */
    for (int m = 0; m < g->nmsg; m++, t += 0.06) {
        if (m == 3 || m == 4)
            continue;
        rdof_mn_down_message(&d, "box-b", false, g->msg[m].msg, g->msg[m].len, t);
    }
    CHECK(d.gaps == 1);
    CHECK(cap.seq[3] == 5); /* header 0, A 1, B 2, then E 5 */
    /* a message again (seq lower than the next): late, dropped */
    const int before = cap.n;
    rdof_mn_down_message(&d, "box-b", false, g->msg[8].msg, g->msg[8].len, t);
    CHECK(cap.n == before && d.late == 1);
    /* a stream that stops: a terminator after the idle time */
    rdof_mn_down_init(&d);
    memset(&cap, 0, sizeof cap);
    d.burst = on_down;
    d.ctx = &cap;
    for (int m = 0; m < 4; m++)
        rdof_mn_down_message(&d, "box-b", false, g->msg[m].msg, g->msg[m].len, m * 0.06);
    rdof_mn_down_tick(&d, 0.3);
    CHECK(!cap.terminators);
    rdof_mn_down_tick(&d, 0.8);
    CHECK(cap.terminators && (cap.control[cap.n - 1] & 0x0F) == RDOF_DT_TERMINATOR_WITH_LC);
    /* raw items: dropped unless raw_tx */
    rdof_mn_down_init(&d);
    memset(&cap, 0, sizeof cap);
    d.burst = on_down;
    d.ctx = &cap;
    rdof_mn_down_message(&d, "app", false, sc_hands[5].msg, sc_hands[5].len, 0);
    CHECK(cap.n == 0 && d.raw_dropped == 1);
    rdof_mn_down_init(&d);
    d.raw_tx = true;
    d.burst = on_down;
    d.ctx = &cap;
    rdof_mn_down_message(&d, "app", false, sc_hands[5].msg, sc_hands[5].len, 0);
    CHECK(cap.n == 1 && cap.burst[0][0] == 0xA0);
    /* bundled voice and an unknown item: taken */
    rdof_mn_down_init(&d);
    memset(&cap, 0, sizeof cap);
    d.burst = on_down;
    d.ctx = &cap;
    rdof_mn_down_message(&d, "app", false, sc_hands[4].msg, sc_hands[4].len, 0);
    CHECK(cap.n == 4); /* a header from the addresses, then B C D */
    rdof_mn_down_message(&d, "app", false, sc_hands[6].msg, sc_hands[6].len, 0.1);
    CHECK(cap.n == 4 && d.late == 1); /* seq 4 when 7 was next: late, dropped */
    rdof_mn_down_init(&d);
    memset(&cap, 0, sizeof cap);
    d.burst = on_down;
    d.ctx = &cap;
    rdof_mn_down_message(&d, "app", false, sc_hands[6].msg, sc_hands[6].len, 0);
    CHECK(cap.n == 2 && cap.control[1] == 4); /* a header, then E; the unknown item skipped */
    /* garbage */
    const uint8_t bad[5] = {3, 0, 0, 0, 0};
    CHECK(!rdof_mn_down_message(&d, "app", false, bad, sizeof bad, 0.2) && d.bad == 1);
    /* a header and no items: one or more items (section 4) */
    const uint8_t bare[RDOF_MN_HEADER] = {3, 2, 1, 0, 0x78, 0x56, 0x34, 0x12, 0x32, 0x52, 0x2F, 0x5B, 0, 0};
    CHECK(!rdof_mn_down_message(&d, "app", false, bare, sizeof bare, 0.3) && d.bad == 2);
#if RDOF_FEATURE_DATA_BLOCKS
    /* a data PDU item too short to read, then one with no header: skipped, nothing sent */
    uint8_t pdu[RDOF_MN_HEADER + 4 + 5];
    memcpy(pdu, bare, RDOF_MN_HEADER);
    const uint8_t items[] = {RDOF_MN_DATA_PDU, 2, 0x00, 0x00, RDOF_MN_DATA_PDU, 3, 0x00, 0x00, 0x00};
    memcpy(pdu + RDOF_MN_HEADER, items, sizeof items);
    rdof_mn_down_init(&d);
    memset(&cap, 0, sizeof cap);
    d.burst = on_down;
    d.ctx = &cap;
    rdof_mn_down_message(&d, "app", false, pdu, sizeof pdu, 0.4);
    CHECK(cap.n == 0);
#endif
}

#if RDOF_FEATURE_DATA_BLOCKS
/* The data call builder against the IP gateway's: the same bursts for an unconfirmed rate 1/2 call. */
static void test_data_bursts(void)
{
    static rdof_ipgw_dmr_t p;
    static uint8_t controls[RDOF_MN_DATA_BURSTS], bursts[RDOF_MN_DATA_BURSTS][RDOF_BURST_BYTES];
    uint8_t data[100];
    for (size_t len = 1; len <= sizeof data; len += 13) {
        for (size_t i = 0; i < len; i++)
            data[i] = (uint8_t)(0x30 + i);
        CHECK(rdof_ipgw_build_dmr(9990001, 9990002, 4, data, len, 0, SC_DSYNC, &p));
        uint8_t h12[12];
        rdof_bptc196_decode(p.bursts[0], h12);
        const int n = rdof_mn_data_bursts(RDOF_MN_RATE_12, 1, h12, data, len, SC_DSYNC, controls, bursts);
        CHECK(n == p.nbursts);
        for (int k = 0; k < n && k < p.nbursts; k++)
            CHECK(controls[k] == p.control[k] && !memcmp(bursts[k], p.bursts[k], RDOF_BURST_BYTES));
    }
    /* too big */
    uint8_t h[10] = {0x03};
    CHECK(rdof_mn_data_bursts(RDOF_MN_RATE_12, 1, h, data, 3000, SC_DSYNC, controls, bursts) == 0);
    CHECK(rdof_mn_rate_of_dt(RDOF_DT_RATE_34) == RDOF_MN_RATE_34 && rdof_mn_dt_of_rate(RDOF_MN_RATE_1) == RDOF_DT_RATE_1);
    CHECK(rdof_mn_rate_of_dt(RDOF_DT_CSBK) == -1);
}

/* data_result: the downlink's data hook, then the transmitter's end, the radio's answer or none. */
static rdof_mn_results_t results;
static int data_calls;
static void on_data(void *ctx, const rdof_mn_hdr_t *h, bool confirmed, const char *from)
{
    data_calls++;
    rdof_mn_results_add(&results, h, confirmed, from, h->path, 0);
}

static void test_results(void)
{
    sc_run_all();
    static const char *const names[] = {"unconfirmed_data", "confirmed_rate1"};
    rdof_mn_hdr_t hs[2];
    for (int k = 0; k < 2; k++) {
        const sc_out_t *o = scene(names[k]);
        CHECK(o != NULL);
        if (!o)
            return;
        static rdof_mn_down_t d;
        rdof_mn_down_init(&d);
        d.data = on_data;
        for (int m = 0; m < o->nmsg; m++)
            rdof_mn_down_message(&d, "box-b", true, o->msg[m].msg, o->msg[m].len, 0);
        rdof_mn_parse(o->msg[0].msg, o->msg[0].len, &hs[k]);
    }
    CHECK(data_calls == 2);
    rdof_mn_pending_t p;
    /* unconfirmed: done when it's sent */
    const char *r = rdof_mn_results_sent(&results, hs[0].stream, 1.0, &p);
    CHECK(r && !strcmp(r, "unconfirmed") && p.dst == 9990002 && !strcmp(p.from, "box-b") && p.path == 1);
    /* confirmed: the radio's ACK, from the called radio to the caller */
    CHECK(!rdof_mn_results_sent(&results, hs[1].stream, 1.0, &p));
    CHECK(!rdof_mn_results_response(&results, 9990001, 9990002, 0, 1, &p)); /* the other way round: not it */
    r = rdof_mn_results_response(&results, 9990002, 9990001, 0, 1, &p);
    CHECK(r && !strcmp(r, "delivered") && p.confirmed);
    /* no answer, and never on air */
    rdof_mn_results_add(&results, &hs[1], true, "box-b", 1, 2.0);
    CHECK(!rdof_mn_results_sent(&results, hs[1].stream, 2.5, &p));
    CHECK(!rdof_mn_results_tick(&results, 3.4, &p));
    r = rdof_mn_results_tick(&results, 3.6, &p);
    CHECK(r && !strcmp(r, "no_response"));
    rdof_mn_results_add(&results, &hs[1], true, "box-b", 1, 10.0);
    r = rdof_mn_results_response(&results, 9990002, 9990001, 1, 0, &p);
    CHECK(r && !strcmp(r, "failed")); /* a NAK */
    rdof_mn_results_add(&results, &hs[0], false, "box-b", 1, 20.0);
    CHECK(!rdof_mn_results_tick(&results, 24.0, &p));
    r = rdof_mn_results_tick(&results, 25.5, &p);
    CHECK(r && !strcmp(r, "failed") && !rdof_mn_results_tick(&results, 26, &p));
    /* the events: path null when there is none */
    char ev[512];
    CHECK(rdof_mn_tx_event("box-a", 0, -1, 1, 0x12345678, "start", 9990001, 91, "box-b", ev, sizeof ev) &&
          strstr(ev, "\"path\":null,\"slot\":1"));
    CHECK(rdof_mn_data_result_event("box-a", 0, 2, 1, 7, 9990001, 9990002, "router", 7, "delivered", 1, ev,
                                    sizeof ev) &&
          strstr(ev, "\"path\":2,"));
}
#endif

/* ── the info JSON and the subscriptions ── */

static const char SITE[] =
    "{\"version\":3,\"name\":\"lab\",\"id\":9990100,\"defaults\":{\"cc\":1},"
    "\"devices\":[{\"kind\":\"sdr\",\"driver\":\"iio\",\"match\":{\"uri\":\"local:\"}}],"
    "\"frequencies\":[{\"name\":\"rpt in\",\"freq\":438.5},{\"name\":\"rpt out\",\"freq\":418.525},"
    "{\"name\":\"mon\",\"freq\":438.2,\"tx\":false,\"tg\":\"*\"}],"
    "\"apps\":[{\"name\":\"R1\",\"type\":\"repeater\",\"in\":\"rpt in\",\"out\":\"rpt out\","
    "\"slots\":{\"1\":{\"tg\":\"91,3100\",\"ptt\":\"3101-3199\",\"ptt_hold_s\":600},\"2\":{\"tg\":31672}}},"
    "{\"type\":\"parrot\",\"tg\":9990}],"
    "\"networks\":[{\"type\":\"mqtt\",\"host\":\"localhost\",\"calls\":true,\"format\":\"v3\"}]}";

static void test_info(void)
{
    static rdof_site_t s;
    char err[200];
    const bool ok = rdof_site_parse(SITE, sizeof SITE - 1, &s, err, sizeof err);
    CHECK(ok);
    if (!ok) {
        printf("  %s\n", err);
        return;
    }
    const int net = rdof_site_net(&s, "mqtt");
    CHECK(net >= 0 && s.net[net].format == RDOF_MQTT_FORMAT_V3 && s.net[net].bundle == 1);
    static char out[4096];
    const rdof_mn_info_t in = {.node = "box-a", .firmware = "0.9.0", .transport = "mqtt/tcp", .dmr_id = 9990100,
                               .bundle = 1};
    const size_t n = rdof_mn_info_json(&s, net, &in, out, sizeof out);
    CHECK(n > 0);
    static rdof_jnode_t nd[512];
    static char sb[8192];
    rdof_jdoc_t jd = {nd, 512, 0, sb, sizeof sb, 0, {0}};
    const int root = rdof_jparse(&jd, out, n);
    CHECK(root >= 0);
    if (root < 0) {
        printf("  %s\n  %s\n", jd.error, out);
        return;
    }
    const int paths = rdof_jget(&jd, root, "paths");
    CHECK(paths >= 0 && rdof_jnode(&jd, paths)->count == s.npath);
    CHECK(strstr(out, "\"claims\":[9990]") != NULL);
    if (!strstr(out, "\"claims\":[9990]"))
        printf("  %s\n", out);
    CHECK(strstr(out, "\"kind\":\"box\"") && strstr(out, "\"schema\":1"));
    CHECK(rdof_jnode(&jd, rdof_jget(&jd, root, "paths_rev"))->num == rdof_mn_paths_rev(&s, net));
    /* paths_rev: CRC-16/CCITT-FALSE; the router's node vectors (§6's example list gives 2754) */
    CHECK(rdof_mn_crc16((const uint8_t *)"123456789", 9) == 0x29B1);
    CHECK(rdof_mn_crc16(NULL, 0) == 65535);
    static const uint8_t one[] = "\x00hb-9801234", three[] = "\x00rpt1/1\0\x01rpt1/2\0\x02" "438.1";
    CHECK(rdof_mn_crc16(one, sizeof one) == 27753);
    CHECK(rdof_mn_crc16(three, sizeof three) == 2754);
    /* the repeater's two paths: app repeater, ts 1 / 2, tx_ok; the monitor: no ts, no tx */
    const int p0 = rdof_jat(&jd, paths, rdof_site_path(&s, "R1/1"));
    CHECK(!strcmp(rdof_jnode(&jd, rdof_jget(&jd, p0, "app"))->str, "repeater"));
    CHECK(rdof_jnode(&jd, rdof_jget(&jd, p0, "tx_ok"))->b);
    /* subscriptions: 91 and 3100 */
    uint32_t tgs[RDOF_MN_SUBS];
    bool all;
    const int k = rdof_mn_sub_tgs(&s, net, NULL, 0, tgs, RDOF_MN_SUBS, &all);
    CHECK(!all && k == 3 && tgs[0] == 91 && tgs[1] == 3100 && tgs[2] == 31672);
    /* a ptt TG an RF user keys joins them */
    static rdof_router_t r;
    rdof_router_init(&r, &s);
    const rdof_rcall_t key = {.src = 9990001, .dst = 3150, .group = true};
    rdof_router_rf(&r, rdof_site_path(&s, "R1/1"), &key, 10.0);
    const int k2 = rdof_mn_sub_tgs(&s, net, &r, 10.5, tgs, RDOF_MN_SUBS, &all);
    CHECK(!all && k2 == 4 && tgs[2] == 3150); /* after its path's tg set */
    /* a star: everything */
    static rdof_site_t s2;
    static char site2[sizeof SITE + 16];
    snprintf(site2, sizeof site2, "%s", SITE);
    char *t = strstr(site2, "\"91,3100\"");
    memcpy(t, "\"*\"      ", 9);
    CHECK(rdof_site_parse(site2, strlen(site2), &s2, err, sizeof err) || !printf("  %s\n", err));
    rdof_mn_sub_tgs(&s2, net, NULL, 0, tgs, RDOF_MN_SUBS, &all);
    CHECK(all);
}

/* ── config: the mqtt network's new keys ── */

static void test_config(void)
{
    static rdof_site_t s;
    char err[200], text[2048];
    static const struct {
        const char *net;
        bool ok;
        const char *err;
    } cases[] = {
        {"{\"type\":\"mqtt\",\"host\":\"h\",\"calls\":true}", true, ""},
        {"{\"type\":\"mqtt\",\"host\":\"h\",\"calls\":true,\"format\":\"dmrd\"}", true, ""},
        {"{\"type\":\"mqtt\",\"host\":\"h\",\"calls\":true,\"format\":\"v3\",\"bundle\":3,\"raw_tx\":true,"
         "\"transport\":\"mqtt/tls\"}",
         true, ""},
        {"{\"type\":\"mqtt\",\"host\":\"h\",\"format\":\"v3\"}", false, "format: with calls true"},
        {"{\"type\":\"mqtt\",\"host\":\"h\",\"calls\":true,\"format\":\"v4\"}", false, "format: dmrd or v3"},
        {"{\"type\":\"mqtt\",\"host\":\"h\",\"calls\":true,\"format\":\"v3\",\"bundle\":4}", false, "bundle: 1 to 3"},
        {"{\"type\":\"mqtt\",\"host\":\"h\",\"calls\":true,\"bundle\":2}", false, "bundle: with format v3"},
        {"{\"type\":\"mqtt\",\"host\":\"h\",\"calls\":true,\"raw_tx\":true}", false, "raw_tx: with format v3"},
        {"{\"type\":\"mqtt\",\"host\":\"h\",\"transport\":\"mqtt-sn/udp\"}", false, "transport: mqtt/tcp or mqtt/tls"},
        {"{\"type\":\"mqtt\",\"host\":\"h\",\"tls\":true,\"transport\":\"mqtt/tcp\"}", false, "with tls true"},
    };
    for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        snprintf(text, sizeof text,
                 "{\"version\":3,\"name\":\"lab\",\"id\":9990100,\"defaults\":{\"cc\":1},"
                 "\"frequencies\":[438.0625],\"networks\":[%s]}",
                 cases[k].net);
        err[0] = 0;
        const bool ok = rdof_site_parse(text, strlen(text), &s, err, sizeof err);
        CHECK(ok == cases[k].ok && strstr(err, cases[k].err));
        if (ok != cases[k].ok || !strstr(err, cases[k].err))
            printf("  case %u: %s\n", k, err);
    }
    snprintf(text, sizeof text,
             "{\"version\":3,\"name\":\"lab\",\"id\":9990100,\"defaults\":{\"cc\":1},"
             "\"frequencies\":[438.0625],\"networks\":[%s]}",
             cases[2].net);
    CHECK(rdof_site_parse(text, strlen(text), &s, err, sizeof err));
    CHECK(s.net[0].format == RDOF_MQTT_FORMAT_V3 && s.net[0].bundle == 3 && s.net[0].raw_tx &&
          s.net[0].transport == RDOF_MQTT_TLS);
    static char res[16384];
    rdof_site_resolved(&s, res, sizeof res);
    CHECK(strstr(res, "\"format\":\"v3\",\"bundle\":3,\"raw_tx\":true,\"transport\":\"mqtt/tls\"") != NULL);
}

int main(void)
{
    test_spec();
    test_fixture();
    test_scenes();
    test_reader();
    test_topics();
    test_downlink();
    test_downlink_loss();
#if RDOF_FEATURE_DATA_BLOCKS
    test_data_bursts();
    test_results();
#endif
    test_info();
    test_config();
    printf("mqttnet: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
