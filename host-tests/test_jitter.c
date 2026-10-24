// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The jitter buffer (rdof/jitter.h): its pieces, then the transceiver and the repeater fed network streams that
 * arrive steady, jittered, after a stall, in a late storm and with bursts lost, on a radio that sends one burst
 * every 60 ms. */
#include <math.h>

#include "harness.h"
#include "rdof/features.h"

#if RDOF_FEATURE_TX && RDOF_FEATURE_REPEATER

#include "rdof/ambe.h"
#include "rdof/bits.h"
#include "rdof/fec.h"
#include "rdof/hblink.h"
#include "rdof/jitter.h"
#include "rdof/json.h"
#include "rdof/layer2.h"
#include "rdof/mmdvm.h"
#include "rdof/pdu.h"
#include "rdof/repeater.h"
#include "rdof/txvr.h"

#define CHECK(cond, ...)                                                                                     \
    do {                                                                                                     \
        checks++;                                                                                            \
        if (!(cond)) {                                                                                       \
            failures++;                                                                                      \
            printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                          \
            printf(__VA_ARGS__);                                                                             \
            printf("\n");                                                                                    \
        }                                                                                                    \
    } while (0)

#define CC 1
#define SRC 9992001u
#define TG 91u
#define FREQ 462562500u
#define PEER 9990301u
#define C_HEADER (RDOF_SYNC_DATA | RDOF_DT_VOICE_LC_HEADER)
#define C_TERM (RDOF_SYNC_DATA | RDOF_DT_TERMINATOR_WITH_LC)

/* ── a network stream, as it arrives ───────────────────────────────────────── */

#define NB 1100 /* bursts at most: a minute */

typedef struct {
    rdof_net_burst_t b[NB];
    double at[NB];
    bool lost[NB];
    int n;
    double t0; /* when burst 0 left its source */
    uint8_t lc9[9];
} stream_t;

/* What goes wrong on the way, burst by burst. FIFO: a burst never overtakes the one before it. */
typedef struct {
    double jitter_ms;          /* each burst late by 0 to this, at random */
    int jitter_from;           /* from this burst (the first ones on time: the worst case for the prebuffer) */
    int stall_at;              /* from this burst (0: none) nothing arrives for stall_s; then all of it at once */
    double stall_s;
    int storm_from, storm_to;  /* these bursts late by storm_s each */
    double storm_s;
    int lose_every;            /* every Nth voice burst lost (0: none) */
    uint32_t seed;
} pattern_t;

