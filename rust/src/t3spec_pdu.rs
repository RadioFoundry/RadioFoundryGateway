// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! Spec conformance, PDU layouts (ETSI TS 102 361-4 clause 7 and annex B): every block t3pdu builds or reads,
//! checked against a block built field by field from the standard's tables, and the coding tables (C_SYScode,
//! NRand_Wait, Backoff, day of week). Expected values come from the standard, never from t3pdu.

use crate::t3pdu as p;
use crate::t3spec_bits::{csbko, field, lb, spec, Bits};

// ── opcodes, data types, addresses ──

/// 361-4 table B.1 and the PDU tables of 7.1.1: the CSBKOs we use.
#[test]
fn opcodes_table_b1() {
    assert_eq!(p::OP_ALOHA, spec::C_ALOHA);
    assert_eq!(p::OP_AHOY, spec::C_AHOY);
    assert_eq!(p::OP_RAND, spec::C_RAND);
    assert_eq!(p::OP_ACKD, spec::C_ACKD);
    assert_eq!(p::OP_ACKU, spec::C_ACKU);
    assert_eq!(p::OP_BCAST, spec::C_BCAST);
    assert_eq!(p::OP_MAINT, spec::P_MAINT);
    assert_eq!(p::OP_CLEAR, spec::P_CLEAR);
    assert_eq!(p::OP_PROTECT, spec::P_PROTECT);
    assert_eq!(p::OP_PV_GRANT, spec::PV_GRANT);
    assert_eq!(p::OP_TV_GRANT, spec::TV_GRANT);
    assert_eq!(p::OP_BTV_GRANT, spec::BTV_GRANT);
    assert_eq!(p::OP_MOVE, spec::C_MOVE);
}

/// 361-1 table 9.22: CSBK 0011b, MBC header 0100b, MBC continuation 0101b.
#[test]
fn data_types_table_9_22() {
    assert_eq!(p::DT_CSBK, spec::DT_CSBK);
    assert_eq!(p::DT_MBC_HDR, spec::DT_MBC_HEADER);
    assert_eq!(p::DT_MBC_CONT, spec::DT_MBC_CONT);
}

/// 361-4 A.4: the gateway identifiers and all-call addresses.
#[test]
fn addresses_a4() {
    assert_eq!(p::ADRNULL, spec::ADRNULL);
    assert_eq!(p::PSTNI, spec::PSTNI);
    assert_eq!(p::REGI, spec::REGI);
    assert_eq!(p::TSI, spec::TSI);
    assert_eq!(p::ALLMSI, spec::ALLMSI);
    assert_eq!(p::TATTSI, spec::TATTSI);
    assert_eq!(p::ALLMSIDL, spec::ALLMSIDL);
    assert_eq!(p::ALLMSIDZ, spec::ALLMSIDZ);
    assert_eq!(p::ALLMSID, spec::ALLMSID);
}

/// 361-4 A.4: every gateway identifier is one; an MS ID, a talkgroup and the all-call IDs are not.
#[test]
fn gateway_and_allcall_a4() {
    for g in [
        spec::PSTNI,
        spec::PABXI,
        spec::LINEI,
        spec::IPI,
        spec::SUPLI,
        spec::SDMI,
        spec::REGI,
        spec::MSI,
        spec::DIVERTI,
        spec::TSI,
        spec::DISPATI,
        spec::STUNI,
        spec::AUTHI,
        spec::GPI,
        spec::KILLI,
        spec::PSTNDI,
        spec::DISPATDI,
        spec::ALLMSI,
        spec::IPDI,
        spec::DGNAI,
        spec::TATTSI,
    ] {
        assert!(p::is_gateway(g), "{g:#x} is a gateway");
        assert!(!p::is_allcall(g), "{g:#x} isn't an all-call");
    }
    for a in [spec::ALLMSIDL, spec::ALLMSIDZ, spec::ALLMSID] {
        assert!(p::is_allcall(a), "{a:#x} is an all-call (7.1.1.1.1.2)");
        assert!(!p::is_gateway(a));
    }
    for id in [1, 1001, 9, 0xFFFEBF] {
        assert!(!p::is_gateway(id) && !p::is_allcall(id), "{id:#x}");
    }
}

/// 361-4 7.2.12, table 7.49: the Service_Kind codes we use.
#[test]
fn service_kinds_table_7_49() {
    assert_eq!(p::SK_IND_VOICE, spec::SK_IND_VOICE);
    assert_eq!(p::SK_TG_VOICE, spec::SK_TG_VOICE);
    assert_eq!(p::SK_CALL_ANSWER, spec::SK_CALL_ANSWER);
    assert_eq!(p::SK_REG, spec::SK_REG);
    assert_eq!(p::SK_CANCEL, spec::SK_CANCEL);
}

