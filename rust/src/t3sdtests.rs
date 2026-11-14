// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! Scenario tests for short data on the TSCC (docs/spec/fw/tier3.md 3.15): UDT short data (ETSI TS 102 361-4
//! 6.6.4), status delivery and polling (6.6.6), on the slot-exact air.

use std::boxed::Box;
use std::vec::Vec;

use crate::t3air::Air;
use crate::t3ms::{self, Ms};
use crate::t3pdu::*;
use crate::t3site::*;
use crate::t3udt::*;

const T0: i64 = 1_793_912_651_000;
const TG: u32 = 9;
const TEXT: &[u8] = b"Hello from the trunk site, 34 byte";

fn site_with(f: impl FnOnce(&mut Cfg)) -> Box<Site> {
    let mut c = cfg_default();
    f(&mut c);
    new_site(&c)
}

fn ms(id: u32, tg: &[u32]) -> Ms {
    Ms::new(id, tg, &[1, 2], id.wrapping_mul(2654435761))
}

fn two_carriers(c: &mut Cfg) {
    c.ncarriers = 2;
    c.carriers[1] = Carrier { tx_hz: 438_600_000, rx_hz: 431_000_000, chan: 2, tscc: 0, enabled: 1 };
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
    fn sd(&self, ty: u8, op: u8) -> Vec<SdMsg> {
        self.of(ty).iter().filter(|e| e.op == op).map(SdMsg::of_event).collect()
    }
    fn take<const N: usize>(&mut self, air: &mut Air<N>) -> &Self {
        while let Some(e) = air.site.next_event() {
            self.0.push(e);
        }
        self
    }
}

fn register_all<const N: usize>(air: &mut Air<N>, log: &mut Log) {
    let now = air.now();
    for (i, m) in air.ms.iter_mut().enumerate() {
        m.act(t3ms::DO_REGISTER, 0, 0, 0, now + 100 * i as i64);
    }
    assert!(air.until(20_000, &mut log.f(), |a| a.ms.iter().all(|m| m.status().registered != 0)), "registration");
}

fn done<const N: usize>(i: usize) -> impl FnMut(&Air<N>) -> bool {
    move |a| a.ms[i].state() == t3ms::IDLE && a.ms[i].status().calls_done + a.ms[i].status().calls_failed > 0
}

/// Every TSCC block the site sends while running ms milliseconds.
fn tscc_blocks<const N: usize>(air: &mut Air<N>, log: &mut Log, ms: i64) -> Vec<(u8, [u8; 10])> {
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

// ── UDT short data (6.6.4) ──

#[test]
fn udt_ms_to_ms() {
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG]), ms(1002, &[TG]), ms(1003, &[TG])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    assert!(air.ms[0].send_sdm(1002, 0, FMT_ISO8, TEXT, now));
    assert!(air.until(8000, &mut log.f(), done(0)));
    assert_eq!(air.ms[0].status().calls_done, 1);
    // 6.6.4.1.5: the called MS's acknowledgement, mirrored
    assert_eq!(air.ms[0].status().last_reason, R_MS_ACCEPTED);
    let (h, d, n) = air.ms[1].inbox().expect("delivered");
    assert_eq!((n, h.source, h.target, h.format, h.group, h.response), (1, 1001, 1002, FMT_ISO8, false, true));
    assert_eq!(&d[..TEXT.len()], TEXT);
    assert!(air.ms[2].inbox().is_none(), "not for the third MS");
}

#[test]
fn udt_one_to_four_blocks() {
    for len in [1usize, 10, 11, 22, 23, 46] {
        let mut s = site_with(|_| {});
        let mut log = Log::default();
        let mut air = Air::new(&mut s, [ms(1001, &[TG]), ms(1002, &[TG])], T0);
        register_all(&mut air, &mut log);
        let data: Vec<u8> = (0..len as u8).map(|i| i.wrapping_mul(37)).collect();
        let now = air.now();
        assert!(air.ms[0].send_sdm(1002, 0, FMT_BINARY, &data, now));
        assert!(air.until(8000, &mut log.f(), done(0)), "len {len}");
        assert_eq!(air.ms[0].status().last_reason, R_MS_ACCEPTED, "len {len}");
        let (h, d, _) = air.ms[1].inbox().unwrap();
        assert_eq!(h.blocks as usize, blocks_for(len), "len {len}");
        assert_eq!(&d[..len], &data[..], "len {len}");
    }
    // 47 octets don't fit
    let mut m = ms(1001, &[TG]);
    assert!(!m.send_sdm(1002, 0, FMT_BINARY, &[0; 47], 0));
}

