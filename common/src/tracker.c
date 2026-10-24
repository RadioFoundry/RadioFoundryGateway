// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* One channel's call tracker (rdof/tracker.h); the golden tests check it. */
#include "rdof/tracker.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "rdof/ambe.h"
#include "rdof/bits.h"
#include "rdof/features.h"
#include "rdof/json.h"
#include "rdof/mmdvm.h"
#include "rdof/pdu.h"
#include "rdof/rawburst.h"

#if RDOF_FEATURE_TRACKER
/* Voice LCs whose embedded LC starts a late entry: ETSI, and Motorola's. */
static bool late_entry_fid(int fid)
{
    return fid == 0x00 || fid == 0x10;
}

#define CSBK_BSDWNACT 0x38

static const char *csbk_desc(int opcode)
{
    switch (opcode) {
    case 0x04: return "Unit to Unit Voice Service Request";
    case 0x05: return "Unit to Unit Voice Answer Response";
    case 0x26: return "Negative Acknowledgment Response";
    case 0x3D: return "Preamble";
    case 0x1F: return "Call Alert";
    case 0x20: return "Call Alert Ack";
    case 0x24: return "Radio Check";
    case 0x27: return "Call Emergency";
    default: return NULL;
    }
}

#endif

/* ── time ──────────────────────────────────────────────────────────────────── */

/* (long long) of a double, saturating, NaN 0: the cast is undefined outside the range. */
static long long sat_ll(double v)
{
    if (isnan(v))
        return 0;
    if (v >= 9223372036854775808.0)
        return LLONG_MAX;
    if (v <= -9223372036854775808.0)
        return LLONG_MIN;
    return (long long)v;
}

/* Howard Hinnant's civil_from_days (public domain algorithm). */
static void civil_from_days(long long z, int *y, int *m, int *d)
{
    z += 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long yy = (long long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*m <= 2));
}

int64_t rdof_time_ms(double t)
{
    /* As datetime.fromtimestamp (microseconds rounded half even) for the seconds, then int(t * 1000) % 1000 for
     * the ms: what tracker.iso(t) writes. */
    double ip, frac = modf(t, &ip);
    double us = rint(frac * 1e6);
    /* past the int64 ends: saturated, then wrapping (unsigned) */
    uint64_t secs = (uint64_t)sat_ll(ip);
    if (us >= 1000000)
        secs += 1;
    else if (us < 0)
        secs -= 1;
    long long ms = sat_ll(t * 1000) % 1000;
    if (ms < 0)
        ms += 1000;
    return (int64_t)(secs * 1000 + (uint64_t)ms);
}

void rdof_iso_ms(int64_t ms_total, char out[32])
{
    /* floor division (negating INT64_MIN overflowed) */
    long long secs = ms_total / 1000, ms = ms_total % 1000;
    if (ms < 0) {
        ms += 1000;
        secs -= 1;
    }
    long long days = secs / 86400, rem = secs % 86400;
    if (rem < 0) {
        rem += 86400;
        days -= 1;
    }
    int y, mo, d;
    civil_from_days(days, &y, &mo, &d);
    char tmp[96];
    snprintf(tmp, sizeof tmp, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", y, mo, d, (int)(rem / 3600),
             (int)(rem / 60 % 60), (int)(rem % 60), (int)ms);
    memcpy(out, tmp, 31);
    out[31] = 0;
}

void rdof_iso_time(double t, char out[32])
{
    rdof_iso_ms(rdof_time_ms(t), out);
}

/* ── setup ─────────────────────────────────────────────────────────────────── */

void rdof_tracker_init(rdof_tracker_t *tr, const char *channel, double rssi_offset, double freq_hz)
{
    memset(tr, 0, sizeof *tr);
    snprintf(tr->channel, sizeof tr->channel, "%s", channel);
    tr->rssi_offset = rssi_offset;
    tr->freq_hz = freq_hz;
}

bool rdof_tracker_add_sink(rdof_tracker_t *tr, rdof_sink_t sink)
{
    if (tr->nsinks < 0 || tr->nsinks >= RDOF_TRACKER_MAX_SINKS)
        return false;
    tr->sinks[tr->nsinks++] = sink;
    return true;
}

