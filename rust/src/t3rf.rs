// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! A trunked site's radio layer (rdof/t3rf.h; docs/spec/fw/tier3.md section 9): the bursts and CACH of a Tier III
//! site's carriers, its payload channels, and the routing of what its receivers hear. The C header is the contract;
//! the structs below are its types field for field (checked by rdof_t3rf_layout against the header's sizes).
//! Written from ETSI TS 102 361-1 (bursts, CACH, short LC, CRC masks) and -4 (the TSCC and payload channels' use of
//! them). The FEC, the voice regeneration and the split stay C (fec.c, duplex.c, split.c, RadioFoundry's CACH codes).

use core::ffi::{c_int, c_void};
use core::ptr::null_mut;

use crate::ffi::{
    dmrCachDeinterleave, dmrCachInterleave, dmrCachTactDecode, dmrCachTactEncode, rdof_bptc196_decode,
    rdof_check_crc16, rdof_crc16_ccitt, rdof_duplex_data, rdof_duplex_lc_burst, rdof_rate1_encode,
    rdof_rate1_payload, rdof_trellis34_decode_block, rdof_trellis34_encode, rdof_voice_regen_burst, rdof_voice_regen_init,
    RDOF_DT_CSBK, RDOF_DT_DATA_HEADER, RDOF_DT_MBC_CONTINUATION, RDOF_DT_MBC_HEADER, RDOF_DT_TERMINATOR_WITH_LC,
    RDOF_DT_VOICE, RDOF_DT_VOICE_LC_HEADER, RDOF_DT_VOICE_SYNC, RDOF_MASK_CSBK, RDOF_MASK_DATA_HEADER,
};
use crate::layer2::{
    rdof_data_burst, rdof_decode_slot_type, rdof_detect_sync, rdof_family_voice_sync, rdof_group_lc, rdof_info_burst,
    RDOF_DATA_XOR,
};
use crate::mmdvm::{rdof_mmdvm_frame, RDOF_SYNC_DATA};
use crate::pdu::{rdof_decode_voice_lc, RdofFullLc};
use crate::repeater::RdofVoiceRegen;
use crate::t3rc::{rc_bits, rc_decode};
use crate::t3site::RC_SEND;
use crate::tracker::{RdofBurst, RdofFrame};

pub const CARRIERS: usize = 4;
pub const CHANNELS: usize = 2 * CARRIERS;
pub const QUEUE: usize = 24;
pub const NET_PREBUFFER: usize = 4;
pub const WARMUP: c_int = 2;
pub const LOST_K: i64 = 17;
pub const BURST_BYTES: usize = 33;

type Burst = [u8; BURST_BYTES];

const MASK_MBC_HEADER: u16 = 0xAAAA; // RDOF_MASK_MBC_HEADER
const DT_IDLE: c_int = 9;
const DT_RATE12: c_int = 7; // rate 1/2 data: a UDT's appended blocks on the TSCC
const FAMILY_BS: u32 = 0;
const SYNC_AUDIO: u8 = 0x20;
const LCSS_FIRST: u8 = 1;
const LCSS_LAST: u8 = 2;
const LCSS_CONT: u8 = 3;

// MMDVM commands (rdof/mmdvm.h)
const DMR_DATA1: u8 = 0x18;
const DMR_DATA2: u8 = 0x1A;
const DMR_SHORTLC: u8 = 0x1C;
const DMR_START: u8 = 0x1D;
const MAX_FRAME: usize = 512;

extern "C" {
    fn rdof_duplex_idle(cc: c_int, out: *mut u8); // duplex.c
    fn rdof_short_lc(lc5: *const u8, out9: *mut u8); // duplex.c
    fn dmrShortLcEncode(slc: u32, rows: *mut u32); // radiofoundry/dmrCach.c
    fn dmrShortLcDecode(rows: *const u32, slc: *mut u32) -> bool;
    // split.c (struct rdof_split stays opaque here)
    fn rdof_split_ready(s: *const c_void) -> bool;
    fn rdof_split_upstream(s: *mut c_void, data: *const u8, n: usize, now: f64);
    fn rdof_split_sched_ok(s: *const c_void) -> bool;
    fn rdof_split_sched(s: *mut c_void, n: i64, control: u8, burst: *const u8) -> bool;
    fn rdof_split_has_cach(s: *const c_void) -> bool;
    fn rdof_split_align_pending(s: *const c_void) -> bool;
    fn rdof_split_sched_cach(s: *mut c_void, n: i64, control: u8, burst: *const u8, cach: *const u8) -> bool;
}

/// rdof_trunk_pdu_t
pub const NOT_PDU: c_int = 0;
pub const PDU_OK: c_int = 1;
pub const PDU_BAD: c_int = 2;

/// rdof_t3rf_io_t's activity
pub const HEADER: c_int = 1;
pub const VOICE: c_int = 2;
pub const TERM: c_int = 3;
pub const LOST: c_int = 4;
pub const DATA: c_int = 5; // a packet data block (rate 1/2, 3/4 or 1) repeated on an open channel

/// rdof_t3rf_out_t
pub const OUT_IDLE: c_int = 0;
pub const OUT_TRAFFIC: c_int = 1;
pub const OUT_CONTROL: c_int = 2;
pub const OUT_HANG: c_int = 3; // a call hang time terminator (361-1 5.2.1.4)

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
    pub rc: c_int,
    pub after_term: bool,
    pub rc_k: i64,
    pub rc_sent: u32,
    pub duplex: bool,
    pub so: u8,
    pub hang: bool,
    pub hang_lc: [u8; 9],
}

/// rdof_t3rf_io_t
#[repr(C)]
#[derive(Clone, Copy)]
pub struct T3rfIo {
    pub tscc: Option<unsafe extern "C" fn(ctx: *mut c_void, k: i64, dt: *mut c_int, pdu: *mut u8) -> bool>,
    pub cach: Option<unsafe extern "C" fn(ctx: *mut c_void, carrier: c_int, k: i64, at: *mut c_int, slc28: *mut i32)>,
    pub pdu: Option<unsafe extern "C" fn(ctx: *mut c_void, ch: c_int, k: i64, dt: c_int, pdu: *const u8, rssi: c_int)>,
    pub bad: Option<unsafe extern "C" fn(ctx: *mut c_void, ch: c_int, k: i64)>,
    pub activity:
        Option<unsafe extern "C" fn(ctx: *mut c_void, ch: c_int, k: i64, what: c_int, src: u32, dst: u32, group: bool)>,
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
    /// A composite TSCC (361-4 5.3.2): channel 0 may carry a call; `rev` says it does in the slot at hand (the
    /// unit sets it per slot from the controller's SlotOut.rev).
    pub composite: bool,
    pub rev: bool,
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
    pub cach_bursts: u32,
    /// the slots the pump took from the controller, by k % SPLIT_SENT: what an air MS hears (rdof_t3rf_split_sent)
    pub sent: [T3rfSent; SPLIT_SENT],
}

