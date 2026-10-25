// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Host tests for sdr against golden vectors from the unpublished reference
 * (sdr_vectors.h and sdr-data/, under fixtures/; see fixtures/README.md).
 *
 * Exact: IQ conversion, the tuner plan, every demod burst (position, type,
 * slot, family, color code, error count, the 33 bytes), the tracker JSON end
 * to end, the AGC decisions. Within a tolerance (printed): the FFT, the
 * filters, the channelizer and discriminator outputs, burst power and levels.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/sdr/agc.h"
#include "rdof/sdr/channelizer.h"
#include "rdof/sdr/config.h"
#include "rdof/sdr/demod.h"
#include "rdof/sdr/fft.h"
#include "rdof/sdr/fm.h"
#include "rdof/sdr/frontend.h"
#include "rdof/sdr/iq.h"
#include "rdof/sdr/ppm.h"
#include "netclock_tempcal.h"
#include "rdof/sdr/rate.h"
#include "rdof/sdr/receiver.h"
#include "rdof/sdr/resample.h"
#include "rdof/sdr/spectrum.h"
#include "rdof/sdr/status.h"
#include "sdr_vectors.h"

#ifndef RDOF_SDR_DATA
#define RDOF_SDR_DATA "sdr-data/"
#endif

/* Tolerances for the float stages (what the C must stay within; the run prints what it got). */
#define TOL_FFT_REL 2e-6        /* max |error| / max |X| */
#define TOL_TAPS_REL 1e-12      /* double-precision filter design */
#define TOL_H_REL 1e-6          /* channel filter bins, float */
#define TOL_CHZ_REL 5e-5        /* channelizer: max |error| / RMS of the output (the reference casts its NCO phase to float32) */
#define TOL_FM_HZ 2.0           /* discriminator + RRC (fast atan2 is good to ~1e-4 rad = 0.8 Hz) */
#define TOL_POW_REL 1e-5
#define TOL_BURST_DB 0.01       /* burst power */
#define TOL_LEVEL_HZ 1.5        /* burst 4FSK center and deviation (the float32 recentering differs by up to 1.4 Hz) */
#define TOL_AFC_HZ 1.0          /* AFC correction after the end-to-end run */

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

static void *load(const char *name, size_t *size)
{
    char path[512];
    snprintf(path, sizeof path, "%s%s", RDOF_SDR_DATA, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("FAIL cannot open %s\n", path);
        failures++;
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *p = malloc((size_t)n + 1);
    if (fread(p, 1, (size_t)n, f) != (size_t)n)
        n = 0;
    fclose(f);
    *size = (size_t)n;
    return p;
}

static rdof_cf_t *load_cu8(const char *name, int *n)
{
    size_t size;
    uint8_t *raw = load(name, &size);
    *n = (int)(size / 2);
    rdof_cf_t *x = malloc(sizeof(rdof_cf_t) * (size_t)(*n + 1));
    rdof_iq_from_cu8(raw, (size_t)*n, x);
    free(raw);
    return x;
}

static void tohex(const uint8_t *b, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++)
        snprintf(out + 2 * i, 3, "%02x", b[i]);
    out[2 * n] = 0;
}

static size_t unhex(const char *s, uint8_t *out)
{
    size_t n = strlen(s) / 2;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(s + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
    return n;
}

/* ── an external FFT, for the build without the built-in one ─────────────── */

#if !RDOF_FEATURE_SDR_BUILTIN_FFT
/* A plain DFT with a twiddle table: slow, but enough to show the hook works. */
struct rdof_fft {
    int n;
    rdof_cf_t *tw;
};

rdof_fft_t *rdof_fft_new(int n, bool inverse)
{
    rdof_fft_t *f = malloc(sizeof *f);
    f->n = n;
    f->tw = malloc(sizeof(rdof_cf_t) * n);
    for (int i = 0; i < n; i++) {
        double ph = (inverse ? 2.0 : -2.0) * 3.14159265358979323846 * i / n;
        f->tw[i].re = (float)cos(ph);
        f->tw[i].im = (float)sin(ph);
    }
    return f;
}

void rdof_fft_free(rdof_fft_t *f)
{
    if (f) {
        free(f->tw);
        free(f);
    }
}

void rdof_fft_run(rdof_fft_t *f, const rdof_cf_t *in, rdof_cf_t *out)
{
    for (int k = 0; k < f->n; k++) {
        double re = 0, im = 0;
        for (int j = 0, m = 0; j < f->n; j++, m = (m + k) % f->n) {
            re += (double)in[j].re * f->tw[m].re - (double)in[j].im * f->tw[m].im;
            im += (double)in[j].re * f->tw[m].im + (double)in[j].im * f->tw[m].re;
        }
        out[k].re = (float)re;
        out[k].im = (float)im;
    }
}
#endif

/* ── tests ─────────────────────────────────────────────────────────────── */

static void test_iq(void)
{
    uint8_t u[256];
    int8_t s[256];
    for (int i = 0; i < 256; i++) {
        u[i] = (uint8_t)i;
        s[i] = (int8_t)(uint8_t)i;
    }
    rdof_cf_t a[128], b[128];
    rdof_iq_from_cu8(u, 128, a);
    rdof_iq_from_cs8(s, 128, b);
    int bad_u = 0, bad_s = 0;
    for (int i = 0; i < 128; i++) {
        bad_u += a[i].re != v_iq_cu8[2 * i] || a[i].im != v_iq_cu8[2 * i + 1];
        bad_s += b[i].re != v_iq_cs8[2 * i] || b[i].im != v_iq_cs8[2 * i + 1];
    }
    CHECK(bad_u == 0, "cu8: %d samples differ", bad_u);
    CHECK(bad_s == 0, "cs8: %d samples differ", bad_s);
}

static void fft_input(int n, rdof_cf_t *x)
{
    for (int i = 0; i < n; i++) {
        x[i].re = (float)((((long)i * 7919) % 255 - 127) / 128.0);
        x[i].im = (float)((((long)i * 104729) % 251 - 125) / 128.0);
    }
}

static void test_fft(void)
{
    static const int sizes[] = {1, 2, 3, 4, 5, 8, 12, 60, 64, 384, 2048, 3200};
    double worst = 0.0;
    for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        int n = sizes[s];
        rdof_cf_t *x = malloc(sizeof(rdof_cf_t) * n), *X = malloc(sizeof(rdof_cf_t) * n), *back = malloc(sizeof(rdof_cf_t) * n);
        fft_input(n, x);
        rdof_fft_t *f = rdof_fft_new(n, false), *inv = rdof_fft_new(n, true);
        CHECK(f && inv, "fft %d: no plan", n);
        if (!f || !inv)
            continue;
        rdof_fft_run(f, x, X);
        double maxref = 0.0, maxerr = 0.0;
        for (size_t i = 0; i < V_FFT_N; i++)
            if (v_fft[i].n == n)
                maxref = fmax(maxref, hypot(v_fft[i].re, v_fft[i].im));
        for (size_t i = 0; i < V_FFT_N; i++)
            if (v_fft[i].n == n)
                maxerr = fmax(maxerr, hypot(X[v_fft[i].k].re - v_fft[i].re, X[v_fft[i].k].im - v_fft[i].im));
        double rel = maxerr / maxref;
        worst = fmax(worst, rel);
        CHECK(rel < TOL_FFT_REL, "fft %d: relative error %.3g", n, rel);
        /* inverse round trip */
        rdof_fft_run(inv, X, back);
        double rt = 0.0;
        for (int i = 0; i < n; i++)
            rt = fmax(rt, hypot(back[i].re / n - x[i].re, back[i].im / n - x[i].im));
        CHECK(rt < 1e-5, "fft %d: round trip error %.3g", n, rt);
        rdof_fft_free(f);
        rdof_fft_free(inv);
        free(x);
        free(X);
        free(back);
    }
#if RDOF_FEATURE_SDR_BUILTIN_FFT
    CHECK(rdof_fft_new(7, false) == NULL, "size 7 should be refused");
#endif
    printf("  fft: worst relative error %.2g (tolerance %.0g)\n", worst, TOL_FFT_REL);
}

