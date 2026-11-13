// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! MS authentication (ETSI TS 102 361-4 6.4.4.1.5, 6.4.8; docs/spec/fw/tier3.md 3.10): the site and simulated MSs
//! on the slot-exact air, step by step as the clauses read.

use std::boxed::Box;
use std::vec::Vec;

use crate::t3air::Air;
use crate::t3auth::{response, R_AUTH_RESPONSE, R_MS_NOT_SUPPORTED};
use crate::t3ms::{self, Ms};
use crate::t3pdu::*;
use crate::t3site::*;

const T0: i64 = 1_794_480_000_000; // 2026-11-12
const K1: [u8; 16] = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16];
const K2: [u8; 16] = [0xA5; 16];

fn site(auth: u8) -> Box<Site> {
    let mut c = cfg_default();
    c.auth = auth;
    new_site(&c)
}

fn keyed(key: &[u8; 16], psn: Option<u32>) -> MsPolicy {
    MsPolicy {
        enabled: 1,
        private_ok: 1,
        wide_private: 1,
        emergency_ok: 1,
        has_key: 1,
        key: *key,
        has_psn: psn.is_some() as u8,
        psn: psn.unwrap_or(0),
        ..Default::default()
    }
}

fn ms(id: u32, key: Option<&[u8; 16]>, psn: Option<u32>) -> Ms {
    let mut m = Ms::new(id, &[9], &[1], id.wrapping_mul(2654435761));
    m.set_key(key, psn);
    m
}

/// What the run saw: the site's events and the TSCC's blocks (slot, block, AT).
#[derive(Default)]
struct Seen {
    ev: Vec<Event>,
    out: Vec<(i64, [u8; 10], bool)>,
}

