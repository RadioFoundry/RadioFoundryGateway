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
    assert_eq!((opens[0].svc, opens[1].svc), (0, 0x80), "361-2 table 7.11: the LC's Emergency option");
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
    // 361-2 table 7.11: an LC the site makes for the call from then on carries Emergency (the radio layer takes it
    // from the grant's svc)
    assert_eq!(log.of(EV_CH_OPEN)[0].svc, 0, "a normal call at first");
    assert!(grants.iter().any(|e| e.src == 1002 && e.svc == 0x80));
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

// ── status delivery and the emergency alarm (361-4 6.6.6.1, 6.6.6.3) ──

fn statuses(log: &Log) -> Vec<(u8, u32, u32, u8, bool)> {
    let acts = log.of(EV_ACTIVITY);
    acts.iter().filter(|e| e.kind == ACT_STATUS).map(|e| (e.op, e.src, e.dst, e.reason, e.flags & F_GROUP != 0)).collect()
}

/// The emergency alarm to a talkgroup: its members store it, nobody answers, the caller gets C_ACKD.
#[test]
fn emergency_alarm_to_a_talkgroup() {
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1]), ms(1002, &[TG], &[1]), ms(1003, &[10], &[1])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].act_status(STATUS_EMERGENCY_ALARM, true, TG, now);
    assert!(air.until(3000, &mut log.f(), |a| a.ms[0].status().calls_done == 1));
    air.run(500, &mut log.f());
    assert_eq!(air.ms[0].status().last_reason, R_MESSAGE_ACCEPTED);
    assert_eq!(air.ms[1].statuses(), (1, (1001, TG, STATUS_EMERGENCY_ALARM)));
    assert_eq!(air.ms[2].statuses().0, 0, "another talkgroup's member");
    assert_eq!(statuses(&log), [(STATUS_EMERGENCY_ALARM, 1001, TG, R_MESSAGE_ACCEPTED, true)]);
    assert!(log.of(EV_LOG).iter().any(|e| e.text().contains("emergency alarm from 1001")));
    // and the cancel to the TSCC: a system message, taken there
    let now = air.now();
    air.ms[0].act_status(STATUS_CANCEL_ALARM, false, TSI, now);
    assert!(air.until(3000, &mut log.f(), |a| a.ms[0].status().calls_done == 2));
    assert_eq!(statuses(&log)[1], (STATUS_CANCEL_ALARM, 1001, TSI, R_MESSAGE_ACCEPTED, false));
}

/// A status to an MS: C_AHOY, its C_ACKU mirrored to the caller; refused, its C_NACKU mirrored; silent, MSaway.
#[test]
fn status_to_an_ms() {
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1]), ms(1002, &[TG], &[1])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].act_status(42, false, 1002, now);
    assert!(air.until(3000, &mut log.f(), |a| a.ms[0].status().calls_done == 1));
    assert_eq!(air.ms[0].status().last_reason, R_MS_ACCEPTED);
    assert_eq!(air.ms[1].statuses(), (1, (1001, 1002, 42)));
    air.ms[1].set_answer(t3ms::ANSWER_REFUSE, 0);
    let now = air.now();
    air.ms[0].act_status(7, false, 1002, now);
    assert!(air.until(3000, &mut log.f(), |a| a.ms[0].status().calls_failed == 1));
    assert_eq!(air.ms[0].status().last_reason, R_RECIPIENT_REFUSED);
    air.ms[1].set_answer(t3ms::ANSWER_SILENT, 0);
    let now = air.now();
    air.ms[0].act_status(8, false, 1002, now);
    assert!(air.until(8000, &mut log.f(), |a| a.ms[0].status().calls_failed == 2));
    assert_eq!(air.ms[0].status().last_reason, R_MSAWAY);
    assert_eq!(air.ms[1].statuses().0, 3, "each C_AHOY heard");
    let st = statuses(&log);
    assert_eq!(st.iter().map(|s| (s.0, s.3)).collect::<Vec<_>>(), [(42, R_MS_ACCEPTED), (7, R_RECIPIENT_REFUSED), (8, R_MSAWAY)]);
}

/// A status to an MS in a call waits (C_QACKD) until it's back on the TSCC; polling and unknown MSs are refused.
#[test]
fn status_stored_while_the_called_ms_talks() {
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1]), ms(1002, &[10], &[1]), ms(1003, &[10], &[1])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[1].act(t3ms::DO_GROUP, 10, 3000, 0, now);
    assert!(air.until(3000, &mut log.f(), |a| a.ms[1].state() == t3ms::PAYLOAD));
    let now = air.now();
    air.ms[0].act_status(STATUS_EMERGENCY_ALARM, false, 1002, now);
    assert!(air.until(3000, &mut log.f(), |a| a.ms[0].status().last_reason == R_QUEUED_BUSY));
    assert_eq!(air.ms[1].statuses().0, 0);
    assert!(air.until(15_000, &mut log.f(), |a| a.ms[0].status().calls_done == 1));
    assert_eq!(air.ms[0].status().last_reason, R_MS_ACCEPTED);
    assert_eq!(air.ms[1].statuses(), (1, (1001, 1002, STATUS_EMERGENCY_ALARM)));
    // a poll (127) is answered by the polled MS (6.6.6.2); an MS not registered here, with no network to look in,
    // is NoregMSaway_Refused (table 7.29: not in radio contact and not registered)
    let now = air.now();
    air.ms[0].act_status(127, false, 1002, now);
    assert!(air.until(3000, &mut log.f(), |a| a.ms[0].status().calls_done == 2));
    assert_eq!(air.ms[0].status().last_reason, crate::t3udt::R_POLL_MS);
    let now = air.now();
    air.ms[0].act_status(5, false, 1999, now);
    assert!(air.until(3000, &mut log.f(), |a| a.ms[0].status().calls_failed == 1));
    assert_eq!(air.ms[0].status().last_reason, R_NOREG_MSAWAY);
}

#[test]
fn status_pdus_round_trip() {
    for st in [0u8, 1, 42, 99, 124, 126] {
        for group in [false, true] {
            let r = Rand::decode(&status_rand(st, group, 9, 1001).encode()).unwrap();
            assert_eq!((r.service_kind, r.service_options & 0x40 != 0, status_of(&r)), (SK_STATUS, group, st));
            let a = Ahoy::decode(&status_ahoy(st, group, 9, 1001).encode()).unwrap();
            assert_eq!((a.service_kind, a.group, ahoy_status(&a)), (SK_STATUS, group, st));
        }
    }
    // table 6.57: 126 = 111 1110b: STATUS(5) 11111 in Service_Options' low bits, STATUS(2) 10
    let r = status_rand(126, false, 9, 1001);
    assert_eq!((r.service_options, r.dep), (0x1F, 0b10));
}

// ── N carriers ──

