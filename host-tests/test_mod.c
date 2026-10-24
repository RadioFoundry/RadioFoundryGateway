// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Host tests for sdr's DMR modulator (mod.c) against golden vectors
 * (mod_vectors.h and mod-data/, under fixtures/), and round trips through the C receiver: the C
 * modulator's output, through the channelizer where the rate needs it, into
 * the C demod gives the same bursts at the same sample positions.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rdof/sdr/channelizer.h"
#include "rdof/sdr/demod.h"
#include "rdof/sdr/mod.h"
#include "mod_vectors.h"

#ifndef RDOF_MOD_DATA
#define RDOF_MOD_DATA "mod-data/"
#endif

#define TOL_IQ 1e-4       /* |C - reference| per sample (float shaping, the table's sin / cos, phase rounding) */
#define TOL_HZ 0.05       /* frequency output */
#define TOL_ENV 1e-5

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
    snprintf(path, sizeof path, "%s%s", RDOF_MOD_DATA, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("FAIL cannot open %s\n", path);
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

static void tohex(const uint8_t *b, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++)
        snprintf(out + 2 * i, 3, "%02x", b[i]);
    out[2 * n] = 0;
}

static int init_case(rdof_mod_t *m, const struct v_mod *v, int queue)
{
    return rdof_mod_init(m, v->fs, v->offset, v->ramp, (float)v->amplitude, queue, 0);
}

static int queue_seg(rdof_mod_t *m, const struct v_mod_seg *s)
{
    return rdof_mod_queue(m, s->start, s->sym, s->first, s->count, (unsigned)s->flags);
}

/* The case's output from C, pulled in uneven chunks. */
static rdof_cf_t *run_iq(const struct v_mod *v)
{
    rdof_mod_t m;
    CHECK(init_case(&m, v, v->nseg) == 0, "%s: init", v->name);
    for (int i = 0; i < v->nseg; i++)
        CHECK(queue_seg(&m, &v_mod_segs[v->first_seg + i]) == 0, "%s: queue %d", v->name, i);
    rdof_cf_t *x = malloc(sizeof(rdof_cf_t) * (size_t)v->n);
    static const int chunks[] = {1, 7, 256, 1000, 4097, 333};
    for (int done = 0, c = 0; done < v->n; c++) {
        int k = chunks[c % 6] < v->n - done ? chunks[c % 6] : v->n - done;
        rdof_mod_iq(&m, x + done, k);
        done += k;
    }
    CHECK(rdof_mod_idle(&m), "%s: idle at the end", v->name);
    rdof_mod_free(&m);
    return x;
}

static void test_vectors(void)
{
    for (int t = 0; t < V_MODS_N; t++) {
        const struct v_mod *v = &v_mods[t];
        char name[64];
        size_t size;
        snprintf(name, sizeof name, "mod_%s.f32", v->name);
        float *want = load(name, &size);
        CHECK(size == sizeof(float) * 2 * (size_t)v->n, "%s: %zu bytes", v->name, size);
        if (v->freq) {
            rdof_mod_t m;
            CHECK(init_case(&m, v, v->nseg) == 0, "%s: init", v->name);
            for (int i = 0; i < v->nseg; i++)
                CHECK(queue_seg(&m, &v_mod_segs[v->first_seg + i]) == 0, "%s: queue %d", v->name, i);
            float *hz = malloc(sizeof(float) * (size_t)v->n), *env = malloc(sizeof(float) * (size_t)v->n);
            for (int done = 0; done < v->n;) {
                int k = 777 < v->n - done ? 777 : v->n - done;
                rdof_mod_freq(&m, hz + done, env + done, k);
                done += k;
            }
            double ehz = 0, eenv = 0;
            for (int i = 0; i < v->n; i++) {
                ehz = fmax(ehz, fabs(hz[i] - want[i]));
                eenv = fmax(eenv, fabs(env[i] - want[v->n + i]));
            }
            printf("mod %-8s %u S/s: frequency within %.4f Hz, envelope within %.2g\n", v->name, v->fs, ehz, eenv);
            CHECK(ehz < TOL_HZ, "%s: frequency off by %g Hz", v->name, ehz);
            CHECK(eenv < TOL_ENV, "%s: envelope off by %g", v->name, eenv);
            free(hz);
            free(env);
            rdof_mod_free(&m);
        } else {
            rdof_cf_t *x = run_iq(v);
            double err = 0;
            for (int i = 0; i < v->n; i++)
                err = fmax(err, hypot(x[i].re - want[2 * i], x[i].im - want[2 * i + 1]));
            printf("mod %-8s %u S/s: I/Q within %.2g of the reference\n", v->name, v->fs, err);
            CHECK(err < TOL_IQ, "%s: I/Q off by %g", v->name, err);
            free(x);
        }
        free(want);
    }
}

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;

