// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
// NTP packets and per-server maths, chrony-style (see netclock_ntp.h). Ported
// from smartlaps' ntp_client.c, less the sockets.
#include "netclock_ntp.h"

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "netclock_select.h"

#define LAMBDA_MIN_US  500.0    // floor on root distance (our own stamping jitter)
#define DELAY_SLACK_US 10000.0  // a sample may exceed its server's recent minimum round trip by this...
#define DELAY_SLACK_K  0.5      // ...or this fraction of it, whichever is more; slower ones were queued
                                // (Wi-Fi TX busy, bufferbloat), and queueing is one-sided: it biases the offset
#define NTP_UNIX_DELTA 2208988800LL

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t be64(const uint8_t *p)
{
    return (uint64_t)be32(p) << 32 | be32(p + 4);
}

int64_t nc_ntp_to_us(uint64_t ts)
{
    uint32_t sec = (uint32_t)(ts >> 32), frac = (uint32_t)ts;
    int64_t s = (int64_t)sec - NTP_UNIX_DELTA;
    if (!(sec & 0x80000000u)) {
        s += 4294967296LL;
    }
    return s * 1000000 + (int64_t)(((uint64_t)frac * 1000000) >> 32);
}

uint64_t nc_us_to_ntp(int64_t us)
{
    int64_t s = us / 1000000, f = us % 1000000;
    uint32_t sec = (uint32_t)(s + NTP_UNIX_DELTA);  // wraps into era 1 after 2036, as the wire does
    uint32_t frac = (uint32_t)((((uint64_t)f << 32) + 999999) / 1000000);
    return (uint64_t)sec << 32 | frac;
}

int nc_parse_servers(const char *list, nc_src_t *out, int max)
{
    int k = 0;
    const char *p = list ? list : "";
    while (*p && k < max) {
        while (*p == ' ' || *p == ',' || *p == ';' || *p == '\t') {
            p++;
        }
        size_t len = strcspn(p, " ,;\t");
        if (!len) {
            break;
        }
        char tok[40];
        snprintf(tok, sizeof(tok), "%.*s", (int)(len < sizeof(tok) - 1 ? len : sizeof(tok) - 1), p);
        p += len;
        size_t tl = strlen(tok);
        bool pool = tl >= 12 && strcmp(tok + tl - 12, "pool.ntp.org") == 0 && !isdigit((unsigned char)tok[0]);
        for (int i = 0; i < (pool ? 3 : 1) && k < max; i++) {
            memset(&out[k], 0, sizeof(out[k]));
            out[k].mark = '-';
            if (pool) {
                snprintf(out[k].name, sizeof(out[k].name), "%d.%.36s", i, tok);
            } else {
                snprintf(out[k].name, sizeof(out[k].name), "%s", tok);
            }
            k++;
        }
    }
    return k;
}

void nc_ntp_request(uint8_t p[48], uint64_t cookie, uint32_t poll_s)
{
    memset(p, 0, 48);
    p[0] = (0 << 6) | (4 << 3) | 3;  // LI 0, version 4, client
    uint8_t poll = 0;
    for (uint32_t v = poll_s; v > 1; v >>= 1) {
        poll++;
    }
    p[2] = poll;
    // A random transmit timestamp (chrony does the same): the reply must echo
    // it, which rejects stale and spoofed replies and leaks nothing.
    for (int i = 0; i < 8; i++) {
        p[40 + i] = (uint8_t)(cookie >> (56 - 8 * i));
    }
}

nc_reply_t nc_src_reply(nc_src_t *s, const uint8_t *p, int len, int64_t t4, bool iburst)
{
    if (len < 48 || (p[0] & 7) != 4 || be64(p + 24) != s->cookie || !s->t1 || s->got) {
        return NC_REPLY_IGNORED;
    }
    uint8_t li = p[0] >> 6, stratum = p[1];
    s->got = true;  // answered (counts for reach even if unusable, as in chrony)
    if (stratum == 0) {
        s->kod = true;
        s->kod_skip = NC_NTP_KOD_SKIP;
        return NC_REPLY_KOD;
    }
    if (li == 3 || stratum > 15) {
        return NC_REPLY_UNUSABLE;  // server not synchronized
    }
    int64_t t2 = nc_ntp_to_us(be64(p + 32)), t3 = nc_ntp_to_us(be64(p + 40));
    double delay = (double)(t4 - s->t1) - (double)(t3 - t2);
    if (delay < 0) {
        delay = 0;
    }
    if (delay > NC_NTP_MAX_DELAY || t3 < t2) {
        return NC_REPLY_UNUSABLE;
    }
    s->kod = false;
    s->stratum = stratum;
    s->refid = be32(p + 12);
    s->last_rx_us = t4;
    s->misses = 0;
    if (iburst && s->tm && s->delay_us <= delay) {
        return NC_REPLY_KEPT;
    }
    s->tm = s->t1 + (t4 - s->t1) / 2;  // symmetric paths: the server's midpoint is ours
    s->u = t2 + (t3 - t2) / 2;
    s->delay_us = delay;
    s->root_delay_us = be32(p + 4) / 65536.0 * 1e6;
    s->root_disp_us = be32(p + 8) / 65536.0 * 1e6;
    return NC_REPLY_SAMPLE;
}

