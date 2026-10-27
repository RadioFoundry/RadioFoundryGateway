// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Host tests for the corners of sdr: bad arguments and limits, an allocator that runs out at every step,
 * the clocks and the CPU profile, the paths the golden-vector tests don't take (the idle gate, the demod's skip,
 * the two-core split, the lone sample in an odd-length block), and the CPU bench. Where the reference has the
 * answer (the RRC taps, the AGC, the config, the receiver status) it comes from sdr_vectors.h (fixtures/); the rest is checked against a second way of getting the same result (one pass or many,
 * the gate on or off, a plain DFT) or against what the code is documented to do.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rdof/mmdvm.h"
#include "rdof/pdu.h"
#include "rdof/rawburst.h"
#include "rdof/slotgrid.h"
#include "rdof/sdr/agc.h"
#include "rdof/sdr/bench.h"
#include "rdof/sdr/channelizer.h"
#include "rdof/sdr/config.h"
#include "rdof/sdr/demod.h"
#include "rdof/sdr/fft.h"
#include "rdof/sdr/fm.h"
#include "rdof/sdr/frontend.h"
#include "rdof/sdr/iq.h"
#include "rdof/sdr/mod.h"
#include "rdof/sdr/ppm.h"
#include "rdof/sdr/rate.h"
#include "rdof/sdr/receiver.h"
#include "rdof/sdr/resample.h"
#include "rdof/sdr/spectrum.h"
#include "rdof/sdr/status.h"
#include "sdr_vectors.h"

#ifndef RDOF_SDR_DATA
#define RDOF_SDR_DATA "sdr-data/"
#endif

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

/* ── an allocator that runs out ──────────────────────────────────────────── */

static long a_count, a_fail_at, a_live, a_fired, a_fail_all;

static void *fa_alloc(size_t n)
{
    a_count++;
    if (a_fail_all || (a_fail_at && a_count == a_fail_at)) {
        a_fired++;
        return NULL;
    }
    a_live++;
    return malloc(n);
}

static void fa_free(void *p)
{
    a_live--;
    free(p);
}

static size_t *sizes_rec;
static long *sizes_n;
static void *rec_alloc(size_t n)
{
    if (*sizes_n < 2000)
        sizes_rec[(*sizes_n)++] = n ? n : 1;
    return fa_alloc(n);
}

static void fa_arm(long fail_at)
{
    a_count = a_live = a_fired = 0;
    a_fail_at = fail_at;
    rdof_sdr_set_allocator(fa_alloc, fa_free);
}

static void fa_disarm(void)
{
    rdof_sdr_set_allocator(NULL, NULL);
    a_fail_at = a_fail_all = 0;
}

/* Runs init (-> nonzero on failure) with allocation 1, 2, ... failing in turn. Each failure must be reported
 * and, after free, leave nothing allocated. Returns how many allocations a good run makes. */
static long alloc_sweep(const char *name, int (*init)(void), void (*release)(void))
{
    long total = 0;
    for (long n = 1; n < 500; n++) {
        fa_arm(n);
        int r = init();
        long fired = a_fired;
        release();
        long live = a_live;
        fa_disarm();
        if (!fired) {
            CHECK(r == 0, "%s: %d with every allocation good", name, r);
            CHECK(live == 0, "%s: %ld allocations left after free", name, live);
            total = n - 1;
            break;
        }
        CHECK(r != 0, "%s: allocation %ld failed and init said %d", name, n, r);
        CHECK(live == 0, "%s: allocation %ld failed and left %ld allocated", name, n, live);
    }
    return total;
}

/* ── things shared by the tests ──────────────────────────────────────────── */

