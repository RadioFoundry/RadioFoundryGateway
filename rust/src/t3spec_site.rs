// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! Spec conformance, the site controller (ETSI TS 102 361-4 clauses 5, 6 and annex A): the TSCC's outbound
//! signalling, random access answers, registration, calls and the configuration ranges. The site is driven slot
//! by slot through its public API; the MS side is hand-built CSBKs, and every block the site sends is read with
//! the field reader, not with t3pdu.

use std::boxed::Box;
use std::vec::Vec;

use crate::t3site::*;
use crate::t3spec_bits::{csbko, field, lb, spec, Bits};

const T0: i64 = 1_793_912_651_000; // Thursday 2026-11-05 21:04:11 UTC
const A: u32 = 1001;
const B: u32 = 2002;
const C: u32 = 3003;
const TG: u32 = 9;
const TG2: u32 = 10;

// ── MS PDUs, built from the 361-4 tables ──

/// C_RAND (table 7.25).
fn rand(so: u8, dep: u8, sk: u8, target: u32, source: u32) -> [u8; 10] {
    Bits::csbk(true, spec::C_RAND)
        .f(7, so as u64)
        .f(1, 0)
        .f(4, dep as u64)
        .f(4, sk as u64)
        .f(24, target as u64)
        .f(24, source as u64)
        .done()
}

/// Registration to REG_ADDR (table 6.9: Reg_Dereg = 1, TRGT_ADR_CNTS 00b, target 0x00 + C_SYScode).
fn reg(sc: u16, id: u32) -> [u8; 10] {
    rand(0b000_0001, 0b0000, spec::SK_REG, sc as u32, id)
}

/// Registration with one talkgroup subscription (TRGT_ADR_CNTS 01b, the talkgroup as target).
fn reg_tg(tg: u32, id: u32) -> [u8; 10] {
    rand(0b000_0001, 0b0100, spec::SK_REG, tg, id)
}

/// De-registration (6.4.6: Reg_Dereg = 0).
fn dereg(sc: u16, id: u32) -> [u8; 10] {
    rand(0b000_0000, 0b0000, spec::SK_REG, sc as u32, id)
}

fn group(tg: u32, id: u32, so: u8) -> [u8; 10] {
    rand(so, 0, spec::SK_TG_VOICE, tg, id)
}

fn private(called: u32, id: u32) -> [u8; 10] {
    rand(0, 0, spec::SK_IND_VOICE, called, id)
}

/// Call Answer (table 6.38): ACCEPT is dep bit 1 (0 accepts); target the caller.
fn answer(caller: u32, id: u32, accept: bool) -> [u8; 10] {
    rand(0, if accept { 0b0000 } else { 0b0010 }, spec::SK_CALL_ANSWER, caller, id)
}

fn cancel(called: u32, id: u32) -> [u8; 10] {
    rand(0, 0, spec::SK_CANCEL, called, id)
}

/// C_ACKU / C_NACKU (table 7.27): target = the TS PDU's source.
fn acku(reason: u8, target: u32, id: u32) -> [u8; 10] {
    Bits::csbk(true, spec::C_ACKU).f(7, 0).f(8, reason as u64).f(1, 0).f(24, target as u64).f(24, id as u64).done()
}

/// P_MAINT DISCON (table 7.32): target TSI.
fn maint(id: u32) -> [u8; 10] {
    Bits::csbk(true, spec::P_MAINT).f(12, 0).f(3, 0).f(1, 0).f(24, spec::TSI as u64).f(24, id as u64).done()
}

// ── reading the site's blocks (bit positions from the 361-4 tables) ──

fn tgt(b: &[u8; 10]) -> u32 {
    field(b, 32, 24) as u32
}
fn src(b: &[u8; 10]) -> u32 {
    field(b, 56, 24) as u32
}

/// C_ACKD fields (table 7.23): (Response_Info, Reason, target, source).
fn ackd(b: &[u8; 10]) -> (u8, u8, u32, u32) {
    (field(b, 16, 7) as u8, field(b, 23, 8) as u8, tgt(b), src(b))
}

/// Grant fields (tables 7.9, 7.11, 7.12): (chan, LCN, Late_Entry or reserved, Emergency, Offset).
fn grant(b: &[u8; 10]) -> (u16, u8, bool, bool, bool) {
    (field(b, 16, 12) as u16, field(b, 28, 1) as u8, field(b, 29, 1) != 0, field(b, 30, 1) != 0, field(b, 31, 1) != 0)
}

/// C_ALOHA fields (table 7.19).
#[derive(Debug, PartialEq)]
struct AlohaF {
    reserved: u8,
    tsccas: u8,
    site_sync: u8,
    version: u8,
    offset: u8,
    active: u8,
    mask: u8,
    sf: u8,
    nrand_wait: u8,
    reg: u8,
    backoff: u8,
    syscode: u16,
    addr: u32,
}

fn aloha(b: &[u8; 10]) -> AlohaF {
    let f = |s, w| field(b, s, w);
    AlohaF {
        reserved: f(16, 1) as u8,
        tsccas: f(17, 1) as u8,
        site_sync: f(18, 1) as u8,
        version: f(19, 3) as u8,
        offset: f(22, 1) as u8,
        active: f(23, 1) as u8,
        mask: f(24, 5) as u8,
        sf: f(29, 2) as u8,
        nrand_wait: f(31, 4) as u8,
        reg: f(35, 1) as u8,
        backoff: f(36, 4) as u8,
        syscode: f(40, 16) as u16,
        addr: f(56, 24) as u32,
    }
}

/// C_BCAST fields (table 7.20): (Announcement_type, Parms1, Reg, Backoff, C_SYScode, Parms2).
fn bcast(b: &[u8; 10]) -> (u8, u16, u8, u8, u16, u32) {
    let f = |s, w| field(b, s, w);
    (f(16, 5) as u8, f(21, 14) as u16, f(35, 1) as u8, f(36, 4) as u8, f(40, 16) as u16, f(56, 24) as u32)
}

fn is(dt: u8, b: &[u8; 10], op: u8) -> bool {
    dt == spec::DT_CSBK && csbko(b) == op && b[1] == 0
}

// ── the rig: one site, slot by slot ──

struct Rig {
    s: Box<Site>,
    cfg: Cfg,
    k: i64,
    hist: Vec<(i64, SlotOut)>,
}

impl Rig {
    fn new(f: impl FnOnce(&mut Cfg)) -> Rig {
        let mut c = cfg_default();
        f(&mut c);
        Rig { s: new_site(&c), cfg: c, k: 0, hist: Vec::new() }
    }

    /// The site's C_SYScode, built by hand from its settings (figure 6.15).
    fn sc(&self) -> u16 {
        spec::syscode(self.cfg.model, self.cfg.net as u32, self.cfg.site as u32, self.cfg.par)
    }

    /// NWait in frames for the configured NRand_Wait (table 6.2).
    fn nwait(&self) -> i64 {
        spec::NWAIT[self.cfg.nrand_wait as usize] as i64
    }

    fn step(&mut self) -> (i64, SlotOut) {
        let k = self.k;
        let o = self.s.slot(k, T0 + 30 * k);
        while self.s.next_event().is_some() {}
        self.hist.push((k, o));
        self.k += 1;
        (k, o)
    }

    fn run(&mut self, slots: i64) {
        for _ in 0..slots {
            self.step();
        }
    }

