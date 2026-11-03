// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* An analog FM receiver that measures as it goes (rdof/sdr/fmrx.h). */
#include "rdof/sdr/fmrx.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/sdr/features.h"
#if RDOF_FEATURE_SDR

#include "rdof/sdr/filters.h"
#include "rdof/sdr/fm.h"
#include "rdof/sdr/fmgen.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define N RDOF_FMRX_FFT
#define RING 2048                /* the subaudio's last quarter second (about 4 kS/s) and room */
#define BLOCKS 64                /* finished blocks waiting to be taken */
#define DCS_ENTRIES (2 * 104)

typedef struct {
    double psum, gfsum;
    float emin, emax, dpos, dneg;
    double a2sum, s2sum;
    long n, gn, na, ns;
} acc_t;

#define DELAY_S 0.01             /* the gate looks this far ahead and back */
#define LEARN_S 0.03             /* after the gate opens: the carrier's offset learned, nothing measured */
#define SETTLE_S 0.03            /* then the filters settle: still nothing measured */

struct rdof_fmrx {
    rdof_fmrx_opts_t o;
    void (*dealloc)(void *);
    double fs, ar, sr;
    int da, ds, dk, sk;
    rdof_cf_t prev;              /* the discriminator's last sample */
    float scale;
    /* the input rate: the gate (the power, 2 ms smoothed, over the squelch for DELAY_S each side of a sample) and
     * the samples it delays */
    float ps, ps_a, sq_lin;
    float pf, pf_a;              /* the power 0.25 ms smoothed: the 2 ms one takes 14 ms to fall 30 dB, past DELAY_S */
    long since_on, since_off;    /* samples the power has been over the squelch, and under it */
    int D, di;
    float dly[2 * 2000];         /* DELAY_S at 200 kS/s */
    bool gate;
    rdof_bq_t lp5[2];
    double pos;                  /* input samples out of the delay */
    long bsize;
    acc_t b;
    /* the audio rate */
    long gated;                  /* audio samples since the gate opened */
    long learn, settle;
    double learn_sum;
    float cfo, cfo_a;
    rdof_bq_t hp[3], lp3[2], notch;
    bool notching;
    float de, de_a, de_g;
    float sub_acc;
    rdof_bq_t lps[3];
    /* the spectrum */
    rdof_psd_t psd;
    /* the subaudio's last quarter second */
    float ring[RING];
    int rlen, rn, rw, rsince;
    double sub_n;                /* subaudio samples, all */
    /* CTCSS */
    long ct_hits[64];
    double ct_fsum, ct_devsum;
    long ct_n;
    double ct_ph, ct_w;          /* the reverse burst's correlator, radians and its step */
    double zr, zi, zr1, zi1, zr2, zi2, zmag;
    int zn, zlen, zk;
    double reverse_t;
    /* DCS */
    float dc_mean, dc_ph, dc_step;
    float dc_x[3];               /* the last three bit centers' values (relative to the tracked carrier) */
    int dc_b[3];
    double dc_sp, dc_sn;         /* sums at the middle of three equal bits, + and - */
    long dc_np, dc_nn;
    float dc_amp, dc_bias;       /* the NRZ's level each way, and its mean (a code's ones and zeros differ: the
                                  * tracked carrier sits that far off the real one) */
    int dc_sign;
    uint32_t reg;
    long dc_bits, dc_run, dc_words;
    uint32_t dc_canon, dc_found;
    int dc_code;
    bool dc_inv, dc_turnoff;
    /* the summary's sums (over gated samples) */
    double carrier_s, p_sum, noise_sum, env_sum, env2_sum, a2_sum, d2_sum, s2_sum, cfo_sum;
    long p_n, noise_n, env_n, a_n, d_n, s_n, cfo_n, blocks_on;
    float p_min, p_max, cfo_min, cfo_max, dpos, dneg, apos, aneg;
    rdof_fmrx_block_t q[BLOCKS];
    int qh, qn;
};

/* ── DCS ── */

typedef struct {
    uint32_t canon;
    uint16_t code;
    bool inv;
} dcs_entry_t;
static dcs_entry_t dcs_tab[DCS_ENTRIES];
static int dcs_n;

static uint32_t rot23(uint32_t w, int k) { return ((w >> k) | (w << (23 - k))) & 0x7FFFFF; }

