// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! The version 3 plan (rdof/siteplan.h, rdof/site.h), which stays C: its structs as config.rs and benchcfg.rs read
//! them, and the accessors they call. The mirrors are whole (layout.rs checks their size), so some fields aren't
//! read here.
#![allow(dead_code)]

use core::ffi::{c_char, c_int, c_long};

use crate::jsonp::JDoc;

pub const SITE_NAME: usize = 40; // RDOF_SITE_NAME
const MAX_FREQS: usize = 48; // RDOF_SITE_MAX_FREQS
const MAX_DEVICES: usize = 16;
const MAX_APPS: usize = 16;
const MAX_NETS: usize = 4;
const MAX_POLICIES: usize = 16;
const MAX_PATHS: usize = 64;
const IDSET_MAX: usize = 12; // RDOF_IDSET_MAX
const PK_N: usize = 7; // RDOF_PK_N

pub const HEARS_SCAN: u8 = 2; // rdof_hears_t
pub const APP_PARROT: c_int = 2; // rdof_app_type_t
pub const NET_HBLINK: c_int = 1; // rdof_net_type_t
pub const NET_MQTT: c_int = 2;
pub const DEV_SDR: c_int = 1; // rdof_dev_kind_t
pub const DEV_RADIO: c_int = 3;
pub const PORT_UART: c_int = 1; // rdof_port_kind_t
pub const PORT_PATH: c_int = 3;
pub const UNIT_REPEATER: u8 = 1; // rdof_unit_kind_t
pub const UNIT_SFR: u8 = 2;
pub const UNIT_DCDM: u8 = 3;
pub const UNIT_TRANSCEIVER: u8 = 4;
pub const UNIT_RECEIVER: u8 = 5;

/// rdof_idrange_t
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RdofIdrange {
    pub lo: u32,
    pub hi: u32,
}

/// rdof_idset_t
#[repr(C)]
pub struct RdofIdset {
    pub star: bool,
    pub nin: u8,
    pub nout: u8,
    pub r: [RdofIdrange; IDSET_MAX],
}

/// rdof_policy_t
#[repr(C)]
pub struct RdofPolicy {
    pub tg: RdofIdset,
    pub ptt: RdofIdset,
    pub pc: RdofIdset,
    pub ptt_hold_s: f64,
    pub apps: u32,
    pub net: c_int,
    pub dir: u8,
    pub src: [i8; PK_N],
}

/// rdof_freq_t
#[repr(C)]
pub struct RdofFreq {
    pub name: [u8; SITE_NAME],
    pub hz: u32,
    pub cc: c_int,
    pub mode: u8,
    pub ts: u8,
    pub tx: bool,
    pub power_dbm: f64,
    pub devices: u32,
    pub app: c_int,
}

/// rdof_device3_t
#[repr(C)]
pub struct RdofDevice3 {
    pub name: [u8; SITE_NAME],
    pub kind: c_int,
    pub driver: [u8; 16],
    pub hears: u8,
    pub idle: c_int,
    pub scan: u64,
    pub can_tx: bool,
    pub transmits: u64,
}

/// rdof_app3_t
#[repr(C)]
pub struct RdofApp3 {
    pub name: [u8; SITE_NAME],
    pub type_: u8,
    pub in_: c_int,
    pub out: c_int,
    pub path: [c_int; 2],
    pub tg: RdofIdset,
    pub pc: RdofIdset,
}

/// rdof_net3_t
#[repr(C)]
pub struct RdofNet3 {
    pub name: [u8; SITE_NAME],
    pub type_: u8,
    pub per_path: bool,
    pub roaming: bool,
    pub report: bool,
    pub calls: bool,
    pub format: u8,
    pub bundle: u8,
    pub raw_tx: bool,
    pub transport: u8,
}

/// rdof_path_t
#[repr(C)]
pub struct RdofPath {
    pub name: [u8; SITE_NAME + 4],
    pub freq: c_int,
    pub tx_freq: c_int,
    pub app: c_int,
    pub slot: u8,
    pub tx_slot: u8,
    pub peer_id: u32,
    pub peer_name: [u8; 9],
    pub pol: RdofPolicy,
}

