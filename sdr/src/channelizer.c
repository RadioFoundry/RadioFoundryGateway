// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Overlap-save channelizer: one block at a time, NCO recomputed per block.
 */
#include "rdof/sdr/channelizer.h"

#if RDOF_FEATURE_SDR

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sdr_private.h"

/* Modified Bessel function of the first kind, order 0 (power series; x <= ~20 here). */
static double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0, q = x * x / 4.0;
    for (int k = 1; k < 200; k++) {
        term *= q / ((double)k * (double)k);
        sum += term;
        if (term < sum * 1e-17)
            break;
    }
    return sum;
}

void rdof_chz_lowpass(int num_taps, double cutoff_hz, double fs, double *out)
{
    const double beta = 5.0, alpha = (num_taps - 1) / 2.0, i0b = bessel_i0(beta);
    double sum = 0.0;
    for (int i = 0; i < num_taps; i++) {
        double n = i - alpha;
        double x = 2.0 * cutoff_hz / fs * n;
        double y = RDOF_PI * (x == 0.0 ? 1.0e-20 : x);
        double sinc = sin(y) / y;
        double r = num_taps > 1 ? (i - alpha) / alpha : 0.0;
        double w = num_taps > 1 ? bessel_i0(beta * sqrt(fmax(0.0, 1.0 - r * r))) / i0b : 1.0;
        out[i] = sinc * w;
        sum += out[i];
    }
    for (int i = 0; i < num_taps; i++)
        out[i] /= sum;
}

void rdof_chz_free(rdof_chz_t *s)
{
    rdof_fft_free(s->fwd);
    rdof_fft_free(s->inv);
    rdof_sdr_free(s->k);
    rdof_sdr_free(s->residual_hz);
    rdof_sdr_free(s->correction_hz);
    rdof_sdr_free(s->phase);
    rdof_sdr_free(s->step_cps);
    rdof_sdr_free(s->step_rad);
    rdof_sdr_free(s->corr_used);
    rdof_sdr_free(s->block_rad);
    rdof_sdr_free(s->stepr);
    rdof_sdr_free(s->H);
    rdof_sdr_free(s->win);
    rdof_sdr_free(s->X);
    rdof_sdr_free(s->Y);
    rdof_sdr_free(s->y);
    rdof_sdr_free(s->twiddle);
    rdof_sdr_free(s->level);
    rdof_fft_free(s->inv2);
    rdof_sdr_free(s->Y2);
    rdof_sdr_free(s->y2);
    rdof_sdr_free(s->Xs);
    rdof_sdr_free(s->xs_boff);
    memset(s, 0, sizeof *s);
}

