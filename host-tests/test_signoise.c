// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* Noise on a bench transmit job (rust's signoise.rs, rdof/signoise.h): the fields' parse and refusals, the
 * attenuation that keeps the carrier at power_dbm, and what the SDR modem sends: the carrier over the noise as
 * asked, the noise inside its band and not outside, at its offset, the noise mode's level, and the generator's CPU
 * cost per stream sample. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rdof/jsonp.h"
#include "rdof/sdr/fft.h"
#include "rdof/sdr/sdrmodem.h"
#include "rdof/signoise.h"

static int fails;

#define CHECK(cond, ...)                                                                                           \
    do {                                                                                                           \
        if (!(cond)) {                                                                                             \
            fails++;                                                                                               \
            printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                                 \
            printf(__VA_ARGS__);                                                                                   \
            printf("\n");                                                                                          \
        }                                                                                                          \
    } while (0)

#define FS 864000
#define CARRIER_OFF 30000.0 /* the carrier from the stream's center */

/* ── the fields ── */

/* nz from a job's JSON; false with why */
static bool parse(const char *json, double power_dbm, bool alone, rdof_bench_noise_t *nz, char *why, size_t cap)
{
    static rdof_jnode_t nodes[64];
    static char sbuf[1024];
    static rdof_jdoc_t d = {nodes, 64, 0, sbuf, sizeof sbuf, 0, ""};
    const int root = rdof_jparse(&d, json, strlen(json));
    if (root < 0) {
        snprintf(why, cap, "the test's JSON");
        return false;
    }
    why[0] = 0;
    return rdof_signoise_parse(&d, root, power_dbm, alone, nz, why, cap);
}

