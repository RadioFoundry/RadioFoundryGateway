// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! The repeater (rdof/repeater.h): a port of common/src/repeater.c. Rule ids (R4.6) point to docs/spec/repeater.md.
//!
//! An rdof_repeater_t is shared by several tasks: HBlink4's (rdof_repeater_on_burst), the modem reader's
//! (rdof_repeater_heard, _lost, _nak), the status reports' (rdof_repeater_busy, _on_air) and the repeater's own
//! (rdof_repeater_poll). So, as in txvr.rs, the functions work through the raw pointer and never make a `&mut` of the
//! whole struct, a slot or a call; the input queues and `dropped` are only touched under the platform's lock.
//! duplex.c (the bursts, the voice regeneration, the short LC) stays C.

use core::ffi::{c_char, c_int, c_long, c_void};
use core::ptr::{addr_of, addr_of_mut, null_mut};

use crate::ffi::{
    self, RDOF_BURST_BYTES, RDOF_DT_CSBK, RDOF_DT_DATA_HEADER, RDOF_DT_PI_HEADER, RDOF_DT_RATE_1, RDOF_DT_RATE_12,
    RDOF_DT_RATE_34, RDOF_DT_TERMINATOR_WITH_LC, RDOF_DT_VOICE_LC_HEADER,
};
use crate::hblink::{RdofNetBurst, RdofTune};
use crate::jitter::{
    rdof_jb_arrival, rdof_jb_begin, rdof_jb_drop, rdof_jb_end, rdof_jb_fill, rdof_jb_init, rdof_jb_key, rdof_jb_prebuffer,
    rdof_jb_reframe, rdof_jb_silent, rdof_jb_underrun, RdofJb, RdofJbStream,
};
use crate::bits::rdof_set_field;
use crate::layer2::{rdof_decode_emb, rdof_group_lc, RDOF_DATA_XOR};
use crate::mmdvm::{RdofMmdvmStatus, RDOF_SYNC_DATA};
use crate::modem::{
    rdof_modem_dmr_abort, rdof_modem_dmr_short_lc, rdof_modem_dmr_start, rdof_modem_send_cwid, rdof_modem_set_mode,
    rdof_modem_status, rdof_modem_write_dmr, rdof_radio_rssi, rdof_radio_tune_dcdm, rdof_radio_tune_duplex, RdofModem,
    RdofRadio,
};
use crate::netcalls::RdofTxCallInfo;

const RDOF_DPF_PROPRIETARY: c_int = 15;
use crate::pdu::{rdof_decode_csbk, rdof_decode_data_header, rdof_decode_voice_lc, rdof_parse_full_lc, RdofCsbk, RdofDataHeader, RdofFullLc};
use crate::txvr::RdofReceiver;

type Burst = [u8; RDOF_BURST_BYTES];

const HEADERS: usize = 3;
const BURST_S: f64 = 0.06;
const FILL_AHEAD_S: f64 = 0.12;
const KEY_LEAD_S: f64 = 0.36;
const STATUS_EVERY_S: f64 = 0.06;
const IDLE_STATUS_EVERY_S: f64 = 2.0;
const STATUS_GIVE_UP_S: f64 = 2.0;
const NET_TIMEOUT_S: f64 = 1.5;
const RF_TIMEOUT_S: f64 = 1.0;
const ECHO_S: f64 = 2.0;
const SFR_QUIET_S: f64 = 0.3;
const EPS: f64 = 1e-6;

pub const RDOF_RPT_OUT: usize = 192;
pub const RDOF_RPT_PENDING: usize = 96;
pub const RDOF_RPT_CALLS: usize = 6;
pub const RDOF_RPT_IN: usize = 64;
pub const RDOF_RPT_DONE: usize = 64;
const NAKS: usize = 16;

// rdof_rpt_kind_t
const KIND_NONE: c_int = 0;
const KIND_VOICE: c_int = 1;
const KIND_DATA: c_int = 2;
// rdof_activity_kind_t
const ACT_VOICE: c_int = 1;
const ACT_DATA: c_int = 2;

const BS_DWN_ACT: c_int = 0x38;
const RDOF_NONE: i32 = -1;
const RDOF_SYNC_AUDIO: u8 = 0x20;
const RDOF_CONTROL_IDLE: u8 = 0x80;
const RDOF_NAK_RF_BUSY: c_int = 6;
const DMR_DATA1: u8 = 0x18;
const DMR_DATA2: u8 = 0x1A;
const MODE_IDLE: c_int = 0;
const MODE_DMR: c_int = 2;
const HEADER: u8 = RDOF_SYNC_DATA | RDOF_DT_VOICE_LC_HEADER as u8;
const TERMINATOR: u8 = RDOF_SYNC_DATA | RDOF_DT_TERMINATOR_WITH_LC as u8;

/// rdof_voice_regen_t (rdof/duplex.h)
#[repr(C)]
pub struct RdofVoiceRegen {
    pub lc9: [u8; 9],
    pub cc: c_int,
    pub regen_ambe: bool,
    pub lc_raw: [u8; 128],
    pub sending: [u8; 128],
    pub heard: [[u8; 32]; 5],
    pub have: u8,
    pub errors: c_long,
}

/// rdof_activity_t (rdof/duplex.h)
#[repr(C)]
#[derive(Clone, Copy, PartialEq)]
pub struct RdofActivity {
    pub kind: c_int,
    pub dst: u32,
    pub group: bool,
}

/// rdof_rpt_out_t
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RdofRptOut {
    pub end: bool,
    pub control: u8,
    pub burst: Burst,
    pub call: i8,
    pub report: bool,
    pub end_kind: u8,
    pub seq: u32,
}

/// rdof_voice_out_t
#[repr(C)]
pub struct RdofVoiceOut {
    pub regen: RdofVoiceRegen,
    pub cc: c_int,
    pub header: Burst,
    pub wait_a: bool,
    pub next: c_int,
    pub filled: c_int,
    pub shift: c_int,
    pub pi: c_int,
}

/// rdof_rpt_call_t
#[repr(C)]
pub struct RdofRptCall {
    pub used: bool,
    pub tune: RdofTune,
    pub slot: c_int,
    pub src: u32,
    pub dst: u32,
    pub group: bool,
    pub kind: c_int,
    pub lc9: [u8; 9],
    pub has_voice: bool,
    pub voice: RdofVoiceOut,
    pub blocks: c_int,
    pub pending: [RdofRptOut; RDOF_RPT_PENDING],
    pub npending: c_int,
    pub received: c_int,
    pub started: bool,
    pub on_air: bool,
    pub finished: bool,
    pub on_air_at: f64,
    pub last_burst: f64,
    pub has_csbk: bool,
    pub csbk: Burst,
    pub jbs: RdofJbStream,
}

/// rdof_rpt_rf_t
#[repr(C)]
pub struct RdofRptRf {
    pub active: bool,
    pub kind: c_int,
    pub src: u32,
    pub dst: u32,
    pub group: bool,
    pub lc9: [u8; 9],
    pub last: f64,
    pub voice: RdofVoiceOut,
    pub blocks: c_int,
}

/// rdof_rpt_slot_t
#[repr(C)]
pub struct RdofRptSlot {
    pub n: c_int,
    pub rf: RdofRptRf,
    pub net: c_int,
    pub out: [RdofRptOut; RDOF_RPT_OUT],
    pub head: c_int,
    pub count: c_int,
    pub space: c_int,
    pub max_space: c_int,
    pub has_last_rf: bool,
    pub last_src: u32,
    pub last_dst: u32,
    pub last_at: f64,
    pub done: [[u8; 4]; RDOF_RPT_DONE],
    pub ndone: c_int,
    pub done_next: c_int,
    pub activity: RdofActivity,
    pub late: [[u8; 32]; 5],
    pub late_have: u8,
    pub seq: u32,
    pub has_hang: bool,
    pub hang_term: Burst,
    pub hang_until: f64,
    pub air_until: f64,
    pub net_until: f64,
}

