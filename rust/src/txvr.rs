// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! The roaming transceiver (rdof/txvr.h): a port of common/src/txvr.c.
//!
//! An rdof_txvr_t is shared by three tasks: HBlink4's (rdof_txvr_on_tune, rdof_txvr_on_burst), the modem reader's
//! (rdof_txvr_heard) and the transceiver's own (rdof_txvr_poll). So, as in modem.rs, the exported functions work through
//! the raw pointer and never make a `&mut` of the whole struct; `tuning` (volatile in the C) is read and written
//! volatile. The call's queue is only touched under the platform's lock, as in the C.

use core::ffi::{c_char, c_int, c_void, CStr};
use core::ptr::{addr_of, addr_of_mut, null, read_volatile, write_volatile};

use crate::bits::rdof_get_field;
use crate::ffi::{self, RDOF_BURST_BYTES, RDOF_DT_CSBK, RDOF_DT_DATA_HEADER, RDOF_DT_RATE_12, RDOF_DT_TERMINATOR_WITH_LC, RDOF_DT_VOICE_LC_HEADER};
use crate::hblink::{rdof_dmrk_packet, RdofNetBurst, RdofTune, RDOF_DMRK_LEN};
use crate::jitter::{
    rdof_jb_arrival, rdof_jb_begin, rdof_jb_drop, rdof_jb_end, rdof_jb_fill, rdof_jb_init, rdof_jb_key, rdof_jb_prebuffer,
    rdof_jb_reframe, rdof_jb_silent, rdof_jb_underrun, RdofJb, RdofJbStream,
};
use crate::layer2::{rdof_decode_emb, rdof_group_lc, rdof_lc_burst, RDOF_DATA_XOR, RDOF_MS_VOICE};
use crate::mmdvm::{rdof_csbk_preambles, rdof_regenerate, rdof_silence_burst, RdofMmdvmStatus, RDOF_SYNC_DATA};
use crate::modem::{
    rdof_modem_send_cwid, rdof_modem_set_mode, rdof_modem_status, rdof_modem_write_dmr, rdof_radio_rssi, rdof_radio_tune, RdofModem,
    RdofRadio,
};
use crate::netcalls::RdofTxCallInfo;
use crate::pdu::{rdof_decode_csbk, rdof_decode_data_header, RdofCsbk, RdofDataHeader};

type Burst = [u8; RDOF_BURST_BYTES];

const FREQ_TOLERANCE_HZ: u32 = 100;
const ORPHAN_S: f64 = 0.5;
const STREAM_TIMEOUT_S: f64 = 1.5;
const STATUS_EVERY_S: f64 = 0.06;
const BURST_S: f64 = 0.06;
const IDLE_STATUS_EVERY_S: f64 = 2.0;
const STATUS_GIVE_UP_S: f64 = 2.0;
const TX_TAIL_S: f64 = 0.5;
const RX_HOLD_S: f64 = 1.0;
const CALL_TAIL_S: f64 = 0.5;

pub const RDOF_TXVR_QUEUE: usize = 256;
const ORPHANS: usize = 4;
const ORPHAN_BURSTS: usize = 16;
const REFUSED: usize = 64;
pub const RDOF_TXVR_MAX_CHANNELS: usize = 32;
const MAX_COVERED: usize = 32;
const IDS: usize = 16;

const TMS_PORT: u16 = 4007;
const TMS_ACK_REQUESTED: u8 = 0x40;
const TMS_CONTROL: u8 = 0x10;
const RDOF_SYNC_AUDIO: u8 = 0x20;
const CSBK_CONTROL: u8 = RDOF_SYNC_DATA | RDOF_DT_CSBK as u8;
const HEADER_CONTROL: u8 = RDOF_SYNC_DATA | RDOF_DT_VOICE_LC_HEADER as u8;
const TERMINATOR_CONTROL: u8 = RDOF_SYNC_DATA | RDOF_DT_TERMINATOR_WITH_LC as u8;
const RDOF_DPF_UNCONFIRMED: c_int = 2;
const RDOF_DPF_CONFIRMED: c_int = 3;
const MODE_IDLE: c_int = 0;
const MODE_DMR: c_int = 2;

// rdof/hblink.h's DMRK statuses
const DMRK_ON_AIR: c_int = 0;
const DMRK_CHANNEL_BUSY: c_int = 1;
const DMRK_NOT_ALLOWED: c_int = 2;
const DMRK_RADIO_ERROR: c_int = 3;
const DMRK_TXVR_BUSY: c_int = 4;

// rdof_txcall_state_t
const CALL_PENDING: c_int = 0;
const CALL_LISTENING: c_int = 1;
const CALL_ON_AIR: c_int = 2;
const CALL_DONE: c_int = 3;

/// rdof_receiver_t
#[repr(C)]
pub struct RdofReceiver {
    pub hear: Option<
        unsafe extern "C" fn(
            ctx: *mut c_void,
            slot: c_int,
            control: u8,
            burst: *const u8,
            dbm: f64,
            cc: c_int,
            freq: u32,
        ),
    >,
    pub tick: Option<unsafe extern "C" fn(ctx: *mut c_void)>,
    pub ctx: *mut c_void,
}

/// rdof_channel_t
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RdofChannel {
    pub freq: u32,
    pub cc: c_int,
    pub power: c_int,
}

/// rdof_scan_t
#[repr(C)]
pub struct RdofScan {
    pub channels: [RdofChannel; RDOF_TXVR_MAX_CHANNELS],
    pub nchannels: c_int,
    pub dwell_s: f64,
    pub hang_s: f64,
    pub carrier_s: f64,
}

