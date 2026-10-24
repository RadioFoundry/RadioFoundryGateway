// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! tracker.c: one channel's call tracker (rdof/tracker.h), and the timestamps.

use core::ffi::{c_char, c_int, c_long, c_void, CStr};

use crate::ambe::rdof_ambe_decode_frame;
use crate::bits::rdof_be24;
use crate::ffi::{self, RDOF_BURST_BYTES};
use crate::ffi::{
    RDOF_DT_CSBK, RDOF_DT_DATA_HEADER, RDOF_DT_RATE_1, RDOF_DT_RATE_12, RDOF_DT_RATE_34, RDOF_DT_TERMINATOR_WITH_LC,
    RDOF_DT_VOICE, RDOF_DT_VOICE_LC_HEADER, RDOF_DT_VOICE_SYNC,
};
use crate::json::RdofJson;
use crate::layer2::{rdof_family_voice_sync, rdof_group_lc, rdof_lc_burst, RDOF_DATA_XOR};
use crate::mmdvm::rdof_control_to_dt;
use crate::pdu::{
    rdof_decode_csbk, rdof_decode_data_header, rdof_decode_voice_lc, rdof_parse_full_lc, RdofCsbk, RdofDataHeader, RdofFullLc,
};
use crate::rawburst::{rdof_rawburst_json, RdofRawburst};
use crate::text::{c_bytes, c_out, Out};

type Burst = [u8; RDOF_BURST_BYTES];

const VOICE_LOST_SECONDS: f64 = 0.5;
const DATA_LOST_SECONDS: f64 = 2.0;
const IDLE_HANG_SECONDS: f64 = 3.0;
const METRIC_SECONDS: f64 = 1.0;
const PREAMBLE_REPEAT_SECONDS: f64 = 1.0;
const REPEAT_TERMINATOR_SECONDS: f64 = 1.0;
const JSON_MAX: usize = 640;
const MAX_SINKS: usize = 4;

const RDOF_DT_IDLE: c_int = 9;
const RDOF_FAMILY_MS: u32 = 1;
const RDOF_NONE: i32 = -1;
const RDOF_FLCO_GROUP: c_int = 0x00;
const RDOF_FLCO_PRIVATE: c_int = 0x03;
const RDOF_SO_EMERGENCY: c_int = 0x80;
const RDOF_DPF_PROPRIETARY: c_int = 15;
const CSBK_BSDWNACT: c_int = 0x38;

const CALL_VOICE: c_int = 0;
const CALL_DATA: c_int = 1;

const MODE_DMR: c_int = 1;
const MODE_IDLE: c_int = 2;

/// rdof_burst_t, field for field.
#[repr(C)]
pub struct RdofBurst {
    pub burst: Burst,
    pub dt: c_int,
    pub n: c_int,
    pub slot: c_int,
    pub family: u32,
    pub cc: c_int,
    pub sync_errors: c_int,
    pub power_dbfs: f64,
    pub utc: bool,
    pub utc_slot: i64,
    pub utc_us: i64,
}

/// rdof_frame_t, field for field.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RdofFrame {
    pub slot: c_int,
    pub dt: c_int,
    pub n: c_int,
    pub burst: Burst,
    pub has_addr: bool,
    pub src: u32,
    pub dst: u32,
    pub group: bool,
    pub new_stream: bool,
    pub synthetic: bool,
    pub errors: c_int,
    pub rssi: c_int,
}

type TextCb = Option<unsafe extern "C" fn(ctx: *mut c_void, channel: *const c_char, json: *const c_char)>;

/// rdof_sink_t, field for field.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RdofSink {
    pub on_json: TextCb,
    pub on_raw: TextCb,
    pub on_frame: Option<unsafe extern "C" fn(ctx: *mut c_void, channel: *const c_char, f: *const RdofFrame)>,
    pub ctx: *mut c_void,
    pub on_call: Option<
        unsafe extern "C" fn(ctx: *mut c_void, channel: *const c_char, action: *const c_char, src: u32, dst: u32),
    >,
    pub on_raw_burst: Option<unsafe extern "C" fn(ctx: *mut c_void, channel: *const c_char, r: *const RdofRawburst)>,
}

