// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
// Host tests for lr2021_packet_send / _receive (src/lr2021_packet.c) over the shimmed ESP-IDF calls and a small
// model of a packet modem: a FIFO, SetTx draining it at a fixed rate to TX_DONE, SetRx delivering a scripted packet
// (a byte at a time) and an IRQ. Not the chip: it pins what the calls send and how they read the answers.
#include <stdio.h>
#include <string.h>

#include "esp_shim.h"
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

#define RX_DONE (1u << 18)
#define TX_DONE (1u << 19)
#define TIMEOUT (1u << 21)
#define CRC_ERROR (1u << 22)
#define HEADER_ERROR (1u << 9)

static struct {
    int mode;            /* LR2021_MODE_* */
    int cap;             /* FIFO size */
    uint8_t fifo[2048];  /* TX: bytes in; RX: bytes received, not yet read */
    int level, rd;       /* RX: fifo[rd .. rd+level) */
    int sent_total;      /* TX: bytes the modem sent */
    int tx_len;          /* TX: the packet's length (the modem stops there) */
    double bytes_per_us;
    double carry;
    int64_t last_us;
    uint32_t irq;
    /* RX script */
    const uint8_t *pkt;
    int pkt_len, pkt_at;
    int64_t rx_start_us, rx_delay_us; /* the packet starts arriving rx_delay_us after SetRx */
    uint32_t rx_end_irq;              /* raised when it's all in (RX_DONE, maybe with CRC_ERROR) */
    bool never;                       /* no packet: the chip's timeout fires */
    uint32_t rx_timeout;
    uint16_t pending;
    int overflow, standbys, settx, setrx;
    bool refuse;         /* the last command was refused: the next GetStatus says FAIL */
    uint8_t txdata[2048];
} M;

static void advance(void)
{
    double dt = (double)(F.now_us - M.last_us);
    M.last_us = F.now_us;
    if (M.mode == LR2021_MODE_TX) {
        M.carry += dt * M.bytes_per_us;
        while (M.carry >= 1 && M.sent_total < M.tx_len && M.level > 0) {
            M.txdata[M.sent_total++] = M.fifo[M.rd];
            M.rd++;
            M.level--;
            M.carry -= 1;
        }
        if (M.sent_total >= M.tx_len) {
            M.mode = LR2021_MODE_STBY_RC;
            M.irq |= TX_DONE;
        }
    } else if (M.mode == LR2021_MODE_RX) {
        int64_t since = F.now_us - M.rx_start_us;
        if (M.never) {
            if (M.rx_timeout && since > (int64_t)M.rx_timeout * 1000000 / 32768) {
                M.mode = LR2021_MODE_STBY_RC;
                M.irq |= TIMEOUT;
            }
            return;
        }
        if (since < M.rx_delay_us)
            return;
        M.carry += dt * M.bytes_per_us;
        while (M.carry >= 1 && M.pkt_at < M.pkt_len) {
            if (M.level >= M.cap)
                M.overflow++;
            else
                M.fifo[(M.rd + M.level++) % (int)sizeof M.fifo] = M.pkt[M.pkt_at];
            M.pkt_at++;
            M.carry -= 1;
        }
        if (M.pkt_at >= M.pkt_len) {
            M.mode = LR2021_MODE_STBY_RC;
            M.irq |= M.rx_end_irq;
        }
    }
}

static uint32_t be32(const uint8_t *b) { return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; }

