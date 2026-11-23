// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! Several carriers in one transmit job (rdof/sigmulti.h; docs/spec/bench-api.md 14.6): any signal mode's
//! transmitter, once a carrier, summed into one complex baseband at the transmit stream's rate. A trunked system's
//! control channel beside a voice channel (P25, DMR Tier III) from one SDR, each carrier the mode's own job.
//!
//! Each carrier is an object in the job's `carriers`: its `freq` (MHz), `start_s` (when it keys, from the job's
//! key-up), `level` (its share of full scale; 1/N by default, the sum at most 1), and any of the mode's fields, over
//! the job's own (the job's fields are every carrier's defaults). Each carrier's source renders at its own rate (the
//! platform's sig_rate, or the mode's), spread over the output's samples linearly, as the SDR modem spreads one
//! (rdof_sdrmodem_signal), then mixed to its offset from the job's `freq`. A mode whose job takes input after the
//! request (a stream) can't be a carrier: there is one input. Rust only: no C twin.

use core::ffi::{c_char, c_int, c_void};

use crate::ffi::{rint, snprintf};
use crate::jsonp::{rdof_jat, rdof_jget, rdof_jnode, JDoc, JNode, RDOF_JNUM, RDOF_JOBJ};
use crate::sigiq::RdofSigSrc;

extern "C" {
    fn calloc(n: usize, size: usize) -> *mut c_void;
    fn free(p: *mut c_void);
}

type Fn0 = Option<unsafe extern "C" fn()>;

/// rdof_sig_mode_t (the receive half as opaque pointers: only the transmit half is called here)
#[repr(C)]
pub struct RdofSigMode {
    pub name: *const c_char,
    pub about: *const c_char,
    pub tx_new: Option<
        unsafe extern "C" fn(*const JDoc, c_int, f64, *mut f64, *mut c_char, usize) -> *mut c_void,
    >,
    pub tx_free: Option<unsafe extern "C" fn(*mut c_void)>,
    pub tx_src: Option<unsafe extern "C" fn(*mut c_void) -> RdofSigSrc>,
    pub tx_in_room: Fn0,
    pub tx_in: Fn0,
    pub tx_in_end: Fn0,
    pub tx_in_want: Option<unsafe extern "C" fn(*mut c_void) -> u64>,
    pub tx_report: Option<unsafe extern "C" fn(*mut c_void, *mut c_char, usize) -> usize>,
    pub tx_record: Fn0,
    pub rx_new: Fn0,
    pub rx_free: Fn0,
    pub rx_streams: Fn0,
    pub rx_feed: Fn0,
    pub rx_feed_freq: Fn0,
    pub rx_record: Fn0,
    pub rx_summary: Fn0,
}

/// Carriers in one job, at most (the bench line's claims).
const MAX: usize = 8;
/// Source samples fetched at a time.
const BLK: usize = 256;
/// The oscillator's table: TAB points a turn, cos and sin, linearly interpolated.
const TAB: usize = 1024;

struct Carrier {
    tx: *mut c_void,
    src: RdofSigSrc,
    freq_mhz: f64,
    start_s: f64,
    level: f32,
    /// output samples before it keys; source samples spread over k output samples each
    start: u64,
    k: u32,
    j: u32,
    /// the source's samples: hz and env (a frequency source) or I and Q; the previous and the current one
    a: [f32; BLK],
    b: [f32; BLK],
    nbuf: usize,
    ibuf: usize,
    prev: (f32, f32),
    cur: (f32, f32),
    /// the source's end: it keyed fewer than asked (then its zeros); every one of its samples out
    src_end: bool,
    over: bool,
    phase: u32,
    /// phase steps an Hz (freq), and the offset's step
    per_hz: f32,
    off_step: u32,
}

struct Multi {
    n: usize,
    c: [*mut Carrier; MAX],
    m: *const RdofSigMode,
    rate: f64,
    pos: u64,
    cos: [f32; TAB + 1],
    sin: [f32; TAB + 1],
}

unsafe fn say(error: *mut c_char, ecap: usize, msg: *const c_char, x: f64) {
    if ecap > 0 {
        snprintf(error, ecap, msg, x);
    }
}

unsafe fn num(doc: &JDoc, obj: c_int, key: &core::ffi::CStr, dflt: f64) -> f64 {
    let n = rdof_jnode(doc, rdof_jget(doc, obj, key.as_ptr()));
    if !n.is_null() && (*n).type_ == RDOF_JNUM {
        (*n).num
    } else {
        dflt
    }
}

