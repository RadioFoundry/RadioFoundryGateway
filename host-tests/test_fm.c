// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The analog FM generator (rdof/sdr/fmgen.h) and receiver (rdof/sdr/fmrx.h): the one's signal through a channel into the
 * other, and what it measures against what was sent. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/sdr/fmgen.h"
#include "rdof/sdr/fmrx.h"
#include "rdof/sdr/modes.h"
#include "rdof/jsonp.h"

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

static rdof_fmg_cfg_t base(void)
{
    rdof_fmg_cfg_t c;
    memset(&c, 0, sizeof c);
    c.dev_hz = 2500;
    return c;
}

static void tone(rdof_fmg_cfg_t *c, float hz, float seconds, float level)
{
    rdof_fmg_seg_t *s = &c->seg[c->nseg++];
    s->kind = RDOF_FMG_TONE, s->ntones = 1, s->hz[0] = hz, s->seconds = seconds, s->level = level;
}

typedef struct {
    double cfo, snr_db, pre_s, post_s; /* the channel: offset, noise, quiet before and after */
    int16_t *pcm;
    uint32_t npcm;
    double tail_hz, tail_s, tail_dev; /* after the transmission, a turn-off tone this long on the carrier */
} chan_t;

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

/* The generator's transmission through a channel into a receiver: the summary (and the receiver, freed). */
static void through(const rdof_fmg_cfg_t *c, const chan_t *ch, rdof_fmrx_opts_t o, rdof_fmrx_summary_t *s,
                    float *audio_rms)
{
    static int16_t ring[8192];
    rdof_fmg_t *g = calloc(1, sizeof *g);
    CHECK(rdof_fmg_init(g, c, FS, ring, 8192) == 0, "init: %s", rdof_fmg_check(c) ? rdof_fmg_check(c) : "?");
    rdof_fmrx_t *r = rdof_fmrx_new(FS, &o, NULL, NULL);
    CHECK(r != NULL, "the receiver");
    const double sigma = pow(10, -ch->snr_db / 20) / sqrt(2);
    double ph = 0;
    uint32_t fed = 0;
    float dev[480], env[480], iq[960], audio[480];
    double asum = 0;
    long an = 0;
    const long pre = lround(ch->pre_s * FS), post = lround(ch->post_s * FS);
    long t = 0, after = -1, tail = lround(ch->tail_s * FS), tail_i = 0;
    for (;;) {
        if (ch->pcm && fed < ch->npcm) {
            const uint32_t room = rdof_fmg_pcm_space(g), k = ch->npcm - fed < room ? ch->npcm - fed : room;
            fed += rdof_fmg_pcm_push(g, ch->pcm + fed, k);
            if (fed == ch->npcm)
                rdof_fmg_pcm_end(g);
        }
        if (t >= pre && rdof_fmg_done(g) && tail > 0) { /* the turn-off tone, in place of the subaudio */
            for (int i = 0; i < 480; i++, tail_i++) {
                const bool on = tail_i < tail;
                dev[i] = on ? (float)(ch->tail_dev * sin(2 * M_PI * ch->tail_hz * tail_i / FS)) : 0;
                env[i] = on ? 1 : 0;
            }
            if (tail_i >= tail)
                tail = 0;
        } else if (t >= pre) {
            rdof_fmg_render(g, dev, env, 480);
        } else {
            memset(dev, 0, sizeof dev), memset(env, 0, sizeof env);
        }
        for (int i = 0; i < 480; i++) {
            ph = fmod(ph + 2 * M_PI * (dev[i] + ch->cfo) / FS, 2 * M_PI);
            iq[2 * i] = (float)(0.5 * env[i] * cos(ph) + sigma * 0.5 * gauss());
            iq[2 * i + 1] = (float)(0.5 * env[i] * sin(ph) + sigma * 0.5 * gauss());
        }
        int na = 0;
        rdof_fmrx_feed(r, iq, 480, NULL, audio, &na);
        for (int i = 0; i < na; i++)
            asum += (double)audio[i] * audio[i], an++;
        t += 480;
        if (t >= pre && rdof_fmg_done(g) && !tail && (after = (after < 0 ? 0 : after) + 480) > post)
            break; /* the quiet after */
        if (t > (long)(400 * FS))
            break;
    }
    rdof_fmrx_summary(r, s);
    if (audio_rms)
        *audio_rms = an ? (float)sqrt(asum / an) : 0;
    char json[16384];
    CHECK(rdof_fmrx_summary_json(r, s, json, sizeof json) > 0, "the summary's JSON fits");
    rdof_fmrx_block_t b;
    long blocks = 0;
    while (rdof_fmrx_block(r, &b))
        blocks++;
    CHECK(blocks > 0, "blocks");
    rdof_fmrx_free(r);
    free(g);
}

