// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * A trunked site's air, simulated (docs/spec/fw/tier3.md section 10): simulated MSs (rdof/t3.h's rdof_t3ms) against a
 * site (rdof/t3unit.h) through the full burst path. Each slot: the site's outbound bursts built (rdof_t3unit_slot:
 * CRC, BPTC, slot type, sync) and read back for the MSs that listen on that channel; what the MSs send built as their
 * bursts (control blocks with the MS data sync, voice headers and terminators with their LC, voice) and heard by the
 * site (rdof_t3unit_rx). Two MSs in one TSCC inbound slot collide: the site hears a burst whose CRC fails.
 *
 * What rdofradio --sim-air runs, and the host tests. Needs the controller (rust's t3 feature), as trunkunit does.
 */
#ifndef RDOF_T3SIM_H
#define RDOF_T3SIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/features.h"

#include "rdof/t3.h"
#include "rdof/t3unit.h"

#ifdef __cplusplus
extern "C" {
#endif

#if RDOF_FEATURE_REPEATER

#define RDOF_T3SIM_MS 64
#define RDOF_T3SIM_EXT 32 /* external bursts a slot */

/* An external MS's burst for the inbound (the hook ext_in): channel ch (2 c + slot - 1) and the burst as heard. The
 * sim sets fate. */
typedef enum { RDOF_T3SIM_HEARD, RDOF_T3SIM_COLLIDED, RDOF_T3SIM_DROPPED } rdof_t3sim_fate_t;
typedef struct {
    int ch;
    rdof_burst_t b;
    rdof_t3sim_fate_t fate;
    void *tag; /* the hook's */
} rdof_t3sim_in_t;

/* What carrier c aired in a slot, for external MSs (the hook ext_out). */
typedef struct {
    bool aired;                      /* false: nothing (an outside carrier with no burst) */
    uint8_t burst[RDOF_BURST_BYTES];
    int dt, cc;                      /* from the slot type, -1 unreadable (voice) */
    bool pdu_ok;                     /* pdu: a control block whose CRC passed */
    uint8_t pdu[10];
    uint8_t cach[3];                 /* the CACH before this slot: AT, TC, LCSS, a short LC fragment */
    int at;                          /* its AT bit */
} rdof_t3sim_out_t;

typedef struct {
    rdof_t3unit_t *u;
    int n;
    rdof_t3ms_t *ms[RDOF_T3SIM_MS]; /* rdof_t3ms_size() bytes each, the caller's */
    uint32_t id[RDOF_T3SIM_MS];
    int voice_n[RDOF_T3SIM_MS];     /* each MS's next voice burst (A = 0 .. F = 5) */
    /* Carriers whose outbound comes from elsewhere (bit c), as aired by a split TX half, say: outbound gives slot k's
     * burst on carrier c (false: nothing aired). The sim doesn't ask the unit for them. */
    uint32_t outside;
    bool (*outbound)(void *ctx, int c, int64_t k, uint8_t burst[RDOF_BURST_BYTES]);
    void *outbound_ctx;
    /* External MSs (rdofradio --sim-ext). ext_in gives slot k's bursts (up to max), asked after the internal MSs
     * decided theirs: each joins its channel's collision count; a burst on a channel not on air in slot k (the wrong
     * slot, no such carrier) is dropped. ext_out gets what each carrier aired in slot k, after the internal MSs heard
     * it, the AT bit of the CACH after it (rdof_t3unit_at), and ext_in's bursts with their fates. */
    int (*ext_in)(void *ctx, int64_t k, rdof_t3sim_in_t *in, int max);
    void (*ext_out)(void *ctx, int64_t k, const rdof_t3sim_out_t *out, int ncar, int at_next,
                    const rdof_t3sim_in_t *in, int nin);
    void *ext_ctx;
    int32_t slc[RDOF_T3_CARRIERS]; /* the short LC on air on each carrier, -1 none yet */
    /* counters */
    uint32_t out_pdus, out_bad, in_pdus, in_voice, collisions, ext_in_n, ext_drop;
} rdof_t3sim_t;

/* n MSs (up to RDOF_T3SIM_MS), each initialized by the caller (rdof_t3ms_init) with id[i]. */
void rdof_t3sim_init(rdof_t3sim_t *s, rdof_t3unit_t *u, int n, rdof_t3ms_t *const *ms, const uint32_t *id);
/* Slot k at wall clock now_ms: the site's controller, the outbound, the MSs' inbound. Call once per k, in order. */
void rdof_t3sim_slot(rdof_t3sim_t *s, int64_t k, int64_t now_ms);
/* An MS's burst for tx: a control block, a voice header or terminator with its LC, or voice burst n (A = 0 .. F = 5,
   silence). b->cc and the slot from cc and tx->ch. False: nothing to air. */
bool rdof_t3sim_ms_burst(const rdof_t3ms_tx_t *tx, int n, int cc, rdof_burst_t *b);
/* The MS with this ID, -1 none. */
int rdof_t3sim_find(const rdof_t3sim_t *s, uint32_t id);

#endif /* RDOF_FEATURE_REPEATER */

#ifdef __cplusplus
}
#endif

#endif
