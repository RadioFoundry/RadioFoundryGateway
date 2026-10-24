// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The split duplex core (rdof/split.h) on its own: bring-up against two fake halves, refusals, a half going away,
 * and the clock filter. The repeater on a split is in test_repeater.c (the *_split scenarios). */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "rdof/features.h"

#if RDOF_FEATURE_REPEATER

#define CHECK(cond, ...)                                                                                     \
    do {                                                                                                     \
        checks++;                                                                                            \
        if (!(cond)) {                                                                                       \
            failures++;                                                                                      \
            printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                           \
            printf(__VA_ARGS__);                                                                             \
            printf("\n");                                                                                    \
        }                                                                                                    \
    } while (0)

typedef struct {
    double clock;
    fake_radio_t half[2];
    rdof_split_t s;
    uint8_t up[8192];
    size_t up_len;
} pair_t;

static void pump(pair_t *p)
{
    uint8_t buf[4096];
    size_t n;
    bool moved = true;
    while (moved) {
        moved = false;
        for (int i = 0; i < 2; i++) {
            if ((n = rdof_split_take(&p->s, i, buf, sizeof buf)) > 0) {
                fake_write(&p->half[i], buf, n);
                moved = true;
            }
            while ((n = fake_take(&p->half[i], buf, sizeof buf)) > 0) {
                rdof_split_from_half(&p->s, i, buf, n, p->clock);
                moved = true;
            }
        }
    }
    p->up_len += rdof_split_take(&p->s, -1, p->up + p->up_len, sizeof p->up - p->up_len);
}

static void pair_init(pair_t *p, const char *tx_kind, const char *rx_kind)
{
    memset(p, 0, sizeof *p);
    p->clock = 1000.0;
    fake_init(&p->half[0], tx_kind);
    fake_init(&p->half[1], rx_kind);
    for (int i = 0; i < 2; i++)
        p->half[i].clock = &p->clock;
    p->half[0].clock_offset_us = 1234567, p->half[0].clock_rate = 20e-6;
    p->half[1].clock_offset_us = 4000000000.0, p->half[1].clock_rate = -15e-6;
    rdof_split_init(&p->s, p->clock, NULL);
    pump(p);
}

static void run(pair_t *p, double seconds)
{
    for (int k = 0; k < (int)lround(seconds / 0.02); k++) {
        p->clock += 0.02;
        rdof_split_tick(&p->s, p->clock);
        pump(p);
    }
}

static void test_bring_up(void)
{
    static pair_t p;
    pair_init(&p, "odr_split", "odr_split");
    CHECK(p.s.ready && p.s.epoch_ok, "ready %d epoch %d (%s)", p.s.ready, p.s.epoch_ok, p.s.failed);
    CHECK(p.half[0].role == 1 && p.half[1].role == 2, "roles %d %d", p.half[0].role, p.half[1].role);
    CHECK(p.half[1].has_epoch, "the RX half has an epoch");
    uint8_t ask[3] = {0xE0, 0x03, RDOF_MMDVM_GET_VERSION};
    rdof_split_upstream(&p.s, ask, 3, p.clock);
    pump(&p);
    char desc[64];
    CHECK(p.up_len > 4 && p.up[2] == RDOF_MMDVM_GET_VERSION, "version reply");
    rdof_mmdvm_parse_version(p.up + 3, p.up_len - 3, desc, sizeof desc);
    CHECK(!strcmp(desc, RDOF_SPLIT_DESCRIPTION) && rdof_radio_detect(desc) == RDOF_RADIO_SPLIT, "%s", desc);
    /* the epoch: the TX half's slot start now, in the RX half's clock, to the microsecond */
    run(&p, 40);
    uint32_t before = p.half[1].epoch_t_us;
    p.s.epoch_busy = false;
    p.s.has_epoch_due = true;
    p.s.epoch_due = p.clock;
    rdof_split_tick(&p.s, p.clock);
    pump(&p);
    double b;
    long k = fake_slot_boundary(&p.half[0], fake_modem_f(&p.half[0]), &b); /* the TX half's latest; TS1 if even */
    b += (p.half[0].counter_base + (uint32_t)k) % 2 ? 30000 : 0;
    double host = (b - 1234567) / (1 + 20e-6);
    uint32_t want = (uint32_t)(uint64_t)(int64_t)rint(host * (1 - 15e-6) + 4000000000.0);
    CHECK(fabs(rdof_frame_rem(p.half[1].epoch_t_us, want, p.half[1].epoch_period)) <= 2, "epoch %u want %u (%u)",
          (unsigned)p.half[1].epoch_t_us, (unsigned)want, (unsigned)before);
    double ratio = (1 - 15e-6) / (1 + 20e-6);
    CHECK(fabs(p.half[1].epoch_period - 30e6 * ratio) < 300, "period %u", (unsigned)p.half[1].epoch_period);
}

