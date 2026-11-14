// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! Scenario tests: the site and simulated MSs on the slot-exact air (docs/spec/fw/tier3.md section 12, the
//! controller list).

use std::boxed::Box;
use std::vec::Vec;

use crate::t3air::Air;
use crate::t3ms::{self, Ms};
use crate::t3pdu::*;
use crate::t3site::*;

const T0: i64 = 1_793_912_651_000; // 2026-11-05 21:04:11 UTC
const TG: u32 = 9;

fn site_with(f: impl FnOnce(&mut Cfg)) -> Box<Site> {
    let mut c = cfg_default();
    f(&mut c);
    new_site(&c)
}

/// Two carriers: the TSCC on chan 1 LCN 0, payload channels 1, 2 and 3.
fn two_carriers(c: &mut Cfg) {
    c.ncarriers = 2;
    c.carriers[1] = Carrier { tx_hz: 438_600_000, rx_hz: 431_000_000, chan: 2, tscc: 0, enabled: 1 };
}

fn ms(id: u32, tg: &[u32], chans: &[u16]) -> Ms {
    Ms::new(id, tg, chans, id.wrapping_mul(2654435761))
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
    fn setups(&self, op: u8) -> Vec<Event> {
        self.of(EV_NET_SETUP).into_iter().filter(|e| e.op == op).collect()
    }
    fn refused(&self, reason: u8) -> bool {
        self.of(EV_ACTIVITY).iter().any(|e| e.kind == ACT_REFUSE && e.reason == reason)
    }
    /// The events the site holds now (between steps).
    fn take<const N: usize>(&mut self, air: &mut Air<N>) -> &Self {
        while let Some(e) = air.site.next_event() {
            self.0.push(e);
        }
        self
    }
}

/// Every MS registers, a little apart.
fn register_all<const N: usize>(air: &mut Air<N>, log: &mut Log) {
    let now = air.now();
    for (i, m) in air.ms.iter_mut().enumerate() {
        m.act(t3ms::DO_REGISTER, 0, 0, 0, now + 100 * i as i64);
    }
    assert!(air.until(20_000, &mut log.f(), |a| a.ms.iter().all(|m| m.status().registered != 0)), "registration");
}

fn reg_tg<const N: usize>(air: &mut Air<N>, log: &mut Log, i: usize, tg: u32) {
    let now = air.now();
    air.ms[i].act(t3ms::DO_REGISTER, tg, 0, 0, now);
    let id = air.ms[i].id();
    let ok = air.until(10_000, &mut log.f(), |a| {
        a.site.record(id).is_some_and(|r| r.sub_tg == tg) && a.ms[i].status().registered != 0 && a.ms[i].state() == t3ms::IDLE
    });
    assert!(ok, "subscribe {tg}");
}

fn all_idle<const N: usize>(a: &Air<N>) -> bool {
    a.ms.iter().all(|m| m.state() == t3ms::IDLE)
}

fn all_payload<const N: usize>(a: &Air<N>) -> bool {
    a.ms.iter().all(|m| m.state() == t3ms::PAYLOAD)
}

fn last_aloha<const N: usize>(a: &Air<N>) -> Option<Aloha> {
    (a.last.tscc_dt == DT_CSBK).then(|| Aloha::decode(&a.last.tscc_pdu)).flatten()
}

// ── registration ──

#[test]
fn registration_open() {
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1])], T0);
    reg_tg(&mut air, &mut log, 0, TG);
    let r = air.site.record(1001).unwrap();
    assert_eq!((r.how, r.sub_tg, r.tgs()), (HOW_REGISTER, TG, &[TG][..]));
    assert!(log.of(EV_RECORD).iter().any(|e| e.src == 1001 && e.why == 0));
    assert!(!log.of(EV_NET_SUBS).is_empty());
    let mut subs = [0u32; 8];
    let n = air.site.subs(&mut subs);
    assert_eq!(&subs[..n], &[TG]);
    assert_eq!(air.ms[0].status().registered, 1);
    assert_eq!(air.ms[0].status().last_reason, R_REG_ACCEPTED);

    // deregistration: Reg_Accepted, the record goes
    let now = air.now();
    air.ms[0].act(t3ms::DO_DEREGISTER, 0, 0, 0, now);
    assert!(air.until(5000, &mut log.f(), |a| a.ms[0].status().registered == 0));
    assert!(air.site.record(1001).is_none());
    assert!(log.of(EV_RECORD).iter().any(|e| e.src == 1001 && e.why == 1));
}

#[test]
fn registration_listed_and_denied() {
    let mut s = site_with(|_| {});
    let mut p = Policy::default_open();
    p.open = 0;
    p.ms = IdSet::of(&[(1000, 1009)]);
    s.policy_set(&p);
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1]), ms(2001, &[TG], &[1])], T0);
    let now = air.now();
    air.ms[0].act(t3ms::DO_REGISTER, 0, 0, 0, now);
    air.ms[1].act(t3ms::DO_REGISTER, 0, 0, 0, now + 200);
    assert!(air.until(10_000, &mut log.f(), |a| a.ms[0].status().registered == 1 && a.ms[1].status().last_reason != 0));
    assert_eq!(air.ms[1].status().last_reason, R_REG_DENIED);
    assert!(air.site.record(2001).is_none());

    // refuse_unlisted: Reg_Refused instead
    p.refuse_unlisted = 1;
    air.site.policy_set(&p);
    let now = air.now();
    air.ms[1].act(t3ms::DO_REGISTER, 0, 0, 0, now);
    assert!(air.until(10_000, &mut log.f(), |a| a.ms[1].status().last_reason == R_REG_REFUSED));
    assert!(log.of(EV_ACTIVITY).iter().any(|e| e.kind == ACT_REG && e.op == REG_DENY && e.src == 2001));
    assert!(log.of(EV_ACTIVITY).iter().any(|e| e.kind == ACT_REG && e.op == REG_REFUSE && e.src == 2001));
}

