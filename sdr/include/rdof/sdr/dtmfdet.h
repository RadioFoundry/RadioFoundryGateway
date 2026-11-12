// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * DTMF digits in a voice-band signal (an FM receiver's audio, Hz of deviation), as they stream. The bench analyzer's
 * method: 40 ms Hann windows every 5 ms, the eight ITU-T Q.23 tones by Goertzel; a window holds a digit when its row
 * and column tones stand 10 dB over the other six and hold 60 % of the window's power. A run of one digit's windows
 * 30 ms or longer is a digit: its span (the windows' centers, half a step out each side), its peak deviation and its
 * twist (the column tone over the row's, dB), each the median over the run.
 *
 * About 8 multiplies a sample per tone at 16 kS/s, 1 M a second; portable C11, one thread.
 */
#ifndef RDOF_SDR_DTMFDET_H
#define RDOF_SDR_DTMFDET_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char digit;           /* 0-9, A-D, *, # */
    double t0, t1;        /* s from the first sample */
    float dev_hz;         /* peak deviation (sqrt 2 times the window's rms) */
    float twist_db;       /* the high tone over the low */
} rdof_dtmf_run_t;

typedef struct rdof_dtmfdet rdof_dtmfdet_t;

/* fs: the signal's rate (8-48 kS/s). NULL: no memory, or a rate it can't take. */
rdof_dtmfdet_t *rdof_dtmfdet_new(double fs, void *(*alloc)(size_t), void (*dealloc)(void *));
void rdof_dtmfdet_free(rdof_dtmfdet_t *d);
void rdof_dtmfdet_feed(rdof_dtmfdet_t *d, const float *x, int n);
/* The run going on ends (the stream's end). */
void rdof_dtmfdet_finish(rdof_dtmfdet_t *d);
/* The next finished digit (false: none yet). */
bool rdof_dtmfdet_take(rdof_dtmfdet_t *d, rdof_dtmf_run_t *out);

#endif
