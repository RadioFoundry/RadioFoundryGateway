// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * rdofsdr: the SDR receive path on a host, for checking it against reference output
 * and for measuring its CPU cost. Not firmware: it uses stdio and POSIX time.
 *
 *   rdofsdr -i rec.cu8 [-r 1536000] [--center HZ] --ch SDR02:461.4625[:CC[:SLOT]] ...
 *         [--rssi-offset DB] [--ppm PPM] [--no-afc] [--no-gate] [--t0 S] [--chunk N] [--bursts] [--quiet]
 *   rdofsdr --bench [--ghz 3.2] [--reps 3] (same input options)
 *   rdofsdr --soapy ARGS [--gain DB] [--setting KEY=VALUE ...] [--seconds S] (same channel and receiver options)
 *
 * The input is rtl_sdr's cu8, as rtl_sdr writes
 * it; or, when built with SoapySDR (RDOF_SOAPY), any SoapySDR device live: ARGS as SoapySDRUtil takes them
 * ("driver=rtlsdr", "driver=airspy,serial=..."), streamed as CF32 at the rate and tuned to the planned center. Output lines: "<id> json <text>", "<id> raw <text>", and with --bursts
 * "<id> burst <pos> <dt> <n> <slot> <family> <cc> <errors> <power> <offset> <deviation> <hex>".
 */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rdof/layer2.h"
#include "rdof/sdr/bench.h"
#include "rdof/sdr/channelizer.h"
#include "rdof/sdr/demod.h"
#include "rdof/sdr/fft.h"
#include "rdof/sdr/fm.h"
#include "rdof/sdr/iq.h"
#include "rdof/sdr/receiver.h"

#ifdef RDOF_SOAPY
#include <signal.h>

#include <SoapySDR/Device.h>
#include <SoapySDR/Formats.h>
#endif

#define MAX_CH 32

static rdof_sdr_channel_t chans[MAX_CH];
static char ids[MAX_CH][24];
static int nch;
static int quiet, show_bursts;

static void usage(void)
{
    fprintf(stderr, "usage: rdofsdr -i rec.cu8 [-r RATE] [--center HZ] --ch ID:MHZ[:CC[:SLOT]] ... [--rssi-offset DB]\n"
                    "             [--ppm PPM] [--no-afc] [--no-gate] [--t0 S] [--chunk N] [--bursts] [--quiet]\n"
                    "       rdofsdr --bench [--ghz GHZ] [--reps N] (same input options)\n"
                    "       rdofsdr --soapy ARGS [--gain DB] [--setting KEY=VALUE ...] [--seconds S] (live; with SoapySDR)\n");
    exit(2);
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc((size_t)n + 1);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "%s: read failed\n", path);
        exit(1);
    }
    fclose(f);
    *size = (size_t)n;
    return p;
}

static void on_json(void *ctx, const char *ch, const char *json)
{
    if (!quiet)
        printf("%s json %s\n", ch, json);
}

static void on_raw(void *ctx, const char *ch, const char *json)
{
    if (!quiet)
        printf("%s raw %s\n", ch, json);
}

static void on_burst(void *ctx, int c, const rdof_sdr_burst_t *b, double t)
{
    if (!show_bursts)
        return;
    char hex[67];
    for (int i = 0; i < 33; i++)
        snprintf(hex + 2 * i, 3, "%02x", b->b.burst[i]);
    printf("%s burst %lld %d %d %d %s %d %d %.2f %.1f %.1f %s\n", chans[c].id, (long long)b->position, b->b.dt, b->b.n,
           b->b.slot, rdof_family_name(b->b.family), b->b.cc, b->b.sync_errors, b->b.power_dbfs, b->freq_offset_hz,
           b->deviation_hz, hex);
}

/* ── the bench (rdof_sdr_bench) ───────────────────────────────────────────── */

