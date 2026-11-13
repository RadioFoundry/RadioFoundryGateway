// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! A trunked site's settings into the controller's (rdof/t3cfg.h, docs/spec/fw/tier3.md sections 3.8 and 4): the
//! `tier3` app's object, and the manager's provisioning (rdof/mnctl.h's rdof_mn_site_cfg_t). The plan (siteplan.c)
//! and the ID sets (idset.c) stay C; the JSON reader is jsonp.rs. %g in errors through the C library.

use core::ffi::{c_char, c_int, CStr};

use crate::ffi::{floor, llround, snprintf};
use crate::jsonp::{rdof_jget, rdof_jnode, JDoc, JNode, RDOF_JARR, RDOF_JBOOL, RDOF_JNUM, RDOF_JOBJ, RDOF_JSTR};
use crate::plan::{field, rdof_idset_ranges, Plan, RdofIdrange, RdofIdset, RdofPlan, SITE_NAME, UNIT_TRUNK};
use crate::t3site::{cfg_default, Carrier, Cfg, IdSet, Policy, Range, CARRIERS, IDSET_RANGES};
use crate::text::{c_bytes, print, Piece, Piece::I, Piece::S};

const ID_MAX: f64 = 16_777_215.0; // RDOF_ID_MAX

// ── rdof/mnctl.h's provisioning, as rdof_t3cfg_site reads it ──────────────────

const MN_CFG_TIMERS: usize = 16;

const MN_HAS_MODEL: u32 = 1 << 0;
const MN_HAS_NET: u32 = 1 << 1;
const MN_HAS_SITE: u32 = 1 << 2;
const MN_HAS_PAR: u32 = 1 << 3;
const MN_HAS_AREA_BITS: u32 = 1 << 4;
const MN_HAS_REGISTRATION: u32 = 1 << 5;
const MN_HAS_MS: u32 = 1 << 6;
const MN_HAS_REFUSE: u32 = 1 << 7;
const MN_HAS_PRIVATE: u32 = 1 << 8;
const MN_HAS_TG: u32 = 1 << 9;
const MN_HAS_WIDE_TG: u32 = 1 << 10;
const MN_HAS_STATIC_TG: u32 = 1 << 11;
const MN_HAS_REQUIRE_REG: u32 = 1 << 12;
const MN_HAS_FOACSU: u32 = 1 << 13;

// rdof_mn_timer_t, rdof_mn_site_cfg_t: mirrored with the rest of mqtt-net in t3unit.rs
pub use crate::t3unit::{MnSiteCfg, MnTimer};

// rdof/idset.h (C)
extern "C" {
    fn rdof_idset_clear(s: *mut RdofIdset);
    fn rdof_idset_add(s: *mut RdofIdset, items: *const c_char, err: *mut c_char, cap: usize) -> bool;
    fn rdof_idset_add_id(s: *mut RdofIdset, id: u32, err: *mut c_char, cap: usize) -> bool;
    fn rdof_idset_finish(s: *mut RdofIdset);
}

// ── the model and the timers ──────────────────────────────────────────────────

/// MODEL's NET and SITE bits (361-4 6.3.2.2.1.1)
fn model_bits(model: c_int) -> (c_int, c_int) {
    const NET: [c_int; 4] = [9, 7, 4, 2];
    const SITE: [c_int; 4] = [3, 5, 8, 10];
    let m = (model & 3) as usize;
    (NET[m], SITE[m])
}

const MODELS: [&[u8]; 4] = [b"tiny", b"small", b"large", b"huge"];
const SYSTEM_KEYS: &[&[u8]] = &[b"model", b"net", b"site", b"par", b"area_bits"];
const BROADCAST_KEYS: &[&[u8]] = &[
    b"adjacent_s",
    b"chan_freq_s",
    b"local_time_s",
    b"call_timers_s",
    b"vote_now_s",
    b"ann_tscc_s",
    b"vote_blk",
    b"priority",
];
const SERVICES_KEYS: &[&[u8]] =
    &[b"emergency_preempt", b"power_save", b"power_save_awake_s", b"mass_reg_at_start_s", b"hibernate_s", b"duplex", b"composite_tscc"];

/// Where a timer goes in the settings: a code, ms from seconds, or seconds.
#[derive(Clone, Copy)]
enum At {
    U8(fn(&mut Cfg) -> &mut u8),
    Ms(fn(&mut Cfg) -> &mut u32),
    S(fn(&mut Cfg) -> &mut u32),
}

/// The timers (tier3.md 3.8): each a cfg field in its unit, its range.
struct Timer {
    name: &'static [u8],
    at: At,
    lo: f64,
    hi: f64,
    frac: bool,
}

