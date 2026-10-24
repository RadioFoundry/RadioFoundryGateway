// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! One burst for .../raw (rdof/rawburst.h): the binary form, version 1 or 2, and MMDVMHost's MQTTRaw JSON. A port
//! of common/src/rawburst.c; the JSON goes through the C's writer (rdof/json.h).

use core::ffi::{c_char, c_int, c_longlong, CStr};

use crate::ffi::{self, RDOF_BURST_BYTES};
use crate::json::{
    rdof_base64, rdof_json_bool, rdof_json_close, rdof_json_done, rdof_json_init, rdof_json_int, rdof_json_open, rdof_json_str, RdofJson,
};

const RDOF_RAWBURST_VERSION: u8 = 1;
const RDOF_RAWBURST_BYTES: usize = 49;
const RDOF_RAWBURST_VERSION_UTC: u8 = 2;
const RDOF_RAWBURST_BYTES_UTC: usize = 62;
const RDOF_RAWBURST_MAX: usize = RDOF_RAWBURST_BYTES_UTC;

const RDOF_RAWBURST_SLOT2: u8 = 0x01;
const RDOF_RAWBURST_NETWORK: u8 = 0x02;
const RDOF_RAWBURST_ADDRESSED: u8 = 0x04;
const RDOF_RAWBURST_GROUP: u8 = 0x08;

/// rdof_rawburst_t, field for field.
#[repr(C)]
pub struct RdofRawburst {
    pub ms: i64,
    pub slot: c_int,
    pub network: bool,
    pub dt: c_int,
    pub n: c_int,
    pub addressed: bool,
    pub src: u32,
    pub dst: u32,
    pub group: bool,
    pub burst: [u8; RDOF_BURST_BYTES],
    pub utc: bool,
    pub utc_us: i64,
    pub utc_slot: i64,
}

fn type_name(dt: c_int) -> Option<&'static CStr> {
    Some(match dt {
        ffi::RDOF_DT_PI_HEADER => c"voice_pi_header",
        ffi::RDOF_DT_VOICE_LC_HEADER => c"voice_lc_header",
        ffi::RDOF_DT_TERMINATOR_WITH_LC => c"terminator",
        ffi::RDOF_DT_CSBK => c"csbk",
        ffi::RDOF_DT_MBC_HEADER => c"mbc_header",
        ffi::RDOF_DT_MBC_CONTINUATION => c"mbc_continuation",
        ffi::RDOF_DT_DATA_HEADER => c"data_header",
        ffi::RDOF_DT_RATE_12 => c"rate_1_2",
        ffi::RDOF_DT_RATE_34 => c"rate_3_4",
        ffi::RDOF_DT_RATE_1 => c"rate_1",
        ffi::RDOF_DT_USBD => c"usbd",
        ffi::RDOF_DT_VOICE_SYNC => c"voice_sync",
        ffi::RDOF_DT_VOICE => c"voice",
        _ => return None,
    })
}

/// v in decimal, NUL terminated, as "%d" (12 bytes at most: "-2147483648").
fn decimal(v: c_int, out: &mut [u8; 12]) {
    let (mut digits, mut n, mut u) = ([0u8; 10], 0, v.unsigned_abs());
    loop {
        digits[n] = b'0' + (u % 10) as u8;
        n += 1;
        u /= 10;
        if u == 0 {
            break;
        }
    }
    let mut i = 0;
    if v < 0 {
        out[0] = b'-';
        i = 1;
    }
    for &d in digits[..n].iter().rev() {
        out[i] = d;
        i += 1;
    }
    out[i] = 0;
}

#[no_mangle]
pub extern "C" fn rdof_raw_type_name(dt: c_int, tmp: &mut [u8; 12]) -> *const c_char {
    match type_name(dt) {
        Some(name) => name.as_ptr(),
        None => {
            decimal(dt, tmp);
            tmp.as_ptr().cast()
        }
    }
}

/// v's low n bytes, little endian.
fn le(out: &mut [u8], v: u64, n: usize) {
    out[..n].copy_from_slice(&v.to_le_bytes()[..n]);
}

#[no_mangle]
pub extern "C" fn rdof_rawburst_encode(r: &RdofRawburst, out: &mut [u8; RDOF_RAWBURST_MAX]) -> usize {
    out[0] = if r.utc { RDOF_RAWBURST_VERSION_UTC } else { RDOF_RAWBURST_VERSION };
    let mut flags = if r.slot == 2 { RDOF_RAWBURST_SLOT2 } else { 0 };
    if r.network {
        flags |= RDOF_RAWBURST_NETWORK;
    }
    if r.addressed {
        flags |= RDOF_RAWBURST_ADDRESSED | if r.group { RDOF_RAWBURST_GROUP } else { 0 };
    }
    out[1] = flags;
    out[2] = r.dt as u8;
    out[3] = r.n as u8;
    le(&mut out[4..], r.ms.max(0) as u64, 6);
    le(&mut out[10..], if r.addressed { r.src.into() } else { 0 }, 3);
    le(&mut out[13..], if r.addressed { r.dst.into() } else { 0 }, 3);
    out[16..RDOF_RAWBURST_BYTES].copy_from_slice(&r.burst);
    if !r.utc {
        return RDOF_RAWBURST_BYTES;
    }
    le(&mut out[49..], r.utc_us as u64, 8);
    le(&mut out[57..], r.utc_slot as u64, 5);
    RDOF_RAWBURST_BYTES_UTC
}

#[no_mangle]
pub unsafe extern "C" fn rdof_rawburst_json(r: &RdofRawburst, out: *mut c_char, cap: usize) -> bool {
    let (mut ts, mut tmp, mut b64) = ([0 as c_char; 32], [0u8; 12], [0 as c_char; 48]);
    let j = &mut RdofJson { buf: out, cap, len: 0, overflow: false, depth: 0, first: [false; 8] };
    crate::tracker::rdof_iso_ms(r.ms, ts.as_mut_ptr());
    rdof_base64(r.burst.as_ptr(), RDOF_BURST_BYTES, b64.as_mut_ptr());
    rdof_json_init(j, out, cap);
    rdof_json_open(j, core::ptr::null());
    rdof_json_open(j, c"DMR".as_ptr());
    let str = |j: &mut RdofJson, key: &CStr, v: *const c_char| unsafe { rdof_json_str(j, key.as_ptr(), v) };
    let int = |j: &mut RdofJson, key: &CStr, v: c_longlong| unsafe { rdof_json_int(j, key.as_ptr(), v) };
    str(j, c"timestamp", ts.as_ptr());
    int(j, c"slot", r.slot.into());
    str(j, c"source", if r.network { c"network" } else { c"rf" }.as_ptr());
    str(j, c"type", rdof_raw_type_name(r.dt, &mut tmp));
    int(j, c"dt", r.dt.into());
    int(j, c"n", r.n.into());
    str(j, c"data", b64.as_ptr());
    if r.addressed {
        int(j, c"src_id", r.src.into());
        int(j, c"dst_id", r.dst.into());
        rdof_json_bool(j, c"group".as_ptr(), r.group);
    }
    if r.utc {
        int(j, c"utc_slot", r.utc_slot);
        int(j, c"utc_us", r.utc_us);
    }
    rdof_json_close(j);
    rdof_json_close(j);
    rdof_json_done(j)
}