void rdof_modem_burst(uint8_t control, const uint8_t burst[RDOF_BURST_BYTES], double dbm, int cc, int slot,
                    rdof_burst_t *out)
{
    memset(out, 0, sizeof *out);
    memcpy(out->burst, burst, RDOF_BURST_BYTES);
    rdof_control_to_dt(control, &out->dt, &out->n);
    out->slot = slot;
    out->family = RDOF_FAMILY_MS;
    out->cc = cc;
    out->power_dbfs = dbm;
}

#if RDOF_FEATURE_TRACKER
/* ── output helpers ────────────────────────────────────────────────────────── */

static void emit_json(rdof_tracker_t *tr, rdof_json_t *j)
{
    if (!rdof_json_done(j))
        return;
    for (int i = 0; i < tr->nsinks; i++)
        if (tr->sinks[i].on_json)
            tr->sinks[i].on_json(tr->sinks[i].ctx, tr->channel, j->buf);
}

static void emit_raw(rdof_tracker_t *tr, const rdof_burst_t *b, double t, const rdof_call_t *call)
{
    rdof_rawburst_t r = {.ms = rdof_time_ms(t), .slot = b->slot, .dt = b->dt, .n = b->n, .addressed = call != NULL};
    if (call) {
        r.src = call->src;
        r.dst = call->dst;
        r.group = call->group;
    }
    memcpy(r.burst, b->burst, RDOF_BURST_BYTES);
    r.utc = b->utc;
    r.utc_slot = b->utc_slot;
    r.utc_us = b->utc_us;
    char buf[RDOF_JSON_MAX];
    bool json = false;
    for (int i = 0; i < tr->nsinks; i++)
        if (tr->sinks[i].on_raw && !json) /* the JSON only for a sink that takes it (the tests, the self-test) */
            json = true;
    if (json && !rdof_rawburst_json(&r, buf, sizeof buf))
        json = false;
    for (int i = 0; i < tr->nsinks; i++) {
        if (tr->sinks[i].on_raw && json)
            tr->sinks[i].on_raw(tr->sinks[i].ctx, tr->channel, buf);
        if (tr->sinks[i].on_raw_burst)
            tr->sinks[i].on_raw_burst(tr->sinks[i].ctx, tr->channel, &r);
    }
}

static void emit_frame(rdof_tracker_t *tr, const rdof_frame_t *f)
{
    for (int i = 0; i < tr->nsinks; i++)
        if (tr->sinks[i].on_frame)
            tr->sinks[i].on_frame(tr->sinks[i].ctx, tr->channel, f);
}

static long wadd(long a, long b) { return (long)((unsigned long)a + (unsigned long)b); }

static int burst_rssi(const rdof_tracker_t *tr, const rdof_burst_t *b)
{
    const double v = rint(b->power_dbfs + tr->rssi_offset);
    /* saturating (the cast is undefined outside int), NaN (no power, or the offset) 0 */
    return isnan(v) ? 0 : v >= 2147483648.0 ? INT_MAX : v <= -2147483648.0 ? INT_MIN : (int)v;
}

static void make_frame(rdof_frame_t *f, int slot, int dt, int n, const uint8_t *burst, const rdof_call_t *call)
{
    memset(f, 0, sizeof *f);
    f->slot = slot;
    f->dt = dt;
    f->n = n;
    memcpy(f->burst, burst, RDOF_BURST_BYTES);
    f->group = true;
    if (call) {
        f->has_addr = true;
        f->src = call->src;
        f->dst = call->dst;
        f->group = call->group;
    }
}

static void frame_from(rdof_tracker_t *tr, const rdof_burst_t *b, int n, const rdof_call_t *call, bool new_stream)
{
    rdof_frame_t f;
    make_frame(&f, b->slot, b->dt, n, b->burst, call);
    f.new_stream = new_stream;
    f.errors = b->sync_errors;
    f.rssi = burst_rssi(tr, b);
    emit_frame(tr, &f);
}