static double gauss(void)
{
    double u[2];
    for (int i = 0; i < 2; i++) {
        rng_state ^= rng_state << 13;
        rng_state ^= rng_state >> 7;
        rng_state ^= rng_state << 17;
        u[i] = ((rng_state >> 11) + 0.5) / 9007199254740992.0;
    }
    return sqrt(-2 * log(u[0])) * cos(2 * 3.14159265358979323846 * u[1]);
}

/* C mod -> (C channelizer) -> C demod: the case's bursts, at its positions. */
static void round_trip(const struct v_mod *v, double sigma)
{
    rdof_cf_t *x = run_iq(v);
    for (int i = 0; sigma > 0 && i < v->n; i++) {
        x[i].re += (float)(sigma * gauss());
        x[i].im += (float)(sigma * gauss());
    }
    const int D = (int)(v->fs / 48000);
    rdof_cf_t *y = x;
    int ny = v->n;
    rdof_chz_t chz;
    if (D > 1) {
        CHECK(rdof_chz_init(&chz, v->fs, &v->rx_offset, 1, D, RDOF_CHZ_OUT_FFT, RDOF_CHZ_CUTOFF_HZ) == 0, "chz init");
        y = malloc(sizeof(rdof_cf_t) * (size_t)(rdof_chz_out_len(&chz, v->n) + 1));
        ny = rdof_chz_process(&chz, x, v->n, y, rdof_chz_out_len(&chz, v->n) + 1);
        rdof_chz_free(&chz);
    }
    rdof_demod_t d;
    const int chunk = 2400;
    CHECK(rdof_demod_init(&d, 2, 1, RDOF_DEMOD_SQUELCH_HZ, chunk) == 0, "demod init");
    int k = 0;
    for (int i = 0; i < ny; i += chunk) {
        int nb;
        const rdof_sdr_burst_t *bs = rdof_demod_feed(&d, y + i, ny - i < chunk ? ny - i : chunk, &nb);
        for (int j = 0; j < nb; j++) {
            if (bs[j].b.dt == 9) /* idle */
                continue;
            char hex[67];
            tohex(bs[j].b.burst, 33, hex);
            if (k < v->nburst) {
                const struct v_mod_burst *w = &v_mod_bursts[v->first_burst + k];
                CHECK(!strcmp(hex, w->hex) && bs[j].position == w->position,
                      "%s (noise %g) burst %d: pos %lld%s, want %lld", v->name, sigma, k, (long long)bs[j].position,
                      strcmp(hex, w->hex) ? " (bits differ)" : "", w->position);
            }
            k++;
        }
    }
    CHECK(k == v->nburst, "%s (noise %g): %d bursts, want %d", v->name, sigma, k, v->nburst);
    printf("mod %-8s round trip through the C receiver, noise %.2f: %d of %d bursts bit-exact at their positions\n",
           v->name, sigma, k, v->nburst);
    rdof_demod_free(&d);
    if (y != x)
        free(y);
    free(x);
}

