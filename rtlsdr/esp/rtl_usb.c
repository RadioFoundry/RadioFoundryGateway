// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * RTL-SDR over the ESP-IDF USB host library. Three tasks: the library's
 * event loop, our client's event loop (transfer callbacks run there, so it
 * must never block), and an open task that waits for a dongle, does the
 * blocking control-transfer work (descriptors, rtl_open) and hands it over.
 */
#include "rtlsdr/rtl_usb.h"
#include "rtlsdr/gaps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "usb/usb_helpers.h"
#include "usb/usb_host.h"

static const char *TAG = "rtl_usb";

#define MAX_TRANSFERS 16
#define CTRL_DATA_MAX 64

static rtl_usb_config_t cfg;
static usb_host_client_handle_t client;
static usb_device_handle_t dev_hdl;
static volatile uint8_t new_addr;
static SemaphoreHandle_t new_dev, ctrl_done, ctrl_lock, gone_sem, opened;
static usb_transfer_t *ctrl_xfer;
static volatile bool device_gone;
static rtl_dev_t rtl;
static volatile bool is_open;
static int ep_mps;
static char serial_str[64]; /* the open dongle's "VID:PID serial" */

/* streaming */
static StreamBufferHandle_t ring;
static StaticStreamBuffer_t ring_struct;
static uint8_t *ring_storage;
static usb_transfer_t *xfers[MAX_TRANSFERS];
static int n_xfers, xfer_bytes;
static volatile bool streaming;
static volatile int in_flight;
static rtl_usb_stats_t stats;
static portMUX_TYPE stats_lock = portMUX_INITIALIZER_UNLOCKED;
static rtl_gaps_t gaps;    /* under stats_lock */
static uint64_t data_read; /* bytes taken from the ring since the stream started (filler not counted) */

/* ── control transfers: the driver's hook ───────────────────────────────── */

static void ctrl_cb(usb_transfer_t *t) { xSemaphoreGive(ctrl_done); }

static int control(void *ctx, bool in, uint16_t value, uint16_t index, uint8_t *data, uint16_t len)
{
    if (!dev_hdl || device_gone || len > CTRL_DATA_MAX)
        return -1;
    xSemaphoreTake(ctrl_lock, portMAX_DELAY);
    usb_setup_packet_t *s = (usb_setup_packet_t *)ctrl_xfer->data_buffer;
    s->bmRequestType = in ? 0xC0 : 0x40; /* vendor, device, IN / OUT */
    s->bRequest = 0;
    s->wValue = value;
    s->wIndex = index;
    s->wLength = len;
    if (!in && len)
        memcpy(ctrl_xfer->data_buffer + sizeof *s, data, len);
    ctrl_xfer->num_bytes = (int)(sizeof *s + len);
    ctrl_xfer->device_handle = dev_hdl;
    ctrl_xfer->bEndpointAddress = 0;
    ctrl_xfer->callback = ctrl_cb;
    ctrl_xfer->timeout_ms = 1000;
    int r = -1;
    xSemaphoreTake(ctrl_done, 0);
    if (usb_host_transfer_submit_control(client, ctrl_xfer) == ESP_OK) {
        if (xSemaphoreTake(ctrl_done, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (ctrl_xfer->status == USB_TRANSFER_STATUS_COMPLETED) {
                r = ctrl_xfer->actual_num_bytes - (int)sizeof *s;
                if (in && r > 0)
                    memcpy(data, ctrl_xfer->data_buffer + sizeof *s, (size_t)r);
            }
        } else {
            ESP_LOGW(TAG, "control 0x%04x/0x%04x timed out", value, index);
        }
    }
    xSemaphoreGive(ctrl_lock);
    return r;
}

static void sleep_ms(void *ctx, int ms) { vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1); }

/* ── bulk IN ────────────────────────────────────────────────────────────── */

