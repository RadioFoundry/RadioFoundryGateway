// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The MQTT network's scenes (rdof/mqttnet.h): calls built as bursts with this project's encoders, fed through the
 * tracker into the uplink, the messages and activity events kept. fixtures/mqtt-net/gen_vectors.c prints them as
 * fixtures/mqtt-net/vectors.json; test_mqttnet.c runs them again and checks the C still makes the same bytes.
 *
 * The test IDs are 999xxxx (fixtures/README.md). Every scene is on slot 1 at color code 1, heard at -87 dBm.
 */
#ifndef MQTTNET_SCENES_H
#define MQTTNET_SCENES_H

#include <stdio.h>
#include <string.h>

#include "rdof/bits.h"
#include "rdof/fec.h"
#include "rdof/ipgw.h"
#include "rdof/layer2.h"
#include "rdof/mmdvm.h"
#include "rdof/mqttnet.h"
#include "rdof/pdu.h"
#include "rdof/tracker.h"

#define SC_MSGS 64
#define SC_EVENTS 48
#define SC_WALL 1793000000.0 /* 2026-10-26T07:33:20Z: the events' clock is this plus the scene's */

static const uint8_t SC_SILENCE[9] = {0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B};

typedef struct {
    int kind;
    uint32_t dst;
    size_t len;
    uint8_t msg[RDOF_MN_MSG_MAX];
} sc_msg_t;

typedef struct {
    const char *name, *what;
    int nmsg, nev;
    sc_msg_t msg[SC_MSGS];
    char ev[SC_EVENTS][1200];
    /* data calls the uplink handed over (the confirmed-data answer's input) */
    int ndata;
    rdof_mn_data_t data[4];
} sc_out_t;

typedef struct {
    rdof_tracker_t tr;
    rdof_mn_up_t up;
    sc_out_t *out;
    double t;
    uint32_t rnd;
} sc_run_t;

static void sc_publish(void *ctx, int kind, uint32_t dst, const uint8_t *msg, size_t len)
{
    sc_run_t *r = ctx;
    if (r->out->nmsg >= SC_MSGS || len > RDOF_MN_MSG_MAX)
        return;
    sc_msg_t *m = &r->out->msg[r->out->nmsg++];
    m->kind = kind;
    m->dst = dst;
    m->len = len;
    memcpy(m->msg, msg, len);
}

static void sc_activity(void *ctx, const char *json)
{
    sc_run_t *r = ctx;
    if (r->out->nev < SC_EVENTS)
        snprintf(r->out->ev[r->out->nev++], sizeof r->out->ev[0], "%s", json);
}

static void sc_data(void *ctx, const rdof_mn_data_t *d)
{
    sc_run_t *r = ctx;
    if (r->out->ndata < 4)
        r->out->data[r->out->ndata++] = *d; /* data points into the uplink: read it before the next frame */
}

static uint32_t sc_rand(void *ctx)
{
    sc_run_t *r = ctx;
    r->rnd = r->rnd * 1103515245u + 12345u;
    return r->rnd;
}

static double sc_wall(void *ctx) { return SC_WALL + ((sc_run_t *)ctx)->t; }
static int sc_path(void *ctx, int slot) { return 1; }

static void sc_frame(void *ctx, const char *ch, const rdof_frame_t *f)
{
    sc_run_t *r = ctx;
    rdof_mn_up_frame(&r->up, f, r->t);
}

static void sc_begin(sc_run_t *r, sc_out_t *o, const char *name, const char *what)
{
    memset(o, 0, sizeof *o);
    o->name = name;
    o->what = what;
    memset(r, 0, sizeof *r);
    r->out = o;
    r->rnd = 0x5EED0000u;
    rdof_tracker_init(&r->tr, "SDR01", 0.0, 438.5e6);
    rdof_sink_t s = {0};
    s.on_frame = sc_frame;
    s.ctx = r;
    rdof_tracker_add_sink(&r->tr, s);
    rdof_mn_up_init(&r->up, "box-a");
    r->up.tx_ok = 1u << 1;
    r->up.path_of = sc_path;
    r->up.publish = sc_publish;
    r->up.activity = sc_activity;
    r->up.data_done = sc_data;
    r->up.rand32 = sc_rand;
    r->up.wall = sc_wall;
    r->up.ctx = r;
}

