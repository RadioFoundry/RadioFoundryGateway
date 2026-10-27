// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Raw I/Q sessions and transmissions (lr2021_iq.h). Every step cites docs/spec/lr2021-iq.md.
 */
#include "lr2021_iq.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "lr2021_radio.h"

/* The FIFO level that raises the RX FIFO IRQ on the DIO. LORA (125 kS/s int16, 500 kB/s): 1.5 ms in, the rest fills
 * in 0.5. CONV8 fills the FIFO in 0.77 ms at 576 k: half of it. FSK16 at 250 k fills it in 1 ms: at 512 bytes a
 * reader had too little time to start and lost samples about 25 times a second, so a quarter. */
#define IRQ_LEVEL_LORA 768
#define IRQ_LEVEL_CONV8 512
#define IRQ_LEVEL_FSK16 256
/* By the level: wait for about 640 bytes and read them in one frame. Each read costs about 90 us besides its bytes
 * (a level query, BUSY, the DMA's set-up): reading whatever had come in, 300-400 bytes at a time, kept the SPI busy
 * 93% of the time at 576 k (R2.29, R2.34) and the reader on the CPU half of it. Between real level queries (every
 * RECKON_READS reads) the level is reckoned from the rate, less a margin: a read past the level would underflow
 * (R2.14). Waiting only for an empty FIFO never waited, and a reader starved its core. */
#define READ_TARGET 640
#define READ_MIN 256 /* cfg.slack_us's floor */
#define RECKON_READS 4
#define RECKON_MARGIN 64
#define LOSS (LR2021_FIFO_OVERFLOW | LR2021_FIFO_UNDERFLOW) /* R2.14 */

static lr2021_err_t failed(lr2021_iq_t *iq, const char *what, lr2021_err_t e)
{
    const char *why = iq->dev && *lr2021_last_error(iq->dev) ? lr2021_last_error(iq->dev) : lr2021_err_name(e);
    snprintf(iq->error, sizeof iq->error, "%s: %s", what, why);
    return e;
}

static lr2021_err_t cmd(lr2021_iq_t *iq, const uint8_t *f, size_t n) { return lr2021_write(iq->dev, f, n); }

/* ── The chip ── */

/* After power-up or a reset: standby, the clock and regulator, calibration (DS 6.4). */
static lr2021_err_t chip_setup(lr2021_iq_t *iq)
{
    uint8_t f[12];
    lr2021_err_t e;
    if ((e = cmd(iq, f, lr2021_cmd_u8(f, LR2021_SET_STANDBY, 0))) != LR2021_OK)
        return failed(iq, "SetStandby", e);
    cmd(iq, f, lr2021_cmd(f, LR2021_CLEAR_ERRORS));
    if (iq->board->tcxo_tune >= 0 &&
        (e = cmd(iq, f, lr2021_cmd_set_tcxo(f, (uint8_t)iq->board->tcxo_tune,
                                            (iq->board->tcxo_start_us ? iq->board->tcxo_start_us : 5000) * 32))) !=
            LR2021_OK)
        return failed(iq, "SetTcxoMode", e);
    if ((e = cmd(iq, f, lr2021_cmd_u8(f, LR2021_SET_REG_MODE, 0))) != LR2021_OK) /* LDO */
        return failed(iq, "SetRegMode", e);
    if ((e = cmd(iq, f,
                 lr2021_cmd_u8(f, LR2021_CALIBRATE,
                               LR2021_CAL_LF_RC | LR2021_CAL_HF_RC | LR2021_CAL_PLL | LR2021_CAL_AAF | LR2021_CAL_MU |
                                   LR2021_CAL_PA_OFF))) != LR2021_OK)
        return failed(iq, "Calibrate", e);
    uint16_t errs = 0;
    lr2021_errors(iq->dev, &errs);
    if (errs) {
        snprintf(iq->error, sizeof iq->error, "after calibration: errors 0x%04x (%s)", errs, lr2021_error_name(errs));
        return LR2021_FAIL;
    }
    return LR2021_OK;
}