static void respond(int idx, const uint8_t *tx, size_t len, uint8_t *rx)
{
    (void)idx;
    advance();
    rx[0] = (M.refuse ? LR2021_CMD_FAIL : LR2021_CMD_OK) << 1;
    rx[1] = (uint8_t)M.mode;
    M.refuse = false;
    const uint16_t op = (uint16_t)(tx[0] << 8 | tx[1]);
    if (op == 0 && M.pending) {
        uint16_t v = 0;
        if (M.pending == LR2021_GET_TX_FIFO_LEVEL)
            v = (uint16_t)M.level;
        else if (M.pending == LR2021_GET_RX_FIFO_LEVEL)
            v = (uint16_t)M.level;
        else if (M.pending == 0x0212)
            v = (uint16_t)M.pkt_len;
        rx[2] = (uint8_t)(v >> 8);
        rx[3] = (uint8_t)v;
        M.pending = 0;
        return;
    }
    switch (op) {
    case 0x0001: /* ReadRxFifo: data in this frame */
        for (size_t i = 2; i < len && M.level > 0; i++) {
            rx[i] = M.fifo[M.rd % (int)sizeof M.fifo];
            M.rd++;
            M.level--;
        }
        break;
    case LR2021_GET_STATUS:
        if (len >= 6)
            rx[2] = (uint8_t)(M.irq >> 24), rx[3] = (uint8_t)(M.irq >> 16), rx[4] = (uint8_t)(M.irq >> 8),
            rx[5] = (uint8_t)M.irq;
        break;
    case LR2021_WRITE_TX_FIFO:
        for (size_t i = 2; i < len; i++) {
            if (M.rd + M.level >= (int)sizeof M.fifo || M.level >= M.cap)
                M.overflow++;
            else
                M.fifo[M.rd + M.level++] = tx[i];
        }
        break;
    case LR2021_CLEAR_TX_FIFO:
    case LR2021_CLEAR_RX_FIFO:
        if (M.mode == LR2021_MODE_RX || M.mode == LR2021_MODE_TX) /* as the chip does (refused out of standby) */
            M.refuse = true;
        else
            M.level = 0, M.rd = 0;
        break;
    case LR2021_CLEAR_IRQ: M.irq &= ~be32(tx + 2); break;
    case LR2021_SET_TX:
        M.settx++;
        M.mode = LR2021_MODE_TX;
        break;
    case LR2021_SET_RX:
        M.setrx++;
        M.mode = LR2021_MODE_RX;
        M.rx_start_us = F.now_us;
        M.rx_timeout = (uint32_t)tx[2] << 16 | (uint32_t)tx[3] << 8 | tx[4];
        break;
    case LR2021_SET_STANDBY:
        M.mode = LR2021_MODE_STBY_RC;
        M.standbys++;
        break;
    case LR2021_GET_TX_FIFO_LEVEL:
    case LR2021_GET_RX_FIFO_LEVEL:
    case 0x0212: M.pending = op; break;
    default: break;
    }
}

static lr2021_t *open_dev(int cap)
{
    shim_reset();
    memset(&M, 0, sizeof M);
    M.mode = LR2021_MODE_STBY_RC;
    M.cap = cap;
    F.respond = respond;
    lr2021_config_t c = {.spi_host = 1, .sck = 1, .mosi = 2, .miso = 3, .nss = 4, .busy = 5, .nreset = 6, .irq = -1};
    lr2021_t *d = NULL;
    lr2021_open(&c, &d);
    M.last_us = F.now_us;
    return d;
}

static void sending(void)
{
    static uint8_t data[700];
    for (size_t i = 0; i < sizeof data; i++)
        data[i] = (uint8_t)(i * 7 + 1);
    lr2021_packet_result_t r;

    // a short packet: all of it in the FIFO, then SetTx, then TX_DONE
    lr2021_t *d = open_dev(256);
    M.tx_len = 20;
    M.bytes_per_us = 0.001; /* 8 kb/s */
    lr2021_packet_cfg_t c = {.max_ms = 1000};
    esp_err_t e = lr2021_packet_send(d, data, 20, &c, &r);
    CHECK(e == ESP_OK && (r.irq & TX_DONE) && r.len == 20 && M.settx == 1, "short: %d irq %08X", e, (unsigned)r.irq);
    CHECK(!memcmp(M.txdata, data, 20) && M.overflow == 0, "short: bytes");
    CHECK(r.done_us >= 19000 && r.done_us < 40000, "short: %lld us", (long long)r.done_us);
    lr2021_close(d);

    // longer than the FIFO: prefilled to 256, the rest fed as it drains, nothing lost or overflowed
    d = open_dev(256);
    M.tx_len = 700;
    M.bytes_per_us = 0.05;
    e = lr2021_packet_send(d, data, 700, &c, &r);
    CHECK(e == ESP_OK && r.len == 700 && M.sent_total == 700 && M.overflow == 0, "long: %d sent %d overflow %d", e,
          M.sent_total, M.overflow);
    CHECK(!memcmp(M.txdata, data, 700), "long: bytes");
    lr2021_close(d);

    // the 1 KB FIFO: one fill
    d = open_dev(1020);
    M.tx_len = 700;
    M.bytes_per_us = 0.05;
    c.fifo_bytes = 1020;
    int before = F.xfers;
    e = lr2021_packet_send(d, data, 700, &c, &r);
    CHECK(e == ESP_OK && M.sent_total == 700 && M.overflow == 0, "1K: %d", e);
    (void)before;
    c.fifo_bytes = 0;
    lr2021_close(d);

    // the chip's timeout: ESP_ERR_TIMEOUT
    d = open_dev(256);
    M.tx_len = 1000; /* never reached */
    M.bytes_per_us = 0;
    M.irq = 0;
    c.max_ms = 30;
    e = lr2021_packet_send(d, data, 10, &c, &r);
    CHECK(e == ESP_ERR_TIMEOUT && r.timed_out && M.standbys == 2 && M.mode != LR2021_MODE_TX, "tx timeout: %d", e); /* at the start and the end */
    c.max_ms = 1000;
    lr2021_close(d);
}

