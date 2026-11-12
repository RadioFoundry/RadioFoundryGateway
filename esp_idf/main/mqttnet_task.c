// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The MQTT network on the board (docs/spec/fw/mqtt-net.md, rdof/mqttnet.h): an mqtt network with calls and format
 * "v3" carries the calls as call messages, on a connection of its own (client rdof-net-{name}, will online "0").
 *
 * Up: each receiver's tracker (the SDR's channels, each radio's) hands its frames to mn_sink's queue; the task runs
 * them through an uplink per receiver (rdof_mn_up) and publishes on {root}/tg|pc/{dst}/{node}. Down: esp-mqtt's
 * task copies {root}/dl/{node} and the subscribed {root}/tg/{tg}/+ into a message buffer; the task runs them through
 * the downlink (rdof_mn_down) to the router's transmitters (site_mn_burst). Confirmed data heard here and sent up is
 * answered on RF with an ACK (section 4.4). The transmitters' tx events come through mn_tx_event's queue.
 *
 * Everything lives in PSRAM: the uplinks, the downlink, the queues and the task's stack (it never writes flash).
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app.h"

#if CONFIG_RDOF_MQTT && CONFIG_RDOF_ROAMER && RDOF_FEATURE_TRACKER

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/message_buffer.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mqtt_client.h"
#if CONFIG_RDOF_MQTT_TLS
#include "esp_crt_bundle.h"
#endif
#if CONFIG_MQTT_PROTOCOL_5
#include "mqtt5_client.h"
#endif
#include "rdof/bits.h"
#include "rdof/ipgw.h"
#include "rdof/ipgwtx.h"
#include "rdof/mmdvm.h"
#include "rdof/mnctl.h"
#include "rdof/mqttnet.h"
#if CONFIG_RDOF_SDR
#include "rdof/sdr/config.h"
#endif

static const char *TAG = "mqtt-net";

#define MAX_SOURCES (24 + RDOF_MAX_RADIOS) /* the SDR's channels and taps, then the radios */
#define FRAMES 96                          /* frames queued for the uplinks */
#define IN_KB 16                           /* messages queued from the broker */
#define TXQ 8                              /* tx events queued */
#define TOPIC_MAX 96

typedef struct {
    uint8_t source;
    rdof_frame_t f;
} frame_in_t;

typedef struct {
    bool end;
    uint32_t stream, src, dst, freq;
    int slot;
} tx_in_t;

typedef struct {
    bool used;
    int id; /* mn_sink's source */
    volatile uint32_t freq;
    const double *freq_now;
    rdof_mn_up_t *up;
} source_t;

typedef struct {
    const rdof_site_t *site;
    int net;
    char node[RDOF_MN_NODE_MAX + 1], root[16];
    char online[TOPIC_MAX], info[TOPIC_MAX], act[TOPIC_MAX], dl[TOPIC_MAX], status[TOPIC_MAX];
    esp_mqtt_client_handle_t client;
    volatile bool up, connected_edge; /* connected_edge: set by esp-mqtt's task, cleared by the task */
    volatile bool v5;        /* this connection is MQTT 5 (else 3.1.1: the broker didn't take 5) */
    volatile bool fall_back; /* refused MQTT 5: the task reconnects on 3.1.1 */
    bool tls, use_tls;
    QueueHandle_t frames, txq;
    SemaphoreHandle_t sink_lock; /* mn_sink's */
    MessageBufferHandle_t in;
    source_t src[MAX_SOURCES];
    rdof_mn_down_t down;
    rdof_mn_results_t results;
    /* the filters subscribed now, and ones whose unsubscribe failed (tried again); the task's only */
    char subs[2 * (RDOF_MN_SUBS + 1 + 2 * RDOF_MN_CLAIM_SUBS)][TOPIC_MAX];
    int nsubs;
    uint8_t msg[RDOF_MN_MSG_MAX + TOPIC_MAX + 2];
    char json[4096];
    uint32_t in_dropped, frames_dropped, acks;
} mn_t;

static mn_t *M;

static double mono(void) { return esp_timer_get_time() / 1e6; }
static double wall(void *ctx) { return now_unix(); }
static uint32_t rand32(void *ctx) { return esp_random(); }