/// 361-4 7.2.8, tables 7.42 to 7.45: the Reason codes we use.
#[test]
fn reason_codes_7_2_8() {
    let pairs = [
        (p::R_MESSAGE_ACCEPTED, spec::MESSAGE_ACCEPTED),
        (p::R_REG_ACCEPTED, spec::REG_ACCEPTED),
        (p::R_REG_SUBSCRIPTION, spec::REG_SUBSCRIPTION),
        (p::R_MS_ACCEPTED, spec::MS_ACCEPTED),
        (p::R_CALLBACK, spec::CALLBACK),
        (p::R_MS_ALERTING, spec::MS_ALERTING),
        (p::R_NOT_SUPPORTED, spec::NOT_SUPPORTED),
        (p::R_PERM_USER_REFUSED, spec::PERM_USER_REFUSED),
        (p::R_NOREG_MSAWAY, spec::NOREG_MSAWAY),
        (p::R_MSAWAY, spec::MSAWAY_REFUSED),
        (p::R_SYSBUSY, spec::SYSBUSY),
        (p::R_CALL_CANCEL_REFUSED, spec::CALL_CANCEL_REFUSED),
        (p::R_REG_REFUSED, spec::REG_REFUSED),
        (p::R_REG_DENIED, spec::REG_DENIED),
        (p::R_MS_NOT_REGISTERED, spec::MS_NOT_REGISTERED),
        (p::R_CALLED_PARTY_BUSY, spec::CALLED_PARTY_BUSY),
        (p::R_GROUP_NOT_ALLOWED, spec::GROUP_NOT_ALLOWED),
        (p::R_EQUIP_BUSY, spec::EQUIP_BUSY),
        (p::R_RECIPIENT_REFUSED, spec::RECIPIENT_REFUSED),
        (p::R_QUEUED, spec::QUEUED_RESOURCE),
        (p::R_QUEUED_BUSY, spec::QUEUED_BUSY),
        (p::R_WAIT, spec::WAIT),
    ];
    for (ours, std) in pairs {
        assert_eq!(ours, std, "reason {std:#04x}");
    }
}

/// 361-4 7.2.8.0: tt = 00 NACK, 01 ACK, 10 QACK, 11 WACK.
#[test]
fn reason_type_tt() {
    assert_eq!(p::reason_type(spec::REG_DENIED), 0b00);
    assert_eq!(p::reason_type(spec::EQUIP_BUSY), 0b00);
    assert_eq!(p::reason_type(spec::MESSAGE_ACCEPTED), 0b01);
    assert_eq!(p::reason_type(spec::MS_ALERTING), 0b01);
    assert_eq!(p::reason_type(spec::QUEUED_RESOURCE), 0b10);
    assert_eq!(p::reason_type(spec::QUEUED_BUSY), 0b10);
    assert_eq!(p::reason_type(spec::WAIT), 0b11);
}

/// 361-4 7.2.19, table 7.68: the Announcement_type codes.
#[test]
fn announcement_types_table_7_68() {
    assert_eq!(p::BC_ANN_WD_TSCC, spec::ANN_WD_TSCC);
    assert_eq!(p::BC_CALL_TIMERS, spec::CALL_TIMER_PARMS);
    assert_eq!(p::BC_VOTE_NOW, spec::VOTE_NOW);
    assert_eq!(p::BC_LOCAL_TIME, spec::LOCAL_TIME);
    assert_eq!(p::BC_MASS_REG, spec::MASS_REG);
    assert_eq!(p::BC_CHAN_FREQ, spec::CHAN_FREQ);
    assert_eq!(p::BC_ADJACENT_SITE, spec::ADJACENT_SITE);
    assert_eq!(p::BC_GEN_SITE_PARAMS, spec::GEN_SITE_PARAMS);
}

/// 361-4 tables 7.33 and 7.34: SLCO 0010b and 0011b.
#[test]
fn slco_tables_7_33_7_34() {
    assert_eq!(p::SLCO_C_SYS_PARMS, spec::SLCO_C_SYS_PARMS);
    assert_eq!(p::SLCO_P_SYS_PARMS, spec::SLCO_P_SYS_PARMS);
}

// ── C_ALOHA (7.1.1.1.4, table 7.19) ──

fn aloha_bits(a: &p::Aloha) -> [u8; 10] {
    Bits::csbk(true, spec::C_ALOHA)
        .f(1, 0) // reserved
        .f(1, a.tsccas as u64)
        .f(1, a.site_sync as u64)
        .f(3, a.version as u64)
        .f(1, a.offset as u64)
        .f(1, a.active_connection as u64)
        .f(5, a.mask as u64)
        .f(2, a.service_function as u64)
        .f(4, a.nrand_wait as u64)
        .f(1, a.reg as u64)
        .f(4, a.backoff as u64)
        .f(16, a.syscode as u64)
        .f(24, a.addr as u64)
        .done()
}