#[test]
fn calls_need_registration_when_the_site_says_so() {
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1])], T0);
    let now = air.now();
    air.ms[0].act(t3ms::DO_GROUP, TG, 1000, 0, now);
    assert!(air.until(5000, &mut log.f(), |a| a.ms[0].status().last_reason != 0));
    assert_eq!(air.ms[0].status().last_reason, R_MS_NOT_REGISTERED);

    // without the Reg flag: an implicit registration, and the call goes
    let mut s = site_with(|c| c.reg_required = 0);
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1])], T0);
    let now = air.now();
    air.ms[0].act(t3ms::DO_GROUP, TG, 1000, 0, now);
    assert!(air.until(5000, &mut log.f(), |a| a.ms[0].state() == t3ms::PAYLOAD));
    let r = air.site.record(1001).unwrap();
    assert_eq!((r.how, r.tgs()), (HOW_CALL, &[TG][..]));
}

#[test]
fn collisions_and_backoff() {
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mss: [Ms; 12] = core::array::from_fn(|i| ms(1001 + i as u32, &[TG], &[1]));
    let mut air = Air::new(&mut s, mss, T0);
    air.run(500, &mut log.f());
    let now = air.now();
    for m in air.ms.iter_mut() {
        m.act(t3ms::DO_REGISTER, 0, 0, 0, now); // all at once
    }
    let mut max_backoff = 0;
    let ok = air.until(30_000, &mut log.f(), |a| {
        if let Some(x) = last_aloha(a) {
            max_backoff = max_backoff.max(x.backoff);
        }
        a.ms.iter().all(|m| m.status().registered != 0)
    });
    let n = air.ms.iter().filter(|m| m.status().registered != 0).count();
    assert!(ok, "registered: {n}");
    assert!(air.collisions > 0);
    assert!(max_backoff > 3, "{max_backoff}");
    assert_eq!(air.site.status().collisions, air.collisions);
    assert_eq!(air.site.status().regs, 12);
}

#[test]
fn backoff_rises_with_load() {
    let mut s = site_with(|_| {});
    let a0 = (0..10).map(|k| s.slot(k, T0 + k * 30)).find(|o| o.tscc_dt == DT_CSBK && Aloha::decode(&o.tscc_pdu).is_some());
    assert_eq!(Aloha::decode(&a0.unwrap().tscc_pdu).unwrap().backoff, 3);
    for _ in 0..10 {
        s.rx_bad(s.tscc_ch());
    }
    let mut max = 0;
    for k in 10..30 {
        let o = s.slot(k, T0 + k * 30);
        if let Some(a) = (o.tscc_dt == DT_CSBK).then(|| Aloha::decode(&o.tscc_pdu)).flatten() {
            max = max.max(a.backoff);
        }
    }
    assert_eq!(max, 8);
    // 2 s later the load has gone
    let o = (100..110)
        .map(|k| s.slot(k, T0 + 3000 + k * 30))
        .find(|o| o.tscc_dt == DT_CSBK && Aloha::decode(&o.tscc_pdu).is_some());
    assert_eq!(Aloha::decode(&o.unwrap().tscc_pdu).unwrap().backoff, 3);
}

// ── group calls ──

#[test]
fn group_call_hang_and_clear() {
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1]), ms(1002, &[TG], &[1]), ms(1003, &[TG], &[1])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].act(t3ms::DO_GROUP, TG, 2000, 0, now);
    assert!(air.until(5000, &mut log.f(), all_payload));
    let open = log.of(EV_CH_OPEN);
    assert_eq!(open.len(), 1);
    assert_eq!((open[0].ch, open[0].chan, open[0].lcn, open[0].src, open[0].dst), (1, 1, 1, 1001, TG));
    assert_eq!(open[0].flags & (F_GROUP | F_NET), F_GROUP);
    air.run(1000, &mut log.f());
    assert_eq!(air.site.status().ch[1].r#use, USE_CALL);
    assert_eq!(air.ms[1].status().heard_src, 1001);
    // talk 2 s, hang 3 s, then P_CLEAR takes them all back
    assert!(air.until(8000, &mut log.f(), all_idle));
    air.run(300, &mut log.f()); // the rest of the P_CLEARs
    let close = log.of(EV_CH_CLOSE);
    assert_eq!((close.len(), close[0].why), (1, CLOSE_HANG));
    assert_eq!(air.ms[0].status().calls_done, 1);
    assert!(air.ms.iter().all(|m| m.status().clears == 1));
    assert_eq!(air.site.status().ch[1].r#use, USE_IDLE);
    // the call's talkgroup went into the caller's record
    assert_eq!(air.site.record(1001).unwrap().tgs(), &[TG]);
}

#[test]
fn group_queue_and_grant_on_release() {
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1]), ms(1002, &[10], &[1])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].act(t3ms::DO_GROUP, TG, 3000, 0, now);
    air.ms[1].act(t3ms::DO_GROUP, 10, 1000, 0, now + 600);
    assert!(air.until(5000, &mut log.f(), |a| a.ms[1].status().last_reason == R_QUEUED));
    assert_eq!(air.ms[1].state(), t3ms::WAITING);
    assert_eq!(air.site.status().queue, 1);
    // A's call ends (talk, hang, clear): B gets the channel
    assert!(air.until(10_000, &mut log.f(), |a| a.ms[1].state() == t3ms::PAYLOAD));
    let opens = log.of(EV_CH_OPEN);
    assert_eq!(opens.iter().map(|e| e.dst).collect::<Vec<_>>(), [TG, 10]);
    assert!(log.of(EV_ACTIVITY).iter().any(|e| e.kind == ACT_QUEUE && e.op == QUEUE_OUT && e.src == 1002));
    assert_eq!(air.site.status().queued, 1);
}

#[test]
fn queue_expires_with_sysbusy() {
    let mut s = site_with(|c| c.queue_ms = 2000);
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1]), ms(1002, &[10], &[1])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].act(t3ms::DO_GROUP, TG, 8000, 0, now);
    air.ms[1].act(t3ms::DO_GROUP, 10, 1000, 0, now + 600);
    assert!(air.until(6000, &mut log.f(), |a| a.ms[1].status().last_reason == R_SYSBUSY));
    assert!(log.of(EV_ACTIVITY).iter().any(|e| e.kind == ACT_QUEUE && e.op == QUEUE_DROP));
}

