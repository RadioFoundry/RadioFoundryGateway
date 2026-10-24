// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! The bench API's transmit job (rdof/benchtx.h): one WS /tx socket's job as a state machine. A port of
//! common/src/benchtx.c. The tool's frames are untrusted text; the modem's bytes come through the MMDVM reader.

use core::ffi::{c_char, c_int, c_long, c_void, CStr};

use crate::bits::slice;
use crate::ffi::{RDOF_BURST_BYTES, RDOF_DT_TERMINATOR_WITH_LC};
use crate::jsonp::{rdof_jat, rdof_jget, rdof_jnode, rdof_jparse, JDoc, JNode, RDOF_JARR, RDOF_JNUM, RDOF_JSTR};
use crate::layer2::{rdof_decode_slot_type, rdof_detect_sync};
use crate::mmdvm::{
    rdof_mmdvm_config_v1, rdof_mmdvm_config_v2, rdof_mmdvm_frame, rdof_mmdvm_parse_version, rdof_mmdvm_reader_feed,
    rdof_mmdvm_reader_init, rdof_mmdvm_set_freq_payload, rdof_modem_config_default, RdofMmdvmReader, RdofModemConfig,
    RDOF_SYNC_DATA,
};
use crate::text::{c_bytes, c_out, Out};

type Burst = [u8; RDOF_BURST_BYTES];

const MAX_FREQS: usize = 8;
const QUEUE: usize = 64;
const BURST_S: f64 = 0.060;
const LEAD: f64 = 3.0;
const DEADMAN_S: f64 = 2.0;
const REPLY_S: f64 = 1.0;
const TRIES: c_int = 3;
const NODES: usize = 400;

const GET_VERSION: u8 = 0x00;
const SET_CONFIG: u8 = 0x02;
const SET_MODE: u8 = 0x03;
const SET_FREQ: u8 = 0x04;
const DMR_DATA1: u8 = 0x18;
const DMR_DATA2: u8 = 0x1A;
const ACK: u8 = 0x70;
const NAK: u8 = 0x7F;
const MODE_IDLE: u8 = 0;
const MODE_DMR: u8 = 2;
const SYNC_AUDIO: u8 = 0x20;
const NO_REPLY: u8 = 0xFF;

const SETUP: c_int = 0;
const READY: c_int = 1;
const DONE: c_int = 2;

const STEP_VERSION: c_int = 0;
const STEP_CONFIG: c_int = 1;
const STEP_FREQ: c_int = 2;
const STEP_MODE: c_int = 3;
const STEP_DONE: c_int = 4;
const STEP_NAMES: [&[u8]; 4] = [b"GET_VERSION", b"SET_CONFIG", b"SET_FREQ", b"SET_MODE"];

/// data_nak: a burst was NAKed (with its reason in the low byte when the modem gave one)
const NAKED: c_int = 0x200;
const NO_REASON: c_int = 0x100;

/// rdof_benchtx_policy_t
#[repr(C)]
pub struct RdofBenchtxPolicy {
    pub freqs_hz: [u32; MAX_FREQS],
    pub nfreqs: c_int,
    pub max_key_s: f64,
    pub rf_level: c_int,
    pub arm_gpio: c_int,
}

impl RdofBenchtxPolicy {
    /// No TX.
    pub const NONE: Self = RdofBenchtxPolicy { freqs_hz: [0; MAX_FREQS], nfreqs: 0, max_key_s: 0.0, rf_level: 0, arm_gpio: -1 };

    fn freqs(&self) -> &[u32] {
        &self.freqs_hz[..(self.nfreqs.max(0) as usize).min(MAX_FREQS)]
    }
}

/// rdof_benchtx_io_t
#[repr(C)]
pub struct RdofBenchtxIo {
    pub to_modem: Option<unsafe extern "C" fn(ctx: *mut c_void, b: *const u8, n: usize)>,
    pub to_tool: Option<unsafe extern "C" fn(ctx: *mut c_void, json: *const c_char, n: usize)>,
    pub keying: Option<unsafe extern "C" fn(ctx: *mut c_void, start: f64, seconds: f64, bursts: c_int, why: *const c_char)>,
    pub utc: Option<unsafe extern "C" fn(ctx: *mut c_void) -> f64>,
    pub ctx: *mut c_void,
}

