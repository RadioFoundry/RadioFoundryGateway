// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * tinysctp: a compact SCTP (RFC 9260) for one-to-one associations, with
 * partial reliability (PR-SCTP, RFC 3758, timed lifetime), meant to run over
 * UDP encapsulation (RFC 6951) on small boards.
 *
 * This is the portable core: plain C11, no sockets, no threads, no OS. The
 * caller hands it received packets, calls tsctp_poll() at the deadline it
 * returns, and gets packets to send through `output`, messages through
 * `on_message` and association changes through `on_event`. The time is always
 * the caller's, in milliseconds from any monotonic clock.
 *
 * One tsctp_t is one association, either end: tsctp_connect() starts one
 * (INIT), tsctp_listen() waits for one (answers INIT with a state cookie).
 *
 * Not reentrant: one caller at a time. tsctp_send() may be called from inside
 * on_message / on_event (it queues, and the packet goes out when the callback
 * returns); tsctp_free() may not.
 */
#ifndef TINYSCTP_H
#define TINYSCTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tsctp tsctp_t;

typedef enum {
    TSCTP_CLOSED = 0,
    TSCTP_LISTEN,
    TSCTP_COOKIE_WAIT,
    TSCTP_COOKIE_ECHOED,
    TSCTP_ESTABLISHED,
    TSCTP_SHUTDOWN_PENDING,
    TSCTP_SHUTDOWN_SENT,
    TSCTP_SHUTDOWN_RECEIVED,
    TSCTP_SHUTDOWN_ACK_SENT,
} tsctp_state_t;

typedef enum {
    TSCTP_EV_UP = 1,  /* established */
    TSCTP_EV_DOWN,    /* closed; `reason` says why */
    TSCTP_EV_RESTART, /* the peer restarted the association (RFC 9260 5.2.4 A); queued data was dropped */
} tsctp_event_t;

typedef enum {
    TSCTP_R_NONE = 0,
    TSCTP_R_SHUTDOWN,      /* a graceful close, either end */
    TSCTP_R_ABORTED,       /* the peer sent ABORT */
    TSCTP_R_INIT_TIMEOUT,  /* no answer to INIT or COOKIE ECHO */
    TSCTP_R_PEER_LOST,     /* too many retransmissions or unanswered heartbeats */
    TSCTP_R_LOCAL_ABORT,   /* tsctp_abort() */
} tsctp_reason_t;

/* Errors from the calls that return int. */
enum {
    TSCTP_OK = 0,
    TSCTP_ERR_STATE = -1,  /* not established (or already connecting) */
    TSCTP_ERR_NOBUFS = -2, /* the send buffer is full */
    TSCTP_ERR_INVAL = -3,  /* bad stream, empty or oversized message */
    TSCTP_ERR_NOMEM = -4,
};

/* A received message. `data` is valid during the callback only. */
typedef struct {
    const uint8_t *data;
    size_t len;
    uint16_t stream;
    uint32_t ppid; /* host byte order */
    bool unordered;
} tsctp_msg_t;

/* How to send a message. Zeroed: stream 0, PPID 0, ordered, fully reliable. */
typedef struct {
    uint16_t stream;
    uint32_t ppid;        /* host byte order */
    bool unordered;
    uint32_t lifetime_ms; /* PR-SCTP: abandon it this long after tsctp_send() if not acknowledged; 0: reliable */
} tsctp_sendinfo_t;

typedef struct {
    /* the SCTP common header's ports; local_port 0: a random one from 49152-65535 */
    uint16_t local_port, peer_port;
    /* streams asked for (each way); the association gets min(ours, theirs) */
    uint16_t streams_out, streams_in;
    uint16_t mtu;            /* largest SCTP packet (without IP / UDP headers) */
    uint32_t rto_initial_ms, rto_min_ms, rto_max_ms;
    uint32_t hb_interval_ms; /* heartbeat on an idle path every this + RTO (with jitter); 0: none */
    uint8_t max_retrans;     /* retransmissions / unanswered heartbeats in a row before the peer is lost */
    uint8_t max_init_retrans;
    uint32_t sack_delay_ms;  /* delayed SACK; every second packet is acknowledged at once */
    uint32_t sndbuf;         /* bytes queued and in flight */
    uint32_t rcvbuf;         /* receive window (bytes held for reassembly or ordering) */
    uint32_t max_message;    /* largest message accepted from the peer */
    uint32_t cookie_life_ms;
    /* connect only: reconnect after the association goes down, with backoff from min to max; 0: don't */
    uint32_t reconnect_min_ms, reconnect_max_ms;

    /* platform */
    void (*output)(void *ctx, const uint8_t *pkt, size_t len); /* send one SCTP packet */
    void *output_ctx;
    uint32_t (*random)(void *ctx);                             /* unpredictable 32-bit values */
    void *random_ctx;
    void *(*alloc)(size_t);                                    /* NULL: malloc / free */
    void (*free)(void *);

    /* application */
    void (*on_message)(void *user, const tsctp_msg_t *msg);
    void (*on_event)(void *user, tsctp_event_t ev, tsctp_reason_t reason);
    void *user;
} tsctp_config_t;