    /// Sends an MS CSBK in the next TSCC slot; returns that slot.
    fn send(&mut self, pdu: [u8; 10]) -> i64 {
        loop {
            let (k, _) = self.step();
            if k % 2 == 0 {
                let ch = self.s.tscc_ch();
                self.s.rx_pdu(ch, spec::DT_CSBK, &pdu);
                return k;
            }
        }
    }

    /// The first TSCC block at or after slot `from`, within `frames` TDMA frames, that `f` accepts.
    fn find(&mut self, from: i64, frames: i64, f: impl Fn(u8, &[u8; 10]) -> bool) -> Option<(i64, SlotOut)> {
        let end = from + 2 * frames;
        let mut i = self.hist.iter().position(|(k, _)| *k >= from).unwrap_or(self.hist.len());
        loop {
            while i < self.hist.len() {
                let (k, o) = self.hist[i];
                i += 1;
                if k > end {
                    return None;
                }
                if k % 2 == 0 && f(o.tscc_dt, &o.tscc_pdu) {
                    return Some((k, o));
                }
            }
            if self.k > end {
                return None;
            }
            self.step();
        }
    }

    /// Every TSCC block from slot `from` to `from + 2 * frames` that `f` accepts.
    fn all(&mut self, from: i64, frames: i64, f: impl Fn(u8, &[u8; 10]) -> bool) -> Vec<(i64, SlotOut)> {
        let end = from + 2 * frames;
        while self.k <= end {
            self.step();
        }
        self.hist.iter().filter(|(k, o)| *k >= from && *k <= end && k % 2 == 0 && f(o.tscc_dt, &o.tscc_pdu)).copied().collect()
    }

    /// The first C_ACKD (any kind) to `target` within `frames` frames of slot `from`.
    fn ack_to(&mut self, from: i64, frames: i64, target: u32) -> Option<(i64, (u8, u8, u32, u32))> {
        self.find(from, frames, |dt, b| is(dt, b, spec::C_ACKD) && tgt(b) == target).map(|(k, o)| (k, ackd(&o.tscc_pdu)))
    }

    /// The first C_ACKD to `target` with `reason` within `frames` frames of slot `from`.
    fn ack_with(&mut self, from: i64, frames: i64, target: u32, reason: u8) -> Option<(i64, (u8, u8, u32, u32))> {
        self.find(from, frames, |dt, b| is(dt, b, spec::C_ACKD) && tgt(b) == target && field(b, 23, 8) as u8 == reason)
            .map(|(k, o)| (k, ackd(&o.tscc_pdu)))
    }

    /// Registers `id` (with a talkgroup when given) and checks Reg_Accepted.
    fn register(&mut self, id: u32, tg: Option<u32>) {
        let pdu = match tg {
            Some(t) => reg_tg(t, id),
            None => reg(self.sc(), id),
        };
        let k = self.send(pdu);
        let (_, (_, reason, _, _)) = self.ack_to(k, 40, id).expect("a registration answer");
        assert_eq!(reason, spec::REG_ACCEPTED, "{id} registers");
    }

    /// A response to a random access in slot `k` must come within NWait frames of the next one (6.2.1.1.4,
    /// 6.2.1.1.5): its frame is at most NWait + 1 after the access.
    fn within_nwait(&self, k_in: i64, k_out: i64) -> bool {
        k_out > k_in && (k_out - k_in) / 2 <= self.nwait() + 1
    }
}

fn two_carriers(c: &mut Cfg) {
    c.ncarriers = 2;
    c.carriers[1] = Carrier { tx_hz: 438_600_000, rx_hz: 431_000_000, chan: 2, tscc: 0, enabled: 1 };
}

// ════ Control channel ════

/// 361-4 7.1.1.1.4, table 7.19: the C_ALOHA the site sends: LB 1, FID 0, reserved 0, Version 100b (7.2.32),
/// NRand_Wait from the settings, Reg from require_registration, C_SYScode from the settings, Mask 0 and ADRNULL
/// (every MS may access; tier3.md 3.2), Service Function 00b when the queue isn't full (3.3).
#[test]
fn aloha_fields_table_7_19() {
    let mut r = Rig::new(|c| c.nrand_wait = 9);
    let (_, o) = r.find(0, 8, |dt, b| is(dt, b, spec::C_ALOHA)).expect("an Aloha");
    let b = o.tscc_pdu;
    assert!(lb(&b));
    let a = aloha(&b);
    assert_eq!(a.reserved, 0);
    assert_eq!(a.version, spec::VERSION_1_11_1);
    assert_eq!(a.nrand_wait, 9);
    assert_eq!(a.reg, 1);
    assert_eq!(a.syscode, r.sc());
    assert_eq!((a.mask, a.addr), (0, spec::ADRNULL));
    assert_eq!(a.sf, 0b00);
    assert!((1..=15).contains(&a.backoff), "Backoff 0 is reserved (table 6.3)");
}

/// 5.1.2: Reg is 0 when the system doesn't require registration, in C_ALOHA and C_SYS_Parms alike.
#[test]
fn reg_flag_off() {
    let mut r = Rig::new(|c| c.reg_required = 0);
    let (_, o) = r.find(0, 8, |dt, b| is(dt, b, spec::C_ALOHA)).unwrap();
    assert_eq!(aloha(&o.tscc_pdu).reg, 0);
    let slc = r.hist.iter().find(|(_, o)| o.has_slc != 0).unwrap().1.slc_tscc;
    assert_eq!((slc >> 9) & 1, 0);
}

/// 7.2.32, table 7.93 and 6.3.2.2.1.1: the site's syscode follows its MODEL, NET, SITE and PAR in each model.
#[test]
fn aloha_syscode_each_model() {
    for (model, net, site, par) in [(0u8, 300u16, 5u16, 1u8), (1, 100, 30, 2), (2, 12, 200, 3), (3, 2, 1000, 3)] {
        let mut r = Rig::new(|c| {
            c.model = model;
            c.net = net;
            c.site = site;
            c.par = par;
            c.area_bits = 1;
        });
        let want = spec::syscode(model, net as u32, site as u32, par);
        let (_, o) = r.find(0, 8, |dt, b| is(dt, b, spec::C_ALOHA)).unwrap();
        assert_eq!(aloha(&o.tscc_pdu).syscode, want, "model {model}");
        assert_eq!(r.s.syscode(), want);
    }
}

/// 6.2.1.3 and tier3.md 3.2: an MS waits for a C_ALOHA before it accesses, so one goes out at least every fourth
/// TSCC slot even while grants, broadcasts and acknowledgements are due.
#[test]
fn aloha_at_least_every_fourth_tscc_slot() {
    let mut r = Rig::new(|c| {
        two_carriers(c);
        c.clock_ok = 1;
        c.chan_freq_ms = 2000;
        c.adjacent_ms = 1000;
    });
    r.s.set_neighbors(&[
        Neighbor { syscode: spec::syscode(1, 1, 2, 3), chan: 7, ..Default::default() },
        Neighbor { syscode: spec::syscode(1, 1, 3, 3), chan: 8, ..Default::default() },
    ]);
    r.register(A, Some(TG));
    r.register(B, Some(TG2));
    r.register(C, None);
    r.send(group(TG, A, 0));
    r.send(group(TG2, B, 0));
    r.send(private(A, C));
    r.run(3000);
    let tscc: Vec<bool> =
        r.hist.iter().filter(|(k, _)| k % 2 == 0).map(|(_, o)| is(o.tscc_dt, &o.tscc_pdu, spec::C_ALOHA)).collect();
    for (i, w) in tscc.windows(4).enumerate() {
        assert!(w.iter().any(|&x| x), "no C_ALOHA in TSCC slots {i}..{}", i + 4);
    }
}

