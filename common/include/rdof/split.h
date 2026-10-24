// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Split duplex: two MMDVM modems, a TX half and an RX half, as one duplex
 * MMDVM modem. The halves speak
 * RadioFoundry's split duplex extension (vendor command 0xD6, "SPLIT1" in
 * their description; RadioFoundry docs/spec/split-duplex.md, MIT).
 *
 * The repeater talks MMDVM to one duplex modem (rdof_modem_t over an
 * rdof_modem_port_t) whatever is behind it: an MMDVM_HS dual hat, the SDR modem,
 * or this. So the MMDVM protocol is the front-end interface, and rdof_split_t is
 * a composite behind it.
 *
 * No I/O, no threads, no allocation: bytes in from upstream (what rdof_modem_t
 * writes) and from each half, with the time in seconds; bytes out with
 * rdof_split_take. The platform serializes the calls (one lock) and ticks it
 * every ~20 ms. Checked against the golden scenarios frame for frame (scenarios.h).
 */
#ifndef RDOF_SPLIT_H
#define RDOF_SPLIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/features.h"
#include "rdof/mmdvm.h"
#include "rdof/splitops.h"

#ifdef __cplusplus
extern "C" {
#endif

#if RDOF_FEATURE_REPEATER

#define RDOF_ODR_SPLIT 0xD6
enum { RDOF_SPLIT_INFO = 0, RDOF_SPLIT_SET_ROLE, RDOF_SPLIT_TIME, RDOF_SPLIT_GET_EPOCH, RDOF_SPLIT_SET_EPOCH, RDOF_SPLIT_SET_AT,
       RDOF_SPLIT_STATUS };
enum { RDOF_SPLIT_ROLE_NORMAL = 0, RDOF_SPLIT_ROLE_TX = 1, RDOF_SPLIT_ROLE_RX = 2 };
#define RDOF_SPLIT_TX 0
#define RDOF_SPLIT_RX 1
/* What its GET_VERSION says (protocol 1). rdof_radio_detect knows it. */
#define RDOF_SPLIT_DESCRIPTION "RadioFoundry Gateway split duplex modem"

#define RDOF_SPLIT_SYNC_SAMPLES 8
#define RDOF_SPLIT_TIME_EVERY_S 1.0
#define RDOF_SPLIT_EPOCH_EVERY_S 10.0
#define RDOF_SPLIT_EPOCH_EARLY_EVERY_S 2.0
#define RDOF_SPLIT_EPOCH_EARLY_S 30.0
#define RDOF_SPLIT_EPOCH_RETRY_S 0.2
#define RDOF_SPLIT_AT_EVERY_S 1.0
#define RDOF_SPLIT_DATA_BUSY_S 0.36
#define RDOF_SPLIT_REQUEST_TIMEOUT_S 1.0
#define RDOF_SPLIT_DEAD_MISSES 3 /* requests in a row with no reply: the half is gone */

/* ClockSync: one half's modem clock against the host's (split-duplex.md 6). */
#define RDOF_CLOCK_KEEP 16
#define RDOF_CLOCK_GROUP 4
#define RDOF_CLOCK_POINTS 160 /* one per 4 s at one sample a second, over 120 s; bring-up's burst adds a few */
#define RDOF_CLOCK_FIT_S 120.0

typedef struct {
    int64_t rtt;
    double mid, off;
} rdof_clock_sample_t;

typedef struct {
    rdof_clock_sample_t samples[RDOF_CLOCK_KEEP]; /* the last KEEP, oldest first */
    int nsamples;
    rdof_clock_sample_t group[RDOF_CLOCK_GROUP];
    int ngroup;
    double pts_h[RDOF_CLOCK_POINTS], pts_o[RDOF_CLOCK_POINTS];
    int64_t pts_r[RDOF_CLOCK_POINTS]; /* each point's round trip: its error bound */
    int npts;
    bool has_base;
    double base;
    uint32_t count;
} rdof_clock_sync_t;

int32_t rdof_wrap_diff(uint32_t a, uint32_t b);
void rdof_clock_sync_init(rdof_clock_sync_t *c);
/* t1, t4: host µs; t_rx, t_tx: modem µs. Returns the round trip. */
int64_t rdof_clock_sync_add(rdof_clock_sync_t *c, int64_t t1, int64_t t4, uint32_t t_rx, uint32_t t_tx);
/* offset(h) = icpt + rate (h - h0). Needs one sample. */
void rdof_clock_sync_fit(const rdof_clock_sync_t *c, double *rate, double *icpt, double *h0);
uint32_t rdof_clock_sync_to_modem(const rdof_clock_sync_t *c, double h);
/* The fit's worst error (µs): half the largest round trip it rests on (RadioFoundry PR #109); INFINITY with no sample. */
double rdof_clock_sync_bound_us(const rdof_clock_sync_t *c);
double rdof_clock_sync_to_host(const rdof_clock_sync_t *c, uint32_t m, double near_h);
/* SET_EPOCH's t_us and period_ns for the RX half from the TX half's GET_EPOCH (split-duplex.md 6.2). */
void rdof_split_epoch_for_rx(const rdof_clock_sync_t *tx, const rdof_clock_sync_t *rx, uint32_t t_us, int ts, double now_h,
                           uint32_t *t_rx, uint32_t *period_ns);
/* Which epochs go to the RX half (split-duplex.md 6.3 as of RadioFoundry PR #109): the first only with its
 * error bound under limit_us; later ones only when they also land within agree_us of the last one sent, by whole
 * frames; REPLACE in a row that agree with each other replace it. Every decision goes in the log ring. */
#define RDOF_RELAY_LIMIT_US 10000.0
#define RDOF_RELAY_AGREE_US 7500.0
#define RDOF_RELAY_REPLACE 3
#define RDOF_RELAY_LOG 8
typedef enum { RDOF_RELAY_SEND, RDOF_RELAY_SKIP, RDOF_RELAY_HOLD, RDOF_RELAY_REPLACED } rdof_relay_action_t;
typedef struct {
    uint32_t t, period_ns;
    double bound_us, rem_us; /* rem_us NAN: the first */
    rdof_relay_action_t action;
} rdof_relay_entry_t;
typedef struct {
    double limit_us, agree_us;
    bool has_sent;
    uint32_t sent;
    uint32_t held[RDOF_RELAY_REPLACE];
    int nheld;
    uint32_t skipped, rejected, decisions;
    rdof_relay_entry_t log[RDOF_RELAY_LOG]; /* the last RDOF_RELAY_LOG: log[(decisions - 1) % RDOF_RELAY_LOG] newest */
} rdof_epoch_relay_t;
void rdof_epoch_relay_init(rdof_epoch_relay_t *r, double limit_us, double agree_us);
void rdof_epoch_relay_reset(rdof_epoch_relay_t *r);
double rdof_frame_rem(uint32_t t_a, uint32_t t_b, uint32_t period_ns);
bool rdof_epoch_relay_decide(rdof_epoch_relay_t *r, uint32_t t, uint32_t period_ns, double bound_us);

/* " SPLIT1" (or later) in a modem's description. */
bool rdof_has_split(const char *description);

/* A request to a half, waiting its turn: one in flight per half. */
typedef enum {
    RDOF_SPLIT_DONE_NONE = 0,
    RDOF_SPLIT_DONE_VERSION,
    RDOF_SPLIT_DONE_INFO,
    RDOF_SPLIT_DONE_ROLE,
    RDOF_SPLIT_DONE_TIME,
    RDOF_SPLIT_DONE_GET_EPOCH,
    RDOF_SPLIT_DONE_SET_EPOCH,
    RDOF_SPLIT_DONE_JOIN,    /* SET_FREQ / SET_CONFIG / SET_MODE to both */
    RDOF_SPLIT_DONE_RESTORE, /* SET_ROLE 1 again after a CW ID */
    RDOF_SPLIT_DONE_CWID,
    RDOF_SPLIT_DONE_STATUS,
    RDOF_SPLIT_DONE_ALIGN,
    RDOF_SPLIT_DONE_SNAP, /* the TX half's 0xD6 STATUS while a snap is under way */
} rdof_split_done_t;

#define RDOF_SPLIT_ALIGN_EVERY_S 10.0
#define RDOF_SPLIT_ALIGN_LIMIT_US 5000.0
#define RDOF_SPLIT_ALIGN_AGREE_US 2500.0
#define RDOF_SPLIT_ALIGN_JUMP_QUIET_US 100
#define RDOF_SPLIT_SNAP_POLL_S 0.03 /* STATUS this often while a snap is under way (RadioFoundry: 3-6 slots; a boot's first, redone at once, up to ~12) */
#define RDOF_SPLIT_SNAP_POLLS 20    /*   at most this many times */
typedef enum {
    RDOF_ALIGN_OFF,
    RDOF_ALIGN_UNSUPPORTED,
    RDOF_ALIGN_WAITING,
    RDOF_ALIGN_SNAPPING, /* unkeyed: its slot clock restarting on the grid; STATUS 46 says when it's done */
    RDOF_ALIGN_HELD, /* keyed and a millisecond (or a parity) off: it keeps its clock until unkeyed */
    RDOF_ALIGN_SLEWING,
    RDOF_ALIGN_LOCKED
} rdof_align_state_t;

#define RDOF_SPLIT_REQ_FRAME 64
#define RDOF_SPLIT_QUEUE 16
#define RDOF_SPLIT_JOINS 8
#define RDOF_SPLIT_WAITERS 8
#define RDOF_SPLIT_HALF_OUT 4096
#define RDOF_SPLIT_UP_OUT 4096

typedef struct {
    int key; /* the command, or 0xD600 | sub */
    uint8_t frame[RDOF_SPLIT_REQ_FRAME];
    size_t len;
    rdof_split_done_t done;
    int arg;
    double sent_at;
} rdof_split_req_t;

typedef struct {
    rdof_mmdvm_reader_t reader;
    rdof_split_req_t queue[RDOF_SPLIT_QUEUE];
    int qhead, qcount;
    bool has_inflight;
    rdof_split_req_t inflight;
    char description[128];
    int protocol;
    int version;  /* INFO's extension version */
    uint16_t caps; /* RDOF_OPS_*: INFO's capabilities */
    uint8_t info[24]; /* its INFO */
    size_t info_len;
    rdof_clock_sync_t sync;
    double last_reply;
    int misses;
    bool timing;
    uint8_t out[RDOF_SPLIT_HALF_OUT];
    size_t out_len;
    uint32_t overflow; /* bytes dropped: out full */
} rdof_split_half_t;

typedef struct {
    uint8_t command, left, err;
    bool has_err, timeout, used;
    uint8_t mode; /* SET_MODE's mode */
} rdof_split_join_t;

typedef struct {
    int power_cap, prebuffer, lease_s, cooldown_s;
    double max_key_s;
} rdof_split_params_t;

typedef struct rdof_split {
    rdof_split_half_t halves[2];
    rdof_mmdvm_reader_t up_reader;
    uint8_t up[RDOF_SPLIT_UP_OUT];
    size_t up_len;
    uint8_t role_params[5];
    bool ready;
    char failed[256];
    int mode;
    bool version_asked;
    bool epoch_ok;
    bool has_epoch_due;
    double epoch_due;
    bool epoch_busy;
    uint8_t waiters[RDOF_SPLIT_WAITERS]; /* commands whose ACK waits for the epoch */
    int nwaiters;
    bool has_first_epoch;
    double first_epoch;
    bool tx_role;
    bool open[3];
    double data_until[3];
    int at_sent; /* -1: none yet */
    double at_at;
    double next_time;
    uint32_t dropped;
    rdof_split_join_t joins[RDOF_SPLIT_JOINS];
    rdof_epoch_relay_t relay;
    /* UTC slot timing (rdof_split_set_utc) */
    bool utc;                  /* the host's clock is on the grid */
    double utc_minus_host_us, utc_bound_us;
    int offset_us;
    bool transmit;             /* false: the gate says stop */
    bool keyed_up;             /* upstream's DMR_START */
    rdof_epoch_relay_t align_relay;
    bool has_align_due, align_busy, has_align_first, has_align_err;
    double align_due, align_first;
    rdof_align_state_t align_state;
    int32_t align_err_us; /* the last ALIGN's phase_us */
    int align_flags;      /* and flags */
    int64_t align_n;
    bool align_jump, has_snap_due;
    double snap_due;
    int snap_polls;
    int stream_mode;      /* the TX half's (RDOF_OPS_STREAM_FIFO: upstream's DMR_DATA) */
    /* optional: the log */
    void (*log)(void *ctx, const char *msg);
    void *log_ctx;
} rdof_split_t;

void rdof_split_params_default(rdof_split_params_t *p);
/* Starts bring-up: GET_VERSION to both halves (take their bytes). */
void rdof_split_init(rdof_split_t *s, double now, const rdof_split_params_t *p);
void rdof_split_upstream(rdof_split_t *s, const uint8_t *data, size_t n, double now);
void rdof_split_from_half(rdof_split_t *s, int which, const uint8_t *data, size_t n, double now);
void rdof_split_tick(rdof_split_t *s, double now);
/* What to write to a half (RDOF_SPLIT_TX / RDOF_SPLIT_RX) or upstream (-1); returns the length (up to cap; the rest
 * stays). */
size_t rdof_split_take(rdof_split_t *s, int which, uint8_t *buf, size_t cap);
/* The host's clock against UTC (UTC µs = host µs + utc_minus_host_us, wrong by at most bound_us); utc false: off the
 * grid, no aligning (the TX half keeps its slot clock). transmit false: the gate says stop (unkeyed, data refused). */
void rdof_split_set_utc(rdof_split_t *s, bool utc, double utc_minus_host_us, double bound_us, int offset_us, bool transmit,
                      double now);
const char *rdof_align_state_name(rdof_align_state_t a);
/* The inbound slots busy now (bit 0 TS1, bit 1 TS2), as SET_AT says. */
int rdof_split_busy_mask(const rdof_split_t *s, double now);

#endif /* RDOF_FEATURE_REPEATER */

#ifdef __cplusplus
}
#endif

#endif
