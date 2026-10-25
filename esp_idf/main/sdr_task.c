// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The receive loop, as receiver.run with SoapySource: the front end's samples
 * (rdof/sdr/frontend.h: the RTL-SDR from the USB ring, the self-test clip in a
 * loop with "sim", an LR2021 on SPI), clip measurement and the AGC law
 * (rdof/sdr/agc.h), the gain scaled back out so RSSI holds, then the rdof_sdr receiver
 * on core 1, or with dsp_cores 2 the channel demods split over both cores
 * (balanced as it runs: sdr_process_chunk).
 * While the page shows it, a display spectrum of the span (rdof/sdr/spectrum.h)
 * is kept too.
 */
#include <math.h>
#include <string.h>

#include "app.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "rdof/sdr/agc.h"
#include "rdof/sdr/channelizer.h"
#include "rdof/sdr/demod.h"
#include "rdof/sdr/iq.h"
#include "rdof/sdr/ppm.h"
#include "rdof/sdr/rate.h"
#include "rdof/sdr/spectrum.h"
#include "netclock.h"
#include "netclock_tempcal.h"
#include "nvs.h"
#include "sdr_fe.h"
#include "sdr_prof.h"
#include "esp_memory_utils.h"
#include "mem.h"

static const char *TAG = "sdr";

#define CHUNK CONFIG_RDOF_SDR_CHUNK
#define DSP_CORE 1
#define STALL_US 3000000 /* no samples this long: restart the stream */

rdof_sdr_rx_t g_rx;
bool g_rx_ready;
static SemaphoreHandle_t rx_mutex;
static TaskHandle_t dsp_task_handle;
static rdof_fe_t fe;     /* the front end the settings name (sdr_start) */
static int fe_ppm;     /* the whole ppm it's set to */
static double fe_rate; /* the rate it really runs at (tune's) */
static size_t fe_bytes; /* bytes a sample, in its format */
static volatile bool fe_streaming; /* started and not stopped */

/* levels and gain (rdof/sdr/agc.h), read by the status */
static portMUX_TYPE level_lock = portMUX_INITIALIZER_UNLOCKED;
static rdof_level_stats_t levels;
#if RDOF_FEATURE_SDR_AGC
static rdof_agc_t agc;
#define AGC_FIELD(f) agc.f
#else
#define AGC_FIELD(f) 0
#endif
static bool agc_on;
static double gain_db = NAN, ref_gain = NAN;
static float scale = 1.0f;
/* load */
static int64_t busy_us, wall_from_us;
static int64_t stage_us[SDR_STAGES]; /* busy_us by stage */
static long dropped, usb_errors;
/* the display spectrum: rows of SPEC_BINS, newest at spec_seq */
#define SPEC_BINS 512
#define SPEC_ROWS 64
static portMUX_TYPE spec_lock = portMUX_INITIALIZER_UNLOCKED;
static rdof_spectrum_t spec;
static uint8_t *spec_rows; /* SPEC_ROWS x SPEC_BINS, PSRAM */
static uint32_t spec_seq;
static long long spec_sc[SPEC_ROWS]; /* each row's last wideband sample (the bench API's sample clock) */
static volatile int64_t spec_want_until; /* esp_timer us: someone looked at it lately */
static int spec_row_ms;
static bool spec_failed;
static rdof_cf_t *iq; /* the chunk as float, internal RAM */
static uint8_t *raw_buf; /* the chunk as the front end gives it, internal RAM */
static double rx_freq[RDOF_SDR_MAX_CHANNELS]; /* g_rx's channels' frequencies */
static int64_t last_wait_us; /* when the DSP task last blocked: behind, it has to let core 1's idle task run */

void rx_lock(void)
{
    if (rx_mutex)
        xSemaphoreTake(rx_mutex, portMAX_DELAY);
}

void rx_unlock(void)
{
    if (rx_mutex)
        xSemaphoreGive(rx_mutex);
}

/* ── two cores ──────────────────────────────────────────────────────────── */

/* The profile (sdr_prof.h). */
static sdr_prof_t prof = {.split = -1};

void sdr_prof_reset(void) { prof = (sdr_prof_t){.split = -1}; }
#if CONFIG_RDOF_SDR_TWO_CORES
static bool serial_cores; /* the self-test's contention check: core 1 waits for the helper before its own share */
void sdr_prof_serial(bool on) { serial_cores = on; }
#else
void sdr_prof_serial(bool on) {}
#endif
void sdr_prof_get(sdr_prof_t *out) { *out = prof; }

static void place(char *out, size_t cap, size_t *n, const char *name, const void *p, size_t bytes)
{
    if (p && *n < cap)
        *n += (size_t)snprintf(out + *n, cap - *n, "%s%s %s %u KB", *n ? ", " : "", name,
                               esp_ptr_external_ram(p) ? "PSRAM" : "internal", (unsigned)((bytes + 512) / 1024));
}

void sdr_prof_placement(const rdof_sdr_rx_t *rx, char *out, size_t cap)
{
    size_t n = 0;
    out[0] = 0;
    const rdof_chz_t *z = &rx->chz;
    place(out, cap, &n, "y", rx->y, sizeof(rdof_cf_t) * (size_t)rx->stride * rx->C);
    place(out, cap, &n, "Xs", z->Xs, sizeof(rdof_cf_t) * (size_t)z->N * z->xs_cap);
    place(out, cap, &n, "win", z->win, sizeof(rdof_cf_t) * z->N);
    place(out, cap, &n, "X", z->X, sizeof(rdof_cf_t) * z->N);
    place(out, cap, &n, "demods", rx->demods, sizeof(rdof_demod_t) * rx->C);
    place(out, cap, &n, "trackers", rx->trackers, sizeof(rdof_tracker_t) * rx->C);
    int in = 0;
    for (int c = 0; c < rx->C; c++)
        in += !esp_ptr_external_ram(rx->demods[c].buf);
    if (n < cap)
        n += (size_t)snprintf(out + n, cap - n, ", demod buf+pow %u KB each, %d of %d channels' internal",
                              (unsigned)(2 * sizeof(float) * rx->demods[0].cap / 1024), in, rx->C);
}

/* With dsp_cores 2, this task (core 1) runs the channelizer's forward FFTs (the one part that can't be split),
 * then channels [0, split) end to end (their share of the channelizer, then the demod) while the helper (core 0)
 * does [split, C) the same way; then the trackers' ticks and AFC here.
 *
 * SDR_SPLIT_AUTO picks the split by what a chunk really takes, start to end: every 64 chunks it tries one channel
 * more on the less busy core, keeps that if the next 64 chunks were 2% faster, else goes back and waits 8 windows
 * before trying again. split C is every channel on core 1 (two cores not used), so it can't settle on something
 * slower than one core; core 1 always keeps at least one channel. */
#if CONFIG_RDOF_SDR_TWO_CORES
static TaskHandle_t helper;
static SemaphoreHandle_t go, done;
static rdof_sdr_rx_t *job_rx;
static volatile int job_c0, job_c1;
static volatile int64_t go_us, helper_start_us;
static volatile uint32_t helper_chz, helper_demod;

static struct {
    const rdof_sdr_rx_t *rx;     /* the receiver it's balancing */
    int s, from, wait;         /* the split; the one a probe came from; windows before the next probe */
    bool probe;
    int n;
    int64_t wall, c1, c0;      /* this window: chunk time, core 1's busy time, core 0's */
    double base;               /* the last settled window's time a chunk */
} bal = {.s = -1};

static void helper_task(void *arg)
{
    for (;;) {
        xSemaphoreTake(go, portMAX_DELAY);
        helper_start_us = esp_timer_get_time();
        rdof_sdr_rx_t *rx = job_rx;
        uint32_t a = esp_cpu_get_cycle_count();
        if (!rx->fused)
            rdof_chz_channels(&rx->chz, job_c0, job_c1, 1, rx->y, rx->stride);
        uint32_t b = esp_cpu_get_cycle_count();
        rdof_sdr_rx_demod(rx, job_c0, job_c1);
        helper_chz = b - a;
        helper_demod = esp_cpu_get_cycle_count() - b;
        xSemaphoreGive(done);
    }
}

/* On its planned stack (sdr_mem_claims), the first time a chunk splits. */
static void helper_start(void)
{
    if (helper)
        return;
    go = xSemaphoreCreateBinary();
    done = xSemaphoreCreateBinary();
    mem_task("sdr", "dsp_helper", helper_task, CONFIG_RDOF_SDR_HELPER_STACK, NULL, 10, 0, &helper);
}

static int first_split(int C)
{
    int k = (int)lrint(C / 2.0 - 0.6); /* 33 + 28 k = 28 (C - k) */
    return k < 1 ? 1 : k > C ? C : k;
}