static void *load(const char *name, size_t *size)
{
    char path[512];
    snprintf(path, sizeof path, "%s%s", RDOF_SDR_DATA, name);
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

static uint32_t rng_state = 12345;
static float noise(void)
{
    float s = 0;
    for (int i = 0; i < 4; i++) { /* roughly normal, unit variance */
        rng_state = rng_state * 1664525u + 1013904223u;
        s += (float)(rng_state >> 8) / 16777216.0f - 0.5f;
    }
    return s * 1.732f;
}

/* ── allocation failures ─────────────────────────────────────────────────── */

static rdof_chz_t g_chz;
static int init_chz(void)
{
    const double off[3] = {-50e3, 10e3, 60e3};
    return rdof_chz_init(&g_chz, 288e3, off, 3, 6, RDOF_CHZ_OUT_FFT, RDOF_CHZ_CUTOFF_HZ);
}
static void free_chz(void) { rdof_chz_free(&g_chz); }

static int init_chz_reserve(void)
{
    if (init_chz())
        return -1;
    return rdof_chz_reserve(&g_chz, 4608);
}

static rdof_demod_t g_demod;
static int init_demod(void) { return rdof_demod_init(&g_demod, 2, 1, RDOF_DEMOD_SQUELCH_HZ, 4096); }
static void free_demod(void) { rdof_demod_free(&g_demod); }

static rdof_sdr_rx_t g_rx;
static int init_rx(void)
{
    const rdof_sdr_rx_config_t cfg = {288000, 461000000.0, -60.0, 0, true, 4608};
    const rdof_sdr_channel_t ch[2] = {{"A", 461037500.0, 1, 1}, {"B", 461087500.0, 1, 2}};
    return rdof_sdr_rx_init(&g_rx, &cfg, ch, 2, NULL, 0);
}
static void free_rx(void) { rdof_sdr_rx_free(&g_rx); }

static rdof_fft_t *g_fft;
static int init_fft(void)
{
    g_fft = rdof_fft_new(30, false);
    return g_fft ? 0 : -1;
}
static void free_fft(void)
{
    rdof_fft_free(g_fft);
    g_fft = NULL;
}

static rdof_spectrum_t g_spec;
static int init_spec(void) { return rdof_spectrum_init(&g_spec, 64, 2); }
static void free_spec(void) { rdof_spectrum_free(&g_spec); }

static rdof_resamp_t g_rs;
static int init_resamp(void) { return rdof_resamp_init(&g_rs, 1536000, 288000, 32, 0.8, 4096); }
static void free_resamp(void) { rdof_resamp_free(&g_rs); }

static rdof_mod_t g_mod;
static int init_mod(void) { return rdof_mod_init(&g_mod, 288000, 1000.0, 4.0, 1.0f, 4, 0); }
static void free_mod(void) { rdof_mod_free(&g_mod); }

static int init_plan(void)
{
    /* nine channels that don't all fit: the window search allocates */
    const double f[9] = {461.0375e6, 461.0875e6, 461.1375e6, 461.1875e6, 461.2375e6, 461.2875e6, 461.3375e6,
                         461.4625e6, 461.6875e6};
    double c;
    return rdof_plan_window(f, 9, 288e3, &c, NULL) > 0 ? 0 : -1;
}
static void free_nothing(void) {}

static int init_config(void)
{
    static rdof_sdr_config_t cfg;
    const char *json = "{\"device\":{\"sample_rate\":288000},\"channels\":[{\"id\":\"A\",\"freq\":461.0375}]}";
    char err[64];
    bool ok = rdof_sdr_config_parse(json, strlen(json), &cfg, err, sizeof err);
    return ok ? 0 : (strstr(err, "no memory") ? 1 : 2);
}

static uint8_t *g_bench_cu8;
static int g_bench_n;
static double fake_now_t;
static double fake_now(void) { return fake_now_t += 0.001; }
static rdof_sdr_channel_t g_bench_chans[4];
static rdof_sdr_rx_config_t g_bench_cfg;

static void test_alloc(void)
{
    long n;
    n = alloc_sweep("chz init", init_chz, free_chz);
    CHECK(n > 15, "chz init makes %ld allocations", n);
    n = alloc_sweep("chz reserve", init_chz_reserve, free_chz);
    CHECK(n > 15, "chz init and reserve make %ld allocations", n);
    n = alloc_sweep("demod init", init_demod, free_demod);
    CHECK(n >= 7, "demod init makes %ld allocations", n);
    n = alloc_sweep("receiver init", init_rx, free_rx);
    CHECK(n > 40, "receiver init makes %ld allocations", n);
    n = alloc_sweep("fft plan", init_fft, free_fft);
    CHECK(n == 2, "an fft plan makes %ld allocations", n);
    n = alloc_sweep("spectrum", init_spec, free_spec);
    CHECK(n >= 5, "spectrum init makes %ld allocations", n);
    n = alloc_sweep("resampler", init_resamp, free_resamp);
    CHECK(n == 2, "resampler init makes %ld allocations", n);
    n = alloc_sweep("modulator", init_mod, free_mod);
    CHECK(n == 3, "modulator init makes %ld allocations", n);
    n = alloc_sweep("window planner", init_plan, free_nothing);
    CHECK(n == 2, "window planner makes %ld allocations", n);
    n = alloc_sweep("config parse", init_config, free_nothing);
    CHECK(n == 2, "config parse makes %ld allocations", n);

    /* the bench: every allocation failing in turn must be reported */
    g_bench_cu8 = load("e2e_288k.cu8", (size_t *)&n);
    g_bench_n = 2000; /* a short clip: the allocations are the same, the run is quick */
    for (size_t i = 0; i < 4; i++) {
        g_bench_chans[i] = (rdof_sdr_channel_t){v_e2e_chans[i].id, v_e2e_chans[i].freq, v_e2e_chans[i].cc,
                                              v_e2e_chans[i].slot};
    }
    g_bench_cfg = (rdof_sdr_rx_config_t){288e3, 0.0, V_E2E_RSSI_OFFSET, 0.0, true, V_E2E_CHUNK};
    long total_allocs = 0;
    for (long k = 1; k < 600; k++) {
        fa_arm(k);
        rdof_sdr_bench_t out;
        int r = rdof_sdr_bench(g_bench_cu8, g_bench_n, &g_bench_cfg, g_bench_chans, 4, fake_now, 1, &out);
        long fired = a_fired, live = a_live;
        fa_disarm();
        if (!fired) {
            CHECK(r == 0 && live == 0, "bench with every allocation good: %d, %ld left", r, live);
            total_allocs = k - 1;
            break;
        }
        CHECK(r == -1 && live == 0, "bench: allocation %ld failed: %d, %ld left", k, r, live);
    }
    CHECK(total_allocs > 10, "bench makes %ld allocations", total_allocs);
    free(g_bench_cu8);
}

/* ── slot timing utc: the receiver's UTC slots ───────────────────────────── */

#define UTC_T0 1790521200000000000LL /* ns */
#define UTC_RATE (48000.0 * (1 + 3e-6))
#define UTC_OFFSET_US 1000

struct utc_ctx {
    long bursts, stamped, raw, raw_utc, wrong;
};

static void utc_burst(void *ctx, int c, const rdof_sdr_burst_t *b, double t)
{
    struct utc_ctx *u = ctx;
    u->bursts++;
    const bool timed = b->b.family == RDOF_FAMILY_BS || b->b.family == RDOF_FAMILY_MS;
    if (b->b.utc != timed) {
        u->wrong++;
        return;
    }
    if (!timed)
        return;
    u->stamped++;
    /* the start from the map, in integers where the map's rate allows */
    const int64_t start = UTC_T0 + (int64_t)floor((double)b->position * 1e9 / UTC_RATE + 0.5);
    int ts;
    int64_t err;
    const int64_t n = b->b.family == RDOF_FAMILY_BS ? rdof_grid_outbound(start, UTC_OFFSET_US, &ts, &err)
                                                  : rdof_grid_inbound(start, UTC_OFFSET_US, &ts, &err);
    if (b->b.utc_slot != n || b->b.utc_us != start / 1000 || (b->b.family == RDOF_FAMILY_MS && b->b.slot != ts))
        u->wrong++;
}

static void utc_raw(void *ctx, const char *ch, const rdof_rawburst_t *r)
{
    struct utc_ctx *u = ctx;
    uint8_t out[RDOF_RAWBURST_MAX];
    const size_t size = rdof_rawburst_encode(r, out);
    u->raw++;
    if (r->utc)
        u->raw_utc++;
    if (size != (r->utc ? RDOF_RAWBURST_BYTES_UTC : RDOF_RAWBURST_BYTES) ||
        out[0] != (r->utc ? RDOF_RAWBURST_VERSION_UTC : RDOF_RAWBURST_VERSION))
        u->wrong++;
}

static void test_rx_utc(void)
{
    size_t size;
    uint8_t *cu8 = load("e2e_288k.cu8", &size);
    const int n = (int)(size / 2);
    rdof_sdr_channel_t chans[4];
    for (size_t i = 0; i < 4; i++)
        chans[i] = (rdof_sdr_channel_t){v_e2e_chans[i].id, v_e2e_chans[i].freq, v_e2e_chans[i].cc, v_e2e_chans[i].slot};
    rdof_sdr_rx_config_t cfg = {288e3, 0.0, V_E2E_RSSI_OFFSET, 0.0, true, V_E2E_CHUNK};
    rdof_cf_t *x = malloc(sizeof(rdof_cf_t) * (size_t)n);
    rdof_iq_from_cu8(cu8, (size_t)n, x);
    for (int mapped = 0; mapped < 2; mapped++) {
        static rdof_sdr_rx_t rx;
        struct utc_ctx u = {0};
        const rdof_sink_t sink = {.ctx = &u, .on_raw_burst = utc_raw};
        CHECK(rdof_sdr_rx_init(&rx, &cfg, chans, 4, &sink, 0) == 0, "rx init");
        rx.on_burst = utc_burst;
        rx.burst_ctx = &u;
        rx.gate = false;
        rdof_sdr_rx_utc_map(&rx, mapped, 0, UTC_T0, UTC_RATE, UTC_OFFSET_US);
        for (int i = 0; i < n; i += V_E2E_CHUNK)
            rdof_sdr_rx_process(&rx, x + i, n - i < V_E2E_CHUNK ? n - i : V_E2E_CHUNK);
        if (mapped) {
            CHECK(u.bursts > 20 && u.stamped > 20 && !u.wrong, "mapped: %ld bursts, %ld stamped, %ld wrong", u.bursts,
                  u.stamped, u.wrong);
            CHECK(u.raw > 0 && u.raw_utc > 0, "mapped: %ld raw, %ld with the UTC slot", u.raw, u.raw_utc);
        } else {
            CHECK(u.bursts > 20 && u.stamped == 0 && u.raw > 0 && u.raw_utc == 0, "unmapped: %ld bursts, %ld stamped, "
                  "%ld raw with the UTC slot", u.bursts, u.stamped, u.raw_utc);
        }
        rdof_sdr_rx_free(&rx);
    }
    /* a map with no rate is none */
    static rdof_sdr_rx_t rx;
    CHECK(rdof_sdr_rx_init(&rx, &cfg, chans, 4, NULL, 0) == 0, "rx init");
    rdof_sdr_rx_utc_map(&rx, true, 0, UTC_T0, 0, 0);
    CHECK(!rx.utc_ok, "a map with rate 0 is off");
    rdof_sdr_rx_free(&rx);
    free(x);
    free(cu8);
}

/* ── the CPU bench ───────────────────────────────────────────────────────── */

struct count_ctx {
    long bursts;
};
static void count_burst(void *ctx, int c, const rdof_sdr_burst_t *b, double t) { ((struct count_ctx *)ctx)->bursts++; }

static void test_bench(void)
{
    size_t size;
    uint8_t *cu8 = load("e2e_288k.cu8", &size);
    const int n = (int)(size / 2);
    rdof_sdr_channel_t chans[4];
    for (size_t i = 0; i < 4; i++)
        chans[i] = (rdof_sdr_channel_t){v_e2e_chans[i].id, v_e2e_chans[i].freq, v_e2e_chans[i].cc, v_e2e_chans[i].slot};
    rdof_sdr_rx_config_t cfg = {288e3, 0.0, V_E2E_RSSI_OFFSET, 0.0, true, V_E2E_CHUNK};

    /* the receiver on the same clip: how many bursts there are, how many forward FFTs */
    static rdof_sdr_rx_t rx;
    struct count_ctx cc = {0};
    CHECK(rdof_sdr_rx_init(&rx, &cfg, chans, 4, NULL, 0) == 0, "rx init");
    rx.on_burst = count_burst;
    rx.burst_ctx = &cc;
    rx.gate = false;
    rdof_cf_t *x = malloc(sizeof(rdof_cf_t) * (size_t)n);
    rdof_iq_from_cu8(cu8, (size_t)n, x);
    for (int i = 0; i < n; i += V_E2E_CHUNK)
        rdof_sdr_rx_process(&rx, x + i, n - i < V_E2E_CHUNK ? n - i : V_E2E_CHUNK);

    fake_now_t = 0;
    rdof_sdr_bench_t b;
    CHECK(rdof_sdr_bench(cu8, n, &cfg, chans, 4, fake_now, 2, &b) == 0, "bench");
    CHECK(fabs(b.clip_seconds - n / 288000.0) < 1e-12, "clip %.6f s", b.clip_seconds);
    CHECK(b.bursts == cc.bursts && b.bursts > 20, "bench found %ld bursts, the receiver %ld", b.bursts, cc.bursts);
    CHECK(b.ffts == rx.chz.block && b.fft_points == rx.chz.N, "%lld FFTs of %d points, want %lld of %d", b.ffts,
          b.fft_points, (long long)rx.chz.block, rx.chz.N);
    for (int s = 0; s < RDOF_BENCH_STAGES; s++) {
        CHECK(isfinite(b.seconds[s]) && b.seconds[s] > 0, "stage %d (%s): %g s", s, rdof_bench_stage_names[s],
              b.seconds[s]);
        CHECK(rdof_bench_stage_names[s] && rdof_bench_stage_names[s][0], "stage %d has a name", s);
    }
    CHECK(isfinite(b.search_no_squelch) && b.search_no_squelch > 0, "search without the squelch %g",
          b.search_no_squelch);

    /* the tuner center given: the same window gives the same answer */
    rdof_sdr_bench_t b2;
    cfg.center_hz = V_E2E_CENTER;
    CHECK(rdof_sdr_bench(cu8, n, &cfg, chans, 4, fake_now, 1, &b2) == 0 && b2.bursts == b.bursts && b2.ffts == b.ffts,
          "bench with the center given");
    /* no repetitions: nothing measured, nothing wrong */
    CHECK(rdof_sdr_bench(cu8, n, &cfg, chans, 4, fake_now, 0, &b2) == 0 && b2.bursts == 0 && isinf(b2.seconds[0]) &&
              isinf(b2.search_no_squelch),
          "no repetitions");
    /* a clip shorter than a block (384 samples with 96 already in) */
    CHECK(rdof_sdr_bench(cu8, 200, &cfg, chans, 4, fake_now, 1, &b2) == 0 && b2.ffts == 0 && b2.bursts == 0,
          "a short clip: %lld FFTs", b2.ffts);
    /* no window, too few channels, too many */
    cfg.center_hz = 0.0;
    rdof_sdr_rx_config_t nowhere = cfg; /* a rate with no usable band: the planner finds no window */
    nowhere.sample_rate = 0.0;
    CHECK(rdof_sdr_bench(cu8, n, &nowhere, chans, 4, fake_now, 1, &b2) == -1, "no window");
    CHECK(rdof_sdr_bench(cu8, n, &cfg, chans, 0, fake_now, 1, &b2) == -1, "no channels");
    CHECK(rdof_sdr_bench(cu8, n, &cfg, chans, 65, fake_now, 1, &b2) == -1, "65 channels");
    free(x);
    rdof_sdr_rx_free(&rx);
    free(cu8);
}

/* ── the channelizer ─────────────────────────────────────────────────────── */

static void test_chz_args(void)
{
    rdof_chz_t z;
    const double off[1] = {0.0};
    /* decimation 0, an FFT under 4 points or an odd one, no channels, a hop that isn't a multiple of the
     * decimation (3 / 4 of 12 is 9, which 2 doesn't divide), an FFT size the planner can't make (7) */
    CHECK(rdof_chz_init(&z, 288e3, off, 1, 0, 64, 7500) == -1, "decimation 0");
    rdof_chz_free(&z);
    CHECK(rdof_chz_init(&z, 288e3, off, 1, 6, 2, 7500) == -1, "2 points");
    rdof_chz_free(&z);
    CHECK(rdof_chz_init(&z, 288e3, off, 1, 6, 63, 7500) == -1, "63 points");
    rdof_chz_free(&z);
    CHECK(rdof_chz_init(&z, 288e3, off, 0, 6, 64, 7500) == -1, "no channels");
    rdof_chz_free(&z);
    CHECK(rdof_chz_init(&z, 288e3, off, 1, 2, 6, 7500) == -1, "hop 9 for decimation 2");
    rdof_chz_free(&z);
    CHECK(rdof_chz_init(&z, 288e3, off, 1, 1, 14, 7500) == -1, "a 14-point inverse and a 14-point forward");
    rdof_chz_free(&z);
    CHECK(rdof_chz_init(&z, 288e3, off, 1, 1, 4, 7500) == 0 && z.N == 4 && z.hop == 3 && z.discard == 1 &&
              z.out_per_hop == 3,
          "the smallest channelizer");
    rdof_chz_free(&z);
    /* freeing twice, or a zeroed one, is harmless */
    rdof_chz_free(&z);
    memset(&z, 0, sizeof z);
    rdof_chz_free(&z);
}

/* An odd number of output samples a block (D 1, 4 points: 3 out): the NCO's lone last sample. A plain DFT
 * of each block with the filter's bins gives the output. */
static void test_chz_odd(void)
{
    rdof_chz_t z;
    const double off[1] = {0.0};
    CHECK(rdof_chz_init(&z, 48000, off, 1, 1, 4, 7500) == 0, "init");
    enum { NIN = 22 };
    rdof_cf_t x[NIN], out[32];
    for (int i = 0; i < NIN; i++) {
        x[i].re = noise();
        x[i].im = noise();
    }
    int want = rdof_chz_out_len(&z, NIN), got = rdof_chz_process(&z, x, NIN, out, 32);
    CHECK(got == want && got == 21, "%d outputs, %d promised", got, want);
    double worst = 0;
    for (int b = 0; b < got / 3; b++) {
        double yr[4] = {0}, yi[4] = {0};
        for (int j = 0; j < 4; j++) {
            const int bin = j < 2 ? j : j - 4;
            double Yr = 0, Yi = 0;
            for (int n = 0; n < 4; n++) {
                const int idx = b * 3 + n - 1; /* the window starts N - hop = 1 zero in */
                const double re = idx >= 0 ? x[idx].re : 0, im = idx >= 0 ? x[idx].im : 0;
                const double ph = -2 * M_PI * bin * n / 4;
                Yr += re * cos(ph) - im * sin(ph);
                Yi += re * sin(ph) + im * cos(ph);
            }
            const double hr = z.H[j].re, hi = z.H[j].im, ar = Yr * hr - Yi * hi, ai = Yr * hi + Yi * hr;
            for (int m = 0; m < 4; m++) {
                const double ph = 2 * M_PI * j * m / 4;
                yr[m] += ar * cos(ph) - ai * sin(ph);
                yi[m] += ar * sin(ph) + ai * cos(ph);
            }
        }
        for (int i = 0; i < 3; i++)
            worst = fmax(worst, hypot(out[b * 3 + i].re - yr[1 + i], out[b * 3 + i].im - yi[1 + i]));
    }
    CHECK(worst < 1e-5, "odd-length blocks differ from a plain DFT by %g", worst);
    rdof_chz_free(&z);
}

static uint32_t clk;
static uint32_t tick10(void) { return clk += 10; }

static void test_chz_clock(void)
{
    rdof_chz_t z;
    const double off[2] = {-50e3, 60e3};
    CHECK(rdof_chz_init(&z, 288e3, off, 2, 6, 64, 7500) == 0, "init");
    CHECK(rdof_chz_reserve(&z, 4608) == 0 && z.xs_cap > 0, "reserve");
    rdof_cf_t *xs = z.Xs;
    CHECK(rdof_chz_reserve(&z, 4608) == 0 && z.Xs == xs, "reserving the same again keeps the store");
    CHECK(rdof_chz_reserve(&z, 100) == 0 && z.Xs == xs, "a smaller one too");
    const int cap = z.xs_cap;
    CHECK(rdof_chz_reserve(&z, 40000) == 0 && z.xs_cap > cap, "a bigger one grows it");
    static rdof_cf_t in[4608];
    for (int i = 0; i < 4608; i++) {
        in[i].re = 0.1f * noise();
        in[i].im = 0.1f * noise();
    }
    /* a clock set: each timed step counts 10 ticks. process: one FFT, one noise floor, the channels and a window
     * move a block, and each window fill */
    memset(&rdof_chz_prof, 0, sizeof rdof_chz_prof);
    clk = 0;
    rdof_chz_set_clock(tick10);
    static rdof_cf_t out[2 * 2000];
    int fill = z.fill, blocks = 0, iters = 0;
    for (int used = 0; used < 4608;) {
        int take = z.N - fill < 4608 - used ? z.N - fill : 4608 - used;
        fill += take;
        used += take;
        iters++;
        if (fill < z.N)
            break;
        blocks++;
        fill = z.N - z.hop;
    }
    int got = rdof_chz_process(&z, in, 4608, out, 2000);
    CHECK(got == blocks * z.out_per_hop, "process: %d outputs, %d blocks", got, blocks);
    CHECK(rdof_chz_prof.blocks == (uint64_t)blocks && rdof_chz_prof.fft == 10u * (uint64_t)blocks &&
              rdof_chz_prof.noise == 10u * (uint64_t)blocks && rdof_chz_prof.copy == 10u * (uint64_t)(iters + blocks) &&
              rdof_chz_prof.chan == 10u * (uint64_t)blocks,
          "process profile: %llu blocks, fft %llu noise %llu copy %llu chan %llu",
          (unsigned long long)rdof_chz_prof.blocks, (unsigned long long)rdof_chz_prof.fft,
          (unsigned long long)rdof_chz_prof.noise, (unsigned long long)rdof_chz_prof.copy,
          (unsigned long long)rdof_chz_prof.chan);
    /* forward: the same blocks, fills and moves; the channels are rdof_chz_channels', not counted */
    rdof_chz_free(&z);
    CHECK(rdof_chz_init(&z, 288e3, off, 2, 6, 64, 7500) == 0 && rdof_chz_reserve(&z, 4608) == 0, "again");
    memset(&rdof_chz_prof, 0, sizeof rdof_chz_prof);
    CHECK(rdof_chz_forward(&z, in, 4608) == got, "forward: %d outputs a channel", got);
    CHECK(rdof_chz_prof.blocks == (uint64_t)blocks && rdof_chz_prof.fft == 10u * (uint64_t)blocks &&
              rdof_chz_prof.noise == 10u * (uint64_t)blocks && rdof_chz_prof.copy == 10u * (uint64_t)(iters + blocks) &&
              rdof_chz_prof.chan == 0,
          "forward profile: copy %llu, want %d", (unsigned long long)rdof_chz_prof.copy, 10 * (iters + blocks));
    rdof_chz_set_clock(NULL);
    memset(&rdof_chz_prof, 0, sizeof rdof_chz_prof);
    rdof_chz_forward(&z, in, 4608);
    CHECK(rdof_chz_prof.blocks == 0 && rdof_chz_prof.fft == 0 && rdof_chz_prof.copy == 0, "no clock, nothing counted");

    /* more input than the store was sized for: the blocks that fit, and no more */
    rdof_chz_free(&z);
    CHECK(rdof_chz_init(&z, 288e3, off, 2, 6, 64, 7500) == 0 && rdof_chz_reserve(&z, 1000) == 0, "small store");
    const int fit = z.xs_cap;
    int n_out = rdof_chz_forward(&z, in, 4608);
    CHECK(z.nblk == fit && n_out == fit * z.out_per_hop && fit < blocks, "%d of %d blocks kept, %d outputs", z.nblk,
          fit, n_out);
    /* a channel range past the last is clipped to the channels there are */
    rdof_chz_channels(&z, 0, 99, 0, out, 2000);
    rdof_chz_channels(&z, 5, 9, 0, out, 2000);
    rdof_chz_free(&z);
}

/* The noise floor of an all-zero input is zero, and any signal in a channel then reads infinitely high. */
static void test_chz_level(void)
{
    rdof_chz_t z;
    const double off[1] = {0.0};
    CHECK(rdof_chz_init(&z, 288e3, off, 1, 6, 64, 7500) == 0, "init");
    static rdof_cf_t in[2000], out[2000];
    int got = rdof_chz_process(&z, in, 2000, out, 2000);
    CHECK(got > 0 && z.noise_in == 0.0f && z.level[0] == 0.0f, "silence reads %g over noise %g", z.level[0],
          z.noise_in);
    for (int i = 0; i < 2000; i++)
        in[i].re = 1.0f; /* DC: all of it in the one bin */
    got = rdof_chz_process(&z, in, 2000, out, 2000);
    CHECK(got > 0 && z.level[0] > 1000.0f, "a strong tone over a silent band reads %g", z.level[0]);
    rdof_chz_free(&z);
}

/* ── the FM stage ────────────────────────────────────────────────────────── */

static void test_rrc(void)
{
    for (size_t i = 0; i < V_RRC_CASES_N; i++) {
        const struct v_rrc_case *v = &v_rrc_cases[i];
        double h[96];
        CHECK(rdof_rrc_taps(v->sps, v->alpha, v->span, h) == v->n, "rrc case %zu length", i);
        double e = 0;
        for (int k = 0; k < v->n; k++)
            e = fmax(e, fabs(h[k] - v->h[k]));
        CHECK(e < 1e-12, "rrc (sps %d, alpha %g, span %d) off by %g", v->sps, v->alpha, v->span, e);
    }
}

static void *decline_fir(const float *h, int taps) { return NULL; }

static void test_fm_edges(void)
{
    enum { N = 3000 };
    static rdof_cf_t x[N];
    double ph = 0;
    for (int i = 0; i < N; i++) { /* a 4FSK-like wander of the frequency, and some noise */
        ph += 2 * M_PI * (900.0 * sin(i * 0.013) + 1500.0 * sin(i * 0.0041)) / 48000.0;
        x[i].re = (float)(0.5 * cos(ph)) + 0.01f * noise();
        x[i].im = (float)(0.5 * sin(ph)) + 0.01f * noise();
    }
    static float of[N], op[N], cf[N], cp[N];
    rdof_fm_t a, b;
    rdof_fm_init(&a, 48000.0);
    rdof_fm_process(&a, x, N, of, op);
    /* the same samples in pieces of every awkward size: the filter, the delay line and the power line carry over */
    static const int pieces[] = {1, 7, 39, 40, 41, 255, 256, 257, 1000};
    for (size_t k = 0; k < sizeof pieces / sizeof pieces[0]; k++) {
        rdof_fm_init(&b, 48000.0);
        for (int done = 0; done < N;) {
            int m = pieces[k] < N - done ? pieces[k] : N - done;
            rdof_fm_process(&b, x + done, m, cf + done, cp + done);
            done += m;
        }
        CHECK(!memcmp(of, cf, sizeof of) && !memcmp(op, cp, sizeof op), "pieces of %d differ from one pass", pieces[k]);
        rdof_fm_free(&b);
    }
    /* the power line is |x|^2 delayed by RDOF_FM_DELAY */
    int bad = 0;
    for (int i = RDOF_FM_DELAY; i < N; i++) {
        const rdof_cf_t v = x[i - RDOF_FM_DELAY];
        bad += op[i] != v.re * v.re + v.im * v.im;
    }
    CHECK(bad == 0, "%d power samples off", bad);
    /* the folded filter against the plain loop over every tap (the taps are symmetric, so it is only the
     * order of the float sums that differs) */
    rdof_fm_init(&b, 48000.0);
    b.symmetric = 0;
    rdof_fm_process(&b, x, N, cf, cp);
    double worst = 0, big = 0;
    for (int i = 0; i < N; i++) {
        worst = fmax(worst, fabs(cf[i] - of[i]));
        big = fmax(big, fabs(of[i]));
    }
    CHECK(worst < 1e-4 * big && !memcmp(op, cp, sizeof op), "plain loop off by %g Hz of %g", worst, big);
    rdof_fm_free(&b);
    rdof_fm_free(&a);
    rdof_fm_free(&a);

    /* a FIR backend that declines leaves the built-in filter in use */
    static const rdof_fir_backend_t no_fir = {decline_fir, NULL, NULL};
    rdof_fm_set_fir_backend(&no_fir);
    rdof_fm_init(&b, 48000.0);
    CHECK(b.fir == NULL && b.fir_state == NULL && b.symmetric, "a declined backend");
    rdof_fm_process(&b, x, N, cf, cp);
    CHECK(!memcmp(of, cf, sizeof of), "and the built-in filter runs");
    rdof_fm_set_fir_backend(NULL);
    rdof_fm_free(&b);

    /* the discriminator: odd and even counts, and the last sample carried in *prev */
    rdof_cf_t prev = {1, 0}, p2 = {1, 0};
    float d1[7], d2[7];
    rdof_fm_discriminate(&prev, 48000.0f / 6.2831853f, x, 7, d1);
    for (int i = 0; i < 7; i++)
        rdof_fm_discriminate(&p2, 48000.0f / 6.2831853f, x + i, 1, d2 + i);
    CHECK(!memcmp(d1, d2, sizeof d1) && prev.re == x[6].re && prev.im == x[6].im && p2.re == prev.re,
          "discriminate in one call or seven");
    rdof_fm_discriminate(&prev, 1.0f, x, 0, d1);
    CHECK(prev.re == x[6].re, "nothing to do: *prev kept");
}

/* ── the tuner window ────────────────────────────────────────────────────── */

/* The most channels any center takes inside the usable band and clear of the DC guard, by trying every Hz. */
static int best_window(const double *f, int n, double fs)
{
    const double half = fs * 0.8 / 2;
    double lo = f[0], hi = f[0];
    for (int i = 1; i < n; i++) {
        lo = fmin(lo, f[i]);
        hi = fmax(hi, f[i]);
    }
    int best = 0;
    for (double c = lo - half; c <= hi + half; c += 1.0) {
        int k = 0;
        for (int i = 0; i < n; i++)
            k += fabs(f[i] - c) <= half && fabs(f[i] - c) >= 25e3;
        best = k > best ? k : best;
    }
    return best;
}

static void test_plan_edges(void)
{
    double c = -1, f[8] = {0};
    bool in[8];
    CHECK(rdof_plan_center(f, 0, 288e3, &c) == -1 && c == -1, "no channels, no center");
    CHECK(rdof_plan_window(f, 0, 288e3, &c, in) == 0 && c == -1, "no channels, no window");
    /* 300 kHz apart: wider than the usable 230.4 kHz */
    f[0] = 461.0e6;
    f[1] = 461.3e6;
    CHECK(rdof_plan_center(f, 2, 288e3, &c) == -1, "channels too far apart for one center");
    CHECK(rdof_plan_window(f, 2, 288e3, &c, in) == 1 && in[0] + in[1] == 1 && fabs(c - (in[0] ? f[0] : f[1])) >= 25e3,
          "a window takes one of them");
    /* just inside: 230.4 kHz exactly fits, one Hz more doesn't */
    f[1] = f[0] + 230400.0;
    CHECK(rdof_plan_center(f, 2, 288e3, &c) == 0 && fabs(c - (f[0] + f[1]) / 2) < 1e-6, "fits to the Hz");
    f[1] = f[0] + 230401.0;
    CHECK(rdof_plan_center(f, 2, 288e3, &c) == -1, "one Hz too wide");
    /* a rate with no band at all */
    CHECK(rdof_plan_window(f, 2, 0.0, &c, NULL) == 0, "rate 0");
    /* a window that has to put a center where plan_center would sit a channel on DC */
    const double g[5] = {461000000, 461087500, 461475000, 461387500, 461600000};
    int k = rdof_plan_window(g, 5, 288e3, &c, in);
    const int kg = k;
    int ok = 1;
    for (int i = 0; i < 5; i++)
        if (in[i])
            ok &= fabs(g[i] - c) <= 115200.0 && fabs(g[i] - c) >= 25e3;
    CHECK(k == best_window(g, 5, 288e3) && ok && k >= 2, "window of %d at %.0f (best %d)", k, c,
          best_window(g, 5, 288e3));
    /* channels every 40 kHz: any center leaves one within the 25 kHz of DC that can't be used, so the window is the
     * best that does */
    const double h[5] = {461.000e6, 461.040e6, 461.080e6, 461.120e6, 461.160e6};
    k = rdof_plan_window(h, 5, 288e3, &c, in);
    ok = 1;
    for (int i = 0; i < 5; i++)
        if (in[i])
            ok &= fabs(h[i] - c) <= 115200.0 && fabs(h[i] - c) >= 25e3;
    CHECK(k == best_window(h, 5, 288e3) && k == 4 && ok, "40 kHz apart: %d inside (best %d)", k, best_window(h, 5, 288e3));
    CHECK(rdof_plan_center(h, 5, 288e3, &c) == 0 && fabs(c - 461.08e6) <= 40e3, "plan_center still finds a center");
    /* inside may be NULL */
    CHECK(rdof_plan_window(g, 5, 288e3, &c, NULL) == kg, "no inside array");
    /* a narrow front end (the LR2021 at 125 kS/s resampled to 96: channels within +-8 kHz, DC guard 5 kHz): one
     * channel goes 5 kHz off the center, two 50 kHz apart don't fit, and the window takes one of them */
    const double lr[2] = {461.0375e6, 461.0875e6}, u = 16e3 / 96e3;
    CHECK(rdof_plan_center_band(lr + 1, 1, 96e3, u, 5e3, &c) == 0 && fabs(fabs(c - lr[1]) - 5e3) < 1e-6,
          "one channel, 5 kHz off: %.0f", c - lr[1]);
    CHECK(rdof_plan_center_band(lr, 2, 96e3, u, 5e3, &c) == -1, "two channels don't fit the narrow band");
    CHECK(rdof_plan_window_band(lr, 2, 96e3, u, 5e3, &c, in) == 1 && in[0] + in[1] == 1 &&
              fabs(fabs(c - (in[0] ? lr[0] : lr[1])) - 5e3) < 1e-6,
          "a window takes one of them");
    /* the plain ones are the band ones at 0.8 and 25 kHz */
    double c2 = -1;
    CHECK(rdof_plan_window(g, 5, 288e3, &c, NULL) == rdof_plan_window_band(g, 5, 288e3, 0.8, 25e3, &c2, NULL) && c == c2,
          "plan_window is plan_window_band at 0.8, 25 kHz");
}

/* ── a scene: three channels at 288 kS/s, one with a few bursts after the idle gate has closed ─────────────── */

#define FS 288000
#define CENTER 461000000.0
#define CHUNK 4608
#define NSCENE_CH 3
#define SCENE_CHUNKS 200   /* 3.2 s */
#define SCENE_ON (2 * FS + FS * 3 / 10) /* the first burst: 2.3 s, after the gate has closed (2 s) */
#define MAXB 64

typedef struct {
    int c;
    int64_t pos;
    uint8_t burst[RDOF_BURST_BYTES];
} seen_t;

typedef struct {
    seen_t seen[MAXB];
    int n;
    long gated[NSCENE_CH];
    double corr[NSCENE_CH];
    long bursts[NSCENE_CH];
    int rssi0;
} scene_out_t;

static void scene_burst(void *ctx, int c, const rdof_sdr_burst_t *b, double t)
{
    scene_out_t *o = ctx;
    if (o->n < MAXB) {
        o->seen[o->n].c = c;
        o->seen[o->n].pos = b->position;
        memcpy(o->seen[o->n].burst, b->b.burst, RDOF_BURST_BYTES);
        o->n++;
    }
}

static const rdof_sdr_channel_t scene_chans[NSCENE_CH] = {
    {"A", 461037500.0, 1, 1}, {"B", 461087500.0, 1, 2}, {"C", 460937500.0, 1, 1}};

/* The whole recording, a chunk at a time (a fresh modulator and the same noise each run). mode: 0 process;
 * 1 channelize, demod in two halves, finish; 2 forward, channels on two workers, finish; 3 as 2 with no
 * memory for the blocks' store (the channelizer fused into forward). */
typedef struct {
    bool gate;
    int mode;
    double tx_shift_hz;     /* A's transmitter off its channel by this */
    double preset_hz;       /* A's correction before the first chunk */
    double software_ppm;
    bool no_afc, muted;
    bool brief;             /* 1.1 s, the bursts from 0.2 s: for what needs no idle period first */
} scene_opt_t;

static bool scene_muted(void *ctx, int64_t pos) { return true; }

static void run_scene_opt(const scene_opt_t *opt, scene_out_t *o)
{
    const int mode = opt->mode;
    memset(o, 0, sizeof *o);
    static rdof_sdr_rx_t rx;
    const rdof_sdr_rx_config_t cfg = {FS, CENTER, -60.0, opt->software_ppm, !opt->no_afc, CHUNK};
    CHECK(rdof_sdr_rx_init(&rx, &cfg, scene_chans, NSCENE_CH, NULL, 0) == 0, "scene rx init");
    rx.on_burst = scene_burst;
    rx.burst_ctx = o;
    rx.gate = opt->gate;
    if (opt->muted)
        rx.muted = scene_muted;
    if (opt->preset_hz)
        rdof_sdr_rx_set_correction(&rx, 0, opt->preset_hz);
    rdof_mod_t m;
    CHECK(rdof_mod_init(&m, FS, scene_chans[0].freq_hz - CENTER + opt->tx_shift_hz, 4.0, 1.0f, 8, 0) == 0,
          "scene mod init");
    uint8_t pre[3][RDOF_BURST_BYTES];
    rdof_csbk_preambles(9990003, 91, true, 3, 1, pre);
    const int64_t slot = rdof_mod_slot_samples(&m);
    const int64_t t_on = (opt->brief ? FS / 5 : SCENE_ON) + rdof_mod_samples(&m, 24);
    int queued = 0;
    rdof_cf_t *iq = malloc(sizeof(rdof_cf_t) * CHUNK), *sig = malloc(sizeof(rdof_cf_t) * CHUNK);
    rng_state = 777;
    const int chunks = opt->brief ? 70 : SCENE_CHUNKS;
    if (mode == 3)
        a_fail_all = 1; /* below: only the store's allocations are asked for after init */
    rdof_sdr_set_allocator(mode == 3 ? fa_alloc : NULL, mode == 3 ? fa_free : NULL);
    for (int ch = 0; ch < chunks; ch++) {
        const int64_t pos = (int64_t)ch * CHUNK;
        while (queued < 9 && t_on + 2 * queued * slot < pos + 3 * slot) {
            CHECK(rdof_mod_queue_burst(&m, t_on + 2 * queued * slot, pre[queued % 3], NULL, RDOF_MOD_RAMPS) == 0,
                  "queue burst %d", queued);
            queued++;
        }
        rdof_mod_iq(&m, sig, CHUNK);
        for (int i = 0; i < CHUNK; i++) {
            iq[i].re = 0.5f * sig[i].re + 0.02f * noise();
            iq[i].im = 0.5f * sig[i].im + 0.02f * noise();
        }
        if (mode == 0) {
            rdof_sdr_rx_process(&rx, iq, CHUNK);
        } else if (mode == 1) {
            rdof_sdr_rx_channelize(&rx, iq, CHUNK);
            rdof_sdr_rx_demod(&rx, 0, 2);
            rdof_sdr_rx_demod(&rx, 2, 3);
            rdof_sdr_rx_finish(&rx);
        } else {
            rdof_sdr_rx_forward(&rx, iq, CHUNK);
            rdof_sdr_rx_channels(&rx, 0, 1, 0);
            rdof_sdr_rx_channels(&rx, 1, 3, 1);
            rdof_sdr_rx_finish(&rx);
        }
    }
    fa_disarm();
    if (mode == 3)
        CHECK(rx.fused && rx.chz.xs_cap == 0, "no memory for the store: fused");
    else if (mode == 2)
        CHECK(!rx.fused && rx.chz.xs_cap > 0, "the store allocated");
    for (int c = 0; c < NSCENE_CH; c++) {
        o->gated[c] = rx.gated[c];
        o->corr[c] = rx.chz.correction_hz[c];
        o->bursts[c] = rx.bursts[c];
    }
    o->rssi0 = rx.rssi[0];
    free(iq);
    free(sig);
    rdof_mod_free(&m);
    rdof_sdr_rx_free(&rx);
}

static void run_scene(bool gate, int mode, scene_out_t *o)
{
    scene_opt_t opt = {.gate = gate, .mode = mode};
    run_scene_opt(&opt, o);
}

static void same_scene(const char *what, const scene_out_t *a, const scene_out_t *b)
{
    int same = a->n == b->n;
    for (int i = 0; same && i < a->n; i++)
        same = a->seen[i].c == b->seen[i].c && a->seen[i].pos == b->seen[i].pos &&
               !memcmp(a->seen[i].burst, b->seen[i].burst, RDOF_BURST_BYTES);
    CHECK(same, "%s: %d bursts against %d, or different ones", what, a->n, b->n);
    for (int c = 0; c < NSCENE_CH; c++)
        CHECK(a->corr[c] == b->corr[c] && a->bursts[c] == b->bursts[c], "%s: channel %d correction %g/%g bursts %ld/%ld",
              what, c, a->corr[c], b->corr[c], a->bursts[c], b->bursts[c]);
}

static void test_scene(void)
{
    static scene_out_t on, off, split, fwd, fused;
    run_scene(false, 0, &off);
    run_scene(true, 0, &on);
    CHECK(off.n >= 7 && off.bursts[0] >= 7 && off.bursts[1] == 0 && off.bursts[2] == 0, "%d bursts in the scene, %ld on A",
          off.n, off.bursts[0]);
    for (int i = 0; i < off.n; i++)
        CHECK(off.seen[i].c == 0, "burst %d on channel %d", i, off.seen[i].c);
    CHECK(off.gated[0] == 0 && off.gated[1] == 0 && off.gated[2] == 0, "gate off: %ld %ld %ld chunks skipped",
          off.gated[0], off.gated[1], off.gated[2]);
    /* with the gate the empty channels are skipped for most of the recording, A until its bursts, and nothing
     * is heard differently */
    const long chunks = SCENE_CHUNKS;
    CHECK(on.gated[1] > chunks / 4 && on.gated[2] > chunks / 4 && on.gated[0] > 0 && on.gated[0] < on.gated[1],
          "gate on: skipped %ld %ld %ld of %ld chunks", on.gated[0], on.gated[1], on.gated[2], chunks);
    same_scene("gate on and off", &off, &on);
    CHECK(on.rssi0 == off.rssi0 && on.rssi0 != INT32_MIN, "RSSI %d / %d", on.rssi0, off.rssi0);
    /* the splits give the same result */
    run_scene(true, 1, &split);
    same_scene("channelize then demod in halves", &on, &split);
    CHECK(split.gated[1] == on.gated[1], "split gated %ld, whole %ld", split.gated[1], on.gated[1]);
    run_scene(true, 2, &fwd);
    same_scene("forward and channels on two workers", &on, &fwd);
    run_scene(true, 3, &fused);
    same_scene("forward with no memory for the store", &on, &fused);
}

/* ── the demod ───────────────────────────────────────────────────────────── */

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

typedef struct {
    rdof_sdr_burst_t b[MAXB];
    int n;
    int busy_chunks;
} dem_out_t;

/* Feed x in pieces of chunk; the first skip_n samples (in pieces of skip_chunk) go through rdof_demod_skip when
 * skip_n > 0. */
static void run_demod(rdof_demod_t *d, const rdof_cf_t *x, int n, int chunk, int skip_n, int skip_chunk, dem_out_t *o)
{
    memset(o, 0, sizeof *o);
    int i = 0;
    for (; i < skip_n; i += skip_chunk)
        rdof_demod_skip(d, x + i, skip_n - i < skip_chunk ? skip_n - i : skip_chunk);
    i = skip_n;
    for (; i < n; i += chunk) {
        int m = n - i < chunk ? n - i : chunk, nb;
        const rdof_sdr_burst_t *bs = rdof_demod_feed(d, x + i, m, &nb);
        for (int j = 0; j < nb && o->n < MAXB; j++)
            o->b[o->n++] = bs[j];
        o->busy_chunks += rdof_demod_busy(d);
    }
}

static bool same_bursts(const dem_out_t *a, const dem_out_t *b)
{
    if (a->n != b->n)
        return false;
    for (int i = 0; i < a->n; i++)
        if (a->b[i].position != b->b[i].position || memcmp(a->b[i].b.burst, b->b[i].b.burst, RDOF_BURST_BYTES) ||
            a->b[i].b.dt != b->b[i].b.dt || a->b[i].b.slot != b->b[i].b.slot || a->b[i].b.n != b->b[i].b.n ||
            a->b[i].freq_offset_hz != b->b[i].freq_offset_hz || a->b[i].b.power_dbfs != b->b[i].b.power_dbfs)
            return false;
    return true;
}

static uint32_t dclk;
static uint32_t dtick(void) { return dclk += 10; }

static void test_demod_edges(void)
{
    static dem_out_t ref, other;
    int n_noise, n_bs;
    rdof_cf_t *noisy = load_cu8("demod_noise_only.cu8", &n_noise);
    rdof_cf_t *bs = load_cu8("demod_bs_both_slots.cu8", &n_bs);

    /* the cases of the vector table with no squelch: the squelch only saves work, it decides nothing the sync
     * search wouldn't */
    for (size_t t = 0; t < V_DEMOD_N; t++) {
        const struct v_demod *v = &v_demod[t];
        char name[128];
        int n;
        snprintf(name, sizeof name, "demod_%s.cu8", v->name);
        rdof_cf_t *x = load_cu8(name, &n);
        rdof_demod_t d, q;
        CHECK(rdof_demod_init(&d, v->cc, v->default_slot, RDOF_DEMOD_SQUELCH_HZ, v->chunk) == 0, "init");
        CHECK(rdof_demod_init(&q, v->cc, v->default_slot, 0.0, v->chunk) == 0, "init");
        run_demod(&d, x, n, v->chunk, 0, 0, &ref);
        run_demod(&q, x, n, v->chunk, 0, 0, &other);
        CHECK(ref.n == v->count && same_bursts(&ref, &other), "%s: %d bursts with the squelch, %d without (want %d)",
              v->name, ref.n, other.n, v->count);
        rdof_demod_free(&d);
        rdof_demod_free(&q);
        free(x);
    }

    /* skipping what holds nothing: ahead of a call, quiet samples skipped (in pieces smaller than the FM stage's
     * history, and larger) give the bursts, positions and levels feeding them would have */
    const int quiet = 9600;
    rdof_cf_t *cat = malloc(sizeof(rdof_cf_t) * (size_t)(quiet + n_bs));
    memcpy(cat, noisy, sizeof(rdof_cf_t) * quiet);
    memcpy(cat + quiet, bs, sizeof(rdof_cf_t) * (size_t)n_bs);
    const int total = quiet + n_bs;
    rdof_demod_t d, q;
    for (int variant = 0; variant < 4; variant++) {
        static const int pieces[] = {2400, 7, 39, 1000};
        CHECK(rdof_demod_init(&d, 3, 1, RDOF_DEMOD_SQUELCH_HZ, 2400) == 0, "init");
        CHECK(rdof_demod_init(&q, 3, 1, RDOF_DEMOD_SQUELCH_HZ, 2400) == 0, "init");
        run_demod(&d, cat, total, 2400, 0, 0, &ref);
        run_demod(&q, cat, total, 2400, quiet, pieces[variant], &other);
        CHECK(ref.n >= 30 && same_bursts(&ref, &other), "skipping in pieces of %d: %d bursts, %d fed", pieces[variant],
              other.n, ref.n);
        CHECK(q.samples_in == d.samples_in && q.base + q.len == d.base + d.len, "same stream length");
        rdof_demod_free(&d);
        rdof_demod_free(&q);
    }
    /* skipping more than max_feed is clamped to it, and so is feeding */
    CHECK(rdof_demod_init(&d, 2, 1, RDOF_DEMOD_SQUELCH_HZ, 1000) == 0, "init");
    rdof_demod_skip(&d, cat, 5000);
    CHECK(d.samples_in == 1000, "skip clamped: %lld", (long long)d.samples_in);
    int nb;
    rdof_demod_feed(&d, cat, 5000, &nb);
    CHECK(d.samples_in == 2000, "feed clamped: %lld", (long long)d.samples_in);
    rdof_demod_free(&d);

    /* a call is in progress between its first sync and its last burst, and the demod says so */
    CHECK(rdof_demod_init(&d, 3, 1, RDOF_DEMOD_SQUELCH_HZ, 2400) == 0, "init");
    CHECK(!rdof_demod_busy(&d), "a new demod isn't busy");
    run_demod(&d, bs, n_bs, 2400, 0, 0, &ref);
    CHECK(ref.busy_chunks > 3, "busy during %d chunks of a voice call", ref.busy_chunks);
    rdof_demod_free(&d);

    /* the CPU profile with a clock: per feed the FM stage, the buffer move and the flywheel read the clock once
     * each, so 10 ticks a feed; the search accounts for the rest */
    memset(&rdof_demod_prof, 0, sizeof rdof_demod_prof);
    dclk = 0;
    rdof_demod_set_clock(dtick);
    CHECK(rdof_demod_init(&d, 3, 1, RDOF_DEMOD_SQUELCH_HZ, 2400) == 0, "init");
    run_demod(&d, bs, n_bs, 2400, 0, 0, &other);
    rdof_demod_set_clock(NULL);
    const int feeds = (n_bs + 2399) / 2400;
    CHECK(same_bursts(&other, &ref), "the profile doesn't change the bursts");
    CHECK(rdof_demod_prof.fm == 10u * (uint64_t)feeds && rdof_demod_prof.fly == 10u * (uint64_t)feeds &&
              rdof_demod_prof.compact == 10u * (uint64_t)feeds,
          "fm %llu fly %llu compact %llu over %d feeds", (unsigned long long)rdof_demod_prof.fm,
          (unsigned long long)rdof_demod_prof.fly, (unsigned long long)rdof_demod_prof.compact, feeds);
    CHECK(rdof_demod_prof.positions > 1000 && rdof_demod_prof.scored > 0 && rdof_demod_prof.scored < rdof_demod_prof.positions &&
              rdof_demod_prof.syncs >= 10 && rdof_demod_prof.syncs <= (uint64_t)other.n && rdof_demod_prof.pre > 0 && rdof_demod_prof.sync > 0 &&
              rdof_demod_prof.at_sync > 0 && rdof_demod_prof.sync_fly > 0 && rdof_demod_prof.search > 0,
          "search profile: %llu positions, %llu scored, %llu syncs, pre %llu sync %llu at_sync %llu sync_fly %llu search %llu",
          (unsigned long long)rdof_demod_prof.positions, (unsigned long long)rdof_demod_prof.scored,
          (unsigned long long)rdof_demod_prof.syncs, (unsigned long long)rdof_demod_prof.pre,
          (unsigned long long)rdof_demod_prof.sync, (unsigned long long)rdof_demod_prof.at_sync,
          (unsigned long long)rdof_demod_prof.sync_fly, (unsigned long long)rdof_demod_prof.search);
    memset(&rdof_demod_prof, 0, sizeof rdof_demod_prof);
    run_demod(&d, bs, 2400, 2400, 0, 0, &other);
    CHECK(rdof_demod_prof.fm == 0 && rdof_demod_prof.positions == 0, "no clock, no profile");
    rdof_demod_free(&d);
    free(cat);
    free(noisy);
    free(bs);
}

/* ── the receiver ────────────────────────────────────────────────────────── */

struct nsink {
    int json, raw;
};
static void nsink_json(void *ctx, const char *ch, const char *j) { ((struct nsink *)ctx)->json++; }
static void nsink_raw(void *ctx, const char *ch, const char *j) { ((struct nsink *)ctx)->raw++; }

static void test_rx_edges(void)
{
    static rdof_sdr_rx_t rx;
    const rdof_sdr_rx_config_t good = {FS, CENTER, -60.0, 0, true, CHUNK};
    rdof_sdr_rx_config_t cfg;

    /* what init refuses: no channels, no chunk, a rate that isn't a multiple of 48 kS/s, channels no window holds */
    CHECK(rdof_sdr_rx_init(&rx, &good, scene_chans, 0, NULL, 0) == -1, "no channels");
    rdof_sdr_rx_free(&rx);
    cfg = good;
    cfg.max_chunk = 0;
    CHECK(rdof_sdr_rx_init(&rx, &cfg, scene_chans, 3, NULL, 0) == -1, "no chunk");
    rdof_sdr_rx_free(&rx);
    cfg = good;
    cfg.sample_rate = 250000;
    CHECK(rdof_sdr_rx_init(&rx, &cfg, scene_chans, 3, NULL, 0) == -1, "250 kS/s");
    rdof_sdr_rx_free(&rx);
    cfg = good;
    cfg.center_hz = 0.0;
    const rdof_sdr_channel_t far[2] = {{"A", 461.0e6, 1, 1}, {"B", 461.5e6, 1, 1}};
    CHECK(rdof_sdr_rx_init(&rx, &cfg, far, 2, NULL, 0) == -1, "channels too far apart to plan a center");
    rdof_sdr_rx_free(&rx);
    CHECK(rdof_sdr_rx_init(&rx, &cfg, scene_chans, 3, NULL, 0) == 0 && rx.center_hz != 0.0, "center planned");
    rdof_sdr_rx_free(&rx);
    rdof_sdr_rx_free(&rx);

    /* sinks: one per call to add, a bounded number a channel, none for a channel that isn't there */
    struct nsink a = {0, 0}, b = {0, 0};
    rdof_sink_t sa = {.on_json = nsink_json, .on_raw = nsink_raw, .ctx = &a};
    rdof_sink_t sb = {.on_json = nsink_json, .on_raw = nsink_raw, .ctx = &b};
    CHECK(rdof_sdr_rx_init(&rx, &good, scene_chans, 3, &sa, 0) == 0, "init with a sink");
    CHECK(!rdof_sdr_rx_add_sink(&rx, -1, &sb) && !rdof_sdr_rx_add_sink(&rx, 3, &sb), "no such channel");
    int added = 0;
    while (rdof_sdr_rx_add_sink(&rx, 1, &sb) && added < 20)
        added++;
    CHECK(added == RDOF_TRACKER_MAX_SINKS - 1, "%d more sinks on a channel with one", added);
    CHECK(rdof_sdr_rx_add_sink(&rx, 0, &sb), "and one for channel 0");
    rdof_sdr_rx_free(&rx);

    /* more samples than a chunk is clamped to the chunk, whichever way in */
    static rdof_cf_t big[2 * CHUNK];
    for (int i = 0; i < 2 * CHUNK; i++) {
        big[i].re = 0.01f * noise();
        big[i].im = 0.01f * noise();
    }
    CHECK(rdof_sdr_rx_init(&rx, &good, scene_chans, 3, NULL, 0) == 0, "init");
    rdof_sdr_rx_channelize(&rx, big, 2 * CHUNK);
    CHECK(rx.samples == CHUNK, "channelize took %lld", rx.samples);
    rdof_sdr_rx_forward(&rx, big, 2 * CHUNK);
    CHECK(rx.samples == 2 * CHUNK, "forward took %lld more", rx.samples - CHUNK);
    rdof_sdr_rx_free(&rx);

    /* the software ppm: every channel starts shifted by its frequency times the ppm */
    cfg = good;
    cfg.software_ppm = 12.5;
    CHECK(rdof_sdr_rx_init(&rx, &cfg, scene_chans, 3, NULL, 0) == 0, "init");
    for (int c = 0; c < 3; c++)
        CHECK(fabs(rx.chz.correction_hz[c] - scene_chans[c].freq_hz * 12.5e-6) < 1e-9 &&
                  rdof_sdr_rx_correction_at(&rx, c, 0) == rx.chz.correction_hz[c],
              "channel %d starts at %g Hz", c, rx.chz.correction_hz[c]);
    rdof_sdr_rx_free(&rx);

    /* the AFC's history: 32 corrections a channel, each from the channel sample it started at */
    CHECK(rdof_sdr_rx_init(&rx, &good, scene_chans, 3, NULL, 0) == 0, "init");
    CHECK(rdof_sdr_rx_correction_at(&rx, 1, -5) == 0.0 && rdof_sdr_rx_correction_at(&rx, 1, 1000000) == 0.0,
          "before the first entry, and long after");
    for (int k = 1; k <= 40; k++) {
        rx.out_index = k * 100;
        rdof_sdr_rx_set_correction(&rx, 1, (double)k);
    }
    CHECK(rx.hist_n[1] == RDOF_AFC_HISTORY && rx.chz.correction_hz[1] == 40.0, "history full at %d", rx.hist_n[1]);
    CHECK(rdof_sdr_rx_correction_at(&rx, 1, 4000) == 40.0 && rdof_sdr_rx_correction_at(&rx, 1, 3999) == 39.0 &&
              rdof_sdr_rx_correction_at(&rx, 1, 3950) == 39.0 && rdof_sdr_rx_correction_at(&rx, 1, 1000) == 10.0,
          "lookups: %g %g %g %g", rdof_sdr_rx_correction_at(&rx, 1, 4000), rdof_sdr_rx_correction_at(&rx, 1, 3999),
          rdof_sdr_rx_correction_at(&rx, 1, 3950), rdof_sdr_rx_correction_at(&rx, 1, 1000));
    /* the oldest kept is correction 9 (40 - 32 + 1), from sample 900: anything older reads it too */
    CHECK(rdof_sdr_rx_correction_at(&rx, 1, 900) == 9.0 && rdof_sdr_rx_correction_at(&rx, 1, 0) == 9.0 &&
              rdof_sdr_rx_correction_at(&rx, 1, -1000) == 9.0,
          "older than the history: %g", rdof_sdr_rx_correction_at(&rx, 1, 0));
    CHECK(rdof_sdr_rx_correction_at(&rx, 0, 4000) == 0.0, "another channel's history is its own");
    rdof_sdr_rx_free(&rx);

    /* AFC: a transmitter 400 Hz high pulls the correction up; off, or with its bursts muted (our own
     * transmitter's), it measures nothing */
    static scene_out_t base, hi, noafc, muted, limit_hi, limit_lo;
    scene_opt_t opt = {.mode = 0, .brief = true};
    run_scene_opt(&opt, &base);
    opt.tx_shift_hz = 400;
    run_scene_opt(&opt, &hi);
    CHECK(hi.bursts[0] == base.bursts[0] && hi.corr[0] > 50 && hi.corr[0] < 450 && fabs(base.corr[0]) < 50,
          "400 Hz high: correction %.1f Hz (%.1f unshifted), %ld bursts", hi.corr[0], base.corr[0], hi.bursts[0]);
    /* and one 900 Hz low pulls it down, by no more than a step a chunk (RDOF_AFC_STEP_HZ for 0.1 s of samples) */
    static scene_out_t lo;
    opt.tx_shift_hz = -900;
    run_scene_opt(&opt, &lo);
    CHECK(lo.bursts[0] == base.bursts[0] && lo.corr[0] < -100 && lo.corr[0] > -1000, "900 Hz low: %.1f Hz", lo.corr[0]);
    opt.tx_shift_hz = 400;
    opt.no_afc = true;
    run_scene_opt(&opt, &noafc);
    CHECK(noafc.corr[0] == 0.0 && noafc.bursts[0] == base.bursts[0], "AFC off: correction %g", noafc.corr[0]);
    opt.no_afc = false;
    opt.muted = true;
    run_scene_opt(&opt, &muted);
    CHECK(muted.corr[0] == 0.0 && muted.bursts[0] == base.bursts[0], "muted bursts measure nothing: %g", muted.corr[0]);
    /* the correction never leaves +-5 kHz: 300 Hz off a transmitter 5300 Hz away, and the other way */
    opt = (scene_opt_t){.mode = 0, .tx_shift_hz = 5300, .preset_hz = 5000, .brief = true};
    run_scene_opt(&opt, &limit_hi);
    CHECK(limit_hi.bursts[0] >= 5 && limit_hi.corr[0] == RDOF_AFC_LIMIT_HZ, "high limit: %.1f Hz, %ld bursts",
          limit_hi.corr[0], limit_hi.bursts[0]);
    opt = (scene_opt_t){.mode = 0, .tx_shift_hz = -5300, .preset_hz = -5000, .brief = true};
    run_scene_opt(&opt, &limit_lo);
    CHECK(limit_lo.bursts[0] >= 5 && limit_lo.corr[0] == -RDOF_AFC_LIMIT_HZ, "low limit: %.1f Hz, %ld bursts",
          limit_lo.corr[0], limit_lo.bursts[0]);
}

/* ── the front end's checks ──────────────────────────────────────────────── */

static void test_frontend_edges(void)
{
    char err[160];
    CHECK(!strcmp(rdof_fe_type_name((rdof_fe_type_t)99), "?") && !strcmp(rdof_fe_type_name(RDOF_FE_RTLSDR), "rtlsdr") &&
              !strcmp(rdof_fe_type_name(RDOF_FE_LR2021), "lr2021") && !strcmp(rdof_fe_type_name(RDOF_FE_SIM), "sim"),
          "type names");
    CHECK(rdof_fe_type_parse("") == RDOF_FE_RTLSDR && rdof_fe_type_parse("sx1255") == RDOF_FE_SX1255 &&
              rdof_fe_type_parse("RTLSDR") == -1 && rdof_fe_type_parse("rtlsdr ") == -1,
          "type parse: empty is the default, the rest is exact");
    static const double three[] = {240000, 250000, 288000};
    rdof_fe_caps_t list = {.kind = "X", .rates = three, .n_rates = 3, .freq_min = 100e6, .freq_max = 500e6};
    const double ok[1] = {461e6}, low[1] = {99.9e6}, high[1] = {500.1e6};
    CHECK(rdof_fe_check(&list, 250000.4, ok, 1, err, sizeof err) == 0, "a rate within half a sample/second");
    CHECK(rdof_fe_check(&list, 250001, ok, 1, err, sizeof err) < 0 && !strcmp(err, "the X takes sample_rate 240000, 250000 or 288000"),
          "three rates: %s", err);
    /* a message that doesn't fit is cut short, never past the buffer */
    char small[24];
    memset(small, 'x', sizeof small);
    CHECK(rdof_fe_check(&list, 250001, ok, 1, small, sizeof small) < 0 && memchr(small, 0, sizeof small) &&
              !strncmp(small, "the X takes sample_rate", 23),
          "a short buffer: %.24s", small);
    rdof_fe_caps_t range = {.kind = "Y", .rate_min = 48000, .rate_max = 192000, .freq_min = 100e6, .freq_max = 500e6};
    CHECK(rdof_fe_check(&range, 47999.6, ok, 1, err, sizeof err) == 0 && rdof_fe_check(&range, 192000.4, ok, 1, err, sizeof err) == 0,
          "range edges, within half a sample/second");
    CHECK(rdof_fe_check(&range, 47999.4, ok, 1, err, sizeof err) < 0 && strstr(err, "(got 47999)"), "under: %s", err);
    CHECK(rdof_fe_check(&range, 192000.6, ok, 1, err, sizeof err) < 0 && strstr(err, "(got 192001)"), "over: %s", err);
    CHECK(rdof_fe_check(&range, 96000, low, 1, err, sizeof err) < 0 && strstr(err, "99.90000 MHz is out of its range") &&
              strstr(err, "100.000 to 500.000 MHz"),
          "below the tuner: %s", err);
    CHECK(rdof_fe_check(&range, 96000, high, 1, err, sizeof err) < 0 && strstr(err, "500.10000 MHz"), "above: %s", err);
    const double mixed[3] = {461e6, 300e6, 600e6};
    CHECK(rdof_fe_check(&range, 96000, mixed, 3, err, sizeof err) < 0 && strstr(err, "600.00000 MHz"),
          "the first one out of range is named: %s", err);
    CHECK(rdof_fe_check(&range, 96000, mixed, 0, err, sizeof err) == 0, "no frequencies");
    CHECK(rdof_fe_sample_bytes(RDOF_FE_CS8) == 2 && rdof_fe_fill_byte(RDOF_FE_CS8) == 0, "cs8 sample size and filler");
}

/* ── the modulator ───────────────────────────────────────────────────────── */

static void test_mod_edges(void)
{
    rdof_mod_t m;
    /* what init refuses */
    CHECK(rdof_mod_init(&m, 0, 0, 4, 1, 4, 0) == -1, "fs 0");
    CHECK(rdof_mod_init(&m, 288000, 0, -0.1, 1, 4, 0) == -1, "ramp under 0");
    CHECK(rdof_mod_init(&m, 288000, 0, 6.1, 1, 4, 0) == -1, "ramp over 6 symbols");
    CHECK(rdof_mod_init(&m, 288000, 0, NAN, 1, 4, 0) == -1, "ramp NaN");
    CHECK(rdof_mod_init(&m, 288000, 0, 4, 1, 0, 0) == -1, "no queue");
    CHECK(rdof_mod_init(&m, 4800, 0, 4, 1, 4, 0) == -1, "one sample a symbol");
    CHECK(rdof_mod_init(&m, 4800u * 1025u, 0, 4, 1, 4, 0) == -1, "1025 phases");
    CHECK(rdof_mod_init(&m, 288000, 0, 0.0, 1, 4, 0) == 0 && m.inv_ramp == 0.0f, "ramp 0 is allowed");
    rdof_mod_free(&m);
    CHECK(rdof_mod_init(&m, 288000, 0, 6.0, 1, 4, 0) == 0, "ramp 6 is allowed");
    rdof_mod_free(&m);
    CHECK(rdof_mod_init(&m, 9600, 0, 4, 1, 1, 0) == 0 && m.P == 2 && m.Q == 1, "2 samples a symbol is the least");
    rdof_mod_free(&m);

    /* the offset: idle, the frequency out is the offset, to the phase accumulator's step */
    float hz[8];
    CHECK(rdof_mod_init(&m, 288000, 0, 4, 1, 4, 0) == 0, "init");
    static const double offsets[] = {1234.5, -1234.5, 0.0, -87500.0, 87500.0};
    for (size_t i = 0; i < sizeof offsets / sizeof offsets[0]; i++) {
        rdof_mod_set_offset(&m, offsets[i]);
        rdof_mod_freq(&m, hz, NULL, 8);
        CHECK(fabs(hz[7] - offsets[i]) < 1e-3 && m.offset_hz == offsets[i], "offset %g reads %g", offsets[i], hz[7]);
    }
    rdof_mod_free(&m);

    /* the queue: idle until the last queued sample is out; room again once the oldest has played */
    uint8_t burst[RDOF_BURST_BYTES];
    rdof_csbk_preambles(1, 2, true, 1, 1, (uint8_t(*)[RDOF_BURST_BYTES])burst);
    CHECK(rdof_mod_init(&m, 288000, 0, 4, 1, 2, 0) == 0, "init");
    const int64_t slot = rdof_mod_slot_samples(&m);
    CHECK(rdof_mod_idle(&m), "nothing queued");
    CHECK(rdof_mod_queue_burst(&m, 2 * slot, burst, NULL, RDOF_MOD_RAMPS) == 0 && !rdof_mod_idle(&m), "one queued");
    CHECK(rdof_mod_queue_burst(&m, 4 * slot, burst, NULL, RDOF_MOD_RAMPS) == 0, "two");
    CHECK(rdof_mod_queue_burst(&m, 6 * slot, burst, NULL, RDOF_MOD_RAMPS) == -1, "a third doesn't fit a queue of 2");
    CHECK(rdof_mod_queue(&m, 7 * slot, NULL, 0, 0, 0) == -1, "no symbols");
    rdof_cf_t out[1000];
    int64_t done = 0;
    while (done < 3 * slot) { /* past the first burst's last sample, not the second's */
        rdof_mod_iq(&m, out, 1000);
        done += 1000;
    }
    CHECK(!rdof_mod_idle(&m), "the second is still to play");
    CHECK(rdof_mod_queue_burst(&m, 6 * slot, burst, NULL, RDOF_MOD_RAMPS) == 0, "the first has played: room");
    while (done < 8 * slot) {
        rdof_mod_iq(&m, out, 1000);
        done += 1000;
    }
    CHECK(rdof_mod_idle(&m) && m.qn >= 0, "all played");
    rdof_mod_free(&m);
    rdof_mod_free(&m);
}

/* ── the resampler, the FFT and the spectrum ─────────────────────────────── */

static void test_resamp_edges(void)
{
    rdof_resamp_t r, q;
    CHECK(rdof_resamp_init(&r, 0, 288000, 32, 0.8, 100) == -1, "in rate 0");
    CHECK(rdof_resamp_init(&r, 1536000, -1, 32, 0.8, 100) == -1, "out rate negative");
    CHECK(rdof_resamp_init(&r, 1536000, 288000, 1, 0.8, 100) == -1, "one tap a phase");
    CHECK(rdof_resamp_init(&r, 1536000, 288000, 32, 0.8, 0) == -1, "no input block");
    CHECK(rdof_resamp_init(&r, 1000003, 7, 32, 0.8, 100) == -1, "a ratio past 1000");
    CHECK(rdof_resamp_init(&r, 7, 1000003, 32, 0.8, 100) == -1, "and the other way");
    rdof_resamp_free(&r);
    /* 8 in, 3 out; more than the block size is cut to it, so a long run is a run of max_in */
    CHECK(rdof_resamp_init(&r, 8000, 3000, 24, 0.8, 100) == 0 && r.L == 3 && r.M == 8, "ratio 3/8");
    CHECK(rdof_resamp_init(&q, 8000, 3000, 24, 0.8, 100) == 0, "init");
    static rdof_cf_t in[300], o1[200], o2[200];
    for (int i = 0; i < 300; i++) {
        in[i].re = noise();
        in[i].im = noise();
    }
    int n1 = rdof_resamp_run(&r, in, 300, o1);
    int n2 = rdof_resamp_run(&q, in, 100, o2);
    CHECK(n1 == n2 && n1 > 30 && !memcmp(o1, o2, sizeof(rdof_cf_t) * (size_t)n1) && n1 <= rdof_resamp_max_out(&r, 100),
          "300 inputs to a 100-input block: %d outputs against %d", n1, n2);
    rdof_resamp_free(&r);
    rdof_resamp_free(&q);
    rdof_resamp_free(&q);
}

/* A tone of f Hz through the resampler, its level at the output in dB (0: unchanged) past the start-up. */
static double resamp_tone_db(long fin, long fout, int K, double f)
{
    rdof_resamp_t r;
    enum { N = 8000 };
    static rdof_cf_t x[N], y[3 * N];
    if (rdof_resamp_init(&r, fin, fout, K, 0.8, N))
        return NAN;
    for (int i = 0; i < N; i++) {
        const double ph = 2 * M_PI * f * i / fin;
        x[i].re = (float)(0.5 * cos(ph));
        x[i].im = (float)(0.5 * sin(ph));
    }
    const int n = rdof_resamp_run(&r, x, N, y);
    rdof_resamp_free(&r);
    double re = 0, im = 0;
    int cnt = 0;
    for (int i = 600; i < n; i++, cnt++) {
        const double ph = -2 * M_PI * f * i / fout;
        re += y[i].re * cos(ph) - y[i].im * sin(ph);
        im += y[i].re * sin(ph) + y[i].im * cos(ph);
    }
    return 20 * log10(hypot(re, im) / cnt / 0.5);
}

static void test_resamp_tones(void)
{
    /* up: the same level and frequency out, across the band the input could carry */
    double worst = 0;
    for (double f = -17e3; f <= 17e3; f += 3400)
        worst = fmax(worst, fabs(resamp_tone_db(48000, 96000, 32, f)));
    CHECK(worst < 0.1, "48 to 96 kS/s: worst %.3f dB", worst);
    CHECK(fabs(resamp_tone_db(240000, 288000, 40, 60e3)) < 0.1, "240 to 288 kS/s");
    /* an odd number of taps in the prototype (3 phases of 63): its middle tap is the sinc's center */
    rdof_resamp_t r;
    CHECK(rdof_resamp_init(&r, 8000, 3000, 63, 0.8, 100) == 0 && r.L * r.K % 2 == 1, "odd length prototype");
    rdof_resamp_free(&r);
    worst = 0;
    for (double f = -1000; f <= 1000; f += 250)
        worst = fmax(worst, fabs(resamp_tone_db(8000, 3000, 63, f)));
    CHECK(worst < 0.1, "8 to 3 kS/s with 189 taps: worst %.3f dB", worst);
}

static void test_fft_edges(void)
{
    CHECK(rdof_fft_new(0, false) == NULL && rdof_fft_new(-4, true) == NULL, "no FFT of nothing");
    CHECK(rdof_fft_new(7, false) == NULL && rdof_fft_new(22, true) == NULL, "primes past 5 are refused");
    rdof_fft_t *f = rdof_fft_new(1, false);
    CHECK(f != NULL, "one point");
    rdof_cf_t in = {3.5f, -2.0f}, out = {0, 0};
    rdof_fft_run(f, &in, &out);
    CHECK(out.re == 3.5f && out.im == -2.0f, "an FFT of one point is the point");
    rdof_fft_free(f);
    rdof_fft_free(NULL);
    /* the inverse is the forward's conjugate: ifft(fft(x)) = n x */
    rdof_fft_t *a = rdof_fft_new(60, false), *b = rdof_fft_new(60, true);
    rdof_cf_t x[60], X[60], y[60];
    for (int i = 0; i < 60; i++) {
        x[i].re = noise();
        x[i].im = noise();
    }
    rdof_fft_run(a, x, X);
    rdof_fft_run(b, X, y);
    double e = 0;
    for (int i = 0; i < 60; i++)
        e = fmax(e, hypot(y[i].re / 60 - x[i].re, y[i].im / 60 - x[i].im));
    CHECK(e < 1e-5, "round trip off by %g", e);
    rdof_fft_free(a);
    rdof_fft_free(b);

    rdof_spectrum_t s;
    CHECK(rdof_spectrum_init(&s, 7, 1) == -1 && rdof_spectrum_init(&s, 0, 1) == -1, "spectrum sizes");
    CHECK(rdof_spectrum_init(&s, 64, 0) == 0 && s.per_row == 1, "per_row under 1 is 1");
    rdof_cf_t sig[64];
    uint8_t row[64];
    for (int i = 0; i < 64; i++) {
        sig[i].re = (float)cos(2 * M_PI * 8 * i / 64.0);
        sig[i].im = (float)sin(2 * M_PI * 8 * i / 64.0);
    }
    CHECK(rdof_spectrum_feed(&s, sig, 64, row), "a row at once");
    int peak = 0;
    for (int i = 1; i < 64; i++)
        peak = row[i] < row[peak] ? i : peak; /* lowest value = strongest */
    CHECK(peak == 32 + 8 && row[peak] <= 1, "a full scale tone at bin 8 (row index %d) reads %d", peak, row[peak]);
    /* a short block is zero padded; a very long one uses the most FFTs a chunk may */
    CHECK(rdof_spectrum_feed(&s, sig, 10, row) && s.ffts == 0, "short block");
    static rdof_cf_t longer[64 * 10];
    for (int i = 0; i < 640; i++)
        longer[i] = sig[i % 64];
    rdof_spectrum_feed(&s, longer, 640, row);
    rdof_spectrum_free(&s);
    rdof_spectrum_free(&s);
}

static void test_rate_edges(void)
{
    rdof_rate_t r;
    double ppm = 123;
    rdof_rate_init(&r);
    CHECK(!rdof_rate_recent(&r, 60, &ppm) && ppm == 123, "no points, no rate");
    for (int i = 0; i < 12; i++)
        rdof_rate_point(&r, i * 10.0, 2.0 * i * 10.0); /* later by 2 us every second: the samples are 2 ppm slow */
    CHECK(rdof_rate_recent(&r, 1000, &ppm) && fabs(ppm + 2.0) < 1e-6, "the whole fit: %g", ppm);
    CHECK(!rdof_rate_recent(&r, 40, &ppm), "5 points in the last 40 s: too few");
    CHECK(rdof_rate_recent(&r, 60, &ppm), "7 points in the last 60 s");
    rdof_rate_trim(&r, 1e9);
    CHECK(r.count == 0, "trimmed to nothing");
    rdof_rate_trim(&r, 0);
    CHECK(r.count == 0 && !rdof_rate_recent(&r, 60, &ppm), "trimming nothing");
}

/* ── the settings: what the reference table can't say ────────────────────── */

static bool parse(const char *json, rdof_sdr_config_t *cfg, char *err, size_t cap)
{
    return rdof_sdr_config_parse(json, strlen(json), cfg, err, cap);
}

static void test_config_edges(void)
{
    static rdof_sdr_config_t cfg;
    char err[200];
    const char *one = "\"channels\":[{\"id\":\"A\",\"freq\":461.0375}]";
    char buf[8192];

    /* not settings at all */
    CHECK(!parse("{", &cfg, err, sizeof err) && !strncmp(err, "JSON: ", 6), "bad JSON: %s", err);
    CHECK(!parse("[1,2]", &cfg, err, sizeof err) && !strcmp(err, "the settings must be a JSON object"), "array: %s", err);
    CHECK(!parse("", &cfg, err, sizeof err) && !strncmp(err, "JSON: ", 6), "empty: %s", err);
    /* an error with nowhere to put it, and with room for a few characters */
    CHECK(!rdof_sdr_config_parse("{", 1, &cfg, NULL, 0), "no error buffer");
    /* device.settings.iq_bits: 8 or 16, absent 0 */
    snprintf(buf, sizeof buf, "{\"device\":{\"settings\":{\"iq_bits\":16}},%s}", one);
    CHECK(parse(buf, &cfg, err, sizeof err) && cfg.iq_bits == 16, "iq_bits 16: %s", err);
    snprintf(buf, sizeof buf, "{\"device\":{\"settings\":{\"iq_bits\":8}},%s}", one);
    CHECK(parse(buf, &cfg, err, sizeof err) && cfg.iq_bits == 8, "iq_bits 8: %s", err);
    snprintf(buf, sizeof buf, "{%s}", one);
    CHECK(parse(buf, &cfg, err, sizeof err) && cfg.iq_bits == 0, "iq_bits absent: %s", err);
    snprintf(buf, sizeof buf, "{\"device\":{\"settings\":{\"iq_bits\":12}},%s}", one);
    CHECK(!parse(buf, &cfg, err, sizeof err) && !strcmp(err, "device.settings.iq_bits must be 8 or 16"), "iq_bits 12: %s",
          err);
    snprintf(buf, sizeof buf, "{\"device\":{\"settings\":{\"iq_bits\":\"16\"}},%s}", one);
    CHECK(parse(buf, &cfg, err, sizeof err) && cfg.iq_bits == 16, "iq_bits as a string, as numbers are elsewhere: %s",
          err);
    char tiny[6];
    CHECK(!parse("[1]", &cfg, tiny, sizeof tiny) && !strcmp(tiny, "the s"), "five characters of the error: %s", tiny);
    memset(tiny, 'x', sizeof tiny);
    CHECK(!parse("[1]", &cfg, tiny, 0) && tiny[0] == 'x', "a zero-size buffer is left alone");

    /* JSON with comments: at the very end, inside and outside strings, a lone slash, nested trailing commas */
    snprintf(buf, sizeof buf, "{\"name\":\"a//b/*c*/\", /* one */ \"device\":{\"sample_rate\":288000,}, %s,} // end", one);
    CHECK(parse(buf, &cfg, err, sizeof err) && !strcmp(cfg.name, "a//b/*c*/") && cfg.nch == 1, "comments: %s", err);
    snprintf(buf, sizeof buf, "{\"device\":{\"sample_rate\":288000}, %s} /", one);
    CHECK(!parse(buf, &cfg, err, sizeof err), "a stray slash after the object is an error");
    snprintf(buf, sizeof buf, "{\"device\":{\"sample_rate\":288000}, %s} /* never closed", one);
    CHECK(parse(buf, &cfg, err, sizeof err), "an unterminated block comment is dropped: %s", err);
    snprintf(buf, sizeof buf, "{\"name\":\"x\\\\\",\"channels\":[{\"id\":\"A\\\"B\",\"freq\":461.0375},],}");
    CHECK(parse(buf, &cfg, err, sizeof err) && !strcmp(cfg.name, "x\\") && !strcmp(cfg.ch[0].id, "A\"B"),
          "escapes in strings: %s / %s", cfg.name, cfg.ch[0].id);

    /* more than the board holds, in a settings file and in a box */
    size_t o = (size_t)snprintf(buf, sizeof buf, "{\"device\":{\"sample_rate\":288000},\"channels\":[");
    for (int i = 0; i < RDOF_SDR_MAX_CHANNELS + 1; i++)
        o += (size_t)snprintf(buf + o, sizeof buf - o, "%s{\"id\":\"C%d\",\"freq\":%d}", i ? "," : "", i,
                              461000000 + 12500 * i);
    snprintf(buf + o, sizeof buf - o, "]}");
    CHECK(!parse(buf, &cfg, err, sizeof err) && !strcmp(err, "17 channels: this board takes 16"), "17 channels: %s", err);
    o = (size_t)snprintf(buf, sizeof buf, "{\"cc\":1,\"channels\":{");
    for (int i = 0; i < RDOF_SDR_MAX_CHANNELS + 1; i++)
        o += (size_t)snprintf(buf + o, sizeof buf - o, "%s\"%d\":%d", i ? "," : "", i + 1, 461000000 + 12500 * i);
    o += (size_t)snprintf(buf + o, sizeof buf - o, "},\"radios\":[{\"type\":\"sdr\",\"name\":\"s\",\"channels\":[");
    for (int i = 0; i < RDOF_SDR_MAX_CHANNELS + 1; i++)
        o += (size_t)snprintf(buf + o, sizeof buf - o, "%s%d", i ? "," : "", i + 1);
    snprintf(buf + o, sizeof buf - o, "]}]}");
    CHECK(!parse(buf, &cfg, err, sizeof err) && !strcmp(err, "17 channels: this board takes 16"), "17 in a box: %s", err);
    /* 16 do fit */
    o = (size_t)snprintf(buf, sizeof buf, "{\"device\":{\"sample_rate\":288000},\"channels\":[");
    for (int i = 0; i < RDOF_SDR_MAX_CHANNELS; i++)
        o += (size_t)snprintf(buf + o, sizeof buf - o, "%s{\"id\":\"C%d\",\"freq\":%d}", i ? "," : "", i,
                              461000000 + 12500 * i);
    snprintf(buf + o, sizeof buf - o, "]}");
    CHECK(parse(buf, &cfg, err, sizeof err) && cfg.nch == 16, "16 channels: %s", err);
    /* a document with more nodes than the parser has room for, or more text */
    o = (size_t)snprintf(buf, sizeof buf, "{\"x\":[");
    for (int i = 0; i < 1500 && o + 8 < sizeof buf; i++)
        o += (size_t)snprintf(buf + o, sizeof buf - o, "%s1", i ? "," : "");
    snprintf(buf + o, sizeof buf - o, "]}");
    CHECK(!parse(buf, &cfg, err, sizeof err) && !strncmp(err, "JSON: ", 6), "too many values: %s", err);

    /* what the board refuses that the reference doesn't look at */
    snprintf(buf, sizeof buf, "{\"device\":{\"sample_rate\":288000,\"agc\":true}, %s}", one);
    CHECK(!parse(buf, &cfg, err, sizeof err) && strstr(err, "device.agc needs a fixed device.gain"), "agc without gain: %s", err);
    snprintf(buf, sizeof buf, "{\"device\":{\"sample_rate\":288000,\"agc\":{\"min\":10},\"gain\":40}, %s}", one);
    CHECK(parse(buf, &cfg, err, sizeof err) && cfg.agc && cfg.agc_cfg[0] == 10.0 && cfg.agc_cfg[2] == 6.0 &&
              cfg.has_gain && cfg.gain == 40.0,
          "agc with a gain and one setting: %s", err);
    /* rdof_agc_config_default is the dataclass's, and so are the board settings' defaults */
#if RDOF_FEATURE_SDR_AGC
    rdof_agc_config_t ac;
    rdof_agc_config_default(&ac);
    CHECK(ac.min == v_agc_defaults[0] && ac.clip == v_agc_defaults[1] && ac.headroom_db == v_agc_defaults[2] &&
              ac.hold_s == v_agc_defaults[3] && ac.relax_s == v_agc_defaults[4] && ac.settle_s == v_agc_defaults[5],
          "AgcConfig defaults");
    snprintf(buf, sizeof buf, "{\"device\":{\"sample_rate\":288000,\"agc\":true,\"gain\":40}, %s}", one);
    CHECK(parse(buf, &cfg, err, sizeof err), "agc: true: %s", err);
    for (int i = 0; i < 6; i++)
        CHECK(cfg.agc_cfg[i] == v_agc_defaults[i], "agc setting %d defaults to %g, got %g", i, v_agc_defaults[i],
              cfg.agc_cfg[i]);
#endif
    /* a location's height clamps to what the protocol carries */
    static const char *const heights[] = {"-5", "0", "999", "1000", "12.5", "0.5", "1.5"};
    static const int want_h[] = {0, 0, 999, 999, 12, 0, 2};
    for (int i = 0; i < 7; i++) {
        snprintf(buf, sizeof buf, "{\"device\":{\"sample_rate\":288000},\"location\":{\"lat\":1,\"lon\":2,\"height\":%s}, %s}",
                 heights[i], one);
        CHECK(parse(buf, &cfg, err, sizeof err) && cfg.has_location && cfg.location.height_m == want_h[i],
              "height %s -> %d", heights[i], cfg.location.height_m);
    }
    /* names that are longer than their fields are cut, not overrun */
    snprintf(buf, sizeof buf, "{\"name\":\"%.60s\",\"device\":{\"sample_rate\":288000},\"channels\":[{\"id\":\"%.40s\",\"freq\":461.0375}]}",
             "0123456789012345678901234567890123456789012345678901234567890123", "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz");
    CHECK(parse(buf, &cfg, err, sizeof err) && strlen(cfg.name) == sizeof cfg.name - 1 &&
              strlen(cfg.ch[0].id) == sizeof cfg.ch[0].id - 1,
          "long names: %zu / %zu", strlen(cfg.name), strlen(cfg.ch[0].id));

    /* receiver_ids in a box: what str.format would take and the board doesn't, and the escapes it does */
    const char *box_head = "{\"cc\":1,\"channels\":{\"4\":461.0875,\"6\":461.1375},\"radios\":[{\"type\":\"sdr\",\"name\":\"s\",";
    static const struct {
        const char *fmt, *want_id, *err;
    } ids[] = {
        {"R{ch}", "R4", NULL},
        {"{{x}}{ch}", "{x}4", NULL},
        {"{ch:d}", "4", NULL},
        {"{ch:03d}", "004", NULL},
        {"{ch:3d}", "  4", NULL},
        {"{ch:x}", NULL, "receiver_ids {ch:x}: only {ch} or {ch:02d}"},
        {"{ch:10d}", NULL, "receiver_ids {ch:10d}: only {ch} or {ch:02d}"},
        {"{channel}", NULL, "receiver_ids {channel}: only {ch} or {ch:02d}"},
        {"R{ch", NULL, "receiver_ids R{ch: only {ch} or {ch:02d}"},
    };
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        snprintf(buf, sizeof buf, "%s\"receiver_ids\":\"%s\",\"channels\":[4,6]}]}", box_head, ids[i].fmt);
        bool ok = parse(buf, &cfg, err, sizeof err);
        if (ids[i].want_id)
            CHECK(ok && !strcmp(cfg.ch[0].id, ids[i].want_id), "receiver_ids %s -> '%s': %s", ids[i].fmt, cfg.ch[0].id, err);
        else
            CHECK(!ok && !strcmp(err, ids[i].err), "receiver_ids %s: %s", ids[i].fmt, err);
    }
    snprintf(buf, sizeof buf, "%s\"receiver_ids\":\"%.30s{ch}\",\"channels\":[4]}]}", box_head,
             "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    CHECK(parse(buf, &cfg, err, sizeof err) && strlen(cfg.ch[0].id) == 23, "a long receiver id is cut to the field: %zu", strlen(cfg.ch[0].id));
    snprintf(buf, sizeof buf, "%s\"receiver_ids\":\"%.22s{ch:02d}\",\"channels\":[4]}]}", box_head,
             "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    CHECK(parse(buf, &cfg, err, sizeof err) && strlen(cfg.ch[0].id) == 23 && !strncmp(cfg.ch[0].id + 22, "0", 1),
          "a field that runs past the end of the id: '%s'", cfg.ch[0].id);
    /* a value of the wrong kind where a number goes is the default (the reference refuses some of these) */
    snprintf(buf, sizeof buf, "{\"device\":{\"sample_rate\":288000,\"ppm\":[1],\"gain\":{}}, %s}", one);
    CHECK(parse(buf, &cfg, err, sizeof err) && cfg.ppm == 0.0 && !cfg.has_gain, "arrays and objects aren't numbers: %s", err);
}

/* describe into a buffer too small: cut, ended, and the length it says is what is there */
static void test_describe_edges(void)
{
    static rdof_sdr_config_t cfg;
    char err[100];
    const char *json = "{\"name\":\"n\",\"device\":{\"sample_rate\":288000,\"gain\":30},\"location\":{\"lat\":1,\"lon\":2},"
                       "\"channels\":[{\"id\":\"A\",\"freq\":461.0375}]}";
    CHECK(rdof_sdr_config_parse(json, strlen(json), &cfg, err, sizeof err), "%s", err);
    char full[1024], cut[64];
    size_t n = rdof_sdr_config_describe(&cfg, full, sizeof full);
    CHECK(n == strlen(full) && n > 150, "full description %zu", n);
    CHECK(rdof_sdr_config_rssi_offset(&cfg) == -55.0, "rssi offset from the gain: %g", rdof_sdr_config_rssi_offset(&cfg));
    cfg.has_rssi_offset = true;
    cfg.rssi_offset = -70.5;
    CHECK(rdof_sdr_config_rssi_offset(&cfg) == -70.5, "and from the setting");
    cfg.has_gain = false;
    cfg.has_rssi_offset = false;
    CHECK(rdof_sdr_config_rssi_offset(&cfg) == -55.0, "gain auto counts as 30 dB");
    for (size_t cap = 1; cap < sizeof cut; cap += 9) {
        memset(cut, 'x', sizeof cut);
        size_t m = rdof_sdr_config_describe(&cfg, cut, cap);
        CHECK(m == cap - 1 && strlen(cut) == cap - 1 && cut[sizeof cut - 1] == 'x', "cap %zu: %zu characters", cap, m);
    }
    memset(cut, 'x', sizeof cut);
    CHECK(rdof_sdr_config_describe(&cfg, cut, 0) == 0 && cut[0] == 'x', "cap 0: nothing written, nothing said");
}

/* ── status messages ─────────────────────────────────────────────────────── */

static int count_of(const char *hay, const char *needle)
{
    int n = 0;
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += strlen(needle))
        n++;
    return n;
}

