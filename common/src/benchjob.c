// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The bench's jobs (rdof/benchjob.h).
 */
#include "rdof/benchjob.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/bits.h"
#include "rdof/fec.h"
#include "rdof/features.h"
#include "rdof/layer2.h"
#include "rdof/mmdvm.h"
#include "rdof/pdu.h"
#include "rdof/ambe.h"

#define HEADERS 8               /* an MMDVM hotspot's voice LC headers a call */
#define TERMINATORS 3
#define MAX_BLOCKS 127
#define CRC9_MASK_RATE12 0x0F0  /* confirmed rate 1/2 blocks (TS 102 361-1 B.3.12) */
#define SAP_IP 4
#define SAP_SHORT_DATA 10
#define DD_ISO8859_1 0x03       /* defined data format: ISO 8859-1 (TS 102 361-1 Table 9.50) */
#define TA_ISO8 1

/* AMBE+2 silence, as a 9-byte channel frame with its FEC */
static const uint8_t SILENCE[9] = {0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B};

/* CRC-32 as zlib's crc32 (the tool's content check), chainable from 0 */
static uint32_t crc32_up(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

/* ── the request ──────────────────────────────────────────────────────────── */

static const rdof_jnode_t *node(const rdof_jdoc_t *d, int obj, const char *key)
{
    const rdof_jnode_t *n = rdof_jnode(d, rdof_jget(d, obj, key));
    return n && n->type != RDOF_JNULL ? n : NULL;
}

static double num(const rdof_jdoc_t *d, int obj, const char *key, double dflt)
{
    const rdof_jnode_t *n = node(d, obj, key);
    if (n && n->type == RDOF_JNUM)
        return n->num;
    if (n && n->type == RDOF_JSTR && n->str[0])
        return atof(n->str);
    return dflt;
}

/* argparse's type=lambda s: int(s, 16): a string of hex digits (or a number as sent) */
static int hexnum(const rdof_jdoc_t *d, int obj, const char *key, int dflt)
{
    const rdof_jnode_t *n = node(d, obj, key);
    if (n && n->type == RDOF_JNUM)
        return (int)n->num;
    if (n && n->type == RDOF_JSTR && n->str[0])
        return (int)strtol(n->str, NULL, 16);
    return dflt;
}

static bool flag(const rdof_jdoc_t *d, int obj, const char *key)
{
    const rdof_jnode_t *n = node(d, obj, key);
    return n && ((n->type == RDOF_JBOOL && n->b) || (n->type == RDOF_JNUM && n->num != 0));
}

static bool text(const rdof_jdoc_t *d, int obj, const char *key, char *out, size_t cap)
{
    const rdof_jnode_t *n = node(d, obj, key);
    if (!n || n->type != RDOF_JSTR) {
        out[0] = 0;
        return false;
    }
    snprintf(out, cap, "%s", n->str);
    return true;
}

static int unhex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0;
    for (; s[0] && s[1]; s += 2) {
        unsigned v;
        if (n == cap || sscanf(s, "%2x", &v) != 1)
            return -1;
        out[n++] = (uint8_t)v;
    }
    return s[0] ? -1 : (int)n;
}

bool rdof_bj_parse(const rdof_jdoc_t *d, int obj, rdof_bj_job_t *j, uint8_t *ambe_buf, size_t cap, char *error,
                   size_t ecap)
{
    memset(j, 0, sizeof *j);
    char s[64];
    text(d, obj, "tool", s, sizeof s);
    if (strcmp(s, "bench_tx")) {
        snprintf(error, ecap, "%s jobs aren't done here yet: bench_tx only", s[0] ? s : "no tool:");
        return false;
    }
    text(d, obj, "cmd", s, sizeof s);
    j->cmd = !strcmp(s, "voice") ? RDOF_BJ_VOICE : !strcmp(s, "csbk") ? RDOF_BJ_CSBK : !strcmp(s, "data") ? RDOF_BJ_DATA
           : !strcmp(s, "listen") ? RDOF_BJ_LISTEN : 0;
    if (!j->cmd) {
        snprintf(error, ecap, "bench_tx %s isn't done here (voice, csbk, data, listen)", s);
        return false;
    }
    j->freq_mhz = num(d, obj, "freq", 438.5);
    j->cc = (int)num(d, obj, "cc", 2);
    j->power = (int)num(d, obj, "power", 255);
    j->burst_cc = (int)num(d, obj, "burst_cc", j->cc);
    j->listen_after = num(d, obj, "listen_after", 0);
    text(d, obj, "ambe_out", j->ambe_out, sizeof j->ambe_out);
    if (strchr(j->ambe_out, '/') && strstr(j->ambe_out, "..")) {
        snprintf(error, ecap, "--ambe-out: a name in the tool's directory");
        return false;
    }
    const uint32_t dst = j->cmd == RDOF_BJ_VOICE ? 215 : 9801276;
    j->src = (uint32_t)num(d, obj, "src", 9990301);
    j->dst = (uint32_t)num(d, obj, "dst", dst);
    j->src2 = (uint32_t)num(d, obj, "src2", 9990302);
    text(d, obj, "family", s, sizeof s);
    j->family = !s[0] || !strcmp(s, "ms") ? RDOF_FAMILY_MS : !strcmp(s, "bs") ? RDOF_FAMILY_BS
              : !strcmp(s, "ts1") ? RDOF_FAMILY_TS1 : !strcmp(s, "ts2") ? RDOF_FAMILY_TS2 : -1;
    if (j->family < 0) {
        snprintf(error, ecap, "--family %s: ms, bs, ts1 or ts2", s);
        return false;
    }
    /* voice */
    j->priv = flag(d, obj, "private");
    j->pi = flag(d, obj, "pi");
    j->emergency = flag(d, obj, "emergency");
    j->superframes = (int)num(d, obj, "superframes", 3);
    j->headers = (int)num(d, obj, "headers", -1);
    j->terminators = (int)num(d, obj, "terminators", -1);
    j->late = (int)num(d, obj, "late", -1);
    j->calls = (int)num(d, obj, "calls", 1);
    j->gap_ms = (int)num(d, obj, "gap_ms", 120);
    j->service_options = hexnum(d, obj, "service_options", 0) & 0xFF;
    text(d, obj, "talker_alias", j->talker_alias, sizeof j->talker_alias);
    if (text(d, obj, "gps", s, sizeof s)) {
        j->gps_err = 1;
        if (sscanf(s, "%lf,%lf,%d", &j->lat, &j->lon, &j->gps_err) < 2) {
            snprintf(error, ecap, "--gps %s: LAT,LON[,ERR]", s);
            return false;
        }
        j->gps = true;
    }
    const rdof_jnode_t *a = node(d, obj, "ambe_hex");
    if (a && a->type == RDOF_JSTR) {
        const int n = unhex(a->str, ambe_buf, cap);
        if (n < 0) {
            snprintf(error, ecap, "--ambe: more than %zu bytes, or not hex", cap);
            return false;
        }
        if (n >= 4 && !memcmp(ambe_buf, ".amb", 4)) {
            snprintf(error, ecap, "--ambe: a DSD .amb file has no FEC; give a 9-byte-per-frame .ambe recording");
            return false;
        }
        j->ambe = ambe_buf;
        j->ambe_len = (size_t)n / 9 * 9;
    }
    if (j->superframes < 1 || j->superframes > 200 || j->calls < 1 || j->calls > 50) {
        snprintf(error, ecap, "--superframes 1-200, --calls 1-50");
        return false;
    }
    /* csbk */
    j->count = (int)num(d, obj, "count", 4);
    j->opcode = hexnum(d, obj, "opcode", -1);
    j->fid = hexnum(d, obj, "fid", 0);
    j->octet3 = hexnum(d, obj, "octet3", 0);
    /* data */
    j->confirmed = flag(d, obj, "confirmed");
    j->tms_no_ack = flag(d, obj, "tms_no_ack");
    j->etsi_no_response = flag(d, obj, "etsi_no_response");
    j->group = flag(d, obj, "group");
    j->has_tms = text(d, obj, "tms", j->tms, sizeof j->tms);
    j->has_etsi = text(d, obj, "etsi", j->etsi, sizeof j->etsi);
    j->has_ip_text = text(d, obj, "ip_text", j->ip_text, sizeof j->ip_text);
    j->octets = (int)num(d, obj, "octets", 40);
    j->tms_seq = (int)num(d, obj, "tms_seq", 1);
    j->udp_port = (int)num(d, obj, "udp_port", -1);
    j->udp_src_port = (int)num(d, obj, "udp_src_port", -1);
    j->udp_octets = (int)num(d, obj, "udp_octets", 40);
    j->preambles = (int)num(d, obj, "preambles", 2);
    j->repeat = (int)num(d, obj, "repeat", 1);
    j->repeat_gap_ms = (int)num(d, obj, "repeat_gap_ms", 0);
    j->udp_payload_len = -1;
    const rdof_jnode_t *u = node(d, obj, "udp_payload");
    if (u && u->type == RDOF_JSTR && (j->udp_payload_len = unhex(u->str, j->udp_payload, sizeof j->udp_payload)) < 0) {
        snprintf(error, ecap, "--udp-payload: hex, %d octets at most", RDOF_BJ_UDP);
        return false;
    }
    if (j->repeat < 1)
        j->repeat = 1;
    if (j->repeat > 100 || j->preambles < 0 || j->preambles > 64 || j->count < 0 || j->count > 64 ||
        j->octets < 0 || j->octets > 1500 || j->udp_octets < 0 || j->udp_octets > RDOF_BJ_UDP) {
        snprintf(error, ecap, "--repeat 1-100, --preambles / --count 0-64, --octets 0-1500, --udp-octets 0-%d",
                 RDOF_BJ_UDP);
        return false;
    }
    /* listen */
    j->seconds = num(d, obj, "seconds", 20);
    j->json = flag(d, obj, "json");
    if (j->seconds < 0 || j->seconds > 3600 || j->listen_after < 0 || j->listen_after > 3600) {
        snprintf(error, ecap, "--seconds, --listen-after: 0-3600");
        return false;
    }
    return true;
}

