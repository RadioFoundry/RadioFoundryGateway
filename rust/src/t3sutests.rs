// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! Scenario tests for the supplementary services (docs/spec/fw/tier3.md 3.12): stun and revive without
//! authentication (ETSI TS 102 361-4 6.4.9.1), the Ambient Listening Service (6.6.2.6) and call diversion
//! (6.6.7), on the slot-exact air.

use std::boxed::Box;
use std::vec::Vec;

use crate::t3air::Air;
use crate::t3ms::{self, Ms, TX_VOICE};
use crate::t3pdu::*;
use crate::t3site::*;
use crate::t3udt::*;

const T0: i64 = 1_793_912_651_000;
const TG: u32 = 9;

fn site_with(f: impl FnOnce(&mut Cfg)) -> Box<Site> {
    let mut c = cfg_default();
    f(&mut c);
    new_site(&c)
}

fn ms(id: u32) -> Ms {
    Ms::new(id, &[TG], &[1, 2], id.wrapping_mul(2654435761))
}

#[derive(Default)]
struct Log(Vec<Event>);

impl Log {
    fn f(&mut self) -> impl FnMut(&Event) + '_ {
        |e| self.0.push(*e)
    }
    /// The ACT_SUPP results so far: (op, src, dst, reason).
    fn supp(&self) -> Vec<(u8, u32, u32, u8)> {
        let act = |e: &&Event| e.r#type == EV_ACTIVITY && e.kind == ACT_SUPP;
        self.0.iter().filter(act).map(|e| (e.op, e.src, e.dst, e.reason)).collect()
    }
}

fn register_all<const N: usize>(air: &mut Air<N>, log: &mut Log) {
    let now = air.now();
    for (i, m) in air.ms.iter_mut().enumerate() {
        m.act(t3ms::DO_REGISTER, 0, 0, 0, now + 100 * i as i64);
    }
    assert!(air.until(20_000, &mut log.f(), |a| a.ms.iter().all(|m| m.status().registered != 0)), "registration");
}

fn ends(m: &Ms) -> u32 {
    m.status().calls_done + m.status().calls_failed
}

fn done<const N: usize>(i: usize, before: u32) -> impl FnMut(&Air<N>) -> bool {
    move |a| a.ms[i].state() == t3ms::IDLE && ends(&a.ms[i]) > before
}

/// Every TSCC block the site sends while running ms milliseconds: (dt, the first 10 octets).
fn tscc_out<const N: usize>(air: &mut Air<N>, log: &mut Log, ms: i64) -> Vec<(u8, [u8; 10])> {
    let mut v = Vec::new();
    let end = air.now() + ms;
    while air.now() < end {
        air.step(&mut log.f());
        if (air.k - 1) & 1 == 0 {
            v.push((air.last.tscc_dt, air.last.tscc_pdu));
        }
    }
    v
}

fn csbks(v: &[(u8, [u8; 10])]) -> impl Iterator<Item = &[u8; 10]> + '_ {
    v.iter().filter(|(dt, _)| *dt == DT_CSBK).map(|(_, p)| p)
}

/// A C_RAND straight into the site's TSCC (a request the simulated MS wouldn't make), then the TSCC's output.
fn inject<const N: usize>(air: &mut Air<N>, log: &mut Log, r: Rand) -> Vec<(u8, [u8; 10])> {
    let ch = air.site.tscc_ch();
    air.site.rx_pdu(ch, DT_CSBK, &r.encode());
    tscc_out(air, log, 1500)
}

fn acks(v: &[(u8, [u8; 10])], target: u32) -> Vec<Ack> {
    csbks(v).filter_map(Ack::decode).filter(|a| a.target == target).collect()
}

// ── stun and revive (6.4.9.1) ──