/// rdof_benchtx_item_t
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RdofBenchtxItem {
    pub control: u8,
    pub burst: Burst,
}

/// rdof_benchtx_t
#[repr(C)]
pub struct RdofBenchtx {
    pub io: RdofBenchtxIo,
    pub freq: u32,
    pub cc: c_int,
    pub rf_level: c_int,
    pub max_key_s: f64,
    pub state: c_int,
    pub step: c_int,
    pub tries: c_int,
    pub protocol: c_int,
    pub want: u8,
    pub deadline: f64,
    pub replied: bool,
    pub nak: bool,
    pub q: [RdofBenchtxItem; QUEUE],
    pub qh: c_int,
    pub qn: c_int,
    pub voice_n: c_int,
    pub keyed: bool,
    pub unkey_asked: bool,
    pub last_voice: bool,
    pub key_start: f64,
    pub air_free: f64,
    pub key_bursts: c_int,
    pub last_tool: f64,
    pub need_mode: bool,
    pub data_nak: c_int,
    pub sent: c_long,
    pub keyings: c_long,
    pub underruns: c_long,
    pub frames: c_long,
    pub error: [c_char; 96],
    pub reader: RdofMmdvmReader,
    pub nodes: [JNode; NODES],
    pub strbuf: [c_char; 8192],
}

#[no_mangle]
pub unsafe extern "C" fn rdof_benchtx_gates(
    p: &RdofBenchtxPolicy,
    built: bool,
    can_tx: bool,
    freq: u32,
    lease: *const u32,
    nlease: c_int,
    armed: bool,
) -> *const c_char {
    let why: &CStr = if !built {
        c"this firmware was built without bench TX (CONFIG_RDOF_BENCH_TX)"
    } else if !can_tx {
        c"this path can't transmit"
    } else if p.max_key_s <= 0.0 || p.nfreqs == 0 {
        c"the settings allow no TX (bench.tx: freqs_hz and max_key_s)"
    } else if !p.freqs().contains(&freq) {
        c"that frequency isn't in the bench's TX list (bench.tx.freqs_hz)"
    } else if !(nlease > 0 && !lease.is_null() && core::slice::from_raw_parts(lease, nlease as usize).contains(&freq)) {
        c"the session holds no TX lease on that frequency"
    } else if p.arm_gpio >= 0 && !armed {
        c"the TX arm jumper is out"
    } else {
        return core::ptr::null();
    };
    why.as_ptr()
}

#[no_mangle]
pub extern "C" fn rdof_benchtx_control(burst: &Burst, mut n: c_int, voice_n: &mut c_int) -> c_int {
    let mut data = false;
    if rdof_detect_sync(burst, 4, None, Some(&mut data), None) {
        if !data {
            *voice_n = 0;
            return SYNC_AUDIO.into();
        }
        let mut dt = 0;
        if !rdof_decode_slot_type(burst, None, Some(&mut dt), None) {
            return -1;
        }
        return c_int::from(RDOF_SYNC_DATA) | (dt & 0x0F);
    }
    if !(1..=5).contains(&n) {
        if !(0..5).contains(voice_n) {
            return -1; // no sync, and no voice superframe to place it in
        }
        n = *voice_n + 1;
    }
    *voice_n = n;
    n
}

// ── frames ──

/// One frame to the tool, as the C's snprintf into 256 bytes (cut to 255).
fn tool(t: &RdofBenchtx, f: impl FnOnce(&mut Out)) {
    let mut b = [0u8; 256];
    let mut o = Out::new(&mut b);
    f(&mut o);
    let n = o.n.min(255);
    if n > 0 {
        if let Some(to_tool) = t.io.to_tool {
            unsafe { to_tool(t.io.ctx, b.as_ptr().cast(), n) };
        }
    }
}

fn utc(t: &RdofBenchtx, now: f64) -> f64 {
    match t.io.utc {
        Some(utc) => unsafe { utc(t.io.ctx) },
        None => now,
    }
}

