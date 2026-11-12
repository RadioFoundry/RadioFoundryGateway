// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* A trunked site's payload carrier on a split TX half (rdof/sdr/t3split.h). */
#include "rdof/sdr/t3split.h"

#include <stdio.h>
#include <string.h>

#include "rdof/layer2.h"
#include "rdof/pdu.h"
#include "rdof/tracker.h"
#include "rdof/split.h"
#include "rdof/splitops.h"

#if RDOF_FEATURE_REPEATER

#define SETUP_WAIT_S 2.0

static void ts_to_radio(void *ctx, int i, const uint8_t *d, size_t n)
{
    rdof_t3split_t *t = ctx;
    t->io.to_radio(t->io.ctx, d, n);
}

static void on_up(void *ctx, uint8_t cmd, const uint8_t *d, size_t n)
{
    rdof_t3split_t *t = ctx;
    if (cmd == RDOF_MMDVM_ACK || cmd == RDOF_MMDVM_NAK) {
        if (t->state != RDOF_T3SPLIT_SETUP)
            return;
        if (cmd == RDOF_MMDVM_NAK) {
            t->naks++;
            snprintf(t->error, sizeof t->error, "the split refused its setup (NAK 0x%02x err %d)", n ? d[0] : 0,
                     n > 1 ? d[1] : 0);
            t->state = RDOF_T3SPLIT_FAILED;
            return;
        }
        t->setup_step++;
        t->setup_at = -1; /* the next goes out on the next tick */
        return;
    }
    if ((cmd == RDOF_MMDVM_DMR_DATA1 || cmd == RDOF_MMDVM_DMR_DATA2) && n >= 1 + RDOF_BURST_BYTES) {
        const int next = (t->qhead + 1) % RDOF_T3SPLIT_Q;
        if (next == t->qtail) {
            t->dropped++;
            return;
        }
        t->q[t->qhead].slot = cmd == RDOF_MMDVM_DMR_DATA2 ? 2 : 1;
        t->q[t->qhead].control = d[0];
        t->q[t->qhead].rssi = 0;
        memcpy(t->q[t->qhead].burst, d + 1, RDOF_BURST_BYTES);
        t->qhead = next;
        t->heard++;
    }
}

static void ts_to_host(void *ctx, int i, const uint8_t *d, size_t n)
{
    rdof_t3split_t *t = ctx;
    rdof_mmdvm_reader_feed(&t->reader, d, n, on_up, t);
}

static int ts_channel(void *ctx, uint32_t hz)
{
    rdof_t3split_t *t = ctx;
    return t->io.rx_channel ? t->io.rx_channel(t->io.ctx, hz) : -1;
}

static int64_t ts_now48(void *ctx)
{
    rdof_t3split_t *t = ctx;
    return t->io.now48(t->io.ctx);
}

bool rdof_t3split_init(rdof_t3split_t *t, const rdof_t3split_io_t *io, uint32_t rx_fs, int carrier,
                          uint32_t rx_hz, uint32_t tx_hz, int cc, double now)
{
    memset(t, 0, sizeof *t);
    t->io = *io;
    t->rx_hz = rx_hz, t->tx_hz = tx_hz, t->cc = cc;
    rdof_mmdvm_reader_init(&t->reader);
    rdof_t3rf_split_init(&t->o, carrier);
    const rdof_sdrsplits_io_t sio = {ts_to_radio, ts_to_host, ts_channel, io->now48 ? ts_now48 : NULL, t};
    rdof_sdrsplits_init(&t->set, &sio, rx_fs);
    rdof_split_params_t p;
    rdof_split_params_default(&p);
    p.stream_mode = RDOF_OPS_STREAM_SCHEDULED;
    if (rdof_sdrsplits_add(&t->set, &p, now) < 0) {
        snprintf(t->error, sizeof t->error, "%s", t->set.error);
        t->state = RDOF_T3SPLIT_FAILED;
        return false;
    }
    return true;
}

void rdof_t3split_free(rdof_t3split_t *t) { rdof_sdrsplits_free(&t->set); }

void rdof_t3split_from_radio(rdof_t3split_t *t, const uint8_t *d, size_t n, double now)
{
    rdof_sdrsplits_from_radio(&t->set, 0, d, n, now);
}

void rdof_t3split_heard(rdof_t3split_t *t, int channel, const rdof_sdr_burst_t *b)
{
    rdof_sdrsplits_heard(&t->set, channel, b);
}

