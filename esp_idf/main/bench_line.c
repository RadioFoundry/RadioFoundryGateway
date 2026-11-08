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
#include "rdof/sdr/modes.h"

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
    sdr_ppm_now_t p; /* not sdr_ppm_status: rx_lock under the engine's lock deadlocks with a burst */
    sdr_ppm_now(&p);
    w->ppm = p.xtal, w->lo_ppm = p.lo_left, w->fs_ppm = p.fs_left, w->ppm_source = p.source;
    w->ppm_tol = p.tol, w->ppm_ref = p.ref, w->ppm_trusted = p.trusted;
    w->die_known = isfinite(p.die_c), w->die_c = p.die_c;
    w->ppm_held = p.held, w->ppm_settled = p.settled;
    w->iq_scale_db = bench_wide_scale_db();
    const rdof_fe_caps_t *caps = sdr_fe_caps();
    if (caps && caps->tx_reach_hz > 0) {
        w->tx_lo_hz = fmax(g_sstat.center_hz - caps->tx_reach_hz, caps->tx_freq_min);
        w->tx_hi_hz = fmin(g_sstat.center_hz + caps->tx_reach_hz, caps->tx_freq_max);
    }
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

static int bl_tx_bands(void *ctx, double (*out)[2], int max) { return bench_tx_bands(out, max); }

static long bl_iq_read(void *ctx, uint64_t sc, int8_t *out, long n, uint64_t *oldest, uint64_t *next)
{
    return bench_wide_read(sc, out, n, oldest, next);
}

static long bl_iq_filled(void *ctx, uint64_t sc, long n) { return bench_wide_filled(sc, n); }

static void bl_iq_warm(void *ctx) { bench_wide_warm(); }
static void bl_ppm_hold(void *ctx) { sdr_ppm_hold(); }

static double bl_utc(void *ctx)
{
    bool synced;
    return netclock_utc_ns(esp_timer_get_time(), &synced) / 1e9;
}

/* Another task's read of two counters: a stale answer only moves a flash write by a tick. */
bool bench_line_measuring(void) { return bl && (rdof_bl_wants_iq(bl) || rdof_bl_wants_bursts(bl)); }

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

static bool bl_tx_open(void *ctx, uint32_t freq, int cc, rdof_bench_power_t power, bool as_built, char *why,
                       size_t cap)
{
    return bench_tx_line_open(freq, cc, power, as_built, why, cap);
}

static int bl_tx_space(void *ctx, char *why, size_t cap) { return bench_tx_line_space(why, cap); }
static void bl_tx_push(void *ctx, uint8_t control, const uint8_t burst[33]) { bench_tx_line_push(control, burst); }
static void bl_tx_unkey(void *ctx) {} /* the job's modem unkeys once its queue has gone out */
static bool bl_tx_idle(void *ctx) { return bench_tx_line_idle(); }
static void bl_tx_close(void *ctx) { bench_tx_line_close(); }
static const char *bl_why_family(void *ctx, int family) { return bench_tx_why_family(family); }

/* bench_tx outbound: the SDR modem's own slot clock (modem_sdr.c), its TX samples on the receiver's timeline (the
 * front end sends TX sample p when the stream's count reaches p at 48 kS/s), so UTC from the stream's start. */
static const char *bl_ob_can(void *ctx) { return bench_tx_ob_why(); }

static bool bl_ob_open(void *ctx, const rdof_bl_ob_cfg_t *cfg, char *why, size_t cap)
{
    return bench_tx_ob_open(cfg->freq_hz, cfg->cc, cfg->power, cfg->seconds, why, cap);
}

static int bl_ob_state(void *ctx, rdof_bl_ob_state_t *s, char *why, size_t cap)
{
    if (bench_tx_ob_check(why, cap) < 0)
        return -1;
    sdr_modem_slots_t m;
    if (!sdr_modem_bench_slots(&m) || !m.tx_fs) {
        snprintf(why, cap, "the SDR modem went");
        return -1;
    }
    sdr_ppm_now_t p;
    sdr_ppm_now(&p);
    const double ppm = p.fs_left; /* the TX samples' clock: the chip corrects its carrier, not its rate */
    *s = (rdof_bl_ob_state_t){.keyed = m.keyed && m.key_k >= 0, .first = m.key_k, .next = m.slot_k, .base = m.base,
                              .slot = m.slot, .rate = m.tx_fs, .skips = (long)m.skips, .last_from = m.skip_from,
                              .last_to = m.skip_to, .on_air = m.keyed && m.key_k >= 0 && g_rx_ready, .map_pos = 0,
                              .map_t = g_rx.t0, .ppm = isfinite(ppm) ? ppm : 0};
    /* the CACH (12 symbols) and the burst's first 65.5 symbols, at 10 samples a symbol at 48 kS/s */
    s->sync_off = (int)llround(775.0 * m.tx_fs / 48000.0);
    if (!m.keyed && m.key_k >= 0)
        return 2;
    return s->keyed ? 1 : 0;
}

static int bl_sig_open(void *ctx, uint32_t freq, rdof_bench_power_t power, double seconds, const rdof_sig_src_t *src,
                       char *why, size_t cap)
{
    return bench_tx_sig_open(freq, power, seconds, src, why, cap);
}
static int bl_sig_state(void *ctx, char *why, size_t cap) { return bench_tx_sig_state(why, cap); }
static void bl_sig_close(void *ctx) { bench_tx_sig_close(); }
static double bl_sig_rate(void *ctx) { return bench_tx_sig_rate(); }