static uint32_t canon23(uint32_t w)
{
    uint32_t m = w;
    for (int k = 1; k < 23; k++) {
        const uint32_t r = rot23(w, k);
        m = r < m ? r : m;
    }
    return m;
}

static void dcs_table(void)
{
    if (dcs_n)
        return;
    int n = 0;
    for (int inv = 0; inv < 2; inv++)
        for (int i = 0; i < rdof_dcs_ncodes && n < DCS_ENTRIES; i++) {
            const uint32_t w = rdof_dcs_word(rdof_dcs_codes[i]);
            dcs_tab[n++] = (dcs_entry_t){canon23(inv ? ~w & 0x7FFFFF : w), rdof_dcs_codes[i], inv != 0};
        }
    dcs_n = n;
}

int rdof_dcs_lookup(uint32_t word, bool *inv)
{
    dcs_table();
    const uint32_t c = canon23(word & 0x7FFFFF);
    for (int i = 0; i < dcs_n; i++) /* normal codes first, each by its number: the name a radio would show */
        if (dcs_tab[i].canon == c) {
            *inv = dcs_tab[i].inv;
            return dcs_tab[i].code;
        }
    return -1;
}

/* ── the spectrum ── */

/* ── the subaudio ── */

/* The last quarter second of subaudio: a CTCSS tone (or the DCS turn-off), by its zero crossings and a Goertzel
 * there. */
static void ctcss_window(rdof_fmrx_t *r)
{
    const int L = r->rlen;
    double p = 0, mean = 0;
    for (int i = 0; i < L; i++)
        mean += r->ring[(r->rw - L + i + RING) % RING];
    mean /= L;
    double first = -1, last = -1;
    long cross = 0;
    float prev = 0;
    for (int i = 0; i < L; i++) {
        const float x = r->ring[(r->rw - L + i + RING) % RING] - (float)mean;
        p += (double)x * x;
        if (i && prev < 0 && x >= 0) {
            const double t = i - 1 + prev / (prev - x);
            if (first < 0)
                first = t;
            last = t, cross++;
        }
        prev = x;
    }
    p /= L;
    if (cross < 3 || !(p > 0))
        return;
    const double f = (cross - 1) / (last - first) * r->sr;
    const double w = 2 * M_PI * f / r->sr, k = 2 * cos(w);
    double s1 = 0, s2 = 0;
    for (int i = 0; i < L; i++) {
        const double s = (r->ring[(r->rw - L + i + RING) % RING] - mean) + k * s1 - s2;
        s2 = s1, s1 = s;
    }
    const double g2 = s1 * s1 + s2 * s2 - k * s1 * s2, tone = 2 * g2 / ((double)L * L);
    if (tone / p < 0.7)
        return; /* not one tone */
    if (r->dc_words && fabs(f - RDOF_FMG_DCS_OFF_HZ) < 1.5) {
        r->dc_turnoff = true;
        return;
    }
    int best = -1;
    for (int i = 0; i < rdof_ctcss_ntones; i++)
        if (best < 0 || fabs(rdof_ctcss_tones[i] - f) < fabs(rdof_ctcss_tones[best] - f))
            best = i;
    r->ct_hits[best]++;
    r->ct_fsum += f;
    r->ct_devsum += sqrt(2 * tone) / rdof_bq_mag(r->lps, 3, f, r->sr);
    r->ct_n++;
    if (!r->notching) { /* the filtered audio loses it from here on */
        rdof_bq_set(&r->notch, RDOF_BQ_NOTCH, f, 4.0, r->ar);
        r->notching = true;
    }
    if (!(r->ct_w > 0)) {
        r->ct_w = w;
        r->zlen = (int)lround(0.04 * r->sr);
    }
}

/* The CTCSS tone's phase over 40 ms blocks: a turn of about 180 degrees is the reverse burst. */
static void reverse_in(rdof_fmrx_t *r, float x)
{
    if (!(r->ct_w > 0))
        return;
    r->zr += x * cos(r->ct_ph), r->zi -= x * sin(r->ct_ph);
    r->ct_ph = fmod(r->ct_ph + r->ct_w, 2 * M_PI);
    if (++r->zn < r->zlen)
        return;
    const double m = hypot(r->zr, r->zi), m2 = hypot(r->zr2, r->zi2);
    if (r->zk >= 2 && m > 0.5 * r->zmag && m2 > 0.5 * r->zmag) {
        const double d = atan2(r->zi * r->zr2 - r->zr * r->zi2, r->zr * r->zr2 + r->zi * r->zi2);
        if (fabs(d) > 0.8 * M_PI)
            r->reverse_t = (r->sub_n - 1.5 * r->zlen) / r->sr;
    }
    r->zmag = r->zk ? 0.9 * r->zmag + 0.1 * m : m;
    r->zr2 = r->zr1, r->zi2 = r->zi1, r->zr1 = r->zr, r->zi1 = r->zi;
    r->zr = r->zi = 0, r->zn = 0, r->zk++;
}

