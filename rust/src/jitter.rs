// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! The jitter buffer for network voice going out over the air (rdof/jitter.h). A port of common/src/jitter.c.
//! Sizes are in bursts (60 ms each). Nothing here blocks; the caller holds its lock.

use core::ffi::{c_char, c_int, CStr};

use crate::bits::{bits_to_u64, set_field};
use crate::ffi::{self, RDOF_BURST_BYTES};
use crate::json::RdofJson;
use crate::layer2::{rdof_emb_bits, RDOF_BS_VOICE, RDOF_MS_VOICE};

type Burst = [u8; RDOF_BURST_BYTES];

pub const RDOF_JB_BURST_MS: c_int = 60;
pub const RDOF_JB_WINDOW: usize = 64; // arrivals the estimate looks back over (3.84 s)
pub const RDOF_JB_KEYS: usize = 8; // talkgroups and slots remembered
const RDOF_JB_LEAD: c_int = 2; // bursts the radio needs in hand besides the jitter
const RDOF_JB_MARGIN_MS: f64 = 10.0; // added to the lateness before rounding up to bursts
const RDOF_JB_UNKNOWN: c_int = 6; // the prebuffer before anything is known about the link (360 ms)
const RDOF_JB_SAMPLES: c_int = 16; // arrivals before a stream's own estimate sets its target
const RDOF_JB_REMEMBER: c_int = 6; // arrivals a stream needs to be remembered
const RDOF_JB_SHRINK_S: f64 = 2.0; // ahead of target this long: one voice burst dropped
pub const RDOF_JB_MAX_MS: f64 = 3000.0; // the most a setting may ask for
pub const RDOF_JB_MIN_MS: f64 = 180.0; // the defaults for auto
pub const RDOF_JB_MAX_DEFAULT_MS: f64 = 480.0;

const SYNC_AUDIO: u8 = 0x20;
/// The on-air AMBE+2 silence frame (mmdvm's).
const SILENCE: [u8; 9] = [0xb9, 0xe8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2a, 0x6b];

/// rdof_jb_key_t: one talkgroup and slot's remembered need.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RdofJbKey {
    pub used: u32, // 0: free; else when last used (RdofJb.stamp)
    pub dst: u32,
    pub slot: c_int,
    pub group: bool,
    pub late_ms: f64,
}

/// rdof_jb_t: a transceiver's or repeater's jitter buffer: its settings, its memory and what it reports.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RdofJb {
    pub adaptive: bool,
    pub min: c_int,
    pub max: c_int,
    pub keys: [RdofJbKey; RDOF_JB_KEYS],
    pub stamp: u32,
    pub has_link: bool,
    pub link_ms: f64,
    pub target: c_int,
    pub jitter_ms: f64,
    pub late_ms: f64,
    pub streams: u32,
    pub underruns: u32,
    pub grown: u32,
    pub shrunk: u32,
}

/// rdof_jb_stream_t: one stream's measure.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RdofJbStream {
    pub keyed: bool,
    pub dst: u32,
    pub slot: c_int,
    pub group: bool,
    pub measuring: bool,
    pub t0: f64,
    pub idx: c_int,
    pub seq: c_int,
    pub last_transit: f64,
    pub transit: [i16; RDOF_JB_WINDOW],
    pub nwin: c_int,
    pub wnext: c_int,
    pub n: c_int,
    pub jitter_ms: f64,
    pub late_ms: f64,
    pub peak_ms: f64,
    pub start: c_int,
    pub target: c_int,
    pub shift: c_int,
    pub above: bool,
    pub above_since: f64,
}

fn ceil(x: f64) -> f64 {
    unsafe { ffi::ceil(x) }
}

fn lrint(x: f64) -> i64 {
    unsafe { ffi::rint(x) as i64 }
}

/// Milliseconds to whole bursts, rounded up (at most RDOF_JB_MAX_MS).
#[no_mangle]
pub extern "C" fn rdof_jb_bursts(ms: f64) -> c_int {
    if ms.is_nan() || ms <= 0.0 {
        return 0;
    }
    let ms = if ms > RDOF_JB_MAX_MS { RDOF_JB_MAX_MS } else { ms };
    ceil(ms / f64::from(RDOF_JB_BURST_MS) - 1e-9) as c_int
}

#[no_mangle]
pub extern "C" fn rdof_jb_init(jb: &mut RdofJb) {
    *jb = unsafe { core::mem::zeroed() };
    jb.min = rdof_jb_bursts(RDOF_JB_MIN_MS);
    jb.max = rdof_jb_bursts(RDOF_JB_MAX_DEFAULT_MS);
}

fn clamp(jb: &RdofJb, n: c_int) -> c_int {
    let lo = if jb.min > 0 { jb.min } else { 1 };
    let hi = if jb.max > lo { jb.max } else { lo };
    n.clamp(lo, hi)
}

/// The prebuffer for a lateness: the radio's lead and the lateness, rounded up to bursts.
fn target_for(jb: &RdofJb, late_ms: f64) -> c_int {
    clamp(jb, RDOF_JB_LEAD + rdof_jb_bursts(late_ms + RDOF_JB_MARGIN_MS))
}

