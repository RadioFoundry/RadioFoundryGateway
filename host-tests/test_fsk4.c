// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* 4FSK as raw dibits (sdr/src/mode_fsk4.c): a job's transmitter rendered, and its frequency at the symbol centers
 * behind each pulse shape's receive filter against the levels sent. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/jsonp.h"
#include "rdof/sdr/modes.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int fails, checks;
#define CHECK(c, ...)                                                    \
    do {                                                                 \
        checks++;                                                        \
        if (!(c)) {                                                      \
            fails++;                                                     \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                  \
            printf(__VA_ARGS__);                                         \
            printf("\n");                                                \
        }                                                                \
    } while (0)

#define FS 48000.0
#define NSYM 400

static rdof_jnode_t nodes[512];
static char sbuf[8192];
static rdof_jdoc_t doc = {nodes, 512, 0, sbuf, sizeof sbuf, 0, ""};
static const rdof_sig_mode_t *m;
static uint8_t bytes[NSYM / 4];
static int levels[NSYM];
static float hz[400000], env[400000];

static uint32_t rng = 777;
static uint32_t rnd(void)
{
    rng = rng * 1664525u + 1013904223u;
    return rng >> 8;
}

/* Random dibits, packed, and the levels they stand for (01 +3, 00 +1, 10 -1, 11 -3) */
static void make_symbols(void)
{
    static const int lv[4] = {1, 3, -1, -3};
    memset(bytes, 0, sizeof bytes);
    for (int k = 0; k < NSYM; k++) {
        const unsigned b = rnd() & 3;
        bytes[k / 4] |= (uint8_t)(b << (6 - 2 * (k % 4)));
        levels[k] = lv[b];
    }
}

static char *hex_of(const uint8_t *b, int n)
{
    static char s[2 * NSYM + 1];
    for (int i = 0; i < n; i++)
        snprintf(s + 2 * i, 3, "%02x", b[i]);
    return s;
}

/* Render the whole job: samples keyed */
static long render(void *tx, double *seconds_out)
{
    const rdof_sig_src_t src = m->tx_src(tx);
    CHECK(src.freq && !src.iq, "fsk4 renders frequency");
    long n = 0;
    while (n < (long)(sizeof hz / sizeof hz[0]) - 480) {
        const int k = src.freq(src.ctx, hz + n, env + n, 480);
        n += k;
        if (k < 480)
            break;
    }
    CHECK(src.done(src.ctx), "done at the end");
    (void)seconds_out;
    return n;
}

static void *job(const char *text, double *seconds)
{
    char err[200] = "";
    const int root = rdof_jparse(&doc, text, strlen(text));
    CHECK(root >= 0, "parses: %s", text);
    void *tx = m->tx_new(&doc, root, FS, seconds, err, sizeof err);
    CHECK(tx != NULL, "tx_new %s: %s", text, err);
    return tx;
}

/* An RRC (roll-off 0.2) receive filter, unit energy a symbol, by its closed form */
static double rrc(double t)
{
    const double a = 0.2;
    if (fabs(t) < 1e-9)
        return 1 - a + 4 * a / M_PI;
    if (fabs(fabs(t) - 1 / (4 * a)) < 1e-9)
        return a / sqrt(2) * ((1 + 2 / M_PI) * sin(M_PI / (4 * a)) + (1 - 2 / M_PI) * cos(M_PI / (4 * a)));
    return (sin(M_PI * t * (1 - a)) + 4 * a * t * cos(M_PI * t * (1 + a))) / (M_PI * t * (1 - 16 * a * a * t * t));
}

/* The levels at each symbol's center behind the shape's receive filter, against those sent: the worst error (Hz) */
static double worst_error(const char *shape, double dev, long first, double sps, long n)
{
    const double unit = dev / 3;
    double worst = 0;
    for (int k = 20; k < NSYM - 20; k++) { /* clear of the ends' tails (and inside the RRC's span) */
        const long c = first + lround((k + 0.5) * sps);
        double v = 0;
        if (!strcmp(shape, "c4fm")) { /* integrate-and-dump over one symbol, by Simpson's rule */
            const int h = (int)(sps / 2);
            for (int i = -h; i <= h; i++)
                v += hz[c + i] * (abs(i) == h ? 1 : (i + h) % 2 ? 4 : 2);
            v /= 3 * 2 * h;
        } else if (!strcmp(shape, "rrc")) { /* the matched RRC, 16 symbols each side */
            const int span = (int)(16 * sps);
            for (int i = -span; i <= span; i++)
                v += hz[c + i] * rrc(i / sps) / sps;
        } else {
            v = hz[c];
        }
        const double e = fabs(v - levels[k] * unit);
        worst = e > worst ? e : worst;
        CHECK(c < n, "inside");
    }
    return worst;
}

