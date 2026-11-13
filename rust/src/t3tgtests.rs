// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! Scenario tests for talkgroup lists (docs/spec/fw/tier3.md 3.17): registration with a talkgroup
//! subscription/attachment list (ETSI TS 102 361-4 6.4.4.1.13.2) and Dynamic Group Numbering Assignment (6.6.8),
//! on the slot-exact air.

use std::boxed::Box;
use std::vec::Vec;

use crate::t3air::Air;
use crate::t3ms::{self, Ms};
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

fn ms(id: u32, tg: &[u32]) -> Ms {
    Ms::new(id, tg, &[1, 2], id.wrapping_mul(2654435761))
}

/// Talkgroups 1 to 99 only.
fn tgs_to_99(s: &mut Site) {
    let mut p = Policy::default_open();
    p.tg = IdSet::of(&[(1, 99)]);
    s.policy_set(&p);
}

#[derive(Default)]
struct Log(Vec<Event>);

impl Log {
    fn f(&mut self) -> impl FnMut(&Event) + '_ {
        |e| self.0.push(*e)
    }
    fn take<const N: usize>(&mut self, air: &mut Air<N>) {
        while let Some(e) = air.site.next_event() {
            self.0.push(e);
        }
    }
}

fn register_all<const N: usize>(air: &mut Air<N>, log: &mut Log) {
    let now = air.now();
    for (i, m) in air.ms.iter_mut().enumerate() {
        m.act(t3ms::DO_REGISTER, 0, 0, 0, now + 100 * i as i64);
    }
    assert!(air.until(20_000, &mut log.f(), |a| a.ms.iter().all(|m| m.status().registered != 0)), "registration");
}

fn idle<const N: usize>(i: usize) -> impl FnMut(&Air<N>) -> bool {
    move |a| a.ms[i].state() == t3ms::IDLE && a.ms[i].status().last_reason != 0
}

fn done<const N: usize>(i: usize) -> impl FnMut(&Air<N>) -> bool {
    move |a| a.ms[i].state() == t3ms::IDLE && a.ms[i].status().calls_done + a.ms[i].status().calls_failed > 0
}

fn subs(s: &Site) -> Vec<u32> {
    let mut b = [0u32; 64];
    let n = s.subs(&mut b);
    let mut v = b[..n].to_vec();
    v.sort_unstable();
    v
}

/// Every TSCC CSBK the site sends while running ms milliseconds.
fn tscc_csbks<const N: usize>(air: &mut Air<N>, log: &mut Log, ms: i64) -> Vec<[u8; 10]> {
    let mut v = Vec::new();
    let end = air.now() + ms;
    while air.now() < end {
        air.step(&mut log.f());
        if (air.k - 1) & 1 == 0 && air.last.tscc_dt == DT_CSBK {
            v.push(air.last.tscc_pdu);
        }
    }
    v
}

// ── registration with a talkgroup list (6.4.4.1.13.2) ──

#[test]
fn registration_with_a_talkgroup_list() {
    let mut s = site_with(|_| {});
    tgs_to_99(&mut s);
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG])], T0);
    let now = air.now();
    // ADDRESS3 is outside the site's talkgroups
    assert!(air.ms[0].register_list(&[TG, 10, 500, 11], now));
    let pdus = tscc_csbks(&mut air, &mut log, 3000);
    assert_eq!(air.ms[0].state(), t3ms::IDLE);
    assert_eq!(air.ms[0].status().registered, 1);
    // table 6.11: C_AHOY from TATTSI, Service_Kind_Flag 1, Service_Kind 1110b, two blocks for four addresses
    let a = pdus.iter().find_map(Ahoy::decode).expect("C_AHOY for the list");
    assert_eq!((a.source, a.target, a.service_kind, a.service_kind_flag, a.appended), (TATTSI, 1001, SK_REG, true, 1));
    assert!(!a.als && !a.group);
    // table 6.10: one bit per address, ADDRESS1 the MSB; ADDRESS3 refused; the bits past the list are 1
    assert_eq!(air.ms[0].status().last_reason, R_REG_SUBSCRIPTION);
    assert_eq!(air.ms[0].reg_info(), 0b110_1111);
    let ack = pdus.iter().filter_map(Ack::decode).find(|a| a.reason == R_REG_SUBSCRIPTION).unwrap();
    assert_eq!((ack.source, ack.target, ack.response_info), (TATTSI, 1001, 0b110_1111));
    assert_eq!(air.site.tg_lists(1001).0, [TG, 10, 0, 11, 0, 0, 0]);
    assert_eq!(subs(air.site), [TG, 10, 11]);
}

