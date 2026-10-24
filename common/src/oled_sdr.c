// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The OLED's SDR screen and the board's oled settings (rdof/oled_sdr.h). */
#include "rdof/oled_sdr.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/jsonp.h"

#define COLS (RDOF_OLED_W / 6)
#define FLAGS_Y 8 /* the network and the flags in the top 16 rows: a two-color panel's yellow band */
#define BADGES_Y 20
#define CALLS_Y 29
#define BOTTOM_Y (RDOF_OLED_H - 8)

char *rdof_oled_badge(const char *id, char out[8])
{
    size_t n = strlen(id), d = n;
    while (d > 0 && isdigit((unsigned char)id[d - 1]))
        d--;
    if (d == n) {
        snprintf(out, 8, "%.3s", id);
        return out;
    }
    long v = strtol(id + d, NULL, 10);
    bool sdr = d >= 3 && toupper((unsigned char)id[0]) == 'S' && toupper((unsigned char)id[1]) == 'D' &&
               toupper((unsigned char)id[2]) == 'R';
    if (sdr || d == 0)
        snprintf(out, 8, "%ld", v % 100000);
    else
        snprintf(out, 8, "%c%ld", toupper((unsigned char)id[0]), v % 100000);
    return out;
}

int rdof_oled_bars(int dbm)
{
    if (dbm == 0)
        return -1;
    static const int floors[4] = {-55, -65, -75, -85};
    for (int i = 0; i < 4; i++)
        if (dbm >= floors[i])
            return 4 - i;
    return 0;
}

/* MHz with the trailing zeros off: 461.0875, 467.375, 446. */
static void mhz(double hz, char *out, size_t cap)
{
    snprintf(out, cap, "%.4f", hz / 1e6);
    char *e = out + strlen(out);
    while (e > out && e[-1] == '0')
        *--e = 0;
    if (e > out && e[-1] == '.')
        e[-1] = 0;
}

size_t rdof_oled_tuner_line(const rdof_oled_sdr_t *s, char *out, size_t cap)
{
    if (!cap)
        return 0;
    if (!s->receiving) {
        snprintf(out, cap, "starting");
        return strlen(out);
    }
    char c[24];
    mhz(s->center_hz, c, sizeof c);
    int n = snprintf(out, cap, "%s %.0fk", c, s->sample_rate / 1e3);
    if (n > 0 && (size_t)n < cap && isfinite(s->cpu_load))
        snprintf(out + n, cap - (size_t)n, " cpu %.0f%%", s->cpu_load * 100);
    return strlen(out);
}

/* Wi-Fi: bars by signal, or a small x without it. Returns the x after it. */
static int wifi_icon(rdof_frame_buf_t *f, int x, int y, bool up, int dbm)
{
    int n = up ? rdof_oled_bars(dbm) : -1;
    if (up && n < 0)
        n = 4; /* up, signal not known */
    if (n < 0) {
        for (int i = 0; i < 5; i++) {
            rdof_oled_pixel(f, x + i, y + 2 + i, true);
            rdof_oled_pixel(f, x + 4 - i, y + 2 + i, true);
        }
        return x + 9;
    }
    static const int h[4] = {2, 3, 5, 7};
    for (int i = 0; i < 4; i++) {
        int bx = x + 2 * i;
        if (i < n)
            for (int yy = y + 7 - h[i]; yy <= y + 6; yy++)
                rdof_oled_pixel(f, bx, yy, true);
        else
            rdof_oled_pixel(f, bx, y + 6, true);
    }
    return x + 10;
}

/* A health flag: on a filled box when up, struck through when down, left out for none. */
static int flag(rdof_frame_buf_t *f, int x, int y, const char *label, rdof_oled_flag_t v)
{
    if (v == RDOF_OLED_NONE)
        return x;
    int w = 6 * (int)strlen(label) + 1;
    if (v == RDOF_OLED_UP) {
        rdof_oled_rect(f, x, y, w, 8, true, true);
        rdof_oled_text(f, x + 1, y, label, true, false);
    } else {
        rdof_oled_text(f, x + 1, y, label, false, false);
        rdof_oled_hline(f, x, x + w - 1, y + 3, true);
    }
    return x + w + 1;
}

/* A channel badge: inverted while hearing, struck through out of the window. */
static int badge(rdof_frame_buf_t *f, int x, int y, const char *label, bool on, bool in)
{
    int w = 6 * (int)strlen(label) + 1;
    if (on)
        rdof_oled_rect(f, x, y, w, 8, true, true);
    rdof_oled_text(f, x + 1, y, label, on, false);
    if (!in)
        rdof_oled_hline(f, x, x + w - 1, y + 3, true);
    return x + w + 1;
}

/* text cut to the screen's width */
static void line(rdof_frame_buf_t *f, int y, const char *s)
{
    char b[COLS + 1];
    snprintf(b, sizeof b, "%s", s);
    rdof_oled_text(f, 0, y, b, false, false);
}

