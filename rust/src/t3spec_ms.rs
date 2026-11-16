// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! Spec conformance, the simulated MS (ETSI TS 102 361-4 clause 6.2 random access, 6.4 registration, 6.6 calls,
//! annex A constants). The MS hears hand-built TSCC blocks and its transmissions are read field by field.

use std::vec::Vec;

use crate::t3ms::{self, Ms};
use crate::t3spec_bits::{csbko, field, lb, spec, Bits};

const T0: i64 = 1_793_912_651_000;
const ID: u32 = 1001; // odd
const EVEN_ID: u32 = 1002;
const CALLER: u32 = 2002;
const TG: u32 = 9;

fn sc() -> u16 {
    spec::syscode(1, 1, 1, 3)
}

/// A C_ALOHA (table 7.19) with the given access controls.
fn aloha_with(mask: u8, addr: u32, sf: u8, nrand_wait: u8, backoff: u8) -> [u8; 10] {
    Bits::csbk(true, spec::C_ALOHA)
        .f(1, 0)
        .f(1, 0)
        .f(1, 0)
        .f(3, spec::VERSION_1_11_1 as u64)
        .f(1, 0)
        .f(1, 1)
        .f(5, mask as u64)
        .f(2, sf as u64)
        .f(4, nrand_wait as u64)
        .f(1, 1)
        .f(4, backoff as u64)
        .f(16, sc() as u64)
        .f(24, addr as u64)
        .done()
}

fn aloha() -> [u8; 10] {
    aloha_with(0, spec::ADRNULL, 0b00, 2, 3)
}

/// An outbound block that is neither an Aloha nor addressed to anyone here (a C_BCAST Gen_Site_Params).
fn filler() -> [u8; 10] {
    Bits::csbk(true, spec::C_BCAST).f(5, spec::GEN_SITE_PARAMS as u64).f(14, 0).f(1, 1).f(4, 3).f(16, sc() as u64).f(24, 0).done()
}

fn ahoy(skf: bool, sk: u8, target: u32, source: u32) -> [u8; 10] {
    Bits::csbk(true, spec::C_AHOY)
        .f(7, 0)
        .f(1, skf as u64)
        .f(1, 0)
        .f(1, 0)
        .f(2, 0)
        .f(4, sk as u64)
        .f(24, target as u64)
        .f(24, source as u64)
        .done()
}

fn ackd(reason: u8, target: u32, source: u32) -> [u8; 10] {
    Bits::csbk(true, spec::C_ACKD).f(7, 0).f(8, reason as u64).f(1, 0).f(24, target as u64).f(24, source as u64).done()
}

/// PV_GRANT (table 7.9) on channel 1, LCN 1.
fn pv_grant(target: u32, source: u32) -> [u8; 10] {
    Bits::csbk(true, spec::PV_GRANT).f(12, 1).f(1, 1).f(1, 0).f(1, 0).f(1, 0).f(24, target as u64).f(24, source as u64).done()
}

/// One MS on a one-carrier site (TSCC on channel 1 TS1), fed one TSCC frame at a time.
struct Rig {
    m: Ms,
    k: i64,
    /// (frame, block) for each TSCC transmission
    sent: Vec<(i64, [u8; 10])>,
}

impl Rig {
    fn new(id: u32, seed: u32) -> Rig {
        Rig { m: Ms::new(id, &[TG], &[1], seed), k: 0, sent: Vec::new() }
    }

    fn now(&self) -> i64 {
        T0 + 30 * self.k
    }

    /// One TDMA frame: the MS's inbound slot (TS1), then the outbound block and the AT bit for the next one.
    fn frame(&mut self, pdu: [u8; 10], at: bool) -> Option<[u8; 10]> {
        let k = self.k;
        let tx = self.m.tx(k, self.now());
        let out = tx.filter(|t| t.kind == t3ms::TX_PDU).map(|t| t.pdu);
        if let Some(b) = out {
            self.sent.push((k / 2, b));
        }
        self.m.rx(0, k, spec::DT_CSBK, &pdu, at);
        // TS2 of the TSCC carrier: nothing for an MS on the TSCC
        let _ = self.m.tx(k + 1, self.now() + 30);
        self.k += 2;
        out
    }

    fn frames(&mut self, n: usize, pdu: [u8; 10], at: bool) -> usize {
        (0..n).filter(|_| self.frame(pdu, at).is_some()).count()
    }

