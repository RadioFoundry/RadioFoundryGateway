// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The FPGA channelizer block on a Pluto (chzfx.h is its model): its registers, and the frames it writes to the RX
 * DMA. Portable C: the registers through two callbacks (a UIO mapping on the board, a fake in the host tests), the
 * frames from whatever buffers the DMA hands over.
 *
 * Use: rdof_chzhw_probe (starts the block's HLS control once, checks ID, reads CAPS), rdof_chzhw_start with a
 * rdof_chzfx_design configuration, rdof_chzhw_set_inc for the AFC or a retune, rdof_chzhw_stop on exit (the board
 * looks like a stock Pluto again). rdof_chzhw_rx_feed parses the DMA's words: frames may straddle buffers.
 */
#ifndef RDOF_SDR_CHZHW_H
#define RDOF_SDR_CHZHW_H

#include <stddef.h>
#include <stdint.h>

#include "rdof/sdr/chzfx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The registers (fixed in the block's source, the same in every build) */
#define RDOF_CHZHW_AP_CTRL 0x000
#define RDOF_CHZHW_CTRL 0x010
#define RDOF_CHZHW_DECIM 0x018
#define RDOF_CHZHW_TAPS 0x020
#define RDOF_CHZHW_CHANNELS 0x028
#define RDOF_CHZHW_OUT_SHIFT 0x030
#define RDOF_CHZHW_SNAP_PERIOD 0x038
#define RDOF_CHZHW_SNAP_LEN 0x040
#define RDOF_CHZHW_ID 0x048
#define RDOF_CHZHW_CAPS0 0x058
#define RDOF_CHZHW_CAPS1 0x068
#define RDOF_CHZHW_STATUS 0x078
#define RDOF_CHZHW_FRAMES 0x088
#define RDOF_CHZHW_INC 0x400
#define RDOF_CHZHW_PHASE0 0x800
#define RDOF_CHZHW_COEF 0x4000
#define RDOF_CHZHW_SPAN 0x8000 /* the register space */

#define RDOF_CHZHW_ID_RCH1 0x52434831u   /* "RCH1" */
#define RDOF_CHZHW_AP_RUN 0x81u          /* ap_start and auto-restart */
#define RDOF_CHZHW_CTRL_ENABLE 1u
#define RDOF_CHZHW_CTRL_APPLY 2u
#define RDOF_CHZHW_STATUS_RUNNING 1u
#define RDOF_CHZHW_STATUS_IN_OVF 2u      /* the input FIFO was full when a sample came */
#define RDOF_CHZHW_STATUS_DMA_OVF 4u     /* the DMA had no buffer for a word */

typedef struct {
    uint32_t (*rd)(void *ctx, uint32_t off);
    void (*wr)(void *ctx, uint32_t off, uint32_t v);
    void *ctx;
} rdof_chzhw_io_t;

typedef struct {
    rdof_chzhw_io_t io;
    int channels, lanes, max_decim, max_taps, hist; /* CAPS0, CAPS1 */
    uint32_t ctrl;                                  /* what CTRL holds */
    int nch;                                        /* channels started */
} rdof_chzhw_t;

/* Start the block's control if it isn't running, ENABLE off, check ID and read CAPS. 0, or -1 with why in err. */
int rdof_chzhw_probe(rdof_chzhw_t *h, const rdof_chzhw_io_t *io, char *err, size_t cap);
/* Configure and ENABLE (which resets the block: frame 0 next). RAW frames: snap_len in a row every snap_period
 * output instants, 0 for none. 0, or -1 with why in err (cfg past the block's or the model's sizes). */
int rdof_chzhw_start(rdof_chzhw_t *h, const rdof_chzfx_cfg_t *cfg, int snap_period, int snap_len, char *err,
                     size_t cap);
/* New steps for every started channel, taken after the next output instant (all at once: APPLY). */
void rdof_chzhw_set_inc(rdof_chzhw_t *h, const uint32_t *inc);
/* STATUS and FRAMES (output instants since ENABLE). */
uint32_t rdof_chzhw_status(const rdof_chzhw_t *h);
uint32_t rdof_chzhw_frames(const rdof_chzhw_t *h);
/* ENABLE off: the stock path to the DMA again. */
void rdof_chzhw_stop(rdof_chzhw_t *h);

/* The frame parser: what it's given goes to on_ch (a channel frame: one sample per channel) and on_raw. */
#define RDOF_CHZHW_RX_WORDS 64 /* the longest frame kept across buffers: 126 samples */

typedef struct {
    void (*on_ch)(void *ctx, uint32_t seq, const rdof_iq16_t *x, int n);
    void (*on_raw)(void *ctx, uint32_t seq, const rdof_iq16_t *x, int n);
    void *ctx;
    int channels;                         /* a channel frame of another size is junk */
    uint64_t part[RDOF_CHZHW_RX_WORDS];   /* a frame begun in the last buffer */
    size_t npart;
    uint32_t next_seq;                    /* the channel frame expected next */
    int have_seq;
    uint64_t ch_frames, raw_frames;
    uint64_t lost;                        /* output instants missing (gaps in the numbers) */
    uint64_t junk;                        /* words skipped to find a header */
} rdof_chzhw_rx_t;

void rdof_chzhw_rx_init(rdof_chzhw_rx_t *r, int channels,
                        void (*on_ch)(void *ctx, uint32_t seq, const rdof_iq16_t *x, int n),
                        void (*on_raw)(void *ctx, uint32_t seq, const rdof_iq16_t *x, int n), void *ctx);
void rdof_chzhw_rx_feed(rdof_chzhw_rx_t *r, const uint64_t *w, size_t n);

#ifdef __cplusplus
}
#endif

#endif
