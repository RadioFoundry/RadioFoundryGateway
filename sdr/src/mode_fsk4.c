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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdr_private.h"

#define PH 64                    /* table points a symbol */
#define SPAN 16                  /* symbols either side, at most (the table) */
#define TAB (PH * SPAN + 1)      /* g(0) .. g(SPAN), g even */
_Static_assert(PH == 64, "the render takes the phase row from the fraction's top 6 bits");
#define ALPHA 0.2
#define RAMP_S 0.002             /* the envelope's raised-cosine ramp, key-up and key-down */
#define MAX_BYTES (256u * 1024u) /* a million symbols: 218 s at 4800 */
#define WAIT_S 2.0               /* for the symbols to come in before the first key-up */

enum { SH_C4FM, SH_RRC, SH_RC, SH_NRZ };
static const char *const shape_names[] = {"c4fm", "rrc", "rc", "nrz"};

/* The raised cosine's spectrum (T = 1, area 1) at f >= 0 */
static double rc_spec(double f)
{
    const double f1 = (1 - ALPHA) / 2, f2 = (1 + ALPHA) / 2;
    if (f <= f1)
        return 1;
    if (f >= f2)
        return 0;
    return 0.5 * (1 + cos(RDOF_PI / ALPHA * (f - f1)));
}

/* The pulse's spectrum at f >= 0 */
static double spec(int shape, double f)
{
    const double h = rc_spec(f);
    if (shape == SH_RRC)
        return sqrt(h);
    if (shape == SH_C4FM && f > 0)
        return h * (RDOF_PI * f) / sin(RDOF_PI * f);
    return h;
}

/* g at t = i / PH, i = 0 .. TAB-1, scaled to 1 behind the shape's receive filter */
static void build_pulse(int shape, float *g)
{
    if (shape == SH_NRZ) {
        for (int i = 0; i < TAB; i++)
            g[i] = i < PH / 2 ? 1.0f : i == PH / 2 ? 0.5f : 0.0f;
        return;
    }
    /* g(t) = 2 * integral over 0 .. (1 + ALPHA) / 2 of spec(f) cos(2 pi f t) df, by Simpson's rule */
    enum { NF = 240 };
    const double fmax = (1 + ALPHA) / 2, df = fmax / NF;
    float w[NF + 1];
    for (int k = 0; k <= NF; k++)
        w[k] = (float)(spec(shape, k * df) * (k == 0 || k == NF ? 1 : k % 2 ? 4 : 2) * df / 3 * 2);
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

static int span_of(int shape) { return shape == SH_RRC ? 16 : shape == SH_NRZ ? 1 : 12; }

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
} fsk4_tx_t;

static int dibit(const fsk4_tx_t *t, uint32_t k)
{
    const unsigned b = t->sym[k >> 2] >> (6 - 2 * (k & 3)) & 3;
    static const int lv[4] = {1, 3, -1, -3}; /* 00 01 10 11 */
    return lv[b];
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
    t->nsym = nsym > 0 && nsym <= 4.0 * t->nbytes ? (uint32_t)nsym : 4 * t->nbytes;
    return NULL;
}