static void bench(const uint8_t *raw, int n, const rdof_sdr_rx_config_t *cfg, double ghz, int reps)
{
    rdof_sdr_bench_t b;
    if (rdof_sdr_bench(raw, n, cfg, chans, nch, now_s, reps, &b)) {
        fprintf(stderr, "bench failed (no tuner plan, or out of memory)\n");
        exit(1);
    }
    const double secs = b.clip_seconds;
    double total = 0.0;
    printf("%d channels at %.0f kS/s (FFT %d points), %.2f s of IQ, %ld bursts; best of %d; %.2f GHz assumed\n", nch,
           cfg->sample_rate / 1e3, b.fft_points, secs, b.bursts, reps, ghz);
    printf("%-46s %12s %14s\n", "stage", "ms per s IQ", "Mcycles per s");
    for (int i = 0; i < RDOF_BENCH_STAGES; i++) {
        printf("%-46s %12.2f %14.1f\n", rdof_bench_stage_names[i], 1e3 * b.seconds[i] / secs,
               ghz * 1e3 * b.seconds[i] / secs);
        total += b.seconds[i];
    }
    printf("%-46s %12.2f %14.1f\n", "total", 1e3 * total / secs, ghz * 1e3 * total / secs);
    printf("%-46s %12.2f %14.1f\n", "(search without the squelch, for comparison)", 1e3 * b.search_no_squelch / secs,
           ghz * 1e3 * b.search_no_squelch / secs);
    printf("forward FFTs: %lld of %d points, %.0f ns each\n", b.ffts, b.fft_points,
           1e9 * b.seconds[RDOF_BENCH_FFT] / (double)b.ffts);
}

#ifdef RDOF_SOAPY
/* ── live input from a SoapySDR device ───────────────────────────────────── */

static volatile sig_atomic_t stopping;
static void on_signal(int sig) { stopping = 1; }

static int soapy_run(rdof_sdr_rx_t *rx, const char *args, double gain, char **settings, int n_settings,
                     double seconds)
{
    SoapySDRKwargs kw = SoapySDRKwargs_fromString(args);
    SoapySDRDevice *dev = SoapySDRDevice_make(&kw);
    SoapySDRKwargs_clear(&kw);
    if (!dev) {
        fprintf(stderr, "soapy %s: %s\n", args, SoapySDRDevice_lastError());
        return 1;
    }
    for (int i = 0; i < n_settings; i++) {
        char *eq = strchr(settings[i], '=');
        if (eq) {
            *eq = 0;
            SoapySDRDevice_writeSetting(dev, settings[i], eq + 1);
        }
    }
    int rc = 1;
    SoapySDRStream *st = NULL;
    if (SoapySDRDevice_setSampleRate(dev, SOAPY_SDR_RX, 0, rx->cfg.sample_rate) ||
        SoapySDRDevice_setFrequency(dev, SOAPY_SDR_RX, 0, rx->center_hz, NULL) ||
        SoapySDRDevice_setGainMode(dev, SOAPY_SDR_RX, 0, isnan(gain)) ||
        (!isnan(gain) && SoapySDRDevice_setGain(dev, SOAPY_SDR_RX, 0, gain))) {
        fprintf(stderr, "soapy setup: %s\n", SoapySDRDevice_lastError());
        goto out;
    }
    double got = SoapySDRDevice_getSampleRate(dev, SOAPY_SDR_RX, 0);
    if (fabs(got - rx->cfg.sample_rate) > 0.5)
        fprintf(stderr, "soapy: the device runs at %.1f S/s, not %.0f: timing and frequencies will be off\n", got,
                rx->cfg.sample_rate);
    st = SoapySDRDevice_setupStream(dev, SOAPY_SDR_RX, SOAPY_SDR_CF32, NULL, 0, NULL);
    if (!st || SoapySDRDevice_activateStream(dev, st, 0, 0, 0)) {
        fprintf(stderr, "soapy stream: %s\n", SoapySDRDevice_lastError());
        goto out;
    }
    fprintf(stderr, "soapy %s: %.0f S/s at %.0f Hz, gain %s\n", args, got, rx->center_hz,
            isnan(gain) ? "auto" : "manual");
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    rdof_cf_t *iq = malloc(sizeof(rdof_cf_t) * (size_t)rx->cfg.max_chunk);
    long long total = 0, want = seconds > 0 ? (long long)(seconds * rx->cfg.sample_rate) : -1;
    int overflows = 0;
    while (!stopping && (want < 0 || total < want)) {
        void *buffs[] = {iq};
        int flags = 0;
        long long t_ns = 0;
        int n = SoapySDRDevice_readStream(dev, st, buffs, (size_t)rx->cfg.max_chunk, &flags, &t_ns, 500000);
        if (n == SOAPY_SDR_TIMEOUT)
            continue;
        if (n == SOAPY_SDR_OVERFLOW) {
            overflows++;
            continue;
        }
        if (n < 0) {
            fprintf(stderr, "soapy read: %d\n", n);
            break;
        }
        rdof_sdr_rx_process(rx, iq, n);
        total += n;
    }
    free(iq);
    SoapySDRDevice_deactivateStream(dev, st, 0, 0);
    fprintf(stderr, "soapy: %.1f s, %d overflows\n", total / rx->cfg.sample_rate, overflows);
    rc = 0;
out:
    if (st)
        SoapySDRDevice_closeStream(dev, st);
    SoapySDRDevice_unmake(dev);
    return rc;
}
#endif

