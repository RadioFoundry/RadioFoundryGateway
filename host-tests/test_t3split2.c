// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* A two-carrier trunked site sent wholly by split TX halves (rdof/sdr/t3split.h), the SDR's receiver the RX half of
 * both: carrier A, the TSCC's, and carrier B each on a fake radio (odr_split) with its own clock. Carrier A's half
 * takes the CACH with each scheduled burst (RDOF_OPS_CACH), so the controller's AT and short LC go out slot by slot:
 * every slot of carrier A is scheduled while the site runs, each CACH's AT, TC and short LC are the controller's, and
 * the MSs register and call from what the two halves air. Carrier B's half has no CACH op: its bursts go as before,
 * idle slots left to the half. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_radio.h"
#include "rdof/sdr/t3split.h"
#include "rdof/split.h"
#include "rdof/splitops.h"
#include "rdof/t3cfg.h"
#include "rdof/t3rf.h"
#include "rdof/t3sim.h"
#include "rdof/t3unit.h"

static int fails;

#define CHECK(cond, ...)                                                                                           \
    do {                                                                                                           \
        if (!(cond)) {                                                                                             \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                            \
            printf(__VA_ARGS__);                                                                                   \
            printf("\n");                                                                                          \
            fails++;                                                                                               \
        }                                                                                                          \
    } while (0)

#define T0 1793912651000LL /* UTC ms at host time 1000 s */
#define UTC_MINUS_HOST_US ((double)T0 * 1000.0 - 1000e6)
#define RX_A 438250000u
#define TX_A 438750000u
#define RX_B 438275000u
#define TX_B 438775000u
#define NMS 4

static struct {
    double clock;
    fake_radio_t f[2];
    rdof_t3split_t ts[2];
    rdof_t3unit_t *u;
    rdof_t3sim_t sim;
    rdof_t3ms_t *ms[NMS];
    uint32_t id[NMS];
    int64_t k_done;
    uint32_t aired[2]; /* each carrier's bursts an MS read from its half */
    /* carrier A's CACHs as aired: how many, their AT against the controller's, TC against the slot, the short LC */
    uint32_t cach, cach_none, cach_bad, at_ok, at_busy, tc_ok, slc_groups, slc_ok;
    int64_t group_k;
    uint32_t rows[4];
    int nrows;
} S;

static void to_radio(void *ctx, const uint8_t *d, size_t n) { fake_write(&S.f[(intptr_t)ctx], d, n); }
static int rx_channel(void *ctx, uint32_t hz) { return hz == RX_B ? 1 : hz == RX_A ? 0 : -1; }
static int64_t now48(void *ctx) { return llround(S.clock * 48000.0); }
static int64_t utc_ms(void) { return T0 + llround((S.clock - 1000.0) * 1000.0); }

static void pump(void)
{
    uint8_t buf[4096];
    size_t n;
    for (int c = 0; c < 2; c++)
        while ((n = fake_take(&S.f[c], buf, sizeof buf)) > 0)
            rdof_t3split_from_radio(&S.ts[c], buf, n, S.clock);
}

/* carrier A's CACH before slot k, as aired: checked against the controller */
static void check_cach(int64_t k, const uint8_t cach[3])
{
    int at = -1, slot = 0, lcss = -1;
    uint32_t p17 = 0;
    if (!rdof_t3rf_cach_read(cach, &at, &slot, &lcss, &p17)) {
        S.cach_bad++;
        return;
    }
    S.cach++;
    S.at_ok += at == (rdof_t3unit_at(S.u, k - 1) > 0);
    S.at_busy += at == 1;
    S.tc_ok += slot == (int)(k & 1) + 1;
    if ((k & 3) == 0)
        S.group_k = k, S.nrows = 0;
    if (S.group_k == (k & ~3LL) && S.nrows == (int)(k & 3))
        S.rows[S.nrows++] = p17;
    uint32_t slc;
    if (S.nrows == 4 && rdof_t3rf_slc_join(S.rows, &slc)) {
        S.slc_groups++;
        S.slc_ok += (int64_t)slc == rdof_t3unit_slc(S.u, 0, S.group_k);
        S.nrows = 0;
    }
}