#[test]
fn udt_to_a_talkgroup() {
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG]), ms(1002, &[TG]), ms(1003, &[TG]), ms(1004, &[10])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    assert!(air.ms[0].send_sdm(TG, t3ms::F_GROUP, FMT_ISO8, TEXT, now));
    assert!(air.until(8000, &mut log.f(), done(0)));
    // no acknowledgement from the talkgroup; Message_Accepted after the last outbound phase
    assert_eq!(air.ms[0].status().last_reason, R_MESSAGE_ACCEPTED);
    for i in [1, 2] {
        let (h, d, n) = air.ms[i].inbox().expect("member got it");
        assert_eq!((h.group, h.response, h.target, n), (true, false, TG, 1)); // repeats are one message
        assert_eq!(&d[..TEXT.len()], TEXT);
    }
    assert!(air.ms[3].inbox().is_none());
}

#[test]
fn udt_to_the_site() {
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    assert!(air.ms[0].send_sdm(TSI, 0, FMT_ISO8, TEXT, now));
    assert!(air.until(5000, &mut log.f(), done(0)));
    assert_eq!(air.ms[0].status().last_reason, R_MESSAGE_ACCEPTED);
    let m = log.sd(EV_SD, SD_SEND);
    assert_eq!(m.len(), 1);
    assert_eq!((m[0].what, m[0].src, m[0].dst, m[0].value, m[0].data()), (SD_UDT, 1001, TSI, FMT_ISO8, TEXT));
}

#[test]
fn udt_refusals() {
    // the called MS refuses, is busy, or doesn't answer (the TSCC repeats the download, then MSaway)
    for (answer, reason) in
        [(t3ms::ANSWER_REFUSE, R_RECIPIENT_REFUSED), (t3ms::ANSWER_BUSY, R_EQUIP_BUSY), (t3ms::ANSWER_SILENT, R_MSAWAY)]
    {
        let mut s = site_with(|_| {});
        let mut log = Log::default();
        let mut air = Air::new(&mut s, [ms(1001, &[TG]), ms(1002, &[TG])], T0);
        air.ms[1].set_answer(answer, 0);
        register_all(&mut air, &mut log);
        let now = air.now();
        air.ms[0].send_sdm(1002, 0, FMT_ISO8, TEXT, now);
        assert!(air.until(10_000, &mut log.f(), done(0)), "answer {answer}");
        assert_eq!(air.ms[0].status().calls_failed, 1);
        assert_eq!(air.ms[0].status().last_reason, reason, "answer {answer}");
        assert!(air.ms[1].inbox().is_none());
    }
    // to an MS that isn't registered, with no network: NoregMSaway; to a gateway it doesn't serve, a
    // talkgroup it doesn't allow, or itself: refused at once
    let mut s = site_with(|_| {});
    let mut p = Policy::default_open();
    p.tg = IdSet::of(&[(TG, TG)]);
    s.policy_set(&p);
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG])], T0);
    register_all(&mut air, &mut log);
    for (dst, flags, reason) in [
        (5555, 0, R_NOREG_MSAWAY),
        (PSTNI, 0, R_NOT_SUPPORTED),
        (1001, 0, R_NOT_SUPPORTED),
        (77, t3ms::F_GROUP, R_GROUP_NOT_ALLOWED),
    ] {
        let now = air.now();
        let before = air.ms[0].status().calls_failed;
        air.ms[0].send_sdm(dst, flags, FMT_ISO8, TEXT, now);
        assert!(air.until(5000, &mut log.f(), |a| a.ms[0].status().calls_failed > before), "dst {dst}");
        assert_eq!(air.ms[0].status().last_reason, reason, "dst {dst}");
    }
}

#[test]
fn udt_needs_registration() {
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG])], T0);
    let now = air.now();
    air.run(1000, &mut log.f());
    air.ms[0].send_sdm(TSI, 0, FMT_ISO8, TEXT, now);
    assert!(air.until(5000, &mut log.f(), done(0)));
    assert_eq!(air.ms[0].status().last_reason, R_MS_NOT_REGISTERED);
}

