// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The SDR role: an RTL-SDR on the USB host and the SDR receive path on 4
 * or 5 channels at 288 kS/s, published to MQTT and HBlink4. Nothing of it is allocated until it's started: the sample ring
 * (PSRAM), the DSP buffers (internal RAM while some stays free, else PSRAM)
 * and esp-dsp's tables.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "rdof/sdr/channelizer.h"

static const char *TAG = "sdr";

sdr_status_t g_sstat = {.gain_db = NAN, .dsp_split = -1};

/* sdr's buffers in internal RAM (PSRAM is several times slower for the DSP) while
   CONFIG_RDOF_SDR_INTERNAL_RESERVE_KB stays free for the network and the roamer, else in PSRAM. */
size_t g_dsp_internal_bytes, g_dsp_psram_bytes; /* what fast_alloc has handed out (not net of frees) */

/* The hot arena: the memory plan's "hot" claim (mem.c), the receiver's buffers touched for every sample and
 * esp-dsp's plans and filter states, taken at boot before Wi-Fi carves the heap up. Only while the DSP task builds
 * a receiver (sdr_hot_arena_open to _close) does anything come from it, and each receiver starts it over: what's in
 * it is the receiver's alone (the page's spectrum, made later, isn't). A free inside it is nothing. Past its end,
 * the heap as before. */
static uint8_t *arena;
static size_t arena_cap, arena_used, arena_peak;
static TaskHandle_t arena_owner; /* NULL: closed */
/* sdr_hot_count: a dry run measures what the receiver will take, in PSRAM so the internal heap isn't touched */
static bool counting;
static size_t counted;
/* sdr_alloc_cold: what isn't on the receive path (the display spectrum) goes in PSRAM */
static bool cold;

#define HOT_ALIGN 16 /* the S3's SIMD loads */

void sdr_hot_arena(void *base, size_t cap)
{
    arena = base;
    arena_cap = base ? cap : 0;
    arena_used = 0;
}

void sdr_hot_arena_open(void)
{
    arena_used = 0;
    arena_owner = xTaskGetCurrentTaskHandle();
}

void sdr_hot_arena_close(void) { arena_owner = NULL; }

void sdr_hot_arena_use(size_t *used, size_t *peak, size_t *cap)
{
    *used = arena_used;
    *peak = arena_peak;
    *cap = arena_cap;
}

void sdr_hot_count(bool on)
{
    counting = on;
    if (on)
        counted = 0;
}

size_t sdr_hot_counted(void) { return counted; }

void sdr_alloc_cold(bool on) { cold = on; }

static bool in_arena(const void *p) { return arena && (const uint8_t *)p >= arena && (const uint8_t *)p < arena + arena_cap; }

