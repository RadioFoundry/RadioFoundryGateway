// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! A 128x64 SSD1306 / SH1106 frame, its panel commands and the board's status screen (rdof/oled.h): a port of
//! common/src/oled.c.

use core::ffi::{c_char, c_int};

use crate::bits::slice_mut;
use crate::text::{c_bytes, Out};

const W: c_int = 128;
const H: c_int = 64;
const PAGES: usize = 8;

#[repr(C)]
pub struct RdofFrameBuf {
    pub buf: [u8; W as usize * PAGES],
}

#[repr(C)]
pub struct RdofOledStatus {
    pub name: *const c_char,
    pub ip: *const c_char,
    pub wifi: bool,
    pub mqtt: bool,
    pub hblink: bool,
    pub radio: bool,
    pub repeater: bool,
    pub freq: u32,
    pub tx_freq: u32,
    pub cc: c_int,
    pub state: *const c_char,
    pub rx_line: *const c_char,
    pub rx_active: bool,
    pub tx_line: *const c_char,
    pub tx_active: bool,
    pub clock: *const c_char,
}

/// The classic 5x7 bitmap font (Adafruit GFX glcdfont.c, BSD license), printable ASCII from 0x20.
static GLYPHS: [[u8; 5]; 95] = [
    [0x00, 0x00, 0x00, 0x00, 0x00],
    [0x00, 0x00, 0x5F, 0x00, 0x00],
    [0x00, 0x07, 0x00, 0x07, 0x00],
    [0x14, 0x7F, 0x14, 0x7F, 0x14],
    [0x24, 0x2A, 0x7F, 0x2A, 0x12],
    [0x23, 0x13, 0x08, 0x64, 0x62],
    [0x36, 0x49, 0x56, 0x20, 0x50],
    [0x00, 0x08, 0x07, 0x03, 0x00],
    [0x00, 0x1C, 0x22, 0x41, 0x00],
    [0x00, 0x41, 0x22, 0x1C, 0x00],
    [0x2A, 0x1C, 0x7F, 0x1C, 0x2A],
    [0x08, 0x08, 0x3E, 0x08, 0x08],
    [0x00, 0x80, 0x70, 0x30, 0x00],
    [0x08, 0x08, 0x08, 0x08, 0x08],
    [0x00, 0x00, 0x60, 0x60, 0x00],
    [0x20, 0x10, 0x08, 0x04, 0x02],
    [0x3E, 0x51, 0x49, 0x45, 0x3E],
    [0x00, 0x42, 0x7F, 0x40, 0x00],
    [0x72, 0x49, 0x49, 0x49, 0x46],
    [0x21, 0x41, 0x49, 0x4D, 0x33],
    [0x18, 0x14, 0x12, 0x7F, 0x10],
    [0x27, 0x45, 0x45, 0x45, 0x39],
    [0x3C, 0x4A, 0x49, 0x49, 0x31],
    [0x41, 0x21, 0x11, 0x09, 0x07],
    [0x36, 0x49, 0x49, 0x49, 0x36],
    [0x46, 0x49, 0x49, 0x29, 0x1E],
    [0x00, 0x00, 0x14, 0x00, 0x00],
    [0x00, 0x40, 0x34, 0x00, 0x00],
    [0x00, 0x08, 0x14, 0x22, 0x41],
    [0x14, 0x14, 0x14, 0x14, 0x14],
    [0x00, 0x41, 0x22, 0x14, 0x08],
    [0x02, 0x01, 0x59, 0x09, 0x06],
    [0x3E, 0x41, 0x5D, 0x59, 0x4E],
    [0x7C, 0x12, 0x11, 0x12, 0x7C],
    [0x7F, 0x49, 0x49, 0x49, 0x36],
    [0x3E, 0x41, 0x41, 0x41, 0x22],
    [0x7F, 0x41, 0x41, 0x41, 0x3E],
    [0x7F, 0x49, 0x49, 0x49, 0x41],
    [0x7F, 0x09, 0x09, 0x09, 0x01],
    [0x3E, 0x41, 0x41, 0x51, 0x73],
    [0x7F, 0x08, 0x08, 0x08, 0x7F],
    [0x00, 0x41, 0x7F, 0x41, 0x00],
    [0x20, 0x40, 0x41, 0x3F, 0x01],
    [0x7F, 0x08, 0x14, 0x22, 0x41],
    [0x7F, 0x40, 0x40, 0x40, 0x40],
    [0x7F, 0x02, 0x1C, 0x02, 0x7F],
    [0x7F, 0x04, 0x08, 0x10, 0x7F],
    [0x3E, 0x41, 0x41, 0x41, 0x3E],
    [0x7F, 0x09, 0x09, 0x09, 0x06],
    [0x3E, 0x41, 0x51, 0x21, 0x5E],
    [0x7F, 0x09, 0x19, 0x29, 0x46],
    [0x26, 0x49, 0x49, 0x49, 0x32],
    [0x03, 0x01, 0x7F, 0x01, 0x03],
    [0x3F, 0x40, 0x40, 0x40, 0x3F],
    [0x1F, 0x20, 0x40, 0x20, 0x1F],
    [0x3F, 0x40, 0x38, 0x40, 0x3F],
    [0x63, 0x14, 0x08, 0x14, 0x63],
    [0x03, 0x04, 0x78, 0x04, 0x03],
    [0x61, 0x59, 0x49, 0x4D, 0x43],
    [0x00, 0x7F, 0x41, 0x41, 0x41],
    [0x02, 0x04, 0x08, 0x10, 0x20],
    [0x00, 0x41, 0x41, 0x41, 0x7F],
    [0x04, 0x02, 0x01, 0x02, 0x04],
    [0x40, 0x40, 0x40, 0x40, 0x40],
    [0x00, 0x03, 0x07, 0x08, 0x00],
    [0x20, 0x54, 0x54, 0x78, 0x40],
    [0x7F, 0x28, 0x44, 0x44, 0x38],
    [0x38, 0x44, 0x44, 0x44, 0x28],
    [0x38, 0x44, 0x44, 0x28, 0x7F],
    [0x38, 0x54, 0x54, 0x54, 0x18],
    [0x00, 0x08, 0x7E, 0x09, 0x02],
    [0x18, 0xA4, 0xA4, 0x9C, 0x78],
    [0x7F, 0x08, 0x04, 0x04, 0x78],
    [0x00, 0x44, 0x7D, 0x40, 0x00],
    [0x20, 0x40, 0x40, 0x3D, 0x00],
    [0x7F, 0x10, 0x28, 0x44, 0x00],
    [0x00, 0x41, 0x7F, 0x40, 0x00],
    [0x7C, 0x04, 0x78, 0x04, 0x78],
    [0x7C, 0x08, 0x04, 0x04, 0x78],
    [0x38, 0x44, 0x44, 0x44, 0x38],
    [0xFC, 0x18, 0x24, 0x24, 0x18],
    [0x18, 0x24, 0x24, 0x18, 0xFC],
    [0x7C, 0x08, 0x04, 0x04, 0x08],
    [0x48, 0x54, 0x54, 0x54, 0x24],
    [0x04, 0x04, 0x3F, 0x44, 0x24],
    [0x3C, 0x40, 0x40, 0x20, 0x7C],
    [0x1C, 0x20, 0x40, 0x20, 0x1C],
    [0x3C, 0x40, 0x30, 0x40, 0x3C],
    [0x44, 0x28, 0x10, 0x28, 0x44],
    [0x4C, 0x90, 0x90, 0x90, 0x7C],
    [0x44, 0x64, 0x54, 0x4C, 0x44],
    [0x00, 0x08, 0x36, 0x41, 0x00],
    [0x00, 0x00, 0x77, 0x00, 0x00],
    [0x00, 0x41, 0x36, 0x08, 0x00],
    [0x02, 0x01, 0x02, 0x04, 0x02],
];