/// rdof_call_t, field for field.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RdofCall {
    pub active: bool,
    pub kind: c_int,
    pub src: u32,
    pub dst: u32,
    pub group: bool,
    pub start: f64,
    pub last: f64,
    pub cc: c_int,
    pub family: u32,
    pub bits: c_long,
    pub errs: c_long,
    pub rssi_n: c_int,
    pub rssi_sum: c_long,
    pub rssi_min: c_int,
    pub rssi_max: c_int,
    pub blocks_left: c_int,
    pub metrics_at: f64,
    pub m_bits: c_long,
    pub m_errs: c_long,
    pub has_lc: bool,
    pub lc: [u8; 9],
    pub emergency: bool,
}

/// rdof_slot_state_t, field for field.
#[repr(C)]
pub struct RdofSlotState {
    pub call: RdofCall,
    pub frags: [[u8; 32]; 5],
    pub have_frag: u8,
    pub has_preamble: bool,
    pub preamble_src: u32,
    pub preamble_dst: u32,
    pub preamble_at: f64,
    pub has_ended: bool,
    pub ended_src: u32,
    pub ended_dst: u32,
    pub ended_at: f64,
}

/// rdof_tracker_t, field for field.
#[repr(C)]
pub struct RdofTracker {
    pub channel: [c_char; 24],
    pub sinks: [RdofSink; MAX_SINKS],
    pub nsinks: c_int,
    pub rssi_offset: f64,
    pub freq_hz: f64,
    pub slots: [RdofSlotState; 2],
    pub mode: c_int,
    pub last_activity: f64,
}

// ── time ──

/// (long long) of a double, saturating, NaN 0: the C's sat_ll.
fn to_i64(v: f64) -> i64 {
    v as i64
}

// Howard Hinnant's civil_from_days (public domain algorithm).
fn civil_from_days(z: i64) -> (i64, i64, i64) {
    let z = z + 719468;
    let era = (if z >= 0 { z } else { z - 146096 }) / 146097;
    let doe = (z - era * 146097) as u32;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let yy = i64::from(yoe) + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    (yy + i64::from(m <= 2), i64::from(m), i64::from(d))
}

#[no_mangle]
pub extern "C" fn rdof_time_ms(t: f64) -> i64 {
    // As datetime.fromtimestamp (microseconds rounded half even) for the seconds, then int(t * 1000) % 1000 for
    // the ms: what tracker.iso(t) writes.
    let mut ip = 0.0;
    let frac = unsafe { ffi::modf(t, &mut ip) };
    let us = unsafe { ffi::rint(frac * 1e6) };
    let mut secs = to_i64(ip);
    if us >= 1_000_000.0 {
        secs = secs.wrapping_add(1);
    } else if us < 0.0 {
        secs = secs.wrapping_sub(1);
    }
    let ms = to_i64(t * 1000.0) % 1000;
    secs.wrapping_mul(1000).wrapping_add(if ms < 0 { ms + 1000 } else { ms })
}

#[no_mangle]
pub unsafe extern "C" fn rdof_iso_ms(ms_total: i64, out: *mut c_char) {
    let (secs, ms) = (ms_total.div_euclid(1000), ms_total.rem_euclid(1000));
    let (days, rem) = (secs.div_euclid(86400), secs.rem_euclid(86400));
    let (y, mo, d) = civil_from_days(days);
    c_out(out, 32, |o| {
        o.int(y, 4);
        o.byte(b'-');
        o.int(mo, 2);
        o.byte(b'-');
        o.int(d, 2);
        o.byte(b'T');
        o.int(rem / 3600, 2);
        o.byte(b':');
        o.int(rem / 60 % 60, 2);
        o.byte(b':');
        o.int(rem % 60, 2);
        o.byte(b'.');
        o.int(ms, 3);
        o.byte(b'Z');
    });
}

#[no_mangle]
pub unsafe extern "C" fn rdof_iso_time(t: f64, out: *mut c_char) {
    rdof_iso_ms(rdof_time_ms(t), out);
}

/// rdof_iso_time's text.
fn iso(t: f64) -> [c_char; 32] {
    let mut ts = [0; 32];
    unsafe { rdof_iso_time(t, ts.as_mut_ptr()) };
    ts
}

// ── setup ──