int rdof_chz_init(rdof_chz_t *s, double sample_rate, const double *offsets_hz, int channels, int decimation,
                int out_fft, double cutoff_hz)
{
    memset(s, 0, sizeof *s);
    if (decimation < 1 || out_fft < 4 || out_fft % 2 || channels < 1)
        return -1;
    s->fs = sample_rate;
    s->D = decimation;
    s->M = out_fft;
    s->N = s->D * s->M;
    s->hop = s->N * 3 / 4;
    s->C = channels;
    s->out_rate = s->fs / s->D;
    if (s->hop % s->D)
        return -1;
    s->discard = (s->N - s->hop) / s->D;
    s->out_per_hop = s->M - s->discard;
    const int N = s->N, M = s->M, C = s->C;
    s->k = rdof_sdr_alloc(sizeof(int) * C);
    s->residual_hz = rdof_sdr_alloc(sizeof(double) * C);
    s->correction_hz = rdof_sdr_zalloc(sizeof(double) * (size_t)(C));
    s->phase = rdof_sdr_zalloc(sizeof(double) * (size_t)(C));
    s->step_cps = rdof_sdr_alloc(sizeof(double) * C);
    s->step_rad = rdof_sdr_zalloc(sizeof(double) * (size_t)(C));
    s->corr_used = rdof_sdr_alloc(sizeof(double) * (size_t)C);
    s->block_rad = rdof_sdr_zalloc(sizeof(double) * (size_t)C);
    s->stepr = rdof_sdr_alloc_hot(sizeof(rdof_cf_t) * C);
    s->H = rdof_sdr_alloc_hot(sizeof(rdof_cf_t) * M);
    s->win = rdof_sdr_zalloc(sizeof(rdof_cf_t) * (size_t)(N));
    s->X = rdof_sdr_alloc_hot(sizeof(rdof_cf_t) * N);
    s->Y = rdof_sdr_alloc_hot(sizeof(rdof_cf_t) * M);
    s->y = rdof_sdr_alloc_hot(sizeof(rdof_cf_t) * M);
    s->twiddle = rdof_sdr_alloc_hot(sizeof(rdof_cf_t) * N);
    s->level = rdof_sdr_zalloc(sizeof(float) * (size_t)C);
    s->fwd = rdof_fft_new(N, false);
    s->inv = rdof_fft_new(M, true);
    if (!s->k || !s->residual_hz || !s->correction_hz || !s->phase || !s->H || !s->win || !s->X || !s->Y || !s->y ||
        !s->twiddle || !s->fwd || !s->inv || !s->step_cps || !s->step_rad || !s->stepr || !s->corr_used ||
        !s->block_rad || !s->level)
        return -1;

    /* Filter: N/4 + 1 taps, its response at the M bins around 0 (0..M/2-1, -M/2..-1). */
    const int taps = N - s->hop + 1;
    double *h = rdof_sdr_alloc(sizeof(double) * taps);
    if (!h)
        return -1;
    rdof_chz_lowpass(taps, cutoff_hz, s->fs, h);
    double hn = 0.0;
    for (int n = 0; n < taps; n++)
        hn += h[n] * h[n];
    s->hnoise = (float)hn;
    for (int j = 0; j < M; j++) {
        int rel = j < M / 2 ? j : j - M;
        int bin = ((rel % N) + N) % N;
        double re = 0.0, im = 0.0;
        for (int n = 0; n < taps; n++) {
            long long kn = ((long long)bin * n) % N;
            double ph = -2.0 * RDOF_PI * (double)kn / (double)N;
            re += h[n] * cos(ph);
            im += h[n] * sin(ph);
        }
        /* / D for unity gain, then / M for the unnormalized inverse FFT */
        float hr = (float)(re / s->D), hi = (float)(im / s->D);
        s->H[j].re = hr / (float)M;
        s->H[j].im = hi / (float)M;
    }
    rdof_sdr_free(h);

    const double bin_hz = s->fs / N;
    for (int c = 0; c < C; c++) {
        double kk = nearbyint(offsets_hz[c] / bin_hz);    /* round half to even */
        s->residual_hz[c] = offsets_hz[c] - kk * bin_hz;
        int ki = (int)kk;
        s->k[c] = ((ki % N) + N) % N;
    }
    for (int m = 0; m < N; m++) {
        double ph = -2.0 * RDOF_PI * (double)m / (double)N;
        s->twiddle[m].re = (float)cos(ph);
        s->twiddle[m].im = (float)sin(ph);
    }
    for (int c = 0; c < C; c++) {
        s->step_cps[c] = NAN; /* compute the NCO step on first use */
        s->corr_used[c] = NAN;
    }
    s->fill = N - s->hop; /* starts with N - hop zeros */
    return 0;
}

int rdof_chz_out_len(const rdof_chz_t *s, int n_in)
{
    return ((s->fill + n_in - (s->N - s->hop)) / s->hop) * s->out_per_hop;
}

static inline rdof_cf_t cmul(rdof_cf_t a, rdof_cf_t b)
{
    rdof_cf_t r = {a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re};
    return r;
}

/* Y[i] = X[(start + i) mod N] * H[i] for n bins: at most two runs, no test per bin. */
static void gather(const rdof_cf_t *X, int N, int start, const rdof_cf_t *H, rdof_cf_t *Y, int n)
{
    while (n > 0) {
        int run = N - start < n ? N - start : n;
        for (int i = 0; i < run; i++)
            Y[i] = cmul(X[start + i], H[i]);
        Y += run;
        H += run;
        n -= run;
        start = 0;
    }
}

