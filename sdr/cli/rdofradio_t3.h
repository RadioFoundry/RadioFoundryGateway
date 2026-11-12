// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * rdofradio's trunked site (docs/spec/fw/tier3.md): a settings file with a tier3 app runs it, the controller and the
 * radio layer in rdof/t3unit.h, on the MQTT network as the box's node (with libmosquitto; without it the site
 * runs alone). Built with RDOF_TIER3 (cargo found: the controller is rust's t3 feature).
 *
 * --sim-air N: no device. N simulated MSs (rdof/t3sim.h) on the site's air, the full burst path each slot, the slot
 * clock on the wall clock (30 ms a slot), the network real. Each registers at start (staggered, with --sim-tg's
 * talkgroup); --sim-talk S makes one of them call the talkgroup every S seconds; --sim-script FILE adds timed actions,
 * one a line ("#" comments):
 *     AT_S  ID  register [TG] | deregister | group TG [TALK_MS] | private ID2 [TALK_MS] | cancel
 * An ID not among the N is a simulated MS of its own, idle until the script makes it act.
 * --sim-ext PATH adds external MSs (other programs) on a unix socket, in lockstep with the slots: rdofradio_simext.h.
 * With it N may be 0.
 *
 * On a device: the SDR is carrier 1 (the TSCC's): rdof_sdrmodem's slot source, keyed with no end (hold_key), what it
 * hears on carrier 1's inbound given its slot by timing (rdof_sdrmodem_inbound_k). A second carrier is a split TX
 * half on its radio's port (rdof/sdr/t3split.h): its slot counter aligned to UTC from the host's clock, the
 * scheduled stream (TX_SCHED for each slot), the SDR's receiver its RX half on carrier 2's inbound. The trunk's slot
 * k is the UTC slot number (floor(UTC ms / 30)); carrier 1's modem slots are moved onto it by an even offset taken
 * at its first slot (the carriers aren't slot-synchronized: site_sync 0, the payload warm-up). Two carriers at most.
 *
 * --state-dir DIR keeps the provisioning (cfg-cache.json) across restarts.
 */
#ifndef RDOFRADIO_T3_H
#define RDOFRADIO_T3_H

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>

#include "rdof/layer2.h"
#include "rdof/sdr/demod.h"
#include "rdofradio_cfg.h"

typedef struct {
    int sim_n;               /* --sim-air: the MSs (0: on a device) */
    int air_ms;              /* --air-ms: on a device, simulated MSs on carrier 1 beside the real radios */
    uint32_t sim_id;         /* the first one's ID (--sim-id; the rest follow) */
    uint32_t sim_tg;         /* --sim-tg */
    double sim_talk_s;       /* --sim-talk: a group call every this many seconds, 0 none */
    const char *sim_script;  /* --sim-script */
    const char *sim_ext;     /* --sim-ext: the external MSs' socket, NULL none */
    double sim_ext_loss;     /* --sim-ext-loss: an external burst lost, probability */
    double sim_ext_err;      /* --sim-ext-err: an external burst corrupted, probability */
    bool sim_fast;           /* --sim-fast: with --sim-ext, slots as fast as the clients answer */
    const char *state_dir;   /* --state-dir, NULL none */
    double seconds;          /* --seconds: stop after, 0 never */
    bool verbose;
} rdofradio_t3_opts_t;

/* --sim-air's whole run. Returns the exit code. */
int rdofradio_t3_sim(const rdofradio_cfg_t *c, const rdofradio_t3_opts_t *o, volatile sig_atomic_t *stop);

struct rdof_sdrmodem;
/* What the device side needs from rdofradio's receiver. */
typedef struct {
    int (*channel)(uint32_t hz); /* the receiver's channel on hz, -1 none */
    int64_t (*now48)(void);      /* the receiver's 48 kS/s position now (the positions its bursts carry) */
    double (*now)(void);         /* the host's monotonic clock, s */
    uint32_t rx_fs;              /* the receiver's rate, S/s */
} rdofradio_t3_dev_t;
/* On a device: the site on modem m (keyed, its slot source), carrier 2's split TX half brought up on its port (a
 * two-carrier site), the network started. Before the receiver starts: the bring-up waits on the port. False with err
 * set. */
bool rdofradio_t3_start(const rdofradio_cfg_t *c, const rdofradio_t3_opts_t *o, struct rdof_sdrmodem *m,
                           const rdofradio_t3_dev_t *dev, char *err, size_t cap);
bool rdofradio_t3_on(void);
/* A burst the receiver heard on its channel c: carrier 1's inbound (the modem places it), carrier 2's (the split's
 * RX half), else nothing. */
void rdofradio_t3_heard(int c, const rdof_sdr_burst_t *b);
/* The main loop's: messages in, timers, the link. */
void rdofradio_t3_poll(void);
void rdofradio_t3_stop(void);

#endif
