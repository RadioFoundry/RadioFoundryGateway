// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The bench API (docs/spec/bench-api.md), version 1, under /api/bench/v1: what a host tool needs to use this board
 * as an SDR bench in place of the authors' bench daemon. HTTP + JSON for discovery, status, sessions and the spectrum; WebSocket
 * streams for the rest. This file: info, status, sdr, spectrum, sessions and leases, the bursts stream, and the
 * I/Q stream (a receiver channel's 48 kS/s output, or the wideband samples; pre-roll and a level trigger).
 *
 * Streams are sent from one task of their own (bursts_task): the DSP cores only copy a burst into a queue, and only
 * while a bursts stream is open, so with nothing open the receive path pays a load and a branch per burst. A burst
 * the queue has no room for is counted, and each stream gets a "drop" frame for it.
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/message_buffer.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "netclock.h"
#include "rdof/benchline.h"
#include "rdof/jsonp.h"
#include "rdof/pdu.h"
#include "rdof/sdr/demod.h"

#if CONFIG_RDOF_BENCH

static const char *TAG = "bench";

#define BASE "/api/bench/v1"
#define MAX_STREAMS 4
#define MAX_SESSIONS 4
#define MAX_LEASE_FREQS 4
#define BURST_QUEUE 64
#define SESSION_TTL_MAX_S 3600

/* ── sessions and leases (spec section 4) ─────────────────────────────────── */

typedef struct {
    bool used;
    char id[12];        /* "s-" and 4 hex digits */
    char label[32];
    double ttl_s;
    int64_t renewed_us;
    /* one lease per session for now: what it covers */
    bool leased;
    double freqs[MAX_LEASE_FREQS];
    int nfreqs;
    bool rx, tx, exclusive;
} session_t;

/* ── streams ──────────────────────────────────────────────────────────────── */

typedef enum { ST_NONE = 0, ST_BURSTS, ST_IQ, ST_SPECTRUM, ST_CALLS } stream_kind_t;

typedef struct {
    stream_kind_t kind;
    int fd;
    int64_t opened_us;
    bool started;       /* the start frame has gone */
    int channel;        /* receiver channel, -1: all */
    bool idle;          /* idle bursts too */
    int64_t until_us;   /* 0: until closed */
    uint32_t run;       /* the receiver run it opened in: a channel's stream ends at a retune */
    long max;           /* 0: no limit */
    long sent;
    uint32_t dropped;   /* bursts the queue had no room for while this was open, not yet reported */
    /* I/Q */
    int fmt;            /* IQ_CS8 .. IQ_CF32 */
    bool wide;          /* the front end's wideband samples, else channel `channel`'s 48 kS/s */
    double rate;
    long long want;     /* samples to send, 0: until closed */
    long long samples;  /* sent */
    int64_t sent_us;    /* when a frame last went */
    uint32_t gaps;
    long long filled;   /* of those, filler (zeros where the front end lost samples) */
    uint32_t pre_n;     /* pre-roll samples asked for */
    bool trigger;       /* waits for its channel's level (spec 6.2); fired: the pre-roll marker has come */
    bool fired;
    uint64_t trig_sc;   /* trigger: the rise, in sc; its level and the floor, in dB full scale */
    float level_db, floor_db;
    float thr;          /* trigger: power over the floor, as a ratio */
    uint32_t min_n, post_n; /* trigger: samples above it to fire; samples after */
    /* spectrum */
    uint32_t seq;       /* the last row sent */
    /* calls */
    bool ambe;          /* the voice bursts' AMBE frames too */
} stream_t;

/* A tracker's JSON (a call starting or ending, a CSBK, data), as the DSP hands it over. */
typedef struct {
    int channel;
    char json[RDOF_JSON_MAX];
} call_rec_t;

/* A call in progress on a channel's slot, for the end's record. */
typedef struct {
    bool open;
    uint32_t src, dst;
    bool group;
    char start[32];
    long voice0;        /* voice_bursts at the start */
} call_open_t;

/* One burst, as the DSP hands it over: copied into the queue, formatted on bursts_task. */
typedef struct {
    int channel;
    int64_t position;   /* 48 kS/s channel sample of the burst start */
    double t;           /* UTC seconds */
    float cfo_hz, dev_hz;
    float level_dbfs;
    int8_t dt, n, slot, family, cc, sync_errors;
    uint8_t burst[RDOF_BURST_BYTES];
} burst_rec_t;

static httpd_handle_t hd;
static SemaphoreHandle_t lock;        /* sessions and streams */
EXT_RAM_BSS_ATTR static session_t sessions[MAX_SESSIONS];
static stream_t streams[MAX_STREAMS];

/* I/Q history for pre-roll (hist_write, below) */
#define HIST_S 2.0
#define PREROLL_S 1.0
#define HIST_CH_N ((uint32_t)(HIST_S * RDOF_DEMOD_FS))
#define HIST_KEEP_FREE (1536 * 1024) /* PSRAM left for the rest when sizing the channels' rings */
static int16_t *hist_ch;           /* [C][HIST_CH_N][2] */
static int8_t *hist_wide;          /* [hist_wide_n][2] */
static uint32_t hist_wide_n;
static int hist_c;                 /* channels hist_ch holds (0: no channel pre-roll) */
static uint32_t hist_run;          /* the run the rings are for */
static volatile uint64_t hist_ch_w, hist_wide_w; /* samples written (channel samples, wideband samples) */
static SemaphoreHandle_t hist_mu;  /* the rings while bench_iq reads them, against a new run's */
static int64_t hist_us, hist_from_us; /* time spent writing them, since (status's history_load) */
static int64_t iq_us; /* the I/Q streams' share of the DSP (their frames converted and queued), since hist_from_us
                       * (status's stream_load) */
/* The rings are written only while warm: a stream that reads them opened in the last HIST_WARM_US (they cost about a
 * tenth of a core). A wideband stream and the line protocol's captures read only the wideband ring, so they warm only
 * it (wide_warm_until); a channel's stream warms the channels' rings (hist_warm_until) only with pre-roll or a
 * trigger: those rings are most of the cost, on a DSP core with little to spare. hist_*_from: where the samples kept
 * since warming up start. */
#define HIST_WARM_US (600LL * 1000000)
static volatile int64_t hist_warm_until, wide_warm_until;
static bool hist_on, hist_ch_on;
static volatile uint64_t hist_ch_from, hist_wide_from;
static QueueHandle_t burst_q;
static volatile int n_burst_streams;  /* read by the DSP without the lock: only says whether to queue */
static volatile int n_iq_streams;
static int n_spectrum_streams;
static volatile int n_call_streams, n_ambe_streams;
static QueueHandle_t call_q;
static volatile uint32_t call_drops;
static volatile long voice_bursts[RDOF_SDR_MAX_CHANNELS][2]; /* counted always: a call's length in bursts */
EXT_RAM_BSS_ATTR static call_open_t calls_open[RDOF_SDR_MAX_CHANNELS][2];
static volatile uint32_t queue_drops;
static uint32_t run_id = 1;           /* bumps with every receiver start (spec section 2) */

static const rdof_sdr_cfg_channel_t *rx_channel(int k);

static void take(void) { xSemaphoreTake(lock, portMAX_DELAY); }
static void give(void) { xSemaphoreGive(lock); }

/* ── a small appender for JSON text ───────────────────────────────────────── */

typedef struct {
    char *buf;
    size_t cap, len;
} out_t;

static void put(out_t *o, const char *fmt, ...)
{
    if (o->len >= o->cap)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->buf + o->len, o->cap - o->len, fmt, ap);
    va_end(ap);
    o->len = n < 0 ? o->cap : o->len + (size_t)n > o->cap ? o->cap : o->len + (size_t)n;
}

/* A JSON string: quotes, with " \ and control characters escaped. */
static void put_str(out_t *o, const char *s)
{
    put(o, "\"");
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            put(o, "\\%c", c);
        else if (c < 0x20)
            put(o, "\\u%04x", c);
        else
            put(o, "%c", c);
    }
    put(o, "\"");
}

/* A number, or null when it isn't one: fmt's one conversion (as "%.1f") becomes null, the rest stays. */
static void put_num(out_t *o, const char *fmt, double v)
{
    if (isfinite(v)) {
        put(o, fmt, v);
        return;
    }
    const char *pc = strchr(fmt, '%'), *end = pc ? strpbrk(pc + 1, "fgeFGE") : NULL;
    if (!end) {
        put(o, "null");
        return;
    }
    put(o, "%.*snull%s", (int)(pc - fmt), fmt, end + 1);
}

static bool overflowed(const out_t *o) { return o->len >= o->cap; }

/* ── HTTP helpers ─────────────────────────────────────────────────────────── */

static const char *status_line(int http)
{
    switch (http) {
    case 400: return "400 Bad Request";
    case 403: return "403 Forbidden";
    case 404: return "404 Not Found";
    case 409: return "409 Conflict";
    case 501: return "501 Not Implemented";
    case 503: return "503 Service Unavailable";
    default: return "500 Internal Server Error";
    }
}

/* An error as the spec's table has it: the status and {"error", "code"}. */
static esp_err_t send_error(httpd_req_t *req, int http, const char *code, const char *fmt, ...)
{
    char msg[160], body[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    out_t o = {body, sizeof body, 0};
    put(&o, "{\"error\":");
    put_str(&o, msg);
    put(&o, ",\"code\":\"%s\"}", code);
    httpd_resp_set_status(req, status_line(http));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, (ssize_t)o.len);
}

static esp_err_t send_json(httpd_req_t *req, const out_t *o)
{
    if (overflowed(o))
        return send_error(req, 500, "internal", "the reply didn't fit");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, o->buf, (ssize_t)o->len);
}

/* A query parameter, percent-decoded (false: absent). */
bool bench_query(void *r, const char *key, char *val, size_t cap)
{
    httpd_req_t *req = r;
    char q[256];
    if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK || httpd_query_key_value(q, key, val, cap) != ESP_OK)
        return false;
    char *o = val;
    for (const char *p = val; *p; p++) {
        unsigned h;
        if (*p == '%' && p[1] && p[2] && sscanf(p + 1, "%2x", &h) == 1) {
            *o++ = (char)h;
            p += 2;
        } else
            *o++ = *p == '+' ? ' ' : *p;
    }
    *o = 0;
    return true;
}

#define query bench_query

static double query_num(httpd_req_t *req, const char *key, double dflt)
{
    char v[24];
    return query(req, key, v, sizeof v) ? strtod(v, NULL) : dflt;
}

/* The request body, NUL terminated (false: none, or too big for cap). */
static bool body(httpd_req_t *req, char *buf, size_t cap)
{
    if (req->content_len >= cap)
        return false;
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0)
            return false;
        got += (size_t)r;
    }
    buf[got] = 0;
    return true;
}

/* A number from a flat JSON body: "key": value (enough for the session and lease requests). */
static bool json_num(const char *js, const char *key, double *out)
{
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(js, pat);
    if (!p)
        return false;
    p = strchr(p + strlen(pat), ':');
    if (!p)
        return false;
    char *end;
    double v = strtod(p + 1, &end);
    if (end == p + 1)
        return false;
    *out = v;
    return true;
}

static bool json_bool(const char *js, const char *key, bool dflt)
{
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(js, pat);
    if (!p || !(p = strchr(p + strlen(pat), ':')))
        return dflt;
    while (*++p == ' ')
        ;
    return strncmp(p, "true", 4) == 0 ? true : strncmp(p, "false", 5) == 0 ? false : dflt;
}

static void json_str(const char *js, const char *key, char *out, size_t cap)
{
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(js, pat);
    out[0] = 0;
    if (!p || !(p = strchr(p + strlen(pat), ':')) || !(p = strchr(p, '"')))
        return;
    size_t i = 0;
    for (p++; *p && *p != '"' && i + 1 < cap; p++)
        out[i++] = *p;
    out[i] = 0;
}

/* ── the clock (spec section 2) ───────────────────────────────────────────── */

static void put_clock(out_t *o)
{
    sdr_ppm_status_t p;
    sdr_ppm_status(&p);
    bool synced = false;
    int64_t now_us = netclock_utc_ns(esp_timer_get_time(), &synced) / 1000;
    put(o, "\"clock\":{\"run\":%lu,\"rate\":%.1f,\"sc0\":0,", (unsigned long)run_id, g_scfg.sample_rate);
    /* t0_us: UTC of wideband sample 0, from the receiver's stream time */
    double t0 = g_rx_ready ? g_rx.t0 : NAN;
    put_num(o, "\"t0_us\":%.0f", t0 * 1e6);
    put_num(o, ",\"ppm\":%.3f", p.clock_trusted ? p.clock_ppm : p.learned);
    put(o, ",\"utc_us\":%lld,\"synced\":%s}", (long long)now_us, synced ? "true" : "false");
}

/* ── info, status, sdr ────────────────────────────────────────────────────── */

static void put_channel(out_t *o, int c)
{
    const rdof_sdr_cfg_channel_t *ch = &g_scfg.ch[c];
    put(o, "{\"id\":");
    put_str(o, ch->id);
    put(o, ",\"name\":");
    put_str(o, ch->name[0] ? ch->name : ch->id);
    put(o, ",\"freq_hz\":%.1f,\"cc\":%d,\"slot\":%d,\"in_band\":%s,\"caps\":[\"rx\",\"dmr\"]}", ch->freq, ch->cc,
        ch->slot, g_sstat.in_window[c] ? "true" : "false");
}

