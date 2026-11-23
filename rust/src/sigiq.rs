// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! The bench's `iq` signal mode, transmit only (rdof/sigiq.h; docs/spec/bench-api.md 14.6): complex baseband the
//! client renders, sent as it comes. The client's samples (cs16 LE, I then Q, int16 full scale = |x| 1) at `rate`
//! go out as they are, `freq` their 0 Hz, mixed to the carrier's offset by the SDR modem (which spreads them over its
//! stream when `rate` divides it). Anything the client can render goes out this way: several carriers at once (a
//! trunked control channel beside a voice channel), a modulation the board has no mode for, a recording.
//!
//! Always a stream, as fsk4's `stream`: a ring of `ahead_s` of samples, one writer (the line's input) and one reader
//! (the transmitter's thread), paces the client; key-up waits (carrier off) until it is full, the input ends or 2 s
//! pass. When it runs dry the carrier drops (zeros) and an underrun record goes out; the client's samples resume after
//! it. A half-close ends it; `seconds` is the keying's ceiling. Rust only: no C twin.

use core::ffi::{c_char, c_int, c_void};
use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};

use crate::ffi::snprintf;
use crate::jsonp::{rdof_jget, rdof_jnode, JDoc, RDOF_JBOOL, RDOF_JNUM};

extern "C" {
    fn malloc(n: usize) -> *mut c_void;
    fn calloc(n: usize, size: usize) -> *mut c_void;
    fn free(p: *mut c_void);
}

/// rdof_sig_src_t
#[repr(C)]
pub struct RdofSigSrc {
    pub freq: Option<unsafe extern "C" fn(*mut c_void, *mut f32, *mut f32, c_int) -> c_int>,
    pub iq: Option<unsafe extern "C" fn(*mut c_void, *mut f32, c_int) -> c_int>,
    pub done: Option<unsafe extern "C" fn(*mut c_void) -> bool>,
    pub peak_hz: f64,
    pub rate: f64,
    pub span_lo_hz: f64,
    pub span_hi_hz: f64,
    pub ctx: *mut c_void,
}

/// Seconds key-up waits for the ring to fill.
const WAIT_S: f64 = 2.0;

struct IqTx {
    rate: f64,
    /// the ring: cap bytes, a power of two (whole samples, 4 bytes each)
    ring: *mut u8,
    cap: u32,
    rw: AtomicU32, // bytes written
    rr: AtomicU32, // bytes read
    eof: AtomicBool,
    ready: bool,
    starved: bool,
    done: AtomicBool,
    /// samples rendered (keyed, after the wait), the most it keys, waited (carrier off) and the wait's limit
    pos: u64,
    max_q: u64,
    waited: u64,
    wait_max: u64,
    played: u64,
    zero: u64,
    peak: f32,
    clipped: u64,
    underruns: AtomicU32,
    under_at: AtomicU32, // the last one's sample, from the first: its low and high 32 bits (no 64-bit atomics on
    under_hi: AtomicU32, // the ESP32-S3; read after the job, or once a record, where a torn pair can't matter)
    reported: u32,
}

unsafe fn num(d: *const c_void, obj: c_int, key: &core::ffi::CStr, dflt: f64) -> f64 {
    let doc = &*(d as *const JDoc);
    let n = rdof_jnode(doc, rdof_jget(doc, obj, key.as_ptr()));
    if !n.is_null() && (*n).type_ == RDOF_JNUM {
        (*n).num
    } else {
        dflt
    }
}

unsafe fn flag(d: *const c_void, obj: c_int, key: &core::ffi::CStr) -> Option<bool> {
    let doc = &*(d as *const JDoc);
    let n = rdof_jnode(doc, rdof_jget(doc, obj, key.as_ptr()));
    if n.is_null() {
        None
    } else if (*n).type_ == RDOF_JBOOL {
        Some((*n).b)
    } else if (*n).type_ == RDOF_JNUM {
        Some((*n).num != 0.0)
    } else {
        None
    }
}

unsafe fn say(error: *mut c_char, ecap: usize, msg: &core::ffi::CStr) {
    if ecap > 0 {
        snprintf(error, ecap, c"%s".as_ptr(), msg.as_ptr());
    }
}

