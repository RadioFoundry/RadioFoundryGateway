// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * APRS on receive: Bell 202 AFSK and AX.25 (rdof/sdr/afskrx.h). Written from the Bell 202 and AX.25 2.2
 * definitions.
 */
#include "rdof/sdr/afskrx.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BAUD 1200.0
#define MARK 1200.0
#define SPACE 2200.0
#define NDEC 7
#define NBITS 4096   /* each decoder's bits kept (3.4 s) */
#define NAUDIO 65536 /* audio samples kept (4.1 s at 16 kS/s; the rate's 4.1 s at most is asked) */
#define MAXF 400     /* a frame's octets at most (AX.25: 330 with the FCS) */
#define QF 8
#define RECENT 8
#define HBIN 5.0     /* Hz, the deviation's histogram */
#define HBINS 2400

static const float GAINS[NDEC] = {1.0f, 0.7f, 1.4f, 0.5f, 2.0f, 0.35f, 2.8f};

typedef struct {
    float g;
    float phase;              /* float: double is soft float on an ESP32 */
    bool prev, started;
    uint32_t *idx;            /* [NBITS]: each bit's center sample */
    uint8_t *tone;            /* [NBITS]: 1 mark */
    long k;                   /* tones so far */
    /* HDLC */
    unsigned sr;              /* the last 9 raw bits, the newest in bit 0 */
    long r;                   /* raw bits so far */
    long feed_from;           /* the first raw bit the frame collects (past its opening flag) */
    bool in_frame, bad;
    int ones, nb;
    uint8_t cur, buf[MAXF];
    int len;
    long open_r, last_flag_r; /* the opening flag's last bit; the last flag's */
    int nflags, open_nflags;
} dec_t;

struct rdof_afskrx {
    void (*dealloc)(void *);
    double fs;
    int nbit;
    float step;               /* bits a sample */
    int16_t *audio;           /* [NAUDIO]: Hz x 4 */
    uint32_t n;               /* samples in */
    /* the tone mixers and their one-bit means */
    float mr, mi, sr_, si;    /* the phasors */
    float mcr, mci, scr, sci; /* their steps */
    float *box;               /* [nbit][4]: the mixed products in the means */
    float sum[4];             /* float, made exact again every 1024 samples */
    int bi;
    dec_t d[NDEC];
    long fcs_bad;
    struct { uint16_t crc; double t0; } recent[RECENT];
    int nrecent;
    rdof_afsk_frame_t q[QF];
    int qh, qn;
    uint32_t hist[HBINS];
    float *amp, *fe;          /* [NBITS]: a measurement's scratch */
};

static uint16_t crc_x25(const uint8_t *p, int n)
{
    uint16_t c = 0xFFFF;
    for (int i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = c & 1 ? (uint16_t)((c >> 1) ^ 0x8408) : (uint16_t)(c >> 1);
    }
    return (uint16_t)(c ^ 0xFFFF);
}

rdof_afskrx_t *rdof_afskrx_new(double fs, void *(*alloc)(size_t), void (*dealloc)(void *))
{
    if (!(fs >= 8000 && fs <= 48000))
        return NULL;
    if (!alloc)
        alloc = malloc, dealloc = free;
    rdof_afskrx_t *a = alloc(sizeof *a);
    if (!a)
        return NULL;
    memset(a, 0, sizeof *a);
    a->dealloc = dealloc;
    a->fs = fs;
    a->nbit = (int)lround(fs / BAUD);
    a->step = (float)(BAUD / fs);
    a->audio = alloc(sizeof(int16_t) * NAUDIO);
    a->box = alloc(sizeof(float) * 4 * (size_t)a->nbit);
    a->amp = alloc(sizeof(float) * NBITS);
    a->fe = alloc(sizeof(float) * NBITS);
    bool ok = a->audio && a->box && a->amp && a->fe;
    for (int i = 0; i < NDEC && ok; i++) {
        a->d[i].g = GAINS[i];
        a->d[i].idx = alloc(sizeof(uint32_t) * NBITS);
        a->d[i].tone = alloc(NBITS);
        ok = a->d[i].idx && a->d[i].tone;
    }
    if (!ok) {
        rdof_afskrx_free(a);
        return NULL;
    }
    memset(a->box, 0, sizeof(float) * 4 * (size_t)a->nbit);
    a->mr = a->sr_ = 1;
    a->mcr = (float)cos(2 * M_PI * MARK / fs), a->mci = (float)-sin(2 * M_PI * MARK / fs);
    a->scr = (float)cos(2 * M_PI * SPACE / fs), a->sci = (float)-sin(2 * M_PI * SPACE / fs);
    return a;
}