    /// Hears an Aloha, then is asked to do `what`.
    fn ready(id: u32, what: u8, arg: u32, flags: u8) -> Rig {
        let mut r = Rig::new(id, id.wrapping_mul(2654435761));
        r.frame(aloha(), false);
        let now = r.now();
        r.m.act(what, arg, 1000, flags, now);
        r
    }

    /// Runs until the MS sends a block, at most n frames.
    fn next_tx(&mut self, n: usize, pdu: [u8; 10]) -> Option<[u8; 10]> {
        (0..n).find_map(|_| self.frame(pdu, false))
    }
}

/// C_RAND fields (table 7.25): (Service_Options, Proxy, dep, Service_Kind, target, source).
fn rand_f(b: &[u8; 10]) -> (u8, u8, u8, u8, u32, u32) {
    let f = |s, w| field(b, s, w);
    (f(16, 7) as u8, f(23, 1) as u8, f(24, 4) as u8, f(28, 4) as u8, f(32, 24) as u32, f(56, 24) as u32)
}

fn is_rand(b: &[u8; 10]) -> bool {
    lb(b) && csbko(b) == spec::C_RAND && b[1] == 0
}

// ── access rules (6.2.1) ──

/// 6.2.1.1.1 and 6.2.1.3: the MS waits for a C_ALOHA before random access.
#[test]
fn waits_for_aloha() {
    let mut r = Rig::new(ID, 7);
    let now = r.now();
    r.m.act(t3ms::DO_REGISTER, 0, 0, 0, now);
    assert_eq!(r.frames(40, filler(), false), 0, "no access before an Aloha");
    assert!(r.next_tx(20, aloha()).is_some(), "access after one");
}

/// 6.2.1.1.3: no random access in a slot the AT bit withdrew.
#[test]
fn honors_at() {
    let mut r = Rig::new(ID, 7);
    r.frame(aloha(), true); // the AT bit after it withdraws the next frame
    let now = r.now();
    r.m.act(t3ms::DO_REGISTER, 0, 0, 0, now);
    assert_eq!(r.frames(60, filler(), true), 0, "every slot withdrawn");
    assert!(r.next_tx(20, filler()).is_some(), "access once AT is 0");
}

/// 6.2.1.1.1: Mask 24 with ADRNULL lets no MS access.
#[test]
fn mask_24_adrnull_bars_access() {
    let mut r = Rig::new(ID, 7);
    r.frame(aloha_with(24, spec::ADRNULL, 0, 2, 3), false);
    let now = r.now();
    r.m.act(t3ms::DO_GROUP, TG, 1000, 0, now);
    assert_eq!(r.frames(60, aloha_with(24, spec::ADRNULL, 0, 2, 3), false), 0);
}

/// 6.2.1.1.1: Mask 1 with an even address: only even MS IDs may access (non-emergency).
#[test]
fn mask_1_splits_population() {
    let a = aloha_with(1, 0x000000, 0, 2, 3);
    let mut odd = Rig::new(ID, 7);
    odd.frame(a, false);
    let now = odd.now();
    odd.m.act(t3ms::DO_GROUP, TG, 1000, 0, now);
    assert_eq!(odd.frames(60, a, false), 0, "odd ID barred");
    let mut even = Rig::new(EVEN_ID, 7);
    even.frame(a, false);
    let now = even.now();
    even.m.act(t3ms::DO_GROUP, TG, 1000, 0, now);
    assert!(even.next_tx(20, a).is_some(), "even ID may access");
}

/// 6.2.1.1.1: Mask 0: every MS may access.
#[test]
fn mask_0_lets_all_access() {
    for id in [ID, EVEN_ID] {
        let a = aloha_with(0, 0x123456, 0, 2, 3);
        let mut r = Rig::new(id, 7);
        r.frame(a, false);
        let now = r.now();
        r.m.act(t3ms::DO_GROUP, TG, 1000, 0, now);
        assert!(r.next_tx(20, a).is_some());
    }
}

/// 6.2.1.1.2, table 6.1: Service Function 11b invites registration only: no call request.
#[test]
fn service_function_registration_only() {
    let a = aloha_with(0, 0, 0b11, 2, 3);
    let mut r = Rig::new(ID, 7);
    r.frame(a, false);
    let now = r.now();
    r.m.act(t3ms::DO_GROUP, TG, 1000, 0, now);
    assert_eq!(r.frames(60, a, false), 0);
}

/// Table 6.1: Service Function 10b doesn't invite services that need a payload channel.
#[test]
fn service_function_no_payload() {
    let a = aloha_with(0, 0, 0b10, 2, 3);
    let mut r = Rig::new(ID, 7);
    r.frame(a, false);
    let now = r.now();
    r.m.act(t3ms::DO_PRIVATE, CALLER, 1000, 0, now);
    assert_eq!(r.frames(60, a, false), 0);
}

