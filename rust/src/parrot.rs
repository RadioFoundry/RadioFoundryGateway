// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! The parrot: a transmission heard on the air, sent back over the air once it ends (rdof/parrot.h). A port of
//! common/src/parrot.c.
//!
//! The hooks (admit, ready, start, send) may look at the parrot (a test's admit reads its hz), so, as in txvr.rs, the
//! exported functions work through the raw pointer and make a `&mut` of the whole struct only where no hook is called.
//! The voice regeneration (duplex.c), the CRC-32 and the data reply's bursts (ipgw.c) are C, through ffi.rs.

use core::ffi::{c_char, c_int, c_long, c_void, CStr};

use crate::ffi::{
    self, RDOF_BURST_BYTES, RDOF_DT_CSBK, RDOF_DT_DATA_HEADER, RDOF_DT_MBC_HEADER, RDOF_DT_RATE_1, RDOF_DT_RATE_12,
    RDOF_DT_RATE_34, RDOF_DT_TERMINATOR_WITH_LC, RDOF_DT_VOICE_LC_HEADER,
};
use crate::hblink::RdofNetBurst;
use crate::layer2::{rdof_decode_slot_type, rdof_lc_burst, RDOF_BS_VOICE, RDOF_DATA_XOR, RDOF_MS_VOICE};
use crate::mmdvm::{rdof_csbk_preambles, RDOF_SYNC_DATA};
use crate::pdu::{
    rdof_decode_data_block, rdof_decode_data_header, rdof_split_confirmed, RdofDataBlock, RdofDataHeader,
};
use crate::repeater::RdofVoiceRegen;

type Burst = [u8; RDOF_BURST_BYTES];

pub const RDOF_PARROT_BURSTS: usize = 1000;
const RDOF_PARROT_DELAY_S: f64 = 2.5;
const RDOF_PARROT_GAP_S: f64 = 0.5;
const RDOF_PARROT_AHEAD: c_int = 3;
const RDOF_PARROT_STREAM: u32 = 0x50410000;
const RDOF_PARROT_WAIT_S: f64 = 10.0;
const FRAME_S: f64 = 0.06; // one slot's bursts come a 60 ms frame apart

// rdof_parrot_state_t
const IDLE: u32 = 0;
const RECORDING: u32 = 1;
const WAITING: u32 = 2;
const REPLAYING: u32 = 3;

const RDOF_DT_IDLE: c_int = 9;
const RDOF_FLCO_GROUP: u8 = 0x00;
const RDOF_CSBK_PREAMBLE: u8 = 0x3D;
const RDOF_DPF_UNCONFIRMED: c_int = 2;
const RDOF_DPF_CONFIRMED: c_int = 3;
const RDOF_SAP_IP: c_int = 4;
const RDOF_SAP_UDP_COMPRESSED: c_int = 3;

pub const RDOF_IPGW_MAX_PACKET: usize = 1520;
pub const RDOF_IPGW_MAX_BURSTS: usize = 128;

/// rdof_ipgw_dmr_t (rdof/ipgw.h)
#[repr(C)]
pub struct RdofIpgwDmr {
    pub dst: u32,
    pub src: u32,
    pub slot: c_int,
    pub freq: u32,
    pub cc: c_int,
    pub response: bool,
    pub nbursts: c_int,
    pub control: [u8; RDOF_IPGW_MAX_BURSTS],
    pub bursts: [Burst; RDOF_IPGW_MAX_BURSTS],
}

/// rdof_parrot_burst_t
#[repr(C)]
pub struct RdofParrotBurst {
    pub control: u8,
    pub burst: Burst,
}