/* ── bursts ───────────────────────────────────────────────────────────────── */

static uint64_t voice_sync(const rdof_bj_job_t *j) { return rdof_family_voice_sync((rdof_family_t)j->family); }
static uint64_t data_sync(const rdof_bj_job_t *j) { return rdof_data_sync(voice_sync(j)); }

static void call_lc(uint32_t src, uint32_t dst, bool group, int so, uint8_t lc[9])
{
    lc[0] = group ? RDOF_FLCO_GROUP : RDOF_FLCO_PRIVATE;
    lc[1] = 0;
    lc[2] = (uint8_t)so;
    rdof_put_be24(lc + 3, dst);
    rdof_put_be24(lc + 6, src);
}

/* Talker alias LCs (TS 102 361-2 7.2.18), ISO 8-bit: 6 characters in the header (its first data bit unused), 7 a
 * block. Returns how many (header and blocks). */
static int talker_alias_lcs(const char *s, uint8_t out[4][9])
{
    uint8_t data[27] = {0};
    const size_t n = strlen(s) < 27 ? strlen(s) : 27;
    memcpy(data, s, n); /* the text as given: ISO 8859-1 octets (UTF-8 passes through, octet for octet) */
    uint8_t bits[8 + 27 * 8] = {0};
    bits[0] = 0, bits[1] = 1; /* format ISO 8-bit */
    for (int i = 0; i < 5; i++)
        bits[2 + i] = (uint8_t)((n >> (4 - i)) & 1);
    bits[7] = 0;              /* unused in the 8-bit formats */
    rdof_bytes_to_bits(data, 27, bits + 8);
    out[0][0] = RDOF_FLCO_TA_HEADER, out[0][1] = 0;
    rdof_bits_to_bytes(bits, 56, out[0] + 2);
    int k = 1;
    for (int i = 0; i < 3 && n > (size_t)(6 + 7 * i); i++, k++) {
        out[k][0] = (uint8_t)(RDOF_FLCO_TA_HEADER + 1 + i), out[k][1] = 0;
        rdof_bits_to_bytes(bits + 56 + 56 * i, 56, out[k] + 2);
    }
    return k;
}

/* GPS Info LC (FLCO 8): position error 3 bits, longitude 25 bits (360 / 2^25 degree), latitude 24 (180 / 2^24) */
static void gps_lc(double lat, double lon, int err, uint8_t lc[9])
{
    long lo = lround(lon * (double)(1 << 25) / 360.0), la = lround(lat * (double)(1 << 24) / 180.0);
    lo = lo < -(1L << 24) ? -(1L << 24) : lo > (1L << 24) - 1 ? (1L << 24) - 1 : lo;
    la = la < -(1L << 23) ? -(1L << 23) : la > (1L << 23) - 1 ? (1L << 23) - 1 : la;
    const uint32_t ulo = (uint32_t)lo & 0x1FFFFFF, ula = (uint32_t)la & 0xFFFFFF;
    lc[0] = RDOF_FLCO_GPS_INFO, lc[1] = 0;
    lc[2] = (uint8_t)(((err & 7) << 1) | (ulo >> 24));
    rdof_put_be24(lc + 3, ulo & 0xFFFFFF);
    rdof_put_be24(lc + 6, ula);
}

static void push(rdof_bj_burst_t *out, int *n, int cap, uint8_t control, const uint8_t burst[33])
{
    if (*n < cap) {
        out[*n].control = control;
        memcpy(out[*n].burst, burst, 33);
    }
    (*n)++;
}

static int voice_frames(const rdof_bj_job_t *j) { return j->ambe ? (int)(j->ambe_len / 9) : 18 * j->superframes; }