/* One burst heard, then 60 ms on. */
static void sc_hear(sc_run_t *r, uint8_t control, const uint8_t burst[RDOF_BURST_BYTES])
{
    rdof_burst_t b;
    rdof_modem_burst(control, burst, -87.0, 1, 1, &b);
    rdof_tracker_feed(&r->tr, &b, r->t);
    rdof_mn_up_tick(&r->up, r->t);
    r->t += 0.06;
}

static void sc_idle(sc_run_t *r, double seconds)
{
    for (double end = r->t + seconds; r->t < end; r->t += 0.06) {
        rdof_tracker_tick(&r->tr, r->t);
        rdof_mn_up_tick(&r->up, r->t);
    }
}

static const uint64_t SC_DSYNC = RDOF_MS_VOICE ^ RDOF_DATA_XOR;

static void sc_lc(sc_run_t *r, int dt, const uint8_t lc9[9])
{
    uint8_t b[RDOF_BURST_BYTES];
    rdof_lc_burst(lc9, dt, 1, SC_DSYNC, b);
    sc_hear(r, (uint8_t)(RDOF_SYNC_DATA | dt), b);
}

static void sc_block(sc_run_t *r, int dt, const uint8_t payload12[12])
{
    uint8_t b[RDOF_BURST_BYTES];
    rdof_data_burst(payload12, dt, 1, SC_DSYNC, b);
    sc_hear(r, (uint8_t)(RDOF_SYNC_DATA | dt), b);
}

static const int SC_LCSS[6] = {0, 1, 3, 3, 2, 0};

/* Voice burst n of a superframe whose embedded LC is emb (bursts B-E; NULL: none), AMBE+2 silence. */
static void sc_voice(sc_run_t *r, int n, const uint8_t *emb, int pi)
{
    uint8_t voice[216], middle[48], burst[RDOF_BURST_BYTES], raw[128], zero[32] = {0};
    for (int i = 0; i < 3; i++)
        rdof_bytes_to_bits(SC_SILENCE, 9, voice + 72 * i);
    if (n == 0) {
        rdof_u64_to_bits(RDOF_MS_VOICE, 48, middle);
    } else {
        if (emb)
            rdof_embedded_lc_encode(emb, raw);
        rdof_voice_middle(1, emb && n <= 4 ? SC_LCSS[n] : 0, emb && n <= 4 ? raw + 32 * (n - 1) : zero, pi, middle);
    }
    rdof_voice_burst(voice, middle, burst);
    sc_hear(r, n == 0 ? RDOF_SYNC_AUDIO : (uint8_t)n, burst);
}

static void sc_superframe(sc_run_t *r, const uint8_t *emb, int pi)
{
    for (int n = 0; n < 6; n++)
        sc_voice(r, n, emb, pi);
}

/* A talker alias header LC: format 1 (8-bit), the text's length, its first 49 bits. */
static void sc_alias_lc(const char *text, uint8_t lc9[9])
{
    memset(lc9, 0, 9);
    lc9[0] = RDOF_FLCO_TA_HEADER;
    const int n = (int)strlen(text);
    rdof_set_field(lc9, 16, 2, 1);
    rdof_set_field(lc9, 18, 5, (uint64_t)n);
    for (int i = 0; i < 49 && i < 8 * n; i++)
        rdof_set_field(lc9, 23 + i, 1, (uint64_t)((uint8_t)text[i / 8] >> (7 - i % 8) & 1));
}

