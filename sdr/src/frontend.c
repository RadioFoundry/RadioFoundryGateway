// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* What's portable about the SDR front ends (rdof/sdr/frontend.h): names, formats, the settings check. */
#include "rdof/sdr/frontend.h"
#include "rdof/sdr/features.h"

#if RDOF_FEATURE_SDR

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "rdof/sdr/iq.h"

static const char *const names[RDOF_FE_TYPES] = {"rtlsdr", "lr2021", "sim", "sx1255"};

const char *rdof_fe_type_name(rdof_fe_type_t t) { return (unsigned)t < RDOF_FE_TYPES ? names[t] : "?"; }

int rdof_fe_type_parse(const char *s)
{
    if (!s || !*s)
        return RDOF_FE_RTLSDR;
    for (int t = 0; t < RDOF_FE_TYPES; t++)
        if (!strcmp(s, names[t]))
            return t;
    return -1;
}

int rdof_fe_sample_bytes(rdof_fe_format_t f) { return f == RDOF_FE_CS16 ? 4 : 2; }

uint8_t rdof_fe_fill_byte(rdof_fe_format_t f) { return f == RDOF_FE_CU8 ? 0x80 : 0; }

void rdof_fe_convert(rdof_fe_format_t f, const void *raw, size_t n, rdof_cf_t *out)
{
    switch (f) {
    case RDOF_FE_CU8: rdof_iq_from_cu8((const uint8_t *)raw, n, out); break;
    case RDOF_FE_CS8: rdof_iq_from_cs8((const int8_t *)raw, n, out); break;
    case RDOF_FE_CS16: rdof_iq_from_cs16((const int16_t *)raw, n, out); break;
    }
}

int rdof_fe_check(const rdof_fe_caps_t *c, double rate, const double *freq_hz, int n, char *error, size_t cap)
{
    if (c->n_rates > 0) {
        bool ok = false;
        for (int i = 0; i < c->n_rates; i++)
            ok |= fabs(c->rates[i] - rate) < 0.5;
        if (!ok) {
            int o = snprintf(error, cap, "the %s takes sample_rate", c->kind);
            for (int i = 0; i < c->n_rates && o > 0 && (size_t)o < cap; i++)
                o += snprintf(error + o, cap - (size_t)o, "%s%.0f", i ? (i + 1 == c->n_rates ? " or " : ", ") : " ",
                              c->rates[i]);
            return -1;
        }
    } else if (rate < c->rate_min - 0.5 || rate > c->rate_max + 0.5) {
        snprintf(error, cap, "the %s takes sample_rate %.0f to %.0f (got %.0f)", c->kind, c->rate_min, c->rate_max,
                 rate);
        return -1;
    }
    for (int i = 0; i < n; i++)
        if (freq_hz[i] < c->freq_min || freq_hz[i] > c->freq_max) {
            snprintf(error, cap, "the %s tunes %.3f to %.3f MHz: %.5f MHz is out of its range", c->kind,
                     c->freq_min / 1e6, c->freq_max / 1e6, freq_hz[i] / 1e6);
            return -1;
        }
    return 0;
}

#endif
