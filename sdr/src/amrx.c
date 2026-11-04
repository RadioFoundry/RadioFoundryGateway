// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* An analog AM receiver that measures as it goes (rdof/sdr/amrx.h). */
#include "rdof/sdr/amrx.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/sdr/features.h"
#if RDOF_FEATURE_SDR

#include "rdof/sdr/fm.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define N RDOF_AMRX_FFT
#define BLOCKS 64                /* finished blocks waiting to be taken */

typedef struct {
    double psum, csum, a2sum, f2sum;
    float mpos, mneg;
    long n, gn, nc, na;
} acc_t;

/* The per-sample sums, in float (an ESP32's FPU has no double), folded into the doubles (fold) every FOLD input
 * samples and before they are read: as fmrx.c does. */
typedef struct {
    float psum, csum, a2sum, f2sum;                          /* the block's */
    float p, noise, c, m2, a2, f2, cfo;                      /* the summary's */
} part_t;
#define FOLD 256

#define DELAY_S 0.01             /* the gate looks this far ahead and back */
#define LEARN_S 0.03             /* after the gate opens: the carrier's level and offset learned, nothing measured */
#define SETTLE_S 0.03            /* then the filters settle: still nothing measured */
#define CARRIER_S 0.2            /* the carrier's level tracked over this */
#define CFO_S 0.5                /* and its offset over this */

struct rdof_amrx {
    rdof_amrx_opts_t o;
    void (*dealloc)(void *);
    double fs, ar;
    int da, dk;
    rdof_cf_t prev;              /* the discriminator's last sample */
    float scale;
    /* the input rate: the gate (fmrx.c's) and the samples it delays */
    float ps, ps_a, sq_lin;
    float pf, pf_a;
    long since_on, since_off;
    int D, di;
    float dly[2 * 2000];         /* DELAY_S at 200 kS/s */
    bool gate;
    rdof_bq_t lpe[2], lpf[2];    /* the envelope's and the discriminator's 5 kHz low-pass */
    int64_t pos;                 /* input samples out of the delay */
    part_t fp;
    int fpn;
    long bsize;
    acc_t b;
    /* the audio rate */
    long gated;                  /* audio samples since the gate opened */
    long learn, settle;
    float learn_e, learn_f;
    float car, car_a;            /* the carrier's level (|x|) */
    float cfo, cfo_a;
    rdof_bq_t hp[3], lp[2];
    rdof_psd_t psd;
    /* the summary's sums (over gated samples) */
    long carrier_n;
    double p_sum, noise_sum, c_sum, m2_sum, a2_sum, f2_sum, cfo_sum;
    long p_n, noise_n, m_n, a_n, blocks_on;
    float p_min, p_max, cfo_min, cfo_max, mpos, mneg, apos, aneg, fpos, fneg;
    rdof_amrx_block_t q[BLOCKS];
    int qh, qn;
};

static void fold(rdof_amrx_t *r)
{
    part_t *f = &r->fp;
    acc_t *a = &r->b;
    a->psum += f->psum, a->csum += f->csum, a->a2sum += f->a2sum, a->f2sum += f->f2sum;
    r->p_sum += f->p, r->noise_sum += f->noise, r->c_sum += f->c, r->m2_sum += f->m2;
    r->a2_sum += f->a2, r->f2_sum += f->f2, r->cfo_sum += f->cfo;
    memset(f, 0, sizeof *f);
    r->fpn = 0;
}