static void test_status_edges(void)
{
    for (size_t i = 0; i < V_ROUNDS_N; i++) {
        char out[64];
        rdof_format_round(v_rounds[i].v, v_rounds[i].nd, out, sizeof out);
        CHECK(!strcmp(out, v_rounds[i].want), "round(%.17g, %d) = %s, want %s", v_rounds[i].v, v_rounds[i].nd, out,
              v_rounds[i].want);
    }
    rdof_sdr_channel_t chans[4];
    for (size_t i = 0; i < 4; i++)
        chans[i] = (rdof_sdr_channel_t){v_e2e_chans[i].id, v_e2e_chans[i].freq, v_e2e_chans[i].cc, v_e2e_chans[i].slot};
    const rdof_sdr_rx_config_t cfg = {288e3, 0.0, V_E2E_RSSI_OFFSET, 0.0, true, V_E2E_CHUNK};
    static rdof_sdr_rx_t rx;
    CHECK(rdof_sdr_rx_init(&rx, &cfg, chans, 4, NULL, V_E2E_T0) == 0, "init");
    /* a receiver that has heard nothing: no bursts, no RSSI, no CPU figure, no level */
    static char json[4096];
    rdof_sdr_status_t st = {.name = "e2e-288k", .at = V_E2E_T0 + 0.5, .cpu_load = NAN};
    size_t len = rdof_sdr_status_json(&rx, &st, json, sizeof json);
    CHECK(len > 0 && !strcmp(json, V_STATUS_FRESH_JSON), "fresh status:\n  got  %s\n  want %s", json, V_STATUS_FRESH_JSON);
    /* too small a buffer: nothing, whatever the size, and never a byte past it */
    int fits_at = -1;
    for (size_t cap = 0; cap <= len + 2; cap++) {
        char *tight = malloc(cap ? cap : 1);
        size_t r = rdof_sdr_status_json(&rx, &st, tight, cap);
        if (r) {
            CHECK(r == len && !memcmp(tight, json, len + 1), "cap %zu gives the same text", cap);
            if (fits_at < 0)
                fits_at = (int)cap;
        }
        free(tight);
        if (fits_at >= 0)
            break;
    }
    CHECK(fits_at == (int)len + 1, "fits from %d bytes, the text is %zu", fits_at, len);
    /* channel display names, some missing; the last ones left out; the level keys */
    const char *names[4] = {"Race control", NULL, "", "Tow"};
    st.names = names;
    st.leave_out = 2;
    st.has_level = true;
    st.clip_pct = NAN;
    st.peak_dbfs = NAN;
    len = rdof_sdr_status_json(&rx, &st, json, sizeof json);
    CHECK(len > 0 && count_of(json, "\"id\":") == 2 && strstr(json, "\"name\":\"Race control\"") &&
              strstr(json, "\"name\":\"\",\"freqHz\":461137500.0") && !strstr(json, "\"SDR08\"") &&
              strstr(json, "\"clipPct\":null,\"peakDbfs\":null") && !strstr(json, "gainDb") && !strstr(json, "\"agc\"") &&
              !strstr(json, "hblink"),
          "names, leave_out, level: %s", json);
    /* a channel name too long for the 512 bytes one channel's entry gets: no message at all */
    static char longname[700];
    memset(longname, 'n', sizeof longname - 1);
    const char *long_names[4] = {longname, NULL, NULL, NULL};
    st.names = long_names;
    CHECK(rdof_sdr_status_json(&rx, &st, json, sizeof json) == 0, "a 700 character channel name");
    st.names = names;
    st.leave_out = 9; /* more than there are: none left out of range, all listed as 4 - 9 < 0 */
    len = rdof_sdr_status_json(&rx, &st, json, sizeof json);
    CHECK(len > 0 && count_of(json, "\"id\":") == 0, "all left out: %s", json);
    st.leave_out = 0;
    st.has_gain = true;
    st.gain_db = 28.04;
    st.has_agc = true;
    st.agc_ceiling_db = 46.2;
    st.agc_min_db = 10.0;
    st.agc_changes = 3;
    st.clip_pct = 0.0004;
    st.peak_dbfs = -12.34;
    static const char *const hid[] = {"SDR04"};
    static const bool hup[] = {false};
    st.n_hblink = 1;
    st.hblink_ids = hid;
    st.hblink_up = hup;
    len = rdof_sdr_status_json(&rx, &st, json, sizeof json);
    CHECK(len > 0 && strstr(json, "\"clipPct\":0.0,\"peakDbfs\":-12.3,\"gainDb\":28.0,\"agc\":{\"ceilingDb\":46.2,\"minDb\":10.0,\"changes\":3}") &&
              strstr(json, "\"hblink\":{\"SDR04\":false}"),
          "level, gain, agc, hblink: %s", json);
    /* the channel list or the closing of the message not fitting */
    int zero = 0;
    for (size_t cap = len - 120; cap < len; cap += 7)
        zero += rdof_sdr_status_json(&rx, &st, json, cap) == 0;
    CHECK(zero >= 15, "%d of the short buffers refused", zero);
    rdof_sdr_rx_free(&rx);

    /* the Receiver message: a color code and talkgroup that mean 'any', with and without a location */
    char rj[400];
    CHECK(rdof_sdr_receiver_json("X", 461087500.0, -1, "any", -1, NULL, rj, sizeof rj) > 0 &&
              !strcmp(rj, V_RECEIVER_ANY_JSON),
          "receiver message:\n  got  %s\n  want %s", rj, V_RECEIVER_ANY_JSON);
    rdof_location_t loc = {31.793, -74.034, 12};
    size_t rl = rdof_sdr_receiver_json("X", 461087500.0, 0, "any", 0, &loc, rj, sizeof rj);
    CHECK(rl > 0 && strstr(rj, "\"colorCode\":0") && strstr(rj, "\"tg\":0,\"latitude\":31.793,\"longitude\":-74.034,\"height\":12"),
          "cc 0 and tg 0 are values, not 'any': %s", rj);
    for (size_t cap = 0; cap < rl; cap += 5) {
        char *tight = malloc(cap ? cap : 1);
        CHECK(rdof_sdr_receiver_json("X", 461087500.0, 0, "any", 0, &loc, tight, cap) == 0, "cap %zu refused", cap);
        free(tight);
    }
}