/* The k-th smallest of v[0..n-1] (reorders v). */
static float select_kth(float *v, int n, int k)
{
    int lo = 0, hi = n - 1;
    while (lo < hi) {
        const float pivot = v[(lo + hi) / 2];
        int i = lo, j = hi;
        while (i <= j) {
            while (v[i] < pivot)
                i++;
            while (v[j] > pivot)
                j--;
            if (i <= j) {
                float t = v[i];
                v[i++] = v[j];
                v[j--] = t;
            }
        }
        if (k <= j)
            hi = j;
        else if (k >= i)
            lo = i;
        else
            break;
    }
    return v[k];
}

#define FLOOR_STEP 4 /* bins between the floor's samples */
#define FLOOR_MAX 512 /* N / FLOOR_STEP at most */

/* The input's noise power per sample, from the block's spectrum in X: the 25th percentile of every
 * FLOOR_STEP-th bin of the Hann-windowed spectrum, over ln(4/3) (an exponential's 25th percentile as a share of
 * its mean), over the window's power gain (3N/8). Channels and strong signals sit above it, the band edges
 * below. */
static float noise_in(const rdof_chz_t *s, const rdof_cf_t *X)
{
    const int N = s->N;
    float mag[FLOOR_MAX];
    int n = 0;
    for (int k = 0; k < N && n < FLOOR_MAX; k += FLOOR_STEP) {
        const rdof_cf_t a = X[k ? k - 1 : N - 1], b = X[k], c = X[k + 1 < N ? k + 1 : 0];
        const float re = 0.5f * b.re - 0.25f * (a.re + c.re), im = 0.5f * b.im - 0.25f * (a.im + c.im);
        mag[n++] = re * re + im * im;
    }
    return select_kth(mag, n, n / 4) * 3.4760595f / (0.375f * (float)N);
}

/* One channel's share of one block (spectrum X, block offset boff): its bins, the filter, the inverse FFT, its
 * level, the bin-shift fix and the NCO, out_per_hop samples to dst. inv, Y, y: the caller's (one set a core). */
static void channel_block(rdof_chz_t *s, int c, const rdof_cf_t *X, unsigned boff, float noise, rdof_cf_t *dst,
                          rdof_fft_t *inv, rdof_cf_t *Y, rdof_cf_t *y)
{
    const int N = s->N, M = s->M, half = M / 2;
    const int kc = s->k[c];
    /* bins kc .. kc+M/2-1 and kc-M/2 .. kc-1, wrapped */
    gather(X, N, kc, s->H, Y, half);
    gather(X, N, kc >= half ? kc - half : kc - half + N, s->H + half, Y + half, half);
    rdof_fft_run(inv, Y, y);
    float pw = 0.0f; /* the block's output power, per sample (the NCO doesn't change it) */
    for (int j = s->discard; j < M; j++)
        pw += y[j].re * y[j].re + y[j].im * y[j].im;
    pw /= (float)s->out_per_hop;
    if (pw > s->level[c] * noise)
        s->level[c] = noise > 0.0f ? pw / noise : INFINITY;
    /* Bin shift k on a block starting at b * hop leaves e^{-j 2 pi k b hop / N}: undo it. */
    const rdof_cf_t fix = s->twiddle[((unsigned)kc * boff) % (unsigned)N];
    /* NCO e^{-j phase} at (residual + correction) / out_rate cycles per sample. Doubles are software
     * on the S3: worked out only when the correction changes (AFC, once a chunk at most). */
    if (!(s->correction_hz[c] == s->corr_used[c])) {
        s->corr_used[c] = s->correction_hz[c];
        const double cps = (s->residual_hz[c] + s->correction_hz[c]) / s->out_rate;
        if (cps != s->step_cps[c]) {
            s->step_cps[c] = cps;
            s->step_rad[c] = 2.0 * RDOF_PI * cps;
            s->block_rad[c] = s->step_rad[c] * s->out_per_hop;
            s->stepr[c].re = cosf((float)s->step_rad[c]);
            s->stepr[c].im = -sinf((float)s->step_rad[c]);
        }
    }
    double ph = s->phase[c];
    const float phf = (float)ph;
    rdof_cf_t rot0 = {cosf(phf), -sinf(phf)};
    /* two phasors a sample apart, each stepping two: half the dependency chain */
    const rdof_cf_t step = s->stepr[c], step2 = cmul(step, step);
    rdof_cf_t ra = cmul(rot0, fix), rb = cmul(ra, step);
    const rdof_cf_t *src = y + s->discard;
    int j = 0;
    for (; j + 1 < s->out_per_hop; j += 2) {
        dst[j] = cmul(src[j], ra);
        dst[j + 1] = cmul(src[j + 1], rb);
        ra = cmul(ra, step2);
        rb = cmul(rb, step2);
    }
    if (j < s->out_per_hop)
        dst[j] = cmul(src[j], ra);
    ph += s->block_rad[c]; /* one double add per block, kept in [0, 2 pi) */
    if (ph >= 2.0 * RDOF_PI || ph < 0.0)
        ph -= 2.0 * RDOF_PI * floor(ph / (2.0 * RDOF_PI));
    s->phase[c] = ph;
}

