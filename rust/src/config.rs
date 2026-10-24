// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! Box config JSON -> one roamer or repeater (rdof/config.h, docs/spec/fw/config.md). A port of
//! common/src/config.c.
//!
//! The settings text is untrusted. Numbers in strings go through the C library's strtod and strtol, %g and floats
//! through its snprintf, so they read and print as the C's did.

use core::ffi::{c_char, c_int, c_long, CStr};
use core::mem::size_of;
use core::ptr;

use crate::bits::slice_mut;
use crate::ffi::{calloc, free, rint, snprintf, strtod, strtol};
use crate::hblink::RdofLocation;
use crate::jitter::{rdof_jb_bursts, RDOF_JB_BURST_MS, RDOF_JB_MAX_DEFAULT_MS, RDOF_JB_MAX_MS, RDOF_JB_MIN_MS};
use crate::json::rdof_format_float3;
use crate::jsonp::{
    rdof_jat, rdof_jget, rdof_jnode, rdof_jparse, rdof_jsonc_strip, JDoc, JNode, RDOF_JARR, RDOF_JBOOL, RDOF_JNULL, RDOF_JNUM, RDOF_JOBJ,
    RDOF_JSTR,
};
use crate::mmdvm::{rdof_modem_config_default, RdofModemConfig};
use crate::modem::{rdof_rssi_map_sort, RdofRssiMap};
use crate::slotgrid::{rdof_slot_timing_describe, rdof_slot_timing_parse, RdofSlotTiming};
use crate::status::rdof_format_repr;
use crate::text::{c_bytes, c_out, fixed, print, Out, Piece, Piece::I, Piece::S};

const MAX_NODES: usize = 1200;
const STR_BUF: usize = 8192;
const CHANNELS: usize = 32; // RDOF_CFG_CHANNELS
const MAX_RADIOS: usize = 2; // RDOF_MAX_RADIOS
const RSSI_POINTS: usize = 16; // RDOF_RSSI_POINTS

const ROAMER: u32 = 1; // RDOF_CFG_ROAMER
const REPEATER: u32 = 2;
const DUPLEX_NONE: u32 = 0; // rdof_sdr_duplex_t
const DUPLEX_HALF: u32 = 1;
const DUPLEX_FULL: u32 = 2;
const TUNER_MODEM: c_int = 0; // rdof_tuner_kind_t
const TUNER_FIXED: c_int = 1;
const TUNER_NONE: c_int = 2;

/// rdof_cfg_channel_t
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RdofCfgChannel {
    pub freq: u32,
    pub cc: c_int,
    pub power: c_int,
}

/// rdof_cfg_port_t
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RdofCfgPort {
    pub modem_port: [u8; 8],
    pub uart_tx: c_int,
    pub uart_rx: c_int,
    pub uart_num: c_int,
    pub uart_speed: c_long,
    pub usb_serial: [u8; 32],
    pub usb_port: [u8; 16],
}

/// rdof_config_t
#[repr(C)]
pub struct RdofConfig {
    pub mode: u32,
    pub name: [u8; 24],
    pub peer_id: u32,
    pub callsign: [u8; 12],
    pub master_host: [u8; 64],
    pub master_port: c_int,
    pub transport: [u8; 8],
    pub sctp_encap_port: c_int,
    pub sctp_heartbeat_ms: c_int,
    pub sctp_max_retransmit: c_int,
    pub sctp_ttl_ms: c_int,
    pub receiver_hblink: bool,

    pub radio_type: [u8; 16],
    pub modem: RdofModemConfig,
    pub rx_offset_hz: i32,
    pub tx_offset_hz: i32,
    pub tuner: c_int,
    pub tuner_freq: u32,
    pub rssi: RdofRssiMap,
    pub headers: c_int,
    pub terminators: c_int,
    pub settle_ms: c_int,

    pub channels: [RdofCfgChannel; CHANNELS],
    pub nchannels: c_int,
    pub idle: RdofCfgChannel,
    pub listen_s: f64,
    pub clear_s: f64,
    pub prebuffer: c_int,
    pub csbk_preambles: c_int,
    pub jitter_auto: bool,
    pub jitter_min_ms: c_int,
    pub jitter_max_ms: c_int,
    pub answer_s: f64,
    pub answer_max_s: f64,
    pub idle_after_s: f64,
    pub covered: [u32; CHANNELS],
    pub ncovered: c_int,
    pub has_scan: bool,
    pub scan_channels: [RdofCfgChannel; CHANNELS],
    pub nscan: c_int,
    pub dwell_s: f64,
    pub hang_s_scan: f64,
    pub carrier_s: f64,

    pub rx_freq: u32,
    pub tx_freq: u32,
    pub cc: c_int,
    pub power: c_int,
    pub slot1: bool,
    pub slot2: bool,
    pub hang_s: f64,
    pub call_hang_s: f64,
    pub short_lc_ts1_low: bool,
    pub sfr: bool,
    pub dcdm: bool,
    pub inbound_slot: c_int,
    pub slot_timing: RdofSlotTiming,

    pub cwid: [u8; 41],
    pub cwid_every_s: f64,

    pub has_location: bool,
    pub location: RdofLocation,

    pub has_mqtt: bool,
    pub mqtt_host: [u8; 64],
    pub mqtt_port: c_int,
    pub mqtt_user: [u8; 32],
    pub mqtt_pass: [u8; 64],
    pub mqtt_root: [u8; 16],
    pub mqtt_app: [u8; 16],
    pub mqtt_tls: bool,

    pub has_oled: bool,
    pub oled_address: c_int,
    pub oled_sh1106: bool,
    pub oled_rotate180: bool,
    pub oled_refresh_s: f64,
    pub oled_sda: c_int,
    pub oled_scl: c_int,
    pub oled_channel: c_int,

    pub modem_port: [u8; 8],
    pub sdr_duplex: u32,
    pub tx_power_dbm: f64,
    pub uart_tx: c_int,
    pub uart_rx: c_int,
    pub uart_num: c_int,
    pub uart_speed: c_long,
    pub usb_serial: [u8; 32],
    pub usb_port: [u8; 16],
    pub split: [RdofCfgPort; 2],
    pub split_power_cap: c_int,
    pub split_prebuffer: c_int,
    pub split_lease_s: c_int,
    pub split_cooldown_s: c_int,
    pub split_max_key_s: f64,
}

/// rdof_roles_t
#[repr(C)]
pub struct RdofRoles {
    pub box_: [u8; 32],
    pub has_radio: bool,
    pub has_sdr: bool,
    pub radio: [u8; 24],
    pub nradios: c_int,
    pub usb_hub: bool,
    pub radios: [[u8; 24]; MAX_RADIOS],
    pub sdr: [u8; 32],
    pub sdr_plain: bool,
}

const ROAMER_KEYS: &[&[u8]] = &[
    b"type",
    b"name",
    b"peer_id",
    b"callsign",
    b"idle",
    b"channels",
    b"listen_s",
    b"clear_s",
    b"prebuffer",
    b"jitter_buffer",
    b"jitter_min_ms",
    b"jitter_max_ms",
    b"idle_after_s",
    b"cwid",
    b"cwid_every_s",
    b"hblink",
    b"location",
    b"radio",
    b"scan",
    b"csbk_preambles",
    b"answer_s",
    b"answer_max_s",
];
const REPEATER_KEYS: &[&[u8]] = &[
    b"type",
    b"name",
    b"peer_id",
    b"callsign",
    b"rx_freq",
    b"tx_freq",
    b"cc",
    b"power",
    b"slots",
    b"hang_s",
    b"call_hang_s",
    b"prebuffer",
    b"jitter_buffer",
    b"jitter_min_ms",
    b"jitter_max_ms",
    b"cwid",
    b"cwid_every_s",
    b"hblink",
    b"location",
    b"radio",
    b"mode",
    b"freq",
    b"inbound_slot",
    b"short_lc_ts1_low",
    b"slot_timing",
];

/// What rdof_jnode gives for no node, as a node: the C's checks of NULL are the ones of null.
const NONE: JNode = JNode {
    type_: RDOF_JNULL,
    key: ptr::null(),
    str_: ptr::null(),
    num: 0.0,
    b: false,
    integer: false,
    child: -1,
    next: -1,
    count: 0,
};

// ── text ────────────────────────────────────────────────────────────────────

/// A NUL-terminated field's text.
fn field(f: &[u8]) -> &[u8] {
    &f[..f.iter().position(|&c| c == 0).unwrap_or(f.len())]
}

/// %ld (long is 32 bits on the S3, 64 on the host)
#[allow(clippy::useless_conversion)]
fn long(v: c_long) -> Piece<'static> {
    I(v.into())
}

/// snprintf(out, sizeof out, "%s", s)
fn put(out: &mut [u8], s: &[u8]) {
    print(out, &[S(s)]);
}

/// %g
fn g(v: f64, buf: &mut [u8; 32]) -> &[u8] {
    let n = unsafe { snprintf(buf.as_mut_ptr().cast(), buf.len(), c"%g".as_ptr(), v) };
    &buf[..(n.max(0) as usize).min(buf.len() - 1)]
}

/// A string node's text; "" for any other.
fn s(n: &JNode) -> &[u8] {
    if n.type_ == RDOF_JSTR {
        unsafe { c_bytes(n.str_) }
    } else {
        b""
    }
}

fn is_env(n: &JNode) -> bool {
    let t = s(n);
    t.len() >= 3 && t[0] == b'$' && t[1] == b'{' && t[t.len() - 1] == b'}'
}

/// The old name opengd77 becomes radio_hotspot.
fn canonical(t: &mut [u8]) {
    if field(t) == b"opengd77" {
        put(t, b"radio_hotspot");
    }
}

/// A radio type, or opengd77, radio_hotspot's old name.
fn known_type(t: &[u8]) -> bool {
    matches!(t, b"auto" | b"radio_hotspot" | b"opengd77" | b"mmdvm_hs" | b"mmdvm")
}

/// The C's lower-casing: A to Z only.
fn lower(t: &mut [u8]) {
    t.iter_mut().for_each(|c| c.make_ascii_lowercase());
}

/// strncmp(a, b, n) == 0
fn same_n(a: &[u8], b: &[u8], n: usize) -> bool {
    a[..a.len().min(n)] == b[..b.len().min(n)]
}

// ── the parsed settings ─────────────────────────────────────────────────────

#[repr(C)]
struct Ctx {
    doc: JDoc,
    nodes: [JNode; MAX_NODES],
    strs: [c_char; STR_BUF],
}

