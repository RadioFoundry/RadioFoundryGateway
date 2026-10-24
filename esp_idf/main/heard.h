// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/*
 * Last heard (heard.c): the board's recent calls, RF and NET, for /api/lastheard. The tasks that hear or send
 * calls feed it; the web server reads it. role: "sdr", "roamer" or "repeater".
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "rdof/netcalls.h"
#include "rdof/pdu.h"

/* A tracker's JSON line (rdof/tracker.h): call starts and ends, CSBKs, BER and RSSI. channel: "" for a radio. */
void heard_rf_json(const char *role, const char *radio, const char *channel, const char *json);
/* A burst's data type on a slot: a PI header marks the call there encrypted. Cheap for any other type. */
void heard_burst(const char *radio, const char *channel, int slot, int dt);
/* A call sent out over RF from the network (on_tx's start and end). */
void heard_tx(const char *role, const char *radio, const char *action, const rdof_tx_call_info_t *call,
              double duration);
/* The calls changed after seq `since`, as JSON (rdof_lh_json). */
size_t heard_json(uint32_t since, char *out, size_t cap);