fn modem(t: &RdofBenchtx, cmd: u8, p: &[u8]) {
    let mut f = [0u8; 64];
    let k = unsafe { rdof_mmdvm_frame(cmd, p.as_ptr(), p.len(), f.as_mut_ptr(), f.len()) };
    if k != 0 {
        if let Some(to_modem) = t.io.to_modem {
            unsafe { to_modem(t.io.ctx, f.as_ptr(), k) };
        }
    }
}

fn request(t: &mut RdofBenchtx, now: f64) {
    let mut p = [0u8; 37];
    let n = match t.step {
        STEP_VERSION => {
            t.want = GET_VERSION;
            0
        }
        STEP_CONFIG => {
            let mut c = core::mem::MaybeUninit::<RdofModemConfig>::uninit();
            let c = unsafe {
                rdof_modem_config_default(&mut *c.as_mut_ptr());
                c.assume_init()
            };
            t.want = SET_CONFIG;
            if t.protocol >= 2 {
                rdof_mmdvm_config_v2(&c, t.cc, MODE_DMR.into(), false, &mut p)
            } else {
                rdof_mmdvm_config_v1(&c, t.cc, MODE_DMR.into(), false, (&mut p[..23]).try_into().unwrap())
            }
        }
        STEP_FREQ => {
            t.want = SET_FREQ;
            rdof_mmdvm_set_freq_payload(t.freq, t.freq, t.rf_level, (&mut p[..14]).try_into().unwrap())
        }
        STEP_MODE => {
            t.want = SET_MODE;
            p[0] = MODE_DMR;
            1
        }
        _ => return,
    };
    t.replied = false;
    t.nak = false;
    t.deadline = now + REPLY_S;
    modem(t, t.want, &p[..n]);
}

fn finish(t: &mut RdofBenchtx, code: Option<&[u8]>, why: &[u8]) {
    if t.state == DONE {
        return;
    }
    if let Some(code) = code {
        unsafe { c_out(t.error.as_mut_ptr(), t.error.len(), |o| o.bytes(why)) };
        // quotes and backslashes dropped, at most 118 bytes (the C's 120-byte buffer)
        let mut esc = [0u8; 118];
        let mut k = 0;
        for &c in why.iter().filter(|&&c| c != b'"' && c != b'\\') {
            if k == esc.len() {
                break;
            }
            esc[k] = c;
            k += 1;
        }
        tool(t, |o| {
            o.bytes(b"{\"type\":\"error\",\"code\":\"");
            o.bytes(code);
            o.bytes(b"\",\"error\":\"");
            o.bytes(&esc[..k]);
            o.bytes(b"\"}");
        });
    }
    let (sent, keyings, underruns) = (t.sent, t.keyings, t.underruns);
    #[allow(clippy::useless_conversion)] // c_long is 32 bits on the S3
    tool(t, |o| {
        o.bytes(b"{\"type\":\"done\",\"why\":\"");
        o.bytes(code.unwrap_or(b"closed"));
        o.bytes(b"\",\"bursts\":");
        o.int(sent.into(), 0);
        o.bytes(b",\"keyings\":");
        o.int(keyings.into(), 0);
        o.bytes(b",\"underruns\":");
        o.int(underruns.into(), 0);
        o.bytes(b"}");
    });
    t.state = DONE;
}

/// Off the air: the queue dropped, the modem idle (its buffer goes with the mode), the keying logged.
fn unkey(t: &mut RdofBenchtx, why: &CStr, now: f64) {
    t.qn = 0;
    t.voice_n = -1;
    if !t.keyed {
        return;
    }
    if why != c"end" && why != c"underrun" {
        modem(t, SET_MODE, &[MODE_IDLE]);
        t.need_mode = true;
    }
    let end = if now < t.air_free { now } else { t.air_free };
    let secs = end - t.key_start;
    if let Some(keying) = t.io.keying {
        unsafe { keying(t.io.ctx, utc(t, now) - (now - t.key_start), secs, t.key_bursts, why.as_ptr()) };
    }
    let (at, bursts) = (utc(t, now), t.key_bursts);
    tool(t, |o| {
        o.bytes(b"{\"type\":\"unkeyed\",\"why\":\"");
        o.bytes(why.to_bytes());
        o.bytes(b"\",\"t\":");
        o.fixed(at, 6);
        o.bytes(b",\"seconds\":");
        o.fixed(secs, 3);
        o.bytes(b",\"bursts\":");
        o.int(bursts.into(), 0);
        o.bytes(b"}");
    });
    t.keyed = false;
    t.unkey_asked = false;
}