static const char *fsk4_cfg(fsk4_tx_t *t, const rdof_jdoc_t *d, int job, char *why, size_t cap)
{
    char shape[16] = "c4fm";
    rdof_sig_text(d, job, "shape", shape, sizeof shape);
    t->shape = -1;
    for (int i = 0; i < 4; i++)
        if (!strcmp(shape, shape_names[i]))
            t->shape = i;
    if (t->shape < 0)
        return "fsk4 shape: c4fm, rrc, rc or nrz";
    t->baud = rdof_sig_num(d, job, "baud", 4800);
    t->dev_hz = rdof_sig_num(d, job, "dev_hz", t->shape == SH_RRC ? 1944 : 1800);
    t->scale = rdof_sig_num(d, job, "dev_scale_pct", 100) / 100;
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
    if (!(t->dev_hz > 0 && t->dev_hz <= 5000) || !(t->scale > 0 && t->scale <= 2))
        return "fsk4 dev_hz: up to 5000; dev_scale_pct 1-200";
    if (!(fabs(t->offset_hz) <= 5000))
        return "fsk4 offset_hz: -5000 to 5000";
    if (!(t->lead_s >= 0 && t->lead_s <= 10) || !(t->tail_s >= 0 && t->tail_s <= 10) ||
        !(t->gap_s >= 0 && t->gap_s <= 60))
        return "fsk4 lead_s, tail_s: 0-10; gap_s 0-60";
    if (!(rep >= 1 && rep <= 10000))
        return "fsk4 repeat: 1 to 10000";
    t->repeat = (unsigned)rep;
    /* the symbols: hex in the request, or a stream of `symbols` after it */
    const double nsym = rdof_sig_num(d, job, "symbols", 0);
    const rdof_jnode_t *hn = rdof_jnode(d, rdof_jget(d, job, "hex"));
    if (hn && hn->type == RDOF_JSTR && hn->str[0])
        return hex_in(t, hn->str, nsym);
    if (!(nsym >= 1 && nsym <= 4.0 * MAX_BYTES)) {
        snprintf(why, cap, "fsk4: hex, or symbols (1 to %u) and the dibits after the request", 4 * MAX_BYTES);
        return why;
    }
    t->nsym = (uint32_t)nsym;
    t->nbytes = (t->nsym + 3) / 4;
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
    if (t->have == t->nbytes && t->nsym <= 8192) {
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
            worst = fmax(worst, s * 3 * unit + fabs(t->offset_hz));
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
        rdof_sdr_free(t->sym);
        rdof_sdr_free(t);
        return NULL;
    }
    if (!(fs >= 8 * t->baud)) {
        snprintf(error, ecap, "fsk4: the transmitter's %.0f S/s is under 8 samples a symbol", fs);
        rdof_sdr_free(t->sym);
        rdof_sdr_free(t);
        return NULL;
    }
    t->fs = fs;
    t->sps = fs / t->baud;
    t->span = span_of(t->shape);
    build_pulse(t->shape, t->g);
    const int nt = 2 * t->span;
    t->rows = rdof_sdr_zalloc(sizeof(float) * (size_t)(PH + 1) * (size_t)nt);
    if (!t->rows) {
        snprintf(error, ecap, "no memory for a 4FSK transmitter");
        rdof_sdr_free(t->sym);
        rdof_sdr_free(t);
        return NULL;
    }
    for (int p = 0; p <= PH; p++)
        for (int m = 0; m < nt; m++)
            t->rows[p * nt + m] = pulse(t->g, (float)p / PH - (float)(m - t->span + 1), t->span);
    t->step = (uint64_t)llround(t->baud / fs * 4294967296.0);
    t->peak = peak_hz(t);
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
    rdof_sdr_free(t);
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
            if ((j >> 2) < t->have) {
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
    return (rdof_sig_src_t){.freq = fsk4_freq, .done = fsk4_done, .peak_hz = t->peak, .ctx = t};
}

static uint64_t fsk4_tx_in_want(void *tx)
{
    const fsk4_tx_t *t = tx;
    return t->nbytes - t->have;
}

static size_t fsk4_tx_in_room(void *tx) { return (size_t)fsk4_tx_in_want(tx); }

static size_t fsk4_tx_in(void *tx, const uint8_t *b, size_t n)
{
    fsk4_tx_t *t = tx;
    const size_t k = n < t->nbytes - t->have ? n : t->nbytes - t->have;
    memcpy(t->sym + t->have, b, k);
    t->have += (uint32_t)k;
    return k;
}

static void fsk4_tx_in_end(void *tx, bool gone)
{
    fsk4_tx_t *t = tx;
    (void)gone;
    t->wait_max = 0; /* nothing more comes: what's missing goes as 0 */
}

static size_t fsk4_tx_report(void *tx, char *out, size_t cap)
{
    const fsk4_tx_t *t = tx;
    const uint32_t in = t->have * 4 < t->nsym ? t->have * 4 : t->nsym;
    const int n = snprintf(out, cap,
                           ",\"shape\":\"%s\",\"baud\":%.1f,\"dev_hz\":%.1f,\"peak_hz\":%.0f,\"offset_hz\":%.1f,"
                           "\"symbols\":%u,\"repeat\":%u,\"bursts\":%u,\"seconds\":%.3f,\"symbols_in\":%u,"
                           "\"symbols_short\":%llu,\"wait_s\":%.3f,\"clip_hz\":%.0f,\"clipped\":%llu",
                           shape_names[t->shape], t->baud, t->dev_hz * t->scale, t->peak, t->offset_hz,
                           (unsigned)t->nsym, t->repeat, t->bursts, (double)t->total / t->fs, (unsigned)in,
                           (unsigned long long)(t->nsym - in) * t->repeat, (double)t->waited / t->fs, t->clip_hz,
                           (unsigned long long)t->clipped);
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
};

#endif
