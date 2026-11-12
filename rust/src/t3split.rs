// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! A trunked site's payload carrier on a split TX half (rdof/sdr/t3split.h; docs/spec/fw/tier3.md 9.3). The splits
//! (rdof_sdrsplits_*) are in rdof_sdr, which this library can't link: they come in rdof_t3split_ops_t, filled in by
//! rdof_t3split_init in the header (compiled into its caller). The split itself (split.c) is rdof_common's C.

use core::ffi::{c_char, c_int, c_void};
use core::ptr::addr_of_mut;

use crate::mmdvm::{rdof_mmdvm_frame, rdof_mmdvm_reader_feed, rdof_mmdvm_reader_init, RdofMmdvmReader, RDOF_SYNC_DATA};
use crate::t3rf::{layouts, rdof_t3rf_split_init, Layout, T3rfSplit, BURST_BYTES};
use crate::text::{c_bytes, c_out, Out};
use crate::tracker::RdofBurst;

pub const Q: usize = 32;
const SETUP_WAIT_S: f64 = 2.0;

// rdof_t3split_state_t
pub const BRINGUP: c_int = 0;
pub const SETUP: c_int = 1;
pub const UP: c_int = 2;
pub const FAILED: c_int = 3;

// rdof/mmdvm.h, rdof/splitops.h, rdof/layer2.h
const SET_CONFIG: u8 = 0x02;
const SET_MODE: u8 = 0x03;
const SET_FREQ: u8 = 0x04;
const DMR_DATA1: u8 = 0x18;
const DMR_DATA2: u8 = 0x1A;
const ACK: u8 = 0x70;
const NAK: u8 = 0x7F;
const MODE_DMR: u8 = 2;
const SYNC_AUDIO: u8 = 0x20;
const STREAM_SCHEDULED: c_int = 1;
const FAMILY_MS: u32 = 1;
const DT_VOICE_SYNC: c_int = 0xF0;
const DT_VOICE: c_int = 0xF1;

/// rdof_t3split_io_t
#[repr(C)]
#[derive(Clone, Copy)]
pub struct T3splitIo {
    pub to_radio: Option<unsafe extern "C" fn(ctx: *mut c_void, d: *const u8, n: usize)>,
    pub rx_channel: Option<unsafe extern "C" fn(ctx: *mut c_void, hz: u32) -> c_int>,
    pub now48: Option<unsafe extern "C" fn(ctx: *mut c_void) -> i64>,
    pub ctx: *mut c_void,
}

/// rdof_sdrsplits_io_t (rdof/sdr/sdrsplit.h)
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SdrsplitsIo {
    pub to_radio: Option<unsafe extern "C" fn(ctx: *mut c_void, i: c_int, d: *const u8, n: usize)>,
    pub to_host: Option<unsafe extern "C" fn(ctx: *mut c_void, i: c_int, d: *const u8, n: usize)>,
    pub rx_channel: Option<unsafe extern "C" fn(ctx: *mut c_void, hz: u32) -> c_int>,
    pub now48: Option<unsafe extern "C" fn(ctx: *mut c_void) -> i64>,
    pub ctx: *mut c_void,
}

/// rdof_sdrsplits_t (rdof/sdr/sdrsplit.h): only `error` is read here; the rest is its size.
#[repr(C)]
pub struct Sdrsplits {
    pub io: SdrsplitsIo,
    pub rx_fs: u32,
    pub u: [*mut c_void; 8],
    pub n: c_int,
    pub busy: bool,
    pub again: bool,
    pub error: [c_char; 128],
}

/// rdof_split_params_t (rdof/split.h)
#[repr(C)]
pub struct SplitParams {
    pub power_cap: c_int,
    pub prebuffer: c_int,
    pub lease_s: c_int,
    pub cooldown_s: c_int,
    pub max_key_s: f64,
    pub stream_mode: c_int,
}

type Set = *mut Sdrsplits;
type Split = *mut c_void; // struct rdof_split, opaque here