#[no_mangle]
pub unsafe extern "C" fn rdof_tracker_init(tr: *mut RdofTracker, channel: *const c_char, rssi_offset: f64, freq_hz: f64) {
    tr.write_bytes(0, 1);
    let tr = &mut *tr;
    let ch = c_bytes(channel);
    c_out(tr.channel.as_mut_ptr(), tr.channel.len(), |o| o.bytes(ch));
    tr.rssi_offset = rssi_offset;
    tr.freq_hz = freq_hz;
}

#[no_mangle]
pub extern "C" fn rdof_tracker_add_sink(tr: &mut RdofTracker, sink: RdofSink) -> bool {
    let Some(n) = usize::try_from(tr.nsinks).ok().filter(|&n| n < MAX_SINKS) else {
        return false;
    };
    tr.sinks[n] = sink;
    tr.nsinks += 1;
    true
}

#[no_mangle]
pub unsafe extern "C" fn rdof_modem_burst(control: u8, burst: &Burst, dbm: f64, cc: c_int, slot: c_int, out: *mut RdofBurst) {
    out.write_bytes(0, 1);
    let out = &mut *out;
    out.burst = *burst;
    rdof_control_to_dt(control, &mut out.dt, &mut out.n);
    out.slot = slot;
    out.family = RDOF_FAMILY_MS;
    out.cc = cc;
    out.power_dbfs = dbm;
}

// ── output ──

impl RdofTracker {
    fn sinks(&self) -> &[RdofSink] {
        &self.sinks[..(self.nsinks.max(0) as usize).min(MAX_SINKS)]
    }

    fn emit_json(&self, j: &RdofJson) {
        if !j.done() {
            return;
        }
        for s in self.sinks() {
            if let Some(f) = s.on_json {
                unsafe { f(s.ctx, self.channel.as_ptr(), j.buf) };
            }
        }
    }

    fn emit_raw(&self, b: &RdofBurst, t: f64, call: Option<&RdofCall>) {
        let r = RdofRawburst {
            ms: rdof_time_ms(t),
            slot: b.slot,
            network: false,
            dt: b.dt,
            n: b.n,
            addressed: call.is_some(),
            src: call.map_or(0, |c| c.src),
            dst: call.map_or(0, |c| c.dst),
            group: call.is_some_and(|c| c.group),
            burst: b.burst,
            utc: b.utc,
            utc_us: b.utc_us,
            utc_slot: b.utc_slot,
        };
        let mut buf = [0 as c_char; JSON_MAX];
        // the JSON only for a sink that takes it (the tests, the self-test)
        let json = self.sinks().iter().any(|s| s.on_raw.is_some()) && unsafe { rdof_rawburst_json(&r, buf.as_mut_ptr(), buf.len()) };
        for s in self.sinks() {
            if let (Some(f), true) = (s.on_raw, json) {
                unsafe { f(s.ctx, self.channel.as_ptr(), buf.as_ptr()) };
            }
            if let Some(f) = s.on_raw_burst {
                unsafe { f(s.ctx, self.channel.as_ptr(), &r) };
            }
        }
    }

    fn emit_frame(&self, f: &RdofFrame) {
        for s in self.sinks() {
            if let Some(cb) = s.on_frame {
                unsafe { cb(s.ctx, self.channel.as_ptr(), f) };
            }
        }
    }

    fn emit_call(&self, action: &CStr, src: u32, dst: u32) {
        for s in self.sinks() {
            if let Some(f) = s.on_call {
                unsafe { f(s.ctx, self.channel.as_ptr(), action.as_ptr(), src, dst) };
            }
        }
    }

    fn burst_rssi(&self, b: &RdofBurst) -> c_int {
        if b.power_dbfs.is_nan() {
            return 0;
        }
        // saturating, as the C's
        unsafe { ffi::rint(b.power_dbfs + self.rssi_offset) as c_int }
    }

    fn frame_from(&self, b: &RdofBurst, n: c_int, call: Option<&RdofCall>, new_stream: bool) {
        let mut f = make_frame(b.slot, b.dt, n, &b.burst, call);
        f.new_stream = new_stream;
        f.errors = b.sync_errors;
        f.rssi = self.burst_rssi(b);
        self.emit_frame(&f);
    }

