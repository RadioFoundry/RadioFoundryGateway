// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The SDR receiver, all its channels in one process: channelizer, a demod
 * and a call tracker per channel, AFC after each chunk.
 */
#include "rdof/sdr/receiver.h"

#if RDOF_FEATURE_SDR && RDOF_FEATURE_SDR_RECEIVER

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sdr_private.h"

#include "rdof/pdu.h"
#include "rdof/slotgrid.h"

void rdof_sdr_rx_free(rdof_sdr_rx_t *rx)
{
    rdof_chz_free(&rx->chz);
    if (rx->demods)
        for (int c = 0; c < rx->C; c++)
            rdof_demod_free(&rx->demods[c]);
    rdof_sdr_free(rx->demods);
    rdof_sdr_free(rx->trackers);
    rdof_sdr_free(rx->y);
    rdof_sdr_free(rx->bursts);
    rdof_sdr_free(rx->last_burst);
    rdof_sdr_free(rx->rssi);
    rdof_sdr_free(rx->afc_pending);
    rdof_sdr_free(rx->afc_at);
    rdof_sdr_free(rx->hist_at);
    rdof_sdr_free(rx->hist_corr);
    rdof_sdr_free(rx->hist_n);
    rdof_sdr_free(rx->afc_chan);
    rdof_sdr_free(rx->was_hot);
    rdof_sdr_free(rx->last_pos);
    rdof_sdr_free(rx->gated);
    memset(rx, 0, sizeof *rx);
}

void rdof_sdr_rx_utc_map(rdof_sdr_rx_t *rx, bool ok, int64_t i0, int64_t t0_ns, double rate, int offset_us)
{
    rx->utc_ok = ok && rate > 0;
    rx->utc_i0 = i0;
    rx->utc_t0 = t0_ns;
    rx->utc_rate = rate;
    rx->utc_offset_us = offset_us;
}

/* A BS or MS burst's UTC start and slot n; an MS burst (no CACH to say) takes its slot from it. */
static void stamp(const rdof_sdr_rx_t *rx, rdof_sdr_burst_t *b)
{
    if (!rx->utc_ok || (b->b.family != RDOF_FAMILY_BS && b->b.family != RDOF_FAMILY_MS))
        return;
    const int64_t start =
        rx->utc_t0 + (int64_t)floor((double)(b->position - rx->utc_i0) * 1e9 / rx->utc_rate + 0.5);
    int ts;
    int64_t err;
    b->b.utc_slot = b->b.family == RDOF_FAMILY_BS ? rdof_grid_outbound(start, rx->utc_offset_us, &ts, &err)
                                                : rdof_grid_inbound(start, rx->utc_offset_us, &ts, &err);
    b->b.utc_us = start / 1000 - (start % 1000 < 0); /* start // 1000 */
    b->b.utc = true;
    if (b->b.family == RDOF_FAMILY_MS)
        b->b.slot = ts;
}