static uint32_t hz_of(const source_t *s)
{
    return s->freq_now && *s->freq_now > 0 ? (uint32_t)*s->freq_now : s->freq;
}

static void pub(const char *topic, const void *data, int len, int qos, bool retain)
{
    if (M->up)
        esp_mqtt_client_publish(M->client, topic, data, len, qos, retain);
}

/* ── the uplinks' hooks (the task's) ───────────────────────────────────────── */

static int path_of(void *ctx, int slot) { return site_mn_path_at(hz_of(ctx), slot); }

static bool admit(void *ctx, int slot, uint32_t src, uint32_t dst, bool group)
{
    return site_rf_net(hz_of(ctx), slot, src, dst, group) == M->net;
}

static void publish(void *ctx, int kind, uint32_t dst, const uint8_t *msg, size_t len)
{
    char t[TOPIC_MAX];
    if (rdof_mn_topic(t, sizeof t, M->root, kind, dst, M->node))
        pub(t, msg, (int)len, 0, false);
}

static void activity(void *ctx, const char *json) { pub(M->act, json, 0, 1, false); }

static void result(const char *r, const rdof_mn_pending_t *p)
{
    char buf[512];
    if (rdof_mn_data_result_event(M->node, now_unix(), p->path, p->slot, p->stream, p->src, p->dst, p->from,
                                  p->stream, r, 1, buf, sizeof buf))
        pub(M->act, buf, 0, 1, false);
    ESP_LOGI(TAG, "data %lu > %lu from %s: %s", (unsigned long)p->src, (unsigned long)p->dst, p->from, r);
}

static void response(void *ctx, int slot, uint32_t src, uint32_t dst, int klass, int type, int status)
{
    rdof_mn_pending_t p;
    const char *r = rdof_mn_results_response(&M->results, src, dst, klass, type, &p);
    if (r)
        result(r, &p);
}

/* Confirmed data heard here and sent up: the radio's ACK from this box, on the path it was heard on, unless the
   called radio was last heard there too (the radios answer each other). */
static void data_done(void *ctx, const rdof_mn_data_t *d)
{
#if RDOF_FEATURE_DATA_BLOCKS && RDOF_FEATURE_TX
    if (!d->confirmed || d->group || !d->sent || d->path < 0 || site_heard_path(d->dst) == d->path)
        return;
    const rdof_path_t *p = &M->site->path[d->path];
    if (p->tx_freq < 0)
        return;
    static EXT_RAM_BSS_ATTR rdof_ipgw_dmr_t ack;
    const int cc = M->site->freq[p->tx_freq].cc >= 0 ? M->site->freq[p->tx_freq].cc : 1;
    const bool rpt = p->app >= 0;
    rdof_ipgw_build_ack(d->dst, d->src, d->sap, d->ns, cc, rdof_data_sync(rpt ? RDOF_BS_VOICE : RDOF_MS_VOICE), &ack);
    uint8_t sid[4];
    rdof_put_be32(sid, esp_random());
    for (int i = 0; i < ack.nbursts; i++) {
        rdof_net_burst_t b;
        rdof_ipgw_net_burst(&ack, i, sid, d->slot, (uint8_t)i, &b);
        if (!site_app_burst(d->path, &b, true, INFINITY) && !site_mn_burst(1ull << d->path, false, &b)) /* a repeater's path */
            return;
    }
    M->acks++;
    ESP_LOGI(TAG, "answered %lu's confirmed data to %lu (N(S) %d)", (unsigned long)d->src, (unsigned long)d->dst,
             d->ns);
#endif
}

/* ── the downlink's hooks ──────────────────────────────────────────────────── */

/* A dl message goes out on the path in its path byte; a tg message, or path 0xFF, where the router says (its TG
   paths; a private call where the radio was last heard). */
static void burst(void *ctx, const rdof_net_burst_t *b, int path, bool dl, uint8_t flags, const char *from)
{
    uint64_t only = ~0ull;
    if (dl && path != RDOF_MN_PATH_NONE) {
        if (path >= M->site->npath)
            return;
        only = 1ull << path;
    }
    site_mn_burst(only, (flags & RDOF_MN_F_EMERGENCY) != 0, b);
}

/* The path and slot are this box's: a dl message's path byte, else not known until the transmitter ends it (a tg
   message's path byte and slot flag are the publisher's). */