static void test_filters(void)
{
    double h[RDOF_FM_TAPS];
    CHECK(rdof_rrc_taps(10, 0.2, 8, h) == RDOF_FM_TAPS, "rrc length");
    double e = 0.0;
    for (int i = 0; i < RDOF_FM_TAPS; i++)
        e = fmax(e, fabs(h[i] - v_rrc[i]));
    CHECK(e < TOL_TAPS_REL, "rrc taps off by %.3g", e);
    rdof_fm_t fm;
    rdof_fm_init(&fm, 48000.0);
    int ulp = 0;
    for (int i = 0; i < RDOF_FM_TAPS; i++)
        ulp += fm.h[i] != v_fm_h[i] && fabsf(fm.h[i] - v_fm_h[i]) > 1.2e-7f * fabsf(v_fm_h[i]);
    CHECK(ulp == 0, "fm taps: %d more than 1 ulp off", ulp);
    CHECK(fm.symmetric, "fm taps should be symmetric");

    double lp[513];
    rdof_chz_lowpass(513, 7500.0, 1.536e6, lp);
    double lmax = 0.0, lerr = 0.0;
    for (int i = 0; i < 513; i += 8) {
        lmax = fmax(lmax, fabs(v_lowpass[i / 8]));
        lerr = fmax(lerr, fabs(lp[i] - v_lowpass[i / 8]));
    }
    CHECK(lerr / lmax < TOL_TAPS_REL, "lowpass off by %.3g", lerr / lmax);

    static const double rates[] = {1.536e6, 288e3, 2.4e6};
    for (int r = 0; r < 3; r++) {
        rdof_chz_t s;
        double off = 0.0;
        int err = rdof_chz_init(&s, rates[r], &off, 1, (int)(rates[r] / 48000), RDOF_CHZ_OUT_FFT, RDOF_CHZ_CUTOFF_HZ);
        CHECK(err == 0, "chz init at %g", rates[r]);
        if (err) {
            rdof_chz_free(&s);
            continue;
        }
        double maxh = 0.0, maxe = 0.0;
        for (size_t i = 0; i < V_CHZ_H_N; i++) {
            if (v_chz_h[i].fs != rates[r])
                continue;
            int j = v_chz_h[i].j;
            maxh = fmax(maxh, hypot(v_chz_h[i].re, v_chz_h[i].im));
            maxe = fmax(maxe, hypot(s.H[j].re * s.M - v_chz_h[i].re, s.H[j].im * s.M - v_chz_h[i].im));
        }
        CHECK(maxe / maxh < TOL_H_REL, "channel filter at %g off by %.3g", rates[r], maxe / maxh);
        rdof_chz_free(&s);
    }
}

static void test_plan(void)
{
    for (size_t i = 0; i < V_PLAN_N; i++) {
        const struct v_plan *p = &v_plan[i];
        double c = 0.0;
        int err = rdof_plan_center(p->freqs, p->n, p->fs, &c);
        CHECK((err == 0) == (p->ok == 1), "plan %s: ok %d", p->name, err == 0);
        if (p->ok)
            CHECK(c == p->center, "plan %s: center %.1f, want %.1f", p->name, c, p->center);
    }
}

static void test_channelizer(void)
{
    for (size_t t = 0; t < V_CHZ_N; t++) {
        const struct v_chz *v = &v_chz[t];
        char name[128];
        int n_in;
        snprintf(name, sizeof name, "%s_in.cu8", v->name);
        rdof_cf_t *x = load_cu8(name, &n_in);
        size_t size;
        snprintf(name, sizeof name, "%s_out.cf32", v->name);
        rdof_cf_t *want = load(name, &size);
        CHECK(size == sizeof(rdof_cf_t) * (size_t)v->nch * v->n_out, "%s: expected output size", v->name);
        rdof_chz_t s;
        int err = rdof_chz_init(&s, v->fs, v->offsets, v->nch, (int)(v->fs / 48000), RDOF_CHZ_OUT_FFT, RDOF_CHZ_CUTOFF_HZ);
        CHECK(err == 0, "%s: init", v->name);
        const int stride = v->n_out + 64;
        rdof_cf_t *y = calloc((size_t)stride * v->nch, sizeof(rdof_cf_t));
        int used = 0, produced = 0;
        for (int k = 0; k < v->nchunks; k++) {
            if (k == 1)
                for (int c = 0; c < v->nch; c++)
                    s.correction_hz[c] = v->corr[c];
            /* outputs land per channel at the running offset */
            int want_n = rdof_chz_out_len(&s, v->chunks[k]);
            rdof_cf_t *tmp = malloc(sizeof(rdof_cf_t) * (size_t)(want_n + 1) * v->nch);
            int got = rdof_chz_process(&s, x + used, v->chunks[k], tmp, want_n + 1);
            CHECK(got == want_n, "%s chunk %d: %d outputs, out_len said %d", v->name, k, got, want_n);
            for (int c = 0; c < v->nch; c++)
                memcpy(y + (size_t)c * stride + produced, tmp + (size_t)c * (want_n + 1), sizeof(rdof_cf_t) * got);
            free(tmp);
            produced += got;
            used += v->chunks[k];
        }
        CHECK(produced == v->n_out, "%s: %d outputs, want %d", v->name, produced, v->n_out);
        double rms = 0.0, maxerr = 0.0;
        for (int c = 0; c < v->nch; c++)
            for (int i = 0; i < v->n_out && i < produced; i++) {
                rdof_cf_t w = want[(size_t)c * v->n_out + i], g = y[(size_t)c * stride + i];
                rms += (double)w.re * w.re + (double)w.im * w.im;
                maxerr = fmax(maxerr, hypot(g.re - w.re, g.im - w.im));
            }
        rms = sqrt(rms / ((double)v->nch * v->n_out));
        CHECK(maxerr / rms < TOL_CHZ_REL, "%s: max error %.3g of RMS %.3g", v->name, maxerr, rms);
        printf("  %s: max error %.2g of output RMS (tolerance %.0g)\n", v->name, maxerr / rms, TOL_CHZ_REL);
        rdof_chz_free(&s);
        free(y);
        free(x);
        free(want);
    }
}

static void test_fm(void)
{
    int n;
    rdof_cf_t *x = load_cu8("demod_bs_slot2_noisy.cu8", &n);
    size_t size;
    float *want = load("fm_out.f32", &size);
    CHECK(size == sizeof(float) * 2 * V_FM_SAMPLES, "fm_out size");
    rdof_fm_t fm;
    rdof_fm_init(&fm, 48000.0);
    float *f = malloc(sizeof(float) * V_FM_SAMPLES), *p = malloc(sizeof(float) * V_FM_SAMPLES);
    static const int cuts[] = {0, 100, 5100, V_FM_SAMPLES};
    for (int k = 0; k < 3; k++)
        rdof_fm_process(&fm, x + cuts[k], cuts[k + 1] - cuts[k], f + cuts[k], p + cuts[k]);
    /* Where two samples in a row point exactly opposite ways (quantized noise), the phase step is
     * exactly pi and its sign is down to rounding: the reference's FMA-contracted multiply gives -pi where the
     * exact product gives +pi. Outputs whose filter span covers such a step are left out. */
    float *raw = malloc(sizeof(float) * V_FM_SAMPLES);
    rdof_cf_t prev = {0, 0};
    rdof_fm_discriminate(&prev, fm.scale, x, V_FM_SAMPLES, raw);
    int skipped = 0, last_pi = -1000000;
    double ferr = 0.0, perr = 0.0, pmax = 0.0;
    for (int i = 0; i < V_FM_SAMPLES; i++) {
        if (i) { /* x[i] and x[i-1] exactly opposite in direction */
            double cross = (double)x[i].im * x[i - 1].re - (double)x[i].re * x[i - 1].im;
            double dot = (double)x[i].re * x[i - 1].re + (double)x[i].im * x[i - 1].im;
            if (cross == 0.0 && dot < 0.0)
                last_pi = i;
        }
        if (i - last_pi <= RDOF_FM_TAPS - 1 && fabs(f[i] - want[i]) >= TOL_FM_HZ) {
            skipped++;
            continue;
        }
        ferr = fmax(ferr, fabs(f[i] - want[i]));
        perr = fmax(perr, fabs(p[i] - want[V_FM_SAMPLES + i]));
        pmax = fmax(pmax, want[V_FM_SAMPLES + i]);
    }
    CHECK(ferr < TOL_FM_HZ, "fm frequency off by %.3g Hz", ferr);
    CHECK(perr / pmax < TOL_POW_REL, "fm power off by %.3g", perr / pmax);
    CHECK(skipped < V_FM_SAMPLES / 20, "fm: %d outputs left out", skipped);
    printf("  fm: frequency within %.2g Hz (tolerance %.0f; %d outputs next to an exact +-pi step left out), "
           "power within %.2g relative\n", ferr, TOL_FM_HZ, skipped, perr / pmax);
    rdof_fm_free(&fm);
    free(raw);
    free(f);
    free(p);
    free(x);
    free(want);
}

