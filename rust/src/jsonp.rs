// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! The JSON reader (rdof/jsonp.h): parses into the caller's node array and string buffer, no allocation. A port of
//! common/src/jsonp.c. Also the JSON-with-comments text helpers (rdof_jsonc_*), which work on the stored settings.
//!
//! The text is untrusted (config files, HTTP bodies) and needn't be NUL terminated: everything here reads it as a
//! slice. Numbers go through the C library's strtod, so they parse exactly as the C did on each target.

use core::ffi::{c_char, c_int, CStr};

use crate::bits::{slice, slice_mut};
use crate::ffi::strtod;

pub const RDOF_JNULL: u32 = 0;
pub const RDOF_JBOOL: u32 = 1;
pub const RDOF_JNUM: u32 = 2;
pub const RDOF_JSTR: u32 = 3;
pub const RDOF_JARR: u32 = 4;
pub const RDOF_JOBJ: u32 = 5;

const MAX_DEPTH: i32 = 32;

/// rdof_jnode_t
#[repr(C)]
pub struct JNode {
    pub type_: u32,
    pub key: *const c_char,
    pub str_: *const c_char,
    pub num: f64,
    pub b: bool,
    pub integer: bool,
    pub child: c_int,
    pub next: c_int,
    pub count: c_int,
}

/// rdof_jdoc_t
#[repr(C)]
pub struct JDoc {
    pub nodes: *mut JNode,
    pub max: c_int,
    pub n: c_int,
    pub buf: *mut c_char,
    pub cap: usize,
    pub used: usize,
    pub error: [c_char; 64],
}

struct Parser<'a> {
    doc: &'a mut JDoc,
    nodes: &'a mut [JNode],
    buf: &'a mut [u8],
    t: &'a [u8],
    p: usize,
    depth: i32,
}

/// Appends `s` to the error at `*at`, as snprintf would (cut at 63 bytes).
fn put_error(error: &mut [c_char; 64], at: &mut usize, s: &[u8]) {
    for &c in s {
        if *at < error.len() - 1 {
            error[*at] = c as c_char;
            *at += 1;
        }
    }
}

/// `v` in decimal, as %d.
fn put_int(error: &mut [c_char; 64], at: &mut usize, v: i32) {
    let mut digits = [0u8; 11];
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
    if v < 0 {
        put_error(error, at, b"-");
    }
    put_error(error, at, &digits[i..]);
}

fn hex4(p: &[u8]) -> Option<u32> {
    p.iter().take(4).try_fold(0, |v, &c| Some(v << 4 | (c as char).to_digit(16)?))
}