/// rdof_rpt_rf_in_t
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RdofRptRfIn {
    pub slot: c_int,
    pub lost: bool,
    pub control: u8,
    pub burst: Burst,
    pub rssi: c_int,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct RdofRptNak {
    pub command: u8,
    pub err: c_int,
}

/// rdof_repeater_t
#[repr(C)]
pub struct RdofRepeater {
    pub radio: *mut RdofRadio,
    pub peer_id: u32,
    pub rx_freq: u32,
    pub tx_freq: u32,
    pub cc: c_int,
    pub power: c_int,
    pub enabled: [bool; 3],
    pub inactivity_s: f64,
    pub call_hang_s: f64,
    pub short_lc_ts1_low: bool,
    pub prebuffer: c_int,
    pub cwid: [c_char; 41],
    pub cwid_every_s: f64,
    pub sfr: bool,
    pub dcdm: bool,
    pub inbound: c_int,
    pub outbound: c_int,

    pub on_tx: Option<
        unsafe extern "C" fn(ctx: *mut c_void, action: *const c_char, call: *const RdofTxCallInfo, duration: f64),
    >,
    pub on_sent: Option<unsafe extern "C" fn(ctx: *mut c_void, call: *const RdofTxCallInfo, b: *const RdofNetBurst)>,
    pub on_key: Option<unsafe extern "C" fn(ctx: *mut c_void, keyed: bool)>,
    pub lock: Option<unsafe extern "C" fn(ctx: *mut c_void)>,
    pub unlock: Option<unsafe extern "C" fn(ctx: *mut c_void)>,
    pub ctx: *mut c_void,
    pub receiver: RdofReceiver,
    pub clock: Option<unsafe extern "C" fn(ctx: *mut c_void) -> f64>,

    pub slots: [RdofRptSlot; 3],
    pub calls: [RdofRptCall; RDOF_RPT_CALLS],
    pub keyed: bool,
    pub ready: bool,
    pub transmitting: bool,
    pub rf_in: [RdofRptRfIn; RDOF_RPT_IN],
    pub rf_head: c_int,
    pub rf_count: c_int,
    pub net_in: [RdofNetBurst; RDOF_RPT_IN],
    pub net_at: [f64; RDOF_RPT_IN],
    pub net_head: c_int,
    pub net_count: c_int,
    pub naks: [RdofRptNak; NAKS],
    pub nak_head: c_int,
    pub nak_count: c_int,
    pub tx_until: f64,
    pub last_status: f64,
    pub lead_until: f64,
    pub has_status_missed: bool,
    pub status_missed: f64,
    pub has_short_lc: bool,
    pub short_lc: [RdofActivity; 2],
    pub last_id: f64,
    pub sent_since_id: bool,
    pub inbound_last: f64,
    pub inbound_flag: bool,
    pub dropped: u32,
    pub jb: RdofJb,
}

type R = *mut RdofRepeater;
type S = *mut RdofRptSlot;
type C = *mut RdofRptCall;

unsafe fn now_of(r: *const RdofRepeater) -> f64 {
    match (*r).clock {
        Some(f) => f((*r).ctx),
        None => 0.0,
    }
}

unsafe fn lock(r: *const RdofRepeater) {
    if let Some(f) = (*r).lock {
        f((*r).ctx);
    }
}

unsafe fn unlock(r: *const RdofRepeater) {
    if let Some(f) = (*r).unlock {
        f((*r).ctx);
    }
}

unsafe fn modem(r: *const RdofRepeater) -> *mut RdofModem {
    (*(*r).radio).modem
}

unsafe fn slot(r: R, n: c_int) -> S {
    addr_of_mut!((*r).slots[n as usize])
}

unsafe fn call(r: R, idx: c_int) -> C {
    addr_of_mut!((*r).calls[idx as usize])
}

fn is_block(dt: c_int) -> bool {
    dt == RDOF_DT_RATE_12 || dt == RDOF_DT_RATE_34 || dt == RDOF_DT_RATE_1
}

/// An input or burst dropped for want of room; under the lock, as the other tasks count theirs.
unsafe fn drop_one(r: R) {
    lock(r);
    (*r).dropped = (*r).dropped.wrapping_add(1);
    unlock(r);
}

#[no_mangle]
pub unsafe extern "C" fn rdof_repeater_set_call_hang(r: R, call_hang_s: f64) {
    (*r).call_hang_s = if call_hang_s > 0.0 { call_hang_s } else { 0.0 };
}

#[no_mangle]
pub unsafe extern "C" fn rdof_repeater_init(r: R, radio: *mut RdofRadio, peer_id: u32, rx_freq: u32, tx_freq: u32, cc: c_int) {
    r.write_bytes(0, 1);
    (*r).radio = radio;
    (*r).peer_id = peer_id;
    (*r).rx_freq = rx_freq;
    (*r).tx_freq = tx_freq;
    (*r).cc = cc;
    (*r).power = 255;
    (*r).enabled[1] = true;
    (*r).enabled[2] = true;
    (*r).inactivity_s = 5.0;
    rdof_repeater_set_call_hang(r, 3.0);
    (*r).prebuffer = 6;
    (*r).cwid_every_s = 900.0;
    (*r).inbound = 1;
    (*r).outbound = 2;
    for n in 1..=2 {
        (*r).slots[n].n = n as c_int;
        (*r).slots[n].net = -1;
    }
    (*r).tx_until = f64::NEG_INFINITY;
    (*r).lead_until = f64::NEG_INFINITY;
    (*r).last_status = f64::NEG_INFINITY;
    (*r).last_id = f64::NEG_INFINITY;
    (*r).inbound_last = f64::NEG_INFINITY;
    rdof_jb_init(&mut (*r).jb);
}

#[no_mangle]
pub unsafe extern "C" fn rdof_repeater_set_sfr(r: R, inbound_slot: c_int) {
    let inbound = if inbound_slot == 2 { 2 } else { 1 }; // the slots are 1 and 2
    (*r).sfr = true;
    (*r).dcdm = false;
    (*r).inbound = inbound;
    (*r).outbound = 3 - inbound;
}

#[no_mangle]
pub unsafe extern "C" fn rdof_repeater_set_dcdm(r: R) {
    (*r).dcdm = true;
    (*r).sfr = false;
}

/// The modem keys itself for what it's given: no DMR_START, no lead, no short LC (SFR, DCDM).
unsafe fn keys_itself(r: *const RdofRepeater) -> bool {
    (*r).sfr || (*r).dcdm
}

// ── queues ──

#[no_mangle]
pub unsafe extern "C" fn rdof_repeater_on_burst(r: R, b: &RdofNetBurst) {
    lock(r);
    if (*r).net_count < RDOF_RPT_IN as c_int {
        let i = ((*r).net_head + (*r).net_count) as usize % RDOF_RPT_IN;
        (*r).net_in[i] = *b;
        (*r).net_at[i] = now_of(r);
        (*r).net_count += 1;
    } else {
        (*r).dropped = (*r).dropped.wrapping_add(1);
    }
    unlock(r);
}

unsafe fn rf_push(r: R, input: &RdofRptRfIn) {
    lock(r);
    if (*r).rf_count < RDOF_RPT_IN as c_int {
        (*r).rf_in[((*r).rf_head + (*r).rf_count) as usize % RDOF_RPT_IN] = *input;
        (*r).rf_count += 1;
    } else {
        (*r).dropped = (*r).dropped.wrapping_add(1);
    }
    unlock(r);
}

#[no_mangle]
pub unsafe extern "C" fn rdof_repeater_heard(r: R, slot: c_int, control: u8, burst: *const Burst, rssi: c_int) {
    let input = RdofRptRfIn { slot, lost: false, control, burst: *burst, rssi };
    rf_push(r, &input);
}

#[no_mangle]
pub unsafe extern "C" fn rdof_repeater_lost(r: R, slot: c_int) {
    let input = RdofRptRfIn { slot, lost: true, control: 0, burst: [0; RDOF_BURST_BYTES], rssi: -1 };
    rf_push(r, &input);
}

#[no_mangle]
pub unsafe extern "C" fn rdof_repeater_nak(r: R, command: u8, err: c_int) {
    lock(r);
    if (*r).nak_count < NAKS as c_int {
        let i = ((*r).nak_head + (*r).nak_count) as usize % NAKS;
        (*r).naks[i] = RdofRptNak { command, err };
        (*r).nak_count += 1;
    }
    unlock(r);
}

unsafe fn out_push(r: R, s: S, o: &RdofRptOut) {
    if (*s).count >= RDOF_RPT_OUT as c_int {
        drop_one(r);
        return;
    }
    (*s).out[((*s).head + (*s).count) as usize % RDOF_RPT_OUT] = *o;
    (*s).count += 1;
}

unsafe fn out_at(s: S, i: c_int) -> *mut RdofRptOut {
    addr_of_mut!((*s).out[((*s).head + i) as usize % RDOF_RPT_OUT])
}

unsafe fn out_pop(s: S) {
    (*s).head = ((*s).head + 1) % RDOF_RPT_OUT as c_int;
    (*s).count -= 1;
}

unsafe fn pending_push(r: R, c: C, o: &RdofRptOut) {
    if (*c).npending >= RDOF_RPT_PENDING as c_int {
        drop_one(r);
        return;
    }
    (*c).pending[(*c).npending as usize] = *o;
    (*c).npending += 1;
}

fn out_item(control: u8, burst: Option<&Burst>, call: c_int, report: bool) -> RdofRptOut {
    RdofRptOut {
        end: false,
        control,
        burst: burst.copied().unwrap_or([0; RDOF_BURST_BYTES]),
        call: call as i8,
        report,
        end_kind: 0,
        seq: 0,
    }
}

unsafe fn done_add(s: S, sid: &[u8; 4]) {
    let k = (*s).done_next as usize;
    (*s).done[k] = *sid;
    (*s).done_next = ((k + 1) % RDOF_RPT_DONE) as c_int;
    if (*s).ndone < RDOF_RPT_DONE as c_int {
        (*s).ndone += 1;
    }
}

unsafe fn is_done(s: S, sid: &[u8; 4]) -> bool {
    (&(*s).done)[..(*s).ndone as usize].iter().any(|d| d == sid)
}

unsafe fn activity_set(s: S, kind: c_int, dst: u32, group: bool) {
    let kind = if kind == KIND_VOICE { ACT_VOICE } else { ACT_DATA };
    (*s).activity = RdofActivity { kind, dst, group };
}

unsafe fn activity_clear(s: S) {
    addr_of_mut!((*s).activity).write_bytes(0, 1);
}

// ── hooks ──

unsafe fn call_info(c: *const RdofRptCall) -> RdofTxCallInfo {
    RdofTxCallInfo {
        stream_id: (*c).tune.stream_id,
        src: (*c).src,
        dst: (*c).dst,
        group: (*c).group,
        freq: (*c).tune.freq,
        cc: (*c).tune.cc,
        slot: (*c).slot,
        has_csbk: (*c).has_csbk,
        csbk: (*c).csbk,
        frames: if (*c).kind == KIND_DATA { (*c).blocks } else { -1 },
    }
}

unsafe fn call_end(r: R, idx: c_int) {
    if idx < 0 {
        return;
    }
    let c = call(r, idx);
    if (*c).finished {
        return;
    }
    (*c).finished = true;
    if !(*c).on_air {
        return;
    }
    (*c).on_air = false;
    if let Some(f) = (*r).on_tx {
        let info = call_info(c);
        let d = now_of(r) - (*c).on_air_at;
        f((*r).ctx, c"end".as_ptr(), &info, if d > 0.0 { d } else { 0.0 });
    }
}

unsafe fn call_sent(r: R, idx: c_int, item: &RdofRptOut, slot: c_int) {
    let c = call(r, idx);
    if (*c).finished {
        return;
    }
    if !(*c).on_air {
        (*c).on_air = true;
        (*c).on_air_at = now_of(r);
        if let Some(f) = (*r).on_tx {
            let info = call_info(c);
            f((*r).ctx, c"start".as_ptr(), &info, 0.0);
        }
    }
    if item.report {
        if let Some(f) = (*r).on_sent {
            let b = RdofNetBurst {
                stream_id: (*c).tune.stream_id,
                control: item.control,
                burst: item.burst,
                terminator: false,
                seq: 0,
                src: (*c).src,
                dst: (*c).dst,
                group: (*c).group,
                slot,
            };
            let info = call_info(c);
            f((*r).ctx, &info, &b);
        }
    }
}

// ── the voice stream out (section 6) ──

unsafe fn vout_init(v: *mut RdofVoiceOut, lc9: &[u8; 9], cc: c_int) {
    v.write_bytes(0, 1);
    ffi::rdof_voice_regen_init(addr_of_mut!((*v).regen), lc9.as_ptr(), cc);
    (*v).cc = cc;
    ffi::rdof_duplex_lc_burst(lc9.as_ptr(), RDOF_DT_VOICE_LC_HEADER, cc, addr_of_mut!((*v).header).cast());
    (*v).wait_a = true;
}

/// The underrun fill (R6.3): silence for the next position. grow: a network stream's jitter buffer grows by it, so
/// the stream's later bursts go out a position on, not dropped (R6.4).
unsafe fn vout_silence(v: *mut RdofVoiceOut, out: &mut Burst, grow: bool) -> u8 {
    let mut raw = [0u8; RDOF_BURST_BYTES];
    let control = ffi::rdof_duplex_silence((*v).next, (*v).cc, raw.as_mut_ptr());
    (*v).next = ((*v).next + 1) % 6;
    if grow {
        (*v).shift = ((*v).shift + 1) % 6;
    } else {
        (*v).filled = (*v).filled.wrapping_add(1);
    }
    ffi::rdof_voice_regen_burst(addr_of_mut!((*v).regen), control, raw.as_ptr(), out.as_mut_ptr());
    control
}

/// What to queue for a voice burst (R6.1-R6.5, R4.6, R7.6): none, or gap fill and it. Returns the count; the burst
/// itself is the last. jb / js: a network stream's jitter buffer (None for RF), with `ahead` bursts in hand:
/// positions moved by shift, the gap filled only up to the target, a burst dropped when it's ahead.
#[allow(clippy::too_many_arguments)]
unsafe fn vout_take(
    v: *mut RdofVoiceOut,
    control: u8,
    burst: &Burst,
    controls: &mut [u8; 6],
    out: &mut [Burst; 6],
    jb: Option<(*mut RdofJb, *mut RdofJbStream)>,
    ahead: f64,
    now: f64,
) -> usize {
    let pos = if control & RDOF_SYNC_AUDIO != 0 { 0 } else { c_int::from(control & 0x0F) };
    if (*v).wait_a {
        if pos != 0 {
            return 0;
        }
        (*v).wait_a = false;
        (*v).next = 0;
        (*v).filled = 0;
    }
    let mut tpos = (pos + (*v).shift) % 6; // where it goes out
    let mut gap = if (*v).filled >= 6 {
        (tpos - (*v).next).rem_euclid(6) // a superframe or more filled: can't tell late from early
    } else {
        let d = (tpos - ((*v).next - (*v).filled)).rem_euclid(6);
        if d < (*v).filled {
            return 0; // R6.4: its position went out as silence
        }
        d - (*v).filled
    };
    if let Some((jb, js)) = jb {
        if (*jb).adaptive {
            let keep = rdof_jb_fill(&mut *jb, &mut *js, gap, ahead);
            if keep < gap {
                // the silence past the target isn't needed
                (*v).shift = ((*v).shift - (gap - keep)).rem_euclid(6);
                tpos = (tpos - (gap - keep)).rem_euclid(6);
                gap = keep;
            }
            if gap == 0 && rdof_jb_drop(&mut *jb, &mut *js, ahead, rdof_jb_silent(burst), now) {
                (*v).shift = ((*v).shift + 5) % 6; // its position goes to the next one
                return 0;
            }
        }
    }
    let mut n = 0;
    for _ in 0..gap {
        controls[n] = vout_silence(v, &mut out[n], false);
        n += 1;
    }
    if tpos != pos {
        let mut moved = [0u8; RDOF_BURST_BYTES];
        let (mut cc, mut pi, mut lcss) = (0, 0, 0);
        if pos != 0 && rdof_decode_emb(burst, Some(&mut cc), Some(&mut pi), Some(&mut lcss), None) {
            (*v).pi = pi;
        }
        // the call's own LC fragment for its new position: regen reads it as heard, and passes the LC on
        let frag = if (1..=4).contains(&tpos) {
            addr_of!((*v).regen.lc_raw).cast::<u8>().add(32 * (tpos as usize - 1))
        } else {
            core::ptr::null()
        };
        controls[n] = rdof_jb_reframe(burst, tpos, (*v).cc, true, (*v).pi, frag, &mut moved);
        ffi::rdof_voice_regen_burst(addr_of_mut!((*v).regen), controls[n], moved.as_ptr(), out[n].as_mut_ptr());
    } else {
        controls[n] = control;
        ffi::rdof_voice_regen_burst(addr_of_mut!((*v).regen), control, burst.as_ptr(), out[n].as_mut_ptr());
    }
    n += 1;
    (*v).next = (tpos + 1) % 6;
    (*v).filled = 0;
    n
}

// ── to the modem ──

unsafe fn inactivity(r: *const RdofRepeater) -> f64 {
    if (*r).inactivity_s > (*r).call_hang_s { (*r).inactivity_s } else { (*r).call_hang_s } // R3.6
}

unsafe fn restart(r: R, t: f64) {
    if t + inactivity(r) > (*r).tx_until {
        (*r).tx_until = t + inactivity(r); // R3.4
    }
}

unsafe fn held(s: S, now: f64) -> f64 {
    if (*s).air_until > now { (*s).air_until - now } else { 0.0 }
}

unsafe fn drop_queue(r: R, s: S) {
    let mut ended = [0 as c_int; RDOF_RPT_CALLS];
    let mut n = 0;
    for i in 0..(*s).count {
        let c = c_int::from((*out_at(s, i)).call);
        if c < 0 || !(*call(r, c)).on_air {
            continue;
        }
        let seen = ended[..n].contains(&c);
        if !seen && n < RDOF_RPT_CALLS {
            ended[n] = c;
            n += 1;
        }
    }
    (*s).count = 0;
    (*s).head = 0;
    for &c in &ended[..n] {
        call_end(r, c);
    }
}

unsafe fn aborted(r: R, s: S, now: f64) {
    (*s).space = 0;
    (*s).air_until = now;
    (*s).net_until = f64::NEG_INFINITY;
    (*r).last_status = f64::NEG_INFINITY;
}

unsafe fn abort_slot(r: R, s: S, now: f64) -> bool {
    drop_queue(r, s);
    if !rdof_modem_dmr_abort(modem(r), (*s).n) {
        return false;
    }
    aborted(r, s, now);
    true
}

unsafe fn status(r: R, st: &mut RdofMmdvmStatus) -> bool {
    if !rdof_modem_status(modem(r), st) {
        return false;
    }
    (*r).transmitting = st.transmitting != 0;
    (*r).inbound_flag = (*r).sfr && st.inbound_busy != 0;
    true
}

unsafe fn inbound_busy(r: *const RdofRepeater, now: f64) -> bool {
    (*r).inbound_flag || now - (*r).inbound_last < SFR_QUIET_S
}

#[no_mangle]
pub unsafe extern "C" fn rdof_repeater_busy(r: R, slot: c_int) -> bool {
    if !(1..=2).contains(&slot) {
        return false;
    }
    if (*r).sfr && slot == (*r).outbound && inbound_busy(r, now_of(r)) {
        return true;
    }
    let s = addr_of!((*r).slots[slot as usize]);
    (*s).rf.active || (*s).net >= 0 || (*s).count > 0
}

#[no_mangle]
pub unsafe extern "C" fn rdof_repeater_on_air(r: *const RdofRepeater) -> bool {
    (*r).keyed || (*r).transmitting
}

fn zero_status() -> RdofMmdvmStatus {
    RdofMmdvmStatus { transmitting: 0, carrier: 0, space: 0, space1: 0, inbound_busy: 0 }
}

unsafe fn wait_until_quiet(r: R) -> bool {
    let m = modem(r);
    for _ in 0..150 {
        let mut st = zero_status();
        if !rdof_modem_status(m, &mut st) {
            return false;
        }
        if st.transmitting == 0 {
            return true;
        }
        if let Some(f) = (*m).port.sleep_ms {
            f((*m).port.ctx, 100);
        }
    }
    true
}

unsafe fn key(r: R) -> bool {
    if (*r).keyed {
        return true;
    }
    if !keys_itself(r) && !rdof_modem_dmr_start(modem(r), true) {
        return false;
    }
    (*r).keyed = true;
    if let Some(f) = (*r).on_key {
        f((*r).ctx, true);
    }
    true
}

unsafe fn unkey(r: R, now: f64) -> bool {
    if !keys_itself(r) && !rdof_modem_dmr_start(modem(r), false) {
        return false;
    }
    (*r).keyed = false;
    for n in 1..=2 {
        let s = slot(r, n);
        (*s).has_hang = false;
        (*s).air_until = f64::NEG_INFINITY;
        if !(*s).rf.active && (*s).net < 0 {
            activity_clear(s);
        }
    }
    if let Some(f) = (*r).on_key {
        f((*r).ctx, false);
    }
    if (*r).cwid[0] != 0 && (*r).sent_since_id && now - (*r).last_id >= (*r).cwid_every_s {
        let m = modem(r);
        if !wait_until_quiet(r)
            || !rdof_modem_set_mode(m, MODE_IDLE)
            || !rdof_modem_send_cwid(m, addr_of!((*r).cwid).cast())
            || !wait_until_quiet(r)
            || !rdof_modem_set_mode(m, MODE_DMR)
        {
            return false;
        }
        (*r).last_id = now_of(r);
        (*r).sent_since_id = false;
    }
    true
}

/// R9.4: a new short LC whenever either slot's activity changes, the Null one when both are idle.
unsafe fn update_short_lc(r: R) -> bool {
    let a1 = (*r).slots[1].activity;
    let a2 = (*r).slots[2].activity;
    if (*r).has_short_lc && a1 == (*r).short_lc[0] && a2 == (*r).short_lc[1] {
        return true;
    }
    (*r).has_short_lc = true;
    (*r).short_lc = [a1, a2];
    let mut lc = [0u8; 9];
    ffi::rdof_activity_lc(&a1, &a2, (*r).short_lc_ts1_low, lc.as_mut_ptr());
    rdof_modem_dmr_short_lc(modem(r), lc.as_ptr())
}

// ── calls ──

unsafe fn new_call(r: R) -> c_int {
    for i in 0..RDOF_RPT_CALLS as c_int {
        let mut referenced = false;
        for n in 1..=2 {
            if referenced {
                break;
            }
            let s = slot(r, n);
            if (*s).net == i {
                referenced = true;
            }
            let mut k = 0;
            while k < (*s).count && !referenced {
                referenced = c_int::from((*out_at(s, k)).call) == i;
                k += 1;
            }
        }
        if !referenced {
            let c = call(r, i);
            c.write_bytes(0, 1);
            (*c).used = true;
            (*c).group = true;
            return i;
        }
    }
    -1
}

fn end_marker(call: c_int, kind: c_int, seq: u32, term: Option<&Burst>) -> RdofRptOut {
    RdofRptOut {
        end: true,
        control: 0,
        burst: term.copied().unwrap_or([0; RDOF_BURST_BYTES]),
        call: call as i8,
        report: false,
        end_kind: kind as u8,
        seq,
    }
}

unsafe fn rf_end(r: R, s: S, now: f64) {
    let rf = addr_of_mut!((*s).rf);
    (*rf).active = false;
    (*s).has_last_rf = true;
    (*s).last_src = (*rf).src;
    (*s).last_dst = (*rf).dst;
    (*s).last_at = now;
    if (*r).dcdm {
        return; // nothing repeated (R13.3)
    }
    let o = if (*rf).kind == KIND_VOICE {
        let mut term = [0u8; RDOF_BURST_BYTES];
        ffi::rdof_duplex_lc_burst(addr_of!((*rf).lc9).cast(), RDOF_DT_TERMINATOR_WITH_LC, (*r).cc, term.as_mut_ptr());
        out_push(r, s, &out_item(TERMINATOR, Some(&term), -1, false));
        end_marker(-1, KIND_VOICE, (*s).seq, Some(&term))
    } else {
        end_marker(-1, KIND_DATA, (*s).seq, None)
    };
    out_push(r, s, &o);
}

/// R8.1: an RF call has the slot. The network stream (and anything of the network's queued here or held by the
/// modem) is dropped; RF traffic queued before it stays.
unsafe fn network_yields(r: R, s: S, now: f64) -> bool {
    let mut theirs = false;
    let mut i = 0;
    while i < (*s).count && !theirs {
        theirs = (*out_at(s, i)).call >= 0;
        i += 1;
    }
    if (*s).net < 0 && !theirs && now >= (*s).net_until {
        return true;
    }
    let mut ended = [0 as c_int; RDOF_RPT_CALLS + 1];
    let mut n = 0;
    if (*s).net >= 0 {
        ended[n] = (*s).net;
        n += 1;
        done_add(s, &(*call(r, (*s).net)).tune.stream_id);
        rdof_jb_end(&mut (*r).jb, &mut (*call(r, (*s).net)).jbs);
        (*s).net = -1;
    }
    let mut kept = 0;
    let count = (*s).count;
    for i in 0..count {
        let o = *out_at(s, i);
        if o.call >= 0 {
            let c = c_int::from(o.call);
            let seen = ended[..n].contains(&c);
            if !seen && n < RDOF_RPT_CALLS + 1 {
                ended[n] = c;
                n += 1;
            }
            continue;
        }
        if kept != i {
            *out_at(s, kept) = o; // in place: kept <= i
        }
        kept += 1;
    }
    (*s).count = kept;
    for &c in &ended[..n] {
        call_end(r, c);
    }
    if !rdof_modem_dmr_abort(modem(r), (*s).n) {
        return false;
    }
    aborted(r, s, now);
    true
}

#[allow(clippy::too_many_arguments)]
unsafe fn rf_start(r: R, s: S, kind: c_int, src: u32, dst: u32, group: bool, lc9: Option<&[u8; 9]>, now: f64) -> bool {
    if !network_yields(r, s, now) {
        return false;
    }
    (*s).seq = (*s).seq.wrapping_add(1);
    (*s).has_hang = false; // R3.7
    let rf = addr_of_mut!((*s).rf);
    rf.write_bytes(0, 1);
    (*rf).active = true;
    (*rf).kind = kind;
    (*rf).src = src;
    (*rf).dst = dst;
    (*rf).group = group;
    if let Some(lc9) = lc9 {
        (*rf).lc9 = *lc9;
    }
    (*rf).last = now;
    activity_set(s, kind, dst, group);
    (*s).late_have = 0;
    true
}

unsafe fn rf_voice(r: R, s: S, src: u32, dst: u32, group: bool, lc9: &[u8; 9], now: f64) -> bool {
    if !rf_start(r, s, KIND_VOICE, src, dst, group, Some(lc9), now) {
        return false;
    }
    let v = addr_of_mut!((*s).rf.voice);
    vout_init(v, lc9, (*r).cc);
    let o = out_item(HEADER, Some(&(*v).header), -1, false);
    for _ in 0..(if (*r).dcdm { 0 } else { HEADERS }) {
        out_push(r, s, &o);
    }
    true
}

unsafe fn queue_rf(r: R, s: S, control: u8, burst: &Burst) {
    if (*r).dcdm {
        return; // RF is only followed, to know who has the slot (R13.3)
    }
    out_push(r, s, &out_item(control, Some(burst), -1, false));
}

/// R4.6: late entry. 1 if the call started, 0 if not, -1 on a modem error.
unsafe fn late_entry(r: R, s: S, control: u8, burst: &Burst, now: f64) -> c_int {
    const LCSS_OF: [c_int; 5] = [0, 1, 3, 3, 2];
    if control & RDOF_SYNC_AUDIO != 0 {
        (*s).late_have = 0;
        return 0;
    }
    let n = c_int::from(control & 0x0F);
    let (mut cc, mut lcss) = (-1, -1);
    if !rdof_decode_emb(burst, Some(&mut cc), None, Some(&mut lcss), None) {
        cc = -1;
    }
    if !(1..=4).contains(&n) || cc != (*r).cc || lcss != LCSS_OF[n as usize] {
        return 0;
    }
    ffi::rdof_embedded_signalling(burst.as_ptr(), addr_of_mut!((*s).late[n as usize]).cast());
    (*s).late_have |= 1 << n;
    if n != 4 || (*s).late_have != 0x1E {
        return 0;
    }
    let mut raw = [0u8; 128];
    for i in 0..4 {
        raw[32 * i..32 * i + 32].copy_from_slice(&(*s).late[i + 1]);
    }
    (*s).late_have = 0;
    let mut lc9 = [0u8; 9];
    if !ffi::rdof_embedded_lc_decode(raw.as_ptr(), lc9.as_mut_ptr()) {
        return 0;
    }
    let mut lc: RdofFullLc = core::mem::zeroed();
    rdof_parse_full_lc(&lc9, &mut lc);
    if lc.src_id == RDOF_NONE {
        return 0;
    }
    if rf_voice(r, s, lc.src_id as u32, lc.dst_id as u32, lc.group, &lc9, now) { 1 } else { -1 }
}

/// The slot carries a network voice call (streaming, or its bursts still queued here or in the modem).
unsafe fn net_voice_on(r: R, s: S, now: f64) -> bool {
    if ((*s).net >= 0 && (*call(r, (*s).net)).kind == KIND_VOICE) || now < (*s).net_until {
        return true;
    }
    for i in 0..(*s).count {
        let o = out_at(s, i);
        if (*o).call >= 0 && !(*o).end && (*call(r, c_int::from((*o).call))).kind == KIND_VOICE {
            return true;
        }
    }
    false
}

unsafe fn rf_block(r: R, s: S, control: u8, burst: &Burst, now: f64) -> c_int {
    let mut out = [0u8; RDOF_BURST_BYTES];
    if !ffi::rdof_duplex_data(control, burst.as_ptr(), (*r).cc, out.as_mut_ptr()) {
        return 0;
    }
    (*s).rf.blocks -= 1;
    (*s).rf.last = now;
    queue_rf(r, s, control, &out);
    if (*s).rf.blocks <= 0 {
        rf_end(r, s, now);
    }
    1
}

/// _repeat (section 4): 1 queued, 0 nothing, -1 a modem error.
unsafe fn repeat(r: R, s: S, control: u8, burst: &Burst, now: f64) -> c_int {
    let rf = addr_of_mut!((*s).rf);
    let mut out = [0u8; RDOF_BURST_BYTES];
    if control & RDOF_SYNC_DATA != 0 {
        let dt = c_int::from(control & 0x0F);
        if dt == RDOF_DT_VOICE_LC_HEADER {
            let mut lc: RdofFullLc = core::mem::zeroed();
            let ok = rdof_decode_voice_lc(burst, dt, &mut lc) && lc.src_id != RDOF_NONE;
            if (*rf).active && (*rf).kind == KIND_VOICE {
                if !ok || (lc.src_id as u32 == (*rf).src && lc.dst_id as u32 == (*rf).dst) {
                    (*rf).last = now;
                    return 1; // R4.2: its header again, absorbed
                }
                rf_end(r, s, now); // R4.3
            }
            if !ok {
                return 0;
            }
            return if rf_voice(r, s, lc.src_id as u32, lc.dst_id as u32, lc.group, &lc.raw, now) { 1 } else { -1 };
        }
        if dt == RDOF_DT_TERMINATOR_WITH_LC {
            if !(*rf).active || (*rf).kind != KIND_VOICE {
                return 0;
            }
            rf_end(r, s, now);
            return 1;
        }
        if dt == RDOF_DT_PI_HEADER {
            if !(*rf).active || (*rf).kind != KIND_VOICE || !(*rf).voice.wait_a {
                return 0; // R4.4: after the headers, before A
            }
            if !ffi::rdof_duplex_data(control, burst.as_ptr(), (*r).cc, out.as_mut_ptr()) {
                return 0;
            }
            queue_rf(r, s, control, &out);
            return 1;
        }
        if dt == RDOF_DT_DATA_HEADER {
            let mut h: RdofDataHeader = core::mem::zeroed();
            rdof_decode_data_header(burst, &mut h);
            if (*rf).active && (*rf).kind == KIND_DATA {
                if !h.crc_ok || h.dpf == RDOF_DPF_PROPRIETARY {
                    return rf_block(r, s, control, burst, now); // R4.8: part of the open data call
                }
                rf_end(r, s, now); // R4.8: the next packet (a chain, or a block of this one lost)
            }
            if !h.crc_ok {
                return 0;
            }
            if !rf_start(r, s, KIND_DATA, h.src_id, h.dst_id, h.group, None, now) {
                return -1;
            }
            (*s).rf.blocks = h.blocks;
            if ffi::rdof_duplex_data(control, burst.as_ptr(), (*r).cc, out.as_mut_ptr()) {
                queue_rf(r, s, control, &out);
            }
            if h.blocks == 0 {
                rf_end(r, s, now);
            }
            return 1;
        }
        if dt == RDOF_DT_CSBK {
            let mut c = RdofCsbk::default();
            rdof_decode_csbk(burst, &mut c);
            if !c.crc_ok || c.opcode == BS_DWN_ACT {
                return 0;
            }
            if c.src_id != RDOF_NONE {
                (*s).has_last_rf = true;
                (*s).last_src = c.src_id as u32;
                (*s).last_dst = c.dst_id as u32;
                (*s).last_at = now;
            }
            if net_voice_on(r, s, now) {
                return 0; // R8.3: not into the network's superframes
            }
            if !ffi::rdof_duplex_data(control, burst.as_ptr(), (*r).cc, out.as_mut_ptr()) {
                return 0;
            }
            queue_rf(r, s, control, &out);
            return 1;
        }
        if is_block(dt) {
            if !(*rf).active || (*rf).kind != KIND_DATA || (*rf).blocks <= 0 {
                return 0;
            }
            return rf_block(r, s, control, burst, now);
        }
        return 0; // MBC, USBD (R4.10, not yet), idle (R4.11)
    }
    if (*rf).active && (*rf).kind == KIND_VOICE {
        if control & RDOF_SYNC_AUDIO == 0 && control & 0x0F > 5 {
            return 0;
        }
        (*rf).last = now;
        let mut controls = [0u8; 6];
        let mut bursts = [[0u8; RDOF_BURST_BYTES]; 6];
        let n = vout_take(addr_of_mut!((*rf).voice), control, burst, &mut controls, &mut bursts, None, 0.0, now);
        for i in 0..n {
            queue_rf(r, s, controls[i], &bursts[i]);
        }
        return 1;
    }
    late_entry(r, s, control, burst, now)
}

unsafe fn rf_takes(r: R, s: S, now: f64) -> bool {
    let c = (*s).net;
    (*s).net = -1;
    if c >= 0 {
        done_add(s, &(*call(r, c)).tune.stream_id);
        rdof_jb_end(&mut (*r).jb, &mut (*call(r, c)).jbs);
    }
    if !abort_slot(r, s, now) {
        return false;
    }
    activity_clear(s);
    (*s).has_hang = false;
    call_end(r, c);
    true
}

unsafe fn hear(r: R, slot: c_int, control: u8, burst: &Burst, rssi: c_int) {
    let Some(f) = (*r).receiver.hear else {
        return;
    };
    let mut dbm = 0;
    let d = if rdof_radio_rssi(&*(*r).radio, rssi, (*r).rx_freq, &mut dbm) { f64::from(dbm) } else { f64::NAN };
    f((*r).receiver.ctx, slot, control, burst.as_ptr(), d, (*r).cc, (*r).rx_freq);
}

unsafe fn rf_sfr(r: R, input: &RdofRptRfIn, now: f64) -> bool {
    if input.slot != (*r).inbound {
        return true;
    }
    if input.lost {
        (*r).inbound_last = f64::NEG_INFINITY;
        return true;
    }
    if input.control & RDOF_CONTROL_IDLE != 0 {
        return true; // a wakeup: the radio keys itself
    }
    hear(r, input.slot, input.control, &input.burst, input.rssi);
    if input.control & RDOF_SYNC_DATA != 0 && c_int::from(input.control & 0x0F) == RDOF_DT_TERMINATOR_WITH_LC {
        (*r).inbound_last = f64::NEG_INFINITY; // over: the network may have the slot
        return true;
    }
    (*r).inbound_last = now;
    let out = slot(r, (*r).outbound);
    if (*out).net >= 0 || (*out).count > 0 {
        return rf_takes(r, out, now);
    }
    true
}

unsafe fn rf(r: R, input: &RdofRptRfIn, now: f64) -> bool {
    if (*r).sfr {
        return rf_sfr(r, input, now);
    }
    if !(1..=2).contains(&input.slot) {
        return true;
    }
    let s = slot(r, input.slot);
    if input.lost {
        if (*s).rf.active {
            rf_end(r, s, now);
        }
        return true;
    }
    let control = input.control;
    if control & RDOF_CONTROL_IDLE != 0 {
        // The idle receiver (transmitter off) only passes data sync: BS_Dwn_Act wakes us (R3.1).
        if control & RDOF_SYNC_DATA != 0 && c_int::from(control & 0x0F) == RDOF_DT_CSBK {
            let mut c = RdofCsbk::default();
            rdof_decode_csbk(&input.burst, &mut c);
            if c.crc_ok && c.opcode == BS_DWN_ACT {
                if !key(r) {
                    return false;
                }
                restart(r, now);
            }
        }
        return true;
    }
    hear(r, input.slot, control, &input.burst, input.rssi);
    if (*r).enabled[input.slot as usize] {
        let q = repeat(r, s, control, &input.burst, now);
        if q < 0 {
            return false;
        }
        if q > 0 {
            restart(r, now); // R3.4
        }
    }
    true
}

unsafe fn refused(r: R, command: u8, err: c_int) -> bool {
    let outbound = if (*r).outbound == 1 { DMR_DATA1 } else { DMR_DATA2 };
    if (*r).sfr && command == outbound && err == RDOF_NAK_RF_BUSY {
        (*r).inbound_flag = true;
        let s = slot(r, (*r).outbound);
        if (*s).net >= 0 || (*s).count > 0 {
            return rf_takes(r, s, now_of(r));
        }
    }
    true
}

// ── the network ──

/// DCDM: a network voice call's headers and terminators, as a simplex hotspot's (R11.4).
unsafe fn headers(r: R) -> c_int {
    let h = (*(*r).radio).headers;
    if h > 0 { h } else { 1 }
}

unsafe fn terminators(r: R) -> c_int {
    let t = (*(*r).radio).terminators;
    if t > 0 { t } else { 1 }
}

unsafe fn net_end(r: R, s: S, idx: c_int) {
    let c = call(r, idx);
    let o = if (*c).kind == KIND_VOICE {
        let mut term = [0u8; RDOF_BURST_BYTES];
        ffi::rdof_duplex_lc_burst(addr_of!((*c).lc9).cast(), RDOF_DT_TERMINATOR_WITH_LC, (*r).cc, term.as_mut_ptr());
        for i in 0..(if (*r).dcdm { terminators(r) } else { 1 }) {
            pending_push(r, c, &out_item(TERMINATOR, Some(&term), idx, i == 0)); // DCDM: Ts of them, no hang (R13.5)
        }
        end_marker(idx, KIND_VOICE, (*s).seq, Some(&term))
    } else {
        end_marker(idx, (*c).kind, (*s).seq, None)
    };
    pending_push(r, c, &o);
    for i in 0..(*c).npending as usize {
        let p = (*c).pending[i];
        out_push(r, s, &p);
    }
    (*c).npending = 0;
    (*c).started = true;
    rdof_jb_end(&mut (*r).jb, &mut (*c).jbs);
    if (*c).kind != KIND_DATA {
        done_add(s, &(*c).tune.stream_id); // data: the stream may carry another packet (R7.9)
    }
    (*s).net = -1;
}

/// A network voice or data call: the slot's call hang ends (R3.7, R8.7); the previous call's bursts go first.
unsafe fn net_call_starts(s: S, kind: c_int, c: C) {
    (*s).seq = (*s).seq.wrapping_add(1);
    (*s).has_hang = false;
    activity_set(s, kind, (*c).dst, (*c).group);
}

unsafe fn net_voice(r: R, s: S, idx: c_int, lc9: &[u8; 9]) {
    let c = call(r, idx);
    (*c).kind = KIND_VOICE;
    (*c).lc9 = *lc9;
    vout_init(addr_of_mut!((*c).voice), lc9, (*r).cc);
    (*c).has_voice = true;
    net_call_starts(s, KIND_VOICE, c);
    for i in 0..(if (*r).dcdm { headers(r) as usize } else { HEADERS }) {
        let o = out_item(HEADER, Some(&(*c).voice.header), idx, i == 0);
        pending_push(r, c, &o);
    }
}

unsafe fn net_add(r: R, s: S, idx: c_int, b: &RdofNetBurst, at: f64, now: f64) {
    let c = call(r, idx);
    (*c).last_burst = now;
    (*c).received = (*c).received.wrapping_add(1);
    rdof_jb_key(&mut (*r).jb, &mut (*c).jbs, b.dst, b.group, b.slot, (*c).started);
    rdof_jb_arrival(&mut (*r).jb, &mut (*c).jbs, at, c_int::from(b.seq));
    let control = b.control;
    if control & RDOF_SYNC_DATA != 0 {
        let dt = c_int::from(control & 0x0F);
        if dt == RDOF_DT_VOICE_LC_HEADER {
            if (*c).kind != KIND_VOICE {
                let mut lc: RdofFullLc = core::mem::zeroed();
                let mut lc9 = [0u8; 9];
                if rdof_decode_voice_lc(&b.burst, dt, &mut lc) && lc.src_id != RDOF_NONE {
                    lc9 = lc.raw;
                } else {
                    rdof_group_lc((*c).src, (*c).dst, (*c).group, 0, &mut lc9);
                }
                net_voice(r, s, idx, &lc9);
            }
            return; // R7.2: later headers ignored
        }
        if dt == RDOF_DT_TERMINATOR_WITH_LC {
            if (*c).kind == KIND_VOICE {
                net_end(r, s, idx);
            }
            return;
        }
        if dt == RDOF_DT_PI_HEADER && ((*c).kind != KIND_VOICE || !(*c).voice.wait_a) {
            return; // R7.6: only between the headers and A
        }
        let mut data = [0u8; RDOF_BURST_BYTES];
        let have = ffi::rdof_duplex_data(control, b.burst.as_ptr(), (*r).cc, data.as_mut_ptr());
        if dt == RDOF_DT_DATA_HEADER && (*c).kind != KIND_DATA {
            let mut h: RdofDataHeader = core::mem::zeroed();
            rdof_decode_data_header(&b.burst, &mut h);
            (*c).kind = KIND_DATA;
            (*c).blocks = if h.crc_ok { h.blocks } else { 0 };
            net_call_starts(s, KIND_DATA, c);
        }
        if have {
            pending_push(r, c, &out_item(control, Some(&data), idx, true));
        }
        if dt == RDOF_DT_CSBK && b.terminator && (*c).kind == KIND_NONE {
            (*c).has_csbk = true; // a CSBK on its own (a radio check)
            (*c).csbk = b.burst;
            net_end(r, s, idx);
            return;
        }
        if (*c).kind == KIND_DATA {
            if dt == RDOF_DT_DATA_HEADER && (*c).blocks <= 0 {
                net_end(r, s, idx);
            } else if dt != RDOF_DT_DATA_HEADER && is_block(dt) {
                (*c).blocks -= 1;
                if (*c).blocks <= 0 {
                    net_end(r, s, idx);
                }
            }
        }
        return;
    }
    if (*c).kind != KIND_VOICE {
        // voice with no header: one from its addressing
        let mut lc9 = [0u8; 9];
        rdof_group_lc((*c).src, (*c).dst, (*c).group, 0, &mut lc9);
        net_voice(r, s, idx, &lc9);
    }
    if control & RDOF_SYNC_AUDIO == 0 && control & 0x0F > 5 {
        return;
    }
    let mut controls = [0u8; 6];
    let mut bursts = [[0u8; RDOF_BURST_BYTES]; 6];
    let ahead = f64::from((*s).count) + f64::from((*c).npending) + held(s, now) / BURST_S;
    let jb = Some((addr_of_mut!((*r).jb), addr_of_mut!((*c).jbs)));
    let n = vout_take(addr_of_mut!((*c).voice), control, &b.burst, &mut controls, &mut bursts, jb, ahead, now); // R7.6, R7.7
    for i in 0..n {
        pending_push(r, c, &out_item(controls[i], Some(&bursts[i]), idx, i == n - 1));
    }
}

unsafe fn net(r: R, b: &RdofNetBurst, at: f64, now: f64) -> bool {
    let n = if (*r).sfr { (*r).outbound } else { b.slot }; // SFR: everything goes out on the outbound slot
    if !(1..=2).contains(&n) || !(*r).enabled[n as usize] {
        return true;
    }
    let s = slot(r, n);
    if is_done(s, &b.stream_id) {
        return true;
    }
    if (*s).net >= 0 && (*call(r, (*s).net)).tune.stream_id == b.stream_id {
        net_add(r, s, (*s).net, b, at, now);
        return true;
    }
    if (*s).net >= 0 || (*s).rf.active {
        done_add(s, &b.stream_id); // another stream (R8.5), or an RF call (R8.2), has the slot
        return true;
    }
    if (*r).sfr && inbound_busy(r, now) {
        if now - (*r).inbound_last >= SFR_QUIET_S {
            let mut st = zero_status(); // busy only by the flag, which may be old: ask again
            if !status(r, &mut st) {
                return false;
            }
        }
        if inbound_busy(r, now) {
            done_add(s, &b.stream_id);
            return true;
        }
    }
    if (*s).has_last_rf && (*s).last_src == b.src && (*s).last_dst == b.dst && now - (*s).last_at < ECHO_S {
        done_add(s, &b.stream_id); // our own repeat of it (R8.4)
        return true;
    }
    let idx = new_call(r);
    if idx < 0 {
        drop_one(r);
        done_add(s, &b.stream_id);
        return true;
    }
    let c = call(r, idx);
    (*c).tune.stream_id = b.stream_id;
    (*c).tune.freq = (*r).tx_freq;
    (*c).tune.cc = (*r).cc;
    (*c).tune.power = (*r).power;
    (*c).slot = (*s).n;
    (*c).src = b.src;
    (*c).dst = b.dst;
    (*c).group = b.group;
    (*c).last_burst = now;
    rdof_jb_begin(&mut (*r).jb, &mut (*c).jbs);
    (*s).net = idx;
    if !(*r).keyed && !keys_itself(r) {
        (*r).lead_until = now + KEY_LEAD_S; // R3.3
    }
    if !key(r) {
        return false; // R7.5: keyed while it prebuffers
    }
    restart(r, now);
    net_add(r, s, idx, b, at, now);
    true
}

unsafe fn timeouts(r: R, s: S, now: f64) {
    if (*s).rf.active && now - (*s).rf.last > RF_TIMEOUT_S {
        rf_end(r, s, now);
    }
    if (*s).net >= 0 && now - (*call(r, (*s).net)).last_burst > NET_TIMEOUT_S {
        net_end(r, s, (*s).net);
    }
}

unsafe fn feed(r: R, s: S) {
    if (*s).net < 0 {
        return;
    }
    let c = call(r, (*s).net);
    if !(*c).started && (*c).received >= rdof_jb_prebuffer(&(*r).jb, &(*c).jbs, (*r).prebuffer) {
        (*c).started = true; // R7.4
    }
    if (*c).started && (*c).npending != 0 {
        for i in 0..(*c).npending as usize {
            let p = (*c).pending[i];
            out_push(r, s, &p);
        }
        (*c).npending = 0;
    }
}

/// The voice stream going out on the slot, if one is open (RF, or the network's past its prebuffer).
unsafe fn open_voice(r: R, s: S) -> *mut RdofVoiceOut {
    if (*s).rf.active && (*s).rf.kind == KIND_VOICE {
        return if (*r).dcdm { null_mut() } else { addr_of_mut!((*s).rf.voice) }; // DCDM: RF isn't repeated
    }
    if (*s).net >= 0 {
        let c = call(r, (*s).net);
        if (*c).kind == KIND_VOICE && (*c).started {
            return addr_of_mut!((*c).voice);
        }
    }
    null_mut()
}

unsafe fn write(r: R, s: S, control: u8, burst: *const u8, now: f64) -> bool {
    let mut framed = [0u8; RDOF_BURST_BYTES];
    let mut burst = burst;
    if (*r).dcdm && control & (RDOF_SYNC_AUDIO | RDOF_SYNC_DATA) != 0 {
        // R13.4: the slot's direct mode sync
        let v: u64 = if (*s).n == 1 { 0x5D577F7757FF } else { 0x7DFFD5F55D5F }; // RDOF_TS1_VOICE, RDOF_TS2_VOICE
        framed.as_mut_ptr().copy_from_nonoverlapping(burst, RDOF_BURST_BYTES);
        rdof_set_field(framed.as_mut_ptr(), 108, 48, if control & RDOF_SYNC_DATA != 0 { v ^ RDOF_DATA_XOR } else { v });
        burst = framed.as_ptr();
    }
    if !rdof_modem_write_dmr(modem(r), control, burst, (*s).n) {
        return false;
    }
    (*s).space -= 1;
    (*s).air_until = (if (*s).air_until > now { (*s).air_until } else { now }) + BURST_S;
    (*r).sent_since_id = true;
    true
}

/// Filler, just in time (R10.3): headers while a call waits for voice (R4.2), silence when its voice runs dry
/// (R6.3), the call hang's terminators (R3.5, R3.8).
unsafe fn fill(r: R, s: S, now: f64) -> bool {
    let v = open_voice(r, s);
    if !v.is_null() {
        if (*v).wait_a {
            while (*s).space > 1 && held(s, now) < FILL_AHEAD_S - EPS {
                if !write(r, s, HEADER, addr_of!((*v).header).cast(), now) {
                    return false;
                }
            }
        } else if (*s).space > 1 && held(s, now) <= BURST_S + EPS {
            let mut b = [0u8; RDOF_BURST_BYTES];
            let c = if (*s).net >= 0 && v == addr_of_mut!((*call(r, (*s).net)).voice) { call(r, (*s).net) } else { null_mut() };
            let grow = !c.is_null() && rdof_jb_underrun(&mut (*r).jb, &mut (*c).jbs);
            let control = vout_silence(v, &mut b, grow);
            if !write(r, s, control, b.as_ptr(), now) {
                return false;
            }
        }
        return true;
    }
    if !(*s).has_hang {
        return true;
    }
    let air = |s: S| if (*s).air_until > now { (*s).air_until } else { now };
    while (*s).space > 1 && held(s, now) < FILL_AHEAD_S - EPS && air(s) + BURST_S <= (*s).hang_until + EPS {
        if !write(r, s, TERMINATOR, addr_of!((*s).hang_term).cast(), now) {
            return false;
        }
    }
    if air(s) + BURST_S > (*s).hang_until + EPS && now >= (*s).air_until - EPS {
        (*s).has_hang = false; // channel hang: the activity goes (R3.6, R9.3)
        if !(*s).rf.active && (*s).net < 0 {
            activity_clear(s);
        }
    }
    true
}

/// A call's last burst has gone to the modem.
unsafe fn ended(r: R, s: S, item: &RdofRptOut, now: f64) {
    call_end(r, c_int::from(item.call));
    if item.seq != (*s).seq {
        return; // a later call has the slot
    }
    if c_int::from(item.end_kind) == KIND_VOICE && !(*r).dcdm {
        // R3.5: T_CallHt from its terminator, which the modem holds as its last burst.
        (*s).has_hang = true;
        (*s).hang_term = item.burst;
        (*s).hang_until = (if (*s).air_until > now { (*s).air_until } else { now }) - BURST_S + (*r).call_hang_s;
    } else if c_int::from(item.end_kind) != KIND_NONE && !(*s).rf.active && (*s).net < 0 {
        activity_clear(s); // data, or DCDM voice: no hang
    }
}

unsafe fn transmit(r: R, now: f64) -> bool {
    let mut active = [false; 3];
    let (mut any, mut traffic) = (false, false);
    for n in 1..=2 {
        let s = slot(r, n);
        active[n as usize] = (*s).count > 0 || (*s).has_hang || !open_voice(r, s).is_null();
        any |= active[n as usize];
        traffic |= (*s).count > 0;
    }
    if !any {
        if (*r).keyed
            && now >= (*r).tx_until
            && !(*r).slots[1].rf.active
            && !(*r).slots[2].rf.active
            && (*r).slots[1].net < 0
            && (*r).slots[2].net < 0
        {
            return unkey(r, now); // R3.4
        }
        if now - (*r).last_status >= IDLE_STATUS_EVERY_S {
            let mut st = zero_status();
            if !status(r, &mut st) {
                return false;
            }
            (*r).last_status = now;
        }
        return true;
    }
    if traffic && !key(r) {
        return false;
    }
    let all_low = !(1..=2).any(|n| active[n] && (*r).slots[n].space > 1);
    if now - (*r).last_status >= STATUS_EVERY_S || all_low {
        let mut st = zero_status();
        if !status(r, &mut st) {
            if !(*r).has_status_missed {
                (*r).has_status_missed = true;
                (*r).status_missed = now;
            } else if now - (*r).status_missed > STATUS_GIVE_UP_S {
                return false;
            }
            return true;
        }
        (*r).last_status = now;
        (*r).has_status_missed = false;
        (*r).slots[1].space = st.space1;
        (*r).slots[2].space = st.space;
    }
    for n in 1..=2 {
        if !active[n as usize] {
            continue;
        }
        let s = slot(r, n);
        while (*s).count != 0 {
            let item = *out_at(s, 0);
            if item.end {
                out_pop(s);
                ended(r, s, &item, now);
                continue;
            }
            if (*s).space <= 1 {
                break; // R10.2: write while the modem has room for 2
            }
            if item.call >= 0 && now < (*r).lead_until {
                break; // R3.3: the outbound runs a while before the call
            }
            out_pop(s);
            if !write(r, s, item.control, item.burst.as_ptr(), now) {
                return false;
            }
            if (*s).air_until + inactivity(r) > (*r).tx_until {
                (*r).tx_until = (*s).air_until + inactivity(r); // R3.4
            }
            if item.call >= 0 {
                (*s).net_until = (*s).air_until;
                call_sent(r, c_int::from(item.call), &item, (*s).n);
            }
        }
        if (*s).count == 0 && !fill(r, s, now) {
            return false;
        }
    }
    true
}

unsafe fn fault(r: R) {
    (*r).keyed = false;
    (*r).ready = false;
    for n in 1..=2 {
        let s = slot(r, n);
        drop_queue(r, s);
        if (*s).net >= 0 {
            let c = (*s).net;
            done_add(s, &(*call(r, c)).tune.stream_id);
            rdof_jb_end(&mut (*r).jb, &mut (*call(r, c)).jbs);
            call_end(r, c);
        }
        (*s).rf.active = false;
        (*s).net = -1;
        (*s).has_hang = false;
        (*s).air_until = f64::NEG_INFINITY;
        (*s).net_until = f64::NEG_INFINITY;
        activity_clear(s);
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_repeater_start(r: R) -> bool {
    lock(r);
    (*r).rf_count = 0;
    (*r).net_count = 0;
    (*r).nak_count = 0;
    unlock(r);
    (*r).inbound_last = f64::NEG_INFINITY;
    (*r).inbound_flag = false;
    (*r).transmitting = false;
    let tuned = if (*r).dcdm {
        rdof_radio_tune_dcdm(&*(*r).radio, (*r).tx_freq, (*r).cc, (*r).power)
    } else {
        rdof_radio_tune_duplex(&*(*r).radio, (*r).rx_freq, (*r).tx_freq, (*r).cc, (*r).power)
    };
    if !tuned {
        return false;
    }
    (*r).keyed = false;
    (*r).has_short_lc = true; // as (idle, idle): the Null short LC only once something changes
    addr_of_mut!((*r).short_lc).write_bytes(0, 1);
    for n in 1..=2 {
        (*r).slots[n].has_hang = false;
        (*r).slots[n].air_until = f64::NEG_INFINITY;
        (*r).slots[n].net_until = f64::NEG_INFINITY;
    }
    (*r).last_status = f64::NEG_INFINITY;
    (*r).ready = true;
    true
}

unsafe fn poll(r: R) -> bool {
    let now = now_of(r);
    loop {
        lock(r);
        let input = if (*r).rf_count > 0 {
            let input = (*r).rf_in[(*r).rf_head as usize];
            (*r).rf_head = ((*r).rf_head + 1) % RDOF_RPT_IN as c_int;
            (*r).rf_count -= 1;
            Some(input)
        } else {
            None
        };
        unlock(r);
        let Some(input) = input else { break };
        if !rf(r, &input, now) {
            return false;
        }
    }
    loop {
        lock(r);
        let nak = if (*r).nak_count > 0 {
            let nak = (*r).naks[(*r).nak_head as usize];
            (*r).nak_head = ((*r).nak_head + 1) % NAKS as c_int;
            (*r).nak_count -= 1;
            Some(nak)
        } else {
            None
        };
        unlock(r);
        let Some(nak) = nak else { break };
        if !refused(r, nak.command, nak.err) {
            return false;
        }
    }
    for n in 1..=2 {
        timeouts(r, slot(r, n), now);
    }
    loop {
        lock(r);
        let b = if (*r).net_count > 0 {
            let b = (*r).net_in[(*r).net_head as usize];
            let at = (*r).net_at[(*r).net_head as usize];
            (*r).net_head = ((*r).net_head + 1) % RDOF_RPT_IN as c_int;
            (*r).net_count -= 1;
            Some((b, at))
        } else {
            None
        };
        unlock(r);
        let Some((b, at)) = b else { break };
        if !net(r, &b, at, now) {
            return false;
        }
    }
    for n in 1..=2 {
        feed(r, slot(r, n));
    }
    if !transmit(r, now) {
        return false;
    }
    if !keys_itself(r) && !update_short_lc(r) {
        return false; // no CACH from us (R9.5)
    }
    if let Some(f) = (*r).receiver.tick {
        f((*r).receiver.ctx);
    }
    true
}

#[no_mangle]
pub unsafe extern "C" fn rdof_repeater_poll(r: R) -> bool {
    if poll(r) {
        return true;
    }
    fault(r);
    false
}

#[no_mangle]
pub unsafe extern "C" fn rdof_repeater_stop(r: R) {
    if (*r).keyed && !(*r).dcdm {
        rdof_modem_dmr_start(modem(r), false);
    }
}