static void balance(int C, int64_t wall, int64_t c1, int64_t c0)
{
    bal.wall += wall;
    bal.c1 += c1;
    bal.c0 += c0;
    if (++bal.n < 64)
        return;
    const double avg = (double)bal.wall / bal.n;
    if (bal.probe) {
        bal.probe = false;
        if (avg < 0.98 * bal.base) {
            ESP_LOGI(TAG, "dsp: %d of %d channels on core 1: %.0f us a chunk (was %.0f with %d)", bal.s, C, avg,
                     bal.base, bal.from);
            bal.base = avg;
        } else {
            bal.s = bal.from;
            bal.wait = 8;
        }
    } else {
        bal.base = avg;
        if (bal.wait > 0) {
            bal.wait--;
        } else {
            const int to = bal.s + (bal.c1 > bal.c0 ? -1 : 1); /* work off the busier core */
            if (to >= 1 && to <= C) {
                bal.from = bal.s;
                bal.s = to;
                bal.probe = true;
            } else {
                bal.wait = 8;
            }
        }
    }
    g_sstat.dsp_split = bal.s;
    bal.n = 0;
    bal.wall = bal.c1 = bal.c0 = 0;
}
#endif

void sdr_process_chunk(rdof_sdr_rx_t *rx, const rdof_cf_t *iq, int n, int split)
{
    const int64_t t0 = esp_timer_get_time();
    int64_t t1, t2;
    /* cycles on this core: the forward step ends at kf, its channels' channelizer runs kc to kz, their demods kz to
     * kd, the finish starts at ks (after the join) */
    const uint32_t k0 = esp_cpu_get_cycle_count();
    uint32_t kf, kc, kz, kd, ks;
#if CONFIG_RDOF_SDR_TWO_CORES
    const bool autos = split == SDR_SPLIT_AUTO && rx->C > 1;
    if (autos) {
        if (bal.s < 0 || bal.rx != rx) { /* a new receiver: start over */
            memset(&bal, 0, sizeof bal);
            bal.rx = rx;
            bal.s = first_split(rx->C);
        }
        split = bal.s;
    }
    if (autos || (split > 0 && split < rx->C)) {
        rdof_sdr_rx_forward(rx, iq, n);
        t1 = esp_timer_get_time();
        kf = esp_cpu_get_cycle_count();
        const bool two = split < rx->C;
        if (two) {
            helper_start();
            job_rx = rx;
            job_c0 = split;
            job_c1 = rx->C;
            go_us = t1;
            xSemaphoreGive(go);
        }
        int64_t waited = 0;
        if (two && serial_cores) { /* one core at a time: what each share costs without the other running */
            xSemaphoreTake(done, portMAX_DELAY);
            waited = esp_timer_get_time() - t1;
        }
        kc = esp_cpu_get_cycle_count();
        if (!rx->fused)
            rdof_chz_channels(&rx->chz, 0, split, 0, rx->y, rx->stride);
        kz = esp_cpu_get_cycle_count();
        rdof_sdr_rx_demod(rx, 0, split);
        kd = esp_cpu_get_cycle_count();
        const int64_t wait0 = esp_timer_get_time(), mine = wait0 - t1 - waited;
        if (two) {
            if (!serial_cores)
                xSemaphoreTake(done, portMAX_DELAY);
            prof.join_us += esp_timer_get_time() - wait0 + waited;
            prof.wake_us += helper_start_us - go_us;
            prof.chz[1] += helper_chz;
            prof.demod[1] += helper_demod;
        }
        t2 = esp_timer_get_time();
        ks = esp_cpu_get_cycle_count();
        prof.split = split;
        if (autos)
            balance(rx->C, t2 - t0, (t1 - t0) + mine,
                    two ? (int64_t)((helper_chz + helper_demod) / CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ) : 0);
    } else
#endif
    {
        rdof_sdr_rx_channelize(rx, iq, n); /* rdof_sdr_rx_process in two steps, to time them */
        t1 = esp_timer_get_time();
        kf = kc = kz = esp_cpu_get_cycle_count();
        rdof_sdr_rx_demod(rx, 0, rx->C);
        kd = ks = esp_cpu_get_cycle_count();
        t2 = esp_timer_get_time();
        prof.split = -1;
    }
    rdof_sdr_rx_finish(rx);
    if (rx == &g_rx)
        bench_on_chunk(rx, iq, n); /* the bench API's I/Q streams */
    const int64_t t3 = esp_timer_get_time();
    prof.chunks++;
    prof.samples += n;
    prof.fwd += kf - k0;
    prof.chz[0] += kz - kc;
    prof.demod[0] += kd - kz;
    prof.finish += esp_cpu_get_cycle_count() - ks;
    prof.wall_us += t3 - t0;
    stage_us[SDR_STAGE_CHANNELIZE] += t1 - t0;
    stage_us[SDR_STAGE_DEMOD] += t2 - t1;
    stage_us[SDR_STAGE_TRACK] += t3 - t2;
}

void sdr_dsp_stacks(int *dsp, int *helper_free)
{
    *dsp = dsp_task_handle ? (int)uxTaskGetStackHighWaterMark(dsp_task_handle) : -1;
#if CONFIG_RDOF_SDR_TWO_CORES
    *helper_free = helper ? (int)uxTaskGetStackHighWaterMark(helper) : -1;
#else
    *helper_free = -1;
#endif
}

/* ── status ─────────────────────────────────────────────────────────────── */

void sdr_level(bool *has_level, double *clip_pct, double *peak_dbfs, bool *has_gain, double *gdb, bool *has_agc,
               double *ceiling, double *min_db, int *changes)
{
    portENTER_CRITICAL(&level_lock);
    rdof_level_stats_t s = levels;
    memset(&levels, 0, sizeof levels);
    *has_gain = !isnan(gain_db);
    *gdb = gain_db;
    *has_agc = agc_on;
    *ceiling = agc_on ? AGC_FIELD(ceiling) : 0;
#if RDOF_FEATURE_SDR_AGC
    *min_db = agc_on ? agc.steps[0] : 0;
#else
    *min_db = 0;
#endif
    *changes = agc_on ? AGC_FIELD(changes) : 0;
    portEXIT_CRITICAL(&level_lock);
    *has_level = g_rx_ready;
    rdof_level_stats_take(&s, clip_pct, peak_dbfs);
}

bool sdr_fe_stats(rdof_fe_stats_t *out)
{
    memset(out, 0, sizeof *out);
    if (!fe.ops || !fe.ops->stats)
        return false;
    fe.ops->stats(fe.ctx, out);
    return true;
}

const rdof_fe_caps_t *sdr_fe_caps(void) { return fe.caps; }

void sdr_counters(long *dropped_chunks, long *overflows, double *cpu_load)
{
    rdof_fe_stats_t s;
    sdr_fe_stats(&s);
    *dropped_chunks = dropped + (long)s.overflows;
    *overflows = usb_errors + (long)s.errors;
    int64_t now = esp_timer_get_time(), wall = now - wall_from_us;
    *cpu_load = wall > 0 && wall_from_us ? (double)busy_us / (double)wall : NAN;
    for (int k = 0; k < SDR_STAGES; k++) {
        g_sstat.stage_load[k] = wall > 0 && wall_from_us ? (double)stage_us[k] / (double)wall : NAN;
        stage_us[k] = 0;
    }
    /* the demods' part by step, in CPU cycles (rdof_demod_prof) */
    const double cyc = (double)wall * CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    const rdof_demod_prof_t p = rdof_demod_prof;
    memset(&rdof_demod_prof, 0, sizeof rdof_demod_prof);
    const uint64_t v[4] = {p.fm, p.search, p.fly, p.compact};
    for (int k = 0; k < 4; k++)
        g_sstat.demod_load[k] = wall > 0 && wall_from_us ? (double)v[k] / cyc : NAN;
    busy_us = 0;
    wall_from_us = now;
    g_sstat.cpu_load = *cpu_load;
}

/* ── setup ──────────────────────────────────────────────────────────────── */

/* The chip's name in the logs, as they have always said it for the RTL-SDR and the simulated one. */
static const char *chip(void) { return g_scfg.frontend == RDOF_FE_LR2021 ? "LR2021" : "RTL"; }

/* ── a retune at run time (PUT /sdr, the bench API) ─────────────────────── */

/* What the retune holds while it's on (NAN: the settings'), and its taps: receiver channels at any frequency in the
 * window, after the configured ones, never on MQTT or HBlink4. live_taps: those the running receiver has. */
static double rt_center = NAN, rt_gain = NAN;
static rdof_sdr_cfg_channel_t taps[SDR_MAX_TAPS];
static int ntaps, live_taps;
static sdr_retune_t rt_req;
static volatile int rt_state; /* 0 idle, 1 asked (the DSP task applies it between chunks) */
static volatile bool rt_restore; /* back to the settings (a lease ended), applied the same way */
static char rt_why[96];

const rdof_sdr_cfg_channel_t *sdr_rx_cfg_channel(int k)
{
    int i = 0;
    for (int c = 0; c < g_scfg.nch; c++)
        if (g_sstat.in_window[c] && i++ == k)
            return &g_scfg.ch[c];
    k -= i;
    return k >= 0 && k < live_taps ? &taps[k] : NULL;
}