/// rdof_parrot_t
#[repr(C)]
pub struct RdofParrot {
    pub id: u32,
    pub delay_s: f64,
    pub start: Option<
        unsafe extern "C" fn(ctx: *mut c_void, stream_id: *const u8, slot: c_int, src: u32, dst: u32, group: bool),
    >,
    pub send: Option<unsafe extern "C" fn(ctx: *mut c_void, b: *const RdofNetBurst)>,
    pub admit: Option<unsafe extern "C" fn(ctx: *mut c_void, slot: c_int, src: u32, dst: u32, group: bool) -> bool>,
    pub ready: Option<unsafe extern "C" fn(ctx: *mut c_void) -> bool>,
    pub ctx: *mut c_void,
    pub state: u32,
    pub hz: u32,
    pub slot: c_int,
    pub src: u32,
    pub dst: u32,
    pub group: bool,
    pub voice: bool,
    pub last: f64,
    pub due: f64,
    pub n: c_int,
    pub sent: c_int,
    pub cc: c_int,
    pub regen: RdofVoiceRegen,
    pub corrected: c_long,
    pub streams: u32,
    pub replayed: u32,
    pub cut: u32,
    pub ignored: u32,
    pub gave_up: u32,
    pub bursts: [RdofParrotBurst; RDOF_PARROT_BURSTS],
    pub data_replies: u32,
    pub data: [u8; RDOF_IPGW_MAX_PACKET + 32],
    pub reply: RdofIpgwDmr,
}

#[no_mangle]
pub unsafe extern "C" fn rdof_parrot_init(p: *mut RdofParrot, id: u32) {
    core::ptr::write_bytes(p, 0, 1);
    (*p).id = id;
    (*p).delay_s = RDOF_PARROT_DELAY_S;
}

#[no_mangle]
pub unsafe extern "C" fn rdof_parrot_state_name(p: *const RdofParrot) -> *const c_char {
    const NAMES: [&CStr; 4] = [c"idle", c"recording", c"waiting", c"replaying"];
    NAMES[(*p).state as usize].as_ptr()
}

/// A burst that starts a transmission, and whom it's from and to: a voice LC header, a CSBK (a data preamble among
/// them), an MBC header or a data header. (voice, src, dst, group)
fn starts(control: u8, burst: &Burst) -> Option<(bool, u32, u32, bool)> {
    if control & RDOF_SYNC_DATA == 0 {
        return None;
    }
    let dt = c_int::from(control & 0x0F);
    if dt != RDOF_DT_VOICE_LC_HEADER && dt != RDOF_DT_CSBK && dt != RDOF_DT_MBC_HEADER && dt != RDOF_DT_DATA_HEADER {
        return None;
    }
    let mut b = [0u8; 12];
    if !unsafe { ffi::rdof_bptc196_decode(burst.as_ptr(), b.as_mut_ptr()) } {
        return None;
    }
    let be24 = |k: usize| u32::from_be_bytes([0, b[k], b[k + 1], b[k + 2]]);
    Some(if dt == RDOF_DT_VOICE_LC_HEADER {
        // full LC: FLCO, FID, service options, dst, src
        (true, be24(6), be24(3), (b[0] & 0x3F) == RDOF_FLCO_GROUP)
    } else if dt == RDOF_DT_DATA_HEADER {
        // G/I, ..., dst, src
        (false, be24(5), be24(2), b[0] & 0x80 != 0)
    } else {
        // a CSBK or MBC header: opcode, FID, two bytes, dst, src (the preamble's group bit in the third)
        (false, be24(7), be24(4), (b[0] & 0x3F) == RDOF_CSBK_PREAMBLE && b[2] & 0x40 != 0)
    })
}

fn swap8(d: &mut [u8], a: usize, b: usize, n: usize) {
    for i in 0..n {
        d.swap(a + i, b + i);
    }
}

