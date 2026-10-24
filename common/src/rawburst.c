// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* One burst for .../raw: the binary encoder and the MQTTRaw JSON (rdof/rawburst.h). */
#include "rdof/rawburst.h"

#include <stdio.h>
#include <string.h>

#include "rdof/json.h"
#include "rdof/pdu.h"
#include "rdof/tracker.h"

const char *rdof_raw_type_name(int dt, char tmp[12])
{
    switch (dt) {
    case RDOF_DT_PI_HEADER: return "voice_pi_header";
    case RDOF_DT_VOICE_LC_HEADER: return "voice_lc_header";
    case RDOF_DT_TERMINATOR_WITH_LC: return "terminator";
    case RDOF_DT_CSBK: return "csbk";
    case RDOF_DT_MBC_HEADER: return "mbc_header";
    case RDOF_DT_MBC_CONTINUATION: return "mbc_continuation";
    case RDOF_DT_DATA_HEADER: return "data_header";
    case RDOF_DT_RATE_12: return "rate_1_2";
    case RDOF_DT_RATE_34: return "rate_3_4";
    case RDOF_DT_RATE_1: return "rate_1";
    case RDOF_DT_USBD: return "usbd";
    case RDOF_DT_VOICE_SYNC: return "voice_sync";
    case RDOF_DT_VOICE: return "voice";
    default: snprintf(tmp, 12, "%d", dt); return tmp;
    }
}

static void le(uint8_t *p, uint64_t v, int n)
{
    for (int i = 0; i < n; i++, v >>= 8)
        p[i] = (uint8_t)v;
}

size_t rdof_rawburst_encode(const rdof_rawburst_t *r, uint8_t out[RDOF_RAWBURST_MAX])
{
    out[0] = r->utc ? RDOF_RAWBURST_VERSION_UTC : RDOF_RAWBURST_VERSION;
    out[1] = (uint8_t)((r->slot == 2 ? RDOF_RAWBURST_SLOT2 : 0) | (r->network ? RDOF_RAWBURST_NETWORK : 0) |
                       (r->addressed ? RDOF_RAWBURST_ADDRESSED | (r->group ? RDOF_RAWBURST_GROUP : 0) : 0));
    out[2] = (uint8_t)r->dt;
    out[3] = (uint8_t)r->n;
    le(out + 4, r->ms > 0 ? (uint64_t)r->ms : 0, 6);
    le(out + 10, r->addressed ? r->src : 0, 3);
    le(out + 13, r->addressed ? r->dst : 0, 3);
    memcpy(out + 16, r->burst, RDOF_BURST_BYTES);
    if (!r->utc)
        return RDOF_RAWBURST_BYTES;
    le(out + 49, (uint64_t)r->utc_us, 8);
    le(out + 57, (uint64_t)r->utc_slot, 5);
    return RDOF_RAWBURST_BYTES_UTC;
}

bool rdof_rawburst_json(const rdof_rawburst_t *r, char *out, size_t cap)
{
    char ts[32], tmp[12], b64[48];
    rdof_json_t j;
    rdof_iso_ms(r->ms, ts);
    rdof_base64(r->burst, RDOF_BURST_BYTES, b64);
    rdof_json_init(&j, out, cap);
    rdof_json_open(&j, NULL);
    rdof_json_open(&j, "DMR");
    rdof_json_str(&j, "timestamp", ts);
    rdof_json_int(&j, "slot", r->slot);
    rdof_json_str(&j, "source", r->network ? "network" : "rf");
    rdof_json_str(&j, "type", rdof_raw_type_name(r->dt, tmp));
    rdof_json_int(&j, "dt", r->dt);
    rdof_json_int(&j, "n", r->n);
    rdof_json_str(&j, "data", b64);
    if (r->addressed) {
        rdof_json_int(&j, "src_id", r->src);
        rdof_json_int(&j, "dst_id", r->dst);
        rdof_json_bool(&j, "group", r->group);
    }
    if (r->utc) {
        rdof_json_int(&j, "utc_slot", r->utc_slot);
        rdof_json_int(&j, "utc_us", r->utc_us);
    }
    rdof_json_close(&j);
    rdof_json_close(&j);
    return rdof_json_done(&j);
}