static void bulk_cb(usb_transfer_t *t)
{
    bool resubmit = streaming && !device_gone;
    if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
        const int64_t now = esp_timer_get_time();
        size_t n = (size_t)t->actual_num_bytes, put = 0;
        /* all of a transfer or none: a part would leave a gap the reader can't place */
        if (n && xStreamBufferSpacesAvailable(ring) >= n)
            put = xStreamBufferSend(ring, t->data_buffer, n, 0);
        size_t used = xStreamBufferBytesAvailable(ring);
        portENTER_CRITICAL(&stats_lock);
        stats.transfers++;
        if (n < (size_t)t->num_bytes)
            stats.short_transfers++;
        if (put < n) {
            stats.ring_overflows++;
            stats.bytes_lost += n - put;
            rtl_gaps_note(&gaps, stats.bytes + put, (uint32_t)(n - put));
        }
        stats.bytes += put;
        stats.ring_used = used;
        if (used > stats.ring_peak)
            stats.ring_peak = used;
        const uint64_t sent = stats.bytes + stats.bytes_lost;
        portEXIT_CRITICAL(&stats_lock);
        if (cfg.on_data && n)
            cfg.on_data(cfg.ctx, now, sent);
    } else if (t->status != USB_TRANSFER_STATUS_CANCELED) {
        portENTER_CRITICAL(&stats_lock);
        stats.errors++;
        portEXIT_CRITICAL(&stats_lock);
        if (t->status == USB_TRANSFER_STATUS_NO_DEVICE)
            resubmit = false;
    }
    if (resubmit && usb_host_transfer_submit(t) == ESP_OK)
        return;
    if (resubmit) {
        portENTER_CRITICAL(&stats_lock);
        stats.resubmit_fails++;
        portEXIT_CRITICAL(&stats_lock);
    }
    portENTER_CRITICAL(&stats_lock);
    in_flight--;
    portEXIT_CRITICAL(&stats_lock);
}

esp_err_t rtl_usb_stream_start(int n, int bytes)
{
    if (!is_open || streaming)
        return ESP_ERR_INVALID_STATE;
    if (n < 1)
        n = 1;
    if (n > MAX_TRANSFERS)
        n = MAX_TRANSFERS;
    bytes = usb_round_up_to_mps(bytes, ep_mps ? ep_mps : 64);
    /* a transfer a stop timed out on is still the host's: freeing it would leave its callback a freed one */
    if (in_flight > 0) {
        ESP_LOGW(TAG, "can't stream: %d transfers still out from the last stream", in_flight);
        return ESP_ERR_INVALID_STATE;
    }
    /* The transfers are kept from one stream to the next (a restart, a replug): allocated again only when the plan
     * changes, so a stream restart never needs internal DMA memory the heap may no longer have in one piece. */
    if (n_xfers != n || xfer_bytes != bytes) {
        for (int i = 0; i < n_xfers; i++) {
            usb_host_transfer_free(xfers[i]);
            xfers[i] = NULL;
        }
        n_xfers = 0;
        xfer_bytes = bytes;
        for (int i = 0; i < n; i++) {
            if (cfg.before_alloc)
                cfg.before_alloc(cfg.ctx, (size_t)bytes);
            if (usb_host_transfer_alloc((size_t)bytes, 0, &xfers[i]) != ESP_OK)
                break;
            n_xfers++;
        }
    }
    if (!n_xfers)
        return ESP_ERR_NO_MEM;
    xStreamBufferReset(ring);
    memset(&stats, 0, sizeof stats);
    rtl_gaps_reset(&gaps, (uint32_t)((uint64_t)rtl.rate * 2 * (uint64_t)cfg.gap_fill_ms / 1000) & ~1u);
    data_read = 0;
    stats.started_us = esp_timer_get_time();
    if (rtl_reset_buffer(&rtl))
        return ESP_FAIL;
    streaming = true;
    for (int i = 0; i < n_xfers; i++) {
        usb_transfer_t *t = xfers[i];
        t->device_handle = dev_hdl;
        t->bEndpointAddress = RTL_BULK_EP;
        t->callback = bulk_cb;
        t->num_bytes = bytes;
        t->timeout_ms = 0;
        portENTER_CRITICAL(&stats_lock);
        in_flight++;
        portEXIT_CRITICAL(&stats_lock);
        if (usb_host_transfer_submit(t) != ESP_OK) {
            portENTER_CRITICAL(&stats_lock);
            in_flight--;
            portEXIT_CRITICAL(&stats_lock);
        }
    }
    ESP_LOGI(TAG, "streaming: %d transfers of %d bytes in flight", n_xfers, bytes);
    return ESP_OK;
}

void rtl_usb_stream_stop(void)
{
    if (!streaming)
        return;
    streaming = false;
    if (dev_hdl && !device_gone) {
        usb_host_endpoint_halt(dev_hdl, RTL_BULK_EP);
        usb_host_endpoint_flush(dev_hdl, RTL_BULK_EP);
    }
    for (int i = 0; i < 200 && in_flight > 0; i++)
        vTaskDelay(pdMS_TO_TICKS(5));
    if (dev_hdl && !device_gone)
        usb_host_endpoint_clear(dev_hdl, RTL_BULK_EP);
    if (in_flight > 0)
        ESP_LOGW(TAG, "%d transfers didn't come back", in_flight);
}