#[test]
fn udt_stored_while_the_called_ms_is_busy() {
    let mut s = site_with(two_carriers);
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG]), ms(1002, &[TG]), ms(1003, &[TG])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[1].act(t3ms::DO_PRIVATE, 1003, 4000, 0, now);
    assert!(air.until(5000, &mut log.f(), |a| a.ms[1].state() == t3ms::PAYLOAD));
    let now = air.now();
    air.ms[0].send_sdm(1002, 0, FMT_ISO8, TEXT, now);
    // 6.6.6.1.1.3 a) 2): C_QACKD, the called MS engaged; held until it's free
    assert!(air.until(3000, &mut log.f(), |a| a.ms[0].status().last_reason == R_QUEUED_BUSY));
    assert!(air.until(15_000, &mut log.f(), done(0)));
    assert_eq!(air.ms[0].status().last_reason, R_MS_ACCEPTED);
    assert!(air.ms[1].inbox().is_some());

    // the store timeout (6.6.6.1.1.5): Called_Party_Busy
    let mut s = site_with(|c| {
        two_carriers(c);
        c.sd_store_ms = 2000;
    });
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG]), ms(1002, &[TG]), ms(1003, &[TG])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[1].act(t3ms::DO_PRIVATE, 1003, 8000, 0, now);
    assert!(air.until(5000, &mut log.f(), |a| a.ms[1].state() == t3ms::PAYLOAD));
    let now = air.now();
    air.ms[0].send_sdm(1002, 0, FMT_ISO8, TEXT, now);
    assert!(air.until(6000, &mut log.f(), done(0)));
    assert_eq!(air.ms[0].status().last_reason, R_CALLED_PARTY_BUSY);
    assert!(air.ms[1].inbox().is_none());
}

#[test]
fn udt_cancel_while_stored() {
    let mut s = site_with(two_carriers);
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG]), ms(1002, &[TG]), ms(1003, &[TG])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[1].act(t3ms::DO_PRIVATE, 1003, 6000, 0, now);
    assert!(air.until(5000, &mut log.f(), |a| a.ms[1].state() == t3ms::PAYLOAD));
    let now = air.now();
    air.ms[0].send_sdm(1002, 0, FMT_ISO8, TEXT, now);
    assert!(air.until(3000, &mut log.f(), |a| a.ms[0].status().last_reason == R_QUEUED_BUSY));
    let now = air.now();
    air.ms[0].act(t3ms::DO_CANCEL, 1002, 0, 0, now);
    assert!(air.until(3000, &mut log.f(), |a| a.ms[0].status().last_reason == R_MESSAGE_ACCEPTED));
    air.run(10_000, &mut log.f());
    assert!(air.ms[1].inbox().is_none(), "cancelled: not delivered");
}

#[test]
fn udt_upload_is_kept_in_consecutive_slots() {
    // 6.5.0 and figure 6.43: after the C_AHOY from SDMI, the TSCC keeps the inbound slots (AT busy, C_ALOHA
    // mask 24 address NULL) for the header and every block; the download is the header then the blocks in a row
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG]), ms(1002, &[TG])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].send_sdm(1002, 0, FMT_ISO8, TEXT, now);
    let v = tscc_blocks(&mut air, &mut log, 3000);
    let i = v
        .iter()
        .position(|(dt, p)| *dt == DT_CSBK && Ahoy::decode(p).is_some_and(|a| a.source == SDMI && a.target == 1001))
        .expect("C_AHOY from SDMI");
    let a = Ahoy::decode(&v[i].1).unwrap();
    assert_eq!((a.service_kind, a.appended), (SK_IND_UDT, 2));
    for j in 1..=3 {
        let al = Aloha::decode(&v[i + j].1).expect("C_ALOHA keeps the slot");
        assert_eq!((al.mask, al.addr), (24, ADRNULL));
    }
    let h = v.iter().position(|(dt, _)| *dt == DT_DATA_HDR).expect("download header");
    let hd = UdtHead::decode(&v[h].1).unwrap();
    assert_eq!((hd.op, hd.blocks, hd.target, hd.source), (OP_UDTHD, 3, 1002, 1001));
    assert!(v[h + 1..h + 4].iter().all(|(dt, _)| *dt == DT_RATE12));
}

#[test]
fn udt_upload_crc_error() {
    // an upload with a bad CRC: the TSCC asks again, then C_NACKD(CRC error)
    // (the caller isn't a simulated MS here, which would refuse an upload it didn't ask for)
    let mut s = site_with(|c| {
        c.ahoy_tries = 2;
        c.reg_required = 0;
    });
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [], T0);
    air.run(500, &mut log.f());
    let ch = air.site.tscc_ch();
    let r = Rand { dep: 0, service_kind: SK_IND_UDT, target: TSI, source: 1001, ..Default::default() };
    air.site.rx_pdu(ch, DT_CSBK, &r.encode());
    let mut ahoys = 0;
    let mut nack = None;
    for _ in 0..400 {
        air.step(&mut log.f());
        if (air.k - 1) & 1 != 0 || air.last.tscc_dt != DT_CSBK {
            continue;
        }
        let p = air.last.tscc_pdu;
        if Ahoy::decode(&p).is_some_and(|a| a.source == SDMI) {
            ahoys += 1;
            let h = UdtHead { op: OP_UDTHU, target: TSI, source: 1001, blocks: 1, ..Default::default() };
            air.site.rx_pdu(ch, DT_DATA_HDR, &h.encode());
            let mut b = Blocks::of(b"short", 0).unwrap().b[0];
            b[3] ^= 0x10;
            air.site.rx_block(ch, DT_RATE12, &b);
        } else if let Some(a) = Ack::decode(&p).filter(|a| a.op == OP_ACKD && a.target == 1001) {
            nack = Some(a.reason);
            break;
        }
    }
    assert_eq!((ahoys, nack), (2, Some(R_UDT_CRC)));
    assert!(log.take(&mut air).of(EV_SD).is_empty());
}