/// rdof_t3split_ops_t
#[repr(C)]
pub struct Ops {
    pub init: unsafe extern "C" fn(set: Set, io: *const SdrsplitsIo, rx_fs: u32),
    pub add: unsafe extern "C" fn(set: Set, p: *const SplitParams, now: f64) -> c_int,
    pub release: unsafe extern "C" fn(set: Set),
    pub upstream: unsafe extern "C" fn(set: Set, i: c_int, d: *const u8, n: usize, now: f64),
    pub from_radio: unsafe extern "C" fn(set: Set, i: c_int, d: *const u8, n: usize, now: f64),
    pub heard: unsafe extern "C" fn(set: Set, channel: c_int, b: *const c_void),
    pub tick: unsafe extern "C" fn(set: Set, now: f64),
    pub split: unsafe extern "C" fn(set: Set, i: c_int) -> Split,
}

/// rdof_t3split_t's q[]
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Heard {
    pub slot: u8,
    pub control: u8,
    pub rssi: c_int,
    pub burst: [u8; BURST_BYTES],
}

/// rdof_t3split_t
#[repr(C)]
pub struct T3split {
    pub io: T3splitIo,
    pub ops: *const Ops,
    pub set: Sdrsplits,
    pub o: T3rfSplit,
    pub rx_hz: u32,
    pub tx_hz: u32,
    pub cc: c_int,
    pub state: c_int,
    pub setup_step: c_int,
    pub setup_at: f64,
    pub error: [c_char; 128],
    pub reader: RdofMmdvmReader,
    pub q: [Heard; Q],
    pub qhead: c_int,
    pub qtail: c_int,
    pub heard: u32,
    pub dropped: u32,
    pub naks: u32,
}

extern "C" {
    // split.c
    fn rdof_split_params_default(p: *mut SplitParams);
    fn rdof_split_ready(s: *const c_void) -> bool;
    fn rdof_split_failed(s: *const c_void) -> *const c_char;
    fn rdof_split_set_utc(
        s: Split,
        utc: bool,
        utc_minus_host_us: f64,
        bound_us: f64,
        offset_us: c_int,
        transmit: bool,
        now: f64,
    );
}

unsafe fn set_error(t: *mut T3split, f: impl FnOnce(&mut Out)) {
    let e = addr_of_mut!((*t).error);
    c_out(e.cast(), (*e).len(), f);
}

unsafe fn ops<'a>(t: *const T3split) -> &'a Ops {
    &*(*t).ops
}

unsafe extern "C" fn ts_to_radio(ctx: *mut c_void, _i: c_int, d: *const u8, n: usize) {
    let t = ctx.cast::<T3split>();
    if let Some(f) = (*t).io.to_radio {
        f((*t).io.ctx, d, n);
    }
}

/// What the split sends its repeater: the setup's answers, and the heard bursts.
unsafe extern "C" fn on_up(ctx: *mut c_void, cmd: u8, d: *const u8, n: usize) {
    let t = ctx.cast::<T3split>();
    let d = crate::bits::slice(d, n);
    if cmd == ACK || cmd == NAK {
        if (*t).state != SETUP {
            return;
        }
        if cmd == NAK {
            (*t).naks += 1;
            let (c, err) = (d.first().copied().unwrap_or(0), d.get(1).copied().unwrap_or(0));
            set_error(t, |o| {
                const HEX: &[u8; 16] = b"0123456789abcdef";
                o.bytes(b"the split refused its setup (NAK 0x");
                o.byte(HEX[usize::from(c >> 4)]);
                o.byte(HEX[usize::from(c & 15)]);
                o.bytes(b" err ");
                o.int(err.into(), 0);
                o.byte(b')');
            });
            (*t).state = FAILED;
            return;
        }
        (*t).setup_step += 1;
        (*t).setup_at = -1.0; // the next goes out on the next tick
        return;
    }
    if (cmd == DMR_DATA1 || cmd == DMR_DATA2) && n > BURST_BYTES {
        let next = ((*t).qhead + 1) % Q as c_int;
        if next == (*t).qtail {
            (*t).dropped += 1;
            return;
        }
        let q = &mut (*t).q[(*t).qhead as usize];
        q.slot = if cmd == DMR_DATA2 { 2 } else { 1 };
        q.control = d[0];
        q.rssi = 0;
        q.burst.copy_from_slice(&d[1..1 + BURST_BYTES]);
        (*t).qhead = next;
        (*t).heard += 1;
    }
}