static void data(void *ctx, const rdof_mn_hdr_t *h, bool confirmed, bool dl, const char *from)
{
    const int k = dl && h->path < M->site->npath ? h->path : -1;
    const rdof_path_t *p = k >= 0 ? &M->site->path[k] : NULL;
    rdof_mn_results_add(&M->results, h, confirmed, from, k, p ? (p->slot ? p->slot : p->tx_slot) : 0, mono());
}

/* A call setup on our dl (mqtt-net.md section 9.4). This box is Tier II: it can't ask the radio, so it answers a
   request at once from its own last heard (rdof_mn_setup_tier2): accept (unconfirmed) when the called radio was heard
   on a path of this network that can transmit, else reject not_found. The voice then comes as an ordinary private
   call. The setup and the answer go in activity. */
static void on_setup(void *ctx, const rdof_mn_hdr_t *h, const rdof_mn_setup_t *s, bool dl, const char *from)
{
    static EXT_RAM_BSS_ATTR rdof_mn_tier2_t t; /* the task's only */
    rdof_mn_setup_tier2(M->site, M->net, M->node, now_unix(), h, s, dl, from,
                        dl && s->op == RDOF_MN_SETUP_REQUEST ? site_heard_path(h->dst) : -1, esp_random(), &t);
    for (int i = 0; i < t.nev; i++)
        pub(M->act, t.ev[i], 0, 1, false);
    if (t.len)
        publish(NULL, RDOF_MN_T_PC, t.to, t.msg, t.len);
}

/* ── in: the trackers' sinks, esp-mqtt's task, the transmitters ───────────── */

static void on_frame(void *ctx, const char *channel, const rdof_frame_t *f)
{
    mn_t *m = M;
    if (!m) /* mn_start gave up after this sink was handed out */
        return;
    frame_in_t in = {.source = (uint8_t)((source_t *)ctx - m->src), .f = *f};
    if (xQueueSend(m->frames, &in, 0) != pdTRUE)
        m->frames_dropped++;
}