lr2021_err_t lr2021_iq_init(lr2021_iq_t *iq, lr2021_t *dev, const lr2021_board_t *board, bool allow_untested)
{
    memset(iq, 0, sizeof *iq);
    iq->dev = dev;
    iq->board = board ? board : &lr2021_board_generic;
    lr2021_err_t e = lr2021_version(dev, &iq->rom_major, &iq->rom_minor);
    if (e != LR2021_OK)
        return failed(iq, "GetVersion", e);
    iq->rom = lr2021_rom_find(iq->rom_major, iq->rom_minor);
    if ((!iq->rom || !iq->rom->iq_rx) && !allow_untested) {
        snprintf(iq->error, sizeof iq->error,
                 "ROM %u.%u: raw I/Q is untested on it (known: 1.24); allow_untested to try", iq->rom_major,
                 iq->rom_minor);
        return LR2021_ERR_NOT_SUPPORTED;
    }
    /* R1.2: the raw paths with a patch running are refused */
    uint32_t marker = 0;
    if ((e = lr2021_read32(dev, LR2021_PATCH_MARKER_ADDR, &marker)) != LR2021_OK)
        return failed(iq, "ReadRegMem32", e);
    if (marker == LR2021_PATCH_ACTIVE) {
        snprintf(iq->error, sizeof iq->error, "a Semtech patch is running; raw I/Q needs the plain ROM (reset it)");
        return LR2021_ERR_INVALID_STATE;
    }
    return chip_setup(iq);
}

/* The RF switch for the frequency (the PA band or the bypass), and the RX path: HF above 1.2 GHz (untried for raw
 * I/Q; the findings are all on the LF path). */
static void rf_setup(lr2021_iq_t *iq, double hz)
{
    uint8_t dio[LR2021_BOARD_RFSW_MAX], modes[LR2021_BOARD_RFSW_MAX], f[12];
    int n = lr2021_board_rfsw(iq->board, hz, dio, modes);
    for (int i = 0; i < n; i++) {
        cmd(iq, f, lr2021_cmd_set_dio_function(f, dio[i], LR2021_DIO_RF_SWITCH, 0));
        cmd(iq, f, lr2021_cmd_set_dio_rf_switch(f, dio[i], modes[i]));
    }
}

/* The board's RF switch table; hold: during a transmission, the TX pattern in standby and FS too, so an external
 * front end stays on between runs (the W12's GC1109, shut down between runs, came back 22-28 dB low for 5-13 ms:
 * R3.44) */
static void rf_hold_tx(lr2021_iq_t *iq, double hz, bool hold)
{
    uint8_t dio[LR2021_BOARD_RFSW_MAX], modes[LR2021_BOARD_RFSW_MAX], f[12];
    int n = lr2021_board_rfsw(iq->board, hz, dio, modes);
    for (int i = 0; i < n; i++)
        cmd(iq, f, lr2021_cmd_set_dio_rf_switch(f, dio[i],
                                                (uint8_t)(modes[i] | (hold && (modes[i] & LR2021_RFSW_TX_LF)
                                                                          ? LR2021_RFSW_STANDBY
                                                                          : 0))));
}

static bool hf(double hz) { return hz > 1.2e9; }

