// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! The bench API's radios and TX policy from the settings (rdof/benchcfg.h). A port of common/src/benchcfg.c.
//!
//! The settings text is untrusted. Numbers in strings go through the C library's strtol, so they read as the C's did.

use core::ffi::{c_char, c_int, c_long};
use core::mem::size_of;

use crate::benchtx::RdofBenchtxPolicy;
use crate::bits::slice_mut;
use crate::ffi::{calloc, free, strtol};
use crate::jsonp::{rdof_jat, rdof_jget, rdof_jnode, rdof_jparse, rdof_jsonc_strip, JDoc, JNode, RDOF_JARR, RDOF_JNULL, RDOF_JNUM, RDOF_JOBJ, RDOF_JSTR};
use crate::text::{c_bytes, print, Piece};

const MAX_NODES: usize = 1024;
const STR_BUF: usize = 8192;
const RDOF_BENCHTX_MAX_FREQS: usize = 8; // rdof/benchtx.h

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

/// The parsed settings: the doc and its buffers on the heap (about 64 KB), freed on drop.
struct Parsed {
    ctx: *mut Ctx,
    clean: *mut c_char,
}

#[repr(C)]
struct Ctx {
    doc: JDoc,
    nodes: [JNode; MAX_NODES],
    strs: [c_char; STR_BUF],
}

impl Drop for Parsed {
    fn drop(&mut self) {
        unsafe {
            free(self.ctx.cast());
            free(self.clean.cast());
        }
    }
}

impl Parsed {
    /// The settings parsed, or the error ("no memory", "JSON: ...").
    unsafe fn new(text: *const c_char, len: usize, e: &mut Error) -> Option<(Parsed, c_int)> {
        let p = Parsed { ctx: calloc(1, size_of::<Ctx>()).cast(), clean: calloc(len + 1, 1).cast() };
        if p.ctx.is_null() || p.clean.is_null() {
            e.set(&[Piece::S(b"no memory")]);
            return None;
        }
        let c = &mut *p.ctx;
        c.doc.nodes = c.nodes.as_mut_ptr();
        c.doc.max = MAX_NODES as c_int;
        c.doc.buf = c.strs.as_mut_ptr();
        c.doc.cap = STR_BUF;
        let root = rdof_jparse(&mut c.doc, p.clean, rdof_jsonc_strip(text, len, p.clean));
        if root < 0 {
            e.set(&[Piece::S(b"JSON: "), Piece::S(c_bytes(c.doc.error.as_ptr()))]);
            return None;
        }
        Some((p, root))
    }

    fn doc(&self) -> &JDoc {
        unsafe { &(*self.ctx).doc }
    }

    fn get(&self, obj: c_int, key: &core::ffi::CStr) -> c_int {
        unsafe { rdof_jget(self.doc(), obj, key.as_ptr()) }
    }

    /// Node i; None for -1 (or any node the parse didn't make).
    fn node(&self, i: c_int) -> Option<&JNode> {
        unsafe { rdof_jnode(self.doc(), i).as_ref() }
    }

    fn at(&self, obj: c_int, key: &core::ffi::CStr) -> Option<&JNode> {
        self.node(self.get(obj, key))
    }

    /// A key under "bench" in the root; -1 none.
    fn bench(&self, root: c_int, key: &core::ffi::CStr) -> c_int {
        let bench = self.get(root, c"bench");
        if bench >= 0 {
            self.get(bench, key)
        } else {
            -1
        }
    }
}

/// The error text, as the C's snprintf(error, cap, ...).
struct Error {
    out: *mut u8,
    cap: usize,
}

impl Error {
    /// -1, to return.
    fn set(&mut self, pieces: &[Piece]) -> c_int {
        print(unsafe { slice_mut(self.out, self.cap) }, pieces);
        -1
    }
}