static void test_refusals(void)
{
    static pair_t p;
    pair_init(&p, "odr_split", "odr");
    CHECK(!p.s.ready && strstr(p.s.failed, "RX half") && strstr(p.s.failed, "SPLIT1"), "%s", p.s.failed);
    pair_init(&p, "mmdvm_hs", "odr_split");
    CHECK(!p.s.ready && strstr(p.s.failed, "TX half"), "%s", p.s.failed);
}

/* Any half offering the RX role with any half offering the TX role (INFO's roles); a modem
 * without the extension can't be either half. */
static void test_role_matrix(void)
{
    static pair_t p;
    for (int tr = 1; tr <= 3; tr++)
        for (int rr = 1; rr <= 3; rr++) {
            memset(&p, 0, sizeof p);
            p.clock = 1000.0;
            fake_init(&p.half[0], "odr_split");
            fake_init(&p.half[1], "odr_split");
            for (int i = 0; i < 2; i++)
                p.half[i].clock = &p.clock;
            p.half[0].roles = tr, p.half[1].roles = rr;
            rdof_split_init(&p.s, p.clock, NULL);
            pump(&p);
            if ((tr & 1) && (rr & 2))
                CHECK(p.s.ready && p.half[0].role == 1 && p.half[1].role == 2, "roles %d/%d: %s", tr, rr, p.s.failed);
            else
                CHECK(!p.s.ready && strstr(p.s.failed, (tr & 1) ? "RX half" : "TX half") &&
                          strstr(p.s.failed, "can't take that role"),
                      "roles %d/%d: %s", tr, rr, p.s.failed);
        }
    static const char *const plain[] = {"odr", "opengd77", "mmdvm_hs", "mmdvm", "odr_sfr"};
    for (size_t k = 0; k < sizeof plain / sizeof plain[0]; k++)
        for (int half = 0; half < 2; half++) {
            pair_init(&p, half ? "odr_split" : plain[k], half ? plain[k] : "odr_split");
            CHECK(!p.s.ready && strstr(p.s.failed, half ? "RX half" : "TX half") && strstr(p.s.failed, "SPLIT1"),
                  "%s as the %s half: %s", plain[k], half ? "RX" : "TX", p.s.failed);
        }
}

static void test_dead_half(void)
{
    static pair_t p;
    pair_init(&p, "odr_split", "odr_split");
    p.clock += 5; /* a long gap between ticks isn't a dead half */
    rdof_split_tick(&p.s, p.clock);
    pump(&p);
    run(&p, 1);
    CHECK(!p.s.failed[0], "%s", p.s.failed);
    /* a half that stops answering: three requests in a row */
    p.half[1].silent = true;
    run(&p, 6);
    CHECK(strstr(p.s.failed, "RX half") != NULL, "failed: %s", p.s.failed);
}

static void test_clock_sync(void)
{
    rdof_clock_sync_t c;
    rdof_clock_sync_init(&c);
    double off = 4294967296.0 - 3000000.0, rate = 50e-6;
    for (int k = 0; k < 400; k++) {
        double h = 1e9 + k * 250000.0;
        uint32_t m = (uint32_t)(uint64_t)(int64_t)rint(h * (1 + rate) + off);
        rdof_clock_sync_add(&c, (int64_t)h - 100, (int64_t)h + 100, m, m);
    }
    double r, icpt, h0;
    rdof_clock_sync_fit(&c, &r, &icpt, &h0);
    CHECK(fabs(r - rate) < 1e-7, "rate %g", r);
    double h = 1e9 + 400 * 250000.0;
    uint32_t want = (uint32_t)(uint64_t)(int64_t)rint(h * (1 + rate) + off);
    CHECK(abs(rdof_wrap_diff(rdof_clock_sync_to_modem(&c, h), want)) <= 2, "to_modem");
    CHECK(rdof_wrap_diff(5, 0xFFFFFFFBu) == 10 && rdof_wrap_diff(0xFFFFFFFBu, 5) == -10, "wrap_diff");
}