rdof_chz_prof_t rdof_chz_prof;
static uint32_t (*chz_clock)(void);
void rdof_chz_set_clock(uint32_t (*ticks)(void)) { chz_clock = ticks; }
static inline uint32_t chz_now(void) { return chz_clock ? chz_clock() : 0; }

/* The next block's window is full: its forward FFT into X, the noise floor on a call's first block. */
static void forward_block(rdof_chz_t *s, rdof_cf_t *X, bool *first)
{
    const uint32_t t0 = chz_now();
    rdof_fft_run(s->fwd, s->win, X);
    const uint32_t t1 = chz_now();
    if (*first) { /* the floor moves slowly: every 8th call */
        if (s->noise_in == 0.0f || ++s->noise_age >= 8) {
            s->noise_in = noise_in(s, X);
            s->noise_age = 0;
        }
        *first = false;
    }
    if (chz_clock) {
        rdof_chz_prof.fft += (uint32_t)(t1 - t0);
        rdof_chz_prof.noise += (uint32_t)(chz_now() - t1);
        rdof_chz_prof.blocks++;
    }
}

int rdof_chz_process(rdof_chz_t *s, const rdof_cf_t *in, int n_in, rdof_cf_t *out, int stride)
{
    const int N = s->N;
    int produced = 0, used = 0;
    bool first = true;
    for (int c = 0; c < s->C; c++)
        s->level[c] = 0.0f;
    while (used < n_in) {
        int take = N - s->fill;
        if (take > n_in - used)
            take = n_in - used;
        const uint32_t tc = chz_now();
        memcpy(s->win + s->fill, in + used, sizeof(rdof_cf_t) * take);
        if (chz_clock)
            rdof_chz_prof.copy += (uint32_t)(chz_now() - tc);
        s->fill += take;
        used += take;
        if (s->fill < N)
            break;
        forward_block(s, s->X, &first);
        const float noise = s->noise_in * s->hnoise;
        const unsigned boff = s->boff;
        s->boff = (boff + (unsigned)s->hop) % (unsigned)N;
        s->block++;
        const uint32_t th = chz_now();
        for (int c = 0; c < s->C; c++)
            channel_block(s, c, s->X, boff, noise, out + (size_t)c * stride + produced, s->inv, s->Y, s->y);
        const uint32_t tm = chz_now();
        produced += s->out_per_hop;
        memmove(s->win, s->win + s->hop, sizeof(rdof_cf_t) * (N - s->hop));
        if (chz_clock) {
            rdof_chz_prof.chan += (uint32_t)(tm - th);
            rdof_chz_prof.copy += (uint32_t)(chz_now() - tm);
        }
        s->fill = N - s->hop;
    }
    return produced;
}