/// A number, or a hex string for vid / pid; dflt when absent (or null). None: there but not one.
#[allow(clippy::useless_conversion)] // c_long is 32 bits on the S3
fn num(n: Option<&JNode>, hex: bool, dflt: i64) -> Option<i64> {
    let Some(n) = n.filter(|n| n.type_ != RDOF_JNULL) else {
        return Some(dflt);
    };
    match n.type_ {
        // saturating, so a huge number stays out of every range here (the C checks before its cast)
        RDOF_JNUM if n.integer => Some(n.num as i64),
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
    out.fill(0);
    let Some(n) = n.filter(|n| n.type_ != RDOF_JNULL) else {
        return true;
    };
    let s = unsafe { c_bytes(n.str_) };
    if n.type_ != RDOF_JSTR || s.len() >= out.len() {
        return false;
    }
    out[..s.len()].copy_from_slice(s);
    true
}

/// A NUL-terminated field's text.
fn field(f: &[u8]) -> &[u8] {
    &f[..f.iter().position(|&c| c == 0).unwrap_or(f.len())]
}

/// %04X
fn hex4(v: u16) -> [u8; 4] {
    const HEX: &[u8; 16] = b"0123456789ABCDEF";
    core::array::from_fn(|i| HEX[usize::from(v >> (12 - 4 * i) & 15)])
}

fn radio(p: &Parsed, node: c_int, i: c_int, r: &mut RdofBenchRadio, e: &mut Error) -> c_int {
    if p.node(node).is_none_or(|n| n.type_ != RDOF_JOBJ) {
        return e.set(&[Piece::S(b"bench.radios["), Piece::I(i.into()), Piece::S(b"]: an object")]);
    }
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
    if !string(p.at(node, c"name"), &mut r.name) || r.name[0] == 0 {
        return e.set(&[
            Piece::S(b"bench.radios["),
            Piece::I(i.into()),
            Piece::S(b"]: a name (up to "),
            Piece::I(r.name.len() as i64 - 1),
            Piece::S(b" characters)"),
        ]);
    }
    let name = r.name;
    let name = field(&name);
    // "bench radio <name>" and why
    let fail = |e: &mut Error, why: &[Piece]| {
        let mut pieces = [Piece::S(b""); 8];
        pieces[0] = Piece::S(b"bench radio ");
        pieces[1] = Piece::S(name);
        pieces[2..2 + why.len()].copy_from_slice(why);
        e.set(&pieces[..2 + why.len()])
    };
    if !name.iter().all(|&c| c.is_ascii_alphanumeric() || c == b'-' || c == b'_') {
        return fail(e, &[Piece::S(b": a name of letters, digits, - and _")]);
    }
    if !string(p.at(node, c"proto"), &mut r.proto) {
        return fail(e, &[Piece::S(b": proto odr-debug or mmdvm")]);
    }
    if r.proto[0] == 0 {
        r.proto[..9].copy_from_slice(b"odr-debug");
    }
    let proto = field(&r.proto);
    if proto != b"odr-debug" && proto != b"mmdvm" {
        return fail(e, &[Piece::S(b": proto "), Piece::S(proto), Piece::S(b": odr-debug or mmdvm")]);
    }
    let port = p.get(node, c"port");
    if p.node(port).is_none_or(|n| n.type_ != RDOF_JOBJ) {
        return fail(e, &[Piece::S(b": a port {\"uart\": ...} or {\"usb\": ...}")]);
    }
    let (uart, usb) = (p.at(port, c"uart"), p.at(port, c"usb"));
    if uart.is_some() == usb.is_some() {
        return fail(e, &[Piece::S(b": the port is a uart or usb, one of them")]);
    }
    match num(p.at(port, c"baud"), false, 115200) {
        Some(v @ 1200..=6000000) => r.baud = v as c_long,
        _ => return fail(e, &[Piece::S(b": baud 1200 to 6000000")]),
    }
    if uart.is_some() {
        r.port = RDOF_BPORT_UART;
        match num(uart, false, 0) {
            Some(v @ 1..=2) => r.uart = v as c_int,
            _ => return fail(e, &[Piece::S(b": uart 1 or 2 (UART0 is the console)")]),
        }
        const PINS: [(&core::ffi::CStr, &[u8]); 4] = [(c"tx", b"tx"), (c"rx", b"rx"), (c"rts", b"rts"), (c"cts", b"cts")];
        let mut pins = [0; 4];
        for (k, (key, pin)) in PINS.iter().enumerate() {
            match num(p.at(port, key), false, -1) {
                Some(v) if (-1..=48).contains(&v) && (k >= 2 || v >= 0) => pins[k] = v as c_int,
                _ => {
                    let none: &[u8] = if k < 2 { b"" } else { b" (or none)" };
                    return fail(e, &[Piece::S(b": "), Piece::S(pin), Piece::S(b", a GPIO 0 to 48"), Piece::S(none)]);
                }
            }
        }
        [r.tx, r.rx, r.rts, r.cts] = pins;
        for a in 0..4 {
            for b in a + 1..4 {
                if pins[a] >= 0 && pins[a] == pins[b] {
                    let (pa, pb) = (PINS[a].1, PINS[b].1);
                    return fail(e, &[Piece::S(b": "), Piece::S(pa), Piece::S(b" and "), Piece::S(pb), Piece::S(b" on the same GPIO")]);
                }
            }
        }
        return 0;
    }
    let mut kind = [0u8; 12];
    if !string(usb, &mut kind) || !matches!(field(&kind), b"cdc-acm" | b"uart") {
        return fail(e, &[Piece::S(b": usb cdc-acm or uart")]);
    }
    r.port = if field(&kind) == b"uart" { RDOF_BPORT_USB_UART } else { RDOF_BPORT_CDC_ACM };
    match num(p.at(port, c"vid"), true, 0) {
        Some(v @ 0..=0xFFFF) => r.vid = v as u16,
        _ => return fail(e, &[Piece::S(b": vid, four hex digits")]),
    }
    match num(p.at(port, c"pid"), true, 0) {
        Some(v @ 0..=0xFFFF) => r.pid = v as u16,
        _ => return fail(e, &[Piece::S(b": pid, four hex digits")]),
    }
    if r.pid != 0 && r.vid == 0 {
        return fail(e, &[Piece::S(b": a pid needs its vid")]);
    }
    if r.port == RDOF_BPORT_CDC_ACM && (r.vid == 0 || r.pid == 0) {
        return fail(e, &[Piece::S(b": a cdc-acm radio needs its vid and pid")]);
    }
    let (vid, pid) = (hex4(r.vid), hex4(r.pid));
    if r.vid == 0x0BDA {
        return fail(e, &[Piece::S(b": "), Piece::S(&vid), Piece::S(b" is the RTL-SDR's vendor")]);
    }
    if r.port == RDOF_BPORT_USB_UART && r.vid != 0 && r.pid != 0 && rdof_bench_usb_bridge(r.vid, r.pid) == 0 {
        return fail(
            e,
            &[
                Piece::S(b": "),
                Piece::S(&vid),
                Piece::S(b":"),
                Piece::S(&pid),
                Piece::S(b" isn't a USB-serial bridge this firmware knows (FTDI, CP210x, CH34x)"),
            ],
        );
    }
    if !string(p.at(port, c"serial"), &mut r.serial) || !string(p.at(port, c"usb_port"), &mut r.usb_port) {
        return fail(e, &[Piece::S(b": serial and usb_port are strings")]);
    }
    0
}

/// The caller's error buffer, or a few bytes of our own when it gives none; emptied.
fn error_out(error: *mut c_char, cap: usize, dummy: &mut [u8; 8]) -> Error {
    let e = if error.is_null() { Error { out: dummy.as_mut_ptr(), cap: dummy.len() } } else { Error { out: error.cast(), cap } };
    if let Some(b) = unsafe { slice_mut(e.out, e.cap) }.first_mut() {
        *b = 0;
    }
    e
}

#[no_mangle]
pub unsafe extern "C" fn rdof_bench_radios_parse(
    text: *const c_char,
    len: usize,
    out: *mut RdofBenchRadio,
    max: c_int,
    error: *mut c_char,
    cap: usize,
) -> c_int {
    let mut dummy = [0u8; 8];
    let e = &mut error_out(error, cap, &mut dummy);
    let Some((p, root)) = Parsed::new(text, len, e) else {
        return -1;
    };
    let radios = p.bench(root, c"radios");
    let Some(rn) = p.node(radios).filter(|n| n.type_ != RDOF_JNULL) else {
        return 0;
    };
    if rn.type_ != RDOF_JARR {
        return e.set(&[Piece::S(b"bench.radios: a list")]);
    }
    let count = rn.count;
    if count > max {
        return e.set(&[Piece::S(b"bench.radios: at most "), Piece::I(max.into())]);
    }
    let out = if count > 0 { core::slice::from_raw_parts_mut(out, count as usize) } else { &mut [] };
    for i in 0..out.len() {
        if radio(&p, rdof_jat(p.doc(), radios, i as c_int), i as c_int, &mut out[i], e) < 0 {
            return -1;
        }
        let (done, rest) = out.split_at(i);
        let r = &rest[0];
        for k in done {
            if k.name == r.name {
                return e.set(&[Piece::S(b"bench radios: two named "), Piece::S(field(&r.name))]);
            }
            if k.port == RDOF_BPORT_UART && r.port == RDOF_BPORT_UART && k.uart == r.uart {
                return e.set(&[
                    Piece::S(b"bench radios "),
                    Piece::S(field(&k.name)),
                    Piece::S(b" and "),
                    Piece::S(field(&r.name)),
                    Piece::S(b": both on UART"),
                    Piece::I(r.uart.into()),
                ]);
            }
        }
    }
    count
}

#[no_mangle]
pub unsafe extern "C" fn rdof_bench_tx_parse(
    text: *const c_char,
    len: usize,
    out: &mut RdofBenchtxPolicy,
    error: *mut c_char,
    cap: usize,
) -> c_int {
    *out = RdofBenchtxPolicy::NONE;
    let mut dummy = [0u8; 8];
    let e = &mut error_out(error, cap, &mut dummy);
    let rc = tx_policy(text, len, out, e);
    if rc < 0 {
        *out = RdofBenchtxPolicy::NONE; // no TX, not half a policy
    }
    rc
}

unsafe fn tx_policy(text: *const c_char, len: usize, out: &mut RdofBenchtxPolicy, e: &mut Error) -> c_int {
    let Some((p, root)) = Parsed::new(text, len, e) else {
        return -1;
    };
    let tx = p.bench(root, c"tx");
    let Some(t) = p.node(tx).filter(|n| n.type_ != RDOF_JNULL) else {
        return 0;
    };
    if t.type_ != RDOF_JOBJ {
        return e.set(&[Piece::S(b"bench.tx: an object")]);
    }
    let fl = p.get(tx, c"freqs_hz");
    if let Some(f) = p.node(fl).filter(|n| n.type_ != RDOF_JNULL) {
        if f.type_ != RDOF_JARR {
            return e.set(&[Piece::S(b"bench.tx.freqs_hz: a list of frequencies in Hz")]);
        }
        for i in 0..f.count {
            if i as usize >= RDOF_BENCHTX_MAX_FREQS {
                return e.set(&[Piece::S(b"bench.tx.freqs_hz: at most "), Piece::I(RDOF_BENCHTX_MAX_FREQS as i64)]);
            }
            let v = p.node(rdof_jat(p.doc(), fl, i));
            match v {
                Some(v) if v.type_ == RDOF_JNUM && (30e6..=3e9).contains(&v.num) && v.num == f64::from(v.num as u32) => {
                    out.freqs_hz[out.nfreqs as usize] = v.num as u32;
                    out.nfreqs += 1;
                }
                _ => {
                    return e.set(&[Piece::S(b"bench.tx.freqs_hz["), Piece::I(i.into()), Piece::S(b"]: a frequency in whole Hz")])
                }
            }
        }
    }
    out.max_key_s = match p.at(tx, c"max_key_s").filter(|k| k.type_ != RDOF_JNULL) {
        None => 10.0,
        Some(k) if k.type_ == RDOF_JNUM && (0.0..=60.0).contains(&k.num) => k.num,
        Some(_) => return e.set(&[Piece::S(b"bench.tx.max_key_s: 0 to 60 seconds")]),
    };
    match num(p.at(tx, c"rf_level"), false, 0) {
        Some(v @ 0..=255) => out.rf_level = v as c_int,
        _ => return e.set(&[Piece::S(b"bench.tx.rf_level: 0 to 255 (0: the lowest)")]),
    }
    match num(p.at(tx, c"arm_gpio"), false, -1) {
        Some(v @ -1..=48) => out.arm_gpio = v as c_int,
        _ => return e.set(&[Piece::S(b"bench.tx.arm_gpio: a GPIO 0 to 48 (a jumper to ground), or none")]),
    }
    0
}
