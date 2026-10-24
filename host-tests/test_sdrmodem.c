// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* sdr's SDR modem on the host: the duplex CACH (short LC and AT), split duplex pairs of radios and SDR modems
 * in every combination (rdof/split.h between them), each role on its own, and the half and full duplex loopbacks
 * (rdof_sdrmodem_loopback). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_radio.h"
#include "rdof/split.h"

#include "rdof/duplex.h"
#include "rdof/layer2.h"
#include "rdof/pdu.h"
#include "rdof/sdr/sdrmodem.h"

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

#if RDOF_FEATURE_REPEATER

static void no_reply(void *ctx, const uint8_t *f, size_t n) { (void)ctx, (void)f, (void)n; }

static void cach_bits(const uint8_t c3[3], uint8_t bits[24])
{
    for (int i = 0; i < 24; i++)
        bits[i] = (uint8_t)((c3[i / 8] >> (7 - i % 8)) & 1);
}

/* The short LC read back from four CACHes from slot k (a multiple of 4); false if they don't make one. */
static bool read_short_lc(rdof_sdrmodem_t *m, long k, uint8_t lc5[5])
{
    static const int want[4] = {1, 3, 3, 2};
    uint32_t rows[4];
    for (int q = 0; q < 4; q++) {
        uint8_t c3[3], bits[24];
        int lcss;
        rdof_sdrmodem_cach(m, k + q, c3);
        cach_bits(c3, bits);
        rows[q] = rdof_decode_cach_payload(bits, &lcss);
        if (lcss != want[q] || rdof_decode_cach_slot(bits) != (int)((k + q) & 1) + 1)
            return false;
    }
    uint8_t sl9[9];
    rdof_short_lc_join(rows, sl9);
    return rdof_decode_short_lc(sl9, lc5);
}

static void shortlc_frame(rdof_sdrmodem_t *m, const uint8_t p9[9])
{
    uint8_t f[16];
    size_t n = rdof_mmdvm_frame(RDOF_MMDVM_DMR_SHORTLC, p9, 9, f, sizeof f);
    rdof_sdrmodem_write(m, f, n);
}

static void heard(rdof_sdrmodem_t *m, int slot, int dt, int64_t pos)
{
    rdof_sdr_burst_t b;
    memset(&b, 0, sizeof b);
    b.b.slot = slot;
    b.b.dt = dt;
    b.position = pos;
    rdof_sdrmodem_heard(m, 0, &b);
}

static int at_bit(rdof_sdrmodem_t *m, long k)
{
    uint8_t c3[3], bits[24];
    rdof_sdrmodem_cach(m, k, c3);
    cach_bits(c3, bits);
    return bits[0]; /* AT is the first bit on air */
}