/* tune: PUT /sdr's retune (bench_api.c), asked without waiting: the engine's lock is held, and the DSP may be
 * waiting for it with a burst */
static const char *bl_tune(void *ctx, const rdof_bl_tune_t *t, const char *who, char *why, size_t cap)
{
    if (t->ntaps > SDR_MAX_TAPS) {
        snprintf(why, cap, "%d taps at most", SDR_MAX_TAPS);
        return "bad_request";
    }
    sdr_retune_t r = {.center_hz = t->center_hz,
                      .gain_db = t->gain_db == RDOF_BL_TUNE_AUTO ? SDR_RETUNE_SETTINGS : t->gain_db,
                      .ntaps = t->ntaps};
    for (int i = 0; i < t->ntaps; i++)
        r.taps_hz[i] = t->taps_hz[i];
    return bench_retune_line(&r, who, why, cap);
}
static int bl_tune_state(void *ctx, char *why, size_t cap) { return bench_retune_line_state(why, cap); }
static void bl_untune(void *ctx) { bench_retune_line_end(); }

/* mssim: the confirmed-data responder on the SDR modem (bench_tx.c, modem_sdr.c) */
static bool bl_mssim(void *ctx, const char *action, const char *req, size_t len, uint32_t freq_hz, int cc,
                     rdof_bench_power_t power, double seconds, uint32_t since, char *out, size_t cap,
                     const char **code)
{
    return bench_tx_mssim(action, req, len, freq_hz, cc, power, seconds, since, out, cap, code);
}

static void bl_ob_unkey(void *ctx) { bench_tx_ob_unkey(); }
static void bl_ob_close(void *ctx) { bench_tx_ob_close(); }
static void bl_ob_push(void *ctx, int slot, uint8_t control, const uint8_t burst[33])
{
    bench_tx_ob_push(slot, control, burst);
}

/* From the DSP cores: a copy only while a connection wants bursts (the engine queues it). */
void bench_line_burst(int channel, const rdof_sdr_burst_t *b, double t)
{
    if (!bl || !rdof_bl_wants_bursts(bl))
        return;
    rdof_bl_burst_t o = {.ch = channel, .t = t, .pos48 = b->position, .cfo_hz = (float)sdr_burst_cfo_hz(channel, b),
                         .dev_hz = (float)b->deviation_hz, .level_dbfs = (float)b->b.power_dbfs,
                         .dt = (uint8_t)b->b.dt, .n = (int8_t)b->b.n, .slot = (int8_t)b->b.slot,
                         .family = (int8_t)b->b.family, .cc = (int8_t)b->b.cc,
                         .sync_errors = (int8_t)b->b.sync_errors};
    memcpy(o.raw, b->b.burst, sizeof o.raw);
    o.has_cach = b->has_cach;
    memcpy(o.cach, b->cach, sizeof o.cach);
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
                                          .tx_bands = bl_tx_bands,
                                          .iq_read = bl_iq_read, .iq_filled = bl_iq_filled,
                                          .iq_warm = bl_iq_warm, .ppm_hold = bl_ppm_hold, .utc = bl_utc,
                                          .sc_utc = bl_sc_utc, .rx_stats = bl_rx_stats, .can_tx = bl_can_tx,
                                          .tx_open = bl_tx_open, .tx_space = bl_tx_space, .tx_push = bl_tx_push,
                                          .tx_unkey = bl_tx_unkey, .tx_idle = bl_tx_idle, .tx_close = bl_tx_close,
                                          .tx_why_family = bl_why_family, .ob_open = bl_ob_open,
                                          .ob_state = bl_ob_state, .ob_unkey = bl_ob_unkey, .ob_close = bl_ob_close,
                                          .ob_can = bl_ob_can, .ob_push = bl_ob_push, .modes = rdof_sdr_modes, .sig_open = bl_sig_open,
                                          .sig_state = bl_sig_state, .sig_close = bl_sig_close,
                                          .sig_rate = bl_sig_rate, .tune = bl_tune, .tune_state = bl_tune_state,
                                          .untune = bl_untune, .mssim = bl_mssim};
    if (!(bl = rdof_bl_new(&pf))) {
        ESP_LOGE(TAG, "line protocol: no memory");
        return;
    }
    netclock_set_store_hold(bench_line_measuring);
    /* the server's thread: its stack in PSRAM, internal RAM being the scarce one */
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.thread_name = "bench_line";
    /* 7: over ipgw (6), under gnss, lwIP, Wi-Fi and the front end. At 3 a trigger at 384 kS/s didn't get enough of core
     * 0 and its history moved on under it */
    cfg.prio = 7;
    cfg.pin_to_core = 0;
    cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    esp_pthread_set_cfg(&cfg);
    char err[160];
    /* 16 KB: a job's records and JSON parse nest deep (8 KB overflowed in burst_out) */
    net = rdof_bnet_start(bl, NULL, CONFIG_RDOF_BENCH_LINE_PORT, now_s, 16384, err, sizeof err);
    const esp_pthread_cfg_t dflt = esp_pthread_get_default_config();
    esp_pthread_set_cfg(&dflt);
    if (!net) {
        ESP_LOGE(TAG, "line protocol: %s", err);
        rdof_bl_free(bl);
        bl = NULL;
        return;
    }
    ESP_LOGI(TAG, "line protocol on TCP %d", rdof_bnet_port(net));
    dnssd_bench(g_scfg.name[0] ? g_scfg.name : g_roles.box, rdof_bnet_port(net));
}

#else

void bench_line_start(void) {}
void bench_line_burst(int channel, const rdof_sdr_burst_t *b, double t) {}

#endif
