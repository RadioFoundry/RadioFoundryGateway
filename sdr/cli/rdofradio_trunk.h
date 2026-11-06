// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * rdofradio's trunked site (docs/spec/fw/trunk.md): a settings file with a trunk app runs it, the controller and the
 * radio layer in rdof/trunkunit.h, on the MQTT network as the box's node (with libmosquitto; without it the site
 * runs alone). Built with RDOF_TRUNK (cargo found: the controller is rust's t3 feature).
 *
 * --sim-air N: no device. N simulated MSs (rdof/trunksim.h) on the site's air, the full burst path each slot, the slot
 * clock on the wall clock (30 ms a slot), the network real. Each registers at start (staggered, with --sim-tg's
 * talkgroup); --sim-talk S makes one of them call the talkgroup every S seconds; --sim-script FILE adds timed actions,
 * one a line ("#" comments):
 *     AT_S  ID  register [TG] | deregister | group TG [TALK_MS] | private ID2 [TALK_MS] | cancel
 * An ID not among the N is a simulated MS of its own, idle until the script makes it act.
 *
 * On a device: the SDR is carrier 1 (the TSCC's): rdof_sdrmodem's slot source, keyed with no end (hold_key), what it
 * hears on carrier 1's inbound given its slot by timing (rdof_sdrmodem_inbound_k). One carrier for now.
 *
 * --state-dir DIR keeps the provisioning (cfg-cache.json) across restarts.
 */
#ifndef RDOFRADIO_TRUNK_H
#define RDOFRADIO_TRUNK_H

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>

#include "rdof/layer2.h"
#include "rdofradio_cfg.h"

typedef struct {
    int sim_n;               /* --sim-air: the MSs (0: on a device) */
    uint32_t sim_id;         /* the first one's ID (--sim-id; the rest follow) */
    uint32_t sim_tg;         /* --sim-tg */
    double sim_talk_s;       /* --sim-talk: a group call every this many seconds, 0 none */
    const char *sim_script;  /* --sim-script */
    const char *state_dir;   /* --state-dir, NULL none */
    double seconds;          /* --seconds: stop after, 0 never */
    bool verbose;
} rdofradio_trunk_opts_t;

/* --sim-air's whole run. Returns the exit code. */
int rdofradio_trunk_sim(const rdofradio_cfg_t *c, const rdofradio_trunk_opts_t *o, volatile sig_atomic_t *stop);

struct rdof_sdrmodem;
/* On a device: the site on modem m (keyed, its slot source), the network started. False with err set. */
bool rdofradio_trunk_start(const rdofradio_cfg_t *c, const rdofradio_trunk_opts_t *o, struct rdof_sdrmodem *m,
                           char *err, size_t cap);
bool rdofradio_trunk_on(void);
/* A burst heard on carrier c's inbound under slot k (k < 0: not placed, dropped). */
void rdofradio_trunk_heard(int c, int64_t k, const rdof_burst_t *b);
/* The main loop's: messages in, timers, the link. */
void rdofradio_trunk_poll(void);
void rdofradio_trunk_stop(void);

#endif
