// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* An analog FM test signal (rdof/fmgen.h). */
#include "rdof/fmgen.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

const uint16_t rdof_dcs_codes[] = {
    0023, 0025, 0026, 0031, 0032, 0036, 0043, 0047, 0051, 0053, 0054, 0065, 0071, 0072, 0073, 0074, 0114, 0115,
    0116, 0122, 0125, 0131, 0132, 0134, 0143, 0145, 0152, 0155, 0156, 0162, 0165, 0172, 0174, 0205, 0212, 0223,
    0225, 0226, 0243, 0244, 0245, 0246, 0251, 0252, 0255, 0261, 0263, 0265, 0266, 0271, 0274, 0306, 0311, 0315,
    0325, 0331, 0332, 0343, 0346, 0351, 0356, 0364, 0365, 0371, 0411, 0412, 0413, 0423, 0431, 0432, 0445, 0446,
    0452, 0454, 0455, 0462, 0464, 0465, 0466, 0503, 0506, 0516, 0523, 0526, 0532, 0546, 0565, 0606, 0612, 0624,
    0627, 0631, 0632, 0654, 0662, 0664, 0703, 0712, 0723, 0731, 0732, 0734, 0743, 0754};
const int rdof_dcs_ncodes = (int)(sizeof rdof_dcs_codes / sizeof rdof_dcs_codes[0]);

const float rdof_ctcss_tones[] = {
    67.0f,  69.3f,  71.9f,  74.4f,  77.0f,  79.7f,  82.5f,  85.4f,  88.5f,  91.5f,  94.8f,  97.4f,  100.0f,
    103.5f, 107.2f, 110.9f, 114.8f, 118.8f, 123.0f, 127.3f, 131.8f, 136.5f, 141.3f, 144.95f, 150.0f, 151.4f,
    156.7f, 159.8f, 162.2f, 165.5f, 167.9f, 171.3f, 173.8f, 177.3f, 179.9f, 183.5f, 186.2f, 189.9f, 192.8f,
    196.6f, 199.5f, 203.5f, 206.5f, 210.7f, 218.1f, 225.7f, 229.1f, 233.6f, 241.8f, 250.3f, 254.1f};
const int rdof_ctcss_ntones = (int)(sizeof rdof_ctcss_tones / sizeof rdof_ctcss_tones[0]);

static float sin_tab[1025];
static atomic_bool sin_ready;

float rdof_fm_sin(uint32_t phase)
{
    if (!atomic_load_explicit(&sin_ready, memory_order_acquire)) { /* two threads may both fill it: the same values */
        for (int i = 0; i <= 1024; i++)
            sin_tab[i] = (float)sin(2 * M_PI * i / 1024.0);
        atomic_store_explicit(&sin_ready, true, memory_order_release);
    }
    const uint32_t i = phase >> 22;
    const float f = (float)(phase & 0x3FFFFF) * (1.0f / 4194304.0f);
    return sin_tab[i] + (sin_tab[i + 1] - sin_tab[i]) * f;
}

uint32_t rdof_dcs_word(unsigned code)
{
    /* the 12 data bits: the code, then 100 (bit 11); the check bits by division by 0xC75, LSB first */
    const uint32_t data = (code & 0x1FF) | 0x800;
    uint32_t cw = data;
    for (int i = 0; i < 12; i++) {
        if (cw & 1)
            cw ^= 0xC75;
        cw >>= 1;
    }
    return (cw << 12) | data;
}

bool rdof_dtmf_tones(char digit, float *lo, float *hi)
{
    static const char keys[] = "123A456B789C*0#D";
    static const float row[] = {697, 770, 852, 941}, col[] = {1209, 1336, 1477, 1633};
    const char *p = digit ? strchr(keys, digit >= 'a' && digit <= 'd' ? digit - 32 : digit) : NULL;
    if (!p)
        return false;
    *lo = row[(p - keys) / 4];
    *hi = col[(p - keys) % 4];
    return true;
}

static uint32_t inc_of(double hz, double fs) { return (uint32_t)(int64_t)llround(hz / fs * 4294967296.0); }

/* A segment's length, seconds. */
static double seg_seconds(const rdof_fmg_cfg_t *c, const rdof_fmg_seg_t *s)
{
    switch (s->kind) {
    case RDOF_FMG_DTMF:
        return s->n * ((double)s->seconds + s->gap_s);
    case RDOF_FMG_FSK:
        return s->baud > 0 ? s->n / (double)s->baud : 0;
    case RDOF_FMG_PCM:
        return c->pcm_rate > 0 ? c->pcm_samples / (double)c->pcm_rate : 0;
    default:
        return s->seconds;
    }
}

