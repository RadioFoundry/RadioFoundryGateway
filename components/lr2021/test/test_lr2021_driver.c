// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
// Host tests for the LR20xx driver (src/lr2021.c) against a scripted fake chip: the mock HAL and clock in
// mock_hal.c stand in for the board. They pin the frames the driver clocks out, how it reads the
// answers, the BUSY handshake and its timeout, reset, open / close (including their failure paths), and the error
// text and counters.
#include <stdio.h>
#include <string.h>

#include "mock_hal.h"
#include "lr2021.h"

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

// What the chip answers on each transaction: the whole rx frame, by transaction index.
static struct {
    uint8_t d[1030];
    size_t n;
} ans[16];
static int nans;

static void respond(int idx, const uint8_t *tx, size_t len, uint8_t *rx)
{
    (void)tx;
    if (idx < nans) {
        memcpy(rx, ans[idx].d, ans[idx].n < len ? ans[idx].n : len);
    }
}

static void script_reset(void)
{
    memset(ans, 0, sizeof ans);
    nans = 0;
}

// Transaction `idx` answers with `n` bytes.
static void answer(int idx, const uint8_t *bytes, size_t n)
{
    memcpy(ans[idx].d, bytes, n);
    ans[idx].n = n;
    if (idx >= nans) {
        nans = idx + 1;
    }
}

static lr2021_config_t config(void) { return mock_config(); }

static lr2021_t *open_ok(void)
{
    shim_reset();
    script_reset();
    F.respond = respond;
    lr2021_config_t c = config();
    lr2021_t *d = NULL;
    lr2021_err_t e = lr2021_open(&c, &d);
    CHECK(e == LR2021_OK && d, "open: %d", e);
    F.xfers = 0; // the tests count their own
    return d;
}

static void opening(void)
{
    shim_reset();
    lr2021_config_t c = config();
    lr2021_t *d = NULL;
    CHECK(lr2021_open(&c, &d) == LR2021_OK && d, "open");
    // NRESET: low, high (the reset), the low for at least 100 us
    CHECK(F.n_nreset == 2 && F.nreset_levels[0] == 0 && F.nreset_levels[1] == 1, "NRESET sequence (%d sets)", F.n_nreset);
    CHECK(F.n_nreset == 2 && F.nreset_times[1] - F.nreset_times[0] >= 1000, "NRESET low for %lld us",
          (long long)(F.nreset_times[1] - F.nreset_times[0]));
    CHECK(!strcmp(lr2021_last_error(d), "") && lr2021_failures(d) == 0, "no errors yet");
    lr2021_close(d);
    CHECK(F.closes == 1, "close: the HAL closed");
    CHECK(F.nreset_levels[F.n_nreset - 1] == 0, "and the chip held in reset");
    lr2021_close(NULL);
    CHECK(F.closes == 1, "closing NULL does nothing");
}

static void open_failures(void)
{
    lr2021_config_t c = config();
    lr2021_t *d = (lr2021_t *)0x1;
    // a HAL without its calls: refused, the HAL closed, *out unchanged
    shim_reset();
    c.hal.busy = NULL;
    CHECK(lr2021_open(&c, &d) == LR2021_ERR_INVALID_ARG && d == (lr2021_t *)0x1 && F.closes == 1 && F.n_nreset == 0,
          "no BUSY: refused");
    // BUSY never drops after reset: the HAL closed
    shim_reset();
    F.busy_stuck = true;
    c = config();
    CHECK(lr2021_open(&c, &d) == LR2021_ERR_TIMEOUT && F.closes == 1 && d == (lr2021_t *)0x1,
          "reset timeout: HAL closed (%d)", F.closes);
    CHECK(F.now_us >= 200000 && F.now_us < 230000, "after 200 ms of waiting (%lld us)", (long long)F.now_us);
}