/* Called from each receiver's task as it sets up (the SDR's, each radio's), maybe at once and more than once. */
bool mn_sink(int source, uint32_t freq, const double *freq_now, rdof_sink_t *out)
{
    mn_t *m = M;
    if (!m)
        return false;
    xSemaphoreTake(m->sink_lock, portMAX_DELAY);
    if (M != m) { /* mn_start gave up meanwhile */
        xSemaphoreGive(m->sink_lock);
        return false;
    }
    source_t *s = NULL;
    for (int i = 0; i < MAX_SOURCES && !s; i++)
        if (M->src[i].used && M->src[i].id == source)
            s = &M->src[i];
    if (s) { /* the receiver set up again (a retune, a restart): the same uplink, its calls time out on their own */
        s->freq_now = freq_now;
        s->freq = freq;
        xSemaphoreGive(M->sink_lock);
        *out = (rdof_sink_t){.on_frame = on_frame, .ctx = s};
        return true;
    }
    for (int i = 0; i < MAX_SOURCES && !s; i++)
        if (!M->src[i].used)
            s = &M->src[i];
    rdof_mn_up_t *u = s ? heap_caps_calloc(1, sizeof *u, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (!u) {
        ESP_LOGE(TAG, "no %s for receiver %d's uplink", s ? "memory" : "room", source);
        xSemaphoreGive(M->sink_lock);
        return false;
    }
    rdof_mn_up_init(u, M->node);
    u->path_of = path_of;
    u->admit = admit;
    u->publish = publish;
    u->activity = activity;
    u->data_done = data_done;
    u->response = response;
    u->rand32 = rand32;
    u->wall = wall;
    u->ctx = s;
    for (int k = 0; k < M->site->npath && k < 64; k++)
        if (M->site->path[k].tx_freq >= 0)
            u->tx_ok |= 1ull << k;
    s->id = source;
    s->freq = freq;
    s->freq_now = freq_now;
    s->up = u;
    __atomic_store_n(&s->used, true, __ATOMIC_RELEASE); /* last: the task polls the ones in use */
    xSemaphoreGive(M->sink_lock);
    *out = (rdof_sink_t){.on_frame = on_frame, .ctx = s};
    return true;
}

void mn_tx_event(const char *action, const rdof_tx_call_info_t *c)
{
    if (!M)
        return;
    const bool end = !strcmp(action, "end") || !strcmp(action, "lost");
    if (!end && strcmp(action, "start"))
        return;
    const tx_in_t t = {.end = end, .stream = rdof_be32(c->stream_id), .src = c->src, .dst = c->dst, .freq = c->freq,
                       .slot = c->slot};
    xQueueSend(M->txq, &t, 0);
}

/* TCP_NODELAY on the connection's socket (mqtt-net.md section 5), so a message never waits for the next one. esp-mqtt
   has no setting for it; its transport gives the socket once connected. */
static void nodelay(void)
{
    esp_transport_handle_t t =
        esp_mqtt_client_get_transport(M->client, (char *)(M->use_tls ? MQTT_OVER_SSL_SCHEME : MQTT_OVER_TCP_SCHEME));
    const int fd = t ? esp_transport_get_socket(t) : -1;
    int one = 1;
    if (fd < 0 || setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one))
        ESP_LOGW(TAG, "no TCP_NODELAY on the connection (socket %d)", fd);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *ev)
{
    esp_mqtt_event_handle_t e = ev;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        M->v5 = e->protocol_ver == MQTT_PROTOCOL_V_5;
        ESP_LOGI(TAG, "connected to the broker, node %s (MQTT %s)", M->node, M->v5 ? "5" : "3.1.1");
        nodelay();
        /* a new session: the task subscribes again (it owns the list) */
        M->up = true;
        M->connected_edge = true;
        break;
    case MQTT_EVENT_DISCONNECTED:
        if (M->up)
            ESP_LOGW(TAG, "disconnected from the broker");
        M->up = false;
        break;
    case MQTT_EVENT_ERROR:
        if (e->error_handle && e->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            const int code = e->error_handle->connect_return_code;
            /* 3.1.1's "unacceptable protocol version" (1) or 5's "unsupported protocol version" (0x84): an older
             * broker. The node drops its own messages by their topic, so No Local isn't needed (mqtt-net.md 4) */
            if (e->protocol_ver == MQTT_PROTOCOL_V_5 && (code == 1 || code == 0x84))
                M->fall_back = true;
            else
                ESP_LOGW(TAG, "refused by the broker (check the username / password)");
        }
        break;
    case MQTT_EVENT_DATA: {
        /* whole messages only (the buffer takes the largest call message); [topic length][topic][message] */
        if (e->current_data_offset || e->data_len != e->total_data_len || e->topic_len <= 0 ||
            e->topic_len >= TOPIC_MAX || e->data_len <= 0 || e->data_len > RDOF_MN_MSG_MAX) {
            M->in_dropped++;
            break;
        }
        static EXT_RAM_BSS_ATTR uint8_t buf[RDOF_MN_MSG_MAX + TOPIC_MAX + 1]; /* esp-mqtt's task only */
        buf[0] = (uint8_t)e->topic_len;
        memcpy(buf + 1, e->topic, (size_t)e->topic_len);
        memcpy(buf + 1 + e->topic_len, e->data, (size_t)e->data_len);
        const size_t n = 1 + (size_t)e->topic_len + (size_t)e->data_len;
        if (xMessageBufferSend(M->in, buf, n, 0) != n)
            M->in_dropped++;
        break;
    }
    default:
        break;
    }
}

/* ── the task ──────────────────────────────────────────────────────────────── */

/* The filters this box wants (its dl, its talkgroups); each change is made with the broker. A subscribe or
   unsubscribe esp-mqtt refuses (not connected, outbox full) isn't recorded as done: it's tried again. False: one
   failed. */