#[no_mangle]
pub unsafe extern "C" fn rdof_benchtx_init(
    t: *mut RdofBenchtx,
    io: &RdofBenchtxIo,
    freq: u32,
    cc: c_int,
    rf_level: c_int,
    max_key_s: f64,
    now: f64,
) {
    t.write_bytes(0, 1);
    let t = &mut *t;
    t.io = RdofBenchtxIo { ..*io };
    t.freq = freq;
    t.cc = cc;
    t.rf_level = rf_level.clamp(0, 255);
    t.max_key_s = max_key_s;
    t.state = SETUP;
    t.protocol = 1;
    t.voice_n = -1;
    t.want = NO_REPLY;
    t.last_tool = now;
    rdof_mmdvm_reader_init(&mut t.reader);
    t.step = STEP_VERSION;
    request(t, now);
}

// ── the tool's frames ──

fn hexval(c: u8) -> Option<u8> {
    (c as char).to_digit(16).map(|v| v as u8)
}

/// Why a frame stops the job: a code and a message (at most 95 bytes, as the C's 96-byte buffers).
struct Stop {
    code: &'static [u8],
    why: [u8; 96],
    n: usize,
}

impl Stop {
    fn new(code: &'static [u8], f: impl FnOnce(&mut Out)) -> Self {
        let mut s = Stop { code, why: [0; 96], n: 0 };
        let n = unsafe { c_out(s.why.as_mut_ptr().cast(), s.why.len(), f) };
        s.n = n.min(s.why.len() - 1);
        s
    }

    fn bad(why: &[u8]) -> Self {
        Stop::new(b"bad_request", |o| o.bytes(why))
    }
}

/// What a tool frame asks for, read from the doc before anything acts on it: the doc lives in t's node buffer,
/// so nothing holds `&mut t` while it's read.
enum Act {
    Bursts { count: c_int, voice_n: c_int },
    Unkey,
    Ping,
    Stop(Stop),
}

/// The job's state a bursts frame is checked against.
struct Queue {
    qn: c_int,
    voice_n: c_int,
    keyed: bool,
    air_free: f64,
    key_start: f64,
    max_key_s: f64,
}