bool sdr_is_tap(const char *id)
{
    return id >= (const char *)taps && id < (const char *)(taps + SDR_MAX_TAPS);
}

void sdr_retune_get(double *center_hz, double *gain_db, double *taps_hz, int *n)
{
    *center_hz = rt_center;
    *gain_db = rt_gain;
    *n = live_taps;
    for (int i = 0; i < live_taps; i++)
        taps_hz[i] = taps[i].freq;
}

/* Which channels the tuner takes, and where it's tuned: plan_center when they all fit, else
 * the best window (rdof_plan_window), the rest left out; both in the front end's band. A retune's center comes first. */
static int plan(double *center, int *map)
{
    double f[RDOF_SDR_MAX_CHANNELS];
    bool in[RDOF_SDR_MAX_CHANNELS];
    for (int c = 0; c < g_scfg.nch; c++)
        f[c] = g_scfg.ch[c].freq;
    const double half = g_scfg.sample_rate * fe.caps->usable / 2;
    if (isfinite(rt_center) || g_scfg.center) {
        *center = isfinite(rt_center) ? rt_center : g_scfg.center;
        for (int c = 0; c < g_scfg.nch; c++)
            in[c] = fabs(f[c] - *center) <= half;
    } else if (rdof_plan_center_band(f, g_scfg.nch, g_scfg.sample_rate, fe.caps->usable, fe.caps->dc_guard_hz, center) ==
               0) {
        for (int c = 0; c < g_scfg.nch; c++)
            in[c] = true;
    } else if (rdof_plan_window_band(f, g_scfg.nch, g_scfg.sample_rate, fe.caps->usable, fe.caps->dc_guard_hz, center,
                                     in) == 0) {
        return 0;
    }
    int n = 0;
    for (int c = 0; c < g_scfg.nch; c++) {
        g_sstat.in_window[c] = in[c];
        if (in[c])
            map[n++] = c;
        else
            ESP_LOGW(TAG, "%s (%.5f MHz) is outside the %.0f kS/s window at %.5f MHz: left out", g_scfg.ch[c].id,
                     f[c] / 1e6, g_scfg.sample_rate / 1e3, *center / 1e6);
        if (in[c] && fabs(f[c] - *center) < fe.caps->dc_guard_hz)
            ESP_LOGW(TAG, "%s is %.1f kHz from the tuner center: the %s's DC spike may hurt it", g_scfg.ch[c].id,
                     fabs(f[c] - *center) / 1e3, chip());
    }
    g_sstat.center_hz = *center;
    g_sstat.channels_in = n;
    return n;
}