static uint32_t rnd(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

/* A voice burst at position n carrying its stream index k in its AMBE (three good frames), with the LC's embedded
 * fragment on B-E, MS framed as a hotspot sends it up. */
static uint8_t voice_burst(int k, int n, const uint8_t lc9[9], uint8_t out[RDOF_BURST_BYTES])
{
    uint8_t raw[128], params[49], frame[9], fbits[72], bits[264], voice[216];
    rdof_embedded_lc_encode(lc9, raw);
    uint8_t control = rdof_silence_burst(n, CC, false, n >= 1 && n <= 4 ? raw + 32 * (n - 1) : NULL, out);
    rdof_bytes_to_bits(out, RDOF_BURST_BYTES, bits);
    for (int f = 0; f < 3; f++) {
        memset(params, 0, sizeof params);
        for (int i = 0; i < 8; i++)
            params[i] = (uint8_t)((0xA5 >> i) & 1); /* ours */
        for (int i = 0; i < 16; i++)
            params[8 + i] = (uint8_t)((k >> i) & 1);
        params[30] = (uint8_t)(f & 1);
        rdof_ambe_encode_frame(params, frame);
        rdof_bytes_to_bits(frame, 9, fbits);
        memcpy(voice + 72 * f, fbits, 72);
    }
    memcpy(bits, voice, 108);
    memcpy(bits + 156, voice + 108, 108);
    rdof_bits_to_bytes(bits, 264, out);
    return control;
}

/* The index a voice burst carries, or -1 (silence, or not ours). */
static int burst_index(const uint8_t burst[RDOF_BURST_BYTES])
{
    uint8_t f[3][9], params[49];
    rdof_ambe_frames(burst, f);
    rdof_ambe_decode_frame(f[0], params);
    for (int i = 0; i < 8; i++)
        if (params[i] != ((0xA5 >> i) & 1))
            return -1;
    int k = 0;
    for (int i = 0; i < 16; i++)
        k |= params[8 + i] << i;
    return k;
}

/* A voice call: a header, superframes of voice, a terminator; sequence numbers from 0. */
static void make_call(stream_t *s, uint32_t sid, int superframes, int slot, double t0)
{
    memset(s, 0, sizeof *s);
    s->t0 = t0;
    rdof_group_lc(SRC, TG, true, 0, s->lc9);
    int k = 0;
    for (; k < 1 + 6 * superframes + 1 && k < NB; k++) {
        rdof_net_burst_t *b = &s->b[k];
        rdof_put_be32(b->stream_id, sid);
        b->src = SRC;
        b->dst = TG;
        b->group = true;
        b->slot = slot;
        b->seq = (uint8_t)k;
        if (k == 0) {
            b->control = C_HEADER;
            rdof_lc_burst(s->lc9, RDOF_DT_VOICE_LC_HEADER, CC, rdof_data_sync(RDOF_MS_VOICE), b->burst);
        } else if (k == 6 * superframes + 1) {
            b->control = C_TERM;
            b->terminator = true;
            rdof_lc_burst(s->lc9, RDOF_DT_TERMINATOR_WITH_LC, CC, rdof_data_sync(RDOF_MS_VOICE), b->burst);
        } else {
            b->control = voice_burst(k, (k - 1) % 6, s->lc9, b->burst);
        }
        s->at[k] = t0 + 0.06 * k;
    }
    s->n = k;
}

static void apply(stream_t *s, const pattern_t *p)
{
    uint32_t seed = p->seed ? p->seed : 1;
    for (int k = 0; k < s->n; k++) {
        double nominal = s->t0 + 0.06 * k, d = 0;
        if (p->jitter_ms > 0 && k >= p->jitter_from)
            d = p->jitter_ms / 1000.0 * (double)(rnd(&seed) % 10001) / 10000.0;
        if (p->stall_at && k >= p->stall_at) {
            double until = s->t0 + 0.06 * p->stall_at + p->stall_s;
            if (nominal < until)
                d = until - nominal;
        }
        if (k >= p->storm_from && k < p->storm_to)
            d = p->storm_s;
        s->at[k] = nominal + d;
        if (k && s->at[k] < s->at[k - 1])
            s->at[k] = s->at[k - 1];
        if (p->lose_every && k > 2 && k < s->n - 1 && k % p->lose_every == 0)
            s->lost[k] = true;
    }
}

/* ── what went out over the air ────────────────────────────────────────────── */

#define NAIR 4000

typedef struct {
    double start;
    int slot;
    uint8_t control;
    uint8_t burst[RDOF_BURST_BYTES];
} aired_t;

typedef struct {
    aired_t a[NAIR];
    int n;
    double free_at[3]; /* per slot: when the radio has sent all it holds */
} air_t;

static void air_write(air_t *air, double now, int slot, uint8_t control, const uint8_t *burst)
{
    double start = air->free_at[slot] > now ? air->free_at[slot] : now;
    air->free_at[slot] = start + 0.06;
    if (air->n < NAIR) {
        aired_t *e = &air->a[air->n++];
        e->start = start;
        e->slot = slot;
        e->control = control;
        memcpy(e->burst, burst, RDOF_BURST_BYTES);
    }
}

static int held(const air_t *air, int slot, double now)
{
    double h = air->free_at[slot] - now;
    return h > 1e-9 ? (int)ceil(h / 0.06 - 1e-9) : 0;
}

/* One call's voice on the air, as a listener gets it. */
typedef struct {
    int voice, real, silences, gaps, out_of_place, lc_bad, lc_ok;
    int first, last;          /* indices of the first and last real bursts */
    int missing;              /* real bursts the network delivered that never went out */
    double first_delay;       /* the first voice burst on air, after burst 0 left its source */
    double min_delay, max_delay, end_delay; /* real bursts: on air after leaving their source */
} heard_t;

static bool is_voice(uint8_t control)
{
    return !(control & RDOF_SYNC_DATA) && ((control & RDOF_SYNC_AUDIO) || ((control & 0x0F) >= 1 && (control & 0x0F) <= 5));
}

static void listen(const air_t *air, int slot, const stream_t *s, double from, double to, heard_t *h)
{
    memset(h, 0, sizeof *h);
    h->first = h->last = -1;
    h->min_delay = 1e9;
    h->max_delay = -1e9;
    static bool seen[NB];
    memset(seen, 0, sizeof seen);
    int prev_pos = -1;
    double prev_end = 0;
    uint8_t frags[5][32];
    int have = 0;
    for (int i = 0; i < air->n; i++) {
        const aired_t *e = &air->a[i];
        if (e->slot != slot || e->start < from || e->start >= to || !is_voice(e->control))
            continue;
        int pos = (e->control & RDOF_SYNC_AUDIO) ? 0 : e->control & 0x0F;
        if (h->voice) {
            if (e->start > prev_end + 1e-6)
                h->gaps++; /* the radio ran dry mid-call */
            if (pos != (prev_pos + 1) % 6)
                h->out_of_place++;
        } else {
            h->first_delay = e->start - s->t0;
        }
        h->voice++;
        prev_pos = pos;
        prev_end = e->start + 0.06;
        /* the frame: sync on A, an EMB for the position on B-F */
        if (pos) {
            int cc, pi, lcss;
            static const int lcss_of[6] = {0, 1, 3, 3, 2, 0};
            if (!rdof_decode_emb(e->burst, &cc, &pi, &lcss, NULL) || cc != CC || lcss != lcss_of[pos])
                h->out_of_place++;
            if (pos <= 4) {
                rdof_embedded_signalling(e->burst, frags[pos]);
                have |= 1 << pos;
            }
            if (pos == 4 && have == 0x1E) { /* a whole superframe's embedded LC */
                uint8_t raw[128], lc9[9];
                for (int f = 0; f < 4; f++)
                    memcpy(raw + 32 * f, frags[f + 1], 32);
                if (rdof_embedded_lc_decode(raw, lc9) && !memcmp(lc9, s->lc9, 9))
                    h->lc_ok++;
                else
                    h->lc_bad++;
            }
        } else {
            have = 0;
        }
        int k = burst_index(e->burst);
        if (k < 0) {
            h->silences++;
            continue;
        }
        h->real++;
        if (k < NB)
            seen[k] = true;
        if (h->first < 0)
            h->first = k;
        h->last = k;
        double d = e->start - (s->t0 + 0.06 * k);
        h->min_delay = d < h->min_delay ? d : h->min_delay;
        h->max_delay = d > h->max_delay ? d : h->max_delay;
        h->end_delay = d;
    }
    for (int k = 1; k < s->n - 1; k++)
        if (!s->lost[k] && !seen[k])
            h->missing++;
}

/* ── the transceiver on a radio that sends a burst every 60 ms ─────────────── */

typedef struct {
    fake_rig_t rig; /* first: the port's ctx */
    rdof_radio_t radio;
    rdof_txvr_t t;
    air_t air;
} tx_t;

static bool tx_write(void *ctx, const uint8_t *d, size_t n)
{
    tx_t *h = ctx;
    if (n > 4 + RDOF_BURST_BYTES - 1 && d[2] == RDOF_MMDVM_DMR_DATA2)
        air_write(&h->air, h->rig.clock, 2, d[3], d + 4);
    fake_write(&h->rig.radio, d, n);
    rig_pump(&h->rig);
    return true;
}

static void tx_sync(tx_t *h)
{
    h->rig.radio.held = held(&h->air, 2, h->rig.clock);
    h->rig.radio.transmitting = h->rig.radio.held > 0;
}

static tx_t *tx_new(bool adaptive, double listen_s)
{
    static tx_t h;
    memset(&h, 0, sizeof h);
    rig_init(&h.rig, "opengd77", NULL);
    rdof_modem_port_t port = {tx_write, rig_wait, NULL, rig_clock, NULL, &h.rig};
    rdof_modem_init(&h.rig.modem, &port);
    rdof_modem_start(&h.rig.modem, 1);
    rdof_radio_init(&h.radio, &h.rig.modem, RDOF_RADIO_HOTSPOT);
    rdof_txvr_t *t = &h.t;
    rdof_txvr_init(t, &h.radio, PEER);
    t->channels[0] = (rdof_channel_t){FREQ, CC, 255};
    t->nchannels = 1;
    t->listen_s = listen_s;
    t->jb.adaptive = adaptive;
    t->clock = rig_clock;
    t->ctx = &h.rig;
    rdof_txvr_start(t);
    return &h;
}

/* Poll every 20 ms and deliver what arrives, until `until`. */
static void tx_run(tx_t *h, const stream_t *s, uint32_t sid, double until)
{
    double p = ceil(h->rig.clock / 0.02) * 0.02;
    int i = 0;
    bool tuned = false;
    while (p < until) {
        while (i < s->n && s->lost[i])
            i++;
        double next = !tuned ? s->at[0] - 0.005 : i < s->n ? s->at[i] : INFINITY;
        if (next <= p) {
            h->rig.clock = next;
            if (!tuned) {
                rdof_tune_t tu = {.freq = FREQ, .cc = CC, .power = 255};
                rdof_put_be32(tu.stream_id, sid);
                rdof_txvr_on_tune(&h->t, &tu);
                tuned = true;
            } else {
                rdof_txvr_on_burst(&h->t, &s->b[i++]);
            }
            continue;
        }
        h->rig.clock = p;
        tx_sync(h);
        rdof_txvr_poll(&h->t);
        p += 0.02;
    }
}

/* One call through the transceiver: its stream made, spoiled by pattern, sent and heard; the jitter buffer's
 * counters it moved. */
typedef struct {
    heard_t h;
    int start;                 /* the prebuffer it started with */
    uint32_t underruns, grown, shrunk;
    double late_ms, jitter_ms;
} result_t;

static stream_t g_stream;

static void tx_call(tx_t *h, uint32_t sid, int superframes, const pattern_t *p, result_t *r)
{
    stream_t *s = &g_stream;
    double t0 = ceil(h->rig.clock) + 1.0;
    make_call(s, sid, superframes, 2, t0);
    apply(s, p);
    rdof_jb_t before = h->t.jb;
    int from = h->air.n;
    tx_run(h, s, sid, s->at[s->n - 1] + 4.0);
    memset(r, 0, sizeof *r);
    r->start = h->t.call.jbs.start;
    r->underruns = h->t.jb.underruns - before.underruns;
    r->grown = h->t.jb.grown - before.grown;
    r->shrunk = h->t.jb.shrunk - before.shrunk;
    r->late_ms = h->t.call.jbs.peak_ms;
    r->jitter_ms = h->t.jb.jitter_ms;
    listen(&h->air, 2, s, from < h->air.n ? h->air.a[from].start : t0, INFINITY, &r->h);
}

/* ── the repeater, likewise ───────────────────────────────────────────────── */

typedef struct {
    fake_radio_t f;
    rdof_modem_t m;
    rdof_radio_t radio;
    rdof_repeater_t r;
    double clock;
    air_t air;
} rp_t;

static bool rp_write(void *ctx, const uint8_t *d, size_t n)
{
    rp_t *h = ctx;
    if (n > 4 + RDOF_BURST_BYTES - 1 && (d[2] == RDOF_MMDVM_DMR_DATA1 || d[2] == RDOF_MMDVM_DMR_DATA2))
        air_write(&h->air, h->clock, d[2] == RDOF_MMDVM_DMR_DATA1 ? 1 : 2, d[3], d + 4);
    fake_write(&h->f, d, n);
    uint8_t buf[4096];
    size_t k;
    while ((k = fake_take(&h->f, buf, sizeof buf)) > 0)
        rdof_modem_feed(&h->m, buf, k);
    return true;
}

static bool rp_wait(void *ctx, int ms) { return ((rp_t *)ctx)->m.reply_ready; }
static double rp_clock(void *ctx) { return ((rp_t *)ctx)->clock; }

static rp_t *rp_new(bool adaptive)
{
    static rp_t h;
    memset(&h, 0, sizeof h);
    fake_init(&h.f, "mmdvm");
    h.clock = 1000.0;
    rdof_modem_port_t port = {rp_write, rp_wait, NULL, rp_clock, NULL, &h};
    rdof_modem_init(&h.m, &port);
    rdof_modem_start(&h.m, 1);
    rdof_radio_init(&h.radio, &h.m, RDOF_RADIO_MMDVM);
    h.radio.fixed_hz = 462550000;
    rdof_repeater_init(&h.r, &h.radio, PEER, 467550000, 462550000, CC);
    h.r.jb.adaptive = adaptive;
    h.r.clock = rp_clock;
    h.r.ctx = &h;
    rdof_repeater_start(&h.r);
    return &h;
}

static void rp_run(rp_t *h, const stream_t *s, double until)
{
    double p = ceil(h->clock / 0.02) * 0.02;
    int i = 0;
    while (p < until) {
        while (i < s->n && s->lost[i])
            i++;
        if (i < s->n && s->at[i] <= p) {
            h->clock = s->at[i];
            rdof_repeater_on_burst(&h->r, &s->b[i++]);
            continue;
        }
        h->clock = p;
        h->f.held1 = held(&h->air, 1, p);
        h->f.held = held(&h->air, 2, p);
        rdof_repeater_poll(&h->r);
        p += 0.02;
    }
}

static void rp_call(rp_t *h, uint32_t sid, int superframes, const pattern_t *p, double gap_s, result_t *r)
{
    stream_t *s = &g_stream;
    double t0 = h->clock + gap_s;
    make_call(s, sid, superframes, 2, t0);
    apply(s, p);
    rdof_jb_t before = h->r.jb;
    rp_run(h, s, s->at[s->n - 1] + 1.0);
    rdof_rpt_call_t *c = NULL;
    for (int i = 0; i < RDOF_RPT_CALLS; i++)
        if (h->r.calls[i].used && rdof_be32(h->r.calls[i].tune.stream_id) == sid)
            c = &h->r.calls[i];
    memset(r, 0, sizeof *r);
    r->start = c ? c->jbs.start : -1;
    r->underruns = h->r.jb.underruns - before.underruns;
    r->grown = h->r.jb.grown - before.grown;
    r->shrunk = h->r.jb.shrunk - before.shrunk;
    r->late_ms = c ? c->jbs.peak_ms : -1;
    r->jitter_ms = h->r.jb.jitter_ms;
    listen(&h->air, 2, s, t0, s->at[s->n - 1] + 1.0, &r->h);
}

static void show(const char *what, const result_t *r)
{
    if (getenv("RDOF_JB_VERBOSE"))
        printf("%-28s start %d underruns %u grown %u shrunk %u late %.0f jitter %.1f | voice %d real %d silence %d "
               "gaps %d misplaced %d lc %d/%d missing %d delay first %.3f min %.3f max %.3f end %.3f\n",
               what, r->start, r->underruns, r->grown, r->shrunk, r->late_ms, r->jitter_ms, r->h.voice, r->h.real,
               r->h.silences, r->h.gaps, r->h.out_of_place, r->h.lc_ok, r->h.lc_ok + r->h.lc_bad, r->h.missing,
               r->h.first_delay, r->h.min_delay, r->h.max_delay, r->h.end_delay);
}

/* Every call on air whole: no gap, every position in place, the embedded LC good. */
#define WHOLE(r, what)                                                                                       \
    CHECK((r).h.gaps == 0 && (r).h.out_of_place == 0 && (r).h.lc_bad == 0 && (r).h.lc_ok > 0,                \
          "%s: gaps %d misplaced %d lc %d bad %d", what, (r).h.gaps, (r).h.out_of_place, (r).h.lc_ok, (r).h.lc_bad)

/* ── the pieces ────────────────────────────────────────────────────────────── */

static void t_bursts(void)
{
    static const struct {
        double ms;
        int n;
    } v[] = {{0, 0}, {-5, 0}, {NAN, 0}, {1, 1}, {60, 1}, {60.5, 2}, {61, 2}, {180, 3}, {250, 5}, {480, 8},
             {3000, 50}, {5000, 50}};
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++)
        CHECK(rdof_jb_bursts(v[i].ms) == v[i].n, "%g ms: %d, want %d", v[i].ms, rdof_jb_bursts(v[i].ms), v[i].n);
}