pub const SPLIT_SENT: usize = 16;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct T3rfSent {
    pub k: i64,
    pub aired: bool,
    pub burst: [u8; BURST_BYTES],
}

/// Slot k of a split carrier as its pump took it: 1 sent to the half (the burst in burst), 0 not sent (the half's
/// idle), -1 not taken (unkeyed, or gone from the ring).
#[no_mangle]
pub unsafe extern "C" fn rdof_t3rf_split_sent(o: *const T3rfSplit, k: i64, burst: *mut u8) -> c_int {
    let e = &(*o).sent[k.rem_euclid(SPLIT_SENT as i64) as usize];
    if e.k != k || k == 0 {
        return -1;
    }
    if !e.aired {
        return 0;
    }
    core::ptr::copy_nonoverlapping(e.burst.as_ptr(), burst, BURST_BYTES);
    1
}

// ── control blocks ─────────────────────────────────────────────────────────────

#[no_mangle]
pub extern "C" fn rdof_t3rf_mask(dt: c_int) -> c_int {
    match dt {
        RDOF_DT_CSBK => RDOF_MASK_CSBK as c_int,
        RDOF_DT_MBC_HEADER => MASK_MBC_HEADER as c_int,
        // a last block's CRC takes no mask: it covers every block but the header (361-1 7.4.0, B.3.12). The site's
        // MBCs are two blocks, so it covers the last block alone
        RDOF_DT_MBC_CONTINUATION => 0,
        RDOF_DT_DATA_HEADER => RDOF_MASK_DATA_HEADER as c_int,
        _ => -1,
    }
}

#[no_mangle]
pub extern "C" fn rdof_t3rf_pdu_burst(dt: c_int, pdu: &[u8; 10], cc: c_int, family: u32, out: &mut Burst) -> bool {
    let mask = rdof_t3rf_mask(dt);
    if mask < 0 {
        return false;
    }
    let mut b12 = [0u8; 12];
    b12[..10].copy_from_slice(pdu);
    let crc = unsafe { rdof_crc16_ccitt(b12.as_ptr(), 10) } ^ mask as u16;
    b12[10..].copy_from_slice(&crc.to_be_bytes());
    rdof_data_burst(&b12, dt, cc & 0x0F, rdof_family_voice_sync(family) ^ RDOF_DATA_XOR, out);
    true
}

/// A block's burst: a control block (as rdof_t3rf_pdu_burst, octets 10 and 11 ignored) or a data block as given
/// (361-1 8.2.2: rate 1/2 12 octets, BPTC(196,96); rate 3/4 18, the B.2 trellis; rate 1 24, uncoded; its CRCs are
/// the message's). blk: 12 octets, 18 for rate 3/4, 24 for rate 1.
///
/// # Safety
/// blk holds the octets its data type says.
#[no_mangle]
pub unsafe extern "C" fn rdof_t3rf_block_burst(
    dt: c_int,
    blk: *const u8,
    cc: c_int,
    family: u32,
    out: &mut Burst,
) -> bool {
    let sync = rdof_family_voice_sync(family) ^ RDOF_DATA_XOR;
    if dt == DT_RATE34 || dt == DT_RATE1 {
        let mut info = [0u8; 196];
        if dt == DT_RATE34 {
            rdof_trellis34_encode(blk, info.as_mut_ptr());
        } else {
            rdof_rate1_encode(blk, info.as_mut_ptr());
        }
        rdof_info_burst(&info, dt, cc & 0x0F, sync, out);
        return true;
    }
    let b12 = &*blk.cast::<[u8; 12]>();
    if dt == DT_RATE12 {
        rdof_data_burst(b12, dt, cc & 0x0F, sync, out);
        return true;
    }
    let mut pdu = [0u8; 10];
    pdu.copy_from_slice(&b12[..10]);
    rdof_t3rf_pdu_burst(dt, &pdu, cc, family, out)
}

/// A burst read back as rdof_t3rf_burst_pdu, and a data block too: rate 1/2 (PDU_OK when its BPTC decodes), rate 3/4
/// (the trellis always finds a path) and rate 1 (uncoded): PDU_OK, their CRCs left to the message. blk: the block's
/// octets (12, 18 or 24; a control block's 12, its CRC the last two).
#[no_mangle]
pub extern "C" fn rdof_t3rf_burst_block(burst: &Burst, dt: &mut c_int, cc: &mut c_int, blk: &mut [u8; 24]) -> c_int {
    let mut pdu = [0u8; 10];
    let r = rdof_t3rf_burst_pdu(burst, dt, cc, &mut pdu);
    if r == NOT_PDU && *dt == DT_RATE12 {
        return if unsafe { rdof_bptc196_decode(burst.as_ptr(), blk.as_mut_ptr()) } { PDU_OK } else { PDU_BAD };
    }
    if r == NOT_PDU && *dt == DT_RATE34 {
        let _ = unsafe { rdof_trellis34_decode_block(burst.as_ptr(), blk.as_mut_ptr()) };
        return PDU_OK;
    }
    if r == NOT_PDU && *dt == DT_RATE1 {
        unsafe { rdof_rate1_payload(burst.as_ptr(), blk.as_mut_ptr()) };
        return PDU_OK;
    }
    if r == PDU_OK {
        let _ = unsafe { rdof_bptc196_decode(burst.as_ptr(), blk.as_mut_ptr()) };
    }
    r
}