bool rtl_usb_streaming(void) { return streaming; }

size_t rtl_usb_read(uint8_t *out, size_t len, TickType_t timeout, size_t *filled)
{
    size_t got = 0, fill_n = 0;
    TickType_t end = xTaskGetTickCount() + timeout;
    while (got < len) {
        bool fill;
        portENTER_CRITICAL(&stats_lock);
        size_t k = rtl_gaps_next(&gaps, data_read, len - got, &fill);
        if (fill) {
            rtl_gaps_took(&gaps, k);
            stats.bytes_filled += k;
        }
        portEXIT_CRITICAL(&stats_lock);
        if (fill) {
            memset(out + got, 0x80, k);
            got += k;
            fill_n += k;
            continue;
        }
        TickType_t now = xTaskGetTickCount();
        TickType_t left = now < end ? end - now : 0;
        size_t n = xStreamBufferReceive(ring, out + got, k, left);
        got += n;
        data_read += n;
        if (!n && !left)
            break;
    }
    if (filled)
        *filled = fill_n;
    return got;
}

void rtl_usb_stats(rtl_usb_stats_t *out)
{
    portENTER_CRITICAL(&stats_lock);
    *out = stats;
    portEXIT_CRITICAL(&stats_lock);
    out->ring_used = ring ? xStreamBufferBytesAvailable(ring) : 0;
}

int rtl_usb_packet_size(void) { return is_open ? ep_mps : 0; }
const char *rtl_usb_serial(void) { return is_open ? serial_str : ""; }
rtl_dev_t *rtl_usb_device(void) { return is_open ? &rtl : NULL; }

bool rtl_usb_wait_open(TickType_t timeout)
{
    if (is_open)
        return true;
    if (xSemaphoreTake(opened, timeout) == pdTRUE) {
        xSemaphoreGive(opened);
        return is_open;
    }
    return false;
}

/* ── tasks ──────────────────────────────────────────────────────────────── */

static void client_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        new_addr = msg->new_dev.address;
        xSemaphoreGive(new_dev);
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        if (msg->dev_gone.dev_hdl == dev_hdl) {
            device_gone = true;
            streaming = false;
            xSemaphoreGive(gone_sem);
        }
    }
}

static void lib_task(void *arg)
{
    for (;;) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)
            usb_host_device_free_all();
    }
}

static void client_task(void *arg)
{
    for (;;)
        usb_host_client_handle_events(client, portMAX_DELAY);
}

static void ascii(const usb_str_desc_t *d, char *out, size_t cap)
{
    size_t n = 0;
    if (d)
        for (int i = 0; i < (d->bLength - 2) / 2 && n + 1 < cap; i++)
            out[n++] = d->wData[i] < 128 ? (char)d->wData[i] : '?';
    out[n] = 0;
}

