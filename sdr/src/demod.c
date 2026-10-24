// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Per-channel DMR burst receiver. One linear buffer per channel (sized at
 * init), trimmed as the search moves on.
 */
#include "rdof/sdr/demod.h"

#if RDOF_FEATURE_SDR

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/bits.h"
#include "rdof/layer2.h"
#include "rdof/pdu.h"
#include "sdr_private.h"

enum {
    SPS = RDOF_FM_SPS,
    BURST_SYMBOLS = 132,
    SYNC_START_SYMBOL = 54,
    CACH_SYMBOLS = 12,
    SLOT_PERIOD = 288 * SPS,
    PRE = SYNC_START_SYMBOL * SPS,
    POST = (BURST_SYMBOLS - SYNC_START_SYMBOL) * SPS,
    CACH = CACH_SYMBOLS * SPS,
    LOOKAHEAD = POST + 8,
    HISTORY = PRE + CACH + 16,
    GATE_SPAN = 48 * SPS,
    GATE_LEAD = 12 * SPS,
    RETAIN = 2048, /* most samples kept between feeds (about 1470 used) */
    SLACK_FEEDS = 8, /* room for this many feeds more before the history is moved down (PSRAM is cheap, moves aren't) */
    RESUM = 512,   /* squelch positions between fresh sums */
};
#define MIN_VAR (24 * 100.0 * 100.0) /* windows flatter than +-100 Hz can't be a sync */

static const int slot_hint[RDOF_FAMILY_COUNT] = {0, 0, 1, 2};

/* The sync templates' signs (every DMR sync symbol is +3 or -3), symbol by symbol as a 4-bit key, bit f set
 * where family f has +3: maybe_sync_keyed's buckets. rdof_demod_init checks them against the families. */
static const uint8_t SYNC_KEYS[24] = {15, 4, 1, 13, 7, 5, 4, 2, 6, 11, 9, 10, 4, 3, 14, 10, 15, 12, 5, 11, 8, 8, 1, 2};
static bool keys_ok;

/* ── setup ──────────────────────────────────────────────────────────────── */

void rdof_demod_free(rdof_demod_t *d)
{
    rdof_fm_free(&d->fm);
    rdof_sdr_free(d->buf);
    rdof_sdr_free(d->pow);
    rdof_sdr_free(d->pos);
    rdof_sdr_free(d->best);
    rdof_sdr_free(d->best_fam);
    rdof_sdr_free(d->out);
    rdof_sdr_free(d->tail);
    memset(d, 0, sizeof *d);
}

int rdof_demod_init(rdof_demod_t *d, int cc, int default_slot, double squelch_hz, int max_feed)
{
    memset(d, 0, sizeof *d);
    d->cc = cc;
    d->default_slot = default_slot;
    d->squelch_hz = squelch_hz;
    d->max_feed = max_feed;
    d->keep = max_feed + RETAIN;
    d->cap = d->keep + SLACK_FEEDS * max_feed;
    d->out_cap = d->keep / 400 + 32;
    rdof_fm_init(&d->fm, RDOF_DEMOD_FS);
    d->buf = rdof_sdr_alloc(sizeof(float) * d->cap);
    d->pow = rdof_sdr_alloc(sizeof(float) * d->cap);
    /* Candidates (score >= 0.8) cluster within a few samples of each sync, and a sync is at
     * least 900 samples from the next: one in 32 positions is ample. */
    d->cand_cap = d->keep / 32 + 64;
    d->pos = rdof_sdr_alloc(sizeof(int64_t) * d->cand_cap);
    d->best = rdof_sdr_alloc(sizeof(float) * d->cand_cap);
    d->best_fam = rdof_sdr_alloc(d->cand_cap);
    d->out = rdof_sdr_alloc(sizeof(rdof_sdr_burst_t) * d->out_cap);
    d->tail = rdof_sdr_alloc(sizeof(rdof_cf_t) * (RDOF_FM_TAPS + 1));
    if (!d->buf || !d->pow || !d->pos || !d->best || !d->best_fam || !d->out || !d->tail)
        return -1;
    for (int f = 0; f < RDOF_FAMILY_COUNT; f++) {
        uint64_t p = rdof_family_voice_sync((rdof_family_t)f);
        double mean = 0.0, t[24], norm = 0.0;
        for (int i = 0; i < 24; i++) {
            int dibit = (int)((p >> (46 - 2 * i)) & 3);
            d->templ[f][i] = (int8_t)(dibit == 1 ? 3 : dibit == 0 ? 1 : dibit == 2 ? -1 : -3);
            mean += d->templ[f][i];
        }
        mean /= 24;
        for (int i = 0; i < 24; i++) {
            t[i] = d->templ[f][i] - mean;
            norm += t[i] * t[i];
        }
        norm = sqrt(norm);
        for (int i = 0; i < 24; i++)
            d->unit[f][i] = (float)(t[i] / norm);
    }
    bool ok = true;
    for (int i = 0; i < 24; i++) {
        int key = 0;
        for (int f = 0; f < RDOF_FAMILY_COUNT; f++) {
            key |= (d->templ[f][i] == 3) << f;
            ok = ok && (d->templ[f][i] == 3 || d->templ[f][i] == -3);
        }
        ok = ok && key == SYNC_KEYS[i];
    }
    for (int f = 0; f < RDOF_FAMILY_COUNT; f++) { /* zero-mean templates */
        int sum = 0;
        for (int i = 0; i < 24; i++)
            sum += d->templ[f][i];
        ok = ok && sum == 0;
    }
    keys_ok = ok;
    return 0;
}

