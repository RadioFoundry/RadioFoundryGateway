// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! A trunked site, joined up (rdof/t3unit.h; docs/spec/fw/tier3.md sections 2, 5 and 9): the controller (t3site.rs),
//! its radio layer (rdof/t3rf.h) and its part of the MQTT network (rdof/mqttnet.h, rdof/mnctl.h, C in common).
//!
//! rdof_t3unit_t is opaque to C: rdof_t3unit_size() bytes of the caller's, a Unit first, then the four mqtt-net
//! uplinks and the downlink, laid out at the sizes and alignments their C reports. The Rust sets their hooks through
//! mirrors of the structs' heads (MnUpHead, MnDownHead). The other C structs it passes are mirrored whole; each is in
//! rdof_t3unit_layout, which host-tests/test_t3unit.c checks against the headers.
//!
//! The radio layer and mqtt-net call back into the unit while they run, so everything here works through the raw
//! pointer, as the C did, and holds no reference to the Unit across a call out.

use core::ffi::{c_char, c_int, c_long, c_void, CStr};
use core::mem::{align_of, offset_of, size_of};
use core::ptr::{self, addr_of, addr_of_mut, null};

use crate::ffi::llround;
use crate::hblink::RdofNetBurst;
use crate::jsonp::{rdof_jat, rdof_jget, rdof_jnode, rdof_jparse, JDoc, JNode, RDOF_JNUM, RDOF_JSTR};
use crate::mmdvm::rdof_control_to_dt;
use crate::plan::RdofIdset;
use crate::t3cfg::{rdof_t3cfg_check, rdof_t3cfg_site};
use crate::t3rf::{
    rdof_t3rf_channel, rdof_t3rf_close, rdof_t3rf_control, rdof_t3rf_init, rdof_t3rf_net, rdof_t3rf_net_end,
    rdof_t3rf_open, rdof_t3rf_rx, rdof_t3rf_slot, rdof_t3rf_split_pump, rdof_t3rf_tick, T3rf, T3rfIo, T3rfSplit,
    WARMUP,
};
use crate::t3site::{
    self as t3, Cfg, Event, IdSet, MsPolicy, Neighbor, Policy, Record, Setup, Site, SlotOut, Status, TgPolicy,
};
use crate::text::{c_bytes, c_out, Out};
use crate::tracker::{RdofBurst, RdofFrame};

const CARRIERS: usize = t3::CARRIERS;
const CHANNELS: usize = t3::CHANNELS;
pub const RING: usize = 16;
pub const STREAMS: usize = 16;
pub const HOLD: usize = 12;
pub const TGS: usize = 20;
pub const FILTERS: usize = 10 + TGS;
pub const PEERS: usize = 32;
pub const STATUS_S: i64 = 10;

const NODE_MAX: usize = 39;
const JNODES: usize = 256;
const JBUF: usize = 2048;

// rdof/mqttnet.h, rdof/mnctl.h
const MN_T_NONE: c_int = 0;
const MN_T_TG: c_int = 1;
const MN_T_PC: c_int = 2;
const MN_T_DL: c_int = 3;
const MN_PATH_NONE: u8 = 0xFF;
const MN_HEADER: usize = 14;
const MN_SETUP_LEN: usize = 8;
const MN_F_EMERGENCY: u8 = 0x04;
const MN_REC_TG: usize = 8;
const MN_HAS_ENABLED: u32 = 1 << 14;
const MN_CFG_TIMERS: usize = 16;
const MN_CFG_BROADCASTS: usize = 4;
const MN_CFG_NEIGHBORS: usize = 32;
const MN_CFG_TG: usize = 64;
const MN_CFG_MS: usize = 512;
const MN_CFG_NAME: usize = 32;
const MN_CFG_APPLIED: c_int = 1;
const MN_CFG_DELETED: c_int = 2;
const MN_CFG_BAD: c_int = 4;
const MN_CFG_FULL: c_int = 5;
const MN_CMD_DEREGISTER: c_int = 1;
const MN_CMD_CLEAR: c_int = 2;
const MN_CMD_WITHDRAW: c_int = 3;
const MN_CMD_RESTORE: c_int = 4;
const MN_CMD_RESEND_RECORDS: c_int = 5;
const MN_CMD_STATUS: c_int = 6;

// ── the C structs: rdof/t3unit.h's ─────────────────────────────────────────────

/// rdof_t3unit_peer_t
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Peer {
    pub node: [u8; NODE_MAX + 1],
    pub trunk: bool,
    pub online: bool,
    pub syscode: u16,
    pub chan: u16,
    pub site: u16,
}

/// rdof_t3unit_io_t
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Io {
    pub publish: Option<
        unsafe extern "C" fn(
            ctx: *mut c_void,
            topic: *const c_char,
            data: *const c_void,
            len: usize,
            qos: c_int,
            retain: bool,
        ),
    >,
    pub subscribe: Option<unsafe extern "C" fn(ctx: *mut c_void, filters: *const *const c_char, n: c_int)>,
    pub log: Option<unsafe extern "C" fn(ctx: *mut c_void, text: *const c_char)>,
    pub cache: Option<unsafe extern "C" fn(ctx: *mut c_void)>,
    pub rand32: Option<unsafe extern "C" fn(ctx: *mut c_void) -> u32>,
    pub ctx: *mut c_void,
}

/// rdof_t3unit_counts_t
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Counts {
    pub net_in: u32,
    pub net_held: u32,
    pub net_dropped: u32,
    pub setups_out: u32,
    pub setups_in: u32,
    pub records_out: u32,
    pub records_in: u32,
    pub cfg_applied: u32,
    pub cfg_bad: u32,
    pub cmds: u32,
    pub tg_filtered: u32,
}

// ── the C structs: mqtt-net's ──────────────────────────────────────────────────

/// rdof_mn_hdr_t
#[repr(C)]
pub struct MnHdr {
    pub flags: u8,
    pub path: u8,
    pub seq: u8,
    pub stream: u32,
    pub src: u32,
    pub dst: u32,
}

/// rdof_mn_setup_t
#[repr(C)]
pub struct MnSetup {
    pub op: u8,
    pub reason: u8,
    pub flags: u8,
    pub options: u8,
    pub r#ref: u32,
}

/// rdof_mn_ev_t
#[repr(C)]
pub struct MnEv {
    pub node: *const c_char,
    pub wall: f64,
    pub path: c_int,
    pub slot: c_int,
    pub source: *const c_char,
    pub stream: u32,
}

/// rdof_mn_record_t
#[repr(C)]
pub struct MnRecord {
    pub id: u32,
    pub at_ms: i64,
    pub exp_ms: i64,
    pub implicit: bool,
    pub tg: [u32; MN_REC_TG],
    pub ntg: c_int,
    pub sub: u32,
    pub area: c_int,
}

/// rdof_mn_where_t
#[repr(C)]
pub struct MnWhere {
    pub node: [u8; NODE_MAX + 1],
    pub path: c_int,
    pub at_ms: i64,
    pub reg: bool,
}

/// rdof_mn_chan_status_t
#[repr(C)]
#[derive(Clone, Copy)]
pub struct MnChanStatus {
    pub chan: c_int,
    pub lcn: c_int,
    pub r#use: *const c_char,
    pub src: u32,
    pub dst: u32,
    pub group: bool,
    pub since_ms: i64,
}

/// rdof_mn_trunk_status_t
#[repr(C)]
pub struct MnTrunkStatus {
    pub connected: bool,
    pub regs: c_int,
    pub queue: c_int,
    pub ch: *const MnChanStatus,
    pub nch: c_int,
    pub aloha: u32,
    pub rand: u32,
    pub collisions: u32,
    pub grants: u32,
    pub refused: u32,
    pub queued: u32,
}

/// rdof_mn_status_t
#[repr(C)]
pub struct MnStatus {
    pub node: *const c_char,
    pub firmware: *const c_char,
    pub wall: f64,
    pub uptime_s: f64,
    pub trunk: *const MnTrunkStatus,
}

/// rdof_mn_carrier_t
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct MnCarrier {
    pub chan: c_int,
    pub tx_hz: u32,
    pub rx_hz: u32,
    pub cc: c_int,
}

/// rdof_mn_trunk_info_t
#[repr(C)]
pub struct MnTrunkInfo {
    pub model: *const c_char,
    pub net: c_int,
    pub site: c_int,
    pub area_bits: c_int,
    pub par: *const c_char,
    pub tscc: *const MnCarrier,
    pub ntscc: c_int,
    pub carriers: *const MnCarrier,
    pub ncarriers: c_int,
    pub services: *const *const c_char,
    pub nservices: c_int,
    pub reg: bool,
}

/// rdof_mn_timer_t
#[repr(C)]
pub struct MnTimer {
    pub name: [u8; 20],
    pub value: f64,
}

/// rdof_mn_site_cfg_t
#[repr(C)]
pub struct MnSiteCfg {
    pub has: u32,
    pub name: [u8; MN_CFG_NAME],
    pub model: c_int,
    pub net: c_int,
    pub site: c_int,
    pub par: c_int,
    pub area_bits: c_int,
    pub listed: bool,
    pub refused: bool,
    pub require_registration: bool,
    pub foacsu: bool,
    pub enabled: bool,
    pub ms: RdofIdset,
    pub private_: RdofIdset,
    pub tg: RdofIdset,
    pub wide_tg: RdofIdset,
    pub static_tg: RdofIdset,
    pub neighbors: [c_int; MN_CFG_NEIGHBORS],
    pub nneighbors: c_int,
    pub timers: [MnTimer; MN_CFG_TIMERS],
    pub ntimers: c_int,
    pub broadcast: [MnTimer; MN_CFG_BROADCASTS],
    pub nbroadcast: c_int,
}

