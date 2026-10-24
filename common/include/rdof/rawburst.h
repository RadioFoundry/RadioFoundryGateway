// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * One burst for .../raw. Published as the 49-byte binary form
 * (version 1, little endian; the layout is in docs/spec/fw/mqtt.md, section 5), or version 2 (62 bytes: the
 * burst's UTC start and grid slot appended) when it has a UTC slot, or, for the host tests and the self-test, as
 * MMDVMHost's MQTTRaw JSON, byte for byte the golden vectors'.
 */
#ifndef RDOF_RAWBURST_H
#define RDOF_RAWBURST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/fec.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_RAWBURST_VERSION 1
#define RDOF_RAWBURST_BYTES 49
#define RDOF_RAWBURST_VERSION_UTC 2
#define RDOF_RAWBURST_BYTES_UTC 62
#define RDOF_RAWBURST_MAX RDOF_RAWBURST_BYTES_UTC

#define RDOF_RAWBURST_SLOT2 0x01
#define RDOF_RAWBURST_NETWORK 0x02
#define RDOF_RAWBURST_ADDRESSED 0x04
#define RDOF_RAWBURST_GROUP 0x08

typedef struct {
    int64_t ms;     /* UTC, ms since the Unix epoch (rdof_time_ms); 0 unknown */
    int slot;       /* 1 / 2 */
    bool network;   /* "source": "network" (what a roamer or repeater sent), else "rf" */
    int dt, n;
    bool addressed; /* src / dst / group are the call's ("src_id" in the JSON) */
    uint32_t src, dst;
    bool group;
    uint8_t burst[RDOF_BURST_BYTES];
    bool utc;          /* has a UTC slot (slot timing utc, the clock on the grid): version 2 */
    int64_t utc_us;    /* the burst's start, UTC µs */
    int64_t utc_slot;  /* its UTC grid slot n (u40 on the wire) */
} rdof_rawburst_t;

/* The binary form; returns its size, RDOF_RAWBURST_BYTES or, with a UTC slot, RDOF_RAWBURST_BYTES_UTC. */
size_t rdof_rawburst_encode(const rdof_rawburst_t *r, uint8_t out[RDOF_RAWBURST_MAX]);
/* {"DMR": {...}} as MMDVMHost's MQTTRaw writes it; false if it didn't fit. */
bool rdof_rawburst_json(const rdof_rawburst_t *r, char *out, size_t cap);
/* The JSON's "type" for a data type, or its number in tmp. */
const char *rdof_raw_type_name(int dt, char tmp[12]);

#ifdef __cplusplus
}
#endif

#endif
