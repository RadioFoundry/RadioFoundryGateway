// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! The MMDVM modem serial protocol (rdof/mmdvm.h): framing, config bytes, status and version replies, the radio
//! detection, and the transmit-side burst regeneration. A port of common/src/mmdvm.c.

use core::ffi::{c_char, c_int, c_void};

use crate::bits::{bits_to_u64, set_field, slice, slice_mut};
use crate::ffi::{self, RDOF_BURST_BYTES, RDOF_DT_CSBK, RDOF_DT_VOICE, RDOF_DT_VOICE_SYNC, RDOF_MASK_CSBK};
use crate::layer2::{
    rdof_data_burst, rdof_decode_emb, rdof_emb_bits, rdof_slot_type_bits, RDOF_BS_VOICE, RDOF_DATA_XOR, RDOF_MS_VOICE,
};
use crate::text::{c_bytes, c_out};

type Burst = [u8; RDOF_BURST_BYTES];

const FRAME_START: u8 = 0xE0;
const DMR_DATA1: u8 = 0x18;
const DMR_DATA2: u8 = 0x1A;
const DMR_SHORTLC: u8 = 0x1C;
const DMR_START: u8 = 0x1D;
const DMR_ABORT: u8 = 0x1E;
const DMR_ENABLED: u8 = 0x02;
const SIMPLEX: c_int = 0x80;
const SYNC_AUDIO: u8 = 0x20;
pub const RDOF_SYNC_DATA: u8 = 0x40;
const STATUS_INBOUND_BUSY: u8 = 0x80;
const MAX_FRAME: usize = 512;
const CSBK_PREAMBLE: u8 = 0x3D;

/// rdof_modem_config_t
#[repr(C)]
pub struct RdofModemConfig {
    pub rx_invert: bool,
    pub tx_invert: bool,
    pub ptt_invert: bool,
    pub tx_delay_ms: c_int,
    pub rx_level: f64,
    pub tx_level: f64,
    pub cwid_level: f64,
    pub rx_dc_offset: c_int,
    pub tx_dc_offset: c_int,
    pub dmr_delay: c_int,
}

/// rdof_mmdvm_reader_t
#[repr(C)]
pub struct RdofMmdvmReader {
    pub buf: [u8; 2 * MAX_FRAME],
    pub len: usize,
}

/// rdof_mmdvm_status_t
#[repr(C)]
pub struct RdofMmdvmStatus {
    pub transmitting: c_int,
    pub carrier: c_int,
    pub space: c_int,
    pub space1: c_int,
    pub inbound_busy: c_int,
}

pub type FrameCb = Option<unsafe extern "C" fn(ctx: *mut c_void, command: u8, payload: *const u8, len: usize)>;