/* A GPS LC (FLCO 8): position error code, lat and lon (ETSI TS 102 361-2 7.2.16). */
static void sc_gps_lc(int err, double lat, double lon, uint8_t lc9[9])
{
    memset(lc9, 0, 9);
    lc9[0] = RDOF_FLCO_GPS_INFO;
    rdof_set_field(lc9, 20, 3, (uint64_t)err);
    const int32_t x = (int32_t)(lon * (1 << 25) / 360.0), y = (int32_t)(lat * (1 << 24) / 180.0);
    rdof_set_field(lc9, 23, 25, (uint64_t)(uint32_t)x & 0x1FFFFFF);
    rdof_set_field(lc9, 48, 24, (uint64_t)(uint32_t)y & 0xFFFFFF);
}

/* ── the scenes ── */

/* A group call to TG 91: three headers, two superframes and burst A, three terminators. The first superframe's
   embedded LC is the voice LC, the second's a talker alias header ("N0CALL" fits in it), then a GPS LC. */
static void scene_group_call(sc_out_t *o)
{
    static sc_run_t r;
    uint8_t lc[9], ta[9], gps[9];
    sc_begin(&r, o, "group_call",
             "group call 9990001 to TG 91: lc_header, voice A-F (F with the voice LC as embedded_lc), a talker "
             "alias and a GPS LC in later superframes, one terminator for three on air");
    rdof_group_lc(9990001, 91, true, 0, lc);
    sc_alias_lc("N0CALL", ta);
    sc_gps_lc(2, 35.12345, -80.12345, gps);
    for (int i = 0; i < 3; i++)
        sc_lc(&r, RDOF_DT_VOICE_LC_HEADER, lc);
    sc_superframe(&r, lc, 0);
    sc_superframe(&r, ta, 0);
    sc_superframe(&r, gps, 0);
    sc_voice(&r, 0, NULL, 0);
    for (int i = 0; i < 3; i++)
        sc_lc(&r, RDOF_DT_TERMINATOR_WITH_LC, lc);
    sc_idle(&r, 1.0);
}

/* A private call heard from its burst C on (no header), the carrier lost after two superframes. */
static void scene_late_entry(sc_out_t *o)
{
    static sc_run_t r;
    uint8_t lc[9];
    sc_begin(&r, o, "late_entry",
             "private call 9990001 to 9990002, emergency, heard from burst C on: the voice before the embedded LC is "
             "held (the last 6 bursts), then sent with the late flag, the LC first; the call is lost, so a "
             "terminator from the LC ends it");
    rdof_group_lc(9990001, 9990002, false, RDOF_SO_EMERGENCY, lc);
    for (int n = 2; n < 6; n++)
        sc_voice(&r, n, lc, 0);
    sc_superframe(&r, lc, 0);
    sc_superframe(&r, lc, 0);
    sc_idle(&r, 1.5);
}

/* A group call with a privacy indicator header after its LC header. */
static void scene_pi_call(sc_out_t *o)
{
    static sc_run_t r;
    uint8_t lc[9], pi[12] = {0};
    sc_begin(&r, o, "pi_call", "group call 9990003 to TG 9 with a PI header: pi_header item, the privacy flag on");
    rdof_group_lc(9990003, 9, true, 0x40, lc); /* SO: privacy */
    pi[0] = 0x21; /* algorithm 1, key 0x21 (made up) */
    pi[1] = 0x00;
    rdof_put_be32(pi + 2, 0xDEADBEEFu); /* the IV */
    rdof_put_be24(pi + 7, 9);
    const uint16_t c = rdof_crc16_ccitt(pi, 10) ^ RDOF_MASK_PI_HEADER;
    pi[10] = (uint8_t)(c >> 8);
    pi[11] = (uint8_t)c;
    sc_lc(&r, RDOF_DT_VOICE_LC_HEADER, lc);
    sc_block(&r, RDOF_DT_PI_HEADER, pi);
    sc_superframe(&r, lc, 1);
    sc_lc(&r, RDOF_DT_TERMINATOR_WITH_LC, lc);
    sc_idle(&r, 1.0);
}

