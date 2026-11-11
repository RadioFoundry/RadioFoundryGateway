// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* rdofradio's MQTT connection (rdofradio_mqtt.h) on libmosquitto. */
#define _POSIX_C_SOURCE 200809L
#include "rdofradio_mqtt.h"

#include <mosquitto.h>
#if LIBMOSQUITTO_MAJOR == 2 && LIBMOSQUITTO_MINOR == 0 /* the protocol's numbers: their own header in 2.0 */
#include <mqtt_protocol.h>
#endif
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_ALIASES 64

struct rdofmq {
    struct mosquitto *mosq;
    rdofmq_opts_t o;
    pthread_mutex_t mu;
    bool up, v5, fresh, refused;
    int n_alias;                /* the aliases on this connection: the first n_alias of o.alias_topics */
    bool sent[MAX_ALIASES];     /* the alias has gone out with its topic on this connection */
    int unacked;                /* QoS 1 messages in libmosquitto's queue */
    uint32_t dropped;
    char subs[RDOFMQ_SUBS][160]; /* rdofmq_subscribe's */
    int nsubs;
};

static void say(rdofmq_t *q, const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (q->o.say)
        q->o.say(line);
}

static int sub_opts(const rdofmq_t *q) { return q->v5 && q->o.no_local ? MQTT_SUB_OPT_NO_LOCAL : 0; }

static void on_connect(struct mosquitto *m, void *ctx, int rc, int flags, const mosquitto_property *props)
{
    rdofmq_t *q = ctx;
    if (rc == 0) {
        uint16_t max = 0;
        if (q->v5)
            mosquitto_property_read_int16(props, MQTT_PROP_TOPIC_ALIAS_MAXIMUM, &max, false);
        pthread_mutex_lock(&q->mu);
        q->up = q->fresh = true;
        q->refused = false;
        q->n_alias = q->o.n_alias < max ? q->o.n_alias : max;
        if (q->n_alias > MAX_ALIASES)
            q->n_alias = MAX_ALIASES;
        memset(q->sent, 0, sizeof q->sent);
        pthread_mutex_unlock(&q->mu);
        say(q, "mqtt: %s connected to %s:%d (MQTT %s%s)", q->o.client_id, q->o.host, q->o.port, q->v5 ? "5" : "3.1.1",
            q->n_alias ? ", topic aliases" : "");
        if (q->o.subscribe && mosquitto_subscribe_v5(m, NULL, q->o.subscribe, 0, sub_opts(q), NULL) != MOSQ_ERR_SUCCESS)
            say(q, "mqtt: %s couldn't take %s", q->o.client_id, q->o.subscribe);
        pthread_mutex_lock(&q->mu);
        for (int i = 0; i < q->nsubs; i++)
            if (mosquitto_subscribe_v5(m, NULL, q->subs[i], 0, sub_opts(q), NULL) != MOSQ_ERR_SUCCESS)
                say(q, "mqtt: %s couldn't take %s", q->o.client_id, q->subs[i]);
        pthread_mutex_unlock(&q->mu);
        return;
    }
    /* 3.1.1's "unacceptable protocol version" (1), or 5's "unsupported protocol version" (0x84): the same on 3.1.1,
       from the next try on */
    if (q->v5 && (rc == 1 || rc == MQTT_RC_UNSUPPORTED_PROTOCOL_VERSION)) {
        say(q, "mqtt: %s:%d doesn't take MQTT 5: using 3.1.1", q->o.host, q->o.port);
        q->v5 = false;
        mosquitto_int_option(m, MOSQ_OPT_PROTOCOL_VERSION, MQTT_PROTOCOL_V311);
        return;
    }
    if (!q->refused)
        say(q, "mqtt: refused by %s:%d: %s (check the username / password)", q->o.host, q->o.port,
            q->v5 ? mosquitto_reason_string(rc) : mosquitto_connack_string(rc));
    q->refused = true;
}

static void on_disconnect(struct mosquitto *m, void *ctx, int rc, const mosquitto_property *props)
{
    rdofmq_t *q = ctx;
    pthread_mutex_lock(&q->mu);
    const bool was = q->up;
    q->up = false;
    q->n_alias = 0;
    pthread_mutex_unlock(&q->mu);
    if (was)
        say(q, "mqtt: %s disconnected from %s:%d", q->o.client_id, q->o.host, q->o.port);
}

static void on_publish(struct mosquitto *m, void *ctx, int mid, int reason, const mosquitto_property *props)
{
    rdofmq_t *q = ctx;
    pthread_mutex_lock(&q->mu);
    if (q->unacked > 0)
        q->unacked--;
    pthread_mutex_unlock(&q->mu);
}

static void on_message(struct mosquitto *m, void *ctx, const struct mosquitto_message *msg,
                       const mosquitto_property *props)
{
    rdofmq_t *q = ctx;
    if (q->o.on_message && msg->topic)
        q->o.on_message(q->o.ctx, msg->topic, msg->payload, msg->payloadlen);
}