static rdof_fmrx_opts_t opts(void)
{
    return (rdof_fmrx_opts_t){.squelch_dbfs = -20, .block_s = 0.1f, .nominal_dev_hz = 2500, .spectrum_hz = 4000};
}

static void t_dcs_words(void)
{
    /* +023 is -047's pattern (a DCS code's known alias): the bit order and check bits as radios have them */
    const uint32_t w023 = rdof_dcs_word(0023), w047 = rdof_dcs_word(0047);
    bool inv = true;
    CHECK(rdof_dcs_lookup(w023, &inv) == 0023 && !inv, "023 is 023N");
    CHECK(rdof_dcs_lookup(~w047 & 0x7FFFFF, &inv) == 0023 && !inv, "047I reads as 023N");
    CHECK(rdof_dcs_lookup(~w023 & 0x7FFFFF, &inv) == 0047 && !inv, "023I reads as 047N");
    CHECK((w023 & 0xFFF) == (0023 | 0x800), "the data bits: the code, then 100");
    CHECK(w023 >> 23 == 0, "23 bits");
    for (int k = 1; k < 23; k++) /* any rotation is the same code */
        CHECK(rdof_dcs_lookup(((w023 >> k) | (w023 << (23 - k))) & 0x7FFFFF, &inv) == 0023, "rotation %d", k);
    CHECK(rdof_dcs_lookup(0x155555, &inv) == -1, "not a code");
    float lo, hi;
    CHECK(rdof_dtmf_tones('5', &lo, &hi) && lo == 770 && hi == 1336, "DTMF 5");
    CHECK(rdof_dtmf_tones('d', &lo, &hi) && lo == 941 && hi == 1633, "DTMF D");
    CHECK(!rdof_dtmf_tones('E', &lo, &hi), "DTMF E");
}

