// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* HBlink4 packets and the HBlink sink (rdof/hblink.h). */
#include "rdof/hblink.h"

#include <stdio.h>
#include <string.h>

#include "rdof/bits.h"
#include "rdof/features.h"
#include "rdof/pdu.h"

static size_t location_tail(const rdof_location_t *loc, char *out)
{
    char lat[48], lon[48];
    snprintf(lat, sizeof lat, "%.4f", loc->latitude);
    snprintf(lon, sizeof lon, "%.4f", loc->longitude);
    lat[8] = 0; /* [:8] (the buffers are long enough that these are inside the string or past its end) */
    lon[9] = 0;
    return (size_t)snprintf(out, 32, "%-8s%-9s%03d", lat, lon, loc->height_m);
}

size_t rdof_dmrc_packet(uint32_t peer_id, const char *callsign, uint32_t rx_hz, uint32_t tx_hz, int cc,
                      char slots, int power, const char *version, const char *software,
                      const rdof_location_t *location, const uint32_t *idle_freq, const int *idle_cc,
                      uint8_t out[RDOF_DMRC_MAX])
{
    char body[RDOF_DMRC_MAX + 32];
    int n = snprintf(body, sizeof body, "%-8.8s%09lu%09lu%02u%02u%c%-40.40s%-40.40s", callsign,
                     (unsigned long)rx_hz, (unsigned long)tx_hz, (unsigned)(power < 99 ? power : 99), (unsigned)cc,
                     slots, version, software);
    if (location)
        n += (int)location_tail(location, body + n);
    if (idle_freq && idle_cc) {
        if (!location) {
            rdof_location_t zero = {0.0, 0.0, 0};
            n += (int)location_tail(&zero, body + n);
        }
        n += snprintf(body + n, sizeof body - (size_t)n, "%09lu%02u", (unsigned long)*idle_freq, (unsigned)*idle_cc);
    }
    if (n + 8 > RDOF_DMRC_MAX)
        n = RDOF_DMRC_MAX - 8;
    memcpy(out, "DMRC", 4);
    rdof_put_be32(out + 4, peer_id);
    memcpy(out + 8, body, (size_t)n);
    return (size_t)n + 8;
}

void rdof_dmrd_packet(int seq, uint32_t src, uint32_t dst, uint32_t peer_id, int slot, bool group, int dt, int n,
                    uint32_t stream_id, const uint8_t burst[RDOF_BURST_BYTES], int ber, int rssi,
                    uint8_t out[RDOF_DMRD_LEN])
{
    uint8_t flags = slot == 2 ? 0x80 : 0x00;
    if (!group)
        flags |= 0x40;
    if (dt == RDOF_DT_VOICE_SYNC)
        flags |= 0x10;
    else if (dt == RDOF_DT_VOICE)
        flags |= (uint8_t)(n & 0x0F);
    else
        flags |= (uint8_t)(0x20 | (dt & 0x0F));
    memcpy(out, "DMRD", 4);
    out[4] = (uint8_t)(seq & 0xFF);
    rdof_put_be24(out + 5, src);
    rdof_put_be24(out + 8, dst);
    rdof_put_be32(out + 11, peer_id);
    out[15] = flags;
    rdof_put_be32(out + 16, stream_id);
    memcpy(out + 20, burst, RDOF_BURST_BYTES);
    if (rssi < 0)
        rssi = -rssi;
    out[53] = (uint8_t)(ber < 255 ? ber : 255);
    out[54] = (uint8_t)(rssi < 255 ? rssi : 255);
}

bool rdof_single_csbk(const uint8_t burst[RDOF_BURST_BYTES])
{
    rdof_csbk_t c;
    rdof_decode_csbk(burst, &c);
    return c.crc_ok && c.opcode != RDOF_CSBK_PREAMBLE;
}

bool rdof_parse_dmrd(const uint8_t *d, size_t len, rdof_net_burst_t *o)
{
    if (len < 53 || memcmp(d, "DMRD", 4) != 0)
        return false;
    uint8_t flags = d[15];
    int ft = (flags >> 4) & 0x03, dtv = flags & 0x0F;
    memset(o, 0, sizeof *o);
    o->control = (uint8_t)(ft == 1 ? 0x20 : ft == 2 ? (0x40 | dtv) : dtv);
    memcpy(o->burst, d + 20, RDOF_BURST_BYTES);
    o->terminator = ft == 2 && (dtv == RDOF_DT_TERMINATOR_WITH_LC || (dtv == RDOF_DT_CSBK && rdof_single_csbk(o->burst)));
    memcpy(o->stream_id, d + 16, 4);
    o->seq = d[4];
    o->src = rdof_be24(d + 5);
    o->dst = rdof_be24(d + 8);
    o->group = !(flags & 0x40);
    o->slot = (flags & 0x80) ? 2 : 1;
    return true;
}