/* Queued just in time, two at a time, into a queue of 4: the same samples as all queued up front. */
static void test_just_in_time(void)
{
    const struct v_mod *v = &v_mods[0];
    rdof_cf_t *whole = run_iq(v);
    rdof_mod_t m;
    CHECK(init_case(&m, v, 4) == 0, "init");
    rdof_cf_t *x = malloc(sizeof(rdof_cf_t) * (size_t)v->n);
    int next = 0, done = 0;
    while (done < v->n) {
        while (next < v->nseg && v_mod_segs[v->first_seg + next].start < done + 2 * 2880) {
            CHECK(queue_seg(&m, &v_mod_segs[v->first_seg + next]) == 0, "queue %d at %d", next, done);
            next++;
        }
        int k = 500 < v->n - done ? 500 : v->n - done;
        rdof_mod_iq(&m, x + done, k);
        done += k;
    }
    CHECK(!memcmp(x, whole, sizeof(rdof_cf_t) * (size_t)v->n), "just in time differs");
    /* late, out of order, too long */
    const struct v_mod_seg *s = &v_mod_segs[v->first_seg];
    CHECK(rdof_mod_queue(&m, m.pos, s->sym, 0, 132, RDOF_MOD_RAMPS) == -1, "late accepted");
    CHECK(rdof_mod_queue(&m, m.pos + 10000, s->sym, 0, 132, RDOF_MOD_RAMPS) == 0, "on time refused");
    CHECK(rdof_mod_queue(&m, m.pos + 9000, s->sym, 0, 132, RDOF_MOD_RAMPS) == -1, "out of order accepted");
    CHECK(rdof_mod_queue(&m, m.pos + 20000, s->sym, 0, RDOF_MOD_MAX_SYMBOLS + 1, RDOF_MOD_RAMPS) == -1, "too long");
    CHECK(rdof_mod_slot_samples(&m) == 1440 && rdof_mod_key_on(&m, 1000, 0) == 956, "timing");
    free(x);
    free(whole);
    rdof_mod_free(&m);

    rdof_mod_t f;
    CHECK(rdof_mod_init(&f, 250000, 0, 4, 1, 4, 0) == 0 && f.P == 625 && f.Q == 12 && rdof_mod_slot_samples(&f) == 7500,
          "250 kS/s");
    CHECK(rdof_mod_samples(&f, 1) == -1, "a 250 kS/s symbol isn't whole");
    rdof_mod_free(&f);
    CHECK(rdof_mod_init(&f, 4800, 0, 4, 1, 4, 0) == -1, "one sample a symbol accepted");
    rdof_mod_free(&f);
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* A repeater's continuous downlink at 288 kS/s: time per second of I/Q (rough, on the host). */
static void bench(void)
{
    rdof_mod_t m;
    rdof_mod_init(&m, 288000, 62500.0, 4.0, 1.0f, 4, 0);
    const struct v_mod_seg *s = &v_mod_segs[0];
    int8_t sym[144];
    for (int i = 0; i < 144; i++)
        sym[i] = s->sym[i % 132];
    enum { SECONDS = 5, BLOCK = 2880 };
    static rdof_cf_t buf[BLOCK];
    const int64_t slot = rdof_mod_slot_samples(&m);
    int64_t next = 6000;
    double t0 = now();
    for (int64_t done = 0; done < 288000LL * SECONDS; done += BLOCK) {
        while (next < done + 3 * slot) {
            rdof_mod_queue(&m, next, sym, -12, 144, next == 6000 ? RDOF_MOD_RAMP_UP : 0);
            next += slot;
        }
        rdof_mod_iq(&m, buf, BLOCK);
    }
    double t = (now() - t0) / SECONDS;
    printf("mod bench: 288 kS/s continuous I/Q, %.1f ms of host CPU per second (%.1f ns a sample)\n", t * 1e3,
           t * 1e9 / 288000);
    rdof_mod_free(&m);
}

int main(void)
{
    test_vectors();
    for (int t = 0; t < V_MODS_N; t++)
        if (v_mods[t].roundtrip) {
            round_trip(&v_mods[t], 0.0);
            round_trip(&v_mods[t], 0.1);
        }
    test_just_in_time();
    bench();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
