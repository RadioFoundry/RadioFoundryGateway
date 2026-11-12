// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * A trunked site's payload carrier on a split TX half (docs/spec/fw/tier3.md 9.3): a radio with the split duplex
 * extension sends the carrier's outbound, the SDR hears its inbound. One split of rdof/sdr/sdrsplit.h: its RX half an
 * SDR modem of its own, receive only, on the carrier's in; its TX half the radio. No repeater over it: once the halves
 * are up this sets the split up itself (SET_CONFIG duplex DMR, SET_MODE, SET_FREQ) and then the Tier III unit writes the
 * carrier's bursts into it (rdof_t3unit_split_pump: TX_SCHED for UTC slot k when the scheduled stream is up).
 *
 * The site's slot k is the UTC grid's n (rdof_t3split_utc gives the split the host's clock against UTC, so the TX
 * half's counter is aligned to it). What the RX half hears comes back labeled with its slot by the TX half's epoch
 * (the split relays it), the way a split repeater's inbound does: rdof_t3split_take.
 *
 * No I/O, no threads: the platform writes to_radio's bytes to the radio, feeds back what it sends, the heard bursts,
 * and ticks it every ~20 ms. One lock around every call. In Rust (rust/src/t3split.rs).
 */
#ifndef RDOF_SDR_TRUNKSPLIT_H
#define RDOF_SDR_TRUNKSPLIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/features.h"
#include "rdof/mmdvm.h"
#include "rdof/sdr/sdrsplit.h"
#include "rdof/t3rf.h"

#ifdef __cplusplus
extern "C" {
#endif

#if RDOF_FEATURE_REPEATER

#define RDOF_T3SPLIT_Q 32

typedef struct {
    void (*to_radio)(void *ctx, const uint8_t *d, size_t n);
    int (*rx_channel)(void *ctx, uint32_t hz); /* the receiver channel for an RX frequency, -1 none */
    int64_t (*now48)(void *ctx);               /* the receiver's 48 kS/s position now; NULL: the latest heard */
    void *ctx;
} rdof_t3split_io_t;

/* The rdof_sdrsplits_* calls it makes. The Rust (rust/src/t3split.rs) is in rdof_common and can't link rdof_sdr, so
 * rdof_t3split_init below, compiled into its caller, hands it this table. */
typedef struct {
    void (*init)(rdof_sdrsplits_t *set, const rdof_sdrsplits_io_t *io, uint32_t rx_fs);
    int (*add)(rdof_sdrsplits_t *set, const rdof_split_params_t *p, double now);
    void (*release)(rdof_sdrsplits_t *set);
    void (*upstream)(rdof_sdrsplits_t *set, int i, const uint8_t *d, size_t n, double now);
    void (*from_radio)(rdof_sdrsplits_t *set, int i, const uint8_t *d, size_t n, double now);
    void (*heard)(rdof_sdrsplits_t *set, int channel, const rdof_sdr_burst_t *b);
    void (*tick)(rdof_sdrsplits_t *set, double now);
    rdof_split_t *(*split)(rdof_sdrsplits_t *set, int i);
} rdof_t3split_ops_t;

typedef enum {
    RDOF_T3SPLIT_BRINGUP = 0, /* the halves' versions, roles, clocks */
    RDOF_T3SPLIT_SETUP,       /* SET_CONFIG, SET_MODE, SET_FREQ answered one by one */
    RDOF_T3SPLIT_UP,          /* the carrier's bursts may go out */
    RDOF_T3SPLIT_FAILED,
} rdof_t3split_state_t;

typedef struct rdof_t3split {
    rdof_t3split_io_t io;
    const rdof_t3split_ops_t *ops;
    rdof_sdrsplits_t set;
    rdof_t3rf_split_t o;    /* the Tier III unit's pump state for this carrier */
    uint32_t rx_hz, tx_hz;
    int cc;
    int state;                 /* rdof_t3split_state_t */
    int setup_step;            /* the setup's frame awaiting its answer (0..2) */
    double setup_at;
    char error[128];
    rdof_mmdvm_reader_t reader;
    struct {
        uint8_t slot, control;
        int rssi;
        uint8_t burst[RDOF_BURST_BYTES];
    } q[RDOF_T3SPLIT_Q];
    int qhead, qtail;
    uint32_t heard, dropped, naks;
} rdof_t3split_t;

/* Carrier `carrier` (its index in the Tier III unit) on rx_hz / tx_hz, cc; the bring-up started (the scheduled stream
 * asked for: FIFO if the radio lacks it). rx_fs: the receiver's rate, as rdof_sdrsplits_init's. False with error.
 * rdof_t3split_start is the same with the rdof_sdrsplits_* calls given (ops, kept: static). */
bool rdof_t3split_start(rdof_t3split_t *t, const rdof_t3split_ops_t *ops, const rdof_t3split_io_t *io, uint32_t rx_fs,
                        int carrier, uint32_t rx_hz, uint32_t tx_hz, int cc, double now);
static inline bool rdof_t3split_init(rdof_t3split_t *t, const rdof_t3split_io_t *io, uint32_t rx_fs, int carrier,
                                     uint32_t rx_hz, uint32_t tx_hz, int cc, double now)
{
    static const rdof_t3split_ops_t ops = {
        rdof_sdrsplits_init,       rdof_sdrsplits_add,   rdof_sdrsplits_free, rdof_sdrsplits_upstream,
        rdof_sdrsplits_from_radio, rdof_sdrsplits_heard, rdof_sdrsplits_tick, rdof_sdrsplits_split,
    };
    return rdof_t3split_start(t, &ops, io, rx_fs, carrier, rx_hz, tx_hz, cc, now);
}
void rdof_t3split_free(rdof_t3split_t *t);
/* Bytes from the radio, a burst the receiver heard (any channel: the RX half keeps its own), timers. */
void rdof_t3split_from_radio(rdof_t3split_t *t, const uint8_t *d, size_t n, double now);
void rdof_t3split_heard(rdof_t3split_t *t, int channel, const rdof_sdr_burst_t *b);
void rdof_t3split_tick(rdof_t3split_t *t, double now);
/* The host's clock against UTC (UTC µs = host µs + utc_minus_host_us, wrong by at most bound_us). */
void rdof_t3split_utc(rdof_t3split_t *t, double utc_minus_host_us, double bound_us, double now);
/* The split under it (for rdof_t3unit_split_pump; flush with rdof_t3split_tick after). */
struct rdof_split *rdof_t3split_split(rdof_t3split_t *t);
/* The next burst heard on the carrier's inbound: its slot (1, 2) by the TX half's epoch, the MMDVM control byte, the
 * burst. False: none. */
bool rdof_t3split_take(rdof_t3split_t *t, int *slot, uint8_t *control, uint8_t burst[RDOF_BURST_BYTES],
                          int *rssi);
/* A heard burst as rdof_t3unit_rx takes it, from its control byte (the MS family, the carrier's cc). */
void rdof_t3split_burst(const rdof_t3split_t *t, int slot, uint8_t control, const uint8_t burst[RDOF_BURST_BYTES],
                           rdof_burst_t *out);
/* The Rust's size and alignment of each struct it mirrors (rdof_t3split_t and those in it), as rdof_t3rf_layout. */
const rdof_t3rf_layout_t *rdof_t3split_layout(size_t i);

#endif /* RDOF_FEATURE_REPEATER */

#ifdef __cplusplus
}
#endif

#endif
