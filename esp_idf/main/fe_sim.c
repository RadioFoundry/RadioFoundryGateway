// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The simulated front end ("sim": true, or device.type sim): no dongle, the
 * self-test clip over and over, a chunk each chunk / rate seconds. A chunk late
 * by more than half a second is skipped and counted, as the USB ring would drop
 * it. With sim_ppm, a tuner that many ppm fast, corrected by whatever whole ppm
 * the ppm learning sets, so learn_ppm can be watched at work: its signals are
 * that far off, and its samples come that much fast in true time (the board's
 * own crystal error, from netclock, taken out of the pacing), so timing the
 * sample clock (rdof/sdr/rate.h) finds it too.
 *
 * It "transmits" too (full duplex, I/Q at the receive rate): what an
 * sdr_transceiver sends is mixed into the samples it reads, a quarter of full
 * scale, so the receiver hears it as a station near the board would sound.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "netclock.h"
#include "sdr_fe.h"
#if CONFIG_RDOF_SDR_SELFTEST
#include "selftest/clip.h"
#endif

static const char *TAG = "sdr";

static const rdof_fe_caps_t caps = {
    .kind = "sim",
    .format = RDOF_FE_CU8,
    .rate_min = 48000,
    .rate_max = 3200000,
    .freq_min = 0,
    .freq_max = 6e9,
    .usable = 0.8,
    .dc_guard_hz = 25e3,
    .duplex = RDOF_FE_FULL_DUPLEX,
    .tx_mode = RDOF_FE_TX_IQ,
    .tx_format = RDOF_FE_CS16,
    .tx_rate = 0,
    .tx_freq_min = 0,
    .tx_freq_max = 6e9,
    .tx_power_min = -40,
    .tx_power_max = 20,
    .tx_power_step = 1,
};

#if CONFIG_RDOF_SDR_SELFTEST

extern const uint8_t clip_start[] asm("_binary_clip_cu8_start");
extern const uint8_t clip_end[] asm("_binary_clip_cu8_end");

static struct {
    double rate, center;
    double chip;  /* the simulated tuner's whole-ppm correction */
    double phase;
    size_t pos;
    long sent;
    double due;     /* local us the next chunk is due */
    rdof_fe_stats_t stats;
    /* the loopback */
    const rdof_fe_tx_source_t *volatile tx;
    int64_t n;      /* samples posted since boot: the receiver's index */
    rdof_cf_t *txbuf;
    int txcap;
} st;

static int init(void *ctx) { return 0; }
static bool wait(void *ctx, int timeout_ms) { return true; }

static int tune(void *ctx, double rate, double center_hz, int ppm, double *got_rate)
{
    const size_t len = (size_t)(clip_end - clip_start) & ~(size_t)1;
    st.rate = rate;
    st.center = center_hz;
    st.chip = ppm;
    *got_rate = rate;
    snprintf(g_sstat.rtl_desc, sizeof g_sstat.rtl_desc, "simulated: the self-test clip (%.1f s, %d channels) in a loop",
             len / 2 / CLIP_RATE, CLIP_CHANNELS);
    g_sstat.rtl_open = true;
    if (rate != CLIP_RATE || fabs(center_hz - CLIP_CENTER) > 1)
        ESP_LOGW(TAG, "sim: the clip is %.0f S/s around %.5f MHz, the settings %.0f S/s around %.5f MHz: nothing "
                 "will decode (use the clip's channels: 13, 4, 6, 8, 10 at 461 MHz)",
                 CLIP_RATE, CLIP_CENTER / 1e6, rate, center_hz / 1e6);
    ESP_LOGI(TAG, "sim: %s, %d-sample chunks every %.1f ms", g_sstat.rtl_desc, CONFIG_RDOF_SDR_CHUNK,
             1e3 * CONFIG_RDOF_SDR_CHUNK / rate);
    return 0;
}

static int set_ppm(void *ctx, int ppm)
{
    st.chip = ppm;
    return 0;
}

static int start(void *ctx)
{
    if (g_scfg.sim_ppm)
        ESP_LOGI(TAG, "sim: the simulated tuner is %.2f ppm fast", g_scfg.sim_ppm);
    st.pos = 0;
    st.sent = 0;
    st.due = (double)esp_timer_get_time();
    return 0;
}

static void stop(void *ctx) {}