#[test]
fn emergency_goes_first_in_the_queue() {
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1]), ms(1002, &[10], &[1]), ms(1003, &[11], &[1])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].act(t3ms::DO_GROUP, TG, 2000, 0, now);
    air.ms[1].act(t3ms::DO_GROUP, 10, 1000, 0, now + 500);
    air.ms[2].act(t3ms::DO_GROUP, 11, 1000, t3ms::F_EMERGENCY, now + 1000);
    assert!(air.until(15_000, &mut log.f(), |a| a.ms[2].state() == t3ms::PAYLOAD));
    let opens = log.of(EV_CH_OPEN);
    assert_eq!(opens.iter().map(|e| e.dst).collect::<Vec<_>>(), [TG, 11]);
    assert!(opens[1].flags & F_EMERGENCY != 0);
}

#[test]
fn talkgroup_priority_in_the_queue() {
    // cfg/tg's priority: tg 11 (priority 2) asked after tg 10 and still gets the channel first
    let mut s = site_with(|_| {});
    s.set_tg_policy(11, Some(&TgPolicy { tg: 11, hang_ms: 0, priority: 2, pad: [0; 3] }));
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1]), ms(1002, &[10], &[1]), ms(1003, &[11], &[1])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].act(t3ms::DO_GROUP, TG, 2000, 0, now);
    air.ms[1].act(t3ms::DO_GROUP, 10, 1000, 0, now + 500);
    air.ms[2].act(t3ms::DO_GROUP, 11, 1000, 0, now + 1000);
    assert!(air.until(15_000, &mut log.f(), |a| a.ms[2].state() == t3ms::PAYLOAD));
    let opens = log.of(EV_CH_OPEN);
    assert_eq!(opens.iter().map(|e| e.dst).collect::<Vec<_>>(), [TG, 11]);
}

#[test]
fn talkgroup_hang() {
    // cfg/tg's hang: tg 9 hangs 6 s, not the site's 3 s; removed, the site's again
    let mut s = site_with(|_| {});
    s.set_tg_policy(TG, Some(&TgPolicy { tg: TG, hang_ms: 6000, priority: 0, pad: [0; 3] }));
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1]), ms(1002, &[TG], &[1])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].act(t3ms::DO_GROUP, TG, 1000, 0, now);
    assert!(air.until(5000, &mut log.f(), all_payload));
    let t_open = air.now();
    assert!(air.until(15_000, &mut log.f(), all_idle));
    let held = air.now() - t_open;
    assert!((6500..8500).contains(&held), "held {held} ms");
    air.site.set_tg_policy(TG, None);
    let now = air.now();
    air.ms[0].act(t3ms::DO_GROUP, TG, 1000, 0, now);
    assert!(air.until(5000, &mut log.f(), all_payload));
    let t_open = air.now();
    assert!(air.until(15_000, &mut log.f(), all_idle));
    let held = air.now() - t_open;
    assert!((3500..5500).contains(&held), "held {held} ms");
}

#[test]
fn late_entry_grant_repeats() {
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1]), ms(1002, &[TG], &[1])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].act(t3ms::DO_GROUP, TG, 8000, 0, now);
    // B is busy registering again while the grant goes out, then joins from a Late_Entry repeat
    air.ms[1].act(t3ms::DO_REGISTER, 0, 0, 0, now);
    let mut late = 0;
    let ok = air.until(6000, &mut log.f(), |a| {
        if a.last.tscc_dt == DT_CSBK && Grant::decode(&a.last.tscc_pdu).is_some_and(|g| g.late_entry) {
            late += 1;
        }
        a.ms[1].state() == t3ms::PAYLOAD && late > 0
    });
    assert!(ok, "late {late}, B state {}", air.ms[1].state());
    // they repeat every grant_every_s while the call lasts
    let mut late = 0;
    air.until(4000, &mut log.f(), |a| {
        if a.last.tscc_dt == DT_CSBK && Grant::decode(&a.last.tscc_pdu).is_some_and(|g| g.late_entry && g.target == TG) {
            late += 1;
        }
        false
    });
    assert!((3..=4).contains(&late), "{late}");
}

