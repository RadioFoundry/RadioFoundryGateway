// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* What both roles' network code shares: an MQTT connection with its last will, and a connected UDP socket. */
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "esp_log.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#if CONFIG_RDOF_MQTT

#if CONFIG_RDOF_MQTT_TLS
#include "esp_crt_bundle.h"
#endif
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/message_buffer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#if CONFIG_MQTT_PROTOCOL_5
#include "mqtt5_client.h"
#endif

static const char *TAG = "mqtt";

#define POST_MAX 4096  /* the largest message net_mqtt_post takes, topic and all */
#define MAX_ALIASES 32 /* topic aliases a connection uses at most */

/* A queued message: this header, the topic, then the payload. */
typedef struct {
    uint8_t qos, retain;
    uint16_t topic_len;
} post_hdr_t;

typedef struct {
    MessageBufferHandle_t buf;
    SemaphoreHandle_t lock; /* a message buffer takes one writer at a time */
    esp_mqtt_client_handle_t client;
    net_mqtt_t *m;
    char *msg;              /* POST_MAX: the writers' (under lock) */
    char *out;              /* POST_MAX + 2: the task's */
    /* topic aliases, the task's: for connection gen, n_alias of them; sent[i]: alias i + 1 went out with its topic */
    uint32_t gen;
    int n_alias;
    bool sent[MAX_ALIASES];
    bool held; /* QoS 1 held back: more than the broker's Receive Maximum unacked */
#if CONFIG_MQTT_PROTOCOL_5
    esp_mqtt5_publish_property_config_t prop; /* esp-mqtt keeps a pointer to it until the next publish */
#endif
} post_queue_t;

static esp_mqtt_client_config_t client_cfg[2]; /* each connection's, for the fall back to 3.1.1 */
static int n_client;

#if CONFIG_MQTT_PROTOCOL_5
/* The broker's Topic Alias Maximum isn't in esp-mqtt's API, but setting a publish property with an alias over it
 * fails: the largest that doesn't, up to what's wanted. Each try that succeeds is undone with alias 0 (none), so
 * the next publish carries no property. esp-mqtt's complaints about the ones over it are kept out of the log. */
static int probe_aliases(post_queue_t *q, int want)
{
    const esp_log_level_t was = esp_log_level_get("mqtt5_client");
    esp_log_level_set("mqtt5_client", ESP_LOG_NONE);
    int lo = 0, hi = want; /* lo works (0 always does), hi + 1 doesn't */
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        q->prop = (esp_mqtt5_publish_property_config_t){.topic_alias = (uint16_t)mid};
        if (esp_mqtt5_client_set_publish_property(q->client, &q->prop) == ESP_OK)
            lo = mid;
        else
            hi = mid - 1;
    }
    q->prop = (esp_mqtt5_publish_property_config_t){0};
    esp_mqtt5_client_set_publish_property(q->client, &q->prop);
    esp_log_level_set("mqtt5_client", was);
    return lo;
}

static void clear_property(post_queue_t *q)
{
    q->prop = (esp_mqtt5_publish_property_config_t){0};
    esp_mqtt5_client_set_publish_property(q->client, &q->prop);
}
#endif

/* The topic's alias on this connection (1..), or 0. A new connection starts over: aliases are per connection. */
static int alias_for(post_queue_t *q, const char *topic)
{
#if CONFIG_MQTT_PROTOCOL_5
    net_mqtt_t *m = q->m;
    if (q->gen != m->gen) {
        q->gen = m->gen;
        memset(q->sent, 0, sizeof q->sent);
        const int want = m->n_alias_topics < MAX_ALIASES ? m->n_alias_topics : MAX_ALIASES;
        q->n_alias = m->v5 && want > 0 ? probe_aliases(q, want) : 0;
        if (q->n_alias)
            ESP_LOGI(TAG, "%s: %d topic aliases", m->client_id, q->n_alias);
    }
    for (int i = 0; i < q->n_alias; i++)
        if (!strcmp(m->alias_topics[i], topic))
            return i + 1;
#endif
    return 0;
}