#[test]
fn stun_and_revive_without_authentication() {
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001), ms(1002)], T0);
    register_all(&mut air, &mut log);
    assert!(air.site.command("stun", 1001, 0));
    let out = tscc_out(&mut air, &mut log, 2000);
    // table 6.19: SO mirror 0, Service_Kind_Flag 0 (stun), ALS 0, G/I 0, no blocks, 1101b, from STUNI
    let a = csbks(&out).find_map(Ahoy::decode).expect("C_AHOY from STUNI");
    let want = Ahoy { service_kind: SK_SUPPL, target: 1001, source: STUNI, ..Default::default() };
    assert_eq!(a, want);
    assert!(air.ms[0].stunned() && air.site.stunned(1001));
    assert_eq!(log.supp(), [(SUPP_STUN, 1001, STUNI, R_MESSAGE_ACCEPTED)]);
    // a stunned MS's request is refused (Perm_User_Refused); the simulated MS doesn't even ask
    let r = Rand { service_kind: SK_IND_VOICE, target: 1002, source: 1001, ..Default::default() };
    let out = inject(&mut air, &mut log, r);
    assert!(acks(&out, 1001).iter().any(|a| a.op == OP_ACKD && a.reason == R_PERM_USER_REFUSED));
    let (before, now) = (ends(&air.ms[0]), air.now());
    air.ms[0].act(t3ms::DO_PRIVATE, 1002, 500, 0, now);
    air.run(300, &mut log.f());
    assert_eq!(ends(&air.ms[0]), before + 1);
    assert_eq!(air.ms[0].status().grants, 0);
    // nor receives: a call to it is refused at once (MSaway_Refused)
    let (before, now) = (ends(&air.ms[1]), air.now());
    air.ms[1].act(t3ms::DO_PRIVATE, 1001, 500, 0, now);
    assert!(air.until(5000, &mut log.f(), done(1, before)));
    assert_eq!(air.ms[1].status().last_reason, R_MSAWAY);
    // registration stays
    let now = air.now();
    air.ms[0].act(t3ms::DO_REGISTER, 0, 0, 0, now);
    assert!(air.until(5000, &mut log.f(), |a| a.ms[0].status().last_reason == R_REG_ACCEPTED));
    // revived: Service_Kind_Flag 1, and calls go again
    assert!(air.site.command("revive", 1001, 0));
    let out = tscc_out(&mut air, &mut log, 2000);
    let a = csbks(&out).filter_map(Ahoy::decode).find(|a| a.source == STUNI).unwrap();
    assert!(a.service_kind_flag && a.service_kind == SK_SUPPL && a.service_options == 0);
    assert!(!air.ms[0].stunned() && !air.site.stunned(1001));
    assert_eq!(log.supp()[1], (SUPP_REVIVE, 1001, STUNI, R_MESSAGE_ACCEPTED));
    let (before, now) = (ends(&air.ms[1]), air.now());
    air.ms[1].act(t3ms::DO_PRIVATE, 1001, 500, 0, now);
    assert!(air.until(10_000, &mut log.f(), done(1, before)));
    assert!(air.ms[0].status().grants > 0);
}

#[test]
fn stun_refused_unanswered_and_kill() {
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001)], T0);
    register_all(&mut air, &mut log);
    // an MS without stun answers MSNot_Supported: not stunned
    air.ms[0].refuse_supp(true, false);
    assert!(air.site.command("stun", 1001, 0));
    air.run(2000, &mut log.f());
    assert_eq!(log.supp(), [(SUPP_STUN, 1001, STUNI, R_MS_NOT_SUPPORTED)]);
    assert!(!air.site.stunned(1001) && !air.ms[0].stunned());
    // no answer: ahoy_tries C_AHOYs, then MSaway
    assert!(air.site.command("stun", 1999, 0));
    let out = tscc_out(&mut air, &mut log, 6000);
    let n = csbks(&out).filter_map(Ahoy::decode).filter(|a| a.target == 1999).count();
    assert_eq!(n, cfg_default().ahoy_tries as usize);
    assert_eq!(log.supp()[1], (SUPP_STUN, 1999, STUNI, R_MSAWAY));
    assert!(!air.site.stunned(1999));
    // kill needs authentication (6.4.10): not offered; nor stun of a non-MS address
    assert!(!air.site.command("kill", 1001, 0));
    assert!(!air.site.command("stun", TSI, 0) && !air.site.command("stun", 0, 0));
}

// ── ambient listening (6.6.2.6) ──

