// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#include "gnss_nmea.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FIELDS 24

void gnss_nmea_init(gnss_nmea_t *n)
{
    memset(n, 0, sizeof(*n));
    n->hdop = n->pdop = n->vdop = NAN;
    n->lat = n->lon = n->alt_m = n->geoid_m = n->speed_kn = n->course = NAN;
}

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

// A decimal number filling the whole field; false for an empty field or anything else in it.
static bool num(const char *f, double *out)
{
    if (!*f) {
        return false;
    }
    char *end;
    double v = strtod(f, &end);
    if (*end || !isfinite(v)) {
        return false;
    }
    *out = v;
    return true;
}

static bool inum(const char *f, int *out)
{
    double v;
    if (!num(f, &v) || v != floor(v) || fabs(v) > 1e6) {
        return false;
    }
    *out = (int)v;
    return true;
}

// hhmmss[.ss]
static bool parse_time(gnss_nmea_t *n, const char *f)
{
    double v;
    if (strlen(f) < 6 || !num(f, &v) || v < 0) {
        return false;
    }
    int hms = (int)floor(v);
    int h = hms / 10000, m = hms / 100 % 100;
    double s = v - (hms / 100) * 100;
    if (h > 23 || m > 59 || s >= 61) {  // 60: a leap second
        return false;
    }
    n->hour = h;
    n->min = m;
    n->sec = s;
    n->time_ok = true;
    return true;
}

static bool set_date(gnss_nmea_t *n, int y, int m, int d)
{
    if (m < 1 || m > 12 || d < 1 || d > 31 || y < 1980) {
        return false;
    }
    n->year = y;
    n->month = m;
    n->day = d;
    n->date_ok = true;
    return true;
}

// ddmm.mmmm + hemisphere: degrees, south and west negative.
static bool coord(const char *f, const char *hemi, bool lat, double *out)
{
    double v;
    if (!num(f, &v) || v < 0 || strlen(hemi) != 1) {
        return false;
    }
    double deg = floor(v / 100), min = v - deg * 100;
    if (min >= 60 || deg > (lat ? 90 : 180)) {
        return false;
    }
    double d = deg + min / 60;
    char h = hemi[0];
    if (lat ? (h != 'N' && h != 'S') : (h != 'E' && h != 'W')) {
        return false;
    }
    *out = h == 'S' || h == 'W' ? -d : d;
    return true;
}

static bool position(gnss_nmea_t *n, char **f, int at)
{
    double lat, lon;
    if (!coord(f[at], f[at + 1], true, &lat) || !coord(f[at + 2], f[at + 3], false, &lon)) {
        return false;
    }
    n->lat = lat;
    n->lon = lon;
    return true;
}

static gnss_nmea_talker_t *talker(gnss_nmea_t *n, const char *t)
{
    for (int i = 0; i < n->n_talkers; i++) {
        if (!strcmp(n->talkers[i].talker, t)) {
            return &n->talkers[i];
        }
    }
    if (n->n_talkers == GNSS_NMEA_TALKERS) {
        return NULL;
    }
    gnss_nmea_talker_t *p = &n->talkers[n->n_talkers++];
    memcpy(p->talker, t, 2);
    p->talker[2] = 0;
    return p;
}

// $--RMC,time,status,lat,N,lon,E,speed,course,ddmmyy,magvar,E[,mode[,navstatus]]
static gnss_nmea_kind_t rmc(gnss_nmea_t *n, char **f, int nf)
{
    if (nf < 10) {
        return GNSS_NMEA_BAD;
    }
    if (*f[1] && !parse_time(n, f[1])) {
        return GNSS_NMEA_BAD;
    }
    n->valid = f[2][0] == 'A';
    if (strlen(f[9]) == 6) {
        int ddmmyy = atoi(f[9]);
        set_date(n, 2000 + ddmmyy % 100, ddmmyy / 100 % 100, ddmmyy / 10000);
    }
    n->pos_ok = n->valid && position(n, f, 3);
    double v;
    if (num(f[7], &v)) {
        n->speed_kn = v;
    }
    if (num(f[8], &v)) {
        n->course = v;
    }
    return GNSS_NMEA_RMC;
}

// $--GGA,time,lat,N,lon,E,quality,sats,hdop,alt,M,geoid,M,age,station
static gnss_nmea_kind_t gga(gnss_nmea_t *n, char **f, int nf)
{
    if (nf < 12) {
        return GNSS_NMEA_BAD;
    }
    if (*f[1] && !parse_time(n, f[1])) {
        return GNSS_NMEA_BAD;
    }
    int q = 0, s = 0;
    inum(f[6], &q);
    n->quality = q;
    if (inum(f[7], &s)) {
        n->sats_used = s;
    }
    double v;
    if (num(f[8], &v)) {
        n->hdop = v;
    }
    n->pos_ok = q > 0 && position(n, f, 2);
    n->alt_ok = q > 0 && num(f[9], &n->alt_m);
    if (num(f[11], &v)) {
        n->geoid_m = v;
    }
    return GNSS_NMEA_GGA;
}

// $--GSA,mode1,mode2,sv x 12,pdop,hdop,vdop[,system]
static gnss_nmea_kind_t gsa(gnss_nmea_t *n, char **f, int nf)
{
    if (nf < 18) {
        return GNSS_NMEA_BAD;
    }
    int m = 0;
    inum(f[2], &m);
    n->mode = m;
    double v;
    if (num(f[15], &v)) {
        n->pdop = v;
    }
    if (num(f[16], &v)) {
        n->hdop = v;
    }
    if (num(f[17], &v)) {
        n->vdop = v;
    }
    return GNSS_NMEA_GSA;
}

