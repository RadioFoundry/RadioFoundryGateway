// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The MMDVM serial protocol (framing, config, replies), burst regeneration and radio detection (rdof/mmdvm.h). */
#include "rdof/mmdvm.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "rdof/bits.h"
#include "rdof/features.h"
#include "rdof/layer2.h"
#include "rdof/pdu.h"

bool rdof_mmdvm_unanswered(uint8_t c)
{
    return c == RDOF_MMDVM_DMR_DATA1 || c == RDOF_MMDVM_DMR_DATA2 || c == RDOF_MMDVM_DMR_SHORTLC ||
           c == RDOF_MMDVM_DMR_START || c == RDOF_MMDVM_DMR_ABORT;
}

size_t rdof_mmdvm_frame(uint8_t command, const uint8_t *payload, size_t len, uint8_t *out, size_t cap)
{
    if (len > 252 || cap < len + 3)
        return 0;
    out[0] = RDOF_MMDVM_FRAME_START;
    out[1] = (uint8_t)(len + 3);
    out[2] = command;
    if (len)
        memcpy(out + 3, payload, len);
    return len + 3;
}

void rdof_modem_config_default(rdof_modem_config_t *c)
{
    memset(c, 0, sizeof *c);
    c->tx_delay_ms = 100;
    c->rx_level = c->tx_level = c->cwid_level = 50.0;
}

int rdof_mmdvm_pct(double level)
{
    double v = level * 255 / 100 + 0.5;
    return v >= 255 ? 255 : v > 0 ? (int)v : 0; /* NaN: 0 (int() of it would be undefined) */
}

int rdof_modem_config_flags(const rdof_modem_config_t *c, bool duplex)
{
    return (duplex ? 0 : RDOF_MMDVM_SIMPLEX) | (c->rx_invert ? 0x01 : 0) | (c->tx_invert ? 0x02 : 0) |
           (c->ptt_invert ? 0x04 : 0);
}