#[test]
fn talkgroup_list_all_accepted_replaced_and_removed() {
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG])], T0);
    let now = air.now();
    // all accepted: Reg_Accepted, Response_Info 1111111b whatever the count
    assert!(air.ms[0].register_list(&[21, 22, 23, 24, 25, 26, 27], now));
    assert!(air.until(4000, &mut log.f(), idle(0)));
    assert_eq!((air.ms[0].status().last_reason, air.ms[0].reg_info()), (R_REG_ACCEPTED, 0x7F));
    assert_eq!(air.site.tg_lists(1001).0, [21, 22, 23, 24, 25, 26, 27]);
    // a new list replaces it (ADRNULL entries hold nothing)
    let now = air.now();
    assert!(air.ms[0].register_list(&[30, ADRNULL, 31], now));
    assert!(air.until(4000, &mut log.f(), |a| a.site.tg_lists(1001).0[0] == 30 && a.ms[0].state() == t3ms::IDLE));
    assert_eq!(air.site.tg_lists(1001).0, [30, 0, 31, 0, 0, 0, 0]);
    assert_eq!(air.ms[0].reg_info(), 0x7F);
    assert_eq!(subs(air.site), [30, 31]);
    // a single subscription replaces the list (6.4.4.1.13.2: Trgt_Adr_Cnts 01b, list size 1)
    let now = air.now();
    air.ms[0].act(t3ms::DO_REGISTER, 40, 0, 0, now);
    assert!(air.until(4000, &mut log.f(), |a| subs(a.site) == [40]));
    assert_eq!(air.site.tg_lists(1001).0, [0; 7]);
    // a list again, then deregistration: gone
    let now = air.now();
    assert!(air.ms[0].register_list(&[50, 51], now));
    assert!(air.until(4000, &mut log.f(), |a| a.site.tg_lists(1001).0[0] == 50 && a.ms[0].state() == t3ms::IDLE));
    assert!(!subs(air.site).contains(&40), "the list replaced the single subscription");
    let now = air.now();
    air.ms[0].act(t3ms::DO_DEREGISTER, 0, 0, 0, now);
    assert!(air.until(4000, &mut log.f(), |a| a.ms[0].status().registered == 0 && a.ms[0].state() == t3ms::IDLE));
    assert_eq!(air.site.tg_lists(1001).0, [0; 7]);
    assert!(subs(air.site).is_empty());
}

#[test]
fn talkgroup_list_registration_refused() {
    let mut s = site_with(|_| {});
    let mut p = Policy::default_open();
    p.open = 0;
    p.ms = IdSet::of(&[(2000, 2999)]);
    s.policy_set(&p);
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG])], T0);
    let now = air.now();
    assert!(air.ms[0].register_list(&[TG, 10], now));
    let pdus = tscc_csbks(&mut air, &mut log, 3000);
    // C_NACK Reg_Denied from TATTSI, and no C_AHOY for a list
    let nack = pdus.iter().filter_map(Ack::decode).find(|a| a.target == 1001).expect("refused");
    assert_eq!((nack.reason, nack.source), (R_REG_DENIED, TATTSI));
    assert!(pdus.iter().filter_map(Ahoy::decode).all(|a| a.target != 1001));
    assert_eq!(air.ms[0].status().registered, 0);
}