static void emit_call(rdof_tracker_t *tr, const char *action, uint32_t src, uint32_t dst)
{
    for (int i = 0; i < tr->nsinks; i++)
        if (tr->sinks[i].on_call)
            tr->sinks[i].on_call(tr->sinks[i].ctx, tr->channel, action, src, dst);
}

static void start_json(rdof_tracker_t *tr, const char *action, int slot, const rdof_call_t *call, double t, int frames)
{
    char buf[RDOF_JSON_MAX], ts[32], info[16], freq[24];
    rdof_json_t j;
    rdof_iso_time(t, ts);
    rdof_json_init(&j, buf, sizeof buf);
    rdof_json_open(&j, NULL);
    rdof_json_open(&j, "DMR");
    rdof_json_str(&j, "timestamp", ts);
    rdof_json_str(&j, "source", "rf");
    rdof_json_str(&j, "action", action);
    rdof_json_int(&j, "slot", slot);
    rdof_json_int(&j, "src_id", call->src);
    rdof_json_int(&j, "dst_id", call->dst);
    rdof_json_str(&j, "group", call->group ? "yes" : "no");
    snprintf(info, sizeof info, "%lu", (unsigned long)call->src);
    rdof_json_str(&j, "src_info", info);
    if (tr->freq_hz != 0) {
        snprintf(freq, sizeof freq, "%.5f", tr->freq_hz / 1e6);
        rdof_json_str(&j, "frequency", freq);
        rdof_json_int(&j, "colorCode", call->cc);
    }
    if (frames >= 0)
        rdof_json_int(&j, "frames", frames);
    if (call->emergency)
        rdof_json_bool(&j, "emergency", true);
    rdof_json_close(&j);
    rdof_json_close(&j);
    emit_json(tr, &j);
    emit_call(tr, action, call->src, call->dst);
}

static void end_json(rdof_tracker_t *tr, const char *action, int slot, const rdof_call_t *call, double t)
{
    char buf[RDOF_JSON_MAX], ts[32];
    rdof_json_t j;
    rdof_iso_time(t, ts);
    rdof_json_init(&j, buf, sizeof buf);
    rdof_json_open(&j, NULL);
    rdof_json_open(&j, "DMR");
    rdof_json_str(&j, "timestamp", ts);
    rdof_json_str(&j, "action", action);
    rdof_json_int(&j, "slot", slot);
    if (call->kind == RDOF_CALL_VOICE) {
        double dur = t - call->start;
        rdof_json_float3(&j, "duration", dur > 0.0 ? dur : 0.0);
        rdof_json_float3(&j, "ber", call->bits ? 100.0 * (double)call->errs / (double)call->bits : 0.0);
        if (call->rssi_n) {
            rdof_json_open(&j, "rssi");
            rdof_json_int(&j, "min", call->rssi_min);
            rdof_json_int(&j, "max", call->rssi_max);
            rdof_json_int(&j, "ave", (long long)rint((double)call->rssi_sum / call->rssi_n));
            rdof_json_close(&j);
        }
    }
    rdof_json_close(&j);
    rdof_json_close(&j);
    emit_json(tr, &j);
    emit_call(tr, action, call->src, call->dst);
}

static void mode_json(rdof_tracker_t *tr, double t, const char *mode)
{
    char buf[128], ts[32];
    rdof_json_t j;
    rdof_iso_time(t, ts);
    rdof_json_init(&j, buf, sizeof buf);
    rdof_json_open(&j, NULL);
    rdof_json_open(&j, "MMDVM");
    rdof_json_str(&j, "timestamp", ts);
    rdof_json_str(&j, "mode", mode);
    rdof_json_close(&j);
    rdof_json_close(&j);
    emit_json(tr, &j);
}

static void activity(rdof_tracker_t *tr, double t)
{
    tr->last_activity = t;
    if (tr->mode != 1) {
        tr->mode = 1;
        mode_json(tr, t, "DMR");
    }
}

/* ── calls ─────────────────────────────────────────────────────────────────── */