/// A doc whose node `root` is carrier `ci`'s job: the job's members, then the carrier's (the last of a key wins).
/// The nodes are a copy (freed by the caller); the text stays the job's.
unsafe fn sub_job(d: &JDoc, job: c_int, ci: c_int, out: &mut JDoc) -> c_int {
    let n = d.n.max(0) as usize;
    let count = |o: c_int| -> usize {
        let p = rdof_jnode(d, o);
        if p.is_null() {
            0
        } else {
            (*p).count.max(0) as usize
        }
    };
    let total = n + 1 + count(job) + count(ci);
    let nodes = calloc(total, core::mem::size_of::<JNode>()) as *mut JNode;
    if nodes.is_null() {
        return -1;
    }
    core::ptr::copy_nonoverlapping(d.nodes, nodes, n);
    let root = n;
    let mut at = n + 1;
    let mut last: Option<usize> = None;
    for o in [job, ci] {
        let mut i = 0;
        loop {
            let c = rdof_jat(d, o, i);
            if c < 0 {
                break;
            }
            i += 1;
            let src = &*d.nodes.add(c as usize);
            if o == job && !src.key.is_null() && core::ffi::CStr::from_ptr(src.key) == c"carriers" {
                continue;
            }
            core::ptr::copy_nonoverlapping(src, nodes.add(at), 1);
            (*nodes.add(at)).next = -1;
            match last {
                Some(l) => (*nodes.add(l)).next = at as c_int,
                None => (*nodes.add(root)).child = at as c_int,
            }
            last = Some(at);
            at += 1;
        }
    }
    let r = &mut *nodes.add(root);
    r.type_ = RDOF_JOBJ;
    r.key = core::ptr::null();
    r.count = (at - n - 1) as c_int;
    if last.is_none() {
        r.child = -1;
    }
    r.next = -1;
    out.nodes = nodes;
    out.max = total as c_int;
    out.n = at as c_int;
    out.buf = d.buf;
    out.cap = d.cap;
    out.used = d.used;
    root as c_int
}

unsafe fn free_multi(t: *mut Multi) {
    if t.is_null() {
        return;
    }
    let m = &*(*t).m;
    for i in 0..(*t).n {
        let c = (*t).c[i];
        if !c.is_null() {
            if !(*c).tx.is_null() {
                if let Some(f) = m.tx_free {
                    f((*c).tx);
                }
            }
            free(c.cast());
        }
    }
    free(t.cast());
}