/// rdof_mn_tg_cfg_t
#[repr(C)]
pub struct MnTgCfg {
    pub tg: u32,
    pub rev: c_long,
    pub name: [u8; MN_CFG_NAME],
    pub has_wide: bool,
    pub wide: bool,
    pub sites: RdofIdset,
    pub ms: RdofIdset,
    pub priority: c_int,
    pub hang_s: f64,
}

/// rdof_mn_ms_cfg_t
#[repr(C)]
pub struct MnMsCfg {
    pub id: u32,
    pub rev: c_long,
    pub name: [u8; MN_CFG_NAME],
    pub enabled: bool,
    pub private_: bool,
    pub wide_private: bool,
    pub emergency: bool,
    pub priority: c_int,
    pub tg: [u32; MN_REC_TG],
    pub ntg: c_int,
}

/// rdof_mn_cfg_t
#[repr(C)]
pub struct MnCfg {
    pub node: [u8; NODE_MAX + 1],
    pub net_rev: c_long,
    pub node_rev: c_long,
    pub net: MnSiteCfg,
    pub nodecfg: MnSiteCfg,
    pub ntg: c_int,
    pub nms: c_int,
    pub tg: [MnTgCfg; MN_CFG_TG],
    pub ms: [MnMsCfg; MN_CFG_MS],
    pub changes: u32,
}

/// rdof_mn_cmd_t
#[repr(C)]
pub struct MnCmd {
    pub id: [u8; 40],
    pub cmd: c_int,
    pub name: [u8; 24],
    pub ms: u32,
    pub chan: c_int,
    pub lcn: c_int,
}

/// rdof_mn_up_t's head: node to ctx (the hooks). The rest is the C's alone, rdof_mn_up_size() bytes in all.
#[repr(C)]
struct MnUpHead {
    node: *const c_char,
    tx_ok: u64,
    max_msg: usize,
    path_of: Option<unsafe extern "C" fn(ctx: *mut c_void, slot: c_int) -> c_int>,
    admit: Option<unsafe extern "C" fn(ctx: *mut c_void, slot: c_int, src: u32, dst: u32, group: bool) -> bool>,
    publish: Option<unsafe extern "C" fn(ctx: *mut c_void, kind: c_int, dst: u32, msg: *const u8, len: usize)>,
    activity: Option<unsafe extern "C" fn(ctx: *mut c_void, json: *const c_char)>,
    data_done: Option<unsafe extern "C" fn(ctx: *mut c_void, d: *const c_void)>,
    response: Option<
        unsafe extern "C" fn(ctx: *mut c_void, slot: c_int, src: u32, dst: u32, klass: c_int, ty: c_int, status: c_int),
    >,
    rand32: Option<unsafe extern "C" fn(ctx: *mut c_void) -> u32>,
    wall: Option<unsafe extern "C" fn(ctx: *mut c_void) -> f64>,
    ctx: *mut c_void,
}

/// rdof_mn_down_t's head: raw_tx to ctx. The rest is the C's alone, rdof_mn_down_size() bytes in all.
#[repr(C)]
struct MnDownHead {
    raw_tx: bool,
    max_preambles: c_int,
    burst: Option<
        unsafe extern "C" fn(
            ctx: *mut c_void,
            b: *const RdofNetBurst,
            path: c_int,
            dl: bool,
            flags: u8,
            from: *const c_char,
        ),
    >,
    data:
        Option<unsafe extern "C" fn(ctx: *mut c_void, h: *const MnHdr, confirmed: bool, dl: bool, from: *const c_char)>,
    setup: Option<
        unsafe extern "C" fn(ctx: *mut c_void, h: *const MnHdr, s: *const MnSetup, dl: bool, from: *const c_char),
    >,
    ctx: *mut c_void,
}

extern "C" {
    // rdof/mqttnet.h
    fn rdof_mn_node_ok(node: *const c_char) -> bool;
    fn rdof_mn_topic(
        out: *mut c_char,
        cap: usize,
        root: *const c_char,
        kind: c_int,
        id: u32,
        node: *const c_char,
    ) -> usize;
    fn rdof_mn_node_topic(
        out: *mut c_char,
        cap: usize,
        root: *const c_char,
        node: *const c_char,
        leaf: *const c_char,
    ) -> usize;
    fn rdof_mn_topic_parse(
        topic: *const c_char,
        root: *const c_char,
        id: *mut u32,
        node: *mut c_char,
        cap: usize,
    ) -> c_int;
    fn rdof_mn_setup_msg(
        out: *mut u8,
        cap: usize,
        stream: u32,
        src: u32,
        dst: u32,
        path: u8,
        s: *const MnSetup,
    ) -> usize;
    fn rdof_mn_reg_event(
        e: *const MnEv,
        src: u32,
        action: *const c_char,
        tg: *const u32,
        ntg: c_int,
        out: *mut c_char,
        cap: usize,
    ) -> usize;
    fn rdof_mn_grant_event(
        e: *const MnEv,
        src: u32,
        dst: u32,
        group: bool,
        chan: c_int,
        lcn: c_int,
        late: bool,
        out: *mut c_char,
        cap: usize,
    ) -> usize;
    fn rdof_mn_queue_event(
        e: *const MnEv,
        src: u32,
        dst: u32,
        group: bool,
        action: *const c_char,
        why: *const c_char,
        out: *mut c_char,
        cap: usize,
    ) -> usize;
    fn rdof_mn_setup_event(
        e: *const MnEv,
        src: u32,
        dst: u32,
        op: c_int,
        reason: c_int,
        peer: *const c_char,
        out: *mut c_char,
        cap: usize,
    ) -> usize;
    fn rdof_mn_up_init(u: *mut MnUpHead, node: *const c_char);
    fn rdof_mn_up_frame(u: *mut MnUpHead, f: *const RdofFrame, now: f64);
    fn rdof_mn_up_tick(u: *mut MnUpHead, now: f64);
    fn rdof_mn_up_size() -> usize;
    fn rdof_mn_up_align() -> usize;
    fn rdof_mn_down_init(d: *mut MnDownHead);
    fn rdof_mn_down_message(
        d: *mut MnDownHead,
        node: *const c_char,
        dl: bool,
        msg: *const u8,
        len: usize,
        now: f64,
    ) -> bool;
    fn rdof_mn_down_tick(d: *mut MnDownHead, now: f64);
    fn rdof_mn_down_size() -> usize;
    fn rdof_mn_down_align() -> usize;

    // rdof/mnctl.h
    fn rdof_mn_ms_topic(out: *mut c_char, cap: usize, root: *const c_char, node: *const c_char, id: u32) -> usize;
    fn rdof_mn_ms_topic_parse(
        topic: *const c_char,
        root: *const c_char,
        node: *mut c_char,
        cap: usize,
        id: *mut u32,
    ) -> bool;
    fn rdof_mn_where_topic_parse(topic: *const c_char, root: *const c_char, id: *mut u32) -> bool;
    fn rdof_mn_record_json(r: *const MnRecord, out: *mut c_char, cap: usize) -> usize;
    fn rdof_mn_record_parse(json: *const c_char, len: usize, r: *mut MnRecord, err: *mut c_char, cap: usize) -> bool;
    fn rdof_mn_where_parse(json: *const c_char, len: usize, w: *mut MnWhere, err: *mut c_char, cap: usize) -> bool;
    fn rdof_mn_status_json(s: *const MnStatus, out: *mut c_char, cap: usize) -> usize;
    fn rdof_mn_trunk_info_json(t: *const MnTrunkInfo, out: *mut c_char, cap: usize) -> usize;
    fn rdof_mn_cfg_init(c: *mut MnCfg, node: *const c_char);
    fn rdof_mn_cfg_message(
        c: *mut MnCfg,
        root: *const c_char,
        topic: *const c_char,
        payload: *const c_char,
        len: usize,
        note: *mut c_char,
        cap: usize,
    ) -> c_int;
    fn rdof_mn_cfg_site(c: *const MnCfg, base: *const MnSiteCfg, out: *mut MnSiteCfg);
    fn rdof_mn_cmd_parse(json: *const c_char, len: usize, c: *mut MnCmd, err: *mut c_char, cap: usize) -> bool;
    fn rdof_mn_cmd_result_json(
        id: *const c_char,
        ok: bool,
        error: *const c_char,
        out: *mut c_char,
        cap: usize,
    ) -> usize;

}

// ── the unit ───────────────────────────────────────────────────────────────────

#[derive(Clone, Copy)]
struct Stream {
    used: bool,
    mapped: bool,
    group: bool,
    ended: bool,
    stream: u32,
    src: u32,
    dst: u32,
    ch: c_int,
    last_ms: i64,
    hold: [RdofFrame; HOLD],
    nhold: usize,
}

#[derive(Clone, Copy)]
struct Held {
    k: i64, // -1: none
    o: SlotOut,
}

struct UpCtx {
    u: *mut Unit,
    c: c_int,
}

