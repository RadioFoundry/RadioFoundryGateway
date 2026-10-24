// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The 0xD6 additions for UTC slot timing (split duplex, RadioFoundry's
 * draft): their encodings, in one place, outside the repeater feature so a
 * test's simulated half can speak them in any build. rdof/split.h's host side
 * and the fakes use them. Plain C11.
 */
#ifndef RDOF_SPLITOPS_H
#define RDOF_SPLITOPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* UTC slot n as a radio's slot counter (n mod 2^32, parity kept), and back to the whole n nearest near_n. */
static inline uint32_t rdof_slot_counter(int64_t n) { return (uint32_t)(uint64_t)n; }
int64_t rdof_n_of_counter(uint32_t c, int64_t near_n);

/* The 0xD6 additions for UTC slot timing as RadioFoundry built them (feature/split_utc_slots, e83857b2; its
 * docs/spec/split-duplex.md 3.1-3.9): every byte of them is here. Little
 * endian. The extension version stays 1: a half has an op only if its INFO capability bit says so.
 *
 * INFO 10-11 capabilities (only schedule and align built; the others reserved, read as 0), 12 the scheduled TX guard
 * (ms; 55 on the HR_C7000: read it), 13 the horizon (slots), 14-15 the slot-to-air delay (µs s16; 4600 HR_C7000,
 * 2200 HR_C6000; 0 unknown). SET_ROLE byte 6 the TX stream mode; both modes key alike (DMR_START, the 2 s rule), and
 * in mode 1 a queued burst keys up ~150 ms ahead and a key-up it made ends 1 s after the last. ALIGN (sub 7, either
 * mode) t_us, period_ns, counter [, max_slew_us u16]; reply phase_us s32 and flags: unkeyed 04 snapping (done over
 * 3-6 slots, read from STATUS 46), keyed 01 (counter only, within 1 ms and on parity) or 10 with result 0. After a
 * retune the TX half snaps back to the last grid itself. TX_SCHED (sub 8, mode 1, a counter base) counter, slot,
 * control, burst; reply counter and outcome (4 with result 0 in the cool-down); an idle burst or a repeated
 * terminator clears the slot's place. STATUS 32-47: sent, late, early or full, replaced, cool-down refusals (u16),
 * the live phase (s32), the ALIGN flags now (01 aligned, 04 snapping, 20 counter base), reserved. */
#define RDOF_OPS_CAPS_AT 10
#define RDOF_OPS_GUARD_AT 12
#define RDOF_OPS_HORIZON_AT 13
#define RDOF_OPS_AIR_DELAY_AT 14
#define RDOF_OPS_INFO_LEN 16
#define RDOF_OPS_SCHEDULE 0x0001
#define RDOF_OPS_ALIGN 0x0002
#define RDOF_OPS_SLEW 0x0004    /* reserved: not built */
#define RDOF_OPS_RX_META 0x0008 /* reserved: not built */
#define RDOF_OPS_TRIM 0x0010    /* reserved: not built */
#define RDOF_OPS_PPS 0x0020     /* reserved: not built */
#define RDOF_OPS_BUILT 0x0003
#define RDOF_OPS_GUARD_MS 40 /* only when INFO is too short to say */
#define RDOF_OPS_HORIZON_SLOTS 32
#define RDOF_OPS_STREAM_MODE_AT 6 /* SET_ROLE's body (the role at 0) */
#define RDOF_OPS_STREAM_FIFO 0
#define RDOF_OPS_STREAM_SCHEDULED 1
#define RDOF_OPS_SUB_ALIGN 7
#define RDOF_OPS_SUB_TX_SCHED 8
#define RDOF_OPS_ALIGNED 0x01 /* ALIGN's reply flags, and STATUS 46's */
#define RDOF_OPS_SLEWING 0x02
#define RDOF_OPS_SNAPPING 0x04
#define RDOF_OPS_PERIOD_APPLIED 0x08 /* never set yet */
#define RDOF_OPS_NOT_APPLIED 0x10
#define RDOF_OPS_COUNTER_BASE 0x20 /* STATUS 46 only */
#define RDOF_OPS_ALIGNED_US 1000
#define RDOF_OPS_STATUS_EXT_AT 32
#define RDOF_OPS_STATUS_LEN 48
#define RDOF_OPS_QUEUE_KEY_MS 150   /* a queued burst keys up this long before its slot */
#define RDOF_OPS_QUEUE_UNKEY_S 1.0  /* a key-up from the queue alone ends this long after its last burst */
enum { RDOF_SCHED_QUEUED = 0, RDOF_SCHED_LATE, RDOF_SCHED_EARLY, RDOF_SCHED_DUPLICATE, RDOF_SCHED_FULL };
#define RDOF_SCHED_REPLACED RDOF_SCHED_DUPLICATE

typedef struct {
    uint16_t sent, late, early_full, replaced, cooling;
    int32_t phase_us;
    int flags;
} rdof_ops_status_t;

uint16_t rdof_ops_caps(const uint8_t *info, size_t len);
int rdof_ops_guard_ms(const uint8_t *info, size_t len);
int rdof_ops_horizon(const uint8_t *info, size_t len);
int rdof_ops_air_delay_us(const uint8_t *info, size_t len);
bool rdof_ops_can_align(uint16_t caps, int stream_mode);
/* ALIGN's body: a boundary t_us on its clock, period_ns (0: unchanged), the counter for it; max_slew_us < 0: left
 * out. Returns the length. */
size_t rdof_ops_align(uint8_t *out, uint32_t t_us, uint32_t period_ns, int64_t n, int max_slew_us);
/* ALIGN's reply (ok: result 0): phase_us and flags. False: refused, or nothing to read. */
bool rdof_ops_aligned(bool ok, const uint8_t *data, size_t len, int32_t *phase_us, int *flags);
/* TX_SCHED's body for UTC slot n (its slot must match n's parity: TS1 even); 0 if they don't. */
size_t rdof_ops_schedule(uint8_t *out, int64_t n, int slot, uint8_t control, const uint8_t burst[33]);
bool rdof_ops_scheduled(const uint8_t *data, size_t len, uint32_t *counter, int *outcome);
/* STATUS's bytes 32-46 (a reply's body, the role at 0); false if it's too short. */
bool rdof_ops_status_ext(const uint8_t *status, size_t len, rdof_ops_status_t *st);
/* STATUS 32-46 written into a 48-byte status body. */
void rdof_ops_put_status_ext(uint8_t *status, const rdof_ops_status_t *st);

#ifdef __cplusplus
}
#endif

#endif
