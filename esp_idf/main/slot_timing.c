// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * UTC slot timing on the board (docs/spec/slot-timing.md): the SDR stream's
 * sample clock against UTC, the clock gate, and what the SDR modem gets from
 * them.
 *
 * The DSP task stamps each chunk with its arrival time on netclock's UTC
 * (slot_timing_arrived); rdof_sample_clock fits the earliest-arrival line, so
 * USB and scheduling jitter only make arrivals late. The gate follows
 * netclock_quality once a second: aligned while netclock is synced to PPS or
 * NTP under the bound, else holdover / free / stop as slot_timing.on_loss
 * says. The modem (modem_sdr.c) asks for the map and the gate before every
 * pull (slot_timing_apply); the receiver gets the map before every chunk to
 * label its bursts' UTC slots (slot_timing_rx).
 *
 * Assumed: 48 kS/s position base + p is stream sample p * rate / 48000 (the
 * receiver's decimation, its few samples of filter delay not taken off; base
 * the receiver's position when the stream started, as a stalled stream
 * restarts without a new receiver), and the front end's transmit positions
 * count the same crystal's samples.
 */
#include <math.h>
#include <string.h>

#include "app.h"

#if CONFIG_RDOF_SDR

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "netclock.h"
#include "rdof/sdr/receiver.h"
#include "rdof/sdr/sdrmodem.h"
#include "rdof/slotgrid.h"

#define WINDOW_S 2.0
#define GATE_EVERY_US 1000000

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static rdof_slot_timing_t timing;
static rdof_slot_gate_t gate;
static rdof_sample_clock_t sclock;
static double stream_rate;
static int64_t stream_base48; /* the receiver's 48 kS/s position at the stream's sample 0 */
static int64_t gate_at_us;
static bool set_up;
static bool modem_seen; /* what the modem last said about its slots */
static int64_t modem_err_ns;
static uint32_t modem_slips, modem_snaps, modem_labeled;

void slot_timing_setup(const rdof_slot_timing_t *t)
{
    portENTER_CRITICAL(&mux);
    timing = *t;
    rdof_slot_gate_init(&gate, t);
    set_up = true;
    portEXIT_CRITICAL(&mux);
}

void slot_timing_stream(double rate, int64_t base48)
{
    portENTER_CRITICAL(&mux);
    stream_rate = rate;
    stream_base48 = base48;
    rdof_sample_clock_init(&sclock, rate, WINDOW_S, false);
    portEXIT_CRITICAL(&mux);
}

static void gate_tick(int64_t now_us)
{
    netclock_source_t src = NETCLOCK_SRC_NONE;
    double err = NAN;
    const bool synced = netclock_quality(&src, &err);
    portENTER_CRITICAL(&mux);
    rdof_slot_gate_update(&gate, now_us / 1e6, synced, netclock_source_name(src), synced ? err : NAN);
    portEXIT_CRITICAL(&mux);
}

void slot_timing_arrived(int64_t end_index, int64_t esp_us)
{
    if (!set_up || !timing.utc || stream_rate <= 0)
        return;
    const int64_t utc_ns = netclock_utc_ns(esp_us, NULL);
    portENTER_CRITICAL(&mux);
    rdof_sample_clock_add(&sclock, end_index, utc_ns);
    portEXIT_CRITICAL(&mux);
    if (esp_us - gate_at_us >= GATE_EVERY_US) {
        gate_at_us = esp_us;
        gate_tick(esp_us);
    }
}

/* The map at 48 kS/s: position *i0 is UTC *t0 ns, *rate positions a UTC second. False while off the grid. */
static bool map48(bool *utc, int *offset, int64_t *i0, int64_t *t0, double *rate, bool *tx)
{
    portENTER_CRITICAL(&mux);
    *utc = set_up && timing.utc;
    const bool on = *utc && rdof_slot_gate_on_grid(&gate) && sclock.has_anchor && stream_rate > 0;
    if (tx)
        *tx = !*utc || rdof_slot_gate_may_transmit(&gate);
    *offset = timing.offset_us;
    const int64_t si0 = sclock.i0, base = stream_base48;
    *t0 = sclock.t0;
    const double r = stream_rate, srate = rdof_sample_clock_rate(&sclock);
    portEXIT_CRITICAL(&mux);
    if (!on)
        return false;
    *i0 = base + (int64_t)llround((double)si0 * 48000.0 / r);
    *rate = srate * 48000.0 / r;
    return true;
}

void slot_timing_rx(rdof_sdr_rx_t *rx)
{
    bool utc;
    int offset;
    int64_t i0 = 0, t0 = 0;
    double rate = 0;
    const bool on = map48(&utc, &offset, &i0, &t0, &rate, NULL);
    rdof_sdr_rx_utc_map(rx, on, i0, t0, rate, offset);
}

void slot_timing_apply(rdof_sdrmodem_t *m)
{
    bool utc, tx;
    int offset;
    int64_t i0 = 0, t0 = 0;
    double rate = 0;
    const bool on = map48(&utc, &offset, &i0, &t0, &rate, &tx);
    rdof_sdrmodem_grid(m, utc, offset);
    rdof_sdrmodem_grid_map(m, on, i0, t0, rate);
    rdof_sdrmodem_set_transmit(m, tx);
    portENTER_CRITICAL(&mux);
    modem_seen = true;
    modem_err_ns = m->grid_err_ns;
    modem_slips = m->grid_slips, modem_snaps = m->grid_snaps, modem_labeled = m->grid_labeled;
    portEXIT_CRITICAL(&mux);
}

void slot_timing_json(rdof_json_t *j)
{
    if (!set_up)
        return;
    portENTER_CRITICAL(&mux);
    const rdof_slot_gate_t g = gate;
    const bool has_err = sclock.has_err;
    const int64_t clock_err_ns = sclock.err_ns;
    const uint32_t windows = sclock.windows;
    const bool seen = modem_seen;
    const int64_t phase_ns = modem_err_ns;
    const uint32_t slips = modem_slips, snaps = modem_snaps, labeled = modem_labeled;
    portEXIT_CRITICAL(&mux);
    char d[96];
    rdof_slot_timing_describe(&timing, d, sizeof d);
    rdof_json_open(j, "slot_timing");
    rdof_json_str(j, "setting", d);
    rdof_json_str(j, "mode", timing.utc ? "utc" : "free");
    rdof_json_str(j, "state", rdof_gate_state_name(g.state));
    rdof_json_str(j, "source", g.source);
    if (isfinite(g.err_us))
        rdof_json_float3(j, "err_us", g.err_us);
    else
        rdof_json_raw(j, "err_us", "null");
    rdof_json_float3(j, "holdover_s", rdof_slot_gate_holdover_age(&g, esp_timer_get_time() / 1e6));
    rdof_json_str(j, "why", g.why);
    if (has_err)
        rdof_json_float3(j, "sample_clock_err_us", clock_err_ns / 1000.0);
    else
        rdof_json_raw(j, "sample_clock_err_us", "null");
    rdof_json_int(j, "sample_clock_windows", windows);
    if (seen) { /* the modem: its last slot against the target before steering, and how often it moved */
        rdof_json_float3(j, "phase_us", phase_ns / 1000.0);
        rdof_json_int(j, "slips", slips);
        rdof_json_int(j, "snaps", snaps);
        rdof_json_int(j, "labeled", labeled);
    }
    rdof_json_close(j);
}

#endif