impl Parser<'_> {
    fn fail(&mut self, why: &[u8]) -> c_int {
        let mut at = 0;
        let e = &mut self.doc.error;
        put_error(e, &mut at, why);
        put_error(e, &mut at, b" at offset ");
        put_int(e, &mut at, self.p as i32);
        e[at] = 0;
        -1
    }

    fn peek(&self) -> Option<u8> {
        self.t.get(self.p).copied()
    }

    fn ws(&mut self) {
        while matches!(self.peek(), Some(b' ' | b'\t' | b'\n' | b'\r')) {
            self.p += 1;
        }
    }

    fn new_node(&mut self, type_: u32) -> c_int {
        let d = &mut *self.doc;
        if d.n >= d.max {
            return -1;
        }
        self.nodes[d.n as usize] = JNode {
            type_,
            key: core::ptr::null(),
            str_: core::ptr::null(),
            num: 0.0,
            b: false,
            integer: false,
            child: -1,
            next: -1,
            count: 0,
        };
        d.n += 1;
        d.n - 1
    }

    fn put_char(&mut self, c: u8) -> bool {
        let d = &mut *self.doc;
        if d.used >= d.cap {
            return false;
        }
        self.buf[d.used] = c;
        d.used += 1;
        true
    }

    // Each byte goes in on its own: in a full buffer, as many as fit (the string then fails at its NUL).
    fn put_utf8(&mut self, cp: u32) {
        if cp < 0x80 {
            self.put_char(cp as u8);
        } else if cp < 0x800 {
            self.put_char((0xC0 | (cp >> 6)) as u8);
            self.put_char((0x80 | (cp & 0x3F)) as u8);
        } else if cp < 0x10000 {
            self.put_char((0xE0 | (cp >> 12)) as u8);
            self.put_char((0x80 | ((cp >> 6) & 0x3F)) as u8);
            self.put_char((0x80 | (cp & 0x3F)) as u8);
        } else {
            self.put_char((0xF0 | (cp >> 18)) as u8);
            self.put_char((0x80 | ((cp >> 12) & 0x3F)) as u8);
            self.put_char((0x80 | ((cp >> 6) & 0x3F)) as u8);
            self.put_char((0x80 | (cp & 0x3F)) as u8);
        }
    }

    /// The string at p, unescaped into the buffer and NUL terminated: its text, or None.
    fn string(&mut self) -> Option<*const c_char> {
        let start = self.doc.used;
        if self.peek() != Some(b'"') {
            return None;
        }
        self.p += 1;
        while let Some(c) = self.peek().filter(|&c| c != b'"') {
            self.p += 1;
            if c < 0x20 {
                return None;
            }
            if c != b'\\' {
                if !self.put_char(c) {
                    return None;
                }
                continue;
            }
            let c = self.peek()?;
            self.p += 1;
            match c {
                b'"' | b'\\' | b'/' => _ = self.put_char(c),
                b'b' => _ = self.put_char(0x08),
                b'f' => _ = self.put_char(0x0C),
                b'n' => _ = self.put_char(b'\n'),
                b'r' => _ = self.put_char(b'\r'),
                b't' => _ = self.put_char(b'\t'),
                b'u' => {
                    let rest = &self.t[self.p..];
                    let mut cp = if rest.len() < 4 { None } else { hex4(rest) }?;
                    self.p += 4;
                    // a high surrogate and its low one: one character; either alone is kept as its code point
                    let rest = &self.t[self.p..];
                    if (0xD800..0xDC00).contains(&cp) && rest.len() >= 6 && rest.starts_with(b"\\u") {
                        if let Some(l) = hex4(&rest[2..]).filter(|l| (0xDC00..0xE000).contains(l)) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (l - 0xDC00);
                            self.p += 6;
                        }
                    }
                    self.put_utf8(cp);
                }
                _ => return None,
            }
        }
        if self.peek().is_none() || !self.put_char(0) {
            return None;
        }
        self.p += 1;
        Some(self.buf[start..].as_ptr().cast())
    }

    fn container(&mut self, object: bool) -> c_int {
        let me = self.new_node(if object { RDOF_JOBJ } else { RDOF_JARR });
        if me < 0 {
            return self.fail(b"too many values");
        }
        self.depth += 1;
        if self.depth > MAX_DEPTH {
            return self.fail(b"nested too deep");
        }
        let close = if object { b'}' } else { b']' };
        self.p += 1;
        self.ws();
        if self.peek() == Some(close) {
            self.p += 1;
            self.depth -= 1;
            return me;
        }
        let mut last = -1;
        loop {
            let mut key = core::ptr::null();
            self.ws();
            if object {
                match self.string() {
                    Some(k) => key = k,
                    None => return self.fail(b"bad key"),
                }
                self.ws();
                if self.peek() != Some(b':') {
                    return self.fail(b"expected :");
                }
                self.p += 1;
            }
            self.ws();
            let v = self.value();
            if v < 0 {
                return -1;
            }
            self.nodes[v as usize].key = key;
            if last < 0 {
                self.nodes[me as usize].child = v;
            } else {
                self.nodes[last as usize].next = v;
            }
            last = v;
            self.nodes[me as usize].count += 1;
            self.ws();
            match self.peek() {
                Some(b',') => self.p += 1,
                Some(c) if c == close => {
                    self.p += 1;
                    self.depth -= 1;
                    return me;
                }
                _ => return self.fail(if object { b"expected , or }" } else { b"expected , or ]" }),
            }
        }
    }

    fn value(&mut self) -> c_int {
        self.ws();
        let Some(c) = self.peek() else {
            return self.fail(b"unexpected end");
        };
        if c == b'{' || c == b'[' {
            return self.container(c == b'{');
        }
        if c == b'"' {
            let n = self.new_node(RDOF_JSTR);
            if n < 0 {
                return self.fail(b"too many values");
            }
            return match self.string() {
                Some(s) => {
                    self.nodes[n as usize].str_ = s;
                    n
                }
                None => self.fail(b"bad string"),
            };
        }
        let rest = &self.t[self.p..];
        if rest.starts_with(b"true") || rest.starts_with(b"false") {
            let n = self.new_node(RDOF_JBOOL);
            if n < 0 {
                return self.fail(b"too many values");
            }
            self.nodes[n as usize].b = c == b't';
            self.p += if c == b't' { 4 } else { 5 };
            return n;
        }
        if rest.starts_with(b"null") {
            let n = self.new_node(RDOF_JNULL);
            if n < 0 {
                return self.fail(b"too many values");
            }
            self.p += 4;
            return n;
        }
        if c == b'-' || c.is_ascii_digit() {
            // at most 47 characters of number, NUL terminated for strtod
            let mut tmp = [0u8; 48];
            let mut k = 0;
            let mut integer = true;
            while let Some(c) = self.peek().filter(|c| k < tmp.len() - 1 && b"+-0123456789.eE".contains(c)) {
                if matches!(c, b'.' | b'e' | b'E') {
                    integer = false;
                }
                tmp[k] = c;
                k += 1;
                self.p += 1;
            }
            let mut endp: *mut c_char = core::ptr::null_mut();
            let v = unsafe { strtod(tmp.as_ptr().cast(), &mut endp) };
            if unsafe { *endp } != 0 {
                return self.fail(b"bad number");
            }
            let n = self.new_node(RDOF_JNUM);
            if n < 0 {
                return self.fail(b"too many values");
            }
            self.nodes[n as usize].num = v;
            self.nodes[n as usize].integer = integer;
            return n;
        }
        self.fail(b"unexpected character")
    }
}