/* n arrivals, burst k at t0 + 60 ms k + late[k] (ms), sequence numbers from seq0 by step. */
static void feed(rdof_jb_t *jb, rdof_jb_stream_t *s, int n, const double *late, int seq0, int step)
{
    for (int k = 0; k < n; k++)
        rdof_jb_arrival(jb, s, 10.0 + 0.06 * k + (late ? late[k] / 1000.0 : 0), seq0 + step * k);
}

static void t_measure(void)
{
    rdof_jb_t jb;
    rdof_jb_stream_t s;
    rdof_jb_init(&jb);
    CHECK(!jb.adaptive && jb.min == 3 && jb.max == 8, "init: %d %d %d", jb.adaptive, jb.min, jb.max);
    /* steady: nothing late, no jitter */
    rdof_jb_begin(&jb, &s);
    feed(&jb, &s, 40, NULL, 0, 1);
    CHECK(s.late_ms == 0 && s.jitter_ms < 1e-9 && s.n == 40 && s.nwin == 40, "steady: %g %g", s.late_ms, s.jitter_ms);
    /* one straggler doesn't set the size, two do */
    double late[80] = {0};
    late[10] = 150;
    rdof_jb_begin(&jb, &s);
    feed(&jb, &s, 40, late, 0, 1);
    CHECK(s.late_ms == 0 && s.peak_ms == 0, "one late: %g", s.late_ms);
    CHECK(s.jitter_ms > 0, "one late, jitter: %g", s.jitter_ms);
    late[20] = 100;
    rdof_jb_begin(&jb, &s);
    feed(&jb, &s, 40, late, 0, 1);
    CHECK(s.late_ms == 100 && s.peak_ms == 100, "two late: %g", s.late_ms);
    /* the window forgets: 64 later, gone; the peak stays */
    for (int k = 40; k < 40 + RDOF_JB_WINDOW; k++)
        rdof_jb_arrival(&jb, &s, 10.0 + 0.06 * k, k);
    CHECK(s.late_ms == 0 && s.peak_ms == 100, "forgotten: %g %g", s.late_ms, s.peak_ms);
    /* lateness counts from the earliest: the first burst late makes the rest early */
    memset(late, 0, sizeof late);
    late[0] = 200;
    rdof_jb_begin(&jb, &s);
    feed(&jb, &s, 10, late, 0, 1);
    CHECK(s.late_ms == 0, "first late, one: %g", s.late_ms);
    late[1] = 200;
    rdof_jb_begin(&jb, &s);
    feed(&jb, &s, 10, late, 0, 1);
    CHECK(s.late_ms == 200, "first two late: %g", s.late_ms);
    /* lost bursts, by the sequence numbers: on time still */
    rdof_jb_begin(&jb, &s);
    rdof_jb_arrival(&jb, &s, 10.0, 250);
    rdof_jb_arrival(&jb, &s, 10.06, 251);
    rdof_jb_arrival(&jb, &s, 10.30, 255); /* 252-254 lost */
    rdof_jb_arrival(&jb, &s, 10.42, 1);   /* 0 lost, across the wrap */
    rdof_jb_arrival(&jb, &s, 10.48, 2);
    CHECK(s.late_ms == 0 && s.idx == 8 && s.seq == 2, "lost: %g %d %d", s.late_ms, s.idx, s.seq);
    /* out of order: an earlier one, late; it doesn't move the count */
    rdof_jb_arrival(&jb, &s, 10.54, 3);
    rdof_jb_arrival(&jb, &s, 10.61, 0); /* 0 at 10.36 had it come: 250 ms late */
    CHECK(s.idx == 9 && s.seq == 3 && s.transit[(s.wnext + RDOF_JB_WINDOW - 1) % RDOF_JB_WINDOW] == 250,
          "out of order: %d %d %d", s.idx, s.seq, s.transit[(s.wnext + RDOF_JB_WINDOW - 1) % RDOF_JB_WINDOW]);
    /* no sequence numbers (all the same): a burst on each */
    rdof_jb_begin(&jb, &s);
    feed(&jb, &s, 20, NULL, 7, 0);
    CHECK(s.late_ms == 0 && s.idx == 19, "no seq: %g %d", s.late_ms, s.idx);
    /* lateness past the int16 window clamps */
    rdof_jb_begin(&jb, &s);
    rdof_jb_arrival(&jb, &s, 10.0, 0);
    rdof_jb_arrival(&jb, &s, 50.0, 1);
    rdof_jb_arrival(&jb, &s, 90.0, 2);
    CHECK(s.late_ms == 30000, "clamped: %g", s.late_ms);
}