typedef struct {
    uint32_t pkts_in, pkts_out, bad_checksum, bad_vtag, ootb;
    uint32_t msgs_sent, msgs_received, bytes_sent, bytes_received;
    uint32_t chunks_sent, chunks_received, dup_chunks, chunks_dropped_window;
    uint32_t retransmits_t3, retransmits_fast, t3_expiries;
    uint32_t abandoned_msgs, abandoned_unsent;   /* PR-SCTP: sent but given up; dropped before sending */
    uint32_t fwd_tsn_sent, fwd_tsn_received, skipped_msgs; /* skipped: messages the peer abandoned */
    uint32_t sacks_sent, sacks_received;
    uint32_t hb_sent, hb_acked, hb_received;
    uint32_t established, restarts, failures, aborts_received, send_rejected;
    /* now */
    uint32_t srtt_ms, rttvar_ms, rto_ms, cwnd, ssthresh, flight, peer_rwnd;
    uint32_t queued_bytes, queued_msgs, outstanding_chunks;
    uint32_t error_count, local_vtag, peer_vtag;
    uint16_t streams_out, streams_in, local_port, peer_port;
    bool peer_prsctp;
} tsctp_stats_t;

/* RFC 9260 defaults: RTO 1 / 1 / 60 s, heartbeat 30 s, 10 retransmissions, 8 INIT retransmissions,
   SACK delay 200 ms, MTU 1200, 4 streams each way, 64 KB buffers, no reconnect. */
void tsctp_config_defaults(tsctp_config_t *cfg);

/* NULL when out of memory or `output` / `random` is missing. */
tsctp_t *tsctp_new(const tsctp_config_t *cfg, uint64_t now_ms);
void tsctp_free(tsctp_t *s);

/* Start an association (INIT); TSCTP_EV_UP follows. */
int tsctp_connect(tsctp_t *s, uint64_t now_ms);
/* Wait for one (from CLOSED). */
int tsctp_listen(tsctp_t *s);
/* Graceful close once what's queued is acknowledged. */
int tsctp_shutdown(tsctp_t *s, uint64_t now_ms);
/* ABORT and close now; nothing more is sent or delivered. Reconnect is off until tsctp_connect(). */
void tsctp_abort(tsctp_t *s, uint64_t now_ms);

/* Queue a message (fragmented if it's over the MTU) and send what can go now. */
int tsctp_send(tsctp_t *s, const void *data, size_t len, const tsctp_sendinfo_t *info, uint64_t now_ms);

/* One received SCTP packet (the UDP payload when encapsulated). */
void tsctp_input(tsctp_t *s, const uint8_t *pkt, size_t len, uint64_t now_ms);
/* Run the timers. Returns when to call again (absolute ms; UINT64_MAX: nothing pending). */
uint64_t tsctp_poll(tsctp_t *s, uint64_t now_ms);

tsctp_state_t tsctp_state(const tsctp_t *s);
const char *tsctp_state_name(tsctp_state_t st);
const char *tsctp_reason_name(tsctp_reason_t r);
void tsctp_get_stats(const tsctp_t *s, tsctp_stats_t *out);
/* The state and stats as one JSON object (for a status page or a log). Returns its length. */
size_t tsctp_status_json(const tsctp_t *s, char *buf, size_t cap);

/* CRC32c (Castagnoli), as SCTP uses it; tsctp_crc32c(0, ...) starts one. */
uint32_t tsctp_crc32c(uint32_t crc, const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif
