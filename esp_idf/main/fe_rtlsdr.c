// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The RTL-SDR front end (rdof/sdr/frontend.h): rtlsdr's driver on the board's
 * USB host, its samples from the PSRAM ring, the dongle's status in g_sstat.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "mem.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rtlsdr/rtl_usb.h"
#include "sdr_fe.h"

static const char *TAG = "sdr";

static double gains[32];
static int n_gains;

static const rdof_fe_caps_t caps = {
    .kind = "RTL-SDR",
    .format = RDOF_FE_CU8,
    /* what the RTL takes (225001 to 300000 and 900001 to 3200000); the S3's Full Speed USB carries 240000 or
     * 288000 (docs/reference.md, "Why 288 kS/s") */
    .rate_min = 240000,
    .rate_max = 3200000,
    .freq_min = 24e6,
    .freq_max = 1766e6,
    .usable = 0.8,
    .dc_guard_hz = 25e3,
    .gain = true,
    .usb = true,
};

static struct {
    volatile bool gone;
    TaskHandle_t waiter;
    char error[128];
} st;

static void on_open(void *ctx, rtl_dev_t *dev)
{
    st.gone = false;
    g_sstat.rtl_open = true;
    snprintf(g_sstat.rtl_desc, sizeof g_sstat.rtl_desc, "RTL2832U + %s, %d-byte packets",
             rtl_tuner_name(dev->tuner), rtl_usb_packet_size());
    g_sstat.rtl_error[0] = 0;
    if (st.waiter)
        xTaskNotifyGive(st.waiter);
}

static void on_gone(void *ctx)
{
    st.gone = true;
    g_sstat.rtl_open = false;
    g_sstat.receiving = false;
}

static void on_fail(void *ctx, const char *why)
{
    snprintf(g_sstat.rtl_error, sizeof g_sstat.rtl_error, "%s", why);
    ESP_LOGE(TAG, "RTL-SDR %s", why);
}

static void on_data(void *ctx, int64_t t_us, uint64_t bytes) { sdr_rate_block(t_us, bytes / 2); }

/* The memory plan's "sdr_fe" (mem.c): the bulk transfers' buffers (internal DMA memory, 8 of 8 KB by default), and
 * the driver's two tasks, kept from boot as holds and handed over as the driver allocates them; its ring (PSRAM). */
void fe_rtlsdr_mem_claims(void)
{
    const int n = g_scfg.usb_transfers < 1 ? 1 : g_scfg.usb_transfers;
    mem_add_hold("sdr_fe", "usb_xfer", ((size_t)g_scfg.usb_transfer_kb * 1024 + 63) / 64 * 64, n, RDOF_MEM_DMA);
    mem_add_hold("sdr_fe", "usb_client", 4096, 1, RDOF_MEM_INTERNAL);
    mem_add_hold("sdr_fe", "usb_open", 6144, 1, RDOF_MEM_INTERNAL);
    mem_add("sdr_fe", "ring", (size_t)CONFIG_RDOF_SDR_RING_KB * 1024 + 1, 4, RDOF_MEM_PSRAM, RDOF_MEM_BUDGET);
}

static void before_alloc(void *ctx, size_t bytes) { mem_hand_over("sdr_fe", "usb_xfer"); }

static int init(void *ctx)
{
    usb_host_start();
    mem_hand_over("sdr_fe", "usb_client"); /* rtl_usb_start's tasks */
    mem_hand_over("sdr_fe", "usb_open");
    rtl_usb_config_t uc = {
        .ring_bytes = (size_t)CONFIG_RDOF_SDR_RING_KB * 1024,
        .task_core = 0,
        .task_priority = 18,
        .host_installed = true,
        .gap_fill_ms = 250, /* a dropped transfer keeps the timeline: the flywheel misses only what was lost */
        .on_open = on_open,
        .on_gone = on_gone,
        .on_fail = on_fail,
        .on_data = on_data,
        .before_alloc = before_alloc,
    };
    if (rtl_usb_start(&uc) != ESP_OK) {
        snprintf(st.error, sizeof st.error, "the RTL-SDR driver didn't start");
        return -1;
    }
    return 0;
}

static bool wait(void *ctx, int timeout_ms)
{
    st.waiter = xTaskGetCurrentTaskHandle();
    if (rtl_usb_device())
        return true;
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeout_ms));
    return false; /* the caller asks again */
}

static size_t hex_regs(char *out, size_t cap, const char *key, int n, int (*rd)(rtl_dev_t *, int, uint8_t *),
                       rtl_dev_t *dev)
{
    size_t len = (size_t)snprintf(out, cap, ",\"%s\":\"", key);
    for (int i = 0; i < n && len + 4 < cap; i++) {
        uint8_t v;
        if (rd(dev, i, &v))
            len += (size_t)snprintf(out + len, cap - len, "--");
        else
            len += (size_t)snprintf(out + len, cap - len, "%02x", v);
    }
    len += (size_t)snprintf(out + len, cap > len ? cap - len : 0, "\"");
    return len;
}

static int rdof_page;
static int rdof_demod(rtl_dev_t *d, int i, uint8_t *v) { return rtl_read_demod_reg(d, rdof_page, (uint8_t)i, v); }
static int rdof_sys(rtl_dev_t *d, int i, uint8_t *v) { return rtl_read_block_reg(d, 2, (uint16_t)(0x3000 + i), v); }

