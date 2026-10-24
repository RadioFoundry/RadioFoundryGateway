// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * A fake radio for the host tests: answers
 * like the firmware of `kind`, synchronously. What it sends back queues in
 * `out` until the harness pumps it into the modem.
 */
#ifndef FAKE_RADIO_H
#define FAKE_RADIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/mmdvm.h"

#define FAKE_OUT 16384

typedef struct {
    char kind[16]; /* opengd77, opengd77_sfr, odr, odr_sfr, odr_split, mmdvm_hs, mmdvm */
    int protocol;
    char description[128];
    uint32_t band_lo, band_hi;
    uint8_t out[FAKE_OUT];
    size_t out_len;
    rdof_mmdvm_reader_t reader;
    bool has_freq;
    uint32_t freq, tx_freq;
    int cc, power, tx_level;
    bool has_pending;
    uint32_t pending_rx, pending_tx;
    bool simplex;
    int mode;
    int held, held1;
    bool transmitting, carrier, rf_busy, duplex_tx;
    char cw[8][41];
    int ncw;
    int drop_status; /* the next N GET_STATUS replies are lost */
    /* odr_split: a split duplex half (0xD6). Its modem clock: round(*clock * 1e6 * (1 + clock_rate) + offset_us). */
    const double *clock;
    double clock_offset_us, clock_rate;
    int role;
    int roles; /* the roles INFO offers (bit 0 TX half, bit 1 RX half); 0: both */
    bool has_epoch;
    uint32_t epoch_t_us, epoch_period;
    int epoch_refused;
    double epoch_at; /* the host time of the last GET_EPOCH answered: an outbound TS1 started then */
    bool silent; /* answers nothing (a half gone) */
    /* The TX role's slot clock (modem µs, unwrapped), and RadioFoundry's UTC additions (rdof/splitops.h's RDOF_OPS_*):
     * capabilities, guard, horizon and air delay; ALIGN (unkeyed a snap done snap_slots later, keyed the counter
     * only or flag 10), the snap back after a retune, the stream mode, TX_SCHED (keying up ~150 ms ahead, down 1 s
     * after the last when the queue keyed it) and STATUS's counters and flags. */
    int split_version; /* 0: 1 */
    bool no_caps;      /* a build without them: INFO stops at byte 9 */
    uint16_t split_caps;
    int guard_ms, horizon; /* 55 and 32, the HR_C7000's */
    int air_delay_us;      /* INFO 14-15 (4600 on the HR_C7000; 0 unknown) */
    double slot_origin, slot_period_us;
    uint32_t counter_base;
    bool aligned;      /* a counter base: an ALIGN, and no retune since that hasn't snapped back */
    bool has_grid;     /* the last ALIGN's grid */
    double grid_target, grid_period;
    uint32_t grid_counter;
    bool has_snap;     /* a snap under way until snap_until (modem µs): STATUS 46 shows 04 */
    double snap_until;
    double snap_slots;     /* 2.5 */
    double snap_offset_us; /* how far off the grid a snap lands */
    bool queue_keyed;      /* keyed by a queued burst, not DMR_START 01 */
    int naligns;
    uint32_t align_t_us, align_period, align_counter;
    int32_t align_phase;
    int align_flags;
    int stream_mode;
    struct {
        bool used;
        uint32_t counter;
        uint8_t slot, control, burst[33];
    } sched[32];
    int sched_cap;
    struct {
        uint32_t counter;
        uint8_t slot, burst[33];
        double start;
    } on_air[64];
    int non_air;
    uint32_t sched_sent, sched_late, sched_early_full, sched_replaced, sched_cooling;
    bool cooling;
    bool has_last_aired;
    double last_aired;
} fake_radio_t;

void fake_init(fake_radio_t *f, const char *kind);
void fake_write(fake_radio_t *f, const uint8_t *data, size_t len);
void fake_hear(fake_radio_t *f, uint8_t control, const uint8_t *burst, int rssi, int slot);
void fake_lose(fake_radio_t *f, int slot);
void fake_drain(fake_radio_t *f);
/* odr_split's TX slot clock: its modem time (µs, unwrapped), the slot clock's origin with any slew applied, and the
 * latest boundary at or before now_f (returns the slot index from the origin). */
double fake_modem_f(const fake_radio_t *f);
double fake_origin(const fake_radio_t *f);
long fake_slot_boundary(const fake_radio_t *f, double now_f, double *b);
/* The firmware's poll: a snap finishing, the snap back after a retune, and scheduled bursts whose slot has started
 * going on air (the queue keys up ~150 ms ahead, and a key-up it made ends 1 s after the last). */
void fake_air(fake_radio_t *f);
/* Take what it sent back. */
size_t fake_take(fake_radio_t *f, uint8_t *buf, size_t cap);

#endif