static void t_generator(void)
{
    rdof_fmg_cfg_t c = base();
    c.sub = RDOF_FMG_SUB_CTCSS, c.ctcss_hz = 100, c.sub_dev_hz = 500, c.lead_s = 0.1f, c.tail_s = 0.1f;
    tone(&c, 1000, 1, 1);
    CHECK(!rdof_fmg_check(&c), "a tone with CTCSS");
    NEAR(rdof_fmg_seconds(&c), 1.2 + RDOF_FMG_RAMP_S, 1e-6, "length");
    NEAR(rdof_fmg_peak_hz(&c), 3000, 1e-3, "peak");
    rdof_fmg_t g;
    CHECK(rdof_fmg_init(&g, &c, FS, NULL, 0) == 0, "init");
    float dev[4800], env[4800];
    double peak = 0, sub_peak = 0;
    long keyed = 0;
    for (int b = 0; b < 13; b++) {
        const int k = rdof_fmg_render(&g, dev, env, 4800);
        keyed += k;
        for (int i = 0; i < k; i++) {
            const double t = (b * 4800 + i) / FS;
            if (t > 0.01 && t < 0.09)
                sub_peak = fmax(sub_peak, fabs(dev[i]));
            peak = fmax(peak, fabs(dev[i]));
        }
    }
    CHECK(keyed == lround(1.202 * FS), "keyed %ld samples", keyed);
    CHECK(rdof_fmg_done(&g), "done");
    NEAR(sub_peak, 500, 1, "the lead: CTCSS alone");
    CHECK(peak <= 3000.5 && peak > 2950, "the peak deviation %.1f", peak);
    CHECK(env[4799] == 0, "unkeyed after");

    /* refusals */
    rdof_fmg_cfg_t bad = base();
    CHECK(rdof_fmg_check(&bad) != NULL, "nothing to send");
    bad = base(), tone(&bad, 1000, 1, 1.5f);
    CHECK(rdof_fmg_check(&bad) != NULL, "a level over 1");
    bad = base(), bad.dev_hz = 9000, tone(&bad, 1000, 1, 1);
    CHECK(rdof_fmg_check(&bad) != NULL, "too much deviation");
    bad = base(), bad.sub = RDOF_FMG_SUB_DCS, bad.dcs = 01000, tone(&bad, 1000, 1, 1);
    CHECK(rdof_fmg_check(&bad) != NULL, "a DCS code past 777");
    bad = base(), bad.pcm_rate = 8000, bad.pcm_samples = 8000, bad.repeat = 2;
    bad.seg[bad.nseg++] = (rdof_fmg_seg_t){.kind = RDOF_FMG_PCM, .level = 1};
    CHECK(rdof_fmg_check(&bad) != NULL, "PCM repeated");
    bad = base(), bad.data[0] = 'X', bad.seg[bad.nseg++] = (rdof_fmg_seg_t){.kind = RDOF_FMG_DTMF, .level = 1,
                                                                             .seconds = 0.1f, .n = 1};
    CHECK(rdof_fmg_check(&bad) != NULL, "a DTMF digit X");

    /* pre-emphasis: 0 dB at 1 kHz, +9.4 dB at 3 kHz (750 us) */
    c = base(), c.preemph_us = 750, tone(&c, 3000, 0.5f, 1);
    rdof_fmg_init(&g, &c, FS, NULL, 0);
    peak = 0;
    for (int b = 0; b < 5; b++) {
        rdof_fmg_render(&g, dev, env, 4800);
        for (int i = 2400; i < 4800; i++)
            peak = fmax(peak, fabs(dev[i]));
    }
    NEAR(20 * log10(peak / 2500), 20 * log10(sqrt(1 + pow(2 * M_PI * 750e-6 * 3000, 2)) /
                                              sqrt(1 + pow(2 * M_PI * 750e-6 * 1000, 2))),
         0.3, "pre-emphasis at 3 kHz, dB");
}

static void t_tone_ctcss(void)
{
    rdof_fmg_cfg_t c = base();
    c.sub = RDOF_FMG_SUB_CTCSS, c.ctcss_hz = 100, c.sub_dev_hz = 500, c.sub_end_s = 0.2f;
    tone(&c, 1000, 3, 1);
    rdof_fmrx_summary_t s;
    float arms;
    chan_t ch = {.cfo = 420, .snr_db = 40, .pre_s = 0.5, .post_s = 0.5};
    through(&c, &ch, opts(), &s, &arms);
    /* the receiver measures from 60 ms after the key-up: the tone is 3 s of 3.14 */
    const double on = sqrt(3 / 3.14);
    NEAR(s.carrier_s, 3.2, 0.03, "carrier seconds");
    NEAR(s.cfo_hz, 420, 3, "carrier offset");
    CHECK(s.tone, "a tone");
    NEAR(s.tone_hz, 1000, 0.5, "tone frequency");
    NEAR(s.tone_dev_hz, 2500 * on, 60, "tone deviation");
    CHECK(s.sinad_db > 30, "SINAD %.1f", s.sinad_db);
    CHECK(s.thd_pct < 1, "THD %.2f", s.thd_pct);
    NEAR(s.dev_pos_hz, 3000, 150, "peak deviation +");
    NEAR(s.dev_neg_hz, -3000, 150, "peak deviation -");
    CHECK(s.sub == RDOF_FMG_SUB_CTCSS, "CTCSS");
    NEAR(s.ctcss_hz, 100.0, 0.1, "CTCSS frequency");
    NEAR(s.ctcss_nominal_hz, 100.0, 0.01, "CTCSS tone");
    NEAR(s.ctcss_dev_hz, 500, 40, "CTCSS deviation");
    NEAR(s.reverse_burst_t, 0.5 + 3.0, 0.06, "the reverse burst");
    NEAR(s.audio_rms_hz, 2500 / sqrt(2) * on, 60, "filtered audio rms (CTCSS out)");
    CHECK(s.power_dbfs > -7 && s.power_dbfs < -5, "power %.1f dBFS", s.power_dbfs);
    CHECK(s.noise_dbfs < s.power_dbfs - 30, "noise %.1f dBFS", s.noise_dbfs);
    CHECK(s.am_pct < 5, "AM %.2f%%", s.am_pct);
}

