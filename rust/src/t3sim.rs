// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! A trunked site's air, simulated (rdof/t3sim.h, docs/spec/fw/tier3.md section 10): simulated MSs (t3ms.rs) against
//! a site (rdof/t3unit.h) through the full burst path, what rdofradio --sim-air runs. The Tier III unit and the radio
//! layer's burst helpers (rdof/t3rf.h) through their C API.

use core::ffi::{c_int, c_void};

use crate::ffi::{
    rdof_duplex_silence, RDOF_DT_TERMINATOR_WITH_LC, RDOF_DT_VOICE, RDOF_DT_VOICE_LC_HEADER, RDOF_DT_VOICE_SYNC,
};
use crate::layer2::{rdof_group_lc, rdof_lc_burst, RDOF_DATA_XOR, RDOF_MS_VOICE};
use crate::t3ms::{Ms, MsTx};
use crate::t3rf::{BURST_BYTES, PDU_BAD, PDU_OK};
use crate::t3site::{CARRIERS, VOICE_FRAME, VOICE_HEADER, VOICE_TERM};
use crate::tracker::RdofBurst;

pub const MS_MAX: usize = 64; // RDOF_T3SIM_MS
pub const EXT: usize = 32; // RDOF_T3SIM_EXT

const FAMILY_MS: u32 = 1; // RDOF_FAMILY_MS

/// rdof_t3sim_fate_t
pub const HEARD: u32 = 0;
pub const COLLIDED: u32 = 1;
pub const DROPPED: u32 = 2;

use crate::t3rf::{rdof_t3rf_block_burst, rdof_t3rf_burst_block, rdof_t3rf_cach4};
/// rdof_t3unit_t
pub use crate::t3unit::Unit as T3unit;
use crate::t3unit::{rdof_t3unit_at, rdof_t3unit_cfg, rdof_t3unit_run, rdof_t3unit_rx, rdof_t3unit_slot};

/// rdof_t3sim_in_t
#[repr(C)]
pub struct SimIn {
    pub ch: c_int,
    pub b: RdofBurst,
    pub fate: u32,
    pub tag: *mut c_void,
}

/// rdof_t3sim_out_t
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SimOut {
    pub aired: bool,
    pub burst: [u8; BURST_BYTES],
    pub dt: c_int,
    pub cc: c_int,
    pub pdu_ok: bool,
    pub pdu: [u8; 10],
    pub cach: [u8; 3],
    pub pdu_ext: [u8; 2],
    pub at: c_int,
}

/// rdof_t3sim_t
#[repr(C)]
pub struct Sim {
    pub u: *mut T3unit,
    pub n: c_int,
    pub ms: [*mut Ms; MS_MAX],
    pub id: [u32; MS_MAX],
    pub voice_n: [c_int; MS_MAX],
    pub outside: u32,
    pub outbound: Option<unsafe extern "C" fn(ctx: *mut c_void, c: c_int, k: i64, burst: *mut u8) -> bool>,
    pub outbound_ctx: *mut c_void,
    pub ext_in: Option<unsafe extern "C" fn(ctx: *mut c_void, k: i64, inp: *mut SimIn, max: c_int) -> c_int>,
    pub ext_out: Option<
        unsafe extern "C" fn(
            ctx: *mut c_void,
            k: i64,
            out: *const SimOut,
            ncar: c_int,
            at_next: c_int,
            inp: *const SimIn,
            nin: c_int,
        ),
    >,
    pub ext_ctx: *mut c_void,
    pub slc: [i32; CARRIERS],
    pub out_pdus: u32,
    pub out_bad: u32,
    pub in_pdus: u32,
    pub in_voice: u32,
    pub collisions: u32,
    pub ext_in_n: u32,
    pub ext_drop: u32,
}

impl Sim {
    fn mss(&self) -> usize {
        (self.n.max(0) as usize).min(MS_MAX)
    }
}

/// An MS's burst for tx (rdof_t3sim_ms_burst).
fn ms_burst(tx: &MsTx, n: c_int, cc: c_int, b: &mut RdofBurst) -> bool {
    unsafe { core::ptr::write_bytes(b as *mut RdofBurst, 0, 1) };
    b.family = FAMILY_MS;
    b.cc = cc;
    b.slot = c_int::from(tx.ch & 1) + 1;
    if tx.kind == 1 {
        let mut blk = [0u8; 12];
        blk[..10].copy_from_slice(&tx.pdu);
        blk[10..].copy_from_slice(&tx.ext); // a rate 1/2 block's last two octets
        if !rdof_t3rf_block_burst(tx.dt.into(), &blk, cc, FAMILY_MS, &mut b.burst) {
            return false;
        }
        b.dt = tx.dt.into();
        return true;
    }
    match tx.what {
        VOICE_HEADER | VOICE_TERM => {
            let dt = if tx.what == VOICE_HEADER { RDOF_DT_VOICE_LC_HEADER } else { RDOF_DT_TERMINATOR_WITH_LC };
            let mut lc9 = [0u8; 9];
            rdof_group_lc(tx.src, tx.dst, tx.group != 0, 0, &mut lc9);
            rdof_lc_burst(&lc9, dt, cc, RDOF_MS_VOICE ^ RDOF_DATA_XOR, &mut b.burst);
            b.dt = dt;
            true
        }
        VOICE_FRAME => {
            let n = n.rem_euclid(6);
            unsafe { rdof_duplex_silence(n, cc, b.burst.as_mut_ptr()) };
            b.dt = if n != 0 { RDOF_DT_VOICE } else { RDOF_DT_VOICE_SYNC };
            b.n = n;
            true
        }
        _ => false,
    }
}

