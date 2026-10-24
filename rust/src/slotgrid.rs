// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! The UTC slot grid (rdof/slotgrid.h): a port of common/src/slotgrid.c, operation for
//! operation (vectors.h checks both). The grid, the slot_timing
//! setting, the clock gate and a stream's sample clock. The int64 arithmetic wraps, as the C's; floats in the
//! C's order, with the C library's floor and %.3f.

use core::ffi::{c_char, c_int, CStr};

use crate::bits::slice_mut;
use crate::ffi::floor;
use crate::jsonp::{rdof_jget, rdof_jnode, JDoc, JNode, RDOF_JNULL, RDOF_JNUM, RDOF_JOBJ, RDOF_JSTR};
use crate::text::Piece::{I, S};
use crate::text::{c_bytes, fixed, print, Piece};

const RDOF_SLOT_NS: i64 = 30_000_000;
const RDOF_CACH_NS: i64 = 2_500_000;
const RDOF_IN_START_NS: i64 = 1_250_000;
const RDOF_SLOT_MAX_OFFSET_US: c_int = 15_000;

const RDOF_LOSS_HOLDOVER: u32 = 0;
const RDOF_LOSS_FREE: u32 = 1;
const RDOF_LOSS_STOP: u32 = 2;

const RDOF_GATE_FREE: u32 = 0;
const RDOF_GATE_ALIGNED: u32 = 1;
const RDOF_GATE_HOLDOVER: u32 = 2;
const RDOF_GATE_STOPPED: u32 = 3;

/// rdof_slot_timing_t
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RdofSlotTiming {
    pub utc: bool,
    pub offset_us: c_int,
    pub max_err_pps_us: c_int,
    pub max_err_ntp_us: c_int,
    pub on_loss: u32,
    pub holdover_s: f64,
}

/// rdof_slot_gate_t
#[repr(C)]
pub struct RdofSlotGate {
    pub timing: RdofSlotTiming,
    pub state: u32,
    pub has_last_ok: bool,
    pub last_ok: f64,
    pub source: [u8; 8],
    pub err_us: f64,
    pub why: [u8; 64],
}

/// rdof_sample_clock_t
#[repr(C)]
pub struct RdofSampleClock {
    pub nominal: f64,
    pub ppm: f64,
    pub window_s: f64,
    pub stamped: bool,
    pub has_anchor: bool,
    pub i0: i64,
    pub t0: i64,
    pub has_err: bool,
    pub err_ns: i64,
    pub has_best: bool,
    pub best_r: i64,
    pub best_i: i64,
    pub best_t: i64,
    pub win_start: i64,
    pub moved: i64,
    pub windows: u32,
}

// ── the grid ──────────────────────────────────────────────────────────────────

fn off_ns(offset_us: c_int) -> i64 {
    i64::from(offset_us) * 1000
}

fn ts_of(n: i64, even: c_int, odd: c_int) -> c_int {
    if n & 1 == 0 {
        even
    } else {
        odd
    }
}

#[no_mangle]
pub extern "C" fn rdof_grid_boundary(n: i64, offset_us: c_int) -> i64 {
    n.wrapping_mul(RDOF_SLOT_NS).wrapping_add(off_ns(offset_us))
}

#[no_mangle]
pub extern "C" fn rdof_grid_slot_at(t_ns: i64, offset_us: c_int, ts: &mut c_int, phase_ns: &mut i64) -> i64 {
    let x = t_ns.wrapping_sub(off_ns(offset_us));
    let n = x.div_euclid(RDOF_SLOT_NS);
    *ts = ts_of(n, 1, 2);
    *phase_ns = x.wrapping_sub(n.wrapping_mul(RDOF_SLOT_NS));
    n
}

#[no_mangle]
pub extern "C" fn rdof_grid_next_boundary(t_ns: i64, offset_us: c_int, b: &mut i64) -> i64 {
    let x = t_ns.wrapping_sub(off_ns(offset_us));
    // -(-x // SLOT_NS): -x overflows at i64::MIN
    let n = x / RDOF_SLOT_NS + i64::from(x % RDOF_SLOT_NS > 0);
    *b = rdof_grid_boundary(n, offset_us);
    n
}