static void t_targets(void)
{
    rdof_jb_t jb;
    rdof_jb_stream_t s;
    rdof_jb_init(&jb);
    /* fixed: the caller's prebuffer, whatever was measured */
    rdof_jb_begin(&jb, &s);
    CHECK(rdof_jb_prebuffer(&jb, &s, 6) == 6 && rdof_jb_prebuffer(&jb, &s, 2) == 2, "fixed");
    jb.adaptive = true;
    /* nothing known: 6 bursts, within the bounds */
    rdof_jb_begin(&jb, &s);
    CHECK(s.start == 6 && rdof_jb_prebuffer(&jb, &s, 2) == 6 && jb.target == 6, "unknown: %d", s.start);
    jb.max = 4;
    rdof_jb_begin(&jb, &s);
    CHECK(s.start == 4, "unknown, max 4: %d", s.start);
    jb.max = 8;
    jb.min = 7;
    rdof_jb_begin(&jb, &s);
    CHECK(s.start == 7, "unknown, min 7: %d", s.start);
    jb.min = 3;
    /* a call needing 100 ms: the next on the link starts at 2 + (110 / 60 up) = 4; its talkgroup the same */
    double late[40] = {0};
    late[5] = late[9] = 100;
    rdof_jb_begin(&jb, &s);
    rdof_jb_key(&jb, &s, TG, true, 2, false);
    feed(&jb, &s, 30, late, 0, 1);
    CHECK(s.target == 4 && jb.target == 4, "mid-call target: %d", s.target);
    rdof_jb_end(&jb, &s);
    CHECK(jb.has_link && jb.link_ms == 100, "link: %g", jb.link_ms);
    rdof_jb_begin(&jb, &s);
    CHECK(s.start == 4, "the link's: %d", s.start);
    /* another talkgroup needing 300 ms: 2 + 310 / 60 up = 8 for it; the link goes to 300 too */
    late[5] = late[9] = 300;
    rdof_jb_key(&jb, &s, 92, true, 2, false);
    feed(&jb, &s, 30, late, 0, 1);
    CHECK(s.target == 8, "300 ms: %d", s.target);
    rdof_jb_end(&jb, &s);
    CHECK(jb.link_ms == 300, "link up at once: %g", jb.link_ms);
    /* a clean call on TG 91: down by half */
    rdof_jb_begin(&jb, &s);
    rdof_jb_key(&jb, &s, TG, true, 2, false);
    CHECK(s.start == 4 && jb.target == 4, "TG 91 remembered: %d", s.start);
    feed(&jb, &s, 30, NULL, 0, 1);
    CHECK(s.target == 3, "clean: %d", s.target);
    rdof_jb_end(&jb, &s);
    rdof_jb_begin(&jb, &s);
    rdof_jb_key(&jb, &s, TG, true, 2, false);
    CHECK(s.start == 3, "TG 91 at 50 ms: %d", s.start);
    rdof_jb_end(&jb, &s); /* too short to remember */
    CHECK(jb.link_ms == 150, "the link down by half: %g", jb.link_ms);
    rdof_jb_begin(&jb, &s);
    rdof_jb_key(&jb, &s, 92, true, 2, false);
    CHECK(s.start == 8, "TG 92 remembered: %d", s.start);
    /* slot and group are part of the key */
    rdof_jb_begin(&jb, &s);
    rdof_jb_key(&jb, &s, 92, true, 1, false);
    CHECK(s.start == 5, "TG 92 slot 1: the link's (150 ms: 2 + 160 / 60 up): %d", s.start);
    rdof_jb_begin(&jb, &s);
    rdof_jb_key(&jb, &s, 92, false, 2, false);
    CHECK(s.start == 5, "private 92: the link's: %d", s.start);
    /* a key once started, or after enough arrivals, doesn't move the start */
    rdof_jb_begin(&jb, &s);
    rdof_jb_key(&jb, &s, 92, true, 2, true);
    CHECK(s.start == 5 && s.keyed, "started: %d", s.start);
    rdof_jb_key(&jb, &s, TG, true, 2, false);
    CHECK(s.dst == 92, "keyed once: %u", (unsigned)s.dst);
    /* eight talkgroups remembered; a ninth takes the one used longest ago (TG 91) */
    for (uint32_t g = 100; g < 107; g++) {
        rdof_jb_begin(&jb, &s);
        rdof_jb_key(&jb, &s, g, true, 2, false);
        feed(&jb, &s, 10, NULL, 0, 1);
        rdof_jb_end(&jb, &s);
    }
    int found91 = 0, found92 = 0;
    for (int i = 0; i < RDOF_JB_KEYS; i++) {
        found91 += jb.keys[i].used && jb.keys[i].dst == TG;
        found92 += jb.keys[i].used && jb.keys[i].dst == 92;
    }
    CHECK(!found91 && found92 == 1, "LRU: %d %d", found91, found92);
    /* streams counted */
    CHECK(jb.streams == 19, "streams: %u", jb.streams);
}

