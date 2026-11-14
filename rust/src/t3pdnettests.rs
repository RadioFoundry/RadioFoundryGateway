// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! Scenario tests for packet data across sites (docs/spec/fw/tier3.md 3.16, 5.10): two sites, each on its own
//! slot-exact air, joined by a stand-in network that carries their setup messages (mqtt-net.md 9.4) and the
//! messages their relays take (the data_pdu item, mqtt-net.md 4.4) from one to the other. The ALS call across sites
//! (3.18) too, on the same stand-in network.

use std::boxed::Box;
use std::vec::Vec;

use crate::t3air::Air;
use crate::t3ms::{self, Ms};
use crate::t3pdnet::{NetData, NetResult, RESULT_DELIVERED, RESULT_UNCONFIRMED};
use crate::t3pdrate::CLASS_SACK;
use crate::t3pdu::*;
use crate::t3site::*;
use crate::t3udt::{DT_DATA_HDR, SK_IND_PD};

const T0: i64 = 1_793_912_651_000;
const TG: u32 = 9;
const TEXT: &[u8] = b"packet data from one site to an MS at the other, by way of the network";

fn site() -> Box<Site> {
    site_with(|_| {})
}

fn site_als(als: bool) -> Box<Site> {
    site_with(|c| c.als = als as u8)
}

fn site_with(f: impl FnOnce(&mut Cfg)) -> Box<Site> {
    let mut c = cfg_default();
    f(&mut c);
    c.ncarriers = 2;
    c.carriers[1] = Carrier { tx_hz: 438_600_000, rx_hz: 431_000_000, chan: 2, tscc: 0, enabled: 1 };
    let mut s = new_site(&c);
    let mut p = Policy::default_open();
    p.wide_tg = IdSet::of(&[(TG, TG)]);
    s.policy_set(&p);
    s.set_link(true);
    s
}

fn ms(id: u32, tg: &[u32]) -> Ms {
    Ms::new(id, tg, &[1, 2], id.wrapping_mul(2654435761))
}

/// What one site did: its events, the setups it sent, the blocks its MSs sent and its relays took.
#[derive(Default)]
struct Side {
    ev: Vec<Event>,
    seen: usize,
    tscc: Vec<[u8; 10]>,
    heads: Vec<(usize, DataHead)>, // data headers its MSs sent (MS index)
    results: Vec<NetResult>,
}

impl Side {
    fn of(&self, ty: u8) -> Vec<Event> {
        self.ev.iter().filter(|e| e.r#type == ty).copied().collect()
    }
    fn setups(&self, op: u8) -> Vec<Event> {
        self.of(EV_NET_SETUP).into_iter().filter(|e| e.op == op).collect()
    }
    fn grants(&self) -> Vec<DataGrant> {
        self.tscc.iter().filter_map(DataGrant::decode).collect()
    }
}

/// Two sites and the network between them: setups and data each way, every message carried (`drop_setups` loses
/// the next n setup messages, as a network that loses them would).
struct Net<'a> {
    a: Air<'a, 2>,
    b: Air<'a, 2>,
    sa: Side,
    sb: Side,
    wire: Vec<NetData>,
    streams: u32,
    drop_setups: u32,
}

impl<'a> Net<'a> {
    fn new(a: &'a mut Site, b: &'a mut Site, ma: [Ms; 2], mb: [Ms; 2]) -> Self {
        let mut n = Net {
            a: Air::new(a, ma, T0),
            b: Air::new(b, mb, T0),
            sa: Side::default(),
            sb: Side::default(),
            wire: Vec::new(),
            streams: 100,
            drop_setups: 0,
        };
        let now = n.a.now();
        for (i, m) in n.a.ms.iter_mut().chain(n.b.ms.iter_mut()).enumerate() {
            m.act(t3ms::DO_REGISTER, TG, 0, 0, now + 100 * i as i64); // subscribed to TG
        }
        assert!(n.until(20_000, |n| n.a.ms.iter().chain(n.b.ms.iter()).all(|m| m.status().registered != 0)));
        n
    }