/// Private packet data, whole and its CRC good: the recording replaced by the reply (the header comment says how).
/// Anything else stays as it was heard.
unsafe fn data_reply(p: &mut RdofParrot) {
    let Some(h) = (0..p.n as usize).find(|&i| {
        let c = p.bursts[i].control;
        c & RDOF_SYNC_DATA != 0 && c_int::from(c & 0x0F) == RDOF_DT_DATA_HEADER
    }) else {
        return;
    };
    let mut hd: RdofDataHeader = core::mem::zeroed();
    rdof_decode_data_header(&p.bursts[h].burst, &mut hd);
    if !hd.crc_ok || hd.group || (hd.dpf != RDOF_DPF_UNCONFIRMED && hd.dpf != RDOF_DPF_CONFIRMED) || hd.blocks <= 0 {
        return;
    }
    let (mut len, mut got) = (0usize, 0);
    let mut i = h + 1;
    while i < p.n as usize && got < hd.blocks {
        let control = p.bursts[i].control;
        let dt = c_int::from(control & 0x0F);
        i += 1;
        if control & RDOF_SYNC_DATA == 0 {
            return;
        }
        if dt == RDOF_DT_DATA_HEADER {
            // a second header (proprietary): one of the blocks
            got += 1;
            continue;
        }
        if dt != RDOF_DT_RATE_12 && dt != RDOF_DT_RATE_34 && dt != RDOF_DT_RATE_1 {
            return;
        }
        let mut b: RdofDataBlock = core::mem::zeroed();
        if !rdof_decode_data_block(&p.bursts[i - 1].burst, dt, &mut b) || !b.fec_ok {
            return;
        }
        let mut body = [0u8; 24];
        let mut n = b.len;
        let src: &[u8] = if hd.dpf == RDOF_DPF_CONFIRMED {
            let mut dbsn = 0;
            if !rdof_split_confirmed(&b, &mut dbsn, body.as_mut_ptr(), &mut n) {
                return;
            }
            &body
        } else {
            &b.payload
        };
        let n = n as usize;
        if len + n > p.data.len() {
            return;
        }
        p.data[len..len + n].copy_from_slice(&src[..n]);
        len += n;
        got += 1;
    }
    let user = len as c_int - 4 - hd.pad_octets;
    if got < hd.blocks || user < 0 || !ffi::rdof_check_crc32(p.data.as_ptr(), len) {
        return;
    }
    let d = &mut p.data;
    if hd.sap == RDOF_SAP_IP && user >= 20 && d[0] >> 4 == 4 {
        let ihl = usize::from(d[0] & 0x0F) * 4;
        swap8(d, 12, 16, 4);
        if d[9] == 17 && user >= ihl as c_int + 8 {
            swap8(d, ihl, ihl + 2, 2);
        }
    } else if hd.sap == RDOF_SAP_UDP_COMPRESSED && user >= 5 {
        if (d[2] & 0x0F) == 2 {
            return; // to a talkgroup's address
        }
        d[2] = d[2].rotate_right(4); // SAID and DAID
        let (s3, s4) = (d[3], d[4]); // SPID and DPID, the opcode bits stay
        d[3] = (s3 & 0x80) | (s4 & 0x7F);
        d[4] = (s4 & 0x80) | (s3 & 0x7F);
        if s3 & 0x7F == 0 && s4 & 0x7F == 0 && user >= 9 {
            swap8(d, 5, 7, 2); // both ports there; with one, it stays in its place
        }
    }
    let r = &mut p.reply;
    if !ffi::rdof_ipgw_build_dmr(
        hd.dst_id,
        hd.src_id,
        hd.sap,
        p.data.as_ptr(),
        user as usize,
        p.cc,
        RDOF_BS_VOICE ^ RDOF_DATA_XOR,
        r,
    ) {
        return;
    }
    let room = RDOF_PARROT_BURSTS as c_int - r.nbursts;
    let pre = if (h as c_int) < room { h as c_int } else { room };
    // the preambles are built in data: the message is in the reply now
    const FIT: c_int = ((RDOF_IPGW_MAX_PACKET + 32) / RDOF_BURST_BYTES) as c_int;
    let npre = if pre * RDOF_BURST_BYTES as c_int <= (RDOF_IPGW_MAX_PACKET + 32) as c_int { pre } else { FIT };
    let pb = p.data.as_mut_ptr() as *mut Burst;
    if npre != 0 {
        rdof_csbk_preambles(hd.dst_id, hd.src_id, false, npre, p.cc, pb);
    }
    for i in 0..npre as usize {
        p.bursts[i].control = RDOF_SYNC_DATA | RDOF_DT_CSBK as u8;
        p.bursts[i].burst = *pb.add(i);
    }
    for i in 0..r.nbursts as usize {
        p.bursts[npre as usize + i].control = r.control[i];
        p.bursts[npre as usize + i].burst = r.bursts[i];
    }
    p.n = npre + r.nbursts;
    p.src = hd.dst_id;
    p.dst = hd.src_id;
    p.data_replies = p.data_replies.wrapping_add(1);
}