static void t_adapt(void)
{
    rdof_jb_t jb;
    rdof_jb_stream_t s;
    rdof_jb_init(&jb);
    /* fixed: underruns counted, nothing grows, every gap filled, nothing dropped */
    rdof_jb_begin(&jb, &s);
    CHECK(!rdof_jb_underrun(&jb, &s) && jb.underruns == 1 && !jb.grown, "fixed underrun");
    CHECK(rdof_jb_fill(&jb, &s, 3, 10) == 3 && rdof_jb_fill(&jb, &s, 0, 0) == 0, "fixed fill");
    CHECK(!rdof_jb_drop(&jb, &s, 20, true, 100), "fixed drop");
    jb.adaptive = true;
    jb.has_link = true;
    jb.link_ms = 0; /* starts at 3 */
    rdof_jb_begin(&jb, &s);
    CHECK(s.start == 3, "start: %d", s.start);
    /* grows to the maximum (8), then underruns are plain */
    int grew = 0;
    for (int i = 0; i < 7; i++)
        grew += rdof_jb_underrun(&jb, &s);
    CHECK(grew == 5 && s.shift == 5 && jb.grown == 5 && jb.underruns == 8, "grow: %d %d", grew, s.shift);
    /* a gap: filled up to the target (3: 2 ahead), the rest given back */
    CHECK(rdof_jb_fill(&jb, &s, 3, 0.5) == 2 && s.shift == 4 && jb.shrunk == 1, "fill 3 at 0.5: %d", s.shift);
    CHECK(rdof_jb_fill(&jb, &s, 2, 1.0) == 1 && s.shift == 3, "fill 2 at 1: %d", s.shift);
    CHECK(rdof_jb_fill(&jb, &s, 2, 2.0) == 0 && s.shift == 1, "fill 2 at 2: %d", s.shift);
    CHECK(rdof_jb_fill(&jb, &s, 1, 1.99) == 1 && s.shift == 1, "fill 1 at 1.99: %d", s.shift);
    CHECK(rdof_jb_fill(&jb, &s, -1, 0) == -1, "no gap");
    /* drops: not at the target; silence when ahead by a burst; voice after 2 s ahead, only what it grew */
    CHECK(!rdof_jb_drop(&jb, &s, 2.9, true, 100.0) && !s.above, "at the target");
    CHECK(rdof_jb_drop(&jb, &s, 3.0, true, 100.0) && s.shift == 0 && s.above, "silence ahead");
    CHECK(!rdof_jb_drop(&jb, &s, 3.5, false, 100.5), "voice, not grown");
    s.shift = 2;
    CHECK(!rdof_jb_drop(&jb, &s, 3.5, false, 101.99), "voice, 1.99 s ahead");
    CHECK(rdof_jb_drop(&jb, &s, 3.5, false, 102.0) && s.shift == 1, "voice, 2 s ahead");
    CHECK(!rdof_jb_drop(&jb, &s, 3.5, false, 103.0), "again: the clock restarted");
    CHECK(!rdof_jb_drop(&jb, &s, 1.0, false, 104.5) && !s.above, "back at the target");
    CHECK(!rdof_jb_drop(&jb, &s, 3.5, false, 105.0), "ahead again: from now");
    CHECK(rdof_jb_drop(&jb, &s, 3.5, false, 107.0) && s.shift == 0, "2 s later");
    CHECK(!rdof_jb_drop(&jb, &s, 9, false, 200.0), "nothing grown left");
    CHECK(jb.shrunk == 7, "shrunk: %u", jb.shrunk);
}