static void block_end(rdof_amrx_t *r)
{
    fold(r);
    acc_t *a = &r->b;
    rdof_amrx_block_t b = {0};
    b.t = (double)(r->pos - a->n) / r->fs;
    const double p = a->n ? a->psum / (double)a->n : 0;
    b.power_dbfs = (float)(10 * log10(p + 1e-20));
    b.carrier = a->gn * 2 > a->n;
    b.carrier_dbfs = a->nc ? (float)(20 * log10(a->csum / (double)a->nc + 1e-10)) : NAN;
    b.cfo_hz = a->nc ? r->cfo : NAN;
    b.depth_pos = a->mpos, b.depth_neg = -a->mneg;
    b.audio_rms = a->na ? (float)sqrt(a->a2sum / (double)a->na) : 0;
    b.fm_rms_hz = a->nc ? (float)sqrt(a->f2sum / (double)a->nc) : 0;
    if (b.carrier && a->nc) {
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
}

/* ── the receiver ── */

rdof_amrx_t *rdof_amrx_new(double fs, const rdof_amrx_opts_t *o, void *(*alloc)(size_t), void (*dealloc)(void *))
{
    if (!(fs >= 24000 && fs <= 200000))
        return NULL;
    rdof_amrx_opts_t op = *o;
    if (!(op.block_s >= 0.001f))
        op.block_s = 0.1f;
    if (!(op.audio_lo_hz > 0))
        op.audio_lo_hz = 300;
    if (!(op.audio_hi_hz > 0))
        op.audio_hi_hz = 3000;
    const int da = (int)lround(fs / 16000);
    if (!(op.audio_lo_hz < op.audio_hi_hz && op.audio_hi_hz <= 0.4 * fs / da))
        return NULL;
    rdof_amrx_t *r = alloc ? alloc(sizeof *r) : malloc(sizeof *r);
    if (!r)
        return NULL;
    memset(r, 0, sizeof *r);
    r->o = op;
    r->dealloc = dealloc ? dealloc : free;
    r->fs = fs;
    r->scale = (float)(fs / (2 * M_PI));
    r->ps_a = (float)(1 / (0.002 * fs));
    r->pf_a = (float)(1 / (0.00025 * fs));
    r->sq_lin = (float)pow(10, r->o.squelch_dbfs / 10);
    r->D = (int)lround(DELAY_S * fs);
    r->da = da;
    r->ar = fs / r->da;
    r->learn = lround(LEARN_S * r->ar);
    r->settle = r->learn + lround(SETTLE_S * r->ar);
    r->bsize = lround(r->o.block_s * fs);
    if (r->bsize < 1)
        r->bsize = 1;
    rdof_bq_butter(r->lpe, 2, RDOF_BQ_LP, 5000, fs);
    rdof_bq_butter(r->lpf, 2, RDOF_BQ_LP, 5000, fs);
    rdof_bq_butter(r->hp, 3, RDOF_BQ_HP, r->o.audio_lo_hz, r->ar);
    rdof_bq_butter(r->lp, 2, RDOF_BQ_LP, r->o.audio_hi_hz, r->ar);
    r->car_a = (float)(1 / (CARRIER_S * r->ar));
    r->cfo_a = (float)(1 / (CFO_S * r->ar));
    if (rdof_psd_init(&r->psd, N) < 0) {
        r->dealloc(r);
        return NULL;
    }
    return r;
}

void rdof_amrx_free(rdof_amrx_t *r)
{
    if (!r)
        return;
    rdof_psd_free(&r->psd);
    r->dealloc(r);
}

double rdof_amrx_audio_rate(const rdof_amrx_t *r) { return r->ar; }

double rdof_amrx_delay_s(const rdof_amrx_t *r) { return r->D / r->fs; }

/* One sample out of the delay: carrier (gate), noise (quiet), or neither (an edge). */
static void sample(rdof_amrx_t *r, float x, float y, float f, bool gate, bool quiet, float *env, float *audio, int *na)
{
    const float p = x * x + y * y, e = sqrtf(p);
    if (env)
        *env = e;
    acc_t *a = &r->b;
    r->fp.psum += p, a->n++;
    if (++r->fpn == FOLD)
        fold(r);
    if (gate) {
        a->gn++;
        r->fp.p += p, r->p_n++;
    } else if (quiet) {
        r->fp.noise += p, r->noise_n++;
    }
    if (gate && !r->gate)
        r->gated = 0, r->learn_e = r->learn_f = 0;
    r->gate = gate;
    const float e5 = rdof_bq(&r->lpe[1], rdof_bq(&r->lpe[0], e));
    const float f5 = rdof_bq(&r->lpf[1], rdof_bq(&r->lpf[0], f));
    if (++r->dk < r->da)
        return;
    r->dk = 0; /* the audio rate */
    float m = 0, d = 0;
    bool measure = false;
    if (gate) {
        r->gated++;
        if (r->gated <= r->learn) { /* the carrier first: its level and where it sits */
            r->learn_e += e5, r->learn_f += f5;
            if (r->gated == r->learn)
                r->car = r->learn_e / (float)r->learn, r->cfo = r->learn_f / (float)r->learn;
        } else {
            r->car += (e5 - r->car) * r->car_a;
            r->cfo += (f5 - r->cfo) * r->cfo_a;
            m = r->car > 0 ? e5 / r->car - 1 : 0;
            d = f5 - r->cfo;
            measure = r->gated > r->settle;
        }
    }
    if (measure) {
        r->fp.csum += r->car, r->fp.f2sum += d * d, a->nc++;
        r->fp.c += r->car, r->fp.cfo += r->cfo, r->fp.f2 += d * d, r->fp.m2 += m * m, r->m_n++;
        a->mpos = m > a->mpos ? m : a->mpos;
        a->mneg = m < a->mneg ? m : a->mneg;
        r->mpos = m > r->mpos ? m : r->mpos;
        r->mneg = m < r->mneg ? m : r->mneg;
        r->fpos = d > r->fpos ? d : r->fpos;
        r->fneg = d < r->fneg ? d : r->fneg;
        rdof_psd_in(&r->psd, m);
    }
    float u = m;
    for (int k = 0; k < 3; k++)
        u = rdof_bq(&r->hp[k], u);
    u = rdof_bq(&r->lp[1], rdof_bq(&r->lp[0], u));
    if (measure) {
        r->fp.a2sum += u * u, a->na++;
        r->fp.a2 += u * u, r->a_n++;
        r->apos = u > r->apos ? u : r->apos;
        r->aneg = u < r->aneg ? u : r->aneg;
    }
    if (audio)
        audio[(*na)++] = u;
}

void rdof_amrx_feed(rdof_amrx_t *r, const float *iq, int n, float *env, float *audio, int *naudio)
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
                r->carrier_n++;
            r->since_off = r->ps < r->sq_lin ? r->since_off + 1 : 0;
            float *d = &r->dly[2 * r->di];
            x[j] = (rdof_cf_t){d[0], d[1]};
            d[0] = xi, d[1] = xq;
            if (++r->di == r->D)
                r->di = 0;
            g[j] = (uint8_t)((r->since_on > 2 * r->D) | (r->since_off > 2 * r->D) << 1);
        }
        rdof_fm_discriminate(&r->prev, r->scale, x, m, f);
        for (int j = 0; j < m; j++) {
            sample(r, x[j].re, x[j].im, f[j], g[j] & 1, g[j] & 2, env ? &env[i0 + j] : NULL, audio, &na);
            r->pos++;
            if (r->b.n >= r->bsize)
                block_end(r);
        }
    }
    if (naudio)
        *naudio = na;
}