static size_t read(void *ctx, uint8_t *out, size_t want, int timeout_ms, size_t *filled)
{
    const size_t len = (size_t)(clip_end - clip_start) & ~(size_t)1;
    /* a true second is 1 + board ppm local ones; the simulated crystal sends (sim_ppm - chip) ppm fast in it */
    double board = 0, skew;
    bool learned;
    if (!netclock_rate(&board, &skew, &learned))
        board = 0;
    const double chunk_us = 1e6 * (double)(want / 2) / (st.rate * (1 + (g_scfg.sim_ppm - st.chip) * 1e-6)) /
                            (1 + board * 1e-6);
    st.due += chunk_us;
    int64_t due = (int64_t)st.due, now = esp_timer_get_time();
    if (due > now + 1000) {
        vTaskDelay(pdMS_TO_TICKS((due - now) / 1000));
    } else if (now - due > 500000) {
        long behind = (long)((now - due) / chunk_us);
        st.stats.overflows += (uint32_t)behind;
        st.sent += behind;
        st.due += behind * chunk_us;
        st.pos = (st.pos + (size_t)behind * want) % len;
        ESP_LOGW(TAG, "sim: %ld chunks behind: dropped", behind);
    }
    for (size_t k = 0; k < want;) {
        size_t m = len - st.pos < want - k ? len - st.pos : want - k;
        memcpy(out + k, clip_start + st.pos, m);
        k += m;
        st.pos = (st.pos + m) % len;
    }
    st.stats.bytes += want;
    st.sent++;
    /* stamped when the chunk was due, as a dongle's USB would deliver it: read late on a busy board, the read time
     * would look like lost samples and start the sample clock's fit over */
    sdr_rate_block((int64_t)st.due, (uint64_t)st.sent * (want / 2));
    *filled = 0;
    return want;
}

static bool gone(void *ctx) { return false; }
static void stats(void *ctx, rdof_fe_stats_t *out) { *out = st.stats; }
static const char *identity(void *ctx) { return "sim"; }
static const char *error(void *ctx) { return ""; }
static double sample_ppm(void *ctx) { return st.chip; }

static int tx_set(void *ctx, const rdof_fe_tx_source_t *src, double power_dbm)
{
    st.tx = src;
    return 0;
}

/* What's transmitted, into the samples read: n of them from the receiver's index st.n. */
static void loopback(rdof_cf_t *x, int n)
{
    const rdof_fe_tx_source_t *src = st.tx;
    if (src && n > st.txcap) {
        free(st.txbuf);
        st.txbuf = heap_caps_malloc(sizeof(rdof_cf_t) * n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        st.txcap = st.txbuf ? n : 0;
    }
    if (src && st.txcap && src->pull_iq(src->ctx, st.n, st.txbuf, n))
        for (int i = 0; i < n; i++) {
            x[i].re += 0.25f * st.txbuf[i].re;
            x[i].im += 0.25f * st.txbuf[i].im;
        }
    st.n += n;
}

/* The simulated tuner, sim_ppm fast and corrected by chip, puts every signal f * (sim_ppm - chip) * 1e-6 low:
 * one rotation of the whole span (its channels are within 0.03% of the center). Then the loopback. */
static void post(void *ctx, rdof_cf_t *x, int n)
{
    const double off = -st.center * (g_scfg.sim_ppm - st.chip) * 1e-6;
    if (off == 0.0) {
        loopback(x, n);
        return;
    }
    const double step = 2.0 * M_PI * off / st.rate;
    rdof_cf_t r = {cosf((float)st.phase), sinf((float)st.phase)};
    const rdof_cf_t s = {cosf((float)step), sinf((float)step)};
    for (int i = 0; i < n; i++) {
        const rdof_cf_t v = x[i];
        x[i].re = v.re * r.re - v.im * r.im;
        x[i].im = v.re * r.im + v.im * r.re;
        const float re = r.re * s.re - r.im * s.im;
        r.im = r.re * s.im + r.im * s.re;
        r.re = re;
    }
    st.phase = fmod(st.phase + step * n, 2.0 * M_PI);
    loopback(x, n);
}

static const rdof_fe_ops_t ops = {
    .init = init,
    .wait = wait,
    .tune = tune,
    .set_ppm = set_ppm,
    .start = start,
    .stop = stop,
    .read = read,
    .gone = gone,
    .stats = stats,
    .identity = identity,
    .error = error,
    .sample_ppm = sample_ppm,
    .post = post,
    .tx_set = tx_set,
};

#else

static int init(void *ctx) { return -1; }
static const char *error(void *ctx) { return "sim needs the self-test clip (CONFIG_RDOF_SDR_SELFTEST)"; }
static const rdof_fe_ops_t ops = {.init = init, .error = error};

#endif

void fe_sim(rdof_fe_t *fe) { *fe = (rdof_fe_t){&ops, &caps, NULL}; }
