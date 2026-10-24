// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * MQTT with the SDR's topics:
 *   {root}/{channel}/{app}/json   call JSON (QoS 1); retained Receiver message per channel
 *   {root}/{channel}/{app}/raw    every non-idle burst, binary (rdof/rawburst.h; QoS 0)
 *   {root}/_sdr/{name}/status     retained status every status_seconds; {"state":"offline"} as the last will
 * The DSP cores post calls and bursts to net_mqtt_post's queue, whose task publishes them: publishing from the DSP
 * would hold it up for each connect attempt while the broker is unreachable. With MQTT 5 the topics get aliases, every
 * channel's raw first, then their json (used by QoS 0 only, so raw).
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "mem.h"
#include "heard.h"

#if CONFIG_RDOF_MQTT

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "gnss.h"
#include "rdof/sdr/status.h"
#include "rdof/status.h"

static const char *TAG = "mqtt";
static esp_mqtt_client_handle_t client;
static char status_topic[96];
static net_mqtt_t m;
static char alias_buf[2 * RDOF_SDR_MAX_CHANNELS][112];
static const char *alias_topics[2 * RDOF_SDR_MAX_CHANNELS];

static void topic(const char *channel, const char *kind, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s/%s/%s", g_scfg.mqtt_root, channel, g_scfg.mqtt_app, kind);
}

static void on_json(void *ctx, const char *ch, const char *json)
{
    char t[112];
    if (sdr_is_tap(ch)) /* a bench retune's taps stay off MQTT and last heard */
        return;
    heard_rf_json("sdr", g_scfg.name, ch, json);
    if (!client)
        return;
    topic(ch, "json", t, sizeof t);
    net_mqtt_post(&m, t, json, -1, 1, false);
}

static void on_raw_burst(void *ctx, const char *ch, const rdof_rawburst_t *r)
{
    if (r->dt == RDOF_DT_PI_HEADER && !sdr_is_tap(ch))
        heard_burst(g_scfg.name, ch, r->slot, r->dt);
#if CONFIG_RDOF_MQTT_RAW
    char t[112];
    uint8_t b[RDOF_RAWBURST_MAX];
    if (!client || sdr_is_tap(ch))
        return;
    topic(ch, "raw", t, sizeof t);
    net_mqtt_post(&m, t, b, (int)rdof_rawburst_encode(r, b), 0, false);
#endif
}

rdof_sink_t sdr_mqtt_sink(void)
{
    rdof_sink_t s = {.on_json = on_json, .on_raw_burst = on_raw_burst};
    return s;
}

void sdr_publish_receivers(void)
{
    char buf[512], t[112];
    if (!client)
        return;
    for (int c = 0; c < g_scfg.nch; c++) {
        const rdof_sdr_cfg_channel_t *ch = &g_scfg.ch[c];
        rdof_sdr_receiver_json(ch->name[0] ? ch->name : ch->id, ch->freq, ch->cc, g_scfg.name, ch->tg,
                             g_scfg.has_location ? &g_scfg.location : NULL, buf, sizeof buf);
        topic(ch->id, "json", t, sizeof t);
        net_mqtt_post(&m, t, buf, -1, 1, true);
    }
}

