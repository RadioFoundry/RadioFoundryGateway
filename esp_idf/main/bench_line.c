// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The bench's line protocol (docs/spec/bench-api.md section 14) on TCP CONFIG_RDOF_BENCH_LINE_PORT, for
 * tools/bench/rfgbench.py: the receiver channels' bursts, cs8 captures from the bench API's wideband history, and
 * bench_tx jobs through the SDR modem under bench.tx's gates (bench_tx.c). The engine and its server are common's
 * (rdof/benchline.h, rdof/benchnet.h); this file is the board's side of them.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app.h"

#if CONFIG_RDOF_BENCH && CONFIG_RDOF_BENCH_LINE_PORT > 0

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pthread.h"
#include "esp_timer.h"
#include "netclock.h"
#include "rdof/benchline.h"
#include "rdof/benchnet.h"
#include "rdof/benchtx.h"
#include "rdof/pdu.h"

static const char *TAG = "bench";

static rdof_bl_t *bl;
static rdof_bnet_t *net;

static double now_s(void) { return esp_timer_get_time() / 1e6; }

static void bl_device(void *ctx, char *kind, size_t kcap, char *name, size_t ncap, char *version, size_t vcap)
{
    snprintf(kind, kcap, "esp32s3-gateway");
    snprintf(name, ncap, "%s", g_scfg.name);
    snprintf(version, vcap, "%s", esp_app_get_description()->version);
}

static void bl_window(void *ctx, rdof_bl_window_t *w)
{
    memset(w, 0, sizeof *w);
    const double rate = g_scfg.sample_rate;
    w->running = g_sstat.receiving && isfinite(g_sstat.center_hz);
    w->center_hz = g_sstat.center_hz;
    w->fs = rate;
    w->lo_hz = g_sstat.center_hz - 0.4 * rate;
    w->hi_hz = g_sstat.center_hz + 0.4 * rate;
    w->gain_db = isfinite(g_sstat.gain_db) ? g_sstat.gain_db : 0;
    sdr_ppm_status_t p;
    sdr_ppm_status(&p);
    w->ppm = p.learned;
}

static int bl_freqs(void *ctx, rdof_bl_freq_t *out, int max)
{
    int k = 0;
    for (int c = 0; c < RDOF_SDR_MAX_CHANNELS && k < max; c++) {
        const rdof_sdr_cfg_channel_t *ch = sdr_rx_cfg_channel(c);
        if (!ch)
            break;
        out[k] = (rdof_bl_freq_t){.freq_hz = ch->freq, .ch = c,
                                  .caps = RDOF_BL_CAP_RX | RDOF_BL_CAP_DMR | RDOF_BL_CAP_IQ |
                                          (bench_tx_listed((uint32_t)llround(ch->freq)) ? RDOF_BL_CAP_TX : 0)};
        snprintf(out[k++].id, sizeof out[0].id, "%.15s", ch->id);
    }
    const int nrx = k;
    uint32_t tx[RDOF_BENCHTX_MAX_FREQS];
    const int ntx = bench_tx_freqs(tx, RDOF_BENCHTX_MAX_FREQS);
    for (int i = 0; i < ntx && k < max; i++) { /* a transmit frequency no channel hears */
        bool heard = false;
        for (int c = 0; c < nrx; c++)
            heard |= fabs(out[c].freq_hz - tx[i]) < 100.0;
        if (!heard)
            out[k++] = (rdof_bl_freq_t){.freq_hz = tx[i], .ch = -1, .caps = RDOF_BL_CAP_TX};
    }
    return k;
}

static long bl_iq_read(void *ctx, uint64_t sc, int8_t *out, long n, uint64_t *oldest, uint64_t *next)
{
    return bench_wide_read(sc, out, n, oldest, next);
}

static void bl_iq_warm(void *ctx) { bench_wide_warm(); }

static double bl_utc(void *ctx)
{
    bool synced;
    return netclock_utc_ns(esp_timer_get_time(), &synced) / 1e9;
}

static double bl_sc_utc(void *ctx, uint64_t sc) { return g_rx.t0 + (double)sc / g_scfg.sample_rate; }

