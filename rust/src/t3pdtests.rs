// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! Scenario tests for packet data calls (docs/spec/fw/tier3.md 3.10): ETSI TS 102 361-4 6.6.3 on the slot-exact
//! air, with 361-1 9.2 messages (unconfirmed and confirmed) that the air's radio layer counts and repeats.

use std::boxed::Box;
use std::vec::Vec;

use crate::t3air::Air;
use crate::t3ms::{self, Ms};
use crate::t3pdu::*;
use crate::t3site::*;
use crate::t3udt::{IPI, SK_IND_PD};

const T0: i64 = 1_793_912_651_000;
const TG: u32 = 9;
const GW: u32 = 9_999_999;
const DATA: &[u8] = b"an IP datagram's worth of packet data, 52 octets long";

fn site_with(f: impl FnOnce(&mut Cfg)) -> Box<Site> {
    let mut c = cfg_default();
    c.ncarriers = 2;
    c.carriers[1] = Carrier { tx_hz: 438_600_000, rx_hz: 431_000_000, chan: 2, tscc: 0, enabled: 1 };
    f(&mut c);
    new_site(&c)
}

fn ms(id: u32, tg: &[u32]) -> Ms {
    Ms::new(id, tg, &[1, 2], id.wrapping_mul(2654435761))
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
    assert!(air.until(20_000, &mut log.f(), |a| a.ms.iter().all(|m| m.status().registered != 0)), "registration");
}

fn settled<const N: usize>(i: usize, before: u32) -> impl FnMut(&Air<N>) -> bool {
    move |a| a.ms[i].state() == t3ms::IDLE && a.ms[i].status().calls_done + a.ms[i].status().calls_failed > before
}

/// Every TSCC block the site sends while running ms milliseconds (events to log).
fn tscc_blocks<const N: usize>(air: &mut Air<N>, log: &mut Log, ms: i64) -> Vec<[u8; 10]> {
    let mut v = Vec::new();
    let end = air.now() + ms;
    while air.now() < end {
        air.step(&mut log.f());
        if (air.k - 1) & 1 == 0 && air.last.tscc_dt == 3 {
            v.push(air.last.tscc_pdu);
        }
    }
    v
}

fn data_grants(v: &[[u8; 10]]) -> Vec<DataGrant> {
    v.iter().filter_map(DataGrant::decode).collect()
}

/// The time (ms) from the first data channel open to its close, and the close's reason.
fn channel_life(log: &Log) -> Option<(u8, usize)> {
    let open = log.of(EV_CH_OPEN).into_iter().find(|e| e.flags & F_DATA != 0)?;
    let close = log.of(EV_CH_CLOSE).into_iter().find(|e| e.ch == open.ch)?;
    Some((close.why, open.ch as usize))
}

// ── individual packet data (6.6.3.1.1, 6.6.3.1.4) ──

#[test]
fn packet_data_ms_to_ms_unconfirmed() {
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG]), ms(1002, &[TG]), ms(1003, &[TG])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    assert!(air.ms[0].send_data(1002, 0, DATA, now));
    let blocks = tscc_blocks(&mut air, &mut log, 3000);
    // the availability check: C_AHOY to the called MS, Service_Kind 0010b, source the caller (table 6.49)
    let ahoy = blocks.iter().filter_map(Ahoy::decode).find(|a| a.service_kind == SK_IND_PD).expect("C_AHOY");
    assert_eq!((ahoy.target, ahoy.source, ahoy.group), (1002, 1001, false));
    assert_eq!(air.ms[1].status().ahoys, 1);
    // then PD_GRANT, single item, single slot (7.1.1.1.1.4)
    let g = data_grants(&blocks);
    assert!(!g.is_empty());
    assert_eq!((g[0].op, g[0].target, g[0].source, g[0].hi_rate), (OP_PD_GRANT, 1002, 1001, false));
    assert!(air.until(3000, &mut log.f(), settled(0, 0)));
    assert_eq!(air.ms[0].status().calls_done, 1);
    let (h, d, n, count) = air.ms[1].data_inbox().expect("delivered");
    assert_eq!((h.src, h.dst, h.dpf, h.group, count), (1001, 1002, DPF_UNCONFIRMED, false, 1));
    assert_eq!(&d[..n], DATA);
    assert!(air.ms[2].data_inbox().is_none());
    // 6.6.3.3.1.6 b 2): the unconfirmed single item sent, the channel clears at once
    air.run(2000, &mut log.f());
    let (why, _) = channel_life(&log).expect("cleared");
    assert_eq!(why, CLOSE_HANG);
    let open = log.of(EV_PD);
    assert_eq!((open[0].op, open[0].kind, open[0].src, open[0].dst), (PD_OPEN, 0, 1001, 1002));
}