    fn step(&mut self) {
        for (air, side) in [(&mut self.a, &mut self.sa), (&mut self.b, &mut self.sb)] {
            air.step(&mut |e| side.ev.push(*e));
            if (air.k - 1) & 1 == 0 && air.last.tscc_dt == DT_CSBK {
                side.tscc.push(air.last.tscc_pdu);
            }
            for (i, t) in air.inbound.iter().enumerate() {
                if let Some(t) = t.filter(|t| t.kind == t3ms::TX_DATA && t.dt == DT_DATA_HDR) {
                    side.heads.extend(DataHead::decode(&t.pdu).map(|h| (i, h)));
                }
            }
            while let Some(r) = air.net.take_result() {
                side.results.push(r);
            }
        }
        // setups: each to the other site, as the router would (both ways named caller and called); sdata too
        let now = self.a.now();
        for from_a in [true, false] {
            let (side, to) = if from_a { (&mut self.sa, &mut self.b) } else { (&mut self.sb, &mut self.a) };
            for e in side.ev[side.seen..].iter().filter(|e| e.r#type == EV_NET_SD) {
                to.site.net_sd_in(&SdMsg::of_event(e), now);
            }
            for e in side.ev[side.seen..].iter().filter(|e| e.r#type == EV_NET_SETUP) {
                if self.drop_setups > 0 {
                    self.drop_setups -= 1;
                    continue;
                }
                let m = Setup {
                    op: e.op,
                    reason: e.reason,
                    flags: e.flags,
                    svc: e.svc,
                    src: e.src,
                    dst: e.dst,
                    stream: e.stream,
                    r#ref: e.r#ref,
                };
                to.site.net_setup_in(&m, now);
            }
            side.seen = side.ev.len();
        }
        // data: what a relay took goes to the other site
        while let Some(nd) = self.a.take_published() {
            self.streams += 1;
            self.wire.push(nd);
            self.b.net_data(&nd, self.streams);
        }
        while let Some(nd) = self.b.take_published() {
            self.streams += 1;
            self.wire.push(nd);
            self.a.net_data(&nd, self.streams);
        }
    }

    fn until(&mut self, ms: i64, cond: impl Fn(&Self) -> bool) -> bool {
        let end = self.a.now() + ms;
        while self.a.now() < end {
            self.step();
            if cond(self) {
                return true;
            }
        }
        false
    }

    fn run(&mut self, ms: i64) {
        self.until(ms, |_| false);
    }
}

fn done(i: usize, before: u32) -> impl Fn(&Net) -> bool {
    move |n| {
        let s = n.a.ms[i].status();
        n.a.ms[i].state() == t3ms::IDLE && s.calls_done + s.calls_failed > before
    }
}

