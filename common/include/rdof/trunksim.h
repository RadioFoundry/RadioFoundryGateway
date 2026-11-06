// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * A trunked site's air, simulated (docs/spec/fw/trunk.md section 10): simulated MSs (rdof/t3.h's rdof_t3ms) against a
 * site (rdof/trunkunit.h) through the full burst path. Each slot: the site's outbound bursts built (rdof_trunkunit_slot:
 * CRC, BPTC, slot type, sync) and read back for the MSs that listen on that channel; what the MSs send built as their
 * bursts (control blocks with the MS data sync, voice headers and terminators with their LC, voice) and heard by the
 * site (rdof_trunkunit_rx). Two MSs in one TSCC inbound slot collide: the site hears a burst whose CRC fails.
 *
 * What rdofradio --sim-air runs, and the host tests. Needs the controller (rust's t3 feature), as trunkunit does.
 */
#ifndef RDOF_TRUNKSIM_H
#define RDOF_TRUNKSIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/features.h"

#include "rdof/t3.h"
#include "rdof/trunkunit.h"

#ifdef __cplusplus
extern "C" {
#endif

#if RDOF_FEATURE_REPEATER

#define RDOF_TRUNKSIM_MS 64

typedef struct {
    rdof_trunkunit_t *u;
    int n;
    rdof_t3ms_t *ms[RDOF_TRUNKSIM_MS]; /* rdof_t3ms_size() bytes each, the caller's */
    uint32_t id[RDOF_TRUNKSIM_MS];
    int voice_n[RDOF_TRUNKSIM_MS];     /* each MS's next voice burst (A = 0 .. F = 5) */
    /* counters */
    uint32_t out_pdus, out_bad, in_pdus, in_voice, collisions;
} rdof_trunksim_t;

/* n MSs (up to RDOF_TRUNKSIM_MS), each initialized by the caller (rdof_t3ms_init) with id[i]. */
void rdof_trunksim_init(rdof_trunksim_t *s, rdof_trunkunit_t *u, int n, rdof_t3ms_t *const *ms, const uint32_t *id);
/* Slot k at wall clock now_ms: the site's controller, the outbound, the MSs' inbound. Call once per k, in order. */
void rdof_trunksim_slot(rdof_trunksim_t *s, int64_t k, int64_t now_ms);
/* The MS with this ID, -1 none. */
int rdof_trunksim_find(const rdof_trunksim_t *s, uint32_t id);

#endif /* RDOF_FEATURE_REPEATER */

#ifdef __cplusplus
}
#endif

#endif