/// Table 6.1: registration is invited under every Service Function; 6.2.1.1.2: an emergency request ignores it.
#[test]
fn service_function_still_allows() {
    for sf in 0..4u8 {
        let a = aloha_with(0, 0, sf, 2, 3);
        let mut r = Rig::new(ID, 7);
        r.frame(a, false);
        let now = r.now();
        r.m.act(t3ms::DO_REGISTER, 0, 0, 0, now);
        assert!(r.next_tx(20, a).is_some(), "registration under SF {sf:02b}");
        let mut r = Rig::new(ID, 7);
        r.frame(a, false);
        let now = r.now();
        r.m.act(t3ms::DO_GROUP, TG, 1000, t3ms::F_EMERGENCY, now);
        assert!(r.next_tx(20, a).is_some(), "emergency under SF {sf:02b}");
    }
}

/// Table A.6 and 6.2.1.1.7: with no answer, NRand_NR (6) attempts for a normal request, then the MS gives up.
#[test]
fn attempts_nrand_nr() {
    let mut r = Rig::ready(ID, t3ms::DO_GROUP, TG, 0);
    let n = r.frames(2000, aloha(), false);
    assert_eq!(n as u32, spec::NRAND_NR);
    assert_eq!(r.m.state(), t3ms::IDLE);
}

/// Table A.6 and 6.2.1.1.7: NRand_NE (10) attempts for an emergency request.
#[test]
fn attempts_nrand_ne() {
    let mut r = Rig::ready(ID, t3ms::DO_GROUP, TG, t3ms::F_EMERGENCY);
    let n = r.frames(4000, aloha(), false);
    assert_eq!(n as u32, spec::NRAND_NE);
}

/// 6.2.1.1.5 to 6.2.1.1.7: after an access in frame f the MS waits NWait + 1 frames for the answer (table 6.2),
/// then picks a frame at random among the next Backoff frames (table 6.3): a retry falls in f + NWait + 2 ..
/// f + NWait + 1 + Backoff, and the choices spread over that window.
#[test]
fn retry_after_nwait_within_backoff() {
    for (nrw, bo) in [(0u8, 1u8), (2, 3), (6, 6), (14, 9), (15, 4)] {
        let nwait = spec::NWAIT[nrw as usize] as i64;
        let n = spec::BACKOFF[bo as usize] as i64;
        let a = aloha_with(0, 0, 0, nrw, bo);
        let mut gaps = Vec::new();
        for seed in 1..40u32 {
            let mut r = Rig::new(ID, seed.wrapping_mul(2654435761));
            r.frame(a, false);
            let now = r.now();
            r.m.act(t3ms::DO_REGISTER, 0, 0, 0, now);
            r.frames(3000, a, false);
            assert_eq!(r.sent.len() as u32, spec::NRAND_NR);
            for w in r.sent.windows(2) {
                gaps.push(w[1].0 - w[0].0);
            }
        }
        let (lo, hi) = (nwait + 2, nwait + 1 + n);
        assert!(gaps.iter().all(|g| (lo..=hi).contains(g)), "NRand_Wait {nrw}, Backoff {bo}: gaps {gaps:?}, want {lo}..={hi}");
        if n > 1 {
            assert!(gaps.iter().any(|&g| g != gaps[0]), "random spread");
        }
    }
}

/// 6.2.1.1.5: an answer in the last frame of the window (frame f + NWait + 1) still counts; no retry.
#[test]
fn answer_late_in_window() {
    let nrw = 4u8;
    let a = aloha_with(0, 0, 0, nrw, 3);
    let mut r = Rig::new(ID, 11);
    r.frame(a, false);
    let now = r.now();
    r.m.act(t3ms::DO_REGISTER, 0, 0, 0, now);
    let f0 = loop {
        if r.frame(a, false).is_some() {
            break r.k / 2 - 1;
        }
    };
    while r.k / 2 < f0 + spec::NWAIT[nrw as usize] as i64 + 1 {
        r.frame(a, false);
    }
    assert!(r.frame(ackd(spec::REG_ACCEPTED, ID, spec::REGI), false).is_none());
    assert_eq!(r.frames(200, a, false), 0, "no retry after the answer");
    assert_eq!(r.m.status().registered, 1);
}

// ── request PDUs (7.1.1.2.1, tables 6.9, 6.38, 7.52) ──