    fn start_json(&self, action: &CStr, slot: c_int, call: &RdofCall, t: f64, frames: c_int) {
        let mut buf = [0 as c_char; JSON_MAX];
        let ts = iso(t);
        let mut j = unsafe { RdofJson::new(buf.as_mut_ptr(), buf.len()) };
        j.open(None);
        j.open(Some(c"DMR"));
        j.str(c"timestamp", unsafe { c_bytes(ts.as_ptr()) });
        j.str(c"source", b"rf");
        j.str(c"action", action.to_bytes());
        j.int(c"slot", slot.into());
        j.int(c"src_id", call.src.into());
        j.int(c"dst_id", call.dst.into());
        j.str(c"group", if call.group { b"yes" } else { b"no" });
        let mut info = [0u8; 16];
        let mut o = Out::new(&mut info);
        o.int(call.src.into(), 0);
        let n = o.n;
        j.str(c"src_info", &info[..n]);
        if self.freq_hz != 0.0 {
            let mut freq = [0u8; 24];
            let mut o = Out::new(&mut freq);
            o.fixed(self.freq_hz / 1e6, 5);
            let n = o.n.min(23);
            j.str(c"frequency", &freq[..n]);
            j.int(c"colorCode", call.cc.into());
        }
        if frames >= 0 {
            j.int(c"frames", frames.into());
        }
        if call.emergency {
            j.bool(c"emergency", true);
        }
        j.close();
        j.close();
        self.emit_json(&j);
        self.emit_call(action, call.src, call.dst);
    }

    fn end_json(&self, action: &CStr, slot: c_int, call: &RdofCall, t: f64) {
        let mut buf = [0 as c_char; JSON_MAX];
        let ts = iso(t);
        let mut j = unsafe { RdofJson::new(buf.as_mut_ptr(), buf.len()) };
        j.open(None);
        j.open(Some(c"DMR"));
        j.str(c"timestamp", unsafe { c_bytes(ts.as_ptr()) });
        j.str(c"action", action.to_bytes());
        j.int(c"slot", slot.into());
        if call.kind == CALL_VOICE {
            let dur = t - call.start;
            j.float3(c"duration", if dur > 0.0 { dur } else { 0.0 });
            j.float3(c"ber", if call.bits != 0 { 100.0 * call.errs as f64 / call.bits as f64 } else { 0.0 });
            if call.rssi_n != 0 {
                j.open(Some(c"rssi"));
                j.int(c"min", call.rssi_min.into());
                j.int(c"max", call.rssi_max.into());
                j.int(c"ave", to_i64(unsafe { ffi::rint(call.rssi_sum as f64 / f64::from(call.rssi_n)) }));
                j.close();
            }
        }
        j.close();
        j.close();
        self.emit_json(&j);
        self.emit_call(action, call.src, call.dst);
    }

    fn mode_json(&self, t: f64, mode: &[u8]) {
        let mut buf = [0 as c_char; 128];
        let ts = iso(t);
        let mut j = unsafe { RdofJson::new(buf.as_mut_ptr(), buf.len()) };
        j.open(None);
        j.open(Some(c"MMDVM"));
        j.str(c"timestamp", unsafe { c_bytes(ts.as_ptr()) });
        j.str(c"mode", mode);
        j.close();
        j.close();
        self.emit_json(&j);
    }

    fn activity(&mut self, t: f64) {
        self.last_activity = t;
        if self.mode != MODE_DMR {
            self.mode = MODE_DMR;
            self.mode_json(t, b"DMR");
        }
    }

    // ── calls ──

    fn close_call(&mut self, s: usize, slot: c_int, action: &CStr, t: f64, terminator: bool) {
        if !self.slots[s].call.active {
            return;
        }
        let call = self.slots[s].call;
        self.slots[s].call.active = false;
        self.end_json(action, slot, &call, t);
        if call.kind == CALL_VOICE && !terminator {
            // Carrier lost: give the network a proper end of stream, with the call's own LC.
            let mut lc = [0u8; 9];
            if call.has_lc {
                lc = call.lc;
            } else {
                rdof_group_lc(call.src, call.dst, call.group, 0, &mut lc);
            }
            let mut term = [0u8; RDOF_BURST_BYTES];
            rdof_lc_burst(&lc, RDOF_DT_TERMINATOR_WITH_LC, call.cc, rdof_family_voice_sync(call.family) ^ RDOF_DATA_XOR, &mut term);
            let mut f = make_frame(slot, RDOF_DT_TERMINATOR_WITH_LC, 0, &term, Some(&call));
            f.synthetic = true;
            self.emit_frame(&f);
        }
    }