static esp_err_t get_info(httpd_req_t *req)
{
    EXT_RAM_BSS_ATTR static char buf[3072];
    out_t o = {buf, sizeof buf, 0};
    const sdr_status_t *s = &g_sstat;
    const rdof_fe_caps_t *caps = sdr_fe_caps();
    double rate = g_scfg.sample_rate;
    put(&o, "{\"api\":\"v1\",\"device\":{\"kind\":\"esp32s3-gateway\",\"name\":");
    put_str(&o, g_scfg.name);
    put(&o, ",\"version\":");
    put_str(&o, esp_app_get_description()->version);
    put(&o, ",\"idf\":");
    put_str(&o, esp_app_get_description()->idf_ver);
    put(&o, "},\"sdr\":{\"frontend\":\"%s\",\"desc\":", rdof_fe_type_name(g_scfg.frontend));
    put_str(&o, s->rtl_desc);
    put(&o, ",\"kind\":");
    put_str(&o, caps ? caps->kind : "");
    put(&o, ",\"open\":%s,\"receiving\":%s,\"rate\":%.1f,", s->rtl_open ? "true" : "false",
        s->receiving ? "true" : "false", rate);
    put_num(&o, "\"center_hz\":%.1f,", s->center_hz);
    put_num(&o, "\"gain_db\":%.1f,", s->gain_db);
    sdr_ppm_status_t p;
    sdr_ppm_status(&p);
    put_num(&o, "\"ppm\":%.3f,", p.learned);
    put(&o, "\"window\":");
    if (isfinite(s->center_hz))
        put(&o, "{\"lo_hz\":%.1f,\"hi_hz\":%.1f}", s->center_hz - 0.4 * rate, s->center_hz + 0.4 * rate);
    else
        put(&o, "null");
    put(&o, ",\"tx\":%s},\"channels\":[", caps && caps->tx_mode != RDOF_FE_TX_NONE ? "true" : "false");
    for (int c = 0; c < g_scfg.nch; c++) {
        if (c)
            put(&o, ",");
        put_channel(&o, c);
    }
    put(&o, "],\"radios\":");
    if (o.len < o.cap) {
        size_t k = bench_radios_json(o.buf + o.len, o.cap - o.len, NULL);
        o.len = k ? o.len + k : o.cap;
    }
    put(&o, ",\"tx\":");
    if (o.len < o.cap) {
        size_t k = bench_tx_info_json(o.buf + o.len, o.cap - o.len);
        o.len = k ? o.len + k : o.cap;
    }
    put(&o, ",\"caps\":{\"spectrum\":true,\"bursts\":true,\"calls\":true,\"iq\":[\"cs16\",\"cf32\",\"cs8\",\"cu8\"],\"iq_wideband\":true,"
            "\"tune\":false,\"tx\":%s,\"radio_control\":%s},", bench_tx_enabled() ? "true" : "false",
        bench_radio_count() ? "true" : "false");
    put(&o, "\"limits\":{\"streams\":%d,\"sessions\":%d,\"preroll_s\":%.1f},", MAX_STREAMS, MAX_SESSIONS,
        hist_mu ? PREROLL_S : 0.0);
    put_clock(&o);
    put(&o, "}");
    return send_json(req, &o);
}

/* PUT /sdr's retune lasts as long as the exclusive lease of the session that asked: then the settings again. A line
 * protocol session's tune (bench_retune_line) is "line:" and its id, for as long as the line engine says. */
static char retune_owner[24];
#define LINE_OWNER "line:"
static bool line_owns(void) { return !strncmp(retune_owner, LINE_OWNER, strlen(LINE_OWNER)); }

static void retune_ends(const char *id)
{
    if (retune_owner[0] && !strcmp(retune_owner, id)) {
        ESP_LOGI(TAG, "session %s's retune ends: back to the settings", id);
        retune_owner[0] = 0;
        sdr_retune_restore();
    }
}

static void expire_sessions(void)
{
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (sessions[i].used && now - sessions[i].renewed_us > (int64_t)(sessions[i].ttl_s * 1e6)) {
            ESP_LOGI(TAG, "session %s (%s) expired", sessions[i].id, sessions[i].label);
            sessions[i].used = false;
            retune_ends(sessions[i].id);
        }
}

static void put_session(out_t *o, const session_t *s)
{
    int64_t now = esp_timer_get_time();
    put(o, "{\"session\":\"%s\",\"label\":", s->id);
    put_str(o, s->label);
    put(o, ",\"ttl_s\":%.0f,\"expires_in_s\":%.1f,\"lease\":", s->ttl_s, s->ttl_s - (now - s->renewed_us) / 1e6);
    if (!s->leased) {
        put(o, "null}");
        return;
    }
    put(o, "{\"freq_hz\":[");
    for (int k = 0; k < s->nfreqs; k++)
        put(o, "%s%.1f", k ? "," : "", s->freqs[k]);
    put(o, "],\"rx\":%s,\"tx\":%s,\"exclusive\":%s}}", s->rx ? "true" : "false", s->tx ? "true" : "false",
        s->exclusive ? "true" : "false");
}

