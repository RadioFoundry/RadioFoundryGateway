// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* The bench's transmit through the router (rdof/benchroute.h). */
#include "rdof/benchroute.h"

#include <string.h>

#include "rdof/pdu.h"

void rdof_benchroute_init(rdof_benchroute_t *r, void (*reply)(void *ctx, const uint8_t *frame, size_t n),
                          bool (*burst)(void *ctx, const rdof_net_burst_t *b), void *ctx)
{
    memset(r, 0, sizeof *r);
    r->reply = reply;
    r->burst = burst;
    r->ctx = ctx;
    rdof_mmdvm_reader_init(&r->reader);
}

static void send(rdof_benchroute_t *r, uint8_t cmd, const uint8_t *p, size_t n)
{
    uint8_t f[64];
    const size_t k = rdof_mmdvm_frame(cmd, p, n, f, sizeof f);
    if (k && r->reply)
        r->reply(r->ctx, f, k);
}

static void ack(rdof_benchroute_t *r, uint8_t cmd) { send(r, RDOF_MMDVM_ACK, &cmd, 1); }

static void nak(rdof_benchroute_t *r, uint8_t cmd, uint8_t reason)
{
    const uint8_t p[2] = {cmd, reason};
    send(r, RDOF_MMDVM_NAK, p, 2);
}

static void data(rdof_benchroute_t *r, uint8_t cmd, const uint8_t *p, size_t len)
{
    if (len < 1 + RDOF_BURST_BYTES) {
        nak(r, cmd, 4); /* invalid */
        return;
    }
    const uint8_t control = p[0];
    const uint8_t *burst = p + 1;
    const bool sync_data = (control & 0xF0) == RDOF_SYNC_DATA;
    const int dt = control & 0x0F;
    const bool term = sync_data && dt == RDOF_DT_TERMINATOR_WITH_LC;
    if (!r->streaming && term && r->ended) {
        r->dropped++; /* a call's further terminators: its stream has ended (the transceiver sends its own count) */
        return;
    }
    r->ended = false;
    if (!r->streaming) {
        r->count++;
        const uint32_t id = RDOF_BENCHROUTE_STREAM | r->count;
        r->sid[0] = (uint8_t)(id >> 24);
        r->sid[1] = (uint8_t)(id >> 16);
        r->sid[2] = (uint8_t)(id >> 8);
        r->sid[3] = (uint8_t)id;
        r->seq = 0;
        r->src = r->dst = 0;
        r->group = false;
        r->streaming = true;
    }
    rdof_full_lc_t lc;
    if (sync_data && (dt == RDOF_DT_VOICE_LC_HEADER || dt == RDOF_DT_TERMINATOR_WITH_LC) &&
        rdof_decode_voice_lc(burst, dt, &lc) && lc.src_id >= 0 && lc.dst_id >= 0) {
        r->src = (uint32_t)lc.src_id;
        r->dst = (uint32_t)lc.dst_id;
        r->group = lc.group;
    }
    rdof_net_burst_t b;
    memset(&b, 0, sizeof b);
    memcpy(b.stream_id, r->sid, 4);
    b.control = control;
    memcpy(b.burst, burst, RDOF_BURST_BYTES);
    b.seq = r->seq++;
    b.src = r->src;
    b.dst = r->dst;
    b.group = r->group;
    b.slot = cmd == RDOF_MMDVM_DMR_DATA1 ? 1 : 2;
    b.terminator = term;
    if (!r->burst || !r->burst(r->ctx, &b)) {
        r->refused++;
        r->streaming = false;
        nak(r, cmd, 5); /* the transmitter isn't free */
        return;
    }
    r->bursts++;
    if (b.terminator) {
        r->streaming = false;
        r->ended = true;
    }
}

static void frame(void *ctx, uint8_t cmd, const uint8_t *p, size_t len)
{
    rdof_benchroute_t *r = ctx;
    switch (cmd) {
    case RDOF_MMDVM_GET_VERSION: {
        uint8_t v[1 + sizeof RDOF_BENCHROUTE_DESC] = {1};
        memcpy(v + 1, RDOF_BENCHROUTE_DESC, sizeof RDOF_BENCHROUTE_DESC - 1);
        send(r, cmd, v, sizeof v - 1);
        break;
    }
    case RDOF_MMDVM_SET_MODE:
        if (len >= 1 && p[0] == RDOF_MODE_IDLE) {
            r->streaming = false; /* unkeyed: the next burst is a new stream */
            r->ended = false;
        }
        ack(r, cmd);
        break;
    case RDOF_MMDVM_SET_CONFIG:
    case RDOF_MMDVM_SET_FREQ:
        ack(r, cmd);
        break;
    case RDOF_MMDVM_DMR_DATA1:
    case RDOF_MMDVM_DMR_DATA2:
        data(r, cmd, p, len);
        break;
    default:
        if (!rdof_mmdvm_unanswered(cmd))
            nak(r, cmd, 1); /* not supported */
        break;
    }
}

void rdof_benchroute_write(rdof_benchroute_t *r, const uint8_t *b, size_t n)
{
    rdof_mmdvm_reader_feed(&r->reader, b, n, frame, r);
}