#if RDOF_FEATURE_DATA_BLOCKS
/* An ARS-like UDP datagram as unconfirmed rate 1/2 data, after 4 preambles (built by the IP gateway's encoder). */
static void scene_unconfirmed(sc_out_t *o)
{
    static sc_run_t r;
    static rdof_ipgw_dmr_t p;
    uint8_t ip[64], c[12] = {0};
    const uint8_t text[] = "ARS hello";
    sc_begin(&r, o, "unconfirmed_data",
             "4 CSBK preambles, then an unconfirmed rate 1/2 IP datagram 9990001 to 9990002 (SAP 4): one message, a "
             "csbk item with count 4, the data_pdu (no pad, no CRC-32) and quality");
    const size_t n = rdof_ipgw_udp_datagram(0x0C987531u, 4005, 0x0D987532u, 4005, 1, text, sizeof text - 1, ip,
                                            sizeof ip);
    rdof_ipgw_build_dmr(9990001, 9990002, 4, ip, n, 1, SC_DSYNC, &p);
    for (int k = 0; k < 4; k++) {
        c[0] = 0x80 | RDOF_CSBK_PREAMBLE; /* last block */
        c[1] = 0;
        c[2] = 0x80;  /* data follows, individual */
        c[3] = (uint8_t)(3 - k + p.nbursts); /* blocks to follow: the preambles left and the data's */
        rdof_put_be24(c + 4, 9990002);
        rdof_put_be24(c + 7, 9990001);
        const uint16_t crc = rdof_crc16_ccitt(c, 10) ^ RDOF_MASK_CSBK;
        c[10] = (uint8_t)(crc >> 8);
        c[11] = (uint8_t)crc;
        sc_block(&r, RDOF_DT_CSBK, c);
    }
    for (int k = 0; k < p.nbursts; k++)
        sc_hear(&r, p.control[k], p.bursts[k]);
    sc_idle(&r, 2.0);
}

/* A confirmed data call: rate (RDOF_MN_RATE_*), n octets of a counting pattern, published with max_msg. */
static void scene_confirmed(sc_out_t *o, const char *name, const char *what, int rate, size_t n, size_t max_msg)
{
    static sc_run_t r;
    static uint8_t controls[RDOF_MN_DATA_BURSTS], bursts[RDOF_MN_DATA_BURSTS][RDOF_BURST_BYTES];
    uint8_t data[400], h[12];
    sc_begin(&r, o, name, what);
    r.up.max_msg = max_msg;
    for (size_t i = 0; i < n; i++)
        data[i] = (uint8_t)(i * 7 + 1);
    rdof_ipgw_data_header(false, true, true, 4, 0, 9990002, 9990001, 0, 3, h);
    const int nb = rdof_mn_data_bursts(rate, 1, h, data, n, SC_DSYNC, controls, bursts);
    for (int k = 0; k < nb; k++)
        sc_hear(&r, controls[k], bursts[k]);
    sc_idle(&r, 2.0);
}
#endif

/* A radio check (CSBK 0x24, a request) sent twice: one message, count 1; the repeat is folded. */
static void scene_radio_check(sc_out_t *o)
{
    static sc_run_t r;
    uint8_t c[12] = {0};
    sc_begin(&r, o, "radio_check", "a radio check 9990001 to 9990002 sent twice: one csbk item, count 1, at once");
    c[0] = 0x80 | 0x24;
    c[1] = 0;
    c[2] = 0xCC;
    c[3] = 0x80;
    rdof_put_be24(c + 4, 9990002);
    rdof_put_be24(c + 7, 9990001);
    const uint16_t crc = rdof_crc16_ccitt(c, 10) ^ RDOF_MASK_CSBK;
    c[10] = (uint8_t)(crc >> 8);
    c[11] = (uint8_t)crc;
    sc_block(&r, RDOF_DT_CSBK, c);
    sc_block(&r, RDOF_DT_CSBK, c);
    sc_idle(&r, 2.0);
}

#define SC_SCENES 7
static sc_out_t sc_outs[SC_SCENES];