/* ── helpers ────────────────────────────────────────────────────────────── */

static inline float at(const rdof_demod_t *d, int64_t abs_index) { return d->buf[abs_index - d->base]; }

/* demod._slice: 4FSK decisions on n symbol-spaced values from absolute index p, to packed bytes. */
static void slice_bytes(const rdof_demod_t *d, int64_t p, int nsym, float center, float outer, uint8_t *out)
{
    const float t = (float)(2.0 / 3.0) * outer;
    uint8_t bits[2 * BURST_SYMBOLS];
    for (int i = 0; i < nsym; i++) {
        float v = at(d, p + (int64_t)i * SPS) - center;
        int sym = v > t ? 3 : v > 0 ? 1 : v > -t ? -1 : -3;
        /* +3 -> 01, +1 -> 00, -1 -> 10, -3 -> 11 */
        bits[2 * i] = sym < 0;
        bits[2 * i + 1] = sym == 3 || sym == -3;
    }
    rdof_bits_to_bytes(bits, 2 * nsym, out);
}

static void slice_bits(const rdof_demod_t *d, int64_t p, int nsym, float center, float outer, uint8_t *bits)
{
    const float t = (float)(2.0 / 3.0) * outer;
    for (int i = 0; i < nsym; i++) {
        float v = at(d, p + (int64_t)i * SPS) - center;
        int sym = v > t ? 3 : v > 0 ? 1 : v > -t ? -1 : -3;
        bits[2 * i] = sym < 0;
        bits[2 * i + 1] = sym == 3 || sym == -3;
    }
}

static double power_over(const rdof_demod_t *d, int64_t start, int n)
{
    int64_t rel = start - d->base, rel0 = d->lbase - d->base;
    int64_t lo = rel > rel0 ? rel : rel0, hi = rel + n;
    if (hi > d->len)
        hi = d->len;
    float sum = 0.0f;
    for (int64_t i = lo; i < hi; i++)
        sum += d->pow[i];
    float mean = hi > lo ? sum / (float)(hi - lo) : 0.0f;
    /* float log: a double one is software on these cores (it was a good part of each burst's cost), and
     * power goes out rounded to the dB */
    return 10.0 * (double)log10f(mean + 1e-12f);
}

static double power_at(const rdof_demod_t *d, int64_t start) { return power_over(d, start, BURST_SYMBOLS * SPS); }

/* Cosine similarity of the 24 symbols from absolute position p with every sync family. */
static void scores(const rdof_demod_t *d, int64_t p, float sc[RDOF_FAMILY_COUNT])
{
    /* float only (the P4 and S3 have no double FPU); two passes keep the variance exact enough */
    const float *x = d->buf + (p - d->base);
    float s1 = 0.0f, var = 0.0f, v[24];
    for (int i = 0; i < 24; i++) {
        v[i] = x[i * SPS];
        s1 += v[i];
    }
    const float mean = s1 / 24.0f;
    for (int i = 0; i < 24; i++) {
        float c = v[i] - mean;
        var += c * c;
    }
    if (!(var > (float)MIN_VAR)) {
        for (int f = 0; f < RDOF_FAMILY_COUNT; f++)
            sc[f] = 0.0f;
        return;
    }
    const float inv = 1.0f / sqrtf(var);
    for (int f = 0; f < RDOF_FAMILY_COUNT; f++) {
        /* templates are zero-mean, so the window mean drops out of the dot product */
        float acc = 0.0f;
        for (int i = 0; i < 24; i++)
            acc += d->unit[f][i] * v[i];
        sc[f] = acc * inv;
    }
}