impl Seen {
    fn ahoys(&self) -> Vec<Ahoy> {
        self.out.iter().filter_map(|o| Ahoy::decode(&o.1)).collect()
    }
    fn challenges(&self, ms: u32) -> Vec<Ahoy> {
        self.ahoys().into_iter().filter(|a| a.target == ms && a.service_kind == SK_REG).collect()
    }
    fn acks_to(&self, ms: u32) -> Vec<Ack> {
        self.out.iter().filter_map(|o| Ack::decode(&o.1)).filter(|a| a.op == OP_ACKD && a.target == ms).collect()
    }
    fn reg(&self, ms: u32, op: u8) -> usize {
        self.ev.iter().filter(|e| e.r#type == EV_ACTIVITY && e.kind == ACT_REG && e.src == ms && e.op == op).count()
    }
}

/// Steps until cond holds (true) or ms pass (false).
fn go<const N: usize>(air: &mut Air<N>, seen: &mut Seen, ms: i64, cond: impl Fn(&Air<N>) -> bool) -> bool {
    let end = air.now() + ms;
    while air.now() < end {
        air.step(&mut |e: &Event| seen.ev.push(*e));
        if air.last.tscc_dt != 0 {
            seen.out.push((air.k - 1, air.last.tscc_pdu, air.last.at != 0));
        }
        if cond(air) {
            return true;
        }
    }
    false
}

fn settled<const N: usize>(a: &Air<N>, i: usize) -> bool {
    a.ms[i].state() == t3ms::IDLE && a.ms[i].status().last_reason != 0
}

fn register<const N: usize>(air: &mut Air<N>, seen: &mut Seen, i: usize) -> bool {
    let now = air.now();
    air.ms[i].act(t3ms::DO_REGISTER, 0, 0, 0, now);
    go(air, seen, 2000, |a| a.ms[i].state() != t3ms::IDLE);
    go(air, seen, 15_000, |a| settled(a, i)) && air.ms[i].status().registered != 0
}

// ── the algorithm and the PDUs ──

#[test]
fn test_vectors_6_4_8() {
    // tables 6.15 and 6.16
    assert_eq!(response(0x7A17C0, &K1, None), 0xE8CA1D);
    assert_eq!(response(0x7A17C0, &K1, Some(0x010203)), 0xE9C81E);
}

#[test]
fn challenge_and_response_pdus() {
    // table 6.17: C_AHOY, Service_Options_Mirror 7, Service_Kind_Flag 0, ALS 0, G/I 0, Appended_Blocks 00,
    // Service_Kind 1110b, target the MS, source the challenge
    let a =
        Ahoy { service_options: SO_REG, service_kind: SK_REG, target: 1001, source: 0x7A17C0, ..Default::default() };
    let b = a.encode();
    assert_eq!(b[0] & 0x3F, OP_AHOY);
    assert_eq!(b[2], SO_REG << 1); // the mirror, then Service_Kind_Flag 0
    assert_eq!(b[3], SK_REG); // ALS, G/I, Appended_Blocks 0, Service_Kind
    assert_eq!(&b[4..10], &[0x00, 0x03, 0xE9, 0x7A, 0x17, 0xC0]);
    // table 6.18: C_ACKU, Response_Info 7, Reason 0100 1000b, reserved 0, target the response, source the MS
    let r = Ack { op: OP_ACKU, response_info: 0, reason: R_AUTH_RESPONSE, target: 0xE8CA1D, source: 1001 }.encode();
    assert_eq!(r[0] & 0x3F, OP_ACKU);
    assert_eq!((r[2], r[3]), (0x00, 0x48 << 1));
    assert_eq!(&r[4..10], &[0xE8, 0xCA, 0x1D, 0x00, 0x03, 0xE9]);
}

#[test]
fn settings_check() {
    let mut c = cfg_default();
    c.auth = AUTH_REQUIRED + 1;
    let mut err = [0u8; 80];
    let mut s: Box<Site> = new_site(&cfg_default());
    assert!(!s.reconfigure(&c, &mut err));
    assert!(std::str::from_utf8(&err).unwrap().starts_with("auth:"));
}

// ── registration (6.4.4.1.5, figure 6.20) ──

#[test]
fn off_sends_no_challenge() {
    let mut s = site(AUTH_OFF);
    s.set_ms_policy(1001, Some(&keyed(&K1, None)));
    let mut seen = Seen::default();
    let mut air = Air::new(&mut s, [ms(1001, Some(&K1), None)], T0);
    assert!(register(&mut air, &mut seen, 0));
    assert!(seen.ahoys().is_empty());
    assert_eq!(air.ms[0].challenges(), 0);
}

#[test]
fn registration_with_a_good_key() {
    let mut s = site(AUTH_REGISTRATION);
    s.set_ms_policy(1001, Some(&keyed(&K1, None)));
    let mut seen = Seen::default();
    let mut air = Air::new(&mut s, [ms(1001, Some(&K1), None)], T0);
    assert!(register(&mut air, &mut seen, 0));
    // "B": the C_AHOY answers the random access and challenges the MS; AT keeps the next inbound slot
    let ch = seen.challenges(1001);
    assert_eq!(ch.len(), 1);
    let a = ch[0];
    assert!(!a.service_kind_flag && !a.group && !a.als && a.appended == 0);
    assert_eq!(a.service_options, SO_REG); // 6.4.8.2: the C_RAND's Service_Options
    assert!(a.source <= crate::t3auth::CHALLENGE_MAX);
    assert!(seen.out.iter().any(|o| Ahoy::decode(&o.1).is_some_and(|x| x.target == 1001) && o.2), "AT");
    // "C": the MS's response; "D": the final C_ACKD(Reg_Accepted), no C_WACKD before it
    assert_eq!(air.ms[0].challenges(), 1);
    let acks = seen.acks_to(1001);
    assert_eq!(acks.len(), 1);
    assert_eq!((acks[0].reason, acks[0].source), (R_REG_ACCEPTED, REGI));
    assert!(air.site.record(1001).is_some());
    assert_eq!((seen.reg(1001, REG_AUTH_OK), seen.reg(1001, REG_REGISTER)), (1, 1));
}

#[test]
fn registration_with_a_psn() {
    // 6.4.8.0: the PSN is XORed onto the response; the TS applies the same
    let mut s = site(AUTH_REQUIRED);
    s.set_ms_policy(1001, Some(&keyed(&K1, Some(0x010203))));
    s.set_ms_policy(1002, Some(&keyed(&K2, Some(0x0A0B0C))));
    let mut seen = Seen::default();
    let mut air = Air::new(&mut s, [ms(1001, Some(&K1), Some(0x010203)), ms(1002, Some(&K2), None)], T0);
    assert!(register(&mut air, &mut seen, 0));
    // the site expects a PSN this MS doesn't apply: refused
    assert!(!register(&mut air, &mut seen, 1));
    assert_eq!(air.ms[1].status().last_reason, R_REG_DENIED);
}

#[test]
fn a_wrong_key_is_denied() {
    let mut s = site(AUTH_REGISTRATION);
    s.set_ms_policy(1001, Some(&keyed(&K1, None)));
    let mut seen = Seen::default();
    let mut air = Air::new(&mut s, [ms(1001, Some(&K2), None)], T0);
    assert!(!register(&mut air, &mut seen, 0));
    let st = air.ms[0].status();
    assert_eq!((st.registered, st.last_reason), (0, R_REG_DENIED));
    let acks = seen.acks_to(1001);
    assert_eq!((acks.len(), acks[0].reason, acks[0].source), (1, R_REG_DENIED, REGI));
    assert!(air.site.record(1001).is_none());
    assert_eq!((seen.reg(1001, REG_AUTH_FAIL), seen.reg(1001, REG_REGISTER)), (1, 0));
    // an earlier registration goes too: the MS registered before its key changed
    let mut s2 = site(AUTH_OFF);
    s2.set_ms_policy(1001, Some(&keyed(&K1, None)));
    let mut seen = Seen::default();
    let mut air = Air::new(&mut s2, [ms(1001, Some(&K2), None)], T0);
    assert!(register(&mut air, &mut seen, 0));
    let mut c = *air.site.cfg();
    c.auth = AUTH_REGISTRATION;
    assert!(air.site.reconfigure(&c, &mut [0u8; 80]));
    assert!(!register(&mut air, &mut seen, 0));
    assert!(air.site.record(1001).is_none());
    assert!(seen.ev.iter().any(|e| e.r#type == EV_RECORD && e.src == 1001 && e.why == 1));
}

#[test]
fn no_key_under_registration() {
    let mut s = site(AUTH_REGISTRATION);
    let mut seen = Seen::default();
    // 1001: no key at the site: registers as without authentication. 1002: a key at the site, none in the MS: it
    // answers C_NACKU(MSNot_Supported) and registers.
    s.set_ms_policy(1002, Some(&keyed(&K1, None)));
    let mut air = Air::new(&mut s, [ms(1001, None, None), ms(1002, None, None)], T0);
    assert!(register(&mut air, &mut seen, 0));
    assert!(seen.challenges(1001).is_empty());
    assert!(register(&mut air, &mut seen, 1));
    assert_eq!(seen.challenges(1002).len(), 1);
    assert_eq!((air.ms[1].challenges(), seen.reg(1002, REG_AUTH_NONE)), (1, 1));
}

#[test]
fn no_key_under_required() {
    let mut s = site(AUTH_REQUIRED);
    s.set_ms_policy(1002, Some(&keyed(&K1, None)));
    let mut seen = Seen::default();
    let mut air = Air::new(&mut s, [ms(1001, Some(&K1), None), ms(1002, None, None)], T0);
    // no key at the site: Reg_Denied at once, no challenge
    assert!(!register(&mut air, &mut seen, 0));
    assert!(seen.challenges(1001).is_empty());
    assert_eq!(air.ms[0].status().last_reason, R_REG_DENIED);
    // no key in the MS: C_NACKU(MSNot_Supported), then Reg_Denied
    assert!(!register(&mut air, &mut seen, 1));
    assert_eq!(air.ms[1].challenges(), 1);
    assert_eq!(air.ms[1].status().last_reason, R_REG_DENIED);
    assert_eq!((seen.reg(1001, REG_AUTH_FAIL), seen.reg(1002, REG_AUTH_FAIL)), (1, 1));
    assert!(air.site.record(1001).is_none() && air.site.record(1002).is_none());
}

#[test]
fn no_implicit_registration_past_authentication() {
    // Reg = 0 normally lets a call request register its MS (4.10.2); an MS to be challenged must register
    let mut s = site(AUTH_REQUIRED);
    let mut c = *s.cfg();
    c.reg_required = 0;
    assert!(s.reconfigure(&c, &mut [0u8; 80]));
    s.set_ms_policy(1001, Some(&keyed(&K1, None)));
    let mut seen = Seen::default();
    let mut air = Air::new(&mut s, [ms(1001, Some(&K1), None)], T0);
    let now = air.now();
    air.ms[0].act(t3ms::DO_GROUP, 9, 1000, 0, now);
    assert!(go(&mut air, &mut seen, 10_000, |a| settled(a, 0)));
    assert_eq!(air.ms[0].status().last_reason, R_MS_NOT_REGISTERED);
    assert!(air.site.record(1001).is_none());
    // registered through the challenge, it calls
    assert!(register(&mut air, &mut seen, 0));
    let now = air.now();
    air.ms[0].act(t3ms::DO_GROUP, 9, 1000, 0, now);
    assert!(go(&mut air, &mut seen, 10_000, |a| a.ms[0].status().grants > 0));
}

#[test]
fn a_lost_challenge_is_repeated() {
    let mut s = site(AUTH_REGISTRATION);
    s.set_ms_policy(1001, Some(&keyed(&K1, None)));
    let mut seen = Seen::default();
    let mut air = Air::new(&mut s, [ms(1001, Some(&K1), None)], T0);
    air.lose_out = (OP_AHOY, 1);
    assert!(register(&mut air, &mut seen, 0));
    // the repeat (the site's timer, or the MS's repeated request) carries the same challenge
    let ch = seen.challenges(1001);
    assert!(ch.len() >= 2);
    assert!(ch.iter().all(|a| a.source == ch[0].source));
    assert_eq!(air.ms[0].challenges(), 1);
}

#[test]
fn a_lost_answer() {
    let mut s = site(AUTH_REGISTRATION);
    s.set_ms_policy(1001, Some(&keyed(&K1, None)));
    let mut seen = Seen::default();
    let mut air = Air::new(&mut s, [ms(1001, Some(&K1), None)], T0);
    air.lose_in = (OP_ACKU, 1);
    let now = air.now();
    air.ms[0].act(t3ms::DO_REGISTER, 0, 0, 0, now);
    // after its answer the MS waits (TNP_Timer) and doesn't repeat its request; the site repeats the challenge
    // after ahoy_every_s, the MS answers again
    assert!(go(&mut air, &mut seen, 600, |a| a.ms[0].challenges() == 1));
    assert!(go(&mut air, &mut seen, 400, |a| a.ms[0].state() == t3ms::WAITING));
    assert!(go(&mut air, &mut seen, 15_000, |a| settled(a, 0)));
    assert_eq!(air.ms[0].status().registered, 1);
    assert_eq!(air.ms[0].challenges(), 2);
    let ch = seen.challenges(1001);
    assert_eq!(ch.len(), 2);
    let gap = seen.out.iter().filter(|o| Ahoy::decode(&o.1).is_some()).map(|o| o.0).collect::<Vec<_>>();
    assert!((gap[1] - gap[0]) * 30 >= 900, "repeated about ahoy_every_s later: {gap:?}");
}

#[test]
fn no_answer_gives_up_silently() {
    let mut s = site(AUTH_REGISTRATION);
    s.set_ms_policy(1001, Some(&keyed(&K1, None)));
    let mut seen = Seen::default();
    let mut air = Air::new(&mut s, [ms(1001, Some(&K1), None)], T0);
    air.lose_out = (OP_AHOY, 1000);
    let now = air.now();
    air.ms[0].act(t3ms::DO_REGISTER, 0, 0, 0, now);
    go(&mut air, &mut seen, 30_000, |_| false);
    // the MS's random access gives up; the site sends no final answer
    assert_eq!(air.ms[0].status().registered, 0);
    assert!(seen.acks_to(1001).is_empty());
    assert!(air.site.record(1001).is_none());
    assert!(seen.reg(1001, REG_AUTH_NONE) >= 1);
    assert_eq!(air.ms[0].challenges(), 0);
}

#[test]
fn two_registrations_interleaved() {
    let mut s = site(AUTH_REQUIRED);
    s.set_ms_policy(1001, Some(&keyed(&K1, None)));
    s.set_ms_policy(1002, Some(&keyed(&K2, Some(0x123456))));
    let mut seen = Seen::default();
    let mut air = Air::new(&mut s, [ms(1001, Some(&K1), None), ms(1002, Some(&K2), Some(0x123456))], T0);
    // 1001's first answer is lost: its challenge stays open while 1002 registers
    air.lose_in = (OP_ACKU, 1);
    let now = air.now();
    air.ms[0].act(t3ms::DO_REGISTER, 0, 0, 0, now);
    air.ms[1].act(t3ms::DO_REGISTER, 0, 0, 0, now + 300);
    assert!(go(&mut air, &mut seen, 20_000, |a| settled(a, 0) && settled(a, 1)));
    assert_eq!((air.ms[0].status().registered, air.ms[1].status().registered), (1, 1));
    let order: Vec<u32> = seen.challenges(1001).iter().chain(seen.challenges(1002).iter()).map(|a| a.target).collect();
    assert_eq!(order.len(), 3);
    // in time: 1001's challenge, 1002's, then 1001's repeat
    let targets: Vec<u32> = seen.ahoys().iter().filter(|a| a.service_kind == SK_REG).map(|a| a.target).collect();
    assert_eq!(targets, [1001, 1002, 1001]);
    // different challenges
    assert_ne!(seen.challenges(1001)[0].source, seen.challenges(1002)[0].source);
}

// ── the authentication poll (6.4.8.2) ──

#[test]
fn poll_pass_and_fail() {
    let mut s = site(AUTH_REGISTRATION);
    s.set_ms_policy(1001, Some(&keyed(&K1, None)));
    let mut seen = Seen::default();
    let mut air = Air::new(&mut s, [ms(1001, Some(&K1), None), ms(1002, None, None)], T0);
    assert!(register(&mut air, &mut seen, 0));
    assert!(register(&mut air, &mut seen, 1));
    // not for an MS without a key, or one not registered here
    assert!(!air.site.command("auth", 1002, 0));
    assert!(!air.site.command("auth", 1003, 0));
    // pass: Service_Options_Mirror 0 (table 7.61); the record stays
    let before = seen.ahoys().len();
    assert!(air.site.command("auth", 1001, 0));
    assert!(go(&mut air, &mut seen, 3000, |a| a.ms[0].challenges() == 2));
    go(&mut air, &mut seen, 300, |_| false);
    let polls = &seen.ahoys()[before..];
    assert_eq!((polls.len(), polls[0].service_options, polls[0].service_kind), (1, 0, SK_REG));
    assert_eq!(seen.reg(1001, REG_AUTH_OK), 2);
    assert!(air.site.record(1001).is_some());
    assert_eq!(air.ms[0].state(), t3ms::IDLE);
    // fail: the key changed at the site; the registration is removed, no final answer on air
    let acks = seen.acks_to(1001).len();
    air.site.set_ms_policy(1001, Some(&keyed(&K2, None)));
    assert!(air.site.command("auth", 1001, 0));
    assert!(go(&mut air, &mut seen, 3000, |a| a.site.record(1001).is_none()));
    assert_eq!(seen.reg(1001, REG_AUTH_FAIL), 1);
    assert!(seen.ev.iter().any(|e| e.r#type == EV_RECORD && e.src == 1001 && e.why == 1));
    assert_eq!(seen.acks_to(1001).len(), acks);
    // no log line carries the key or the expected response
    for e in seen.ev.iter().filter(|e| e.r#type == EV_LOG) {
        let t = e.text();
        assert!(!t.contains("a5") && !t.contains("A5"), "{t}");
    }
}

#[test]
fn poll_unanswered() {
    let mut s = site(AUTH_REGISTRATION);
    s.set_ms_policy(1001, Some(&keyed(&K1, None)));
    let mut seen = Seen::default();
    let mut air = Air::new(&mut s, [ms(1001, Some(&K1), None)], T0);
    assert!(register(&mut air, &mut seen, 0));
    air.lose_out = (OP_AHOY, 100);
    assert!(air.site.command("auth", 1001, 0));
    go(&mut air, &mut seen, 5000, |_| false);
    // three tries a second apart, then given up; an unanswered poll alone doesn't remove the registration
    assert_eq!(seen.challenges(1001).len(), 4);
    assert_eq!(seen.reg(1001, REG_AUTH_NONE), 1);
    assert!(air.site.record(1001).is_some());
}

#[test]
fn the_ms_answers_a_poll_when_idle() {
    // 6.4.8.3: C_ACKU(Authentication Response) with the response as the target and the MS as the source
    let mut m = ms(1001, Some(&K1), Some(0x010203));
    let s = site(AUTH_OFF);
    let sc = s.syscode();
    let al = Aloha { syscode: sc, nrand_wait: 6, backoff: 3, ..Default::default() }.encode();
    m.rx(0, 0, DT_CSBK, &al, false);
    let a = Ahoy { service_kind: SK_REG, target: 1001, source: 0x7A17C0, ..Default::default() }.encode();
    m.rx(0, 2, DT_CSBK, &a, true);
    let tx = m.tx(3 + 1, T0).expect("an answer");
    let ack = Ack::decode(&tx.pdu).unwrap();
    assert_eq!((ack.op, ack.reason, ack.target, ack.source), (OP_ACKU, R_AUTH_RESPONSE, 0xE9C81E, 1001));
    assert_eq!(m.state(), t3ms::IDLE);
    // another MS's challenge, or the talkgroup list request (Service_Kind_Flag 1), isn't one
    let b = Ahoy { service_kind: SK_REG, target: 1002, source: 1, ..Default::default() }.encode();
    m.rx(0, 6, DT_CSBK, &b, true);
    let c = Ahoy { service_kind: SK_REG, service_kind_flag: true, target: 1001, source: TATTSI, ..Default::default() };
    m.rx(0, 8, DT_CSBK, &c.encode(), true);
    assert_eq!(m.challenges(), 1);
    // without a key: C_NACKU(MSNot_Supported)
    m.set_key(None, None);
    m.rx(0, 10, DT_CSBK, &a, true);
    let tx = m.tx(12, T0 + 400).expect("an answer");
    let n = Ack::decode(&tx.pdu).unwrap();
    assert_eq!((n.op, n.reason, n.source), (OP_ACKU, R_MS_NOT_SUPPORTED, 1001));
}