#[test]
fn udt_no_upload_times_out() {
    // C_AHOY from SDMI unanswered: asked again, then C_NACKD(CRC error in the UDT upload)
    let mut s = site_with(|c| c.reg_required = 0);
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [], T0);
    air.run(500, &mut log.f());
    let ch = air.site.tscc_ch();
    let r = Rand { dep: 1, service_kind: SK_IND_UDT, target: TSI, source: 1001, ..Default::default() };
    air.site.rx_pdu(ch, DT_CSBK, &r.encode());
    let v = tscc_blocks(&mut air, &mut log, 6000);
    let asks = v.iter().filter(|(dt, p)| *dt == DT_CSBK && Ahoy::decode(p).is_some_and(|a| a.source == SDMI)).count();
    assert_eq!(asks, 3);
    assert!(v.iter().any(|(dt, p)| *dt == DT_CSBK
        && Ack::decode(p).is_some_and(|a| a.op == OP_ACKD && a.reason == R_UDT_CRC && a.target == 1001)));
}

#[test]
fn udt_unasked_upload_is_ignored() {
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG])], T0);
    register_all(&mut air, &mut log);
    let ch = air.site.tscc_ch();
    let h = UdtHead { op: OP_UDTHU, target: TSI, source: 1001, blocks: 1, ..Default::default() };
    air.site.rx_pdu(ch, DT_DATA_HDR, &h.encode());
    air.site.rx_block(ch, DT_RATE12, &Blocks::of(b"hi", 0).unwrap().b[0]);
    air.run(2000, &mut log.f());
    assert!(log.of(EV_SD).is_empty());
}

// ── status (6.6.6) ──

#[test]
fn status_to_an_ms_and_a_talkgroup() {
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG]), ms(1002, &[TG]), ms(1003, &[TG])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].act(t3ms::DO_STATUS, 1002, 42, 0, now);
    assert!(air.until(5000, &mut log.f(), done(0)));
    assert_eq!(air.ms[0].status().last_reason, R_MS_ACCEPTED);
    assert_eq!(air.ms[1].sd_statuses(), (1001, 42, 1, 0));
    assert_eq!(air.ms[2].sd_statuses().2, 0);

    let now = air.now();
    air.ms[0].act(t3ms::DO_STATUS, TG, 99, t3ms::F_GROUP, now);
    assert!(air.until(5000, &mut log.f(), |a| a.ms[0].status().calls_done == 2 && a.ms[0].state() == t3ms::IDLE));
    assert_eq!(air.ms[0].status().last_reason, R_MESSAGE_ACCEPTED);
    for i in [1, 2] {
        let (src, st, _, _) = air.ms[i].sd_statuses();
        assert_eq!((src, st), (1001, 99));
    }
    // a status to the site
    let now = air.now();
    air.ms[0].act(t3ms::DO_STATUS, TSI, 7, 0, now);
    assert!(air.until(5000, &mut log.f(), |a| a.ms[0].status().calls_done == 3 && a.ms[0].state() == t3ms::IDLE));
    let m = log.sd(EV_SD, SD_SEND);
    assert_eq!((m[0].what, m[0].value, m[0].src), (SD_STATUS, 7, 1001));
}

#[test]
fn status_refused_and_unanswered() {
    for (answer, reason) in [(t3ms::ANSWER_REFUSE, R_RECIPIENT_REFUSED), (t3ms::ANSWER_SILENT, R_MSAWAY)] {
        let mut s = site_with(|_| {});
        let mut log = Log::default();
        let mut air = Air::new(&mut s, [ms(1001, &[TG]), ms(1002, &[TG])], T0);
        air.ms[1].set_answer(answer, 0);
        register_all(&mut air, &mut log);
        let now = air.now();
        air.ms[0].act(t3ms::DO_STATUS, 1002, 5, 0, now);
        assert!(air.until(8000, &mut log.f(), done(0)));
        assert_eq!(air.ms[0].status().last_reason, reason);
    }
}