/// A transmitter for the job (rdof_sig_mode_t.tx_new): NULL with the reason in error.
#[no_mangle]
pub unsafe extern "C" fn rdof_sigiq_tx_new(
    d: *const c_void,
    job: c_int,
    _fs: f64,
    seconds: *mut f64,
    error: *mut c_char,
    ecap: usize,
) -> *mut c_void {
    let rate = num(d, job, c"rate", 0.0);
    let secs = num(d, job, c"seconds", 0.0);
    let ahead = num(d, job, c"ahead_s", 0.25);
    if !(rate >= 8000.0 && rate <= 20e6) {
        say(error, ecap, c"iq: rate (8000-20000000 S/s), the samples' rate: the transmit stream's divided by a \
              whole number");
        return core::ptr::null_mut();
    }
    if !(secs > 0.0 && secs <= 3600.0) {
        say(error, ecap, c"iq: seconds (0-3600), the most it keys");
        return core::ptr::null_mut();
    }
    if !(ahead >= 0.02 && ahead <= 10.0) {
        say(error, ecap, c"iq: ahead_s 0.02-10");
        return core::ptr::null_mut();
    }
    if flag(d, job, c"stream") == Some(false) {
        say(error, ecap, c"iq: always a stream (the samples after the request, a half-close at their end)");
        return core::ptr::null_mut();
    }
    // a power of two, so the byte counts' wrap at 2^32 keeps their place in it
    let ns = ((ahead * rate) as u64).clamp(256, 16 << 20).next_power_of_two();
    let cap = (ns * 4) as u32;
    let t = calloc(1, core::mem::size_of::<IqTx>()) as *mut IqTx;
    let ring = malloc(cap as usize) as *mut u8;
    if t.is_null() || ring.is_null() {
        free(t.cast());
        free(ring.cast());
        say(error, ecap, c"no memory for the samples");
        return core::ptr::null_mut();
    }
    t.write(IqTx {
        rate,
        ring,
        cap,
        rw: AtomicU32::new(0),
        rr: AtomicU32::new(0),
        eof: AtomicBool::new(false),
        ready: false,
        starved: false,
        done: AtomicBool::new(false),
        pos: 0,
        max_q: (secs * rate) as u64,
        waited: 0,
        wait_max: (WAIT_S * rate) as u64,
        played: 0,
        zero: 0,
        peak: 0.0,
        clipped: 0,
        underruns: AtomicU32::new(0),
        under_at: AtomicU32::new(0),
        under_hi: AtomicU32::new(0),
        reported: 0,
    });
    *seconds = secs + WAIT_S;
    t.cast()
}

#[no_mangle]
pub unsafe extern "C" fn rdof_sigiq_tx_free(tx: *mut c_void) {
    let t = tx as *mut IqTx;
    if t.is_null() {
        return;
    }
    free((*t).ring.cast());
    free(t.cast());
}

unsafe fn sample(t: &IqTx, at: u32) -> (f32, f32) {
    let mut b = [0u8; 4];
    for (k, v) in b.iter_mut().enumerate() {
        *v = *t.ring.add((at.wrapping_add(k as u32) & (t.cap - 1)) as usize);
    }
    let i = i16::from_le_bytes([b[0], b[1]]) as f32 * (1.0 / 32767.0);
    let q = i16::from_le_bytes([b[2], b[3]]) as f32 * (1.0 / 32767.0);
    (i, q)
}

/// n samples, I and Q interleaved (rdof_sig_src_t.iq): how many are keyed (fewer: the end).
unsafe extern "C" fn iq_render(ctx: *mut c_void, out: *mut f32, n: c_int) -> c_int {
    let t = &mut *(ctx as *mut IqTx);
    let out = core::slice::from_raw_parts_mut(out, 2 * n.max(0) as usize);
    for i in 0..n.max(0) as usize {
        let (mut x, mut y) = (0.0f32, 0.0f32);
        let w = t.rw.load(Ordering::Acquire);
        let r = t.rr.load(Ordering::Relaxed);
        let have = w.wrapping_sub(r);
        if !t.ready {
            // the ring full (or the stream over) before the key-up, for up to 2 s, carrier off
            if have < t.cap && !t.eof.load(Ordering::Acquire) && t.waited < t.wait_max {
                t.waited += 1;
                out[2 * i] = 0.0;
                out[2 * i + 1] = 0.0;
                continue;
            }
            t.ready = true;
        }
        if t.pos >= t.max_q || (have < 4 && t.eof.load(Ordering::Acquire)) {
            t.done.store(true, Ordering::Release);
            return i as c_int;
        }
        if have >= 4 {
            (x, y) = sample(t, r);
            t.rr.store(r.wrapping_add(4), Ordering::Release);
            t.played += 1;
            t.starved = false;
            let m = x.abs().max(y.abs());
            if m > t.peak {
                t.peak = m;
            }
            let p = x * x + y * y;
            if p > 1.0 {
                let s = 1.0 / sqrtf(p);
                x *= s;
                y *= s;
                t.clipped += 1;
            }
        } else {
            // dry: an underrun, the carrier off until the client's samples come again
            if !t.starved {
                t.starved = true;
                t.under_at.store(t.pos as u32, Ordering::Relaxed);
                t.under_hi.store((t.pos >> 32) as u32, Ordering::Relaxed);
                t.underruns.fetch_add(1, Ordering::Release);
            }
            t.zero += 1;
        }
        t.pos += 1;
        out[2 * i] = x;
        out[2 * i + 1] = y;
    }
    n
}