/// The parsed nodes (rdof_jget and the like): the first doc.n of them.
///
/// # Safety
/// doc's nodes hold doc.n of them.
unsafe fn parsed(doc: &JDoc) -> &[JNode] {
    let n = doc.n.max(0) as usize;
    if n == 0 {
        &[]
    } else {
        core::slice::from_raw_parts(doc.nodes, n)
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_jparse(doc: &mut JDoc, text: *const c_char, len: usize) -> c_int {
    doc.n = 0;
    doc.used = 0;
    doc.error[0] = 0;
    let nodes = core::slice::from_raw_parts_mut(doc.nodes, doc.max.max(0) as usize);
    let buf = slice_mut(doc.buf.cast(), doc.cap);
    let mut ps = Parser { doc, nodes, buf, t: slice(text.cast(), len), p: 0, depth: 0 };
    let root = ps.value();
    if root < 0 {
        return -1;
    }
    ps.ws();
    if ps.p != ps.t.len() {
        return ps.fail(b"extra text after the value");
    }
    root
}

#[no_mangle]
pub unsafe extern "C" fn rdof_jget(doc: &JDoc, obj: c_int, key: *const c_char) -> c_int {
    let nodes = parsed(doc);
    let Some(o) = nodes.get(obj as usize).filter(|o| obj >= 0 && o.type_ == RDOF_JOBJ) else {
        return -1;
    };
    let key = CStr::from_ptr(key);
    let mut found = -1;
    let mut c = o.child;
    while let Some(n) = nodes.get(c as usize).filter(|_| c >= 0) {
        if !n.key.is_null() && CStr::from_ptr(n.key) == key {
            found = c;
        }
        c = n.next;
    }
    found
}

#[no_mangle]
pub unsafe extern "C" fn rdof_jat(doc: &JDoc, node: c_int, mut i: c_int) -> c_int {
    let nodes = parsed(doc);
    let Some(n) = nodes.get(node as usize).filter(|_| node >= 0) else {
        return -1;
    };
    let mut c = n.child;
    while c >= 0 && i > 0 {
        i -= 1;
        c = nodes.get(c as usize).map_or(-1, |n| n.next);
    }
    c
}

#[no_mangle]
pub unsafe extern "C" fn rdof_jnode(doc: &JDoc, node: c_int) -> *const JNode {
    match parsed(doc).get(node as usize) {
        Some(n) if node >= 0 => n,
        _ => core::ptr::null(),
    }
}

// ── JSON with comments, as text ───────────────────────────────────────────────

fn is_ws(c: u8) -> bool {
    matches!(c, b' ' | b'\t' | b'\n' | b'\r')
}

/// Past the whitespace and comments at i.
fn skip_ws(t: &[u8], mut i: usize) -> usize {
    let n = t.len();
    loop {
        while i < n && is_ws(t[i]) {
            i += 1;
        }
        if i + 1 < n && t[i] == b'/' && t[i + 1] == b'/' {
            while i < n && t[i] != b'\n' {
                i += 1;
            }
        } else if i + 1 < n && t[i] == b'/' && t[i + 1] == b'*' {
            i += 2;
            while i + 1 < n && !(t[i] == b'*' && t[i + 1] == b'/') {
                i += 1;
            }
            i = (i + 2).min(n);
        } else {
            return i;
        }
    }
}

/// Past the string starting at i (a quote).
fn skip_str(t: &[u8], mut i: usize) -> usize {
    i += 1;
    while i < t.len() && t[i] != b'"' {
        i += if t[i] == b'\\' { 2 } else { 1 };
    }
    if i < t.len() {
        i + 1
    } else {
        t.len()
    }
}

/// Past the value at i: a string, a container, or a bare word.
fn skip_value(t: &[u8], mut i: usize) -> usize {
    let n = t.len();
    if i >= n {
        return n;
    }
    if t[i] == b'"' {
        return skip_str(t, i);
    }
    if t[i] == b'{' || t[i] == b'[' {
        let mut depth = 0;
        while i < n {
            i = skip_ws(t, i);
            if i >= n {
                break;
            }
            match t[i] {
                b'"' => {
                    i = skip_str(t, i);
                    continue;
                }
                b'{' | b'[' => depth += 1,
                b'}' | b']' => {
                    depth -= 1;
                    if depth == 0 {
                        return i + 1;
                    }
                }
                _ => {}
            }
            i += 1;
        }
        return n;
    }
    // a NUL ends it too (the C's strchr found the terminator)
    while i < n && !b",}] \t\r\n/\0".contains(&t[i]) {
        i += 1;
    }
    i
}

/// The top-level member `key` from *i (inside the object): its key's start and value's end, or None. *i is where
/// to keep looking.
fn find_member(t: &[u8], i: &mut usize, key: &[u8]) -> Option<(usize, usize)> {
    let n = t.len();
    while *i < n {
        let p = skip_ws(t, *i);
        if p >= n || t[p] == b'}' {
            return None;
        }
        if t[p] == b',' {
            *i = p + 1;
            continue;
        }
        if t[p] != b'"' {
            return None;
        }
        let ke = skip_str(t, p);
        let c = skip_ws(t, ke);
        if c >= n || t[c] != b':' {
            return None;
        }
        let e = skip_value(t, skip_ws(t, c + 1));
        *i = e;
        if ke - p == key.len() + 2 && &t[p + 1..ke - 1] == key {
            return Some((p, e));
        }
    }
    None
}

#[no_mangle]
pub unsafe extern "C" fn rdof_jsonc_strip(input: *const c_char, n: usize, out: *mut c_char) -> usize {
    let t = slice(input.cast(), n);
    let out = slice_mut(out.cast(), n);
    let (mut o, mut i) = (0, 0);
    while i < n {
        if t[i] == b'"' {
            let mut j = i + 1;
            while j < n && t[j] != b'"' {
                j += if t[j] == b'\\' { 2 } else { 1 };
            }
            j = j.min(n - 1);
            out[o..o + j - i + 1].copy_from_slice(&t[i..=j]);
            o += j - i + 1;
            i = j + 1;
        } else if t[i] == b'/' && i + 1 < n && t[i + 1] == b'/' {
            while i < n && t[i] != b'\n' {
                i += 1;
            }
        } else if t[i] == b'/' && i + 1 < n && t[i + 1] == b'*' {
            i += 2;
            while i + 1 < n && !(t[i] == b'*' && t[i + 1] == b'/') {
                i += 1;
            }
            i += 2;
            out[o] = b' ';
            o += 1;
        } else {
            if t[i] == b']' || t[i] == b'}' {
                // drop a trailing comma before it
                let mut k = o;
                while k > 0 && is_ws(out[k - 1]) {
                    k -= 1;
                }
                if k > 0 && out[k - 1] == b',' {
                    out[k - 1] = b' ';
                }
            }
            out[o] = t[i];
            o += 1;
            i += 1;
        }
    }
    o
}

#[no_mangle]
pub unsafe extern "C" fn rdof_jsonc_drop(t: *mut c_char, n: usize, key: *const c_char) -> usize {
    let t = slice_mut(t.cast(), n);
    let key = CStr::from_ptr(key).to_bytes();
    let mut n = n;
    let open = skip_ws(t, 0);
    if open >= n || t[open] != b'{' {
        return n;
    }
    let mut i = open + 1;
    while let Some((mut ks, ve)) = find_member(&t[..n], &mut i, key) {
        // With its comma after it; a comma left before a last member is a trailing comma (fine in JSONC).
        let mut end = ve;
        let a = skip_ws(&t[..n], ve);
        if a < n && t[a] == b',' {
            end = a + 1;
        }
        // The whole line, when the member had it to itself.
        let mut s = ks;
        while s > 0 && (t[s - 1] == b' ' || t[s - 1] == b'\t') {
            s -= 1;
        }
        let mut e = end;
        while e < n && (t[e] == b' ' || t[e] == b'\t') {
            e += 1;
        }
        if (s == 0 || t[s - 1] == b'\n') && e < n && t[e] == b'\n' {
            ks = s;
            end = e + 1;
        }
        t.copy_within(end..n, ks);
        n -= end - ks;
        t[n] = 0;
        i = ks;
    }
    n
}

#[no_mangle]
pub unsafe extern "C" fn rdof_jsonc_add(
    t: *const c_char,
    n: usize,
    key: *const c_char,
    value: *const c_char,
    out: *mut c_char,
    cap: usize,
) -> usize {
    let t = slice(t.cast(), n);
    let open = skip_ws(t, 0);
    if open >= n || t[open] != b'{' {
        return 0;
    }
    // After the last member (and its comma, if it has one), or after the brace.
    let (mut i, mut last) = (open + 1, open + 1);
    let (mut any, mut comma) = (false, false);
    loop {
        let p = skip_ws(t, i);
        if p >= n {
            return 0;
        }
        if t[p] == b'}' {
            break;
        }
        if t[p] == b',' {
            (i, last, comma) = (p + 1, p + 1, true);
            continue;
        }
        if t[p] != b'"' {
            return 0;
        }
        let c = skip_ws(t, skip_str(t, p));
        if c >= n || t[c] != b':' {
            return 0;
        }
        last = skip_value(t, skip_ws(t, c + 1));
        i = last;
        (any, comma) = (true, false);
    }
    let parts: [&[u8]; 7] = [
        &t[..last],
        if any && !comma { b"," } else { b"" },
        b"\n  \"",
        CStr::from_ptr(key).to_bytes(),
        b"\": ",
        CStr::from_ptr(value).to_bytes(),
        &t[last..],
    ];
    let w: usize = parts.iter().map(|p| p.len()).sum();
    let out = slice_mut(out.cast(), cap);
    if w >= cap {
        if cap > 0 {
            out[0] = 0;
        }
        return 0;
    }
    let mut o = 0;
    for p in parts {
        out[o..o + p.len()].copy_from_slice(p);
        o += p.len();
    }
    out[w] = 0;
    w
}