#[test]
fn ambient_listening_call() {
    let mut s = site_with(|c| c.als = 1);
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001), ms(1002)], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].act(t3ms::DO_PRIVATE, 1002, 0, t3ms::F_ALS, now);
    let out = tscc_out(&mut air, &mut log, 1500);
    // table 6.46: SO mirror 0, Service_Kind_Flag 0, ALS_SERV 1, individual voice, from the caller
    let a = csbks(&out).filter_map(Ahoy::decode).find(|a| a.target == 1002).expect("ALS radio check");
    let want = Ahoy { als: true, service_kind: SK_IND_VOICE, target: 1002, source: 1001, ..Default::default() };
    assert_eq!(a, want);
    // the called MS accepted (Message_Accepted), the grant came, and it transmits to the caller; the caller listens
    let mut caller_talked = false;
    let called_talks = |a: &Air<2>| {
        caller_talked |= a.inbound[0].is_some_and(|t| t.kind == TX_VOICE);
        a.inbound[1].is_some_and(|t| t.kind == TX_VOICE && t.src == 1002 && t.dst == 1001)
    };
    assert!(air.until(5000, &mut log.f(), called_talks));
    assert!(!caller_talked);
    assert_eq!(air.ms[1].status().grants, 1);
    // an MS without ALS: MSNot_Supported mirrored to the caller
    air.run(15_000, &mut log.f());
    air.ms[1].refuse_supp(false, true);
    let (before, now) = (ends(&air.ms[0]), air.now());
    air.ms[0].act(t3ms::DO_PRIVATE, 1002, 0, t3ms::F_ALS, now);
    assert!(air.until(10_000, &mut log.f(), done(0, before)));
    assert_eq!(air.ms[0].status().last_reason, R_MS_NOT_SUPPORTED);
}

#[test]
fn ambient_listening_not_offered() {
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001), ms(1002)], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].act(t3ms::DO_PRIVATE, 1002, 0, t3ms::F_ALS, now);
    assert!(air.until(5000, &mut log.f(), done(0, 0)));
    assert_eq!(air.ms[0].status().last_reason, R_NOT_SUPPORTED);
    assert_eq!(air.ms[1].status().ahoys, 0);
}

// ── call diversion (6.6.7) ──

#[test]
fn call_diversion_set_used_and_cleared() {
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001), ms(1002), ms(1003)], T0);
    register_all(&mut air, &mut log);
    // 1002 diverts voice and short data to 1003: C_AHOY from DIVERTI, the upload, C_ACKD from MSI
    let now = air.now();
    assert!(air.ms[1].divert(1003, false, DIV_VOICE | DIV_SD, now));
    let out = tscc_out(&mut air, &mut log, 3000);
    let a = csbks(&out).filter_map(Ahoy::decode).find(|a| a.source == DIVERTI).expect("C_AHOY from DIVERTI");
    assert_eq!((a.target, a.service_kind, a.appended), (1002, SK_DIVERT, 0));
    let k = acks(&out, 1002).into_iter().find(|a| a.op == OP_ACKD && a.source == MSI).expect("final C_ACKD");
    assert_eq!(k.reason, R_MESSAGE_ACCEPTED);
    assert_eq!(air.site.diversions(1002), ([1003, 0, 1003, 0], 0));
    assert!(log.supp().contains(&(SUPP_DIVERT, 1002, 1003, R_MESSAGE_ACCEPTED)));
    // 1001 calls 1002: a UDT header with the diverted address instead (SF 1, A 0, from MSI)
    let now = air.now();
    air.ms[0].act(t3ms::DO_PRIVATE, 1002, 500, 0, now);
    let out = tscc_out(&mut air, &mut log, 2000);
    let h = out.iter().filter(|(dt, _)| *dt == DT_DATA_HDR).find_map(|(_, p)| UdtHead::decode(p)).unwrap();
    assert_eq!((h.op, h.sf, h.response, h.group), (OP_UDTHD, true, false, false));
    assert_eq!((h.source, h.target, h.format, h.blocks), (MSI, 1001, FMT_ADDRESS, 1));
    assert_eq!(air.ms[0].diverted(), (MSI, 1003, 1));
    assert_eq!((air.ms[0].state(), air.ms[0].status().grants, air.ms[1].status().ahoys), (t3ms::IDLE, 0, 0));
    // short data too; a status isn't diverted
    let now = air.now();
    assert!(air.ms[0].send_sdm(1002, 0, FMT_ISO8, b"hi", now));
    air.ms[0].act(t3ms::DO_STATUS, 1002, 5, 0, now + 2000);
    assert!(air.until(8000, &mut log.f(), |a| a.ms[1].statuses().2 == 1));
    assert_eq!(air.ms[0].diverted(), (MSI, 1003, 2));
    assert!(air.ms[1].inbox().is_none());
    // 1002 clears its voice diversion (target TSI): calls reach it again
    let now = air.now();
    assert!(air.ms[1].divert_clear(false, DIV_VOICE, now));
    let out = tscc_out(&mut air, &mut log, 2000);
    assert!(acks(&out, 1002).iter().any(|a| a.source == TSI && a.reason == R_MESSAGE_ACCEPTED));
    assert_eq!(air.site.diversions(1002), ([0, 0, 1003, 0], 0));
    let (before, now) = (ends(&air.ms[0]), air.now());
    air.ms[0].act(t3ms::DO_PRIVATE, 1002, 300, 0, now);
    assert!(air.until(10_000, &mut log.f(), done(0, before)));
    assert_eq!(air.ms[1].status().grants, 1);
    // 1003 cancels every diversion to it (target DIVERTI)
    let now = air.now();
    assert!(air.ms[2].divert_clear(true, DIV_KINDS, now));
    let out = tscc_out(&mut air, &mut log, 2000);
    assert!(acks(&out, 1003).iter().any(|a| a.source == DIVERTI && a.reason == R_MESSAGE_ACCEPTED));
    assert_eq!(air.site.diversions(1002), ([0; 4], 0));
    // to a talkgroup (GPI): only 1002 may clear it
    let now = air.now();
    assert!(air.ms[1].divert(TG, true, DIV_VOICE, now));
    assert!(air.until(4000, &mut log.f(), |a| a.site.diversions(1002) == ([TG, 0, 0, 0], 1)));
    air.run(1000, &mut log.f());
    let now = air.now();
    air.ms[0].act(t3ms::DO_PRIVATE, 1002, 500, 0, now);
    assert!(air.until(4000, &mut log.f(), |a| a.ms[0].diverted().2 == 3));
    assert_eq!(air.ms[0].diverted(), (GPI, TG, 3));
    let now = air.now();
    assert!(air.ms[2].divert_clear(true, DIV_KINDS, now));
    air.run(2000, &mut log.f());
    assert_eq!(air.site.diversions(1002), ([TG, 0, 0, 0], 1));
    let now = air.now();
    assert!(air.ms[1].divert_clear(false, DIV_KINDS, now));
    assert!(air.until(4000, &mut log.f(), |a| a.site.diversions(1002) == ([0; 4], 0)));
}

