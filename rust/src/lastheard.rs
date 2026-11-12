// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! Last heard: the ring of recent calls (rdof/lastheard.h). A port of common/src/lastheard.c. A frequency's text is
//! read with the C library's sscanf ("%lf") and rounded with its llround; everything else is here.

use core::ffi::{c_char, c_int, CStr};

use crate::bits::slice_mut;
use crate::ffi;
use crate::json::RdofJson;
use crate::jsonp::{rdof_jget, rdof_jnode, rdof_jparse, JDoc, JNode, RDOF_JBOOL, RDOF_JNUM, RDOF_JOBJ, RDOF_JSTR};
use crate::netcalls::{rdof_csbk_desc, RdofTxCallInfo};
use crate::text::{c_bytes, print, Piece};

// rdof_lh_dir_t
pub const RDOF_LH_RF: u32 = 0;
pub const RDOF_LH_NET: u32 = 1;
// rdof_lh_kind_t
pub const RDOF_LH_VOICE: u32 = 0;
pub const RDOF_LH_DATA: u32 = 1;
pub const RDOF_LH_CSBK: u32 = 2;

const NODES: usize = 40;
const SBUF: usize = 320;

/// rdof_lh_entry_t
#[repr(C)]
pub struct RdofLhEntry {
    pub id: u32,
    pub seq: u32,
    pub dir: u32,
    pub kind: u32,
    pub role: [c_char; 10],
    pub radio: [c_char; 24],
    pub channel: [c_char; 24],
    pub slot: c_int,
    pub cc: c_int,
    pub freq: u32,
    pub src: u32,
    pub dst: u32,
    pub group: bool,
    pub active: bool,
    pub late: bool,
    pub emergency: bool,
    pub encrypted: bool,
    pub start: f64,
    pub start_ms: i64,
    pub duration: f64,
    pub end: [c_char; 8],
    pub has_ber: bool,
    pub has_rssi: bool,
    pub ber: f32,
    pub rssi: i16,
    pub rssi_min: i16,
    pub rssi_max: i16,
    pub frames: c_int,
    pub desc: [c_char; 24],
}

/// rdof_lh_t
#[repr(C)]
pub struct RdofLh {
    pub e: *mut RdofLhEntry,
    pub cap: c_int,
    pub n: c_int,
    pub head: c_int,
    pub seq: u32,
    pub nodes: [JNode; NODES],
    pub sbuf: [c_char; SBUF],
}

/// A char array's bytes.
fn bytes(a: &[c_char]) -> &[u8] {
    unsafe { core::slice::from_raw_parts(a.as_ptr().cast(), a.len()) }
}

/// A char array's text, up to its NUL.
fn text(a: &[c_char]) -> &[u8] {
    let b = bytes(a);
    &b[..b.iter().position(|&c| c == 0).unwrap_or(b.len())]
}

/// snprintf(dst, sizeof dst, "%s", src): cut to fit, NUL terminated.
fn copy(dst: &mut [c_char], src: &[u8]) {
    let n = src.len().min(dst.len() - 1);
    for (d, &s) in dst.iter_mut().zip(&src[..n]) {
        *d = s as c_char;
    }
    dst[n] = 0;
}

/// !strncmp(a, b, sizeof a - 1)
unsafe fn same(a: &[c_char], b: *const c_char) -> bool {
    let a = bytes(a);
    for (k, &x) in a[..a.len() - 1].iter().enumerate() {
        if x != *b.add(k) as u8 {
            return false;
        }
        if x == 0 {
            return true;
        }
    }
    true
}

fn or_empty(s: *const c_char) -> *const c_char {
    if s.is_null() {
        c"".as_ptr()
    } else {
        s
    }
}

/// The ring's slot of the i-th newest (0 = the newest).
fn nth(head: c_int, cap: c_int, i: c_int) -> usize {
    (((head - 1 - i) % cap + cap) % cap) as usize
}

/// rdof_lh_t's ring, apart from the parse buffers the tracker's JSON is read into.
struct Ring<'a> {
    e: &'a mut [RdofLhEntry],
    n: &'a mut c_int,
    head: &'a mut c_int,
    seq: &'a mut u32,
}