bool rdof_parse_dmrt(const uint8_t *d, size_t len, rdof_tune_t *o)
{
    if (len < 18 || memcmp(d, "DMRT", 4) != 0)
        return false;
    memcpy(o->stream_id, d + 8, 4);
    o->freq = rdof_be32(d + 12);
    o->cc = d[16] & 0x0F;
    o->power = d[17];
    o->preempt = len >= 19 && (d[18] & 0x01);
    return true;
}

void rdof_dmrk_packet(uint32_t peer_id, const uint8_t stream_id[4], int status, uint32_t freq, int cc,
                    uint8_t out[RDOF_DMRK_LEN])
{
    memcpy(out, "DMRK", 4);
    rdof_put_be32(out + 4, peer_id);
    memcpy(out + 8, stream_id, 4);
    out[12] = (uint8_t)status;
    rdof_put_be32(out + 13, freq);
    out[17] = (uint8_t)(cc & 0x0F);
}

char rdof_hb_kind(const uint8_t *d, size_t len)
{
    if (len < 4)
        return 0;
    if (!memcmp(d, "DMRP", 4))
        return 'P';
    if (!memcmp(d, "MSTN", 4) || !memcmp(d, "MSTC", 4))
        return 'N';
    if (!memcmp(d, "DMRT", 4))
        return 'T';
    if (!memcmp(d, "DMRD", 4))
        return 'D';
    return 0;
}

#if RDOF_FEATURE_TRACKER
/* ── the HBlink sink, one peer ─────────────────────────────────────────────── */

void rdof_hb_sink_init(rdof_hb_sink_t *s, uint32_t peer_id)
{
    memset(s, 0, sizeof *s);
    s->peer_id = peer_id;
}

static uint32_t new_stream_id(rdof_hb_sink_t *s)
{
    uint32_t r = s->rand32 ? s->rand32(s->ctx) : 0x12345678u;
    return r % 0xFFFFFFFEu + 1; /* randint(1, 0xFFFFFFFE) */
}

static void before_stream(rdof_hb_sink_t *s)
{
    if (s->before_stream)
        s->before_stream(s->ctx);
}

static void send_frame(rdof_hb_sink_t *s, const rdof_frame_t *f)
{
    uint8_t pkt[RDOF_DMRD_LEN];
    int i = f->slot - 1;
    rdof_dmrd_packet(s->seq[i], f->src, f->dst, s->peer_id, f->slot, f->group, f->dt, f->n, s->stream[i], f->burst,
                   f->errors, f->rssi, pkt);
    s->seq[i] = (s->seq[i] + 1) & 0xFF;
    if (s->send)
        s->send(s->ctx, pkt, sizeof pkt);
}

void rdof_hb_sink_on_frame(rdof_hb_sink_t *s, const rdof_frame_t *f)
{
    if (f->slot < 1 || f->slot > 2)
        return;
    int i = f->slot - 1;
    if (!f->has_addr) {
        /* Voice before we know who's talking: hold it for the late-entry header. */
        if (f->dt == RDOF_DT_VOICE_SYNC || f->dt == RDOF_DT_VOICE) {
            if (s->nheld[i] == RDOF_HB_HOLD_FRAMES) {
                memmove(&s->held[i][0], &s->held[i][1], sizeof(rdof_frame_t) * (RDOF_HB_HOLD_FRAMES - 1));
                s->nheld[i]--;
            }
            s->held[i][s->nheld[i]++] = *f;
        }
        return;
    }
    if (f->new_stream) {
        before_stream(s);
        s->stream[i] = new_stream_id(s);
        s->seq[i] = 0;
        int nheld = s->nheld[i];
        s->nheld[i] = 0;
        send_frame(s, f);
        if (f->dt == RDOF_DT_VOICE_LC_HEADER && f->synthetic)
            for (int k = 0; k < nheld; k++) {
                rdof_frame_t h = s->held[i][k];
                h.has_addr = true;
                h.src = f->src;
                h.dst = f->dst;
                h.group = f->group;
                h.new_stream = h.synthetic = false;
                send_frame(s, &h);
            }
        return;
    }
    if (s->stream[i] == 0) {
        before_stream(s);
        s->stream[i] = new_stream_id(s);
    }
    send_frame(s, f);
}

static void sink_frame(void *ctx, const char *channel, const rdof_frame_t *f)
{
    (void)channel;
    rdof_hb_sink_on_frame((rdof_hb_sink_t *)ctx, f);
}

rdof_sink_t rdof_hb_sink_as_sink(rdof_hb_sink_t *s)
{
    rdof_sink_t k = {.on_frame = sink_frame, .ctx = s};
    return k;
}
#endif /* RDOF_FEATURE_TRACKER */