void rdof_oled_render_sdr(rdof_frame_buf_t *f, const rdof_oled_sdr_t *s)
{
    char b[64], lb[8];
    rdof_oled_clear(f);
    /* the network */
    int x = wifi_icon(f, 0, 0, s->wifi, s->wifi_dbm);
    snprintf(b, sizeof b, "%s%s", s->ap ? "AP " : "", s->ip && s->ip[0] ? s->ip : "no address");
    rdof_oled_text(f, x, 0, b, false, false);
    /* health and the time */
    x = flag(f, 0, FLAGS_Y, "MQ", s->mqtt);
    x = flag(f, x, FLAGS_Y, "HB", s->hblink);
    x = flag(f, x, FLAGS_Y, "SDR", s->sdr);
    flag(f, x, FLAGS_Y, "HAT", s->radio);
    if (s->clock && s->clock[0]) /* its last gap off the edge */
        rdof_oled_text(f, RDOF_OLED_W + 1 - rdof_oled_text_width(s->clock, false), FLAGS_Y, s->clock, false, false);
    rdof_oled_hline(f, 0, RDOF_OLED_W - 1, 18, true);
    rdof_oled_hline(f, 0, RDOF_OLED_W - 1, BOTTOM_Y - 2, true);

    /* RX: a badge per channel; the "RX" label goes when the badges need the room */
    int width = 0, heard = 0;
    for (int i = 0; i < s->nch; i++) {
        width += 6 * (int)strlen(rdof_oled_badge(s->ch[i].id, lb)) + 2;
        heard += s->ch[i].active;
    }
    x = 0;
    if (!s->nch)
        rdof_oled_text(f, 0, BADGES_Y, "RX no channels", false, false);
    else if (width + 13 <= RDOF_OLED_W)
        x = rdof_oled_text(f, 0, BADGES_Y, "RX", false, false) + 1;
    for (int i = 0; i < s->nch; i++) {
        rdof_oled_badge(s->ch[i].id, lb);
        if (x + 6 * (int)strlen(lb) + 1 > RDOF_OLED_W - (i < s->nch - 1 ? 7 : 0)) {
            rdof_oled_text(f, x, BADGES_Y, "+", false, false);
            break;
        }
        x = badge(f, x, BADGES_Y, lb, s->ch[i].active, s->ch[i].in);
    }

    /* the calls being heard, else the front end's error, else the channels */
    const int lines = (BOTTOM_Y - 2 - CALLS_Y) / 8;
    if (heard) {
        int shown = heard <= lines ? heard : lines - 1, k = 0;
        for (int i = 0; i < s->nch && k < shown; i++) {
            const rdof_oled_channel_t *c = &s->ch[i];
            if (!c->active)
                continue;
            snprintf(b, sizeof b, "%2s %c%lu<%lu", rdof_oled_badge(c->id, lb), c->group ? 'T' : 'P',
                     (unsigned long)c->dst, (unsigned long)c->src);
            line(f, CALLS_Y + 8 * k++, b);
        }
        if (heard > lines) {
            snprintf(b, sizeof b, "+%d more", heard - shown);
            line(f, CALLS_Y + 8 * (lines - 1), b);
        }
    } else if (s->error && s->error[0]) {
        size_t n = strlen(s->error);
        for (int k = 0; k < lines && (size_t)k * COLS < n; k++)
            line(f, CALLS_Y + 8 * k, s->error + (size_t)k * COLS);
    } else {
        int shown = s->nch <= lines ? s->nch : lines - 1;
        for (int i = 0; i < shown; i++) {
            const rdof_oled_channel_t *c = &s->ch[i];
            char m[24];
            mhz(c->freq, m, sizeof m);
            snprintf(b, sizeof b, "%2s %s %s", rdof_oled_badge(c->id, lb), m, c->name ? c->name : "");
            line(f, CALLS_Y + 8 * i, b);
        }
        if (s->nch > lines) {
            snprintf(b, sizeof b, "+%d more", s->nch - shown);
            line(f, CALLS_Y + 8 * (lines - 1), b);
        }
    }

    /* the bottom row: a roamer's TX, else the tuner */
    if (s->has_tx) {
        x = rdof_oled_text(f, 0, BOTTOM_Y, "TX", s->tx_active, false);
        rdof_oled_text(f, x + 3, BOTTOM_Y, s->tx_line && s->tx_line[0] ? s->tx_line : "idle", false, true);
    } else {
        rdof_oled_tuner_line(s, b, sizeof b);
        line(f, BOTTOM_Y, b);
    }
}

/* ── the settings ── */

#define OLED_NODES 1200
#define OLED_STRS 8192

typedef struct {
    rdof_jdoc_t doc;
    rdof_jnode_t nodes[OLED_NODES];
    char strs[OLED_STRS];
} oled_doc_t;