void rdof_afskrx_free(rdof_afskrx_t *a)
{
    if (!a)
        return;
    void (*f)(void *) = a->dealloc ? a->dealloc : free;
    for (int i = 0; i < NDEC; i++)
        f(a->d[i].idx), f(a->d[i].tone);
    f(a->audio), f(a->box), f(a->amp), f(a->fe);
    f(a);
}

static float X(const rdof_afskrx_t *a, uint32_t i) { return a->audio[i % NAUDIO] * 0.25f; }

static int cmpf(const void *x, const void *y)
{
    const float u = *(const float *)x, v = *(const float *)y;
    return u < v ? -1 : u > v;
}

static float median(float *v, int n)
{
    if (!n)
        return NAN;
    qsort(v, (size_t)n, sizeof *v, cmpf);
    return n % 2 ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
}

/* A tone's one-bit mean centered on sample c, mixed with its absolute phase: its amplitude and phase */
static void tone_at(const rdof_afskrx_t *a, uint32_t c, double f, float *amp, float *ph)
{
    /* the phase in double once (the sample count is large), then a float phasor along the bit: double trig is soft
     * float on an ESP32 */
    const uint32_t s0 = c - (uint32_t)(a->nbit / 2);
    const float w0 = (float)(2 * M_PI * fmod(f * (double)s0 / a->fs, 1.0)), dw = (float)(2 * M_PI * f / a->fs);
    float pr = cosf(w0), pi = -sinf(w0);
    const float cr = cosf(dw), ci = -sinf(dw);
    float re = 0, im = 0;
    for (int j = 0; j < a->nbit; j++) {
        const float x = X(a, s0 + (uint32_t)j);
        re += x * pr, im += x * pi;
        const float t = pr * cr - pi * ci;
        pi = pr * ci + pi * cr, pr = t;
    }
    re /= a->nbit, im /= a->nbit;
    *amp = 2 * sqrtf(re * re + im * im);
    *ph = atan2f(im, re);
}

static float wrapf(float x)
{
    while (x > (float)M_PI)
        x -= (float)(2 * M_PI);
    while (x < (float)-M_PI)
        x += (float)(2 * M_PI);
    return x;
}

/* One tone's level and frequency from its runs: tone t, bits k0..k1 of decoder d */
static void tone_runs(rdof_afskrx_t *a, const dec_t *d, long k0, long k1, int t, float *dev, float *hz)
{
    const double f = t ? MARK : SPACE;
    int na = 0, nf = 0;
    float prev_ph = 0;
    long prev_k = -10;
    uint32_t prev_c = 0;
    for (long k = k0 + 1; k < k1 - 1 && na < NBITS; k++) {
        const uint8_t a0 = d->tone[(k - 1) % NBITS], a1 = d->tone[k % NBITS], a2 = d->tone[(k + 1) % NBITS];
        if (!(a0 == a1 && a1 == a2 && a1 == t))
            continue;
        const uint32_t c = d->idx[k % NBITS];
        float amp, ph;
        tone_at(a, c, f, &amp, &ph);
        a->amp[na++] = amp;
        if (prev_k == k - 1 && c > prev_c && nf < NBITS) /* the phase's drift from the bit before: the tone's offset */
            a->fe[nf++] = (float)(wrapf(ph - prev_ph) / (2 * M_PI) * a->fs / (double)(c - prev_c));
        prev_ph = ph, prev_k = k, prev_c = c;
    }
    *dev = median(a->amp, na);
    *hz = nf ? (float)f + median(a->fe, nf) : NAN;
}

static void put_call(const uint8_t *p, char *out)
{
    int k = 0;
    for (int i = 0; i < 6; i++) {
        const char ch = (char)(p[i] >> 1);
        if (ch != ' ')
            out[k++] = ch;
    }
    const int ssid = (p[6] >> 1) & 0x0F;
    if (ssid)
        k += snprintf(out + k, 4, "-%d", ssid);
    out[k] = 0;
}

static bool parse(const uint8_t *b, int n, rdof_afsk_frame_t *f)
{
    int i = 0, na = 0;
    size_t dk = 0;
    for (;;) {
        if (i + 7 > n || na > 10)
            return false;
        char call[12];
        put_call(b + i, call);
        if (na == 0)
            strcpy(f->dst, call);
        else if (na == 1)
            strcpy(f->src, call);
        else if (dk + strlen(call) + 3 < sizeof f->digis)
            dk += (size_t)snprintf(f->digis + dk, sizeof f->digis - dk, "%s%s%s", na > 2 ? "," : "", call,
                                   b[i + 6] & 0x80 ? "*" : "");
        na++;
        const bool last = b[i + 6] & 1;
        i += 7;
        if (last)
            break;
    }
    if (na < 2 || i + 2 > n)
        return false;
    f->control = b[i], f->pid = b[i + 1];
    f->info_len = n - i - 2;
    const int m = f->info_len < RDOF_AFSK_INFO - 1 ? f->info_len : RDOF_AFSK_INFO - 1;
    memcpy(f->info, b + i + 2, (size_t)m);
    f->info[m] = 0;
    return true;
}

