// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* UTC slot timing in rdofradio's full duplex run: rdofradio_utc.h. One thread (the run's loop). */
#include "rdofradio_utc.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#if defined(__linux__) || defined(__APPLE__)
#include <sys/timex.h>
#endif

#include "rdof/sdr/sdrmodem.h"

#define WINDOW_S 2.0
#define GATE_EVERY_S 1.0
#define FIT_S 120.0 /* the rate from the windows' anchors this far apart: 10 us of jitter is 0.1 ppm */

static struct {
    bool on;
    rdof_slot_timing_t timing;
    rdof_slot_gate_t gate;
    rdof_sample_clock_t sclock;
    rdofradio_utc_clock_t clock;
    double rate, gate_at;
    bool own, gate_done, told, own_now;
    uint32_t windows;
    bool has_ref;
    int64_t ref_i, ref_t; /* a window's anchor, for the rate */
} U;

static int64_t host_utc_ns(void *ctx)
{
    (void)ctx;
    struct timespec w;
    clock_gettime(CLOCK_REALTIME, &w);
    return (int64_t)w.tv_sec * 1000000000LL + w.tv_nsec;
}

static bool host_synced(void *ctx, double *err_us)
{
    (void)ctx;
    *err_us = NAN;
#if defined(__linux__) || defined(__APPLE__)
    struct timex t;
    memset(&t, 0, sizeof t);
    const int r = ntp_adjtime(&t);
    if (r < 0 || r == TIME_ERROR || (t.status & STA_UNSYNC))
        return false;
    *err_us = (double)t.esterror;
    return true;
#else
    return false;
#endif
}

void rdofradio_utc_setup(const rdof_slot_timing_t *t, double rate, bool own_time, const rdofradio_utc_clock_t *clock)
{
    memset(&U, 0, sizeof U);
    U.own = own_time;
    U.timing = *t;
    U.on = t->utc && rate > 0;
    U.rate = rate;
    U.clock = clock ? *clock : (rdofradio_utc_clock_t){host_utc_ns, host_synced, NULL};
    rdof_slot_gate_init(&U.gate, t);
    rdof_sample_clock_init(&U.sclock, rate, WINDOW_S, false);
}

bool rdofradio_utc_enabled(void) { return U.on; }

static void gate_tick(double now_s)
{
    double err = NAN;
    bool synced = U.clock.synced(U.clock.ctx, &err);
    const bool own_was = U.own_now;
    U.own_now = U.own && (!synced || !(err <= U.timing.max_err_ntp_us));
    if (U.own_now) /* the host's own time is the reference: no error against itself */
        synced = true, err = 0;
    const rdof_gate_state_t was = U.gate.state;
    rdof_slot_gate_update(&U.gate, now_s, synced, "ntp", synced ? err : NAN);
    if (U.gate.state != was || U.own_now != own_was || !U.told) {
        U.told = true;
        if (U.own_now)
            fprintf(stderr, "slot timing: on the host's own time (its clock isn't synced to UTC)\n");
        else if (rdof_slot_gate_on_grid(&U.gate))
            fprintf(stderr, "slot timing: %s on the UTC grid (the host's clock, %.0f us)\n",
                    rdof_gate_state_name(U.gate.state), err);
        else
            fprintf(stderr, "slot timing: %s: %s\n", rdof_gate_state_name(U.gate.state), U.gate.why);
    }
}

void rdofradio_utc_arrived(int64_t end_index, double now_s)
{
    if (!U.on)
        return;
    rdof_sample_clock_add(&U.sclock, end_index, U.clock.utc_ns(U.clock.ctx));
    if (U.sclock.windows != U.windows) { /* a new anchor: the rate against one FIT_S back */
        U.windows = U.sclock.windows;
        const double dt = (double)(U.sclock.t0 - U.ref_t) / 1e9;
        if (!U.has_ref) {
            U.has_ref = true;
            U.ref_i = U.sclock.i0, U.ref_t = U.sclock.t0;
        } else if (dt >= FIT_S) {
            const double ppm = ((double)(U.sclock.i0 - U.ref_i) / dt / U.rate - 1.0) * 1e6;
            if (fabs(ppm) < 200) /* a step in the host's clock (or a stream that lost samples) is not a rate */
                U.sclock.ppm = ppm;
            U.ref_i = U.sclock.i0, U.ref_t = U.sclock.t0;
        }
    }
    if (!U.gate_done || now_s - U.gate_at >= GATE_EVERY_S) {
        U.gate_done = true;
        U.gate_at = now_s;
        gate_tick(now_s);
    }
}

bool rdofradio_utc_map48(int64_t *i0, int64_t *t0, double *rate)
{
    if (!U.on || !rdof_slot_gate_on_grid(&U.gate) || !U.sclock.has_anchor)
        return false;
    *i0 = (int64_t)llround((double)U.sclock.i0 * 48000.0 / U.rate);
    *t0 = U.sclock.t0;
    *rate = rdof_sample_clock_rate(&U.sclock) * 48000.0 / U.rate;
    return true;
}

void rdofradio_utc_apply(struct rdof_sdrmodem *m)
{
    if (!U.on)
        return;
    int64_t i0 = 0, t0 = 0;
    double rate = 0;
    const bool on = rdofradio_utc_map48(&i0, &t0, &rate);
    rdof_sdrmodem_grid(m, true, U.timing.offset_us);
    rdof_sdrmodem_grid_map(m, on, i0, t0, rate);
    rdof_sdrmodem_set_transmit(m, rdof_slot_gate_may_transmit(&U.gate));
}

rdof_gate_state_t rdofradio_utc_state(const char **why, double *ppm)
{
    if (why)
        *why = U.on ? U.gate.why : "slot timing free";
    if (ppm)
        *ppm = U.sclock.ppm;
    return U.on ? U.gate.state : RDOF_GATE_FREE;
}