    #[allow(clippy::too_many_arguments)]
    fn open_call(&mut self, s: usize, slot: c_int, kind: c_int, src: u32, dst: u32, group: bool, t: f64, b: &RdofBurst) {
        if self.slots[s].call.active {
            self.close_call(s, slot, c"lost", t, false);
        }
        self.slots[s].call = RdofCall {
            active: true,
            kind,
            src,
            dst,
            group,
            start: t,
            last: t,
            cc: if b.cc >= 0 { b.cc } else { 0 },
            family: b.family,
            metrics_at: t,
            ..CALL_NONE
        };
    }

    fn current(&self, s: usize) -> Option<&RdofCall> {
        Some(&self.slots[s].call).filter(|c| c.active)
    }

    fn on_lc(&mut self, b: &RdofBurst, s: usize, t: f64) {
        let mut lc: RdofFullLc = unsafe { core::mem::zeroed() };
        let have = rdof_decode_voice_lc(&b.burst, b.dt, &mut lc);
        let has_call = self.current(s).is_some();
        let emergency = lc.service_options & RDOF_SO_EMERGENCY != 0;
        if b.dt == RDOF_DT_VOICE_LC_HEADER {
            if !have || lc.src_id == RDOF_NONE {
                self.emit_raw(b, t, self.current(s));
                return;
            }
            let (src, dst) = (lc.src_id as u32, lc.dst_id as u32);
            let same = self.current(s).is_some_and(|c| c.kind == CALL_VOICE && c.src == src && c.dst == dst);
            if !same {
                self.open_call(s, b.slot, CALL_VOICE, src, dst, lc.group, t, b);
                self.slots[s].call.emergency = emergency;
                let call = self.slots[s].call;
                self.start_json(c"start", b.slot, &call, t, -1);
            }
            let st = &mut self.slots[s];
            st.have_frag = 0;
            st.call.last = t;
            st.call.has_lc = true;
            st.call.lc = lc.raw;
            let call = st.call;
            self.emit_raw(b, t, Some(&call));
            self.frame_from(b, 0, Some(&call), !same);
            return;
        }
        // Terminator with LC.
        let st = &self.slots[s];
        let (src, dst) = (lc.src_id as u32, lc.dst_id as u32);
        if !has_call
            && have
            && st.has_ended
            && lc.src_id != RDOF_NONE
            && st.ended_src == src
            && st.ended_dst == dst
            && t - st.ended_at <= REPEAT_TERMINATOR_SECONDS
        {
            self.emit_raw(b, t, None); // a repeat of the one that just ended it (MMDVMHost sends 3)
            return;
        }
        if !has_call && have && lc.src_id != RDOF_NONE {
            // Only heard the end: report it as a (very short) late entry.
            self.open_call(s, b.slot, CALL_VOICE, src, dst, lc.group, t, b);
            self.slots[s].call.emergency = emergency;
            let call = self.slots[s].call;
            self.start_json(c"late_entry", b.slot, &call, t, -1);
        }
        let call = self.current(s).copied();
        self.emit_raw(b, t, call.as_ref());
        if let Some(call) = call {
            self.frame_from(b, 0, Some(&call), false);
            self.close_call(s, b.slot, c"end", t, true);
            let st = &mut self.slots[s];
            st.has_ended = true;
            st.ended_src = call.src;
            st.ended_dst = call.dst;
            st.ended_at = t;
        }
    }

