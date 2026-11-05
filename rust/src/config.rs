// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! The settings (version 3) -> one repeater, SFR or DCDM gateway, or transceiver (rdof/config.h,
//! docs/spec/fw/config-v3.md section 12). A port of common/src/config.c: it reads the plan (rdof/siteplan.h, C) as
//! the C does.
//!
//! The settings text is untrusted. Numbers in strings go through the C library's strtod and strtol, %g and floats
//! through its snprintf, so they read and print as the C's did.

use core::ffi::{c_char, c_int, c_long, CStr};
use core::ptr;

use crate::bits::slice_mut;
use crate::ffi::{rint, snprintf, strtod, strtol};
use crate::hblink::RdofLocation;
use crate::jitter::{rdof_jb_bursts, RDOF_JB_BURST_MS, RDOF_JB_MAX_DEFAULT_MS, RDOF_JB_MAX_MS, RDOF_JB_MIN_MS};
use crate::json::rdof_format_float3;
use crate::jsonp::{rdof_jat, rdof_jget, rdof_jnode, JDoc, JNode, RDOF_JARR, RDOF_JBOOL, RDOF_JNULL, RDOF_JNUM, RDOF_JOBJ, RDOF_JSTR};
use crate::mmdvm::{rdof_modem_config_default, RdofModemConfig};
use crate::modem::{rdof_rssi_map_sort, RdofRssiMap};
use crate::plan::{
    field, first_id, rdof_plan_close, rdof_txpower_level, rdof_plan_open, Plan, RdofPlan, RdofPort, RdofTxmap, RdofUnit, APP_PARROT, DEV_SDR, HEARS_SCAN,
    NET_HBLINK, NET_MQTT, PORT_PATH, PORT_UART, UNIT_DCDM, UNIT_RECEIVER, UNIT_REPEATER, UNIT_SFR, UNIT_TRANSCEIVER,
};
use crate::slotgrid::{rdof_slot_timing_describe, rdof_slot_timing_parse, RdofSlotTiming};
use crate::status::rdof_format_repr;
use crate::text::{c_out, fixed, print, Out, Piece, Piece::I, Piece::S};

const CHANNELS: usize = 32; // RDOF_CFG_CHANNELS
const ROLES_MAX: usize = 8; // RDOF_ROLES_MAX
const RSSI_POINTS: usize = 16; // RDOF_RSSI_POINTS
const SITE_MAX_FREQS: usize = 48; // RDOF_SITE_MAX_FREQS

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
    pub slot: c_int, // 1 or 2: a direct mode call's here (the plan's slot; nothing on the air says)
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
    pub has_parrot: bool,
    pub parrot: u32,

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
    pub tx_map: RdofTxmap,
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
    pub radios: [[u8; 24]; ROLES_MAX],
    pub sdr: [u8; 32],
}

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
        unsafe { crate::text::c_bytes(n.str_) }
    } else {
        b""
    }
}

fn is_env(n: &JNode) -> bool {
    let t = s(n);
    t.len() >= 3 && t[0] == b'$' && t[1] == b'{' && t[t.len() - 1] == b'}'
}

fn known_type(t: &[u8]) -> bool {
    matches!(t, b"auto" | b"radio_hotspot" | b"mmdvm_hs" | b"mmdvm")
}

/// The C's lower-casing: A to Z only.
fn lower(t: &mut [u8]) {
    t.iter_mut().for_each(|c| c.make_ascii_lowercase());
}

/// strncmp(a, b, n) == 0
fn same_n(a: &[u8], b: &[u8], n: usize) -> bool {
    a[..a.len().min(n)] == b[..b.len().min(n)]
}

/// mmdvm.config.freq_hz: MHz below 10000
fn freq_hz(f: f64) -> u32 {
    unsafe { rint(if f < 10000.0 { f * 1e6 } else { f }) as u32 }
}

// ── the plan and where errors go ────────────────────────────────────────────

type R<T = ()> = Option<T>;

struct Cx<'a> {
    plan: Plan<'a>,
    out: *mut u8,
    cap: usize,
    mm: [c_int; 2], // the modem's MMDVM settings: a split's TX half's, then its RX half's
    mmw: [u8; 64],  // where they are: devices.NAME.mmdvm
    map: RdofTxmap, // the transmitter's RF levels in dBm
}