/// 6.2.1.1.3: the CACH following an Aloha has AT = 0 (Aloha PDUs never withdraw a slot).
#[test]
fn at_zero_after_aloha() {
    let mut r = Rig::new(|_| {});
    r.run(400);
    let alohas: Vec<_> = r.hist.iter().filter(|(k, o)| k % 2 == 0 && is(o.tscc_dt, &o.tscc_pdu, spec::C_ALOHA)).collect();
    assert!(alohas.len() > 50);
    assert!(alohas.iter().all(|(_, o)| o.at == 0));
}

/// 7.1.2.1, table 7.33 and 5.1.3: C_SYS_Parms in every 4-slot short LC (120 ms): SLCO 0010b, the syscode's 14
/// MSBs, Reg as in C_ALOHA, a Common_Slot_Counter that goes up by one each time.
#[test]
fn c_sys_parms_every_120_ms() {
    let mut r = Rig::new(|_| {});
    r.run(200);
    let slcs: Vec<(i64, u32)> = r.hist.iter().filter(|(_, o)| o.has_slc != 0).map(|(k, o)| (*k, o.slc_tscc)).collect();
    assert_eq!(slcs.len(), 50, "one per 4 slots");
    for w in slcs.windows(2) {
        assert_eq!(w[1].0 - w[0].0, 4, "120 ms apart");
        assert_eq!((w[1].1 & 0x1FF), ((w[0].1 & 0x1FF) + 1) % 512, "counter + 1");
    }
    let sc = r.sc();
    for (_, slc) in &slcs {
        assert_eq!(slc >> 28, 0, "28 bits");
        assert_eq!((slc >> 24) as u8, spec::SLCO_C_SYS_PARMS);
        assert_eq!(((slc >> 10) & 0x3FFF) as u16, sc >> 2);
        assert_eq!((slc >> 9) & 1, 1, "Reg as in the Aloha");
    }
}

/// 5.1.3: the counter rolls over from 511 to 0.
#[test]
fn common_slot_counter_wraps() {
    let mut r = Rig::new(|_| {});
    r.k = 4 * 500;
    r.run(4 * 20);
    let c: Vec<u32> = r.hist.iter().filter(|(_, o)| o.has_slc != 0).map(|(_, o)| o.slc_tscc & 0x1FF).collect();
    let i = c.iter().position(|&x| x == 511).expect("511 seen");
    assert_eq!(c[i + 1], 0);
    assert_eq!(c[i + 2], 1);
}

/// 7.1.2.2, table 7.34: a carrier with two payload slots sends P_SYS_Parms: SLCO 0011b, the same syscode bits and
/// counter, Payload Channel Type 0 (normal payload channel; table 7.95).
#[test]
fn p_sys_parms_payload_carrier() {
    let mut r = Rig::new(two_carriers);
    r.run(40);
    let sc = r.sc();
    for (_, o) in r.hist.iter().filter(|(_, o)| o.has_slc != 0) {
        let p = o.slc_payload;
        assert_eq!((p >> 24) as u8, spec::SLCO_P_SYS_PARMS);
        assert_eq!(((p >> 10) & 0x3FFF) as u16, sc >> 2);
        assert_eq!((p >> 9) & 1, 0);
        assert_eq!(p & 0x1FF, o.slc_tscc & 0x1FF, "one Common_Slot_Counter");
    }
}

// ── broadcasts (7.1.1.1.5, 7.2.19) ──

/// 7.2.19.6, table 7.78; 7.1.1.1.5.1, table 7.21; annex C.1.1.4: Chan_Freq is an MBC header (LB 0) followed at
/// once by a BC_AP block (LB 1, Cdeftype 0000b) with the channel's frequencies as the MS uses them: its TX is the
/// carrier's receive frequency.
#[test]
fn chan_freq_mbc_and_bc_ap() {
    let mut r = Rig::new(|c| {
        two_carriers(c);
        c.chan_freq_ms = 2000;
    });
    let hdrs = r.all(0, 400, |dt, b| dt == spec::DT_MBC_HEADER && csbko(b) == spec::C_BCAST);
    assert!(!hdrs.is_empty(), "Chan_Freq sent");
    let mut seen = [false; 3];
    for (k, o) in hdrs {
        let b = o.tscc_pdu;
        assert!(!lb(&b), "an MBC header has LB 0");
        let (ann, p1, _, _, sc, p2) = bcast(&b);
        assert_eq!(ann, spec::CHAN_FREQ);
        assert_eq!(p1, 0);
        assert_eq!(sc, r.sc());
        assert_eq!(p2 >> 12, 0, "12 reserved bits");
        let chan = (p2 & 0xFFF) as u16;
        let (_, cont) = *r.hist.iter().find(|(kk, _)| *kk == k + 2).expect("the next TSCC slot");
        assert_eq!(cont.tscc_dt, spec::DT_MBC_CONT, "the continuation follows at once");
        let c = cont.tscc_pdu;
        assert!(lb(&c), "the last block has LB 1");
        assert_eq!(csbko(&c), spec::C_BCAST);
        assert_eq!(field(&c, 8, 8), 0, "reserved");
        assert_eq!(field(&c, 16, 4), 0, "Cdeftype 0000b");
        assert_eq!(field(&c, 22, 12) as u16, chan, "CHAN matches the header");
        let hz = |m: usize, k: usize| field(&c, m, 10) as u32 * 1_000_000 + field(&c, k, 13) as u32 * 125;
        let (tx, rx) = (hz(34, 44), hz(57, 67));
        let car = r.cfg.carriers.iter().find(|x| x.chan == chan).expect("a configured channel");
        assert_eq!((tx, rx), (car.rx_hz, car.tx_hz), "chan {chan}: MS TX = carrier RX");
        seen[chan as usize] = true;
    }
    assert!(seen[1] && seen[2], "both channels announced");
}

/// 7.2.19.7, table 7.79: Adjacent_Site from the neighbor list.
#[test]
fn adjacent_site_broadcast() {
    let mut r = Rig::new(|c| c.adjacent_ms = 1000);
    let n = spec::syscode(1, 1, 9, 3);
    r.s.set_neighbors(&[Neighbor { syscode: n, chan: 0x2C5, active_known: 1, active: 1, prio_confirmed: 6, prio_adjacent: 2 }]);
    let (_, o) = r.find(0, 200, |dt, b| is(dt, b, spec::C_BCAST) && field(b, 16, 5) as u8 == spec::ADJACENT_SITE).expect("sent");
    let (_, p1, reg, _, sc, p2) = bcast(&o.tscc_pdu);
    assert_eq!(p1, n >> 2, "the neighbor's 14 syscode MSBs");
    assert_eq!(sc, r.sc(), "our own syscode");
    assert_eq!(reg, 1);
    assert_eq!(p2 >> 23, 1, "information available");
    assert_eq!((p2 >> 22) & 1, 1, "Active_Connection");
    assert_eq!((p2 >> 19) & 7, 6, "confirmed priority");
    assert_eq!((p2 >> 16) & 7, 2, "adjacent priority");
    assert_eq!((p2 >> 12) & 0xF, 0, "reserved");
    assert_eq!(p2 & 0xFFF, 0x2C5, "CH_ADJ");
}