/* carrier c's slot k as its half airs it (queued or gone out) */
static bool outbound(void *ctx, int c, int64_t k, uint8_t burst[RDOF_BURST_BYTES])
{
    const fake_radio_t *f = &S.f[c];
    const uint32_t n = (uint32_t)k;
    const uint8_t *b = NULL, *cach = NULL;
    for (int i = 0; i < (int)(sizeof f->sched / sizeof f->sched[0]) && !b; i++)
        if (f->sched[i].used && f->sched[i].counter == n)
            b = f->sched[i].burst, cach = f->sched[i].has_cach ? f->sched[i].cach : NULL;
    for (int i = f->non_air - 1; i >= 0 && !b; i--)
        if (f->on_air[i].counter == n)
            b = f->on_air[i].burst, cach = f->on_air[i].has_cach ? f->on_air[i].cach : NULL;
    if (!b)
        return false;
    memcpy(burst, b, RDOF_BURST_BYTES);
    S.aired[c]++;
    if (c == 0) {
        if (cach)
            check_cach(k, cach);
        else
            S.cach_none++;
    }
    return true;
}

static void run(double seconds)
{
    const double end = S.clock + seconds;
    while (S.clock < end) {
        S.clock += 0.01;
        const int64_t k_now = utc_ms() / 30;
        for (int c = 0; c < 2; c++) {
            rdof_t3split_tick(&S.ts[c], S.clock);
            pump();
            if (S.ts[c].state == RDOF_T3SPLIT_UP) {
                rdof_t3unit_split_pump(S.u, &S.ts[c].o, rdof_t3split_split(&S.ts[c]), k_now, S.clock);
                pump();
            }
        }
        for (; S.k_done < k_now; S.k_done++) {
            const int64_t k = S.k_done + 1;
            rdof_t3sim_slot(&S.sim, k, k * 30);
            if (k % 10 == 0)
                rdof_t3unit_tick(S.u, k * 30);
        }
        for (int c = 0; c < 2; c++)
            if (S.f[c].non_air > 48) { /* keep the newest */
                memmove(S.f[c].on_air, S.f[c].on_air + 32, 16 * sizeof S.f[c].on_air[0]);
                S.f[c].non_air = 16;
            }
    }
}

static void no_publish(void *ctx, const char *topic, const void *data, size_t len, int qos, bool retain) {}
static void no_subscribe(void *ctx, const char *const *f, int n) {}
static void say(void *ctx, const char *text)
{
    if (getenv("RDOF_TEST_VERBOSE"))
        printf("%s\n", text);
}
static uint32_t rng = 778;
static uint32_t rand32(void *ctx)
{
    rng ^= rng << 13, rng ^= rng >> 17, rng ^= rng << 5;
    return rng;
}

static rdof_t3ms_status_t ms_status(int i)
{
    rdof_t3ms_status_t st;
    rdof_t3ms_status(S.ms[i], &st);
    return st;
}