static void bl_rx_stats(void *ctx, double *seconds, long *overflows, double *clip_pct, long *dsp_skipped, long *dropped)
{
    bool has_level, has_gain, has_agc;
    double peak, gain, ceiling, min_db, cpu;
    int changes;
    sdr_level(&has_level, clip_pct, &peak, &has_gain, &gain, &has_agc, &ceiling, &min_db, &changes);
    sdr_counters(dropped, overflows, &cpu);
    *seconds = g_rx_ready ? bl_utc(NULL) - g_rx.t0 : 0;
    *dsp_skipped = 0;
}

static const char *bl_can_tx(void *ctx) { return bench_tx_line_why(); }

static bool bl_tx_open(void *ctx, uint32_t freq, int cc, int power, char *why, size_t cap)
{
    return bench_tx_line_open(freq, cc, power, why, cap);
}

static int bl_tx_space(void *ctx, char *why, size_t cap) { return bench_tx_line_space(why, cap); }
static void bl_tx_push(void *ctx, uint8_t control, const uint8_t burst[33]) { bench_tx_line_push(control, burst); }
static void bl_tx_unkey(void *ctx) {} /* the job's modem unkeys once its queue has gone out */
static bool bl_tx_idle(void *ctx) { return bench_tx_line_idle(); }
static void bl_tx_close(void *ctx) { bench_tx_line_close(); }

/* From the DSP cores: a copy only while a connection wants bursts (the engine queues it). */
void bench_line_burst(int channel, const rdof_sdr_burst_t *b, double t)
{
    if (!bl || !rdof_bl_wants_bursts(bl))
        return;
    rdof_bl_burst_t o = {.ch = channel, .t = t, .pos48 = b->position, .cfo_hz = (float)b->freq_offset_hz,
                         .dev_hz = (float)b->deviation_hz, .level_dbfs = (float)b->b.power_dbfs,
                         .dt = (uint8_t)b->b.dt, .n = (int8_t)b->b.n, .slot = (int8_t)b->b.slot,
                         .family = (int8_t)b->b.family, .cc = (int8_t)b->b.cc,
                         .sync_errors = (int8_t)b->b.sync_errors};
    memcpy(o.raw, b->b.burst, sizeof o.raw);
    rdof_bl_burst(bl, &o);
}

void bench_line_start(void)
{
    if (bl)
        return;
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) < 1024 * 1024) { /* its 410 KB would starve internal RAM */
        ESP_LOGW(TAG, "line protocol: off (it needs PSRAM)");
        return;
    }
    static const rdof_bl_platform_t pf = {.device = bl_device, .window = bl_window, .freqs = bl_freqs,
                                          .iq_read = bl_iq_read, .iq_warm = bl_iq_warm, .utc = bl_utc,
                                          .sc_utc = bl_sc_utc, .rx_stats = bl_rx_stats, .can_tx = bl_can_tx,
                                          .tx_open = bl_tx_open, .tx_space = bl_tx_space, .tx_push = bl_tx_push,
                                          .tx_unkey = bl_tx_unkey, .tx_idle = bl_tx_idle, .tx_close = bl_tx_close};
    if (!(bl = rdof_bl_new(&pf))) {
        ESP_LOGE(TAG, "line protocol: no memory");
        return;
    }
    /* the server's thread: its stack in PSRAM, internal RAM being the scarce one */
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.thread_name = "bench_line";
    cfg.prio = 3;
    cfg.pin_to_core = 0;
    cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    esp_pthread_set_cfg(&cfg);
    char err[160];
    net = rdof_bnet_start(bl, NULL, CONFIG_RDOF_BENCH_LINE_PORT, now_s, 8192, err, sizeof err);
    const esp_pthread_cfg_t dflt = esp_pthread_get_default_config();
    esp_pthread_set_cfg(&dflt);
    if (!net) {
        ESP_LOGE(TAG, "line protocol: %s", err);
        rdof_bl_free(bl);
        bl = NULL;
        return;
    }
    ESP_LOGI(TAG, "line protocol on TCP %d", rdof_bnet_port(net));
}

#else

void bench_line_start(void) {}
void bench_line_burst(int channel, const rdof_sdr_burst_t *b, double t) {}

#endif