unsafe extern "C" fn ts_to_host(ctx: *mut c_void, _i: c_int, d: *const u8, n: usize) {
    let t = ctx.cast::<T3split>();
    rdof_mmdvm_reader_feed(&mut *addr_of_mut!((*t).reader), d, n, Some(on_up), ctx);
}

unsafe extern "C" fn ts_channel(ctx: *mut c_void, hz: u32) -> c_int {
    let t = ctx.cast::<T3split>();
    match (*t).io.rx_channel {
        Some(f) => f((*t).io.ctx, hz),
        None => -1,
    }
}

unsafe extern "C" fn ts_now48(ctx: *mut c_void) -> i64 {
    let t = ctx.cast::<T3split>();
    match (*t).io.now48 {
        Some(f) => f((*t).io.ctx),
        None => 0, // not reached: given only when io.now48 is
    }
}

#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn rdof_t3split_start(
    t: *mut T3split,
    ops: *const Ops,
    io: *const T3splitIo,
    rx_fs: u32,
    carrier: c_int,
    rx_hz: u32,
    tx_hz: u32,
    cc: c_int,
    now: f64,
) -> bool {
    t.write_bytes(0, 1);
    (*t).io = *io;
    (*t).ops = ops;
    (*t).rx_hz = rx_hz;
    (*t).tx_hz = tx_hz;
    (*t).cc = cc;
    rdof_mmdvm_reader_init(&mut *addr_of_mut!((*t).reader));
    rdof_t3rf_split_init(addr_of_mut!((*t).o), carrier);
    let sio = SdrsplitsIo {
        to_radio: Some(ts_to_radio),
        to_host: Some(ts_to_host),
        rx_channel: Some(ts_channel),
        now48: if (*io).now48.is_some() { Some(ts_now48) } else { None },
        ctx: t.cast(),
    };
    let set = addr_of_mut!((*t).set);
    ((*ops).init)(set, &sio, rx_fs);
    let mut p: SplitParams = core::mem::zeroed();
    rdof_split_params_default(&mut p);
    p.stream_mode = STREAM_SCHEDULED;
    if ((*ops).add)(set, &p, now) < 0 {
        let why = c_bytes((*set).error.as_ptr());
        set_error(t, |o| o.bytes(why));
        (*t).state = FAILED;
        return false;
    }
    true
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3split_free(t: *mut T3split) {
    (ops(t).release)(addr_of_mut!((*t).set));
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3split_from_radio(t: *mut T3split, d: *const u8, n: usize, now: f64) {
    (ops(t).from_radio)(addr_of_mut!((*t).set), 0, d, n, now);
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3split_heard(t: *mut T3split, channel: c_int, b: *const c_void) {
    (ops(t).heard)(addr_of_mut!((*t).set), channel, b);
}

/// The setup's frames, as a repeater's start sends them: duplex DMR, the mode, the carrier's frequencies.
unsafe fn setup_send(t: *mut T3split, now: f64) {
    let mut body = [0u8; 20];
    let mut f = [0u8; 64];
    let (cmd, n) = match (*t).setup_step {
        0 => {
            body[3] = MODE_DMR; // the mode after it
            body[6] = (*t).cc as u8; // DMR color code
            (SET_CONFIG, 20)
        }
        1 => {
            body[0] = MODE_DMR;
            (SET_MODE, 1)
        }
        _ => {
            body[1..5].copy_from_slice(&(*t).rx_hz.to_le_bytes());
            body[5..9].copy_from_slice(&(*t).tx_hz.to_le_bytes());
            body[9] = 255; // RF level: full (the radio's own cap applies)
            (SET_FREQ, 10)
        }
    };
    let len = rdof_mmdvm_frame(cmd, body.as_ptr(), n, f.as_mut_ptr(), f.len());
    (*t).setup_at = now;
    (ops(t).upstream)(addr_of_mut!((*t).set), 0, f.as_ptr(), len, now);
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3split_tick(t: *mut T3split, now: f64) {
    let set = addr_of_mut!((*t).set);
    (ops(t).tick)(set, now);
    if (*t).state == FAILED {
        return;
    }
    let s = (ops(t).split)(set, 0);
    let failed = c_bytes(rdof_split_failed(s));
    if !failed.is_empty() {
        set_error(t, |o| o.bytes(failed));
        (*t).state = FAILED;
        return;
    }
    if (*t).state == BRINGUP && rdof_split_ready(s) {
        (*t).state = SETUP;
        (*t).setup_step = 0;
        setup_send(t, now);
    } else if (*t).state == SETUP {
        if (*t).setup_step >= 3 {
            (*t).state = UP;
        } else if (*t).setup_at < 0.0 {
            setup_send(t, now);
        } else if now - (*t).setup_at > SETUP_WAIT_S {
            let step = (*t).setup_step;
            set_error(t, |o| {
                o.bytes(b"the split didn't answer its setup (step ");
                o.int(step.into(), 0);
                o.byte(b')');
            });
            (*t).state = FAILED;
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3split_utc(t: *mut T3split, utc_minus_host_us: f64, bound_us: f64, now: f64) {
    rdof_split_set_utc(rdof_t3split_split(t), true, utc_minus_host_us, bound_us, 0, true, now);
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3split_split(t: *mut T3split) -> Split {
    (ops(t).split)(addr_of_mut!((*t).set), 0)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3split_take(
    t: *mut T3split,
    slot: &mut c_int,
    control: &mut u8,
    burst: &mut [u8; BURST_BYTES],
    rssi: *mut c_int,
) -> bool {
    let t = &mut *t;
    if t.qtail == t.qhead {
        return false;
    }
    let q = &t.q[t.qtail as usize];
    *slot = q.slot.into();
    *control = q.control;
    if !rssi.is_null() {
        *rssi = q.rssi;
    }
    *burst = q.burst;
    t.qtail = (t.qtail + 1) % Q as c_int;
    true
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3split_burst(
    t: &T3split,
    slot: c_int,
    control: u8,
    burst: &[u8; BURST_BYTES],
    out: *mut RdofBurst,
) {
    out.write_bytes(0, 1);
    let out = &mut *out;
    out.family = FAMILY_MS;
    out.cc = t.cc;
    out.slot = slot;
    out.burst = *burst;
    if control & RDOF_SYNC_DATA != 0 {
        out.dt = c_int::from(control & 0x0F);
    } else if control & SYNC_AUDIO != 0 {
        out.dt = DT_VOICE_SYNC;
        out.n = 0;
    } else {
        out.dt = DT_VOICE;
        out.n = c_int::from(control & 0x0F);
    }
}

// ── layouts (rdof_t3split_layout) ──────────────────────────────────────────────

static LAYOUTS: [Layout; 7] = layouts![
    ("rdof_t3split_io_t", T3splitIo),
    ("rdof_t3split_ops_t", Ops),
    ("rdof_sdrsplits_io_t", SdrsplitsIo),
    ("rdof_sdrsplits_t", Sdrsplits),
    ("rdof_split_params_t", SplitParams),
    ("rdof_t3split_t", T3split),
];

#[no_mangle]
pub extern "C" fn rdof_t3split_layout(i: usize) -> *const Layout {
    LAYOUTS.get(i).map_or(core::ptr::null(), |l| l as *const Layout)
}