    fn on_voice(&mut self, b: &RdofBurst, s: usize, t: f64) {
        let voice = |c: &RdofCall| c.active && c.kind == CALL_VOICE;
        // Embedded LC: late entry, or a new talker that skipped the header.
        if b.n == 0 {
            self.slots[s].have_frag = 0;
        } else if (1..=4).contains(&b.n) {
            let st = &mut self.slots[s];
            let k = b.n as usize;
            unsafe { ffi::rdof_embedded_signalling(b.burst.as_ptr(), st.frags[k].as_mut_ptr()) };
            st.have_frag |= 1 << k;
            if b.n == 4 && st.have_frag == 0x1E {
                let mut raw = [0u8; 128];
                for (r, f) in raw.as_chunks_mut::<32>().0.iter_mut().zip(&st.frags[1..]) {
                    r.copy_from_slice(f);
                }
                st.have_frag = 0;
                let mut lc9 = [0u8; 9];
                if unsafe { ffi::rdof_embedded_lc_decode(raw.as_ptr(), lc9.as_mut_ptr()) } {
                    let mut lc: RdofFullLc = unsafe { core::mem::zeroed() };
                    rdof_parse_full_lc(&lc9, &mut lc);
                    let (src, dst) = (lc.src_id as u32, lc.dst_id as u32);
                    let same = Some(&st.call).filter(|c| voice(c)).is_some_and(|c| c.src == src && c.dst == dst);
                    if (lc.flco == RDOF_FLCO_GROUP || lc.flco == RDOF_FLCO_PRIVATE) && late_entry_fid(lc.fid) && !same {
                        self.open_call(s, b.slot, CALL_VOICE, src, dst, lc.group, t, b);
                        let call = &mut self.slots[s].call;
                        call.has_lc = true;
                        call.lc = lc9;
                        call.emergency = lc.service_options & RDOF_SO_EMERGENCY != 0;
                        let call = *call;
                        self.start_json(c"late_entry", b.slot, &call, t, -1);
                        let mut hdr = [0u8; RDOF_BURST_BYTES];
                        rdof_lc_burst(
                            &lc9,
                            RDOF_DT_VOICE_LC_HEADER,
                            call.cc,
                            rdof_family_voice_sync(b.family) ^ RDOF_DATA_XOR,
                            &mut hdr,
                        );
                        let mut f = make_frame(b.slot, RDOF_DT_VOICE_LC_HEADER, 0, &hdr, Some(&call));
                        f.new_stream = true;
                        f.synthetic = true;
                        self.emit_frame(&f);
                    }
                }
            }
        }
        let rssi = self.burst_rssi(b);
        let measured = !b.power_dbfs.is_nan();
        let call = &mut self.slots[s].call;
        let mut metrics = None;
        let call = if voice(call) {
            call.last = t;
            let mut frames = [[0u8; 9]; 3];
            unsafe { ffi::rdof_ambe_frames(b.burst.as_ptr(), frames.as_mut_ptr().cast()) };
            let errs: c_long = frames.iter().map(|f| c_long::from(rdof_ambe_decode_frame(f, None))).sum();
            call.bits = call.bits.wrapping_add(141);
            call.errs = call.errs.wrapping_add(errs);
            call.m_bits = call.m_bits.wrapping_add(141);
            call.m_errs = call.m_errs.wrapping_add(errs);
            if measured {
                if call.rssi_n == 0 || rssi < call.rssi_min {
                    call.rssi_min = rssi;
                }
                if call.rssi_n == 0 || rssi > call.rssi_max {
                    call.rssi_max = rssi;
                }
                call.rssi_sum = call.rssi_sum.wrapping_add(rssi.into());
                call.rssi_n = call.rssi_n.wrapping_add(1);
            }
            if t - call.metrics_at >= METRIC_SECONDS {
                call.metrics_at = t;
                metrics = Some(100.0 * call.m_errs as f64 / call.m_bits.max(1) as f64);
                call.m_bits = 0;
                call.m_errs = 0;
            }
            Some(*call)
        } else {
            None
        };
        if let Some(ber) = metrics {
            let mut buf = [0 as c_char; 160];
            let ts = iso(t);
            let ts = unsafe { c_bytes(ts.as_ptr()) };
            let mut j = unsafe { RdofJson::new(buf.as_mut_ptr(), buf.len()) };
            j.open(None);
            j.open(Some(c"BER"));
            j.str(c"timestamp", ts);
            j.str(c"mode", b"DMR");
            j.int(c"slot", b.slot.into());
            j.float3(c"value", ber);
            j.close();
            j.close();
            self.emit_json(&j);
            if measured {
                let mut j = unsafe { RdofJson::new(buf.as_mut_ptr(), buf.len()) };
                j.open(None);
                j.open(Some(c"RSSI"));
                j.str(c"timestamp", ts);
                j.str(c"mode", b"DMR");
                j.int(c"slot", b.slot.into());
                j.int(c"value", rssi.into());
                j.close();
                j.close();
                self.emit_json(&j);
            }
        }
        self.emit_raw(b, t, call.as_ref());
        self.frame_from(b, b.n, call.as_ref(), false);
    }