/// 6.6.2.4.1: the TSCC may send Late_Entry grants "at intervals while the call is active"; the call holds its payload
/// channel through TV_Hangtime (6.6.2, quasi-transmission trunking), so the repeats go on in the hang time (the
/// captured site sends them up to P_CLEAR). None once the call is cleared. A member talking in the hang time is the
/// same call.
#[test]
fn grant_repeats_through_hang_time() {
    let mut s = site_with(|c| c.hang_ms = 6000);
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1]), ms(1002, &[TG], &[1]), ms(1003, &[11], &[1])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].act(t3ms::DO_GROUP, TG, 2000, 0, now);
    assert!(air.until(3000, &mut log.f(), |a| a.site.status().ch[1].r#use == USE_CALL), "A talks");
    assert!(air.until(4000, &mut log.f(), |a| a.site.status().ch[1].r#use == USE_HANG), "hang time");
    let late_entry = |a: &Air<3>| {
        a.last.tscc_dt == DT_CSBK && Grant::decode(&a.last.tscc_pdu).is_some_and(|g| g.late_entry && g.target == TG)
    };
    let mut late = 0;
    air.until(3000, &mut log.f(), |a| {
        late += late_entry(a) as u32;
        false
    });
    assert_eq!(air.site.status().ch[1].r#use, USE_HANG, "still in hang time");
    assert!(late >= 2, "Late_Entry repeats in hang time (every 1.2 s): {late}");
    // C (on the TSCC, the talkgroup not in its list) talks to it in hang time: the same call, the repeats go on
    let now = air.now();
    air.ms[2].act(t3ms::DO_GROUP, TG, 4000, 0, now);
    let mut late = 0;
    let mut hang_held = true;
    air.until(4000, &mut log.f(), |a| {
        hang_held &= a.site.status().ch[1].r#use != USE_CLEAR;
        late += late_entry(a) as u32;
        false
    });
    assert!(hang_held, "C talked in A's hang time, the same call");
    assert!(late >= 2, "repeats with the next talker: {late}");
    // the hang runs out: P_CLEAR, and no repeat after it
    assert!(air.until(15_000, &mut log.f(), |a| a.site.status().ch[1].r#use == USE_CLEAR), "cleared");
    let mut late = 0;
    air.until(3000, &mut log.f(), |a| {
        late += late_entry(a) as u32;
        false
    });
    assert_eq!(late, 0, "no Late_Entry grant after P_CLEAR");
}

#[test]
fn call_timer() {
    let mut s = site_with(|c| c.call_ms = 3000);
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].act(t3ms::DO_GROUP, TG, 20_000, 0, now);
    air.run(6000, &mut log.f());
    let close = log.of(EV_CH_CLOSE);
    assert_eq!((close.len(), close[0].why), (1, CLOSE_TIMER));
    assert_eq!(air.ms[0].state(), t3ms::IDLE);
}

#[test]
fn two_carriers_three_calls() {
    let mut s = site_with(two_carriers);
    let mut log = Log::default();
    let c = [1u16, 2];
    let mut air =
        Air::new(&mut s, [ms(1001, &[9], &c), ms(1002, &[10], &c), ms(1003, &[11], &c), ms(1004, &[12], &c)], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    for (i, tg) in [9u32, 10, 11, 12].iter().enumerate() {
        air.ms[i].act(t3ms::DO_GROUP, *tg, 6000, 0, now + 200 * i as i64);
    }
    assert!(air.until(5000, &mut log.f(), |a| a.ms[3].status().last_reason == R_QUEUED));
    let mut chs: Vec<_> = log.of(EV_CH_OPEN).iter().map(|e| (e.ch, e.chan, e.lcn)).collect();
    chs.sort();
    assert_eq!(chs, [(1, 1, 1), (2, 2, 0), (3, 2, 1)]);
    assert!(air.ms[..3].iter().all(|m| m.state() == t3ms::PAYLOAD));
}

// ── individual calls ──

/// P_AHOY on a voice payload channel in the hang time (t3pcheck.rs): the radio check of a party (6.6.2.3.1.1,
/// table 6.42) and of the talkgroup (table 6.43: the members' P_ACKUs collide, the burst counts), the selective clear
/// of one MS (6.6.2.3.1.8, table 6.44: it answers and leaves, the call goes on) and of the talkgroup (table 6.45:
/// they leave, the call clears).
#[test]
fn payload_radio_check_and_selective_clear() {
    let mut s = site_with(|c| c.hang_ms = 10_000);
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1]), ms(1002, &[TG], &[1]), ms(1003, &[TG], &[1])], T0);
    register_all(&mut air, &mut log);
    let said = |log: &Log, t: &str| log.of(EV_LOG).iter().any(|e| e.text().contains(t));
    let wait = |air: &mut Air<3>, log: &mut Log, t: &str| {
        for _ in 0..80 {
            if said(log, t) {
                return;
            }
            air.run(100, &mut log.f());
        }
        panic!("no \"{t}\"");
    };
    let now = air.now();
    air.ms[0].act(t3ms::DO_GROUP, TG, 1500, 0, now);
    assert!(air.until(5000, &mut log.f(), all_payload));
    assert!(air.site.command("radio_check", 1001, 0), "the caller, on the payload channel");
    assert!(!air.site.command("radio_check", TG, 1), "one at a time");
    wait(&mut air, &mut log, "radio check of 1001 on payload: answered");
    assert_eq!(air.ms[0].payload_ahoys(), (1, 0));
    assert_eq!(air.site.status().ch[1].r#use, USE_HANG, "sent in the hang time, not over the talker");
    assert!(air.site.command("radio_check", TG, 1));
    wait(&mut air, &mut log, &format!("radio check of TG {TG} on payload: answers collided"));
    assert!(air.ms.iter().all(|m| m.payload_ahoys().0 >= 1));
    assert!(air.site.command("clear_ms", 1001, 0));
    wait(&mut air, &mut log, "selective clear of 1001 on payload: answered");
    air.run(200, &mut log.f());
    assert_eq!((air.ms[0].state(), air.ms[0].payload_ahoys().1), (t3ms::IDLE, 1));
    assert!(air.ms[1..].iter().all(|m| m.state() == t3ms::PAYLOAD), "the call goes on for the others");
    assert!(log.of(EV_CH_CLOSE).is_empty());
    assert!(air.site.command("clear_ms", TG, 1));
    wait(&mut air, &mut log, &format!("selective clear of TG {TG} on payload: answers collided"));
    assert!(air.until(3000, &mut log.f(), all_idle));
    air.run(300, &mut log.f());
    assert!(air.ms.iter().all(|m| m.payload_ahoys().1 == 1));
    assert_eq!(log.of(EV_CH_CLOSE)[0].why, CLOSE_CMD);
    // no call: a selective clear is refused, a radio check goes on the TSCC (6.4.12)
    assert!(!air.site.command("clear_ms", 1002, 0));
    assert!(air.site.command("radio_check", 1002, 0));
    wait(&mut air, &mut log, "radio check of 1002: answered");
    assert_eq!(air.ms[1].radio_checks(), 1);
}

#[test]
fn individual_oacsu() {
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1]), ms(1002, &[TG], &[1])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].act(t3ms::DO_PRIVATE, 1002, 2000, 0, now);
    assert!(air.until(5000, &mut log.f(), all_payload));
    assert_eq!(air.ms[1].status().ahoys, 1);
    let open = log.of(EV_CH_OPEN);
    assert_eq!((open[0].src, open[0].dst, open[0].flags & F_GROUP), (1001, 1002, 0));
    // the caller ends with P_MAINT: the channel clears at once, no hang
    assert!(air.until(4000, &mut log.f(), all_idle));
    air.run(300, &mut log.f());
    assert_eq!(log.of(EV_CH_CLOSE)[0].why, CLOSE_DISCON);
    assert_eq!(air.ms[0].status().calls_done, 1);
}