/// rdof_site_t
#[repr(C)]
pub struct RdofSite {
    pub name: [u8; 32],
    pub id: u32,
    pub pc_hold_s: f64,
    pub nfreq: c_int,
    pub ndev: c_int,
    pub napp: c_int,
    pub nnet: c_int,
    pub npol: c_int,
    pub npath: c_int,
    pub freq: [RdofFreq; MAX_FREQS],
    pub dev: [RdofDevice3; MAX_DEVICES],
    pub app: [RdofApp3; MAX_APPS],
    pub net: [RdofNet3; MAX_NETS],
    pub pol_name: [[u8; SITE_NAME]; MAX_POLICIES],
    pub path: [RdofPath; MAX_PATHS],
}

/// rdof_port_t
#[repr(C)]
pub struct RdofPort {
    pub kind: c_int,
    pub uart: c_int,
    pub tx: c_int,
    pub rx: c_int,
    pub rts: c_int,
    pub cts: c_int,
    pub baud: c_long,
    pub usb: [u8; 8],
    pub vid: u16,
    pub pid: u16,
    pub serial: [u8; 32],
    pub usb_port: [u8; 16],
    pub path: [u8; 64],
}

/// rdof_dev_bench_t
#[repr(C)]
pub struct RdofDevBench {
    pub on: bool,
    pub nfreqs: c_int,
    pub freqs_hz: [u32; 8],
    pub max_key_s: f64,
}

/// rdof_plan_dev_t
#[repr(C)]
pub struct RdofPlanDev {
    pub port: RdofPort,
    pub needs: u32,
    pub tx: bool,
    pub tx_carriers: c_int,
    pub tx_power_dbm: f64,
    pub has_tx_power: bool,
    pub txmap: RdofTxmap,
    pub bench: RdofDevBench,
}

/// rdof_txmap_t
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RdofTxmap {
    pub min_dbm: f64,
    pub max_dbm: f64,
}

/// rdof_unit_t
#[repr(C)]
pub struct RdofUnit {
    pub name: [u8; SITE_NAME + 4],
    pub kind: u8,
    pub rx: c_int,
    pub tx: c_int,
    pub app: c_int,
    pub freq: c_int,
    pub freqs: u64,
    pub duplex_full: bool,
}

/// rdof_plan_extra_t
#[repr(C)]
pub struct RdofPlanExtra {
    pub hz: u32,
    pub cc: c_int,
    pub dev: c_int,
    pub id: [u8; SITE_NAME + 8],
}

/// rdof_plan_t: only its pointer.
#[repr(C)]
pub struct RdofPlan {
    _private: [u8; 0],
}

extern "C" {
    pub fn rdof_txpower_level(m: *const RdofTxmap, dbm: f64) -> c_int;
    pub fn rdof_plan_open(text: *const c_char, len: usize, err: *mut c_char, cap: usize) -> *mut RdofPlan;
    pub fn rdof_plan_close(p: *mut RdofPlan);
    pub fn rdof_plan_site(p: *const RdofPlan) -> *const RdofSite;
    pub fn rdof_plan_doc(p: *const RdofPlan) -> *const JDoc;
    pub fn rdof_plan_root(p: *const RdofPlan) -> c_int;
    pub fn rdof_plan_dev_node(p: *const RdofPlan, dev: c_int) -> c_int;
    pub fn rdof_plan_freq_node(p: *const RdofPlan, freq: c_int) -> c_int;
    pub fn rdof_plan_app_node(p: *const RdofPlan, app: c_int) -> c_int;
    pub fn rdof_plan_net_node(p: *const RdofPlan, net: c_int) -> c_int;
    pub fn rdof_plan_net_of(p: *const RdofPlan, type_: c_int) -> c_int;
    pub fn rdof_plan_dev(p: *const RdofPlan, dev: c_int) -> *const RdofPlanDev;
    pub fn rdof_plan_nunits(p: *const RdofPlan) -> c_int;
    pub fn rdof_plan_unit(p: *const RdofPlan, k: c_int) -> *const RdofUnit;
    pub fn rdof_plan_nextra(p: *const RdofPlan) -> c_int;
    pub fn rdof_plan_extra(p: *const RdofPlan, k: c_int) -> *const RdofPlanExtra;
    pub fn rdof_plan_nbench(p: *const RdofPlan) -> c_int;
    pub fn rdof_plan_bench(p: *const RdofPlan, k: c_int) -> c_int;
    pub fn rdof_plan_receiver(p: *const RdofPlan, dev: c_int) -> c_int;
    pub fn rdof_plan_sdr_used(p: *const RdofPlan, dev: c_int) -> bool;

    pub fn rdof_site_path_rx(s: *const RdofSite, hz: u32, slot: c_int) -> c_int;
    pub fn rdof_site_app_of(s: *const RdofSite, type_: c_int) -> c_int;
    pub fn rdof_idset_ranges(s: *const RdofIdset, out: *mut RdofIdrange, max: c_int) -> c_int;
}

