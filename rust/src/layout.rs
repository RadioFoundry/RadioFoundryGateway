// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! The structs this crate shares with C, by C name, with their size and alignment. rust/layout.c checks them
//! against the C headers' (rdof_tests_rust, and the firmware at boot): a mirror that drifts from its header would
//! corrupt memory without a word. A module that mirrors a struct adds it here and to layout.c's table.

use core::ffi::c_char;
use core::mem::{align_of, size_of};

use crate::benchcfg::RdofBenchRadio;
use crate::benchtx::{RdofBenchtx, RdofBenchtxIo, RdofBenchtxItem, RdofBenchtxPolicy};
use crate::config::{RdofCfgChannel, RdofCfgPort, RdofConfig, RdofRoles};
use crate::hblink::{RdofHbSink, RdofLocation, RdofNetBurst, RdofTune};
use crate::jitter::{RdofJb, RdofJbKey, RdofJbStream};
use crate::json::RdofJson;
use crate::jsonp::{JDoc, JNode};
use crate::mmdvm::{RdofMmdvmReader, RdofMmdvmStatus, RdofModemConfig};
use crate::modem::{RdofModem, RdofModemPort, RdofRadio, RdofRssiMap};
use crate::netcalls::{RdofNetcalls, RdofTxCallInfo};
use crate::oled::{RdofFrameBuf, RdofOledStatus};
use crate::pdu::{RdofCsbk, RdofDataBlock, RdofDataHeader, RdofFullLc};
use crate::rawburst::RdofRawburst;
use crate::repeater::{
    RdofActivity, RdofRepeater, RdofRptCall, RdofRptOut, RdofRptRf, RdofRptRfIn, RdofRptSlot, RdofVoiceOut, RdofVoiceRegen,
};
use crate::slotgrid::{RdofSampleClock, RdofSlotGate, RdofSlotTiming};
use crate::status::{RdofRepeaterStatus, RdofRoamerStatus};
use crate::tracker::{RdofBurst, RdofCall, RdofFrame, RdofSink, RdofSlotState, RdofTracker};
use crate::txvr::{RdofChannel, RdofReceiver, RdofScan, RdofTxcall, RdofTxvr};
use crate::usbmatch::{RdofUsbDev, RdofUsbWant};

#[repr(C)]
pub struct Layout {
    name: *const c_char,
    size: usize,
    align: usize,
}

// Only pointers to string literals.
unsafe impl Sync for Layout {}

macro_rules! layouts {
    ($(($name:literal, $t:ty)),* $(,)?) => {
        [$(Layout { name: concat!($name, "\0").as_ptr().cast(), size: size_of::<$t>(), align: align_of::<$t>() }),*]
    };
}

static LAYOUTS: &[Layout] = &layouts![
    ("rdof_csbk_t", RdofCsbk),
    ("rdof_location_t", RdofLocation),
    ("rdof_net_burst_t", RdofNetBurst),
    ("rdof_tune_t", RdofTune),
    ("rdof_frame_t", RdofFrame),
    ("rdof_sink_t", RdofSink),
    ("rdof_hb_sink_t", RdofHbSink),
    ("rdof_frame_buf_t", RdofFrameBuf),
    ("rdof_oled_status_t", RdofOledStatus),
    ("rdof_usb_dev_t", RdofUsbDev),
    ("rdof_usb_want_t", RdofUsbWant),
    ("rdof_json_t", RdofJson),
    ("rdof_rawburst_t", RdofRawburst),
    ("rdof_full_lc_t", RdofFullLc),
    ("rdof_data_header_t", RdofDataHeader),
    ("rdof_data_block_t", RdofDataBlock),
    ("rdof_jnode_t", JNode),
    ("rdof_jdoc_t", JDoc),
    ("rdof_tx_call_info_t", RdofTxCallInfo),
    ("rdof_netcalls_t", RdofNetcalls),
    ("rdof_roamer_status_t", RdofRoamerStatus),
    ("rdof_repeater_status_t", RdofRepeaterStatus),
    ("rdof_modem_config_t", RdofModemConfig),
    ("rdof_mmdvm_reader_t", RdofMmdvmReader),
    ("rdof_mmdvm_status_t", RdofMmdvmStatus),
    ("rdof_slot_timing_t", RdofSlotTiming),
    ("rdof_slot_gate_t", RdofSlotGate),
    ("rdof_sample_clock_t", RdofSampleClock),
    ("rdof_bench_radio_t", RdofBenchRadio),
    ("rdof_benchtx_io_t", RdofBenchtxIo),
    ("rdof_benchtx_item_t", RdofBenchtxItem),
    ("rdof_benchtx_policy_t", RdofBenchtxPolicy),
    ("rdof_benchtx_t", RdofBenchtx),
    ("rdof_modem_port_t", RdofModemPort),
    ("rdof_modem_t", RdofModem),
    ("rdof_rssi_map_t", RdofRssiMap),
    ("rdof_radio_t", RdofRadio),
    ("rdof_burst_t", RdofBurst),
    ("rdof_call_t", RdofCall),
    ("rdof_slot_state_t", RdofSlotState),
    ("rdof_tracker_t", RdofTracker),
    ("rdof_receiver_t", RdofReceiver),
    ("rdof_channel_t", RdofChannel),
    ("rdof_scan_t", RdofScan),
    ("rdof_txcall_t", RdofTxcall),
    ("rdof_txvr_t", RdofTxvr),
    ("rdof_jb_key_t", RdofJbKey),
    ("rdof_jb_t", RdofJb),
    ("rdof_jb_stream_t", RdofJbStream),
    ("rdof_cfg_channel_t", RdofCfgChannel),
    ("rdof_cfg_port_t", RdofCfgPort),
    ("rdof_config_t", RdofConfig),
    ("rdof_roles_t", RdofRoles),
    ("rdof_voice_regen_t", RdofVoiceRegen),
    ("rdof_activity_t", RdofActivity),
    ("rdof_rpt_out_t", RdofRptOut),
    ("rdof_voice_out_t", RdofVoiceOut),
    ("rdof_rpt_call_t", RdofRptCall),
    ("rdof_rpt_rf_t", RdofRptRf),
    ("rdof_rpt_slot_t", RdofRptSlot),
    ("rdof_rpt_rf_in_t", RdofRptRfIn),
    ("rdof_repeater_t", RdofRepeater),
];

/// The i-th struct, or NULL past the last.
#[no_mangle]
pub extern "C" fn rdof_rust_layout(i: usize) -> Option<&'static Layout> {
    LAYOUTS.get(i)
}