int main(void)
{
    memset(&S, 0, sizeof S);
    S.clock = 1000.0;
    for (int c = 0; c < 2; c++) {
        fake_init(&S.f[c], "odr_split");
        S.f[c].clock = &S.clock;
    }
    S.f[0].clock_offset_us = 2345678.0, S.f[0].clock_rate = 12e-6;
    S.f[1].clock_offset_us = 987654.0, S.f[1].clock_rate = -7e-6;
    S.f[0].split_caps |= RDOF_OPS_CACH; /* carrier A's half takes the CACH; carrier B's doesn't */

    rdof_t3_cfg_t cfg;
    rdof_t3_policy_t pol;
    rdof_t3cfg_default(&cfg, &pol);
    cfg.ncarriers = 2;
    cfg.carriers[0] = (rdof_t3_carrier_t){.tx_hz = TX_A, .rx_hz = RX_A, .chan = 1, .tscc = 1, .enabled = 1};
    cfg.carriers[1] = (rdof_t3_carrier_t){.tx_hz = TX_B, .rx_hz = RX_B, .chan = 2, .tscc = 0, .enabled = 1};
    S.u = calloc(1, rdof_t3unit_size());
    void *mem = aligned_alloc(8, (rdof_t3_site_size() + 7) / 8 * 8);
    const rdof_t3unit_io_t io = {no_publish, no_subscribe, say, NULL, rand32, NULL};
    char err[120];
    CHECK(rdof_t3unit_init(S.u, mem, &cfg, &pol, &io, NULL, NULL, -1, "test", err, sizeof err), "init: %s", err);

    const uint16_t chans[2] = {1, 2};
    for (int i = 0; i < NMS; i++) {
        const uint32_t tg = i < 2 ? 9 : 10;
        S.id[i] = 3100001 + (uint32_t)i;
        S.ms[i] = malloc(rdof_t3ms_size());
        rdof_t3ms_init(S.ms[i], S.id[i], &tg, 1, chans, 2, 21 + (uint32_t)i);
        rdof_t3ms_do(S.ms[i], RDOF_T3MS_DO_REGISTER, tg, 0, 0, T0 + 4300 + 400 * i);
    }
    rdof_t3sim_init(&S.sim, S.u, NMS, S.ms, S.id);
    S.sim.outside = 3; /* both carriers as their halves air them */
    S.sim.outbound = outbound;
    S.k_done = T0 / 30;
    rdof_t3unit_run(S.u, S.k_done, T0);

    /* both halves up, their settings, UTC */
    const uint32_t rx[2] = {RX_A, RX_B}, tx[2] = {TX_A, TX_B};
    for (int c = 0; c < 2; c++) {
        const rdof_t3split_io_t tio = {to_radio, rx_channel, now48, (void *)(intptr_t)c};
        CHECK(rdof_t3split_init(&S.ts[c], &tio, 1632000, c, rx[c], tx[c], cfg.cc, S.clock), "split %d init: %s", c,
              S.ts[c].error);
    }
    pump();
    for (int i = 0; i < 300 && (S.ts[0].state != RDOF_T3SPLIT_UP || S.ts[1].state != RDOF_T3SPLIT_UP); i++)
        run(0.01);
    for (int c = 0; c < 2; c++) {
        CHECK(S.ts[c].state == RDOF_T3SPLIT_UP, "split %d up: state %d %s", c, S.ts[c].state, S.ts[c].error);
        CHECK(S.f[c].freq == rx[c] && S.f[c].tx_freq == tx[c], "half %d on its carrier: %u / %u", c,
              (unsigned)S.f[c].freq, (unsigned)S.f[c].tx_freq);
        rdof_t3split_utc(&S.ts[c], UTC_MINUS_HOST_US, 10, S.clock);
    }
    pump();
    run(3.0);
    CHECK(rdof_split_has_cach(rdof_t3split_split(&S.ts[0])) && !rdof_split_has_cach(rdof_t3split_split(&S.ts[1])),
          "carrier A's half takes the CACH, carrier B's doesn't");
    for (int c = 0; c < 2; c++) {
        const rdof_split_t *sp = rdof_t3split_split(&S.ts[c]);
        CHECK(rdof_split_sched_ok(sp) && S.f[c].stream_mode == RDOF_OPS_STREAM_SCHEDULED && S.f[c].naligns > 0,
              "half %d: scheduled stream on UTC: align %s, stream %d, aligns %d", c,
              rdof_align_state_name(sp->align_state), S.f[c].stream_mode, S.f[c].naligns);
    }
    if (getenv("RDOF_TEST_VERBOSE"))
        printf("A: sched %u fifo %u keyups %u; half sent %u late %u early/full %u aired %d\n", S.ts[0].o.sched_bursts,
               S.ts[0].o.fifo_bursts, S.ts[0].o.keyups, S.f[0].sched_sent, S.f[0].sched_late,
               S.f[0].sched_early_full, S.f[0].non_air);
    CHECK(S.ts[0].o.keyed && !S.ts[1].o.keyed, "the TSCC's carrier keyed, carrier B quiet (%d, %d)", S.ts[0].o.keyed,
          S.ts[1].o.keyed);

    /* every slot of carrier A goes to its half, with a CACH, and the MSs register from them */
    const uint32_t a0 = S.aired[0], cach0 = S.cach;
    const int64_t k0 = S.k_done;
    run(4.0);
    const uint32_t slots = (uint32_t)(S.k_done - k0);
    CHECK(S.aired[0] - a0 >= slots - 1, "carrier A: every slot aired (%u of %u)", S.aired[0] - a0, slots);
    CHECK(S.cach - cach0 >= slots - 1 && S.cach_none == 0 && S.cach_bad == 0,
          "carrier A: a readable CACH with each (%u of %u, %u without, %u unreadable)", S.cach - cach0, slots,
          S.cach_none, S.cach_bad);
    for (int i = 0; i < NMS; i++)
        CHECK(ms_status(i).registered, "MS %d registered from the halves' air", i);

    /* tg 9's call on carrier A's payload slot, tg 10's on carrier B */
    rdof_t3ms_do(S.ms[0], RDOF_T3MS_DO_GROUP, 9, 4000, 0, utc_ms());
    run(0.6);
    rdof_t3ms_do(S.ms[2], RDOF_T3MS_DO_GROUP, 10, 3000, 0, utc_ms());
    run(2.0);
    const rdof_t3ms_status_t a1 = ms_status(1), b3 = ms_status(3);
    CHECK(a1.state == RDOF_T3MS_PAYLOAD && a1.ch == 1 && a1.heard_src == S.id[0], "tg 9 on carrier A (ch %d, %u)",
          a1.ch, (unsigned)a1.heard_src);
    CHECK(b3.state == RDOF_T3MS_PAYLOAD && b3.ch / 2 == 1 && b3.heard_src == S.id[2] && b3.heard_dst == 10,
          "tg 10 on carrier B (state %d, ch %d, %u -> %u)", b3.state, b3.ch, (unsigned)b3.heard_src,
          (unsigned)b3.heard_dst);
    run(12.0);
    CHECK(ms_status(0).calls_done == 1 && ms_status(2).calls_done == 1, "both calls done");
    CHECK(S.ts[0].o.keyed && !S.ts[1].o.keyed, "after the calls: carrier A keyed, carrier B unkeyed (%d, %d)",
          S.ts[0].o.keyed, S.ts[1].o.keyed);

    /* the CACHs as aired: the controller's AT (busy some of the time), TC and short LC */
    if (getenv("RDOF_TEST_VERBOSE"))
        printf("carrier A: %u CACHs, AT %u right (%u busy), TC %u right, short LCs %u of %u; carrier B: %u sched, "
               "%u with CACH\n",
               S.cach, S.at_ok, S.at_busy, S.tc_ok, S.slc_ok, S.slc_groups, S.ts[1].o.sched_bursts,
               S.ts[1].o.cach_bursts);
    CHECK(S.cach > 500 && S.at_ok == S.cach && S.at_busy > 0 && S.at_busy < S.cach,
          "AT as the controller's (%u of %u, %u busy)", S.at_ok, S.cach, S.at_busy);
    CHECK(S.tc_ok == S.cach, "TC the slot after (%u of %u)", S.tc_ok, S.cach);
    CHECK(S.slc_groups > 100 && S.slc_ok == S.slc_groups, "short LC the controller's (%u of %u)", S.slc_ok,
          S.slc_groups);
    CHECK(S.ts[0].o.cach_bursts == S.ts[0].o.sched_bursts && S.ts[1].o.cach_bursts == 0,
          "TX_SCHED with a CACH on carrier A only (%u of %u; B %u)", S.ts[0].o.cach_bursts, S.ts[0].o.sched_bursts,
          S.ts[1].o.cach_bursts);
    CHECK(S.f[0].sched_late == 0 && S.f[1].sched_late == 0, "none late (%u, %u)", S.f[0].sched_late,
          S.f[1].sched_late);

    for (int c = 0; c < 2; c++)
        rdof_t3split_free(&S.ts[c]);
    for (int i = 0; i < NMS; i++)
        free(S.ms[i]);
    free(mem);
    free(S.u);
    if (fails)
        printf("rdof_t3split2_tests: %d failed\n", fails);
    else
        printf("rdof_t3split2_tests: all passed\n");
    return fails != 0;
}