static void test_demod(void)
{
    double worst_db = 0.0, worst_hz = 0.0;
    int total = 0;
    for (size_t t = 0; t < V_DEMOD_N; t++) {
        const struct v_demod *v = &v_demod[t];
        char name[128];
        int n;
        snprintf(name, sizeof name, "demod_%s.cu8", v->name);
        rdof_cf_t *x = load_cu8(name, &n);
        CHECK(n == v->samples, "%s: %d samples", v->name, n);
        rdof_demod_t d;
        CHECK(rdof_demod_init(&d, v->cc, v->default_slot, RDOF_DEMOD_SQUELCH_HZ, v->chunk) == 0, "init");
        int k = 0;
        for (int i = 0; i < n; i += v->chunk) {
            int m = n - i < v->chunk ? n - i : v->chunk, nb;
            const rdof_sdr_burst_t *bs = rdof_demod_feed(&d, x + i, m, &nb);
            for (int j = 0; j < nb; j++, k++) {
                const rdof_sdr_burst_t *b = &bs[j];
                if (k >= v->count) {
                    CHECK(0, "%s: extra burst at %lld dt %d", v->name, (long long)b->position, b->b.dt);
                    continue;
                }
                const struct v_dburst *w = &v_dbursts[v->first + k];
                char hex[67];
                tohex(b->b.burst, 33, hex);
                bool same = b->position == w->position && b->b.dt == w->dt && b->b.n == w->n && b->b.slot == w->slot &&
                            (int)b->b.family == w->family && b->b.cc == w->cc && b->b.sync_errors == w->sync_errors &&
                            strcmp(hex, w->burst) == 0;
                CHECK(same, "%s burst %d: got pos %lld dt %d n %d slot %d fam %d cc %d err %d, want pos %lld dt %d n %d "
                      "slot %d fam %d cc %d err %d%s",
                      v->name, k, (long long)b->position, b->b.dt, b->b.n, b->b.slot, b->b.family, b->b.cc,
                      b->b.sync_errors, w->position, w->dt, w->n, w->slot, w->family, w->cc, w->sync_errors,
                      strcmp(hex, w->burst) ? " (bytes differ)" : "");
                double ddb = fabs(b->b.power_dbfs - w->power);
                double dhz = fmax(fabs(b->freq_offset_hz - w->freq_offset), fabs(b->deviation_hz - w->deviation));
                worst_db = fmax(worst_db, ddb);
                worst_hz = fmax(worst_hz, dhz);
                CHECK(ddb < TOL_BURST_DB, "%s burst %d: power %.4f, want %.4f", v->name, k, b->b.power_dbfs, w->power);
                CHECK(dhz < TOL_LEVEL_HZ, "%s burst %d: levels %.2f / %.2f, want %.2f / %.2f", v->name, k,
                      b->freq_offset_hz, b->deviation_hz, w->freq_offset, w->deviation);
            }
        }
        CHECK(k == v->count, "%s: %d bursts, want %d", v->name, k, v->count);
        total += k;
        rdof_demod_free(&d);
        free(x);
    }
    printf("  demod: %d bursts in %d cases; power within %.2g dB, levels within %.2g Hz\n", total, (int)V_DEMOD_N,
           worst_db, worst_hz);
}

#if RDOF_FEATURE_SDR_RECEIVER
struct e2e {
    int next, bad;
};

static void e2e_out(struct e2e *c, const char *ch, const char *kind, const char *text)
{
    checks++;
    if (c->next >= (int)V_E2E_OUTS_N) {
        failures++;
        if (c->bad++ < 3)
            printf("FAIL e2e: extra output %s %s %s\n", ch, kind, text);
        c->next++;
        return;
    }
    const struct v_e2e_out *o = &v_e2e_outs[c->next++];
    if (strcmp(o->channel, ch) || strcmp(o->kind, kind) || strcmp(o->text, text)) {
        failures++;
        if (c->bad++ < 3)
            printf("FAIL e2e output %d:\n  want %s %s %s\n  got  %s %s %s\n", c->next - 1, o->channel, o->kind, o->text,
                   ch, kind, text);
    }
}

static void e2e_json(void *ctx, const char *ch, const char *json) { e2e_out(ctx, ch, "json", json); }
static void e2e_raw(void *ctx, const char *ch, const char *json) { e2e_out(ctx, ch, "raw", json); }

static void test_e2e(void)
{
    int n;
    rdof_cf_t *x = load_cu8("e2e_288k.cu8", &n);
    rdof_sdr_channel_t ch[8];
    for (size_t i = 0; i < V_E2E_CHANS_N; i++) {
        ch[i].id = v_e2e_chans[i].id;
        ch[i].freq_hz = v_e2e_chans[i].freq;
        ch[i].cc = v_e2e_chans[i].cc;
        ch[i].slot = v_e2e_chans[i].slot;
    }
    rdof_sdr_rx_config_t cfg = {288e3, 0.0, V_E2E_RSSI_OFFSET, 0.0, true, V_E2E_CHUNK};
    struct e2e cap = {0, 0};
    rdof_sink_t sink = {.on_json = e2e_json, .on_raw = e2e_raw, .ctx = &cap};
    static rdof_sdr_rx_t rx;
    CHECK(rdof_sdr_rx_init(&rx, &cfg, ch, (int)V_E2E_CHANS_N, &sink, V_E2E_T0) == 0, "e2e init");
    CHECK(rx.center_hz == V_E2E_CENTER, "e2e center %.1f", rx.center_hz);
    for (int i = 0; i < n; i += V_E2E_CHUNK)
        rdof_sdr_rx_process(&rx, x + i, n - i < V_E2E_CHUNK ? n - i : V_E2E_CHUNK);
    CHECK(cap.next == (int)V_E2E_OUTS_N, "e2e: %d outputs, want %d", cap.next, (int)V_E2E_OUTS_N);
    double worst = 0.0;
    for (size_t i = 0; i < V_E2E_CHANS_N; i++) {
        double e = fabs(rx.chz.correction_hz[i] - v_e2e_chans[i].afc);
        worst = fmax(worst, e);
        CHECK(e < TOL_AFC_HZ, "e2e %s: AFC %.2f Hz, want %.2f", ch[i].id, rx.chz.correction_hz[i], v_e2e_chans[i].afc);
    }
    printf("  e2e: %d JSON / raw messages on %d channels match; AFC within %.2g Hz\n", cap.next, (int)V_E2E_CHANS_N,
           worst);

    /* the status message after the run, with the generator's fixed extras (and the reference's AFC
     * values, which only match within a tolerance, so the rounding to 0.1 Hz is the same) */
    for (size_t i = 0; i < V_E2E_CHANS_N; i++)
        rx.chz.correction_hz[i] = v_e2e_chans[i].afc;
    rdof_level_stats_t ls = {0};
    rdof_level_stats_add(&ls, 131072, 0.5, 0.0004);
    rdof_level_stats_add(&ls, 131072, 0.8125, 0.0);
    static const char *const hb_ids[] = {"SDR04", "SDR06"};
    static const bool hb_up[] = {true, false};
    rdof_sdr_status_t st = {.name = "e2e-288k", .at = V_E2E_T0 + 1.25, .cpu_load = 3.21 / 12.5, .overflows = 7,
                          .has_level = true, .has_gain = true, .gain_db = 28.04, .has_agc = true,
                          .agc_ceiling_db = 28.0, .agc_min_db = 0.0, .agc_changes = 2, .n_hblink = 2,
                          .hblink_ids = hb_ids, .hblink_up = hb_up};
    rdof_level_stats_take(&ls, &st.clip_pct, &st.peak_dbfs);
    static char sj[4096];
    size_t len = rdof_sdr_status_json(&rx, &st, sj, sizeof sj);
    CHECK(len > 0 && strcmp(sj, V_STATUS_JSON) == 0, "status:\n  got  %s\n  want %s", sj, V_STATUS_JSON);
    rdof_location_t loc = {31.793, -74.0340002, 12};
    for (size_t i = 0; i < V_E2E_CHANS_N; i++) {
        char rj[512];
        rdof_sdr_receiver_json(ch[i].id, ch[i].freq_hz, ch[i].cc, "e2e-288k", 204 + 2 * (long)i, i % 2 ? &loc : NULL, rj,
                             sizeof rj);
        CHECK(strcmp(rj, v_receiver_json[i]) == 0, "receiver %zu:\n  got  %s\n  want %s", i, rj, v_receiver_json[i]);
    }
    rdof_sdr_rx_free(&rx);
    free(x);
}
#endif

