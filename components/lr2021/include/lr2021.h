// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * A Semtech LR2021 on an ESP32's SPI bus: reset, the BUSY handshake, commands
 * and responses, the RX FIFO. Written from the datasheet (DS.LR20xx Rev. 2.2:
 * 3.7.2 SPI, 4.2 reset, 5.2 BUSY, 5.4 command frames); the encodings are in
 * lr2021_cmd.h. No gateway code: any ESP-IDF project can use it.
 *
 * Runs on the W12 (ROM 1.24) since 2026-10-21; README.md says what has been checked on air.
 */
#ifndef LR2021_H
#define LR2021_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "lr2021_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int spi_host;          /* SPI2_HOST or SPI3_HOST */
    int sck, mosi, miso, nss, busy, nreset;
    int irq;               /* the GPIO a DIO with IRQs drives, -1 for none */
    int clock_hz;          /* 16 MHz at most (3.7.2) */
    bool bus_initialized;  /* the app set up the bus already (shared with other devices) */
    void *mem;             /* the handle's memory, lr2021_handle_size() bytes of internal DMA-capable RAM aligned to
                            * 4, the caller's to keep until lr2021_close; NULL: lr2021_open allocates it */
} lr2021_config_t;

typedef struct lr2021 lr2021_t;

/* Bytes a handle takes (its 1 KB transmit and receive frames among them), for lr2021_config_t.mem. */
size_t lr2021_handle_size(void);

/* Set up the bus and pins, reset the chip and wait for it. *out is the handle. */
esp_err_t lr2021_open(const lr2021_config_t *cfg, lr2021_t **out);
void lr2021_close(lr2021_t *dev);

/* NRESET low 1 ms (100 us at least, 4.2), then wait for BUSY to drop (boot calibration). */
esp_err_t lr2021_reset(lr2021_t *dev);
/* Wake from SetSleep: NSS low for 120 us (a frame of zeros that long; the chip needs 100, DS 4.4.1) without waiting
 * for BUSY, which is high while it sleeps, then wait for BUSY to drop. For a sleeping chip only: an awake one takes
 * the zeros as a command. */
esp_err_t lr2021_wake(lr2021_t *dev);

/* A write command: frame (from an lr2021_cmd_* builder) out, BUSY waited for before and after. */
esp_err_t lr2021_write(lr2021_t *dev, const uint8_t *frame, size_t len);
/* A write command's frame alone: no GetStatus after it, so a FAIL isn't seen (the next frame's status bytes
 * hold it). For a hot path's commands that can't fail. */
esp_err_t lr2021_send(lr2021_t *dev, const uint8_t *frame, size_t len);
/* A read command: the frame, then a second frame for the answer: n bytes after the two status bytes. */
esp_err_t lr2021_read(lr2021_t *dev, const uint8_t *frame, size_t len, uint8_t *out, size_t n);
/* n bytes of the RX FIFO (a direct read, 5.4.1.3), 1024 at most. Frames of 32 bytes or more go by interrupt
 * (the SPI's DMA fills the buffer while the task sleeps); shorter ones poll. */
esp_err_t lr2021_read_fifo(lr2021_t *dev, uint8_t *out, size_t n);
/* The status of the last command (a GetStatus frame). */
esp_err_t lr2021_get_status(lr2021_t *dev, lr2021_status_t *st);

esp_err_t lr2021_version(lr2021_t *dev, uint8_t *major, uint8_t *minor);
esp_err_t lr2021_errors(lr2021_t *dev, uint16_t *errors);
esp_err_t lr2021_rx_fifo_level(lr2021_t *dev, uint16_t *level);
esp_err_t lr2021_fifo_flags(lr2021_t *dev, uint8_t *rx, uint8_t *tx); /* and clears them */

/* GetFifoIrqFlags without clearing them (the raw stream keeps a loss visible, docs/spec/lr2021-iq.md R2.14). */
esp_err_t lr2021_fifo_flags_peek(lr2021_t *dev, uint8_t *rx, uint8_t *tx);
esp_err_t lr2021_tx_fifo_level(lr2021_t *dev, uint16_t *level);
/* n bytes into the TX FIFO (a direct write, 5.4.1.3). */
esp_err_t lr2021_write_tx_fifo(lr2021_t *dev, const uint8_t *data, size_t n);
/* A 32-bit word of the chip's memory or registers (ReadRegMem32). */
esp_err_t lr2021_read32(lr2021_t *dev, uint32_t addr, uint32_t *value);
/* 1 to 64 words from addr on (ReadRegMem32 with a count). */
esp_err_t lr2021_read_mem(lr2021_t *dev, uint32_t addr, uint32_t *words, int n);
/* The pending IRQs (GetStatus's IrqStatus). */
esp_err_t lr2021_irq(lr2021_t *dev, uint32_t *irq);

