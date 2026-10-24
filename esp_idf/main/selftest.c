// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The on-board self-test, with no dongle needed: the embedded 0.6 s clip
 * (five busy channels at 288 kS/s, selftest/clip.h) through the
 * receiver. It checks the output against the expected (message count and
 * CRC per channel), on one core and split over two, then times each stage
 * (rdof_sdr_bench) and the FFT and filter choices, and says whether one core
 * keeps up. Run at boot with "selftest": true, or from the web page.
 *
 * And the USB throughput spike (rtl_usb_spike), run from the DSP task once
 * the dongle is open, with "spike": true or from the web page.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rdof/sdr/bench.h"
#include "rdof/sdr/fft.h"
#include "rdof/sdr/fm.h"
#include "rdof/sdr/sdrmodem.h"
#include "rdof/layer2.h"
#include "cJSON.h"
#include "rdof/jsonp.h"
#include "rtlsdr/rtl_usb.h"
#include "selftest/clip.h"
#include "selftest_core.h"
#include "sdr_prof.h"
#include "esp_cpu.h"

static const char *TAG = "selftest";

#if CONFIG_RDOF_SDR_SELFTEST

extern const uint8_t clip_start[] asm("_binary_clip_cu8_start");
extern const uint8_t clip_end[] asm("_binary_clip_cu8_end");

#ifdef CONFIG_RDOF_SDR_DSP_IRAM
#define DSP_IRAM 1
#else
#define DSP_IRAM 0
#endif

static double clock_s(void) { return esp_timer_get_time() * 1e-6; }

/*
 * The load test: nch channels, every one busy (a repeater's downlink: a voice LC header in every slot, with its
 * CACH), at a sample rate the RTL-SDR can run, through the receiver on one core and then on two (the balanced
 * split, as dsp_cores 2 runs it). The signal is made as it goes and isn't timed. Settings:
 * "selftest_load": {"channels": 8, "rates": [288000, 960000]} (those are the defaults: 8, [288000]).
 */
#define LOAD_SECONDS 2.0

/* What sdr_process_chunk measured since sdr_prof_reset, in Mcycles a second of IQ at rate (so 240 is a whole
 * core), and where rx's buffers are (NULL: not shown). */