#[no_mangle]
pub extern "C" fn rdof_mmdvm_unanswered(c: u8) -> bool {
    matches!(c, DMR_DATA1 | DMR_DATA2 | DMR_SHORTLC | DMR_START | DMR_ABORT)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_mmdvm_frame(
    command: u8,
    payload: *const u8,
    len: usize,
    out: *mut u8,
    cap: usize,
) -> usize {
    if len > 252 || cap < len + 3 {
        return 0;
    }
    let out = slice_mut(out, len + 3);
    out[..3].copy_from_slice(&[FRAME_START, (len + 3) as u8, command]);
    out[3..].copy_from_slice(slice(payload, len));
    len + 3
}

#[no_mangle]
pub extern "C" fn rdof_modem_config_default(c: &mut RdofModemConfig) {
    *c = RdofModemConfig {
        rx_invert: false,
        tx_invert: false,
        ptt_invert: false,
        tx_delay_ms: 100,
        rx_level: 50.0,
        tx_level: 50.0,
        cwid_level: 50.0,
        rx_dc_offset: 0,
        tx_dc_offset: 0,
        dmr_delay: 0,
    };
}

/// max(0, min(255, int(level * 255 / 100 + 0.5))); NaN is 0 (`as` saturates).
#[no_mangle]
pub extern "C" fn rdof_mmdvm_pct(level: f64) -> c_int {
    ((level * 255.0 / 100.0 + 0.5) as c_int).clamp(0, 255)
}

#[no_mangle]
pub extern "C" fn rdof_modem_config_flags(c: &RdofModemConfig, duplex: bool) -> c_int {
    (if duplex { 0 } else { SIMPLEX })
        | c_int::from(c.rx_invert)
        | c_int::from(c.tx_invert) << 1
        | c_int::from(c.ptt_invert) << 2
}

fn pct(level: f64) -> u8 {
    rdof_mmdvm_pct(level) as u8
}

fn tx_delay(c: &RdofModemConfig) -> u8 {
    (c.tx_delay_ms / 10).clamp(0, 255) as u8
}

/// A DC offset's byte: offset + 128, wrapped.
fn dc(offset: c_int) -> u8 {
    (offset as u8).wrapping_add(128)
}

#[no_mangle]
pub extern "C" fn rdof_mmdvm_config_v1(
    c: &RdofModemConfig,
    cc: c_int,
    state: c_int,
    duplex: bool,
    p: &mut [u8; 23],
) -> usize {
    *p = [0; 23];
    p[0] = rdof_modem_config_flags(c, duplex) as u8;
    p[1] = DMR_ENABLED;
    p[2] = tx_delay(c);
    p[3] = state as u8;
    p[4] = pct(c.rx_level);
    p[5] = pct(c.cwid_level);
    p[6] = (cc & 0x0F) as u8;
    p[7] = c.dmr_delay as u8;
    p[8] = 128;
    p[10] = pct(c.tx_level);
    p[13] = dc(c.tx_dc_offset);
    p[14] = dc(c.rx_dc_offset);
    23
}

#[no_mangle]
pub extern "C" fn rdof_mmdvm_config_v2(
    c: &RdofModemConfig,
    cc: c_int,
    state: c_int,
    duplex: bool,
    p: &mut [u8; 37],
) -> usize {
    *p = [0; 37];
    p[0] = rdof_modem_config_flags(c, duplex) as u8;
    p[1] = DMR_ENABLED;
    p[3] = tx_delay(c);
    p[4] = state as u8;
    p[5] = dc(c.tx_dc_offset);
    p[6] = dc(c.rx_dc_offset);
    p[7] = pct(c.rx_level);
    p[8] = pct(c.cwid_level);
    p[10] = pct(c.tx_level);
    p[26] = (cc & 0x0F) as u8;
    p[27] = c.dmr_delay as u8;
    p[28] = 128;
    37
}

#[no_mangle]
pub extern "C" fn rdof_mmdvm_set_freq_payload(rx_hz: u32, tx_hz: u32, rf_level: c_int, out: &mut [u8; 14]) -> usize {
    out[0] = 0;
    out[1..5].copy_from_slice(&rx_hz.to_le_bytes());
    out[5..9].copy_from_slice(&tx_hz.to_le_bytes());
    out[9] = rf_level as u8;
    out[10..14].copy_from_slice(&tx_hz.to_le_bytes());
    14
}

// ── frame reader ──

#[no_mangle]
pub extern "C" fn rdof_mmdvm_reader_init(r: &mut RdofMmdvmReader) {
    r.len = 0;
}

impl RdofMmdvmReader {
    fn drop_front(&mut self, n: usize) {
        self.buf.copy_within(n..self.len, 0);
        self.len -= n;
    }

    /// Hands each whole frame to cb, skipping junk; keeps a partial one.
    unsafe fn parse(&mut self, cb: FrameCb, ctx: *mut c_void) {
        loop {
            let Some(start) = self.buf[..self.len].iter().position(|&b| b == FRAME_START) else {
                self.len = 0;
                return;
            };
            self.drop_front(start);
            if self.len < 2 {
                return;
            }
            let (length, head) = match self.buf[1] {
                // protocol 2's long frame
                0 if self.len < 3 => return,
                0 => (usize::from(self.buf[2]) + 255, 3),
                1 | 2 => {
                    self.drop_front(1);
                    continue;
                }
                n => (usize::from(n), 2),
            };
            if self.len < length {
                return;
            }
            if let Some(cb) = cb {
                cb(ctx, self.buf[head], self.buf[head + 1..].as_ptr(), length - head - 1);
            }
            self.drop_front(length);
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_mmdvm_reader_feed(
    r: &mut RdofMmdvmReader,
    data: *const u8,
    n: usize,
    cb: FrameCb,
    ctx: *mut c_void,
) {
    // What's left after a parse is always shorter than the longest frame, so there's room for a chunk.
    let mut data = slice(data, n);
    while !data.is_empty() {
        let take = data.len().min(r.buf.len() - r.len);
        r.buf[r.len..r.len + take].copy_from_slice(&data[..take]);
        r.len += take;
        data = &data[take..];
        r.parse(cb, ctx);
    }
}

// ── replies ──

#[no_mangle]
pub unsafe extern "C" fn rdof_mmdvm_parse_status(
    protocol: c_int,
    p: *const u8,
    len: usize,
    o: &mut RdofMmdvmStatus,
) -> bool {
    if len < 6 {
        return false;
    }
    let p = slice(p, len);
    let flags = if protocol >= 2 { p[1] } else { p[2] };
    o.transmitting = c_int::from(flags & 0x01);
    o.carrier = c_int::from(flags & 0x40 != 0);
    o.space = p[5].into();
    o.space1 = p[4].into();
    o.inbound_busy = c_int::from(flags & STATUS_INBOUND_BUSY != 0);
    true
}

/// Whitespace in ASCII: \t to \r, the separators 1C to 1F, and space.
fn py_space(c: u8) -> bool {
    matches!(c, b'\t'..=b'\r' | 0x1C..=0x1F | b' ')
}

#[no_mangle]
pub unsafe extern "C" fn rdof_mmdvm_parse_version(p: *const u8, len: usize, desc: *mut c_char, cap: usize) -> c_int {
    let p = slice(p, len);
    let protocol = p.first().map_or(1, |&v| c_int::from(v));
    if cap == 0 {
        return protocol;
    }
    // up to the NUL, cut to fit, non-ASCII as '?', then stripped
    let text = p.get(if protocol >= 2 { 20 } else { 1 }..).unwrap_or(&[]);
    let text = &text[..text.iter().position(|&c| c == 0).unwrap_or(text.len()).min(cap - 1)];
    let a = text.iter().position(|&c| !py_space(c)).unwrap_or(text.len());
    let b = text.iter().rposition(|&c| !py_space(c)).map_or(a, |i| i + 1);
    c_out(desc, cap, |o| text[a..b].iter().for_each(|&c| o.byte(if c < 0x80 { c } else { b'?' })));
    protocol
}

// ── the radio ──

#[repr(C)]
#[derive(Clone, Copy, PartialEq)]
pub enum RdofRadioKind {
    Unknown = 0,
    Hotspot,
    MmdvmHs,
    Mmdvm,
    Split,
}

/// needle (upper case) anywhere in hay, ignoring case.
fn contains_upper(hay: &[u8], needle: &[u8]) -> bool {
    hay.windows(needle.len()).any(|w| w.iter().zip(needle).all(|(h, n)| h.to_ascii_uppercase() == *n))
}

fn is_radio_hotspot(d: &[u8]) -> bool {
    if contains_upper(d, b"RADIOFOUNDRY GATEWAY") {
        return false; // this gateway's own SDR modem
    }
    contains_upper(d, b"RADIOFOUNDRY") || contains_upper(d, b"OPENDIGITALRADIO") || contains_upper(d, b"OPENGD77")
}

#[no_mangle]
pub unsafe extern "C" fn rdof_is_radio_hotspot(d: *const c_char) -> bool {
    is_radio_hotspot(c_bytes(d))
}

#[no_mangle]
pub unsafe extern "C" fn rdof_radio_detect(d: *const c_char) -> RdofRadioKind {
    const HS: [&[u8]; 8] =
        [b"MMDVM_HS", b"ZUMSPOT", b"NANO_HOTSPOT", b"NANODV", b"SKYBRIDGE", b"D2RG", b"LONESTAR", b"ADF7021"];
    let d = c_bytes(d);
    if contains_upper(d, b"SPLIT DUPLEX MODEM") {
        RdofRadioKind::Split // RDOF_SPLIT_DESCRIPTION
    } else if is_radio_hotspot(d) {
        RdofRadioKind::Hotspot
    } else if HS.iter().any(|h| contains_upper(d, h)) {
        RdofRadioKind::MmdvmHs
    } else if contains_upper(d, b"MMDVM") {
        RdofRadioKind::Mmdvm
    } else {
        RdofRadioKind::Unknown
    }
}

const KIND_NAMES: [(RdofRadioKind, &core::ffi::CStr); 4] = [
    (RdofRadioKind::Hotspot, c"radio_hotspot"),
    (RdofRadioKind::MmdvmHs, c"mmdvm_hs"),
    (RdofRadioKind::Mmdvm, c"mmdvm"),
    (RdofRadioKind::Split, c"split"),
];

/// Takes the C's int: an enum's value the C passes need not be one of its names.
#[no_mangle]
pub extern "C" fn rdof_radio_kind_name(k: c_int) -> *const c_char {
    KIND_NAMES.iter().find(|(kind, _)| *kind as c_int == k).map_or(c"unknown", |(_, n)| n).as_ptr()
}

#[no_mangle]
pub unsafe extern "C" fn rdof_radio_kind_of(t: *const c_char) -> RdofRadioKind {
    let t = c_bytes(t);
    if t == b"opengd77" {
        return RdofRadioKind::Hotspot;
    }
    KIND_NAMES.iter().find(|(_, n)| n.to_bytes() == t).map_or(RdofRadioKind::Unknown, |&(k, _)| k)
}

fn word_char(c: u8) -> bool {
    c.is_ascii_alphanumeric() || c == b'_'
}

/// The description in upper case, cut to 255 bytes as the C's buffer.
fn upper(description: *const c_char, d: &mut [u8; 255]) -> &[u8] {
    let s = unsafe { c_bytes(description) };
    let n = s.len().min(d.len());
    for (o, c) in d.iter_mut().zip(&s[..n]) {
        *o = c.to_ascii_uppercase();
    }
    &d[..n]
}

/// "TS<a>\s*>\s*TS<b>\b" at d[i], with a word boundary before: (a, b, the length matched).
fn ts_pair(d: &[u8], i: usize) -> Option<(c_int, c_int, usize)> {
    let at = |k: usize| d.get(k).copied().unwrap_or(0);
    let slot = |k: usize| {
        (at(k) == b'T' && at(k + 1) == b'S' && matches!(at(k + 2), b'1' | b'2')).then(|| c_int::from(at(k + 2) - b'0'))
    };
    if i > 0 && word_char(d[i - 1]) {
        return None;
    }
    let a = slot(i)?;
    let mut k = i + 3;
    while py_space(at(k)) {
        k += 1;
    }
    if at(k) != b'>' {
        return None;
    }
    k += 1;
    while py_space(at(k)) {
        k += 1;
    }
    let b = slot(k)?;
    if word_char(at(k + 3)) {
        return None;
    }
    Some((a, b, k + 3 - i))
}

/// The first whole word w in d at or after `from`.
fn find_word(d: &[u8], w: &[u8], from: usize) -> Option<usize> {
    (from..=d.len().saturating_sub(w.len())).find(|&i| {
        d[i..].starts_with(w)
            && (i == 0 || !word_char(d[i - 1]))
            && !word_char(d.get(i + w.len()).copied().unwrap_or(0))
    })
}

#[no_mangle]
pub unsafe extern "C" fn rdof_sfr_slots(description: *const c_char, inbound: &mut c_int, outbound: &mut c_int) -> bool {
    let mut buf = [0; 255];
    let d = upper(description, &mut buf);
    if !is_radio_hotspot(d) || find_word(d, b"SFR", 0).is_none() {
        return false;
    }
    let (a, b) = (0..d.len()).find_map(|i| ts_pair(d, i)).map_or((1, 2), |(a, b, _)| (a, b));
    *inbound = a;
    *outbound = b;
    true
}

#[no_mangle]
pub unsafe extern "C" fn rdof_dcdm_capable(description: *const c_char) -> bool {
    let mut buf = [0; 255];
    find_word(upper(description, &mut buf), b"DCDM", 0).is_some()
}

/// "\bSFR\s+TS[12]\s*>\s*TS[12]\s+([A-Z]+)\b": the framing word, at its first match.
fn sfr_framing(d: &[u8]) -> Option<&[u8]> {
    let at = |k: usize| d.get(k).copied().unwrap_or(0);
    let mut from = 0;
    while let Some(p) = find_word(d, b"SFR", from) {
        from = p + 1;
        let mut q = p + 3;
        if !py_space(at(q)) {
            continue;
        }
        while py_space(at(q)) {
            q += 1;
        }
        let Some((_, _, n)) = ts_pair(d, q) else { continue };
        q += n;
        if !py_space(at(q)) {
            continue;
        }
        while py_space(at(q)) {
            q += 1;
        }
        let k = d[q..].iter().take_while(|c| c.is_ascii_uppercase()).count();
        if k > 0 && !word_char(at(q + k)) {
            return Some(&d[q..q + k]);
        }
    }
    None
}

#[no_mangle]
pub unsafe extern "C" fn rdof_sfr_framing(description: *const c_char, out: *mut c_char, cap: usize) {
    let mut buf = [0; 255];
    let d = upper(description, &mut buf);
    c_out(out, cap, |o| o.bytes(sfr_framing(d).unwrap_or(&[])));
}

#[no_mangle]
pub extern "C" fn rdof_radio_hotspot_dbm(raw: c_int, freq_hz: u32) -> c_int {
    let raw = i64::from(raw);
    let v = if freq_hz >= 300_000_000 { -151 + raw } else { -164 + (raw * 32).div_euclid(27) };
    v.clamp(c_int::MIN.into(), c_int::MAX.into()) as c_int
}

// ── burst regeneration ──

/// A voice superframe's LC fragments by burst B-F (1-5): bursts._LCSS.get(n, 0).
fn lcss_of(n: c_int) -> c_int {
    match n {
        1 => 1,
        2 | 3 => 3,
        4 => 2,
        _ => 0,
    }
}

fn put_emb(cc: c_int, lcss: c_int, pi: c_int, out: &mut Burst) {
    let mut e = [0u8; 16];
    rdof_emb_bits(cc, lcss, pi, &mut e);
    set_field(out, 108, 8, bits_to_u64(&e[..8]));
    set_field(out, 148, 8, bits_to_u64(&e[8..]));
}

/// burst and out may be the same buffer.
#[no_mangle]
pub unsafe extern "C" fn rdof_regenerate(control: u8, burst: *const Burst, cc: c_int, bs: bool, out: *mut Burst) {
    let burst = &*burst;
    let voice = if bs { RDOF_BS_VOICE } else { RDOF_MS_VOICE };
    let mut b = *burst;
    regenerate(control, &mut b, cc, voice);
    *out = b;
}

/// The burst made what the modem wants: the sync (and slot type) for its family, or a voice burst's EMB for cc.
fn regenerate(control: u8, out: &mut Burst, cc: c_int, voice: u64) {
    let burst = *out;
    if control & RDOF_SYNC_DATA != 0 {
        let mut st = [0u8; 20];
        rdof_slot_type_bits(cc, c_int::from(control & 0x0F), &mut st);
        set_field(out, 98, 10, bits_to_u64(&st[..10]));
        set_field(out, 108, 48, voice ^ RDOF_DATA_XOR);
        set_field(out, 156, 10, bits_to_u64(&st[10..]));
    } else if control & SYNC_AUDIO != 0 {
        set_field(out, 108, 48, voice);
    } else {
        let (mut old_cc, mut pi, mut lcss) = (0, 0, 0);
        if !rdof_decode_emb(&burst, Some(&mut old_cc), Some(&mut pi), Some(&mut lcss), None) {
            pi = 0;
            lcss = lcss_of(c_int::from(control & 0x0F));
        }
        put_emb(cc, lcss, pi, out);
    }
}

/// The on-air AMBE+2 silence frame (72 bits, FEC coded), three to a silence burst.
const AMBE_SILENCE: [u8; 9] = [0xb9, 0xe8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2a, 0x6b];

#[no_mangle]
pub unsafe extern "C" fn rdof_silence_burst(n: c_int, cc: c_int, bs: bool, embedded32: *const u8, out: &mut Burst) -> u8 {
    // three frames over bits 0-107 and 156-263; the 48 bits between are the sync or EMB + embedded field
    let frames: [u8; 27] = core::array::from_fn(|i| AMBE_SILENCE[i % 9]);
    *out = [0; RDOF_BURST_BYTES];
    set_field(out, 0, 64, crate::bits::get_field(&frames, 0, 64));
    set_field(out, 64, 44, crate::bits::get_field(&frames, 64, 44));
    for k in 0..3 {
        let at = 108 + 36 * k;
        set_field(out, 156 + 36 * k, 36, crate::bits::get_field(&frames, at, 36));
    }
    if n == 0 {
        set_field(out, 108, 48, if bs { RDOF_BS_VOICE } else { RDOF_MS_VOICE });
        return SYNC_AUDIO;
    }
    if !embedded32.is_null() {
        let e = slice(embedded32, 32).iter().fold(0, |v, &b| v << 1 | u64::from(b != 0)); // any nonzero is a 1
        set_field(out, 116, 32, e);
    }
    put_emb(cc, lcss_of(n), 0, out);
    n as u8
}

#[no_mangle]
pub unsafe extern "C" fn rdof_csbk_preambles(src: u32, dst: u32, group: bool, n: c_int, cc: c_int, out: *mut Burst) {
    let n = n.max(0) as usize;
    let out = if n == 0 { &mut [][..] } else { core::slice::from_raw_parts_mut(out, n) };
    for (i, burst) in out.iter_mut().enumerate() {
        let mut info = [0u8; 12];
        info[0] = 0x80 | CSBK_PREAMBLE;
        info[2] = if group { 0x40 } else { 0 };
        info[3] = (n - i) as u8;
        info[4..7].copy_from_slice(&dst.to_be_bytes()[1..]);
        info[7..10].copy_from_slice(&src.to_be_bytes()[1..]);
        let crc = ffi::rdof_crc16_ccitt(info.as_ptr(), 10) ^ RDOF_MASK_CSBK;
        info[10..].copy_from_slice(&crc.to_be_bytes());
        rdof_data_burst(&info, RDOF_DT_CSBK, cc, RDOF_MS_VOICE ^ RDOF_DATA_XOR, burst);
    }
}

#[no_mangle]
pub extern "C" fn rdof_control_to_dt(control: u8, dt: &mut c_int, n: &mut c_int) {
    (*dt, *n) = if control & RDOF_SYNC_DATA != 0 {
        (c_int::from(control & 0x0F), 0)
    } else if control & SYNC_AUDIO != 0 {
        (RDOF_DT_VOICE_SYNC, 0)
    } else {
        (RDOF_DT_VOICE, c_int::from(control & 0x0F))
    };
}