static void close_call(rdof_tracker_t *tr, rdof_slot_state_t *st, int slot, const char *action, double t,
                       bool terminator)
{
    if (!st->call.active)
        return;
    rdof_call_t call = st->call;
    st->call.active = false;
    end_json(tr, action, slot, &call, t);
    if (call.kind == RDOF_CALL_VOICE && !terminator) {
        /* Carrier lost: give the network a proper end of stream, with the call's own LC. */
        uint8_t lc[9], term[RDOF_BURST_BYTES];
        if (call.has_lc)
            memcpy(lc, call.lc, 9);
        else
            rdof_group_lc(call.src, call.dst, call.group, 0, lc);
        rdof_lc_burst(lc, RDOF_DT_TERMINATOR_WITH_LC, call.cc, rdof_data_sync(rdof_family_voice_sync(call.family)), term);
        rdof_frame_t f;
        make_frame(&f, slot, RDOF_DT_TERMINATOR_WITH_LC, 0, term, &call);
        f.synthetic = true;
        emit_frame(tr, &f);
    }
}

static rdof_call_t *open_call(rdof_tracker_t *tr, rdof_slot_state_t *st, int slot, rdof_call_kind_t kind, uint32_t src,
                            uint32_t dst, bool group, double t, const rdof_burst_t *b)
{
    if (st->call.active)
        close_call(tr, st, slot, "lost", t, false);
    rdof_call_t *c = &st->call;
    memset(c, 0, sizeof *c);
    c->active = true;
    c->kind = kind;
    c->src = src;
    c->dst = dst;
    c->group = group;
    c->start = c->last = t;
    c->cc = b->cc >= 0 ? b->cc : 0;
    c->family = b->family;
    c->metrics_at = t;
    return c;
}

static rdof_call_t *current(rdof_slot_state_t *st)
{
    return st->call.active ? &st->call : NULL;
}

static void on_lc(rdof_tracker_t *tr, const rdof_burst_t *b, rdof_slot_state_t *st, double t)
{
    rdof_full_lc_t lc;
    bool have = rdof_decode_voice_lc(b->burst, b->dt, &lc);
    rdof_call_t *call = current(st);
    if (b->dt == RDOF_DT_VOICE_LC_HEADER) {
        if (!have || lc.src_id == RDOF_NONE) {
            emit_raw(tr, b, t, call);
            return;
        }
        bool same = call && call->kind == RDOF_CALL_VOICE && call->src == (uint32_t)lc.src_id &&
                    call->dst == (uint32_t)lc.dst_id;
        if (!same) {
            call = open_call(tr, st, b->slot, RDOF_CALL_VOICE, (uint32_t)lc.src_id, (uint32_t)lc.dst_id, lc.group, t, b);
            call->emergency = lc.service_options & RDOF_SO_EMERGENCY;
            start_json(tr, "start", b->slot, call, t, -1);
        }
        st->have_frag = 0;
        call->last = t;
        call->has_lc = true;
        memcpy(call->lc, lc.raw, 9);
        emit_raw(tr, b, t, call);
        frame_from(tr, b, 0, call, !same);
        return;
    }
    /* Terminator with LC. */
    if (!call && have && st->has_ended && lc.src_id != RDOF_NONE && st->ended_src == (uint32_t)lc.src_id &&
        st->ended_dst == (uint32_t)lc.dst_id && t - st->ended_at <= RDOF_REPEAT_TERMINATOR_SECONDS) {
        emit_raw(tr, b, t, NULL); /* a repeat of the one that just ended it (MMDVMHost sends 3) */
        return;
    }
    if (!call && have && lc.src_id != RDOF_NONE) {
        /* Only heard the end: report it as a (very short) late entry. */
        call = open_call(tr, st, b->slot, RDOF_CALL_VOICE, (uint32_t)lc.src_id, (uint32_t)lc.dst_id, lc.group, t, b);
        call->emergency = lc.service_options & RDOF_SO_EMERGENCY;
        start_json(tr, "late_entry", b->slot, call, t, -1);
    }
    emit_raw(tr, b, t, call);
    if (call) {
        frame_from(tr, b, 0, call, false);
        uint32_t src = call->src, dst = call->dst;
        close_call(tr, st, b->slot, "end", t, true);
        st->has_ended = true;
        st->ended_src = src;
        st->ended_dst = dst;
        st->ended_at = t;
    }
}

