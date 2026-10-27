// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* Board profiles (lr2021_board.h). */
#include "lr2021_board.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const lr2021_board_t lr2021_board_generic = {
    .name = "generic",
    .tcxo_tune = -1,
    .irq_dio = -1,
    .freq_min = 150e6, /* the LF path, RFI_LF (DS 1.1); the HF path (1.5-2.5 GHz) is untried for raw I/Q */
    .freq_max = 960e6,
    .pa_lf_duty = 4, /* DS table 7-19: about +14 dBm */
    .pa_lf_slices = 2,
    .tx_power_max = 22,
};

/* The GC1109 (its datasheet's table 4): CSD 0 off; CSD 1 CTX 0 receive (LNA); CTX 1 CPS 0 transmit bypass; CTX 1
 * CPS 1 transmit (PA). DIO9 CTX, DIO10 CPS, DIO11 CSD; masks: bit 1 RX, bit 2 TX. */
const lr2021_board_t lr2021_board_w12 = {
    .name = "w12",
    .tcxo_tune = -1,
    .rfsw_n = 3,
    .rfsw_dio = {9, 10, 11},
    .rfsw_pa = {0x04, 0x04, 0x06},
    .rfsw_bypass = {0x04, 0x00, 0x06},
    .pa_lo_hz = 860e6,
    .pa_hi_hz = 930e6,
    .irq_dio = 7,
    .freq_min = 150e6, /* a 902-928 MHz SAW follows the LNA: 461 MHz arrives about 46 dB down */
    .freq_max = 960e6,
    .pa_lf_duty = 4,
    .pa_lf_slices = 2,
    .tx_power_max = 22,
};

static const lr2021_board_t *const boards[] = {&lr2021_board_generic, &lr2021_board_w12};

const lr2021_board_t *lr2021_board_find(const char *name)
{
    for (size_t i = 0; i < sizeof boards / sizeof boards[0]; i++)
        if (!strcmp(boards[i]->name, name))
            return boards[i];
    return NULL;
}

/* A number, a ':' and another, or one alone: how many were read (0 on bad text). */
static int nums(const char *s, double *v, int max)
{
    int n = 0;
    while (n < max) {
        char *end;
        v[n] = strtod(s, &end);
        if (end == s)
            return 0;
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
            v[n] = (double)strtol(s, &end, 16);
        n++;
        s = end;
        if (*s != ':')
            break;
        s++;
    }
    return *s ? 0 : n;
}

int lr2021_board_parse(const char *text, lr2021_board_buf_t *out, char *err, size_t errlen)
{
    char tok[64];
    const char *s = text;
    bool first = true, rfsw_new = true;
    while (*s) {
        while (*s == ',' || isspace((unsigned char)*s))
            s++;
        size_t n = 0;
        while (s[n] && s[n] != ',' && !isspace((unsigned char)s[n]))
            n++;
        if (!n)
            break;
        if (n >= sizeof tok) {
            snprintf(err, errlen, "board: \"%.*s...\" too long", 16, s);
            return -1;
        }
        memcpy(tok, s, n);
        tok[n] = 0;
        s += n;
        char *eq = strchr(tok, '=');
        if (first && !eq) {
            const lr2021_board_t *b = lr2021_board_find(tok);
            if (!b) {
                snprintf(err, errlen, "board: no board named \"%s\"", tok);
                return -1;
            }
            out->b = *b;
            snprintf(out->name, sizeof out->name, "%.31s", tok);
            out->b.name = out->name;
            first = false;
            continue;
        }
        if (first) {
            out->b = lr2021_board_generic;
            snprintf(out->name, sizeof out->name, "custom");
            out->b.name = out->name;
            first = false;
        }
        if (!eq) {
            snprintf(err, errlen, "board: \"%s\" isn't key=value", tok);
            return -1;
        }
        *eq = 0;
        const char *key = tok;
        double v[3];
        int k = nums(eq + 1, v, 3);
        lr2021_board_t *b = &out->b;
        if (!strcmp(key, "tcxo") && (k == 1 || k == 2) && v[0] >= -1 && v[0] <= 7) {
            b->tcxo_tune = (int)v[0];
            b->tcxo_start_us = k == 2 ? (uint32_t)v[1] : 5000;
        } else if (!strcmp(key, "irq") && k == 1 && (v[0] == -1 || (v[0] >= 5 && v[0] <= 11))) {
            b->irq_dio = (int)v[0];
        } else if (!strcmp(key, "rfsw") && k == 3 && v[0] >= 5 && v[0] <= 11) {
            if (rfsw_new) {
                b->rfsw_n = 0;
                rfsw_new = false;
            }
            if (b->rfsw_n >= LR2021_BOARD_RFSW_MAX) {
                snprintf(err, errlen, "board: at most %d rfsw", LR2021_BOARD_RFSW_MAX);
                return -1;
            }
            b->rfsw_dio[b->rfsw_n] = (uint8_t)v[0];
            b->rfsw_pa[b->rfsw_n] = (uint8_t)v[1];
            b->rfsw_bypass[b->rfsw_n++] = (uint8_t)v[2];
        } else if (!strcmp(key, "pa") && k == 2) {
            b->pa_lo_hz = v[0];
            b->pa_hi_hz = v[1];
        } else if (!strcmp(key, "range") && k == 2 && v[0] < v[1]) {
            b->freq_min = v[0];
            b->freq_max = v[1];
        } else if (!strcmp(key, "boost") && k == 1 && v[0] >= 0 && v[0] <= 7) {
            b->rx_boost = (uint8_t)v[0];
        } else if (!strcmp(key, "power") && k == 1) {
            b->tx_power_max = v[0];
        } else {
            snprintf(err, errlen, "board: bad \"%s=%s\"", key, eq + 1);
            return -1;
        }
    }
    if (first) {
        snprintf(err, errlen, "board: empty");
        return -1;
    }
    return 0;
}

int lr2021_board_rfsw(const lr2021_board_t *b, double hz, uint8_t *dio, uint8_t *modes)
{
    const bool pa = b->pa_hi_hz > 0 && hz >= b->pa_lo_hz && hz <= b->pa_hi_hz;
    for (int i = 0; i < b->rfsw_n; i++) {
        dio[i] = b->rfsw_dio[i];
        modes[i] = pa ? b->rfsw_pa[i] : b->rfsw_bypass[i];
    }
    return b->rfsw_n;
}