impl Cx<'_> {
    fn doc(&self) -> &JDoc {
        self.plan.doc
    }

    /// snprintf(error, cap, ...); None, to return.
    fn err<T>(&self, pieces: &[Piece]) -> R<T> {
        print(unsafe { slice_mut(self.out, self.cap) }, pieces);
        None
    }

    fn mmw(&self) -> &[u8] {
        field(&self.mmw)
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

    /// The jitter buffer: jitter_buffer "auto" (the default) or a fixed size in ms, rounded up to whole 60 ms bursts;
    /// prebuffer, the older key, a fixed size in bursts. jitter_min_ms and jitter_max_ms bound auto.
    fn jitter(&self, r: c_int, what: &[u8], cfg: &mut RdofConfig) -> R<()> {
        let (jb, pb) = (self.get(r, c"jitter_buffer"), self.get(r, c"prebuffer"));
        if jb >= 0 && pb >= 0 {
            return self.err(&[S(what), S(b": prebuffer and jitter_buffer: set one (prebuffer is a fixed size in bursts)")]);
        }
        cfg.prebuffer = self.iget(r, c"prebuffer", 6, what)? as c_int;
        cfg.jitter_auto = pb < 0;
        if jb >= 0 {
            let n = self.n(jb);
            if n.type_ == RDOF_JSTR && s(n) == b"auto" {
                cfg.jitter_auto = true;
            } else if let Some(ms) = self.num(jb).filter(|&ms| n.type_ != RDOF_JBOOL && (0.0..=RDOF_JB_MAX_MS).contains(&ms)) {
                cfg.jitter_auto = false;
                cfg.prebuffer = rdof_jb_bursts(ms);
            } else {
                return self.err(&[S(what), S(b": jitter_buffer: \"auto\" or milliseconds, 0 to "), I(RDOF_JB_MAX_MS as i64)]);
            }
        }
        let lo = self.fget(r, c"jitter_min_ms", RDOF_JB_MIN_MS, what)?;
        let hi = self.fget(r, c"jitter_max_ms", RDOF_JB_MAX_DEFAULT_MS, what)?;
        if !(lo >= f64::from(RDOF_JB_BURST_MS) && lo <= hi && hi <= RDOF_JB_MAX_MS) {
            return self.err(&[
                S(what),
                S(b": jitter_min_ms and jitter_max_ms: "),
                I(RDOF_JB_BURST_MS.into()),
                S(b" <= min <= max <= "),
                I(RDOF_JB_MAX_MS as i64),
            ]);
        }
        cfg.jitter_min_ms = rdof_jb_bursts(lo) * RDOF_JB_BURST_MS;
        cfg.jitter_max_ms = rdof_jb_bursts(hi) * RDOF_JB_BURST_MS;
        Some(())
    }

    /// A radio setting: the TX half's, else the RX half's (one modem: both are its own).
    fn rget(&self, key: &CStr) -> c_int {
        let i = self.get(self.mm[0], key);
        if i >= 0 {
            i
        } else {
            self.get(self.mm[1], key)
        }
    }

    fn radio_settings(&self, cfg: &mut RdofConfig) -> R {
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
        let mmw = self.mmw();
        for (k, &(key, t)) in F.iter().enumerate() {
            let i = self.rget(key);
            if i < 0 {
                continue;
            }
            let (b, v) = if t == 0 {
                (self.truthy(i), 0.0)
            } else {
                match self.num(i) {
                    Some(v) => (false, v),
                    None => return self.err(&[S(mmw), S(b"."), S(key.to_bytes()), S(b" isn't a number")]),
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
        if let Some(v) = self.num(self.rget(c"rx_offset_hz")) {
            cfg.rx_offset_hz = v as i32;
        }
        if let Some(v) = self.num(self.rget(c"tx_offset_hz")) {
            cfg.tx_offset_hz = v as i32;
        }
        cfg.headers = -1;
        cfg.terminators = -1;
        let i = self.rget(c"headers");
        if let Some(v) = self.num(i).filter(|_| self.n(i).type_ != RDOF_JNULL) {
            cfg.headers = v as c_int;
        }
        let i = self.rget(c"terminators");
        if let Some(v) = self.num(i).filter(|_| self.n(i).type_ != RDOF_JNULL) {
            cfg.terminators = v as c_int;
        }
        // tuner: "fixed" / {type, freq, settle_ms}; rigctld and command tuners need a computer
        cfg.tuner = TUNER_NONE; // the kind's default
        cfg.tuner_freq = 0;
        cfg.settle_ms = 0;
        let i = self.rget(c"tuner");
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
                        None => return self.err(&[S(mmw), S(b".tuner.freq")]),
                    }
                }
                let mut tw = [0u8; 96];
                print(&mut tw, &[S(mmw), S(b".tuner")]);
                cfg.settle_ms = self.fget(i, c"settle_ms", 0.0, field(&tw))? as c_int;
            }
            match field(&t) {
                b"modem" => cfg.tuner = TUNER_MODEM,
                b"fixed" => cfg.tuner = TUNER_FIXED,
                t @ (b"rigctld" | b"command") => {
                    return self.err(&[S(mmw), S(b".tuner "), S(t), S(b": not on this board (modem or fixed only)")])
                }
                b"" => {}
                t => return self.err(&[S(mmw), S(b".tuner "), S(t)]),
            }
        }
        cfg.rssi.n = 0;
        let i = self.rget(c"rssi");
        if self.truthy(i) {
            let wants = [S(mmw), S(b".rssi wants [[raw, dBm], ...]")];
            if self.n(i).type_ != RDOF_JARR {
                return self.err(&wants);
            }
            for k in 0..self.n(i).count.min(RSSI_POINTS as c_int) {
                let p = self.at(i, k);
                let np = self.n(p);
                let pair = np.type_ == RDOF_JARR && np.count == 2;
                let (Some(a), Some(b)) =
                    (if pair { (self.num(self.at(p, 0)), self.num(self.at(p, 1))) } else { (None, None) })
                else {
                    return self.err(&wants);
                };
                cfg.rssi.raw[k as usize] = a as c_int;
                cfg.rssi.dbm[k as usize] = b as c_int;
                cfg.rssi.n = k + 1;
            }
            rdof_rssi_map_sort(&mut cfg.rssi);
        }
        // check_radio
        let mut proto = [0u8; 12];
        let i = self.rget(c"protocol");
        if i >= 0 {
            self.text_of(i, &mut proto);
        } else {
            put(&mut proto, b"uart");
        }
        lower(&mut proto);
        let p = field(&proto);
        if p != b"uart" && p != b"udp" {
            return self.err(&[S(mmw), S(b".protocol "), S(p), S(b": uart or udp")]);
        }
        if p == b"udp" {
            if !self.truthy(self.rget(c"modem_address")) {
                return self.err(&[S(mmw), S(b".protocol udp needs "), S(mmw), S(b".modem_address")]);
            }
            return self.err(&[S(mmw), S(b".protocol udp: not on this board")]);
        }
        Some(())
    }

    fn channel_entry(&self, freq: u32, cc: f64, power: f64, slot: c_int, out: &mut RdofCfgChannel) -> R {
        if !(0.0..=15.0).contains(&cc) || cc as c_long as f64 != cc {
            let mut a = [0u8; 32];
            return self.err(&[S(b"color code "), S(g(cc, &mut a))]);
        }
        *out = RdofCfgChannel {
            freq,
            cc: cc as c_int,
            power: (power as c_long).clamp(0, 255) as c_int,
            slot: if slot == 2 { 2 } else { 1 },
        };
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

    /// The hblink network's sctp_*: the firmware's SCTP (over UDP); defaults as MMDVMHost's and HBlink4's. what:
    /// where it is (networks.NAME).
    fn sctp_settings(&self, hb: c_int, what: &[u8], cfg: &mut RdofConfig) -> R {
        const KEYS: [(&CStr, c_long, c_long, c_long); 4] = [
            (c"sctp_encap_port", 9899, 1, 65535),
            (c"sctp_heartbeat_ms", 10000, 0, 600000),
            (c"sctp_max_retransmit", 3, 1, 20),
            (c"sctp_ttl_ms", 0, 0, 60000),
        ];
        for (k, &(key, dflt, lo, hi)) in KEYS.iter().enumerate() {
            let v = self.iget(hb, key, dflt, what)?;
            if v < lo || v > hi {
                return self.err(&[
                    S(what),
                    S(b": "),
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

    // ── version 3: a unit of the plan (rdof/siteplan.h) ──

    /// A frequency's color code: its own, else the defaults' (-1: any).
    fn freq_cc(&self, f: c_int) -> c_int {
        let cc = self.plan.site.freq[f as usize].cc;
        if cc >= 0 {
            return cc;
        }
        let d = self.get(self.get(self.plan.root(), c"defaults"), c"cc");
        match self.num(d) {
            Some(v) if d >= 0 && self.n(d).type_ != RDOF_JNULL => v as c_int,
            _ => -1,
        }
    }

    /// The HBlink peer a unit is (its path's), and its callsign: the path's peer name, else the top-level callsign,
    /// else its name's first 8 characters.
    fn unit_peer(&self, path: c_int, where_: &[u8], cfg: &mut RdofConfig) -> R {
        let s = self.plan.site;
        let hb = self.plan.net_of(NET_HBLINK);
        let pa = (path >= 0).then(|| &s.path[path as usize]);
        cfg.receiver_hblink = pa.is_some_and(|pa| hb >= 0 && pa.pol.net == hb);
        cfg.peer_id = pa.map_or(0, |pa| pa.peer_id);
        if cfg.peer_id == 0 && hb >= 0 {
            // a peer ID only for an hblink network
            return self.err(&[S(where_), S(b": no HBlink peer ID (the top-level id)")]);
        }
        match pa.filter(|pa| pa.peer_name[0] != 0) {
            Some(pa) => put(&mut cfg.callsign, field(&pa.peer_name)),
            None => {
                let mut name8 = [0u8; 9];
                name8[..8].copy_from_slice(&cfg.name[..8]); // name[:8]
                self.sor(self.plan.root(), c"callsign", field(&name8), &mut cfg.callsign);
            }
        }
        Some(())
    }

    /// The parrot app's ID when one of the unit's paths may reach it (reach: the apps they reach).
    fn unit_parrot(&self, reach: u32, cfg: &mut RdofConfig) -> R {
        let s = self.plan.site;
        let a = self.plan.app_of(APP_PARROT);
        cfg.has_parrot = a >= 0 && (reach >> a & 1) != 0;
        cfg.parrot = 0;
        if !cfg.has_parrot {
            return Some(());
        }
        let ap = &s.app[a as usize];
        let id = first_id(&ap.tg);
        cfg.parrot = if id != 0 { id } else { first_id(&ap.pc) };
        if cfg.parrot > 0xFF_FFFF {
            return self.err(&[S(b"apps."), S(field(&ap.name)), S(b": "), I(cfg.parrot.into()), S(b" isn't a DMR id")]);
        }
        Some(())
    }

    /// A split repeater's halves ([0] TX, [1] RX): each one's port, and app.split's SET_ROLE parameters.
    fn unit_split(&self, u: &RdofUnit, where_: &[u8], cfg: &mut RdofConfig) -> R {
        let s = self.plan.site;
        let (mut usb, mut uart) = (0, 0);
        for k in 0..2 {
            let d = if k == 1 { u.rx } else { u.tx };
            let pt = if s.dev[d as usize].kind == DEV_SDR {
                let mut pt = unit_port(None);
                put(&mut pt.modem_port, b"sdr");
                pt
            } else {
                unit_port(Some(&self.plan.dev(d).port))
            };
            let path = s.dev[d as usize].kind != DEV_SDR && self.plan.dev(d).port.kind == PORT_PATH;
            match field(&pt.modem_port) {
                b"usb" => {
                    // a path (the host's) isn't the board's one USB port
                    usb += i32::from(!path);
                    put(&mut cfg.usb_serial, field(&pt.usb_serial));
                    put(&mut cfg.usb_port, field(&pt.usb_port));
                }
                b"uart" => {
                    uart += 1;
                    if uart == 1 {
                        cfg.uart_tx = pt.uart_tx;
                        cfg.uart_rx = pt.uart_rx;
                        cfg.uart_num = pt.uart_num;
                        cfg.uart_speed = pt.uart_speed;
                    }
                }
                _ => {}
            }
            cfg.split[k] = pt;
        }
        let (tx, rx) = (field(&s.dev[u.tx as usize].name), field(&s.dev[u.rx as usize].name));
        if usb > 1 {
            return self.err(&[
                S(where_),
                S(b": devices."),
                S(tx),
                S(b" and devices."),
                S(rx),
                S(b" are both on USB: one half on USB at most (the other on a UART, or the SDR as the RX half)"),
            ]);
        }
        if uart > 1 {
            let (a, b) = (&cfg.split[0], &cfg.split[1]);
            if a.uart_num == b.uart_num {
                return self.err(&[
                    S(where_),
                    S(b": devices."),
                    S(tx),
                    S(b" and devices."),
                    S(rx),
                    S(b" are both on UART"),
                    I(a.uart_num.into()),
                ]);
            }
            if a.uart_tx == b.uart_tx || a.uart_rx == b.uart_rx || a.uart_tx == b.uart_rx || a.uart_rx == b.uart_tx {
                return self.err(&[S(where_), S(b": devices."), S(tx), S(b" and devices."), S(rx), S(b" share a UART pin")]);
            }
        }
        let sp = self.get(self.plan.app_node(u.app), c"split");
        for (k, key) in [c"power_cap", c"prebuffer", c"lease_s", c"cooldown_s"].into_iter().enumerate() {
            let i = self.get(sp, key);
            if i < 0 {
                continue;
            }
            let v = match self.num(i) {
                Some(v) if (0.0..=255.0).contains(&v) => v as c_int, // NaN refused too
                _ => return self.err(&[S(where_), S(b".split."), S(key.to_bytes()), S(b": 0 to 255")]),
            };
            *[&mut cfg.split_power_cap, &mut cfg.split_prebuffer, &mut cfg.split_lease_s, &mut cfg.split_cooldown_s]
                [k] = v;
        }
        let i = self.get(sp, c"max_key_s");
        if i >= 0 {
            let v = self.num(i);
            if let Some(v) = v {
                cfg.split_max_key_s = v;
            }
            if !v.is_some_and(|v| (0.0..=600.0).contains(&v)) {
                return self.err(&[S(where_), S(b".split.max_key_s: 0 to 600")]);
            }
        }
        put(&mut cfg.modem_port, b"split");
        Some(())
    }

    /// A plan channel: a frequency of the site as a roamer's channel (power 255 when it has none).
    fn unit_channel(&self, f: c_int, with_power: bool, where_: &[u8], out: &mut RdofCfgChannel) -> R {
        let fr = &self.plan.site.freq[f as usize];
        let cc = self.freq_cc(f);
        if !(0..=15).contains(&cc) {
            return self.err(&[S(where_), S(b": frequencies."), S(field(&fr.name)), S(b" has no color code")]);
        }
        let power = if with_power { unsafe { rdof_txpower_level(&self.map, fr.power_dbm) } } else { 255 };
        self.channel_entry(fr.hz, cc.into(), power.into(), fr.ts.into(), out)
    }

    /// The frequencies the board's SDRs hear (its receivers' and the units' on them), sorted: a roamer leaves those
    /// to the SDR.
    fn unit_covered(&self, cfg: &mut RdofConfig) {
        let (s, p) = (self.plan.site, &self.plan);
        cfg.ncovered = 0;
        for d in 0..s.ndev {
            if s.dev[d as usize].kind != DEV_SDR || !p.sdr_used(d) {
                continue;
            }
            let mut f = [0u32; SITE_MAX_FREQS + 64];
            let mut n = 0;
            let r = p.receiver(d);
            for x in 0..s.nfreq {
                if r >= 0 && p.unit(r).freqs >> x & 1 != 0 {
                    f[n] = s.freq[x as usize].hz;
                    n += 1;
                }
            }
            for x in 0..p.nextra() {
                if p.extra(x).dev == d {
                    f[n] = p.extra(x).hz;
                    n += 1;
                }
            }
            for &hz in &f[..n] {
                let nc = cfg.ncovered as usize;
                if !cfg.covered[..nc].contains(&hz) && nc < CHANNELS {
                    cfg.covered[nc] = hz;
                    cfg.ncovered += 1;
                }
            }
        }
        cfg.covered[..cfg.ncovered as usize].sort_unstable(); // sorted
    }

    /// A transceiver: the frequencies its device transmits on; the talker keys on the device.
    fn unit_roamer(&self, u: &RdofUnit, where_: &[u8], cfg: &mut RdofConfig) -> R {
        let s = self.plan.site;
        let dv = &s.dev[u.rx as usize];
        let r = self.plan.dev_node(u.rx);
        cfg.mode = ROAMER;
        cfg.nchannels = 0;
        let mut reach = 0u32;
        for f in 0..s.nfreq {
            if u.freqs >> f & 1 == 0 {
                continue;
            }
            if cfg.nchannels as usize >= CHANNELS {
                return self.err(&[S(where_), S(b".transmits: "), I(CHANNELS as i64), S(b" frequencies at most")]);
            }
            let k = cfg.nchannels as usize;
            cfg.nchannels += 1;
            self.unit_channel(f, true, where_, &mut cfg.channels[k])?;
            let fr = &s.freq[f as usize];
            let pa = self.plan.path_rx(fr.hz, fr.ts.into());
            if pa >= 0 {
                reach |= s.path[pa as usize].pol.apps;
            }
        }
        if cfg.nchannels == 0 {
            return self.err(&[S(where_), S(b": no frequencies to transmit on")]);
        }
        self.unit_channel(u.freq, false, where_, &mut cfg.idle)?;
        cfg.oled_channel = u.freq + 1;
        let idle = &s.freq[u.freq as usize];
        self.unit_peer(self.plan.path_rx(idle.hz, idle.ts.into()), where_, cfg)?;
        cfg.listen_s = self.fget(r, c"listen_s", 0.3, where_)?;
        cfg.clear_s = self.fget(r, c"clear_s", 2.0, where_)?;
        cfg.answer_s = self.fget(r, c"answer_s", 0.75, where_)?;
        cfg.answer_max_s = self.fget(r, c"answer_max_s", 5.0, where_)?;
        cfg.idle_after_s = self.fget(r, c"idle_after_s", 0.0, where_)?;
        cfg.cwid_every_s = self.fget(r, c"cwid_every_s", 900.0, where_)?;
        self.jitter(r, where_, cfg)?;
        cfg.csbk_preambles = self.iget(r, c"csbk_preambles", 2, where_)? as c_int;
        self.sget(r, c"cwid", b"", &mut cfg.cwid);
        self.unit_parrot(reach, cfg)?;
        self.unit_covered(cfg);
        cfg.has_scan = dv.hears == HEARS_SCAN;
        if cfg.has_scan {
            cfg.nscan = 0;
            for f in 0..s.nfreq {
                if dv.scan >> f & 1 == 0 {
                    continue;
                }
                if cfg.nscan as usize >= CHANNELS {
                    return self.err(&[S(where_), S(b".hears.scan: "), I(CHANNELS as i64), S(b" frequencies at most")]);
                }
                let k = cfg.nscan as usize;
                cfg.nscan += 1;
                self.unit_channel(f, false, where_, &mut cfg.scan_channels[k])?;
            }
            let mut sw = [0u8; 80];
            print(&mut sw, &[S(where_), S(b".hears")]);
            let sw = field(&sw);
            let h = self.get(r, c"hears");
            if cfg.nscan == 0 {
                return self.err(&[S(sw), S(b".scan: no frequencies")]);
            }
            cfg.dwell_s = self.fget(h, c"dwell_s", 0.45, sw)?;
            cfg.hang_s_scan = self.fget(h, c"hang_s", 3.0, sw)?;
            cfg.carrier_s = self.fget(h, c"carrier_s", 0.0, sw)?;
            if cfg.dwell_s <= 0.0 || cfg.hang_s_scan < 0.0 || !(0.0 <= cfg.carrier_s && cfg.carrier_s < cfg.dwell_s) {
                return self.err(&[S(sw), S(b": dwell_s > 0, hang_s >= 0, and carrier_s (0 = off) under dwell_s")]);
            }
        }
        Some(())
    }

    /// A repeater app, or an SFR or DCDM frequency: the repeater's code. r: the app's or the frequency's object.
    fn unit_repeater(&self, u: &RdofUnit, where_: &[u8], cfg: &mut RdofConfig) -> R {
        let s = self.plan.site;
        let (sfr, dcdm) = (u.kind == UNIT_SFR, u.kind == UNIT_DCDM);
        let (r, inf, outf, p0, p1);
        if u.kind == UNIT_REPEATER {
            let ap = &s.app[u.app as usize];
            r = self.plan.app_node(u.app);
            (inf, outf, p0, p1) = (ap.in_, ap.out, ap.path[0], ap.path[1]);
        } else {
            r = self.plan.freq_node(u.freq);
            (inf, outf) = (u.freq, u.freq);
            let hz = s.freq[inf as usize].hz;
            p0 = self.plan.path_rx(hz, 1);
            p1 = self.plan.path_rx(hz, 2);
        }
        let fo = &s.freq[outf as usize];
        cfg.mode = REPEATER;
        cfg.rx_freq = s.freq[inf as usize].hz;
        cfg.tx_freq = fo.hz;
        if !(0..=15).contains(&fo.cc) {
            return self.err(&[S(where_), S(b": frequencies."), S(field(&fo.name)), S(b" has no color code")]);
        }
        cfg.cc = fo.cc;
        self.unit_peer(p0, where_, cfg)?;
        if let Some(f) = s.freq[..s.nfreq as usize].iter().find(|f| !f.tx && f.hz == cfg.tx_freq) {
            return self.err(&[
                S(where_),
                S(b": frequencies."),
                S(field(&f.name)),
                S(b" has tx false: nothing transmits on it"),
            ]);
        }
        cfg.power = unsafe { rdof_txpower_level(&self.map, fo.power_dbm) };
        cfg.hang_s = self.fget(r, c"hang_s", 5.0, where_)?;
        cfg.call_hang_s = self.fget(r, c"call_hang_s", 3.0, where_)?;
        cfg.cwid_every_s = self.fget(r, c"cwid_every_s", 900.0, where_)?;
        self.jitter(r, where_, cfg)?;
        self.sget(r, c"cwid", b"", &mut cfg.cwid);
        let apps = |p: c_int| if p >= 0 { s.path[p as usize].pol.apps } else { 0 };
        self.unit_parrot(apps(p0) | apps(p1), cfg)?;
        cfg.short_lc_ts1_low = self.bget(r, c"short_lc_ts1_low", false);
        cfg.sfr = sfr;
        cfg.dcdm = dcdm;
        cfg.inbound_slot = 1;
        let rt = cfg.radio_type;
        let rt = field(&rt);
        if sfr {
            let inbound = self.iget(r, c"inbound_slot", 1, where_)?;
            if inbound != 1 && inbound != 2 {
                return self.err(&[S(where_), S(b".inbound_slot: 1 or 2")]);
            }
            cfg.inbound_slot = inbound as c_int;
            cfg.slot1 = inbound == 2;
            cfg.slot2 = inbound == 1;
        } else {
            let a = self.get(r, c"active_slots");
            cfg.slot1 = a < 0;
            cfg.slot2 = a < 0;
            // checked in the parse: [1], [2] or [1, 2]
            for k in 0..if a >= 0 { self.n(a).count } else { 0 } {
                if let Some(v) = self.num(self.at(a, k)) {
                    if v as c_long == 1 {
                        cfg.slot1 = true;
                    } else {
                        cfg.slot2 = true;
                    }
                }
            }
            if !dcdm && rt == b"radio_hotspot" {
                return self.err(&[
                    S(where_),
                    S(b": devices."),
                    S(field(&s.dev[u.rx as usize].name)),
                    S(b" is a radio_hotspot (a radio in hotspot mode), which is simplex: a repeater needs a duplex \
                         MMDVM_HS board, an MMDVM board or two radios as a split (or a frequency with mode sfr)"),
                ]);
            }
        }
        if cfg.hang_s <= 0.0 || cfg.call_hang_s < 0.0 {
            return self.err(&[S(where_), S(b": hang_s > 0 and call_hang_s >= 0")]);
        }
        // slot timing (docs/spec/slot-timing.md): the board's reaches a duplex repeater on a split or the SDR; a
        // modem or an SFR radio keeps its own slot clock, so utc is refused there
        let timed = !sfr && !dcdm && (rt == b"split" || rt == b"auto");
        let mut why = [0u8; 96];
        let base = if timed { self.get(self.plan.root(), c"slot_timing") } else { -1 };
        let own = self.get(r, c"slot_timing");
        if !unsafe {
            rdof_slot_timing_parse(self.doc(), base, own, &mut cfg.slot_timing, why.as_mut_ptr().cast(), why.len())
        } {
            return self.err(&[S(where_), S(b"."), S(field(&why))]);
        }
        if cfg.slot_timing.utc && sfr {
            return self.err(&[S(where_), S(b".slot_timing: utc: a single frequency repeater's radio times its own slots")]);
        }
        if cfg.slot_timing.utc && dcdm {
            return self.err(&[
                S(where_),
                S(b".slot_timing: utc: in dual capacity direct mode the modem times its slots from the channel"),
            ]);
        }
        if cfg.slot_timing.utc && !timed {
            return self.err(&[
                S(where_),
                S(b".slot_timing: utc: a "),
                S(rt),
                S(b" keeps its own slot clock (its firmware times the slots, and MMDVM has no command to move it): \
                     only a split or the board's SDR can follow the UTC grid"),
            ]);
        }
        // the OLED's channel number: its place in frequencies, from 1
        cfg.oled_channel = s.freq[..s.nfreq as usize]
            .iter()
            .position(|f| (-100..=100).contains(&(i64::from(f.hz) - i64::from(cfg.rx_freq))))
            .map_or(0, |f| f as c_int + 1);
        Some(())
    }

    fn unit_mqtt(&self, cfg: &mut RdofConfig) -> R {
        let k = self.plan.net_of(NET_MQTT);
        let n = (k >= 0).then(|| &self.plan.site.net[k as usize]);
        cfg.has_mqtt = n.is_some_and(|n| n.report || n.calls);
        let Some(n) = n.filter(|_| cfg.has_mqtt) else {
            return Some(());
        };
        let m = self.plan.net_node(k);
        let mut what = [0u8; 56];
        print(&mut what, &[S(b"networks."), S(field(&n.name))]);
        let what = field(&what);
        self.sget(m, c"host", b"localhost", &mut cfg.mqtt_host);
        cfg.mqtt_port = self.iget(m, c"port", 1883, what)? as c_int;
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

    /// rdof_plan_radio's work: the unit called want (NULL: the first that transmits).
    unsafe fn radio(&mut self, want: *const c_char, cfg: &mut RdofConfig) -> R {
        let (s, p) = (self.plan.site, self.plan);
        let want = if want.is_null() { None } else { Some(CStr::from_ptr(want).to_bytes()) };
        let Some(k) = (0..p.nunits()).find(|&i| {
            let u = p.unit(i);
            u.kind != UNIT_RECEIVER && want.is_none_or(|w| same_n(field(&u.name), w, cfg.name.len() - 1))
        }) else {
            return match want {
                Some(w) => self.err(&[
                    S(w),
                    S(b" isn't a unit that transmits (a repeater, an SFR or DCDM frequency, or a device transmitting \
                         on simplex frequencies)"),
                ]),
                None => self.err(&[S(b"nothing transmits: no repeater, SFR or DCDM frequency, or device transmitting")]),
            };
        };
        let u = p.unit(k);
        let root = p.root();
        let mut where_ = [0u8; 56];
        let wn = match u.kind {
            UNIT_REPEATER => [S(b"apps."), S(field(&s.app[u.app as usize].name))],
            UNIT_SFR | UNIT_DCDM => [S(b"frequencies."), S(field(&s.freq[u.freq as usize].name))],
            _ => [S(b"devices."), S(field(&s.dev[u.rx as usize].name))],
        };
        print(&mut where_, &wn);
        let where_ = field(&where_);
        put(&mut cfg.name, field(&u.name));
        {
            // the board's, checked whether or not anything inherits it
            let mut t = core::mem::zeroed::<RdofSlotTiming>();
            let mut why = [0u8; 96];
            if !rdof_slot_timing_parse(self.doc(), -1, self.get(root, c"slot_timing"), &mut t, why.as_mut_ptr().cast(), why.len()) {
                return self.err(&[S(field(&why))]);
            }
        }
        // the network
        let hbk = p.net_of(NET_HBLINK);
        let hb = p.net_node(hbk);
        let mut hbw = [0u8; 56];
        print(&mut hbw, &[S(b"networks."), S(if hbk >= 0 { field(&s.net[hbk as usize].name) } else { b"hblink" })]);
        let hbw = field(&hbw);
        self.sget(hb, c"host", b"localhost", &mut cfg.master_host);
        cfg.master_port = self.iget(hb, c"port", 62031, hbw)? as c_int;
        self.sget(hb, c"transport", b"udp", &mut cfg.transport);
        self.sctp_settings(hb, hbw, cfg)?;
        self.location(self.get(root, c"location"), cfg)?;
        // the radio: the board's SDR (an sdr_transceiver), one modem, or a split's two
        let split = u.rx != u.tx;
        let on_sdr = !split && s.dev[u.rx as usize].kind == DEV_SDR;
        self.mm = [self.get(p.dev_node(u.tx), c"mmdvm"), self.get(p.dev_node(u.rx), c"mmdvm")];
        let md = if self.mm[0] >= 0 { u.tx } else { u.rx };
        print(&mut self.mmw, &[S(b"devices."), S(field(&s.dev[md as usize].name)), S(b".mmdvm")]);
        self.radio_settings(cfg)?;
        let drv = field(&s.dev[u.rx as usize].driver);
        put(&mut cfg.radio_type, if on_sdr { b"auto" } else if split { b"split" } else { drv });
        if !split && !known_type(field(&cfg.radio_type)) {
            return self.err(&[
                S(b"devices."),
                S(field(&s.dev[u.rx as usize].name)),
                S(b".type "),
                S(field(&cfg.radio_type)),
            ]);
        }
        cfg.sdr_duplex = DUPLEX_NONE;
        cfg.tx_power_dbm = 10.0;
        if on_sdr {
            let dv = p.dev(u.rx);
            cfg.sdr_duplex = if u.kind == UNIT_REPEATER || u.duplex_full { DUPLEX_FULL } else { DUPLEX_HALF };
            if dv.has_tx_power {
                cfg.tx_power_dbm = dv.tx_power_dbm;
            }
        }
        self.map = p.dev(if u.tx >= 0 { u.tx } else { u.rx }).txmap;
        cfg.tx_map = self.map;
        if u.kind == UNIT_TRANSCEIVER {
            self.unit_roamer(u, where_, cfg)?;
        } else {
            self.unit_repeater(u, where_, cfg)?;
        }
        self.unit_mqtt(cfg)?;
        self.oled(root, cfg)?;
        // the port
        let mut pt = unit_port(None);
        if on_sdr {
            put(&mut pt.modem_port, b"sdr");
        } else if split {
            put(&mut pt.modem_port, b"uart");
        } else {
            pt = unit_port(Some(&p.dev(u.rx).port));
        }
        use_port(cfg, &pt);
        if split {
            self.unit_split(u, where_, cfg)?;
        }
        Some(())
    }
}

/// A modem's port as the radio's: a UART, or USB (a path is the host's: USB too). None: no port (115200 baud).
fn unit_port(pt: Option<&RdofPort>) -> RdofCfgPort {
    let mut out = RdofCfgPort {
        modem_port: [0; 8],
        uart_tx: 5,
        uart_rx: 6,
        uart_num: 1,
        uart_speed: pt.map_or(115200, |pt| pt.baud),
        usb_serial: [0; 32],
        usb_port: [0; 16],
    };
    match pt {
        Some(pt) if pt.kind == PORT_UART => {
            put(&mut out.modem_port, b"uart");
            (out.uart_num, out.uart_tx, out.uart_rx) = (pt.uart, pt.tx, pt.rx);
        }
        _ => {
            put(&mut out.modem_port, b"usb");
            if let Some(pt) = pt {
                put(&mut out.usb_serial, field(&pt.serial));
                put(&mut out.usb_port, field(&pt.usb_port));
            }
        }
    }
    out
}

fn use_port(cfg: &mut RdofConfig, pt: &RdofCfgPort) {
    put(&mut cfg.modem_port, field(&pt.modem_port));
    (cfg.uart_tx, cfg.uart_rx, cfg.uart_num) = (pt.uart_tx, pt.uart_rx, pt.uart_num);
    cfg.uart_speed = pt.uart_speed;
    put(&mut cfg.usb_serial, field(&pt.usb_serial));
    put(&mut cfg.usb_port, field(&pt.usb_port));
}

#[no_mangle]
pub unsafe extern "C" fn rdof_plan_radio(
    p: *const RdofPlan,
    name: *const c_char,
    cfg: &mut RdofConfig,
    error: *mut c_char,
    cap: usize,
) -> bool {
    let mut dummy = [0u8; 8];
    let (out, cap) = if error.is_null() { (dummy.as_mut_ptr(), dummy.len()) } else { (error.cast(), cap) };
    if let Some(b) = slice_mut(out, cap).first_mut() {
        *b = 0;
    }
    ptr::write_bytes(cfg as *mut RdofConfig, 0, 1);
    let mut c = Cx { plan: Plan::new(p), out, cap, mm: [-1, -1], mmw: [0; 64], map: RdofTxmap { min_dbm: 0.0, max_dbm: 0.0 } };
    c.radio(name, cfg).is_some()
}

// The text forms: a plan each.

#[no_mangle]
pub unsafe extern "C" fn rdof_config_parse_radio(
    text: *const c_char,
    len: usize,
    name: *const c_char,
    cfg: &mut RdofConfig,
    error: *mut c_char,
    cap: usize,
) -> bool {
    let p = rdof_plan_open(text, len, error, cap);
    if p.is_null() {
        ptr::write_bytes(cfg as *mut RdofConfig, 0, 1);
        return false;
    }
    let ok = rdof_plan_radio(p, name, cfg, error, cap);
    rdof_plan_close(p);
    ok
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

extern "C" {
    // siteplan.c: what a board runs
    fn rdof_plan_roles(p: *const RdofPlan, roles: &mut RdofRoles, err: *mut c_char, cap: usize) -> bool;
}

#[no_mangle]
pub unsafe extern "C" fn rdof_box_roles(
    text: *const c_char,
    len: usize,
    roles: &mut RdofRoles,
    error: *mut c_char,
    cap: usize,
) -> bool {
    let p = rdof_plan_open(text, len, error, cap);
    if p.is_null() {
        ptr::write_bytes(roles as *mut RdofRoles, 0, 1);
        return false;
    }
    let ok = rdof_plan_roles(p, roles, error, cap);
    rdof_plan_close(p);
    ok
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

/// The board's ports a unit uses: one, or a split's two.
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

/// Two ports (of units a and b) that can't both be: false with error set.
fn ports_apart(a_name: &[u8], a: &RdofCfgPort, b_name: &[u8], b: &RdofCfgPort, error: &mut [u8]) -> bool {
    let mp = field(&a.modem_port);
    if mp != field(&b.modem_port) {
        return true;
    }
    let both = |error: &mut [u8], why: &[Piece]| {
        let mut pieces = [S(b""); 8];
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
                &[
                    S(b" are both on UART"),
                    I(a.uart_num.into()),
                    S(b": give one's device \"port\": {\"uart\": "),
                    I((3 - a.uart_num).into()),
                    S(b", ...}"),
                ],
            );
        }
        if a.uart_tx == b.uart_tx || a.uart_rx == b.uart_rx || a.uart_tx == b.uart_rx || a.uart_rx == b.uart_tx {
            return both(error, &[S(b" share a UART pin")]);
        }
    } else if (sa.is_empty() && pa.is_empty()) || (sb.is_empty() && pb.is_empty()) {
        return both(
            error,
            &[S(b" are both on USB: tell their devices apart with \"serial\" or \"usb_port\" in each one's port")],
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
    if c.has_parrot {
        o.bytes(b"parrot=");
        int(o, c.parrot);
        o.byte(b';');
    }
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
