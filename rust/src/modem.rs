// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! The MMDVM modem and the radio on it (rdof/modem.h). A port of common/src/modem.c, without threads or tuners other than the modem's own
//! and fixed.
//!
//! An rdof_modem_t is shared by two tasks: the reader feeds it bytes (rdof_modem_feed) while the caller waits in a
//! request. So the functions here work through the raw pointer and never make a `&mut` of the whole struct, and
//! `waiting_for` and `reply_ready` (volatile in the C) are read and written volatile. The port's wait_reply /
//! on_reply (a semaphore) is what orders the reply's bytes against reply_ready, as in the C.

use core::ffi::{c_char, c_int, c_void};
use core::ptr::{addr_of, addr_of_mut, read_volatile, write_volatile};

use crate::bits::slice;
use crate::ffi::{rint, RDOF_BURST_BYTES};
use crate::mmdvm::{
    rdof_mmdvm_config_v1, rdof_mmdvm_config_v2, rdof_mmdvm_frame, rdof_mmdvm_parse_status, rdof_mmdvm_parse_version,
    rdof_mmdvm_reader_feed, rdof_mmdvm_reader_init, rdof_mmdvm_set_freq_payload, rdof_mmdvm_unanswered, rdof_modem_config_default,
    rdof_radio_hotspot_dbm, RdofMmdvmReader, RdofMmdvmStatus, RdofModemConfig,
};
use crate::text::{c_bytes, c_out, fixed, Out};

const GET_VERSION: u8 = 0x00;
const GET_STATUS: u8 = 0x01;
const SET_CONFIG: u8 = 0x02;
const SET_MODE: u8 = 0x03;
const SET_FREQ: u8 = 0x04;
const SEND_CWID: u8 = 0x0A;
const DMR_DATA1: u8 = 0x18;
const DMR_LOST1: u8 = 0x19;
const DMR_DATA2: u8 = 0x1A;
const DMR_LOST2: u8 = 0x1B;
const DMR_SHORTLC: u8 = 0x1C;
const DMR_START: u8 = 0x1D;
const DMR_ABORT: u8 = 0x1E;
const ACK: u8 = 0x70;
const NAK: u8 = 0x7F;
const MODE_DMR: c_int = 2;
const DCDM: u8 = 0x40; // RDOF_MMDVM_DCDM

const RADIO_HOTSPOT: c_int = 1;
const RADIO_MMDVM: c_int = 3;
const RADIO_SPLIT: c_int = 4;
const TUNER_MODEM: c_int = 0;
const TUNER_FIXED: c_int = 1;

const RSSI_POINTS: usize = 16;

/// rdof_modem_port_t
#[repr(C)]
pub struct RdofModemPort {
    pub write: Option<unsafe extern "C" fn(ctx: *mut c_void, data: *const u8, len: usize) -> bool>,
    pub wait_reply: Option<unsafe extern "C" fn(ctx: *mut c_void, timeout_ms: c_int) -> bool>,
    pub on_reply: Option<unsafe extern "C" fn(ctx: *mut c_void)>,
    pub clock: Option<unsafe extern "C" fn(ctx: *mut c_void) -> f64>,
    pub sleep_ms: Option<unsafe extern "C" fn(ctx: *mut c_void, ms: c_int)>,
    pub ctx: *mut c_void,
}

/// rdof_modem_t
#[repr(C)]
pub struct RdofModem {
    pub port: RdofModemPort,
    pub protocol: c_int,
    pub version: [c_char; 128],
    pub last_rx: f64,
    pub failed: [c_char; 64],
    pub error: [c_char; 96],
    pub timeout_ms: c_int,
    pub requests: u32,
    pub no_reply: u32,
    pub refused: u32,
    pub waiting_for: c_int,
    pub reply_ready: bool,
    pub reply_ok: bool,
    pub reply: [u8; 256],
    pub reply_len: usize,
    pub reader: RdofMmdvmReader,
    pub on_rx: Option<unsafe extern "C" fn(ctx: *mut c_void, slot: c_int, control: u8, burst: *const u8, rssi: c_int)>,
    pub on_lost: Option<unsafe extern "C" fn(ctx: *mut c_void, slot: c_int)>,
    pub on_nak: Option<unsafe extern "C" fn(ctx: *mut c_void, command: u8, err: c_int)>,
    pub cb_ctx: *mut c_void,
}