int main(int argc, char **argv)
{
    const char *input = NULL, *soapy = NULL;
    double gain = NAN, seconds = 0;
    char *settings[16];
    int n_settings = 0;
    rdof_sdr_rx_config_t cfg = {1536000.0, 0.0, -55.0, 0.0, true, 1 << 17};
    double t0 = 1760000000.0, ghz = 3.2;
    int do_bench = 0, reps = 3, gate = 1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
#define NEXT()                                                                                                    \
    do {                                                                                                          \
        if (!v)                                                                                                   \
            usage();                                                                                              \
        i++;                                                                                                      \
    } while (0)
        if (!strcmp(a, "-i")) {
            NEXT();
            input = v;
        } else if (!strcmp(a, "-r") || !strcmp(a, "--rate")) {
            NEXT();
            cfg.sample_rate = atof(v);
        } else if (!strcmp(a, "--center")) {
            NEXT();
            cfg.center_hz = atof(v);
        } else if (!strcmp(a, "--rssi-offset")) {
            NEXT();
            cfg.rssi_offset_db = atof(v);
        } else if (!strcmp(a, "--ppm")) {
            NEXT();
            cfg.software_ppm = atof(v);
        } else if (!strcmp(a, "--no-afc")) {
            cfg.afc = false;
        } else if (!strcmp(a, "--no-gate")) {
            gate = 0;
        } else if (!strcmp(a, "--t0")) {
            NEXT();
            t0 = atof(v);
        } else if (!strcmp(a, "--chunk")) {
            NEXT();
            cfg.max_chunk = atoi(v);
        } else if (!strcmp(a, "--bursts")) {
            show_bursts = 1;
        } else if (!strcmp(a, "--quiet")) {
            quiet = 1;
        } else if (!strcmp(a, "--soapy")) {
            NEXT();
            soapy = v;
        } else if (!strcmp(a, "--gain")) {
            NEXT();
            gain = atof(v);
        } else if (!strcmp(a, "--setting")) {
            NEXT();
            if (n_settings < 16)
                settings[n_settings++] = argv[i];
        } else if (!strcmp(a, "--seconds")) {
            NEXT();
            seconds = atof(v);
        } else if (!strcmp(a, "--bench")) {
            do_bench = 1;
        } else if (!strcmp(a, "--ghz")) {
            NEXT();
            ghz = atof(v);
        } else if (!strcmp(a, "--reps")) {
            NEXT();
            reps = atoi(v);
        } else if (!strcmp(a, "--ch")) {
            NEXT();
            if (nch == MAX_CH)
                usage();
            char buf[128];
            snprintf(buf, sizeof buf, "%s", v);
            char *field[4] = {buf, NULL, NULL, NULL}; /* ID:FREQ[:CC[:SLOT]], CC may be empty */
            for (int k = 1; k < 4 && field[k - 1]; k++) {
                char *colon = strchr(field[k - 1], ':');
                if (colon) {
                    *colon = 0;
                    field[k] = colon + 1;
                }
            }
            char *id = field[0], *mhz = field[1], *cc = field[2], *slot = field[3];
            if (!*id || !mhz)
                usage();
            snprintf(ids[nch], sizeof ids[nch], "%.*s", (int)sizeof ids[nch] - 1, id);
            chans[nch].id = ids[nch];
            double f = atof(mhz);
            chans[nch].freq_hz = f < 1e5 ? f * 1e6 : f; /* MHz or Hz, as config._hz */
            chans[nch].cc = cc && *cc ? atoi(cc) : -1;
            chans[nch].slot = slot ? atoi(slot) : 1;
            nch++;
        } else {
            usage();
        }
    }
    if ((!input && !soapy) || !nch || cfg.max_chunk < 1)
        usage();
    if (soapy) {
#ifdef RDOF_SOAPY
        static rdof_sdr_rx_t live;
        rdof_sink_t live_sink = {.on_json = on_json, .on_raw = on_raw};
        if (rdof_sdr_rx_init(&live, &cfg, chans, nch, &live_sink, (double)time(NULL))) {
            fprintf(stderr, "receiver init failed (rate not a multiple of 48 kS/s, or no tuner plan)\n");
            return 1;
        }
        live.on_burst = on_burst;
        live.gate = gate;
        setvbuf(stdout, NULL, _IOLBF, 0);
        int rc = soapy_run(&live, soapy, gain, settings, n_settings, seconds);
        fprintf(stderr, "center %.0f Hz;", live.center_hz);
        for (int c = 0; c < nch; c++)
            fprintf(stderr, " %s %ld bursts AFC %.1f Hz;", chans[c].id, live.bursts[c], live.chz.correction_hz[c]);
        fprintf(stderr, "\n");
        rdof_sdr_rx_free(&live);
        return rc;
#else
        (void)gain, (void)seconds, (void)n_settings;
        fprintf(stderr, "rdofsdr was built without SoapySDR (RDOF_SOAPY)\n");
        return 1;
#endif
    }
    size_t size;
    uint8_t *raw = read_file(input, &size);
    const int n = (int)(size / 2);
    if (do_bench) {
        bench(raw, n, &cfg, ghz, reps < 1 ? 1 : reps);
        free(raw);
        return 0;
    }
    static rdof_sdr_rx_t rx;
    rdof_sink_t sink = {.on_json = on_json, .on_raw = on_raw};
    if (rdof_sdr_rx_init(&rx, &cfg, chans, nch, &sink, t0)) {
        fprintf(stderr, "receiver init failed (rate not a multiple of 48 kS/s, or no tuner plan)\n");
        return 1;
    }
    rx.on_burst = on_burst;
    rx.gate = gate;
    rdof_cf_t *iq = malloc(sizeof(rdof_cf_t) * cfg.max_chunk);
    for (int i = 0; i < n; i += cfg.max_chunk) {
        int m = n - i < cfg.max_chunk ? n - i : cfg.max_chunk;
        rdof_iq_from_cu8(raw + 2 * (size_t)i, (size_t)m, iq);
        rdof_sdr_rx_process(&rx, iq, m);
    }
    fprintf(stderr, "center %.0f Hz;", rx.center_hz);
    for (int c = 0; c < nch; c++)
        fprintf(stderr, " %s %ld bursts AFC %.1f Hz gated %ld;", chans[c].id, rx.bursts[c], rx.chz.correction_hz[c],
                rx.gated[c]);
    fprintf(stderr, "\n");
    rdof_sdr_rx_free(&rx);
    free(iq);
    free(raw);
    return 0;
}