static void on_voice(rdof_tracker_t *tr, const rdof_burst_t *b, rdof_slot_state_t *st, double t)
{
    rdof_call_t *call = st->call.active && st->call.kind == RDOF_CALL_VOICE ? &st->call : NULL;
    /* Embedded LC: late entry, or a new talker that skipped the header. */
    if (b->n == 0) {
        st->have_frag = 0;
    } else if (b->n >= 1 && b->n <= 4) {
        rdof_embedded_signalling(b->burst, st->frags[b->n]);
        st->have_frag |= (uint8_t)(1 << b->n);
        if (b->n == 4 && st->have_frag == 0x1E) {
            uint8_t raw[128], lc9[9];
            for (int i = 0; i < 4; i++)
                memcpy(raw + 32 * i, st->frags[i + 1], 32);
            st->have_frag = 0;
            if (rdof_embedded_lc_decode(raw, lc9)) {
                rdof_full_lc_t lc;
                rdof_parse_full_lc(lc9, &lc);
                if ((lc.flco == RDOF_FLCO_GROUP || lc.flco == RDOF_FLCO_PRIVATE) && late_entry_fid(lc.fid)) {
                    if (!call || call->src != (uint32_t)lc.src_id || call->dst != (uint32_t)lc.dst_id) {
                        call = open_call(tr, st, b->slot, RDOF_CALL_VOICE, (uint32_t)lc.src_id, (uint32_t)lc.dst_id,
                                         lc.group, t, b);
                        call->has_lc = true;
                        memcpy(call->lc, lc9, 9);
                        call->emergency = lc.service_options & RDOF_SO_EMERGENCY;
                        start_json(tr, "late_entry", b->slot, call, t, -1);
                        rdof_frame_t f;
                        uint8_t hdr[RDOF_BURST_BYTES];
                        rdof_lc_burst(lc9, RDOF_DT_VOICE_LC_HEADER, call->cc,
                                    rdof_data_sync(rdof_family_voice_sync(b->family)), hdr);
                        make_frame(&f, b->slot, RDOF_DT_VOICE_LC_HEADER, 0, hdr, call);
                        f.new_stream = true;
                        f.synthetic = true;
                        emit_frame(tr, &f);
                    }
                }
            }
        }
    }
    if (call) {
        uint8_t frames[3][9];
        int errs = 0;
        call->last = t;
        rdof_ambe_frames(b->burst, frames);
        for (int i = 0; i < 3; i++)
            errs += rdof_ambe_decode_frame(frames[i], NULL);
        /* wrapping: long is 32 bits on the S3 */
        call->bits = wadd(call->bits, 141);
        call->errs = wadd(call->errs, errs);
        call->m_bits = wadd(call->m_bits, 141);
        call->m_errs = wadd(call->m_errs, errs);
        bool measured = !isnan(b->power_dbfs);
        if (measured) {
            int r = burst_rssi(tr, b);
            if (call->rssi_n == 0 || r < call->rssi_min)
                call->rssi_min = r;
            if (call->rssi_n == 0 || r > call->rssi_max)
                call->rssi_max = r;
            call->rssi_sum = wadd(call->rssi_sum, r);
            call->rssi_n = (int)((unsigned)call->rssi_n + 1u);
        }
        if (t - call->metrics_at >= RDOF_METRIC_SECONDS) {
            char buf[160], ts[32];
            rdof_json_t j;
            call->metrics_at = t;
            rdof_iso_time(t, ts);
            rdof_json_init(&j, buf, sizeof buf);
            rdof_json_open(&j, NULL);
            rdof_json_open(&j, "BER");
            rdof_json_str(&j, "timestamp", ts);
            rdof_json_str(&j, "mode", "DMR");
            rdof_json_int(&j, "slot", b->slot);
            rdof_json_float3(&j, "value", 100.0 * (double)call->m_errs / (double)(call->m_bits > 1 ? call->m_bits : 1));
            rdof_json_close(&j);
            rdof_json_close(&j);
            emit_json(tr, &j);
            if (measured) {
                rdof_json_init(&j, buf, sizeof buf);
                rdof_json_open(&j, NULL);
                rdof_json_open(&j, "RSSI");
                rdof_json_str(&j, "timestamp", ts);
                rdof_json_str(&j, "mode", "DMR");
                rdof_json_int(&j, "slot", b->slot);
                rdof_json_int(&j, "value", burst_rssi(tr, b));
                rdof_json_close(&j);
                rdof_json_close(&j);
                emit_json(tr, &j);
            }
            call->m_bits = call->m_errs = 0;
        }
    }
    emit_raw(tr, b, t, call);
    frame_from(tr, b, b->n, call, false);
}