/* DCS: the bits at 134.4 a second, the clock from their edges; the words they make. */
static void dcs_in(rdof_fmrx_t *r, float x)
{
    r->dc_mean += (x - r->dc_mean) * (float)(1 / r->sr);
    const float v = x - (r->dc_mean - r->dc_bias); /* about the levels' midpoint */
    const int s = v >= 0 ? 1 : -1;
    const float was = r->dc_ph;
    r->dc_ph += r->dc_step;
    if (s != r->dc_sign) { /* an edge: the bit boundary is here */
        const float e = r->dc_ph >= 0.5f ? r->dc_ph - 1 : r->dc_ph;
        r->dc_ph -= 0.3f * e;
        r->dc_sign = s;
    }
    if (was < 0.5f && r->dc_ph >= 0.5f) {
        r->reg = (r->reg >> 1) | ((uint32_t)(s > 0) << 22);
        r->dc_x[0] = r->dc_x[1], r->dc_x[1] = r->dc_x[2], r->dc_x[2] = x;
        r->dc_b[0] = r->dc_b[1], r->dc_b[1] = r->dc_b[2], r->dc_b[2] = s;
        if (r->dc_run && r->dc_b[0] == r->dc_b[1] && r->dc_b[1] == r->dc_b[2]) {
            /* the middle of three equal bits is at its full level: the two levels, from them */
            if (s > 0)
                r->dc_sp += r->dc_x[1], r->dc_np++;
            else
                r->dc_sn += r->dc_x[1], r->dc_nn++;
            if (r->dc_np && r->dc_nn) {
                const double hi = r->dc_sp / (double)r->dc_np, lo = r->dc_sn / (double)r->dc_nn;
                r->dc_amp = (float)((hi - lo) / 2), r->dc_bias = (float)(-(hi + lo) / 2);
            }
        }
        if (++r->dc_bits >= 23) {
            const uint32_t c = canon23(r->reg);
            bool hit = false;
            for (int i = 0; i < dcs_n && !hit; i++)
                hit = dcs_tab[i].canon == c;
            if (hit && c == r->dc_canon) {
                if (++r->dc_run % 23 == 0) {
                    r->dc_words++;
                    r->dc_found = c;
                    r->dc_code = rdof_dcs_lookup(r->reg, &r->dc_inv);
                }
            } else {
                r->dc_canon = hit ? c : 0;
                r->dc_run = hit;
            }
        }
    }
    while (r->dc_ph >= 1)
        r->dc_ph -= 1;
    while (r->dc_ph < 0)
        r->dc_ph += 1;
}

static void sub_in(rdof_fmrx_t *r, float x, bool measure)
{
    for (int i = 0; i < 3; i++)
        x = rdof_bq(&r->lps[i], x);
    r->sub_n++;
    if (!measure) {
        r->rn = 0;
        r->zk = r->zn = 0, r->zr = r->zi = 0;
        return;
    }
    r->b.s2sum += (double)x * x, r->b.ns++;
    r->s2_sum += (double)x * x, r->s_n++;
    r->ring[r->rw] = x;
    r->rw = (r->rw + 1) % RING;
    if (r->rn < r->rlen)
        r->rn++;
    if (r->rn == r->rlen && ++r->rsince >= r->rlen / 8) {
        r->rsince = 0;
        ctcss_window(r);
    }
    reverse_in(r, x);
    dcs_in(r, x);
}