/* The receiver the route sits in (2.1; R2.19 for FSK), at the center. calib: CalibFE there first. */
static lr2021_err_t modem_setup(lr2021_iq_t *iq, bool calib)
{
    uint8_t f[16];
    lr2021_err_t e;
    const double hz = iq->cfg.center_hz;
    const uint32_t chip_hz = lr2021_freq_corrected(hz, iq->cfg.ppm), cal_hz = (uint32_t)lrint(hz);
    const bool lora = iq->plan.mode == LR2021_IQ_LORA;
    rf_setup(iq, hz);
    if ((e = cmd(iq, f, lr2021_cmd_u8(f, LR2021_SET_STANDBY, 0))) != LR2021_OK ||
        (e = cmd(iq, f, lr2021_cmd_u8(f, LR2021_SET_PACKET_TYPE, lora ? LR2021_PACKET_LORA : LR2021_PACKET_FSK))) !=
            LR2021_OK)
        return failed(iq, "SetPacketType", e);
    if (calib) {
        if ((e = cmd(iq, f, lr2021_cmd_calib_fe(f, &cal_hz, 1, hf(hz)))) != LR2021_OK) {
            iq->cal_word = 0; /* the last one's may be gone too */
            return failed(iq, "CalibFE", e);
        }
        iq->cal_word = lr2021_calib_fe_word(cal_hz, hf(hz));
    }
    if ((e = cmd(iq, f, lr2021_cmd_set_rf_frequency(f, chip_hz))) != LR2021_OK)
        return failed(iq, "SetRfFrequency", e);
    if ((e = cmd(iq, f, lr2021_cmd_set_rx_path(f, hf(hz), iq->board->rx_boost))) != LR2021_OK)
        return failed(iq, "SetRxPath", e);
    if (lora) {
        /* SF7, CR 4/5, a preamble of 8, 255 bytes, sync 0x12: they don't matter to the samples (R2.2) */
        if ((e = cmd(iq, f, lr2021_cmd_lora_modulation(f, 7, iq->plan.lora_bw, 1, 0))) != LR2021_OK ||
            (e = cmd(iq, f, lr2021_cmd_lora_packet(f, 8, 255, 0x02))) != LR2021_OK ||
            (e = cmd(iq, f, lr2021_cmd_u8(f, LR2021_SET_LORA_SYNCWORD, 0x12))) != LR2021_OK)
            return failed(iq, "LoRa setup", e);
    } else {
        /* rx_bw sets the rate and the filter (R2.19); bit rate and deviation don't matter, set well inside it */
        uint32_t num, den;
        lr2021_fsk_rx_rate(iq->plan.rx_bw, &num, &den);
        const uint32_t bitrate = (uint32_t)((double)num / den * lr2021_fsk_filter_share(iq->plan.rx_bw) / 4);
        if ((e = cmd(iq, f, lr2021_cmd_fsk_modulation(f, bitrate, 0, iq->plan.rx_bw, bitrate / 2))) != LR2021_OK)
            return failed(iq, "SetFskModulationParams", e);
    }
    uint16_t errs = 0;
    if ((e = lr2021_errors(iq->dev, &errs)) != LR2021_OK || errs) { /* R2.3 */
        snprintf(iq->error, sizeof iq->error, "setup at %.5f MHz: %s", hz / 1e6,
                 errs ? lr2021_error_name(errs) : lr2021_err_name(e));
        return e != LR2021_OK ? e : LR2021_FAIL;
    }
    return LR2021_OK;
}

static bool fifo_mapped(lr2021_iq_t *iq)
{
    uint32_t base = 0, size = 0;
    if (lr2021_read32(iq->dev, LR2021_REG_RX_FIFO_BASE, &base) != LR2021_OK ||
        lr2021_read32(iq->dev, LR2021_REG_RX_FIFO_SIZE, &size) != LR2021_OK)
        return false;
    /* ROM 1.24 reads the base back without its alias bit (2.2 step 4) */
    return (base == LR2021_RX_FIFO_BASE_1K || base == (LR2021_RX_FIFO_BASE_1K & 0xFFFF)) &&
           size == LR2021_RX_FIFO_SIZE_1K;
}

static size_t irq_level(const lr2021_iq_t *iq)
{
    return iq->plan.mode == LR2021_IQ_LORA ? IRQ_LEVEL_LORA
           : iq->plan.mode == LR2021_IQ_CONV8 ? IRQ_LEVEL_CONV8
                                              : IRQ_LEVEL_FSK16;
}

static bool use_irq(const lr2021_iq_t *iq) { return iq->board->irq_dio >= 0 && lr2021_has_irq(iq->dev); }

/* A session (2.2). quick: back from a transmission, the chip known and calibrated here (R3.16): no checks, no
 * CalibFE. */
