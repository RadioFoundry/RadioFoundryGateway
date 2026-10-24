// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! The JSON writer (rdof/json.h): into a fixed buffer, compact (no spaces after , and
//! :), for what the gateway sends. A port of common/src/json.c. Floats go through the C library's snprintf ("%.3f",
//! correctly rounded as the C's was); everything else is written here.

use core::ffi::{c_char, c_int, c_longlong, CStr};

use crate::bits::slice_mut;
use crate::ffi;

pub const RDOF_JSON_DEPTH: usize = 8;

/// rdof_json_t
#[repr(C)]
pub struct RdofJson {
    pub buf: *mut c_char,
    pub cap: usize,
    pub len: usize,
    pub overflow: bool,
    pub depth: c_int,
    pub first: [bool; RDOF_JSON_DEPTH],
}

impl RdofJson {
    /// Appends s and a NUL, or marks the writer overflowed (and writes nothing more).
    fn put(&mut self, s: &[u8]) {
        if self.overflow {
            return;
        }
        if self.len + s.len() + 1 > self.cap {
            self.overflow = true;
            return;
        }
        let buf = unsafe { slice_mut(self.buf.cast(), self.cap) };
        buf[self.len..self.len + s.len()].copy_from_slice(s);
        self.len += s.len();
        buf[self.len] = 0;
    }

    /// ASCII only: control characters and everything past ASCII as \uXXXX, surrogate
    /// pairs past U+FFFF.
    fn quoted(&mut self, s: &[u8]) {
        self.put(b"\"");
        let mut i = 0;
        while i < s.len() {
            let c = s[i];
            if c == b'"' || c == b'\\' {
                self.put(&[b'\\', c]);
                i += 1;
            } else if !(0x20..0x80).contains(&c) {
                let (cp, n) = if c < 0x80 { (u32::from(c), 1) } else { utf8(&s[i..]) };
                if cp >= 0x10000 {
                    self.put(&u_escape(0xD800 + ((cp - 0x10000) >> 10)));
                    self.put(&u_escape(0xDC00 + ((cp - 0x10000) & 0x3FF)));
                } else {
                    self.put(&u_escape(cp));
                }
                i += n;
            } else {
                self.put(&[c]);
                i += 1;
            }
        }
        self.put(b"\"");
    }

    fn key(&mut self, k: *const c_char) {
        // past RDOF_JSON_DEPTH the writer has overflowed (rdof_json_open): nothing to track
        let d = self.depth as usize;
        if self.depth > 0 && d <= RDOF_JSON_DEPTH {
            if !self.first[d - 1] {
                self.put(b",");
            }
            self.first[d - 1] = false;
        }
        if !k.is_null() {
            self.quoted(unsafe { CStr::from_ptr(k) }.to_bytes());
            self.put(b":");
        }
    }
}

/// The writer for Rust callers: keys as &CStr, values as bytes.
impl RdofJson {
    /// rdof_json_init's.
    pub unsafe fn new(buf: *mut c_char, cap: usize) -> Self {
        let mut j = RdofJson { buf, cap, len: 0, overflow: false, depth: 0, first: [false; RDOF_JSON_DEPTH] };
        rdof_json_init(&mut j, buf, cap);
        j
    }

    pub fn open(&mut self, k: Option<&CStr>) {
        unsafe { rdof_json_open(self, k.map_or(core::ptr::null(), CStr::as_ptr)) }
    }

    pub fn close(&mut self) {
        rdof_json_close(self)
    }

    pub fn str(&mut self, k: &CStr, v: &[u8]) {
        self.key(k.as_ptr());
        self.quoted(v);
    }

    pub fn int(&mut self, k: &CStr, v: i64) {
        unsafe { rdof_json_int(self, k.as_ptr(), v) }
    }

    pub fn bool(&mut self, k: &CStr, v: bool) {
        unsafe { rdof_json_bool(self, k.as_ptr(), v) }
    }

    /// v as it is (a number already formatted).
    pub fn raw(&mut self, k: &CStr, v: &[u8]) {
        self.key(k.as_ptr());
        self.put(v);
    }

    pub fn float3(&mut self, k: &CStr, v: f64) {
        unsafe { rdof_json_float3(self, k.as_ptr(), v) }
    }

    pub fn done(&self) -> bool {
        !self.overflow
    }
}

/// \uXXXX, lowercase.
fn u_escape(v: u32) -> [u8; 6] {
    let hex = |k: u32| b"0123456789abcdef"[((v >> k) & 15) as usize];
    [b'\\', b'u', hex(12), hex(8), hex(4), hex(0)]
}

