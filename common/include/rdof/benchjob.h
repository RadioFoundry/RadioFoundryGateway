// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The bench's jobs (docs/spec/bench-api.md section 14.3): what a bench tool's command line asks for (bench_tx.py's
 * voice, csbk, data and listen), as bursts to transmit and a listener for what comes back, in the tool's own output
 * format. The line protocol engine (rdof/benchline.h) runs them; this file holds no state of its own beyond the
 * structures here. Portable C.
 *
 * What goes on the air is DMO simplex: one burst every 60 ms, as an MMDVM modem sends it. The bursts are built from
 * ETSI TS 102 361-1 / -2: voice LC header and terminator, voice superframes with the embedded LC (the call's, or a
 * talker alias or GPS Info LC in odd superframes), preamble CSBKs, data headers and rate 1/2 blocks (unconfirmed,
 * or confirmed with the DBSN and CRC-9), defined short data.
 */
#ifndef RDOF_BENCHJOB_H
#define RDOF_BENCHJOB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/benchline.h"
#include "rdof/jsonp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_BJ_TEXT 160        /* --tms, --etsi, --ip-text, --talker-alias */
#define RDOF_BJ_UDP 600         /* --udp-payload octets */
#define RDOF_BJ_CALLS 16        /* calls a listener keeps for its summary */

enum { RDOF_BJ_VOICE = 1, RDOF_BJ_CSBK, RDOF_BJ_DATA, RDOF_BJ_LISTEN, RDOF_BJ_REPEATER };

typedef struct {
    int cmd;
    double freq_mhz;
    int cc, power, burst_cc;
    double listen_after;
    char ambe_out[64];          /* "" none */
    uint32_t src, dst, src2;
    int family;                 /* rdof_family_t */
    /* voice */
    bool priv, pi, emergency;
    int superframes, headers, terminators, late, calls, gap_ms, service_options;
    const uint8_t *ambe;        /* 9-byte AMBE+2 frames with FEC (the request's ambe_hex, decoded; kept by the caller) */
    size_t ambe_len;
    char talker_alias[RDOF_BJ_TEXT];
    bool gps;
    double lat, lon;
    int gps_err;
    /* csbk */
    int count, opcode, fid, octet3;
    /* data */
    bool confirmed, tms_no_ack, etsi_no_response, group;
    char tms[RDOF_BJ_TEXT], etsi[RDOF_BJ_TEXT], ip_text[RDOF_BJ_TEXT];
    bool has_tms, has_etsi, has_ip_text;
    int octets, tms_seq, udp_port, udp_src_port, udp_octets, preambles, repeat, repeat_gap_ms;
    uint8_t udp_payload[RDOF_BJ_UDP];
    int udp_payload_len;        /* -1: none (filler) */
    /* listen */
    double seconds;
    bool json;
    /* bench_repeater (cmd RDOF_BJ_REPEATER; freq_mhz is tx_mhz) */
    double rx_mhz, tx_mhz, hang, call_hang, beacon, beacon_len, beacon_phase;
    unsigned slots;             /* bit 1: slot 1, bit 2: slot 2 */
} rdof_bj_job_t;

/* The job object of a run request (bench-api.md 14.3): false with error set when it can't be done here. ambe_buf
 * (cap bytes) takes the AMBE frames. */
bool rdof_bj_parse(const rdof_jdoc_t *d, int obj, rdof_bj_job_t *j, uint8_t *ambe_buf, size_t cap, char *error,
                   size_t ecap);

/* A burst to send: its MMDVM DMR_DATA control byte (voice sync 0x20, B-F 1-5, data 0x40 | type) and the burst. */
typedef struct {
    uint8_t control;
    uint8_t burst[33];
} rdof_bj_burst_t;

/* The key-ups a transmitting job has (a voice call each, a data packet each with --repeat-gap-ms, else one), and
 * the gap before each but the first (seconds). 0 for listen. */
int rdof_bj_parts(const rdof_bj_job_t *j);
double rdof_bj_gap(const rdof_bj_job_t *j);
/* Key-up k's bursts into out (up to cap): how many, -1 when they don't fit or can't be built (error set). note: a
 * line for the tool's log before it ("" none), done: one after. */
int rdof_bj_build(const rdof_bj_job_t *j, int k, rdof_bj_burst_t *out, int cap, char *note, char *done, size_t ncap,
                  char *error, size_t ecap);