#[test]
fn status_polling() {
    // 6.6.6.2: the poll (status 127) goes in a C_AHOY; the polled MS's C_ACKU(0x47) carries its status, which
    // the caller gets in C_ACKD(0x47, Response_Info)
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG]), ms(1002, &[TG])], T0);
    air.ms[1].set_status(63);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].act(t3ms::DO_STATUS, 1002, STATUS_POLL as u32, 0, now);
    assert!(air.until(5000, &mut log.f(), done(0)));
    assert_eq!(air.ms[0].status().last_reason, R_POLL_MS);
    assert_eq!(air.ms[0].sd_statuses().3, 63);
    assert_eq!(air.ms[1].sd_statuses().2, 0, "a poll isn't a status");
    // polling a talkgroup isn't a service
    let now = air.now();
    air.ms[0].act(t3ms::DO_STATUS, TG, STATUS_POLL as u32, t3ms::F_GROUP, now);
    assert!(air.until(5000, &mut log.f(), |a| a.ms[0].status().calls_failed == 1));
    assert_eq!(air.ms[0].status().last_reason, R_NOT_SUPPORTED);
}

// ── UDT short data polling (6.6.5) ──

#[test]
fn upoll_ms_to_ms() {
    // 6.6.5.1: the TSCC answers the poll with a C_AHOY from the caller to the polled MS (Service_Kind 0110b, the
    // C_RAND's Service_Options mirrored, Appended_Blocks the UDTs asked for); the polled MS uploads its data to the
    // caller, the TSCC downloads it, the caller's C_ACKU ends it with no final C_ACKD
    const POS: &[u8] = b"33.6 N 86.6 W, 2 m/s";
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG]), ms(1002, &[TG]), ms(1003, &[TG])], T0);
    assert!(air.ms[1].set_poll_reply(FMT_ISO8, POS));
    register_all(&mut air, &mut log);
    let now = air.now();
    assert!(air.ms[0].send_upoll(1002, FMT_ISO8, 2, now));
    let mut ahoy = None;
    let mut acks = 0;
    let end = air.now() + 8000;
    while air.now() < end && !done::<3>(0)(&air) {
        air.step(&mut log.f());
        if (air.k - 1) & 1 == 0 && air.last.tscc_dt == DT_CSBK {
            if let Some(a) = Ahoy::decode(&air.last.tscc_pdu) {
                if a.service_kind == SK_UDT_POLL {
                    ahoy = Some(a);
                }
            }
            if let Some(a) = Ack::decode(&air.last.tscc_pdu) {
                acks += (a.target == 1001 && reason_type(a.reason) == 1) as u32;
            }
        }
    }
    assert!(done::<3>(0)(&air), "the poll ends");
    assert_eq!(air.ms[0].status().calls_done, 1);
    let a = ahoy.expect("the poll's C_AHOY");
    assert_eq!((a.source, a.target, a.service_options, a.appended, a.group), (1001, 1002, FMT_ISO8, 1, false));
    assert_eq!(air.ms[1].polled(), 1);
    let (h, d, n) = air.ms[0].inbox().expect("the polled data");
    assert_eq!((n, h.source, h.target, h.format, h.response), (1, 1002, 1001, FMT_ISO8, true));
    assert_eq!(&d[..POS.len()], POS);
    assert!(air.ms[2].inbox().is_none() && air.ms[1].inbox().is_none());
    // run on: no final C_ACKD to the caller after its own C_ACKU, nothing left in progress
    let _ = tscc_blocks(&mut air, &mut log, 2000);
    assert_eq!(acks, 0, "no C_ACKD to the caller");
    assert!(!air.site.sd_pending());
}

#[test]
fn upoll_refusals() {
    // the polled MS refuses (C_NACKU, mirrored in C_NACKD: 6.6.5.1.2 a), an MS not registered here, itself, a
    // gateway; a diverted MS: Div_Cause_Fail (t3sutests)
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG]), ms(1002, &[TG])], T0);
    register_all(&mut air, &mut log);
    for (dst, reason) in
        [(1002, R_RECIPIENT_REFUSED), (5555, R_NOREG_MSAWAY), (1001, R_NOT_SUPPORTED), (PSTNI, R_NOT_SUPPORTED)]
    {
        let now = air.now();
        let before = air.ms[0].status().calls_failed;
        air.ms[0].send_upoll(dst, FMT_ISO8, 1, now);
        assert!(air.until(8000, &mut log.f(), |a| a.ms[0].status().calls_failed > before), "dst {dst}");
        assert_eq!(air.ms[0].status().last_reason, reason, "dst {dst}");
    }
    assert!(air.ms[0].inbox().is_none());
    let _ = tscc_blocks(&mut air, &mut log, 2000);
    assert!(!air.site.sd_pending());
}