static void test_agc(void)
{
    for (size_t i = 0; i < V_MEASURE_N; i++) {
        uint8_t raw[1024];
        size_t nb = unhex(v_measure[i].cu8, raw);
        rdof_cf_t x[512];
        rdof_iq_from_cu8(raw, nb / 2, x);
        double peak, clipped, peak8, clipped8;
        rdof_agc_measure(x, nb / 2, &peak, &clipped);
        rdof_agc_measure_cu8(raw, nb / 2, &peak8, &clipped8);
        CHECK(peak == v_measure[i].peak && clipped == v_measure[i].clipped, "measure %zu: %g %g, want %g %g", i, peak,
              clipped, v_measure[i].peak, v_measure[i].clipped);
        CHECK(peak8 == v_measure[i].peak && clipped8 == v_measure[i].clipped, "measure_cu8 %zu: %g %g", i, peak8,
              clipped8);
    }
#if RDOF_FEATURE_SDR_AGC
    for (size_t s = 0; s < V_AGC_N; s++) {
        const struct v_agc *v = &v_agc[s];
        rdof_agc_config_t c = {v->cfg[0], v->cfg[1], v->cfg[2], v->cfg[3], v->cfg[4], v->cfg[5]};
        static rdof_agc_t a;
        rdof_agc_init(&a, &c, v_agc_steps, (int)V_AGC_STEPS_N, v->max_gain, NAN);
        int idx = 0, ev = 0;
        for (int g = 0; g < v->nseg; g++) {
            const double *sg = v->seg[g];
            for (int k = 0;; k++) {
                double t = sg[0] + k * sg[2];
                if (t >= sg[1])
                    break;
                double ng;
                if (rdof_agc_update(&a, sg[3], sg[4], t, &ng)) {
                    const struct v_agc_event *w = ev < v->count ? &v_agc_events[v->first + ev] : NULL;
                    CHECK(w && w->index == idx && w->gain == ng && w->ceiling == a.ceiling,
                          "agc %s: change at %d to %.1f (ceiling %.1f), want %d to %.1f", v->name, idx, ng, a.ceiling,
                          w ? w->index : -1, w ? w->gain : 0.0);
                    ev++;
                }
                idx++;
            }
        }
        CHECK(ev == v->count && idx == v->updates, "agc %s: %d changes in %d updates, want %d in %d", v->name, ev, idx,
              v->count, v->updates);
        CHECK(a.gain == v->gain && a.ceiling == v->ceiling && a.changes == v->changes,
              "agc %s: end gain %.1f ceiling %.1f changes %d", v->name, a.gain, a.ceiling, a.changes);
    }
#endif
    rdof_level_stats_t ls = {0};
    rdof_level_stats_add(&ls, 1000, 0.5, 0.001);
    rdof_level_stats_add(&ls, 1000, 0.25, 0.0);
    double pct, dbfs;
    rdof_level_stats_take(&ls, &pct, &dbfs);
    CHECK(pct == 0.05 && dbfs == -6.0, "level stats %g %g", pct, dbfs);
    rdof_level_stats_take(&ls, &pct, &dbfs);
    CHECK(isnan(pct) && isnan(dbfs), "empty level stats");
}

/* ── hooks and the window planner ─────────────────────────────────────── */

static int n_alloc, n_free;
static void *count_alloc(size_t n)
{
    n_alloc++;
    return malloc(n);
}
static void count_free(void *p)
{
    n_free++;
    free(p);
}

static void test_allocator(void)
{
    rdof_sdr_set_allocator(count_alloc, count_free);
    rdof_demod_t d;
    rdof_chz_t z;
    double off[2] = {-50e3, 60e3};
    rdof_demod_init(&d, 1, 1, RDOF_DEMOD_SQUELCH_HZ, 4096);
    rdof_chz_init(&z, 288e3, off, 2, 6, RDOF_CHZ_OUT_FFT, RDOF_CHZ_CUTOFF_HZ);
    rdof_chz_free(&z);
    rdof_demod_free(&d);
    rdof_sdr_set_allocator(NULL, NULL);
    CHECK(n_alloc > 10 && n_alloc == n_free, "allocator: %d allocations, %d frees", n_alloc, n_free);
}

#if RDOF_FEATURE_SDR_BUILTIN_FFT
/* A stand-in "esp-dsp": a plain DFT for powers of two up to 64, declining everything else. */
struct dft_plan {
    int n;
    bool inv;
};
static int dft_runs;
static void *dft_create(int n, bool inverse)
{
    if (n > 64 || (n & (n - 1)))
        return NULL;
    struct dft_plan *p = malloc(sizeof *p);
    p->n = n;
    p->inv = inverse;
    return p;
}
static void dft_run(void *plan, const rdof_cf_t *in, rdof_cf_t *out)
{
    const struct dft_plan *p = plan;
    dft_runs++;
    for (int k = 0; k < p->n; k++) {
        double re = 0, im = 0;
        for (int j = 0; j < p->n; j++) {
            double ph = (p->inv ? 2.0 : -2.0) * 3.14159265358979323846 * ((long)j * k % p->n) / p->n;
            re += in[j].re * cos(ph) - in[j].im * sin(ph);
            im += in[j].re * sin(ph) + in[j].im * cos(ph);
        }
        out[k].re = (float)re;
        out[k].im = (float)im;
    }
}
static void dft_destroy(void *plan) { free(plan); }
static const rdof_fft_backend_t dft_backend = {dft_create, dft_run, dft_destroy};
#endif

/* A direct-form FIR standing in for esp-dsp's dsps_fir_f32. */
struct fir_state {
    float h[RDOF_FM_TAPS], d[RDOF_FM_TAPS];
    int taps;
};
static int fir_runs;
static void *fir_create(const float *h, int taps)
{
    struct fir_state *s = calloc(1, sizeof *s);
    memcpy(s->h, h, sizeof(float) * taps);
    s->taps = taps;
    return s;
}
static void fir_run(void *state, const float *in, float *out, int n)
{
    struct fir_state *s = state;
    fir_runs++;
    for (int i = 0; i < n; i++) {
        memmove(s->d + 1, s->d, sizeof(float) * (s->taps - 1));
        s->d[0] = in[i];
        float acc = 0.0f;
        for (int k = 0; k < s->taps; k++)
            acc += s->h[k] * s->d[k];
        out[i] = acc;
    }
}
static void fir_destroy(void *state) { free(state); }
static const rdof_fir_backend_t fir_backend = {fir_create, fir_run, fir_destroy};

static void test_window(void)
{
    /* every channel fits: plan_center's answer */
    for (size_t i = 0; i < V_PLAN_N; i++) {
        const struct v_plan *p = &v_plan[i];
        if (!p->ok)
            continue;
        double c = 0;
        bool in[10];
        int k = rdof_plan_window(p->freqs, p->n, p->fs, &c, in);
        CHECK(k == p->n && c == p->center, "window %s: %d inside at %.1f, want all at %.1f", p->name, k, c, p->center);
    }
    /* the ten 461 MHz channels at 288 kS/s: no window holds more, and every one inside is usable */
    const double f[10] = {461.4625e6, 462e6, 461.0875e6, 461.1375e6, 461.1875e6, 461.2375e6, 461.2875e6,
                          461.0375e6, 461.3375e6, 461.6875e6};
    const double half = 288e3 * 0.8 / 2;
    double c = 0;
    bool in[10];
    int k = rdof_plan_window(f, 10, 288e3, &c, in);
    int best = 0;
    for (double t = 460.8e6; t < 462.2e6; t += 250.0) {
        int m = 0;
        for (int i = 0; i < 10; i++)
            m += fabs(f[i] - t) <= half && fabs(f[i] - t) >= 25e3;
        best = m > best ? m : best;
    }
    int ok = 1, count = 0;
    for (int i = 0; i < 10; i++)
        if (in[i]) {
            count++;
            ok &= fabs(f[i] - c) <= half && fabs(f[i] - c) >= 25e3;
        }
    CHECK(k == best && count == k && ok, "window at 288k: %d at %.0f (best %d)", k, c, best);
    printf("  window: the ten 461 MHz channels at 288 kS/s: %d at %.4f MHz (", k, c / 1e6);
    for (int i = 0; i < 10; i++)
        if (in[i])
            printf(" %.4f", f[i] / 1e6);
    printf(" )\n");
    /* one channel far from everything */
    const double one[1] = {467.375e6};
    k = rdof_plan_window(one, 1, 288e3, &c, in);
    CHECK(k == 1 && fabs(c - one[0]) >= 25e3 && fabs(c - one[0]) <= half, "window one: %d at %.0f", k, c);
}