/* A frame decoder d found: raw bits a (its first) to b (the closing flag's first), measured and queued */
static void frame(rdof_afskrx_t *a, dec_t *d, long ra, long rb)
{
    const int n = d->len;
    const uint16_t fcs = (uint16_t)(d->buf[n - 2] | d->buf[n - 1] << 8);
    const uint16_t crc = crc_x25(d->buf, n - 2);
    if (crc != fcs) {
        if (d == &a->d[0])
            a->fcs_bad++;
        return;
    }
    /* raw bit r lies between tones r and r + 1 */
    if (d->k - (ra + 1) >= NBITS)
        return; /* longer than the bits kept */
    const double t0 = d->idx[(ra + 1) % NBITS] / a->fs, t1 = d->idx[(rb + 1) % NBITS] / a->fs;
    for (int i = 0; i < a->nrecent; i++) /* another weight's decoder had it */
        if (a->recent[i].crc == crc && fabs(a->recent[i].t0 - t0) < 0.05)
            return;
    rdof_afsk_frame_t f;
    memset(&f, 0, sizeof f);
    if (!parse(d->buf, n - 2, &f))
        return;
    if (a->nrecent == RECENT) {
        memmove(a->recent, a->recent + 1, sizeof a->recent[0] * (RECENT - 1));
        a->nrecent--;
    }
    a->recent[a->nrecent].crc = crc, a->recent[a->nrecent].t0 = t0;
    a->nrecent++;
    f.t0 = t0, f.t1 = t1, f.txdelay_flags = d->open_nflags, f.decision_gain = d->g;
    /* the transmission: its flags before to a flag after */
    long k0 = ra - 8L * d->open_nflags, k1 = rb + 9;
    if (k0 < 1)
        k0 = 1;
    if (k0 <= d->k - NBITS)
        k0 = d->k - NBITS + 1;
    if (k1 > d->k - 1)
        k1 = d->k - 1;
    const uint32_t s0 = d->idx[k0 % NBITS], s1 = d->idx[k1 % NBITS];
    f.peak_dev_hz = f.max_dev_hz = f.rms_dev_hz = NAN;
    f.mark_dev_hz = f.space_dev_hz = f.twist_db = f.mark_hz = f.space_hz = NAN;
    if (s1 > s0 && a->n - s0 < NAUDIO - (uint32_t)a->nbit) {
        memset(a->hist, 0, sizeof a->hist);
        double sum = 0, sum2 = 0, mx = 0;
        for (uint32_t i = s0; i < s1; i++) {
            const double x = X(a, i), ax = fabs(x);
            sum += x, sum2 += x * x;
            mx = ax > mx ? ax : mx;
            const int h = (int)(ax / HBIN);
            a->hist[h < HBINS ? h : HBINS - 1]++;
        }
        const uint32_t cnt = s1 - s0;
        uint32_t acc = 0;
        int h = 0;
        while (h < HBINS - 1 && (acc += a->hist[h]) < (uint32_t)(0.999 * cnt))
            h++;
        const double mean = sum / cnt;
        f.peak_dev_hz = (float)((h + 0.5) * HBIN), f.max_dev_hz = (float)mx;
        f.rms_dev_hz = (float)sqrt(fmax(sum2 / cnt - mean * mean, 0));
        tone_runs(a, d, k0, k1, 1, &f.mark_dev_hz, &f.mark_hz);
        tone_runs(a, d, k0, k1, 0, &f.space_dev_hz, &f.space_hz);
        if (isfinite(f.mark_dev_hz) && isfinite(f.space_dev_hz) && f.space_dev_hz > 0)
            f.twist_db = 20 * log10f(f.mark_dev_hz / f.space_dev_hz);
    }
    if (a->qn == QF)
        a->qh = (a->qh + 1) % QF, a->qn--;
    a->q[(a->qh + a->qn++) % QF] = f;
}

static void collect(dec_t *d, int bit)
{
    if (d->bad)
        return;
    if (bit) {
        if (++d->ones > 6) { /* an abort */
            d->bad = true;
            return;
        }
    } else {
        if (d->ones == 5) { /* the stuffed 0 */
            d->ones = 0;
            return;
        }
        if (d->ones == 6) {
            d->bad = true;
            return;
        }
        d->ones = 0;
    }
    d->cur |= (uint8_t)(bit << d->nb);
    if (++d->nb == 8) {
        if (d->len == MAXF) {
            d->bad = true;
            return;
        }
        d->buf[d->len++] = d->cur;
        d->cur = 0, d->nb = 0;
    }
}