#[test]
fn packet_data_confirmed_and_hang_time() {
    let mut s = site_with(|c| c.data_hang_ms = 4000);
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG]), ms(1002, &[TG])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    assert!(air.ms[0].send_data(1002, t3ms::F_CONFIRMED, DATA, now));
    assert!(air.until(8000, &mut log.f(), settled(0, 0)));
    // 361-1 9.2.3: the called MS's ACK reached the caller through the site's repeat
    assert_eq!(air.ms[0].status().calls_done, 1);
    let (h, d, n, _) = air.ms[1].data_inbox().expect("delivered");
    assert_eq!((h.dpf, h.response), (DPF_CONFIRMED, true));
    assert_eq!(&d[..n], DATA);
    // 6.6.3.3.1.6 b 1): confirmed data holds the channel for the data hang time
    let t_end = air.now();
    let ch = log.of(EV_CH_OPEN)[0].ch as usize;
    assert!(air.until(8000, &mut log.f(), |a| a.site.status().ch[ch].r#use == USE_IDLE));
    let held = air.now() - t_end;
    assert!((3000..6000).contains(&held), "held {held} ms");
}

#[test]
fn packet_data_to_a_talkgroup() {
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG]), ms(1002, &[TG]), ms(1003, &[TG]), ms(1004, &[77])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    assert!(air.ms[0].send_data(TG, t3ms::F_GROUP, DATA, now));
    let blocks = tscc_blocks(&mut air, &mut log, 2000);
    let g = data_grants(&blocks);
    assert_eq!((g[0].op, g[0].target, g[0].source), (OP_TD_GRANT, TG, 1001));
    assert!(blocks.iter().filter_map(Ahoy::decode).all(|a| a.service_kind != SK_IND_PD), "no availability check");
    assert!(air.until(3000, &mut log.f(), |a| a.ms[1].data_inbox().is_some() && a.ms[2].data_inbox().is_some()));
    assert!(air.until(3000, &mut log.f(), settled(0, 0)));
    assert_eq!(air.ms[0].status().calls_done, 1);
    assert!(air.ms[3].data_inbox().is_none(), "another talkgroup");
    let (h, ..) = air.ms[2].data_inbox().unwrap();
    assert!(h.group);
    // a voice call to the talkgroup meanwhile gets a channel of its own
    let open = log.of(EV_CH_OPEN);
    assert_eq!(open.len(), 1);
    assert_eq!(open[0].flags & (F_DATA | F_GROUP), F_DATA | F_GROUP);
}