/// One UTF-8 character at s[0]: its code point and length, or U+FFFD and 1 for a byte that doesn't start a valid one.
fn utf8(s: &[u8]) -> (u32, usize) {
    const MIN: [u32; 4] = [0, 0x80, 0x800, 0x10000];
    let n = match s[0] {
        0xF0..=0xF4 => 3,
        0xE0..=0xEF => 2,
        0xC2..=0xDF => 1,
        _ => return (0xFFFD, 1),
    };
    let mut cp = u32::from(s[0]) & (0x3F >> n);
    for k in 1..=n {
        match s.get(k) {
            Some(&b) if b & 0xC0 == 0x80 => cp = cp << 6 | u32::from(b & 0x3F),
            _ => return (0xFFFD, 1),
        }
    }
    if cp < MIN[n] || cp > 0x10FFFF || (0xD800..0xE000).contains(&cp) {
        return (0xFFFD, 1);
    }
    (cp, n + 1)
}

/// A C string's bytes.
unsafe fn c_bytes<'a>(s: *const c_char) -> &'a [u8] {
    CStr::from_ptr(s).to_bytes()
}

#[no_mangle]
pub unsafe extern "C" fn rdof_json_init(j: &mut RdofJson, buf: *mut c_char, cap: usize) {
    j.buf = buf;
    j.cap = cap;
    j.len = 0;
    j.overflow = cap == 0;
    j.depth = 0;
    if cap > 0 {
        *buf = 0;
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_json_open(j: &mut RdofJson, k: *const c_char) {
    j.key(k);
    j.put(b"{");
    match j.first.get_mut(j.depth as usize) {
        Some(f) if j.depth >= 0 => *f = true,
        // deeper than it can track
        _ => j.overflow = true,
    }
    j.depth += 1;
}

#[no_mangle]
pub extern "C" fn rdof_json_close(j: &mut RdofJson) {
    j.put(b"}");
    if j.depth > 0 {
        j.depth -= 1;
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_json_str(j: &mut RdofJson, k: *const c_char, v: *const c_char) {
    j.key(k);
    j.quoted(c_bytes(v));
}

#[no_mangle]
pub unsafe extern "C" fn rdof_json_int(j: &mut RdofJson, k: *const c_char, v: c_longlong) {
    let mut digits = [0u8; 20];
    let mut i = digits.len();
    let mut u = v.unsigned_abs();
    loop {
        i -= 1;
        digits[i] = b'0' + (u % 10) as u8;
        u /= 10;
        if u == 0 {
            break;
        }
    }
    j.key(k);
    if v < 0 {
        j.put(b"-");
    }
    j.put(&digits[i..]);
}

#[no_mangle]
pub unsafe extern "C" fn rdof_json_raw(j: &mut RdofJson, k: *const c_char, text: *const c_char) {
    j.key(k);
    j.put(c_bytes(text));
}

#[no_mangle]
pub unsafe extern "C" fn rdof_json_bool(j: &mut RdofJson, k: *const c_char, v: bool) {
    j.key(k);
    j.put(if v { b"true" } else { b"false" });
}

#[no_mangle]
pub unsafe extern "C" fn rdof_format_float3(v: f64, out: *mut c_char, cap: usize) {
    ffi::snprintf(out, cap, c"%.3f".as_ptr(), v);
    let s = slice_mut(out.cast(), cap);
    let mut n = s.iter().position(|&c| c == 0).unwrap_or(0);
    while n > 2 && s[n - 1] == b'0' && s[n - 2] != b'.' {
        n -= 1;
        s[n] = 0;
    }
    if &s[..n] == b"-0.0" {
        ffi::snprintf(out, cap, c"-0.0".as_ptr());
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_json_float3(j: &mut RdofJson, k: *const c_char, v: f64) {
    let mut s = [0 as c_char; 40];
    rdof_format_float3(v, s.as_mut_ptr(), s.len());
    j.key(k);
    j.put(c_bytes(s.as_ptr()));
}

#[no_mangle]
pub extern "C" fn rdof_json_done(j: &RdofJson) -> bool {
    !j.overflow
}

#[no_mangle]
pub unsafe extern "C" fn rdof_base64(input: *const u8, n: usize, out: *mut c_char) {
    const TBL: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let input = crate::bits::slice(input, n);
    let out = slice_mut(out.cast(), n.div_ceil(3) * 4 + 1);
    for (i, chunk) in input.chunks(3).enumerate() {
        let v = chunk.iter().enumerate().fold(0u32, |v, (i, &b)| v | u32::from(b) << (16 - 8 * i));
        let c = |k: u32| TBL[((v >> k) & 63) as usize];
        out[i * 4..i * 4 + 4].copy_from_slice(&[
            c(18),
            c(12),
            if chunk.len() > 1 { c(6) } else { b'=' },
            if chunk.len() > 2 { c(0) } else { b'=' },
        ]);
    }
    out[n.div_ceil(3) * 4] = 0;
}