impl RdofLh {
    /// The ring and a parse of the tracker's JSON into the nodes and sbuf.
    unsafe fn split(&mut self) -> (Ring<'_>, JDoc) {
        let e: &mut [RdofLhEntry] = if self.e.is_null() || self.cap <= 0 {
            &mut []
        } else {
            core::slice::from_raw_parts_mut(self.e, self.cap as usize)
        };
        let doc = JDoc {
            nodes: self.nodes.as_mut_ptr(),
            max: NODES as c_int,
            n: 0,
            buf: self.sbuf.as_mut_ptr(),
            cap: SBUF,
            used: 0,
            error: [0; 64],
        };
        (Ring { e, n: &mut self.n, head: &mut self.head, seq: &mut self.seq }, doc)
    }

    unsafe fn ring(&self) -> &[RdofLhEntry] {
        if self.e.is_null() || self.cap <= 0 {
            &[]
        } else {
            core::slice::from_raw_parts(self.e, self.cap as usize)
        }
    }
}

impl Ring<'_> {
    fn cap(&self) -> c_int {
        self.e.len() as c_int
    }

    fn touch(&mut self, i: usize) {
        *self.seq = self.seq.wrapping_add(1);
        self.e[i].seq = *self.seq;
    }

    unsafe fn find_active(&self, dir: u32, radio: *const c_char, channel: *const c_char, slot: c_int) -> Option<usize> {
        (0..*self.n).map(|i| nth(*self.head, self.cap(), i)).find(|&k| {
            let e = &self.e[k];
            e.active && e.dir == dir && e.slot == slot && same(&e.radio, radio) && same(&e.channel, channel)
        })
    }

    fn finish(&mut self, i: usize, reason: &[u8], mut duration: f64, now: f64) {
        let e = &mut self.e[i];
        e.active = false;
        copy(&mut e.end, reason);
        if duration < 0.0 || duration.is_nan() {
            duration = now - e.start;
        }
        e.duration = if duration > 0.0 { duration } else { 0.0 };
        self.touch(i);
    }

    /// A new entry. A call: any still active on that radio, channel and slot ends as lost.
    #[allow(clippy::too_many_arguments)]
    unsafe fn add(
        &mut self,
        dir: u32,
        role: *const c_char,
        radio: *const c_char,
        channel: *const c_char,
        slot: c_int,
        call: bool,
        now: f64,
        now_ms: i64,
    ) -> Option<usize> {
        if self.e.is_empty() {
            return None;
        }
        if call {
            if let Some(old) = self.find_active(dir, radio, channel, slot) {
                self.finish(old, b"lost", -1.0, now);
            }
        }
        let i = *self.head as usize;
        *self.head = (*self.head + 1) % self.cap();
        if *self.n < self.cap() {
            *self.n += 1;
        }
        let e = &mut self.e[i];
        core::ptr::write_bytes(e as *mut RdofLhEntry, 0, 1);
        e.dir = dir;
        copy(&mut e.role, c_bytes(role));
        copy(&mut e.radio, c_bytes(radio));
        copy(&mut e.channel, c_bytes(channel));
        e.slot = slot;
        e.cc = -1;
        e.frames = -1;
        e.start = now;
        e.start_ms = if now_ms > 0 { now_ms } else { 0 };
        e.active = call;
        self.touch(i);
        self.e[i].id = self.e[i].seq;
        Some(i)
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_lh_init(lh: *mut RdofLh, buf: *mut RdofLhEntry, cap: c_int) {
    core::ptr::write_bytes(lh, 0, 1);
    let lh = &mut *lh;
    lh.e = buf;
    lh.cap = if !buf.is_null() && cap > 0 { cap } else { 0 };
}

// ── RF: the tracker's JSON ─────────────────────────────────────────────────

/// An object of the parse.
struct J<'a> {
    doc: &'a JDoc,
    obj: c_int,
}

impl J<'_> {
    unsafe fn field(&self, key: &CStr) -> Option<&JNode> {
        rdof_jnode(self.doc, rdof_jget(self.doc, self.obj, key.as_ptr())).as_ref()
    }

    unsafe fn num(&self, key: &CStr) -> Option<f64> {
        self.field(key).filter(|n| n.type_ == RDOF_JNUM).map(|n| n.num)
    }

    unsafe fn str(&self, key: &CStr) -> Option<&CStr> {
        self.field(key).filter(|n| n.type_ == RDOF_JSTR).map(|n| CStr::from_ptr(n.str_))
    }

    /// src_id, dst_id and group, as a call start and a CSBK give them.
    unsafe fn who(&self) -> (u32, u32, bool) {
        let src = self.num(c"src_id").map_or(0, |v| v as u32);
        let dst = self.num(c"dst_id").map_or(0, |v| v as u32);
        (src, dst, self.str(c"group").is_some_and(|g| g.to_bytes() == b"yes"))
    }
}