/// rdof_t3unit_t. Valid all zero (init zeroes it, then fills it in).
pub struct Unit {
    io: Io,
    net: bool, // on the MQTT network (a root and node given)
    root: [u8; 32],
    node: [u8; NODE_MAX + 1],
    firmware: [u8; 48],
    path: c_int, // the trunk's path on the site (events, the uplink's path byte); -1 none
    site: *mut Site,
    base: Cfg, // the settings file's
    cfg: Cfg,  // what runs (provisioning over it)
    base_pol: Policy,
    pol: Policy,
    withdrawn_cfg: bool, // cfg/node's enabled false
    rf: T3rf,
    rf_tscc: bool, // rf.tscc, kept here: the radio layer's callbacks don't read rf
    mc: MnCfg,     // provisioning as received
    ms_ids: [u32; t3::MS_POLICIES],
    nms: usize,
    tg_ids: [u32; t3::TG_POLICIES], // cfg/tg documents applied
    ntgp: usize,
    peers: [Peer; PEERS], // the other nodes' trunk info and presence
    npeers: usize,
    nneighbors: usize, // in the controller's adjacent-site list
    up: [*mut MnUpHead; CARRIERS],
    upctx: [UpCtx; CARRIERS],
    down: *mut MnDownHead,
    ring: [Held; RING],
    k_next: i64,              // the next slot for the controller; -1 before the first
    ch_flags: [u8; CHANNELS], // t3::F_* of the call on each channel (0: none)
    st: [Stream; STREAMS],
    tgs: [u32; TGS],
    ntgs: usize,
    tg_all: bool, // subscribed {root}/tg/+/+
    connected: bool,
    now_ms: i64,
    start_ms: i64,
    status_ms: i64,
    n: Counts,
    // peer_info's parse
    jnodes: [JNode; JNODES],
    jbuf: [u8; JBUF],
}

const fn round_up(n: usize, a: usize) -> usize {
    n.div_ceil(a) * a
}

/// Where the uplinks and downlink go in the caller's memory: (first uplink, uplink stride, downlink, total bytes,
/// alignment needed).
fn mem_plan() -> (usize, usize, usize, usize, usize) {
    let (ua, us, da, ds) =
        unsafe { (rdof_mn_up_align(), rdof_mn_up_size(), rdof_mn_down_align(), rdof_mn_down_size()) };
    let up0 = round_up(size_of::<Unit>(), ua);
    let stride = round_up(us, ua);
    let down = round_up(up0 + CARRIERS * stride, da);
    (up0, stride, down, down + ds, align_of::<Unit>().max(ua).max(da))
}

// ── small helpers ──────────────────────────────────────────────────────────────

/// A NUL-terminated array's text.
fn cstr(b: &[u8]) -> &[u8] {
    &b[..b.iter().position(|&c| c == 0).unwrap_or(b.len())]
}

/// snprintf into buf: cut to fit, NUL terminated.
fn put(buf: &mut [u8], f: impl FnOnce(&mut Out)) {
    let room = buf.len() - 1;
    let mut o = Out::new(&mut buf[..room]);
    f(&mut o);
    let n = o.n.min(room);
    buf[n] = 0;
}

/// %0Wx
fn hex(o: &mut Out, v: u32, width: usize) {
    let mut d = [0u8; 8];
    let mut k = d.len();
    let mut m = v;
    loop {
        k -= 1;
        d[k] = b"0123456789abcdef"[(m & 15) as usize];
        m >>= 4;
        if m == 0 {
            break;
        }
    }
    (d.len() - k..width).for_each(|_| o.byte(b'0'));
    o.bytes(&d[k..]);
}

fn cp(b: &[u8]) -> *const c_char {
    b.as_ptr().cast()
}

fn cpm(b: &mut [u8]) -> *mut c_char {
    b.as_mut_ptr().cast()
}

/// A struct with no implicit padding, as bytes (what C's memcmp compares).
unsafe fn bytes_of<T>(v: &T) -> &[u8] {
    core::slice::from_raw_parts((v as *const T).cast::<u8>(), size_of::<T>())
}

unsafe fn log(u: *mut Unit, f: impl FnOnce(&mut Out)) {
    let Some(l) = (*u).io.log else { return };
    let mut t = [0u8; 200];
    put(&mut t, f);
    l((*u).io.ctx, cp(&t));
}

unsafe fn publish(u: *mut Unit, topic: *const c_char, data: *const c_void, len: usize, qos: c_int, retain: bool) {
    if (*u).net {
        if let Some(p) = (*u).io.publish {
            p((*u).io.ctx, topic, data, len, qos, retain);
        }
    }
}

unsafe fn publish_str(u: *mut Unit, topic: *const c_char, s: *const c_char, qos: c_int, retain: bool) {
    publish(u, topic, s.cast(), CStr::from_ptr(s).to_bytes().len(), qos, retain);
}

unsafe fn wall_s(u: *const Unit) -> f64 {
    (*u).now_ms as f64 / 1000.0
}

unsafe fn rnd(u: *mut Unit) -> u32 {
    let r = match (*u).io.rand32 {
        Some(f) => f((*u).io.ctx),
        None => 0x9E37_79B9u32.wrapping_mul(((*u).now_ms + 1) as u32),
    };
    if r != 0 {
        r
    } else {
        1
    }
}

unsafe fn root(u: *const Unit) -> *const c_char {
    addr_of!((*u).root).cast()
}

unsafe fn node(u: *const Unit) -> *const c_char {
    addr_of!((*u).node).cast()
}

unsafe fn rf(u: *mut Unit) -> *mut T3rf {
    addr_of_mut!((*u).rf)
}

// ── the subscriptions ──────────────────────────────────────────────────────────

fn tg_in(s: &IdSet, tg: u32) -> bool {
    s.all != 0 || s.r[..(s.n as usize).min(t3::IDSET_RANGES)].iter().any(|r| tg >= r.lo && tg <= r.hi)
}

/// The talkgroups the site needs: its MSs' (the controller's) and static_tg; all when they're too many to list.
unsafe fn want_tgs(u: *mut Unit, tgs: &mut [u32; TGS]) -> (usize, bool) {
    let mut t = [0u32; TGS + 1];
    let m = t3::rdof_t3_subs((*u).site, t.as_mut_ptr(), TGS + 1);
    let mut all = m > TGS || (*u).pol.static_tg.all != 0;
    let mut n = 0;
    for &g in &t[..m.min(TGS)] {
        tgs[n] = g;
        n += 1;
    }
    let s = (*u).pol.static_tg;
    for r in &s.r[..(s.n as usize).min(t3::IDSET_RANGES)] {
        if all {
            break;
        }
        let mut g = u64::from(r.lo);
        while g <= u64::from(r.hi) && !all {
            if !tgs[..n].iter().any(|&x| u64::from(x) == g) {
                if n == TGS {
                    all = true;
                } else {
                    tgs[n] = g as u32;
                    n += 1;
                }
            }
            g += 1;
        }
    }
    (n, all)
}

unsafe fn subscribe(u: *mut Unit, force: bool) {
    let mut tgs = [0u32; TGS];
    let (n, all) = want_tgs(u, &mut tgs);
    let same = !force && all == (*u).tg_all && n == (*u).ntgs && tgs[..n] == (&(*u).tgs)[..n];
    (*u).tgs = tgs;
    (*u).ntgs = n;
    (*u).tg_all = all;
    let Some(sub) = (*u).io.subscribe else { return };
    if same || !(*u).net {
        return;
    }
    let mut f = [[0u8; 128]; FILTERS];
    let r = cstr(&(*u).root);
    let nd = cstr(&(*u).node);
    rdof_mn_topic(cpm(&mut f[0]), 128, root(u), MN_T_DL, 0, node(u));
    let leaves: [(&[u8], &[u8], &[u8]); 9] = [
        (b"/cfg/net", b"", b""),
        (b"/cfg/node/", nd, b""),
        (b"/cfg/tg/+", b"", b""),
        (b"/cfg/ms/+", b"", b""),
        (b"/node/", nd, b"/ms/+"),
        (b"/node/", nd, b"/cmd"),
        (b"/where/+", b"", b""),
        (b"/node/+/info", b"", b""),
        (b"/node/+/online", b"", b""),
    ];
    for (b, (x, y, z)) in f[1..].iter_mut().zip(leaves) {
        put(b, |o| {
            o.bytes(r);
            o.bytes(x);
            o.bytes(y);
            o.bytes(z);
        });
    }
    let mut k = 1 + leaves.len();
    if all {
        put(&mut f[k], |o| (o.bytes(r), o.bytes(b"/tg/+/+")).1);
        k += 1;
    } else {
        for &g in &tgs[..n] {
            put(&mut f[k], |o| {
                o.bytes(r);
                o.bytes(b"/tg/");
                o.int(i64::from(g), 0);
                o.bytes(b"/+");
            });
            k += 1;
        }
    }
    let mut fp = [null::<c_char>(); FILTERS];
    for (p, b) in fp.iter_mut().zip(f.iter()).take(k) {
        *p = cp(b);
    }
    sub((*u).io.ctx, fp.as_ptr(), k as c_int);
}

// ── events out ─────────────────────────────────────────────────────────────────

unsafe fn activity_json(u: *mut Unit, json: *const c_char) {
    let mut t = [0u8; 128];
    if rdof_mn_node_topic(cpm(&mut t), t.len(), root(u), node(u), c"activity".as_ptr()) != 0 {
        publish_str(u, cp(&t), json, 1, false);
    }
}

unsafe fn ev_of(u: *const Unit, ch: c_int, net: bool, stream: u32) -> MnEv {
    MnEv {
        node: node(u),
        wall: wall_s(u),
        path: (*u).path,
        slot: if ch >= 0 { (ch & 1) + 1 } else { 0 },
        source: if net { c"net".as_ptr() } else { c"rf".as_ptr() },
        stream,
    }
}

unsafe fn record_out(u: *mut Unit, id: u32, del: bool) {
    let mut t = [0u8; 128];
    let mut j = [0u8; 512];
    if rdof_mn_ms_topic(cpm(&mut t), t.len(), root(u), node(u), id) == 0 {
        return;
    }
    let mut r = Record::default();
    if del || !t3::rdof_t3_record((*u).site, id, &mut r) {
        publish(u, cp(&t), c"".as_ptr().cast(), 0, 1, true);
        (*u).n.records_out += 1;
        return;
    }
    let ntg = usize::from(r.ntg).min(MN_REC_TG);
    let mut m = MnRecord {
        id: r.id,
        at_ms: r.at_ms,
        exp_ms: r.exp_ms,
        implicit: r.how == t3::HOW_CALL,
        tg: [0; MN_REC_TG],
        ntg: ntg as c_int,
        sub: r.sub_tg,
        area: c_int::from(r.area),
    };
    m.tg[..ntg].copy_from_slice(&r.tg[..ntg]);
    if rdof_mn_record_json(&m, cpm(&mut j), j.len()) != 0 {
        publish_str(u, cp(&t), cp(&j), 1, true);
        (*u).n.records_out += 1;
    }
}

