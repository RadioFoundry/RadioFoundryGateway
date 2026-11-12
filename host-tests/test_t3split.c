// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* A two-carrier trunked site (rdof/t3unit.h): carrier A (the TSCC) as the SDR repeater carries it, carrier B on a
 * split TX half (rdof/sdr/t3split.h) with the SDR's receiver as its RX half. The TX half is a fake radio
 * (odr_split) with its own clock: it comes up, takes the carrier's settings, aligns its slot counter to UTC and takes
 * the scheduled stream. A group call granted on carrier B goes out as TX_SCHED for the trunk's slots (the UTC slot
 * numbers), and an MS on carrier B follows it from what the half airs; a burst the receiver hears on carrier B's
 * inbound comes back up labeled with its slot; the carrier unkeys after the call. */
#include <math.h>
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_radio.h"
#include "rdof/pdu.h"
#include "rdof/sdr/t3split.h"
#include "rdof/split.h"
#include "rdof/splitops.h"
#include "rdof/t3cfg.h"
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

#define LAYOUT(t) {#t, sizeof(t), alignof(t)}

/* The Rust's mirrors (rust/src/t3split.rs) have the C's size and alignment. */
static void test_layouts(void)
{
    static const rdof_t3rf_layout_t c[] = {
        LAYOUT(rdof_t3split_io_t), LAYOUT(rdof_t3split_ops_t),  LAYOUT(rdof_sdrsplits_io_t),
        LAYOUT(rdof_sdrsplits_t),  LAYOUT(rdof_split_params_t), LAYOUT(rdof_t3split_t),
    };
    const size_t n = sizeof c / sizeof c[0];
    size_t i = 0;
    for (const rdof_t3rf_layout_t *r; (r = rdof_t3split_layout(i)) && r->name; i++) {
        CHECK(i < n, "rust lists %s, C doesn't", r->name);
        if (i >= n)
            break;
        CHECK(strcmp(r->name, c[i].name) == 0, "%zu: rust %s, C %s", i, r->name, c[i].name);
        CHECK(r->size == c[i].size && r->align == c[i].align, "%s: rust %zu/%zu, C %zu/%zu", c[i].name, r->size,
              r->align, c[i].size, c[i].align);
    }
    CHECK(i == n, "rust lists %zu structs, C %zu", i, n);
}

#define T0 1793912651000LL /* UTC ms at host time 1000 s */
#define UTC_MINUS_HOST_US ((double)T0 * 1000.0 - 1000e6)
#define RX_A 438250000u
#define TX_A 438750000u
#define RX_B 438275000u
#define TX_B 438775000u
#define NMS 4

static struct {
    double clock;
    fake_radio_t f;
    rdof_t3split_t ts;
    rdof_t3unit_t *u;
    rdof_t3sim_t sim;
    rdof_t3ms_t *ms[NMS];
    uint32_t id[NMS];
    int64_t k_done;
    uint32_t aired_b; /* carrier B's bursts an MS read from the half */
} S;

static void to_radio(void *ctx, const uint8_t *d, size_t n) { fake_write(&S.f, d, n); }
static int rx_channel(void *ctx, uint32_t hz) { return hz == RX_B ? 1 : hz == RX_A ? 0 : -1; }
static int64_t now48(void *ctx) { return llround(S.clock * 48000.0); }
static int64_t utc_ms(void) { return T0 + llround((S.clock - 1000.0) * 1000.0); }

static void pump(void)
{
    uint8_t buf[4096];
    size_t n;
    while ((n = fake_take(&S.f, buf, sizeof buf)) > 0)
        rdof_t3split_from_radio(&S.ts, buf, n, S.clock);
}

/* carrier B's slot k as the half airs it (queued or gone out) */
static bool outbound(void *ctx, int c, int64_t k, uint8_t burst[RDOF_BURST_BYTES])
{
    const uint32_t n = (uint32_t)k;
    for (int i = 0; i < (int)(sizeof S.f.sched / sizeof S.f.sched[0]); i++)
        if (S.f.sched[i].used && S.f.sched[i].counter == n) {
            memcpy(burst, S.f.sched[i].burst, RDOF_BURST_BYTES);
            S.aired_b++;
            return true;
        }
    for (int i = S.f.non_air - 1; i >= 0; i--)
        if (S.f.on_air[i].counter == n) {
            memcpy(burst, S.f.on_air[i].burst, RDOF_BURST_BYTES);
            S.aired_b++;
            return true;
        }
    return false;
}