/* split-duplex.md 6.3's trust rules (RadioFoundry PR #109) */
static void test_relay(void)
{
    rdof_epoch_relay_t r;
    rdof_epoch_relay_init(&r, RDOF_RELAY_LIMIT_US, RDOF_RELAY_AGREE_US);
    CHECK(rdof_epoch_relay_decide(&r, 1000000, 30000000, 100), "first");
    CHECK(rdof_epoch_relay_decide(&r, 1000000 + 60000 * 500 + 3000, 30000000, 100), "agrees by whole frames");
    CHECK(!rdof_epoch_relay_decide(&r, 5, 30000000, 10001) && r.skipped == 1, "over the limit");
    rdof_epoch_relay_init(&r, RDOF_RELAY_LIMIT_US, RDOF_RELAY_AGREE_US);
    rdof_epoch_relay_decide(&r, 0, 30000000, 10);
    CHECK(!rdof_epoch_relay_decide(&r, 30000, 30000000, 10), "a slot off: held");
    CHECK(!rdof_epoch_relay_decide(&r, 30000 + 60000 * 3, 30000000, 10), "held");
    CHECK(rdof_epoch_relay_decide(&r, 30000 + 60000 * 9 + 50, 30000000, 10) && r.sent == 30000 + 60000 * 9 + 50 &&
              r.log[(r.decisions - 1) % RDOF_RELAY_LOG].action == RDOF_RELAY_REPLACED,
          "three that agree replace it");
    rdof_epoch_relay_init(&r, RDOF_RELAY_LIMIT_US, RDOF_RELAY_AGREE_US);
    rdof_epoch_relay_decide(&r, 0, 30000000, 10);
    rdof_epoch_relay_decide(&r, 20000, 30000000, 10);
    rdof_epoch_relay_decide(&r, 40000, 30000000, 10);
    CHECK(r.nheld == 1 && r.held[0] == 40000, "held ones that disagree start over");
    CHECK(fabs(rdof_frame_rem(100, 0xFFFFFFFFu - 59899, 30000000)) < 1e-9, "frame_rem wraps");
    rdof_clock_sync_t c;
    rdof_clock_sync_init(&c);
    CHECK(isinf(rdof_clock_sync_bound_us(&c)), "no samples: no bound");
    for (int i = 0; i < 3; i++)
        rdof_clock_sync_add(&c, i * 1000000, i * 1000000 + 800, 5, 5);
    CHECK(rdof_clock_sync_bound_us(&c) == 400, "bound %g", rdof_clock_sync_bound_us(&c));
    int64_t n = 59666666700LL + 4294967296LL - 3;
    CHECK(rdof_n_of_counter(rdof_slot_counter(n), n + 1000) == n && rdof_n_of_counter(rdof_slot_counter(n) + 5, n) == n + 5,
          "counters");
    uint8_t out[64], burst[33] = {0};
    CHECK(rdof_ops_schedule(out, 3, 1, 0x41, burst) == 0 && rdof_ops_schedule(out, 3, 2, 0x41, burst) == 39, "parity");
    const uint8_t info[16] = {1, 3, 0, 16, 1, 0, 0, 0, 0, 0, 0x3F, 0x00, 55, 32, 0xF8, 0x11}; /* 4600 µs */
    CHECK(rdof_ops_caps(info, 16) == (RDOF_OPS_ALIGN | RDOF_OPS_SCHEDULE) && rdof_ops_caps(info, 10) == 0 &&
              rdof_ops_guard_ms(info, 16) == 55 && rdof_ops_horizon(info, 16) == 32 &&
              rdof_ops_air_delay_us(info, 16) == 4600 && rdof_ops_air_delay_us(info, 14) == 0 && rdof_ops_guard_ms(info, 12) == RDOF_OPS_GUARD_MS,
          "INFO: the built caps only (reserved bits off), guard, horizon, air delay");
    uint8_t st[RDOF_OPS_STATUS_LEN] = {0};
    rdof_ops_status_t a = {1, 2, 3, 4, 5, -1234567, 0x21}, b;
    rdof_ops_put_status_ext(st, &a);
    CHECK(rdof_ops_status_ext(st, sizeof st, &b) && b.sent == 1 && b.late == 2 && b.early_full == 3 && b.replaced == 4 &&
              b.cooling == 5 && b.phase_us == -1234567 && b.flags == 0x21 && st[38] == 4 && st[40] == 5 &&
              st[46] == 0x21 && !rdof_ops_status_ext(st, 46, &b),
          "STATUS 32-47");
}

