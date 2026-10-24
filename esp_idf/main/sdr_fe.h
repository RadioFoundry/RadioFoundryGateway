// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The SDR role's front ends (rdof/sdr/frontend.h), one file each: the RTL-SDR on
 * USB (fe_rtlsdr.c), the self-test clip in a loop (fe_sim.c), an LR2021 on
 * SPI (fe_lr2021.c, CONFIG_RDOF_SDR_LR2021). sdr_task.c picks one by the
 * settings' device.type and drives it.
 */
#pragma once

#include "rdof/sdr/frontend.h"
#include "sdkconfig.h"

void fe_rtlsdr(rdof_fe_t *fe);
/* The dongle's registers as JSON, for looking for a temperature (reads only): the tuner's 32, demod pages 0-4, the SYS
 * block's 0x3000-0x30ff, each a hex string ("--" for a failed read). 0 with no dongle or too small a buffer. */
size_t fe_rtlsdr_regs_json(char *buf, size_t cap);
/* n words of the LR2021's memory from addr (ReadRegMem32, read only), between the reader's reads. -1: no chip or
 * an error. */
/* Experiments (web.c): lend the chip out (the reader stops; on release a new session starts), a command frame and
 * its answer (n bytes; 0 for a write) with the status after it, the FIFO read flat out into buf, the SPI clock
 * (hz <= 0: just say it). Each returns -1 if the chip isn't held (or there's none). */
int fe_lr2021_hold(bool on, bool keep, int patch); /* patch 1: load Semtech's patch RAM after the reset; 2: and ours */ /* keep: the chip left receiving as it was */
bool fe_lr2021_held(void);
int fe_lr2021_cmd(const uint8_t *frame, size_t len, uint8_t *out, size_t n, uint8_t status[2]);
const char *fe_lr2021_error(void);
int fe_lr2021_capture(uint8_t *buf, size_t n, size_t *got, unsigned *overflows, int64_t *us);
int fe_lr2021_spi(int hz, int delay_ns);
/* Frames in order, each timed to BUSY low; a length of 0 is a chip reset. Returns how many ran (-1: not held). */
int fe_lr2021_seq(const uint8_t *const *frames, const size_t *lens, int n, int64_t *us, uint8_t (*status)[2]);

/* A raw transmit run (experiments, web.c's /api/lr2021/tx). */
#define FE_LR2021_TX_TRACE 400
#define FE_LR2021_TX_AT 8
#define FE_LR2021_TX_RUNS 16
typedef struct {
    size_t prefill;  /* bytes in the FIFO before SetTx */
    size_t total;    /* bytes to send, the data repeated (0: the data once) */
    size_t chunk;    /* top the FIFO up once this much is free (0: 256) */
    uint32_t timeout; /* SetTx's, 1/32 ms (0: none) */
    int max_ms;      /* stop polling after this */
    int poll_us;     /* between polls with nothing to do (0: 100) */
    bool keep_fifo;  /* don't clear the FIFO and flags first */
    bool no_settx;   /* only fill and trace (the caller started TX) */
    bool trace_all;  /* trace every poll, not just changes */
    uint8_t start[16]; /* a frame to start with instead of SetTx (start_len 0: SetTx) */
    size_t start_len;
    int runs;        /* start again at each TX_DONE, this many runs in all (0 or 1: one) */
    struct {
        uint32_t us; /* from the first start */
        uint8_t frame[16];
        size_t len;
    } at[FE_LR2021_TX_AT]; /* in time order */
    int n_at;
} fe_lr2021_tx_opts_t;
typedef struct {
    uint32_t us;
    uint16_t level;
    uint8_t mode;
    uint32_t irq;
} fe_lr2021_tx_trace_t;
typedef struct {
    int64_t settx_us, done_us, left_tx_us, all_in_us, end_us;
    uint8_t start_cmd; /* the status code after the start frame */
    int runs;          /* TX_DONEs seen */
    int64_t run_start_us[FE_LR2021_TX_RUNS], run_done_us[FE_LR2021_TX_RUNS];
    int64_t at_us[FE_LR2021_TX_AT];
    uint8_t at_status[FE_LR2021_TX_AT][2];
    size_t sent;
    unsigned refills;
    bool timed_out;
    uint8_t rx_flags, tx_flags, mode;
    uint32_t irq;
    int n_trace;
    fe_lr2021_tx_trace_t trace[FE_LR2021_TX_TRACE];
} fe_lr2021_tx_result_t;
int fe_lr2021_tx(const uint8_t *data, size_t n, const fe_lr2021_tx_opts_t *o, fe_lr2021_tx_result_t *r);
int fe_lr2021_read_mem(uint32_t addr, uint32_t *words, int n);
/* A driver TX mode on the held chip (lr2021_tx.h): its setup frames (*setup_ok counts the ones that went), then
 * data through lr2021_tx_stream. -1 on an error (fe_lr2021_error says what; stats tell an early end). */
#include "lr2021.h"
#include "lr2021_tx.h"
int fe_lr2021_tx_mode(const lr2021_tx_setup_t *s, const uint8_t *data, size_t n, uint32_t timeout, int max_ms,
                      lr2021_tx_stream_stats_t *stats, int *setup_ok);
/* One packet (lr2021_packet_send / _receive) on the held chip; *err is the driver's result. -1 if not held. */
int fe_lr2021_packet(bool tx, uint8_t *data, size_t n, size_t *got, const lr2021_packet_cfg_t *c,
                     lr2021_packet_result_t *r, esp_err_t *err);
/* DMR 4-FSK reception of a family (LR2021_DMR_BS / _MS) at hz on the held chip, our patch running
 * (lr2021_fsk4_dmr_rx). -1 on an error (fe_lr2021_error). */
int fe_lr2021_dmr_rx(double hz, int family);
/* Wake the held chip from SetSleep (lr2021_wake). -1 on an error. */
int fe_lr2021_wake(void);
void fe_sim(rdof_fe_t *fe);
#if CONFIG_RDOF_SDR_LR2021
void fe_lr2021(rdof_fe_t *fe);
#endif

/* A block of samples arrived at t_us (esp_timer), the front end having sent `samples` since its stream started,
 * dropped ones included: timing its sample clock (sdr_task.c, rdof/sdr/rate.h). From the front end's own task. */
void sdr_rate_block(int64_t t_us, uint64_t samples);

/* The front end the settings name, or false (and why) if this build doesn't have it. */
bool sdr_fe_pick(int type, rdof_fe_t *fe, const char **why);