static void t_ctcss_tail(void)
{
    /* a turn-off tone after the CTCSS (55 Hz, some radios' second tone slot): the tone's frequency is the CTCSS
     * windows', not pulled down by the tail's */
    rdof_fmg_cfg_t c = base();
    c.sub = RDOF_FMG_SUB_CTCSS, c.ctcss_hz = 100, c.sub_dev_hz = 340;
    tone(&c, 1000, 3, 0.0001f);
    rdof_fmrx_summary_t s;
    chan_t ch = {.cfo = 300, .snr_db = 30, .pre_s = 0.3, .post_s = 0.5, .tail_hz = 55, .tail_s = 0.6, .tail_dev = 340};
    through(&c, &ch, opts(), &s, NULL);
    CHECK(s.sub == RDOF_FMG_SUB_CTCSS && s.ctcss_nominal_hz == 100.0f, "CTCSS %.1f", s.ctcss_nominal_hz);
    NEAR(s.ctcss_hz, 100.0, 0.05, "CTCSS frequency with a 55 Hz tail");
    NEAR(s.ctcss_dev_hz, 340, 30, "CTCSS deviation with a 55 Hz tail");
}

static void t_ctcss_tones(void)
{
    /* neighbors 1.4 Hz apart, and the band's ends */
    static const float tones[] = {67.0f, 150.0f, 151.4f, 254.1f};
    for (int i = 0; i < 4; i++) {
        rdof_fmg_cfg_t c = base();
        c.sub = RDOF_FMG_SUB_CTCSS, c.ctcss_hz = tones[i], c.sub_dev_hz = 600;
        tone(&c, 1500, 3, 0.8f);
        rdof_fmrx_summary_t s;
        chan_t ch = {.snr_db = 30, .pre_s = 0.2, .post_s = 0.2};
        through(&c, &ch, opts(), &s, NULL);
        CHECK(s.sub == RDOF_FMG_SUB_CTCSS && s.ctcss_nominal_hz == tones[i], "CTCSS %.1f: read %.1f (%.2f)", tones[i],
              s.ctcss_nominal_hz, s.ctcss_hz);
        NEAR(s.ctcss_dev_hz, 600, 60, "CTCSS deviation");
        CHECK(s.reverse_burst_t < 0, "no reverse burst asked");
    }
}

static void t_dcs(void)
{
    static const struct {
        uint16_t code;
        bool inv;
        uint16_t read;
        bool read_inv;
        const char *alias;
    } cases[] = {{0023, false, 0023, false, "047I"}, {0023, true, 0047, false, "023I"}, {0754, false, 0754, false, ""}};
    for (int i = 0; i < 3; i++) {
        rdof_fmg_cfg_t c = base();
        c.sub = RDOF_FMG_SUB_DCS, c.dcs = cases[i].code, c.dcs_inv = cases[i].inv, c.sub_dev_hz = 600;
        c.sub_end_s = i == 2 ? 0.18f : 0.3f; /* the last: the transmitter's own default, shorter than CTCSS's window */
        tone(&c, 1000, 3, 0.8f);
        rdof_fmrx_summary_t s;
        chan_t ch = {.cfo = -250, .snr_db = 30, .pre_s = 0.2, .post_s = 0.2};
        through(&c, &ch, opts(), &s, NULL);
        CHECK(s.sub == RDOF_FMG_SUB_DCS, "DCS %03o%c: seen", cases[i].code, cases[i].inv ? 'I' : 'N');
        CHECK(s.dcs_code == cases[i].read && s.dcs_inv == cases[i].read_inv, "DCS %03o%c: read %03o%c",
              cases[i].code, cases[i].inv ? 'I' : 'N', s.dcs_code, s.dcs_inv ? 'I' : 'N');
        CHECK(!cases[i].alias[0] || strstr(s.dcs_aliases, cases[i].alias), "aliases %s", s.dcs_aliases);
        CHECK(s.dcs_words >= 15, "words %ld", s.dcs_words);
        CHECK(s.dcs_turnoff, "the turn-off tone, %.2f s", c.sub_end_s);
        NEAR(s.tone_hz, 1000, 1, "the tone over DCS");
        NEAR(s.cfo_hz, -250, 12, "the carrier offset, the code's mean out (the turn-off pulls it a little)");
        NEAR(s.dcs_dev_hz, 600, 60, "DCS deviation");
    }
}