#[test]
fn packet_data_to_the_site() {
    for target in [TSI, GW, IPI] {
        let mut s = site_with(|c| c.data_id = GW);
        let mut log = Log::default();
        let mut air = Air::new(&mut s, [ms(1001, &[TG])], T0);
        register_all(&mut air, &mut log);
        let now = air.now();
        assert!(air.ms[0].send_data(target, t3ms::F_CONFIRMED, DATA, now));
        let blocks = tscc_blocks(&mut air, &mut log, 2000);
        let g = data_grants(&blocks);
        assert_eq!((g[0].op, g[0].target, g[0].source), (OP_PD_GRANT, target, 1001), "the gateway as the called party");
        let pd = log.of(EV_PD);
        assert_eq!((pd[0].op, pd[0].kind, pd[0].dst), (PD_OPEN, 1, target), "the site is a party");
        // the site's IP gateway answers confirmed data; nobody here does: the MS gives up
        assert!(air.until(8000, &mut log.f(), settled(0, 0)));
        assert_eq!(air.ms[0].status().calls_failed, 1);
    }
    // without a data ID, that ID is just an MS that isn't here
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    assert!(air.ms[0].send_data(GW, 0, DATA, now));
    assert!(air.until(5000, &mut log.f(), settled(0, 0)));
    assert_eq!(air.ms[0].status().last_reason, R_NOREG_MSAWAY);
}

#[test]
fn packet_data_refusals() {
    // the called MS's C_NACKU, mirrored; no answer: MSaway (6.6.3.1.4)
    for (answer, reason) in [(t3ms::ANSWER_REFUSE, R_RECIPIENT_REFUSED), (t3ms::ANSWER_SILENT, R_MSAWAY)] {
        let mut s = site_with(|_| {});
        let mut log = Log::default();
        let mut air = Air::new(&mut s, [ms(1001, &[TG]), ms(1002, &[TG])], T0);
        register_all(&mut air, &mut log);
        air.ms[1].set_answer(answer, 0);
        let now = air.now();
        assert!(air.ms[0].send_data(1002, 0, DATA, now));
        assert!(air.until(15_000, &mut log.f(), settled(0, 0)));
        assert_eq!((air.ms[0].status().calls_failed, air.ms[0].status().last_reason), (1, reason));
        assert!(log.of(EV_CH_OPEN).is_empty());
    }
    let mut s = site_with(|c| c.ahoy_tries = 1);
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG]), ms(1002, &[TG])], T0);
    register_all(&mut air, &mut log);
    s_refuse(&mut air, &mut log, 7777, 0, R_NOREG_MSAWAY); // not registered here
    s_refuse(&mut air, &mut log, PSTNI, 0, R_NOT_SUPPORTED); // a multi-part call (extended addressing): not built
    s_refuse(&mut air, &mut log, 1001, 0, R_NOT_SUPPORTED); // itself
    let mut p = Policy::default_open();
    p.tg = IdSet::of(&[(1, 8)]);
    air.site.policy_set(&p);
    s_refuse(&mut air, &mut log, TG, t3ms::F_GROUP, R_GROUP_NOT_ALLOWED);
}

fn s_refuse<const N: usize>(air: &mut Air<N>, log: &mut Log, dst: u32, flags: u8, reason: u8) {
    let before = air.ms[0].status().calls_failed;
    let now = air.now();
    assert!(air.ms[0].send_data(dst, flags, DATA, now));
    assert!(air.until(5000, &mut log.f(), settled(0, before)), "dst {dst}");
    assert_eq!((air.ms[0].status().calls_failed, air.ms[0].status().last_reason), (before + 1, reason), "dst {dst}");
}

#[test]
fn packet_data_called_party_busy() {
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG]), ms(1002, &[TG]), ms(1003, &[TG])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[1].act(t3ms::DO_PRIVATE, 1003, 8000, 0, now);
    assert!(air.until(5000, &mut log.f(), |a| a.ms[1].state() == t3ms::PAYLOAD));
    let now = air.now();
    assert!(air.ms[0].send_data(1002, 0, DATA, now));
    assert!(air.until(5000, &mut log.f(), settled(0, 0)));
    assert_eq!(air.ms[0].status().last_reason, R_CALLED_PARTY_BUSY);
}