/* Could position p score 0.79 or better with any family? A prefilter: it only passes positions on to
 * scores(), which decides. The same sums, but with no square root or divide and every family in one pass
 * (independent chains the FPU pipelines). The 0.01 margin is orders above the rounding apart. */
static bool maybe_sync_loop(const rdof_demod_t *d, const float *x)
{
    const float *u0 = d->unit[0], *u1 = d->unit[1], *u2 = d->unit[2], *u3 = d->unit[3];
    float s1 = 0.0f, s2 = 0.0f, a0 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
    for (int i = 0; i < 24; i++) {
        const float v = x[i * SPS];
        s1 += v;
        s2 += v * v;
        a0 += u0[i] * v;
        a1 += u1[i] * v;
        a2 += u2[i] * v;
        a3 += u3[i] * v;
    }
    const float var = s2 - s1 * s1 * (1.0f / 24.0f);
    if (!(var > (float)(MIN_VAR * 0.99)))
        return false;
    const float lim = 0.79f * 0.79f * var;
    return a0 * a0 >= lim || a1 * a1 >= lim || a2 * a2 >= lim || a3 * a3 >= lim;
}

/* maybe_sync_loop with the templates' structure: each is +-1 times sqrt(1/24) and zero-mean, so family f's
 * dot product is (2 P_f - S) / sqrt(24), P_f the sum over its +3 symbols and S over all. The symbols are
 * summed by key (SYNC_KEYS, generated from the four syncs), then the keys by row (bits 2-3) and column (bits
 * 0-1): P_0 = C1 + C3, P_1 = C2 + C3, P_2 = R1 + R3, P_3 = R2 + R3. About a third of the loop's instructions,
 * none of them multiplies but the squares. */
static inline bool maybe_sync_keyed(const float *x)
{
    const float v0 = x[0 * SPS], v1 = x[1 * SPS], v2 = x[2 * SPS], v3 = x[3 * SPS], v4 = x[4 * SPS],
                v5 = x[5 * SPS], v6 = x[6 * SPS], v7 = x[7 * SPS], v8 = x[8 * SPS], v9 = x[9 * SPS],
                v10 = x[10 * SPS], v11 = x[11 * SPS], v12 = x[12 * SPS], v13 = x[13 * SPS], v14 = x[14 * SPS],
                v15 = x[15 * SPS], v16 = x[16 * SPS], v17 = x[17 * SPS], v18 = x[18 * SPS], v19 = x[19 * SPS],
                v20 = x[20 * SPS], v21 = x[21 * SPS], v22 = x[22 * SPS], v23 = x[23 * SPS];
    const float q0 = v0 * v0 + v3 * v3 + v6 * v6 + v9 * v9 + v12 * v12 + v15 * v15 + v18 * v18 + v21 * v21;
    const float q1 = v1 * v1 + v4 * v4 + v7 * v7 + v10 * v10 + v13 * v13 + v16 * v16 + v19 * v19 + v22 * v22;
    const float q2 = v2 * v2 + v5 * v5 + v8 * v8 + v11 * v11 + v14 * v14 + v17 * v17 + v20 * v20 + v23 * v23;
    const float b1 = v2 + v22, b2 = v7 + v23, b3 = v13, b4 = v1 + v6 + v12, b5 = v5 + v18, b6 = v8, b7 = v4,
                b8 = v20 + v21, b9 = v10, b10 = v11 + v15, b11 = v9 + v19, b12 = v17, b13 = v3, b14 = v14,
                b15 = v0 + v16;
    const float r0 = b1 + b2 + b3, r1 = b4 + b5 + b6 + b7, r2 = b8 + b9 + b10 + b11, r3 = b12 + b13 + b14 + b15;
    const float c1 = b1 + b5 + b9 + b13, c2 = b2 + b6 + b10 + b14, c3 = b3 + b7 + b11 + b15;
    const float s1 = (r0 + r1) + (r2 + r3), s2 = q0 + q1 + q2;
    const float var = s2 - s1 * s1 * (1.0f / 24.0f);
    if (!(var > (float)(MIN_VAR * 0.99)))
        return false;
    const float lim = 0.79f * 0.79f * 24.0f * var;
    const float d0 = 2.0f * (c1 + c3) - s1, d1 = 2.0f * (c2 + c3) - s1, d2 = 2.0f * (r1 + r3) - s1,
                d3 = 2.0f * (r2 + r3) - s1;
    return d0 * d0 >= lim || d1 * d1 >= lim || d2 * d2 >= lim || d3 * d3 >= lim;
}