/// The plan's parts, borrowed from its pointer (valid while the plan is open).
#[derive(Clone, Copy)]
pub struct Plan<'a> {
    pub p: *const RdofPlan,
    pub site: &'a RdofSite,
    pub doc: &'a JDoc,
}

impl Plan<'_> {
    pub unsafe fn new(p: *const RdofPlan) -> Self {
        Plan { p, site: &*rdof_plan_site(p), doc: &*rdof_plan_doc(p) }
    }

    pub fn root(&self) -> c_int {
        unsafe { rdof_plan_root(self.p) }
    }

    pub fn dev_node(&self, d: c_int) -> c_int {
        unsafe { rdof_plan_dev_node(self.p, d) }
    }

    pub fn freq_node(&self, f: c_int) -> c_int {
        unsafe { rdof_plan_freq_node(self.p, f) }
    }

    pub fn app_node(&self, a: c_int) -> c_int {
        unsafe { rdof_plan_app_node(self.p, a) }
    }

    pub fn net_node(&self, n: c_int) -> c_int {
        unsafe { rdof_plan_net_node(self.p, n) }
    }

    pub fn net_of(&self, t: c_int) -> c_int {
        unsafe { rdof_plan_net_of(self.p, t) }
    }

    pub fn dev(&self, d: c_int) -> &RdofPlanDev {
        unsafe { &*rdof_plan_dev(self.p, d) }
    }

    pub fn nunits(&self) -> c_int {
        unsafe { rdof_plan_nunits(self.p) }
    }

    pub fn unit(&self, k: c_int) -> &RdofUnit {
        unsafe { &*rdof_plan_unit(self.p, k) }
    }

    pub fn nextra(&self) -> c_int {
        unsafe { rdof_plan_nextra(self.p) }
    }

    pub fn extra(&self, k: c_int) -> &RdofPlanExtra {
        unsafe { &*rdof_plan_extra(self.p, k) }
    }

    pub fn nbench(&self) -> c_int {
        unsafe { rdof_plan_nbench(self.p) }
    }

    pub fn bench(&self, k: c_int) -> c_int {
        unsafe { rdof_plan_bench(self.p, k) }
    }

    pub fn receiver(&self, d: c_int) -> c_int {
        unsafe { rdof_plan_receiver(self.p, d) }
    }

    pub fn sdr_used(&self, d: c_int) -> bool {
        unsafe { rdof_plan_sdr_used(self.p, d) }
    }

    pub fn path_rx(&self, hz: u32, slot: c_int) -> c_int {
        unsafe { rdof_site_path_rx(self.site, hz, slot) }
    }

    pub fn app_of(&self, t: c_int) -> c_int {
        unsafe { rdof_site_app_of(self.site, t) }
    }
}

/// The first ID of a set, 0 if it's empty.
pub fn first_id(set: &RdofIdset) -> u32 {
    let mut r = [RdofIdrange { lo: 0, hi: 0 }];
    if unsafe { rdof_idset_ranges(set, r.as_mut_ptr(), 1) } != 0 {
        r[0].lo
    } else {
        0
    }
}

/// A NUL-terminated field's text.
pub fn field(f: &[u8]) -> &[u8] {
    &f[..f.iter().position(|&c| c == 0).unwrap_or(f.len())]
}
