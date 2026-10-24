// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The jitter buffer for network voice going out over the air (rdof/jitter.h); test_jitter.c checks it. */
#include "rdof/jitter.h"

#include <math.h>
#include <string.h>

int rdof_jb_bursts(double ms)
{
    if (!(ms > 0))
        return 0;
    if (ms > RDOF_JB_MAX_MS)
        ms = RDOF_JB_MAX_MS;
    return (int)ceil(ms / RDOF_JB_BURST_MS - 1e-9);
}

#if RDOF_FEATURE_TX

#include "rdof/bits.h"
#include "rdof/layer2.h"
#include "rdof/mmdvm.h"

/* The on-air AMBE+2 silence frame (mmdvm.c's). */
static const uint8_t SILENCE[9] = {0xb9, 0xe8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2a, 0x6b};

void rdof_jb_init(rdof_jb_t *jb)
{
    memset(jb, 0, sizeof *jb);
    jb->min = rdof_jb_bursts(RDOF_JB_MIN_MS);
    jb->max = rdof_jb_bursts(RDOF_JB_MAX_DEFAULT_MS);
}

static int clamp(const rdof_jb_t *jb, int n)
{
    int lo = jb->min > 0 ? jb->min : 1, hi = jb->max > lo ? jb->max : lo;
    return n < lo ? lo : n > hi ? hi : n;
}

/* The prebuffer for a lateness: the radio's lead and the lateness, rounded up to bursts. */
static int target_for(const rdof_jb_t *jb, double late_ms)
{
    return clamp(jb, RDOF_JB_LEAD + rdof_jb_bursts(late_ms + RDOF_JB_MARGIN_MS));
}

void rdof_jb_begin(rdof_jb_t *jb, rdof_jb_stream_t *s)
{
    memset(s, 0, sizeof *s);
    s->start = jb->has_link ? target_for(jb, jb->link_ms) : clamp(jb, RDOF_JB_UNKNOWN);
    s->target = s->start;
    jb->streams++;
    jb->target = s->target;
    jb->jitter_ms = jb->late_ms = 0;
}

static rdof_jb_key_t *find(rdof_jb_t *jb, uint32_t dst, bool group, int slot)
{
    for (int i = 0; i < RDOF_JB_KEYS; i++) {
        rdof_jb_key_t *k = &jb->keys[i];
        if (k->used && k->dst == dst && k->group == group && k->slot == slot)
            return k;
    }
    return NULL;
}

void rdof_jb_key(rdof_jb_t *jb, rdof_jb_stream_t *s, uint32_t dst, bool group, int slot, bool started)
{
    if (s->keyed)
        return;
    s->keyed = true;
    s->dst = dst;
    s->group = group;
    s->slot = slot;
    rdof_jb_key_t *k = find(jb, dst, group, slot);
    if (!k)
        return;
    k->used = ++jb->stamp;
    if (!started && s->n < RDOF_JB_SAMPLES) {
        s->start = s->target = target_for(jb, k->late_ms);
        jb->target = s->target;
    }
}

void rdof_jb_arrival(rdof_jb_t *jb, rdof_jb_stream_t *s, double at, int seq)
{
    int idx;
    seq &= 0xFF;
    if (!s->measuring) {
        s->measuring = true;
        s->t0 = at;
        s->idx = idx = 0;
        s->seq = seq;
    } else {
        int d = (seq - s->seq) & 0xFF;
        if (!d)
            d = 1; /* no sequence numbers: one burst on */
        if (d < 128) {
            s->idx += d; /* more than one: lost bursts */
            s->seq = seq;
            idx = s->idx;
        } else {
            idx = s->idx - (256 - d); /* out of order: an earlier one, late */
        }
    }
    double transit = (at - s->t0) * 1000.0 - (double)idx * RDOF_JB_BURST_MS;
    transit = transit > 30000 ? 30000 : transit < -30000 ? -30000 : transit;
    if (s->n) /* RFC 3550 6.4.1 */
        s->jitter_ms += (fabs(transit - s->last_transit) - s->jitter_ms) / 16.0;
    s->last_transit = transit;
    s->n++;
    s->transit[s->wnext] = (int16_t)lrint(transit);
    s->wnext = (s->wnext + 1) % RDOF_JB_WINDOW;
    if (s->nwin < RDOF_JB_WINDOW)
        s->nwin++;
    /* Lateness against the window's earliest: the worst but one, so a lone straggler doesn't set the size. */
    int lo = s->transit[0], hi1 = INT16_MIN, hi2 = INT16_MIN;
    for (int i = 0; i < s->nwin; i++) {
        int v = s->transit[i];
        lo = v < lo ? v : lo;
        if (v > hi1) {
            hi2 = hi1;
            hi1 = v;
        } else if (v > hi2) {
            hi2 = v;
        }
    }
    s->late_ms = s->nwin >= 2 ? hi2 - lo : 0;
    if (s->late_ms > s->peak_ms)
        s->peak_ms = s->late_ms;
    if (jb->adaptive && s->n >= RDOF_JB_SAMPLES)
        s->target = target_for(jb, s->late_ms);
    jb->target = s->target;
    jb->jitter_ms = s->jitter_ms;
    jb->late_ms = s->late_ms;
}