/* The device at addr, if it's an RTL2832U: open it, run it until it's unplugged, close it. false if it isn't one. */
static bool run_device(uint8_t addr)
{
    usb_device_handle_t h;
    if (usb_host_device_open(client, addr, &h) != ESP_OK) {
        ESP_LOGW(TAG, "device at address %d: can't open it", addr);
        return false;
    }
    const usb_device_desc_t *dd;
    usb_host_get_device_descriptor(h, &dd);
    if (dd->idVendor != RTL_VID || (dd->idProduct != RTL_PID_2838 && dd->idProduct != RTL_PID_2832)) {
        ESP_LOGI(TAG, "%04X:%04X isn't an RTL2832U: not ours", dd->idVendor, dd->idProduct);
        usb_host_device_close(client, h);
        return false;
    }
    usb_device_info_t info;
    usb_host_device_info(h, &info);
    char mfr[48], prod[48], serial[48];
    ascii(info.str_desc_manufacturer, mfr, sizeof mfr);
    ascii(info.str_desc_product, prod, sizeof prod);
    ascii(info.str_desc_serial_num, serial, sizeof serial);
    snprintf(serial_str, sizeof serial_str, "%04X:%04X %s", dd->idVendor, dd->idProduct, serial);
    const char *speed = info.speed == USB_SPEED_HIGH ? "High" : info.speed == USB_SPEED_FULL ? "Full" : "Low";
    ESP_LOGI(TAG, "RTL-SDR %04X:%04X \"%s\" \"%s\" serial \"%s\", %s Speed, address %d", dd->idVendor,
             dd->idProduct, mfr, prod, serial, speed, info.dev_addr);
    usb_print_device_descriptor(dd);
    const usb_config_desc_t *cd;
    usb_host_get_active_config_descriptor(h, &cd);
    usb_print_config_descriptor(cd, NULL);
    int off = 0;
    const usb_ep_desc_t *ep = usb_parse_endpoint_descriptor_by_address(cd, 0, 0, RTL_BULK_EP, &off);
    ep_mps = ep ? USB_EP_DESC_GET_MPS(ep) : 64;
    ESP_LOGI(TAG, "bulk IN 0x%02X: %d-byte packets", RTL_BULK_EP, ep_mps);
    if (usb_host_interface_claim(client, h, 0, 0) != ESP_OK) {
        ESP_LOGE(TAG, "can't claim interface 0");
        usb_host_device_close(client, h);
        return true;
    }
    dev_hdl = h;
    device_gone = false;
    xSemaphoreTake(gone_sem, 0);
    rtl_io_t io = {control, sleep_ms, NULL};
    bool v4 = strstr(prod, "Blog V4") != NULL;
    /* a dongle on marginal power, or just plugged in, can fail a control transfer or two: try again before
     * giving up until it's replugged */
    int r = -1;
    for (int attempt = 1; attempt <= 4 && r && !device_gone; attempt++) {
        if (attempt > 1)
            vTaskDelay(pdMS_TO_TICKS(500 * attempt));
        r = rtl_open(&rtl, &io, info.speed != USB_SPEED_HIGH, v4);
        if (r)
            ESP_LOGE(TAG, "rtl_open failed (%d), attempt %d of 4: %s", r, attempt, rtl.error);
    }
    if (r) {
        if (cfg.on_fail) {
            char why[128];
            snprintf(why, sizeof why, "won't open (%s): replug it", rtl.error);
            cfg.on_fail(cfg.ctx, why);
        }
    } else {
        ESP_LOGI(TAG, "%s tuner, bulk FIFO set for %d-byte packets%s", rtl_tuner_name(rtl.tuner),
                 rtl.full_speed ? 64 : 512, v4 ? ", RTL-SDR Blog V4" : "");
        if (cfg.bias_tee && rtl_set_bias_tee(&rtl, true) == 0)
            ESP_LOGI(TAG, "bias tee on");
        is_open = true;
        xSemaphoreGive(opened);
        if (cfg.on_open)
            cfg.on_open(cfg.ctx, &rtl);
    }
    xSemaphoreTake(gone_sem, portMAX_DELAY);
    ESP_LOGW(TAG, "RTL-SDR unplugged");
    is_open = false;
    xSemaphoreTake(opened, 0);
    streaming = false;
    for (int i = 0; i < 100 && in_flight > 0; i++)
        vTaskDelay(pdMS_TO_TICKS(10));
    if (cfg.on_gone)
        cfg.on_gone(cfg.ctx);
    /* the transfers stay for the next dongle (rtl_usb_stream_start) */
    usb_host_interface_release(client, h, 0);
    usb_host_device_close(client, h);
    dev_hdl = NULL;
    return true;
}

static void open_task(void *arg)
{
    /* one already plugged in (the app's host may have enumerated it before this client was there) */
    uint8_t addrs[8];
    int n = 0;
    if (usb_host_device_addr_list_fill(sizeof addrs, addrs, &n) == ESP_OK)
        for (int i = 0; i < n; i++)
            if (run_device(addrs[i]))
                break;
    for (;;) {
        ESP_LOGI(TAG, "waiting for an RTL-SDR (%04X:%04X); it needs 5 V on VBUS from outside the S3", RTL_VID,
                 RTL_PID_2838);
        xSemaphoreTake(new_dev, portMAX_DELAY);
        run_device(new_addr);
    }
}

