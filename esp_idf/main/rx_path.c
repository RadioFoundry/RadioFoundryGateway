// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The radio as an ordinary receiver: what the
 * roamer or repeater passes on goes through the call tracker, to MQTT as
 * MMDVMHost-style JSON and raw bursts, and to HBlink4 as DMRD from this peer.
 * Bursts are queued and handled on their own task, so network writes never
 * hold up the modem.
 */
#include <math.h>
#include <string.h>

#include "app.h"
#include "heard.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "rdof/tracker.h"

static const char *TAG = "rx";

typedef struct {
    uint8_t slot, control;
    uint8_t burst[RDOF_BURST_BYTES];
    int cc;
    uint32_t freq;
    float dbm;
} heard_t;

struct roam_rx {
    QueueHandle_t queue;
#if RDOF_FEATURE_TRACKER
    rdof_tracker_t tracker;
    rdof_hb_sink_t hb_sink;
#endif
};

void rx_hear(roam_t *r, int slot, uint8_t control, const uint8_t *burst, double dbm, int cc, uint32_t freq)
{
    heard_t h = {.slot = (uint8_t)slot, .control = control, .cc = cc, .freq = freq, .dbm = (float)dbm};
    memcpy(h.burst, burst, RDOF_BURST_BYTES);
    if (r->rx && xQueueSend(r->rx->queue, &h, 0) != pdTRUE)
        ESP_LOGW(TAG, "receive queue full: burst dropped");
}

#if RDOF_FEATURE_TRACKER

static const char *role(const roam_t *r)
{
    return r->cfg->mode == RDOF_CFG_ROAMER ? "roamer" : "repeater";
}

static void on_json(void *ctx, const char *channel, const char *json)
{
    roam_t *r = ctx;
    heard_rf_json(role(r), r->cfg->name, "", json); /* last heard: RF */
    roam_publish(ctx, "json", json, 1);
    ESP_LOGI(TAG, "%s", json);
}

static void on_raw_burst(void *ctx, const char *channel, const rdof_rawburst_t *b)
{
    heard_burst(((roam_t *)ctx)->cfg->name, "", b->slot, b->dt); /* a PI header: the call's encrypted */
#if CONFIG_RDOF_MQTT_RAW
    roam_publish_raw(ctx, b);
#endif
}

static void on_call(void *ctx, const char *channel, const char *action, uint32_t src, uint32_t dst)
{
    roam_t *r = ctx;
#if RDOF_FEATURE_TX
    if (r->cfg->mode == RDOF_CFG_ROAMER && r->txvr)
        rdof_txvr_heard_call(r->txvr, action, src, dst); /* listen-before-talk's last call heard */
#endif
    if (!strcmp(action, "start") || !strcmp(action, "late_entry")) {
        snprintf(r->st.heard, sizeof r->st.heard, "%lu>%lu", (unsigned long)src, (unsigned long)dst);
        r->st.hearing = true;
    } else {
        r->st.hearing = false;
    }
}

static void hb_out(void *ctx, const uint8_t *data, size_t len)
{
    roam_hb_send(ctx, data, len);
}

static void hb_before(void *ctx)
{
    roam_hb_send_config(ctx, false); /* HBlink4 credits a stream to the channel in the peer's last DMRC */
}

static uint32_t hb_rand(void *ctx)
{
    return esp_random();
}

static void rx_task(void *arg)
{
    roam_t *r = arg;
    rdof_tracker_t *tracker = &r->rx->tracker;
    rdof_hb_sink_t *hb_sink = &r->rx->hb_sink;
    bool warned = false;
    rdof_tracker_init(tracker, r->cfg->name, 0.0, 0.0);
    rdof_sink_t out = {.on_json = on_json, .ctx = r, .on_call = on_call, .on_raw_burst = on_raw_burst};
    rdof_tracker_add_sink(tracker, out);
    if (r->cfg->receiver_hblink) {
        rdof_hb_sink_init(hb_sink, r->cfg->peer_id);
        hb_sink->send = hb_out;
        if (r->cfg->mode == RDOF_CFG_ROAMER)
            hb_sink->before_stream = hb_before;
        hb_sink->rand32 = hb_rand;
        hb_sink->ctx = r;
        rdof_tracker_add_sink(tracker, rdof_hb_sink_as_sink(hb_sink));
    }
#if CONFIG_RDOF_IPGW
    rdof_sink_t gs; /* data bursts to the IP gateway, at the frequency the radio is on */
    if (ipgw_sink(IPGW_SOURCE_RADIO + r->i, 0, -1, &tracker->freq_hz, &gs))
        rdof_tracker_add_sink(tracker, gs);
#endif
    for (;;) {
        heard_t h;
        if (xQueueReceive(r->rx->queue, &h, pdMS_TO_TICKS(100)) == pdTRUE) {
            double t = now_unix();
            if (t < 1.6e9 && !warned) {
                ESP_LOGW(TAG, "no time from SNTP yet: timestamps are wrong");
                warned = true;
            }
            rdof_burst_t b;
            if (h.freq)
                tracker->freq_hz = h.freq;
            rdof_modem_burst(h.control, h.burst, h.dbm, h.cc, h.slot, &b);
            rdof_tracker_feed(tracker, &b, t);
        }
        rdof_tracker_tick(tracker, now_unix());
    }
}

#else

static void rx_task(void *arg)
{
    roam_t *r = arg;
    for (;;) {
        heard_t h;
        xQueueReceive(r->rx->queue, &h, portMAX_DELAY); /* built without the tracker */
    }
}

#endif

void roam_rx_start(roam_t *r)
{
    /* the first radio's in internal RAM as before; another's in PSRAM */
    r->rx = r->i ? heap_caps_calloc(1, sizeof *r->rx, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (!r->rx)
        r->rx = calloc(1, sizeof *r->rx);
    r->rx->queue = xQueueCreate(64, sizeof(heard_t));
    roam_task(r, rx_task, "rx", 8192, 7);
}
