// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * MQTT with the roamer's or repeater's topics:
 *   {root}/{name}/{app}/json   call JSON (QoS 1), network calls it sends, and the retained Receiver
 *   {root}/{name}/{app}/raw    every burst, binary (rdof/rawburst.h; QoS 0)
 *   {root}/{name}/{app}/tx     what it transmits (QoS 1)
 *   {root}/_sdr/{name}/status  retained status every 10 s and on changes; {"state": "offline"} as the last will
 * app is "roaming" or "repeater". With MQTT 5, raw and then json get topic aliases (used by QoS 0 only, so raw).
 */
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "gnss.h"

#if CONFIG_RDOF_MQTT

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "rdof/status.h"

static const char *TAG = "mqtt";

struct roam_mq {
    esp_mqtt_client_handle_t client;
    char status_topic[96];
    char client_id[64];
    char alias_buf[2][112];
    const char *alias_topics[2];
    net_mqtt_t m;
    rdof_netcalls_t calls;
    SemaphoreHandle_t calls_lock;
};

static void topic(roam_t *r, const char *kind, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s/%s/%s", r->cfg->mqtt_root, r->cfg->name, r->cfg->mqtt_app, kind);
}

void roam_publish(roam_t *r, const char *kind, const char *json, int qos)
{
    char t[112];
    if (!r->mq || !r->mq->client)
        return;
    topic(r, kind, t, sizeof t);
    net_mqtt_post(&r->mq->m, t, json, -1, qos, false);
}

void roam_publish_raw(roam_t *r, const rdof_rawburst_t *b)
{
    char t[112];
    uint8_t buf[RDOF_RAWBURST_MAX];
    if (!r->mq || !r->mq->client)
        return;
    topic(r, "raw", t, sizeof t);
    net_mqtt_post(&r->mq->m, t, buf, (int)rdof_rawburst_encode(b, buf), 0, false);
}

static void publish_receiver(roam_t *r)
{
    char buf[512], t[112];
    const rdof_location_t *loc = r->cfg->has_location ? &r->cfg->location : NULL;
    if (r->cfg->mode == RDOF_CFG_REPEATER)
        rdof_repeater_receiver_json(r->cfg->name, r->cfg->rx_freq, r->cfg->tx_freq, r->cfg->cc, r->cfg->callsign, loc,
                                  r->cfg->sfr ? r->cfg->inbound_slot : 0, buf, sizeof buf);
    else
        rdof_roamer_receiver_json(r->cfg->name, r->cfg->idle.freq, r->cfg->idle.cc, r->cfg->callsign, loc, buf, sizeof buf);
    topic(r, "json", t, sizeof t);
    net_mqtt_post(&r->mq->m, t, buf, -1, 1, true);
}

void roam_publish_status(roam_t *r)
{
    char buf[600];
    if (!r->mq || !r->mq->client)
        return;
    net_mqtt_t *m = &r->mq->m;
    const char *status_topic = r->mq->status_topic;
#if RDOF_FEATURE_REPEATER
    if (r->cfg->mode == RDOF_CFG_REPEATER && r->rpt) {
        rdof_repeater_status_t s = {now_unix(), r->cfg->rx_freq, r->cfg->tx_freq, r->cfg->cc, r->rpt->ready,
                                  r->st.hblink_up, rdof_repeater_on_air(r->rpt), {false, r->cfg->slot1, r->cfg->slot2},
                                  {false, rdof_repeater_busy(r->rpt, 1), rdof_repeater_busy(r->rpt, 2)}};
        rdof_repeater_status_json(&s, buf, sizeof buf);
        gnss_status_append(buf, strlen(buf), sizeof buf); /* where the board is */
        net_mqtt_post(m, status_topic, buf, -1, 1, true);
        return;
    }
#endif
    rdof_roamer_status_t s = {now_unix(), 0, 0, false, false, r->st.radio_ready, r->st.hblink_up, false};
    radio_channel_now(r, &s.freq, &s.cc);
#if RDOF_FEATURE_TX
    if (r->txvr) {
        s.listening = rdof_txvr_listening(r->txvr);
        s.scanning = rdof_txvr_scanning(r->txvr);
        s.radio = r->txvr->ready;
        s.on_air = r->txvr->has_call && r->txvr->call.state == RDOF_CALL_ON_AIR;
    }
#endif
    rdof_roamer_status_json(&s, buf, sizeof buf);
    gnss_status_append(buf, strlen(buf), sizeof buf); /* where the board is, with a GNSS receiver */
    net_mqtt_post(m, status_topic, buf, -1, 1, true);
}