static void receiving(void)
{
    static uint8_t pkt[600], buf[700];
    for (size_t i = 0; i < sizeof pkt; i++)
        pkt[i] = (uint8_t)(i * 13 + 5);
    lr2021_packet_result_t r;
    lr2021_packet_cfg_t c = {.max_ms = 2000, .timeout = 32768};

    // a short packet after 50 ms
    lr2021_t *d = open_dev(256);
    M.pkt = pkt;
    M.pkt_len = 40;
    M.rx_delay_us = 50000;
    M.bytes_per_us = 0.01;
    M.rx_end_irq = RX_DONE;
    memset(buf, 0, sizeof buf);
    esp_err_t e = lr2021_packet_receive(d, buf, sizeof buf, &c, &r);
    CHECK(e == ESP_OK && r.len == 40 && !memcmp(buf, pkt, 40), "short rx: %d len %zu", e, r.len);
    CHECK(M.rx_timeout == 32768 && M.setrx == 1, "SetRx timeout %u", (unsigned)M.rx_timeout);
    lr2021_close(d);

    // longer than the FIFO at 400 kb/s: drained as it comes with a short poll, no overflow
    d = open_dev(256);
    c.poll_us = 200;
    M.pkt = pkt;
    M.pkt_len = 600;
    M.rx_delay_us = 1000;
    M.bytes_per_us = 0.05;
    M.rx_end_irq = RX_DONE;
    memset(buf, 0, sizeof buf);
    e = lr2021_packet_receive(d, buf, sizeof buf, &c, &r);
    CHECK(e == ESP_OK && r.len == 600 && M.overflow == 0 && !memcmp(buf, pkt, 600), "long rx: %d overflow %d", e,
          M.overflow);
    c.poll_us = 0;
    lr2021_close(d);

    // a bad CRC: the payload is there, the error says so
    d = open_dev(256);
    M.pkt = pkt;
    M.pkt_len = 16;
    M.bytes_per_us = 0.01;
    M.rx_end_irq = RX_DONE | CRC_ERROR;
    e = lr2021_packet_receive(d, buf, sizeof buf, &c, &r);
    CHECK(e == ESP_ERR_INVALID_CRC && r.crc_error && !memcmp(buf, pkt, 16), "crc: %d", e);
    lr2021_close(d);

    // a header error: no packet
    d = open_dev(256);
    M.pkt = pkt;
    M.pkt_len = 4;
    M.bytes_per_us = 0.01;
    M.rx_end_irq = HEADER_ERROR;
    e = lr2021_packet_receive(d, buf, sizeof buf, &c, &r);
    CHECK(e == ESP_ERR_INVALID_RESPONSE && !r.timed_out, "header error: %d", e);
    lr2021_close(d);

    // nothing on air: the chip's timeout
    d = open_dev(256);
    M.never = true;
    c.timeout = 3277; /* 100 ms */
    e = lr2021_packet_receive(d, buf, sizeof buf, &c, &r);
    CHECK(e == ESP_ERR_TIMEOUT && r.timed_out && (r.irq & TIMEOUT), "rx timeout: %d", e);
    lr2021_close(d);

    // a packet longer than the buffer: the start kept, the rest read out and dropped
    d = open_dev(256);
    M.pkt = pkt;
    M.pkt_len = 100;
    M.bytes_per_us = 0.01;
    M.rx_end_irq = RX_DONE;
    c.timeout = 0;
    memset(buf, 0, sizeof buf);
    e = lr2021_packet_receive(d, buf, 30, &c, &r);
    CHECK(e == ESP_ERR_INVALID_SIZE && r.len == 100 && !memcmp(buf, pkt, 30) && buf[30] == 0 && M.level == 0,
          "too long: %d", e);
    lr2021_close(d);

    // the chip left in RX (a run still going): the call puts it in standby first, and the packet comes in
    d = open_dev(256);
    M.mode = LR2021_MODE_RX;
    M.pkt = pkt;
    M.pkt_len = 8;
    M.bytes_per_us = 0.01;
    M.rx_end_irq = RX_DONE;
    e = lr2021_packet_receive(d, buf, sizeof buf, &c, &r);
    CHECK(e == ESP_OK && r.len == 8 && M.standbys >= 1, "from RX: %d", e);
    lr2021_close(d);

    CHECK(lr2021_packet_receive(NULL, buf, 10, &c, &r) == ESP_ERR_INVALID_ARG, "no dev");
}

int main(void)
{
    sending();
    receiving();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
