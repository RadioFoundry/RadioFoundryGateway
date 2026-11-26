// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * rdofradio's settings (--config FILE): the firmware's settings document (version 3, docs/spec/fw/config-v3.md),
 * read by the firmware's parser (rdof/siteplan.h), and the part of it rdofradio applies. Split repeaters whose RX
 * half is the SDR run as many as the file has (up to RDOF_ROLES_MAX), each with its own radio as the TX half, at its
 * device's port path (or --split-tx's for the first). Otherwise the radio is the first
 * unit that transmits: a repeater (a repeater app on the SDR or on a modem) or a half duplex transceiver (the SDR
 * or a modem transmitting on simplex frequencies, on its idle frequency); a unit on a modem runs with --mmdvm. From
 * it: the name, peer id, rx / tx frequencies, cc, parrot, and a repeater's slots, hang_s, call_hang_s,
 * short_lc_ts1_low, prebuffer and the jitter buffer, cwid and cwid_every_s; a modem's mmdvm settings (levels,
 * offsets, the RF level of each frequency); the hblink network (the master), callsign, location and power for its
 * HBlink4 peer, as the board's; the mqtt network, published as the board does (rdofradio_mqtt.h); the SDR's
 * channels (as the board's SDR resolves them: the unit hears on the one at its frequency); and the site, for the call
 * router (rdof/siteroute.h: what goes up and what comes down, through the radio's peer unless identity is per_path).
 * A trunked site (a tier3 app) runs alone: trunk below, from its app and the mqtt network (rdofradio_t3.h).
 * mode sfr and dcdm are refused. The devices stay on the command line (--soapy, -r, --gain, --tx-gain ..., --mmdvm
 * PORT).
 * No I/O here: rdofradio reads and writes the file.
 */
#ifndef RDOFRADIO_CFG_H
#define RDOFRADIO_CFG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/benchtx.h"
#include "rdof/ipgw.h"
#include "rdof/config.h"
#include "rdof/repeater.h"
#include "rdof/site.h"
#include "rdof/siteplan.h"
#include "rdof/t3.h"
#include "rdof/txvr.h"
#include "rdof/sdr/config.h"

/* A split repeater on the SDR: the SDR hears in, a radio on port sends out. */
typedef struct {
    char name[24];
    uint32_t peer_id, rx_hz, tx_hz;
    int cc;
    char port[64];      /* the TX half's device's port path; "" none (the first takes --split-tx's) */
    long baud;
    rdof_config_t cfg;  /* the unit as the firmware resolves it (its repeater's settings, split.power_cap ...) */
} rdofradio_split_t;

/* A trunked site (docs/spec/fw/tier3.md): its controller's settings, carriers and the mqtt network's connection. */
typedef struct {
    char name[24];      /* the unit's (the tier3 app's) */
    int path;           /* its path on the site, -1 none */
    int net;            /* the mqtt network (site.net), -1 none: the site alone */
    rdof_t3_cfg_t cfg;  /* the settings file's (rdof_t3cfg_plan) */
    rdof_t3_policy_t pol;
    int ncar;
    struct {
        uint32_t rx_hz, tx_hz;
        bool tx_sdr;    /* the SDR sends it; else a radio (a split's TX half) on port */
        int tx_dev;     /* the device that sends it (the settings' index), -1 none */
        char port[64];
        long baud;
        bool rx_sdr;        /* an SDR hears its in; else a radio (a split's RX half) on rx_port */
        char rx_port[64];
        long rx_baud;
    } car[RDOF_T3_CARRIERS];
    char host[64], user[32], pass[64], root[32];
    int port;
    bool tls;
    /* what the site's rules (config-v3.md 8.1) keep off the MQTT network [0] and the login [1]: talkgroups, private
       calls */
    rdof_idset_t deny_tg[2], deny_pc[2];
    /* a brandmeister or homebrew network (tier3.md 5.11): the site's own login, -1 none */
    int bm;
    uint32_t bm_peer;       /* its ID (peer_ids, under the app's name) */
    bool bm_dl5di;          /* brandmeister: the 2015 specification's dialect; homebrew: HBlink's */
    char bm_host[64], bm_pass[64], bm_location[21], bm_description[20], bm_url[125], callsign[12];
    int bm_port;
    bool bm_activate;       /* key the talkgroups the site's radios register to (default true) */
    double bm_renew_s;      /* key them again after this long with none keyed here (0: never; default 840) */
    bool bm_announce;       /* key a radio that registers on TG 9 from its ID (default true) */
    double bm_unlink_s;     /* TG 4000 this long after the last call on a talkgroup no radio wants (default 120) */
    uint32_t bm_pin_tg[32]; /* ts1_tg, ts2_tg: talkgroups that always go on that slot of the login */
    uint8_t bm_pin_slot[32];
    int bm_npin;
    bool has_location;
    double latitude, longitude;
    int height_m;
    /* "standby" (tier3.md 6.2): two boxes with these settings, one on the air; it waits while the node is online
       and takes over takeover_s after its will; the active one stops when its broker is gone fence_s while a
       standby is there */
    bool standby;
    double takeover_s, fence_s;
} rdofradio_t3_cfg_t;

typedef struct {
    bool full;          /* a repeater (full duplex); false: a transceiver (half duplex) */
    bool on_modem;      /* the unit is on a modem device (rdofradio --mmdvm PORT), not on the SDR */
    char box[32];       /* the box's name */
    char web_hosts[RDOF_WEB_HOSTS]; /* more names the web page answers to (rdof_hostweb_hosts), "" none */
    char name[24];      /* the unit's */
    uint32_t peer_id;
    uint32_t rx_hz, tx_hz;
    int cc;
    int rx_slot;        /* a direct mode call's slot on rx_hz (the frequency's ts; a repeater: 1) */
    bool has_parrot;
    uint32_t parrot;    /* 0: everything */
    /* the clock (ntp): the board's (netclock) from servers, or the OS's (chrony, ntpd: clock "os") */
    bool own_clock;
    char ntp_servers[256];
    unsigned ntp_poll_s; /* 0: netclock's default */
    bool has_hblink;    /* an hblink network: a peer of cfg.master_host */
    bool unit_hblink;   /* the unit itself is a peer there (its path is on it, or the box has one identity) */
    /* the SDR's receive channels, as the board's SDR resolves them: its receiver's frequencies, then the units'
       (the transceiver's, a repeater's input; not a repeater's output). Each with hblink and a peer_id is an
       HBlink4 peer of its own (identity per_path); the others go up through the unit's peer, as its policy says */
    int nchan;
    rdof_sdr_cfg_channel_t chan[RDOF_SDR_MAX_CHANNELS];
    char sdr_name[32], sdr_app[16]; /* their MQTT names, as the board's SDR's (name, app "sdr") */
    double sdr_status_s;            /* the SDR's status period (status_seconds) */
    rdof_site_t site;
    rdof_config_t cfg;  /* all of it, as the firmware resolves it */
    double tx_power_dbm; /* the unit's SDR device's tx.power_dbm, at the antenna; NAN: not given */
    /* split repeaters with the SDR as the RX half: the first is the radio above */
    int nsplit;
    rdofradio_split_t split[RDOF_ROLES_MAX];
    bool tier3;         /* a trunked site: t, and the site, box, clock; nothing else above */
    rdofradio_t3_cfg_t t;
    /* the IP gateway app (apps: type ipgw), when the document has one */
    bool has_ipgw;
    rdof_ipgw_config_t ipgw;
    /* bench.tx: where and how the bench's jobs may transmit, added to --bench-tx (the line itself is --bench) */
    bool has_bench_tx;
    rdof_benchtx_policy_t bench_tx;
} rdofradio_cfg_t;

/* Read a settings document. False: err says why (the parser's reason, or what rdofradio can't run). */
bool rdofradio_cfg_parse(const char *text, size_t len, rdofradio_cfg_t *out, char *err, size_t err_cap);
/* The channel on hz (within 100 Hz); -1 none. */
int rdofradio_cfg_channel_at(const rdofradio_cfg_t *c, uint32_t hz);
/* Last heard for a tracker's line, from a channel that is an SDR channel (sdr), the unit's (unit), or both: the SDR's
   entry, and the unit's only when no SDR channel reports it (--mmdvm's modem), as the board's SDR roamer keeps quiet
   (rx_path.c). One call on air is one entry. */
static inline bool rdofradio_cfg_heard_unit(bool sdr, bool unit) { return unit && !sdr; }
/* The settings document (version 3) for what the command line set (shown read only on the page, a start for a
   file): a repeater app, or the SDR as a transceiver, on driver (soapy, iio: the flag's); a parrot app for a parrot
   ID (0, every ID, has none: a parrot app answers one). */
size_t rdofradio_cfg_from_flags(char *out, size_t cap, const char *driver, bool full, uint32_t rx_hz, uint32_t tx_hz,
                                int cc, bool has_parrot, uint32_t parrot, uint32_t peer_id);
/* A repeater's settings past rdof_repeater_init's (slots, hangs, jitter buffer, CW ID), as the firmware sets them:
   the radio's, or a split's (rdofradio_cfg_apply_unit, from its own cfg). */
void rdofradio_cfg_apply_repeater(const rdofradio_cfg_t *c, rdof_repeater_t *r);
void rdofradio_cfg_apply_unit(const rdof_config_t *c, rdof_repeater_t *r);
/* A transceiver's settings past rdof_txvr_init's, as the firmware's roamer (radio_task.c): the channels it may be
   tuned to (the roamer's channels and idle), listen and clear, the jitter buffer, CSBK preambles, the answer
   window, the idle channel and the return to it, CW ID. Not the scan: the SDR hears every channel at once. */
void rdofradio_cfg_apply_txvr(const rdofradio_cfg_t *c, rdof_txvr_t *t);
/* A unit on a modem (on_modem): its radio from the device's type and mmdvm settings, as the firmware's radio task
   sets it up once the modem has answered GET_VERSION (description: its reply). False with err set when the type
   can't be told ("auto" and a description rdof_radio_detect doesn't know) or an MMDVM board has no tuner. */
bool rdofradio_cfg_radio(const rdofradio_cfg_t *c, rdof_radio_t *r, rdof_modem_t *modem, const char *description,
                         char *err, size_t cap);
/* /api/resolved: the radio as resolved, and device (the command line's device flags). */
size_t rdofradio_cfg_describe(const rdofradio_cfg_t *c, const char *source, const char *device, char *out,
                              size_t cap);

#endif