static void t_reframe(void)
{
    uint8_t lc9[9], raw[128], a[RDOF_BURST_BYTES], b[RDOF_BURST_BYTES], out[RDOF_BURST_BYTES];
    rdof_group_lc(SRC, TG, true, 0, lc9);
    rdof_embedded_lc_encode(lc9, raw);
    voice_burst(77, 0, lc9, a);
    voice_burst(78, 2, lc9, b);
    /* A to C with PI and C's fragment: AMBE kept, EMB and fragment for C */
    uint8_t c = rdof_jb_reframe(a, 2, 5, false, 1, raw + 32, out);
    int cc, pi, lcss;
    uint8_t emb[32];
    CHECK(c == 2 && rdof_decode_emb(out, &cc, &pi, &lcss, NULL) && cc == 5 && pi == 1 && lcss == 3, "A to C: %d %d %d",
          cc, pi, lcss);
    rdof_embedded_signalling(out, emb);
    CHECK(!memcmp(emb, raw + 32, 32) && burst_index(out) == 77, "A to C: fragment, AMBE");
    /* C to A, MS and BS sync */
    CHECK(rdof_jb_reframe(b, 0, CC, false, 0, NULL, out) == RDOF_SYNC_AUDIO &&
              rdof_get_field(out, 108, 48) == RDOF_MS_VOICE && burst_index(out) == 78,
          "C to A, MS");
    CHECK(rdof_jb_reframe(b, 6, CC, true, 0, NULL, out) == RDOF_SYNC_AUDIO && rdof_get_field(out, 108, 48) == RDOF_BS_VOICE,
          "past F: A, BS");
    /* to F: LCSS 0, the field zeros; in place */
    memcpy(out, b, sizeof out);
    CHECK(rdof_jb_reframe(out, 5, CC, false, 0, NULL, out) == 5 && rdof_decode_emb(out, &cc, &pi, &lcss, NULL) &&
              lcss == 0 && burst_index(out) == 78,
          "C to F in place: %d", lcss);
    rdof_embedded_signalling(out, emb);
    int ones = 0;
    for (int i = 0; i < 32; i++)
        ones += emb[i];
    CHECK(!ones, "F's field: %d ones", ones);
    /* silence */
    rdof_silence_burst(3, CC, false, NULL, out);
    CHECK(rdof_jb_silent(out) && !rdof_jb_silent(a), "silent");
    out[2] ^= 0x10; /* one frame not quite */
    CHECK(!rdof_jb_silent(out), "almost silent");
}

static void t_json(void)
{
    rdof_jb_t jb;
    rdof_jb_init(&jb);
    char buf[512];
    rdof_json_t j;
    rdof_json_init(&j, buf, sizeof buf);
    rdof_json_open(&j, NULL);
    rdof_jb_json(&jb, 6, &j, "jitter");
    rdof_json_close(&j);
    CHECK(rdof_json_done(&j) && !strcmp(buf, "{\"jitter\":{\"mode\":\"fixed\",\"target_ms\":360,\"jitter_ms\":0,\"late_ms\":0,"
                                          "\"streams\":0,\"underruns\":0,\"grown\":0,\"shrunk\":0}}"),
          "fixed: %s", buf);
    jb.adaptive = true;
    jb.max = 7;
    rdof_json_init(&j, buf, sizeof buf);
    rdof_json_open(&j, NULL);
    rdof_jb_json(&jb, 6, &j, "jitter");
    rdof_json_close(&j);
    CHECK(!strcmp(buf, "{\"jitter\":{\"mode\":\"auto\",\"target_ms\":360,\"jitter_ms\":0,\"late_ms\":0,\"min_ms\":180,"
                       "\"max_ms\":420,\"streams\":0,\"underruns\":0,\"grown\":0,\"shrunk\":0}}"),
          "auto: %s", buf);
    rdof_jb_stream_t s;
    rdof_jb_begin(&jb, &s);
    double late[20] = {0, 0, 0, 0, 120, 0, 0, 130};
    feed(&jb, &s, 20, late, 0, 1);
    rdof_jb_underrun(&jb, &s);
    rdof_json_init(&j, buf, sizeof buf);
    rdof_json_open(&j, NULL);
    rdof_jb_json(&jb, 6, &j, "jitter");
    rdof_json_close(&j);
    CHECK(!strcmp(buf, "{\"jitter\":{\"mode\":\"auto\",\"target_ms\":300,\"jitter_ms\":14,\"late_ms\":120,\"min_ms\":180,"
                       "\"max_ms\":420,\"streams\":1,\"underruns\":1,\"grown\":1,\"shrunk\":0}}"),
          "auto, a stream: %s", buf);
}

/* ── the transceiver ───────────────────────────────────────────────────────── */

/* The start the jitter buffer picks after a call that needed late_ms (2 bursts' lead and the lateness, 3 to 8). */
static int after(double late_ms)
{
    int n = RDOF_JB_LEAD + rdof_jb_bursts(late_ms + RDOF_JB_MARGIN_MS);
    return n < 3 ? 3 : n > 8 ? 8 : n;
}