fn bursts(d: &JDoc, root: c_int, q: &Queue, add: &mut [RdofBenchtxItem; QUEUE], now: f64) -> Act {
    let node = |i: c_int| unsafe { rdof_jnode(d, i).as_ref() }.filter(|_| i >= 0);
    let items = unsafe { rdof_jget(d, root, c"items".as_ptr()) };
    let Some(list) = node(items).filter(|n| n.type_ == RDOF_JARR) else {
        return Act::Stop(Stop::bad(b"a bursts frame has an items list"));
    };
    let count = list.count;
    if i64::from(q.qn) + i64::from(count) > QUEUE as i64 {
        return Act::Stop(Stop::bad(
            b"more bursts than the queue holds: wait for queued_s to fall before sending more",
        ));
    }
    // key time: what's on the air or handed over, what's queued, and these
    let ahead = if q.keyed { (if q.air_free > now { q.air_free - now } else { 0.0 }) + (now - q.key_start) } else { 0.0 };
    let total = ahead + f64::from(q.qn + count) * BURST_S;
    if total > q.max_key_s + 1e-9 {
        return Act::Stop(Stop::new(b"refused", |o| {
            o.bytes(b"that's ");
            o.fixed(total, 2);
            o.bytes(b" s of key time; the limit is ");
            o.fixed(q.max_key_s, 1);
            o.bytes(b" s");
        }));
    }
    let mut voice_n = q.voice_n;
    for (i, a) in (0..count).zip(add.iter_mut()) {
        let it = unsafe { rdof_jat(d, items, i) };
        let get = |k: &CStr| unsafe { rdof_jget(d, it, k.as_ptr()) };
        let hex = node(get(c"raw_hex")).filter(|h| h.type_ == RDOF_JSTR).map(|h| unsafe { c_bytes(h.str_) });
        let Some(hex) = hex.filter(|h| h.len() == 2 * RDOF_BURST_BYTES) else {
            return Act::Stop(Stop::bad(b"each item has raw_hex: the 33-byte burst"));
        };
        for (b, &[hi, lo]) in a.burst.iter_mut().zip(hex.as_chunks::<2>().0) {
            match (hexval(hi), hexval(lo)) {
                (Some(hi), Some(lo)) => *b = hi << 4 | lo,
                _ => return Act::Stop(Stop::bad(b"raw_hex isn't hex")),
            }
        }
        // "n" only as a number in range (the C's cast of a huge one was undefined); else counted from the sync
        let n = match node(get(c"n")) {
            Some(n) if n.type_ == RDOF_JNUM && (-1.0..=5.0).contains(&n.num) => n.num as c_int,
            _ => -1,
        };
        let c = rdof_benchtx_control(&a.burst, n, &mut voice_n);
        if c < 0 {
            return Act::Stop(Stop::new(b"bad_request", |o| {
                o.bytes(b"item ");
                o.int(i.into(), 0);
                o.bytes(b" has no sync and isn't voice B-F after a voice sync");
            }));
        }
        a.control = c as u8;
    }
    // at_sc is ignored: a modem can't place a burst on the sample clock
    Act::Bursts { count, voice_n }
}

#[no_mangle]
pub extern "C" fn rdof_benchtx_alive(t: &mut RdofBenchtx, now: f64) {
    t.last_tool = now;
}

#[no_mangle]
pub unsafe extern "C" fn rdof_benchtx_tool(t: &mut RdofBenchtx, json: *const c_char, n: usize, now: f64) {
    if t.state == DONE {
        return;
    }
    t.last_tool = now;
    t.frames += 1;
    let q = Queue {
        qn: t.qn,
        voice_n: t.voice_n,
        keyed: t.keyed,
        air_free: t.air_free,
        key_start: t.key_start,
        max_key_s: t.max_key_s,
    };
    let mut add = [RdofBenchtxItem { control: 0, burst: [0; RDOF_BURST_BYTES] }; QUEUE];
    let act = {
        let mut d = JDoc {
            nodes: t.nodes.as_mut_ptr(),
            max: NODES as c_int,
            n: 0,
            buf: t.strbuf.as_mut_ptr(),
            cap: t.strbuf.len(),
            used: 0,
            error: [0; 64],
        };
        let root = rdof_jparse(&mut d, json, n);
        let ty = if root >= 0 { rdof_jget(&d, root, c"type".as_ptr()) } else { -1 };
        let ty = rdof_jnode(&d, ty).as_ref().filter(|n| ty >= 0 && n.type_ == RDOF_JSTR);
        match ty.map_or(&b""[..], |n| c_bytes(n.str_)) {
            b"bursts" => bursts(&d, root, &q, &mut add, now),
            b"unkey" => Act::Unkey,
            b"ping" => Act::Ping,
            b"fm" | b"iq" => Act::Stop(Stop::new(b"unsupported", |o| {
                o.bytes(b"a modem sends DMR bursts only: no FM tone or I/Q through it")
            })),
            _ => Act::Stop(Stop::bad(if root < 0 { b"not JSON" } else { b"unknown frame type" })),
        }
    };
    match act {
        Act::Bursts { count, voice_n } => {
            for a in &add[..count as usize] {
                t.q[(t.qh + t.qn) as usize % QUEUE] = *a;
                t.qn += 1;
            }
            t.voice_n = voice_n;
            let queued = f64::from(t.qn) * BURST_S + if t.keyed && t.air_free > now { t.air_free - now } else { 0.0 };
            let qn = t.qn;
            tool(t, |o| {
                o.bytes(b"{\"type\":\"ack\",\"bursts\":");
                o.int(count.into(), 0);
                o.bytes(b",\"queued\":");
                o.int(qn.into(), 0);
                o.bytes(b",\"queued_s\":");
                o.fixed(queued, 3);
                o.bytes(b",\"timed\":false}");
            });
        }
        Act::Unkey => unkey(t, c"unkey", now), // not keyed: the queue and the voice count dropped
        Act::Ping => {}
        Act::Stop(s) => stop(t, Some(s.code), &s.why[..s.n], now),
    }
}