static esp_err_t get_status(httpd_req_t *req)
{
    EXT_RAM_BSS_ATTR static char buf[6144];
    out_t o = {buf, sizeof buf, 0};
    const sdr_status_t *s = &g_sstat;
    rdof_fe_stats_t u;
    sdr_fe_stats(&u);
    long dropped, overflows;
    double load;
    sdr_counters(&dropped, &overflows, &load);
    put(&o, "{\"api\":\"v1\",\"device\":{\"name\":");
    put_str(&o, g_scfg.name);
    put(&o, ",\"uptime_s\":%.0f,\"heap_free\":%u},", esp_timer_get_time() / 1e6,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    put(&o, "\"sdr\":{\"open\":%s,\"receiving\":%s,", s->rtl_open ? "true" : "false", s->receiving ? "true" : "false");
    put(&o, "\"error\":");
    put_str(&o, s->rtl_error);
    put_num(&o, ",\"cpu_load\":%.3f", load);
    const int64_t now_us = esp_timer_get_time(); /* the pre-roll history's share of a core since the last status */
    if (hist_from_us && now_us > hist_from_us)
        put(&o, ",\"history_load\":%.3f,\"stream_load\":%.3f", (double)hist_us / (double)(now_us - hist_from_us),
            (double)iq_us / (double)(now_us - hist_from_us));
    hist_us = iq_us = 0;
    hist_from_us = now_us;
    put(&o, ",\"dropped_chunks\":%ld,\"overflows\":%ld,\"usb_errors\":%ld,\"bytes_lost\":%llu,\"stream_restarts\":%d},",
        dropped, overflows, (long)u.errors, (unsigned long long)u.bytes_lost, s->stream_restarts);
    put(&o, "\"channels\":[");
    rx_lock();
    int k = 0;
    for (int c = 0; c < g_scfg.nch; c++) {
        bool in = g_sstat.in_window[c] && g_rx_ready && k < g_rx.C;
        put(&o, "%s{\"id\":", c ? "," : "");
        put_str(&o, g_scfg.ch[c].id);
        put(&o, ",\"freq_hz\":%.1f,\"in_band\":%s", g_scfg.ch[c].freq, in ? "true" : "false");
        if (in) {
            put(&o, ",\"bursts\":%ld", g_rx.bursts[k]);
            if (g_rx.rssi[k] != INT32_MIN)
                put(&o, ",\"rssi\":%d", g_rx.rssi[k]);
            put(&o, ",\"afc_hz\":%.1f,\"idle_pct\":%.0f", g_rx.chz.correction_hz[k],
                g_rx.chunks ? 100.0 * (double)g_rx.gated[k] / (double)g_rx.chunks : 0.0);
            k++;
        }
        put(&o, "}");
    }
    for (const rdof_sdr_cfg_channel_t *tc; g_rx_ready && k < g_rx.C && (tc = rx_channel(k)) != NULL; k++) {
        put(&o, "%s{\"id\":", g_scfg.nch || k ? "," : ""); /* a retune's taps */
        put_str(&o, tc->id);
        put(&o, ",\"freq_hz\":%.1f,\"in_band\":true,\"tap\":true,\"bursts\":%ld", tc->freq, g_rx.bursts[k]);
        if (g_rx.rssi[k] != INT32_MIN)
            put(&o, ",\"rssi\":%d", g_rx.rssi[k]);
        put(&o, "}");
    }
    rx_unlock();
    put(&o, "],\"sessions\":[");
    take();
    expire_sessions();
    for (int i = 0, k = 0; i < MAX_SESSIONS; i++)
        if (sessions[i].used) {
            put(&o, k++ ? "," : "");
            put_session(&o, &sessions[i]);
        }
    put(&o, "],\"streams\":[");
    for (int i = 0, k = 0; i < MAX_STREAMS; i++)
        if (streams[i].kind != ST_NONE) {
            put(&o, "%s{\"kind\":\"%s\",\"channel\":", k++ ? "," : "",
                streams[i].kind == ST_IQ ? (streams[i].wide ? "iq_wide" : "iq")
                : streams[i].kind == ST_SPECTRUM ? "spectrum"
                : streams[i].kind == ST_CALLS    ? "calls"
                                                 : "bursts");
            if (streams[i].channel >= 0 && rx_channel(streams[i].channel))
                put_str(&o, rx_channel(streams[i].channel)->id);
            else
                put(&o, "null");
            if (streams[i].kind == ST_IQ)
                put(&o, ",\"samples\":%lld,\"gaps\":%lu}", streams[i].samples, (unsigned long)streams[i].gaps);
            else
                put(&o, ",\"sent\":%ld}", streams[i].sent);
        }
    give();
    put(&o, "],\"burst_queue_drops\":%lu,\"tx\":", (unsigned long)queue_drops);
    if (o.len < o.cap) {
        size_t k = bench_tx_status_json(o.buf + o.len, o.cap - o.len);
        o.len = k ? o.len + k : o.cap;
    }
    put(&o, ",");
    put_clock(&o);
    put(&o, "}");
    return send_json(req, &o);
}

static esp_err_t get_sdr(httpd_req_t *req)
{
    EXT_RAM_BSS_ATTR static char buf[512];
    out_t o = {buf, sizeof buf, 0};
    const sdr_status_t *s = &g_sstat;
    put(&o, "{\"frontend\":\"%s\",\"rate\":%.1f,", rdof_fe_type_name(g_scfg.frontend), g_scfg.sample_rate);
    put_num(&o, "\"center_hz\":%.1f,", s->center_hz);
    put_num(&o, "\"gain_db\":%.1f,", s->gain_db);
    sdr_ppm_status_t p;
    sdr_ppm_status(&p);
    put_num(&o, "\"ppm\":%.3f,", p.learned);
    put(&o, "\"rtl_ppm\":%.0f,\"receiving\":%s,\"retune\":", p.chip, s->receiving ? "true" : "false");
    double c, g, taps[SDR_MAX_TAPS];
    int nt;
    sdr_retune_get(&c, &g, taps, &nt);
    take();
    const bool on = retune_owner[0] != 0;
    if (on) {
        put(&o, "{\"session\":\"%s\",", retune_owner);
        isnan(c) ? put(&o, "\"center_hz\":\"auto\",") : put(&o, "\"center_hz\":%.1f,", c);
        isnan(g) ? put(&o, "\"gain_db\":\"auto\",") : put(&o, "\"gain_db\":%.1f,", g);
        put(&o, "\"taps_hz\":[");
        for (int i = 0; i < nt; i++)
            put(&o, "%s%.1f", i ? "," : "", taps[i]);
        put(&o, "]}}");
    } else
        put(&o, "null}");
    give();
    return send_json(req, &o);
}

static session_t *find_session(const char *id);

/* Numbers from a flat JSON array: "key": [a, b] (or one number); -1: absent. */
static int json_nums(const char *js, const char *key, double *out, int max)
{
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *f = strstr(js, pat);
    if (!f || !(f = strchr(f + strlen(pat), ':')))
        return -1;
    f++;
    while (*f == ' ' || *f == '[')
        f++;
    int n = 0;
    while (n < max && *f && *f != ']') {
        char *end;
        double v = strtod(f, &end);
        if (end == f)
            break;
        out[n++] = v;
        f = end;
        while (*f == ',' || *f == ' ')
            f++;
    }
    return n;
}

static bool json_auto(const char *js, const char *key)
{
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(js, pat);
    if (!p || !(p = strchr(p + strlen(pat), ':')))
        return false;
    while (*++p == ' ')
        ;
    return !strncmp(p, "\"auto\"", 6);
}

/* A retune's checks, PUT /sdr's and the line protocol's tune's: NULL, or an error code with why. */
static const char *retune_check(const sdr_retune_t *r, char *why, size_t cap)
{
    if (isfinite(r->center_hz) && r->center_hz != 0 && !(r->center_hz > 24e6 && r->center_hz < 1.766e9))
        snprintf(why, cap, "the center is 24 MHz to 1766 MHz");
    else if (isfinite(r->gain_db) && r->gain_db != SDR_RETUNE_SETTINGS && !(r->gain_db >= 0 && r->gain_db <= 60))
        snprintf(why, cap, "the gain is 0 to 60 dB");
    else if (r->ntaps > SDR_MAX_TAPS)
        snprintf(why, cap, "%d taps at most", SDR_MAX_TAPS);
    else
        return NULL;
    return "bad_request";
}

/* sdr_retune's why not, as an error code and HTTP status. */
static const char *retune_code(const char *why, int *http)
{
    const bool down = strstr(why, "no SDR") || strstr(why, "come back"), busy = strstr(why, "under way");
    *http = down ? 503 : busy ? 409 : 400;
    return down ? "unavailable" : busy ? "busy" : "bad_request";
}

/* PUT /sdr?session=ID {"center_hz": Hz | "auto", "gain_db": dB | "auto", "taps_hz": [Hz, ...]}: a retune for as
 * long as the session's exclusive lease lasts (spec section 5); fields left out stay as they are. */
static esp_err_t put_sdr(httpd_req_t *req)
{
    char js[256], session[16] = "";
    query(req, "session", session, sizeof session);
    if (!body(req, js, sizeof js))
        return send_error(req, 400, "bad_request", "a JSON body: {\"center_hz\", \"gain_db\", \"taps_hz\"}");
    take();
    expire_sessions();
    session_t *s = session[0] ? find_session(session) : NULL;
    const bool ok = s && s->leased && s->exclusive;
    if (ok)
        s->renewed_us = esp_timer_get_time();
    give();
    if (!ok)
        return send_error(req, 403, "refused", "a retune needs an exclusive lease: ?session=ID after POST "
                                               "/session/ID/lease {\"exclusive\": true}");
    sdr_retune_t r = {.center_hz = NAN, .gain_db = NAN, .ntaps = -1};
    double v;
    if (json_auto(js, "center_hz"))
        r.center_hz = 0;
    else if (json_num(js, "center_hz", &v))
        r.center_hz = isfinite(v) && v > 0 ? v : -1; /* -1: refused below */
    if (json_auto(js, "gain_db"))
        r.gain_db = SDR_RETUNE_SETTINGS;
    else if (json_num(js, "gain_db", &v))
        r.gain_db = isfinite(v) ? v : -1;
    if (json_num(js, "rate", &v) && fabs(v - g_scfg.sample_rate) > 1)
        return send_error(req, 400, "unsupported", "the rate is the settings' (%.0f S/s) on this board",
                          g_scfg.sample_rate);
    if (strstr(js, "\"ppm\""))
        return send_error(req, 400, "unsupported", "the ppm is learned on this board, not set");
    double taps[SDR_MAX_TAPS + 1];
    int nt = json_nums(js, "taps_hz", taps, SDR_MAX_TAPS + 1);
    if (nt > SDR_MAX_TAPS)
        return send_error(req, 400, "bad_request", "taps_hz: %d at most", SDR_MAX_TAPS);
    if (nt >= 0) {
        r.ntaps = nt;
        memcpy(r.taps_hz, taps, sizeof(double) * (size_t)nt);
    }
    char bad[64];
    const char *code = retune_check(&r, bad, sizeof bad);
    if (code)
        return send_error(req, 400, code, "%s", bad);
    const char *why = sdr_retune(&r, true);
    if (why) {
        int http;
        code = retune_code(why, &http);
        return send_error(req, http, code, "%s", why);
    }
    take();
    snprintf(retune_owner, sizeof retune_owner, "%s", session);
    give();
    ESP_LOGI(TAG, "session %s retuned the SDR", session);
    return get_sdr(req);
}

/* The spectrum: the same as /api/spectrum (spec section 6.1). */
static esp_err_t get_spectrum(httpd_req_t *req)
{
    enum { MAX_ROWS = 16, MAX_BINS = 512 };
    uint32_t since = (uint32_t)query_num(req, "since", 0);
    uint8_t *rows = heap_caps_malloc(MAX_ROWS * MAX_BINS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t cap = 256 + MAX_ROWS * (MAX_BINS * 4 / 3 + 8);
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rows || !buf) {
        free(rows);
        free(buf);
        return send_error(req, 500, "internal", "no memory");
    }
    uint32_t seq;
    sdr_spectrum_info_t info;
    int n = sdr_spectrum_rows(since, rows, MAX_ROWS, &seq, &info, NULL);
    out_t o = {buf, cap, 0};
    put(&o, "{\"seq\":%lu,\"bins\":%d,\"row_ms\":%d,\"rate\":%.0f,\"center\":%.0f,\"rows\":[", (unsigned long)seq,
        info.bins, info.row_ms, info.rate, info.center_hz);
    for (int i = 0; i < n && o.len + (size_t)info.bins * 4 / 3 + 8 < o.cap; i++) {
        put(&o, i ? ",\"" : "\"");
        rdof_base64(rows + (size_t)i * info.bins, (size_t)info.bins, o.buf + o.len);
        o.len += strlen(o.buf + o.len);
        put(&o, "\"");
    }
    put(&o, "]}");
    esp_err_t e = send_json(req, &o);
    free(rows);
    free(buf);
    return e;
}

/* ── sessions (spec section 4) ────────────────────────────────────────────── */

static session_t *find_session(const char *id)
{
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (sessions[i].used && !strcmp(sessions[i].id, id))
            return &sessions[i];
    return NULL;
}

/* The session id after "/session/" in the URI, up to the next / or ?. */
static void uri_session(httpd_req_t *req, char *id, size_t cap)
{
    const char *p = strstr(req->uri, "/session/");
    id[0] = 0;
    if (!p)
        return;
    p += 9;
    size_t i = 0;
    while (p[i] && p[i] != '/' && p[i] != '?' && i + 1 < cap) {
        id[i] = p[i];
        i++;
    }
    id[i] = 0;
}

static esp_err_t post_session(httpd_req_t *req)
{
    char js[256], id[16];
    if (!body(req, js, sizeof js))
        js[0] = 0;
    uri_session(req, id, sizeof id);
    take();
    expire_sessions();
    if (id[0]) { /* POST /session/{id}/lease */
        session_t *s = find_session(id);
        if (!s) {
            give();
            return send_error(req, 404, "bad_request", "no session %s (expired?)", id);
        }
        const char *f = strstr(js, "\"freq_hz\"");
        s->nfreqs = 0;
        if (f && (f = strchr(f, ':'))) {
            f++;
            while (*f == ' ' || *f == '[')
                f++;
            while (*f && *f != ']' && *f != '}' && s->nfreqs < MAX_LEASE_FREQS) {
                char *end;
                double v = strtod(f, &end);
                if (end == f)
                    break;
                s->freqs[s->nfreqs++] = v;
                f = end;
                while (*f == ',' || *f == ' ')
                    f++;
            }
        }
        s->rx = json_bool(js, "rx", true);
        s->tx = json_bool(js, "tx", false);
        s->exclusive = json_bool(js, "exclusive", false);
        if (s->exclusive && line_owns()) {
            s->exclusive = false;
            give();
            return send_error(req, 409, "busy", "the line protocol's session %s has the SDR tuned",
                              retune_owner + strlen(LINE_OWNER));
        }
        for (int i = 0; s->exclusive && i < MAX_SESSIONS; i++)
            if (sessions[i].used && &sessions[i] != s && sessions[i].leased && sessions[i].exclusive) {
                s->exclusive = false;
                give();
                return send_error(req, 409, "busy", "session %s (%s) holds the SDR", sessions[i].id,
                                  sessions[i].label);
            }
        if (!s->exclusive)
            retune_ends(s->id);
        if (s->tx && !s->nfreqs) {
            give();
            return send_error(req, 400, "bad_request", "a TX lease names its frequencies (freq_hz)");
        }
        s->leased = true;
        s->renewed_us = esp_timer_get_time();
        EXT_RAM_BSS_ATTR static char buf[512];
        out_t o = {buf, sizeof buf, 0};
        put_session(&o, s);
        give();
        return send_json(req, &o);
    }
    session_t *s = NULL;
    for (int i = 0; i < MAX_SESSIONS && !s; i++)
        if (!sessions[i].used)
            s = &sessions[i];
    if (!s) {
        give();
        return send_error(req, 409, "busy", "all %d sessions are in use", MAX_SESSIONS);
    }
    memset(s, 0, sizeof *s);
    s->used = true;
    snprintf(s->id, sizeof s->id, "s-%04lx", (unsigned long)(esp_random() & 0xffff));
    json_str(js, "label", s->label, sizeof s->label);
    double ttl = 30;
    json_num(js, "ttl_s", &ttl);
    s->ttl_s = ttl < 5 ? 5 : ttl > SESSION_TTL_MAX_S ? SESSION_TTL_MAX_S : ttl;
    s->renewed_us = esp_timer_get_time();
    ESP_LOGI(TAG, "session %s (%s), ttl %.0f s", s->id, s->label, s->ttl_s);
    EXT_RAM_BSS_ATTR static char buf[512];
    out_t o = {buf, sizeof buf, 0};
    put_session(&o, s);
    give();
    return send_json(req, &o);
}

const char *bench_retune_line(const sdr_retune_t *r, const char *who, char *why, size_t cap)
{
    const char *code = retune_check(r, why, cap);
    if (code)
        return code;
    take();
    expire_sessions();
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (sessions[i].used && sessions[i].leased && sessions[i].exclusive) {
            snprintf(why, cap, "the HTTP API's session %s (%s) holds the SDR", sessions[i].id, sessions[i].label);
            give();
            return "busy";
        }
    const bool had = line_owns();
    snprintf(retune_owner, sizeof retune_owner, LINE_OWNER "%.16s", who);
    give();
    const char *e = sdr_retune(r, false); /* the engine's lock is held: never wait for the DSP here */
    if (e) {
        snprintf(why, cap, "%s", e);
        if (!had) {
            take();
            retune_owner[0] = 0;
            give();
        }
        int http;
        return retune_code(e, &http);
    }
    ESP_LOGI(TAG, "line session %s retunes the SDR", who);
    return NULL;
}

int bench_retune_line_state(char *why, size_t cap)
{
    const int st = sdr_retune_poll(why, cap);
    double c, g, taps[SDR_MAX_TAPS];
    int nt;
    sdr_retune_get(&c, &g, taps, &nt);
    if (st < 0 && isnan(c) && isnan(g) && !nt) { /* refused, and no tune before it: nobody's */
        take();
        if (line_owns())
            retune_owner[0] = 0;
        give();
    }
    return st;
}

void bench_retune_line_end(void)
{
    take();
    const bool mine = line_owns();
    if (mine) {
        ESP_LOGI(TAG, "line session %s's tune ends: back to the settings", retune_owner + strlen(LINE_OWNER));
        retune_owner[0] = 0;
    }
    give();
    if (mine)
        sdr_retune_restore();
}

/* For a radio link (bench_radio.c): the TX frequencies the session's lease declared; -1: no session. */
int bench_session_tx(const char *id, uint32_t *tx, int max, bool renew)
{
    take();
    expire_sessions();
    session_t *s = find_session(id);
    int n = -1;
    if (s) {
        if (renew)
            s->renewed_us = esp_timer_get_time();
        n = 0;
        for (int k = 0; s->leased && s->tx && k < s->nfreqs && n < max; k++)
            tx[n++] = (uint32_t)llround(s->freqs[k]);
    }
    give();
    return n;
}

/* Does the session's lease cover this frequency (within 100 Hz; receive or transmit)? */
bool bench_session_covers(const char *id, double freq_hz)
{
    take();
    expire_sessions();
    session_t *s = find_session(id);
    bool yes = false;
    if (s) {
        s->renewed_us = esp_timer_get_time();
        for (int k = 0; s->leased && k < s->nfreqs && !yes; k++)
            yes = fabs(s->freqs[k] - freq_hz) < 100;
    }
    give();
    return yes;
}

bool bench_session_label(const char *id, char *label, size_t cap)
{
    take();
    session_t *s = find_session(id);
    snprintf(label, cap, "%s", s ? s->label : "");
    give();
    return s != NULL;
}

/* GET /session/{id}: renew it and say what it holds. */
static esp_err_t get_session(httpd_req_t *req)
{
    char id[16];
    uri_session(req, id, sizeof id);
    take();
    expire_sessions();
    session_t *s = find_session(id);
    if (!s) {
        give();
        return send_error(req, 404, "bad_request", "no session %s (expired?)", id);
    }
    s->renewed_us = esp_timer_get_time();
    EXT_RAM_BSS_ATTR static char buf[512];
    out_t o = {buf, sizeof buf, 0};
    put_session(&o, s);
    give();
    return send_json(req, &o);
}

static esp_err_t delete_session(httpd_req_t *req)
{
    char id[16];
    uri_session(req, id, sizeof id);
    take();
    session_t *s = find_session(id);
    bool lease = strstr(req->uri, "/lease") != NULL;
    if (s && lease)
        s->leased = false;
    else if (s)
        s->used = false;
    if (s)
        retune_ends(s->id);
    give();
    if (!s)
        return send_error(req, 404, "bad_request", "no session %s", id);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* ── the bursts stream (spec section 6.3) ─────────────────────────────────── */

/* The settings' channel for receiver channel k (the receiver has those in the tuner window, in order); NULL: none. */
static const rdof_sdr_cfg_channel_t *rx_channel(int k)
{
    return sdr_rx_cfg_channel(k); /* the configured channels in the window, then a retune's taps */
}

/* From the DSP cores (either one): the SDR transceiver's modem hears it as before, then a copy for the streams. */
void bench_on_burst(void *ctx, int channel, const rdof_sdr_burst_t *b, double t)
{
    sdr_tx_heard(ctx, channel, b, t);
    const bool voice = b->b.dt == RDOF_DT_VOICE_SYNC || b->b.dt == RDOF_DT_VOICE;
    if (voice && channel >= 0 && channel < RDOF_SDR_MAX_CHANNELS && (b->b.slot == 1 || b->b.slot == 2))
        voice_bursts[channel][b->b.slot - 1]++;
    bench_line_burst(channel, b, t);
    if (!(n_burst_streams || (n_ambe_streams && voice)) || !burst_q)
        return;
    burst_rec_t r = {.channel = channel, .position = b->position, .t = t, .cfo_hz = b->freq_offset_hz,
                     .dev_hz = b->deviation_hz, .level_dbfs = (float)b->b.power_dbfs, .dt = (int8_t)b->b.dt,
                     .n = (int8_t)b->b.n, .slot = (int8_t)b->b.slot, .family = (int8_t)b->b.family,
                     .cc = (int8_t)b->b.cc, .sync_errors = (int8_t)b->b.sync_errors};
    memcpy(r.burst, b->b.burst, sizeof r.burst);
    if (xQueueSend(burst_q, &r, 0) != pdTRUE)
        queue_drops++;
}

void bench_rx_started(void)
{
    run_id++;
}

static const char *kind_name(int dt, int n, char tmp[12])
{
    static const char *const names[] = {"PI_HDR", "VLC_HDR", "TERM_LC", "CSBK", "MBC_HDR", "MBC_CONT",
                                        "DATA_HDR", "R1/2", "R3/4", "IDLE", "R1", "USBD"};
    if ((uint8_t)dt == RDOF_DT_VOICE_SYNC || (uint8_t)dt == RDOF_DT_VOICE) {
        snprintf(tmp, 12, "VOICE_%c", 'A' + (n >= 0 && n < 6 ? n : 0));
        return tmp;
    }
    if (dt >= 0 && dt < 12)
        return names[dt];
    snprintf(tmp, 12, "%d", dt);
    return tmp;
}

static const char *sync_name(int family, int dt, char tmp[12])
{
    static const char *const fam[] = {"BS", "MS", "TS1", "TS2"};
    if ((uint8_t)dt == RDOF_DT_VOICE)
        return "EMB";
    if (family < 0 || family >= 4)
        return "?";
    snprintf(tmp, 12, "%s_%s", fam[family], (uint8_t)dt == RDOF_DT_VOICE_SYNC ? "VOICE" : "DATA");
    return tmp;
}

static size_t burst_json(const burst_rec_t *r, char *buf, size_t cap)
{
    out_t o = {buf, cap, 0};
    char k1[12], k2[12];
    const double sc = (double)r->position * g_scfg.sample_rate / RDOF_DEMOD_FS;
    const rdof_sdr_cfg_channel_t *ch = rx_channel(r->channel);
    put(&o, "{\"type\":\"burst\",\"burst\":{\"t\":%.6f,\"sc\":%.0f,\"channel\":", r->t, sc);
    put_str(&o, ch ? ch->id : "");
    put(&o, ",\"freq_hz\":%.1f,\"slot\":%d,\"sync\":\"%s\",\"kind\":\"%s\",\"dt\":%d,", ch ? ch->freq : 0.0, r->slot,
        sync_name(r->family, r->dt, k2), kind_name(r->dt, r->n, k1), (uint8_t)r->dt);
    if ((uint8_t)r->dt == RDOF_DT_VOICE_SYNC || (uint8_t)r->dt == RDOF_DT_VOICE)
        put(&o, "\"n\":%d,", r->n);
    if (r->cc >= 0)
        put(&o, "\"cc\":%d,", r->cc);
    put(&o, "\"cfo_hz\":%.1f,\"dev_hz\":%.1f,", r->cfo_hz, r->dev_hz);
    put_num(&o, "\"level_dbfs\":%.1f,", r->level_dbfs);
    put(&o, "\"sync_err\":%d,\"raw_hex\":\"", r->sync_errors);
    for (int i = 0; i < RDOF_BURST_BYTES; i++)
        put(&o, "%02x", r->burst[i]);
    put(&o, "\"}}");
    return overflowed(&o) ? 0 : o.len;
}

/* The receiver channel a frequency or id names (-1: none). */
static int channel_by(const char *id, double freq)
{
    const rdof_sdr_cfg_channel_t *ch;
    for (int k = 0; (ch = rx_channel(k)) != NULL; k++)
        if ((id && id[0] && !strcmp(ch->id, id)) || (isfinite(freq) && fabs(ch->freq - freq) < 1.0))
            return k; /* the receiver's index: channels in the window, in order, then taps */
    return -1;
}

static int ws_send_text(int fd, const char *s, size_t n)
{
    httpd_ws_frame_t f = {.final = true, .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)s, .len = n};
    return httpd_ws_send_frame_async(hd, fd, &f) == ESP_OK ? 0 : -1;
}

static void tap_set(int i, const stream_t *st);
static void spectrum_send(stream_t *st);
static size_t call_json(const call_rec_t *cr, char *buf, size_t cap, int *ended);
static size_t ambe_json(const burst_rec_t *r, char *buf, size_t cap);

static void put_u64(uint8_t *p, uint64_t v) { for (int k = 0; k < 8; k++) p[k] = (uint8_t)(v >> (8 * k)); }

/* Forget a stream (the socket is gone or closing). Under the lock. */
static void drop_stream(stream_t *st)
{
    if (st->kind == ST_BURSTS)
        n_burst_streams--;
    if (st->kind == ST_SPECTRUM)
        n_spectrum_streams--;
    if (st->kind == ST_CALLS) {
        n_call_streams--;
        if (st->ambe)
            n_ambe_streams--;
    }
    if (st->kind == ST_IQ) {
        n_iq_streams--;
        tap_set((int)(st - streams), NULL);
    }
    st->kind = ST_NONE;
}

/* Close a stream: its done frame, then the socket. Under the lock. */
static double iq_scale_db(int fmt);

static void finish(stream_t *st, const char *why)
{
    char b[288];
    int n = st->kind == ST_IQ && st->fired
              ? snprintf(b, sizeof b, "{\"type\":\"done\",\"why\":\"%s\",\"samples\":%lld,\"gaps\":%lu,\"filled\":%lld,"
                         "\"trigger_sc\":%llu,\"level_db\":%.1f,\"floor_db\":%.1f,\"scale_db\":%.2f}", why,
                         st->samples, (unsigned long)st->gaps, st->filled, (unsigned long long)st->trig_sc,
                         st->level_db, st->floor_db, iq_scale_db(st->fmt))
          : st->kind == ST_IQ ? snprintf(b, sizeof b, "{\"type\":\"done\",\"why\":\"%s\",\"samples\":%lld,\"gaps\":%lu,"
                                         "\"filled\":%lld,\"scale_db\":%.2f}", why, st->samples,
                                         (unsigned long)st->gaps, st->filled, iq_scale_db(st->fmt))
          : st->kind == ST_SPECTRUM ? snprintf(b, sizeof b, "{\"type\":\"done\",\"why\":\"%s\",\"rows\":%ld}", why, st->sent)
          : st->kind == ST_CALLS    ? snprintf(b, sizeof b, "{\"type\":\"done\",\"why\":\"%s\",\"calls\":%ld}", why, st->sent)
                                    : snprintf(b, sizeof b, "{\"type\":\"done\",\"why\":\"%s\",\"bursts\":%ld}", why, st->sent);
    ws_send_text(st->fd, b, (size_t)n);
    httpd_sess_trigger_close(hd, st->fd);
    drop_stream(st);
}

static void send_start(stream_t *st)
{
    EXT_RAM_BSS_ATTR static char b[512];
    out_t o = {b, sizeof b, 0};
    if (st->kind == ST_SPECTRUM) {
        uint32_t seq;
        sdr_spectrum_info_t info;
        sdr_spectrum_rows(UINT32_MAX, NULL, 0, &seq, &info, NULL);
        put(&o, "{\"type\":\"start\",\"stream\":\"spectrum\",\"bins\":%d,\"row_ms\":%d,\"rate\":%.0f,\"center_hz\":%.0f,"
                "\"sc_rate\":%.1f,", info.bins, info.row_ms, info.rate, info.center_hz, g_scfg.sample_rate);
        put_clock(&o);
        put(&o, "}");
        ws_send_text(st->fd, b, o.len);
        st->seq = seq; /* from the next row on */
        st->started = true;
        return;
    }
    put(&o, "{\"type\":\"start\",\"stream\":\"%s\",\"channel\":", st->kind == ST_CALLS ? "calls" : "bursts");
    const rdof_sdr_cfg_channel_t *ch = st->channel >= 0 ? rx_channel(st->channel) : NULL;
    if (ch)
        put_str(&o, ch->id);
    else
        put(&o, "null");
    put(&o, ",\"sc_rate\":%.1f,", g_scfg.sample_rate);
    if (st->kind == ST_CALLS)
        put(&o, "\"audio\":%s,", st->ambe ? "\"ambe\"" : "null");
    put_clock(&o);
    put(&o, "}");
    ws_send_text(st->fd, b, o.len);
    st->started = true;
}

static void bursts_task(void *arg)
{
    EXT_RAM_BSS_ATTR static char js[640];
    for (;;) {
        burst_rec_t r;
        bool got = xQueueReceive(burst_q, &r, pdMS_TO_TICKS(n_spectrum_streams || n_call_streams ? 50 : 250)) == pdTRUE;
        size_t n = got && n_burst_streams ? burst_json(&r, js, sizeof js) : 0;
        EXT_RAM_BSS_ATTR static call_rec_t cr;
        EXT_RAM_BSS_ATTR static char cjs[768], ajs[256];
        size_t cn = 0, an = 0;
        int call_ended = 0;
        if (call_q && xQueueReceive(call_q, &cr, 0) == pdTRUE)
            cn = call_json(&cr, cjs, sizeof cjs, &call_ended);
        int64_t now = esp_timer_get_time();
        static uint32_t drops_seen;
        uint32_t drops = queue_drops - drops_seen;
        drops_seen += drops;
        take();
        for (int i = 0; i < MAX_STREAMS; i++) {
            stream_t *st = &streams[i];
            if (st->kind != ST_BURSTS && st->kind != ST_SPECTRUM && st->kind != ST_CALLS)
                continue;
            if (httpd_ws_get_fd_info(hd, st->fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
                if (now - st->opened_us < 2000000)
                    continue; /* still finishing its handshake */
                drop_stream(st); /* the client went away */
                continue;
            }
            if (!st->started)
                send_start(st);
            if (st->channel >= 0 && st->run != run_id) { /* its channel index means something else now */
                finish(st, "retuned");
                continue;
            }
            if (st->kind == ST_CALLS) {
                if (cn && (st->channel < 0 || st->channel == cr.channel)) {
                    if (ws_send_text(st->fd, cjs, cn)) {
                        drop_stream(st);
                        continue;
                    }
                    st->sent += call_ended;
                }
                if (got && st->ambe && (st->channel < 0 || st->channel == r.channel) &&
                    ((uint8_t)r.dt == RDOF_DT_VOICE_SYNC || (uint8_t)r.dt == RDOF_DT_VOICE)) {
                    if (!an)
                        an = ambe_json(&r, ajs, sizeof ajs);
                    if (ws_send_text(st->fd, ajs, an)) {
                        drop_stream(st);
                        continue;
                    }
                }
                if (st->until_us && now >= st->until_us)
                    finish(st, "seconds");
                continue;
            }
            if (st->kind == ST_SPECTRUM) {
                spectrum_send(st);
                if (st->kind != ST_NONE && st->until_us && now >= st->until_us)
                    finish(st, "seconds");
                continue;
            }
            st->dropped += drops;
            if (st->dropped) {
                char d[80];
                int k = snprintf(d, sizeof d, "{\"type\":\"drop\",\"bursts\":%lu}", (unsigned long)st->dropped);
                ws_send_text(st->fd, d, (size_t)k);
                st->dropped = 0;
            }
            if (n && (st->channel < 0 || st->channel == r.channel) && (st->idle || r.dt != RDOF_DT_IDLE)) {
                if (ws_send_text(st->fd, js, n)) {
                    drop_stream(st);
                    continue;
                }
                st->sent++;
            }
            if (st->max && st->sent >= st->max)
                finish(st, "max_bursts");
            else if (st->until_us && now >= st->until_us)
                finish(st, "seconds");
        }
        give();
    }
}

/* New spectrum rows to a spectrum stream: binary frames of a 16-byte header (RDSP, u16 bins, u16 row_ms, u64 sc of
 * the row's last sample) and the row. Rows the stream fell behind on are a drop frame. Under the lock. */
static void spectrum_send(stream_t *st)
{
    enum { MAX_ROWS = 8, BINS = 512 };
    EXT_RAM_BSS_ATTR static uint8_t rows[MAX_ROWS * BINS], frame[16 + BINS];
    long long sc[MAX_ROWS];
    uint32_t seq;
    sdr_spectrum_info_t info;
    int n = sdr_spectrum_rows(st->seq, rows, MAX_ROWS, &seq, &info, sc);
    if (seq < st->seq) /* the receiver restarted: its rows count from 0 again */
        st->seq = 0;
    if ((uint32_t)n < seq - st->seq) {
        char d[64];
        int k = snprintf(d, sizeof d, "{\"type\":\"drop\",\"rows\":%lu}", (unsigned long)(seq - st->seq - (uint32_t)n));
        ws_send_text(st->fd, d, (size_t)k);
    }
    for (int i = 0; i < n && info.bins <= BINS; i++) {
        memcpy(frame, "RDSP", 4);
        frame[4] = (uint8_t)info.bins;
        frame[5] = (uint8_t)(info.bins >> 8);
        frame[6] = (uint8_t)info.row_ms;
        frame[7] = (uint8_t)(info.row_ms >> 8);
        put_u64(frame + 8, (uint64_t)sc[i]);
        memcpy(frame + 16, rows + (size_t)i * info.bins, (size_t)info.bins);
        httpd_ws_frame_t f = {.final = true, .type = HTTPD_WS_TYPE_BINARY, .payload = frame, .len = 16 + (size_t)info.bins};
        if (httpd_ws_send_frame_async(hd, st->fd, &f) != ESP_OK) {
            drop_stream(st);
            return;
        }
        st->sent++;
    }
    st->seq = seq;
}

/* WS /spectrum/ws?seconds=S: the spectrum rows as they come (spec section 6.1). The board has one size, 512 bins
 * about every 200 ms; the start frame says so, whatever bins and row_ms ask for. */
static esp_err_t ws_spectrum(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        double seconds = query_num(req, "seconds", 0);
        take();
        stream_t *st = NULL;
        for (int i = 0; i < MAX_STREAMS && !st; i++)
            if (streams[i].kind == ST_NONE)
                st = &streams[i];
        if (!st) {
            give();
            ESP_LOGW(TAG, "spectrum: all %d streams in use", MAX_STREAMS);
            return ESP_FAIL;
        }
        *st = (stream_t){.kind = ST_SPECTRUM, .fd = httpd_req_to_sockfd(req), .opened_us = esp_timer_get_time(),
                         .channel = -1,
                         .until_us = seconds > 0 ? esp_timer_get_time() + (int64_t)(seconds * 1e6) : 0, .run = run_id};
        n_spectrum_streams++;
        give();
        return ESP_OK;
    }
    httpd_ws_frame_t f = {0};
    if (httpd_ws_recv_frame(req, &f, 0) != ESP_OK)
        return ESP_FAIL;
    if (f.len) {
        uint8_t tmp[64];
        f.payload = tmp;
        if (f.len > sizeof tmp)
            return ESP_FAIL;
        httpd_ws_recv_frame(req, &f, f.len);
    }
    return ESP_OK;
}

/* ── calls (spec section 6.4) ─────────────────────────────────────────────── */

/* A tracker's JSON, from a DSP core: queued only while a calls stream is open. */
static void on_tracker_json(void *ctx, const char *chname, const char *json)
{
    if (!n_call_streams || !call_q || strncmp(json, "{\"DMR\"", 6))
        return;
    EXT_RAM_BSS_ATTR static call_rec_t recs[2]; /* one each DSP core: sinks run on both */
    call_rec_t *r = &recs[xPortGetCoreID() & 1];
    r->channel = (int)(intptr_t)ctx;
    snprintf(r->json, sizeof r->json, "%s", json);
    if (xQueueSend(call_q, r, 0) != pdTRUE)
        call_drops++;
}

bool bench_call_sink(int channel, rdof_sink_t *s)
{
    *s = (rdof_sink_t){.on_json = on_tracker_json, .ctx = (void *)(intptr_t)channel};
    return true;
}

/* The record for a tracker's JSON: {"type": "call", "event": start | late_entry | end | lost, channel, slot, src,
 * dst, group, ...} (an end with the start's fields, its length in voice bursts and lost: true when the carrier
 * went), else {"type": "event", "event": action, channel, "dmr": {...}}. *ended: 1 when a call ended. */
static size_t call_json(const call_rec_t *cr, char *buf, size_t cap, int *ended)
{
    enum { NODES = 48 };
    EXT_RAM_BSS_ATTR static rdof_jnode_t nodes[NODES];
    EXT_RAM_BSS_ATTR static char strs[RDOF_JSON_MAX];
    rdof_jdoc_t d = {.nodes = nodes, .max = NODES, .buf = strs, .cap = sizeof strs};
    int root = rdof_jparse(&d, cr->json, strlen(cr->json));
    int dmr = root >= 0 ? rdof_jget(&d, root, "DMR") : -1;
    if (dmr < 0)
        return 0;
#define STR(k) (rdof_jget(&d, dmr, k) >= 0 && rdof_jnode(&d, rdof_jget(&d, dmr, k))->type == RDOF_JSTR ? rdof_jnode(&d, rdof_jget(&d, dmr, k))->str : "")
#define NUM(k) (rdof_jget(&d, dmr, k) >= 0 ? rdof_jnode(&d, rdof_jget(&d, dmr, k))->num : NAN)
    const char *action = STR("action"), *ts = STR("timestamp");
    const double slot_n = NUM("slot");
    const int slot = isfinite(slot_n) ? (int)slot_n : 0;
    const rdof_sdr_cfg_channel_t *ch = rx_channel(cr->channel);
    out_t o = {buf, cap, 0};
    const bool start = !strcmp(action, "start") || !strcmp(action, "late_entry");
    const bool end = !strcmp(action, "end") || !strcmp(action, "lost");
    put(&o, "{\"type\":\"%s\",\"event\":", start || end ? "call" : "event");
    put_str(&o, action);
    put(&o, ",\"channel\":");
    put_str(&o, ch ? ch->id : "");
    put(&o, ",\"freq_hz\":%.1f,\"t\":", ch ? ch->freq : 0.0);
    put_str(&o, ts);
    call_open_t *c = cr->channel >= 0 && cr->channel < RDOF_SDR_MAX_CHANNELS && (slot == 1 || slot == 2)
                         ? &calls_open[cr->channel][slot - 1]
                         : NULL;
    if (start && c) {
        *c = (call_open_t){.open = true, .src = (uint32_t)NUM("src_id"), .dst = (uint32_t)NUM("dst_id"),
                           .group = !strcmp(STR("group"), "yes"), .voice0 = voice_bursts[cr->channel][slot - 1]};
        snprintf(c->start, sizeof c->start, "%s", ts);
    }
    if ((start || end) && c) {
        put(&o, ",\"slot\":%d", slot);
        if (c->open)
            put(&o, ",\"src\":%lu,\"dst\":%lu,\"group\":%s", (unsigned long)c->src, (unsigned long)c->dst,
                c->group ? "true" : "false");
        if (start) {
            if (isfinite(NUM("colorCode")))
                put(&o, ",\"cc\":%d", (int)NUM("colorCode"));
            if (rdof_jget(&d, dmr, "emergency") >= 0)
                put(&o, ",\"emergency\":true");
        }
        if (end) {
            put(&o, ",\"start\":");
            put_str(&o, c->open ? c->start : "");
            put(&o, ",\"lost\":%s,\"voice_bursts\":%ld", !strcmp(action, "lost") ? "true" : "false",
                c->open ? voice_bursts[cr->channel][slot - 1] - c->voice0 : 0L);
            put_num(&o, ",\"duration\":%.3f", NUM("duration"));
            put_num(&o, ",\"ber\":%.3f", NUM("ber"));
            int rs = rdof_jget(&d, dmr, "rssi");
            if (rs >= 0) {
                int mn = rdof_jget(&d, rs, "min"), mx = rdof_jget(&d, rs, "max"), av = rdof_jget(&d, rs, "ave");
                if (mn >= 0 && mx >= 0 && av >= 0)
                    put(&o, ",\"rssi\":{\"min\":%.0f,\"max\":%.0f,\"ave\":%.0f}", rdof_jnode(&d, mn)->num,
                        rdof_jnode(&d, mx)->num, rdof_jnode(&d, av)->num);
            }
            c->open = false;
            *ended = 1;
        }
    } else {
        if (slot)
            put(&o, ",\"slot\":%d", slot);
        /* the tracker's own object: {"DMR": {...}} without its outer braces' key */
        const char *inner = strchr(cr->json + 1, '{');
        size_t len = inner ? strlen(inner) : 0;
        put(&o, ",\"dmr\":%.*s", len > 1 ? (int)(len - 1) : 2, len > 1 ? inner : "{}");
    }
    put(&o, "}");
#undef STR
#undef NUM
    return o.len < o.cap ? o.len : 0;
}

/* A voice burst's three AMBE+2 frames (72 bits each: bits 0-107 and 156-263 of the burst), as hex. */
static size_t ambe_json(const burst_rec_t *r, char *buf, size_t cap)
{
    uint8_t f[27] = {0};
    for (int i = 0; i < 216; i++) {
        int k = i < 108 ? i : i + 48;
        f[i >> 3] |= (uint8_t)(((r->burst[k >> 3] >> (7 - (k & 7))) & 1) << (7 - (i & 7)));
    }
    const rdof_sdr_cfg_channel_t *ch = rx_channel(r->channel);
    out_t o = {buf, cap, 0};
    put(&o, "{\"type\":\"ambe\",\"channel\":");
    put_str(&o, ch ? ch->id : "");
    put(&o, ",\"slot\":%d,\"n\":%d,\"t\":%.6f,\"frames\":[", r->slot, r->n, r->t);
    for (int k = 0; k < 3; k++) {
        put(&o, k ? ",\"" : "\"");
        for (int i = 0; i < 9; i++)
            put(&o, "%02x", f[k * 9 + i]);
        put(&o, "\"");
    }
    put(&o, "]}");
    return o.len < o.cap ? o.len : 0;
}

/* WS /calls?channel=ID|freq_hz=F&seconds=S&audio=ambe */
static esp_err_t ws_calls(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        char id[24] = "", audio[8] = "";
        query(req, "channel", id, sizeof id);
        query(req, "audio", audio, sizeof audio);
        double freq = query_num(req, "freq_hz", NAN), seconds = query_num(req, "seconds", 0);
        int c = -1;
        if ((id[0] || isfinite(freq)) && (c = channel_by(id, freq)) < 0) {
            ESP_LOGW(TAG, "calls: no receiver channel %s", id[0] ? id : "at that frequency");
            return ESP_FAIL;
        }
        if (audio[0] && strcmp(audio, "ambe")) {
            ESP_LOGW(TAG, "calls: audio=%s: only ambe", audio);
            return ESP_FAIL;
        }
        take();
        stream_t *st = NULL;
        for (int i = 0; i < MAX_STREAMS && !st; i++)
            if (streams[i].kind == ST_NONE)
                st = &streams[i];
        if (!st) {
            give();
            ESP_LOGW(TAG, "calls: all %d streams in use", MAX_STREAMS);
            return ESP_FAIL;
        }
        *st = (stream_t){.kind = ST_CALLS, .fd = httpd_req_to_sockfd(req), .opened_us = esp_timer_get_time(),
                         .channel = c, .ambe = audio[0] != 0,
                         .until_us = seconds > 0 ? esp_timer_get_time() + (int64_t)(seconds * 1e6) : 0, .run = run_id};
        n_call_streams++;
        n_ambe_streams += st->ambe;
        give();
        return ESP_OK;
    }
    httpd_ws_frame_t f = {0};
    if (httpd_ws_recv_frame(req, &f, 0) != ESP_OK)
        return ESP_FAIL;
    if (f.len) {
        uint8_t tmp[64];
        f.payload = tmp;
        if (f.len > sizeof tmp)
            return ESP_FAIL;
        httpd_ws_recv_frame(req, &f, f.len);
    }
    return ESP_OK;
}