/// 7.2.19.4, tables 7.73 to 7.75: Local_Time carries the local date and time, the UTC offset and the day of week.
#[test]
fn local_time_broadcast() {
    let mut r = Rig::new(|c| {
        c.clock_ok = 1;
        c.utc_offset_min = -300;
        c.local_time_ms = 2000;
    });
    let (k, o) = r.find(0, 400, |dt, b| is(dt, b, spec::C_BCAST) && field(b, 16, 5) as u8 == spec::LOCAL_TIME).expect("sent");
    let (_, p1, _, _, sc, p2) = bcast(&o.tscc_pdu);
    assert_eq!(sc, r.sc());
    // the slot's local time, worked out by hand: T0 is 21:04:11 UTC, so 16:04:11 at UTC-5, the same day
    let now_s = (T0 + 30 * k) / 1000;
    let day_start = T0 / 1000 - (21 * 3600 + 4 * 60 + 11);
    let local = now_s - day_start - 5 * 3600;
    let (h, m, s) = (local / 3600, local / 60 % 60, local % 60);
    assert_eq!(p1 >> 9, 5, "day");
    assert_eq!((p1 >> 5) & 0xF, 11, "month");
    assert_eq!((p1 >> 1) & 0xF, 5, "UTC offset hours");
    assert_eq!(p1 & 1, 1, "sign: behind UTC");
    assert_eq!(p2 >> 19, h as u32);
    assert_eq!((p2 >> 13) & 0x3F, m as u32);
    assert!(((p2 >> 7) & 0x3F) as i64 == s || ((p2 >> 7) & 0x3F) as i64 == s - 1, "seconds");
    assert_eq!((p2 >> 4) & 7, 5, "Thursday (1 = Sunday)");
    assert_eq!((p2 >> 2) & 3, 0, "no fraction");
}

/// tier3.md 3.2: no Local_Time while the clock isn't good.
#[test]
fn local_time_needs_clock() {
    let mut r = Rig::new(|c| {
        c.clock_ok = 0;
        c.local_time_ms = 1000;
    });
    assert!(r.find(0, 600, |dt, b| is(dt, b, spec::C_BCAST) && field(b, 16, 5) as u8 == spec::LOCAL_TIME).is_none());
}

/// 7.2.19.8, table 7.80: Gen_Site_Params with our syscode; not hibernating (7.2.40).
#[test]
fn gen_site_params_broadcast() {
    let mut r = Rig::new(|_| {});
    let frames = r.cfg.gen_site_ms as i64 / 60 + 20;
    let (_, o) = r.find(0, frames, |dt, b| is(dt, b, spec::C_BCAST) && field(b, 16, 5) as u8 == spec::GEN_SITE_PARAMS).expect("sent");
    let (_, p1, reg, _, sc, p2) = bcast(&o.tscc_pdu);
    assert_eq!((p1, reg, sc), (0, 1, r.sc()));
    assert_eq!((p2 >> 22) & 1, 0, "not hibernating");
}

// ════ Random access and registration ════

/// 6.4.4.1, table 7.23: registration answered C_ACKD(Reg_Accepted), target the MS, source REGI, within NWait.
#[test]
fn registration_accepted_from_regi() {
    let mut r = Rig::new(|_| {});
    r.run(20);
    let k = r.send(reg(r.sc(), A));
    let (ko, (_, reason, target, source)) = r.ack_to(k, 30, A).expect("answered");
    assert_eq!(reason, spec::REG_ACCEPTED);
    assert_eq!((target, source), (A, spec::REGI));
    assert!(r.within_nwait(k, ko), "answer at frame +{}", (ko - k) / 2);
    assert!(r.s.record(A).is_some());
}

/// 6.2.1.1.4: a response to every random access, within NWait, for each NRand_Wait setting.
#[test]
fn response_within_nwait_each_setting() {
    for nrw in [0u8, 1, 3, 6, 14] {
        let mut r = Rig::new(|c| c.nrand_wait = nrw);
        r.run(10);
        for id in [A, B, C] {
            let k = r.send(reg(r.sc(), id));
            let (ko, _) = r.ack_to(k, 40, id).expect("answered");
            assert!(r.within_nwait(k, ko), "NRand_Wait {nrw}: frame +{}", (ko - k) / 2);
        }
    }
}

/// 6.4.4.1.13 and table 6.9: a talkgroup subscription (TRGT_ADR_CNTS 01b) is accepted and recorded.
#[test]
fn registration_with_talkgroup() {
    let mut r = Rig::new(|_| {});
    let k = r.send(reg_tg(TG, A));
    let (_, (_, reason, target, source)) = r.ack_to(k, 30, A).unwrap();
    assert_eq!((reason, target, source), (spec::REG_ACCEPTED, A, spec::REGI));
    assert_eq!(r.s.record(A).unwrap().sub_tg, TG);
}

/// 6.4.4.1.13.2 and tier3.md 3.17: a list subscription (TRGT_ADR_CNTS 10b, target TATTSI): the TSCC asks for the
/// list with a C_AHOY; an MS that never uploads it is registered with every talkgroup refused, Reg_Subscription
/// (0110 0101b) with an index pattern of 0 (table 6.10). The answer comes from TATTSI, the C_RAND's target: 6.4.4.1.13.2
/// names TATTSI as the source of a refusal and doesn't give the final C_ACK another.
#[test]
fn registration_list_subscription() {
    let mut r = Rig::new(|_| {});
    let k = r.send(rand(0b000_0001, 0b1000, spec::SK_REG, spec::TATTSI, A));
    let (_, (ri, reason, target, source)) = r.ack_to(k, 30, A).unwrap();
    assert_eq!((reason, ri, target, source), (spec::REG_SUBSCRIPTION, 0, A, spec::TATTSI));
}

/// 6.4.4.1.0 and tier3.md 3.4: a registration whose REG_ADDR isn't this site's C_SYScode is discarded: no answer
/// and no record.
#[test]
fn registration_other_syscode_discarded() {
    let mut r = Rig::new(|_| {});
    let other = spec::syscode(1, 1, 2, 3);
    let k = r.send(reg(other, A));
    assert!(r.find(k, 60, |dt, b| dt == spec::DT_CSBK && tgt(b) == A).is_none(), "no PDU to the MS");
    assert!(r.s.record(A).is_none());
}

/// 6.4.6: de-registration's only answer is C_ACKD(Reg_Accepted), and the record goes.
#[test]
fn deregistration() {
    let mut r = Rig::new(|_| {});
    r.register(A, Some(TG));
    let k = r.send(dereg(r.sc(), A));
    let (ko, (_, reason, target, source)) = r.ack_to(k, 30, A).unwrap();
    assert_eq!((reason, target, source), (spec::REG_ACCEPTED, A, spec::REGI));
    assert!(r.within_nwait(k, ko));
    assert!(r.s.record(A).is_none());
}

/// 6.4.6: the same answer from an MS the site doesn't know.
#[test]
fn deregistration_unknown_ms() {
    let mut r = Rig::new(|_| {});
    let k = r.send(dereg(r.sc(), B));
    let (_, (_, reason, _, source)) = r.ack_to(k, 30, B).unwrap();
    assert_eq!((reason, source), (spec::REG_ACCEPTED, spec::REGI));
}