/// The parsed settings (the doc and its buffers on the heap, freed on drop) and where errors go.
struct Cx {
    ctx: *mut Ctx,
    clean: *mut c_char,
    out: *mut u8,
    cap: usize,
}

impl Drop for Cx {
    fn drop(&mut self) {
        unsafe {
            free(self.ctx.cast());
            free(self.clean.cast());
        }
    }
}

type R<T = ()> = Option<T>;

impl Cx {
    /// The settings parsed, and the root (-1: not JSON, the error said); None: no memory (said when there's
    /// an error buffer). Errors go to `error`, else to `dummy`; emptied.
    unsafe fn new(
        text: *const c_char,
        len: usize,
        error: *mut c_char,
        cap: usize,
        dummy: &mut [u8; 8],
    ) -> R<(Cx, c_int)> {
        let c = Cx {
            ctx: calloc(1, size_of::<Ctx>()).cast(),
            clean: calloc(len + 1, 1).cast(),
            out: dummy.as_mut_ptr(),
            cap: dummy.len(),
        };
        if c.ctx.is_null() || c.clean.is_null() {
            if !error.is_null() {
                print(slice_mut(error.cast(), cap), &[S(b"no memory")]);
            }
            return None;
        }
        let mut c = c;
        if !error.is_null() {
            (c.out, c.cap) = (error.cast(), cap);
        }
        if let Some(b) = slice_mut(c.out, c.cap).first_mut() {
            *b = 0;
        }
        let x = &mut *c.ctx;
        x.doc.nodes = x.nodes.as_mut_ptr();
        x.doc.max = MAX_NODES as c_int;
        x.doc.buf = x.strs.as_mut_ptr();
        x.doc.cap = STR_BUF;
        let root = rdof_jparse(&mut x.doc, c.clean, rdof_jsonc_strip(text, len, c.clean));
        if root < 0 {
            c.err::<()>(&[S(b"JSON: "), S(c_bytes(x.doc.error.as_ptr()))]);
        }
        Some((c, root))
    }

    /// The jitter buffer: jitter_buffer "auto" (the default) or a fixed size in ms, rounded up to whole 60 ms bursts;
    /// prebuffer, the older key, a fixed size in bursts. jitter_min_ms and jitter_max_ms bound auto.
    fn jitter(&self, r: c_int, cfg: &mut RdofConfig) -> R<()> {
        let (jb, pb) = (self.get(r, c"jitter_buffer"), self.get(r, c"prebuffer"));
        if jb >= 0 && pb >= 0 {
            return self.err(&[S(b"radio: prebuffer and jitter_buffer: set one (prebuffer is a fixed size in bursts)")]);
        }
        cfg.prebuffer = self.iget(r, c"prebuffer", 6, b"radio")? as c_int;
        cfg.jitter_auto = pb < 0;
        if jb >= 0 {
            let n = self.n(jb);
            if n.type_ == RDOF_JSTR && s(n) == b"auto" {
                cfg.jitter_auto = true;
            } else if let Some(ms) = self.num(jb).filter(|&ms| n.type_ != RDOF_JBOOL && (0.0..=RDOF_JB_MAX_MS).contains(&ms)) {
                cfg.jitter_auto = false;
                cfg.prebuffer = rdof_jb_bursts(ms);
            } else {
                return self.err(&[S(b"radio: jitter_buffer: \"auto\" or milliseconds, 0 to "), I(RDOF_JB_MAX_MS as i64)]);
            }
        }
        let lo = self.fget(r, c"jitter_min_ms", RDOF_JB_MIN_MS, b"radio")?;
        let hi = self.fget(r, c"jitter_max_ms", RDOF_JB_MAX_DEFAULT_MS, b"radio")?;
        if !(lo >= f64::from(RDOF_JB_BURST_MS) && lo <= hi && hi <= RDOF_JB_MAX_MS) {
            return self.err(&[
                S(b"radio: jitter_min_ms and jitter_max_ms: "),
                I(RDOF_JB_BURST_MS.into()),
                S(b" <= min <= max <= "),
                I(RDOF_JB_MAX_MS as i64),
            ]);
        }
        cfg.jitter_min_ms = rdof_jb_bursts(lo) * RDOF_JB_BURST_MS;
        cfg.jitter_max_ms = rdof_jb_bursts(hi) * RDOF_JB_BURST_MS;
        Some(())
    }

    fn doc(&self) -> &JDoc {
        unsafe { &(*self.ctx).doc }
    }

    /// snprintf(error, cap, ...); None, to return.
    fn err<T>(&self, pieces: &[Piece]) -> R<T> {
        print(unsafe { slice_mut(self.out, self.cap) }, pieces);
        None
    }

    /// Node i; NONE for -1 (or any node the parse didn't make).
    fn n(&self, i: c_int) -> &JNode {
        unsafe { rdof_jnode(self.doc(), i).as_ref() }.unwrap_or(&NONE)
    }

    fn get(&self, obj: c_int, key: &CStr) -> c_int {
        unsafe { rdof_jget(self.doc(), obj, key.as_ptr()) }
    }

    fn at(&self, node: c_int, k: c_int) -> c_int {
        unsafe { rdof_jat(self.doc(), node, k) }
    }

    // ── value helpers (int, float, string, bool) ──

    /// "Truthy": present, not null/false/0/""/{}/[] (env refs expand to "").
    fn truthy(&self, i: c_int) -> bool {
        let n = self.n(i);
        match n.type_ {
            RDOF_JNULL => false,
            RDOF_JBOOL => n.b,
            RDOF_JNUM => n.num != 0.0,
            RDOF_JSTR => !s(n).is_empty() && !is_env(n),
            _ => n.count > 0,
        }
    }

    fn num(&self, i: c_int) -> R<f64> {
        let n = self.n(i);
        match n.type_ {
            RDOF_JNUM => Some(n.num),
            RDOF_JBOOL => Some(if n.b { 1.0 } else { 0.0 }),
            RDOF_JSTR => {
                let mut e: *mut c_char = ptr::null_mut();
                let v = unsafe { strtod(n.str_, &mut e) };
                unsafe {
                    while *e == b' ' as c_char {
                        e = e.add(1);
                    }
                }
                // not hex
                let t = s(n);
                (!t.is_empty() && unsafe { *e } == 0 && !t.iter().any(|&c| c == b'x' || c == b'X')).then_some(v)
            }
            _ => None,
        }
    }

    /// float(d.get(key, dflt))
    fn fget(&self, obj: c_int, key: &CStr, dflt: f64, what: &[u8]) -> R<f64> {
        let i = self.get(obj, key);
        if i < 0 {
            return Some(dflt);
        }
        self.num(i).or_else(|| self.err(&[S(what), S(b": "), S(key.to_bytes()), S(b" isn't a number")]))
    }

    /// int(d.get(key, dflt))
    fn iget(&self, obj: c_int, key: &CStr, dflt: c_long, what: &[u8]) -> R<c_long> {
        let i = self.get(obj, key);
        if i < 0 {
            return Some(dflt);
        }
        let n = self.n(i);
        if n.type_ == RDOF_JSTR {
            let mut e: *mut c_char = ptr::null_mut();
            let x = unsafe { strtol(n.str_, &mut e, 10) };
            if s(n).is_empty() || unsafe { *e } != 0 {
                return self.err(&[S(what), S(b": "), S(key.to_bytes()), S(b" isn't a whole number")]);
            }
            return Some(x);
        }
        match self.num(i) {
            Some(v) => Some(v as c_long), // int() truncates (saturating, NaN 0)
            None => self.err(&[S(what), S(b": "), S(key.to_bytes()), S(b" isn't a number")]),
        }
    }

    fn text_of(&self, i: c_int, out: &mut [u8]) {
        let n = self.n(i);
        match n.type_ {
            RDOF_JNULL => put(out, b"None"),
            RDOF_JSTR => put(out, if is_env(n) { b"" } else { s(n) }),
            RDOF_JNUM if n.integer => {
                fixed(n.num + 0.0, 0, out); // -0 is 0
            }
            RDOF_JNUM => unsafe { rdof_format_repr(n.num, out.as_mut_ptr().cast(), out.len()) }, // a float as text
            RDOF_JBOOL => put(out, if n.b { b"True" } else { b"False" }),
            _ => put(out, b"?"),
        }
    }

    /// str(d.get(key, dflt))
    fn sget(&self, obj: c_int, key: &CStr, dflt: &[u8], out: &mut [u8]) {
        let i = self.get(obj, key);
        if i < 0 {
            put(out, dflt);
        } else {
            self.text_of(i, out);
        }
    }

    /// d.get(key) or dflt (falsy -> dflt) as text
    fn sor(&self, obj: c_int, key: &CStr, dflt: &[u8], out: &mut [u8]) {
        let i = self.get(obj, key);
        if self.truthy(i) {
            self.text_of(i, out);
        } else {
            put(out, dflt);
        }
    }

    fn bget(&self, obj: c_int, key: &CStr, dflt: bool) -> bool {
        let i = self.get(obj, key);
        if i < 0 {
            dflt
        } else {
            self.truthy(i)
        }
    }

    /// box._hz: MHz below 1e5
    fn box_hz(&self, i: c_int, what: &[u8]) -> R<u32> {
        match self.num(i) {
            Some(f) => Some(unsafe { rint(if f < 1e5 { f * 1e6 } else { f }) } as u32),
            None => self.err(&[S(what), S(b": bad frequency")]),
        }
    }

    // ── the plan ──

    /// A radio's channel entry -> (plan channel, override object or -1).
    fn plan_ref(&self, plan: &Plan, r: c_int) -> R<(PlanCh, c_int)> {
        let mut over = -1;
        let n: c_long;
        if self.n(r).type_ == RDOF_JOBJ {
            over = r;
            match self.iget(r, c"ch", 0, b"channel") {
                Some(v) if self.get(r, c"ch") >= 0 => n = v,
                _ => return self.err(&[S(b"a channel entry needs ch")]),
            }
        } else {
            match self.num(r) {
                Some(v) => n = v as c_long,
                None => return self.err(&[S(b"bad channel reference")]),
            }
        }
        match plan.find(n) {
            Some(pc) => Some((pc, over)),
            None => self.err(&[S(b"channel "), long(n), S(b" isn't in the plan (channels:)")]),
        }
    }

    // ── the location ──

