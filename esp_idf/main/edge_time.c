// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Pin edges timed in hardware (MCPWM capture, one 80 MHz timer, three channels): the LR2021's IRQ pin (the RX FIFO's
 * level, a transmission's TX_DONE), its NSS (an output of ours: a capture channel only needs the pin's input path)
 * and the GNSS PPS. An interrupt's or a read's own time has latency in it; the captured count doesn't.
 *
 * The capture timer and esp_timer come from the same crystal, so a constant ties them. Once a second a software capture
 * is triggered right after esp_timer is read; a pair is good to a few us, and the median of the last 15 is the tie.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "app.h"
#include "driver/mcpwm_cap.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "hal/mcpwm_ll.h"
#include "lr2021_port.h"
#include "netclock.h"
#include "rdof/json.h"
#include "sdkconfig.h"

static const char *TAG = "edge_time";

#define TICKS_PER_US 80

#ifndef CONFIG_NETCLOCK_PPS_GPIO
#define CONFIG_NETCLOCK_PPS_GPIO -1
#endif

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static mcpwm_cap_channel_handle_t chan[EDGE_N];
static int hw_ch[EDGE_N], hw_used; /* the driver hands out the capture channels in order */
static struct {
    uint32_t n, cap;
} last[EDGE_N];

/* the tie: (esp_timer us * 80) - capture count, mod 2^32. A pair's scatter around the median: tie_*_ns */
#define TIES 15
static bool soft, pair_new;
static uint32_t a_cap;
static int64_t a_us, soft_us;
static uint32_t tie_now;
static bool tie_ok;
static uint32_t ties[TIES];
static int ties_n, ties_at;
static int tie_min_ns, tie_max_ns;

/* the SetTx's NSS edge: the next edge once marked */
static bool settx_armed, settx_new;
static uint32_t settx_cap;
static int64_t settx_at, armed_at;
static int settx_min_ns = INT32_MAX, settx_max_ns = INT32_MIN;
static uint32_t settx_n;

/* PPS: the interrupt's time against the capture's */
static int pps_isr_min_ns = INT32_MAX, pps_isr_max_ns = INT32_MIN;
static uint32_t pps_seen;

static double to_us(uint32_t cap)
{
    const int64_t now = esp_timer_get_time();
    return (double)now - (double)(int32_t)((uint32_t)(now * TICKS_PER_US) - cap - tie_now) / TICKS_PER_US;
}

static bool on_cap(mcpwm_cap_channel_handle_t c, const mcpwm_capture_event_data_t *e, void *arg)
{
    const int i = (int)(intptr_t)arg;
    portENTER_CRITICAL_ISR(&mux);
    if (i == EDGE_PPS && soft) {
        soft = false;
        a_cap = e->cap_value;
        a_us = soft_us;
        pair_new = true;
    } else {
        last[i].cap = e->cap_value;
        last[i].n++;
        if (i == EDGE_NSS && settx_armed) {
            settx_armed = false;
            mcpwm_ll_intr_enable(&MCPWM0, MCPWM_LL_EVENT_CAPTURE(hw_ch[EDGE_NSS]), false);
            settx_new = true;
            settx_cap = e->cap_value;
            settx_at = armed_at;
        }
    }
    portEXIT_CRITICAL_ISR(&mux);
    return false;
}

/* From the SetTx's sender, just before its frame: the next NSS edge is the SetTx's, asked for at_us (0: not timed). */
void lr2021_mark_settx(int64_t at_us)
{
    portENTER_CRITICAL(&mux);
    settx_armed = chan[EDGE_NSS] != NULL;
    armed_at = at_us;
    if (settx_armed) { /* NSS falls thousands of times a second: its interrupt is on for this one edge */
        mcpwm_ll_intr_clear_status(&MCPWM0, MCPWM_LL_EVENT_CAPTURE(hw_ch[EDGE_NSS]));
        mcpwm_ll_intr_enable(&MCPWM0, MCPWM_LL_EVENT_CAPTURE(hw_ch[EDGE_NSS]), true);
    }
    portEXIT_CRITICAL(&mux);
}

bool edge_time_last(edge_ch_t ch, double *us, uint32_t *n)
{
    portENTER_CRITICAL(&mux);
    const bool ok = tie_ok && chan[ch] && last[ch].n;
    if (ok)
        *us = to_us(last[ch].cap);
    if (n)
        *n = last[ch].n;
    portEXIT_CRITICAL(&mux);
    return ok;
}

/* In IRAM, and the register written directly: from flash, a cache miss between the two steps put them about 10 us apart. */
static IRAM_ATTR void tie(void)
{
    portENTER_CRITICAL(&mux);
    soft = true;
    soft_us = esp_timer_get_time();
    mcpwm_ll_trigger_soft_capture(&MCPWM0, hw_ch[EDGE_PPS]);
    portEXIT_CRITICAL(&mux);
}

/* The last pair into the tie. The ties are compared by their difference from the first: they all sit within a few
 * ticks of each other, far from wrapping apart. */
