// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* What a roamer or repeater transmits, as network calls (rdof/netcalls.h). */
#include "rdof/netcalls.h"

#include <stdio.h>
#include <string.h>

#include "rdof/json.h"
#include "rdof/mmdvm.h"
#include "rdof/pdu.h"
#include "rdof/tracker.h"

void rdof_netcalls_init(rdof_netcalls_t *n)
{
    memset(n, 0, sizeof *n);
}

void rdof_csbk_desc(const uint8_t burst[RDOF_BURST_BYTES], char *out, size_t cap)
{
    rdof_csbk_t c;
    rdof_decode_csbk(burst, &c);
    const char *d = NULL;
    switch (c.opcode) {
    case 0x04: d = "Unit to Unit Voice Service Request"; break;
    case 0x05: d = "Unit to Unit Voice Answer Response"; break;
    case 0x26: d = "Negative Acknowledgment Response"; break;
    case 0x3D: d = "Preamble"; break;
    case 0x1F: d = "Call Alert"; break;
    case 0x20: d = "Call Alert Ack"; break;
    case 0x24: d = c.raw[3] == 0x80 ? "Radio Check" : "Radio Check Ack"; break;
    case 0x27: d = "Call Emergency"; break;
    }
    if (d)
        snprintf(out, cap, "%s", d);
    else
        snprintf(out, cap, "CSBK 0x%02X", c.opcode);
}

static void freq_text(uint32_t f, char *out, size_t cap)
{
    snprintf(out, cap, "%.5f", f / 1e6);
}

bool rdof_netcalls_event(rdof_netcalls_t *n, const char *action, const rdof_tx_call_info_t *call, double duration,
                       double wall_t, char *out, size_t cap)
{
    char ts[32], info[16], freq[24], desc[48];
    rdof_json_t j;
    int slot = call->slot >= 1 && call->slot <= 2 ? call->slot : 2;
    if (call->has_csbk && strcmp(action, "start") != 0)
        return false; /* a lone CSBK is reported once, whole; out untouched */
    rdof_iso_time(wall_t, ts);
    rdof_json_init(&j, out, cap);
    rdof_json_open(&j, NULL);
    rdof_json_open(&j, "DMR");
    if (call->has_csbk) {
        rdof_csbk_desc(call->csbk, desc, sizeof desc);
        snprintf(info, sizeof info, "%lu", (unsigned long)call->src);
        freq_text(call->freq, freq, sizeof freq);
        rdof_json_str(&j, "timestamp", ts);
        rdof_json_str(&j, "source", "network");
        rdof_json_str(&j, "action", "csbk");
        rdof_json_int(&j, "slot", slot);
        rdof_json_int(&j, "src_id", call->src);
        rdof_json_int(&j, "dst_id", call->dst);
        rdof_json_str(&j, "group", call->group ? "yes" : "no");
        rdof_json_str(&j, "src_info", info);
        rdof_json_str(&j, "csbk_desc", desc);
        rdof_json_str(&j, "frequency", freq);
        rdof_json_int(&j, "colorCode", call->cc);
    } else {
        rdof_json_str(&j, "timestamp", ts);
        rdof_json_str(&j, "source", "network");
        rdof_json_str(&j, "action", action);
        rdof_json_int(&j, "slot", slot);
        if (strcmp(action, "start") == 0) {
            n->has_last[slot] = false;
            snprintf(info, sizeof info, "%lu", (unsigned long)call->src);
            freq_text(call->freq, freq, sizeof freq);
            rdof_json_int(&j, "src_id", call->src);
            rdof_json_int(&j, "dst_id", call->dst);
            rdof_json_str(&j, "group", call->group ? "yes" : "no");
            rdof_json_str(&j, "src_info", info);
            rdof_json_str(&j, "frequency", freq);
            rdof_json_int(&j, "colorCode", call->cc);
            if (call->frames >= 0)
                rdof_json_int(&j, "frames", call->frames);
        } else {
            rdof_json_float3(&j, "duration", duration);
        }
    }
    rdof_json_close(&j);
    rdof_json_close(&j);
    return rdof_json_done(&j);
}

bool rdof_netcalls_burst_raw(rdof_netcalls_t *n, const rdof_tx_call_info_t *call, uint8_t control,
                           const uint8_t burst[RDOF_BURST_BYTES], double wall_t, rdof_rawburst_t *out)
{
    if (call->has_csbk)
        return false; /* reported whole by the event */
    int slot = call->slot >= 1 && call->slot <= 2 ? call->slot : 2;
    int dt, nn;
    rdof_control_to_dt(control, &dt, &nn);
    if ((control & RDOF_SYNC_DATA) && n->has_last[slot] && n->last_control[slot] == control &&
        !memcmp(n->last_burst[slot], burst, RDOF_BURST_BYTES))
        return false; /* a repeated header or terminator */
    n->has_last[slot] = true;
    n->last_control[slot] = control;
    memcpy(n->last_burst[slot], burst, RDOF_BURST_BYTES);
    *out = (rdof_rawburst_t){.ms = rdof_time_ms(wall_t), .slot = slot, .network = true, .dt = dt, .n = nn,
                           .addressed = true, .src = call->src, .dst = call->dst, .group = call->group};
    memcpy(out->burst, burst, RDOF_BURST_BYTES);
    return true;
}

bool rdof_netcalls_burst(rdof_netcalls_t *n, const rdof_tx_call_info_t *call, uint8_t control,
                       const uint8_t burst[RDOF_BURST_BYTES], double wall_t, char *out, size_t cap)
{
    rdof_rawburst_t r;
    return rdof_netcalls_burst_raw(n, call, control, burst, wall_t, &r) && rdof_rawburst_json(&r, out, cap);
}

void rdof_tx_json(const char *action, const rdof_tx_call_info_t *call, double duration, double wall_t, char *out,
                size_t cap)
{
    char ts[32], freq[24];
    rdof_json_t j;
    rdof_iso_time(wall_t, ts);
    freq_text(call->freq, freq, sizeof freq);
    rdof_json_init(&j, out, cap);
    rdof_json_open(&j, NULL);
    rdof_json_open(&j, "TX");
    rdof_json_str(&j, "timestamp", ts);
    rdof_json_str(&j, "action", action);
    rdof_json_int(&j, "slot", call->slot);
    rdof_json_int(&j, "src_id", call->src);
    rdof_json_int(&j, "dst_id", call->dst);
    rdof_json_str(&j, "group", call->group ? "yes" : "no");
    rdof_json_str(&j, "frequency", freq);
    rdof_json_int(&j, "colorCode", call->cc);
    if (strcmp(action, "end") == 0)
        rdof_json_float3(&j, "duration", duration);
    rdof_json_close(&j);
    rdof_json_close(&j);
}
