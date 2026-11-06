// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The trunked site controller through its C API (rdof/t3.h, in Rust): the structs' layouts, the settings checks, and
 * a short scenario: two simulated MSs register and one makes a group call the other follows, until P_CLEAR. */
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/t3.h"
#include "rdof/trunkcfg.h"

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

#define LAYOUT(t) {#t, sizeof(t), alignof(t)}

static void test_layouts(void)
{
    static const rdof_t3_layout_t c[] = {
        LAYOUT(rdof_t3_carrier_t),   LAYOUT(rdof_t3_cfg_t),       LAYOUT(rdof_t3_range_t),
        LAYOUT(rdof_t3_idset_t),     LAYOUT(rdof_t3_policy_t),    LAYOUT(rdof_t3_ms_policy_t),
        LAYOUT(rdof_t3_record_t),    LAYOUT(rdof_t3_setup_t),     LAYOUT(rdof_t3_neighbor_t),
        LAYOUT(rdof_t3_event_t),     LAYOUT(rdof_t3_ch_out_t),    LAYOUT(rdof_t3_slot_out_t),
        LAYOUT(rdof_t3_ch_status_t), LAYOUT(rdof_t3_status_t),    LAYOUT(rdof_t3ms_tx_t),
        LAYOUT(rdof_t3ms_status_t),  LAYOUT(rdof_t3_tg_policy_t),
    };
    const size_t n = sizeof c / sizeof c[0];
    size_t i = 0;
    for (const rdof_t3_layout_t *r; (r = rdof_t3_layout(i)) && r->name; i++) {
        CHECK(i < n, "rust lists %s, C doesn't", r->name);
        if (i >= n)
            break;
        CHECK(strcmp(r->name, c[i].name) == 0, "%zu: rust %s, C %s", i, r->name, c[i].name);
        CHECK(r->size == c[i].size && r->align == c[i].align, "%s: rust %zu/%zu, C %zu/%zu", c[i].name, r->size,
              r->align, c[i].size, c[i].align);
    }
    CHECK(i == n, "rust lists %zu structs, C %zu", i, n);
}

static rdof_t3_site_t *new_site(const rdof_t3_cfg_t *cfg)
{
    rdof_t3_site_t *s = aligned_alloc(16, (rdof_t3_site_size() + 15) & ~(size_t)15);
    char err[96];
    CHECK(s && rdof_t3_init(s, cfg, err, sizeof err), "init: %s", err);
    return s;
}

static void test_settings(void)
{
    rdof_t3_cfg_t cfg;
    rdof_t3_cfg_default(&cfg);
    CHECK(cfg.ncarriers == 1 && cfg.carriers[0].tscc == 1 && cfg.reg_required == 1, "defaults");
    rdof_t3_site_t *s = new_site(&cfg);
    char err[96];
    cfg.hang_ms = 0;
    CHECK(!rdof_t3_reconfigure(s, &cfg, err, sizeof err), "a bad hang time is refused");
    CHECK(strncmp(err, "timers.hang_s", 13) == 0, "err: %s", err);
    cfg.hang_ms = 3000;
    cfg.ncarriers = 2;
    cfg.carriers[1] = (rdof_t3_carrier_t){.tx_hz = 438600000, .rx_hz = 431000000, .chan = 1, .enabled = 1};
    CHECK(!rdof_t3_init(s, &cfg, err, sizeof err) && strstr(err, "used twice"), "err: %s", err);
    cfg.carriers[1].chan = 2;
    CHECK(rdof_t3_reconfigure(s, &cfg, err, sizeof err), "two carriers: %s", err);
    rdof_t3_status_t st;
    rdof_t3_status(s, &st);
    CHECK(st.nch == 4 && st.ch[0].use == RDOF_T3_USE_TSCC && st.ch[1].use == RDOF_T3_USE_IDLE &&
              st.ch[3].use == RDOF_T3_USE_IDLE && st.ch[3].chan == 2 && st.ch[3].lcn == 1,
          "channels");
    CHECK(!rdof_t3_command(s, "nonsense", 0, 0) && rdof_t3_command(s, "status", 0, 0), "commands");
    free(s);
}

#define NMS 2
#define TG 9

struct air {
    rdof_t3_site_t *s;
    rdof_t3ms_t *ms[NMS];
    int64_t k, t0;
    int opens, closes, records;
};

static void drain(struct air *a)
{
    rdof_t3_event_t e;
    while (rdof_t3_event(a->s, &e)) {
        a->opens += e.type == RDOF_T3_EV_CH_OPEN;
        a->closes += e.type == RDOF_T3_EV_CH_CLOSE;
        a->records += e.type == RDOF_T3_EV_RECORD && e.why == 0;
    }
}

