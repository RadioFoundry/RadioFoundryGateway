// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The RTL-SDR on an ESP32's USB host port (ESP-IDF usb component): waits for
 * a 0BDA:2838 / 0BDA:2832 dongle, logs its descriptors, opens it with
 * rtl_open() over control transfers, and streams the bulk IN endpoint into a
 * ring buffer (PSRAM when there is some) with several transfers in flight.
 *
 * The S3's USB is Full Speed: bulk packets are 64 bytes, at most 19 a 1 ms
 * frame, about 1.2 MB/s on the wire before host overhead. 288 kS/s of cu8 is
 * 576 KB/s. The RTL drops samples silently when its FIFO overflows, so a
 * shortfall in bytes per second is the only sign: rtl_usb_spike measures it.
 */
#ifndef RTLSDR_RTL_USB_H
#define RTLSDR_RTL_USB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "rtlsdr/rtl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    size_t ring_bytes;        /* the sample ring (PSRAM if there is some): e.g. 256 KB */
    int task_core;            /* the core for the USB tasks (tskNO_AFFINITY for any) */
    int task_priority;        /* e.g. 18: above the DSP, below Wi-Fi */
    bool bias_tee;            /* power the antenna (4.5 V on GPIO0) once open */
    bool host_installed;      /* the app runs the USB host (usb_host_install and its event task) for others too */
    int gap_fill_ms;          /* a transfer dropped because the ring was full is read back as this much filler at
                               * most (0: none), so the samples after it keep their place in time */
    /* called from the USB task once the dongle is open (dev ready to configure), and when it's gone */
    void (*on_open)(void *ctx, rtl_dev_t *dev);
    void (*on_gone)(void *ctx);
    /* called from the USB task when a dongle is there but won't open (after retries): why */
    void (*on_fail)(void *ctx, const char *why);
    /* optional, called from the USB task as each transfer completes with data: when (esp_timer us) and the bytes
     * the dongle has sent since the stream started, dropped ones included (timing its sample clock) */
    void (*on_data)(void *ctx, int64_t t_us, uint64_t bytes);
    /* optional, called just before each bulk transfer's buffer (bytes of internal DMA memory) is allocated: the
     * app's memory plan frees the block it kept for it */
    void (*before_alloc)(void *ctx, size_t bytes);
    void *ctx;
} rtl_usb_config_t;

typedef struct {
    uint64_t bytes;           /* into the ring since the stream started */
    uint32_t transfers;       /* completed */
    uint32_t short_transfers; /* completed with less than asked */
    uint32_t errors;          /* failed (not cancelled) */
    uint32_t ring_overflows;  /* transfers dropped because the ring was full */
    uint64_t bytes_lost;      /* in those transfers */
    uint64_t bytes_filled;    /* filler read in their place */
    uint32_t resubmit_fails;
    size_t ring_used, ring_peak;
    int64_t started_us;
} rtl_usb_stats_t;

/* Install the USB host (unless host_installed) and start waiting for a dongle; one already plugged in is
 * taken too. Other devices are left to the app's other USB clients. */
esp_err_t rtl_usb_start(const rtl_usb_config_t *cfg);
/* The open dongle, or NULL. */
rtl_dev_t *rtl_usb_device(void);
/* Wait until a dongle is open (or the timeout). */
bool rtl_usb_wait_open(TickType_t timeout);
/* Start streaming: reset the RTL's FIFO, then n_transfers bulk IN transfers of transfer_bytes (a multiple of the
 * endpoint's packet size) kept in flight. ESP_ERR_INVALID_STATE while a stop's transfers are still out. */
esp_err_t rtl_usb_stream_start(int n_transfers, int transfer_bytes);
/* Stop: cancel and wait for every transfer to come back. */
void rtl_usb_stream_stop(void);
bool rtl_usb_streaming(void);
/* Up to len bytes from the ring: blocks until len are there or the timeout. Returns the count. Where transfers
 * were dropped, mid-scale filler (0x80) stands in for them; *filled (if not NULL) says how many bytes of it. */
size_t rtl_usb_read(uint8_t *out, size_t len, TickType_t timeout, size_t *filled);
void rtl_usb_stats(rtl_usb_stats_t *out);
/* The bulk endpoint's packet size (64 at Full Speed, 512 at High Speed), 0 before open. */
int rtl_usb_packet_size(void);
/* The open dongle's "VID:PID serial" (many share serial 00000001), "" before open. */
const char *rtl_usb_serial(void);

/* The throughput spike: for each rate, stream `seconds` (reading and discarding), then log and return the
 * bytes per second against 2 * rate. Results: one per rate, in percent of expected. */
typedef struct {
    uint32_t rate;
    int transfers, transfer_bytes;
    double bytes_per_s, percent;
    uint32_t errors, overflows, short_transfers;
} rtl_spike_result_t;
int rtl_usb_spike(const uint32_t *rates, int n_rates, const int (*plans)[2], int n_plans, uint32_t freq_hz,
                  int seconds, rtl_spike_result_t *out, int max_out);

#ifdef __cplusplus
}
#endif

#endif
