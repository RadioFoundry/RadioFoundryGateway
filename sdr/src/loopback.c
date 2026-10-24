// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * The SDR modem's loopback check (rdof_sdrmodem_loopback): the roamer's modem
 * client talks to the SDR modem; what it transmits goes, as the sim front
 * end's loopback puts it, into the receiver's samples along with another
 * station's bursts and some noise; the receiver's bursts go back to the modem.
 * Portable: the host tests and the firmware's self-test both run it.
 */
#include "rdof/sdr/sdrmodem.h"

#if RDOF_FEATURE_SDR && RDOF_FEATURE_SDR_RECEIVER && RDOF_FEATURE_TX

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdof/modem.h"
#include "rdof/pdu.h"
#include "rdof/sdr/receiver.h"

#define FS 288000
#define CENTER 461000000.0
#define CH0 461037500u
#define CH1 461087500u
#define CHUNK 4608
#define NB 3
#define MAXHEARD 64

typedef struct {
    rdof_sdrmodem_t modem;
    rdof_modem_t client;
    rdof_sdr_rx_t rx;
    /* what the host got (DMR_DATA) and what the receiver heard (not idle), by channel */
    uint8_t host[MAXHEARD][RDOF_BURST_BYTES];
    int host_slot[MAXHEARD], nhost, lost;
    uint8_t heard[2][MAXHEARD][RDOF_BURST_BYTES];
    int64_t heard_pos[2][MAXHEARD];
    int nheard[2], idle[2];
    uint32_t rng;
    double t;
} lb_t;

/* the modem's side */
static void lb_reply(void *ctx, const uint8_t *f, size_t len) { rdof_modem_feed(&((lb_t *)ctx)->client, f, len); }
static int lb_channel(void *ctx, uint32_t hz) { return hz == CH0 ? 0 : hz == CH1 ? 1 : -1; }
static bool lb_tx_tune(void *ctx, uint32_t hz, double *off)
{
    *off = (double)hz - CENTER;
    return fabs(*off) < FS * 0.4;
}

/* the client's port: synchronous, the reply is in by the time write returns */
static bool lb_write(void *ctx, const uint8_t *d, size_t len)
{
    rdof_sdrmodem_write(&((lb_t *)ctx)->modem, d, len);
    return true;
}
static bool lb_wait(void *ctx, int ms) { return ((lb_t *)ctx)->client.reply_ready; }
static double lb_clock(void *ctx) { return ((lb_t *)ctx)->t; }
static void lb_sleep(void *ctx, int ms) { ((lb_t *)ctx)->t += ms / 1e3; }

static void lb_on_rx(void *ctx, int slot, uint8_t control, const uint8_t *burst, int rssi)
{
    lb_t *l = ctx;
    if (l->nhost < MAXHEARD) {
        memcpy(l->host[l->nhost], burst, RDOF_BURST_BYTES);
        l->host_slot[l->nhost++] = slot;
    }
}
static void lb_on_lost(void *ctx, int slot) { ((lb_t *)ctx)->lost++; }

static void lb_burst(void *ctx, int c, const rdof_sdr_burst_t *b, double t)
{
    lb_t *l = ctx;
    rdof_sdrmodem_heard(&l->modem, c, b);
    if (b->b.dt == RDOF_DT_IDLE) {
        l->idle[c]++;
    } else if (l->nheard[c] < MAXHEARD) {
        memcpy(l->heard[c][l->nheard[c]], b->b.burst, RDOF_BURST_BYTES);
        l->heard_pos[c][l->nheard[c]++] = b->position;
    }
}
static bool lb_muted(void *ctx, int64_t p) { return rdof_sdrmodem_muted_at(&((lb_t *)ctx)->modem, p); }

static float noise(lb_t *l)
{
    float s = 0;
    for (int i = 0; i < 4; i++) { /* roughly normal, unit variance */
        l->rng = l->rng * 1664525u + 1013904223u;
        s += (float)(l->rng >> 8) / 16777216.0f - 0.5f;
    }
    return s * 1.732f;
}

