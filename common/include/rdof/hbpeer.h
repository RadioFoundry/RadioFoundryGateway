// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * An HBlink4 peer's state, without sockets: the board's peers (esp_idf/main/roam_hblink.c, sdr_hblink.c) and the
 * host's (sdr/cli/rdofradio_net.c) keep it the same way and differ only in how they send and receive.
 *
 * No login (HBlink4): the DMRC config packet every keepalive_s is the keepalive, the master answers DMRP (MSTN /
 * MSTC refuse), and three keepalives without an answer mark it down. Times are seconds on any steady clock.
 *
 * With a login (rdof_hb_peer_set_login, BrandMeister: docs/spec/fw/brandmeister.md): RPTL, RPTACK + salt, RPTK
 * (SHA-256 of salt and password), RPTACK, RPTC (made from the DMRC the caller keeps setting), RPTACK; then RPTPING
 * every keepalive_s, MSTPONG back, down after three unanswered. A refusal or a step with no answer in 5 s backs off
 * 1 s, doubling to 60 s (a refused password: 60 s at once). DMRD from the master count only while logged in.
 *
 * The callers' loop is the same either way: when rdof_hb_peer_tick says so, set the config (the DMRC) and send what
 * rdof_hb_peer_packet gives.
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

#define RDOF_RPTC_LEN 302
#define RDOF_HB_OUT_MAX RDOF_RPTC_LEN /* the largest packet rdof_hb_peer_packet makes */
#define RDOF_HB_LOGIN_TIMEOUT_S 5.0
#define RDOF_HB_BACKOFF_MAX_S 60.0

/* A login's step. */
typedef enum {
    RDOF_HBL_LOGIN = 0, /* RPTL due, or sent and waiting for the salt */
    RDOF_HBL_AUTH,      /* RPTK */
    RDOF_HBL_CONFIG,    /* RPTC */
    RDOF_HBL_UP,        /* logged in: RPTPING */
    RDOF_HBL_BACKOFF,   /* waiting to start again */
} rdof_hbl_state_t;

typedef struct {
    uint32_t peer_id;
    double keepalive_s;
    uint8_t dmrc[RDOF_DMRC_MAX]; /* the last DMRC, sent again as the keepalive */
    size_t dmrc_len;
    double next_ka, last_pong;
    bool up;
    unsigned tx, rx, refused;
    /* the login (rdof_hb_peer_set_login); all zero without one */
    bool login;
    uint8_t state;       /* rdof_hbl_state_t */
    uint8_t refused_at;  /* the step the last refusal came at (rdof_hbl_state_t) */
    bool bad_password;   /* the last refusal came at RPTK */
    bool pending;        /* this step's packet is due now */
    bool relogin;        /* the config changed: log in again at the next tick */
    uint8_t salt[4];
    double backoff_s;
    unsigned logins, pings_missed;
    char password[64];
    char location[21], description[20], url[125];
} rdof_hb_peer_t;

typedef enum {
    RDOF_HBP_NONE,       /* nothing to say */
    RDOF_HBP_REGISTERED, /* the first DMRP (or the first after it went down) */
    RDOF_HBP_REFUSED,    /* MSTN / MSTC */
    RDOF_HBP_QUIET,      /* three keepalives without a DMRP: down */
    RDOF_HBP_PACKET,     /* something else from the master: the caller's (rdof_hb_dispatch) */
} rdof_hbp_event_t;

void rdof_hb_peer_init(rdof_hb_peer_t *p, uint32_t peer_id, double keepalive_s);
/* Log in (after init): RPTC's location, description and URL fields (cut to fit; NULL: empty). */
void rdof_hb_peer_set_login(rdof_hb_peer_t *p, const char *password, const char *location, const char *description,
                            const char *url);
/* The link (re)opened: the DMRC goes at the next tick. */
void rdof_hb_peer_link_up(rdof_hb_peer_t *p);
/* The link closed: down. */
void rdof_hb_peer_link_down(rdof_hb_peer_t *p);
/* A new DMRC. True when it differs from the last (or force): send p->dmrc now; the keepalive restarts from now.
   With a login it never asks to send: a changed config while logging in or logged in starts the login again. */
bool rdof_hb_peer_set_config(rdof_hb_peer_t *p, const uint8_t *dmrc, size_t n, bool force, double now);
/* True when something is due: set the config (rdof_hb_peer_set_config) and send rdof_hb_peer_packet's. *ev is
   RDOF_HBP_QUIET when the master just went quiet. */
bool rdof_hb_peer_tick(rdof_hb_peer_t *p, double now, rdof_hbp_event_t *ev);
/* The packet due (after tick returned true): the DMRC, or the login's step or ping. Returns its length. */
size_t rdof_hb_peer_packet(const rdof_hb_peer_t *p, uint8_t out[RDOF_HB_OUT_MAX]);
/* RPTCL (a login, on shutdown) into out; 0 without a login. */
size_t rdof_hb_peer_close_packet(const rdof_hb_peer_t *p, uint8_t out[9]);
/* Whether DMRD may go up now: always without a login, only logged in with one. */
bool rdof_hb_peer_may_send(const rdof_hb_peer_t *p);
/* The state's name for status: "up", "down" (no login) or the login's step: "login", "auth", "config",
   "bad_password", "refused", "backoff". */
const char *rdof_hb_peer_state_name(const rdof_hb_peer_t *p);

/* RPTC from a DMRC (rdof/hblink.h's layout): callsign, frequencies, power, color code, slots, location, the DMRC's
   version and software as RPTC's software and package IDs; location, description and url as given (NULL: empty).
   Returns RDOF_RPTC_LEN. */
size_t rdof_rptc_from_dmrc(const uint8_t *dmrc, size_t n, const char *location, const char *description,
                           const char *url, uint8_t out[RDOF_RPTC_LEN]);
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