static void busy_handshake(void)
{
    lr2021_t *d = open_ok();
    // BUSY high for 1 ms: polled in microseconds
    F.busy_until_us = F.now_us + 1000;
    int64_t t0 = F.now_us;
    lr2021_status_t st;
    CHECK(lr2021_get_status(d, &st) == LR2021_OK, "get_status after a short BUSY");
    CHECK(F.now_us - t0 >= 1000 && F.now_us - t0 < 1100 && F.busy_polls > 100, "waited 1 ms in 1 us, then 5 us steps (%lld us, %d polls)",
          (long long)(F.now_us - t0), F.busy_polls);
    // BUSY high for 30 ms: after 2 ms it sleeps a tick at a time
    F.busy_until_us = F.now_us + 30000;
    t0 = F.now_us;
    F.busy_polls = 0;
    CHECK(lr2021_get_status(d, &st) == LR2021_OK, "get_status after a long BUSY");
    CHECK(F.now_us - t0 >= 30000 && F.now_us - t0 < 45000 && F.busy_polls < 470, "then a tick at a time (%lld us, %d polls)",
          (long long)(F.now_us - t0), F.busy_polls);
    CHECK(lr2021_failures(d) == 0, "no failures");
    // wake: the frame goes out while BUSY is high, then it waits for BUSY to drop
    F.busy_until_us = F.now_us + 5000;
    t0 = F.now_us;
    int w0 = F.xfers;
    CHECK(lr2021_wake(d) == LR2021_OK && F.xfers == w0 + 1 && F.tx_len[w0] * 8 >= 16000000 / 10000 &&
              F.tx[w0][0] == 0 && F.tx[w0][F.tx_len[w0] - 1] == 0,
          "wake: 100 us of zeros at 16 MHz at once (%zu bytes)", F.tx_len[w0]);
    CHECK(F.now_us - t0 >= 5000, "then waits for BUSY (%lld us)", (long long)(F.now_us - t0));
    // BUSY stuck: LR2021_ERR_TIMEOUT after 200 ms, with the text and the count
    F.busy_stuck = true;
    t0 = F.now_us;
    int x0 = F.xfers;
    CHECK(lr2021_get_status(d, &st) == LR2021_ERR_TIMEOUT, "stuck BUSY times out");
    CHECK(F.now_us - t0 >= 200000 && F.now_us - t0 < 215000 && F.xfers == x0, "after 200 ms, nothing sent (%lld us)",
          (long long)(F.now_us - t0));
    CHECK(!strcmp(lr2021_last_error(d), "BUSY stayed high") && lr2021_failures(d) == 1, "error text and count");
    // every call waits first
    uint8_t out[2], f[2] = {1, 1};
    CHECK(lr2021_write(d, f, 2) == LR2021_ERR_TIMEOUT && lr2021_read(d, f, 2, out, 2) == LR2021_ERR_TIMEOUT &&
              lr2021_read_fifo(d, out, 2) == LR2021_ERR_TIMEOUT && lr2021_write_tx_fifo(d, out, 2) == LR2021_ERR_TIMEOUT,
          "write, read, FIFO read and write all time out");
    uint32_t irq = 7;
    CHECK(lr2021_irq(d, &irq) == LR2021_ERR_TIMEOUT && irq == 0, "irq: 0 on error");
    CHECK(lr2021_failures(d) == 6, "six failures counted (%u)", lr2021_failures(d));
    F.busy_stuck = false;
    CHECK(lr2021_get_status(d, &st) == LR2021_OK, "recovers once BUSY drops");
    CHECK(!strcmp(lr2021_last_error(d), "BUSY stayed high"), "the last error stays until another");
    lr2021_close(d);
}