static inline bool maybe_sync(const rdof_demod_t *d, const float *x)
{
    return keys_ok ? maybe_sync_keyed(x) : maybe_sync_loop(d, x);
}

static inline int argmax_abs(const float *v, int n)
{
    int k = 0;
    for (int i = 1; i < n; i++)
        if (fabsf(v[i]) > fabsf(v[k]))
            k = i;
    return k;
}

static void emit(rdof_demod_t *d, const rdof_sdr_burst_t *b)
{
    if (d->out_n < d->out_cap)
        d->out[d->out_n++] = *b;
}

static void make_burst(rdof_sdr_burst_t *o, const uint8_t burst[RDOF_BURST_BYTES], int dt, int n, int slot, rdof_family_t fam,
                       int cc, int64_t start, int errs, double power, float center, float outer)
{
    memset(o, 0, sizeof *o);
    memcpy(o->b.burst, burst, RDOF_BURST_BYTES);
    o->b.dt = dt;
    o->b.n = n;
    o->b.slot = slot;
    o->b.family = fam;
    o->b.cc = cc;
    o->b.sync_errors = errs;
    o->b.power_dbfs = power;
    o->position = start;
    o->freq_offset_hz = center;
    o->deviation_hz = outer;
}

static rdof_demod_track_t *track(rdof_demod_t *d, int slot) { return &d->tracks[slot == 2 ? 1 : 0]; }

static void set_track(rdof_demod_t *d, int slot, const rdof_demod_track_t *t)
{
    rdof_demod_track_t *tr = track(d, slot);
    unsigned order = tr->active ? tr->order : d->order_next++; /* dict assignment keeps a key's place */
    *tr = *t;
    tr->active = true;
    tr->order = order;
}

/* ── voice flywheel ─────────────────────────────────────────────────────── */

/* The 4FSK center for the burst at start, from the burst itself: slice it with the track's center, then move the
 * center by the mean slicing error, twice. A voice burst has no sync to measure it from, and the center moves
 * under the flywheel when AFC retunes the channel or the transmitter drifts. */
static float recenter(const rdof_demod_t *d, int64_t start, const rdof_demod_track_t *tr)
{
    if (start < d->lbase)
        return tr->center;
    const float scale = tr->outer / 3.0f;
    float center = tr->center;
    for (int it = 0; it < 2; it++) {
        float sum = 0.0f;
        for (int i = 0; i < BURST_SYMBOLS; i++) {
            float v = (at(d, start + (int64_t)i * SPS) - center) / scale;
            float q = nearbyintf((v + 3.0f) / 2.0f) * 2.0f - 3.0f;
            q = q < -3.0f ? -3.0f : q > 3.0f ? 3.0f : q;
            sum += v - q;
        }
        center = center + sum / (float)BURST_SYMBOLS * scale;
    }
    return center;
}