#[test]
fn confirmed_data_to_an_ms_at_another_site() {
    let (mut sa, mut sb) = (site(), site());
    let mut n = Net::new(&mut sa, &mut sb, [ms(1001, &[TG]), ms(1002, &[TG])], [ms(2001, &[TG]), ms(2002, &[TG])]);
    let now = n.a.now();
    assert!(n.a.ms[0].send_data(2001, t3ms::F_CONFIRMED, TEXT, now));
    n.a.damage = 0b10; // its second block reaches the site damaged: the site's SACK
    assert!(n.until(10_000, done(0, 0)), "the caller's call ends");
    assert_eq!(n.a.ms[0].status().calls_done, 1, "acknowledged by its own site");

    // the calling site asks the far one (mqtt-net 9.4, flag data), which checks the MS (C_AHOY 0010b, source the
    // caller) and accepts; then the caller's PD_GRANT
    let req = n.sa.setups(SETUP_REQUEST);
    assert_eq!((req.len(), req[0].src, req[0].dst, req[0].flags), (1, 1001, 2001, SETUP_F_DATA));
    let ahoy = n.sb.tscc.iter().filter_map(Ahoy::decode).find(|a| a.service_kind == SK_IND_PD).expect("C_AHOY");
    assert_eq!((ahoy.target, ahoy.source), (2001, 1001));
    let acc = n.sb.setups(SETUP_ACCEPT);
    assert_eq!((acc.len(), acc[0].r#ref, acc[0].flags), (1, req[0].stream, SETUP_F_DATA));
    let g = n.sa.grants();
    assert_eq!((g[0].op, g[0].target, g[0].source), (OP_PD_GRANT, 2001, 1001));
    let open = n.sa.of(EV_PD).into_iter().find(|e| e.op == PD_OPEN).unwrap();
    assert_eq!((open.kind, open.flags & F_PRIVATE_NET), (PARTY_NET, F_PRIVATE_NET));
    // the site's SACK for block 1, the caller's retry, then one message on the network
    let sack = n.sa.heads.iter().find(|(_, h)| h.dpf == DPF_CONFIRMED && !h.full).expect("a selective retry");
    assert_eq!(sack.1.blocks, 1);
    assert_eq!(n.wire.len(), 1);
    assert_eq!((n.wire[0].head.src, n.wire[0].head.dst, &n.wire[0].data[..n.wire[0].len]), (1001, 2001, TEXT));

    // the far site grants the called MS (source the caller) and sends it confirmed; its third block damaged on
    // the far air: the called MS's SACK, the far site's retry
    n.b.damage = 0b100;
    assert!(n.until(10_000, |n| n.b.ms[0].data_inbox().is_some()), "delivered");
    let (h, d, len, count) = n.b.ms[0].data_inbox().unwrap();
    assert_eq!((h.src, h.dst, h.dpf, &d[..len], count), (1001, 2001, DPF_CONFIRMED, TEXT, 1));
    let g = n.sb.grants();
    assert!(g.iter().any(|g| (g.op, g.target, g.source) == (OP_PD_GRANT, 2001, 1001)));
    assert!(n.sb.heads.iter().any(|(i, h)| *i == 0 && h.dpf == DPF_RESPONSE && h.class == CLASS_SACK));
    n.run(2000);
    assert_eq!(n.sb.results, [(1001, 2001, 101, RESULT_DELIVERED, 2)]);
    assert_eq!(n.b.ms[0].state(), t3ms::IDLE, "back on the TSCC");
    // the called site checked the MS once: no second C_AHOY for the data
    let ahoys = n.sb.tscc.iter().filter_map(Ahoy::decode).filter(|a| a.service_kind == SK_IND_PD).count();
    assert!(ahoys <= 2, "{ahoys}"); // a C_AHOY goes twice at most here (its repeat before the answer)
}

#[test]
fn unconfirmed_rate34_data_across() {
    let (mut sa, mut sb) = (site(), site());
    let mut n = Net::new(&mut sa, &mut sb, [ms(1001, &[TG]), ms(1002, &[TG])], [ms(2001, &[TG]), ms(2002, &[TG])]);
    let now = n.a.now();
    assert!(n.a.ms[0].send_data(2002, t3ms::F_RATE34, TEXT, now));
    assert!(n.until(10_000, |n| n.b.ms[1].data_inbox().is_some()));
    let (h, d, len, _) = n.b.ms[1].data_inbox().unwrap();
    assert_eq!((h.dpf, &d[..len]), (DPF_UNCONFIRMED, TEXT));
    assert_eq!(n.wire[0].dt, DT_R34, "sent on at the rate it came");
    assert!(n.sb.heads.iter().all(|(_, h)| h.dpf != DPF_RESPONSE), "no response to unconfirmed data");
    n.run(1000);
    assert_eq!(n.sb.results[0].3, RESULT_UNCONFIRMED);
    assert!(n.a.ms[0].status().calls_done == 1);
}

/// An ALS call across sites (tier3.md 3.18, 6.6.2.6): the request carries SETUP_F_ALS; the called MS's site
/// sends table 6.46's radio check and grants it; the called MS transmits to the caller, who listens. Refused: the
/// called site without ALS (Not_Supported), an MS there without it (its MSNot_Supported, as Not_Supported).
#[test]
fn als_call_across_sites() {
    let (mut sa, mut sb) = (site_als(true), site_als(true));
    let mut n = Net::new(&mut sa, &mut sb, [ms(1001, &[TG]), ms(1002, &[TG])], [ms(2001, &[TG]), ms(2002, &[TG])]);
    let now = n.a.now();
    n.a.ms[0].act(t3ms::DO_PRIVATE, 2001, 0, t3ms::F_ALS, now);
    let called_talks =
        |n: &Net| n.b.inbound[0].is_some_and(|t| t.kind == t3ms::TX_VOICE && t.src == 2001 && t.dst == 1001);
    assert!(n.until(8000, |n| called_talks(n) && n.a.ms[0].state() == t3ms::PAYLOAD));
    let rq = n.sa.setups(SETUP_REQUEST);
    assert_eq!((rq.len(), rq[0].flags, rq[0].dst), (1, SETUP_F_ALS, 2001));
    let a = n.sb.tscc.iter().filter_map(Ahoy::decode).find(|a| a.target == 2001).expect("ALS radio check");
    let want = Ahoy { als: true, service_kind: SK_IND_VOICE, target: 2001, source: 1001, ..Default::default() };
    assert_eq!(a, want, "table 6.46");
    assert_eq!(n.sa.of(EV_CH_OPEN)[0].flags & F_PRIVATE_NET, F_PRIVATE_NET);
    assert!(!n.until(2000, |n| n.a.inbound[0].is_some_and(|t| t.kind == t3ms::TX_VOICE)), "the caller listens");
    assert!(n.until(15_000, |n| n.a.ms[0].state() == t3ms::IDLE && n.b.ms[0].state() == t3ms::IDLE));

    // the called MS without ALS: its MSNot_Supported comes back as Not_Supported
    n.b.ms[1].refuse_supp(false, true);
    let (before, now) = (n.a.ms[0].status().calls_failed, n.a.now());
    n.a.ms[0].act(t3ms::DO_PRIVATE, 2002, 0, t3ms::F_ALS, now);
    assert!(n.until(10_000, |n| n.a.ms[0].status().calls_failed > before));
    assert_eq!(n.a.ms[0].status().last_reason, R_NOT_SUPPORTED);
    let rj = n.sb.setups(SETUP_REJECT);
    assert_eq!(rj.last().map(|e| e.reason), Some(WHY_NOT_SUPPORTED));
}

/// The ALS caller ends the call (status 123) while the called MS talks at the other site (tier3.md 3.20): its site
/// answers Message_Accepted and sends SD_INTR; the called MS's site de-keys it with the RC and clears its channel;
/// the caller's site clears its own.
#[test]
fn als_call_ended_across_sites() {
    let on = |c: &mut Cfg| {
        c.als = 1;
        c.intr = 1;
    };
    let (mut sa, mut sb) = (site_with(on), site_with(on));
    let mut n = Net::new(&mut sa, &mut sb, [ms(1001, &[TG]), ms(1002, &[TG])], [ms(2001, &[TG]), ms(2002, &[TG])]);
    let now = n.a.now();
    n.a.ms[0].act(t3ms::DO_PRIVATE, 2001, 0, t3ms::F_ALS, now);
    let called_talks = |n: &Net| n.b.inbound[0].is_some_and(|t| t.kind == t3ms::TX_VOICE && t.src == 2001);
    assert!(n.until(8000, called_talks));
    let now = n.a.now();
    assert!(n.a.ms[0].als_end(2001, now + 500));
    assert!(n.until(6000, |n| n.sa.of(EV_CH_CLOSE).len() == 1 && n.sb.of(EV_CH_CLOSE).len() == 1));
    assert_eq!(n.b.ms[0].state(), t3ms::IDLE);
    assert_eq!(n.b.ms[0].ceased(), 1, "B de-keyed its talker");
    assert_eq!((n.sa.of(EV_CH_CLOSE)[0].why, n.sb.of(EV_CH_CLOSE)[0].why), (CLOSE_INTR, CLOSE_INTR));
    let sent = n.sa.of(EV_NET_SD).iter().map(SdMsg::of_event).find(|m| m.what == SD_INTR).expect("SD_INTR");
    assert_eq!((sent.src, sent.dst, sent.value), (1001, 2001, STATUS_ALS_END));
    let res = n.sb.of(EV_NET_SD).iter().map(SdMsg::of_event).find(|m| m.what == SD_INTR).expect("its result");
    assert_eq!((res.op, res.reason, res.r#ref), (SD_RESULT, R_MESSAGE_ACCEPTED, sent.stream));
    assert_eq!(n.a.ms[0].state(), t3ms::IDLE, "the caller stays on the TSCC");
}

/// The called MS's site without ALS refuses the request.
#[test]
fn als_call_across_sites_refused() {
    let (mut sa, mut sb) = (site_als(true), site_als(false));
    let mut n = Net::new(&mut sa, &mut sb, [ms(1001, &[TG]), ms(1002, &[TG])], [ms(2001, &[TG]), ms(2002, &[TG])]);
    let now = n.a.now();
    n.a.ms[0].act(t3ms::DO_PRIVATE, 2001, 0, t3ms::F_ALS, now);
    assert!(n.until(8000, done(0, 0)));
    assert_eq!(n.a.ms[0].status().last_reason, R_NOT_SUPPORTED);
    assert_eq!(n.b.ms[0].status().ahoys, 0);
}

#[test]
fn refusals_across_sites() {
    let (mut sa, mut sb) = (site(), site());
    let mut n = Net::new(&mut sa, &mut sb, [ms(1001, &[TG]), ms(1002, &[TG])], [ms(2001, &[TG]), ms(2002, &[TG])]);
    // registered nowhere: the far site's reject not_found, NoregMSaway
    let now = n.a.now();
    assert!(n.a.ms[0].send_data(7777, t3ms::F_CONFIRMED, TEXT, now));
    assert!(n.until(10_000, done(0, 0)));
    assert_eq!(n.sb.setups(SETUP_REJECT)[0].reason, WHY_NOT_FOUND);
    assert_eq!(n.a.ms[0].status().last_reason, R_NOREG_MSAWAY);
    // it doesn't answer the far site's check: reject no_answer, MSaway
    n.b.ms[1].set_answer(t3ms::ANSWER_SILENT, 0);
    let now = n.a.now();
    assert!(n.a.ms[0].send_data(2002, t3ms::F_CONFIRMED, TEXT, now));
    assert!(n.until(20_000, done(0, 1)));
    assert_eq!(n.sb.setups(SETUP_REJECT)[1].reason, WHY_NO_ANSWER);
    assert_eq!(n.a.ms[0].status().last_reason, R_MSAWAY);
    // no answer from the network at all: MSaway after the setup timer, and the request cancelled
    n.drop_setups = 1;
    let now = n.a.now();
    assert!(n.a.ms[0].send_data(2001, t3ms::F_CONFIRMED, TEXT, now));
    assert!(n.until(20_000, done(0, 2)));
    assert_eq!(n.a.ms[0].status().last_reason, R_MSAWAY);
    let c = n.sa.setups(SETUP_CANCEL);
    assert_eq!((c.len(), c[0].flags), (1, SETUP_F_DATA));
    assert!(n.wire.is_empty(), "nothing sent on");
    // the network down: refused at once, nothing asked
    n.a.site.set_link(false);
    let asked = n.sa.setups(SETUP_REQUEST).len();
    let now = n.a.now();
    assert!(n.a.ms[0].send_data(2001, t3ms::F_CONFIRMED, TEXT, now));
    assert!(n.until(10_000, done(0, 3)));
    assert_eq!(n.a.ms[0].status().last_reason, R_NOREG_MSAWAY);
    assert_eq!(n.sa.setups(SETUP_REQUEST).len(), asked);
}

#[test]
fn talkgroup_data_to_the_sites_that_carry_it() {
    let (mut sa, mut sb) = (site(), site());
    let mut n = Net::new(&mut sa, &mut sb, [ms(1001, &[TG]), ms(1002, &[TG])], [ms(2001, &[TG]), ms(2002, &[TG])]);
    let now = n.a.now();
    assert!(n.a.ms[0].send_data(TG, t3ms::F_GROUP, TEXT, now));
    assert!(n.until(10_000, |n| n.b.ms.iter().all(|m| m.data_inbox().is_some())));
    // the local member heard it on the calling site's channel; both far members on the far site's TD_GRANT
    assert!(n.a.ms[1].data_inbox().is_some());
    let open = n.sa.of(EV_PD).into_iter().find(|e| e.op == PD_OPEN).unwrap();
    assert_eq!((open.kind, open.flags & F_WIDE), (PARTY_NET, F_WIDE));
    assert_eq!((n.wire.len(), n.wire[0].head.group, n.wire[0].head.dst), (1, true, TG));
    let g = n.sb.grants();
    assert!(g.iter().any(|g| (g.op, g.target, g.source) == (OP_TD_GRANT, TG, 1001)));
    let (h, d, len, _) = n.b.ms[0].data_inbox().unwrap();
    assert_eq!((h.group, h.src, &d[..len]), (true, 1001, TEXT));
    assert!(n.sa.setups(SETUP_REQUEST).is_empty(), "no setup for a talkgroup");
}

#[test]
fn data_with_no_setup_is_checked_first() {
    // a message from a Tier II radio or another site's IP gateway: no setup came, the far site checks the MS
    let (mut sa, mut sb) = (site(), site());
    let mut n = Net::new(&mut sa, &mut sb, [ms(1001, &[TG]), ms(1002, &[TG])], [ms(2001, &[TG]), ms(2002, &[TG])]);
    let h = DataHead { dpf: DPF_UNCONFIRMED, sap: SAP_IP, src: 9_999_999, dst: 2001, ..Default::default() };
    let nd = NetData::of_msg(&DataMsg::build(h, TEXT).unwrap()).unwrap();
    assert!(n.b.net_data(&nd, 55));
    assert!(n.until(10_000, |n| n.b.ms[0].data_inbox().is_some()));
    let ahoy = n.sb.tscc.iter().filter_map(Ahoy::decode).find(|a| a.service_kind == SK_IND_PD).expect("C_AHOY");
    assert_eq!((ahoy.target, ahoy.source), (2001, 9_999_999));
    // to an MS not here: refused (the caller drops it); to one that doesn't answer: failed
    assert!(!n.b.net_data(&NetData { head: DataHead { dst: 4242, ..h }, ..nd }, 56));
    n.b.ms[1].set_answer(t3ms::ANSWER_SILENT, 0);
    assert!(n.b.net_data(&NetData { head: DataHead { dst: 2002, ..h }, ..nd }, 57));
    n.run(15_000);
    assert!(n.sb.results.iter().any(|r| r.2 == 57 && r.3 == crate::t3pdnet::RESULT_FAILED));
    assert!(n.b.ms[1].data_inbox().is_none());
}

#[test]
fn the_largest_message_across_sites() {
    // 127 blocks, Blocks to Follow's most (361-1 9.3.21): rate 1/2 confirmed, 1266 octets. Blocks lost past 64 on
    // each air: a SACK in two C_RDATA blocks (361-1 8.2.2.3), each way
    let big: Vec<u8> = (0..1266u32).map(|i| (i * 7 + i / 255) as u8).collect();
    let (mut sa, mut sb) = (site(), site());
    let mut n = Net::new(&mut sa, &mut sb, [ms(1001, &[TG]), ms(1002, &[TG])], [ms(2001, &[TG]), ms(2002, &[TG])]);
    let now = n.a.now();
    assert!(!n.a.ms[0].send_data(2001, t3ms::F_CONFIRMED, &[0; 1267], now), "one octet too many");
    assert!(n.a.ms[0].send_data(2001, t3ms::F_CONFIRMED, &big, now));
    n.a.damage = 1 << 5 | 1 << 70 | 1 << 126;
    assert!(n.until(30_000, done(0, 0)), "the caller's call ends");
    assert_eq!(n.a.ms[0].status().calls_done, 1, "acknowledged by its own site");
    // the site's SACK in two C_RDATA blocks: the caller sends the three blocks again, two of them listed in the second
    let retry = n.sa.heads.iter().find(|(_, h)| h.dpf == DPF_CONFIRMED && !h.full).expect("a selective retry");
    assert_eq!(retry.1.blocks, 3, "blocks 5, 70 and 126 again");
    assert_eq!(n.wire.len(), 1);
    assert_eq!((n.wire[0].len, &n.wire[0].data[..n.wire[0].len]), (1266, &big[..]));

    n.b.damage = 1 << 3 | 1 << 100;
    assert!(n.until(30_000, |n| n.b.ms[0].data_inbox().is_some()), "delivered");
    let (h, d, len, count) = n.b.ms[0].data_inbox().unwrap();
    assert_eq!((h.src, h.dst, h.dpf, h.blocks, &d[..len], count), (1001, 2001, DPF_CONFIRMED, 127, &big[..], 1));
    let sack = n.sb.heads.iter().find(|(_, h)| h.dpf == DPF_RESPONSE && h.class == CLASS_SACK).expect("a SACK");
    assert_eq!(sack.1.blocks, 2);
    n.run(3000);
    assert_eq!(n.sb.results, [(1001, 2001, 101, RESULT_DELIVERED, 2)]);
}

#[test]
fn the_largest_unconfirmed_message_at_rate_1() {
    // 127 rate 1 blocks: 3044 octets, the most a DMR data message holds
    let big: Vec<u8> = (0..3044u32).map(|i| (i ^ i >> 8) as u8).collect();
    let (mut sa, mut sb) = (site(), site());
    let mut n = Net::new(&mut sa, &mut sb, [ms(1001, &[TG]), ms(1002, &[TG])], [ms(2001, &[TG]), ms(2002, &[TG])]);
    let now = n.a.now();
    assert!(n.a.ms[0].send_data(2002, t3ms::F_RATE1, &big, now));
    assert!(n.until(30_000, |n| n.b.ms[1].data_inbox().is_some()), "delivered");
    let (h, d, len, _) = n.b.ms[1].data_inbox().unwrap();
    assert_eq!((h.dpf, h.blocks, &d[..len]), (DPF_UNCONFIRMED, 127, &big[..]));
    assert_eq!(n.wire[0].dt, DT_R1);
}

/// A multi-item call (PD_GRANT_MI, 6.6.3.3.2.5) to an MS at another site: the caller's item crosses as one message
/// and the far site delivers it in a single-item call (the caller's P_MAINT ends only its own channel).
#[test]
fn multi_item_data_across_sites() {
    let (mut sa, mut sb) = (site(), site());
    let mut n = Net::new(&mut sa, &mut sb, [ms(1001, &[TG]), ms(1002, &[TG])], [ms(2001, &[TG]), ms(2002, &[TG])]);
    let now = n.a.now();
    assert!(n.a.ms[0].send_data(2001, t3ms::F_CONFIRMED | t3ms::F_MULTI, TEXT, now));
    assert!(n.until(10_000, done(0, 0)), "the caller's call ends");
    assert_eq!(n.a.ms[0].status().calls_done, 1);
    let g = n.sa.grants();
    assert_eq!((g[0].op, g[0].target, g[0].source), (OP_PD_GRANT_MI, 2001, 1001));
    assert!(n.until(10_000, |n| n.b.ms[0].data_inbox().is_some()), "delivered");
    let (h, d, len, count) = n.b.ms[0].data_inbox().unwrap();
    assert_eq!((h.src, &d[..len], count), (1001, TEXT, 1));
    n.run(3000);
    assert_eq!(n.wire.len(), 1);
    assert_eq!(n.sb.results.last().map(|r| r.3), Some(RESULT_DELIVERED));
    // the far site grants single-item (PD_GRANT): the called MS leaves after the item
    assert!(n.sb.grants().iter().all(|g| (g.op, g.target, g.source) == (OP_PD_GRANT, 2001, 1001)));
    assert!(n.a.ms.iter().chain(n.b.ms.iter()).all(|m| m.state() == t3ms::IDLE), "all back on the TSCC");
}

/// Supplementary user data on a voice call across sites (tier3.md 3.21): the caller's site takes the upload and sends
/// it to the called MS's site, which downloads it from SUPLI before its grant; a wide talkgroup's goes to every site.
#[test]
fn supplementary_data_across_sites() {
    let (mut sa, mut sb) = (site(), site());
    let mut n = Net::new(&mut sa, &mut sb, [ms(1001, &[TG]), ms(1002, &[TG])], [ms(2001, &[TG]), ms(2002, &[TG])]);
    let said = |s: &Side, t: &str| s.of(EV_LOG).iter().any(|e| e.text().contains(t));
    assert!(n.a.ms[0].set_supp_as(crate::t3udt::FMT_ISO8, Some(b"meet at the corner")));
    let now = n.a.now();
    n.a.ms[0].act(t3ms::DO_PRIVATE, 2001, 1000, 0, now);
    assert!(n.until(10_000, |n| n.b.ms[0].state() == t3ms::PAYLOAD), "the called MS is granted");
    // on the TSCC before its grant: it has the data now
    let (got, count, _) = n.b.ms[0].supp();
    let (d, len) = got.expect("the data before the grant");
    assert_eq!((&d[..len], count), (&b"meet at the corner"[..], 1));
    assert!(said(&n.sb, "supplementary data from 1001 to 2001: delivered"));
    assert!(n.until(5000, |n| said(&n.sa, "supplementary data from 1001 to 2001: delivered")), "the result came back");
    assert!(n.until(10_000, |n| n.a.ms[0].state() == t3ms::PAYLOAD), "the caller is granted");
    assert!(n.until(30_000, |n| n.a.ms.iter().chain(n.b.ms.iter()).all(|m| m.state() == t3ms::IDLE)));

    // a wide talkgroup: both sites download it to their members
    assert!(n.a.ms[0].set_supp(Some(b"to the talkgroup")));
    let now = n.a.now();
    n.a.ms[0].act(t3ms::DO_GROUP, TG, 1000, 0, now);
    // binary: the two UDTs' 22 octets, the data first
    let to_tg = |m: &Ms| m.supp().0.is_some_and(|(d, len)| d[..len].starts_with(b"to the talkgroup"));
    assert!(n.until(10_000, |n| to_tg(&n.a.ms[1]) && to_tg(&n.b.ms[0]) && to_tg(&n.b.ms[1])), "every member has it");
    assert!(n.until(5000, |n| said(&n.sa, "supplementary data from 1001 to 9: delivered")), "after its repeats");
}
