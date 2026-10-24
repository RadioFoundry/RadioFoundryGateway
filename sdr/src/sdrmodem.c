// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The SDR as an MMDVM modem (rdof/sdr/sdrmodem.h). Written from the MMDVM
 * modem protocol as RadioFoundry's docs/spec/mmdvm-protocol.md writes it
 * down (MIT, commit 1ce780d1): protocol 1 replies, status layout, NAK reasons,
 * a simplex modem's 60 ms burst spacing and DMR_LOST.
 */
#include "rdof/sdr/sdrmodem.h"

#if RDOF_FEATURE_SDR

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "rdof/duplex.h"
#include "rdof/layer2.h"
#include "rdof/pdu.h"

#define LEAD_SYMBOLS 24  /* how far ahead of the pull position a burst is queued: 5 ms */
#define MUTE_48K 720     /* half a slot: a heard burst this close to one sent is ours */
#define CACH_SYMBOLS 12
#define DATA_BUSY_48K 17280 /* 360 ms: an inbound slot stays busy this long after a data burst */
#define DC_STALE_48K 48000  /* DCDM: a slot timing nothing has used for 1 s is dropped */

enum { NAK_UNKNOWN = 1, NAK_WRONG_MODE = 2, NAK_INVALID = 4, NAK_NO_SPACE = 5 };

#define ODR_SPLIT 0xD6
#define ROLE_TX 1
#define ROLE_RX 2
#define AT_HOLD_48K (2 * 48000)    /* SET_AT's mask falls back to idle this long after the last one */
#define PREBUFFER 3                /* SET_ROLE's defaults (split-duplex.md 3.2) */
#define POWER_CAP 64
#define MAX_KEY_S 180
#define COOLDOWN_S 20
#define LEASE_S 10                 /* SET_ROLE's default */
#define EPOCH_VALID_48K (120 * 48000) /* an epoch older than this labels nothing (split-duplex.md 7) */
#define SLOT_GUARD_US 1250         /* an inbound burst starts this long after its slot (30 ms less 27.5, halved) */

#define RX_DELAY_48K(m) (40 + ((m)->rx_fs > 48000 ? 8 : 0)) /* the receiver reports a burst this late */
#define GUARD_MS 40                /* TX_SCHED: a burst due sooner than this is late (INFO byte 12) */
#define HORIZON 32                 /* TX_SCHED: slots ahead it takes (INFO byte 13) */

static void send(rdof_sdrmodem_t *m, uint8_t cmd, const uint8_t *p, size_t len)
{
    uint8_t f[300];
    size_t n = rdof_mmdvm_frame(cmd, p, len, f, sizeof f);
    if (n && m->io.reply)
        m->io.reply(m->io.ctx, f, n);
}

static void ack(rdof_sdrmodem_t *m, uint8_t cmd) { send(m, RDOF_MMDVM_ACK, &cmd, 1); }

static void nak(rdof_sdrmodem_t *m, uint8_t cmd, int why, const char *text)
{
    uint8_t p[2] = {cmd, (uint8_t)why};
    m->refused++;
    snprintf(m->error, sizeof m->error, "%s", text);
    send(m, RDOF_MMDVM_NAK, p, 2);
}

static int space(const rdof_sdrmodem_t *m, int s) { return RDOF_SDRMODEM_QUEUE - m->qn[s]; }

bool rdof_sdrmodem_transmitting(const rdof_sdrmodem_t *m)
{
    return m->keyed || m->qn[0] || m->qn[1] || (m->mod_ok && !rdof_mod_idle(&m->mod));
}

/* Where the receiver reports a burst whose symbol 0 the modulator put at tx sample s. */
static int64_t heard_at(const rdof_sdrmodem_t *m, int64_t s)
{
    return s * 48000 / (int64_t)m->tx_fs + RX_DELAY_48K(m);
}

static void remember(rdof_sdrmodem_t *m, int64_t start)
{
    m->sent48[m->nsent++ % 8] = heard_at(m, start);
}

bool rdof_sdrmodem_muted_at(const rdof_sdrmodem_t *m, int64_t p)
{
    if (m->duplex)
        return false;
    for (int i = 0; i < 8 && i < m->nsent; i++)
        if (p > m->sent48[i] - MUTE_48K && p < m->sent48[i] + MUTE_48K)
            return true;
    return false;
}

int rdof_sdrmodem_init(rdof_sdrmodem_t *m, const rdof_sdrmodem_io_t *io, uint32_t rx_fs, uint32_t tx_fs, bool can_full)
{
    memset(m, 0, sizeof *m);
    m->io = *io;
    m->rx_fs = rx_fs;
    m->tx_fs = tx_fs;
    m->can_full = can_full;
    m->channel = -1;
    m->dc_last = INT64_MIN;
#if RDOF_FEATURE_REPEATER
    const uint8_t null5[5] = {0};
    uint8_t sl9[9];
    rdof_short_lc(null5, sl9);
    rdof_short_lc_rows(sl9, m->slc);
    memcpy(m->slc_next, m->slc, sizeof m->slc);
#endif
    rdof_mmdvm_reader_init(&m->reader);
    if (rx_fs < 48000 || rx_fs % 48000 || rdof_mod_init(&m->mod, tx_fs, 0.0, 4.0, 1.0f, 4, 0) != 0) {
        rdof_mod_free(&m->mod);
        snprintf(m->error, sizeof m->error, "the modulator won't run at %lu S/s", (unsigned long)tx_fs);
        return -1;
    }
    m->mod_ok = true;
    return 0;
}