/* ── the AGC against a plain reference model ─────────────────────────────── */

/* An empty chunk measures nothing (0.0, 0.0). */
static void test_agc_empty(void)
{
    double peak = 9, clipped = 9;
    uint8_t none[2] = {255, 255};
    rdof_cf_t one = {1, 1};
    rdof_agc_measure(&one, 0, &peak, &clipped);
    CHECK(peak == 0.0 && clipped == 0.0, "empty chunk: %g %g", peak, clipped);
    peak = clipped = 9;
    rdof_agc_measure_cu8(none, 0, &peak, &clipped);
    CHECK(peak == 0.0 && clipped == 0.0, "empty cu8 chunk: %g %g", peak, clipped);
#if RDOF_FEATURE_SDR_AGC
    /* more steps than it keeps (64): the lowest ones, sorted */
    double many[80];
    for (int i = 0; i < 80; i++)
        many[i] = 0.5 * i;
    rdof_agc_t a;
    rdof_agc_config_t c;
    rdof_agc_config_default(&c);
    rdof_agc_init(&a, &c, many, 80, 100.0, NAN);
    CHECK(a.nsteps == RDOF_AGC_MAX_STEPS && a.max == 0.5 * 63 && a.gain == a.max, "80 steps: %d kept, top %g", a.nsteps, a.max);
    /* nothing the device can do within [min, max]: the one step is max */
    c.min = 50.0;
    rdof_agc_init(&a, &c, many, 80, 10.0, NAN);
    CHECK(a.nsteps == 1 && a.steps[0] == 10.0 && a.gain == 10.0, "no step in range: %d, %g", a.nsteps, a.gain);
    /* a start gain between steps is the one below, below the lowest the lowest */
    c.min = 0.0;
    rdof_agc_init(&a, &c, v_agc_steps, (int)V_AGC_STEPS_N, 46.2, 14.0);
    CHECK(a.gain == 12.6, "start 14.0 -> %g", a.gain);
    rdof_agc_init(&a, &c, v_agc_steps, (int)V_AGC_STEPS_N, 46.2, -3.0);
    CHECK(a.gain == 0.0, "start -3.0 -> %g", a.gain);
#endif
}