#[test]
fn aloha_layout_table_7_19() {
    let cases = [
        p::Aloha {
            tsccas: false,
            site_sync: false,
            version: spec::VERSION_1_11_1,
            offset: false,
            active_connection: true,
            mask: 0,
            service_function: 0b00,
            nrand_wait: 6,
            reg: true,
            backoff: 3,
            syscode: spec::syscode(1, 5, 3, 0b11),
            addr: spec::ADRNULL,
        },
        p::Aloha {
            tsccas: true,
            site_sync: true,
            version: 0b111,
            offset: true,
            active_connection: false,
            mask: 24,
            service_function: 0b11,
            nrand_wait: 15,
            reg: false,
            backoff: 15,
            syscode: 0xFFFF,
            addr: 0xFFFFFF,
        },
        p::Aloha { mask: 1, service_function: 0b10, addr: 0x123456, syscode: 0x8001, ..Default::default() },
    ];
    for a in cases {
        let want = aloha_bits(&a);
        assert_eq!(a.encode(), want, "{a:?}");
        assert_eq!(p::Aloha::decode(&want), Some(a));
    }
}

/// 361-4 table 7.19: LB = 1, PF = 0, FID = 0; the leading reserved bit is 0.
#[test]
fn aloha_header_octets() {
    let b = p::Aloha { tsccas: true, ..Default::default() }.encode();
    assert_eq!(b[0], 0x80 | 0b01_1001);
    assert_eq!(b[1], 0x00);
    assert_eq!(field(&b, 16, 1), 0, "reserved");
    assert_eq!(field(&b, 17, 1), 1, "TSCCAS follows the reserved bit");
}

/// 361-4 7.1.1: a block whose FID isn't 0 (a manufacturer's) isn't a standard C_ALOHA.
#[test]
fn aloha_other_fid_not_read() {
    let mut b = p::Aloha::default().encode();
    b[1] = 0x10;
    assert_eq!(p::Aloha::decode(&b), None);
}

// ── Acknowledgements (7.1.1.1.7 table 7.23, 7.1.1.2.3 table 7.27) ──

#[test]
fn ackd_layout_table_7_23() {
    let a = p::Ack {
        op: spec::C_ACKD,
        response_info: 0b101_1010,
        reason: spec::REG_ACCEPTED,
        target: 1001,
        source: spec::REGI,
    };
    let want = Bits::csbk(true, spec::C_ACKD)
        .f(7, 0b101_1010)
        .f(8, spec::REG_ACCEPTED as u64)
        .f(1, 0)
        .f(24, 1001)
        .f(24, spec::REGI as u64)
        .done();
    assert_eq!(a.encode(), want);
    assert_eq!(p::Ack::decode(&want), Some(a));
}

#[test]
fn acku_layout_table_7_27() {
    // the called MS accepts a C_AHOY from MS 2002: target = the TS PDU's source, source = the MS
    let want = Bits::csbk(true, spec::C_ACKU)
        .f(7, 0)
        .f(8, spec::MS_ACCEPTED as u64)
        .f(1, 0)
        .f(24, 2002)
        .f(24, 3003)
        .done();
    let a = p::Ack::decode(&want).expect("C_ACKU reads");
    assert_eq!((a.op, a.reason, a.target, a.source), (spec::C_ACKU, spec::MS_ACCEPTED, 2002, 3003));
    assert_eq!(a.encode(), want);
}

/// 7.2.8: a C_NACKD, C_QACKD and C_WACKD are C_ACKD blocks with another Reason type.
#[test]
fn ack_kinds_share_opcode() {
    for r in [spec::REG_DENIED, spec::QUEUED_RESOURCE, spec::WAIT, spec::MESSAGE_ACCEPTED] {
        let b = p::Ack { op: spec::C_ACKD, reason: r, target: 7, source: 8, ..Default::default() }.encode();
        assert_eq!(csbko(&b), spec::C_ACKD);
        assert_eq!(field(&b, 23, 8) as u8, r);
    }
}

// ── Channel grants (7.1.1.1.1, tables 7.9, 7.11, 7.12) ──

fn grant_bits(op: u8, chan: u16, lcn: u8, bit: bool, emerg: bool, offset: bool, tgt: u32, src: u32) -> [u8; 10] {
    Bits::csbk(true, op)
        .f(12, chan as u64)
        .f(1, lcn as u64)
        .f(1, bit as u64)
        .f(1, emerg as u64)
        .f(1, offset as u64)
        .f(24, tgt as u64)
        .f(24, src as u64)
        .done()
}

