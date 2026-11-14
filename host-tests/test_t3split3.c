// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* A one-carrier trunked site on two radios and no SDR (rdof/sdr/t3split.h, rdof_t3split_init_radios): a split TX
 * half sends the carrier's outbound (the TSCC on TS1 with its CACH slot by slot, the payload on TS2) and a split RX
 * half hears its inbound, each a fake radio (odr_split) with its own clock. The MSs' inbound goes to the RX half when
 * the slot is over; the split relays it labeled by the TX half's epoch, and each burst goes to the unit in its UTC
 * slot: from RX_META's slot number when the RX half has it, else from its label's parity and when it came. Both ways
 * the MSs register and call, and every burst lands in the slot it was sent in. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_radio.h"
#include "rdof/pdu.h"
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
#define RX_HZ 438250000u
#define TX_HZ 438750000u
#define NMS 3
#define INQ 64
#define REPORT_MS 8 /* the RX half's report this long after its slot ends */

enum { TXR = 0, RXR = 1 };

static struct {
    double clock;
    fake_radio_t f[2]; /* the TX half's radio, the RX half's */
    rdof_t3split_t ts;
    rdof_t3unit_t *u;
    rdof_t3sim_t sim;
    rdof_t3ms_t *ms[NMS];
    uint32_t id[NMS];
    int64_t k_done;
    struct {
        int64_t k;
        rdof_burst_t b;
    } in[INQ]; /* the MSs' bursts, to the RX half when their slot is over */
    int in_head, in_tail;
    int64_t sent_k[INQ]; /* the slots of the bursts the RX half heard, in order */
    int sent_head, sent_tail;
    uint32_t aired, cach, cach_none, at_ok, heard, slot_right, slot_wrong, counted, unwrapped_utc;
} S;

static void to_tx(void *ctx, const uint8_t *d, size_t n) { fake_write(&S.f[TXR], d, n); }
static void to_rx(void *ctx, const uint8_t *d, size_t n) { fake_write(&S.f[RXR], d, n); }
static int64_t utc_ms(void) { return T0 + llround((S.clock - 1000.0) * 1000.0); }
static double host_of_utc_ms(int64_t ms) { return 1000.0 + (double)(ms - T0) / 1000.0; }

/* The MMDVM control byte of a burst (rdof_t3split_burst's inverse). */
static uint8_t control_of(const rdof_burst_t *b)
{
    if (b->dt == RDOF_DT_VOICE_SYNC)
        return RDOF_SYNC_AUDIO;
    if (b->dt == RDOF_DT_VOICE)
        return (uint8_t)b->n;
    return (uint8_t)(RDOF_SYNC_DATA | b->dt);
}

/* What the RX half relayed, to the unit in its UTC slot. */
static void take(void)
{
    const int64_t k_now = utc_ms() / 30;
    int slot, meta;
    uint8_t control, burst[RDOF_BURST_BYTES];
    int rssi;
    uint32_t counter;
    while (rdof_t3split_take_at(&S.ts, &slot, &control, burst, &rssi, &meta, &counter)) {
        int64_t k;
        if (meta & RDOF_SPLIT_META_COUNTER) {
            k = rdof_t3split_unwrap(counter, k_now);
            S.counted++;
            S.unwrapped_utc += (meta & RDOF_SPLIT_META_UTC) != 0;
        } else {
            k = rdof_t3split_parity_slot(k_now - 1, slot);
        }
        S.heard++;
        if (S.sent_tail != S.sent_head) {
            const int64_t want = S.sent_k[S.sent_tail];
            S.sent_tail = (S.sent_tail + 1) % INQ;
            if (k == want)
                S.slot_right++;
            else {
                S.slot_wrong++;
                if (S.slot_wrong < 5)
                    printf("  burst of slot %lld taken as %lld (label %d, meta %d)\n", (long long)want,
                           (long long)k, slot, meta);
            }
        }
        rdof_burst_t b;
        rdof_t3split_burst(&S.ts, slot, control, burst, &b);
        rdof_t3unit_rx(S.u, 0, k, &b, rssi);
    }
}

static void pump(void)
{
    uint8_t buf[4096];
    size_t n;
    bool moved;
    do {
        moved = false;
        while ((n = fake_take(&S.f[TXR], buf, sizeof buf)) > 0)
            rdof_t3split_from_radio(&S.ts, buf, n, S.clock), moved = true;
        while ((n = fake_take(&S.f[RXR], buf, sizeof buf)) > 0)
            rdof_t3split_from_rx_radio(&S.ts, buf, n, S.clock), moved = true;
    } while (moved);
    take();
}

