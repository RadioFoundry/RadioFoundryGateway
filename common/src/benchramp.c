// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * A burst's PA ramp (rdof/benchramp.h). Indices below are outputs from the start of the window copied for this burst.
 */
#include "rdof/benchramp.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/benchline.h" /* RDOF_BL_MALLOC, rdof_bl_atan2f */

#define SYMS 132
#define SYNC_CENTER 65.5     /* in symbols: the sync's center, between symbols 65 and 66 */
#define FIT_FROM 24          /* the symbols that place a burst and set its frequency: clear of both ramps */
#define FIT_TO 108
#define LEVELS 40            /* symbols whose level is taken */
#define BEFORE_MS 37.0       /* from the sync's center: the middle of the slot before, and its 2.5 ms */
#define AFTER_MS 24.0        /* the end of the falling edge's search */
#define OFF_DB 0.01f         /* -20 dB: under this the burst's carrier is off */

static const float RISE_DB[RDOF_BRAMP_RISES] = {-40, -30, -20, -10, -6, -3, -1};
static const float FALL_DB[RDOF_BRAMP_FALLS] = {-3, -10, -30};

struct rdof_bramp {
    double rate, ms, sps;
    int d, n_ring, cap, w, hs, nsym, search;
    float *iq;
    uint8_t *clip;
    float *p, *env, *f, *med;
    long n;
    uint64_t sc0, last_sc;
    bool have_last;
};

static float db(float v) { return 10 * log10f((v > 0 ? v : 0) + 1e-14f); }

void rdof_bramp_free(rdof_bramp_t *r)
{
    if (!r)
        return;
    free(r->iq);
    free(r->clip);
    free(r->p);
    free(r->env);
    free(r->f);
    free(r->med);
    free(r);
}

rdof_bramp_t *rdof_bramp_new(double rate, int d)
{
    if (!(rate > 1000) || d < 1)
        return NULL;
    rdof_bramp_t *r = calloc(1, sizeof *r);
    if (!r)
        return NULL;
    r->rate = rate, r->d = d, r->ms = rate / 1000, r->sps = rate / 4800;
    r->w = (int)lround(rate * 1e-4) | 1; /* 0.1 ms, odd so it's centered */
    r->hs = (int)lround(0.3 * r->sps);
    if (r->hs < 1)
        r->hs = 1;
    r->nsym = (int)lround(r->sps / 2) | 1;
    r->search = (int)lround(2 * r->sps);
    r->cap = (int)ceil((BEFORE_MS + AFTER_MS + 1) * r->ms) + 2 * r->search + 2 * r->w;
    /* what's kept, past the window: how far a burst's record can come after its samples (0.34 s without room for 0.68) */
    int n = 1024;
    while (n < 16 * r->cap && n < (1 << 15))
        n <<= 1;
    for (; n >= 2 * r->cap; n >>= 1) {
        r->iq = RDOF_BL_MALLOC(sizeof(float) * 2 * (size_t)n);
        r->clip = RDOF_BL_MALLOC((size_t)n);
        if (r->iq && r->clip)
            break;
        free(r->iq);
        free(r->clip);
        r->iq = NULL, r->clip = NULL;
    }
    r->n_ring = n;
    r->p = RDOF_BL_MALLOC(sizeof(float) * (size_t)r->cap);
    r->env = RDOF_BL_MALLOC(sizeof(float) * (size_t)r->cap);
    r->f = RDOF_BL_MALLOC(sizeof(float) * (size_t)r->cap);
    r->med = RDOF_BL_MALLOC(sizeof(float) * (size_t)r->cap);
    if (!r->iq || !r->clip || !r->p || !r->env || !r->f || !r->med) {
        rdof_bramp_free(r);
        return NULL;
    }
    return r;
}

void rdof_bramp_push(rdof_bramp_t *r, uint64_t sc, float i, float q, int clipped)
{
    if (!r)
        return;
    if (r->have_last && sc != r->last_sc + (uint64_t)r->d)
        r->n = 0;
    if (!r->n)
        r->sc0 = sc;
    r->last_sc = sc, r->have_last = true;
    const size_t k = (size_t)(r->n & (r->n_ring - 1));
    r->iq[2 * k] = i, r->iq[2 * k + 1] = q;
    r->clip[k] = (uint8_t)(clipped > 255 ? 255 : clipped);
    r->n++;
}