/// The job's carriers as one source: NULL with the reason in error. `carriers` is the job's array of objects, `base_mhz`
/// the job's freq (0 Hz), fs the platform's rate for a mode's source, rate the output's (the transmit stream's).
#[no_mangle]
pub unsafe extern "C" fn rdof_sigmulti_tx_new(
    m: *const RdofSigMode,
    d: *const JDoc,
    job: c_int,
    carriers: c_int,
    base_mhz: f64,
    fs: f64,
    rate: f64,
    seconds: *mut f64,
    error: *mut c_char,
    ecap: usize,
) -> *mut c_void {
    let doc = &*d;
    let mode = &*m;
    let (Some(tx_new), Some(tx_src)) = (mode.tx_new, mode.tx_src) else {
        say(error, ecap, c"carriers: this mode doesn't transmit".as_ptr(), 0.0);
        return core::ptr::null_mut();
    };
    let arr = rdof_jnode(doc, carriers);
    let n = if arr.is_null() { 0 } else { (*arr).count.max(0) as usize };
    if n == 0 || n > MAX {
        say(error, ecap, c"carriers: 1 to %.0f".as_ptr(), MAX as f64);
        return core::ptr::null_mut();
    }
    if !(rate >= 8000.0 && fs > 0.0) {
        say(error, ecap, c"carriers: the transmitter's rate (%.0f S/s) isn't known".as_ptr(), rate);
        return core::ptr::null_mut();
    }
    let t = calloc(1, core::mem::size_of::<Multi>()) as *mut Multi;
    if t.is_null() {
        say(error, ecap, c"no memory for the carriers".as_ptr(), 0.0);
        return core::ptr::null_mut();
    }
    (*t).m = m;
    (*t).rate = rate;
    // the oscillator's table, by rotation in f64 (no libm here): 2 pi / TAB a step
    let (cs, sn) = (0.999_981_175_282_601_1f64, 0.006_135_884_649_154_475f64);
    let (mut x, mut y) = (1.0f64, 0.0f64);
    for i in 0..=TAB {
        (*t).cos[i] = x as f32;
        (*t).sin[i] = y as f32;
        (x, y) = (x * cs - y * sn, x * sn + y * cs);
    }
    let mut levels = 0.0;
    let mut most = 0.0f64;
    for i in 0..n {
        let ci = rdof_jat(doc, carriers, i as c_int);
        let node = rdof_jnode(doc, ci);
        let f = num(doc, ci, c"freq", f64::NAN);
        let start_s = num(doc, ci, c"start_s", 0.0);
        let level = num(doc, ci, c"level", 1.0 / n as f64);
        if node.is_null() || (*node).type_ != RDOF_JOBJ || !f.is_finite() {
            say(error, ecap, c"carriers: objects, each with its freq (MHz)".as_ptr(), 0.0);
            free_multi(t);
            return core::ptr::null_mut();
        }
        if !(0.0..=3600.0).contains(&start_s) || !(level > 0.0 && level <= 1.0) {
            say(error, ecap, c"carriers: start_s 0-3600, level 0-1 (carrier %.0f)".as_ptr(), i as f64);
            free_multi(t);
            return core::ptr::null_mut();
        }
        levels += level;
        let c = calloc(1, core::mem::size_of::<Carrier>()) as *mut Carrier;
        if c.is_null() {
            say(error, ecap, c"no memory for the carriers".as_ptr(), 0.0);
            free_multi(t);
            return core::ptr::null_mut();
        }
        (*t).c[i] = c;
        (*t).n = i + 1;
        let mut sub = JDoc {
            nodes: core::ptr::null_mut(),
            max: 0,
            n: 0,
            buf: core::ptr::null_mut(),
            cap: 0,
            used: 0,
            error: [0; 64],
        };
        let root = sub_job(doc, job, ci, &mut sub);
        if root < 0 {
            say(error, ecap, c"no memory for the carriers".as_ptr(), 0.0);
            free_multi(t);
            return core::ptr::null_mut();
        }
        let mut secs = 0.0;
        let tx = tx_new(&sub, root, fs, &mut secs, error, ecap);
        free(sub.nodes.cast());
        if tx.is_null() {
            free_multi(t);
            return core::ptr::null_mut();
        }
        (*c).tx = tx;
        if mode.tx_in_want.is_some_and(|w| w(tx) > 0) {
            say(error, ecap, c"carriers: each one's data in the request (hex), none after it".as_ptr(), 0.0);
            free_multi(t);
            return core::ptr::null_mut();
        }
        let src = tx_src(tx);
        let r = if src.rate > 0.0 { src.rate } else { fs };
        let k = rint(rate / r);
        let dr = rate / k - r;
        if k < 1.0 || dr > 1e-6 * r || -dr > 1e-6 * r {
            say(error, ecap, c"carriers: the mode's %.0f S/s doesn't divide the transmitter's".as_ptr(), r);
            free_multi(t);
            return core::ptr::null_mut();
        }
        let off = (f - base_mhz) * 1e6;
        let c = &mut *c;
        c.src = src;
        c.freq_mhz = f;
        c.start_s = start_s;
        c.level = level as f32;
        c.start = rint(start_s * rate) as u64;
        c.k = k as u32;
        c.per_hz = (4294967296.0 / rate) as f32;
        c.off_step = (rint(rint(off) / rate * 4294967296.0) as i64) as u32;
        most = most.max(start_s + secs);
    }
    if levels > 1.0 + 1e-9 {
        say(error, ecap, c"carriers: the levels add to %.3f, over 1".as_ptr(), levels);
        free_multi(t);
        return core::ptr::null_mut();
    }
    *seconds = most;
    t.cast()
}

#[no_mangle]
pub unsafe extern "C" fn rdof_sigmulti_tx_free(tx: *mut c_void) {
    free_multi(tx as *mut Multi);
}

/// The next source sample (zeros past its end).
unsafe fn next(c: &mut Carrier) -> (f32, f32) {
    if c.ibuf == c.nbuf {
        if c.src_end {
            c.over = true;
            return (0.0, 0.0);
        }
        let got = match (c.src.iq, c.src.freq) {
            (Some(iq), _) => {
                let mut tmp = [0.0f32; 2 * BLK];
                let g = iq(c.src.ctx, tmp.as_mut_ptr(), BLK as c_int).clamp(0, BLK as c_int) as usize;
                for i in 0..g {
                    c.a[i] = tmp[2 * i];
                    c.b[i] = tmp[2 * i + 1];
                }
                g
            }
            (None, Some(fr)) => fr(c.src.ctx, c.a.as_mut_ptr(), c.b.as_mut_ptr(), BLK as c_int).clamp(0, BLK as c_int)
                as usize,
            _ => 0,
        };
        if got < BLK {
            c.src_end = true;
        }
        c.nbuf = got;
        c.ibuf = 0;
        if got == 0 {
            c.over = true;
            return (0.0, 0.0);
        }
    }
    let v = (c.a[c.ibuf], c.b[c.ibuf]);
    c.ibuf += 1;
    v
}

