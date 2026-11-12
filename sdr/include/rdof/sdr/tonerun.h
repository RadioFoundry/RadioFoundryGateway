// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Timed steady tones in a voice-band signal (an FM receiver's audio, Hz of deviation), as they stream: tone bursts,
 * roger beeps, CW identifiers. The audio is averaged down to 8-16 kS/s, then 20 ms Hann windows every 5 ms, by FFT: a
 * window holds a tone when one peak in 300-3000 Hz has half the band's energy. Its frequency comes from a parabola
 * through the peak's log power.
 *
 * A window run of one pitch (within 40 Hz) is a piece. Its edges are where the peak's power crosses a quarter of the
 * piece's median: a Hann window centered on a tone's edge sees a quarter of the power, so the edge doesn't move with
 * the signal's level. Pieces of one pitch less than 50 ms apart are one run (a click on a weak signal doesn't split
 * it; CW's gaps of 60 ms or more do). A piece of another pitch shorter than 15 ms is ignored; a longer one ends the
 * run. A run of 40 ms or more is reported.
 *
 * Float only (an ESP32's FPU has no double): about one 256-point FFT each 5 ms. Portable C11, one thread.
 */
#ifndef RDOF_SDR_TONERUN_H
#define RDOF_SDR_TONERUN_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    double t0, t1; /* s from the first sample */
    float hz;      /* the windows' median, averaged over the run's pieces */
    float dev_hz;  /* the tone's peak amplitude, the same way */
} rdof_tone_run_t;

typedef struct rdof_tonerun rdof_tonerun_t;

/* fs: the signal's rate (8-48 kS/s). NULL: no memory, or a rate it can't take. */
rdof_tonerun_t *rdof_tonerun_new(double fs, void *(*alloc)(size_t), void (*dealloc)(void *));
void rdof_tonerun_free(rdof_tonerun_t *d);
void rdof_tonerun_feed(rdof_tonerun_t *d, const float *x, int n);
/* The run going on ends (the stream's end). */
void rdof_tonerun_finish(rdof_tonerun_t *d);
/* The next finished run (false: none yet). */
bool rdof_tonerun_take(rdof_tonerun_t *d, rdof_tone_run_t *out);

#endif