fn glyph(ch: u8) -> &'static [u8; 5] {
    let c = if (0x20..0x20 + 95).contains(&ch) { ch } else { b'?' };
    &GLYPHS[usize::from(c - 0x20)]
}

#[no_mangle]
pub extern "C" fn rdof_oled_clear(f: &mut RdofFrameBuf) {
    f.buf.fill(0);
}

/// The byte and bit of (x, y), None off the panel.
fn at(x: c_int, y: c_int) -> Option<(usize, u8)> {
    ((0..W).contains(&x) && (0..H).contains(&y)).then(|| (((y >> 3) * W + x) as usize, 1 << (y & 7)))
}

#[no_mangle]
pub extern "C" fn rdof_oled_pixel(f: &mut RdofFrameBuf, x: c_int, y: c_int, on: bool) {
    if let Some((i, bit)) = at(x, y) {
        if on {
            f.buf[i] |= bit;
        } else {
            f.buf[i] &= !bit;
        }
    }
}

#[no_mangle]
pub extern "C" fn rdof_oled_get(f: &RdofFrameBuf, x: c_int, y: c_int) -> bool {
    at(x, y).is_some_and(|(i, bit)| f.buf[i] & bit != 0)
}

#[no_mangle]
pub extern "C" fn rdof_oled_hline(f: &mut RdofFrameBuf, x0: c_int, x1: c_int, y: c_int, on: bool) {
    // only what's on the panel: the C's loop over the rest draws nothing
    for x in x0.max(-1)..=x1.min(W) {
        rdof_oled_pixel(f, x, y, on);
    }
}