static bool voice_at(rdof_demod_t *d, int64_t start, rdof_demod_track_t *tr, int slot, rdof_sdr_burst_t *out)
{
    static const int offsets[] = {0, -1, 1, -2, 2, -3, 3};
    const float center = recenter(d, start, tr);
    bool have = false;
    int best_errs = 0, best_d = 0, best_cc = 0;
    float best_err = 0.0f;
    uint8_t best_burst[RDOF_BURST_BYTES];
    for (int k = 0; k < 7; k++) {
        int dd = offsets[k];
        int64_t pos = start + dd;
        if (pos < d->lbase)
            continue;
        uint8_t burst[RDOF_BURST_BYTES];
        slice_bytes(d, pos, BURST_SYMBOLS, center, tr->outer, burst);
        int cc, pi, lcss, errs;
        if (!rdof_decode_emb(burst, &cc, &pi, &lcss, &errs))
            continue;
        if (d->cc >= 0 && cc != d->cc)
            continue;
        /* slicing error as the tie-break between offsets */
        const float scale = tr->outer / 3.0f;
        float sum = 0.0f;
        for (int i = 0; i < BURST_SYMBOLS; i++) {
            float v = (at(d, pos + (int64_t)i * SPS) - center) / scale;
            float q = nearbyintf((v + 3.0f) / 2.0f) * 2.0f - 3.0f;
            q = q < -3.0f ? -3.0f : q > 3.0f ? 3.0f : q;
            sum += fabsf(v - q);
        }
        float err = sum / (float)BURST_SYMBOLS;
        if (!have || errs < best_errs || (errs == best_errs && err < best_err)) {
            have = true;
            best_errs = errs;
            best_err = err;
            best_d = dd;
            best_cc = cc;
            memcpy(best_burst, burst, RDOF_BURST_BYTES);
        }
        if (dd == 0 && errs == 0 && (double)err < 0.35)
            break; /* clean at the predicted timing: no need to search */
    }
    if (!have)
        return false;
    tr->anchor += best_d;
    tr->center = center;
    const double power = power_at(d, start + best_d);
    tr->power = power_over(d, start + best_d + PRE, 24 * SPS);
    make_burst(out, best_burst, RDOF_DT_VOICE, tr->n + 1, slot, tr->family, best_cc, start + best_d, best_errs, power,
               tr->center, tr->outer);
    return true;
}

static void fly(rdof_demod_t *d, int64_t end)
{
    /* tracks are visited in insertion order */
    int order[2] = {0, 1};
    if (d->tracks[0].active && d->tracks[1].active && d->tracks[1].order < d->tracks[0].order) {
        order[0] = 1;
        order[1] = 0;
    }
    for (int oi = 0; oi < 2; oi++) {
        rdof_demod_track_t *tr = &d->tracks[order[oi]];
        if (!tr->active)
            continue;
        const int slot = order[oi] + 1;
        bool broke = false;
        while (tr->n < 5) {
            int64_t nxt = tr->anchor + (int64_t)(tr->n + 1) * SLOT_PERIOD;
            if (nxt + BURST_SYMBOLS * SPS + 4 > end) {
                broke = true;
                break;
            }
            rdof_sdr_burst_t b;
            bool got = voice_at(d, nxt, tr, slot, &b);
            tr->n += 1;
            if (tr->has_pending) {
                if (got)
                    emit(d, &tr->pending);
                tr->has_pending = false;
            }
            if (!got) {
                tr->misses += 1;
                if (tr->misses >= 2) {
                    tr->active = false;
                    broke = true;
                    break;
                }
                continue;
            }
            tr->misses = 0;
            emit(d, &b);
        }
        if (!broke)
            tr->active = false; /* F delivered: the next A re-anchors through the sync search */
    }
}

/* ── sync ───────────────────────────────────────────────────────────────── */

static int guess_slot(const rdof_demod_t *d, int64_t start)
{
    if (d->last_slot) {
        int64_t gap = start - d->last_pos;
        double bursts_f = nearbyint((double)gap / (BURST_SYMBOLS + CACH_SYMBOLS) / SPS);
        long long bursts = (long long)bursts_f;
        if (llabs(gap - bursts * (BURST_SYMBOLS + CACH_SYMBOLS) * SPS) < 3 * SPS && bursts < 40) {
            long long odd = ((bursts % 2) + 2) % 2;
            return odd == 0 ? d->last_slot : (d->last_slot == 1 ? 2 : 1);
        }
    }
    return d->default_slot;
}