#define UTC_MINUS_HOST_US (1790000000000000.0 - 1000000000.0) /* the fakes' host clock starts at 1000 s */

static void pair_utc(pair_t *p, int offset_us)
{
    memset(p, 0, sizeof *p);
    p->clock = 1000.0;
    fake_init(&p->half[0], "odr_split");
    fake_init(&p->half[1], "odr_split");
    for (int i = 0; i < 2; i++)
        p->half[i].clock = &p->clock;
    p->half[0].clock_offset_us = 1234567, p->half[0].clock_rate = 20e-6;
    p->half[1].clock_offset_us = 4000000000.0, p->half[1].clock_rate = -15e-6;
    rdof_split_init(&p->s, p->clock, NULL);
    rdof_split_set_utc(&p->s, true, UTC_MINUS_HOST_US, 10, offset_us, true, p->clock);
    pump(p);
}

/* The TX half's latest boundary in UTC µs (the true clocks), its counter. */
static double tx_utc(const pair_t *p, uint32_t *counter)
{
    double b;
    long k = fake_slot_boundary(&p->half[0], fake_modem_f(&p->half[0]), &b);
    *counter = p->half[0].counter_base + (uint32_t)k;
    return (b - 1234567) / (1 + 20e-6) + UTC_MINUS_HOST_US;
}

/* On the grid moved by off_us, to 5 µs, TS1 even. */
static bool on_grid(const pair_t *p, double off_us)
{
    uint32_t c;
    double u = tx_utc(p, &c) + off_us, ph = fmod(u, 30000);
    long long n = llround(u / 30000);
    return fmin(ph, 30000 - ph) < 5 && c == rdof_slot_counter(n);
}

static void up_cmd(pair_t *p, uint8_t cmd, const uint8_t *body, size_t n)
{
    uint8_t fr[64];
    size_t fl = rdof_mmdvm_frame(cmd, body, n, fr, sizeof fr);
    rdof_split_upstream(&p->s, fr, fl, p->clock);
    pump(p);
}

static void wait_align(pair_t *p)
{
    int before = p->half[0].naligns;
    while (p->half[0].naligns == before)
        run(p, 0.02);
}

static void pair_utc_with(pair_t *p, void (*before)(fake_radio_t *tx))
{
    memset(p, 0, sizeof *p);
    p->clock = 1000.0;
    fake_init(&p->half[0], "odr_split");
    fake_init(&p->half[1], "odr_split");
    for (int i = 0; i < 2; i++)
        p->half[i].clock = &p->clock;
    p->half[0].clock_offset_us = 1234567, p->half[0].clock_rate = 20e-6;
    p->half[1].clock_offset_us = 4000000000.0, p->half[1].clock_rate = -15e-6;
    before(&p->half[0]);
    rdof_split_init(&p->s, p->clock, NULL);
    pump(p);
    rdof_split_set_utc(&p->s, true, UTC_MINUS_HOST_US, 10, 0, true, p->clock);
    pump(p);
}

static void air_delay_4600(fake_radio_t *tx) { tx->air_delay_us = 4600; }
static void slow_snap(fake_radio_t *tx) { tx->snap_slots = 6; }
static void snap_off(fake_radio_t *tx) { tx->snap_offset_us = 1500; }
static void no_caps(fake_radio_t *tx) { tx->no_caps = true; }