static void t_afsk_dtmf_sweep(void)
{
    /* Bell 202: 1200 baud, mark 1200 Hz, space 2200, NRZI, LSB first */
    rdof_fmg_cfg_t c = base();
    for (int i = 0; i < 200; i++)
        c.data[i] = (uint8_t)(i * 37 + 11);
    c.seg[c.nseg++] = (rdof_fmg_seg_t){.kind = RDOF_FMG_FSK, .level = 1, .hz = {1200, 2200}, .baud = 1200, .lsb = true,
                                       .nrzi = true, .off = 0, .n = 1600};
    CHECK(!rdof_fmg_check(&c), "AFSK: %s", rdof_fmg_check(&c));
    NEAR(rdof_fmg_seconds(&c), 1600 / 1200.0 + RDOF_FMG_RAMP_S, 1e-6, "AFSK length");
    rdof_fmrx_summary_t s;
    chan_t ch = {.snr_db = 35, .pre_s = 0.2, .post_s = 0.2};
    through(&c, &ch, opts(), &s, NULL);
    bool m = false, sp = false;
    for (int i = 0; i < s.npeaks; i++)
        m |= fabs(s.peak_hz[i] - 1200) < 60, sp |= fabs(s.peak_hz[i] - 2200) < 60;
    CHECK(m && sp, "AFSK: mark and space in the peaks");
    NEAR(s.dev_pos_hz, 2500, 250, "AFSK deviation");

    c = base();
    memcpy(c.data, "159#", 4);
    c.seg[c.nseg++] = (rdof_fmg_seg_t){.kind = RDOF_FMG_DTMF, .level = 1, .seconds = 0.1f, .gap_s = 0.05f, .n = 4};
    NEAR(rdof_fmg_seconds(&c), 0.6 + RDOF_FMG_RAMP_S, 1e-6, "DTMF length");
    through(&c, &ch, opts(), &s, NULL);
    NEAR(s.dev_pos_hz, 2500, 250, "DTMF deviation");

    c = base();
    c.seg[c.nseg++] = (rdof_fmg_seg_t){.kind = RDOF_FMG_SWEEP, .level = 0.5f, .seconds = 2, .hz = {300, 3000},
                                       .log = true};
    through(&c, &ch, opts(), &s, NULL);
    CHECK(!s.tone, "a sweep isn't a tone");
    NEAR(s.dev_pos_hz, 1250, 120, "sweep deviation");
}