#[test]
fn tv_grant_layout_table_7_11() {
    for (late, emerg, lcn) in [(false, false, 0), (true, false, 1), (false, true, 1), (true, true, 0)] {
        let g = p::Grant {
            op: spec::TV_GRANT,
            chan: 4094,
            lcn,
            late_entry: late,
            emergency: emerg,
            offset: false,
            target: 9,
            source: 1001,
        };
        let want = grant_bits(spec::TV_GRANT, 4094, lcn, late, emerg, false, 9, 1001);
        assert_eq!(g.encode(), want, "{g:?}");
        assert_eq!(p::Grant::decode(&want), Some(g));
    }
}

#[test]
fn btv_grant_layout_table_7_12() {
    let g = p::Grant {
        op: spec::BTV_GRANT,
        chan: 1,
        lcn: 1,
        late_entry: true,
        emergency: false,
        offset: true,
        target: spec::ALLMSID,
        source: 1001,
    };
    let want = grant_bits(spec::BTV_GRANT, 1, 1, true, false, true, spec::ALLMSID, 1001);
    assert_eq!(g.encode(), want);
    assert_eq!(p::Grant::decode(&want), Some(g));
}

/// Table 7.9: PV_GRANT has a reserved bit (0) where TV_GRANT has Late_Entry, whatever is asked.
#[test]
fn pv_grant_reserved_bit_table_7_9() {
    let g = p::Grant {
        op: spec::PV_GRANT,
        chan: 2,
        lcn: 0,
        late_entry: true,
        emergency: true,
        offset: false,
        target: 2002,
        source: 1001,
    };
    let b = g.encode();
    assert_eq!(b, grant_bits(spec::PV_GRANT, 2, 0, false, true, false, 2002, 1001));
    assert_eq!(field(&b, 29, 1), 0, "reserved");
}

/// Table 7.9: LB = 1 for a single block CSBK.
#[test]
fn grants_are_single_blocks() {
    for op in [spec::PV_GRANT, spec::TV_GRANT, spec::BTV_GRANT] {
        let b = p::Grant { op, chan: 3, target: 1, source: 2, ..Default::default() }.encode();
        assert!(lb(&b));
        assert_eq!((csbko(&b), b[1]), (op, 0));
    }
}

// ── C_AHOY (7.1.1.1.6, table 7.22) ──

#[test]
fn ahoy_layout_table_7_22() {
    let a = p::Ahoy {
        service_options: 0b100_0011,
        service_kind_flag: true,
        als: false,
        group: false,
        appended: 0,
        service_kind: spec::SK_IND_VOICE,
        target: 2002,
        source: 1001,
    };
    let want = Bits::csbk(true, spec::C_AHOY)
        .f(7, 0b100_0011)
        .f(1, 1)
        .f(1, 0)
        .f(1, 0)
        .f(2, 0)
        .f(4, spec::SK_IND_VOICE as u64)
        .f(24, 2002)
        .f(24, 1001)
        .done();
    assert_eq!(a.encode(), want);
    assert_eq!(p::Ahoy::decode(&want), Some(a));
}

/// 6.6.1.2.2, table 6.37: the C_AHOY that cancels a call.
#[test]
fn ahoy_cancel_table_6_37() {
    let want = Bits::csbk(true, spec::C_AHOY)
        .f(7, 0)
        .f(1, 0)
        .f(1, 0)
        .f(1, 0)
        .f(2, 0)
        .f(4, spec::SK_CANCEL as u64)
        .f(24, 2002)
        .f(24, 1001)
        .done();
    let a = p::Ahoy::decode(&want).unwrap();
    assert_eq!((a.service_kind, a.service_kind_flag, a.group, a.target, a.source), (spec::SK_CANCEL, false, false, 2002, 1001));
    assert_eq!(a.encode(), want);
}

/// Table 7.22: a talkgroup radio check has G/I = 1.
#[test]
fn ahoy_group_bit() {
    let b = p::Ahoy { group: true, service_kind: spec::SK_TG_VOICE, target: 9, source: 1, ..Default::default() }.encode();
    assert_eq!(field(&b, 16 + 9, 1), 1, "G/I");
    assert_eq!(field(&b, 16 + 12, 4) as u8, spec::SK_TG_VOICE);
}

// ── C_RAND (7.1.1.2.1, table 7.25) ──

fn rand_bits(so: u8, proxy: bool, dep: u8, sk: u8, tgt: u32, src: u32) -> [u8; 10] {
    Bits::csbk(true, spec::C_RAND)
        .f(7, so as u64)
        .f(1, proxy as u64)
        .f(4, dep as u64)
        .f(4, sk as u64)
        .f(24, tgt as u64)
        .f(24, src as u64)
        .done()
}

#[test]
fn rand_layout_table_7_25() {
    let r = p::Rand { service_options: 0b100_1011, proxy: true, dep: 0b1010, service_kind: 0b0001, target: 9, source: 1001 };
    let want = rand_bits(0b100_1011, true, 0b1010, 0b0001, 9, 1001);
    assert_eq!(r.encode(), want);
    assert_eq!(p::Rand::decode(&want), Some(r));
}