static void test_align(void)
{
    static pair_t p;
    pair_utc(&p, 0);
    run(&p, 40);
    CHECK(p.s.align_state == RDOF_ALIGN_LOCKED && p.half[0].naligns > 0, "state %s",
          rdof_align_state_name(p.s.align_state));
    CHECK(on_grid(&p, 0), "on a 30 ms UTC boundary, counter = UTC n mod 2^32");
    CHECK(p.s.epoch_ok && p.s.align_flags == (RDOF_OPS_ALIGNED | RDOF_OPS_COUNTER_BASE) && p.half[0].stream_mode == 0,
          "the epoch follows; STATUS 46 after the snap 0x%02x; ALIGN in the FIFO mode", p.s.align_flags);

    pair_utc(&p, -250);
    run(&p, 20);
    CHECK(on_grid(&p, 250), "offset");

    /* a snap watched through STATUS until it's done */
    pair_utc_with(&p, slow_snap);
    while (!p.half[0].naligns)
        run(&p, 0.02);
    run(&p, 0.06);
    CHECK(p.s.align_state == RDOF_ALIGN_SNAPPING && p.half[0].has_snap, "snapping: %s",
          rdof_align_state_name(p.s.align_state));
    run(&p, 0.3);
    CHECK(p.s.align_state == RDOF_ALIGN_LOCKED && p.s.epoch_ok, "snapped: %s", rdof_align_state_name(p.s.align_state));

    /* a snap that lands off the grid: the counter base but not aligned, and ALIGN again */
    pair_utc_with(&p, snap_off);
    while (p.s.align_state != RDOF_ALIGN_WAITING || !p.half[0].naligns)
        run(&p, 0.02);
    CHECK(p.s.align_flags == RDOF_OPS_COUNTER_BASE && abs(p.s.align_err_us - 1500) < 2, "off: 0x%02x %d",
          p.s.align_flags, (int)p.s.align_err_us);
    int before = p.half[0].naligns;
    run(&p, 1.1);
    CHECK(p.half[0].naligns > before, "again after the next TIME round");

    /* keyed, under a millisecond off: the counter only */
    pair_utc(&p, 0);
    run(&p, 40);
    uint8_t one = 1, zero = 0;
    up_cmd(&p, RDOF_MMDVM_DMR_START, &one, 1);
    rdof_split_set_utc(&p.s, true, UTC_MINUS_HOST_US + 300, 10, 0, true, p.clock);
    wait_align(&p);
    CHECK(p.s.align_state == RDOF_ALIGN_LOCKED && abs(p.s.align_err_us - 300) < 20 &&
              !(p.s.align_flags & RDOF_OPS_SNAPPING) && p.s.epoch_ok,
          "keyed 0.3 ms: %s %d", rdof_align_state_name(p.s.align_state), (int)p.s.align_err_us);

    /* keyed, 2 ms off: result 0 flag 10, held, then snapped once unkeyed */
    pair_utc(&p, 0);
    run(&p, 40);
    up_cmd(&p, RDOF_MMDVM_DMR_START, &one, 1);
    rdof_split_set_utc(&p.s, true, UTC_MINUS_HOST_US + 2000, 10, 0, true, p.clock);
    wait_align(&p);
    CHECK(p.s.align_state == RDOF_ALIGN_HELD && abs(p.s.align_err_us - 2000) < 20 && !on_grid(&p, 2000),
          "keyed 2 ms: %s", rdof_align_state_name(p.s.align_state));
    up_cmd(&p, RDOF_MMDVM_DMR_START, &zero, 1);
    run(&p, 3);
    CHECK(p.s.align_state == RDOF_ALIGN_LOCKED && on_grid(&p, 2000) && p.s.epoch_ok, "unkeyed: %s",
          rdof_align_state_name(p.s.align_state));

    /* the air delay in INFO 14-15 */
    pair_utc_with(&p, air_delay_4600);
    run(&p, 20);
    CHECK(on_grid(&p, 4600), "4.6 ms to air: its slot boundary that much early");

    /* a retune: it snaps back by itself, and the host aligns again too */
    pair_utc(&p, 0);
    run(&p, 20);
    before = p.half[0].naligns;
    uint8_t freq[10] = {0};
    for (int i = 0; i < 2; i++) {
        uint32_t hz = 450000000u;
        memcpy(freq + 1 + 4 * i, &hz, 4); /* little endian host */
    }
    freq[9] = 255;
    up_cmd(&p, RDOF_MMDVM_SET_FREQ, freq, 10);
    run(&p, 0.3);
    CHECK(p.half[0].naligns > before && p.half[0].aligned && p.s.align_state == RDOF_ALIGN_LOCKED && on_grid(&p, 0),
          "retune: aligned again (%s)", rdof_align_state_name(p.s.align_state));

    /* the gate says stop: unkeyed, data refused */
    up_cmd(&p, RDOF_MMDVM_DMR_START, &one, 1);
    rdof_split_set_utc(&p.s, false, 0, 0, 0, false, p.clock);
    pump(&p);
    uint8_t data[34] = {0};
    p.up_len = 0;
    up_cmd(&p, RDOF_MMDVM_DMR_DATA2, data, sizeof data);
    CHECK(p.up_len == 5 && p.up[2] == RDOF_MMDVM_NAK && p.up[3] == RDOF_MMDVM_DMR_DATA2 && p.up[4] == 2, "NAK 2");
    CHECK(!p.half[0].duplex_tx, "unkeyed");

    /* without the capability: unsupported, its own slot clock */
    pair_utc_with(&p, no_caps);
    run(&p, 12);
    CHECK(p.s.align_state == RDOF_ALIGN_UNSUPPORTED && p.half[0].naligns == 0 && p.s.epoch_ok, "no caps: %s",
          rdof_align_state_name(p.s.align_state));
}