/* A whole number (or one written as a string, as config.c takes it). */
static bool whole(const rdof_jdoc_t *d, int obj, const char *key, int *out, char *error, size_t cap)
{
    int i = obj >= 0 ? rdof_jget(d, obj, key) : -1;
    if (i < 0)
        return true;
    const rdof_jnode_t *n = rdof_jnode(d, i);
    if (n->type == RDOF_JNUM && n->num == floor(n->num) && fabs(n->num) < 1e9) {
        *out = (int)n->num;
        return true;
    }
    if (n->type == RDOF_JSTR && n->str[0]) {
        char *e;
        long v = strtol(n->str, &e, 10);
        if (!*e) {
            *out = (int)v;
            return true;
        }
    }
    snprintf(error, cap, "oled: %s isn't a whole number", key);
    return false;
}

static bool truthy(const rdof_jdoc_t *d, int obj, const char *key, bool dflt)
{
    int i = obj >= 0 ? rdof_jget(d, obj, key) : -1;
    if (i < 0)
        return dflt;
    const rdof_jnode_t *n = rdof_jnode(d, i);
    switch (n->type) {
    case RDOF_JBOOL:
        return n->b;
    case RDOF_JNUM:
        return n->num != 0;
    case RDOF_JSTR:
        return n->str[0] != 0;
    case RDOF_JARR:
    case RDOF_JOBJ:
        return n->count > 0;
    default:
        return false;
    }
}

bool rdof_oled_settings_parse(const char *text, size_t len, rdof_oled_settings_t *out, char *error, size_t cap)
{
    char dummy[8];
    if (!error || !cap) {
        error = dummy;
        cap = sizeof dummy;
    }
    error[0] = 0;
    *out = (rdof_oled_settings_t){.address = 0x3C, .refresh_s = 0.5, .sda = -1, .scl = -1};
    oled_doc_t *d = malloc(sizeof *d);
    char *clean = d ? malloc(len + 1) : NULL;
    if (!clean) {
        free(d);
        snprintf(error, cap, "no memory");
        return false;
    }
    size_t n = rdof_jsonc_strip(text, len, clean);
    d->doc = (rdof_jdoc_t){.nodes = d->nodes, .max = OLED_NODES, .buf = d->strs, .cap = OLED_STRS};
    int root = rdof_jparse(&d->doc, clean, n);
    bool ok = false;
    if (root < 0) {
        snprintf(error, cap, "JSON: %s", d->doc.error);
    } else if (rdof_jnode(&d->doc, root)->type != RDOF_JOBJ) {
        snprintf(error, cap, "the settings are a JSON object");
    } else {
        int o = rdof_jget(&d->doc, root, "oled");
        const rdof_jnode_t *on = o >= 0 ? rdof_jnode(&d->doc, o) : NULL;
        out->set = on != NULL;
        out->on = on && on->type != RDOF_JNULL && !(on->type == RDOF_JBOOL && !on->b);
        ok = true;
        if (out->on && on->type == RDOF_JOBJ) {
            int i2c = rdof_jget(&d->doc, o, "i2c");
            if (i2c >= 0 && rdof_jnode(&d->doc, i2c)->type != RDOF_JOBJ)
                i2c = -1;
            int src = rdof_jget(&d->doc, o, "address") >= 0 ? o : i2c;
            ok = whole(&d->doc, src, "address", &out->address, error, cap) &&
                 whole(&d->doc, o, "sda", &out->sda, error, cap) && whole(&d->doc, o, "scl", &out->scl, error, cap);
            out->sh1106 = rdof_jget(&d->doc, o, "sh1106") >= 0 ? truthy(&d->doc, o, "sh1106", false)
                                                                : truthy(&d->doc, i2c, "sh1106", false);
            out->rotate180 = rdof_jget(&d->doc, o, "rotate180") >= 0 ? truthy(&d->doc, o, "rotate180", false)
                                                                      : truthy(&d->doc, i2c, "rotate180", false);
            int r = rdof_jget(&d->doc, o, "refresh_s");
            if (ok && r >= 0) {
                const rdof_jnode_t *rn = rdof_jnode(&d->doc, r);
                char *e = NULL;
                double v = rn->type == RDOF_JNUM ? rn->num : rn->type == RDOF_JSTR ? strtod(rn->str, &e) : NAN;
                if (isnan(v) || (e && (*e || !rn->str[0]))) {
                    snprintf(error, cap, "oled: refresh_s isn't a number");
                    ok = false;
                } else {
                    out->refresh_s = v;
                }
            }
            if (ok && (out->address < 0x03 || out->address > 0x77)) {
                snprintf(error, cap, "oled: address %d isn't a 7-bit I2C address", out->address);
                ok = false;
            }
            if (ok && (out->sda < -1 || out->sda > 48 || out->scl < -1 || out->scl > 48 ||
                       (out->sda >= 0 && out->sda == out->scl))) {
                snprintf(error, cap, "oled: sda and scl are two GPIOs 0 to 48");
                ok = false;
            }
        }
    }
    free(clean);
    free(d);
    return ok;
}