static void status_and_write(void)
{
    lr2021_t *d = open_ok();
    // GetStatus: 01 00 and four zeros out; status and IRQ word back
    static const uint8_t st_ans[6] = {0x05, 0x24, 0x80, 0x00, 0x00, 0x03};
    answer(0, st_ans, 6);
    lr2021_status_t st = {0};
    CHECK(lr2021_get_status(d, &st) == LR2021_OK, "get_status");
    static const uint8_t want_gs[6] = {0x01, 0x00, 0, 0, 0, 0};
    CHECK(F.tx_len[0] == 6 && !memcmp(F.tx[0], want_gs, 6), "GetStatus frame");
    CHECK(st.cmd == LR2021_CMD_OK && st.irq && st.reset == 2 && st.mode == LR2021_MODE_RX, "decoded status");
    F.xfers = 0;
    uint32_t irq = 0;
    CHECK(lr2021_irq(d, &irq) == LR2021_OK && irq == 0x80000003u, "irq word %08x", irq);
    // a failed transfer leaves the status alone
    F.xfers = 0;
    F.fail_xfer_at = 1;
    F.xfer_err = LR2021_FAIL;
    st.mode = 9;
    CHECK(lr2021_get_status(d, &st) == LR2021_FAIL && st.mode == 9, "SPI failure: status untouched");
    CHECK(!strcmp(lr2021_last_error(d), "SPI transfer failed") && lr2021_failures(d) == 1, "SPI error text and count");
    F.fail_xfer_at = 0;
    F.xfers = 0;
    CHECK(lr2021_irq(d, &irq) == LR2021_OK, "irq works again");
    F.fail_xfer_at = 2;
    F.xfers = 0;
    irq = 5;
    CHECK(lr2021_irq(d, &irq) == LR2021_OK, "irq is one transfer");
    F.fail_xfer_at = 1;
    F.xfers = 0;
    CHECK(lr2021_irq(d, &irq) == LR2021_FAIL && irq == 0, "irq on SPI failure: 0");
    F.fail_xfer_at = 0;

    // write: the frame, then GetStatus; OK passes
    script_reset();
    F.xfers = 0;
    uint8_t frame[4] = {0x01, 0x28, 0x00, 0};
    static const uint8_t ok_st[6] = {0x04, 0x01, 0, 0, 0, 0}; // cmd OK, standby RC
    answer(1, ok_st, 6);
    CHECK(lr2021_write(d, frame, 3) == LR2021_OK, "write");
    CHECK(F.xfers == 2 && F.tx_len[0] == 3 && !memcmp(F.tx[0], frame, 3) && F.tx_len[1] == 6 && F.tx[1][0] == 1 && F.tx[1][1] == 0,
          "the frame, then GetStatus");
    // FAIL and PERR in the status that answers the GetStatus
    static const uint8_t fail_st[6] = {0x00, 0x01, 0, 0, 0, 0};
    static const uint8_t perr_st[6] = {0x02, 0x02, 0, 0, 0, 0};
    uint32_t f0 = lr2021_failures(d);
    answer(1, fail_st, 6);
    F.xfers = 0;
    CHECK(lr2021_write(d, frame, 3) == LR2021_FAIL && !strcmp(lr2021_last_error(d), "command failed (chip in standby RC)") &&
              lr2021_failures(d) == f0 + 1,
          "FAIL: %s", lr2021_last_error(d));
    answer(1, perr_st, 6);
    F.xfers = 0;
    CHECK(lr2021_write(d, frame, 3) == LR2021_FAIL && !strcmp(lr2021_last_error(d), "command not understood (chip in standby XOSC)") &&
              lr2021_failures(d) == f0 + 2,
          "PERR: %s", lr2021_last_error(d));
    // a DATA status after a write is not a failure
    static const uint8_t data_st[6] = {0x06, 0x04, 0, 0, 0, 0};
    answer(1, data_st, 6);
    F.xfers = 0;
    CHECK(lr2021_write(d, frame, 3) == LR2021_OK, "DATA after a write is fine");
    // the SPI failing at either transfer
    F.xfers = 0;
    F.fail_xfer_at = 1;
    F.xfer_err = LR2021_ERR_INVALID_STATE;
    CHECK(lr2021_write(d, frame, 3) == LR2021_ERR_INVALID_STATE && F.xfers == 1, "first transfer fails: stops");
    F.xfers = 0;
    F.fail_xfer_at = 2;
    CHECK(lr2021_write(d, frame, 3) == LR2021_ERR_INVALID_STATE && F.xfers == 2, "GetStatus transfer fails");
    F.fail_xfer_at = 0;
    // sizes
    uint32_t f1 = lr2021_failures(d);
    static uint8_t big[300];
    F.xfers = 0;
    CHECK(lr2021_write(d, big, 261) == LR2021_ERR_INVALID_SIZE && F.xfers == 0 && lr2021_failures(d) == f1, "261 bytes: too big, not a failure");
    answer(1, ok_st, 6);
    CHECK(lr2021_write(d, big, 260) == LR2021_OK && F.tx_len[0] == 260, "260 bytes: the largest frame");
    lr2021_close(d);
}