#if RDOF_FEATURE_SDR_AGC

/* agc.Agc line for line, with its unbounded list of peaks. */
typedef struct {
    rdof_agc_config_t cfg;
    double steps[64];
    int n;
    double max, gain, ceiling, settle_until, relaxed, quiet_since;
    bool quiet;
    int changes;
    double *pt, *pv;
    int np;
} ref_agc_t;

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static double ref_at_or_below(const ref_agc_t *a, double g)
{
    double r = a->steps[0];
    for (int i = 0; i < a->n; i++)
        if (a->steps[i] <= g + 1e-6)
            r = a->steps[i];
    return r;
}

static void ref_init(ref_agc_t *a, const rdof_agc_config_t *c, const double *steps, int n, double max_gain, double gain)
{
    memset(a, 0, sizeof *a);
    a->cfg = *c;
    for (int i = 0; i < n; i++)
        if (c->min - 1e-6 <= steps[i] && steps[i] <= max_gain + 1e-6)
            a->steps[a->n++] = steps[i];
    qsort(a->steps, (size_t)a->n, sizeof(double), cmp_d);
    if (!a->n)
        a->steps[a->n++] = max_gain;
    a->max = a->steps[a->n - 1];
    a->gain = ref_at_or_below(a, isnan(gain) ? a->max : gain);
    a->ceiling = a->max;
    a->settle_until = -1.0;
    a->relaxed = -INFINITY;
    a->pt = malloc(sizeof(double) * 200000);
    a->pv = malloc(sizeof(double) * 200000);
}