unsafe fn end(p: &mut RdofParrot, due: f64) {
    if p.n == 0 {
        p.state = IDLE;
        return;
    }
    if !p.voice {
        data_reply(p);
    }
    p.state = WAITING;
    p.due = due;
}

/// A burst as it's kept: regenerated (the header comment says how), into p.bursts[k].
unsafe fn clean(p: &mut RdofParrot, control: u8, burst: &Burst, k: usize) {
    let mut out = p.bursts[k].burst;
    if control & RDOF_SYNC_DATA == 0 {
        if p.voice {
            let before = p.regen.errors;
            ffi::rdof_voice_regen_burst(&mut p.regen, control, burst.as_ptr(), out.as_mut_ptr());
            p.corrected = p.corrected.wrapping_add(p.regen.errors.wrapping_sub(before));
        } else {
            out = *burst;
        }
    } else {
        let dt = c_int::from(control & 0x0F);
        if p.voice && (dt == RDOF_DT_VOICE_LC_HEADER || dt == RDOF_DT_TERMINATOR_WITH_LC) {
            rdof_lc_burst(&p.regen.lc9, dt, p.cc, RDOF_MS_VOICE ^ RDOF_DATA_XOR, &mut out);
        } else if !ffi::rdof_duplex_data(control, burst.as_ptr(), p.cc, out.as_mut_ptr()) {
            out = *burst; // a CSBK or data header whose CRC fails: kept as heard
        }
    }
    p.bursts[k].burst = out;
}

#[no_mangle]
pub unsafe extern "C" fn rdof_parrot_heard(p: *mut RdofParrot, slot: c_int, control: u8, burst: &Burst, now: f64) {
    rdof_parrot_heard_at(p, 0, slot, control, burst, now);
}