static void reading(void)
{
    lr2021_t *d = open_ok();
    // read: frame, then n + 2 zeros; the answer is bytes 2.. of the second frame
    uint8_t f[2] = {0x01, 0x01};
    static const uint8_t a1[4] = {0x06, 0x01, 0x01, 0x18}; // DATA
    answer(1, a1, 4);
    uint8_t out[8] = {0xEE, 0xEE, 0xEE, 0xEE};
    CHECK(lr2021_read(d, f, 2, out, 2) == LR2021_OK && out[0] == 0x01 && out[1] == 0x18 && out[2] == 0xEE, "read DATA");
    CHECK(F.xfers == 2 && F.tx_len[0] == 2 && !memcmp(F.tx[0], f, 2) && F.tx_len[1] == 4 && !F.tx[1][0] && !F.tx[1][3],
          "frame, then 4 zero bytes");
    static const uint8_t a2[4] = {0x04, 0x01, 0x07, 0x09}; // OK
    answer(1, a2, 4);
    F.xfers = 0;
    CHECK(lr2021_read(d, f, 2, out, 2) == LR2021_OK && out[0] == 7 && out[1] == 9, "read OK");
    // FAIL, PERR and a reserved status: an error and the output untouched
    static const uint8_t a_fail[4] = {0x00, 0x02, 0x55, 0x66};
    static const uint8_t a_perr[4] = {0x02, 0x02, 0x55, 0x66};
    static const uint8_t a_rsv[4] = {0x08, 0x02, 0x55, 0x66}; // cmd 4
    memset(out, 0xEE, sizeof out);
    answer(1, a_fail, 4);
    F.xfers = 0;
    CHECK(lr2021_read(d, f, 2, out, 2) == LR2021_FAIL && out[0] == 0xEE && strstr(lr2021_last_error(d), "failed (chip in standby XOSC)"),
          "read FAIL: %s", lr2021_last_error(d));
    answer(1, a_perr, 4);
    F.xfers = 0;
    CHECK(lr2021_read(d, f, 2, out, 2) == LR2021_FAIL && out[0] == 0xEE && strstr(lr2021_last_error(d), "not understood"), "read PERR");
    uint32_t n0 = lr2021_failures(d);
    answer(1, a_rsv, 4);
    F.xfers = 0;
    CHECK(lr2021_read(d, f, 2, out, 2) == LR2021_FAIL && out[0] == 0xEE && !strcmp(lr2021_last_error(d), "unexpected command status") &&
              lr2021_failures(d) == n0 + 1,
          "a reserved status is an error, not a silent success: %s", lr2021_last_error(d));
    // SPI errors at each step
    F.xfers = 0;
    F.fail_xfer_at = 1;
    F.xfer_err = LR2021_FAIL;
    CHECK(lr2021_read(d, f, 2, out, 2) == LR2021_FAIL && F.xfers == 1, "first transfer fails");
    F.xfers = 0;
    F.fail_xfer_at = 2;
    CHECK(lr2021_read(d, f, 2, out, 2) == LR2021_FAIL && F.xfers == 2, "second transfer fails");
    F.fail_xfer_at = 0;
    // sizes: frame up to 260, answer up to 258
    static uint8_t big[300], res[300];
    F.xfers = 0;
    CHECK(lr2021_read(d, big, 261, res, 2) == LR2021_ERR_INVALID_SIZE && lr2021_read(d, f, 2, res, 259) == LR2021_ERR_INVALID_SIZE && F.xfers == 0,
          "frame 261 and answer 259 are refused");
    script_reset();
    static uint8_t full[260] = {0x04, 0x01};
    for (int i = 2; i < 260; i++) {
        full[i] = (uint8_t)i;
    }
    answer(1, full, 260);
    F.xfers = 0;
    CHECK(lr2021_read(d, f, 2, res, 258) == LR2021_OK && !memcmp(res, full + 2, 258) && F.tx_len[1] == 260,
          "the longest answer, 258 bytes");
    lr2021_close(d);
}