static int find(const uint8_t (*list)[RDOF_BURST_BYTES], int n, const uint8_t *b)
{
    for (int i = 0; i < n; i++)
        if (!memcmp(list[i], b, RDOF_BURST_BYTES))
            return i;
    return -1;
}

#define FAIL(...)                                                                                                  \
    do {                                                                                                           \
        fails++;                                                                                                   \
        n += (size_t)snprintf(report + n, n < cap ? cap - n : 0, __VA_ARGS__);                                     \
    } while (0)

int rdof_sdrmodem_loopback(bool full, char *report, size_t cap)
{
    int fails = 0;
    size_t n = 0;
    if (cap)
        report[0] = 0;
    lb_t *l = calloc(1, sizeof *l);
    rdof_cf_t *iq = malloc(sizeof(rdof_cf_t) * CHUNK), *other_iq = malloc(sizeof(rdof_cf_t) * CHUNK);
    rdof_mod_t other;
    memset(&other, 0, sizeof other);
    if (!l || !iq || !other_iq) {
        free(l);
        free(iq);
        free(other_iq);
        snprintf(report, cap, "loopback: out of memory");
        return 1;
    }
    l->rng = 12345;
    const rdof_sdrmodem_io_t io = {lb_reply, lb_channel, lb_tx_tune, l, NULL};
    const rdof_sdr_rx_config_t rc = {FS, CENTER, 0, 0, true, CHUNK};
    const rdof_sdr_channel_t chans[2] = {{"A", CH0, 1, 1}, {"B", CH1, 1, 1}};
    if (rdof_sdrmodem_init(&l->modem, &io, FS, FS, full) || rdof_sdr_rx_init(&l->rx, &rc, chans, 2, NULL, 0) ||
        rdof_mod_init(&other, FS, (double)CH0 - CENTER, 4.0, 1.0f, 4, 0)) {
        FAIL("loopback: init failed (%s)", l->modem.error);
        goto out;
    }
    l->rx.on_burst = lb_burst;
    l->rx.muted = lb_muted;
    l->rx.burst_ctx = l;
    const rdof_modem_port_t port = {lb_write, lb_wait, NULL, lb_clock, lb_sleep, l};
    rdof_modem_init(&l->client, &port);
    l->client.on_rx = lb_on_rx;
    l->client.on_lost = lb_on_lost;
    l->client.cb_ctx = l;
    rdof_modem_config_t mc;
    rdof_modem_config_default(&mc);
    if (!rdof_modem_start(&l->client, 1) || rdof_radio_detect(l->client.version) != RDOF_RADIO_MMDVM_HS)
        FAIL("version: %s %s; ", l->client.version, l->client.error);
    if (!full && rdof_modem_configure(&l->client, &mc, 1, true))
        FAIL("duplex taken by a half-duplex front end; ");
    if (!rdof_modem_configure(&l->client, &mc, 1, full) || !rdof_modem_set_mode(&l->client, RDOF_MODE_DMR))
        FAIL("configure: %s; ", l->client.error);
    if (rdof_modem_set_freq(&l->client, 462000000, 462000000, 100))
        FAIL("a frequency the SDR doesn't hear taken; ");
    if (!rdof_modem_set_freq(&l->client, CH0, full ? CH1 : CH0, 100))
        FAIL("set_freq: %s; ", l->client.error);

    uint8_t ours[NB][RDOF_BURST_BYTES], theirs[NB][RDOF_BURST_BYTES];
    rdof_csbk_preambles(9990003, 91, true, NB, 1, ours);
    rdof_csbk_preambles(9990004, 92, true, NB, 1, theirs);
    for (int k = 0; k < NB; k++)
        if (!rdof_modem_write_dmr(&l->client, RDOF_SYNC_DATA | RDOF_DT_CSBK, ours[k], full ? 1 : 2))
            FAIL("write: %s; ", l->client.error);
    rdof_mmdvm_status_t st;
    if (!rdof_modem_status(&l->client, &st) || st.space != RDOF_SDRMODEM_QUEUE - (full ? 0 : NB) || !st.transmitting)
        FAIL("status: space %d, transmitting %d; ", st.space, st.transmitting);
    if (full)
        rdof_modem_dmr_start(&l->client, true);
    /* the other station: in the gaps between ours (simplex), on the RX channel */
    const int64_t slot = rdof_mod_slot_samples(&other), lead = rdof_mod_samples(&other, 24);
    for (int k = 0; k < NB; k++)
        rdof_mod_queue_burst(&other, lead + slot + 2 * k * slot, theirs[k], NULL, RDOF_MOD_RAMPS);
    const int chunks = (int)(0.9 * FS / CHUNK);
    int64_t pos = 0;
    for (int c = 0; c < chunks; c++, pos += CHUNK) {
        if (full && c == chunks / 2)
            rdof_modem_dmr_start(&l->client, false);
        rdof_sdrmodem_pull_iq(&l->modem, pos, iq, CHUNK);
        rdof_mod_iq(&other, other_iq, CHUNK);
        for (int i = 0; i < CHUNK; i++) {
            iq[i].re = 0.5f * (iq[i].re + other_iq[i].re) + 0.02f * noise(l);
            iq[i].im = 0.5f * (iq[i].im + other_iq[i].im) + 0.02f * noise(l);
        }
        rdof_sdr_rx_process(&l->rx, iq, CHUNK);
        l->t += (double)CHUNK / FS;
    }
    const int txc = full ? 1 : 0; /* the channel our transmitter is on */
    for (int k = 0; k < NB; k++) {
        int i = find((const uint8_t(*)[RDOF_BURST_BYTES])l->heard[txc], l->nheard[txc], ours[k]);
        if (i < 0)
            FAIL("our burst %d not heard by the receiver; ", k);
        else if (!full && l->heard_pos[txc][i] != l->modem.sent48[k])
            FAIL("our burst %d heard at %lld, sent for %lld; ", k, (long long)l->heard_pos[txc][i],
                 (long long)l->modem.sent48[k]);
        if (find((const uint8_t(*)[RDOF_BURST_BYTES])l->host, l->nhost, ours[k]) >= 0)
            FAIL("our own burst %d went up to the host; ", k);
        if (find((const uint8_t(*)[RDOF_BURST_BYTES])l->host, l->nhost, theirs[k]) < 0)
            FAIL("the other station's burst %d didn't reach the host; ", k);
    }
    if (l->modem.tx_bursts != NB)
        FAIL("%u bursts sent, want %d; ", (unsigned)l->modem.tx_bursts, NB);
    if (!full && (l->modem.muted != NB || l->nhost != NB))
        FAIL("muted %u, host got %d; ", (unsigned)l->modem.muted, l->nhost);
    if (full && (l->modem.idle_bursts < 4 || l->idle[1] < 4 || l->modem.keyed))
        FAIL("duplex: %u idle bursts sent, %d heard, keyed %d; ", (unsigned)l->modem.idle_bursts, l->idle[1],
             l->modem.keyed);
    if (l->lost != 1)
        FAIL("%d DMR_LOST, want 1 (no terminator); ", l->lost);
    if (rdof_sdrmodem_transmitting(&l->modem))
        FAIL("still transmitting at the end; ");
    if (!fails)
        snprintf(report, cap, "%s duplex loopback: %d bursts sent and heard back, %u muted, %d from another station "
                 "up to the host, %u idle", full ? "full" : "half", NB, (unsigned)l->modem.muted, l->nhost,
                 (unsigned)l->modem.idle_bursts);
out:
    rdof_mod_free(&other);
    rdof_sdr_rx_free(&l->rx);
    rdof_sdrmodem_free(&l->modem);
    free(l);
    free(iq);
    free(other_iq);
    return fails;
}

#endif