static void t_pcm(void)
{
    const uint32_t n = 8000 * 2;
    int16_t *pcm = malloc(n * sizeof *pcm);
    for (uint32_t i = 0; i < n; i++)
        pcm[i] = (int16_t)lrint(16384 * sin(2 * M_PI * 800 * i / 8000.0));
    rdof_fmg_cfg_t c = base();
    c.pcm_rate = 8000, c.pcm_samples = n;
    c.seg[c.nseg++] = (rdof_fmg_seg_t){.kind = RDOF_FMG_PCM, .level = 1};
    rdof_fmrx_summary_t s;
    chan_t ch = {.snr_db = 35, .pre_s = 0.2, .post_s = 0.2, .pcm = pcm, .npcm = n};
    through(&c, &ch, opts(), &s, NULL);
    CHECK(s.tone, "PCM's tone");
    NEAR(s.tone_hz, 800, 0.5, "PCM's tone frequency");
    NEAR(s.tone_dev_hz, 1250, 40, "PCM at half scale: half the deviation");
    CHECK(s.sinad_db > 25, "PCM's SINAD %.1f", s.sinad_db);

    /* nothing pushed: silence, counted */
    rdof_fmg_t g;
    static int16_t ring[1024];
    rdof_fmg_init(&g, &c, FS, ring, 1024);
    float dev[4800], env[4800];
    rdof_fmg_render(&g, dev, env, 4800);
    CHECK(g.underruns >= 799 && g.underruns <= 801, "underruns %u", g.underruns);
    CHECK(rdof_fmg_init(&g, &c, FS, ring, 1000) == -1, "a ring that isn't a power of 2");
    free(pcm);
}

/* The mode through its table only, as the bench line drives it: a job's JSON in, its samples integrated to I/Q, the
 * receiver's streams, records and summary out. */