/* WS /bursts?channel=ID|freq_hz=F&seconds=S&max_bursts=N&idle=1 */
static esp_err_t ws_bursts(httpd_req_t *req)
{
    if (req->method == HTTP_GET) { /* the handshake: register the stream */
        char id[24] = "", v[8];
        query(req, "channel", id, sizeof id);
        double freq = query_num(req, "freq_hz", NAN);
        double seconds = query_num(req, "seconds", 0);
        long max = (long)query_num(req, "max_bursts", 0);
        int c = -1;
        if (id[0] || isfinite(freq)) {
            c = channel_by(id, freq);
            if (c < 0) {
                ESP_LOGW(TAG, "bursts: no receiver channel %s", id[0] ? id : "at that frequency");
                return ESP_FAIL; /* refuses the upgrade */
            }
        }
        take();
        stream_t *st = NULL;
        for (int i = 0; i < MAX_STREAMS && !st; i++)
            if (streams[i].kind == ST_NONE)
                st = &streams[i];
        if (!st) {
            give();
            ESP_LOGW(TAG, "bursts: all %d streams in use", MAX_STREAMS);
            return ESP_FAIL;
        }
        *st = (stream_t){.kind = ST_BURSTS, .fd = httpd_req_to_sockfd(req), .opened_us = esp_timer_get_time(),
                         .channel = c, .max = max,
                         .idle = query(req, "idle", v, sizeof v) && v[0] == '1',
                         .until_us = seconds > 0 ? esp_timer_get_time() + (int64_t)(seconds * 1e6) : 0, .run = run_id};
        n_burst_streams++;
        give();
        return ESP_OK;
    }
    /* a frame from the client: read and drop it (a ping or a note); the server answers pings and closes itself */
    httpd_ws_frame_t f = {0};
    if (httpd_ws_recv_frame(req, &f, 0) != ESP_OK)
        return ESP_FAIL;
    if (f.len) {
        uint8_t tmp[64];
        f.payload = tmp;
        if (f.len > sizeof tmp)
            return ESP_FAIL;
        httpd_ws_recv_frame(req, &f, f.len);
    }
    return ESP_OK;
}

