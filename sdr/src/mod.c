// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * DMR 4FSK modulator. Float throughout but for the
 * phase, a 32-bit accumulator, so it never drifts and needs no wrap.
 * Doubles (software on the S3) only in init, set_offset and queue.
 */
#include "rdof/sdr/mod.h"

#if RDOF_FEATURE_SDR

#include <math.h>
#include <string.h>

#include "sdr_private.h"

#define BLOCK 256          /* samples per inner block (bounds the stack) */
#define LUT_BITS 8
#define HALF (RDOF_MOD_TAPS / 2)

static uint32_t gcd_u(uint32_t a, uint32_t b)
{
    while (b) {
        uint32_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

/* Root-raised-cosine at t symbols from the peak, unnormalized. */
static double rrc(double t, double alpha)
{
    if (fabs(t) < 1e-9)
        return 1 - alpha + 4 * alpha / RDOF_PI;
    if (fabs(fabs(4 * alpha * t) - 1) < 1e-9)
        return alpha / sqrt(2.0) *
               ((1 + 2 / RDOF_PI) * sin(RDOF_PI / (4 * alpha)) + (1 - 2 / RDOF_PI) * cos(RDOF_PI / (4 * alpha)));
    return (sin(RDOF_PI * t * (1 - alpha)) + 4 * alpha * t * cos(RDOF_PI * t * (1 + alpha))) /
           (RDOF_PI * t * (1 - (4 * alpha * t) * (4 * alpha * t)));
}

/* Round half away from zero */
static inline int32_t round_i(float x) { return (int32_t)(x + (x >= 0.0f ? 0.5f : -0.5f)); }

/* e^{j 2 pi phase / 2^32}: the nearest of 256 table entries, then a second-order step of at most pi / 256
 * (error under 4e-7). */
static inline rdof_cf_t expj(const rdof_cf_t *lut, uint32_t phase)
{
    const uint32_t i = (phase + (1u << (31 - LUT_BITS))) >> (32 - LUT_BITS);
    const float d = (float)(int32_t)(phase - (i << (32 - LUT_BITS))) * (float)(2.0 * RDOF_PI / 4294967296.0);
    const rdof_cf_t t = lut[i & ((1u << LUT_BITS) - 1)];
    const float c = 1.0f - 0.5f * d * d;
    return (rdof_cf_t){t.re * c - t.im * d, t.im * c + t.re * d};
}

void rdof_mod_free(rdof_mod_t *m)
{
    rdof_sdr_free(m->taps);
    rdof_sdr_free(m->lut);
    rdof_sdr_free(m->q);
    m->taps = NULL;
    m->lut = NULL;
    m->q = NULL;
}

void rdof_mod_set_offset(rdof_mod_t *m, double hz)
{
    const double v = hz * 4294967296.0 / m->fs;
    m->offset_hz = hz;
    m->base_inc = (uint32_t)(int64_t)(v + (v >= 0 ? 0.5 : -0.5));
}

int rdof_mod_init(rdof_mod_t *m, uint32_t fs, double freq_offset_hz, double ramp_symbols, float amplitude, int queue,
                int64_t position)
{
    memset(m, 0, sizeof *m);
    const uint32_t g = gcd_u(fs, RDOF_MOD_SYMBOL_RATE);
    if (fs == 0 || !(ramp_symbols >= 0.0 && ramp_symbols <= RDOF_MOD_MAX_RAMP) || queue < 1)
        return -1;
    m->fs = fs;
    m->P = fs / g;
    m->Q = RDOF_MOD_SYMBOL_RATE / g;
    if (m->P > RDOF_MOD_MAX_PHASES || m->P <= m->Q)
        return -1;
    m->taps = rdof_sdr_alloc(sizeof(float) * m->P * RDOF_MOD_TAPS);
    m->lut = rdof_sdr_alloc(sizeof(rdof_cf_t) << LUT_BITS);
    m->q = rdof_sdr_zalloc(sizeof(rdof_mod_seg_t) * (size_t)queue);
    if (!m->taps || !m->lut || !m->q)
        return -1;
    /* the pulse at ph / P + j - 4 symbols, scaled so a held 1 averages 1 */
    const int P = (int)m->P;
    double sum = 0.0;
    for (int ph = 0; ph < P; ph++)
        for (int j = 0; j < RDOF_MOD_TAPS; j++) {
            const int k = ph + (j - HALF) * P;
            sum += k > HALF * P || k < -HALF * P ? 0.0 : rrc((double)k / P, 0.2);
        }
    for (int ph = 0; ph < P; ph++)
        for (int j = 0; j < RDOF_MOD_TAPS; j++) {
            const int k = ph + (j - HALF) * P;
            const double v = k > HALF * P || k < -HALF * P ? 0.0 : rrc((double)k / P, 0.2);
            m->taps[ph * RDOF_MOD_TAPS + j] = (float)(v * P / sum);
        }
    for (int i = 0; i < 1 << LUT_BITS; i++) {
        const double a = 2.0 * RDOF_PI * i / (1 << LUT_BITS);
        m->lut[i] = (rdof_cf_t){(float)cos(a), (float)sin(a)};
    }
    m->ramp = (float)ramp_symbols;
    m->inv_ramp = ramp_symbols > 0.0 ? (float)(1.0 / ramp_symbols) : 0.0f;
    m->inv_p = (float)(1.0 / P);
    m->amplitude = amplitude;
    m->dev_inc = (float)(RDOF_MOD_DEVIATION_HZ * 4294967296.0 / fs);
    m->pos = position;
    m->qcap = queue;
    rdof_mod_set_offset(m, freq_offset_hz);
    return 0;
}

int64_t rdof_mod_samples(const rdof_mod_t *m, int symbols)
{
    const int64_t n = (int64_t)symbols * m->P;
    return n % m->Q ? -1 : n / m->Q;
}

int64_t rdof_mod_slot_samples(const rdof_mod_t *m) { return rdof_mod_samples(m, RDOF_MOD_SLOT_SYMBOLS); }

int64_t rdof_mod_key_on(const rdof_mod_t *m, int64_t start, int first)
{
    return start + (int64_t)floor((first - 0.5 - m->ramp) * m->P / m->Q) + 1;
}

static rdof_mod_seg_t *seg_at(const rdof_mod_t *m, int i) { return &m->q[(m->qhead + i) % m->qcap]; }

bool rdof_mod_idle(const rdof_mod_t *m)
{
    for (int i = 0; i < m->qn; i++)
        if (seg_at(m, i)->hi > m->pos)
            return false;
    return true;
}

void rdof_mod_symbols(const uint8_t *bits, int nbits, int8_t *sym)
{
    static const int8_t map[4] = {1, 3, -1, -3}; /* dibits 00 01 10 11 */
    for (int i = 0; i + 1 < nbits; i += 2) {
        const int b0 = (bits[i >> 3] >> (7 - (i & 7))) & 1, b1 = (bits[(i + 1) >> 3] >> (7 - ((i + 1) & 7))) & 1;
        sym[i / 2] = map[b0 << 1 | b1];
    }
}

int rdof_mod_queue(rdof_mod_t *m, int64_t start, const int8_t *sym, int first, int count, unsigned flags)
{
    if (count < 1 || count > RDOF_MOD_MAX_SYMBOLS)
        return -1;
    /* drop what's done, so a full queue frees up */
    while (m->qn > 0 && seg_at(m, 0)->hi <= m->pos) {
        m->qhead = (m->qhead + 1) % m->qcap;
        m->qn--;
    }
    if (m->qn == m->qcap || (m->qn > 0 && start < seg_at(m, m->qn - 1)->start))
        return -1;
    const double reach = 0.5 + m->ramp > HALF ? 0.5 + m->ramp : HALF; /* symbols past the ends it can touch */
    const double sps = (double)m->P / m->Q;
    const int64_t lo = start + (int64_t)floor((first - reach) * sps);
    if (lo < m->pos)
        return -1; /* late */
    rdof_mod_seg_t *s = seg_at(m, m->qn);
    memset(s->sym, 0, sizeof s->sym);
    for (int i = 0; i < count; i++)
        s->sym[RDOF_MOD_PAD + i] = sym[i];
    s->start = start;
    s->lo = lo;
    s->hi = start + (int64_t)ceil((first + count - 1 + reach) * sps) + 1;
    s->first = (int16_t)first;
    s->count = (int16_t)count;
    s->flags = (uint8_t)flags;
    m->qn++;
    return 0;
}

int rdof_mod_queue_burst(rdof_mod_t *m, int64_t start, const uint8_t burst[33], const uint8_t *cach, unsigned flags)
{
    int8_t sym[12 + 132];
    int n = 0;
    if (cach) {
        rdof_mod_symbols(cach, 24, sym);
        n = 12;
    }
    rdof_mod_symbols(burst, 264, sym + n);
    return rdof_mod_queue(m, start, sym, -n, n + 132, flags);
}

static inline int64_t floor_div(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

/* The next n (<= BLOCK) samples' shaped symbol value and envelope; advances the position. */
static void render(rdof_mod_t *m, float *s, float *env, int n)
{
    const int64_t p0 = m->pos;
    memset(s, 0, sizeof(float) * (size_t)n);
    memset(env, 0, sizeof(float) * (size_t)n);
    const int P = (int)m->P, Q = (int)m->Q;
    const float R = m->ramp;
    for (int qi = 0; qi < m->qn; qi++) {
        const rdof_mod_seg_t *g = seg_at(m, qi);
        const int a = (int)((g->lo > p0 ? g->lo : p0) - p0);
        const int b = (int)((g->hi < p0 + n ? g->hi : p0 + n) - p0);
        if (a >= b)
            continue;
        const int64_t num = (p0 + a - g->start) * Q;
        int64_t k0 = floor_div(num, P);
        int ph = (int)(num - k0 * P);
        /* sym index of symbol k0 + 4 (tap 0); taps j reach back to k0 - 4 */
        const float *sp = g->sym + RDOF_MOD_PAD + (k0 + HALF - g->first);
        const float lo = g->first - 0.5f, hi = g->first + g->count - 0.5f;
        const float k0f = (float)(k0 - g->first); /* u relative to first, kept small for float */
        float kf = k0f;
        const bool up = (g->flags & RDOF_MOD_RAMP_UP) && R > 0.0f, down = (g->flags & RDOF_MOD_RAMP_DOWN) && R > 0.0f;
        const float lo_r = lo - g->first, hi_r = hi - g->first;
        for (int i = a; i < b; i++) {
            const float *t = m->taps + ph * RDOF_MOD_TAPS;
            /* two sums: half the multiply-add latency chain */
            const float a0 = t[0] * sp[0] + t[2] * sp[-2] + t[4] * sp[-4] + t[6] * sp[-6] + t[8] * sp[-8];
            const float a1 = t[1] * sp[-1] + t[3] * sp[-3] + t[5] * sp[-5] + t[7] * sp[-7];
            s[i] += a0 + a1;
            const float u = kf + (float)ph * m->inv_p;
            float e;
            if (u >= lo_r && u < hi_r) {
                e = 1.0f;
            } else if (up && u < lo_r && u >= lo_r - R) {
                const float x = (u - (lo_r - R)) * m->inv_ramp; /* 0..1: cos(pi x) */
                e = 0.5f - 0.5f * expj(m->lut, (uint32_t)(x * 2147483648.0f)).re;
            } else if (down && u >= hi_r && u < hi_r + R) {
                const float x = (u - hi_r) * m->inv_ramp;
                e = 0.5f + 0.5f * expj(m->lut, (uint32_t)(x * 2147483648.0f)).re;
            } else {
                e = 0.0f;
            }
            if (e > env[i])
                env[i] = e;
            ph += Q;
            if (ph >= P) {
                ph -= P;
                kf += 1.0f;
                sp++;
            }
        }
    }
    m->pos += n;
    while (m->qn > 0 && seg_at(m, 0)->hi <= m->pos) {
        m->qhead = (m->qhead + 1) % m->qcap;
        m->qn--;
    }
}

void rdof_mod_iq(rdof_mod_t *m, rdof_cf_t *out, int n)
{
    float s[BLOCK], env[BLOCK];
    for (int done = 0; done < n;) {
        const int k = n - done < BLOCK ? n - done : BLOCK;
        render(m, s, env, k);
        uint32_t ph = m->phase;
        const uint32_t base = m->base_inc;
        const float dev = m->dev_inc, amp = m->amplitude;
        rdof_cf_t *o = out + done;
        for (int i = 0; i < k; i++) {
            ph += base + (uint32_t)round_i(s[i] * dev);
            const float a = amp * env[i];
            if (a == 0.0f) {
                o[i] = (rdof_cf_t){0.0f, 0.0f};
            } else {
                const rdof_cf_t r = expj(m->lut, ph);
                o[i] = (rdof_cf_t){a * r.re, a * r.im};
            }
        }
        m->phase = ph;
        done += k;
    }
}

void rdof_mod_freq(rdof_mod_t *m, float *hz, float *env_out, int n)
{
    float s[BLOCK], env[BLOCK];
    const float off = (float)m->offset_hz, dev = (float)RDOF_MOD_DEVIATION_HZ;
    for (int done = 0; done < n;) {
        const int k = n - done < BLOCK ? n - done : BLOCK;
        render(m, s, env, k);
        uint32_t ph = m->phase;
        for (int i = 0; i < k; i++) {
            ph += m->base_inc + (uint32_t)round_i(s[i] * m->dev_inc);
            hz[done + i] = s[i] * dev + off;
        }
        m->phase = ph;
        if (env_out)
            memcpy(env_out + done, env, sizeof(float) * (size_t)k);
        done += k;
    }
}

#endif