static double program_seconds(const rdof_fmg_cfg_t *c)
{
    double t = 0;
    for (int i = 0; i < c->nseg; i++)
        t += seg_seconds(c, &c->seg[i]);
    return t;
}

const char *rdof_fmg_check(const rdof_fmg_cfg_t *c)
{
    if (!(c->dev_hz >= 0 && c->dev_hz <= 8000))
        return "the deviation is 0-8000 Hz";
    if (!(c->sub_dev_hz >= 0 && c->sub_dev_hz <= 2000))
        return "the subaudio's deviation is 0-2000 Hz";
    if (!(c->preemph_us >= 0 && c->preemph_us <= 2000))
        return "the pre-emphasis is 0-2000 us";
    if (c->sub == RDOF_FMG_SUB_CTCSS && !(c->ctcss_hz >= 30 && c->ctcss_hz <= 300))
        return "a CTCSS tone is 30-300 Hz";
    if (c->sub == RDOF_FMG_SUB_DCS && c->dcs > 0777)
        return "a DCS code is three octal digits";
    if (c->sub > RDOF_FMG_SUB_DCS)
        return "no such subaudio";
    if (!(c->lead_s >= 0 && c->lead_s <= 60 && c->tail_s >= 0 && c->tail_s <= 60 && c->sub_end_s >= 0 &&
          c->sub_end_s <= 2))
        return "lead and tail are 0-60 s, the subaudio's end 0-2 s";
    if (c->nseg > RDOF_FMG_SEGS)
        return "too many segments";
    int pcm = 0;
    for (int i = 0; i < c->nseg; i++) {
        const rdof_fmg_seg_t *s = &c->seg[i];
        const float lmax = s->kind == RDOF_FMG_NOISE ? 0.5f : 1.0f;
        if (!(s->level >= 0 && s->level <= lmax))
            return s->kind == RDOF_FMG_NOISE ? "a noise level (rms) is 0-0.5" : "a level is 0-1";
        if (!(s->seconds >= 0 && s->seconds <= 600 && s->gap_s >= 0 && s->gap_s <= 10))
            return "a segment is 0-600 s, a DTMF gap 0-10 s";
        for (int k = 0; k < RDOF_FMG_TONES; k++)
            if (!(s->hz[k] >= 0 && s->hz[k] <= 20000))
                return "a tone is 0-20000 Hz";
        switch (s->kind) {
        case RDOF_FMG_SILENCE:
        case RDOF_FMG_NOISE:
            break;
        case RDOF_FMG_TONE:
            if (s->ntones < 1 || s->ntones > RDOF_FMG_TONES)
                return "a tone segment has 1-4 tones";
            break;
        case RDOF_FMG_SWEEP:
            if (!(s->hz[0] > 0 && s->hz[1] > 0) || !(s->seconds > 0))
                return "a sweep needs two frequencies over 0 Hz and a length";
            break;
        case RDOF_FMG_DTMF:
            if (!s->n || s->off + s->n > RDOF_FMG_DATA || !(s->seconds > 0))
                return "DTMF needs digits and a tone length";
            for (uint32_t k = 0; k < s->n; k++) {
                float lo, hi;
                if (!rdof_dtmf_tones((char)c->data[s->off + k], &lo, &hi))
                    return "a DTMF digit is 0-9, *, # or A-D";
            }
            break;
        case RDOF_FMG_FSK:
            if (!(s->baud >= 1 && s->baud <= 20000) || !(s->hz[0] > 0 && s->hz[1] > 0))
                return "FSK needs a baud rate of 1-20000 and mark and space tones";
            if (!s->n || s->off + (s->n + 7) / 8 > RDOF_FMG_DATA)
                return "FSK needs bits, within the program's data";
            break;
        case RDOF_FMG_PCM:
            pcm++;
            if (!(c->pcm_rate >= 4000 && c->pcm_rate <= 96000))
                return "PCM is 4000-96000 samples a second";
            break;
        default:
            return "no such segment";
        }
    }
    if (pcm > 1 || (pcm && c->repeat > 1))
        return "the PCM stream plays once: one PCM segment, no repeat";
    const double t = rdof_fmg_seconds(c);
    if (!(t > RDOF_FMG_RAMP_S) || t > 3600)
        return "the transmission is 0-3600 s, with something in it";
    return NULL;
}

