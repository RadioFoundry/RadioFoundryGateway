// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* A fake radio for the host tests (fake_radio.h). */
#include "fake_radio.h"
#include "rdof/splitops.h"

#include <math.h>
#include <string.h>

static const struct {
    const char *kind;
    int protocol;
    const char *description;
} VERSIONS[] = {
    {"opengd77", 1, "OpenGD77_HS v0.1.18 GitID #UNKNOWN (Radio:DM-1701, Mode:MMDVM)"},
    {"opengd77_sfr", 1, "OpenGD77_HS v0.1.18 GitID #UNKNOWN (Radio:DM-1701, Mode:MMDVM) SFR TS1>TS2 TS"},
    {"odr", 1, "OpenDigitalRadio DM-1701 2d45a21b"},
    {"odr_sfr", 1, "OpenDigitalRadio DM-1701 2d45a21b SFR TS1>TS2 TS"},
    {"odr_split", 1, "OpenDigitalRadio DM-1701 2d45a21b SPLIT1"},
    {"odr_dcdm", 1, "OpenDigitalRadio DM-1701 2d45a21b DCDM"},
    {"mmdvm_hs", 1, "MMDVM_HS_Dual_Hat-v1.5.2 20201108 14.7456MHz dual ADF7021 FW by CA6JAU GitID #89daa20"},
    {"mmdvm", 2, "MMDVM 20200901 TCXO (D-Star/DMR/System Fusion/P25/NXDN/POCSAG/FM)"},
};

static int config_min(const char *kind)
{
    if (!strcmp(kind, "mmdvm_hs"))
        return 23;
    if (!strcmp(kind, "mmdvm"))
        return 37;
    return 13;
}

/* A radio_hotspot fake: OpenGD77 and RadioFoundry answer alike. */
static bool is_hotspot(const fake_radio_t *f)
{
    return !strncmp(f->kind, "opengd77", 8) || !strncmp(f->kind, "odr", 3);
}

void fake_init(fake_radio_t *f, const char *kind)
{
    memset(f, 0, sizeof *f);
    strncpy(f->kind, kind, sizeof f->kind - 1);
    for (size_t i = 0; i < sizeof VERSIONS / sizeof VERSIONS[0]; i++)
        if (!strcmp(VERSIONS[i].kind, kind)) {
            f->protocol = VERSIONS[i].protocol;
            strcpy(f->description, VERSIONS[i].description);
        }
    f->band_lo = 400000000;
    f->band_hi = 480000000;
    f->split_version = 1;
    f->split_caps = RDOF_OPS_SCHEDULE | RDOF_OPS_ALIGN; /* RadioFoundry's phase 1 */
    f->guard_ms = 55; /* the HR_C7000's */
    f->snap_slots = 2.5;
    f->horizon = RDOF_OPS_HORIZON_SLOTS;
    f->sched_cap = 16;
    f->slot_period_us = 30000.0;
    rdof_mmdvm_reader_init(&f->reader);
}

static void put(fake_radio_t *f, uint8_t cmd, const uint8_t *p, size_t n)
{
    uint8_t fr[300];
    size_t len = rdof_mmdvm_frame(cmd, p, n, fr, sizeof fr);
    if (f->out_len + len <= FAKE_OUT) {
        memcpy(f->out + f->out_len, fr, len);
        f->out_len += len;
    }
}

static void ack(fake_radio_t *f, uint8_t cmd)
{
    put(f, RDOF_MMDVM_ACK, &cmd, 1);
}

static void nak(fake_radio_t *f, uint8_t cmd, uint8_t err)
{
    uint8_t p[2] = {cmd, err};
    put(f, RDOF_MMDVM_NAK, p, 2);
}

static uint32_t le32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool is_split(const fake_radio_t *f)
{
    return !strcmp(f->kind, "odr_split");
}

static double modem_f(const fake_radio_t *f)
{
    return *f->clock * 1e6 * (1.0 + f->clock_rate) + f->clock_offset_us;
}