static void test_config(void)
{
    static rdof_sdr_config_t cfg;
    for (size_t i = 0; i < V_CONFIGS_N; i++) {
        const struct v_config *v = &v_configs[i];
        char err[160], desc[2048];
        bool ok = rdof_sdr_config_parse(v->json, strlen(v->json), &cfg, err, sizeof err);
        CHECK(ok == (v->ok == 1), "config %s: ok %d (%s)", v->name, ok, err);
        if (ok && v->ok) {
            rdof_sdr_config_describe(&cfg, desc, sizeof desc);
            CHECK(strcmp(desc, v->describe) == 0, "config %s:\n  got  %s\n  want %s", v->name, desc, v->describe);
        }
    }
    const char *jsonc = "{ // a comment\n \"device\": {\"sample_rate\": 288000, /* block */},\n"
                        " \"channels\": [{\"id\": \"A\", \"freq\": 461.0375,},],}";
    char err[160];
    CHECK(rdof_sdr_config_parse(jsonc, strlen(jsonc), &cfg, err, sizeof err) && cfg.nch == 1, "JSONC: %s", err);
}

/* The front end's portable part: device.type, "sim", the sample formats, the settings check. */
static void test_frontend(void)
{
    static rdof_sdr_config_t cfg;
    char err[160];
    CHECK(rdof_fe_type_parse(NULL) == RDOF_FE_RTLSDR && rdof_fe_type_parse("lr2021") == RDOF_FE_LR2021 &&
              rdof_fe_type_parse("sim") == RDOF_FE_SIM && rdof_fe_type_parse("hackrf") == -1,
          "fe type parse");
    CHECK(!strcmp(rdof_fe_type_name(RDOF_FE_SX1255), "sx1255"), "fe type name");
    const char *a = "{\"device\": {\"sample_rate\": 288000}, \"channels\": [{\"id\": \"A\", \"freq\": 461.0375}]}";
    CHECK(rdof_sdr_config_parse(a, strlen(a), &cfg, err, sizeof err) && cfg.frontend == RDOF_FE_RTLSDR && !cfg.sim,
          "default front end: %s", err);
    const char *b = "{\"device\": {\"type\": \"lr2021\", \"sample_rate\": 288000}, \"channels\": [{\"id\": \"A\", "
                    "\"freq\": 461.0375}]}";
    CHECK(rdof_sdr_config_parse(b, strlen(b), &cfg, err, sizeof err) && cfg.frontend == RDOF_FE_LR2021, "lr2021: %s", err);
    const char *c = "{\"device\": {\"type\": \"lr2021\", \"sample_rate\": 288000}, \"sim\": true, \"channels\": "
                    "[{\"id\": \"A\", \"freq\": 461.0375}]}";
    CHECK(rdof_sdr_config_parse(c, strlen(c), &cfg, err, sizeof err) && cfg.frontend == RDOF_FE_SIM && cfg.sim,
          "sim: true wins: %s", err);
    const char *d = "{\"device\": {\"type\": \"sim\", \"sample_rate\": 288000}, \"channels\": [{\"id\": \"A\", "
                    "\"freq\": 461.0375}]}";
    CHECK(rdof_sdr_config_parse(d, strlen(d), &cfg, err, sizeof err) && cfg.frontend == RDOF_FE_SIM && cfg.sim,
          "type sim is sim: %s", err);
    const char *e = "{\"device\": {\"type\": 3, \"sample_rate\": 288000}, \"channels\": [{\"id\": \"A\", "
                    "\"freq\": 461.0375}]}";
    CHECK(!rdof_sdr_config_parse(e, strlen(e), &cfg, err, sizeof err) && strstr(err, "device.type"), "type 3: %s", err);

    /* formats: the same floats as the iq converters, filler reads back as (near) zero */
    const uint8_t u8[4] = {0, 255, 128, 0x80};
    const int8_t s8[4] = {-128, 127, 0, 1};
    const int16_t s16[4] = {-32768, 32767, 0, 1};
    rdof_cf_t x[2], y[2];
    rdof_fe_convert(RDOF_FE_CU8, u8, 2, x);
    rdof_iq_from_cu8(u8, 2, y);
    CHECK(!memcmp(x, y, sizeof x) && rdof_fe_sample_bytes(RDOF_FE_CU8) == 2 && rdof_fe_fill_byte(RDOF_FE_CU8) == 0x80,
          "fe cu8");
    rdof_fe_convert(RDOF_FE_CS8, s8, 2, x);
    CHECK(x[0].re == -1.0f && x[1].im == 1.0f / 128 && rdof_fe_sample_bytes(RDOF_FE_CS8) == 2, "fe cs8");
    rdof_fe_convert(RDOF_FE_CS16, s16, 2, x);
    CHECK(x[0].re == -1.0f && x[0].im == 32767.0f / 32768 && x[1].im == 1.0f / 32768 &&
              rdof_fe_sample_bytes(RDOF_FE_CS16) == 4 && rdof_fe_fill_byte(RDOF_FE_CS16) == 0,
          "fe cs16");

    /* the check: a rate range, a rate list, the tuning range */
    static const double rtl_rates[] = {240000, 288000};
    rdof_fe_caps_t rtl = {.kind = "RTL-SDR", .rates = rtl_rates, .n_rates = 2, .freq_min = 24e6, .freq_max = 1766e6};
    rdof_fe_caps_t lr = {.kind = "LR2021", .rate_min = 48000, .rate_max = 192000, .freq_min = 150e6, .freq_max = 960e6};
    const double f[2] = {461.0375e6, 461.2375e6}, far[1] = {2.4e9};
    CHECK(rdof_fe_check(&rtl, 288000, f, 2, err, sizeof err) == 0, "rtl 288k: %s", err);
    CHECK(rdof_fe_check(&rtl, 1536000, f, 2, err, sizeof err) < 0 && !strcmp(err, "the RTL-SDR takes sample_rate 240000 or 288000"),
          "rtl 1.536M: %s", err);
    CHECK(rdof_fe_check(&lr, 192000, f, 2, err, sizeof err) == 0, "lr 192k: %s", err);
    CHECK(rdof_fe_check(&lr, 288000, f, 2, err, sizeof err) < 0 && strstr(err, "48000 to 192000"), "lr 288k: %s", err);
    CHECK(rdof_fe_check(&lr, 96000, far, 1, err, sizeof err) < 0 && strstr(err, "2400.00000 MHz"), "lr 2.4G: %s", err);
}

/* The rational resampler: a tone keeps its frequency and level across the usable band, aliases are gone, and
 * the block size changes nothing. */
static double tone_out(long fin, long fout, double f, int block, rdof_cf_t *keep, int *nkeep)
{
    rdof_resamp_t r;
    enum { N = 20000 };
    static rdof_cf_t x[N], y[N * 2];
    if (rdof_resamp_init(&r, fin, fout, 32, 0.8, 4096))
        return NAN;
    for (int i = 0; i < N; i++) {
        double ph = 2 * M_PI * f * i / fin;
        x[i].re = (float)(0.5 * cos(ph));
        x[i].im = (float)(0.5 * sin(ph));
    }
    int n = 0;
    for (int i = 0; i < N; i += block) {
        int m = N - i < block ? N - i : block;
        CHECK(rdof_resamp_max_out(&r, m) >= (int)((double)m * fout / fin), "max_out");
        n += rdof_resamp_run(&r, x + i, m, y + n);
    }
    rdof_resamp_free(&r);
    CHECK(abs(n - (int)((double)N * fout / fin)) <= 1, "resample count %d", n);
    /* the level of f in the output, past the filter's start-up: correlate with the expected tone */
    double re = 0, im = 0;
    int from = 1000, cnt = 0;
    for (int i = from; i < n; i++, cnt++) {
        double ph = -2 * M_PI * f * i / fout;
        re += y[i].re * cos(ph) - y[i].im * sin(ph);
        im += y[i].re * sin(ph) + y[i].im * cos(ph);
    }
    if (keep) {
        memcpy(keep, y, sizeof(rdof_cf_t) * (size_t)n);
        *nkeep = n;
    }
    return 20 * log10(hypot(re, im) / cnt / 0.5);
}