#[no_mangle]
pub extern "C" fn rdof_oled_rect(f: &mut RdofFrameBuf, x: c_int, y: c_int, w: c_int, h: c_int, fill: bool, on: bool) {
    let (x1, y1) = (x.saturating_add(w) - 1, y.saturating_add(h) - 1);
    if fill {
        for yy in y.max(-1)..=y1.min(H) {
            rdof_oled_hline(f, x, x1, yy, on);
        }
        return;
    }
    rdof_oled_hline(f, x, x1, y, on);
    rdof_oled_hline(f, x, x1, y1, on);
    for yy in y.max(-1)..=y1.min(H) {
        rdof_oled_pixel(f, x, yy, on);
        rdof_oled_pixel(f, x1, yy, on);
    }
}

/// The glyph columns drawn: all five, or (tight, not a letter or digit) its first lit one, or none.
fn columns(g: &[u8; 5], ch: u8, tight: bool) -> &'static [usize] {
    const ALL: [usize; 5] = [0, 1, 2, 3, 4];
    if tight && !ch.is_ascii_alphanumeric() {
        return match g.iter().position(|&c| c != 0) {
            Some(c) => &ALL[c..c + 1],
            None => &[],
        };
    }
    &ALL
}

fn text_width(s: &[u8], tight: bool) -> c_int {
    s.iter().map(|&ch| columns(glyph(ch), ch, tight).len() as c_int + 1).sum()
}

#[no_mangle]
pub unsafe extern "C" fn rdof_oled_text_width(s: *const c_char, tight: bool) -> c_int {
    text_width(c_bytes(s), tight)
}

fn text(f: &mut RdofFrameBuf, mut x: c_int, y: c_int, s: &[u8], invert: bool, tight: bool) -> c_int {
    for &ch in s {
        if x >= W {
            break;
        }
        let g = glyph(ch);
        let cols = columns(g, ch, tight);
        for k in 0..=cols.len() {
            let bits = cols.get(k).map_or(0, |&c| g[c]);
            for row in 0..8 {
                rdof_oled_pixel(f, x, y.saturating_add(row), ((bits >> row) & 1 != 0) != invert);
            }
            x += 1;
        }
    }
    x
}