/// Two sites a slot at a time, the sdata messages carried between them as the network would.
fn two_sites_sd<const N: usize, const M: usize>(
    a: &mut Air<N>,
    b: &mut Air<M>,
    la: &mut Log,
    lb: &mut Log,
    ms: i64,
    mut stop: impl FnMut(&Air<N>) -> bool,
) -> Vec<SdMsg> {
    let mut sent = Vec::new();
    let end = a.now() + ms;
    while a.now() < end && !stop(a) {
        let (na, nb) = (la.0.len(), lb.0.len());
        a.step(&mut la.f());
        b.step(&mut lb.f());
        let now = a.now();
        let sd = |l: &Log, n: usize| -> Vec<SdMsg> {
            l.0[n..].iter().filter(|e| e.r#type == EV_NET_SD).map(SdMsg::of_event).collect()
        };
        for m in sd(la, na) {
            b.site.net_sd_in(&m, now);
            sent.push(m);
        }
        for m in sd(lb, nb) {
            a.site.net_sd_in(&m, now);
            sent.push(m);
        }
    }
    sent
}

#[test]
fn upoll_across_sites() {
    // tier3.md 5.9: the poll of an MS at another site goes there (what 6); that site sends the C_AHOY from the
    // caller and takes the upload; its result carries the data back, and the caller's site downloads it
    const POS: &[u8] = b"33.6 N 86.6 W";
    let (mut sa, mut sb) = (site_with(|_| {}), site_with(|_| {}));
    sa.set_link(true);
    sb.set_link(true);
    let (mut la, mut lb) = (Log::default(), Log::default());
    let mut a = Air::new(&mut sa, [ms(1001, &[TG])], T0);
    let mut b = Air::new(&mut sb, [ms(2002, &[TG]), ms(2003, &[TG])], T0);
    assert!(b.ms[0].set_poll_reply(FMT_ISO8, POS));
    register_all(&mut a, &mut la);
    register_all(&mut b, &mut lb);
    while b.now() < a.now() {
        b.step(&mut lb.f());
    }
    let now = a.now();
    assert!(a.ms[0].send_upoll(2002, FMT_ISO8, 2, now));
    let sent = two_sites_sd(&mut a, &mut b, &mut la, &mut lb, 15_000, done::<1>(0));
    assert!(done::<1>(0)(&a), "the poll ends");
    assert_eq!((a.ms[0].status().calls_done, b.ms[0].polled()), (1, 1));
    let q = sent.iter().find(|m| m.op == SD_SEND).expect("the poll out");
    assert_eq!((q.what, q.src, q.dst, q.value, q.data()), (SD_UDT_POLL, 1001, 2002, FMT_ISO8, &[2u8][..]));
    let r = sent.iter().find(|m| m.op == SD_RESULT).expect("its result");
    let got = (r.what, r.reason, r.r#ref, r.value, r.data());
    assert_eq!(got, (SD_UDT_POLL, R_MESSAGE_ACCEPTED, q.stream, FMT_ISO8, POS));
    let (h, d, _) = a.ms[0].inbox().expect("the polled data");
    assert_eq!((h.source, h.target, h.format), (2002, 1001, FMT_ISO8));
    assert_eq!(&d[..POS.len()], POS);
    let _ = two_sites_sd(&mut a, &mut b, &mut la, &mut lb, 2000, |_| false);
    assert!(!a.site.sd_pending() && !b.site.sd_pending());

    // the polled MS refuses: its reason back in the result, mirrored to the caller in C_NACKD
    let now = a.now();
    assert!(a.ms[0].send_upoll(2003, FMT_ISO8, 1, now));
    let _ = two_sites_sd(&mut a, &mut b, &mut la, &mut lb, 15_000, |a| a.ms[0].status().calls_failed == 1);
    assert_eq!(a.ms[0].status().last_reason, R_RECIPIENT_REFUSED);
    // no site has it: MSaway once the caller's site gives up
    let now = a.now();
    assert!(a.ms[0].send_upoll(5555, FMT_ISO8, 1, now));
    let _ = two_sites_sd(&mut a, &mut b, &mut la, &mut lb, 40_000, |a| a.ms[0].status().calls_failed == 2);
    assert_eq!(a.ms[0].status().last_reason, R_MSAWAY);
    let _ = two_sites_sd(&mut a, &mut b, &mut la, &mut lb, 2000, |_| false);
    assert!(!a.site.sd_pending() && !b.site.sd_pending());
}

// ── from the site's application ──

#[test]
fn app_sends_udt_status_and_polls() {
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG]), ms(1002, &[TG])], T0);
    air.ms[0].set_status(12);
    register_all(&mut air, &mut log);
    let now = air.now();
    let mut m =
        SdMsg { op: SD_SEND, what: SD_UDT, value: FMT_ISO8, dst: 1001, r#ref: 55, len: 5, ..Default::default() };
    m.data[..5].copy_from_slice(b"hello");
    assert!(air.site.sd_send(&m, now));
    assert!(air.until(5000, &mut log.f(), |a| a.ms[0].inbox().is_some()));
    air.run(1000, &mut log.f());
    let (h, d, _) = air.ms[0].inbox().unwrap();
    assert_eq!((h.source, &d[..5]), (TSI, &b"hello"[..]));
    let r = log.sd(EV_SD, SD_RESULT);
    assert_eq!((r[0].r#ref, r[0].reason, r[0].why), (55, R_MS_ACCEPTED, WHY_OK));

    // a status to a talkgroup and a poll
    let now = air.now();
    let st =
        SdMsg { op: SD_SEND, what: SD_STATUS, value: 3, flags: SD_F_GROUP, dst: TG, r#ref: 56, ..Default::default() };
    assert!(air.site.sd_send(&st, now));
    let poll = SdMsg { op: SD_SEND, what: SD_POLL, dst: 1001, r#ref: 57, ..Default::default() };
    assert!(air.site.sd_send(&poll, now));
    air.run(5000, &mut log.f());
    assert_eq!(air.ms[1].sd_statuses().1, 3);
    let r = log.sd(EV_SD, SD_RESULT);
    let p = r.iter().find(|m| m.r#ref == 57).unwrap();
    assert_eq!((p.reason, p.value), (R_POLL_MS, 12));
    assert_eq!(r.iter().find(|m| m.r#ref == 56).unwrap().reason, R_MESSAGE_ACCEPTED);

    // bad requests: no room for 47 octets, a poll to a talkgroup, status 127 as a status, a gateway
    let mut big = m;
    big.len = 47;
    assert!(!air.site.sd_send(&big, now));
    assert!(!air.site.sd_send(&SdMsg { flags: SD_F_GROUP, ..poll }, now));
    assert!(!air.site.sd_send(&SdMsg { value: STATUS_POLL, ..st }, now));
    assert!(!air.site.sd_send(&SdMsg { dst: PSTNI, ..m }, now));
}

#[test]
fn app_queue_full() {
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    let m = SdMsg { op: SD_SEND, what: SD_STATUS, value: 1, dst: 1001, ..Default::default() };
    let sent = (0..12).filter(|_| air.site.sd_send(&m, now)).count();
    assert_eq!(sent, 8);
    // and an MS's request when it's full: SYSbusy
    let now = air.now();
    air.ms[0].act(t3ms::DO_STATUS, TSI, 9, 0, now);
    assert!(air.until(5000, &mut log.f(), |a| a.ms[0].status().last_reason == R_SYSBUSY));
}

// ── across the network (mqtt-net sdata, tier3.md 5) ──

#[test]
fn network_short_data() {
    let mut s = site_with(|_| {});
    s.set_link(true);
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG]), ms(1002, &[TG])], T0);
    register_all(&mut air, &mut log);

    // out: to an MS not here, then the far site's result
    let now = air.now();
    air.ms[0].send_sdm(7777, 0, FMT_ISO8, TEXT, now);
    assert!(air.until(5000, &mut log.f(), |a| a.ms[0].status().last_reason == R_WAIT));
    let out = log.sd(EV_NET_SD, SD_SEND);
    assert_eq!((out.len(), out[0].dst, out[0].data()), (1, 7777, TEXT));
    let now = air.now();
    let res = SdMsg {
        op: SD_RESULT,
        reason: R_MS_ACCEPTED,
        src: 1001,
        dst: 7777,
        r#ref: out[0].stream,
        ..Default::default()
    };
    air.site.net_sd_in(&res, now);
    assert!(air.until(3000, &mut log.f(), done(0)));
    assert_eq!(air.ms[0].status().last_reason, R_MS_ACCEPTED);

    // out with no answer: MSaway after TNP_Timer
    let now = air.now();
    air.ms[0].act(t3ms::DO_STATUS, 7778, 4, 0, now);
    assert!(air.until(15_000, &mut log.f(), |a| a.ms[0].status().calls_failed == 1));
    assert_eq!(air.ms[0].status().last_reason, R_MSAWAY);

    // in: to an MS here, the result goes back with the sender's stream as ref
    let now = air.now();
    let mut m = SdMsg {
        op: SD_SEND,
        what: SD_UDT,
        value: FMT_ISO8,
        src: 7777,
        dst: 1002,
        stream: 300,
        len: 3,
        ..Default::default()
    };
    m.data[..3].copy_from_slice(b"abc");
    air.site.net_sd_in(&m, now);
    assert!(air.until(5000, &mut log.f(), |a| a.ms[1].inbox().is_some()));
    air.run(500, &mut log.f());
    let r = log.sd(EV_NET_SD, SD_RESULT);
    assert_eq!((r[0].r#ref, r[0].reason, r[0].src, r[0].dst), (300, R_MS_ACCEPTED, 7777, 1002));
    // in: for an MS not here, ignored (another site may have it); for a talkgroup here, sent on the TSCC
    air.site.net_sd_in(&SdMsg { dst: 4444, stream: 301, ..m }, now);
    let n = log.take(&mut air).sd(EV_NET_SD, SD_RESULT).len();
    let now = air.now();
    air.site.net_sd_in(&SdMsg { flags: SD_F_GROUP, dst: TG, stream: 302, src: 7777, ..m }, now);
    air.run(2000, &mut log.f());
    assert!(air.ms[0].inbox().is_none(), "no MS here takes the talkgroup: not sent");
    let now = air.now();
    air.ms[0].act(t3ms::DO_REGISTER, TG, 0, 0, now);
    assert!(air.until(5000, &mut log.f(), |a| a.site.record(1001).is_some_and(|r| r.sub_tg == TG)));
    let now = air.now();
    air.site.net_sd_in(&SdMsg { flags: SD_F_GROUP, dst: TG, stream: 302, src: 7777, ..m }, now);
    assert!(air.until(5000, &mut log.f(), |a| a.ms[0].inbox().is_some_and(|(h, _, _)| h.group)));
    air.run(3000, &mut log.f());
    assert_eq!(log.sd(EV_NET_SD, SD_RESULT).len(), n, "no result for a talkgroup's or one not here");
}

/// A padded format goes to the network as the message, not its blocks: a 24-character ISO 8-bit text fills three
/// blocks with ten pad octets (20 pad nibbles, B.3.14), which stay at the sending site; the far site pads it again.
/// A BCD number with an odd pad nibble keeps it (the half octet goes in the data, the nibble in pad_nibble).
#[test]
fn network_short_data_without_its_pad() {
    const TEXT24: &[u8] = b"rfoundry short data test";
    let mut s = site_with(|_| {});
    s.set_link(true);
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG]), ms(1002, &[TG])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].send_sdm(7777, 0, FMT_ISO8, TEXT24, now);
    assert!(air.until(5000, &mut log.f(), |a| a.ms[0].status().last_reason == R_WAIT));
    let out = log.sd(EV_NET_SD, SD_SEND);
    assert_eq!((out.len(), out[0].data(), out[0].pad_nibble), (1, TEXT24, 20));
    // in again, at an MS here: three blocks, the 20 pad nibbles back, the text whole
    let now = air.now();
    air.site.net_sd_in(&SdMsg { dst: 1002, stream: 400, ..out[0] }, now);
    assert!(air.until(5000, &mut log.f(), |a| a.ms[1].inbox().is_some()));
    let (h, d, _) = air.ms[1].inbox().unwrap();
    assert_eq!((h.blocks, h.pad_nibble), (3, 20));
    assert_eq!(&d[..user_len(3) - 10], TEXT24);
    assert!(d[user_len(3) - 10..user_len(3)].iter().all(|&b| b == 0xFF));
    // BCD from the far site: 11 digits are 6 octets, the last one half pad; one block, 2 * (10 - 6) + 1 pad nibbles
    let mut m = SdMsg { op: SD_SEND, what: SD_UDT, value: FMT_BCD, src: 7778, dst: 1001, stream: 401, len: 6,
                        pad_nibble: 9, ..Default::default() };
    m.data[..6].copy_from_slice(&[0x12, 0x34, 0x56, 0x78, 0x90, 0x1F]);
    let now = air.now();
    air.site.net_sd_in(&m, now);
    assert!(air.until(5000, &mut log.f(), |a| a.ms[0].inbox().is_some_and(|(h, _, _)| h.format == FMT_BCD)));
    let (h, d, _) = air.ms[0].inbox().unwrap();
    assert_eq!((h.blocks, h.pad_nibble), (1, 9));
    assert_eq!(&d[..6], m.data());
}

#[test]
fn network_wide_talkgroup_goes_out() {
    let mut s = site_with(|_| {});
    let mut p = Policy::default_open();
    p.wide_tg = IdSet::of(&[(TG, TG)]);
    s.policy_set(&p);
    s.set_link(true);
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    air.ms[0].act(t3ms::DO_STATUS, TG, 8, t3ms::F_GROUP, now);
    assert!(air.until(5000, &mut log.f(), done(0)));
    let out = log.sd(EV_NET_SD, SD_SEND);
    assert_eq!((out.len(), out[0].flags & SD_F_GROUP, out[0].value), (1, SD_F_GROUP, 8));
}