/// Three carriers, five payload channels: five calls fill them, a sixth queues and gets the first freed; a carrier
/// out of service isn't granted.
#[test]
fn three_carriers_fill_queue_and_free() {
    let mut s = site_with(|c| carriers(c, 3));
    let mut log = Log::default();
    let ch = [1u16, 2, 3];
    let mut air = Air::new(
        &mut s,
        [ms(1001, &[10], &ch), ms(1002, &[11], &ch), ms(1003, &[12], &ch), ms(1004, &[13], &ch), ms(1005, &[14], &ch), ms(1006, &[15], &ch)],
        T0,
    );
    register_all(&mut air, &mut log);
    let now = air.now();
    for i in 0..5 {
        let talk = if i == 2 { 2000 } else { 30_000 };
        air.ms[i].act(t3ms::DO_GROUP, 10 + i as u32, talk, 0, now + 200 * i as i64);
    }
    air.ms[5].act(t3ms::DO_GROUP, 15, 1000, 0, now + 1500);
    assert!(air.until(4000, &mut log.f(), |a| a.ms[5].status().last_reason == R_QUEUED));
    let busy: Vec<_> = air.ms[..5].iter().map(|m| (m.state(), m.ch())).collect();
    assert!(busy.iter().all(|&(st, _)| st == t3ms::PAYLOAD), "{busy:?}");
    let mut chs: Vec<_> = busy.iter().map(|&(_, c)| c).collect();
    chs.sort();
    assert_eq!(chs, [1, 2, 3, 4, 5]);
    // the 2 s call ends (hang 3 s): the queued one takes its channel
    let freed = air.ms[2].ch();
    assert!(air.until(10_000, &mut log.f(), |a| a.ms[5].state() == t3ms::PAYLOAD));
    assert_eq!(air.ms[5].ch(), freed);

    // carrier 3 out of service: only channels 1-3
    let mut s = site_with(|c| {
        carriers(c, 3);
        c.carriers[2].enabled = 0;
    });
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[10], &ch), ms(1002, &[11], &ch), ms(1003, &[12], &ch), ms(1004, &[13], &ch)], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    for i in 0..4 {
        air.ms[i].act(t3ms::DO_GROUP, 10 + i as u32, 30_000, 0, now + 200 * i as i64);
    }
    assert!(air.until(4000, &mut log.f(), |a| a.ms[3].status().last_reason == R_QUEUED));
    let mut chs: Vec<_> = air.ms[..3].iter().map(|m| m.ch()).collect();
    chs.sort();
    assert_eq!(chs, [1, 2, 3]);
}

/// Four carriers: the Chan_Freq broadcast names each carrier's frequencies in turn.
#[test]
fn four_carriers_chan_freq_each() {
    let mut s = site_with(|c| {
        carriers(c, 4);
        c.chan_freq_ms = 2000;
    });
    let mut seen = [false; 5];
    let mut air = Air::new(&mut s, [ms(1001, &[10], &[1, 2, 3, 4])], T0);
    let mut hdr = false;
    for _ in 0..400 {
        air.step(&mut |_| {});
        let o = air.last;
        if o.tscc_dt == DT_MBC_HDR {
            hdr = Bcast::decode(&o.tscc_pdu).is_some_and(|b| b.ann == BC_CHAN_FREQ);
        } else if o.tscc_dt == DT_MBC_CONT && hdr {
            let ap = BcAp::decode(&o.tscc_pdu).unwrap();
            assert_eq!(ap.rx_hz, if ap.chan == 1 { 438_750_000 } else { 438_600_000 + (ap.chan as u32 - 1) * 25_000 }, "{ap:?}");
            seen[ap.chan as usize] = true;
        }
    }
    assert_eq!(seen, [false, true, true, true, true]);
}

// ── C_BCAST announcements, mass re-registration, power save (6.7.1, 6.4.5, 6.4.7) ──

fn regs(log: &Log, from: usize) -> Vec<u32> {
    log.0[from..].iter().filter(|e| e.r#type == EV_ACTIVITY && e.kind == ACT_REG && e.op == REG_REGISTER).map(|e| e.src).collect()
}

/// Mass re-registration (6.4.5): every MS re-registers within the window; one MS by its address; a Reg_Window 0
/// cancels the pending ones.
#[test]
fn mass_reregistration() {
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1]), ms(1002, &[TG], &[1]), ms(1003, &[10], &[1]), ms(1004, &[10], &[1])], T0);
    register_all(&mut air, &mut log);
    let mark = log.0.len();
    assert!(air.site.command("mass_reg", 10, 0));
    air.run(14_000, &mut log.f());
    let mut r = regs(&log, mark);
    r.sort();
    assert_eq!(r, [1001, 1002, 1003, 1004], "each once in the 10 s window");
    assert!(air.ms.iter().all(|m| m.mass_regs() == 1 && m.status().registered != 0));
    assert!(air.ms.iter().all(|m| m.bcasts(BC_MASS_REG) >= 2), "repeated, the repeats ignored");
    // one MS (Mask 24)
    let mark = log.0.len();
    assert!(air.site.command("mass_reg", 2, 1003));
    air.run(5000, &mut log.f());
    assert_eq!(regs(&log, mark), [1003]);
    // a long window, then cancelled: nobody registers
    let mark = log.0.len();
    assert!(air.site.command("mass_reg", 1000, 0));
    air.run(4000, &mut log.f());
    assert!(air.site.command("mass_reg", 0, 0));
    air.run(5000, &mut log.f());
    let early = regs(&log, mark).len();
    air.run(30_000, &mut log.f());
    assert_eq!(regs(&log, mark).len(), early, "none after the cancel");
}

