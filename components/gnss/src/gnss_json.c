// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
// The status as JSON. Pure C: built in host tests too.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "gnss.h"

typedef struct {
    char *b;
    size_t cap, n;
    bool over;
} jw_t;

static void put(jw_t *w, const char *fmt, ...)
{
    if (w->over) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(w->b + w->n, w->cap - w->n, fmt, ap);
    va_end(ap);
    if (k < 0 || (size_t)k >= w->cap - w->n) {
        w->over = true;
        return;
    }
    w->n += (size_t)k;
}

static void fnum(jw_t *w, const char *k, double v, int dec)
{
    if (isfinite(v)) {
        put(w, ",\"%s\":%.*f", k, dec, v);
    } else {
        put(w, ",\"%s\":null", k);
    }
}

static void str(jw_t *w, const char *k, const char *v)
{
    put(w, ",\"%s\":\"", k);
    for (const char *p = v; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            put(w, "\\%c", c);
        } else if (c < 0x20 || c > 0x7e) {
            put(w, "\\u%04x", c);
        } else {
            put(w, "%c", c);
        }
    }
    put(w, "\"");
}

size_t gnss_json(const gnss_status_t *s, char *buf, size_t cap)
{
    if (!cap) {
        return 0;
    }
    jw_t w = {buf, cap, 0, false};
    const gnss_nmea_t *n = &s->nmea;
    const char *fix = !n->valid && n->quality <= 0 ? "none" : n->mode == 3 ? "3d" : n->mode == 2 ? "2d" : "fix";
    put(&w, "{\"running\":%s,\"found\":%s,\"rx_gpio\":%d,\"tx_gpio\":%d,\"baud\":%d", s->running ? "true" : "false",
        s->found ? "true" : "false", s->rx_gpio, s->tx_gpio, s->baud);
    str(&w, "fix", fix);
    put(&w, ",\"quality\":%d,\"sats_used\":%d,\"sats_in_view\":%d", n->quality, n->sats_used, gnss_nmea_in_view(n));
    put(&w, ",\"constellations\":{");
    for (int i = 0; i < n->n_talkers; i++) {
        put(&w, "%s\"%s\":%d", i ? "," : "", n->talkers[i].talker, n->talkers[i].in_view);
    }
    put(&w, "}");
    fnum(&w, "hdop", n->hdop, 2);
    fnum(&w, "pdop", n->pdop, 2);
    fnum(&w, "vdop", n->vdop, 2);
    if (n->pos_ok) {
        fnum(&w, "lat", n->lat, 7);
        fnum(&w, "lon", n->lon, 7);
    } else {
        put(&w, ",\"lat\":null,\"lon\":null");
    }
    fnum(&w, "alt_m", n->alt_ok ? n->alt_m : NAN, 1);
    fnum(&w, "geoid_m", n->geoid_m, 1);
    fnum(&w, "speed_kmh", n->speed_kn * 1.852, 1);
    fnum(&w, "course", n->course, 1);
    int64_t t;
    if (gnss_nmea_utc(n, &t)) {
        put(&w, ",\"utc_s\":%lld", (long long)t);
    } else {
        put(&w, ",\"utc_s\":null");
    }
    if (n->time_ok && n->date_ok) {
        put(&w, ",\"utc\":\"%04d-%02d-%02dT%02d:%02d:%02.0fZ\"", n->year, n->month, n->day, n->hour, n->min,
            floor(n->sec));
    } else {
        put(&w, ",\"utc\":null");
    }
    str(&w, "txt", n->txt);
    put(&w, ",\"last_s\":%ld,\"bytes\":%lu,\"lines\":%lu,\"sentences\":%lu,\"bad\":%lu,\"other\":%lu",
        (long)s->last_s, (unsigned long)s->bytes, (unsigned long)s->lines, (unsigned long)n->sentences,
        (unsigned long)n->bad, (unsigned long)n->other);
    put(&w, ",\"bursts\":%lu,\"no_gap\":%lu,\"overflows\":%lu,\"tries\":%lu}", (unsigned long)s->burst.bursts,
        (unsigned long)s->burst.no_gap, (unsigned long)s->overflows, (unsigned long)s->tries);
    return w.over ? 0 : w.n;
}

size_t gnss_json_append(const gnss_status_t *s, char *json, size_t len, size_t cap)
{
    if (!s->found || len < 2 || json[len - 1] != '}' || len >= cap) {
        return len;
    }
    const gnss_nmea_t *n = &s->nmea;
    char t[200];
    jw_t w = {t, sizeof(t), 0, false};
    const char *fix = !n->valid && n->quality <= 0 ? "none" : n->mode == 3 ? "3d" : n->mode == 2 ? "2d" : "fix";
    put(&w, "%s\"gnss\":{\"fix\":\"%s\",\"sats\":%d", json[len - 2] == '{' ? "" : ",", fix, n->sats_used);
    if (n->pos_ok) {
        fnum(&w, "lat", n->lat, 7);
        fnum(&w, "lon", n->lon, 7);
        fnum(&w, "alt_m", n->alt_ok ? n->alt_m : NAN, 1);
        fnum(&w, "hdop", n->hdop, 2);
    }
    put(&w, "}}");
    if (w.over || len - 1 + w.n >= cap) {
        return len;
    }
    memcpy(json + len - 1, t, w.n + 1);
    return len - 1 + w.n;
}