    fn on_csbk(&mut self, b: &RdofBurst, s: usize, t: f64) {
        let mut c = RdofCsbk::default();
        rdof_decode_csbk(&b.burst, &mut c);
        if !c.crc_ok || (c.fid == 0 && c.opcode == CSBK_BSDWNACT) {
            return;
        }
        let d = &c.raw;
        let (a, z) = unsafe { (rdof_be24(d[4..].as_ptr()), rdof_be24(d[7..].as_ptr())) };
        let (src, dst, group) = if c.opcode == 0x3D {
            (z, a, d[2] & 0x40 != 0)
        } else if c.opcode == 0x26 || (c.opcode == 0x24 && d[3] != 0x80) {
            (a, z, false)
        } else {
            (z, a, c.opcode == 0x27)
        };
        self.emit_raw(b, t, None);
        let addr = RdofCall { src, dst, group, ..CALL_NONE };
        let mut f = make_frame(b.slot, b.dt, 0, &b.burst, Some(&addr));
        f.new_stream = true;
        f.errors = b.sync_errors;
        f.rssi = self.burst_rssi(b);
        self.emit_frame(&f);
        let mut desc = if c.fid == 0x00 || c.fid == 0x10 { csbk_desc(c.opcode) } else { None };
        if c.opcode == 0x24 && desc.is_some() {
            // a request, or the radio's answer
            desc = Some(if d[3] == 0x80 { b"Radio Check" } else { b"Radio Check Ack" });
        }
        let Some(desc) = desc else {
            return;
        };
        if c.opcode == 0x3D {
            let st = &mut self.slots[s];
            if st.has_preamble && st.preamble_src == src && st.preamble_dst == dst && t - st.preamble_at < PREAMBLE_REPEAT_SECONDS {
                st.preamble_at = t;
                return;
            }
            st.has_preamble = true;
            st.preamble_src = src;
            st.preamble_dst = dst;
            st.preamble_at = t;
        }
        let mut buf = [0 as c_char; JSON_MAX];
        let ts = iso(t);
        let mut info = [0u8; 16];
        let mut o = Out::new(&mut info);
        o.int(src.into(), 0);
        let n = o.n;
        let mut j = unsafe { RdofJson::new(buf.as_mut_ptr(), buf.len()) };
        j.open(None);
        j.open(Some(c"DMR"));
        j.str(c"timestamp", unsafe { c_bytes(ts.as_ptr()) });
        j.str(c"source", b"rf");
        j.str(c"action", b"csbk");
        j.int(c"slot", b.slot.into());
        j.int(c"src_id", src.into());
        j.int(c"dst_id", dst.into());
        j.str(c"group", if group { b"yes" } else { b"no" });
        j.str(c"src_info", &info[..n]);
        j.str(c"csbk_desc", desc);
        j.close();
        j.close();
        self.emit_json(&j);
    }

    fn on_data_block(&mut self, b: &RdofBurst, s: usize, t: f64) {
        let call = Some(self.slots[s].call).filter(|c| c.active && c.kind == CALL_DATA);
        self.emit_raw(b, t, call.as_ref());
        self.frame_from(b, 0, call.as_ref(), false);
        if call.is_some() {
            let c = &mut self.slots[s].call;
            c.last = t;
            c.blocks_left = c.blocks_left.wrapping_sub(1);
            if c.blocks_left <= 0 {
                let (src, dst) = (c.src, c.dst);
                self.close_call(s, b.slot, c"end", t, false);
                self.data_ended(s, src, dst, t);
            }
        }
    }

    fn on_data_header(&mut self, b: &RdofBurst, s: usize, t: f64) {
        let mut h: RdofDataHeader = unsafe { core::mem::zeroed() };
        rdof_decode_data_header(&b.burst, &mut h);
        if !h.crc_ok {
            self.emit_raw(b, t, self.current(s));
            return;
        }
        if h.dpf == RDOF_DPF_PROPRIETARY && self.current(s).is_some_and(|c| c.kind == CALL_DATA) {
            // Proprietary second header: counts as one of the announced blocks.
            return self.on_data_block(b, s, t);
        }
        self.open_call(s, b.slot, CALL_DATA, h.src_id, h.dst_id, h.group, t, b);
        self.slots[s].call.blocks_left = h.blocks;
        let call = self.slots[s].call;
        self.start_json(c"start", b.slot, &call, t, h.blocks);
        self.emit_raw(b, t, Some(&call));
        self.frame_from(b, 0, Some(&call), true);
        if call.blocks_left == 0 {
            self.close_call(s, b.slot, c"end", t, false);
            self.data_ended(s, call.src, call.dst, t);
        }
    }