// The C's comparisons, as written (a NaN passes through them).
#[allow(clippy::manual_clamp)]
fn clamp16(v: f64) -> i16 {
    (if v > 32767.0 {
        32767.0
    } else if v < -32768.0 {
        -32768.0
    } else {
        v
    }) as i16
}

#[allow(clippy::too_many_arguments)]
unsafe fn rf_start(
    lh: &mut Ring,
    j: &J,
    role: *const c_char,
    radio: *const c_char,
    channel: *const c_char,
    slot: c_int,
    late: bool,
    now: f64,
    now_ms: i64,
) -> bool {
    let (src, dst, group) = j.who();
    if let Some(e) = lh.find_active(RDOF_LH_RF, radio, channel, slot).map(|i| &lh.e[i]) {
        if late && e.src == src && e.dst == dst && e.group == group {
            return false; // the same call, picked up again
        }
    }
    let Some(i) = lh.add(RDOF_LH_RF, role, radio, channel, slot, true, now, now_ms) else {
        return false;
    };
    let e = &mut lh.e[i];
    e.src = src;
    e.dst = dst;
    e.group = group;
    e.late = late;
    if let Some(f) = j.str(c"frequency") {
        let mut mhz = 0.0f64;
        if ffi::sscanf(f.as_ptr(), c"%lf".as_ptr(), &mut mhz as *mut f64) == 1 && mhz > 0.0 && mhz < 4295.0 {
            e.freq = ffi::llround(mhz * 1e6) as u32;
        }
    }
    if let Some(v) = j.num(c"colorCode") {
        e.cc = v as c_int;
    }
    if let Some(v) = j.num(c"frames") {
        e.kind = RDOF_LH_DATA;
        e.frames = v as c_int;
    }
    e.emergency = j.field(c"emergency").is_some_and(|n| n.type_ == RDOF_JBOOL && n.b);
    true
}

unsafe fn rf_end(
    lh: &mut Ring,
    j: &J,
    radio: *const c_char,
    channel: *const c_char,
    slot: c_int,
    action: &[u8],
    now: f64,
) -> bool {
    let Some(i) = lh.find_active(RDOF_LH_RF, radio, channel, slot) else {
        return false;
    };
    let e = &mut lh.e[i];
    let dur = j.num(c"duration").unwrap_or(-1.0);
    if let Some(v) = j.num(c"ber") {
        e.has_ber = true;
        e.ber = v as f32;
    }
    let r = rdof_jget(j.doc, j.obj, c"rssi".as_ptr());
    if r >= 0 {
        let k = J { doc: j.doc, obj: r };
        if let Some(v) = k.num(c"ave") {
            e.has_rssi = true;
            e.rssi = clamp16(v);
            if let Some(v) = k.num(c"min") {
                e.rssi_min = clamp16(v);
            }
            if let Some(v) = k.num(c"max") {
                e.rssi_max = clamp16(v);
            }
        }
    }
    lh.finish(i, action, dur, now);
    true
}

#[allow(clippy::too_many_arguments)]
unsafe fn rf_csbk(
    lh: &mut Ring,
    j: &J,
    role: *const c_char,
    radio: *const c_char,
    channel: *const c_char,
    slot: c_int,
    now: f64,
    now_ms: i64,
) -> bool {
    let Some(desc) = j.str(c"csbk_desc") else {
        return false;
    };
    if desc.to_bytes() == b"Preamble" {
        return false; // a data call's preambles: the call itself shows
    }
    let (src, dst, group) = j.who();
    // a CSBK doesn't end a call: it's an entry of its own, never active
    let Some(i) = lh.add(RDOF_LH_RF, role, radio, channel, slot, false, now, now_ms) else {
        return false;
    };
    let e = &mut lh.e[i];
    e.kind = RDOF_LH_CSBK;
    e.src = src;
    e.dst = dst;
    e.group = group;
    copy(&mut e.desc, desc.to_bytes());
    true
}