/* TX_SCHED against the simulated half, driven straight through rdof_ops_* (a TX site's controller). */
typedef struct {
    double clock;
    fake_radio_t r;
    rdof_mmdvm_reader_t reader;
    uint8_t sub, result, body[64];
    size_t n;
    double t0; /* modem µs of slot n0 */
    int64_t n0;
} direct_t;

static void direct_reply(void *ctx, uint8_t cmd, const uint8_t *p, size_t len)
{
    direct_t *d = ctx;
    if (cmd == 0xD6 && len >= 2) {
        d->sub = p[0], d->result = p[1], d->n = len - 2;
        memcpy(d->body, p + 2, d->n);
    } else {
        d->sub = 0xFF, d->result = cmd, d->n = len;
        memcpy(d->body, p, len < 64 ? len : 64);
    }
}

static void direct_send(direct_t *d, uint8_t cmd, const uint8_t *body, size_t n)
{
    uint8_t fr[80], buf[512];
    size_t fl = rdof_mmdvm_frame(cmd, body, n, fr, sizeof fr);
    fake_write(&d->r, fr, fl);
    size_t got = fake_take(&d->r, buf, sizeof buf);
    rdof_mmdvm_reader_feed(&d->reader, buf, got, direct_reply, d);
}

static void direct_ask(direct_t *d, uint8_t sub, const uint8_t *body, size_t n)
{
    uint8_t b[64] = {sub};
    if (n)
        memcpy(b + 1, body, n);
    direct_send(d, 0xD6, b, n + 1);
}

static void direct_init(direct_t *d, int stream_mode)
{
    memset(d, 0, sizeof *d);
    d->clock = 1000.0;
    fake_init(&d->r, "odr_split");
    d->r.clock = &d->clock;
    d->r.clock_offset_us = 1234567, d->r.clock_rate = 20e-6;
    rdof_mmdvm_reader_init(&d->reader);
    uint8_t role[7] = {1, 0, 0, 0, 0, 0, (uint8_t)stream_mode};
    direct_ask(d, RDOF_SPLIT_SET_ROLE, role, 7);
}

static void direct_run_to(direct_t *d, double modem_us);

/* Slot n0 140 ms out; settle: run on past the snap (2-3 slots). */
static int direct_align_s(direct_t *d, int64_t n0, bool settle)
{
    uint32_t now = (uint32_t)(uint64_t)(int64_t)rint(fake_modem_f(&d->r));
    uint8_t b[16];
    size_t n = rdof_ops_align(b, now + 140000, 30000000, n0, -1);
    direct_ask(d, RDOF_OPS_SUB_ALIGN, b, n);
    int32_t phase;
    int flags = 0;
    rdof_ops_aligned(d->result == 0, d->body, d->n, &phase, &flags);
    d->t0 = fake_modem_f(&d->r) + (int32_t)(now + 140000 - (uint32_t)(uint64_t)(int64_t)rint(fake_modem_f(&d->r)));
    d->n0 = n0;
    if (settle)
        direct_run_to(d, fake_modem_f(&d->r) + 100000);
    return flags;
}

static int direct_align(direct_t *d, int64_t n0) { return direct_align_s(d, n0, true); }

static int direct_sched_c(direct_t *d, int64_t n, uint8_t fill, uint8_t control);

static rdof_ops_status_t direct_status(direct_t *d)
{
    rdof_ops_status_t st = {0};
    direct_ask(d, RDOF_SPLIT_STATUS, NULL, 0);
    rdof_ops_status_ext(d->body, d->n, &st);
    return st;
}