static int build_voice(const rdof_bj_job_t *j, int call, rdof_bj_burst_t *out, int cap, char *note, char *done,
                       size_t ncap)
{
    const uint32_t src = call % 2 == 0 ? j->src : j->src2;
    const int cc = j->burst_cc;
    uint8_t lc[9];
    call_lc(src, j->dst, !j->priv, j->service_options | (j->emergency ? 0x80 : 0), lc);
    uint8_t extra[5][9];
    int nextra = j->talker_alias[0] ? talker_alias_lcs(j->talker_alias, extra) : 0;
    if (j->gps)
        gps_lc(j->lat, j->lon, j->gps_err, extra[nextra++]);
    const int nframes = voice_frames(j);
    const int superframes = nframes ? (nframes + 17) / 18 : 1;
    const uint64_t vs = voice_sync(j);
    int n = 0;
    uint8_t b[33];
    if (j->late < 0) {
        rdof_lc_burst(lc, RDOF_DT_VOICE_LC_HEADER, cc, rdof_data_sync(vs), b);
        for (int i = 0; i < (j->headers < 0 ? HEADERS : j->headers < 1 ? 1 : j->headers); i++)
            push(out, &n, cap, RDOF_SYNC_DATA | RDOF_DT_VOICE_LC_HEADER, b);
    }
    const int voice0 = n;
    int k = 0, skip = j->late > 0 ? j->late : 0;
    uint32_t crc_frames = 0; /* of the frames sent, for the tool's log */
    long sent_frames = 0;
    for (int sf = 0; sf < superframes; sf++) {
        /* the embedded LC: the call's in even superframes, a talker alias or GPS Info LC in odd ones, in turn */
        const uint8_t *e = !nextra || sf % 2 == 0 ? lc : extra[(sf / 2) % nextra];
        uint8_t emb[128];
        rdof_embedded_lc_encode(e, emb);
        for (int v = 0; v < 6; v++) {
            uint8_t frames[27];
            for (int f = 0; f < 3; f++, k++)
                memcpy(frames + 9 * f, k < nframes && j->ambe ? j->ambe + 9 * k : SILENCE, 9);
            uint8_t bits[216], mid[48];
            rdof_bytes_to_bits(frames, 27, bits);
            if (v == 0) {
                for (int i = 0; i < 48; i++)
                    mid[i] = (uint8_t)((vs >> (47 - i)) & 1);
            } else if (v <= 4) {
                static const int lcss[4] = {1, 3, 3, 2};
                rdof_voice_middle(cc, lcss[v - 1], emb + 32 * (v - 1), j->pi, mid);
            } else {
                static const uint8_t zero[32];
                rdof_voice_middle(cc, 0, zero, j->pi, mid);
            }
            rdof_voice_burst(bits, mid, b);
            if (skip > 0) {
                skip--;
                continue;
            }
            push(out, &n, cap, v == 0 ? RDOF_SYNC_AUDIO : (uint8_t)v, b);
            crc_frames = crc32_up(crc_frames, frames, 27);
            sent_frames += 3;
        }
    }
    (void)voice0;
    if (j->terminators != 0) {
        rdof_lc_burst(lc, RDOF_DT_TERMINATOR_WITH_LC, cc, rdof_data_sync(vs), b);
        for (int i = 0; i < (j->terminators < 0 ? TERMINATORS : j->terminators); i++)
            push(out, &n, cap, RDOF_SYNC_DATA | RDOF_DT_TERMINATOR_WITH_LC, b);
    }
    snprintf(note, ncap, "call from %u: %ld AMBE frames, CRC-32 %08X (the radio's RX content check)", (unsigned)src,
             sent_frames, (unsigned)crc_frames);
    snprintf(done, ncap, "call %d from %u done", call + 1, (unsigned)src);
    return n;
}

static void csbk_info(int opcode, int fid, int o2, int o3, uint32_t dst, uint32_t src, uint8_t info[12])
{
    info[0] = (uint8_t)(0x80 | (opcode & 0x3F)); /* last block */
    info[1] = (uint8_t)fid, info[2] = (uint8_t)o2, info[3] = (uint8_t)o3;
    rdof_put_be24(info + 4, dst);
    rdof_put_be24(info + 7, src);
    const uint16_t crc = rdof_crc16_ccitt(info, 10) ^ RDOF_MASK_CSBK;
    info[10] = (uint8_t)(crc >> 8), info[11] = (uint8_t)crc;
}

static void push_info(const rdof_bj_job_t *j, rdof_bj_burst_t *out, int *n, int cap, int dt, const uint8_t info[12])
{
    uint8_t b[33];
    rdof_data_burst(info, dt, j->burst_cc, data_sync(j), b);
    push(out, n, cap, (uint8_t)(RDOF_SYNC_DATA | dt), b);
}

static int build_csbk(const rdof_bj_job_t *j, rdof_bj_burst_t *out, int cap, char *done, size_t ncap)
{
    int n = 0;
    uint8_t info[12];
    /* preambles: "CSBK content follows", each counting the CSBKs still to come, the last one included */
    for (int i = 0; i < j->count; i++) {
        csbk_info(RDOF_CSBK_PREAMBLE, 0, 0, (j->count - i) & 0xFF, j->dst, j->src, info);
        push_info(j, out, &n, cap, RDOF_DT_CSBK, info);
    }
    if (j->opcode >= 0) {
        csbk_info(j->opcode, j->fid, 0, j->octet3, j->dst, j->src, info);
        push_info(j, out, &n, cap, RDOF_DT_CSBK, info);
    }
    snprintf(done, ncap, "csbk: %d burst(s) sent", n);
    return n;
}

/* ── data ── */

static uint16_t ip_sum(const uint8_t *p, size_t n, uint32_t s)
{
    for (size_t i = 0; i + 1 < n; i += 2)
        s += (uint32_t)(p[i] << 8 | p[i + 1]);
    if (n & 1)
        s += (uint32_t)p[n - 1] << 8;
    while (s >> 16)
        s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}

/* IPv4 / UDP from 12.<src> to 12.<dst> (the CAI network), the checksums set: the length */
static size_t udp_datagram(uint32_t src, uint32_t dst, int sport, int dport, uint16_t ident, const uint8_t *pl,
                           size_t len, uint8_t *out)
{
    const size_t ulen = 8 + len, total = 20 + ulen;
    uint8_t *ip = out, *u = out + 20;
    memset(out, 0, 28);
    ip[0] = 0x45;
    ip[2] = (uint8_t)(total >> 8), ip[3] = (uint8_t)total;
    ip[4] = (uint8_t)(ident >> 8), ip[5] = (uint8_t)ident;
    ip[8] = 64, ip[9] = 17;
    ip[12] = 12, rdof_put_be24(ip + 13, src);
    ip[16] = 12, rdof_put_be24(ip + 17, dst);
    const uint16_t hs = ip_sum(ip, 20, 0);
    ip[10] = (uint8_t)(hs >> 8), ip[11] = (uint8_t)hs;
    u[0] = (uint8_t)(sport >> 8), u[1] = (uint8_t)sport, u[2] = (uint8_t)(dport >> 8), u[3] = (uint8_t)dport;
    u[4] = (uint8_t)(ulen >> 8), u[5] = (uint8_t)ulen;
    memcpy(u + 8, pl, len);
    uint8_t pseudo[12];
    memcpy(pseudo, ip + 12, 8);
    pseudo[8] = 0, pseudo[9] = 17, pseudo[10] = (uint8_t)(ulen >> 8), pseudo[11] = (uint8_t)ulen;
    uint32_t s = 0;
    for (int i = 0; i < 12; i += 2)
        s += (uint32_t)(pseudo[i] << 8 | pseudo[i + 1]);
    uint16_t us = ip_sum(u, ulen, s);
    if (!us)
        us = 0xFFFF;
    u[6] = (uint8_t)(us >> 8), u[7] = (uint8_t)us;
    return total;
}