/* Change the SPI clock, up to 80 MHz: past the chip's 16 MHz (3.7.2) is out of spec, for experiments. The
 * device is taken off the bus and added back; input_delay_ns is ESP-IDF's MISO timing allowance (0: none). */
esp_err_t lr2021_set_clock(lr2021_t *dev, int hz, int input_delay_ns);
int lr2021_clock(const lr2021_t *dev);

/* Wait for the IRQ pin (cfg.irq, a DIO set to IRQ) to be high: true at once if it is, false after timeout_ms
 * or with no IRQ pin. Clearing what raised it is the caller's (ClearIrq). */
bool lr2021_wait_irq(lr2021_t *dev, int timeout_ms);

/* ── Transmit (lr2021_tx.h sets a mode up; docs/spec/lr2021-iq.md section 3) ── */

/* Where lr2021_tx_stream gets its FIFO bytes: up to max into buf, the count back; 0 when there are no more. The
 * total must be a whole number of runs (run_bytes each): pad the last one. */
typedef size_t (*lr2021_tx_pull_t)(void *ctx, uint8_t *buf, size_t max);

typedef struct {
    const uint8_t *start;   /* lr2021_tx_start_frame's frame */
    size_t start_len;
    uint32_t run_bytes;     /* FIFO bytes a run takes: n x lr2021_tx_sample_bytes (FSK4: the payload). 0: a carrier,
                             * keyed up and left on (SetStandby ends it) */
    uint32_t bytes_per_s;   /* how fast the FIFO drains (rate x bytes a sample), to sleep a tick when it can; 0: poll */
    int max_ms;             /* give up after this (0: 10 s) */
    int chunk;              /* top the FIFO up once this many bytes are free (0: 256) */
    int poll_us;            /* the wait between polls with nothing to do (0: 100) */
    uint8_t *buf;           /* a buffer of at least 1020 bytes for the pull (NULL: allocated for the call) */
    size_t buf_len;
} lr2021_tx_stream_cfg_t;

typedef struct {
    int runs;               /* TX_DONEs seen */
    size_t sent;            /* bytes into the FIFO */
    unsigned refills;
    uint8_t tx_flags;       /* the TX FIFO flags at the end: 0x20, an underflow, means the waveform broke (R3.13) */
    bool early;             /* a run ended before taking its bytes and sent nothing (R3.17): send it again */
    bool timed_out;
    int64_t key_us, end_us; /* the first key-up and the last TX_DONE (esp_timer) */
} lr2021_tx_stream_stats_t;

/* Clear the TX FIFO, prefill it from pull, key up with cfg.start, keep the FIFO topped up, and on each TX_DONE send
 * cfg.start again until the data ends (back-to-back runs: a seam of 0.2-0.5 ms at -21 to -25 dB, R3.15). The mode
 * must be set up first (lr2021_tx_frames). Runs in the calling task, polling: give it a priority that keeps the
 * FIFO fed. ESP_ERR_INVALID_RESPONSE with stats.early for R3.17's early end; ESP_ERR_TIMEOUT past max_ms or on the
 * chip's own timeout. On an error the chip is put in standby if it's still in TX. */
esp_err_t lr2021_tx_stream(lr2021_t *dev, const lr2021_tx_stream_cfg_t *cfg, lr2021_tx_pull_t pull, void *ctx,
                           lr2021_tx_stream_stats_t *stats);

/* ── Packets with the chip's modems (src/lr2021_packet.c) ── */

typedef struct {
    uint32_t timeout;   /* SetTx / SetRx's timeout, 32.768 kHz periods (0: none; 0xFFFFFF: RX continuous, DS 7.2.4) */
    int max_ms;         /* the host gives up after this (0: 10 s) */
    size_t fifo_bytes;  /* the FIFO's size: 256 (0) or 1020 / 1000 after the 1 KB mapping (DS 5.3.3.1.1) */
    int poll_us;        /* the wait between polls: 0 sleeps a tick (or until the IRQ pin), fine while the FIFO
                         * outlasts a tick; for a payload longer than the FIFO at a fast rate, a few hundred us */
} lr2021_packet_cfg_t;

typedef struct {
    uint32_t irq;       /* every IRQ seen while it ran */
    size_t len;         /* bytes sent; received: the packet's length (GetRxPacketLength) */
    int64_t done_us;    /* from the start to TX_DONE / the end of RX */
    bool timed_out, crc_error;
} lr2021_packet_result_t;

