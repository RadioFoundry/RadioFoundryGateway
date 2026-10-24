// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#pragma once
// Local clock (a monotonic us counter, esp_timer on a board) -> UTC us,
// steered from occasional references (NTP, a browser's clock; PPS later).
// Pure C (no ESP-IDF) so the host tests can run it against a drifting,
// jittery clock.
//
// The model is a line, utc = u0 + (t - t0) * (1 + rate), plus an offset
// correction that is slewed in rather than stepped. The rate (crystal error)
// is a least-squares fit over the recent references of one source, so time
// between references extrapolates on the learned rate instead of drifting
// ~1 ms/min, and each reference only nudges the phase.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NC_HIST 40

typedef enum {
    NC_STEP = 0,  // clock (re)set to the reference: first one, new source, or a real jump
    NC_SLEW,      // reference accepted, offset being slewed in
    NC_OUTLIER,   // reference far off the model and not (yet) confirmed: ignored
} nc_result_t;

typedef struct {
    // References from the current source, ring by head.
    int64_t ref_t[NC_HIST], ref_u[NC_HIST];
    int n, head;
    int big_run;          // consecutive references > NC_STEP_US off the model

    bool valid;           // some reference has set the clock
    int64_t t0, u0;       // anchor: utc(t0) = u0 (before any slew)
    double rate;          // UTC us per local us, minus 1 (+20e-6 = local clock 20 ppm slow)
    int64_t slew_us;      // offset still to apply after t0, slewed in (see clock_model.c)

    // Last rate fit: offset (utc - local) = fit_off + rate * (t - fit_t).
    bool fit_ok;
    int64_t fit_t, fit_off;
    double fit_rate;       // the rate that fit gave (rate may since have been compensated, nc_model_set_rate)
    bool ref_in[NC_HIST];  // by ring index: reference kept by the last fit (not rejected)
    double rate_skew;      // standard error of the fitted rate (chrony's "skew")

    // Diagnostics.
    bool rate_learned;    // rate came from this run's references (else stored / zero)
    int rate_n;           // references in the last rate fit
    double rate_span_s;   // time they span
    int64_t last_err_us;  // last reference minus the model, before correction
    double resid_rms_us;  // rms residual of the last rate fit
    uint32_t outliers;
} nc_model_t;

// Start empty; `rate` is a prior (e.g. stored from the last run), 0 if none.
void nc_model_init(nc_model_t *m, double rate);

// A reference: the clock read utc_us at local time t_us. new_source = the
// reference comes from a different source than the previous ones (their
// history is dropped; the rate is kept).
nc_result_t nc_model_add(nc_model_t *m, int64_t t_us, int64_t utc_us, bool new_source);

// UTC us at local time t_us. Only meaningful once m->valid.
int64_t nc_model_utc(const nc_model_t *m, int64_t t_us);

// Change the rate from local t_us on (a temperature compensation): the
// model stays continuous there, and the slew still to go carries on.
void nc_model_set_rate(nc_model_t *m, int64_t t_us, double rate);

// Offset still to be slewed in at t_us (+ = the model is behind the references).
int64_t nc_model_slew_left(const nc_model_t *m, int64_t t_us);

// Residual of a reference (t_us, utc_us) about the last rate fit. False if no fit yet.
bool nc_model_fit_resid(const nc_model_t *m, int64_t t_us, int64_t utc_us, double *resid_us);

// Whether (t_us) is a reference the last fit used (false: rejected, too old, or no fit).
bool nc_model_in_fit(const nc_model_t *m, int64_t t_us);

#ifdef __cplusplus
}
#endif
