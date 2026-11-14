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
 * hears on carrier 1's inbound given its slot by timing (rdof_sdrmodem_inbound_k). Carriers 2 to 4 are sent by
 * the same SDR, or carrier 2 of two by a split TX half on its radio's port. Or every carrier, carrier 1's too, by its
 * own split TX half (the TSCC's taking the CACH with each burst), the SDR receiving only. On the SDR: an rdof_sdrmodem each with
 * its own slot source, on the same UTC slot grid and keyed with carrier 1, its I/Q summed into the one transmit stream
 * (rdofradio_t3_mix: each carrier at 1/N amplitude) at its own offset from the TX LO, which sits mid-way between the
 * outs, fixed for the run; what the receiver hears on its inbound placed by that modem's timing. A split TX half
 * (rdof/sdr/t3split.h): its slot counter aligned to UTC from the host's clock, the scheduled stream (TX_SCHED for
 * each slot), the SDR's receiver its RX half on carrier 2's inbound. The trunk's slot k is the UTC slot number
 * (floor(UTC ms / 30)); off the grid each modem's slots are moved onto it by an even offset taken at its first slot
 * (the carriers aren't slot-synchronized: site_sync 0, the payload warm-up). Up to four carriers out of the SDR, or
 * two with carrier 2 on a split TX half, or up to four each on a split TX half.
 *
 * --state-dir DIR keeps the provisioning (cfg-cache.json) across restarts.
 */
#ifndef RDOFRADIO_T3_H
#define RDOFRADIO_T3_H

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
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
    /* For a second carrier the SDR sends: hz's place from the TX LO (fixed for the run, between the carriers), false
     * past what the transmit stream reaches (*reach_hz, either side of the LO); the TX LO now (a bench job moves it);
     * the transmit stream's rate. */
    bool (*tx_offset)(uint32_t hz, double *off, double *reach_hz);
    double (*tx_lo)(void);
    uint32_t tx_fs;
} rdofradio_t3_dev_t;
/* On a device: the site on modem m (keyed, its slot source), carrier 2's modem on the SDR or its split TX half
 * brought up on its port (a two-carrier site), or every carrier's split TX half (m left unkeyed), the network started. Before the receiver starts: a split's bring-up
 * waits on the port. False with err set (carrier 2's out on another device, a carrier past the TX LO's reach). */
bool rdofradio_t3_start(const rdofradio_cfg_t *c, const rdofradio_t3_opts_t *o, struct rdof_sdrmodem *m,
                           const rdofradio_t3_dev_t *dev, char *err, size_t cap);
/* --tier3 FILE with --mmdvm: the site's one carrier on a stock duplex MMDVM modem (docs/spec/fw/tier3-hat.md,
 * rdof/t3hat.h). The modem keeps its own slot clock; the receiver hears the carrier's out (dev->channel on its tx) and
 * the bursts there give the modem's slot grid, and a writer thread polls GET_STATUS every 250 ms (the modem's
 * watchdog), keys the modem by a write and writes each trunk slot's burst a lead before its air time (lead_idle_s, or
 * lead_busy_s where the burst should set the AT of the CACH before it busy) and the short LC before the slot whose
 * CACH starts the modem's cycle. What the receiver hears on the carrier's in is placed on the grid by its position.
 * The modem is set up (duplex, DMR; no DMR_START) by the caller. */
typedef struct {
    bool (*write)(const uint8_t *frame, size_t n); /* a whole frame to the modem, from the writer thread */
    double lead_idle_s, lead_busy_s;
    bool mark; /* the host's idle bursts each told apart on air (rdof_duplex_idle_mark): for measuring */
    /* a lead sweep once placed (sweep_s > 0): both leads from sweep_from_s to sweep_to_s (up or down) by
     * sweep_step_s, each sweep_s long, no catch-up, both queues flushed at each step's start unless sweep_keep; a line
     * per step (where the bursts aired, the AT and short LC heard, the queues, the flushes) */
    double sweep_from_s, sweep_to_s, sweep_step_s, sweep_s;
    bool sweep_keep;
} rdofradio_t3_hat_t;
bool rdofradio_t3_start_hat(const rdofradio_cfg_t *c, const rdofradio_t3_opts_t *o, const rdofradio_t3_dev_t *dev,
                            const rdofradio_t3_hat_t *hat, char *err, size_t cap);
/* A bench job has the modem (true; nothing written, the grid dropped), or it's back (false: set up again first). */
void rdofradio_t3_hat_pause(bool pause);
/* What the modem sends while the site has it, from the port's reader: a GET_STATUS reply's payload (the modem's
 * protocol), a NAK of a command, a burst it heard on the inbound (its timeslot tag; at now). */
void rdofradio_t3_hat_status(int protocol, const uint8_t *p, size_t len);
void rdofradio_t3_hat_nak(uint8_t cmd, int err);
void rdofradio_t3_hat_modem_rx(int slot, const uint8_t burst[33], double now);
/* An outbound burst of carrier 1 heard (without a receiver: the simulated modem's; slc9 the short LC of the cycle its
 * CACH is in, frag which of its 4 CACH). */
void rdofradio_t3_hat_heard(int64_t pos48, int slot, int at, int lcss, const uint8_t burst[33], const uint8_t slc9[9],
                            int frag);
/* --mmdvm sim-hat: the stock modem simulated on a socket (ppm: its clock's error; fetch_s: how long before a slot's
 * air time it takes its burst, about 0.106); returns rdofradio's end, -1 with err set. What it hears is the air MSs'
 * inbound (--air-ms). What it sends goes to
 * rdofradio_t3_hat_heard, its slots' positions at 48 kS/s on CLOCK_MONOTONIC (the device's now48 without an SDR). */
int rdofradio_t3_hat_sim(double ppm, double fetch_s, char *err, size_t cap);
/* After rdofradio's end is closed. */
void rdofradio_t3_hat_sim_stop(void);
bool rdofradio_t3_on(void);
/* A burst the receiver heard on its channel c: carrier 1's inbound (the modem places it), carrier 2's (the split's
 * RX half), else nothing. */
void rdofradio_t3_heard(int c, const rdof_sdr_burst_t *b);
/* The main loop's: messages in, timers, the link. */
void rdofradio_t3_poll(void);
/* The main loop's, after carrier 1's pull (rdof_sdrmodem_pull_iq of the modem given to start, n samples from pos into
 * iq): the other carriers' samples for the same positions summed in, each carrier at 1/N amplitude of N
 * (rdof_sdrmodem_pull_mixw), when the SDR sends them; nothing otherwise, or while a bench job has moved the TX LO
 * (their clocks run on, unsent). Returns whether one of them is keyed there. */
bool rdofradio_t3_mix(int64_t pos, rdof_cf_t *iq, int n);
/* Carriers 2 to 4 for the status page: what sends them and their frequencies into out (empty: one carrier), their
 * bursts sent (traffic and control, not idle) and heard. */
void rdofradio_t3_carrier2(char *out, size_t cap, uint32_t *sent, uint32_t *heard);
void rdofradio_t3_stop(void);

#endif
