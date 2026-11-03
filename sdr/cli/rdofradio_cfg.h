// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * rdofradio's settings (--config FILE): the firmware's settings document (version 3, docs/spec/fw/config-v3.md),
 * read by the firmware's parser (rdof/siteplan.h), and the part of it rdofradio applies. The radio is the first
 * unit that transmits: a repeater (a repeater app on the SDR or on a modem) or a half duplex transceiver (the SDR
 * or a modem transmitting on simplex frequencies, on its idle frequency); a unit on a modem runs with --mmdvm. From
 * it: the name, peer id, rx / tx frequencies, cc, parrot, and a repeater's slots, hang_s, call_hang_s,
 * short_lc_ts1_low, prebuffer and the jitter buffer, cwid and cwid_every_s; a modem's mmdvm settings (levels,
 * offsets, the RF level of each frequency); the hblink network (the master), callsign, location and power for its
 * HBlink4 peer, as the board's; the mqtt network, published as the board does (rdofradio_mqtt.h); the SDR's other
 * frequencies (receive-only channels, as the board's SDR resolves them); and the site, for the call router
 * (rdof/siteroute.h: what goes up and what comes down, through the radio's peer unless identity is per_path). mode
 * sfr and dcdm are refused. The devices stay on the command line (--soapy, -r, --gain, --tx-gain ..., --mmdvm PORT).
 * No I/O here: rdofradio reads and writes the file.
 */
#ifndef RDOFRADIO_CFG_H
#define RDOFRADIO_CFG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/config.h"
#include "rdof/repeater.h"
#include "rdof/site.h"
#include "rdof/txvr.h"
#include "rdof/sdr/config.h"

typedef struct {
    bool full;          /* a repeater (full duplex); false: a transceiver (half duplex) */
    bool on_modem;      /* the unit is on a modem device (rdofradio --mmdvm PORT), not on the SDR */
    char box[32];       /* the box's name */
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
    /* the SDR's other channels (as the board's SDR resolves them), past the radio's own RX and TX; each with
       hblink and a peer_id is an HBlink4 peer of its own (identity per_path) */
    int nmon;
    rdof_sdr_cfg_channel_t mon[RDOF_SDR_MAX_CHANNELS];
    char mon_name[32], mon_app[16]; /* their MQTT names, as the board's SDR's (name, app "sdr") */
    double mon_status_s;            /* the SDR's status period (status_seconds) */
    rdof_site_t site;
    rdof_config_t cfg;  /* all of it, as the firmware resolves it */
} rdofradio_cfg_t;

/* Read a settings document. False: err says why (the parser's reason, or what rdofradio can't run). */
bool rdofradio_cfg_parse(const char *text, size_t len, rdofradio_cfg_t *out, char *err, size_t err_cap);
/* The settings document (version 3) for what the command line set (shown read only on the page, a start for a
   file): a repeater app, or the SDR as a transceiver, on driver (soapy, iio: the flag's); a parrot app for a parrot
   ID (0, every ID, has none: a parrot app answers one). */
size_t rdofradio_cfg_from_flags(char *out, size_t cap, const char *driver, bool full, uint32_t rx_hz, uint32_t tx_hz,
                                int cc, bool has_parrot, uint32_t parrot, uint32_t peer_id);
/* A repeater's settings past rdof_repeater_init's (slots, hangs, jitter buffer, CW ID), as the firmware sets them. */
void rdofradio_cfg_apply_repeater(const rdofradio_cfg_t *c, rdof_repeater_t *r);
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