/* The last upward crossing of level before the 0.1 ms envelope stays at or over it for hold outputs: a dip in the plateau
 * later isn't the rising edge, and the end of a sag at the start is. NAN if it never holds. */
static float rise_cross(const float *p, const float *env, long lo, long hi, float level, int hold)
{
    long at = -1;
    int run = 0;
    for (long j = lo; j < hi; j++) {
        run = env[j] >= level ? run + 1 : 0;
        if (run >= hold) {
            at = j - hold + 1;
            break;
        }
    }
    if (at < 0)
        return NAN;
    long top = at + hold / 8 + 2;
    if (top > hi - 1)
        top = hi - 1;
    for (long j = top - 1; j >= lo; j--)
        if (p[j] < level && p[j + 1] >= level) {
            const float a = db(p[j]), b = db(p[j + 1]);
            return b != a ? (float)j + (db(level) - a) / (b - a) : (float)j;
        }
    return NAN;
}

/* The first downward crossing of level, from where the 0.1 ms envelope goes under it */
static float fall_cross(const float *p, const float *env, long lo, long hi, float level, int w)
{
    long j0 = -1;
    for (long j = lo; j < hi; j++)
        if (env[j] < level) {
            j0 = j;
            break;
        }
    if (j0 < 0)
        return NAN;
    const long from = j0 - w - 1 > lo ? j0 - w - 1 : lo;
    for (long j = from; j < hi - 1; j++)
        if (p[j] >= level && p[j + 1] < level) {
            const float a = db(p[j]), b = db(p[j + 1]);
            return b != a ? (float)j + (db(level) - a) / (b - a) : (float)j;
        }
    return (float)j0;
}

/* The first output from a where the envelope stays within [lo, hi] for hold outputs; -1 none */
static long settle_at(const float *env, long a, long m1, float lo, float hi, int hold)
{
    int run = 0;
    for (long j = a; j < m1; j++) {
        run = env[j] >= lo && env[j] <= hi ? run + 1 : 0;
        if (run >= hold)
            return j - hold + 1;
    }
    return -1;
}

static void bins(const float *p, long len, double at, double width, float quiet_db, float *out)
{
    for (int j = 0; j < RDOF_BRAMP_BINS; j++) {
        const long lo = lround(at + j * width), hi = lround(at + (j + 1) * width);
        if (lo < 0 || hi > len || hi <= lo || !isfinite(quiet_db)) {
            out[j] = NAN;
            continue;
        }
        float s = 0;
        for (long k = lo; k < hi; k++)
            s += p[k];
        out[j] = db(s / (float)(hi - lo)) - quiet_db;
    }
}

int rdof_bramp_measure(rdof_bramp_t *r, double sync_sc, const uint8_t raw[33], double quiet_db, bool last,
                       rdof_bramp_res_t *o)
{
    memset(o, 0, sizeof *o);
    o->p0_ms = o->align_ms = o->settled_db = o->ripple_db = o->rise_30_3_ms = NAN;
    o->overshoot_db = o->overshoot_t_ms = o->settle_1db_ms = o->settle_0p5db_ms = NAN;
    o->sym1_db = o->err5_mean_db = o->err5_abs_db = o->err12_mean_db = o->err12_abs_db = NAN;
    o->fall_3_30_ms = o->f_first1ms_hz = o->f_next2ms_hz = o->dev3_hz = NAN;
    for (int k = 0; k < RDOF_BRAMP_RISES; k++)
        o->rise_ms[k] = NAN;
    for (int k = 0; k < RDOF_BRAMP_FALLS; k++)
        o->fall_ms[k] = NAN;
    for (int k = 0; k < RDOF_BRAMP_BINS; k++)
        o->pre_db[k] = o->empty_db[k] = NAN;
    if (!r || !r->n)
        return last ? RDOF_BRAMP_LOST : RDOF_BRAMP_WAIT;
    const double ic = (sync_sc - (double)r->sc0) / r->d, ms = r->ms, sps = r->sps;
    const long newest = r->n - 1, oldest = r->n > r->n_ring ? r->n - r->n_ring + 1 : 0;
    const long end_payload = (long)ceil(ic + 15 * ms) + r->search;
    long hi_i = (long)ceil(ic + AFTER_MS * ms) + r->w + 2;
    if (ic < 0)
        return RDOF_BRAMP_LOST;
    if (hi_i > newest) {
        if (!last)
            return RDOF_BRAMP_WAIT;
        if (end_payload > newest)
            return RDOF_BRAMP_LOST;
        hi_i = newest;
    }
    long base = (long)floor(ic - BEFORE_MS * ms) - r->search;
    if (base < oldest)
        base = oldest;
    if (ic - 15 * ms - r->search < (double)base)
        return RDOF_BRAMP_LOST;
    long len = hi_i - base + 1;
    if (len > r->cap)
        len = r->cap;
    const int mask = r->n_ring - 1;
    float *p = r->p, *env = r->env, *f = r->f;
    for (long j = 0; j < len; j++) {
        const size_t k = (size_t)((base + j) & mask);
        p[j] = r->iq[2 * k] * r->iq[2 * k] + r->iq[2 * k + 1] * r->iq[2 * k + 1];
    }
    /* the 0.1 ms envelope, centered; a sum of what's in the window, fresh at each output: floats drift over a burst */
    const int h = r->w / 2;
    for (long j = 0; j < len; j++) {
        const long a = j - h > 0 ? j - h : 0, b = j + h < len - 1 ? j + h : len - 1;
        float s = 0;
        for (long k = a; k <= b; k++)
            s += p[k];
        env[j] = s / (float)(b - a + 1);
    }
    /* the discriminator over the symbols: the phase across 2 hs outputs, centered */
    const double ic0 = ic - (double)base;
    const float to_hz = (float)(r->rate / (2 * M_PI * 2 * r->hs));
    const long f_lo = lround(ic0 - (SYNC_CENTER + 2) * sps) - r->search, f_hi = lround(ic0 + (SYNC_CENTER + 2) * sps) + r->search;
    for (long j = 0; j < len; j++)
        f[j] = NAN;
    for (long j = f_lo > r->hs ? f_lo : r->hs; j <= f_hi && j < len - r->hs; j++) {
        const size_t a = (size_t)((base + j + r->hs) & mask), b = (size_t)((base + j - r->hs) & mask);
        const float xi = r->iq[2 * a], xq = r->iq[2 * a + 1], yi = r->iq[2 * b], yq = r->iq[2 * b + 1];
        f[j] = rdof_bl_atan2f(xq * yi - xi * yq, xi * yi + xq * yq) * to_hz;
    }
    /* the symbols, as decoded */
    float sy[SYMS];
    float mean = 0;
    for (int k = 0; k < SYMS; k++) {
        const int dib = (raw[k / 4] >> (6 - 2 * (k % 4))) & 3;
        sy[k] = dib == 1 ? 3.0f : dib == 0 ? 1.0f : dib == 2 ? -1.0f : -3.0f;
    }
    for (int k = FIT_FROM; k < FIT_TO; k++)
        mean += sy[k];
    mean /= FIT_TO - FIT_FROM;
    /* where they are: the best of whole outputs, a parabola between */
    float best = -INFINITY, sc_m = 0, sc_p = 0;
    int off = 0;
    float prev = 0;
    bool have_prev = false, want_next = false;
    for (int s = -r->search; s <= r->search; s++) {
        float v = 0;
        bool ok = true;
        for (int k = FIT_FROM; k < FIT_TO; k++) {
            const long j = lround(ic0 + s + (k - SYNC_CENTER) * sps);
            if (j < 0 || j >= len || !isfinite(f[j])) {
                ok = false;
                break;
            }
            v += f[j] * (sy[k] - mean);
        }
        if (!ok) {
            have_prev = want_next = false;
            continue;
        }
        if (want_next)
            sc_p = v, want_next = false;
        if (v > best) {
            best = v, off = s, sc_m = have_prev ? prev : v, sc_p = v, want_next = true;
        }
        prev = v, have_prev = true;
    }
    if (!isfinite(best) || best <= 0)
        return RDOF_BRAMP_LOST;
    const float den = sc_m - 2 * best + sc_p;
    const double frac = den < 0 ? 0.5 * (double)(sc_m - sc_p) / (double)den : 0;
    const double tau = ic0 + off + frac;
    o->align_ms = (float)((off + frac) / ms);
#define TN(k) (tau + ((k) - SYNC_CENTER) * sps)
#define AT(k) lround(TN(k))
    const double p0 = TN(0) - sps / 2, p1 = TN(SYMS - 1) + sps / 2;
    o->p0_ms = (float)((p0 - ic0) / ms);
    /* the frequency's fit over the burst's middle: f0 and the deviation */
    double n = 0, sx = 0, sxx = 0, sy2 = 0, sxy = 0;
    for (int k = FIT_FROM; k < FIT_TO; k++) {
        const long j = AT(k);
        if (j >= 0 && j < len && isfinite(f[j]))
            n++, sx += sy[k], sxx += sy[k] * sy[k], sy2 += f[j], sxy += sy[k] * f[j];
    }
    const double dd = n * sxx - sx * sx;
    const double dev = dd > 0 ? (n * sxy - sx * sy2) / dd : 0, f0 = n > 0 ? (sy2 - dev * sx) / n : 0;
    o->dev3_hz = (float)(3 * dev);
    /* the settled level */
    const long m0 = lround(ceil(p0 + 4 * ms)), m1 = lround(floor(p1 - 4 * ms));
    int nm = 0;
    float *v = r->med;
    for (long j = m0; j < m1 && j < len; j += r->w) {
        const float x = env[j];
        int k = nm++;
        while (k > 0 && v[k - 1] > x) /* insertion: about two hundred values */
            v[k] = v[k - 1], k--;
        v[k] = x;
    }
    if (nm < 8)
        return RDOF_BRAMP_LOST;
    const float settled = nm % 2 ? v[nm / 2] : 0.5f * (v[nm / 2 - 1] + v[nm / 2]);
    if (!(settled > 0))
        return RDOF_BRAMP_LOST;
    const float settled_db = db(settled);
    o->settled_db = settled_db;
    {
        float s = 0, s2 = 0;
        for (int k = 0; k < nm; k++) {
            const float x = db(v[k]) - settled_db;
            s += x, s2 += x * x;
        }
        const float mu = s / (float)nm;
        o->ripple_db = sqrtf(fabsf(s2 / (float)nm - mu * mu));
    }
    const float off_lin = settled * OFF_DB;
    const float qdb = (float)quiet_db;
    const bool have_q = isfinite(quiet_db);
#define USABLE(l) (!have_q || settled_db + (l) >= qdb + 6.0f)
    /* the rising edge: only where this burst's carrier comes up, past the gap before it */
    const long s0 = lround(p0 - 14 * ms) > 0 ? lround(p0 - 14 * ms) : 0;
    const long s2 = lround(p0 + 10 * ms) < len ? lround(p0 + 10 * ms) : len;
    long gi = -1;
    for (long j = s2 - 1; j >= s0; j--)
        if (env[j] < off_lin) {
            gi = j;
            break;
        }
    long lo = -1;
    if (gi >= 0) {
        lo = gi;
        while (lo > s0 && env[lo - 1] < off_lin)
            lo--;
        const int hold = (int)lround(ms);
        float up[RDOF_BRAMP_RISES];
        for (int k = 0; k < RDOF_BRAMP_RISES; k++) {
            up[k] = USABLE(RISE_DB[k]) ? rise_cross(p, env, lo, s2, settled * powf(10, RISE_DB[k] / 10), hold) : NAN;
            o->rise_ms[k] = isfinite(up[k]) ? (float)((up[k] - p0) / ms) : NAN;
        }
        if (isfinite(up[1]) && isfinite(up[5]))
            o->rise_30_3_ms = (float)((up[5] - up[1]) / ms);
        /* overshoot and settling, from the -3 dB point */
        const float start = isfinite(up[5]) ? up[5] : isfinite(up[3]) ? up[3] : (float)(gi + 1);
        const long a = (long)start;
        long b4 = a + lround(4 * ms);
        if (b4 > len)
            b4 = len;
        long pk = a;
        for (long j = a; j < b4; j++)
            if (env[j] > env[pk])
                pk = j;
        if (b4 > a) {
            o->overshoot_db = db(env[pk]) - settled_db;
            o->overshoot_t_ms = (float)((pk - p0) / ms);
        }
        static const struct { float band, need; } BAND[2] = {{1.0f, 0.4f}, {0.5f, 0.2f}};
        for (int b = 0; b < 2; b++) {
            if (o->ripple_db > BAND[b].need) /* the envelope's own ripple is as big as the band: no claim */
                continue;
            const float g = powf(10, BAND[b].band / 10);
            const long at = settle_at(env, a, m1 < len ? m1 : len, settled / g, settled * g, (int)lround(2 * ms));
            if (at >= 0)
                *(b ? &o->settle_0p5db_ms : &o->settle_1db_ms) = (float)((at - p0) / ms);
        }
    }
    /* the first symbols, sag as well as overshoot */
    float lev[LEVELS];
    const int hsym = r->nsym / 2;
    for (int k = 0; k < LEVELS; k++) {
        const long j = AT(k);
        float s = 0;
        int c = 0;
        for (long i = j - hsym; i <= j + hsym; i++)
            if (i >= 0 && i < len)
                s += p[i], c++;
        lev[k] = (c ? db(s / (float)c) : -140.0f) - settled_db;
        if (have_q && lev[k] < qdb - settled_db)
            lev[k] = qdb - settled_db;
    }
    o->sym1_db = lev[0];
    for (int m = 0; m < 2; m++) {
        const int cnt = m ? 12 : 5;
        float s = 0, a = 0;
        for (int k = 0; k < cnt; k++)
            s += lev[k], a += fabsf(lev[k]);
        *(m ? &o->err12_mean_db : &o->err5_mean_db) = s / (float)cnt;
        *(m ? &o->err12_abs_db : &o->err5_abs_db) = a / (float)cnt;
    }
    /* the falling edge: only where this burst's carrier goes */
    const long f0i = lround(p1 - 4 * ms) > 0 ? lround(p1 - 4 * ms) : 0;
    const long f1i = lround(p1 + 8 * ms) < len ? lround(p1 + 8 * ms) : len;
    bool has_fall = false;
    for (long j = f0i; j < f1i; j++)
        if (env[j] < off_lin) {
            has_fall = true;
            break;
        }
    if (has_fall) {
        float dn[RDOF_BRAMP_FALLS];
        for (int k = 0; k < RDOF_BRAMP_FALLS; k++) {
            dn[k] = USABLE(FALL_DB[k]) ? fall_cross(p, env, f0i, f1i, settled * powf(10, FALL_DB[k] / 10), r->w) : NAN;
            o->fall_ms[k] = isfinite(dn[k]) ? (float)((dn[k] - p1) / ms) : NAN;
        }
        if (isfinite(dn[0]) && isfinite(dn[2]))
            o->fall_3_30_ms = (float)((dn[2] - dn[0]) / ms);
    }
    /* the energy before it, and in the middle of the empty slot before */
    const double hb = 0.5 * ms;
    bins(p, len, p0 - 10 * hb, hb, qdb, o->pre_db);
    for (int j = 0; j < RDOF_BRAMP_BINS; j++)
        if (gi < 0 || lround(p0 - (10 - j) * hb) < lo)
            o->pre_db[j] = NAN;
    const double mid = TN(66) - 30 * ms;
    bool busy = lround(mid - 5 * hb) < 0;
    for (long j = lround(mid - 5 * hb) > 0 ? lround(mid - 5 * hb) : 0; j < lround(mid + 5 * hb) && j < len; j++)
        if (env[j] >= off_lin) {
            busy = true;
            break;
        }
    if (!busy)
        bins(p, len, mid - 5 * hb, hb, qdb, o->empty_db);
    /* the frequency over the rise, once there's carrier */
    int k0 = -1;
    for (int k = 0; k < 24 && k0 < 0; k++)
        if (lev[k] > -30)
            k0 = k;
    if (k0 >= 0) {
        for (int m = 0; m < 2; m++) {
            const int from = m ? k0 + 5 : k0, cnt = m ? 10 : 5;
            float s = 0, low = 0;
            int c = 0;
            for (int k = from; k < from + cnt; k++) {
                const long j = AT(k);
                if (j < 0 || j >= len || !isfinite(f[j]))
                    continue;
                s += f[j] - sy[k] * (float)dev - (float)f0, c++;
                if (lev[k] < low)
                    low = lev[k];
            }
            if (c) {
                *(m ? &o->f_next2ms_hz : &o->f_first1ms_hz) = s / (float)c;
                *(m ? &o->f_next2ms_unreliable : &o->f_first1ms_unreliable) = low < -20;
            }
        }
    }
    for (long j = lround(p0 - ms); j < lround(p1 + ms) && j < len; j++)
        if (j >= 0)
            o->clipped += r->clip[(size_t)((base + j) & mask)];
    return RDOF_BRAMP_OK;
}