// ── DGNA (6.6.8) ──

fn dgna_site() -> Box<Site> {
    let mut s = site_with(|c| c.dgna_ms = 1);
    tgs_to_99(&mut s);
    s
}

#[test]
fn dgna_ms_to_ms() {
    let mut s = dgna_site();
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG]), ms(1002, &[TG]), ms(1003, &[TG])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    // DGNA_Address mode: four talkgroups in two blocks, ADDRESS1 the One-key talkgroup
    assert!(air.ms[0].send_dgna(1002, FMT_ADDRESS, &[20, 21, 22, 23], true, &[], now));
    let pdus = tscc_csbks(&mut air, &mut log, 6000);
    assert_eq!(air.ms[0].status().calls_done, 1);
    // 6.6.8.2.3: the called MS's MS_Accepted, mirrored to the caller
    assert_eq!(air.ms[0].status().last_reason, R_MS_ACCEPTED);
    // 6.6.8.2.1 c): C_AHOY from DGNAI, Service_Kind 1101b, Appended_Blocks from SUPED_VAL
    let a = pdus.iter().filter_map(Ahoy::decode).find(|a| a.source == DGNAI).expect("C_AHOY from DGNAI");
    assert_eq!((a.target, a.service_kind, a.appended, a.service_kind_flag), (1001, SK_SUPPL, 1, false));
    let (tgs, n) = air.ms[1].dgna_tgs();
    assert_eq!((&tgs[..5], n), (&[20, 21, 22, 23, 0][..], 1));
    assert_eq!(air.ms[2].dgna_tgs().1, 0, "only the called MS");
    assert_eq!(&air.site.tg_lists(1002).1[..5], &[20, 21, 22, 23, 0]);
    assert!(subs(air.site).contains(&22));
    // the MS now takes calls to its dynamic talkgroup
    let now = air.now();
    air.ms[2].act(t3ms::DO_GROUP, 21, 1500, 0, now);
    assert!(air.until(4000, &mut log.f(), |a| a.ms[1].status().heard_dst == 21));
    assert!(air.until(8000, &mut log.f(), done(2)));
    // DGNA_Alias mode: the sixteenth talkgroup with its alias (B.3.9: OK, ADDRESS, UTF-16BE)
    let mixed = [0u8, 0, 0, 44, 0, b'O', 0, b'p', 0, b's'];
    let now = air.now();
    assert!(air.ms[0].send_dgna(1002, FMT_MIXED, &[], false, &mixed, now));
    assert!(air.until(6000, &mut log.f(), |a| a.ms[0].status().calls_done == 2 && a.ms[0].state() == t3ms::IDLE));
    let (tgs, n) = air.ms[1].dgna_tgs();
    assert_eq!((tgs[15], tgs[0], n), (44, 20, 2));
    assert_eq!(air.site.tg_lists(1002).1[15], 44);
    // all deleted: ADRNULL in the Address Format (6.6.8.1.1)
    let now = air.now();
    assert!(air.ms[0].send_dgna(1002, FMT_ADDRESS, &[ADRNULL], false, &[], now));
    assert!(air.until(6000, &mut log.f(), |a| a.ms[0].status().calls_done == 3 && a.ms[0].state() == t3ms::IDLE));
    assert_eq!(&air.ms[1].dgna_tgs().0[..15], &[0; 15]);
    assert!(!subs(air.site).contains(&22));
}

