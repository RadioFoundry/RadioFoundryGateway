// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
// Host tests for lr2021_tx_stream (src/lr2021_tx_stream.c) over the shimmed ESP-IDF calls and a small model of the
// chip's TX side: a 1020-byte FIFO that a run drains at a fixed rate in shim time, TX_DONE when the run's bytes are
// through, the underflow flag, and the early end of spec R3.17. Not the chip: it pins what the stream does.
#include <stdio.h>
#include <string.h>

#include "esp_shim.h"
#include "lr2021.h"
#include "lr2021_tx.h"

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

static struct {
    int level;           /* bytes in the FIFO */
    bool tx;             /* in TX */
    int run_bytes;       /* bytes a run takes */
    int run_left;        /* of this run */
    double bytes_per_us; /* the drain rate */
    double carry;
    int64_t last_us;
    uint32_t irq;
    uint8_t tx_flags;
    int aired, starts, standbys;
    bool early_next;     /* the next SetTx ends at once with nothing sent (R3.17) */
    uint16_t pending;    /* a read command waiting for its answer frame */
} M;

static void advance(void)
{
    double dt = (double)(F.now_us - M.last_us);
    M.last_us = F.now_us;
    if (!M.tx)
        return;
    M.carry += dt * M.bytes_per_us;
    int step = (int)M.carry;
    M.carry -= step;
    if (step > M.run_left)
        step = M.run_left;
    int from_fifo = step < M.level ? step : M.level;
    if (from_fifo < step)
        M.tx_flags |= LR2021_FIFO_UNDERFLOW;
    M.level -= from_fifo;
    M.aired += from_fifo;
    M.run_left -= step;
    if (M.run_left == 0) {
        M.tx = false;
        M.irq |= LR2021_IRQ_TX_DONE;
    }
}

static uint32_t be32(const uint8_t *b) { return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; }

static void respond(int idx, const uint8_t *tx, size_t len, uint8_t *rx)
{
    (void)idx;
    advance();
    rx[0] = LR2021_CMD_OK << 1;
    rx[1] = M.tx ? LR2021_MODE_TX : LR2021_MODE_STBY_RC;
    const uint16_t op = (uint16_t)(tx[0] << 8 | tx[1]);
    if (op == 0 && M.pending) { /* a read's answer frame */
        if (M.pending == LR2021_GET_TX_FIFO_LEVEL) {
            rx[2] = (uint8_t)(M.level >> 8);
            rx[3] = (uint8_t)M.level;
        } else if (M.pending == LR2021_GET_FIFO_IRQ_FLAGS) {
            rx[2] = 0;
            rx[3] = M.tx_flags;
        }
        M.pending = 0;
        return;
    }
    switch (op) {
    case LR2021_GET_STATUS:
        if (len >= 6) {
            rx[2] = (uint8_t)(M.irq >> 24), rx[3] = (uint8_t)(M.irq >> 16), rx[4] = (uint8_t)(M.irq >> 8),
            rx[5] = (uint8_t)M.irq;
        }
        break;
    case LR2021_WRITE_TX_FIFO:
        M.level += (int)len - 2;
        if (M.level > 1020) {
            M.level = 1020;
            M.tx_flags |= LR2021_FIFO_OVERFLOW;
        }
        break;
    case LR2021_CLEAR_TX_FIFO: M.level = 0; break;
    case LR2021_CLEAR_FIFO_IRQ_FLAGS: M.tx_flags &= (uint8_t)~tx[3]; break;
    case LR2021_CLEAR_IRQ: M.irq &= ~be32(tx + 2); break;
    case LR2021_SET_TX:
        M.starts++;
        if (M.early_next) {
            M.early_next = false;
            M.irq |= LR2021_IRQ_TX_DONE;
            M.tx_flags |= LR2021_FIFO_FULL | LR2021_FIFO_OVERFLOW;
        } else {
            M.tx = true;
            M.run_left = M.run_bytes;
        }
        break;
    case LR2021_SET_STANDBY:
        M.tx = false;
        M.standbys++;
        break;
    case LR2021_GET_TX_FIFO_LEVEL:
    case LR2021_GET_FIFO_IRQ_FLAGS: M.pending = op; break;
    default: break;
    }
}

static lr2021_t *open_dev(void)
{
    shim_reset();
    memset(&M, 0, sizeof M);
    F.respond = respond;
    lr2021_config_t c = {.spi_host = 1, .sck = 1, .mosi = 2, .miso = 3, .nss = 4, .busy = 5, .nreset = 6, .irq = -1};
    lr2021_t *d = NULL;
    lr2021_open(&c, &d);
    M.last_us = F.now_us;
    return d;
}

/* The pull: a counting pattern, `total` bytes, at most `most` a call. */
static struct {
    size_t total, given, most;
    int calls;
} P;

static size_t pull(void *ctx, uint8_t *buf, size_t max)
{
    (void)ctx;
    P.calls++;
    size_t n = P.total - P.given;
    if (n > max)
        n = max;
    if (P.most && n > P.most)
        n = P.most;
    for (size_t i = 0; i < n; i++)
        buf[i] = (uint8_t)(P.given + i);
    P.given += n;
    return n;
}

static lr2021_tx_stream_cfg_t cfg(const uint8_t *start, size_t start_len, uint32_t run_bytes)
{
    lr2021_tx_stream_cfg_t c = {0};
    c.start = start;
    c.start_len = start_len;
    c.run_bytes = run_bytes;
    c.bytes_per_s = 48000;
    c.max_ms = 5000;
    return c;
}

