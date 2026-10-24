// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The repeater. A duplex
 * MMDVM modem run as a fixed DMR repeater on HBlink4, or (sfr) an OpenGD77
 * single frequency repeater that repeats its inbound slot itself, or (dcdm)
 * a dual capacity direct mode gateway: two simplex calls on one frequency. The behavior
 * is docs/spec/repeater.md's; rule ids (R4.6) point there.
 *
 * The platform queues what other tasks get (rdof_repeater_on_burst from the
 * HBlink4 task; rdof_repeater_heard / _lost / _nak as the modem's callbacks)
 * and calls rdof_repeater_poll every ~20 ms. lock / unlock guard those queues.
 *
 * Bounded: RDOF_RPT_OUT bursts queued per slot,
 * RDOF_RPT_PENDING per network stream held for the prebuffer, RDOF_RPT_CALLS
 * network streams alive at once, RDOF_RPT_IN queued inputs.
 */
#ifndef RDOF_REPEATER_H
#define RDOF_REPEATER_H

#include <stdbool.h>
#include <stdint.h>

#include "rdof/duplex.h"
#include "rdof/features.h"
#include "rdof/hblink.h"
#include "rdof/jitter.h"
#include "rdof/modem.h"
#include "rdof/netcalls.h"
#include "rdof/txvr.h"

#ifdef __cplusplus
extern "C" {
#endif

#if RDOF_FEATURE_REPEATER

#define RDOF_RPT_HEADERS 3           /* R4.2: H, more while voice hasn't come */
#define RDOF_RPT_BURST_S 0.06
#define RDOF_RPT_FILL_AHEAD_S 0.12   /* R10.3: filler while the modem holds less than this for the slot */
#define RDOF_RPT_KEY_LEAD_S 0.36     /* R3.3 */
#define RDOF_RPT_STATUS_EVERY_S 0.06
#define RDOF_RPT_IDLE_STATUS_EVERY_S 2.0
#define RDOF_RPT_STATUS_GIVE_UP_S 2.0
#define RDOF_RPT_NET_TIMEOUT_S 1.5
#define RDOF_RPT_RF_TIMEOUT_S 1.0
#define RDOF_RPT_ECHO_S 2.0
#define RDOF_RPT_SFR_QUIET_S 0.3

#ifndef RDOF_RPT_OUT
#define RDOF_RPT_OUT 192
#endif
#ifndef RDOF_RPT_PENDING
#define RDOF_RPT_PENDING 96
#endif
#define RDOF_RPT_CALLS 6
#define RDOF_RPT_IN 64
#define RDOF_RPT_DONE 64

typedef enum { RDOF_KIND_NONE = 0, RDOF_KIND_VOICE, RDOF_KIND_DATA } rdof_rpt_kind_t;

typedef struct {
    bool end;          /* the end of a call (nothing to send) */
    uint8_t control;
    uint8_t burst[RDOF_BURST_BYTES]; /* an end of a voice call: its terminator, for the call hang */
    int8_t call;       /* index into the call pool, -1 none */
    bool report;
    uint8_t end_kind;  /* an end: RDOF_KIND_VOICE (the call hang follows), RDOF_KIND_DATA, or none */
    uint32_t seq;      /* an end: the slot's call count when it ended (a later call: no hang) */
} rdof_rpt_out_t;

/* One voice call's stream on the outbound (section 6): from the first A, positions kept whole. */
typedef struct {
    rdof_voice_regen_t regen;
    int cc;
    uint8_t header[RDOF_BURST_BYTES];
    bool wait_a;
    int next, filled;
    int shift; /* a network stream, adaptive: where a position goes out, positions on (mod 6) */
    int pi;    /* the PI of its last B-F burst: for one moved to another position */
} rdof_voice_out_t;

typedef struct {
    bool used;
    rdof_tune_t tune;
    int slot;
    uint32_t src, dst;
    bool group;
    rdof_rpt_kind_t kind;
    uint8_t lc9[9];
    bool has_voice;
    rdof_voice_out_t voice;
    int blocks;
    rdof_rpt_out_t pending[RDOF_RPT_PENDING];
    int npending;
    int received;
    bool started, on_air, finished;
    double on_air_at, last_burst;
    bool has_csbk;
    uint8_t csbk[RDOF_BURST_BYTES];
    rdof_jb_stream_t jbs;
} rdof_rpt_call_t;

typedef struct {
    bool active;
    rdof_rpt_kind_t kind;
    uint32_t src, dst;
    bool group;
    uint8_t lc9[9];
    double last;
    rdof_voice_out_t voice;
    int blocks;
} rdof_rpt_rf_t;

typedef struct {
    int n;
    rdof_rpt_rf_t rf;
    int net; /* call pool index, -1 none */
    rdof_rpt_out_t out[RDOF_RPT_OUT];
    int head, count;
    int space, max_space;
    bool has_last_rf;
    uint32_t last_src, last_dst;
    double last_at;
    uint8_t done[RDOF_RPT_DONE][4];
    int ndone, done_next;
    rdof_activity_t activity;
    uint8_t late[5][32];
    uint8_t late_have;
    uint32_t seq;                         /* calls started on the slot */
    bool has_hang;                        /* call hang (R3.5): its terminator, until */
    uint8_t hang_term[RDOF_BURST_BYTES];
    double hang_until;
    double air_until, net_until;          /* when what the modem holds has gone out; the network's among it */
} rdof_rpt_slot_t;

typedef struct {
    int slot;
    bool lost;
    uint8_t control;
    uint8_t burst[RDOF_BURST_BYTES];
    int rssi;
} rdof_rpt_rf_in_t;

typedef struct rdof_repeater {
    rdof_radio_t *radio;
    uint32_t peer_id;
    uint32_t rx_freq, tx_freq;
    int cc, power;
    bool enabled[3];
    double inactivity_s;  /* T_MSInactiv (R3.4); never less than call_hang_s in use (R3.6) */
    double call_hang_s;   /* T_CallHt (R3.5) */
    bool short_lc_ts1_low; /* interop (R9.6): Act_Updt with TS1 in the low nibble */
    int prebuffer;
    char cwid[41];
    double cwid_every_s;
    bool sfr;
    bool dcdm;            /* dual capacity direct mode: a gateway on one frequency, both slots, nothing repeated */
    int inbound, outbound;

    void (*on_tx)(void *ctx, const char *action, const rdof_tx_call_info_t *call, double duration);
    void (*on_sent)(void *ctx, const rdof_tx_call_info_t *call, const rdof_net_burst_t *b);
    void (*on_key)(void *ctx, bool keyed);
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    void *ctx;
    rdof_receiver_t receiver;
    double (*clock)(void *ctx);

    rdof_rpt_slot_t slots[3]; /* 1 and 2 */
    rdof_rpt_call_t calls[RDOF_RPT_CALLS];
    bool keyed, ready, transmitting;
    rdof_rpt_rf_in_t rf_in[RDOF_RPT_IN];
    int rf_head, rf_count;
    rdof_net_burst_t net_in[RDOF_RPT_IN];
    double net_at[RDOF_RPT_IN]; /* when each came */
    int net_head, net_count;
    struct {
        uint8_t command;
        int err;
    } naks[16];
    int nak_head, nak_count;
    double tx_until, last_status, lead_until;
    bool has_status_missed;
    double status_missed;
    bool has_short_lc;
    rdof_activity_t short_lc[2];
    double last_id;
    bool sent_since_id;
    double inbound_last;
    bool inbound_flag;
    uint32_t dropped; /* inputs or bursts dropped for want of room */
    rdof_jb_t jb;     /* the network's jitter buffer: fixed at prebuffer unless jb.adaptive */
} rdof_repeater_t;

/* Defaults: both slots, T_MSInactiv 5 s, call hang 3 s, prebuffer 6 (the jitter buffer fixed), CW ID every 900 s. */
void rdof_repeater_init(rdof_repeater_t *r, rdof_radio_t *radio, uint32_t peer_id, uint32_t rx_freq, uint32_t tx_freq,
                      int cc);
void rdof_repeater_set_call_hang(rdof_repeater_t *r, double call_hang_s);
void rdof_repeater_set_sfr(rdof_repeater_t *r, int inbound_slot);
/* Dual capacity direct mode (docs/spec/repeater.md section 13): one frequency (tx_freq), a DCDM modem, both slots
 * gatewayed between RF and the network, nothing repeated, no call hang. */
void rdof_repeater_set_dcdm(rdof_repeater_t *r);

void rdof_repeater_on_burst(rdof_repeater_t *r, const rdof_net_burst_t *b);
void rdof_repeater_heard(rdof_repeater_t *r, int slot, uint8_t control, const uint8_t burst[RDOF_BURST_BYTES], int rssi);
void rdof_repeater_lost(rdof_repeater_t *r, int slot);
void rdof_repeater_nak(rdof_repeater_t *r, uint8_t command, int err);

bool rdof_repeater_start(rdof_repeater_t *r);
bool rdof_repeater_poll(rdof_repeater_t *r);
bool rdof_repeater_busy(rdof_repeater_t *r, int slot);
bool rdof_repeater_on_air(const rdof_repeater_t *r);
/* Stop: unkey if keyed. */
void rdof_repeater_stop(rdof_repeater_t *r);

#endif /* RDOF_FEATURE_REPEATER */

#ifdef __cplusplus
}
#endif

#endif