double rdof_fmg_seconds(const rdof_fmg_cfg_t *c)
{
    return c->lead_s + program_seconds(c) * (c->repeat > 1 ? c->repeat : 1) + c->tail_s + c->sub_end_s +
           RDOF_FMG_RAMP_S;
}

static double preemph_gain(const rdof_fmg_cfg_t *c, double hz)
{
    if (!(c->preemph_us > 0))
        return 1;
    const double w = 2 * M_PI * c->preemph_us * 1e-6;
    return sqrt(1 + (w * hz) * (w * hz)) / sqrt(1 + (w * 1000) * (w * 1000));
}

double rdof_fmg_peak_hz(const rdof_fmg_cfg_t *c)
{
    double peak = 0;
    for (int i = 0; i < c->nseg; i++) {
        const rdof_fmg_seg_t *s = &c->seg[i];
        double top = 0, lvl = s->level;
        switch (s->kind) {
        case RDOF_FMG_TONE:
        case RDOF_FMG_SWEEP:
        case RDOF_FMG_DTMF:
        case RDOF_FMG_FSK:
            top = s->kind == RDOF_FMG_DTMF ? 1633 : 0;
            for (int k = 0; k < RDOF_FMG_TONES; k++)
                top = s->hz[k] > top ? s->hz[k] : top;
            break;
        case RDOF_FMG_NOISE:
            lvl *= 4; /* rms to a peak it seldom passes */
            top = 0;  /* white: pre-emphasis isn't applied to the peak's estimate */
            break;
        case RDOF_FMG_PCM:
            top = c->pcm_rate / 2;
            break;
        default:
            break;
        }
        const double p = lvl * preemph_gain(c, top);
        peak = p > peak ? p : peak;
    }
    return peak * c->dev_hz + (c->sub ? c->sub_dev_hz : 0);
}

int rdof_fmg_init(rdof_fmg_t *g, const rdof_fmg_cfg_t *c, double fs, int16_t *ring, uint32_t ring_samples)
{
    memset(g, 0, sizeof *g);
    if (rdof_fmg_check(c) || !(fs >= 8000))
        return -1;
    bool pcm = false;
    for (int i = 0; i < c->nseg; i++)
        pcm |= c->seg[i].kind == RDOF_FMG_PCM;
    if (pcm && (!ring || ring_samples < 1024 || (ring_samples & (ring_samples - 1))))
        return -1;
    g->c = *c;
    g->fs = fs;
    g->ring = pcm ? ring : NULL;
    g->cap = pcm ? ring_samples : 0;
    atomic_init(&g->w, 0);
    atomic_init(&g->r, 0);
    atomic_init(&g->eof, false);
    g->ramp = (uint64_t)llround(RDOF_FMG_RAMP_S * fs);
    g->lead = (uint64_t)llround(c->lead_s * fs);
    g->prog = 0;
    for (int i = 0; i < c->nseg; i++)
        g->prog += (uint64_t)llround(seg_seconds(c, &c->seg[i]) * fs);
    g->tail = (uint64_t)llround(c->tail_s * fs);
    g->end = (uint64_t)llround(c->sub_end_s * fs);
    g->total = g->lead + g->prog * (c->repeat > 1 ? c->repeat : 1) + g->tail + g->end + g->ramp;
    g->seg = -1;
    g->rng = c->seed ? c->seed : 0x2545F491u;
    g->pre_k = c->preemph_us * 1e-6 * fs;
    g->pre_g = preemph_gain(c, 0); /* 1 / |H(1 kHz)|: 0 dB at 1 kHz */
    g->dcs_word = rdof_dcs_word(c->dcs);
    if (c->dcs_inv)
        g->dcs_word = ~g->dcs_word & 0x7FFFFF;
    g->lp_a = (float)(1 - exp(-2 * M_PI * 300.0 / fs)); /* DCS: two poles at 300 Hz, so its edges don't splatter */
    return 0;
}

bool rdof_fmg_done(const rdof_fmg_t *g) { return g->done; }

uint32_t rdof_fmg_pcm_space(rdof_fmg_t *g)
{
    if (!g->ring)
        return 0;
    const uint32_t w = (uint32_t)atomic_load_explicit(&g->w, memory_order_relaxed);
    const uint32_t r = (uint32_t)atomic_load_explicit(&g->r, memory_order_acquire);
    return g->cap - (w - r);
}

