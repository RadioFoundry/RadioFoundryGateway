// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#pragma once
// The NTP side of netclock without the sockets: request and reply packets,
// per-server state (reach, delay filter, jitter, root distance) and one
// poll's selection and combination into a single reference. Pure C (no
// ESP-IDF): the platform does DNS, sends and receives, and stamps on its
// monotonic clock.

#include <stdbool.h>
#include <stdint.h>

#include "netclock_status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NC_NTP_HIST       8
#define NC_NTP_IBURST     4        // first polls 2 s apart (chrony's iburst); each server keeps its quickest
#define NC_NTP_MAX_DELAY  500000   // us: a round trip longer than this says nothing useful
#define NC_NTP_KOD_SKIP   16       // polls to skip a server that sent a kiss-o'-death

typedef struct {
    char name[40];
    uint8_t ip[4];
    bool resolved, dup;
    uint8_t reach, stratum;
    uint32_t refid;
    int misses, kod_skip;
    bool kod;
    uint64_t cookie;      // our transmit timestamp, echoed as the reply's origin
    int64_t t1;           // local us when the request went out, 0 = not sent
    bool got;             // reply this poll
    int64_t tm, u;        // last sample: local midpoint, UTC there
    double delay_us, root_delay_us, root_disp_us;
    int64_t last_rx_us;
    float h_off[NC_NTP_HIST];    // offsets vs the model, us
    int hn, hh;
    float h_delay[NC_NTP_HIST];  // round trips of recent usable replies, us
    int dn, dh;
    double min_delay_us;
    bool spike;                  // this poll's sample was a delay spike: not used
    double jitter_us, off_us, lambda_us;
    char mark;
} nc_src_t;

// NTP timestamp <-> UTC us (era rule of RFC 4330: seconds with the top bit clear are after 2036).
int64_t nc_ntp_to_us(uint64_t ts);
uint64_t nc_us_to_ntp(int64_t us);

// Split a server list (spaces, commas or semicolons) into out[], up to max.
// A name ending in "pool.ntp.org" expands to 0., 1. and 2.<name>, like
// chrony's `pool`. Returns the count.
int nc_parse_servers(const char *list, nc_src_t *out, int max);

// A client request (version 4) carrying `cookie` as its transmit timestamp.
void nc_ntp_request(uint8_t pkt[48], uint64_t cookie, uint32_t poll_s);

typedef enum {
    NC_REPLY_IGNORED = 0,  // not a reply to our current request
    NC_REPLY_KOD,          // kiss-o'-death: backing off
    NC_REPLY_UNUSABLE,     // answered (counts for reach), but unsynchronized or a bad round trip
    NC_REPLY_SAMPLE,       // a new sample
    NC_REPLY_KEPT,         // iburst: an earlier reply this burst was quicker; kept that one
} nc_reply_t;

// A reply that arrived at local time t4. `iburst`: during the first polls.
nc_reply_t nc_src_reply(nc_src_t *s, const uint8_t *p, int len, int64_t t4, bool iburst);

// After a poll's reply window: reach and misses.
void nc_src_poll_end(nc_src_t *s);

// The sample's offset from the clock model, false before the clock is set.
typedef bool (*nc_offset_fn)(void *ctx, int64_t t_us, int64_t utc_us, double *off_us);

// One poll: delay filter, jitter, root distance, selection (marks) and the
// combined reference: the clock read *utc_us at local *t_us. Returns the
// best server's index, or -1 (none usable: *had_sample false; or no
// majority: *had_sample true).
int nc_combine(nc_src_t *s, int n, nc_offset_fn off, void *ctx, int64_t *t_us, int64_t *utc_us, bool *had_sample);

// A server's row for the status.
void nc_src_view(const nc_src_t *s, int64_t now_us, netclock_src_view_t *v);

#ifdef __cplusplus
}
#endif