void *sdr_hot_aligned(size_t align, size_t n, bool zero)
{
    if (align < HOT_ALIGN)
        align = HOT_ALIGN;
    if (counting) {
        counted += (n + align - 1) / align * align;
        return heap_caps_aligned_calloc(align, 1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (cold) {
        void *p = heap_caps_aligned_calloc(align, 1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (p)
            g_dsp_psram_bytes += n;
        return p;
    }
    if (arena && arena_owner && xTaskGetCurrentTaskHandle() == arena_owner) {
        size_t off = (arena_used + align - 1) / align * align;
        if (off + n <= arena_cap) {
            void *p = arena + off;
            if (zero)
                memset(p, 0, n);
            arena_used = off + n;
            if (arena_used > arena_peak)
                arena_peak = arena_used;
            g_dsp_internal_bytes += n;
            return p;
        }
    }
    void *p = zero ? heap_caps_aligned_calloc(align, 1, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
                   : heap_caps_aligned_alloc(align, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (p)
        g_dsp_internal_bytes += n;
    return p;
}

void sdr_hot_free(void *p)
{
    if (p && !in_arena(p))
        heap_caps_free(p);
}

static void *fast_alloc(size_t n)
{
    void *p = NULL;
    if (counting)
        return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!cold && heap_caps_get_free_size(MALLOC_CAP_INTERNAL) > n + (size_t)CONFIG_RDOF_SDR_INTERNAL_RESERVE_KB * 1024)
        p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (p) {
        g_dsp_internal_bytes += n;
        return p;
    }
    p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p)
        g_dsp_psram_bytes += n;
    return p;
}

/* The buffers touched for every sample: the hot arena, else internal RAM while it lasts, whatever the reserve (an
 * OLED's few KB taken first once pushed the channelizer's FFT input to PSRAM, and 576 kS/s lost bursts in every
 * call). */
static void *hot_alloc(size_t n)
{
    void *p = sdr_hot_aligned(HOT_ALIGN, n, false);
    return p ? p : fast_alloc(n);
}

void dsp_setup(void)
{
    static bool done;
    if (done)
        return;
    done = true;
    rdof_sdr_set_allocator(fast_alloc, sdr_hot_free); /* a free in the arena is nothing; else heap_caps_free */
    rdof_sdr_set_hot_allocator(hot_alloc);
#if CONFIG_RDOF_SDR_ESP_DSP
    dsp_esp_install();
#endif
}

#if CONFIG_RDOF_SDR_SELFTEST
static SemaphoreHandle_t selftest_done;

static void selftest_task(void *arg)
{
    /* it keeps core 1 busy for seconds on end: its idle task can't feed the watchdog meanwhile */
    TaskHandle_t idle1 = xTaskGetIdleTaskHandleForCore(1);
    bool watched = esp_task_wdt_status(idle1) == ESP_OK;
    if (watched)
        esp_task_wdt_delete(idle1);
    selftest_run();
    if (watched)
        esp_task_wdt_add(idle1);
    xSemaphoreGive(selftest_done);
    vTaskDelete(NULL);
}
#endif

void sdr_boot_selftest(void)
{
    bool asked = settings_take_flag("selftest") || (g_roles.has_sdr && g_scfg.selftest);
    if (!asked)
        return;
#if CONFIG_RDOF_SDR_SELFTEST
    dsp_setup();
    /* on core 1 like the DSP task, so a two-core run has its helper on the other core; and its own stack */
    selftest_done = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(selftest_task, "selftest", 16384, NULL, 5, NULL, 1);
    xSemaphoreTake(selftest_done, portMAX_DELAY);
    vSemaphoreDelete(selftest_done);
#else
    selftest_run(); /* says it isn't in this build */
#endif
}

void sdr_role_start(void)
{
    if (g_sstat.started)
        return;
    g_sstat.started = true;
    if (settings_take_flag("spike"))
        g_scfg.spike = true;
    ESP_LOGI(TAG, "SDR %s: %d channels at %.0f S/s", g_scfg.name, g_scfg.nch, g_scfg.sample_rate);
    dsp_setup();
    slot_timing_setup(&g_scfg.slot_timing);
    sdr_hb_start();
    sdr_mqtt_start();
    sdr_start();
}

static void jnum(rdof_json_t *j, const char *k, double v)
{
    if (!isfinite(v))
        rdof_json_raw(j, k, "null");
    else
        rdof_json_float3(j, k, v);
}

void sdr_status_json(rdof_json_t *j)
{
    static int64_t last_us;
    static uint64_t last_bytes;
    const sdr_status_t *s = &g_sstat;
    rdof_fe_stats_t u;
    sdr_fe_stats(&u);
    int64_t now = esp_timer_get_time();
    double bps = last_us && u.bytes >= last_bytes ? (double)(u.bytes - last_bytes) * 1e6 / (double)(now - last_us) : NAN;
    last_us = now;
    last_bytes = u.bytes;
    rdof_json_open(j, "sdr");
    rdof_json_str(j, "name", g_scfg.name);
    rdof_json_bool(j, "started", s->started);
    rdof_json_bool(j, "rtl_open", s->rtl_open);
    rdof_json_bool(j, "sim", g_scfg.sim);
    rdof_json_str(j, "frontend", rdof_fe_type_name(g_scfg.frontend));
    rdof_json_str(j, "rtl", s->rtl_desc);
    rdof_json_str(j, "rtl_error", s->rtl_error);
    rdof_json_bool(j, "receiving", s->receiving);
    rdof_json_int(j, "sample_rate", (long long)g_scfg.sample_rate);
    jnum(j, "center_hz", s->center_hz);
    jnum(j, "gain_db", s->gain_db);
    jnum(j, "cpu_load", s->cpu_load);
    rdof_json_int(j, "dsp_split", s->dsp_split);
    int dsp_free, helper_free;
    sdr_dsp_stacks(&dsp_free, &helper_free);
    rdof_json_int(j, "dsp_stack_free", dsp_free);
    rdof_json_int(j, "helper_stack_free", helper_free);
    size_t hot_used, hot_peak, hot_cap;
    sdr_hot_arena_use(&hot_used, &hot_peak, &hot_cap);
    rdof_json_int(j, "hot_arena", (long long)hot_cap);       /* the plan's hot arena (bytes, internal) */
    rdof_json_int(j, "hot_arena_used", (long long)hot_used); /* by the receiver now */
    rdof_json_open(j, "cpu_stages");
    static const char *const stage_names[SDR_STAGES] = {"input", "channelize", "demod", "track"};
    for (int k = 0; k < SDR_STAGES; k++)
        jnum(j, stage_names[k], s->stage_load[k]);
    rdof_json_close(j);
    rdof_json_open(j, "demod_steps");
    static const char *const step_names[4] = {"fm", "search", "flywheel", "history"};
    for (int k = 0; k < 4; k++)
        jnum(j, step_names[k], s->demod_load[k]);
    rdof_json_close(j);
    jnum(j, "usb_bytes_per_s", bps);
    const rdof_fe_caps_t *fc = sdr_fe_caps();
    jnum(j, "usb_expected_per_s",
         fc && fc->link_bytes_per_s > 0 ? fc->link_bytes_per_s
                                        : rdof_fe_sample_bytes(fc ? fc->format : RDOF_FE_CU8) * g_scfg.sample_rate);
    rdof_json_int(j, "usb_errors", u.errors);
    rdof_json_int(j, "usb_short", u.short_reads);
    rdof_json_int(j, "ring_overflows", u.overflows);
    rdof_json_int(j, "bytes_lost", (long long)u.bytes_lost);
    rdof_json_int(j, "bytes_filled", (long long)u.bytes_filled);
    rdof_json_int(j, "stream_restarts", s->stream_restarts);
    if (s->started) {
        sdr_ppm_status_t p;
        sdr_ppm_status(&p);
        rdof_json_open(j, "ppm");
        rdof_json_bool(j, "learning", p.on);
        jnum(j, "rtl", p.chip);
        jnum(j, "learned", p.learned);
        rdof_json_str(j, "by", p.measured ? "clock" : "afc");
        rdof_json_int(j, "channels", p.channels);
        rdof_json_bool(j, "settled", p.settled);
        jnum(j, "die_c", p.die_c);
        jnum(j, "die_spread", p.die_spread);
        jnum(j, "die_tol", p.die_tol);
        jnum(j, "expected", p.cal_ppm);
        rdof_json_raw(j, "table", "[");
        for (int i = 0; i < p.cal.count && j->len + 48 < j->cap; i++)
            j->len += (size_t)snprintf(j->buf + j->len, j->cap - j->len, "%s{\"c\":%d,\"ppm\":%.3f,\"n\":%d}",
                                       i ? "," : "", p.cal.e[i].temp_c, p.cal.e[i].v, p.cal.e[i].n);
        if (j->len + 2 < j->cap) {
            j->buf[j->len++] = ']';
            j->buf[j->len] = 0;
        }
        jnum(j, "steady_s", p.steady_s);
        jnum(j, "saved", p.saved);
        if (!isnan(p.sim_ppm))
            jnum(j, "sim", p.sim_ppm);
        if (p.clock_on) {
            rdof_json_open(j, "clock");
            jnum(j, "ppm", p.clock_ppm);
            jnum(j, "err", p.clock_err);
            rdof_json_bool(j, "trusted", p.clock_trusted);
            jnum(j, "raw", p.clock_raw);
            jnum(j, "span_s", p.clock_span_s);
            jnum(j, "rms_us", p.clock_rms_us);
            rdof_json_int(j, "steps", p.clock_steps);
            jnum(j, "spread", p.rate_spread);
            jnum(j, "spread_tol", p.rate_tol);
            jnum(j, "board", p.board_ppm);
            rdof_json_bool(j, "board_learned", p.board_learned);
            rdof_json_close(j);
        }
        rdof_json_close(j);
    }
    slot_timing_json(j);
    rdof_json_int(j, "ring_peak", (long long)u.ring_peak);
    rdof_json_bool(j, "mqtt", s->mqtt_up);
    rdof_json_int(j, "mqtt_dropped", sdr_mqtt_dropped());
    rdof_json_int(j, "hblink_peers", s->hb_peers);
    rdof_json_int(j, "hblink_up", s->hb_up);
    rdof_json_bool(j, "esp_dsp_fft", g_esp_fft);
    rdof_json_bool(j, "esp_dsp_fir", g_esp_fir);
    rdof_json_raw(j, "channels", "[");
    rx_lock();
    for (int c = 0, k = 0; c < g_scfg.nch; c++) {
        char item[256];
        const rdof_sdr_cfg_channel_t *ch = &g_scfg.ch[c];
        bool in = g_rx_ready && s->in_window[c];
        long bursts = 0;
        int rssi = 0;
        double afc = 0, idle = 0;
        if (in && k < g_rx.C) {
            bursts = g_rx.bursts[k];
            rssi = g_rx.rssi[k];
            afc = g_rx.chz.correction_hz[k];
            idle = g_rx.chunks ? 100.0 * (double)g_rx.gated[k] / (double)g_rx.chunks : 0.0;
            k++;
        }
        /* idle: the share of chunks the idle gate skipped */
        int n = snprintf(item, sizeof item, "%s{\"id\":\"%s\",\"name\":\"%s\",\"freq\":%.0f,\"in\":%s,\"bursts\":%ld,"
                         "\"rssi\":%d,\"afc\":%.1f,\"idle\":%.0f}",
                         c ? "," : "", ch->id, ch->name, ch->freq, in ? "true" : "false", bursts,
                         rssi == INT32_MIN ? 0 : rssi, afc, idle);
        if (j->len + (size_t)n + 4 < j->cap) {
            memcpy(j->buf + j->len, item, (size_t)n);
            j->len += (size_t)n;
            j->buf[j->len] = 0;
        }
    }
    rx_unlock();
    if (j->len + 2 < j->cap) {
        j->buf[j->len++] = ']';
        j->buf[j->len] = 0;
    }
    rdof_json_close(j);
}

size_t sdr_resolved(char *buf, size_t cap)
{
    size_t n = rdof_sdr_config_describe(&g_scfg, buf, cap);
    /* the tuner plan: what plan_center or the window picks for these channels */
    double f[RDOF_SDR_MAX_CHANNELS], c = 0;
    bool in[RDOF_SDR_MAX_CHANNELS];
    for (int i = 0; i < g_scfg.nch; i++)
        f[i] = g_scfg.ch[i].freq;
    const rdof_fe_caps_t *caps = sdr_fe_caps();
    const double usable = caps ? caps->usable : 0.8, dc_guard = caps ? caps->dc_guard_hz : 25e3;
    if (g_scfg.center) {
        c = g_scfg.center;
        for (int i = 0; i < g_scfg.nch; i++)
            in[i] = fabs(f[i] - c) <= g_scfg.sample_rate * usable / 2;
        n += (size_t)snprintf(buf + n, cap - n, "plan: center %.5f MHz as set;", c / 1e6);
    } else if (rdof_plan_center_band(f, g_scfg.nch, g_scfg.sample_rate, usable, dc_guard, &c) == 0) {
        for (int i = 0; i < g_scfg.nch; i++)
            in[i] = true;
        n += (size_t)snprintf(buf + n, cap - n, "plan: center %.5f MHz (plan_center), every channel;", c / 1e6);
    } else {
        int k = rdof_plan_window_band(f, g_scfg.nch, g_scfg.sample_rate, usable, dc_guard, &c, in);
        n += (size_t)snprintf(buf + n, cap - n, "plan: they don't all fit %.0f kS/s; the best window is %.5f MHz with %d;",
                              g_scfg.sample_rate / 1e3, c / 1e6, k);
    }
    for (int i = 0; i < g_scfg.nch && n < cap - 64; i++)
        n += (size_t)snprintf(buf + n, cap - n, " %s %s%s", g_scfg.ch[i].id, in[i] ? "in" : "out",
                              in[i] && fabs(f[i] - c) < dc_guard ? " (near DC)" : "");
    n += (size_t)snprintf(buf + n, cap - n, "\n");
    return n < cap ? n : cap - 1;
}
