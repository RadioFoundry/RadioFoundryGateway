// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * A transmission from a pull callback (lr2021.h, lr2021_tx_stream): prefill the 1 KB TX FIFO, key up, keep the
 * FIFO topped up, and start the next run on each TX_DONE until the data ends. Only the driver's public calls:
 * docs/spec/lr2021-iq.md section 3 has the facts (R3.11-R3.17).
 */
#include <stdlib.h>
#include <string.h>

#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lr2021.h"

#define FIFO_BYTES 1020 /* the 1 KB TX FIFO's size register, 0x3FC (R2.22) */

static esp_err_t send_frame(lr2021_t *d, const uint8_t *f, size_t n) { return lr2021_send(d, f, n); }

static void standby(lr2021_t *d)
{
    uint8_t f[4];
    send_frame(d, f, lr2021_cmd_u8(f, LR2021_SET_STANDBY, 0));
}

esp_err_t lr2021_tx_stream(lr2021_t *d, const lr2021_tx_stream_cfg_t *c, lr2021_tx_pull_t pull, void *ctx,
                           lr2021_tx_stream_stats_t *st)
{
    lr2021_tx_stream_stats_t z = {0};
    if (!st)
        st = &z;
    memset(st, 0, sizeof *st);
    if (!c || !c->start || !c->start_len)
        return ESP_ERR_INVALID_ARG;
    uint8_t f[12];
    esp_err_t e;

    if (c->run_bytes == 0) { /* a carrier: key up and leave it on (SetStandby ends it) */
        st->key_us = esp_timer_get_time();
        return lr2021_write(d, c->start, c->start_len);
    }
    if (!pull)
        return ESP_ERR_INVALID_ARG;
    uint8_t *buf = c->buf && c->buf_len >= FIFO_BYTES ? c->buf : malloc(FIFO_BYTES);
    if (!buf)
        return ESP_ERR_NO_MEM;
    const int chunk = c->chunk > 0 ? c->chunk : 256;
    const int64_t max_us = (int64_t)(c->max_ms > 0 ? c->max_ms : 10000) * 1000;

    /* an empty FIFO, its flags and the IRQs a run raises cleared */
    if ((e = lr2021_write(d, f, lr2021_cmd(f, LR2021_CLEAR_TX_FIFO))) != ESP_OK ||
        (e = lr2021_write(d, f, lr2021_cmd_clear_fifo_flags(f, 0, 0x3F))) != ESP_OK ||
        (e = lr2021_write(d, f, lr2021_cmd_clear_irq(f, LR2021_IRQ_TX_DONE | LR2021_IRQ_TIMEOUT))) != ESP_OK)
        goto out;

    bool pulled_all = false;
    size_t n = pull(ctx, buf, FIFO_BYTES);
    if (n == 0) {
        e = ESP_ERR_INVALID_SIZE; /* nothing to send */
        goto out;
    }
    if ((e = lr2021_write_tx_fifo(d, buf, n)) != ESP_OK)
        goto out;
    st->sent = n;
    if ((e = send_frame(d, c->start, c->start_len)) != ESP_OK)
        goto out;
    st->key_us = esp_timer_get_time();

    for (;;) {
        const int64_t now = esp_timer_get_time();
        if (now - st->key_us > max_us) {
            st->timed_out = true;
            e = ESP_ERR_TIMEOUT;
            break;
        }
        uint32_t irq = 0;
        uint16_t level = 0;
        if ((e = lr2021_irq(d, &irq)) != ESP_OK || (e = lr2021_tx_fifo_level(d, &level)) != ESP_OK)
            break;
        if (irq & LR2021_IRQ_TIMEOUT) {
            st->timed_out = true;
            e = ESP_ERR_TIMEOUT;
            break;
        }
        if (irq & LR2021_IRQ_TX_DONE) {
            st->runs++;
            st->end_us = now;
            /* a run that ended short of its bytes with the FIFO still holding some sent nothing (R3.17): the caller
             * sends it again. Short with the FIFO empty is an underflow: the run went out, holding its last sample. */
            const size_t taken = st->sent - level;
            if (level > 0 && taken + 4 < (size_t)st->runs * c->run_bytes) {
                st->early = true;
                e = ESP_ERR_INVALID_RESPONSE;
                break;
            }
            if (pulled_all && level == 0) {
                e = ESP_OK;
                break;
            }
            /* the next run: the setup persists, SetTx again (R3.15) */
            if ((e = lr2021_write(d, f, lr2021_cmd_clear_irq(f, LR2021_IRQ_TX_DONE))) != ESP_OK ||
                (e = send_frame(d, c->start, c->start_len)) != ESP_OK)
                break;
            continue;
        }
        const int free_bytes = FIFO_BYTES - (int)level;
        if (!pulled_all && free_bytes >= chunk) {
            n = pull(ctx, buf, (size_t)free_bytes);
            if (n == 0) {
                pulled_all = true;
            } else {
                if ((e = lr2021_write_tx_fifo(d, buf, n)) != ESP_OK)
                    break;
                st->sent += n;
                st->refills++;
            }
            continue;
        }
        /* nothing to do: sleep a tick if the FIFO lasts well past it, else a short wait */
        const int64_t until_chunk_us = c->bytes_per_s ? (int64_t)(chunk - free_bytes) * 1000000 / c->bytes_per_s : 0;
        if (pulled_all || until_chunk_us > 3 * (int64_t)portTICK_PERIOD_MS * 1000)
            vTaskDelay(1);
        else
            esp_rom_delay_us(c->poll_us > 0 ? (uint32_t)c->poll_us : 100);
    }

out:;
    uint8_t rx = 0, tx = 0;
    lr2021_fifo_flags_peek(d, &rx, &tx);
    st->tx_flags = tx;
    lr2021_status_t s;
    if (e != ESP_OK && lr2021_get_status(d, &s) == ESP_OK && s.mode == LR2021_MODE_TX)
        standby(d);
    if (buf != c->buf)
        free(buf);
    return e;
}