static uint32_t modem_us(const fake_radio_t *f)
{
    return (uint32_t)(uint64_t)(int64_t)rint(modem_f(f));
}

double fake_origin(const fake_radio_t *f)
{
    return f->slot_origin;
}

long fake_slot_boundary(const fake_radio_t *f, double now_f, double *b)
{
    double o = fake_origin(f);
    long k = (long)floor((now_f - o) / f->slot_period_us);
    *b = o + k * f->slot_period_us;
    return k;
}

double fake_modem_f(const fake_radio_t *f)
{
    return modem_f(f);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void put_le16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static int32_t wrap32(uint32_t d)
{
    return (int32_t)d;
}

static void snap(fake_radio_t *f, double now)
{
    double p = f->grid_period;
    long j = (long)floor((now - f->grid_target) / p) + 1; /* the grid's next boundary */
    j += (long)((f->grid_counter + (uint32_t)j) & 1);      /* a TS1 one: slot_origin stays a TS1 boundary */
    f->slot_origin = f->grid_target + j * p + f->snap_offset_us;
    f->counter_base = f->grid_counter + (uint32_t)j;
    f->has_snap = true;
    f->snap_until = now + f->snap_slots * p;
}

void fake_air(fake_radio_t *f)
{
    if (f->role != 1)
        return;
    double now = modem_f(f);
    if (f->has_snap && now >= f->snap_until) {
        f->has_snap = false;
        f->aligned = true;
        f->align_phase = (int32_t)lrint(f->snap_offset_us);
        f->align_flags = RDOF_OPS_COUNTER_BASE | (fabs(f->snap_offset_us) < RDOF_OPS_ALIGNED_US ? RDOF_OPS_ALIGNED : 0);
    }
    if (f->has_grid && !f->aligned && !f->has_snap && !f->duplex_tx) {
        snap(f, now); /* after a retune: back onto the last grid */
        f->align_flags = RDOF_OPS_SNAPPING;
    }
    for (;;) { /* the earliest queued slot first */
        int best = -1;
        for (int i = 0; i < 32; i++)
            if (f->sched[i].used &&
                (best < 0 || wrap32(f->sched[i].counter - f->counter_base) <
                                 wrap32(f->sched[best].counter - f->counter_base)))
                best = i;
        if (best < 0)
            break;
        double start = f->slot_origin + wrap32(f->sched[best].counter - f->counter_base) * f->slot_period_us;
        if (start > now) {
            if (!f->duplex_tx && start - now <= RDOF_OPS_QUEUE_KEY_MS * 1000.0)
                f->duplex_tx = f->transmitting = f->queue_keyed = true;
            break;
        }
        if (f->non_air < 64) {
            f->on_air[f->non_air].counter = f->sched[best].counter;
            f->on_air[f->non_air].slot = f->sched[best].slot;
            memcpy(f->on_air[f->non_air].burst, f->sched[best].burst, 33);
            f->on_air[f->non_air].start = start;
            f->non_air++;
        }
        f->sched[best].used = false;
        f->sched_sent++;
        f->has_last_aired = true;
        f->last_aired = start;
    }
    bool queued = false;
    for (int i = 0; i < 32; i++)
        queued |= f->sched[i].used;
    if (f->queue_keyed && f->duplex_tx && !queued && f->has_last_aired &&
        now - f->last_aired > RDOF_OPS_QUEUE_UNKEY_S * 1e6)
        f->duplex_tx = f->transmitting = f->queue_keyed = false;
}

static void split_reply(fake_radio_t *f, uint8_t sub, uint8_t result, const uint8_t *body, size_t n)
{
    uint8_t p[64];
    p[0] = sub;
    p[1] = result;
    if (n)
        memcpy(p + 2, body, n);
    put(f, 0xD6, p, n + 2);
}

static size_t split_info(const fake_radio_t *f, uint8_t *b)
{
    b[0] = 1, b[1] = (uint8_t)(f->roles ? f->roles : 0x03), b[2] = (uint8_t)f->role, b[3] = 16, b[4] = 1;
    put_le32(b + 5, modem_us(f));
    b[9] = (f->has_epoch ? 0x01 : 0) | (f->role == 1 ? 0x08 : 0);
    b[0] = (uint8_t)f->split_version;
    if (f->no_caps)
        return 10;
    put_le16(b + RDOF_OPS_CAPS_AT, f->split_caps);
    b[RDOF_OPS_GUARD_AT] = (uint8_t)f->guard_ms;
    b[RDOF_OPS_HORIZON_AT] = (uint8_t)f->horizon;
    put_le16(b + RDOF_OPS_AIR_DELAY_AT, (uint32_t)(uint16_t)(int16_t)f->air_delay_us);
    return RDOF_OPS_INFO_LEN;
}

/* The 0xD6 sub-commands. */
static void split_cmd(fake_radio_t *f, const uint8_t *p, size_t len)
{
    uint8_t sub = p[0], b[RDOF_OPS_STATUS_LEN];
    const uint8_t *d = p + 1;
    size_t n = len - 1;
    fake_air(f);
    switch (sub) {
    case 0: split_reply(f, sub, 0, b, split_info(f, b)); return;
    case 1:
        if (!n || d[0] > 2) {
            split_reply(f, sub, 4, NULL, 0);
            return;
        }
        if (d[0] && !((f->roles ? f->roles : 0x03) & (1 << (d[0] - 1)))) {
            split_reply(f, sub, 2, NULL, 0); /* a role it doesn't offer */
            return;
        }
        if (d[0] != f->role) {
            f->has_epoch = false;
            f->slot_origin = d[0] == 1 ? modem_f(f) : 0;
            f->aligned = f->has_grid = f->has_snap = false;
            memset(f->sched, 0, sizeof f->sched);
        }
        f->role = d[0];
        f->stream_mode = n > RDOF_OPS_STREAM_MODE_AT ? d[RDOF_OPS_STREAM_MODE_AT] : 0;
        split_reply(f, sub, 0, b, split_info(f, b));
        return;
    case 2: {
        memcpy(b, d, 8);
        uint32_t t = modem_us(f);
        put_le32(b + 8, t);
        put_le32(b + 12, t);
        split_reply(f, sub, 0, b, 16);
        return;
    }
    case 3:
        if (f->role != 1 || f->epoch_refused) {
            f->epoch_refused = f->epoch_refused > 0 ? f->epoch_refused - 1 : 0;
            split_reply(f, sub, 2, NULL, 0);
            return;
        }
    {
        double bnd;
        long k = fake_slot_boundary(f, modem_f(f), &bnd);
        uint32_t c = f->counter_base + (uint32_t)k;
        put_le32(b, c);
        put_le32(b + 4, (uint32_t)(uint64_t)(int64_t)rint(bnd));
        f->epoch_at = *f->clock;
        b[8] = (uint8_t)(1 + c % 2), b[9] = 0x02;
        split_reply(f, sub, 0, b, 10);
        return;
    }
    case 4:
        if (f->role != 2 || n < 4) {
            split_reply(f, sub, 2, NULL, 0);
            return;
        }
        f->has_epoch = true;
        f->epoch_t_us = le32(d);
        f->epoch_period = n >= 8 ? le32(d + 4) : 30000000u;
        split_reply(f, sub, 0, NULL, 0);
        return;
    case 5:
        if (f->role != 1) {
            split_reply(f, sub, 2, NULL, 0);
            return;
        }
        split_reply(f, sub, 0, d, 1);
        return;
    case 6: {
        memset(b, 0, RDOF_OPS_STATUS_LEN);
        b[0] = (uint8_t)f->role, b[1] = f->duplex_tx ? 0x02 : 0;
        const rdof_ops_status_t st = {(uint16_t)f->sched_sent, (uint16_t)f->sched_late, (uint16_t)f->sched_early_full,
                                    (uint16_t)f->sched_replaced, (uint16_t)f->sched_cooling, f->align_phase,
                                    f->align_flags};
        rdof_ops_put_status_ext(b, &st);
        split_reply(f, sub, 0, b, RDOF_OPS_STATUS_LEN);
        return;
    }
    case RDOF_OPS_SUB_ALIGN: {
        if (f->no_caps || !(f->split_caps & RDOF_OPS_ALIGN))
            break;
        if (f->role != 1) {
            split_reply(f, sub, 2, NULL, 0);
            return;
        }
        if (n < 12) {
            split_reply(f, sub, 4, NULL, 0);
            return;
        }
        uint32_t t_us = le32(d), period_ns = le32(d + 4), c = le32(d + 8);
        if (period_ns && (period_ns < 29990000u || period_ns > 30010000u)) {
            split_reply(f, sub, 4, NULL, 0);
            return;
        }
        f->naligns++;
        f->align_t_us = t_us, f->align_period = period_ns, f->align_counter = c;
        double now = modem_f(f);
        double target = now + (int32_t)(t_us - (uint32_t)(uint64_t)(int64_t)rint(now));
        double o = f->slot_origin, per = f->slot_period_us;
        long j = lrint((target - o) / per); /* the slot clock's boundary nearest the target */
        double phase = o + j * per - target;
        uint32_t cur = f->counter_base + (uint32_t)j;
        int flags;
        if (!f->duplex_tx) { /* unkeyed: a snap, done over the next slots (08 never set: no period kept) */
            f->has_grid = true;
            f->grid_target = target, f->grid_counter = c, f->grid_period = period_ns ? period_ns / 1000.0 : per;
            f->slot_period_us = f->grid_period;
            snap(f, now);
            flags = RDOF_OPS_SNAPPING | RDOF_OPS_ALIGNED;
            f->align_flags = RDOF_OPS_SNAPPING | (f->aligned ? RDOF_OPS_COUNTER_BASE : 0);
        } else if (fabs(phase) < RDOF_OPS_ALIGNED_US && ((cur - c) & 1) == 0) {
            f->counter_base = c - (uint32_t)j; /* keyed: the counter only */
            f->aligned = true;
            flags = RDOF_OPS_ALIGNED;
            f->align_flags = RDOF_OPS_ALIGNED | RDOF_OPS_COUNTER_BASE;
        } else {
            flags = RDOF_OPS_NOT_APPLIED; /* result 0: the phase tells how far a key-up drifted */
        }
        f->align_phase = (int32_t)lrint(phase);
        put_le32(b, (uint32_t)f->align_phase);
        b[4] = (uint8_t)flags;
        split_reply(f, sub, 0, b, 5);
        return;
    }
    case RDOF_OPS_SUB_TX_SCHED: {
        if (f->no_caps || !(f->split_caps & RDOF_OPS_SCHEDULE))
            break;
        if (n < 6 + 33) {
            split_reply(f, sub, 4, NULL, 0);
            return;
        }
        uint32_t c = le32(d);
        int slot = d[4];
        if (f->role != 1 || f->stream_mode != 1 || !f->aligned) {
            split_reply(f, sub, 2, NULL, 0);
            return;
        }
        put_le32(b, c);
        if (f->cooling) {
            f->sched_cooling++;
            b[4] = RDOF_SCHED_FULL;
            split_reply(f, sub, 0, b, 5);
            return;
        }
        if ((slot != 1 && slot != 2) || (slot == 1) != ((c & 1) == 0)) {
            split_reply(f, sub, 4, NULL, 0);
            return;
        }
        double now = modem_f(f), bnd;
        long k_now = fake_slot_boundary(f, now, &bnd);
        long k = wrap32(c - f->counter_base);
        double start = f->slot_origin + k * f->slot_period_us;
        int at = -1, used = 0, free_i = -1;
        for (int i = 0; i < 32; i++) {
            if (f->sched[i].used) {
                used++;
                if (f->sched[i].counter == c)
                    at = i;
            } else if (free_i < 0) {
                free_i = i;
            }
        }
        int outcome;
        const uint8_t control = d[5];
        const uint8_t idle = RDOF_SYNC_DATA | 9, term = RDOF_SYNC_DATA | 2; /* data types 9 and 2 */
        if (control == idle || (control == term && at >= 0 && f->sched[at].control == term)) {
            if (at >= 0) /* an idle burst or a repeated terminator: "nothing", the place cleared */
                f->sched[at].used = false;
            b[4] = RDOF_SCHED_QUEUED;
            split_reply(f, sub, 0, b, 5);
            return;
        }
        if (start - now < f->guard_ms * 1000.0) {
            f->sched_late++;
            outcome = RDOF_SCHED_LATE;
        } else if (k - k_now > f->horizon) {
            f->sched_early_full++;
            outcome = RDOF_SCHED_EARLY;
        } else if (at >= 0) {
            f->sched_replaced++;
            outcome = RDOF_SCHED_REPLACED;
        } else if (used >= f->sched_cap || free_i < 0) {
            f->sched_early_full++;
            outcome = RDOF_SCHED_FULL;
        } else {
            outcome = RDOF_SCHED_QUEUED;
            at = free_i;
        }
        if (outcome == RDOF_SCHED_QUEUED || outcome == RDOF_SCHED_REPLACED) {
            f->sched[at].used = true;
            f->sched[at].counter = c;
            f->sched[at].slot = (uint8_t)slot;
            f->sched[at].control = d[5];
            memcpy(f->sched[at].burst, d + 6, 33);
        }
        fake_air(f); /* within ~150 ms of its slot: keyed now */
        b[4] = (uint8_t)outcome;
        split_reply(f, sub, 0, b, 5);
        return;
    }
    default: break;
    }
    split_reply(f, sub, 1, NULL, 0);
}

static void handle(void *ctx, uint8_t cmd, const uint8_t *p, size_t len)
{
    fake_radio_t *f = ctx;
    if (f->silent)
        return;
    if (cmd == 0xD6) {
        if (is_split(f) && len)
            split_cmd(f, p, len);
        else
            nak(f, cmd, 1);
        return;
    }
    if (cmd == RDOF_MMDVM_GET_VERSION) {
        uint8_t body[200];
        size_t n = 0, d = strlen(f->description);
        if (f->protocol >= 2) {
            body[0] = 2, body[1] = 0x7F, body[2] = 0x01, body[3] = 2;
            memset(body + 4, 0, 16);
            n = 20;
        } else {
            body[0] = 1;
            n = 1;
        }
        memcpy(body + n, f->description, d);
        put(f, cmd, body, n + d);
        return;
    }
    if (cmd == RDOF_MMDVM_GET_STATUS) {
        if (f->drop_status > 0) {
            f->drop_status--;
            return;
        }
        uint8_t flags = (f->transmitting ? 0x01 : 0) | (f->carrier ? 0x40 : 0) | (f->rf_busy ? 0x80 : 0);
        int space = 48 - f->held, space1 = 48 - f->held1;
        if (f->protocol >= 2) {
            uint8_t s[17] = {(uint8_t)f->mode, flags, 0, 0, (uint8_t)space1, (uint8_t)space};
            put(f, cmd, s, sizeof s);
        } else {
            uint8_t s[10] = {0x22, (uint8_t)f->mode, flags, 0, (uint8_t)space1, (uint8_t)space, 0, 0, 0, 1};
            put(f, cmd, s, sizeof s);
        }
        return;
    }
    if ((cmd == RDOF_MMDVM_DMR_DATA1 || cmd == RDOF_MMDVM_DMR_DATA2) && f->role == 1 && f->stream_mode == 1) {
        nak(f, cmd, 2); /* scheduled: only TX_SCHED feeds the transmitter */
        return;
    }
    if (cmd == RDOF_MMDVM_DMR_DATA2) {
        if (f->rf_busy) {
            nak(f, cmd, RDOF_NAK_RF_BUSY);
            return;
        }
        f->held++;
        f->transmitting = true;
        if (is_hotspot(f) && f->role == 0)
            ack(f, cmd);
        return;
    }
    if (cmd == RDOF_MMDVM_DMR_DATA1) {
        if (f->rf_busy) {
            nak(f, cmd, RDOF_NAK_RF_BUSY);
            return;
        }
        f->held1++;
        return;
    }
    if (cmd == RDOF_MMDVM_DMR_START) {
        f->duplex_tx = f->transmitting = p[0] != 0;
        f->queue_keyed = false;
        return;
    }
    if (cmd == RDOF_MMDVM_DMR_ABORT) {
        if (p[0] == 1)
            f->held1 = 0;
        else
            f->held = 0;
        return;
    }
    if (cmd == RDOF_MMDVM_DMR_SHORTLC)
        return;
    if (cmd == RDOF_MMDVM_SET_FREQ) {
        uint32_t rx = le32(p + 1), tx = le32(p + 5);
        f->aligned = false; /* the slot clock restarts: it snaps back to the grid itself */
        f->align_flags = 0;
        if (strcmp(f->kind, "mmdvm") && !(f->band_lo <= rx && rx <= f->band_hi)) {
            nak(f, cmd, 4);
            return;
        }
        if (is_hotspot(f)) {
            f->has_freq = true;
            f->freq = rx;
            f->tx_freq = tx;
            f->power = p[9];
        } else if (!strcmp(f->kind, "mmdvm_hs")) {
            f->has_pending = true;
            f->pending_rx = rx;
            f->pending_tx = tx;
            f->power = p[9];
        }
    } else if (cmd == RDOF_MMDVM_SET_CONFIG) {
        if ((int)len < config_min(f->kind)) {
            nak(f, cmd, 4);
            return;
        }
        f->simplex = p[0] & 0x80;
        if (f->protocol >= 2) {
            f->cc = p[26];
            f->tx_level = p[10];
        } else {
            f->cc = p[6];
            f->tx_level = p[10];
        }
        if (!strcmp(f->kind, "mmdvm_hs") && f->has_pending) {
            f->has_freq = true;
            f->freq = f->pending_rx;
            f->tx_freq = f->pending_tx;
        }
    } else if (cmd == RDOF_MMDVM_SET_MODE) {
        f->mode = p[0];
    } else if (cmd == RDOF_MMDVM_SEND_CWID) {
        if (f->mode != RDOF_MODE_IDLE) {
            nak(f, cmd, 5);
            return;
        }
        if (f->ncw < 8) {
            memcpy(f->cw[f->ncw], p, len < 40 ? len : 40);
            f->cw[f->ncw][len < 40 ? len : 40] = 0;
            f->ncw++;
        }
    }
    ack(f, cmd);
}

void fake_write(fake_radio_t *f, const uint8_t *data, size_t len)
{
    rdof_mmdvm_reader_feed(&f->reader, data, len, handle, f);
}

void fake_hear(fake_radio_t *f, uint8_t control, const uint8_t *burst, int rssi, int slot)
{
    if (is_split(f) && (f->role != 2 || !f->has_epoch))
        return;
    uint8_t p[36];
    p[0] = control;
    memcpy(p + 1, burst, 33);
    p[34] = (uint8_t)(rssi >> 8);
    p[35] = (uint8_t)rssi;
    put(f, slot == 1 ? RDOF_MMDVM_DMR_DATA1 : RDOF_MMDVM_DMR_DATA2, p, sizeof p);
}

void fake_lose(fake_radio_t *f, int slot)
{
    if (is_split(f) && (f->role != 2 || !f->has_epoch))
        return;
    put(f, slot == 1 ? RDOF_MMDVM_DMR_LOST1 : RDOF_MMDVM_DMR_LOST2, NULL, 0);
}

void fake_drain(fake_radio_t *f)
{
    f->held = f->held1 = 0;
    f->transmitting = f->duplex_tx;
}

size_t fake_take(fake_radio_t *f, uint8_t *buf, size_t cap)
{
    size_t n = f->out_len < cap ? f->out_len : cap;
    memcpy(buf, f->out, n);
    memmove(f->out, f->out + n, f->out_len - n);
    f->out_len -= n;
    return n;
}