static void block_end(rdof_fmrx_t *r)
{
    acc_t *a = &r->b;
    rdof_fmrx_block_t b = {0};
    b.t = (r->pos - (double)a->n) / r->fs;
    const double p = a->n ? a->psum / (double)a->n : 0;
    b.power_dbfs = (float)(10 * log10(p + 1e-20));
    b.carrier = a->gn * 2 > a->n;
    b.env_min = a->emin < 1e8f ? a->emin : 0, b.env_max = a->emax;
    /* the offset tracked (0.5 s): a block's plain mean moves with its share of a DCS word */
    b.cfo_hz = a->gn ? (r->gated > r->learn ? r->cfo : (float)(a->gfsum / (double)a->gn)) - r->dc_bias : NAN;
    b.dev_pos_hz = a->dpos + r->dc_bias, b.dev_neg_hz = a->dneg + r->dc_bias;
    b.audio_rms_hz = a->na ? (float)sqrt(a->a2sum / (double)a->na) : 0;
    b.sub_rms_hz = a->ns ? (float)sqrt(a->s2sum / (double)a->ns) : 0;
    if (b.carrier) {
        r->blocks_on++;
        r->p_min = r->blocks_on == 1 || b.power_dbfs < r->p_min ? b.power_dbfs : r->p_min;
        r->p_max = r->blocks_on == 1 || b.power_dbfs > r->p_max ? b.power_dbfs : r->p_max;
        r->cfo_min = r->blocks_on == 1 || b.cfo_hz < r->cfo_min ? b.cfo_hz : r->cfo_min;
        r->cfo_max = r->blocks_on == 1 || b.cfo_hz > r->cfo_max ? b.cfo_hz : r->cfo_max;
    }
    if (r->qn == BLOCKS) /* the oldest goes */
        r->qh = (r->qh + 1) % BLOCKS, r->qn--;
    r->q[(r->qh + r->qn++) % BLOCKS] = b;
    memset(a, 0, sizeof *a);
    a->emin = 1e9f;
}

/* ── the receiver ── */

rdof_fmrx_t *rdof_fmrx_new(double fs, const rdof_fmrx_opts_t *o, void *(*alloc)(size_t), void (*dealloc)(void *))
{
    if (!(fs >= 24000 && fs <= 200000))
        return NULL;
    rdof_fmrx_t *r = alloc ? alloc(sizeof *r) : malloc(sizeof *r);
    if (!r)
        return NULL;
    memset(r, 0, sizeof *r);
    dcs_table();
    r->o = *o;
    if (!(r->o.block_s >= 0.001f))
        r->o.block_s = 0.1f;
    r->dealloc = dealloc ? dealloc : free;
    r->fs = fs;
    r->scale = (float)(fs / (2 * M_PI));
    r->ps_a = (float)(1 / (0.002 * fs));
    r->pf_a = (float)(1 / (0.00025 * fs));
    r->sq_lin = (float)pow(10, r->o.squelch_dbfs / 10);
    r->D = (int)lround(DELAY_S * fs);
    r->da = (int)lround(fs / 16000);
    r->ar = fs / r->da;
    r->ds = (int)lround(r->ar / 4000);
    r->sr = r->ar / r->ds;
    r->learn = lround(LEARN_S * r->ar);
    r->settle = r->learn + lround(SETTLE_S * r->ar);
    r->bsize = lround(r->o.block_s * fs);
    if (r->bsize < 1)
        r->bsize = 1;
    rdof_bq_butter(r->lp5, 2, RDOF_BQ_LP, 5000, fs);
    rdof_bq_butter(r->hp, 3, RDOF_BQ_HP, 300, r->ar);
    rdof_bq_butter(r->lp3, 2, RDOF_BQ_LP, 3000, r->ar);
    rdof_bq_butter(r->lps, 3, RDOF_BQ_LP, 270, r->sr);
    if (r->o.deemph_us > 0) {
        const double tau = r->o.deemph_us * 1e-6;
        r->de_a = (float)(1 - exp(-1 / (tau * r->ar)));
        r->de_g = (float)sqrt(1 + pow(2 * M_PI * 1000 * tau, 2));
    }
    r->cfo_a = (float)(1 / (0.5 * r->ar));
    if (rdof_psd_init(&r->psd, N) < 0) {
        r->dealloc(r);
        return NULL;
    }
    r->rlen = (int)lround(r->sr / 4);
    if (r->rlen > RING)
        r->rlen = RING;
    r->dc_step = (float)(RDOF_FMG_DCS_BAUD / r->sr);
    r->dc_sign = 1;
    r->dc_code = -1;
    r->reverse_t = -1;
    r->b.emin = 1e9f;
    return r;
}

void rdof_fmrx_free(rdof_fmrx_t *r)
{
    if (!r)
        return;
    rdof_psd_free(&r->psd);
    r->dealloc(r);
}

