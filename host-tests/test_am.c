// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* Analog AM as a signal mode (sdr/src/mode_am.c, rdof/sdr/amrx.h): a job's transmitter through a channel into the
 * receiver, both through the mode's table as the bench line drives them, and what it measures against what was
 * sent. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/jsonp.h"
#include "rdof/sdr/amrx.h"
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
#define NEAR(a, b, tol, what) CHECK(fabs((double)(a) - (double)(b)) <= (tol), "%s: %.4f, not %.4f +- %.4f", what, \
                                    (double)(a), (double)(b), (double)(tol))

#define FS 48000.0

static uint32_t rng = 12345;
static double gauss(void)
{
    double s = 0;
    for (int k = 0; k < 12; k++) {
        rng = rng * 1664525u + 1013904223u;
        s += (rng >> 8) / 16777216.0;
    }
    return s - 6;
}

static const rdof_sig_mode_t *am;
static rdof_jnode_t nodes[512];
static char sbuf[4096];
static rdof_jdoc_t doc = {nodes, 512, 0, sbuf, sizeof sbuf, 0, ""};

typedef struct {
    double gain, cfo, snr_db, pre_s, post_s; /* the channel: level, offset, noise (dB under the peak), quiet */
    const int16_t *pcm;
    uint32_t npcm;
    const char *rx;                          /* the receive request (NULL: the default) */
} chan_t;

typedef struct {
    rdof_amrx_summary_t s;
    char json[16384];
    char report[256];
    double seconds;                          /* the transmitter's own reckoning */
    double peak, trough;                     /* the source's |x| while keyed */
    long keyed, audio, env, records;
    double audio_rate;
} result_t;

/* The job's transmitter through the channel into a receiver (the summary from the receiver's own struct too). */
static bool through(const char *job, const chan_t *ch, result_t *res)
{
    memset(res, 0, sizeof *res);
    res->trough = 9;
    char err[200] = "";
    int root = rdof_jparse(&doc, job, strlen(job));
    CHECK(root >= 0, "the job parses: %s", job);
    void *tx = am->tx_new(&doc, root, FS, &res->seconds, err, sizeof err);
    CHECK(tx != NULL, "tx_new: %s", err);
    if (!tx)
        return false;
    const char *rq = ch->rx ? ch->rx : "{\"streams\":\"audio,env\",\"spectrum_hz\":4000}";
    root = rdof_jparse(&doc, rq, strlen(rq));
    const double sigma = ch->gain * pow(10, -ch->snr_db / 20) / sqrt(2);
    void *rx = am->rx_new(&doc, root, FS, 20 * log10(ch->gain) - 30, err, sizeof err);
    CHECK(rx != NULL, "rx_new: %s", err);
    if (!rx) {
        am->tx_free(tx);
        return false;
    }
    rdof_sig_stream_t st[RDOF_SIG_STREAMS];
    const int ns = am->rx_streams(rx, st, RDOF_SIG_STREAMS);
    CHECK(ns == 2 && !strcmp(st[0].name, "audio") && !strcmp(st[1].name, "env"), "two streams");
    res->audio_rate = st[0].rate;
    const rdof_sig_src_t src = am->tx_src(tx);
    CHECK(src.iq && !src.freq, "AM renders I/Q");
    enum { B = 480 };
    static float x[2 * B], y[2 * B], a[B], e[B];
    float *outs[2] = {a, e};
    int nout[2];
    uint32_t fed = 0;
    double ph = 0;
    const long pre = lround(ch->pre_s * FS), post = lround(ch->post_s * FS);
    long t = 0, after = -1;
    for (;;) {
        while (ch->pcm && fed < ch->npcm && am->tx_in_room(tx)) {
            size_t k = am->tx_in_room(tx);
            k = k > 1000 ? 1000 : k & ~(size_t)1;
            const size_t left = 2 * (size_t)(ch->npcm - fed);
            k = k < left ? k : left;
            if (!k)
                break;
            fed += (uint32_t)(am->tx_in(tx, (const uint8_t *)(ch->pcm + fed), k) / 2);
        }
        if (ch->pcm && fed == ch->npcm)
            am->tx_in_end(tx, false);
        int got = 0;
        if (t >= pre)
            got = src.iq(src.ctx, x, B);
        for (int i = got; i < B; i++)
            x[2 * i] = x[2 * i + 1] = 0;
        res->keyed += got;
        for (int i = 0; i < B; i++) {
            const double mag = hypot(x[2 * i], x[2 * i + 1]);
            const long k = res->keyed - got + i;
            if (i < got && k > 0.01 * FS && k < (res->seconds - 0.01) * FS) /* away from the ramps */
                res->peak = fmax(res->peak, mag), res->trough = fmin(res->trough, mag);
            const double c = cos(ph), s = sin(ph);
            ph = fmod(ph + 2 * M_PI * ch->cfo / FS, 2 * M_PI);
            y[2 * i] = (float)(ch->gain * (x[2 * i] * c - x[2 * i + 1] * s) + sigma * gauss());
            y[2 * i + 1] = (float)(ch->gain * (x[2 * i] * s + x[2 * i + 1] * c) + sigma * gauss());
        }
        am->rx_feed(rx, y, B, outs, nout);
        res->audio += nout[0], res->env += nout[1];
        char rec[1024];
        while (am->rx_record(rx, rec, sizeof rec)) {
            res->records++;
            CHECK(!strncmp(rec, "{\"block\":{", 10), "a block record");
        }
        t += B;
        if (t >= pre && src.done(src.ctx) && (after = (after < 0 ? 0 : after) + B) > post)
            break;
        if (t > (long)(120 * FS))
            break;
    }
    CHECK(am->rx_summary(rx, res->json, sizeof res->json) > 0, "the summary's JSON fits");
    am->tx_report(tx, res->report, sizeof res->report);
    am->rx_free(rx);
    am->tx_free(tx);
    return true;
}