static uint8_t tx_delay(const rdof_modem_config_t *c)
{
    int v = c->tx_delay_ms / 10;
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

size_t rdof_mmdvm_config_v1(const rdof_modem_config_t *c, int cc, int state, bool duplex, uint8_t p[23])
{
    memset(p, 0, 23);
    p[0] = (uint8_t)rdof_modem_config_flags(c, duplex);
    p[1] = RDOF_DMR_ENABLED;
    p[2] = tx_delay(c);
    p[3] = (uint8_t)state;
    p[4] = (uint8_t)rdof_mmdvm_pct(c->rx_level);
    p[5] = (uint8_t)rdof_mmdvm_pct(c->cwid_level);
    p[6] = (uint8_t)(cc & 0x0F);
    p[7] = (uint8_t)c->dmr_delay;
    p[8] = 128;
    p[10] = (uint8_t)rdof_mmdvm_pct(c->tx_level);
    p[13] = (uint8_t)((unsigned)c->tx_dc_offset + 128u);
    p[14] = (uint8_t)((unsigned)c->rx_dc_offset + 128u);
    return 23;
}

size_t rdof_mmdvm_config_v2(const rdof_modem_config_t *c, int cc, int state, bool duplex, uint8_t p[37])
{
    memset(p, 0, 37);
    p[0] = (uint8_t)rdof_modem_config_flags(c, duplex);
    p[1] = RDOF_DMR_ENABLED;
    p[3] = tx_delay(c);
    p[4] = (uint8_t)state;
    p[5] = (uint8_t)((unsigned)c->tx_dc_offset + 128u);
    p[6] = (uint8_t)((unsigned)c->rx_dc_offset + 128u);
    p[7] = (uint8_t)rdof_mmdvm_pct(c->rx_level);
    p[8] = (uint8_t)rdof_mmdvm_pct(c->cwid_level);
    p[10] = (uint8_t)rdof_mmdvm_pct(c->tx_level);
    p[26] = (uint8_t)(cc & 0x0F);
    p[27] = (uint8_t)c->dmr_delay;
    p[28] = 128;
    return 37;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

size_t rdof_mmdvm_set_freq_payload(uint32_t rx_hz, uint32_t tx_hz, int rf_level, uint8_t out[14])
{
    out[0] = 0;
    put_le32(out + 1, rx_hz);
    put_le32(out + 5, tx_hz);
    out[9] = (uint8_t)(rf_level & 0xFF);
    put_le32(out + 10, tx_hz);
    return 14;
}

/* ── frame reader ──────────────────────────────────────────────────────────── */

void rdof_mmdvm_reader_init(rdof_mmdvm_reader_t *r)
{
    r->len = 0;
}

static void drop(rdof_mmdvm_reader_t *r, size_t n)
{
    memmove(r->buf, r->buf + n, r->len - n);
    r->len -= n;
}

static void parse(rdof_mmdvm_reader_t *r, rdof_mmdvm_frame_cb cb, void *ctx)
{
    for (;;) {
        uint8_t *start = memchr(r->buf, RDOF_MMDVM_FRAME_START, r->len);
        if (!start) {
            r->len = 0;
            return;
        }
        drop(r, (size_t)(start - r->buf));
        if (r->len < 2)
            return;
        size_t length = r->buf[1], head = 2;
        if (length == 0) { /* protocol 2 long frame */
            if (r->len < 3)
                return;
            length = (size_t)r->buf[2] + 255;
            head = 3;
        } else if (length < 3) {
            drop(r, 1);
            continue;
        }
        if (r->len < length)
            return;
        cb(ctx, r->buf[head], r->buf + head + 1, length - head - 1);
        drop(r, length);
    }
}

void rdof_mmdvm_reader_feed(rdof_mmdvm_reader_t *r, const uint8_t *data, size_t n, rdof_mmdvm_frame_cb cb, void *ctx)
{
    /* What's left after a parse is always shorter than the longest frame, so there's room for a chunk. */
    while (n) {
        size_t room = sizeof r->buf - r->len, take = n < room ? n : room;
        memcpy(r->buf + r->len, data, take);
        r->len += take;
        data += take;
        n -= take;
        parse(r, cb, ctx);
    }
}

/* ── replies ───────────────────────────────────────────────────────────────── */

/* Whitespace in ASCII: isspace's and the separators 1C to 1F. */
static bool py_space(char c)
{
    return isspace((unsigned char)c) || (c >= 0x1C && c <= 0x1F);
}

bool rdof_mmdvm_parse_status(int protocol, const uint8_t *p, size_t len, rdof_mmdvm_status_t *o)
{
    if (len < 6)
        return false;
    uint8_t flags = protocol >= 2 ? p[1] : p[2];
    o->transmitting = flags & 0x01;
    o->carrier = (flags & 0x40) ? 1 : 0;
    o->space = p[5];
    o->space1 = p[4];
    o->inbound_busy = (flags & RDOF_STATUS_INBOUND_BUSY) ? 1 : 0;
    return true;
}

int rdof_mmdvm_parse_version(const uint8_t *p, size_t len, char *desc, size_t cap)
{
    int protocol = len ? p[0] : 1;
    size_t off = protocol >= 2 ? 20 : 1, n = 0;
    if (cap == 0)
        return protocol;
    for (size_t i = off; i < len && p[i] != 0 && n + 1 < cap; i++)
        desc[n++] = p[i] < 0x80 ? (char)p[i] : '?';
    desc[n] = 0;
    /* strip whitespace, both ends */
    size_t a = 0;
    while (a < n && py_space(desc[a]))
        a++;
    while (n > a && py_space(desc[n - 1]))
        n--;
    memmove(desc, desc + a, n - a);
    desc[n - a] = 0;
    return protocol;
}

/* ── the radio ─────────────────────────────────────────────────────────────── */

static bool contains_upper(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < nl && hay[i] && toupper((unsigned char)hay[i]) == needle[i])
            i++;
        if (i == nl)
            return true;
    }
    return false;
}

/* A radio in hotspot mode: RadioFoundry ("RadioFoundry DM-1701 ba226121"; "OpenDigitalRadio ..." before its rename),
 * or the older OpenGD77 firmware. Not this gateway's own SDR modem ("RadioFoundry Gateway SDR modem ..."). */
bool rdof_is_radio_hotspot(const char *d)
{
    if (contains_upper(d, "RADIOFOUNDRY GATEWAY"))
        return false;
    return contains_upper(d, "RADIOFOUNDRY") || contains_upper(d, "OPENDIGITALRADIO") || contains_upper(d, "OPENGD77");
}

rdof_radio_kind_t rdof_radio_detect(const char *d)
{
    static const char *const hs[] = {"MMDVM_HS", "ZUMSPOT", "NANO_HOTSPOT", "NANODV",
                                     "SKYBRIDGE", "D2RG", "LONESTAR", "ADF7021"};
    if (contains_upper(d, "SPLIT DUPLEX MODEM")) /* RDOF_SPLIT_DESCRIPTION */
        return RDOF_RADIO_SPLIT;
    if (rdof_is_radio_hotspot(d))
        return RDOF_RADIO_HOTSPOT;
    for (size_t i = 0; i < sizeof hs / sizeof hs[0]; i++)
        if (contains_upper(d, hs[i]))
            return RDOF_RADIO_MMDVM_HS;
    if (contains_upper(d, "MMDVM"))
        return RDOF_RADIO_MMDVM;
    return RDOF_RADIO_UNKNOWN;
}