/* ── the I/Q stream (spec section 6.2) ────────────────────────────────────── */

enum { IQ_CS8 = RDOF_BL_IQ_CS8, IQ_CU8 = RDOF_BL_IQ_CU8, IQ_CS16 = RDOF_BL_IQ_CS16, IQ_CF32 = RDOF_BL_IQ_CF32 };
#define IQ_HDR 32
#define IQ_RING_BYTES (768 * 1024) /* about 1.3 s of the wideband stream at 288 kS/s */
#define IQ_FLAG_GAP 1
#define IQ_FLAG_CLIP 2
#define IQ_FLAG_RETUNED 4
#define IQ_FLAG_PREROLL 32 /* from the history, before the stream's live frames (their own seq) */
#define IQ_FLAG_LAST 16
#define IQ_REF_LEN (1 + IQ_HDR + 4 + 4) /* a reference frame (iq_frames): stream, header, filler count, run */

static int iq_sample_bytes(int fmt) { return fmt == IQ_CF32 ? 8 : fmt == IQ_CS16 ? 4 : 2; }

/* What the DSP reads per stream: a copy kept under tap_mux, so it never waits on the lock. */
typedef struct {
    bool on, wide;
    int channel, fmt;
    uint32_t seq;
    bool gap;           /* a frame didn't fit in the ring: the next one says so */
    uint32_t run;
    /* pre-roll and trigger: asked (pre_n, trig..), and the DSP's state (anchored: the marker has gone) */
    uint32_t pre_n, min_n, post_n;
    bool trig, anchored;
    float thr, floor;
    uint32_t above;
} tap_t;

static portMUX_TYPE tap_mux = portMUX_INITIALIZER_UNLOCKED;
static tap_t taps[MAX_STREAMS];
static MessageBufferHandle_t iq_mb;
static uint8_t *iq_frame;     /* the DSP's scratch: stream index, header, samples */
static size_t iq_frame_cap;

static void tap_set(int i, const stream_t *st)
{
    taskENTER_CRITICAL(&tap_mux);
    taps[i] = st ? (tap_t){.on = true, .wide = st->wide, .channel = st->channel, .fmt = st->fmt, .run = st->run,
                           .pre_n = st->pre_n, .trig = st->trigger, .thr = st->thr, .min_n = st->min_n,
                           .post_n = st->post_n} : (tap_t){0};
    taskEXIT_CRITICAL(&tap_mux);
}

static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_u32(uint8_t *p, uint32_t v) { for (int k = 0; k < 4; k++) p[k] = (uint8_t)(v >> (8 * k)); }

/* Samples in fmt, the 8-bit ones k times full scale (wide_k); returns the clipped count. */
static int convert(const rdof_cf_t *in, int n, int fmt, float k, uint8_t *out)
{
    return rdof_bl_iq_convert((const float *)in, n, fmt, k, out);
}

