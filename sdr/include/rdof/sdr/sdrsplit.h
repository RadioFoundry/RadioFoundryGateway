// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Splits on one receiver: the SDR as the RX half of up to RDOF_SDRSPLITS_MAX split duplex repeaters
 * (docs/spec/repeater.md), each with its own TX half, a radio with the split duplex extension on a port. One
 * receiver hears every repeater's input at once; each radio transmits one repeater's output.
 *
 * Each split has an SDR modem of its own, receive only (rdof_sdrmodem_t with no tx_tune), and an rdof_split_t joining
 * it to its radio. The repeater's SET_FREQ picks the modem's receiver channel, so every heard burst goes to every
 * modem (rdof_sdrsplits_heard) and each keeps its own channel's. The modems share the receiver's sample clock (now48).
 *
 * No I/O, no threads: the platform writes what each split has for its radio (to_radio) and its repeater (to_host),
 * feeds back what they send, and ticks it every ~20 ms. One lock around every call. rdofradio and the board run it
 * the same way.
 */
#ifndef RDOF_SDR_SDRSPLIT_H
#define RDOF_SDR_SDRSPLIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/features.h"
#include "rdof/sdr/sdrmodem.h"
#include "rdof/split.h"

#ifdef __cplusplus
extern "C" {
#endif

#if RDOF_FEATURE_REPEATER

#define RDOF_SDRSPLITS_MAX 8

typedef struct {
    /* Bytes for split i's radio (its TX half) and for split i's repeater (the rdof_modem over it). */
    void (*to_radio)(void *ctx, int i, const uint8_t *d, size_t n);
    void (*to_host)(void *ctx, int i, const uint8_t *d, size_t n);
    /* The receiver channel for an RX frequency, -1 none. */
    int (*rx_channel)(void *ctx, uint32_t hz);
    /* The receiver's 48 kS/s sample position now (the splits' TIME), as rdof_sdrmodem_io_t's. NULL: the latest heard. */
    int64_t (*now48)(void *ctx);
    void *ctx;
} rdof_sdrsplits_io_t;

struct rdof_sdrsplits;
typedef struct {
    struct rdof_sdrsplits *set;
    int i;
    rdof_sdrmodem_t m; /* the RX half */
    rdof_split_t s;
    double now;        /* the time of the call in progress, for the modem's replies */
} rdof_sdrsplit_t;

typedef struct rdof_sdrsplits {
    rdof_sdrsplits_io_t io;
    uint32_t rx_fs;
    rdof_sdrsplit_t *u[RDOF_SDRSPLITS_MAX];
    int n;
    bool busy, again;
    char error[128];
} rdof_sdrsplits_t;

void rdof_sdrsplits_init(rdof_sdrsplits_t *set, const rdof_sdrsplits_io_t *io, uint32_t rx_fs);
/* A split, its bring-up started (rdof_split_init; NULL p: the defaults). Its index, or -1 (set->error says why: no
 * room, no memory). */
int rdof_sdrsplits_add(rdof_sdrsplits_t *set, const rdof_split_params_t *p, double now);
void rdof_sdrsplits_free(rdof_sdrsplits_t *set);
/* Bytes from split i's repeater, and from its radio. */
void rdof_sdrsplits_upstream(rdof_sdrsplits_t *set, int i, const uint8_t *d, size_t n, double now);
void rdof_sdrsplits_from_radio(rdof_sdrsplits_t *set, int i, const uint8_t *d, size_t n, double now);
/* A burst the receiver heard on a channel: to every split's modem. */
void rdof_sdrsplits_heard(rdof_sdrsplits_t *set, int channel, const rdof_sdr_burst_t *b);
/* Every split's timers. */
void rdof_sdrsplits_tick(rdof_sdrsplits_t *set, double now);
rdof_split_t *rdof_sdrsplits_split(rdof_sdrsplits_t *set, int i);
rdof_sdrmodem_t *rdof_sdrsplits_modem(rdof_sdrsplits_t *set, int i);
/* Whether every split is up (bring-up done), and the first that failed (-1 none). */
bool rdof_sdrsplits_ready(const rdof_sdrsplits_t *set);
int rdof_sdrsplits_failed(const rdof_sdrsplits_t *set);

#endif /* RDOF_FEATURE_REPEATER */

#ifdef __cplusplus
}
#endif

#endif
