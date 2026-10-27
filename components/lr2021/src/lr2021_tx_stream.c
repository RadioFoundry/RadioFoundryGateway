// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * A transmission from a pull callback (lr2021.h, lr2021_tx_stream): prefill the 1 KB TX FIFO, key up, keep the
 * FIFO topped up, and start the next run on each TX_DONE until the data ends. Only the driver's public calls:
 * docs/spec/lr2021-iq.md section 3 has the facts (R3.11-R3.17).
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lr2021_port.h"
#include "lr2021.h"

#define FIFO_BYTES 1020 /* the 1 KB TX FIFO's size register, 0x3FC (R2.22) */

static lr2021_err_t send_frame(lr2021_t *d, const uint8_t *f, size_t n) { return lr2021_send(d, f, n); }

static void standby(lr2021_t *d)
{
    uint8_t f[4];
    send_frame(d, f, lr2021_cmd_u8(f, LR2021_SET_STANDBY, 0));
}

lr2021_err_t lr2021_tx_stream(lr2021_t *d, const lr2021_tx_stream_cfg_t *c, lr2021_tx_pull_t pull, void *ctx,
                           lr2021_tx_stream_stats_t *st)
{
    lr2021_tx_stream_stats_t z = {0};
    if (!st)
        st = &z;
    memset(st, 0, sizeof *st);
    st->done_level = -1;
    if (!c || !c->start || !c->start_len)
        return LR2021_ERR_INVALID_ARG;
    uint8_t f[12];
    lr2021_err_t e;

    if (c->run_bytes == 0) { /* a carrier: key up and leave it on (SetStandby ends it) */
        st->key_us = lr2021_now_us();
        return lr2021_write(d, c->start, c->start_len);
    }
    if (!pull)
        return LR2021_ERR_INVALID_ARG;
    uint8_t *buf = c->buf && c->buf_len >= FIFO_BYTES ? c->buf : malloc(FIFO_BYTES);
    if (!buf)
        return LR2021_ERR_NO_MEM;
    const int chunk = c->chunk > 0 ? c->chunk : 256;
    const int64_t max_us = (int64_t)(c->max_ms > 0 ? c->max_ms : 10000) * 1000;

    /* an empty FIFO, its flags and the IRQs a run raises cleared */
    if ((e = lr2021_write(d, f, lr2021_cmd(f, LR2021_CLEAR_TX_FIFO))) != LR2021_OK ||
        (e = lr2021_write(d, f, lr2021_cmd_clear_fifo_flags(f, 0, 0x3F))) != LR2021_OK ||
        (e = lr2021_write(d, f, lr2021_cmd_clear_irq(f, LR2021_IRQ_TX_DONE | LR2021_IRQ_TIMEOUT))) != LR2021_OK)
        goto out;

    bool pulled_all = false;
    size_t n = pull(ctx, buf, FIFO_BYTES);
    for (int64_t t0 = lr2021_now_us(); n == LR2021_TX_PULL_WAIT; n = pull(ctx, buf, FIFO_BYTES)) {
        if ((c->stop && *c->stop) || lr2021_now_us() - t0 > 2000000) {
            n = 0;
            break;
        }
        lr2021_sleep_us(500);
    }
    if (n == 0) {
        e = LR2021_ERR_INVALID_SIZE; /* nothing to send */
        goto out;
    }
    if ((e = lr2021_write_tx_fifo(d, buf, n)) != LR2021_OK)
        goto out;
    st->sent = n;
    st->ready_us = lr2021_now_us();
    /* a timed start: the SetTx's frame takes some us to go (BUSY, the SPI): started that much early, as the last
     * one took, so it ends (the chip keys up) at the time */
    static int32_t settx_us = 30; /* the W12 at 16 MHz: 24-35 */
    const int64_t at = c->at_us ? c->at_us - settx_us : 0;
    if (c->at_us) {
        /* a timed start: asleep until just before it (a tick's sleep can end early, a timer's late), the last
         * 30 us spun (lr2021_sleep_us spins that short) */
        int64_t now = lr2021_now_us();
        if (now > at + LR2021_TX_LATE_US) {
            st->late = true;
            st->sent = 0;
            lr2021_write(d, f, lr2021_cmd(f, LR2021_CLEAR_TX_FIFO));
            goto out;
        }
        for (; now < at; now = lr2021_now_us()) {
            if (c->stop && *c->stop) { /* nothing sent yet */
                st->stopped = true;
                st->sent = 0;
                lr2021_write(d, f, lr2021_cmd(f, LR2021_CLEAR_TX_FIFO));
                goto out;
            }
            /* a long wait in steps of at most 10 ms, to see stop */
            const int64_t left = at - now;
            lr2021_sleep_us((uint32_t)(left >= 60 ? (left - 30 > 10000 ? 10000 : left - 30) : left > 25 ? 25 : left));
        }
    }
    const int64_t sending = lr2021_now_us();
    if ((e = send_frame(d, c->start, c->start_len)) != LR2021_OK)
        goto out;
    st->key_us = lr2021_now_us();
    if (c->at_us && st->key_us - sending < 200)
        settx_us = (int32_t)(st->key_us - sending);

    /* The FIFO's level is read when a refill may be due, and reckoned from the rate in between. With the IRQ pin,
     * TX_DONE is a GPIO read: the loop sleeps until a refill or the run's end is near, then watches the pin, and
     * sends the next SetTx before anything else (R3.45). Without it, the IRQs are read over SPI each time round. */
    const double bpus = c->bytes_per_s / 1e6;
    const bool pin = c->irq_pin && bpus > 0 && lr2021_irq_pin(d) >= 0;
    const int64_t run_us = bpus > 0 ? (int64_t)(c->run_bytes / bpus) : 0;
    int64_t run_start = st->key_us, last = st->key_us, level_t = st->key_us;
    int level_was = (int)n; /* bytes in the FIFO at level_t */
    double write_us = 0.6;  /* a FIFO write's time a byte, measured as it goes (16 MHz SPI: about 0.5) */
    bool held = false;      /* refills wait for the next run: the FIFO lasts past this one's end */
    int64_t retry_at = 0;   /* the pull had nothing yet: ask again from then */
    /* The next run's SetTx sent ahead of this run's TX_DONE (R3.47): the chip takes it in the run's last 45-60 us
     * and starts the next run sooner. TX_DONE is predicted from the run's start (its SetTx, or the TX_DONE before
     * for a queued one): its length plus the delay measured for runs started that way. Refused (too early) costs
     * more than it saves, so the lead backs off 10 us after a refusal and creeps back 1 us a success. */
    int64_t queued_at = 0;  /* the next run's SetTx went out then */
    int after_pin = c->learned ? c->learned[0] : -1; /* TX_DONE past a run's length from its start, us */
    int after_queued = c->learned ? c->learned[1] : -1;
    int after_first = c->learned ? c->learned[2] : -1;
    bool run_queued = false; /* this run was started by a queued SetTx */
    int lead = 35;          /* the queued SetTx this far ahead of the predicted TX_DONE, us */
    for (;;) {
        int64_t now = lr2021_now_us();
        if (now - last > (int64_t)st->gap_us)
            st->gap_us = (uint32_t)(now - last);
        last = now;
        if (now - st->key_us > max_us) {
            st->timed_out = true;
            e = LR2021_ERR_TIMEOUT;
            break;
        }
        if (c->stop && *c->stop) {
            st->stopped = true;
            standby(d);
            break;
        }
        /* the data ended inside this run: cut it once the FIFO is empty (cfg.cut_end), from 100 us before the
         * reckoned moment, reading the level */
        if (c->cut_end && pulled_all && st->sent < (size_t)(st->runs + 1) * c->run_bytes) {
            const int64_t empty_at = bpus > 0 ? level_t + (int64_t)(level_was / bpus) : now;
            if (empty_at - now > 130) {
                lr2021_sleep_us((uint32_t)(empty_at - now - 100));
                continue;
            }
            uint16_t level = 0;
            if ((e = lr2021_tx_fifo_level(d, &level)) != LR2021_OK)
                break;
            if (level == 0) {
                standby(d);
                st->cut = true;
                st->end_us = lr2021_now_us();
                break;
            }
            level_was = level;
            level_t = lr2021_now_us();
            continue;
        }
        /* a pin that never rose (a run that ends later than reckoned): look over SPI too */
        const bool overdue = pin && now - run_start > run_us + 2000;
        const bool raised = pin && lr2021_irq_pin(d) > 0; /* read once: a second read could see an edge the first
                                                            * didn't and take the slow way */
        const bool next_in = st->sent > (size_t)(st->runs + 1) * c->run_bytes; /* the next run's bytes are in */
        const int after = st->runs == 0 ? after_first
                          : run_queued && after_queued >= 0 ? after_queued
                                                            : after_pin; /* about the same */
        if (pin && !raised && !overdue && next_in && !queued_at && after >= 0 && now >= run_start + run_us + after - lead) {
            if ((e = send_frame(d, c->start, c->start_len)) != LR2021_OK)
                break;
            queued_at = lr2021_now_us();
            continue;
        }
        if (raised && !overdue && (next_in || queued_at)) {
            /* TX_DONE with the next run's bytes in the FIFO: SetTx at once (unless it went in already), the IRQ
             * cleared after (R3.44), then the checks */
            int64_t t = queued_at;
            if (!t) {
                if ((e = send_frame(d, c->start, c->start_len)) != LR2021_OK)
                    break;
                t = lr2021_now_us();
            }
            if ((e = lr2021_send(d, f, lr2021_cmd_clear_irq(f, LR2021_IRQ_TX_DONE))) != LR2021_OK)
                break;
            {
                /* TX_DONE's delay past the run's length, for runs started this way (the first from standby) */
                const int a = (int)(now - run_start - run_us);
                int *m = st->runs == 0 ? &after_first : run_queued ? &after_queued : &after_pin;
                if (a >= 0 && a < 1000)
                    *m = *m < 0 ? a : (3 * *m + a) / 4;
            }
            run_queued = false;
            if (queued_at) {
                /* taken: the chip in TX already. Refused (sent mid-run): back in FS, so SetTx now */
                lr2021_status_t s;
                if ((e = lr2021_get_status(d, &s)) != LR2021_OK)
                    break;
                if (s.mode == LR2021_MODE_TX) {
                    st->queued++;
                    run_queued = true;
                    t = now;
                    if (lead < 45)
                        lead++;
                } else {
                    st->refused++;
                    lead = lead > 20 ? lead - 10 : 10;
                    if ((e = send_frame(d, c->start, c->start_len)) != LR2021_OK)
                        break;
                    t = lr2021_now_us();
                }
            }
            if (t - now > (int64_t)st->restart_us)
                st->restart_us = (uint32_t)(t - now);
            queued_at = 0;
            st->runs++;
            st->end_us = now;
            run_start = t;
            held = false;
            uint32_t irq = 0;
            uint16_t level = 0;
            if ((e = lr2021_irq(d, &irq)) != LR2021_OK || (e = lr2021_tx_fifo_level(d, &level)) != LR2021_OK)
                break;
            if (irq & LR2021_IRQ_TIMEOUT) {
                st->timed_out = true;
                e = LR2021_ERR_TIMEOUT;
                break;
            }
            if (level > 0 && st->sent - level + 4 < (size_t)st->runs * c->run_bytes) { /* R3.17 */
                st->early = true;
                e = LR2021_ERR_INVALID_RESPONSE;
                break;
            }
            if (st->done_level < 0 || level < st->done_level)
                st->done_level = level;
            level_was = level;
            level_t = lr2021_now_us();
            continue;
        }
        if (!pin || overdue || raised) {
            uint32_t irq = 0;
            uint16_t level = 0;
            if ((e = lr2021_irq(d, &irq)) != LR2021_OK || (e = lr2021_tx_fifo_level(d, &level)) != LR2021_OK)
                break;
            level_was = level;
            level_t = lr2021_now_us();
            if (irq & LR2021_IRQ_TIMEOUT) {
                st->timed_out = true;
                e = LR2021_ERR_TIMEOUT;
                break;
            }
            if (irq & LR2021_IRQ_TX_DONE) {
                st->runs++;
                st->end_us = now;
                /* a run that ended short of its bytes with the FIFO still holding some sent nothing (R3.17): the
                 * caller sends it again. Short with the FIFO empty is an underflow: the run went out, holding its
                 * last sample. */
                const size_t taken = st->sent - level;
                if (level > 0 && taken + 4 < (size_t)st->runs * c->run_bytes) {
                    st->early = true;
                    e = LR2021_ERR_INVALID_RESPONSE;
                    break;
                }
                if (!pulled_all && level == 0 && st->sent <= (size_t)st->runs * c->run_bytes) {
                    /* every run's bytes taken and none pulled since (refills held near the end): any more? */
                    n = pull(ctx, buf, FIFO_BYTES);
                    if (n == LR2021_TX_PULL_WAIT) {
                        n = 0; /* the data fell behind: the run starts on what comes */
                    } else if (n == 0) {
                        pulled_all = true;
                    } else {
                        if ((e = lr2021_write_tx_fifo(d, buf, n)) != LR2021_OK)
                            break;
                        st->sent += n;
                        st->refills++;
                        level = (uint16_t)n;
                    }
                }
                if (pulled_all && level == 0) {
                    e = LR2021_OK;
                    break;
                }
                /* the next run: the setup persists, SetTx again (R3.15); SetTx first and the IRQ cleared after,
                 * without a status read, for a shorter seam (R3.44) */
                if (st->done_level < 0 || level < st->done_level)
                    st->done_level = level;
                if ((e = send_frame(d, c->start, c->start_len)) != LR2021_OK)
                    break;
                const int64_t t = lr2021_now_us();
                if ((e = lr2021_send(d, f, lr2021_cmd_clear_irq(f, LR2021_IRQ_TX_DONE))) != LR2021_OK)
                    break;
                if (t - now > (int64_t)st->restart_us)
                    st->restart_us = (uint32_t)(t - now);
                run_start = t;
                held = false;
                queued_at = 0; /* a queued SetTx taken makes this one CMD_FAIL: nothing changes */
                run_queued = false;
                continue;
            }
        }
        /* the FIFO now: reckoned while a run takes from it (never below empty), read when a refill may be due */
        double reckoned = level_was - (bpus > 0 ? (double)(now - level_t) * bpus : 0);
        if (reckoned < 0)
            reckoned = 0;
        int free_bytes = FIFO_BYTES - (int)reckoned;
        if (!pulled_all && !held && now >= retry_at && (free_bytes >= chunk || bpus <= 0)) {
            uint16_t level = 0;
            if ((e = lr2021_tx_fifo_level(d, &level)) != LR2021_OK)
                break;
            level_was = level;
            level_t = lr2021_now_us();
            if (level == 0)
                st->starved++;
            free_bytes = FIFO_BYTES - (int)level;
            int want = free_bytes;
            if (pin && st->sent >= (size_t)(st->runs + 1) * c->run_bytes) {
                /* a write that would run past the run's end holds up the restart (1 KB is about 0.5 ms): near the
                 * end, write what the run still needs and what fits before it, the rest after. (A run whose last
                 * byte isn't in the FIFO yet can't end: no limit then.) */
                const int64_t to_end = run_start + run_us - level_t;
                const int fits = (int)((to_end - 100) / write_us);
                const int needs = (int)((to_end + 1000) * bpus) - (int)level;
                if (fits < want)
                    want = fits > needs ? fits : needs;
            }
            if (want >= chunk || (want > 0 && want < free_bytes && want >= 16)) {
                want &= ~3;
                n = pull(ctx, buf, (size_t)(want < free_bytes ? want : free_bytes));
                if (n == LR2021_TX_PULL_WAIT) {
                    retry_at = lr2021_now_us() + 200;
                    continue;
                }
                if (n == 0) {
                    pulled_all = true;
                } else {
                    const int64_t w0 = lr2021_now_us();
                    if ((e = lr2021_write_tx_fifo(d, buf, n)) != LR2021_OK)
                        break;
                    const int64_t w1 = lr2021_now_us();
                    if (n >= 128 && w1 > w0)
                        write_us = 0.8 * write_us + 0.2 * (double)(w1 - w0) / (double)n;
                    st->sent += n;
                    st->refills++;
                    level_was += (int)n;
                }
                continue;
            }
            held = pin && want < free_bytes; /* cut to nothing near the end */
        }
        /* nothing to do: sleep until a refill is due or the run's end is near, then watch the pin (or the IRQs over
         * SPI) every 2 us */
        if (bpus <= 0) {
            lr2021_sleep_us(c->poll_us > 0 ? (uint32_t)c->poll_us : 100);
            continue;
        }
        int64_t wait = pulled_all || held ? INT64_MAX : (int64_t)((chunk - free_bytes) / bpus);
        if (!pulled_all && !held && retry_at - now > wait) /* the pull had nothing: not before its retry */
            wait = retry_at - now;
        const int64_t to_end = run_start + run_us - now - 150; /* from 150 us before the run's reckoned end */
        if (to_end < wait)
            wait = to_end;
        if (wait >= 30)
            lr2021_sleep_us(wait > 1000000 ? 1000000 : (uint32_t)wait);
        else
            lr2021_delay_us(2);
    }

    if (c->learned) {
        c->learned[0] = after_pin;
        c->learned[1] = after_queued;
        c->learned[2] = after_first;
    }
out:;
    uint8_t rx = 0, tx = 0;
    lr2021_fifo_flags_peek(d, &rx, &tx);
    st->tx_flags = tx;
    if (st->cut)
        st->tx_flags &= (uint8_t)~LR2021_FIFO_UNDERFLOW; /* the cut's own: the FIFO ran out as it ended (starved
                                                          * counts any before) */
    lr2021_status_t s;
    if (e != LR2021_OK && lr2021_get_status(d, &s) == LR2021_OK && s.mode == LR2021_MODE_TX)
        standby(d);
    if (buf != c->buf)
        free(buf);
    return e;
}
