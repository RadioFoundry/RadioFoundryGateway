// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#pragma once
// Source selection for the multi-server NTP client, chrony-style. Pure C so
// the host tests can run it.
//
// Each source gives an offset (UTC minus local clock, relative to any common
// base) and a root distance lambda (half its round trip + the server's own
// root delay/2 + root dispersion + its recent jitter): the true offset lies
// within offset +/- lambda if the source is honest. Marzullo's algorithm
// finds the largest set of intervals that overlap; sources outside it are
// falsetickers. The survivors are averaged, weighted by 1/lambda^2.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NC_SELECT_MAX 16  // candidates nc_select looks at

typedef struct {
    bool ok;          // has a usable sample this round
    double off_us;
    double lambda_us;
} nc_cand_t;

// Marks, as in `chronyc sources`.
#define NC_MARK_BEST     '*'  // survivor with the smallest lambda (the "system peer")
#define NC_MARK_COMBINED '+'  // survivor, averaged in
#define NC_MARK_FALSE    'x'  // falseticker: outside the agreeing majority
#define NC_MARK_NONE     '?'  // no usable sample

// Fills mark[i] for each candidate and *off_us with the combined offset.
// Returns the index of the best survivor, or -1 when there is no usable
// sample or no majority agrees (3+ sources and none overlap by majority).
int nc_select(const nc_cand_t *c, int n, char *mark, double *off_us);

#ifdef __cplusplus
}
#endif
