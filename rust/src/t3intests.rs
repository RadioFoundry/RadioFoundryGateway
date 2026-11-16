// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! Scenario tests for transmit interrupt (docs/spec/fw/tier3.md 3.20): the payload interrupt of a group and an
//! individual call (ETSI TS 102 361-4 6.4.14.3.3), ALS termination (6.4.14.3.4) and the call timer de-key
//! (6.4.14.3.2), on the slot-exact air with the simulated MS asking and obeying.

use std::boxed::Box;
use std::vec::Vec;

use crate::t3air::Air;
use crate::t3ms::{self, Ms, TX_VOICE};
use crate::t3pdu::*;
use crate::t3rc::RC_CEASE;
use crate::t3site::*;

const T0: i64 = 1_793_912_651_000;
const TG: u32 = 9;

fn site_with(f: impl FnOnce(&mut Cfg)) -> Box<Site> {
    let mut c = cfg_default();
    f(&mut c);
    new_site(&c)
}

fn on(c: &mut Cfg) {
    c.intr = 1;
}

/// n carriers, chan 1..=n, the TSCC on chan 1 LCN 0.
fn carriers(c: &mut Cfg, n: usize) {
    c.ncarriers = n as u8;
    for k in 1..n {
        let off = k as u32 * 25_000;
        c.carriers[k] =
            Carrier { tx_hz: 438_600_000 + off, rx_hz: 431_000_000 + off, chan: k as u16 + 1, tscc: 0, enabled: 1 };
    }
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
    fn of(&self, ty: u8) -> Vec<Event> {
        self.0.iter().filter(|e| e.r#type == ty).copied().collect()
    }
    /// The ACT_INTR events so far: (op, interrupting MS, the call's destination, channel).
    fn intr(&self) -> Vec<(u8, u32, u32, u8)> {
        let act = |e: &&Event| e.r#type == EV_ACTIVITY && e.kind == ACT_INTR;
        self.0.iter().filter(act).map(|e| (e.op, e.src, e.dst, e.ch)).collect()
    }
}

fn register_all<const N: usize>(air: &mut Air<N>, log: &mut Log) {
    let now = air.now();
    for (i, m) in air.ms.iter_mut().enumerate() {
        m.act(t3ms::DO_REGISTER, 0, 0, 0, now + 100 * i as i64);
    }
    assert!(air.until(20_000, &mut log.f(), |a| a.ms.iter().all(|m| m.status().registered != 0)), "registration");
}

/// What the site sent while running ms milliseconds: TSCC blocks (dt, PDU), payload channel CSBKs (ch, PDU), RC
/// bursts (ch).
#[derive(Default)]
struct Out {
    tscc: Vec<(u8, [u8; 10])>,
    pay: Vec<(usize, [u8; 10])>,
    rc: Vec<usize>,
}

fn watch<const N: usize>(air: &mut Air<N>, log: &mut Log, ms: i64, mut stop: impl FnMut(&Air<N>) -> bool) -> Out {
    let mut o = Out::default();
    let end = air.now() + ms;
    while air.now() < end && !stop(air) {
        air.step(&mut log.f());
        if (air.k - 1) & 1 == 0 {
            o.tscc.push((air.last.tscc_dt, air.last.tscc_pdu));
        }
        for (ch, c) in air.last.ch.iter().enumerate() {
            if c.dt == DT_CSBK {
                o.pay.push((ch, c.pdu));
            }
            if c.rc != 0 {
                assert_eq!(c.rc, RC_SEND | RC_CEASE, "the cease command, 0100b (table 6.32)");
                o.rc.push(ch);
            }
        }
    }
    o
}

fn acks(o: &Out, target: u32) -> Vec<Ack> {
    o.tscc
        .iter()
        .filter(|(dt, _)| *dt == DT_CSBK)
        .filter_map(|(_, p)| Ack::decode(p))
        .filter(|a| a.target == target)
        .collect()
}

fn protects(o: &Out) -> Vec<(usize, Protect)> {
    o.pay.iter().filter_map(|(ch, p)| Protect::decode(p).map(|x| (*ch, x))).collect()
}

/// A C_RAND straight into the site's TSCC (a request the simulated MS wouldn't make), then 1.5 s of output.
fn inject<const N: usize>(air: &mut Air<N>, log: &mut Log, r: Rand) -> Out {
    let ch = air.site.tscc_ch();
    air.site.rx_pdu(ch, DT_CSBK, &r.encode());
    watch(air, log, 1500, |_| false)
}

fn talks<const N: usize>(i: usize, src: u32, dst: u32) -> impl FnMut(&Air<N>) -> bool {
    move |a| a.inbound[i].is_some_and(|t| t.kind == TX_VOICE && t.src == src && t.dst == dst)
}

// ── payload interrupt (6.4.14.3.3) ──

/// A talks on a talkgroup call; B, listening, interrupts: status 125 to the talkgroup, C_ACKD from TSI, the RC
/// cease command to A in the TSCC's slot (one carrier), A stops, P_PROTECT(EN_PTT_ONE_MS) to B, B talks, A and C hear
/// it and C keeps its PTT disabled meanwhile.
#[test]
fn group_call_interrupted() {
    let mut s = site_with(on);
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001), ms(1002), ms(1003)], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].act(t3ms::DO_GROUP, TG, 30_000, 0, now);
    assert!(air.until(5000, &mut log.f(), |a| {
        a.ms[1].state() == t3ms::PAYLOAD && a.ms[2].state() == t3ms::PAYLOAD && a.inbound[0].is_some()
    }));
    let now = air.now();
    assert!(air.ms[1].interrupt(TG, true, 2000, now + 500));
    let o = watch(&mut air, &mut log, 8000, talks(1, 1002, TG));
    assert!(talks(1, 1002, TG)(&air), "B talks on the talkgroup");
    // the request and its answer: a status call to the talkgroup, C_ACKD(Message_Accepted) from TSI
    let a = acks(&o, 1002);
    assert!(!a.is_empty() && a.iter().all(|a| (a.op, a.reason, a.source) == (OP_ACKD, R_MESSAGE_ACCEPTED, TSI)));
    // the RC on the alternate slot of the call's carrier, the TSCC's (6.4.15.1)
    assert!(!o.rc.is_empty() && o.rc.iter().all(|&ch| ch == 0), "{:?}", o.rc);
    assert_eq!(air.ms[0].ceased(), 1);
    // P_PROTECT(EN_PTT_ONE_MS) to B from TSI, on the payload channel, a few (6.4.15.1)
    let p = protects(&o);
    let want = Protect { kind: PROTECT_EN_PTT_ONE_MS, group: false, target: 1002, source: TSI };
    assert!(!p.is_empty() && p.len() <= 17 && p.iter().all(|&(ch, x)| ch == 1 && x == want), "{p:?}");
    let ch = log.of(EV_CH_OPEN)[0].ch;
    assert_eq!(log.intr(), [(INTR_ACCEPTED, 1002, TG, ch), (INTR_CEASED, 1002, TG, ch)]);
    air.run(1000, &mut log.f());
    assert_eq!(log.intr().last(), Some(&(INTR_TALKS, 1002, TG, ch)));
    // the call goes on with B the talker; A and C heard it; C saw the P_PROTECT for another MS
    let (ok, lost, _, _) = air.ms[1].interrupts();
    assert_eq!((ok, lost), (1, 0));
    assert!(air.ms[2].interrupts().3 > 0);
    assert!(air.ms[0].voice_heard() > 0 && air.ms[0].state() == t3ms::PAYLOAD);
    assert!(log.of(EV_CH_CLOSE).is_empty());
    // B ends its talk; the call hangs and clears as usual
    assert!(air.until(30_000, &mut log.f(), |a| a.ms.iter().all(|m| m.state() == t3ms::IDLE)));
    air.run(2000, &mut log.f());
    assert_eq!(log.of(EV_CH_CLOSE).len(), 1);
}

