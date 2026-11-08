// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * APRS on receive: Bell 202 AFSK (1200 baud, mark 1200 Hz, space 2200 Hz) and AX.25 frames in an FM receiver's
 * audio (Hz of deviation), as they stream, with the transmitter's tones measured. The bench analyzer's method,
 * written from the Bell 202 and AX.25 2.2 definitions:
 *
 * - each tone mixed to 0 Hz and averaged over one bit; the decision mark minus g x space for seven weights g (for
 *   twist), each with a bit clock PLL nudged at each decision change, NRZI (no change = 1), HDLC flags, bit
 *   unstuffing, LSB-first octets, the X.25 CRC; a frame any weight decodes counts once
 * - for each frame, over its flags and body: the deviation (peak = 99.9th percentile of |audio|, max, rms), each
 *   tone's level (the median at the centers of bits inside a run of one tone) and frequency (its phase's drift along
 *   those runs), the twist (mark over space, dB)
 *
 * Frames that unstuff to whole octets and fail the CRC are counted (the weight-1 decoder's only, so noise between
 * flags isn't counted seven times). Portable C11, one thread; about 300 KB (the audio and each decoder's bits, about
 * 4 s), from the allocator given.
 */
#ifndef RDOF_SDR_AFSKRX_H
#define RDOF_SDR_AFSKRX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RDOF_AFSK_INFO 260

typedef struct {
    char src[12], dst[12];
    char digis[96];              /* "WIDE1-1,WIDE2-1*" */
    uint8_t control, pid;
    char info[RDOF_AFSK_INFO];   /* the information field (bytes as they came; NUL-terminated, cut at 259) */
    int info_len;
    double t0, t1;               /* the frame's first and last bit, s from the first sample */
    int txdelay_flags;           /* back-to-back flags before it */
    float decision_gain;
    float peak_dev_hz, max_dev_hz, rms_dev_hz;
    float mark_dev_hz, space_dev_hz, twist_db; /* NAN: no run of the tone */
    float mark_hz, space_hz;
} rdof_afsk_frame_t;

typedef struct rdof_afskrx rdof_afskrx_t;

/* fs: the audio's rate (8-48 kS/s). NULL: no memory, or a rate it can't take. */
rdof_afskrx_t *rdof_afskrx_new(double fs, void *(*alloc)(size_t), void (*dealloc)(void *));
void rdof_afskrx_free(rdof_afskrx_t *a);
void rdof_afskrx_feed(rdof_afskrx_t *a, const float *x, int n);
/* The next decoded frame (false: none yet). */
bool rdof_afskrx_take(rdof_afskrx_t *a, rdof_afsk_frame_t *out);
/* Frames between flags that failed the CRC so far. */
long rdof_afskrx_fcs_bad(const rdof_afskrx_t *a);

#endif