#[test]
fn dgna_refusals() {
    // the site doesn't take DGNA from MSs: Not_Supported at once, no upload
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG]), ms(1002, &[TG])], T0);
    register_all(&mut air, &mut log);
    let now = air.now();
    assert!(air.ms[0].send_dgna(1002, FMT_ADDRESS, &[20], false, &[], now));
    assert!(air.until(4000, &mut log.f(), done(0)));
    assert_eq!((air.ms[0].status().last_reason, air.ms[0].status().calls_failed), (R_NOT_SUPPORTED, 1));
    assert_eq!(air.ms[1].dgna_tgs().1, 0);

    let mut s = dgna_site();
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG]), ms(1002, &[TG])], T0);
    register_all(&mut air, &mut log);
    // a talkgroup the site doesn't allow
    let now = air.now();
    assert!(air.ms[0].send_dgna(1002, FMT_ADDRESS, &[20, 500], false, &[], now));
    assert!(air.until(6000, &mut log.f(), done(0)));
    assert_eq!(air.ms[0].status().last_reason, R_GROUP_NOT_ALLOWED);
    // to itself, or to the site
    for (dst, f) in [(1001, 2), (TSI, 3)] {
        let now = air.now();
        assert!(air.ms[0].send_dgna(dst, FMT_ADDRESS, &[20], false, &[], now));
        assert!(air.until(6000, &mut log.f(), |a| a.ms[0].status().calls_failed == f && a.ms[0].state() == 0));
        assert_eq!(air.ms[0].status().last_reason, R_NOT_SUPPORTED, "to {dst}");
    }
    // the called MS not here, no network: NoReg_MSaway
    let now = air.now();
    assert!(air.ms[0].send_dgna(1777, FMT_ADDRESS, &[20], false, &[], now));
    assert!(air.until(6000, &mut log.f(), |a| a.ms[0].status().calls_failed == 4 && a.ms[0].state() == 0));
    assert_eq!(air.ms[0].status().last_reason, R_NOREG_MSAWAY);
    // the called MS refuses
    air.ms[1].set_answer(t3ms::ANSWER_REFUSE, 0);
    let now = air.now();
    assert!(air.ms[0].send_dgna(1002, FMT_ADDRESS, &[20], false, &[], now));
    assert!(air.until(6000, &mut log.f(), |a| a.ms[0].status().calls_failed == 5 && a.ms[0].state() == 0));
    assert_eq!(air.ms[0].status().last_reason, R_RECIPIENT_REFUSED);
    assert_eq!(air.site.tg_lists(1002).1, [0; 16], "nothing kept");
}

#[test]
fn site_sends_dgna() {
    let mut s = site_with(|_| {});
    let mut log = Log::default();
    let mut air = Air::new(&mut s, [ms(1001, &[TG]), ms(1002, &[TG])], T0);
    register_all(&mut air, &mut log);
    // 6.6.8.3.1: from a gateway, only the outbound phase; the source DGNAI
    let b = AddrList::blocks(false, &[60, 61, 62, 63, 64]).unwrap();
    let mut buf = [0u8; MAX_DATA];
    let n = b.data(&mut buf);
    let mut m = SdMsg { op: SD_SEND, what: SD_DGNA, value: FMT_ADDRESS, dst: 1002, r#ref: 7, ..Default::default() };
    m.len = n as u8;
    m.data[..n].copy_from_slice(&buf[..n]);
    let now = air.now();
    assert!(air.site.sd_send(&m, now));
    assert!(air.until(6000, &mut log.f(), |a| a.ms[1].dgna_tgs().1 == 1));
    tscc_csbks(&mut air, &mut log, 2000); // the result
    log.take(&mut air);
    assert_eq!(&air.ms[1].dgna_tgs().0[..6], &[60, 61, 62, 63, 64, 0]);
    let r =
        log.0.iter().filter(|e| e.r#type == EV_SD && e.op == SD_RESULT).map(SdMsg::of_event).next().expect("result");
    assert_eq!((r.what, r.reason, r.r#ref, r.src, r.dst), (SD_DGNA, R_MS_ACCEPTED, 7, DGNAI, 1002));
    assert!(subs(air.site).contains(&64));
    // not to a talkgroup, not in another format
    assert!(!air.site.sd_send(&SdMsg { flags: SD_F_GROUP, ..m }, now));
    assert!(!air.site.sd_send(&SdMsg { value: FMT_ISO8, ..m }, now));
}