double rdof_fmrx_audio_rate(const rdof_fmrx_t *r) { return r->ar; }

double rdof_fmrx_delay_s(const rdof_fmrx_t *r) { return r->D / r->fs; }

/* One sample out of the delay: carrier (gate), noise (quiet), or neither (an edge). */
static void sample(rdof_fmrx_t *r, float x, float y, float f, bool gate, bool quiet, float *raw, float *audio,
                   int *na)
{
    const float p = x * x + y * y, e = sqrtf(p);
    if (raw)
        *raw = f;
    acc_t *a = &r->b;
    a->psum += p, a->n++;
    if (gate) {
        a->gfsum += f, a->gn++;
        a->emin = e < a->emin ? e : a->emin;
        a->emax = e > a->emax ? e : a->emax;
        r->p_sum += p, r->p_n++;
        r->env_sum += e, r->env2_sum += (double)e * e, r->env_n++;
    } else if (quiet) {
        r->noise_sum += p, r->noise_n++;
    }
    if (gate && !r->gate)
        r->gated = 0, r->learn_sum = 0;
    r->gate = gate;
    const float f5 = rdof_bq(&r->lp5[1], rdof_bq(&r->lp5[0], f));
    if (++r->dk < r->da)
        return;
    r->dk = 0; /* the audio rate */
    float d = 0;
    bool measure = false;
    if (gate) {
        r->gated++;
        if (r->gated <= r->learn) { /* the offset first: what the carrier sits at */
            r->learn_sum += f5;
            if (r->gated == r->learn)
                r->cfo = (float)(r->learn_sum / r->learn);
        } else {
            r->cfo += (f5 - r->cfo) * r->cfo_a;
            d = f5 - r->cfo;
            measure = r->gated > r->settle;
        }
    }
    if (measure) {
        r->cfo_sum += r->cfo, r->cfo_n++;
        a->dpos = d > a->dpos ? d : a->dpos;
        a->dneg = d < a->dneg ? d : a->dneg;
        r->dpos = d > r->dpos ? d : r->dpos;
        r->dneg = d < r->dneg ? d : r->dneg;
        r->d2_sum += (double)d * d, r->d_n++;
        rdof_psd_in(&r->psd, d);
    }
    float u = d;
    for (int k = 0; k < 3; k++)
        u = rdof_bq(&r->hp[k], u);
    if (r->notching)
        u = rdof_bq(&r->notch, u);
    u = rdof_bq(&r->lp3[1], rdof_bq(&r->lp3[0], u));
    if (measure) {
        a->a2sum += (double)u * u, a->na++;
        r->a2_sum += (double)u * u, r->a_n++;
        r->apos = u > r->apos ? u : r->apos;
        r->aneg = u < r->aneg ? u : r->aneg;
    }
    float out = u;
    if (r->de_a > 0) {
        r->de += (u - r->de) * r->de_a;
        out = r->de * r->de_g;
    }
    if (audio)
        audio[(*na)++] = out;
    r->sub_acc += d;
    if (++r->sk == r->ds) { /* the subaudio's rate */
        r->sk = 0;
        sub_in(r, r->sub_acc / (float)r->ds, measure);
        r->sub_acc = 0;
    }
}

void rdof_fmrx_feed(rdof_fmrx_t *r, const float *iq, int n, float *raw, float *audio, int *naudio)
{
    enum { B = 64 };
    rdof_cf_t x[B];
    float f[B];
    uint8_t g[B];
    int na = 0;
    for (int i0 = 0; i0 < n; i0 += B) {
        const int m = n - i0 < B ? n - i0 : B;
        for (int j = 0; j < m; j++) {
            const int i = i0 + j;
            const float xi = iq[2 * i], xq = iq[2 * i + 1];
            r->ps += (xi * xi + xq * xq - r->ps) * r->ps_a;
            r->pf += (xi * xi + xq * xq - r->pf) * r->pf_a;
            /* on: the smoothed power over the squelch, and the fast one not 6 dB under it (a carrier that stopped) */
            const bool on = r->ps >= r->sq_lin && r->pf >= 0.25f * r->sq_lin;
            r->since_on = on ? r->since_on + 1 : 0;
            if (r->since_on)
                r->carrier_s += 1 / r->fs;
            r->since_off = r->ps < r->sq_lin ? r->since_off + 1 : 0;
            /* the sample DELAY_S back: gated when the power was over the squelch from DELAY_S before it to DELAY_S
             * after */
            float *d = &r->dly[2 * r->di];
            x[j] = (rdof_cf_t){d[0], d[1]};
            d[0] = xi, d[1] = xq;
            if (++r->di == r->D)
                r->di = 0;
            g[j] = (uint8_t)((r->since_on > 2 * r->D) | (r->since_off > 2 * r->D) << 1);
        }
        rdof_fm_discriminate(&r->prev, r->scale, x, m, f); /* the shared one (its fast atan2) */
        for (int j = 0; j < m; j++) {
            sample(r, x[j].re, x[j].im, f[j], g[j] & 1, g[j] & 2, raw ? &raw[i0 + j] : NULL, audio, &na);
            r->pos++;
            if (r->b.n >= r->bsize)
                block_end(r);
        }
    }
    if (naudio)
        *naudio = na;
}