/* Runs every scene; returns how many. */
static int sc_run_all(void)
{
    int k = 0;
    scene_group_call(&sc_outs[k++]);
    scene_late_entry(&sc_outs[k++]);
    scene_pi_call(&sc_outs[k++]);
    scene_radio_check(&sc_outs[k++]);
#if RDOF_FEATURE_DATA_BLOCKS
    scene_unconfirmed(&sc_outs[k++]);
    scene_confirmed(&sc_outs[k++], "confirmed_parts",
                    "confirmed rate 3/4 data, 300 octets, 9990001 to 9990002, published with max_msg 200: the call in "
                    "parts (bit 7 of part set on all but the last; the headers in part 0)",
                    RDOF_MN_RATE_34, 300, 200);
    scene_confirmed(&sc_outs[k++], "confirmed_rate1",
                    "confirmed rate 1 data, 280 octets, 9990001 to 9990002: one data_pdu item over 255 octets (the u16 "
                    "length)",
                    RDOF_MN_RATE_1, 280, 0);
#endif
    return k;
}

/* ── vectors made by hand with the writer ── */

typedef struct {
    const char *name, *what;
    size_t len;
    uint8_t msg[200];
} sc_hand_t;

#define SC_HANDS 8
static sc_hand_t sc_hands[SC_HANDS];

static const rdof_mn_hdr_t SC_SPEC_HDR = {.flags = RDOF_MN_F_GROUP, .path = 1, .seq = 0, .stream = 0x12345678u,
                                          .src = 9801234, .dst = 91};

static int sc_make_hands(void)
{
    int k = 0;
    rdof_mn_writer_t w;
    uint8_t lc[9], ambe[27];
    rdof_mn_hdr_t h;
    for (int i = 0; i < 3; i++)
        memcpy(ambe + 9 * i, SC_SILENCE, 9);
#define HAND(nm, wt) (sc_hands[k].name = nm, sc_hands[k].what = wt, &sc_hands[k])
#define HAND_END() (sc_hands[k].len = rdof_mn_end(&w), k++)
    /* section 13: normative */
    h = SC_SPEC_HDR;
    rdof_mn_begin(&w, HAND("spec_group_start", "section 13: call start, group call to TG 91, seq 0")->msg, 200, &h);
    rdof_group_lc(9801234, 91, true, 0, lc);
    rdof_mn_lc(&w, RDOF_MN_LC_HEADER, lc);
    rdof_mn_quality(&w, 3, -87);
    HAND_END();
    h.seq = 2;
    rdof_mn_begin(&w, HAND("spec_voice_a", "section 13: voice burst A of that call, AMBE+2 silence, seq 2")->msg, 200,
                  &h);
    rdof_mn_voice(&w, 0, ambe);
    rdof_mn_quality(&w, 3, -87);
    HAND_END();
    h.flags = 0;
    h.seq = 0;
    h.dst = 9990;
    rdof_mn_begin(&w, HAND("spec_private_start", "section 13: private call to 9990, call start, seq 0")->msg, 200, &h);
    rdof_group_lc(9801234, 9990, false, 0, lc);
    rdof_mn_lc(&w, RDOF_MN_LC_HEADER, lc);
    rdof_mn_quality(&w, 3, -87);
    HAND_END();
    h.path = 0;
    rdof_mn_begin(&w, HAND("spec_private_dl", "section 13: the same on dl to a box where 9990's path is 0")->msg, 200,
                  &h);
    rdof_mn_lc(&w, RDOF_MN_LC_HEADER, lc);
    rdof_mn_quality(&w, 3, -87);
    HAND_END();
    /* the rest */
    h = SC_SPEC_HDR;
    h.src = 9990001;
    h.seq = 3;
    rdof_mn_begin(&w, HAND("bundled_voice", "three voice items in one message (B, C, D), then quality: seq 3, next 7")
                          ->msg,
                  200, &h);
    for (int i = 1; i <= 3; i++)
        rdof_mn_voice(&w, i, ambe);
    rdof_mn_quality(&w, 0, -87);
    HAND_END();
    h.seq = 9;
    {
        uint8_t raw[1 + RDOF_BURST_BYTES] = {12};
        for (int i = 0; i < RDOF_BURST_BYTES; i++)
            raw[1 + i] = (uint8_t)(0xA0 + i);
        rdof_mn_begin(&w, HAND("raw", "a raw item: data type 12 (reserved) and the 33 octets as heard")->msg, 200, &h);
        rdof_mn_item(&w, RDOF_MN_RAW, raw, sizeof raw);
        rdof_mn_quality(&w, 0, -87);
        HAND_END();
    }
    h.seq = 4;
    {
        const uint8_t odd[3] = {1, 2, 3};
        rdof_mn_begin(&w, HAND("unknown_item", "voice E, an item of unknown type 0x90 (skipped), quality: 3 items")
                              ->msg,
                      200, &h);
        rdof_mn_voice(&w, 4, ambe);
        rdof_mn_item(&w, 0x90, odd, sizeof odd);
        rdof_mn_quality(&w, 0, -87);
        HAND_END();
    }
    h.seq = 0;
    {
        uint8_t org[9];
        rdof_put_be32(org, 0);
        org[0] = 0x2A, org[1] = 0, org[2] = 0, org[3] = 0; /* network 42, little endian */
        org[4] = 0x15, org[5] = 0xCD, org[6] = 0x5B, org[7] = 0x07; /* repeater 123456789 */
        org[8] = 1;
        rdof_mn_begin(&w, HAND("origin", "a bridged call's first message: origin (network 42, repeater 123456789, "
                                         "1 hop), lc_header")
                              ->msg,
                      200, &h);
        rdof_mn_item(&w, RDOF_MN_ORIGIN, org, sizeof org);
        rdof_group_lc(9990001, 91, true, 0, lc);
        rdof_mn_lc(&w, RDOF_MN_LC_HEADER, lc);
        HAND_END();
    }
#undef HAND
#undef HAND_END
    return k;
}