/// At start (services.mass_reg_at_start_s): the MSs a restarted site no longer knows register again.
#[test]
fn mass_reregistration_at_start() {
    let mut s = site_with(|c| c.mass_reg_start = reg_window_of(5.0));
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1]), ms(1002, &[TG], &[1])], T0);
    // they heard the site before: simply confirmed on it, not registered
    air.run(8000, &mut log.f());
    let mut r = regs(&log, 0);
    r.sort();
    assert_eq!(r, [1001, 1002]);
}

/// CallTimer_Parms (6.7.1.2) carries the site's call timers as annex A tokens.
#[test]
fn call_timer_parms_broadcast() {
    let mut s = site_with(|c| c.call_timers_ms = 2000);
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1])], T0);
    let mut got = None;
    for _ in 0..200 {
        air.step(&mut |_| {});
        let o = air.last;
        if let Some(b) = (o.tscc_dt == DT_CSBK).then(|| Bcast::decode(&o.tscc_pdu)).flatten() {
            if b.ann == BC_CALL_TIMERS {
                got = Some(call_timers_of(&b));
            }
        }
    }
    assert_eq!(got, Some((Some(300), Some(180))), "emergency 300 s, MS-MS 180 s (the defaults)");
    assert!(air.ms[0].bcasts(BC_CALL_TIMERS) >= 2);
}

fn nb(syscode: u16, chan: u16, active: Option<bool>) -> Neighbor {
    Neighbor {
        syscode,
        chan,
        active_known: active.is_some() as u8,
        active: active.unwrap_or(false) as u8,
        cc: cfg_default().cc,
        ..Default::default()
    }
}

/// Vote_Now (6.7.1.3) for each adjacent site, then VOTE_BLK frames (the default 6) of C_ALOHA that lets no MS in,
/// one TSCC slot each, and no more.
#[test]
fn vote_now_blocks_random_access() {
    let mut s = site_with(|c| {
        c.vote_now_ms = 2000;
        c.adjacent_ms = 0;
    });
    s.set_neighbors(&[nb(0x0123, 21, Some(true)), nb(0x0456, 22, None)]);
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1])], T0);
    let mut chans = Vec::new();
    let mut after = -1; // TSCC blocks checked after a Vote_Now
    let mut blocked = 0;
    for _ in 0..400 {
        air.step(&mut |_| {});
        let o = air.last;
        if air.k & 1 == 0 {
            continue; // slot k - 1 was odd: no TSCC block
        }
        let held = Aloha::decode(&o.tscc_pdu).is_some_and(|a| (a.mask, a.addr) == (24, ADRNULL));
        if after > 0 {
            assert!(held, "only a C_ALOHA that lets no MS in");
            after -= 1;
            blocked += 1;
        } else if after == 0 {
            assert!(!held, "random access again after VOTE_BLK frames");
            after = -1;
        } else if let Some(b) = Bcast::decode(&o.tscc_pdu).filter(|b| b.ann == BC_VOTE_NOW) {
            chans.push(b.parms2 & 0xFFF);
            after = 6;
        }
    }
    assert!(chans.len() >= 4 && chans.contains(&21) && chans.contains(&22), "{chans:?}");
    assert!(blocked >= 24);
}

