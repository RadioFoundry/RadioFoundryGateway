// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Mode-neutral 4FSK as a signal mode (rdof/sigmode.h): any 4-level FSK air interface (P25 phase 1's C4FM, DMR, NXDN,
 * dPMR) as raw symbols. The bench sends the client's dibits on the carrier through a chosen pulse shape and measures
 * a radio's 4FSK on receive. The framing is the client's: nothing here knows a mode's frames. The JSON is bench-api.md
 * section 14.6's.
 *
 * Symbols: dibits packed 4 to a byte, the first in bits 7-6; 01 = +3, 00 = +1, 10 = -1, 11 = -3 (the mapping P25 and
 * DMR share). A symbol a is a deviation of a dev_hz / 3 at its center.
 *
 * Pulse shapes, each a frequency pulse g(t) (t in symbols) with the symbol's level at its center after the receive
 * filter the shape assumes:
 *  - c4fm: the raised cosine (roll-off 0.2) times the shaping filter pi f T / sin(pi f T) (P25 phase 1's C4FM
 *    modulator): behind a one-symbol integrate-and-dump the response is the raised cosine, so the levels are exact at
 *    the symbol centers;
 *  - rrc: the square-root raised cosine (roll-off 0.2), for a receiver's matched RRC (DMR, NXDN);
 *  - rc: the raised cosine itself, exact at the centers with no receive filter;
 *  - nrz: a rectangle one symbol long.
 * The pulse is a table over 16 symbols either side (RRC; 12 for c4fm and rc, whose tails fall faster), 64 points a
 * symbol, from the shape's spectrum (a numerical inverse Fourier transform), scaled so that its response at the
 * center behind the receive filter is exactly 1. Cut at 8 symbols, an RRC's tails put about 45 Hz of intersymbol
 * interference on DMR's levels.
 *
 * Peaks: C4FM at 1800 Hz reaches about 4400 Hz on random symbols (the raised cosine's overshoot times the shaping
 * filter's lift), over a W12's 4064 Hz. clip_hz limits the deviation (the report counts the samples cut); symbols
 * given in the request set the peak from themselves.
 */
#include "rdof/sdr/modes.h"

#include "rdof/sdr/features.h"
#if RDOF_FEATURE_SDR

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/sdr/fm.h"
#include "sdr_private.h"

#define PH 64                    /* table points a symbol */
#define SPAN 16                  /* symbols either side, at most (the table) */
#define TAB (PH * SPAN + 1)      /* g(0) .. g(SPAN), g even */
_Static_assert(PH == 64, "the render takes the phase row from the fraction's top 6 bits");
#define ALPHA 0.2                /* the roll-off by default (rolloff) */
#define RAMP_S 0.002             /* the envelope's raised-cosine ramp, key-up and key-down */
#define MAX_BYTES (256u * 1024u) /* a million symbols: 218 s at 4800 */
#define WAIT_S 2.0               /* for the symbols to come in before the first key-up */

enum { SH_C4FM, SH_RRC, SH_RC, SH_NRZ, SH_GAUSS, SH_N };
static const char *const shape_names[SH_N] = {"c4fm", "rrc", "rc", "nrz", "gauss"};

/* The raised cosine's spectrum (T = 1, area 1), roll-off a, at f >= 0 */
static double rc_spec(double f, double a)
{
    const double f1 = (1 - a) / 2, f2 = (1 + a) / 2;
    if (f <= f1)
        return 1;
    if (f >= f2)
        return 0;
    return 0.5 * (1 + cos(RDOF_PI / a * (f - f1)));
}

/* The pulse's spectrum at f >= 0 */
static double spec(int shape, double f, double a)
{
    const double h = rc_spec(f, a);
    if (shape == SH_RRC)
        return sqrt(h);
    if (shape == SH_C4FM && f > 0)
        return h * (RDOF_PI * f) / sin(RDOF_PI * f);
    return h;
}

/* g at t = i / PH, i = 0 .. TAB-1, scaled to 1 behind the shape's receive filter; a: the roll-off (c4fm, rrc, rc),
 * bt: the Gaussian's bandwidth-time product (gauss) */
static void build_pulse(int shape, double a, double bt, float *g)
{
    if (shape == SH_NRZ) {
        for (int i = 0; i < TAB; i++)
            g[i] = i < PH / 2 ? 1.0f : i == PH / 2 ? 0.5f : 0.0f;
        return;
    }
    if (shape == SH_GAUSS) { /* a one-symbol rectangle through the Gaussian (GMSK's): a long run reaches exactly 1 */
        const double k = RDOF_PI * bt * sqrt(2 / log(2.0));
        for (int i = 0; i < TAB; i++) {
            const double t = (double)i / PH;
            g[i] = (float)(0.5 * (erf(k * (t + 0.5)) - erf(k * (t - 0.5))));
        }
        return;
    }
    /* g(t) = 2 * integral over 0 .. (1 + a) / 2 of spec(f) cos(2 pi f t) df, by Simpson's rule */
    enum { NF = 240 };
    const double fmax = (1 + a) / 2, df = fmax / NF;
    float w[NF + 1];
    for (int k = 0; k <= NF; k++)
        w[k] = (float)(spec(shape, k * df, a) * (k == 0 || k == NF ? 1 : k % 2 ? 4 : 2) * df / 3 * 2);
    for (int i = 0; i < TAB; i++) {
        const float t = (float)i / PH;
        float s = 0;
        for (int k = 0; k <= NF; k++)
            s += w[k] * cosf(2.0f * (float)RDOF_PI * (float)(k * df) * t);
        g[i] = s;
    }
    /* the response at the center behind the receive filter: rc g(0); c4fm the mean over the middle symbol;
     * rrc g convolved with itself at 0, the integral of g squared */
    double r = 0;
    if (shape == SH_RC) {
        r = g[0];
    } else if (shape == SH_C4FM) {
        for (int i = -PH / 2; i <= PH / 2; i++)
            r += g[abs(i)] * (abs(i) == PH / 2 ? 0.5 : 1.0);
        r /= PH;
    } else {
        for (int i = -(TAB - 1); i < TAB; i++)
            r += (double)g[abs(i)] * g[abs(i)];
        r = sqrt(r / PH);
    }
    for (int i = 0; i < TAB; i++)
        g[i] = (float)(g[i] / r);
}

static int span_of(int shape) { return shape == SH_RRC ? 16 : shape == SH_NRZ ? 1 : shape == SH_GAUSS ? 4 : 12; }

/* g(t), t in symbols, cut at span */
static inline float pulse(const float *g, float t, int span)
{
    const float u = fabsf(t) * PH;
    if (u >= (float)(span * PH))
        return 0;
    const int i = (int)u;
    const float f = u - (float)i;
    return g[i] + (g[i + 1] - g[i]) * f;
}

/* ── transmit ── */

typedef struct {
    int shape, span;
    int bits, shb;               /* bits a symbol (2: dibits; 1: two levels, +-3); log2 of the symbols in a byte */
    double rolloff, bt;
    double clip_hz;              /* the deviation's limit (0: none) */
    uint64_t clipped;            /* samples cut to it */
    double baud, dev_hz, scale, offset_hz, lead_s, tail_s, gap_s, fs;
    unsigned repeat;
    uint8_t *sym;                /* the dibits, packed */
    uint32_t nsym, nbytes, have; /* symbols; bytes expected, bytes in */
    float g[TAB];
    /* the pulse by phase: row p (0 .. PH) holds tap m (0 .. 2 span - 1) = g(p / PH - (m - span + 1)), the weight of
     * symbol c + m - span + 1 at fraction p / PH past symbol c's center; the render interpolates between rows */
    float *rows;
    double sps;                  /* samples a symbol */
    uint64_t step;               /* symbols a sample, 32.32 fixed point */
    /* the timeline, in samples: each burst lead + its symbols + tail, keyed; a gap off between bursts */
    unsigned bursts, per_burst;  /* bursts; plays of the symbols in each */
    uint64_t lead, body, tail, burst, gap, ramp, total;
    uint64_t pos;                /* samples rendered, after the wait */
    uint64_t waited;             /* samples held off (carrier off) waiting for the symbols */
    uint64_t wait_max;
    double peak;
    bool done;
    /* stream: true, the symbols as they come, for up to seconds. A ring of ahead_s's bytes, one writer (the stream's
     * input) and one reader (the render), paces the client; when it runs dry, the idle block plays (whole, from its
     * start) or, without one, level 0; each time it does is an underrun. */
    bool stream, ready, ended, starved, in_idle;
    uint8_t *ring, *idle;
    uint32_t cap, nidle, idle_pos;
    atomic_uint_fast32_t rw, rr; /* bytes written, read */
    atomic_bool eof;
    uint8_t cur;
    int sub;                     /* the dibit next in cur (4: a byte needed) */
    int8_t lvh[256];             /* the levels decided, by symbol index */
    uint64_t decided, kend, stop_at, max_q;
    uint64_t sym_in, fill, zero;
    double seconds;
    atomic_uint_fast32_t underruns, under_k; /* how many; the last one's symbol */
    uint32_t reported;
} fsk4_tx_t;

/* Symbol j of a byte as a level: dibits 00 01 10 11 are +1 +3 -1 -3; single bits 1 and 0 are +3 and -3 */
static int8_t sym_level(const fsk4_tx_t *t, uint8_t byte, unsigned j)
{
    static const int8_t lv[4] = {1, 3, -1, -3};
    if (t->bits == 1)
        return byte >> (7 - j) & 1 ? 3 : -3;
    return lv[byte >> (6 - 2 * j) & 3];
}

static int dibit(const fsk4_tx_t *t, uint32_t k)
{
    return sym_level(t, t->sym[k >> t->shb], k & ((1u << t->shb) - 1));
}

/* Symbols given as hex in the request (as long as the request line allows): NULL, or why not */
static const char *hex_in(fsk4_tx_t *t, const char *hex, double nsym)
{
    const size_t L = strlen(hex);
    if (L % 2)
        return "fsk4 hex: whole bytes";
    t->nbytes = (uint32_t)(L / 2);
    if (!(t->sym = rdof_sdr_alloc(t->nbytes)))
        return "no memory for the symbols";
    for (size_t i = 0; i < L; i += 2) {
        unsigned v;
        if (sscanf(hex + i, "%2x", &v) != 1)
            return "fsk4 hex: hex digits";
        t->sym[i / 2] = (uint8_t)v;
    }
    t->have = t->nbytes;
    t->nsym = nsym > 0 && nsym <= (double)(t->nbytes << t->shb) ? (uint32_t)nsym : t->nbytes << t->shb;
    return NULL;
}

static const char *stream_cfg(fsk4_tx_t *t, const rdof_jdoc_t *d, int job)
{
    t->seconds = rdof_sig_num(d, job, "seconds", 0);
    const double ahead = rdof_sig_num(d, job, "ahead_s", 0.25);
    if (!(t->seconds > 0 && t->seconds <= 3600))
        return "fsk4 stream: seconds (0-3600), the most it keys";
    if (!(ahead >= 0.02 && ahead <= 10))
        return "fsk4 stream: ahead_s 0.02-10";
    uint32_t c = (uint32_t)ceil(ahead * t->baud / (1 << t->shb));
    t->cap = c < 16 ? 16 : c;
    if (!(t->ring = rdof_sdr_alloc(t->cap)))
        return "no memory for the symbols";
    const rdof_jnode_t *in = rdof_jnode(d, rdof_jget(d, job, "idle_hex"));
    if (in && in->type == RDOF_JSTR && in->str[0]) {
        const size_t L = strlen(in->str);
        if (L % 2 || L > 2 * 4096)
            return "fsk4 idle_hex: whole bytes, 4096 at most";
        if (!(t->idle = rdof_sdr_alloc(L / 2)))
            return "no memory for the idle block";
        for (size_t i = 0; i < L; i += 2) {
            unsigned v;
            if (sscanf(in->str + i, "%2x", &v) != 1)
                return "fsk4 idle_hex: hex digits";
            t->idle[i / 2] = (uint8_t)v;
        }
        t->nidle = (uint32_t)(L / 2) << t->shb;
    }
    t->sub = 4;
    return NULL;
}

static const char *fsk4_cfg(fsk4_tx_t *t, const rdof_jdoc_t *d, int job, char *why, size_t cap)
{
    char shape[16];
    if (!rdof_sig_text(d, job, "shape", shape, sizeof shape))
        snprintf(shape, sizeof shape, "c4fm");
    t->shape = -1;
    for (int i = 0; i < SH_N; i++)
        if (!strcmp(shape, shape_names[i]))
            t->shape = i;
    if (t->shape < 0)
        return "fsk4 shape: c4fm, rrc, rc, nrz or gauss";
    t->rolloff = rdof_sig_num(d, job, "rolloff", ALPHA);
    t->bt = rdof_sig_num(d, job, "bt", 0.5);
    t->bits = (int)rdof_sig_num(d, job, "bits", t->shape == SH_GAUSS ? 1 : 2);
    if (!(t->rolloff >= 0.05 && t->rolloff <= 1) || !(t->bt >= 0.2 && t->bt <= 1) || (t->bits != 1 && t->bits != 2))
        return "fsk4 rolloff: 0.05-1; bt: 0.2-1; bits: 1 or 2";
    t->shb = t->bits == 1 ? 3 : 2;
    t->baud = rdof_sig_num(d, job, "baud", 4800);
    t->dev_hz = rdof_sig_num(d, job, "dev_hz", t->shape == SH_RRC ? 1944 : t->shape == SH_GAUSS ? 1200 : 1800);
    if (rdof_sig_flag(d, job, "invert"))
        t->scale = -1; /* below: the sign on dev_scale_pct's */
    t->scale = (t->scale < 0 ? -1 : 1) * rdof_sig_num(d, job, "dev_scale_pct", 100) / 100;
    t->offset_hz = rdof_sig_num(d, job, "offset_hz", 0);
    t->lead_s = rdof_sig_num(d, job, "lead_s", 0.01);
    t->tail_s = rdof_sig_num(d, job, "tail_s", 0.01);
    t->gap_s = rdof_sig_num(d, job, "gap_s", 0);
    t->clip_hz = rdof_sig_num(d, job, "clip_hz", 0);
    if (!(t->clip_hz >= 0 && t->clip_hz <= 20000))
        return "fsk4 clip_hz: 0 (none) to 20000";
    const double rep = rdof_sig_num(d, job, "repeat", 1);
    if (!(t->baud >= 600 && t->baud <= 9600))
        return "fsk4 baud: 600 to 9600";
    if (!(t->dev_hz > 0 && t->dev_hz <= 5000) || !(fabs(t->scale) > 0 && fabs(t->scale) <= 2))
        return "fsk4 dev_hz: up to 5000; dev_scale_pct 1-200";
    if (!(fabs(t->offset_hz) <= 5000))
        return "fsk4 offset_hz: -5000 to 5000";
    if (!(t->lead_s >= 0 && t->lead_s <= 10) || !(t->tail_s >= 0 && t->tail_s <= 10) ||
        !(t->gap_s >= 0 && t->gap_s <= 60))
        return "fsk4 lead_s, tail_s: 0-10; gap_s 0-60";
    if (!(rep >= 1 && rep <= 10000))
        return "fsk4 repeat: 1 to 10000";
    t->repeat = (unsigned)rep;
    t->stream = rdof_sig_flag(d, job, "stream");
    if (t->stream)
        return stream_cfg(t, d, job);
    /* the symbols: hex in the request, or a stream of `symbols` after it */
    const double nsym = rdof_sig_num(d, job, "symbols", 0);
    const rdof_jnode_t *hn = rdof_jnode(d, rdof_jget(d, job, "hex"));
    if (hn && hn->type == RDOF_JSTR && hn->str[0])
        return hex_in(t, hn->str, nsym);
    if (!(nsym >= 1 && nsym <= (double)(MAX_BYTES << t->shb))) {
        snprintf(why, cap, "fsk4: hex, or symbols (1 to %u) and the symbols after the request", MAX_BYTES << t->shb);
        return why;
    }
    t->nsym = (uint32_t)nsym;
    t->nbytes = (t->nsym + (1u << t->shb) - 1) >> t->shb;
    if (!(t->sym = rdof_sdr_zalloc(t->nbytes)))
        return "no memory for the symbols";
    return NULL;
}

/* The largest |deviation| (Hz): from the symbols when they're all here (one play, 16 points a symbol), else the
 * worst any symbols could reach; clip_hz caps it */
static double peak_hz(const fsk4_tx_t *t)
{
    const double unit = t->dev_hz * t->scale / 3;
    double worst = 0;
    if (!t->stream && t->have == t->nbytes && t->nsym <= 8192) {
        /* two plays when they go back to back: the join too */
        const int64_t n = (int64_t)t->nsym * (t->repeat > 1 && t->gap_s == 0 ? 2 : 1);
        for (int64_t i = -16 * t->span; i < 16 * (n + t->span); i++) {
            const int64_t c = i >= 0 ? i / 16 : -((-i + 15) / 16);
            const float fu = (float)(i - 16 * c) / 16;
            double s = 0;
            for (int64_t k = c - t->span; k <= c + t->span; k++)
                if (k >= 0 && k < n)
                    s += dibit(t, (uint32_t)(k % t->nsym)) * pulse(t->g, fu - (float)(k - c), t->span);
            worst = fmax(worst, fabs(s * unit + t->offset_hz));
        }
    } else {
        for (int p = 0; p < PH; p++) {
            double s = 0;
            for (int k = -t->span; k <= t->span; k++)
                s += fabs(pulse(t->g, (float)p / PH + (float)k, t->span));
            worst = fmax(worst, s * 3 * fabs(unit) + fabs(t->offset_hz));
        }
    }
    return t->clip_hz > 0 && worst > t->clip_hz ? t->clip_hz : worst;
}

static void *fsk4_tx_new(const rdof_jdoc_t *d, int job, double fs, double *seconds, char *error, size_t ecap)
{
    fsk4_tx_t *t = rdof_sdr_zalloc(sizeof *t);
    if (!t) {
        snprintf(error, ecap, "no memory for a 4FSK transmitter");
        return NULL;
    }
    const char *why = fsk4_cfg(t, d, job, error, ecap);
    if (why) {
        if (why != error)
            snprintf(error, ecap, "%s", why);
        rdof_sdr_free(t->sym), rdof_sdr_free(t->ring), rdof_sdr_free(t->idle);
        rdof_sdr_free(t);
        return NULL;
    }
    if (!(fs >= 8 * t->baud)) {
        snprintf(error, ecap, "fsk4: the transmitter's %.0f S/s is under 8 samples a symbol", fs);
        rdof_sdr_free(t->sym), rdof_sdr_free(t->ring), rdof_sdr_free(t->idle);
        rdof_sdr_free(t);
        return NULL;
    }
    t->fs = fs;
    t->sps = fs / t->baud;
    t->span = span_of(t->shape);
    build_pulse(t->shape, t->rolloff, t->bt, t->g);
    const int nt = 2 * t->span;
    t->rows = rdof_sdr_zalloc(sizeof(float) * (size_t)(PH + 1) * (size_t)nt);
    if (!t->rows) {
        snprintf(error, ecap, "no memory for a 4FSK transmitter");
        rdof_sdr_free(t->sym), rdof_sdr_free(t->ring), rdof_sdr_free(t->idle);
        rdof_sdr_free(t);
        return NULL;
    }
    for (int p = 0; p <= PH; p++)
        for (int m = 0; m < nt; m++)
            t->rows[p * nt + m] = pulse(t->g, (float)p / PH - (float)(m - t->span + 1), t->span);
    t->step = (uint64_t)llround(t->baud / fs * 4294967296.0);
    t->peak = peak_hz(t);
    if (t->stream) {
        t->ramp = (uint64_t)llround(RAMP_S * fs);
        t->lead = (uint64_t)llround(t->lead_s * fs) + t->ramp;
        t->tail = (uint64_t)llround(t->tail_s * fs) + t->ramp;
        t->max_q = (uint64_t)llround(t->seconds * fs);
        t->wait_max = (uint64_t)llround(WAIT_S * fs);
        *seconds = t->seconds + WAIT_S;
        return t;
    }
    /* gap_s 0: the plays back to back in one keying; else each its own keying, gap_s apart */
    t->bursts = t->gap_s > 0 ? t->repeat : 1;
    t->per_burst = t->gap_s > 0 ? 1 : t->repeat;
    t->ramp = (uint64_t)llround(RAMP_S * fs);
    t->lead = (uint64_t)llround(t->lead_s * fs) + t->ramp;
    t->body = (uint64_t)ceil((double)t->nsym * t->per_burst * t->sps);
    t->tail = (uint64_t)llround(t->tail_s * fs) + t->ramp;
    t->burst = t->lead + t->body + t->tail;
    t->gap = (uint64_t)llround(t->gap_s * fs);
    t->total = t->burst * t->bursts + t->gap * (t->bursts - 1);
    t->wait_max = t->have < t->nbytes ? (uint64_t)llround(WAIT_S * fs) : 0;
    *seconds = (double)t->total / fs + (t->wait_max ? WAIT_S : 0);
    return t;
}

static void fsk4_tx_free(void *tx)
{
    fsk4_tx_t *t = tx;
    if (!t)
        return;
    rdof_sdr_free(t->sym);
    rdof_sdr_free(t->rows);
    rdof_sdr_free(t->ring), rdof_sdr_free(t->idle);
    rdof_sdr_free(t);
}

/* ── stream ── */

/* The next symbol decided: the client's, the idle block's or 0; or none more (ended) */
static void stream_decide(fsk4_tx_t *t)
{
    int8_t a = 0;
    if (t->in_idle) { /* an idle block plays whole */
        a = sym_level(t, t->idle[t->idle_pos >> t->shb], t->idle_pos & ((1u << t->shb) - 1));
        if (++t->idle_pos == t->nidle)
            t->in_idle = false, t->idle_pos = 0;
        t->fill++;
    } else {
        const int per = 1 << t->shb;
        if (t->sub == per) {
            const uint_fast32_t w = atomic_load_explicit(&t->rw, memory_order_acquire);
            const uint_fast32_t r = atomic_load_explicit(&t->rr, memory_order_relaxed);
            if (w != r) {
                t->cur = t->ring[r % t->cap];
                atomic_store_explicit(&t->rr, r + 1, memory_order_release);
                t->sub = 0;
            }
        }
        if (t->sub < per) {
            a = sym_level(t, t->cur, (unsigned)t->sub);
            t->sub++, t->sym_in++, t->starved = false;
        } else if (atomic_load_explicit(&t->eof, memory_order_acquire) &&
                   atomic_load_explicit(&t->rw, memory_order_acquire) ==
                       atomic_load_explicit(&t->rr, memory_order_relaxed)) {
            t->ended = true, t->kend = t->decided;
            return;
        } else { /* dry: an underrun */
            if (!t->starved) {
                t->starved = true;
                atomic_store_explicit(&t->under_k, (uint_fast32_t)t->decided, memory_order_relaxed);
                atomic_fetch_add_explicit(&t->underruns, 1, memory_order_release);
            }
            if (t->nidle) {
                t->in_idle = true, t->idle_pos = 0;
                stream_decide(t);
                return;
            }
            t->zero++;
        }
    }
    t->lvh[t->decided & 255] = a;
    t->decided++;
}

static int fsk4_freq_stream(void *ctx, float *hz, float *env, int n)
{
    fsk4_tx_t *t = ctx;
    const float unit = (float)(t->dev_hz * t->scale / 3), off = (float)t->offset_hz;
    int keyed = 0;
    for (int i = 0; i < n; i++) {
        if (!t->ready) { /* the ring full (or the stream over) before the key-up, for up to 2 s, carrier off */
            const uint_fast32_t w = atomic_load_explicit(&t->rw, memory_order_acquire);
            if (w - atomic_load_explicit(&t->rr, memory_order_relaxed) < t->cap &&
                !atomic_load_explicit(&t->eof, memory_order_acquire) && t->waited < t->wait_max) {
                t->waited++;
                hz[i] = 0;
                if (env)
                    env[i] = 0;
                keyed++;
                continue;
            }
            t->ready = true;
        }
        if (t->done || (t->stop_at && t->pos >= t->stop_at + t->tail)) {
            t->done = true;
            hz[i] = 0;
            if (env)
                env[i] = 0;
            continue;
        }
        keyed++;
        const uint64_t q = t->pos++;
        float e = 1;
        if (q < t->ramp)
            e = 0.5f - 0.5f * cosf((float)RDOF_PI * (float)(q + 1) / (float)(t->ramp + 1));
        else if (t->stop_at && q >= t->stop_at + t->tail - t->ramp)
            e = 0.5f - 0.5f * cosf((float)RDOF_PI * (float)(t->stop_at + t->tail - q) / (float)(t->ramp + 1));
        if (env)
            env[i] = e;
        const int64_t u = ((int64_t)q - (int64_t)t->lead) * (int64_t)t->step - ((int64_t)1 << 31);
        const int64_t c = u >> 32;
        const uint32_t fr = (uint32_t)u;
        const int p = (int)(fr >> 26);
        const float w = (float)(fr & 0x3ffffff) * (1.0f / 67108864.0f);
        /* the most it keys: the last symbols' pulses and the tail inside it */
        if (!t->ended && (double)(q + t->tail) + (t->span + 1) * t->sps >= (double)t->max_q)
            t->ended = true, t->kend = t->decided;
        while (!t->ended && (int64_t)t->decided <= c + t->span)
            stream_decide(t);
        if (t->ended && !t->stop_at && u + ((int64_t)1 << 31) >= (int64_t)(t->kend << 32))
            t->stop_at = q; /* the symbols' time over, as a job with hex ends; the tail carries their pulses' ends */
        const int nt = 2 * t->span;
        const float *r0 = t->rows + p * nt, *r1 = r0 + nt;
        int64_t k0 = c - t->span + 1, k1 = c + t->span;
        if (k0 < 0)
            k0 = 0;
        if (k1 >= (int64_t)t->decided)
            k1 = (int64_t)t->decided - 1;
        float s0 = 0, s1 = 0;
        for (int64_t k = k0; k <= k1; k++) {
            const int m = (int)(k - c) + t->span - 1;
            const float a = t->lvh[k & 255];
            s0 += a * r0[m], s1 += a * r1[m];
        }
        float h = (s0 + (s1 - s0) * w) * unit + off;
        if (t->clip_hz > 0 && fabsf(h) > t->clip_hz) {
            h = h > 0 ? (float)t->clip_hz : -(float)t->clip_hz;
            t->clipped++;
        }
        hz[i] = h;
    }
    return keyed;
}

static int fsk4_freq(void *ctx, float *hz, float *env, int n)
{
    fsk4_tx_t *t = ctx;
    const float unit = (float)(t->dev_hz * t->scale / 3), off = (float)t->offset_hz;
    const uint64_t played = (uint64_t)t->nsym * t->per_burst;
    int keyed = 0;
    for (int i = 0; i < n; i++) {
        if (t->pos == 0 && t->have < t->nbytes && t->waited < t->wait_max) {
            /* the symbols aren't all in yet: carrier off, still keyed, a while */
            t->waited++;
            hz[i] = 0;
            if (env)
                env[i] = 0;
            keyed++;
            continue;
        }
        if (t->pos >= t->total) {
            hz[i] = 0;
            if (env)
                env[i] = 0;
            t->done = true;
            continue;
        }
        keyed++;
        const uint64_t q = t->pos % (t->burst + t->gap);
        t->pos++;
        if (q >= t->burst) { /* between bursts: off */
            hz[i] = 0;
            if (env)
                env[i] = 0;
            continue;
        }
        float e = 1;
        if (q < t->ramp)
            e = 0.5f - 0.5f * cosf((float)RDOF_PI * (float)(q + 1) / (float)(t->ramp + 1));
        else if (q >= t->burst - t->ramp)
            e = 0.5f - 0.5f * cosf((float)RDOF_PI * (float)(t->burst - q) / (float)(t->ramp + 1));
        if (env)
            env[i] = e;
        /* time from the first symbol's center, symbols in 32.32 fixed point (no doubles here: the ESP32-S3 has no
         * double FPU): symbol c, then the fraction as a phase row p and a weight w toward row p + 1 */
        const int64_t u = ((int64_t)q - (int64_t)t->lead) * (int64_t)t->step - ((int64_t)1 << 31);
        const int64_t c = u >> 32; /* floor */
        const uint32_t fr = (uint32_t)u;
        const int p = (int)(fr >> 26); /* PH = 64 rows */
        const float w = (float)(fr & 0x3ffffff) * (1.0f / 67108864.0f);
        const int nt = 2 * t->span;
        const float *r0 = t->rows + p * nt, *r1 = r0 + nt;
        int64_t k0 = c - t->span + 1, k1 = c + t->span;
        if (k0 < 0)
            k0 = 0;
        if (k1 >= (int64_t)played)
            k1 = (int64_t)played - 1;
        float s0 = 0, s1 = 0;
        uint32_t j = k0 <= k1 ? (uint32_t)(k0 % t->nsym) : 0;
        for (int64_t k = k0; k <= k1; k++) {
            if ((j >> t->shb) < t->have) {
                const int m = (int)(k - c) + t->span - 1;
                const float a = (float)dibit(t, j);
                s0 += a * r0[m];
                s1 += a * r1[m];
            }
            if (++j == t->nsym)
                j = 0;
        }
        const float s = s0 + (s1 - s0) * w;
        float h = s * unit + off;
        if (t->clip_hz > 0 && fabsf(h) > t->clip_hz) {
            h = h > 0 ? (float)t->clip_hz : -(float)t->clip_hz;
            t->clipped++;
        }
        hz[i] = h;
    }
    return keyed;
}

static bool fsk4_done(void *ctx) { return ((fsk4_tx_t *)ctx)->done; }

static rdof_sig_src_t fsk4_tx_src(void *tx)
{
    fsk4_tx_t *t = tx;
    return (rdof_sig_src_t){.freq = t->stream ? fsk4_freq_stream : fsk4_freq, .done = fsk4_done, .peak_hz = t->peak,
                            .ctx = t};
}

static uint64_t fsk4_tx_in_want(void *tx)
{
    fsk4_tx_t *t = tx;
    if (t->stream) { /* until the client ends it: as much as the job could play */
        if (atomic_load(&t->eof))
            return 0;
        const uint64_t most = (uint64_t)ceil(t->seconds * t->baud / (1 << t->shb)) + 1,
                       w = atomic_load_explicit(&t->rw, memory_order_relaxed);
        return w < most ? most - w : 1;
    }
    return t->nbytes - t->have;
}

static size_t fsk4_tx_in_room(void *tx)
{
    fsk4_tx_t *t = tx;
    if (t->stream)
        return atomic_load(&t->eof) ? 0 : t->cap - (size_t)(atomic_load_explicit(&t->rw, memory_order_relaxed) -
                                                       atomic_load_explicit(&t->rr, memory_order_acquire));
    return (size_t)fsk4_tx_in_want(tx);
}

static size_t fsk4_tx_in(void *tx, const uint8_t *b, size_t n)
{
    fsk4_tx_t *t = tx;
    if (t->stream) {
        const size_t room = fsk4_tx_in_room(tx), k = n < room ? n : room;
        uint_fast32_t w = atomic_load_explicit(&t->rw, memory_order_relaxed);
        for (size_t i = 0; i < k; i++)
            t->ring[(w + i) % t->cap] = b[i];
        atomic_store_explicit(&t->rw, w + (uint_fast32_t)k, memory_order_release);
        return k;
    }
    const size_t k = n < t->nbytes - t->have ? n : t->nbytes - t->have;
    memcpy(t->sym + t->have, b, k);
    t->have += (uint32_t)k;
    return k;
}

static void fsk4_tx_in_end(void *tx, bool gone)
{
    fsk4_tx_t *t = tx;
    (void)gone;
    if (t->stream) {
        atomic_store_explicit(&t->eof, true, memory_order_release);
        return;
    }
    t->wait_max = 0; /* nothing more comes: what's missing goes as 0 */
}

/* An underrun since the last record: how many so far, the last one's symbol and time from the first symbol */
static size_t fsk4_tx_record(void *tx, char *out, size_t cap)
{
    fsk4_tx_t *t = tx;
    if (!t->stream)
        return 0;
    const uint32_t u = (uint32_t)atomic_load_explicit(&t->underruns, memory_order_acquire);
    if (u == t->reported)
        return 0;
    t->reported = u;
    const uint32_t k = (uint32_t)atomic_load_explicit(&t->under_k, memory_order_relaxed);
    const int n = snprintf(out, cap, "{\"underrun\":{\"n\":%u,\"k\":%u,\"t\":%.4f,\"fill\":\"%s\"}}", (unsigned)u, (unsigned)k,
                           k / t->baud, t->nidle ? "idle" : "zero");
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

static size_t fsk4_tx_report(void *tx, char *out, size_t cap)
{
    const fsk4_tx_t *t = tx;
    if (t->stream) {
        const int n = snprintf(out, cap,
                               ",\"shape\":\"%s\",\"rolloff\":%.2f,\"bt\":%.2f,\"bits\":%d,\"baud\":%.1f,"
                               "\"dev_hz\":%.1f,\"peak_hz\":%.0f,\"offset_hz\":%.1f,\"stream\":true,\"ahead_bytes\":%u,\"symbols\":%llu,\"symbols_in\":%llu,"
                               "\"underruns\":%u,\"idle_symbols\":%llu,\"zero_symbols\":%llu,\"wait_s\":%.3f,"
                               "\"clip_hz\":%.0f,\"clipped\":%llu",
                               shape_names[t->shape], t->rolloff, t->bt, t->bits, t->baud, fabs(t->dev_hz * t->scale),
                               t->peak, t->offset_hz, (unsigned)t->cap,
                               (unsigned long long)t->decided, (unsigned long long)t->sym_in,
                               (unsigned)atomic_load(&t->underruns), (unsigned long long)t->fill,
                               (unsigned long long)t->zero, (double)t->waited / t->fs, t->clip_hz,
                               (unsigned long long)t->clipped);
        return n > 0 && (size_t)n < cap ? (size_t)n : 0;
    }
    const uint32_t in = (t->have << t->shb) < t->nsym ? t->have << t->shb : t->nsym;
    const int n = snprintf(out, cap,
                           ",\"shape\":\"%s\",\"rolloff\":%.2f,\"bt\":%.2f,\"bits\":%d,\"baud\":%.1f,"
                           "\"dev_hz\":%.1f,\"peak_hz\":%.0f,\"offset_hz\":%.1f,\"symbols\":%u,\"repeat\":%u,\"bursts\":%u,\"seconds\":%.3f,\"symbols_in\":%u,"
                           "\"symbols_short\":%llu,\"wait_s\":%.3f,\"clip_hz\":%.0f,\"clipped\":%llu",
                           shape_names[t->shape], t->rolloff, t->bt, t->bits, t->baud, fabs(t->dev_hz * t->scale), t->peak,
                           t->offset_hz, (unsigned)t->nsym, t->repeat, t->bursts, (double)t->total / t->fs, (unsigned)in,
                           (unsigned long long)(t->nsym - in) * t->repeat, (double)t->waited / t->fs, t->clip_hz,
                           (unsigned long long)t->clipped);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

/* ── receive ──
 *
 * The discriminator, then the receive filter (c4fm: a one-symbol integrate-and-dump; rrc: the matched RRC, cut at
 * RX_SPAN symbols; rc and nrz: none), a squelch on the power in 1 ms blocks, and a search for the client's sync
 * (Pearson correlation of symbol-spaced samples with the pattern) while the carrier is up. A sync sets the symbol
 * timing and the fit of the carrier offset and the level unit; each later one re-anchors both (P25 and NXDN repeat
 * theirs every frame, so the clocks' difference never builds up). Symbols are sliced from the sync's first on, until
 * the carrier drops; those after its last 1 ms block are dropped.
 */
#define RX_SPAN 4            /* RRC receive filter: symbols either side */
#define RX_HANG 3            /* 1 ms blocks under the squelch before the carrier is off */
#define RX_CHUNK 1024        /* symbols a dibits record */
#define RX_RING 4096         /* filtered samples kept (a power of 2) */
#define RX_PEND 512          /* symbols sliced, not yet known to be inside the carrier */
#define SYNC_MAX 64
#define RX_Q 32768           /* records waiting */

enum { F_NONE, F_IAD, F_RRC };
static const char *const filt_names[] = {"none", "iad", "rrc"};
enum { S_DISC = 1, S_FILT = 2 };

typedef struct {
    double t;
    float y;
} rx_sym_t;

typedef struct {
    double fs, baud, sps, delay; /* delay: the filter's, samples */
    int filter, L;
    unsigned streams;
    float sq_lin, sync_min, scale;
    rdof_cf_t prev;
    float *h, *hist;             /* FIR taps, history (twice nh, for a straight dot product) */
    int nh, hi;
    float idsum;                 /* integrate-and-dump: running sum over hist's nh */
    float *disc, *filt;          /* scratch when a stream isn't asked for */
    int room;
    float ring[RX_RING];
    uint64_t pos;                /* samples in */
    /* squelch */
    int blk_len, blk_n, below;
    float blk_p;
    bool on;
    uint64_t on_at, on_end;      /* the carrier's first sample, the end of its last block over the squelch */
    double pw_sum;
    uint64_t pw_n;
    /* sync */
    float sc[SYNC_MAX], snorm, smean;
    int off[SYNC_MAX];
    float cw[128];               /* correlations from the first over sync_min on, a sample apart */
    int ncw;
    uint64_t cw_p;               /* cw[0]'s first-symbol sample */
    /* timing */
    bool locked;
    double next, anchor;         /* the next symbol's sample position (filtered), the last sync's */
    uint64_t k, anchor_k;        /* the next symbol's index from the keying's first sync */
    double first_p;
    double tail_s, tail_ss, tail_lv[4]; /* trailing symbols near 0 (bare carrier): out of the stats if it ends so */
    double tail_v[4];
    int tail_n;
    rx_sym_t post[4];            /* after 8 or more near 0: the carrier's death, as likely as not */
    int npost;
    uint64_t first_k;
    float cfo, unit;
    rx_sym_t pend[RX_PEND];
    int npend;
    /* dibits being collected */
    uint8_t chunk[RX_CHUNK / 4];
    int nchunk;
    uint64_t chunk_k, ncommit;
    double chunk_t;
    /* this keying */
    double lv_n[4], lv_s[4], lv_ss[4]; /* by level -3 -1 +1 +3 */
    unsigned syncs;
    double sync_t;
    /* all */
    unsigned keyings, syncs_all, dropped;
    uint64_t symbols_all;
    char *q;
    size_t qn;
} fsk4_rx_t;

static void rx_push(fsk4_rx_t *r, const char *s, size_t n)
{
    if (!n || r->qn + n + 1 > RX_Q) {
        r->dropped++;
        return;
    }
    memcpy(r->q + r->qn, s, n);
    r->q[r->qn + n] = '\n';
    r->qn += n + 1;
}

static double rx_t(const fsk4_rx_t *r, double p) { return (p - r->delay) / r->fs; }

static void rx_flush(fsk4_rx_t *r)
{
    if (!r->nchunk)
        return;
    char s[64 + RX_CHUNK / 2];
    int n = snprintf(s, sizeof s, "{\"dibits\":{\"t\":%.6f,\"k\":%llu,\"n\":%d,\"hex\":\"", r->chunk_t,
                     (unsigned long long)r->chunk_k, r->nchunk);
    for (int i = 0; i < (r->nchunk + 3) / 4; i++)
        n += snprintf(s + n, sizeof s - (size_t)n, "%02x", r->chunk[i]);
    n += snprintf(s + n, sizeof s - (size_t)n, "\"}}");
    rx_push(r, s, (size_t)n);
    r->nchunk = 0;
}

/* The symbols held near 0 into the stats (keep), or out of them */
static void rx_untail(fsk4_rx_t *r, bool keep)
{
    if (keep && r->tail_n)
        for (int i = 0; i < 4; i++) {
            r->lv_n[i] += r->tail_lv[i], r->lv_s[i] += r->tail_v[i];
            /* their squares by level aren't kept apart: shared out by count (they sit within 0.5 of 0) */
            r->lv_ss[i] += r->tail_ss * r->tail_lv[i] / r->tail_n;
        }
    memset(r->tail_lv, 0, sizeof r->tail_lv), memset(r->tail_v, 0, sizeof r->tail_v);
    r->tail_ss = 0, r->tail_n = 0;
}

static void rx_stat(fsk4_rx_t *r, float y)
{
    const float a = (y - r->cfo) / r->unit;
    const int li = a > 2 ? 3 : a > 0 ? 2 : a > -2 ? 1 : 0;
    r->lv_n[li]++, r->lv_s[li] += y, r->lv_ss[li] += (double)y * y;
}

static void rx_commit(fsk4_rx_t *r, const rx_sym_t *s)
{
    const float a = (s->y - r->cfo) / r->unit;
    const int li = a > 2 ? 3 : a > 0 ? 2 : a > -2 ? 1 : 0; /* -3 -1 +1 +3 */
    static const uint8_t bits[4] = {3, 2, 0, 1};
    if (!r->nchunk) {
        memset(r->chunk, 0, sizeof r->chunk);
        r->chunk_k = r->ncommit, r->chunk_t = s->t;
    }
    r->chunk[r->nchunk / 4] |= (uint8_t)(bits[li] << (6 - 2 * (r->nchunk % 4)));
    if (fabsf(a) < 0.5f && !r->npost) { /* bare carrier, as likely as not: held until a symbol that isn't */
        r->tail_lv[li]++, r->tail_v[li] += s->y, r->tail_ss += (double)s->y * s->y, r->tail_n++;
    } else if (r->tail_n >= 8 && r->npost < 4) { /* the end of the carrier, or data again: held a few */
        r->post[r->npost++] = (rx_sym_t){s->t, s->y};
    } else {
        rx_untail(r, true);
        rx_stat(r, s->y);
        for (int i = 0; i < r->npost; i++)
            rx_stat(r, r->post[i].y);
        r->npost = 0;
    }
    r->ncommit++, r->symbols_all++;
    if (++r->nchunk == RX_CHUNK)
        rx_flush(r);
}

/* Commit the symbols up to time t, drop the rest when drop */
static void rx_settle(fsk4_rx_t *r, double t, bool drop)
{
    int i = 0;
    while (i < r->npend && r->pend[i].t <= t)
        rx_commit(r, &r->pend[i++]);
    r->npend = drop ? 0 : r->npend - i;
    if (!drop && i)
        memmove(r->pend, r->pend + i, sizeof r->pend[0] * (size_t)r->npend);
}

static void rx_keying_start(fsk4_rx_t *r, uint64_t at)
{
    r->on = true, r->on_at = at, r->below = 0;
    r->pw_sum = 0, r->pw_n = 0;
    r->locked = false, r->npend = 0, r->nchunk = 0, r->ncommit = 0, r->syncs = 0, r->sync_t = -1;
    rx_untail(r, false);
    r->npost = 0;
    memset(r->lv_n, 0, sizeof r->lv_n), memset(r->lv_s, 0, sizeof r->lv_s), memset(r->lv_ss, 0, sizeof r->lv_ss);
}

static void rx_keying_end(fsk4_rx_t *r)
{
    const double t_off = (double)r->on_end / r->fs, t_on = (double)r->on_at / r->fs;
    rx_settle(r, t_off, true);
    rx_flush(r);
    /* 8 or more near 0 at the end, then up to 4 others: the carrier's bare tail and its death, out of the stats */
    const bool keep = r->tail_n < 8;
    for (int i = 0; keep && i < r->npost; i++)
        rx_stat(r, r->post[i].y);
    r->npost = 0;
    rx_untail(r, keep);
    r->on = false, r->locked = false, r->ncw = 0;
    if (!r->syncs && t_off - t_on < 0.005) /* a blip */
        return;
    r->keyings++;
    double cfo = r->cfo, outer = 3 * r->unit;
    if (r->lv_n[0] && r->lv_n[3]) {
        const double lo = r->lv_s[0] / r->lv_n[0], hi = r->lv_s[3] / r->lv_n[3];
        cfo = (lo + hi) / 2, outer = (hi - lo) / 2;
    }
    char s[1024];
    int n = snprintf(s, sizeof s, "{\"keying\":{\"t_on\":%.6f,\"t_off\":%.6f,\"s\":%.4f,\"power_dbfs\":%.1f,", t_on,
                     t_off, t_off - t_on, r->pw_n ? 10 * log10(r->pw_sum / (double)r->pw_n + 1e-30) : -200.0);
    if (r->syncs) {
        double e2 = 0, nn = 0;
        n += snprintf(s + n, sizeof s - (size_t)n, "\"sync_ms\":%.2f,\"syncs\":%u,\"symbols\":%llu,\"cfo_hz\":%.1f,"
                      "\"dev_hz\":%.1f,\"levels_hz\":[", (r->sync_t - t_on) * 1000, r->syncs,
                      (unsigned long long)r->ncommit, cfo, outer);
        for (int i = 0; i < 4; i++) {
            const double m = r->lv_n[i] ? r->lv_s[i] / r->lv_n[i] : NAN;
            n += snprintf(s + n, sizeof s - (size_t)n, i ? ",%.1f" : "%.1f", r->lv_n[i] ? m - cfo : 0.0);
            /* error from the ideal level, cfo + (2i - 3) outer / 3 */
            const double id = cfo + (2 * i - 3) * outer / 3;
            e2 += r->lv_ss[i] - 2 * id * r->lv_s[i] + r->lv_n[i] * id * id, nn += r->lv_n[i];
        }
        n += snprintf(s + n, sizeof s - (size_t)n, "],\"sd_hz\":[");
        for (int i = 0; i < 4; i++) {
            const double m = r->lv_n[i] ? r->lv_s[i] / r->lv_n[i] : 0;
            const double v = r->lv_n[i] > 1 ? (r->lv_ss[i] - r->lv_n[i] * m * m) / (r->lv_n[i] - 1) : 0;
            n += snprintf(s + n, sizeof s - (size_t)n, i ? ",%.1f" : "%.1f", sqrt(v > 0 ? v : 0));
        }
        n += snprintf(s + n, sizeof s - (size_t)n, "],\"counts\":[%.0f,%.0f,%.0f,%.0f],\"evm_pct\":%.2f,",
                      r->lv_n[0], r->lv_n[1], r->lv_n[2], r->lv_n[3],
                      nn && outer > 0 ? 100 * sqrt(fmax(e2, 0) / nn) / outer : 0.0);
        if (r->anchor_k > r->first_k)
            n += snprintf(s + n, sizeof s - (size_t)n, "\"baud_err_ppm\":%.1f,",
                          (r->sps * (double)(r->anchor_k - r->first_k) / (r->anchor - r->first_p) - 1) * 1e6);
        else
            n += snprintf(s + n, sizeof s - (size_t)n, "\"baud_err_ppm\":null,");
    } else {
        n += snprintf(s + n, sizeof s - (size_t)n, "\"sync_ms\":null,\"syncs\":0,\"symbols\":0,");
    }
    n += snprintf(s + n, sizeof s - (size_t)n, "\"filter\":\"%s\",\"baud\":%.1f}}", filt_names[r->filter], r->baud);
    rx_push(r, s, (size_t)n);
}

static float rx_at(const fsk4_rx_t *r, double p)
{
    const double f = floor(p);
    const uint64_t i = (uint64_t)f;
    const float w = (float)(p - f), a = r->ring[i & (RX_RING - 1)], b = r->ring[(i + 1) & (RX_RING - 1)];
    return a + (b - a) * w;
}

/* A sync whose first symbol is at filtered sample p0 */
static void rx_sync(fsk4_rx_t *r, double p0, float corr)
{
    float y[SYNC_MAX], sy = 0;
    for (int i = 0; i < r->L; i++)
        y[i] = rx_at(r, p0 + i * r->sps), sy += y[i] * r->sc[i];
    const float unit = sy / r->snorm;
    if (!(unit > 0))
        return;
    float ym = 0;
    for (int i = 0; i < r->L; i++)
        ym += y[i];
    r->cfo = ym / (float)r->L - unit * r->smean, r->unit = unit;
    uint64_t kk = 0;
    if (!r->locked) {
        r->locked = true;
        r->first_p = p0, r->first_k = 0;
        r->k = 0, r->next = p0;
        r->sync_t = rx_t(r, p0);
    } else {
        const double d = (p0 - r->anchor) / r->sps;
        kk = r->anchor_k + (uint64_t)llround(d > 0 ? d : 0);
        if (kk > r->k)
            kk = r->k; /* can't start after symbols not yet sliced */
        r->next = p0 + (double)(r->k - kk) * r->sps;
    }
    r->anchor = p0, r->anchor_k = kk;
    r->syncs++, r->syncs_all++;
    char s[200];
    const int n = snprintf(s, sizeof s, "{\"sync\":{\"t\":%.6f,\"k\":%llu,\"corr\":%.3f,\"cfo_hz\":%.1f,\"dev_hz\":%.1f}}",
                           rx_t(r, p0), (unsigned long long)kk, corr, r->cfo, 3 * unit);
    rx_push(r, s, (size_t)n);
}

static float rx_filter(fsk4_rx_t *r, float x)
{
    if (r->filter == F_NONE)
        return x;
    if (r->filter == F_IAD) {
        r->idsum += x - r->hist[r->hi];
        r->hist[r->hi] = x;
        if (++r->hi == r->nh) {
            r->hi = 0;
            float s = 0; /* the running sum's rounding, cleared once a pass */
            for (int i = 0; i < r->nh; i++)
                s += r->hist[i];
            r->idsum = s;
        }
        return r->idsum / (float)r->nh;
    }
    r->hist[r->hi] = r->hist[r->hi + r->nh] = x;
    if (++r->hi == r->nh)
        r->hi = 0;
    const float *a = r->hist + r->hi; /* the oldest first; h is even */
    float s = 0;
    for (int i = 0; i < r->nh; i++)
        s += a[i] * r->h[i];
    return s;
}

static void rx_sample(fsk4_rx_t *r, float xi, float xq, float f)
{
    const uint64_t p = r->pos++;
    r->ring[p & (RX_RING - 1)] = f;
    /* squelch */
    const float pw = xi * xi + xq * xq;
    r->blk_p += pw;
    if (r->on)
        r->pw_sum += pw, r->pw_n++;
    if (++r->blk_n == r->blk_len) {
        const bool over = r->blk_p / (float)r->blk_len >= r->sq_lin;
        const uint64_t start = p + 1 - (uint64_t)r->blk_len;
        if (over && !r->on)
            rx_keying_start(r, start);
        if (over) {
            r->below = 0, r->on_end = p + 1;
            rx_settle(r, (double)r->on_end / r->fs, false);
        } else if (r->on && ++r->below >= RX_HANG) {
            rx_keying_end(r);
        }
        r->blk_n = 0, r->blk_p = 0;
    }
    /* the sync: from the first correlation over sync_min to the last (2 symbols at most), the centroid of the top
     * 0.02: a peak's center to a fraction of a sample, or a plateau's middle (no receive filter) */
    const uint64_t span = (uint64_t)r->off[r->L - 1];
    if (r->L && p >= span) {
        const uint64_t p0 = p - span;
        float sy = 0, s1 = 0, s2 = 0;
        for (int i = 0; i < r->L; i++) {
            const float y = r->ring[(p0 + (uint64_t)r->off[i]) & (RX_RING - 1)];
            sy += y * r->sc[i], s1 += y, s2 += y * y;
        }
        const float var = s2 - s1 * s1 / (float)r->L;
        const float c = var > 1e-3f ? sy / sqrtf(var * r->snorm) : 0;
        if (c >= r->sync_min) {
            if (!r->ncw)
                r->cw_p = p0;
            r->cw[r->ncw++] = c;
        }
        if (r->ncw && (c < r->sync_min || r->ncw == 128 || r->ncw > 2 * r->sps)) {
            float mx = 0;
            for (int i = 0; i < r->ncw; i++)
                mx = fmaxf(mx, r->cw[i]);
            double w = 0, wi = 0;
            for (int i = 0; i < r->ncw; i++)
                if (r->cw[i] > mx - 0.02f)
                    w += r->cw[i] - (mx - 0.02f), wi += (r->cw[i] - (mx - 0.02f)) * i;
            const double pc = (double)r->cw_p + (w > 0 ? wi / w : 0);
            r->ncw = 0;
            /* while the carrier's up, and not from before it (a block's grace) */
            if (r->on && pc - r->delay + (double)r->blk_len >= (double)r->on_at)
                rx_sync(r, pc, mx);
        }
    }
    /* the symbols */
    while (r->locked && (double)p >= floor(r->next) + 1) {
        if (r->npend == RX_PEND)
            rx_settle(r, INFINITY, false); /* no room: past the squelch's hang anyway */
        r->pend[r->npend++] = (rx_sym_t){rx_t(r, r->next), rx_at(r, r->next)};
        r->k++, r->next += r->sps;
    }
}

static void *fsk4_rx_new(const rdof_jdoc_t *d, int req, double fs, double squelch_dbfs, char *error, size_t ecap)
{
    char shape[16] = "c4fm", filt[16] = "", sync[2 * SYNC_MAX / 4 + 1] = "", st[32] = "";
    if (!rdof_sig_text(d, req, "shape", shape, sizeof shape))
        snprintf(shape, sizeof shape, "c4fm");
    rdof_sig_text(d, req, "filter", filt, sizeof filt);
    const double baud = rdof_sig_num(d, req, "baud", 4800);
    const double sq = rdof_sig_num(d, req, "squelch_dbfs", squelch_dbfs);
    const double smin = rdof_sig_num(d, req, "sync_min", 0.9);
    const double rolloff = rdof_sig_num(d, req, "rolloff", ALPHA);
    int sh = -1, fl = -1;
    for (int i = 0; i < SH_N; i++)
        if (!strcmp(shape, shape_names[i]))
            sh = i;
    for (int i = 0; i < 3; i++)
        if (!strcmp(filt, filt_names[i]))
            fl = i;
    if (sh < 0 || (filt[0] && fl < 0) || !(rolloff >= 0.05 && rolloff <= 1)) {
        snprintf(error, ecap, "fsk4 shape: c4fm, rrc, rc, nrz or gauss; filter: none, iad or rrc; rolloff 0.05-1");
        return NULL;
    }
    if (fl < 0)
        fl = sh == SH_C4FM ? F_IAD : sh == SH_RRC ? F_RRC : F_NONE;
    if (!(baud >= 600 && baud <= 9600) || !(fs >= 8 * baud) || !(smin > 0 && smin <= 1)) {
        snprintf(error, ecap, "fsk4: baud 600-9600 (8 samples a symbol at %.0f S/s); sync_min to 1", fs);
        return NULL;
    }
    const rdof_jnode_t *sn = rdof_jnode(d, rdof_jget(d, req, "sync"));
    if (sn && (sn->type != RDOF_JSTR || strlen(sn->str) >= sizeof sync)) {
        snprintf(error, ecap, "fsk4 sync: hex, %d symbols at most", SYNC_MAX);
        return NULL;
    }
    rdof_sig_text(d, req, "sync", sync, sizeof sync);
    const size_t hl = strlen(sync);
    const double sl = rdof_sig_num(d, req, "sync_len", (double)hl * 2);
    const double sps = fs / baud;
    if (!(sl >= 0 && sl <= 2.0 * (double)hl) || (hl && sl < 8) || sl * sps >= RX_RING - 64) {
        snprintf(error, ecap, "fsk4 sync_len: 8 to the hex's symbols (and %.0f at this baud)", (RX_RING - 64) / sps);
        return NULL;
    }
    fsk4_rx_t *r = rdof_sdr_zalloc(sizeof *r);
    if (!r || !(r->q = rdof_sdr_alloc(RX_Q))) {
        if (r)
            rdof_sdr_free(r);
        snprintf(error, ecap, "no memory for a 4FSK receiver");
        return NULL;
    }
    r->fs = fs, r->baud = baud, r->sps = sps, r->filter = fl;
    r->sq_lin = (float)pow(10, sq / 10), r->sync_min = (float)smin;
    r->scale = (float)(fs / (2 * RDOF_PI));
    r->blk_len = (int)lround(fs * 0.001);
    r->L = (int)sl;
    float sum = 0;
    for (int i = 0; i < r->L; i++) {
        unsigned v = 0;
        sscanf(sync + i / 2, "%1x", &v);
        const unsigned b = (i % 2 ? v : v >> 2) & 3;
        static const float lv[4] = {1, 3, -1, -3};
        r->sc[i] = lv[b], sum += lv[b];
        r->off[i] = (int)lround(i * sps);
    }
    if (r->L) {
        if (strspn(sync, "0123456789abcdefABCDEF") != hl) {
            snprintf(error, ecap, "fsk4 sync: hex digits");
            rdof_sdr_free(r->q), rdof_sdr_free(r);
            return NULL;
        }
        r->smean = sum / (float)r->L;
        for (int i = 0; i < r->L; i++)
            r->sc[i] -= r->smean, r->snorm += r->sc[i] * r->sc[i];
        if (!(r->snorm > 0)) {
            snprintf(error, ecap, "fsk4 sync: more than one level");
            rdof_sdr_free(r->q), rdof_sdr_free(r);
            return NULL;
        }
    }
    if (fl == F_IAD) {
        r->nh = (int)lround(sps);
        r->delay = (r->nh - 1) / 2.0;
        r->hist = rdof_sdr_zalloc(sizeof(float) * (size_t)r->nh);
    } else if (fl == F_RRC) {
        const int M = (int)ceil(RX_SPAN * sps);
        r->nh = 2 * M + 1, r->delay = M;
        r->hist = rdof_sdr_zalloc(sizeof(float) * 2 * (size_t)r->nh);
        r->h = rdof_sdr_alloc(sizeof(float) * (size_t)r->nh);
        float *g = rdof_sdr_alloc(sizeof(float) * TAB);
        if (g && r->h) {
            build_pulse(SH_RRC, rolloff, 0, g);
            /* the matched filter, scaled so that the transmitter's RRC pulse comes out at exactly 1 */
            double e = 0;
            for (int i = 0; i < r->nh; i++)
                r->h[i] = pulse(g, (float)((i - M) / sps), 16), e += (double)r->h[i] * r->h[i];
            for (int i = 0; i < r->nh; i++)
                r->h[i] = (float)(r->h[i] / e);
        }
        rdof_sdr_free(g);
    }
    if ((fl != F_NONE && !r->hist) || (fl == F_RRC && !r->h)) {
        rdof_sdr_free(r->hist), rdof_sdr_free(r->h), rdof_sdr_free(r->q), rdof_sdr_free(r);
        snprintf(error, ecap, "no memory for a 4FSK receiver");
        return NULL;
    }
    rdof_sig_text(d, req, "streams", st, sizeof st);
    r->streams = (strstr(st, "disc") ? S_DISC : 0) | (strstr(st, "filt") ? S_FILT : 0);
    return r;
}

static void fsk4_rx_free(void *rx)
{
    fsk4_rx_t *r = rx;
    if (!r)
        return;
    rdof_sdr_free(r->hist), rdof_sdr_free(r->h), rdof_sdr_free(r->q);
    rdof_sdr_free(r->disc), rdof_sdr_free(r->filt);
    rdof_sdr_free(r);
}

static int fsk4_rx_streams(void *rx, rdof_sig_stream_t *out, int max)
{
    fsk4_rx_t *r = rx;
    int n = 0;
    if ((r->streams & S_DISC) && n < max)
        out[n++] = (rdof_sig_stream_t){"disc", "Hz", r->fs, r->fs / 2};
    if ((r->streams & S_FILT) && n < max)
        out[n++] = (rdof_sig_stream_t){"filt", "Hz", r->fs, r->fs / 2};
    return n;
}

static void fsk4_rx_feed(void *rx, const float *iq, int n, float *const *out, int *nout)
{
    fsk4_rx_t *r = rx;
    if (n > r->room) {
        rdof_sdr_free(r->disc);
        r->disc = rdof_sdr_alloc(sizeof(float) * (size_t)n);
        r->room = r->disc ? n : 0;
    }
    int k = 0;
    float *disc = (r->streams & S_DISC) ? out[k++] : r->disc;
    float *filt = (r->streams & S_FILT) ? out[k] : NULL;
    if (disc) {
        rdof_fm_discriminate(&r->prev, r->scale, (const rdof_cf_t *)iq, n, disc);
        for (int i = 0; i < n; i++) {
            const float f = rx_filter(r, disc[i]);
            if (filt)
                filt[i] = f;
            rx_sample(r, iq[2 * i], iq[2 * i + 1], f);
        }
    }
    k = 0;
    if (r->streams & S_DISC)
        nout[k++] = disc ? n : 0;
    if (r->streams & S_FILT)
        nout[k] = disc ? n : 0;
}

static size_t fsk4_rx_record(void *rx, char *out, size_t cap)
{
    fsk4_rx_t *r = rx;
    if (!r->qn)
        return 0;
    const char *e = memchr(r->q, '\n', r->qn);
    const size_t n = (size_t)(e - r->q);
    if (n + 1 > cap) { /* too big for the caller: dropped */
        r->dropped++;
    } else {
        memcpy(out, r->q, n);
        out[n] = 0;
    }
    memmove(r->q, e + 1, r->qn - n - 1);
    r->qn -= n + 1;
    return n + 1 > cap ? fsk4_rx_record(rx, out, cap) : n;
}

static size_t fsk4_rx_summary(void *rx, char *out, size_t cap)
{
    const fsk4_rx_t *r = rx;
    const int n = snprintf(out, cap, "{\"baud\":%.1f,\"filter\":\"%s\",\"sync_len\":%d,\"squelch_dbfs\":%.1f,"
                           "\"keyings\":%u,\"syncs\":%u,\"symbols\":%llu,\"carrier\":%s,\"dropped\":%u}",
                           r->baud, filt_names[r->filter], r->L, 10 * log10(r->sq_lin), r->keyings, r->syncs_all,
                           (unsigned long long)r->symbols_all, r->on ? "true" : "false", r->dropped);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

const rdof_sig_mode_t rdof_mode_fsk4 = {
    .name = "fsk4",
    .about = "4-level FSK as raw dibits (P25 C4FM, DMR, NXDN): c4fm, rrc, rc or nrz pulses; measured on receive",
    .tx_new = fsk4_tx_new,
    .tx_free = fsk4_tx_free,
    .tx_src = fsk4_tx_src,
    .tx_in_room = fsk4_tx_in_room,
    .tx_in = fsk4_tx_in,
    .tx_in_end = fsk4_tx_in_end,
    .tx_in_want = fsk4_tx_in_want,
    .tx_report = fsk4_tx_report,
    .tx_record = fsk4_tx_record,
    .rx_new = fsk4_rx_new,
    .rx_free = fsk4_rx_free,
    .rx_streams = fsk4_rx_streams,
    .rx_feed = fsk4_rx_feed,
    .rx_record = fsk4_rx_record,
    .rx_summary = fsk4_rx_summary,
};

#endif