static bool ref_set(ref_agc_t *a, double g, double t, double *out)
{
    if (fabs(g - a->gain) < 1e-6)
        return false;
    a->gain = g;
    a->changes++;
    a->settle_until = t + a->cfg.settle_s;
    a->np = 0;
    *out = g;
    return true;
}

static bool ref_update(ref_agc_t *a, double peak, double clipped, double t, double *out)
{
    if (t < a->settle_until)
        return false;
    if (clipped > a->cfg.clip) {
        a->relaxed = t;
        a->quiet = false;
        a->np = 0;
        double g = a->steps[0];
        for (int i = 0; i < a->n; i++)
            if (a->steps[i] < a->gain - 1e-6)
                g = a->steps[i];
        a->ceiling = g;
        return ref_set(a, g, t, out);
    }
    a->pt[a->np] = t;
    a->pv[a->np++] = peak;
    int drop = 0;
    while (drop < a->np && a->pt[drop] < t - a->cfg.hold_s)
        drop++;
    if (drop) {
        memmove(a->pt, a->pt + drop, sizeof(double) * (size_t)(a->np - drop));
        memmove(a->pv, a->pv + drop, sizeof(double) * (size_t)(a->np - drop));
        a->np -= drop;
    }
    if (!a->quiet) {
        a->quiet = true;
        a->quiet_since = t;
    }
    if (a->ceiling < a->max && t - a->relaxed >= a->cfg.relax_s) {
        double up = a->max;
        for (int i = 0; i < a->n; i++)
            if (a->steps[i] > a->ceiling + 1e-6) {
                up = a->steps[i];
                break;
            }
        a->ceiling = up;
        a->relaxed = t;
    }
    double up = 0;
    bool have = false;
    for (int i = 0; i < a->n; i++)
        if (a->steps[i] > a->gain + 1e-6) {
            up = a->steps[i];
            have = true;
            break;
        }
    if (!have || up > a->ceiling + 1e-6 || t - a->quiet_since < a->cfg.hold_s)
        return false;
    double wp = 0;
    for (int i = 0; i < a->np; i++)
        wp = fmax(wp, a->pv[i]);
    if (wp * pow(10.0, (up - a->gain + a->cfg.headroom_db) / 20.0) >= 0.99)
        return false;
    a->quiet_since = t;
    return ref_set(a, up, t, out);
}

static uint32_t xs = 2463534242u;
static uint32_t xr(void)
{
    xs ^= xs << 13;
    xs ^= xs >> 17;
    xs ^= xs << 5;
    return xs;
}
static double ur(void) { return (xr() >> 8) / 16777216.0; }

/* One random run: the same measurements into both. falling: peaks that only ever fall, so the in-window list
 * grows past the C queue's 256. Returns the first index the two disagree at (-1: never), and whether, there,
 * the reference raised the gain and the C kept it. */
static int agc_run(uint32_t seed, bool falling, bool shuffle, int *conservative, int *events)
{
    xs = seed | 1;
    rdof_agc_config_t c;
    rdof_agc_config_default(&c);
    c.min = ur() < 0.3 ? 5.0 * (xr() % 4) : 0.0;
    c.hold_s = 1.0 + 9.0 * ur();
    c.relax_s = 5.0 + 60.0 * ur();
    c.settle_s = 0.05 + 0.5 * ur();
    c.headroom_db = 3.0 + 6.0 * ur();
    c.clip = ur() < 0.5 ? 1e-4 : 0.01;
    enum { NS = (int)V_AGC_STEPS_N };
    double steps[NS];
    memcpy(steps, v_agc_steps, sizeof steps);
    if (shuffle)
        for (int i = NS - 1; i > 0; i--) {
            int j = (int)(xr() % (unsigned)(i + 1));
            double t = steps[i];
            steps[i] = steps[j];
            steps[j] = t;
        }
    const double max_gain = v_agc_steps[10 + xr() % (NS - 10)];
    const double start = ur() < 0.3 ? NAN : v_agc_steps[xr() % NS];
    rdof_agc_t a;
    ref_agc_t r;
    rdof_agc_init(&a, &c, steps, NS, max_gain, start);
    ref_init(&r, &c, steps, NS, max_gain, start);
    int first = -1;
    *conservative = 0;
    *events = 0;
    double t = 0, peak = 0.3;
    const double dt = falling ? c.hold_s / (300.0 + 100.0 * ur()) : c.hold_s / (20.0 + 130.0 * ur());
    for (int i = 0; i < 2500; i++) {
        t += dt;
        double clipped = 0;
        if (falling) {
            peak = fmax(0.001, 0.95 * (1.0 - i / 1500.0));
            if (i == 3)
                clipped = 0.5;
        } else {
            peak = fmin(1.0, fmax(0.001, peak * (0.9 + 0.2 * ur())));
            if (ur() < 0.01)
                clipped = ur() < 0.5 ? 0.5 : 5e-5;
        }
        double ga = 0, gr = 0;
        bool da = rdof_agc_update(&a, peak, clipped, t, &ga), dr = ref_update(&r, peak, clipped, t, &gr);
        *events += da;
        if (first < 0 && (da != dr || (da && ga != gr))) {
            first = i;
            *conservative = dr && !da;
        }
        if (first < 0 && (a.gain != r.gain || a.ceiling != r.ceiling || a.changes != r.changes))
            first = i;
        if (first >= 0)
            break;
    }
    free(r.pt);
    free(r.pv);
    return first;
}

static void test_agc_reference(void)
{
    rdof_agc_config_t c;
    rdof_agc_config_default(&c);
    int bad = 0, total_events = 0;
    for (uint32_t seed = 1; seed <= 300; seed++) {
        int cons, ev;
        int first = agc_run(seed * 2654435761u, false, seed % 2, &cons, &ev);
        bad += first >= 0;
        total_events += ev;
        if (first >= 0 && bad <= 3)
            printf("  agc seed %u: differs from the reference model at update %d\n", seed, first);
    }
    CHECK(bad == 0, "%d of 300 random runs differ from the reference model's decisions", bad);
    CHECK(total_events > 300, "the runs changed the gain %d times between them", total_events);
    /* peaks falling for longer than the C queue holds: it may only be slower to raise the gain than the reference model,
     * never quicker, and never different before that */
    int diverged = 0, wrong_way = 0;
    for (uint32_t seed = 1; seed <= 60; seed++) {
        int cons, ev;
        int first = agc_run(seed * 40503u + 7, true, false, &cons, &ev);
        if (first >= 0) {
            diverged++;
            wrong_way += !cons;
        }
    }
    CHECK(wrong_way == 0, "%d of %d falling-peak runs where the C raised the gain and the reference model did not", wrong_way,
          diverged);
    printf("  agc: 300 random runs agree with the reference model's decisions (%d gain changes); %d of 60 falling-peak runs "
           "diverge, all by waiting longer\n",
           total_events, diverged);
}

#endif

/* A recording with a few hundred bursts: four channels, a data burst in every slot. Returns cu8 and its length
 * in complex samples. */
static uint8_t *busy_clip(double seconds, int *n_out, rdof_sdr_channel_t chans[4])
{
    static const double offs[4] = {37500.0, 87500.0, -62500.0, -87500.0};
    const int n = (int)(seconds * FS);
    rdof_mod_t m[4];
    uint8_t pre[3][RDOF_BURST_BYTES];
    rdof_csbk_preambles(9990003, 91, true, 3, 1, pre);
    for (int c = 0; c < 4; c++) {
        CHECK(rdof_mod_init(&m[c], FS, offs[c], 4.0, 1.0f, 8, 0) == 0, "mod init");
        chans[c] = (rdof_sdr_channel_t){c == 0 ? "A" : c == 1 ? "B" : c == 2 ? "C" : "D", CENTER + offs[c], 1, 1};
    }
    const int64_t slot = rdof_mod_slot_samples(&m[0]), lead = rdof_mod_samples(&m[0], 24);
    uint8_t *cu8 = malloc(2 * (size_t)n);
    rdof_cf_t *buf = malloc(sizeof(rdof_cf_t) * CHUNK), *sum = malloc(sizeof(rdof_cf_t) * CHUNK);
    int64_t next[4] = {lead + slot, lead + slot, lead + slot, lead + slot};
    int k[4] = {0, 1, 2, 0};
    rng_state = 4242;
    for (int pos = 0; pos < n; pos += CHUNK) {
        const int len = n - pos < CHUNK ? n - pos : CHUNK;
        memset(sum, 0, sizeof(rdof_cf_t) * CHUNK);
        for (int c = 0; c < 4; c++) {
            while (next[c] < pos + 3 * slot && next[c] < n) {
                CHECK(rdof_mod_queue_burst(&m[c], next[c], pre[k[c]++ % 3], NULL, RDOF_MOD_RAMPS) == 0, "queue");
                next[c] += slot;
            }
            rdof_mod_iq(&m[c], buf, len);
            for (int i = 0; i < len; i++) {
                sum[i].re += 0.2f * buf[i].re;
                sum[i].im += 0.2f * buf[i].im;
            }
        }
        for (int i = 0; i < len; i++) {
            const float re = sum[i].re + 0.01f * noise(), im = sum[i].im + 0.01f * noise();
            cu8[2 * (pos + i)] = (uint8_t)fminf(255.0f, fmaxf(0.0f, nearbyintf(127.5f + 127.5f * re)));
            cu8[2 * (pos + i) + 1] = (uint8_t)fminf(255.0f, fmaxf(0.0f, nearbyintf(127.5f + 127.5f * im)));
        }
    }
    for (int c = 0; c < 4; c++)
        rdof_mod_free(&m[c]);
    free(buf);
    free(sum);
    *n_out = n;
    return cu8;
}


