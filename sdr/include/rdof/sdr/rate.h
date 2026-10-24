// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The front end's sample clock, timed against the board's. An RTL-SDR's samples and its tuner's LO come from one
 * crystal, so the rate the samples really arrive at says how far off the crystal is: no signal needed. The board's
 * clock is disciplined (netclock: NTP, or a GNSS PPS), so its own error is known and taken out by the caller.
 *
 * The driver stamps each block of samples as it arrives (the local clock, and the samples the device has sent in
 * all, dropped ones too). How late the stream runs, x = t - n / nominal, falls by e us a second when the samples
 * come e ppm fast. Arrival is never early, only late by a varying latency (USB frames, the task's turn), so the
 * least late in each RDOF_RATE_BIN_S tracks the stream itself; those minima are fitted with a line over up to
 * RDOF_RATE_POINTS bins. Samples the device lost without saying so (the RTL's FIFO overflowing) make x jump: a bin
 * that far off the line starts the fit over, keeping the last estimate until the new one has enough bins.
 *
 * Two halves, so the driver's side stays cheap: rdof_rate_bin_add per block (the driver's task, O(1)), which hands
 * back a bin's minimum when one closes; rdof_rate_point with those (the caller's own task), which fits. Portable C.
 */
#ifndef RDOF_SDR_RATE_H
#define RDOF_SDR_RATE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_RATE_BIN_S 10       /* one point per bin: its least late block */
#define RDOF_RATE_POINTS 180     /* bins in the fit: 30 minutes */
#define RDOF_RATE_MIN_POINTS 6   /* an estimate from a minute of bins */
#define RDOF_RATE_STEP_US 200.0  /* a bin this far off the line: samples lost (or gained); start the fit over */
#define RDOF_RATE_MAX_PPM 500.0  /* anything further off is a wrong nominal rate, not a crystal */

/* The driver's half: bins of blocks. */
typedef struct {
    double nominal;      /* S/s the device should send */
    bool started;
    int64_t t0;          /* the origin: local us and samples sent at the first block */
    uint64_t n0;
    int64_t bin_end;     /* local us the bin closes at */
    bool any;            /* a block in this bin */
    double min_x, min_t; /* its least late block: x (us) and when (s since the origin) */
} rdof_rate_bin_t;

void rdof_rate_bin_init(rdof_rate_bin_t *b, double nominal);
/* A block arrived at local t_us, the device having sent n samples in all since its stream started. Returns true
 * when a bin closed, with its point (seconds since the origin, us late) for rdof_rate_point. */
bool rdof_rate_bin_add(rdof_rate_bin_t *b, int64_t t_us, uint64_t n, double *t_s, double *x_us);

/* The fit. */
typedef struct {
    double t[RDOF_RATE_POINTS], x[RDOF_RATE_POINTS];
    int head, count;
    bool valid;      /* ppm and se_ppm hold an estimate (perhaps from before the last step) */
    double ppm;      /* the samples come this fast against the local clock (ppm; positive: more than nominal) */
    double se_ppm;   /* its standard error, from the points' scatter about the line */
    double span_s;   /* the time the fit's points span */
    double rms_us;   /* their scatter about the line */
    uint32_t steps;  /* fits started over for a jump */
} rdof_rate_t;

void rdof_rate_init(rdof_rate_t *r);
/* A bin's point: start over if it's off the line, else add it and fit. */
void rdof_rate_point(rdof_rate_t *r, double t_s, double x_us);
/* The rate over only the last span_s of points (false: too few), to see whether it's still moving. */
bool rdof_rate_recent(const rdof_rate_t *r, double span_s, double *ppm);
/* Forget the points before t_s (a warm-up the line can't follow) and fit what's left. */
void rdof_rate_trim(rdof_rate_t *r, double t_s);

#ifdef __cplusplus
}
#endif

#endif