/* From the DSP task after each chunk (sdr_process_chunk): the wideband chunk iq (n samples) and the channels' output
 * in rx->y. Frames go into the ring; one that doesn't fit is dropped and the stream's next frame says gap. */
/* ── I/Q history, for pre-roll (spec section 6.2) ────────────────────────── */

/* The DSP keeps the last HIST_S of every receiver channel (cs16, 48 kS/s) and of the wideband samples (cs8) in
 * PSRAM; a stream's pre-roll is read from there by bench_iq, which sends it before the stream's live frames. The
 * rings are HIST_S long and pre-roll stops at PREROLL_S: the rest is room for bench_iq to be behind. */

/* Truncated, then clamped as integers: the S3's float-to-int instruction, no float compares (the history is for
 * looking at, not for measuring to the last bit) */
static inline int16_t sat16(float x)
{
    const int q = (int)x;
    return (int16_t)(q > 32767 ? 32767 : q < -32767 ? -32767 : q);
}

/* The wideband history's cs8: rounded as a stream's cs8 is (rdof_bl_iq_convert), so a wide cs8 or cu8 stream's
 * frames are the history's bytes (iq_frames). Exact for |x| under 2^22: the LR2021's samples times 16256 are, at
 * any AGC step. clip counts values past full scale (rounded past it: a value within half a count over isn't). */
static inline int8_t wide8(float x, int *clip)
{
    const int q = rdof_bl_round(x);
    if (q > 127 || q < -127) {
        (*clip)++;
        return q > 0 ? 127 : -127;
    }
    return (int8_t)q;
}

/* The 8-bit samples' gain over full scale (cs8, cu8: the history, the captures, WS /iq wideband or a channel's). The
 * LR2021's I/Q sits far under its full scale (noise about 3.6 of 32767 at 53.5 dB gain, a W12 2026-11-02), so 127
 * of full scale gave all zeros: it gets 128 (42.14 dB), its noise about 2 counts and full scale at its sample 256.
 * The RTL's own samples are 8-bit: 1. */
static float wide_k(void) { return g_scfg.frontend == RDOF_FE_LR2021 ? 128.0f : 1.0f; }

double bench_wide_scale_db(void) { return 20.0 * log10(wide_k()); }

/* A stream's 8-bit samples (cs8, cu8), wideband or a channel's, have wide_k's gain over full scale: a channel's at
 * 1 gave a W12's LR2021 all zeros (its noise about 1.4 of 32767 in cs16). cs16 and cf32 have none. */
static double iq_scale_db(int fmt) { return rdof_bl_iq_scale_db(fmt, wide_k()); }

/* Where the wideband history holds filler (bench_wide_filler), under fill_mux; its run (a new run starts over).
 * chunk_fill: the next chunk's filler, for the I/Q streams (the DSP's alone). */
static portMUX_TYPE fill_mux = portMUX_INITIALIZER_UNLOCKED;
EXT_RAM_BSS_ATTR static rdof_bl_fills_t hist_fills;
static uint32_t fill_run;
static long chunk_fill;

void bench_wide_filler(uint64_t sc, long n)
{
    taskENTER_CRITICAL(&fill_mux);
    if (fill_run != run_id) {
        rdof_bl_fills_reset(&hist_fills);
        fill_run = run_id;
    }
    rdof_bl_fills_note(&hist_fills, sc, n);
    taskEXIT_CRITICAL(&fill_mux);
    chunk_fill = n;
}

long bench_wide_filled(uint64_t sc, long n)
{
    taskENTER_CRITICAL(&fill_mux);
    const long k = fill_run == run_id ? rdof_bl_fills_count(&hist_fills, sc, n) : 0;
    taskEXIT_CRITICAL(&fill_mux);
    return k;
}

static int hist_chunk_clip;  /* this chunk's clipped values in the wideband history (the DSP's) */
static bool hist_chunk_ok;   /* this chunk is in the wideband history: a wide cs8 / cu8 stream's frame is a reference */

/* From the DSP, each chunk: this chunk into the rings (a new run sizes them again). */
static void hist_write(const rdof_sdr_rx_t *rx, const rdof_cf_t *iq, int n)
{
    hist_chunk_ok = false;
    const int64_t now = esp_timer_get_time();
    const bool ch_warm = now <= hist_warm_until;
    if (!ch_warm && now > wide_warm_until) {
        hist_on = hist_ch_on = false;
        return;
    }
    if (!hist_on || hist_run != run_id) { /* warming up, or a new run: the kept samples start here */
        hist_wide_w = hist_wide_from = (uint64_t)(rx->samples - n);
        hist_on = true;
        hist_ch_on = false;
    }
    if (ch_warm && !hist_ch_on) /* the channels' rings warming up */
        hist_ch_w = hist_ch_from = (uint64_t)(rx->out_index - rx->got);
    hist_ch_on = ch_warm;
    if (hist_run != run_id) {
        xSemaphoreTake(hist_mu, portMAX_DELAY);
        heap_caps_free(hist_ch);
        hist_ch = NULL;
        hist_c = 0;
        const size_t per = (size_t)HIST_CH_N * 4;
        const size_t free_ps = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        if (rx->C > 0 && free_ps > HIST_KEEP_FREE + per * (size_t)rx->C &&
            (hist_ch = heap_caps_malloc(per * (size_t)rx->C, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) != NULL)
            hist_c = rx->C;
        else
            ESP_LOGW(TAG, "no PSRAM for %d channels' pre-roll (%u KB free): none this run", rx->C,
                     (unsigned)(free_ps / 1024));
        if (!hist_wide) {
            hist_wide_n = (uint32_t)(HIST_S * g_scfg.sample_rate);
            if (!(hist_wide = heap_caps_malloc((size_t)hist_wide_n * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)))
                hist_wide_n = 0;
        }
        hist_run = run_id;
        xSemaphoreGive(hist_mu);
    }
    for (int c = 0; c < (hist_ch_on ? hist_c : 0); c++) {
        const rdof_cf_t *y = rx->y + (size_t)c * rx->stride;
        int16_t *h = hist_ch + (size_t)c * HIST_CH_N * 2;
        uint32_t w = (uint32_t)(hist_ch_w % HIST_CH_N);
        for (int j = 0; j < rx->got; j++) {
            h[2 * w] = sat16(y[j].re * 32767.0f);
            h[2 * w + 1] = sat16(y[j].im * 32767.0f);
            if (++w == HIST_CH_N)
                w = 0;
        }
    }
    hist_ch_w += (uint64_t)(rx->got > 0 ? rx->got : 0);
    if (hist_wide_n) {
        uint32_t w = (uint32_t)(hist_wide_w % hist_wide_n);
        const float g = 127.0f * wide_k();
        int clip = 0;
        for (int j = 0; j < n; j++) {
            hist_wide[2 * w] = wide8(iq[j].re * g, &clip);
            hist_wide[2 * w + 1] = wide8(iq[j].im * g, &clip);
            if (++w == hist_wide_n)
                w = 0;
        }
        hist_wide_w += (uint64_t)n;
        hist_chunk_clip = clip;
        hist_chunk_ok = true;
    }
}

/* The line protocol's captures (bench_line.c): the wideband ring from sample sc (the run's own count), as
 * rdof_bl_ring_read: how many it copied; *oldest and *next what it holds. */
long bench_wide_read(uint64_t sc, int8_t *out, long n, uint64_t *oldest, uint64_t *next)
{
    if (!hist_mu) {
        *oldest = *next = 0;
        return 0;
    }
    xSemaphoreTake(hist_mu, portMAX_DELAY);
    if (!hist_on || !hist_wide_n || hist_run != run_id) { /* cold: nothing kept, the run is at its own count */
        *oldest = *next = g_rx_ready && g_rx.samples > 0 ? (uint64_t)g_rx.samples : 0;
        xSemaphoreGive(hist_mu);
        return 0;
    }
    const uint64_t w = hist_wide_w;
    /* a tenth of the ring's margin: the DSP may be filling it while this copies */
    const uint64_t keep = (uint64_t)hist_wide_n - hist_wide_n / 10;
    uint64_t old = w > keep ? w - keep : 0;
    if (old < hist_wide_from)
        old = hist_wide_from;
    *oldest = old, *next = w;
    long k = 0;
    if (n > 0 && sc >= old && sc < w) {
        k = (uint64_t)n > w - sc ? (long)(w - sc) : n;
        uint32_t x = (uint32_t)(sc % hist_wide_n);
        for (long i = 0; i < k; i++) {
            out[2 * i] = hist_wide[2 * x], out[2 * i + 1] = hist_wide[2 * x + 1];
            if (++x == hist_wide_n)
                x = 0;
        }
    }
    xSemaphoreGive(hist_mu);
    return k;
}

void bench_wide_warm(void) { wide_warm_until = esp_timer_get_time() + HIST_WARM_US; }

/* The pre-roll marker, through the I/Q ring ahead of a stream's live frames: send `count` samples from the history
 * from `from` (the stream's own sample index), then `post` (a trigger's; 0: the stream's own length) live. */
typedef struct __attribute__((packed)) {
    uint8_t stream;
    char tag[4]; /* "HIST" */
    uint64_t from;
    uint32_t count, post;
    double t0;
    uint64_t rise;      /* a trigger's: where it rose (stream samples), at what power over what floor */
    float level, floor;
} hist_marker_t;

static void iq_frames(const rdof_sdr_rx_t *rx, const rdof_cf_t *iq, int n, long fill_n);

void bench_on_chunk(const rdof_sdr_rx_t *rx, const rdof_cf_t *iq, int n)
{
    const long fill_n = chunk_fill < n ? chunk_fill : n; /* its first fill_n samples are filler */
    chunk_fill = 0;
    if (hist_mu) {
        const int64_t t0 = esp_timer_get_time();
        hist_write(rx, iq, n);
        hist_us += esp_timer_get_time() - t0;
    }
    if (!n_iq_streams || !iq_mb)
        return;
    const int64_t t_iq = esp_timer_get_time();
    iq_frames(rx, iq, n, fill_n);
    iq_us += esp_timer_get_time() - t_iq;
}

/* bench_on_chunk's I/Q streams: this chunk's frames into the ring */
static void iq_frames(const rdof_sdr_rx_t *rx, const rdof_cf_t *iq, int n, long fill_n)
{
    tap_t t[MAX_STREAMS];
    taskENTER_CRITICAL(&tap_mux);
    memcpy(t, taps, sizeof t);
    taskEXIT_CRITICAL(&tap_mux);
    const double ratio = g_scfg.sample_rate / RDOF_DEMOD_FS;
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (!t[i].on || (!t[i].wide && (t[i].channel < 0 || t[i].channel >= rx->C || t[i].run != run_id)))
            continue; /* a channel's stream from before a retune: its stream task ends it */
        const int m = t[i].wide ? n : rx->got;
        if (m <= 0)
            continue;
        if ((t[i].pre_n || t[i].trig) && !t[i].anchored) {
            /* where this chunk's samples start, in the stream's own samples */
            const uint64_t at = t[i].wide ? (uint64_t)(rx->samples - n) : (uint64_t)(rx->out_index - rx->got);
            hist_marker_t mk = {.stream = (uint8_t)i, .tag = {'H', 'I', 'S', 'T'}, .t0 = rx->t0};
            if (t[i].trig) { /* the channel's power against its floor, which follows it while below */
                const rdof_cf_t *y = rx->y + (size_t)t[i].channel * rx->stride;
                float p = 0;
                for (int j = 0; j < m; j++)
                    p += y[j].re * y[j].re + y[j].im * y[j].im;
                p /= (float)m;
                float fl = t[i].floor;
                uint32_t above = t[i].above;
                if (fl <= 0 || (p <= fl * t[i].thr && p < fl))
                    fl = p > 1e-12f ? p : 1e-12f;
                if (p > fl * t[i].thr)
                    above += (uint32_t)m;
                else {
                    above = 0;
                    fl += (p - fl) * 0.02f;
                }
                const bool fire = above >= t[i].min_n;
                taskENTER_CRITICAL(&tap_mux);
                taps[i].floor = fl;
                taps[i].above = above;
                if (fire)
                    taps[i].anchored = true;
                taskEXIT_CRITICAL(&tap_mux);
                if (!fire)
                    continue; /* nothing goes until it fires */
                /* from the rise (or pre_n before it) to the end of this chunk, then post_n live */
                const uint64_t rise = at + (uint64_t)m - above;
                mk.rise = rise;
                mk.level = p;
                mk.floor = fl;
                mk.from = rise > t[i].pre_n ? rise - t[i].pre_n : 0;
                mk.count = (uint32_t)(at + (uint64_t)m - mk.from);
                mk.post = t[i].post_n ? t[i].post_n : 1;
                xMessageBufferSend(iq_mb, &mk, sizeof mk, pdMS_TO_TICKS(5));
                continue; /* this chunk went with the history */
            }
            mk.from = at > t[i].pre_n ? at - t[i].pre_n : 0;
            mk.count = (uint32_t)(at - mk.from);
            taskENTER_CRITICAL(&tap_mux);
            taps[i].anchored = true;
            taskEXIT_CRITICAL(&tap_mux);
            xMessageBufferSend(iq_mb, &mk, sizeof mk, pdMS_TO_TICKS(5));
        }
        /* a wide cs8 or cu8 stream: the history holds this chunk's bytes already, so the frame goes as a reference
         * to them (no samples, its run after the filler count) and bench_iq copies them in on its own core: converting
         * here cost the DSP about 0.1 of its core (lr2021-iq.md R2.37) */
        const bool ref = t[i].wide && t[i].fmt != IQ_CS16 && hist_chunk_ok;
        const size_t need = ref ? IQ_REF_LEN : 1 + IQ_HDR + (size_t)m * (size_t)iq_sample_bytes(t[i].fmt) + 4;
        if (need > iq_frame_cap)
            continue;
        const rdof_cf_t *src = t[i].wide ? iq : rx->y + (size_t)t[i].channel * rx->stride;
        uint8_t *h = iq_frame + 1;
        int clipped = ref ? hist_chunk_clip : convert(src, m, t[i].fmt, wide_k(), h + IQ_HDR);
        if (ref)
            put_u32(h + IQ_HDR + 4, run_id);
        /* the chunk's filler, in this stream's samples (a channel's share of it), after the samples: bench_iq
         * counts it */
        const uint32_t fill = !fill_n ? 0 : t[i].wide ? (uint32_t)fill_n
                                                      : (uint32_t)(((int64_t)fill_n * m + n - 1) / n);
        /* the sample clock of the first sample: wideband samples since the receiver started */
        const uint64_t sc = t[i].wide ? (uint64_t)(rx->samples - n) : (uint64_t)llround((double)(rx->out_index - rx->got) * ratio);
        const double t_s = rx->t0 + (double)sc / g_scfg.sample_rate;
        iq_frame[0] = (uint8_t)i;
        memcpy(h, "RDIQ", 4);
        h[4] = 1;
        h[5] = (uint8_t)t[i].fmt;
        put_u16(h + 6, (uint16_t)((t[i].gap || fill ? IQ_FLAG_GAP : 0) | (clipped ? IQ_FLAG_CLIP : 0) |
                                   (t[i].run != run_id ? IQ_FLAG_RETUNED : 0)));
        memcpy(h + IQ_HDR + (ref ? 0 : (size_t)m * (size_t)iq_sample_bytes(t[i].fmt)), &fill, 4);
        put_u32(h + 8, t[i].seq);
        put_u32(h + 12, (uint32_t)m);
        put_u64(h + 16, sc);
        put_u64(h + 24, (uint64_t)(int64_t)llround(t_s * 1e6));
        bool sent = xMessageBufferSend(iq_mb, iq_frame, need, 0) == need;
        taskENTER_CRITICAL(&tap_mux);
        if (taps[i].on) {
            taps[i].seq++;
            taps[i].gap = !sent;
        }
        taskEXIT_CRITICAL(&tap_mux);
    }
}