/// Table 6.9: registration to REG_ADDR: Reg_Dereg 1, TRGT_ADR_CNTS 00b, target 0x00 + the Aloha's C_SYScode.
#[test]
fn registration_request_fields() {
    let mut r = Rig::ready(ID, t3ms::DO_REGISTER, 0, 0);
    let b = r.next_tx(20, aloha()).unwrap();
    assert!(is_rand(&b));
    let (so, proxy, dep, sk, target, source) = rand_f(&b);
    assert_eq!(so & 1, 1, "Reg_Dereg: register");
    assert_eq!(so & 0b100_0000, 0, "reserved");
    assert_eq!(proxy, 0);
    assert_eq!(dep >> 2, 0b00, "TRGT_ADR_CNTS: REG_ADDR");
    assert_eq!(sk, spec::SK_REG);
    assert_eq!(target, sc() as u32, "REG_ADDR");
    assert_eq!(target >> 16, 0);
    assert_eq!(source, ID);
}

/// Table 6.9: registration with a talkgroup subscription: TRGT_ADR_CNTS 01b, the talkgroup as target.
#[test]
fn registration_with_tg_fields() {
    let mut r = Rig::ready(ID, t3ms::DO_REGISTER, TG, 0);
    let (so, _, dep, sk, target, source) = rand_f(&r.next_tx(20, aloha()).unwrap());
    assert_eq!((so & 1, dep >> 2, sk, target, source), (1, 0b01, spec::SK_REG, TG, ID));
}

/// 6.4.6: de-registration: Reg_Dereg 0 to REG_ADDR.
#[test]
fn deregistration_fields() {
    let mut r = Rig::ready(ID, t3ms::DO_DEREGISTER, 0, 0);
    let (so, _, dep, sk, target, source) = rand_f(&r.next_tx(20, aloha()).unwrap());
    assert_eq!((so & 1, dep >> 2, sk, target, source), (0, 0b00, spec::SK_REG, sc() as u32, ID));
}

/// 6.6.2.2.1, tables 7.25, 7.52: a talkgroup call request: Service_Kind 0001b, target the talkgroup; EMERG is
/// Service_Options bit 6.
#[test]
fn group_request_fields() {
    let mut r = Rig::ready(ID, t3ms::DO_GROUP, TG, 0);
    let (so, _, _, sk, target, source) = rand_f(&r.next_tx(20, aloha()).unwrap());
    assert_eq!((so & 0x40, sk, target, source), (0, spec::SK_TG_VOICE, TG, ID));
    let mut r = Rig::ready(ID, t3ms::DO_GROUP, TG, t3ms::F_EMERGENCY);
    let (so, _, _, sk, _, _) = rand_f(&r.next_tx(20, aloha()).unwrap());
    assert_eq!((so & 0x40, sk), (0x40, spec::SK_TG_VOICE));
}

/// Table 7.49: an individual call request: Service_Kind 0000b, target the called MS.
#[test]
fn private_request_fields() {
    let mut r = Rig::ready(ID, t3ms::DO_PRIVATE, CALLER, 0);
    let (_, _, _, sk, target, source) = rand_f(&r.next_tx(20, aloha()).unwrap());
    assert_eq!((sk, target, source), (spec::SK_IND_VOICE, CALLER, ID));
}

/// 6.6.1.2, table 7.49: cancel: Service_Kind 1111b, target the called MS.
#[test]
fn cancel_request_fields() {
    let mut r = Rig::ready(ID, t3ms::DO_PRIVATE, CALLER, 0);
    r.next_tx(20, aloha()).unwrap();
    r.frame(ackd(spec::WAIT, ID, CALLER), false);
    let now = r.now();
    r.m.act(t3ms::DO_CANCEL, CALLER, 0, 0, now);
    let b = r.next_tx(40, aloha()).expect("cancel sent");
    let (_, _, _, sk, target, source) = rand_f(&b);
    assert_eq!((sk, target, source), (spec::SK_CANCEL, CALLER, ID));
}

// ── answering the TSCC (6.6.2.2.6, table 7.27) ──

/// C_ACKU fields (table 7.27): (Response_Info, Reason, reserved, target, source).
fn acku_f(b: &[u8; 10]) -> (u8, u8, u8, u32, u32) {
    let f = |s, w| field(b, s, w);
    (f(16, 7) as u8, f(23, 8) as u8, f(31, 1) as u8, f(32, 24) as u32, f(56, 24) as u32)
}