#[test]
fn packet_data_queued_for_a_channel() {
    // one carrier: the TSCC on TS1, one payload channel; a voice call has it
    let mut s = site_with(|c| c.ncarriers = 1);
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG]), ms(1002, &[TG]), ms(1003, &[77])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[2].act(t3ms::DO_GROUP, 77, 3000, 0, now);
    assert!(air.until(5000, &mut log.f(), |a| a.ms[2].state() == t3ms::PAYLOAD));
    let now = air.now();
    assert!(air.ms[0].send_data(TG, t3ms::F_GROUP, DATA, now));
    let blocks = tscc_blocks(&mut air, &mut log, 1500);
    let q = blocks.iter().filter_map(Ack::decode).find(|a| a.target == 1001).expect("an answer");
    assert_eq!(q.reason, R_QUEUED, "C_QACKD Queued_for_Resource (6.6.3.1.3)");
    // the voice call ends and hangs; then the data gets the channel
    assert!(air.until(12_000, &mut log.f(), settled(0, 0)));
    assert_eq!(air.ms[0].status().calls_done, 1);
    assert!(air.ms[1].data_inbox().is_some());
    // and a queued request can be cancelled
    let now = air.now();
    air.ms[2].act(t3ms::DO_GROUP, 77, 3000, 0, now);
    assert!(air.until(5000, &mut log.f(), |a| a.ms[2].state() == t3ms::PAYLOAD));
    let now = air.now();
    assert!(air.ms[0].send_data(TG, t3ms::F_GROUP, DATA, now));
    air.run(1000, &mut log.f());
    let now = air.now();
    air.ms[0].act(t3ms::DO_CANCEL, 0, 0, 0, now);
    assert!(air.until(5000, &mut log.f(), settled(0, 1)));
    assert_eq!(air.ms[0].status().last_reason, R_MESSAGE_ACCEPTED);
}

#[test]
fn packet_data_multi_item() {
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG]), ms(1002, &[TG])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    assert!(air.ms[0].send_data(1002, t3ms::F_MULTI, DATA, now));
    let blocks = tscc_blocks(&mut air, &mut log, 3000);
    assert_eq!(data_grants(&blocks)[0].op, OP_PD_GRANT_MI);
    // after its item the caller ends the call with P_MAINT (6.6.3.3.2.5); the site clears it
    assert!(air.until(8000, &mut log.f(), settled(0, 0)));
    assert_eq!(air.ms[0].status().calls_done, 1);
    assert!(air.ms[1].data_inbox().is_some());
    assert_eq!(channel_life(&log).map(|c| c.0), Some(CLOSE_DISCON));
}

#[test]
fn site_sends_packet_data() {
    let mut s = site_with(|c| c.data_id = GW);
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG]), ms(1002, &[TG])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    assert!(air.site.pd_send(GW, 1001, false, now));
    let blocks = tscc_blocks(&mut air, &mut log, 3000);
    let ahoy = blocks.iter().filter_map(Ahoy::decode).find(|a| a.service_kind == SK_IND_PD).expect("C_AHOY");
    assert_eq!((ahoy.target, ahoy.source), (1001, GW));
    let g = data_grants(&blocks);
    assert_eq!((g[0].target, g[0].source), (1001, GW));
    let pd = log.of(EV_PD);
    assert_eq!((pd[0].op, pd[0].kind, pd[0].src, pd[0].dst), (PD_OPEN, 1, GW, 1001));
    assert_eq!(air.ms[0].state(), t3ms::PAYLOAD);
    // to a talkgroup: TD_GRANT at once
    let now = air.now();
    assert!(air.site.pd_send(GW, TG, true, now));
    air.run(500, &mut log.f());
    assert!(log.of(EV_PD).iter().any(|e| e.op == PD_OPEN && e.dst == TG && e.kind == 0));
    // not here, or no answer
    let now = air.now();
    assert!(!air.site.pd_send(GW, 4242, false, now));
    air.ms[1].set_answer(t3ms::ANSWER_SILENT, 0);
    assert!(air.site.pd_send(GW, 1002, false, now));
    air.run(8000, &mut log.f());
    let f: Vec<Event> = log.of(EV_PD).into_iter().filter(|e| e.op == PD_FAILED).collect();
    assert_eq!((f.len(), f[0].reason, f[0].dst), (1, R_MSAWAY, 1002));
}