static void fifo_and_accessors(void)
{
    lr2021_t *d = open_ok();
    uint8_t out[8];
    // RX FIFO read: 00 01 and n zeros; the data is in the same frame after the status bytes
    static const uint8_t fa[6] = {0x04, 0x01, 0xDE, 0xAD, 0xBE, 0xEF};
    answer(0, fa, 6);
    CHECK(lr2021_read_fifo(d, out, 4) == LR2021_OK && !memcmp(out, fa + 2, 4), "FIFO read data");
    CHECK(F.xfers == 1 && F.tx_len[0] == 6 && F.tx[0][0] == 0 && F.tx[0][1] == 1 && !F.tx[0][2] && !F.tx[0][5], "FIFO read frame");
    // the status is not looked at: a FAIL status still returns the bytes
    static const uint8_t fb[4] = {0x00, 0x01, 0x12, 0x34};
    answer(1, fb, 4);
    CHECK(lr2021_read_fifo(d, out, 2) == LR2021_OK && out[0] == 0x12 && out[1] == 0x34, "FIFO read ignores the status");
    F.fail_xfer_at = 3;
    F.xfer_err = LR2021_FAIL;
    memset(out, 0xEE, sizeof out);
    CHECK(lr2021_read_fifo(d, out, 2) == LR2021_FAIL && out[0] == 0xEE, "FIFO read SPI error: output untouched");
    F.fail_xfer_at = 0;
    static uint8_t whole[1100];
    CHECK(lr2021_read_fifo(d, whole, 1025) == LR2021_ERR_INVALID_SIZE && lr2021_read_fifo(d, out, 0) == LR2021_OK, "FIFO read sizes");
    // the whole 1 KB FIFO in one frame
    F.xfers = 0;
    CHECK(lr2021_read_fifo(d, whole, 1024) == LR2021_OK && F.tx_len[0] == 1026, "1 KB in one frame");
    // send: the frame alone, no GetStatus
    F.xfers = 0;
    static const uint8_t ci[6] = {0x01, 0x16, 0, 0, 0, 1};
    CHECK(lr2021_send(d, ci, 6) == LR2021_OK && F.xfers == 1 && F.tx_len[0] == 6 && !memcmp(F.tx[0], ci, 6), "send: one frame");
    CHECK(lr2021_send(d, whole, 261) == LR2021_ERR_INVALID_SIZE && F.xfers == 1, "send: 261 bytes refused");
    // TX FIFO write: 00 02 and the data
    F.xfers = 0;
    static const uint8_t data[3] = {1, 2, 3};
    CHECK(lr2021_write_tx_fifo(d, data, 3) == LR2021_OK && F.tx_len[0] == 5 && F.tx[0][0] == 0 && F.tx[0][1] == 2 &&
              !memcmp(F.tx[0] + 2, data, 3),
          "FIFO write frame");
    F.fail_xfer_at = 2;
    F.xfers = 0;
    CHECK(lr2021_write_tx_fifo(d, data, 3) == LR2021_OK, "FIFO write one transfer");
    F.fail_xfer_at = 1;
    F.xfers = 0;
    CHECK(lr2021_write_tx_fifo(d, data, 3) == LR2021_FAIL, "FIFO write SPI error");
    F.fail_xfer_at = 0;
    static uint8_t big[300];
    CHECK(lr2021_write_tx_fifo(d, whole, 1025) == LR2021_ERR_INVALID_SIZE && lr2021_write_tx_fifo(d, whole, 1024) == LR2021_OK,
          "FIFO write sizes");
    (void)big;

    // the one-call readers: opcode out, big-endian answer back
    script_reset();
    F.xfers = 0;
    uint8_t maj = 0xAA, min = 0xAA;
    static const uint8_t v[4] = {0x06, 0x01, 0x01, 0x18};
    answer(1, v, 4);
    CHECK(lr2021_version(d, &maj, &min) == LR2021_OK && maj == 1 && min == 0x18 && F.tx[0][0] == 0x01 && F.tx[0][1] == 0x01, "version 1.24");
    // a failed read gives zeros, not stack garbage
    maj = min = 0xAA;
    F.xfers = 0;
    F.fail_xfer_at = 2;
    F.xfer_err = LR2021_FAIL;
    CHECK(lr2021_version(d, &maj, &min) == LR2021_FAIL && maj == 0 && min == 0, "version on error: 0.0 (%u.%u)", maj, min);
    F.fail_xfer_at = 0;
    static const uint8_t e2[4] = {0x06, 0x01, 0x01, 0x02};
    answer(1, e2, 4);
    F.xfers = 0;
    uint16_t u16 = 0;
    CHECK(lr2021_errors(d, &u16) == LR2021_OK && u16 == 0x0102 && F.tx[0][0] == 0x01 && F.tx[0][1] == 0x10, "errors");
    F.xfers = 0;
    CHECK(lr2021_rx_fifo_level(d, &u16) == LR2021_OK && u16 == 0x0102 && F.tx[0][0] == 0x01 && F.tx[0][1] == 0x1C, "rx fifo level");
    F.xfers = 0;
    CHECK(lr2021_tx_fifo_level(d, &u16) == LR2021_OK && u16 == 0x0102 && F.tx[0][0] == 0x01 && F.tx[0][1] == 0x1D, "tx fifo level");
    uint8_t rx = 0, tx = 0;
    F.xfers = 0;
    CHECK(lr2021_fifo_flags(d, &rx, &tx) == LR2021_OK && rx == 1 && tx == 2 && F.tx[0][0] == 0x01 && F.tx[0][1] == 0x2E, "fifo flags (clearing)");
    F.xfers = 0;
    CHECK(lr2021_fifo_flags_peek(d, &rx, &tx) == LR2021_OK && rx == 1 && tx == 2 && F.tx[0][0] == 0x01 && F.tx[0][1] == 0x1B, "fifo flags (peek)");
    // zeros on error for all of them
    F.xfers = 0;
    F.fail_xfer_at = 2;
    u16 = 9;
    rx = tx = 9;
    CHECK(lr2021_errors(d, &u16) == LR2021_FAIL && u16 == 0, "errors on error");
    F.xfers = 0;
    u16 = 9;
    CHECK(lr2021_rx_fifo_level(d, &u16) == LR2021_FAIL && u16 == 0, "rx level on error");
    F.xfers = 0;
    u16 = 9;
    CHECK(lr2021_tx_fifo_level(d, &u16) == LR2021_FAIL && u16 == 0, "tx level on error");
    F.xfers = 0;
    CHECK(lr2021_fifo_flags(d, &rx, &tx) == LR2021_FAIL && rx == 0 && tx == 0, "flags on error");
    rx = tx = 9;
    F.xfers = 0;
    CHECK(lr2021_fifo_flags_peek(d, &rx, &tx) == LR2021_FAIL && rx == 0 && tx == 0, "peek on error");
    F.fail_xfer_at = 0;

    // ReadRegMem32: the builder's frame, a big-endian word back
    uint8_t want[8];
    size_t wn = lr2021_cmd_read_reg32(want, 0xF30030, 1);
    static const uint8_t w[6] = {0x06, 0x01, 0x12, 0x34, 0x56, 0x78};
    answer(1, w, 6);
    F.xfers = 0;
    uint32_t val = 0;
    CHECK(lr2021_read32(d, 0xF30030, &val) == LR2021_OK && val == 0x12345678 && F.tx_len[0] == wn && !memcmp(F.tx[0], want, wn),
          "read32 %08x", val);
    F.xfers = 0;
    F.fail_xfer_at = 1;
    val = 9;
    CHECK(lr2021_read32(d, 0xF30030, &val) == LR2021_FAIL && val == 0, "read32 on error: 0");
    F.fail_xfer_at = 0;
    lr2021_close(d);
}

