// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! The bench API's radios and TX policy from the settings (rdof/benchcfg.h). A port of common/src/benchcfg.c: it
//! reads the version 3 plan (rdof/siteplan.h, C) as the C does.
//!
//! The settings text is untrusted. Numbers in strings go through the C library's strtol, so they read as the C's did.

use core::ffi::{c_char, c_int, c_long, CStr};

use crate::benchtx::RdofBenchtxPolicy;
use crate::bits::slice_mut;
use crate::ffi::strtol;
use crate::jsonp::{rdof_jat, rdof_jget, rdof_jnode, JNode, RDOF_JARR, RDOF_JNULL, RDOF_JNUM, RDOF_JOBJ, RDOF_JSTR};
use crate::plan::{field, rdof_plan_close, rdof_plan_open, Plan, RdofPlan, DEV_RADIO, DEV_SDR, PORT_PATH};
use crate::text::{c_bytes, print, Piece, Piece::I, Piece::S};

const RDOF_BENCHTX_MAX_FREQS: usize = 8; // rdof/benchtx.h
const MAX_DEVICES: usize = 16; // RDOF_SITE_MAX_DEVICES

const RDOF_BPORT_UART: c_int = 1;
const RDOF_BPORT_CDC_ACM: c_int = 2;
const RDOF_BPORT_USB_UART: c_int = 3;

/// rdof_bench_radio_t
#[repr(C)]
pub struct RdofBenchRadio {
    pub name: [u8; 24],
    pub proto: [u8; 12],
    pub port: c_int,
    pub uart: c_int,
    pub tx: c_int,
    pub rx: c_int,
    pub rts: c_int,
    pub cts: c_int,
    pub baud: c_long,
    pub vid: u16,
    pub pid: u16,
    pub serial: [u8; 32],
    pub usb_port: [u8; 16],
}

#[no_mangle]
pub extern "C" fn rdof_bench_usb_bridge(vid: u16, pid: u16) -> c_char {
    (match (vid, pid) {
        (0x0403, _) => b'f',                         // FTDI FT232R, FT2232, FT4232, FT232H, FT-X
        (0x10C4, 0xEA60 | 0xEA70 | 0xEA71) => b'c', // Silicon Labs CP210x
        (0x1A86, 0x7523 | 0x7522 | 0x5523) => b'h', // WCH CH340 / CH341 (the CH343 and CH9102 are CDC-ACM)
        _ => 0,
    }) as c_char
}

/// The plan's document and where errors go.
struct Cx<'a> {
    plan: Plan<'a>,
    out: *mut u8,
    cap: usize,
}

impl Cx<'_> {
    /// snprintf(error, cap, ...); -1, to return.
    fn fail(&self, pieces: &[Piece]) -> c_int {
        print(unsafe { slice_mut(self.out, self.cap) }, pieces);
        -1
    }

    fn get(&self, obj: c_int, key: &CStr) -> c_int {
        unsafe { rdof_jget(self.plan.doc, obj, key.as_ptr()) }
    }

    /// Node i; None for -1.
    fn node(&self, i: c_int) -> Option<&JNode> {
        unsafe { rdof_jnode(self.plan.doc, i).as_ref() }
    }

    fn at(&self, obj: c_int, key: &CStr) -> Option<&JNode> {
        self.node(self.get(obj, key))
    }
}

/// The caller's error buffer, or a few bytes of our own when it gives none; emptied (as the C, even at cap 0).
unsafe fn error_out(error: *mut c_char, cap: usize, dummy: &mut [u8; 8]) -> (*mut u8, usize) {
    let (out, cap) = if error.is_null() { (dummy.as_mut_ptr(), dummy.len()) } else { (error.cast(), cap) };
    *out = 0;
    (out, cap)
}

/// A number, or a hex string for vid / pid; dflt when absent (or null). None: there but not one.
#[allow(clippy::useless_conversion)] // c_long is 32 bits on the S3
fn num(n: Option<&JNode>, hex: bool, dflt: i64) -> Option<i64> {
    let Some(n) = n.filter(|n| n.type_ != RDOF_JNULL) else {
        return Some(dflt);
    };
    match n.type_ {
        // past any value here, and (long) of it undefined in C where long is 32 bits
        RDOF_JNUM if n.integer => (-2147483648.0..=2147483647.0).contains(&n.num).then_some(n.num as i64),
        RDOF_JSTR if !unsafe { c_bytes(n.str_) }.is_empty() => {
            let mut end = core::ptr::null_mut();
            let v = unsafe { strtol(n.str_, &mut end, if hex { 16 } else { 10 }) };
            (unsafe { *end } == 0).then_some(i64::from(v))
        }
        _ => None,
    }
}