esp_err_t rtl_usb_start(const rtl_usb_config_t *c)
{
    cfg = *c;
    if (!cfg.ring_bytes)
        cfg.ring_bytes = 256 * 1024;
    ring_storage = heap_caps_malloc(cfg.ring_bytes + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ring_storage)
        ring_storage = heap_caps_malloc(cfg.ring_bytes + 1, MALLOC_CAP_8BIT);
    if (!ring_storage)
        return ESP_ERR_NO_MEM;
    ring = xStreamBufferCreateStatic(cfg.ring_bytes, 1, ring_storage, &ring_struct);
    new_dev = xSemaphoreCreateBinary();
    ctrl_done = xSemaphoreCreateBinary();
    ctrl_lock = xSemaphoreCreateMutex();
    gone_sem = xSemaphoreCreateBinary();
    opened = xSemaphoreCreateBinary();
    esp_err_t e = ESP_OK;
    if (!cfg.host_installed) {
        usb_host_config_t host = {.skip_phy_setup = false, .intr_flags = ESP_INTR_FLAG_LEVEL1};
        e = usb_host_install(&host);
        if (e != ESP_OK)
            return e;
    }
    usb_host_client_config_t cc = {.is_synchronous = false,
                                   .max_num_event_msg = 5,
                                   .async = {.client_event_callback = client_cb, .callback_arg = NULL}};
    e = usb_host_client_register(&cc, &client);
    if (e != ESP_OK)
        return e;
    e = usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + CTRL_DATA_MAX, 0, &ctrl_xfer);
    if (e != ESP_OK)
        return e;
    int prio = cfg.task_priority ? cfg.task_priority : 18;
    if (!cfg.host_installed)
        xTaskCreatePinnedToCore(lib_task, "usb_lib", 4096, NULL, prio + 1, NULL, cfg.task_core);
    xTaskCreatePinnedToCore(client_task, "usb_client", 4096, NULL, prio, NULL, cfg.task_core);
    xTaskCreatePinnedToCore(open_task, "usb_open", 6144, NULL, prio - 8, NULL, cfg.task_core);
    ESP_LOGI(TAG, "USB host up (ring %u KB)", (unsigned)(cfg.ring_bytes / 1024));
    return ESP_OK;
}

/* ── the throughput spike ───────────────────────────────────────────────── */

int rtl_usb_spike(const uint32_t *rates, int n_rates, const int (*plans)[2], int n_plans, uint32_t freq_hz,
                  int seconds, rtl_spike_result_t *out, int max_out)
{
    int n = 0;
    enum { SINK = 16384 };
    uint8_t *sink = malloc(SINK);
    if (!sink)
        return 0;
    if (!rtl_usb_wait_open(0)) {
        free(sink);
        ESP_LOGE(TAG, "spike: no RTL-SDR open");
        return 0;
    }
    if (freq_hz)
        rtl_set_center_freq(&rtl, freq_hz);
    for (int r = 0; r < n_rates; r++) {
        if (rtl_set_sample_rate(&rtl, rates[r])) {
            ESP_LOGE(TAG, "spike: %lu S/s: %s", (unsigned long)rates[r], rtl.error);
            continue;
        }
        for (int p = 0; p < n_plans && n < max_out; p++) {
            if (rtl_usb_stream_start(plans[p][0], plans[p][1]) != ESP_OK)
                continue;
            vTaskDelay(pdMS_TO_TICKS(500)); /* let it settle; then count from a clean start */
            rtl_usb_stats_t s0, s1;
            while (rtl_usb_read(sink, SINK, 0, NULL) == SINK) {
            }
            rtl_usb_stats(&s0);
            int64_t t0 = esp_timer_get_time();
            while (esp_timer_get_time() - t0 < (int64_t)seconds * 1000000)
                rtl_usb_read(sink, SINK, pdMS_TO_TICKS(20), NULL);
            rtl_usb_stats(&s1);
            int64_t t1 = esp_timer_get_time();
            rtl_usb_stream_stop();
            rtl_spike_result_t *o = &out[n++];
            o->rate = rtl.rate;
            o->transfers = plans[p][0];
            o->transfer_bytes = plans[p][1];
            o->bytes_per_s = (double)(s1.bytes - s0.bytes) * 1e6 / (double)(t1 - t0);
            o->percent = 100.0 * o->bytes_per_s / (2.0 * rtl.rate);
            o->errors = s1.errors - s0.errors;
            o->overflows = s1.ring_overflows - s0.ring_overflows;
            o->short_transfers = s1.short_transfers - s0.short_transfers;
            ESP_LOGI(TAG,
                     "spike: %6lu S/s, %2d x %5d bytes: %7.1f KB/s of %7.1f (%5.1f%%), %lu errors, %lu short, "
                     "%lu ring overflows%s",
                     (unsigned long)o->rate, o->transfers, o->transfer_bytes, o->bytes_per_s / 1e3,
                     2.0 * o->rate / 1e3, o->percent, (unsigned long)o->errors, (unsigned long)o->short_transfers,
                     (unsigned long)o->overflows, o->percent < 99.5 ? "  <- SAMPLES LOST" : "");
        }
    }
    free(sink);
    return n;
}