int rdof_sdr_rx_init(rdof_sdr_rx_t *rx, const rdof_sdr_rx_config_t *cfg, const rdof_sdr_channel_t *chans, int n,
                   const rdof_sink_t *sink, double t0)
{
    memset(rx, 0, sizeof *rx);
    rx->cfg = *cfg;
    rx->C = n;
    rx->t0 = t0;
    if (n < 1 || cfg->max_chunk < 1)
        return -1;
    double dec = cfg->sample_rate / RDOF_DEMOD_FS;
    if (fabs(dec - nearbyint(dec)) > 1e-6)
        return -1; /* sample_rate / decimation must be 48 kS/s */
    double *freqs = rdof_sdr_alloc(sizeof(double) * n), *offsets = rdof_sdr_alloc(sizeof(double) * n);
    if (!freqs || !offsets) {
        rdof_sdr_free(freqs);
        rdof_sdr_free(offsets);
        return -1;
    }
    for (int c = 0; c < n; c++)
        freqs[c] = chans[c].freq_hz;
    rx->center_hz = cfg->center_hz;
    if (rx->center_hz == 0.0 && rdof_plan_center(freqs, n, cfg->sample_rate, &rx->center_hz)) {
        rdof_sdr_free(freqs);
        rdof_sdr_free(offsets);
        return -1;
    }
    for (int c = 0; c < n; c++)
        offsets[c] = freqs[c] - rx->center_hz;
    int err = rdof_chz_init(&rx->chz, cfg->sample_rate, offsets, n, (int)nearbyint(dec), RDOF_CHZ_OUT_FFT,
                          RDOF_CHZ_CUTOFF_HZ);
    rdof_sdr_free(offsets);
    if (!err) /* a tuner that's ppm fast shifts every channel up by f * ppm * 1e-6 */
        for (int c = 0; c < n; c++)
            rx->chz.correction_hz[c] = freqs[c] * cfg->software_ppm * 1e-6;
    rdof_sdr_free(freqs);
    if (err)
        return -1;
    rx->stride = cfg->max_chunk / rx->chz.hop * rx->chz.out_per_hop + rx->chz.out_per_hop;
    rx->y = rdof_sdr_alloc(sizeof(rdof_cf_t) * (size_t)rx->stride * n);
    rx->demods = rdof_sdr_zalloc(sizeof(rdof_demod_t) * (size_t)(n));
    rx->trackers = rdof_sdr_zalloc(sizeof(rdof_tracker_t) * (size_t)(n));
    rx->bursts = rdof_sdr_zalloc(sizeof(long) * (size_t)(n));
    rx->last_burst = rdof_sdr_zalloc(sizeof(double) * (size_t)n);
    rx->rssi = rdof_sdr_alloc(sizeof(int) * (size_t)n);
    rx->afc_cap = 64 + rx->stride / 100; /* measurements a channel can give in one chunk */
    rx->afc_pending = rdof_sdr_alloc(sizeof(double) * (size_t)rx->afc_cap * n);
    rx->afc_chan = rdof_sdr_zalloc(sizeof(int) * (size_t)n);
    rx->afc_at = rdof_sdr_alloc(sizeof(int64_t) * (size_t)rx->afc_cap * n);
    rx->hist_at = rdof_sdr_alloc(sizeof(int64_t) * RDOF_AFC_HISTORY * (size_t)n);
    rx->hist_corr = rdof_sdr_alloc(sizeof(double) * RDOF_AFC_HISTORY * (size_t)n);
    rx->hist_n = rdof_sdr_alloc(sizeof(int) * (size_t)n);
    rx->was_hot = rdof_sdr_alloc(sizeof(bool) * (size_t)n);
    rx->last_pos = rdof_sdr_alloc(sizeof(int64_t) * (size_t)n);
    rx->gated = rdof_sdr_zalloc(sizeof(long) * (size_t)n);
    if (!rx->y || !rx->demods || !rx->trackers || !rx->bursts || !rx->afc_pending || !rx->afc_chan ||
        !rx->last_burst || !rx->rssi || !rx->was_hot || !rx->last_pos || !rx->gated || !rx->afc_at || !rx->hist_at ||
        !rx->hist_corr || !rx->hist_n)
        return -1;
    for (int c = 0; c < n; c++) {
        rx->hist_at[c * RDOF_AFC_HISTORY] = 0;
        rx->hist_corr[c * RDOF_AFC_HISTORY] = rx->chz.correction_hz[c];
        rx->hist_n[c] = 1;
    }
    rx->gate = true;
    for (int c = 0; c < n; c++) {
        rx->was_hot[c] = true; /* nothing skipped before a chunk has been looked at */
        rx->last_pos[c] = 0; /* every channel runs its first RDOF_GATE_HOLD samples */
    }
    for (int c = 0; c < n; c++)
        rx->rssi[c] = INT32_MIN;
    for (int c = 0; c < n; c++) {
        if (rdof_demod_init(&rx->demods[c], chans[c].cc, chans[c].slot, RDOF_DEMOD_SQUELCH_HZ, rx->stride))
            return -1;
        rdof_tracker_init(&rx->trackers[c], chans[c].id, cfg->rssi_offset_db, chans[c].freq_hz);
        if (sink)
            rdof_tracker_add_sink(&rx->trackers[c], *sink);
    }
    return 0;
}