static void publish(post_queue_t *q, const char *topic, const char *data, size_t len, int qos, bool retain)
{
    /* publish, not enqueue: the client's task sends one enqueued message a loop and then waits up to
     * MQTT_POLL_READ_TIMEOUT_MS (1 s) for input, so bursts went out at about one a second. This task can block:
     * it sends now while connected; while not, QoS 1 waits in the outbox and QoS 0 is dropped. QoS 1 never takes an
     * alias: esp-mqtt resends it from the outbox, as first built, on a later connection that doesn't know it. */
    const int alias = qos == 0 && *q->m->up ? alias_for(q, topic) : 0;
#if CONFIG_MQTT_PROTOCOL_5
    if (alias) {
        const uint32_t gen = q->gen;
        q->prop = (esp_mqtt5_publish_property_config_t){.topic_alias = (uint16_t)alias};
        const bool first = !q->sent[alias - 1];
        if (esp_mqtt5_client_set_publish_property(q->client, &q->prop) == ESP_OK) {
            const int id = esp_mqtt_client_publish(q->client, first ? topic : "", data, (int)len, 0, retain);
            if (id < 0) {
                clear_property(q);  /* not sent: it mustn't ride on the next publish */
                q->m->dropped++;
            }
            else if (first && q->m->gen == gen) /* if it reconnected meanwhile, alias_for starts over */
                q->sent[alias - 1] = true;
            return;
        }
        q->n_alias = 0;             /* the broker allows fewer than it seemed: none on this connection */
    }
#else
    (void)alias;
#endif
    /* MQTT 5: esp-mqtt refuses a QoS 1 publish (-1) while more than the broker's Receive Maximum (mosquitto's
     * max_inflight_messages, 20) are unacked, as when a connect resends the backlog the outbox kept. Refused, it
     * would be lost: wait for the acks instead, up to 5 s, the queue keeping what comes meanwhile. esp-mqtt logs two
     * lines for each refusal; while held, they're kept out of the log. */
    for (int waited = 0;; waited += 20) {
        const esp_log_level_t was = esp_log_level_get("mqtt5_client"), was_c = esp_log_level_get("mqtt_client");
        if (q->held) {
            esp_log_level_set("mqtt5_client", ESP_LOG_NONE);
            esp_log_level_set("mqtt_client", ESP_LOG_WARN);
        }
        const int id = esp_mqtt_client_publish(q->client, topic, data, (int)len, qos, retain);
        if (q->held) {
            esp_log_level_set("mqtt5_client", was);
            esp_log_level_set("mqtt_client", was_c);
        }
        if (id >= 0) {
            if (q->held)
                ESP_LOGI(TAG, "%s: the broker acked: publishing again", q->m->client_id);
            q->held = false;
            return;
        }
        if (id != -1 || qos == 0 || !q->m->v5 || !*q->m->up || waited >= 5000)
            break;
        if (!q->held)
            ESP_LOGW(TAG, "%s: the broker's Receive Maximum unacked: holding QoS 1 back", q->m->client_id);
        q->held = true;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    q->m->dropped++; /* the outbox full (-2), the acks over 5 s late, or not sent */
}

#if CONFIG_MQTT_PROTOCOL_5
/* Refused for MQTT 5: the same client on 3.1.1, stopped and started from here (not from its own task). */
static void fall_back(post_queue_t *q)
{
    net_mqtt_t *m = q->m;
    m->fall_back = false;
    for (int i = 0; i < n_client; i++)
        if (client_cfg[i].credentials.client_id == m->client_id) {
            ESP_LOGW(TAG, "%s:%d doesn't take MQTT 5: using 3.1.1", m->host, m->port);
            esp_mqtt_client_stop(q->client);
            client_cfg[i].session.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
            esp_mqtt_set_config(q->client, &client_cfg[i]);
            esp_mqtt_client_start(q->client);
        }
}
#endif

static void post_task(void *arg)
{
    post_queue_t *q = arg;
    for (;;) {
        size_t n = xMessageBufferReceive(q->buf, q->out, POST_MAX, pdMS_TO_TICKS(500));
#if CONFIG_MQTT_PROTOCOL_5
        if (q->m->fall_back)
            fall_back(q);
#endif
        if (n < sizeof(post_hdr_t))
            continue;
        post_hdr_t h;
        memcpy(&h, q->out, sizeof h);
        char *topic = q->out + sizeof h, *data = topic + h.topic_len + 1;
        const size_t len = n - sizeof h - h.topic_len;
        memmove(data, topic + h.topic_len, len); /* NUL after the topic */
        topic[h.topic_len] = 0;
        publish(q, topic, data, len, h.qos, h.retain);
    }
}

static post_queue_t *post_start(net_mqtt_t *m, esp_mqtt_client_handle_t client, int kb)
{
    /* in PSRAM where there is some, the stack too (the task never touches flash); else internal RAM, smaller */
    const uint32_t caps = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) ? MALLOC_CAP_SPIRAM : MALLOC_CAP_8BIT;
    if (caps != MALLOC_CAP_SPIRAM && kb > 8)
        kb = 8;
    post_queue_t *q = heap_caps_calloc(1, sizeof *q, caps);
    if (!q)
        return NULL;
    q->client = client;
    q->m = m;
    q->gen = m->gen; /* each connect bumps it: the aliases are probed on its first publish */
    q->lock = xSemaphoreCreateMutex();
    q->msg = heap_caps_malloc(POST_MAX, caps);
    q->out = heap_caps_malloc(POST_MAX + 2, caps);
    q->buf = xMessageBufferCreateWithCaps((size_t)kb * 1024, caps);
    /* a publish writes the socket, through TLS when that's on: more stack than the 3 KB it had. On core 0 with the
     * network, off the DSP's core 1 */
    if (!q->lock || !q->msg || !q->out || !q->buf ||
        xTaskCreatePinnedToCoreWithCaps(post_task, "mqtt_post", 6144, q, 5, NULL, 0, caps) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the publish queue: publishing directly");
        return NULL;
    }
    return q;
}