// ── the modem's frames ──

unsafe extern "C" fn on_frame(ctx: *mut c_void, cmd: u8, p: *const u8, len: usize) {
    // the reader is borrowed while it calls this, so the job's other fields go through the pointer
    let t = ctx.cast::<RdofBenchtx>();
    let p = slice(p, len);
    let want = (*t).want;
    match (cmd, p.first()) {
        (GET_VERSION, _) if want == GET_VERSION => {
            (*t).protocol = rdof_mmdvm_parse_version(p.as_ptr(), p.len(), core::ptr::null_mut(), 0);
            (*t).replied = true;
        }
        (ACK, Some(&c)) if c == want => (*t).replied = true,
        (NAK, Some(&c)) if c == want => {
            (*t).replied = true;
            (*t).nak = true;
        }
        (NAK, Some(&(DMR_DATA1 | DMR_DATA2))) => {
            (*t).data_nak = p.get(1).map_or(NO_REASON, |&r| NAKED | c_int::from(r));
        }
        _ => {}
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_benchtx_modem(t: &mut RdofBenchtx, b: *const u8, n: usize, now: f64) {
    let ctx: *mut RdofBenchtx = t;
    rdof_mmdvm_reader_feed(&mut *core::ptr::addr_of_mut!((*ctx).reader), b, n, Some(on_frame), ctx.cast());
    let t = &mut *ctx;
    if t.data_nak != 0 && t.state != DONE {
        let mut why = [0u8; 96];
        let reason = t.data_nak;
        let w = c_out(why.as_mut_ptr().cast(), why.len(), |o| {
            o.bytes(b"the modem refused a burst (");
            if reason & NAKED != 0 {
                o.bytes(b"NAK reason ");
                o.int((reason & 0xFF).into(), 0);
            } else {
                o.bytes(b"no NAK reason");
            }
            o.bytes(b")");
        });
        stop(t, Some(b"refused"), &why[..w.min(why.len() - 1)], now);
    }
}

// ── time ──

fn setup_tick(t: &mut RdofBenchtx, now: f64) {
    if t.replied && !t.nak {
        t.want = NO_REPLY;
        t.step += 1;
        if t.step == STEP_DONE {
            t.state = READY;
            t.need_mode = false;
            let (freq, protocol) = (t.freq, t.protocol);
            tool(t, |o| {
                o.bytes(b"{\"type\":\"ready\",\"freq_hz\":");
                o.int(freq.into(), 0);
                o.bytes(b",\"protocol\":");
                o.int(protocol.into(), 0);
                o.bytes(b"}");
            });
            return;
        }
        t.tries = 0;
        request(t, now);
        return;
    }
    let name = STEP_NAMES.get(t.step as usize).copied().unwrap_or(b"?");
    let mut why = [0u8; 96];
    let say = |why: &mut [u8; 96], what: &[u8]| {
        let n = unsafe {
            c_out(why.as_mut_ptr().cast(), why.len(), |o| {
                o.bytes(what);
                o.bytes(name);
            })
        };
        n.min(why.len() - 1)
    };
    if t.nak {
        let n = say(&mut why, b"the modem refused ");
        let code: &[u8] = if t.step == STEP_FREQ { b"refused" } else { b"unavailable" };
        stop(t, Some(code), &why[..n], now);
    } else if now >= t.deadline {
        t.tries += 1;
        if t.tries < TRIES {
            request(t, now);
            return;
        }
        let n = say(&mut why, b"the modem didn't answer ");
        stop(t, Some(b"unavailable"), &why[..n], now);
    }
}

#[no_mangle]
pub extern "C" fn rdof_benchtx_tick(t: &mut RdofBenchtx, now: f64) {
    if t.state == DONE {
        return;
    }
    if now - t.last_tool > DEADMAN_S {
        unkey(t, c"deadman", now);
        stop(t, Some(b"timeout"), b"no frame from the tool for 2 s (dead-man)", now);
        return;
    }
    if t.state == SETUP {
        return setup_tick(t, now);
    }
    if t.need_mode && t.qn != 0 {
        // back to DMR after a stop, before the next burst
        t.state = SETUP;
        t.step = STEP_MODE;
        t.tries = 0;
        return request(t, now);
    }
    if t.keyed && t.air_free - t.key_start > t.max_key_s + 1e-9 {
        return unkey(t, c"max_key", now);
    }
    while t.qn != 0 && t.air_free - now < LEAD * BURST_S {
        if t.keyed && t.air_free + BURST_S - t.key_start > t.max_key_s + 1e-9 {
            return unkey(t, c"max_key", now);
        }
        let it = t.q[t.qh as usize % QUEUE];
        if !t.keyed {
            t.keyed = true;
            t.key_start = now;
            t.air_free = now;
            t.key_bursts = 0;
            t.keyings += 1;
            let (at, freq) = (utc(t, now), t.freq);
            tool(t, |o| {
                o.bytes(b"{\"type\":\"keyed\",\"t\":");
                o.fixed(at, 6);
                o.bytes(b",\"freq_hz\":");
                o.int(freq.into(), 0);
                o.bytes(b"}");
            });
        }
        let mut p = [0u8; 1 + RDOF_BURST_BYTES];
        p[0] = it.control;
        p[1..].copy_from_slice(&it.burst);
        modem(t, DMR_DATA2, &p);
        t.last_voice = it.control == SYNC_AUDIO || it.control & 0xF0 == 0;
        if it.control & 0xF0 == RDOF_SYNC_DATA && c_int::from(it.control & 0x0F) == RDOF_DT_TERMINATOR_WITH_LC {
            t.last_voice = false;
        }
        t.qh = (t.qh + 1) % QUEUE as c_int;
        t.qn -= 1;
        t.air_free = if t.air_free > now { t.air_free } else { now } + BURST_S;
        t.key_bursts += 1;
        t.sent += 1;
    }
    if t.keyed && t.qn == 0 && now >= t.air_free {
        if t.last_voice {
            // a call that stopped without its terminator: the tool fell behind
            t.underruns += 1;
            let at = utc(t, now);
            tool(t, |o| {
                o.bytes(b"{\"type\":\"underrun\",\"t\":");
                o.fixed(at, 6);
                o.bytes(b"}");
            });
            unkey(t, c"underrun", now);
        } else {
            unkey(t, c"end", now);
        }
    }
}

fn stop(t: &mut RdofBenchtx, code: Option<&[u8]>, why: &[u8], now: f64) {
    if t.state == DONE {
        return;
    }
    let keyed = t.keyed;
    unkey(t, c"stopped", now);
    if !keyed {
        // nothing on the air, but leave the modem idle
        modem(t, SET_MODE, &[MODE_IDLE]);
    }
    finish(t, code, why);
}

#[no_mangle]
pub unsafe extern "C" fn rdof_benchtx_stop(t: &mut RdofBenchtx, code: *const c_char, why: *const c_char, now: f64) {
    let code = if code.is_null() { None } else { Some(c_bytes(code)) };
    stop(t, code, c_bytes(why), now);
}

#[no_mangle]
pub extern "C" fn rdof_benchtx_done(t: &RdofBenchtx) -> bool {
    t.state == DONE
}

#[no_mangle]
pub extern "C" fn rdof_benchtx_state(t: &RdofBenchtx) -> *const c_char {
    let s: &CStr = match t.state {
        DONE => c"idle",
        SETUP => c"setup",
        _ if t.keyed => c"keyed",
        _ => c"armed",
    };
    s.as_ptr()
}
