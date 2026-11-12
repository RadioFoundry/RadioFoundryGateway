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
    rdof_check_crc16, rdof_crc16_ccitt, rdof_duplex_lc_burst, rdof_voice_regen_burst, rdof_voice_regen_init,
    RDOF_DT_CSBK, RDOF_DT_DATA_HEADER, RDOF_DT_MBC_CONTINUATION, RDOF_DT_MBC_HEADER, RDOF_DT_TERMINATOR_WITH_LC,
    RDOF_DT_VOICE, RDOF_DT_VOICE_LC_HEADER, RDOF_DT_VOICE_SYNC, RDOF_MASK_CSBK, RDOF_MASK_DATA_HEADER,
};
use crate::layer2::{
    rdof_data_burst, rdof_decode_slot_type, rdof_detect_sync, rdof_family_voice_sync, rdof_group_lc, RDOF_DATA_XOR,
};
use crate::mmdvm::{rdof_mmdvm_frame, RDOF_SYNC_DATA};
use crate::pdu::{rdof_decode_voice_lc, RdofFullLc};
use crate::repeater::RdofVoiceRegen;
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

// ── control blocks ─────────────────────────────────────────────────────────────

#[no_mangle]
pub extern "C" fn rdof_t3rf_mask(dt: c_int) -> c_int {
    match dt {
        RDOF_DT_CSBK => RDOF_MASK_CSBK as c_int,
        // an MBC's last block takes the header's mask
        RDOF_DT_MBC_HEADER | RDOF_DT_MBC_CONTINUATION => MASK_MBC_HEADER as c_int,
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

/// A block's burst: a control block (as rdof_t3rf_pdu_burst, octets 10 and 11 ignored) or a rate 1/2 data block
/// (12 octets as given: an appended block's CRC is over the whole message).
#[no_mangle]
pub extern "C" fn rdof_t3rf_block_burst(dt: c_int, blk: &[u8; 12], cc: c_int, family: u32, out: &mut Burst) -> bool {
    if dt == DT_RATE12 {
        rdof_data_burst(blk, dt, cc & 0x0F, rdof_family_voice_sync(family) ^ RDOF_DATA_XOR, out);
        return true;
    }
    let mut pdu = [0u8; 10];
    pdu.copy_from_slice(&blk[..10]);
    rdof_t3rf_pdu_burst(dt, &pdu, cc, family, out)
}

/// A burst read back as rdof_t3rf_burst_pdu, and a rate 1/2 data block too (PDU_OK when its BPTC decodes; no CRC
/// of its own). blk: the 12 octets (a control block's last two, its CRC).
#[no_mangle]
pub extern "C" fn rdof_t3rf_burst_block(burst: &Burst, dt: &mut c_int, cc: &mut c_int, blk: &mut [u8; 12]) -> c_int {
    let mut pdu = [0u8; 10];
    let r = rdof_t3rf_burst_pdu(burst, dt, cc, &mut pdu);
    if r == NOT_PDU && *dt == DT_RATE12 {
        return if unsafe { rdof_bptc196_decode(burst.as_ptr(), blk.as_mut_ptr()) } { PDU_OK } else { PDU_BAD };
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
    ch >= 0 && ch < 2 * t.ncarriers && !(t.tscc && ch == 0)
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
    if !c.open {
        flush(c);
        c.rf = false;
        c.rf_regen_on = false;
        c.ctl_left = 0;
        c.close_after = false;
        c.warmup = warmup.max(0);
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

fn lc_of(src: u32, dst: u32, group: bool) -> [u8; 9] {
    let mut lc9 = [0u8; 9];
    rdof_group_lc(src, dst, group, 0, &mut lc9);
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
        return; // data blocks: not carried (packet data is later work)
    }
    let mut late = false;
    if !c.rf_regen_on {
        // late entry: the channel's call
        rf_takes(c);
        c.rf_src = c.src;
        c.rf_dst = c.dst;
        c.rf_group = c.group;
        let lc9 = lc_of(c.src, c.dst, c.group);
        rdof_voice_regen_init(&mut c.rf_regen, lc9.as_ptr(), c.cc);
        c.rf_regen_on = true;
        c.rf = true;
        late = true;
    }
    c.rf_last = k;
    let control = if b.dt == RDOF_DT_VOICE_SYNC { SYNC_AUDIO } else { (b.n & 0x0F) as u8 };
    rdof_voice_regen_burst(&mut c.rf_regen, control, b.burst.as_ptr(), out.as_mut_ptr());
    push(c, control, false, &out);
    if late {
        activity(io, c, k, VOICE);
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
    if t.tscc && ch == 0 {
        let (mut dt, mut cc, mut pdu) = (0, 0, [0u8; 12]);
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
    payload_rx(&io, &mut t.ch[ch as usize], k, b, rssi);
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
            lc_of(f.src, f.dst, f.group)
        } else {
            lc_of(c.src, c.dst, c.group)
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
            let lc9 = if f.has_addr { lc_of(f.src, f.dst, f.group) } else { lc_of(c.src, c.dst, c.group) };
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
            c.rf = false;
            c.rf_regen_on = false;
            c.lost += 1;
            activity(&io, c, k, LOST);
        }
    }
}

fn ch_next(t: &mut T3rf, ch: c_int, control: &mut u8, out: &mut Burst) -> c_int {
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
        return OUT_TRAFFIC;
    } else if c.net_started && c.qn == 0 {
        c.net_started = c.net; // an underrun mid stream: it carries on as bursts come
    }
    c.tx_idle += 1;
    unsafe { rdof_duplex_idle(cc, out.as_mut_ptr()) };
    OUT_IDLE
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3rf_ch_next(
    t: *mut T3rf,
    ch: c_int,
    _k: i64,
    control: &mut u8,
    out: &mut Burst,
) -> c_int {
    ch_next(&mut *t, ch, control, out)
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
    if t.tscc && c == 0 && k & 1 == 0 {
        let mut dt = -1;
        let mut pdu = [0u8; 12]; // 12 octets for a rate 1/2 block
        let got = match t.io.tscc {
            Some(tscc) => tscc(t.io.ctx, k, &mut dt, pdu.as_mut_ptr()),
            None => false,
        };
        if got && rdof_t3rf_block_burst(dt, &pdu, t.cc, FAMILY_BS, burst) {
            ctl = RDOF_SYNC_DATA | dt as u8;
            t.tscc_sent += 1;
        } else {
            rdof_duplex_idle(t.cc, burst.as_mut_ptr());
            t.tscc_idle += 1;
        }
    } else if c >= 0 && c < t.ncarriers {
        ch_next(t, 2 * c + (k & 1) as c_int, &mut ctl, burst);
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
    if !busy {
        if o.keyed {
            upstream(s, DMR_START, &[0], now);
            o.keyed = false;
        }
        o.next_k = k_now + 1;
        return;
    }
    let sched = rdof_split_sched_ok(s);
    let ahead = if ahead > 0 {
        ahead
    } else if sched {
        4
    } else {
        1
    };
    if !o.keyed {
        upstream(s, DMR_START, &[1], now);
        o.keyed = true;
        o.keyups += 1;
    }
    // a fresh key-up starts at the next slot; keyed and behind, those slots are gone
    o.next_k = o.next_k.max(k_now + 1);
    o.scheduled = sched;
    while o.next_k <= k_now + ahead as i64 {
        let k = o.next_k;
        o.next_k += 1;
        let mut burst = [0u8; BURST_BYTES];
        let (mut control, mut at, mut slc) = (0u8, 0, 0i32);
        rdof_t3rf_slot(t, o.carrier, k, &mut burst, &mut control, &mut at, &mut slc);
        if slc >= 0 && slc != o.slc_sent {
            let lc5 = [(slc >> 24 & 0x0F) as u8, (slc >> 16) as u8, (slc >> 8) as u8, slc as u8, 0];
            let mut sl9 = [0u8; 9];
            rdof_short_lc(lc5.as_ptr(), sl9.as_mut_ptr());
            upstream(s, DMR_SHORTLC, &sl9, now);
            o.slc_sent = slc;
            o.short_lcs += 1;
        }
        if sched {
            if control & 0x0F == DT_IDLE as u8 && control & RDOF_SYNC_DATA != 0 {
                continue; // the half sends idle between scheduled bursts
            }
            if rdof_split_sched(s, k, control, burst.as_ptr()) {
                o.sched_bursts += 1;
            }
        } else {
            let mut p = [0u8; 1 + BURST_BYTES];
            p[0] = control;
            p[1..].copy_from_slice(&burst);
            upstream(s, if k & 1 != 0 { DMR_DATA2 } else { DMR_DATA1 }, &p, now);
            o.fifo_bursts += 1;
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