#[no_mangle]
pub unsafe extern "C" fn rdof_lh_tracker_json(
    lh: &mut RdofLh,
    role: *const c_char,
    radio: *const c_char,
    channel: *const c_char,
    json: *const c_char,
    now: f64,
    now_ms: i64,
) -> bool {
    if lh.cap == 0 || json.is_null() {
        return false;
    }
    let (radio, channel) = (or_empty(radio), or_empty(channel));
    let (mut ring, mut doc) = lh.split();
    let root = rdof_jparse(&mut doc, json, CStr::from_ptr(json).to_bytes().len());
    let doc = &doc;
    let Some(r) = rdof_jnode(doc, root).as_ref().filter(|r| r.type_ == RDOF_JOBJ && r.child >= 0) else {
        return false;
    };
    let Some(top) = rdof_jnode(doc, r.child).as_ref().filter(|t| t.type_ == RDOF_JOBJ && !t.key.is_null()) else {
        return false;
    };
    let j = J { doc, obj: r.child };
    let Some(v) = j.num(c"slot") else {
        return false;
    };
    let slot = v as c_int;
    let key = CStr::from_ptr(top.key).to_bytes();
    if key == b"BER" || key == b"RSSI" {
        // a call's metrics, every second
        let Some(i) = ring.find_active(RDOF_LH_RF, radio, channel, slot) else {
            return false;
        };
        let Some(v) = j.num(c"value") else {
            return false;
        };
        let e = &mut ring.e[i];
        if key[0] == b'B' {
            e.has_ber = true;
            e.ber = v as f32;
        } else {
            e.has_rssi = true;
            e.rssi = clamp16(v);
        }
        ring.touch(i);
        return true;
    }
    if key != b"DMR" {
        return false;
    }
    let Some(action) = j.str(c"action") else {
        return false;
    };
    match action.to_bytes() {
        b"start" => rf_start(&mut ring, &j, role, radio, channel, slot, false, now, now_ms),
        b"late_entry" => rf_start(&mut ring, &j, role, radio, channel, slot, true, now, now_ms),
        a @ (b"end" | b"lost") => rf_end(&mut ring, &j, radio, channel, slot, a, now),
        b"csbk" => rf_csbk(&mut ring, &j, role, radio, channel, slot, now, now_ms),
        _ => false,
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_lh_privacy(
    lh: &mut RdofLh,
    radio: *const c_char,
    channel: *const c_char,
    slot: c_int,
) -> bool {
    let (mut ring, _) = lh.split();
    let Some(i) = ring.find_active(RDOF_LH_RF, or_empty(radio), or_empty(channel), slot) else {
        return false;
    };
    if ring.e[i].encrypted {
        return false;
    }
    ring.e[i].encrypted = true;
    ring.touch(i);
    true
}

// ── NET: what goes out ────────────────────────────────────────────────────────

#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn rdof_lh_tx(
    lh: &mut RdofLh,
    role: *const c_char,
    radio: *const c_char,
    action: *const c_char,
    call: Option<&RdofTxCallInfo>,
    duration: f64,
    now: f64,
    now_ms: i64,
) -> bool {
    let Some(call) = call else {
        return false;
    };
    if lh.cap == 0 || action.is_null() {
        return false;
    }
    let radio = or_empty(radio);
    let (mut ring, _) = lh.split();
    match c_bytes(action) {
        b"start" => {
            let Some(i) = ring.add(RDOF_LH_NET, role, radio, c"".as_ptr(), call.slot, !call.has_csbk, now, now_ms)
            else {
                return false;
            };
            let e = &mut ring.e[i];
            e.src = call.src;
            e.dst = call.dst;
            e.group = call.group;
            e.freq = call.freq;
            e.cc = call.cc;
            if call.has_csbk {
                let mut d = [0 as c_char; 48];
                rdof_csbk_desc(&call.csbk, d.as_mut_ptr(), d.len());
                e.kind = RDOF_LH_CSBK;
                copy(&mut e.desc, text(&d));
            } else if call.frames >= 0 {
                e.kind = RDOF_LH_DATA;
                e.frames = call.frames;
            }
            true
        }
        b"end" => {
            let Some(i) = ring.find_active(RDOF_LH_NET, radio, c"".as_ptr(), call.slot) else {
                return false;
            };
            ring.finish(i, b"end", duration, now);
            true
        }
        _ => false,
    }
}

// ── JSON ──────────────────────────────────────────────────────────────────────

const KINDS: [&[u8]; 3] = [b"voice", b"data", b"csbk"];

unsafe fn entry_json(e: &RdofLhEntry, now: f64, out: *mut c_char, cap: usize, len: &mut usize) -> bool {
    let j = &mut RdofJson::new(out, cap);
    j.open(None);
    j.int(c"id", e.id.into());
    j.int(c"seq", e.seq.into());
    j.str(c"dir", if e.dir == RDOF_LH_NET { b"net" } else { b"rf" });
    j.str(c"kind", KINDS[e.kind as usize]);
    j.str(c"role", text(&e.role));
    j.str(c"radio", text(&e.radio));
    if e.channel[0] != 0 {
        j.str(c"channel", text(&e.channel));
    }
    j.int(c"slot", e.slot.into());
    j.int(c"src", e.src.into());
    j.int(c"dst", e.dst.into());
    j.bool(c"group", e.group);
    if e.freq != 0 {
        j.int(c"freq", e.freq.into());
    }
    if e.cc >= 0 {
        j.int(c"cc", e.cc.into());
    }
    if e.start_ms != 0 {
        j.int(c"start_ms", e.start_ms);
    }
    let age = now - e.start;
    j.float3(c"age_s", if age > 0.0 { age } else { 0.0 });
    j.bool(c"active", e.active);
    if !e.active && e.kind != RDOF_LH_CSBK {
        j.float3(c"duration", e.duration);
        j.str(c"end", text(&e.end));
    }
    if e.has_ber {
        j.float3(c"ber", e.ber.into());
    }
    if e.has_rssi {
        j.int(c"rssi", e.rssi.into());
        if !e.active && (e.rssi_min != 0 || e.rssi_max != 0) {
            j.int(c"rssi_min", e.rssi_min.into());
            j.int(c"rssi_max", e.rssi_max.into());
        }
    }
    if e.frames >= 0 {
        j.int(c"frames", e.frames.into());
    }
    if e.desc[0] != 0 {
        j.str(c"desc", text(&e.desc));
    }
    if e.late {
        j.bool(c"late", true);
    }
    if e.emergency {
        j.bool(c"emergency", true);
    }
    if e.encrypted {
        j.bool(c"encrypted", true);
    }
    j.close();
    *len = j.len;
    j.done()
}

#[no_mangle]
pub unsafe extern "C" fn rdof_lh_json(
    lh: &RdofLh,
    mut since: u32,
    now: f64,
    now_ms: i64,
    out: *mut c_char,
    cap: usize,
) -> usize {
    use Piece::{I, S};
    if out.is_null() || cap < 64 {
        if !out.is_null() && cap != 0 {
            *out = 0;
        }
        return 0;
    }
    if since > lh.seq {
        since = 0; // the board restarted: everything again
    }
    let buf = slice_mut(out.cast(), cap);
    let mut n = print(
        buf,
        &[
            S(b"{\"seq\":"),
            I(lh.seq.into()),
            S(b",\"now_ms\":"),
            I(if now_ms > 0 { now_ms } else { 0 }),
            S(b",\"max\":"),
            I(lh.cap.into()),
            S(b",\"calls\":["),
        ],
    );
    let (mut first, mut more) = (true, false);
    const TAIL: usize = 16; // ],"more":true}
    if n + b"],\"more\":true}".len() + 1 > cap {
        buf[0] = 0; // no room to close what the header opened: nothing, not cut JSON
        return 0;
    }
    let ring = lh.ring();
    for i in 0..lh.n {
        let e = &ring[nth(lh.head, lh.cap, i)];
        if e.seq <= since {
            continue;
        }
        let sep = usize::from(!first);
        let mut len = 0;
        if n + 1 + TAIL >= cap
            || !entry_json(e, now, buf[n + sep..].as_mut_ptr().cast(), cap - n - sep - TAIL, &mut len)
        {
            more = true;
            // The C writes out[n] even when its header was cut (n >= cap, only with a cap near 64 and huge numbers).
            if let Some(c) = buf.get_mut(n) {
                *c = 0;
            }
            break;
        }
        if !first {
            buf[n] = b',';
        }
        n += len + sep;
        first = false;
    }
    let tail: &[u8] = if more { b"],\"more\":true}" } else { b"]}" };
    // The C's snprintf(out + n, cap - n, ...): past the end (the header cut, as above) it writes nothing here.
    n += match buf.get_mut(n..) {
        Some(rest) => print(rest, &[S(tail)]),
        None => tail.len(),
    };
    if n < cap {
        n
    } else {
        cap - 1
    }
}