int main(void)
{
    m = rdof_sig_find(rdof_sdr_modes, "fsk4");
    CHECK(m && m->tx_new, "fsk4 is listed and transmits");
    if (!m)
        return 1;
    make_symbols();

    /* each shape: the levels at the centers behind its receive filter */
    const struct {
        const char *shape;
        double dev, tol;
    } shapes[] = {{"c4fm", 1800, 3}, {"rc", 1800, 1}, {"rrc", 1944, 6}, {"nrz", 1800, 0.5}};
    for (size_t s = 0; s < sizeof shapes / sizeof shapes[0]; s++) {
        char text[2048];
        snprintf(text, sizeof text, "{\"shape\":\"%s\",\"baud\":4800,\"dev_hz\":%.0f,\"lead_s\":0.01,\"tail_s\":0.01,"
                 "\"hex\":\"%s\"}", shapes[s].shape, shapes[s].dev, hex_of(bytes, NSYM / 4));
        double secs = 0;
        void *tx = job(text, &secs);
        if (!tx)
            continue;
        const long n = render(tx, NULL);
        const long ramp = lround(0.002 * FS), lead = lround(0.01 * FS) + ramp;
        const long want = lead + (long)ceil(NSYM * 10.0) + lead;
        CHECK(n == want, "%s: %ld samples keyed, not %ld", shapes[s].shape, n, want);
        CHECK(fabs(secs - want / FS) < 1e-6, "%s: %.6f s", shapes[s].shape, secs);
        const double e = worst_error(shapes[s].shape, shapes[s].dev, lead, 10.0, n);
        CHECK(e <= shapes[s].tol, "%s: levels off by up to %.2f Hz", shapes[s].shape, e);
        printf("%s: worst level error %.2f Hz\n", shapes[s].shape, e);
        CHECK(env[0] < 0.1f && env[ramp + 5] == 1.0f && env[n - 1] < 0.1f, "%s: ramps", shapes[s].shape);
        char rep[512];
        CHECK(m->tx_report(tx, rep, sizeof rep) > 0 && strstr(rep, "\"symbols\":400"), "%s: report %s",
              shapes[s].shape, rep);
        m->tx_free(tx);
    }

    /* not a whole number of samples a symbol (2400 baud at 48000 is, 4800 at 44100 isn't): still exact */
    {
        char text[2048];
        snprintf(text, sizeof text, "{\"shape\":\"rc\",\"baud\":4800,\"hex\":\"%s\"}", hex_of(bytes, NSYM / 4));
        char err[200];
        const int root = rdof_jparse(&doc, text, strlen(text));
        double secs;
        void *tx = m->tx_new(&doc, root, 44100, &secs, err, sizeof err);
        CHECK(tx != NULL, "44100: %s", err);
        const rdof_sig_src_t src = m->tx_src(tx);
        long n = 0;
        while (src.freq(src.ctx, hz + n, env + n, 480) == 480)
            n += 480;
        /* the frequency at symbol k's center, interpolated */
        const double sps = 44100.0 / 4800, lead = lround(0.01 * 44100) + lround(0.002 * 44100);
        double worst = 0;
        for (int k = 20; k < NSYM - 20; k++) {
            const double c = lead + (k + 0.5) * sps;
            const long i = (long)c;
            const double v = hz[i] + (hz[i + 1] - hz[i]) * (c - i);
            worst = fmax(worst, fabs(v - levels[k] * 600.0));
        }
        CHECK(worst < 25, "44100: levels off by up to %.1f Hz", worst); /* linear interpolation's error */
        m->tx_free(tx);
    }

    /* the stream: symbols after the request, dev_scale_pct, offset_hz, repeat back to back and with gaps */
    {
        double secs;
        void *tx = job("{\"shape\":\"nrz\",\"symbols\":400,\"dev_scale_pct\":50,\"offset_hz\":100,"
                       "\"repeat\":3}", &secs);
        CHECK(m->tx_in_want(tx) == 100, "100 bytes wanted");
        CHECK(m->tx_in(tx, bytes, 60) == 60 && m->tx_in_want(tx) == 40, "60 in");
        CHECK(m->tx_in(tx, bytes + 60, 90) == 40 && m->tx_in_want(tx) == 0, "the rest in, no more");
        const long n = render(tx, NULL);
        const long lead = lround(0.012 * FS);
        CHECK(n == 2 * lead + 3 * 4000, "three plays in one keying: %ld", n);
        double worst = 0;
        for (int r = 0; r < 3; r++)
            for (int k = 0; k < NSYM; k++)
                worst = fmax(worst, fabs(hz[lead + r * 4000 + k * 10 + 5] - (levels[k] * 300.0 + 100)));
        CHECK(worst < 0.5, "scaled and offset: off by %.2f Hz", worst);
        m->tx_free(tx);

        tx = job("{\"shape\":\"nrz\",\"repeat\":2,\"gap_s\":0.5,\"hex\":\"1b\"}", &secs);
        const long n2 = render(tx, NULL);
        const long burst = 2 * lead + 40, gap = 24000;
        CHECK(n2 == 2 * burst + gap, "two keyings, a gap between: %ld", n2);
        CHECK(env[burst + 10] == 0 && env[burst + gap + lead + 5] == 1.0f, "off in the gap, on after");
        /* 1b = 00 01 10 11: +1 +3 -1 -3 */
        CHECK(fabs(hz[lead + 5] - 600) < 0.5 && fabs(hz[lead + 15] - 1800) < 0.5 && fabs(hz[lead + 25] + 600) < 0.5 &&
                  fabs(hz[lead + 35] + 1800) < 0.5, "the dibits' levels: %.0f %.0f %.0f %.0f", hz[lead + 5],
              hz[lead + 15], hz[lead + 25], hz[lead + 35]);
        m->tx_free(tx);
    }

    /* the symbols not yet in at key-up: carrier off a while; ended short: zeros */
    {
        double secs;
        void *tx = job("{\"shape\":\"nrz\",\"symbols\":8}", &secs);
        const rdof_sig_src_t src = m->tx_src(tx);
        CHECK(src.freq(src.ctx, hz, env, 480) == 480 && env[479] == 0, "waits, keyed, carrier off");
        m->tx_in(tx, bytes, 1);
        m->tx_in_end(tx, true);
        long n = 480;
        while (src.freq(src.ctx, hz + n, env + n, 480) == 480)
            n += 480;
        char rep[512];
        m->tx_report(tx, rep, sizeof rep);
        CHECK(strstr(rep, "\"symbols_short\":4,") && strstr(rep, "\"wait_s\":0.010"), "report: %s", rep);
        m->tx_free(tx);
    }

    /* the peak, for a front end that sends frequency (the W12's limit is 4064 Hz): the symbols' own, the worst any
     * could reach when they come later, or clip_hz's; and what's rendered stays under it */
    {
        double secs;
        void *tx = job("{\"shape\":\"c4fm\",\"hex\":\"00000000\"}", &secs); /* +1s: 600 Hz and the step's overshoot */
        double pk = m->tx_src(tx).peak_hz;
        CHECK(pk > 600 && pk < 800, "c4fm steady +1: peak %.0f Hz", pk);
        m->tx_free(tx);
        tx = job("{\"shape\":\"c4fm\",\"symbols\":1000}", &secs);
        pk = m->tx_src(tx).peak_hz;
        CHECK(pk > 4064 && pk < 5000, "c4fm, any symbols: peak %.0f Hz", pk);
        printf("c4fm worst peak %.0f Hz\n", pk);
        m->tx_free(tx);
        char text[2048];
        snprintf(text, sizeof text, "{\"shape\":\"c4fm\",\"clip_hz\":2500,\"hex\":\"%s\"}", hex_of(bytes, NSYM / 4));
        tx = job(text, &secs);
        CHECK(m->tx_src(tx).peak_hz == 2500, "clip_hz caps the peak");
        const long n = render(tx, NULL);
        float mx = 0;
        for (long i = 0; i < n; i++)
            mx = fmaxf(mx, fabsf(hz[i]));
        char rep[512];
        m->tx_report(tx, rep, sizeof rep);
        CHECK(mx <= 2500 && strstr(rep, "\"clipped\":") && !strstr(rep, "\"clipped\":0"), "clipped at %.0f: %s", mx,
              rep);
        m->tx_free(tx);
        snprintf(text, sizeof text, "{\"shape\":\"c4fm\",\"hex\":\"%s\"}", hex_of(bytes, NSYM / 4));
        tx = job(text, &secs);
        pk = m->tx_src(tx).peak_hz;
        render(tx, NULL);
        mx = 0;
        for (long i = 0; i < 4800; i++)
            mx = fmaxf(mx, fabsf(hz[i]));
        CHECK(mx <= pk + 20 && mx > pk - 60, "these symbols' peak %.0f Hz, rendered %.0f", pk, mx);
        m->tx_free(tx);
    }

    /* refused */
    const char *bad[] = {
        "{\"shape\":\"gmsk\",\"hex\":\"00\"}", "{\"hex\":\"0\"}", "{\"hex\":\"zz\"}", "{}", "{\"symbols\":0}",
        "{\"baud\":100,\"hex\":\"00\"}", "{\"dev_hz\":9000,\"hex\":\"00\"}", "{\"repeat\":0,\"hex\":\"00\"}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        char err[200] = "";
        const int root = rdof_jparse(&doc, bad[i], strlen(bad[i]));
        double secs;
        void *tx = m->tx_new(&doc, root, FS, &secs, err, sizeof err);
        CHECK(!tx && err[0], "refused: %s", bad[i]);
        m->tx_free(tx);
    }

    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
