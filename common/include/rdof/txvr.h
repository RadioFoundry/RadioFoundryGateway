// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The roaming transceiver.
 *
 * One simplex radio, tuned per call to wherever HBlink4 needs it (DMRT), and
 * an ordinary receiver on its idle channel (or scanning) between calls. The
 * platform calls rdof_txvr_on_tune / rdof_txvr_on_burst from its HBlink4 task,
 * rdof_txvr_poll every ~20 ms from the transceiver's own task, and wires the
 * modem's on_rx to rdof_txvr_heard. lock / unlock (optional) guard the state
 * the platform's tasks share.
 *
 * Bounded: RDOF_TXVR_QUEUE bursts queued for a call
 * (the rest dropped and counted), RDOF_TXVR_ORPHANS streams of up to
 * RDOF_TXVR_ORPHAN_BURSTS bursts held for a DMRT still to come.
 */
#ifndef RDOF_TXVR_H
#define RDOF_TXVR_H

#include <stdbool.h>
#include <stdint.h>

#include "rdof/features.h"
#include "rdof/hblink.h"
#include "rdof/jitter.h"
#include "rdof/modem.h"
#include "rdof/netcalls.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The receiver a transceiver or repeater passes what it hears to (rx.Receiver). dbm is NAN when there's none. */
typedef struct {
    void (*hear)(void *ctx, int slot, uint8_t control, const uint8_t burst[RDOF_BURST_BYTES], double dbm, int cc,
                 uint32_t freq);
    void (*tick)(void *ctx);
    void *ctx;
} rdof_receiver_t;

#if RDOF_FEATURE_TX

#define RDOF_TXVR_FREQ_TOLERANCE_HZ 100
#define RDOF_TXVR_ORPHAN_S 0.5
#define RDOF_TXVR_STREAM_TIMEOUT_S 1.5
#define RDOF_TXVR_STATUS_EVERY_S 0.06
#define RDOF_TXVR_BURST_S 0.06
#define RDOF_TXVR_IDLE_STATUS_EVERY_S 2.0
#define RDOF_TXVR_STATUS_GIVE_UP_S 2.0
#define RDOF_TXVR_TX_TAIL_S 0.5
#define RDOF_TXVR_RX_HOLD_S 1.0
#define RDOF_TXVR_CALL_TAIL_S 0.5

#ifndef RDOF_TXVR_QUEUE
#define RDOF_TXVR_QUEUE 256
#endif
#define RDOF_TXVR_ORPHANS 4
#define RDOF_TXVR_ORPHAN_BURSTS 16
#define RDOF_TXVR_REFUSED 64
#define RDOF_TXVR_MAX_CHANNELS 32
#define RDOF_TXVR_MAX_COVERED 32
#define RDOF_TXVR_IDS 16

typedef struct {
    uint32_t freq;
    int cc, power;
} rdof_channel_t;

typedef struct {
    rdof_channel_t channels[RDOF_TXVR_MAX_CHANNELS];
    int nchannels;
    double dwell_s, hang_s, carrier_s;
} rdof_scan_t;

typedef enum { RDOF_CALL_PENDING, RDOF_CALL_LISTENING, RDOF_CALL_ON_AIR, RDOF_CALL_DONE } rdof_txcall_state_t;

typedef struct {
    rdof_tune_t tune;
    rdof_txcall_state_t state;
    rdof_net_burst_t bursts[RDOF_TXVR_QUEUE]; /* a ring */
    int head, count;
    int dropped;
    bool ended;
    double last_burst, listen_until, quiet_since;
    bool started;
    int extra;
    int headers, terminators; /* queued for this call (R11.4) */
    int vpos;                 /* the voice position queued next (0 = A ... 5 = F); -1: no A yet (R6.5) */
    int vfilled;              /* silence since the last real voice burst */
    uint8_t frags[5][32];     /* embedded fields B-E (1-4), last heard: for silence */
    uint8_t has_frag;         /* bit n: frags[n] */
    bool has_quiet_at;
    double quiet_at;
    uint32_t src, dst;
    bool group;
    double on_air_at;
    bool has_csbk;
    uint8_t csbk[RDOF_BURST_BYTES];
    bool wants_reply;
    bool collecting_udata;
    uint8_t udata[48];
    int udata_len;
    bool has_lc;
    uint8_t lc9[9]; /* the voice LC from its header: the terminators of a stream that times out */
    int vshift;      /* adaptive: where a network position goes out, positions on (mod 6): grown less shrunk */
    int vpi;         /* the PI of the stream's last B-F burst: for one moved to another position */
    double air_seen; /* when what the radio holds has gone out, as the poll last wrote it (under the lock) */
    rdof_jb_stream_t jbs;
} rdof_txcall_t;