    // A data call over at its last block: a terminator from the same radio soon after (a chain's, RadioFoundry
    // radios end one with it) is that transmission's end, not a call of its own.
    fn data_ended(&mut self, s: usize, src: u32, dst: u32, t: f64) {
        let st = &mut self.slots[s];
        st.has_ended = true;
        st.ended_src = src;
        st.ended_dst = dst;
        st.ended_at = t;
    }
}

const CALL_NONE: RdofCall = RdofCall {
    active: false,
    kind: CALL_VOICE,
    src: 0,
    dst: 0,
    group: false,
    start: 0.0,
    last: 0.0,
    cc: 0,
    family: 0,
    bits: 0,
    errs: 0,
    rssi_n: 0,
    rssi_sum: 0,
    rssi_min: 0,
    rssi_max: 0,
    blocks_left: 0,
    metrics_at: 0.0,
    m_bits: 0,
    m_errs: 0,
    has_lc: false,
    lc: [0; 9],
    emergency: false,
};

/// Voice LCs whose embedded LC starts a late entry: ETSI, and Motorola's.
fn late_entry_fid(fid: c_int) -> bool {
    fid == 0x00 || fid == 0x10
}

fn csbk_desc(opcode: c_int) -> Option<&'static [u8]> {
    Some(match opcode {
        0x04 => b"Unit to Unit Voice Service Request",
        0x05 => b"Unit to Unit Voice Answer Response",
        0x26 => b"Negative Acknowledgment Response",
        0x3D => b"Preamble",
        0x1F => b"Call Alert",
        0x20 => b"Call Alert Ack",
        0x24 => b"Radio Check",
        0x27 => b"Call Emergency",
        _ => return None,
    })
}

fn make_frame(slot: c_int, dt: c_int, n: c_int, burst: &Burst, call: Option<&RdofCall>) -> RdofFrame {
    RdofFrame {
        slot,
        dt,
        n,
        burst: *burst,
        has_addr: call.is_some(),
        src: call.map_or(0, |c| c.src),
        dst: call.map_or(0, |c| c.dst),
        group: call.is_none_or(|c| c.group),
        new_stream: false,
        synthetic: false,
        errors: 0,
        rssi: 0,
    }
}

#[no_mangle]
pub extern "C" fn rdof_tracker_feed(tr: &mut RdofTracker, b: &RdofBurst, t: f64) {
    if b.dt == RDOF_DT_IDLE || !(1..=2).contains(&b.slot) {
        return;
    }
    tr.activity(t);
    let s = (b.slot - 1) as usize;
    match b.dt {
        RDOF_DT_VOICE_SYNC | RDOF_DT_VOICE => tr.on_voice(b, s, t),
        RDOF_DT_VOICE_LC_HEADER | RDOF_DT_TERMINATOR_WITH_LC => tr.on_lc(b, s, t),
        RDOF_DT_CSBK => tr.on_csbk(b, s, t),
        RDOF_DT_DATA_HEADER => tr.on_data_header(b, s, t),
        RDOF_DT_RATE_12 | RDOF_DT_RATE_34 | RDOF_DT_RATE_1 => tr.on_data_block(b, s, t),
        _ => {
            // PI header, MBC, USBD: pass through
            let call = tr.current(s).copied();
            tr.emit_raw(b, t, call.as_ref());
            tr.frame_from(b, 0, call.as_ref(), false);
        }
    }
}

#[no_mangle]
pub extern "C" fn rdof_tracker_tick(tr: &mut RdofTracker, t: f64) {
    for s in 0..2 {
        let c = &tr.slots[s].call;
        if !c.active {
            continue;
        }
        let limit = if c.kind == CALL_VOICE { VOICE_LOST_SECONDS } else { DATA_LOST_SECONDS };
        if t - c.last > limit {
            let last = c.last;
            tr.close_call(s, s as c_int + 1, c"lost", last, false);
        }
    }
    let any = tr.slots.iter().any(|st| st.call.active);
    if tr.mode == MODE_DMR && t - tr.last_activity > IDLE_HANG_SECONDS && !any {
        tr.mode = MODE_IDLE;
        tr.mode_json(t, b"idle");
    }
}