bool rdof_amrx_block(rdof_amrx_t *r, rdof_amrx_block_t *b)
{
    if (!r->qn)
        return false;
    *b = r->q[r->qh];
    r->qh = (r->qh + 1) % BLOCKS, r->qn--;
    return true;
}

void rdof_amrx_summary(rdof_amrx_t *r, rdof_amrx_summary_t *s)
{
    fold(r);
    memset(s, 0, sizeof *s);
    s->seconds = (double)r->pos / r->fs;
    s->carrier_s = r->carrier_n / r->fs;
    s->power_dbfs = r->p_n ? (float)(10 * log10(r->p_sum / r->p_n + 1e-20)) : NAN;
    s->power_min_dbfs = r->blocks_on ? r->p_min : NAN;
    s->power_max_dbfs = r->blocks_on ? r->p_max : NAN;
    s->noise_dbfs = r->noise_n ? (float)(10 * log10(r->noise_sum / r->noise_n + 1e-20)) : NAN;
    s->carrier_dbfs = r->m_n ? (float)(20 * log10(r->c_sum / r->m_n + 1e-10)) : NAN;
    s->cfo_hz = r->m_n ? (float)(r->cfo_sum / r->m_n) : NAN;
    s->cfo_min_hz = r->blocks_on ? r->cfo_min : NAN;
    s->cfo_max_hz = r->blocks_on ? r->cfo_max : NAN;
    s->depth_pos = r->mpos, s->depth_neg = -r->mneg;
    s->depth_rms = r->m_n ? (float)sqrt(r->m2_sum / r->m_n) : 0;
    s->audio_rms = r->a_n ? (float)sqrt(r->a2_sum / r->a_n) : 0;
    s->audio_pos = r->apos, s->audio_neg = -r->aneg;
    s->audio_rms_db = r->o.nominal_depth > 0 && s->audio_rms > 0
                          ? (float)(20 * log10(s->audio_rms * sqrt(2.0) / r->o.nominal_depth))
                          : NAN;
    s->fm_rms_hz = r->m_n ? (float)sqrt(r->f2_sum / r->m_n) : 0;
    s->fm_pos_hz = r->fpos, s->fm_neg_hz = r->fneg;
    rdof_psd_tones_t tn;
    rdof_psd_tones(&r->psd, r->ar, r->o.audio_lo_hz, r->o.audio_hi_hz, 5000, &tn);
    s->tone = tn.tone;
    s->tone_hz = tn.hz, s->tone_depth = tn.amp, s->sinad_db = tn.sinad_db, s->thd_pct = tn.thd_pct;
    s->npeaks = tn.npeaks;
    for (int i = 0; i < tn.npeaks; i++)
        s->peak_hz[i] = tn.peak_hz[i], s->peak_depth[i] = tn.peak_amp[i];
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

size_t rdof_amrx_summary_json(rdof_amrx_t *r, const rdof_amrx_summary_t *s, char *out, size_t cap)
{
    size_t at = 0;
    char a[24], b[24], c[24], d[24];
    PUT("{\"seconds\":%.3f,\"carrier_s\":%.3f,\"power_dbfs\":%s,\"power_min_dbfs\":%s,\"power_max_dbfs\":%s,"
        "\"noise_dbfs\":%s,",
        s->seconds, s->carrier_s, jf(a, sizeof a, s->power_dbfs, 1), jf(b, sizeof b, s->power_min_dbfs, 1),
        jf(c, sizeof c, s->power_max_dbfs, 1), jf(d, sizeof d, s->noise_dbfs, 1));
    PUT("\"carrier_dbfs\":%s,\"cfo_hz\":%s,\"cfo_min_hz\":%s,\"cfo_max_hz\":%s,", jf(a, sizeof a, s->carrier_dbfs, 2),
        jf(b, sizeof b, s->cfo_hz, 1), jf(c, sizeof c, s->cfo_min_hz, 1), jf(d, sizeof d, s->cfo_max_hz, 1));
    PUT("\"depth\":{\"pos_pct\":%.2f,\"neg_pct\":%.2f,\"rms_pct\":%.2f},", 100 * s->depth_pos, 100 * s->depth_neg,
        100 * s->depth_rms);
    PUT("\"audio\":{\"rms_pct\":%.2f,\"pos_pct\":%.2f,\"neg_pct\":%.2f,\"rms_db\":%s,\"tone\":", 100 * s->audio_rms,
        100 * s->audio_pos, 100 * s->audio_neg, jf(a, sizeof a, s->audio_rms_db, 2));
    if (s->tone)
        PUT("{\"hz\":%.2f,\"depth_pct\":%.2f,\"sinad_db\":%.1f,\"thd_pct\":%.2f}", s->tone_hz, 100 * s->tone_depth,
            s->sinad_db, s->thd_pct);
    else
        PUT("null");
    PUT(",\"peaks\":[");
    for (int i = 0; i < s->npeaks; i++)
        PUT("%s{\"hz\":%.1f,\"depth_pct\":%.2f}", i ? "," : "", s->peak_hz[i], 100 * s->peak_depth[i]);
    PUT("]},\"fm\":{\"rms_hz\":%.1f,\"pos_hz\":%.0f,\"neg_hz\":%.0f}", s->fm_rms_hz, s->fm_pos_hz, s->fm_neg_hz);
    if (r->o.spectrum_hz > 0 && r->psd.frames) {
        const double bh = r->ar / N;
        const int top = (int)fmin(N / 2, floor(r->o.spectrum_hz / bh));
        PUT(",\"spectrum\":{\"bin_hz\":%.4f,\"unit\":\"dB depth^2\",\"db\":[", bh);
        for (int k = 0; k <= top; k++)
            PUT("%s%.1f", k ? "," : "", 10 * log10(r->psd.acc[k] / r->psd.frames + 1e-12));
        PUT("]}");
    }
    PUT("}");
    return at;
}

size_t rdof_amrx_block_json(const rdof_amrx_block_t *b, char *out, size_t cap)
{
    size_t at = 0;
    char c[24], f[24];
    PUT("{\"t\":%.3f,\"carrier\":%s,\"power_dbfs\":%.1f,\"carrier_dbfs\":%s,\"cfo_hz\":%s,\"depth_pos_pct\":%.2f,"
        "\"depth_neg_pct\":%.2f,\"audio_rms_pct\":%.2f,\"fm_rms_hz\":%.1f}",
        b->t, b->carrier ? "true" : "false", b->power_dbfs, jf(c, sizeof c, b->carrier_dbfs, 2),
        jf(f, sizeof f, b->cfo_hz, 1), 100 * b->depth_pos, 100 * b->depth_neg, 100 * b->audio_rms, b->fm_rms_hz);
    return at;
}

#endif