/// Ann_WD_TSCC (6.7.1.1): the adjacent TSCCs two to a CSBK; a site known to be off is withdrawn.
#[test]
fn ann_wd_tscc_broadcast() {
    let mut s = site_with(|c| c.ann_tscc_ms = 2000);
    s.set_neighbors(&[nb(0x0123, 21, Some(true)), nb(0x0456, 22, Some(false)), nb(0x0789, 23, None)]);
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1])], T0);
    let mut seen = Vec::new();
    for _ in 0..200 {
        air.step(&mut |_| {});
        let o = air.last;
        if let Some(b) = (o.tscc_dt == DT_CSBK).then(|| Bcast::decode(&o.tscc_pdu)).flatten().filter(|b| b.ann == BC_ANN_WD_TSCC) {
            seen.push((b.parms1, b.parms2));
        }
    }
    let cc = u16::from(cfg_default().cc);
    assert!(seen.contains(&(cc << 6 | 1, 21 << 12 | 22)), "21 added, 22 withdrawn: {seen:x?}");
    assert!(seen.contains(&(cc << 6, 23 << 12)), "23 alone, CH_2 CHNULL: {seen:x?}");
}

/// Power save (6.4.7): the offset in the registration's C_ACKD; C_AHOYs to the sleeping MS only in its wake
/// frame; a call and a status reach it; a ratio over services.power_save gets offset 0.
#[test]
fn power_save_wake_frames() {
    let mut s = site_with(|c| c.power_save = 3);
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1]), ms(1002, &[10], &[1]), ms(1003, &[11], &[1])], T0);
    air.ms[1].set_power_save(2); // 1:4
    air.ms[2].set_power_save(4); // 1:16: over the site's 1:8
    register_all(&mut air, &mut log);
    let (rq, off) = air.ms[1].power_save();
    assert!(rq == 2 && (1..=3).contains(&off), "{rq} {off}");
    assert_eq!(air.site.power_save_of(1002), (2, off));
    assert_eq!(air.ms[2].power_save(), (4, 0), "registered, power save not available");
    // let it fall asleep, then call it: every C_AHOY to it is in its wake frame
    air.run(3000, &mut log.f());
    let now = air.now();
    air.ms[0].act(t3ms::DO_PRIVATE, 1002, 1000, 0, now);
    let mut ahoys = 0;
    for _ in 0..400 {
        air.step(&mut log.f());
        let o = air.last;
        if let Some(a) = (air.k & 1 == 1 && o.tscc_dt == DT_CSBK).then(|| Ahoy::decode(&o.tscc_pdu)).flatten() {
            if a.target == 1002 && ahoys == 0 {
                assert!(ps_wake(air.k - 1, rq, off), "the first C_AHOY in the wake frame, slot {}", air.k - 1);
            }
            ahoys += (a.target == 1002) as u32;
        }
        if air.ms[1].state() == t3ms::PAYLOAD {
            break;
        }
    }
    assert!(ahoys >= 1 && air.ms[1].state() == t3ms::PAYLOAD, "the called MS woke and joined");
    assert!(air.until(10_000, &mut log.f(), |a| a.ms[0].status().calls_done == 1));
    // and a status after it fell asleep again
    air.run(4000, &mut log.f());
    let now = air.now();
    air.ms[0].act_status(9, false, 1002, now);
    assert!(air.until(8000, &mut log.f(), |a| a.ms[0].status().calls_done == 2));
    assert_eq!(air.ms[1].statuses(), (1, (1001, 1002, 9)));
}

// ── the composite TSCC (361-4 5.3.2) ──