/// rdof_txcall_t
#[repr(C)]
pub struct RdofTxcall {
    pub tune: RdofTune,
    pub state: c_int,
    pub bursts: [RdofNetBurst; RDOF_TXVR_QUEUE],
    pub head: c_int,
    pub count: c_int,
    pub dropped: c_int,
    pub ended: bool,
    pub last_burst: f64,
    pub listen_until: f64,
    pub quiet_since: f64,
    pub started: bool,
    pub extra: c_int,
    pub headers: c_int,
    pub terminators: c_int,
    pub vpos: c_int,
    pub vfilled: c_int,
    pub frags: [[u8; 32]; 5],
    pub has_frag: u8,
    pub has_quiet_at: bool,
    pub quiet_at: f64,
    pub src: u32,
    pub dst: u32,
    pub group: bool,
    pub on_air_at: f64,
    pub has_csbk: bool,
    pub csbk: Burst,
    pub wants_reply: bool,
    pub collecting_udata: bool,
    pub udata: [u8; 48],
    pub udata_len: c_int,
    pub has_lc: bool,
    pub lc9: [u8; 9],
    pub vshift: c_int,
    pub vpi: c_int,
    pub air_seen: f64,
    pub jbs: RdofJbStream,
}

#[repr(C)]
pub struct RdofOrphan {
    pub used: bool,
    pub stream_id: [u8; 4],
    pub at: f64,
    pub bursts: [RdofNetBurst; ORPHAN_BURSTS],
    pub n: c_int,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct RdofLastId {
    pub freq: u32,
    pub at: f64,
}

/// rdof_txvr_t
#[repr(C)]
pub struct RdofTxvr {
    pub radio: *mut RdofRadio,
    pub peer_id: u32,
    pub channels: [RdofChannel; RDOF_TXVR_MAX_CHANNELS],
    pub nchannels: c_int,
    pub listen_s: f64,
    pub clear_s: f64,
    pub prebuffer: c_int,
    pub csbk_preambles: c_int,
    pub answer_s: f64,
    pub answer_max_s: f64,
    pub covered: [u32; MAX_COVERED],
    pub ncovered: c_int,
    pub has_idle: bool,
    pub idle: RdofChannel,
    pub idle_after_s: f64,
    pub cwid: [c_char; 41],
    pub cwid_every_s: f64,
    pub has_scan: bool,
    pub scan: RdofScan,

    pub send: Option<unsafe extern "C" fn(ctx: *mut c_void, data: *const u8, len: usize)>,
    pub on_retune: Option<unsafe extern "C" fn(ctx: *mut c_void, freq: u32, cc: c_int)>,
    pub on_tx: Option<
        unsafe extern "C" fn(ctx: *mut c_void, action: *const c_char, call: *const RdofTxCallInfo, duration: f64),
    >,
    pub on_sent: Option<unsafe extern "C" fn(ctx: *mut c_void, call: *const RdofTxCallInfo, b: *const RdofNetBurst)>,
    pub on_scan: Option<unsafe extern "C" fn(ctx: *mut c_void, holding: bool)>,
    pub lock: Option<unsafe extern "C" fn(ctx: *mut c_void)>,
    pub unlock: Option<unsafe extern "C" fn(ctx: *mut c_void)>,
    pub ctx: *mut c_void,
    pub receiver: RdofReceiver,
    pub clock: Option<unsafe extern "C" fn(ctx: *mut c_void) -> f64>,