/// Table 7.52: voice Service_Options are EMERG (b6), Privacy (b5), SUPED (b4), BCAST (b3), reserved (b2),
/// PRIORITY (b1-b0).
#[test]
fn voice_service_options_table_7_52() {
    assert_eq!(p::SO_EMERGENCY, 0b100_0000);
    assert_eq!(p::SO_BROADCAST, 0b000_1000);
    assert_eq!(p::SO_PRIORITY, 0b000_0011);
    let r = p::Rand::decode(&rand_bits(0b100_1011, false, 0, spec::SK_TG_VOICE, 9, 1)).unwrap();
    assert!(r.service_options & p::SO_EMERGENCY != 0);
    assert!(r.service_options & p::SO_BROADCAST != 0);
    assert_eq!(r.service_options & p::SO_PRIORITY, 3);
}

/// Table 7.55 and 6.4.4.1, table 6.9: registration Service_Options are reserved (b6), Privacy (b5), IP_Inform
/// (b4), PowerSave_RQ (b3-b1), Reg_Dereg (b0).
#[test]
fn registration_service_options_table_7_55() {
    assert_eq!(p::SO_IP_INFORM, 0b001_0000);
    assert_eq!(p::SO_PS_RQ, 0b000_1110);
    assert_eq!(p::SO_REG, 0b000_0001);
}

/// Table 6.9: a registration to REG_ADDR (0000 0000b + C_SYScode), TRGT_ADR_CNTS 00b.
#[test]
fn rand_registration_table_6_9() {
    let sc = spec::syscode(1, 1, 1, 3);
    let b = rand_bits(0b000_0001, false, 0b0000, spec::SK_REG, sc as u32, 1001);
    let r = p::Rand::decode(&b).unwrap();
    assert_eq!(r.service_kind, spec::SK_REG);
    assert_eq!(r.trgt_adr_cnts(), 0b00);
    assert_eq!(r.target, sc as u32);
    assert_eq!(r.target >> 16, 0, "REG_ADDR's top octet is 0");
    // with a talkgroup subscription: TRGT_ADR_CNTS 01b, the talkgroup in the target
    let b = rand_bits(0b000_0001, false, 0b0100, spec::SK_REG, 9, 1001);
    assert_eq!(p::Rand::decode(&b).unwrap().trgt_adr_cnts(), 0b01);
    // a list: 10b, TATTSI, one appended block (01b = blocks minus one)
    let b = rand_bits(0b000_0001, false, 0b1001, spec::SK_REG, spec::TATTSI, 1001);
    assert_eq!(p::Rand::decode(&b).unwrap().trgt_adr_cnts(), 0b10);
}

/// 6.6.1.4.0.2, table 6.38: the Call Answer's 4 bits are reserved (2), ACCEPT (1: 0 accepts), reserved (1).
#[test]
fn rand_call_answer_table_6_38() {
    let yes = p::Rand::decode(&rand_bits(0, false, 0b0000, spec::SK_CALL_ANSWER, 1001, 2002)).unwrap();
    assert!(yes.accepted());
    let no = p::Rand::decode(&rand_bits(0, false, 0b0010, spec::SK_CALL_ANSWER, 1001, 2002)).unwrap();
    assert!(!no.accepted());
    // the reserved bits around ACCEPT don't change it
    let r = p::Rand::decode(&rand_bits(0, false, 0b1101, spec::SK_CALL_ANSWER, 1001, 2002)).unwrap();
    assert!(r.accepted());
}

// ── Payload channel PDUs ──

/// 7.1.1.3.2, table 7.30: P_CLEAR to ALLMSI from TSI, back to the TSCC (channel 0), G/I = 0 for ALLMSI.
#[test]
fn p_clear_layout_table_7_30() {
    let c = p::Clear { chan: 0, group: false, target: spec::ALLMSI, source: spec::TSI };
    let want = Bits::csbk(true, spec::P_CLEAR)
        .f(12, 0)
        .f(1, 0)
        .f(2, 0)
        .f(1, 0)
        .f(24, spec::ALLMSI as u64)
        .f(24, spec::TSI as u64)
        .done();
    assert_eq!(c.encode(), want);
    assert_eq!(p::Clear::decode(&want), Some(c));
    let g = p::Clear { chan: 0x123, group: true, target: 9, source: spec::TSI };
    let want = Bits::csbk(true, spec::P_CLEAR).f(12, 0x123).f(3, 0).f(1, 1).f(24, 9).f(24, spec::TSI as u64).done();
    assert_eq!(g.encode(), want);
}

