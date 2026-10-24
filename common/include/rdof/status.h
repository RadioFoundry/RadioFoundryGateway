// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The MQTT status and receiver messages a roamer or repeater publishes:
 *   {root}/_sdr/{name}/status    retained, every 10 s and on changes
 *   {root}/{name}/{app}/json     retained {"Receiver": ...} at start
 */
#ifndef RDOF_STATUS_H
#define RDOF_STATUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/hblink.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The last will and the stop message (JSON with a space after , and :). */
#define RDOF_STATUS_OFFLINE "{\"state\": \"offline\"}"

/* A float as the shortest text that reads back the same, ".0" when whole. */
void rdof_format_repr(double v, char *out, size_t cap);

typedef struct {
    double at;              /* Unix time */
    uint32_t freq;
    int cc;
    bool listening, scanning, radio, hblink, on_air;
} rdof_roamer_status_t;

void rdof_roamer_status_json(const rdof_roamer_status_t *s, char *out, size_t cap);
void rdof_roamer_receiver_json(const char *name, uint32_t idle_freq, int idle_cc, const char *device,
                             const rdof_location_t *loc, char *out, size_t cap);

typedef struct {
    double at;
    uint32_t rx, tx;
    int cc;
    bool radio, hblink, on_air;
    bool slot_configured[3], slot_busy[3];
} rdof_repeater_status_t;

void rdof_repeater_status_json(const rdof_repeater_status_t *s, char *out, size_t cap);
/* inbound_slot 0: not an SFR. */
void rdof_repeater_receiver_json(const char *name, uint32_t rx, uint32_t tx, int cc, const char *device,
                               const rdof_location_t *loc, int inbound_slot, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