/// What MS i sends, as its burst, its voice counter kept.
fn sim_ms_burst(s: &mut Sim, i: usize, tx: &MsTx, cc: c_int, b: &mut RdofBurst) -> bool {
    let n = s.voice_n[i];
    if tx.kind != 1 && tx.what == VOICE_FRAME {
        s.voice_n[i] = (n + 1) % 6;
    } else if tx.kind != 1 {
        s.voice_n[i] = 0;
    }
    ms_burst(tx, n, cc, b)
}

fn is_voice(dt: c_int) -> bool {
    dt == RDOF_DT_VOICE || dt == RDOF_DT_VOICE_SYNC
}

fn slot(s: &mut Sim, k: i64, now_ms: i64) {
    let u = s.u;
    let cfg = unsafe { &*rdof_t3unit_cfg(u) };
    let (ncar, cc) = (usize::from(cfg.ncarriers).min(CARRIERS), c_int::from(cfg.cc));
    unsafe { rdof_t3unit_run(u, k, now_ms) };
    let parity = (k & 1) as usize;

    // the outbound of slot k, read back as an MS would
    let mut dt = [0u8; CARRIERS];
    let mut pdu = [[0u8; 12]; CARRIERS];
    let mut out = [SimOut {
        aired: false,
        burst: [0; BURST_BYTES],
        dt: -1,
        cc: -1,
        pdu_ok: false,
        pdu: [0; 10],
        cach: [0; 3],
        pdu_ext: [0; 2],
        at: 0,
    }; CARRIERS];
    for (c, o) in out.iter_mut().enumerate().take(ncar) {
        let mut control = 0u8;
        let (mut at, mut rdt, mut rcc): (c_int, c_int, c_int) = (-1, -1, -1);
        let mut slc: i32 = -1;
        o.aired = if s.outside >> c & 1 != 0 {
            match s.outbound {
                Some(f) => unsafe { f(s.outbound_ctx, c as c_int, k, o.burst.as_mut_ptr()) },
                None => false,
            }
        } else {
            unsafe { rdof_t3unit_slot(u, c as c_int, k, o.burst.as_mut_ptr(), &mut control, &mut at, &mut slc) }
        };
        // the CACH before slot k: the short LC changes on a 4-slot cycle's first
        if k % 4 == 0 && slc >= 0 {
            s.slc[c] = slc;
        }
        let mut at4: [c_int; 4] = [0; 4];
        let mut c4 = [[0u8; 3]; 4];
        o.at = c_int::from(at > 0);
        at4[(k & 3) as usize] = o.at;
        let slc28 = if s.slc[c] >= 0 { s.slc[c] as u32 } else { 0 };
        rdof_t3rf_cach4(slc28, &at4, k & !3, &mut c4);
        o.cach = c4[(k & 3) as usize];
        if !o.aired {
            continue;
        }
        let mut blk = [0u8; 12];
        let r = rdof_t3rf_burst_block(&o.burst, &mut rdt, &mut rcc, &mut blk);
        o.pdu.copy_from_slice(&blk[..10]);
        o.pdu_ext = [blk[10], blk[11]];
        o.dt = rdt;
        o.cc = rcc;
        if r == PDU_OK {
            s.out_pdus += 1;
            o.pdu_ok = true;
            dt[c] = rdt as u8;
            pdu[c] = blk;
        } else {
            s.out_bad += u32::from(r == PDU_BAD);
        }
    }

    // the inbound of slot k: what each MS sends, the site hears one burst a channel
    let nms = s.mss();
    let mut tx = [MsTx::default(); MS_MAX];
    let mut has = [false; MS_MAX];
    for i in 0..nms {
        if let Some(t) = unsafe { (*s.ms[i]).tx(k, now_ms) } {
            tx[i] = t;
            has[i] = true;
        }
    }
    let mut ext: [SimIn; EXT] = unsafe { core::mem::zeroed() };
    let next = match s.ext_in {
        Some(f) => unsafe { f(s.ext_ctx, k, ext.as_mut_ptr(), EXT as c_int) }.clamp(0, EXT as c_int) as usize,
        None => 0,
    };
    for e in &mut ext[..next] {
        s.ext_in_n += 1;
        e.fate = HEARD;
        if e.ch < 0 || (e.ch & 1) as usize != parity || (e.ch / 2) as usize >= ncar {
            e.ch = -1;
            e.fate = DROPPED;
            s.ext_drop += 1;
        }
    }
    for ch in (parity..2 * ncar).step_by(2) {
        let chi = ch as c_int;
        let on = |i: usize| has[i] && usize::from(tx[i].ch) == ch;
        let first = (0..nms).find(|&i| on(i));
        let count = (0..nms).filter(|&i| on(i)).count() + ext[..next].iter().filter(|e| e.ch == chi).count();
        if count == 0 {
            continue;
        }
        let mut b: RdofBurst = unsafe { core::mem::zeroed() };
        let mut ok = false;
        if let Some(first) = first {
            ok = sim_ms_burst(s, first, &tx[first], cc, &mut b);
            for (i, t) in tx.iter().enumerate().take(nms).skip(first + 1) {
                // the others' voice counters move on as if heard
                if on(i) {
                    let mut other: RdofBurst = unsafe { core::mem::zeroed() };
                    sim_ms_burst(s, i, t, cc, &mut other);
                }
            }
        }
        if !ok {
            let Some(e) = ext[..next].iter().find(|e| e.ch == chi) else {
                continue;
            };
            b = unsafe { core::ptr::read(&e.b) }; // plain data
        }
        b.slot = (chi & 1) + 1;
        if count > 1 {
            // a collision: the strongest is unreadable too (the site counts it)
            s.collisions += 1;
            for e in ext[..next].iter_mut().filter(|e| e.ch == chi) {
                e.fate = COLLIDED;
            }
            for x in &mut b.burst[14..20] {
                *x ^= 0x5A;
            }
        }
        if is_voice(b.dt) || b.dt == RDOF_DT_VOICE_LC_HEADER || b.dt == RDOF_DT_TERMINATOR_WITH_LC {
            s.in_voice += 1;
        } else {
            s.in_pdus += 1;
        }
        unsafe { rdof_t3unit_rx(u, chi / 2, k, &b, -60) };
    }

    // the MSs hear slot k on the channel each is on
    let at = unsafe { rdof_t3unit_at(u, k) };
    for i in 0..nms {
        let ms = unsafe { &mut *s.ms[i] };
        let ch = usize::from(ms.status().ch);
        if ch & 1 != parity || ch / 2 >= ncar {
            continue;
        }
        ms.rx_block(ch, k, dt[ch / 2], &pdu[ch / 2], ch == 0 && at > 0);
    }
    if let Some(f) = s.ext_out {
        unsafe { f(s.ext_ctx, k, out.as_ptr(), ncar as c_int, at, ext.as_ptr(), next as c_int) };
    }
}

