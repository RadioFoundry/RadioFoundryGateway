// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The bench's line protocol server (docs/spec/bench-api.md section 14): a TCP listener and a thread of its own that
 * hands each connection's bytes to the engine (rdof/benchline.h), ticks it, and sends what it gives back. BSD sockets
 * and POSIX threads: a Linux or macOS host, and ESP-IDF (lwIP).
 */
#ifndef RDOF_BENCHNET_H
#define RDOF_BENCHNET_H

#include <stddef.h>

#include "rdof/benchline.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rdof_bnet rdof_bnet_t;

/* Listen on addr (NULL or "": every interface) and port (0: any free one) and start the thread. now: a monotonic
 * clock in seconds. NULL with error set when it can't. */
rdof_bnet_t *rdof_bnet_start(rdof_bl_t *bl, const char *addr, int port, double (*now)(void), size_t stack,
                             char *error, size_t cap);
/* The port it listens on. */
int rdof_bnet_port(const rdof_bnet_t *n);
/* Stop the thread and close everything. */
void rdof_bnet_stop(rdof_bnet_t *n);

#ifdef __cplusplus
}
#endif

#endif