/// rdof_rssi_map_t
#[repr(C)]
pub struct RdofRssiMap {
    pub n: c_int,
    pub raw: [c_int; RSSI_POINTS],
    pub dbm: [c_int; RSSI_POINTS],
}

/// rdof_radio_t
#[repr(C)]
pub struct RdofRadio {
    pub modem: *mut RdofModem,
    pub kind: c_int,
    pub config: RdofModemConfig,
    pub tuner: c_int,
    pub rx_offset_hz: i32,
    pub tx_offset_hz: i32,
    pub fixed_hz: u32,
    pub rssi_map: RdofRssiMap,
    pub hotspot_rssi: bool,
    pub headers: c_int,
    pub terminators: c_int,
    pub settle_ms: c_int,
}

unsafe fn waiting_for(m: *const RdofModem) -> c_int {
    read_volatile(addr_of!((*m).waiting_for))
}

unsafe fn set_waiting_for(m: *mut RdofModem, v: c_int) {
    write_volatile(addr_of_mut!((*m).waiting_for), v)
}

unsafe fn reply_ready(m: *const RdofModem) -> bool {
    read_volatile(addr_of!((*m).reply_ready))
}

unsafe fn set_reply_ready(m: *mut RdofModem, v: bool) {
    write_volatile(addr_of_mut!((*m).reply_ready), v)
}

unsafe fn failed(m: *const RdofModem) -> bool {
    (*m).failed[0] != 0
}

/// snprintf(m->error, sizeof m->error, ...)
unsafe fn set_error(m: *mut RdofModem, f: impl FnOnce(&mut Out)) {
    let e = addr_of_mut!((*m).error);
    c_out(e.cast(), (*e).len(), f);
}

/// The error is the failure's.
unsafe fn error_failed(m: *mut RdofModem) {
    let why = c_bytes(addr_of!((*m).failed).cast());
    set_error(m, |o| o.bytes(why));
}

unsafe fn now_s(m: *const RdofModem) -> f64 {
    match (*m).port.clock {
        Some(clock) => clock((*m).port.ctx),
        None => 0.0,
    }
}

