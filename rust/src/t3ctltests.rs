// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! Scenario tests of the control channel features (docs/spec/fw/tier3.md section 12): emergency pre-emption, N
//! carriers, the broadcasts, power save, mass registration, the composite TSCC and duplex calls. The same air and
//! MS simulator as t3tests.rs; the helpers are this file's own.

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

/// n carriers, chan 1..=n, the TSCC on chan 1 LCN 0.
fn carriers(c: &mut Cfg, n: usize) {
    c.ncarriers = n as u8;
    for k in 1..n {
        let off = k as u32 * 25_000;
        c.carriers[k] = Carrier { tx_hz: 438_600_000 + off, rx_hz: 431_000_000 + off, chan: k as u16 + 1, tscc: 0, enabled: 1 };
    }
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
}

fn register_all<const N: usize>(air: &mut Air<N>, log: &mut Log) {
    let now = air.now();
    for (i, m) in air.ms.iter_mut().enumerate() {
        m.act(t3ms::DO_REGISTER, 0, 0, 0, now + 100 * i as i64);
    }
    assert!(air.until(30_000, &mut log.f(), |a| a.ms.iter().all(|m| m.status().registered != 0)), "registration");
}

// ── emergency pre-emption (361-4 6.4.14.3.1, 6.4.15) ──

/// One carrier: the only payload channel is the TSCC carrier's LCN 1, so the RC goes in the TSCC's slot.
#[test]
fn preemption_dekeys_the_talker_on_the_tscc_carrier() {
    let mut s = site_with(|c| c.preempt = 1);
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1]), ms(1002, &[TG], &[1]), ms(1003, &[11], &[1])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].act(t3ms::DO_GROUP, TG, 30_000, 0, now);
    assert!(air.until(5000, &mut log.f(), |a| a.site.status().ch[1].r#use == USE_CALL));
    let now = air.now();
    air.ms[2].act(t3ms::DO_GROUP, 11, 2000, t3ms::F_EMERGENCY, now);
    // the emergency is queued, the talker hears RC cease in the TSCC's slot, stops, P_CLEAR, then the grant
    let mut rc_slots = 0;
    let mut tscc_gave_way = false;
    let ok = air.until(10_000, &mut log.f(), |a| {
        if a.last.ch[0].rc != 0 {
            assert_eq!(a.last.ch[0].rc, RC_SEND | crate::t3rc::RC_CEASE);
            rc_slots += 1;
            tscc_gave_way |= a.last.tscc_dt == 0;
        }
        a.ms[2].state() == t3ms::PAYLOAD
    });
    assert!(ok, "the emergency call got the channel");
    assert!(rc_slots >= 1 && tscc_gave_way, "RC in the TSCC's slot: {rc_slots}");
    assert_eq!(air.ms[0].ceased(), 1, "the talker ceased");
    assert_eq!(air.ms[2].status().last_reason, R_QUEUED);
    let close = log.of(EV_CH_CLOSE);
    assert_eq!((close.len(), close[0].why, close[0].dst), (1, CLOSE_PREEMPT, TG));
    let opens = log.of(EV_CH_OPEN);
    assert_eq!(opens.iter().map(|e| (e.ch, e.dst)).collect::<Vec<_>>(), [(1, TG), (1, 11)]);
    assert!(opens[1].flags & F_EMERGENCY != 0);
    // the pre-empted parties went back to the TSCC on P_CLEAR
    assert!(air.ms[..2].iter().all(|m| m.status().clears == 1));
}

/// Two carriers, all three payload channels busy: the call in hang time goes first, without an RC.
#[test]
fn preemption_takes_a_hanging_call_first() {
    let mut s = site_with(|c| {
        carriers(c, 2);
        c.preempt = 1;
        c.hang_ms = 20_000;
    });
    let mut log = Log::default();
    let ch = [1u16, 2];
    let mut air =
        Air::new(&mut s, [ms(1001, &[9], &ch), ms(1002, &[10], &ch), ms(1003, &[11], &ch), ms(1004, &[12], &ch)], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].act(t3ms::DO_GROUP, 9, 30_000, 0, now);
    air.ms[1].act(t3ms::DO_GROUP, 10, 1000, 0, now + 300); // talks 1 s, then hangs 20 s
    air.ms[2].act(t3ms::DO_GROUP, 11, 30_000, 0, now + 600);
    assert!(air.until(8000, &mut log.f(), |a| {
        let st = a.site.status();
        st.ch.iter().filter(|c| c.r#use == USE_CALL).count() == 2 && st.ch.iter().any(|c| c.r#use == USE_HANG && c.dst == 10)
    }));
    let now = air.now();
    air.ms[3].act(t3ms::DO_GROUP, 12, 2000, t3ms::F_EMERGENCY, now);
    let mut rc = 0;
    assert!(air.until(8000, &mut log.f(), |a| {
        rc += a.last.ch.iter().filter(|c| c.rc != 0).count();
        a.ms[3].state() == t3ms::PAYLOAD
    }));
    assert_eq!(rc, 0, "nobody to de-key");
    let close = log.of(EV_CH_CLOSE);
    assert_eq!((close.len(), close[0].why, close[0].dst), (1, CLOSE_PREEMPT, 10));
    assert!(air.ms[0].ceased() + air.ms[2].ceased() == 0, "the talkers go on");
    assert_eq!(air.ms[0].state(), t3ms::PAYLOAD);
}

/// Two carriers, three talkers: the RC goes on the alternate slot of a payload carrier (not the TSCC's).
#[test]
fn preemption_rc_on_the_payload_carrier() {
    let mut s = site_with(|c| {
        carriers(c, 2);
        c.preempt = 1;
    });
    let mut log = Log::default();
    let ch = [1u16, 2];
    let mut air =
        Air::new(&mut s, [ms(1001, &[9], &ch), ms(1002, &[10], &ch), ms(1003, &[11], &ch), ms(1004, &[12], &ch)], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    for i in 0..3 {
        air.ms[i].act(t3ms::DO_GROUP, 9 + i as u32, 30_000, 0, now + 300 * i as i64);
    }
    assert!(air.until(8000, &mut log.f(), |a| a.site.status().ch.iter().filter(|c| c.r#use == USE_CALL).count() == 3));
    let now = air.now();
    air.ms[3].act(t3ms::DO_GROUP, 12, 2000, t3ms::F_EMERGENCY, now);
    let mut rc_ch = None;
    assert!(air.until(8000, &mut log.f(), |a| {
        if let Some(i) = a.last.ch.iter().position(|c| c.rc != 0) {
            rc_ch = Some(i);
        }
        a.ms[3].state() == t3ms::PAYLOAD
    }));
    // the oldest call whose talker hears a payload slot: tg 10 on chan 2 LCN 0, its RC on chan 2 LCN 1
    let close = log.of(EV_CH_CLOSE);
    assert_eq!((close.len(), close[0].ch, close[0].dst, close[0].why), (1, 2, 10, CLOSE_PREEMPT));
    assert_eq!(rc_ch, Some(3));
    assert_eq!(air.ms[1].ceased(), 1);
    let open = log.of(EV_CH_OPEN);
    assert_eq!((open[3].ch, open[3].dst), (2, 12));
}

/// Off (the default), an emergency waits in the queue as before; an emergency call is never pre-empted.
#[test]
fn preemption_off_and_emergency_calls_kept() {
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1]), ms(1002, &[11], &[1])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].act(t3ms::DO_GROUP, TG, 8000, 0, now);
    air.ms[1].act(t3ms::DO_GROUP, 11, 1000, t3ms::F_EMERGENCY, now + 1500);
    air.run(5000, &mut log.f());
    assert!(log.of(EV_CH_CLOSE).is_empty());
    assert_eq!(air.ms[1].status().last_reason, R_QUEUED);

    let mut s = site_with(|c| c.preempt = 1);
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1]), ms(1002, &[11], &[1])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].act(t3ms::DO_GROUP, TG, 8000, t3ms::F_EMERGENCY, now);
    air.ms[1].act(t3ms::DO_GROUP, 11, 1000, t3ms::F_EMERGENCY, now + 1500);
    air.run(5000, &mut log.f());
    assert!(log.of(EV_CH_CLOSE).is_empty(), "an emergency call isn't pre-empted");
    assert_eq!(air.ms[0].ceased(), 0);
}