static void t_txvr_steady(void)
{
    /* a clean link: the first call at 6 (nothing known), every later one at the minimum, 3 */
    tx_t *h = tx_new(true, 0);
    pattern_t p = {0};
    result_t r;
    tx_call(h, 1, 20, &p, &r);
    show("txvr steady, first", &r);
    WHOLE(r, "steady, first");
    CHECK(r.start == 6 && r.underruns == 0 && r.h.silences == 0 && r.h.missing == 0 && r.h.real == 120,
          "first: %d %u %d %d", r.start, r.underruns, r.h.silences, r.h.missing);
    tx_call(h, 2, 20, &p, &r);
    show("txvr steady, second", &r);
    WHOLE(r, "steady, second");
    CHECK(r.start == 3 && r.underruns == 0 && r.h.silences == 0 && r.h.missing == 0, "second: %d %u %d %d", r.start,
          r.underruns, r.h.silences, r.h.missing);
    /* latency near the minimum: 3 bursts in hand (header and two voice), the first voice on air after the header */
    CHECK(fabs(r.h.max_delay - 0.14) < 0.021 && fabs(r.h.max_delay - r.h.min_delay) < 1e-6, "delay %.3f..%.3f", r.h.min_delay,
          r.h.max_delay);
    /* fixed, the same link: 6, 180 ms more, every call */
    h = tx_new(false, 0);
    tx_call(h, 3, 20, &p, &r);
    tx_call(h, 4, 20, &p, &r);
    show("txvr steady, fixed", &r);
    WHOLE(r, "steady, fixed");
    CHECK(fabs(r.h.max_delay - 0.32) < 0.021 && r.h.silences == 0, "fixed: %.3f", r.h.max_delay);
}

static void t_txvr_jitter(void)
{
    result_t r;
    /* 0-100 ms late, at random, after the first two superframes (on time: the worst case for a prebuffer): 6 is
     * plenty; the next call starts at 4 and has no underrun either */
    tx_t *h = tx_new(true, 0);
    pattern_t p = {.jitter_ms = 100, .jitter_from = 12, .seed = 7};
    tx_call(h, 1, 30, &p, &r);
    show("txvr jitter 100, first", &r);
    WHOLE(r, "jitter 100, first");
    CHECK(r.underruns == 0 && r.h.missing == 0 && r.late_ms > 80 && r.late_ms <= 100, "first: %u %d %g", r.underruns,
          r.h.missing, r.late_ms);
    p.seed = 8;
    tx_call(h, 2, 30, &p, &r);
    show("txvr jitter 100, second", &r);
    WHOLE(r, "jitter 100, second");
    CHECK(r.start == 4 && r.underruns == 0 && r.h.silences == 0 && r.h.missing == 0 && r.h.max_delay < 0.2 + 0.021,
          "second: %d %u %d %.3f", r.start, r.underruns, r.h.silences, r.h.max_delay);
    /* 0-300 ms (under the most 8 bursts cover, 360 ms): the first call underruns and grows, losing nothing; the next
     * starts where the first needed, and has none. Calls after that converge. */
    h = tx_new(true, 0);
    p = (pattern_t){.jitter_ms = 300, .jitter_from = 12, .seed = 11};
    tx_call(h, 1, 30, &p, &r);
    show("txvr jitter 300, first", &r);
    WHOLE(r, "jitter 300, first");
    CHECK(r.underruns > 0 && r.grown == r.underruns && r.h.missing == 0, "first: %u %u %d", r.underruns, r.grown,
          r.h.missing);
    int want = after(r.late_ms), starts[4];
    for (int i = 0; i < 4; i++) {
        p.seed = (uint32_t)(12 + i);
        tx_call(h, (uint32_t)(2 + i), 30, &p, &r);
        show("txvr jitter 300, later", &r);
        WHOLE(r, "jitter 300, later");
        if (!i)
            CHECK(r.start == want, "second: start %d, want %d", r.start, want);
        starts[i] = r.start;
        CHECK(r.underruns == 0 && r.h.silences == 0 && r.h.missing == 0, "call %d: %u %d %d", i + 2, r.underruns,
              r.h.silences, r.h.missing);
    }
    CHECK(starts[2] == starts[3] && starts[3] >= 7, "converged: %d %d %d %d", starts[0], starts[1], starts[2], starts[3]);
    /* the same, fixed at 6: underruns every call, each late burst dropped */
    h = tx_new(false, 0);
    p.seed = 11;
    tx_call(h, 1, 30, &p, &r);
    p.seed = 12;
    tx_call(h, 2, 30, &p, &r);
    show("txvr jitter 300, fixed", &r);
    WHOLE(r, "jitter 300, fixed");
    CHECK(r.underruns > 10 && r.grown == 0 && r.h.missing == (int)r.underruns, "fixed: %u %d", r.underruns, r.h.missing);
    /* past the most (0-700 ms): it grows to 8, then late bursts go as when fixed; the air never runs dry */
    h = tx_new(true, 0);
    p = (pattern_t){.jitter_ms = 700, .jitter_from = 12, .seed = 5};
    tx_call(h, 1, 30, &p, &r);
    show("txvr jitter 700", &r);
    WHOLE(r, "jitter 700");
    CHECK(r.start == 6 && r.grown == 2 && r.underruns > r.grown && r.h.missing > 0, "past the max: %u %u", r.grown,
          r.underruns);
}

