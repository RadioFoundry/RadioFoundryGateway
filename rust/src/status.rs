// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! The MQTT status and receiver messages a roamer or repeater publishes (rdof/status.h), and a float as the
//! shortest text that reads back the same. A port of common/src/status.c.

use core::ffi::{c_char, c_int};

use crate::ffi;
use crate::hblink::RdofLocation;
use crate::json::RdofJson;
use crate::text::{c_bytes, c_out, fixed, mhz, Out};

/// rdof_roamer_status_t
#[repr(C)]
pub struct RdofRoamerStatus {
    pub at: f64,
    pub freq: u32,
    pub cc: c_int,
    pub listening: bool,
    pub scanning: bool,
    pub radio: bool,
    pub hblink: bool,
    pub on_air: bool,
}

/// rdof_repeater_status_t
#[repr(C)]
pub struct RdofRepeaterStatus {
    pub at: f64,
    pub rx: u32,
    pub tx: u32,
    pub cc: c_int,
    pub radio: bool,
    pub hblink: bool,
    pub on_air: bool,
    pub slot_configured: [bool; 3],
    pub slot_busy: [bool; 3],
}

/// A float as the shortest text that reads back the same, ".0" when whole, d.ddde+XX outside
/// 1e-4 to 1e16.
fn repr(v: f64, o: &mut Out) {
    if v.is_nan() {
        return o.bytes(b"nan");
    }
    if v.is_infinite() {
        return o.bytes(if v > 0.0 { b"inf" } else { b"-inf" });
    }
    if v == 0.0 {
        return o.bytes(if v.is_sign_negative() { b"-0.0" } else { b"0.0" });
    }
    // the fewest significant digits that read back the same (17 always do)
    let mut e = [0u8; 40];
    let mut p = 1;
    loop {
        unsafe {
            ffi::snprintf(e.as_mut_ptr().cast(), e.len(), c"%.*e".as_ptr(), p - 1, v);
            if p == 17 || ffi::strtod(e.as_ptr().cast(), core::ptr::null_mut()) == v {
                break;
            }
        }
        p += 1;
    }
    let e = unsafe { c_bytes(e.as_ptr().cast()) };
    let at = e.iter().position(|&c| c == b'e').unwrap_or(e.len());
    let (mant, ex) = (&e[..at], &e[(at + 1).min(e.len())..]);
    let digits = ex.iter().filter(|c| c.is_ascii_digit()).fold(0, |n, &c| n * 10 + c_int::from(c - b'0'));
    let exp = if ex.first() == Some(&b'-') { -digits } else { digits };
    if (-4..16).contains(&exp) {
        let mut buf = [0u8; 64];
        let text = fixed(v, (p - 1 - exp).max(0), &mut buf);
        o.bytes(text);
        if !text.contains(&b'.') {
            o.bytes(b".0");
        }
    } else {
        o.bytes(mant);
        o.bytes(if exp < 0 { b"e-" } else { b"e+" });
        o.int(exp.abs().into(), 2);
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_format_repr(v: f64, out: *mut c_char, cap: usize) {
    c_out(out, cap, |o| repr(v, o));
}

fn location(j: &mut RdofJson, loc: Option<&RdofLocation>) {
    let Some(loc) = loc else { return };
    let mut v = [0u8; 40];
    for (k, x) in [(c"latitude", loc.latitude), (c"longitude", loc.longitude)] {
        let mut o = Out::new(&mut v);
        repr(x, &mut o);
        let n = o.n.min(v.len());
        j.raw(k, &v[..n]);
    }
    j.int(c"height", loc.height_m.into());
}

fn iso(t: f64) -> [c_char; 32] {
    let mut ts = [0; 32];
    unsafe { crate::tracker::rdof_iso_time(t, ts.as_mut_ptr()) };
    ts
}

#[no_mangle]
pub unsafe extern "C" fn rdof_roamer_status_json(s: &RdofRoamerStatus, out: *mut c_char, cap: usize) {
    let (at, mut f) = (iso(s.at), [0u8; 64]);
    let j = &mut RdofJson::new(out, cap);
    j.open(None);
    j.str(c"state", b"online");
    j.str(c"at", c_bytes(at.as_ptr()));
    j.str(c"frequency", mhz(s.freq, &mut f));
    j.int(c"colorCode", s.cc.into());
    j.bool(c"listening", s.listening);
    j.bool(c"scanning", s.scanning);
    j.bool(c"radio", s.radio);
    j.bool(c"hblink", s.hblink);
    j.bool(c"onAir", s.on_air);
    j.close();
}

#[no_mangle]
pub unsafe extern "C" fn rdof_roamer_receiver_json(
    name: *const c_char,
    idle_freq: u32,
    idle_cc: c_int,
    device: *const c_char,
    loc: Option<&RdofLocation>,
    out: *mut c_char,
    cap: usize,
) {
    let mut f = [0u8; 64];
    let j = &mut RdofJson::new(out, cap);
    j.open(None);
    j.open(Some(c"Receiver"));
    j.str(c"name", c_bytes(name));
    j.str(c"frequency", mhz(idle_freq, &mut f));
    j.int(c"colorCode", idle_cc.into());
    j.str(c"source", b"roaming");
    j.str(c"device", c_bytes(device));
    location(j, loc);
    j.close();
    j.close();
}

#[no_mangle]
pub unsafe extern "C" fn rdof_repeater_status_json(s: &RdofRepeaterStatus, out: *mut c_char, cap: usize) {
    let (at, mut f, mut t) = (iso(s.at), [0u8; 64], [0u8; 64]);
    let j = &mut RdofJson::new(out, cap);
    j.open(None);
    j.str(c"state", b"online");
    j.str(c"at", c_bytes(at.as_ptr()));
    j.str(c"frequency", mhz(s.rx, &mut f));
    j.str(c"txFrequency", mhz(s.tx, &mut t));
    j.int(c"colorCode", s.cc.into());
    j.bool(c"radio", s.radio);
    j.bool(c"hblink", s.hblink);
    j.bool(c"onAir", s.on_air);
    j.open(Some(c"slots"));
    for (n, k) in [(1, c"1"), (2, c"2")] {
        if s.slot_configured[n] {
            j.bool(k, s.slot_busy[n]);
        }
    }
    j.close();
    j.close();
}

#[no_mangle]
#[allow(clippy::too_many_arguments)] // the C's signature
pub unsafe extern "C" fn rdof_repeater_receiver_json(
    name: *const c_char,
    rx: u32,
    tx: u32,
    cc: c_int,
    device: *const c_char,
    loc: Option<&RdofLocation>,
    inbound_slot: c_int,
    out: *mut c_char,
    cap: usize,
) {
    let (mut f, mut t) = ([0u8; 64], [0u8; 64]);
    let j = &mut RdofJson::new(out, cap);
    j.open(None);
    j.open(Some(c"Receiver"));
    j.str(c"name", c_bytes(name));
    j.str(c"frequency", mhz(rx, &mut f));
    j.str(c"txFrequency", mhz(tx, &mut t));
    j.int(c"colorCode", cc.into());
    j.str(c"source", b"repeater");
    j.str(c"device", c_bytes(device));
    location(j, loc);
    if inbound_slot != 0 {
        j.open(Some(c"sfr"));
        j.int(c"inboundSlot", inbound_slot.into());
        j.int(c"outboundSlot", 3 - i64::from(inbound_slot));
        j.close();
    }
    j.close();
    j.close();
}