static void test_parse(void)
{
    rdof_bench_noise_t nz;
    char why[200];
    CHECK(parse("{}", 30, false, &nz, why, sizeof why) && nz.bw_hz == 0, "no noise: %s", why);
    CHECK(parse("{\"noise_bw\":12500,\"cn_db\":20}", 30, false, &nz, why, sizeof why) && nz.bw_hz == 12500 &&
              nz.cn_db == 20 && nz.offset_hz == 0 && !nz.alone,
          "cn_db: %s", why);
    CHECK(parse("{\"noise_bw\":12500,\"noise_dbm\":5}", 30, false, &nz, why, sizeof why) && nz.cn_db == 25,
          "noise_dbm is the carrier's power_dbm less it: cn %.1f, %s", nz.cn_db, why);
    CHECK(parse("{\"noise_bw\":12500,\"cn_db\":10,\"noise_offset_hz\":-25000,\"noise_seed\":7}", 0, false, &nz, why,
                sizeof why) &&
              nz.offset_hz == -25000 && nz.seed == 7,
          "offset and seed: %s", why);
    CHECK(parse("{\"noise_bw\":25000}", 0, true, &nz, why, sizeof why) && nz.alone && nz.bw_hz == 25000,
          "the noise mode: %s", why);

    static const struct {
        const char *json;
        double power;
        bool alone;
        const char *says;
    } bad[] = {
        {"{\"noise_bw\":12500,\"cn_db\":20,\"noise_dbm\":0}", 30, false, "not both"},
        {"{\"noise_bw\":12500}", 30, false, "cn_db or noise_dbm"},
        {"{\"cn_db\":20}", 30, false, "need noise_bw"},
        {"{\"noise_offset_hz\":1000}", 30, false, "need noise_bw"},
        {"{\"noise_bw\":\"wide\",\"cn_db\":20}", 30, false, "numbers"},
        {"{\"noise_bw\":500,\"cn_db\":20}", 30, false, "1000-50000"},
        {"{\"noise_bw\":60000,\"cn_db\":20}", 30, false, "1000-50000"},
        {"{\"noise_bw\":12500,\"cn_db\":20,\"noise_offset_hz\":250000}", 30, false, "noise_offset_hz"},
        {"{\"noise_bw\":12500,\"cn_db\":90}", 30, false, "-20 to 80"},
        {"{\"noise_bw\":12500,\"noise_dbm\":-60}", 30, false, "-20 to 80"},
        {"{\"noise_bw\":12500,\"cn_db\":20,\"noise_seed\":1.5}", 30, false, "noise_seed"},
        {"{\"noise_bw\":12500,\"cn_db\":20}", NAN, false, "power_dbm"},
        {"{}", 0, true, "noise_bw"},
        {"{\"noise_bw\":12500,\"cn_db\":20}", 0, true, "cn_db and noise_dbm are for"},
        {"{\"noise_bw\":12500}", NAN, true, "power_dbm"},
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        CHECK(!parse(bad[i].json, bad[i].power, bad[i].alone, &nz, why, sizeof why) && strstr(why, bad[i].says),
              "%s refused saying \"%s\": \"%s\"", bad[i].json, bad[i].says, why);

    /* the header's and summary's members */
    char js[256];
    nz = (rdof_bench_noise_t){.bw_hz = 12500, .cn_db = 20, .offset_hz = 5000, .seed = 1};
    rdof_signoise_json(&nz, 10, js, sizeof js);
    CHECK(!strcmp(js, ",\"noise_bw\":12500,\"noise_offset_hz\":5000,\"cn_db\":20.0,\"noise_dbm\":-10.0,"
                      "\"carrier_dbm\":10.0"),
          "json: %s", js);
    nz = (rdof_bench_noise_t){.bw_hz = 25000, .cn_db = NAN, .alone = true};
    rdof_signoise_json(&nz, -3, js, sizeof js);
    CHECK(!strcmp(js, ",\"noise_bw\":25000,\"noise_offset_hz\":0,\"cn_db\":null,\"noise_dbm\":-3.0,"
                      "\"carrier_dbm\":null"),
          "json alone: %s", js);
    nz.bw_hz = 0;
    CHECK(rdof_signoise_json(&nz, 0, js, sizeof js) == 0 && !js[0], "json without noise: %s", js);
    printf("signoise: the fields, %zu refusals and the JSON members\n", sizeof bad / sizeof bad[0]);
}

/* ── the attenuation ── */

static void test_atten(void)
{
    char why[256];
    double a = -1;
    /* C/N 20 dB: sigma 0.1, the samples 1/(1 + 0.4) of full scale, 2.92 dB under it */
    rdof_bench_noise_t nz = {.bw_hz = 12500, .cn_db = 20, .seed = 1};
    const double s = 20 * log10(1.4);
    CHECK(fabs(rdof_signoise_scale_db(&nz) - s) < 1e-9, "scale %.4f dB, not %.4f", rdof_signoise_scale_db(&nz), s);
    CHECK(rdof_signoise_atten(&nz, 0, 7, 51, FS, 0, &a, why, sizeof why) && fabs(a - (7 - s)) < 1e-9,
          "0 dBm on a 7 dBm transmitter: %.3f dB under it, not %.3f (%s)", a, 7 - s, why);
    /* the carrier at the transmitter's most leaves no room for the noise */
    CHECK(!rdof_signoise_atten(&nz, 7, 7, 51, FS, 0, &a, why, sizeof why) && strstr(why, "over its most"),
          "7 dBm with noise on a 7 dBm transmitter: %s", why);
    CHECK(rdof_signoise_atten(&nz, 7 - s, 7, 51, FS, 0, &a, why, sizeof why) && fabs(a) < 1e-9,
          "the most with noise: %.3f (%s)", a, why);
    /* under the span: held to it */
    CHECK(rdof_signoise_atten(&nz, -80, 7, 51, FS, 0, &a, why, sizeof why) && a == 51, "held to the span: %.1f", a);
    /* the noise alone: 4 sigma peaks at full scale, its RMS 12.04 dB under */
    rdof_bench_noise_t alone = {.bw_hz = 12500, .cn_db = NAN, .alone = true, .seed = 1};
    CHECK(fabs(rdof_signoise_scale_db(&alone) - 20 * log10(4)) < 1e-9, "alone: %.3f", rdof_signoise_scale_db(&alone));
    CHECK(rdof_signoise_atten(&alone, -20, 7, 51, FS, 0, &a, why, sizeof why) && fabs(a - (27 - 20 * log10(4))) < 1e-9,
          "alone at -20 dBm: %.3f (%s)", a, why);
    /* too wide for the stream, and past the room around freq */
    rdof_bench_noise_t wide = {.bw_hz = 50000, .cn_db = 20, .seed = 1};
    CHECK(!rdof_signoise_atten(&wide, 0, 7, 51, 96000, 0, &a, why, sizeof why) && strstr(why, "quarter"),
          "50 kHz on a 96 kS/s stream: %s", why);
    rdof_bench_noise_t far = {.bw_hz = 12500, .cn_db = 20, .offset_hz = -150000, .seed = 1};
    CHECK(fabs(rdof_signoise_reach_hz(&far) - (150000 + 0.7 * 12500)) < 1e-9, "reach %.0f",
          rdof_signoise_reach_hz(&far));
    CHECK(!rdof_signoise_atten(&far, 0, 7, 51, FS, 0.1 * FS + 12500, &a, why, sizeof why) && strstr(why, "past the"),
          "150 kHz off with 98.9 kHz of room: %s", why);
    CHECK(rdof_signoise_atten(&far, 0, 7, 51, FS, 200000, &a, why, sizeof why), "with 200 kHz of room: %s", why);
    nz.bw_hz = 0;
    CHECK(rdof_signoise_scale_db(&nz) == 0, "no noise, no scale");
    printf("signoise: the attenuation keeps the carrier at power_dbm (%.2f dB less at C/N 20), refusals\n", s);
}

/* ── what the modem sends ── */

static bool tune(void *ctx, uint32_t hz, double *off)
{
    (void)ctx, (void)hz;
    *off = CARRIER_OFF;
    return true;
}

static void no_reply(void *ctx, const uint8_t *f, size_t n) { (void)ctx, (void)f, (void)n; }

typedef struct {
    long left;
    bool carrier;
} cw_t;

static int cw_iq(void *ctx, float *iq, int n)
{
    cw_t *c = ctx;
    const int k = c->left < n ? (int)c->left : n;
    for (int i = 0; i < k; i++)
        iq[2 * i] = c->carrier ? 1.0f : 0.0f, iq[2 * i + 1] = 0;
    c->left -= k;
    return k;
}

static bool cw_done(void *ctx) { return ((cw_t *)ctx)->left == 0; }

#define NS (FS / 2)   /* half a second */
#define SEG 8192      /* a Welch segment: 105 Hz a bin */

typedef struct {
    double carrier; /* |c|^2 at the carrier's offset */
    double psd[SEG]; /* power a bin, the carrier taken out; bin k at k * FS / SEG (wrapped) */
} spectrum_t;

/* n samples of what the modem sends with nz on a carrier (or on the noise mode's nothing) */
static void send(const rdof_bench_noise_t *nz, bool carrier, rdof_cf_t *x, long n)
{
    static rdof_sdrmodem_t m;
    const rdof_sdrmodem_io_t io = {no_reply, NULL, tune, NULL, NULL};
    CHECK(rdof_sdrmodem_init(&m, &io, FS, FS, false) == 0, "init");
    CHECK(rdof_sdrmodem_noise(&m, nz), "the noise");
    cw_t c = {n + FS, carrier};
    const rdof_sig_src_t src = {.iq = cw_iq, .done = cw_done, .ctx = &c};
    char why[128];
    CHECK(rdof_sdrmodem_signal(&m, 438500000, &src, why, sizeof why), "signal: %s", why);
    int64_t pos = 0;
    for (long got = 0; got < n; got += 1000, pos += 1000)
        rdof_sdrmodem_pull_iq(&m, pos, x + got, n - got < 1000 ? (int)(n - got) : 1000);
    rdof_sdrmodem_signal(&m, 0, NULL, why, sizeof why);
    rdof_sdrmodem_noise(&m, NULL);
    rdof_sdrmodem_free(&m);
}

static void analyze(const rdof_cf_t *x, long n, double carrier_hz, spectrum_t *sp)
{
    /* the carrier by correlation, then out */
    double cr = 0, ci = 0;
    for (long j = 0; j < n; j++) {
        const double ph = -2 * M_PI * carrier_hz * (double)j / FS;
        cr += x[j].re * cos(ph) - x[j].im * sin(ph);
        ci += x[j].re * sin(ph) + x[j].im * cos(ph);
    }
    cr /= (double)n, ci /= (double)n;
    sp->carrier = cr * cr + ci * ci;
    rdof_fft_t *f = rdof_fft_new_cold(SEG, false);
    CHECK(f != NULL, "fft");
    static rdof_cf_t in[SEG], out[SEG];
    static double w[SEG];
    double w2 = 0;
    for (int i = 0; i < SEG; i++)
        w[i] = 0.5 - 0.5 * cos(2 * M_PI * i / SEG), w2 += w[i] * w[i];
    memset(sp->psd, 0, sizeof sp->psd);
    int segs = 0;
    for (long s0 = 0; s0 + SEG <= n; s0 += SEG / 2, segs++) {
        for (int i = 0; i < SEG; i++) {
            const long j = s0 + i;
            const double ph = 2 * M_PI * carrier_hz * (double)j / FS;
            in[i].re = (float)((x[j].re - (cr * cos(ph) - ci * sin(ph))) * w[i]);
            in[i].im = (float)((x[j].im - (cr * sin(ph) + ci * cos(ph))) * w[i]);
        }
        rdof_fft_run(f, in, out);
        for (int k = 0; k < SEG; k++)
            sp->psd[k] += ((double)out[k].re * out[k].re + (double)out[k].im * out[k].im) / ((double)SEG * w2);
    }
    for (int k = 0; k < SEG; k++)
        sp->psd[k] /= segs;
    rdof_fft_free(f);
}

/* the power in lo..hi Hz from the stream's center (bins whose centers are in it) and how many bins */
static double band(const spectrum_t *sp, double lo, double hi, int *bins)
{
    double p = 0;
    int k = 0;
    for (int i = 0; i < SEG; i++) {
        const double f = (i < SEG / 2 ? i : i - SEG) * (double)FS / SEG;
        if (f >= lo && f < hi)
            p += sp->psd[i], k++;
    }
    if (bins)
        *bins = k;
    return p;
}

static double db(double x) { return 10 * log10(x); }

static void test_cn(void)
{
    rdof_cf_t *x = calloc(NS, sizeof *x);
    static spectrum_t sp;
    static const double cns[] = {30, 10, 0, -10};
    for (size_t i = 0; i < sizeof cns / sizeof cns[0]; i++) {
        const rdof_bench_noise_t nz = {.bw_hz = 12500, .cn_db = cns[i], .seed = 3};
        send(&nz, true, x, NS);
        analyze(x, NS, CARRIER_OFF, &sp);
        /* The carrier is s exactly (a constant through the modem); the correlation that measures it has the noise's
         * share in its 2 Hz in it, which at C/N -10 dB moves it a few tenths of a dB: C/N is checked on the carrier
         * the samples have and the noise measured, and the carrier measured against it, looser at low C/N. */
        const double nb = band(&sp, CARRIER_OFF - 6250, CARRIER_OFF + 6250, NULL);
        const double s = 1 / (1 + 4 * pow(10, -cns[i] / 20));
        const double cn = db(s * s / nb);
        const double tol = cns[i] >= 10 ? 0.05 : 0.6;
        CHECK(fabs(cn - cns[i]) < 0.5, "C/N asked %.1f dB, sent %.2f dB", cns[i], cn);
        CHECK(fabs(db(sp.carrier) - 20 * log10(s)) < tol, "C/N %.0f: the carrier measured at %.2f dBFS, not %.2f",
              cns[i], db(sp.carrier), 20 * log10(s));
        printf("signoise: C/N %5.1f dB asked, %6.2f sent (%.2f on the carrier as measured); the carrier %.2f dBFS\n",
               cns[i], cn, db(sp.carrier / nb), 20 * log10(s));
    }

    /* the band limit, at 25 kHz: flat inside, down outside (the low-pass is 60 dB down from 0.7 noise_bw; the
     * linear interpolation's images around the noise rate are what's left) */
    const double bw = 25000;
    const rdof_bench_noise_t nz = {.bw_hz = bw, .cn_db = 0, .seed = 9};
    send(&nz, true, x, NS);
    analyze(x, NS, CARRIER_OFF, &sp);
    int kin, kgap, kall;
    const double pin = band(&sp, CARRIER_OFF - bw / 2, CARRIER_OFF + bw / 2, &kin);
    const double rate = FS / floor(FS / (4 * bw));
    /* between the band's stop and the first image's near edge, each side */
    const double gap = band(&sp, CARRIER_OFF + 0.7 * bw, CARRIER_OFF + rate - 0.7 * bw, &kgap) +
                       band(&sp, CARRIER_OFF - rate + 0.7 * bw, CARRIER_OFF - 0.7 * bw, NULL);
    const double all = band(&sp, -FS / 2.0, FS / 2.0, &kall);
    const double out = all - band(&sp, CARRIER_OFF - 0.7 * bw, CARRIER_OFF + 0.7 * bw, NULL);
    const double gap_db = db((gap / (2 * kgap)) / (pin / kin));
    const double out_db = db(out / pin);
    /* the first image's strongest bins */
    double img = 0;
    for (int i = 0; i < SEG; i++) {
        const double f = (i < SEG / 2 ? i : i - SEG) * (double)FS / SEG - CARRIER_OFF;
        if (fabs(fabs(f) - rate) < bw / 2)
            img = fmax(img, sp.psd[i]);
    }
    const double img_db = db(img / (pin / kin));
    CHECK(gap_db < -55, "between the band and its image: %.1f dB under the band's density", gap_db);
    CHECK(out_db < -35, "outside the band: %.1f dB under the band's power", out_db);
    printf("signoise: 25 kHz band (made at %.0f S/s): %.1f dB down between it and its image, the image's peak %.1f "
           "dB, all outside %.1f dB\n",
           rate, gap_db, img_db, out_db);

    /* at an offset: the noise around the carrier's offset plus noise_offset_hz */
    const rdof_bench_noise_t off = {.bw_hz = 12500, .cn_db = 0, .offset_hz = -100000, .seed = 5};
    send(&off, true, x, NS);
    analyze(x, NS, CARRIER_OFF, &sp);
    const double c = CARRIER_OFF - 100000;
    const double at = band(&sp, c - 6250, c + 6250, NULL);
    const double home = band(&sp, CARRIER_OFF - 6250, CARRIER_OFF + 6250, NULL);
    CHECK(fabs(db(sp.carrier / at)) < 0.5, "offset: C/N %.2f dB in the noise's band", db(sp.carrier / at));
    CHECK(db(home / at) < -50, "offset: %.1f dB of it around the carrier", db(home / at));
    printf("signoise: noise 100 kHz under the carrier: C/N %.2f dB there, %.1f dB of it at the carrier\n",
           db(sp.carrier / at), db(home / at));
    free(x);
}

static void test_alone(void)
{
    rdof_cf_t *x = calloc(NS, sizeof *x);
    static spectrum_t sp;
    const rdof_bench_noise_t nz = {.bw_hz = 12500, .cn_db = NAN, .offset_hz = 20000, .alone = true, .seed = 11};
    send(&nz, false, x, NS);
    analyze(x, NS, CARRIER_OFF, &sp);
    const double c = CARRIER_OFF + 20000;
    const double p = band(&sp, c - 6250, c + 6250, NULL);
    double peak = 0;
    for (long j = 0; j < NS; j++)
        peak = fmax(peak, hypot(x[j].re, x[j].im));
    CHECK(fabs(db(p) + 20 * log10(4)) < 0.3, "the noise alone: %.2f dBFS in its band, not -12.04", db(p));
    CHECK(sp.carrier < 1e-4, "no carrier: %.2g", sp.carrier);
    CHECK(peak < 1.6, "its peak %.2f", peak);
    printf("signoise: the noise mode: %.2f dBFS in noise_bw (-12.04 asked), no carrier, peak %.2f\n", db(p), peak);
    free(x);
}

/* ── the cost: the generator alone, per stream sample; Mcycles a second at an assumed 3.2 GHz (docs/reference.md
 * "CPU budget"). The streams: a Pluto's with --fpga-duc (192 kS/s), the bench's 864 kS/s, a Pluto's wider ones.
 * ── */

static void test_cpu(void)
{
    static const double rates[] = {192000, 864000, 1632000, 2400000};
    static const double bws[] = {12500, 50000};
    const int n = 1 << 16;
    float *buf = calloc(2 * (size_t)n, sizeof *buf);
    for (size_t r = 0; r < sizeof rates / sizeof rates[0]; r++)
        for (size_t b = 0; b < 2; b++) {
            const double bw = fmin(bws[b], rates[r] / 4);
            const rdof_bench_noise_t nz = {.bw_hz = bw, .cn_db = 10, .offset_hz = 100000, .seed = 1};
            void *g = rdof_signoise_new(&nz, rates[r]);
            CHECK(g != NULL, "a generator");
            if (!g)
                continue;
            struct timespec t0, t1;
            long total = 0;
            double s;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            do {
                rdof_signoise_add(g, buf, n, 30000);
                total += n;
                clock_gettime(CLOCK_MONOTONIC, &t1);
                s = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9;
            } while (s < 0.2);
            const double ns = s * 1e9 / (double)total;
            printf("signoise: cost at %.0f S/s, noise_bw %.0f (made at %.0f S/s): %.2f ns a stream sample, %.1f "
                   "Mcycles/s\n",
                   rates[r], bw, rdof_signoise_rate(g), ns, ns * rates[r] * 3.2e-6);
            rdof_signoise_free(g);
        }
    free(buf);
}

int main(void)
{
    test_parse();
    test_atten();
    test_cn();
    test_alone();
    test_cpu();
    if (fails)
        printf("%d FAILED\n", fails);
    else
        printf("all passed\n");
    return fails ? 1 : 0;
}