static void iq_start(stream_t *st)
{
    EXT_RAM_BSS_ATTR static char b[640];
    out_t o = {b, sizeof b, 0};
    static const char *const fmts[] = {"", "cs8", "cu8", "cs16", "cf32"};
    put(&o, "{\"type\":\"start\",\"stream\":\"iq\",\"run\":%lu,\"fmt\":\"%s\",\"rate\":%.1f,", (unsigned long)run_id,
        fmts[st->fmt], st->rate);
    const rdof_sdr_cfg_channel_t *ch = st->wide ? NULL : rx_channel(st->channel);
    if (ch) {
        put(&o, "\"channel\":");
        put_str(&o, ch->id);
        put(&o, ",\"freq_hz\":%.1f,\"center_hz\":%.1f,", ch->freq, ch->freq);
    } else
        put_num(&o, "\"center_hz\":%.1f,", g_sstat.center_hz);
    put(&o, "\"scale\":%d,\"sc_rate\":%.1f,", st->fmt == IQ_CS16 ? 32767 : st->fmt == IQ_CF32 ? 1 : 127,
        g_scfg.sample_rate);
    put(&o, "\"scale_db\":%.2f,", iq_scale_db(st->fmt)); /* every stream's: a level less it is dBFS */
    put_num(&o, "\"gain_db\":%.1f,", g_sstat.gain_db);
    put_clock(&o);
    put(&o, "}");
    ws_send_text(st->fd, b, o.len);
    st->started = true;
}

/* bench_iq, under the lock: a stream's pre-roll from the history (what the rings still hold of it), before its
 * live frames; a trigger's marker also sets how much is sent in all. */
static void send_history(stream_t *st, const hist_marker_t *mk, uint8_t *fr, size_t cap)
{
    xSemaphoreTake(hist_mu, portMAX_DELAY);
    const bool wide = st->wide;
    const uint64_t w = wide ? hist_wide_w : hist_ch_w;
    const uint32_t ring = wide ? hist_wide_n : st->channel < hist_c ? HIST_CH_N : 0;
    const double ratio = wide ? 1.0 : g_scfg.sample_rate / RDOF_DEMOD_FS;
    /* the oldest sample still safe to read: the DSP writes on while this sends (0.2 s of room) */
    const uint64_t margin = (uint64_t)(0.2 * (wide ? g_scfg.sample_rate : RDOF_DEMOD_FS));
    uint64_t oldest = ring && w > ring - margin ? w - (ring - margin) : 0;
    const uint64_t kept = wide ? hist_wide_from : hist_ch_from; /* since the history warmed up */
    if (oldest < kept)
        oldest = kept;
    uint64_t from = mk->from;
    const uint64_t end = mk->from + mk->count;
    bool gap = false;
    if (!ring || from < oldest) {
        from = !ring ? end : oldest < end ? oldest : end;
        gap = true;
        st->gaps++;
    }
    /* what's sent in all: what the history has of the pre-roll, then the live samples asked for */
    if (st->trigger) {
        st->fired = true;
        st->until_us = 0;
        st->trig_sc = (uint64_t)llround((double)mk->rise * g_scfg.sample_rate / RDOF_DEMOD_FS);
        st->level_db = 10.0f * log10f(mk->level > 1e-12f ? mk->level : 1e-12f);
        st->floor_db = 10.0f * log10f(mk->floor > 1e-12f ? mk->floor : 1e-12f);
        st->want = (long long)(end - from) + mk->post;
    } else if (st->want)
        st->want += (long long)(end - from);
    const int bytes = iq_sample_bytes(st->fmt);
    const uint32_t per = (uint32_t)((cap - 1 - IQ_HDR) / (size_t)bytes);
    uint32_t seq = 0;
    while (from < end && st->kind == ST_IQ) {
        uint32_t k = end - from < per ? (uint32_t)(end - from) : per;
        uint16_t flags = IQ_FLAG_PREROLL | (gap ? IQ_FLAG_GAP : 0);
        gap = false;
        if (st->want && st->samples + k >= st->want) {
            k = (uint32_t)(st->want - st->samples);
            flags |= IQ_FLAG_LAST;
        }
        uint8_t *h = fr + 1, *o = h + IQ_HDR;
        const int up = wide ? (int)(256.0f / wide_k()) : 1; /* the wide history's 8 bits to cs16's full scale */
        const float k8 = 127.0f * wide_k() / 32767.0f;     /* a channel's cs16 history to 8 bits (iq_scale_db) */
        for (uint32_t j = 0; j < k; j++) {
            const uint32_t x = (uint32_t)((from + j) % ring);
            /* the history's samples, as cs16 (a wide 8-bit stream takes the history's own bytes, below) */
            const int a = wide ? hist_wide[2 * x] * up : hist_ch[((size_t)st->channel * HIST_CH_N + x) * 2];
            const int b = wide ? hist_wide[2 * x + 1] * up : hist_ch[((size_t)st->channel * HIST_CH_N + x) * 2 + 1];
            if (wide && st->fmt != IQ_CF32 && st->fmt != IQ_CS16) {
                o[2 * (size_t)j] = (uint8_t)(hist_wide[2 * x] + (st->fmt == IQ_CU8 ? 128 : 0));
                o[2 * (size_t)j + 1] = (uint8_t)(hist_wide[2 * x + 1] + (st->fmt == IQ_CU8 ? 128 : 0));
            } else if (st->fmt == IQ_CF32) {
                const float v[2] = {a / 32767.0f, b / 32767.0f};
                memcpy(o + 8 * (size_t)j, v, 8);
            } else if (st->fmt == IQ_CS16) {
                put_u16(o + 4 * (size_t)j, (uint16_t)(int16_t)a);
                put_u16(o + 4 * (size_t)j + 2, (uint16_t)(int16_t)b);
            } else {
                const int qa = (int)lrintf(a * k8), qb = (int)lrintf(b * k8);
                o[2 * (size_t)j] = (uint8_t)((qa > 127 ? 127 : qa < -127 ? -127 : qa) + (st->fmt == IQ_CU8 ? 128 : 0));
                o[2 * (size_t)j + 1] = (uint8_t)((qb > 127 ? 127 : qb < -127 ? -127 : qb) + (st->fmt == IQ_CU8 ? 128 : 0));
            }
        }
        const uint64_t sc = (uint64_t)llround((double)from * ratio);
        /* filler in the history there (bench_wide_filler), in this stream's samples */
        const long fw = bench_wide_filled(sc, (long)llround((double)k * ratio));
        const long fill = wide ? fw : (long)ceil((double)fw / ratio);
        if (fill > 0)
            flags |= IQ_FLAG_GAP;
        st->filled += fill < (long)k ? fill : (long)k;
        memcpy(h, "RDIQ", 4);
        h[4] = 1;
        h[5] = (uint8_t)st->fmt;
        put_u16(h + 6, flags);
        put_u32(h + 8, seq++);
        put_u32(h + 12, k);
        put_u64(h + 16, sc);
        put_u64(h + 24, (uint64_t)(int64_t)llround((mk->t0 + (double)sc / g_scfg.sample_rate) * 1e6));
        httpd_ws_frame_t f = {.final = true, .type = HTTPD_WS_TYPE_BINARY, .payload = h,
                              .len = IQ_HDR + (size_t)k * (size_t)bytes};
        if (httpd_ws_send_frame_async(hd, st->fd, &f) != ESP_OK) {
            drop_stream(st);
            break;
        }
        st->samples += k;
        from += k;
        if (st->want && st->samples >= st->want) {
            finish(st, "seconds");
            break;
        }
    }
    xSemaphoreGive(hist_mu);
}

/* bench_iq: a reference frame (iq_frames) made whole, its samples from the wideband history, the filler count after
 * them. Returns the frame's length, or 0 if the history has moved past them (the frame is dropped: the client sees
 * the samples missing, as when the I/Q ring is full). */
static size_t wide_frame(stream_t *st, uint8_t *buf, size_t cap)
{
    uint8_t *h = buf + 1;
    const uint32_t m = h[12] | h[13] << 8 | h[14] << 16 | (uint32_t)h[15] << 24;
    uint64_t sc = 0;
    for (int k = 0; k < 8; k++)
        sc |= (uint64_t)h[16 + k] << (8 * k);
    uint32_t fill, run;
    memcpy(&fill, h + IQ_HDR, 4);
    memcpy(&run, h + IQ_HDR + 4, 4);
    if (1 + IQ_HDR + (size_t)m * 2 + 4 > cap)
        return 0;
    bool ok = false;
    xSemaphoreTake(hist_mu, portMAX_DELAY);
    const uint64_t w = hist_wide_w;
    /* the oldest sample still safe to read: the DSP writes on while this copies (0.2 s of room) */
    const uint64_t room = hist_wide_n - (uint64_t)(0.2 * g_scfg.sample_rate);
    uint64_t oldest = w > room ? w - room : 0;
    if (oldest < hist_wide_from)
        oldest = hist_wide_from;
    if (hist_on && hist_wide_n && hist_run == run && sc >= oldest && sc + m <= w) {
        uint8_t *o = h + IQ_HDR;
        const uint32_t x = (uint32_t)(sc % hist_wide_n), a = m < hist_wide_n - x ? m : hist_wide_n - x;
        memcpy(o, hist_wide + 2 * (size_t)x, (size_t)a * 2); /* to the ring's end, then from its start */
        memcpy(o + (size_t)a * 2, hist_wide, (size_t)(m - a) * 2);
        if (st->fmt == IQ_CU8)
            for (uint32_t j = 0; j < 2 * m; j++)
                o[j] += 128;
        ok = true;
    }
    xSemaphoreGive(hist_mu);
    if (!ok) {
        st->gaps++;
        return 0;
    }
    memcpy(h + IQ_HDR + (size_t)m * 2, &fill, 4);
    return 1 + IQ_HDR + (size_t)m * 2 + 4;
}

static void bench_iq(void *arg)
{
    const size_t cap = iq_frame_cap;
    uint8_t *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *hbuf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); /* pre-roll frames */
    for (;;) {
        size_t len = xMessageBufferReceive(iq_mb, buf, cap, pdMS_TO_TICKS(200));
        int64_t now = esp_timer_get_time();
        take();
        for (int i = 0; i < MAX_STREAMS; i++) { /* starts, and clients gone */
            stream_t *st = &streams[i];
            if (st->kind != ST_IQ)
                continue;
            if (httpd_ws_get_fd_info(hd, st->fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
                if (now - st->opened_us >= 2000000)
                    drop_stream(st);
                continue;
            }
            if (!st->started)
                iq_start(st);
            if (!st->wide && st->run != run_id)
                finish(st, "retuned");
            else if (st->trigger && !st->fired && st->until_us && now >= st->until_us)
                finish(st, "no_signal"); /* the wait ended with nothing heard */
            /* no samples for 2 s past its time (no receiver?), or 20 s late. Samples still coming carry on: two
             * streams at once (wide cs8 and a channel's cs16) fell 2.3 s behind in 4 s on a W12, core 0 busy
             * sending, and this cut them short at 99% (lr2021-iq.md R2.37) */
            else if (st->until_us && now >= st->until_us + 2000000 &&
                     (now - (st->sent_us ? st->sent_us : st->opened_us) >= 2000000 || now >= st->until_us + 20000000))
                finish(st, "seconds");
        }
        if (len == IQ_REF_LEN && buf[0] < MAX_STREAMS && streams[buf[0]].kind == ST_IQ && streams[buf[0]].started)
            len = wide_frame(&streams[buf[0]], buf, cap);
        if (len == sizeof(hist_marker_t) && !memcmp(buf + 1, "HIST", 4)) {
            hist_marker_t mk;
            memcpy(&mk, buf, sizeof mk);
            if (mk.stream < MAX_STREAMS && streams[mk.stream].kind == ST_IQ && streams[mk.stream].started && hbuf)
                send_history(&streams[mk.stream], &mk, hbuf, cap);
        } else if (len > 1 + IQ_HDR + 4 && buf[0] < MAX_STREAMS && streams[buf[0]].kind == ST_IQ &&
                   streams[buf[0]].started) {
            stream_t *st = &streams[buf[0]];
            uint8_t *h = buf + 1;
            uint32_t m = h[12] | h[13] << 8 | h[14] << 16 | (uint32_t)h[15] << 24, fill;
            len -= 4; /* the DSP's filler count, after the samples: the frame's first `fill` are filler */
            memcpy(&fill, buf + len, 4);
            uint16_t flags = (uint16_t)(h[6] | h[7] << 8);
            if (flags & IQ_FLAG_GAP)
                st->gaps++;
            if (st->want && st->samples + m >= st->want) { /* the last frame: cut to what was asked for */
                m = (uint32_t)(st->want - st->samples);
                if (fill > m)
                    fill = m;
                put_u32(h + 12, m);
                put_u16(h + 6, flags | IQ_FLAG_LAST);
                len = 1 + IQ_HDR + (size_t)m * (size_t)iq_sample_bytes(st->fmt);
            }
            httpd_ws_frame_t f = {.final = true, .type = HTTPD_WS_TYPE_BINARY, .payload = h, .len = len - 1};
            if (httpd_ws_send_frame_async(hd, st->fd, &f) != ESP_OK)
                drop_stream(st);
            else {
                st->samples += m;
                st->sent_us = now;
                st->filled += fill;
                if (st->want && st->samples >= st->want)
                    finish(st, "seconds");
            }
        }
        give();
    }
}