static void test_resample(void)
{
    /* 250 kS/s to 240 (24 / 25): flat to the usable +-96 kHz */
    double worst = 0;
    for (double f = -96e3; f <= 96e3; f += 8e3) {
        double db = tone_out(250000, 240000, f, 4096, NULL, NULL);
        if (fabs(db) > fabs(worst))
            worst = db;
    }
    CHECK(fabs(worst) < 0.05, "resample passband: worst %.3f dB", worst);
    /* 480 to 240 kS/s: a tone at 200 kHz would alias to -40 kHz; at 150 to +90 */
    rdof_resamp_t r;
    CHECK(rdof_resamp_init(&r, 480000, 240000, 32, 0.8, 4096) == 0 && r.L == 1 && r.M == 2, "480 / 240 is 1 / 2");
    rdof_resamp_free(&r);
    {
        /* what lands at -40 kHz and +90 kHz in the output */
        enum { N = 20000 };
        static rdof_cf_t x[N], y[N];
        const double fa[2] = {200e3, 150e3}, at[2] = {-40e3, 90e3};
        for (int t = 0; t < 2; t++) {
            rdof_resamp_init(&r, 480000, 240000, 32, 0.8, N);
            for (int i = 0; i < N; i++) {
                double ph = 2 * M_PI * fa[t] * i / 480000;
                x[i].re = (float)(0.5 * cos(ph));
                x[i].im = (float)(0.5 * sin(ph));
            }
            int n = rdof_resamp_run(&r, x, N, y);
            rdof_resamp_free(&r);
            double re = 0, im = 0;
            for (int i = 1000; i < n; i++) {
                double ph = -2 * M_PI * at[t] * i / 240000;
                re += y[i].re * cos(ph) - y[i].im * sin(ph);
                im += y[i].re * sin(ph) + y[i].im * cos(ph);
            }
            double db = 20 * log10(hypot(re, im) / (n - 1000) / 0.5 + 1e-12);
            CHECK(db < -60, "resample alias of %.0f kHz at %.0f kHz: %.1f dB", fa[t] / 1e3, at[t] / 1e3, db);
        }
    }
    /* blocks of 331 give the same samples as blocks of 4096 */
    static rdof_cf_t a[24000], b[24000];
    int na, nb;
    tone_out(250000, 240000, 37e3, 4096, a, &na);
    tone_out(250000, 240000, 37e3, 331, b, &nb);
    CHECK(na == nb && !memcmp(a, b, sizeof(rdof_cf_t) * (size_t)na), "resample blocks: %d / %d", na, nb);
    CHECK(rdof_resamp_init(&r, 1000003, 999983, 32, 0.8, 16) == -1, "a ratio too big refused");
    printf("  resample: 250 to 240 kS/s within %.3f dB to +-96 kHz\n", worst);
}

static void test_spectrum(void)
{
    /* a -20 dBFS tone 64 bins above the center, averaged over three FFTs: 40 (-20 dB x -2) at bin 256 + 64 */
    enum { N = 512 };
    rdof_spectrum_t s;
    CHECK(rdof_spectrum_init(&s, N, 3) == 0, "spectrum init");
    rdof_cf_t x[N + 100];
    uint8_t row[N];
    for (int i = 0; i < N + 100; i++) {
        double ph = 2 * M_PI * 64 * i / N;
        x[i].re = (float)(0.1 * cos(ph));
        x[i].im = (float)(0.1 * sin(ph));
    }
    bool a = rdof_spectrum_feed(&s, x, N + 100, row), b = rdof_spectrum_feed(&s, x, N, row);
    bool c = rdof_spectrum_feed(&s, x, N, row);
    CHECK(!a && !b && c, "a row every third FFT: %d %d %d", a, b, c);
    int peak = 0;
    for (int i = 1; i < N; i++)
        if (row[i] < row[peak])
            peak = i;
    const int level = row[peak];
    CHECK(peak == N / 2 + 64 && level == 40, "tone at bin %d, %d (want %d, 40)", peak, row[peak], N / 2 + 64);
    CHECK(row[N / 2 + 63] <= 52 && row[N / 2 + 65] <= 52, "Hann: the neighbors -6 dB, %d %d", row[N / 2 + 63],
          row[N / 2 + 65]);
    CHECK(row[N / 2 - 100] >= 200, "far from the tone: %d", row[N / 2 - 100]);
    /* starts over: a quiet row */
    memset(x, 0, sizeof x);
    rdof_spectrum_feed(&s, x, N, row);
    rdof_spectrum_feed(&s, x, 10, row);
    CHECK(rdof_spectrum_feed(&s, x, N, row) && row[N / 2] == 255, "silence: %d", row[N / 2]);
    rdof_spectrum_free(&s);
    /* a burst in the second half of each 8N chunk (DMR's 30 ms on, 30 off): FFTs spread over the chunk see it half
     * the time, -3 dB (6), where one at the chunk's start never would */
    static rdof_cf_t y[8 * N];
    for (int i = 0; i < 8 * N; i++) {
        double ph = 2 * M_PI * 64 * i / N, a = i >= 4 * N ? 0.1 : 0.0;
        y[i].re = (float)(a * cos(ph));
        y[i].im = (float)(a * sin(ph));
    }
    CHECK(rdof_spectrum_init(&s, N, 1) == 0, "spectrum init");
    CHECK(rdof_spectrum_feed(&s, y, 8 * N, row), "a row a chunk");
    CHECK(row[N / 2 + 64] == 46, "half-time burst: %d (want 46: -20 dBFS, -3 dB)", row[N / 2 + 64]);
    rdof_spectrum_free(&s);
    /* one FFT a chunk (the board's setting): its start turns through the chunk, so over 40 chunks it sees the burst
     * about half the time too (within 1.5 dB of -3) */
    CHECK(rdof_spectrum_init(&s, N, 40) == 0, "spectrum init");
    rdof_spectrum_set_ffts(&s, 1);
    bool done = false;
    for (int c = 0; c < 40; c++)
        done = rdof_spectrum_feed(&s, y, 8 * N, row);
    CHECK(done && s.ffts == 0 && abs(row[N / 2 + 64] - 46) <= 3, "one FFT a chunk, half-time burst: %d (want 46 +- 3)",
          row[N / 2 + 64]);
    rdof_spectrum_free(&s);
    printf("  spectrum: a -20 dBFS tone at bin %d reads %.1f dBFS; on half the chunk, %.1f\n", peak, -level / 2.0,
           -row[N / 2 + 64] / 2.0);
}

/* The discriminator's atan2 in every octant, on the axes and diagonals, over magnitudes 1e-30 to 1e3,
 * and atan2(0, 0) = 0: against atan2 in double. RDOF_SDR_FAST_ATAN2=0 (atan2f) passes too. */
static void test_atan2(void)
{
    enum { N = 4096 };
    static rdof_cf_t x[N + 8];
    static float out[N + 8];
    double err = 0.0;
    int n = 0;
    for (int i = 0; i < N; i++, n++) {
        const double th = -M_PI + 2 * M_PI * i / N, mag = pow(10.0, -30.0 + 33.0 * (i % 17) / 16.0);
        x[n].re = (float)(mag * cos(th));
        x[n].im = (float)(mag * sin(th));
    }
    x[n].re = 0.0f, x[n++].im = 0.0f;
    x[n].re = -1.0f, x[n++].im = 0.0f;
    x[n].re = 0.0f, x[n++].im = -1.0f;
    for (int i = 0; i < n; i++) {
        /* prev = 1 + 0j each time: the output is the angle of x itself */
        rdof_cf_t prev = {1.0f, 0.0f};
        rdof_fm_discriminate(&prev, 1.0f, x + i, 1, out + i);
        double want = (x[i].re == 0.0f && x[i].im == 0.0f) ? 0.0 : atan2((double)x[i].im, (double)x[i].re);
        double d = fabs(out[i] - want);
        d = fmin(d, fabs(d - 2 * M_PI)); /* -pi and pi are the same angle */
        err = fmax(err, d);
    }
    CHECK(err < 2.1e-4, "atan2 off by %.3g rad", err); /* the polynomial itself is 2.03e-4 off at 45 degrees */
    /* and two at a time (the paired loop) gives the same as one at a time */
    rdof_cf_t prev = {1.0f, 0.0f};
    static float pair[N + 8];
    rdof_fm_discriminate(&prev, 1.0f, x, n, pair);
    int same = 1;
    for (int i = 1; i < n; i++) {
        rdof_cf_t p1 = x[i - 1];
        float one;
        rdof_fm_discriminate(&p1, 1.0f, x + i, 1, &one);
        same &= one == pair[i];
    }
    CHECK(same, "atan2: the paired loop differs from one at a time");
    printf("  atan2: within %.2g rad over %d points (every octant, 1e-30 to 1e3, and 0)\n", err, n);
}