/* A number in the JSON after key (the first match from at). */
static double jnum(const char *json, const char *key)
{
    char k[64];
    snprintf(k, sizeof k, "\"%s\":", key);
    const char *p = strstr(json, k);
    return p ? strtod(p + strlen(k), NULL) : NAN;
}
static const char *after(const char *json, const char *key)
{
    const char *p = strstr(json, key);
    return p ? p : json + strlen(json);
}

static void t_listed(void)
{
    am = rdof_sig_find(rdof_sdr_modes, "am");
    CHECK(am && am->tx_new && am->rx_new, "am is listed, both ways");
    char names[64];
    CHECK(rdof_sig_names_json(rdof_sdr_modes, true, names, sizeof names) && strstr(names, "\"am\"") &&
              strstr(names, "\"fm\""),
          "tx names %s", names);
    CHECK(rdof_sig_names_json(rdof_sdr_modes, false, names, sizeof names) && strstr(names, "\"am\""), "rx names %s",
          names);
}

/* The receiver sensitivity test's signal: 1 kHz at 30%. */
static void t_tone_30(void)
{
    result_t r;
    chan_t ch = {.gain = 0.5, .cfo = 310, .snr_db = 45, .pre_s = 0.3, .post_s = 0.3};
    /* lead and tail carrier would count in the averages (the receiver measures every keyed block): none here */
    if (!through("{\"depth\":0.3,\"lead_s\":0,\"tail_s\":0,\"program\":[{\"kind\":\"tone\",\"hz\":1000,\"s\":2}]}",
                 &ch, &r))
        return;
    NEAR(r.seconds, 2 + 0.002, 0.001, "keyed seconds");
    CHECK(fabs(r.keyed - 2.002 * FS) < 2, "keyed %ld samples", r.keyed);
    NEAR(r.peak, 1.0, 0.002, "the peak at full scale");
    NEAR(r.trough, 0.7 / 1.3, 0.002, "the trough");
    const char *j = r.json;
    NEAR(jnum(j, "carrier_s"), 2.0, 0.03, "carrier seconds");
    NEAR(jnum(j, "carrier_dbfs"), 20 * log10(0.5 / 1.3), 0.1, "the carrier's level");
    NEAR(jnum(j, "cfo_hz"), 310, 3, "carrier offset");
    const char *tn = after(j, "\"tone\":");
    NEAR(jnum(tn, "hz"), 1000, 0.5, "the tone");
    NEAR(jnum(tn, "depth_pct"), 30, 0.6, "the tone's depth");
    CHECK(jnum(tn, "sinad_db") > 35, "SINAD %.1f", jnum(tn, "sinad_db"));
    CHECK(jnum(tn, "thd_pct") < 0.5, "THD %.2f", jnum(tn, "thd_pct"));
    const char *dp = after(j, "\"depth\":");
    NEAR(jnum(dp, "pos_pct"), 30, 1.5, "peak modulation +");
    NEAR(jnum(dp, "neg_pct"), 30, 1.5, "peak modulation -");
    NEAR(jnum(dp, "rms_pct"), 30 / sqrt(2), 0.6, "rms modulation");
    NEAR(jnum(after(j, "\"audio\":"), "rms_pct"), 30 / sqrt(2), 0.6, "the audio's rms");
    CHECK(jnum(after(j, "\"fm\":"), "rms_hz") < 20, "no FM on an AM carrier (%.1f Hz)",
          jnum(after(j, "\"fm\":"), "rms_hz"));
    CHECK(jnum(j, "noise_dbfs") < jnum(j, "power_dbfs") - 35, "the noise before and after");
    CHECK(strstr(j, "\"spectrum\":{"), "spectrum bins asked for");
    NEAR((double)r.audio / r.env, r.audio_rate / FS, 0.01, "audio at its rate");
    CHECK(r.records >= 25, "records %ld (2.6 s of 0.1 s blocks)", r.records);
    CHECK(strstr(r.report, "\"depth\":0.300") && strstr(r.report, "\"carrier_db\":-2.28"), "report %s", r.report);
}