/// A string that fits `out` with its NUL ("" when absent or null); false: there but not one, or too long.
fn string(n: Option<&JNode>, out: &mut [u8]) -> bool {
    out[0] = 0;
    let Some(n) = n.filter(|n| n.type_ != RDOF_JNULL) else {
        return true;
    };
    let s = unsafe { c_bytes(n.str_) };
    if n.type_ != RDOF_JSTR || s.len() >= out.len() {
        return false;
    }
    out[..s.len()].copy_from_slice(s);
    out[s.len()] = 0;
    true
}

/// %04X
fn hex4(v: u16) -> [u8; 4] {
    const HEX: &[u8; 16] = b"0123456789ABCDEF";
    core::array::from_fn(|i| HEX[usize::from(v >> (12 - 4 * i) & 15)])
}

/// r's port (its name and proto set).
fn radio_port(c: &Cx, port: c_int, r: &mut RdofBenchRadio) -> c_int {
    let name = r.name;
    let name = field(&name);
    // "devices.<name>.port: " and why
    let fail = |why: &[Piece]| {
        let mut pieces = [S(b""); 9];
        pieces[..3].copy_from_slice(&[S(b"devices."), S(name), S(b".port: ")]);
        pieces[3..3 + why.len()].copy_from_slice(why);
        c.fail(&pieces[..3 + why.len()])
    };
    if port < 0 || c.node(port).is_none_or(|n| n.type_ != RDOF_JOBJ) {
        return fail(&[S(b"a port {\"uart\": ...} or {\"usb\": ...}")]);
    }
    let (uart, usb) = (c.at(port, c"uart"), c.at(port, c"usb"));
    if uart.is_some() == usb.is_some() {
        return fail(&[S(b"the port is a uart or usb, one of them")]);
    }
    match num(c.at(port, c"baud"), false, 115200) {
        Some(v @ 1200..=6000000) => r.baud = v as c_long,
        _ => return fail(&[S(b"baud 1200 to 6000000")]),
    }
    if uart.is_some() {
        r.port = RDOF_BPORT_UART;
        match num(uart, false, 0) {
            Some(v @ 1..=2) => r.uart = v as c_int,
            _ => return fail(&[S(b"uart 1 or 2 (UART0 is the console)")]),
        }
        const PINS: [(&CStr, &[u8]); 4] = [(c"tx", b"tx"), (c"rx", b"rx"), (c"rts", b"rts"), (c"cts", b"cts")];
        let mut pins = [0; 4];
        for (k, (key, pin)) in PINS.iter().enumerate() {
            match num(c.at(port, key), false, -1) {
                Some(v) if (-1..=48).contains(&v) && (k >= 2 || v >= 0) => pins[k] = v as c_int,
                _ => {
                    let none: &[u8] = if k < 2 { b"" } else { b" (or none)" };
                    return fail(&[S(pin), S(b", a GPIO 0 to 48"), S(none)]);
                }
            }
            *[&mut r.tx, &mut r.rx, &mut r.rts, &mut r.cts][k] = pins[k];
        }
        for a in 0..4 {
            for b in a + 1..4 {
                if pins[a] >= 0 && pins[a] == pins[b] {
                    return fail(&[S(PINS[a].1), S(b" and "), S(PINS[b].1), S(b" on the same GPIO")]);
                }
            }
        }
        return 0;
    }
    let mut kind = [0u8; 12];
    if !string(usb, &mut kind) || !matches!(field(&kind), b"cdc-acm" | b"uart") {
        return fail(&[S(b"usb cdc-acm or uart")]);
    }
    r.port = if field(&kind) == b"uart" { RDOF_BPORT_USB_UART } else { RDOF_BPORT_CDC_ACM };
    match num(c.at(port, c"vid"), true, 0) {
        Some(v @ 0..=0xFFFF) => r.vid = v as u16,
        _ => return fail(&[S(b"vid, four hex digits")]),
    }
    match num(c.at(port, c"pid"), true, 0) {
        Some(v @ 0..=0xFFFF) => r.pid = v as u16,
        _ => return fail(&[S(b"pid, four hex digits")]),
    }
    if r.pid != 0 && r.vid == 0 {
        return fail(&[S(b"a pid needs its vid")]);
    }
    if r.port == RDOF_BPORT_CDC_ACM && (r.vid == 0 || r.pid == 0) {
        return fail(&[S(b"a cdc-acm radio needs its vid and pid")]);
    }
    let (vid, pid) = (hex4(r.vid), hex4(r.pid));
    if r.vid == 0x0BDA {
        return fail(&[S(&vid), S(b" is the RTL-SDR's vendor")]);
    }
    if r.port == RDOF_BPORT_USB_UART && r.vid != 0 && r.pid != 0 && rdof_bench_usb_bridge(r.vid, r.pid) == 0 {
        return fail(&[
            S(&vid),
            S(b":"),
            S(&pid),
            S(b" isn't a USB-serial bridge this firmware knows (FTDI, CP210x, CH34x)"),
        ]);
    }
    if !string(c.at(port, c"serial"), &mut r.serial) || !string(c.at(port, c"usb_port"), &mut r.usb_port) {
        return fail(&[S(b"serial and usb_port are strings")]);
    }
    0
}