    fn location(&self, i: c_int, cfg: &mut RdofConfig) -> R {
        cfg.has_location = false;
        if !self.truthy(i) {
            return Some(());
        }
        let (mut la, mut lo) = (self.get(i, c"latitude"), self.get(i, c"longitude"));
        if la < 0 {
            la = self.get(i, c"lat");
        }
        if lo < 0 {
            lo = self.get(i, c"lon");
        }
        let (Some(lat), Some(lon)) = (self.num(la), self.num(lo)) else {
            return self.err(&[S(b"location: latitude and longitude")]);
        };
        if !((-90.0..=90.0).contains(&lat) && (-180.0..=180.0).contains(&lon)) {
            let (mut a, mut b) = ([0u8; 32], [0u8; 32]);
            return self.err(&[S(b"location "), S(g(lat, &mut a)), S(b", "), S(g(lon, &mut b)), S(b" out of range")]);
        }
        let h = self.fget(i, c"height", 0.0, b"location")?;
        let hh = unsafe { rint(h) } as c_long;
        cfg.location = RdofLocation { latitude: lat, longitude: lon, height_m: hh.clamp(0, 999) as c_int };
        cfg.has_location = true;
        Some(())
    }

    // ── the radio section ──

    /// A radio setting: the section's own key (not one of its section keys) wins over radio.{key}.
    fn rget(&self, r: c_int, keys: &[&[u8]], key: &CStr) -> c_int {
        if !keys.contains(&key.to_bytes()) {
            let i = self.get(r, key);
            if i >= 0 {
                return i;
            }
        }
        self.get(self.get(r, c"radio"), key)
    }

    /// radio.protocol (or a split half's): uart, else refused. `what`: "radio." or "radio.tx." ...
    fn protocol(&self, i: c_int, h: c_int, what: &[u8], name: &[u8]) -> R {
        let mut proto = [0u8; 12];
        put(&mut proto, b"uart");
        if i >= 0 {
            self.text_of(i, &mut proto);
        }
        lower(&mut proto);
        let p = field(&proto);
        if p != b"uart" && p != b"udp" {
            return self.err(&[S(what), S(b"protocol "), S(p), S(b": uart or udp")]);
        }
        if p == b"udp" {
            if !self.truthy(h) {
                return self.err(&[S(what), S(b"protocol udp needs radio."), S(name), S(b"modem_address")]);
            }
            return self.err(&[S(what), S(b"protocol udp: not on this board")]);
        }
        Some(())
    }

    fn radio_settings(&self, r: c_int, keys: &[&[u8]], cfg: &mut RdofConfig) -> R {
        rdof_modem_config_default(&mut cfg.modem);
        // 0 bool, 1 int, 2 double
        const F: [(&CStr, u8); 10] = [
            (c"rx_invert", 0),
            (c"tx_invert", 0),
            (c"ptt_invert", 0),
            (c"tx_delay_ms", 1),
            (c"rx_level", 2),
            (c"tx_level", 2),
            (c"cwid_level", 2),
            (c"rx_dc_offset", 1),
            (c"tx_dc_offset", 1),
            (c"dmr_delay", 1),
        ];
        for (k, &(key, t)) in F.iter().enumerate() {
            let i = self.rget(r, keys, key);
            if i < 0 {
                continue;
            }
            let (b, v) = if t == 0 {
                (self.truthy(i), 0.0)
            } else {
                match self.num(i) {
                    Some(v) => (false, v),
                    None => return self.err(&[S(b"radio."), S(key.to_bytes()), S(b" isn't a number")]),
                }
            };
            let m = &mut cfg.modem;
            match k {
                0 => m.rx_invert = b,
                1 => m.tx_invert = b,
                2 => m.ptt_invert = b,
                3 => m.tx_delay_ms = v as c_int,
                4 => m.rx_level = v,
                5 => m.tx_level = v,
                6 => m.cwid_level = v,
                7 => m.rx_dc_offset = v as c_int,
                8 => m.tx_dc_offset = v as c_int,
                _ => m.dmr_delay = v as c_int,
            }
        }
        cfg.rx_offset_hz = 0;
        cfg.tx_offset_hz = 0;
        if let Some(v) = self.num(self.rget(r, keys, c"rx_offset_hz")) {
            cfg.rx_offset_hz = v as i32;
        }
        if let Some(v) = self.num(self.rget(r, keys, c"tx_offset_hz")) {
            cfg.tx_offset_hz = v as i32;
        }
        cfg.headers = -1;
        cfg.terminators = -1;
        let i = self.rget(r, keys, c"headers");
        if let Some(v) = self.num(i).filter(|_| self.n(i).type_ != RDOF_JNULL) {
            cfg.headers = v as c_int;
        }
        let i = self.rget(r, keys, c"terminators");
        if let Some(v) = self.num(i).filter(|_| self.n(i).type_ != RDOF_JNULL) {
            cfg.terminators = v as c_int;
        }
        // tuner: "fixed" / {type, freq, settle_ms}; rigctld and command tuners need a computer
        cfg.tuner = TUNER_NONE; // the kind's default
        cfg.tuner_freq = 0;
        cfg.settle_ms = 0;
        let i = self.rget(r, keys, c"tuner");
        if self.truthy(i) {
            let mut t = [0u8; 16];
            if self.n(i).type_ == RDOF_JSTR {
                put(&mut t, s(self.n(i)));
            } else {
                self.sget(i, c"type", b"", &mut t);
                let f = self.get(i, c"freq");
                if f >= 0 {
                    match self.num(f) {
                        Some(v) => cfg.tuner_freq = freq_hz(v),
                        None => return self.err(&[S(b"radio.tuner.freq")]),
                    }
                }
                cfg.settle_ms = self.fget(i, c"settle_ms", 0.0, b"tuner")? as c_int;
            }
            match field(&t) {
                b"modem" => cfg.tuner = TUNER_MODEM,
                b"fixed" => cfg.tuner = TUNER_FIXED,
                t @ (b"rigctld" | b"command") => {
                    return self.err(&[S(b"radio.tuner "), S(t), S(b": not on this board (modem or fixed only)")])
                }
                b"" => {}
                t => return self.err(&[S(b"radio.tuner "), S(t)]),
            }
        }
        cfg.rssi.n = 0;
        let i = self.rget(r, keys, c"rssi");
        if self.truthy(i) {
            const WANTS: Piece = S(b"radio.rssi wants [[raw, dBm], ...]");
            if self.n(i).type_ != RDOF_JARR {
                return self.err(&[WANTS]);
            }
            for k in 0..self.n(i).count.min(RSSI_POINTS as c_int) {
                let p = self.at(i, k);
                let np = self.n(p);
                let pair = np.type_ == RDOF_JARR && np.count == 2;
                let (Some(a), Some(b)) =
                    (if pair { (self.num(self.at(p, 0)), self.num(self.at(p, 1))) } else { (None, None) })
                else {
                    return self.err(&[WANTS]);
                };
                cfg.rssi.raw[k as usize] = a as c_int;
                cfg.rssi.dbm[k as usize] = b as c_int;
                cfg.rssi.n = k + 1;
            }
            rdof_rssi_map_sort(&mut cfg.rssi);
        }
        // check_radio
        self.protocol(self.rget(r, keys, c"protocol"), self.rget(r, keys, c"modem_address"), b"radio.", b"")
    }

    /// A split's halves (radio.tx, radio.rx): a radio's checks on each. Their board ports come
    /// later.
    fn split_halves(&self, r: c_int) -> R {
        for (k, key) in [c"tx", c"rx"].into_iter().enumerate() {
            let name = key.to_bytes();
            let h = self.rget(r, REPEATER_KEYS, key);
            if h < 0 || self.n(h).type_ != RDOF_JOBJ {
                let half: &[u8] = if k == 1 { b"RX" } else { b"TX" };
                return self.err(&[
                    S(b"radio type split needs radio."),
                    S(name),
                    S(b": the "),
                    S(half),
                    S(b" half's port"),
                ]);
            }
            let mut t = [0u8; 16];
            self.sget(h, c"type", b"auto", &mut t);
            canonical(&mut t);
            let t = field(&t);
            if t != b"auto" && t != b"radio_hotspot" {
                return self.err(&[
                    S(b"radio."),
                    S(name),
                    S(b".type "),
                    S(t),
                    S(b": a split half is a radio in hotspot mode with the split duplex extension (OpenDigitalRadio)"),
                ]);
            }
            let mut what = [0u8; 12];
            print(&mut what, &[S(b"radio."), S(name), S(b".")]);
            let mut prefix = [0u8; 12];
            print(&mut prefix, &[S(name), S(b".")]);
            self.protocol(self.get(h, c"protocol"), self.get(h, c"modem_address"), field(&what), field(&prefix))?;
        }
        Some(())
    }