/* WS /iq?freq_hz=F|channel=ID&fmt=cs16|cf32|cs8&seconds=S, or rate=wide (fmt cs8 / cu8 / cs16) */
static esp_err_t ws_iq(httpd_req_t *req)
{
    if (req->method != HTTP_GET) { /* client frames: read and drop */
        httpd_ws_frame_t f = {0};
        if (httpd_ws_recv_frame(req, &f, 0) != ESP_OK)
            return ESP_FAIL;
        if (f.len) {
            uint8_t tmp[64];
            if (f.len > sizeof tmp)
                return ESP_FAIL;
            f.payload = tmp;
            httpd_ws_recv_frame(req, &f, f.len);
        }
        return ESP_OK;
    }
    char id[24] = "", v[12] = "";
    query(req, "channel", id, sizeof id);
    double freq = query_num(req, "freq_hz", NAN), seconds = query_num(req, "seconds", 0);
    bool wide = query(req, "rate", v, sizeof v) && !strcmp(v, "wide");
    int fmt = wide ? IQ_CS8 : IQ_CS16;
    if (query(req, "fmt", v, sizeof v))
        fmt = !strcmp(v, "cs8") ? IQ_CS8 : !strcmp(v, "cu8") ? IQ_CU8 : !strcmp(v, "cs16") ? IQ_CS16
            : !strcmp(v, "cf32") ? IQ_CF32 : 0;
    int c = wide ? -1 : channel_by(id, freq);
    /* pre-roll and the level trigger (spec section 6.2) */
    const double pre_s = query_num(req, "pre_s", 0);
    const bool trigger = query(req, "trigger", v, sizeof v);
    const char *bad = pre_s < 0 || pre_s > PREROLL_S ? "pre_s: 0 to 1 s"
                    : trigger && strcmp(v, "level") ? "trigger: level"
                    : trigger && wide ? "a trigger is on a channel, not the wideband stream"
                    : pre_s > 0 && hist_run == run_id && (wide ? !hist_wide_n : c >= hist_c)
                        ? "no pre-roll here (no PSRAM for its history)"
                    : NULL;
    if (bad) {
        ESP_LOGW(TAG, "iq: %s", bad);
        return ESP_FAIL;
    }
    if (!fmt || (!wide && c < 0) || (wide && fmt == IQ_CF32) || !g_rx_ready) {
        ESP_LOGW(TAG, "iq: %s", !fmt ? "unknown fmt" : !g_rx_ready ? "not receiving" : wide ? "cf32 isn't offered wide"
                                                                                       : "no receiver channel there");
        return ESP_FAIL;
    }
    take();
    stream_t *st = NULL;
    int wides = 0;
    for (int i = 0; i < MAX_STREAMS; i++) {
        wides += streams[i].kind == ST_IQ && streams[i].wide;
        if (!st && streams[i].kind == ST_NONE)
            st = &streams[i];
    }
    if (!st || (wide && wides)) {
        give();
        ESP_LOGW(TAG, "iq: %s", st ? "one wideband stream at a time" : "all streams in use");
        return ESP_FAIL;
    }
    const double rate = wide ? g_scfg.sample_rate : RDOF_DEMOD_FS;
    *st = (stream_t){.kind = ST_IQ, .fd = httpd_req_to_sockfd(req), .opened_us = esp_timer_get_time(), .channel = c,
                     .fmt = fmt, .wide = wide, .rate = rate,
                     .want = seconds > 0 && !trigger ? (long long)llround(seconds * rate) : 0,
                     .until_us = seconds > 0 ? esp_timer_get_time() + (int64_t)(seconds * 1e6) : 0, .run = run_id,
                     .pre_n = (uint32_t)llround(pre_s * rate), .trigger = trigger,
                     .thr = (float)pow(10.0, query_num(req, "threshold_db", 10) / 10.0),
                     .min_n = (uint32_t)llround(query_num(req, "min_ms", 10) / 1000.0 * rate),
                     .post_n = (uint32_t)llround(query_num(req, "post_s", 2) * rate)};
    n_iq_streams++;
    /* keeps the pre-roll history going, only what the stream reads: a wideband stream the wideband history (its cs8
     * and cu8 frames come from it), a channel's the channels' rings if it asks for pre-roll or a trigger. Warm, the
     * channels' rings cost a W12's DSP 0.045 of its core, and back-to-back wideband captures then lost samples
     * (lr2021-iq.md R2.37) */
    if (wide)
        wide_warm_until = esp_timer_get_time() + HIST_WARM_US;
    else if (pre_s > 0 || trigger)
        hist_warm_until = esp_timer_get_time() + HIST_WARM_US;
    tap_set((int)(st - streams), st);
    give();
    return ESP_OK;
}

/* ── radios (spec section 8; the links are in bench_radio.c) ──────────────── */

/* POST /radios/{name}/tune?session=ID[&proto=odr-debug] {"freq_hz", "mode": "dmr|fm", "cc", "slot", "tg",
 * "private"}: the lease must cover freq_hz. Answers once the radio has been checked at its new frequency. */
static esp_err_t post_radio_tune(httpd_req_t *req)
{
    char name[32] = "", session[16] = "", proto[12] = "", js[256];
    const char *p = strstr(req->uri, "/radios/");
    if (p)
        snprintf(name, sizeof name, "%.*s", (int)strcspn(p + 8, "/?"), p + 8);
    query(req, "session", session, sizeof session);
    query(req, "proto", proto, sizeof proto);
    if (!body(req, js, sizeof js))
        return send_error(req, 400, "bad_request", "a JSON body: {\"freq_hz\", \"mode\", \"cc\", \"slot\", \"tg\"}");
    double f = NAN, v;
    if (!json_num(js, "freq_hz", &f) || !(f > 1e6 && f < 6e9))
        return send_error(req, 400, "bad_request", "freq_hz: the frequency in Hz");
    rdof_guard_tune_t t = {.freq_hz = (uint32_t)llround(f), .cc = -1};
    const char *m = strstr(js, "\"mode\"");
    t.fm = m && strstr(m, "\"fm\"") && strstr(m, "\"fm\"") < m + 16;
    if (json_num(js, "cc", &v)) {
        if (v < 0 || v > 15)
            return send_error(req, 400, "bad_request", "cc: 0 to 15");
        t.cc = (int)v;
    }
    if (json_num(js, "slot", &v)) {
        if (v != 1 && v != 2)
            return send_error(req, 400, "bad_request", "slot: 1 or 2");
        t.slot = (int)v;
    }
    if (json_num(js, "tg", &v)) {
        if (v < 1 || v > 16777215)
            return send_error(req, 400, "bad_request", "tg: 1 to 16777215");
        t.tg = (long)v;
    }
    t.private_call = json_bool(js, "private", false);
    if (!session[0])
        return send_error(req, 403, "refused", "?session=ID with a lease on %.5f MHz", f / 1e6);
    if (!bench_session_covers(session, f))
        return send_error(req, 403, "refused", "session %s has no lease on %.5f MHz", session, f / 1e6);
    int http;
    uint32_t rx_hz = 0;
    int power = -1;
    const char *why = bench_radio_tune(name, session, &t, !strcmp(proto, "odr-debug"), &http, &rx_hz, &power);
    if (why)
        return send_error(req, http, http == 404 ? "bad_request" : http == 501 ? "unsupported" :
                                     http == 409 ? "busy" : "unavailable", "%s", why);
    char out[200];
    snprintf(out, sizeof out, "{\"radio\":\"%s\",\"freq_hz\":%lu,\"mode\":\"%s\",\"rx_hz\":%lu,\"power\":%d}", name,
             (unsigned long)t.freq_hz, t.fm ? "fm" : "dmr", (unsigned long)rx_hz, power);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out);
}

/* GET /radios, GET /radios/{name} */
static esp_err_t get_radios(httpd_req_t *req)
{
    EXT_RAM_BSS_ATTR static char buf[2048];
    char name[32] = "";
    const char *p = strstr(req->uri, "/radios/");
    if (p)
        snprintf(name, sizeof name, "%.*s", (int)strcspn(p + 8, "/?"), p + 8);
    size_t n = bench_radios_json(buf, sizeof buf, name[0] ? name : NULL);
    if (!n)
        return send_error(req, 500, "internal", "the reply didn't fit");
    if (name[0]) {
        if (n <= 2)
            return send_error(req, 404, "bad_request", "no radio %s", name);
        buf[n - 1] = 0; /* the one object, without the list's brackets */
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, buf + 1, (ssize_t)n - 2);
    }
    out_t o = {buf, sizeof buf, n};
    return send_json(req, &o);
}

/* The web server's URI match: ESP-IDF's wildcards (a * at the end, ?), and a * between slashes for one path
 * segment (/radios/{name}/link). */
bool bench_uri_match(const char *tmpl, const char *uri, size_t len)
{
    if (!strstr(tmpl, "*/"))
        return httpd_uri_match_wildcard(tmpl, uri, len);
    size_t i = 0;
    for (const char *t = tmpl; *t; t++) {
        if (*t == '*') {
            size_t s = i;
            while (i < len && uri[i] != '/')
                i++;
            if (i == s)
                return false;
            continue;
        }
        if (i >= len || uri[i] != *t)
            return false;
        i++;
    }
    return i == len;
}

/* ── registration ─────────────────────────────────────────────────────────── */

int bench_api_register(void *server)
{
    hd = server;
    if (!lock) {
        lock = xSemaphoreCreateMutex();
        burst_q = xQueueCreateWithCaps(BURST_QUEUE, sizeof(burst_rec_t), MALLOC_CAP_SPIRAM);
        call_q = xQueueCreateWithCaps(16, sizeof(call_rec_t), MALLOC_CAP_SPIRAM);
        xTaskCreatePinnedToCoreWithCaps(bursts_task, "bench_bursts", 4096, NULL, 3, NULL, 0, MALLOC_CAP_SPIRAM);
        /* the I/Q ring and the DSP's frame scratch, in PSRAM: the biggest frame is a wideband chunk in cs16 */
        iq_frame_cap = 1 + IQ_HDR + (size_t)CONFIG_RDOF_SDR_CHUNK * 4 + 4; /* and the filler count */
        iq_frame = heap_caps_malloc(iq_frame_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        uint8_t *ring = heap_caps_malloc(IQ_RING_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        static StaticMessageBuffer_t mb;
        if (iq_frame && ring) {
            iq_mb = xMessageBufferCreateStatic(IQ_RING_BYTES, ring, &mb);
            hist_mu = xSemaphoreCreateMutex();
            xTaskCreatePinnedToCoreWithCaps(bench_iq, "bench_iq", 4096, NULL, 3, NULL, 0, MALLOC_CAP_SPIRAM);
        } else
            ESP_LOGW(TAG, "no PSRAM for the I/Q ring: no I/Q streams");
    }
    const httpd_uri_t uris[] = {
        {.uri = BASE "/radios/*/tune", .method = HTTP_POST, .handler = post_radio_tune},
        {.uri = BASE "/info", .method = HTTP_GET, .handler = get_info},
        {.uri = BASE "/status", .method = HTTP_GET, .handler = get_status},
        {.uri = BASE "/sdr", .method = HTTP_GET, .handler = get_sdr},
        {.uri = BASE "/sdr", .method = HTTP_PUT, .handler = put_sdr},
        {.uri = BASE "/spectrum", .method = HTTP_GET, .handler = get_spectrum},
        {.uri = BASE "/spectrum/ws", .method = HTTP_GET, .handler = ws_spectrum, .is_websocket = true},
        {.uri = BASE "/calls", .method = HTTP_GET, .handler = ws_calls, .is_websocket = true},
        {.uri = BASE "/session*", .method = HTTP_POST, .handler = post_session},
        {.uri = BASE "/session/*", .method = HTTP_GET, .handler = get_session},
        {.uri = BASE "/session/*", .method = HTTP_DELETE, .handler = delete_session},
        {.uri = BASE "/bursts", .method = HTTP_GET, .handler = ws_bursts, .is_websocket = true,
         .handle_ws_control_frames = false},
        {.uri = BASE "/iq", .method = HTTP_GET, .handler = ws_iq, .is_websocket = true,
         .handle_ws_control_frames = false},
        {.uri = BASE "/radios", .method = HTTP_GET, .handler = get_radios},
        {.uri = BASE "/radios/*", .method = HTTP_GET, .handler = get_radios},
    };
    int n = bench_radio_register(server); /* its link handler goes before the radios list */
    n += bench_tx_register(server);
    bench_line_start();
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++)
        n += httpd_register_uri_handler(server, &uris[i]) == ESP_OK;
    return n;
}

#endif
