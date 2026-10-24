// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! snprintf's work for the ported modules: text into a fixed buffer, cut where it ends. Integers and strings in
//! Rust; %f through C's snprintf (correctly rounded; core::fmt's float code costs several KB).

use core::ffi::{c_char, c_int, CStr};

use crate::ffi;

/// Output into `buf`: bytes past its end are dropped but counted, as snprintf's return.
pub struct Out<'a> {
    pub buf: &'a mut [u8],
    pub n: usize,
}

impl<'a> Out<'a> {
    pub fn new(buf: &'a mut [u8]) -> Self {
        Out { buf, n: 0 }
    }

    pub fn byte(&mut self, c: u8) {
        if let Some(b) = self.buf.get_mut(self.n) {
            *b = c;
        }
        self.n += 1;
    }

    /// %s
    pub fn bytes(&mut self, s: &[u8]) {
        s.iter().for_each(|&c| self.byte(c));
    }

    /// %-W.Ws: at most `width` bytes of s, space padded to `width`.
    pub fn text(&mut self, s: &[u8], width: usize) {
        let s = &s[..s.len().min(width)];
        self.bytes(s);
        (s.len()..width).for_each(|_| self.byte(b' '));
    }

    /// %0Wd / %0Wu: zero padded to `width`, after the sign (%d is width 0).
    pub fn int(&mut self, v: i64, width: usize) {
        let mut digits = [0u8; 20];
        let mut m = v.unsigned_abs();
        let mut k = digits.len();
        loop {
            k -= 1;
            digits[k] = b'0' + (m % 10) as u8;
            m /= 10;
            if m == 0 {
                break;
            }
        }
        let len = digits.len() - k + usize::from(v < 0);
        if v < 0 {
            self.byte(b'-');
        }
        (len..width).for_each(|_| self.byte(b'0'));
        self.bytes(&digits[k..]);
    }

    /// %.Pf
    pub fn fixed(&mut self, v: f64, prec: c_int) {
        let mut s = [0u8; 64];
        let n = fixed(v, prec, &mut s);
        self.bytes(n);
    }
}

/// %.Pf into `buf`, cut to fit; the text written.
pub fn fixed(v: f64, prec: c_int, buf: &mut [u8]) -> &[u8] {
    if buf.is_empty() {
        return buf;
    }
    let n = unsafe { ffi::snprintf(buf.as_mut_ptr().cast(), buf.len(), c"%.*f".as_ptr(), prec, v) };
    &buf[..(n.max(0) as usize).min(buf.len() - 1)]
}

/// snprintf(out, cap, ...)'s contract for what `f` writes: cut to cap - 1 bytes and NUL terminated (nothing
/// written when cap is 0); the length it would have had.
pub unsafe fn c_out(out: *mut c_char, cap: usize, f: impl FnOnce(&mut Out)) -> usize {
    let Some(room) = cap.checked_sub(1) else {
        let mut o = Out::new(&mut []);
        f(&mut o);
        return o.n;
    };
    let buf = crate::bits::slice_mut(out.cast(), cap);
    let mut o = Out::new(&mut buf[..room]);
    f(&mut o);
    let n = o.n;
    buf[n.min(room)] = 0;
    n
}

/// A frequency in Hz as MHz, "%.5f".
pub fn mhz(hz: u32, buf: &mut [u8; 64]) -> &[u8] {
    fixed(f64::from(hz) / 1e6, 5, buf)
}

/// A piece of text for print(): %s or %d.
#[derive(Clone, Copy)]
pub enum Piece<'a> {
    S(&'a [u8]),
    I(i64),
}

/// snprintf into `buf`: the pieces, cut to fit and NUL terminated (when there's room for one); the length they
/// wanted, as snprintf's return. Not generic, so each message costs only its pieces.
#[inline(never)]
pub fn print(buf: &mut [u8], pieces: &[Piece]) -> usize {
    let mut o = Out::new(buf);
    for p in pieces {
        match *p {
            Piece::S(s) => o.bytes(s),
            Piece::I(v) => o.int(v, 0),
        }
    }
    let n = o.n;
    if let Some(last) = o.buf.len().checked_sub(1) {
        o.buf[n.min(last)] = 0;
    }
    n
}

/// A C string's bytes; NULL is empty.
pub unsafe fn c_bytes<'a>(s: *const c_char) -> &'a [u8] {
    if s.is_null() {
        &[]
    } else {
        CStr::from_ptr(s).to_bytes()
    }
}