/* demod._at_sync: a burst (data) or a held voice burst A (returns false, sets a track). */
static bool at_sync(rdof_demod_t *d, int64_t sync_pos, int fam, float score, rdof_sdr_burst_t *out)
{
    const bool voice = score > 0;
    float hs = 0.0f, ls = 0.0f;
    int hn = 0, ln = 0;
    for (int i = 0; i < 24; i++) {
        float v = at(d, sync_pos + (int64_t)i * SPS);
        int t = d->templ[fam][i] * (voice ? 1 : -1);
        if (t > 0) {
            hs += v;
            hn++;
        } else {
            ls += v;
            ln++;
        }
    }
    const float hi_v = hs / (float)hn, lo_v = ls / (float)ln;
    const float center = (hi_v + lo_v) / 2.0f, outer = (hi_v - lo_v) / 2.0f;
    if (!(300.0f < outer && outer < 5000.0f))
        return false;
    const int64_t start = sync_pos - PRE;
    uint8_t burst[RDOF_BURST_BYTES];
    slice_bytes(d, start, BURST_SYMBOLS, center, outer, burst);
    const uint64_t pattern = rdof_family_voice_sync((rdof_family_t)fam);
    const int errs = rdof_sync_errors(burst, voice ? pattern : rdof_data_sync(pattern));
    if (errs > RDOF_DEMOD_MAX_SYNC_BIT_ERRORS)
        return false;
    int slot = slot_hint[fam];
    if (fam == RDOF_FAMILY_BS) {
        uint8_t cbits[2 * CACH_SYMBOLS];
        slice_bits(d, start - CACH, CACH_SYMBOLS, center, outer, cbits);
        slot = rdof_decode_cach_slot(cbits);
    }
    if (!slot)
        slot = guess_slot(d, start);
    const double power = power_at(d, start);
    /* A sync much weaker than the voice call this slot is following can't take the slot over: the tuner's IQ
     * image of a mirrored channel, or a neighbor's leakage, which the discriminator reads as cleanly as the call
     * whenever the call's transmitter is between bursts. */
    const double mid = power_over(d, sync_pos, 24 * SPS); /* the sync alone: the burst may overlap the call's */
    const rdof_demod_track_t *held = track(d, slot);
    const bool weaker = held->active && mid < held->power - RDOF_DEMOD_WEAKER_DB;
    if (voice) {
        if (weaker)
            return false;
        /* Burst A has no color code: hold it until burst B's EMB confirms the channel. */
        rdof_demod_track_t t;
        memset(&t, 0, sizeof t);
        t.anchor = start;
        t.n = 0;
        t.family = (rdof_family_t)fam;
        t.center = center;
        t.outer = outer;
        t.has_pending = true;
        t.power = mid;
        make_burst(&t.pending, burst, RDOF_DT_VOICE_SYNC, 0, slot, (rdof_family_t)fam, d->cc, start, errs, power, center,
                   outer);
        set_track(d, slot, &t);
        d->last_slot = slot;
        d->last_pos = start;
        return false;
    }
    int cc, dt, st_errs;
    if (!rdof_decode_slot_type(burst, &cc, &dt, &st_errs) || (d->cc >= 0 && cc != d->cc))
        return false;
    if (dt != RDOF_DT_IDLE && !weaker)
        track(d, slot)->active = false; /* a data burst on a slot ends any voice flywheel there */
    make_burst(out, burst, dt, 0, slot, (rdof_family_t)fam, cc, start, errs + st_errs, power, center, outer);
    d->last_slot = slot;
    d->last_pos = start;
    return true;
}

static bool is_held_start(const rdof_demod_t *d, int64_t p)
{
    for (int i = 0; i < 2; i++)
        if (d->tracks[i].active && d->tracks[i].has_pending && d->tracks[i].pending.position + PRE == p)
            return true;
    return false;
}

/* Score one position the prefilter let through; keep it if it is near enough a sync. */
static inline void scored(rdof_demod_t *d, int64_t p, int *nc)
{
    float sc[RDOF_FAMILY_COUNT];
    scores(d, p, sc);
    int f = argmax_abs(sc, RDOF_FAMILY_COUNT);
    if (fabsf(sc[f]) >= (float)(RDOF_DEMOD_SYNC_THRESHOLD - 0.05) && *nc < d->cand_cap) {
        d->pos[*nc] = p;
        d->best[*nc] = sc[f];
        d->best_fam[*nc] = (int8_t)f;
        (*nc)++;
    }
}

rdof_demod_prof_t rdof_demod_prof;
static uint32_t (*prof_clock)(void);

void rdof_demod_set_clock(uint32_t (*ticks)(void)) { prof_clock = ticks; }

static inline uint32_t prof_now(void) { return prof_clock ? prof_clock() : 0; }