int rdof_jb_prebuffer(const rdof_jb_t *jb, const rdof_jb_stream_t *s, int fixed)
{
    return jb->adaptive ? s->start : fixed;
}

bool rdof_jb_underrun(rdof_jb_t *jb, rdof_jb_stream_t *s)
{
    jb->underruns++;
    if (!jb->adaptive || s->start + s->shift >= jb->max)
        return false;
    s->shift++;
    jb->grown++;
    return true;
}

int rdof_jb_fill(rdof_jb_t *jb, rdof_jb_stream_t *s, int gap, double ahead)
{
    if (!jb->adaptive || gap <= 0)
        return gap;
    double want = s->target - 1 - ahead;
    int keep = want > 0 ? (int)ceil(want - 1e-9) : 0;
    keep = keep < gap ? keep : gap;
    s->shift -= gap - keep;
    jb->shrunk += (uint32_t)(gap - keep);
    return keep;
}

bool rdof_jb_drop(rdof_jb_t *jb, rdof_jb_stream_t *s, double ahead, bool silent, double now)
{
    if (!jb->adaptive)
        return false;
    if (ahead - (s->target - 1) < 1 - 1e-9) {
        s->above = false;
        return false;
    }
    if (!s->above) {
        s->above = true;
        s->above_since = now;
    }
    /* Voice only to give back what the stream grew; silence whenever it's ahead. */
    if (!silent && !(s->shift > 0 && now - s->above_since >= RDOF_JB_SHRINK_S - 1e-9))
        return false;
    s->shift--;
    jb->shrunk++;
    s->above_since = now;
    return true;
}

static void remember(double *late, double need)
{
    *late = need >= *late ? need : (*late + need) / 2; /* up at once, down by half a call */
}

void rdof_jb_end(rdof_jb_t *jb, rdof_jb_stream_t *s)
{
    if (s->n < RDOF_JB_REMEMBER) {
        s->n = 0;
        return;
    }
    double need = s->peak_ms;
    if (jb->has_link)
        remember(&jb->link_ms, need);
    else
        jb->link_ms = need;
    jb->has_link = true;
    if (s->keyed) {
        rdof_jb_key_t *k = find(jb, s->dst, s->group, s->slot);
        if (k) {
            remember(&k->late_ms, need);
        } else {
            k = &jb->keys[0];
            for (int i = 1; i < RDOF_JB_KEYS; i++) /* a free one, else the one used longest ago */
                if (jb->keys[i].used < k->used)
                    k = &jb->keys[i];
            k->dst = s->dst;
            k->group = s->group;
            k->slot = s->slot;
            k->late_ms = need;
        }
        k->used = ++jb->stamp;
    }
    s->n = 0; /* once */
}

uint8_t rdof_jb_reframe(const uint8_t in[RDOF_BURST_BYTES], int n, int cc, bool bs, int pi, const uint8_t *embedded32,
                        uint8_t out[RDOF_BURST_BYTES])
{
    static const int lcss_of[6] = {0, 1, 3, 3, 2, 0};
    memmove(out, in, RDOF_BURST_BYTES);
    if (n <= 0 || n > 5) {
        rdof_set_field(out, 108, 48, bs ? RDOF_BS_VOICE : RDOF_MS_VOICE);
        return RDOF_SYNC_AUDIO;
    }
    uint8_t e[16];
    rdof_emb_bits(cc, lcss_of[n], pi, e);
    rdof_set_field(out, 108, 8, rdof_bits_to_u64(e, 8));
    rdof_set_field(out, 148, 8, rdof_bits_to_u64(e + 8, 8));
    for (int i = 0; i < 32; i++)
        rdof_set_field(out, 116 + i, 1, embedded32 ? embedded32[i] & 1 : 0);
    return (uint8_t)n;
}

bool rdof_jb_silent(const uint8_t burst[RDOF_BURST_BYTES])
{
    uint8_t f[3][9];
    rdof_ambe_frames(burst, f);
    return !memcmp(f[0], SILENCE, 9) && !memcmp(f[1], SILENCE, 9) && !memcmp(f[2], SILENCE, 9);
}

void rdof_jb_json(const rdof_jb_t *jb, int fixed, rdof_json_t *j, const char *key)
{
    rdof_json_open(j, key);
    rdof_json_str(j, "mode", jb->adaptive ? "auto" : "fixed");
    int target = !jb->adaptive ? fixed : jb->streams ? jb->target : clamp(jb, RDOF_JB_UNKNOWN);
    rdof_json_int(j, "target_ms", (long long)target * RDOF_JB_BURST_MS);
    rdof_json_int(j, "jitter_ms", (long long)lrint(jb->jitter_ms));
    rdof_json_int(j, "late_ms", (long long)lrint(jb->late_ms));
    if (jb->adaptive) {
        rdof_json_int(j, "min_ms", (long long)clamp(jb, 0) * RDOF_JB_BURST_MS);
        rdof_json_int(j, "max_ms", (long long)clamp(jb, 1 << 20) * RDOF_JB_BURST_MS);
    }
    rdof_json_int(j, "streams", jb->streams);
    rdof_json_int(j, "underruns", jb->underruns);
    rdof_json_int(j, "grown", jb->grown);
    rdof_json_int(j, "shrunk", jb->shrunk);
    rdof_json_close(j);
}

#endif /* RDOF_FEATURE_TX */