#[test]
fn call_diversion_refusals() {
    // not offered: Not_Supported to the C_RAND
    {
        let mut s = site_with(|c| c.divert = 0);
        let mut log = Log::default();
        let mut air = Air::new(&mut s, [ms(1001), ms(1002)], T0);
        register_all(&mut air, &mut log);
        let now = air.now();
        assert!(air.ms[1].divert(1001, false, DIV_VOICE, now));
        assert!(air.until(5000, &mut log.f(), done(1, 0)));
        assert_eq!(air.ms[1].status().last_reason, R_NOT_SUPPORTED);
    }

    let mut s = site_with(|_| {});
    let mut p = Policy::default_open();
    p.tg = IdSet::of(&[(1, 99)]);
    s.policy_set(&p);
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001), ms(1002)], T0);
    register_all(&mut air, &mut log);
    // to a PSTN gateway: no such gateway here (Transient_Sys_Refused, from the C_RAND's target)
    let r = Rand {
        service_options: DIV_ON | DIV_VOICE,
        service_kind: SK_DIVERT,
        target: PSTNI,
        source: 1002,
        ..Default::default()
    };
    let out = inject(&mut air, &mut log, r);
    let k = acks(&out, 1002).into_iter().find(|a| a.op == OP_ACKD).unwrap();
    assert_eq!((k.source, k.reason), (PSTNI, R_TRANSIENT_SYS_REFUSED));
    // to itself, or a talkgroup the site doesn't carry: refused after the upload (table 6.60)
    for (to, group) in [(1002, false), (500, true)] {
        let (before, now) = (ends(&air.ms[1]), air.now());
        assert!(air.ms[1].divert(to, group, DIV_VOICE, now));
        assert!(air.until(5000, &mut log.f(), done(1, before)));
        assert_eq!(air.ms[1].status().last_reason, R_TRANSIENT_SYS_REFUSED);
    }
    assert_eq!(air.site.diversions(1002), ([0; 4], 0));
    // a UDT poll of an MS that diverted its short data: Div_Cause_Fail (6.6.5.1.1)
    let now = air.now();
    assert!(air.ms[1].divert(1001, false, DIV_SD, now));
    assert!(air.until(5000, &mut log.f(), |a| a.site.diversions(1002).0[2] == 1001));
    air.run(1000, &mut log.f());
    let r = Rand { service_kind: SK_UDT_POLL, target: 1002, source: 1001, ..Default::default() };
    let out = inject(&mut air, &mut log, r);
    assert!(acks(&out, 1001).iter().any(|a| a.reason == R_DIV_CAUSE_FAIL));
}