/// An emergency request on a talkgroup whose call is open makes it an emergency call: the grants carry Emergency
/// and the emergency call timer replaces the normal one.
#[test]
fn emergency_upgrades_an_open_group_call() {
    let mut s = site_with(|c| {
        c.call_ms = 4000;
        c.emergency_call_ms = 60_000;
    });
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1]), ms(1002, &[10], &[1])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].act(t3ms::DO_GROUP, TG, 3000, 0, now);
    air.ms[1].act(t3ms::DO_GROUP, TG, 500, t3ms::F_EMERGENCY, now + 1000);
    air.run(12_000, &mut log.f());
    let grants: Vec<_> = log.of(EV_ACTIVITY).into_iter().filter(|e| e.kind == ACT_GRANT).collect();
    assert!(grants.iter().any(|e| e.src == 1002 && e.flags & (F_LATE | F_EMERGENCY) == F_LATE | F_EMERGENCY));
    let close = log.of(EV_CH_CLOSE);
    assert_eq!((close.len(), close[0].why), (1, CLOSE_HANG), "the normal call timer (4 s) no longer applies");
}

/// An emergency past a full queue when pre-emption is on.
#[test]
fn emergency_past_a_full_queue() {
    let mut s = site_with(|c| {
        c.preempt = 1;
        c.queue_max = 1;
    });
    let mut log = Log::default();
    let mut air =
        Air::new(&mut s, [ms(1001, &[TG], &[1]), ms(1002, &[10], &[1]), ms(1003, &[11], &[1])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].act(t3ms::DO_GROUP, TG, 20_000, 0, now);
    air.ms[1].act(t3ms::DO_GROUP, 10, 1000, 0, now + 1000);
    assert!(air.until(5000, &mut log.f(), |a| a.ms[1].status().last_reason == R_QUEUED));
    let now = air.now();
    air.ms[2].act(t3ms::DO_GROUP, 11, 1000, t3ms::F_EMERGENCY, now);
    assert!(air.until(8000, &mut log.f(), |a| a.ms[2].state() == t3ms::PAYLOAD));
    // and the queued normal call is still waiting behind it
    assert_eq!(air.ms[1].state(), t3ms::WAITING);
}
