// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! Which of a board's radios a USB device belongs to (rdof/usbmatch.h): a port of common/src/usbmatch.c.

use core::ffi::{c_char, c_int, CStr};

use crate::bits::{slice, slice_mut};

#[repr(C)]
pub struct RdofUsbDev {
    pub vid: u16,
    pub pid: u16,
    pub serial: [c_char; 32],
    pub port: [c_char; 16],
}

#[repr(C)]
pub struct RdofUsbWant {
    pub vid: u16,
    pub pid: u16,
    pub serial: *const c_char,
    pub port: *const c_char,
}

/// A fixed char array's string: up to its first NUL, or all of it if it has none.
fn field(a: &[c_char]) -> &[u8] {
    let a: &[u8] = unsafe { core::slice::from_raw_parts(a.as_ptr().cast(), a.len()) };
    a.iter().position(|&c| c == 0).map_or(a, |n| &a[..n])
}

/// A want's string, None when NULL or "" (any).
unsafe fn wanted<'a>(s: *const c_char) -> Option<&'a [u8]> {
    if s.is_null() {
        return None;
    }
    let s = CStr::from_ptr(s).to_bytes();
    (!s.is_empty()).then_some(s)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_usb_match(d: &RdofUsbDev, wants: *const RdofUsbWant, n: c_int, taken: *const bool) -> c_int {
    let n = n.max(0) as usize;
    let wants: &[RdofUsbWant] = if n == 0 { &[] } else { core::slice::from_raw_parts(wants, n) };
    let taken: &[bool] = if taken.is_null() || n == 0 { &[] } else { core::slice::from_raw_parts(taken, n) };
    let mut best = (-1, -1);
    for (i, w) in wants.iter().enumerate() {
        if taken.get(i) == Some(&true) || w.vid != d.vid || w.pid != d.pid {
            continue;
        }
        let (serial, port) = (wanted(w.serial), wanted(w.port));
        if serial.is_some_and(|s| s != field(&d.serial)) || port.is_some_and(|p| p != field(&d.port)) {
            continue;
        }
        let score = 2 * c_int::from(serial.is_some()) + c_int::from(port.is_some());
        if score > best.1 {
            best = (i as c_int, score);
        }
    }
    best.0
}

#[no_mangle]
pub unsafe extern "C" fn rdof_usb_port_path(ports: *const u8, n: c_int, out: *mut c_char, cap: usize) {
    let ports = slice(ports, n.max(0) as usize);
    let out = slice_mut(out.cast(), cap);
    let Some(room) = cap.checked_sub(1) else { return };
    // "1.3.2", cut to the room there is, as snprintf would
    let mut o = 0;
    let mut put = |c: u8| {
        if o < room {
            out[o] = c;
            o += 1;
        }
    };
    for (i, &p) in ports.iter().enumerate() {
        if i > 0 {
            put(b'.');
        }
        if p >= 100 {
            put(b'0' + p / 100);
        }
        if p >= 10 {
            put(b'0' + p / 10 % 10);
        }
        put(b'0' + p % 10);
    }
    out[o] = 0;
}