// $--GSV,messages,number,in view,(sv,elev,az,snr) x up to 4[,signal]
static gnss_nmea_kind_t gsv(gnss_nmea_t *n, const char *t, char **f, int nf)
{
    int view;
    if (nf < 4 || !inum(f[3], &view) || view < 0) {
        return GNSS_NMEA_BAD;
    }
    gnss_nmea_talker_t *p = talker(n, t);
    if (p) {
        p->in_view = view;
    }
    return GNSS_NMEA_GSV;
}

// $--ZDA,time,dd,mm,yyyy,zone h,zone m
static gnss_nmea_kind_t zda(gnss_nmea_t *n, char **f, int nf)
{
    int d, m, y;
    if (nf < 5 || !parse_time(n, f[1]) || !inum(f[2], &d) || !inum(f[3], &m) || !inum(f[4], &y)) {
        return GNSS_NMEA_BAD;
    }
    set_date(n, y, m, d);
    return GNSS_NMEA_ZDA;
}

// $--TXT,total,number,id,text
static gnss_nmea_kind_t txt(gnss_nmea_t *n, char **f, int nf)
{
    if (nf < 5) {
        return GNSS_NMEA_BAD;
    }
    snprintf(n->txt, sizeof(n->txt), "%s", f[4]);
    return GNSS_NMEA_TXT;
}

gnss_nmea_kind_t gnss_nmea_line(gnss_nmea_t *n, const char *line, size_t len)
{
    while (len && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
        len--;
    }
    char buf[GNSS_NMEA_MAX_LINE + 1];
    // $ + 5 address characters + * + 2 checksum digits at least
    if (len < 9 || len > GNSS_NMEA_MAX_LINE || line[0] != '$' || line[len - 3] != '*') {
        n->bad++;
        return GNSS_NMEA_BAD;
    }
    uint8_t sum = 0;
    for (size_t i = 1; i < len - 3; i++) {
        sum ^= (uint8_t)line[i];
    }
    int hi = hexval(line[len - 2]), lo = hexval(line[len - 1]);
    if (hi < 0 || lo < 0 || sum != (uint8_t)(hi << 4 | lo)) {
        n->bad++;
        return GNSS_NMEA_BAD;
    }
    memcpy(buf, line + 1, len - 4);
    buf[len - 4] = 0;
    char *f[MAX_FIELDS];
    int nf = 0;
    for (char *p = buf;;) {
        if (nf == MAX_FIELDS) {
            break;
        }
        f[nf++] = p;
        char *c = strchr(p, ',');
        if (!c) {
            break;
        }
        *c = 0;
        p = c + 1;
    }
    if (strlen(f[0]) != 5) {
        n->bad++;
        return GNSS_NMEA_BAD;
    }
    char t[3] = {f[0][0], f[0][1], 0};
    const char *type = f[0] + 2;
    gnss_nmea_kind_t k = !strcmp(type, "RMC") ? rmc(n, f, nf)
                       : !strcmp(type, "GGA") ? gga(n, f, nf)
                       : !strcmp(type, "GSA") ? gsa(n, f, nf)
                       : !strcmp(type, "GSV") ? gsv(n, t, f, nf)
                       : !strcmp(type, "ZDA") ? zda(n, f, nf)
                       : !strcmp(type, "TXT") ? txt(n, f, nf)
                                              : GNSS_NMEA_OTHER;
    if (k == GNSS_NMEA_BAD) {
        n->bad++;
    } else if (k == GNSS_NMEA_OTHER) {
        n->other++;
        n->sentences++;
    } else {
        n->sentences++;
    }
    return k;
}

int64_t gnss_days_from_civil(int y, int m, int d)
{
    // Howard Hinnant's days_from_civil (public domain algorithm)
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

bool gnss_nmea_utc(const gnss_nmea_t *n, int64_t *utc_s)
{
    // RMC's A: its date and time are the fix's. A receiver without one gives its own clock's (an L76K: 1980-01-06),
    // and an old date is a week-number rollover.
    if (!n->time_ok || !n->date_ok || !n->valid || n->year < GNSS_NMEA_MIN_YEAR) {
        return false;
    }
    *utc_s = gnss_days_from_civil(n->year, n->month, n->day) * 86400 + n->hour * 3600 + n->min * 60 +
             (int64_t)floor(n->sec);
    return true;
}

int gnss_nmea_in_view(const gnss_nmea_t *n)
{
    int s = 0;
    for (int i = 0; i < n->n_talkers; i++) {
        s += n->talkers[i].in_view;
    }
    return s;
}

void gnss_burst_init(gnss_burst_t *b)
{
    memset(b, 0, sizeof(*b));
}

void gnss_burst_bytes(gnss_burst_t *b, int64_t end_us, size_t n, int baud)
{
    if (!n || baud <= 0) {
        return;
    }
    const int64_t first = end_us - (int64_t)((double)n * 10e6 / baud);
    if (!b->start_us || first - b->last_us > GNSS_BURST_GAP_US) {
        b->start_us = first;
        b->told = false;
        b->bursts++;
    }
    b->last_us = end_us;
}

bool gnss_burst_time(gnss_burst_t *b, const gnss_nmea_t *n, int64_t *start_us, int64_t *utc_s)
{
    int64_t s;
    if (!b->start_us || !gnss_nmea_utc(n, &s)) {
        return false;
    }
    if (b->told) {
        if (s != b->told_s) {
            b->no_gap++;  // a new second inside one burst: no gap to time it by
            b->told_s = s;
        }
        return false;
    }
    b->told = true;
    b->told_s = s;
    *start_us = b->start_us;
    *utc_s = s;
    return true;
}