int main(void)
{
    uint8_t settx[8];
    size_t settx_len = lr2021_cmd_set_tx(settx, 64000);
    lr2021_tx_stream_stats_t st;

    // three runs back to back: every byte aired, three TX_DONEs, three key-ups, no underflow
    lr2021_t *d = open_dev();
    M.run_bytes = 4800;
    M.bytes_per_us = 0.048;
    memset(&P, 0, sizeof P);
    P.total = 3 * 4800;
    lr2021_tx_stream_cfg_t c = cfg(settx, settx_len, 4800);
    esp_err_t e = lr2021_tx_stream(d, &c, pull, NULL, &st);
    CHECK(e == ESP_OK, "chained: %d", e);
    CHECK(st.runs == 3 && M.starts == 3, "runs %d starts %d", st.runs, M.starts);
    CHECK(st.sent == 14400 && M.aired == 14400, "sent %zu aired %d", st.sent, M.aired);
    CHECK(!(st.tx_flags & LR2021_FIFO_UNDERFLOW) && !st.early && !st.timed_out, "flags %02X", st.tx_flags);
    CHECK(st.refills > 0 && st.end_us > st.key_us, "refills %u", st.refills);
    // about 300 ms of air: the stream slept rather than spun (the shim's clock moves only when it waits)
    CHECK(st.end_us - st.key_us >= 290000 && st.end_us - st.key_us < 320000, "took %lld us",
          (long long)(st.end_us - st.key_us));
    lr2021_close(d);

    // a short last run: the chip holds the last sample and flags the underflow; the stream reports it
    d = open_dev();
    M.run_bytes = 4800;
    M.bytes_per_us = 0.048;
    memset(&P, 0, sizeof P);
    P.total = 4000;
    e = lr2021_tx_stream(d, &c, pull, NULL, &st);
    CHECK(e == ESP_OK && st.runs == 1, "short: %d runs %d", e, st.runs);
    CHECK(st.tx_flags & LR2021_FIFO_UNDERFLOW, "no underflow flag: %02X", st.tx_flags);
    lr2021_close(d);

    // R3.17: the run ends at once with nothing taken: early, an error, the caller sends again
    d = open_dev();
    M.run_bytes = 4800;
    M.bytes_per_us = 0.048;
    M.early_next = true;
    memset(&P, 0, sizeof P);
    P.total = 4800;
    e = lr2021_tx_stream(d, &c, pull, NULL, &st);
    CHECK(e == ESP_ERR_INVALID_RESPONSE && st.early && st.runs == 1, "early: %d %d", e, st.early);
    memset(&P, 0, sizeof P);
    P.total = 4800;
    M.aired = 0;
    e = lr2021_tx_stream(d, &c, pull, NULL, &st);
    CHECK(e == ESP_OK && !st.early && M.aired == 4800, "again: %d aired %d", e, M.aired);
    lr2021_close(d);

    // a chip that never finishes: the stream gives up at max_ms and puts it in standby
    d = open_dev();
    M.run_bytes = 4800;
    M.bytes_per_us = 0;
    memset(&P, 0, sizeof P);
    P.total = 4800;
    c.max_ms = 50;
    e = lr2021_tx_stream(d, &c, pull, NULL, &st);
    CHECK(e == ESP_ERR_TIMEOUT && st.timed_out && M.standbys == 1 && !M.tx, "timeout: %d standbys %d", e, M.standbys);
    c.max_ms = 5000;
    lr2021_close(d);

    // a pull that gives little at a time still keeps up (it's asked again at once)
    d = open_dev();
    M.run_bytes = 4800;
    M.bytes_per_us = 0.048;
    memset(&P, 0, sizeof P);
    P.total = 9600;
    P.most = 100;
    e = lr2021_tx_stream(d, &c, pull, NULL, &st);
    CHECK(e == ESP_OK && st.runs == 2 && M.aired == 9600 && !(st.tx_flags & LR2021_FIFO_UNDERFLOW),
          "small pulls: %d runs %d aired %d flags %02X", e, st.runs, M.aired, st.tx_flags);
    lr2021_close(d);

    // a carrier: one write of the start frame, no FIFO, left on
    d = open_dev();
    uint8_t cw[4];
    size_t cw_len = lr2021_cmd_u8(cw, LR2021_SET_TX_TEST_MODE, LR2021_TEST_CW);
    lr2021_tx_stream_cfg_t cc = cfg(cw, cw_len, 0);
    int before = F.xfers;
    e = lr2021_tx_stream(d, &cc, NULL, NULL, &st);
    CHECK(e == ESP_OK && F.xfers - before == 2, "cw: %d xfers %d", e, F.xfers - before);
    lr2021_close(d);

    // bad arguments
    d = open_dev();
    CHECK(lr2021_tx_stream(d, NULL, pull, NULL, &st) == ESP_ERR_INVALID_ARG, "no cfg");
    c.start = NULL;
    CHECK(lr2021_tx_stream(d, &c, pull, NULL, &st) == ESP_ERR_INVALID_ARG, "no start");
    c.start = settx;
    CHECK(lr2021_tx_stream(d, &c, NULL, NULL, &st) == ESP_ERR_INVALID_ARG, "no pull");
    memset(&P, 0, sizeof P);
    CHECK(lr2021_tx_stream(d, &c, pull, NULL, &st) == ESP_ERR_INVALID_SIZE, "no data");
    lr2021_close(d);

    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