static void test_bench_busy(void)
{
    rdof_sdr_channel_t chans[4];
    int n;
    uint8_t *cu8 = busy_clip(0.8, &n, chans);
    rdof_sdr_rx_config_t cfg = {FS, CENTER, -60.0, 0.0, true, CHUNK};

    static rdof_sdr_rx_t rx;
    struct count_ctx cc = {0};
    CHECK(rdof_sdr_rx_init(&rx, &cfg, chans, 4, NULL, 0) == 0, "rx init");
    rx.on_burst = count_burst;
    rx.burst_ctx = &cc;
    rx.gate = false;
    rdof_cf_t *x = malloc(sizeof(rdof_cf_t) * (size_t)n);
    rdof_iq_from_cu8(cu8, (size_t)n, x);
    for (int i = 0; i < n; i += CHUNK)
        rdof_sdr_rx_process(&rx, x + i, n - i < CHUNK ? n - i : CHUNK);
    free(x);
    rdof_sdr_rx_free(&rx);
    CHECK(cc.bursts > 80, "the receiver finds %ld bursts", cc.bursts);

    /* the bench keeps the bursts for its tracker stage, in a buffer that doubles: past 66 it has grown twice.
     * Recording what it allocates tells which calls those are: three at once, in the ratio of a burst, a double
     * and an int. */
    static size_t sizes[2000];
    static long nsz;
    nsz = 0;
    fake_now_t = 0;
    rdof_sdr_bench_t b;
    fa_arm(0);
    rdof_sdr_set_allocator(rec_alloc, fa_free);
    sizes_rec = sizes;
    sizes_n = &nsz;
    CHECK(rdof_sdr_bench(cu8, n, &cfg, chans, 4, fake_now, 1, &b) == 0, "bench");
    rdof_sdr_set_allocator(NULL, NULL);
    CHECK(b.bursts == cc.bursts, "the bench counts %ld bursts, the receiver %ld", b.bursts, cc.bursts);
    long growth[2][3];
    int ng = 0;
    for (long i = 0; i + 2 < nsz && ng < 2; i++) {
        const size_t c3 = sizes[i + 2];
        if (c3 % 4 == 0 && sizes[i + 1] == 2 * c3 && sizes[i] == sizeof(rdof_sdr_burst_t) * (c3 / 4) && c3 / 4 >= 64) {
            growth[ng][0] = i + 1;
            growth[ng][1] = i + 2;
            growth[ng][2] = i + 3;
            ng++;
        }
    }
    CHECK(ng == 2, "found %d growths of the burst buffer in %ld allocations", ng, nsz);
    /* out of memory at each of the second growth's three allocations: reported, and nothing left allocated (the
     * first growth's failures are the same code) */
    for (int w = 0; ng == 2 && w < 3; w++) {
        fa_arm(growth[1][w]);
        int r = rdof_sdr_bench(cu8, n, &cfg, chans, 4, fake_now, 1, &b);
        long live = a_live, fired = a_fired;
        fa_disarm();
        CHECK(fired == 1 && r == -1 && live == 0, "growth allocation %d failing: %d, %ld left", w, r, live);
    }
    free(cu8);
}

/* ── voice superframes on both slots ─────────────────────────────────────── */

typedef struct {
    int slot, dt, n;
    int64_t pos;
} vb_t;

typedef struct {
    vb_t b[64];
    int count;
    int busy;
} voice_out_t;

static void voice_burst(void *ctx, int c, const rdof_sdr_burst_t *b, double t)
{
    voice_out_t *o = ctx;
    if (o->count < 64)
        o->b[o->count++] = (vb_t){b->b.slot, b->b.dt, b->b.n, b->position};
}

/* Two voice calls on one carrier, the slots alternating every 30 ms, as a MS or direct-mode radio's would be
 * heard on a channel: a superframe is A (voice sync) and B-F (EMB only). first: the slot of the first burst, which
 * the demod takes for its default; which slot starts first decides whose track the flywheel serves first.
 * lose: bursts k >= lose are not sent (a call that ends in the air). */
static void run_voice(int first_slot, int lose, int cc_sent, voice_out_t *o)
{
    memset(o, 0, sizeof *o);
    static rdof_sdr_rx_t rx;
    const rdof_sdr_rx_config_t cfg = {FS, CENTER, -60.0, 0, true, CHUNK};
    const rdof_sdr_channel_t ch = {"A", scene_chans[0].freq_hz, 1, first_slot};
    CHECK(rdof_sdr_rx_init(&rx, &cfg, &ch, 1, NULL, 0) == 0, "voice rx init");
    rx.on_burst = voice_burst;
    rx.burst_ctx = o;
    rdof_mod_t m;
    CHECK(rdof_mod_init(&m, FS, ch.freq_hz - CENTER, 4.0, 1.0f, 8, 0) == 0, "mod init");
    const int64_t slot = rdof_mod_slot_samples(&m), t0 = FS / 4;
    uint8_t bursts[12][RDOF_BURST_BYTES];
    for (int k = 0; k < 12; k++) /* burst k belongs to the superframe of slot k % 2; its position is k / 2 */
        rdof_silence_burst(k / 2, cc_sent, false, NULL, bursts[k]);
    rdof_cf_t *iq = malloc(sizeof(rdof_cf_t) * CHUNK), *sig = malloc(sizeof(rdof_cf_t) * CHUNK);
    int queued = 0;
    rng_state = 99;
    for (int c = 0; c < 70; c++) {
        const int64_t pos = (int64_t)c * CHUNK;
        while (queued < 12 && queued < lose && t0 + queued * slot < pos + 3 * slot) {
            CHECK(rdof_mod_queue_burst(&m, t0 + queued * slot, bursts[queued], NULL, RDOF_MOD_RAMPS) == 0, "queue %d", queued);
            queued++;
        }
        rdof_mod_iq(&m, sig, CHUNK);
        for (int i = 0; i < CHUNK; i++) {
            iq[i].re = 0.5f * sig[i].re + 0.01f * noise();
            iq[i].im = 0.5f * sig[i].im + 0.01f * noise();
        }
        rdof_sdr_rx_process(&rx, iq, CHUNK);
        o->busy += rdof_demod_busy(&rx.demods[0]);
    }
    CHECK(!rdof_demod_busy(&rx.demods[0]) || lose < 12, "idle once the calls are over");
    free(iq);
    free(sig);
    rdof_mod_free(&m);
    rdof_sdr_rx_free(&rx);
}

/* One slot's bursts in the order they came: A to F, 60 ms apart, from pos0 (-1: don't check where). */
static void check_slot(const voice_out_t *o, int slot, int want, int64_t pos0, const char *what)
{
    int k = 0;
    int64_t first = 0;
    for (int i = 0; i < o->count; i++) {
        const vb_t *b = &o->b[i];
        if (b->slot != slot)
            continue;
        CHECK(b->dt == (k == 0 ? RDOF_DT_VOICE_SYNC : RDOF_DT_VOICE) && b->n == k, "%s slot %d burst %d: dt %02x n %d", what,
              slot, k, b->dt, b->n);
        if (k == 0)
            first = b->pos;
        else
            CHECK(llabs(b->pos - first - 2880 * k) <= 3, "%s slot %d burst %d at %lld after A, want %d", what, slot, k,
                  (long long)(b->pos - first), 2880 * k);
        k++;
    }
    CHECK(k == want, "%s slot %d: %d bursts, want %d", what, slot, k, want);
    if (pos0 >= 0 && k)
        CHECK(llabs(first - pos0) <= 3, "%s slot %d starts at %lld, want %lld", what, slot, (long long)first, (long long)pos0);
}

static void test_voice(void)
{
    static voice_out_t o, o1, cut, other;
    /* two full calls, the second slot's first: A to F on each, the slots 30 ms apart; the two flywheels run at
     * once (the older one served first, whichever slot it is) */
    run_voice(2, 12, 1, &o);
    CHECK(o.count == 12, "%d bursts of 12", o.count);
    check_slot(&o, 2, 6, -1, "second slot first");
    check_slot(&o, 1, 6, -1, "second slot first");
    int64_t a2 = 0, a1 = 0;
    for (int i = 0; i < o.count; i++)
        if (o.b[i].dt == RDOF_DT_VOICE_SYNC) {
            if (o.b[i].slot == 2)
                a2 = o.b[i].pos;
            else
                a1 = o.b[i].pos;
        }
    CHECK(llabs(a1 - a2 - 1440) <= 3, "the second call's A is 30 ms (1440 samples) after the first's: %lld",
          (long long)(a1 - a2));
    CHECK(o.busy > 15, "busy for %d chunks", o.busy);
    /* the same with slot 1 first */
    run_voice(1, 12, 1, &o1);
    CHECK(o1.count == 12 && o1.b[0].slot == 1, "default slot 1: %d bursts, first on slot %d", o1.count, o1.b[0].slot);
    check_slot(&o1, 1, 6, -1, "first slot first");
    check_slot(&o1, 2, 6, -1, "first slot first");
    /* the calls end in the air: the first slot's after B, the second slot's after A. The flywheel misses twice and
     * gives up; what was heard came out, and nothing is left busy */
    run_voice(2, 4, 1, &cut);
    CHECK(cut.count >= 3 && cut.count <= 4, "%d bursts of a call cut off after 4", cut.count);
    /* another color code than the demod's: the A is held, B never confirms it, nothing comes out */
    run_voice(2, 12, 7, &other);
    CHECK(other.count == 0, "a call on color code 7 gives %d bursts to a demod on 1", other.count);
}

/* ── bursts built symbol by symbol, straight into the demod at 48 kS/s ───── */

/* A CSBK's symbols for color code cc, ready to bend. */
static void csbk_symbols(int cc, int8_t sym[132])
{
    uint8_t b[1][RDOF_BURST_BYTES];
    rdof_csbk_preambles(9990003, 91, true, 1, cc, b);
    rdof_mod_symbols(b[0], 264, sym);
}

/* The 48 kS/s signal of one burst (symbols 0..131) at the 5000th sample, its frequency scaled by dev_scale; zeros
 * either side with a little noise. */
static rdof_cf_t *burst_iq(const int8_t sym[132], double dev_scale, int n, float noise_amp)
{
    rdof_mod_t m;
    CHECK(rdof_mod_init(&m, 48000, 0.0, 4.0, 1.0f, 2, 0) == 0, "mod init");
    CHECK(rdof_mod_queue(&m, 5000, sym, 0, 132, RDOF_MOD_RAMPS) == 0, "queue");
    float *hz = malloc(sizeof(float) * (size_t)n), *env = malloc(sizeof(float) * (size_t)n);
    rdof_mod_freq(&m, hz, env, n);
    rdof_cf_t *x = malloc(sizeof(rdof_cf_t) * (size_t)n);
    double ph = 0;
    for (int i = 0; i < n; i++) {
        ph += 2 * M_PI * hz[i] * dev_scale / 48000.0;
        x[i].re = 0.5f * env[i] * (float)cos(ph) + noise_amp * noise();
        x[i].im = 0.5f * env[i] * (float)sin(ph) + noise_amp * noise();
    }
    free(hz);
    free(env);
    rdof_mod_free(&m);
    return x;
}

/* Bursts (not counting idle ones) the demod finds in x. */
static int bursts_sq(const rdof_cf_t *x, int n, int cc, double squelch_hz, rdof_sdr_burst_t *first)
{
    rdof_demod_t d;
    CHECK(rdof_demod_init(&d, cc, 1, squelch_hz, 2400) == 0, "init");
    int count = 0;
    for (int i = 0; i < n; i += 2400) {
        int nb;
        const rdof_sdr_burst_t *bs = rdof_demod_feed(&d, x + i, n - i < 2400 ? n - i : 2400, &nb);
        for (int j = 0; j < nb; j++)
            if (bs[j].b.dt != RDOF_DT_IDLE) {
                if (first && !count)
                    *first = bs[j];
                count++;
            }
    }
    rdof_demod_free(&d);
    return count;
}

static int bursts_in(const rdof_cf_t *x, int n, int cc, rdof_sdr_burst_t *first)
{
    return bursts_sq(x, n, cc, RDOF_DEMOD_SQUELCH_HZ, first);
}

/* The variance of the 24 values at 10-sample steps that the sync search looks at, the largest over a burst's
 * sync positions, of x run through the FM stage. */
static double sync_var_max(const rdof_cf_t *x, int n)
{
    rdof_fm_t fm;
    rdof_fm_init(&fm, 48000.0);
    float *f = malloc(sizeof(float) * (size_t)n), *p = malloc(sizeof(float) * (size_t)n);
    rdof_fm_process(&fm, x, n, f, p);
    rdof_fm_free(&fm);
    double best = 0;
    for (int r = 5000 + 400; r < 5000 + 900; r += 2) { /* around the sync (symbol 54 at +540, FM delay 40) */
        double s1 = 0, s2 = 0;
        for (int i = 0; i < 24; i++) {
            s1 += f[r + 10 * i];
            s2 += (double)f[r + 10 * i] * f[r + 10 * i];
        }
        best = fmax(best, s2 - s1 * s1 / 24.0);
    }
    free(f);
    free(p);
    return best;
}

static void test_demod_bent(void)
{
    enum { N = 12000 };
    int8_t sym[132];
    csbk_symbols(1, sym);
    rdof_sdr_burst_t b;
    rdof_cf_t *x = burst_iq(sym, 1.0, N, 0.005f);
    CHECK(bursts_in(x, N, 1, &b) == 1 && b.b.dt == RDOF_DT_CSBK && b.b.cc == 1, "a clean CSBK");
    const int64_t clean_pos = b.position;
    free(x);

    /* a deviation past what a sync can be measured at: under 300 Hz or over 5 kHz between the levels */
    const double weak = 0.12, strong = 3.2;
    x = burst_iq(sym, 0.2, N, 0.005f);
    CHECK(bursts_in(x, N, 1, &b) == 1 && fabs(b.deviation_hz - 0.2 * 1944) < 60, "a fifth of the deviation: levels %.0f", b.deviation_hz);
    free(x);
    x = burst_iq(sym, weak, N, 0.002f);
    CHECK(bursts_in(x, N, 1, &b) == 0, "an eighth of the deviation (233 Hz) is refused");
    free(x);
    x = burst_iq(sym, strong, N, 0.002f);
    CHECK(bursts_sq(x, N, 1, 0.0, &b) == 0, "3.2 times the deviation (6.2 kHz) is refused");
    free(x);
    /* the squelch, which wants the frequency to stay within about 2 kHz of the middle, closes before that */
    x = burst_iq(sym, 2.5, N, 0.002f);
    CHECK(bursts_in(x, N, 1, &b) == 0, "2.5 times (4.9 kHz) is shut out by the squelch");
    CHECK(bursts_sq(x, N, 1, 0.0, &b) == 1 && llabs(b.position - clean_pos) <= 3,
          "and taken without one, at the same place (%lld, %lld)", (long long)b.position, (long long)clean_pos);
    free(x);

    /* the same burst with a color code the demod isn't on, and with it set to any */
    int8_t sym3[132];
    csbk_symbols(3, sym3);
    x = burst_iq(sym3, 1.0, N, 0.005f);
    CHECK(bursts_in(x, N, 1, &b) == 0, "another color code, refused");
    CHECK(bursts_in(x, N, 3, &b) == 1 && b.b.cc == 3 && bursts_in(x, N, -1, &b) == 1 && b.b.cc == 3,
          "taken by a demod on that color code, and by one on any");
    free(x);

    /* sync symbols weakened (+-3 to +-1): 5 of them put 5 bits wrong, past the 4 allowed; 4 are tolerated */
    int8_t soft[132];
    memcpy(soft, sym, sizeof soft);
    for (int i = 0; i < 4; i++)
        soft[54 + 3 * i] = (int8_t)(soft[54 + 3 * i] > 0 ? 1 : -1);
    x = burst_iq(soft, 1.0, N, 0.002f);
    CHECK(bursts_in(x, N, 1, &b) == 1 && b.b.sync_errors <= 4, "4 weak sync symbols: found with %d errors", b.b.sync_errors);
    free(x);
    soft[54 + 12] = (int8_t)(soft[54 + 12] > 0 ? 1 : -1);
    x = burst_iq(soft, 1.0, N, 0.002f);
    CHECK(bursts_in(x, N, 1, &b) == 0, "5 weak sync symbols: refused");
    free(x);

    /* a slot type no Golay code can repair: a burst of the right color code isn't taken for another's */
    int8_t broken[132];
    memcpy(broken, sym, sizeof broken);
    for (int i = 49; i < 54; i++)
        broken[i] = (int8_t)-broken[i];
    x = burst_iq(broken, 1.0, N, 0.002f);
    CHECK(bursts_in(x, N, 1, &b) == 0, "a damaged slot type is refused");
    free(x);

    /* another copy of the sync pattern inside the payload: one burst, and the spurious sync is skipped over */
    int8_t twice[132];
    memcpy(twice, sym, sizeof twice);
    memcpy(twice + 90, sym + 54, 24);
    x = burst_iq(twice, 1.0, N, 0.002f);
    CHECK(bursts_in(x, N, 1, &b) == 1 && b.position == clean_pos, "a sync pattern inside a burst");
    free(x);

    /* a deviation so small that a sync-shaped window just passes the prefilter and just fails the scoring:
     * found by scaling until the window's variance is within 0.4% under the floor (24 x 100 Hz^2) */
    double lo = 0.02, hi = 0.2;
    const double floor_var = 24 * 100.0 * 100.0;
    for (int it = 0; it < 40; it++) {
        const double mid = 0.5 * (lo + hi);
        x = burst_iq(sym, mid, N, 0.0f);
        const double v = sync_var_max(x, N);
        free(x);
        if (v < 0.9975 * floor_var)
            lo = mid;
        else
            hi = mid;
    }
    x = burst_iq(sym, 0.5 * (lo + hi), N, 0.0f);
    const double v = sync_var_max(x, N);
    memset(&rdof_demod_prof, 0, sizeof rdof_demod_prof);
    dclk = 0;
    rdof_demod_set_clock(dtick);
    CHECK(bursts_in(x, N, 1, &b) == 0 && rdof_demod_prof.scored > 0, "a window just under the variance floor (%.4f of it): %llu scored",
          v / floor_var, (unsigned long long)rdof_demod_prof.scored);
    rdof_demod_set_clock(NULL);
    free(x);
}

/* A feed size so small that the history a voice call needs (two slots and a burst: about 4200 samples) doesn't fit
 * the buffer the demod sizes from it: it drops the oldest samples rather than overrun, and the calls it can still
 * follow come out as they do at a large feed size. */