/* The tuner's error learned from the AFC: three channels' corrections (the tuner 5.3 ppm fast, which puts the
 * signals low, each transmitter a little off, one far off), looked at every 5 s. */
static void test_ppm(void)
{
    const double f[4] = {461.0375e6, 461.0875e6, 461.1375e6, 461.1875e6};
    const double radio[4] = {0.3, -0.4, 0.1, 4.0}; /* ppm of their own; the last one disagrees */
    double tuner = 5.3, age[4] = {1, 2, 3, 400}, corr[4];
    rdof_ppm_t p;
    rdof_ppm_init(&p, 0.0, NAN);
    int moved = 0;
    double t = 0, moved_at = -1;
    bool saved = false;
    double saved_at = -1;
    for (; t <= 3600; t += 5) {
        age[3] = t < 100 ? 400 : 5; /* the outlier joins after 100 s: the median keeps it out */
        for (int i = 0; i < 4; i++)
            corr[i] = f[i] * (-(tuner - p.chip) + radio[i]) * 1e-6;
        int step = rdof_ppm_update(&p, corr, f, age, 4, t);
        if (step) {
            CHECK(step == 5, "ppm: step %d, want 5", step);
            rdof_ppm_applied(&p, step);
            moved += step;
            if (moved_at < 0)
                moved_at = t;
        }
        if (rdof_ppm_should_save(&p, t)) {
            CHECK(!saved, "ppm: saved twice at %.0f s (first at %.0f s)", t, saved_at);
            rdof_ppm_saved(&p, t);
            saved = true;
            saved_at = t;
        }
    }
    CHECK(moved == 5 && moved_at >= RDOF_PPM_APPLY_S && moved_at <= RDOF_PPM_APPLY_S + 15, "ppm: moved %d at %.0f s",
          moved, moved_at);
    CHECK(fabs(rdof_ppm_learned(&p) - (5.3 - 0.1)) < 0.3, "ppm: learned %.2f, want about 5.2 (less the radios' median)",
          rdof_ppm_learned(&p));
    CHECK(saved && saved_at >= RDOF_PPM_FIRST_SAVE_S && saved_at <= RDOF_PPM_FIRST_SAVE_S + 10, "ppm: first save at %.0f s",
          saved_at);
    /* the tuner drifts 1 ppm (warming up): saved again, but not before 6 hours */
    tuner += 1.0;
    double again = -1;
    for (; t <= 30000; t += 5) {
        for (int i = 0; i < 4; i++)
            corr[i] = f[i] * (-(tuner - p.chip) + radio[i]) * 1e-6;
        int step = rdof_ppm_update(&p, corr, f, age, 4, t);
        if (step)
            rdof_ppm_applied(&p, step);
        if (rdof_ppm_should_save(&p, t) && again < 0) {
            rdof_ppm_saved(&p, t);
            again = t;
        }
    }
    CHECK(again >= saved_at + RDOF_PPM_SAVE_EVERY_S, "ppm: saved again at %.0f s, first at %.0f s", again, saved_at);
    /* one channel alone, or channels that disagree: nothing learned */
    rdof_ppm_t q;
    rdof_ppm_init(&q, 0.0, NAN);
    const double lone_age[2] = {1, 1e9}, spread[3] = {f[0] * 3e-6, f[1] * 6e-6, f[2] * 9e-6};
    const double ages3[3] = {1, 1, 1};
    for (t = 0; t < 600; t += 5) {
        CHECK(rdof_ppm_update(&q, corr, f, lone_age, 2, t) == 0, "ppm: moved on one channel");
        CHECK(rdof_ppm_update(&q, spread, f, ages3, 3, t) == 0, "ppm: moved on channels that disagree");
    }
    CHECK(isnan(rdof_ppm_learned(&q)), "ppm: learned %.2f from nothing", rdof_ppm_learned(&q));
    /* a value kept from before, loaded at boot: saved again once confident and 0.5 ppm off, without waiting */
    rdof_ppm_t r;
    rdof_ppm_init(&r, 5.0, 5.0);
    for (t = 0; t <= 1000; t += 5) {
        for (int i = 0; i < 3; i++)
            corr[i] = -f[i] * (6.0 - 5.0) * 1e-6;
        const double a3[3] = {1, 1, 1};
        rdof_ppm_update(&r, corr, f, a3, 3, t);
    }
    CHECK(rdof_ppm_should_save(&r, t), "ppm: a loaded value 1 ppm off isn't saved over");
    printf("  ppm: learned %.2f ppm, moved the RTL %d at %.0f s, saved at %.0f s and again at %.0f s\n",
           rdof_ppm_learned(&p), moved, moved_at, saved_at, again);
}

/* The sample clock timed: an RTL at 288 kS/s whose crystal is 37.2 ppm fast against the local clock, 8192-sample
 * transfers stamped late by 0.1 to 2 ms (USB frames, the task's turn) and now and then 30 ms (a busy core), and at
 * 15 minutes 512 samples the RTL dropped without saying so. Then the ppm learning taking the estimate. */
static void test_rate(void)
{
    const double fs = 288000, e = 37.2;
    rdof_rate_bin_t b;
    rdof_rate_t r;
    rdof_rate_bin_init(&b, fs);
    rdof_rate_init(&r);
    uint32_t seed = 11;
    uint64_t made = 0, sent = 0; /* samples the RTL made; those it sent */
    int points = 0;
    double at_10min = NAN, worst_after = 0;
    for (;;) {
        made += 8192;
        sent += 8192;
        if (made >= (uint64_t)(900 * fs * (1 + e * 1e-6)) && made - 8192 < (uint64_t)(900 * fs * (1 + e * 1e-6)))
            sent -= 512;
        const double t_true = made / (fs * (1 + e * 1e-6)); /* when its last sample was made, local s */
        seed = seed * 1664525u + 1013904223u;
        double late = 100 + (seed >> 8) % 1900;
        if ((seed >> 4) % 97 == 0)
            late += 30000;
        const int64_t t_us = (int64_t)(t_true * 1e6 + late) + 5000000; /* the local clock didn't start at 0 */
        double ts, xs;
        if (rdof_rate_bin_add(&b, t_us, sent, &ts, &xs)) {
            rdof_rate_point(&r, ts, xs);
            points++;
            if (isnan(at_10min) && t_true >= 600)
                at_10min = r.ppm;
            if (t_true > 1200)
                worst_after = fmax(worst_after, fabs(r.ppm - e));
        }
        if (t_true > 3600)
            break;
    }
    CHECK(r.valid && fabs(r.ppm - e) < 0.02, "rate: %.4f ppm, want %.1f", r.ppm, e);
    CHECK(fabs(at_10min - e) < 0.1, "rate: %.4f ppm after 10 min", at_10min);
    CHECK(r.steps == 1, "rate: %u restarts, want 1 (the dropped samples)", r.steps);
    CHECK(worst_after < 0.05, "rate: %.3f ppm off after the restart", worst_after);
    CHECK(r.span_s > 1700 && r.span_s <= RDOF_RATE_POINTS * RDOF_RATE_BIN_S, "rate: spans %.0f s", r.span_s);
    printf("  rate: %.4f ppm (true %.1f) +-%.4f over %.0f s, %.1f us rms, %d bins, %u restart; %.3f after 10 min\n",
           r.ppm, e, r.se_ppm, r.span_s, r.rms_us, points, r.steps, at_10min);

    /* nothing in a bin (a stall): no point for it, and no restart if nothing was lost */
    rdof_rate_bin_init(&b, fs);
    rdof_rate_init(&r);
    sent = 0;
    for (double t = 0; t < 600; t += 8192 / fs) {
        sent += 8192;
        if (t > 200 && t < 230)
            continue; /* 30 s with nothing, then all of it at once */
        double ts, xs;
        if (rdof_rate_bin_add(&b, (int64_t)(t * 1e6), sent, &ts, &xs))
            rdof_rate_point(&r, ts, xs);
    }
    CHECK(r.valid && fabs(r.ppm) < 0.05 && r.steps == 0, "rate: through a stall %.3f ppm, %u restarts", r.ppm, r.steps);

    /* the ppm learning from it: steady, moves the RTL by the whole ppm after RDOF_PPM_APPLY_S, saves later */
    rdof_ppm_t p;
    rdof_ppm_init(&p, 0.0, NAN);
    int moved = 0;
    double moved_at = -1, saved_at = -1;
    for (double t = 0; t <= 1200; t += 5) {
        const int step = rdof_ppm_measured(&p, 3.8 + 0.01 * sin(t / 50), t);
        if (step) {
            rdof_ppm_applied(&p, step);
            moved += step;
            moved_at = t;
        }
        if (rdof_ppm_should_save(&p, t) && saved_at < 0) {
            rdof_ppm_saved(&p, t);
            saved_at = t;
        }
    }
    CHECK(moved == 4 && moved_at >= RDOF_PPM_APPLY_S && moved_at <= RDOF_PPM_APPLY_S + 5 && p.measured,
          "rate: ppm moved %d at %.0f s", moved, moved_at);
    CHECK(fabs(rdof_ppm_learned(&p) - 3.8) < 0.02, "rate: learned %.2f, want 3.8", rdof_ppm_learned(&p));
    CHECK(saved_at >= RDOF_PPM_FIRST_SAVE_S && saved_at <= RDOF_PPM_FIRST_SAVE_S + 5, "rate: saved at %.0f s", saved_at);
}