#[no_mangle]
pub extern "C" fn rdof_t3rf_burst_pdu(burst: &Burst, dt: &mut c_int, cc: &mut c_int, pdu: &mut [u8; 10]) -> c_int {
    *dt = -1;
    *cc = -1;
    let (mut fam, mut data, mut e) = (0u32, false, 0);
    if !rdof_detect_sync(burst, 4, Some(&mut fam), Some(&mut data), Some(&mut e)) || !data {
        return NOT_PDU;
    }
    if !rdof_decode_slot_type(burst, Some(&mut *cc), Some(&mut *dt), Some(&mut e)) {
        *dt = -1;
        *cc = -1;
        return PDU_BAD; // a data sync with an unreadable slot type: two radios at once, most likely
    }
    let mask = rdof_t3rf_mask(*dt);
    if mask < 0 {
        return NOT_PDU;
    }
    let mut b12 = [0u8; 12];
    if !unsafe { rdof_bptc196_decode(burst.as_ptr(), b12.as_mut_ptr()) && rdof_check_crc16(b12.as_ptr(), mask as u16) }
    {
        return PDU_BAD;
    }
    pdu.copy_from_slice(&b12[..10]);
    PDU_OK
}

fn pack24(bits: u32, out: &mut [u8; 3]) {
    out.copy_from_slice(&bits.to_be_bytes()[1..]);
}

#[no_mangle]
pub extern "C" fn rdof_t3rf_cach4(slc28: u32, at: &[c_int; 4], k: i64, out: &mut [[u8; 3]; 4]) {
    const LCSS: [u8; 4] = [LCSS_FIRST, LCSS_CONT, LCSS_CONT, LCSS_LAST];
    let mut rows = [0u32; 4];
    unsafe { dmrShortLcEncode(slc28 & 0x0FFF_FFFF, rows.as_mut_ptr()) };
    for q in 0..4 {
        let tc = ((k + q as i64) & 1) as u8;
        let tact = unsafe { dmrCachTactEncode((at[q] & 1) as u8, tc, LCSS[q]) };
        pack24(unsafe { dmrCachInterleave(tact, rows[q]) }, &mut out[q]);
    }
}

#[no_mangle]
pub extern "C" fn rdof_t3rf_cach_read(
    c3: &[u8; 3],
    at: &mut c_int,
    slot: &mut c_int,
    lcss: &mut c_int,
    payload17: &mut u32,
) -> bool {
    let (mut tact, mut a, mut tc, mut ls, mut payload) = (0u8, 0u8, 0u8, 0u8, 0u32);
    unsafe {
        dmrCachDeinterleave(u32::from_be_bytes([0, c3[0], c3[1], c3[2]]), &mut tact, &mut payload);
        dmrCachTactDecode(tact, &mut a, &mut tc, &mut ls); // Hamming(7,4) always decodes
    }
    *at = a.into();
    *slot = if tc != 0 { 2 } else { 1 };
    *lcss = ls.into();
    *payload17 = payload;
    true
}

#[no_mangle]
pub extern "C" fn rdof_t3rf_slc_join(rows: &[u32; 4], slc28: &mut u32) -> bool {
    unsafe { dmrShortLcDecode(rows.as_ptr(), slc28) }
}

// ── payload channels ───────────────────────────────────────────────────────────

fn is_payload(t: &T3rf, ch: c_int) -> bool {
    ch >= 0 && ch < 2 * t.ncarriers && !(t.tscc && ch == 0 && !t.composite)
}

/// Channel 0 sends or hears the TSCC in slot k (not a composite TSCC's call).
fn tscc_slot(t: &T3rf, ch: c_int) -> bool {
    t.tscc && ch == 0 && !t.rev
}

