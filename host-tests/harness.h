// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* Shared by the scenario tests: an expected-output cursor and a fake modem port. */
#ifndef HARNESS_H
#define HARNESS_H

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_radio.h"
#include "rdof/modem.h"
#include "rdof/split.h"

extern int checks, failures;

struct expect_out {
    const char *kind, *text;
};

typedef struct {
    const char *name;
    const struct expect_out *outs;
    int nouts, next, bad;
} expect_t;

static inline void expect(expect_t *e, const char *kind, const char *text)
{
    checks++;
    if (e->next >= e->nouts) {
        failures++;
        if (e->bad++ < 3)
            printf("FAIL %s: extra output %s %s\n", e->name, kind, text);
        e->next++;
        return;
    }
    const struct expect_out *o = &e->outs[e->next++];
    if (strcmp(o->kind, kind) || strcmp(o->text, text)) {
        failures++;
        if (e->bad++ < 3)
            printf("FAIL %s output %d:\n  want %s %.300s\n  got  %s %.300s\n", e->name, e->next - 1, o->kind, o->text,
                   kind, text);
    }
}

static inline void expect_done(expect_t *e)
{
    checks++;
    if (e->next != e->nouts) {
        failures++;
        printf("FAIL %s: %d outputs, want %d\n", e->name, e->next, e->nouts);
        if (e->next < e->nouts)
            printf("  next wanted: %s %.200s\n", e->outs[e->next].kind, e->outs[e->next].text);
    }
}

static inline void hexs(const uint8_t *b, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++)
        snprintf(out + 2 * i, 3, "%02x", b[i]);
    out[2 * n] = 0;
}

static inline size_t unhexs(const char *s, uint8_t *out)
{
    size_t n = strlen(s) / 2;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(s + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
    return n;
}

/* A modem on a fake radio: every write recorded as "w", the reply pumped straight back. */
typedef struct {
    fake_radio_t radio; /* a split: the TX half */
    rdof_modem_t modem;
    expect_t *e;
    double clock;
#if RDOF_FEATURE_REPEATER
    rdof_split_t *split; /* non-NULL: the modem is a split duplex pair, radio and rx its halves (rig_init_split) */
    fake_radio_t rx;
#endif
} fake_rig_t;

#if RDOF_FEATURE_REPEATER
/* As the scenarios were recorded: each half's bytes (recorded "w0" / "w1") to its fake, its replies back,
 * until nothing moves; then what goes upstream into the modem. */
static inline void split_pump(fake_rig_t *r)
{
    uint8_t buf[4096];
    char hex[2 * sizeof buf + 1];
    size_t n;
    bool moved = true;
    while (moved) {
        moved = false;
        for (int i = 0; i < 2; i++) {
            fake_radio_t *f = i ? &r->rx : &r->radio;
            if ((n = rdof_split_take(r->split, i, buf, sizeof buf)) > 0) {
                hexs(buf, n, hex);
                expect(r->e, i ? "w1" : "w0", hex);
                fake_write(f, buf, n);
                moved = true;
            }
            while ((n = fake_take(f, buf, sizeof buf)) > 0) {
                rdof_split_from_half(r->split, i, buf, n, r->clock);
                moved = true;
            }
        }
    }
    while ((n = rdof_split_take(r->split, -1, buf, sizeof buf)) > 0)
        rdof_modem_feed(&r->modem, buf, n);
}
#endif

static inline void rig_pump(fake_rig_t *r)
{
    uint8_t buf[4096];
    size_t n;
#if RDOF_FEATURE_REPEATER
    if (r->split) {
        split_pump(r);
        return;
    }
#endif
    while ((n = fake_take(&r->radio, buf, sizeof buf)) > 0)
        rdof_modem_feed(&r->modem, buf, n);
}

static inline bool rig_write(void *ctx, const uint8_t *d, size_t n)
{
    fake_rig_t *r = ctx;
    char hex[1100];
    hexs(d, n < 512 ? n : 512, hex);
    expect(r->e, "w", hex);
#if RDOF_FEATURE_REPEATER
    if (r->split)
        rdof_split_upstream(r->split, d, n, r->clock);
    else
#endif
        fake_write(&r->radio, d, n);
    rig_pump(r);
    return true;
}

static inline bool rig_wait(void *ctx, int ms)
{
    fake_rig_t *r = ctx;
    return r->modem.reply_ready;
}

static inline double rig_clock(void *ctx)
{
    return ((fake_rig_t *)ctx)->clock;
}

static inline void rig_init(fake_rig_t *r, const char *kind, expect_t *e)
{
    memset(r, 0, sizeof *r);
    fake_init(&r->radio, kind);
    r->e = e;
    r->clock = 1000.0;
    rdof_modem_port_t port = {rig_write, rig_wait, NULL, rig_clock, NULL, r};
    rdof_modem_init(&r->modem, &port);
}

#if RDOF_FEATURE_REPEATER
/* the scenarios' split clocks */
static inline void rig_init_split(fake_rig_t *r, rdof_split_t *sp, expect_t *e)
{
    rig_init(r, "odr_split", e);
    fake_init(&r->rx, "odr_split");
    r->split = sp;
    r->radio.clock = r->rx.clock = &r->clock;
    r->radio.clock_offset_us = 1234567, r->radio.clock_rate = 20e-6;
    r->rx.clock_offset_us = 4000000000.0, r->rx.clock_rate = -15e-6;
    rdof_split_init(sp, r->clock, NULL);
    split_pump(r);
}

/* The split's own timers, after the clock moved. */
static inline void rig_tick(fake_rig_t *r)
{
    if (!r->split)
        return;
    rdof_split_tick(r->split, r->clock);
    split_pump(r);
}
#endif

static inline rdof_radio_kind_t kind_of(const char *s)
{
    if (!strcmp(s, "radio_hotspot") || !strncmp(s, "opengd77", 8) || !strncmp(s, "odr", 3))
        return RDOF_RADIO_HOTSPOT;
    if (!strcmp(s, "mmdvm_hs"))
        return RDOF_RADIO_MMDVM_HS;
    if (!strcmp(s, "split"))
        return RDOF_RADIO_SPLIT;
    return RDOF_RADIO_MMDVM;
}

#endif