static lr2021_err_t session_start(lr2021_iq_t *iq, bool quick)
{
    uint8_t f[16];
    lr2021_err_t e;
    if ((e = modem_setup(iq, !quick)) != LR2021_OK)
        return e;
    cmd(iq, f, lr2021_cmd_u8(f, LR2021_SET_STANDBY, 0));
    /* CONV8 through the converter (R2.21); FSK16 at the modem's rate; LORA the route SDR found (2.2) */
    if ((e = cmd(iq, f,
                 iq->plan.mode == LR2021_IQ_CONV8
                     ? lr2021_cmd_raw_route_rate(f, iq->plan.rx_bw, iq->plan.rate_num, LR2021_RAW_SRC_CONV)
                     : lr2021_cmd_raw_route_fifo(f))) != LR2021_OK)
        return failed(iq, "raw route", e);
    cmd(iq, f, lr2021_cmd_write_reg32(f, LR2021_REG_RX_FIFO_BASE, LR2021_RX_FIFO_BASE_1K));
    cmd(iq, f, lr2021_cmd_write_reg32(f, LR2021_REG_RX_FIFO_SIZE, LR2021_RX_FIFO_SIZE_1K));
    if (!quick && !fifo_mapped(iq)) {
        snprintf(iq->error, sizeof iq->error, "the 1 KB RX FIFO didn't map");
        return LR2021_ERR_INVALID_RESPONSE;
    }
    cmd(iq, f, lr2021_cmd_clear_fifo_flags(f, 0x3F, 0x3F));
    if (use_irq(iq)) {
        /* the RX FIFO IRQ on the DIO at a level (ConfigFifoIrq, DS 6.10.1) */
        cmd(iq, f, lr2021_cmd_set_dio_function(f, (uint8_t)iq->board->irq_dio, LR2021_DIO_IRQ, 0));
        cmd(iq, f, lr2021_cmd_set_dio_irq(f, (uint8_t)iq->board->irq_dio, LR2021_IRQ_RX_FIFO));
        cmd(iq, f, lr2021_cmd_config_fifo_irq(f, LR2021_FIFO_HIGH, 0, (uint16_t)irq_level(iq), 0, 0, 0));
        cmd(iq, f, lr2021_cmd_clear_irq(f, 0xFFFFFFFF));
    }
    if ((e = cmd(iq, f, lr2021_cmd_set_rx(f, 0xFFFFFF))) != LR2021_OK) /* continuous (DS 6.3.5) */
        return failed(iq, "SetRx", e);
    /* the gain after SetRx: SetRx sets step 13 whatever came before (R2.25) */
    if ((e = cmd(iq, f, lr2021_cmd_set_gain(f, (uint8_t)iq->cfg.gain))) != LR2021_OK)
        return failed(iq, "SetAgcGainManual", e);
    if (!quick && !fifo_mapped(iq)) {
        snprintf(iq->error, sizeof iq->error, "the RX FIFO moved when RX started");
        return LR2021_ERR_INVALID_RESPONSE;
    }
    cmd(iq, f, lr2021_cmd(f, LR2021_CLEAR_RX_FIFO));
    cmd(iq, f, lr2021_cmd_clear_fifo_flags(f, 0x3F, 0x3F));
    iq->carry_n = 0;
    iq->since_flags = 0;
    iq->by_level = true; /* what's in the FIFO first, then the IRQ */
    iq->reckon_n = 0;
    iq->in_session = true;
    return LR2021_OK;
}

/* End a session: standby, reset, the chip set up again (R2.16). */
static void session_end(lr2021_iq_t *iq)
{
    uint8_t f[4];
    if (!iq->in_session)
        return;
    iq->in_session = false;
    iq->cal_word = 0;
    cmd(iq, f, lr2021_cmd_u8(f, LR2021_SET_STANDBY, 0));
    lr2021_reset(iq->dev);
    chip_setup(iq);
}

lr2021_err_t lr2021_iq_start(lr2021_iq_t *iq, const lr2021_iq_cfg_t *cfg)
{
    lr2021_iq_plan_t plan;
    lr2021_err_t e = lr2021_iq_plan(cfg->rate, cfg->mode, &plan);
    if (e != LR2021_OK) {
        snprintf(iq->error, sizeof iq->error, "no mode gives %.0f S/s", cfg->rate);
        return e;
    }
    if (cfg->gain < 0 || cfg->gain > 13) {
        snprintf(iq->error, sizeof iq->error, "gain step %d (0 to 13)", cfg->gain);
        return LR2021_ERR_INVALID_ARG;
    }
    session_end(iq);
    iq->cfg = *cfg;
    iq->plan = plan;
    iq->have_cfg = true;
    iq->gap = false;
    return session_start(iq, false);
}