/* Bursts key-up k has at most (for the buffer). */
int rdof_bj_max_bursts(const rdof_bj_job_t *j);

/* The listener: bursts heard on the job's frequency, as bench_tx.py listen's records and call summaries. */
typedef struct {
    bool active, ended, ta_hdr_seen;
    uint32_t src, dst;
    int flco;
    bool has_src;
    const char *start;          /* "header", "late entry" */
    double t0, t_last;
    int headers, voice, terminators, emb_lc, pi, after_end;
    const char *ended_by;       /* "terminator", "lost", NULL */
    unsigned cc_mask;           /* color codes heard on voice B-F */
    char lc_type[24];
    int lc_types[8];            /* by type index */
    int ta_format, ta_length, ta_have;  /* the talker alias: header format, length; blocks seen (bit 0 header) */
    uint8_t ta_bits[49 + 3 * 56];
    bool gps;
    double lat, lon;
    int gps_err;
    long frames, silence, fec_bits;
    uint32_t crc;               /* CRC-32 of the frames, running */
    uint8_t *ambe;              /* the frames, with an ambe_out prefix (malloc'd) */
    size_t ambe_n, ambe_cap;
} rdof_bj_call_t;

typedef struct {
    bool json, keep_ambe;
    rdof_bj_call_t calls[RDOF_BJ_CALLS];
    int ncalls;
    int cur;                    /* calls[cur], -1 none */
    uint8_t frag[4][32];        /* embedded signalling of B-E */
    unsigned frag_have;
    char ta_fmt_hint;
} rdof_bj_listener_t;

/* line: a record (JSON text, no newline). */
typedef void (*rdof_bj_emit_fn)(void *ctx, const char *line);

void rdof_bj_listen_init(rdof_bj_listener_t *l, bool json, bool keep_ambe);
void rdof_bj_listen_free(rdof_bj_listener_t *l);
/* A burst heard on the frequency: its record out. */
void rdof_bj_listen_burst(rdof_bj_listener_t *l, const rdof_bl_burst_t *b, rdof_bj_emit_fn emit, void *ctx);
/* The calls' summaries out, one record each; with keep_ambe, file(name, bytes) for PREFIX_<n>.ambe and .amb. */
void rdof_bj_listen_summary(rdof_bj_listener_t *l, const char *prefix, rdof_bj_emit_fn emit,
                            void (*file)(void *ctx, const char *name, const uint8_t *b, size_t n), void *ctx);

/* A dmr-bursts record's decode (bench-api.md 14.2): ",\"pi\":..,\"lcss\":..,\"fec\":{..},\"lc\":.." for burst b into out
 * (cap bytes); its length. emb carries the embedded LC's fragments from burst to burst (zero it at the start). */
typedef struct {
    uint8_t frag[4][32];
    unsigned have;
} rdof_bj_emb_t;

size_t rdof_bj_burst_decode(rdof_bj_emb_t *emb, const rdof_bl_burst_t *b, char *out, size_t cap);

/* bench_repeater's records (each {"t": UTC, ...}): what the repeater heard on its input, lost, keyed, and at the end
 * a summary per slot. */
typedef struct {
    long wakeups, voice, dt[16];
} rdof_bj_rpt_slot_t;

typedef struct {
    rdof_bj_rpt_slot_t s[3];    /* 1 and 2 */
} rdof_bj_rpt_t;

/* A burst heard on the input (its MMDVM control byte): an "rx" record, and a "wakeup" before it for a BS_Dwn_Act. */
void rdof_bj_rpt_heard(rdof_bj_rpt_t *r, double t, int slot, uint8_t control, const uint8_t burst[33], int rssi,
                       rdof_bj_emit_fn emit, void *ctx);
/* Any other record: body is its fields after "t" (",\"key\":true"). */
void rdof_bj_rpt_record(double t, const char *body, rdof_bj_emit_fn emit, void *ctx);
void rdof_bj_rpt_summary(const rdof_bj_rpt_t *r, double t, rdof_bj_emit_fn emit, void *ctx);
/* The BS_Dwn_Act an idle beacon hands the repeater (as a radio's, on cc): its control byte. */
uint8_t rdof_bj_rpt_wake_burst(int cc, uint8_t out[33]);

#ifdef __cplusplus
}
#endif

#endif