unsafe fn reply_in(m: *mut RdofModem) {
    set_reply_ready(m, true);
    if let Some(on_reply) = (*m).port.on_reply {
        on_reply((*m).port.ctx);
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_modem_init(m: *mut RdofModem, port: *const RdofModemPort) {
    m.write_bytes(0, 1);
    core::ptr::copy_nonoverlapping(port, addr_of_mut!((*m).port), 1);
    (*m).protocol = 1;
    (*m).last_rx = f64::NEG_INFINITY;
    (*m).timeout_ms = 1000;
    set_waiting_for(m, -1);
    rdof_mmdvm_reader_init(&mut *addr_of_mut!((*m).reader));
}

#[no_mangle]
pub unsafe extern "C" fn rdof_modem_fail(m: *mut RdofModem, why: *const c_char) {
    let why = c_bytes(why);
    let f = addr_of_mut!((*m).failed);
    c_out(f.cast(), (*f).len(), |o| o.bytes(why));
    if waiting_for(m) >= 0 {
        (*m).reply_ok = false;
        reply_in(m);
    }
}

unsafe extern "C" fn on_frame(ctx: *mut c_void, cmd: u8, p: *const u8, len: usize) {
    let m = ctx.cast::<RdofModem>();
    let mut p = slice(p, len);
    if matches!(cmd, DMR_DATA1 | DMR_LOST1 | DMR_DATA2 | DMR_LOST2) {
        (*m).last_rx = now_s(m);
        let slot = if matches!(cmd, DMR_DATA1 | DMR_LOST1) { 1 } else { 2 };
        if matches!(cmd, DMR_LOST1 | DMR_LOST2) {
            if let Some(on_lost) = (*m).on_lost {
                on_lost((*m).cb_ctx, slot);
            }
        } else if p.len() > RDOF_BURST_BYTES {
            let rssi = match p.get(34..36) {
                Some(&[hi, lo]) => c_int::from(u16::from_be_bytes([hi, lo])),
                _ => -1,
            };
            if let Some(on_rx) = (*m).on_rx {
                on_rx((*m).cb_ctx, slot, p[0], p[1..].as_ptr(), rssi);
            }
        }
        return;
    }
    let (key, ok) = if cmd == ACK || cmd == NAK {
        let Some((&key, rest)) = p.split_first() else {
            return;
        };
        if rdof_mmdvm_unanswered(key) {
            if cmd == NAK {
                if let Some(on_nak) = (*m).on_nak {
                    on_nak((*m).cb_ctx, key, rest.first().map_or(0, |&e| e.into()));
                }
            }
            return;
        }
        p = rest;
        (c_int::from(key), cmd == ACK)
    } else {
        (c_int::from(cmd), true)
    };
    if key != waiting_for(m) {
        return; // nobody's waiting for it (a reply that came too late)
    }
    let reply = &mut *addr_of_mut!((*m).reply);
    let n = p.len().min(reply.len());
    reply[..n].copy_from_slice(&p[..n]);
    (*m).reply_len = n;
    (*m).reply_ok = ok;
    reply_in(m);
}

#[no_mangle]
pub unsafe extern "C" fn rdof_modem_feed(m: *mut RdofModem, data: *const u8, len: usize) {
    rdof_mmdvm_reader_feed(&mut *addr_of_mut!((*m).reader), data, len, Some(on_frame), m.cast());
}

unsafe fn write_frame(m: *mut RdofModem, cmd: u8, payload: &[u8]) -> bool {
    let mut buf = [0u8; 260];
    if failed(m) {
        error_failed(m);
        return false;
    }
    let n = rdof_mmdvm_frame(cmd, payload.as_ptr(), payload.len(), buf.as_mut_ptr(), buf.len());
    let wrote = n != 0 && (*m).port.write.is_some_and(|write| write((*m).port.ctx, buf.as_ptr(), n));
    if !wrote {
        if !failed(m) {
            rdof_modem_fail(m, c"port write failed".as_ptr());
        }
        error_failed(m);
        return false;
    }
    true
}

unsafe fn request(m: *mut RdofModem, cmd: u8, payload: &[u8]) -> bool {
    set_reply_ready(m, false);
    set_waiting_for(m, cmd.into());
    (*m).requests = (*m).requests.wrapping_add(1);
    if !write_frame(m, cmd, payload) {
        set_waiting_for(m, -1);
        return false;
    }
    let came = reply_ready(m)
        || (*m).port.wait_reply.is_some_and(|wait| wait((*m).port.ctx, (*m).timeout_ms))
        || reply_ready(m);
    set_waiting_for(m, -1);
    if failed(m) {
        error_failed(m);
        return false;
    }
    if !came {
        (*m).no_reply = (*m).no_reply.wrapping_add(1);
        set_error(m, |o| {
            o.bytes(b"no reply to command 0x");
            hex2(o, cmd);
        });
        return false;
    }
    if !(*m).reply_ok {
        (*m).refused = (*m).refused.wrapping_add(1);
        let err = if (*m).reply_len != 0 { i64::from((*m).reply[0]) } else { -1 };
        set_error(m, |o| {
            o.bytes(b"modem refused command 0x");
            hex2(o, cmd);
            o.bytes(b" (error ");
            o.int(err, 0);
            o.bytes(b")");
        });
        return false;
    }
    true
}

/// %02x
fn hex2(o: &mut Out, v: u8) {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    o.byte(HEX[usize::from(v >> 4)]);
    o.byte(HEX[usize::from(v & 15)]);
}

/// The reply's bytes.
unsafe fn reply<'a>(m: *const RdofModem) -> &'a [u8] {
    let r = &*addr_of!((*m).reply);
    &r[..(*m).reply_len.min(r.len())]
}

#[no_mangle]
pub unsafe extern "C" fn rdof_modem_start(m: *mut RdofModem, tries: c_int) -> bool {
    for _ in 0..tries {
        if request(m, GET_VERSION, &[]) {
            let r = reply(m);
            let v = addr_of_mut!((*m).version);
            (*m).protocol = rdof_mmdvm_parse_version(r.as_ptr(), r.len(), v.cast(), (*v).len());
            return true;
        }
        if failed(m) {
            return false;
        }
    }
    false
}

#[no_mangle]
pub unsafe extern "C" fn rdof_modem_status(m: *mut RdofModem, out: &mut RdofMmdvmStatus) -> bool {
    if !request(m, GET_STATUS, &[]) {
        return false;
    }
    let r = reply(m);
    if !rdof_mmdvm_parse_status((*m).protocol, r.as_ptr(), r.len(), out) {
        set_error(m, |o| o.bytes(b"short status reply"));
        return false;
    }
    true
}

#[no_mangle]
pub unsafe extern "C" fn rdof_modem_configure(m: *mut RdofModem, c: &RdofModemConfig, cc: c_int, duplex: bool) -> bool {
    configure(m, c, cc, duplex, 0)
}

/// SET_CONFIG with extra flags ORed into its flags byte (RDOF_MMDVM_DCDM).
unsafe fn configure(m: *mut RdofModem, c: &RdofModemConfig, cc: c_int, duplex: bool, extra: u8) -> bool {
    let mut p = [0u8; 37];
    let n = if (*m).protocol >= 2 {
        rdof_mmdvm_config_v2(c, cc, MODE_DMR, duplex, &mut p)
    } else {
        rdof_mmdvm_config_v1(c, cc, MODE_DMR, duplex, (&mut p[..23]).try_into().unwrap())
    };
    p[0] |= extra;
    request(m, SET_CONFIG, &p[..n])
}

#[no_mangle]
pub unsafe extern "C" fn rdof_modem_set_freq(m: *mut RdofModem, rx_hz: u32, tx_hz: u32, rf_level: c_int) -> bool {
    let mut p = [0u8; 14];
    let n = rdof_mmdvm_set_freq_payload(rx_hz, tx_hz, rf_level, &mut p);
    request(m, SET_FREQ, &p[..n])
}

#[no_mangle]
pub unsafe extern "C" fn rdof_modem_set_mode(m: *mut RdofModem, mode: c_int) -> bool {
    request(m, SET_MODE, &[mode as u8])
}

#[no_mangle]
pub unsafe extern "C" fn rdof_modem_send_cwid(m: *mut RdofModem, text: *const c_char) -> bool {
    let mut p = [0u8; 40];
    let mut n = 0;
    for &c in c_bytes(text) {
        if n == p.len() {
            break;
        }
        if c < 0x80 {
            p[n] = c.to_ascii_uppercase();
            n += 1;
        }
    }
    request(m, SEND_CWID, &p[..n])
}

#[no_mangle]
pub unsafe extern "C" fn rdof_modem_write_dmr(m: *mut RdofModem, control: u8, burst: *const u8, slot: c_int) -> bool {
    let mut p = [0u8; 1 + RDOF_BURST_BYTES];
    p[0] = control;
    p[1..].copy_from_slice(slice(burst, RDOF_BURST_BYTES));
    write_frame(m, if slot == 1 { DMR_DATA1 } else { DMR_DATA2 }, &p)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_modem_dmr_start(m: *mut RdofModem, on: bool) -> bool {
    write_frame(m, DMR_START, &[u8::from(on)])
}

#[no_mangle]
pub unsafe extern "C" fn rdof_modem_dmr_abort(m: *mut RdofModem, slot: c_int) -> bool {
    write_frame(m, DMR_ABORT, &[slot as u8])
}

#[no_mangle]
pub unsafe extern "C" fn rdof_modem_dmr_short_lc(m: *mut RdofModem, lc9: *const u8) -> bool {
    write_frame(m, DMR_SHORTLC, slice(lc9, 9))
}

// ── the radio ──

#[no_mangle]
pub unsafe extern "C" fn rdof_radio_init(r: *mut RdofRadio, modem: *mut RdofModem, kind: c_int) {
    r.write_bytes(0, 1);
    let r = &mut *r;
    r.modem = modem;
    r.kind = kind;
    rdof_modem_config_default(&mut r.config);
    r.tuner = if kind == RADIO_MMDVM { TUNER_FIXED } else { TUNER_MODEM };
    r.hotspot_rssi = kind == RADIO_HOTSPOT || kind == RADIO_SPLIT; // a split's RX half: a radio
    r.headers = if kind == RADIO_HOTSPOT { 1 } else { 8 };
    r.terminators = if kind == RADIO_HOTSPOT { 1 } else { 3 };
}

impl RdofRssiMap {
    /// The points in use: n, within the arrays.
    fn len(&self) -> usize {
        (self.n.max(0) as usize).min(RSSI_POINTS)
    }
}

#[no_mangle]
pub extern "C" fn rdof_rssi_map_sort(map: &mut RdofRssiMap) {
    let n = map.len();
    for i in 1..n {
        let mut j = i;
        while j > 0 && (map.raw[j - 1], map.dbm[j - 1]) > (map.raw[j], map.dbm[j]) {
            map.raw.swap(j, j - 1);
            map.dbm.swap(j, j - 1);
            j -= 1;
        }
    }
}

#[no_mangle]
pub extern "C" fn rdof_rssi_map_dbm(map: &RdofRssiMap, raw: c_int) -> c_int {
    let n = map.len();
    let i = map.raw[..n].iter().position(|&r| r >= raw).unwrap_or(n); // bisect_left on (raw, -inf)
    if i == 0 {
        return map.dbm[0];
    }
    if i >= n {
        return map.dbm[n - 1];
    }
    let (r0, d0, r1, d1) = (i64::from(map.raw[i - 1]), map.dbm[i - 1], i64::from(map.raw[i]), map.dbm[i]);
    let span = (r1 - r0).max(1);
    let v = f64::from(d0) + (f64::from(d1) - f64::from(d0)) * (i64::from(raw) - r0) as f64 / span as f64;
    unsafe { rint(v) as c_int }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_radio_rssi(r: &RdofRadio, raw: c_int, freq: u32, dbm: &mut c_int) -> bool {
    if raw < 0 {
        return false;
    }
    if r.rssi_map.n != 0 {
        *dbm = rdof_rssi_map_dbm(&r.rssi_map, raw);
        return true;
    }
    if r.hotspot_rssi {
        *dbm = rdof_radio_hotspot_dbm(raw, freq);
        return true;
    }
    false
}

unsafe fn tune_split(r: &RdofRadio, rx: u32, tx: u32, power: c_int) -> bool {
    let m = r.modem;
    match r.tuner {
        TUNER_MODEM => rdof_modem_set_freq(
            m,
            (i64::from(rx) + i64::from(r.rx_offset_hz)) as u32,
            (i64::from(tx) + i64::from(r.tx_offset_hz)) as u32,
            power,
        ),
        TUNER_FIXED => {
            let d = i64::from(tx) - i64::from(r.fixed_hz);
            if !(-100..=100).contains(&d) {
                let mut buf = [0u8; 64];
                let mhz = fixed(f64::from(r.fixed_hz) / 1e6, 5, &mut buf);
                set_error(m, |o| {
                    o.bytes(b"this radio is fixed on ");
                    o.bytes(mhz);
                    o.bytes(b" MHz");
                });
                return false;
            }
            true
        }
        _ => {
            set_error(m, |o| o.bytes(b"no tuner"));
            false
        }
    }
}

unsafe fn tune(r: &RdofRadio, rx: u32, tx: u32, cc: c_int, power: c_int, duplex: bool) -> bool {
    tune_flags(r, rx, tx, cc, power, duplex, 0)
}

unsafe fn tune_flags(r: &RdofRadio, rx: u32, tx: u32, cc: c_int, power: c_int, duplex: bool, extra: u8) -> bool {
    if !tune_split(r, rx, tx, power) {
        return false;
    }
    let m = r.modem;
    if r.settle_ms != 0 {
        if let Some(sleep_ms) = (*m).port.sleep_ms {
            sleep_ms((*m).port.ctx, r.settle_ms);
        }
    }
    configure(m, &r.config, cc, duplex, extra) && rdof_modem_set_mode(m, MODE_DMR)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_radio_tune(r: &RdofRadio, freq: u32, cc: c_int, power: c_int) -> bool {
    tune(r, freq, freq, cc, power, false)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_radio_tune_duplex(r: &RdofRadio, rx: u32, tx: u32, cc: c_int, power: c_int) -> bool {
    tune(r, rx, tx, cc, power, true)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_radio_tune_dcdm(r: &RdofRadio, freq: u32, cc: c_int, power: c_int) -> bool {
    tune_flags(r, freq, freq, cc, power, false, DCDM)
}