#[test]
fn individual_foacsu() {
    let mut s = site_with(|c| c.foacsu = 1);
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1]), ms(1002, &[TG], &[1])], T0);
    air.ms[1].set_answer(t3ms::ANSWER_FOACSU, 1500);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].act(t3ms::DO_PRIVATE, 1002, 1000, 0, now);
    assert!(air.until(3000, &mut log.f(), |a| a.ms[0].status().last_reason == R_MS_ALERTING));
    assert_eq!(air.ms[0].state(), t3ms::WAITING);
    assert!(air.until(5000, &mut log.f(), all_payload));
    assert!(air.now() - now >= 1500);
    assert!(air.until(5000, &mut log.f(), all_idle));
    assert_eq!(air.ms[0].status().calls_done, 1);
}

#[test]
fn individual_refused_busy_and_no_answer() {
    for (answer, reason) in
        [(t3ms::ANSWER_REFUSE, R_RECIPIENT_REFUSED), (t3ms::ANSWER_BUSY, R_EQUIP_BUSY), (t3ms::ANSWER_SILENT, R_MSAWAY)]
    {
        let mut s = site_with(|_| {});
        let mut log = Log::default();
        let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1]), ms(1002, &[TG], &[1])], T0);
        air.ms[1].set_answer(answer, 0);
        register_all(&mut air, &mut log);
        let now = air.now();
        air.ms[0].act(t3ms::DO_PRIVATE, 1002, 1000, 0, now);
        assert!(air.until(8000, &mut log.f(), |a| a.ms[0].status().calls_failed == 1), "answer {answer}");
        assert_eq!(air.ms[0].status().last_reason, reason, "answer {answer}");
        let tries = if answer == t3ms::ANSWER_SILENT { 3 } else { 1 };
        assert_eq!(air.ms[1].status().ahoys, tries);
        assert!(log.of(EV_CH_OPEN).is_empty());
    }
}

#[test]
fn called_party_busy() {
    let mut s = site_with(two_carriers);
    let mut log = Log::default();
    let c = [1u16, 2];
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &c), ms(1002, &[TG], &c), ms(1003, &[TG], &c)], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].act(t3ms::DO_PRIVATE, 1002, 5000, 0, now);
    assert!(air.until(5000, &mut log.f(), |a| a.ms[1].state() == t3ms::PAYLOAD));
    let now = air.now();
    air.ms[2].act(t3ms::DO_PRIVATE, 1002, 1000, 0, now);
    assert!(air.until(5000, &mut log.f(), |a| a.ms[2].status().calls_failed == 1));
    assert_eq!(air.ms[2].status().last_reason, R_CALLED_PARTY_BUSY);
}

#[test]
fn cancel_while_alerting() {
    let mut s = site_with(|c| c.foacsu = 1);
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1]), ms(1002, &[TG], &[1])], T0);
    air.ms[1].set_answer(t3ms::ANSWER_FOACSU, 8000);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].act(t3ms::DO_PRIVATE, 1002, 1000, 0, now);
    air.ms[0].act(t3ms::DO_CANCEL, 1002, 0, 0, now + 2500);
    assert!(air.until(5000, &mut log.f(), |a| a.ms[0].state() == t3ms::IDLE
        && a.ms[0].status().last_reason == R_MESSAGE_ACCEPTED));
    // the called MS was told (C_AHOY cancel): it never answers, no channel opens
    air.run(10_000, &mut log.f());
    assert!(log.of(EV_CH_OPEN).is_empty());
    assert_eq!(air.ms[1].status().grants, 0);
}

#[test]
fn not_supported() {
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].act(t3ms::DO_PRIVATE, PSTNI, 1000, 0, now);
    assert!(air.until(5000, &mut log.f(), |a| a.ms[0].status().calls_failed == 1));
    assert_eq!(air.ms[0].status().last_reason, R_NOT_SUPPORTED);
    // a service the site doesn't offer (a status message)
    let r = Rand { service_kind: 0b1011, target: 1002, source: 1001, ..Default::default() };
    let ch = air.site.tscc_ch();
    air.site.rx_pdu(ch, DT_CSBK, &r.encode());
    assert!(air.until(500, &mut log.f(), |a| a.last.tscc_dt == DT_CSBK
        && Ack::decode(&a.last.tscc_pdu).is_some_and(|k| k.reason == R_NOT_SUPPORTED && k.target == 1001)));
    assert!(log.refused(R_NOT_SUPPORTED));
}

// ── the network ──

#[test]
fn network_group_in_and_out() {
    let mut s = site_with(|_| {});
    let mut p = Policy::default_open();
    p.wide_tg = IdSet::of(&[(TG, TG)]);
    s.policy_set(&p);
    s.set_link(true);
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1]), ms(1002, &[TG], &[1])], T0);
    register_all(&mut air, &mut log);
    reg_tg(&mut air, &mut log, 1, TG);

    // in: a stream for a talkgroup an MS here needs opens a channel, already talking
    let now = air.now();
    air.site.net_group_in(77, 5555, TG, 0, now);
    assert!(air.until(3000, &mut log.f(), all_payload));
    let open = log.of(EV_CH_OPEN);
    assert_eq!((open[0].stream, open[0].src, open[0].dst), (77, 5555, TG));
    assert_eq!(open[0].flags & (F_NET | F_GROUP), F_NET | F_GROUP);
    assert_eq!(air.site.status().ch[1].r#use, USE_CALL);
    air.site.net_end_in(77);
    assert!(air.until(5000, &mut log.f(), all_idle));
    // a talkgroup no one here needs: dropped
    let now = air.now();
    air.site.net_group_in(78, 5555, 4000, 0, now);
    assert!(log.take(&mut air).of(EV_NET_DROP).iter().any(|e| e.stream == 78 && e.why == WHY_NOT_FOUND));

    // out: a local call on a wide talkgroup is marked wide (the caller sends it on)
    let now = air.now();
    air.ms[0].act(t3ms::DO_GROUP, TG, 1000, 0, now);
    assert!(air.until(3000, &mut log.f(), |a| a.ms[0].state() == t3ms::PAYLOAD));
    let open = *log.of(EV_CH_OPEN).last().unwrap();
    assert_eq!((open.src, open.stream, open.flags & (F_WIDE | F_NET)), (1001, 0, F_WIDE));

    // network voice during local talk is refused; after it, it joins the call
    air.run(500, &mut log.f());
    let now = air.now();
    air.site.net_group_in(79, 5555, TG, 0, now);
    assert!(log.take(&mut air).of(EV_NET_DROP).iter().any(|e| e.stream == 79 && e.why == WHY_BUSY));
    air.run(1500, &mut log.f()); // the talker stopped: hang time
    let now = air.now();
    air.site.net_group_in(80, 5556, TG, 0, now);
    assert!(log.take(&mut air).of(EV_CH_STREAM).iter().any(|e| e.stream == 80 && e.src == 5556));
}