/* carrier 0's slot k as the TX half airs it (queued or gone out) */
static bool outbound(void *ctx, int c, int64_t k, uint8_t burst[RDOF_BURST_BYTES])
{
    const fake_radio_t *f = &S.f[TXR];
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
    S.aired++;
    int at = -1, slot = 0, lcss = -1;
    uint32_t p17 = 0;
    if (cach && rdof_t3rf_cach_read(cach, &at, &slot, &lcss, &p17)) {
        S.cach++;
        S.at_ok += at == (rdof_t3unit_at(S.u, k - 1) > 0);
    } else
        S.cach_none++;
    return true;
}

/* an MS's burst on carrier 0 in slot k: for the RX half once the slot is over */
static void inbound(void *ctx, int c, int64_t k, const rdof_burst_t *b)
{
    const int next = (S.in_head + 1) % INQ;
    if (next == S.in_tail)
        return;
    S.in[S.in_head].k = k;
    S.in[S.in_head].b = *b;
    S.in_head = next;
}

static void deliver(void)
{
    while (S.in_tail != S.in_head && utc_ms() >= (S.in[S.in_tail].k + 1) * 30 + REPORT_MS) {
        const int64_t k = S.in[S.in_tail].k;
        const rdof_burst_t *b = &S.in[S.in_tail].b;
        S.in_tail = (S.in_tail + 1) % INQ;
        const int before = S.f[RXR].out_len > 0;
        const size_t len0 = S.f[RXR].out_len;
        fake_hear_at(&S.f[RXR], control_of(b), b->burst, -70, host_of_utc_ms(k * 30));
        (void)before;
        if (S.f[RXR].out_len > len0) { /* heard (the RX half has an epoch) */
            S.sent_k[S.sent_head] = k;
            S.sent_head = (S.sent_head + 1) % INQ;
        }
    }
}