/// Two carriers, the call on the second: the RC goes on that carrier's other slot, not the TSCC's.
#[test]
fn group_interrupt_rc_on_the_payload_carrier() {
    let mut s = site_with(|c| {
        on(c);
        carriers(c, 2);
    });
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001), ms(1002), Ms::new(1003, &[10], &[1, 2], 7)], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[2].act(t3ms::DO_GROUP, 10, 30_000, 0, now); // takes chan 1 LCN 1
    air.ms[0].act(t3ms::DO_GROUP, TG, 30_000, 0, now + 300); // on chan 2
    assert!(air.until(5000, &mut log.f(), |a| a.ms[1].state() == t3ms::PAYLOAD && a.inbound[0].is_some()));
    let ch = log.of(EV_CH_OPEN).iter().find(|e| e.dst == TG).unwrap().ch as usize;
    assert!(ch >= 2);
    let now = air.now();
    air.ms[1].interrupt(TG, true, 2000, now + 200);
    let o = watch(&mut air, &mut log, 8000, talks(1, 1002, TG));
    assert!(talks(1, 1002, TG)(&air));
    assert!(!o.rc.is_empty() && o.rc.iter().all(|&r| r == ch ^ 1), "{:?} for {ch}", o.rc);
    assert_eq!((air.ms[0].ceased(), air.ms[2].ceased()), (1, 0));
}