/// The called party's side answers (mqtt-net.md 9.4): its messages come from the called radio to the caller. The
/// controller names both ways as caller (src) and called (dst).
fn answer_op(op: u8) -> bool {
    matches!(op, t3::SETUP_ACCEPT | t3::SETUP_REJECT | t3::SETUP_ALERTING | t3::SETUP_ANSWERED)
}

unsafe fn setup_out(u: *mut Unit, e: &Event) {
    let s = MnSetup { op: e.op, reason: e.reason, flags: e.flags, options: e.svc, r#ref: e.r#ref };
    let (src, dst) = if answer_op(e.op) { (e.dst, e.src) } else { (e.src, e.dst) };
    let mut m = [0u8; MN_HEADER + 2 + MN_SETUP_LEN];
    let path = if (*u).path >= 0 { (*u).path as u8 } else { MN_PATH_NONE };
    let n = rdof_mn_setup_msg(m.as_mut_ptr(), m.len(), e.stream, src, dst, path, &s);
    let mut t = [0u8; 128];
    if n != 0 && rdof_mn_topic(cpm(&mut t), t.len(), root(u), MN_T_PC, dst, node(u)) != 0 {
        publish(u, cp(&t), m.as_ptr().cast(), n, 1, false);
        (*u).n.setups_out += 1;
    }
}

unsafe fn activity_out(u: *mut Unit, e: &Event) {
    const REG: [&CStr; 6] = [c"?", c"register", c"deregister", c"expire", c"deny", c"refuse"];
    const QUEUE: [&CStr; 4] = [c"?", c"in", c"out", c"drop"];
    let mut j = [0u8; 512];
    let mut why = [0u8; 8];
    put(&mut why, |o| (o.bytes(b"0x"), hex(o, u32::from(e.reason), 2)).1);
    let group = e.flags & t3::F_GROUP != 0;
    let grant = e.r#type == t3::EV_ACTIVITY && e.kind == t3::ACT_GRANT;
    let ev = ev_of(u, if grant { c_int::from(e.lcn) } else { -1 }, e.flags & t3::F_NET != 0, e.stream);
    let (jp, jn) = (cpm(&mut j), 512);
    let n = match e.kind {
        t3::ACT_REG => {
            let mut r = Record::default();
            let action = REG[if e.op <= 5 { usize::from(e.op) } else { 0 }];
            let (tg, ntg) = if t3::rdof_t3_record((*u).site, e.src, &mut r) {
                (r.tg.as_ptr(), c_int::from(r.ntg))
            } else {
                (null(), 0)
            };
            rdof_mn_reg_event(&ev, e.src, action.as_ptr(), tg, ntg, jp, jn)
        }
        t3::ACT_GRANT => {
            let late = e.flags & t3::F_LATE != 0;
            rdof_mn_grant_event(&ev, e.src, e.dst, group, c_int::from(e.chan), c_int::from(e.lcn), late, jp, jn)
        }
        t3::ACT_QUEUE => {
            let action = QUEUE[if e.op <= 3 { usize::from(e.op) } else { 0 }];
            let w = if e.op == 3 { cp(&why) } else { null() };
            rdof_mn_queue_event(&ev, e.src, e.dst, group, action.as_ptr(), w, jp, jn)
        }
        t3::ACT_SETUP => {
            rdof_mn_setup_event(&ev, e.src, e.dst, c_int::from(e.op), c_int::from(e.reason), null(), jp, jn)
        }
        t3::ACT_REFUSE => rdof_mn_queue_event(&ev, e.src, e.dst, group, c"refuse".as_ptr(), cp(&why), jp, jn),
        _ => 0,
    };
    if n != 0 {
        activity_json(u, cp(&j));
    }
}

// ── network streams ────────────────────────────────────────────────────────────

unsafe fn stream_find(u: *const Unit, id: u32) -> Option<usize> {
    (0..STREAMS).find(|&i| (*u).st[i].used && (*u).st[i].stream == id)
}

unsafe fn stream_feed(u: *mut Unit, i: usize, f: &RdofFrame) {
    let s = addr_of_mut!((*u).st[i]);
    if (*s).mapped {
        if !rdof_t3rf_net(rf(u), (*s).ch, f) {
            (*u).n.net_dropped += 1;
        }
        return;
    }
    if (*s).nhold == HOLD {
        // keep the header (the first), drop the oldest after it
        (*s).hold.copy_within(2..HOLD, 1);
        (*s).nhold -= 1;
        (*u).n.net_dropped += 1;
    }
    (*s).hold[(*s).nhold] = *f;
    (*s).nhold += 1;
    (*u).n.net_held += 1;
}

unsafe fn stream_map(u: *mut Unit, id: u32, ch: c_int) {
    let Some(i) = stream_find(u, id) else { return };
    let s = addr_of_mut!((*u).st[i]);
    if (*s).mapped {
        return;
    }
    (*s).mapped = true;
    (*s).ch = ch;
    for h in 0..(*s).nhold {
        if !rdof_t3rf_net(rf(u), ch, addr_of!((*s).hold[h])) {
            (*u).n.net_dropped += 1;
        }
    }
    (*s).nhold = 0;
}

unsafe fn stream_end(u: *mut Unit, i: usize, term: bool) {
    let s = addr_of_mut!((*u).st[i]);
    if (*s).mapped && !term {
        rdof_t3rf_net_end(rf(u), (*s).ch);
    }
    if (*s).group {
        t3::rdof_t3_net_group_end((*u).site, (*s).stream);
    } else {
        t3::rdof_t3_net_private_end((*u).site, (*s).stream);
    }
    ptr::write_bytes(s, 0, 1);
}

// ── the controller's events ────────────────────────────────────────────────────

unsafe fn drain(u: *mut Unit) {
    let mut e = Event::default();
    let mut subs = false;
    while t3::rdof_t3_event((*u).site, &mut e) {
        match e.r#type {
            t3::EV_CH_OPEN => {
                let ch = usize::from(e.ch);
                if ch < CHANNELS {
                    (*u).ch_flags[ch] = e.flags; // the call's, for the uplink
                    rdof_t3rf_open(rf(u), ch as c_int, e.src, e.dst, e.flags & t3::F_GROUP != 0, WARMUP);
                    if e.stream != 0 {
                        stream_map(u, e.stream, ch as c_int);
                    }
                }
            }
            t3::EV_CH_CLOSE => {
                let ch = c_int::from(e.ch);
                let c = rdof_t3rf_channel(rf(u), ch);
                if !c.is_null() && (*c).ctl_left > 0 {
                    (*c).close_after = true; // after its last P_CLEAR
                } else if !c.is_null() {
                    rdof_t3rf_close(rf(u), ch);
                }
                if usize::from(e.ch) < CHANNELS {
                    (*u).ch_flags[usize::from(e.ch)] = 0;
                }
                for i in 0..STREAMS {
                    let s = addr_of_mut!((*u).st[i]);
                    if (*s).used && (*s).mapped && (*s).ch == ch {
                        (*s).mapped = false;
                        (*s).ended = true;
                    }
                }
            }
            t3::EV_CH_STREAM => stream_map(u, e.stream, c_int::from(e.ch)),
            t3::EV_NET_SETUP => setup_out(u, &e),
            t3::EV_NET_SUBS => subs = true,
            t3::EV_RECORD => record_out(u, e.src, e.why == 1),
            t3::EV_ACTIVITY => activity_out(u, &e),
            t3::EV_NET_DROP => log(u, |o| {
                o.bytes(b"tier3: network stream ");
                hex(o, e.stream, 8);
                o.bytes(b" not carried (");
                o.int(i64::from(e.why), 0);
                o.bytes(b")");
            }),
            t3::EV_LOG => log(u, |o| (o.bytes(b"tier3: "), o.bytes(cstr(&e.text))).1),
            _ => {}
        }
    }
    if subs {
        subscribe(u, false);
    }
}

// ── the radio layer's callbacks ────────────────────────────────────────────────

unsafe fn held(u: *const Unit, k: i64) -> *const SlotOut {
    if k < 0 {
        return null();
    }
    let r = addr_of!((*u).ring[(k % RING as i64) as usize]);
    if (*r).k == k {
        addr_of!((*r).o)
    } else {
        null()
    }
}

unsafe extern "C" fn io_tscc(ctx: *mut c_void, k: i64, dt: *mut c_int, pdu: *mut u8) -> bool {
    let o = held(ctx.cast(), k);
    if o.is_null() || (*o).tscc_dt == 0 {
        return false;
    }
    *dt = c_int::from((*o).tscc_dt);
    ptr::copy_nonoverlapping((*o).tscc_pdu.as_ptr(), pdu, 10);
    true
}

unsafe extern "C" fn io_cach(ctx: *mut c_void, carrier: c_int, k: i64, at: *mut c_int, slc28: *mut i32) {
    let u: *mut Unit = ctx.cast();
    let (o, prev) = (held(u, k), held(u, k - 1));
    let tscc = carrier == 0 && (*u).rf_tscc;
    if tscc && !prev.is_null() {
        *at = c_int::from((*prev).at); // the controller's for the CACH before slot k is slot k - 1's
    }
    if !o.is_null() && (*o).has_slc != 0 {
        *slc28 = (if tscc { (*o).slc_tscc } else { (*o).slc_payload }) as i32;
    }
}

