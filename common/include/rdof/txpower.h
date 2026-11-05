// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Transmit power in dBm at the antenna connector, for every transmitter.
 *
 * A modem's SET_FREQ carries an RF level, 0..255. A transmitter's map says what the levels mean: level 0 is min_dbm,
 * 255 is max_dbm, linear in dB between. A modem's map is its device's tx.min_dbm / tx.max_dbm; without one it sends
 * at level 255 (the radio's own setting). An SDR's map ends at its device's tx.power_dbm, so level 255 is the
 * device's power and a frequency's power_dbm over it is held to it (rdof_txpower_sdr_map).
 *
 * A front end with a PA and a bypass (the W12's GC1109) picks the path per transmission (rdof_txpower_path).
 * Portable C, no allocation.
 */
#ifndef RDOF_TXPOWER_H
#define RDOF_TXPOWER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* An SDR's map spans this much under its device's power: 0.2 dB a level. */
#define RDOF_TXPOWER_SDR_SPAN_DB 51.0

typedef struct {
    double min_dbm, max_dbm; /* the power at level 0 and at 255; no map when max_dbm isn't over min_dbm */
} rdof_txmap_t;

bool rdof_txmap_valid(const rdof_txmap_t *m);
/* The level for dbm: rounded, 0..255. NAN or no map: 255. */
int rdof_txpower_level(const rdof_txmap_t *m, double dbm);
/* The power at level (0..255). No map: NAN. */
double rdof_txpower_dbm(const rdof_txmap_t *m, int level);
/* An SDR's map: its device's power at level 255. */
rdof_txmap_t rdof_txpower_sdr_map(double device_dbm);

/* A front end whose chip feeds the antenna through a PA or a bypass. Gains are chip to antenna, dB. */
typedef struct {
    double chip_min, chip_max, chip_step; /* the chip's own output, dBm */
    double pa_gain, bypass_gain;
    double pa_above; /* the PA only for a request over this, dBm at the antenna */
    double pa_max;   /* the PA path's most at the antenna (it saturates there) */
} rdof_txpower_fe_t;

typedef struct {
    bool pa;         /* through the PA, else the bypass */
    double chip_dbm; /* the chip's setting, on its steps */
    double out_dbm;  /* at the antenna, as the gains say */
} rdof_txpower_path_t;

/* The path for want dBm at the antenna: the PA only when pa_ok (tuned in its band) and want is over pa_above; the
   chip at want less the path's gain, a step down when between steps, held to its range (and to pa_max through the
   PA). */
rdof_txpower_path_t rdof_txpower_path(const rdof_txpower_fe_t *fe, double want, bool pa_ok);
/* The least and the most at the antenna (pa_ok: the PA may be used). */
void rdof_txpower_range(const rdof_txpower_fe_t *fe, bool pa_ok, double *min_dbm, double *max_dbm);

/* A bench job's power (bench_tx's power_dbm or power): dbm, at the antenna, when not NAN; else level, the older
   0..255 on the transmitter's range (255: the most). */
typedef struct {
    double dbm;
    int level;
} rdof_bench_power_t;

/* The bench's own limit (bench.tx): max_dbm when has_max_dbm, else rf_level on the transmitter's range. */
typedef struct {
    bool has_max_dbm;
    double max_dbm;
    int rf_level;
} rdof_bench_limit_t;

/* What a job sends at on a transmitter whose range is lo..hi dBm (an SDR's front end): the job's power under the
   bench's limit. *asked (may be NULL): the job's own, on that range. */
double rdof_bench_power_dbm(const rdof_bench_limit_t *lim, rdof_bench_power_t want, double lo, double hi,
                            double *asked);
/* The same as an RF level on a modem's map. Without a map a dBm can't be told: a dBm asked or allowed sends at 0
   (the least), else the job's level under the limit's rf_level. */
int rdof_bench_power_level(const rdof_bench_limit_t *lim, rdof_bench_power_t want, const rdof_txmap_t *map);

#ifdef __cplusplus
}
#endif

#endif