    /// Firmware only: each half's port on the board (modem, uart, speed, usb, as a radio's own) and SET_ROLE's
    /// parameters.
    fn split_ports(&self, r: c_int, cfg: &mut RdofConfig) -> R {
        let (mut usb, mut uart, mut sdr) = (0, 0, 0);
        for (k, key) in [c"tx", c"rx"].into_iter().enumerate() {
            let name = key.to_bytes();
            let h = self.rget(r, REPEATER_KEYS, key);
            let p = &mut cfg.split[k];
            self.sget(h, c"modem", b"usb", &mut p.modem_port);
            let mp = field(&p.modem_port);
            if mp != b"usb" && mp != b"uart" && mp != b"sdr" {
                return self.err(&[
                    S(b"radio."),
                    S(name),
                    S(b".modem "),
                    S(mp),
                    S(b": usb, uart, or sdr (the board's SDR modem)"),
                ]);
            }
            let u = self.get(h, c"uart");
            p.uart_tx = self.iget(u, c"tx", 5, b"uart")? as c_int;
            p.uart_rx = self.iget(u, c"rx", 6, b"uart")? as c_int;
            let v = self.iget(u, c"num", 1, b"uart")?;
            if v != 1 && v != 2 {
                return self.err(&[
                    S(b"radio."),
                    S(name),
                    S(b".uart.num "),
                    long(v),
                    S(b": 1 or 2 (UART0 is the console)"),
                ]);
            }
            p.uart_num = v as c_int;
            p.uart_speed = self.iget(h, c"speed", 115200, b"radio")?;
            let ub = self.get(h, c"usb");
            self.sget(ub, c"serial", b"", &mut p.usb_serial);
            self.sget(ub, c"port", b"", &mut p.usb_port);
            let p = *p;
            match field(&p.modem_port) {
                b"sdr" => sdr += 1,
                b"usb" => {
                    usb += 1;
                    put(&mut cfg.usb_serial, field(&p.usb_serial));
                    put(&mut cfg.usb_port, field(&p.usb_port));
                }
                _ => {
                    uart += 1;
                    if uart == 1 {
                        cfg.uart_tx = p.uart_tx;
                        cfg.uart_rx = p.uart_rx;
                        cfg.uart_num = p.uart_num;
                        cfg.uart_speed = p.uart_speed;
                    }
                }
            }
        }
        if sdr > 1 {
            return self
                .err(&[S(b"radio.tx and radio.rx are both the SDR: the board runs one SDR modem (two SDRs as a \
                                   split need two boards, or a radio for one half)")]);
        }
        if usb > 1 {
            return self
                .err(&[S(b"radio.tx and radio.rx are both on USB: one half on USB at most (the other on a UART, \
                                   or the SDR as the RX half)")]);
        }
        if uart > 1 {
            let (a, b) = (&cfg.split[0], &cfg.split[1]);
            if a.uart_num == b.uart_num {
                return self.err(&[
                    S(b"radio.tx and radio.rx are both on UART"),
                    I(a.uart_num.into()),
                    S(b": give one \"uart\": {\"num\": 2, ...}"),
                ]);
            }
            if a.uart_tx == b.uart_tx || a.uart_rx == b.uart_rx || a.uart_tx == b.uart_rx || a.uart_rx == b.uart_tx {
                return self.err(&[S(b"radio.tx and radio.rx share a UART pin")]);
            }
        }
        for (k, key) in [c"power_cap", c"prebuffer", c"lease_s", c"cooldown_s"].into_iter().enumerate() {
            let i = self.rget(r, REPEATER_KEYS, key);
            if i < 0 {
                continue;
            }
            let v = match self.num(i) {
                Some(v) if (0.0..=255.0).contains(&v) => v as c_int, // NaN refused too
                _ => return self.err(&[S(b"radio."), S(key.to_bytes()), S(b": 0 to 255")]),
            };
            *[&mut cfg.split_power_cap, &mut cfg.split_prebuffer, &mut cfg.split_lease_s, &mut cfg.split_cooldown_s]
                [k] = v;
        }
        let i = self.rget(r, REPEATER_KEYS, c"max_key_s");
        if i >= 0 {
            match self.num(i) {
                Some(v) if (0.0..=600.0).contains(&v) => cfg.split_max_key_s = v,
                _ => return self.err(&[S(b"radio.max_key_s: 0 to 600")]),
            }
        }
        put(&mut cfg.modem_port, b"split");
        Some(())
    }

    /// A radio's name: its own, else the default "{box}-{type}{n}".
    fn radio_name(&self, r: c_int, k: c_int, box_name: &[u8], out: &mut [u8]) {
        let t = self.get(r, c"type");
        let mut dflt = [0u8; 96];
        print(&mut dflt, &[S(box_name), S(b"-"), S(s(self.n(t))), I((k + 1).into())]);
        self.sget(r, c"name", field(&dflt), out);
    }

    /// A radio's "enabled": 1 (true or not there), 0 (false: left out, as if it weren't in radios), -1 (not a bool).
    fn enabled(&self, r: c_int) -> c_int {
        let e = self.get(r, c"enabled");
        if e < 0 {
            return 1;
        }
        let n = self.n(e);
        if n.type_ == RDOF_JBOOL { c_int::from(n.b) } else { -1 }
    }

    /// Is `name` in "run" (a name or a list of names)?
    fn in_run(&self, run: c_int, name: &[u8]) -> bool {
        let n = self.n(run);
        if n.type_ == RDOF_JSTR {
            return s(n) == name;
        }
        n.type_ == RDOF_JARR
            && (0..n.count).any(|k| {
                let e = self.n(self.at(run, k));
                e.type_ == RDOF_JSTR && s(e) == name
            })
    }

    /// The radios this board runs: with the firmware key "run", the ones it names (up to RDOF_MAX_RADIOS roamers or
    /// repeaters and one SDR); without it, the first roamer or repeater, or failing that the first SDR. A radio with
    /// "enabled": false is left out; "run" may name it.
    fn pick_radios(&self, bx: c_int, box_name: &[u8]) -> R<Picked> {
        let (radios, run) = (self.get(bx, c"radios"), self.get(bx, c"run"));
        let mut p = Picked { radio: [-1; MAX_RADIOS], radio_k: [0; MAX_RADIOS], nradio: 0, sdr: -1, sdr_k: 0 };
        let rn = self.n(radios);
        if radios >= 0 && rn.type_ != RDOF_JARR && rn.type_ != RDOF_JNULL {
            return self.err(&[S(b"radios: a list")]);
        }
        let (mut on, mut off_in_run) = (0, 0); // radios enabled; disabled ones "run" names
        for k in 0..rn.count {
            let sx = self.at(radios, k);
            match self.enabled(sx) {
                -1 => return self.err(&[S(b"radios["), I(k.into()), S(b"]: enabled: true or false")]),
                0 => {
                    let mut name = [0u8; 96];
                    self.radio_name(sx, k, box_name, &mut name);
                    off_in_run += c_int::from(run >= 0 && self.in_run(run, field(&name)));
                    continue;
                }
                _ => on += 1,
            }
            let ty = s(self.n(self.get(sx, c"type")));
            if ty != b"sdr" && ty != b"repeater" && ty != b"sdr_transceiver" && !known_type(ty) {
                return self.err(&[
                    S(b"radios["),
                    I(k.into()),
                    S(b"]: type "),
                    S(ty),
                    S(b": sdr, repeater, sdr_transceiver or a roamer (auto, mmdvm_hs, radio_hotspot, mmdvm; \
                         opengd77 = radio_hotspot)"),
                ]);
            }
            let is_sdr = ty == b"sdr";
            if run >= 0 {
                let mut name = [0u8; 96];
                self.radio_name(sx, k, box_name, &mut name);
                if !self.in_run(run, field(&name)) {
                    continue;
                }
                if is_sdr && p.sdr >= 0 {
                    return self.err(&[S(b"run: one SDR a board")]);
                }
                if !is_sdr && p.nradio >= MAX_RADIOS {
                    return self.err(&[S(b"run: "), I(MAX_RADIOS as i64), S(b" roamers or repeaters a board at most")]);
                }
            } else if p.nradio > 0 || (is_sdr && p.sdr >= 0) {
                continue;
            }
            if is_sdr {
                p.sdr = sx;
                p.sdr_k = k;
            } else {
                p.radio[p.nradio] = sx;
                p.radio_k[p.nradio] = k;
                p.nradio += 1;
                if run < 0 {
                    p.sdr = -1; // without run, a roamer or repeater wins
                }
            }
        }
        if on == 0 {
            return self.err(&[S(b"no radios")]);
        }
        if run >= 0 {
            const NAMES: Piece = S(b"run: a radio's name or a list of names");
            let n = self.n(run);
            if n.type_ != RDOF_JSTR && n.type_ != RDOF_JARR {
                return self.err(&[NAMES]);
            }
            if n.type_ == RDOF_JARR && (0..n.count).any(|k| self.n(self.at(run, k)).type_ != RDOF_JSTR) {
                return self.err(&[NAMES]);
            }
            let names = if n.type_ == RDOF_JSTR { 1 } else { n.count };
            if names != (p.nradio + usize::from(p.sdr >= 0)) as c_int + off_in_run {
                return self.err(&[S(b"run: a name that isn't in radios")]);
            }
        }
        Some(p)
    }

    // ── the box ──

    fn channel_entry(&self, freq: u32, cc: f64, power: f64, out: &mut RdofCfgChannel) -> R {
        if !(0.0..=15.0).contains(&cc) || cc as c_long as f64 != cc {
            let mut a = [0u8; 32];
            return self.err(&[S(b"color code "), S(g(cc, &mut a))]);
        }
        *out = RdofCfgChannel { freq, cc: cc as c_int, power: (power as c_long).clamp(0, 255) as c_int };
        Some(())
    }

