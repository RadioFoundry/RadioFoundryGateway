// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * HBlink4 packets (no sockets).
 *
 * The peers use the DMRC protocol MMDVMHost uses: no login handshake; a
 * DMRC config packet every few seconds doubles as the keepalive, and the
 * master answers DMRP (MSTN / MSTC refuse). Bursts go up as 55-byte DMRD.
 * A roaming transceiver also takes DMRT (tune) and answers DMRK.
 */
#ifndef RDOF_HBLINK_H
#define RDOF_HBLINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/fec.h"
#include "rdof/features.h"
#include "rdof/tracker.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RDOF_HB_KEEPALIVE_SECONDS 10.0
#define RDOF_HB_ROAMING_KEEPALIVE_SECONDS 5.0
#define RDOF_HB_HOLD_FRAMES 12
#define RDOF_DMRD_LEN 55
#define RDOF_DMRC_MAX 160 /* 119 + location 20 + idle channel 11 */
#define RDOF_DMRK_LEN 18

typedef struct {
    double latitude, longitude;
    int height_m;
} rdof_location_t;

/* DMRC. location and idle may be NULL (idle: frequency Hz and color code). Returns the length. */
size_t rdof_dmrc_packet(uint32_t peer_id, const char *callsign, uint32_t rx_hz, uint32_t tx_hz, int cc,
                      char slots, int power, const char *version, const char *software,
                      const rdof_location_t *location, const uint32_t *idle_freq, const int *idle_cc,
                      uint8_t out[RDOF_DMRC_MAX]);

void rdof_dmrd_packet(int seq, uint32_t src, uint32_t dst, uint32_t peer_id, int slot, bool group, int dt, int n,
                    uint32_t stream_id, const uint8_t burst[RDOF_BURST_BYTES], int ber, int rssi,
                    uint8_t out[RDOF_DMRD_LEN]);

/* A burst the master sends down to transmit. */
typedef struct {
    uint8_t stream_id[4];
    uint8_t control; /* the MMDVM DMR_DATA control byte */
    uint8_t burst[RDOF_BURST_BYTES];
    bool terminator;
    uint8_t seq; /* DMRD's sequence number (the jitter buffer counts lost bursts by it) */
    uint32_t src, dst;
    bool group;
    int slot;
} rdof_net_burst_t;

bool rdof_parse_dmrd(const uint8_t *data, size_t len, rdof_net_burst_t *out);
/* A CSBK that is a whole transmission on its own (good CRC, not a preamble). */
bool rdof_single_csbk(const uint8_t burst[RDOF_BURST_BYTES]);

typedef struct {
    uint8_t stream_id[4];
    uint32_t freq;
    int cc, power;
    bool preempt;
} rdof_tune_t;

bool rdof_parse_dmrt(const uint8_t *data, size_t len, rdof_tune_t *out);

enum { RDOF_DMRK_ON_AIR = 0, RDOF_DMRK_CHANNEL_BUSY, RDOF_DMRK_NOT_ALLOWED, RDOF_DMRK_RADIO_ERROR, RDOF_DMRK_TXVR_BUSY };
void rdof_dmrk_packet(uint32_t peer_id, const uint8_t stream_id[4], int status, uint32_t freq, int cc,
                    uint8_t out[RDOF_DMRK_LEN]);

/* What a received packet is: 'P' DMRP, 'N' refused (MSTN / MSTC), 'T' DMRT, 'D' DMRD, 0 other. */
char rdof_hb_kind(const uint8_t *data, size_t len);

#if RDOF_FEATURE_TRACKER
/*
 * The HBlink sink for one peer: frames from the tracker -> DMRD, holding voice
 * heard before the late-entry LC and resending it after the synthesized
 * header. send() gets each packet; before_stream() (optional) runs before a
 * new stream's first DMRD (the roamer sends DMRC then); rand32() makes stream ids.
 */
typedef struct {
    uint32_t peer_id;
    void (*send)(void *ctx, const uint8_t *data, size_t len);
    void (*before_stream)(void *ctx);
    uint32_t (*rand32)(void *ctx);
    void *ctx;
    int seq[2];
    uint32_t stream[2];
    rdof_frame_t held[2][RDOF_HB_HOLD_FRAMES];
    int nheld[2];
} rdof_hb_sink_t;

void rdof_hb_sink_init(rdof_hb_sink_t *s, uint32_t peer_id);
void rdof_hb_sink_on_frame(rdof_hb_sink_t *s, const rdof_frame_t *f);
/* An rdof_sink_t for a tracker whose on_frame feeds this sink. */
rdof_sink_t rdof_hb_sink_as_sink(rdof_hb_sink_t *s);
#endif /* RDOF_FEATURE_TRACKER */


#ifdef __cplusplus
}
#endif

#endif