/// Two carriers, three payload channels busy with A, B and C's calls: D's call takes the TSCC's own slot (5.3.2),
/// D talks there and E, idle on the TSCC, waits through it on P_SYS_Parms (Payload Channel Type 1) rather than
/// hunting. After D's call the TSCC is back: E's call goes through it.
#[test]
fn composite_tscc_carries_the_fourth_call() {
    let mut s = site_with(|c| {
        carriers(c, 2);
        c.composite = 1;
    });
    let mut log = Log::default();
    let ch = [1, 2];
    let mut air = Air::new(
        &mut s,
        [ms(1001, &[9], &ch), ms(1002, &[10], &ch), ms(1003, &[11], &ch), ms(1004, &[12], &ch), ms(1005, &[13], &ch)],
        T0,
    );
    register_all(&mut air, &mut log);
    let now = air.now();
    for (i, tg) in [9, 10, 11].into_iter().enumerate() {
        air.ms[i].act(t3ms::DO_GROUP, tg, 20_000, 0, now + 600 * i as i64);
    }
    assert!(
        air.until(10_000, &mut log.f(), |a| (0..3).all(|i| a.ms[i].state() == t3ms::PAYLOAD)),
        "three calls on the three payload channels"
    );
    let now = air.now();
    air.ms[3].act(t3ms::DO_GROUP, 12, 10_000, 0, now);
    assert!(air.until(5000, &mut log.f(), |a| a.ms[3].state() == t3ms::PAYLOAD), "D granted");
    assert_eq!(air.ms[3].status().ch, 0, "D on the TSCC's slot");
    assert!(air.until(500, &mut log.f(), |a| a.site.tscc_payload()), "the slot turns payload after its grants");
    // through D's call (longer than the MS's hunt, 100 TSCC slots): E stays on the TSCC
    let mut rev_slots = 0;
    let ok = air.until(15_000, &mut log.f(), |a| {
        assert_eq!(a.ms[4].status().ch, 0, "E hunted");
        rev_slots += (a.last.rev != 0) as u32;
        a.ms[3].status().calls_done == 1
    });
    assert!(ok, "D's call done");
    assert!(rev_slots > 200, "{rev_slots} slots reverted");
    assert!(air.until(3000, &mut log.f(), |a| !a.site.tscc_payload() && a.ms[3].state() == t3ms::IDLE));
    let now = air.now();
    air.ms[4].act(t3ms::DO_GROUP, 13, 2000, 0, now);
    assert!(air.until(8000, &mut log.f(), |a| a.ms[4].state() == t3ms::PAYLOAD), "the TSCC answers again");
}

// ── C_MOVE and the asynchronous TSCC (6.3.3.1 e, 6.1.1.2) ──

/// C_MOVE to one MS: it leaves for the named TSCC; the others stay. (No TSCC there: it hunts back later.)
#[test]
fn c_move_one_ms() {
    let mut s = site_with(|c| carriers(c, 2));
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1, 2]), ms(1002, &[TG], &[1, 2])], T0);
    register_all(&mut air, &mut log);
    assert!(air.site.command("move", 2, 1002));
    assert!(!air.site.command("move", 0, 1002), "CHNULL");
    assert!(air.until(1000, &mut log.f(), |a| a.ms[1].activations().1 == 1));
    assert_eq!(air.ms[1].ch(), 2, "on carrier 2's LCN 0");
    assert_eq!((air.ms[0].activations().1, air.ms[0].ch()), (0, 0));
    assert!(air.until(15_000, &mut log.f(), |a| a.ms[1].ch() == 0), "nothing there: back to the TSCC");
}

