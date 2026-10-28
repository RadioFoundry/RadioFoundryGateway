// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * rdofradio's MQTT connection, as the board's (esp_idf/main/net.c) on libmosquitto (EPL-2.0 / EDL-1.0, linked:
 * PROVENANCE.md): MQTT 5, falling back to 3.1.1 when the broker refuses it; the status topic's retained last will;
 * topic aliases for QoS 0 (as many of alias_topics as the broker's Topic Alias Maximum allows, the first to fit);
 * a QoS 1 publish waits in libmosquitto's queue while the broker is away and goes out on the next connection, QoS
 * 0 is dropped then. libmosquitto's own thread runs the connection; publish from any thread.
 */
#ifndef RDOFRADIO_MQTT_H
#define RDOFRADIO_MQTT_H

#include <stdbool.h>
#include <stdint.h>

typedef struct rdofmq rdofmq_t;

typedef struct {
    const char *host, *user, *pass; /* user / pass "": none */
    int port;
    bool tls;                       /* the OS's CA certificates */
    const char *client_id;
    const char *will_topic, *will_msg;
    const char *const *alias_topics; /* the topics to alias, most published first; kept by the caller */
    int n_alias;
    int queue_max;                  /* QoS 1 messages not yet acked, past which a QoS 1 publish is dropped */
    void (*say)(const char *line);  /* the connection's news, a line at a time */
} rdofmq_opts_t;

/* Connects in the background. NULL: libmosquitto refused the options (said why). */
rdofmq_t *rdofmq_start(const rdofmq_opts_t *o);
/* len < 0: data is a string. False: dropped (QoS 0 while down, or the queue full). */
bool rdofmq_publish(rdofmq_t *q, const char *topic, const void *data, int len, int qos, bool retain);
bool rdofmq_up(rdofmq_t *q);
/* True once after each connect: time to publish the retained messages. */
bool rdofmq_connected(rdofmq_t *q);
uint32_t rdofmq_dropped(rdofmq_t *q);
/* Publishes the will's message (a clean disconnect sends no will), disconnects and frees. */
void rdofmq_stop(rdofmq_t *q);

#endif