#[no_mangle]
pub extern "C" fn rdof_jb_begin(jb: &mut RdofJb, s: &mut RdofJbStream) {
    *s = unsafe { core::mem::zeroed() };
    s.start = if jb.has_link { target_for(jb, jb.link_ms) } else { clamp(jb, RDOF_JB_UNKNOWN) };
    s.target = s.start;
    jb.streams = jb.streams.wrapping_add(1);
    jb.target = s.target;
    jb.jitter_ms = 0.0;
    jb.late_ms = 0.0;
}

fn find(jb: &RdofJb, dst: u32, group: bool, slot: c_int) -> Option<usize> {
    jb.keys.iter().position(|k| k.used != 0 && k.dst == dst && k.group == group && k.slot == slot)
}

#[no_mangle]
pub extern "C" fn rdof_jb_key(jb: &mut RdofJb, s: &mut RdofJbStream, dst: u32, group: bool, slot: c_int, started: bool) {
    if s.keyed {
        return;
    }
    s.keyed = true;
    s.dst = dst;
    s.group = group;
    s.slot = slot;
    let Some(i) = find(jb, dst, group, slot) else {
        return;
    };
    jb.stamp = jb.stamp.wrapping_add(1);
    jb.keys[i].used = jb.stamp;
    if !started && s.n < RDOF_JB_SAMPLES {
        s.target = target_for(jb, jb.keys[i].late_ms);
        s.start = s.target;
        jb.target = s.target;
    }
}

#[no_mangle]
pub extern "C" fn rdof_jb_arrival(jb: &mut RdofJb, s: &mut RdofJbStream, at: f64, seq: c_int) {
    let seq = seq & 0xFF;
    let idx;
    if !s.measuring {
        s.measuring = true;
        s.t0 = at;
        s.idx = 0;
        idx = 0;
        s.seq = seq;
    } else {
        let mut d = (seq - s.seq) & 0xFF;
        if d == 0 {
            d = 1; // no sequence numbers: one burst on
        }
        if d < 128 {
            s.idx += d; // more than one: lost bursts
            s.seq = seq;
            idx = s.idx;
        } else {
            idx = s.idx - (256 - d); // out of order: an earlier one, late
        }
    }
    let transit = (at - s.t0) * 1000.0 - f64::from(idx) * f64::from(RDOF_JB_BURST_MS);
    let transit = transit.clamp(-30000.0, 30000.0);
    if s.n != 0 {
        // RFC 3550 6.4.1
        let d = transit - s.last_transit;
        s.jitter_ms += ((if d < 0.0 { -d } else { d }) - s.jitter_ms) / 16.0;
    }
    s.last_transit = transit;
    s.n += 1;
    s.transit[s.wnext as usize] = lrint(transit) as i16;
    s.wnext = (s.wnext + 1) % RDOF_JB_WINDOW as c_int;
    if s.nwin < RDOF_JB_WINDOW as c_int {
        s.nwin += 1;
    }
    // Lateness against the window's earliest: the worst but one, so a lone straggler doesn't set the size.
    let (mut lo, mut hi1, mut hi2) = (c_int::from(s.transit[0]), c_int::from(i16::MIN), c_int::from(i16::MIN));
    for &t in &s.transit[..s.nwin as usize] {
        let v = c_int::from(t);
        lo = lo.min(v);
        if v > hi1 {
            hi2 = hi1;
            hi1 = v;
        } else if v > hi2 {
            hi2 = v;
        }
    }
    s.late_ms = if s.nwin >= 2 { f64::from(hi2 - lo) } else { 0.0 };
    if s.late_ms > s.peak_ms {
        s.peak_ms = s.late_ms;
    }
    if jb.adaptive && s.n >= RDOF_JB_SAMPLES {
        s.target = target_for(jb, s.late_ms);
    }
    jb.target = s.target;
    jb.jitter_ms = s.jitter_ms;
    jb.late_ms = s.late_ms;
}

#[no_mangle]
pub extern "C" fn rdof_jb_prebuffer(jb: &RdofJb, s: &RdofJbStream, fixed: c_int) -> c_int {
    if jb.adaptive {
        s.start
    } else {
        fixed
    }
}

#[no_mangle]
pub extern "C" fn rdof_jb_underrun(jb: &mut RdofJb, s: &mut RdofJbStream) -> bool {
    jb.underruns = jb.underruns.wrapping_add(1);
    if !jb.adaptive || s.start + s.shift >= jb.max {
        return false;
    }
    s.shift += 1;
    jb.grown = jb.grown.wrapping_add(1);
    true
}

#[no_mangle]
pub extern "C" fn rdof_jb_fill(jb: &mut RdofJb, s: &mut RdofJbStream, gap: c_int, ahead: f64) -> c_int {
    if !jb.adaptive || gap <= 0 {
        return gap;
    }
    let want = f64::from(s.target - 1) - ahead;
    let keep = if want > 0.0 { ceil(want - 1e-9) as c_int } else { 0 };
    let keep = keep.min(gap);
    s.shift -= gap - keep;
    jb.shrunk = jb.shrunk.wrapping_add((gap - keep) as u32);
    keep
}