unsafe extern "C" fn io_pdu(ctx: *mut c_void, ch: c_int, k: i64, dt: c_int, pdu: *const u8, rssi: c_int) {
    let u: *mut Unit = ctx.cast();
    t3::rdof_t3_rx_pdu((*u).site, ch, k, dt as u8, pdu.cast(), rssi);
}

unsafe extern "C" fn io_bad(ctx: *mut c_void, ch: c_int, k: i64) {
    let u: *mut Unit = ctx.cast();
    t3::rdof_t3_rx_bad((*u).site, ch, k);
}

unsafe extern "C" fn io_activity(ctx: *mut c_void, ch: c_int, k: i64, what: c_int, src: u32, dst: u32, group: bool) {
    const MAP: [u8; 5] = [0, t3::VOICE_HEADER, t3::VOICE_FRAME, t3::VOICE_TERM, t3::VOICE_LOST];
    let u: *mut Unit = ctx.cast();
    if (1..=4).contains(&what) {
        t3::rdof_t3_rx_voice((*u).site, ch, k, MAP[what as usize], src, dst, group);
    }
}

unsafe extern "C" fn io_uplink(ctx: *mut c_void, ch: c_int, f: *const RdofFrame) {
    let u: *mut Unit = ctx.cast();
    let c = ch / 2;
    if (*u).net && (0..CARRIERS as c_int).contains(&c) {
        rdof_mn_up_frame((*u).up[c as usize], f, wall_s(u));
    }
}

// ── the uplinks: a carrier each, its slots its LCNs ────────────────────────────

unsafe fn up_unit(ctx: *mut c_void) -> (*mut Unit, usize) {
    let x: *const UpCtx = ctx.cast();
    ((*x).u, (*x).c as usize)
}

/// The unit, and whether the call on the uplink's slot goes to the network (wide, or a private call across).
unsafe fn up_wide(ctx: *mut c_void, slot: c_int) -> (*mut Unit, bool) {
    let (u, c) = up_unit(ctx);
    (u, (*u).ch_flags[2 * c + usize::from(slot == 2)] & (t3::F_WIDE | t3::F_PRIVATE_NET) != 0)
}

unsafe extern "C" fn up_path_of(ctx: *mut c_void, slot: c_int) -> c_int {
    let (u, wide) = up_wide(ctx, slot);
    if (*u).path >= 0 && wide {
        (*u).path
    } else {
        -1
    }
}

unsafe extern "C" fn up_admit(ctx: *mut c_void, slot: c_int, _src: u32, _dst: u32, _group: bool) -> bool {
    up_wide(ctx, slot).1
}

unsafe extern "C" fn up_publish(ctx: *mut c_void, kind: c_int, dst: u32, msg: *const u8, len: usize) {
    let (u, _) = up_unit(ctx);
    let mut t = [0u8; 128];
    if rdof_mn_topic(cpm(&mut t), t.len(), root(u), kind, dst, node(u)) != 0 {
        publish(u, cp(&t), msg.cast(), len, 0, false);
    }
}

unsafe extern "C" fn up_activity(ctx: *mut c_void, json: *const c_char) {
    activity_json(up_unit(ctx).0, json);
}

unsafe extern "C" fn up_rand(ctx: *mut c_void) -> u32 {
    rnd(up_unit(ctx).0)
}

unsafe extern "C" fn up_wall(ctx: *mut c_void) -> f64 {
    wall_s(up_unit(ctx).0)
}

// ── the downlink ───────────────────────────────────────────────────────────────

/// A new network stream's slot: the controller told of it. None: not carried.
unsafe fn stream_new(u: *mut Unit, b: &RdofNetBurst, id: u32, flags: u8) -> Option<usize> {
    if b.terminator {
        return None; // the end of a stream not carried
    }
    if b.group && (*u).tg_all && !tg_in(&(*u).pol.static_tg, b.dst) {
        // {root}/tg/+/+: only the talkgroups the site's MSs need (all of them when it has too many to tell)
        let mut t = [0u32; TGS + 1];
        let m = t3::rdof_t3_subs((*u).site, t.as_mut_ptr(), TGS + 1);
        if m <= TGS && !t[..m].contains(&b.dst) {
            (*u).n.tg_filtered += 1;
            return None;
        }
    }
    let Some(i) = (0..STREAMS).find(|&i| !(*u).st[i].used) else {
        (*u).n.net_dropped += 1;
        return None;
    };
    let s = addr_of_mut!((*u).st[i]);
    ptr::write_bytes(s, 0, 1);
    (*s).used = true;
    (*s).group = b.group;
    (*s).stream = id;
    (*s).src = b.src;
    (*s).dst = b.dst;
    (*s).last_ms = (*u).now_ms;
    if b.group {
        let fl = if flags & MN_F_EMERGENCY != 0 { t3::F_EMERGENCY } else { 0 };
        t3::rdof_t3_net_group((*u).site, id, b.src, b.dst, fl, (*u).now_ms);
    } else {
        t3::rdof_t3_net_private((*u).site, id, b.src, b.dst, (*u).now_ms);
    }
    drain(u); // a free channel: CH_OPEN at once
    stream_find(u, id)
}

unsafe extern "C" fn down_burst(
    ctx: *mut c_void,
    b: *const RdofNetBurst,
    _path: c_int,
    _dl: bool,
    flags: u8,
    _from: *const c_char,
) {
    let u: *mut Unit = ctx.cast();
    let b = &*b;
    let id = u32::from_be_bytes(b.stream_id);
    let mut f: RdofFrame = core::mem::zeroed();
    rdof_control_to_dt(b.control, &mut f.dt, &mut f.n);
    f.burst = b.burst;
    f.slot = if b.slot != 0 { b.slot } else { 1 };
    f.has_addr = true;
    f.src = b.src;
    f.dst = b.dst;
    f.group = b.group;
    (*u).n.net_in += 1;
    let Some(i) = stream_find(u, id).or_else(|| stream_new(u, b, id, flags)) else { return };
    (*u).st[i].last_ms = (*u).now_ms;
    if !(*u).st[i].ended {
        stream_feed(u, i, &f);
    }
    if b.terminator {
        stream_end(u, i, true);
    }
    drain(u);
}

unsafe extern "C" fn down_setup(ctx: *mut c_void, h: *const MnHdr, s: *const MnSetup, dl: bool, _from: *const c_char) {
    let u: *mut Unit = ctx.cast();
    if !dl {
        return;
    }
    let (h, s) = (&*h, &*s);
    let ans = answer_op(s.op);
    let m = Setup {
        op: s.op,
        reason: s.reason,
        flags: s.flags,
        svc: s.options,
        src: if ans { h.dst } else { h.src },
        dst: if ans { h.src } else { h.dst },
        stream: h.stream,
        r#ref: s.r#ref,
    };
    (*u).n.setups_in += 1;
    t3::rdof_t3_net_setup((*u).site, &m, (*u).now_ms);
    drain(u);
}

// ── provisioning ───────────────────────────────────────────────────────────────

unsafe fn ms_policies(u: *mut Unit) {
    let mut now = [0u32; t3::MS_POLICIES];
    let mut n = 0;
    let nms = ((*u).mc.nms.max(0) as usize).min(MN_CFG_MS);
    for i in 0..nms {
        if n == t3::MS_POLICIES {
            break;
        }
        let m = addr_of!((*u).mc.ms[i]);
        let ntg = ((*m).ntg.max(0) as usize).min(t3::REG_TGS);
        let mut p = MsPolicy {
            enabled: u8::from((*m).enabled),
            private_ok: u8::from((*m).private_),
            wide_private: u8::from((*m).wide_private),
            emergency_ok: u8::from((*m).emergency),
            priority: (*m).priority as u8,
            ntg: ntg as u8,
            ..Default::default()
        };
        p.tg[..ntg].copy_from_slice(&(&(*m).tg)[..ntg]);
        if t3::rdof_t3_ms_policy((*u).site, (*m).id, &p) {
            now[n] = (*m).id;
            n += 1;
        }
    }
    for i in 0..(*u).nms {
        // documents deleted
        let id = (*u).ms_ids[i];
        if !now[..n].contains(&id) {
            t3::rdof_t3_ms_policy((*u).site, id, null());
        }
    }
    (&mut (*u).ms_ids)[..n].copy_from_slice(&now[..n]);
    (*u).nms = n;
}

unsafe fn tg_policies(u: *mut Unit) {
    let mut now = [0u32; t3::TG_POLICIES];
    let mut n = 0;
    let ntg = ((*u).mc.ntg.max(0) as usize).min(MN_CFG_TG);
    for i in 0..ntg {
        if n == t3::TG_POLICIES {
            break;
        }
        let t = addr_of!((*u).mc.tg[i]);
        let (tg, hang_s) = ((*t).tg, (*t).hang_s);
        let h = hang_s * 1000.0;
        let p = TgPolicy {
            hang_ms: if (1000.0..=60000.0).contains(&h) { llround(h) as u32 } else { 0 },
            priority: (*t).priority.clamp(0, 3) as u8,
            ..Default::default()
        };
        if hang_s > 0.0 && p.hang_ms == 0 {
            log(u, |o| {
                o.bytes(b"tier3: cfg/tg/");
                o.int(i64::from(tg), 0);
                o.bytes(b": hang_s ");
                o.fixed(hang_s, 1);
                o.bytes(b" outside 1-60: the site's");
            });
        }
        if t3::rdof_t3_tg_policy((*u).site, tg, &p) {
            now[n] = tg;
            n += 1;
        }
    }
    for i in 0..(*u).ntgp {
        // documents deleted
        let tg = (*u).tg_ids[i];
        if !now[..n].contains(&tg) {
            t3::rdof_t3_tg_policy((*u).site, tg, null());
        }
    }
    (&mut (*u).tg_ids)[..n].copy_from_slice(&now[..n]);
    (*u).ntgp = n;
}