static void run(double seconds)
{
    const double end = S.clock + seconds;
    while (S.clock < end) {
        S.clock += 0.01;
        const int64_t k_now = utc_ms() / 30;
        rdof_t3split_tick(&S.ts, S.clock);
        pump();
        if (S.ts.state == RDOF_T3SPLIT_UP) {
            rdof_t3unit_split_pump(S.u, &S.ts.o, rdof_t3split_split(&S.ts), k_now, S.clock);
            pump();
        }
        for (; S.k_done < k_now; S.k_done++) {
            const int64_t k = S.k_done + 1;
            rdof_t3sim_slot(&S.sim, k, k * 30);
            if (k % 10 == 0)
                rdof_t3unit_tick(S.u, k * 30);
        }
        if (S.f.non_air > 48) { /* keep the newest */
            memmove(S.f.on_air, S.f.on_air + 32, 16 * sizeof S.f.on_air[0]);
            S.f.non_air = 16;
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
static uint32_t rng = 777;
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

/* host time of the next inbound slot `slot` start on the half's grid, at least ahead s from now */
static double slot_time(int slot, double ahead)
{
    const fake_radio_t *t = &S.f;
    double b;
    const long k = fake_slot_boundary(t, fake_modem_f(t), &b);
    if ((t->counter_base + (uint32_t)k) % 2)
        b -= t->slot_period_us;
    const double e = (b - t->clock_offset_us) / (1 + t->clock_rate) / 1e6;
    const double slot_s = t->slot_period_us / 1e6 / (1 + t->clock_rate);
    const double m = ceil((S.clock + ahead - e) / (2 * slot_s));
    return e + (2 * m + (slot == 2)) * slot_s + 0.00125;
}

int main(void)
{
    test_layouts();
    memset(&S, 0, sizeof S);
    S.clock = 1000.0;
    fake_init(&S.f, "odr_split");
    S.f.clock = &S.clock;
    S.f.clock_offset_us = 2345678.0, S.f.clock_rate = 12e-6;

    /* the site: carrier A the TSCC, carrier B a payload carrier */
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

    /* two MSs on talkgroup 9, two on 10 */
    const uint16_t chans[2] = {1, 2};
    for (int i = 0; i < NMS; i++) {
        const uint32_t tg = i < 2 ? 9 : 10;
        S.id[i] = 3100001 + (uint32_t)i;
        S.ms[i] = malloc(rdof_t3ms_size());
        rdof_t3ms_init(S.ms[i], S.id[i], &tg, 1, chans, 2, 11 + (uint32_t)i);
        rdof_t3ms_do(S.ms[i], RDOF_T3MS_DO_REGISTER, tg, 0, 0, T0 + 300 + 400 * i);
    }
    rdof_t3sim_init(&S.sim, S.u, NMS, S.ms, S.id);
    S.sim.outside = 1u << 1;
    S.sim.outbound = outbound;
    S.k_done = T0 / 30;
    rdof_t3unit_run(S.u, S.k_done, T0);

    /* carrier B's split: bring-up, its settings, UTC */
    const rdof_t3split_io_t tio = {to_radio, rx_channel, now48, NULL};
    CHECK(rdof_t3split_init(&S.ts, &tio, 1632000, 1, RX_B, TX_B, cfg.cc, S.clock), "split init: %s", S.ts.error);
    pump();
    for (int i = 0; i < 300 && S.ts.state != RDOF_T3SPLIT_UP && S.ts.state != RDOF_T3SPLIT_FAILED; i++)
        run(0.01);
    CHECK(S.ts.state == RDOF_T3SPLIT_UP, "the split up: state %d %s", S.ts.state, S.ts.error);
    CHECK(S.f.freq == RX_B && S.f.tx_freq == TX_B && S.f.cc == cfg.cc, "the half on carrier B: %u / %u cc %d",
          (unsigned)S.f.freq, (unsigned)S.f.tx_freq, S.f.cc);
    rdof_t3split_utc(&S.ts, UTC_MINUS_HOST_US, 10, S.clock);
    pump();
    run(3.0);
    const rdof_split_t *sp = rdof_t3split_split(&S.ts);
    CHECK(rdof_split_sched_ok(sp) && S.f.stream_mode == RDOF_OPS_STREAM_SCHEDULED && S.f.naligns > 0,
          "scheduled stream on UTC: align %s, stream %d, aligns %d", rdof_align_state_name(sp->align_state),
          S.f.stream_mode, S.f.naligns);
    CHECK(!S.ts.o.keyed && S.f.non_air == 0, "carrier B quiet while idle (keyed %d, aired %d)", S.ts.o.keyed,
          S.f.non_air);
    for (int i = 0; i < NMS; i++)
        CHECK(ms_status(i).registered, "MS %d registered", i);

    /* tg 9's call takes carrier A's payload slot, tg 10's goes to carrier B */
    rdof_t3ms_do(S.ms[0], RDOF_T3MS_DO_GROUP, 9, 4000, 0, utc_ms());
    run(0.6);
    rdof_t3ms_do(S.ms[2], RDOF_T3MS_DO_GROUP, 10, 3000, 0, utc_ms());
    run(2.0);
    const rdof_t3ms_status_t a1 = ms_status(1), b3 = ms_status(3);
    CHECK(a1.state == RDOF_T3MS_PAYLOAD && a1.ch == 1 && a1.heard_src == S.id[0], "tg 9 on carrier A (ch %d, %u)",
          a1.ch, (unsigned)a1.heard_src);
    CHECK(b3.state == RDOF_T3MS_PAYLOAD && b3.ch / 2 == 1 && b3.heard_src == S.id[2] && b3.heard_dst == 10,
          "tg 10 on carrier B, followed from the half's air (state %d, ch %d, %u -> %u)", b3.state, b3.ch,
          (unsigned)b3.heard_src, (unsigned)b3.heard_dst);
    CHECK(S.ts.o.keyed && S.ts.o.scheduled && S.ts.o.sched_bursts > 20 && S.ts.o.fifo_bursts == 0 && S.aired_b > 20,
          "carrier B keyed, TX_SCHED only (%u sched, %u fifo, %u read)", S.ts.o.sched_bursts, S.ts.o.fifo_bursts,
          S.aired_b);
    if (getenv("RDOF_TEST_VERBOSE"))
        printf("carrier B: %u sched, %u read, %u keyups, %u short LCs, half sent %u, aired %d; tg 10 MS ch %d\n",
               S.ts.o.sched_bursts, S.aired_b, S.ts.o.keyups, S.ts.o.short_lcs, S.f.sched_sent, S.f.non_air, b3.ch);
    CHECK(S.f.sched_late == 0, "none late at the half (%u)", S.f.sched_late);
    /* each aired burst at the start of its UTC slot */
    int off = 0;
    for (int i = 0; i < S.f.non_air; i++) {
        const double utc_us = (S.f.on_air[i].start - S.f.clock_offset_us) / (1 + S.f.clock_rate) + UTC_MINUS_HOST_US;
        const double want = fmod((double)S.f.on_air[i].counter - fmod(utc_us / 30000.0, 4294967296.0), 4294967296.0);
        off += fabs(want) > 0.01 && fabs(want - 4294967296.0) > 0.01;
    }
    CHECK(S.f.non_air > 0 && off == 0, "the half's counter is the UTC slot (%d of %d off)", off, S.f.non_air);

    /* the receiver hears a CSBK on carrier B's inbound TS2: up labeled slot 2 */
    while (rdof_t3split_take(&S.ts, &(int){0}, &(uint8_t){0}, (uint8_t[RDOF_BURST_BYTES]){0}, NULL))
        ;
    const double t = slot_time(2, 0.05);
    run(t - S.clock);
    rdof_sdr_burst_t hb;
    memset(&hb, 0, sizeof hb);
    hb.b.slot = 1;
    hb.b.dt = RDOF_DT_CSBK;
    hb.b.family = RDOF_FAMILY_MS;
    hb.position = llround(t * 48000.0);
    rdof_t3split_heard(&S.ts, 1, &hb);
    run(0.1);
    int slot = 0;
    uint8_t control = 0, burst[RDOF_BURST_BYTES];
    const bool got = rdof_t3split_take(&S.ts, &slot, &control, burst, NULL);
    rdof_burst_t rb;
    if (got)
        rdof_t3split_burst(&S.ts, slot, control, burst, &rb);
    CHECK(got && slot == 2 && rb.dt == RDOF_DT_CSBK && rb.slot == 2 && rb.family == RDOF_FAMILY_MS,
          "heard on carrier B: got %d slot %d control 0x%02x", got, slot, control);

    /* a heard voice burst's control byte back to the burst: sync, or its number */
    rdof_burst_t vb;
    rdof_t3split_burst(&S.ts, 1, RDOF_SYNC_AUDIO, burst, &vb);
    CHECK(vb.dt == RDOF_DT_VOICE_SYNC && vb.n == 0 && vb.slot == 1 && vb.cc == cfg.cc && vb.family == RDOF_FAMILY_MS,
          "voice sync: dt 0x%02x n %d", vb.dt, vb.n);
    rdof_t3split_burst(&S.ts, 2, 3, burst, &vb);
    CHECK(vb.dt == RDOF_DT_VOICE && vb.n == 3 && vb.slot == 2, "voice burst D: dt 0x%02x n %d", vb.dt, vb.n);

    /* after the calls and the hang: carrier B unkeyed */
    run(12.0);
    CHECK(ms_status(2).calls_done == 1 && ms_status(0).calls_done == 1, "both calls done");
    CHECK(!S.ts.o.keyed && S.ts.o.keyups == 1, "carrier B unkeyed after the hang (keyed %d, keyups %u)", S.ts.o.keyed,
          S.ts.o.keyups);

    rdof_t3split_free(&S.ts);
    for (int i = 0; i < NMS; i++)
        free(S.ms[i]);
    free(mem);
    free(S.u);
    if (fails)
        printf("rdof_t3split_tests: %d failed\n", fails);
    else
        printf("rdof_t3split_tests: all passed\n");
    return fails != 0;
}