void nc_src_poll_end(nc_src_t *s)
{
    s->reach = (uint8_t)(s->reach << 1 | (s->got ? 1 : 0));
    if (s->t1 && !s->got) {
        s->misses++;
    }
}

int nc_combine(nc_src_t *src, int n, nc_offset_fn offset, void *ctx, int64_t *t_us, int64_t *utc_us, bool *had_sample)
{
    nc_cand_t c[NETCLOCK_MAX_SRC] = {0};
    char mark[NETCLOCK_MAX_SRC];
    int64_t base = 0;
    bool have_base = false;
    if (n > NETCLOCK_MAX_SRC) {
        n = NETCLOCK_MAX_SRC;
    }
    for (int i = 0; i < n; i++) {
        nc_src_t *s = &src[i];
        bool ok = s->got && s->t1 && s->last_rx_us >= s->t1;  // a usable reply to this poll's request
        s->spike = false;
        if (ok) {
            // chrony-style delay filter: compare with the quickest recent round trip.
            s->h_delay[s->dh] = (float)s->delay_us;
            s->dh = (s->dh + 1) % NC_NTP_HIST;
            if (s->dn < NC_NTP_HIST) {
                s->dn++;
            }
            s->min_delay_us = s->delay_us;
            for (int j = 0; j < s->dn; j++) {
                s->min_delay_us = fmin(s->min_delay_us, s->h_delay[j]);
            }
            s->spike = s->delay_us > s->min_delay_us + fmax(DELAY_SLACK_US, DELAY_SLACK_K * s->min_delay_us);
            ok = !s->spike;
        }
        c[i].ok = ok;
        if (!ok) {
            continue;
        }
        // Offset vs the model, for the jitter history (once there is a model).
        double om;
        if (offset && offset(ctx, s->tm, s->u, &om)) {
            s->off_us = om;
            s->h_off[s->hh] = (float)om;
            s->hh = (s->hh + 1) % NC_NTP_HIST;
            if (s->hn < NC_NTP_HIST) {
                s->hn++;
            }
        }
        double m = 0, v = 0;
        for (int j = 0; j < s->hn; j++) {
            m += s->h_off[j];
        }
        m = s->hn ? m / s->hn : 0;
        for (int j = 0; j < s->hn; j++) {
            v += (s->h_off[j] - m) * (s->h_off[j] - m);
        }
        s->jitter_us = s->hn > 1 ? sqrt(v / (s->hn - 1)) : 0;
        s->lambda_us = fmax(LAMBDA_MIN_US, s->delay_us / 2 + s->root_delay_us / 2 + s->root_disp_us + s->jitter_us);
        // Raw offsets (UTC - local) relative to the first server: the local
        // clock is common, so they compare directly (samples ms apart).
        int64_t raw = s->u - s->tm;
        if (!have_base) {
            base = raw;
            have_base = true;
        }
        c[i].off_us = (double)(raw - base);
        c[i].lambda_us = s->lambda_us;
    }
    double off = 0;
    int bi = nc_select(c, n, mark, &off);
    for (int i = 0; i < n; i++) {
        src[i].mark = !src[i].resolved || src[i].dup ? '-' : src[i].spike ? '~' : mark[i];
    }
    *had_sample = have_base;
    if (bi < 0) {
        return -1;
    }
    int64_t t = src[bi].tm;
    *t_us = t;
    *utc_us = t + base + (int64_t)llround(off);
    return bi;
}

void nc_src_view(const nc_src_t *s, int64_t now_us, netclock_src_view_t *v)
{
    *v = (netclock_src_view_t){
        .mark = s->mark, .stratum = s->stratum, .reach = s->reach,
        .last_rx_s = s->last_rx_us ? (int32_t)((now_us - s->last_rx_us) / 1000000) : -1,
        .offset_ms = (float)(s->off_us / 1000), .delay_ms = (float)(s->delay_us / 1000),
        .lambda_ms = (float)(s->lambda_us / 1000), .jitter_ms = (float)(s->jitter_us / 1000),
        .min_delay_ms = (float)(s->min_delay_us / 1000),
        .root_delay_ms = (float)(s->root_delay_us / 1000), .root_disp_ms = (float)(s->root_disp_us / 1000),
        .np = (uint8_t)s->hn, .kod = s->kod,
    };
    snprintf(v->name, sizeof(v->name), "%s", s->name);
    if (s->resolved) {
        snprintf(v->ip, sizeof(v->ip), "%u.%u.%u.%u", s->ip[0], s->ip[1], s->ip[2], s->ip[3]);
    }
    if (s->stratum == 1) {
        for (int k = 0; k < 4; k++) {
            char ch = (char)(s->refid >> (24 - 8 * k));
            v->refid[k] = isprint((unsigned char)ch) ? ch : 0;
        }
    } else if (s->stratum > 1) {
        snprintf(v->refid, sizeof(v->refid), "%" PRIu32 ".%" PRIu32 ".%" PRIu32 ".%" PRIu32, s->refid >> 24,
                 (s->refid >> 16) & 255, (s->refid >> 8) & 255, s->refid & 255);
    }
}