// ── C API (rdof/t3sim.h) ──────────────────────────────────────────────────────

#[no_mangle]
pub unsafe extern "C" fn rdof_t3sim_init(s: *mut Sim, u: *mut T3unit, n: c_int, ms: *const *mut Ms, id: *const u32) {
    core::ptr::write_bytes(s, 0, 1);
    let s = &mut *s;
    s.u = u;
    s.n = n.min(MS_MAX as c_int);
    s.slc = [-1; CARRIERS];
    for i in 0..s.mss() {
        s.ms[i] = *ms.add(i);
        s.id[i] = *id.add(i);
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3sim_slot(s: *mut Sim, k: i64, now_ms: i64) {
    slot(&mut *s, k, now_ms);
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3sim_ms_burst(tx: *const MsTx, n: c_int, cc: c_int, b: *mut RdofBurst) -> bool {
    ms_burst(&*tx, n, cc, &mut *b)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3sim_find(s: *const Sim, id: u32) -> c_int {
    let s = &*s;
    s.id[..s.mss()].iter().position(|&x| x == id).map_or(-1, |i| i as c_int)
}

// ── layouts (rdof_t3sim_layout) ───────────────────────────────────────────────

pub use crate::t3site::Layout;

macro_rules! layout {
    ($name:literal, $t:ty) => {
        Layout {
            name: concat!($name, "\0").as_ptr().cast(),
            size: core::mem::size_of::<$t>(),
            align: core::mem::align_of::<$t>(),
        }
    };
}

static LAYOUTS: [Layout; 4] = [
    layout!("rdof_t3sim_in_t", SimIn),
    layout!("rdof_t3sim_out_t", SimOut),
    layout!("rdof_t3sim_t", Sim),
    Layout { name: core::ptr::null(), size: 0, align: 0 },
];

#[no_mangle]
pub extern "C" fn rdof_t3sim_layout(i: usize) -> *const Layout {
    LAYOUTS.get(i).map_or(core::ptr::null(), |l| l as *const Layout)
}