bool rdof_fmrx_block(rdof_fmrx_t *r, rdof_fmrx_block_t *b)
{
    if (!r->qn)
        return false;
    *b = r->q[r->qh];
    r->qh = (r->qh + 1) % BLOCKS, r->qn--;
    return true;
}

static double bin_hz(const rdof_fmrx_t *r) { return r->ar / N; }

/* The power of the tone about bin k (its Hann lobe, k - 3 to k + 3), and its frequency by the bins' centroid. */
static double lobe(const rdof_fmrx_t *r, const double *s, int k, double *hz)
{
    double p = 0, m = 0;
    for (int j = k - 3; j <= k + 3; j++)
        if (j >= 0 && j <= N / 2)
            p += s[j], m += s[j] * j;
    if (hz)
        *hz = p > 0 ? m / p * bin_hz(r) : k * bin_hz(r);
    return p;
}

void rdof_fmrx_summary(rdof_fmrx_t *r, rdof_fmrx_summary_t *s)
{
    memset(s, 0, sizeof *s);
    s->seconds = r->pos / r->fs;
    s->carrier_s = r->carrier_s;
    s->power_dbfs = r->p_n ? (float)(10 * log10(r->p_sum / r->p_n + 1e-20)) : NAN;
    s->power_min_dbfs = r->blocks_on ? r->p_min : NAN;
    s->power_max_dbfs = r->blocks_on ? r->p_max : NAN;
    s->noise_dbfs = r->noise_n ? (float)(10 * log10(r->noise_sum / r->noise_n + 1e-20)) : NAN;
    if (r->env_n) {
        const double m = r->env_sum / r->env_n, v = r->env2_sum / r->env_n - m * m;
        s->env_mean = (float)m;
        s->am_pct = m > 0 ? (float)(100 * sqrt(v > 0 ? v : 0) / m) : 0;
    }
    s->cfo_hz = r->cfo_n ? (float)(r->cfo_sum / r->cfo_n) - r->dc_bias : NAN;
    s->cfo_min_hz = r->blocks_on ? r->cfo_min : NAN;
    s->cfo_max_hz = r->blocks_on ? r->cfo_max : NAN;
    s->dev_pos_hz = r->dpos + r->dc_bias, s->dev_neg_hz = r->dneg + r->dc_bias;
    s->dev_rms_hz = r->d_n ? (float)sqrt(r->d2_sum / r->d_n) : 0;
    s->audio_rms_hz = r->a_n ? (float)sqrt(r->a2_sum / r->a_n) : 0;
    s->audio_pos_hz = r->apos, s->audio_neg_hz = r->aneg;
    s->audio_rms_db = r->o.nominal_dev_hz > 0 && s->audio_rms_hz > 0
                          ? (float)(20 * log10(s->audio_rms_hz * sqrt(2.0) / r->o.nominal_dev_hz))
                          : NAN;
    s->sub_rms_hz = r->s_n ? (float)sqrt(r->s2_sum / r->s_n) : 0;
    s->reverse_burst_t = -1;
    s->dcs_code = -1;
    /* the spectrum: the audio band's strongest tone, its SINAD and THD; the peaks anywhere */
    if (r->psd.frames) {
        const double *sp = r->psd.acc, inv = 1.0 / (double)r->psd.frames; /* sums: the mean is inv times */
        const double bh = bin_hz(r);
        const int lo = (int)ceil(300 / bh), hi = (int)floor(3000 / bh);
        int k0 = lo;
        double tot = 0;
        for (int k = lo; k <= hi && k <= N / 2; k++) {
            tot += sp[k];
            if (sp[k] > sp[k0])
                k0 = k;
        }
        double f0;
        const double p0 = lobe(r, sp, k0, &f0);
        if (tot > 0 && p0 / tot > 0.5) {
            s->tone = true;
            s->tone_hz = (float)f0;
            s->tone_dev_hz = (float)sqrt(2 * p0 * inv);
            const double rest = tot - p0;
            s->sinad_db = (float)(rest > 0 ? fmin(10 * log10(tot / rest), 90) : 90);
            double h = 0;
            for (int m = 2; m <= 5; m++) {
                const int kh = (int)lround(m * f0 / bh);
                if (kh + 3 <= N / 2)
                    h += lobe(r, sp, kh, NULL);
            }
            s->thd_pct = (float)(100 * sqrt(h / p0));
        }
        const int top = (int)fmin(N / 2 - 1, floor(5000 / bh));
        double maxp = 0;
        for (int k = 1; k < top; k++)
            maxp = sp[k] > maxp ? sp[k] : maxp;
        for (int n = 0; n < RDOF_FMRX_PEAKS; n++) { /* the strongest local maxima, apart */
            int best = -1;
            for (int k = (int)ceil(30 / bh); k < top; k++) {
                if (sp[k] < sp[k - 1] || sp[k] < sp[k + 1] || sp[k] < 0.001 * maxp)
                    continue;
                bool near = false;
                for (int j = 0; j < s->npeaks; j++)
                    near |= fabs(k * bh - s->peak_hz[j]) < 5 * bh;
                if (!near && (best < 0 || sp[k] > sp[best]))
                    best = k;
            }
            if (best < 0)
                break;
            double hz;
            const double p = lobe(r, sp, best, &hz);
            s->peak_hz[s->npeaks] = (float)hz;
            s->peak_dev_hz[s->npeaks++] = (float)sqrt(2 * p * inv);
        }
    }
    /* the subaudio */
    if (r->dc_words) {
        s->sub = RDOF_FMG_SUB_DCS;
        s->dcs_code = r->dc_code;
        s->dcs_inv = r->dc_inv;
        s->dcs_words = r->dc_words;
        s->dcs_turnoff = r->dc_turnoff;
        s->dcs_dev_hz = r->dc_amp;
        const uint32_t c = r->dc_found;
        size_t at = 0;
        for (int i = 0; i < dcs_n && c; i++)
            if (dcs_tab[i].canon == c && !(dcs_tab[i].code == r->dc_code && dcs_tab[i].inv == r->dc_inv) &&
                at + 6 < sizeof s->dcs_aliases)
                at += (size_t)snprintf(s->dcs_aliases + at, sizeof s->dcs_aliases - at, "%s%03o%c", at ? " " : "",
                                       dcs_tab[i].code, dcs_tab[i].inv ? 'I' : 'N');
    } else if (r->ct_n) {
        int best = 0;
        for (int i = 1; i < rdof_ctcss_ntones; i++)
            best = r->ct_hits[i] > r->ct_hits[best] ? i : best;
        s->sub = RDOF_FMG_SUB_CTCSS;
        s->ctcss_hz = (float)(r->ct_fsum / r->ct_n);
        s->ctcss_nominal_hz = rdof_ctcss_tones[best];
        s->ctcss_dev_hz = (float)(r->ct_devsum / r->ct_n);
        s->reverse_burst_t = r->reverse_t;
    }
}