fn listed(r: &mut Rig, refuse: bool) {
    let mut p = Policy::default_open();
    p.open = 0;
    p.refuse_unlisted = refuse as u8;
    p.ms = IdSet::of(&[(1000, 1999)]);
    r.s.policy_set(&p);
}

/// 6.4.4.1, 6.4.2: an MS not allowed here gets C_NACKD(Reg_Denied) from REGI.
#[test]
fn registration_denied() {
    let mut r = Rig::new(|_| {});
    listed(&mut r, false);
    let k = r.send(reg(r.sc(), B));
    let (_, (_, reason, target, source)) = r.ack_to(k, 30, B).unwrap();
    assert_eq!((reason, target, source), (spec::REG_DENIED, B, spec::REGI));
    assert!(r.s.record(B).is_none());
    // a listed MS still registers
    r.register(A, None);
}

/// 6.4.4.1: or C_NACKD(Reg_Refused), as the provisioning says.
#[test]
fn registration_refused() {
    let mut r = Rig::new(|_| {});
    listed(&mut r, true);
    let k = r.send(reg(r.sc(), B));
    let (_, (_, reason, target, source)) = r.ack_to(k, 30, B).unwrap();
    assert_eq!((reason, target, source), (spec::REG_REFUSED, B, spec::REGI));
}

/// 6.4.4.1.12: with registration required, a service request from an unregistered MS gets
/// C_NACKD(MS_Not_Registered).
#[test]
fn service_needs_registration() {
    let mut r = Rig::new(|_| {});
    let k = r.send(group(TG, A, 0));
    let (ko, (_, reason, target, _)) = r.ack_to(k, 30, A).unwrap();
    assert_eq!((reason, target), (spec::MS_NOT_REGISTERED, A));
    assert!(r.within_nwait(k, ko));
    assert!(r.find(k, 30, |dt, b| is(dt, b, spec::TV_GRANT)).is_none());
}

/// 7.2.7, table 7.41: an acknowledgement that isn't Reg_Accepted, Status or Reg_Subscription carries G/I (0: the
/// target is an MS) and the six Response_Check bits from NET + SITE (bits 8 to 3 of figure 6.19).
#[test]
fn response_info_check_bits() {
    let mut r = Rig::new(|c| {
        c.model = 2;
        c.net = 0xB;
        c.site = 0x6D;
        c.area_bits = 4;
    });
    let want = ((r.sc() >> 2) & 0x3F) as u8;
    assert_ne!(want, 0);
    let k = r.send(group(TG, A, 0));
    let (_, (ri, reason, _, _)) = r.ack_to(k, 30, A).unwrap();
    assert_eq!(reason, spec::MS_NOT_REGISTERED);
    assert_eq!(ri, want, "G/I 0 + Response_Check");
    // and on a positive one: the cancel's Message_Accepted
    r.register(A, None);
    r.register(B, None);
    r.send(private(B, A));
    let k = r.send(cancel(B, A));
    let (_, (ri, _, _, _)) = r.ack_with(k, 30, A, spec::MESSAGE_ACCEPTED).expect("Message_Accepted");
    assert_eq!(ri, want);
    // and on the C_WACKD the caller gets while the called MS is checked
    let mut r2 = two(false);
    let k = r2.send(private(B, A));
    if let Some((_, (ri, _, _, _))) = r2.ack_with(k, 120, A, spec::WAIT) {
        assert_eq!(ri, ((r2.sc() >> 2) & 0x3F) as u8);
    }
}

/// 6.4.1.2, figure 6.19 and table 6.5: SYS_AREA is the DMRLA most significant bits of SITE.
#[test]
fn record_area_is_site_msbs() {
    for (bits, site, area) in [(3u8, 0b10110u16, 0b101u8), (5, 0b10110, 0b10110), (1, 0b10110, 1), (2, 0b01001, 0b01)] {
        let mut r = Rig::new(|c| {
            c.area_bits = bits;
            c.site = site;
        });
        r.register(A, None);
        assert_eq!(r.s.record(A).unwrap().area, area, "DMRLA {bits}, site {site:#b}");
    }
    // the huge model: 10 SITE bits, DMRLA 8
    let mut r = Rig::new(|c| {
        c.model = 3;
        c.net = 1;
        c.site = 0b11_0101_1010;
        c.area_bits = 8;
    });
    r.register(A, None);
    assert_eq!(r.s.record(A).unwrap().area, 0b1101_0110);
}

// ════ Group calls ════

fn tv(dt: u8, b: &[u8; 10]) -> bool {
    is(dt, b, spec::TV_GRANT)
}

/// 6.6.2.1.1, 7.1.1.1.1.2, table 7.11: a group request gets TV_GRANT for the payload slot (the carrier's channel
/// number, LCN 1 for TS2), target the talkgroup, source the caller, Late_Entry 0 at set-up, within NWait.
#[test]
fn group_call_tv_grant() {
    let mut r = Rig::new(|_| {});
    r.register(A, Some(TG));
    let k = r.send(group(TG, A, 0));
    let (k1, o) = r.find(k, 30, tv).expect("TV_GRANT");
    assert!(r.within_nwait(k, k1));
    let b = o.tscc_pdu;
    assert!(lb(&b));
    assert_eq!((tgt(&b), src(&b)), (TG, A));
    let (chan, lcn, late, emerg, offset) = grant(&b);
    assert_eq!((chan, lcn), (r.cfg.carriers[0].chan, 1));
    assert_eq!((late, emerg, offset), (false, false, false));
    assert_eq!(o.at, 0, "a grant solicits nothing");
}