#[inline]
fn osc(t: &Multi, ph: u32) -> (f32, f32) {
    let i = (ph >> 22) as usize;
    let w = ((ph >> 6) & 0xffff) as f32 * (1.0 / 65536.0);
    (t.cos[i] + (t.cos[i + 1] - t.cos[i]) * w, t.sin[i] + (t.sin[i + 1] - t.sin[i]) * w)
}

/// n samples, I and Q interleaved: how many are keyed (fewer: every carrier is over).
unsafe extern "C" fn render(ctx: *mut c_void, out: *mut f32, n: c_int) -> c_int {
    let t = &mut *(ctx as *mut Multi);
    let n = n.max(0) as usize;
    let out = core::slice::from_raw_parts_mut(out, 2 * n);
    out.fill(0.0);
    let mut keyed = 0usize;
    for ci in 0..t.n {
        let c = &mut *t.c[ci];
        if c.over {
            continue;
        }
        let is_iq = c.src.iq.is_some();
        for i in 0..n {
            if t.pos + (i as u64) < c.start {
                keyed = keyed.max(i + 1);
                continue;
            }
            if c.j == 0 {
                c.prev = c.cur;
                c.cur = next(c);
                if c.over {
                    break;
                }
            }
            c.j += 1;
            let w = c.j as f32 / c.k as f32;
            if c.j == c.k {
                c.j = 0;
            }
            let a = c.prev.0 + (c.cur.0 - c.prev.0) * w;
            let b = c.prev.1 + (c.cur.1 - c.prev.1) * w;
            let (re, im) = if is_iq {
                c.phase = c.phase.wrapping_add(c.off_step);
                let (co, si) = osc(t, c.phase);
                (a * co - b * si, a * si + b * co)
            } else {
                c.phase = c.phase.wrapping_add(c.off_step).wrapping_add((a * c.per_hz) as i32 as u32);
                let (co, si) = osc(t, c.phase);
                (b * co, b * si)
            };
            out[2 * i] += c.level * re;
            out[2 * i + 1] += c.level * im;
            keyed = keyed.max(i + 1);
        }
    }
    t.pos += n as u64;
    keyed as c_int
}

unsafe extern "C" fn done(ctx: *mut c_void) -> bool {
    let t = &*(ctx as *const Multi);
    (0..t.n).all(|i| (*t.c[i]).over)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_sigmulti_tx_src(tx: *mut c_void) -> RdofSigSrc {
    let t = &*(tx as *const Multi);
    RdofSigSrc {
        freq: None,
        iq: Some(render),
        done: Some(done),
        peak_hz: 0.0,
        rate: t.rate,
        span_lo_hz: 0.0,
        span_hi_hz: 0.0,
        ctx: tx,
    }
}

/// `,"carriers":[{"freq","start_s","level", the mode's report}, ...]`: its length (0: no room).
#[no_mangle]
pub unsafe extern "C" fn rdof_sigmulti_tx_report(tx: *mut c_void, out: *mut c_char, cap: usize) -> usize {
    let t = &*(tx as *const Multi);
    let m = &*t.m;
    let mut at = 0usize;
    let put = |s: *const c_char, a: f64, b: f64, c: f64, at: &mut usize| -> bool {
        if *at >= cap {
            return false;
        }
        let k = snprintf(out.add(*at), cap - *at, s, a, b, c);
        if k < 0 || *at + k as usize >= cap {
            return false;
        }
        *at += k as usize;
        true
    };
    if !put(c",\"carriers\":[".as_ptr(), 0.0, 0.0, 0.0, &mut at) {
        return 0;
    }
    for i in 0..t.n {
        let c = &*t.c[i];
        let fmt = if i > 0 {
            c",{\"freq\":%.6f,\"start_s\":%.3f,\"level\":%.3f".as_ptr()
        } else {
            c"{\"freq\":%.6f,\"start_s\":%.3f,\"level\":%.3f".as_ptr()
        };
        if !put(fmt, c.freq_mhz, c.start_s, c.level as f64, &mut at) {
            return 0;
        }
        if let Some(r) = m.tx_report {
            if at >= cap {
                return 0;
            }
            at += r(c.tx, out.add(at), cap - at);
        }
        if !put(c"}".as_ptr(), 0.0, 0.0, 0.0, &mut at) {
            return 0;
        }
    }
    if !put(c"]".as_ptr(), 0.0, 0.0, 0.0, &mut at) {
        return 0;
    }
    at
}