lr2021_err_t lr2021_iq_set_gain(lr2021_iq_t *iq, int gain)
{
    if (gain < 0 || gain > 13)
        return LR2021_ERR_INVALID_ARG;
    iq->cfg.gain = gain;
    if (!iq->in_session)
        return LR2021_OK;
    uint8_t f[4];
    lr2021_err_t e = cmd(iq, f, lr2021_cmd_set_gain(f, (uint8_t)gain));
    return e == LR2021_OK ? LR2021_OK : failed(iq, "SetAgcGainManual", e);
}

void lr2021_iq_stop(lr2021_iq_t *iq) { session_end(iq); }

/* ── Reading ── */

/* Lost samples: in the LORA mode a new session (R2.15); in the FSK modes empty the FIFO and go on (R2.28: a new
 * session takes about 30 ms, and at a wide rate the stream overflowed again right after it). */
static lr2021_err_t lost(lr2021_iq_t *iq)
{
    iq->stats.losses++;
    iq->gap = true;
    iq->reckon_n = 0;
    if (iq->plan.mode != LR2021_IQ_LORA) {
        uint8_t f[6];
        cmd(iq, f, lr2021_cmd(f, LR2021_CLEAR_RX_FIFO));
        cmd(iq, f, lr2021_cmd_clear_fifo_flags(f, 0x3F, 0x3F));
        iq->carry_n = 0;
        return LR2021_OK;
    }
    iq->stats.restarts++;
    session_end(iq);
    return session_start(iq, false);
}

/* Re-arm the IRQ: clear FIFO-high (the loss flags stay for the peek), then the IRQ it raised (the flag alone
 * leaves it, DS 6.10.3). Two frames, no status read: they can't fail. */
static void rearm(lr2021_iq_t *iq)
{
    uint8_t f[6];
    lr2021_send(iq->dev, f, lr2021_cmd_clear_fifo_flags(f, LR2021_FIFO_HIGH, 0));
    lr2021_send(iq->dev, f, lr2021_cmd_clear_irq(f, LR2021_IRQ_RX_FIFO));
}

/* n bytes of the FIFO (at most 1 KB) in one read, whole samples to the sink. Each read is n - 2 bytes (n a multiple
 * of 4), so its frame, 2 status bytes and the samples, is a multiple of 4: an ESP32's SPI DMA needs no buffer of its
 * own for it (R2.32). A read may end mid-sample: its part waits in buf for the next read (R2.30). The FIFO flags
 * are read every 8 KB (1 KB without the IRQ): a loss shows there (R2.14). Returns the samples delivered, or -1 if
 * the read failed or samples were lost. */
static int drain(lr2021_iq_t *iq, size_t n, bool then_rearm, lr2021_iq_sink_t sink, void *ctx)
{
    if (n >= 4 && !(n & 3))
        n -= 2;
    const size_t c = iq->carry_n;
    iq->stats.reads++;
    lr2021_err_t e = lr2021_read_fifo(iq->dev, iq->buf + c, n);
    if (e != LR2021_OK) {
        iq->stats.errors++;
        iq->reckon_n = 0;
        return -1;
    }
    if (iq->reckon_n) {
        iq->reckon_n++;
        iq->anchor_level -= (int32_t)n;
    }
    const int64_t t = lr2021_now_us();
    uint8_t rx = 0, tx = 0;
    bool checked = false;
    if ((iq->since_flags += (uint32_t)n) >= (use_irq(iq) ? 8192u : 1024u)) {
        iq->since_flags = 0;
        checked = lr2021_fifo_flags_peek(iq->dev, &rx, &tx) == LR2021_OK;
    }
    if (use_irq(iq) && then_rearm)
        rearm(iq);
    if (checked && (rx & LOSS)) {
        lost(iq);
        return -1;
    }
    const size_t have = c + n, sb = (size_t)iq->plan.sample_bytes, whole = have - have % sb;
    if (whole) {
        sink(ctx, iq->buf, whole / sb, iq->gap ? LR2021_IQ_GAP : 0, t);
        iq->gap = false;
        iq->stats.samples += whole / sb;
    }
    iq->carry_n = (uint8_t)(have - whole);
    if (iq->carry_n)
        memmove(iq->buf, iq->buf + whole, iq->carry_n);
    return (int)(whole / sb);
}

