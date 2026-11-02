// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The parrot: a transmission heard on the air, sent back over the air once it ends. Voice (from its LC header to
 * its terminator), data (preambles, header and blocks) and CSBKs, burst for burst, so a radio hears its own call
 * or data come back. Each burst is kept regenerated, as a repeater would send it (rdof/duplex.h): the LC header
 * and terminator coded again from the header's LC, voice's AMBE FEC and embedded LC, a CSBK's, data header's and
 * rate 1/2 block's BPTC. What was heard wrong past the FEC stays wrong; the bit errors the FEC took out don't go
 * back on the air. With an id set, only what's addressed to it (a private or group call to
 * the id, data or a CSBK to it) is sent back; with id 0, everything. A private voice call comes back from whom it
 * called to the caller (its LC's ids swapped), so the caller's radio plays it. Private packet data (its CRC-32
 * good) comes back the same way: the header's ids swapped, and in an IPv4 datagram (SAP 4) the addresses and the
 * UDP ports, in a compressed UDP/IPv4 header (SAP 3) the address ids and ports, rebuilt as unconfirmed rate 1/2
 * after as many preambles as it came with (swapping both of a pair leaves the IP and UDP checksums right). A
 * group call, group data and CSBKs come back as they were sent.
 *
 * The platform passes what the radio hears (rdof_parrot_heard, rdof_parrot_lost: the modem's on_rx and on_lost)
 * and calls rdof_parrot_poll every ~20 ms. The replay comes out as network bursts, one a 60 ms frame, a few ahead
 * of their time: start() (optional) once before the first, then send() each, so a transceiver gets a tune
 * (rdof_txvr_on_tune) and then its bursts (rdof_txvr_on_burst), a repeater its bursts (rdof_repeater_on_burst).
 *
 * One transmission at a time: while one is recorded or replayed, what's heard on the other slot (or, through
 * rdof_parrot_heard_at, on another frequency) is not kept.
 * Bounded: RDOF_PARROT_BURSTS bursts (60 s of voice); a longer transmission is cut there and its terminator kept.
 * The caller locks: nothing here is thread safe.
 */
#ifndef RDOF_PARROT_H
#define RDOF_PARROT_H

#include <stdbool.h>
#include <stdint.h>

#include "rdof/duplex.h"
#include "rdof/features.h"
#include "rdof/hblink.h"
#include "rdof/ipgw.h"

#ifdef __cplusplus
extern "C" {
#endif

#if RDOF_FEATURE_TX

#ifndef RDOF_PARROT_BURSTS
#define RDOF_PARROT_BURSTS 1000
#endif
#define RDOF_PARROT_DELAY_S 2.5 /* after the end, before the replay: past a transceiver's 2 s clear channel */
#define RDOF_PARROT_GAP_S 0.5   /* a transmission with no terminator (data, a CSBK, a lost call) ends this quiet */
#define RDOF_PARROT_AHEAD 3     /* bursts sent ahead of their frame */
#define RDOF_PARROT_STREAM 0x50410000u /* a replay's stream id: "PA" and the replay's count */
#define RDOF_PARROT_WAIT_S 10.0 /* with ready: how long a replay waits for a transmitter before it's given up */

typedef enum { RDOF_PARROT_IDLE = 0, RDOF_PARROT_RECORDING, RDOF_PARROT_WAITING, RDOF_PARROT_REPLAYING } rdof_parrot_state_t;

typedef struct {
    uint8_t control;
    uint8_t burst[RDOF_BURST_BYTES];
} rdof_parrot_burst_t;

typedef struct rdof_parrot {
    /* settings */
    uint32_t id;    /* only what's addressed to it; 0: everything */
    double delay_s; /* RDOF_PARROT_DELAY_S by default */
    /* the replay: start (optional) before the first burst, send each burst */
    void (*start)(void *ctx, const uint8_t stream_id[4], int slot, uint32_t src, uint32_t dst, bool group);
    void (*send)(void *ctx, const rdof_net_burst_t *b);
    /* optional: whether a transmission to it heard on slot is kept (the talk path's policy may not reach it) */
    bool (*admit)(void *ctx, int slot, uint32_t src, uint32_t dst, bool group);
    /* optional: whether a transmitter is free for the replay now; while not, it waits, up to RDOF_PARROT_WAIT_S */
    bool (*ready)(void *ctx);
    void *ctx;

    /* state */
    rdof_parrot_state_t state;
    uint32_t hz; /* where the transmission was heard (rdof_parrot_heard_at; admit sees it), 0 not known */
    int slot;
    uint32_t src, dst;
    bool group, voice;
    double last, due; /* the last burst heard; when the replay starts */
    int n, sent;
    int cc;                    /* the header's color code */
    rdof_voice_regen_t regen;  /* voice: the call's LC and the embedded LC's state */
    long corrected;            /* bit errors the regeneration took out, this recording */
    uint32_t streams; /* replays started, also the stream id's low bits */
    uint32_t replayed; /* transmissions sent back */
    uint32_t cut;      /* bursts left out of a full recording */
    uint32_t ignored;  /* transmissions not to the id */
    uint32_t gave_up;  /* replays no transmitter was free for */
    rdof_parrot_burst_t bursts[RDOF_PARROT_BURSTS];
#if RDOF_FEATURE_DATA_BLOCKS
    uint32_t data_replies; /* packet data sent back to its sender */
    uint8_t data[RDOF_IPGW_MAX_PACKET + 32]; /* a data reply's message, put together */
    rdof_ipgw_dmr_t reply;
#endif
} rdof_parrot_t;

void rdof_parrot_init(rdof_parrot_t *p, uint32_t id);
void rdof_parrot_heard(rdof_parrot_t *p, int slot, uint8_t control, const uint8_t burst[RDOF_BURST_BYTES], double now);
void rdof_parrot_lost(rdof_parrot_t *p, int slot, double now);
/* The same from a receiver of several frequencies: a recording keeps to the frequency it began on (hz), so the
   same transmission heard on a neighbor, or another one elsewhere, isn't mixed in. */
void rdof_parrot_heard_at(rdof_parrot_t *p, uint32_t hz, int slot, uint8_t control,
                          const uint8_t burst[RDOF_BURST_BYTES], double now);
void rdof_parrot_lost_at(rdof_parrot_t *p, uint32_t hz, int slot, double now);
void rdof_parrot_poll(rdof_parrot_t *p, double now);
const char *rdof_parrot_state_name(const rdof_parrot_t *p);

#endif /* RDOF_FEATURE_TX */

#ifdef __cplusplus
}
#endif

#endif
