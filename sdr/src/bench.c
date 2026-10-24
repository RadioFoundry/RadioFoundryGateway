// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* rdof_sdr_bench: each stage on its own over a clip, best of reps. */
#include "rdof/sdr/bench.h"

#if RDOF_FEATURE_SDR && RDOF_FEATURE_SDR_RECEIVER

#include <math.h>
#include <string.h>

#include "rdof/sdr/iq.h"
#include "sdr_private.h"

const char *const rdof_bench_stage_names[RDOF_BENCH_STAGES] = {
    "cu8 to float",
    "channelizer: forward FFT",
    "channelizer: per channel (bins, IFFT, NCO)",
    "discriminator (fast atan2)",
    "RRC matched filter + power",
    "squelch, sync search, slicing, flywheel",
    "call trackers",
};

int rdof_sdr_bench(const uint8_t *cu8, int n, const rdof_sdr_rx_config_t *cfg, const rdof_sdr_channel_t *chans, int nch,
                 double (*now)(void), int reps, rdof_sdr_bench_t *out)
{
    memset(out, 0, sizeof *out);
    for (int s = 0; s < RDOF_BENCH_STAGES; s++)
        out->seconds[s] = INFINITY;
    out->search_no_squelch = INFINITY;
    out->clip_seconds = n / cfg->sample_rate;
    double freqs[64], offsets[64], center = cfg->center_hz;
    if (nch < 1 || nch > 64)
        return -1;
    for (int c = 0; c < nch; c++)
        freqs[c] = chans[c].freq_hz;
    if (center == 0.0 && rdof_plan_window(freqs, nch, cfg->sample_rate, &center, NULL) == 0)
        return -1;
    for (int c = 0; c < nch; c++)
        offsets[c] = freqs[c] - center;
    const int D = (int)nearbyint(cfg->sample_rate / 48000.0);
    /* one chunk of floats at a time (a board can't hold the whole clip as floats and the outputs too) */
    rdof_cf_t *iq = rdof_sdr_alloc(sizeof(rdof_cf_t) * (size_t)cfg->max_chunk);
    rdof_chz_t chz;
    if (!iq)
        return -1;
    if (rdof_chz_init(&chz, cfg->sample_rate, offsets, nch, D, RDOF_CHZ_OUT_FFT, RDOF_CHZ_CUTOFF_HZ)) {
        rdof_chz_free(&chz); /* what it did allocate */
        rdof_sdr_free(iq);
        return -1;
    }
    const int stride = rdof_chz_out_len(&chz, n) + chz.out_per_hop;
    const int per = (cfg->max_chunk / chz.hop + 1) * chz.out_per_hop; /* most outputs one chunk gives */
    rdof_cf_t *y = rdof_sdr_alloc(sizeof(rdof_cf_t) * (size_t)stride * nch);
    rdof_cf_t *tmp = rdof_sdr_alloc(sizeof(rdof_cf_t) * (size_t)(per + 1) * nch);
    float *f = rdof_sdr_alloc(sizeof(float) * (size_t)(stride + 1)), *p = rdof_sdr_alloc(sizeof(float) * (size_t)(stride + 1));
    rdof_cf_t *X = rdof_sdr_alloc(sizeof(rdof_cf_t) * chz.N);
    rdof_sdr_burst_t *bursts = NULL;
    double *btimes = NULL;
    int *bchan = NULL, nbursts = 0, bcap = 0;
    rdof_chz_free(&chz);
    int err = !y || !tmp || !f || !p || !X;
    int produced = 0;
    for (int r = 0; r < reps && !err; r++) {
        double t, t_conv = 0, t_chz = 0;
        if (rdof_chz_init(&chz, cfg->sample_rate, offsets, nch, D, RDOF_CHZ_OUT_FFT, RDOF_CHZ_CUTOFF_HZ)) {
            rdof_chz_free(&chz);
            err = 1;
            break;
        }
        produced = 0;
        for (int i = 0; i < n; i += cfg->max_chunk) {
            int m = n - i < cfg->max_chunk ? n - i : cfg->max_chunk;
            t = now();
            rdof_iq_from_cu8(cu8 + 2 * (size_t)i, (size_t)m, iq);
            double t1 = now();
            int got = rdof_chz_process(&chz, iq, m, tmp, per + 1);
            t_chz += now() - t1;
            t_conv += t1 - t;
            for (int c = 0; c < nch; c++)
                memcpy(y + (size_t)c * stride + produced, tmp + (size_t)c * (per + 1), sizeof(rdof_cf_t) * got);
            produced += got;
        }
        out->seconds[RDOF_BENCH_CONVERT] = fmin(out->seconds[RDOF_BENCH_CONVERT], t_conv);
        out->ffts = chz.block;
        out->fft_points = chz.N;
        t = now();
        const int span = cfg->max_chunk - chz.N > 0 ? cfg->max_chunk - chz.N : 1; /* the last chunk's samples */
        for (long long b = 0; b < chz.block; b++)
            rdof_fft_run(chz.fwd, iq + (b * chz.hop) % span, X);
        double t_fft = now() - t;
        rdof_chz_free(&chz);
        out->seconds[RDOF_BENCH_FFT] = fmin(out->seconds[RDOF_BENCH_FFT], t_fft);
        out->seconds[RDOF_BENCH_CHANNEL] = fmin(out->seconds[RDOF_BENCH_CHANNEL], t_chz - t_fft);

        t = now();
        for (int c = 0; c < nch; c++) {
            rdof_cf_t prev = {0, 0};
            rdof_fm_discriminate(&prev, 48000.0f / 6.2831853f, y + (size_t)c * stride, produced, f);
        }
        double t_disc = now() - t;
        double t_fm = 0;
        for (int c = 0; c < nch; c++) {
            rdof_fm_t fm;
            rdof_fm_init(&fm, 48000.0); /* not timed: the taps, once per channel */
            t = now();
            rdof_fm_process(&fm, y + (size_t)c * stride, produced, f, p);
            t_fm += now() - t;
            rdof_fm_free(&fm);
        }
        out->seconds[RDOF_BENCH_DISC] = fmin(out->seconds[RDOF_BENCH_DISC], t_disc);
        out->seconds[RDOF_BENCH_RRC] = fmin(out->seconds[RDOF_BENCH_RRC], t_fm - t_disc);

        for (int sq = 0; sq < 2; sq++) {
            long count = 0;
            double t_init = 0;
            t = now();
            for (int c = 0; c < nch; c++) {
                rdof_demod_t d;
                const double ti = now();
                if (rdof_demod_init(&d, chans[c].cc, chans[c].slot, sq ? 0.0 : RDOF_DEMOD_SQUELCH_HZ, per)) {
                    rdof_demod_free(&d);
                    err = 1;
                    break;
                }
                t_init += now() - ti; /* not timed: once per channel */
                for (int i = 0; i < produced; i += per) {
                    int nb;
                    const rdof_sdr_burst_t *bs =
                        rdof_demod_feed(&d, y + (size_t)c * stride + i, produced - i < per ? produced - i : per, &nb);
                    count += nb;
                    if (!sq && r == 0) { /* keep the bursts for the tracker stage */
                        if (nbursts + nb > bcap) {
                            int ncap = (nbursts + nb) * 2 + 64;
                            rdof_sdr_burst_t *nb2 = rdof_sdr_alloc(sizeof *nb2 * ncap);
                            double *nt = rdof_sdr_alloc(sizeof *nt * ncap);
                            int *nc = rdof_sdr_alloc(sizeof *nc * ncap);
                            if (!nb2 || !nt || !nc) {
                                rdof_sdr_free(nb2);
                                rdof_sdr_free(nt);
                                rdof_sdr_free(nc);
                                err = 1;
                                break;
                            }
                            if (nbursts) {
                                memcpy(nb2, bursts, sizeof *nb2 * nbursts);
                                memcpy(nt, btimes, sizeof *nt * nbursts);
                                memcpy(nc, bchan, sizeof *nc * nbursts);
                            }
                            rdof_sdr_free(bursts);
                            rdof_sdr_free(btimes);
                            rdof_sdr_free(bchan);
                            bursts = nb2;
                            btimes = nt;
                            bchan = nc;
                            bcap = ncap;
                        }
                        for (int k = 0; k < nb; k++) {
                            bursts[nbursts] = bs[k];
                            btimes[nbursts] = bs[k].position / 48000.0;
                            bchan[nbursts++] = c;
                        }
                    }
                }
                rdof_demod_free(&d);
            }
            double dt = now() - t - t_init - t_fm;
            if (sq)
                out->search_no_squelch = fmin(out->search_no_squelch, dt);
            else {
                out->seconds[RDOF_BENCH_SEARCH] = fmin(out->seconds[RDOF_BENCH_SEARCH], dt);
                out->bursts = count;
            }
        }

        rdof_tracker_t *tr = rdof_sdr_zalloc(sizeof(rdof_tracker_t) * (size_t)nch);
        if (!tr) {
            err = 1;
            break;
        }
        t = now();
        for (int c = 0; c < nch; c++)
            rdof_tracker_init(&tr[c], chans[c].id, cfg->rssi_offset_db, chans[c].freq_hz);
        for (int k = 0; k < nbursts; k++)
            rdof_tracker_feed(&tr[bchan[k]], &bursts[k].b, btimes[k]);
        for (int c = 0; c < nch; c++)
            rdof_tracker_tick(&tr[c], out->clip_seconds + 10);
        out->seconds[RDOF_BENCH_TRACKER] = fmin(out->seconds[RDOF_BENCH_TRACKER], now() - t);
        rdof_sdr_free(tr);
    }
    rdof_sdr_free(bursts);
    rdof_sdr_free(btimes);
    rdof_sdr_free(bchan);
    rdof_sdr_free(iq);
    rdof_sdr_free(y);
    rdof_sdr_free(tmp);
    rdof_sdr_free(f);
    rdof_sdr_free(p);
    rdof_sdr_free(X);
    return err ? -1 : 0;
}

#endif