bool rdof_sdr_rx_add_sink(rdof_sdr_rx_t *rx, int channel, const rdof_sink_t *sink)
{
    if (channel < 0 || channel >= rx->C)
        return false;
    return rdof_tracker_add_sink(&rx->trackers[channel], *sink);
}

int rdof_sdr_rx_channelize(rdof_sdr_rx_t *rx, const rdof_cf_t *iq, int n)
{
    if (n > rx->cfg.max_chunk)
        n = rx->cfg.max_chunk;
    rx->got = rdof_chz_process(&rx->chz, iq, n, rx->y, rx->stride);
    rx->samples += n;
    rx->out_index += rx->got;
    if (rx->got)
        rx->chunks++;
    return rx->got;
}

int rdof_sdr_rx_forward(rdof_sdr_rx_t *rx, const rdof_cf_t *iq, int n)
{
    if (n > rx->cfg.max_chunk)
        n = rx->cfg.max_chunk;
    /* the blocks' store, once; without the memory, the whole channelizer here (rdof_sdr_rx_channels then only
     * demods) */
    rx->fused = !rx->chz.xs_cap && rdof_chz_reserve(&rx->chz, rx->cfg.max_chunk) != 0;
    rx->got = rx->fused ? rdof_chz_process(&rx->chz, iq, n, rx->y, rx->stride) : rdof_chz_forward(&rx->chz, iq, n);
    rx->samples += n;
    rx->out_index += rx->got;
    if (rx->got)
        rx->chunks++;
    return rx->got;
}

void rdof_sdr_rx_channels(rdof_sdr_rx_t *rx, int c0, int c1, int worker)
{
    if (!rx->fused)
        rdof_chz_channels(&rx->chz, c0, c1, worker, rx->y, rx->stride);
    rdof_sdr_rx_demod(rx, c0, c1);
}

void rdof_sdr_rx_demod(rdof_sdr_rx_t *rx, int c0, int c1)
{
    for (int c = c0; c < c1 && c < rx->C; c++) {
        int nb;
        double *afc = rx->afc_pending + (size_t)c * rx->afc_cap;
        rx->afc_chan[c] = 0;
        /* a chunk too short to give output says nothing about the channel */
        const bool hot = rx->chz.level[c] > RDOF_GATE_LEVEL, idle = rx->gate && rx->got && !hot && !rx->was_hot[c];
        if (rx->got)
            rx->was_hot[c] = hot;
        const int64_t start = rx->out_index - rx->got; /* this chunk's first channel sample */
        const bool probe = (start + (int64_t)c * RDOF_GATE_EVERY / rx->C) % RDOF_GATE_EVERY < RDOF_GATE_PROBE;
        if (idle && !probe && start - rx->last_pos[c] > RDOF_GATE_HOLD && !rdof_demod_busy(&rx->demods[c])) {
            rdof_demod_skip(&rx->demods[c], rx->y + (size_t)c * rx->stride, rx->got);
            rx->gated[c]++;
            continue;
        }
        const rdof_sdr_burst_t *bs = rdof_demod_feed(&rx->demods[c], rx->y + (size_t)c * rx->stride, rx->got, &nb);
        if (nb)
            rx->last_pos[c] = bs[nb - 1].position;
        for (int i = 0; i < nb; i++) {
            rdof_sdr_burst_t stamped = bs[i];
            const rdof_sdr_burst_t *b = &stamped;
            stamp(rx, &stamped);
            double t = rx->t0 + (double)b->position / RDOF_DEMOD_FS;
            if (b->b.dt != RDOF_DT_VOICE && rx->afc_chan[c] < rx->afc_cap &&
                !(rx->muted && rx->muted(rx->burst_ctx, b->position))) {
                rx->afc_at[(size_t)c * rx->afc_cap + rx->afc_chan[c]] = b->position + RDOF_AFC_AT;
                afc[rx->afc_chan[c]++] = b->freq_offset_hz;
            }
            if (b->b.dt != RDOF_DT_IDLE) {
                rx->bursts[c]++;
                rx->last_burst[c] = t;
                rx->rssi[c] = (int)nearbyint(b->b.power_dbfs + rx->cfg.rssi_offset_db);
            }
            if (rx->on_burst)
                rx->on_burst(rx->burst_ctx, c, b, t);
            rdof_tracker_feed(&rx->trackers[c], &b->b, t);
        }
    }
}

