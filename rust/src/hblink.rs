// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! HBlink4 packets and the HBlink sink (rdof/hblink.h): a port of common/src/hblink.c. DMRC config, DMRD bursts up and down, DMRT tune and DMRK replies.

use core::ffi::{c_char, c_int, c_void};

use crate::bits::slice;
use crate::ffi::RDOF_BURST_BYTES;
use crate::pdu::{rdof_decode_csbk, RdofCsbk};
use crate::text::{c_bytes, fixed, Out};
use crate::tracker::{RdofFrame, RdofSink};

type Burst = [u8; RDOF_BURST_BYTES];

pub const RDOF_HB_HOLD_FRAMES: usize = 12;
pub const RDOF_DMRD_LEN: usize = 55;
pub const RDOF_DMRC_MAX: usize = 160;
pub const RDOF_DMRK_LEN: usize = 18;

const RDOF_DT_VOICE_LC_HEADER: c_int = 1;
const RDOF_DT_TERMINATOR_WITH_LC: c_int = 2;
const RDOF_DT_CSBK: c_int = 3;
const RDOF_DT_VOICE_SYNC: c_int = 0xF0;
const RDOF_DT_VOICE: c_int = 0xF1;
const RDOF_CSBK_PREAMBLE: c_int = 0x3D;