/// 7.1.1.3.3, table 7.31: P_PROTECT, 12 reserved bits, Protect_Kind (table 7.82: EN_PTT_ONE_MS 011b), G/I, target,
/// source.
#[test]
fn p_protect_layout_table_7_31() {
    let pr = p::Protect { kind: p::PROTECT_EN_PTT_ONE_MS, group: false, target: 1002, source: spec::TSI };
    let want = Bits::csbk(true, spec::P_PROTECT).f(12, 0).f(3, 0b011).f(1, 0).f(24, 1002).f(24, spec::TSI as u64).done();
    assert_eq!(pr.encode(), want);
    assert_eq!(p::Protect::decode(&want), Some(pr));
    assert_eq!((p::PROTECT_DIS_PTT, p::PROTECT_EN_PTT, p::PROTECT_ILLEGALLY_PARKED), (0b000, 0b001, 0b010));
    let g = p::Protect { kind: p::PROTECT_DIS_PTT, group: true, target: 9, source: 1001 };
    let want = Bits::csbk(true, spec::P_PROTECT).f(12, 0).f(3, 0).f(1, 1).f(24, 9).f(24, 1001).done();
    assert_eq!(g.encode(), want);
}

/// 7.1.1.4.3, table 7.32: P_MAINT (Maint_Kind 000b DISCON) to TSI from the MS.
#[test]
fn p_maint_layout_table_7_32() {
    let want = Bits::csbk(true, spec::P_MAINT)
        .f(12, 0)
        .f(3, 0)
        .f(1, 0)
        .f(24, spec::TSI as u64)
        .f(24, 1001)
        .done();
    let m = p::Maint::decode(&want).unwrap();
    assert_eq!((m.kind, m.target, m.source), (p::MAINT_DISCON, spec::TSI, 1001));
    assert_eq!(m.encode(), want);
}

// ── C_BCAST (7.1.1.1.5, table 7.20, and 7.2.19) ──

fn bcast_bits(lbit: bool, ann: u8, p1: u16, reg: bool, backoff: u8, sc: u16, p2: u32) -> [u8; 10] {
    Bits::csbk(lbit, spec::C_BCAST)
        .f(5, ann as u64)
        .f(14, p1 as u64)
        .f(1, reg as u64)
        .f(4, backoff as u64)
        .f(16, sc as u64)
        .f(24, p2 as u64)
        .done()
}

#[test]
fn bcast_layout_table_7_20() {
    let b = p::Bcast { lb: true, ann: 7, parms1: 0x2ABC, reg: true, backoff: 9, syscode: 0x4321, parms2: 0xA5A5A5 };
    let want = bcast_bits(true, 7, 0x2ABC, true, 9, 0x4321, 0xA5A5A5);
    assert_eq!(b.encode(), want);
    assert_eq!(p::Bcast::decode(&want), Some(b));
}

/// 7.2.19.7, table 7.79: Parms1 = the neighbor's 14 syscode MSBs; Parms2 = info available, Active_Connection,
/// confirmed priority (3), adjacent priority (3), reserved (4), CH_ADJ (12).
#[test]
fn bcast_adjacent_site_table_7_79() {
    let sc = spec::syscode(1, 1, 1, 3);
    let n = spec::syscode(1, 1, 2, 3);
    let b = p::Bcast::adjacent_site(true, 3, sc, n, Some(true), 5, 2, 0x0AB).encode();
    let p2 = (1 << 23) | (1 << 22) | (5 << 19) | (2 << 16) | 0x0AB;
    assert_eq!(b, bcast_bits(true, spec::ADJACENT_SITE, n >> 2, true, 3, sc, p2));
    // Active_Connection unknown: both bits 0
    let b = p::Bcast::adjacent_site(false, 4, sc, n, None, 0, 0, 2).encode();
    assert_eq!(b, bcast_bits(true, spec::ADJACENT_SITE, n >> 2, false, 4, sc, 2));
}

/// 7.2.19.6, table 7.78: Chan_Freq is an MBC header (LB = 0); Parms1 0; Parms2 = 12 reserved bits, the channel.
#[test]
fn bcast_chan_freq_table_7_78() {
    let sc = spec::syscode(1, 1, 1, 3);
    let b = p::Bcast::chan_freq(true, 3, sc, 1).encode();
    assert_eq!(b, bcast_bits(false, spec::CHAN_FREQ, 0, true, 3, sc, 1));
    assert!(!lb(&b));
}

/// 7.2.19.8, table 7.80, 7.2.40, 7.2.41: Parms2 = site information (reserved, Hibernating, 6 reserved),
/// 8 reserved, network information (bit 7: registration without a talkgroup).
#[test]
fn bcast_gen_site_params_table_7_80() {
    let sc = spec::syscode(2, 3, 0x81, 1);
    let b = p::Bcast::gen_site_params(false, 5, sc, true, false).encode();
    assert_eq!(b, bcast_bits(true, spec::GEN_SITE_PARAMS, 0, false, 5, sc, 0b0100_0000 << 16));
    let b = p::Bcast::gen_site_params(true, 5, sc, false, true).encode();
    assert_eq!(b, bcast_bits(true, spec::GEN_SITE_PARAMS, 0, true, 5, sc, 0b1000_0000));
}