    fn roamer(&self, bx: c_int, r: c_int, plan: &Plan, cfg: &mut RdofConfig) -> R {
        cfg.mode = ROAMER;
        // channels
        let refs = self.get(r, c"channels");
        cfg.nchannels = 0;
        let rn = self.n(refs);
        let all = refs < 0 || (rn.type_ == RDOF_JSTR && s(rn) == b"all");
        let count = if all {
            plan.n as c_int
        } else if self.truthy(refs) {
            rn.count
        } else {
            0
        };
        if !all && self.truthy(refs) && rn.type_ != RDOF_JARR {
            return self.err(&[S(b"channels: numbers, or all")]);
        }
        for k in 0..count {
            let (pc, over) = if all { (plan.ch[k as usize], -1) } else { self.plan_ref(plan, self.at(refs, k))? };
            let cc = self.fget(over, c"cc", pc.cc.into(), b"channel")?;
            let power = self.fget(over, c"power", pc.power.into(), b"channel")?;
            let n = cfg.nchannels as usize;
            if n >= CHANNELS {
                return self.err(&[S(b"too many channels ("), I(CHANNELS as i64), S(b" at most)")]);
            }
            cfg.nchannels += 1;
            self.channel_entry(freq_hz(pc.freq.into()), cc, power, &mut cfg.channels[n])?;
        }
        // idle
        let idle = self.get(r, c"idle");
        if idle < 0 {
            return self.err(&[S(b"no idle channel (idle: a channel number)")]);
        }
        if self.n(idle).type_ == RDOF_JOBJ && self.get(idle, c"freq") >= 0 {
            let f = self.box_hz(self.get(idle, c"freq"), b"idle");
            let (Some(f), Some(cc)) = (f, f.and_then(|_| self.num(self.get(idle, c"cc")))) else {
                return self.err(&[S(b"idle: freq and cc")]);
            };
            self.channel_entry(freq_hz(f.into()), cc as c_long as f64, 255.0, &mut cfg.idle)?;
        } else {
            let (pc, _) = self.plan_ref(plan, idle)?;
            self.channel_entry(freq_hz(pc.freq.into()), pc.cc.into(), 255.0, &mut cfg.idle)?;
            let n = cfg.nchannels as usize;
            if !cfg.channels[..n].iter().any(|c| c.freq == pc.freq) {
                if n >= CHANNELS {
                    return self.err(&[S(b"too many channels")]);
                }
                cfg.nchannels += 1;
                self.channel_entry(pc.freq, pc.cc.into(), pc.power.into(), &mut cfg.channels[n])?;
            }
            cfg.oled_channel = pc.number;
        }
        // the radio
        put(&mut cfg.radio_type, s(self.n(self.get(r, c"type"))));
        canonical(&mut cfg.radio_type);
        if field(&cfg.radio_type) == b"sdr_transceiver" {
            put(&mut cfg.radio_type, b"auto"); // the SDR's emulated modem answers as one
        }
        self.radio_settings(r, ROAMER_KEYS, cfg)?;
        if !known_type(field(&cfg.radio_type)) {
            return self.err(&[S(b"radio type "), S(field(&cfg.radio_type))]);
        }
        self.peer_and_callsign(r, cfg)?;
        // Config.from_dict's numbers
        cfg.listen_s = self.fget(r, c"listen_s", 0.3, b"radio")?;
        cfg.clear_s = self.fget(r, c"clear_s", 2.0, b"radio")?;
        cfg.answer_s = self.fget(r, c"answer_s", 0.75, b"radio")?;
        cfg.answer_max_s = self.fget(r, c"answer_max_s", 5.0, b"radio")?;
        cfg.idle_after_s = self.fget(r, c"idle_after_s", 0.0, b"radio")?;
        cfg.cwid_every_s = self.fget(r, c"cwid_every_s", 900.0, b"radio")?;
        self.jitter(r, cfg)?;
        cfg.csbk_preambles = self.iget(r, c"csbk_preambles", 2, b"radio")? as c_int;
        self.sget(r, c"cwid", b"", &mut cfg.cwid);
        // covered: this box's SDR channels
        cfg.ncovered = 0;
        let radios = self.get(bx, c"radios");
        for k in 0..self.n(radios).count {
            let sx = self.at(radios, k);
            let t = self.get(sx, c"type");
            if t < 0 || s(self.n(t)) != b"sdr" || self.enabled(sx) != 1 {
                continue;
            }
            let chs = self.get(sx, c"channels");
            for m in 0..self.n(chs).count {
                let (pc, _) = self.plan_ref(plan, self.at(chs, m))?;
                let n = cfg.ncovered as usize;
                if !cfg.covered[..n].contains(&pc.freq) && n < CHANNELS {
                    cfg.covered[n] = pc.freq;
                    cfg.ncovered += 1;
                }
            }
        }
        let covered = &mut cfg.covered[..cfg.ncovered as usize]; // sorted
        for a in 1..covered.len() {
            let mut b = a;
            while b > 0 && covered[b - 1] > covered[b] {
                covered.swap(b, b - 1);
                b -= 1;
            }
        }
        // scan
        cfg.has_scan = false;
        let sc = self.get(r, c"scan");
        if self.truthy(sc) {
            cfg.has_scan = true;
            let is_obj = self.n(sc).type_ == RDOF_JOBJ;
            let sref = if is_obj { self.get(sc, c"channels") } else { -1 };
            let sall = sref < 0 || (self.n(sref).type_ == RDOF_JSTR && s(self.n(sref)) == b"all");
            cfg.nscan = 0;
            if sall {
                cfg.scan_channels = cfg.channels;
                cfg.nscan = cfg.nchannels;
            } else {
                for k in 0..self.n(sref).count {
                    let (pc, over) = self.plan_ref(plan, self.at(sref, k))?;
                    let cc = self.fget(over, c"cc", pc.cc.into(), b"scan")?;
                    let n = cfg.nscan as usize;
                    if n >= CHANNELS {
                        return self.err(&[S(b"scan: too many channels")]);
                    }
                    cfg.nscan += 1;
                    self.channel_entry(freq_hz(pc.freq.into()), cc, 255.0, &mut cfg.scan_channels[n])?;
                }
            }
            if cfg.nscan == 0 {
                return self.err(&[S(b"scan: no channels")]);
            }
            let so = if is_obj { sc } else { -1 };
            cfg.dwell_s = self.fget(so, c"dwell_s", 0.45, b"scan")?;
            cfg.hang_s_scan = self.fget(so, c"hang_s", 3.0, b"scan")?;
            cfg.carrier_s = self.fget(so, c"carrier_s", 0.0, b"scan")?;
            if cfg.dwell_s <= 0.0 || cfg.hang_s_scan < 0.0 || !(0.0 <= cfg.carrier_s && cfg.carrier_s < cfg.dwell_s) {
                return self.err(&[S(b"scan: dwell_s > 0, hang_s >= 0, and carrier_s (0 = off) under dwell_s")]);
            }
        }
        if cfg.nchannels == 0 {
            return self.err(&[S(b"no channels: list the simplex channels this radio may use")]);
        }
        Some(())
    }

    /// peer_id, and the callsign (else name[:8]).
    fn peer_and_callsign(&self, r: c_int, cfg: &mut RdofConfig) -> R {
        match self.iget(r, c"peer_id", 0, b"radio") {
            Some(v) if self.get(r, c"peer_id") >= 0 => cfg.peer_id = v as u32,
            _ => return self.err(&[S(b"no peer_id")]),
        }
        let mut name8 = [0u8; 9];
        name8[..8].copy_from_slice(&cfg.name[..8]); // name[:8]
        self.sor(r, c"callsign", field(&name8), &mut cfg.callsign);
        Some(())
    }

    fn repeater(
        &self,
        bx: c_int,
        r: c_int,
        plan: &Plan,
        cfg: &mut RdofConfig,
        default_cc: c_long,
        has_default_cc: bool,
    ) -> R {
        cfg.mode = REPEATER;
        let mut mode = [0u8; 12];
        self.sget(r, c"mode", b"duplex", &mut mode);
        let mode = field(&mode);
        let sfr = mode == b"sfr";
        let dcdm = mode == b"dcdm";
        let (rx, tx);
        if sfr || dcdm {
            if self.get(r, c"freq") < 0 {
                return self.err(&[S(b"no freq (MHz)")]);
            }
            rx = self.box_hz(self.get(r, c"freq"), b"freq")?;
            tx = rx;
        } else {
            if self.get(r, c"rx_freq") < 0 {
                return self.err(&[S(b"no rx_freq (MHz)")]);
            }
            if self.get(r, c"tx_freq") < 0 {
                return self.err(&[S(b"no tx_freq (MHz)")]);
            }
            rx = self.box_hz(self.get(r, c"rx_freq"), b"rx_freq")?;
            tx = self.box_hz(self.get(r, c"tx_freq"), b"tx_freq")?;
        }
        let cc = if self.get(r, c"cc") >= 0 {
            self.iget(r, c"cc", 0, b"radio")?
        } else if has_default_cc {
            default_cc
        } else {
            return self.err(&[S(b"no color code (set cc on it or at the top)")]);
        };
        if mode != b"duplex" && mode != b"sfr" && !dcdm {
            return self.err(&[S(b"mode "), S(mode), S(b": duplex, sfr or dcdm")]);
        }
        if !(0..=15).contains(&cc) {
            return self.err(&[S(b"color code "), long(cc)]);
        }
        cfg.cc = cc as c_int;
        self.peer_and_callsign(r, cfg)?;
        cfg.rx_freq = freq_hz(rx.into());
        cfg.tx_freq = freq_hz(tx.into());
        cfg.power = self.iget(r, c"power", 255, b"radio")?.clamp(0, 255) as c_int;
        cfg.hang_s = self.fget(r, c"hang_s", 5.0, b"radio")?;
        cfg.call_hang_s = self.fget(r, c"call_hang_s", 3.0, b"radio")?;
        cfg.cwid_every_s = self.fget(r, c"cwid_every_s", 900.0, b"radio")?;
        self.jitter(r, cfg)?;
        self.sget(r, c"cwid", b"", &mut cfg.cwid);
        cfg.short_lc_ts1_low = self.bget(r, c"short_lc_ts1_low", false);
        self.radio_settings(r, REPEATER_KEYS, cfg)?;
        self.sget(self.get(r, c"radio"), c"type", b"auto", &mut cfg.radio_type);
        canonical(&mut cfg.radio_type);
        let rt = cfg.radio_type;
        let rt = field(&rt);
        if sfr {
            let inbound = self.iget(r, c"inbound_slot", 1, b"radio")?;
            if inbound != 1 && inbound != 2 {
                return self.err(&[S(b"inbound_slot: 1 or 2")]);
            }
            if rt != b"auto" && rt != b"radio_hotspot" {
                return self.err(&[
                    S(b"radio.type "),
                    S(rt),
                    S(b": a single frequency repeater is a radio_hotspot (a radio in hotspot mode with SFR firmware)"),
                ]);
            }
            cfg.sfr = true;
            cfg.inbound_slot = inbound as c_int;
            cfg.slot1 = inbound == 2;
            cfg.slot2 = inbound == 1;
        } else {
            cfg.sfr = false;
            cfg.dcdm = dcdm;
            cfg.inbound_slot = 1;
            let sl = self.get(r, c"slots");
            if sl < 0 {
                cfg.slot1 = true;
                cfg.slot2 = true;
            } else {
                cfg.slot1 = false;
                cfg.slot2 = false;
                let n = self.n(sl);
                let mut bad = n.type_ != RDOF_JARR || n.count == 0;
                let mut k = 0;
                while !bad && k < n.count {
                    match self.num(self.at(sl, k)).map(|v| v as c_long) {
                        Some(1) => cfg.slot1 = true,
                        Some(2) => cfg.slot2 = true,
                        _ => bad = true,
                    }
                    k += 1;
                }
                if bad {
                    return self.err(&[S(b"slots: 1, 2 or both")]);
                }
            }
            if dcdm && rt == b"split" {
                return self.err(&[S(b"radio.type split: a split is a duplex pair; dual capacity direct mode is one \
                                       radio on one frequency")]);
            }
            if rt == b"split" {
                self.split_halves(r)?;
            } else if !known_type(rt) {
                return self.err(&[
                    S(b"radio type "),
                    S(rt),
                    S(b": auto or one of radio_hotspot, mmdvm_hs, mmdvm, split"),
                ]);
            }
            if !dcdm && rt == b"radio_hotspot" {
                return self
                    .err(&[S(b"a radio_hotspot (a radio in hotspot mode) is simplex: a repeater needs a duplex \
                                       MMDVM_HS board, an MMDVM board or two radios as a split (but see mode: sfr)")]);
            }
        }
        if cfg.hang_s <= 0.0 || cfg.call_hang_s < 0.0 {
            return self.err(&[S(b"hang_s > 0 and call_hang_s >= 0")]);
        }
        // slot timing (docs/spec/slot-timing.md): the box's reaches a duplex repeater on a split or auto radio (the
        // SDR transceiver's is auto); a HAT or an SFR radio keeps its own slot clock, so utc is refused there
        let timed = !sfr && !dcdm && (rt == b"split" || rt == b"auto");
        let mut why = [0u8; 96];
        let base = if timed { self.get(bx, c"slot_timing") } else { -1 };
        let own = self.get(r, c"slot_timing");
        if !unsafe {
            rdof_slot_timing_parse(self.doc(), base, own, &mut cfg.slot_timing, why.as_mut_ptr().cast(), why.len())
        } {
            return self.err(&[S(field(&why))]);
        }
        if cfg.slot_timing.utc && sfr {
            return self.err(&[S(b"slot_timing utc: a single frequency repeater's radio times its own slots")]);
        }
        if cfg.slot_timing.utc && dcdm {
            return self.err(&[S(b"slot_timing utc: in dual capacity direct mode the modem times its slots from the \
                                   channel")]);
        }
        if cfg.slot_timing.utc && !timed {
            return self.err(&[
                S(b"slot_timing utc: a "),
                S(rt),
                S(b" keeps its own slot clock (its firmware times the slots, and MMDVM has no command to move it): \
                     only a split or the board's SDR can follow the UTC grid"),
            ]);
        }
        // The OLED's channel number: the plan's, by frequency.
        cfg.oled_channel = plan.ch[..plan.n]
            .iter()
            .find(|p| (-100..=100).contains(&(i64::from(p.freq) - i64::from(cfg.rx_freq))))
            .map_or(0, |p| p.number);
        Some(())
    }

