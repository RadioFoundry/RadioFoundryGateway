// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * rdofradio --sim-ext PATH: external MSs on a --sim-air site's air (docs/spec/fw/trunk.md 9.4). A unix stream socket
 * at PATH; each client is one or more MSs of its own (another program: RadioFoundry's MS engine built for the host,
 * say). JSON lines both ways, in lockstep with the site's slot clock:
 *
 *   site -> client, after each slot k (once the client has joined):
 *     {"k":K,"t_ms":T,"at_next":0|1,"out":[CARRIER...],"in":[FATE...]}
 *     CARRIER: {"c":C,"ch":CH,"slot":1|2,"aired":true|false,"burst":HEX66,"dt":DT,"cc":CC,"pdu":HEX20|null,
 *               "cach":HEX6,"at":0|1,"tc":1|2,"lcss":L,"slc17":N}
 *       c the carrier (0 = the TSCC's), ch = 2 c + slot - 1, burst the 33 bytes aired, dt / cc from its slot type (-1
 *       voice or unreadable), pdu a control block whose CRC passed, cach the CACH before the burst with its fields
 *       read back. at_next: the AT bit of the CACH after slot k on carrier 0 (the TSCC's; an MS answers in its next
 *       inbound slot).
 *     FATE: {"k":K,"ch":CH,"id":ID,"fate":"heard"|"collided"|"no-slot"|"timing"|"loss"|"corrupted"|"late"|"bad"}
 *       what became of each burst the client sent, reported after the slot it was for.
 *   client -> site, an answer to every slot k it was sent (the site waits up to 2 s, then drops the client):
 *     {"k":K,"tx":[TX...]}             ("hello":NAME in any line names the client in the log)
 *     TX: {"c":C,"slot":1|2 | "ch":CH, "k":TARGET, "id":ID, "offset_us":US, "cc":CC, and one of
 *            "dt":DT,"pdu":HEX20                                   a control block (CRC added)
 *            "voice":"header"|"term","src":ID,"dst":ID,"group":true  a voice LC header / terminator
 *            "voice":"frame","n":0..5                                voice burst A..F (silence)
 *            "burst":HEX66[,"dt":DT,"n":N]                           the burst as given}
 *       TARGET: the inbound slot it goes in (default: the next slot of that timeslot after K). offset_us: the
 *       burst's timing error, logged; past half a slot (15000 us) it misses the slot. A burst on a channel not on air
 *       in its slot (the other timeslot, no such carrier) is lost. Two in one channel and slot (from any MSs,
 *       internal or external) collide.
 *
 * Every inbound burst gets a log line (voice bursts B-F only with -v). Without --sim-fast the slots follow the wall
 * clock (30 ms each) and fall behind while clients answer slowly; --sim-fast runs them as fast as the clients answer
 * (a site alone: its network clock runs ahead). --sim-ext-loss P / --sim-ext-err P lose / corrupt each external
 * inbound burst with probability P.
 */
#ifndef RDOFRADIO_SIMEXT_H
#define RDOFRADIO_SIMEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdof/trunksim.h"

/* Listen on path (an old socket file there is replaced), hooks set on sim. False with err set. */
bool rdofradio_simext_open(rdof_trunksim_t *sim, const char *path, double loss, double err_rate, uint32_t seed,
                           bool verbose, char *err, size_t cap);
/* After slot k ran: take new clients, wait (up to 2 s) for each joined client's answer to slot k. */
void rdofradio_simext_wait(int64_t k);
/* Clients joined now. */
int rdofradio_simext_clients(void);
void rdofradio_simext_close(void);

#endif