static void on_csbk(rdof_tracker_t *tr, const rdof_burst_t *b, rdof_slot_state_t *st, double t)
{
    rdof_csbk_t c;
    rdof_decode_csbk(b->burst, &c);
    if (!c.crc_ok || (c.fid == 0 && c.opcode == CSBK_BSDWNACT))
        return;
    const uint8_t *d = c.raw;
    uint32_t a = rdof_be24(d + 4), z = rdof_be24(d + 7), src, dst;
    bool group = false;
    if (c.opcode == 0x3D) {
        group = d[2] & 0x40;
        src = z;
        dst = a;
    } else if (c.opcode == 0x26 || (c.opcode == 0x24 && d[3] != 0x80)) {
        src = a;
        dst = z;
    } else {
        src = z;
        dst = a;
        group = c.opcode == 0x27;
    }
    emit_raw(tr, b, t, NULL);
    rdof_frame_t f;
    rdof_call_t addr = {0};
    addr.src = src;
    addr.dst = dst;
    addr.group = group;
    make_frame(&f, b->slot, b->dt, 0, b->burst, &addr);
    f.new_stream = true;
    f.errors = b->sync_errors;
    f.rssi = burst_rssi(tr, b);
    emit_frame(tr, &f);
    const char *desc = (c.fid == 0x00 || c.fid == 0x10) ? csbk_desc(c.opcode) : NULL;
    if (c.opcode == 0x24 && desc)
        desc = d[3] == 0x80 ? "Radio Check" : "Radio Check Ack"; /* a request, or the radio's answer */
    if (!desc)
        return;
    if (c.opcode == 0x3D) {
        if (st->has_preamble && st->preamble_src == src && st->preamble_dst == dst &&
            t - st->preamble_at < RDOF_PREAMBLE_REPEAT_SECONDS) {
            st->preamble_at = t;
            return;
        }
        st->has_preamble = true;
        st->preamble_src = src;
        st->preamble_dst = dst;
        st->preamble_at = t;
    }
    char buf[RDOF_JSON_MAX], ts[32], info[16];
    rdof_json_t j;
    rdof_iso_time(t, ts);
    snprintf(info, sizeof info, "%lu", (unsigned long)src);
    rdof_json_init(&j, buf, sizeof buf);
    rdof_json_open(&j, NULL);
    rdof_json_open(&j, "DMR");
    rdof_json_str(&j, "timestamp", ts);
    rdof_json_str(&j, "source", "rf");
    rdof_json_str(&j, "action", "csbk");
    rdof_json_int(&j, "slot", b->slot);
    rdof_json_int(&j, "src_id", src);
    rdof_json_int(&j, "dst_id", dst);
    rdof_json_str(&j, "group", group ? "yes" : "no");
    rdof_json_str(&j, "src_info", info);
    rdof_json_str(&j, "csbk_desc", desc);
    rdof_json_close(&j);
    rdof_json_close(&j);
    emit_json(tr, &j);
}

/* A data call over at its last block: a terminator from the same radio soon after (a chain's, RadioFoundry
 * radios end one with it) is that transmission's end, not a call of its own. */
