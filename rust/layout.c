// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
/* rust's struct mirrors against the C headers (src/layout.rs): built with the Rust, into rdof_tests_rust and the
 * firmware (CONFIG_RDOF_RUST), which check rdof_rust_layout_mismatch() is NULL. */
#include <stdalign.h>
#include <stdbool.h>
#include <string.h>

#include "rdof/benchcfg.h"
#include "rdof/benchtx.h"
#include "rdof/config.h"
#include "rdof/duplex.h"
#include "rdof/features.h"
#include "rdof/hblink.h"
#include "rdof/jitter.h"
#include "rdof/oled.h"
#include "rdof/json.h"
#include "rdof/jsonp.h"
#include "rdof/mmdvm.h"
#include "rdof/modem.h"
#include "rdof/netcalls.h"
#include "rdof/pdu.h"
#include "rdof/rawburst.h"
#include "rdof/repeater.h"
#include "rdof/rust.h"
#include "rdof/slotgrid.h"
#include "rdof/status.h"
#include "rdof/tracker.h"
#include "rdof/txvr.h"
#include "rdof/usbmatch.h"

typedef struct {
    const char *name;
    size_t size, align;
} rdof_rust_layout_t;

const rdof_rust_layout_t *rdof_rust_layout(size_t i);

#define L(t) {#t, sizeof(t), alignof(t)}
static const rdof_rust_layout_t c_layouts[] = {
    L(rdof_csbk_t),
    L(rdof_location_t),
    L(rdof_net_burst_t),
    L(rdof_tune_t),
    L(rdof_frame_t),
    L(rdof_sink_t),
#if RDOF_FEATURE_TRACKER
    L(rdof_hb_sink_t),
#endif
    L(rdof_frame_buf_t),
    L(rdof_oled_status_t),
    L(rdof_usb_dev_t),
    L(rdof_usb_want_t),
    L(rdof_json_t),
    L(rdof_rawburst_t),
    L(rdof_full_lc_t),
    L(rdof_data_header_t),
    L(rdof_data_block_t),
    L(rdof_jnode_t),
    L(rdof_jdoc_t),
    L(rdof_tx_call_info_t),
    L(rdof_netcalls_t),
    L(rdof_roamer_status_t),
    L(rdof_repeater_status_t),
    L(rdof_modem_config_t),
    L(rdof_mmdvm_reader_t),
    L(rdof_mmdvm_status_t),
    L(rdof_slot_timing_t),
    L(rdof_slot_gate_t),
    L(rdof_sample_clock_t),
    L(rdof_bench_radio_t),
    L(rdof_benchtx_io_t),
    L(rdof_benchtx_item_t),
    L(rdof_benchtx_policy_t),
    L(rdof_benchtx_t),
    L(rdof_modem_port_t),
    L(rdof_modem_t),
    L(rdof_rssi_map_t),
    L(rdof_radio_t),
    L(rdof_burst_t),
    L(rdof_call_t),
    L(rdof_slot_state_t),
    L(rdof_tracker_t),
    L(rdof_receiver_t),
#if RDOF_FEATURE_TX
    L(rdof_channel_t),
    L(rdof_scan_t),
    L(rdof_txcall_t),
    L(rdof_txvr_t),
    L(rdof_jb_key_t),
    L(rdof_jb_t),
    L(rdof_jb_stream_t),
#endif
    L(rdof_cfg_channel_t),
    L(rdof_cfg_port_t),
    L(rdof_config_t),
    L(rdof_roles_t),
#if RDOF_FEATURE_REPEATER
    L(rdof_voice_regen_t),
    L(rdof_activity_t),
    L(rdof_rpt_out_t),
    L(rdof_voice_out_t),
    L(rdof_rpt_call_t),
    L(rdof_rpt_rf_t),
    L(rdof_rpt_slot_t),
    L(rdof_rpt_rf_in_t),
    L(rdof_repeater_t),
#endif
};

static bool is_repeater_struct(const char *name)
{
    static const char *const names[] = {"rdof_voice_regen_t", "rdof_activity_t", "rdof_rpt_out_t",  "rdof_voice_out_t",
                                        "rdof_rpt_call_t",    "rdof_rpt_rf_t",   "rdof_rpt_slot_t", "rdof_rpt_rf_in_t",
                                        "rdof_repeater_t"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcmp(name, names[i]))
            return true;
    return false;
}

const char *rdof_rust_layout_mismatch(void)
{
    size_t found = 0;
    for (size_t i = 0; rdof_rust_layout(i); i++) {
        const rdof_rust_layout_t *r = rdof_rust_layout(i);
        const rdof_rust_layout_t *c = NULL;
        for (size_t k = 0; k < sizeof c_layouts / sizeof c_layouts[0]; k++)
            if (!strcmp(c_layouts[k].name, r->name))
                c = &c_layouts[k];
        if (!c) {
            if (!RDOF_FEATURE_TRACKER && !strcmp(r->name, "rdof_hb_sink_t"))
                continue; /* not in this build's header */
            if (!RDOF_FEATURE_TX && (!strcmp(r->name, "rdof_channel_t") || !strcmp(r->name, "rdof_scan_t") ||
                                   !strcmp(r->name, "rdof_txcall_t") || !strcmp(r->name, "rdof_txvr_t") ||
                                   !strncmp(r->name, "rdof_jb_", 8)))
                continue;
            if (!RDOF_FEATURE_REPEATER && is_repeater_struct(r->name))
                continue;
            return r->name;
        }
        if (c->size != r->size || c->align != r->align)
            return r->name;
        found++;
    }
    return found == sizeof c_layouts / sizeof c_layouts[0] ? NULL : "a struct only layout.c lists";
}
