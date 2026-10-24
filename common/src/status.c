// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#include "rdof/status.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/json.h"
#include "rdof/tracker.h"

/* s into out[cap], cut to cap - 1 bytes (as snprintf "%s" cuts it, without its truncation warning). */
static void copy_str(char *out, size_t cap, const char *s)
{
    if (!cap)
        return;
    size_t n = strlen(s);
    if (n >= cap)
        n = cap - 1;
    memcpy(out, s, n);
    out[n] = '\0';
}

/* Into t (64 bytes: the longest is 24), then out as snprintf would cut it. */
static void repr(double v, char t[64])
{
    char e[40];
    int p;
    if (isnan(v) || isinf(v)) {
        strcpy(t, isnan(v) ? "nan" : v > 0 ? "inf" : "-inf");
        return;
    }
    if (v == 0) {
        strcpy(t, signbit(v) ? "-0.0" : "0.0");
        return;
    }
    for (p = 1; p < 17; p++) { /* the fewest significant digits that read back the same (17 always do) */
        snprintf(e, sizeof e, "%.*e", p - 1, v);
        if (strtod(e, NULL) == v)
            break;
    }
    snprintf(e, sizeof e, "%.*e", p - 1, v);
    int exp = atoi(strchr(e, 'e') + 1);
    if (exp >= -4 && exp < 16) {
        int decimals = p - 1 - exp;
        snprintf(t, 64, "%.*f", decimals > 0 ? decimals : 0, v);
        if (!strchr(t, '.'))
            strcat(t, ".0");
        return;
    }
    /* d.ddde+XX */
    snprintf(t, 64, "%.*se%c%02d", (int)(strchr(e, 'e') - e), e, exp < 0 ? '-' : '+', exp < 0 ? -exp : exp);
}

void rdof_format_repr(double v, char *out, size_t cap)
{
    char t[64];
    repr(v, t);
    copy_str(out, cap, t);
}

static void freq_text(uint32_t f, char *out, size_t cap)
{
    snprintf(out, cap, "%.5f", f / 1e6);
}

static void location(rdof_json_t *j, const rdof_location_t *loc)
{
    char v[40];
    if (!loc)
        return;
    rdof_format_repr(loc->latitude, v, sizeof v);
    rdof_json_raw(j, "latitude", v);
    rdof_format_repr(loc->longitude, v, sizeof v);
    rdof_json_raw(j, "longitude", v);
    rdof_json_int(j, "height", loc->height_m);
}

void rdof_roamer_status_json(const rdof_roamer_status_t *s, char *out, size_t cap)
{
    char at[32], f[24];
    rdof_json_t j;
    rdof_iso_time(s->at, at);
    freq_text(s->freq, f, sizeof f);
    rdof_json_init(&j, out, cap);
    rdof_json_open(&j, NULL);
    rdof_json_str(&j, "state", "online");
    rdof_json_str(&j, "at", at);
    rdof_json_str(&j, "frequency", f);
    rdof_json_int(&j, "colorCode", s->cc);
    rdof_json_bool(&j, "listening", s->listening);
    rdof_json_bool(&j, "scanning", s->scanning);
    rdof_json_bool(&j, "radio", s->radio);
    rdof_json_bool(&j, "hblink", s->hblink);
    rdof_json_bool(&j, "onAir", s->on_air);
    rdof_json_close(&j);
}

void rdof_roamer_receiver_json(const char *name, uint32_t idle_freq, int idle_cc, const char *device,
                             const rdof_location_t *loc, char *out, size_t cap)
{
    char f[24];
    rdof_json_t j;
    freq_text(idle_freq, f, sizeof f);
    rdof_json_init(&j, out, cap);
    rdof_json_open(&j, NULL);
    rdof_json_open(&j, "Receiver");
    rdof_json_str(&j, "name", name);
    rdof_json_str(&j, "frequency", f);
    rdof_json_int(&j, "colorCode", idle_cc);
    rdof_json_str(&j, "source", "roaming");
    rdof_json_str(&j, "device", device);
    location(&j, loc);
    rdof_json_close(&j);
    rdof_json_close(&j);
}

void rdof_repeater_status_json(const rdof_repeater_status_t *s, char *out, size_t cap)
{
    char at[32], f[24], t[24];
    rdof_json_t j;
    rdof_iso_time(s->at, at);
    freq_text(s->rx, f, sizeof f);
    freq_text(s->tx, t, sizeof t);
    rdof_json_init(&j, out, cap);
    rdof_json_open(&j, NULL);
    rdof_json_str(&j, "state", "online");
    rdof_json_str(&j, "at", at);
    rdof_json_str(&j, "frequency", f);
    rdof_json_str(&j, "txFrequency", t);
    rdof_json_int(&j, "colorCode", s->cc);
    rdof_json_bool(&j, "radio", s->radio);
    rdof_json_bool(&j, "hblink", s->hblink);
    rdof_json_bool(&j, "onAir", s->on_air);
    rdof_json_open(&j, "slots");
    for (int n = 1; n <= 2; n++)
        if (s->slot_configured[n])
            rdof_json_bool(&j, n == 1 ? "1" : "2", s->slot_busy[n]);
    rdof_json_close(&j);
    rdof_json_close(&j);
}

void rdof_repeater_receiver_json(const char *name, uint32_t rx, uint32_t tx, int cc, const char *device,
                               const rdof_location_t *loc, int inbound_slot, char *out, size_t cap)
{
    char f[24], t[24];
    rdof_json_t j;
    freq_text(rx, f, sizeof f);
    freq_text(tx, t, sizeof t);
    rdof_json_init(&j, out, cap);
    rdof_json_open(&j, NULL);
    rdof_json_open(&j, "Receiver");
    rdof_json_str(&j, "name", name);
    rdof_json_str(&j, "frequency", f);
    rdof_json_str(&j, "txFrequency", t);
    rdof_json_int(&j, "colorCode", cc);
    rdof_json_str(&j, "source", "repeater");
    rdof_json_str(&j, "device", device);
    location(&j, loc);
    if (inbound_slot) {
        rdof_json_open(&j, "sfr");
        rdof_json_int(&j, "inboundSlot", inbound_slot);
        rdof_json_int(&j, "outboundSlot", 3 - (long long)inbound_slot);
        rdof_json_close(&j);
    }
    rdof_json_close(&j);
    rdof_json_close(&j);
}