/// The individual call form: B, called by A, interrupts A (status 125 to A's MSID, G/I 0) and talks back.
#[test]
fn individual_call_interrupted() {
    let mut s = site_with(on);
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001), ms(1002)], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].act(t3ms::DO_PRIVATE, 1002, 30_000, 0, now);
    assert!(air.until(5000, &mut log.f(), talks(0, 1001, 1002)));
    let now = air.now();
    air.ms[1].interrupt(1001, false, 2000, now + 300);
    let o = watch(&mut air, &mut log, 8000, talks(1, 1002, 1001));
    assert!(talks(1, 1002, 1001)(&air), "B talks to A");
    assert!(acks(&o, 1002).iter().all(|a| (a.reason, a.source) == (R_MESSAGE_ACCEPTED, TSI)));
    assert_eq!(air.ms[0].ceased(), 1);
    let p = protects(&o);
    assert!(!p.is_empty() && p.iter().all(|(_, x)| x.target == 1002 && !x.group));
    // B ends its talk without P_MAINT (it isn't its call); A ends the call with P_MAINT when it next talks, or the
    // hang time runs out
    assert!(air.until(30_000, &mut log.f(), |a| a.ms.iter().all(|m| m.state() == t3ms::IDLE)));
    air.run(2000, &mut log.f()); // the P_CLEAR repeats, then the close
    let close = log.of(EV_CH_CLOSE);
    assert_eq!(close.len(), 1);
    assert_ne!(close[0].why, CLOSE_INTR);
}

/// An interrupt in hang time: nobody to de-key, P_PROTECT at once.
#[test]
fn interrupt_in_hang_time() {
    let mut s = site_with(|c| {
        on(c);
        c.hang_ms = 10_000;
    });
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001), ms(1002)], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].act(t3ms::DO_GROUP, TG, 1000, 0, now);
    assert!(air.until(5000, &mut log.f(), talks(0, 1001, TG)));
    assert!(air.until(3000, &mut log.f(), |a| a.site.status().ch[1].r#use == USE_HANG));
    let now = air.now();
    air.ms[1].interrupt(TG, true, 1000, now);
    let o = watch(&mut air, &mut log, 5000, talks(1, 1002, TG));
    assert!(talks(1, 1002, TG)(&air));
    assert!(o.rc.is_empty());
    assert_eq!(log.intr().iter().map(|x| x.0).collect::<Vec<_>>(), [INTR_ACCEPTED]);
    air.run(500, &mut log.f());
    assert_eq!(log.intr().last().unwrap().0, INTR_TALKS);
}

/// A talker that doesn't obey the RC keeps the call (6.4.15.1); the interrupter gives up and stays in it.
#[test]
fn talker_that_doesnt_cease_keeps_the_call() {
    let mut s = site_with(on);
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001), ms(1002)], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].act(t3ms::DO_GROUP, TG, 20_000, 0, now);
    assert!(air.until(5000, &mut log.f(), |a| a.ms[1].state() == t3ms::PAYLOAD && a.inbound[0].is_some()));
    // A ignores the RC
    air.ms[0].rc_deaf(true);
    let now = air.now();
    air.ms[1].interrupt(TG, true, 2000, now);
    let mut rc = 0;
    while air.now() < now + 8000 {
        air.step(&mut log.f());
        rc += air.last.ch.iter().filter(|c| c.rc != 0).count();
    }
    assert!(rc >= 5, "{rc}");
    assert_eq!(air.ms[0].ceased(), 0);
    assert_eq!(log.intr().iter().map(|x| x.0).collect::<Vec<_>>(), [INTR_ACCEPTED, INTR_GAVE_UP]);
    assert!(air.until(200, &mut log.f(), talks(0, 1001, TG)), "A talks on");
    assert!(log.of(EV_CH_CLOSE).is_empty());
    assert_eq!(air.ms[1].interrupts().0, 0);
    assert_eq!(air.ms[1].state(), t3ms::PAYLOAD);
}