/* A dongle warming up as on the bench: its rate from -3 to -2 ppm, settling with a 4 minute time constant. The
 * recent rate, sampled each minute, settles (0.2 ppm over 10 minutes) once the drift has died down, and trimming
 * the fit to that window gives the warm rate where the whole fit is pulled toward the cold one. Then a die
 * temperature warming 41 to 51 C. */
static void test_settle(void)
{
    const double e0 = -3.0, e1 = -2.0, tau = 240;
    rdof_rate_t r, whole = {0};
    nc_settle_t st;
    rdof_rate_init(&r);
    nc_settle_init(&st, 60, 600, 0.2);
    uint32_t seed = 5;
    double settled_at = -1;
    for (double t = 0; t <= 1800; t += 10) {
        seed = seed * 1664525u + 1013904223u;
        const double jitter = (seed >> 8) % 20; /* us */
        rdof_rate_point(&r, t, -(e1 * t + (e0 - e1) * tau * (1 - exp(-t / tau))) + jitter);
        double recent;
        if (rdof_rate_recent(&r, 300, &recent))
            nc_settle_add(&st, t, recent);
        if (settled_at < 0 && nc_settle_ok(&st, t)) {
            settled_at = t;
            whole = r;
            rdof_rate_trim(&r, t - 600);
            break;
        }
    }
    CHECK(settled_at >= 900 && settled_at <= 1300, "settle: the rate settled at %.0f s", settled_at);
    CHECK(fabs(r.ppm - e1) < 0.05 && fabs(r.ppm - e1) < fabs(whole.ppm - e1) / 3 && r.span_s >= 590,
          "settle: trimmed %.3f ppm over %.0f s, whole %.3f, want %.1f", r.ppm, r.span_s, whole.ppm, e1);
    printf("  settle: the rate settled at %.0f s; trimmed to the window %.3f ppm (whole fit %.3f), warm %.1f\n",
           settled_at, r.ppm, whole.ppm, e1);

    /* the die: 41 to 51 C over 8 minutes, then wandering a degree; settled once the window holds only the ramp's
     * last degree or so (the tolerance is 2 C); a gap starts over */
    nc_settle_init(&st, 60, 600, 2.0);
    double die_at = -1;
    for (double t = 0; t <= 1800; t += 5) {
        const double c = (t < 480 ? 41 + 10 * t / 480 : 51) + ((int)(t / 60) % 2);
        nc_settle_add(&st, t, c);
        if (die_at < 0 && nc_settle_ok(&st, t))
            die_at = t;
    }
    CHECK(die_at >= 480 + 420 && die_at <= 480 + 660, "settle: the die settled at %.0f s", die_at);
    CHECK(isnan(nc_settle_range(&st, 1800)) == false, "settle: covered");
    nc_settle_add(&st, 1805, NAN);
    nc_settle_add(&st, 1810, 51);
    CHECK(!nc_settle_ok(&st, 1810), "settle: a gap starts over");
}

/* The channelizer in two steps (forward, then channel ranges on two workers) gives exactly rdof_chz_process's
 * output, with the correction moving between calls and uneven call sizes. */
static void test_chz_split(void)
{
    const double fs = 288000, off[5] = {-100e3, -37.5e3, 12.5e3, 50e3, 87.5e3};
    rdof_chz_t a, b;
    CHECK(rdof_chz_init(&a, fs, off, 5, 6, RDOF_CHZ_OUT_FFT, RDOF_CHZ_CUTOFF_HZ) == 0 &&
              rdof_chz_init(&b, fs, off, 5, 6, RDOF_CHZ_OUT_FFT, RDOF_CHZ_CUTOFF_HZ) == 0 && rdof_chz_reserve(&b, 5000) == 0,
          "split channelizer init");
    const int stride = rdof_chz_out_len(&a, 5000) + a.out_per_hop;
    rdof_cf_t *x = malloc(sizeof(rdof_cf_t) * 5000), *ya = malloc(sizeof(rdof_cf_t) * 5 * stride),
            *yb = malloc(sizeof(rdof_cf_t) * 5 * stride);
    uint32_t r = 7;
    int same = 1, total = 0;
    for (int call = 0; call < 40; call++) {
        const int n = 700 + (call * 977) % 4300;
        for (int i = 0; i < n; i++) {
            r = r * 1664525u + 1013904223u;
            x[i].re = (float)(r >> 8) / 16777216.0f - 0.5f;
            r = r * 1664525u + 1013904223u;
            x[i].im = (float)(r >> 8) / 16777216.0f - 0.5f;
        }
        for (int c = 0; c < 5; c++)
            a.correction_hz[c] = b.correction_hz[c] = 30.0 * c + call;
        int ga = rdof_chz_process(&a, x, n, ya, stride), gb = rdof_chz_forward(&b, x, n);
        rdof_chz_channels(&b, 2, 5, 1, yb, stride);
        rdof_chz_channels(&b, 0, 2, 0, yb, stride);
        same &= ga == gb;
        for (int c = 0; c < 5 && ga == gb; c++) {
            same &= !memcmp(ya + (size_t)c * stride, yb + (size_t)c * stride, sizeof(rdof_cf_t) * (size_t)ga);
            same &= a.level[c] == b.level[c];
        }
        total += ga;
    }
    CHECK(same, "the two-step channelizer differs from rdof_chz_process");
    printf("channelizer split by channel: %d samples a channel, bit-exact with one step\n", total);
    rdof_chz_free(&a);
    rdof_chz_free(&b);
    free(x);
    free(ya);
    free(yb);
}

int main(void)
{
    test_chz_split();
    test_ppm();
    test_rate();
    test_settle();
    test_atan2();
    test_iq();
    test_fft();
    test_filters();
    test_plan();
    test_channelizer();
    test_fm();
    test_demod();
#if RDOF_FEATURE_SDR_RECEIVER
    test_e2e();
#endif
    test_agc();
    test_window();
    test_config();
    test_frontend();
    test_resample();
    test_allocator();
    test_spectrum();
#if RDOF_FEATURE_SDR_BUILTIN_FFT
    rdof_fft_set_backend(&dft_backend);
    test_channelizer();
    rdof_fft_set_backend(NULL);
    CHECK(dft_runs > 0, "the FFT backend wasn't used");
    printf("  (channelizer again with the 64-point IFFTs on a backend: %d runs)\n", dft_runs);
#endif
    rdof_fm_set_fir_backend(&fir_backend);
    test_fm();
    test_demod();
    rdof_fm_set_fir_backend(NULL);
    CHECK(fir_runs > 0, "the FIR backend wasn't used");
    printf("  (discriminator and demod again with the matched filter on a backend: %d runs)\n", fir_runs);
    printf("%d checks, %d failures (features: agc %d, builtin fft %d, receiver %d, fast atan2 %d)\n", checks, failures,
           RDOF_FEATURE_SDR_AGC, RDOF_FEATURE_SDR_BUILTIN_FFT, RDOF_FEATURE_SDR_RECEIVER, RDOF_SDR_FAST_ATAN2);
    return failures ? 1 : 0;
}