int lr2021_iq_poll(lr2021_iq_t *iq, lr2021_iq_sink_t sink, void *ctx, int wait_ms)
{
    if (!iq->have_cfg)
        return LR2021_ERR_INVALID_STATE;
    if (!iq->in_session) {
        lr2021_err_t e = session_start(iq, false);
        if (e != LR2021_OK)
            return e;
    }
    if (use_irq(iq) && !iq->by_level) {
        /* 2 ms at most (the rest of the FIFO fills in about 0.5-1): past that, read by the level */
        if (!lr2021_wait_irq(iq->dev, wait_ms < 2 ? wait_ms : 2)) {
            iq->stats.irq_late++;
            iq->by_level = true;
            return 0;
        }
        const bool conv = iq->plan.mode == LR2021_IQ_CONV8;
        int got = drain(iq, irq_level(iq), !conv, sink, ctx);
        /* the FSK modes: what came in during the read can pass the level before the re-arm, and the FIFO-high flag
         * then never rises again (no new edge): read by the level until it's nearly empty */
        if (got < 0 || iq->plan.mode != LR2021_IQ_LORA)
            iq->by_level = true;
        return got < 0 ? 0 : got;
    }
    const double bpus = iq->plan.rate * iq->plan.sample_bytes / 1e6; /* FIFO bytes a microsecond */
    size_t backlog;
    bool reckon = iq->reckon_n && iq->reckon_n <= RECKON_READS;
    double est = 0;
    if (reckon) {
        est = iq->anchor_level + (double)(lr2021_now_us() - iq->anchor_us) * bpus - RECKON_MARGIN;
        /* only while it can't have filled: a late poll asks, so an overflow is caught before the read (R2.13) */
        reckon = est < LR2021_IQ_FIFO_BYTES - 3 * RECKON_MARGIN;
    }
    if (reckon) {
        backlog = est > 0 ? (size_t)est & ~3u : 0;
    } else {
        uint16_t level = 0;
        lr2021_err_t e = lr2021_rx_fifo_level(iq->dev, &level);
        if (e != LR2021_OK) {
            iq->stats.errors++;
            iq->reckon_n = 0;
            return 0;
        }
        if (level >= LR2021_IQ_FIFO_BYTES) { /* it filled: samples are gone (R2.13) */
            iq->reckon_n = 0;
            e = lost(iq);
            return e == LR2021_OK ? 0 : e;
        }
        /* the level as of the query's end: the latest it can be from, so the reckoning never runs ahead of it */
        iq->anchor_us = lr2021_now_us();
        iq->anchor_level = level;
        iq->reckon_n = 1;
        backlog = level & ~3u;
        /* LORA with the IRQ: back to waiting for it once the FIFO is well under its level, so the level rising
         * past it raises it (the SPI outruns the samples: each read leaves less behind) */
        if (use_irq(iq) && iq->plan.mode == LR2021_IQ_LORA && backlog < irq_level(iq) / 2) {
            if (backlog)
                drain(iq, backlog, false, sink, ctx);
            rearm(iq);
            iq->by_level = false;
            iq->reckon_n = 0;
            return 0;
        }
    }
    size_t target = READ_TARGET;
    if (iq->cfg.slack_us > 0) {
        const double t = LR2021_IQ_FIFO_BYTES - RECKON_MARGIN - bpus * iq->cfg.slack_us;
        target = t < READ_MIN ? READ_MIN : t < READ_TARGET ? (size_t)t & ~3u : READ_TARGET;
    }
    if (backlog < target) {
        /* the time the FIFO takes to reach the target, at most wait_ms */
        double us = (target - backlog) / bpus;
        if (us > wait_ms * 1000.0)
            us = wait_ms * 1000.0;
        if (us > 0)
            lr2021_sleep_us((uint32_t)us);
        return 0;
    }
    if (backlog > LR2021_IQ_FIFO_BYTES)
        backlog = LR2021_IQ_FIFO_BYTES;
    int got = drain(iq, backlog, false, sink, ctx);
    return got < 0 ? 0 : got;
}