#[repr(C)]
pub struct RdofLocation {
    pub latitude: f64,
    pub longitude: f64,
    pub height_m: c_int,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct RdofNetBurst {
    pub stream_id: [u8; 4],
    pub control: u8,
    pub burst: Burst,
    pub terminator: bool,
    pub seq: u8,
    pub src: u32,
    pub dst: u32,
    pub group: bool,
    pub slot: c_int,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct RdofTune {
    pub stream_id: [u8; 4],
    pub freq: u32,
    pub cc: c_int,
    pub power: c_int,
    pub preempt: bool,
}

#[repr(C)]
pub struct RdofHbSink {
    pub peer_id: u32,
    pub send: Option<unsafe extern "C" fn(*mut c_void, *const u8, usize)>,
    pub before_stream: Option<unsafe extern "C" fn(*mut c_void)>,
    pub rand32: Option<unsafe extern "C" fn(*mut c_void) -> u32>,
    pub ctx: *mut c_void,
    pub seq: [c_int; 2],
    pub stream: [u32; 2],
    pub held: [[RdofFrame; RDOF_HB_HOLD_FRAMES]; 2],
    pub nheld: [c_int; 2],
}

/// %.4f cut to `width` and space padded: RPTC's fields.
fn degrees(o: &mut Out, v: f64, width: usize) {
    let mut s = [0u8; 64];
    o.text(fixed(v, 4, &mut s), width);
}

/// Location.dmrc_tail: 8 + 9 + 3 ASCII.
fn location_tail(o: &mut Out, loc: &RdofLocation) {
    degrees(o, loc.latitude, 8);
    degrees(o, loc.longitude, 9);
    o.int(i64::from(loc.height_m), 3);
}

#[no_mangle]
pub unsafe extern "C" fn rdof_dmrc_packet(
    peer_id: u32,
    callsign: *const c_char,
    rx_hz: u32,
    tx_hz: u32,
    cc: c_int,
    slots: c_char,
    power: c_int,
    version: *const c_char,
    software: *const c_char,
    location: Option<&RdofLocation>,
    idle_freq: Option<&u32>,
    idle_cc: Option<&c_int>,
    out: &mut [u8; RDOF_DMRC_MAX],
) -> usize {
    let mut body = [0u8; RDOF_DMRC_MAX + 32];
    let mut o = Out::new(&mut body);
    o.text(c_bytes(callsign), 8);
    o.int(i64::from(rx_hz), 9);
    o.int(i64::from(tx_hz), 9);
    o.int(i64::from(power.min(99) as u32), 2); // the C's (unsigned)
    o.int(i64::from(cc as u32), 2);
    o.byte(slots as u8);
    o.text(c_bytes(version), 40);
    o.text(c_bytes(software), 40);
    if let Some(loc) = location {
        location_tail(&mut o, loc);
    }
    if let (Some(&freq), Some(&icc)) = (idle_freq, idle_cc) {
        if location.is_none() {
            location_tail(&mut o, &RdofLocation { latitude: 0.0, longitude: 0.0, height_m: 0 });
        }
        o.int(i64::from(freq), 9);
        o.int(i64::from(icc as u32), 2);
    }
    let n = o.n.min(RDOF_DMRC_MAX - 8);
    out[..4].copy_from_slice(b"DMRC");
    out[4..8].copy_from_slice(&peer_id.to_be_bytes());
    out[8..8 + n].copy_from_slice(&body[..n]);
    n + 8
}

#[no_mangle]
pub extern "C" fn rdof_dmrd_packet(
    seq: c_int,
    src: u32,
    dst: u32,
    peer_id: u32,
    slot: c_int,
    group: bool,
    dt: c_int,
    n: c_int,
    stream_id: u32,
    burst: &Burst,
    ber: c_int,
    rssi: c_int,
    out: &mut [u8; RDOF_DMRD_LEN],
) {
    let mut flags: u8 = if slot == 2 { 0x80 } else { 0 };
    if !group {
        flags |= 0x40;
    }
    flags |= match dt {
        RDOF_DT_VOICE_SYNC => 0x10,
        RDOF_DT_VOICE => (n & 0x0F) as u8,
        _ => 0x20 | (dt & 0x0F) as u8,
    };
    out[..4].copy_from_slice(b"DMRD");
    out[4] = seq as u8;
    out[5..8].copy_from_slice(&src.to_be_bytes()[1..]);
    out[8..11].copy_from_slice(&dst.to_be_bytes()[1..]);
    out[11..15].copy_from_slice(&peer_id.to_be_bytes());
    out[15] = flags;
    out[16..20].copy_from_slice(&stream_id.to_be_bytes());
    out[20..53].copy_from_slice(burst);
    out[53] = ber.min(255) as u8; // the C's (uint8_t): a negative ber wraps
    out[54] = rssi.unsigned_abs().min(255) as u8;
}

#[no_mangle]
pub extern "C" fn rdof_single_csbk(burst: &Burst) -> bool {
    let mut c = RdofCsbk::default();
    rdof_decode_csbk(burst, &mut c);
    c.crc_ok && c.opcode != RDOF_CSBK_PREAMBLE
}

#[no_mangle]
pub unsafe extern "C" fn rdof_parse_dmrd(data: *const u8, len: usize, out: &mut RdofNetBurst) -> bool {
    let d = slice(data, len);
    if d.len() < 53 || &d[..4] != b"DMRD" {
        return false;
    }
    let flags = d[15];
    let (ft, dtv) = ((flags >> 4) & 0x03, flags & 0x0F);
    let mut burst = [0u8; RDOF_BURST_BYTES];
    burst.copy_from_slice(&d[20..53]);
    let dtv_i = c_int::from(dtv);
    *out = RdofNetBurst {
        stream_id: [d[16], d[17], d[18], d[19]],
        control: match ft {
            1 => 0x20,
            2 => 0x40 | dtv,
            _ => dtv,
        },
        burst,
        terminator: ft == 2 && (dtv_i == RDOF_DT_TERMINATOR_WITH_LC || (dtv_i == RDOF_DT_CSBK && rdof_single_csbk(&burst))),
        seq: d[4],
        src: u32::from_be_bytes([0, d[5], d[6], d[7]]),
        dst: u32::from_be_bytes([0, d[8], d[9], d[10]]),
        group: flags & 0x40 == 0,
        slot: if flags & 0x80 != 0 { 2 } else { 1 },
    };
    true
}

#[no_mangle]
pub unsafe extern "C" fn rdof_parse_dmrt(data: *const u8, len: usize, out: &mut RdofTune) -> bool {
    let d = slice(data, len);
    if d.len() < 18 || &d[..4] != b"DMRT" {
        return false;
    }
    *out = RdofTune {
        stream_id: [d[8], d[9], d[10], d[11]],
        freq: u32::from_be_bytes([d[12], d[13], d[14], d[15]]),
        cc: c_int::from(d[16] & 0x0F),
        power: c_int::from(d[17]),
        preempt: d.get(18).is_some_and(|b| b & 1 != 0),
    };
    true
}

#[no_mangle]
pub extern "C" fn rdof_dmrk_packet(
    peer_id: u32,
    stream_id: &[u8; 4],
    status: c_int,
    freq: u32,
    cc: c_int,
    out: &mut [u8; RDOF_DMRK_LEN],
) {
    out[..4].copy_from_slice(b"DMRK");
    out[4..8].copy_from_slice(&peer_id.to_be_bytes());
    out[8..12].copy_from_slice(stream_id);
    out[12] = status as u8;
    out[13..17].copy_from_slice(&freq.to_be_bytes());
    out[17] = (cc & 0x0F) as u8;
}

#[no_mangle]
pub unsafe extern "C" fn rdof_hb_kind(data: *const u8, len: usize) -> c_char {
    let d = slice(data, len);
    let kind = match d.get(..4) {
        Some(b"DMRP") => b'P',
        Some(b"MSTN" | b"MSTC") => b'N',
        Some(b"DMRT") => b'T',
        Some(b"DMRD") => b'D',
        _ => 0,
    };
    kind as c_char
}

// The HBlink sink, one peer. Through raw pointers: send() and before_stream() get the owner's ctx, which may hold the
// sink, so no &mut to it lives across a callback.

#[no_mangle]
pub unsafe extern "C" fn rdof_hb_sink_init(s: *mut RdofHbSink, peer_id: u32) {
    core::ptr::write_bytes(s, 0, 1);
    (*s).peer_id = peer_id;
}

unsafe fn new_stream_id(s: *mut RdofHbSink) -> u32 {
    let r = match (*s).rand32 {
        Some(f) => f((*s).ctx),
        None => 0x12345678,
    };
    r % 0xFFFF_FFFE + 1 // randint(1, 0xFFFFFFFE)
}

unsafe fn before_stream(s: *mut RdofHbSink) {
    if let Some(f) = (*s).before_stream {
        f((*s).ctx);
    }
}

unsafe fn send_frame(s: *mut RdofHbSink, f: &RdofFrame) {
    let mut pkt = [0u8; RDOF_DMRD_LEN];
    let i = (f.slot - 1) as usize;
    rdof_dmrd_packet(
        (*s).seq[i],
        f.src,
        f.dst,
        (*s).peer_id,
        f.slot,
        f.group,
        f.dt,
        f.n,
        (*s).stream[i],
        &f.burst,
        f.errors,
        f.rssi,
        &mut pkt,
    );
    (*s).seq[i] = ((*s).seq[i] + 1) & 0xFF;
    if let Some(send) = (*s).send {
        send((*s).ctx, pkt.as_ptr(), pkt.len());
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_hb_sink_on_frame(s: *mut RdofHbSink, f: &RdofFrame) {
    if !(1..=2).contains(&f.slot) {
        return;
    }
    let i = (f.slot - 1) as usize;
    if !f.has_addr {
        // Voice before we know who's talking: hold it for the late-entry header.
        if f.dt == RDOF_DT_VOICE_SYNC || f.dt == RDOF_DT_VOICE {
            let held = &mut (*s).held[i];
            let nheld = &mut (*s).nheld[i];
            if *nheld as usize >= RDOF_HB_HOLD_FRAMES {
                held.copy_within(1.., 0);
                *nheld = RDOF_HB_HOLD_FRAMES as c_int - 1;
            }
            held[*nheld as usize] = *f;
            *nheld += 1;
        }
        return;
    }
    if f.new_stream {
        before_stream(s);
        (*s).stream[i] = new_stream_id(s);
        (*s).seq[i] = 0;
        let nheld = (*s).nheld[i] as usize;
        (*s).nheld[i] = 0;
        send_frame(s, f);
        if f.dt == RDOF_DT_VOICE_LC_HEADER && f.synthetic {
            for k in 0..nheld.min(RDOF_HB_HOLD_FRAMES) {
                let h = RdofFrame {
                    has_addr: true,
                    src: f.src,
                    dst: f.dst,
                    group: f.group,
                    new_stream: false,
                    synthetic: false,
                    ..(*s).held[i][k]
                };
                send_frame(s, &h);
            }
        }
        return;
    }
    if (*s).stream[i] == 0 {
        before_stream(s);
        (*s).stream[i] = new_stream_id(s);
    }
    send_frame(s, f);
}

unsafe extern "C" fn sink_frame(ctx: *mut c_void, _channel: *const c_char, f: *const RdofFrame) {
    rdof_hb_sink_on_frame(ctx.cast(), &*f);
}

#[no_mangle]
pub extern "C" fn rdof_hb_sink_as_sink(s: *mut RdofHbSink) -> RdofSink {
    RdofSink { on_json: None, on_raw: None, on_frame: Some(sink_frame), ctx: s.cast(), on_call: None, on_raw_burst: None }
}