uint32_t rdof_fmg_pcm_push(rdof_fmg_t *g, const int16_t *s, uint32_t n)
{
    const uint32_t room = rdof_fmg_pcm_space(g);
    if (n > room)
        n = room;
    uint32_t w = (uint32_t)atomic_load_explicit(&g->w, memory_order_relaxed);
    for (uint32_t i = 0; i < n; i++)
        g->ring[(w + i) & (g->cap - 1)] = s[i];
    atomic_store_explicit(&g->w, w + n, memory_order_release);
    return n;
}

void rdof_fmg_pcm_end(rdof_fmg_t *g) { atomic_store_explicit(&g->eof, true, memory_order_release); }

/* The next PCM sample from the ring; silence (an underrun) when it isn't there yet. */
static int16_t pcm_pop(rdof_fmg_t *g)
{
    const uint32_t r = (uint32_t)atomic_load_explicit(&g->r, memory_order_relaxed);
    const uint32_t w = (uint32_t)atomic_load_explicit(&g->w, memory_order_acquire);
    g->pcm_played++;
    if (r == w) {
        g->underruns++;
        return 0;
    }
    const int16_t v = g->ring[r & (g->cap - 1)];
    atomic_store_explicit(&g->r, r + 1, memory_order_release);
    return v;
}

static float noise(rdof_fmg_t *g)
{
    float s = 0;
    for (int k = 0; k < 4; k++) { /* four uniforms: close enough to Gaussian, variance 1 once scaled */
        g->rng ^= g->rng << 13, g->rng ^= g->rng >> 17, g->rng ^= g->rng << 5;
        s += (float)(g->rng >> 8) * (1.0f / 16777216.0f);
    }
    return (s - 2.0f) * 1.7320508f;
}

/* Into segment i (or past the last: -1), from sample `at`. */
static void seg_start(rdof_fmg_t *g, int i, uint64_t at)
{
    g->seg = i;
    g->seg_at = at;
    if (i < 0)
        return;
    const rdof_fmg_seg_t *s = &g->c.seg[i];
    g->seg_len = (uint64_t)llround(seg_seconds(&g->c, s) * g->fs);
    memset(g->ph, 0, sizeof g->ph);
    g->sweep_ph = 0;
    g->fsk_bit = UINT32_MAX;
    g->fsk_tone = true;
    g->pcm_t = 0;
    g->pcm_prev = g->pcm_cur = 0;
}

/* The program's value at its sample k of segment s (k from the segment's start), -1..1 times the level. */
static float seg_value(rdof_fmg_t *g, const rdof_fmg_seg_t *s, uint64_t k)
{
    const double fs = g->fs;
    switch (s->kind) {
    case RDOF_FMG_TONE: {
        float v = 0;
        for (int t = 0; t < s->ntones; t++) {
            v += rdof_fm_sin(g->ph[t]);
            g->ph[t] += inc_of(s->hz[t], fs);
        }
        return v * s->level / s->ntones;
    }
    case RDOF_FMG_SWEEP: {
        const double x = (double)k / (double)(g->seg_len ? g->seg_len : 1);
        const double f = s->log ? s->hz[0] * pow((double)s->hz[1] / s->hz[0], x) : s->hz[0] + (s->hz[1] - s->hz[0]) * x;
        const float v = (float)sin(g->sweep_ph);
        g->sweep_ph = fmod(g->sweep_ph + 2 * M_PI * f / fs, 2 * M_PI);
        return v * s->level;
    }
    case RDOF_FMG_DTMF: {
        const uint64_t on = (uint64_t)llround(s->seconds * fs), per = on + (uint64_t)llround(s->gap_s * fs);
        const uint64_t d = per ? k / per : 0;
        if (d >= s->n || k % per >= on)
            return 0;
        float lo, hi;
        rdof_dtmf_tones((char)g->c.data[s->off + d], &lo, &hi);
        const float v = rdof_fm_sin(g->ph[0]) + rdof_fm_sin(g->ph[1]);
        g->ph[0] += inc_of(lo, fs), g->ph[1] += inc_of(hi, fs);
        return v * s->level * 0.5f;
    }
    case RDOF_FMG_FSK: {
        uint32_t b = (uint32_t)((double)k * s->baud / fs);
        if (b >= s->n)
            b = s->n - 1;
        if (b != g->fsk_bit) {
            g->fsk_bit = b;
            const uint32_t at = b / 8, sh = s->lsb ? b % 8 : 7 - b % 8;
            const bool bit = (g->c.data[s->off + at] >> sh) & 1;
            g->fsk_tone = s->nrzi ? (bit ? g->fsk_tone : !g->fsk_tone) : bit;
        }
        const float v = rdof_fm_sin(g->ph[0]);
        g->ph[0] += inc_of(g->fsk_tone ? s->hz[0] : s->hz[1], fs); /* continuous phase */
        return v * s->level;
    }
    case RDOF_FMG_NOISE:
        return noise(g) * s->level;
    case RDOF_FMG_PCM: {
        g->pcm_t += g->c.pcm_rate / fs;
        while (g->pcm_t >= 1.0) { /* linear between the last two samples */
            g->pcm_t -= 1.0;
            g->pcm_prev = g->pcm_cur;
            g->pcm_cur = g->pcm_played < g->c.pcm_samples ? pcm_pop(g) : 0;
        }
        const float v = (float)g->pcm_prev + (float)(g->pcm_cur - g->pcm_prev) * (float)g->pcm_t;
        return v * (1.0f / 32768.0f) * s->level;
    }
    default:
        return 0;
    }
}