/// 6.6.1.6.2: the payload channel is on from the first grant.
#[test]
fn payload_channel_active_at_grant() {
    let mut r = Rig::new(|_| {});
    r.register(A, Some(TG));
    let k = r.send(group(TG, A, 0));
    let (k1, o) = r.find(k, 30, tv).unwrap();
    assert_ne!(o.ch[1].r#use, USE_IDLE, "slot {k1}");
    assert_ne!(o.ch[1].r#use, USE_OFF);
}

/// 6.6.2.4 and tier3.md 3.5: grants repeated through the call carry Late_Entry 1.
#[test]
fn group_grant_repeats_late_entry() {
    let mut r = Rig::new(|_| {});
    r.register(A, Some(TG));
    let k = r.send(group(TG, A, 0));
    let grants = r.all(k, 100, tv);
    assert!(grants.len() >= 4, "{} grants", grants.len());
    assert!(!grant(&grants[0].1.tscc_pdu).2 && !grant(&grants[1].1.tscc_pdu).2, "set-up: Late_Entry 0");
    for (_, o) in &grants[2..] {
        let b = o.tscc_pdu;
        assert_eq!((tgt(&b), src(&b)), (TG, A));
        assert!(grant(&b).2, "repeat: Late_Entry 1");
    }
}

/// 7.1.1.1.1.2, table 7.12: a call to ALLMSID is a broadcast: BTV_GRANT.
#[test]
fn allcall_btv_grant() {
    let mut r = Rig::new(|_| {});
    r.register(A, None);
    let k = r.send(group(spec::ALLMSID, A, 0));
    let (_, o) = r.find(k, 30, |dt, b| is(dt, b, spec::BTV_GRANT)).expect("BTV_GRANT");
    assert_eq!((tgt(&o.tscc_pdu), src(&o.tscc_pdu)), (spec::ALLMSID, A));
    assert!(r.find(k, 30, tv).is_none());
}

/// Table 7.52 and table 7.11: an emergency request's grant has Emergency 1.
#[test]
fn emergency_grant_bit() {
    let mut r = Rig::new(|_| {});
    r.register(A, Some(TG));
    let k = r.send(group(TG, A, 0b100_0000));
    let (_, o) = r.find(k, 30, tv).unwrap();
    assert!(grant(&o.tscc_pdu).3);
}

/// P_CLEAR fields (table 7.30) on payload channel `ch`.
fn clears(r: &Rig, ch: usize) -> Vec<(i64, [u8; 10])> {
    r.hist
        .iter()
        .filter(|(_, o)| o.ch[ch].dt == spec::DT_CSBK && csbko(&o.ch[ch].pdu) == spec::P_CLEAR)
        .map(|(k, o)| (*k, o.ch[ch].pdu))
        .collect()
}

fn check_clear(b: &[u8; 10]) {
    assert!(lb(b));
    assert_eq!(b[1], 0, "FID");
    assert_eq!(field(b, 16, 12), 0, "channel 0: back to the control channel");
    assert_eq!(field(b, 28, 3), 0, "reserved");
    assert_eq!(field(b, 31, 1), 0, "G/I 0 for ALLMSI");
    assert_eq!((tgt(b), src(b)), (spec::ALLMSI, spec::TSI));
}

/// 6.6.2.3.1.6 (d), 7.1.1.3.2: with no voice, the call clears at hang expiry with P_CLEAR on the payload channel
/// (repeated, tier3.md 3.5: 4 times), then the channel is idle.
#[test]
fn hang_expiry_p_clear() {
    let mut r = Rig::new(|_| {});
    r.register(A, Some(TG));
    let k = r.send(group(TG, A, 0));
    let wait = (r.cfg.start_grace_ms + r.cfg.hang_ms) as i64 / 30 + 100;
    r.run(wait);
    let c = clears(&r, 1);
    assert_eq!(c.len(), r.cfg.clear_repeats as usize);
    for (kc, b) in &c {
        assert!(*kc > k);
        assert_eq!(kc % 2, 1, "on TS2");
        check_clear(b);
    }
    assert_eq!(r.hist.last().unwrap().1.ch[1].r#use, USE_IDLE);
}

/// 6.6.2.3.1.6 (b), 6.6.3.3.1: P_MAINT(DISCON) from the caller (target TSI, table 7.32) clears the call with
/// P_CLEAR at once.
#[test]
fn p_maint_discon_clears() {
    let mut r = Rig::new(|_| {});
    r.register(A, Some(TG));
    r.register(B, None);
    let k = r.send(private(B, A));
    let _ = r.find(k, 30, |dt, b| is(dt, b, spec::C_AHOY)).expect("C_AHOY");
    let k = r.send(acku(spec::MS_ACCEPTED, A, B));
    let _ = r.find(k, 30, |dt, b| is(dt, b, spec::PV_GRANT)).expect("PV_GRANT");
    r.run(20);
    r.s.rx_pdu(1, spec::DT_CSBK, &maint(A));
    let k = r.k;
    r.run(20);
    let c: Vec<_> = clears(&r, 1).into_iter().filter(|(kc, _)| *kc >= k).collect();
    assert!(!c.is_empty(), "P_CLEAR");
    assert!(c[0].0 - k <= 4, "at once");
    check_clear(&c[0].1);
}

/// 6.6.2.1.3 a) 3): no free payload channel: C_QACKD (Queued for resource), with Response_Info as table 7.41.
#[test]
fn busy_queued() {
    let mut r = Rig::new(|_| {});
    r.register(A, Some(TG));
    r.register(B, Some(TG2));
    let k = r.send(group(TG, A, 0));
    r.find(k, 30, tv).unwrap();
    let k = r.send(group(TG2, B, 0));
    let (ko, (ri, reason, target, _)) = r.ack_to(k, 30, B).expect("an answer");
    assert_eq!((reason, target), (spec::QUEUED_RESOURCE, B));
    assert_eq!(ri, ((r.sc() >> 2) & 0x3F) as u8);
    assert!(r.within_nwait(k, ko));
}

// ════ Individual calls ════

fn ahoy(dt: u8, b: &[u8; 10]) -> bool {
    is(dt, b, spec::C_AHOY)
}

/// C_AHOY fields (table 7.22): (Service_Options, SKF, ALS, G/I, Appended, Service_Kind).
fn ahoy_f(b: &[u8; 10]) -> (u8, u8, u8, u8, u8, u8) {
    let f = |s, w| field(b, s, w) as u8;
    (f(16, 7), f(23, 1), f(24, 1), f(25, 1), f(26, 2), f(28, 4))
}

fn two(foacsu: bool) -> Rig {
    let mut r = Rig::new(|c| c.foacsu = foacsu as u8);
    r.register(A, None);
    r.register(B, None);
    r
}

/// 6.6.2.1.1 c), 6.6.2.1.4 a), table 7.22: an individual call's availability check: C_AHOY to the called MS,
/// source the caller, Service_Kind 0000b, SKF 0 for OACSU, G/I 0; it solicits, so AT = 1 after it (6.2.1.1.3);
/// it is the caller's response within NWait.
#[test]
fn oacsu_ahoy() {
    let mut r = two(false);
    let k = r.send(private(B, A));
    let (ka, o) = r.find(k, 30, ahoy).expect("C_AHOY");
    assert!(r.within_nwait(k, ka));
    let b = o.tscc_pdu;
    assert_eq!((tgt(&b), src(&b)), (B, A));
    let (_, skf, als, gi, ab, sk) = ahoy_f(&b);
    assert_eq!((skf, als, gi, ab, sk), (0, 0, 0, 0, spec::SK_IND_VOICE));
    assert_eq!(o.at, 1, "the next inbound frame is withdrawn");
}

/// 6.2.1.1.3: AT = 1 only after a block that asks for an answer; never after an Aloha, a grant or a C_ACKD.
#[test]
fn at_only_after_soliciting_block() {
    let mut r = two(false);
    let k = r.send(private(B, A));
    r.find(k, 30, ahoy).unwrap();
    let k = r.send(acku(spec::MS_ACCEPTED, A, B));
    r.find(k, 30, |dt, b| is(dt, b, spec::PV_GRANT)).unwrap();
    r.run(200);
    for (k, o) in r.hist.iter().filter(|(k, _)| k % 2 == 0) {
        let b = &o.tscc_pdu;
        if ahoy(o.tscc_dt, b) {
            assert_eq!(o.at, 1, "slot {k}");
        } else if o.tscc_dt == spec::DT_CSBK
            && [spec::C_ALOHA, spec::C_ACKD, spec::PV_GRANT, spec::TV_GRANT, spec::C_BCAST].contains(&csbko(b))
        {
            assert_eq!(o.at, 0, "slot {k}, CSBKO {:#x}", csbko(b));
        }
    }
}

/// 6.6.2.1.4 and 6.6.2.2.6: the called MS accepts with C_ACKU(MS_Accepted) (target = the AHOY's source, table
/// 7.27): PV_GRANT, target the called MS, source the caller, its reserved bit 0 (table 7.9), twice.
#[test]
fn oacsu_ms_accepted_pv_grant() {
    let mut r = two(false);
    let k = r.send(private(B, A));
    r.find(k, 30, ahoy).unwrap();
    let k = r.send(acku(spec::MS_ACCEPTED, A, B));
    let g = r.all(k, 60, |dt, b| is(dt, b, spec::PV_GRANT));
    assert!(g.len() >= 2, "granted twice");
    assert!(r.within_nwait(k, g[0].0));
    for (_, o) in &g {
        let b = o.tscc_pdu;
        assert_eq!((tgt(&b), src(&b)), (B, A));
        let (chan, lcn, reserved, _, _) = grant(&b);
        assert_eq!((chan, lcn), (r.cfg.carriers[0].chan, 1));
        assert!(!reserved, "PV_GRANT's reserved bit (repeats too)");
    }
}

/// 6.6.2.1.4 says the TSCC allocates a payload channel on C_ACKU(Message_Accepted); accept that too.
#[test]
fn oacsu_message_accepted_pv_grant() {
    let mut r = two(false);
    let k = r.send(private(B, A));
    r.find(k, 30, ahoy).unwrap();
    let k = r.send(acku(spec::MESSAGE_ACCEPTED, A, B));
    assert!(r.find(k, 30, |dt, b| is(dt, b, spec::PV_GRANT)).is_some());
}

/// 6.6.2.1.4: C_ACKU(CallBack): C_ACKD(mirrored CallBack) to the caller, no grant.
#[test]
fn callback_mirrored() {
    let mut r = two(false);
    let k = r.send(private(B, A));
    r.find(k, 30, ahoy).unwrap();
    let k = r.send(acku(spec::CALLBACK, A, B));
    r.ack_with(k, 30, A, spec::CALLBACK).expect("C_ACKD(CallBack) to the caller");
    assert!(r.find(k, 60, |dt, b| is(dt, b, spec::PV_GRANT)).is_none());
}

/// 6.6.2.1.4: C_NACKU from the called MS: C_NACKD to the caller with the reason mirrored.
#[test]
fn nacku_mirrored() {
    let mut r = two(false);
    let k = r.send(private(B, A));
    r.find(k, 30, ahoy).unwrap();
    let k = r.send(acku(spec::EQUIP_BUSY, A, B));
    r.ack_with(k, 30, A, spec::EQUIP_BUSY).expect("C_NACKD(mirrored) to the caller");
    assert!(r.find(k, 60, |dt, b| is(dt, b, spec::PV_GRANT)).is_none());
}

/// 6.6.2.1.4 (the C_AHOY may be repeated) and tier3.md 3.5: no answer: C_NACKD(MSaway_Refused) to the caller.
#[test]
fn no_answer_msaway() {
    let mut r = two(false);
    let k = r.send(private(B, A));
    let frames = (r.cfg.ahoy_tries as i64 + 2) * r.cfg.ahoy_every_ms as i64 / 60 + 50;
    let got = r.find(k, frames, |dt, b| is(dt, b, spec::C_ACKD) && tgt(b) == A && field(b, 23, 2) == 0);
    let (_, o) = got.expect("C_NACKD");
    assert_eq!(field(&o.tscc_pdu, 23, 8) as u8, spec::MSAWAY_REFUSED);
    let n = r.all(k, frames, ahoy).len();
    assert!((1..=r.cfg.ahoy_tries as usize).contains(&n), "{n} C_AHOYs");
}

/// 6.6.1.4, 6.6.2.1.3 a) 4): FOACSU: C_AHOY with SKF 1; the called MS's C_ACKU(MS_Alerting) goes back to the
/// caller as C_ACKD(mirrored MS_Alerting); its Call Answer (accept) brings PV_GRANT.
#[test]
fn foacsu_alerting_then_answer() {
    let mut r = two(true);
    let k = r.send(private(B, A));
    let (_, o) = r.find(k, 30, ahoy).unwrap();
    assert_eq!(ahoy_f(&o.tscc_pdu).1, 1, "SKF 1");
    let k = r.send(acku(spec::MS_ALERTING, A, B));
    r.ack_with(k, 30, A, spec::MS_ALERTING).expect("C_ACKD(mirrored MS_Alerting) to the caller");
    let k = r.send(answer(A, B, true));
    let (_, o) = r.find(k, 30, |dt, b| is(dt, b, spec::PV_GRANT)).expect("PV_GRANT");
    assert_eq!((tgt(&o.tscc_pdu), src(&o.tscc_pdu)), (B, A));
}

/// 6.6.1.4: the called MS rejects in its Call Answer: C_NACKD(Recipient_Refused, mirrored) to the caller, an
/// acknowledgement of the Call Answer (Message_Accepted), and no grant.
#[test]
fn foacsu_answer_reject() {
    let mut r = two(true);
    let k = r.send(private(B, A));
    r.find(k, 30, ahoy).unwrap();
    let k = r.send(acku(spec::MS_ALERTING, A, B));
    r.ack_with(k, 30, A, spec::MS_ALERTING).unwrap();
    let k = r.send(answer(A, B, false));
    let n = r.find(k, 30, |dt, b| is(dt, b, spec::C_ACKD) && tgt(b) == A && field(b, 23, 8) as u8 == spec::RECIPIENT_REFUSED);
    assert!(n.is_some(), "C_NACKD(Recipient_Refused) to the caller");
    let a = r.find(k, 30, |dt, b| {
        is(dt, b, spec::C_ACKD) && field(b, 23, 8) as u8 == spec::MESSAGE_ACCEPTED && (tgt(b) == A || tgt(b) == B)
    });
    assert!(a.is_some(), "the Call Answer acknowledged");
    assert!(r.find(k, 60, |dt, b| is(dt, b, spec::PV_GRANT)).is_none());
}

/// 6.6.1.2: a cancel is answered C_ACKD(Message_Accepted) to the caller.
#[test]
fn cancel_acknowledged() {
    let mut r = two(false);
    let k = r.send(private(B, A));
    r.find(k, 30, ahoy).unwrap();
    let k = r.send(cancel(B, A));
    let (ko, _) = r.ack_with(k, 30, A, spec::MESSAGE_ACCEPTED).expect("Message_Accepted to the caller");
    assert!(r.within_nwait(k, ko));
    assert!(r.find(k, 60, |dt, b| is(dt, b, spec::PV_GRANT)).is_none());
}

/// 6.6.1.2.2, table 6.37: when the called MS had answered (FOACSU, alerting), the cancel also goes to it as a
/// C_AHOY: Service_Options 0, SKF 0, G/I 0, Service_Kind 1111b, target the called MS, source the caller.
#[test]
fn cancel_after_answer_ahoy() {
    let mut r = two(true);
    let k = r.send(private(B, A));
    r.find(k, 30, ahoy).unwrap();
    let k = r.send(acku(spec::MS_ALERTING, A, B));
    r.ack_with(k, 30, A, spec::MS_ALERTING).unwrap();
    let k = r.send(cancel(B, A));
    let (_, o) = r.find(k, 30, |dt, b| ahoy(dt, b) && field(b, 28, 4) as u8 == spec::SK_CANCEL).expect("cancel AHOY");
    let b = o.tscc_pdu;
    let (so, skf, _, gi, _, _) = ahoy_f(&b);
    assert_eq!((so, skf, gi), (0, 0, 0));
    assert_eq!((tgt(&b), src(&b)), (B, A));
}

/// tier3.md 3.7 (4.2): a Service_Kind the site doesn't offer gets C_NACKD(Not_Supported). Status, short data and
/// packet data are offered (tier3.md 3.15, 3.16); UDT polling isn't; duplex calls are off by default
/// (`services.duplex`).
#[test]
fn unsupported_service_not_supported() {
    for sk in [spec::SK_POLL, spec::SK_DUPLEX_IND] {
        let mut r = two(false);
        let k = r.send(rand(0, 0, sk, B, A));
        let (ko, (_, reason, target, _)) = r.ack_to(k, 30, A).expect("answered");
        assert_eq!((reason, target), (spec::NOT_SUPPORTED, A), "Service_Kind {sk:#06b}");
        assert!(r.within_nwait(k, ko));
    }
}

/// tier3.md 3.7: a call to a gateway (PSTNI) isn't offered: Not_Supported.
#[test]
fn gateway_target_not_supported() {
    let mut r = two(false);
    let k = r.send(private(spec::PSTNI, A));
    let (_, (_, reason, _, _)) = r.ack_to(k, 30, A).unwrap();
    assert_eq!(reason, spec::NOT_SUPPORTED);
}

// ════ Settings against annex A and table 6.5 ════

fn accepts(f: impl FnOnce(&mut Cfg)) -> bool {
    let mut r = Rig::new(|_| {});
    let mut c = cfg_default();
    f(&mut c);
    let mut err = [0u8; 128];
    r.s.reconfigure(&c, &mut err)
}

/// Annex A, table A.1: TP_Timer 4-60 s, TNP_Timer 2-60 s, TV_Hangtime 1-60 s, T_MS-MS_TIMER 1-4094 s (4095 is
/// infinity), T_EMERG_TIMER 1-510 s (511 is infinity).
#[test]
fn timer_ranges_table_a1() {
    assert!(accepts(|c| c.tp_ms = 4000) && accepts(|c| c.tp_ms = 60_000));
    assert!(!accepts(|c| c.tp_ms = 3000) && !accepts(|c| c.tp_ms = 61_000));
    assert!(accepts(|c| c.tnp_ms = 2000) && accepts(|c| c.tnp_ms = 60_000));
    assert!(!accepts(|c| c.tnp_ms = 1000) && !accepts(|c| c.tnp_ms = 61_000));
    assert!(accepts(|c| c.hang_ms = 1000) && accepts(|c| c.hang_ms = 60_000));
    assert!(!accepts(|c| c.hang_ms = 0) && !accepts(|c| c.hang_ms = 61_000));
    assert!(accepts(|c| c.call_ms = 1000) && accepts(|c| c.call_ms = 4_094_000));
    assert!(!accepts(|c| c.call_ms = 0));
    assert!(accepts(|c| c.emergency_call_ms = 1000) && accepts(|c| c.emergency_call_ms = 510_000));
    assert!(!accepts(|c| c.emergency_call_ms = 0));
}

/// Annex A and tier3.md 3.8: the defaults are inside the standard's ranges.
#[test]
fn defaults_in_range_annex_a() {
    let c = cfg_default();
    assert!((4000..=60_000).contains(&c.tp_ms));
    assert!((2000..=60_000).contains(&c.tnp_ms));
    assert!((1000..=60_000).contains(&c.hang_ms));
    assert!((1000..=4_094_000).contains(&c.call_ms));
    assert!((1000..=510_000).contains(&c.emergency_call_ms));
    assert!(c.nrand_wait <= 15);
    assert!((1..=15).contains(&c.backoff_min) && (c.backoff_min..=15).contains(&c.backoff_max), "table 6.3: 0 reserved");
    assert!((1..=3).contains(&c.par));
    assert!(c.cc <= 15);
}

/// 6.3.2.2.1.4: PAR 00b is reserved; 01b, 10b, 11b are A, B, A+B.
#[test]
fn par_range() {
    assert!(!accepts(|c| c.par = 0));
    for par in 1..=3 {
        assert!(accepts(|c| c.par = par));
    }
}

/// Table 6.5 and annex A.2: DMRLA 1-3, 1-5, 1-8, 1-10 for the four models.
#[test]
fn dmrla_range_table_6_5() {
    for (model, max) in [(0u8, 3u8), (1, 5), (2, 8), (3, 10)] {
        let set = |bits: u8| {
            move |c: &mut Cfg| {
                c.model = model;
                c.net = 1;
                c.site = 1;
                c.area_bits = bits;
            }
        };
        assert!(accepts(set(1)), "model {model}: DMRLA 1");
        assert!(accepts(set(max)), "model {model}: DMRLA {max}");
        assert!(!accepts(set(0)), "model {model}: DMRLA 0");
        assert!(!accepts(set(max + 1)), "model {model}: DMRLA {}", max + 1);
    }
}

/// Table 6.5: NET and SITE must fit the model.
#[test]
fn net_site_widths_table_6_5() {
    for (model, nb, sb) in [(0u8, 9u32, 3u32), (1, 7, 5), (2, 4, 8), (3, 2, 10)] {
        let set = |net: u32, site: u32| {
            move |c: &mut Cfg| {
                c.model = model;
                c.net = net as u16;
                c.site = site as u16;
                c.area_bits = 1;
            }
        };
        assert!(accepts(set((1 << nb) - 1, (1 << sb) - 1)), "model {model}: the top values");
        assert!(!accepts(set(1 << nb, 1)), "model {model}: NET too wide");
        assert!(!accepts(set(1, 1 << sb)), "model {model}: SITE too wide");
    }
}

/// Table 6.5 gives 8, 32, 256 and 1024 sites per network: SITE 0 is one of them.
#[test]
fn site_zero_allowed() {
    assert!(accepts(|c| c.site = 0));
}

/// 361-1 9.1.5 (CC is 4 bits) and 6.7.1.3 (channel numbers 1-4094 are logical channels; 4095 means absolute
/// frequencies in an appended block).
#[test]
fn cc_and_channel_ranges() {
    assert!(accepts(|c| c.cc = 0) && accepts(|c| c.cc = 15));
    assert!(!accepts(|c| c.cc = 16));
    assert!(accepts(|c| c.carriers[0].chan = 4094));
    assert!(!accepts(|c| c.carriers[0].chan = 0));
    assert!(!accepts(|c| c.carriers[0].chan = 4095));
}

/// A standard radio's registration with one talkgroup, its bytes as captured from a RadioFoundry radio on the
/// bench (2026-11-13): 9F 00 02 4E 000001 2F63CA. Service_Options 0000001b (register), TRGT_ADR_CNTS 01b, the
/// talkgroup in the target (6.4.4.1, table 6.9). Accepted, and the talkgroup is the record's subscription.
#[test]
fn registration_with_talkgroup_captured() {
    let mut r = Rig::new(|_| {});
    let pdu = [0x9F, 0x00, 0x02, 0x4E, 0x00, 0x00, 0x01, 0x2F, 0x63, 0xCA];
    let k = r.send(pdu);
    let (_, (_, reason, _, _)) = r.ack_to(k, 30, 3105738).expect("answered");
    let rec = r.s.record(3105738).expect("registered");
    assert_eq!((reason, rec.sub_tg), (0x62, 1), "reason {reason:#04x}, subscription {}", rec.sub_tg);
}