/* Raw (NRZI-decoded) bit number d->r */
static void raw_bit(rdof_afskrx_t *a, dec_t *d, int bit)
{
    const long r = d->r++;
    d->sr = ((d->sr << 1) | (unsigned)bit) & 0x1FF;
    /* the bit 8 back can't be in a flag ending here: into the frame */
    if (d->in_frame && r - 8 >= d->feed_from)
        collect(d, (d->sr >> 8) & 1);
    if ((d->sr & 0xFF) != 0x7E)
        return;
    /* a flag, its last bit r */
    if (d->in_frame && !d->bad && d->nb == 0 && d->len >= 18 && r - 7 - (d->open_r + 1) >= 18 * 8)
        frame(a, d, d->open_r + 1, r - 7);
    d->nflags = d->last_flag_r == r - 8 ? d->nflags + 1 : 1;
    d->last_flag_r = r;
    d->in_frame = true, d->bad = false;
    d->ones = 0, d->nb = 0, d->cur = 0, d->len = 0;
    d->open_r = r, d->open_nflags = d->nflags;
    d->feed_from = r + 1;
}

static void tone_in(rdof_afskrx_t *a, dec_t *d, uint32_t c, bool mark)
{
    const long k = d->k++;
    d->idx[k % NBITS] = c;
    d->tone[k % NBITS] = mark;
    if (k >= 1)
        raw_bit(a, d, d->tone[(k - 1) % NBITS] == mark);
}

void rdof_afskrx_feed(rdof_afskrx_t *a, const float *x, int n)
{
    if (!a)
        return;
    const int nb = a->nbit;
    for (int i = 0; i < n; i++) {
        const float v = x[i];
        const float q = v * 4;
        a->audio[a->n % NAUDIO] = (int16_t)(q > 32767 ? 32767 : q < -32768 ? -32768 : lrintf(q));
        /* the mixers: v times each tone's phasor, into their one-bit means */
        const float p[4] = {v * a->mr, v * a->mi, v * a->sr_, v * a->si};
        float *b = a->box + 4 * a->bi;
        for (int k = 0; k < 4; k++)
            a->sum[k] += p[k] - b[k], b[k] = p[k];
        if (++a->bi == nb)
            a->bi = 0;
        float t = a->mr * a->mcr - a->mi * a->mci;
        a->mi = a->mr * a->mci + a->mi * a->mcr, a->mr = t;
        t = a->sr_ * a->scr - a->si * a->sci;
        a->si = a->sr_ * a->sci + a->si * a->scr, a->sr_ = t;
        if ((a->n & 1023) == 1023) { /* the phasors back on the unit circle, the sums exact */
            const float gm = 1 / sqrtf(a->mr * a->mr + a->mi * a->mi), gs = 1 / sqrtf(a->sr_ * a->sr_ + a->si * a->si);
            a->mr *= gm, a->mi *= gm, a->sr_ *= gs, a->si *= gs;
            for (int k = 0; k < 4; k++) {
                float s = 0;
                for (int j = 0; j < nb; j++)
                    s += a->box[4 * j + k];
                a->sum[k] = s;
            }
        }
        const uint32_t c = a->n - (uint32_t)(nb / 2); /* the means' center */
        a->n++;
        if (a->n < (uint32_t)nb)
            continue;
        const float m = 2 * sqrtf(a->sum[0] * a->sum[0] + a->sum[1] * a->sum[1]) / nb;
        const float s = 2 * sqrtf(a->sum[2] * a->sum[2] + a->sum[3] * a->sum[3]) / nb;
        for (int k = 0; k < NDEC; k++) {
            dec_t *d = &a->d[k];
            const bool h = m - d->g * s > 0;
            if (!d->started)
                d->started = true, d->prev = h;
            if (h != d->prev) { /* expected half way between two samplings */
                d->phase += (0.5f - d->phase) * 0.25f;
                d->prev = h;
            }
            d->phase += a->step;
            if (d->phase >= 1.0f) {
                d->phase -= 1.0f;
                tone_in(a, d, c, h);
            }
        }
    }
}

bool rdof_afskrx_take(rdof_afskrx_t *a, rdof_afsk_frame_t *out)
{
    if (!a || !a->qn)
        return false;
    *out = a->q[a->qh];
    a->qh = (a->qh + 1) % QF, a->qn--;
    return true;
}

long rdof_afskrx_fcs_bad(const rdof_afskrx_t *a) { return a ? a->fcs_bad : 0; }
