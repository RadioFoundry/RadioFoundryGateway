// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * A radiosim process seen from a test: start it, send it requests, take what it
 * says back (radiosim.c has the protocol). One request at a time, in virtual time.
 */
#ifndef RDOF_SYSTEST_RADIOLINK_H
#define RDOF_SYSTEST_RADIOLINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct {
    uint32_t ms;
    uint32_t slot;
    int kind, index, sync, lcss;
    uint8_t emb[4];
    uint8_t payload[27];
    size_t len;
} radiolink_air_t;

typedef struct radiolink {
    pid_t pid;
    int to, from; /* pipe ends */
    char buf[1 << 16];
    size_t have;
    uint32_t now; /* the virtual clock, ms, as radiosim keeps it */
    /* What came back (each may be NULL). */
    void (*on_frame)(void *ctx, uint32_t ms, const uint8_t *frame, size_t len);
    void (*on_air)(void *ctx, const radiolink_air_t *a);
    void (*on_event)(void *ctx, uint32_t ms, const char *what); /* "enter 0", "freq 1 2", ... */
    void *ctx;
} radiolink_t;

/* Start radiosim at path (description NULL: its default). False if it can't. */
bool radiolink_start(radiolink_t *l, const char *path, const char *description);
void radiolink_stop(radiolink_t *l);
/* One request line (no newline); the callbacks run for its output. False if radiosim is gone. */
bool radiolink_request(radiolink_t *l, const char *line);
/* Bytes from the host to the radio's serial port. */
bool radiolink_host(radiolink_t *l, const uint8_t *data, size_t len);
/* Advance the clock by ms. */
bool radiolink_advance(radiolink_t *l, uint32_t ms);

#endif