/* one slot, as t3air.rs does it: the site's blocks, the MSs' inbound, the site hears it, the MSs hear slot k */
static void step(struct air *a)
{
    const int64_t now = a->t0 + a->k * 30;
    rdof_t3_slot_out_t out;
    rdof_t3_slot(a->s, a->k, now, &out);
    drain(a);
    rdof_t3ms_tx_t tx[NMS];
    bool has[NMS];
    for (int i = 0; i < NMS; i++)
        has[i] = rdof_t3ms_tx(a->ms[i], a->k, now, &tx[i]);
    for (int ch = 0; ch < RDOF_T3_CHANNELS; ch++) {
        int n = 0, first = -1;
        for (int i = 0; i < NMS; i++)
            if (has[i] && tx[i].ch == ch && n++ == 0)
                first = i;
        if (n > 1 && ch == 0)
            rdof_t3_rx_bad(a->s, ch, a->k);
        else if (n && tx[first].kind == 1)
            rdof_t3_rx_pdu(a->s, ch, a->k, tx[first].dt, tx[first].pdu, -60);
        else if (n)
            rdof_t3_rx_voice(a->s, ch, a->k, tx[first].what, tx[first].src, tx[first].dst, tx[first].group);
    }
    drain(a);
    for (int i = 0; i < NMS; i++) {
        rdof_t3ms_status_t st;
        rdof_t3ms_status(a->ms[i], &st);
        if (st.ch == 0 && (a->k & 1) == 0)
            rdof_t3ms_rx(a->ms[i], 0, a->k, out.tscc_dt, out.tscc_pdu, out.at);
        else if ((st.ch & 1) == (a->k & 1))
            rdof_t3ms_rx(a->ms[i], st.ch, a->k, out.ch[st.ch].dt, out.ch[st.ch].pdu, false);
    }
    a->k++;
}

static void run(struct air *a, int ms)
{
    for (int64_t end = a->k + ms / 30; a->k < end;)
        step(a);
}

/* The settings parser's defaults (C, built without the controller) are the controller's. */
static void test_cfg_defaults(void)
{
    rdof_t3_cfg_t a, b;
    rdof_t3_policy_t pa, pb;
    memset(&a, 0, sizeof a);
    memset(&pa, 0, sizeof pa);
    rdof_t3_cfg_default(&a);
    rdof_t3_policy_default(&pa);
    rdof_trunk_cfg_default(&b, &pb);
    CHECK(!memcmp(&a, &b, sizeof a), "trunkcfg's defaults are rdof_t3_cfg_default's");
    CHECK(!memcmp(&pa, &pb, sizeof pa), "trunkcfg's policy is rdof_t3_policy_default's");
    char err[96];
    CHECK(rdof_trunk_cfg_check(&b, err, sizeof err), "defaults pass: %s", err);
}

static void test_scenario(void)
{
    rdof_t3_cfg_t cfg;
    rdof_t3_cfg_default(&cfg);
    struct air a = {.s = new_site(&cfg), .t0 = 1793912651000};
    const uint32_t tg[1] = {TG};
    const uint16_t chans[1] = {1};
    for (int i = 0; i < NMS; i++) {
        a.ms[i] = malloc(rdof_t3ms_size());
        rdof_t3ms_init(a.ms[i], 1001 + i, tg, 1, chans, 1, 7 + i);
        CHECK(rdof_t3ms_do(a.ms[i], RDOF_T3MS_DO_REGISTER, TG, 0, 0, a.t0 + 200 + 300 * i), "queue");
    }
    run(&a, 5000);
    rdof_t3ms_status_t st[NMS];
    for (int i = 0; i < NMS; i++) {
        rdof_t3ms_status(a.ms[i], &st[i]);
        CHECK(st[i].registered == 1 && st[i].last_reason == 0x62, "MS %d registered (reason %#x)", i,
              st[i].last_reason);
    }
    rdof_t3_record_t r;
    CHECK(rdof_t3_record(a.s, 1001, &r) && r.sub_tg == TG && r.how == RDOF_T3_HOW_REGISTER, "record");
    CHECK(a.records >= 2, "records written: %d", a.records);
    uint32_t subs[4];
    CHECK(rdof_t3_subs(a.s, subs, 4) == 1 && subs[0] == TG, "subscriptions");

    rdof_t3ms_do(a.ms[0], RDOF_T3MS_DO_GROUP, TG, 2000, 0, a.t0 + a.k * 30);
    run(&a, 1500);
    for (int i = 0; i < NMS; i++) {
        rdof_t3ms_status(a.ms[i], &st[i]);
        CHECK(st[i].state == RDOF_T3MS_PAYLOAD && st[i].ch == 1, "MS %d on the payload channel (%d, ch %d)", i,
              st[i].state, st[i].ch);
    }
    rdof_t3_status_t ss;
    rdof_t3_status(a.s, &ss);
    CHECK(ss.ch[1].use == RDOF_T3_USE_CALL && ss.ch[1].src == 1001 && ss.ch[1].dst == TG && ss.grants == 1,
          "the call (use %d)", ss.ch[1].use);
    CHECK(a.opens == 1, "opens %d", a.opens);
    run(&a, 6000); // talk, hang, P_CLEAR
    for (int i = 0; i < NMS; i++) {
        rdof_t3ms_status(a.ms[i], &st[i]);
        CHECK(st[i].state == RDOF_T3MS_IDLE && st[i].clears == 1, "MS %d back (%d)", i, st[i].state);
    }
    rdof_t3ms_status(a.ms[0], &st[0]);
    CHECK(st[0].calls_done == 1, "call done");
    CHECK(a.closes == 1, "closes %d", a.closes);
    rdof_t3_status(a.s, &ss);
    CHECK(ss.ch[1].use == RDOF_T3_USE_IDLE && ss.regs == 2 && ss.aloha > 50, "site after (aloha %u)", ss.aloha);

    CHECK(rdof_t3_command(a.s, "deregister", 1002, 0) && !rdof_t3_record(a.s, 1002, &r), "deregister");
    for (int i = 0; i < NMS; i++)
        free(a.ms[i]);
    free(a.s);
}

int main(void)
{
    test_layouts();
    test_settings();
    test_cfg_defaults();
    test_scenario();
    printf("%s (%d failures)\n", fails ? "FAILED" : "ok", fails);
    return fails != 0;
}