static void run(double seconds)
{
    const double end = S.clock + seconds;
    while (S.clock < end) {
        S.clock += 0.002;
        const int64_t k_now = utc_ms() / 30;
        deliver();
        pump();
        rdof_t3split_tick(&S.ts, S.clock);
        pump();
        if (S.ts.state == RDOF_T3SPLIT_UP) {
            rdof_t3unit_split_pump(S.u, &S.ts.o, rdof_t3split_split(&S.ts), k_now, S.clock);
            rdof_t3split_tick(&S.ts, S.clock);
            pump();
        }
        for (; S.k_done < k_now; S.k_done++) {
            const int64_t k = S.k_done + 1;
            rdof_t3sim_slot(&S.sim, k, k * 30);
            if (k % 10 == 0)
                rdof_t3unit_tick(S.u, k * 30);
        }
        if (S.f[TXR].non_air > 48) { /* keep the newest */
            memmove(S.f[TXR].on_air, S.f[TXR].on_air + 32, 16 * sizeof S.f[TXR].on_air[0]);
            S.f[TXR].non_air = 16;
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

static void site(bool meta)
{
    const char *name = meta ? "RX_META" : "parity";
    memset(&S, 0, sizeof S);
    S.clock = 1000.0;
    for (int r = 0; r < 2; r++) {
        fake_init(&S.f[r], "odr_split");
        S.f[r].clock = &S.clock;
    }
    S.f[TXR].clock_offset_us = 2345678.0, S.f[TXR].clock_rate = 12e-6;
    S.f[RXR].clock_offset_us = 987654.0, S.f[RXR].clock_rate = -9e-6;
    S.f[TXR].split_caps |= RDOF_OPS_CACH;
    S.f[TXR].roles = 1, S.f[RXR].roles = 2;
    if (meta)
        S.f[RXR].split_caps |= RDOF_OPS_RX_META;

    rdof_t3_cfg_t cfg;
    rdof_t3_policy_t pol;
    rdof_t3cfg_default(&cfg, &pol);
    cfg.ncarriers = 1;
    cfg.carriers[0] = (rdof_t3_carrier_t){.tx_hz = TX_HZ, .rx_hz = RX_HZ, .chan = 1, .tscc = 1, .enabled = 1};
    S.u = calloc(1, rdof_t3unit_size());
    void *mem = aligned_alloc(8, (rdof_t3_site_size() + 7) / 8 * 8);
    const rdof_t3unit_io_t io = {no_publish, no_subscribe, say, NULL, rand32, NULL};
    char err[120];
    CHECK(rdof_t3unit_init(S.u, mem, &cfg, &pol, &io, NULL, NULL, -1, "test", err, sizeof err), "%s: init: %s", name,
          err);

    const uint16_t chans[1] = {1};
    for (int i = 0; i < NMS; i++) {
        const uint32_t tg = 9;
        S.id[i] = 3100001 + (uint32_t)i;
        S.ms[i] = malloc(rdof_t3ms_size());
        rdof_t3ms_init(S.ms[i], S.id[i], &tg, 1, chans, 1, 31 + (uint32_t)i);
        rdof_t3ms_do(S.ms[i], RDOF_T3MS_DO_REGISTER, tg, 0, 0, T0 + 4300 + 400 * i);
    }
    rdof_t3sim_init(&S.sim, S.u, NMS, S.ms, S.id);
    S.sim.outside = 1; /* the outbound as the TX half airs it */
    S.sim.outbound = outbound;
    S.sim.outside_in = 1; /* the inbound through the RX half */
    S.sim.inbound = inbound;
    S.k_done = T0 / 30;
    rdof_t3unit_run(S.u, S.k_done, T0);

    const rdof_t3split_io_t tio = {to_tx, NULL, NULL, NULL, to_rx};
    CHECK(rdof_t3split_init_radios(&S.ts, &tio, 0, RX_HZ, TX_HZ, cfg.cc, S.clock), "%s: init: %s", name,
          S.ts.error);
    pump();
    for (int i = 0; i < 1500 && S.ts.state != RDOF_T3SPLIT_UP; i++)
        run(0.002);
    CHECK(S.ts.state == RDOF_T3SPLIT_UP, "%s: split up: state %d %s", name, S.ts.state, S.ts.error);
    for (int r = 0; r < 2; r++)
        CHECK(S.f[r].freq == RX_HZ && S.f[r].tx_freq == TX_HZ, "%s: radio %d on the carrier: %u / %u", name, r,
              (unsigned)S.f[r].freq, (unsigned)S.f[r].tx_freq);
    CHECK(S.f[TXR].role == 1 && S.f[RXR].role == 2, "%s: roles %d %d", name, S.f[TXR].role, S.f[RXR].role);
    CHECK(S.f[RXR].rx_meta == meta, "%s: RX_META asked only of a half that has it (%d)", name, S.f[RXR].rx_meta);
    rdof_t3split_utc(&S.ts, UTC_MINUS_HOST_US, 10, S.clock);
    pump();
    run(3.0);
    const rdof_split_t *sp = rdof_t3split_split(&S.ts);
    CHECK(rdof_split_sched_ok(sp) && rdof_split_has_cach(sp), "%s: scheduled on UTC with the CACH (align %s)", name,
          rdof_align_state_name(sp->align_state));
    CHECK(S.f[RXR].has_epoch, "%s: the RX half has the TX half's epoch", name);

    run(4.0);
    for (int i = 0; i < NMS; i++)
        CHECK(ms_status(i).registered, "%s: MS %d registered through the two radios", name, i);

    rdof_t3ms_do(S.ms[0], RDOF_T3MS_DO_GROUP, 9, 3000, 0, utc_ms());
    run(1.5);
    const rdof_t3ms_status_t a1 = ms_status(1);
    CHECK(a1.state == RDOF_T3MS_PAYLOAD && a1.ch == 1 && a1.heard_src == S.id[0],
          "%s: the call on the payload slot (state %d ch %d, %u)", name, a1.state, a1.ch, (unsigned)a1.heard_src);
    run(10.0);
    CHECK(ms_status(0).calls_done == 1, "%s: call done", name);

    if (getenv("RDOF_TEST_VERBOSE"))
        printf("%s: aired %u, CACH %u (AT right %u, none %u); heard %u (%u counted, %u UTC), slot right %u wrong %u; "
               "events %d, split metas %u counted %u\n",
               name, S.aired, S.cach, S.at_ok, S.cach_none, S.heard, S.counted, S.unwrapped_utc, S.slot_right,
               S.slot_wrong, S.f[RXR].metas, sp->meta_events, sp->meta_counted);
    CHECK(S.heard > 40, "%s: the RX half's bursts reached the unit (%u)", name, S.heard);
    CHECK(S.slot_wrong == 0 && S.slot_right == S.heard, "%s: each in the slot it was sent in (%u of %u)", name,
          S.slot_right, S.heard);
    if (meta)
        CHECK(S.counted == S.heard && S.unwrapped_utc == S.heard,
              "%s: every burst's slot from RX_META, on UTC (%u, %u of %u)", name, S.counted, S.unwrapped_utc, S.heard);
    else
        CHECK(S.counted == 0 && S.f[RXR].metas == 0, "%s: no RX_META (%u)", name, S.counted);
    CHECK(S.cach > 300 && S.at_ok == S.cach && S.cach_none == 0, "%s: the TSCC's CACH each slot (%u, AT %u, %u none)",
          name, S.cach, S.at_ok, S.cach_none);
    CHECK(S.f[TXR].sched_late == 0, "%s: none late (%u)", name, S.f[TXR].sched_late);

    rdof_t3split_free(&S.ts);
    for (int i = 0; i < NMS; i++)
        free(S.ms[i]);
    free(mem);
    free(S.u);
}

int main(void)
{
    site(true);
    site(false);
    if (fails)
        printf("rdof_t3split3_tests: %d failed\n", fails);
    else
        printf("rdof_t3split3_tests: all passed\n");
    return fails != 0;
}