static void print_prof(const char *what, double rate, const rdof_sdr_rx_t *rx)
{
    sdr_prof_t p;
    sdr_prof_get(&p);
    if (!p.chunks)
        return;
    const double sec = p.samples / rate, mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, M = 1e6 * sec;
    const rdof_demod_prof_t d = rdof_demod_prof;
    const double wall = p.wall_us * mhz / M, busy1 = (p.fwd + p.chz[0] + p.demod[0] + p.finish) / M;
    ESP_LOGI(TAG, "prof %s: Mcycles per s of IQ: core 1: fwd %.1f, chz %.1f, demod %.1f, finish %.1f (%.1f); "
             "core 0: chz %.1f, demod %.1f (%.1f); demod steps fm %.1f search %.1f fly %.1f compact %.1f",
             what, p.fwd / M, p.chz[0] / M, p.demod[0] / M, p.finish / M, busy1, p.chz[1] / M, p.demod[1] / M,
             (p.chz[1] + p.demod[1]) / M, d.fm / M, d.search / M, d.fly / M, d.compact / M);
    ESP_LOGI(TAG, "prof %s: search steps squelch+prefilter+score %.1f syncs %.1f (flywheel %.1f, at sync %.1f); per s of "
             "IQ: %.0f positions, %.0f scored, %.0f syncs tried", what, d.pre / M, d.sync / M, d.sync_fly / M,
             d.at_sync / M, d.positions / sec,
             d.scored / sec, d.syncs / sec);
    ESP_LOGI(TAG, "prof %s: wall %.1f (%.0f%% of a core), core 1 busy %.1f + join wait %.1f, helper wake %.0f us a "
             "chunk, %u chunks of %lld samples, split %d",
             what, wall, 100 * wall / mhz, busy1, p.join_us * mhz / M, p.chunks ? (double)p.wake_us / p.chunks : 0,
             (unsigned)p.chunks, (long long)(p.samples / p.chunks), p.split);
    const rdof_chz_prof_t z = rdof_chz_prof;
    ESP_LOGI(TAG, "prof %s: forward step: FFT %.1f (%.0f cycles a block), copy and window move %.1f, noise floor %.1f; "
             "internal RAM free %u KB", what, z.fft / M, z.blocks ? (double)z.fft / z.blocks : 0, z.copy / M,
             z.noise / M, (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    if (rx) {
        char where[320];
        sdr_prof_placement(rx, where, sizeof where);
        ESP_LOGI(TAG, "prof %s: buffers: %s", what, where);
    }
}

static void prof_start(void)
{
    rdof_demod_set_clock(esp_cpu_get_cycle_count);
    memset(&rdof_demod_prof, 0, sizeof rdof_demod_prof);
    rdof_chz_set_clock(esp_cpu_get_cycle_count);
    memset(&rdof_chz_prof, 0, sizeof rdof_chz_prof);
    sdr_prof_reset();
}

static double load_run(int nch, int rate, const double *off, int split, const uint8_t burst[33], int *split_used)
{
    const double center = 461e6;
    rdof_sdr_channel_t ch[RDOF_SDR_MAX_CHANNELS];
    rdof_mod_t *mods = heap_caps_calloc(nch, sizeof *mods, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    rdof_cf_t *iq = heap_caps_malloc(sizeof(rdof_cf_t) * CLIP_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    rdof_cf_t *one = heap_caps_malloc(sizeof(rdof_cf_t) * CLIP_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    rdof_sdr_rx_t rx = {0};
    int64_t *next = heap_caps_calloc(nch, sizeof *next, MALLOC_CAP_8BIT);
    double busy = -1;
    char ids[RDOF_SDR_MAX_CHANNELS][8];
    for (int c = 0; c < nch; c++) {
        snprintf(ids[c], sizeof ids[c], "L%d", c);
        ch[c] = (rdof_sdr_channel_t){ids[c], center + off[c], 1, 1};
    }
    const rdof_sdr_rx_config_t cfg = {rate, center, CLIP_RSSI_OFFSET, 0.0, true, CLIP_CHUNK};
    bool ok = mods && iq && one && next && rdof_sdr_rx_init(&rx, &cfg, ch, nch, NULL, 0) == 0;
    for (int c = 0; ok && c < nch; c++)
        ok = rdof_mod_init(&mods[c], rate, off[c], 4.0, 0.5f / nch, 4, 0) == 0;
    if (ok) {
        uint8_t bits[24], cach[2][3] = {{0}};
        for (int s = 0; s < 2; s++) {
            rdof_encode_cach(s + 1, 1, 0, bits);
            for (int i = 0; i < 24; i++)
                cach[s][i / 8] |= (uint8_t)((bits[i] & 1) << (7 - i % 8));
        }
        const int64_t slot = rdof_mod_slot_samples(&mods[0]), lead = rdof_mod_samples(&mods[0], 24);
        const int64_t total = (int64_t)(LOAD_SECONDS * rate);
        int64_t spent = 0;
        prof_start();
        for (int64_t pos = 0; pos < total; pos += CLIP_CHUNK) {
            memset(iq, 0, sizeof(rdof_cf_t) * CLIP_CHUNK);
            for (int c = 0; c < nch; c++) {
                for (;;) { /* keep a slot or two queued */
                    const int64_t k = next[c], at = lead + k * slot + rdof_mod_samples(&mods[c], 12);
                    if (at > pos + CLIP_CHUNK + slot ||
                        rdof_mod_queue_burst(&mods[c], at, burst, cach[k & 1], k ? 0 : RDOF_MOD_RAMP_UP) != 0)
                        break;
                    next[c]++;
                }
                rdof_mod_iq(&mods[c], one, CLIP_CHUNK);
                for (int i = 0; i < CLIP_CHUNK; i++) {
                    iq[i].re += one[i].re;
                    iq[i].im += one[i].im;
                }
            }
            int64_t t = esp_timer_get_time();
            sdr_process_chunk(&rx, iq, CLIP_CHUNK, split);
            spent += esp_timer_get_time() - t;
        }
        busy = spent / 1e6 / LOAD_SECONDS;
        char what[48];
        snprintf(what, sizeof what, "%d ch at %d k, %s", nch, rate / 1000,
                 !split ? "one core" : split == SDR_SPLIT_AUTO ? "two cores" : "two cores taking turns");
        print_prof(what, rate, &rx);
        *split_used = split == SDR_SPLIT_AUTO ? g_sstat.dsp_split : split;
    }
    for (int c = 0; mods && c < nch; c++)
        rdof_mod_free(&mods[c]);
    rdof_sdr_rx_free(&rx);
    free(mods);
    free(iq);
    free(one);
    free(next);
    return busy;
}

static void load_test(void)
{
    int nch = 8, rates[4] = {288000}, nr = 1;
    /* The stored settings are JSON with comments (and maybe trailing commas): strip them first, as the settings'
     * own parser does, or cJSON gives up and the defaults would run without a word. "selftest_load" at the top,
     * or in the SDR's section of radios. */
    char *text = settings_text();
    size_t tl = text ? strlen(text) : 0;
    char *clean = text ? malloc(tl + 1) : NULL;
    cJSON *root = NULL, *o = NULL;
    if (clean) {
        clean[rdof_jsonc_strip(text, tl, clean)] = 0;
        root = cJSON_Parse(clean);
    }
    if (!root)
        ESP_LOGW(TAG, "load: the stored settings didn't parse here: the defaults run");
    o = cJSON_GetObjectItem(root, "selftest_load");
    const cJSON *radios = cJSON_GetObjectItem(root, "radios"), *e;
    cJSON_ArrayForEach(e, radios) if (!o && cJSON_IsObject(e) && !cJSON_IsFalse(cJSON_GetObjectItem(e, "enabled")))
        o = cJSON_GetObjectItem(e, "selftest_load");
    if (o && !cJSON_IsObject(o))
        ESP_LOGW(TAG, "load: selftest_load is an object, {\"channels\": 8, \"rates\": [288000]}: the defaults run");
    if (cJSON_IsObject(o)) {
        const cJSON *c = cJSON_GetObjectItem(o, "channels"), *r = cJSON_GetObjectItem(o, "rates");
        if (cJSON_IsNumber(c))
            nch = c->valueint;
        if (cJSON_IsNumber(r)) {
            rates[0] = r->valueint;
        } else if (cJSON_IsArray(r)) {
            nr = 0;
            cJSON_ArrayForEach(e, r)
            {
                if (cJSON_IsNumber(e) && nr < 4)
                    rates[nr++] = e->valueint;
                else
                    ESP_LOGW(TAG, "load: selftest_load.rates: up to 4 numbers (S/s); one left out");
            }
        } else if (r) {
            ESP_LOGW(TAG, "load: selftest_load.rates: a list of sample rates (S/s)");
        }
    }
    cJSON_Delete(root);
    free(clean);
    free(text);
    char list[64] = "";
    for (int k = 0, m = 0; k < nr; k++)
        m += snprintf(list + m, sizeof list - m, "%s%d", k ? ", " : "", rates[k]);
    ESP_LOGI(TAG, "load: %d channels at %s S/s (%s)", nch, list, o ? "selftest_load" : "the defaults");
    if (nch < 1 || nch > RDOF_SDR_MAX_CHANNELS) {
        ESP_LOGW(TAG, "load: selftest_load.channels %d: 1 to %d", nch, RDOF_SDR_MAX_CHANNELS);
        return;
    }
    uint8_t lc9[9], burst[33];
    rdof_group_lc(9990003, 91, true, 0, lc9);
    rdof_lc_burst(lc9, 1, 1, rdof_data_sync(rdof_family_voice_sync(RDOF_FAMILY_BS)), burst);
    for (int k = 0; k < nr; k++) {
        const int rate = rates[k];
        double spacing = 25000, off[RDOF_SDR_MAX_CHANNELS];
        if (nch * spacing > 0.75 * rate)
            spacing = 12500;
        if (rate % 48000 || rate < 96000 || nch * spacing > 0.75 * rate) {
            ESP_LOGW(TAG, "load: %d channels at %d S/s: not a rate here (a multiple of 48000), or they don't fit",
                     nch, rate);
            continue;
        }
        for (int c = 0; c < nch; c++)
            off[c] = (c - (nch - 1) / 2.0) * spacing + spacing / 2; /* off the center's DC */
        int s1, s2 = -1;
        double one = load_run(nch, rate, off, 0, burst, &s1), two = -1;
#if CONFIG_RDOF_SDR_TWO_CORES
        two = load_run(nch, rate, off, SDR_SPLIT_AUTO, burst, &s2);
        if (two >= 0 && s2 > 0 && s2 < nch) { /* the same split with the cores taking turns: the cost of sharing */
            int s3;
            sdr_prof_serial(true);
            load_run(nch, rate, off, s2, burst, &s3);
            sdr_prof_serial(false);
        }
#endif
        if (one < 0 || (CONFIG_RDOF_SDR_TWO_CORES && two < 0)) {
            ESP_LOGW(TAG, "load: %d channels at %d S/s didn't run: the receiver or the signals didn't start (out of "
                     "memory? %u KB internal, %u KB PSRAM free)", nch, rate,
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
            continue;
        }
        ESP_LOGI(TAG, "load: %d busy channels at %d kS/s: %.0f%% of one core on one; %.0f%% of the wall clock on "
                 "two (%d of the %d demodded on core 1 after balancing)",
                 nch, rate / 1000, 100 * one, 100 * two, s2, nch);
    }
}

static void step(rdof_sdr_rx_t *rx, const rdof_cf_t *iq, int n, int split) { sdr_process_chunk(rx, iq, n, split); }

/* Seconds for n calls of an FFT of size points, from the backend in use (esp-dsp for powers of two) or not. */
static double time_fft(int points, bool builtin)
{
    rdof_cf_t *x = heap_caps_calloc(points, sizeof(rdof_cf_t), MALLOC_CAP_INTERNAL);
    rdof_cf_t *y = heap_caps_calloc(points, sizeof(rdof_cf_t), MALLOC_CAP_INTERNAL);
    rdof_fft_t *f;
    if (builtin)
        dsp_esp_fft_enable(false);
    f = rdof_fft_new(points, false);
    double t = clock_s();
    for (int i = 0; i < 200; i++)
        rdof_fft_run(f, x, y);
    t = (clock_s() - t) / 200;
    rdof_fft_free(f);
    if (builtin)
        dsp_esp_fft_enable(true); /* the backend back, if it passed its check */
    free(x);
    free(y);
    return t;
}

void selftest_run(void)
{
    const size_t len = (size_t)(clip_end - clip_start);
    const double mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    const double clip_s = (len / 2) / CLIP_RATE;
    ESP_LOGI(TAG, "self-test: %.2f s clip, %d channels at %.0f kS/s, CPU %.0f MHz, esp-dsp FFT %s, FIR %s; DSP code "
             "in %s, data cache %d KB", clip_s, CLIP_CHANNELS, CLIP_RATE / 1e3, mhz, g_esp_fft ? "on" : "off",
             g_esp_fir ? "on" : "off", DSP_IRAM ? "IRAM" : "flash", CONFIG_ESP32S3_DATA_CACHE_SIZE / 1024);

    /* 1. the output, against the expected */
    sdr_check_t c1, c2 = {0};
    sdr_selftest_step = step;
    prof_start();
    int64_t t = esp_timer_get_time();
    sdr_selftest_check(clip_start, len, 0, &c1);
    double one = (esp_timer_get_time() - t) * 1e-6;
    print_prof("clip, one core", CLIP_RATE, NULL);
    sdr_prof_t pr1, pr2 = {0};
    sdr_prof_get(&pr1);
    double two = 0;
#if CONFIG_RDOF_SDR_TWO_CORES
    prof_start();
    t = esp_timer_get_time();
    sdr_selftest_check(clip_start, len, SDR_SPLIT_AUTO, &c2);
    two = (esp_timer_get_time() - t) * 1e-6;
    print_prof("clip, two cores", CLIP_RATE, NULL);
    sdr_prof_get(&pr2);
#endif
    ESP_LOGI(TAG, "output, one core:  %s: %d messages (expected %d), CRC %08lx (expected %08lx), %ld bursts (expected %ld)",
             c1.pass ? "PASS" : "FAIL", c1.messages, c1.expect_messages, (unsigned long)c1.crc,
             (unsigned long)c1.expect_crc, c1.bursts, c1.expect_bursts);
#if CONFIG_RDOF_SDR_TWO_CORES
    ESP_LOGI(TAG, "output, two cores: %s: %d messages, CRC %08lx, %ld bursts", c2.pass ? "PASS" : "FAIL", c2.messages,
             (unsigned long)c2.crc, c2.bursts);
#endif
    /* The check's own time includes the receiver's setup (filter design in software double, allocation), the
     * clip's cu8 conversion and the CRC sinks: the receiver's chunks alone are the prof lines' wall. */
    ESP_LOGI(TAG, "whole check: %.0f%% of one core on one core, %.0f%% of the wall clock on two (%.2f s of IQ in "
             "%.2f s / %.2f s); the receiver's chunks alone %.0f%% / %.0f%%, the rest (setup, cu8, the check's sinks) "
             "%.2f s / %.2f s",
             100 * one / clip_s, 100 * two / clip_s, clip_s, one, two, 100 * pr1.wall_us * 1e-6 / clip_s,
             100 * pr2.wall_us * 1e-6 / clip_s, one - pr1.wall_us * 1e-6, two - pr2.wall_us * 1e-6);
    /* the clip with a fault in it: this board's own DSP (int16 filter, FFT) under offset, noise and overload */
    for (int v = 0; v < CLIP_VARIANTS; v++) {
        sdr_check_t cv;
        sdr_selftest_check_variant(clip_start, len, v, &cv);
        ESP_LOGI(TAG, "output with %-6s %s: %d messages (expected %d), %ld bursts (expected %ld), CRC %s", cv.name,
                 cv.pass ? "PASS" : "FAIL", cv.messages, cv.expect_messages, cv.bursts, cv.expect_bursts,
                 cv.crc == cv.expect_crc ? "as expected" : "differs (bits in noise)");
        if (!cv.pass)
            c1.pass = false;
    }

    /* 2. per stage */
    rdof_sdr_channel_t ch[CLIP_CHANNELS];
    for (int i = 0; i < CLIP_CHANNELS; i++) {
        ch[i].id = clip_channels[i].id;
        ch[i].freq_hz = clip_channels[i].freq;
        ch[i].cc = clip_channels[i].cc;
        ch[i].slot = clip_channels[i].slot;
    }
    rdof_sdr_rx_config_t cfg = {CLIP_RATE, CLIP_CENTER, CLIP_RSSI_OFFSET, 0.0, true, CLIP_CHUNK};
    rdof_sdr_bench_t b;
    double total = 0;
    if (rdof_sdr_bench(clip_start, (int)(len / 2), &cfg, ch, CLIP_CHANNELS, clock_s, 2, &b) == 0) {
        ESP_LOGI(TAG, "%-46s %14s", "stage (every channel busy)", "Mcycles per s");
        for (int i = 0; i < RDOF_BENCH_STAGES; i++) {
            double m = mhz * b.seconds[i] / b.clip_seconds;
            total += m;
            ESP_LOGI(TAG, "%-46s %14.1f", rdof_bench_stage_names[i], m);
        }
        ESP_LOGI(TAG, "%-46s %14.1f", "total", total);
        ESP_LOGI(TAG, "%-46s %14.1f", "(search without the squelch)", mhz * b.search_no_squelch / b.clip_seconds);
    } else {
        ESP_LOGE(TAG, "bench failed (out of memory?)");
    }

    /* 3. the FFT choices: built-in 384 points (288 kS/s) against esp-dsp at 512 (384 kS/s would need it) */
    double f384 = time_fft(384, true), f384e = time_fft(384, false), f512b = time_fft(512, true),
           f512 = time_fft(512, false);
    double f64b = time_fft(64, true), f64 = time_fft(64, false);
    ESP_LOGI(TAG, "FFT per call: 1280 (960 kS/s) built-in %.0f cycles, 5 x 256 radix-4 %.0f",
             time_fft(1280, true) * mhz * 1e6, time_fft(1280, false) * mhz * 1e6);
    ESP_LOGI(TAG, "FFT per call: 384 built-in %.0f cycles, 6 x 64 radix-4 %.0f; 512 built-in %.0f, esp-dsp %.0f; "
             "64 built-in %.0f, esp-dsp %.0f",
             f384 * mhz * 1e6, f384e * mhz * 1e6, f512b * mhz * 1e6, f512 * mhz * 1e6, f64b * mhz * 1e6,
             f64 * mhz * 1e6);
    ESP_LOGI(TAG, "  per second: 288 kS/s needs 1000 x 384 = %.1f Mcycles; 384 kS/s would need 1000 x 512 = %.1f "
             "(esp-dsp)",
             1000 * f384 * mhz, 1000 * f512 * mhz);
    kbench_run();

    /* 4. the load test: N busy channels at the rates asked for, one core and two */
    load_test();

    /* 5. transmit: the SDR modem's half and full duplex loopbacks (modulator -> receiver -> modem) */
    const char *txv = "-";
#if RDOF_FEATURE_TX && RDOF_FEATURE_SDR_RECEIVER
    {
        char rep[256];
        int fails = 0;
        for (int full = 0; full < 2; full++) {
            t = esp_timer_get_time();
            int f = rdof_sdrmodem_loopback(full, rep, sizeof rep);
            ESP_LOGI(TAG, "transmit: %s: %s (%.2f s)", f ? "FAIL" : "PASS", rep, (esp_timer_get_time() - t) * 1e-6);
            fails += f;
        }
        txv = fails ? "FAIL" : "pass";
    }
#endif

    /* 6. the verdict */
    const double core = mhz;
    const char *verdict = total < 0.7 * core   ? "one core keeps up"
                          : total < 0.95 * core ? "one core is tight: set \"dsp_cores\": 2"
                                                : "one core can't keep up: set \"dsp_cores\": 2, or fewer channels";
    snprintf(g_sstat.selftest, sizeof g_sstat.selftest,
             "output %s / %s, transmit %s; %.0f Mcycles/s of %.0f on one core (%.0f%%): %s", c1.pass ? "pass" : "FAIL",
             c2.pass ? "pass" : (CONFIG_RDOF_SDR_TWO_CORES ? "FAIL" : "-"), txv, total, core, 100 * total / core,
             verdict);
    ESP_LOGI(TAG, "self-test: %s", g_sstat.selftest);
}

#else

void selftest_run(void)
{
    snprintf(g_sstat.selftest, sizeof g_sstat.selftest, "not in this build (CONFIG_RDOF_SDR_SELFTEST)");
}

#endif

#if CONFIG_RDOF_SDR_SPIKE

void spike_run(void)
{
    /* 300000 is the top of the RTL's low range (225001 to 300000); 960000, in its high range, asks for 1.92 MB/s,
       more than Full Speed carries, so what arrives there is the USB host's ceiling. */
    static const uint32_t rates[] = {288000, 250000, 240000, 300000, 960000};
    static const int plans[][2] = {{8, 8192}, {4, 16384}, {16, 4096}};
    enum { RATES = sizeof rates / sizeof rates[0], PLANS = sizeof plans / sizeof plans[0] };
    rtl_spike_result_t r[RATES * PLANS];
    ESP_LOGI(TAG, "USB throughput spike: %d rates x %d transfer plans, 5 s each (about a minute and a half)", RATES,
             PLANS);
    int n = rtl_usb_spike(rates, RATES, plans, PLANS, 461162500, 5, r, RATES * PLANS);
    size_t o = 0;
    for (int i = 0; i < n && o < sizeof g_sstat.spike; i++)
        o += (size_t)snprintf(g_sstat.spike + o, sizeof g_sstat.spike - o, "%lu:%dx%d %.1f%%; ",
                              (unsigned long)r[i].rate, r[i].transfers, r[i].transfer_bytes, r[i].percent);
    double best = 0;
    for (int i = 0; i < n; i++)
        if (r[i].rate == 288000 && r[i].percent > best)
            best = r[i].percent;
    double ceiling = 0;
    for (int i = 0; i < n; i++)
        if (r[i].bytes_per_s > ceiling)
            ceiling = r[i].bytes_per_s;
    ESP_LOGI(TAG, "spike done: 288 kS/s at best %.1f%% of the samples (%s); the most that arrived %.1f KB/s", best,
             best >= 99.5 ? "Full Speed keeps up" : "SAMPLES LOST at 288 kS/s: try 240000", ceiling / 1e3);
}

#else

void spike_run(void) {}

#endif