#[no_mangle]
pub extern "C" fn rdof_jb_drop(jb: &mut RdofJb, s: &mut RdofJbStream, ahead: f64, silent: bool, now: f64) -> bool {
    if !jb.adaptive {
        return false;
    }
    if ahead - f64::from(s.target - 1) < 1.0 - 1e-9 {
        s.above = false;
        return false;
    }
    if !s.above {
        s.above = true;
        s.above_since = now;
    }
    // Voice only to give back what the stream grew; silence whenever it's ahead.
    if !silent && !(s.shift > 0 && now - s.above_since >= RDOF_JB_SHRINK_S - 1e-9) {
        return false;
    }
    s.shift -= 1;
    jb.shrunk = jb.shrunk.wrapping_add(1);
    s.above_since = now;
    true
}

/// Up at once, down by half a call.
fn remember(late: &mut f64, need: f64) {
    *late = if need >= *late { need } else { (*late + need) / 2.0 };
}

#[no_mangle]
pub extern "C" fn rdof_jb_end(jb: &mut RdofJb, s: &mut RdofJbStream) {
    if s.n < RDOF_JB_REMEMBER {
        s.n = 0;
        return;
    }
    let need = s.peak_ms;
    if jb.has_link {
        remember(&mut jb.link_ms, need);
    } else {
        jb.link_ms = need;
    }
    jb.has_link = true;
    if s.keyed {
        let i = match find(jb, s.dst, s.group, s.slot) {
            Some(i) => {
                remember(&mut jb.keys[i].late_ms, need);
                i
            }
            None => {
                // a free one, else the one used longest ago
                let mut i = 0;
                for j in 1..RDOF_JB_KEYS {
                    if jb.keys[j].used < jb.keys[i].used {
                        i = j;
                    }
                }
                let k = &mut jb.keys[i];
                k.dst = s.dst;
                k.group = s.group;
                k.slot = s.slot;
                k.late_ms = need;
                i
            }
        };
        jb.stamp = jb.stamp.wrapping_add(1);
        jb.keys[i].used = jb.stamp;
    }
    s.n = 0; // once
}

#[no_mangle]
pub unsafe extern "C" fn rdof_jb_reframe(
    input: *const Burst,
    n: c_int,
    cc: c_int,
    bs: bool,
    pi: c_int,
    embedded32: *const u8,
    out: *mut Burst,
) -> u8 {
    const LCSS_OF: [c_int; 6] = [0, 1, 3, 3, 2, 0];
    core::ptr::copy(input, out, 1);
    let out = &mut *out;
    if n <= 0 || n > 5 {
        set_field(out, 108, 48, if bs { RDOF_BS_VOICE } else { RDOF_MS_VOICE });
        return SYNC_AUDIO;
    }
    let mut e = [0u8; 16];
    rdof_emb_bits(cc, LCSS_OF[n as usize], pi, &mut e);
    set_field(out, 108, 8, bits_to_u64(&e[..8]));
    set_field(out, 148, 8, bits_to_u64(&e[8..]));
    for i in 0..32 {
        let bit = if embedded32.is_null() { 0 } else { *embedded32.add(i) & 1 };
        set_field(out, 116 + i, 1, u64::from(bit));
    }
    n as u8
}

#[no_mangle]
pub unsafe extern "C" fn rdof_jb_silent(burst: *const Burst) -> bool {
    let mut f = [[0u8; 9]; 3];
    ffi::rdof_ambe_frames(burst.cast(), f.as_mut_ptr().cast());
    f.iter().all(|x| *x == SILENCE)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_jb_json(jb: &RdofJb, fixed: c_int, j: &mut RdofJson, key: *const c_char) {
    j.open(if key.is_null() { None } else { Some(CStr::from_ptr(key)) });
    j.str(c"mode", if jb.adaptive { b"auto" } else { b"fixed" });
    let target = if !jb.adaptive {
        fixed
    } else if jb.streams != 0 {
        jb.target
    } else {
        clamp(jb, RDOF_JB_UNKNOWN)
    };
    j.int(c"target_ms", i64::from(target) * i64::from(RDOF_JB_BURST_MS));
    j.int(c"jitter_ms", lrint(jb.jitter_ms));
    j.int(c"late_ms", lrint(jb.late_ms));
    if jb.adaptive {
        j.int(c"min_ms", i64::from(clamp(jb, 0)) * i64::from(RDOF_JB_BURST_MS));
        j.int(c"max_ms", i64::from(clamp(jb, 1 << 20)) * i64::from(RDOF_JB_BURST_MS));
    }
    j.int(c"streams", i64::from(jb.streams));
    j.int(c"underruns", i64::from(jb.underruns));
    j.int(c"grown", i64::from(jb.grown));
    j.int(c"shrunk", i64::from(jb.shrunk));
    j.close();
}