fn sqrtf(x: f32) -> f32 {
    // Newton from a bit-level guess: |x| near 1 here, three steps are exact to a float
    let mut g = f32::from_bits((x.to_bits() >> 1) + 0x1fbd_1df5);
    for _ in 0..3 {
        g = 0.5 * (g + x / g);
    }
    g
}

unsafe extern "C" fn iq_done(ctx: *mut c_void) -> bool {
    (*(ctx as *const IqTx)).done.load(Ordering::Acquire)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_sigiq_tx_src(tx: *mut c_void) -> RdofSigSrc {
    let t = &*(tx as *const IqTx);
    RdofSigSrc {
        freq: None,
        iq: Some(iq_render),
        done: Some(iq_done),
        peak_hz: 0.0,
        rate: t.rate,
        span_lo_hz: 0.0,
        span_hi_hz: 0.0,
        ctx: tx,
    }
}

/// Bytes it still expects in all: until the client ends it, as much as the job could play.
#[no_mangle]
pub unsafe extern "C" fn rdof_sigiq_tx_in_want(tx: *mut c_void) -> u64 {
    let t = &*(tx as *const IqTx);
    if t.eof.load(Ordering::Acquire) {
        return 0;
    }
    let most = t.max_q * 4 + 4;
    let w = t.rw.load(Ordering::Relaxed) as u64; // wraps past 4 GB: then it only says "more"
    if w < most {
        most - w
    } else {
        4
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_sigiq_tx_in_room(tx: *mut c_void) -> usize {
    let t = &*(tx as *const IqTx);
    if t.eof.load(Ordering::Acquire) {
        return 0;
    }
    let used = t.rw.load(Ordering::Relaxed).wrapping_sub(t.rr.load(Ordering::Acquire));
    t.cap.saturating_sub(used) as usize
}

#[no_mangle]
pub unsafe extern "C" fn rdof_sigiq_tx_in(tx: *mut c_void, b: *const u8, n: usize) -> usize {
    let room = rdof_sigiq_tx_in_room(tx);
    let t = &*(tx as *const IqTx);
    let k = n.min(room);
    let w = t.rw.load(Ordering::Relaxed);
    for i in 0..k {
        *t.ring.add((w.wrapping_add(i as u32) & (t.cap - 1)) as usize) = *b.add(i);
    }
    t.rw.store(w.wrapping_add(k as u32), Ordering::Release);
    k
}

#[no_mangle]
pub unsafe extern "C" fn rdof_sigiq_tx_in_end(tx: *mut c_void, _gone: bool) {
    (*(tx as *const IqTx)).eof.store(true, Ordering::Release);
}

/// An underrun since the last record: {"underrun":{"n","sample","t"}}, its length; 0 none.
#[no_mangle]
pub unsafe extern "C" fn rdof_sigiq_tx_record(tx: *mut c_void, out: *mut c_char, cap: usize) -> usize {
    let t = &mut *(tx as *mut IqTx);
    let u = t.underruns.load(Ordering::Acquire);
    if u == t.reported {
        return 0;
    }
    t.reported = u;
    let at = t.under_at.load(Ordering::Relaxed) as u64 | (t.under_hi.load(Ordering::Relaxed) as u64) << 32;
    let n = snprintf(
        out,
        cap,
        c"{\"underrun\":{\"n\":%u,\"sample\":%.0f,\"t\":%.4f}}".as_ptr(),
        u as core::ffi::c_uint,
        at as f64,
        at as f64 / t.rate,
    );
    if n > 0 && (n as usize) < cap {
        n as usize
    } else {
        0
    }
}

/// The job's summary members.
#[no_mangle]
pub unsafe extern "C" fn rdof_sigiq_tx_report(tx: *mut c_void, out: *mut c_char, cap: usize) -> usize {
    let t = &*(tx as *const IqTx);
    let n = snprintf(
        out,
        cap,
        c",\"rate\":%.1f,\"stream\":true,\"ahead_bytes\":%u,\"samples\":%llu,\"samples_in\":%llu,\
          \"underruns\":%u,\"zero_samples\":%llu,\"wait_s\":%.3f,\"peak\":%.4f,\"clipped\":%llu".as_ptr(),
        t.rate,
        t.cap as core::ffi::c_uint,
        t.pos as core::ffi::c_ulonglong,
        t.played as core::ffi::c_ulonglong,
        t.underruns.load(Ordering::Acquire) as core::ffi::c_uint,
        t.zero as core::ffi::c_ulonglong,
        t.waited as f64 / t.rate,
        t.peak as f64,
        t.clipped as core::ffi::c_ulonglong,
    );
    if n > 0 && (n as usize) < cap {
        n as usize
    } else {
        0
    }
}