static void t_txvr_events(void)
{
    result_t r;
    /* a stall: nothing for 400 ms mid-call, then all of it at once. The silence filled in moves the stream back
     * (grown, up to the most: 8), and what it grew is given back slowly, a voice burst every 2 s at most once the
     * stall has left the 3.84 s window. Lost: the one late burst past the most, and the ones given back. */
    tx_t *h = tx_new(true, 0);
    pattern_t p = {0};
    tx_call(h, 1, 10, &p, &r); /* learn the link: start at 3 */
    p.stall_at = 60;
    p.stall_s = 0.4;
    tx_call(h, 2, 60, &p, &r);
    show("txvr stall 400 ms", &r);
    WHOLE(r, "stall");
    CHECK(r.start == 3 && r.underruns == 6 && r.grown == 5 && r.shrunk == 5 && r.h.missing == 1 + 5,
          "stall: %u %u %u %d", r.underruns, r.grown, r.shrunk, r.h.missing);
    CHECK(fabs(r.h.end_delay - 0.14) < 0.021 && r.h.max_delay > 0.4, "given back: %.3f %.3f", r.h.end_delay,
          r.h.max_delay);
    /* the next call learned from it: it starts bigger */
    p = (pattern_t){0};
    tx_call(h, 3, 10, &p, &r);
    CHECK(r.start == 8, "after the stall: %d", r.start);
    /* a 250 ms late storm: 30 bursts late, then on time again (bunched up behind the last late one) */
    h = tx_new(true, 0);
    p = (pattern_t){0};
    tx_call(h, 1, 10, &p, &r);
    p = (pattern_t){.storm_from = 50, .storm_to = 80, .storm_s = 0.25};
    tx_call(h, 2, 60, &p, &r);
    show("txvr storm 250 ms", &r);
    WHOLE(r, "storm");
    CHECK(r.underruns == 3 && r.grown == 3 && r.shrunk == 3 && r.h.missing == 3, "storm: %u %u %u %d", r.underruns,
          r.grown, r.shrunk, r.h.missing);
    /* bursts lost (every tenth): silence in their place keeps the timing; nothing grows (the last, before the
     * terminator, has nothing after it to show it lost) */
    h = tx_new(true, 0);
    p = (pattern_t){0};
    tx_call(h, 1, 10, &p, &r);
    p.lose_every = 10;
    tx_call(h, 2, 30, &p, &r);
    show("txvr loss 1 in 10", &r);
    WHOLE(r, "loss");
    CHECK(r.underruns == 0 && r.grown == 0 && r.shrunk == 0 && r.h.missing == 0 && r.h.silences == 17,
          "loss: %u %u %d", r.underruns, r.grown, r.h.silences);
    /* loss and jitter together: underruns grow it; nothing lost the network sent */
    h = tx_new(true, 0);
    p = (pattern_t){.jitter_ms = 100, .jitter_from = 12, .lose_every = 7, .seed = 3};
    tx_call(h, 1, 30, &p, &r);
    p.seed = 4;
    tx_call(h, 2, 30, &p, &r);
    show("txvr loss and jitter", &r);
    WHOLE(r, "loss and jitter");
    CHECK(r.h.missing == 0 && r.h.max_delay < 0.3, "loss and jitter: %d %.3f", r.h.missing, r.h.max_delay);
    /* listen before talk: what comes meanwhile is held, nothing lost, no underrun */
    h = tx_new(true, 0.3);
    p = (pattern_t){0};
    tx_call(h, 1, 10, &p, &r);
    tx_call(h, 2, 20, &p, &r);
    show("txvr listen 0.3 s", &r);
    WHOLE(r, "listen");
    CHECK(r.underruns == 0 && r.h.missing == 0 && r.h.silences == 0, "listen: %u %d", r.underruns, r.h.missing);
}

/* ── the repeater ──────────────────────────────────────────────────────────── */

static void t_repeater(void)
{
    result_t r;
    rp_t *h = rp_new(true);
    pattern_t p = {0};
    rp_call(h, 1, 20, &p, 1.0, &r);
    show("rpt steady, first", &r);
    WHOLE(r, "rpt steady, first");
    CHECK(r.start == 6 && r.underruns == 0 && r.h.silences == 0 && r.h.missing == 0, "first: %d %u %d", r.start,
          r.underruns, r.h.silences);
    rp_call(h, 2, 20, &p, 1.0, &r); /* still keyed: no key-up lead; the 3 headers ahead of the voice */
    show("rpt steady, second", &r);
    WHOLE(r, "rpt steady, second");
    CHECK(r.start == 3 && r.underruns == 0 && r.h.silences == 0 && r.h.missing == 0 && fabs(r.h.max_delay - 0.26) < 0.021,
          "second: %d %u %d %.3f", r.start, r.underruns, r.h.silences, r.h.max_delay);
    p = (pattern_t){.jitter_ms = 300, .jitter_from = 12, .seed = 21};
    rp_call(h, 3, 30, &p, 1.0, &r);
    show("rpt jitter 300, first", &r);
    WHOLE(r, "rpt jitter 300, first");
    CHECK(r.start == 3 && r.underruns > 0 && r.grown == r.underruns && r.h.missing == 0, "jitter 300: %u %u %d",
          r.underruns, r.grown, r.h.missing);
    int want = after(r.late_ms);
    p.seed = 22;
    rp_call(h, 4, 30, &p, 1.0, &r);
    show("rpt jitter 300, second", &r);
    WHOLE(r, "rpt jitter 300, second");
    CHECK(r.start == want && r.underruns == 0 && r.h.missing == 0, "jitter 300, second: %d %u", r.start, r.underruns);
    /* a stall, and loss, on a clean link */
    h = rp_new(true);
    p = (pattern_t){0};
    rp_call(h, 1, 10, &p, 1.0, &r);
    p = (pattern_t){.stall_at = 60, .stall_s = 0.4};
    rp_call(h, 2, 60, &p, 1.0, &r);
    show("rpt stall", &r);
    WHOLE(r, "rpt stall");
    CHECK(r.start == 3 && r.grown == r.underruns && r.grown >= 4 && r.shrunk == r.grown && r.h.missing == (int)r.shrunk &&
              fabs(r.h.end_delay - 0.26) < 0.021,
          "stall: %u %u %u %d %.3f", r.underruns, r.grown, r.shrunk, r.h.missing, r.h.end_delay);
    h = rp_new(true);
    p = (pattern_t){0};
    rp_call(h, 1, 10, &p, 1.0, &r);
    /* the repeater's own two extra headers (R4.2) put it two ahead of its target: the first two lost bursts' silence
     * isn't needed, and goes (two bursts less delay); then silence for each */
    p = (pattern_t){.lose_every = 10};
    rp_call(h, 2, 30, &p, 1.0, &r);
    show("rpt loss", &r);
    WHOLE(r, "rpt loss");
    CHECK(r.underruns == 0 && r.grown == 0 && r.shrunk == 2 && r.h.missing == 0 && r.h.silences == 15 &&
              fabs(r.h.end_delay - 0.14) < 0.021,
          "loss: %u %u %d %.3f", r.underruns, r.shrunk, r.h.silences, r.h.end_delay);
    /* fixed: the old behavior, late bursts dropped */
    h = rp_new(false);
    p = (pattern_t){0};
    rp_call(h, 1, 10, &p, 1.0, &r); /* keyed: no lead for the next */
    p = (pattern_t){.jitter_ms = 450, .jitter_from = 12, .seed = 21};
    rp_call(h, 2, 30, &p, 1.0, &r);
    show("rpt jitter 450, fixed", &r);
    /* (a superframe all underrun silence carries zero embedded fields, which decode as an LC: the next superframe
     * sends that; as before) */
    CHECK(r.h.gaps == 0 && r.h.out_of_place == 0 && r.h.lc_bad <= 1, "fixed: %d %d %d", r.h.gaps, r.h.out_of_place,
          r.h.lc_bad);
    CHECK(r.underruns > 10 && r.grown == 0 && r.h.missing > 10, "fixed: %u %d", r.underruns, r.h.missing);
}

void test_jitter(void)
{
    t_bursts();
    t_measure();
    t_targets();
    t_adapt();
    t_reframe();
    t_json();
    t_txvr_steady();
    t_txvr_jitter();
    t_txvr_events();
    t_repeater();
}

#else
void test_jitter(void) {}
#endif
