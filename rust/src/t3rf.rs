// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! A trunked site's radio layer (rdof/t3rf.h; docs/spec/fw/tier3.md section 9): the bursts and CACH of a Tier III
//! site's carriers, its payload channels, and the routing of what its receivers hear. The C header is the contract;
//! the structs below are its types field for field (checked by rdof_t3rf_layout against the header's sizes).

use core::ffi::{c_int, c_void};

use crate::repeater::RdofVoiceRegen;
use crate::tracker::{RdofBurst, RdofFrame};

pub const CARRIERS: usize = 4;
pub const CHANNELS: usize = 2 * CARRIERS;
pub const QUEUE: usize = 24;
pub const NET_PREBUFFER: usize = 4;
pub const WARMUP: c_int = 2;
pub const LOST_K: i64 = 17;
pub const BURST_BYTES: usize = 33;

/// rdof_trunk_pdu_t
pub const NOT_PDU: c_int = 0;
pub const PDU_OK: c_int = 1;
pub const PDU_BAD: c_int = 2;

/// rdof_t3rf_io_t's activity
pub const HEADER: c_int = 1;
pub const VOICE: c_int = 2;
pub const TERM: c_int = 3;
pub const LOST: c_int = 4;

/// rdof_t3rf_out_t
pub const OUT_IDLE: c_int = 0;
pub const OUT_TRAFFIC: c_int = 1;
pub const OUT_CONTROL: c_int = 2;

/// rdof_t3rf_burst_t
#[repr(C)]
#[derive(Clone, Copy)]
pub struct T3rfBurst {
    pub control: u8,
    pub net: bool,
    pub b: [u8; BURST_BYTES],
}

/// rdof_t3rf_ch_t
#[repr(C)]
pub struct T3rfCh {
    pub index: c_int,
    pub cc: c_int,
    pub open: bool,
    pub src: u32,
    pub dst: u32,
    pub group: bool,
    pub warmup: c_int,
    pub q: [T3rfBurst; QUEUE],
    pub qh: c_int,
    pub qn: c_int,
    pub ctl: [u8; BURST_BYTES],
    pub ctl_left: c_int,
    pub close_after: bool,
    pub rf: bool,
    pub rf_last: i64,
    pub rf_regen_on: bool,
    pub rf_new: bool,
    pub rf_regen: RdofVoiceRegen,
    pub rf_src: u32,
    pub rf_dst: u32,
    pub rf_group: bool,
    pub net: bool,
    pub net_started: bool,
    pub net_regen_on: bool,
    pub net_held: c_int,
    pub net_regen: RdofVoiceRegen,
    pub rx_bursts: u32,
    pub rx_pdus: u32,
    pub tx_traffic: u32,
    pub tx_idle: u32,
    pub tx_control: u32,
    pub net_bursts: u32,
    pub net_dropped: u32,
    pub overflows: u32,
    pub lost: u32,
}

/// rdof_t3rf_io_t
#[repr(C)]
#[derive(Clone, Copy)]
pub struct T3rfIo {
    pub tscc: Option<unsafe extern "C" fn(ctx: *mut c_void, k: i64, dt: *mut c_int, pdu: *mut u8) -> bool>,
    pub cach: Option<unsafe extern "C" fn(ctx: *mut c_void, carrier: c_int, k: i64, at: *mut c_int, slc28: *mut i32)>,
    pub pdu: Option<unsafe extern "C" fn(ctx: *mut c_void, ch: c_int, k: i64, dt: c_int, pdu: *const u8, rssi: c_int)>,
    pub bad: Option<unsafe extern "C" fn(ctx: *mut c_void, ch: c_int, k: i64)>,
    pub activity: Option<
        unsafe extern "C" fn(ctx: *mut c_void, ch: c_int, k: i64, what: c_int, src: u32, dst: u32, group: bool),
    >,
    pub uplink: Option<unsafe extern "C" fn(ctx: *mut c_void, ch: c_int, f: *const RdofFrame)>,
    pub ctx: *mut c_void,
}

/// rdof_t3rf_t
#[repr(C)]
pub struct T3rf {
    pub io: T3rfIo,
    pub ncarriers: c_int,
    pub cc: c_int,
    pub tscc: bool,
    pub ch: [T3rfCh; CHANNELS],
    pub tscc_sent: u32,
    pub tscc_idle: u32,
    pub rx_tscc: u32,
    pub rx_tscc_bad: u32,
    pub rx_ignored: u32,
}

/// rdof_t3rf_split_t
#[repr(C)]
#[derive(Clone, Copy)]
pub struct T3rfSplit {
    pub carrier: c_int,
    pub keyed: bool,
    pub next_k: i64,
    pub slc_sent: i32,
    pub scheduled: bool,
    pub sched_bursts: u32,
    pub fifo_bursts: u32,
    pub short_lcs: u32,
    pub keyups: u32,
}

// Kept so the type is used before the port fills this module in.
#[allow(dead_code)]
type Heard = RdofBurst;