/* UTF-8 to UTF-16 code units (BMP; anything else U+FFFD) */
static int utf16(const char *s, uint16_t *out, int cap)
{
    int n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && n < cap) {
        uint32_t c = *p++;
        if (c >= 0xC0 && c < 0xE0 && (*p & 0xC0) == 0x80)
            c = (c & 0x1F) << 6 | (*p++ & 0x3F);
        else if (c >= 0xE0 && c < 0xF0 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80)
            c = (c & 0x0F) << 12 | (p[0] & 0x3F) << 6 | (p[1] & 0x3F), p += 2;
        else if (c >= 0x80)
            c = 0xFFFD;
        out[n++] = (uint16_t)c;
    }
    return n;
}

/* The user data of packet index (IPv4 for --tms, --ip-text, --udp-port; else --octets of filler): its length */
static size_t user_data(const rdof_bj_job_t *j, int index, uint8_t *out)
{
    uint8_t pl[RDOF_BJ_UDP + 400];
    uint16_t u16[RDOF_BJ_TEXT];
    if (j->has_tms) {
        const int n = utf16(j->tms, u16, RDOF_BJ_TEXT);
        const size_t body = 4 + 2 * (size_t)n;
        pl[0] = (uint8_t)(body >> 8), pl[1] = (uint8_t)body;
        pl[2] = j->tms_no_ack ? 0xA0 : 0xE0, pl[3] = 0, pl[4] = (uint8_t)(0x80 | (j->tms_seq & 0x1F)), pl[5] = 4;
        for (int i = 0; i < n; i++)
            pl[6 + 2 * i] = (uint8_t)u16[i], pl[7 + 2 * i] = (uint8_t)(u16[i] >> 8); /* UTF-16LE */
        return udp_datagram(j->src, j->dst, 4007, 4007, 1, pl, 2 + body, out);
    }
    if (j->has_ip_text) {
        const int n = utf16(j->ip_text, u16, RDOF_BJ_TEXT);
        for (int i = 0; i < n; i++)
            pl[2 * i] = (uint8_t)(u16[i] >> 8), pl[2 * i + 1] = (uint8_t)u16[i]; /* UTF-16BE */
        return udp_datagram(j->src, j->dst, 5016, 5016, 1, pl, 2 * (size_t)n, out);
    }
    if (j->udp_port >= 0) {
        size_t len;
        if (j->udp_payload_len >= 0) {
            len = (size_t)j->udp_payload_len;
            memcpy(pl, j->udp_payload, len);
        } else {
            len = (size_t)j->udp_octets;
            for (size_t i = 0; i < len; i++)
                pl[i] = (uint8_t)((i * 7 + 1) & 0xFF);
        }
        if (len >= 2) /* the packet's index first, big endian */
            pl[0] = (uint8_t)(index >> 8), pl[1] = (uint8_t)index;
        const int sport = j->udp_src_port >= 0 ? j->udp_src_port : j->udp_port;
        return udp_datagram(j->src, j->dst, sport, j->udp_port, (uint16_t)(1 + index), pl, len, out);
    }
    for (int i = 0; i < j->octets; i++)
        out[i] = (uint8_t)((i * 7 + 1) & 0xFF);
    return (size_t)j->octets;
}

static void preamble(const rdof_bj_job_t *j, bool group, int blocks, uint8_t info[12])
{
    /* "data content follows", the bursts still to come */
    csbk_info(RDOF_CSBK_PREAMBLE, 0, 0x80 | (group ? 0x40 : 0), blocks & 0xFF, j->dst, j->src, info);
}

/* The data block CRC-32 (TS 102 361-1 B.3.9): polynomial 0x04C11DB7, from 0, over the octets in swapped pairs
 * (1, 0, 3, 2, ...; an odd last one paired with a zero), sent least significant octet first */
static void put_crc32(uint8_t *body, size_t len)
{
    uint32_t crc = 0;
    const size_t n = len + (len & 1);
    for (size_t i = 0; i < n; i++) {
        const size_t x = i ^ 1;
        crc ^= (uint32_t)(x < len ? body[x] : 0) << 24;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
    }
    for (int i = 0; i < 4; i++)
        body[len + (size_t)i] = (uint8_t)(crc >> (8 * i));
}

static int build_etsi(const rdof_bj_job_t *j, rdof_bj_burst_t *out, int cap, int *n)
{
    /* defined short data (DPF 13, SAP 10, ISO 8859-1): header, rate 1/2 blocks, the CRC-32 in the last */
    const size_t len = strlen(j->etsi);
    const int blocks = (int)((len + 4 + 11) / 12);
    const int pad = blocks * 12 - 4 - (int)len;
    const bool response = !j->etsi_no_response && !j->group;
    uint8_t info[12];
    for (int k = 0; k < j->preambles; k++) {
        preamble(j, j->group, blocks + 1 + j->preambles - 1 - k, info);
        push_info(j, out, n, cap, RDOF_DT_CSBK, info);
    }
    uint8_t h[12];
    h[0] = (uint8_t)((j->group ? 0x80 : 0) | (response ? 0x40 : 0) | (((blocks >> 4) & 3) << 4) |
                     RDOF_DPF_SHORT_DATA_DEFINED);
    h[1] = (uint8_t)(SAP_SHORT_DATA << 4 | (blocks & 0x0F));
    rdof_put_be24(h + 2, j->dst);
    rdof_put_be24(h + 5, j->src);
    h[8] = (uint8_t)(DD_ISO8859_1 << 2 | 1);
    h[9] = (uint8_t)(pad * 8);
    const uint16_t crc = rdof_crc16_ccitt(h, 10) ^ RDOF_MASK_DATA_HEADER;
    h[10] = (uint8_t)(crc >> 8), h[11] = (uint8_t)crc;
    push_info(j, out, n, cap, RDOF_DT_DATA_HEADER, h);
    uint8_t body[RDOF_BJ_TEXT + 16] = {0};
    memcpy(body, j->etsi, len);
    put_crc32(body, len + (size_t)pad);
    for (int b = 0; b < blocks; b++)
        push_info(j, out, n, cap, RDOF_DT_RATE_12, body + 12 * b);
    return 0;
}