/// Two radios with one name, or on one UART: refused (done checked, r new).
fn radio_apart(c: &Cx, done: &[RdofBenchRadio], r: &RdofBenchRadio) -> c_int {
    for k in done {
        if field(&k.name) == field(&r.name) {
            return c.fail(&[S(b"bench radios: two named "), S(field(&r.name))]);
        }
        if k.port == RDOF_BPORT_UART && r.port == RDOF_BPORT_UART && k.uart == r.uart {
            return c.fail(&[
                S(b"bench radios "),
                S(field(&k.name)),
                S(b" and "),
                S(field(&r.name)),
                S(b": both on UART"),
                I(r.uart.into()),
            ]);
        }
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn rdof_plan_bench_radios(
    p: *const RdofPlan,
    out: *mut RdofBenchRadio,
    max: c_int,
    error: *mut c_char,
    cap: usize,
) -> c_int {
    let mut dummy = [0u8; 8];
    let (eo, ecap) = error_out(error, cap, &mut dummy);
    let c = Cx { plan: Plan::new(p), out: eo, cap: ecap };
    let s = c.plan.site;
    let mut n: c_int = 0;
    for k in 0..c.plan.nbench() {
        let d = c.plan.bench(k);
        let dv = &s.dev[d as usize];
        if dv.kind == DEV_SDR {
            continue;
        }
        if n >= max {
            return c.fail(&[S(b"bench.devices: "), I(max.into()), S(b" radios and modems at most")]);
        }
        let all = core::slice::from_raw_parts_mut(out, n as usize + 1);
        let (done, rest) = all.split_at_mut(n as usize);
        let r = &mut rest[0];
        *r = RdofBenchRadio {
            name: [0; 24],
            proto: [0; 12],
            port: 0,
            uart: 0,
            tx: 0,
            rx: 0,
            rts: -1,
            cts: -1,
            baud: 0,
            vid: 0,
            pid: 0,
            serial: [0; 32],
            usb_port: [0; 16],
        };
        let dn = field(&dv.name);
        if dn.len() >= r.name.len() {
            return c.fail(&[
                S(b"bench.devices["),
                I(k.into()),
                S(b"]: a name up to "),
                I(r.name.len() as i64 - 1),
                S(b" characters"),
            ]);
        }
        r.name[..dn.len()].copy_from_slice(dn);
        if !dn.iter().all(|&ch| ch.is_ascii_alphanumeric() || ch == b'-' || ch == b'_') {
            return c.fail(&[S(b"bench.devices["), I(k.into()), S(b"]: "), S(dn), S(b": a name of letters, digits, - and _")]);
        }
        let proto: &[u8] = if dv.kind == DEV_RADIO { b"odr-debug" } else { b"mmdvm" };
        r.proto[..proto.len()].copy_from_slice(proto);
        if c.plan.dev(d).port.kind == PORT_PATH {
            return c.fail(&[S(b"devices."), S(dn), S(b".port: a bench device is on a UART or USB, not a path")]);
        }
        if radio_port(&c, c.get(c.plan.dev_node(d), c"port"), r) < 0 || radio_apart(&c, done, r) < 0 {
            return -1;
        }
        n += 1;
    }
    n
}

/// bench.tx: tx's freqs_hz, then more (each not there already), and its other keys.
fn policy(c: &Cx, tx: c_int, more: &[u32], out: &mut RdofBenchtxPolicy) -> c_int {
    let t = c.node(tx).filter(|t| t.type_ != RDOF_JNULL);
    if t.is_none() && more.is_empty() {
        return 0;
    }
    if t.is_some_and(|t| t.type_ != RDOF_JOBJ) {
        return c.fail(&[S(b"bench.tx: an object")]);
    }
    let fl = if t.is_some() { c.get(tx, c"freqs_hz") } else { -1 };
    let f = c.node(fl).filter(|f| f.type_ != RDOF_JNULL); // null: none, as no freqs_hz
    if f.is_some_and(|f| f.type_ != RDOF_JARR) {
        return c.fail(&[S(b"bench.tx.freqs_hz: a list of frequencies in Hz")]);
    }
    for i in 0..f.map_or(0, |f| f.count) {
        if i as usize >= RDOF_BENCHTX_MAX_FREQS {
            return c.fail(&[S(b"bench.tx.freqs_hz: at most "), I(RDOF_BENCHTX_MAX_FREQS as i64)]);
        }
        match c.node(unsafe { rdof_jat(c.plan.doc, fl, i) }) {
            Some(v) if v.type_ == RDOF_JNUM && !(v.num < 30e6 || v.num > 3e9) && v.num == f64::from(v.num as u32) => {
                out.freqs_hz[out.nfreqs as usize] = v.num as u32;
                out.nfreqs += 1;
            }
            _ => return c.fail(&[S(b"bench.tx.freqs_hz["), I(i.into()), S(b"]: a frequency in whole Hz")]),
        }
    }
    for &m in more {
        if out.freqs_hz[..out.nfreqs as usize].contains(&m) {
            continue;
        }
        if out.nfreqs as usize >= RDOF_BENCHTX_MAX_FREQS {
            return c.fail(&[
                S(b"bench.tx.freqs_hz and the bench devices' freqs_hz: at most "),
                I(RDOF_BENCHTX_MAX_FREQS as i64),
            ]);
        }
        out.freqs_hz[out.nfreqs as usize] = m;
        out.nfreqs += 1;
    }
    // null: the default, as null is for the others
    let k = if t.is_some() { c.at(tx, c"max_key_s") } else { None }.filter(|k| k.type_ != RDOF_JNULL);
    if k.is_some_and(|k| k.type_ != RDOF_JNUM || k.num < 0.0 || k.num > 60.0) {
        return c.fail(&[S(b"bench.tx.max_key_s: 0 to 60 seconds")]);
    }
    out.max_key_s = k.map_or(10.0, |k| k.num);
    match num(if t.is_some() { c.at(tx, c"rf_level") } else { None }, false, 0) {
        Some(v @ 0..=255) => out.rf_level = v as c_int,
        _ => return c.fail(&[S(b"bench.tx.rf_level: 0 to 255 (0: the lowest)")]),
    }
    match num(if t.is_some() { c.at(tx, c"arm_gpio") } else { None }, false, -1) {
        Some(v @ -1..=48) => out.arm_gpio = v as c_int,
        _ => return c.fail(&[S(b"bench.tx.arm_gpio: a GPIO 0 to 48 (a jumper to ground), or none")]),
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn rdof_plan_bench_tx(
    p: *const RdofPlan,
    out: &mut RdofBenchtxPolicy,
    error: *mut c_char,
    cap: usize,
) -> c_int {
    *out = RdofBenchtxPolicy::NONE;
    let mut dummy = [0u8; 8];
    let (eo, ecap) = error_out(error, cap, &mut dummy);
    let c = Cx { plan: Plan::new(p), out: eo, cap: ecap };
    let bench = c.get(c.plan.root(), c"bench");
    if bench < 0 {
        return 0;
    }
    // the bench devices' own frequencies, after bench.tx's
    let mut more = [0u32; MAX_DEVICES * 8];
    let mut nmore = 0;
    for k in 0..c.plan.nbench() {
        let b = &c.plan.dev(c.plan.bench(k)).bench;
        for &f in &b.freqs_hz[..b.nfreqs as usize] {
            more[nmore] = f;
            nmore += 1;
        }
    }
    let rc = policy(&c, c.get(bench, c"tx"), &more[..nmore], out);
    if rc < 0 {
        *out = RdofBenchtxPolicy::NONE; // no TX, not half a policy
    }
    rc
}

// The text forms: a plan each.

#[no_mangle]
pub unsafe extern "C" fn rdof_bench_radios_parse(
    text: *const c_char,
    len: usize,
    out: *mut RdofBenchRadio,
    max: c_int,
    error: *mut c_char,
    cap: usize,
) -> c_int {
    let p = rdof_plan_open(text, len, error, cap);
    if p.is_null() {
        return -1;
    }
    let n = rdof_plan_bench_radios(p, out, max, error, cap);
    rdof_plan_close(p);
    n
}

#[no_mangle]
pub unsafe extern "C" fn rdof_bench_tx_parse(
    text: *const c_char,
    len: usize,
    out: &mut RdofBenchtxPolicy,
    error: *mut c_char,
    cap: usize,
) -> c_int {
    let p = rdof_plan_open(text, len, error, cap);
    if p.is_null() {
        *out = RdofBenchtxPolicy::NONE;
        return -1;
    }
    let rc = rdof_plan_bench_tx(p, out, error, cap);
    rdof_plan_close(p);
    rc
}
