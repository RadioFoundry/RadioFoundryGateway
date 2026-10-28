// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * An HBlink4 peer's state, without sockets: the board's peers (esp_idf/main/roam_hblink.c, sdr_hblink.c) and the
 * host's (sdr/cli/rdofradio_net.c) keep it the same way and differ only in how they send and receive.
 *
 * No login: the DMRC config packet every keepalive_s is the keepalive, the master answers DMRP (MSTN / MSTC
 * refuse), and three keepalives without an answer mark it down. Times are seconds on any steady clock.
 */
#ifndef RDOF_HBPEER_H
#define RDOF_HBPEER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/config.h"
#include "rdof/hblink.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t peer_id;
    double keepalive_s;
    uint8_t dmrc[RDOF_DMRC_MAX]; /* the last DMRC, sent again as the keepalive */
    size_t dmrc_len;
    double next_ka, last_pong;
    bool up;
    unsigned tx, rx, refused;
} rdof_hb_peer_t;

typedef enum {
    RDOF_HBP_NONE,       /* nothing to say */
    RDOF_HBP_REGISTERED, /* the first DMRP (or the first after it went down) */
    RDOF_HBP_REFUSED,    /* MSTN / MSTC */
    RDOF_HBP_QUIET,      /* three keepalives without a DMRP: down */
    RDOF_HBP_PACKET,     /* something else from the master: the caller's (rdof_hb_dispatch) */
} rdof_hbp_event_t;

void rdof_hb_peer_init(rdof_hb_peer_t *p, uint32_t peer_id, double keepalive_s);
/* The link (re)opened: the DMRC goes at the next tick. */
void rdof_hb_peer_link_up(rdof_hb_peer_t *p);
/* The link closed: down. */
void rdof_hb_peer_link_down(rdof_hb_peer_t *p);
/* A new DMRC. True when it differs from the last (or force): send p->dmrc now; the keepalive restarts from now. */
bool rdof_hb_peer_set_config(rdof_hb_peer_t *p, const uint8_t *dmrc, size_t n, bool force, double now);
/* True when the keepalive is due: send the DMRC (p->dmrc, or a new one through rdof_hb_peer_set_config). *ev is
   RDOF_HBP_QUIET when the master just went quiet. */
bool rdof_hb_peer_tick(rdof_hb_peer_t *p, double now, rdof_hbp_event_t *ev);
/* A packet from the master. */
rdof_hbp_event_t rdof_hb_peer_received(rdof_hb_peer_t *p, const uint8_t *d, size_t n, double now);

/* A role's DMRC: a repeater's RX / TX and slots; a roamer's (a transceiver's) channel now, and its idle channel. */
size_t rdof_hb_role_dmrc(const rdof_config_t *c, uint32_t freq_now, int cc_now, uint8_t out[RDOF_DMRC_MAX]);
/* A receive-only channel's (an SDR channel's or a monitor's): the channel id (8 characters) as the callsign. */
size_t rdof_hb_channel_dmrc(uint32_t peer_id, const char *id, uint32_t freq, int cc, const rdof_location_t *loc,
                            uint8_t out[RDOF_DMRC_MAX]);

struct rdof_repeater;
struct rdof_txvr;
/*
 * A packet from the master for a role: DMRD to the repeater or the transceiver, DMRT to the transceiver. A DMRT
 * with no transceiver to take it (a receive-only build or role) is refused: the DMRK is in dmrk and it returns
 * true (send it). Either of rpt and txvr may be NULL.
 */
bool rdof_hb_dispatch(const uint8_t *d, size_t n, struct rdof_repeater *rpt, struct rdof_txvr *txvr,
                      uint32_t peer_id, uint8_t dmrk[RDOF_DMRK_LEN]);

#ifdef __cplusplus
}
#endif

#endif