static void data_header(const rdof_bj_job_t *j, bool confirmed, int pad, int blocks, uint8_t h[12])
{
    memset(h, 0, 12);
    h[0] = (uint8_t)((confirmed ? 0x40 : 0) | ((pad & 0x10) ? 0x10 : 0) |
                     (confirmed ? RDOF_DPF_CONFIRMED : RDOF_DPF_UNCONFIRMED));
    h[1] = (uint8_t)(SAP_IP << 4 | (pad & 0x0F));
    rdof_put_be24(h + 2, j->dst);
    rdof_put_be24(h + 5, j->src);
    h[8] = (uint8_t)(0x80 | (blocks & 0x7F));         /* F: the whole message */
    h[9] = (uint8_t)((confirmed ? 1 << 4 : 0) | 0x8); /* N(S) 1, FSN: a single fragment */
    const uint16_t crc = rdof_crc16_ccitt(h, 10) ^ RDOF_MASK_DATA_HEADER;
    h[10] = (uint8_t)(crc >> 8), h[11] = (uint8_t)crc;
}

static int build_data(const rdof_bj_job_t *j, int index, rdof_bj_burst_t *out, int cap, int *n, char *error,
                      size_t ecap)
{
    if (j->has_etsi)
        return build_etsi(j, out, cap, n);
    uint8_t user[MAX_BLOCKS * 12];
    const size_t len = user_data(j, index, user);
    const int bl = j->confirmed ? 10 : 12;
    const int blocks = (int)((len + 4 + (size_t)bl - 1) / (size_t)bl);
    if (blocks > MAX_BLOCKS) {
        snprintf(error, ecap, "%zu octets is too long for one packet", len);
        return -1;
    }
    const int pad = blocks * bl - 4 - (int)len;
    uint8_t body[MAX_BLOCKS * 12 + 4];
    memcpy(body, user, len);
    memset(body + len, 0, (size_t)pad);
    put_crc32(body, len + (size_t)pad);
    uint8_t info[12];
    for (int k = 0; k < j->preambles; k++) {
        preamble(j, false, blocks + 1 + j->preambles - 1 - k, info);
        push_info(j, out, n, cap, RDOF_DT_CSBK, info);
    }
    data_header(j, j->confirmed, pad, blocks, info);
    push_info(j, out, n, cap, RDOF_DT_DATA_HEADER, info);
    for (int b = 0; b < blocks; b++) {
        if (!j->confirmed) {
            push_info(j, out, n, cap, RDOF_DT_RATE_12, body + 12 * b);
            continue;
        }
#if RDOF_FEATURE_DATA_BLOCKS
        /* confirmed: DBSN (7 bits), CRC-9 over the octets and the DBSN, 10 octets */
        uint8_t bits[87];
        rdof_bytes_to_bits(body + 10 * b, 10, bits);
        for (int i = 0; i < 7; i++)
            bits[80 + i] = (uint8_t)((b >> (6 - i)) & 1);
        const uint16_t crc = rdof_crc9_dmr(bits, 87) ^ CRC9_MASK_RATE12;
        info[0] = (uint8_t)((b & 0x7F) << 1 | (crc >> 8));
        info[1] = (uint8_t)crc;
        memcpy(info + 2, body + 10 * b, 10);
        push_info(j, out, n, cap, RDOF_DT_RATE_12, info);
#else
        snprintf(error, ecap, "--confirmed: this build has no confirmed data");
        return -1;
#endif
    }
    return 0;
}

int rdof_bj_parts(const rdof_bj_job_t *j)
{
    switch (j->cmd) {
    case RDOF_BJ_VOICE:
        return j->calls;
    case RDOF_BJ_DATA:
        return j->repeat_gap_ms > 0 ? j->repeat : 1;
    case RDOF_BJ_CSBK:
        return 1;
    default:
        return 0;
    }
}

double rdof_bj_gap(const rdof_bj_job_t *j)
{
    return j->cmd == RDOF_BJ_VOICE ? j->gap_ms / 1000.0 : j->cmd == RDOF_BJ_DATA ? j->repeat_gap_ms / 1000.0 : 0;
}

int rdof_bj_max_bursts(const rdof_bj_job_t *j)
{
    switch (j->cmd) {
    case RDOF_BJ_VOICE: {
        const int f = voice_frames(j);
        return (j->headers > HEADERS ? j->headers : HEADERS) + (j->terminators > TERMINATORS ? j->terminators : TERMINATORS) +
               6 * ((f ? f : 1) + 17) / 18 + 6;
    }
    case RDOF_BJ_CSBK:
        return j->count + 1;
    case RDOF_BJ_DATA:
        return (j->repeat_gap_ms > 0 ? 1 : j->repeat) * (j->preambles + 1 + MAX_BLOCKS + 1);
    default:
        return 0;
    }
}

int rdof_bj_build(const rdof_bj_job_t *j, int k, rdof_bj_burst_t *out, int cap, char *note, char *done, size_t ncap,
                  char *error, size_t ecap)
{
    note[0] = done[0] = 0;
    int n = 0;
    switch (j->cmd) {
    case RDOF_BJ_VOICE:
        n = build_voice(j, k, out, cap, note, done, ncap);
        break;
    case RDOF_BJ_CSBK:
        n = build_csbk(j, out, cap, done, ncap);
        break;
    case RDOF_BJ_DATA: {
        const bool one = j->repeat_gap_ms <= 0;
        for (int p = one ? 0 : k; p < (one ? j->repeat : k + 1); p++)
            if (build_data(j, p, out, cap, &n, error, ecap) < 0)
                return -1;
        if (one || k == j->repeat - 1)
            snprintf(done, ncap, "data: %d packet(s) sent", j->repeat);
        break;
    }
    default:
        snprintf(error, ecap, "nothing to transmit");
        return -1;
    }
    if (n > cap) {
        snprintf(error, ecap, "%d bursts: more than %d", n, cap);
        return -1;
    }
    return n;
}

/* ── the listener ─────────────────────────────────────────────────────────── */

static const char *const LC_TYPES[8] = {"grp_voice", "priv_voice", "talker_alias_hdr", "talker_alias_blk1",
                                        "talker_alias_blk2", "talker_alias_blk3", "gps_info", "unknown"};

static int lc_type(int flco, int fid)
{
    if (fid != 0)
        return -1; /* manufacturer specific */
    switch (flco) {
    case RDOF_FLCO_GROUP: return 0;
    case RDOF_FLCO_PRIVATE: return 1;
    case RDOF_FLCO_TA_HEADER: return 2;
    case RDOF_FLCO_TA_HEADER + 1: return 3;
    case RDOF_FLCO_TA_HEADER + 2: return 4;
    case RDOF_FLCO_TA_HEADER + 3: return 5;
    case RDOF_FLCO_GPS_INFO: return 6;
    default: return 7;
    }
}

static const char *lc_type_name(int flco, int fid)
{
    const int t = lc_type(flco, fid);
    return t < 0 ? "manufacturer_specific" : LC_TYPES[t];
}

void rdof_bj_listen_init(rdof_bj_listener_t *l, bool json, bool keep_ambe)
{
    memset(l, 0, sizeof *l);
    l->json = json, l->keep_ambe = keep_ambe;
    l->cur = -1;
}

void rdof_bj_listen_free(rdof_bj_listener_t *l)
{
    for (int i = 0; i < l->ncalls; i++)
        free(l->calls[i].ambe);
    l->ncalls = 0;
    l->cur = -1;
}