static bool subscribe(void)
{
    static EXT_RAM_BSS_ATTR char want[RDOF_MN_SUBS + 1 + 2 * RDOF_MN_CLAIM_SUBS][TOPIC_MAX];
    static EXT_RAM_BSS_ATTR char now[2 * (RDOF_MN_SUBS + 1 + 2 * RDOF_MN_CLAIM_SUBS)][TOPIC_MAX];
    uint32_t tgs[RDOF_MN_SUBS];
    bool all = false, ok = true;
    const int n = site_mn_sub_tgs(tgs, RDOF_MN_SUBS - 1, &all);
    int k = 0, m = 0;
    snprintf(want[k++], TOPIC_MAX, "%s", M->dl);
    if (all)
        snprintf(want[k++], TOPIC_MAX, "%s/tg/+/+", M->root);
    else
        for (int i = 0; i < n; i++)
            snprintf(want[k++], TOPIC_MAX, "%s/tg/%lu/+", M->root, (unsigned long)tgs[i]);
    /* the IDs the box's own apps answer for (mqtt-net.md 9.2): their individual calls come on pc, not through the
       router; a talkgroup an app takes, unless the paths' subscriptions have it */
    uint32_t ids[RDOF_MN_CLAIM_SUBS];
    int nc = rdof_mn_claim_ids(M->site, 0, ids, RDOF_MN_CLAIM_SUBS);
    for (int i = 0; i < nc; i++)
        snprintf(want[k++], TOPIC_MAX, "%s/pc/%lu/+", M->root, (unsigned long)ids[i]);
    nc = all ? 0 : rdof_mn_claim_ids(M->site, 1, ids, RDOF_MN_CLAIM_SUBS);
    for (int i = 0; i < nc; i++) {
        bool had = false;
        for (int q = 0; q < n && !had; q++)
            had = tgs[q] == ids[i];
        if (!had)
            snprintf(want[k++], TOPIC_MAX, "%s/tg/%lu/+", M->root, (unsigned long)ids[i]);
    }
    for (int i = 0; i < M->nsubs; i++) { /* dropped ones; one that won't go stays listed, to try again */
        bool keep = false;
        for (int j = 0; j < k && !keep; j++)
            keep = !strcmp(M->subs[i], want[j]);
        if (!keep && esp_mqtt_client_unsubscribe(M->client, M->subs[i]) < 0) {
            ok = false;
            if (m < (int)(sizeof now / sizeof now[0]))
                snprintf(now[m++], TOPIC_MAX, "%s", M->subs[i]);
        }
    }
    for (int j = 0; j < k; j++) { /* new ones */
        bool had = false;
        for (int i = 0; i < M->nsubs && !had; i++)
            had = !strcmp(M->subs[i], want[j]);
        if (!had) {
#if CONFIG_MQTT_PROTOCOL_5
            if (M->v5) {
                const esp_mqtt5_subscribe_property_config_t sp = {.no_local_flag = true}; /* our own stay out */
                esp_mqtt5_client_set_subscribe_property(M->client, &sp);
            }
#endif
            if (esp_mqtt_client_subscribe_single(M->client, want[j], 0) < 0) {
                ok = false;
                continue;
            }
        }
        if (m < (int)(sizeof now / sizeof now[0]))
            snprintf(now[m++], TOPIC_MAX, "%s", want[j]);
    }
    memcpy(M->subs, now, sizeof now);
    M->nsubs = m;
    return ok;
}

static void publish_info(void)
{
    const rdof_mn_info_t in = {.node = M->node, .name = M->site->name, .firmware = esp_app_get_description()->version,
                               .transport = M->tls ? "mqtt/tls" : "mqtt/tcp", .dmr_id = M->site->id, .bundle = 1};
    if (rdof_mn_info_json(M->site, M->net, &in, M->json, sizeof M->json))
        pub(M->info, M->json, 0, 1, true);
}

/* status (mqtt-net.md §16), retained: the node, its firmware and the time since boot */
static void publish_status(double now)
{
    const rdof_mn_status_t st = {.node = M->node, .firmware = esp_app_get_description()->version, .wall = now_unix(),
                                 .uptime_s = now};
    if (rdof_mn_status_json(&st, M->json, sizeof M->json))
        pub(M->status, M->json, 0, 1, true);
}

static void handle_message(size_t n)
{
    const size_t tl = M->msg[0];
    if (n < 1 + tl)
        return;
    char topic[TOPIC_MAX];
    memcpy(topic, M->msg + 1, tl);
    topic[tl] = 0;
    const uint8_t *msg = M->msg + 1 + tl;
    const size_t len = n - 1 - tl;
    uint32_t id;
    char node[RDOF_MN_NODE_MAX + 1];
    const int kind = rdof_mn_topic_parse(topic, M->root, &id, node, sizeof node);
    if (kind == RDOF_MN_T_DL && !strcmp(node, M->node)) /* from the router: the node in the topic is ours */
        rdof_mn_down_message(&M->down, "router", true, msg, len, mono());
    else if (kind == RDOF_MN_T_TG && strcmp(node, M->node))
        rdof_mn_down_message(&M->down, node, false, msg, len, mono());
    else if (kind == RDOF_MN_T_PC && strcmp(node, M->node)) { /* to an ID one of the box's apps answers for */
        uint32_t ids[RDOF_MN_CLAIM_SUBS];
        const int nc = rdof_mn_claim_ids(M->site, 0, ids, RDOF_MN_CLAIM_SUBS);
        for (int i = 0; i < nc; i++)
            if (ids[i] == id) {
                rdof_mn_down_message(&M->down, node, false, msg, len, mono());
                break;
            }
    }
}

