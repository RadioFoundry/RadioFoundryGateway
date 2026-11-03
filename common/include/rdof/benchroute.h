// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The bench's transmit through the router (docs/spec/fw/transmitters.md §5): an MMDVM modem as a bench job
 * (rdof/benchtx.h) sees it, whose bursts become an app's stream for the router's pool, as the parrot's do. It
 * answers GET_VERSION, ACKs SET_CONFIG, SET_FREQ and SET_MODE, and hands each DMR_DATA1 / DMR_DATA2 on as a network
 * burst: its stream ID (RDOF_BENCHROUTE_STREAM and a count), the addresses from the call's LC header or terminator
 * (a late entry's from its embedded LC), terminator set on a voice terminator. A stream ends there, or at SET_MODE
 * idle (the job unkeyed or stopped): the
 * next burst starts a new one. A call's further terminators are dropped (the router's transceiver sends its own
 * count of headers and terminators a call, R11.4). A burst the router refuses (no transmitter free) is NAKed, so the job stops.
 *
 * The job's frequency, color code and power don't reach the air from here: the router tunes its transmitter to the
 * path's (the job's frequency must be one of the site's paths). Portable C, no allocation, no locking.
 */
#ifndef RDOF_BENCHROUTE_H
#define RDOF_BENCHROUTE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/hblink.h"
#include "rdof/mmdvm.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_BENCHROUTE_STREAM 0x42450000u /* a bench stream's ID: "BE" and its count */
#define RDOF_BENCHROUTE_DESC "RadioFoundry Gateway router"

typedef struct {
    void (*reply)(void *ctx, const uint8_t *frame, size_t n);   /* frames back to the job */
    bool (*burst)(void *ctx, const rdof_net_burst_t *b);         /* to the router; false: refused */
    void *ctx;
    rdof_mmdvm_reader_t reader;
    bool streaming;                 /* a stream is open (its ID in sid) */
    bool ended;                     /* the last stream ended at its terminator: more terminators are dropped */
    uint8_t sid[4], seq;
    uint32_t src, dst;
    bool group;
    uint8_t frag[4][32];            /* a late entry's embedded signalling of B-E, for its addresses */
    unsigned frag_have;
    uint16_t count;                 /* streams so far */
    long bursts, refused, dropped;
} rdof_benchroute_t;

void rdof_benchroute_init(rdof_benchroute_t *r, void (*reply)(void *ctx, const uint8_t *frame, size_t n),
                          bool (*burst)(void *ctx, const rdof_net_burst_t *b), void *ctx);
/* Bytes from the job (its to_modem). */
void rdof_benchroute_write(rdof_benchroute_t *r, const uint8_t *b, size_t n);

#ifdef __cplusplus
}
#endif

#endif