size_t fe_rtlsdr_regs_json(char *buf, size_t cap)
{
    rtl_dev_t *dev = rtl_usb_device();
    if (!dev || cap < 4096)
        return 0;
    uint8_t tun[32];
    rx_lock(); /* the tuner's I2C goes through the repeater the SDR task's tuner calls use */
    int n = -1;
    static const int tries[] = {32, 16, 8, 5}; /* as many as the RTL's I2C bridge reads at once */
    for (int k = 0; k < 4 && n < 0; k++)
        n = rtl_read_tuner_regs(dev, tun, tries[k]);
    rx_unlock();
    size_t len = (size_t)snprintf(buf, cap, "{\"t_s\":%.1f,\"tuner_n\":%d,\"error\":\"%s\",\"tuner\":\"",
                                  esp_timer_get_time() / 1e6, n, n < 0 ? dev->error : "");
    for (int i = 0; i < (int)sizeof tun; i++)
        len += (size_t)snprintf(buf + len, cap - len, i < n ? "%02x" : "--", tun[i]);
    len += (size_t)snprintf(buf + len, cap - len, "\"");
    static char *keys[] = {"demod0", "demod1", "demod2", "demod3", "demod4"};
    for (rdof_page = 0; rdof_page < 5; rdof_page++)
        len += hex_regs(buf + len, cap - len, keys[rdof_page], 256, rdof_demod, dev);
    len += hex_regs(buf + len, cap - len, "sys", 256, rdof_sys, dev);
    if (len + 2 >= cap)
        return 0;
    buf[len++] = '}';
    buf[len] = 0;
    return len;
}

static int fail(rtl_dev_t *dev)
{
    snprintf(st.error, sizeof st.error, "%s", dev->error);
    return -1;
}

static int tune(void *ctx, double rate, double center_hz, int ppm, double *got_rate)
{
    rtl_dev_t *dev = rtl_usb_device();
    if (!dev) {
        snprintf(st.error, sizeof st.error, "no dongle");
        return -1;
    }
    /* the sample clock times the crystal (sdr_task.c), so the correction moves only the LO and IF: what the RTL's
     * sample frequency register does to the rate isn't what the datasheet formula says (rtl.h) */
    dev->fixed_sample_rate = true;
    if (rtl_set_sample_rate(dev, (uint32_t)rate) || rtl_set_freq_correction(dev, ppm) ||
        rtl_set_center_freq(dev, (uint32_t)lrint(center_hz)))
        return fail(dev);
    if (g_scfg.biastee)
        rtl_set_bias_tee(dev, true);
    rtl_resampler_ratio(dev->rtl_xtal, (uint32_t)rate, got_rate); /* to the fraction: timing the samples needs it */
    return 0;
}

static int set_ppm(void *ctx, int ppm)
{
    rtl_dev_t *dev = rtl_usb_device();
    if (!dev) {
        snprintf(st.error, sizeof st.error, "no dongle");
        return -1;
    }
    return rtl_set_freq_correction(dev, ppm) ? fail(dev) : 0;
}

static double sample_ppm(void *ctx)
{
    rtl_dev_t *dev = rtl_usb_device();
    return dev && !dev->fixed_sample_rate ? rtl_sample_correction_ppm(dev->ppm) : 0.0;
}

static int set_gain(void *ctx, double db, double *got)
{
    rtl_dev_t *dev = rtl_usb_device();
    if (!dev) {
        snprintf(st.error, sizeof st.error, "no dongle");
        return -1;
    }
    if (isnan(db)) {
        rtl_set_tuner_gain_mode(dev, false);
        *got = NAN;
        return 0;
    }
    rtl_set_tuner_gain(dev, (int)lrint(db * 10));
    *got = dev->gain / 10.0;
    return 0;
}

static int gain_steps(void *ctx, const double **db)
{
    rtl_dev_t *dev = rtl_usb_device();
    const int *steps;
    int n = dev ? rtl_tuner_gains(dev, &steps) : 0;
    n_gains = 0;
    for (int i = 0; i < n && i < 32; i++)
        gains[n_gains++] = steps[i] / 10.0;
    *db = gains;
    return n_gains;
}

static int start(void *ctx)
{
    if (rtl_usb_stream_start(g_scfg.usb_transfers, g_scfg.usb_transfer_kb * 1024) != ESP_OK) {
        snprintf(st.error, sizeof st.error, "can't start streaming");
        return -1;
    }
    return 0;
}

static void stop(void *ctx) { rtl_usb_stream_stop(); }

static size_t read(void *ctx, uint8_t *out, size_t len, int timeout_ms, size_t *filled)
{
    return rtl_usb_read(out, len, pdMS_TO_TICKS(timeout_ms), filled);
}

static bool gone(void *ctx) { return st.gone; }

static void stats(void *ctx, rdof_fe_stats_t *out)
{
    rtl_usb_stats_t s;
    rtl_usb_stats(&s);
    *out = (rdof_fe_stats_t){s.bytes, s.errors, s.short_transfers, s.ring_overflows, s.bytes_lost, s.bytes_filled,
                           s.ring_peak};
}

static const char *identity(void *ctx) { return rtl_usb_serial(); }
static const char *error(void *ctx) { return st.error; }

static void spike(void *ctx) { spike_run(); }

static const rdof_fe_ops_t ops = {
    .init = init,
    .wait = wait,
    .tune = tune,
    .set_ppm = set_ppm,
    .set_gain = set_gain,
    .gain_steps = gain_steps,
    .start = start,
    .stop = stop,
    .read = read,
    .gone = gone,
    .stats = stats,
    .identity = identity,
    .error = error,
    .sample_ppm = sample_ppm,
#if CONFIG_RDOF_SDR_SPIKE
    .spike = spike,
#endif
};

void fe_rtlsdr(rdof_fe_t *fe)
{
    (void)spike;
    *fe = (rdof_fe_t){&ops, &caps, NULL};
}