// The SPI clock: through the HAL, refused out of range or without one
static void clock_change(void)
{
    lr2021_t *d = open_ok();
    CHECK(lr2021_set_clock(d, 0, 0) == LR2021_ERR_INVALID_ARG && lr2021_set_clock(d, 80000001, 0) == LR2021_ERR_INVALID_ARG &&
              F.clock_sets == 0,
          "clock out of range refused");
    CHECK(lr2021_set_clock(d, 24000000, 5) == LR2021_OK && F.clock_sets == 1 && F.clock_hz == 24000000 &&
              F.input_delay_ns == 5,
          "24 MHz, 5 ns (%d %d)", F.clock_hz, F.input_delay_ns);
    lr2021_close(d);
    shim_reset();
    lr2021_config_t c = config();
    c.hal.set_clock = NULL;
    CHECK(lr2021_open(&c, &d) == LR2021_OK && lr2021_set_clock(d, 8000000, 0) == LR2021_ERR_NOT_SUPPORTED,
          "a HAL with a fixed clock: not supported");
    lr2021_close(d);
}

// The IRQ pin: the HAL's, waited on; none, and the wait says no at once.
static void irq_pin(void)
{
    shim_reset();
    script_reset();
    F.has_irq = true;
    lr2021_config_t c = config();
    lr2021_t *d = NULL;
    CHECK(lr2021_open(&c, &d) == LR2021_OK && d && lr2021_has_irq(d), "open with an IRQ pin");
    F.irq_level = 1;
    CHECK(lr2021_wait_irq(d, 20) && F.irq_waits == 1, "high: at once");
    F.irq_level = 0;
    int64_t t0 = F.now_us;
    CHECK(!lr2021_wait_irq(d, 20) && F.now_us - t0 == 20000, "low: times out");
    lr2021_close(d);
    d = open_ok();
    CHECK(!lr2021_has_irq(d) && !lr2021_wait_irq(d, 20) && F.irq_waits == 0, "no IRQ pin");
    lr2021_close(d);
}