/* ── Transmit ── */

typedef struct {
    lr2021_tx_pull_t pull;
    void *ctx;
    size_t run_bytes, sent;
    bool ended;
} pad_t;

/* The caller's pull, with the last run filled out with zeros. */
static size_t pad_pull(void *ctx, uint8_t *buf, size_t max)
{
    pad_t *p = ctx;
    size_t n = p->ended ? 0 : p->pull(p->ctx, buf, max);
    if (n == LR2021_TX_PULL_WAIT)
        return n;
    if (!n && p->run_bytes) {
        p->ended = true;
        size_t rest = p->sent % p->run_bytes ? p->run_bytes - p->sent % p->run_bytes : 0;
        n = rest < max ? rest : max;
        memset(buf, 0, n);
    }
    p->sent += n;
    return n;
}

bool lr2021_iq_tx_calibrates(const lr2021_iq_t *iq, const lr2021_iq_tx_t *tx)
{
    const double hz = tx->center_hz > 0 ? tx->center_hz : iq->cfg.center_hz;
    return iq->cal_word != lr2021_calib_fe_word((uint32_t)lrint(hz), hf(hz));
}

lr2021_err_t lr2021_iq_transmit(lr2021_iq_t *iq, const lr2021_iq_tx_t *tx, lr2021_tx_pull_t pull, void *ctx,
                                lr2021_tx_stream_stats_t *stats)
{
    static uint8_t frames[LR2021_TX_FRAMES_MAX][LR2021_TX_FRAME_MAX];
    size_t lens[LR2021_TX_FRAMES_MAX];
    uint8_t f[16], start[16];
    lr2021_tx_stream_stats_t local;
    if (!stats)
        stats = &local;
    memset(stats, 0, sizeof *stats);
    const bool receiving = iq->in_session;
    const double hz = tx->center_hz > 0 ? tx->center_hz : iq->cfg.center_hz;
    const double ppm = tx->center_hz > 0 ? tx->ppm : iq->cfg.ppm;
    if (!(hz > 0) || (tx->mode != LR2021_TX_CW && !tx->rate)) {
        snprintf(iq->error, sizeof iq->error, "transmit: no frequency or rate");
        return LR2021_ERR_INVALID_ARG;
    }
    if (iq->rom && !iq->rom->iq_tx && tx->mode != LR2021_TX_CW) {
        snprintf(iq->error, sizeof iq->error, "transmit: untested on ROM %u.%u", iq->rom_major, iq->rom_minor);
        return LR2021_ERR_NOT_SUPPORTED;
    }
    /* Runs of equal length up to the 16-bit counts (R3.12, R3.43), so the zeros that fill the last are fewer than the
     * runs */
    const int sb = lr2021_tx_sample_bytes(tx->mode);
    const uint32_t max_run = lr2021_tx_max_run(tx->mode);
    uint32_t run = max_run;
    if (tx->samples && tx->mode != LR2021_TX_CW) {
        uint64_t runs = (tx->samples + max_run - 1) / max_run;
        run = (uint32_t)((tx->samples + runs - 1) / runs);
    }
    const uint32_t cal_hz = (uint32_t)lrint(hz);
    const lr2021_tx_setup_t s = {
        .mode = tx->mode,
        .freq_hz = lr2021_freq_corrected(hz, ppm),
        /* R3.16: no CalibFE where the chip was last calibrated at the same cell (a receive session, or a
         * transmission before) and not reset since */
        .calib_fe = iq->cal_word != lr2021_calib_fe_word(cal_hz, hf(hz)),
        .standby_first = true,
        .power_code = lr2021_tx_power_code(tx->power_dbm),
        .ramp_code = 4,
        .pa_lf_duty = iq->board->pa_lf_duty,
        .pa_lf_slices = iq->board->pa_lf_slices,
        .fallback = LR2021_FALLBACK_FS, /* between runs: the synthesizer stays up for the next SetTx */
        .rate = tx->rate,
        .fdev_hz = tx->fdev_hz,
        .rx_bw = 0x1C,
        .n = tx->mode == LR2021_TX_CW ? 0 : (uint16_t)run,
    };
    const int nf = lr2021_tx_frames(&s, frames, lens);
    if (nf <= 0) {
        snprintf(iq->error, sizeof iq->error, "transmit: a setup the chip can't take");
        return LR2021_ERR_INVALID_ARG;
    }
    rf_setup(iq, hz);
    lr2021_err_t e = LR2021_OK;
    for (int i = 0; i < nf && e == LR2021_OK; i++)
        e = cmd(iq, frames[i], lens[i]);
    if (s.calib_fe)
        iq->cal_word = e == LR2021_OK ? lr2021_calib_fe_word(cal_hz, hf(hz)) : 0;
    if (e == LR2021_OK)
        e = cmd(iq, f, lr2021_cmd_clear_fifo_flags(f, 0, 0x3F));
    if (e == LR2021_OK)
        e = cmd(iq, f, lr2021_cmd_clear_irq(f, LR2021_IRQ_TX_DONE | LR2021_IRQ_TIMEOUT));
    /* the IRQ pin on TX_DONE and the timeout alone, for the restarts (R3.45; a receive session sets it back) */
    const bool pin = use_irq(iq) && tx->mode != LR2021_TX_CW;
    if (e == LR2021_OK && pin) {
        cmd(iq, f, lr2021_cmd_set_dio_function(f, (uint8_t)iq->board->irq_dio, LR2021_DIO_IRQ, 0));
        e = cmd(iq, f, lr2021_cmd_set_dio_irq(f, (uint8_t)iq->board->irq_dio, LR2021_IRQ_TX_DONE | LR2021_IRQ_TIMEOUT));
    }
    if (e != LR2021_OK) {
        failed(iq, "transmit setup", e);
    } else {
        /* SetTx's timeout past a run's end (1/32 ms), so a run that never ends can't hold the chip */
        const uint32_t timeout =
            tx->mode == LR2021_TX_CW ? 0 : (uint32_t)((uint64_t)run * 32000 / tx->rate + 32 * 50);
        const int64_t total_ms = tx->samples && tx->rate ? (int64_t)(tx->samples * 1000 / tx->rate) : 0;
        pad_t pad = {pull, ctx, tx->streamed ? 0 : (size_t)run * (size_t)sb, 0, false};
        lr2021_tx_stream_cfg_t c = {
            .start = start,
            .start_len = lr2021_tx_start_frame(&s, start, timeout),
            .run_bytes = tx->mode == LR2021_TX_CW ? 0 : (uint32_t)run * (uint32_t)sb,
            .bytes_per_s = tx->rate * (uint32_t)sb,
            .max_ms = tx->max_ms > 0     ? tx->max_ms
                      : tx->streamed     ? INT_MAX
                      : total_ms         ? (int)(total_ms + 2000)
                                         : 10000,
            .irq_pin = pin,
            .cut_end = tx->streamed && tx->mode != LR2021_TX_CW,
            .stop = tx->stop,
            .learned = iq->tx_learned,
            .at_us = tx->at_us,
        };
        const uint32_t key = (uint32_t)tx->mode << 24 ^ tx->rate;
        if (iq->tx_learned_for != key) {
            iq->tx_learned[0] = iq->tx_learned[1] = iq->tx_learned[2] = -1;
            iq->tx_learned_for = key;
        }
        rf_hold_tx(iq, hz, true);
        e = lr2021_tx_stream(iq->dev, &c, pad_pull, &pad, stats);
        rf_hold_tx(iq, hz, false);
        if (e != LR2021_OK)
            failed(iq, "transmit", e);
    }
    cmd(iq, f, lr2021_cmd_u8(f, LR2021_SET_STANDBY, 0));
    /* back to receiving: the same session's setup without its checks (R3.16); a gap in the stream */
    if (receiving) {
        iq->gap = true;
        iq->in_session = false;
        lr2021_err_t r = session_start(iq, !s.calib_fe); /* calibrated elsewhere: again at the center */
        if (r != LR2021_OK && e == LR2021_OK)
            e = r;
    }
    return e;
}
