// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * An analog FM test signal: the deviation, sample by sample, for a transmitter to put on its carrier.
 *
 * Two parts are added together. The subaudio runs from the key-up to the key-down: a CTCSS tone or a DCS code, at its
 * own deviation, with the end the standard asks for (a CTCSS reverse burst, the DCS turn-off tone). The program is
 * the main audio: segments one after another (tones, a sweep, DTMF digits, FSK or AFSK from bytes, noise, silence,
 * PCM), at a peak deviation set for the whole program, optionally pre-emphasized. Carrier with the subaudio alone
 * goes before and after it (lead_s, tail_s), and the key-up and key-down are short raised-cosine ramps of the
 * envelope.
 *
 * DCS (TIA-603's digital squelch): a 23-bit Golay (23,12) word, the 9 bits of the octal code, then 100, then 11
 * check bits (generator 0xC75), sent LSB first at 134.4 bits/s, over and over. Normal: a 1 is a positive deviation;
 * inverted: every bit flipped. The turn-off is a 134.4 Hz tone.
 *
 * Portable C11; the PCM ring is one writer's and one reader's (C11 atomics), the rest one thread's.
 */
#ifndef RDOF_FMGEN_H
#define RDOF_FMGEN_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_FMG_SEGS 32        /* program segments */
#define RDOF_FMG_TONES 4        /* tones a segment plays at once */
#define RDOF_FMG_DATA 1024      /* bytes for FSK segments and DTMF digits, the program's */
#define RDOF_FMG_RAMP_S 0.002   /* the envelope's ramp, key-up and key-down */
#define RDOF_FMG_DCS_BAUD 134.4
#define RDOF_FMG_DCS_OFF_HZ 134.4

enum {
    RDOF_FMG_SILENCE = 0,
    RDOF_FMG_TONE,              /* hz[0..ntones): the level split among them */
    RDOF_FMG_SWEEP,             /* hz[0] to hz[1], linear or logarithmic */
    RDOF_FMG_DTMF,              /* data[off..off+n): digits 0-9 * # A-D, each `seconds` on, `gap_s` off */
    RDOF_FMG_FSK,               /* data[off..) nbits bits, continuous phase: hz[0] mark (1), hz[1] space (0) */
    RDOF_FMG_NOISE,             /* white, level is its rms */
    RDOF_FMG_PCM,               /* the PCM stream, at pcm_rate, until pcm_samples are played */
};

enum { RDOF_FMG_SUB_NONE = 0, RDOF_FMG_SUB_CTCSS, RDOF_FMG_SUB_DCS };

typedef struct {
    uint8_t kind;
    bool log;                   /* sweep: logarithmic */
    bool lsb;                   /* fsk: each byte's lowest bit first (AX.25); else highest */
    bool nrzi;                  /* fsk: a 0 changes the tone, a 1 keeps it (AX.25 / Bell 202 packet) */
    uint8_t ntones;
    float seconds;              /* tone, sweep, noise, silence: its length; dtmf: each digit's tone */
    float gap_s;                /* dtmf: the silence after each digit */
    float level;                /* peak as a fraction of dev_hz (noise: rms), 0-1 */
    float hz[RDOF_FMG_TONES];
    float baud;                 /* fsk */
    uint16_t off;               /* dtmf, fsk: where its digits or bytes start in data */
    uint32_t n;                 /* dtmf: digits; fsk: bits */
} rdof_fmg_seg_t;

typedef struct {
    float dev_hz;               /* the program's peak deviation at level 1 */
    float preemph_us;           /* 0: flat; else 6 dB an octave from 1/(2 pi tau), 0 dB at 1 kHz (750: TIA-603) */
    uint8_t sub;                /* RDOF_FMG_SUB_* */
    bool dcs_inv;
    uint16_t dcs;               /* the code's value: octal 023 is 023 in C */
    float ctcss_hz;
    float sub_dev_hz;
    float sub_end_s;            /* the end: a CTCSS reverse burst (180 degrees) or the DCS turn-off, 0 none */
    float lead_s, tail_s;       /* the subaudio alone before and after the program */
    uint16_t repeat;            /* the program plays this many times (0: once) */
    uint8_t nseg;
    rdof_fmg_seg_t seg[RDOF_FMG_SEGS];
    uint8_t data[RDOF_FMG_DATA];
    float pcm_rate;             /* PCM segments: samples a second */
    uint32_t pcm_samples;       /* PCM segments: how many the stream has in all (each PCM segment plays them all) */
    uint32_t seed;              /* noise; 0 a fixed one */
} rdof_fmg_cfg_t;

typedef struct {
    rdof_fmg_cfg_t c;
    double fs;
    uint64_t pos, total, ramp, lead, prog, tail, end; /* samples; prog is one play of the program */
    /* the program */
    int seg;
    uint16_t play;
    uint64_t seg_at, seg_len;   /* the segment's first sample, its length */
    uint32_t ph[RDOF_FMG_TONES];
    double sweep_ph;
    uint32_t fsk_bit;           /* fsk: the bit playing */
    bool fsk_tone;              /* fsk: mark */
    uint32_t rng;
    float pre_x1;               /* pre-emphasis */
    double pre_k, pre_g;
    /* the subaudio */
    uint32_t sub_ph;
    uint32_t dcs_word;
    double dcs_t;               /* DCS: bits into the word, fractional */
    float lp[4];                /* DCS's low-pass, two first-order stages each side */
    float lp_a;
    /* PCM */
    int16_t *ring;
    uint32_t cap;               /* a power of 2 */
    atomic_uint_fast32_t w, r;
    atomic_bool eof;
    double pcm_t;               /* PCM samples played, fractional */
    uint32_t pcm_played;
    int16_t pcm_prev, pcm_cur;
    uint32_t underruns;         /* PCM samples that weren't there in time (played as silence) */
    bool done;
} rdof_fmg_t;

/* The cfg's sense (NULL) or why not, in plain words. */
const char *rdof_fmg_check(const rdof_fmg_cfg_t *c);
/* fs: the output rate. ring: PCM samples to buffer (a power of 2, at least 1024), its memory the caller's (NULL when
 * the program has no PCM segment). Returns 0, or -1 (rdof_fmg_check). */
int rdof_fmg_init(rdof_fmg_t *g, const rdof_fmg_cfg_t *c, double fs, int16_t *ring, uint32_t ring_samples);
/* The whole transmission's length, seconds (from rdof_fmg_check'ed settings; PCM's from pcm_samples). */
double rdof_fmg_seconds(const rdof_fmg_cfg_t *c);
/* The largest deviation it can ask for, Hz (with pre-emphasis, at a segment's highest tone). */
double rdof_fmg_peak_hz(const rdof_fmg_cfg_t *c);
/* The next n samples: deviation (Hz) and envelope (0..1). Returns how many of them are keyed (the rest are zeros:
 * the end). */
int rdof_fmg_render(rdof_fmg_t *g, float *dev_hz, float *env, int n);
bool rdof_fmg_done(const rdof_fmg_t *g);

/* PCM, the writer's side: room in the ring now (samples); push up to that many; the end of the stream. */
uint32_t rdof_fmg_pcm_space(rdof_fmg_t *g);
uint32_t rdof_fmg_pcm_push(rdof_fmg_t *g, const int16_t *s, uint32_t n);
void rdof_fmg_pcm_end(rdof_fmg_t *g);

/* The 23-bit DCS word for a code (bit 0 goes first), and the standard codes (TIA-603's 104 and their count). */
uint32_t rdof_dcs_word(unsigned code);
extern const uint16_t rdof_dcs_codes[];
extern const int rdof_dcs_ncodes;
/* The CTCSS tones (Hz), and their count. */
extern const float rdof_ctcss_tones[];
extern const int rdof_ctcss_ntones;
/* DTMF: a digit's low and high tone; false for one that isn't. */
bool rdof_dtmf_tones(char digit, float *lo, float *hi);

/* sin(2 pi phase / 2^32): a 1024-entry table, linear between entries (error under 5e-6). */
float rdof_fm_sin(uint32_t phase);

#ifdef __cplusplus
}
#endif

#endif