void roam_tx_event(roam_t *r, const char *action, const rdof_tx_call_info_t *call, double duration)
{
    char buf[512];
    double t = now_unix();
    struct roam_mq *q = r->mq;
    if (!q || !q->client)
        return;
    rdof_tx_json(action, call, duration, t, buf, sizeof buf);
    roam_publish(r, "tx", buf, 1);
    xSemaphoreTake(q->calls_lock, portMAX_DELAY);
    bool any = rdof_netcalls_event(&q->calls, action, call, duration, t, buf, sizeof buf);
    xSemaphoreGive(q->calls_lock);
    if (any)
        roam_publish(r, "json", buf, 1);
    if (r->cfg->mode == RDOF_CFG_ROAMER)
        roam_publish_status(r);
}

void roam_tx_burst(roam_t *r, const rdof_tx_call_info_t *call, const rdof_net_burst_t *b)
{
#if CONFIG_RDOF_MQTT_RAW
    rdof_rawburst_t raw;
    struct roam_mq *q = r->mq;
    if (!q || !q->client)
        return;
    xSemaphoreTake(q->calls_lock, portMAX_DELAY);
    bool any = rdof_netcalls_burst_raw(&q->calls, call, b->control, b->burst, now_unix(), &raw);
    xSemaphoreGive(q->calls_lock);
    if (any)
        roam_publish_raw(r, &raw);
#endif
}

static void on_connected(void *ctx)
{
    publish_receiver(ctx);
    roam_publish_status(ctx);
}

static void heartbeat_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        roam_publish_status(arg);
    }
}

void roam_mqtt_start(roam_t *r)
{
    const rdof_config_t *c = r->cfg;
    /* the first radio's in internal RAM as before; another's in PSRAM */
    struct roam_mq *q = r->i ? heap_caps_calloc(1, sizeof *q, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (!q)
        q = calloc(1, sizeof *q);
    q->calls_lock = xSemaphoreCreateMutex();
    rdof_netcalls_init(&q->calls);
    r->mq = q;
    if (!c->has_mqtt || !c->mqtt_host[0]) {
        ESP_LOGW(TAG, "no mqtt in the settings: MQTT off");
        return;
    }
    snprintf(q->status_topic, sizeof q->status_topic, "%s/_sdr/%s/status", c->mqtt_root, c->name);
    snprintf(q->client_id, sizeof q->client_id, "rdof-%s-%s", c->mode == RDOF_CFG_ROAMER ? "roaming" : "repeater",
             c->name);
    topic(r, "raw", q->alias_buf[0], sizeof q->alias_buf[0]);
    topic(r, "json", q->alias_buf[1], sizeof q->alias_buf[1]);
    q->alias_topics[0] = q->alias_buf[0];
    q->alias_topics[1] = q->alias_buf[1];
    q->m = (net_mqtt_t){.host = c->mqtt_host, .user = c->mqtt_user, .pass = c->mqtt_pass, .port = c->mqtt_port,
                        .tls = c->mqtt_tls, .client_id = q->client_id, .will_topic = q->status_topic,
                        .will_msg = RDOF_STATUS_OFFLINE, .outbox_kb = 32, .queue_kb = 16,
                        .on_connected = on_connected, .ctx = r, .up = &r->st.mqtt_up,
                        .alias_topics = q->alias_topics, .n_alias_topics = 2};
    q->client = net_mqtt_start(&q->m);
    roam_task(r, heartbeat_task, "roam_status", 3072, 4);
}

#else

void roam_mqtt_start(roam_t *r) {}
void roam_publish(roam_t *r, const char *kind, const char *json, int qos) {}
void roam_publish_raw(roam_t *r, const rdof_rawburst_t *b) {}
void roam_publish_status(roam_t *r) {}
void roam_tx_event(roam_t *r, const char *action, const rdof_tx_call_info_t *call, double duration) {}
void roam_tx_burst(roam_t *r, const rdof_tx_call_info_t *call, const rdof_net_burst_t *b) {}

#endif