fn channel(t: &mut T3rf, ch: c_int) -> Option<&mut T3rfCh> {
    if is_payload(t, ch) {
        Some(&mut t.ch[ch as usize])
    } else {
        None
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3rf_channel(t: *mut T3rf, ch: c_int) -> *mut T3rfCh {
    channel(&mut *t, ch).map_or(null_mut(), |c| c as *mut T3rfCh)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3rf_init(
    t: *mut T3rf,
    io: *const T3rfIo,
    ncarriers: c_int,
    cc: c_int,
    tscc: bool,
) -> bool {
    t.write_bytes(0, 1);
    let t = &mut *t;
    if !(1..=CARRIERS as c_int).contains(&ncarriers) {
        return false;
    }
    if !io.is_null() {
        t.io = *io;
    }
    t.ncarriers = ncarriers;
    t.cc = cc & 0x0F;
    t.tscc = tscc;
    let cc = t.cc;
    for (i, c) in t.ch.iter_mut().enumerate() {
        c.index = i as c_int;
        c.cc = cc;
    }
    true
}

fn flush(c: &mut T3rfCh) {
    c.qh = 0;
    c.qn = 0;
    c.net = false;
    c.net_started = false;
    c.net_regen_on = false;
    c.net_held = 0;
}

fn push(c: &mut T3rfCh, control: u8, net: bool, b: &Burst) {
    if c.qn == QUEUE as c_int {
        c.overflows += 1;
        return;
    }
    let q = &mut c.q[(c.qh + c.qn) as usize % QUEUE];
    q.control = control;
    q.net = net;
    q.b = *b;
    c.qn += 1;
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3rf_open(
    t: *mut T3rf,
    ch: c_int,
    src: u32,
    dst: u32,
    group: bool,
    warmup: c_int,
) -> bool {
    let Some(c) = channel(&mut *t, ch) else { return false };
    // a channel still sending its last call's P_CLEARs (close_after) is granted again: a fresh start, or the
    // close would come under the new call
    if !c.open || c.close_after {
        flush(c);
        c.rf = false;
        c.rf_regen_on = false;
        c.ctl_left = 0;
        c.close_after = false;
        c.warmup = warmup.max(0);
        c.hang = false;
    }
    c.open = true;
    c.src = src;
    c.dst = dst;
    c.group = group;
    true
}

fn close(t: &mut T3rf, ch: c_int) {
    let Some(c) = channel(t, ch) else { return };
    flush(c);
    c.duplex = false;
    c.so = 0;
    c.hang = false;
    c.open = false;
    c.rf = false;
    c.rf_regen_on = false;
    c.ctl_left = 0;
    c.warmup = 0;
    c.close_after = false;
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3rf_close(t: *mut T3rf, ch: c_int) {
    close(&mut *t, ch);
}

/// Payload channel ch is one party's of a full-duplex call (361-4 6.6.9.4): its carrier in offset timing (its
/// inbound in the carrier's other inbound slot, 361-1 5.1.1.2), its inbound repeated on the other channel's
/// outbound. Closing the channel ends it.
#[no_mangle]
pub unsafe extern "C" fn rdof_t3rf_duplex(t: *mut T3rf, ch: c_int, on: bool) -> bool {
    match channel(&mut *t, ch) {
        Some(c) => {
            c.duplex = on;
            true
        }
        None => false,
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3rf_control(
    t: *mut T3rf,
    ch: c_int,
    dt: c_int,
    pdu: &[u8; 10],
    repeats: c_int,
    close_after: bool,
) -> bool {
    let Some(c) = channel(&mut *t, ch) else { return false };
    if repeats < 1 || !rdof_t3rf_pdu_burst(dt, pdu, c.cc, FAMILY_BS, &mut c.ctl) {
        return false;
    }
    c.ctl_left = repeats;
    c.close_after = close_after;
    if close_after {
        flush(c); // the channel is being cleared: nothing after the PDU
    }
    true
}

/// The LC the site makes for a call it joins without one (late entry, network voice): the call's Service Options
/// kept (361-2 5.3.2.1, 5.3.3.1, table 7.11: Emergency, Broadcast).
fn lc_of(src: u32, dst: u32, group: bool, so: u8) -> [u8; 9] {
    let mut lc9 = [0u8; 9];
    rdof_group_lc(src, dst, group, c_int::from(so), &mut lc9);
    lc9
}

unsafe fn up(io: &T3rfIo, c: &T3rfCh, dt: c_int, n: c_int, b: &Burst, new_stream: bool) {
    let Some(uplink) = io.uplink else { return };
    let mut f: RdofFrame = core::mem::zeroed();
    f.slot = (c.index & 1) + 1;
    f.dt = dt;
    f.n = n;
    f.burst = *b;
    f.has_addr = true;
    f.src = c.rf_src;
    f.dst = c.rf_dst;
    f.group = c.rf_group;
    f.new_stream = new_stream;
    uplink(io.ctx, c.index, &f);
}

unsafe fn activity(io: &T3rfIo, c: &T3rfCh, k: i64, what: c_int) {
    if let Some(a) = io.activity {
        a(io.ctx, c.index, k, what, c.rf_src, c.rf_dst, c.rf_group);
    }
}

/// RF beats the network (tier3.md 3.6): a network stream on the channel is dropped.
fn rf_takes(c: &mut T3rfCh) {
    if c.net {
        c.net_dropped += 1;
        flush(c);
    }
}

const DT_RATE34: c_int = 8;
const DT_RATE1: c_int = 10;

/// Packet data on an open channel (tier3.md 3.16): regenerated with the BS sync and repeated, given to the uplink
/// (the site's data, when it is a party), and a block counted (activity DATA; the header goes to io.pdu).
unsafe fn data_rx(io: &T3rfIo, c: &mut T3rfCh, k: i64, b: &RdofBurst, header: bool) {
    let mut out = [0u8; BURST_BYTES];
    let control = RDOF_SYNC_DATA | b.dt as u8;
    if !rdof_duplex_data(control, b.burst.as_ptr(), c.cc, out.as_mut_ptr()) {
        return;
    }
    rf_takes(c);
    c.rf_src = c.src;
    c.rf_dst = c.dst;
    c.rf_group = c.group;
    push(c, control, false, &out);
    up(io, c, b.dt, 0, &out, header);
    if !header {
        activity(io, c, k, DATA);
    }
}

/// Puts a data burst (a header or block the site sends: control as MMDVM's) on open channel ch's outbound queue,
/// regenerated with the BS sync and the channel's colour code; false when closed, not data or the queue is full.
#[no_mangle]
pub unsafe extern "C" fn rdof_t3rf_data(t: *mut T3rf, ch: c_int, control: u8, burst: *const u8) -> bool {
    let Some(c) = channel(&mut *t, ch) else { return false };
    let dt = c_int::from(control & 0x0F);
    if !c.open || burst.is_null() || !matches!(dt, RDOF_DT_DATA_HEADER | DT_RATE12 | DT_RATE34 | DT_RATE1) {
        return false;
    }
    if c.qn >= QUEUE as c_int {
        return false;
    }
    let mut out = [0u8; BURST_BYTES];
    let control = RDOF_SYNC_DATA | dt as u8;
    if !rdof_duplex_data(control, burst, c.cc, out.as_mut_ptr()) {
        return false;
    }
    push(c, control, false, &out);
    true
}

/// Room on open channel ch's outbound queue (bursts).
#[no_mangle]
pub unsafe extern "C" fn rdof_t3rf_room(t: *const T3rf, ch: c_int) -> c_int {
    let t = &*t;
    if !is_payload(t, ch) || !t.ch[ch as usize].open {
        return 0;
    }
    QUEUE as c_int - t.ch[ch as usize].qn
}

fn is_voice(dt: c_int) -> bool {
    dt == RDOF_DT_VOICE || dt == RDOF_DT_VOICE_SYNC
}

/// An inbound burst on payload channel c, repeated (regenerated) on its outbound when it is open.
unsafe fn payload_rx(io: &T3rfIo, c: &mut T3rfCh, k: i64, b: &RdofBurst, rssi: c_int) {
    c.rx_bursts += 1;
    let mut out = [0u8; BURST_BYTES];
    if !is_voice(b.dt) && rdof_t3rf_mask(b.dt) >= 0 {
        let (mut dt, mut cc, mut pdu) = (0, 0, [0u8; 10]);
        if rdof_t3rf_burst_pdu(&b.burst, &mut dt, &mut cc, &mut pdu) == PDU_OK {
            c.rx_pdus += 1;
            if b.dt == RDOF_DT_DATA_HEADER && c.open {
                data_rx(io, c, k, b, true); // packet data (tier3.md 3.16): repeated, the controller counts it
            }
            if let Some(p) = io.pdu {
                p(io.ctx, c.index, k, dt, pdu.as_ptr(), rssi);
            }
        }
        return; // the controller answers control blocks itself: nothing is repeated
    }
    if !c.open {
        return;
    }
    if b.dt == RDOF_DT_VOICE_LC_HEADER || b.dt == RDOF_DT_TERMINATOR_WITH_LC {
        let mut lc: RdofFullLc = core::mem::zeroed();
        if !rdof_decode_voice_lc(&b.burst, b.dt, &mut lc) {
            return;
        }
        if b.dt == RDOF_DT_VOICE_LC_HEADER {
            rf_takes(c);
            let fresh = !c.rf;
            c.rf = true;
            c.rf_src = if lc.src_id >= 0 { lc.src_id as u32 } else { c.src };
            c.rf_dst = if lc.dst_id >= 0 { lc.dst_id as u32 } else { c.dst };
            c.rf_group = if lc.src_id >= 0 { lc.group } else { c.group };
            rdof_voice_regen_init(&mut c.rf_regen, lc.raw.as_ptr(), c.cc);
            c.rf_regen_on = true;
            c.rf_last = k;
            rdof_duplex_lc_burst(lc.raw.as_ptr(), RDOF_DT_VOICE_LC_HEADER, c.cc, out.as_mut_ptr());
            push(c, RDOF_SYNC_DATA | RDOF_DT_VOICE_LC_HEADER as u8, false, &out);
            if fresh {
                activity(io, c, k, HEADER);
            }
            up(io, c, b.dt, 0, &out, fresh);
            return;
        }
        if !c.rf {
            return; // a terminator of nothing heard
        }
        rdof_duplex_lc_burst(lc.raw.as_ptr(), RDOF_DT_TERMINATOR_WITH_LC, c.cc, out.as_mut_ptr());
        push(c, RDOF_SYNC_DATA | RDOF_DT_TERMINATOR_WITH_LC as u8, false, &out);
        up(io, c, b.dt, 0, &out, false);
        activity(io, c, k, TERM);
        c.rf = false;
        c.rf_regen_on = false;
        return;
    }
    if !is_voice(b.dt) {
        if matches!(b.dt, DT_RATE12 | DT_RATE34 | DT_RATE1) {
            data_rx(io, c, k, b, false);
        }
        return;
    }
    let mut late = false;
    if !c.rf_regen_on {
        // late entry: the channel's call
        rf_takes(c);
        c.rf_src = c.src;
        c.rf_dst = c.dst;
        c.rf_group = c.group;
        let lc9 = lc_of(c.src, c.dst, c.group, c.so);
        rdof_voice_regen_init(&mut c.rf_regen, lc9.as_ptr(), c.cc);
        c.rf_regen_on = true;
        c.rf = true;
        late = true;
    }
    c.rf_last = k;
    let control = if b.dt == RDOF_DT_VOICE_SYNC { SYNC_AUDIO } else { (b.n & 0x0F) as u8 };
    rdof_voice_regen_burst(&mut c.rf_regen, control, b.burst.as_ptr(), out.as_mut_ptr());
    push(c, control, false, &out);
    if late || b.dt == RDOF_DT_VOICE_SYNC {
        activity(io, c, k, VOICE); // and once a superframe: the controller's talker is still there
    }
    up(io, c, b.dt, b.n, &out, late);
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3rf_rx(t: *mut T3rf, c: c_int, k: i64, b: *const RdofBurst, rssi: c_int) {
    let t = &mut *t;
    if c < 0 || c >= t.ncarriers || b.is_null() {
        t.rx_ignored += 1;
        return;
    }
    let b = &*b;
    let ch = 2 * c + (k & 1) as c_int;
    if tscc_slot(t, ch) {
        let (mut dt, mut cc, mut pdu) = (0, 0, [0u8; 24]);
        match rdof_t3rf_burst_block(&b.burst, &mut dt, &mut cc, &mut pdu) {
            PDU_OK => {
                if cc != t.cc {
                    t.rx_ignored += 1; // another site's, or a stray: not ours
                    return;
                }
                t.rx_tscc += 1;
                if let Some(p) = t.io.pdu {
                    p(t.io.ctx, 0, k, dt, pdu.as_ptr(), rssi);
                }
            }
            PDU_BAD => {
                t.rx_tscc_bad += 1;
                if let Some(bad) = t.io.bad {
                    bad(t.io.ctx, 0, k);
                }
            }
            _ => t.rx_ignored += 1, // voice on the TSCC: nothing to do with it
        }
        return;
    }
    if b.cc >= 0 && b.cc != t.cc {
        // the payload channels use the TSCC's color code (tier3.md 3.1)
        t.rx_ignored += 1;
        return;
    }
    let io = t.io;
    // offset timing on a duplex carrier: this inbound slot is the other channel's
    let ch = if t.ch[(2 * c) as usize].duplex || t.ch[(2 * c + 1) as usize].duplex { ch ^ 1 } else { ch };
    let c = &mut t.ch[ch as usize];
    let before = c.qn;
    payload_rx(&io, c, k, b, rssi);
    if c.duplex && c.qn > before {
        // the repeat goes out on the other party's slot
        // (one burst a call: the header, a voice burst or the terminator)
        let q = c.q[((c.qh + before) as usize) % QUEUE];
        c.qn = before;
        push(&mut t.ch[(ch ^ 1) as usize], q.control, q.net, &q.b);
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3rf_net(t: *mut T3rf, ch: c_int, f: *const RdofFrame) -> bool {
    let Some(c) = channel(&mut *t, ch) else { return false };
    if !c.open || c.rf || c.close_after {
        c.net_dropped += 1;
        return false;
    }
    let f = &*f;
    let mut out = [0u8; BURST_BYTES];
    let control;
    if f.dt == RDOF_DT_VOICE_LC_HEADER || f.dt == RDOF_DT_TERMINATOR_WITH_LC {
        let mut lc: RdofFullLc = core::mem::zeroed();
        let lc9 = if rdof_decode_voice_lc(&f.burst, f.dt, &mut lc) {
            lc.raw
        } else if f.has_addr {
            lc_of(f.src, f.dst, f.group, c.so)
        } else {
            lc_of(c.src, c.dst, c.group, c.so)
        };
        if f.dt == RDOF_DT_VOICE_LC_HEADER {
            if !c.net {
                c.net = true;
                c.net_started = false;
                c.net_held = 0;
            }
            rdof_voice_regen_init(&mut c.net_regen, lc9.as_ptr(), c.cc);
            c.net_regen_on = true;
        } else if !c.net {
            c.net_dropped += 1;
            return false;
        }
        rdof_duplex_lc_burst(lc9.as_ptr(), f.dt, c.cc, out.as_mut_ptr());
        control = RDOF_SYNC_DATA | f.dt as u8;
    } else if is_voice(f.dt) {
        if !c.net_regen_on {
            // joined late: the addresses it gives, else the channel's
            let lc9 = if f.has_addr { lc_of(f.src, f.dst, f.group, c.so) } else { lc_of(c.src, c.dst, c.group, c.so) };
            rdof_voice_regen_init(&mut c.net_regen, lc9.as_ptr(), c.cc);
            c.net_regen_on = true;
            c.net = true;
            c.net_started = false;
            c.net_held = 0;
        }
        control = if f.dt == RDOF_DT_VOICE_SYNC { SYNC_AUDIO } else { (f.n & 0x0F) as u8 };
        rdof_voice_regen_burst(&mut c.net_regen, control, f.burst.as_ptr(), out.as_mut_ptr());
    } else {
        c.net_dropped += 1; // data and control blocks from the network: not carried here
        return false;
    }
    if c.qn == QUEUE as c_int {
        c.overflows += 1;
        return false;
    }
    push(c, control, true, &out);
    c.net_bursts += 1;
    c.net_held += 1;
    if f.dt == RDOF_DT_TERMINATOR_WITH_LC || c.net_held >= NET_PREBUFFER as c_int {
        c.net_started = true;
    }
    if f.dt == RDOF_DT_TERMINATOR_WITH_LC {
        c.net = false; // what's queued plays out
        c.net_regen_on = false;
    }
    true
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3rf_net_end(t: *mut T3rf, ch: c_int) {
    let Some(c) = channel(&mut *t, ch) else { return };
    if !c.net {
        return;
    }
    if c.net_regen_on && !c.duplex {
        // ended without its terminator: the site sends one (the call hang time follows, 361-1 5.2.1.4)
        let mut out = [0u8; BURST_BYTES];
        rdof_duplex_lc_burst(c.net_regen.lc9.as_ptr(), RDOF_DT_TERMINATOR_WITH_LC, c.cc, out.as_mut_ptr());
        push(c, RDOF_SYNC_DATA | RDOF_DT_TERMINATOR_WITH_LC as u8, true, &out);
    }
    c.net_started = true;
    c.net = false;
    c.net_regen_on = false;
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3rf_tick(t: *mut T3rf, k: i64) {
    let t = &mut *t;
    let io = t.io;
    for i in 0..2 * t.ncarriers {
        let Some(c) = channel(t, i) else { continue };
        if c.rf && k - c.rf_last > LOST_K {
            if c.rf_regen_on && !c.duplex {
                // the talker went without its terminator: the site sends one (the call hang time follows,
                // 361-1 5.2.1.4)
                let mut out = [0u8; BURST_BYTES];
                rdof_duplex_lc_burst(c.rf_regen.lc9.as_ptr(), RDOF_DT_TERMINATOR_WITH_LC, c.cc, out.as_mut_ptr());
                push(c, RDOF_SYNC_DATA | RDOF_DT_TERMINATOR_WITH_LC as u8, false, &out);
            }
            c.rf = false;
            c.rf_regen_on = false;
            c.lost += 1;
            activity(&io, c, k, LOST);
        }
    }
}

// ── the Reverse Channel (361-1 5.1.5, 6.4.2, 9.1.5, B.2.2.2) ─────────────────

/// 361-1 5.1.5.1: RC messages at least 360 ms apart.
const RC_EVERY_K: i64 = 12;
const VOICE_F: u8 = 5;

/// RC info in a burst's 48-bit middle: the EMB (CC, PI = 1, LCSS single fragment) around the 32 RC bits.
fn rc_embed(burst: &mut Burst, cc: c_int, info: u8) {
    let mut mid = [0u8; 48];
    crate::layer2::rdof_voice_middle(cc, 0, &rc_bits(info), 1, &mut mid);
    for (j, &bit) in mid.iter().enumerate() {
        let (n, m) = ((108 + j) / 8, 0x80u8 >> ((108 + j) % 8));
        burst[n] = if bit != 0 { burst[n] | m } else { burst[n] & !m };
    }
}

#[no_mangle]
pub extern "C" fn rdof_t3rf_rc_read(burst: &Burst) -> c_int {
    let (mut pi, mut lcss) = (0, 0);
    if !crate::layer2::rdof_decode_emb(burst, None, Some(&mut pi), Some(&mut lcss), None) || pi != 1 || lcss != 0 {
        return -1; // an RC burst's EMB: PI = 1, single fragment
    }
    let mut tx = [0u8; 32];
    for (j, b) in tx.iter_mut().enumerate() {
        *b = burst[(116 + j) / 8] >> (7 - (116 + j) % 8) & 1;
    }
    rc_decode(&tx).map_or(-1, c_int::from)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3rf_rc(t: *mut T3rf, ch: c_int, rc: c_int) {
    let t = &mut *t;
    if (0..2 * t.ncarriers).contains(&ch) {
        let c = &mut t.ch[ch as usize];
        if rc < 0 {
            c.rc = 0;
        } else if rc & RC_SEND as c_int != 0 {
            c.rc = rc; // goes once, in the next burst that may carry it
        }
    }
}

/// The pending RC into the burst going out on c in slot k, where one may go: an idle burst (a dedicated RC) or
/// voice burst F (an embedded RC). True when it went.
fn rc_put(c: &mut T3rfCh, k: i64, kind: c_int, control: u8, out: &mut Burst) -> bool {
    let fits = match kind {
        OUT_IDLE => true,
        OUT_TRAFFIC => control == VOICE_F, // no sync flag: a voice burst, F the sixth
        _ => false,
    };
    if c.rc == 0 || !fits || c.after_term || k - c.rc_k < RC_EVERY_K {
        return false;
    }
    rc_embed(out, c.cc, (c.rc & 0x0F) as u8);
    c.rc = 0;
    c.rc_k = k;
    c.rc_sent += 1;
    true
}

fn ch_next(t: &mut T3rf, ch: c_int, k: i64, control: &mut u8, out: &mut Burst) -> c_int {
    let mut kind = ch_burst(t, ch, control, out);
    if let Some(c) = channel(t, ch) {
        if kind == OUT_HANG && c.rc != 0 && !c.after_term && k - c.rc_k >= RC_EVERY_K {
            // the RC for the other slot's talker in place of a hang time terminator: an idle burst carries it (a
            // dedicated RC, 361-1 5.1.5.2), not the first burst after the voice (5.1.5.1)
            *control = RDOF_SYNC_DATA | DT_IDLE as u8;
            unsafe { rdof_duplex_idle(c.cc, out.as_mut_ptr()) };
            kind = OUT_IDLE;
        }
        if rc_put(c, k, kind, *control, out) && kind == OUT_IDLE {
            c.after_term = false;
            return OUT_CONTROL; // signalling, not idle: a split half sends it
        }
        c.after_term = kind == OUT_TRAFFIC && *control == RDOF_SYNC_DATA | RDOF_DT_TERMINATOR_WITH_LC as u8;
    }
    kind
}

fn ch_burst(t: &mut T3rf, ch: c_int, control: &mut u8, out: &mut Burst) -> c_int {
    *control = RDOF_SYNC_DATA | DT_IDLE as u8;
    let cc = t.cc;
    let Some(c) = channel(t, ch) else {
        unsafe { rdof_duplex_idle(cc, out.as_mut_ptr()) };
        return OUT_IDLE;
    };
    if c.ctl_left > 0 {
        *out = c.ctl;
        let (mut bcc, mut dt, mut e) = (0, 0, 0);
        let dt =
            if rdof_decode_slot_type(out, Some(&mut bcc), Some(&mut dt), Some(&mut e)) { dt } else { RDOF_DT_CSBK };
        *control = RDOF_SYNC_DATA | dt as u8;
        c.tx_control += 1;
        c.ctl_left -= 1;
        if c.ctl_left == 0 && c.close_after {
            close(t, ch);
        }
        return OUT_CONTROL;
    }
    if c.open && c.warmup > 0 {
        c.warmup -= 1;
    } else if c.qn != 0 && (!c.q[c.qh as usize].net || c.net_started) {
        let q = &c.q[c.qh as usize];
        *out = q.b;
        *control = q.control;
        c.qh = (c.qh + 1) % QUEUE as c_int;
        c.qn -= 1;
        if c.qn == 0 && !c.net {
            c.net_started = false;
        }
        c.tx_traffic += 1;
        // after a voice call's terminator, the call hang time (361-1 5.2.1.4): its LC for the hang time terminators
        let mut lc: RdofFullLc = unsafe { core::mem::zeroed() };
        c.hang = *control == RDOF_SYNC_DATA | RDOF_DT_TERMINATOR_WITH_LC as u8
            && !c.duplex
            && rdof_decode_voice_lc(out, RDOF_DT_TERMINATOR_WITH_LC, &mut lc);
        if c.hang {
            c.hang_lc = lc.raw;
        }
        return OUT_TRAFFIC;
    } else if c.net_started && c.qn == 0 {
        c.net_started = c.net; // an underrun mid stream: it carries on as bursts come
    }
    if c.open && c.hang {
        // 361-1 5.2.1.4: in the call hang time the BS "shall maintain the channel in the Busy state by transmitting
        // Terminator with LC (hang time) messages ... with the source and destination IDs set to reflect the voice
        // call in progress"; until the next talker's voice, or the controller's P_CLEAR
        *control = RDOF_SYNC_DATA | RDOF_DT_TERMINATOR_WITH_LC as u8;
        unsafe { rdof_duplex_lc_burst(c.hang_lc.as_ptr(), RDOF_DT_TERMINATOR_WITH_LC, cc, out.as_mut_ptr()) };
        c.tx_traffic += 1;
        return OUT_HANG;
    }
    c.tx_idle += 1;
    unsafe { rdof_duplex_idle(cc, out.as_mut_ptr()) };
    OUT_IDLE
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3rf_ch_next(
    t: *mut T3rf,
    ch: c_int,
    k: i64,
    control: &mut u8,
    out: &mut Burst,
) -> c_int {
    ch_next(&mut *t, ch, k, control, out)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3rf_slot(
    t: *mut T3rf,
    c: c_int,
    k: i64,
    burst: &mut Burst,
    control: *mut u8,
    at: &mut c_int,
    slc28: &mut i32,
) -> bool {
    let t = &mut *t;
    let mut ctl = RDOF_SYNC_DATA | DT_IDLE as u8;
    *at = -1;
    *slc28 = -1;
    if let Some(cach) = t.io.cach {
        cach(t.io.ctx, c, k, at, slc28);
    }
    if k % 4 != 0 {
        *slc28 = -1;
    }
    if c == 0 && k & 1 == 0 && tscc_slot(t, 0) {
        let mut dt = -1;
        let mut pdu = [0u8; 12]; // 12 octets for a rate 1/2 block
        let got = match t.io.tscc {
            Some(tscc) => tscc(t.io.ctx, k, &mut dt, pdu.as_mut_ptr()),
            None => false,
        };
        if got && dt != DT_RATE34 && dt != DT_RATE1 && rdof_t3rf_block_burst(dt, pdu.as_ptr(), t.cc, FAMILY_BS, burst) {
            ctl = RDOF_SYNC_DATA | dt as u8;
            t.tscc_sent += 1;
        } else {
            rdof_duplex_idle(t.cc, burst.as_mut_ptr());
            t.tscc_idle += 1;
            // a slot the controller left free: the RC for the talker on the TSCC carrier's other slot
            let cc = t.cc;
            let c0 = &mut t.ch[0];
            if c0.rc != 0 && k - c0.rc_k >= RC_EVERY_K {
                rc_embed(burst, cc, (c0.rc & 0x0F) as u8);
                c0.rc = 0;
                c0.rc_k = k;
                c0.rc_sent += 1;
            }
        }
    } else if c >= 0 && c < t.ncarriers {
        ch_next(t, 2 * c + (k & 1) as c_int, k, &mut ctl, burst);
    } else {
        rdof_duplex_idle(t.cc, burst.as_mut_ptr());
    }
    if !control.is_null() {
        *control = ctl;
    }
    true
}

#[no_mangle]
pub extern "C" fn rdof_t3rf_carrier_busy(t: &T3rf, c: c_int) -> bool {
    if c < 0 || c >= t.ncarriers {
        return false;
    }
    (0..2).any(|s| {
        let ch = 2 * c + s;
        if t.tscc && ch == 0 {
            return true; // a TSCC's carrier is always on
        }
        let x = &t.ch[ch as usize];
        x.open || x.ctl_left != 0 || x.qn != 0
    })
}

// ── a payload carrier on a split TX half ───────────────────────────────────────

#[no_mangle]
pub unsafe extern "C" fn rdof_t3rf_split_init(o: *mut T3rfSplit, carrier: c_int) {
    o.write_bytes(0, 1);
    (*o).carrier = carrier;
    (*o).slc_sent = -1;
}

unsafe fn upstream(s: *mut c_void, cmd: u8, p: &[u8], now: f64) {
    let mut f = [0u8; MAX_FRAME];
    let len = rdof_mmdvm_frame(cmd, p.as_ptr(), p.len(), f.as_mut_ptr(), f.len());
    if len != 0 {
        rdof_split_upstream(s, f.as_ptr(), len, now);
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3rf_split_pump(
    t: *mut T3rf,
    o: *mut T3rfSplit,
    s: *mut c_void,
    k_now: i64,
    ahead: c_int,
    now: f64,
) {
    let o = &mut *o;
    let busy = rdof_t3rf_carrier_busy(&*t, o.carrier);
    if !rdof_split_ready(s) {
        o.keyed = false;
        return;
    }
    // a half that will be on the UTC grid keys only once it is; the TSCC's carrier, never idle, lets go of a key-up
    // held off the grid so the half can snap back (a payload carrier keeps its call, from the FIFO meanwhile)
    let tscc_carrier = (*t).tscc && o.carrier == 0;
    let pending = rdof_split_align_pending(s) && (!o.keyed || tscc_carrier);
    if !busy || pending {
        if o.keyed {
            upstream(s, DMR_START, &[0], now);
            o.keyed = false;
        }
        o.next_k = k_now + 1;
        return;
    }
    let sched = rdof_split_sched_ok(s);
    // a half that takes the CACH with each burst gets every slot's, the controller's AT and short LC in it (a TSCC's
    // carrier needs them slot by slot); idle slots go too, so none is left to the half's own CACH
    let cach = sched && rdof_split_has_cach(s);
    let ahead = if ahead > 0 {
        ahead
    } else if sched {
        4
    } else {
        1
    };
    let fresh = !o.keyed;
    if !o.keyed {
        upstream(s, DMR_START, &[1], now);
        o.keyed = true;
        o.keyups += 1;
    }
    // a fresh key-up starts at the next slot; keyed and behind, those slots are gone. Scheduled, a fresh key-up's
    // first two slots are inside the half's guard: taken from the controller (a payload channel's warm-up idles, a
    // TSCC's PDUs it sends again) but not sent
    o.next_k = o.next_k.max(k_now + 1);
    let first_sent = if sched && fresh { k_now + 3 } else { i64::MIN };
    o.scheduled = sched;
    while o.next_k <= k_now + ahead as i64 {
        let k = o.next_k;
        o.next_k += 1;
        let mut burst = [0u8; BURST_BYTES];
        let (mut control, mut at, mut slc) = (0u8, 0, 0i32);
        rdof_t3rf_slot(t, o.carrier, k, &mut burst, &mut control, &mut at, &mut slc);
        let e = k.rem_euclid(SPLIT_SENT as i64) as usize;
        o.sent[e] = T3rfSent { k, aired: false, burst };
        if slc >= 0 && slc != o.slc_sent {
            let lc5 = [(slc >> 24 & 0x0F) as u8, (slc >> 16) as u8, (slc >> 8) as u8, slc as u8, 0];
            let mut sl9 = [0u8; 9];
            rdof_short_lc(lc5.as_ptr(), sl9.as_mut_ptr());
            upstream(s, DMR_SHORTLC, &sl9, now);
            o.slc_sent = slc;
            o.short_lcs += 1;
        }
        if k < first_sent {
            continue;
        }
        if cach {
            let slc = if o.slc_sent >= 0 { o.slc_sent as u32 } else { 0 };
            let mut rows = [[0u8; 3]; 4];
            rdof_t3rf_cach4(slc, &[at.max(0); 4], k & !3, &mut rows);
            if rdof_split_sched_cach(s, k, control, burst.as_ptr(), rows[(k & 3) as usize].as_ptr()) {
                o.sched_bursts += 1;
                o.cach_bursts += 1;
                o.sent[e].aired = true;
            }
        } else if sched {
            let rc_here = (*t).ch[(2 * o.carrier + (k & 1) as c_int) as usize].rc_k == k && k != 0;
            if control & 0x0F == DT_IDLE as u8 && control & RDOF_SYNC_DATA != 0 && !rc_here {
                continue; // the half sends idle between scheduled bursts (an RC burst isn't idle)
            }
            if rdof_split_sched(s, k, control, burst.as_ptr()) {
                o.sched_bursts += 1;
                o.sent[e].aired = true;
            }
        } else {
            let mut p = [0u8; 1 + BURST_BYTES];
            p[0] = control;
            p[1..].copy_from_slice(&burst);
            upstream(s, if k & 1 != 0 { DMR_DATA2 } else { DMR_DATA1 }, &p, now);
            o.fifo_bursts += 1;
            o.sent[e].aired = true;
        }
    }
}

// ── layouts (rdof_t3rf_layout) ─────────────────────────────────────────────────

pub use crate::t3site::Layout;

macro_rules! layouts {
    ($(($name:literal, $t:ty)),* $(,)?) => {
        [$($crate::t3rf::Layout {
            name: concat!($name, "\0").as_ptr().cast(),
            size: core::mem::size_of::<$t>(),
            align: core::mem::align_of::<$t>(),
        },)*
         $crate::t3rf::Layout { name: core::ptr::null(), size: 0, align: 0 }]
    };
}
pub(crate) use layouts;

static LAYOUTS: [Layout; 6] = layouts![
    ("rdof_t3rf_burst_t", T3rfBurst),
    ("rdof_t3rf_ch_t", T3rfCh),
    ("rdof_t3rf_io_t", T3rfIo),
    ("rdof_t3rf_t", T3rf),
    ("rdof_t3rf_split_t", T3rfSplit),
];

#[no_mangle]
pub extern "C" fn rdof_t3rf_layout(i: usize) -> *const Layout {
    LAYOUTS.get(i).map_or(core::ptr::null(), |l| l as *const Layout)
}
