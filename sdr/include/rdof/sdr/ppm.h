// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The tuner's crystal error, learned from the AFC. A tuner whose crystal is fast puts every signal low by
 * f * ppm * 1e-6 (less what the RTL's correction already takes out), and a channel's AFC correction follows
 * where its signal is: so -correction / f is the tuner's error left, in the RTL's sign (positive: crystal fast,
 * fixed by rtl_set_freq_correction with a positive ppm), plus the transmitter's own, negated (a radio may be
 * 1.5 ppm off; a repeater rarely is).
 * Where two or more channels with recent bursts agree, their median is the tuner's error; held steady long
 * enough, it is confident. The caller moves the RTL's correction by the whole ppm (rdof_ppm_applied) once at least
 * RDOF_PPM_MOVE is left and keeps
 * the learned value (rdof_ppm_learned) where the next boot finds it. rdof_ppm_should_save is one rule for when: first
 * after RDOF_PPM_FIRST_SAVE_S of confidence, then at most every RDOF_PPM_SAVE_EVERY_S, and only when it moved
 * RDOF_PPM_SAVE_MOVE. The board doesn't use it: it keeps a temperature table instead (sdr_task.c, cal_learn), and
 * calls rdof_ppm_saved when it stores. Portable C, no clock of its own: the caller passes the time.
 */
#ifndef RDOF_SDR_PPM_H
#define RDOF_SDR_PPM_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_PPM_FRESH_S 30.0         /* a channel's AFC counts for this long after its last burst */
#define RDOF_PPM_AGREE 1.0            /* channels within this of their median agree */
#define RDOF_PPM_MIN_CHANNELS 2
#define RDOF_PPM_STEADY 0.5           /* an estimate within this of the running one keeps the confidence */
#define RDOF_PPM_APPLY_S 60.0         /* confident this long: move the RTL by the whole ppm... */
/* ...once what's left is this much: past 0.5 so a crystal near a half doesn't flap between two settings (a move ends
 * an LR2021's session), under 1 so the chip leaves at most this much (a capture's samples keep it; the channels and
 * the transmitter take the rest) */
#define RDOF_PPM_MOVE 0.75
#define RDOF_PPM_FIRST_SAVE_S 900.0   /* 15 minutes */
#define RDOF_PPM_SAVE_EVERY_S 21600.0 /* 6 hours */
#define RDOF_PPM_SAVE_MOVE 0.5

typedef struct {
    double chip;        /* whole ppm the RTL is set to */
    double residual;    /* running estimate of the error left (ppm, the AFC's share), NAN: none */
    double steady_s;    /* how long the estimate has held */
    double last_t;      /* the last update's time, NAN: none */
    int channels;       /* how many agreed in the last update (0 after rdof_ppm_measured) */
    bool measured;      /* the last update was rdof_ppm_measured's */
    bool has_saved;
    double saved;       /* the value last saved (or loaded) */
    double saved_t;     /* when, NAN: loaded at boot, not saved in this run */
} rdof_ppm_t;

/* What's left of a crystal error e (ppm, positive: fast) once a correction of c ppm has taken it out: a chip told
 * its crystal runs c fast programs f / (1 + c) for f, which the crystal makes f (1 + e) / (1 + c). */
static inline double rdof_ppm_left(double e, double c) { return ((1.0 + e * 1e-6) / (1.0 + c * 1e-6) - 1.0) * 1e6; }

/* Where a signal truly at f shows in a stream tuned to center whose LO runs lo_ppm fast and whose sample clock
 * fs_ppm fast (what the chip's corrections leave, rdof_ppm_left): Hz from center, as the stream's samples count
 * them. A channel tuned there hears a carrier truly at f on frequency. The two differ where the chip corrects its
 * LO but not its sample rate (the LR2021: a 300 kHz offset 32 ppm off is 9.6 Hz). */
static inline double rdof_ppm_seen_hz(double f, double center, double lo_ppm, double fs_ppm)
{
    return (f - center * (1.0 + lo_ppm * 1e-6)) / (1.0 + fs_ppm * 1e-6);
}

/* chip: the whole ppm the RTL starts at; saved: the learned value kept from before (NAN: none). */
void rdof_ppm_init(rdof_ppm_t *p, double chip, double saved);
/* One look at the channels at time t (seconds): corr_hz[i] each one's AFC correction, freq_hz[i] its frequency,
 * age_s[i] seconds since its last burst (negative or huge: none). Returns the whole ppm to move the RTL by
 * now (0: nothing to do); after moving it (which raises every channel's correction by f * step * 1e-6),
 * call rdof_ppm_applied. */
int rdof_ppm_update(rdof_ppm_t *p, const double *corr_hz, const double *freq_hz, const double *age_s, int n, double t);
/* Instead of rdof_ppm_update, when the crystal's error is measured directly (rdof/sdr/rate.h: the sample clock timed
 * against the board's) well enough to trust: ppm is that error, whole, as rdof_ppm_learned gives it. The same
 * steadiness, moves and saves follow. Returns the whole ppm to move the RTL by, as rdof_ppm_update. */
int rdof_ppm_measured(rdof_ppm_t *p, double ppm, double t);
/* The RTL was moved by step ppm (and every channel's correction by +f * step * 1e-6). */
void rdof_ppm_applied(rdof_ppm_t *p, int step);
/* The learned value: chip + residual (NAN while there's no estimate). */
double rdof_ppm_learned(const rdof_ppm_t *p);
/* Whether to save rdof_ppm_learned now (then call rdof_ppm_saved). */
bool rdof_ppm_should_save(const rdof_ppm_t *p, double t);
void rdof_ppm_saved(rdof_ppm_t *p, double t);

#ifdef __cplusplus
}
#endif

#endif