static void data_ended(rdof_slot_state_t *st, uint32_t src, uint32_t dst, double t)
{
    st->has_ended = true;
    st->ended_src = src;
    st->ended_dst = dst;
    st->ended_at = t;
}

static void on_data_block(rdof_tracker_t *tr, const rdof_burst_t *b, rdof_slot_state_t *st, double t)
{
    rdof_call_t *call = st->call.active && st->call.kind == RDOF_CALL_DATA ? &st->call : NULL;
    emit_raw(tr, b, t, call);
    frame_from(tr, b, 0, call, false);
    if (call) {
        call->last = t;
        call->blocks_left--;
        if (call->blocks_left <= 0) {
            uint32_t src = call->src, dst = call->dst;
            close_call(tr, st, b->slot, "end", t, false);
            data_ended(st, src, dst, t);
        }
    }
}

static void on_data_header(rdof_tracker_t *tr, const rdof_burst_t *b, rdof_slot_state_t *st, double t)
{
    rdof_data_header_t h;
    rdof_decode_data_header(b->burst, &h);
    if (!h.crc_ok) {
        emit_raw(tr, b, t, current(st));
        return;
    }
    rdof_call_t *call = current(st);
    if (h.dpf == RDOF_DPF_PROPRIETARY && call && call->kind == RDOF_CALL_DATA) {
        /* Proprietary second header: counts as one of the announced blocks. */
        on_data_block(tr, b, st, t);
        return;
    }
    call = open_call(tr, st, b->slot, RDOF_CALL_DATA, h.src_id, h.dst_id, h.group, t, b);
    call->blocks_left = h.blocks;
    start_json(tr, "start", b->slot, call, t, h.blocks);
    emit_raw(tr, b, t, call);
    frame_from(tr, b, 0, call, true);
    if (call->blocks_left == 0) {
        uint32_t src = call->src, dst = call->dst;
        close_call(tr, st, b->slot, "end", t, false);
        data_ended(st, src, dst, t);
    }
}

void rdof_tracker_feed(rdof_tracker_t *tr, const rdof_burst_t *b, double t)
{
    if (b->dt == RDOF_DT_IDLE || b->slot < 1 || b->slot > 2)
        return;
    activity(tr, t);
    rdof_slot_state_t *st = &tr->slots[b->slot - 1];
    switch (b->dt) {
    case RDOF_DT_VOICE_SYNC:
    case RDOF_DT_VOICE: on_voice(tr, b, st, t); break;
    case RDOF_DT_VOICE_LC_HEADER:
    case RDOF_DT_TERMINATOR_WITH_LC: on_lc(tr, b, st, t); break;
    case RDOF_DT_CSBK: on_csbk(tr, b, st, t); break;
    case RDOF_DT_DATA_HEADER: on_data_header(tr, b, st, t); break;
    case RDOF_DT_RATE_12:
    case RDOF_DT_RATE_34:
    case RDOF_DT_RATE_1: on_data_block(tr, b, st, t); break;
    default: { /* PI header, MBC, USBD: pass through */
        rdof_call_t *call = current(st);
        emit_raw(tr, b, t, call);
        frame_from(tr, b, 0, call, false);
    }
    }
}

void rdof_tracker_tick(rdof_tracker_t *tr, double t)
{
    bool any = false;
    for (int i = 0; i < 2; i++) {
        rdof_slot_state_t *st = &tr->slots[i];
        if (!st->call.active)
            continue;
        double limit = st->call.kind == RDOF_CALL_VOICE ? RDOF_VOICE_LOST_SECONDS : RDOF_DATA_LOST_SECONDS;
        if (t - st->call.last > limit)
            close_call(tr, st, i + 1, "lost", st->call.last, false);
    }
    for (int i = 0; i < 2; i++)
        any |= tr->slots[i].call.active;
    if (tr->mode == 1 && t - tr->last_activity > RDOF_IDLE_HANG_SECONDS && !any) {
        tr->mode = 2;
        mode_json(tr, t, "idle");
    }
}
#endif /* RDOF_FEATURE_TRACKER */