    pub has_answer_window: bool,
    pub aw_freq: u32,
    pub aw_cc: c_int,
    pub aw_until: f64,
    pub aw_cap: f64,
    pub scan_next: c_int,
    pub carrier_checked: bool,
    pub stopped: bool,
    pub has_heard_call: bool,
    pub heard_call_ended: bool,
    pub heard_src: u32,
    pub heard_dst: u32,
    pub heard_at: f64,
    pub has_tuned: bool,
    pub tuned_freq: u32,
    pub tuned_cc: c_int,
    pub tuned_power: c_int,
    pub tuned_at: f64,
    pub tuning: bool,
    pub has_call: bool,
    pub call: RdofTxcall,
    pub ready: bool,
    pub idle_since: f64,
    pub orphans: [RdofOrphan; ORPHANS],
    pub refused: [[u8; 4]; REFUSED],
    pub nrefused: c_int,
    pub refused_next: c_int,
    pub last_status: f64,
    pub has_status_missed: bool,
    pub status_missed: f64,
    pub space: c_int,
    pub air_until: f64,
    pub last_id: [RdofLastId; IDS],
    pub nlast_id: c_int,
    pub status_misses: u32,
    pub jb: RdofJb,
}

/// A count from the struct, as far as its array goes.
fn upto(n: c_int, max: usize) -> usize {
    (n.max(0) as usize).min(max)
}

unsafe fn now(t: *const RdofTxvr) -> f64 {
    match (*t).clock {
        Some(f) => f((*t).ctx),
        None => 0.0,
    }
}

unsafe fn lock(t: *const RdofTxvr) {
    if let Some(f) = (*t).lock {
        f((*t).ctx);
    }
}

unsafe fn unlock(t: *const RdofTxvr) {
    if let Some(f) = (*t).unlock {
        f((*t).ctx);
    }
}

fn near(a: u32, b: u32) -> bool {
    a.abs_diff(b) <= FREQ_TOLERANCE_HZ
}

unsafe fn modem(t: *const RdofTxvr) -> *mut RdofModem {
    (*(*t).radio).modem
}

unsafe fn tuning(t: *const RdofTxvr) -> bool {
    read_volatile(addr_of!((*t).tuning))
}

unsafe fn set_tuning(t: *mut RdofTxvr, v: bool) {
    write_volatile(addr_of_mut!((*t).tuning), v)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_scan_defaults(s: *mut RdofScan) {
    s.write_bytes(0, 1);
    (*s).dwell_s = 0.45;
    (*s).hang_s = 3.0;
}

#[no_mangle]
pub unsafe extern "C" fn rdof_txvr_init(t: *mut RdofTxvr, radio: *mut RdofRadio, peer_id: u32) {
    t.write_bytes(0, 1);
    (*t).radio = radio;
    (*t).peer_id = peer_id;
    (*t).listen_s = 0.3;
    (*t).clear_s = 2.0;
    (*t).prebuffer = 6;
    (*t).csbk_preambles = 2;
    (*t).answer_s = 0.75;
    (*t).answer_max_s = 5.0;
    (*t).cwid_every_s = 900.0;
    (*t).tuned_at = f64::NEG_INFINITY;
    (*t).air_until = f64::NEG_INFINITY;
    (*t).last_status = f64::NEG_INFINITY;
    rdof_jb_init(&mut (*t).jb);
}

// ── queue ──

fn push(c: &mut RdofTxcall, b: &RdofNetBurst) {
    if c.count >= RDOF_TXVR_QUEUE as c_int {
        c.dropped = c.dropped.wrapping_add(1);
        return;
    }
    c.bursts[(c.head + c.count) as usize % RDOF_TXVR_QUEUE] = *b;
    c.count += 1;
}

fn pop(c: &mut RdofTxcall) -> RdofNetBurst {
    let b = c.bursts[c.head as usize];
    c.head = (c.head + 1) % RDOF_TXVR_QUEUE as c_int;
    c.count -= 1;
    b
}

// ── from the HBlink4 task ──

#[no_mangle]
pub unsafe extern "C" fn rdof_txvr_channel_now(t: *const RdofTxvr, freq: &mut u32, cc: &mut c_int) {
    let tuned = (*t).has_tuned;
    *freq = if tuned { (*t).tuned_freq } else { 0 };
    *cc = if tuned { (*t).tuned_cc } else { 0 };
}

unsafe fn answer(t: *mut RdofTxvr, tune: &RdofTune, status: c_int) {
    if status != DMRK_ON_AIR {
        let k = (*t).refused_next as usize;
        (*t).refused[k] = tune.stream_id;
        (*t).refused_next = ((k + 1) % REFUSED) as c_int;
        if (*t).nrefused < REFUSED as c_int {
            (*t).nrefused += 1;
        }
        for o in (*t).orphans.iter_mut() {
            if o.used && o.stream_id == tune.stream_id {
                o.used = false;
            }
        }
    }
    let mut pkt = [0u8; RDOF_DMRK_LEN];
    rdof_dmrk_packet((*t).peer_id, &tune.stream_id, status, tune.freq, tune.cc, &mut pkt);
    if let Some(f) = (*t).send {
        f((*t).ctx, pkt.as_ptr(), pkt.len());
    }
}

unsafe fn allowed(t: *const RdofTxvr, freq: u32, cc: c_int) -> Option<RdofChannel> {
    let n = upto((*t).nchannels, RDOF_TXVR_MAX_CHANNELS);
    (&(*t).channels)[..n].iter().find(|ch| near(ch.freq, freq) && ch.cc == cc).copied()
}

/// _tms_wants_ack
fn tms_wants_ack(ip: &[u8]) -> bool {
    if ip.len() < 20 || ip[0] >> 4 != 4 || ip[9] != 17 {
        return false;
    }
    let ihl = usize::from(ip[0] & 0x0F) * 4;
    if ip.len() < ihl + 8 + 3 {
        return false;
    }
    if u16::from_be_bytes([ip[ihl + 2], ip[ihl + 3]]) != TMS_PORT {
        return false;
    }
    let first = ip[ihl + 8 + 2];
    first & TMS_ACK_REQUESTED != 0 && first & TMS_CONTROL == 0
}

/// _asks_reply
unsafe fn asks_reply(c: &mut RdofTxcall, b: &RdofNetBurst) -> bool {
    if b.control & RDOF_SYNC_DATA == 0 {
        return false;
    }
    let dt = c_int::from(b.control & 0x0F);
    if dt == RDOF_DT_CSBK {
        let mut cs = RdofCsbk::default();
        rdof_decode_csbk(&b.burst, &mut cs);
        return cs.crc_ok
            && (cs.fid == 0x00 || cs.fid == 0x10)
            && ((cs.opcode == 0x24 && cs.raw[3] == 0x80) || cs.opcode == 0x1F);
    }
    if dt == RDOF_DT_DATA_HEADER {
        let mut h: RdofDataHeader = core::mem::zeroed();
        rdof_decode_data_header(&b.burst, &mut h);
        if !h.crc_ok {
            return false;
        }
        if h.dpf == RDOF_DPF_CONFIRMED && h.response_requested {
            return true;
        }
        if h.dpf == RDOF_DPF_UNCONFIRMED && h.sap == 4 {
            // SAP_IP
            c.collecting_udata = true;
            c.udata_len = 0;
        }
        return false;
    }
    if dt == RDOF_DT_RATE_12 && c.collecting_udata && c.udata_len < 48 {
        let mut block = [0u8; 12];
        ffi::rdof_bptc196_decode(b.burst.as_ptr(), block.as_mut_ptr());
        let at = c.udata_len as usize;
        let n = (48 - at).min(12);
        c.udata[at..at + n].copy_from_slice(&block[..n]);
        c.udata_len += 12; // the length grows by 12 each time
        return tms_wants_ack(&c.udata[..upto(c.udata_len, 48)]);
    }
    false
}

/// The embedded field last heard for voice position n, or null.
fn frag(c: &RdofTxcall, n: c_int) -> *const u8 {
    if (1..=4).contains(&n) && (c.has_frag >> n) & 1 != 0 {
        c.frags[n as usize].as_ptr()
    } else {
        null()
    }
}

/// Silence for the call's next voice position (R5.10, simplex framing). grow: an underrun the jitter buffer grows by,
/// so the stream's later bursts go out a position on, not dropped (R6.4).
unsafe fn silence(c: &mut RdofTxcall, out: &mut Burst, grow: bool) -> u8 {
    let n = c.vpos;
    let control = rdof_silence_burst(n, c.tune.cc, false, frag(c, n), out);
    c.vpos = (n + 1) % 6;
    if grow {
        c.vshift = (c.vshift + 1) % 6;
    } else {
        c.vfilled = c.vfilled.wrapping_add(1);
    }
    control
}

/// Bursts in hand ahead of the next one: queued, and what the radio still holds. Called locked.
fn ahead(c: &RdofTxcall, n: f64) -> f64 {
    let held = c.air_seen - n;
    f64::from(c.count) + if held > 0.0 { held / BURST_S } else { 0.0 }
}

/// A voice burst's place in the call (section 6): from the first A (R6.5), a gap filled with silence (R6.2), a burst
/// whose position went out as silence dropped (R6.4). Adaptive (jitter buffer): positions moved by vshift, the gap
/// filled only up to the target, a burst dropped when the stream is ahead of it.
unsafe fn voice(t: *mut RdofTxvr, c: &mut RdofTxcall, q: &RdofNetBurst) {
    let pos = if q.control & RDOF_SYNC_AUDIO != 0 { 0 } else { c_int::from(q.control & 0x0F) };
    if pos > 5 {
        push(c, q);
        return;
    }
    if c.vpos < 0 {
        if pos != 0 {
            return;
        }
        c.vpos = 0;
        c.vfilled = 0;
    }
    if (1..=4).contains(&pos) {
        for (i, f) in c.frags[pos as usize].iter_mut().enumerate() {
            *f = rdof_get_field(q.burst.as_ptr(), 116 + i as i32, 1) as u8;
        }
        c.has_frag |= 1 << pos;
    }
    let mut tpos = (pos + c.vshift) % 6; // where it goes out
    let mut gap = if c.vfilled >= 6 {
        (tpos - c.vpos).rem_euclid(6)
    } else {
        let d = (tpos - (c.vpos - c.vfilled)).rem_euclid(6);
        if d < c.vfilled {
            return;
        }
        d - c.vfilled
    };
    let jb = &mut *addr_of_mut!((*t).jb);
    if jb.adaptive {
        let n = now(t);
        let a = ahead(c, n);
        let keep = rdof_jb_fill(jb, &mut c.jbs, gap, a);
        if keep < gap {
            // the silence past the target isn't needed
            c.vshift = (c.vshift - (gap - keep)).rem_euclid(6);
            tpos = (tpos - (gap - keep)).rem_euclid(6);
            gap = keep;
        }
        if gap == 0 && rdof_jb_drop(jb, &mut c.jbs, a, rdof_jb_silent(&q.burst), n) {
            c.vshift = (c.vshift + 5) % 6; // its position goes to the next one
            return;
        }
    }
    for _ in 0..gap {
        let mut s = *q;
        s.control = silence(c, &mut s.burst, false);
        s.terminator = false;
        push(c, &s);
    }
    let mut v = *q;
    v.terminator = false;
    if tpos != pos {
        let (mut cc, mut pi, mut lcss) = (0, 0, 0);
        if pos != 0 && rdof_decode_emb(&q.burst, Some(&mut cc), Some(&mut pi), Some(&mut lcss), None) {
            c.vpi = pi;
        }
        v.control = rdof_jb_reframe(&q.burst, tpos, c.tune.cc, false, c.vpi, frag(c, tpos), &mut v.burst);
    }
    push(c, &v);
    c.vpos = (tpos + 1) % 6;
    c.vfilled = 0;
}

/// at: when it came, to measure the network by; < 0 not measured (held for its DMRT). Called locked.
unsafe fn add(t: *mut RdofTxvr, b: &RdofNetBurst, at: f64) {
    let c = &mut *addr_of_mut!((*t).call);
    let jb = &mut *addr_of_mut!((*t).jb);
    rdof_jb_key(jb, &mut c.jbs, b.dst, b.group, b.slot, c.started);
    if at >= 0.0 {
        rdof_jb_arrival(jb, &mut c.jbs, at, c_int::from(b.seq));
    }
    if c.src == 0 && b.src != 0 {
        c.src = b.src;
        c.dst = b.dst;
        c.group = b.group;
    }
    if !c.wants_reply && asks_reply(c, b) {
        c.wants_reply = true;
    }
    let mut q = *b;
    rdof_regenerate(b.control, &b.burst, c.tune.cc, false, &mut q.burst);
    if b.control == CSBK_CONTROL && b.terminator && c.count == 0 && !c.started {
        // A CSBK on its own: a short lead-in of preamble CSBKs, then it.
        c.has_csbk = true;
        c.csbk = b.burst;
        let n = (*t).csbk_preambles.clamp(0, 16);
        let mut pre = [[0u8; RDOF_BURST_BYTES]; 16];
        rdof_csbk_preambles(b.src, b.dst, b.group, n, c.tune.cc, pre.as_mut_ptr());
        for burst in &pre[..n as usize] {
            let p = RdofNetBurst { control: CSBK_CONTROL, burst: *burst, terminator: false, ..*b };
            push(c, &p);
        }
    }
    if b.control == HEADER_CONTROL && !c.has_lc {
        let mut lc12 = [0u8; 12];
        if ffi::rdof_bptc196_decode(b.burst.as_ptr(), lc12.as_mut_ptr()) {
            c.lc9.copy_from_slice(&lc12[..9]);
            c.has_lc = true;
        }
    }
    let radio = &*(*t).radio;
    let mut copies = 1; // R11.4: Hs headers and Ts terminators a call, however many come
    if b.control == HEADER_CONTROL {
        let hs = radio.headers.clamp(1, RDOF_TXVR_QUEUE as c_int);
        copies = (hs - c.headers).max(0);
        c.headers += copies;
        if !c.started && copies != 0 {
            c.extra += copies - 1;
        }
    } else if b.control == TERMINATOR_CONTROL {
        let ts = radio.terminators.clamp(1, RDOF_TXVR_QUEUE as c_int);
        copies = (ts - c.terminators).max(0);
        c.terminators += copies;
    }
    if b.control & RDOF_SYNC_DATA == 0 {
        // section 6 (R11.5)
        voice(t, c, &q);
        copies = 0;
    }
    for _ in 0..copies {
        push(c, &q);
    }
    c.last_burst = now(t);
    if b.terminator {
        c.ended = true;
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_txvr_on_tune(t: *mut RdofTxvr, tune: &RdofTune) {
    let mut tune = *tune;
    lock(t);
    if (*t).has_call && (*t).call.tune.stream_id == tune.stream_id {
        unlock(t);
        return; // a repeat
    }
    let refuse = if (*t).has_call {
        Some(DMRK_TXVR_BUSY)
    } else if !(*t).ready {
        Some(DMRK_RADIO_ERROR)
    } else if now(t) - (*modem(t)).last_rx < RX_HOLD_S
        && (*t).has_tuned
        && !near((*t).tuned_freq, tune.freq)
        && !tune.preempt
    {
        Some(DMRK_TXVR_BUSY) // receiving a call here
    } else {
        None
    };
    let ch = if refuse.is_none() { allowed(t, tune.freq, tune.cc) } else { None };
    let Some(ch) = ch else {
        answer(t, &tune, refuse.unwrap_or(DMRK_NOT_ALLOWED));
        unlock(t);
        return;
    };
    // HBlink4 sends power 255 for "the radio's own"; a channel's own setting wins.
    if tune.power == 255 && ch.power != 255 {
        tune.power = ch.power;
    }
    let c = addr_of_mut!((*t).call);
    c.write_bytes(0, 1);
    (*c).tune = tune;
    (*c).vpos = -1;
    (*c).state = CALL_PENDING;
    (*c).last_burst = now(t);
    (*c).group = true;
    (*t).has_call = true;
    rdof_jb_begin(&mut (*t).jb, &mut (*c).jbs);
    for i in 0..ORPHANS {
        let o = addr_of_mut!((*t).orphans[i]);
        if (*o).used && (*o).stream_id == tune.stream_id {
            (*o).used = false;
            for k in 0..upto((*o).n, ORPHAN_BURSTS) {
                let b = (*o).bursts[k];
                add(t, &b, -1.0);
            }
        }
    }
    unlock(t);
}

unsafe fn was_refused(t: *const RdofTxvr, sid: &[u8; 4]) -> bool {
    (&(*t).refused)[..upto((*t).nrefused, REFUSED)].iter().any(|r| r == sid)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_txvr_on_burst(t: *mut RdofTxvr, b: &RdofNetBurst) {
    lock(t);
    if (*t).has_call && (*t).call.tune.stream_id == b.stream_id {
        if (*t).call.state != CALL_DONE {
            add(t, b, now(t));
        }
        unlock(t);
        return;
    }
    if was_refused(t, &b.stream_id) {
        unlock(t);
        return;
    }
    let orphans = &mut *addr_of_mut!((*t).orphans);
    let (mut slot, mut free_slot, mut oldest) = (None, None, 0);
    for (i, o) in orphans.iter().enumerate() {
        if o.used && o.stream_id == b.stream_id {
            slot = Some(i);
        } else if !o.used && free_slot.is_none() {
            free_slot = Some(i);
        }
        if o.at < orphans[oldest].at {
            oldest = i;
        }
    }
    let o = match slot {
        Some(i) => &mut orphans[i],
        None => {
            let o = &mut orphans[free_slot.unwrap_or(oldest)];
            o.used = true;
            o.stream_id = b.stream_id;
            o.at = now(t);
            o.n = 0;
            o
        }
    };
    if o.n < 0 {
        o.n = 0;
    }
    if let Some(dst) = o.bursts.get_mut(o.n as usize) {
        *dst = *b;
        o.n += 1;
    }
    unlock(t);
}

#[no_mangle]
pub unsafe extern "C" fn rdof_txvr_heard_call(t: *mut RdofTxvr, action: *const c_char, src: u32, dst: u32) {
    let a = CStr::from_ptr(action).to_bytes();
    if a == b"start" || a == b"late_entry" {
        (*t).has_heard_call = true;
        (*t).heard_call_ended = false;
        (*t).heard_src = src;
        (*t).heard_dst = dst;
    } else if (a == b"end" || a == b"lost") && (*t).has_heard_call {
        (*t).heard_call_ended = true;
        (*t).heard_at = now(t);
    }
}

// ── where it is ──

#[no_mangle]
pub unsafe extern "C" fn rdof_txvr_on_idle_channel(t: *const RdofTxvr) -> bool {
    (*t).has_tuned && (*t).has_idle && near((*t).tuned_freq, (*t).idle.freq) && (*t).tuned_cc == (*t).idle.cc
}

unsafe fn scan_index(t: *const RdofTxvr) -> Option<usize> {
    if !(*t).has_scan || !(*t).has_tuned {
        return None;
    }
    let n = upto((*t).scan.nchannels, RDOF_TXVR_MAX_CHANNELS);
    (&(*t).scan.channels)[..n].iter().position(|ch| near(ch.freq, (*t).tuned_freq) && ch.cc == (*t).tuned_cc)
}

unsafe fn awaiting_answer(t: *const RdofTxvr) -> bool {
    (*t).has_answer_window
        && (*t).has_tuned
        && now(t) < (*t).aw_until
        && near((*t).tuned_freq, (*t).aw_freq)
        && (*t).tuned_cc == (*t).aw_cc
}

unsafe fn is_covered(t: *const RdofTxvr, freq: u32) -> bool {
    (&(*t).covered)[..upto((*t).ncovered, MAX_COVERED)].iter().any(|&f| near(freq, f))
}

#[no_mangle]
pub unsafe extern "C" fn rdof_txvr_listening(t: *const RdofTxvr) -> bool {
    rdof_txvr_on_idle_channel(t) || scan_index(t).is_some() || awaiting_answer(t)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_txvr_scanning(t: *const RdofTxvr) -> bool {
    (*t).has_scan && !(*t).has_call && !(*t).stopped
}

#[no_mangle]
pub unsafe extern "C" fn rdof_txvr_state_name(t: *const RdofTxvr) -> *const c_char {
    if !(*t).has_call {
        return c"idle".as_ptr();
    }
    match (*t).call.state {
        CALL_PENDING => c"pending",
        CALL_LISTENING => c"listening",
        CALL_ON_AIR => c"on_air",
        _ => c"done",
    }
    .as_ptr()
}

#[no_mangle]
pub unsafe extern "C" fn rdof_txvr_heard(t: *mut RdofTxvr, slot: c_int, control: u8, burst: *const u8, rssi: c_int) {
    if slot != 2 {
        return; // a simplex modem passes up slot 2 (on_rx)
    }
    let Some(hear) = (*t).receiver.hear else {
        return;
    };
    if tuning(t) || !rdof_txvr_listening(t) || ((*t).has_call && (*t).call.state == CALL_ON_AIR) {
        return;
    }
    let (freq, cc) = ((*t).tuned_freq, (*t).tuned_cc);
    if (*t).has_answer_window && awaiting_answer(t) {
        // a reply (or more of one): stay a little longer
        let until = now(t) + (*t).answer_s;
        (*t).aw_until = if until < (*t).aw_cap { until } else { (*t).aw_cap };
    }
    let mut dbm = 0;
    let d = if rdof_radio_rssi(&*(*t).radio, rssi, freq, &mut dbm) { f64::from(dbm) } else { f64::NAN };
    hear((*t).receiver.ctx, 2, control, burst, d, cc, freq);
}

// ── the worker ──

/// None: the radio failed; else whether it retuned.
unsafe fn tune_to(t: *mut RdofTxvr, freq: u32, cc: c_int, power: c_int, hop: bool) -> Option<bool> {
    if (*t).has_tuned && (*t).tuned_freq == freq && (*t).tuned_cc == cc && (*t).tuned_power == power {
        return Some(false);
    }
    set_tuning(t, true);
    let ok = rdof_radio_tune(&*(*t).radio, freq, cc, power);
    if ok {
        (*t).has_tuned = true;
        (*t).tuned_freq = freq;
        (*t).tuned_cc = cc;
        (*t).tuned_power = power;
        (*t).tuned_at = now(t);
    }
    set_tuning(t, false);
    if !ok {
        return None;
    }
    (*t).carrier_checked = false;
    if !hop {
        if let Some(f) = (*t).on_retune {
            f((*t).ctx, freq, cc);
        }
    }
    Some(true)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_txvr_start(t: *mut RdofTxvr) -> bool {
    (*t).has_tuned = false;
    if !rdof_modem_set_mode(modem(t), MODE_DMR) {
        return false;
    }
    let idle = (*t).idle;
    if (*t).has_idle && tune_to(t, idle.freq, idle.cc, idle.power, false).is_none() {
        return false;
    }
    (*t).ready = true;
    true
}

unsafe fn tx_event(t: *mut RdofTxvr, action: &CStr) {
    let Some(f) = (*t).on_tx else {
        return;
    };
    let mut info: RdofTxCallInfo = core::mem::zeroed();
    rdof_txvr_call_info(t, addr_of!((*t).call), &mut info);
    let mut d = 0.0;
    if action == c"end" {
        d = now(t) - (*t).call.on_air_at;
        if d < 0.0 {
            d = 0.0;
        }
    }
    f((*t).ctx, action.as_ptr(), &info, d);
}

unsafe fn finish(t: *mut RdofTxvr) {
    (*t).idle_since = now(t);
    if (*t).has_call {
        let was_on_air = (*t).call.state == CALL_ON_AIR;
        (*t).call.state = CALL_DONE;
        rdof_jb_end(&mut (*t).jb, &mut (*t).call.jbs);
        (*t).has_call = false;
        if was_on_air {
            tx_event(t, c"end");
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_txvr_call_info(_t: *const RdofTxvr, c: *const RdofTxcall, info: *mut RdofTxCallInfo) {
    info.write_bytes(0, 1);
    let (c, info) = (&*c, &mut *info);
    info.stream_id = c.tune.stream_id;
    info.src = c.src;
    info.dst = c.dst;
    info.group = c.group;
    info.freq = c.tune.freq;
    info.cc = c.tune.cc;
    info.slot = 2;
    info.has_csbk = c.has_csbk;
    info.csbk = c.csbk;
    info.frames = -1; // data_frames(): from the bursts still queued
    for i in 0..c.count.max(0) {
        let b = &c.bursts[(c.head + i) as usize % RDOF_TXVR_QUEUE];
        if b.control & RDOF_SYNC_DATA == 0 {
            break;
        }
        let dt = c_int::from(b.control & 0x0F);
        if dt == RDOF_DT_DATA_HEADER {
            let mut h: RdofDataHeader = core::mem::zeroed();
            rdof_decode_data_header(&b.burst, &mut h);
            info.frames = if h.crc_ok { h.blocks } else { 0 };
            break;
        }
        if dt == RDOF_DT_VOICE_LC_HEADER {
            break;
        }
    }
}

unsafe fn status(t: *const RdofTxvr, st: &mut RdofMmdvmStatus) -> bool {
    rdof_modem_status(modem(t), st)
}

fn no_status() -> RdofMmdvmStatus {
    RdofMmdvmStatus { transmitting: 0, carrier: 0, space: 0, space1: 0, inbound_busy: 0 }
}

/// false: a radio error.
unsafe fn start_call(t: *mut RdofTxvr) -> bool {
    (*t).has_answer_window = false;
    let tune = (*t).call.tune;
    let Some(retuned) = tune_to(t, tune.freq, tune.cc, tune.power, false) else {
        return false;
    };
    let n = now(t);
    (*t).call.state = CALL_LISTENING;
    (*t).call.listen_until = n + (*t).listen_s;
    // Already on the channel: what we heard lately counts too.
    let at = (*t).tuned_at;
    (*t).call.quiet_since = if retuned {
        at
    } else if n - (*t).clear_s > at {
        n - (*t).clear_s
    } else {
        at
    };
    true
}

unsafe fn quiet_since(t: *const RdofTxvr) -> f64 {
    // A reply to the call just heard here (its target answering its source) doesn't count that call.
    let c = &(*t).call;
    if (*t).has_heard_call && (*t).heard_call_ended && c.src != 0 && (*t).heard_src == c.dst && (*t).heard_dst == c.src
    {
        let tail = (*t).heard_at + CALL_TAIL_S;
        return if c.quiet_since > tail { c.quiet_since } else { tail };
    }
    c.quiet_since
}

unsafe fn listen(t: *mut RdofTxvr, n: f64) {
    let mut busy = (*modem(t)).last_rx > quiet_since(t);
    if !busy && n - (*t).last_status >= STATUS_EVERY_S {
        // Carrier detect, where the modem has it: anything on the channel, DMR or not.
        let mut st = no_status();
        (*t).last_status = n;
        if status(t, &mut st) {
            busy = st.carrier != 0;
        }
    }
    let tune = (*t).call.tune;
    if busy {
        lock(t);
        answer(t, &tune, DMRK_CHANNEL_BUSY);
        finish(t);
        unlock(t);
        return;
    }
    if n < (*t).call.listen_until {
        return;
    }
    lock(t);
    answer(t, &tune, DMRK_ON_AIR);
    (*t).call.state = CALL_ON_AIR;
    (*t).call.on_air_at = n;
    unlock(t);
    tx_event(t, c"start");
}

unsafe fn write_burst(t: *mut RdofTxvr, b: &RdofNetBurst, n: f64) -> bool {
    if !rdof_modem_write_dmr(modem(t), b.control, b.burst.as_ptr(), 2) {
        return false;
    }
    (*t).space = (*t).space.wrapping_sub(1);
    (*t).air_until = (if (*t).air_until > n { (*t).air_until } else { n }) + BURST_S;
    (*t).call.started = true;
    if let Some(f) = (*t).on_sent {
        let mut info: RdofTxCallInfo = core::mem::zeroed();
        rdof_txvr_call_info(t, addr_of!((*t).call), &mut info);
        f((*t).ctx, &info, b);
    }
    true
}

/// R7.8, R11.5: a stream lost before its terminator ends as if it had come, so the radios leave the call now and not
/// on their own fade timers. Only a voice call: other streams have no terminator. Called locked.
unsafe fn timed_out(t: *mut RdofTxvr) {
    let c = &mut *addr_of_mut!((*t).call);
    c.ended = true;
    if c.vpos < 0 || c.terminators != 0 {
        return;
    }
    let mut lc9 = c.lc9;
    if !c.has_lc {
        rdof_group_lc(c.src, c.dst, c.group, 0, &mut lc9);
    }
    let mut term: RdofNetBurst = core::mem::zeroed();
    term.control = TERMINATOR_CONTROL;
    term.terminator = true;
    term.stream_id = c.tune.stream_id;
    rdof_lc_burst(&lc9, RDOF_DT_TERMINATOR_WITH_LC, c.tune.cc, RDOF_MS_VOICE ^ RDOF_DATA_XOR, &mut term.burst);
    c.terminators = (*(*t).radio).terminators.clamp(1, RDOF_TXVR_QUEUE as c_int);
    for _ in 0..c.terminators {
        push(c, &term);
    }
}

unsafe fn play(t: *mut RdofTxvr, n: f64) -> bool {
    let c = addr_of_mut!((*t).call);
    lock(t);
    if !(*c).ended && n - (*c).last_burst > STREAM_TIMEOUT_S {
        timed_out(t);
    }
    let waiting = (*c).count;
    let ended = (*c).ended;
    unlock(t);
    if !(*c).started && waiting < rdof_jb_prebuffer(&(*t).jb, &(*c).jbs, (*t).prebuffer) + (*c).extra && !ended {
        return true; // a little in hand, so the radio doesn't run dry
    }
    // R6.3: the stream ran dry mid-call: silence at the next position while the radio holds one burst or less.
    let underrun = waiting == 0 && !ended && (*c).started && (*c).vpos >= 0 && (*t).air_until - n <= BURST_S + 1e-6;
    if waiting != 0 || underrun {
        if n - (*t).last_status >= STATUS_EVERY_S || (*t).space <= 1 {
            let mut st = no_status();
            if status(t, &mut st) {
                (*t).space = st.space;
                (*t).last_status = n;
                (*t).has_status_missed = false;
            } else {
                (*t).status_misses = (*t).status_misses.wrapping_add(1);
                if !(*t).has_status_missed {
                    (*t).has_status_missed = true;
                    (*t).status_missed = n;
                } else if n - (*t).status_missed > STATUS_GIVE_UP_S {
                    return false;
                }
                (*t).space = 0; // try again next poll
                return true;
            }
        }
        if underrun {
            if (*t).space > 1 {
                let mut b: RdofNetBurst = core::mem::zeroed();
                b.stream_id = (*c).tune.stream_id;
                b.group = true; // a network burst's defaults
                b.slot = 2;
                lock(t);
                let grow = rdof_jb_underrun(&mut (*t).jb, &mut (*c).jbs);
                b.control = silence(&mut *c, &mut b.burst, grow);
                (*c).air_seen = (*t).air_until.max(n) + BURST_S;
                unlock(t);
                if !write_burst(t, &b, n) {
                    return false;
                }
            }
            return true;
        }
        #[allow(clippy::while_immutable_condition)] // write_burst lowers space through t
        while (*t).space > 1 {
            // R10.2: the modem's last free place is never used
            lock(t);
            if (*c).count == 0 {
                unlock(t);
                break;
            }
            let b = pop(&mut *c);
            (*c).air_seen = (*t).air_until.max(n) + BURST_S;
            unlock(t);
            if !write_burst(t, &b, n) {
                return false;
            }
        }
        return true;
    }
    if ended {
        // All handed over, but the radio still has up to 48 bursts (~3 s) to send: retuning now would cut the end off.
        if n - (*t).last_status < STATUS_EVERY_S {
            return true;
        }
        (*t).last_status = n;
        let mut st = no_status();
        if !status(t, &mut st) {
            return false;
        }
        if st.transmitting != 0 {
            (*c).has_quiet_at = false;
            return true;
        }
        if !(*c).has_quiet_at {
            (*c).has_quiet_at = true;
            (*c).quiet_at = n;
        }
        if !(*c).has_csbk && !(*c).wants_reply && n - (*c).quiet_at < TX_TAIL_S {
            return true; // a voice call's terminators may still be going out
        }
        let (wants, freq, cc) = ((*c).wants_reply, (*c).tune.freq, (*c).tune.cc);
        lock(t);
        finish(t);
        unlock(t);
        if wants && (*t).answer_s > 0.0 && !is_covered(t, freq) {
            // Back to receive at once, on its channel, for the reply; no CW ID now (the next call's).
            let tt = now(t);
            (*t).has_answer_window = true;
            (*t).aw_freq = freq;
            (*t).aw_cc = cc;
            (*t).aw_until = tt + (*t).answer_s;
            (*t).aw_cap = tt + (*t).answer_max_s;
            return true;
        }
        return after_call(t, n);
    }
    true
}

unsafe fn wait_until_quiet(t: *const RdofTxvr) -> bool {
    let m = modem(t);
    for _ in 0..150 {
        // 15 s at most
        let mut st = no_status();
        if !status(t, &mut st) {
            return false;
        }
        if st.transmitting == 0 {
            return true;
        }
        if let Some(sleep) = (*m).port.sleep_ms {
            sleep((*m).port.ctx, 100);
        }
    }
    true
}

unsafe fn after_call(t: *mut RdofTxvr, n: f64) -> bool {
    if (*t).cwid[0] == 0 || !(*t).has_tuned {
        return true;
    }
    let freq = (*t).tuned_freq;
    let nid = upto((*t).nlast_id, IDS);
    let k = (&(*t).last_id)[..nid].iter().position(|id| id.freq == freq);
    if let Some(k) = k {
        if n - (*t).last_id[k].at < (*t).cwid_every_s {
            return true;
        }
    }
    let m = modem(t);
    // the radio sends CW only from idle
    if !wait_until_quiet(t)
        || !rdof_modem_set_mode(m, MODE_IDLE)
        || !rdof_modem_send_cwid(m, (*t).cwid.as_ptr())
        || !wait_until_quiet(t)
        || !rdof_modem_set_mode(m, MODE_DMR)
    {
        return false;
    }
    let k = match k {
        Some(k) => k,
        None if nid < IDS => {
            (*t).nlast_id = nid as c_int + 1;
            nid
        }
        // full: the channel identified longest ago
        None => (0..IDS).fold(0, |o, i| if (*t).last_id[i].at < (*t).last_id[o].at { i } else { o }),
    };
    (*t).last_id[k] = RdofLastId { freq, at: now(t) };
    true
}

unsafe fn scan_step(t: *mut RdofTxvr, n: f64) -> bool {
    let nch = upto((*t).scan.nchannels, RDOF_TXVR_MAX_CHANNELS) as c_int;
    if nch == 0 {
        return true; // nothing to scan
    }
    if let Some(here) = scan_index(t) {
        let last_rx = (*modem(t)).last_rx;
        let heard = last_rx > (*t).tuned_at;
        if heard && n - last_rx < (*t).scan.hang_s {
            if !(*t).stopped {
                (*t).stopped = true;
                if let Some(f) = (*t).on_scan {
                    f((*t).ctx, true);
                }
            }
            return true;
        }
        if (*t).stopped {
            (*t).stopped = false;
            if let Some(f) = (*t).on_scan {
                f((*t).ctx, false);
            }
        } else if !heard {
            let mut dwell = n - (*t).tuned_at;
            if (*t).scan.carrier_s > 0.0 && !(*t).carrier_checked && dwell >= (*t).scan.carrier_s {
                let mut st = no_status();
                (*t).carrier_checked = true;
                (*t).last_status = n;
                if !status(t, &mut st) {
                    return false;
                }
                if st.carrier == 0 {
                    dwell = (*t).scan.dwell_s; // nothing there: move on now
                }
            }
            if dwell < (*t).scan.dwell_s {
                return true;
            }
        }
        (*t).scan_next = (here as c_int + 1) % nch;
    }
    if !(0..nch).contains(&(*t).scan_next) {
        (*t).scan_next = 0;
    }
    let ch = (*t).scan.channels[(*t).scan_next as usize];
    (*t).scan_next = ((*t).scan_next + 1) % nch;
    tune_to(t, ch.freq, ch.cc, ch.power, true).is_some()
}

unsafe fn idle(t: *mut RdofTxvr, n: f64) -> bool {
    if (*t).has_answer_window && !awaiting_answer(t) {
        (*t).has_answer_window = false;
        (*t).idle_since = n; // back to the idle channel / scan from here
    }
    let after = n - (*t).idle_since >= (*t).idle_after_s;
    if (*t).has_answer_window {
        // listening for the answer to a lone CSBK
    } else if (*t).has_scan {
        if after && !scan_step(t, n) {
            return false;
        }
    } else if (*t).has_idle && !rdof_txvr_on_idle_channel(t) && after {
        let i = (*t).idle;
        if tune_to(t, i.freq, i.cc, i.power, false).is_none() {
            return false;
        }
    }
    if n - (*t).last_status >= IDLE_STATUS_EVERY_S {
        let mut st = no_status();
        if !status(t, &mut st) {
            return false;
        }
        (*t).last_status = n;
    }
    true
}

#[no_mangle]
pub unsafe extern "C" fn rdof_txvr_poll(t: *mut RdofTxvr) -> bool {
    let n = now(t);
    lock(t);
    for o in (*t).orphans.iter_mut() {
        if o.used && n - o.at > ORPHAN_S {
            o.used = false;
        }
    }
    let has_call = (*t).has_call;
    let state = (*t).call.state;
    unlock(t);
    if let Some(tick) = (*t).receiver.tick {
        tick((*t).receiver.ctx);
    }
    let ok = if !has_call {
        idle(t, n)
    } else {
        match state {
            CALL_PENDING => start_call(t),
            CALL_LISTENING => {
                listen(t, n);
                true
            }
            CALL_ON_AIR => play(t, n),
            _ => true,
        }
    };
    if !ok {
        (*t).has_tuned = false; // unknown now: retune next time
        (*t).ready = false;
        lock(t);
        if (*t).has_call && ((*t).call.state == CALL_PENDING || (*t).call.state == CALL_LISTENING) {
            let tune = (*t).call.tune;
            answer(t, &tune, DMRK_RADIO_ERROR);
        }
        finish(t);
        unlock(t);
    }
    ok
}