void rdof_sdrmodem_free(rdof_sdrmodem_t *m)
{
    if (m->mod_ok)
        rdof_mod_free(&m->mod);
    m->mod_ok = false;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* ── UTC slot timing ──────────────────────────────────────────────────────── */

void rdof_sdrmodem_grid(rdof_sdrmodem_t *m, bool on, int offset_us)
{
    if (on != m->grid || offset_us != m->grid_offset_us)
        m->grid_placed = false;
    m->grid = on;
    m->grid_offset_us = offset_us;
}

void rdof_sdrmodem_grid_map(rdof_sdrmodem_t *m, bool ok, int64_t i0, int64_t t0_ns, double rate)
{
    m->grid_ok = ok && rate > 0;
    if (m->grid_ok)
        m->grid_i0 = i0, m->grid_t0 = t0_ns, m->grid_rate = rate;
}

static void retune_carrier(rdof_sdrmodem_t *m)
{
    if (m->mod_ok)
        rdof_mod_set_offset(&m->mod, m->tune_off_hz - m->carrier_ppm * 1e-6 * (double)m->tx_hz);
}

void rdof_sdrmodem_carrier_ppm(rdof_sdrmodem_t *m, double ppm)
{
    m->carrier_ppm = ppm;
    retune_carrier(m);
}

void rdof_sdrmodem_set_transmit(rdof_sdrmodem_t *m, bool transmit)
{
    m->stopped = !transmit;
    if (m->stopped && m->keyed)
        m->unkey = true;
}

static bool mapped(const rdof_sdrmodem_t *m) { return m->grid && m->grid_ok; }

/* UTC ns of 48 kS/s position p48 (mapped only). */
static int64_t utc_of(const rdof_sdrmodem_t *m, double p48)
{
    return m->grid_t0 + (int64_t)llround((p48 - (double)m->grid_i0) * 1e9 / m->grid_rate);
}


bool rdof_sdrmodem_utc_slot(const rdof_sdrmodem_t *m, int64_t pos48, int64_t *n, int *ts, int64_t *utc_ns)
{
    if (!mapped(m))
        return false;
    int64_t start = utc_of(m, (double)(pos48 - RX_DELAY_48K(m))), err;
    *n = rdof_grid_inbound(start, m->grid_offset_us, ts, &err);
    if (utc_ns)
        *utc_ns = start;
    return true;
}

#if RDOF_FEATURE_REPEATER
/* The modulator sample at UTC u ns (mapped only). */
static double tx_of(const rdof_sdrmodem_t *m, int64_t u)
{
    return ((double)m->grid_i0 + (double)(u - m->grid_t0) * m->grid_rate / 1e9) * (double)m->tx_fs / 48000.0;
}

/* Where slot k should start (its CACH's first symbol), in modulator samples: the host's ALIGN, else the UTC grid. */
static bool target_of(const rdof_sdrmodem_t *m, int64_t k, double *t)
{
    if (m->align_target) {
        *t = m->align_pos + (double)(k - m->align_k) * m->slot_p;
        return true;
    }
    if (mapped(m) && m->grid_placed) {
        *t = tx_of(m, rdof_grid_boundary(m->grid_n0 + k, m->grid_offset_us));
        return true;
    }
    return false;
}

/* Unkeyed (or a key-up's first slot): the slot clock snapped to its target, slot_k the first slot at or after
 * sample from. False with nothing to snap to. */
static bool place(rdof_sdrmodem_t *m, int64_t from, int64_t slot)
{
    int64_t k;
    if (m->align_target) {
        k = m->align_k + (int64_t)ceil(((double)from - m->align_pos) / m->slot_p);
    } else if (mapped(m)) {
        int64_t b, n = rdof_grid_next_boundary(utc_of(m, (double)from * 48000.0 / (double)m->tx_fs), m->grid_offset_us, &b);
        if (!m->grid_placed) {
            m->grid_n0 = n & ~(int64_t)3; /* TS1 on even n; the short LC's 4-slot cycle on n mod 4 */
            m->grid_placed = true;
        }
        k = n - m->grid_n0;
        if (!m->aligned)
            m->counter_base = (uint32_t)(uint64_t)m->grid_n0;
    } else {
        return false;
    }
    double t;
    target_of(m, k, &t);
    if (t < (double)from) /* rounding: the next one */
        target_of(m, ++k, &t);
    int64_t base = llround(t) - k * slot;
    if (m->clock_running && base != m->base)
        m->grid_snaps++;
    m->base = base;
    m->slot_k = (long)k;
    m->grid_err_ns = 0;
    return true;
}

/* Keyed: slot slot_k's start against its target, steered a sample at most (slipped or inserted before it). */
static void steer(rdof_sdrmodem_t *m, int64_t slot)
{
    double t;
    if (!target_of(m, m->slot_k, &t))
        return;
    double err = (double)(m->base + (int64_t)m->slot_k * slot) - t;
    m->grid_err_ns = (int64_t)llround(err * 1e9 / (double)m->tx_fs);
    int64_t step = llround(err);
    step = step > 1 ? 1 : step < -1 ? -1 : step;
    if (step) {
        m->base -= step;
        m->grid_slips++;
    }
}
#endif

/* ── split duplex ─────────────────────────────────────────────────────────── */
#if RDOF_FEATURE_REPEATER

static void put_le(uint8_t *p, uint32_t v, int n)
{
    for (int i = 0; i < n; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

/* The modem clock: the 48 kS/s sample counter in µs (wraps as a u32). */
static uint32_t us_at(int64_t p48) { return (uint32_t)(uint64_t)(p48 * 125 / 6); }

static int64_t clock48(const rdof_sdrmodem_t *m)
{
    int64_t t = m->io.now48 ? m->io.now48(m->io.ctx) : m->now48;
    return t > m->now48 ? t : m->now48;
}

static bool epoch_valid(const rdof_sdrmodem_t *m, int64_t now)
{
    return m->role == ROLE_RX && m->has_epoch && now - m->epoch_at48 <= EPOCH_VALID_48K;
}

/* It can be the TX half: the modulator runs and the front end has a transmitter (tx_tune). */
static bool can_tx(const rdof_sdrmodem_t *m) { return m->mod_ok && m->io.tx_tune; }

/* What a role had, dropped (a role change or its end). */
static void drop_role(rdof_sdrmodem_t *m)
{
    m->has_epoch = false;
    m->keyed = m->unkey = false;
    m->qn[0] = m->qn[1] = 0;
    m->slot_k = -1;
    m->clock_running = false;
    m->has_at = false;
    m->at_mask = m->at_mode = 0;
    m->cool_until48 = 0;
    for (int s = 0; s < 2; s++)
        m->streaming[s] = m->tx_active[s] = m->last_term[s] = false;
    m->stream_mode = 0;
    m->aligned = m->align_target = false;
    m->align_flags = 0;
    m->counter_base = 0;
    m->grid_placed = false;
    memset(m->sched, 0, sizeof m->sched);
}

/* The role ends when no 0xD6 request has come for its lease. */
static void check_lease(rdof_sdrmodem_t *m, int64_t now)
{
    if (m->role && now - m->role_at48 > (int64_t)m->lease_s * 48000) {
        m->role = 0;
        drop_role(m);
        m->lease_expiries++;
    }
}

int rdof_sdrmodem_at(const rdof_sdrmodem_t *m)
{
    if (m->role != ROLE_TX)
        return rdof_sdrmodem_busy(m);
    int mask = m->has_at && m->now48 - m->at_at48 <= AT_HOLD_48K ? m->at_mask : 0;
    if (m->at_mode == 0)
        mask |= (m->tx_active[0] ? 1 : 0) | (m->tx_active[1] ? 2 : 0);
    return mask;
}

/* µs on the modem clock at modulator sample s. */
static uint32_t us_tx(const rdof_sdrmodem_t *m, int64_t s) { return (uint32_t)(uint64_t)(s * 1000000 / (int64_t)m->tx_fs); }

static int flags_of(const rdof_sdrmodem_t *m, int64_t now)
{
    return (epoch_valid(m, now) ? 0x01 : 0) | (m->role == ROLE_TX && m->keyed ? 0x02 : 0) |
           (m->role == ROLE_TX && now < m->cool_until48 ? 0x04 : 0) |
           (m->role == ROLE_TX && m->clock_running ? 0x08 : 0);
}

/* Outbound slot n after the epoch holds a burst whose slot started at t: TS2 when n is even (inbound TSx starts a
 * slot after outbound TSx), TS1 when odd; 0 without a valid epoch. */
static int label_of(rdof_sdrmodem_t *m, int64_t pos48)
{
    if (!epoch_valid(m, m->now48))
        return 0;
    int64_t dt_ns = (int64_t)(int32_t)(us_at(pos48) - SLOT_GUARD_US - m->epoch_us) * 1000;
    int64_t p = m->period_ns;
    int64_t n = dt_ns >= 0 ? (dt_ns + p / 2) / p : -((-dt_ns + p / 2) / p);
    m->residual_us = (int32_t)((dt_ns - n * p) / 1000);
    return (n & 1) == 0 ? 2 : 1;
}

int rdof_sdrmodem_label(rdof_sdrmodem_t *m, int64_t pos48) { return label_of(m, pos48); }

static void split_reply(rdof_sdrmodem_t *m, uint8_t sub, uint8_t result, const uint8_t *d, size_t n)
{
    uint8_t b[2 + RDOF_OPS_STATUS_LEN] = {sub, result};
    if (n)
        memcpy(b + 2, d, n);
    send(m, ODR_SPLIT, b, 2 + n);
}

static size_t split_info(const rdof_sdrmodem_t *m, int64_t now, uint8_t b[16])
{
    b[0] = 1;                                 /* extension version */
    b[1] = (uint8_t)(0x02 | (can_tx(m) ? 0x01 : 0)); /* roles offered: RX always, TX when it can send */
    b[2] = (uint8_t)m->role;
    b[3] = can_tx(m) ? RDOF_SDRMODEM_QUEUE : 0; /* TX buffer per slot */
    b[4] = can_tx(m) ? 1 : 0;                 /* our CACH bits on air */
    put_le(b + 5, us_at(now), 4);
    b[9] = (uint8_t)flags_of(m, now);
    const unsigned caps = can_tx(m) ? RDOF_OPS_SCHEDULE | RDOF_OPS_ALIGN : 0; /* RadioFoundry's UTC additions (rdof/splitops.h) */
    put_le(b + RDOF_OPS_CAPS_AT, caps, 2);
    b[RDOF_OPS_GUARD_AT] = GUARD_MS;
    b[RDOF_OPS_HORIZON_AT] = HORIZON;
    put_le(b + RDOF_OPS_AIR_DELAY_AT, 0, 2); /* unknown: the front end's own latency after the sample position */
    return RDOF_OPS_INFO_LEN;
}

static void split_status(rdof_sdrmodem_t *m, int64_t now)
{
    uint8_t b[RDOF_OPS_STATUS_LEN] = {0};
    b[0] = (uint8_t)m->role;
    b[1] = (uint8_t)flags_of(m, now);
    b[2] = (uint8_t)(m->role == ROLE_TX ? 0 : rdof_sdrmodem_busy(m));
    if (m->role == ROLE_TX) {
        b[3] = (uint8_t)rdof_sdrmodem_at(m);
        b[4] = (uint8_t)m->qn[0];
        b[5] = (uint8_t)m->qn[1];
        put_le(b + 6, m->underruns[0], 2);
        put_le(b + 8, m->underruns[1], 2);
        put_le(b + 10, m->idle_bursts, 2);
        put_le(b + 12, m->cach_bursts, 2);
        put_le(b + 14, m->short_lcs, 2);
        put_le(b + 26, (uint32_t)(m->keyed ? (now - m->keyed_at48) / 48000 : 0), 2);
        put_le(b + 28, m->bad_short_lcs, 2);
    }
    put_le(b + 16, m->labeled[0], 2);
    put_le(b + 18, m->labeled[1], 2);
    put_le(b + 20, m->unlabeled, 2);
    put_le(b + 22, (uint32_t)(int16_t)(m->residual_us < -32768 ? -32768 : m->residual_us > 32767 ? 32767
                                                                                                   : m->residual_us),
           2);
    int64_t age = m->has_epoch ? (now - m->epoch_at48) / 48000 : 65535;
    put_le(b + 24, (uint32_t)(age > 65535 ? 65535 : age), 2);
    put_le(b + 30, m->lease_expiries, 2);
    const rdof_ops_status_t st = {(uint16_t)m->sched_sent,     (uint16_t)m->sched_late,    (uint16_t)m->sched_early_full,
                                (uint16_t)m->sched_replaced, (uint16_t)m->sched_cooling, m->align_phase_us,
                                m->align_flags};
    rdof_ops_put_status_ext(b, &st);
    split_reply(m, 6, 0, b, sizeof b);
}

/* ALIGN (rdof/splitops.h): slot `counter` starts at modem µs t_us. Unkeyed the slot clock snaps there, at once (the
 * reply says 04 as RadioFoundry's does; STATUS 46 already shows it done); keyed only the counter is taken, if the slot clock
 * is within 1 ms of it on the same parity, else result 0 with flag 10. Either stream mode. */
static void split_align(rdof_sdrmodem_t *m, const uint8_t *d, size_t n, int64_t now)
{
    const uint8_t sub = RDOF_OPS_SUB_ALIGN;
    uint8_t b[5];
    if (m->role != ROLE_TX) {
        split_reply(m, sub, 2, NULL, 0);
        return;
    }
    const uint32_t t_us = n >= 12 ? le32(d) : 0, period = n >= 12 ? le32(d + 4) : 0, c = n >= 12 ? le32(d + 8) : 0;
    if (n < 12 || (period && (period < 29990000u || period > 30010000u))) {
        split_reply(m, sub, 4, NULL, 0);
        return;
    }
    const int64_t slot = rdof_mod_slot_samples(&m->mod), now_tx = now * (int64_t)m->tx_fs / 48000;
    const double target = (double)now_tx + (double)(int32_t)(t_us - us_tx(m, now_tx)) * m->tx_fs / 1e6;
    int64_t j = 0;
    double phase = 0;
    if (m->clock_running) { /* the slot clock's boundary nearest the target */
        j = llround((target - (double)m->base) / (double)slot);
        phase = ((double)(m->base + j * slot) - target) * 1e6 / m->tx_fs;
    }
    int flags;
    if (!m->keyed) {
        double per = period ? period / 30e6 : m->align_target ? m->slot_p / (double)slot : 1.0;
        m->align_target = true;
        m->align_pos = target;
        m->align_k = c & 3; /* slot k's parity and 4-slot cycle are its counter's */
        m->slot_p = (double)slot * per;
        m->counter_base = c - (uint32_t)m->align_k;
        if (m->clock_running) /* numbered from it now, not at the next pull */
            place(m, m->mod.pos + rdof_mod_samples(&m->mod, LEAD_SYMBOLS), slot);
        /* 08 too: unlike RadioFoundry's, this slot clock keeps the period (slot_p) */
        flags = RDOF_OPS_SNAPPING | RDOF_OPS_ALIGNED | (period ? RDOF_OPS_PERIOD_APPLIED : 0);
    } else if (fabs(phase) < RDOF_OPS_ALIGNED_US && (((m->counter_base + (uint32_t)j) - c) & 1) == 0) {
        m->counter_base = c - (uint32_t)j;
        flags = RDOF_OPS_ALIGNED;
    } else { /* result 0: the phase tells how far a key-up drifted; nothing changes */
        put_le(b, (uint32_t)(int32_t)lrint(phase), 4);
        b[4] = RDOF_OPS_NOT_APPLIED;
        split_reply(m, sub, 0, b, 5);
        return;
    }
    m->aligned = true;
    m->align_phase_us = (int32_t)lrint(phase);
    m->align_flags = RDOF_OPS_ALIGNED | RDOF_OPS_COUNTER_BASE; /* STATUS 46: done */
    put_le(b, (uint32_t)m->align_phase_us, 4);
    b[4] = (uint8_t)flags;
    split_reply(m, sub, 0, b, 5);
}

static int64_t slot_of(const rdof_sdrmodem_t *m, uint32_t c) { return (int64_t)(int32_t)(c - m->counter_base); }

/* TX_SCHED: a burst for the slot with counter c, in stream mode 1 with a counter base (an ALIGN, snapped back after
 * any retune). */
static void split_sched(rdof_sdrmodem_t *m, const uint8_t *d, size_t n, int64_t now)
{
    const uint8_t sub = RDOF_OPS_SUB_TX_SCHED;
    uint8_t b[5];
    if (n < 6 + RDOF_BURST_BYTES) {
        split_reply(m, sub, 4, NULL, 0);
        return;
    }
    const uint32_t c = le32(d);
    const int slot_no = d[4];
    if (m->role != ROLE_TX || m->stream_mode != RDOF_OPS_STREAM_SCHEDULED || !m->aligned || m->stopped) {
        split_reply(m, sub, 2, NULL, 0);
        return;
    }
    put_le(b, c, 4);
    if (now < m->cool_until48) { /* the cool-down: result 0, outcome 4 */
        m->sched_cooling++;
        b[4] = RDOF_SCHED_FULL;
        split_reply(m, sub, 0, b, 5);
        return;
    }
    if ((slot_no != 1 && slot_no != 2) || (slot_no == 1) != ((c & 1) == 0)) {
        split_reply(m, sub, 4, NULL, 0);
        return;
    }
    const int64_t slot = rdof_mod_slot_samples(&m->mod), now_tx = now * (int64_t)m->tx_fs / 48000;
    const int64_t k = slot_of(m, c), k_now = (now_tx - m->base) >= 0 ? (now_tx - m->base) / slot : -1;
    double start = (double)(m->base + k * slot);
    target_of(m, k, &start);
    int at = -1, used = 0, free_i = -1;
    for (int i = 0; i < RDOF_SDRMODEM_SCHED; i++) {
        if (m->sched[i].used) {
            used++;
            if (m->sched[i].counter == c)
                at = i;
        } else if (free_i < 0) {
            free_i = i;
        }
    }
    const uint8_t control = d[5];
    if (control == (RDOF_SYNC_DATA | RDOF_DT_IDLE) ||
        (control == (RDOF_SYNC_DATA | RDOF_DT_TERMINATOR_WITH_LC) && at >= 0 && m->sched[at].control == control)) {
        if (at >= 0) /* an idle burst or a repeated terminator: "nothing", the place cleared */
            m->sched[at].used = false;
        b[4] = RDOF_SCHED_QUEUED;
        split_reply(m, sub, 0, b, 5);
        return;
    }
    int outcome;
    if (start - (double)now_tx < GUARD_MS * (double)m->tx_fs / 1000) {
        m->sched_late++;
        outcome = RDOF_SCHED_LATE;
    } else if (k - k_now > HORIZON) {
        m->sched_early_full++;
        outcome = RDOF_SCHED_EARLY;
    } else if (at >= 0) {
        m->sched_replaced++;
        outcome = RDOF_SCHED_REPLACED;
    } else if (free_i < 0) {
        m->sched_early_full++;
        outcome = RDOF_SCHED_FULL;
    } else {
        outcome = RDOF_SCHED_QUEUED;
        at = free_i;
    }
    (void)used;
    if (outcome == RDOF_SCHED_QUEUED || outcome == RDOF_SCHED_REPLACED) {
        m->sched[at].used = true;
        m->sched[at].counter = c;
        m->sched[at].control = control;
        memcpy(m->sched[at].burst, d + 6, RDOF_BURST_BYTES);
    }
    b[4] = (uint8_t)outcome;
    split_reply(m, sub, 0, b, 5);
}

static void split_cmd(rdof_sdrmodem_t *m, const uint8_t *p, size_t len)
{
    if (!len) {
        nak(m, ODR_SPLIT, NAK_INVALID, "0xD6 without a sub-command");
        return;
    }
    const int64_t now = clock48(m);
    check_lease(m, now);
    m->role_at48 = now;
    const uint8_t sub = p[0], *d = p + 1;
    const size_t n = len - 1;
    uint8_t b[16];
    switch (sub) {
    case 0: /* INFO */
        split_reply(m, sub, 0, b, split_info(m, now, b));
        break;
    case 1: /* SET_ROLE */
        if (!n || d[0] > 2) {
            split_reply(m, sub, 4, NULL, 0);
            break;
        }
        if (d[0] == ROLE_TX && !can_tx(m)) { /* no transmitter */
            split_reply(m, sub, 2, NULL, 0);
            break;
        }
        const int stream_mode = n > RDOF_OPS_STREAM_MODE_AT ? d[RDOF_OPS_STREAM_MODE_AT] : RDOF_OPS_STREAM_FIFO;
        if (stream_mode > RDOF_OPS_STREAM_SCHEDULED || (stream_mode && d[0] != ROLE_TX)) {
            split_reply(m, sub, 4, NULL, 0);
            break;
        }
        if (d[0] != m->role)
            drop_role(m);
        if (stream_mode != m->stream_mode)
            memset(m->sched, 0, sizeof m->sched);
        m->stream_mode = stream_mode;
        m->role = d[0];
        m->lease_s = n > 1 && d[1] ? d[1] : LEASE_S;
        m->power_cap = n > 2 && d[2] ? d[2] : POWER_CAP;
        m->prebuffer = n > 3 && d[3] ? d[3] : PREBUFFER;
        m->max_key48 = (int64_t)(n > 4 && d[4] ? d[4] * 10 : MAX_KEY_S) * 48000;
        m->cooldown48 = (int64_t)(n > 5 && d[5] ? d[5] : COOLDOWN_S) * 48000;
        split_reply(m, sub, 0, b, split_info(m, now, b));
        break;
    case 2: /* TIME */
        if (n < 8) {
            split_reply(m, sub, 4, NULL, 0);
            break;
        }
        memcpy(b, d, 8);
        put_le(b + 8, us_at(now), 4);
        put_le(b + 12, us_at(clock48(m)), 4);
        split_reply(m, sub, 0, b, 16);
        break;
    case 4: { /* SET_EPOCH */
        if (m->role != ROLE_RX) {
            split_reply(m, sub, 2, NULL, 0);
            break;
        }
        uint32_t period = n >= 8 ? le32(d + 4) : 0;
        if (!period)
            period = 30000000u;
        if (n < 4 || period < 29990000u || period > 30010000u) {
            split_reply(m, sub, 4, NULL, 0);
            break;
        }
        m->has_epoch = true;
        m->epoch_us = le32(d);
        m->period_ns = period;
        m->epoch_at48 = now;
        split_reply(m, sub, 0, NULL, 0);
        break;
    }
    case 6: /* STATUS */
        split_status(m, now);
        break;
    case 3: { /* GET_EPOCH: the latest outbound slot start, on the modem clock */
        if (m->role != ROLE_TX || !m->clock_running) {
            split_reply(m, sub, 2, NULL, 0);
            break;
        }
        const int64_t slot = rdof_mod_slot_samples(&m->mod), now_tx = now * (int64_t)m->tx_fs / 48000;
        int64_t k = now_tx - m->base;
        k = k >= 0 ? k / slot : -((-k + slot - 1) / slot);
        put_le(b, m->counter_base + (uint32_t)k, 4);
        put_le(b + 4, us_tx(m, m->base + k * slot), 4);
        b[8] = (uint8_t)((k & 1) ? 2 : 1); /* slot 1 first */
        b[9] = (uint8_t)((m->keyed ? 0x01 : 0) | 0x02);
        split_reply(m, sub, 0, b, 10);
        break;
    }
    case 5: /* SET_AT */
        if (m->role != ROLE_TX) {
            split_reply(m, sub, 2, NULL, 0);
            break;
        }
        if (!n || d[0] > 3 || (n > 1 && d[1] > 1)) {
            split_reply(m, sub, 4, NULL, 0);
            break;
        }
        m->at_mask = d[0];
        m->at_mode = n > 1 ? d[1] : 0;
        m->has_at = true;
        m->at_at48 = now;
        b[0] = (uint8_t)rdof_sdrmodem_at(m);
        split_reply(m, sub, 0, b, 1);
        break;
    case RDOF_OPS_SUB_ALIGN:
        if (!can_tx(m)) {
            split_reply(m, sub, 1, NULL, 0);
            break;
        }
        split_align(m, d, n, now);
        break;
    case RDOF_OPS_SUB_TX_SCHED:
        if (!can_tx(m)) {
            split_reply(m, sub, 1, NULL, 0);
            break;
        }
        split_sched(m, d, n, now);
        break;
    default:
        split_reply(m, sub, 1, NULL, 0);
    }
}

#else

int rdof_sdrmodem_label(rdof_sdrmodem_t *m, int64_t pos48) { return 0; }
int rdof_sdrmodem_at(const rdof_sdrmodem_t *m) { return rdof_sdrmodem_busy(m); }

#endif /* RDOF_FEATURE_REPEATER */

static void status(rdof_sdrmodem_t *m)
{
    uint8_t p[10] = {0};
    p[0] = RDOF_DMR_ENABLED;
    p[1] = (uint8_t)m->mode;
    p[2] = (uint8_t)((rdof_sdrmodem_transmitting(m) ? 0x01 : 0) |
                     (m->in_call[0] || m->in_call[1] || m->now48 - m->last48[0] < 4800 ||
                              m->now48 - m->last48[1] < 4800
                          ? 0x40
                          : 0));
    p[4] = (uint8_t)(m->role == ROLE_RX ? 0 : m->duplex || m->dcdm ? space(m, 0) : 10);
    p[5] = (uint8_t)(m->role == ROLE_RX ? 0 : space(m, 1));
    send(m, RDOF_MMDVM_GET_STATUS, p, sizeof p);
}

static void on_frame(void *ctx, uint8_t cmd, const uint8_t *p, size_t len)
{
    rdof_sdrmodem_t *m = ctx;
#if RDOF_FEATURE_REPEATER
    if (cmd == ODR_SPLIT) {
        split_cmd(m, p, len);
        return;
    }
    if (m->role == ROLE_RX) { /* receive only (split-duplex.md 4.2) */
        check_lease(m, clock48(m));
        if (cmd == RDOF_MMDVM_DMR_DATA1 || cmd == RDOF_MMDVM_DMR_DATA2) {
            nak(m, cmd, NAK_WRONG_MODE, "DMR data to the RX half");
            return;
        }
        if (cmd == RDOF_MMDVM_DMR_START || cmd == RDOF_MMDVM_DMR_SHORTLC || cmd == RDOF_MMDVM_DMR_ABORT) {
            ack(m, cmd);
            return;
        }
    }
#endif
    switch (cmd) {
    case RDOF_MMDVM_GET_VERSION: {
        uint8_t v[1 + sizeof RDOF_SDRMODEM_DESC] = {1};
        memcpy(v + 1, RDOF_SDRMODEM_DESC, sizeof RDOF_SDRMODEM_DESC - 1);
        send(m, cmd, v, sizeof v - 1);
        break;
    }
    case RDOF_MMDVM_GET_STATUS:
        status(m);
        break;
    case RDOF_MMDVM_SET_CONFIG:
        if (len < 7) {
            nak(m, cmd, NAK_INVALID, "SET_CONFIG too short");
            break;
        }
        if (!(p[0] & RDOF_MMDVM_SIMPLEX) && !m->can_full && !m->role) {
            nak(m, cmd, NAK_INVALID, "duplex asked for, but this front end is half duplex");
            break;
        }
        m->duplex = !(p[0] & RDOF_MMDVM_SIMPLEX);
        if (m->dcdm != (!m->duplex && (p[0] & RDOF_MMDVM_DCDM) && !m->role))
            m->dc_timed = false;
        m->dcdm = !m->duplex && (p[0] & RDOF_MMDVM_DCDM) && !m->role;
        m->mode = p[3];
        m->cc = p[6] & 0x0F;
        ack(m, cmd);
        break;
    case RDOF_MMDVM_SET_MODE:
        if (len < 1 || (p[0] != RDOF_MODE_IDLE && p[0] != RDOF_MODE_DMR)) {
            nak(m, cmd, NAK_INVALID, "a mode other than idle or DMR");
            break;
        }
        m->mode = p[0];
        ack(m, cmd);
        break;
    case RDOF_MMDVM_SET_FREQ: {
        if (len < 9) {
            nak(m, cmd, NAK_INVALID, "SET_FREQ too short");
            break;
        }
        uint32_t rx = le32(p + 1), tx = le32(p + 5);
        int ch = m->io.rx_channel ? m->io.rx_channel(m->io.ctx, rx) : -1;
        double off = 0;
        if (ch < 0 && m->role != ROLE_TX) {
            char e[64];
            snprintf(e, sizeof e, "%.5f MHz isn't one of the SDR's channels", rx / 1e6);
            nak(m, cmd, NAK_INVALID, e);
            break;
        }
        if (m->role != ROLE_RX && (!m->io.tx_tune || !m->io.tx_tune(m->io.ctx, tx, &off))) {
            char e[64];
            snprintf(e, sizeof e, "the front end can't transmit on %.5f MHz", tx / 1e6);
            nak(m, cmd, NAK_INVALID, e);
            break;
        }
        m->channel = m->role == ROLE_TX ? -1 : ch;
        m->rx_hz = rx;
        m->tx_hz = tx;
        m->tuned = true;
        m->tune_off_hz = off;
        m->dc_timed = false; /* another channel, its own timing */
        retune_carrier(m);
        if (m->role == ROLE_TX) { /* a retune restarts the slot clock (split-duplex.md 3.4): ALIGN again */
            m->slot_k = -1;
            m->clock_running = false;
            m->aligned = false; /* snapped back onto the last ALIGN's target at the next pull */
            m->align_flags = m->align_target ? RDOF_OPS_SNAPPING : 0;
        }
        ack(m, cmd);
        break;
    }
    case RDOF_MMDVM_SEND_CWID:
        ack(m, cmd); /* taken, not sent: no CW keyer here */
        break;
    case RDOF_MMDVM_DMR_DATA1:
    case RDOF_MMDVM_DMR_DATA2: {
        int s = cmd == RDOF_MMDVM_DMR_DATA1 ? 0 : 1;
        if (len < 1 + RDOF_BURST_BYTES) {
            nak(m, cmd, NAK_INVALID, "DMR data too short");
            break;
        }
        if (s == 0 && !m->duplex && !m->dcdm) {
            nak(m, cmd, NAK_WRONG_MODE, "slot 1 data on a simplex modem");
            break;
        }
        if (m->mode != RDOF_MODE_DMR || !m->tuned) {
            nak(m, cmd, NAK_WRONG_MODE, "not in DMR mode, or not tuned");
            break;
        }
        if (m->stopped || (m->role == ROLE_TX && m->stream_mode == RDOF_OPS_STREAM_SCHEDULED)) {
            nak(m, cmd, NAK_WRONG_MODE, m->stopped ? "the slot timing gate stopped transmitting"
                                                   : "DMR data in the scheduled stream mode");
            break;
        }
        if (!space(m, s)) {
            nak(m, cmd, NAK_NO_SPACE, "transmit queue full");
            break;
        }
        if (!m->qn[s])
            m->wait48[s] = m->now48;
        memcpy(m->q[s][(m->qh[s] + m->qn[s]) % RDOF_SDRMODEM_QUEUE].b, p + 1, RDOF_BURST_BYTES);
        m->qn[s]++;
        break;
    }
    case RDOF_MMDVM_DMR_START:
        if (!m->duplex) {
            nak(m, cmd, NAK_WRONG_MODE, "DMR_START on a simplex modem");
            break;
        }
        if (len >= 1 && p[0]) {
            m->queue_keyed = false;
            if (!m->keyed && !m->stopped && !(m->role == ROLE_TX && m->now48 < m->cool_until48)) {
                m->keyed = true;
                m->unkey = false;
                m->ramp_up = true;
                m->keyed_at48 = m->now48;
                if (m->role != ROLE_TX)
                    m->slot_k = -1; /* placed at the next pull; the TX half's slot clock runs on */
            }
        } else if (m->keyed) {
            m->unkey = true;
        }
        break;
    case RDOF_MMDVM_DMR_ABORT:
        if (len >= 1 && (p[0] == 1 || p[0] == 2))
            m->qn[p[0] - 1] = 0;
        break;
    case RDOF_MMDVM_DMR_SHORTLC: {
#if RDOF_FEATURE_REPEATER
        uint8_t sl9[9];
        if (len < 9 || !rdof_short_lc_accept(p, sl9)) {
            m->bad_short_lcs++; /* the previous one stays */
            break;
        }
        rdof_short_lc_rows(sl9, m->slc_next);
        m->short_lcs++;
#endif
        break;
    }
    default:
        nak(m, cmd, NAK_UNKNOWN, "unknown command");
    }
}

void rdof_sdrmodem_write(rdof_sdrmodem_t *m, const uint8_t *data, size_t len)
{
    rdof_mmdvm_reader_feed(&m->reader, data, len, on_frame, m);
}

static void check_lost(rdof_sdrmodem_t *m)
{
    for (int i = 0; i < 2; i++)
        if (m->in_call[i] && m->now48 - m->last48[i] > (int64_t)(RDOF_SDRMODEM_LOST_S * 48000)) {
            m->in_call[i] = m->voice[i] = false;
            m->lost++;
            send(m, i == 0 ? RDOF_MMDVM_DMR_LOST1 : RDOF_MMDVM_DMR_LOST2, NULL, 0);
        }
}

int rdof_sdrmodem_busy(const rdof_sdrmodem_t *m)
{
    int mask = 0;
    for (int i = 0; i < 2; i++)
        if (m->voice[i] || m->now48 < m->data48[i])
            mask |= 1 << i;
    return mask;
}

/* Where the modulator would have put symbol 0 of a burst the receiver reports at 48 kS/s position p (heard_at's
 * inverse). */
static int64_t tx_at(const rdof_sdrmodem_t *m, int64_t p) { return (p - RX_DELAY_48K(m)) * (int64_t)m->tx_fs / 48000; }

/* DCDM: a TS1 (i 0) or TS2 (i 1) burst heard at p. The channel's slot timing, unless ours is in use: a burst of ours
 * queued or sent within 60 ms before it (the other station keeps to ours then, and moving would cost a slot). */
static void dc_heard(rdof_sdrmodem_t *m, int i, int64_t p)
{
    const int64_t slot = rdof_mod_slot_samples(&m->mod), start = tx_at(m, p);
    if (m->dc_last != INT64_MIN && m->dc_last + 2 * slot >= start) {
        if (start > m->dc_seen)
            m->dc_seen = start;
        return;
    }
    m->dc_t0 = start - (i ? slot : 0);
    m->dc_timed = true;
    m->dc_timings++;
    if (start > m->dc_seen)
        m->dc_seen = start;
}

/* A data packet has no terminator: its header says how many blocks follow, and the last one ends the call (no
 * carrier and no DMR_LOST after it; a host replying at once, an ACK, finds the channel clear). A header with no
 * blocks (a response) is over at once; a header while blocks are due starts the next packet (a chain). */
static bool data_over(rdof_sdrmodem_t *m, int i, const rdof_burst_t *b)
{
    if (b->dt == RDOF_DT_DATA_HEADER) {
        rdof_data_header_t h;
        rdof_decode_data_header(b->burst, &h);
        if (!h.crc_ok) {
            m->data_left[i] = 0;
            return false;
        }
        if (h.dpf == RDOF_DPF_PROPRIETARY && m->data_left[i] > 0) /* a second header: one of the blocks */
            return --m->data_left[i] == 0;
        m->data_left[i] = h.blocks > 0 ? h.blocks : 0;
        return m->data_left[i] == 0;
    }
    if (b->dt == RDOF_DT_RATE_12 || b->dt == RDOF_DT_RATE_34 || b->dt == RDOF_DT_RATE_1)
        return m->data_left[i] > 0 && --m->data_left[i] == 0;
    m->data_left[i] = 0;
    return false;
}

void rdof_sdrmodem_heard(rdof_sdrmodem_t *m, int channel, const rdof_sdr_burst_t *sb)
{
    const rdof_burst_t *b = &sb->b;
    if (sb->position > m->now48)
        m->now48 = sb->position;
    if (channel != m->channel || m->channel < 0 || m->mode != RDOF_MODE_DMR || b->dt == RDOF_DT_IDLE)
        return;
    if (rdof_sdrmodem_muted_at(m, sb->position)) {
        m->muted++;
        return;
    }
    uint8_t p[1 + RDOF_BURST_BYTES];
    if (b->dt == RDOF_DT_VOICE_SYNC)
        p[0] = RDOF_SYNC_AUDIO;
    else if (b->dt == RDOF_DT_VOICE)
        p[0] = (uint8_t)(b->n & 0x0F);
    else
        p[0] = (uint8_t)(RDOF_SYNC_DATA | (b->dt & 0x0F));
    memcpy(p + 1, b->burst, RDOF_BURST_BYTES);
    int i = m->duplex && b->slot == 1 ? 0 : 1;
    if (m->dcdm) { /* by its sync: TS1 is slot 1; TS2, and MS (plain direct mode), slot 2 */
        i = b->family == RDOF_FAMILY_TS1 ? 0 : 1;
        if (b->family == RDOF_FAMILY_TS1 || b->family == RDOF_FAMILY_TS2)
            dc_heard(m, i, sb->position);
    }
#if RDOF_FEATURE_REPEATER
    check_lease(m, m->now48);
    int64_t gn;
    int gts = 0;
    if (rdof_sdrmodem_utc_slot(m, sb->position, &gn, &gts, NULL)) { /* UTC timed: the grid labels it */
        m->grid_labeled++;
        if (m->duplex && !m->role)
            i = gts == 1 ? 0 : 1;
    }
    if (m->role) {
        int label = gts ? gts : label_of(m, sb->position);
        if (label)
            m->labeled[label - 1]++;
        else
            m->unlabeled++; /* no epoch: up as DMR_DATA2 */
        i = label == 1 ? 0 : 1;
    }
#endif
    m->rx_bursts++;
    m->last48[i] = sb->position;
    m->in_call[i] = b->dt != RDOF_DT_TERMINATOR_WITH_LC && !data_over(m, i, b);
    if (b->dt == RDOF_DT_TERMINATOR_WITH_LC)
        m->voice[i] = false;
    else if (b->dt == RDOF_DT_VOICE_LC_HEADER || b->dt == RDOF_DT_VOICE_SYNC || b->dt == RDOF_DT_VOICE)
        m->voice[i] = true;
    else
        m->data48[i] = sb->position + DATA_BUSY_48K;
    send(m, i == 0 ? RDOF_MMDVM_DMR_DATA1 : RDOF_MMDVM_DMR_DATA2, p, sizeof p);
}

/* Simplex: the next queued burst, 60 ms after the last or as soon as it can go. */
static void queue_simplex(rdof_sdrmodem_t *m, int64_t pos, int n)
{
    const int64_t lead = rdof_mod_samples(&m->mod, LEAD_SYMBOLS), two = 2 * rdof_mod_slot_samples(&m->mod);
    while (m->qn[1]) {
        int64_t start = m->next_start > pos + lead ? m->next_start : pos + lead;
        if (start > pos + n + two)
            break; /* not due yet */
        if (rdof_mod_queue_burst(&m->mod, start, m->q[1][m->qh[1]].b, NULL, RDOF_MOD_RAMPS) != 0)
            break; /* the modulator's queue is full */
        m->qh[1] = (m->qh[1] + 1) % RDOF_SDRMODEM_QUEUE;
        m->qn[1]--;
        m->next_start = start + two;
        m->tx_bursts++;
        remember(m, start);
    }
}

/* DCDM: each slot's next queued burst in its own slot of the timing, 60 ms after its last, the earliest first (the
 * modulator takes them in order). With no timing, or one unused for DC_STALE_48K, the first burst sets it. */
static void queue_dcdm(rdof_sdrmodem_t *m, int64_t pos, int n)
{
    const int64_t lead = rdof_mod_samples(&m->mod, LEAD_SYMBOLS), slot = rdof_mod_slot_samples(&m->mod), two = 2 * slot;
    const int64_t stale = (int64_t)DC_STALE_48K * (int64_t)m->tx_fs / 48000;
    for (;;) {
        int best = -1;
        int64_t at = 0;
        for (int s = 0; s < 2; s++) {
            if (!m->qn[s])
                continue;
            int64_t from = pos + lead;
            if (m->dc_next[s] > from)
                from = m->dc_next[s];
            if (m->dc_last != INT64_MIN && m->dc_last >= from)
                from = m->dc_last + 1;
            if (!m->dc_timed || from - m->dc_seen > stale) {
                m->dc_t0 = from - (s ? slot : 0);
                m->dc_timed = true;
                m->dc_seen = from;
            }
            const int64_t base = m->dc_t0 + (s ? slot : 0), d = from - base;
            const int64_t t = base + (d > 0 ? (d + two - 1) / two : -(-d / two)) * two;
            if (best < 0 || t < at)
                best = s, at = t;
        }
        if (best < 0 || at > pos + n + slot)
            break; /* nothing queued, or not due yet */
        if (rdof_mod_queue_burst(&m->mod, at, m->q[best][m->qh[best]].b, NULL, RDOF_MOD_RAMPS) != 0)
            break; /* the modulator's queue is full */
        m->qh[best] = (m->qh[best] + 1) % RDOF_SDRMODEM_QUEUE;
        m->qn[best]--;
        m->dc_next[best] = at + two;
        m->dc_last = at;
        if (at > m->dc_seen)
            m->dc_seen = at;
        m->tx_bursts++;
        remember(m, at);
    }
}

void rdof_sdrmodem_cach(rdof_sdrmodem_t *m, long k, uint8_t out3[3])
{
#if RDOF_FEATURE_REPEATER
    static const int lcss[4] = {1, 3, 3, 2}; /* first, continuation, continuation, last */
    const int s = (int)(k & 1), q = (int)(k % 4);
    if (q == 0)
        memcpy(m->slc, m->slc_next, sizeof m->slc); /* a new short LC starts on a multiple of 4 */
    uint8_t bits[24];
    rdof_encode_cach_lc(s + 1, (rdof_sdrmodem_at(m) >> s) & 1, lcss[q], m->slc[q], bits);
    memset(out3, 0, 3);
    for (int i = 0; i < 24; i++)
        out3[i / 8] |= (uint8_t)((bits[i] & 1) << (7 - i % 8));
#else
    (void)m, (void)k;
    memset(out3, 0, 3);
#endif
}

#if RDOF_FEATURE_REPEATER
static bool sched_pending(const rdof_sdrmodem_t *m)
{
    for (int i = 0; i < RDOF_SDRMODEM_SCHED; i++)
        if (m->sched[i].used)
            return true;
    return false;
}

/* Slot counter c's scheduled burst, taken; the ones before it, gone by, dropped as late. */
static bool sched_take(rdof_sdrmodem_t *m, uint32_t c, uint8_t burst[RDOF_BURST_BYTES])
{
    bool found = false;
    for (int i = 0; i < RDOF_SDRMODEM_SCHED; i++) {
        if (!m->sched[i].used)
            continue;
        int32_t d = (int32_t)(m->sched[i].counter - c);
        if (d == 0) {
            memcpy(burst, m->sched[i].burst, RDOF_BURST_BYTES);
            m->sched[i].used = false;
            m->sched_sent++;
            m->sched_last48 = m->now48;
            found = true;
        } else if (d < 0) {
            m->sched[i].used = false;
            m->sched_late++;
        }
    }
    return found;
}

static void sched_put_back(rdof_sdrmodem_t *m, uint32_t c, const uint8_t burst[RDOF_BURST_BYTES])
{
    for (int i = 0; i < RDOF_SDRMODEM_SCHED; i++)
        if (!m->sched[i].used) {
            m->sched[i].used = true;
            m->sched[i].counter = c;
            memcpy(m->sched[i].burst, burst, RDOF_BURST_BYTES);
            return;
        }
}
#endif

/* Duplex: slot after slot while keyed, each with its CACH. */
static void queue_duplex(rdof_sdrmodem_t *m, int64_t pos, int n)
{
#if RDOF_FEATURE_REPEATER
    const int64_t lead = rdof_mod_samples(&m->mod, LEAD_SYMBOLS), slot = rdof_mod_slot_samples(&m->mod),
                  cach = rdof_mod_samples(&m->mod, CACH_SYMBOLS);
    const bool tx_half = m->role == ROLE_TX;
    if ((!m->keyed || m->slot_k < 0) && place(m, pos + lead, slot)) {
        m->clock_running = tx_half; /* on its target (UTC or the host's ALIGN): snapped while unkeyed */
        if (tx_half && m->align_target && !m->aligned) { /* back on the last ALIGN's grid after a retune */
            m->aligned = true;
            m->align_flags = RDOF_OPS_ALIGNED | RDOF_OPS_COUNTER_BASE;
        }
    } else if (m->slot_k < 0) {
        m->base = pos + lead;
        m->slot_k = 0;
        m->clock_running = tx_half;
    } else if (m->base + m->slot_k * slot < pos + lead) {
        if (tx_half) { /* unkeyed, or fell behind: the slot clock runs on */
            m->slot_k = (long)((pos + lead - m->base + slot - 1) / slot);
            if (m->keyed)
                m->ramp_up = true;
        } else { /* fell behind (a skip): carry on from now */
            m->base = pos + lead;
            m->slot_k = 0;
            m->ramp_up = true;
        }
    }
    if (tx_half && m->keyed && m->now48 - m->keyed_at48 > m->max_key48) { /* keyed too long: the cool-down */
        m->unkey = true;
        m->cool_until48 = m->now48 + m->cooldown48;
    }
    const bool scheduled = tx_half && m->stream_mode == RDOF_OPS_STREAM_SCHEDULED;
    if (scheduled && !m->keyed && !m->stopped && m->now48 >= m->cool_until48 && m->slot_k >= 0) {
        /* a queued burst keys up ~150 ms before its slot (split-duplex.md 3.9) */
        for (int i = 0; i < RDOF_SDRMODEM_SCHED; i++) {
            const int64_t k = m->sched[i].used ? slot_of(m, m->sched[i].counter) : -1;
            double t = (double)(m->base + k * slot);
            if (k >= 0)
                target_of(m, k, &t);
            if (k >= 0 && t - (double)pos <= RDOF_OPS_QUEUE_KEY_MS * (double)m->tx_fs / 1000) {
                m->keyed = m->ramp_up = m->queue_keyed = true;
                m->unkey = false;
                m->keyed_at48 = m->sched_last48 = m->now48;
                break;
            }
        }
    }
    if (scheduled && m->keyed && m->queue_keyed && !sched_pending(m) &&
        m->now48 - m->sched_last48 > (int64_t)(RDOF_OPS_QUEUE_UNKEY_S * 48000))
        m->unkey = true; /* the queue keyed it: 1 s after the last scheduled burst */
    while (m->keyed) {
        int64_t at = m->base + m->slot_k * slot;
        if (at > pos + n + slot)
            break;
        if (!m->ramp_up) { /* keyed: steered toward the target a sample a slot */
            steer(m, slot);
            at = m->base + m->slot_k * slot;
        }
        const int s = (int)(m->slot_k & 1); /* slot 1 first */
        uint8_t burst[RDOF_BURST_BYTES], c3[3];
        if (scheduled) { /* TX_SCHED's burst for this slot's counter, else idle */
            bool found = sched_take(m, m->counter_base + (uint32_t)m->slot_k, burst);
            if (!found)
                rdof_duplex_idle(m->cc, burst);
            m->tx_active[s] = found;
            rdof_sdrmodem_cach(m, m->slot_k, c3);
            unsigned flags = (m->ramp_up ? RDOF_MOD_RAMP_UP : 0) | (m->unkey ? RDOF_MOD_RAMP_DOWN : 0);
            if (rdof_mod_queue_burst(&m->mod, at + cach, burst, c3, flags) != 0) {
                if (found)
                    m->sched_sent--, sched_put_back(m, m->counter_base + (uint32_t)m->slot_k, burst);
                break;
            }
            m->ramp_up = false;
            m->cach_bursts++;
            if (found)
                m->tx_bursts++;
            else
                m->idle_bursts++;
            m->slot_k++;
            if (m->unkey) {
                m->keyed = m->unkey = false;
                m->tx_active[0] = m->tx_active[1] = false;
            }
            continue;
        }
        if (tx_half && !m->streaming[s] && m->qn[s] &&
            (m->qn[s] >= m->prebuffer || m->now48 - m->wait48[s] >= (int64_t)m->prebuffer * 2880))
            m->streaming[s] = true; /* the prebuffer's in (or its time, 60 ms a burst, is up) */
        if (tx_half && m->streaming[s] && !m->qn[s]) {
            m->streaming[s] = false;
            if (!m->last_term[s])
                m->underruns[s]++;
        }
        bool idle = !m->qn[s] || (tx_half && !m->streaming[s]);
        if (idle)
            rdof_duplex_idle(m->cc, burst);
        else
            memcpy(burst, m->q[s][m->qh[s]].b, RDOF_BURST_BYTES);
        m->tx_active[s] = !idle;
        rdof_sdrmodem_cach(m, m->slot_k, c3);
        unsigned flags = (m->ramp_up ? RDOF_MOD_RAMP_UP : 0) | (m->unkey ? RDOF_MOD_RAMP_DOWN : 0);
        if (rdof_mod_queue_burst(&m->mod, at + cach, burst, c3, flags) != 0)
            break;
        m->ramp_up = false;
        m->cach_bursts++;
        if (!idle) {
            int cc, dt, e;
            m->last_term[s] = rdof_sync_errors(burst, rdof_data_sync(RDOF_BS_VOICE)) <= 4 &&
                              rdof_decode_slot_type(burst, &cc, &dt, &e) && dt == RDOF_DT_TERMINATOR_WITH_LC;
            m->qh[s] = (m->qh[s] + 1) % RDOF_SDRMODEM_QUEUE;
            m->qn[s]--;
            m->tx_bursts++;
        } else {
            m->idle_bursts++;
        }
        m->slot_k++;
        if (m->unkey) {
            m->keyed = m->unkey = false;
            m->tx_active[0] = m->tx_active[1] = false;
            if (!tx_half)
                m->slot_k = -1;
        }
    }
#else
    m->keyed = false;
#endif
}

static void advance(rdof_sdrmodem_t *m, int64_t pos, int n)
{
    if (pos > m->mod.pos) { /* the front end skipped samples: let the modulator's clock catch up */
        rdof_cf_t junk[64];
        while (m->mod.pos < pos) {
            int64_t k = pos - m->mod.pos;
            rdof_mod_iq(&m->mod, junk, k < 64 ? (int)k : 64);
        }
    }
    pos = m->mod.pos;
#if RDOF_FEATURE_REPEATER
    check_lease(m, clock48(m));
#endif
    if (m->duplex || m->role == ROLE_TX) /* the TX half's slot clock runs from SET_ROLE */
        queue_duplex(m, pos, n);
    else if (m->dcdm)
        queue_dcdm(m, pos, n);
    else
        queue_simplex(m, pos, n);
    int64_t t = (pos + n) * 48000 / (int64_t)m->tx_fs;
    if (t > m->now48)
        m->now48 = t;
    check_lost(m);
}

static bool keyed_in(const rdof_sdrmodem_t *m, const float *env, const rdof_cf_t *iq, int n)
{
    for (int i = 0; i < n; i++)
        if ((env && env[i] > 0) || (iq && (iq[i].re != 0 || iq[i].im != 0)))
            return true;
    return false;
}

bool rdof_sdrmodem_pull_iq(rdof_sdrmodem_t *m, int64_t pos, rdof_cf_t *iq, int n)
{
    if (!m->mod_ok)
        return false;
    advance(m, pos, n);
    rdof_mod_iq(&m->mod, iq, n);
    m->pos = m->mod.pos;
    return keyed_in(m, NULL, iq, n);
}

bool rdof_sdrmodem_pull_freq(rdof_sdrmodem_t *m, int64_t pos, float *hz, float *env, int n)
{
    if (!m->mod_ok)
        return false;
    advance(m, pos, n);
    rdof_mod_freq(&m->mod, hz, env, n);
    m->pos = m->mod.pos;
    return keyed_in(m, env, NULL, n);
}

#endif /* RDOF_FEATURE_SDR */