static void test_cach(void)
{
    static rdof_sdrmodem_t m;
    const rdof_sdrmodem_io_t io = {no_reply, NULL, NULL, NULL, NULL};
    CHECK(rdof_sdrmodem_init(&m, &io, 48000, 48000, true) == 0, "init: %s", m.error);
    uint8_t lc5[5];
    CHECK(read_short_lc(&m, 0, lc5) && !memcmp(lc5, (uint8_t[5]){0}, 4), "the Null short LC before any DMR_SHORTLC");

    /* the encoded form, as the repeater sends it */
    const rdof_activity_t voice = {RDOF_ACT_VOICE, 9990003, true}, idle = {RDOF_ACT_NONE, 0, false};
    uint8_t sl9[9], want5[5];
    rdof_activity_lc(&voice, &idle, false, sl9);
    CHECK(rdof_decode_short_lc(sl9, want5), "rdof_activity_lc decodes");
    shortlc_frame(&m, sl9);
    CHECK(read_short_lc(&m, 2, lc5) == false, "not from the middle of a 4-slot cycle");
    CHECK(read_short_lc(&m, 4, lc5) && !memcmp(lc5, want5, 5), "Act_Updt on air from the next multiple of 4");

    /* the 5-byte form; then a frame that's neither: the previous one stays */
    const uint8_t null5[9] = {0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t five[9] = {0};
    rdof_short_lc(null5, sl9);
    CHECK(rdof_decode_short_lc(sl9, five), "null decodes");
    shortlc_frame(&m, five);
    CHECK(read_short_lc(&m, 8, lc5) && (lc5[0] & 0x0F) == 0 && !lc5[1] && !lc5[2] && !lc5[3], "the 5-byte form");
    uint8_t junk[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    shortlc_frame(&m, junk);
    CHECK(m.bad_short_lcs == 1 && read_short_lc(&m, 12, lc5) && !lc5[0] && !lc5[3], "a bad one dropped");
    CHECK(m.short_lcs == 2, "%u taken", (unsigned)m.short_lcs);

    /* AT: the inbound slot's busy state, in the CACH before the outbound slot with its number */
    m.duplex = true;
    m.mode = RDOF_MODE_DMR;
    m.channel = 0;
    CHECK(at_bit(&m, 0) == 0 && at_bit(&m, 1) == 0, "idle inbound");
    heard(&m, 2, RDOF_DT_VOICE_LC_HEADER, 1000);
    CHECK(at_bit(&m, 0) == 0 && at_bit(&m, 1) == 1 && rdof_sdrmodem_busy(&m) == 2, "TS2 voice busy");
    heard(&m, 2, RDOF_DT_TERMINATOR_WITH_LC, 2000);
    CHECK(rdof_sdrmodem_busy(&m) == 0, "a terminator ends it");
    heard(&m, 1, RDOF_DT_CSBK, 3000);
    CHECK(at_bit(&m, 0) == 1 && rdof_sdrmodem_busy(&m) == 1, "TS1 data busy");
    m.now48 = 3000 + 17280;
    CHECK(rdof_sdrmodem_busy(&m) == 0, "for 360 ms");
    rdof_sdrmodem_free(&m);
}

/* ── the split matrix: any RX-capable half with any TX-capable half ─────────────
 *
 * Each half is a radio (fake_radio's odr_split) or an SDR modem with its own sample clock,
 * and rdof_split between them and the repeater. Every pairing is brought up, given the repeater's
 * settings, then fed inbound bursts on the TX half's outbound slot grid for 90 s: each must go up on its own slot,
 * and the RX half's epoch must sit on the TX half's real slot starts. The SDR TX half also keys up, sends what the
 * repeater queues, carries the AT bits the split sends, and drops its role when the host goes quiet. */

#define RXF 461037500u
#define TXF 466037500u

enum { RADIO = 0, SDR = 1 };

struct pair;
typedef struct {
    struct pair *p;
    int which, kind;
    fake_radio_t f;
    rdof_sdrmodem_t m;
    double rate, off; /* an SDR's 48 kS/s sample clock against the host's */
    int64_t pulled;   /* an SDR's transmit samples pulled */
} half_t;

typedef struct pair {
    double clock; /* the host's, s */
    half_t h[2];
    rdof_split_t s;
    uint8_t up[16384];
    size_t up_len;
} pair_t;

static int64_t pos_at(const half_t *h, double t) { return (int64_t)llround(t * 48000.0 * (1 + h->rate) + h->off); }
static double host_of(const half_t *h, double pos) { return (pos - h->off) / (48000.0 * (1 + h->rate)); }
static int64_t io_now48(void *ctx) { return pos_at(ctx, ((half_t *)ctx)->p->clock); }
static int io_channel(void *ctx, uint32_t hz) { return hz == RXF ? 0 : -1; }
static bool io_tx_tune(void *ctx, uint32_t hz, double *off)
{
    *off = 0;
    return hz == TXF;
}
static void io_reply(void *ctx, const uint8_t *f, size_t n)
{
    half_t *h = ctx;
    rdof_split_from_half(&h->p->s, h->which, f, n, h->p->clock);
}

static void pump(pair_t *p)
{
    uint8_t buf[4096];
    size_t n;
    bool moved = true;
    while (moved) {
        moved = false;
        for (int i = 0; i < 2; i++) {
            half_t *h = &p->h[i];
            if ((n = rdof_split_take(&p->s, i, buf, sizeof buf)) > 0) {
                if (h->kind == RADIO)
                    fake_write(&h->f, buf, n);
                else
                    rdof_sdrmodem_write(&h->m, buf, n); /* its replies come back through io_reply */
                moved = true;
            }
            while (h->kind == RADIO && (n = fake_take(&h->f, buf, sizeof buf)) > 0) {
                rdof_split_from_half(&p->s, i, buf, n, p->clock);
                moved = true;
            }
        }
    }
    p->up_len += rdof_split_take(&p->s, -1, p->up + p->up_len, sizeof p->up - p->up_len);
}

/* The SDRs' front ends pull their transmit samples up to now, as the board's would. */
static void pull(pair_t *p)
{
    static rdof_cf_t iq[256];
    for (int i = 0; i < 2; i++) {
        half_t *h = &p->h[i];
        if (h->kind != SDR)
            continue;
        const int64_t to = pos_at(h, p->clock);
        while (h->pulled < to) {
            int n = to - h->pulled < 256 ? (int)(to - h->pulled) : 256;
            rdof_sdrmodem_pull_iq(&h->m, h->pulled, iq, n);
            h->pulled += n;
        }
    }
}

static void step(pair_t *p, double t, bool host)
{
    p->clock = t;
    pull(p);
    if (host) {
        rdof_split_tick(&p->s, p->clock);
        pump(p);
    }
}

static void run_to(pair_t *p, double until)
{
    while (p->clock + 0.02 <= until)
        step(p, p->clock + 0.02, true);
    step(p, until, true);
}

static void upstream(pair_t *p, uint8_t cmd, const uint8_t *d, size_t n)
{
    uint8_t f[64];
    size_t len = rdof_mmdvm_frame(cmd, d, n, f, sizeof f);
    rdof_split_upstream(&p->s, f, len, p->clock);
    pump(p);
}

typedef struct {
    int n, cmds[64];
} frames_t;

static void on_up(void *ctx, uint8_t cmd, const uint8_t *d, size_t n)
{
    frames_t *f = ctx;
    if (f->n < 64)
        f->cmds[f->n++] = cmd == RDOF_MMDVM_ACK && n ? 0x100 | d[0] : cmd;
}

static frames_t take_up(pair_t *p)
{
    frames_t f = {0};
    rdof_mmdvm_reader_t r;
    rdof_mmdvm_reader_init(&r);
    rdof_mmdvm_reader_feed(&r, p->up, p->up_len, on_up, &f);
    p->up_len = 0;
    return f;
}

/* The TX half's real outbound grid: a TS1 start (host time) and the slot length. */
static void tx_grid(const pair_t *p, double *e, double *slot_s)
{
    const half_t *t = &p->h[RDOF_SPLIT_TX];
    if (t->kind == RADIO) { /* the fake's slot clock: its latest TS1 boundary (an even counter) */
        double b;
        long k = fake_slot_boundary(&t->f, fake_modem_f(&t->f), &b);
        if ((t->f.counter_base + (uint32_t)k) % 2)
            b -= t->f.slot_period_us;
        *e = (b - t->f.clock_offset_us) / (1 + t->f.clock_rate) / 1e6;
        *slot_s = t->f.slot_period_us / 1e6 / (1 + t->f.clock_rate);
    } else {
        *e = host_of(t, (double)t->m.base);
        *slot_s = rdof_mod_slot_samples(&t->m.mod) / (48000.0 * (1 + t->rate));
    }
}

/* Where the RX half's epoch puts a TS1 start against the TX half's real ones, µs. */
static double epoch_error_us(const pair_t *p)
{
    const half_t *r = &p->h[RDOF_SPLIT_RX];
    uint32_t t_us;
    double rate, off_us;
    if (r->kind == RADIO) {
        t_us = r->f.epoch_t_us, rate = r->f.clock_rate, off_us = r->f.clock_offset_us;
    } else {
        t_us = r->m.epoch_us, rate = r->rate, off_us = r->off * 125.0 / 6.0;
    }
    const double now_us = p->clock * 1e6 * (1 + rate) + off_us;
    const double h_e = p->clock + (int32_t)(t_us - (uint32_t)(uint64_t)llround(now_us)) / (1e6 * (1 + rate));
    double e, slot_s;
    tx_grid(p, &e, &slot_s);
    double k = (h_e - e) / (2 * slot_s);
    return (k - round(k)) * 2 * slot_s * 1e6;
}

/* The start (host time) of the first inbound burst on slot at least ahead s from now, on the TX half's grid. */
static double slot_time(const pair_t *p, int slot, double ahead, double *e)
{
    double slot_s;
    tx_grid(p, e, &slot_s);
    double m = ceil((p->clock + ahead - *e) / (2 * slot_s));
    return *e + (2 * m + (slot == 1)) * slot_s + 0.00125;
}

static void inject(pair_t *p, int slot, int dt, double t)
{
    run_to(p, t);
    half_t *r = &p->h[RDOF_SPLIT_RX];
    if (r->kind == RADIO) {
        uint8_t burst[RDOF_BURST_BYTES] = {0};
        fake_hear(&r->f, (uint8_t)(RDOF_SYNC_DATA | dt), burst, 100, slot);
    } else {
        rdof_sdr_burst_t b;
        memset(&b, 0, sizeof b);
        b.b.slot = 1; /* the demod's guess: the RX half labels it itself */
        b.b.dt = dt;
        b.position = pos_at(r, t);
        rdof_sdrmodem_heard(&r->m, 0, &b);
    }
    pump(p);
}

static const char *const KIND_NAMES[2] = {"radio", "SDR"};

static void half_init(pair_t *p, int which, int kind, bool can_tx)
{
    half_t *h = &p->h[which];
    h->p = p, h->which = which, h->kind = kind;
    if (kind == RADIO) {
        fake_init(&h->f, "odr_split");
        h->f.clock = &p->clock;
        h->f.clock_offset_us = which ? 4000000000.0 : 1234567;
        h->f.clock_rate = which ? -15e-6 : 20e-6;
        return;
    }
    h->rate = which ? 80e-6 : -35e-6; /* an RTL-SDR without its ppm learned; a TCXO transmitter */
    h->off = which ? 123456789.0 : 987654.0;
    const rdof_sdrmodem_io_t io = {io_reply, io_channel, can_tx ? io_tx_tune : NULL, h, io_now48};
    CHECK(rdof_sdrmodem_init(&h->m, &io, 48000, 48000, false) == 0, "init: %s", h->m.error);
    h->pulled = pos_at(h, p->clock);
}

static void pair_init(pair_t *p, int tx_kind, int rx_kind, bool sdr_can_tx)
{
    memset(p, 0, sizeof *p);
    p->clock = 1000.0;
    half_init(p, RDOF_SPLIT_TX, tx_kind, sdr_can_tx);
    half_init(p, RDOF_SPLIT_RX, rx_kind, sdr_can_tx);
    rdof_split_init(&p->s, p->clock, NULL);
    pump(p);
    run_to(p, p->clock + 0.2);
}

static void pair_free(pair_t *p)
{
    for (int i = 0; i < 2; i++)
        if (p->h[i].kind != RADIO)
            rdof_sdrmodem_free(&p->h[i].m);
}

static int role_of(const half_t *h) { return h->kind == RADIO ? h->f.role : h->m.role; }

static void test_pair(int tx_kind, int rx_kind)
{
    static pair_t p;
    char name[32];
    snprintf(name, sizeof name, "%s TX + %s RX", KIND_NAMES[tx_kind], KIND_NAMES[rx_kind]);
    pair_init(&p, tx_kind, rx_kind, true);
    CHECK(p.s.ready && p.s.epoch_ok, "%s: bring-up: %s", name, p.s.failed);
    CHECK(role_of(&p.h[0]) == 1 && role_of(&p.h[1]) == 2, "%s: roles %d %d", name, role_of(&p.h[0]),
          role_of(&p.h[1]));

    /* the repeater's settings, to both halves: duplex on half duplex front ends, a frequency each doesn't use */
    uint8_t cfg[20] = {0};
    cfg[3] = RDOF_MODE_DMR;
    cfg[6] = 1;
    upstream(&p, RDOF_MMDVM_SET_CONFIG, cfg, sizeof cfg);
    const uint8_t mode = RDOF_MODE_DMR;
    upstream(&p, RDOF_MMDVM_SET_MODE, &mode, 1);
    uint8_t fq[10] = {0};
    for (int i = 0; i < 4; i++)
        fq[1 + i] = (uint8_t)(RXF >> (8 * i)), fq[5 + i] = (uint8_t)(TXF >> (8 * i));
    fq[9] = 100;
    upstream(&p, RDOF_MMDVM_SET_FREQ, fq, sizeof fq);
    run_to(&p, p.clock + 0.5); /* a retune: the SDR TX half's slot clock starts again, so a new epoch */
    frames_t f = take_up(&p);
    CHECK(f.n == 3 && f.cmds[0] == (0x100 | RDOF_MMDVM_SET_CONFIG) && f.cmds[1] == (0x100 | RDOF_MMDVM_SET_MODE) &&
              f.cmds[2] == (0x100 | RDOF_MMDVM_SET_FREQ),
          "%s: %d replies: %x %x %x (%s %s)", name, f.n, f.cmds[0], f.cmds[1], f.cmds[2], p.h[0].m.error,
          p.h[1].m.error);

    /* inbound bursts on the TX half's grid for 90 s: each up on its slot; the epoch on the real slot starts */
    int wrong = 0, far = 0, sent = 0;
    double worst = 0;
    const double start = p.clock;
    for (int k = 0; p.clock < start + 90; k++) {
        const int slot = k % 2 ? 1 : 2;
        double e, slot_s, t = slot_time(&p, slot, 0.4, &e);
        run_to(&p, t - 0.001);
        double e2, s2;
        tx_grid(&p, &e2, &s2);
        (void)slot_s;
        if (fabs(e2 - e) > 1e-4 && fmod(fabs(e2 - e) + 1e-4, 2 * s2) > 2e-4)
            continue; /* the grid moved (a retune restarts the SDR's): pick the slot from the new one */
        inject(&p, slot, RDOF_DT_CSBK, t);
        f = take_up(&p);
        int want = slot == 1 ? RDOF_MMDVM_DMR_DATA1 : RDOF_MMDVM_DMR_DATA2, got = 0;
        for (int i = 0; i < f.n; i++)
            if (f.cmds[i] == RDOF_MMDVM_DMR_DATA1 || f.cmds[i] == RDOF_MMDVM_DMR_DATA2)
                got = f.cmds[i];
        sent++;
        wrong += got != want;
        if (p.clock > start + 15) { /* once the clock fit has a rate (split-duplex.md 6) */
            double err = fabs(epoch_error_us(&p));
            far += err > 100;
            worst = err > worst ? err : worst;
        }
    }
    CHECK(!wrong && !far && sent > 100, "%s: %d of %d bursts on the wrong slot, %d epochs > 100 us off (worst %.0f)",
          name, wrong, sent, far, worst);
    if (rx_kind == SDR)
        CHECK(p.h[1].m.labeled[0] + p.h[1].m.labeled[1] == (uint32_t)sent && !p.h[1].m.unlabeled,
              "%s: labeled %u %u, unlabeled %u", name, (unsigned)p.h[1].m.labeled[0], (unsigned)p.h[1].m.labeled[1],
              (unsigned)p.h[1].m.unlabeled);
    CHECK(!p.s.failed[0] && p.s.dropped == 0, "%s: %s; %u dropped", name, p.s.failed, (unsigned)p.s.dropped);
    printf("%s: %d bursts labeled, epoch within %.0f us of the TX half's slots\n", name, sent, worst);

    if (tx_kind != SDR) {
        pair_free(&p);
        return;
    }
    rdof_sdrmodem_t *tx = &p.h[0].m;
    /* AT: an inbound call on TS1 (voice header) is busy on air until its terminator, through the split's SET_AT */
    double e;
    run_to(&p, p.clock + 0.5); /* the last data burst's 360 ms */
    inject(&p, 1, RDOF_DT_VOICE_LC_HEADER, slot_time(&p, 1, 0.1, &e));
    run_to(&p, p.clock + 0.1);
    CHECK((rdof_sdrmodem_at(tx) & 1) && tx->has_at, "%s: AT for TS1 %d", name, rdof_sdrmodem_at(tx));
    inject(&p, 1, RDOF_DT_TERMINATOR_WITH_LC, slot_time(&p, 1, 0.1, &e));
    run_to(&p, p.clock + 0.1);
    CHECK(rdof_sdrmodem_at(tx) == 0, "%s: AT after the terminator %d", name, rdof_sdrmodem_at(tx));

    /* the downlink: keyed, the repeater's bursts sent once the prebuffer's in, idle bursts around them */
    const uint8_t on = 1, off = 0;
    upstream(&p, RDOF_MMDVM_DMR_START, &on, 1);
    uint8_t b[1 + RDOF_BURST_BYTES] = {RDOF_SYNC_DATA | RDOF_DT_CSBK};
    const uint32_t sent_before = tx->tx_bursts;
    for (int i = 0; i < 4; i++)
        upstream(&p, RDOF_MMDVM_DMR_DATA2, b, sizeof b);
    run_to(&p, p.clock + 1.0);
    CHECK(tx->keyed && tx->tx_bursts - sent_before == 4 && tx->idle_bursts > 20 && tx->underruns[1] == 1,
          "%s: keyed %d, %u sent, %u idle, %u underruns", name, tx->keyed, (unsigned)(tx->tx_bursts - sent_before),
          (unsigned)tx->idle_bursts, (unsigned)tx->underruns[1]);
    CHECK(rdof_sdrmodem_at(tx) == 0, "%s: no AT once the slot is idle again: %d", name, rdof_sdrmodem_at(tx));
    upstream(&p, RDOF_MMDVM_DMR_START, &off, 1);
    run_to(&p, p.clock + 0.2);
    CHECK(!tx->keyed && tx->clock_running, "%s: unkeyed, the slot clock runs on", name);

    /* the host goes quiet: the role ends with the lease */
    upstream(&p, RDOF_MMDVM_DMR_START, &on, 1);
    const double quiet = p.clock;
    while (p.clock < quiet + 11)
        step(&p, p.clock + 0.02, false);
    CHECK(tx->role == 0 && !tx->keyed && tx->lease_expiries == 1, "%s: role %d keyed %d after the lease", name,
          tx->role, tx->keyed);
    pair_free(&p);
}

/* Pairings that can't work are refused with why: an SDR with no transmitter as the TX half, a radio without the
 * extension as either half. */
static void test_pair_refusals(void)
{
    static pair_t p;
    pair_init(&p, SDR, RADIO, false);
    CHECK(!p.s.ready && strstr(p.s.failed, "TX half") && strstr(p.s.failed, "can't take that role"), "no TX: %s",
          p.s.failed);
    pair_free(&p);
    pair_init(&p, SDR, RADIO, true);
    fake_init(&p.h[1].f, "odr");
    p.h[1].f.clock = &p.clock;
    rdof_split_init(&p.s, p.clock, NULL);
    pump(&p);
    CHECK(!p.s.ready && strstr(p.s.failed, "RX half") && strstr(p.s.failed, "SPLIT1"), "plain hotspot: %s",
          p.s.failed);
    pair_free(&p);
}

static void test_split_matrix(void)
{
    for (int tx = RADIO; tx <= SDR; tx++)
        for (int rx = RADIO; rx <= SDR; rx++)
            test_pair(tx, rx);
    test_pair_refusals();
}

/* Without a split host: no epoch (DMR_DATA2, counted), the lease running out, the TX role refused. */
static int64_t solo_now;
static int64_t solo_now48(void *ctx) { return solo_now; }
static int solo_replies, solo_last_result;
static void solo_reply(void *ctx, const uint8_t *f, size_t n)
{
    solo_replies++;
    if (n >= 5 && f[2] == 0xD6)
        solo_last_result = f[4];
}

static void d6(rdof_sdrmodem_t *m, const uint8_t *d, size_t n)
{
    uint8_t f[64];
    size_t len = rdof_mmdvm_frame(0xD6, d, n, f, sizeof f);
    rdof_sdrmodem_write(m, f, len);
}

static void test_split_rx_solo(void)
{
    static rdof_sdrmodem_t m;
    const rdof_sdrmodem_io_t io = {solo_reply, NULL, NULL, NULL, solo_now48};
    CHECK(rdof_sdrmodem_init(&m, &io, 48000, 48000, false) == 0, "init");
    d6(&m, (uint8_t[]){1, 1}, 2);
    CHECK(solo_last_result == 2 && m.role == 0, "the TX role refused (%d)", solo_last_result);
    d6(&m, (uint8_t[]){1, 2, 3}, 3); /* RX half, a 3 s lease */
    CHECK(solo_last_result == 0 && m.role == 2 && m.lease_s == 3, "the RX role");
    d6(&m, (uint8_t[]){4, 0, 0, 0, 0, 0x80, 0xC3, 0xC9, 0x01}, 9); /* period 30 000 000 */
    CHECK(solo_last_result == 0 && m.has_epoch, "SET_EPOCH");
    d6(&m, (uint8_t[]){4, 0, 0, 0, 0, 0x00, 0x00, 0x00, 0x02}, 9);
    CHECK(solo_last_result == 4, "a period 11 %% off refused");
    CHECK(rdof_sdrmodem_label(&m, 48 * 30 * 2 + 60) == 2 && rdof_sdrmodem_label(&m, 48 * 30 * 3 + 60) == 1,
          "even slot TS2, odd TS1");
    solo_now = 4 * 48000; /* past the lease */
    m.mode = RDOF_MODE_DMR, m.channel = 0;
    heard(&m, 1, RDOF_DT_CSBK, solo_now);
    CHECK(m.role == 0 && m.lease_expiries == 1 && !m.has_epoch, "the lease ran out");
    d6(&m, (uint8_t[]){1, 2}, 2);
    heard(&m, 1, RDOF_DT_CSBK, solo_now + 100);
    CHECK(m.unlabeled == 1, "no epoch: unlabeled %u", (unsigned)m.unlabeled);
    d6(&m, (uint8_t[]){9}, 1);
    CHECK(solo_last_result == 1, "unknown sub-command");
    rdof_sdrmodem_free(&m);
}

/* The TX half on its own: the slot clock and GET_EPOCH, SET_AT's modes and its 2 s hold, the key-up limit. */
static uint8_t solo_d6[48];
static size_t solo_d6_len;
static void solo_tx_reply(void *ctx, const uint8_t *f, size_t n)
{
    if (n >= 5 && f[2] == 0xD6) {
        solo_last_result = f[4];
        solo_d6_len = n - 5 < sizeof solo_d6 ? n - 5 : sizeof solo_d6;
        memcpy(solo_d6, f + 5, solo_d6_len);
    }
}
static bool solo_tune(void *ctx, uint32_t hz, double *off)
{
    *off = 0;
    return true;
}

static void solo_pull(rdof_sdrmodem_t *m, int64_t to)
{
    static rdof_cf_t iq[256];
    while (m->pos < to) {
        int n = to - m->pos < 256 ? (int)(to - m->pos) : 256;
        rdof_sdrmodem_pull_iq(m, m->pos, iq, n);
    }
    solo_now = to;
}

static void test_split_tx_solo(void)
{
    static rdof_sdrmodem_t m;
    const rdof_sdrmodem_io_t io = {solo_tx_reply, NULL, solo_tune, NULL, solo_now48};
    solo_now = 0;
    CHECK(rdof_sdrmodem_init(&m, &io, 48000, 48000, false) == 0, "init");
    d6(&m, (uint8_t[]){0}, 1);
    CHECK(solo_last_result == 0 && solo_d6[1] == 0x03 && solo_d6[3] == RDOF_SDRMODEM_QUEUE && solo_d6[4] == 1,
          "INFO offers both roles: %02x", solo_d6[1]);
    d6(&m, (uint8_t[]){1, 1, 0, 0, 2, 1, 1}, 7); /* TX half: prebuffer 2, 10 s key-up, 1 s cool-down */
    CHECK(solo_last_result == 0 && m.role == 1 && m.prebuffer == 2, "the TX role");
    d6(&m, (uint8_t[]){3}, 1);
    CHECK(solo_last_result == 2, "no slot clock before the first pull");
    solo_pull(&m, 48000);
    d6(&m, (uint8_t[]){3}, 1);
    uint32_t t = solo_d6[4] | solo_d6[5] << 8 | solo_d6[6] << 16 | (uint32_t)solo_d6[7] << 24;
    uint32_t k = solo_d6[0] | solo_d6[1] << 8 | solo_d6[2] << 16 | (uint32_t)solo_d6[3] << 24;
    CHECK(solo_last_result == 0 && (int64_t)t * 48 / 1000 == m.base + (int64_t)k * 1440 && t <= 1000000 &&
              t > 1000000 - 30000 && solo_d6[8] == (k & 1 ? 2 : 1) && (solo_d6[9] & 0x02),
          "GET_EPOCH: slot %u at %u us, TS%d", (unsigned)k, (unsigned)t, solo_d6[8]);
    /* SET_AT: the mask, held 2 s; mode 1 the mask alone, mode 0 ORed with what's sent */
    d6(&m, (uint8_t[]){5, 2}, 2);
    CHECK(solo_last_result == 0 && solo_d6[0] == 2 && rdof_sdrmodem_at(&m) == 2, "SET_AT 2");
    solo_pull(&m, 48000 * 4);
    CHECK(rdof_sdrmodem_at(&m) == 0, "idle 2 s after SET_AT");
    d6(&m, (uint8_t[]){5, 4}, 2);
    CHECK(solo_last_result == 4, "a mask above 3 refused");
    m.duplex = true, m.mode = RDOF_MODE_DMR, m.tuned = true;
    rdof_sdrmodem_write(&m, (uint8_t[]){0xE0, 0x04, RDOF_MMDVM_DMR_START, 1}, 4);
    uint8_t f[48];
    uint8_t b[1 + RDOF_BURST_BYTES] = {RDOF_SYNC_DATA | RDOF_DT_CSBK};
    size_t fn = rdof_mmdvm_frame(RDOF_MMDVM_DMR_DATA1, b, sizeof b, f, sizeof f);
    for (int i = 0; i < 3; i++)
        rdof_sdrmodem_write(&m, f, fn);
    solo_pull(&m, solo_now + 48 * 70);
    CHECK(m.tx_active[0] && rdof_sdrmodem_at(&m) == 1, "mode 0: TS1 sent marks TS1 busy: %d", rdof_sdrmodem_at(&m));
    d6(&m, (uint8_t[]){5, 0, 1}, 3);
    CHECK(rdof_sdrmodem_at(&m) == 0, "mode 1: the mask alone");
    /* the key-up limit, then the cool-down refuses DMR_START */
    const int64_t keyed_at = m.keyed_at48;
    for (int s = 0; s < 60 && m.keyed; s++) {
        d6(&m, (uint8_t[]){2, 0, 0, 0, 0, 0, 0, 0, 0}, 9); /* TIME keeps the lease */
        solo_pull(&m, solo_now + 12000);
    }
    CHECK(!m.keyed && m.cool_until48 > 0 && llabs(solo_now - keyed_at - 10 * 48000) <= 12000 + 48 * 60,
          "unkeyed after 10 s: %.2f s", (solo_now - keyed_at) / 48000.0);
    rdof_sdrmodem_write(&m, (uint8_t[]){0xE0, 0x04, RDOF_MMDVM_DMR_START, 1}, 4);
    CHECK(!m.keyed, "not keyed in the cool-down");
    solo_pull(&m, m.cool_until48 + 48);
    rdof_sdrmodem_write(&m, (uint8_t[]){0xE0, 0x04, RDOF_MMDVM_DMR_START, 1}, 4);
    CHECK(m.keyed, "keyed again after it");
    /* a retune restarts the slot clock */
    uint8_t fq[9] = {0};
    rdof_sdrmodem_write(&m, f, rdof_mmdvm_frame(RDOF_MMDVM_SET_FREQ, fq, sizeof fq, f, sizeof f));
    CHECK(!m.clock_running && m.slot_k < 0, "SET_FREQ restarts the slot clock");
    rdof_sdrmodem_free(&m);
}

/* ── UTC slot timing ────────────────────────────────────────────────────────────
 *
 * A duplex SDR modem whose front end's crystal runs 25 ppm fast, mapped to UTC by a sample clock fed jittery
 * arrival stamps (0-2 ms late, with no ppm learned): keyed, every slot's CACH starts on the UTC grid, steered a
 * sample a slot at most, also out of a map 2 ms wrong at key-up. On the grid here is within 120 µs: the map's own
 * error (the earliest arrival in a window, 20-70 µs with this jitter, as the sample clock's err_ns says) and a sample. Then the gate: stop, free (no map), and the grid
 * labeling heard bursts. */
#define UTC0_NS 1790000000000000000LL

typedef struct {
    double t; /* true UTC s since UTC0 */
    double ppm;
    int64_t off;
    rdof_sdrmodem_t m;
    rdof_sample_clock_t sc;
    int64_t map_bias_ns;
    bool map;
    unsigned seed;
    int last_cmd, last_nak;
} utc_t;

static utc_t U;

static int64_t utc_pos(const utc_t *u, double t) { return (int64_t)llround(t * 48000.0 * (1 + u->ppm * 1e-6)) + u->off; }
static double utc_true_s(const utc_t *u, double pos) { return (pos - (double)u->off) / (48000.0 * (1 + u->ppm * 1e-6)); }
static int64_t utc_now48(void *ctx) { return utc_pos(&U, U.t); }
static void utc_reply(void *ctx, const uint8_t *f, size_t n)
{
    if (n >= 3)
        U.last_cmd = f[2];
    if (n >= 5 && f[2] == RDOF_MMDVM_NAK)
        U.last_nak = f[4];
}
static int utc_channel(void *ctx, uint32_t hz) { return 0; }

static void utc_cmd(uint8_t cmd, const uint8_t *d, size_t n)
{
    uint8_t f[64];
    rdof_sdrmodem_write(&U.m, f, rdof_mmdvm_frame(cmd, d, n, f, sizeof f));
}

/* 20 ms on: the front end pulls its samples, the platform stamps their arrival and maps them. */
static void utc_step(void)
{
    static rdof_cf_t iq[256];
    U.t += 0.02;
    const int64_t to = utc_pos(&U, U.t);
    while (U.m.pos < to) {
        int n = to - U.m.pos < 256 ? (int)(to - U.m.pos) : 256;
        rdof_sdrmodem_pull_iq(&U.m, U.m.pos, iq, n);
    }
    U.seed = U.seed * 1103515245u + 12345u;
    int64_t late = (int64_t)((U.seed >> 8) % 2000000u); /* 0-2 ms */
    rdof_sample_clock_add(&U.sc, to, UTC0_NS + (int64_t)llround(U.t * 1e9) + late);
    if (U.map)
        rdof_sdrmodem_grid_map(&U.m, true, U.sc.i0, U.sc.t0 + U.map_bias_ns, rdof_sample_clock_rate(&U.sc));
}

/* The last queued slot's start against UTC: error µs, and its n. */
static double utc_slot_err(int64_t *n)
{
    const int64_t slot = rdof_mod_slot_samples(&U.m.mod), k = U.m.slot_k - 1;
    const double at = (double)(U.m.base + k * slot);
    *n = U.m.grid_n0 + k;
    return (UTC0_NS + utc_true_s(&U, at) * 1e9 - (double)rdof_grid_boundary(*n, U.m.grid_offset_us)) / 1000.0;
}

static void utc_init(int offset_us)
{
    memset(&U, 0, sizeof U);
    U.ppm = 25, U.off = 777777, U.seed = 1, U.map = true;
    const rdof_sdrmodem_io_t io = {utc_reply, utc_channel, solo_tune, NULL, utc_now48};
    CHECK(rdof_sdrmodem_init(&U.m, &io, 48000, 48000, true) == 0, "init");
    rdof_sample_clock_init(&U.sc, 48000, 2.0, false);
    rdof_sdrmodem_grid(&U.m, true, offset_us);
    U.m.pos = utc_pos(&U, 0);
    uint8_t cfg[20] = {0};
    cfg[3] = RDOF_MODE_DMR;
    utc_cmd(RDOF_MMDVM_SET_CONFIG, cfg, sizeof cfg);
    uint8_t mode = RDOF_MODE_DMR;
    utc_cmd(RDOF_MMDVM_SET_MODE, &mode, 1);
    uint8_t fq[9] = {0};
    utc_cmd(RDOF_MMDVM_SET_FREQ, fq, sizeof fq);
}

/* Keyed for secs: the worst slot error after settle s, the largest step between slots (samples), slots seen. */
static int utc_keyed(double secs, double settle, double *worst, int64_t *max_step)
{
    const int64_t slot = rdof_mod_slot_samples(&U.m.mod);
    const double until = U.t + secs, from = U.t + settle;
    long last_k = U.m.slot_k;
    int64_t last_base = U.m.base;
    int slots = 0;
    *worst = 0, *max_step = 0;
    while (U.t < until) {
        utc_step();
        if (U.m.slot_k <= last_k || !U.m.keyed)
            continue;
        int64_t n, d = llabs(U.m.base - last_base), k = U.m.slot_k - last_k;
        if (d > *max_step * k)
            *max_step = (d + k - 1) / k;
        last_k = U.m.slot_k, last_base = U.m.base;
        double e = utc_slot_err(&n);
        CHECK((n & 1) == ((U.m.slot_k - 1) & 1) && U.m.grid_n0 % 4 == 0, "TS1 on even n");
        if (U.t >= from) {
            worst = fabs(e) > *worst ? (*worst = fabs(e), worst) : worst;
            slots++;
        }
    }
    (void)slot;
    return slots;
}

static void test_utc_tx(void)
{
    double worst;
    int64_t step;
    utc_init(0);
    for (int i = 0; i < 150; i++) /* 3 s for the sample clock */
        utc_step();
    const uint8_t on = 1, off = 0;
    utc_cmd(RDOF_MMDVM_DMR_START, &on, 1);
    int slots = utc_keyed(30, 1, &worst, &step);
    CHECK(slots > 900 && worst < 120 && step <= 1, "keyed 30 s on a 25 ppm fast front end: %d slots, worst %.1f us, "
          "step %lld", slots, worst, (long long)step);
    printf("UTC TX: %d slots within %.1f us of the grid, %u slipped or inserted\n", slots, worst,
           (unsigned)U.m.grid_slips);

    /* out of a map 2 ms off at key-up: steered back a sample a slot, no jump */
    utc_cmd(RDOF_MMDVM_DMR_START, &off, 1);
    U.map_bias_ns = 2000000;
    for (int i = 0; i < 10; i++)
        utc_step();
    utc_cmd(RDOF_MMDVM_DMR_START, &on, 1);
    utc_keyed(0.5, 0, &worst, &step);
    CHECK(worst > 1900, "keyed 2 ms off: %.0f us", worst);
    U.map_bias_ns = 0;
    utc_keyed(5, 4, &worst, &step);
    CHECK(worst < 120 && step <= 1, "converged in 4 s: worst %.1f us, step %lld", worst, (long long)step);

    /* the gate: stop unkeys, refuses data and key-up */
    rdof_sdrmodem_set_transmit(&U.m, false);
    utc_step();
    uint8_t b[1 + RDOF_BURST_BYTES] = {RDOF_SYNC_DATA | RDOF_DT_CSBK};
    U.last_nak = 0;
    utc_cmd(RDOF_MMDVM_DMR_DATA2, b, sizeof b);
    utc_cmd(RDOF_MMDVM_DMR_START, &on, 1);
    utc_step();
    CHECK(!U.m.keyed && U.last_nak == 2, "stopped: keyed %d, NAK %d", U.m.keyed, U.last_nak);
    rdof_sdrmodem_set_transmit(&U.m, true);
    utc_cmd(RDOF_MMDVM_DMR_START, &on, 1);
    utc_keyed(1, 0, &worst, &step);
    CHECK(U.m.keyed && worst < 120, "transmitting again, on the grid from its first slot: %.1f us", worst);

    /* free (no map): the slot clock runs on, unsteered */
    U.map = false;
    rdof_sdrmodem_grid_map(&U.m, false, 0, 0, 0);
    const uint32_t slips = U.m.grid_slips;
    const int64_t base = U.m.base;
    utc_keyed(2, 0, &worst, &step);
    CHECK(U.m.keyed && U.m.grid_slips == slips && U.m.base == base, "no map: unsteered");
    U.map = true;
    rdof_sdrmodem_free(&U.m);
}

/* Heard bursts labeled from the grid: an inbound burst starts 1.25 ms into its slot, TS2 when n is even. */
static void test_utc_rx(void)
{
    for (int offset = 0; offset <= 7000; offset += 7000) {
        utc_init(offset);
        for (int i = 0; i < 150; i++)
            utc_step();
        int wrong = 0;
        for (int64_t j = 0; j < 40; j++) {
            const double t0 = U.t + 0.1;
            const int64_t n = (int64_t)ceil(((UTC0_NS + t0 * 1e9) - offset * 1000.0) / 30e6) + j;
            const double t = ((double)(rdof_grid_boundary(n, offset) - UTC0_NS) + 1.25e6) / 1e9;
            int64_t pos = utc_pos(&U, t) + 40;
            int64_t gn;
            int ts;
            bool ok = rdof_sdrmodem_utc_slot(&U.m, pos, &gn, &ts, NULL);
            rdof_sdr_burst_t sb;
            memset(&sb, 0, sizeof sb);
            sb.b.slot = 1, sb.b.dt = RDOF_DT_CSBK, sb.position = pos;
            rdof_sdrmodem_heard(&U.m, 0, &sb);
            const int want = (n & 1) ? RDOF_MMDVM_DMR_DATA1 : RDOF_MMDVM_DMR_DATA2;
            wrong += !ok || gn != n || ts != ((n & 1) ? 1 : 2) || U.last_cmd != want;
        }
        CHECK(!wrong && U.m.grid_labeled == 40, "offset %d: %d of 40 labeled wrong", offset, wrong);
        rdof_sdrmodem_free(&U.m);
    }
}

/* The TX half's draft UTC additions on its own: INFO, ALIGN, TX_SCHED, the stream mode. */
static void solo_sched(rdof_sdrmodem_t *m, uint32_t c, int fill)
{
    uint8_t b[64] = {RDOF_OPS_SUB_TX_SCHED}, burst[RDOF_BURST_BYTES];
    memset(burst, fill, sizeof burst);
    rdof_ops_schedule(b + 1, c, (c & 1) ? 2 : 1, 0x41, burst);
    d6(m, b, 40);
}

static void solo_align(rdof_sdrmodem_t *m, uint32_t t_us, uint32_t period, uint32_t c)
{
    uint8_t b[16] = {RDOF_OPS_SUB_ALIGN};
    rdof_ops_align(b + 1, t_us, period, c, -1);
    d6(m, b, 13);
}

static uint32_t solo_u32(int at) { return solo_d6[at] | solo_d6[at + 1] << 8 | solo_d6[at + 2] << 16 | (uint32_t)solo_d6[at + 3] << 24; }

static void test_split_tx_utc(void)
{
    static rdof_sdrmodem_t m;
    const rdof_sdrmodem_io_t io = {solo_tx_reply, NULL, solo_tune, NULL, solo_now48};
    solo_now = 0;
    CHECK(rdof_sdrmodem_init(&m, &io, 48000, 48000, false) == 0, "init");
    d6(&m, (uint8_t[]){0}, 1);
    CHECK(solo_d6_len == RDOF_OPS_INFO_LEN && rdof_ops_caps(solo_d6, solo_d6_len) == (RDOF_OPS_SCHEDULE | RDOF_OPS_ALIGN) &&
              rdof_ops_guard_ms(solo_d6, solo_d6_len) == 40 && rdof_ops_horizon(solo_d6, solo_d6_len) == 32 &&
              rdof_ops_air_delay_us(solo_d6, solo_d6_len) == 0,
          "INFO: ALIGN and TX_SCHED, 40 ms, 32 slots, air delay unknown");
    d6(&m, (uint8_t[]){1, 2, 0, 0, 0, 0, 0, 1}, 8);
    CHECK(solo_last_result == 4, "the scheduled mode is the TX half's");
    d6(&m, (uint8_t[]){1, 1, 0, 0, 0, 0, 0, 1}, 8);
    CHECK(solo_last_result == 0 && m.role == 1 && m.stream_mode == 1, "TX half, scheduled");
    m.duplex = true, m.mode = RDOF_MODE_DMR, m.tuned = true;
    solo_pull(&m, 48000);
    solo_sched(&m, 1000, 1);
    CHECK(solo_last_result == 2, "TX_SCHED before an ALIGN: result 2");

    /* unkeyed: snapped, counter 1001 at the target */
    const int64_t tpos = solo_now + 5000;
    solo_align(&m, (uint32_t)(tpos * 125 / 6), 30000000, 1001);
    CHECK(solo_last_result == 0 && solo_d6[4] == (RDOF_OPS_SNAPPING | RDOF_OPS_ALIGNED | RDOF_OPS_PERIOD_APPLIED),
          "ALIGN unkeyed: %d flags %02x", solo_last_result, solo_d6[4]);
    solo_pull(&m, solo_now + 4800);
    d6(&m, (uint8_t[]){3}, 1);
    uint32_t c_now = solo_u32(0), t_now = solo_u32(4);
    CHECK(llabs((int64_t)t_now * 48 / 1000 - (tpos + (int64_t)(int32_t)(c_now - 1001) * 1440)) <= 1 &&
              solo_d6[8] == ((c_now & 1) ? 2 : 1),
          "GET_EPOCH: counter %u at %u us", (unsigned)c_now, (unsigned)t_now);

    /* the outcomes */
    solo_sched(&m, c_now + 1, 1);
    CHECK(solo_last_result == 0 && solo_d6[4] == RDOF_SCHED_LATE, "late %d", solo_d6[4]);
    solo_sched(&m, c_now + 40, 1);
    CHECK(solo_d6[4] == RDOF_SCHED_EARLY, "early %d", solo_d6[4]);
    solo_sched(&m, c_now + 5, 1);
    CHECK(solo_d6[4] == RDOF_SCHED_QUEUED && solo_u32(0) == c_now + 5, "queued");
    solo_sched(&m, c_now + 5, 2);
    CHECK(solo_d6[4] == RDOF_SCHED_REPLACED, "replaced");
    for (uint32_t c = c_now + 6; c < c_now + 21; c++)
        solo_sched(&m, c, 3);
    CHECK(solo_d6[4] == RDOF_SCHED_QUEUED, "16 held");
    solo_sched(&m, c_now + 21, 1);
    CHECK(solo_d6[4] == RDOF_SCHED_FULL, "full");
    uint8_t bad[64] = {RDOF_OPS_SUB_TX_SCHED}, burst[RDOF_BURST_BYTES] = {0};
    rdof_ops_schedule(bad + 1, c_now + 22, 1, 0x41, burst);
    bad[5] = (c_now + 22) & 1 ? 1 : 2;
    d6(&m, bad, 40);
    CHECK(solo_last_result == 4, "the wrong slot for its counter: result 4");
    const uint32_t refused = m.refused;
    uint8_t f[48], b[1 + RDOF_BURST_BYTES] = {RDOF_SYNC_DATA | RDOF_DT_CSBK};
    rdof_sdrmodem_write(&m, f, rdof_mmdvm_frame(RDOF_MMDVM_DMR_DATA2, b, sizeof b, f, sizeof f));
    CHECK(m.refused == refused + 1, "DMR_DATA in the scheduled mode: NAK");

    /* keyed by DMR_START as the FIFO mode is: on air in their slots, idle between */
    rdof_sdrmodem_write(&m, (uint8_t[]){0xE0, 0x04, RDOF_MMDVM_DMR_START, 1}, 4);
    solo_pull(&m, solo_now + 48 * 30 * 30);
    CHECK(m.keyed && m.sched_sent == 16 && m.sched_late == 1 && m.idle_bursts > 0, "sent %u late %u idle %u",
          (unsigned)m.sched_sent, (unsigned)m.sched_late, (unsigned)m.idle_bursts);
    d6(&m, (uint8_t[]){6}, 1);
    rdof_ops_status_t st;
    CHECK(solo_d6_len == RDOF_OPS_STATUS_LEN && rdof_ops_status_ext(solo_d6, solo_d6_len, &st) && st.sent == 16 &&
              st.late == 1 && st.early_full == 2 && st.replaced == 1 &&
              st.flags == (RDOF_OPS_ALIGNED | RDOF_OPS_COUNTER_BASE),
          "STATUS: sent, late, early or full, replaced, the ALIGN flags");

    /* keyed: the counter only within 1 ms; beyond, refused either way RadioFoundry answers (2) */
    d6(&m, (uint8_t[]){3}, 1);
    c_now = solo_u32(0), t_now = solo_u32(4);
    solo_align(&m, t_now + 300 + 30000 * 3, 0, c_now + 1003);
    CHECK(solo_last_result == 0 && solo_d6[4] == RDOF_OPS_ALIGNED && abs((int32_t)solo_u32(0) + 300) <= 21 &&
              m.counter_base + (uint32_t)(m.slot_k) - (c_now + 1000) < 40,
          "keyed 0.3 ms: flags %02x phase %d", solo_d6[4], (int)solo_u32(0));
    solo_align(&m, t_now + 2000, 0, c_now);
    CHECK(solo_last_result == 0 && solo_d6[4] == RDOF_OPS_NOT_APPLIED && abs((int32_t)solo_u32(0) + 2000) <= 21,
          "keyed 2 ms: result 0, flag 10, the phase (%d)", (int)solo_u32(0));

    /* a queued burst keys it up ~150 ms ahead; a key-up the queue made ends 1 s after the last */
    rdof_sdrmodem_write(&m, (uint8_t[]){0xE0, 0x04, RDOF_MMDVM_DMR_START, 0}, 4);
    solo_pull(&m, solo_now + 4800);
    CHECK(!m.keyed, "unkeyed by DMR_START 00");
    d6(&m, (uint8_t[]){3}, 1);
    c_now = solo_u32(0);
    solo_sched(&m, c_now + 10, 9);
    solo_pull(&m, solo_now + 480);
    CHECK(!m.keyed, "300 ms ahead: not yet");
    solo_pull(&m, solo_now + 48 * 160);
    CHECK(m.keyed && m.queue_keyed, "keyed by the queue");
    solo_pull(&m, solo_now + 48000 / 2);
    CHECK(m.keyed, "still keyed 0.5 s on");
    for (int i = 0; i < 10; i++) {
        d6(&m, (uint8_t[]){2, 0, 0, 0, 0, 0, 0, 0, 0}, 9);
        solo_pull(&m, solo_now + 4800);
    }
    CHECK(!m.keyed, "unkeyed 1 s after the last");

    /* a retune: snapped back onto the last ALIGN's grid by itself at the next pull */
    const double pos0 = m.align_pos;
    uint8_t fq[9] = {0};
    rdof_sdrmodem_write(&m, f, rdof_mmdvm_frame(RDOF_MMDVM_SET_FREQ, fq, sizeof fq, f, sizeof f));
    solo_sched(&m, c_now + 400, 1);
    d6(&m, (uint8_t[]){6}, 1);
    CHECK(rdof_ops_status_ext(solo_d6, solo_d6_len, &st) && st.flags == RDOF_OPS_SNAPPING && !m.aligned,
          "retuned: snapping back (0x%02x)", st.flags);
    solo_pull(&m, solo_now + 4800);
    d6(&m, (uint8_t[]){6}, 1);
    CHECK(m.aligned && m.align_pos == pos0 && rdof_ops_status_ext(solo_d6, solo_d6_len, &st) &&
              st.flags == (RDOF_OPS_ALIGNED | RDOF_OPS_COUNTER_BASE),
          "snapped back (0x%02x)", st.flags);
    rdof_sdrmodem_free(&m);
}

/* The split matrix's UTC rows: an SDR TX half aligned by the host (rdof_split_set_utc) over its ALIGN, with either RX
 * half; its slots on the UTC grid unkeyed and keyed, and the RX half's epoch following. */
#define UTC_MINUS_HOST_US (1790000000000000.0 - 1000000000.0)

static void test_pair_utc(int rx_kind)
{
    static pair_t p;
    char name[40];
    snprintf(name, sizeof name, "UTC: SDR TX + %s RX", KIND_NAMES[rx_kind]);
    pair_init(&p, SDR, rx_kind, true);
    rdof_split_set_utc(&p.s, true, UTC_MINUS_HOST_US, 10, 0, true, p.clock);
    uint8_t cfg[20] = {0};
    cfg[3] = RDOF_MODE_DMR;
    cfg[6] = 1;
    upstream(&p, RDOF_MMDVM_SET_CONFIG, cfg, sizeof cfg);
    uint8_t fq[10] = {0};
    for (int i = 0; i < 4; i++)
        fq[1 + i] = (uint8_t)(RXF >> (8 * i)), fq[5 + i] = (uint8_t)(TXF >> (8 * i));
    upstream(&p, RDOF_MMDVM_SET_FREQ, fq, sizeof fq);
    run_to(&p, p.clock + 30);
    rdof_sdrmodem_t *tx = &p.h[0].m;
    const int64_t slot = rdof_mod_slot_samples(&tx->mod);
    double worst = 0;
    int bad_n = 0;
    for (int keyed = 0; keyed < 2; keyed++) {
        if (keyed) {
            const uint8_t on = 1;
            upstream(&p, RDOF_MMDVM_DMR_START, &on, 1);
        }
        for (int i = 0; i < 100; i++) {
            run_to(&p, p.clock + 0.02);
            const long k = tx->slot_k - (tx->keyed ? 1 : 0);
            const double u = host_of(&p.h[0], (double)(tx->base + k * slot)) * 1e6 + UTC_MINUS_HOST_US;
            const double ph = fmod(u, 30000);
            const double e = ph > 15000 ? ph - 30000 : ph;
            worst = fabs(e) > worst ? fabs(e) : worst;
            bad_n += tx->counter_base + (uint32_t)k != rdof_slot_counter(llround(u / 30000));
        }
    }
    CHECK(p.s.align_state == RDOF_ALIGN_LOCKED && tx->keyed && worst < 60 && !bad_n && p.s.epoch_ok,
          "%s: %s, worst %.1f us, %d slots numbered wrong", name, rdof_align_state_name(p.s.align_state), worst, bad_n);
    printf("%s: slots within %.1f us of UTC, unkeyed and keyed\n", name, worst);
    double e;
    int wrong = 0;
    for (int i = 0; i < 20; i++) {
        const int sl = i % 2 ? 1 : 2;
        inject(&p, sl, RDOF_DT_CSBK, slot_time(&p, sl, 0.1, &e));
        frames_t f = take_up(&p);
        int got = 0;
        for (int j = 0; j < f.n; j++)
            if (f.cmds[j] == RDOF_MMDVM_DMR_DATA1 || f.cmds[j] == RDOF_MMDVM_DMR_DATA2)
                got = f.cmds[j];
        wrong += got != (sl == 1 ? RDOF_MMDVM_DMR_DATA1 : RDOF_MMDVM_DMR_DATA2);
    }
    CHECK(!wrong, "%s: %d of 20 inbound on the wrong slot", name, wrong);
    pair_free(&p);
}

#endif

/* ── dual capacity direct mode: two simplex calls on one frequency, one a slot ────────────────────────────────
 *
 * Bursts are built with the TS1 / TS2 direct mode syncs (ETSI TS 102 361-1 9.1.1) and put on air by modulators at
 * the TDMA direct mode timing: one burst a slot every 60 ms, TS2 30 ms after TS1. The SDR receive path must keep the
 * two calls apart; the SDR modem in DCDM, under the repeater's dcdm mode, must send network calls on either slot
 * framed and timed for the channel while it hears the other slot. */

#if RDOF_FEATURE_REPEATER && RDOF_FEATURE_SDR_RECEIVER

#include "rdof/bits.h"
#include "rdof/hblink.h"
#include "rdof/repeater.h"
#include "rdof/sdr/receiver.h"

#define DC_FS 288000
#define DC_CENTER 461000000.0
#define DC_CH 461037500u
#define DC_CHUNK 4608
#define DC_CC 1

static uint64_t ts_voice(int ts) { return ts == 1 ? RDOF_TS1_VOICE : RDOF_TS2_VOICE; }

/* A voice call's bursts on slot ts, direct mode framed: `headers` LC headers, nvoice voice bursts, a terminator.
 * Returns the count; controls[] gets each one's DMR_DATA control byte. */
static int dc_call(int ts, uint32_t src, uint32_t dst, int headers, int nvoice, uint8_t (*out)[RDOF_BURST_BYTES],
                   uint8_t *controls)
{
    uint8_t lc9[9], raw[128];
    rdof_group_lc(src, dst, true, 0, lc9);
    rdof_embedded_lc_encode(lc9, raw);
    int n = 0;
    for (int i = 0; i < headers; i++, n++) {
        rdof_lc_burst(lc9, RDOF_DT_VOICE_LC_HEADER, DC_CC, rdof_data_sync(ts_voice(ts)), out[n]);
        controls[n] = RDOF_SYNC_DATA | RDOF_DT_VOICE_LC_HEADER;
    }
    for (int i = 0; i < nvoice; i++, n++) {
        int p = i % 6;
        controls[n] = rdof_silence_burst(p, DC_CC, false, p >= 1 && p <= 4 ? raw + 32 * (p - 1) : NULL, out[n]);
        if (p == 0)
            rdof_set_field(out[n], 108, 48, ts_voice(ts));
    }
    rdof_lc_burst(lc9, RDOF_DT_TERMINATOR_WITH_LC, DC_CC, rdof_data_sync(ts_voice(ts)), out[n]);
    controls[n++] = RDOF_SYNC_DATA | RDOF_DT_TERMINATOR_WITH_LC;
    return n;
}

/* A station: its bursts every 60 ms from first (modulator samples, symbol 0), on the channel. */
static void dc_station(rdof_mod_t *m, int64_t first, uint8_t (*b)[RDOF_BURST_BYTES], int n)
{
    rdof_mod_init(m, DC_FS, (double)DC_CH - DC_CENTER, 4.0, 1.0f, 64, 0);
    const int64_t two = 2 * rdof_mod_slot_samples(m);
    for (int k = 0; k < n; k++)
        rdof_mod_queue_burst(m, first + k * two, b[k], NULL, RDOF_MOD_RAMPS);
}

static uint32_t dc_rng = 777;
static float dc_noise(void)
{
    float s = 0;
    for (int i = 0; i < 4; i++) {
        dc_rng = dc_rng * 1664525u + 1013904223u;
        s += (float)(dc_rng >> 8) / 16777216.0f - 0.5f;
    }
    return s * 1.732f;
}

/* The SDR receive path (an sdr box's channel): two DCDM calls at once, from two radios, as two calls, each with
 * its own HBlink4 stream on its slot. */
typedef struct {
    int starts[3], ends[3], json_other;
    int dmrd[3], voice[3];
    uint32_t stream[3];
    bool stream_mixed;
    int family[3][RDOF_FAMILY_COUNT];
} dcrx_t;

static void dcrx_json(void *ctx, const char *ch, const char *json)
{
    dcrx_t *d = ctx;
    int slot = strstr(json, "\"slot\":1") ? 1 : strstr(json, "\"slot\":2") ? 2 : 0;
    if (strstr(json, "\"action\":\"start\""))
        d->starts[slot]++;
    else if (strstr(json, "\"action\":\"end\""))
        d->ends[slot]++;
    else if (strstr(json, "\"action\":\"lost\"") || strstr(json, "late_entry"))
        d->json_other++;
}

static void dcrx_send(void *ctx, const uint8_t *p, size_t n)
{
    dcrx_t *d = ctx;
    if (n != RDOF_DMRD_LEN || memcmp(p, "DMRD", 4))
        return;
    int slot = p[15] & 0x80 ? 2 : 1;
    uint32_t sid = (uint32_t)p[16] << 24 | (uint32_t)p[17] << 16 | (uint32_t)p[18] << 8 | p[19];
    if (d->dmrd[slot] && d->stream[slot] != sid)
        d->stream_mixed = true;
    d->stream[slot] = sid;
    d->dmrd[slot]++;
    if ((p[15] & 0x30) != 0x20)
        d->voice[slot]++;
}

static uint32_t dcrx_rand(void *ctx)
{
    static uint32_t x = 1;
    return x += 0x9E3779B9u;
}

static void dcrx_burst(void *ctx, int c, const rdof_sdr_burst_t *b, double t)
{
    dcrx_t *d = ctx;
    if (b->b.dt != RDOF_DT_IDLE && b->b.slot >= 1 && b->b.slot <= 2)
        d->family[b->b.slot][b->b.family]++;
}

static void test_dcdm_receive(void)
{
    static dcrx_t d;
    static rdof_sdr_rx_t rx;
    static rdof_hb_sink_t hb;
    memset(&d, 0, sizeof d);
    rdof_hb_sink_init(&hb, 4242);
    hb.send = dcrx_send;
    hb.rand32 = dcrx_rand;
    hb.ctx = &d;
    const rdof_sink_t js = {.on_json = dcrx_json, .ctx = &d};
    const rdof_sdr_rx_config_t rc = {DC_FS, DC_CENTER, 0, 0, true, DC_CHUNK};
    const rdof_sdr_channel_t ch = {"A", DC_CH, DC_CC, 2};
    CHECK(rdof_sdr_rx_init(&rx, &rc, &ch, 1, &js, 0) == 0, "receiver");
    rdof_sink_t hs = rdof_hb_sink_as_sink(&hb);
    CHECK(rdof_sdr_rx_add_sink(&rx, 0, &hs), "hb sink");
    rx.on_burst = dcrx_burst;
    rx.burst_ctx = &d;

    static uint8_t a[40][RDOF_BURST_BYTES], b[40][RDOF_BURST_BYTES], ca[40], cb[40];
    const int na = dc_call(1, 9990011, 91, 3, 18, a, ca), nb = dc_call(2, 9990012, 92, 3, 12, b, cb);
    static rdof_mod_t ma, mb;
    const int64_t slot = DC_FS * 30 / 1000;
    dc_station(&ma, DC_FS / 10, a, na);                    /* TS1 from 0.1 s */
    dc_station(&mb, DC_FS / 10 + slot + 4 * 2 * slot, b, nb); /* TS2 30 ms off it, four bursts later */
    static rdof_cf_t ia[DC_CHUNK], ib[DC_CHUNK];
    for (int c = 0; c < (int)(2.5 * DC_FS / DC_CHUNK); c++) {
        rdof_mod_iq(&ma, ia, DC_CHUNK);
        rdof_mod_iq(&mb, ib, DC_CHUNK);
        for (int i = 0; i < DC_CHUNK; i++) {
            ia[i].re = 0.5f * ia[i].re + 0.3f * ib[i].re + 0.02f * dc_noise(); /* the other radio 4 dB weaker */
            ia[i].im = 0.5f * ia[i].im + 0.3f * ib[i].im + 0.02f * dc_noise();
        }
        rdof_sdr_rx_process(&rx, ia, DC_CHUNK);
    }
    CHECK(d.starts[1] == 1 && d.ends[1] == 1 && d.starts[2] == 1 && d.ends[2] == 1 && !d.starts[0] && !d.json_other,
          "two calls, one a slot: starts %d %d, ends %d %d, other %d", d.starts[1], d.starts[2], d.ends[1], d.ends[2],
          d.json_other);
    CHECK(d.family[1][RDOF_FAMILY_TS1] == na && d.family[2][RDOF_FAMILY_TS2] == nb && !d.family[1][RDOF_FAMILY_TS2] &&
              !d.family[2][RDOF_FAMILY_TS1],
          "every burst on its slot: TS1 %d of %d, TS2 %d of %d", d.family[1][RDOF_FAMILY_TS1], na,
          d.family[2][RDOF_FAMILY_TS2], nb);
    CHECK(d.voice[1] == 18 && d.voice[2] == 12 && d.dmrd[1] >= na && d.dmrd[2] >= nb && !d.stream_mixed &&
              d.stream[1] != d.stream[2],
          "HBlink4: %d / %d DMRD, %d / %d voice, own streams %d", d.dmrd[1], d.dmrd[2], d.voice[1], d.voice[2],
          !d.stream_mixed);
    rdof_mod_free(&ma);
    rdof_mod_free(&mb);
    rdof_sdr_rx_free(&rx);
}

/* The SDR modem on its own: DCDM asked for and offered, both slots' space, slot 1 data taken. */
typedef struct {
    rdof_sdrmodem_t m;
    rdof_modem_t client;
    rdof_radio_t radio;
    double t;
    int naks;
} dcm_t;

static void dcm_reply(void *ctx, const uint8_t *f, size_t n) { rdof_modem_feed(&((dcm_t *)ctx)->client, f, n); }
static int dcm_channel(void *ctx, uint32_t hz) { return hz == DC_CH ? 0 : -1; }
static bool dcm_tune(void *ctx, uint32_t hz, double *off)
{
    *off = (double)hz - DC_CENTER;
    return fabs(*off) < DC_FS * 0.4;
}
static bool dcm_write(void *ctx, const uint8_t *d, size_t n)
{
    rdof_sdrmodem_write(&((dcm_t *)ctx)->m, d, n);
    return true;
}
static bool dcm_wait(void *ctx, int ms) { return ((dcm_t *)ctx)->client.reply_ready; }
static double dcm_clock(void *ctx) { return ((dcm_t *)ctx)->t; }
static void dcm_sleep(void *ctx, int ms) { ((dcm_t *)ctx)->t += ms / 1e3; }
static void dcm_nak(void *ctx, uint8_t cmd, int err) { ((dcm_t *)ctx)->naks++; }

static void dcm_init(dcm_t *x)
{
    memset(x, 0, sizeof *x);
    const rdof_sdrmodem_io_t io = {dcm_reply, dcm_channel, dcm_tune, x, NULL};
    CHECK(rdof_sdrmodem_init(&x->m, &io, DC_FS, DC_FS, false) == 0, "init");
    const rdof_modem_port_t port = {dcm_write, dcm_wait, NULL, dcm_clock, dcm_sleep, x};
    rdof_modem_init(&x->client, &port);
    x->client.on_nak = dcm_nak;
    x->client.cb_ctx = x;
    CHECK(rdof_modem_start(&x->client, 1), "version");
    rdof_radio_init(&x->radio, &x->client, rdof_radio_detect(x->client.version));
}

static void test_dcdm_modem(void)
{
    static dcm_t x;
    dcm_init(&x);
    CHECK(rdof_dcdm_capable(x.client.version) && rdof_radio_detect(x.client.version) == RDOF_RADIO_MMDVM_HS,
          "offered: %s", x.client.version);
    uint8_t b[RDOF_BURST_BYTES] = {0};
    /* plain simplex: no slot 1 */
    CHECK(rdof_radio_tune(&x.radio, DC_CH, DC_CC, 100) && !x.m.dcdm, "simplex");
    rdof_modem_write_dmr(&x.client, RDOF_SYNC_DATA | RDOF_DT_CSBK, b, 1);
    CHECK(x.naks == 1 && x.m.qn[0] == 0, "slot 1 refused in plain simplex");
    rdof_mmdvm_status_t st;
    CHECK(rdof_modem_status(&x.client, &st) && st.space1 == 10, "slot 1 space the constant: %d", st.space1);
    /* DCDM */
    CHECK(rdof_radio_tune_dcdm(&x.radio, DC_CH, DC_CC, 100) && x.m.dcdm && !x.m.duplex, "dcdm: %s", x.client.error);
    rdof_modem_write_dmr(&x.client, RDOF_SYNC_DATA | RDOF_DT_CSBK, b, 1);
    rdof_modem_write_dmr(&x.client, RDOF_SYNC_DATA | RDOF_DT_CSBK, b, 1);
    rdof_modem_write_dmr(&x.client, RDOF_SYNC_DATA | RDOF_DT_CSBK, b, 2);
    CHECK(x.naks == 1 && x.m.qn[0] == 2 && x.m.qn[1] == 1, "both slots taken: %d %d", x.m.qn[0], x.m.qn[1]);
    CHECK(rdof_modem_status(&x.client, &st) && st.space1 == RDOF_SDRMODEM_QUEUE - 2 &&
              st.space == RDOF_SDRMODEM_QUEUE - 1 && st.transmitting,
          "both slots' space: %d %d", st.space1, st.space);
    rdof_modem_dmr_abort(&x.client, 1);
    CHECK(x.m.qn[0] == 0 && x.m.qn[1] == 1, "DMR_ABORT 1 drops slot 1 only");
    /* heard: labeled by sync */
    rdof_sdr_burst_t h;
    memset(&h, 0, sizeof h);
    h.b.dt = RDOF_DT_CSBK;
    const rdof_family_t fams[3] = {RDOF_FAMILY_TS1, RDOF_FAMILY_TS2, RDOF_FAMILY_MS};
    for (int k = 0; k < 3; k++) {
        h.b.family = fams[k];
        h.b.slot = k == 0 ? 1 : 2;
        h.position = 100000 + 2880 * k;
        uint32_t before = x.m.rx_bursts;
        int i = k == 0 ? 0 : 1;
        int64_t last = x.m.last48[i];
        rdof_sdrmodem_heard(&x.m, 0, &h);
        CHECK(x.m.rx_bursts == before + 1 && x.m.last48[i] != last, "%s up on slot %d", rdof_family_name(fams[k]),
              i + 1);
    }
    CHECK(x.m.dc_timed && x.m.dc_timings == 2, "timing from TS1 / TS2 bursts, not MS: %u", (unsigned)x.m.dc_timings);
    /* back to plain simplex: slot 1 refused again */
    CHECK(rdof_radio_tune(&x.radio, DC_CH, DC_CC, 100) && !x.m.dcdm, "simplex again");
    rdof_modem_write_dmr(&x.client, RDOF_SYNC_DATA | RDOF_DT_CSBK, b, 1);
    CHECK(x.naks == 2, "slot 1 refused");
    rdof_sdrmodem_free(&x.m);
}

/* End to end: the repeater in dcdm mode over the SDR modem; what it sends goes on air into the receiver with other
 * radios' DCDM calls, and what the receiver hears goes back to the modem. */
typedef struct {
    dcm_t x;
    rdof_repeater_t r;
    rdof_sdr_rx_t rx;
    int up[3], up_bursts[3][64];
    uint8_t up_b[3][64][RDOF_BURST_BYTES];
    int64_t on_air[3][64]; /* positions the receiver heard, by the slot it said */
    int n_air[3], fam_air[3][RDOF_FAMILY_COUNT];
    int tx_starts, tx_ends;
} dce_t;

static void dce_rx(void *ctx, int slot, uint8_t control, const uint8_t *burst, int rssi)
{
    rdof_repeater_heard(&((dce_t *)ctx)->r, slot, control, burst, rssi);
}
static void dce_lost(void *ctx, int slot) { rdof_repeater_lost(&((dce_t *)ctx)->r, slot); }
static void dce_nak(void *ctx, uint8_t cmd, int err) { rdof_repeater_nak(&((dce_t *)ctx)->r, cmd, err); }
static void dce_hear(void *ctx, int slot, uint8_t control, const uint8_t *burst, double dbm, int cc, uint32_t freq)
{
    dce_t *e = ctx;
    if (slot >= 1 && slot <= 2 && e->up[slot] < 64)
        memcpy(e->up_b[slot][e->up[slot]], burst, RDOF_BURST_BYTES);
    if (slot >= 0 && slot <= 2)
        e->up[slot]++;
}
static void dce_on_tx(void *ctx, const char *action, const rdof_tx_call_info_t *c, double d)
{
    dce_t *e = ctx;
    if (!strcmp(action, "start"))
        e->tx_starts++;
    else
        e->tx_ends++;
}
static double dce_clock(void *ctx) { return ((dce_t *)ctx)->x.t; }
static void dce_burst(void *ctx, int c, const rdof_sdr_burst_t *b, double t)
{
    dce_t *e = ctx;
    rdof_sdrmodem_heard(&e->x.m, c, b);
    if (b->b.dt == RDOF_DT_IDLE || b->b.slot < 1 || b->b.slot > 2)
        return;
    if (e->n_air[b->b.slot] < 64)
        e->on_air[b->b.slot][e->n_air[b->b.slot]] = b->position;
    e->n_air[b->b.slot]++;
    e->fam_air[b->b.slot][b->b.family]++;
}
static bool dce_muted(void *ctx, int64_t p) { return rdof_sdrmodem_muted_at(&((dce_t *)ctx)->x.m, p); }

static void dce_net_call(dce_t *e, uint32_t sid, int slot, uint32_t src, uint32_t dst, int nvoice)
{
    static uint8_t b[40][RDOF_BURST_BYTES], c[40];
    int n = dc_call(slot, src, dst, 1, nvoice, b, c); /* as HBlink4 relays it: one header */
    for (int i = 0; i < n; i++) {
        rdof_net_burst_t nb = {.control = c[i], .terminator = i == n - 1, .src = src, .dst = dst, .group = true,
                               .slot = slot, .seq = (uint8_t)i};
        nb.stream_id[0] = (uint8_t)(sid >> 24), nb.stream_id[1] = (uint8_t)(sid >> 16);
        nb.stream_id[2] = (uint8_t)(sid >> 8), nb.stream_id[3] = (uint8_t)sid;
        memcpy(nb.burst, b[i], RDOF_BURST_BYTES);
        rdof_repeater_on_burst(&e->r, &nb);
    }
}

static dce_t *dce_new(void)
{
    static dce_t e;
    memset(&e, 0, sizeof e);
    dcm_init(&e.x);
    e.x.client.on_rx = dce_rx;
    e.x.client.on_lost = dce_lost;
    e.x.client.on_nak = dce_nak;
    e.x.client.cb_ctx = &e;
    rdof_repeater_init(&e.r, &e.x.radio, 4243, DC_CH, DC_CH, DC_CC);
    rdof_repeater_set_dcdm(&e.r);
    e.r.clock = dce_clock;
    e.r.on_tx = dce_on_tx;
    e.r.receiver.hear = dce_hear;
    e.r.receiver.ctx = &e;
    e.r.ctx = &e;
    const rdof_sdr_rx_config_t rc = {DC_FS, DC_CENTER, 0, 0, true, DC_CHUNK};
    const rdof_sdr_channel_t ch = {"A", DC_CH, DC_CC, 2};
    CHECK(rdof_sdr_rx_init(&e.rx, &rc, &ch, 1, NULL, 0) == 0, "receiver");
    e.rx.on_burst = dce_burst;
    e.rx.muted = dce_muted;
    e.rx.burst_ctx = &e;
    CHECK(rdof_repeater_start(&e.r) && e.x.m.dcdm, "start: %s", e.x.client.error);
    return &e;
}

/* Runs s seconds: the repeater polled every chunk (16 ms), the modem's and the stations' signals into the receiver.
 * net_at: when to call net(e) (once), -1 never. */
static void dce_run(dce_t *e, rdof_mod_t *st, int nst, double s, double net_at, void (*net)(dce_t *))
{
    static rdof_cf_t iq[DC_CHUNK], o[DC_CHUNK];
    const int chunks = (int)(s * DC_FS / DC_CHUNK);
    for (int c = 0; c < chunks; c++) {
        if (net && net_at >= 0 && e->x.t >= net_at) {
            net(e);
            net_at = -1;
        }
        CHECK(rdof_repeater_poll(&e->r), "poll");
        rdof_sdrmodem_pull_iq(&e->x.m, e->rx.samples, iq, DC_CHUNK);
        for (int i = 0; i < DC_CHUNK; i++)
            iq[i].re *= 0.5f, iq[i].im *= 0.5f;
        for (int k = 0; k < nst; k++) {
            rdof_mod_iq(&st[k], o, DC_CHUNK);
            for (int i = 0; i < DC_CHUNK; i++)
                iq[i].re += 0.4f * o[i].re, iq[i].im += 0.4f * o[i].im;
        }
        for (int i = 0; i < DC_CHUNK; i++)
            iq[i].re += 0.02f * dc_noise(), iq[i].im += 0.02f * dc_noise();
        rdof_sdr_rx_process(&e->rx, iq, DC_CHUNK);
        e->x.t += (double)DC_CHUNK / DC_FS;
    }
}

static void dce_free(dce_t *e)
{
    rdof_sdr_rx_free(&e->rx);
    rdof_sdrmodem_free(&e->x.m);
}

static int dce_count(dce_t *e, int slot, const uint8_t (*b)[RDOF_BURST_BYTES], int n)
{
    int k = 0;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < e->up[slot] && j < 64; j++)
            if (!memcmp(e->up_b[slot][j], b[i], RDOF_BURST_BYTES)) {
                k++;
                break;
            }
    return k;
}

/* Every position in a (slot sa) is 30 ms (1440 at 48 kS/s) off one in b, within 2 samples, mod 60 ms. */
static bool interleaved(const int64_t *a, int na, const int64_t *b, int nb)
{
    if (!na || !nb)
        return false;
    for (int i = 0; i < na; i++) {
        int64_t d = ((a[i] - b[0]) % 2880 + 2880) % 2880;
        if (llabs(d - 1440) > 2)
            return false;
    }
    return true;
}

static void net_ts1(dce_t *e) { dce_net_call(e, 0xDC01, 1, 9990021, 93, 12); }
static void net_both(dce_t *e)
{
    dce_net_call(e, 0xDC02, 1, 9990022, 94, 12);
    dce_net_call(e, 0xDC03, 2, 9990023, 95, 12);
}

static void test_dcdm_e2e(void)
{
    /* 1. A radio on TS2 (RF up) while the network sends on TS1 (down), on its timing */
    dce_t *e = dce_new();
    static uint8_t b[40][RDOF_BURST_BYTES], c[40];
    const int nb = dc_call(2, 9990031, 96, 3, 30, b, c);
    rdof_mod_t st[2];
    dc_station(&st[0], DC_FS / 20 + 1234, b, nb); /* from 50 ms, on a timing of its own */
    dce_run(e, st, 1, 3.4, 0.3, net_ts1);
    CHECK(dce_count(e, 2, (const uint8_t(*)[RDOF_BURST_BYTES])b, nb) == nb && e->up[1] == 0 && e->up[0] == 0,
          "RF up: %d of %d bursts on slot 2, %d on slot 1", dce_count(e, 2, (const uint8_t(*)[RDOF_BURST_BYTES])b, nb),
          nb, e->up[1]);
    CHECK(e->fam_air[1][RDOF_FAMILY_TS1] == 8 + 12 + 3 && e->fam_air[1][RDOF_FAMILY_TS2] == 0,
          "network down on TS1: %d bursts (headers 8, voice 12, terminators 3)", e->fam_air[1][RDOF_FAMILY_TS1]);
    CHECK(e->x.m.muted == (uint32_t)e->fam_air[1][RDOF_FAMILY_TS1], "ours muted: %u", (unsigned)e->x.m.muted);
    CHECK(interleaved(e->on_air[1], e->n_air[1] < 64 ? e->n_air[1] : 64, e->on_air[2], 1),
          "TS1 30 ms off the radio's TS2");
    CHECK(e->tx_starts == 1 && e->tx_ends == 1 && e->x.m.dc_timings >= 1, "one TX call, timing heard");
    rdof_mod_free(&st[0]);
    dce_free(e);

    /* 2. The network on both slots at once, nothing on RF: both on air, 30 ms apart */
    e = dce_new();
    dce_run(e, NULL, 0, 2.0, 0.1, net_both);
    CHECK(e->fam_air[1][RDOF_FAMILY_TS1] == 23 && e->fam_air[2][RDOF_FAMILY_TS2] == 23 && !e->up[1] && !e->up[2],
          "both down: TS1 %d, TS2 %d, up %d %d", e->fam_air[1][RDOF_FAMILY_TS1], e->fam_air[2][RDOF_FAMILY_TS2], e->up[1],
          e->up[2]);
    CHECK(interleaved(e->on_air[1], e->n_air[1] < 64 ? e->n_air[1] : 64, e->on_air[2], 1), "TS1 and TS2 30 ms apart");
    CHECK(e->tx_starts == 2 && e->tx_ends == 2, "two TX calls: %d %d", e->tx_starts, e->tx_ends);
    dce_free(e);

    /* 3. Two radios, one a slot: both up on their own slots; the network refused both slots meanwhile (R8.2) */
    e = dce_new();
    static uint8_t a[40][RDOF_BURST_BYTES], ca[40];
    const int na = dc_call(1, 9990041, 97, 3, 24, a, ca);
    const int64_t slot = DC_FS * 30 / 1000;
    dc_station(&st[0], DC_FS / 20, a, na);
    dc_station(&st[1], DC_FS / 20 + 3 * slot, b, nb);
    dce_run(e, st, 2, 3.4, 0.6, net_both);
    CHECK(dce_count(e, 1, (const uint8_t(*)[RDOF_BURST_BYTES])a, na) == na &&
              dce_count(e, 2, (const uint8_t(*)[RDOF_BURST_BYTES])b, nb) == nb,
          "both up: %d of %d, %d of %d", dce_count(e, 1, (const uint8_t(*)[RDOF_BURST_BYTES])a, na), na,
          dce_count(e, 2, (const uint8_t(*)[RDOF_BURST_BYTES])b, nb), nb);
    CHECK(e->tx_starts == 0 && e->x.m.tx_bursts == 0, "nothing sent over them: %d calls, %u bursts", e->tx_starts,
          (unsigned)e->x.m.tx_bursts);
    printf("dcdm: two radios up on TS1 and TS2 (%d and %d bursts), the network down on TS1 beside RF TS2 and on both "
           "slots at once, 30 ms apart, ours muted\n",
           dce_count(e, 1, (const uint8_t(*)[RDOF_BURST_BYTES])a, na),
           dce_count(e, 2, (const uint8_t(*)[RDOF_BURST_BYTES])b, nb));
    rdof_mod_free(&st[0]);
    rdof_mod_free(&st[1]);
    dce_free(e);
}

#endif

int main(void)
{
    char report[1024];
#if RDOF_FEATURE_REPEATER
    test_cach();
    test_split_matrix();
    test_split_rx_solo();
    test_split_tx_solo();
    test_split_tx_utc();
    test_utc_tx();
    test_utc_rx();
    test_pair_utc(RADIO);
    test_pair_utc(SDR);
#endif
#if RDOF_FEATURE_REPEATER && RDOF_FEATURE_SDR_RECEIVER
    test_dcdm_receive();
    test_dcdm_modem();
    test_dcdm_e2e();
#endif
    for (int full = 0; full < 2; full++) {
        int f = rdof_sdrmodem_loopback(full, report, sizeof report);
        printf("%s%s\n", f ? "FAIL " : "", report);
        fails += f;
    }
    return fails ? 1 : 0;
}
