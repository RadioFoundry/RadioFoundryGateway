// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * What a roamer or repeater transmits, reported as the network calls they
 * are, plus
 * the "TX" message on .../tx.
 */
#ifndef RDOF_NETCALLS_H
#define RDOF_NETCALLS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/fec.h"
#include "rdof/hblink.h"
#include "rdof/rawburst.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t stream_id[4];
    uint32_t src, dst;
    bool group;
    uint32_t freq;
    int cc, slot;
    bool has_csbk; /* a CSBK sent on its own (a radio check) */
    uint8_t csbk[RDOF_BURST_BYTES];
    int frames;    /* the block count a data call's header announces; -1 for voice */
} rdof_tx_call_info_t;

typedef struct {
    bool has_last[3];
    uint8_t last_control[3];
    uint8_t last_burst[3][RDOF_BURST_BYTES];
} rdof_netcalls_t;

void rdof_netcalls_init(rdof_netcalls_t *n);
/* A call start / end as JSON for .../json; false when there's nothing to publish. wall_t: Unix time. */
bool rdof_netcalls_event(rdof_netcalls_t *n, const char *action, const rdof_tx_call_info_t *call, double duration,
                       double wall_t, char *out, size_t cap);
/* A burst for .../raw; false for a repeated header / terminator or a lone CSBK's. */
bool rdof_netcalls_burst_raw(rdof_netcalls_t *n, const rdof_tx_call_info_t *call, uint8_t control,
                           const uint8_t burst[RDOF_BURST_BYTES], double wall_t, rdof_rawburst_t *out);
/* The same as MQTTRaw JSON (the host tests compare it with the golden vectors). */
bool rdof_netcalls_burst(rdof_netcalls_t *n, const rdof_tx_call_info_t *call, uint8_t control,
                       const uint8_t burst[RDOF_BURST_BYTES], double wall_t, char *out, size_t cap);
/* {"TX": {...}} for .../tx. */
void rdof_tx_json(const char *action, const rdof_tx_call_info_t *call, double duration, double wall_t, char *out,
                size_t cap);
/* What a CSBK is, in the words receivers use for it. */
void rdof_csbk_desc(const uint8_t burst[RDOF_BURST_BYTES], char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
