// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The bench API's transmit (docs/spec/bench-api.md section 7): one WS /tx socket's job, as a state machine. The
 * gates, the burst queue, pacing, the key-time limit and the dead-man rule are here; the platform feeds it the
 * tool's text frames and the modem's bytes, ticks it, and closes the socket when it's done.
 *
 * It drives an MMDVM modem in simplex (DMO): GET_VERSION, SET_CONFIG, SET_FREQ (RX = TX = the frequency, the
 * settings' RF level), SET_MODE DMR, then one DMR_DATA2 a burst, handed over RDOF_BENCHTX_LEAD bursts ahead of air
 * time. That modem is the board's own (sdrmodem, via=modem) or a bench radio's (via=radio:NAME, through the link's
 * guard, which forces the lowest power and refuses TX data off the lease). A modem can't place a burst at a sample
 * clock time, so every ack says timed: false.
 *
 * Tool frames: {"type": "bursts", "items": [{"raw_hex", "slot", "n", "at_sc"}, ...]}, {"type": "unkey"},
 * {"type": "ping"}. Each one counts for the dead-man rule. Frames to the tool: ready (freq_hz, protocol; once
 * set up), ack, keyed, unkeyed, underrun, error (then done), done, and what's measured about a keying: modem (the
 * modem's TX on or off), modem_flag, tx_check. `unkeyed` and `done` are this file's model of the modem's queue:
 * bursts handed over, not bursts sent. Portable C, no allocation.
 */
#ifndef RDOF_BENCHTX_H
#define RDOF_BENCHTX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/jsonp.h"
#include "rdof/mmdvm.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_BENCHTX_MAX_FREQS 32 /* as many as a transceiver transmits on (RDOF_CFG_CHANNELS) */
#define RDOF_BENCHTX_MAX_BANDS 8
#define RDOF_BENCHTX_QUEUE 64       /* bursts waiting: 3.8 s of a simplex call */
#define RDOF_BENCHTX_BURST_S 0.060  /* simplex: one burst every 60 ms */
#define RDOF_BENCHTX_LEAD 3         /* bursts handed to the modem ahead of their air time */
#define RDOF_BENCHTX_DEADMAN_S 2.0  /* no frame from the tool this long: stop */
#define RDOF_BENCHTX_REPLY_S 1.0
#define RDOF_BENCHTX_TRIES 3
#define RDOF_BENCHTX_NODES 400      /* JSON nodes in one tool frame (about 64 items) */
#define RDOF_BENCHTX_POLL_S 0.1     /* GET_STATUS while a keying is out */
#define RDOF_BENCHTX_TAIL_S 1.5     /* after a keying, at most this long for the modem to say it's done */
#define RDOF_BENCHTX_LATE_S 0.5     /* the modem still transmitting this long past the modeled end: flagged */
#define RDOF_BENCHTX_HEARD_S 0.4    /* a receiver's latency: what's not heard this long after the end never was */
#define RDOF_BENCHTX_TRACK 600      /* bursts compared with what a receiver heard: 36 s of a keying */
#define RDOF_BENCHTX_EVENTS 12      /* measurement frames waiting for a reader */
#define RDOF_BENCHTX_EVENT_LEN 240
#define RDOF_BENCHTX_SAME_BITS 24   /* a burst heard this close to one handed over is that burst (bit errors) */

/* What the bench's settings allow (bench.tx); the API never changes it. */
typedef struct {
    uint32_t freqs_hz[RDOF_BENCHTX_MAX_FREQS];
    int nfreqs;
    uint32_t bands_hz[RDOF_BENCHTX_MAX_BANDS][2]; /* low, high: any frequency from one to the other */
    int nbands;
    double max_key_s;  /* 0: no TX */
    int rf_level;      /* without max_dbm: the most on the transmitter's range, 0..255 as SET_FREQ's RF level (0: the
                          lowest) */
    int arm_gpio;      /* -1: none; else a jumper to ground that must be in */
    bool has_max_dbm;  /* max_dbm given */
    double max_dbm;    /* the most, dBm at the antenna (rdof_bench_power_dbm) */
} rdof_benchtx_policy_t;

/* freq is one bench.tx allows: in freqs_hz or in one of bands_hz. */
bool rdof_benchtx_allowed(const rdof_benchtx_policy_t *p, uint32_t freq);

/* The section 7 gates for a transmission on freq: NULL when every one is open, else why. built: the firmware has
 * TX; can_tx: the path (front end, modem, radio) can; lease: the session lease's TX frequencies; armed: the arm
 * jumper (ignored without arm_gpio). */
const char *rdof_benchtx_gates(const rdof_benchtx_policy_t *p, bool built, bool can_tx, uint32_t freq,
                             const uint32_t *lease, int nlease, bool armed);

typedef struct {
    void (*to_modem)(void *ctx, const uint8_t *b, size_t n);
    void (*to_tool)(void *ctx, const char *json, size_t n);
    /* A keying ended: for the bench's log. why: "end", "unkey", "underrun", "max_key", "deadman", "stopped",
     * "refused". */
    void (*keying)(void *ctx, double start, double seconds, int bursts, const char *why);
    double (*utc)(void *ctx);   /* optional: UTC seconds for the tool's frames */
    void *ctx;
} rdof_benchtx_io_t;

typedef enum { RDOF_TXS_SETUP = 0, RDOF_TXS_READY, RDOF_TXS_DONE } rdof_benchtx_state_t;

typedef struct {
    uint8_t control;
    uint8_t burst[33];
} rdof_benchtx_item_t;

/* What a keying handed over, against what a separate receiver heard (rdof_benchtx_track). The caller owns it. */
typedef struct {
    struct {
        uint8_t burst[33];
        bool heard;
        double at, heard_at; /* its modeled air time; when the receiver reported it */
    } s[RDOF_BENCHTX_TRACK];
    unsigned n;
} rdof_benchtx_track_t;

typedef struct {
    rdof_benchtx_io_t io;
    uint32_t freq;
    int cc, rf_level;
    double max_key_s;
    rdof_benchtx_state_t state;
    /* setup: the request in flight */
    int step, tries, protocol;
    uint8_t want;          /* the command whose reply is awaited, 0xFF: none */
    double deadline;
    bool replied, nak;
    /* the queue */
    rdof_benchtx_item_t q[RDOF_BENCHTX_QUEUE];
    int qh, qn;
    int voice_n;           /* voice bursts since the last voice sync, for B-F's control byte */
    /* air */
    bool keyed, unkey_asked, last_voice;
    double key_start, air_free; /* when the modem's queue (as modeled here) runs out */
    int key_bursts;
    double last_tool;
    bool need_mode;        /* after a stop: SET_MODE DMR again before the next burst */
    int data_nak;          /* the modem NAKed a burst: 0x200 | its reason, or 0x100 with none given */
    /* counters */
    long sent, keyings, underruns, frames;
    /* what the modem says (GET_STATUS) and what was heard, against the model above */
    double rx_now;         /* rdof_benchtx_modem's time, for its frames */
    double next_poll, tx_on, tx_off, tail_model, tail_start, tail_until;
    int polls, answers, tail_answers, st_space, idle_space, space_min, n_quiet, n_late;
    int held_space;        /* the buffer's space when tail_held came */
    bool st_seen, st_tx, was_on, tailing;
    bool tail_held;        /* after a keying the modem said TX off with bursts still in its buffer */
    rdof_benchtx_track_t *track;
    unsigned track_first;
    const char *no_heard;  /* why nothing compares it: the tx_check's why */
    char ev[RDOF_BENCHTX_EVENTS][RDOF_BENCHTX_EVENT_LEN];
    int evh, evn;
    long ev_lost;
    char error[96];
    rdof_mmdvm_reader_t reader;
    rdof_jnode_t nodes[RDOF_BENCHTX_NODES];
    char strbuf[8192];
} rdof_benchtx_t;

/* A job on freq, cc for SET_CONFIG; max_key_s and rf_level from the policy. */
void rdof_benchtx_init(rdof_benchtx_t *t, const rdof_benchtx_io_t *io, uint32_t freq, int cc, int rf_level,
                     double max_key_s, double now);
/* A text frame from the tool. */
void rdof_benchtx_tool(rdof_benchtx_t *t, const char *json, size_t n, double now);
/* The tool is alive (a WebSocket ping). */
void rdof_benchtx_alive(rdof_benchtx_t *t, double now);
/* A separate receiver will report what it hears (rdof_benchtx_heard): compare each burst with it. NULL, with why,
 * says why not: a keying's tx_check then gives its figures as estimates. */
void rdof_benchtx_track(rdof_benchtx_t *t, rdof_benchtx_track_t *track, const char *why);
/* A separate receiver decoded this burst on the job's frequency. */
void rdof_benchtx_heard(rdof_benchtx_t *t, const uint8_t burst[33], double now);
/* The next measurement frame (modem, modem_flag, tx_check), as the tool got it: its length, 0 with none. */
size_t rdof_benchtx_next_event(rdof_benchtx_t *t, char *out, size_t cap);
/* The last keying's measurements are finished: the modem said it's done, or the wait for it ran out. */
bool rdof_benchtx_settled(const rdof_benchtx_t *t);
/* Bytes from the modem. */
void rdof_benchtx_modem(rdof_benchtx_t *t, const uint8_t *b, size_t n, double now);
/* Every 20 ms or so. */
void rdof_benchtx_tick(rdof_benchtx_t *t, double now);
/* Stop now: the socket closed, the lease ended, the arm jumper came out. The modem goes idle; done with why. */
void rdof_benchtx_stop(rdof_benchtx_t *t, const char *code, const char *why, double now);
bool rdof_benchtx_done(const rdof_benchtx_t *t);
/* "idle", "setup", "armed" (ready, nothing on the air), "keyed". */
const char *rdof_benchtx_state(const rdof_benchtx_t *t);

/* The DMR_DATA control byte for a raw burst: voice sync 0x20, data sync 0x40 | data type, voice B-F the position
 * (n: 1-5, or -1 to count from the last voice sync in *voice_n). -1 when it has no sync and isn't voice. */
int rdof_benchtx_control(const uint8_t burst[33], int n, int *voice_n);

#ifdef __cplusplus
}
#endif

#endif