// ── ALS termination (6.4.14.3.4) ──

/// The ALS caller ends the call: status 123 to the called MS, C_ACKD from TSI, the called MS de-keyed, P_CLEAR; the
/// caller stays on the TSCC.
#[test]
fn als_call_ended_by_its_caller() {
    let mut s = site_with(|c| {
        on(c);
        c.als = 1;
    });
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001), ms(1002)], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].act(t3ms::DO_PRIVATE, 1002, 0, t3ms::F_ALS, now);
    assert!(air.until(5000, &mut log.f(), talks(1, 1002, 1001)));
    let (done, now) = (air.ms[0].status().calls_done, air.now());
    air.ms[0].als_end(1002, now + 500);
    let o = watch(&mut air, &mut log, 5000, |a| a.ms[1].state() == t3ms::IDLE);
    assert_eq!(air.ms[1].state(), t3ms::IDLE, "the called MS went back on P_CLEAR");
    air.run(2000, &mut log.f());
    let a = acks(&o, 1001);
    assert!(!a.is_empty() && a.iter().all(|a| (a.reason, a.source) == (R_MESSAGE_ACCEPTED, TSI)));
    assert!(!o.rc.is_empty());
    assert_eq!(air.ms[1].ceased(), 1);
    let close = log.of(EV_CH_CLOSE);
    assert_eq!((close.len(), close[0].why), (1, CLOSE_INTR));
    assert_eq!(
        log.intr().iter().map(|x| (x.0, x.1, x.2)).collect::<Vec<_>>(),
        [(INTR_ACCEPTED, 1001, 1002), (INTR_CEASED, 1001, 1002)]
    );
    assert!(protects(&o).is_empty());
    // the caller stayed on the TSCC (6.4.14.3.4.3)
    assert_eq!(air.ms[0].state(), t3ms::IDLE);
    assert_eq!(air.ms[0].status().calls_done, done + 2, "the ALS call and the termination");
}

/// Only the ALS caller may end it, and only with the setting on.
#[test]
fn als_termination_refusals() {
    let mut s = site_with(|c| {
        on(c);
        c.als = 1;
    });
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001), ms(1002), ms(1003)], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].act(t3ms::DO_PRIVATE, 1002, 0, t3ms::F_ALS, now);
    assert!(air.until(5000, &mut log.f(), talks(1, 1002, 1001)));
    for (r, want) in [
        (status_rand(123, false, 1002, 1003), R_PERM_USER_REFUSED), // not the caller
        (status_rand(123, true, 1002, 1001), R_NOT_SUPPORTED),      // G/I 1
        (status_rand(123, false, 1003, 1001), R_NOT_SUPPORTED),     // no ALS call to 1003
    ] {
        let src = r.source;
        let o = inject(&mut air, &mut log, r);
        let a = acks(&o, src);
        assert!(!a.is_empty() && a.iter().all(|a| (a.op, a.reason, a.source) == (OP_ACKD, want, TSI)), "{a:?}");
    }
    assert!(air.inbound[1].is_some_and(|t| t.kind == TX_VOICE), "the call goes on");
    assert!(log.intr().is_empty());
}

// ── refusals ──