/// 7.2.19.4, tables 7.73 to 7.75: Parms1 = day (5), month (4), UTC offset hours (4), sign (1: local lags UTC);
/// Parms2 = hours (5), minutes (6), seconds (6), day of week (3), offset fraction (2: 15 min steps), reserved (2).
#[test]
fn bcast_local_time_table_7_73() {
    let sc = spec::syscode(1, 1, 1, 3);
    // Thursday 2026-11-05 16:04:11 local, UTC-5:00
    let t = p::Civil { year: 2026, month: 11, day: 5, hour: 16, minute: 4, second: 11, weekday: 5 };
    let b = p::Bcast::local_time(true, 3, sc, &t, Some(-300)).encode();
    let p1 = (5 << 9) | (11 << 5) | (5 << 1) | 1;
    let p2 = (16 << 19) | (4 << 13) | (11 << 7) | (5 << 4);
    assert_eq!(b, bcast_bits(true, spec::LOCAL_TIME, p1, true, 3, sc, p2));
    // UTC+5:45: offset 5, sign 0, fraction 11b (45 min)
    let b = p::Bcast::local_time(true, 3, sc, &t, Some(345)).encode();
    let p1 = (5 << 9) | (11 << 5) | (5 << 1);
    let p2 = (16 << 19) | (4 << 13) | (11 << 7) | (5 << 4) | (0b11 << 2);
    assert_eq!(b, bcast_bits(true, spec::LOCAL_TIME, p1, true, 3, sc, p2));
    // no offset given: UTC_OFFSET 1111b (table 7.73: not sent)
    let b = p::Bcast::local_time(true, 3, sc, &t, None).encode();
    assert_eq!(field(&b, 16 + 5 + 9, 4), 0b1111);
}

/// 7.1.1.1.5.1, table 7.21 and 7.2.42, table 7.103: BC_AP is LB 1, PF, the header's CSBKO (C_BCAST), 8 reserved
/// bits, Cdeftype 0000b, 2 reserved bits, then CHAN (12), TXMHz (10), TXKHz (13, 125 Hz steps), RXMHz, RXKHz.
#[test]
fn bc_ap_layout_table_7_21() {
    let a = p::BcAp { chan: 1, tx_hz: 438_250_000, rx_hz: 438_750_000 };
    let want = Bits::new()
        .f(1, 1)
        .f(1, 0)
        .f(6, spec::C_BCAST as u64)
        .f(8, 0)
        .f(4, 0)
        .f(2, 0)
        .f(12, 1)
        .f(10, 438)
        .f(13, 250_000 / 125)
        .f(10, 438)
        .f(13, 750_000 / 125)
        .done();
    assert_eq!(a.encode(), want);
    assert_eq!(p::BcAp::decode(&want), Some(a));
    // 125 Hz resolution at the top of the range (annex C, table C.7: 50 to 999 MHz)
    let a = p::BcAp { chan: 4094, tx_hz: 999_999_875, rx_hz: 50_000_125 };
    let b = a.encode();
    assert_eq!(field(&b, 34, 10), 999);
    assert_eq!(field(&b, 44, 13), 7999);
    assert_eq!(field(&b, 57, 10), 50);
    assert_eq!(field(&b, 67, 13), 1);
    assert_eq!(p::BcAp::decode(&b), Some(a));
}

/// Table 7.103: Cdeftype 0001b-1111b is reserved: not read as frequencies.
#[test]
fn bc_ap_reserved_cdeftype() {
    let b = Bits::new().f(1, 1).f(1, 0).f(6, spec::C_BCAST as u64).f(8, 0).f(4, 1).f(2, 0).f(58, 0).done();
    assert_eq!(p::BcAp::decode(&b), None);
}

// ── Short LC (7.1.2, tables 7.33, 7.34) ──

/// C_SYS_Parms: SLCO (4), MODEL + NET + SITE (14: the syscode's MSBs), Reg (1), Common_Slot_Counter (9).
#[test]
fn c_sys_parms_table_7_33() {
    let sc = spec::syscode(1, 5, 3, 3);
    let slc = p::c_sys_parms(sc, true, 300);
    let want = (0b0010u32 << 24) | (((sc >> 2) as u32) << 10) | (1 << 9) | 300;
    assert_eq!(slc, want);
    assert_eq!(slc >> 28, 0, "28 bits");
    assert_eq!(p::sys_parms_decode(slc), (0b0010, sc >> 2, true, 300));
}