bool net_mqtt_post(net_mqtt_t *m, const char *topic, const void *data, int len, int qos, bool retain)
{
    post_queue_t *q = m->queue;
    const size_t dl = len < 0 ? strlen(data) : (size_t)len;
    if (!q)
        return m->client && esp_mqtt_client_enqueue(m->client, topic, data, (int)dl, qos, retain, true) >= 0;
    if (qos == 0 && !*m->up) {
        m->dropped++;
        return false;
    }
    const size_t tl = strlen(topic), n = sizeof(post_hdr_t) + tl + dl;
    if (n > POST_MAX || tl > 0xffff) {
        ESP_LOGW(TAG, "%u bytes to %s: too big to queue", (unsigned)n, topic);
        m->dropped++;
        return false;
    }
    xSemaphoreTake(q->lock, portMAX_DELAY);
    const post_hdr_t h = {.qos = (uint8_t)qos, .retain = retain, .topic_len = (uint16_t)tl};
    memcpy(q->msg, &h, sizeof h);
    memcpy(q->msg + sizeof h, topic, tl);
    memcpy(q->msg + sizeof h + tl, data, dl);
    const bool ok = xMessageBufferSend(q->buf, q->msg, n, 0) == n;
    xSemaphoreGive(q->lock);
    if (!ok)
        m->dropped++;
    return ok;
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    net_mqtt_t *m = arg;
    esp_mqtt_event_handle_t e = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        m->v5 = e->protocol_ver == MQTT_PROTOCOL_V_5;
        m->gen++;
        *m->up = true;
        ESP_LOGI(TAG, "%s connected to %s:%d (MQTT %s)", m->client_id, m->host, m->port, m->v5 ? "5" : "3.1.1");
        if (m->on_connected)
            m->on_connected(m->ctx);
        break;
    case MQTT_EVENT_DISCONNECTED:
        if (*m->up)
            ESP_LOGW(TAG, "%s disconnected from %s:%d", m->client_id, m->host, m->port);
        *m->up = false;
        m->gen++;
        break;
    case MQTT_EVENT_ERROR:
        if (e->error_handle && e->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            const int code = e->error_handle->connect_return_code;
            /* 3.1.1's "unacceptable protocol version" (1) or 5's "unsupported protocol version" (0x84) */
            if (e->protocol_ver == MQTT_PROTOCOL_V_5 && (code == 1 || code == 0x84) && m->queue)
                m->fall_back = true;
            else
                ESP_LOGW(TAG, "refused by %s:%d (check the username / password)", m->host, m->port);
        }
        break;
    default:
        break;
    }
}