#[test]
fn network_setup_out() {
    let mut s = site_with(|_| {});
    s.set_link(true);
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1])], T0);
    register_all(&mut air, &mut log);
    // accepted
    let now = air.now();
    air.ms[0].act(t3ms::DO_PRIVATE, 7777, 1000, 0, now);
    assert!(air.until(3000, &mut log.f(), |a| a.ms[0].state() == t3ms::WAITING));
    let rq = log.setups(SETUP_REQUEST);
    assert_eq!((rq.len(), rq[0].src, rq[0].dst, rq[0].r#ref), (1, 1001, 7777, 0));
    let now = air.now();
    let m = Setup { op: SETUP_ACCEPT, src: 1001, dst: 7777, stream: 900, r#ref: rq[0].stream, ..Default::default() };
    air.site.net_setup_in(&m, now);
    assert!(air.until(2000, &mut log.f(), |a| a.ms[0].state() == t3ms::PAYLOAD));
    assert_eq!(log.of(EV_CH_OPEN)[0].flags & F_PRIVATE_NET, F_PRIVATE_NET);
    // a second acceptor is released
    let now = air.now();
    air.site.net_setup_in(&Setup { stream: 901, ..m }, now);
    assert!(log.take(&mut air).setups(SETUP_CANCEL).iter().any(|e| e.r#ref == rq[0].stream));
    assert!(air.until(5000, &mut log.f(), all_idle));

    // rejected: busy
    let now = air.now();
    air.ms[0].act(t3ms::DO_PRIVATE, 7778, 1000, 0, now);
    assert!(air.until(3000, &mut log.f(), |a| a.ms[0].state() == t3ms::WAITING));
    let rq = *log.setups(SETUP_REQUEST).last().unwrap();
    let now = air.now();
    let m = Setup { op: SETUP_REJECT, reason: WHY_BUSY, src: 1001, dst: 7778, stream: 902, r#ref: rq.stream, ..Default::default() };
    air.site.net_setup_in(&m, now);
    assert!(air.until(2000, &mut log.f(), |a| a.ms[0].status().last_reason == R_CALLED_PARTY_BUSY));

    // no answer from the network: MSaway, and a cancel
    let now = air.now();
    air.ms[0].act(t3ms::DO_PRIVATE, 7779, 1000, 0, now);
    assert!(air.until(9000, &mut log.f(), |a| a.ms[0].status().last_reason == R_MSAWAY));
    let rq = *log.setups(SETUP_REQUEST).last().unwrap();
    assert!(log.setups(SETUP_CANCEL).iter().any(|e| e.r#ref == rq.stream));

    // not connected: NoregMSaway at once
    air.site.set_link(false);
    let now = air.now();
    air.ms[0].act(t3ms::DO_PRIVATE, 7780, 1000, 0, now);
    assert!(air.until(3000, &mut log.f(), |a| a.ms[0].status().last_reason == R_NOREG_MSAWAY));
}

#[test]
fn network_setup_in() {
    let mut s = site_with(|_| {});
    s.set_link(true);
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.site.net_setup_in(&Setup { op: SETUP_REQUEST, src: 7777, dst: 1001, stream: 500, ..Default::default() }, now);
    assert!(air.until(3000, &mut log.f(), |a| a.ms[0].state() == t3ms::PAYLOAD));
    let acc = log.setups(SETUP_ACCEPT);
    assert_eq!((acc.len(), acc[0].r#ref, acc[0].src, acc[0].dst), (1, 500, 7777, 1001));
    let open = log.of(EV_CH_OPEN);
    assert_eq!((open[0].src, open[0].dst, open[0].flags & F_NET), (7777, 1001, F_NET));
    // the caller's voice arrives
    let now = air.now();
    air.site.net_private_in(600, 7777, 1001, now);
    assert!(log.take(&mut air).of(EV_CH_STREAM).iter().any(|e| e.stream == 600));
    assert_eq!(air.site.status().ch[1].r#use, USE_CALL);
    air.site.net_end_in(600);
    assert!(air.until(6000, &mut log.f(), all_idle));

    // an MS not here: not found
    let now = air.now();
    air.site.net_setup_in(&Setup { op: SETUP_REQUEST, src: 7777, dst: 4242, stream: 501, ..Default::default() }, now);
    assert!(log.take(&mut air).setups(SETUP_REJECT).iter().any(|e| e.r#ref == 501 && e.reason == WHY_NOT_FOUND));
    // the network caller gives up before the answer: no reject, no more C_AHOY
    air.ms[0].set_answer(t3ms::ANSWER_SILENT, 0);
    let now = air.now();
    air.site.net_setup_in(&Setup { op: SETUP_REQUEST, src: 7777, dst: 1001, stream: 502, ..Default::default() }, now);
    air.run(500, &mut log.f());
    let now = air.now();
    air.site.net_setup_in(&Setup { op: SETUP_CANCEL, src: 7777, dst: 1001, stream: 503, r#ref: 502, ..Default::default() }, now);
    air.run(5000, &mut log.f());
    assert_eq!(air.ms[0].status().ahoys, 2); // the first call's, and one before the cancel
    assert!(!log.setups(SETUP_REJECT).iter().any(|e| e.r#ref == 502));
}

#[test]
fn tier2_caller_implicit_setup() {
    let mut s = site_with(|_| {});
    s.set_link(true);
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.site.net_private_in(700, 8888, 1001, now);
    assert!(air.until(3000, &mut log.f(), |a| a.ms[0].state() == t3ms::PAYLOAD));
    let open = log.of(EV_CH_OPEN);
    assert_eq!((open[0].stream, open[0].src, open[0].dst), (700, 8888, 1001));
    assert_eq!(air.site.status().ch[1].r#use, USE_CALL);
    assert!(log.setups(SETUP_ACCEPT).is_empty()); // no setup to answer
    // to an MS that isn't here: dropped
    let now = air.now();
    air.site.net_private_in(701, 8888, 4242, now);
    assert!(log.take(&mut air).of(EV_NET_DROP).iter().any(|e| e.stream == 701 && e.why == WHY_NOT_FOUND));
}

// ── state and provisioning ──

#[test]
fn records_reload() {
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let rec = {
        let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1])], T0);
        reg_tg(&mut air, &mut log, 0, TG);
        air.site.record(1001).unwrap()
    };
    // a restarted site loads it: the MS calls without registering again
    let mut s2 = site_with(|_| {});
    assert!(s2.load_record(&rec));
    let mut subs = [0u32; 4];
    let n = s2.subs(&mut subs);
    assert_eq!(&subs[..n], &[TG]);
    let mut air = Air::new(&mut s2, [ms(1001, &[TG], &[1])], T0 + 1000);
    let now = air.now();
    air.ms[0].act(t3ms::DO_GROUP, TG, 1000, 0, now + 300);
    assert!(air.until(3000, &mut log.f(), |a| a.ms[0].state() == t3ms::PAYLOAD));
    // an expired record isn't loaded
    assert!(!air.site.load_record(&Record { id: 1002, exp_ms: T0 - 1, ..rec }));
    // resend_records writes every record again
    let mut l2 = Log::default();
    assert!(air.site.command("resend_records", 0, 0));
    assert!(l2.take(&mut air).of(EV_RECORD).iter().any(|e| e.src == 1001));
    // records expire
    let mut s3 = site_with(|c| c.reg_hold_s = 60);
    assert!(s3.load_record(&Record { exp_ms: T0 + 60_000, ..rec }));
    let mut air = Air::new(&mut s3, [], T0);
    air.run(65_000, &mut log.f());
    assert!(air.site.record(1001).is_some(), "kept while a registration is demanded of it (6.4.5.0)");
    air.run(30_000, &mut log.f());
    assert!(air.site.record(1001).is_none(), "no answer: gone");
}

#[test]
fn provisioning_changes() {
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1])], T0);
    register_all(&mut air, &mut log);
    // a talkgroup not allowed
    let mut p = Policy::default_open();
    p.tg = IdSet::of(&[(1, 9)]);
    air.site.policy_set(&p);
    let now = air.now();
    air.ms[0].act(t3ms::DO_GROUP, 10, 1000, 0, now);
    assert!(air.until(3000, &mut log.f(), |a| a.ms[0].status().last_reason == R_GROUP_NOT_ALLOWED));
    // no individual calls for this MS
    let mp = MsPolicy { enabled: 1, private_ok: 0, ..Default::default() };
    assert!(air.site.set_ms_policy(1001, Some(&mp)));
    let now = air.now();
    air.ms[0].act(t3ms::DO_PRIVATE, 1002, 1000, 0, now);
    assert!(air.until(3000, &mut log.f(), |a| a.ms[0].status().last_reason == R_PERM_USER_REFUSED));
    // provisioned talkgroups count for the subscriptions
    let mp = MsPolicy { enabled: 1, private_ok: 1, ntg: 1, tg: [44, 0, 0, 0, 0, 0, 0, 0], ..Default::default() };
    air.site.set_ms_policy(1001, Some(&mp));
    let mut subs = [0u32; 8];
    let n = air.site.subs(&mut subs);
    assert!(subs[..n].contains(&44));
    // disabled: the record goes, calls are refused
    let mp = MsPolicy { enabled: 0, ..Default::default() };
    air.site.set_ms_policy(1001, Some(&mp));
    assert!(air.site.record(1001).is_none());
    let now = air.now();
    air.ms[0].act(t3ms::DO_GROUP, TG, 1000, 0, now);
    assert!(air.until(3000, &mut log.f(), |a| a.ms[0].status().last_reason == R_PERM_USER_REFUSED));
    // the entry removed: back to the defaults
    assert!(air.site.set_ms_policy(1001, None));
    reg_tg(&mut air, &mut log, 0, TG);
    // where/: registered at another site since
    let at = air.site.record(1001).unwrap().at_ms + 1;
    air.site.where_in(1001, false, at);
    assert!(air.site.record(1001).is_none());
}

#[test]
fn link_loss() {
    let mut s = site_with(|_| {});
    s.set_link(true);
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1]), ms(1002, &[TG], &[1])], T0);
    register_all(&mut air, &mut log);
    let mut seen = None;
    air.until(500, &mut log.f(), |a| {
        seen = last_aloha(a).map(|x| x.active_connection).or(seen);
        false
    });
    assert_eq!(seen, Some(true));
    let now = air.now();
    air.ms[0].act(t3ms::DO_PRIVATE, 7777, 1000, 0, now);
    assert!(air.until(2000, &mut log.f(), |a| a.ms[0].state() == t3ms::WAITING));
    air.site.set_link(false);
    assert!(air.until(2000, &mut log.f(), |a| a.ms[0].status().last_reason == R_MSAWAY));
    let mut seen = None;
    air.until(500, &mut log.f(), |a| {
        seen = last_aloha(a).map(|x| x.active_connection).or(seen);
        false
    });
    assert_eq!(seen, Some(false));
    // local service goes on
    let now = air.now();
    air.ms[0].act(t3ms::DO_GROUP, TG, 1000, 0, now);
    assert!(air.until(3000, &mut log.f(), all_payload));
}

#[test]
fn broadcasts() {
    let mut s = site_with(|c| {
        two_carriers(c);
        c.clock_ok = 1;
        c.utc_offset_min = -360;
    });
    let n = SysCode { model: MODEL_SMALL, net: 1, site: 2, par: 3 }.encode();
    s.set_neighbors(&[Neighbor { syscode: n, chan: 7, active_known: 1, active: 1, ..Default::default() }]);
    let (mut adj, mut freq, mut time, mut gen) = (None, Vec::new(), None, false);
    let mut prev_hdr = false;
    for k in 0..2000i64 {
        let o = s.slot(k, T0 + k * 30);
        if k & 1 == 1 {
            continue;
        }
        if o.tscc_dt == DT_MBC_CONT {
            assert!(prev_hdr);
            freq.push(BcAp::decode(&o.tscc_pdu).unwrap());
        }
        prev_hdr = o.tscc_dt == DT_MBC_HDR;
        if o.tscc_dt == DT_CSBK {
            if let Some(b) = Bcast::decode(&o.tscc_pdu) {
                match b.ann {
                    BC_ADJACENT_SITE => adj = Some(b),
                    BC_LOCAL_TIME => time = time.or(Some(b)),
                    BC_GEN_SITE_PARAMS => gen = true,
                    _ => {}
                }
            }
        }
    }
    let adj = adj.unwrap();
    assert_eq!((adj.parms1, adj.parms2 & 0xFFF, adj.parms2 >> 22), (n >> 2, 7, 0b11));
    assert!(freq.contains(&BcAp { chan: 2, tx_hz: 431_000_000, rx_hz: 438_600_000 }));
    assert!(freq.iter().any(|f| f.chan == 1 && f.rx_hz == 438_750_000));
    // 15:04 local, 6 h behind UTC
    let t = time.unwrap();
    assert_eq!((t.parms2 >> 19, (t.parms2 >> 13) & 63, t.parms1 & 0x1F), (15, 4, (6 << 1) | 1));
    assert!(gen);
}

#[test]
fn withdraw_and_restore() {
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].act(t3ms::DO_GROUP, TG, 10_000, 0, now);
    assert!(air.until(3000, &mut log.f(), |a| a.ms[0].state() == t3ms::PAYLOAD));
    assert!(air.site.command("withdraw", 0, 0));
    air.run(1000, &mut log.f());
    assert_eq!(air.last.tscc_dt, 0);
    assert_eq!(air.site.status().withdrawn, 1);
    assert!(log.of(EV_CH_CLOSE).iter().any(|e| e.why == CLOSE_WITHDRAW));
    assert_eq!(air.ms[0].state(), t3ms::IDLE);
    assert!(air.site.command("restore", 0, 0));
    assert!(air.until(200, &mut log.f(), |a| a.last.tscc_dt == DT_CSBK));
    assert!(!air.site.command("nonsense", 0, 0));
}

#[test]
fn reconfigure_keeps_calls() {
    let mut s = site_with(two_carriers);
    let mut log = Log::default();
    let c = [1u16, 2];
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &c)], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].act(t3ms::DO_GROUP, TG, 5000, 0, now);
    assert!(air.until(3000, &mut log.f(), |a| a.ms[0].state() == t3ms::PAYLOAD));
    air.run(300, &mut log.f());
    let mut cfg = *air.site.cfg();
    cfg.hang_ms = 2000;
    cfg.carriers[1].enabled = 0;
    let mut err = [0u8; 64];
    assert!(air.site.reconfigure(&cfg, &mut err));
    assert_eq!(air.site.status().ch[1].r#use, USE_CALL);
    assert_eq!(air.site.status().ch[2].r#use, USE_OFF);
    cfg.tp_ms = 1;
    assert!(!air.site.reconfigure(&cfg, &mut err));
    assert!(std::str::from_utf8(&err).unwrap().starts_with("timers.tp_s"));
}

#[test]
fn an_expiring_record_demands_a_registration() {
    // 6.4.5.0: before the site drops a record it demands a registration of that MS (C_BCAST MassReg, Mask 24, its
    // address): an MS on the TSCC answers and keeps its record, with its talkgroup; one that's gone loses it
    let mut s = site_with(|c| c.reg_hold_s = 60);
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1])], T0);
    let now = air.now();
    air.ms[0].act(t3ms::DO_REGISTER, TG, 0, 0, now);
    assert!(air.until(5000, &mut log.f(), |a| a.site.record(1001).is_some()));
    let rec = air.site.record(1001).unwrap();
    assert_eq!(rec.tgs(), &[TG][..]);
    assert!(air.site.load_record(&Record { id: 1002, ..rec }));
    air.run(rec.exp_ms - air.now() + 3000, &mut log.f());
    assert_eq!(air.ms[0].mass_regs(), 1, "the demand reached 1001");
    let r = air.site.record(1001).unwrap();
    assert!(r.exp_ms > rec.exp_ms + 55_000, "1001 registered again");
    assert_eq!(r.tgs(), &[TG][..]);
    assert!(air.site.record(1002).is_some(), "1002 waits for its answer");
    air.run(40_000, &mut log.f());
    assert!(air.site.record(1002).is_none(), "1002 never answered");
    assert!(air.site.record(1001).is_some());
}

#[test]
fn an_ms_told_it_isnt_registered_registers() {
    // MS_Not_Registered (7.2.6): the MS must register (6.4.4.1.0); the site demands it too (6.4.5.0). Once
    // registered, its call goes.
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1])], T0);
    let now = air.now();
    air.ms[0].act(t3ms::DO_GROUP, TG, 1000, 0, now);
    assert!(air.until(5000, &mut log.f(), |a| a.ms[0].status().last_reason == R_MS_NOT_REGISTERED));
    assert!(air.until(5000, &mut log.f(), |a| a.site.record(1001).is_some_and(|r| r.how == HOW_REGISTER)));
    let regs_of = |log: &Log| {
        log.of(EV_ACTIVITY).iter().filter(|e| e.kind == ACT_REG && e.op == REG_REGISTER && e.src == 1001).count()
    };
    let regs = regs_of(&log);
    air.run(3000, &mut log.f());
    let after = regs_of(&log);
    assert_eq!((regs, after), (1, 1), "one registration answers both");
    let now = air.now();
    air.ms[0].act(t3ms::DO_GROUP, TG, 1000, 0, now);
    assert!(air.until(5000, &mut log.f(), |a| a.ms[0].state() == t3ms::PAYLOAD));
}