/* Deep modulation, two tones, an offset carrier and an airband audio band. */
static void t_deep_two_tone(void)
{
    result_t r;
    chan_t ch = {.gain = 0.2, .snr_db = 50, .pre_s = 0.2, .post_s = 0.2,
                 .rx = "{\"streams\":\"audio,env\",\"audio_lo_hz\":300,\"audio_hi_hz\":2500,\"nominal_depth\":0.9}"};
    if (!through("{\"depth\":0.9,\"offset_hz\":-1200,\"lead_s\":0,\"tail_s\":0,"
                 "\"program\":[{\"kind\":\"tone\",\"hz\":[700,1900],\"s\":2}]}",
                 &ch, &r))
        return;
    const char *j = r.json;
    NEAR(jnum(j, "cfo_hz"), -1200, 3, "the offset carrier");
    NEAR(r.trough, 0.1 / 1.9, 0.01, "the two tones' trough");
    bool lo = false, hi = false;
    const char *pk = after(j, "\"peaks\":");
    for (const char *p = pk; (p = strstr(p, "{\"hz\":")); p++) {
        const double hz = strtod(p + 6, NULL), dpt = jnum(p, "depth_pct");
        lo |= fabs(hz - 700) < 5 && fabs(dpt - 45) < 2;
        hi |= fabs(hz - 1900) < 5 && fabs(dpt - 45) < 2;
    }
    CHECK(lo && hi, "both tones at 45%%: %.300s", pk);
    NEAR(jnum(after(j, "\"depth\":"), "pos_pct"), 90, 3, "the peak modulation");
    NEAR(jnum(after(j, "\"audio\":"), "rms_db"), 20 * log10(0.45 * sqrt(2) / 0.9) /* two at half */, 0.3,
         "audio against 90%'s tone");
}