int rdof_fmg_render(rdof_fmg_t *g, float *dev_hz, float *env, int n)
{
    const rdof_fmg_cfg_t *c = &g->c;
    const uint64_t prog_end = g->lead + g->prog * (c->repeat > 1 ? c->repeat : 1);
    const uint64_t audio_end = prog_end + g->tail, sub_end = audio_end + g->end;
    const uint32_t ctcss = inc_of(c->ctcss_hz, g->fs), off = inc_of(RDOF_FMG_DCS_OFF_HZ, g->fs);
    int keyed = 0;
    for (int i = 0; i < n; i++, g->pos++) {
        const uint64_t p = g->pos;
        if (p >= g->total) {
            dev_hz[i] = 0;
            if (env)
                env[i] = 0;
            g->done = true;
            continue;
        }
        keyed++;
        /* the envelope */
        float e = 1;
        if (p < g->ramp)
            e = 0.5f - 0.5f * cosf((float)M_PI * (float)(p + 1) / (float)(g->ramp + 1));
        else if (p >= g->total - g->ramp)
            e = 0.5f - 0.5f * cosf((float)M_PI * (float)(g->total - p) / (float)(g->ramp + 1));
        if (env)
            env[i] = e;
        /* the program */
        float a = 0;
        if (p >= g->lead && p < prog_end && g->prog) {
            const uint64_t q = (p - g->lead) % g->prog;
            if (q == 0 || g->seg < 0) {
                g->seg = -1;
                uint64_t at = 0;
                int k = 0;
                while (k < c->nseg && at + (uint64_t)llround(seg_seconds(c, &c->seg[k]) * g->fs) <= q)
                    at += (uint64_t)llround(seg_seconds(c, &c->seg[k++]) * g->fs);
                seg_start(g, k < c->nseg ? k : -1, at);
            }
            while (g->seg >= 0 && q >= g->seg_at + g->seg_len)
                seg_start(g, g->seg + 1 < c->nseg ? g->seg + 1 : -1, g->seg_at + g->seg_len);
            if (g->seg >= 0)
                a = seg_value(g, &c->seg[g->seg], q - g->seg_at);
        }
        if (g->pre_k > 0) {
            const float y = (float)g->pre_g * (a + (float)g->pre_k * (a - g->pre_x1));
            g->pre_x1 = a;
            a = y;
        }
        float dev = a * c->dev_hz;
        /* the subaudio */
        if (c->sub == RDOF_FMG_SUB_CTCSS) {
            const float s = rdof_fm_sin(g->sub_ph);
            g->sub_ph += ctcss;
            dev += (p >= audio_end && p < sub_end ? -s : s) * c->sub_dev_hz; /* the reverse burst: 180 degrees */
        } else if (c->sub == RDOF_FMG_SUB_DCS) {
            float s;
            if (p >= audio_end && p < sub_end) {
                s = rdof_fm_sin(g->sub_ph);
                g->sub_ph += off;
            } else {
                const uint32_t b = (uint32_t)((uint64_t)g->dcs_t % 23);
                g->dcs_t += RDOF_FMG_DCS_BAUD / g->fs;
                if (g->dcs_t >= 23 * 1e6)
                    g->dcs_t = fmod(g->dcs_t, 23);
                const float x = (g->dcs_word >> b) & 1 ? 1.0f : -1.0f;
                g->lp[0] += g->lp_a * (x - g->lp[0]);
                g->lp[1] += g->lp_a * (g->lp[0] - g->lp[1]);
                s = g->lp[1];
            }
            dev += s * c->sub_dev_hz;
        }
        dev_hz[i] = dev;
    }
    return keyed;
}