static bool rx_setup(void)
{
    double center;
    int map[RDOF_SDR_MAX_CHANNELS];
    int n = plan(&center, map);
    const double half = g_scfg.sample_rate * fe.caps->usable / 2;
    if (!n && !ntaps && isfinite(rt_center)) { /* a retune away from every channel: one tap, clear of the DC spike */
        memset(&taps[0], 0, sizeof taps[0]);
        taps[0].freq = rt_center + g_scfg.sample_rate / 8;
        ntaps = 1;
    }
    for (int i = 0; i < ntaps; i++) {
        snprintf(taps[i].id, sizeof taps[i].id, "T%d", i + 1);
        snprintf(taps[i].name, sizeof taps[i].name, "tap %d", i + 1);
        taps[i].cc = -1;
        taps[i].tg = taps[i].peer_id = -1;
        if (fabs(taps[i].freq - center) > half) {
            snprintf(rt_why, sizeof rt_why, "tap %.5f MHz is outside the window (%.5f MHz +- %.0f kHz)",
                     taps[i].freq / 1e6, center / 1e6, half / 1e3);
            return false;
        }
    }
    if (n + ntaps > RDOF_SDR_MAX_CHANNELS) {
        snprintf(rt_why, sizeof rt_why, "%d channels and %d taps: %d at most", n, ntaps, RDOF_SDR_MAX_CHANNELS);
        return false;
    }
    if (!n && !ntaps) {
        ESP_LOGE(TAG, "no channel fits the tuner window");
        snprintf(rt_why, sizeof rt_why, "no channel fits the tuner window");
        return false;
    }
    rdof_sdr_channel_t ch[RDOF_SDR_MAX_CHANNELS];
    for (int i = 0; i < n; i++) {
        const rdof_sdr_cfg_channel_t *c = &g_scfg.ch[map[i]];
        ch[i].id = c->id;
        ch[i].freq_hz = c->freq;
        rx_freq[i] = c->freq;
        ch[i].cc = c->cc;
        ch[i].slot = c->slot;
    }
    for (int i = 0; i < ntaps; i++) {
        ch[n + i].id = taps[i].id;
        ch[n + i].freq_hz = rx_freq[n + i] = taps[i].freq;
        ch[n + i].cc = -1;
        ch[n + i].slot = 0;
    }
    live_taps = ntaps;
    n += ntaps;
    /* the ppm is set on the chip, as SoapySDR does for the RTL: no software correction */
    rdof_sdr_rx_config_t rc = {g_scfg.sample_rate, center, rdof_sdr_config_rssi_offset(&g_scfg), 0.0, g_scfg.afc, CHUNK};
    rdof_sink_t sink = sdr_mqtt_sink();
    rdof_demod_set_clock(esp_cpu_get_cycle_count);
    extern size_t g_dsp_internal_bytes, g_dsp_psram_bytes;
    const size_t in0 = g_dsp_internal_bytes, ps0 = g_dsp_psram_bytes;
    /* the hot arena (sdr.c) is this receiver's: its hot buffers, esp-dsp's plans and filters, and with two cores the
     * blocks' second buffers, taken now rather than on the first chunk */
    sdr_hot_arena_open();
    int bad = rdof_sdr_rx_init(&g_rx, &rc, ch, n, &sink, 0.0);
    if (!bad && g_scfg.dsp_cores > 1)
        rdof_chz_reserve(&g_rx.chz, CHUNK);
    sdr_hot_arena_close();
    size_t hot_used, hot_peak, hot_cap;
    sdr_hot_arena_use(&hot_used, &hot_peak, &hot_cap);
    if (bad) {
        ESP_LOGE(TAG, "receiver init failed (out of memory?)");
        snprintf(rt_why, sizeof rt_why, "receiver init failed (out of memory?)");
        rdof_sdr_rx_free(&g_rx);
        live_taps = 0;
        return false;
    }
    ESP_LOGI(TAG, "receiver buffers: %u KB internal (%u of the hot arena's %u bytes), %u KB PSRAM (internal free %u KB, "
             "largest block %u KB)",
             (unsigned)((g_dsp_internal_bytes - in0) / 1024), (unsigned)hot_used, (unsigned)hot_cap,
             (unsigned)((g_dsp_psram_bytes - ps0) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
    for (int i = 0; i < n; i++) {
        rdof_sink_t hs;
        if (i < n - live_taps && sdr_hb_sink(map[i], &hs))
            rdof_sdr_rx_add_sink(&g_rx, i, &hs);
        if (bench_call_sink(i, &hs))
            rdof_sdr_rx_add_sink(&g_rx, i, &hs);
#if CONFIG_RDOF_IPGW
        if (ipgw_sink(i, (uint32_t)rx_freq[i], ch[i].cc, NULL, &hs)) /* data bursts to the IP gateway */
            rdof_sdr_rx_add_sink(&g_rx, i, &hs);
#endif
    }
    g_rx.gate = g_scfg.idle_gate;
    g_rx.on_burst = bench_on_burst; /* an sdr_transceiver's modem hears through the receiver, the bench API's streams */
    g_rx.muted = sdr_tx_muted;
    g_rx.burst_ctx = NULL;
    bench_rx_started();
    g_rx_ready = true;
    ESP_LOGI(TAG, "receiving %d channels around %.5f MHz (%d dsp core%s); internal RAM free %u KB", n, center / 1e6,
             g_scfg.dsp_cores, g_scfg.dsp_cores > 1 ? "s" : "",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    return true;
}

static bool tune(void)
{
    int ppm = (int)lrint(g_scfg.ppm);
    double rate;
    if (fe.ops->tune(fe.ctx, g_scfg.sample_rate, g_rx.center_hz, ppm, &rate)) {
        snprintf(g_sstat.rtl_error, sizeof g_sstat.rtl_error, "%s", fe.ops->error(fe.ctx));
        ESP_LOGE(TAG, "tuning failed: %s", fe.ops->error(fe.ctx));
        return false;
    }
    fe_ppm = ppm;
    fe_rate = rate;
    if (!fe.caps->gain)
        return true; /* sim: no gain, nothing to say */
    double got = NAN;
    if (isfinite(rt_gain)) { /* a retune's fixed gain: no AGC law while it's on */
        agc_on = false;
        scale = 1.0f;
        fe.ops->set_gain(fe.ctx, rt_gain, &got);
        gain_db = got;
    } else if (g_scfg.has_gain) {
#if RDOF_FEATURE_SDR_AGC
        if (g_scfg.agc) {
            const double *db;
            int ns = fe.ops->gain_steps(fe.ctx, &db);
            rdof_agc_config_t ac = {g_scfg.agc_cfg[0], g_scfg.agc_cfg[1], g_scfg.agc_cfg[2],
                                  g_scfg.agc_cfg[3], g_scfg.agc_cfg[4], g_scfg.agc_cfg[5]};
            rdof_agc_init(&agc, &ac, db, ns, g_scfg.gain, NAN);
            agc_on = true;
            ref_gain = agc.max;
            fe.ops->set_gain(fe.ctx, agc.gain, &got);
        } else
#endif
        {
            if (g_scfg.agc)
                ESP_LOGW(TAG, "device.agc is set but this build has no AGC (CONFIG_RDOF_FEATURE_SDR_AGC)");
            fe.ops->set_gain(fe.ctx, g_scfg.gain, &got);
        }
        gain_db = got;
        if (agc_on)
            scale = (float)pow(10.0, (ref_gain - gain_db) / 20.0);
    } else {
        fe.ops->set_gain(fe.ctx, NAN, &got);
        gain_db = NAN;
    }
    g_sstat.gain_db = gain_db;
    ESP_LOGI(TAG, "tuned %.5f MHz at %lu S/s, ppm %d, gain %s%.1f dB%s", g_rx.center_hz / 1e6, (unsigned long)rate,
             ppm, isnan(gain_db) ? "tuner AGC " : "", isnan(gain_db) ? 0.0 : gain_db,
             agc_on ? " (AGC law on)" : "");
    return true;
}

/* ── the sample clock, timed against the board's (rdof/sdr/rate.h) ─────────── */

#define RATE_PENDING 8           /* closed bins waiting for the fit (one every 10 s; drained every 5 s) */
#define RATE_TRUST_PPM 0.3       /* a crystal estimate this good is the learned value... */
#define RATE_TRUST_SPAN_S 300.0  /* ...once its fit spans this long */
#define RATE_STORED_ERR_PPM 0.5  /* the board's rate from the last run, not fitted in this one: assume this off */
/* Settled: the board's die temperature and the sample rate (over the last RATE_RECENT_S) held for SETTLE_WINDOW_S.
 * Until then the crystals are warming (about 15 minutes from room temperature to 50 C on the bench), so nothing
 * learned is saved and the sample clock isn't trusted. */
#define SETTLE_EVERY_S 60.0
#define SETTLE_WINDOW_S 600.0
#define SETTLE_DIE_C 3.0         /* the sensor reads in steps of about 1 C and wanders by one or two */
#define SETTLE_RATE_PPM 0.2
#define RATE_RECENT_S 300.0

static portMUX_TYPE rate_lock = portMUX_INITIALIZER_UNLOCKED;
static rdof_rate_bin_t rate_bin;          /* rate_lock: the front end's task adds to it */
static bool rate_on;                    /* rate_lock */
static double rate_pend[RATE_PENDING][2];
static int rate_npend;                  /* rate_lock */
static rdof_rate_t rate_fit;              /* rx_lock, from here down */
static double rate_corr;                /* the ppm the front end's correction takes out of its sample rate */
static double xtal = NAN, xtal_err = NAN; /* the crystal's error from the fit, while it's good */
static double board_ppm = NAN;
static bool board_learned, xtal_trusted, xtal_ever;
static nc_settle_t die_settle, rate_settle;
static double die_c = NAN;
static bool settled;

void sdr_rate_block(int64_t t_us, uint64_t samples)
{
    double t, x;
    portENTER_CRITICAL(&rate_lock);
    if (rate_on && rdof_rate_bin_add(&rate_bin, t_us, samples, &t, &x) && rate_npend < RATE_PENDING) {
        rate_pend[rate_npend][0] = t;
        rate_pend[rate_npend][1] = x;
        rate_npend++;
    }
    portEXIT_CRITICAL(&rate_lock);
}

/* Time the stream from scratch: it (re)started, or its sample rate moved (the correction). fresh: a dongle not
 * timed before, or a rate that moved, so whether it has settled starts over too. Under rx_lock, or before the
 * stream starts. */
static void rate_restart(bool fresh)
{
    portENTER_CRITICAL(&rate_lock);
    rdof_rate_bin_init(&rate_bin, fe_rate);
    rate_on = fe_rate > 0;
    rate_npend = 0;
    portEXIT_CRITICAL(&rate_lock);
    rdof_rate_init(&rate_fit);
    if (!die_settle.every_s)
        nc_settle_init(&die_settle, SETTLE_EVERY_S, SETTLE_WINDOW_S, SETTLE_DIE_C);
    if (fresh || !rate_settle.every_s)
        nc_settle_init(&rate_settle, SETTLE_EVERY_S, SETTLE_WINDOW_S, SETTLE_RATE_PPM);
    rate_corr = fe.ops && fe.ops->sample_ppm ? fe.ops->sample_ppm(fe.ctx) : 0.0;
    xtal_trusted = false; /* xtal keeps the last value until the new fit has its own */
}

/* Whether the crystals have settled (SETTLE_WINDOW_S): the die's temperature held, and the sample rate where the
 * front end has a sample clock. On settling, the fit forgets the warm-up before the window. Under rx_lock. */
static void settle_tick(double t)
{
    die_c = netclock_die_c();
    nc_settle_add(&die_settle, t, die_c);
    double recent;
    const bool clocked = fe.ops && fe.ops->sample_ppm;
    if (clocked && rdof_rate_recent(&rate_fit, RATE_RECENT_S, &recent))
        nc_settle_add(&rate_settle, t, recent);
    const bool was = settled;
    settled = (isnan(die_c) || nc_settle_ok(&die_settle, t)) && (!clocked || nc_settle_ok(&rate_settle, t));
    if (settled && !was) {
        if (clocked && rate_fit.count)
            rdof_rate_trim(&rate_fit, rate_fit.t[(rate_fit.head + rate_fit.count - 1) % RDOF_RATE_POINTS] - SETTLE_WINDOW_S);
        ESP_LOGI(TAG, "ppm: settled (die %.1f C)", die_c);
    } else if (!settled && was) {
        ESP_LOGI(TAG, "ppm: not settled any more (die %.1f C, spread %.1f C; sample rate spread %.2f ppm)", die_c,
                 nc_settle_range(&die_settle, t), nc_settle_range(&rate_settle, t));
    }
}

/* The bins closed since the last look, into the fit; then the crystal's error. Under rx_lock. */
static void rate_tick(double t)
{
    double pend[RATE_PENDING][2];
    portENTER_CRITICAL(&rate_lock);
    const int n = rate_npend;
    memcpy(pend, rate_pend, sizeof pend[0] * n);
    rate_npend = 0;
    portEXIT_CRITICAL(&rate_lock);
    for (int i = 0; i < n; i++)
        rdof_rate_point(&rate_fit, pend[i][0], pend[i][1]);
    settle_tick(t);
    double skew = 0;
    if (!netclock_rate(&board_ppm, &skew, &board_learned)) {
        board_ppm = NAN; /* the board's own crystal error isn't known: neither is the dongle's */
        xtal_trusted = false;
        return;
    }
    if (!rate_fit.valid || rate_fit.count < RDOF_RATE_MIN_POINTS) {
        xtal_trusted = false;
        return;
    }
    /* the samples' rate in true time (a local second is 1 + board ppm true ones), then the crystal's: what the
     * correction takes out of the sample rate, added back */
    xtal = rate_fit.ppm - board_ppm + rate_corr;
    const double board_err = board_learned ? skew : RATE_STORED_ERR_PPM;
    xtal_err = sqrt(rate_fit.se_ppm * rate_fit.se_ppm + board_err * board_err);
    xtal_trusted = settled && xtal_err <= RATE_TRUST_PPM && rate_fit.span_s >= RATE_TRUST_SPAN_S;
    xtal_ever |= xtal_trusted;
}

/* The crystal correction on the chip, kept in fe_ppm. 0 or < 0 (the front end's error says why). */
static int set_chip_ppm(int ppm)
{
    if (fe.ops->set_ppm(fe.ctx, ppm))
        return -1;
    fe_ppm = ppm;
    if (fe.ops->sample_ppm && fe.ops->sample_ppm(fe.ctx) != rate_corr)
        rate_restart(true); /* the correction moved the sample rate too: time it afresh */
    return 0;
}

/* ── the tuner's crystal error, learned (rdof/sdr/ppm.h) and kept in NVS ─────── */

#define PPM_NS "rdof_ppm"
#define PPM_EVERY_US 5000000   /* a look at the AFC every 5 s */
#define CAL_FOLLOW_PPM 0.25    /* with nothing learned of its own, follow the table when it says this different */
#define CAL_AGAIN_S 7200.0     /* the same temperature bin learned again in one run at most this often */

/* "learned", from before the table: one value, its temperature unknown. Read when there's no table yet. */
typedef struct {
    char serial[64]; /* the dongle it was learned with ("sim" for the simulated tuner) */
    float ppm;
} ppm_rec_t;

/* "cal": the dongle's error against the die's temperature (netclock_tempcal.h) */
typedef struct {
    char serial[64];
    nc_tcal_t cal;
} cal_rec_t;

static rdof_ppm_t ppm;
static bool ppm_on, ppm_inited;
static char ppm_serial[64];
static int64_t ppm_last_us;
static nc_tcal_t cal;
static double ppm_assumed = NAN; /* what the chip and the channels' corrections assume, before any estimate */
static int cal_put_bin = INT32_MIN;
static double cal_put_t;

static double ppm_load(const char *serial)
{
    nvs_handle_t h;
    ppm_rec_t r;
    size_t len = sizeof r;
    double v = NAN;
    if (nvs_open(PPM_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, "learned", &r, &len) == ESP_OK && len == sizeof r && !strncmp(r.serial, serial, sizeof r.serial))
            v = r.ppm;
        nvs_close(h);
    }
    return v;
}

static void cal_load(const char *serial)
{
    static cal_rec_t r; /* not on the DSP task's stack */
    nvs_handle_t h;
    size_t len = sizeof r;
    nc_tcal_init(&cal);
    if (nvs_open(PPM_NS, NVS_READONLY, &h) != ESP_OK)
        return;
    if (nvs_get_blob(h, "cal", &r, &len) == ESP_OK && len == sizeof r && !strncmp(r.serial, serial, sizeof r.serial) &&
        nc_tcal_valid(&r.cal))
        cal = r.cal;
    nvs_close(h);
}

static void cal_store(const char *serial)
{
    static cal_rec_t r;
    nvs_handle_t h;
    memset(&r, 0, sizeof r);
    snprintf(r.serial, sizeof r.serial, "%s", serial);
    r.cal = cal;
    if (nvs_open(PPM_NS, NVS_READWRITE, &h) != ESP_OK)
        return;
    if (nvs_set_blob(h, "cal", &r, sizeof r) != ESP_OK || nvs_commit(h) != ESP_OK)
        ESP_LOGW(TAG, "ppm: can't save the table");
    nvs_close(h);
}

/* Each channel's correction moved by +f * step * 1e-6: moving the RTL's correction up by step ppm raises every
 * signal by that much. */
static void ppm_shift_channels(double step)
{
    for (int c = 0; c < g_rx.C; c++)
        rdof_sdr_rx_set_correction(&g_rx, c, g_rx.chz.correction_hz[c] + rx_freq[c] * step * 1e-6);
}

/* Start learning for this front end (the dongle, the simulated one, the chip): the chip at the whole ppm of what
 * the table expects at the die's temperature now (else the value kept from before the table, else the setting's),
 * the rest in the channels' corrections. Under rx_lock. */
static void ppm_start(const char *serial)
{
    ppm_on = g_scfg.learn_ppm;
    if (ppm_inited && !strcmp(serial, ppm_serial)) {
        /* the same dongle again (a stream restart, which tuned it to the setting): back where learning left it */
        const int chip_ppm = (int)ppm.chip;
        if (chip_ppm != fe_ppm && set_chip_ppm(chip_ppm))
            ESP_LOGW(TAG, "ppm: can't set the %s to %d: %s", chip(), chip_ppm, fe.ops->error(fe.ctx));
        return;
    }
    int chip_ppm = (int)lrint(g_scfg.ppm);
    double kept = NAN;
    const double die = netclock_die_c();
    nc_tcal_init(&cal);
    cal_put_bin = INT32_MIN;
    if (ppm_on) {
        cal_load(serial);
        if (!nc_tcal_get(&cal, die, &kept))
            kept = ppm_load(serial);
        if (!isnan(kept))
            chip_ppm = (int)lrint(kept);
    }
    if (chip_ppm != fe_ppm && set_chip_ppm(chip_ppm)) {
        ESP_LOGW(TAG, "ppm: can't set the %s to %d: %s", chip(), chip_ppm, fe.ops->error(fe.ctx));
        chip_ppm = fe_ppm;
    }
    snprintf(ppm_serial, sizeof ppm_serial, "%s", serial);
    rdof_ppm_init(&ppm, chip_ppm, kept);
    ppm_inited = true;
    ppm_assumed = isnan(kept) ? chip_ppm : kept;
    if (!isnan(kept))
        ppm_shift_channels(-(kept - chip_ppm)); /* the fraction the chip can't take, into the corrections */
    ESP_LOGI(TAG, "ppm: %s; %s at %d ppm%s", serial, chip(), chip_ppm,
             !ppm_on ? ", learning off" : isnan(kept) ? ", nothing kept yet: learning" : "");
    if (cal.count)
        ESP_LOGI(TAG, "ppm: the table (%d temperatures) expects %.2f at %.1f C", cal.count, kept, die);
    else if (!isnan(kept))
        ESP_LOGI(TAG, "ppm: kept %.2f from before (no table yet)", kept);
}

/* With nothing learned of its own yet (no AFC estimate held, the sample clock not trusted), the chip and channels
 * follow what the table expects as the room warms or cools. Only once settled: the table is the board's die
 * against the dongle's settled error, and the dongle has no thermometer of its own, so while either warms (a
 * dongle just plugged in starts cold, whatever the die reads) the table can't say where its crystal is. Under
 * rx_lock. */
static void cal_follow(void)
{
    double want;
    if (!settled || xtal_trusted || ppm.steady_s >= RDOF_PPM_APPLY_S || !nc_tcal_get(&cal, die_c, &want))
        return;
    const double now = isnan(rdof_ppm_learned(&ppm)) ? ppm_assumed : rdof_ppm_learned(&ppm);
    if (isnan(now) || fabs(want - now) < CAL_FOLLOW_PPM)
        return;
    const int to = (int)lrint(want), step = to - (int)ppm.chip;
    if (step) {
        if (set_chip_ppm(to)) {
            ESP_LOGW(TAG, "ppm: can't move the %s to %d: %s", chip(), to, fe.ops->error(fe.ctx));
            return;
        }
        ppm_shift_channels(step);
        rdof_ppm_applied(&ppm, step);
    }
    ppm_shift_channels(-(want - now)); /* the signals where the table expects them */
    if (!isnan(ppm.residual))
        ppm.residual = want - ppm.chip;
    ppm_assumed = want;
    ESP_LOGI(TAG, "ppm: at %.1f C the table expects %.2f: %s at %d ppm", die_c, want, chip(), (int)ppm.chip);
}

/* Settled and confident: the learned value into the table at the die's temperature, once a bin each run (again
 * after CAL_AGAIN_S), and saved. Under rx_lock. */
static void cal_learn(double t)
{
    const double v = rdof_ppm_learned(&ppm);
    if (!settled || isnan(v) || isnan(die_c) || ppm.steady_s < RDOF_PPM_FIRST_SAVE_S)
        return;
    const int bin = nc_tcal_bin(die_c);
    if (bin == cal_put_bin && t - cal_put_t < CAL_AGAIN_S)
        return;
    nc_tcal_put(&cal, die_c, v);
    cal_put_bin = bin;
    cal_put_t = t;
    cal_store(ppm_serial);
    rdof_ppm_saved(&ppm, t);
    const nc_tcal_entry_t *e = nc_tcal_find(&cal, die_c);
    ESP_LOGI(TAG, "ppm: learned %.2f at %.1f C (%s): the %d C bin is %.2f over %d, %d temperatures in the table", v,
             die_c, ppm.measured ? "sample clock" : "AFC", bin, e ? e->v : NAN, e ? e->n : 0, cal.count);
}

/* Every few seconds, under rx_lock: learn, move the RTL when confident, follow the temperature table until then,
 * and put a settled value into it (cal_learn). */
static void ppm_tick(void)
{
    const int64_t now = esp_timer_get_time();
    if (now - ppm_last_us < PPM_EVERY_US)
        return;
    ppm_last_us = now;
    const double t = now / 1e6;
    if (!ppm_on) {
        rate_tick(t); /* still timed, for the page */
        return;
    }
    double corr[RDOF_SDR_MAX_CHANNELS], age[RDOF_SDR_MAX_CHANNELS];
    for (int c = 0; c < g_rx.C; c++) {
        corr[c] = g_rx.chz.correction_hz[c];
        age[c] = g_rx.last_pos[c] > 0 ? (double)(g_rx.out_index - g_rx.last_pos[c]) / RDOF_DEMOD_FS : -1.0;
    }
    rate_tick(t);
    /* the sample clock, when it's good enough, else the AFC; once it has been, hold while it times afresh (after a
     * move) rather than go back to the AFC, which has the radios' own errors in it */
    int step = xtal_trusted ? rdof_ppm_measured(&ppm, xtal, t)
               : xtal_ever  ? 0
                            : rdof_ppm_update(&ppm, corr, rx_freq, age, g_rx.C, t);
    if (step) {
        const int to = (int)ppm.chip + step;
        if (set_chip_ppm(to)) {
            ESP_LOGW(TAG, "ppm: can't move the %s to %d: %s", chip(), to, fe.ops->error(fe.ctx));
        } else {
            ppm_shift_channels(step);
            rdof_ppm_applied(&ppm, step);
            if (ppm.measured)
                ESP_LOGI(TAG, "ppm: the tuner is %.2f ppm off (its sample clock, +-%.2f): %s moved to %d ppm",
                         rdof_ppm_learned(&ppm), xtal_err, chip(), to);
            else
                ESP_LOGI(TAG, "ppm: the tuner is %.2f ppm off (%d channels agree): %s moved to %d ppm",
                         rdof_ppm_learned(&ppm), ppm.channels, chip(), to);
        }
    }
    cal_follow();
    cal_learn(t); /* only once settled: learned while warming, it holds only for that moment */
}

void sdr_ppm_status(sdr_ppm_status_t *out)
{
    rx_lock();
    out->on = ppm_on;
    out->chip = ppm.chip;
    out->learned = rdof_ppm_learned(&ppm);
    out->channels = ppm.channels;
    out->steady_s = ppm.steady_s;
    out->saved = ppm.has_saved ? ppm.saved : NAN;
    out->sim_ppm = g_scfg.sim ? g_scfg.sim_ppm : NAN;
    out->measured = ppm.measured;
    out->clock_on = fe.ops && fe.ops->sample_ppm;
    out->clock_ppm = rate_fit.valid ? xtal : NAN;
    out->clock_err = rate_fit.valid ? xtal_err : NAN;
    out->clock_trusted = xtal_trusted;
    out->clock_raw = rate_fit.valid ? rate_fit.ppm : NAN;
    out->clock_span_s = rate_fit.span_s;
    out->clock_rms_us = rate_fit.rms_us;
    out->clock_steps = rate_fit.steps;
    out->board_ppm = board_ppm;
    out->board_learned = board_learned;
    out->settled = settled;
    out->cal = cal;
    if (!nc_tcal_get(&cal, die_c, &out->cal_ppm))
        out->cal_ppm = NAN;
    out->die_c = die_c;
    const double t = esp_timer_get_time() / 1e6;
    out->die_spread = nc_settle_range(&die_settle, t);
    out->rate_spread = nc_settle_range(&rate_settle, t);
    out->die_tol = SETTLE_DIE_C;
    out->rate_tol = SETTLE_RATE_PPM;
    rx_unlock();
}

/* ── one chunk ──────────────────────────────────────────────────────────── */

static void spectrum_feed(const rdof_cf_t *iq, int n)
{
    if (spec_failed || esp_timer_get_time() > spec_want_until)
        return;
    if (!spec_rows) {
        /* a row about every 200 ms: one FFT a chunk, averaged */
        int per_row = (int)lrint(0.2 * g_scfg.sample_rate / CHUNK);
        spec_rows = heap_caps_calloc(SPEC_ROWS, SPEC_BINS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        /* in PSRAM: once the hot arena is full its FFT plan took 4 KB of internal RAM, which the W12 can't spare */
        sdr_alloc_cold(true);
        int bad = !spec_rows || rdof_spectrum_init(&spec, SPEC_BINS, per_row);
        sdr_alloc_cold(false);
        if (!bad) /* one FFT a chunk: four took 13% of the DSP's core with the page open (overflows on the W12) */
            rdof_spectrum_set_ffts(&spec, 1);
        if (bad) {
            ESP_LOGE(TAG, "no memory for the spectrum");
            spec_failed = true;
            free(spec_rows);
            spec_rows = NULL;
            return;
        }
        spec_row_ms = (int)lrint(1000.0 * spec.per_row * CHUNK / g_scfg.sample_rate);
    }
    uint8_t row[SPEC_BINS];
    if (!rdof_spectrum_feed(&spec, iq, n, row))
        return;
    portENTER_CRITICAL(&spec_lock);
    memcpy(spec_rows + (size_t)((spec_seq + 1) % SPEC_ROWS) * SPEC_BINS, row, SPEC_BINS);
    spec_sc[(spec_seq + 1) % SPEC_ROWS] = g_rx_ready ? g_rx.samples + n : 0;
    spec_seq++;
    portEXIT_CRITICAL(&spec_lock);
}

int sdr_spectrum_rows(uint32_t since, uint8_t *out, int max, uint32_t *seq, sdr_spectrum_info_t *info, long long *sc)
{
    spec_want_until = esp_timer_get_time() + 10 * 1000000LL;
    info->bins = SPEC_BINS;
    info->row_ms = spec_row_ms;
    info->rate = g_scfg.sample_rate;
    info->center_hz = g_sstat.center_hz;
    if (!spec_rows) {
        *seq = 0;
        return 0;
    }
    portENTER_CRITICAL(&spec_lock);
    uint32_t last = spec_seq, from = since > last ? 0 : since; /* ahead of us: the board restarted */
    if (max > SPEC_ROWS)
        max = SPEC_ROWS;
    if (last - from > (uint32_t)max)
        from = last - (uint32_t)max;
    int k = 0;
    for (uint32_t q = from + 1; q <= last && k < max; q++, k++) {
        memcpy(out + (size_t)k * SPEC_BINS, spec_rows + (size_t)(q % SPEC_ROWS) * SPEC_BINS, SPEC_BINS);
        if (sc)
            sc[k] = spec_sc[q % SPEC_ROWS];
    }
    portEXIT_CRITICAL(&spec_lock);
    *seq = last;
    return k;
}

/* filler: the chunk has filler where the front end lost data (its level says nothing about the gain) */
static void chunk(const uint8_t *raw, int n, int split, bool *first, bool filler)
{
    int64_t t0 = esp_timer_get_time();
    double peak, clipped;
    if (fe.caps->format == RDOF_FE_CU8) {
        rdof_agc_measure_cu8(raw, (size_t)n, &peak, &clipped);
    } else {
        rdof_fe_convert(fe.caps->format, raw, (size_t)n, iq);
        rdof_agc_measure(iq, (size_t)n, &peak, &clipped);
    }
    double new_gain;
    portENTER_CRITICAL(&level_lock);
    rdof_level_stats_add(&levels, (size_t)n, peak, clipped);
#if RDOF_FEATURE_SDR_AGC
    bool change = agc_on && !filler && rdof_agc_update(&agc, peak, clipped, t0 / 1e6, &new_gain);
#else
    bool change = false;
    new_gain = 0;
#endif
    portEXIT_CRITICAL(&level_lock);
    if (fe.caps->format == RDOF_FE_CU8)
        rdof_iq_from_cu8(raw, (size_t)n, iq);
    if (fe.ops->post)
        fe.ops->post(fe.ctx, iq, n);
    if (scale != 1.0f)
        for (int i = 0; i < n; i++) {
            iq[i].re *= scale;
            iq[i].im *= scale;
        }
    if (change) {
        double was = gain_db, got;
        if (fe.ops->set_gain(fe.ctx, new_gain, &got) == 0)
            gain_db = got;
        g_sstat.gain_db = gain_db;
        scale = (float)pow(10.0, (ref_gain - gain_db) / 20.0);
        ESP_LOGI(TAG, "AGC: gain %.1f -> %.1f dB (%s; ceiling %.1f dB)", was, gain_db,
                 new_gain < was ? "clipping" : "headroom", (double)AGC_FIELD(ceiling));
    }
    spectrum_feed(iq, n);
    stage_us[SDR_STAGE_INPUT] += esp_timer_get_time() - t0;
    rx_lock();
    /* Re-anchor stream time if it has drifted from the clock (dropped samples, NTP arriving): 1 s at
     * once. Once netclock is synced, also 100 ms, from the least lag over 2 s: netclock slews in what its first
     * reference says (a soft restart keeps the RTC's time, often a fraction of a second off), which never
     * reaches 1 s, and the crystal drifts. A chunk handled late (USB, a backlog) only adds lag, so the least is
     * the truest. */
    static double least;
    static int64_t least_from;
    if (*first) {
        g_rx.t0 = now_unix() - n / g_scfg.sample_rate;
        *first = false;
        least_from = 0;
    } else {
        /* at this chunk's start, as the first anchor */
        const double lag = now_unix() - n / g_scfg.sample_rate - (g_rx.t0 + g_rx.out_index / RDOF_DEMOD_FS);
        double move = fabs(lag) > 1.0 ? lag : 0;
        if (!move && netclock_synced()) {
            if (!least_from || lag < least)
                least = lag;
            if (!least_from)
                least_from = t0;
            else if (t0 - least_from >= 2000000) {
                move = fabs(least) > 0.1 ? least : 0;
                least_from = 0;
            }
        }
        if (move) {
            /* a front end that loses samples (the DSP behind, the sim dropping chunks) re-anchors every few
             * seconds: a line a minute at most, so the log ring keeps the rest */
            static int64_t logged_at;
            static int quiet;
            static double quiet_s;
            if (!logged_at || t0 - logged_at >= 60000000) {
                if (quiet)
                    ESP_LOGW(TAG, "stream time off by %.2f s; re-anchoring (and %d more times, %.2f s in all, "
                             "since the last line)", move, quiet, quiet_s);
                else
                    ESP_LOGW(TAG, "stream time off by %.2f s; re-anchoring", move);
                logged_at = t0;
                quiet = 0;
                quiet_s = 0;
            } else {
                quiet++;
                quiet_s += move;
            }
            g_rx.t0 += move;
            least_from = 0;
        }
    }
    slot_timing_rx(&g_rx);
    sdr_process_chunk(&g_rx, iq, n, split);
    ppm_tick();
    rx_unlock();
    int64_t t1 = esp_timer_get_time();
    busy_us += t1 - t0;
    if (t1 - last_wait_us > 100000) { /* hasn't blocked for 100 ms: falling behind; a tick for the idle task */
        vTaskDelay(1);
        last_wait_us = esp_timer_get_time();
    }
}

/* ── the loop ───────────────────────────────────────────────────────────── */

/* The DSP task, between runs: the receiver rebuilt for the asked retune (or the settings again); a retune that
 * fails leaves the one before. */
static void apply_retune(void)
{
    const double oc = rt_center, og = rt_gain;
    const int on = ntaps;
    rdof_sdr_cfg_channel_t ot[SDR_MAX_TAPS];
    memcpy(ot, taps, sizeof ot);
    static const sdr_retune_t restore = {0, SDR_RETUNE_SETTINGS, {0}, 0};
    const bool back = rt_restore;
    const sdr_retune_t *q = back ? &restore : &rt_req; /* a restore first: a request is the next lease's */
    rt_restore = false;
    if (back && isnan(rt_center) && isnan(rt_gain) && !ntaps)
        return; /* nothing to undo */
    rt_why[0] = 0;
    rx_lock();
    g_rx_ready = false;
    rdof_sdr_rx_free(&g_rx);
#if CONFIG_RDOF_SDR_TWO_CORES
    bal.s = -1; /* the channel count may differ: the split starts over */
#endif
    rt_center = q->center_hz == 0 ? NAN : isnan(q->center_hz) ? oc : q->center_hz;
    rt_gain = q->gain_db == SDR_RETUNE_SETTINGS ? NAN : isnan(q->gain_db) ? og : q->gain_db;
    if (q->ntaps >= 0) {
        ntaps = q->ntaps;
        memset(taps, 0, sizeof taps);
        for (int i = 0; i < ntaps; i++)
            taps[i].freq = q->taps_hz[i];
    }
    if (!rx_setup()) {
        char why[sizeof rt_why];
        snprintf(why, sizeof why, "%s", rt_why);
        ESP_LOGW(TAG, "retune refused: %s", why);
        rt_center = oc;
        rt_gain = og;
        ntaps = on;
        memcpy(taps, ot, sizeof taps);
        rdof_sdr_rx_free(&g_rx);
        rx_setup();
        snprintf(rt_why, sizeof rt_why, "%s", why[0] ? why : "the receiver didn't start");
    } else
        ESP_LOGI(TAG, "retuned: center %s, gain %s, %d tap%s", isnan(rt_center) ? "the settings'" : "set",
                 isnan(rt_gain) ? "the settings'" : "set", live_taps, live_taps == 1 ? "" : "s");
    rx_unlock();
    if (!back)
        rt_state = 0;
}

void sdr_retune_restore(void)
{
    rt_restore = true;
}

const char *sdr_retune(const sdr_retune_t *r, bool wait)
{
    if (!dsp_task_handle)
        return "no SDR running";
    if (rt_state)
        return "a retune is under way";
    rt_req = *r;
    rt_state = 1;
    for (int i = 0; wait && rt_state && i < 300; i++)
        vTaskDelay(pdMS_TO_TICKS(20));
    if (!wait)
        return NULL;
    if (rt_state)
        return "the SDR didn't come back in 6 s";
    return rt_why[0] ? rt_why : NULL;
}

static volatile bool paused; /* sdr_pause: the front end stopped, the DSP idle */

bool sdr_pause(bool on)
{
    paused = on;
    if (!on || !dsp_task_handle)
        return true;
    for (int i = 0; fe_streaming && i < 150; i++) /* the read's 500 ms timeout at most, then the stop */
        vTaskDelay(pdMS_TO_TICKS(20));
    return !fe_streaming;
}

static void dsp_task(void *arg)
{
    const size_t want = (size_t)CHUNK * fe_bytes;
    uint8_t *raw = raw_buf; /* sdr_start's, with iq */
    if (!rx_setup()) {
        dsp_task_handle = NULL; /* the status reads its stack: never a deleted task's */
        vTaskDelete(NULL);
    }
    const int split = g_scfg.dsp_cores > 1 ? SDR_SPLIT_AUTO : 0;
    for (;;) {
        if (paused) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        while (rt_state == 1 || rt_restore)
            apply_retune();
        if (!fe.ops->wait(fe.ctx, 1000))
            continue;
        if (g_scfg.spike && fe.ops->spike) {
            fe.ops->spike(fe.ctx);
            g_scfg.spike = false;
        }
        if (tune()) {
            rx_lock();
            const char *id = fe.ops->identity(fe.ctx);
            const bool same = ppm_inited && !strcmp(id, ppm_serial); /* a stream restart: it's as warm as it was */
            ppm_start(id);
            rate_restart(!same);
            rx_unlock();
        } else {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        if (fe.ops->start(fe.ctx)) {
            ESP_LOGE(TAG, "%s", fe.ops->error(fe.ctx));
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        g_sstat.receiving = true;
        fe_streaming = true;
        bool first = true;
        int64_t stream_samples = 0; /* UTC slot timing: the stream's sample count, stamped as it arrives */
        slot_timing_stream(g_scfg.sample_rate, g_rx.out_index);
        wall_from_us = esp_timer_get_time();
        busy_us = 0;
        int64_t last_data = esp_timer_get_time();
        bool stalled = false;
        static bool stacks_logged;
        while (!fe.ops->gone(fe.ctx) && rt_state != 1 && !rt_restore && !paused) {
            int64_t asked = esp_timer_get_time();
            size_t filled = 0;
            size_t got = fe.ops->read(fe.ctx, raw, want, 500, &filled);
            int64_t now = esp_timer_get_time();
            if (now - asked > 1000)
                last_wait_us = now;
            if (got < fe_bytes) {
                /* a front end that stops sending (the RTL's FIFO wedged, a halted endpoint, every transfer failed)
                 * is stopped, retuned and started again */
                if (now - last_data > STALL_US) {
                    stalled = true;
                    break;
                }
                continue;
            }
            last_data = now;
            stream_samples += (int64_t)(got / fe_bytes);
            slot_timing_arrived(stream_samples, now);
            chunk(raw, (int)(got / fe_bytes), split, &first, filled > 0);
            if (!stacks_logged && now - wall_from_us > 60000000) { /* a minute in: how much stack each used */
                int d, h;
                sdr_dsp_stacks(&d, &h);
                ESP_LOGI(TAG, "stacks: dsp %d of 8192 bytes free, dsp_helper %d of %d free", d, h,
                         CONFIG_RDOF_SDR_TWO_CORES ? CONFIG_RDOF_SDR_HELPER_STACK : 0);
                stacks_logged = true;
            }
        }
        fe.ops->stop(fe.ctx);
        g_sstat.receiving = false;
        fe_streaming = false;
        if (stalled) {
            rdof_fe_stats_t u;
            sdr_fe_stats(&u);
            g_sstat.stream_restarts++;
            snprintf(g_sstat.rtl_error, sizeof g_sstat.rtl_error, "no samples for %d s (%lu %s errors): restarted",
                     (int)(STALL_US / 1000000), (unsigned long)u.errors,
                     g_scfg.frontend == RDOF_FE_LR2021 ? "SPI" : "USB");
            ESP_LOGW(TAG, "%s", g_sstat.rtl_error);
            continue;
        }
        if (rt_state == 1 || rt_restore || paused)
            continue; /* the retune, then a new run; or idle while paused */
        ESP_LOGW(TAG, "the %s is gone: waiting for it", fe.caps->kind);
    }
}

bool sdr_tx_frontend(rdof_fe_t *out, double *rate, double *center)
{
    if (!fe_streaming || !g_rx_ready)
        return false;
    *out = fe;
    *rate = g_scfg.sample_rate;
    *center = g_rx.center_hz;
    return true;
}

int sdr_rx_channel(uint32_t hz)
{
    for (int i = 0; g_rx_ready && i < g_rx.C; i++)
        if (fabs(rx_freq[i] - hz) <= 100)
            return i;
    return -1;
}

bool sdr_fe_pick(int type, rdof_fe_t *out, const char **why)
{
    *why = NULL;
    switch (type) {
    case RDOF_FE_RTLSDR: fe_rtlsdr(out); return true;
    case RDOF_FE_SIM: fe_sim(out); return true;
#if CONFIG_RDOF_SDR_LR2021
    case RDOF_FE_LR2021: fe_lr2021(out); return true;
#endif
    default:
        *why = type == RDOF_FE_LR2021 ? "this build has no LR2021 driver (CONFIG_RDOF_SDR_LR2021)"
                                    : "this build has no driver for that front end";
        return false;
    }
}

/* What the receiver will take of the hot arena: built once in a dry run, its hot allocations counted (and made in
 * PSRAM, so the internal heap the plan measures isn't touched). Channels the settings name; a retune's taps past
 * them take the heap, as before. */
static size_t hot_dry_run(void)
{
    rdof_sdr_rx_t *rx = heap_caps_calloc(1, sizeof *rx, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rx)
        return 0;
    rdof_sdr_channel_t ch[RDOF_SDR_MAX_CHANNELS];
    for (int i = 0; i < g_scfg.nch; i++)
        ch[i] = (rdof_sdr_channel_t){.id = g_scfg.ch[i].id, .freq_hz = g_scfg.ch[i].freq, .cc = g_scfg.ch[i].cc,
                                     .slot = g_scfg.ch[i].slot};
    const double center = g_scfg.center ? g_scfg.center : g_scfg.ch[0].freq;
    rdof_sdr_rx_config_t rc = {g_scfg.sample_rate, center, 0.0, 0.0, g_scfg.afc, CHUNK};
    sdr_hot_count(true);
    if (!rdof_sdr_rx_init(rx, &rc, ch, g_scfg.nch, NULL, 0.0) && g_scfg.dsp_cores > 1)
        rdof_chz_reserve(&rx->chz, CHUNK);
    rdof_sdr_rx_free(rx);
    sdr_hot_count(false);
    heap_caps_free(rx);
    return sdr_hot_counted();
}

static size_t hot_result;

static void hot_task(void *arg)
{
    hot_result = hot_dry_run();
    xTaskNotifyGive((TaskHandle_t)arg);
    vTaskDelete(NULL);
}

/* The dry run builds a receiver, which needs the DSP task's stack: on the main task's 6 KB it overflowed (W12,
 * 2026-10-24). So it runs in a task of its own, with the DSP task's stack size, gone before the plan is solved. */
static size_t hot_measure(void)
{
    if (!g_scfg.nch)
        return 0;
    hot_result = 0;
    if (xTaskCreatePinnedToCore(hot_task, "sdr_dry", 8192, xTaskGetCurrentTaskHandle(), uxTaskPriorityGet(NULL), NULL,
                                tskNO_AFFINITY) != pdPASS)
        return 0;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    vTaskDelay(2); /* the idle task frees its stack */
    return hot_result;
}

/* The SDR's memory plan (mem.c, docs/spec/fw/memory.md). Role "sdr", the DSP: the chunk buffers (iq is 32 KB in one
 * piece; taken after Wi-Fi, with two DSP cores at 576 kS/s, it didn't fit: 98 KB free, 31 KB in one piece), the hot
 * arena, the DSP tasks' stacks. Role "sdr_net": its MQTT and HBlink4 tasks. Role "sdr_fe": the front end's. Each
 * internal: PSRAM is several times slower for the DSP. True if the front end is on USB. */
bool sdr_mem_claims(void)
{
    rdof_fe_t f;
    const char *why;
    if (!sdr_fe_pick(g_scfg.frontend, &f, &why))
        return false;
    dsp_setup(); /* esp-dsp's tables now, while the heap is whole; and the allocators the dry run counts with */
    const size_t fb = (size_t)rdof_fe_sample_bytes(f.caps->format);
    mem_add("sdr", "iq", sizeof(rdof_cf_t) * CHUNK, 16, RDOF_MEM_INTERNAL, RDOF_MEM_ALLOC);
    mem_add("sdr", "raw", CHUNK * fb, 16, RDOF_MEM_INTERNAL, RDOF_MEM_ALLOC);
    const size_t hot = hot_measure();
    if (hot)
        mem_add("sdr", "hot", hot, 16, RDOF_MEM_INTERNAL | RDOF_MEM_FALLBACK, RDOF_MEM_ALLOC); /* PSRAM when short: the W12's
                                                                                         LR2021 then still opens */
    mem_add_task("sdr", "dsp", 8192, RDOF_MEM_INTERNAL);
#if CONFIG_RDOF_SDR_TWO_CORES
    if (g_scfg.dsp_cores > 1 && !helper) /* the boot self-test may have started it already */
        mem_add_task("sdr", "dsp_helper", CONFIG_RDOF_SDR_HELPER_STACK, RDOF_MEM_INTERNAL);
#endif
    sdr_net_mem_claims();
    switch (g_scfg.frontend) {
#if CONFIG_RDOF_SDR_LR2021
    case RDOF_FE_LR2021: fe_lr2021_mem_claims(); return false;
#endif
    case RDOF_FE_RTLSDR: fe_rtlsdr_mem_claims(); return true;
    default: return false;
    }
}

/* The SDR's network side: its MQTT status task and client, its HBlink4 peers' task when a channel has a peer. */
void sdr_net_mem_claims(void)
{
    mem_add_task("sdr_net", "sdr_status", 4096, RDOF_MEM_INTERNAL);
    mem_add("sdr_net", "mqtt", MQTT_CLIENT_INTERNAL, 4, RDOF_MEM_INTERNAL, RDOF_MEM_BUDGET);
#if CONFIG_RDOF_SDR_HBLINK
    bool peers = false;
    for (int c = 0; g_scfg.hb_enabled && c < g_scfg.nch; c++)
        peers |= g_scfg.ch[c].hblink && g_scfg.ch[c].peer_id >= 0;
    if (peers)
        mem_add_task("sdr_net", "sdr_hblink", 4096, RDOF_MEM_INTERNAL);
#endif
}

/* The chunk buffers and the hot arena: the plan's, or, not planned (an "either" board), the heap's. */
static void take_buffers(rdof_fe_t *f)
{
    if (raw_buf)
        return;
    fe_bytes = (size_t)rdof_fe_sample_bytes(f->caps->format);
    size_t n = 0;
    iq = mem_take("sdr", "iq", NULL);
    raw_buf = mem_take("sdr", "raw", NULL);
    void *hot = mem_take("sdr", "hot", &n);
    if (hot)
        sdr_hot_arena(hot, n);
    const bool planned = iq && raw_buf;
    if (!iq)
        iq = heap_caps_malloc(sizeof(rdof_cf_t) * CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!raw_buf)
        raw_buf = heap_caps_malloc((size_t)CHUNK * fe_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "DSP buffers: %u bytes internal%s, hot arena %u bytes%s; internal RAM free %u KB, %u KB in one piece",
             (unsigned)(sizeof(rdof_cf_t) * CHUNK + CHUNK * fe_bytes), planned ? " (planned)" : " (from the heap)",
             (unsigned)n, raw_buf && iq ? "" : " (DIDN'T FIT)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024);
}

/* The front end didn't start: nothing runs on the DSP's memory yet, so it goes back to the heap (and the network). */
static void release_dsp(void)
{
    if (mem_role_state("sdr") != 1)
        return;
    iq = NULL;
    raw_buf = NULL;
    sdr_hot_arena(NULL, 0);
    mem_release("sdr");
}

void sdr_start(void)
{
    rx_mutex = xSemaphoreCreateMutex();
    rdof_fe_t f;
    const char *why;
    if (!sdr_fe_pick(g_scfg.frontend, &f, &why)) {
        ESP_LOGE(TAG, "device.type %s: %s", rdof_fe_type_name(g_scfg.frontend), why);
        return;
    }
    if (mem_role_state("sdr") < 0 || mem_role_state("sdr_fe") < 0) {
        snprintf(g_sstat.rtl_error, sizeof g_sstat.rtl_error, "memory plan: %s",
                 mem_role_state("sdr") < 0 ? mem_role_why("sdr") : mem_role_why("sdr_fe"));
        ESP_LOGE(TAG, "not started: %s", g_sstat.rtl_error);
        return;
    }
    take_buffers(&f);
    if (!raw_buf || !iq) {
        ESP_LOGE(TAG, "no memory for the DSP buffers (%u bytes internal free, %u in one piece): no SDR",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return;
    }
    if (f.ops->init(f.ctx)) {
        ESP_LOGE(TAG, "%s", f.ops->error(f.ctx));
        release_dsp();
        return;
    }
    fe = f;
    mem_task("sdr", "dsp", dsp_task, 8192, NULL, 12, DSP_CORE, &dsp_task_handle);
}