static void test_demod_small_feed(void)
{
    int n;
    rdof_cf_t *x = load_cu8("demod_bs_both_slots.cu8", &n);
    static dem_out_t big, small;
    rdof_demod_t d;
    CHECK(rdof_demod_init(&d, 3, 1, RDOF_DEMOD_SQUELCH_HZ, 2400) == 0, "init");
    run_demod(&d, x, n, 2400, 0, 0, &big);
    rdof_demod_free(&d);
    static const int feeds[] = {100, 200, 331, 700};
    for (size_t k = 0; k < sizeof feeds / sizeof feeds[0]; k++) {
        CHECK(rdof_demod_init(&d, 3, 1, RDOF_DEMOD_SQUELCH_HZ, feeds[k]) == 0, "init");
        run_demod(&d, x, n, feeds[k], 0, 0, &small);
        CHECK(small.n > 10 && d.samples_in == n, "feed %d: %d bursts of %d", feeds[k], small.n, big.n);
        rdof_demod_free(&d);
    }
    free(x);
}

/* A voice sync much weaker than the call its slot is following can't take the slot over (a neighbor's leakage, a
 * mirrored channel's image); one that isn't that much weaker does. The call: A and B only, so its flywheel is
 * still waiting for C when the other sync arrives where C belongs. */
static void test_demod_weaker(void)
{
    enum { N = 20000 };
    static const double weak_amp[2] = {0.012, 0.15}; /* 32 dB and 10 dB under the call's 0.5 */
    int64_t anchor[2], first_anchor[2];
    int busy[2];
    for (int w = 0; w < 2; w++) {
        uint8_t burst[3][RDOF_BURST_BYTES];
        rdof_silence_burst(0, 1, false, NULL, burst[0]);
        rdof_silence_burst(1, 1, false, NULL, burst[1]);
        rdof_silence_burst(0, 1, false, NULL, burst[2]);
        rdof_mod_t strong, weak;
        CHECK(rdof_mod_init(&strong, 48000, 0.0, 4.0, 0.5f, 4, 0) == 0 && rdof_mod_init(&weak, 48000, 0.0, 4.0, (float)weak_amp[w], 4, 0) == 0,
              "mod init");
        const int64_t p0 = 3000;
        CHECK(rdof_mod_queue_burst(&strong, p0, burst[0], NULL, RDOF_MOD_RAMPS) == 0 &&
                  rdof_mod_queue_burst(&strong, p0 + 2880, burst[1], NULL, RDOF_MOD_RAMPS) == 0 &&
                  rdof_mod_queue_burst(&weak, p0 + 5760, burst[2], NULL, RDOF_MOD_RAMPS) == 0,
              "queue");
        rdof_cf_t *a = malloc(sizeof(rdof_cf_t) * N), *b = malloc(sizeof(rdof_cf_t) * N);
        rdof_mod_iq(&strong, a, N);
        rdof_mod_iq(&weak, b, N);
        for (int i = 0; i < N; i++) {
            a[i].re += b[i].re + 0.002f * noise();
            a[i].im += b[i].im + 0.002f * noise();
        }
        free(b);
        rdof_mod_free(&strong);
        rdof_mod_free(&weak);

        rdof_demod_t d;
        CHECK(rdof_demod_init(&d, 1, 2, RDOF_DEMOD_SQUELCH_HZ, 400) == 0, "init");
        first_anchor[w] = -1;
        busy[w] = 0;
        int64_t last_anchor = -1;
        for (int i = 0; i < N; i += 400) {
            int nb;
            rdof_demod_feed(&d, a + i, 400, &nb);
            if (d.tracks[1].active) {
                if (first_anchor[w] < 0)
                    first_anchor[w] = d.tracks[1].anchor;
                last_anchor = d.tracks[1].anchor;
                busy[w]++;
            }
        }
        anchor[w] = last_anchor;
        rdof_demod_free(&d);
        free(a);
    }
    CHECK(first_anchor[0] > 0 && first_anchor[1] == first_anchor[0], "the call's A at %lld / %lld", (long long)first_anchor[0],
          (long long)first_anchor[1]);
    CHECK(anchor[0] == first_anchor[0], "a sync 32 dB down leaves the call's track where it was (%lld, %lld)",
          (long long)anchor[0], (long long)first_anchor[0]);
    CHECK(anchor[1] > first_anchor[1] + 5000, "a sync 10 dB down takes the slot over: the track moves to %lld from %lld",
          (long long)anchor[1], (long long)first_anchor[1]);
}

/* The board's own keys, which the reference doesn't have and the description doesn't print. */
static void test_config_board_keys(void)
{
    static rdof_sdr_config_t cfg;
    char err[200], buf[2048];
    const char *one = "\"channels\":[{\"id\":\"A\",\"freq\":461.0375}]";
    snprintf(buf, sizeof buf, "{\"device\":{\"sample_rate\":288000}, %s}", one);
    CHECK(rdof_sdr_config_parse(buf, strlen(buf), &cfg, err, sizeof err), "%s", err);
    CHECK(cfg.status_seconds == 30.0 && cfg.usb_transfers == 8 && cfg.usb_transfer_kb == 8 && cfg.dsp_cores == 1 &&
              !cfg.spike && !cfg.selftest && !cfg.sim && cfg.idle_gate && cfg.learn_ppm && cfg.sim_ppm == 0.0 &&
              cfg.frontend == RDOF_FE_RTLSDR && !cfg.has_mqtt && !cfg.mqtt_tls && !strcmp(cfg.mqtt_host, "localhost") &&
              cfg.mqtt_port == 1883 && !strcmp(cfg.mqtt_root, "DMR") && !strcmp(cfg.mqtt_app, "sdr") && !cfg.hb_enabled &&
              cfg.hb_port == 62031 && !strcmp(cfg.hb_callsign, "SDR") && cfg.afc && !cfg.has_rssi_offset,
          "defaults");
    snprintf(buf, sizeof buf,
             "{\"device\":{\"sample_rate\":288000},\"status_seconds\":10,\"usb\":{\"transfers\":4,\"transfer_kb\":16},"
             "\"dsp_cores\":2,\"spike\":true,\"selftest\":1,\"sim\":true,\"sim_ppm\":3.5,\"idle_gate\":false,"
             "\"learn_ppm\":0,\"rssi_offset\":-61.5,\"mqtt\":{\"host\":\"h\",\"username\":\"u\",\"password\":\"p\","
             "\"root\":\"//X//\",\"app\":\"a\",\"tls\":true},\"hblink\":{\"enabled\":true,\"host\":\"hb\",\"port\":1,"
             "\"callsign\":\"CALL\",\"peer_id_base\":9990200}, %s}", one);
    CHECK(rdof_sdr_config_parse(buf, strlen(buf), &cfg, err, sizeof err), "%s", err);
    CHECK(cfg.status_seconds == 10.0 && cfg.usb_transfers == 4 && cfg.usb_transfer_kb == 16 && cfg.dsp_cores == 2 &&
              cfg.spike && cfg.selftest && cfg.sim && cfg.frontend == RDOF_FE_SIM && cfg.sim_ppm == 3.5 && !cfg.idle_gate &&
              !cfg.learn_ppm && cfg.has_rssi_offset && cfg.rssi_offset == -61.5 && cfg.has_mqtt &&
              !strcmp(cfg.mqtt_host, "h") && !strcmp(cfg.mqtt_user, "u") && !strcmp(cfg.mqtt_pass, "p") &&
              !strcmp(cfg.mqtt_root, "X") && !strcmp(cfg.mqtt_app, "a") && cfg.mqtt_tls && cfg.hb_enabled &&
              !strcmp(cfg.hb_host, "hb") && cfg.hb_port == 1 && !strcmp(cfg.hb_callsign, "CALL") &&
              cfg.ch[0].peer_id == 9990200,
          "board keys");
    /* dsp_cores is 1 or 2 */
    snprintf(buf, sizeof buf, "{\"device\":{\"sample_rate\":288000},\"dsp_cores\":7, %s}", one);
    CHECK(rdof_sdr_config_parse(buf, strlen(buf), &cfg, err, sizeof err) && cfg.dsp_cores == 2, "dsp_cores 7");
    snprintf(buf, sizeof buf, "{\"device\":{\"sample_rate\":288000},\"dsp_cores\":0, %s}", one);
    CHECK(rdof_sdr_config_parse(buf, strlen(buf), &cfg, err, sizeof err) && cfg.dsp_cores == 1, "dsp_cores 0");
    /* the same keys in a box's radio, and a radio whose name isn't the default */
    snprintf(buf, sizeof buf,
             "{\"cc\":1,\"channels\":{\"4\":461.0875},\"radios\":[{\"type\":\"mmdvm_hs\",\"name\":\"R\"},{\"type\":\"sdr\","
             "\"usb\":{\"transfers\":3},\"dsp_cores\":2,\"idle_gate\":0,\"learn_ppm\":false,\"selftest\":true,"
             "\"channels\":[4]}]}");
    CHECK(rdof_sdr_config_parse(buf, strlen(buf), &cfg, err, sizeof err) && cfg.usb_transfers == 3 && cfg.usb_transfer_kb == 8 &&
              cfg.dsp_cores == 2 && !cfg.idle_gate && !cfg.learn_ppm && cfg.selftest && !strcmp(cfg.name, "radiofoundry-sdr2") &&
              !strcmp(cfg.ch[0].id, "SDR04"),
          "box radio: %s / %s", cfg.name, err);
}

/* ── learning the tuner's ppm ────────────────────────────────────────────── */

/* Two channels whose corrections both say the tuner is est ppm off. */
static int ppm_step(rdof_ppm_t *p, double est, double t)
{
    const double f[2] = {461.0375e6, 461.0875e6}, age[2] = {1, 2};
    const double corr[2] = {-est * f[0] * 1e-6, -est * f[1] * 1e-6};
    return rdof_ppm_update(p, corr, f, age, 2, t);
}

static void test_ppm_edges(void)
{
    rdof_ppm_t p;
    /* steady for a minute, then an estimate that jumps by more than 0.5 ppm: the minute starts over */
    rdof_ppm_init(&p, 0.0, NAN);
    int first = -1, step_at_first = 0;
    for (int t = 0; t <= 200 && first < 0; t += 5) {
        const double est = t < 30 ? 2.0 : 2.7;
        const int step = ppm_step(&p, est, t);
        if (step) {
            first = t;
            step_at_first = step;
            rdof_ppm_applied(&p, step);
        }
        if (t == 25)
            CHECK(p.channels == 2 && p.steady_s == 25.0 && fabs(p.residual - 2.0) < 1e-9, "steady at 25 s: %g", p.steady_s);
        if (t == 30)
            CHECK(p.steady_s == 0.0 && fabs(p.residual - 2.7) < 1e-9, "the jump restarts the minute: %g", p.steady_s);
    }
    CHECK(first == 90 && step_at_first == 3, "first move at %d s by %d (want 90 s, by 3)", first, step_at_first);
    CHECK(fabs(rdof_ppm_learned(&p) - 2.7) < 1e-9 && p.chip == 3.0 && fabs(p.residual + 0.3) < 1e-9,
          "learned %g, the RTL at %g, %g left", rdof_ppm_learned(&p), p.chip, p.residual);
    /* a small error is learned but not worth moving the RTL for */
    rdof_ppm_init(&p, 0.0, NAN);
    int moves = 0;
    for (int t = 0; t <= 300; t += 5)
        moves += ppm_step(&p, 0.4, t) != 0;
    CHECK(moves == 0 && fabs(rdof_ppm_learned(&p) - 0.4) < 1e-9 && p.steady_s == 300.0, "0.4 ppm: %d moves", moves);
    /* a within-0.5 wobble moves the estimate a quarter of the way and keeps the confidence */
    rdof_ppm_init(&p, 0.0, NAN);
    ppm_step(&p, 2.0, 0);
    ppm_step(&p, 2.4, 5);
    CHECK(fabs(p.residual - 2.1) < 1e-9 && p.steady_s == 5.0, "a quarter of the way: %g", p.residual);

    /* which channels count: fresh (0..30 s old) with a frequency; the rest are ignored */
    const double f[5] = {461.0375e6, 461.0875e6, 461.1375e6, 0.0, 461.2375e6};
    const double age[5] = {10, 31, -1, 5, 30};
    double corr[5];
    for (int i = 0; i < 5; i++)
        corr[i] = -(i == 4 ? 2.2 : 2.0) * 461e6 * 1e-6;
    rdof_ppm_init(&p, 0.0, NAN);
    rdof_ppm_update(&p, corr, f, age, 5, 0);
    CHECK(p.channels == 2 && fabs(p.residual - 2.1) < 0.05, "channels counted: %d, residual %g (the stale, future and unknown ones out)",
          p.channels, p.residual);
    /* an odd number of agreeing channels takes the middle one, an even number the mean of the middle two */
    const double f3[3] = {461.0e6, 461.0e6, 461.0e6}, a3[3] = {1, 1, 1};
    const double c3[3] = {-1.0 * 461, -2.0 * 461, -2.4 * 461};
    rdof_ppm_init(&p, 0.0, NAN);
    rdof_ppm_update(&p, c3, f3, a3, 3, 0);
    CHECK(p.channels == 3 && fabs(p.residual - 2.0) < 1e-9, "median of three: %g", p.residual);
    const double f4[4] = {461.0e6, 461.0e6, 461.0e6, 461.0e6}, a4[4] = {1, 1, 1, 1};
    const double c4[4] = {-1.8 * 461, -2.0 * 461, -2.4 * 461, -2.6 * 461};
    rdof_ppm_init(&p, 0.0, NAN);
    rdof_ppm_update(&p, c4, f4, a4, 4, 0);
    CHECK(p.channels == 4 && fabs(p.residual - 2.2) < 1e-9, "median of four: %g", p.residual);
    /* an outlier more than 1 ppm from the median is dropped */
    const double c5[4] = {-2.0 * 461, -2.1 * 461, -2.2 * 461, -5.0 * 461};
    rdof_ppm_init(&p, 0.0, NAN);
    rdof_ppm_update(&p, c5, f4, a4, 4, 0);
    CHECK(p.channels == 3 && fabs(p.residual - 2.1) < 1e-9, "outlier out: %d channels, %g", p.channels, p.residual);
    /* more channels than the 64 it looks at */
    double f70[70], a70[70], c70[70];
    for (int i = 0; i < 70; i++) {
        f70[i] = 461.0e6;
        a70[i] = 1;
        c70[i] = -(i < 64 ? 2.0 : 9.0) * 461;
    }
    rdof_ppm_init(&p, 0.0, NAN);
    rdof_ppm_update(&p, c70, f70, a70, 70, 0);
    CHECK(p.channels == 64 && fabs(p.residual - 2.0) < 1e-9, "64 of 70 channels: %d, %g", p.channels, p.residual);
    /* one channel alone says nothing, and keeps the confidence it had */
    CHECK(ppm_step(&p, 2.0, 5) == 0 && p.channels == 2, "two channels again");
    const double one_f[1] = {461e6}, one_a[1] = {1}, one_c[1] = {-2 * 461.0};
    const double steady = p.steady_s, res = p.residual;
    CHECK(rdof_ppm_update(&p, one_c, one_f, one_a, 1, 10) == 0 && p.channels == 0 && p.steady_s == steady && p.residual == res,
          "a lone channel: nothing learned, nothing lost");

    /* applied before any estimate: only the RTL's setting moves */
    rdof_ppm_init(&p, 1.0, NAN);
    rdof_ppm_applied(&p, 2);
    CHECK(p.chip == 3.0 && isnan(p.residual) && isnan(rdof_ppm_learned(&p)), "applied with no estimate");
    CHECK(!rdof_ppm_should_save(&p, 1e6), "nothing to save");

    /* measured directly: the same steadiness and moves */
    rdof_ppm_init(&p, 0.0, NAN);
    first = -1;
    for (int t = 0; t <= 200; t += 10) {
        const double ppm = t < 50 ? 3.2 : 4.1;
        const int step = rdof_ppm_measured(&p, ppm, t);
        if (step && first < 0) {
            first = t;
            step_at_first = step;
        }
        if (step)
            rdof_ppm_applied(&p, step);
    }
    CHECK(p.measured && p.channels == 0 && first == 110 && step_at_first == 4, "measured: first move at %d by %d", first,
          step_at_first);
    /* an estimate within the steady band replaces the running one outright (it is already an average) */
    rdof_ppm_init(&p, 0.0, NAN);
    rdof_ppm_measured(&p, 2.0, 0);
    rdof_ppm_measured(&p, 2.3, 10);
    CHECK(p.residual == 2.3 && p.steady_s == 10.0, "measured replaces: %g", p.residual);

    /* saving: a value loaded at boot is saved as soon as it is confident and 0.5 ppm away; a first value
     * (nothing saved ever) as soon as it has held 15 minutes; neither while it hasn't moved enough */
    rdof_ppm_init(&p, 0.0, 2.0);
    for (int t = 0; t <= 960; t += 10)
        rdof_ppm_measured(&p, 2.3, t);
    CHECK(p.steady_s >= RDOF_PPM_FIRST_SAVE_S && !rdof_ppm_should_save(&p, 960), "2.3 against a saved 2.0: not enough");
    for (int t = 970; t <= 1000; t += 10)
        rdof_ppm_measured(&p, 2.6, t);
    CHECK(rdof_ppm_should_save(&p, 1000), "2.6 against a saved 2.0: saved at once");
    rdof_ppm_saved(&p, 1000);
    CHECK(p.saved == 2.6 && !rdof_ppm_should_save(&p, 1010), "just saved");
    for (int t = 1010; t <= 2100; t += 10)
        rdof_ppm_measured(&p, 3.4, t);
    CHECK(p.steady_s >= RDOF_PPM_FIRST_SAVE_S && !rdof_ppm_should_save(&p, 2100), "moved 0.8 and steady, but saved a quarter of an hour ago");
    CHECK(!rdof_ppm_should_save(&p, 1000 + RDOF_PPM_SAVE_EVERY_S - 1) && rdof_ppm_should_save(&p, 1000 + RDOF_PPM_SAVE_EVERY_S),
          "and six hours on");
}

int main(void)
{
    /* RDOF_EDGE_TIMES=1 prints the seconds each test takes */
    const bool times = getenv("RDOF_EDGE_TIMES") != NULL;
    static void (*const tests[])(void) = {
        test_alloc, test_bench, test_rx_utc, test_bench_busy, test_chz_args, test_chz_odd, test_chz_clock, test_chz_level,
        test_rrc, test_fm_edges, test_plan_edges, test_scene, test_demod_edges, test_demod_bent, test_demod_small_feed, test_demod_weaker, test_voice, test_rx_edges, test_frontend_edges,
        test_mod_edges, test_resamp_edges, test_resamp_tones, test_fft_edges, test_rate_edges, test_ppm_edges, test_config_edges, test_config_board_keys, test_describe_edges,
        test_status_edges, test_agc_empty,
#if RDOF_FEATURE_SDR_AGC
        test_agc_reference,
#endif
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        const clock_t t0 = clock();
        tests[i]();
        if (times)
            printf("  test %zu: %.2f s\n", i, (double)(clock() - t0) / CLOCKS_PER_SEC);
    }
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