const fn tm(name: &'static [u8], at: At, lo: f64, hi: f64, frac: bool) -> Timer {
    Timer { name, at, lo, hi, frac }
}

static TIMERS: [Timer; 23] = [
    tm(b"nrand_wait", At::U8(|c| &mut c.nrand_wait), 0.0, 15.0, false),
    tm(b"backoff_min", At::U8(|c| &mut c.backoff_min), 1.0, 15.0, false),
    tm(b"backoff_max", At::U8(|c| &mut c.backoff_max), 1.0, 15.0, false),
    tm(b"tp_s", At::Ms(|c| &mut c.tp_ms), 4.0, 60.0, false),
    tm(b"tnp_s", At::Ms(|c| &mut c.tnp_ms), 2.0, 60.0, false),
    tm(b"hang_s", At::Ms(|c| &mut c.hang_ms), 1.0, 60.0, true),
    tm(b"call_s", At::Ms(|c| &mut c.call_ms), 1.0, 4094.0, false),
    tm(b"emergency_call_s", At::Ms(|c| &mut c.emergency_call_ms), 1.0, 510.0, false),
    tm(b"ahoy_tries", At::U8(|c| &mut c.ahoy_tries), 1.0, 10.0, false),
    tm(b"ahoy_every_s", At::Ms(|c| &mut c.ahoy_every_ms), 0.3, 5.0, true),
    tm(b"grant_every_s", At::Ms(|c| &mut c.grant_every_ms), 0.3, 10.0, true),
    tm(b"clear_repeats", At::U8(|c| &mut c.clear_repeats), 1.0, 8.0, false),
    tm(b"warmup_cach", At::U8(|c| &mut c.warmup_cach), 2.0, 4.0, false),
    tm(b"reg_hold_s", At::S(|c| &mut c.reg_hold_s), 60.0, 604_800.0, false),
    tm(b"queue_max", At::U8(|c| &mut c.queue_max), 1.0, 16.0, false),
    tm(b"queue_s", At::Ms(|c| &mut c.queue_ms), 1.0, 60.0, false),
    tm(b"net_queue_s", At::Ms(|c| &mut c.net_queue_ms), 0.0, 10.0, true),
    tm(b"setup_s", At::Ms(|c| &mut c.setup_ms), 1.0, 30.0, true),
    tm(b"start_grace_s", At::Ms(|c| &mut c.start_grace_ms), 0.0, 10.0, true),
    tm(b"sd_tries", At::U8(|c| &mut c.sd_tries), 1.0, 10.0, false),
    tm(b"sd_repeats", At::U8(|c| &mut c.sd_repeats), 1.0, 10.0, false),
    tm(b"sd_store_s", At::Ms(|c| &mut c.sd_store_ms), 0.0, 600.0, false),
    tm(b"data_hang_s", At::Ms(|c| &mut c.data_hang_ms), 0.5, 30.0, true),
];

// ── text ──────────────────────────────────────────────────────────────────────

/// %g
fn g(v: f64, buf: &mut [u8; 32]) -> &[u8] {
    let n = unsafe { snprintf(buf.as_mut_ptr().cast(), buf.len(), c"%g".as_ptr(), v) };
    &buf[..(n.max(0) as usize).min(buf.len() - 1)]
}

/// snprintf(buf, sizeof buf, "%s%s", a, b): a path and its part, cut to fit.
fn join<'b>(buf: &'b mut [u8], a: &[u8], b: &[u8]) -> &'b [u8] {
    let n = print(buf, &[S(a), S(b)]).min(buf.len() - 1);
    &buf[..n]
}

fn is_whole(v: f64) -> bool {
    v == unsafe { floor(v) }
}

/// The error buffer (cleared), or none when there's no room.
unsafe fn err_buf<'a>(err: *mut c_char, cap: usize) -> &'a mut [u8] {
    if err.is_null() || cap == 0 {
        &mut []
    } else {
        *err = 0;
        core::slice::from_raw_parts_mut(err.cast::<u8>(), cap)
    }
}

// ── the parse ─────────────────────────────────────────────────────────────────

struct Cx<'a> {
    doc: Option<&'a JDoc>,
    err: &'a mut [u8],
}

impl<'a> Cx<'a> {
    /// snprintf(err, cap, ...); false, to return.
    fn fail(&mut self, pieces: &[Piece]) -> bool {
        print(self.err, pieces);
        false
    }

