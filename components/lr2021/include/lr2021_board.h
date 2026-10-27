// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * What a board puts around the chip, as far as the chip drives it: the clock source, the RF switch on its DIOs,
 * which DIO is wired to the host as an IRQ, the range its front end passes. The host's own pins (SPI, BUSY,
 * NRESET, a front-end supply) are the HAL's.
 *
 * Boards are named (lr2021_board_find) or given as text (lr2021_board_parse), so a setting or argument can carry one.
 */
#ifndef LR2021_BOARD_H
#define LR2021_BOARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LR2021_BOARD_RFSW_MAX 4

typedef struct {
    const char *name;
    int tcxo_tune;            /* SetTcxoMode's supply code (0: 1.6 V to 7: 3.3 V), -1 for a crystal */
    uint32_t tcxo_start_us;   /* the TCXO's start-up time */
    /* The RF switch: DIOs set to RF_SWITCH, each high in the chip modes its mask names (DS 6.6.2: bit 0 standby,
     * 1 RX, 2 TX, 3 TX HP, 4 RX HF, 5 TX HF). A front end with a PA band: rfsw_pa inside pa_lo_hz..pa_hi_hz,
     * rfsw_bypass outside it. */
    int rfsw_n;
    uint8_t rfsw_dio[LR2021_BOARD_RFSW_MAX];
    uint8_t rfsw_pa[LR2021_BOARD_RFSW_MAX];
    uint8_t rfsw_bypass[LR2021_BOARD_RFSW_MAX];
    double pa_lo_hz, pa_hi_hz;
    int irq_dio;              /* the DIO wired to the host's IRQ pin (5 to 11), -1 for none */
    uint8_t rx_boost;         /* SetRxPath's boost (0 to 7; the datasheet recommends 0) */
    double freq_min, freq_max; /* what the board's front end passes, Hz */
    uint8_t pa_lf_duty, pa_lf_slices; /* SetPaConfig for the LF PA (DS table 7-19) */
    double tx_power_max;      /* dBm */
} lr2021_board_t;

/* A bare chip: no TCXO, no RF switch, no IRQ DIO, the datasheet's LF and HF ranges. */
extern const lr2021_board_t lr2021_board_generic;
/* Meshnology W12: an LR2021 with a crystal, a GC1109 front end (860-930 MHz PA, a 902-928 MHz SAW after its LNA),
 * DIO9-11 its CTX / CPS / CSD, DIO7 the IRQ (the host's GPIO7). */
extern const lr2021_board_t lr2021_board_w12;

/* A board by name ("generic", "w12"), NULL if none. */
const lr2021_board_t *lr2021_board_find(const char *name);

/* A board from text: a base board's name, then changes, comma or space separated:
 *   "w12", "generic tcxo=3 irq=7", "generic rfsw=9:4:4,rfsw=10:4:0,pa=860e6:930e6,range=150e6:960e6"
 * keys: tcxo=<code>[:<start us>], irq=<dio>, rfsw=<dio>:<pa mask>:<bypass mask> (repeated: one a DIO; the first
 * replaces the base's switch), pa=<lo>:<hi>, range=<min>:<max>, boost=<0-7>, power=<dBm>. A key's value is a
 * number (C syntax: 0x.. for hex). The name strings point into storage owned by out. -1 and a message in err
 * (errlen bytes) for bad text. */
typedef struct {
    lr2021_board_t b;
    char name[32];
} lr2021_board_buf_t;
int lr2021_board_parse(const char *text, lr2021_board_buf_t *out, char *err, size_t errlen);

/* The front end's RF switch masks for a frequency (PA band or bypass) into dio / modes; returns how many. */
int lr2021_board_rfsw(const lr2021_board_t *b, double hz, uint8_t *dio, uint8_t *modes);

#ifdef __cplusplus
}
#endif

#endif
