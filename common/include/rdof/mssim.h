// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* A timed confirmed-data responder: it plays a radio that answers confirmed data packets (ACK, NACK, SACK, nothing,
   or ACK twice) N slots after a packet ends, for testing the far end's retry logic and turnaround.

   It works on the receiver's timeline: positions are sample counts at 48 kS/s, a burst's position being its first
   sample. The caller feeds it the bursts it hears (rdof_mssim_heard), calls rdof_mssim_poll each turn with the
   receiver's position and how far ahead the transmitter needs an answer, and places each answer burst where the
   send callback asks. One slot is 1440 samples (30 ms). */
#ifndef RDOF_MSSIM_H
#define RDOF_MSSIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/tracker.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_MSSIM_SLOT48 1440
#define RDOF_MSSIM_STEPS 8
#define RDOF_MSSIM_LOG 32

typedef enum {
    RDOF_MSSIM_ACK = 0,
    RDOF_MSSIM_NACK,
    RDOF_MSSIM_SACK,
    RDOF_MSSIM_NONE,
    RDOF_MSSIM_ACK_TWICE,
} rdof_mssim_kind_t;

/* Where a packet ends: its last block, its terminator (the last block when none came), or what the sender did on its
   previous packet (a terminator then: wait for one now). */
typedef enum { RDOF_MSSIM_END_BLOCK = 0, RDOF_MSSIM_END_TERMINATOR, RDOF_MSSIM_END_LEARN } rdof_mssim_end_t;

typedef struct {
    rdof_mssim_kind_t kind;
    int slots;         /* the answer starts this many slots after the end's start, 1.. */
    int nack_type;     /* NACK: class 1, this type (1 = CRC-32 failure) */
    int sack_type;     /* SACK: class 2, this type (0) */
    uint64_t resend;   /* SACK: blocks asked for again (bit n = block n) */
} rdof_mssim_step_t;

typedef struct {
    rdof_mssim_step_t step[RDOF_MSSIM_STEPS]; /* packet k gets step k (the last one after, or k % n with repeat) */
    int nsteps;
    bool repeat;
    rdof_mssim_end_t end;
    uint32_t to;   /* answer packets to this ID only (0: any individual packet) */
    uint32_t from; /* and from this ID only (0: any) */
    int count;     /* answer this many packets, then disarm (0: no limit) */
} rdof_mssim_profile_t;

/* Place burst at at48 (the receiver's timeline); *placed48 is where it went. false: it couldn't be placed. */
typedef bool (*rdof_mssim_send_fn)(void *ctx, int64_t at48, const uint8_t burst[33], int64_t *placed48);

typedef struct {
    uint32_t src, dst;
    int sap, ns, fsn, blocks, rate_dt; /* the packet: rate_dt the blocks' data type */
    int heard;                         /* blocks heard (the end's count at the send) */
    uint64_t dbsn;                     /* serial numbers heard with good CRC-9s */
    bool terminator;                   /* a terminator came after it */
    rdof_mssim_end_t end_used;         /* BLOCK or TERMINATOR */
    bool end_heard;                    /* end48 is a burst heard, not where one was expected */
    int64_t header48, end48;           /* the header's and the end burst's positions (end48 expected if unheard) */
    int step;
    rdof_mssim_kind_t kind;
    int slots;
    int64_t asked48, placed48, air48;  /* the answer's first burst: asked, placed, heard back (-1 not yet) */
    bool sent, failed;
    uint32_t seq; /* from 1 */
} rdof_mssim_entry_t;

typedef struct {
    rdof_mssim_profile_t prof;
    bool armed;
    rdof_mssim_send_fn send;
    void *ctx;
    int answered;

    /* the packet in flight: its log entry */
    bool active;
    int cur;
    rdof_family_t family;
    int cc;
    int64_t last48;       /* its last burst heard */
    bool learned_term;    /* the last packet from cur.src had a terminator */
    uint32_t learned_src;

    /* answers queued for poll: up to 3 bursts (SACK header + block, or ACK twice) */
    struct {
        int64_t at48;
        uint8_t burst[33];
        bool first; /* the answer's first burst: its placement goes in the log */
    } q[3];
    int nq, nsent; /* queued, sent of them */

    rdof_mssim_entry_t log[RDOF_MSSIM_LOG];
    int nlog, logpos; /* entries kept, the next slot */
    uint32_t seq;     /* entries ever logged */
} rdof_mssim_t;

void rdof_mssim_init(rdof_mssim_t *s, rdof_mssim_send_fn send, void *ctx);
/* Profile JSON: {"answers":[{"answer":"ack|nack|sack|none|ack2","slots":N,"nack_type":1,"sack_type":0,
   "resend":[1,2]}, ...], "repeat":false, "end":"block|terminator|learn", "to":ID, "from":ID, "count":N}.
   One step may be given flat ({"answer":"sack","slots":2,...}). false (err says why) on a bad profile. */
bool rdof_mssim_parse(const char *json, size_t len, rdof_mssim_profile_t *out, char *err, size_t errcap);
void rdof_mssim_arm(rdof_mssim_t *s, const rdof_mssim_profile_t *p);
void rdof_mssim_disarm(rdof_mssim_t *s);

/* A burst heard at pos48. */
void rdof_mssim_heard(rdof_mssim_t *s, const rdof_burst_t *b, int64_t pos48);
/* Our own answer heard back at pos48 (the caller's echo match): its on-air position goes in the log. */
void rdof_mssim_echo(rdof_mssim_t *s, int64_t pos48);
/* Send the queued bursts due by now48 + lead48. */
void rdof_mssim_poll(rdof_mssim_t *s, int64_t now48, int64_t lead48);

/* The answer bursts themselves (for tests and other users). Response header: class, type, status, blocks after. */
void rdof_mssim_response(uint32_t src, uint32_t dst, int sap, int cls, int type, int status, int blocks, int cc,
                         uint64_t sync48, uint8_t out[33]);
/* The SACK's block: the flag map (bit n set = block n received) and its CRC-32, rate 1/2. */
void rdof_mssim_sack_block(uint64_t received, int cc, uint64_t sync48, uint8_t out[33]);

/* The log as a JSON array, the newest last; entries after `since` (a seq) only, as many as fit. Returns the length
   (0 only when cap < 3). *next is the seq to pass next time (the last entry written). */
size_t rdof_mssim_log_json(const rdof_mssim_t *s, uint32_t since, char *out, size_t cap, uint32_t *next);

const char *rdof_mssim_kind_name(rdof_mssim_kind_t k);

#ifdef __cplusplus
}
#endif

#endif