#define PUT(...)                                                                    \
    do {                                                                            \
        const int k_ = snprintf(out + at, at < cap ? cap - at : 0, __VA_ARGS__);   \
        if (k_ < 0 || (at += (size_t)k_) >= cap)                                    \
            return 0;                                                               \
    } while (0)

/* A float as JSON: null for NAN. */
static const char *jf(char *b, size_t n, double v, int dp)
{
    if (!isfinite(v))
        snprintf(b, n, "null");
    else
        snprintf(b, n, "%.*f", dp, v);
    return b;
}

size_t rdof_fmrx_summary_json(rdof_fmrx_t *r, const rdof_fmrx_summary_t *s, char *out, size_t cap)
{
    size_t at = 0;
    char a[24], b[24], c[24], d[24];
    PUT("{\"seconds\":%.3f,\"carrier_s\":%.3f,\"power_dbfs\":%s,\"power_min_dbfs\":%s,\"power_max_dbfs\":%s,"
        "\"noise_dbfs\":%s,",
        s->seconds, s->carrier_s, jf(a, sizeof a, s->power_dbfs, 1), jf(b, sizeof b, s->power_min_dbfs, 1),
        jf(c, sizeof c, s->power_max_dbfs, 1), jf(d, sizeof d, s->noise_dbfs, 1));
    PUT("\"envelope\":{\"mean\":%.5f,\"am_pct\":%.2f},\"cfo_hz\":%s,\"cfo_min_hz\":%s,\"cfo_max_hz\":%s,",
        s->env_mean, s->am_pct, jf(a, sizeof a, s->cfo_hz, 1), jf(b, sizeof b, s->cfo_min_hz, 1),
        jf(c, sizeof c, s->cfo_max_hz, 1));
    PUT("\"deviation\":{\"pos_hz\":%.0f,\"neg_hz\":%.0f,\"rms_hz\":%.0f},", s->dev_pos_hz, s->dev_neg_hz,
        s->dev_rms_hz);
    PUT("\"audio\":{\"rms_hz\":%.1f,\"pos_hz\":%.0f,\"neg_hz\":%.0f,\"rms_db\":%s,\"tone\":", s->audio_rms_hz,
        s->audio_pos_hz, s->audio_neg_hz, jf(a, sizeof a, s->audio_rms_db, 2));
    if (s->tone)
        PUT("{\"hz\":%.2f,\"dev_hz\":%.1f,\"sinad_db\":%.1f,\"thd_pct\":%.2f}", s->tone_hz, s->tone_dev_hz,
            s->sinad_db, s->thd_pct);
    else
        PUT("null");
    PUT(",\"peaks\":[");
    for (int i = 0; i < s->npeaks; i++)
        PUT("%s{\"hz\":%.1f,\"dev_hz\":%.1f}", i ? "," : "", s->peak_hz[i], s->peak_dev_hz[i]);
    PUT("]},\"sub\":");
    if (s->sub == RDOF_FMG_SUB_CTCSS)
        PUT("{\"kind\":\"ctcss\",\"hz\":%.2f,\"nominal_hz\":%.1f,\"dev_hz\":%.1f,\"reverse_burst_t\":%s}", s->ctcss_hz,
            s->ctcss_nominal_hz, s->ctcss_dev_hz, jf(a, sizeof a, s->reverse_burst_t >= 0 ? s->reverse_burst_t : NAN, 3));
    else if (s->sub == RDOF_FMG_SUB_DCS)
        PUT("{\"kind\":\"dcs\",\"code\":\"%03o%c\",\"aliases\":\"%s\",\"words\":%ld,\"dev_hz\":%.1f,\"turnoff\":%s}",
            s->dcs_code, s->dcs_inv ? 'I' : 'N', s->dcs_aliases, s->dcs_words, s->dcs_dev_hz,
            s->dcs_turnoff ? "true" : "false");
    else
        PUT("{\"kind\":\"none\",\"rms_hz\":%.1f}", s->sub_rms_hz);
    if (r->o.spectrum_hz > 0 && r->psd.frames) {
        const double bh = bin_hz(r);
        const int top = (int)fmin(N / 2, floor(r->o.spectrum_hz / bh));
        PUT(",\"spectrum\":{\"bin_hz\":%.4f,\"unit\":\"dB Hz^2\",\"db\":[", bh);
        for (int k = 0; k <= top; k++)
            PUT("%s%.1f", k ? "," : "", 10 * log10(r->psd.acc[k] / r->psd.frames + 1e-12));
        PUT("]}");
    }
    PUT("}");
    return at;
}

size_t rdof_fmrx_block_json(const rdof_fmrx_block_t *b, char *out, size_t cap)
{
    size_t at = 0;
    char c[24];
    PUT("{\"t\":%.3f,\"carrier\":%s,\"power_dbfs\":%.1f,\"env_min\":%.4f,\"env_max\":%.4f,\"cfo_hz\":%s,"
        "\"dev_pos_hz\":%.0f,\"dev_neg_hz\":%.0f,\"audio_rms_hz\":%.1f,\"sub_rms_hz\":%.1f}",
        b->t, b->carrier ? "true" : "false", b->power_dbfs, b->env_min, b->env_max, jf(c, sizeof c, b->cfo_hz, 1),
        b->dev_pos_hz, b->dev_neg_hz, b->audio_rms_hz, b->sub_rms_hz);
    return at;
}

#endif
