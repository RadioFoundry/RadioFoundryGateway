// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Host tests for the FPGA channelizer's bit-exact model (chzfx.h):
 *
 * - exact: the golden vectors in fixtures/chzfx/ (the inputs, the configuration, the outputs the model gave when
 *   they were made; an HLS testbench checks the FPGA block against the same files), the model fed in chunks, the
 *   frames, the table, the configuration checks;
 * - within a tolerance (printed): an ideal DDC in double (the same steps without rounding: what the fixed point
 *   costs), and the float overlap-save channelizer (channelizer.h) on the same input (not with a CIC: its FFT at
 *   23 MS/s is past what it is for).
 *
 * `rdof_chzfx_tests --write` makes the golden vectors again (after a deliberate change to the model).
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/sdr/channelizer.h"
#include "rdof/sdr/chzfx.h"

#ifndef RDOF_CHZFX_DATA
#define RDOF_CHZFX_DATA "chzfx/"
#endif

#define PI 3.14159265358979323846

/* Tolerances, per channel: the error's RMS at most a share of the output's RMS plus some output LSB. */
#define TOL_IDEAL_REL 1.5e-3 /* the ideal DDC: the NCO's 12-bit phase (spurs at -66 dBc) */
#define TOL_IDEAL_LSB 0.6    /* and the output's rounding (0.41 LSB RMS, I and Q) */
#define TOL_CHZ_REL 1e-2     /* the float channelizer: its filter sits on the nearest bin and its NCO after the
                                filter, which moves the transition band by up to half a bin (375 Hz at 2.4 MS/s) */
#define TOL_CHZ_LSB 1.0

static int checks, failures;

#define CHECK(cond, ...)                                                                                     \
    do {                                                                                                     \
        checks++;                                                                                            \
        if (!(cond)) {                                                                                       \
            failures++;                                                                                      \
            if (failures <= 40) {                                                                            \
                printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                      \
                printf(__VA_ARGS__);                                                                         \
                printf("\n");                                                                                \
            }                                                                                                \
        }                                                                                                    \
    } while (0)

/* A test case: a configuration, an input made from tones and noise, AFC steps mid-stream. */
typedef struct {
    double hz, amp;
} tone_t;

typedef struct {
    int at;       /* before input sample at * R * decim: latched after output at */
    int c;
    double shift; /* Hz added to the channel's offset */
} retune_t;

typedef struct {
    const char *name;
    double fs;
    int cic, decim, channels, n; /* cic: R, 1 for none */
    double offsets[RDOF_CHZFX_MAX_CH];
    tone_t tones[24];
    int ntones;
    int noise;  /* uniform, +-noise LSB on I and Q */
    retune_t retunes[2];
    int nretunes;
    int compare; /* checked against the ideal DDC and the float channelizer (no saturation in it) */
} chzcase_t;

static const chzcase_t cases[] = {
    {
        /* 16 channels at 2.4 MS/s across the band (two near DC), a tone in each within DMR's deviation, levels
         * from 6 to 190 LSB, noise; an AFC step on one channel */
        "2400k_16ch",
        2400000.0,
        1,
        50,
        16,
        48000,
        {-950000, -812500, -600000, -437500, -300000, -162500, -62500, -30000, 30000, 112500, 250000, 387500, 525000,
         662500, 800000, 950000},
        {{-950000 + 1000, 120},
         {-812500 - 1500, 60},
         {-600000 + 300, 190},
         {-437500 - 1944, 90},
         {-300000 + 648, 40},
         {-162500 - 648, 150},
         {-62500 + 1944, 25},
         {-30000 - 250, 110},
         {30000 + 1200, 70},
         {112500 - 1800, 180},
         {250000 + 500, 6},
         {387500 - 900, 130},
         {525000 + 1700, 85},
         {662500 - 100, 160},
         {800000 + 1944, 45},
         {950000 - 1300, 100},
         {700000, 150}}, /* between channels: the filters' stopband */
        17,
        3,
        {{500, 3, 200.0}},
        1,
        1,
    },
    {
        /* 1.536 MS/s, three channels, a tone past 12 bits: the mixer and the output saturate */
        "1536k_sat",
        1536000.0,
        1,
        32,
        3,
        16384,
        {-400000, 100000, 600000},
        {{100000 + 500, 30000}, {-400000 - 700, 900}, {600000 + 1500, 50}},
        3,
        2,
        {{100, 0, -150.0}},
        1,
        0,
    },
    {
        /* The 15-channel event plan (438.0125 to 438.6 MHz) at 23.04 MS/s around 438.5 MHz: CIC 60, then D 8. A tone
         * in each channel within DMR's deviation, 8 to 200 LSB; two strong ones between channels (the CIC's
         * aliases and the FIR's stopband); noise; an AFC step on one channel */
        "23040k_15ch",
        23040000.0,
        60,
        8,
        15,
        480 * 100,
        {8075000, 2162500, 2700000, 1787500, -7525000, 1837500, -7400000, 1887500, -7800000, 1937500, -7425000,
         1987500, 1737500, 2037500, 2387500},
        {{8075000 + 1000, 120},
         {2162500 - 1500, 60},
         {2700000 + 300, 190},
         {1787500 - 1944, 90},
         {-7525000 + 648, 40},
         {1837500 - 648, 150},
         {-7400000 + 1944, 25},
         {1887500 - 250, 110},
         {-7800000 + 1200, 70},
         {1937500 - 1800, 180},
         {-7425000 + 500, 8},
         {1987500 - 900, 130},
         {1737500 + 1700, 85},
         {2037500 - 100, 160},
         {2387500 + 1944, 45},
         {-3000000, 400},  /* nothing near: the CIC */
         {2212500, 300}},  /* between 2.1625 and 2.3875: the FIR's stopband */
        17,
        3,
        {{50, 3, 200.0}},
        1,
        1,
    },
};
#define NCASES ((int)(sizeof cases / sizeof cases[0]))