/* ── the JSON for one message: its header and items, decoded ── */

static size_t sc_hex(char *o, size_t cap, const uint8_t *p, size_t n)
{
    size_t k = 0;
    for (size_t i = 0; i < n && k + 3 < cap; i++)
        k += (size_t)snprintf(o + k, cap - k, "%02x", p[i]);
    o[k] = 0;
    return k;
}

static const char *sc_type_name(int t)
{
    switch (t) {
    case RDOF_MN_VOICE: return "voice";
    case RDOF_MN_LC_HEADER: return "lc_header";
    case RDOF_MN_TERMINATOR: return "terminator";
    case RDOF_MN_EMBEDDED_LC: return "embedded_lc";
    case RDOF_MN_PI_HEADER: return "pi_header";
    case RDOF_MN_CSBK: return "csbk";
    case RDOF_MN_DATA_PDU: return "data_pdu";
    case RDOF_MN_TALKER_ALIAS: return "talker_alias";
    case RDOF_MN_GPS: return "gps";
    case RDOF_MN_QUALITY: return "quality";
    case RDOF_MN_ORIGIN: return "origin";
    case RDOF_MN_RAW: return "raw";
    default: return "unknown";
    }
}

static int32_t sc_le32s(const uint8_t *p) { return (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24); }

/* One message as a JSON object: "hex", "header" {...}, "items" [...]. Returns the length. */
static size_t sc_msg_json(const uint8_t *msg, size_t len, char *o, size_t cap)
{
    rdof_mn_hdr_t h;
    size_t k = 0;
#define P(...) (k += (size_t)snprintf(o + k, k < cap ? cap - k : 0, __VA_ARGS__))
    static char hx[2 * RDOF_MN_MSG_MAX + 1];
    sc_hex(hx, sizeof hx, msg, len);
    rdof_mn_parse(msg, len, &h);
    P("{\"hex\": \"%s\",\n       \"header\": {\"version\": 3, \"flags\": %u, \"slot2\": %s, \"group\": %s, "
      "\"emergency\": %s, \"priority\": %s, \"privacy\": %s, \"late\": %s, \"path\": %u, \"seq\": %u, \"stream\": %lu, "
      "\"src\": %lu, \"dst\": %lu, \"items\": %d},\n       \"items\": [",
      hx, h.flags, h.flags & RDOF_MN_F_SLOT2 ? "true" : "false", h.flags & RDOF_MN_F_GROUP ? "true" : "false",
      h.flags & RDOF_MN_F_EMERGENCY ? "true" : "false", h.flags & RDOF_MN_F_PRIORITY ? "true" : "false",
      h.flags & RDOF_MN_F_PRIVACY ? "true" : "false", h.flags & RDOF_MN_F_LATE ? "true" : "false", h.path, h.seq,
      (unsigned long)h.stream, (unsigned long)h.src, (unsigned long)h.dst, rdof_mn_count(msg, len));
    rdof_mn_reader_t r;
    rdof_mn_item_t it;
    rdof_mn_reader(&r, msg, len);
    bool first = true;
    while (rdof_mn_next(&r, &it)) {
        sc_hex(hx, sizeof hx, it.value, it.len);
        P("%s\n         {\"type\": %d, \"name\": \"%s\", \"value\": \"%s\"", first ? "" : ",", it.type,
          sc_type_name(it.type), hx);
        first = false;
        switch (it.type) {
        case RDOF_MN_VOICE:
            P(", \"index\": %d", it.value[0]);
            break;
        case RDOF_MN_LC_HEADER:
        case RDOF_MN_TERMINATOR:
        case RDOF_MN_EMBEDDED_LC: {
            rdof_full_lc_t lc;
            rdof_parse_full_lc(it.value, &lc);
            P(", \"flco\": %d, \"fid\": %d", lc.flco, lc.fid);
            if (lc.flco == RDOF_FLCO_GROUP || lc.flco == RDOF_FLCO_PRIVATE)
                P(", \"service_options\": %d, \"src\": %ld, \"dst\": %ld", lc.service_options, (long)lc.src_id,
                  (long)lc.dst_id);
            break;
        }
        case RDOF_MN_CSBK:
            P(", \"opcode\": %d, \"count\": %d", it.value[0] & 0x3F, it.value[10]);
            break;
        case RDOF_MN_DATA_PDU: {
            rdof_mn_pdu_t p;
            if (rdof_mn_pdu(&it, &p)) {
                P(", \"part\": %d, \"more\": %s, \"rate\": %d, \"headers\": [", p.part, p.more ? "true" : "false",
                  p.rate);
                for (int i = 0; i < p.nheaders; i++) {
                    sc_hex(hx, sizeof hx, p.headers + 10 * i, 10);
                    P("%s\"%s\"", i ? ", " : "", hx);
                }
                sc_hex(hx, sizeof hx, p.data, p.len);
                P("], \"data\": \"%s\"", hx);
            }
            break;
        }
        case RDOF_MN_TALKER_ALIAS:
            P(", \"format\": %d, \"text\": \"%.*s\"", it.value[0], (int)it.len - 1, (const char *)it.value + 1);
            break;
        case RDOF_MN_GPS:
            P(", \"lat_e7\": %ld, \"lon_e7\": %ld, \"error\": %d", (long)sc_le32s(it.value),
              (long)sc_le32s(it.value + 4), it.value[8]);
            break;
        case RDOF_MN_QUALITY:
            P(", \"bits\": %d, \"rssi\": %d", it.value[0], -(int)it.value[1]);
            break;
        case RDOF_MN_ORIGIN:
            P(", \"network\": %lu, \"repeater\": %lu, \"hops\": %d", (unsigned long)(uint32_t)sc_le32s(it.value),
              (unsigned long)(uint32_t)sc_le32s(it.value + 4), it.value[8]);
            break;
        case RDOF_MN_RAW:
            P(", \"dt\": %d", it.value[0]);
            break;
        default: break;
        }
        P("}");
    }
    P("]}");
#undef P
    return k;
}

#endif