int rdof_chz_reserve(rdof_chz_t *s, int max_in)
{
    const int blocks = (s->N + max_in) / s->hop + 1;
    if (blocks <= s->xs_cap)
        return 0;
    rdof_sdr_free(s->Xs);
    rdof_sdr_free(s->xs_boff);
    s->Xs = rdof_sdr_alloc(sizeof(rdof_cf_t) * (size_t)s->N * (size_t)blocks);
    s->xs_boff = rdof_sdr_alloc(sizeof(unsigned) * (size_t)blocks);
    if (!s->inv2)
        s->inv2 = rdof_fft_new(s->M, true);
    if (!s->Y2)
        s->Y2 = rdof_sdr_alloc_hot(sizeof(rdof_cf_t) * s->M);
    if (!s->y2)
        s->y2 = rdof_sdr_alloc_hot(sizeof(rdof_cf_t) * s->M);
    s->xs_cap = s->Xs && s->xs_boff ? blocks : 0;
    return s->xs_cap && s->inv2 && s->Y2 && s->y2 ? 0 : -1;
}

int rdof_chz_forward(rdof_chz_t *s, const rdof_cf_t *in, int n_in)
{
    const int N = s->N;
    int used = 0;
    bool first = true;
    s->nblk = 0;
    for (int c = 0; c < s->C; c++)
        s->level[c] = 0.0f;
    while (used < n_in && s->nblk < s->xs_cap) {
        int take = N - s->fill;
        if (take > n_in - used)
            take = n_in - used;
        const uint32_t tc = chz_now();
        memcpy(s->win + s->fill, in + used, sizeof(rdof_cf_t) * take);
        if (chz_clock)
            rdof_chz_prof.copy += (uint32_t)(chz_now() - tc);
        s->fill += take;
        used += take;
        if (s->fill < N)
            break;
        forward_block(s, s->Xs + (size_t)s->nblk * N, &first);
        s->xs_boff[s->nblk++] = s->boff;
        s->boff = (s->boff + (unsigned)s->hop) % (unsigned)N;
        s->block++;
        const uint32_t tm = chz_now();
        memmove(s->win, s->win + s->hop, sizeof(rdof_cf_t) * (N - s->hop));
        if (chz_clock)
            rdof_chz_prof.copy += (uint32_t)(chz_now() - tm);
        s->fill = N - s->hop;
    }
    s->xs_noise = s->noise_in * s->hnoise;
    return s->nblk * s->out_per_hop;
}

void rdof_chz_channels(rdof_chz_t *s, int c0, int c1, int worker, rdof_cf_t *out, int stride)
{
    rdof_fft_t *inv = worker ? s->inv2 : s->inv;
    rdof_cf_t *Y = worker ? s->Y2 : s->Y, *y = worker ? s->y2 : s->y;
    for (int c = c0; c < c1 && c < s->C; c++)
        for (int b = 0; b < s->nblk; b++)
            channel_block(s, c, s->Xs + (size_t)b * s->N, s->xs_boff[b], s->xs_noise,
                          out + (size_t)c * stride + (size_t)b * s->out_per_hop, inv, Y, y);
}

#define PLAN_USABLE 0.8
#define PLAN_DC_GUARD 25e3

/* Channels in the window at c: inside the usable band and clear of DC. */
static int window_count(const double *f, int n, double c, double half, double dc_guard, bool *in)
{
    int k = 0;
    for (int i = 0; i < n; i++) {
        double o = fabs(f[i] - c);
        bool ok = o <= half && o >= dc_guard;
        if (in)
            in[i] = ok;
        k += ok;
    }
    return k;
}

int rdof_plan_window(const double *freqs_hz, int n, double sample_rate, double *center, bool *inside)
{
    return rdof_plan_window_band(freqs_hz, n, sample_rate, PLAN_USABLE, PLAN_DC_GUARD, center, inside);
}