static void t_mode(void)
{
    const rdof_sig_mode_t *m = rdof_sig_find(rdof_sdr_modes, "fm");
    CHECK(m && m->tx_new && m->rx_new, "fm is listed, both ways");
    CHECK(!rdof_sig_find(rdof_sdr_modes, "p25"), "no p25 yet");
    char names[64];
    CHECK(rdof_sig_names_json(rdof_sdr_modes, true, names, sizeof names) && strstr(names, "\"fm\""), "names %s", names);

    static rdof_jnode_t nodes[256];
    static char buf[2048];
    rdof_jdoc_t d = {nodes, 256, 0, buf, sizeof buf, 0, ""};
    const char *job = "{\"mode\":\"fm\",\"dev_hz\":2500,\"ctcss\":100.0,\"sub_dev_hz\":500,\"lead_s\":0,"
                      "\"pcm\":{\"rate\":8000,\"samples\":8000},"
                      "\"program\":[{\"kind\":\"tone\",\"hz\":1000,\"s\":1,\"level\":0.8},{\"kind\":\"pcm\",\"s\":1}]}";
    int root = rdof_jparse(&d, job, strlen(job));
    CHECK(root >= 0, "the job parses");
    char err[160] = "";
    double secs = 0;
    void *tx = m->tx_new(&d, root, FS, &secs, err, sizeof err);
    CHECK(tx != NULL, "tx_new: %s", err);
    if (!tx)
        return;
    NEAR(secs, 2.4, 0.05, "keyed seconds (1 + 1 + tail and the reverse burst)");
    /* the PCM: 1 s of 500 Hz, given in odd-sized pieces, with backpressure */
    CHECK(m->tx_in_want(tx) == 16000, "wants 16000 bytes");
    static uint8_t pcm[16000];
    for (int i = 0; i < 8000; i++) {
        const int16_t v = (int16_t)lrint(16000 * sin(2 * M_PI * 500 * i / 8000.0));
        pcm[2 * i] = (uint8_t)v, pcm[2 * i + 1] = (uint8_t)((uint16_t)v >> 8);
    }
    size_t at = 0;
    while (at < sizeof pcm) {
        size_t k = m->tx_in_room(tx);
        k = k > 333 ? 333 : k;
        CHECK(k > 0, "room for the PCM");
        if (!k)
            break;
        at += m->tx_in(tx, pcm + at, k);
    }
    CHECK(m->tx_in_want(tx) == 0, "all of it taken");
    rdof_sig_src_t src = m->tx_src(tx);
    NEAR(src.peak_hz, 2500 + 500, 1, "the source's peak");

    const char *rq = "{\"mode\":\"fm\",\"streams\":\"audio,disc\",\"spectrum_hz\":1500}";
    root = rdof_jparse(&d, rq, strlen(rq));
    void *rx = m->rx_new(&d, root, FS, -40, err, sizeof err); /* the noise is at -47 dBFS */
    CHECK(rx != NULL, "rx_new: %s", err);
    rdof_sig_stream_t st[RDOF_SIG_STREAMS];
    const int ns = m->rx_streams(rx, st, RDOF_SIG_STREAMS);
    CHECK(ns == 2 && !strcmp(st[0].name, "audio") && !strcmp(st[1].name, "disc"), "two streams");
    enum { B = 480 };
    float hz[B], env[B], iq[2 * B], a[B], dsc[B];
    float *outs[2] = {a, dsc};
    int nout[2];
    double ph = 0, pw = 0;
    long audio = 0, disc = 0, recs = 0;
    for (int blk = 0; blk < 400; blk++) { /* 4 s: the key and quiet after */
        const int got = src.freq(src.ctx, hz, env, B);
        for (int i = 0; i < B; i++) {
            const float e = i < got ? env[i] : 0;
            ph += 2 * M_PI * (i < got ? hz[i] : 0) / FS;
            iq[2 * i] = (float)(e * cos(ph) + 0.003 * gauss()), iq[2 * i + 1] = (float)(e * sin(ph) + 0.003 * gauss());
        }
        m->rx_feed(rx, iq, B, outs, nout);
        audio += nout[0], disc += nout[1];
        for (int i = 0; i < nout[0]; i++) {
            pw = fmax(pw, fabs(a[i]));
        }
        char rec[1024];
        while (m->rx_record(rx, rec, sizeof rec)) {
            recs++;
            CHECK(!strncmp(rec, "{\"block\":{", 10), "a block record");
        }
    }
    CHECK(src.done(src.ctx), "the source is done");
    CHECK(disc == 400 * B, "the discriminator: every sample");
    NEAR((double)audio / disc, st[0].rate / FS, 0.01, "audio at its rate");
    CHECK(pw > 1000 && pw < st[0].full, "audio within its full scale, no click at key-off (%.0f)", pw);
    CHECK(recs >= 35, "records %ld", recs);
    static char sum[8192];
    const size_t n = m->rx_summary(rx, sum, sizeof sum);
    CHECK(n > 0 && strstr(sum, "\"ctcss\"") && strstr(sum, "100.0"), "the summary names the tone: %.300s", sum);
    char rep[256];
    CHECK(m->tx_report(tx, rep, sizeof rep) && strstr(rep, "\"underruns\":0"), "report %s", rep);
    m->rx_free(rx);
    m->tx_free(tx);

    /* refusals name the problem */
    const char *bad[][2] = {{"{\"program\":[{\"kind\":\"chirp\"}]}", "chirp"},
                            {"{\"ctcss\":100,\"dcs\":\"023N\",\"program\":[{\"kind\":\"tone\",\"hz\":1000}]}", "not both"},
                            {"{\"dcs\":\"089N\",\"program\":[{\"kind\":\"tone\",\"hz\":1000}]}", "octal"},
                            {"{\"program\":[{\"kind\":\"pcm\"}]}", "pcm"},
                            {"{\"program\":[{\"kind\":\"fsk\"}]}", "hex or text"}};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        root = rdof_jparse(&d, bad[i][0], strlen(bad[i][0]));
        err[0] = 0;
        CHECK(!m->tx_new(&d, root, FS, &secs, err, sizeof err) && strstr(err, bad[i][1]), "refused (%s): %s", bad[i][1],
              err);
    }
    /* AFSK: Bell 202 by default */
    const char *afsk = "{\"program\":[{\"kind\":\"fsk\",\"text\":\"hi\"}]}";
    root = rdof_jparse(&d, afsk, strlen(afsk));
    tx = m->tx_new(&d, root, FS, &secs, err, sizeof err);
    CHECK(tx != NULL, "AFSK: %s", err);
    m->tx_free(tx);
}

int main(void)
{
    t_dcs_words();
    t_generator();
    t_tone_ctcss();
    t_ctcss_tail();
    t_ctcss_tones();
    t_dcs();
    t_afsk_dtmf_sweep();
    t_pcm();
    t_mode();
    printf("fm: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