/* The case's input: tones and noise, rounded and clipped to int16 (a fixed LCG, so it is the same everywhere it
 * runs; only --write uses it, the tests read the committed file). */
static void make_input(const chzcase_t *k, rdof_iq16_t *x)
{
    uint32_t lcg = 12345u;
    for (int n = 0; n < k->n; n++) {
        double re = 0.0, im = 0.0;
        for (int t = 0; t < k->ntones; t++) {
            double ph = 2.0 * PI * fmod(k->tones[t].hz / k->fs * n, 1.0);
            re += k->tones[t].amp * cos(ph);
            im += k->tones[t].amp * sin(ph);
        }
        lcg = lcg * 1664525u + 1013904223u;
        re += (int)(lcg >> 16) % (2 * k->noise + 1) - k->noise;
        lcg = lcg * 1664525u + 1013904223u;
        im += (int)(lcg >> 16) % (2 * k->noise + 1) - k->noise;
        x[n].i = (int16_t)fmax(-32768.0, fmin(32767.0, nearbyint(re)));
        x[n].q = (int16_t)fmax(-32768.0, fmin(32767.0, nearbyint(im)));
    }
}

static char *path(const char *name, const char *suffix)
{
    static char p[512];
    snprintf(p, sizeof p, "%s%s%s", RDOF_CHZFX_DATA, name, suffix);
    return p;
}

static int write_file(const char *p, const void *data, size_t size)
{
    FILE *f = fopen(p, "wb");
    if (!f)
        return -1;
    size_t w = fwrite(data, 1, size, f);
    return fclose(f) == 0 && w == size ? 0 : -1;
}