    fn mqtt(&self, bx: c_int, cfg: &mut RdofConfig) -> R {
        let m = self.get(bx, c"mqtt");
        cfg.has_mqtt = self.truthy(m);
        if !cfg.has_mqtt {
            return Some(());
        }
        self.sget(m, c"host", b"localhost", &mut cfg.mqtt_host);
        cfg.mqtt_port = self.iget(m, c"port", 1883, b"mqtt")? as c_int;
        self.sor(m, c"username", b"", &mut cfg.mqtt_user);
        self.sor(m, c"password", b"", &mut cfg.mqtt_pass);
        self.sget(m, c"root", b"DMR", &mut cfg.mqtt_root);
        // the root's slashes are stripped, both ends
        let root = cfg.mqtt_root;
        let t = field(&root);
        let lead = t.iter().take_while(|&&c| c == b'/').count();
        let mut rl = t.len();
        while rl > lead && t[rl - 1] == b'/' {
            rl -= 1;
        }
        put(&mut cfg.mqtt_root, &t[lead..rl]);
        put(&mut cfg.mqtt_app, if cfg.mode == ROAMER { b"roaming" } else { b"repeater" });
        cfg.mqtt_tls = self.bget(m, c"tls", false);
        Some(())
    }

    fn oled(&self, bx: c_int, cfg: &mut RdofConfig) -> R {
        let o = self.get(bx, c"oled");
        let on = self.n(o);
        cfg.has_oled = o >= 0 && on.type_ != RDOF_JNULL && !(on.type_ == RDOF_JBOOL && !on.b);
        cfg.oled_address = 0x3C;
        cfg.oled_refresh_s = 0.5;
        cfg.oled_sda = 8;
        cfg.oled_scl = 9;
        if !cfg.has_oled || on.type_ != RDOF_JOBJ {
            return Some(());
        }
        let i2c = self.get(o, c"i2c");
        let src = if self.get(o, c"address") >= 0 { o } else { i2c };
        cfg.oled_address = self.iget(src, c"address", 0x3C, b"oled")? as c_int;
        let either =
            |key: &CStr| if self.get(o, key) >= 0 { self.bget(o, key, false) } else { self.bget(i2c, key, false) };
        cfg.oled_sh1106 = either(c"sh1106");
        cfg.oled_rotate180 = either(c"rotate180");
        cfg.oled_refresh_s = self.fget(o, c"refresh_s", 0.5, b"oled")?;
        cfg.oled_sda = self.iget(o, c"sda", 8, b"oled")? as c_int;
        cfg.oled_scl = self.iget(o, c"scl", 9, b"oled")? as c_int;
        Some(())
    }

    /// hblink.sctp_*: the firmware's SCTP (over UDP); defaults as MMDVMHost's and HBlink4's.
    fn sctp_settings(&self, hb: c_int, cfg: &mut RdofConfig) -> R {
        const KEYS: [(&CStr, c_long, c_long, c_long); 4] = [
            (c"sctp_encap_port", 9899, 1, 65535),
            (c"sctp_heartbeat_ms", 10000, 0, 600000),
            (c"sctp_max_retransmit", 3, 1, 20),
            (c"sctp_ttl_ms", 0, 0, 60000),
        ];
        for (k, &(key, dflt, lo, hi)) in KEYS.iter().enumerate() {
            let v = self.iget(hb, key, dflt, b"hblink")?;
            if v < lo || v > hi {
                return self.err(&[
                    S(b"hblink: "),
                    S(key.to_bytes()),
                    S(b" "),
                    long(v),
                    S(b" out of range ("),
                    long(lo),
                    S(b" to "),
                    long(hi),
                    S(b")"),
                ]);
            }
            *[
                &mut cfg.sctp_encap_port,
                &mut cfg.sctp_heartbeat_ms,
                &mut cfg.sctp_max_retransmit,
                &mut cfg.sctp_ttl_ms,
            ][k] = v as c_int;
        }
        Some(())
    }

    /// rdof_config_parse_radio's work, the settings parsed.
    unsafe fn parse(&self, bx: c_int, want: *const c_char, cfg: &mut RdofConfig) -> R {
        if bx < 0 {
            return None; // the JSON error, said
        }
        if self.n(bx).type_ != RDOF_JOBJ {
            return self.err(&[S(b"the settings are a JSON object")]);
        }
        {
            let mut t: RdofSlotTiming = core::mem::zeroed(); // the box's, checked whether or not anything inherits it
            let mut why = [0u8; 96];
            if !rdof_slot_timing_parse(
                self.doc(),
                -1,
                self.get(bx, c"slot_timing"),
                &mut t,
                why.as_mut_ptr().cast(),
                why.len(),
            ) {
                return self.err(&[S(b"box: "), S(field(&why))]);
            }
        }
        // the plan
        let mut plan = Plan { ch: [PlanCh { number: 0, freq: 0, cc: 0, power: 0 }; CHANNELS * 2], n: 0 };
        let cc_node = self.get(bx, c"cc");
        let has_default_cc = cc_node >= 0 && self.n(cc_node).type_ != RDOF_JNULL;
        let mut dcc = 0.0;
        if has_default_cc {
            dcc = self.num(cc_node).or_else(|| self.err(&[S(b"cc isn't a number")]))?;
        }
        let chans = self.get(bx, c"channels");
        let count = if self.n(chans).type_ == RDOF_JOBJ { self.n(chans).count } else { 0 };
        for k in 0..count {
            let ch = self.at(chans, k);
            let cn = self.n(ch);
            if plan.n >= plan.ch.len() {
                return self.err(&[S(b"too many channels in the plan")]);
            }
            let key = c_bytes(cn.key);
            let mut e: *mut c_char = ptr::null_mut();
            let number = strtol(cn.key, &mut e, 10);
            let pn = number as c_int;
            if key.is_empty() || *e != 0 || number != c_long::from(pn) {
                // "" isn't 0; nor 2^32 + 1 channel 1
                return self.err(&[S(b"channel "), S(key), S(b": not a number")]);
            }
            let f = if cn.type_ == RDOF_JOBJ { self.get(ch, c"freq") } else { ch };
            if f < 0 {
                return self.err(&[S(b"channel "), I(pn.into()), S(b": no freq")]);
            }
            let freq = self.box_hz(f, b"channel")?;
            let obj = if cn.type_ == RDOF_JOBJ { ch } else { -1 };
            let ccn = self.get(obj, c"cc");
            let cc = match (ccn >= 0, self.num(ccn)) {
                (true, Some(v)) => v,
                (false, _) if has_default_cc => dcc,
                _ => {
                    return self.err(&[
                        S(b"channel "),
                        I(pn.into()),
                        S(b": no color code (set cc on it or at the top)"),
                    ])
                }
            };
            let power = self.fget(obj, c"power", 255.0, b"channel")?;
            let p = PlanCh { number: pn, freq, cc: cc as c_int, power: power as c_int };
            match plan.ch[..plan.n].iter_mut().find(|q| q.number == pn) {
                Some(same) => *same = p, // "1" and "01": the last one
                None => {
                    plan.ch[plan.n] = p;
                    plan.n += 1;
                }
            }
        }
        let ch = &mut plan.ch[..plan.n]; // sorted by number
        for a in 1..ch.len() {
            let mut b = a;
            while b > 0 && ch[b - 1].number > ch[b].number {
                ch.swap(b, b - 1);
                b -= 1;
            }
        }
        // the radio: the one "run" names, else the first that isn't an SDR
        let mut box_name = [0u8; 64];
        self.sor(bx, c"name", b"radiofoundry", &mut box_name);
        let box_name = field(&box_name);
        let pk = self.pick_radios(bx, box_name)?;
        if pk.nradio == 0 {
            return self.err(&[S(b"no roamer or repeater in radios")]);
        }
        let mut k = 0;
        if !want.is_null() {
            let want = c_bytes(want);
            while k < pk.nradio {
                let mut name = [0u8; 96];
                self.radio_name(pk.radio[k], pk.radio_k[k], box_name, &mut name);
                if same_n(field(&name), want, cfg.name.len() - 1) {
                    break; // as far as rdof_roles_t and cfg->name keep it
                }
                k += 1;
            }
            if k == pk.nradio {
                return self.err(&[S(want), S(b" isn't a roamer or repeater this board runs")]);
            }
        }
        let (r, index) = (pk.radio[k], pk.radio_k[k]);
        let ty = s(self.n(self.get(r, c"type")));
        let mut dflt_name = [0u8; 96];
        print(&mut dflt_name, &[S(box_name), S(b"-"), S(ty), I((index + 1).into())]);
        self.sget(r, c"name", field(&dflt_name), &mut cfg.name);
        let hb = self.get(bx, c"hblink");
        self.sget(hb, c"host", b"localhost", &mut cfg.master_host);
        cfg.master_port = self.iget(hb, c"port", 62031, b"hblink")? as c_int;
        self.sget(hb, c"transport", b"udp", &mut cfg.transport);
        self.sctp_settings(hb, cfg)?;
        cfg.receiver_hblink = self.bget(r, c"hblink", true);
        let loc = self.get(r, c"location");
        self.location(if loc >= 0 { loc } else { self.get(bx, c"location") }, cfg)?;
        // sdr_transceiver: the board's SDR front end as the radio (docs/reference.md, "Transmit"): half duplex is a
        // roamer, full a duplex repeater; its modem is the SDR's emulated one.
        let sdrtx = ty == b"sdr_transceiver";
        let mut sdr_dcdm = false; // mode dcdm: a DCDM gateway (the repeater's code), half or full duplex
        cfg.sdr_duplex = DUPLEX_NONE;
        cfg.tx_power_dbm = 10.0;
        if sdrtx {
            let mut duplex = [0u8; 8];
            self.sget(r, c"duplex", b"half", &mut duplex);
            let duplex = field(&duplex);
            if duplex != b"half" && duplex != b"full" {
                return self.err(&[S(b"duplex "), S(duplex), S(b": half (a roamer) or full (a duplex repeater)")]);
            }
            cfg.sdr_duplex = if duplex == b"full" { DUPLEX_FULL } else { DUPLEX_HALF };
            cfg.tx_power_dbm = self.fget(r, c"tx_power_dbm", 10.0, b"radio")?;
            let mut mode = [0u8; 8];
            self.sget(r, c"mode", b"duplex", &mut mode);
            if cfg.sdr_duplex == DUPLEX_FULL && field(&mode) != b"duplex" && field(&mode) != b"dcdm" {
                return self.err(&[S(b"an sdr_transceiver with duplex: full is a duplex repeater (no mode: sfr)")]);
            }
            sdr_dcdm = field(&mode) == b"dcdm";
            let radios = self.get(bx, c"radios");
            if !(0..self.n(radios).count).any(|k| {
                let sx = self.at(radios, k);
                s(self.n(self.get(sx, c"type"))) == b"sdr" && self.enabled(sx) == 1
            }) {
                return self.err(&[S(b"an sdr_transceiver needs an SDR in radios: its front end is the radio")]);
            }
        }
        if ty == b"repeater" || (sdrtx && (cfg.sdr_duplex == DUPLEX_FULL || sdr_dcdm)) {
            self.repeater(bx, r, &plan, cfg, dcc as c_long, has_default_cc)?;
        } else {
            self.roamer(bx, r, &plan, cfg)?;
        }
        self.mqtt(bx, cfg)?;
        self.oled(bx, cfg)?;
        // firmware only (wifi and ntp belong to the board's Wi-Fi and clock, not here)
        let rt = field(&cfg.radio_type);
        let dflt_port: &[u8] = if rt == b"radio_hotspot" || rt == b"auto" { b"usb" } else { b"uart" };
        let mut box_port = [0u8; 8];
        self.sget(bx, c"modem", dflt_port, &mut box_port);
        self.sget(r, c"modem", field(&box_port), &mut cfg.modem_port); // the radio's own, else the box's
        let mp = field(&cfg.modem_port);
        if sdrtx {
            put(&mut cfg.modem_port, b"sdr");
        } else if mp != b"usb" && mp != b"uart" {
            return self.err(&[S(b"modem "), S(mp), S(b": usb or uart")]);
        }
        let u = if self.get(r, c"uart") >= 0 { self.get(r, c"uart") } else { self.get(bx, c"uart") };
        cfg.uart_tx = self.iget(u, c"tx", 5, b"uart")? as c_int;
        cfg.uart_rx = self.iget(u, c"rx", 6, b"uart")? as c_int;
        let v = self.iget(u, c"num", 1, b"uart")?;
        if v != 1 && v != 2 {
            return self.err(&[S(b"uart.num "), long(v), S(b": 1 or 2 (UART0 is the console)")]);
        }
        cfg.uart_num = v as c_int;
        cfg.uart_speed = self.iget(r, c"speed", 115200, b"radio")?;
        let usb = self.get(r, c"usb");
        self.sget(usb, c"serial", b"", &mut cfg.usb_serial);
        self.sget(usb, c"port", b"", &mut cfg.usb_port);
        if field(&cfg.radio_type) == b"split" {
            self.split_ports(r, cfg)?;
        }
        Some(())
    }
}

