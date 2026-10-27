// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
// Host tests of the raw I/Q session (src/lr2021_iq.c) over the mock HAL and a model of the chip: its RX FIFO fills at
// the plan's rate from when SetRx starts it, overflows past 1 KB (the flag set, the bytes lost), raises the IRQ pin at
// the configured level, and drains its TX FIFO at the run's rate. The stream's bytes count up, so the samples a sink
// gets show whether any went missing or came twice.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lr2021_iq.h"
#include "mock_hal.h"

static int checks, fails;

#define CHECK(cond, ...)                                \
    do {                                                \
        checks++;                                       \
        if (!(cond)) {                                  \
            fails++;                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

#define ST_OK 0x04   /* status: command OK (bits 11:9 = 2) */
#define ST_DATA 0x06 /* data follows */
#define TX_DONE (1u << 19)

static struct {
    uint8_t ver[2];
    uint32_t marker;
    bool rx;               /* SetRx sent, not yet SetStandby */
    double bytes_per_us;   /* the plan's, set by the test */
    double carry;
    int64_t last_us;
    int level;             /* bytes in the RX FIFO */
    uint64_t made;         /* stream bytes made so far (each byte is made & 0xFF) */
    uint8_t rx_flags;
    int irq_high_at;       /* ConfigFifoIrq's level, 0 off */
    uint16_t pending;      /* the read command whose answer comes next */
    uint32_t pending_addr;
    uint16_t ops[512];     /* every command, in order */
    uint8_t args[512][14];
    int n_ops;
    int resets_seen;
    int level_queries;     /* GetRxFifoLevel */
    uint32_t dio_irqs;     /* SetDioIrqConfig's mask: the IRQ pin */
    int64_t done_at;       /* the last TX_DONE */
    int64_t seam_max;      /* the longest from a TX_DONE to the next SetTx */
    bool tail_settx;       /* SetTx taken in a run's last 50 us, the next run started at its end (R3.47) */
    bool settx_queued;
    int settx_refused, settx_taken;
    /* TX */
    bool tx;
    int tx_level, tx_run, tx_sent, tx_sb;
    double tx_bytes_per_us, tx_carry;
    uint32_t irq;
    int tx_rate;
} M;

static void advance(void)
{
    int64_t now = F.now_us;
    double dt = (double)(now - M.last_us);
    M.last_us = now;
    if (M.rx) {
        M.carry += dt * M.bytes_per_us;
        int add = (int)M.carry;
        M.carry -= add;
        M.made += (uint64_t)add;
        M.level += add;
        if (M.level > LR2021_IQ_FIFO_BYTES) {
            M.level = LR2021_IQ_FIFO_BYTES; /* what didn't fit is gone */
            M.rx_flags |= LR2021_FIFO_OVERFLOW;
        }
    }
    if (M.tx) {
        M.tx_carry += dt * M.tx_bytes_per_us;
        int take = (int)M.tx_carry;
        M.tx_carry -= take;
        if (take > M.tx_level)
            take = M.tx_level;
        if (take > M.tx_run - M.tx_sent)
            take = M.tx_run - M.tx_sent; /* the rest is the next run's */
        M.tx_level -= take;
        M.tx_sent += take;
        if (M.tx_sent >= M.tx_run) {
            M.tx = false;
            M.irq |= TX_DONE;
            M.done_at = now - (int64_t)(M.tx_carry / M.tx_bytes_per_us); /* when the last byte went */
            if (M.settx_queued) { /* the next run at once */
                M.settx_queued = false;
                M.tx = true;
                M.tx_sent = 0;
                M.done_at = 0;
                M.seam_max = M.seam_max > 0 ? M.seam_max : 1;
            }
        }
    }
    F.irq_level = (M.irq_high_at && M.rx && M.level >= M.irq_high_at) || (M.irq & M.dio_irqs & ~0x3Fu);
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static void respond(int idx, const uint8_t *tx, size_t len, uint8_t *rx)
{
    (void)idx;
    const uint16_t op = (uint16_t)(tx[0] << 8 | tx[1]);
    /* 16 MHz SPI: 0.5 us a byte. A FIFO read takes its bytes as of the frame's start and the samples that come in
     * during it after: it drains faster than they come (2 MB/s against at most 1.33), so the level only falls. */
    if (op != LR2021_READ_RX_FIFO || M.pending) {
        F.now_us += (int64_t)(len / 2);
        advance();
    }
    rx[0] = ST_OK;
    rx[1] = (uint8_t)(M.tx ? LR2021_MODE_TX : M.rx ? LR2021_MODE_RX : LR2021_MODE_FS); /* the chip's mode */
    if (op == 0 && M.pending) { /* a read's answer frame */
        rx[0] = ST_DATA;
        uint8_t *a = rx + 2;
        switch (M.pending) {
        case LR2021_GET_VERSION: a[0] = M.ver[0]; a[1] = M.ver[1]; break;
        case LR2021_GET_RX_FIFO_LEVEL: M.level_queries++; a[0] = (uint8_t)(M.level >> 8); a[1] = (uint8_t)M.level; break;
        case LR2021_GET_TX_FIFO_LEVEL: a[0] = (uint8_t)(M.tx_level >> 8); a[1] = (uint8_t)M.tx_level; break;
        case LR2021_GET_FIFO_IRQ_FLAGS: a[0] = M.rx_flags; a[1] = 0; break;
        case LR2021_GET_AND_CLEAR_FIFO_IRQ_FLAGS: a[0] = M.rx_flags; M.rx_flags = 0; break;
        case LR2021_READ_REG_MEM32: {
            uint32_t v = M.pending_addr == LR2021_PATCH_MARKER_ADDR  ? M.marker
                         : M.pending_addr == LR2021_REG_RX_FIFO_BASE ? (LR2021_RX_FIFO_BASE_1K & 0xFFFF)
                         : M.pending_addr == LR2021_REG_RX_FIFO_SIZE ? LR2021_RX_FIFO_SIZE_1K
                                                                     : 0;
            a[0] = (uint8_t)(v >> 24); a[1] = (uint8_t)(v >> 16); a[2] = (uint8_t)(v >> 8); a[3] = (uint8_t)v;
            break;
        }
        default: break;
        }
        M.pending = 0;
        return;
    }
    if (op == LR2021_READ_RX_FIFO) {
        size_t n = len - 2;
        size_t real = n < (size_t)M.level ? n : (size_t)M.level;
        uint64_t first = M.made - (uint64_t)M.level; /* the oldest byte still in the FIFO */
        for (size_t i = 0; i < real; i++)
            rx[2 + i] = (uint8_t)(first + i);
        M.level -= (int)real;
        if (real < n)
            M.rx_flags |= LR2021_FIFO_UNDERFLOW;
        F.now_us += (int64_t)(len / 2);
        advance();
        return;
    }
    if (op == LR2021_GET_STATUS) {
        rx[2] = (uint8_t)(M.irq >> 24); rx[3] = (uint8_t)(M.irq >> 16); rx[4] = (uint8_t)(M.irq >> 8);
        rx[5] = (uint8_t)M.irq;
        return;
    }
    if (op == LR2021_WRITE_TX_FIFO) {
        M.tx_level += (int)(len - 2);
        return;
    }
    if (M.n_ops < 512) {
        M.ops[M.n_ops] = op;
        memcpy(M.args[M.n_ops], tx + 2, len - 2 < 14 ? len - 2 : 14);
        M.n_ops++;
    }
    switch (op) {
    case LR2021_GET_VERSION: case LR2021_GET_RX_FIFO_LEVEL: case LR2021_GET_TX_FIFO_LEVEL: case LR2021_GET_ERRORS:
    case LR2021_GET_FIFO_IRQ_FLAGS: case LR2021_GET_AND_CLEAR_FIFO_IRQ_FLAGS:
        M.pending = op;
        break;
    case LR2021_READ_REG_MEM32:
        M.pending = op;
        M.pending_addr = (uint32_t)tx[2] << 16 | (uint32_t)tx[3] << 8 | tx[4];
        break;
    case LR2021_SET_RX: M.rx = true; break;
    case LR2021_SET_STANDBY: M.rx = false; M.tx = false; M.done_at = 0; break;
    case LR2021_CLEAR_RX_FIFO: M.level = 0; break;
    case LR2021_CLEAR_TX_FIFO: M.tx_level = 0; break;
    case LR2021_CLEAR_FIFO_IRQ_FLAGS: M.rx_flags &= (uint8_t)~tx[2]; break;
    case LR2021_CLEAR_IRQ: M.irq &= ~be32(tx + 2); break;
    case LR2021_CONFIG_FIFO_IRQ: M.irq_high_at = tx[4] << 8 | tx[5]; break;
    case LR2021_RAW_TX: /* as the chip: the run is a 16-bit byte count, two bytes a sample for I/Q (R3.43) */
        M.tx_sb = tx[8] == LR2021_RAW_TX_IQ ? 2 : 1;
        M.tx_run = ((tx[2] << 8 | tx[3]) * M.tx_sb) & 0xFFFF;
        M.tx_rate = (int)be32(tx + 4);
        break;
    case LR2021_SET_DIO_IRQ_CONFIG: M.dio_irqs = be32(tx + 3); break;
    case LR2021_SET_TX:
        if (M.tx) {
            /* in a run's last 50 us: taken for when it ends; before, CMD_FAIL and the run goes on (R3.45) */
            if (M.tail_settx && (M.tx_run - M.tx_sent) / M.tx_bytes_per_us <= 50 &&
                M.tx_level >= M.tx_run - M.tx_sent) {
                M.settx_queued = true;
                M.settx_taken++;
            } else {
                M.settx_refused++;
            }
            break;
        }
        if (M.done_at && F.now_us - M.done_at > M.seam_max)
            M.seam_max = F.now_us - M.done_at;
        M.done_at = 0;
        M.tx = true;
        M.tx_sent = 0;
        M.tx_bytes_per_us = M.tx_rate * M.tx_sb / 1e6;
        break;
    default: break;
    }
}

static int count_op(uint16_t op)
{
    int n = 0;
    for (int i = 0; i < M.n_ops; i++)
        n += M.ops[i] == op;
    return n;
}

static int find_op(uint16_t op, int from)
{
    for (int i = from; i < M.n_ops; i++)
        if (M.ops[i] == op)
            return i;
    return -1;
}

/* What a sink saw: the stream must count up from its first byte, except across a gap. */
static struct {
    uint64_t samples;
    int calls, gaps, breaks;
    bool have;
    uint8_t next;
    int sb;
} S;

static void sink(void *ctx, const uint8_t *d, size_t n, unsigned flags, int64_t t)
{
    (void)ctx;
    (void)t;
    S.calls++;
    if (flags & LR2021_IQ_GAP) {
        S.gaps++;
        S.have = false;
    }
    for (size_t i = 0; i < n * (size_t)S.sb; i++) {
        if (S.have && d[i] != S.next)
            S.breaks++;
        S.next = (uint8_t)(d[i] + 1);
        S.have = true;
    }
    S.samples += n;
}

static void model_time(void) { advance(); }

static lr2021_t *chip(bool irq)
{
    shim_reset();
    memset(&M, 0, sizeof M);
    memset(&S, 0, sizeof S);
    M.ver[0] = 0x01;
    M.ver[1] = 0x18;
    F.respond = respond;
    F.on_time = model_time;
    F.has_irq = irq;
    lr2021_config_t c = mock_config();
    lr2021_t *d = NULL;
    lr2021_err_t e = lr2021_open(&c, &d);
    CHECK(e == LR2021_OK && d, "open: %d", e);
    return d;
}

static void plans(void)
{
    lr2021_iq_plan_t p;
    CHECK(lr2021_iq_plan(125000, LR2021_IQ_LORA, &p) == LR2021_OK && p.rate == 125000 && p.format == LR2021_IQ_CS16 &&
              p.lora_bw == LR2021_LORA_BW_31 && p.sample_bytes == 4,
          "LORA 125 k");
    CHECK(lr2021_iq_plan(240000, LR2021_IQ_LORA, &p) == LR2021_OK && p.rate == 250000, "LORA: the nearer, 250 k");
    CHECK(lr2021_iq_plan(240000, LR2021_IQ_AUTO, &p) == LR2021_OK && p.mode == LR2021_IQ_FSK16 && p.rate >= 240000 &&
              p.rate <= LR2021_IQ_FSK16_MAX && p.format == LR2021_IQ_CS16,
          "AUTO 240 k: FSK16 at %.1f", p.rate);
    CHECK(fabs(p.rate - 250000) < 1000, "R2.19: 0x1B is 250 k (%.1f)", p.rate);
    CHECK(lr2021_iq_plan(384000, LR2021_IQ_AUTO, &p) == LR2021_OK && p.mode == LR2021_IQ_CONV8 && p.rate == 384000 &&
              p.rate_den == 1 && p.format == LR2021_IQ_CS8 && p.sample_bytes == 2 && p.chip_rate >= 384000 &&
              p.chip_rate < 4 * 384000 && p.passband_hz <= 0.8 * 384000,
          "AUTO 384 k: CONV8, exact, from %.0f", p.chip_rate);
    CHECK(lr2021_iq_plan(576000, LR2021_IQ_CONV8, &p) == LR2021_OK && p.chip_rate > 576000, "576 k (R2.21)");
    CHECK(lr2021_iq_plan(600000, LR2021_IQ_CONV8, &p) == LR2021_ERR_NOT_SUPPORTED, "past 576 k");
    CHECK(lr2021_iq_plan(300000, LR2021_IQ_FSK16, &p) == LR2021_ERR_NOT_SUPPORTED, "FSK16 past 260 k");
    CHECK(lr2021_iq_plan(100000.5, LR2021_IQ_CONV8, &p) == LR2021_ERR_NOT_SUPPORTED, "CONV8: whole Hz");
    CHECK(lr2021_iq_plan(0, LR2021_IQ_AUTO, &p) == LR2021_ERR_INVALID_ARG, "no rate");

    double r[64], lo, hi;
    int n = lr2021_iq_rates(LR2021_IQ_FSK16, r, 64, &lo, &hi);
    bool sorted = n > 3;
    for (int i = 1; i < n; i++)
        sorted &= r[i] > r[i - 1];
    CHECK(sorted && lo == r[0] && hi == r[n - 1] && hi <= LR2021_IQ_FSK16_MAX, "FSK16: %d rates, %.0f to %.0f", n, lo,
          hi);
    for (int i = 0; i < n; i++)
        CHECK(lr2021_iq_plan(r[i], LR2021_IQ_FSK16, &p) == LR2021_OK && p.rate == r[i], "listed %.1f plans as itself",
              r[i]);
    CHECK(lr2021_iq_rates(LR2021_IQ_LORA, r, 64, &lo, &hi) == 2 && r[0] == 125000 && hi == 250000, "LORA: two");
    CHECK(lr2021_iq_rates(LR2021_IQ_CONV8, r, 64, &lo, &hi) == 0 && lo > 0 && lo < 125000 && hi == 576000,
          "CONV8: a range, %.0f to %.0f", lo, hi);
    CHECK(lr2021_iq_plan(lo, LR2021_IQ_CONV8, &p) == LR2021_OK, "its low end plans");
}

static void init_checks(void)
{
    lr2021_iq_t iq;
    lr2021_t *d = chip(false);
    CHECK(lr2021_iq_init(&iq, d, NULL, false) == LR2021_OK && iq.rom && iq.board == &lr2021_board_generic,
          "1.24: taken (%s)", iq.error);
    CHECK(find_op(LR2021_CALIBRATE, 0) > find_op(LR2021_SET_STANDBY, 0) && find_op(LR2021_SET_TCXO_MODE, 0) < 0,
          "standby, calibrate; no TCXO on a generic board");
    lr2021_close(d);

    d = chip(false);
    M.ver[1] = 0x19;
    CHECK(lr2021_iq_init(&iq, d, NULL, false) == LR2021_ERR_NOT_SUPPORTED && strstr(iq.error, "1.25"),
          "1.25: refused (%s)", iq.error);
    CHECK(lr2021_iq_init(&iq, d, NULL, true) == LR2021_OK && !iq.rom, "allow_untested: taken");
    lr2021_close(d);

    d = chip(false);
    M.marker = LR2021_PATCH_ACTIVE;
    CHECK(lr2021_iq_init(&iq, d, NULL, false) == LR2021_ERR_INVALID_STATE, "a patch running: refused (R1.2)");
    lr2021_close(d);

    lr2021_board_buf_t b;
    char err[64];
    lr2021_board_parse("generic tcxo=2", &b, err, sizeof err);
    d = chip(false);
    CHECK(lr2021_iq_init(&iq, d, &b.b, false) == LR2021_OK && find_op(LR2021_SET_TCXO_MODE, 0) >= 0 &&
              M.args[find_op(LR2021_SET_TCXO_MODE, 0)][0] == 2,
          "a TCXO board: SetTcxoMode 2");
    lr2021_close(d);
}

/* Poll for ms of model time. */
static void run(lr2021_iq_t *iq, int ms)
{
    int64_t end = F.now_us + (int64_t)ms * 1000;
    while (F.now_us < end) {
        int r = lr2021_iq_poll(iq, sink, NULL, 5);
        CHECK(r >= 0, "poll: %d (%s)", r, iq->error);
        if (r < 0)
            break;
    }
}

static void session_frames(void)
{
    lr2021_iq_t iq;
    lr2021_t *d = chip(false);
    lr2021_iq_init(&iq, d, NULL, false);
    M.n_ops = 0;
    lr2021_iq_cfg_t cfg = {.rate = 384000, .mode = LR2021_IQ_AUTO, .center_hz = 915e6, .gain = 9};
    CHECK(lr2021_iq_start(&iq, &cfg) == LR2021_OK && iq.in_session, "start (%s)", iq.error);
    int route = find_op(LR2021_RAW_ROUTE, 0), rx = find_op(LR2021_SET_RX, 0), gain = find_op(LR2021_SET_AGC_GAIN_MANUAL, 0);
    CHECK(find_op(LR2021_SET_PACKET_TYPE, 0) >= 0 && M.args[find_op(LR2021_SET_PACKET_TYPE, 0)][0] == LR2021_PACKET_FSK,
          "CONV8: the FSK modem");
    CHECK(find_op(LR2021_CALIB_FE, 0) >= 0 && find_op(LR2021_CALIB_FE, 0) < route, "CalibFE before the route (R2.5)");
    CHECK(route >= 0 && M.args[route][0] == iq.plan.rx_bw && be32(M.args[route] + 1) == 384000 &&
              (M.args[route][5] & 7) == LR2021_RAW_SRC_CONV,
          "the route through the converter at 384 000 (R2.21)");
    CHECK(rx > route && gain > rx && M.args[gain][0] == 9, "the gain after SetRx (R2.25)");
    CHECK(find_op(LR2021_CLEAR_RX_FIFO, rx) > gain, "the FIFO cleared last (2.2 step 8)");
    CHECK(find_op(LR2021_CONFIG_FIFO_IRQ, 0) < 0, "no IRQ DIO: no FIFO IRQ");

    M.n_ops = 0;
    CHECK(lr2021_iq_set_gain(&iq, 0) == LR2021_OK && count_op(LR2021_SET_AGC_GAIN_MANUAL) == 1 &&
              M.args[0][0] == 0,
          "AGC in a session: sent at once");
    CHECK(lr2021_iq_set_gain(&iq, 14) == LR2021_ERR_INVALID_ARG, "gain 14 refused");

    M.n_ops = 0;
    lr2021_iq_stop(&iq);
    CHECK(!iq.in_session && find_op(LR2021_SET_STANDBY, 0) == 0 && find_op(LR2021_CALIBRATE, 0) > 0 &&
              F.nreset_levels[F.n_nreset - 2] == 0,
          "stop: standby, reset, set up again (R2.16)");
    lr2021_close(d);

    d = chip(false);
    lr2021_iq_init(&iq, d, NULL, false);
    M.n_ops = 0;
    cfg.rate = 125000;
    cfg.mode = LR2021_IQ_LORA;
    CHECK(lr2021_iq_start(&iq, &cfg) == LR2021_OK, "LORA start");
    route = find_op(LR2021_RAW_ROUTE, 0);
    CHECK(M.args[find_op(LR2021_SET_PACKET_TYPE, 0)][0] == LR2021_PACKET_LORA && route >= 0 && M.args[route][0] == 0xFF &&
              M.args[route][5] == 0x0A,
          "LORA: the route SDR found (FF ... 0A, 2.2)");
    CHECK(find_op(LR2021_SET_LORA_MODULATION, 0) >= 0 && (M.args[find_op(LR2021_SET_LORA_MODULATION, 0)][0] & 0xF) ==
                                                             LR2021_LORA_BW_31,
          "31.25 kHz");
    lr2021_close(d);

    d = chip(false);
    lr2021_iq_init(&iq, d, NULL, false);
    cfg.rate = 700000;
    cfg.mode = LR2021_IQ_AUTO;
    CHECK(lr2021_iq_start(&iq, &cfg) == LR2021_ERR_NOT_SUPPORTED && !iq.in_session, "700 k: refused");
    cfg.rate = 125000;
    cfg.gain = 20;
    CHECK(lr2021_iq_start(&iq, &cfg) == LR2021_ERR_INVALID_ARG, "gain 20: refused");
    CHECK(lr2021_iq_poll(&iq, sink, NULL, 1) == LR2021_ERR_INVALID_STATE, "poll before a start");
    lr2021_close(d);
}

static void stream(lr2021_iq_mode_t mode, double rate, bool irq, int slack_us, const char *what)
{
    lr2021_iq_t iq;
    lr2021_t *d = chip(irq);
    lr2021_iq_init(&iq, d, irq ? &lr2021_board_w12 : NULL, false);
    lr2021_iq_cfg_t cfg = {.rate = rate, .mode = mode, .center_hz = 915e6, .gain = 13, .slack_us = slack_us};
    CHECK(lr2021_iq_start(&iq, &cfg) == LR2021_OK, "%s: start (%s)", what, iq.error);
    S.sb = iq.plan.sample_bytes;
    M.bytes_per_us = iq.plan.rate * iq.plan.sample_bytes / 1e6;
    if (irq)
        CHECK(find_op(LR2021_CONFIG_FIFO_IRQ, 0) >= 0 && M.irq_high_at > 0, "%s: the FIFO IRQ at %d", what,
              M.irq_high_at);
    int64_t t0 = F.now_us;
    run(&iq, 200);
    double want = iq.plan.rate * (double)(F.now_us - t0) / 1e6;
    CHECK(S.breaks == 0 && S.gaps == 0 && iq.stats.losses == 0, "%s: no loss, nothing out of order (%d breaks, %d gaps)",
          what, S.breaks, S.gaps);
    CHECK(S.samples > want * 0.97 && S.samples <= want + 1024, "%s: %llu of about %.0f samples", what,
          (unsigned long long)S.samples, want);
    CHECK(iq.stats.samples == S.samples, "%s: counted", what);
    if (irq && mode == LR2021_IQ_LORA)
        CHECK(F.irq_waits > 10, "%s: waited on the IRQ (%d)", what, F.irq_waits);
    if (mode != LR2021_IQ_LORA) {
        /* by the level: reads of about 640 bytes (with a slack, what leaves the FIFO that long before it fills), the
         * level asked before one read in four or fewer */
        const double per_read = (double)iq.stats.samples * iq.plan.sample_bytes / iq.stats.reads;
        const double target = slack_us ? 1024 - 64 - M.bytes_per_us * slack_us : 640;
        const int asked = M.level_queries;
        CHECK(per_read > target - 80 && per_read < target + 80 && asked * 3 < (int)iq.stats.reads,
              "%s: %.0f bytes a read (about %.0f), %d level queries for %u reads", what, per_read, target, asked,
              iq.stats.reads);
    }

    /* a stall: the FIFO overflows */
    S.calls = 0;
    M.n_ops = 0;
    F.now_us += 20000;
    advance();
    int restarts = (int)iq.stats.restarts;
    run(&iq, 50);
    CHECK(iq.stats.losses == 1 && S.gaps == 1 && S.breaks == 0, "%s: a stall is one loss, one gap (%u, %d, %d)", what,
          iq.stats.losses, S.gaps, S.breaks);
    if (mode == LR2021_IQ_LORA)
        CHECK((int)iq.stats.restarts == restarts + 1, "%s: a new session (R2.15)", what);
    else
        CHECK((int)iq.stats.restarts == restarts && count_op(LR2021_CLEAR_RX_FIFO) == 1, "%s: the FIFO cleared (R2.28)",
              what);
    CHECK(S.calls > 10, "%s: and it goes on", what);
    lr2021_iq_stop(&iq);
    lr2021_close(d);
}

static size_t pull_n;
static size_t pull(void *ctx, uint8_t *buf, size_t max)
{
    (void)ctx;
    size_t n = pull_n < max ? pull_n : max;
    memset(buf, 7, n);
    pull_n -= n;
    return n;
}

static void transmit(void)
{
    lr2021_iq_t iq;
    lr2021_t *d = chip(false);
    lr2021_iq_init(&iq, d, NULL, false);
    lr2021_iq_cfg_t cfg = {.rate = 250000, .mode = LR2021_IQ_FSK16, .center_hz = 915e6, .gain = 13};
    lr2021_iq_start(&iq, &cfg);
    S.sb = iq.plan.sample_bytes;
    M.bytes_per_us = iq.plan.rate * iq.plan.sample_bytes / 1e6;
    run(&iq, 20);
    M.n_ops = 0;
    pull_n = 4800;
    lr2021_iq_tx_t tx = {.mode = LR2021_TX_FREQ, .rate = 48000, .fdev_hz = 2048, .power_dbm = 0, .samples = 4800};
    lr2021_tx_stream_stats_t st;
    lr2021_err_t e = lr2021_iq_transmit(&iq, &tx, pull, NULL, &st);
    CHECK(e == LR2021_OK && st.runs == 1 && st.sent == 4800, "a 100 ms FREQ burst: %d (%s), %d runs, %zu bytes", e,
          iq.error, st.runs, st.sent);
    int raw = find_op(LR2021_RAW_TX, 0);
    CHECK(raw >= 0 && (M.args[raw][0] << 8 | M.args[raw][1]) == 4800 && be32(M.args[raw] + 2) == 48000 &&
              M.args[raw][6] == LR2021_RAW_TX_FREQ,
          "0x0293: 4800 at 48 000, frequency samples (R3.1)");
    CHECK(find_op(LR2021_CALIB_FE, 0) < 0, "no CalibFE where RX calibrated (R3.16)");
    CHECK(find_op(LR2021_SET_RX, raw) > raw && iq.in_session, "back to RX after");
    S.calls = 0;
    run(&iq, 20);
    CHECK(S.calls > 0 && S.gaps == 1, "the stream goes on, a gap told");

    /* longer than a run: equal runs, the last filled out */
    M.n_ops = 0;
    pull_n = 100000;
    tx.samples = 100000;
    e = lr2021_iq_transmit(&iq, &tx, pull, NULL, &st);
    raw = find_op(LR2021_RAW_TX, 0);
    CHECK(e == LR2021_OK && st.runs == 2 && (M.args[raw][0] << 8 | M.args[raw][1]) == 50000,
          "100 000: two runs of 50 000 (%d, %d runs)", e, st.runs);

    /* I/Q: a run's bytes are a 16-bit count too (R3.43), so 100 000 samples are four runs of 25 000 */
    M.n_ops = 0;
    pull_n = 200000;
    tx.mode = LR2021_TX_IQ_POLAR;
    e = lr2021_iq_transmit(&iq, &tx, pull, NULL, &st);
    raw = find_op(LR2021_RAW_TX, 0);
    CHECK(e == LR2021_OK && st.runs == 4 && (M.args[raw][0] << 8 | M.args[raw][1]) == 25000,
          "100 000 I/Q: four runs of 25 000 (%d, %d runs)", e, st.runs);
    CHECK(M.seam_max > 0 && M.seam_max < 400, "no IRQ pin: a TX_DONE seen and the next run started within %lld us",
          (long long)M.seam_max);
    CHECK(lr2021_tx_max_run(LR2021_TX_IQ_PHASE) == 32767 && lr2021_tx_max_run(LR2021_TX_FREQ) == 65535,
          "the run caps");
    tx.mode = LR2021_TX_FREQ;

    /* another frequency: calibrated there */
    M.n_ops = 0;
    pull_n = 480;
    tx.samples = 480;
    tx.center_hz = 433.92e6;
    e = lr2021_iq_transmit(&iq, &tx, pull, NULL, &st);
    CHECK(e == LR2021_OK && find_op(LR2021_CALIB_FE, 0) >= 0, "another band: CalibFE first");
    lr2021_close(d);
}

/* A board with a front end on RF switch DIOs: held in TX between runs, its own table back after (R3.44) */
/* With the IRQ pin: TX_DONE on the DIO, the next run started on its edge (R3.45). */
static void transmit_pin(void)
{
    lr2021_iq_t iq;
    lr2021_t *d = chip(true);
    lr2021_iq_init(&iq, d, &lr2021_board_w12, false);
    M.n_ops = 0;
    pull_n = 200000;
    lr2021_iq_tx_t tx = {.mode = LR2021_TX_IQ_POLAR, .rate = 96000, .center_hz = 915e6, .power_dbm = 0,
                         .samples = 100000};
    lr2021_tx_stream_stats_t st;
    lr2021_err_t e = lr2021_iq_transmit(&iq, &tx, pull, NULL, &st);
    const int dio = find_op(LR2021_SET_DIO_IRQ_CONFIG, 0);
    CHECK(dio >= 0 && M.args[dio][0] == lr2021_board_w12.irq_dio &&
              be32(M.args[dio] + 1) == (LR2021_IRQ_TX_DONE | LR2021_IRQ_TIMEOUT),
          "the IRQ DIO on TX_DONE and the timeout");
    CHECK(e == LR2021_OK && st.runs == 4 && st.sent == 200000 && !st.early && !st.timed_out,
          "four runs by the pin (%d, %s, %d runs, %zu bytes)", e, iq.error, st.runs, st.sent);
    CHECK(M.seam_max > 0 && M.seam_max < 40, "the next run's SetTx within %lld us of TX_DONE", (long long)M.seam_max);
    CHECK(st.refills > 100 && st.tx_flags == 0, "fed throughout: %u refills, flags 0x%02x", st.refills, st.tx_flags);

    /* a chip that takes SetTx in a run's last 50 us: from the third run on, each sent ahead of TX_DONE (R3.47) */
    M.tail_settx = true;
    M.settx_refused = M.settx_taken = 0;
    M.seam_max = 0;
    pull_n = 400000;
    tx.samples = 200000;
    e = lr2021_iq_transmit(&iq, &tx, pull, NULL, &st);
    CHECK(e == LR2021_OK && st.runs == 7 && st.sent >= 400000 && !st.early,
          "seven runs (%d, %s, %d runs, %zu bytes)", e, iq.error, st.runs, st.sent);
    CHECK(st.queued >= 3 && st.queued == (unsigned)M.settx_taken && st.refused == (unsigned)M.settx_refused &&
              st.refused <= 1,
          "SetTx ahead of TX_DONE: %u taken, %u refused", st.queued, st.refused);
    /* again: the delays learned, every restart queued */
    pull_n = 400000;
    e = lr2021_iq_transmit(&iq, &tx, pull, NULL, &st);
    CHECK(e == LR2021_OK && st.runs == 7 && st.queued == 6 && st.refused == 0,
          "the next burst: all six restarts queued (%u, %u refused)", st.queued, st.refused);
    M.tail_settx = false;
    tx.samples = 100000;
    lr2021_close(d);
}

/* A source fed as it goes: bytes arrive at the rate in 256-byte blocks, 768 ahead (under a FIFO), else
 * LR2021_TX_PULL_WAIT */
static struct {
    int64_t t0;
    size_t total, given, waits, stop_after;
    double bpus;
    volatile bool stop;
} SRC;
static size_t pull_streamed(void *ctx, uint8_t *buf, size_t max)
{
    (void)ctx;
    if (SRC.given >= SRC.total)
        return 0;
    if (SRC.stop_after && SRC.given >= SRC.stop_after)
        SRC.stop = true;
    double come = 768 + (double)(F.now_us - SRC.t0) * SRC.bpus;
    size_t have = come >= (double)SRC.total ? SRC.total : (size_t)come / 256 * 256;
    if (have <= SRC.given) {
        SRC.waits++;
        return LR2021_TX_PULL_WAIT;
    }
    size_t n = have - SRC.given < max ? have - SRC.given : max;
    memset(buf, 7, n);
    SRC.given += n;
    return n;
}

static void transmit_streamed(void)
{
    lr2021_iq_t iq;
    lr2021_t *d = chip(true);
    lr2021_iq_init(&iq, d, &lr2021_board_w12, false);
    M.n_ops = 0;
    memset(&SRC, 0, sizeof SRC);
    SRC.t0 = F.now_us;
    SRC.total = 150000; /* two runs of 65 534 bytes and part of a third */
    SRC.bpus = 96000 * 2 / 1e6;
    lr2021_iq_tx_t tx = {.mode = LR2021_TX_IQ_POLAR, .rate = 96000, .center_hz = 915e6, .power_dbm = 0,
                         .streamed = true};
    lr2021_tx_stream_stats_t st;
    lr2021_err_t e = lr2021_iq_transmit(&iq, &tx, pull_streamed, NULL, &st);
    CHECK(e == LR2021_OK && st.sent == 150000 && st.runs == 2 && st.cut && !st.early && !st.timed_out,
          "streamed: 150 000 bytes, two runs and a cut third (%d, %s, %zu bytes, %d runs, cut %d)", e, iq.error, st.sent,
          st.runs, st.cut);
    CHECK(SRC.waits > 0 && st.starved == 0 && M.tx_level == 0 && !M.tx,
          "the pull waited %zu times, never starved (%u), the FIFO empty at the cut (%d left)", SRC.waits, st.starved,
          M.tx_level);

    /* stopped part way */
    M.n_ops = 0;
    memset(&SRC, 0, sizeof SRC);
    SRC.t0 = F.now_us;
    SRC.total = 1000000;
    SRC.stop_after = 100000;
    SRC.bpus = 96000 * 2 / 1e6;
    tx.stop = &SRC.stop;
    e = lr2021_iq_transmit(&iq, &tx, pull_streamed, NULL, &st);
    CHECK(e == LR2021_OK && st.stopped && st.sent < 110000 && find_op(LR2021_SET_STANDBY, 0) >= 0,
          "stopped: ends at once (%d, %zu bytes)", e, st.sent);
    lr2021_close(d);
}

static void transmit_front_end(void)
{
    lr2021_iq_t iq;
    lr2021_t *d = chip(false);
    lr2021_iq_init(&iq, d, &lr2021_board_w12, false);
    lr2021_iq_tx_t tx = {.mode = LR2021_TX_FREQ, .center_hz = 915e6, .rate = 48000, .fdev_hz = 2048, .samples = 4800};
    M.n_ops = 0;
    pull_n = 4800;
    lr2021_tx_stream_stats_t st;
    lr2021_err_t e = lr2021_iq_transmit(&iq, &tx, pull, NULL, &st);
    const int key = find_op(LR2021_SET_TX, 0);
    int held = 0, back = 0;
    for (int i = 0; i < M.n_ops; i++) {
        if (M.ops[i] != LR2021_SET_DIO_RF_SWITCH || M.args[i][0] != 11)
            continue;
        if (i < key && M.args[i][1] == (0x06 | LR2021_RFSW_STANDBY))
            held = i;
        if (i > key && M.args[i][1] == 0x06)
            back = i;
    }
    CHECK(e == LR2021_OK && key > 0 && held > 0 && back > key, "the front end held in TX, then its table (%d %d %d)",
          held, key, back);
    CHECK(st.done_level == -1 && st.restart_us == 0, "one run: no restarts");
    CHECK(find_op(LR2021_CALIB_FE, 0) >= 0, "not receiving: calibrated first");

    /* the same cell again: the calibration kept; another: calibrated there */
    CHECK(!lr2021_iq_tx_calibrates(&iq, &tx), "known calibrated");
    M.n_ops = 0;
    pull_n = 4800;
    e = lr2021_iq_transmit(&iq, &tx, pull, NULL, &st);
    CHECK(e == LR2021_OK && find_op(LR2021_CALIB_FE, 0) < 0, "again: no CalibFE");
    tx.center_hz = 868e6;
    CHECK(lr2021_iq_tx_calibrates(&iq, &tx), "another cell: to calibrate");
    M.n_ops = 0;
    pull_n = 4800;
    e = lr2021_iq_transmit(&iq, &tx, pull, NULL, &st);
    CHECK(e == LR2021_OK && find_op(LR2021_CALIB_FE, 0) >= 0, "another cell: CalibFE");
    tx.center_hz = 915e6;
    pull_n = 4800;
    lr2021_iq_transmit(&iq, &tx, pull, NULL, &st);

    /* timed: keyed up at the time asked (the second time: the SetTx frame's time learned from the first); past it,
     * nothing sent */
    for (int i = 0; i < 2; i++) {
        tx.at_us = F.now_us + 50000;
        pull_n = 4800;
        e = lr2021_iq_transmit(&iq, &tx, pull, NULL, &st);
    }
    CHECK(e == LR2021_OK && !st.late && llabs(st.key_us - tx.at_us) < 3 && st.runs == 1,
          "timed: keyed %lld us after the time asked", (long long)(st.key_us - tx.at_us));
    M.n_ops = 0;
    tx.at_us = F.now_us - 1000;
    pull_n = 4800;
    e = lr2021_iq_transmit(&iq, &tx, pull, NULL, &st);
    CHECK(e == LR2021_OK && st.late && st.runs == 0 && find_op(LR2021_SET_TX, 0) < 0, "late: nothing sent (%d)", e);

    /* stopped while it waits for its time: at once, nothing sent */
    static volatile bool stop_now;
    stop_now = true;
    tx.stop = &stop_now;
    tx.at_us = F.now_us + 60000000;
    const int64_t t_stop = F.now_us;
    M.n_ops = 0;
    pull_n = 4800;
    e = lr2021_iq_transmit(&iq, &tx, pull, NULL, &st);
    CHECK(e == LR2021_OK && st.stopped && find_op(LR2021_SET_TX, 0) < 0 && F.now_us - t_stop < 100000,
          "stopped in the wait: %d, %lld us", e, (long long)(F.now_us - t_stop));
    tx.stop = NULL;
    lr2021_close(d);
}

int main(void)
{
    plans();
    init_checks();
    session_frames();
    stream(LR2021_IQ_FSK16, 250000, false, 0, "FSK16 250 k, polled");
    stream(LR2021_IQ_CONV8, 576000, false, 0, "CONV8 576 k, polled");
    stream(LR2021_IQ_LORA, 125000, false, 0, "LORA 125 k, polled");
    stream(LR2021_IQ_LORA, 125000, true, 0, "LORA 125 k, IRQ");
    stream(LR2021_IQ_FSK16, 250000, true, 0, "FSK16 250 k, IRQ");
    stream(LR2021_IQ_CONV8, 384000, true, 0, "CONV8 384 k, IRQ");
    stream(LR2021_IQ_CONV8, 384000, true, 700, "CONV8 384 k, IRQ, 700 us slack");
    transmit();
    transmit_pin();
    transmit_streamed();
    transmit_front_end();
    printf("lr2021 iq: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