/* the setup's frames, as a repeater's start sends them: duplex DMR, the mode, the carrier's frequencies */
static void setup_send(rdof_t3split_t *t, double now)
{
    uint8_t body[20] = {0}, f[64];
    size_t n = 0;
    uint8_t cmd;
    switch (t->setup_step) {
    case 0:
        body[3] = RDOF_MODE_DMR;  /* the mode after it */
        body[6] = (uint8_t)t->cc; /* DMR color code */
        n = 20;
        cmd = RDOF_MMDVM_SET_CONFIG;
        break;
    case 1:
        body[0] = RDOF_MODE_DMR;
        n = 1;
        cmd = RDOF_MMDVM_SET_MODE;
        break;
    default:
        for (int k = 0; k < 4; k++)
            body[1 + k] = (uint8_t)(t->rx_hz >> (8 * k)), body[5 + k] = (uint8_t)(t->tx_hz >> (8 * k));
        body[9] = 255; /* RF level: full (the radio's own cap applies) */
        n = 10;
        cmd = RDOF_MMDVM_SET_FREQ;
        break;
    }
    const size_t len = rdof_mmdvm_frame(cmd, body, n, f, sizeof f);
    t->setup_at = now;
    rdof_sdrsplits_upstream(&t->set, 0, f, len, now);
}

void rdof_t3split_tick(rdof_t3split_t *t, double now)
{
    rdof_sdrsplits_tick(&t->set, now);
    const rdof_split_t *s = rdof_sdrsplits_split(&t->set, 0);
    if (t->state == RDOF_T3SPLIT_FAILED)
        return;
    if (s->failed[0]) {
        snprintf(t->error, sizeof t->error, "%s", s->failed);
        t->state = RDOF_T3SPLIT_FAILED;
        return;
    }
    if (t->state == RDOF_T3SPLIT_BRINGUP && s->ready) {
        t->state = RDOF_T3SPLIT_SETUP;
        t->setup_step = 0;
        setup_send(t, now);
    } else if (t->state == RDOF_T3SPLIT_SETUP) {
        if (t->setup_step >= 3)
            t->state = RDOF_T3SPLIT_UP;
        else if (t->setup_at < 0)
            setup_send(t, now);
        else if (now - t->setup_at > SETUP_WAIT_S) {
            snprintf(t->error, sizeof t->error, "the split didn't answer its setup (step %d)", t->setup_step);
            t->state = RDOF_T3SPLIT_FAILED;
        }
    }
}

void rdof_t3split_utc(rdof_t3split_t *t, double utc_minus_host_us, double bound_us, double now)
{
    rdof_split_set_utc(rdof_sdrsplits_split(&t->set, 0), true, utc_minus_host_us, bound_us, 0, true, now);
}

struct rdof_split *rdof_t3split_split(rdof_t3split_t *t) { return rdof_sdrsplits_split(&t->set, 0); }

bool rdof_t3split_take(rdof_t3split_t *t, int *slot, uint8_t *control, uint8_t burst[RDOF_BURST_BYTES],
                          int *rssi)
{
    if (t->qtail == t->qhead)
        return false;
    *slot = t->q[t->qtail].slot;
    *control = t->q[t->qtail].control;
    if (rssi)
        *rssi = t->q[t->qtail].rssi;
    memcpy(burst, t->q[t->qtail].burst, RDOF_BURST_BYTES);
    t->qtail = (t->qtail + 1) % RDOF_T3SPLIT_Q;
    return true;
}

void rdof_t3split_burst(const rdof_t3split_t *t, int slot, uint8_t control, const uint8_t burst[RDOF_BURST_BYTES],
                           rdof_burst_t *out)
{
    memset(out, 0, sizeof *out);
    out->family = RDOF_FAMILY_MS;
    out->cc = t->cc;
    out->slot = slot;
    memcpy(out->burst, burst, RDOF_BURST_BYTES);
    if (control & RDOF_SYNC_DATA) {
        out->dt = control & 0x0F;
    } else if (control & RDOF_SYNC_AUDIO) {
        out->dt = RDOF_DT_VOICE_SYNC;
        out->n = 0;
    } else {
        out->dt = RDOF_DT_VOICE;
        out->n = control & 0x0F;
    }
}

#endif /* RDOF_FEATURE_REPEATER */