    /// "path.key: lo to hi": %g, or %.0f when the range is whole numbers.
    fn range(&mut self, path: &[u8], key: &[u8], lo: f64, hi: f64, frac: bool) -> bool {
        if frac {
            let (mut a, mut b) = ([0u8; 32], [0u8; 32]);
            let (a, b) = (g(lo, &mut a), g(hi, &mut b));
            self.fail(&[S(path), S(b"."), S(key), S(b": "), S(a), S(b" to "), S(b)])
        } else {
            self.fail(&[S(path), S(b"."), S(key), S(b": "), I(lo as i64), S(b" to "), I(hi as i64)])
        }
    }

    fn n(&self, i: c_int) -> Option<&'a JNode> {
        let doc = self.doc?;
        if i < 0 {
            return None;
        }
        unsafe { rdof_jnode(doc, i).as_ref() }
    }

    fn get(&self, obj: c_int, key: &CStr) -> c_int {
        match self.doc {
            Some(doc) if obj >= 0 => unsafe { rdof_jget(doc, obj, key.as_ptr()) },
            _ => -1,
        }
    }

    fn is_type(&self, i: c_int, t: u32) -> bool {
        self.n(i).is_some_and(|n| n.type_ == t)
    }

    /// A string node's text, NULL for any other.
    fn str_ptr(&self, i: c_int) -> *const c_char {
        match self.n(i) {
            Some(n) if n.type_ == RDOF_JSTR => n.str_,
            _ => core::ptr::null(),
        }
    }

    fn str_of(&self, i: c_int) -> Option<&'a [u8]> {
        let p = self.str_ptr(i);
        (!p.is_null()).then(|| unsafe { c_bytes(p) })
    }

    fn num_of(&self, i: c_int) -> f64 {
        match self.n(i) {
            Some(n) if n.type_ == RDOF_JNUM => n.num,
            _ => f64::NAN,
        }
    }

    fn first_child(&self, i: c_int) -> c_int {
        self.n(i).map_or(-1, |n| n.child)
    }

    /// Every key of obj one of keys.
    fn known(&mut self, obj: c_int, path: &[u8], keys: &[&[u8]]) -> bool {
        if obj < 0 {
            return true;
        }
        if !self.is_type(obj, RDOF_JOBJ) {
            return self.fail(&[S(path), S(b": an object")]);
        }
        let mut i = self.first_child(obj);
        while let Some(n) = self.n(i) {
            let key = unsafe { c_bytes(n.key) };
            if !keys.contains(&key) {
                return self.fail(&[S(path), S(b"."), S(key), S(b": not a key here")]);
            }
            i = n.next;
        }
        true
    }

    /// A number in [lo, hi], integral unless frac; absent: *out untouched.
    #[allow(clippy::too_many_arguments)]
    fn num_in(&mut self, obj: c_int, key: &CStr, path: &[u8], lo: f64, hi: f64, frac: bool, out: &mut f64) -> bool {
        let i = self.get(obj, key);
        if i < 0 {
            return true;
        }
        let v = self.num_of(i);
        if !(v >= lo && v <= hi) || (!frac && !is_whole(v)) {
            return self.range(path, key.to_bytes(), lo, hi, frac);
        }
        *out = v;
        true
    }

    fn u8_in(&mut self, obj: c_int, key: &CStr, path: &[u8], lo: c_int, hi: c_int, out: &mut u8) -> bool {
        let mut v = f64::from(*out);
        if !self.num_in(obj, key, path, f64::from(lo), f64::from(hi), false, &mut v) {
            return false;
        }
        *out = v as u8;
        true
    }

    /// seconds into ms
    fn ms_in(&mut self, obj: c_int, key: &CStr, path: &[u8], lo: f64, hi: f64, out: &mut u32) -> bool {
        let mut v = f64::from(*out) / 1000.0;
        if !self.num_in(obj, key, path, lo, hi, true, &mut v) {
            return false;
        }
        *out = unsafe { llround(v * 1000.0) } as u32;
        true
    }

    fn bool_of(&mut self, obj: c_int, key: &CStr, path: &[u8], out: &mut u8) -> bool {
        let i = self.get(obj, key);
        if i < 0 {
            return true;
        }
        match self.n(i) {
            Some(n) if n.type_ == RDOF_JBOOL => {
                *out = u8::from(n.b);
                true
            }
            _ => self.fail(&[S(path), S(b"."), S(key.to_bytes()), S(b": true or false")]),
        }
    }

    /// A parsed ID set into the controller's ranges.
    fn idset_t3(&mut self, path: &[u8], key: &[u8], s: &RdofIdset, out: &mut IdSet) -> bool {
        *out = IdSet::default();
        if s.star && s.nout == 0 {
            out.all = 1;
            return true;
        }
        let mut r = [RdofIdrange { lo: 0, hi: 0 }; IDSET_RANGES];
        let n = unsafe { rdof_idset_ranges(s, r.as_mut_ptr(), IDSET_RANGES as c_int) };
        if n > IDSET_RANGES as c_int {
            return self.fail(&[
                S(path),
                S(b"."),
                S(key),
                S(b": "),
                I(n.into()),
                S(b" ranges, the site holds "),
                I(IDSET_RANGES as i64),
            ]);
        }
        let n = n.max(0) as usize;
        for (o, r) in out.r.iter_mut().zip(&r[..n]) {
            *o = Range { lo: r.lo, hi: r.hi };
        }
        out.n = n as u8;
        true
    }

    /// An ID set (config-v3 section 3) into the controller's ranges.
    fn idset_of(&mut self, obj: c_int, key: &CStr, path: &[u8], out: &mut IdSet) -> bool {
        let i = self.get(obj, key);
        if i < 0 {
            return true;
        }
        let k = key.to_bytes();
        let mut why = [0u8; 96];
        let mut s: RdofIdset = unsafe { core::mem::zeroed() };
        unsafe { rdof_idset_clear(&mut s) };
        let ty = self.n(i).map_or(u32::MAX, |n| n.type_);
        if ty == RDOF_JBOOL && !self.n(i).is_some_and(|n| n.b) {
            // false: none
        } else if ty == RDOF_JNUM {
            if !add_id(&mut s, self.num_of(i), &mut why) {
                return self.fail(&[S(path), S(b"."), S(k), S(b": an ID from 1 to 16777215")]);
            }
        } else if ty == RDOF_JSTR {
            if !unsafe { rdof_idset_add(&mut s, self.str_ptr(i), why.as_mut_ptr().cast(), why.len()) } {
                return self.fail(&[S(path), S(b"."), S(k), S(b": "), S(field(&why))]);
            }
        } else if ty == RDOF_JARR {
            let mut it = self.first_child(i);
            let mut idx: i64 = 0;
            while let Some(item) = self.n(it) {
                if item.type_ == RDOF_JNUM {
                    if !add_id(&mut s, item.num, &mut why) {
                        return self.fail(&[
                            S(path),
                            S(b"."),
                            S(k),
                            S(b"["),
                            I(idx),
                            S(b"]: an ID from 1 to 16777215"),
                        ]);
                    }
                } else {
                    let p = self.str_ptr(it);
                    if p.is_null() || !unsafe { rdof_idset_add(&mut s, p, why.as_mut_ptr().cast(), why.len()) } {
                        let reason: &[u8] =
                            if p.is_null() { b"an ID, a range \"A-B\", \"*\" or \"!...\"" } else { field(&why) };
                        return self.fail(&[S(path), S(b"."), S(k), S(b"["), I(idx), S(b"]: "), S(reason)]);
                    }
                }
                it = item.next;
                idx += 1;
            }
        } else {
            return self.fail(&[
                S(path),
                S(b"."),
                S(k),
                S(b": an ID set: 202, \"3100-3199\", \"*\", \"!350\", a list of those, or false"),
            ]);
        }
        unsafe { rdof_idset_finish(&mut s) };
        self.idset_t3(path, k, &s, out)
    }

    fn system_keys(&mut self, o: c_int, path: &[u8], top: bool, cfg: &mut Cfg) -> bool {
        let mut pb = [0u8; 96];
        let p = join(&mut pb, path, if top { b"" } else { b".system" });
        if !top && !self.known(o, p, SYSTEM_KEYS) {
            return false;
        }
        let m = self.get(o, c"model");
        if !top && m >= 0 {
            let s = self.str_of(m);
            let Some(k) = MODELS.iter().position(|&name| s == Some(name)) else {
                return self.fail(&[S(p), S(b".model: tiny, small, large or huge")]);
            };
            cfg.model = k as u8;
            if self.get(o, c"area_bits") < 0 {
                cfg.area_bits = model_bits(k as c_int).1 as u8; // every site its own registration area
            }
        }
        let (nb, sb) = model_bits(cfg.model.into());
        let mut v = f64::from(cfg.net);
        if !top && !self.num_in(o, c"net", p, 0.0, f64::from((1 << nb) - 1), false, &mut v) {
            return false;
        }
        cfg.net = v as u16;
        v = f64::from(cfg.site);
        if !self.num_in(o, c"site", p, 1.0, f64::from((1 << sb) - 1), false, &mut v) {
            return false;
        }
        cfg.site = v as u16;
        if !self.u8_in(o, c"area_bits", p, 1, sb, &mut cfg.area_bits) {
            return false;
        }
        let pi = self.get(o, c"par");
        if !top && pi >= 0 {
            cfg.par = match self.str_of(pi) {
                Some(b"a") => 1,
                Some(b"b") => 2,
                Some(b"ab") => 3,
                _ => return self.fail(&[S(p), S(b".par: a, b or ab")]),
            };
        }
        true
    }

    /// Timer name to value v (checked); false with err set ("path.name: range").
    fn set_timer(&mut self, path: &[u8], name: &[u8], v: f64, cfg: &mut Cfg) -> bool {
        let Some(t) = TIMERS.iter().find(|t| t.name == name) else {
            return self.fail(&[S(path), S(b"."), S(name), S(b": not a key here")]);
        };
        let warmup = t.name == b"warmup_cach";
        if !(v >= t.lo && v <= t.hi) || (!t.frac && !is_whole(v)) || (warmup && v == 3.0) {
            if warmup {
                return self.fail(&[S(path), S(b"."), S(name), S(b": 2 or 4")]);
            }
            return self.range(path, name, t.lo, t.hi, t.frac);
        }
        match t.at {
            At::U8(f) => *f(cfg) = v as u8,
            At::Ms(f) => *f(cfg) = unsafe { llround(v * 1000.0) } as u32,
            At::S(f) => *f(cfg) = v as u32,
        }
        true
    }

    fn timer_keys(&mut self, t: c_int, path: &[u8], cfg: &mut Cfg) -> bool {
        let mut pb = [0u8; 96];
        let p = join(&mut pb, path, b".timers");
        if !self.is_type(t, RDOF_JOBJ) {
            return self.fail(&[S(p), S(b": an object")]);
        }
        let mut i = self.first_child(t);
        while let Some(n) = self.n(i) {
            if !self.set_timer(p, unsafe { c_bytes(n.key) }, self.num_of(i), cfg) {
                return false;
            }
            i = n.next;
        }
        true
    }

    fn apply(&mut self, obj: c_int, path: &[u8], cfg: &mut Cfg, pol: &mut Policy) -> bool {
        if !self.is_type(obj, RDOF_JOBJ) {
            return self.fail(&[S(path), S(b": an object")]);
        }
        let sys = self.get(obj, c"system");
        if sys >= 0 && !self.system_keys(sys, path, false, cfg) {
            return false;
        }
        // cfg/node: site and area_bits at the top
        if (self.get(obj, c"site") >= 0 || self.get(obj, c"area_bits") >= 0) && !self.system_keys(obj, path, true, cfg)
        {
            return false;
        }
        let r = self.get(obj, c"registration");
        if r >= 0 {
            let open = match self.str_of(r) {
                Some(b"open") => true,
                Some(b"listed") => false,
                _ => return self.fail(&[S(path), S(b".registration: open or listed")]),
            };
            pol.open = u8::from(open);
            if !open && self.get(obj, c"ms") < 0 && pol.ms.all != 0 {
                pol.ms = IdSet::default(); // listed: the list, none until given
            }
        }
        let au = self.get(obj, c"auth");
        if au >= 0 {
            cfg.auth = match self.str_of(au) {
                Some(b"off") => crate::t3site::AUTH_OFF,
                Some(b"registration") => crate::t3site::AUTH_REGISTRATION,
                Some(b"required") => crate::t3site::AUTH_REQUIRED,
                _ => return self.fail(&[S(path), S(b".auth: off, registration or required")]),
            };
        }
        let ru = self.get(obj, c"refuse_unlisted");
        if ru >= 0 {
            pol.refuse_unlisted = match self.str_of(ru) {
                Some(b"denied") => 0,
                Some(b"refused") => 1,
                _ => return self.fail(&[S(path), S(b".refuse_unlisted: denied or refused")]),
            };
        }
        if !self.idset_of(obj, c"ms", path, &mut pol.ms)
            || !self.idset_of(obj, c"private", path, &mut pol.private_ms)
            || !self.idset_of(obj, c"tg", path, &mut pol.tg)
            || !self.idset_of(obj, c"wide_tg", path, &mut pol.wide_tg)
            || !self.idset_of(obj, c"static_tg", path, &mut pol.static_tg)
        {
            return false;
        }
        if !self.bool_of(obj, c"require_registration", path, &mut cfg.reg_required)
            || !self.bool_of(obj, c"foacsu", path, &mut cfg.foacsu)
            || !self.bool_of(obj, c"dgna_from_ms", path, &mut cfg.dgna_ms)
            || !self.bool_of(obj, c"ambient_listening", path, &mut cfg.als)
            || !self.bool_of(obj, c"call_diversion", path, &mut cfg.divert)
        {
            return false;
        }
        let mut id = f64::from(cfg.data_id);
        if !self.num_in(obj, c"data_gateway", path, 0.0, f64::from(0xFFFCDFu32), false, &mut id) {
            return false;
        }
        cfg.data_id = id as u32;
        let t = self.get(obj, c"timers");
        if t >= 0 && !self.timer_keys(t, path, cfg) {
            return false;
        }
        let sv = self.get(obj, c"services");
        if sv >= 0 {
            let mut pb = [0u8; 96];
            let p = join(&mut pb, path, b".services");
            let mut mass_ms = crate::t3site::reg_window_ms(cfg.mass_reg_start);
            if !self.known(sv, p, SERVICES_KEYS)
                || !self.bool_of(sv, c"emergency_preempt", p, &mut cfg.preempt)
                || !self.bool_of(sv, c"duplex", p, &mut cfg.duplex)
                || !self.bool_of(sv, c"composite_tscc", p, &mut cfg.composite)
                || !self.u8_in(sv, c"power_save", p, 0, 7, &mut cfg.power_save)
                || !self.ms_in(sv, c"power_save_awake_s", p, 0.1, 60.0, &mut cfg.awake_ms)
                || !self.ms_in(sv, c"mass_reg_at_start_s", p, 0.0, 200_000.0, &mut mass_ms)
                || !self.ms_in(sv, c"hibernate_s", p, 0.0, 300.0, &mut cfg.hibernate_ms)
            {
                return false;
            }
            if cfg.hibernate_ms != 0 && cfg.hibernate_ms < 1000 {
                return self.fail(&[S(p), S(b".hibernate_s: 0 (never) or 1 to 300")]);
            }
            // the Reg_Window that covers it (table 6.12)
            cfg.mass_reg_start = crate::t3site::reg_window_of(f64::from(mass_ms) / 1000.0);
        }
        let b = self.get(obj, c"broadcast");
        if b >= 0 {
            let mut pb = [0u8; 96];
            let p = join(&mut pb, path, b".broadcast");
            if !self.known(b, p, BROADCAST_KEYS)
                || !self.ms_in(b, c"adjacent_s", p, 0.0, 600.0, &mut cfg.adjacent_ms)
                || !self.ms_in(b, c"chan_freq_s", p, 0.0, 600.0, &mut cfg.chan_freq_ms)
                || !self.ms_in(b, c"local_time_s", p, 0.0, 600.0, &mut cfg.local_time_ms)
                || !self.ms_in(b, c"call_timers_s", p, 0.0, 600.0, &mut cfg.call_timers_ms)
                || !self.ms_in(b, c"vote_now_s", p, 0.0, 600.0, &mut cfg.vote_now_ms)
                || !self.ms_in(b, c"ann_tscc_s", p, 0.0, 600.0, &mut cfg.ann_tscc_ms)
                || !self.u8_in(b, c"vote_blk", p, 2, 10, &mut cfg.vote_blk)
                || !self.u8_in(b, c"priority", p, 0, 7, &mut cfg.priority)
            {
                return false;
            }
            // 0 is off; anything else at least every TSCC slot pair
            let each = [cfg.adjacent_ms, cfg.chan_freq_ms, cfg.local_time_ms, cfg.call_timers_ms, cfg.vote_now_ms, cfg.ann_tscc_ms];
            if each.iter().any(|&ms| ms != 0 && ms < 1000) {
                return self.fail(&[S(p), S(b": each 0 (off) or 1 to 600 s")]);
            }
        }
        if cfg.backoff_min > cfg.backoff_max {
            return self.fail(&[
                S(path),
                S(b".timers.backoff_min: "),
                I(cfg.backoff_min.into()),
                S(b", over backoff_max "),
                I(cfg.backoff_max.into()),
            ]);
        }
        true
    }

    fn check(&mut self, cfg: &Cfg) -> bool {
        if cfg.model > 3 {
            return self.fail(&[S(b"system.model: tiny, small, large or huge")]);
        }
        let model = MODELS[usize::from(cfg.model)];
        let (nb, sb) = model_bits(cfg.model.into());
        if c_int::from(cfg.net) >= 1 << nb {
            return self.fail(&[S(b"system.net: 0 to "), I(((1 << nb) - 1).into()), S(b" for model "), S(model)]);
        }
        if c_int::from(cfg.site) >= 1 << sb {
            return self.fail(&[S(b"system.site: 0 to "), I(((1 << sb) - 1).into()), S(b" for model "), S(model)]);
        }
        if cfg.area_bits == 0 || c_int::from(cfg.area_bits) > sb {
            return self.fail(&[S(b"system.area_bits: 1 to "), I(sb.into()), S(b" for model "), S(model)]);
        }
        if !(1..=3).contains(&cfg.par) {
            return self.fail(&[S(b"system.par: a, b or ab")]);
        }
        if cfg.cc > 15 {
            return self.fail(&[S(b"cc: 0 to 15")]);
        }
        if cfg.ncarriers == 0 || usize::from(cfg.ncarriers) > CARRIERS {
            return self.fail(&[S(b"carriers: 1 to "), I(CARRIERS as i64)]);
        }
        let cars = &cfg.carriers[..usize::from(cfg.ncarriers)];
        let mut tscc = 0;
        for (i, a) in cars.iter().enumerate() {
            if a.chan == 0 || a.chan > 4094 {
                return self.fail(&[S(b"carriers["), I(i as i64), S(b"].chan: 1 to 4094")]);
            }
            if cars[..i].iter().any(|b| b.chan == a.chan) {
                return self.fail(&[
                    S(b"carriers["),
                    I(i as i64),
                    S(b"].chan: "),
                    I(a.chan.into()),
                    S(b" is another carrier's"),
                ]);
            }
            tscc += i32::from(a.tscc != 0);
        }
        if tscc != 1 {
            return self.fail(&[S(b"carriers: exactly one with tscc")]);
        }
        if cfg.backoff_min < 1 || cfg.backoff_max > 15 || cfg.backoff_min > cfg.backoff_max {
            return self.fail(&[S(b"timers.backoff_min, backoff_max: 1 to 15, min not over max")]);
        }
        if cfg.warmup_cach != 2 && cfg.warmup_cach != 4 {
            return self.fail(&[S(b"timers.warmup_cach: 2 or 4")]);
        }
        true
    }

    fn site(&mut self, sc: &MnSiteCfg, cfg: &mut Cfg, pol: &mut Policy) -> bool {
        let h = sc.has;
        if h & MN_HAS_MODEL != 0 {
            if !(0..=3).contains(&sc.model) {
                return self.fail(&[S(b"cfg.system.model: tiny, small, large or huge")]);
            }
            cfg.model = sc.model as u8;
            if h & MN_HAS_AREA_BITS == 0 {
                cfg.area_bits = model_bits(sc.model).1 as u8;
            }
        }
        if h & MN_HAS_NET != 0 {
            cfg.net = sc.net as u16;
        }
        if h & MN_HAS_SITE != 0 {
            cfg.site = sc.site as u16;
        }
        if h & MN_HAS_PAR != 0 {
            cfg.par = sc.par as u8;
        }
        if h & MN_HAS_AREA_BITS != 0 {
            cfg.area_bits = sc.area_bits as u8;
        }
        if h & MN_HAS_REGISTRATION != 0 {
            pol.open = u8::from(!sc.listed);
            if sc.listed && h & MN_HAS_MS == 0 && pol.ms.all != 0 {
                pol.ms = IdSet::default();
            }
        }
        if h & MN_HAS_REFUSE != 0 {
            pol.refuse_unlisted = u8::from(sc.refused);
        }
        if (h & MN_HAS_MS != 0 && !self.idset_t3(b"cfg", b"ms", &sc.ms, &mut pol.ms))
            || (h & MN_HAS_PRIVATE != 0 && !self.idset_t3(b"cfg", b"private", &sc.private_, &mut pol.private_ms))
            || (h & MN_HAS_TG != 0 && !self.idset_t3(b"cfg", b"tg", &sc.tg, &mut pol.tg))
            || (h & MN_HAS_WIDE_TG != 0 && !self.idset_t3(b"cfg", b"wide_tg", &sc.wide_tg, &mut pol.wide_tg))
            || (h & MN_HAS_STATIC_TG != 0 && !self.idset_t3(b"cfg", b"static_tg", &sc.static_tg, &mut pol.static_tg))
        {
            return false;
        }
        if h & MN_HAS_REQUIRE_REG != 0 {
            cfg.reg_required = u8::from(sc.require_registration);
        }
        if h & MN_HAS_FOACSU != 0 {
            cfg.foacsu = u8::from(sc.foacsu);
        }
        let nt = (sc.ntimers.max(0) as usize).min(MN_CFG_TIMERS);
        for t in &sc.timers[..nt] {
            if !self.set_timer(b"cfg.timers", field(&t.name), t.value, cfg) {
                return false;
            }
        }
        self.check(cfg)
    }
}