fn answered(answer: u8, skf: bool) -> (Rig, [u8; 10]) {
    let mut r = Rig::new(ID, 5);
    r.m.set_answer(answer, 2000);
    r.frame(aloha(), false);
    r.frame(ahoy(skf, spec::SK_IND_VOICE, ID, CALLER), true);
    let b = r.frame(aloha(), false).expect("the answer goes in the next frame (6.2.1.1.3)");
    (r, b)
}

/// 6.6.2.2.6, table 7.27: accepting a C_AHOY: C_ACKU(MS_Accepted), target = the C_AHOY's source, in the slot the
/// AT bit withdrew.
#[test]
fn ahoy_accepted() {
    let (_, b) = answered(t3ms::ANSWER_ACCEPT, false);
    assert!(lb(&b) && csbko(&b) == spec::C_ACKU && b[1] == 0);
    let (_, reason, reserved, target, source) = acku_f(&b);
    assert_eq!((reason, reserved, target, source), (spec::MS_ACCEPTED, 0, CALLER, ID));
}

/// 7.2.8: every reason an MS sends has d = 0 (MS to TS); a refusal is a C_NACKU (tt 00).
#[test]
fn ahoy_refusals_direction() {
    for ans in [t3ms::ANSWER_ACCEPT, t3ms::ANSWER_BUSY, t3ms::ANSWER_REFUSE, t3ms::ANSWER_FOACSU] {
        let (_, b) = answered(ans, ans == t3ms::ANSWER_FOACSU);
        let (_, reason, _, target, _) = acku_f(&b);
        assert_eq!(reason & 0b0010_0000, 0, "answer {ans}: reason {reason:#04x} has d = 0");
        assert_eq!(target, CALLER);
        if ans == t3ms::ANSWER_BUSY || ans == t3ms::ANSWER_REFUSE {
            assert_eq!(reason >> 6, 0b00, "a C_NACKU");
        }
    }
}

/// 6.6.1.4, 6.6.2.1.3 a) 4) and table 6.38: FOACSU: C_ACKU(MS_Alerting), then a Call Answer C_RAND (Service_Kind
/// 1001b, target the caller, ACCEPT 0).
#[test]
fn foacsu_alerting_then_call_answer() {
    let (mut r, b) = answered(t3ms::ANSWER_FOACSU, true);
    assert_eq!(acku_f(&b).1, spec::MS_ALERTING);
    let ans = r.next_tx(200, aloha()).expect("Call Answer");
    let (_, _, dep, sk, target, source) = rand_f(&ans);
    assert_eq!((sk, target, source), (spec::SK_CALL_ANSWER, CALLER, ID));
    assert_eq!((dep >> 1) & 1, 0, "ACCEPT");
}

// ── payload channel (6.6.2.3.2) ──

/// Runs an individual call this MS made: granted, talks 300 ms, then what it sends on the payload channel.
fn call_end() -> Vec<[u8; 10]> {
    let mut r = Rig::new(ID, 7);
    r.frame(aloha(), false);
    let now = r.now();
    r.m.act(t3ms::DO_PRIVATE, CALLER, 300, 0, now);
    let _ = r.next_tx(20, aloha()).unwrap();
    r.frame(pv_grant(CALLER, ID), false);
    assert_eq!(r.m.state(), t3ms::PAYLOAD);
    let mut out = Vec::new();
    for _ in 0..200 {
        let k = r.k;
        for kk in [k, k + 1] {
            if let Some(t) = r.m.tx(kk, T0 + 30 * kk) {
                if t.kind == t3ms::TX_PDU {
                    out.push(t.pdu);
                }
            }
        }
        r.k += 2;
    }
    out
}

/// Table A.6: N_Maint (4) P_MAINT(DISCON) blocks at the end of an individual call (6.6.2.3.2.5).
#[test]
fn p_maint_count() {
    let m: Vec<_> = call_end().into_iter().filter(|b| csbko(b) == spec::P_MAINT).collect();
    assert_eq!(m.len(), 4);
    for b in &m {
        assert!(lb(b));
        assert_eq!(b[1], 0);
        assert_eq!(field(b, 16, 12), 0, "reserved");
        assert_eq!(field(b, 28, 3), 0, "Maint_Kind DISCON");
        assert_eq!(field(b, 56, 24) as u32, ID, "source the MS");
    }
}

/// Table 7.32: P_MAINT's target address is TSI.
#[test]
fn p_maint_target_tsi() {
    let m: Vec<_> = call_end().into_iter().filter(|b| csbko(b) == spec::P_MAINT).collect();
    assert!(!m.is_empty());
    for b in &m {
        assert_eq!(field(b, 32, 24) as u32, spec::TSI);
    }
}