/// Off (the default): Not_Supported from TSI, and the interrupter goes back to its call and keeps listening.
#[test]
fn interrupt_off_by_default() {
    assert_eq!(cfg_default().intr, 0);
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001), ms(1002)], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].act(t3ms::DO_GROUP, TG, 10_000, 0, now);
    assert!(air.until(5000, &mut log.f(), |a| a.ms[1].state() == t3ms::PAYLOAD && a.inbound[0].is_some()));
    let (failed, now) = (air.ms[1].status().calls_failed, air.now());
    air.ms[1].interrupt(TG, true, 2000, now);
    let o = watch(&mut air, &mut log, 3000, |_| false);
    let a = acks(&o, 1002);
    assert!(!a.is_empty() && a.iter().all(|a| (a.op, a.reason, a.source) == (OP_ACKD, R_NOT_SUPPORTED, TSI)));
    assert!(o.rc.is_empty() && protects(&o).is_empty());
    assert_eq!(air.ms[1].status().calls_failed, failed + 1);
    assert_eq!(air.ms[1].state(), t3ms::PAYLOAD, "back in the call (6.4.14.3.3)");
    assert_eq!(air.ms[0].ceased(), 0);
    assert!(log.intr().is_empty());
}

/// Who may interrupt what (6.4.14.3.3.1): not a party of an individual call, a broadcast call, another's interrupt
/// under way, no call at all.
#[test]
fn interrupt_refusals() {
    let mut s = site_with(|c| {
        on(c);
        carriers(c, 2);
    });
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001), ms(1002), ms(1003), ms(1004)], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].act(t3ms::DO_PRIVATE, 1002, 30_000, 0, now);
    air.ms[2].act(t3ms::DO_GROUP, TG, 30_000, t3ms::F_BROADCAST, now + 300);
    assert!(air.until(5000, &mut log.f(), |a| a.site.status().ch.iter().filter(|c| c.r#use == USE_CALL).count() == 2));
    let check = |air: &mut Air<4>, log: &mut Log, r: Rand, want: u8| {
        let src = r.source;
        let o = inject(air, log, r);
        let a = acks(&o, src);
        assert!(!a.is_empty() && a.iter().all(|a| (a.reason, a.source) == (want, TSI)), "{want}: {a:?}");
    };
    check(&mut air, &mut log, status_rand(125, false, 1001, 1004), R_PERM_USER_REFUSED); // not a party
    check(&mut air, &mut log, status_rand(125, true, TG, 1004), R_PERM_USER_REFUSED); // a broadcast call
    check(&mut air, &mut log, status_rand(125, true, 77, 1004), R_NOT_SUPPORTED); // no call
    check(&mut air, &mut log, status_rand(125, false, 1002, 1001), R_NOT_SUPPORTED); // the talker itself
                                                                                     // the called party interrupts; a second interrupter meanwhile is refused
    let ch = air.site.tscc_ch();
    air.site.rx_pdu(ch, DT_CSBK, &status_rand(125, false, 1001, 1002).encode());
    check(&mut air, &mut log, status_rand(125, false, 1002, 1004), R_PERM_USER_REFUSED);
    assert_eq!(log.intr()[0].0, INTR_ACCEPTED);
    assert_eq!(air.ms[2].ceased(), 0, "the broadcast call goes on");
}

// ── across sites (SD_INTR) ──

fn wide_site(intr: u8) -> Box<Site> {
    let mut s = site_with(|c| c.intr = intr);
    let mut p = Policy::default_open();
    p.wide_tg = IdSet::of(&[(TG, TG)]);
    s.policy_set(&p);
    s.set_link(true);
    s
}

/// Every MS registers subscribed to TG (a site then wants the talkgroup's network calls).
fn register_tg<const N: usize>(air: &mut Air<N>, log: &mut Log) {
    let now = air.now();
    for (i, m) in air.ms.iter_mut().enumerate() {
        m.act(t3ms::DO_REGISTER, TG, 0, 0, now + 100 * i as i64);
    }
    assert!(air.until(20_000, &mut log.f(), |a| a.ms.iter().all(|m| m.status().registered != 0)), "registration");
}

/// Two sites run slot by slot: the talkgroup call of B's MS 2001 reaches A as a network stream (77) while 2001
/// talks, and SD_INTR messages go each way as the network would carry them. Returns A's output; `stop` ends it.
fn two_sites(
    a: &mut Air<2>,
    b: &mut Air<1>,
    la: &mut Log,
    lb: &mut Log,
    ms: i64,
    mut stop: impl FnMut(&Air<2>, &Air<1>) -> bool,
) -> Out {
    let mut o = Out::default();
    let end = a.now() + ms;
    let mut heard = i64::MIN / 2; // B's talker's last voice burst (on its slot: every other one)
    let mut streaming = false;
    while a.now() < end && !stop(a, b) {
        let (na, nb) = (la.0.len(), lb.0.len());
        let one = watch(a, la, 30, |_| false);
        o.tscc.extend(one.tscc);
        o.pay.extend(one.pay);
        o.rc.extend(one.rc);
        b.step(&mut lb.f());
        let now = a.now();
        if b.inbound[0].is_some_and(|t| t.kind == TX_VOICE) {
            heard = now;
        }
        let talking = now - heard < 200;
        if talking && !streaming {
            a.site.net_group_in(77, 2001, TG, 0, now);
        } else if !talking && streaming {
            a.site.net_end_in(77);
        }
        streaming = talking;
        let sd = |l: &Log, n: usize| -> Vec<SdMsg> {
            l.0[n..].iter().filter(|e| e.r#type == EV_NET_SD).map(SdMsg::of_event).collect()
        };
        for m in sd(la, na) {
            b.site.net_sd_in(&m, now);
        }
        for m in sd(lb, nb) {
            a.site.net_sd_in(&m, now);
        }
    }
    o
}

/// The talker is at another site: A's site sends SD_INTR, B's site de-keys its MS with the RC and answers
/// Message_Accepted; once the stream ends, A sends P_PROTECT(EN_PTT_ONE_MS) to the interrupter, which talks. B's
/// channel sends P_PROTECT to it too, so B's MSs stay off the channel meanwhile.
#[test]
fn interrupt_a_talker_at_another_site() {
    let (mut sa, mut sb) = (wide_site(1), wide_site(1));
    let (mut la, mut lb) = (Log::default(), Log::default());
    let mut a = Air::new(&mut sa, [ms(1001), ms(1002)], T0);
    let mut b = Air::new(&mut sb, [ms(2001)], T0);
    register_tg(&mut a, &mut la);
    register_all(&mut b, &mut lb);
    while b.now() < a.now() {
        b.step(&mut lb.f());
    }
    let now = b.now();
    b.ms[0].act(t3ms::DO_GROUP, TG, 30_000, 0, now);
    two_sites(&mut a, &mut b, &mut la, &mut lb, 5000, |a, _| a.ms.iter().all(|m| m.state() == t3ms::PAYLOAD));
    assert!(a.ms[1].state() == t3ms::PAYLOAD, "A's MSs listen to the call from B");
    let now = a.now();
    assert!(a.ms[1].interrupt(TG, true, 2000, now + 500));
    let o = two_sites(&mut a, &mut b, &mut la, &mut lb, 10_000, |a, _| talks(1, 1002, TG)(a));
    assert!(talks(1, 1002, TG)(&a), "the interrupter talks");
    let ack = acks(&o, 1002);
    assert!(!ack.is_empty() && ack.iter().all(|x| (x.reason, x.source) == (R_MESSAGE_ACCEPTED, TSI)));
    assert!(o.rc.is_empty(), "A sends no RC: its talker is B's");
    assert_eq!(b.ms[0].ceased(), 1, "B de-keyed its talker");
    let p = protects(&o);
    assert!(!p.is_empty() && p.iter().all(|(_, x)| x.target == 1002 && x.kind == PROTECT_EN_PTT_ONE_MS), "{p:?}");
    let ch = la.of(EV_CH_OPEN)[0].ch;
    assert_eq!(la.intr(), [(INTR_ACCEPTED, 1002, TG, ch), (INTR_CEASED, 1002, TG, ch)]);
    let chb = lb.of(EV_CH_OPEN)[0].ch;
    assert_eq!(lb.intr(), [(INTR_ACCEPTED, 1002, TG, chb), (INTR_CEASED, 1002, TG, chb)]);
    // B's result went back: Message_Accepted, ref A's message
    let sent = la.of(EV_NET_SD).iter().map(SdMsg::of_event).find(|m| m.what == SD_INTR).unwrap();
    assert_eq!((sent.src, sent.dst, sent.value, sent.data()), (1002, 2001, 125, &[0, 0, TG as u8, 1][..]));
    let res = lb.of(EV_NET_SD).iter().map(SdMsg::of_event).find(|m| m.what == SD_INTR).unwrap();
    assert_eq!((res.op, res.reason, res.r#ref), (SD_RESULT, R_MESSAGE_ACCEPTED, sent.stream));
}

/// Across sites, refused: the link down (Not_Supported at once), or the talker's site without the setting (its
/// Not_Supported result ends A's wait: the call goes on).
#[test]
fn interrupt_across_sites_refused() {
    for link in [false, true] {
        let (mut sa, mut sb) = (wide_site(1), wide_site(0));
        let (mut la, mut lb) = (Log::default(), Log::default());
        let mut a = Air::new(&mut sa, [ms(1001), ms(1002)], T0);
        let mut b = Air::new(&mut sb, [ms(2001)], T0);
        register_tg(&mut a, &mut la);
        register_all(&mut b, &mut lb);
        while b.now() < a.now() {
            b.step(&mut lb.f());
        }
        let now = b.now();
        b.ms[0].act(t3ms::DO_GROUP, TG, 30_000, 0, now);
        two_sites(&mut a, &mut b, &mut la, &mut lb, 5000, |a, _| a.ms.iter().all(|m| m.state() == t3ms::PAYLOAD));
        if !link {
            a.site.set_link(false);
            let now = a.now();
            a.site.net_group_in(77, 2001, TG, 0, now); // the stream is gone with the link: as if it went on
        }
        let now = a.now();
        assert!(a.ms[1].interrupt(TG, true, 2000, now + 500));
        let o = two_sites(&mut a, &mut b, &mut la, &mut lb, 3000, |_, _| false);
        let ack = acks(&o, 1002);
        let want = if link { R_MESSAGE_ACCEPTED } else { R_NOT_SUPPORTED };
        assert!(!ack.is_empty() && ack.iter().all(|x| x.reason == want), "link {link}: {ack:?}");
        assert_eq!(b.ms[0].ceased(), 0, "link {link}");
        if link {
            let ch = la.of(EV_CH_OPEN)[0].ch;
            assert_eq!(la.intr(), [(INTR_ACCEPTED, 1002, TG, ch), (INTR_GAVE_UP, 1002, TG, ch)]);
            let res = lb.of(EV_NET_SD).iter().map(SdMsg::of_event).find(|m| m.what == SD_INTR).unwrap();
            assert_eq!((res.op, res.reason), (SD_RESULT, R_NOT_SUPPORTED));
        } else {
            assert!(la.of(EV_NET_SD).is_empty());
        }
    }
}

// ── call timer expiration de-key (6.4.14.3.2) ──

/// The call timer runs out while A talks: on, the RC cease command first and P_CLEAR once A stops; off, P_CLEAR
/// straight away.
#[test]
fn call_timer_dekeys_the_talker() {
    for intr in [1, 0] {
        let mut s = site_with(|c| {
            c.intr = intr;
            c.call_ms = 6000;
        });
        let mut log = Log::default();
        let mut air = Air::new(&mut s, [ms(1001), ms(1002)], T0);
        register_all(&mut air, &mut log);
        let now = air.now();
        air.ms[0].act(t3ms::DO_GROUP, TG, 30_000, 0, now);
        let o = watch(&mut air, &mut log, 12_000, |a| a.ms[0].status().clears > 0);
        air.run(2000, &mut log.f());
        let close = log.of(EV_CH_CLOSE);
        assert_eq!((close.len(), close[0].why), (1, CLOSE_TIMER), "intr {intr}");
        assert_eq!(air.ms[0].ceased(), intr as u32, "intr {intr}");
        assert_eq!(o.rc.is_empty(), intr == 0);
        assert!(log.intr().is_empty());
    }
}

#[allow(dead_code)]
fn dump(log: &Log) {
    for e in log.0.iter().filter(|e| e.r#type != EV_RECORD) {
        let t = core::str::from_utf8(&e.text).unwrap_or("");
        std::eprintln!(
            "ty{} k{} op{} src{} dst{} ch{} why{} r{} {}",
            e.r#type,
            e.kind,
            e.op,
            e.src,
            e.dst,
            e.ch,
            e.why,
            e.reason,
            t.trim_end_matches(char::from(0))
        );
    }
}