/// One ID (a JSON number) into s; false if it isn't one.
fn add_id(s: &mut RdofIdset, v: f64, why: &mut [u8; 96]) -> bool {
    is_whole(v)
        && (1.0..=ID_MAX).contains(&v)
        && unsafe { rdof_idset_add_id(s, v as u32, why.as_mut_ptr().cast(), why.len()) }
}

// ── C API (rdof/t3cfg.h) ──────────────────────────────────────────────────────

#[no_mangle]
pub unsafe extern "C" fn rdof_t3cfg_default(cfg: *mut Cfg, pol: *mut Policy) {
    if !cfg.is_null() {
        core::ptr::write_bytes(cfg, 0, 1);
        *cfg = cfg_default();
    }
    if !pol.is_null() {
        core::ptr::write_bytes(pol, 0, 1);
        *pol = Policy::default_open();
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3cfg_apply(
    doc: *const JDoc,
    obj: c_int,
    path: *const c_char,
    cfg: *mut Cfg,
    pol: *mut Policy,
    err: *mut c_char,
    cap: usize,
) -> bool {
    Cx { doc: doc.as_ref(), err: err_buf(err, cap) }.apply(obj, c_bytes(path), &mut *cfg, &mut *pol)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3cfg_check(cfg: *const Cfg, err: *mut c_char, cap: usize) -> bool {
    Cx { doc: None, err: err_buf(err, cap) }.check(&*cfg)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3cfg_unit(p: *const RdofPlan) -> c_int {
    let p = Plan::new(p);
    (0..p.nunits()).find(|&k| p.unit(k).kind == UNIT_TRUNK).unwrap_or(-1)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3cfg_plan(
    p: *const RdofPlan,
    unit: c_int,
    cfg: *mut Cfg,
    pol: *mut Policy,
    err: *mut c_char,
    cap: usize,
) -> bool {
    let mut cx = Cx { doc: None, err: err_buf(err, cap) };
    let unit = if unit < 0 { rdof_t3cfg_unit(p) } else { unit };
    let p = Plan::new(p);
    if unit < 0 || unit >= p.nunits() || p.unit(unit).kind != UNIT_TRUNK {
        return cx.fail(&[S(b"no trunked site (an app of type tier3)")]);
    }
    let (cfg, pol) = (&mut *cfg, &mut *pol);
    let s = p.site;
    let app = p.unit(unit).app;
    let ap = &s.app[app as usize];
    rdof_t3cfg_default(cfg, pol);
    let mut pb = [0u8; SITE_NAME + 8];
    let path = join(&mut pb, b"apps.", field(&ap.name));
    cx.doc = Some(p.doc);
    if !cx.apply(p.app_node(app), path, cfg, pol) {
        return false;
    }
    cfg.carriers = [Carrier::default(); CARRIERS];
    cfg.ncarriers = ap.ncarrier;
    for (k, cr) in ap.carrier.iter().take(usize::from(ap.ncarrier)).enumerate() {
        cfg.carriers[k] = Carrier {
            tx_hz: s.freq[cr.out as usize].hz,
            rx_hz: s.freq[cr.in_ as usize].hz,
            chan: cr.chan,
            tscc: u8::from(k == 0),
            enabled: 1,
        };
    }
    cfg.cc = s.freq[ap.carrier[0].out as usize].cc as u8;
    if !cx.check(cfg) {
        let mut why = [0u8; 160];
        print(&mut why, &[S(field(cx.err))]);
        return cx.fail(&[S(path), S(b"."), S(field(&why))]);
    }
    true
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3cfg_site(
    sc: *const MnSiteCfg,
    cfg: *mut Cfg,
    pol: *mut Policy,
    err: *mut c_char,
    cap: usize,
) -> bool {
    Cx { doc: None, err: err_buf(err, cap) }.site(&*sc, &mut *cfg, &mut *pol)
}

// ── layouts (rdof_t3cfg_layout) ───────────────────────────────────────────────

pub use crate::t3site::Layout;

macro_rules! layout {
    ($name:literal, $t:ty) => {
        Layout {
            name: concat!($name, "\0").as_ptr().cast(),
            size: core::mem::size_of::<$t>(),
            align: core::mem::align_of::<$t>(),
        }
    };
}

static LAYOUTS: [Layout; 3] = [
    layout!("rdof_mn_timer_t", MnTimer),
    layout!("rdof_mn_site_cfg_t", MnSiteCfg),
    Layout { name: core::ptr::null(), size: 0, align: 0 },
];

#[no_mangle]
pub extern "C" fn rdof_t3cfg_layout(i: usize) -> *const Layout {
    LAYOUTS.get(i).map_or(core::ptr::null(), |l| l as *const Layout)
}