static void search(rdof_demod_t *d, int64_t end)
{
    int64_t lo = d->next_search, hi = end - LOOKAHEAD;
    if (lo < d->lbase + PRE + CACH)
        lo = d->lbase + PRE + CACH;
    if (hi <= lo)
        return;
    d->next_search = hi;

    /* Squelch: correlate only where the discriminator spread over a 480-sample window around the
     * sync is below squelch_hz (a DMR signal keeps it within about +-2 kHz, noise swings wider).
     * Indices relative to buf from here: 64-bit arithmetic costs several instructions a step on these cores. */
    const int npos = (int)((hi - lo + RDOF_DEMOD_SEARCH_STRIDE - 1) / RDOF_DEMOD_SEARCH_STRIDE);
    const int r0 = (int)(lo - d->base);
    const float *buf = d->buf;
    const uint32_t t0 = prof_now();
    int nc = 0, nscored = 0;
    if (d->squelch_hz > 0) {
        /* Running sums in float, restarted every RESUM positions so rounding can't build up. */
        const float lim = (float)(d->squelch_hz * d->squelch_hz), inv = 1.0f / GATE_SPAN;
        float s1 = 0.0f, s2 = 0.0f;
        for (int k = 0; k < npos; k++) {
            const int r = r0 + k * RDOF_DEMOD_SEARCH_STRIDE, ws = r - GATE_LEAD;
            if (k % RESUM == 0) {
                s1 = s2 = 0.0f;
                for (int i = 0; i < GATE_SPAN; i++) {
                    const float v = buf[ws + i];
                    s1 += v;
                    s2 += v * v;
                }
            } else {
                for (int j = ws - RDOF_DEMOD_SEARCH_STRIDE; j < ws; j++) {
                    const float vo = buf[j], vi = buf[j + GATE_SPAN];
                    s1 += vi - vo;
                    s2 += vi * vi - vo * vo;
                }
            }
            const float m1 = s1 * inv, m2 = s2 * inv;
            if (m2 - m1 * m1 < lim && maybe_sync(d, buf + r)) {
                scored(d, d->base + r, &nc);
                nscored++;
            }
        }
    } else {
        for (int k = 0; k < npos; k++) {
            const int r = r0 + k * RDOF_DEMOD_SEARCH_STRIDE;
            if (maybe_sync(d, buf + r)) {
                scored(d, d->base + r, &nc);
                nscored++;
            }
        }
    }

    const uint32_t t2 = prof_now();
    int i = 0, nsync = 0;
    while (i < nc) {
        /* the strongest point of this cluster, refined to the sample */
        int j = i;
        while (j + 1 < nc && d->pos[j + 1] - d->pos[i] <= SPS)
            j++;
        int k = i;
        for (int q = i + 1; q <= j; q++)
            if (fabsf(d->best[q]) > fabsf(d->best[k]))
                k = q;
        i = j + 1;
        const int64_t p0 = d->pos[k];
        int64_t near[3];
        float val[3];
        int fam[3], nn = 0;
        for (int64_t q = p0 - RDOF_DEMOD_SEARCH_STRIDE + 1; q < p0 + RDOF_DEMOD_SEARCH_STRIDE; q++) {
            if (q < lo || q >= hi)
                continue;
            float sc[RDOF_FAMILY_COUNT];
            scores(d, q, sc);
            fam[nn] = argmax_abs(sc, RDOF_FAMILY_COUNT);
            val[nn] = sc[fam[nn]];
            near[nn++] = q;
        }
        if (!nn)
            continue;
        int r = argmax_abs(val, nn);
        if (fabsf(val[r]) < (float)RDOF_DEMOD_SYNC_THRESHOLD)
            continue;
        const int64_t sp = near[r];
        nsync++;
        /* Release flywheel bursts before this sync first, so a new superframe can't overtake
         * the previous one's B-F. */
        const uint32_t tf = prof_now();
        fly(d, sp);
        const uint32_t ta = prof_now();
        rdof_sdr_burst_t b;
        bool got = at_sync(d, sp, fam[r], val[r], &b);
        if (prof_clock) {
            rdof_demod_prof.sync_fly += (uint32_t)(ta - tf);
            rdof_demod_prof.at_sync += (uint32_t)(prof_now() - ta);
        }
        if (got || is_held_start(d, sp)) {
            if (got)
                emit(d, &b);
            const int64_t skip = sp + POST + CACH; /* skip to after this burst */
            while (i < nc && d->pos[i] < skip)
                i++;
        }
    }
    if (prof_clock) {
        rdof_demod_prof.pre += (uint32_t)(t2 - t0);
        rdof_demod_prof.sync += (uint32_t)(prof_now() - t2);
        rdof_demod_prof.positions += (uint32_t)npos;
        rdof_demod_prof.scored += (uint32_t)nscored;
        rdof_demod_prof.syncs += (uint32_t)nsync;
    }
}

/* ── feed ───────────────────────────────────────────────────────────────── */