#[no_mangle]
pub unsafe extern "C" fn rdof_parrot_heard_at(
    p: *mut RdofParrot,
    hz: u32,
    slot: c_int,
    control: u8,
    burst: &Burst,
    now: f64,
) {
    let state = (*p).state;
    if state == WAITING || state == REPLAYING {
        return;
    }
    if state == RECORDING && hz != (*p).hz {
        return; // another frequency's
    }
    let data = control & RDOF_SYNC_DATA != 0;
    let dt = c_int::from(control & 0x0F);
    if data && dt == RDOF_DT_IDLE {
        return;
    }
    if state == IDLE {
        let Some((voice, src, dst, group)) = starts(control, burst) else {
            return; // late entry, or the rest of a transmission not kept
        };
        (*p).hz = hz;
        // admit may look at the parrot: no &mut of it across the call
        let refused = match (*p).admit {
            _ if (*p).id != 0 && dst != (*p).id => true,
            Some(admit) => !admit((*p).ctx, slot, src, dst, group),
            None => false,
        };
        let p = &mut *p;
        if refused {
            p.ignored = p.ignored.wrapping_add(1);
            return;
        }
        p.state = RECORDING;
        p.slot = slot;
        p.voice = voice;
        p.src = src;
        p.dst = dst;
        p.group = group;
        p.n = 0;
        p.corrected = 0;
        let (mut cc, mut dt, mut errors) = (0, 0, 0);
        p.cc = if rdof_decode_slot_type(burst, Some(&mut cc), Some(&mut dt), Some(&mut errors)) { cc } else { 1 };
        if voice {
            let mut lc = [0u8; 12];
            ffi::rdof_bptc196_decode(burst.as_ptr(), lc.as_mut_ptr());
            if !group {
                // a private call comes back from whom it called to the caller, as a reply would
                let t: [u8; 3] = lc[3..6].try_into().unwrap();
                lc.copy_within(6..9, 3);
                lc[6..9].copy_from_slice(&t);
                p.src = dst;
                p.dst = src;
            }
            ffi::rdof_voice_regen_init(&mut p.regen, lc.as_ptr(), p.cc);
            p.regen.swap = !group; // the embedded LC too, or the radio drops the call at the first superframe
        }
    } else if slot != (*p).slot {
        return;
    } else if !(*p).voice && data && dt == RDOF_DT_VOICE_LC_HEADER && starts(control, burst).is_some() {
        // data or a CSBK, then a call right after it: the call is its own transmission
        let p = &mut *p;
        end(p, now + p.delay_s);
        return;
    }
    let p = &mut *p;
    let terminator = data && dt == RDOF_DT_TERMINATOR_WITH_LC;
    const LAST: c_int = RDOF_PARROT_BURSTS as c_int;
    if p.n < LAST - 1 || (terminator && p.n < LAST) {
        let k = p.n as usize;
        p.bursts[k].control = control;
        clean(p, control, burst, k);
        p.n += 1;
    } else {
        p.cut = p.cut.wrapping_add(1); // full: the last place is kept for the terminator
    }
    p.last = now;
    if p.voice && terminator {
        end(p, now + p.delay_s);
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_parrot_lost(p: *mut RdofParrot, slot: c_int, now: f64) {
    rdof_parrot_lost_at(p, 0, slot, now);
}

#[no_mangle]
pub unsafe extern "C" fn rdof_parrot_lost_at(p: *mut RdofParrot, hz: u32, slot: c_int, now: f64) {
    let p = &mut *p;
    if p.state == RECORDING && slot == p.slot && hz == p.hz {
        end(p, now + p.delay_s);
    }
}

fn stream_id(p: *const RdofParrot) -> [u8; 4] {
    (RDOF_PARROT_STREAM | (unsafe { (*p).streams } & 0xFFFF)).to_be_bytes()
}

unsafe fn send(p: *mut RdofParrot, i: c_int) {
    let mut b: RdofNetBurst = core::mem::zeroed();
    b.stream_id = stream_id(p);
    {
        let p = &*p;
        let k = i as usize;
        b.control = p.bursts[k].control;
        b.burst = p.bursts[k].burst;
        b.terminator = i == p.n - 1; // the end even when the call was cut or lost
        b.seq = i as u8;
        b.src = p.src;
        b.dst = p.dst;
        b.group = p.group;
        b.slot = p.slot;
    }
    if let Some(send) = (*p).send {
        send((*p).ctx, &b);
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_parrot_poll(p: *mut RdofParrot, now: f64) {
    if (*p).state == RECORDING && now - (*p).last >= RDOF_PARROT_GAP_S {
        let p = &mut *p;
        let due = p.last + p.delay_s;
        end(p, if due > now { due } else { now });
    }
    if (*p).state == WAITING && now >= (*p).due {
        if let Some(ready) = (*p).ready {
            if !ready((*p).ctx) {
                if now - (*p).due >= RDOF_PARROT_WAIT_S {
                    // no transmitter came free
                    (*p).gave_up = (*p).gave_up.wrapping_add(1);
                    (*p).state = IDLE;
                }
                return;
            }
        }
    }
    if (*p).state == WAITING && now >= (*p).due {
        if (*p).ready.is_some() {
            (*p).due = now; // the replay's frames from here, after any wait
        }
        (*p).streams = (*p).streams.wrapping_add(1);
        (*p).sent = 0;
        (*p).state = REPLAYING;
        if let Some(start) = (*p).start {
            let sid = stream_id(p);
            start((*p).ctx, sid.as_ptr(), (*p).slot, (*p).src, (*p).dst, (*p).group);
        }
    }
    if (*p).state == REPLAYING {
        let upto = ((now - (*p).due) / FRAME_S) as c_int + 1 + RDOF_PARROT_AHEAD;
        while (*p).sent < (*p).n && (*p).sent < upto {
            let i = (*p).sent;
            (*p).sent += 1;
            send(p, i);
        }
        if (*p).sent == (*p).n {
            (*p).replayed = (*p).replayed.wrapping_add(1);
            (*p).state = IDLE;
        }
    }
}