/// The values that apply: cfg/net under cfg/node.
unsafe fn site_cfg(u: *const Unit) -> MnSiteCfg {
    let mut sc: MnSiteCfg = core::mem::zeroed();
    rdof_mn_cfg_site(addr_of!((*u).mc), null(), &mut sc);
    sc
}

/// The adjacent-site list: the other online-or-not trunked nodes of the same MODEL and NET (from their info), only
/// the sites cfg/node's neighbors lists when it gives them.
unsafe fn neighbors(u: *mut Unit) {
    let sc = site_cfg(u);
    let listed = &sc.neighbors[..(sc.nneighbors.max(0) as usize).min(MN_CFG_NEIGHBORS)];
    let mut nb = [Neighbor::default(); t3::NEIGHBORS];
    let mut n = 0;
    let cfg = (*u).cfg;
    for i in 0..(*u).npeers {
        if n == t3::NEIGHBORS {
            break;
        }
        let p = (*u).peers[i];
        if !p.trunk || p.syscode == 0 || p.site == cfg.site {
            continue;
        }
        // same MODEL and NET: their syscode is what ours would be with their SITE and PAR
        if t3::rdof_t3_syscode(cfg.model, cfg.net, p.site, (p.syscode & 3) as u8) != p.syscode {
            continue;
        }
        if !listed.is_empty() && !listed.contains(&c_int::from(p.site)) {
            continue;
        }
        nb[n] = Neighbor {
            syscode: p.syscode,
            chan: p.chan,
            active_known: 1,
            active: u8::from(p.online),
            ..Default::default()
        };
        n += 1;
    }
    t3::rdof_t3_neighbors((*u).site, nb.as_ptr(), n as c_int);
    (*u).nneighbors = n;
}

unsafe fn peer_of(u: *mut Unit, nd: &[u8]) -> Option<usize> {
    if let Some(i) = (0..(*u).npeers).find(|&i| cstr(&(*u).peers[i].node) == nd) {
        return Some(i);
    }
    if (*u).npeers == PEERS {
        return None;
    }
    let i = (*u).npeers;
    (*u).npeers += 1;
    let p = addr_of_mut!((*u).peers[i]);
    ptr::write_bytes(p, 0, 1);
    let n = nd.len().min(NODE_MAX);
    (&mut (*p).node)[..n].copy_from_slice(&nd[..n]);
    Some(i)
}

/// another node's info: its trunk object's system and TSCC channel (none: not a neighbor)
unsafe fn peer_info(u: *mut Unit, nd: &[u8], text: *const c_char, len: usize) {
    const MODEL_NAMES: [&[u8]; 4] = [b"tiny", b"small", b"large", b"huge"];
    let Some(i) = peer_of(u, nd) else { return };
    let mut d = JDoc {
        nodes: addr_of_mut!((*u).jnodes).cast(),
        max: JNODES as c_int,
        n: 0,
        buf: addr_of_mut!((*u).jbuf).cast(),
        cap: JBUF,
        used: 0,
        error: [0; 64],
    };
    (*u).peers[i].trunk = false;
    let root = if len != 0 { rdof_jparse(&mut d, text, len) } else { -1 };
    let t = if root >= 0 { rdof_jget(&d, root, c"trunk".as_ptr()) } else { -1 };
    let sys = if t >= 0 { rdof_jget(&d, t, c"system".as_ptr()) } else { -1 };
    let tscc = if t >= 0 { rdof_jat(&d, rdof_jget(&d, t, c"tscc".as_ptr()), 0) } else { -1 };
    if sys < 0 || tscc < 0 {
        return;
    }
    let get = |obj: c_int, key: &CStr| rdof_jnode(&d, rdof_jget(&d, obj, key.as_ptr())).as_ref();
    let (Some(model), Some(net), Some(site), Some(chan)) =
        (get(sys, c"model"), get(sys, c"net"), get(sys, c"site"), get(tscc, c"chan"))
    else {
        return;
    };
    if model.type_ != RDOF_JSTR || net.type_ != RDOF_JNUM || site.type_ != RDOF_JNUM || chan.type_ != RDOF_JNUM {
        return;
    }
    let m = MODEL_NAMES.iter().position(|&x| x == c_bytes(model.str_));
    let pv = match get(sys, c"par") {
        Some(p) if p.type_ == RDOF_JSTR => match c_bytes(p.str_) {
            b"a" => 1,
            b"b" => 2,
            _ => 3,
        },
        _ => 3, // "ab"
    };
    let Some(m) = m else { return };
    let (net, site, chan) = (net.num, site.num, chan.num);
    if net < 0.0 || net > 65535.0 || site < 1.0 || site > 65535.0 || chan < 1.0 || chan > 4094.0 {
        return;
    }
    let syscode = t3::rdof_t3_syscode(m as u8, net as u16, site as u16, pv);
    let p = addr_of_mut!((*u).peers[i]);
    (*p).syscode = syscode;
    (*p).site = site as u16;
    (*p).chan = chan as u16;
    (*p).trunk = syscode != 0;
}

unsafe fn apply(u: *mut Unit) -> bool {
    let sc = site_cfg(u);
    let mut cfg = (*u).base;
    let mut pol = (*u).base_pol;
    let mut err = [0u8; 120];
    if !rdof_t3cfg_site(&sc, &mut cfg, &mut pol, cpm(&mut err), err.len())
        || !rdof_t3cfg_check(&cfg, cpm(&mut err), err.len())
    {
        log(u, |o| (o.bytes(b"tier3: provisioning refused: "), o.bytes(cstr(&err))).1);
        (*u).n.cfg_bad += 1;
        return false;
    }
    // Cfg and Policy have no implicit padding: their bytes compare as the C's memcmp did
    if bytes_of(&cfg) != bytes_of(&(*u).cfg) {
        if !t3::rdof_t3_reconfigure((*u).site, &cfg, cpm(&mut err), err.len()) {
            log(u, |o| (o.bytes(b"tier3: provisioning refused by the controller: "), o.bytes(cstr(&err))).1);
            (*u).n.cfg_bad += 1;
            return false;
        }
        (*u).cfg = cfg;
    }
    if bytes_of(&pol) != bytes_of(&(*u).pol) {
        t3::rdof_t3_policy((*u).site, &pol);
        (*u).pol = pol;
    }
    let off = sc.has & MN_HAS_ENABLED != 0 && !sc.enabled;
    if off != (*u).withdrawn_cfg {
        t3::rdof_t3_withdraw((*u).site, off);
        (*u).withdrawn_cfg = off;
        let what: &[u8] = if off { b"withdrawn" } else { b"back on the air" };
        log(u, |o| (o.bytes(b"tier3: "), o.bytes(what), o.bytes(b" by provisioning")).2);
    }
    ms_policies(u);
    tg_policies(u);
    neighbors(u);
    (*u).n.cfg_applied += 1;
    drain(u);
    subscribe(u, false);
    true
}

// ── commands ───────────────────────────────────────────────────────────────────

unsafe fn publish_status(u: *mut Unit) {
    let mut t = [0u8; 128];
    let mut j = [0u8; 2048];
    if rdof_mn_node_topic(cpm(&mut t), t.len(), root(u), node(u), c"status".as_ptr()) != 0
        && rdof_t3unit_status_json(u, cpm(&mut j), j.len()) != 0
    {
        publish_str(u, cp(&t), cp(&j), 1, true);
    }
    (*u).status_ms = (*u).now_ms;
}

unsafe fn command(u: *mut Unit, json: *const c_char, len: usize) {
    let mut c: MnCmd = core::mem::zeroed();
    let mut err = [0u8; 96];
    let mut t = [0u8; 128];
    let mut j = [0u8; 256];
    let mut ok = rdof_mn_cmd_parse(json, len, &mut c, cpm(&mut err), err.len());
    (*u).n.cmds += 1;
    if ok {
        let site = (*u).site;
        ok = match c.cmd {
            MN_CMD_DEREGISTER => t3::rdof_t3_command(site, c"deregister".as_ptr(), c.ms, 0),
            MN_CMD_CLEAR => t3::rdof_t3_command(site, c"clear".as_ptr(), c.chan as u32, c.lcn as u32),
            MN_CMD_WITHDRAW => t3::rdof_t3_command(site, c"withdraw".as_ptr(), 0, 0),
            MN_CMD_RESTORE => t3::rdof_t3_command(site, c"restore".as_ptr(), 0, 0),
            MN_CMD_RESEND_RECORDS => t3::rdof_t3_command(site, c"resend_records".as_ptr(), 0, 0),
            MN_CMD_STATUS => true,
            _ => false,
        };
        if !ok {
            let name: &[u8] = if c.name[0] != 0 { cstr(&c.name) } else { b"command" };
            put(&mut err, |o| (o.bytes(name), o.bytes(b": refused")).1);
        }
        drain(u);
        if c.cmd == MN_CMD_STATUS {
            publish_status(u);
        }
    }
    if rdof_mn_node_topic(cpm(&mut t), t.len(), root(u), node(u), c"cmd/res".as_ptr()) != 0
        && rdof_mn_cmd_result_json(cp(&c.id), ok, if ok { null() } else { cp(&err) }, cpm(&mut j), j.len()) != 0
    {
        publish_str(u, cp(&t), cp(&j), 1, false);
    }
}

// ── the API (rdof/t3unit.h) ────────────────────────────────────────────────────

#[no_mangle]
pub extern "C" fn rdof_t3unit_size() -> usize {
    mem_plan().3
}