static void tie_update(void)
{
    portENTER_CRITICAL(&mux);
    const bool got = pair_new;
    pair_new = false;
    const uint32_t v = (uint32_t)(a_us * TICKS_PER_US) - a_cap;
    portEXIT_CRITICAL(&mux);
    if (!got)
        return;
    ties[ties_at] = v;
    ties_at = (ties_at + 1) % TIES;
    if (ties_n < TIES)
        ties_n++;
    int32_t d[TIES];
    int n = 0;
    for (int i = 0; i < ties_n; i++) {
        const int32_t x = (int32_t)(ties[i] - v);
        int k = n++;
        while (k > 0 && d[k - 1] > x) {
            d[k] = d[k - 1];
            k--;
        }
        d[k] = x;
    }
    const uint32_t med = v + (uint32_t)d[n / 2];
    portENTER_CRITICAL(&mux);
    tie_now = med;
    tie_ok = ties_n >= 3;
    portEXIT_CRITICAL(&mux);
    if (ties_n == TIES) { /* a full window: the medians settled */
        const int ns = (int)((int32_t)(v - med) * 25 / 2);
        if (ns < tie_min_ns)
            tie_min_ns = ns;
        if (ns > tie_max_ns)
            tie_max_ns = ns;
    }
}

static void tick(void *arg)
{
    tie_update();
    tie();

    portENTER_CRITICAL(&mux);
    const bool have = tie_ok && settx_new;
    const double sent = have ? to_us(settx_cap) : 0;
    const int64_t at = settx_at;
    if (have)
        settx_new = false;
    portEXIT_CRITICAL(&mux);
    if (have && at) {
        const int ns = (int)((sent - (double)at) * 1000);
        if (ns < settx_min_ns)
            settx_min_ns = ns;
        if (ns > settx_max_ns)
            settx_max_ns = ns;
        settx_n++;
    }

    double pps;
    uint32_t n;
#if CONFIG_NETCLOCK_PPS_GPIO >= 0
    const int64_t isr = netclock_pps_last_us();
#else
    const int64_t isr = 0;
#endif
    if (isr && edge_time_last(EDGE_PPS, &pps, &n) && n != pps_seen) {
        pps_seen = n;
        const double late = (double)isr - pps;
        if (late > -1e5 && late < 1e5) { /* the same edge */
            if (late * 1000 < pps_isr_min_ns)
                pps_isr_min_ns = (int)(late * 1000);
            if (late * 1000 > pps_isr_max_ns)
                pps_isr_max_ns = (int)(late * 1000);
        }
    }
}

static bool add(mcpwm_cap_timer_handle_t t, edge_ch_t i, int gpio, bool falling)
{
    if (gpio < 0)
        return true;
    const mcpwm_capture_channel_config_t c = {.gpio_num = gpio, .prescale = 1, .flags = {.pos_edge = !falling, .neg_edge = falling}};
    const mcpwm_capture_event_callbacks_t cb = {.on_cap = on_cap};
    mcpwm_cap_channel_handle_t h = NULL;
    esp_err_t e = mcpwm_new_capture_channel(t, &c, &h);
    if (e == ESP_OK)
        e = mcpwm_capture_channel_register_event_callbacks(h, &cb, (void *)(intptr_t)i);
    if (e == ESP_OK)
        e = mcpwm_capture_channel_enable(h);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "GPIO %d: %s", gpio, esp_err_to_name(e));
        return false;
    }
    hw_ch[i] = hw_used++;
    chan[i] = h;
    return true;
}

void edge_time_start(void)
{
    static bool started;
    if (started || CONFIG_NETCLOCK_PPS_GPIO < 0) /* the software capture needs the PPS channel */
        return;
    started = true;
    const mcpwm_capture_timer_config_t tc = {.group_id = 0, .clk_src = MCPWM_CAPTURE_CLK_SRC_DEFAULT,
                                             .resolution_hz = TICKS_PER_US * 1000000};
    mcpwm_cap_timer_handle_t t = NULL;
    esp_err_t e = mcpwm_new_capture_timer(&tc, &t);
    if (e == ESP_OK)
        e = mcpwm_capture_timer_enable(t);
    if (e == ESP_OK)
        e = mcpwm_capture_timer_start(t);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "capture timer: %s", esp_err_to_name(e));
        return;
    }
    if (!add(t, EDGE_FIFO, CONFIG_RDOF_LR2021_IRQ, false) || !add(t, EDGE_NSS, CONFIG_RDOF_LR2021_NSS, true) ||
        !add(t, EDGE_PPS, CONFIG_NETCLOCK_PPS_GPIO, false))
        return;
    if (chan[EDGE_NSS])
        mcpwm_ll_intr_enable(&MCPWM0, MCPWM_LL_EVENT_CAPTURE(hw_ch[EDGE_NSS]), false);
    const esp_timer_create_args_t a = {.callback = tick, .name = "edge_time"};
    esp_timer_handle_t h;
    if (esp_timer_create(&a, &h) == ESP_OK)
        esp_timer_start_periodic(h, 1000000);
}

void edge_time_json(rdof_json_t *j)
{
    if (!chan[EDGE_PPS])
        return;
    uint32_t n[EDGE_N] = {0};
    double unused;
    for (int i = 0; i < EDGE_N; i++)
        if (chan[i])
            edge_time_last(i, &unused, &n[i]);
    rdof_json_open(j, "edges");
    rdof_json_int(j, "irq", n[EDGE_FIFO]);
    rdof_json_int(j, "nss", n[EDGE_NSS]);
    rdof_json_int(j, "pps", n[EDGE_PPS]);
    rdof_json_int(j, "tie_min_ns", tie_min_ns);
    rdof_json_int(j, "tie_max_ns", tie_max_ns);
    if (pps_isr_min_ns != INT32_MAX) {
        rdof_json_int(j, "pps_isr_min_ns", pps_isr_min_ns);
        rdof_json_int(j, "pps_isr_max_ns", pps_isr_max_ns);
    }
    if (settx_n) {
        rdof_json_int(j, "settx_n", settx_n);
        rdof_json_int(j, "settx_min_ns", settx_min_ns);
        rdof_json_int(j, "settx_max_ns", settx_max_ns);
    }
    rdof_json_close(j);
}