static void handle_tx(const tx_in_t *t)
{
    const char *from = rdof_mn_down_from(&M->down, t->stream);
    if (!from)
        return; /* not the network's stream */
    int path = -1;
    const int f = rdof_site_freq_hz(M->site, t->freq);
    for (int k = 0; k < M->site->npath && f >= 0; k++)
        if (M->site->path[k].tx_freq == f && (!M->site->path[k].tx_slot || M->site->path[k].tx_slot == t->slot))
            path = k;
    char buf[512];
    if (rdof_mn_tx_event(M->node, now_unix(), path, t->slot, t->stream, t->end ? "end" : "start", t->src, t->dst,
                         from, buf, sizeof buf))
        pub(M->act, buf, 0, 1, false);
    rdof_mn_pending_t p;
    const char *r = t->end ? rdof_mn_results_sent(&M->results, t->stream, path, t->slot, mono(), &p) : NULL;
    if (r)
        result(r, &p);
}

/* The connection's settings, kept: the fall back to 3.1.1 sets them again. */
static EXT_RAM_BSS_ATTR esp_mqtt_client_config_t client_cfg;

static void mn_task(void *arg)
{
    double ticked = 0, subs_at = -10, status_at = -10, restart_at = -1;
    for (;;) {
        frame_in_t fi;
        if (M->fall_back) { /* from here, not from esp-mqtt's own task, which a stop waits for */
            M->fall_back = false;
            ESP_LOGW(TAG, "the broker doesn't take MQTT 5: using 3.1.1");
            esp_mqtt_client_stop(M->client);
            client_cfg.session.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
            esp_mqtt_set_config(M->client, &client_cfg);
            restart_at = mono();
        }
        /* the start can fail: esp-mqtt's task takes 6 KB of internal RAM, and the reporting connections fall back
         * (and take theirs) at the same moment. Tried again each second until it runs: a W12 was left with no
         * MQTT network after "Error create mqtt task" */
        if (restart_at >= 0 && mono() >= restart_at) {
            if (esp_mqtt_client_start(M->client) == ESP_OK)
                restart_at = -1;
            else
                restart_at = mono() + 1;
        }
        /* frames first: a call's bursts every 60 ms on each receiver */
        while (xQueueReceive(M->frames, &fi, 0) == pdTRUE)
            if (fi.source < MAX_SOURCES && M->src[fi.source].used)
                rdof_mn_up_frame(M->src[fi.source].up, &fi.f, mono());
        size_t n;
        while ((n = xMessageBufferReceive(M->in, M->msg, sizeof M->msg, 0)) > 0)
            handle_message(n);
        tx_in_t t;
        while (xQueueReceive(M->txq, &t, 0) == pdTRUE)
            handle_tx(&t);
        const double now = mono();
        if (now - ticked >= 0.1) {
            ticked = now;
            rdof_mn_down_tick(&M->down, now);
            for (int i = 0; i < MAX_SOURCES; i++)
                if (M->src[i].used)
                    rdof_mn_up_tick(M->src[i].up, now);
            rdof_mn_pending_t p;
            const char *r;
            while ((r = rdof_mn_results_tick(&M->results, now, &p)))
                result(r, &p);
        }
        if (M->connected_edge) {
            M->connected_edge = false;
            M->nsubs = 0; /* a new session: subscribe again */
            pub(M->online, "1", 1, 1, true);
            publish_info();
            publish_status(now);
            status_at = now;
            subs_at = -10;
        } else if (M->up && now - status_at >= 10) {
            publish_status(now);
            status_at = now;
        }
        if (M->up && now - subs_at >= 5) { /* held ptt TGs come and go; a failed change is tried again in 1 s */
            subs_at = now;
            if (!subscribe())
                subs_at = now - 4;
        }
        /* wake for the next frame, or every 20 ms for the broker's messages and the timers */
        xQueuePeek(M->frames, &fi, pdMS_TO_TICKS(20));
    }
}