struct esp_mqtt_client *net_mqtt_start(net_mqtt_t *m)
{
    static char uri[2][112];
    if (n_client >= 2)
        return NULL;
    bool tls = false;
#if CONFIG_RDOF_MQTT_TLS
    tls = m->tls;
#else
    if (m->tls)
        ESP_LOGW(TAG, "mqtt.tls is set but this build has no TLS (CONFIG_RDOF_MQTT_TLS): plain MQTT");
#endif
    char *u = uri[n_client];
    snprintf(u, sizeof uri[0], "%s://%s:%d", tls ? "mqtts" : "mqtt", m->host, m->port);
    esp_mqtt_client_config_t *c = &client_cfg[n_client++];
    *c = (esp_mqtt_client_config_t){
        .broker.address.uri = u,
        .credentials.client_id = m->client_id,
        .credentials.username = m->user[0] ? m->user : NULL,
        .credentials.authentication.password = m->pass[0] ? m->pass : NULL,
        .session.keepalive = 30,
        .session.last_will = {.topic = m->will_topic, .msg = m->will_msg, .qos = 1, .retain = 1},
        .network.reconnect_timeout_ms = 2000,
        .outbox.limit = (uint64_t)m->outbox_kb * 1024,
    };
#if CONFIG_MQTT_PROTOCOL_5
    /* MQTT 5 for the topic aliases; the fall back to 3.1.1 runs from the publish queue's task */
    if (m->queue_kb > 0)
        c->session.protocol_ver = MQTT_PROTOCOL_V_5;
#endif
#if CONFIG_RDOF_MQTT_TLS
    if (tls)
        c->broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
#endif
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(c);
    if (!client)
        return NULL;
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, on_event, (void *)m);
    m->client = client;
    if (m->queue_kb > 0)
        m->queue = post_start(m, client, m->queue_kb);
#if CONFIG_MQTT_PROTOCOL_5
    if (!m->queue && c->session.protocol_ver == MQTT_PROTOCOL_V_5) {
        c->session.protocol_ver = MQTT_PROTOCOL_V_3_1_1; /* nothing to fall back from: 3.1.1 */
        esp_mqtt_set_config(client, c);
    }
#endif
    esp_mqtt_client_start(client);
    return client;
}

#else

struct esp_mqtt_client *net_mqtt_start(net_mqtt_t *m) { return NULL; }
bool net_mqtt_post(net_mqtt_t *m, const char *topic, const void *data, int len, int qos, bool retain) { return false; }

#endif

int net_udp_connect(const char *host, int port)
{
    char p[8];
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM}, *res = NULL;
    snprintf(p, sizeof p, "%d", port);
    if (getaddrinfo(host, p, &hints, &res) != 0 || !res)
        return -1;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s >= 0 && connect(s, res->ai_addr, res->ai_addrlen) != 0) {
        close(s);
        s = -1;
    }
    freeaddrinfo(res);
    return s;
}