/* A whole file (NULL if missing); *size its bytes. */
static void *read_file(const char *p, size_t *size)
{
    FILE *f = fopen(p, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = malloc((size_t)n + 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    ((char *)buf)[n] = 0;
    *size = (size_t)n;
    return buf;
}

/* int16 pairs as little-endian bytes (the files) and back */
static void to_le(const rdof_iq16_t *x, size_t n, uint8_t *b)
{
    for (size_t k = 0; k < n; k++) {
        const uint16_t i = (uint16_t)x[k].i, q = (uint16_t)x[k].q;
        b[4 * k] = (uint8_t)i;
        b[4 * k + 1] = (uint8_t)(i >> 8);
        b[4 * k + 2] = (uint8_t)q;
        b[4 * k + 3] = (uint8_t)(q >> 8);
    }
}

static void from_le(const uint8_t *b, size_t n, rdof_iq16_t *x)
{
    for (size_t k = 0; k < n; k++) {
        x[k].i = (int16_t)(uint16_t)(b[4 * k] | b[4 * k + 1] << 8);
        x[k].q = (int16_t)(uint16_t)(b[4 * k + 2] | b[4 * k + 3] << 8);
    }
}

/* The configuration file: what the registers get, in order, and the AFC steps. */
static int write_cfg(const chzcase_t *k, const rdof_chzfx_cfg_t *g, const uint32_t *retune_inc)
{
    FILE *f = fopen(path(k->name, ".cfg"), "w");
    if (!f)
        return -1;
    fprintf(f, "# chzfx golden vector %s: rdof_chzfx_tests --write made it (host-tests/test_chzfx.c).\n", k->name);
    fprintf(f, "# Input %s_in.cs16, output %s_out.cs16: int16 I, Q pairs, little-endian; the output has\n", k->name,
            k->name);
    fprintf(f, "# a pair per channel per output instant, channel order. 'retune K C INC': once the inputs before\n");
    fprintf(f, "# sample K * cic_r * decim are in, channel C's step becomes INC (latched after output K).\n");
    fprintf(f, "fs %.0f\ndecim %d\ntaps %d\nchannels %d\nout_shift %d\ninputs %d\n", k->fs, g->decim, g->taps,
            g->channels, g->out_shift, k->n);
    if (g->cic_r > 1)
        fprintf(f, "cic_r %d\ncic_shift %d\n", g->cic_r, g->cic_shift);
    fprintf(f, "inc");
    for (int c = 0; c < g->channels; c++)
        fprintf(f, " %u", (unsigned)g->inc[c]);
    fprintf(f, "\nphase0");
    for (int c = 0; c < g->channels; c++)
        fprintf(f, " %u", (unsigned)g->phase0[c]);
    fprintf(f, "\ncoef");
    for (int m = 0; m < g->taps; m++)
        fprintf(f, " %d", (int)g->coef[m]);
    fprintf(f, "\n");
    for (int r = 0; r < k->nretunes; r++)
        fprintf(f, "retune %d %d %u\n", k->retunes[r].at, k->retunes[r].c, (unsigned)retune_inc[r]);
    return fclose(f);
}

/* The case's configuration: rdof_chzfx_design's, and phase0 so the outputs line up with the float channelizer
 * (whose phase at the start comes from its bin shift and from where its NCO sits, after the filter). */
static int case_cfg(const chzcase_t *k, rdof_chzfx_cfg_t *g, uint32_t *retune_inc)
{
    if (rdof_chzfx_design_cic(g, k->fs, k->cic, k->decim, k->offsets, k->channels, RDOF_CHZ_CUTOFF_HZ) != 0)
        return -1;
    for (int r = 0; r < k->nretunes; r++) {
        const retune_t *rt = &k->retunes[r];
        retune_inc[r] = rdof_chzfx_inc(k->offsets[rt->c] + rt->shift, k->fs);
    }
    if (k->cic > 1)
        return 0; /* phases 0: nothing to line up with */
    rdof_chz_t chz;
    if (rdof_chz_init(&chz, k->fs, k->offsets, k->channels, k->decim, RDOF_CHZ_OUT_FFT, RDOF_CHZ_CUTOFF_HZ) != 0) {
        rdof_chz_free(&chz);
        return -1;
    }
    const double d = (g->taps - 1) / 2.0;
    for (int c = 0; c < k->channels; c++) {
        /* turns: -(k hop mod N) / N + residual d / fs */
        double t = -(double)(((long long)chz.k[c] * chz.hop) % chz.N) / chz.N + chz.residual_hz[c] * d / k->fs;
        t -= floor(t);
        g->phase0[c] = (uint32_t)((uint64_t)llround(t * 4294967296.0) & 0xFFFFFFFFu);
    }
    rdof_chz_free(&chz);
    return 0;
}

/* The model over the input, in chunks of chunk samples (0: all at once), with the case's AFC steps. */
static int run_model(const chzcase_t *k, const rdof_chzfx_cfg_t *g, const uint32_t *retune_inc, int nretunes,
                     const rdof_iq16_t *x, int chunk, rdof_iq16_t *y)
{
    rdof_chzfx_t *s = rdof_chzfx_new(g);
    if (!s)
        return -1;
    int done = 0, out = 0, r = 0;
    while (done < k->n) {
        int stop = k->n;
        if (r < nretunes && k->retunes[r].at * rdof_chzfx_step(g) < stop)
            stop = k->retunes[r].at * rdof_chzfx_step(g);
        if (chunk && done + chunk < stop)
            stop = done + chunk;
        if (stop > done) {
            const int want = rdof_chzfx_out_len(s, stop - done);
            const int got = rdof_chzfx_process(s, x + done, stop - done, y + (size_t)out * g->channels);
            CHECK(got == want, "%s: out_len %d, process %d", k->name, want, got);
            out += got;
            done = stop;
        }
        while (r < nretunes && k->retunes[r].at * rdof_chzfx_step(g) <= done) {
            rdof_chzfx_set_inc(s, k->retunes[r].c, retune_inc[r]);
            r++;
        }
    }
    CHECK(s->seq == (uint32_t)out, "%s: seq %u, %d outputs", k->name, (unsigned)s->seq, out);
    rdof_chzfx_free(s);
    return out;
}

/* The same steps in double, nothing rounded: an ideal DDC with the float filter, output in the model's units. */
static void ideal_ddc(const chzcase_t *k, const rdof_chzfx_cfg_t *g, const uint32_t *retune_inc, int nretunes,
                      const rdof_iq16_t *x, int nout, double *yi, double *yq)
{
    const int C = g->channels, L = g->taps, R = g->cic_r > 1 ? g->cic_r : 1;
    double *h = malloc(sizeof(double) * (size_t)L);
    double *mi = malloc(sizeof(double) * (size_t)k->n), *mq = malloc(sizeof(double) * (size_t)k->n);
    rdof_chz_lowpass(L, RDOF_CHZ_CUTOFF_HZ, k->fs / R, h);
    /* the CIC as its FIR, ((1 - z^-R) / (1 - z^-1))^N, with unity gain (the design folds the CIC's into the taps) */
    const int nb = RDOF_CHZFX_CIC_N * (R - 1) + 1;
    double *b = calloc((size_t)nb, sizeof(double));
    b[0] = 1.0;
    for (int st = 0, len = 1; R > 1 && st < RDOF_CHZFX_CIC_N; st++) {
        /* times the boxcar of R */
        double *t = calloc((size_t)(len + R - 1), sizeof(double));
        for (int i = 0; i < len; i++)
            for (int j = 0; j < R; j++)
                t[i + j] += b[i] / R;
        len += R - 1;
        memcpy(b, t, sizeof(double) * (size_t)len);
        free(t);
    }
    for (int c = 0; c < C; c++) {
        uint32_t ph = g->phase0[c], inc = g->inc[c];
        for (int n = 0; n < k->n; n++) {
            const double th = 2.0 * PI * ph / 4294967296.0, co = cos(th), si = sin(th);
            mi[n] = x[n].i * co + x[n].q * si;
            mq[n] = x[n].q * co - x[n].i * si;
            ph += inc;
            for (int r = 0; r < nretunes; r++)
                if (k->retunes[r].c == c && n == k->retunes[r].at * rdof_chzfx_step(g))
                    inc = retune_inc[r];
        }
        for (int j = 0; j < nout; j++) {
            const int n = j * g->decim; /* in history samples */
            double ai = 0.0, aq = 0.0;
            for (int m = 0; m < L && m <= n; m++) {
                const int at = (n - m) * R; /* the input of history sample n - m */
                double vi = 0.0, vq = 0.0;
                for (int i = 0; i < nb && i <= at; i++) {
                    vi += b[i] * mi[at - i];
                    vq += b[i] * mq[at - i];
                }
                ai += h[m] * vi;
                aq += h[m] * vq;
            }
            yi[(size_t)j * C + c] = ai * (1 << RDOF_CHZFX_OUT_FRAC);
            yq[(size_t)j * C + c] = aq * (1 << RDOF_CHZFX_OUT_FRAC);
        }
    }
    free(h);
    free(b);
    free(mi);
    free(mq);
}

/* Per channel, from output skip on: the error's RMS in output LSB, and over the reference's RMS. Fails a channel
 * whose error is past tol_rel of its RMS plus tol_lsb (the output's rounding, which a weak channel sees whole);
 * returns the worst ratio of error to that allowance, and the worst relative error of a channel above 100 LSB. */
static double check_err(const char *what, const char *name, const rdof_iq16_t *y, const double *ri, const double *rq,
                        int nout, int C, int skip, double tol_rel, double tol_lsb, double *worst_rel)
{
    double worst = 0.0;
    *worst_rel = 0.0;
    for (int c = 0; c < C; c++) {
        double e = 0.0, p = 0.0;
        for (int j = skip; j < nout; j++) {
            const size_t at = (size_t)j * C + c;
            const double di = y[at].i - ri[at], dq = y[at].q - rq[at];
            e += di * di + dq * dq;
            p += ri[at] * ri[at] + rq[at] * rq[at];
        }
        e = sqrt(e / (nout - skip));
        p = sqrt(p / (nout - skip));
        const double ratio = e / (tol_rel * p + tol_lsb);
        CHECK(ratio <= 1.0, "%s: channel %d against %s: error %.3g LSB RMS on %.4g (%.3g), allowed %.3g", name, c, what,
              e, p, e / p, tol_rel * p + tol_lsb);
        worst = fmax(worst, ratio);
        if (p > 100.0)
            *worst_rel = fmax(*worst_rel, e / p);
    }
    return worst;
}

static int write_vectors(void)
{
    FILE *f = fopen(path("qcos", ".txt"), "w");
    if (!f)
        return -1;
    fprintf(f, "# rdof_chzfx_qcos: round(32767 cos(2 pi (i + 0.5) / 4096)), i = 0..1023\n");
    for (int i = 0; i < 1024; i++)
        fprintf(f, "%d\n", rdof_chzfx_qcos[i]);
    if (fclose(f) != 0)
        return -1;
    for (int t = 0; t < NCASES; t++) {
        const chzcase_t *k = &cases[t];
        rdof_chzfx_cfg_t g;
        uint32_t rinc[2];
        if (case_cfg(k, &g, rinc) != 0)
            return -1;
        rdof_iq16_t *x = malloc(sizeof *x * (size_t)k->n);
        rdof_iq16_t *y = malloc(sizeof *y * (size_t)(k->n / rdof_chzfx_step(&g) + 1) * (size_t)k->channels);
        uint8_t *b = malloc((size_t)k->n * 4);
        make_input(k, x);
        const int nout = run_model(k, &g, rinc, k->nretunes, x, 0, y);
        to_le(x, (size_t)k->n, b);
        int err = write_cfg(k, &g, rinc) || write_file(path(k->name, "_in.cs16"), b, (size_t)k->n * 4);
        to_le(y, (size_t)nout * (size_t)k->channels, b);
        err = err || write_file(path(k->name, "_out.cs16"), b, (size_t)nout * (size_t)k->channels * 4);
        printf("wrote %s: %d inputs, %d outputs a channel\n", k->name, k->n, nout);
        free(x);
        free(y);
        free(b);
        if (err)
            return -1;
    }
    return 0;
}

/* The cfg file against the case's configuration (the golden files and this test agree on the registers). */
static void check_cfg_file(const chzcase_t *k, const rdof_chzfx_cfg_t *g, const uint32_t *rinc)
{
    size_t size;
    char *txt = read_file(path(k->name, ".cfg"), &size);
    CHECK(txt != NULL, "%s.cfg missing", k->name);
    if (!txt)
        return;
    int decim = -1, taps = -1, chans = -1, shift = -1, coef_ok = 0, inc_ok = 0, ph_ok = 0, rt = 0, cic_r = 1,
        cic_shift = 0;
    for (char *line = strtok(txt, "\n"); line; line = strtok(NULL, "\n")) {
        char *p;
        if (sscanf(line, "decim %d", &decim) == 1 || sscanf(line, "taps %d", &taps) == 1 ||
            sscanf(line, "channels %d", &chans) == 1 || sscanf(line, "out_shift %d", &shift) == 1 ||
            sscanf(line, "cic_r %d", &cic_r) == 1 || sscanf(line, "cic_shift %d", &cic_shift) == 1)
            continue;
        if (!strncmp(line, "coef ", 5)) {
            p = line + 5;
            coef_ok = 1;
            for (int m = 0; m < g->taps; m++)
                coef_ok &= strtol(p, &p, 10) == g->coef[m];
        } else if (!strncmp(line, "inc ", 4) || !strncmp(line, "phase0 ", 7)) {
            const int is_inc = line[0] == 'i';
            p = line + (is_inc ? 4 : 7);
            int ok = 1;
            for (int c = 0; c < g->channels; c++)
                ok &= strtoul(p, &p, 10) == (is_inc ? g->inc[c] : g->phase0[c]);
            *(is_inc ? &inc_ok : &ph_ok) = ok;
        } else if (!strncmp(line, "retune ", 7)) {
            int at, c;
            unsigned inc;
            CHECK(rt < k->nretunes && sscanf(line, "retune %d %d %u", &at, &c, &inc) == 3 && at == k->retunes[rt].at &&
                      c == k->retunes[rt].c && inc == rinc[rt],
                  "%s.cfg: %s", k->name, line);
            rt++;
        }
    }
    CHECK(decim == g->decim && taps == g->taps && chans == g->channels && shift == g->out_shift &&
              cic_r == (g->cic_r > 1 ? g->cic_r : 1) && cic_shift == g->cic_shift,
          "%s.cfg: decim %d taps %d channels %d out_shift %d cic %d/%d", k->name, decim, taps, chans, shift, cic_r,
          cic_shift);
    CHECK(coef_ok && inc_ok && ph_ok && rt == k->nretunes, "%s.cfg: coef %d inc %d phase0 %d retunes %d", k->name,
          coef_ok, inc_ok, ph_ok, rt);
    free(txt);
}

/* With a CIC, the filters themselves: each channel's own tone (tone c) at its level, eight times the input's (the
 * output's 3 fraction bits), and every other tone gone. From output 20, the filters full; not a retuned channel. */
static void check_tones(const chzcase_t *k, const rdof_chzfx_cfg_t *g, const rdof_iq16_t *y, int nout)
{
    const int C = g->channels;
    for (int c = 0; c < C; c++) {
        int retuned = 0; /* its tone moves: the ideal DDC has checked it */
        for (int r = 0; r < k->nretunes; r++)
            retuned |= k->retunes[r].c == c;
        if (retuned)
            continue;
        const double f = k->tones[c].hz - k->offsets[c], fo = k->fs / rdof_chzfx_step(g);
        /* the tone's complex amplitude, and what's left once it's taken out */
        double re = 0.0, im = 0.0, p = 0.0;
        const int n = nout - 20;
        for (int j = 20; j < nout; j++) {
            const double th = -2.0 * PI * f * j / fo, a = y[(size_t)j * C + c].i, b = y[(size_t)j * C + c].q;
            re += a * cos(th) - b * sin(th);
            im += a * sin(th) + b * cos(th);
            p += a * a + b * b;
        }
        re /= n;
        im /= n;
        const double amp = hypot(re, im), rest = sqrt(fmax(0.0, p / n - amp * amp));
        const double want = k->tones[c].amp * (1 << RDOF_CHZFX_OUT_FRAC);
        CHECK(fabs(amp / want - 1.0) < 0.01, "%s: channel %d's tone %.1f, want %.1f", k->name, c, amp, want);
        /* the noise in 12.5 kHz of 23 MHz, +-3 LSB uniform: about 0.08 LSB in, 0.6 out; the rest under 2 LSB */
        CHECK(rest < 2.0, "%s: channel %d: %.2f LSB RMS besides its tone", k->name, c, rest);
    }
}

static void test_case(const chzcase_t *k)
{
    rdof_chzfx_cfg_t g;
    uint32_t rinc[2];
    CHECK(case_cfg(k, &g, rinc) == 0, "%s: configuration", k->name);
    check_cfg_file(k, &g, rinc);
    size_t size;
    uint8_t *bx = read_file(path(k->name, "_in.cs16"), &size);
    CHECK(bx && size == (size_t)k->n * 4, "%s_in.cs16: %zu bytes", k->name, bx ? size : 0);
    if (!bx || size != (size_t)k->n * 4) {
        free(bx);
        return;
    }
    rdof_iq16_t *x = malloc(sizeof *x * (size_t)k->n);
    from_le(bx, (size_t)k->n, x);
    free(bx);
    const int C = g.channels, cap = k->n / rdof_chzfx_step(&g) + 1;
    rdof_iq16_t *y = malloc(sizeof *y * (size_t)cap * C), *y2 = malloc(sizeof *y2 * (size_t)cap * C);
    const int nout = run_model(k, &g, rinc, k->nretunes, x, 0, y);

    /* the golden output, exactly */
    uint8_t *by = read_file(path(k->name, "_out.cs16"), &size);
    CHECK(by && size == (size_t)nout * C * 4, "%s_out.cs16: %zu bytes, want %d", k->name, by ? size : 0, nout * C * 4);
    if (by && size == (size_t)nout * C * 4) {
        from_le(by, (size_t)nout * C, y2);
        int bad = 0, first = -1;
        for (int j = 0; j < nout * C; j++)
            if (y[j].i != y2[j].i || y[j].q != y2[j].q) {
                bad++;
                if (first < 0)
                    first = j;
            }
        CHECK(bad == 0, "%s: %d samples differ from the golden output, the first at output %d channel %d", k->name,
              bad, first / C, first % C);
    }
    free(by);

    /* fed in odd chunks: the same, sample for sample */
    const int chunks[] = {1, 7, 333, 4096};
    for (int t = 0; t < 4; t++) {
        const int n2 = run_model(k, &g, rinc, k->nretunes, x, chunks[t], y2);
        CHECK(n2 == nout && !memcmp(y, y2, sizeof *y * (size_t)nout * C), "%s: chunks of %d differ", k->name,
              chunks[t]);
    }

    int sat_out = 0;
    for (int j = 0; j < nout * C; j++)
        sat_out += y[j].i == 32767 || y[j].i == -32768 || y[j].q == 32767 || y[j].q == -32768;
    if (!k->compare) {
        CHECK(sat_out > 0, "%s: nothing saturated", k->name);
        printf("  chzfx %s: %d outputs a channel match the golden file; %d saturated\n", k->name, nout, sat_out);
        free(x);
        free(y);
        free(y2);
        return;
    }
    CHECK(sat_out == 0, "%s: %d outputs saturated", k->name, sat_out);

    /* against the ideal DDC, AFC step included; from output 20 (the first fill of the filter is exact anyway) */
    double *ri = malloc(sizeof(double) * (size_t)nout * C), *rq = malloc(sizeof(double) * (size_t)nout * C);
    ideal_ddc(k, &g, rinc, k->nretunes, x, nout, ri, rq);
    double rel_ideal, rel_chz;
    const double r_ideal = check_err("the ideal DDC", k->name, y, ri, rq, nout, C, 0, TOL_IDEAL_REL, TOL_IDEAL_LSB,
                                     &rel_ideal);
    if (k->cic > 1) {
        check_tones(k, &g, y, nout);
        printf("  chzfx %s: %d outputs a channel match the golden file. RMS error / RMS, the worst channel over 100 "
               "LSB: %.2e against the ideal DDC (%.0f%% of the allowance)\n",
               k->name, nout, rel_ideal, 100 * r_ideal);
        free(ri);
        free(rq);
        free(x);
        free(y);
        free(y2);
        return;
    }

    /* against the float channelizer: no AFC step (the two apply one differently), input at full scale 2048 */
    const int n0 = run_model(k, &g, rinc, 0, x, 0, y2);
    rdof_chz_t chz;
    CHECK(rdof_chz_init(&chz, k->fs, k->offsets, C, g.decim, RDOF_CHZ_OUT_FFT, RDOF_CHZ_CUTOFF_HZ) == 0, "chz init");
    rdof_cf_t *xf = malloc(sizeof *xf * (size_t)k->n);
    const int stride = rdof_chz_out_len(&chz, k->n) + 1;
    rdof_cf_t *yf = malloc(sizeof *yf * (size_t)stride * C);
    for (int n = 0; n < k->n; n++) {
        xf[n].re = x[n].i / 2048.0f;
        xf[n].im = x[n].q / 2048.0f;
    }
    const int nf = rdof_chz_process(&chz, xf, k->n, yf, stride);
    rdof_chz_free(&chz);
    CHECK(nf == n0, "%s: the float channelizer gave %d outputs, the model %d", k->name, nf, n0);
    const double scale = 2048.0 * (1 << RDOF_CHZFX_OUT_FRAC);
    for (int j = 0; j < nf && j < n0; j++)
        for (int c = 0; c < C; c++) {
            ri[(size_t)j * C + c] = yf[(size_t)c * stride + j].re * scale;
            rq[(size_t)j * C + c] = yf[(size_t)c * stride + j].im * scale;
        }
    /* from output 20: both start from zeros, but the float filter's shifted transition band settles too */
    const double r_chz = check_err("the float channelizer", k->name, y2, ri, rq, nf < n0 ? nf : n0, C, 20,
                                   TOL_CHZ_REL, TOL_CHZ_LSB, &rel_chz);
    printf("  chzfx %s: %d outputs a channel match the golden file. RMS error / RMS, the worst channel over 100 "
           "LSB: %.2e against the ideal DDC, %.2e against the float channelizer (%.0f%% and %.0f%% of the "
           "allowance)\n",
           k->name, nout, rel_ideal, rel_chz, 100 * r_ideal, 100 * r_chz);
    free(xf);
    free(yf);
    free(ri);
    free(rq);
    free(x);
    free(y);
    free(y2);
}

static void test_table(void)
{
    int worst = 0;
    for (int k = 0; k < 4096; k++) {
        const uint32_t ph = (uint32_t)k << 20;
        const double th = 2.0 * PI * (k + 0.5) / 4096.0;
        const int co = rdof_chzfx_lo(ph), si = rdof_chzfx_lo(ph - (1u << 30));
        const int dc = abs(co - (int)lround(32767.0 * cos(th))), ds = abs(si - (int)lround(32767.0 * sin(th)));
        worst = dc > worst ? dc : worst;
        worst = ds > worst ? ds : worst;
        /* the low bits of the phase don't move it */
        CHECK(rdof_chzfx_lo(ph | 0xFFFFFu) == co, "lo at %d", k);
    }
    CHECK(worst == 0, "the table is off by %d from round(32767 cos)", worst);
}

static void test_cfg(void)
{
    CHECK(rdof_chzfx_inc(600000.0, 2400000.0) == 0x40000000u, "inc +fs/4");
    CHECK(rdof_chzfx_inc(-600000.0, 2400000.0) == 0xC0000000u, "inc -fs/4");
    CHECK(rdof_chzfx_inc(0.0, 2400000.0) == 0, "inc 0");
    rdof_chzfx_cfg_t g;
    const double off[2] = {100000.0, -250000.0};
    CHECK(rdof_chzfx_design(&g, 2400000.0, 50, off, 2, RDOF_CHZ_CUTOFF_HZ) == 0, "design 2.4 MS/s");
    int peak = 0;
    long long sum = 0;
    for (int m = 0; m < g.taps; m++) {
        peak = abs(g.coef[m]) > peak ? abs(g.coef[m]) : peak;
        sum += g.coef[m];
    }
    CHECK(g.taps == 801 && g.out_shift == 25 && peak > 65535 && peak <= 131071, "2.4 MS/s: %d taps, shift %d, peak %d",
          g.taps, g.out_shift, peak);
    /* unity gain at DC: sum of taps 2^(out_shift + OUT_FRAC - 4) */
    const double dc = (double)sum / ldexp(1.0, g.out_shift + RDOF_CHZFX_OUT_FRAC - 4);
    CHECK(fabs(dc - 1.0) < 1e-4, "DC gain %.6f", dc);
    CHECK(rdof_chzfx_design(&g, 2880000.0, 60, off, 2, RDOF_CHZ_CUTOFF_HZ) == 0 && g.taps == 961, "2.88 MS/s");
    CHECK(rdof_chzfx_design(&g, 3072000.0, 64, off, 2, RDOF_CHZ_CUTOFF_HZ) == -1, "3.072 MS/s is past the history");
    CHECK(rdof_chzfx_design(&g, 2400000.0, 50, off, 0, RDOF_CHZ_CUTOFF_HZ) == -1, "no channels");
    CHECK(rdof_chzfx_design(&g, 2400000.0, 50, off, 17, RDOF_CHZ_CUTOFF_HZ) == -1, "17 channels");
    rdof_chzfx_design(&g, 2400000.0, 50, off, 2, RDOF_CHZ_CUTOFF_HZ);
    g.coef[3] = 131072;
    CHECK(rdof_chzfx_check(&g) == -1 && rdof_chzfx_new(&g) == NULL, "a coefficient past 18 bits");
    g.coef[3] = -131072;
    CHECK(rdof_chzfx_check(&g) == 0, "-2^17 fits");
    g.out_shift = 0;
    CHECK(rdof_chzfx_check(&g) == -1, "shift 0");

    /* reset: back to phase0 and empty histories, the same output again */
    rdof_chzfx_design(&g, 2400000.0, 50, off, 2, RDOF_CHZ_CUTOFF_HZ);
    g.phase0[1] = 0x12345678u;
    rdof_chzfx_t *s = rdof_chzfx_new(&g);
    rdof_iq16_t x[200], a[8], b[8];
    for (int n = 0; n < 200; n++) {
        x[n].i = (int16_t)(n * 37 % 2001 - 1000);
        x[n].q = (int16_t)(n * 91 % 1777 - 888);
    }
    CHECK(rdof_chzfx_out_len(s, 200) == 4 && rdof_chzfx_process(s, x, 200, a) == 4, "4 outputs from 200");
    CHECK(rdof_chzfx_out_len(s, 1) == 1 && rdof_chzfx_out_len(s, 0) == 0, "out_len at an output instant");
    rdof_chzfx_reset(s);
    CHECK(rdof_chzfx_process(s, x, 200, b) == 4 && !memcmp(a, b, sizeof a), "reset");
    rdof_chzfx_set_inc(s, 5, 1); /* past the channels: ignored */
    CHECK(s->pending_mask == 0, "set_inc on channel 5 of 2");
    rdof_chzfx_free(s);
}

static void test_frames(void)
{
    rdof_iq16_t x[5] = {{1, -1}, {32767, -32768}, {-2, 3}, {0, 0}, {-12345, 23456}};
    uint64_t w[8];
    for (int n = 0; n <= 5; n++) {
        const size_t words = rdof_chzfx_frame(n == 3 ? RDOF_CHZFX_FRAME_RAW : RDOF_CHZFX_FRAME_CH, 0xDEADBEEFu + n, x,
                                              n, w);
        CHECK(words == rdof_chzfx_frame_words(n) && words == 1 + (size_t)(n + 1) / 2, "words for %d", n);
        rdof_chzfx_frame_t f;
        CHECK(rdof_chzfx_unframe(w, words, &f) == (int)words, "unframe %d", n);
        CHECK(f.n == n && f.seq == 0xDEADBEEFu + n && f.type == (n == 3 ? RDOF_CHZFX_FRAME_RAW : RDOF_CHZFX_FRAME_CH),
              "header %d", n);
        for (int k = 0; k < n; k++) {
            const rdof_iq16_t v = rdof_chzfx_frame_sample(&f, k);
            CHECK(v.i == x[k].i && v.q == x[k].q, "sample %d of %d", k, n);
        }
        if (n % 2)
            CHECK(w[words - 1] >> 32 == 0, "padding");
        CHECK(rdof_chzfx_unframe(w, words - 1, &f) == (n ? 0 : -1), "short %d", n);
    }
    /* the first word in memory: I of the first sample in its two low bytes (voltage0 in an IIO buffer) */
    rdof_chzfx_frame(RDOF_CHZFX_FRAME_CH, 7, x + 4, 1, w);
    CHECK((w[0] >> 48) == RDOF_CHZFX_MAGIC && (uint32_t)w[0] == 7 && (w[0] >> 32 & 0xFFF) == 1, "header bits");
    CHECK((int16_t)(w[1] & 0xFFFF) == -12345 && (int16_t)(w[1] >> 16 & 0xFFFF) == 23456, "sample bits");
    w[0] ^= 1ull << 63;
    rdof_chzfx_frame_t f;
    CHECK(rdof_chzfx_unframe(w, 2, &f) == -1, "bad magic");
    w[0] = (uint64_t)RDOF_CHZFX_MAGIC << 48 | 5ull << 44;
    CHECK(rdof_chzfx_unframe(w, 2, &f) == -1, "bad type");
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--write")) {
        if (write_vectors() != 0) {
            printf("FAIL writing the vectors under %s\n", RDOF_CHZFX_DATA);
            return 1;
        }
        return 0;
    }
    test_table();
    test_cfg();
    test_frames();
    for (int t = 0; t < NCASES; t++)
        test_case(&cases[t]);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
