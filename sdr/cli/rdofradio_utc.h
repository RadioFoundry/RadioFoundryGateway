// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * UTC slot timing in rdofradio's full duplex run (docs/spec/slot-timing.md),
 * as the board does it (esp_idf/main/slot_timing.c): the receive stream's
 * sample clock against the host's UTC, the clock gate, and the map the SDR
 * modem and the receiver get from them.
 *
 * Each read is stamped with its arrival on the host's UTC (CLOCK_REALTIME);
 * rdof_sample_clock fits the earliest-arrival line, so USB and scheduling
 * jitter only make arrivals late, anchored on the earliest arrival of every
 * 2 s; the rate between anchors two minutes apart is its ppm. The gate follows the kernel's clock
 * discipline once a second (ntp_adjtime: synced unless STA_UNSYNC, the
 * error its estimate): aligned while under slot_timing's NTP bound, else
 * holdover / free / stop as on_loss says. macOS gives no usable estimate,
 * so there the gate stays off the grid. With own_time the host's clock is
 * the reference even when nothing disciplines it (a PlutoSDR runs no NTP
 * client): a one-site Tier III needs its carriers on one clock, not on UTC.
 *
 * Assumed, as on the board: 48 kS/s position p is stream sample
 * p * rate / 48000, counted from the stream's start.
 */
#ifndef RDOFRADIO_UTC_H
#define RDOFRADIO_UTC_H

#include <stdbool.h>
#include <stdint.h>

#include "rdof/slotgrid.h"

struct rdof_sdrmodem;

/* The host's clock: UTC ns now, and the discipline's state (err_us NAN: no estimate). Tests give their own. */
typedef struct {
    int64_t (*utc_ns)(void *ctx);
    bool (*synced)(void *ctx, double *err_us);
    void *ctx;
} rdofradio_utc_clock_t;

/* Slot timing t for a stream of rate samples a second; own_time: on the host's clock synced or not; clock NULL: the
 * host's. */
void rdofradio_utc_setup(const rdof_slot_timing_t *t, double rate, bool own_time, const rdofradio_utc_clock_t *clock);
bool rdofradio_utc_enabled(void);
/* The stream's samples up to end_index (exclusive) arrived now; now_s the monotonic clock, for the gate. */
void rdofradio_utc_arrived(int64_t end_index, double now_s);
/* The map at 48 kS/s: position *i0 is UTC *t0 ns, *rate positions a UTC second. False while off the grid. */
bool rdofradio_utc_map48(int64_t *i0, int64_t *t0, double *rate);
/* Before every pull: the modem's grid, map and transmit gate. */
void rdofradio_utc_apply(struct rdof_sdrmodem *m);
/* The gate's state, why it's off the grid (empty when on it), the sample clock's ppm. */
rdof_gate_state_t rdofradio_utc_state(const char **why, double *ppm);
/* Whether the host's clock has settled: a program that sets the clock itself (netclock with --set-os-clock) steps it
 * at its first fix, which would move a keyed carrier's grid (the modem then steers a sample a frame, a minute or
 * more for a step of tens of ms). ready NULL: always settled. */
void rdofradio_utc_clock_ready(bool (*ready)(void));
bool rdofradio_utc_clock_settled(void);

#endif