int rdof_plan_window_band(const double *freqs_hz, int n, double sample_rate, double usable, double dc_guard,
                          double *center, bool *inside)
{
    const double half = sample_rate * usable / 2;
    double c;
    if (n < 1)
        return 0;
    if (rdof_plan_center_band(freqs_hz, n, sample_rate, usable, dc_guard, &c) == 0 &&
        window_count(freqs_hz, n, c, half, dc_guard, NULL) == n) {
        *center = c;
        if (inside)
            for (int i = 0; i < n; i++)
                inside[i] = true;
        return n;
    }
    /* Candidates: where some channel meets an edge of the band or of the DC guard (the optimum sits on
     * one of these), each nudged 1 Hz inward. */
    const double edges[] = {half, -half, dc_guard, -dc_guard};
    const double nudge[] = {-1.0, 1.0, 1.0, -1.0};
    int best = 0;
    double best_c = 0.0, best_score = -INFINITY;
    bool *in = rdof_sdr_alloc(sizeof(bool) * (size_t)n);
    double *sub = rdof_sdr_alloc(sizeof(double) * (size_t)n);
    if (!in || !sub) {
        rdof_sdr_free(in);
        rdof_sdr_free(sub);
        return 0;
    }
    for (int i = 0; i < n; i++) {
        for (int e = 0; e < 4; e++) {
            const double cand = freqs_hz[i] + edges[e] + nudge[e];
            const int k = window_count(freqs_hz, n, cand, half, dc_guard, in);
            if (k == 0 || k < best)
                continue;
            /* score the set as plan_center would, and prefer it centered */
            int m = 0;
            for (int a = 0; a < n; a++)
                if (in[a])
                    sub[m++] = freqs_hz[a];
            double pc;
            double score = -INFINITY;
            if (rdof_plan_center_band(sub, m, sample_rate, usable, dc_guard, &pc) == 0) {
                double dc = INFINITY, img = 1e9;
                for (int a = 0; a < m; a++) {
                    double oa = sub[a] - pc;
                    dc = fmin(dc, fabs(oa));
                    for (int b = a; b < m; b++)
                        if (oa * (sub[b] - pc) < 0)
                            img = fmin(img, fabs(oa + sub[b] - pc));
                }
                score = fmin(dc, dc_guard * 2) + fmin(img, 30e3);
                if (window_count(sub, m, pc, half, dc_guard, NULL) < m)
                    pc = cand; /* plan_center put one on DC: keep the candidate */
            } else {
                pc = cand;
            }
            if (k > best || score > best_score) {
                best = k;
                best_score = score;
                best_c = pc;
            }
        }
    }
    rdof_sdr_free(sub);
    if (best) {
        *center = best_c;
        best = window_count(freqs_hz, n, best_c, half, dc_guard, in);
        if (inside)
            for (int i = 0; i < n; i++)
                inside[i] = in[i];
    }
    rdof_sdr_free(in);
    return best;
}

int rdof_plan_center(const double *freqs_hz, int n, double sample_rate, double *center)
{
    return rdof_plan_center_band(freqs_hz, n, sample_rate, PLAN_USABLE, PLAN_DC_GUARD, center);
}

int rdof_plan_center_band(const double *freqs_hz, int n, double sample_rate, double usable, double dc_guard,
                          double *center)
{
    const double step = 5e3;
    if (n < 1)
        return -1;
    double lo = freqs_hz[0], hi = freqs_hz[0];
    for (int i = 1; i < n; i++) {
        lo = fmin(lo, freqs_hz[i]);
        hi = fmax(hi, freqs_hz[i]);
    }
    const double half = sample_rate * usable / 2;
    if (hi - lo > 2 * half)
        return -1;
    const double mid = (lo + hi) / 2;
    const int span = (int)((2 * half - (hi - lo)) / 2 / step);
    bool found = false;
    double best = 0.0, best_score = -INFINITY;
    for (int i = -span; i <= span; i++) {
        double c = mid + i * step, maxo = 0.0, dc = INFINITY, img = 1e9;
        for (int a = 0; a < n; a++) {
            double oa = freqs_hz[a] - c;
            maxo = fmax(maxo, fabs(oa));
            dc = fmin(dc, fabs(oa));
            for (int b = a; b < n; b++) {
                double ob = freqs_hz[b] - c;
                if (oa * ob < 0)
                    img = fmin(img, fabs(oa + ob));
            }
        }
        if (maxo > half)
            continue;
        double score = fmin(dc, dc_guard * 2) + fmin(img, 30e3) - fabs((double)i) * 1.0;
        if (dc < dc_guard)
            score -= 1e6;
        if (score > best_score) {
            best = c;
            best_score = score;
            found = true;
        }
    }
    if (!found)
        return -1;
    *center = best;
    return 0;
}

#endif