rdofmq_t *rdofmq_start(const rdofmq_opts_t *o)
{
    static bool init;
    if (!init) {
        mosquitto_lib_init();
        init = true;
    }
    rdofmq_t *q = calloc(1, sizeof *q);
    if (!q)
        return NULL;
    q->o = *o;
    q->v5 = true;
    pthread_mutex_init(&q->mu, NULL);
    q->mosq = mosquitto_new(o->client_id, true, q);
    if (!q->mosq) {
        say(q, "mqtt: no client");
        free(q);
        return NULL;
    }
    struct mosquitto *m = q->mosq;
    mosquitto_int_option(m, MOSQ_OPT_PROTOCOL_VERSION, MQTT_PROTOCOL_V5);
    if (o->nodelay)
        mosquitto_int_option(m, MOSQ_OPT_TCP_NODELAY, 1);
    if (o->user && o->user[0])
        mosquitto_username_pw_set(m, o->user, o->pass && o->pass[0] ? o->pass : NULL);
    if (o->will_topic)
        mosquitto_will_set_v5(m, o->will_topic, (int)strlen(o->will_msg), o->will_msg, 1, true, NULL);
    if (o->tls && mosquitto_int_option(m, MOSQ_OPT_TLS_USE_OS_CERTS, 1) != MOSQ_ERR_SUCCESS)
        say(q, "mqtt: mqtt.tls is set but this build's libmosquitto has no TLS: plain MQTT");
    mosquitto_connect_v5_callback_set(m, on_connect);
    mosquitto_disconnect_v5_callback_set(m, on_disconnect);
    mosquitto_publish_v5_callback_set(m, on_publish);
    mosquitto_message_v5_callback_set(m, on_message);
    mosquitto_reconnect_delay_set(m, 2, 30, true);
    const int rc = mosquitto_connect_async(m, o->host, o->port, 30);
    if (rc != MOSQ_ERR_SUCCESS) /* the loop tries again */
        say(q, "mqtt: %s:%d: %s", o->host, o->port, mosquitto_strerror(rc));
    if (mosquitto_loop_start(m) != MOSQ_ERR_SUCCESS) {
        say(q, "mqtt: no thread for the connection");
        mosquitto_destroy(m);
        free(q);
        return NULL;
    }
    return q;
}

bool rdofmq_publish(rdofmq_t *q, const char *topic, const void *data, int len, int qos, bool retain)
{
    if (!q)
        return false;
    const int n = len < 0 ? (int)strlen(data) : len;
    pthread_mutex_lock(&q->mu);
    if ((qos == 0 && !q->up) || (qos > 0 && q->unacked >= q->o.queue_max)) {
        q->dropped++;
        pthread_mutex_unlock(&q->mu);
        return false;
    }
    /* an alias for QoS 0 only: QoS 1 waits in the queue and may go out on a later connection, which doesn't know it */
    int alias = 0;
    for (int i = 0; qos == 0 && i < q->n_alias; i++)
        if (!strcmp(q->o.alias_topics[i], topic)) {
            alias = i + 1;
            break;
        }
    mosquitto_property *p = NULL;
    if (alias)
        mosquitto_property_add_int16(&p, MQTT_PROP_TOPIC_ALIAS, (uint16_t)alias);
    const int rc =
        mosquitto_publish_v5(q->mosq, NULL, alias && q->sent[alias - 1] ? NULL : topic, n, data, qos, retain, p);
    if (rc == MOSQ_ERR_SUCCESS) {
        if (alias)
            q->sent[alias - 1] = true;
        if (qos > 0)
            q->unacked++;
    } else {
        q->dropped++;
    }
    pthread_mutex_unlock(&q->mu);
    mosquitto_property_free_all(&p);
    return rc == MOSQ_ERR_SUCCESS;
}

void rdofmq_subscribe(rdofmq_t *q, const char *const *filters, int n)
{
    if (!q)
        return;
    if (n > RDOFMQ_SUBS)
        n = RDOFMQ_SUBS;
    pthread_mutex_lock(&q->mu);
    const bool up = q->up;
    for (int i = 0; i < q->nsubs; i++) { /* dropped */
        int k = 0;
        while (k < n && strcmp(q->subs[i], filters[k]))
            k++;
        if (k == n && up)
            mosquitto_unsubscribe_v5(q->mosq, NULL, q->subs[i], NULL);
    }
    for (int k = 0; k < n; k++) { /* new */
        int i = 0;
        while (i < q->nsubs && strcmp(q->subs[i], filters[k]))
            i++;
        if (i == q->nsubs && up && mosquitto_subscribe_v5(q->mosq, NULL, filters[k], 0, sub_opts(q), NULL))
            say(q, "mqtt: %s couldn't take %s", q->o.client_id, filters[k]);
    }
    for (int k = 0; k < n; k++)
        snprintf(q->subs[k], sizeof q->subs[k], "%s", filters[k]);
    q->nsubs = n;
    pthread_mutex_unlock(&q->mu);
}

bool rdofmq_up(rdofmq_t *q)
{
    if (!q)
        return false;
    pthread_mutex_lock(&q->mu);
    const bool up = q->up;
    pthread_mutex_unlock(&q->mu);
    return up;
}

bool rdofmq_connected(rdofmq_t *q)
{
    if (!q)
        return false;
    pthread_mutex_lock(&q->mu);
    const bool f = q->fresh;
    q->fresh = false;
    pthread_mutex_unlock(&q->mu);
    return f;
}

uint32_t rdofmq_dropped(rdofmq_t *q)
{
    if (!q)
        return 0;
    pthread_mutex_lock(&q->mu);
    const uint32_t d = q->dropped;
    pthread_mutex_unlock(&q->mu);
    return d;
}

void rdofmq_stop(rdofmq_t *q)
{
    if (!q)
        return;
    if (q->o.will_topic && rdofmq_up(q)) {
        rdofmq_publish(q, q->o.will_topic, q->o.will_msg, -1, 1, true);
        for (int i = 0; i < 50; i++) { /* up to 1 s for the acks */
            pthread_mutex_lock(&q->mu);
            const int left = q->unacked;
            pthread_mutex_unlock(&q->mu);
            if (!left)
                break;
            nanosleep(&(struct timespec){0, 20 * 1000 * 1000}, NULL);
        }
    }
    mosquitto_disconnect(q->mosq);
    mosquitto_loop_stop(q->mosq, false);
    mosquitto_destroy(q->mosq);
    pthread_mutex_destroy(&q->mu);
    free(q);
}