void rdof_sdr_rx_finish(rdof_sdr_rx_t *rx)
{
    /* Every channel's bursts, then the trackers' ticks, then AFC in channel order. */
    double t_now = rx->t0 + (double)rx->out_index / RDOF_DEMOD_FS;
    for (int c = 0; c < rx->C; c++)
        rdof_tracker_tick(&rx->trackers[c], t_now);
    if (!rx->cfg.afc)
        return;
    const double share = rx->got < RDOF_AFC_PERIOD ? (double)rx->got / RDOF_AFC_PERIOD : 1.0;
    for (int c = 0; c < rx->C; c++) {
        const double *afc = rx->afc_pending + (size_t)c * rx->afc_cap;
        const int64_t *at = rx->afc_at + (size_t)c * rx->afc_cap;
        const int n = rx->afc_chan[c];
        if (!n)
            continue;
        double sum = 0.0;
        for (int i = 0; i < n; i++)
            sum += rdof_sdr_rx_correction_at(rx, c, at[i]) + afc[i];
        double *corr = &rx->chz.correction_hz[c];
        const double now = *corr, lim = RDOF_AFC_STEP_HZ * share;
        double step = RDOF_AFC_GAIN * share * (sum / n - now);
        step = step < -lim ? -lim : step > lim ? lim : step;
        double v = now + step;
        v = v < -RDOF_AFC_LIMIT_HZ ? -RDOF_AFC_LIMIT_HZ : v > RDOF_AFC_LIMIT_HZ ? RDOF_AFC_LIMIT_HZ : v;
        if (v != now)
            rdof_sdr_rx_set_correction(rx, c, v);
    }
}

double rdof_sdr_rx_correction_at(const rdof_sdr_rx_t *rx, int c, int64_t pos)
{
    const int64_t *at = rx->hist_at + (size_t)c * RDOF_AFC_HISTORY;
    const double *corr = rx->hist_corr + (size_t)c * RDOF_AFC_HISTORY;
    for (int i = rx->hist_n[c] - 1; i >= 0; i--)
        if (at[i] <= pos)
            return corr[i];
    return corr[0];
}

void rdof_sdr_rx_set_correction(rdof_sdr_rx_t *rx, int c, double hz)
{
    rx->chz.correction_hz[c] = hz;
    int64_t *at = rx->hist_at + (size_t)c * RDOF_AFC_HISTORY;
    double *corr = rx->hist_corr + (size_t)c * RDOF_AFC_HISTORY;
    if (rx->hist_n[c] == RDOF_AFC_HISTORY) {
        memmove(at, at + 1, sizeof(int64_t) * (RDOF_AFC_HISTORY - 1));
        memmove(corr, corr + 1, sizeof(double) * (RDOF_AFC_HISTORY - 1));
        rx->hist_n[c]--;
    }
    at[rx->hist_n[c]] = rx->out_index;
    corr[rx->hist_n[c]++] = hz;
}

void rdof_sdr_rx_process(rdof_sdr_rx_t *rx, const rdof_cf_t *iq, int n)
{
    rdof_sdr_rx_channelize(rx, iq, n);
    rdof_sdr_rx_demod(rx, 0, rx->C);
    rdof_sdr_rx_finish(rx);
}

#endif