/// The nearest slot to a burst that started `x` after the grid's place for slot 0's: n, and how far off.
fn nearest(x: i64) -> (i64, i64) {
    let n = x.wrapping_add(RDOF_SLOT_NS / 2).div_euclid(RDOF_SLOT_NS);
    (n, x.wrapping_sub(n.wrapping_mul(RDOF_SLOT_NS)))
}

#[no_mangle]
pub extern "C" fn rdof_grid_inbound(start_ns: i64, offset_us: c_int, ts: &mut c_int, err_ns: &mut i64) -> i64 {
    let (n, err) = nearest(start_ns.wrapping_sub(RDOF_IN_START_NS).wrapping_sub(off_ns(offset_us)));
    *ts = ts_of(n, 2, 1);
    *err_ns = err;
    n
}

#[no_mangle]
pub extern "C" fn rdof_grid_outbound(start_ns: i64, offset_us: c_int, ts: &mut c_int, err_ns: &mut i64) -> i64 {
    let (n, err) = nearest(start_ns.wrapping_sub(RDOF_CACH_NS).wrapping_sub(off_ns(offset_us)));
    *ts = ts_of(n, 1, 2);
    *err_ns = err;
    n
}

// ── the setting ───────────────────────────────────────────────────────────────

const DEFAULT_TIMING: RdofSlotTiming = RdofSlotTiming {
    utc: false,
    offset_us: 0,
    max_err_pps_us: 20,
    max_err_ntp_us: 2000,
    on_loss: RDOF_LOSS_HOLDOVER,
    holdover_s: 60.0,
};

#[no_mangle]
pub extern "C" fn rdof_slot_timing_default(t: &mut RdofSlotTiming) {
    *t = DEFAULT_TIMING;
}

const KEYS: [&[u8]; 5] = [b"mode", b"offset_us", b"max_err_us", b"on_loss", b"holdover_s"];

/// Node i, None for one the parse didn't make.
fn node(doc: &JDoc, i: c_int) -> Option<&JNode> {
    unsafe { rdof_jnode(doc, i).as_ref() }
}

/// An object's children.
fn children<'a>(doc: &'a JDoc, n: &JNode) -> impl Iterator<Item = &'a JNode> {
    let mut c = n.child;
    core::iter::from_fn(move || {
        let n = node(doc, c).filter(|_| c >= 0)?;
        c = n.next;
        Some(n)
    })
}

fn key(n: &JNode) -> &[u8] {
    unsafe { c_bytes(n.key) }
}

fn string(n: &JNode) -> Option<&[u8]> {
    (n.type_ == RDOF_JSTR).then(|| unsafe { c_bytes(n.str_) })
}

/// The node for `k` in a slot_timing value (a string is {"mode": it}); -1 none.
fn key_of(doc: &JDoc, v: c_int, k: &CStr) -> c_int {
    let Some(n) = node(doc, v).filter(|_| v >= 0) else {
        return -1;
    };
    match n.type_ {
        RDOF_JSTR if k == c"mode" => v,
        RDOF_JOBJ => unsafe { rdof_jget(doc, v, k.as_ptr()) },
        _ => -1,
    }
}

/// The error text, as the C's snprintf(error, cap, ...); false, to return.
struct Error {
    out: *mut c_char,
    cap: usize,
}

impl Error {
    fn set(&self, pieces: &[Piece]) -> bool {
        print(unsafe { slice_mut(self.out.cast(), self.cap) }, pieces);
        false
    }
}

fn int_us(n: &JNode, what: &[u8], out: &mut c_int, e: &Error) -> bool {
    let v = n.num;
    if n.type_ != RDOF_JNUM || v.is_nan() || v != unsafe { floor(v) } || v > 2e9 || v < -2e9 {
        return e.set(&[S(b"slot_timing."), S(what), S(b": whole microseconds")]);
    }
    *out = v as c_int;
    true
}