/* PCM through the stream input, and the program's other kinds. */
static void t_pcm_and_kinds(void)
{
    const uint32_t n = 8000 * 2;
    int16_t *pcm = malloc(n * sizeof *pcm);
    for (uint32_t i = 0; i < n; i++)
        pcm[i] = (int16_t)lrint(16384 * sin(2 * M_PI * 800 * i / 8000.0));
    result_t r;
    chan_t ch = {.gain = 0.5, .snr_db = 45, .pre_s = 0.2, .post_s = 0.2, .pcm = pcm, .npcm = n};
    if (through("{\"depth\":0.6,\"lead_s\":0,\"tail_s\":0,\"pcm\":{\"rate\":8000,\"samples\":16000}}", &ch, &r)) {
        const char *tn = after(r.json, "\"tone\":");
        NEAR(jnum(tn, "hz"), 800, 0.5, "PCM's tone");
        NEAR(jnum(tn, "depth_pct"), 30, 1, "PCM at half scale: half the depth");
        CHECK(strstr(r.report, "\"pcm_samples\":16000") && strstr(r.report, "\"underruns\":0"), "report %s",
              r.report);
    }
    free(pcm);
    /* ACARS-like MSK (2400 baud, 1200 and 2400 Hz), DTMF, a sweep and noise, never past 100% */
    chan_t c2 = {.gain = 0.5, .snr_db = 45, .pre_s = 0.1, .post_s = 0.1};
    if (through("{\"depth\":1,\"lead_s\":0,\"tail_s\":0,\"program\":["
                "{\"kind\":\"fsk\",\"hex\":\"2B2A16\",\"baud\":2400,\"hz\":[1200,2400]},"
                "{\"kind\":\"dtmf\",\"digits\":\"123\"},{\"kind\":\"sweep\",\"hz\":[300,3000],\"s\":0.5},"
                "{\"kind\":\"noise\",\"s\":0.5,\"level\":0.5}]}",
                &c2, &r)) {
        CHECK(r.peak <= 1.0001 && r.trough >= 0, "within 0 and full scale: %.4f to %.4f", r.trough, r.peak);
        NEAR(r.seconds, 24 / 2400.0 + 0.45 + 0.5 + 0.5 + 0.002, 0.002, "the program's length");
    }
}

/* No program: the lead and tail alone, a bare carrier (the defaults: 0.25 s each). */
static void t_carrier(void)
{
    result_t r;
    chan_t ch = {.gain = 0.5, .snr_db = 45, .pre_s = 0.2, .post_s = 0.2};
    if (!through("{}", &ch, &r))
        return;
    NEAR(r.seconds, 0.502, 0.001, "half a second of carrier");
    NEAR(r.trough, 1 / 1.3, 0.002, "unmodulated at the default depth's carrier");
    CHECK(strstr(r.json, "\"tone\":null"), "no tone: %.200s", after(r.json, "\"audio\""));
    CHECK(jnum(after(r.json, "\"depth\":"), "rms_pct") < 1, "no modulation");
}

static void t_refusals(void)
{
    const char *bad[][2] = {{"{\"depth\":1.5,\"program\":[{\"kind\":\"tone\",\"hz\":1000}]}", "depth"},
                            {"{\"offset_hz\":20000,\"program\":[{\"kind\":\"tone\",\"hz\":1000}]}", "offset_hz"},
                            {"{\"program\":[{\"kind\":\"chirp\"}]}", "chirp"},
                            {"{\"program\":[{\"kind\":\"pcm\"}]}", "pcm"},
                            {"{\"lead_s\":0,\"tail_s\":0}", "something in it"}};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        const int root = rdof_jparse(&doc, bad[i][0], strlen(bad[i][0]));
        char err[200] = "";
        double secs;
        void *tx = am->tx_new(&doc, root, FS, &secs, err, sizeof err);
        CHECK(!tx && strstr(err, bad[i][1]), "refused (%s): %s", bad[i][1], err);
        am->tx_free(tx);
    }
    const char *rbad = "{\"audio_lo_hz\":3000,\"audio_hi_hz\":2500}";
    const int root = rdof_jparse(&doc, rbad, strlen(rbad));
    char err[200] = "";
    CHECK(!am->rx_new(&doc, root, FS, -40, err, sizeof err) && strstr(err, "audio_lo_hz"), "rx refused: %s", err);
}

int main(void)
{
    t_listed();
    if (am) {
        t_tone_30();
        t_deep_two_tone();
        t_pcm_and_kinds();
        t_carrier();
        t_refusals();
    }
    printf("am: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