/// P_SYS_Parms: the same with Payload Channel Type (0 normal, 1 composite control channel) in Reg's place.
#[test]
fn p_sys_parms_table_7_34() {
    let sc = spec::syscode(3, 2, 1023, 1);
    let slc = p::p_sys_parms(sc, false, 511);
    assert_eq!(slc, (0b0011u32 << 24) | (((sc >> 2) as u32) << 10) | 511);
    let slc = p::p_sys_parms(sc, true, 0);
    assert_eq!(slc, (0b0011u32 << 24) | (((sc >> 2) as u32) << 10) | (1 << 9));
}

// ── C_SYScode (6.3.2.2.1.1, table 6.5, figure 6.15; 7.2.7) ──

#[test]
fn syscode_models_figure_6_15() {
    // tiny: NET 9 bits, SITE 3; small 7/5; large 4/8; huge 2/10
    let cases = [
        (0u8, 511u32, 7u32, 1u8, 0b00_111111111_111_01u16),
        (0, 0x0A5, 2, 3, 0b00_010100101_010_11),
        (1, 0x55, 0x13, 2, 0b01_1010101_10011_10),
        (2, 0x9, 0xC3, 3, 0b10_1001_11000011_11),
        (3, 2, 0x2AA, 1, 0b11_10_1010101010_01),
    ];
    for (model, net, site, par, want) in cases {
        assert_eq!(spec::syscode(model, net, site, par), want, "the hand-built value");
        let c = p::SysCode { model, net: net as u16, site: site as u16, par };
        assert!(c.valid(), "{c:?}");
        assert_eq!(c.encode(), want, "{c:?}");
        assert_eq!(p::SysCode::decode(want), c);
        assert_eq!(p::model_bits(model), spec::model_widths(model));
    }
}

/// Table 6.5: 512/128/16/4 networks and 8/32/256/1024 sites per model; PAR 00b is reserved (6.3.2.2.1.4).
#[test]
fn syscode_ranges_table_6_5() {
    for model in 0..4u8 {
        let (nb, sb) = spec::model_widths(model);
        let ok = |net: u32, site: u32, par: u8| p::SysCode { model, net: net as u16, site: site as u16, par }.valid();
        assert!(ok((1 << nb) - 1, (1 << sb) - 1, 3), "model {model}: the top NET and SITE");
        assert!(!ok(1 << nb, 1, 3), "model {model}: NET too big");
        assert!(!ok(1, 1 << sb, 3), "model {model}: SITE too big");
        assert!(!ok(1, 1, 0), "PAR 00b is reserved");
        for par in 1..=3 {
            assert!(ok(0, 1, par));
        }
    }
}

/// 7.2.7, table 7.41: Response_Check is the six LSBs of NET + SITE (bits 8 to 3 of figure 6.19).
#[test]
fn response_check_table_7_41() {
    for (model, net, site, par) in [(1u8, 5u32, 3u32, 3u8), (2, 9, 0xC3, 1), (0, 0x1FF, 7, 2), (3, 1, 0x3FF, 3)] {
        let sc = spec::syscode(model, net, site, par);
        let want = ((sc >> 2) & 0x3F) as u8;
        let c = p::SysCode { model, net: net as u16, site: site as u16, par };
        assert_eq!(c.response_check(), want, "{c:?}");
    }
}

// ── Random access coding (6.2.1.1.5 table 6.2, 6.2.1.1.6 table 6.3) ──

#[test]
fn nrand_wait_table_6_2() {
    for code in 0..16u8 {
        assert_eq!(p::nwait_frames(code), spec::NWAIT[code as usize], "NRand_Wait {code}");
    }
}

#[test]
fn backoff_table_6_3() {
    for code in 1..16u8 {
        assert_eq!(p::backoff_frames(code), spec::BACKOFF[code as usize], "Backoff {code}");
    }
}

// ── Local time (7.2.19.4) ──

/// Table 7.75: day of week 1 = Sunday .. 7 = Saturday.
#[test]
fn day_of_week_table_7_75() {
    // 1970-01-01 and 2026-11-05 were Thursdays (5), 2026-11-18 a Wednesday (4), 2000-02-29 a Tuesday (3)
    let c = p::civil_from_ms(0);
    assert_eq!((c.year, c.month, c.day, c.weekday), (1970, 1, 1, 5));
    let c = p::civil_from_ms(1_793_912_651_000);
    assert_eq!((c.year, c.month, c.day, c.hour, c.minute, c.second, c.weekday), (2026, 11, 5, 21, 4, 11, 5));
    let c = p::civil_from_ms(1_795_000_000_000);
    assert_eq!((c.month, c.day, c.weekday), (11, 18, 4));
    let c = p::civil_from_ms(951_782_400_000);
    assert_eq!((c.year, c.month, c.day, c.weekday), (2000, 2, 29, 3));
}