void sdr_publish_status(void)
{
    static char buf[3072];
    if (!client || !g_rx_ready)
        return;
    const char *names[RDOF_SDR_MAX_CHANNELS];
    const char *hb_ids[RDOF_SDR_MAX_CHANNELS];
    bool hb_up[RDOF_SDR_MAX_CHANNELS];
    const rdof_sdr_cfg_channel_t *rc;
    for (int k = 0; k < RDOF_SDR_MAX_CHANNELS && (rc = sdr_rx_cfg_channel(k)) != NULL; k++) /* the receiver's */
        names[k] = rc->name;
    rdof_sdr_status_t st = {.name = g_scfg.name, .at = now_unix(), .names = names};
    sdr_counters(&st.dropped_chunks, &st.overflows, &st.cpu_load);
    sdr_level(&st.has_level, &st.clip_pct, &st.peak_dbfs, &st.has_gain, &st.gain_db, &st.has_agc, &st.agc_ceiling_db,
              &st.agc_min_db, &st.agc_changes);
    st.n_hblink = sdr_hb_status(hb_ids, hb_up, RDOF_SDR_MAX_CHANNELS);
    st.hblink_ids = hb_ids;
    st.hblink_up = hb_up;
    rx_lock();
    for (int k = 0; k < g_rx.C; k++) /* a bench retune's taps (the last channels): not here */
        st.leave_out += sdr_rx_cfg_channel(k) && sdr_is_tap(sdr_rx_cfg_channel(k)->id);
    size_t n = rdof_sdr_status_json(&g_rx, &st, buf, sizeof buf);
    rx_unlock();
    if (n)
        n = gnss_status_append(buf, n, sizeof buf); /* where the board is, with a GNSS receiver */
    if (n)
        net_mqtt_post(&m, status_topic, buf, -1, 1, true);
}

static void on_connected(void *ctx)
{
    sdr_publish_receivers();
    sdr_publish_status();
}

static void status_task(void *arg)
{
    const int ms = (int)(g_scfg.status_seconds > 1 ? g_scfg.status_seconds * 1000 : 30000);
    vTaskDelay(pdMS_TO_TICKS(5000)); /* the first status comes 5 s in */
    for (;;) {
        sdr_publish_status();
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
}

void sdr_mqtt_start(void)
{
    static char client_id[64];
    if (!g_scfg.has_mqtt || !g_scfg.mqtt_host[0]) {
        ESP_LOGW(TAG, "no mqtt in the settings: MQTT off");
        return;
    }
    snprintf(status_topic, sizeof status_topic, "%s/_sdr/%s/status", g_scfg.mqtt_root, g_scfg.name);
    snprintf(client_id, sizeof client_id, "rdof-sdr-%s", g_scfg.name);
    for (int k = 0; k < 2; k++) /* aliases: every channel's raw, then every channel's json */
        for (int c = 0; c < g_scfg.nch; c++) {
            topic(g_scfg.ch[c].id, k ? "json" : "raw", alias_buf[k * g_scfg.nch + c], sizeof alias_buf[0]);
            alias_topics[k * g_scfg.nch + c] = alias_buf[k * g_scfg.nch + c];
        }
    m = (net_mqtt_t){.host = g_scfg.mqtt_host, .user = g_scfg.mqtt_user, .pass = g_scfg.mqtt_pass,
                     .port = g_scfg.mqtt_port, .tls = g_scfg.mqtt_tls, .client_id = client_id,
                     .will_topic = status_topic, .will_msg = RDOF_STATUS_OFFLINE, .outbox_kb = 64,
                     .queue_kb = 32, .on_connected = on_connected, .up = &g_sstat.mqtt_up,
                     .alias_topics = alias_topics, .n_alias_topics = 2 * g_scfg.nch};
    client = net_mqtt_start(&m);
    mem_task("sdr_net", "sdr_status", status_task, 4096, NULL, 4, 0, NULL);
}

uint32_t sdr_mqtt_dropped(void) { return m.dropped; }

#else

uint32_t sdr_mqtt_dropped(void) { return 0; }
static void none(void *ctx, const char *ch, const char *json)
{
    if (!sdr_is_tap(ch))
        heard_rf_json("sdr", g_scfg.name, ch, json);
}
static void none_raw(void *ctx, const char *ch, const rdof_rawburst_t *r)
{
    if (r->dt == RDOF_DT_PI_HEADER && !sdr_is_tap(ch))
        heard_burst(g_scfg.name, ch, r->slot, r->dt);
}
rdof_sink_t sdr_mqtt_sink(void)
{
    rdof_sink_t s = {.on_json = none, .on_raw_burst = none_raw}; /* no MQTT: last heard only */
    return s;
}
void sdr_mqtt_start(void) {}
void sdr_publish_receivers(void) {}
void sdr_publish_status(void) {}

#endif