static rdof_bj_call_t *new_call(rdof_bj_listener_t *l, double t, const char *how)
{
    if (l->cur >= 0 && !l->calls[l->cur].ended)
        return &l->calls[l->cur];
    if (l->ncalls == RDOF_BJ_CALLS) { /* the oldest goes */
        free(l->calls[0].ambe);
        memmove(l->calls, l->calls + 1, sizeof l->calls[0] * (RDOF_BJ_CALLS - 1));
        l->ncalls--;
    }
    rdof_bj_call_t *c = &l->calls[l->ncalls];
    memset(c, 0, sizeof *c);
    c->active = true;
    c->start = how;
    c->t0 = t;
    c->flco = -1;
    c->ta_format = -1;
    c->crc = 0;
    l->cur = l->ncalls++;
    return c;
}

/* JSON text helpers */
static size_t cat(char *o, size_t k, size_t cap, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
#include <stdarg.h>
static size_t cat(char *o, size_t k, size_t cap, const char *fmt, ...)
{
    if (k >= cap)
        return k;
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(o + k, cap - k, fmt, ap);
    va_end(ap);
    return n < 0 ? k : k + (size_t)n >= cap ? cap - 1 : k + (size_t)n;
}

static size_t cat_str(char *o, size_t k, size_t cap, const char *s)
{
    k = cat(o, k, cap, "\"");
    for (; *s && k + 8 < cap; s++) {
        const unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\')
            k = cat(o, k, cap, "\\%c", ch);
        else if (ch < 0x20)
            k = cat(o, k, cap, "\\u%04x", ch);
        else
            o[k++] = (char)ch, o[k] = 0;
    }
    return cat(o, k, cap, "\"");
}

static size_t cat_hex(char *o, size_t k, size_t cap, const uint8_t *b, size_t n)
{
    k = cat(o, k, cap, "\"");
    for (size_t i = 0; i < n; i++)
        k = cat(o, k, cap, "%02x", b[i]);
    return cat(o, k, cap, "\"");
}

/* the uncorrected 12 info octets of a BPTC(196,96) burst: deinterleaved (i * 181 mod 196), data bits as received */
static void bptc_raw(const uint8_t burst[33], uint8_t out[12])
{
    uint8_t bits[196], de[196], data[96];
    rdof_info_bits(burst, bits);
    for (int i = 0; i < 196; i++)
        de[i] = bits[(i * 181) % 196];
    int k = 0;
    for (int i = 4; i < 12; i++)
        data[k++] = de[i];
    for (int r = 1; r <= 8; r++)
        for (int a = 1 + 15 * r; a < 12 + 15 * r; a++)
            data[k++] = de[a];
    rdof_bits_to_bytes(data, 96, out);
}

/* The alias text of a call, when every block its length needs has come: true. */
static bool alias_text(const rdof_bj_call_t *c, char *out, size_t cap, int *need)
{
    const int fmt = c->ta_format, n = c->ta_length;
    *need = 0;
    out[0] = 0;
    if (fmt < 0 || n <= 0)
        return false;
    const int bits = fmt == 0 ? 7 * n : fmt == 3 ? 16 * n : 8 * n;
    const int room = fmt == 0 ? 49 : 48;
    *need = bits <= room ? 0 : (bits - room + 55) / 56;
    if (*need > 3)
        *need = 3;
    for (int i = 0; i <= *need; i++)
        if (!(c->ta_have & (1u << i)))
            return false;
    const uint8_t *b = c->ta_bits + (fmt == 0 ? 0 : 1);
    size_t k = 0;
    if (fmt == 0) {
        for (int i = 0; i < n && k + 1 < cap; i++) {
            int v = 0;
            for (int x = 0; x < 7; x++)
                v = v << 1 | b[7 * i + x];
            out[k++] = (char)v;
        }
    } else if (fmt == 3) {
        for (int i = 0; i < n && k + 4 < cap; i++) {
            int v = 0;
            for (int x = 0; x < 16; x++)
                v = v << 1 | b[16 * i + x];
            if (v < 0x80)
                out[k++] = (char)v;
            else if (v < 0x800)
                out[k++] = (char)(0xC0 | v >> 6), out[k++] = (char)(0x80 | (v & 0x3F));
            else
                out[k++] = (char)(0xE0 | v >> 12), out[k++] = (char)(0x80 | ((v >> 6) & 0x3F)),
                out[k++] = (char)(0x80 | (v & 0x3F));
        }
    } else {
        for (int i = 0; i < n && k + 3 < cap; i++) {
            int v = 0;
            for (int x = 0; x < 8; x++)
                v = v << 1 | b[8 * i + x];
            if (fmt == 1 && v >= 0x80) /* ISO 8859-1 to UTF-8 */
                out[k++] = (char)(0xC0 | v >> 6), out[k++] = (char)(0x80 | (v & 0x3F));
            else if (v)
                out[k++] = (char)v;
        }
    }
    out[k] = 0;
    return true;
}

static void on_embedded(rdof_bj_listener_t *l, const uint8_t lc9[9], char *rec, size_t *k, size_t cap)
{
    rdof_full_lc_t lc;
    rdof_parse_full_lc(lc9, &lc);
    const char *type = lc_type_name(lc.flco, lc.fid);
    *k = cat(rec, *k, cap, ",\"emb_lc\":{\"flco\":%d,\"fid\":%d,", lc.flco, lc.fid);
    if (lc.src_id != RDOF_NONE)
        *k = cat(rec, *k, cap, "\"src\":%ld,\"dst\":%ld,", (long)lc.src_id, (long)lc.dst_id);
    else
        *k = cat(rec, *k, cap, "\"src\":null,\"dst\":null,");
    *k = cat(rec, *k, cap, "\"type\":\"%s\",\"raw\":", type);
    *k = cat_hex(rec, *k, cap, lc9, 9);
    rdof_bj_call_t *c = l->cur >= 0 ? &l->calls[l->cur] : NULL;
    if (lc.flco == RDOF_FLCO_GROUP || lc.flco == RDOF_FLCO_PRIVATE) {
        const int so = lc.service_options;
        *k = cat(rec, *k, cap, ",\"group\":%s,\"so\":%d,\"emergency\":%s,\"privacy\":%s,\"broadcast\":%s,"
                               "\"ovcm\":%s,\"priority\":%d",
                 lc.group ? "true" : "false", so, so & 0x80 ? "true" : "false", so & 0x40 ? "true" : "false",
                 so & 0x08 ? "true" : "false", so & 0x04 ? "true" : "false", so & 3);
    } else if (lc.has_gps) {
        *k = cat(rec, *k, cap, ",\"position_error\":%d,\"lon\":%.6f,\"lat\":%.6f,\"gps\":[%.6f,%.6f,%d]",
                 lc.position_error, lc.lon, lc.lat, lc.lat, lc.lon, lc.position_error);
    } else if (lc.ta_format != RDOF_NONE) {
        *k = cat(rec, *k, cap, ",\"format\":%d,\"length\":%d", lc.ta_format, lc.ta_length);
    } else if (lc.ta_nbits == 56) {
        *k = cat(rec, *k, cap, ",\"block\":%d", lc.flco - RDOF_FLCO_TA_HEADER);
    }
    *k = cat(rec, *k, cap, "}");
    if (!c)
        return;
    const int t = lc_type(lc.flco, lc.fid);
    if (t >= 0)
        c->lc_types[t]++;
    if (lc.has_gps)
        c->gps = true, c->lat = lc.lat, c->lon = lc.lon, c->gps_err = lc.position_error;
    if (lc.flco == RDOF_FLCO_GROUP || lc.flco == RDOF_FLCO_PRIVATE) {
        c->emb_lc++;
        if (!c->has_src)
            c->src = (uint32_t)lc.src_id, c->dst = (uint32_t)lc.dst_id, c->flco = lc.flco, c->has_src = true;
    } else if (lc.ta_format != RDOF_NONE) {
        c->ta_format = lc.ta_format, c->ta_length = lc.ta_length;
        memcpy(c->ta_bits, lc.ta_bits, 49);
        c->ta_have |= 1;
    } else if (lc.ta_nbits == 56) {
        const int blk = lc.flco - RDOF_FLCO_TA_HEADER;
        memcpy(c->ta_bits + 49 + 56 * (blk - 1), lc.ta_bits, 56);
        c->ta_have |= 1u << blk;
    }
}

void rdof_bj_listen_burst(rdof_bj_listener_t *l, const rdof_bl_burst_t *b, rdof_bj_emit_fn emit, void *ctx)
{
    char rec[1536];
    size_t k = 0;
    const size_t cap = sizeof rec;
    const bool voice = b->dt == RDOF_BL_VOICE_SYNC || b->dt == RDOF_BL_VOICE;
    const int n = b->dt == RDOF_BL_VOICE ? b->n : 0;
    const int control = voice ? (n ? n : RDOF_SYNC_AUDIO) : RDOF_SYNC_DATA | (b->dt & 0x0F);
    k = cat(rec, k, cap, "{\"t\":%.3f,\"control\":%d,\"rssi\":null", b->t, control);
    if (!voice) {
        const int dt = b->dt;
        int cc = -1, sdt = 0;
        rdof_slot_type_raw(b->raw, &cc, &sdt);
        k = cat(rec, k, cap, ",\"dt\":%d,\"cc\":%d", dt, cc);
        if (dt == RDOF_DT_CSBK || dt == RDOF_DT_DATA_HEADER || dt == RDOF_DT_RATE_12) {
            uint8_t info[12], raw[12];
            const bool fec_ok = rdof_bptc196_decode(b->raw, info);
            bptc_raw(b->raw, raw);
            k = cat(rec, k, cap, ",\"info\":");
            k = cat_hex(rec, k, cap, info, 12);
            k = cat(rec, k, cap, ",\"info_raw\":");
            k = cat_hex(rec, k, cap, raw, 12);
            if (dt == RDOF_DT_CSBK)
                k = cat(rec, k, cap, ",\"crc_ok\":%s",
                        fec_ok && rdof_check_crc16(info, RDOF_MASK_CSBK) ? "true" : "false");
            else if (dt == RDOF_DT_DATA_HEADER)
                k = cat(rec, k, cap, ",\"crc_ok\":%s",
                        fec_ok && rdof_check_crc16(info, RDOF_MASK_DATA_HEADER) ? "true" : "false");
        }
        if (dt == RDOF_DT_VOICE_LC_HEADER || dt == RDOF_DT_TERMINATOR_WITH_LC) {
            rdof_full_lc_t lc;
            bool ok = rdof_decode_voice_lc(b->raw, dt, &lc);
            if (ok)
                k = cat(rec, k, cap, ",\"src\":%ld,\"dst\":%ld,\"flco\":%d,\"so\":%d,\"lc_type\":\"%s\"",
                        (long)lc.src_id, (long)lc.dst_id, lc.flco, lc.service_options,
                        lc_type_name(lc.flco, lc.fid));
            if (dt == RDOF_DT_VOICE_LC_HEADER) {
                rdof_bj_call_t *c = new_call(l, b->t, "header");
                if (ok && !c->has_src)
                    c->src = (uint32_t)lc.src_id, c->dst = (uint32_t)lc.dst_id, c->flco = lc.flco, c->has_src = true;
                c->headers++;
                if (ok && !c->lc_type[0])
                    snprintf(c->lc_type, sizeof c->lc_type, "%s", lc_type_name(lc.flco, lc.fid));
            } else if (l->cur >= 0) {
                rdof_bj_call_t *c = &l->calls[l->cur];
                c->terminators++;
                if (!c->ended_by)
                    c->ended_by = "terminator";
                c->ended = true;
            }
        } else if (dt == RDOF_DT_CSBK) {
            rdof_csbk_t cs;
            rdof_decode_csbk(b->raw, &cs);
            k = cat(rec, k, cap, ",\"csbk\":\"opcode %02X fid %02X dst %ld src %ld\"", cs.opcode, cs.fid,
                    (long)cs.dst_id, (long)cs.src_id);
        } else if (dt == RDOF_DT_DATA_HEADER) {
            rdof_data_header_t h;
            rdof_decode_data_header(b->raw, &h);
            k = cat(rec, k, cap, ",\"data_header\":\"dpf %d sap %d dst %lu src %lu blocks %d\"", h.dpf, h.sap,
                    (unsigned long)h.dst_id, (unsigned long)h.src_id, h.blocks);
        }
    } else {
        k = cat(rec, k, cap, ",\"voice\":%d", n);
        if (n) {
            int cc = -1, pi = 0, lcss = 0, errs = 0;
            rdof_decode_emb(b->raw, &cc, &pi, &lcss, &errs);
            k = cat(rec, k, cap, ",\"cc\":%d,\"pi\":%d,\"lcss\":%d", cc, pi, lcss);
            if (n >= 1 && n <= 4) {
                rdof_embedded_signalling(b->raw, l->frag[n - 1]);
                l->frag_have |= 1u << (n - 1);
            }
            if (n == 4 && l->frag_have == 0xF) {
                uint8_t raw128[128], lc9[9];
                for (int i = 0; i < 4; i++)
                    memcpy(raw128 + 32 * i, l->frag[i], 32);
                if (rdof_embedded_lc_decode(raw128, lc9))
                    on_embedded(l, lc9, rec, &k, cap);
            }
            if (l->cur >= 0 && !l->calls[l->cur].ended) {
                rdof_bj_call_t *c = &l->calls[l->cur];
                if (pi)
                    c->pi++;
                if (cc >= 0 && cc < 16)
                    c->cc_mask |= 1u << cc;
            }
        } else {
            l->frag_have = 0;
        }
        rdof_bj_call_t *c = l->cur >= 0 ? &l->calls[l->cur] : NULL;
        if (!c || c->ended) {
            if (c && c->ended_by && !strcmp(c->ended_by, "terminator") && b->t - c->t_last < 0.3)
                c->after_end++;
            else
                c = new_call(l, b->t, "late entry");
        }
        if (!c->ended) {
            c->voice++;
            uint8_t f[3][9];
            rdof_ambe_frames(b->raw, f);
            for (int i = 0; i < 3; i++) {
                c->frames++;
                c->fec_bits += rdof_ambe_decode_frame(f[i], NULL);
                if (!memcmp(f[i], SILENCE, 9))
                    c->silence++;
                c->crc = crc32_up(c->crc, f[i], 9);
                if (l->keep_ambe) {
                    if (c->ambe_n + 9 > c->ambe_cap) {
                        const size_t nc = c->ambe_cap ? 2 * c->ambe_cap : 9 * 300;
                        uint8_t *p = realloc(c->ambe, nc);
                        if (p)
                            c->ambe = p, c->ambe_cap = nc;
                    }
                    if (c->ambe_n + 9 <= c->ambe_cap)
                        memcpy(c->ambe + c->ambe_n, f[i], 9), c->ambe_n += 9;
                }
            }
        }
    }
    if (l->cur >= 0)
        l->calls[l->cur].t_last = b->t;
    k = cat(rec, k, cap, "}");
    emit(ctx, rec);
}

void rdof_bj_listen_summary(rdof_bj_listener_t *l, const char *prefix, rdof_bj_emit_fn emit,
                            void (*file)(void *ctx, const char *name, const uint8_t *b, size_t n), void *ctx)
{
    static const char *const FMT[4] = {"7-bit", "ISO 8-bit", "UTF-8", "UTF-16"};
    char rec[1536], ta[128], name[96];
    for (int i = 0; i < l->ncalls; i++) {
        rdof_bj_call_t *c = &l->calls[i];
        size_t k = 0;
        const size_t cap = sizeof rec;
        k = cat(rec, k, cap, "{\"call\":%d,", i + 1);
        if (c->has_src)
            k = cat(rec, k, cap, "\"src\":%lu,\"dst\":%lu,\"flco\":%d,", (unsigned long)c->src,
                    (unsigned long)c->dst, c->flco);
        else
            k = cat(rec, k, cap, "\"src\":null,\"dst\":null,\"flco\":null,");
        k = cat(rec, k, cap, "\"start\":\"%s\",\"t0\":%.3f,\"headers\":%d,\"voice\":%d,\"terminators\":%d,",
                c->start, c->t0, c->headers, c->voice, c->terminators);
        k = c->ended_by ? cat(rec, k, cap, "\"ended\":\"%s\",", c->ended_by) : cat(rec, k, cap, "\"ended\":null,");
        k = cat(rec, k, cap, "\"emb_lc\":%d,\"pi\":%d,", c->emb_lc, c->pi);
        int need = 0;
        const bool whole = alias_text(c, ta, sizeof ta, &need);
        k = cat(rec, k, cap, "\"talker_alias\":");
        k = whole ? cat_str(rec, k, cap, ta) : cat(rec, k, cap, "null");
        if (c->gps)
            k = cat(rec, k, cap, ",\"gps\":[%.6f,%.6f,%d]", c->lat, c->lon, c->gps_err);
        else
            k = cat(rec, k, cap, ",\"gps\":null");
        k = cat(rec, k, cap, ",\"cc\":[");
        for (int x = 0, first = 1; x < 16; x++)
            if (c->cc_mask & (1u << x))
                k = cat(rec, k, cap, "%s%d", first ? "" : ",", x), first = 0;
        k = cat(rec, k, cap, "],\"after_end\":%d,\"lc_type\":", c->after_end);
        k = c->lc_type[0] ? cat_str(rec, k, cap, c->lc_type) : cat(rec, k, cap, "null");
        k = cat(rec, k, cap, ",\"lc_types\":{");
        for (int t = 0, first = 1; t < 8; t++)
            if (c->lc_types[t])
                k = cat(rec, k, cap, "%s\"%s\":%d", first ? "" : ",", LC_TYPES[t], c->lc_types[t]), first = 0;
        k = cat(rec, k, cap, "},\"gps_info\":");
        if (c->gps) {
            static const int ERR_M[8] = {2, 20, 200, 2000, 20000, 200000, -1, -1};
            k = cat(rec, k, cap, "{\"lat\":%.6f,\"lon\":%.6f,\"position_error\":%d,\"position_error_m\":", c->lat,
                    c->lon, c->gps_err);
            k = ERR_M[c->gps_err & 7] < 0 ? cat(rec, k, cap, "null}") : cat(rec, k, cap, "%d}", ERR_M[c->gps_err & 7]);
        } else {
            k = cat(rec, k, cap, "null");
        }
        k = cat(rec, k, cap, ",\"alias\":");
        if (c->ta_have & 1) {
            k = cat(rec, k, cap, "{\"format\":%d,\"format_name\":\"%s\",\"length\":%d,\"blocks_needed\":%d,"
                                 "\"blocks_seen\":[",
                    c->ta_format, FMT[c->ta_format & 3], c->ta_length, need);
            for (int x = 1, first = 1; x <= 3; x++)
                if (c->ta_have & (1u << x))
                    k = cat(rec, k, cap, "%s%d", first ? "" : ",", x), first = 0;
            k = cat(rec, k, cap, "],\"complete\":%s,\"text\":", whole ? "true" : "false");
            k = whole ? cat_str(rec, k, cap, ta) : cat(rec, k, cap, "null");
            k = cat(rec, k, cap, "}");
        } else {
            k = cat(rec, k, cap, "null");
        }
        k = cat(rec, k, cap, ",\"ambe_frames\":%ld", c->frames);
        if (c->frames) {
            k = cat(rec, k, cap, ",\"ambe_fec_bits_corrected\":%ld,\"ambe_silence_frames\":%ld,\"ambe_crc32\":\"%08X\"",
                    c->fec_bits, c->silence, (unsigned)c->crc);
            if (prefix && prefix[0] && file && c->ambe_n) {
                snprintf(name, sizeof name, "%s_%d.ambe", prefix, i + 1);
                file(ctx, name, c->ambe, c->ambe_n);
                /* DSD's .amb: ".amb", then a frame each: errors corrected, 49 parameter bits in 7 octets (the
                 * last holds one bit, as its value) */
                const size_t nf = c->ambe_n / 9;
                uint8_t *amb = malloc(4 + 8 * nf);
                if (amb) {
                    memcpy(amb, ".amb", 4);
                    for (size_t f = 0; f < nf; f++) {
                        uint8_t p[49];
                        const int e = rdof_ambe_decode_frame(c->ambe + 9 * f, p);
                        uint8_t *o = amb + 4 + 8 * f;
                        o[0] = (uint8_t)(e > 255 ? 255 : e);
                        rdof_bits_to_bytes(p, 48, o + 1);
                        o[7] = p[48];
                    }
                    snprintf(name, sizeof name, "%s_%d.amb", prefix, i + 1);
                    file(ctx, name, amb, 4 + 8 * nf);
                    free(amb);
                }
                k = cat(rec, k, cap, ",\"files\":[\"%s_%d.ambe\",\"%s_%d.amb\"]", prefix, i + 1, prefix, i + 1);
            }
        }
        k = cat(rec, k, cap, "}");
        emit(ctx, rec);
    }
}