/// With services.hibernate_s, the idle TSCC says so and goes quiet; an MS with a call to make wakes it with
/// BS_Dwn_Act and the call goes on.
#[test]
fn asynchronous_tscc_hibernates_and_wakes() {
    let mut s = site_with(|c| c.hibernate_ms = 3000);
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1]), ms(1002, &[TG], &[1])], T0);
    register_all(&mut air, &mut log);
    let mut flagged = false;
    let mut quiet = 0;
    for _ in 0..300 {
        air.step(&mut log.f());
        let o = air.last;
        if let Some(b) = (o.tscc_dt == DT_CSBK).then(|| Bcast::decode(&o.tscc_pdu)).flatten() {
            flagged |= b.ann == BC_GEN_SITE_PARAMS && b.parms2 >> 22 & 1 == 1;
        }
        if air.site.hibernation().0 {
            assert_eq!((o.tscc_dt, o.has_slc), (0, 0), "nothing on the TSCC");
            quiet += 1;
        }
    }
    assert!(flagged, "Gen_Site_Params with the Hibernating_Flag first");
    assert!(quiet > 100);
    let now = air.now();
    air.ms[0].act(t3ms::DO_GROUP, TG, 1000, 0, now);
    assert!(air.until(5000, &mut log.f(), |a| a.ms[0].state() == t3ms::PAYLOAD));
    assert!(air.ms[0].activations().0 >= 1);
    assert_eq!(air.site.hibernation(), (false, 1));
    assert_eq!(air.ms[1].state(), t3ms::PAYLOAD, "the talkgroup's other member follows");
    assert!(air.until(10_000, &mut log.f(), |a| a.ms[0].status().calls_done == 1));
    assert!(air.until(10_000, &mut log.f(), |a| a.site.hibernation().0), "and back to sleep");
}

// ── full-duplex MS to MS calls (6.6.9) ──

/// Two carriers: the duplex call takes carrier 2 whole, each party on its own slot in offset timing; each hears
/// the other while it talks; the caller's disconnect clears both slots.
#[test]
fn duplex_call_both_talk() {
    let mut s = site_with(|c| {
        carriers(c, 2);
        c.duplex = 1;
    });
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1, 2]), ms(1002, &[TG], &[1, 2])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].act(t3ms::DO_DUPLEX, 1002, 3000, 0, now);
    assert!(air.until(5000, &mut log.f(), |a| a.ms.iter().all(|m| m.state() == t3ms::PAYLOAD)));
    assert_eq!((air.ms[0].ch(), air.ms[1].ch()), (2, 3), "carrier 2: LCN 1 the caller's, LCN 2 the called MS's");
    let st = air.site.status();
    assert!(st.ch[2].r#use != USE_IDLE && st.ch[3].r#use != USE_IDLE);
    let opens = log.of(EV_CH_OPEN);
    assert!(opens.len() == 2 && opens.iter().all(|e| e.why == OPEN_DUPLEX));
    assert!((opens[0].src, opens[0].dst, opens[1].src, opens[1].dst) == (1001, 1002, 1002, 1001));
    air.run(1500, &mut log.f());
    assert!(air.ms[0].voice_heard() > 20 && air.ms[1].voice_heard() > 20, "both directions at once");
    assert!(air.until(8000, &mut log.f(), |a| a.ms[0].status().calls_done == 1));
    assert!(air.until(2000, &mut log.f(), |a| a.ms.iter().all(|m| m.state() == t3ms::IDLE)));
    assert!(air.until(2000, &mut log.f(), |a| a.site.status().ch[2].r#use == USE_IDLE && a.site.status().ch[3].r#use == USE_IDLE));
}

/// Off (the default): refused, as before. A called MS without the service: its reason mirrored. One carrier:
/// no carrier free (the TSCC's has one payload slot), busy.
#[test]
fn duplex_refusals() {
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1]), ms(1002, &[TG], &[1])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].act(t3ms::DO_DUPLEX, 1002, 1000, 0, now);
    assert!(air.until(5000, &mut log.f(), |a| a.ms[0].status().calls_failed == 1));
    assert_eq!(air.ms[0].status().last_reason, R_NOT_SUPPORTED);

    let mut s = site_with(|c| c.duplex = 1);
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1]), ms(1002, &[TG], &[1])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].act(t3ms::DO_DUPLEX, 1002, 1000, 0, now);
    assert!(air.until(5000, &mut log.f(), |a| a.ms[0].status().calls_failed == 1));
    assert_eq!(air.ms[0].status().last_reason, R_SYSBUSY, "one carrier: none free");
    air.ms[1].set_no_duplex(true);
    let now = air.now();
    air.ms[0].act(t3ms::DO_DUPLEX, 1002, 1000, 0, now);
    assert!(air.until(5000, &mut log.f(), |a| a.ms[0].status().calls_failed == 2));
    assert_eq!(air.ms[0].status().last_reason, R_DUPLEX_NOT_SUPPORTED, "mirrored");
}