#[no_mangle]
pub unsafe extern "C" fn rdof_oled_text(
    f: &mut RdofFrameBuf,
    x: c_int,
    y: c_int,
    s: *const c_char,
    invert: bool,
    tight: bool,
) -> c_int {
    text(f, x, y, c_bytes(s), invert, tight)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_oled_init_cmds(rotate180: bool, contrast: c_int, out: *mut u8, cap: usize) -> usize {
    let (seg, com) = if rotate180 { (0xA0, 0xC0) } else { (0xA1, 0xC8) };
    let cmds = [
        0xAE,
        0xD5,
        0x80,
        0xA8,
        0x3F,
        0xD3,
        0x00,
        0x40,
        0x8D,
        0x14,
        0x20,
        0x00,
        seg,
        com,
        0xDA,
        0x12,
        0x81,
        contrast as u8,
        0xD9,
        0xF1,
        0xDB,
        0x40,
        0xA4,
        0xA6,
        0xAF,
    ];
    let n = cmds.len().min(cap);
    slice_mut(out, n).copy_from_slice(&cmds[..n]);
    n
}

#[no_mangle]
pub unsafe extern "C" fn rdof_oled_page_cmds(sh1106: bool, page: c_int, out: *mut u8, cap: usize) -> usize {
    let page = page as u8;
    let cmds: &[u8] =
        if sh1106 { &[0xB0u8.wrapping_add(page), 0x02, 0x10] } else { &[0x21, 0, W as u8 - 1, 0x22, page, page] };
    if cap < cmds.len() {
        return 0;
    }
    slice_mut(out, cmds.len()).copy_from_slice(cmds);
    cmds.len()
}

/// A health tag: inverted when up, struck through when down.
fn tag(f: &mut RdofFrameBuf, x: c_int, y: c_int, s: &[u8], up: bool) -> c_int {
    let x1 = text(f, x, y, s, up, false);
    if !up {
        rdof_oled_hline(f, x, x1 - 2, y + 3, true);
    }
    x1 + 2
}

#[no_mangle]
pub unsafe extern "C" fn rdof_oled_render(f: &mut RdofFrameBuf, s: &RdofOledStatus) {
    let mut buf = [0u8; 40];
    rdof_oled_clear(f);
    // the network
    let mut line = Out::new(&mut buf);
    line.bytes(if s.wifi { b"WiFi " } else { b"no WiFi " });
    line.bytes(c_bytes(s.ip));
    text(f, 0, 0, cut(&line), false, true);
    // health and the time
    let mut x = tag(f, 0, 9, b"MQ", s.mqtt);
    x = tag(f, x, 9, b"HB", s.hblink);
    tag(f, x, 9, if s.repeater { b"RPT" } else { b"HAT" }, s.radio);
    let clock = c_bytes(s.clock);
    if !clock.is_empty() {
        text(f, W - text_width(clock, false), 9, clock, false, false);
    }
    rdof_oled_hline(f, 0, W - 1, 18, true);
    // where it is
    let mut line = Out::new(&mut buf);
    line.bytes(c_bytes(s.name));
    line.byte(b' ');
    line.fixed(f64::from(s.freq) / 1e6, 4);
    if s.repeater {
        line.byte(b'/');
        line.fixed(f64::from(s.tx_freq) / 1e6, 4);
    } else {
        line.bytes(b" CC");
        line.int(i64::from(s.cc), 0);
    }
    text(f, 0, 21, cut(&line), false, true);
    text(f, 0, 30, c_bytes(s.state), false, true);
    // what it hears and sends
    let rx = c_bytes(s.rx_line);
    if !rx.is_empty() {
        x = text(f, 0, 41, b"RX", s.rx_active, false);
        text(f, x + 3, 41, rx, false, true);
    }
    rdof_oled_hline(f, 0, W - 1, 51, true);
    x = text(f, 0, 54, b"TX", s.tx_active, false);
    let tx = c_bytes(s.tx_line);
    text(f, x + 3, 54, if tx.is_empty() { b"idle" } else { tx }, false, true);
}

/// The line as snprintf into char[40] leaves it: at most 39 bytes.
fn cut<'a>(line: &'a Out) -> &'a [u8] {
    &line.buf[..line.n.min(line.buf.len() - 1)]
}