// What comes back: a command receives nothing or its status rounded up to 4 (an ESP32's SPI DMA then needs no buffer
// of its own); only answers that end off a multiple of 4 aren't. The handle in the caller's memory.
static void test_alignment(void)
{
    lr2021_t *d = open_ok();
    lr2021_config_t c = config();
    F.unaligned = 0;
    static const uint8_t ok[4] = {0x05, 0x24, 0, 0};
    answer(2, ok, 4); /* write's GetStatus */
    answer(5, ok, 4); /* the version's answer */
    static const uint8_t ci[6] = {0x01, 0x16, 0, 0, 0, 1};
    CHECK(lr2021_send(d, ci, 6) == LR2021_OK && F.rx_len[0] == 0, "send: nothing received (%zu)", F.rx_len[0]);
    CHECK(lr2021_write(d, ci, 6) == LR2021_OK && F.rx_len[1] == 0 && F.tx_len[2] == 6 && F.rx_len[2] == 4,
          "write: the frame receives nothing, its GetStatus the status rounded up to 4 (%zu %zu)", F.rx_len[1],
          F.rx_len[2]);
    lr2021_status_t st;
    CHECK(lr2021_get_status(d, &st) == LR2021_OK && F.rx_len[3] == 4, "status: 4 of 6");
    uint8_t maj, min;
    CHECK(lr2021_version(d, &maj, &min) == LR2021_OK && F.rx_len[4] == 0 && F.tx_len[5] == 4 && F.rx_len[5] == 4,
          "a read: its command receives nothing, the answer all 4");
    static uint8_t out[1100];
    CHECK(lr2021_read_fifo(d, out, 342) == LR2021_OK && F.rx_len[6] == 344, "a FIFO read a multiple of 4");
    CHECK(lr2021_write_tx_fifo(d, out, 9) == LR2021_OK && F.rx_len[7] == 0, "a TX FIFO write receives nothing");
    CHECK(F.unaligned == 0, "all aligned (%d)", F.unaligned);
    uint32_t irq;
    CHECK(lr2021_irq(d, &irq) == LR2021_OK && F.rx_len[8] == 6 && F.unaligned == 1, "the IRQ word: 6 bytes");
    lr2021_close(d);

    // the caller's memory: cleared first, left on close
    shim_reset();
    static uint32_t mem[2048];
    CHECK(lr2021_handle_size() > 2 * 1026 && lr2021_handle_size() <= sizeof mem, "handle size %zu", lr2021_handle_size());
    memset(mem, 0xA5, sizeof mem);
    c.mem = mem;
    CHECK(lr2021_open(&c, &d) == LR2021_OK && (void *)d == (void *)mem, "open in the caller's memory");
    CHECK(lr2021_failures(d) == 0, "cleared");
    lr2021_close(d);
    c.mem = (uint8_t *)mem + 2;
    CHECK(lr2021_open(&c, &d) == LR2021_ERR_INVALID_ARG, "memory not aligned to 4: refused");
}

int main(void)
{
    opening();
    open_failures();
    busy_handshake();
    status_and_write();
    reading();
    fifo_and_accessors();
    irq_pin();
    clock_change();
    test_alignment();
    printf("lr2021 driver: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