static int direct_sched(direct_t *d, int64_t n, uint8_t fill) { return direct_sched_c(d, n, fill, 0x41); }

static int direct_sched_c(direct_t *d, int64_t n, uint8_t fill, uint8_t control)
{
    uint8_t b[64], burst[33];
    memset(burst, fill, 33);
    size_t len = rdof_ops_schedule(b, n, (n & 1) == 0 ? 1 : 2, control, burst);
    direct_ask(d, RDOF_OPS_SUB_TX_SCHED, b, len);
    uint32_t c;
    int outcome = -1;
    if (d->result == 0 && rdof_ops_scheduled(d->body, d->n, &c, &outcome) && c != rdof_slot_counter(n))
        return -2;
    return d->result ? -10 - d->result : outcome;
}

static void direct_run_to(direct_t *d, double modem_us)
{
    d->clock += (modem_us - fake_modem_f(&d->r)) / 1e6 / (1 + 20e-6) + 1e-6;
    fake_air(&d->r);
}

static double direct_at(const direct_t *d, int64_t n)
{
    return d->t0 + (double)(n - d->n0) * 30000;
}

static void test_sched(void)
{
    static direct_t d;
    direct_init(&d, 1);
    int flags = direct_align_s(&d, 1000, false);
    CHECK((flags & RDOF_OPS_SNAPPING) && !(flags & RDOF_OPS_PERIOD_APPLIED), "snapping, 08 never set: 0x%02x", flags);
    rdof_ops_status_t st = direct_status(&d);
    CHECK((st.flags & RDOF_OPS_SNAPPING) && direct_sched(&d, 1010, 1) == -12, "no counter base till the snap's done");
    direct_run_to(&d, fake_modem_f(&d.r) + 100000);
    st = direct_status(&d);
    CHECK(st.flags == (RDOF_OPS_ALIGNED | RDOF_OPS_COUNTER_BASE), "STATUS 46 0x%02x", st.flags);
    const int64_t ns[3] = {1010, 1011, 1014};
    for (int i = 0; i < 3; i++)
        CHECK(direct_sched(&d, ns[i], (uint8_t)ns[i]) == RDOF_SCHED_QUEUED, "queued %lld", (long long)ns[i]);
    direct_run_to(&d, direct_at(&d, 1015));
    bool ok = d.r.non_air == 3;
    for (int i = 0; ok && i < 3; i++)
        ok = d.r.on_air[i].counter == rdof_slot_counter(ns[i]) && d.r.on_air[i].burst[0] == (uint8_t)ns[i] &&
             d.r.on_air[i].slot == ((ns[i] & 1) ? 2 : 1) && fabs(d.r.on_air[i].start - direct_at(&d, ns[i])) < 1;
    CHECK(ok, "on air in their UTC slots (%d)", d.r.non_air);

    direct_init(&d, 1);
    d.r.sched_cap = 2;
    direct_align(&d, 1000);
    direct_run_to(&d, direct_at(&d, 1010) - 50000); /* inside the 55 ms guard */
    CHECK(direct_sched(&d, 1010, 1) == RDOF_SCHED_LATE, "late");
    CHECK(direct_sched(&d, 1050, 1) == RDOF_SCHED_EARLY, "early");
    CHECK(direct_sched(&d, 1012, 1) == RDOF_SCHED_QUEUED, "queued");
    CHECK(direct_sched(&d, 1012, 2) == RDOF_SCHED_REPLACED, "replaced");
    CHECK(direct_sched(&d, 1013, 1) == RDOF_SCHED_QUEUED, "queued");
    CHECK(direct_sched(&d, 1014, 1) == RDOF_SCHED_FULL, "full");
    d.r.cooling = true;
    CHECK(direct_sched(&d, 1015, 1) == RDOF_SCHED_FULL, "cool-down: result 0, outcome 4");
    d.r.cooling = false;
    st = direct_status(&d);
    CHECK(st.late == 1 && st.early_full == 2 && st.replaced == 1 && st.cooling == 1 &&
              st.flags == (RDOF_OPS_ALIGNED | RDOF_OPS_COUNTER_BASE),
          "status counters");

    /* an idle burst, or a repeated terminator, clears the place */
    direct_init(&d, 1);
    direct_align(&d, 1000);
    direct_sched(&d, 1012, 1);
    CHECK(direct_sched_c(&d, 1012, 0, 0x49) == RDOF_SCHED_QUEUED, "idle");
    direct_sched_c(&d, 1014, 0, 0x42);
    direct_sched_c(&d, 1014, 0, 0x42);
    direct_run_to(&d, direct_at(&d, 1016));
    CHECK(d.r.non_air == 0, "nothing on air (%d)", d.r.non_air);

    /* wrap aware */
    direct_init(&d, 1);
    int64_t n0 = 4294967296LL - 2;
    direct_align(&d, n0);
    CHECK(direct_sched(&d, n0 + 4, 7) == RDOF_SCHED_QUEUED, "past the wrap");
    direct_run_to(&d, direct_at(&d, n0 + 5));
    CHECK(d.r.non_air == 1 && d.r.on_air[0].counter == 2 && d.r.on_air[0].slot == 1, "counter 2 on air");

    /* refusals: the FIFO mode, no ALIGN, the wrong slot, DMR_DATA in mode 1 */
    direct_init(&d, 0);
    direct_align(&d, 1000);
    CHECK(direct_sched(&d, 1010, 1) == -12, "mode 0: result 2");
    direct_init(&d, 1);
    CHECK(direct_sched(&d, 1010, 1) == -12, "no ALIGN: result 2");
    direct_align(&d, 1000);
    uint8_t b[64], burst[33] = {0};
    rdof_ops_schedule(b, 1010, 1, 0x41, burst);
    b[4] = 2;
    direct_ask(&d, RDOF_OPS_SUB_TX_SCHED, b, 39);
    CHECK(d.result == 4, "TS2 in an even slot: result 4");
    uint8_t data[34] = {0};
    direct_send(&d, RDOF_MMDVM_DMR_DATA2, data, 34);
    CHECK(d.sub == 0xFF && d.result == RDOF_MMDVM_NAK && d.body[1] == 2, "DMR_DATA in mode 1: NAK 2");

    /* a retune: refused until it has snapped back onto the old grid by itself */
    uint8_t freq[10] = {0, 0x80, 0x74, 0xD2, 0x1A, 0x80, 0x74, 0xD2, 0x1A, 255}; /* 450 MHz */
    direct_send(&d, RDOF_MMDVM_SET_FREQ, freq, 10);
    CHECK(direct_sched(&d, 1020, 1) == -12 && (direct_status(&d).flags & RDOF_OPS_SNAPPING), "a retune: snapping back");
    direct_run_to(&d, fake_modem_f(&d.r) + 100000);
    CHECK(direct_sched(&d, 1030, 1) == RDOF_SCHED_QUEUED, "snapped back");
    direct_run_to(&d, direct_at(&d, 1031));
    CHECK(d.r.non_air == 1 && fabs(d.r.on_air[0].start - direct_at(&d, 1030)) < 1, "on the old grid");

    /* keying: a queued burst keys up ~150 ms ahead; a key-up it made ends 1 s after the last */
    direct_init(&d, 1);
    direct_align(&d, 1000);
    direct_sched(&d, 1020, 1);
    direct_run_to(&d, direct_at(&d, 1020) - 200000);
    CHECK(!d.r.duplex_tx, "not yet keyed");
    direct_run_to(&d, direct_at(&d, 1020) - 140000);
    CHECK(d.r.duplex_tx, "keyed ~150 ms before its slot");
    direct_run_to(&d, direct_at(&d, 1021) + 900000);
    CHECK(d.r.duplex_tx, "still keyed 0.9 s after");
    direct_run_to(&d, direct_at(&d, 1021) + 1100000);
    CHECK(!d.r.duplex_tx, "unkeyed 1 s after the last");
    /* DMR_START keys it as in the FIFO mode: up until DMR_START 00 */
    direct_init(&d, 1);
    direct_align(&d, 1000);
    uint8_t one = 1;
    direct_send(&d, RDOF_MMDVM_DMR_START, &one, 1);
    direct_sched(&d, 1020, 1);
    direct_run_to(&d, direct_at(&d, 1021) + 2000000);
    CHECK(d.r.duplex_tx && d.r.non_air == 1, "keyed by DMR_START");
}

void test_split(void)
{
    test_relay();
    test_align();
    test_sched();
    test_bring_up();
    test_refusals();
    test_role_matrix();
    test_dead_half();
    test_clock_sync();
}

#else

void test_split(void) {}

#endif