/* The connection's settings: the SDR's mqtt network, else the first radio's (one mqtt network in version 3). */
static bool conn(const char **host, int *port, const char **user, const char **pass, const char **root, bool *tls)
{
#if CONFIG_RDOF_SDR
    if (g_roles.has_sdr && g_scfg.has_mqtt && g_scfg.mqtt_host[0]) {
        *host = g_scfg.mqtt_host, *port = g_scfg.mqtt_port, *user = g_scfg.mqtt_user, *pass = g_scfg.mqtt_pass;
        *root = g_scfg.mqtt_root, *tls = g_scfg.mqtt_tls;
        return true;
    }
#endif
    for (int i = 0; i < g_roles.nradios; i++) {
        const rdof_config_t *c = g_rcfg[i];
        if (c && c->has_mqtt && c->mqtt_host[0]) {
            *host = c->mqtt_host, *port = c->mqtt_port, *user = c->mqtt_user, *pass = c->mqtt_pass;
            *root = c->mqtt_root, *tls = c->mqtt_tls;
            return true;
        }
    }
    return false;
}

/* What mn_start made, when it gives up. */
static void mn_free(mn_t *m)
{
    if (m->client)
        esp_mqtt_client_destroy(m->client);
    if (m->frames)
        vQueueDeleteWithCaps(m->frames);
    if (m->txq)
        vQueueDeleteWithCaps(m->txq);
    if (m->in)
        vMessageBufferDeleteWithCaps(m->in);
    if (m->sink_lock)
        vSemaphoreDelete(m->sink_lock);
    heap_caps_free(m);
}