/// A C string copied into a fixed array, cut to fit (snprintf "%s"); NULL: dflt.
unsafe fn set_text(dst: &mut [u8], s: *const c_char, dflt: &[u8]) {
    let s = if s.is_null() { dflt } else { c_bytes(s) };
    put(dst, |o| o.bytes(s));
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3unit_init(
    u: *mut Unit,
    site_mem: *mut c_void,
    cfg: *const Cfg,
    pol: *const Policy,
    io: *const Io,
    root_s: *const c_char,
    node_s: *const c_char,
    path: c_int,
    firmware: *const c_char,
    err: *mut c_char,
    cap: usize,
) -> bool {
    let (up0, stride, down, total, align) = mem_plan();
    if !(u as usize).is_multiple_of(align) {
        c_out(err, cap, |o| {
            o.bytes(b"tier3: the unit's memory isn't aligned to ");
            o.int(align as i64, 0);
        });
        return false;
    }
    ptr::write_bytes(u.cast::<u8>(), 0, total);
    let cfg = &*cfg;
    (*u).io = *io;
    (*u).site = site_mem.cast();
    (*u).base = *cfg;
    (*u).cfg = *cfg;
    (*u).base_pol = *pol;
    (*u).pol = *pol;
    (*u).path = path;
    (*u).k_next = -1;
    for i in 0..RING {
        (*u).ring[i].k = -1;
    }
    set_text(&mut (*u).firmware, firmware, b"rdofradio");
    (*u).net = !root_s.is_null() && !node_s.is_null() && *root_s != 0 && rdof_mn_node_ok(node_s);
    set_text(&mut (*u).root, root_s, b"");
    set_text(&mut (*u).node, node_s, b"");
    if !t3::rdof_t3_init((*u).site, cfg, err, cap) {
        return false;
    }
    t3::rdof_t3_policy((*u).site, pol);
    let rio = T3rfIo {
        tscc: Some(io_tscc),
        cach: Some(io_cach),
        pdu: Some(io_pdu),
        bad: Some(io_bad),
        activity: Some(io_activity),
        uplink: Some(io_uplink),
        ctx: u.cast(),
    };
    let tscc = cfg.carriers[0].tscc != 0;
    if !rdof_t3rf_init(rf(u), &rio, c_int::from(cfg.ncarriers), c_int::from(cfg.cc), tscc) {
        c_out(err, cap, |o| {
            o.bytes(b"tier3: ");
            o.int(i64::from(cfg.ncarriers), 0);
            o.bytes(b" carriers: the radio layer takes 1 to ");
            o.int(crate::t3rf::CARRIERS as i64, 0);
        });
        return false;
    }
    (*u).rf_tscc = tscc;
    rdof_mn_cfg_init(addr_of_mut!((*u).mc), node(u));
    for c in 0..CARRIERS {
        let x: *mut MnUpHead = u.cast::<u8>().add(up0 + c * stride).cast();
        (*u).up[c] = x;
        rdof_mn_up_init(x, node(u));
        (*u).upctx[c] = UpCtx { u, c: c as c_int };
        (*x).path_of = Some(up_path_of);
        (*x).admit = Some(up_admit);
        (*x).publish = Some(up_publish);
        (*x).activity = Some(up_activity);
        (*x).rand32 = Some(up_rand);
        (*x).wall = Some(up_wall);
        (*x).ctx = addr_of_mut!((*u).upctx[c]).cast();
        if (0..64).contains(&path) {
            (*x).tx_ok = 1u64 << path;
        }
    }
    let d: *mut MnDownHead = u.cast::<u8>().add(down).cast();
    (*u).down = d;
    rdof_mn_down_init(d);
    (*d).burst = Some(down_burst);
    (*d).setup = Some(down_setup);
    (*d).ctx = u.cast();
    drain(u);
    true
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3unit_run(u: *mut Unit, k_upto: i64, now_ms: i64) {
    (*u).now_ms = now_ms;
    if (*u).start_ms == 0 {
        (*u).start_ms = now_ms;
    }
    if (*u).k_next < 0 || k_upto - (*u).k_next > RING as i64 * 8 {
        (*u).k_next = k_upto; // the first slot, or the clock jumped: start again there
    }
    while (*u).k_next <= k_upto {
        let k = (*u).k_next;
        let r = addr_of_mut!((*u).ring[k.rem_euclid(RING as i64) as usize]);
        t3::rdof_t3_slot((*u).site, k, now_ms, addr_of_mut!((*r).o));
        (*r).k = k;
        for c in 0..usize::from((*u).cfg.ncarriers) {
            let ch = 2 * c + (k & 1) as usize;
            if ch == 0 && (*u).rf_tscc {
                continue;
            }
            let o = addr_of!((*r).o.ch[ch]);
            if (*o).dt != 0 {
                rdof_t3rf_control(rf(u), ch as c_int, c_int::from((*o).dt), &(*o).pdu, 1, false);
            }
        }
        drain(u);
        rdof_t3rf_tick(rf(u), k);
        drain(u);
        (*u).k_next += 1;
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3unit_slot(
    u: *mut Unit,
    c: c_int,
    k: i64,
    burst: *mut u8,
    control: *mut u8,
    at: *mut c_int,
    slc28: *mut i32,
) -> bool {
    if (*u).k_next <= k {
        rdof_t3unit_run(u, k, (*u).now_ms);
    }
    let ok = rdof_t3rf_slot(rf(u), c, k, &mut *burst.cast(), control, &mut *at, &mut *slc28);
    drain(u);
    ok
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3unit_split_pump(u: *mut Unit, o: *mut T3rfSplit, s: *mut c_void, k_now: i64, now: f64) {
    if (*u).k_next <= k_now + 4 {
        rdof_t3unit_run(u, k_now + 4, (*u).now_ms);
    }
    rdof_t3rf_split_pump(rf(u), o, s, k_now, 0, now);
    drain(u);
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3unit_at(u: *const Unit, k: i64) -> c_int {
    let o = held(u, k);
    if o.is_null() {
        -1
    } else {
        c_int::from((*o).at)
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3unit_rx(u: *mut Unit, c: c_int, k: i64, b: *const RdofBurst, rssi: c_int) {
    rdof_t3rf_rx(rf(u), c, k, b, rssi);
    drain(u);
}

/// {root}/node/{who}/info or /online from another node: the neighbor list.
unsafe fn node_message(u: *mut Unit, who: &[u8], info: bool, text: *const c_char, len: usize) {
    if who == cstr(&(*u).node) {
        return;
    }
    if info {
        peer_info(u, who, text, len);
    } else if let Some(i) = peer_of(u, who) {
        (*u).peers[i].online = len == 1 && *text == b'1' as c_char;
    }
    neighbors(u);
    drain(u);
}

/// {root}/node/{node}/ms/{id}: a registration record of ours, read back (after a restart).
unsafe fn record_in(u: *mut Unit, leaf: &[u8], text: *const c_char, len: usize) {
    let mut m: MnRecord = core::mem::zeroed();
    let mut err = [0u8; 96];
    if !rdof_mn_record_parse(text, len, &mut m, cpm(&mut err), err.len()) {
        log(u, |o| (o.bytes(b"tier3: "), o.bytes(leaf), o.bytes(b": "), o.bytes(cstr(&err))).3);
        return;
    }
    let ntg = (m.ntg.max(0) as usize).min(t3::REG_TGS);
    let mut r = Record {
        id: m.id,
        sub_tg: m.sub,
        how: t3::HOW_LOADED,
        area: if m.area >= 0 { m.area as u8 } else { 0 },
        ntg: ntg as u8,
        at_ms: m.at_ms,
        exp_ms: m.exp_ms,
        ..Default::default()
    };
    r.tg[..ntg].copy_from_slice(&m.tg[..ntg]);
    let mut have = Record::default();
    if t3::rdof_t3_record((*u).site, m.id, &mut have) && have.at_ms >= m.at_ms {
        return; // ours is as new: this is the one we published
    }
    if t3::rdof_t3_load_record((*u).site, &r) {
        (*u).n.records_in += 1;
    }
    drain(u);
    subscribe(u, false);
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3unit_message(u: *mut Unit, topic: *const c_char, data: *const c_void, len: usize) {
    if !(*u).net {
        return;
    }
    let text: *const c_char = data.cast();
    let mut id = 0u32;
    let mut nd = [0u8; NODE_MAX + 1];
    let mut note = [0u8; 96];
    let mut err = [0u8; 96];
    let kind = rdof_mn_topic_parse(topic, root(u), &mut id, cpm(&mut nd), nd.len());
    let me = cstr(&(*u).node);
    if kind == MN_T_DL {
        if cstr(&nd) == me {
            rdof_mn_down_message((*u).down, c"router".as_ptr(), true, data.cast(), len, wall_s(u));
        }
        return;
    }
    if kind == MN_T_TG {
        if cstr(&nd) != me {
            rdof_mn_down_message((*u).down, cp(&nd), false, data.cast(), len, wall_s(u));
        }
        return;
    }
    if kind != MN_T_NONE {
        return;
    }
    let tb = CStr::from_ptr(topic).to_bytes();
    let rt = cstr(&(*u).root);
    if !tb.starts_with(rt) || tb.get(rt.len()) != Some(&b'/') {
        return;
    }
    let leaf = &tb[rt.len() + 1..];
    if let Some(rest) = leaf.strip_prefix(b"node/") {
        if let Some(sl) = rest.iter().position(|&c| c == b'/') {
            let what = &rest[sl..];
            if sl <= NODE_MAX && (what == b"/info" || what == b"/online") {
                node_message(u, &rest[..sl], what == b"/info", text, len);
                return;
            }
        }
    }
    if leaf.starts_with(b"cfg/") {
        let r = rdof_mn_cfg_message(addr_of_mut!((*u).mc), root(u), topic, text, len, cpm(&mut note), note.len());
        if r == MN_CFG_APPLIED || r == MN_CFG_DELETED {
            if note[0] != 0 {
                log(u, |o| (o.bytes(b"tier3: "), o.bytes(leaf), o.bytes(b": "), o.bytes(cstr(&note))).3);
            }
            apply(u);
            if let Some(cache) = (*u).io.cache {
                cache((*u).io.ctx);
            }
        } else if r == MN_CFG_BAD || r == MN_CFG_FULL {
            let what: &[u8] = if r == MN_CFG_FULL { b"no room" } else { b"malformed" };
            log(u, |o| (o.bytes(b"tier3: "), o.bytes(leaf), o.bytes(b": "), o.bytes(what)).3);
            (*u).n.cfg_bad += 1;
        }
        return;
    }
    if rdof_mn_ms_topic_parse(topic, root(u), cpm(&mut nd), nd.len(), &mut id) {
        if cstr(&nd) == me && len != 0 {
            record_in(u, leaf, text, len); // not another node's, nor a deletion
        }
        return;
    }
    if rdof_mn_where_topic_parse(topic, root(u), &mut id) {
        let mut w: MnWhere = core::mem::zeroed();
        if len != 0 && rdof_mn_where_parse(text, len, &mut w, cpm(&mut err), err.len()) {
            t3::rdof_t3_where((*u).site, id, cstr(&w.node) == me, w.at_ms);
            drain(u);
        }
        return;
    }
    let mut cmd = [0u8; 128];
    put(&mut cmd, |o| (o.bytes(b"node/"), o.bytes(me), o.bytes(b"/cmd")).2);
    if leaf == cstr(&cmd) {
        command(u, text, len);
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3unit_link(u: *mut Unit, up: bool) {
    (*u).connected = up;
    t3::rdof_t3_net_link((*u).site, up);
    if up {
        subscribe(u, true);
        t3::rdof_t3_command((*u).site, c"resend_records".as_ptr(), 0, 0);
        drain(u);
        publish_status(u);
    } else {
        drain(u);
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3unit_tick(u: *mut Unit, now_ms: i64) {
    (*u).now_ms = now_ms;
    if (*u).start_ms == 0 {
        (*u).start_ms = now_ms;
    }
    let now = wall_s(u);
    rdof_mn_down_tick((*u).down, now);
    for c in 0..usize::from((*u).cfg.ncarriers).min(CARRIERS) {
        rdof_mn_up_tick((*u).up[c], now);
    }
    for i in 0..STREAMS {
        // a stream gone quiet without its terminator
        if (*u).st[i].used && now_ms - (*u).st[i].last_ms > 3000 {
            stream_end(u, i, false);
        }
    }
    drain(u);
    if (*u).connected && now_ms - (*u).status_ms >= STATUS_S * 1000 {
        publish_status(u);
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3unit_apply(u: *mut Unit) -> bool {
    apply(u)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3unit_trunk_json(u: *const Unit, out: *mut c_char, cap: usize) -> usize {
    const MODELS: [&CStr; 4] = [c"tiny", c"small", c"large", c"huge"];
    const PAR: [&CStr; 4] = [c"?", c"a", c"b", c"ab"];
    let services = [c"reg".as_ptr(), c"group".as_ptr(), c"private".as_ptr(), c"allcall".as_ptr()];
    let c = &(*u).cfg;
    let mut car = [MnCarrier::default(); CARRIERS];
    let mut tscc = [MnCarrier::default(); 1];
    let mut ntscc = 0;
    for (o, k) in car.iter_mut().zip(c.carriers.iter()).take(usize::from(c.ncarriers)) {
        *o = MnCarrier { chan: c_int::from(k.chan), tx_hz: k.tx_hz, rx_hz: k.rx_hz, cc: -1 };
        if k.tscc != 0 && ntscc == 0 {
            tscc[0] = MnCarrier { cc: c_int::from(c.cc), ..*o };
            ntscc = 1;
        }
    }
    let t = MnTrunkInfo {
        model: MODELS[usize::from(c.model & 3)].as_ptr(),
        net: c_int::from(c.net),
        site: c_int::from(c.site),
        area_bits: c_int::from(c.area_bits),
        par: PAR[usize::from(c.par & 3)].as_ptr(),
        tscc: tscc.as_ptr(),
        ntscc,
        carriers: car.as_ptr(),
        ncarriers: c_int::from(c.ncarriers),
        services: services.as_ptr(),
        nservices: 4,
        reg: c.reg_required != 0,
    };
    rdof_mn_trunk_info_json(&t, out, cap)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3unit_status_json(u: *const Unit, out: *mut c_char, cap: usize) -> usize {
    const USE: [&CStr; 6] = [c"off", c"idle", c"tscc", c"call", c"hang", c"hang"];
    let mut s = Status::default();
    t3::rdof_t3_status((*u).site, &mut s);
    let blank = MnChanStatus { chan: 0, lcn: 0, r#use: null(), src: 0, dst: 0, group: false, since_ms: 0 };
    let mut ch = [blank; CHANNELS];
    let n = usize::from(s.nch).min(CHANNELS);
    for (o, x) in ch.iter_mut().zip(s.ch.iter()).take(n) {
        let busy = x.r#use == t3::USE_CALL || x.r#use == t3::USE_HANG || x.r#use == t3::USE_CLEAR;
        *o = MnChanStatus {
            chan: c_int::from(x.chan),
            lcn: c_int::from(x.lcn),
            r#use: USE[if x.r#use <= 5 { usize::from(x.r#use) } else { 0 }].as_ptr(),
            src: if busy { x.src } else { 0 },
            dst: if busy { x.dst } else { 0 },
            group: x.flags & t3::F_GROUP != 0,
            since_ms: if busy { x.since_ms } else { 0 },
        };
    }
    let t = MnTrunkStatus {
        connected: s.connected != 0,
        regs: s.regs as c_int,
        queue: s.queue as c_int,
        ch: ch.as_ptr(),
        nch: n as c_int,
        aloha: s.aloha,
        rand: s.rand,
        collisions: s.collisions,
        grants: s.grants,
        refused: s.refused,
        queued: s.queued,
    };
    let st = MnStatus {
        node: node(u),
        firmware: addr_of!((*u).firmware).cast(),
        wall: wall_s(u),
        uptime_s: ((*u).now_ms - (*u).start_ms) as f64 / 1000.0,
        trunk: &t,
    };
    rdof_mn_status_json(&st, out, cap)
}

// ── accessors ──────────────────────────────────────────────────────────────────

#[no_mangle]
pub unsafe extern "C" fn rdof_t3unit_set_now(u: *mut Unit, now_ms: i64) {
    (*u).now_ms = now_ms;
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3unit_cfg(u: *const Unit) -> *const Cfg {
    addr_of!((*u).cfg)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3unit_mc(u: *mut Unit) -> *mut MnCfg {
    addr_of_mut!((*u).mc)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3unit_rf(u: *const Unit) -> *const T3rf {
    addr_of!((*u).rf)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3unit_counts(u: *const Unit) -> *const Counts {
    addr_of!((*u).n)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3unit_peers(u: *const Unit, n: *mut c_int) -> *const Peer {
    if !n.is_null() {
        *n = (*u).npeers as c_int;
    }
    addr_of!((*u).peers).cast()
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3unit_neighbors(u: *const Unit) -> c_int {
    (*u).nneighbors as c_int
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3unit_tg_policies(u: *const Unit, ids: *mut *const u32) -> c_int {
    if !ids.is_null() {
        *ids = addr_of!((*u).tg_ids).cast();
    }
    (*u).ntgp as c_int
}

// ── layouts (rdof_t3unit_layout) ───────────────────────────────────────────────

pub use crate::t3site::Layout;

macro_rules! layout {
    ($name:literal, $size:expr, $align:expr) => {
        Layout { name: concat!($name, "\0").as_ptr().cast(), size: $size, align: $align }
    };
    ($name:literal, $t:ty) => {
        layout!($name, size_of::<$t>(), align_of::<$t>())
    };
}

/// The mqtt-net heads: to the end of ctx (what the Rust touches); the C side gives offsetof(ctx) + sizeof(void *).
static LAYOUTS: [Layout; 22] = [
    layout!("rdof_t3unit_peer_t", Peer),
    layout!("rdof_t3unit_io_t", Io),
    layout!("rdof_t3unit_counts_t", Counts),
    layout!("rdof_mn_hdr_t", MnHdr),
    layout!("rdof_mn_setup_t", MnSetup),
    layout!("rdof_mn_ev_t", MnEv),
    layout!("rdof_mn_record_t", MnRecord),
    layout!("rdof_mn_where_t", MnWhere),
    layout!("rdof_mn_chan_status_t", MnChanStatus),
    layout!("rdof_mn_trunk_status_t", MnTrunkStatus),
    layout!("rdof_mn_status_t", MnStatus),
    layout!("rdof_mn_carrier_t", MnCarrier),
    layout!("rdof_mn_trunk_info_t", MnTrunkInfo),
    layout!("rdof_mn_timer_t", MnTimer),
    layout!("rdof_mn_site_cfg_t", MnSiteCfg),
    layout!("rdof_mn_tg_cfg_t", MnTgCfg),
    layout!("rdof_mn_ms_cfg_t", MnMsCfg),
    layout!("rdof_mn_cfg_t", MnCfg),
    layout!("rdof_mn_cmd_t", MnCmd),
    layout!("rdof_mn_up_t head", offset_of!(MnUpHead, ctx) + size_of::<*mut c_void>(), align_of::<MnUpHead>()),
    layout!("rdof_mn_down_t head", offset_of!(MnDownHead, ctx) + size_of::<*mut c_void>(), align_of::<MnDownHead>()),
    Layout { name: null(), size: 0, align: 0 },
];

#[no_mangle]
pub extern "C" fn rdof_t3unit_layout(i: usize) -> *const Layout {
    LAYOUTS.get(i).map_or(null(), |l| l as *const Layout)
}