typedef struct rdof_txvr {
    rdof_radio_t *radio;
    uint32_t peer_id;
    rdof_channel_t channels[RDOF_TXVR_MAX_CHANNELS];
    int nchannels;
    double listen_s, clear_s;
    int prebuffer, csbk_preambles;
    double answer_s, answer_max_s;
    uint32_t covered[RDOF_TXVR_MAX_COVERED];
    int ncovered;
    bool has_idle;
    rdof_channel_t idle;
    double idle_after_s;
    char cwid[41];
    double cwid_every_s;
    bool has_scan;
    rdof_scan_t scan;

    /* hooks */
    void (*send)(void *ctx, const uint8_t *data, size_t len);              /* to HBlink4 */
    void (*on_retune)(void *ctx, uint32_t freq, int cc);
    void (*on_tx)(void *ctx, const char *action, const rdof_tx_call_info_t *call, double duration);
    void (*on_sent)(void *ctx, const rdof_tx_call_info_t *call, const rdof_net_burst_t *b);
    void (*on_scan)(void *ctx, bool holding);
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    void *ctx;
    rdof_receiver_t receiver; /* hear NULL: none */
    double (*clock)(void *ctx);

    /* state */
    bool has_answer_window;
    uint32_t aw_freq;
    int aw_cc;
    double aw_until, aw_cap;
    int scan_next;
    bool carrier_checked, stopped;
    bool has_heard_call, heard_call_ended;
    uint32_t heard_src, heard_dst;
    double heard_at;
    bool has_tuned;
    uint32_t tuned_freq;
    int tuned_cc, tuned_power;
    double tuned_at;
    volatile bool tuning;
    bool has_call;
    rdof_txcall_t call;
    bool ready;
    double idle_since;
    struct {
        bool used;
        uint8_t stream_id[4];
        double at;
        rdof_net_burst_t bursts[RDOF_TXVR_ORPHAN_BURSTS];
        int n;
    } orphans[RDOF_TXVR_ORPHANS];
    uint8_t refused[RDOF_TXVR_REFUSED][4];
    int nrefused, refused_next;
    double last_status;
    bool has_status_missed;
    double status_missed;
    int space;
    double air_until; /* when what the radio holds has gone out (one burst per 60 ms) */
    struct {
        uint32_t freq;
        double at;
    } last_id[RDOF_TXVR_IDS];
    int nlast_id;
    uint32_t status_misses; /* missed status replies ridden out (diagnostics) */
    rdof_jb_t jb;           /* the jitter buffer: fixed at prebuffer unless jb.adaptive */
} rdof_txvr_t;

/* Defaults (listen 0.3 s, clear 2 s, prebuffer 6, 2 preambles, answer 0.75 / 5 s, CW ID every 900 s); the jitter
 * buffer fixed at prebuffer (jb.adaptive false; bounds 180 and 480 ms when set). */
void rdof_txvr_init(rdof_txvr_t *t, rdof_radio_t *radio, uint32_t peer_id);
void rdof_scan_defaults(rdof_scan_t *s);

void rdof_txvr_on_tune(rdof_txvr_t *t, const rdof_tune_t *tune);
void rdof_txvr_on_burst(rdof_txvr_t *t, const rdof_net_burst_t *b);
/* The modem's on_rx (reader context). */
void rdof_txvr_heard(rdof_txvr_t *t, int slot, uint8_t control, const uint8_t burst[RDOF_BURST_BYTES], int rssi);
/* A receiver sink's call start / end (tracker on_call): the last call heard, for listen-before-talk. */
void rdof_txvr_heard_call(rdof_txvr_t *t, const char *action, uint32_t src, uint32_t dst);

/* Radio into DMR mode, on the idle channel. false: a modem error. */
bool rdof_txvr_start(rdof_txvr_t *t);
/* false: a modem error (the platform then restarts the modem and calls rdof_txvr_start). */
bool rdof_txvr_poll(rdof_txvr_t *t);

/* (frequency, color code) for DMRC: 0, 0 when untuned. */
void rdof_txvr_channel_now(const rdof_txvr_t *t, uint32_t *freq, int *cc);
bool rdof_txvr_on_idle_channel(const rdof_txvr_t *t);
bool rdof_txvr_listening(const rdof_txvr_t *t);
bool rdof_txvr_scanning(const rdof_txvr_t *t);
const char *rdof_txvr_state_name(const rdof_txvr_t *t);
/* The call's details, as the network calls report them (rdof/netcalls.h). */
void rdof_txvr_call_info(const rdof_txvr_t *t, const rdof_txcall_t *call, rdof_tx_call_info_t *info);

#endif /* RDOF_FEATURE_TX */

#ifdef __cplusplus
}
#endif

#endif