/* What the feed no longer needs: lbase moves up (the samples stay until compact needs the room). */
static void trim(rdof_demod_t *d)
{
    int64_t keep_from = d->next_search - HISTORY;
    for (int i = 0; i < 2; i++) {
        const rdof_demod_track_t *tr = &d->tracks[i];
        if (tr->active) {
            int64_t k = tr->anchor + (int64_t)(tr->n + 1) * SLOT_PERIOD - 8;
            if (k < keep_from)
                keep_from = k;
        }
    }
    const int64_t end = d->base + d->len;
    if (keep_from > end)
        keep_from = end;
    if (keep_from > d->lbase)
        d->lbase = keep_from;
}

/* Move the samples from lbase down to the start of the buffers. */
static void compact(rdof_demod_t *d)
{
    const int64_t drop = d->lbase - d->base;
    if (drop <= 0)
        return;
    memmove(d->buf, d->buf + drop, sizeof(float) * (size_t)(d->len - drop));
    memmove(d->pow, d->pow + drop, sizeof(float) * (size_t)(d->len - drop));
    d->len -= (int)drop;
    d->base += drop;
}


const rdof_sdr_burst_t *rdof_demod_feed(rdof_demod_t *d, const rdof_cf_t *iq, int n, int *count)
{
    d->out_n = 0;
    if (n > d->max_feed)
        n = d->max_feed;
    const int64_t end0 = d->base + d->len;
    if (end0 - d->lbase + n > d->keep) /* never expected (the trim keeps under RETAIN): drop the oldest */
        d->lbase = end0 + n - d->keep;
    const uint32_t t0 = prof_now();
    if (d->len + n > d->cap)
        compact(d);
    const uint32_t t1 = prof_now();
    if (d->skipped) {
        /* the FM stage's filter and delay line as if it had run through the skipped samples */
        float wf[RDOF_FM_TAPS + 1], wp[RDOF_FM_TAPS + 1];
        rdof_fm_process(&d->fm, d->tail, d->tail_n, wf, wp);
        d->skipped = false;
    }
    rdof_fm_process(&d->fm, iq, n, d->buf + d->len, d->pow + d->len);
    d->len += n;
    d->samples_in += n;
    const int64_t end = d->base + d->len;
    const uint32_t t2 = prof_now();
    search(d, end);
    const uint32_t t3 = prof_now();
    fly(d, end);
    if (prof_clock) {
        rdof_demod_prof.compact += (uint32_t)(t1 - t0);
        rdof_demod_prof.fm += (uint32_t)(t2 - t1);
        rdof_demod_prof.search += (uint32_t)(t3 - t2);
        rdof_demod_prof.fly += (uint32_t)(prof_now() - t3);
    }
    /* stable sort by position */
    for (int i = 1; i < d->out_n; i++) {
        rdof_sdr_burst_t t = d->out[i];
        int j = i - 1;
        while (j >= 0 && d->out[j].position > t.position) {
            d->out[j + 1] = d->out[j];
            j--;
        }
        d->out[j + 1] = t;
    }
    trim(d);
    *count = d->out_n;
    return d->out;
}

bool rdof_demod_busy(const rdof_demod_t *d) { return d->tracks[0].active || d->tracks[1].active; }

void rdof_demod_skip(rdof_demod_t *d, const rdof_cf_t *iq, int n)
{
    d->out_n = 0;
    if (n > d->max_feed)
        n = d->max_feed;
    const int64_t end0 = d->base + d->len;
    if (end0 - d->lbase + n > d->keep)
        d->lbase = end0 + n - d->keep;
    if (d->len + n > d->cap)
        compact(d);
    memset(d->buf + d->len, 0, sizeof(float) * (size_t)n);
    memset(d->pow + d->len, 0, sizeof(float) * (size_t)n);
    d->len += n;
    d->samples_in += n;
    const int64_t hi = d->base + d->len - LOOKAHEAD;
    if (hi > d->next_search)
        d->next_search = hi;
    /* keep the last RDOF_FM_TAPS + 1 samples */
    const int T = RDOF_FM_TAPS + 1;
    if (n >= T) {
        memcpy(d->tail, iq + n - T, sizeof(rdof_cf_t) * T);
        d->tail_n = T;
    } else {
        int keep = d->tail_n + n > T ? T - n : d->tail_n;
        memmove(d->tail, d->tail + d->tail_n - keep, sizeof(rdof_cf_t) * (size_t)keep);
        memcpy(d->tail + keep, iq, sizeof(rdof_cf_t) * (size_t)n);
        d->tail_n = keep + n;
    }
    d->skipped = true;
    trim(d);
}

#endif
