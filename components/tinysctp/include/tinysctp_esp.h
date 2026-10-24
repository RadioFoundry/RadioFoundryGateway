// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * tinysctp on ESP-IDF: one association over UDP encapsulation, run by its
 * own FreeRTOS task. Any task may send; on_message and on_event run in the
 * association's task, with its lock held (they may call tsctp_esp_send; keep
 * them short).
 */
#ifndef TINYSCTP_ESP_H
#define TINYSCTP_ESP_H

#include <stdint.h>

#include "tinysctp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tsctp_esp tsctp_esp_t;

typedef struct {
    const char *host;        /* the peer's name or address; NULL: listen for one */
    uint16_t encap_port;     /* the peer's UDP encapsulation port (9899 is usual) */
    uint16_t local_udp_port; /* 0: any (a listener needs one) */
    tsctp_config_t sctp;     /* output and random are filled in */
    const char *task_name;
    uint32_t task_stack;
    int task_priority;
} tsctp_esp_config_t;

/* tsctp_config_defaults, encapsulation port 9899, the task's stack and priority from Kconfig. */
void tsctp_esp_config_defaults(tsctp_esp_config_t *cfg);

/* Resolve the host, open the socket, start connecting (or listening) and the task. NULL on failure
   (the name doesn't resolve, no memory). */
tsctp_esp_t *tsctp_esp_start(const tsctp_esp_config_t *cfg);
/* ABORT if associated, end the task, free everything. */
void tsctp_esp_stop(tsctp_esp_t *h);

int tsctp_esp_send(tsctp_esp_t *h, const void *data, size_t len, const tsctp_sendinfo_t *info);
int tsctp_esp_shutdown(tsctp_esp_t *h);
tsctp_state_t tsctp_esp_state(tsctp_esp_t *h);
void tsctp_esp_stats(tsctp_esp_t *h, tsctp_stats_t *out);
size_t tsctp_esp_status_json(tsctp_esp_t *h, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