/// A broadcast call to a talkgroup (BCAST_SV, 6.6.2): BTV_GRANT, which the talkgroup's members follow and the
/// others don't; an all-call (ALLMSIDL) still takes everyone.
#[test]
fn broadcast_call_to_a_talkgroup() {
    let mut s = site_with(|c| carriers(c, 2));
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1, 2]), ms(1002, &[TG], &[1, 2]), ms(1003, &[11], &[1, 2])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].act(t3ms::DO_GROUP, TG, 1500, t3ms::F_BROADCAST, now);
    let mut btv = 0;
    let saw = air.until(5000, &mut log.f(), |a| {
        let o = a.last;
        if o.tscc_dt == DT_CSBK && Grant::decode(&o.tscc_pdu).is_some_and(|g| g.op == OP_BTV_GRANT && g.target == TG) {
            btv += 1;
        }
        a.ms[1].state() == t3ms::PAYLOAD
    });
    assert!(saw && btv >= 1, "BTV_GRANT to the talkgroup");
    air.run(300, &mut log.f());
    assert_eq!(air.ms[2].state(), t3ms::IDLE, "not its talkgroup");
    assert!(air.until(8000, &mut log.f(), |a| a.ms[0].status().calls_done == 1));
    assert!(air.until(8000, &mut log.f(), |a| a.ms.iter().all(|m| m.state() == t3ms::IDLE)));
    let now = air.now();
    air.ms[0].act(t3ms::DO_GROUP, ALLMSIDL, 1000, 0, now);
    assert!(air.until(5000, &mut log.f(), |a| a.ms.iter().all(|m| m.state() == t3ms::PAYLOAD)), "an all-call: everyone");
    // 361-2 5.3.2.1, 5.3.3.1: the Broadcast Service Option in an LC the site makes for either call
    let opens = log.of(EV_CH_OPEN);
    assert_eq!(opens.iter().map(|e| (e.dst, e.svc)).collect::<Vec<_>>(), [(TG, 0x08), (ALLMSIDL, 0x08)]);
}

/// 361-2 5.3.3.0, 5.3.3.1: a broadcast call has no hang time, so P_CLEAR follows the caller's terminator. A
/// normal group call on the same site keeps its hang (3 s here).
#[test]
fn broadcast_call_has_no_hang() {
    let mut s = site_with(|c| carriers(c, 2));
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG], &[1, 2]), ms(1002, &[TG], &[1, 2])], T0);
    register_all(&mut air, &mut log);
    for (flags, done, hang) in [(t3ms::F_BROADCAST, 1, false), (0, 2, true)] {
        let now = air.now();
        air.ms[0].act(t3ms::DO_GROUP, TG, 1000, flags, now);
        assert!(air.until(5000, &mut log.f(), |a| a.ms[1].state() == t3ms::PAYLOAD));
        assert!(air.until(5000, &mut log.f(), |a| a.site.status().ch.iter().any(|c| c.r#use == USE_CALL)));
        assert!(air.until(5000, &mut log.f(), |a| a.site.status().ch.iter().all(|c| c.r#use != USE_CALL)));
        let ended = air.now(); // the talker's terminator
        assert!(air.until(5000, &mut log.f(), |a| a.site.status().ch.iter().all(|c| c.r#use != USE_HANG)));
        let held = air.now() - ended;
        assert_eq!(held > 2000, hang, "held {held} ms after the talker stopped (broadcast: {})", flags != 0);
        assert!(air.until(4000, &mut log.f(), |a| a.ms.iter().all(|m| m.state() == t3ms::IDLE)));
        assert_eq!(air.ms[0].status().calls_done, done);
    }
}