const char *rdof_radio_kind_name(rdof_radio_kind_t k)
{
    switch (k) {
    case RDOF_RADIO_HOTSPOT: return "radio_hotspot";
    case RDOF_RADIO_MMDVM_HS: return "mmdvm_hs";
    case RDOF_RADIO_MMDVM: return "mmdvm";
    case RDOF_RADIO_SPLIT: return "split";
    default: return "unknown";
    }
}

rdof_radio_kind_t rdof_radio_kind_of(const char *type)
{
    if (!strcmp(type, "opengd77"))
        return RDOF_RADIO_HOTSPOT;
    for (int k = RDOF_RADIO_HOTSPOT; k <= RDOF_RADIO_SPLIT; k++)
        if (!strcmp(type, rdof_radio_kind_name((rdof_radio_kind_t)k)))
            return (rdof_radio_kind_t)k;
    return RDOF_RADIO_UNKNOWN;
}

static bool word_char(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

static void upper(const char *in, char *out, size_t cap)
{
    size_t i = 0;
    for (; in[i] && i + 1 < cap; i++)
        out[i] = (char)toupper((unsigned char)in[i]);
    out[i] = 0;
}

/* "TS<a>\s*>\s*TS<b>" at s, with a word boundary before; returns the length matched or 0. */
static size_t ts_pair(const char *base, const char *s, int *a, int *b)
{
    const char *p = s;
    if ((p > base && word_char(p[-1])) || strncmp(p, "TS", 2) || (p[2] != '1' && p[2] != '2'))
        return 0;
    *a = p[2] - '0';
    p += 3;
    while (py_space(*p))
        p++;
    if (*p != '>')
        return 0;
    p++;
    while (py_space(*p))
        p++;
    if (strncmp(p, "TS", 2) || (p[2] != '1' && p[2] != '2') || word_char(p[3]))
        return 0;
    *b = p[2] - '0';
    return (size_t)(p + 3 - s);
}

static const char *find_word(const char *d, const char *w)
{
    size_t n = strlen(w);
    for (const char *p = strstr(d, w); p; p = strstr(p + 1, w))
        if ((p == d || !word_char(p[-1])) && !word_char(p[n]))
            return p;
    return NULL;
}

bool rdof_sfr_slots(const char *description, int *inbound, int *outbound)
{
    char d[256];
    upper(description, d, sizeof d);
    if (!rdof_is_radio_hotspot(d) || !find_word(d, "SFR"))
        return false;
    *inbound = 1;
    *outbound = 2;
    for (const char *p = d; *p; p++) {
        int a, b;
        if (ts_pair(d, p, &a, &b)) {
            *inbound = a;
            *outbound = b;
            break;
        }
    }
    return true;
}

bool rdof_dcdm_capable(const char *description)
{
    char d[256];
    upper(description, d, sizeof d);
    return find_word(d, "DCDM") != NULL;
}

void rdof_sfr_framing(const char *description, char *out, size_t cap)
{
    char d[256];
    upper(description, d, sizeof d);
    if (cap)
        out[0] = 0;
    for (const char *p = find_word(d, "SFR"); p; p = find_word(p + 1, "SFR")) {
        const char *q = p + 3;
        int a, b;
        if (!py_space(*q))
            continue;
        while (py_space(*q))
            q++;
        size_t n = ts_pair(d, q, &a, &b);
        if (!n)
            continue;
        q += n;
        if (!py_space(*q))
            continue;
        while (py_space(*q))
            q++;
        size_t k = 0;
        while (q[k] >= 'A' && q[k] <= 'Z')
            k++;
        if (k == 0 || word_char(q[k]))
            continue;
        snprintf(out, cap, "%.*s", (int)k, q);
        return;
    }
}

int rdof_radio_hotspot_dbm(int raw, uint32_t freq_hz)
{
    long long x = (long long)raw * 32;
    long long v = freq_hz >= 300000000u ? -151LL + raw : -164 + (x >= 0 ? x / 27 : -((-x + 26) / 27)); /* floor */
    return v < INT_MIN ? INT_MIN : v > INT_MAX ? INT_MAX : (int)v;
}

#if RDOF_FEATURE_TX
/* ── burst regeneration ────────────────────────────────────────────────────── */

void rdof_regenerate(uint8_t control, const uint8_t burst[RDOF_BURST_BYTES], int cc, bool bs, uint8_t out[RDOF_BURST_BYTES])
{
    /* A voice superframe's LC fragments, by burst B-F (1-5), when a burst's own EMB can't be read. */
    static const int lcss_by_n[6] = {0, 1, 3, 3, 2, 0};
    uint64_t voice = bs ? RDOF_BS_VOICE : RDOF_MS_VOICE;
    memcpy(out, burst, RDOF_BURST_BYTES);
    if (control & RDOF_SYNC_DATA) {
        uint8_t st[20];
        rdof_slot_type_bits(cc, control & 0x0F, st);
        rdof_set_field(out, 98, 10, rdof_bits_to_u64(st, 10));
        rdof_set_field(out, 108, 48, rdof_data_sync(voice));
        rdof_set_field(out, 156, 10, rdof_bits_to_u64(st + 10, 10));
    } else if (control & RDOF_SYNC_AUDIO) {
        rdof_set_field(out, 108, 48, voice);
    } else {
        int old_cc, pi, lcss;
        if (!rdof_decode_emb(burst, &old_cc, &pi, &lcss, NULL)) {
            int n = control & 0x0F;
            pi = 0;
            lcss = n <= 5 ? lcss_by_n[n] : 0;
        }
        uint8_t e[16];
        rdof_emb_bits(cc, lcss, pi, e);
        rdof_set_field(out, 108, 8, rdof_bits_to_u64(e, 8));
        rdof_set_field(out, 148, 8, rdof_bits_to_u64(e + 8, 8));
    }
}

/* The on-air AMBE+2 silence frame (72 bits, FEC coded), three to a silence burst. */
static const uint8_t AMBE_SILENCE[9] = {0xb9, 0xe8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2a, 0x6b};

uint8_t rdof_silence_burst(int n, int cc, bool bs, const uint8_t *embedded32, uint8_t out[RDOF_BURST_BYTES])
{
    static const int lcss_of[6] = {0, 1, 3, 3, 2, 0};
    uint8_t frames[27], fbits[216], bits[264];
    for (int i = 0; i < 3; i++)
        memcpy(frames + 9 * i, AMBE_SILENCE, 9);
    rdof_bytes_to_bits(frames, 27, fbits);
    memset(bits, 0, sizeof bits);
    memcpy(bits, fbits, 108);
    memcpy(bits + 156, fbits + 108, 108);
    if (n != 0 && embedded32)
        memcpy(bits + 116, embedded32, 32);
    rdof_bits_to_bytes(bits, 264, out);
    if (n == 0) {
        rdof_set_field(out, 108, 48, bs ? RDOF_BS_VOICE : RDOF_MS_VOICE);
        return RDOF_SYNC_AUDIO;
    }
    uint8_t e[16];
    rdof_emb_bits(cc, n >= 1 && n <= 5 ? lcss_of[n] : 0, 0, e); /* bursts._LCSS.get(n, 0) */
    rdof_set_field(out, 108, 8, rdof_bits_to_u64(e, 8));
    rdof_set_field(out, 148, 8, rdof_bits_to_u64(e + 8, 8));
    return (uint8_t)n;
}

void rdof_csbk_preambles(uint32_t src, uint32_t dst, bool group, int n, int cc, uint8_t (*out)[RDOF_BURST_BYTES])
{
    for (int i = 0; i < n; i++) {
        uint8_t info[12];
        info[0] = 0x80 | RDOF_CSBK_PREAMBLE;
        info[1] = 0;
        info[2] = group ? 0x40 : 0x00;
        info[3] = (uint8_t)((n - i) & 0xFF);
        rdof_put_be24(info + 4, dst);
        rdof_put_be24(info + 7, src);
        uint16_t crc = rdof_crc16_ccitt(info, 10) ^ RDOF_MASK_CSBK;
        info[10] = (uint8_t)(crc >> 8);
        info[11] = (uint8_t)crc;
        rdof_data_burst(info, RDOF_DT_CSBK, cc, rdof_data_sync(RDOF_MS_VOICE), out[i]);
    }
}

#endif /* RDOF_FEATURE_TX */

void rdof_control_to_dt(uint8_t control, int *dt, int *n)
{
    if (control & RDOF_SYNC_DATA) {
        *dt = control & 0x0F;
        *n = 0;
    } else if (control & RDOF_SYNC_AUDIO) {
        *dt = RDOF_DT_VOICE_SYNC;
        *n = 0;
    } else {
        *dt = RDOF_DT_VOICE;
        *n = control & 0x0F;
    }
}