/// mmdvm.config.freq_hz: MHz below 10000
fn freq_hz(f: f64) -> u32 {
    unsafe { rint(if f < 10000.0 { f * 1e6 } else { f }) as u32 }
}

#[derive(Clone, Copy)]
struct PlanCh {
    number: c_int,
    freq: u32,
    cc: c_int,
    power: c_int,
}

struct Plan {
    ch: [PlanCh; CHANNELS * 2],
    n: usize,
}

impl Plan {
    fn find(&self, number: c_long) -> Option<PlanCh> {
        self.ch[..self.n].iter().find(|p| c_long::from(p.number) == number).copied()
    }
}

/// The radios pick_radios found: indexes into radios (-1 for none) and their places there.
struct Picked {
    radio: [c_int; MAX_RADIOS],
    radio_k: [c_int; MAX_RADIOS],
    nradio: usize,
    sdr: c_int,
    sdr_k: c_int,
}

#[no_mangle]
pub unsafe extern "C" fn rdof_config_parse(
    text: *const c_char,
    len: usize,
    cfg: &mut RdofConfig,
    error: *mut c_char,
    cap: usize,
) -> bool {
    rdof_config_parse_radio(text, len, ptr::null(), cfg, error, cap)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_config_parse_radio(
    text: *const c_char,
    len: usize,
    want: *const c_char,
    cfg: &mut RdofConfig,
    error: *mut c_char,
    cap: usize,
) -> bool {
    let mut dummy = [0u8; 8];
    let Some((c, root)) = Cx::new(text, len, error, cap, &mut dummy) else {
        return false;
    };
    ptr::write_bytes(cfg as *mut RdofConfig, 0, 1);
    c.parse(root, want, cfg).is_some()
}

#[no_mangle]
pub unsafe extern "C" fn rdof_config_ports(cfg: &RdofConfig, out: *mut RdofCfgPort) -> c_int {
    if field(&cfg.modem_port) == b"split" {
        *out = cfg.split[0];
        *out.add(1) = cfg.split[1];
        return 2;
    }
    let mut p = RdofCfgPort {
        modem_port: [0; 8],
        uart_tx: cfg.uart_tx,
        uart_rx: cfg.uart_rx,
        uart_num: cfg.uart_num,
        uart_speed: cfg.uart_speed,
        usb_serial: [0; 32],
        usb_port: [0; 16],
    };
    put(&mut p.modem_port, field(&cfg.modem_port));
    put(&mut p.usb_serial, field(&cfg.usb_serial));
    put(&mut p.usb_port, field(&cfg.usb_port));
    *out = p;
    1
}

/// The board's ports a radio uses: one, or a split's two.
fn ports(cfg: &RdofConfig) -> ([RdofCfgPort; 2], usize) {
    let mut p = [cfg.split[0]; 2];
    let n = unsafe { rdof_config_ports(cfg, p.as_mut_ptr()) };
    (p, n as usize)
}

#[no_mangle]
pub extern "C" fn rdof_config_uses_usb(cfg: &RdofConfig) -> bool {
    let (p, n) = ports(cfg);
    p[..n].iter().any(|p| field(&p.modem_port) == b"usb")
}

/// Two ports (of radios a and b) that can't both be: false with error set.
fn ports_apart(a_name: &[u8], a: &RdofCfgPort, b_name: &[u8], b: &RdofCfgPort, error: &mut [u8]) -> bool {
    let mp = field(&a.modem_port);
    if mp != field(&b.modem_port) {
        return true;
    }
    let both = |error: &mut [u8], why: &[Piece]| {
        let mut pieces = [S(b""); 6];
        pieces[..3].copy_from_slice(&[S(a_name), S(b" and "), S(b_name)]);
        pieces[3..3 + why.len()].copy_from_slice(why);
        print(error, &pieces[..3 + why.len()]);
        false
    };
    let (sa, sb, pa, pb) = (field(&a.usb_serial), field(&b.usb_serial), field(&a.usb_port), field(&b.usb_port));
    if mp == b"sdr" {
        return both(error, &[S(b" both use the SDR's modem: the board has one SDR")]);
    }
    if mp == b"uart" {
        if a.uart_num == b.uart_num {
            return both(
                error,
                &[S(b" are both on UART"), I(a.uart_num.into()), S(b": give one \"uart\": {\"num\": 2, ...}")],
            );
        }
        if a.uart_tx == b.uart_tx || a.uart_rx == b.uart_rx || a.uart_tx == b.uart_rx || a.uart_rx == b.uart_tx {
            return both(error, &[S(b" share a UART pin")]);
        }
    } else if (sa.is_empty() && pa.is_empty()) || (sb.is_empty() && pb.is_empty()) {
        return both(
            error,
            &[S(
                b" are both radios on USB: tell them apart with \"usb\": {\"serial\": ...} or {\"port\": ...} on each",
            )],
        );
    } else if (!sa.is_empty() && sa == sb && (pa.is_empty() || pb.is_empty() || pa == pb))
        || (sa.is_empty() && sb.is_empty() && pa == pb)
    {
        return both(error, &[S(b" would match the same USB device")]);
    }
    true
}

#[no_mangle]
pub unsafe extern "C" fn rdof_config_check_ports(
    cfgs: *const *const RdofConfig,
    n: c_int,
    error: *mut c_char,
    cap: usize,
) -> bool {
    let error = if error.is_null() { &mut [][..] } else { slice_mut(error.cast(), cap) }; // none wanted
    let cfgs = if n > 0 { core::slice::from_raw_parts(cfgs, n as usize) } else { &[] };
    for (i, &a) in cfgs.iter().enumerate() {
        for &b in &cfgs[i + 1..] {
            let (a, b) = (&*a, &*b);
            let ((pa, na), (pb, nb)) = (ports(a), ports(b));
            for x in &pa[..na] {
                for y in &pb[..nb] {
                    if !ports_apart(field(&a.name), x, field(&b.name), y, error) {
                        return false;
                    }
                }
            }
        }
    }
    true
}

#[no_mangle]
pub unsafe extern "C" fn rdof_box_roles(
    text: *const c_char,
    len: usize,
    roles: &mut RdofRoles,
    error: *mut c_char,
    cap: usize,
) -> bool {
    ptr::write_bytes(roles as *mut RdofRoles, 0, 1);
    let mut dummy = [0u8; 8];
    let Some((c, bx)) = Cx::new(text, len, error, cap, &mut dummy) else {
        return false;
    };
    if bx < 0 {
        return false; // the JSON error, said
    }
    if c.n(bx).type_ != RDOF_JOBJ {
        return c.err::<()>(&[S(b"the settings are a JSON object")]).is_some();
    }
    if c.get(bx, c"radios") < 0 && c.get(bx, c"device") >= 0 {
        roles.has_sdr = true; // a plain SDR config on its own
        roles.sdr_plain = true;
        c.sor(bx, c"name", b"sdr", &mut roles.sdr);
        put(&mut roles.box_, field(&roles.sdr));
        return true;
    }
    let mut box_name = [0u8; 64];
    c.sor(bx, c"name", b"radiofoundry", &mut box_name);
    let box_name = field(&box_name);
    put(&mut roles.box_, box_name);
    roles.usb_hub = c.bget(bx, c"usb_hub", false);
    let Some(pk) = c.pick_radios(bx, box_name) else {
        return false;
    };
    roles.nradios = pk.nradio as c_int;
    for k in 0..pk.nradio {
        c.radio_name(pk.radio[k], pk.radio_k[k], box_name, &mut roles.radios[k]);
    }
    roles.has_radio = pk.nradio > 0;
    if roles.has_radio {
        let first = roles.radios[0];
        put(&mut roles.radio, field(&first));
    }
    roles.has_sdr = pk.sdr >= 0;
    if roles.has_sdr {
        c.radio_name(pk.sdr, pk.sdr_k, box_name, &mut roles.sdr);
    }
    true
}

// ── describe ────────────────────────────────────────────────────────────────

/// rdof_format_float3 (%.3f, trailing zeros dropped to one)
fn f3(v: f64, buf: &mut [u8; 40]) -> &[u8] {
    unsafe { rdof_format_float3(v, buf.as_mut_ptr().cast(), buf.len()) };
    field(buf)
}

/// jitter=auto:min-max (ms) or fixed (at prebuffer)
fn add_jitter(o: &mut Out, c: &RdofConfig) {
    if c.jitter_auto {
        o.bytes(b"jitter=auto:");
        int(o, c.jitter_min_ms);
        o.byte(b'-');
        int(o, c.jitter_max_ms);
        o.byte(b';');
    } else {
        o.bytes(b"jitter=fixed;");
    }
}

fn int(o: &mut Out, v: impl Into<i64>) {
    o.int(v.into(), 0);
}

#[no_mangle]
pub unsafe extern "C" fn rdof_config_describe(c: &RdofConfig, out: *mut c_char, cap: usize) -> usize {
    let n = c_out(out, cap, |o| describe(c, o));
    n.min(cap.saturating_sub(1)) // each part cut as it went, as the C's
}

fn describe(c: &RdofConfig, o: &mut Out) {
    let mut a = [0u8; 40];
    o.bytes(b"mode=");
    o.bytes(if c.mode == ROAMER { b"roamer" } else { b"repeater" });
    o.bytes(b";name=");
    o.bytes(field(&c.name));
    o.bytes(b";peer=");
    int(o, c.peer_id);
    o.bytes(b";callsign=");
    o.bytes(field(&c.callsign));
    o.bytes(b";master=");
    o.bytes(field(&c.master_host));
    o.byte(b':');
    int(o, c.master_port);
    o.bytes(b";transport=");
    o.bytes(field(&c.transport));
    o.bytes(b";hblink=");
    int(o, c.receiver_hblink);
    o.bytes(b";type=");
    o.bytes(field(&c.radio_type));
    o.byte(b';');
    if field(&c.transport) == b"sctp" {
        // the SCTP settings
        o.bytes(b"sctp=encap:");
        int(o, c.sctp_encap_port);
        o.bytes(b",heartbeat:");
        int(o, c.sctp_heartbeat_ms);
        o.bytes(b",retransmit:");
        int(o, c.sctp_max_retransmit);
        o.bytes(b",ttl:");
        int(o, c.sctp_ttl_ms);
        o.byte(b';');
    }
    let m = &c.modem;
    o.bytes(b"modem=");
    for (k, v) in [m.rx_invert, m.tx_invert, m.ptt_invert].into_iter().enumerate() {
        if k > 0 {
            o.byte(b',');
        }
        int(o, v);
    }
    o.byte(b',');
    int(o, m.tx_delay_ms);
    for v in [m.rx_level, m.tx_level, m.cwid_level] {
        o.byte(b',');
        o.bytes(f3(v, &mut a));
    }
    for v in [m.rx_dc_offset, m.tx_dc_offset, m.dmr_delay] {
        o.byte(b',');
        int(o, v);
    }
    o.bytes(b";offsets=");
    int(o, c.rx_offset_hz);
    o.byte(b',');
    int(o, c.tx_offset_hz);
    o.bytes(b";tuner=");
    o.bytes(match c.tuner {
        TUNER_MODEM => b"modem",
        TUNER_FIXED => b"fixed",
        _ => b"default",
    });
    o.byte(b':');
    int(o, c.tuner_freq);
    o.byte(b':');
    int(o, c.settle_ms);
    o.bytes(b";rssi=");
    for i in 0..(c.rssi.n.max(0) as usize).min(RSSI_POINTS) {
        if i > 0 {
            o.byte(b',');
        }
        int(o, c.rssi.raw[i]);
        o.byte(b':');
        int(o, c.rssi.dbm[i]);
    }
    o.bytes(b";headers=");
    int(o, c.headers);
    o.bytes(b";terminators=");
    int(o, c.terminators);
    o.byte(b';');
    // a count past its array: as far as it goes
    let upto = |n: c_int| (n.max(0) as usize).min(CHANNELS);
    if c.mode == ROAMER {
        o.bytes(b"channels=");
        for (i, ch) in c.channels[..upto(c.nchannels)].iter().enumerate() {
            if i > 0 {
                o.byte(b',');
            }
            int(o, ch.freq);
            o.byte(b':');
            int(o, ch.cc);
            o.byte(b':');
            int(o, ch.power);
        }
        o.bytes(b";idle=");
        int(o, c.idle.freq);
        o.byte(b':');
        int(o, c.idle.cc);
        o.byte(b':');
        int(o, c.idle.power);
        o.bytes(b";listen=");
        o.bytes(f3(c.listen_s, &mut a));
        o.bytes(b";clear=");
        o.bytes(f3(c.clear_s, &mut a));
        o.bytes(b";prebuffer=");
        int(o, c.prebuffer);
        o.byte(b';');
        add_jitter(o, c);
        o.bytes(b"preambles=");
        int(o, c.csbk_preambles);
        o.bytes(b";answer=");
        o.bytes(f3(c.answer_s, &mut a));
        o.byte(b',');
        o.bytes(f3(c.answer_max_s, &mut a));
        o.bytes(b";idle_after=");
        o.bytes(f3(c.idle_after_s, &mut a));
        o.bytes(b";covered=");
        for (i, &f) in c.covered[..upto(c.ncovered)].iter().enumerate() {
            if i > 0 {
                o.byte(b',');
            }
            int(o, f);
        }
        o.bytes(b";scan=");
        if c.has_scan {
            for (i, ch) in c.scan_channels[..upto(c.nscan)].iter().enumerate() {
                if i > 0 {
                    o.byte(b',');
                }
                int(o, ch.freq);
                o.byte(b':');
                int(o, ch.cc);
            }
            o.byte(b'|');
            o.bytes(f3(c.dwell_s, &mut a));
            o.byte(b',');
            o.bytes(f3(c.hang_s_scan, &mut a));
            o.byte(b',');
            o.bytes(f3(c.carrier_s, &mut a));
        } else {
            o.bytes(b"none");
        }
        o.byte(b';');
    } else {
        let mut tm = [0u8; 64];
        unsafe { rdof_slot_timing_describe(&c.slot_timing, tm.as_mut_ptr().cast(), tm.len()) };
        o.bytes(b"rx=");
        int(o, c.rx_freq);
        o.bytes(b";tx=");
        int(o, c.tx_freq);
        o.bytes(b";cc=");
        int(o, c.cc);
        o.bytes(b";power=");
        int(o, c.power);
        o.bytes(b";slots=");
        int(o, c.slot1);
        int(o, c.slot2);
        o.bytes(b";hang=");
        o.bytes(f3(c.hang_s, &mut a));
        o.byte(b',');
        o.bytes(f3(c.call_hang_s, &mut a));
        o.bytes(b";prebuffer=");
        int(o, c.prebuffer);
        o.byte(b';');
        add_jitter(o, c);
        o.bytes(b"sfr=");
        int(o, c.sfr);
        o.bytes(b";inbound=");
        int(o, c.inbound_slot);
        o.bytes(b";ts1low=");
        int(o, c.short_lc_ts1_low);
        o.bytes(b";timing=");
        o.bytes(field(&tm));
        o.byte(b';');
    }
    o.bytes(b"cwid=");
    o.bytes(field(&c.cwid));
    o.byte(b',');
    o.bytes(f3(c.cwid_every_s, &mut a));
    o.byte(b';');
    if c.has_location {
        o.bytes(b"location=");
        o.fixed(c.location.latitude, 6);
        o.byte(b',');
        o.fixed(c.location.longitude, 6);
        o.byte(b',');
        int(o, c.location.height_m);
        o.byte(b';');
    } else {
        o.bytes(b"location=none;");
    }
    if c.has_mqtt {
        o.bytes(b"mqtt=");
        o.bytes(field(&c.mqtt_host));
        o.byte(b':');
        int(o, c.mqtt_port);
        for f in [&c.mqtt_user[..], &c.mqtt_pass, &c.mqtt_root, &c.mqtt_app] {
            o.byte(b':');
            o.bytes(field(f));
        }
        o.byte(b':');
        int(o, c.mqtt_tls);
        o.byte(b';');
    } else {
        o.bytes(b"mqtt=none;");
    }
    if c.has_oled {
        o.bytes(b"oled=");
        int(o, c.oled_address);
        o.byte(b',');
        int(o, c.oled_sh1106);
        o.byte(b',');
        int(o, c.oled_rotate180);
        o.byte(b',');
        o.bytes(f3(c.oled_refresh_s, &mut a));
        o.byte(b',');
        int(o, c.oled_channel);
    } else {
        o.bytes(b"oled=none");
    }
}
