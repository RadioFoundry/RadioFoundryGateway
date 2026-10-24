// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The LR2021's transmit paths, as plain C with no I/O: the command sequence that sets each one up, the frames
 * that change it live, and the sample formats each takes. lr2021_tx_stream (lr2021.h) runs them on a chip.
 *
 * Every fact here is in docs/spec/lr2021-iq.md section 3 (R3.n), measured on a W12 (ROM 1.24) on 2026-10-24.
 * The RF switch around the chip is the board's: set it up before these frames.
 *
 * | Mode              | Samples                             | On air                                           |
 * |-------------------|-------------------------------------|--------------------------------------------------|
 * | CW                | none                                | a carrier, no time limit (R3.29)                 |
 * | FREQ              | int8, count x fdev / 64 Hz          | any FM / FSK the host shapes (R3.9-R3.19)        |
 * | PHASE_STEP        | int8, count x rate / 128 Hz         | the same, a scale set by the rate (R3.18)        |
 * | IQ_PHASE          | int8 I, Q                           | the phase of I/Q at constant power (R3.22)       |
 * | IQ_POLAR          | int8 I, Q                           | phase and magnitude, the PA as AM (R3.22-R3.27)  |
 * | FSK4              | packed dibits, 4 a byte, MSB first  | the chip's own 4-FSK modem (R3.20, R3.21)        |
 */
#ifndef LR2021_TX_H
#define LR2021_TX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lr2021_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LR2021_TX_CW,
    LR2021_TX_FREQ,
    LR2021_TX_PHASE_STEP,
    LR2021_TX_IQ_PHASE,
    LR2021_TX_IQ_POLAR,
    LR2021_TX_FSK4,
} lr2021_tx_mode_t;

/* A sample run's length is a 16-bit count (R3.12); a longer transmission is runs back to back (R3.15). */
#define LR2021_TX_MAX_RUN 65535u
/* The largest frame lr2021_tx_frames writes. */
#define LR2021_TX_FRAME_MAX 13
/* The most frames lr2021_tx_frames writes. */
#define LR2021_TX_FRAMES_MAX 16

typedef struct {
    lr2021_tx_mode_t mode;
    uint32_t freq_hz;      /* already corrected for the crystal (lr2021_freq_corrected) */
    bool calib_fe;         /* calibrate the front end at freq_hz first (about 10.6 ms; not needed where RX already
                            * calibrated the same 4 MHz cell, R3.16) */
    bool standby_first;    /* SetStandby RC first (the chip must be in standby or FS for SetPacketType) */
    int8_t power_code;     /* lr2021_tx_power_code */
    uint8_t ramp_code;     /* lr2021_ramp_code */
    uint8_t pa_lf_duty, pa_lf_slices; /* SetPaConfig (DS table 7-19: 7, 7 for +22 dBm at 915 MHz) */
    uint8_t fallback;      /* LR2021_FALLBACK_*, where the chip goes after a run */
    uint32_t rate;         /* samples a second (FREQ, PHASE_STEP, IQ_*), or the bit rate (FSK4: 2 x symbols) */
    uint32_t fdev_hz;      /* FREQ: the scale (count x fdev / 64 Hz); FSK4: the outer level is 60 x fdev / 64 */
    uint8_t shape;         /* FSK4: the pulse shape (0 none, 0x05 Gaussian: R3.21); 0 for the others */
    uint8_t rx_bw;         /* an rx_bw code the chip accepts (0x1C will do); it doesn't affect TX */
    uint16_t n;            /* samples a run (FREQ, PHASE_STEP, IQ_*) or payload bytes (FSK4); 0 for CW */
} lr2021_tx_setup_t;

/* The frames that set the mode up, in order, each into frames[i] with its length in lens[i]; returns how many
 * (at most LR2021_TX_FRAMES_MAX), or -1 for a bad setup. Send them with lr2021_write, then fill the FIFO and send
 * lr2021_tx_start_frame. */
int lr2021_tx_frames(const lr2021_tx_setup_t *s, uint8_t frames[][LR2021_TX_FRAME_MAX], size_t *lens);
/* The frame that keys up: SetTxTestMode CW for CW, else SetTx with timeout (1/32 ms; 0 none). Sent again on
 * TX_DONE it starts the next run of the same setup (R3.15). */
size_t lr2021_tx_start_frame(const lr2021_tx_setup_t *s, uint8_t *buf, uint32_t timeout);
/* FIFO bytes a sample takes in a mode: 2 for I/Q, 1 for FREQ and PHASE_STEP, 0 for CW. FSK4's are bytes. */
int lr2021_tx_sample_bytes(lr2021_tx_mode_t mode);

/* ── Samples ─────────────────────────────────────────────────────────────── */

/* A PHASE_STEP sample for an instantaneous frequency: round(hz x 128 / rate), clamped to -128..127. */
int8_t lr2021_phase_step_sample(float hz, uint32_t rate);
/* (FREQ samples: lr2021_raw_freq_sample in lr2021_cmd.h.) */

/* FSK4: the fdev that puts the outer levels at outer_hz (60 x fdev / 64 = outer). DMR: 2073.6 Hz for 1944. */
uint32_t lr2021_fsk4_fdev(float outer_hz);
/* FSK4: symbols (+3, +1, -1, -3) into chip bytes, 4 a byte, MSB first (00 -1, 01 -3, 10 +1, 11 +3). A last part
 * byte is padded with -1. Returns the bytes written: (n + 3) / 4. */
size_t lr2021_fsk4_pack(const int8_t *symbols, size_t n, uint8_t *out);
/* FSK4: DMR's dibits (00 +1, 01 +3, 10 -1, 11 -3), packed as DMR packs them (a 33-byte burst is 132 symbols), to the
 * chip's in place: each byte XOR 0xAA. */
void lr2021_fsk4_from_dmr(uint8_t *bytes, size_t n);

/* IQ_POLAR's amplitude: the output level (dB under full) and phase shift (degrees) at each input magnitude, input
 * rising. Measured per board, band, PA setting and rate: fit it with a waveform at the rate to be used (R3.23). */
typedef struct {
    int n;
    const uint8_t *in;
    const float *out_db;
    const float *pm_deg;
} lr2021_am_table_t;
/* One measured table: a W12 at 920 MHz, +22 dBm with PA duty 4 / slices 2, 48 kS/s (R3.23, R3.27). */
extern const lr2021_am_table_t lr2021_am_w12_920;

/* The instantaneous frequency IQ_* follows right at a rate (R3.25): min(20 kHz, 0.31 x rate). */
float lr2021_iq_freq_limit(uint32_t rate);

/* IQ_POLAR samples from n complex samples (iq: I then Q, floats), in place of a waveform's own:
 * - scaled so `peak` is full scale (peak <= 0: this block's largest magnitude);
 * - magnitudes under r_min (a share of full scale; 0.2 did best, R3.27) pushed out to r_min, the phase carried
 *   across the dip in a straight line, so the phase steps stay under the limit;
 * - each magnitude mapped through the table's inverse to the input that gives it, and the table's phase shift
 *   taken off (tab NULL: magnitudes as they are, to 127).
 * out gets 2n int8 (I then Q). A dip that spans two blocks is handled as two: pass whole blocks where it matters. */
void lr2021_polar_iq8(const float *iq, size_t n, float peak, float r_min, const lr2021_am_table_t *tab, int8_t *out);
/* IQ_PHASE samples: the phase of iq at magnitude 100 (any magnitude does; zeros send the last phase). */
void lr2021_phase_iq8(const float *iq, size_t n, int8_t *out);

#ifdef __cplusplus
}
#endif

#endif