fn checked(doc: &JDoc, v: c_int, e: &Error) -> bool {
    let Some(n) = node(doc, v).filter(|_| v >= 0) else {
        return true;
    };
    if n.type_ == RDOF_JNULL || n.type_ == RDOF_JSTR {
        return true;
    }
    if n.type_ != RDOF_JOBJ {
        return e.set(&[S(b"slot_timing: free, utc, or {mode, offset_us, max_err_us, on_loss, holdover_s}")]);
    }
    if let Some(c) = children(doc, n).find(|c| !KEYS.contains(&key(c))) {
        return e.set(&[S(b"slot_timing: unknown key '"), S(key(c)), S(b"'")]);
    }
    true
}

/// base or own: -1 when null or not a node.
fn given(doc: &JDoc, v: c_int) -> c_int {
    match node(doc, v) {
        Some(n) if v >= 0 && n.type_ != RDOF_JNULL => v,
        _ => -1,
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_slot_timing_parse(
    doc: &JDoc,
    base: c_int,
    own: c_int,
    t: &mut RdofSlotTiming,
    error: *mut c_char,
    cap: usize,
) -> bool {
    *t = DEFAULT_TIMING;
    let e = Error { out: error, cap };
    let (base, own) = (given(doc, base), given(doc, own));
    if !checked(doc, base, &e) || !checked(doc, own, &e) {
        return false;
    }
    // each key: the radio's, else the box's ({**box, **own}); (index, node)
    let pick = |k: &CStr| {
        let i = key_of(doc, own, k);
        let i = if i >= 0 { i } else { key_of(doc, base, k) };
        node(doc, i).filter(|_| i >= 0).map(|n| (i, n))
    };
    if let Some((_, n)) = pick(c"mode") {
        match string(n) {
            Some(b"free") => t.utc = false,
            Some(b"utc") => t.utc = true,
            _ => return e.set(&[S(b"slot_timing.mode: free or utc")]),
        }
    }
    const MAX: Piece = I(RDOF_SLOT_MAX_OFFSET_US as i64);
    if let Some((_, n)) = pick(c"offset_us") {
        if !int_us(n, b"offset_us", &mut t.offset_us, &e) {
            return false;
        }
        if t.offset_us > RDOF_SLOT_MAX_OFFSET_US || t.offset_us < -RDOF_SLOT_MAX_OFFSET_US {
            let v = I(t.offset_us.into());
            return e.set(&[S(b"slot_timing.offset_us "), v, S(b": within +-"), MAX, S(b" (half a slot)")]);
        }
    }
    if let Some((i, n)) = pick(c"max_err_us") {
        if n.type_ == RDOF_JOBJ {
            if let Some(c) = children(doc, n).find(|c| key(c) != b"pps" && key(c) != b"ntp") {
                return e.set(&[S(b"slot_timing.max_err_us: unknown source '"), S(key(c)), S(b"' (pps, ntp)")]);
            }
            // a source given twice: the last one
            for (k, what, out) in [
                (c"pps", &b"max_err_us.pps"[..], &mut t.max_err_pps_us),
                (c"ntp", &b"max_err_us.ntp"[..], &mut t.max_err_ntp_us),
            ] {
                let c = rdof_jget(doc, i, k.as_ptr());
                if let Some(cn) = node(doc, c).filter(|_| c >= 0) {
                    if !int_us(cn, what, out, &e) {
                        return false;
                    }
                }
            }
        } else {
            if !int_us(n, b"max_err_us", &mut t.max_err_pps_us, &e) {
                return false;
            }
            t.max_err_ntp_us = t.max_err_pps_us;
        }
        for b in [t.max_err_pps_us, t.max_err_ntp_us] {
            if !(b > 0 && b <= RDOF_SLOT_MAX_OFFSET_US) {
                return e.set(&[S(b"slot_timing.max_err_us "), I(b.into()), S(b": above 0, at most "), MAX]);
            }
        }
    }
    if let Some((_, n)) = pick(c"on_loss") {
        match string(n) {
            Some(b"holdover") => t.on_loss = RDOF_LOSS_HOLDOVER,
            Some(b"free") => t.on_loss = RDOF_LOSS_FREE,
            Some(b"stop") => t.on_loss = RDOF_LOSS_STOP,
            _ => return e.set(&[S(b"slot_timing.on_loss: holdover, free or stop")]),
        }
    }
    if let Some((_, n)) = pick(c"holdover_s") {
        if n.type_ != RDOF_JNUM || !(n.num >= 0.0 && n.num <= 86400.0) {
            return e.set(&[S(b"slot_timing.holdover_s: 0 to 86400 s")]);
        }
        t.holdover_s = n.num;
    }
    true
}

const LOSS_NAMES: [&[u8]; 3] = [b"holdover", b"free", b"stop"];

#[no_mangle]
pub unsafe extern "C" fn rdof_slot_timing_describe(t: &RdofSlotTiming, out: *mut c_char, cap: usize) -> usize {
    // repr(round(holdover_s, 3)): at most three decimals, at least one (%.3f rounds half to even; char[32])
    let mut buf = [0u8; 32];
    let mut h = fixed(t.holdover_s, 3, &mut buf);
    while h.len() > 1 && h[h.len() - 1] == b'0' && h[h.len() - 2] != b'.' {
        h = &h[..h.len() - 1];
    }
    print(
        slice_mut(out.cast(), cap),
        &[
            S(if t.utc { b"utc" } else { b"free" }),
            S(b","),
            I(t.offset_us.into()),
            S(b","),
            I(t.max_err_pps_us.into()),
            S(b","),
            I(t.max_err_ntp_us.into()),
            S(b","),
            S(LOSS_NAMES.get(t.on_loss as usize).copied().unwrap_or(b"?")),
            S(b","),
            S(h),
        ],
    )
}

// ── the gate ──────────────────────────────────────────────────────────────────

#[no_mangle]
pub extern "C" fn rdof_slot_gate_init(g: &mut RdofSlotGate, t: &RdofSlotTiming) {
    *g = RdofSlotGate {
        timing: *t,
        state: if t.utc && t.on_loss == RDOF_LOSS_STOP { RDOF_GATE_STOPPED } else { RDOF_GATE_FREE },
        has_last_ok: false,
        last_ok: 0.0,
        source: [0; 8],
        err_us: f64::NAN,
        why: [0; 64],
    };
    print(&mut g.source, &[S(b"none")]);
    print(&mut g.why, &[S(if t.utc { b"no clock yet" } else { b"slot timing free" })]);
}

#[no_mangle]
pub unsafe extern "C" fn rdof_slot_gate_update(
    g: &mut RdofSlotGate,
    now_s: f64,
    synced: bool,
    source: *const c_char,
    err_us: f64,
) -> u32 {
    let t = g.timing;
    let source = if source.is_null() { b"none" } else { c_bytes(source) };
    print(&mut g.source, &[S(source)]);
    g.err_us = err_us;
    if !t.utc {
        g.state = RDOF_GATE_FREE;
        print(&mut g.why, &[S(b"slot timing free")]);
        return g.state;
    }
    let bound = match source {
        b"pps" => t.max_err_pps_us,
        b"ntp" => t.max_err_ntp_us,
        _ => -1,
    };
    let why: &[Piece] = if !synced {
        &[S(b"the clock isn't synced")]
    } else if bound < 0 {
        &[S(b"a "), S(source), S(b" clock can't time slots")]
    } else if err_us.is_nan() {
        &[S(b"no error estimate")]
    } else if err_us > f64::from(bound) {
        &[S(b"clock error over "), I(bound.into()), S(b" us")]
    } else {
        g.state = RDOF_GATE_ALIGNED;
        g.why[0] = 0;
        g.has_last_ok = true;
        g.last_ok = now_s;
        return g.state;
    };
    print(&mut g.why, why);
    g.state = if t.on_loss == RDOF_LOSS_STOP {
        RDOF_GATE_STOPPED
    } else if t.on_loss == RDOF_LOSS_HOLDOVER && g.has_last_ok && now_s - g.last_ok <= t.holdover_s {
        RDOF_GATE_HOLDOVER
    } else {
        RDOF_GATE_FREE
    };
    g.state
}

#[no_mangle]
pub extern "C" fn rdof_slot_gate_holdover_age(g: &RdofSlotGate, now_s: f64) -> f64 {
    if g.state == RDOF_GATE_ALIGNED || !g.has_last_ok {
        return 0.0;
    }
    now_s - g.last_ok
}

#[no_mangle]
pub extern "C" fn rdof_gate_state_name(s: u32) -> *const c_char {
    const NAMES: [&CStr; 4] = [c"free", c"aligned", c"holdover", c"stopped"];
    NAMES.get(s as usize).unwrap_or(&c"?").as_ptr()
}

// ── the sample clock ──────────────────────────────────────────────────────────

#[no_mangle]
pub extern "C" fn rdof_sample_clock_init(c: &mut RdofSampleClock, rate_hz: f64, window_s: f64, stamped: bool) {
    *c = RdofSampleClock {
        nominal: rate_hz,
        ppm: 0.0,
        window_s,
        stamped,
        has_anchor: false,
        i0: 0,
        t0: 0,
        has_err: false,
        err_ns: 0,
        has_best: false,
        best_r: 0,
        best_i: 0,
        best_t: 0,
        win_start: 0,
        moved: 0,
        windows: 0,
    };
}

impl RdofSampleClock {
    fn rate(&self) -> f64 {
        self.nominal * (1.0 + self.ppm * 1e-6)
    }

    fn utc(&self, index: f64) -> Option<i64> {
        // round half up; `as` saturates, NaN 0, as the C's to_i64
        self.has_anchor.then(|| {
            let x = (index - self.i0 as f64) * 1e9 / self.rate();
            self.t0.wrapping_add(unsafe { floor(x + 0.5) } as i64)
        })
    }
}

#[no_mangle]
pub extern "C" fn rdof_sample_clock_utc(c: &RdofSampleClock, index: f64, t_ns: &mut i64) -> bool {
    c.utc(index).map(|t| *t_ns = t).is_some()
}

#[no_mangle]
pub extern "C" fn rdof_sample_clock_index(c: &RdofSampleClock, t_ns: i64, index: &mut f64) -> bool {
    if !c.has_anchor {
        return false;
    }
    *index = c.i0 as f64 + t_ns.wrapping_sub(c.t0) as f64 * c.rate() / 1e9;
    true
}

#[no_mangle]
pub extern "C" fn rdof_sample_clock_add(c: &mut RdofSampleClock, end_index: i64, arrival_ns: i64) {
    if c.stamped {
        c.has_anchor = true;
        (c.i0, c.t0) = (end_index, arrival_ns);
        c.has_err = true;
        c.err_ns = 0;
        return;
    }
    let Some(line) = c.utc(end_index as f64) else {
        c.has_anchor = true;
        (c.i0, c.t0) = (end_index, arrival_ns);
        c.has_best = true;
        (c.best_r, c.best_i, c.best_t) = (0, end_index, arrival_ns);
        c.win_start = end_index;
        c.moved = 0;
        return;
    };
    let mut resid = arrival_ns.wrapping_sub(line);
    if resid < 0 {
        // earlier than the line allows: it was late
        (c.i0, c.t0) = (end_index, arrival_ns);
        c.moved = c.moved.max(resid.wrapping_neg());
        resid = 0;
    }
    if !c.has_best || resid <= c.best_r {
        c.has_best = true;
        (c.best_r, c.best_i, c.best_t) = (resid, end_index, arrival_ns);
    }
    if end_index.wrapping_sub(c.win_start) as f64 >= c.window_s * c.nominal {
        (c.i0, c.t0) = (c.best_i, c.best_t);
        c.has_err = true;
        c.err_ns = c.best_r.max(c.moved);
        c.windows = c.windows.wrapping_add(1);
        c.has_best = false;
        c.win_start = end_index;
        c.moved = 0;
    }
}