void mn_start(void)
{
    const int net = site_mn_net();
    const rdof_site_t *site = site_get();
    if (net < 0 || !site)
        return;
    const char *host, *user, *pass, *root;
    int port;
    bool tls;
    if (!conn(&host, &port, &user, &pass, &root, &tls)) {
        ESP_LOGW(TAG, "format v3, but no broker in the settings: off");
        return;
    }
    if (!rdof_mn_node_ok(site->name)) {
        ESP_LOGE(TAG, "the box's name \"%s\" can't be a node (letters, digits, . _ -, at most 39): off", site->name);
        return;
    }
    mn_t *m = heap_caps_calloc(1, sizeof *m, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!m) {
        ESP_LOGE(TAG, "no memory (%u bytes): off", (unsigned)sizeof *m);
        return;
    }
    m->site = site;
    m->net = net;
    m->tls = tls;
    snprintf(m->node, sizeof m->node, "%s", site->name);
    snprintf(m->root, sizeof m->root, "%s", root);
    rdof_mn_node_topic(m->online, sizeof m->online, root, m->node, "online");
    rdof_mn_node_topic(m->info, sizeof m->info, root, m->node, "info");
    rdof_mn_node_topic(m->act, sizeof m->act, root, m->node, "activity");
    rdof_mn_node_topic(m->status, sizeof m->status, root, m->node, "status");
    rdof_mn_topic(m->dl, sizeof m->dl, root, RDOF_MN_T_DL, 0, m->node);
    rdof_mn_down_init(&m->down);
    m->down.raw_tx = site->net[net].raw_tx;
    m->down.burst = burst;
    m->down.data = data;
    m->down.setup = on_setup;
    m->frames = xQueueCreateWithCaps(FRAMES, sizeof(frame_in_t), MALLOC_CAP_SPIRAM);
    m->txq = xQueueCreateWithCaps(TXQ, sizeof(tx_in_t), MALLOC_CAP_SPIRAM);
    m->in = xMessageBufferCreateWithCaps(IN_KB * 1024, MALLOC_CAP_SPIRAM);
    m->sink_lock = xSemaphoreCreateMutex();
    if (!m->frames || !m->txq || !m->in || !m->sink_lock) {
        ESP_LOGE(TAG, "no memory for the queues: off");
        mn_free(m);
        return;
    }
    static EXT_RAM_BSS_ATTR char uri[112], id[64];
    bool use_tls = false;
#if CONFIG_RDOF_MQTT_TLS
    use_tls = tls;
#else
    if (tls)
        ESP_LOGW(TAG, "mqtt/tls, but this build has no TLS (CONFIG_RDOF_MQTT_TLS): plain MQTT");
#endif
    m->use_tls = use_tls;
    snprintf(uri, sizeof uri, "%s://%s:%d", use_tls ? "mqtts" : "mqtt", host, port);
    rdof_mn_client_id(id, sizeof id, root, m->node);
    client_cfg = (esp_mqtt_client_config_t){
        .broker.address.uri = uri,
#if CONFIG_RDOF_MQTT_TLS
        .broker.verification.crt_bundle_attach = use_tls ? esp_crt_bundle_attach : NULL,
#endif
        .credentials.client_id = id,
        .credentials.username = user[0] ? user : NULL,
        .credentials.authentication.password = pass[0] ? pass : NULL,
        .session.keepalive = 30,
        .session.last_will = {.topic = m->online, .msg = "0", .msg_len = 1, .qos = 1, .retain = 1},
#if CONFIG_MQTT_PROTOCOL_5
        .session.protocol_ver = MQTT_PROTOCOL_V_5, /* No Local on the subscriptions */
#endif
        .network.reconnect_timeout_ms = 2000,
        .network.disable_auto_reconnect = false,
        /* a call message whole, both ways (esp-mqtt sends a bigger one in pieces: net.c), and over
         * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL so both buffers are in PSRAM */
        .buffer.size = RDOF_MN_MSG_MAX + TOPIC_MAX + 1024,
        .buffer.out_size = RDOF_MN_MSG_MAX + TOPIC_MAX + 1024,
        .outbox.limit = 16 * 1024,
    };
    M = m; /* before the client: its events use M */
    m->client = esp_mqtt_client_init(&client_cfg);
    if (!m->client) {
        ESP_LOGE(TAG, "no MQTT client: off");
        M = NULL;
        mn_free(m);
        return;
    }
    esp_mqtt_client_register_event(m->client, ESP_EVENT_ANY_ID, on_event, NULL);
    /* on core 0 with the network, off the DSP's core 1 */
    if (xTaskCreatePinnedToCoreWithCaps(mn_task, "mqtt_net", 8192, NULL, 5, NULL, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "the task didn't start: off");
        /* the client never started, so no event uses it; a receiver that took a sink meanwhile keeps its memory */
        xSemaphoreTake(m->sink_lock, portMAX_DELAY);
        M = NULL;
        bool sinks = false;
        for (int i = 0; i < MAX_SOURCES; i++)
            sinks |= m->src[i].used;
        xSemaphoreGive(m->sink_lock);
        if (!sinks)
            mn_free(m);
        return;
    }
    esp_mqtt_client_start(m->client);
    ESP_LOGI(TAG, "node %s on %s/{tg,pc,dl,node}/ at %s, as %s (%u KB PSRAM)", m->node, root, uri, id,
             (unsigned)(sizeof *m / 1024));
}

void mn_status_json(rdof_json_t *j)
{
    if (!M)
        return;
    rdof_json_open(j, "mqtt_net");
    rdof_json_bool(j, "connected", M->up);
    rdof_json_str(j, "node", M->node);
    rdof_json_int(j, "messages_in", (long)M->down.messages);
    rdof_json_int(j, "bad_in", (long)M->down.bad);
    rdof_json_int(j, "late_in", (long)M->down.late);
    rdof_json_int(j, "in_dropped", (long)M->in_dropped);
    rdof_json_int(j, "frames_dropped", (long)M->frames_dropped);
    rdof_json_int(j, "acks", (long)M->acks);
    long out = 0;
    for (int i = 0; i < MAX_SOURCES; i++)
        if (M->src[i].used)
            out += (long)M->src[i].up->messages;
    rdof_json_int(j, "messages_out", out);
    rdof_json_close(j);
}

#else

void mn_start(void) {}
bool mn_sink(int source, uint32_t freq, const double *freq_now, rdof_sink_t *out) { return false; }
void mn_tx_event(const char *action, const rdof_tx_call_info_t *c) {}
void mn_status_json(rdof_json_t *j) {}

#endif