/* One packet out: clear the TX FIFO and the IRQs, load the payload (feeding the rest as it drains when it's
 * longer than the FIFO), SetTx, wait for TX_DONE. The modem and its packet length are set up first. ESP_ERR_TIMEOUT
 * on the chip's timeout or past max_ms (the chip is put in standby). */
esp_err_t lr2021_packet_send(lr2021_t *dev, const uint8_t *data, size_t n, const lr2021_packet_cfg_t *cfg,
                             lr2021_packet_result_t *res);
/* One packet in: SetRx, wait for RX_DONE (draining the FIFO as it fills), then read it out. ESP_ERR_TIMEOUT for no
 * packet, ESP_ERR_INVALID_CRC for a bad CRC (the payload is still in buf), ESP_ERR_INVALID_RESPONSE for a header,
 * length or address error, ESP_ERR_INVALID_SIZE when it was longer than max (buf has its start). */
esp_err_t lr2021_packet_receive(lr2021_t *dev, uint8_t *buf, size_t max, const lr2021_packet_cfg_t *cfg,
                                lr2021_packet_result_t *res);

/* ── Semtech's patch RAM (lr2021_patch.h, src/lr2021_patch_load.c) ── */

#include "lr2021_patch.h"
/* Whether a patch runs, and its type and version. */
esp_err_t lr2021_patch_info(lr2021_t *dev, lr2021_patch_info_t *info);
/* Load a patch and run it: image NULL for Semtech's (lr2021_patch_image). In standby, after the chip's setup
 * (TCXO, calibration). ESP_ERR_INVALID_STATE if one runs already; ESP_ERR_INVALID_RESPONSE if it didn't start. The
 * raw I/Q paths are refused while it runs; a reset removes it. */
esp_err_t lr2021_patch_load(lr2021_t *dev, const uint32_t *image, size_t words);
/* Our patch image in place of Semtech's (pram/README.md): load, EnablePram 0, check the marker's type, empty the
 * override table, read the state. rep (may be NULL): that state. ESP_ERR_INVALID_STATE if a patch runs already;
 * ESP_ERR_INVALID_RESPONSE if ours didn't start or its state doesn't read back. */
esp_err_t lr2021_patch_load_rdof(lr2021_t *dev, lr2021_rdof_report_t *rep);
/* Our patch's state, read directly from bank 2 (ReadRegMem32), or through its report command. */
esp_err_t lr2021_rdof_state(lr2021_t *dev, lr2021_rdof_report_t *rep);
esp_err_t lr2021_rdof_report(lr2021_t *dev, lr2021_rdof_report_t *rep);
/* Replace the override table with n entries (0 to LR2021_RDOF_MAX_ENTRIES), applied in order; enable: apply it now
 * and from now on. */
esp_err_t lr2021_rdof_set(lr2021_t *dev, const lr2021_rdof_entry_t *entries, int n, bool enable);
esp_err_t lr2021_rdof_enable(lr2021_t *dev, bool on);
/* DMR 4-FSK reception with our patch running (lr2021_patch.h, spec R3.40), from standby with the frequency set:
 * SetPacketType FSK, the 4-FSK variant (before the modulation: the setter reads it), 9600 b/s, Gaussian BT 0.5,
 * rx_bw 0x1E, fdev 1944 Hz, fixed LR2021_DMR_RX_LEN-byte packets with no preamble detector, CRC or whitening, the
 * family's data sync word, 0x022F LR2021_DMR_RX_DEMOD_OPT, then the override table, enabled. Then SetRx; each burst of the family gives RX_DONE.
 * ESP_ERR_INVALID_STATE if our patch isn't running; ESP_ERR_INVALID_ARG for a bad family. */
esp_err_t lr2021_fsk4_dmr_rx(lr2021_t *dev, int family);

/* What failed last, for logs. */
const char *lr2021_last_error(const lr2021_t *dev);
/* Commands that came back FAIL or PERR, transfers that timed out on BUSY. */
uint32_t lr2021_failures(const lr2021_t *dev);
/* Transfers whose answer wasn't a multiple of 4 bytes, so ESP-IDF's SPI driver allocated an internal DMA buffer
 * for it (the IRQ word, a register read, a FIFO read of an odd length). Commands send without one: a frame
 * receives only the status bytes it needs, rounded up to 4, or nothing. */
uint32_t lr2021_bounces(const lr2021_t *dev);

#ifdef __cplusplus
}
#endif

#endif
