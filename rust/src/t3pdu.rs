// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! DMR Tier III PDUs (ETSI TS 102 361-4): the 10 information bytes of a CSBK, MBC header or MBC continuation block,
//! before the CRC, and the 28-bit system parameter short LCs, before the CRC-8. The radio layer adds the CRCs, the
//! BPTC and the slot type. Clause and table numbers are 361-4 V1.12.1's.

/// Data types of the blocks the site sends and reads (361-1 slot type).
pub const DT_CSBK: u8 = 3;
pub const DT_MBC_HDR: u8 = 4;
pub const DT_MBC_CONT: u8 = 5;

// Opcodes (annex B, table B.1)
pub const OP_ALOHA: u8 = 25;
pub const OP_AHOY: u8 = 28;
pub const OP_RAND: u8 = 31;
pub const OP_ACKD: u8 = 32;
pub const OP_ACKU: u8 = 33;
pub const OP_BCAST: u8 = 40;
pub const OP_MAINT: u8 = 42;
pub const OP_CLEAR: u8 = 46;
pub const OP_PROTECT: u8 = 47;
pub const OP_PV_GRANT: u8 = 48;
pub const OP_TV_GRANT: u8 = 49;
pub const OP_BTV_GRANT: u8 = 50;
pub const OP_MOVE: u8 = 57;

// Gateways and identifiers (annex A.4, table A.8)
pub const ADRNULL: u32 = 0;
pub const PSTNI: u32 = 0xFFFEC0;
pub const REGI: u32 = 0xFFFEC6;
pub const TSI: u32 = 0xFFFECA;
pub const ALLMSI: u32 = 0xFFFED4;
pub const TATTSI: u32 = 0xFFFED7;
pub const ALLMSIDL: u32 = 0xFFFFFD;
pub const ALLMSIDZ: u32 = 0xFFFFFE;
pub const ALLMSID: u32 = 0xFFFFFF;

/// A gateway or service identifier (PSTNI .. TATTSI): not an MS or a talkgroup.
pub fn is_gateway(a: u32) -> bool {
    (PSTNI..=TATTSI).contains(&a)
}

/// An all-call talkgroup (ALLMSIDL, ALLMSIDZ, ALLMSID): its grants are broadcasts (7.1.1.1.1.2).
pub fn is_allcall(a: u32) -> bool {
    a >= ALLMSIDL
}

// Service_Kind (7.2.12, table 7.49)
pub const SK_IND_VOICE: u8 = 0b0000;
pub const SK_TG_VOICE: u8 = 0b0001;
pub const SK_STATUS: u8 = 0b0111;
pub const SK_CALL_ANSWER: u8 = 0b1001;
pub const SK_REG: u8 = 0b1110;
pub const SK_CANCEL: u8 = 0b1111;

// Reasons (7.2.8): tt d aaaaa
pub const R_MESSAGE_ACCEPTED: u8 = 0x60;
pub const R_REG_ACCEPTED: u8 = 0x62;
pub const R_REG_SUBSCRIPTION: u8 = 0x65; // registration accepted with talkgroup subscription (Response_Info pattern)
pub const R_MS_ACCEPTED: u8 = 0x44;
pub const R_CALLBACK: u8 = 0x45;
pub const R_MS_ALERTING: u8 = 0x46;
pub const R_NOT_SUPPORTED: u8 = 0x20;
pub const R_PERM_USER_REFUSED: u8 = 0x21;
pub const R_NOREG_MSAWAY: u8 = 0x24;
pub const R_MSAWAY: u8 = 0x25;
pub const R_SYSBUSY: u8 = 0x27;
pub const R_CALL_CANCEL_REFUSED: u8 = 0x29;
pub const R_REG_REFUSED: u8 = 0x2A;
pub const R_REG_DENIED: u8 = 0x2B;
pub const R_MS_NOT_REGISTERED: u8 = 0x2D;
pub const R_CALLED_PARTY_BUSY: u8 = 0x2E;
pub const R_GROUP_NOT_ALLOWED: u8 = 0x2F;
pub const R_EQUIP_BUSY: u8 = 0x13; // from an MS
pub const R_RECIPIENT_REFUSED: u8 = 0x14; // from an MS (FOACSU), mirrored by the TS
pub const R_QUEUED: u8 = 0xA0;
pub const R_QUEUED_BUSY: u8 = 0xA1;
pub const R_WAIT: u8 = 0xE0;

/// The ACK type of a reason: 0 NACK, 1 ACK, 2 QACK, 3 WACK.
pub fn reason_type(r: u8) -> u8 {
    r >> 6
}

// C_BCAST Announcement_type (7.2.19, table 7.68)
pub const BC_ANN_WD_TSCC: u8 = 0;
pub const BC_CALL_TIMERS: u8 = 1;
pub const BC_VOTE_NOW: u8 = 2;
pub const BC_LOCAL_TIME: u8 = 3;
pub const BC_MASS_REG: u8 = 4;
pub const BC_CHAN_FREQ: u8 = 5;
pub const BC_ADJACENT_SITE: u8 = 6;
pub const BC_GEN_SITE_PARAMS: u8 = 7;

// Short LC opcodes (361-1 7.1.2, 361-4 7.1.2)
pub const SLCO_C_SYS_PARMS: u8 = 0b0010;
pub const SLCO_P_SYS_PARMS: u8 = 0b0011;

/// 10 bytes written MSB first.
struct W {
    b: [u8; 10],
    pos: usize,
}

impl W {
    fn new() -> W {
        W { b: [0; 10], pos: 0 }
    }
    fn put(&mut self, n: usize, v: u32) -> &mut W {
        for i in (0..n).rev() {
            if (v >> i) & 1 != 0 {
                self.b[self.pos / 8] |= 0x80 >> (self.pos % 8);
            }
            self.pos += 1;
        }
        self
    }
    fn flag(&mut self, v: bool) -> &mut W {
        self.put(1, v as u32)
    }
    /// LB, PF (0), the opcode and the FID (0): octets 0 and 1.
    fn head(lb: bool, op: u8) -> W {
        let mut w = W::new();
        w.flag(lb).put(1, 0).put(6, op as u32).put(8, 0);
        w
    }
    fn done(&self) -> [u8; 10] {
        debug_assert!(self.pos == 80);
        self.b
    }
}

/// 10 bytes read MSB first.
struct R<'a> {
    b: &'a [u8; 10],
    pos: usize,
}

impl R<'_> {
    fn get(&mut self, n: usize) -> u32 {
        let mut v = 0;
        for _ in 0..n {
            v = (v << 1) | ((self.b[self.pos / 8] >> (7 - self.pos % 8)) & 1) as u32;
            self.pos += 1;
        }
        v
    }
    fn flag(&mut self) -> bool {
        self.get(1) != 0
    }
}

fn reader(b: &[u8; 10]) -> R<'_> {
    R { b, pos: 16 }
}

/// The opcode of a block (octet 0's six LSBs).
pub fn opcode(b: &[u8; 10]) -> u8 {
    b[0] & 0x3F
}

/// The last block flag.
pub fn last_block(b: &[u8; 10]) -> bool {
    b[0] & 0x80 != 0
}

/// The feature set ID (octet 1): 0 for the standard feature set.
pub fn fid(b: &[u8; 10]) -> u8 {
    b[1]
}

// ── C_SYScode (6.3.2.2.1, 7.2.6) ──────────────────────────────────────────────

pub const MODEL_TINY: u8 = 0;
pub const MODEL_SMALL: u8 = 1;
pub const MODEL_LARGE: u8 = 2;
pub const MODEL_HUGE: u8 = 3;

/// NET bits and SITE bits of a model (figure 6.15).
pub fn model_bits(model: u8) -> (u32, u32) {
    match model & 3 {
        MODEL_TINY => (9, 3),
        MODEL_SMALL => (7, 5),
        MODEL_LARGE => (4, 8),
        _ => (2, 10),
    }
}

/// The system identity: MODEL (2) | NET | SITE (12 together) | PAR (2).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct SysCode {
    pub model: u8,
    pub net: u16,
    pub site: u16,
    pub par: u8,
}

impl SysCode {
    /// Whether NET and SITE fit the model, and SITE isn't 0 (sites count from 1) and PAR isn't the reserved 00b.
    pub fn valid(&self) -> bool {
        let (nb, sb) = model_bits(self.model);
        self.model <= 3 && (self.net as u32) < (1 << nb) && (self.site as u32) < (1 << sb) && self.par >= 1 && self.par <= 3
    }
    pub fn encode(&self) -> u16 {
        let (_, sb) = model_bits(self.model);
        let ns = ((self.net as u32) << sb) | (self.site as u32 & ((1 << sb) - 1));
        (((self.model as u32 & 3) << 14) | ((ns & 0xFFF) << 2) | (self.par as u32 & 3)) as u16
    }
    pub fn decode(v: u16) -> SysCode {
        let model = (v >> 14) as u8;
        let (_, sb) = model_bits(model);
        let ns = ((v >> 2) & 0xFFF) as u32;
        SysCode { model, net: (ns >> sb) as u16, site: (ns & ((1 << sb) - 1)) as u16, par: (v & 3) as u8 }
    }
    /// The six LSBs of NET + SITE: C_ACKD's Response_Check (7.2.7).
    pub fn response_check(&self) -> u8 {
        ((self.encode() >> 2) & 0x3F) as u8
    }
}

// ── TSCC outbound CSBKs ────────────────────────────────────────────────────────

/// C_ALOHA (7.1.1.1.4, table 7.19).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct Aloha {
    pub tsccas: bool,
    pub site_sync: bool,
    pub version: u8,
    pub offset: bool,
    pub active_connection: bool,
    pub mask: u8,
    pub service_function: u8,
    pub nrand_wait: u8,
    pub reg: bool,
    pub backoff: u8,
    pub syscode: u16,
    pub addr: u32,
}

impl Aloha {
    pub fn encode(&self) -> [u8; 10] {
        let mut w = W::head(true, OP_ALOHA);
        w.put(1, 0)
            .flag(self.tsccas)
            .flag(self.site_sync)
            .put(3, self.version as u32)
            .flag(self.offset)
            .flag(self.active_connection)
            .put(5, self.mask as u32)
            .put(2, self.service_function as u32)
            .put(4, self.nrand_wait as u32)
            .flag(self.reg)
            .put(4, self.backoff as u32)
            .put(16, self.syscode as u32)
            .put(24, self.addr);
        w.done()
    }
    pub fn decode(b: &[u8; 10]) -> Option<Aloha> {
        if opcode(b) != OP_ALOHA || fid(b) != 0 {
            return None;
        }
        let mut r = reader(b);
        r.get(1);
        Some(Aloha {
            tsccas: r.flag(),
            site_sync: r.flag(),
            version: r.get(3) as u8,
            offset: r.flag(),
            active_connection: r.flag(),
            mask: r.get(5) as u8,
            service_function: r.get(2) as u8,
            nrand_wait: r.get(4) as u8,
            reg: r.flag(),
            backoff: r.get(4) as u8,
            syscode: r.get(16) as u16,
            addr: r.get(24),
        })
    }
}

/// C_ACKD / C_NACKD / C_QACKD / C_WACKD (7.1.1.1.7, table 7.23), and the MS's C_ACKU / C_NACKU (7.1.1.2.3,
/// table 7.27), which has the same layout under opcode 33.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct Ack {
    pub op: u8,
    pub response_info: u8,
    pub reason: u8,
    pub target: u32,
    pub source: u32,
}

impl Ack {
    pub fn encode(&self) -> [u8; 10] {
        let mut w = W::head(true, self.op);
        w.put(7, self.response_info as u32).put(8, self.reason as u32).put(1, 0).put(24, self.target).put(24, self.source);
        w.done()
    }
    pub fn decode(b: &[u8; 10]) -> Option<Ack> {
        let op = opcode(b);
        if (op != OP_ACKD && op != OP_ACKU) || fid(b) != 0 {
            return None;
        }
        let mut r = reader(b);
        let response_info = r.get(7) as u8;
        let reason = r.get(8) as u8;
        r.get(1);
        Some(Ack { op, response_info, reason, target: r.get(24), source: r.get(24) })
    }
}

/// PV_GRANT, TV_GRANT, BTV_GRANT (7.1.1.1.1, tables 7.9, 7.11, 7.12), also sent on a payload channel as P_GRANT
/// (7.1.1.3.1). PV_GRANT has a reserved bit in Late_Entry's place.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct Grant {
    pub op: u8,
    pub chan: u16,
    pub lcn: u8,
    pub late_entry: bool,
    pub emergency: bool,
    pub offset: bool,
    pub target: u32,
    pub source: u32,
}

impl Grant {
    pub fn encode(&self) -> [u8; 10] {
        let late = self.late_entry && self.op != OP_PV_GRANT;
        let mut w = W::head(true, self.op);
        w.put(12, self.chan as u32)
            .put(1, self.lcn as u32)
            .flag(late)
            .flag(self.emergency)
            .flag(self.offset)
            .put(24, self.target)
            .put(24, self.source);
        w.done()
    }
    pub fn decode(b: &[u8; 10]) -> Option<Grant> {
        let op = opcode(b);
        if !(OP_PV_GRANT..=OP_BTV_GRANT).contains(&op) || fid(b) != 0 {
            return None;
        }
        let mut r = reader(b);
        let chan = r.get(12) as u16;
        let lcn = r.get(1) as u8;
        let late = r.flag();
        Some(Grant {
            op,
            chan,
            lcn,
            late_entry: late && op != OP_PV_GRANT,
            emergency: r.flag(),
            offset: r.flag(),
            target: r.get(24),
            source: r.get(24),
        })
    }
}

/// C_AHOY (7.1.1.1.6, table 7.22).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct Ahoy {
    pub service_options: u8,
    pub service_kind_flag: bool,
    pub als: bool,
    pub group: bool,
    pub appended: u8,
    pub service_kind: u8,
    pub target: u32,
    pub source: u32,
}

impl Ahoy {
    pub fn encode(&self) -> [u8; 10] {
        let mut w = W::head(true, OP_AHOY);
        w.put(7, self.service_options as u32)
            .flag(self.service_kind_flag)
            .flag(self.als)
            .flag(self.group)
            .put(2, self.appended as u32)
            .put(4, self.service_kind as u32)
            .put(24, self.target)
            .put(24, self.source);
        w.done()
    }
    pub fn decode(b: &[u8; 10]) -> Option<Ahoy> {
        if opcode(b) != OP_AHOY || fid(b) != 0 {
            return None;
        }
        let mut r = reader(b);
        Some(Ahoy {
            service_options: r.get(7) as u8,
            service_kind_flag: r.flag(),
            als: r.flag(),
            group: r.flag(),
            appended: r.get(2) as u8,
            service_kind: r.get(4) as u8,
            target: r.get(24),
            source: r.get(24),
        })
    }
}

/// C_RAND (7.1.1.2.1, table 7.25), the MS's random access. `dep` is the 4 bits between the Proxy Flag and the
/// Service_Kind, which depend on the service: for a voice call Appended_Supplementary_Data (2), ALS (1), reserved
/// (1); for registration TRGT_ADR_CNTS (2) and the appended UDT block count (2); for Call Answer reserved (2),
/// ACCEPT (1), reserved (1).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct Rand {
    pub service_options: u8,
    pub proxy: bool,
    pub dep: u8,
    pub service_kind: u8,
    pub target: u32,
    pub source: u32,
}

// Voice Service_Options (table 7.52): EMERG, Privacy, SUPED_SV, BCAST_SV, reserved, PRIORTY_SV (2)
pub const SO_EMERGENCY: u8 = 0x40;
pub const SO_BROADCAST: u8 = 0x08;
pub const SO_PRIORITY: u8 = 0x03;
// Registration Service_Options (table 7.55): reserved, Privacy, IP_Inform, PowerSave_RQ (3), Reg_Dereg
pub const SO_IP_INFORM: u8 = 0x10;
pub const SO_PS_RQ: u8 = 0x0E;
pub const SO_REG: u8 = 0x01;

impl Rand {
    pub fn encode(&self) -> [u8; 10] {
        let mut w = W::head(true, OP_RAND);
        w.put(7, self.service_options as u32)
            .flag(self.proxy)
            .put(4, self.dep as u32)
            .put(4, self.service_kind as u32)
            .put(24, self.target)
            .put(24, self.source);
        w.done()
    }
    pub fn decode(b: &[u8; 10]) -> Option<Rand> {
        if opcode(b) != OP_RAND || fid(b) != 0 {
            return None;
        }
        let mut r = reader(b);
        Some(Rand {
            service_options: r.get(7) as u8,
            proxy: r.flag(),
            dep: r.get(4) as u8,
            service_kind: r.get(4) as u8,
            target: r.get(24),
            source: r.get(24),
        })
    }
    /// Registration: TRGT_ADR_CNTS (7.2.33).
    pub fn trgt_adr_cnts(&self) -> u8 {
        self.dep >> 2
    }
    /// Call Answer: true when the user accepted (ACCEPT = 0).
    pub fn accepted(&self) -> bool {
        self.dep & 0b0010 == 0
    }
}

/// P_CLEAR (7.1.1.3.2, table 7.30).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct Clear {
    pub chan: u16,
    pub group: bool,
    pub target: u32,
    pub source: u32,
}

impl Clear {
    pub fn encode(&self) -> [u8; 10] {
        let mut w = W::head(true, OP_CLEAR);
        w.put(12, self.chan as u32).put(3, 0).flag(self.group).put(24, self.target).put(24, self.source);
        w.done()
    }
    pub fn decode(b: &[u8; 10]) -> Option<Clear> {
        if opcode(b) != OP_CLEAR || fid(b) != 0 {
            return None;
        }
        let mut r = reader(b);
        let chan = r.get(12) as u16;
        r.get(3);
        Some(Clear { chan, group: r.flag(), target: r.get(24), source: r.get(24) })
    }
}

/// P_MAINT (7.1.1.4.3, table 7.32), from the MS. Maint_Kind 0 is DISCON.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct Maint {
    pub kind: u8,
    pub target: u32,
    pub source: u32,
}

pub const MAINT_DISCON: u8 = 0;

impl Maint {
    pub fn encode(&self) -> [u8; 10] {
        let mut w = W::head(true, OP_MAINT);
        w.put(12, 0).put(3, self.kind as u32).put(1, 0).put(24, self.target).put(24, self.source);
        w.done()
    }
    pub fn decode(b: &[u8; 10]) -> Option<Maint> {
        if opcode(b) != OP_MAINT || fid(b) != 0 {
            return None;
        }
        let mut r = reader(b);
        r.get(12);
        let kind = r.get(3) as u8;
        r.get(1);
        Some(Maint { kind, target: r.get(24), source: r.get(24) })
    }
}

/// C_BCAST (7.1.1.1.5, table 7.20): a single block, or with `lb` false the header of an MBC.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct Bcast {
    pub lb: bool,
    pub ann: u8,
    pub parms1: u16,
    pub reg: bool,
    pub backoff: u8,
    pub syscode: u16,
    pub parms2: u32,
}

impl Bcast {
    pub fn encode(&self) -> [u8; 10] {
        let mut w = W::head(self.lb, OP_BCAST);
        w.put(5, self.ann as u32)
            .put(14, self.parms1 as u32)
            .flag(self.reg)
            .put(4, self.backoff as u32)
            .put(16, self.syscode as u32)
            .put(24, self.parms2);
        w.done()
    }
    pub fn decode(b: &[u8; 10]) -> Option<Bcast> {
        if opcode(b) != OP_BCAST || fid(b) != 0 {
            return None;
        }
        let mut r = reader(b);
        Some(Bcast {
            lb: last_block(b),
            ann: r.get(5) as u8,
            parms1: r.get(14) as u16,
            reg: r.flag(),
            backoff: r.get(4) as u8,
            syscode: r.get(16) as u16,
            parms2: r.get(24),
        })
    }

    /// Adjacent_Site (7.2.19.7, table 7.79): a neighbor's top 14 syscode bits, its Active_Connection (when
    /// known), its priorities and its TSCC channel.
    #[allow(clippy::too_many_arguments)]
    pub fn adjacent_site(
        reg: bool,
        backoff: u8,
        syscode: u16,
        n_syscode: u16,
        active: Option<bool>,
        prio_confirmed: u8,
        prio_adjacent: u8,
        chan: u16,
    ) -> Bcast {
        let p2 = ((active.is_some() as u32) << 23)
            | ((active.unwrap_or(false) as u32) << 22)
            | ((prio_confirmed as u32 & 7) << 19)
            | ((prio_adjacent as u32 & 7) << 16)
            | (chan as u32 & 0xFFF);
        Bcast { lb: true, ann: BC_ADJACENT_SITE, parms1: n_syscode >> 2, reg, backoff, syscode, parms2: p2 }
    }

    /// Chan_Freq's header (7.2.19.6, table 7.78): always an MBC, followed by a BC_AP block.
    pub fn chan_freq(reg: bool, backoff: u8, syscode: u16, chan: u16) -> Bcast {
        Bcast { lb: false, ann: BC_CHAN_FREQ, parms1: 0, reg, backoff, syscode, parms2: chan as u32 & 0xFFF }
    }

    /// Gen_Site_Params (7.2.19.8, table 7.80): the hibernating flag (7.2.40) and the network information's "Reg and
    /// TG Subscription" bit (7.2.41).
    pub fn gen_site_params(reg: bool, backoff: u8, syscode: u16, hibernating: bool, reg_without_tg: bool) -> Bcast {
        let p2 = ((hibernating as u32) << 22) | ((reg_without_tg as u32) << 7);
        Bcast { lb: true, ann: BC_GEN_SITE_PARAMS, parms1: 0, reg, backoff, syscode, parms2: p2 }
    }

    /// Local_Time (7.2.19.4, table 7.73) from a broken-down local time and the UTC offset in minutes (local minus
    /// UTC; None leaves the offset out).
    #[allow(clippy::too_many_arguments)]
    pub fn local_time(
        reg: bool,
        backoff: u8,
        syscode: u16,
        t: &Civil,
        utc_offset_min: Option<i32>,
    ) -> Bcast {
        let (hours, sign, frac) = match utc_offset_min {
            Some(m) => {
                let a = m.unsigned_abs();
                ((a / 60).min(14), (m < 0) as u32, (a % 60) / 15)
            }
            None => (0b1111, 0, 0),
        };
        let p1 = ((t.day as u32) << 9) | ((t.month as u32) << 5) | (hours << 1) | sign;
        let p2 = ((t.hour as u32) << 19)
            | ((t.minute as u32) << 13)
            | ((t.second as u32) << 7)
            | ((t.weekday as u32) << 4)
            | (frac << 2);
        Bcast { lb: true, ann: BC_LOCAL_TIME, parms1: p1 as u16, reg, backoff, syscode, parms2: p2 }
    }
}

/// BC_AP (7.1.1.1.5.1, table 7.21) with Cdeftype 0 (7.2.42): a logical channel's absolute frequencies, as the MS
/// sees them (it transmits on `tx_hz`, receives on `rx_hz`). Frequencies in 125 Hz steps, 50-999 MHz.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct BcAp {
    pub chan: u16,
    pub tx_hz: u32,
    pub rx_hz: u32,
}

impl BcAp {
    pub fn encode(&self) -> [u8; 10] {
        let mut w = W::new();
        let f = |hz: u32| (hz / 1_000_000, (hz % 1_000_000) / 125);
        let (tm, tk) = f(self.tx_hz);
        let (rm, rk) = f(self.rx_hz);
        w.put(1, 1).put(1, 0).put(6, OP_BCAST as u32).put(8, 0).put(4, 0).put(2, 0);
        w.put(12, self.chan as u32).put(10, tm).put(13, tk).put(10, rm).put(13, rk);
        w.done()
    }
    pub fn decode(b: &[u8; 10]) -> Option<BcAp> {
        let mut r = reader(b);
        if r.get(4) != 0 {
            return None; // Cdeftype 1-15 reserved
        }
        r.get(2);
        let chan = r.get(12) as u16;
        let tx = r.get(10) * 1_000_000 + r.get(13) * 125;
        let rx = r.get(10) * 1_000_000 + r.get(13) * 125;
        Some(BcAp { chan, tx_hz: tx, rx_hz: rx })
    }
}

// ── Short LC (7.1.2) ───────────────────────────────────────────────────────────

/// C_SYS_Parms (table 7.33): SLCO 0010b, MODEL + NET + SITE (the syscode's top 14 bits), Reg, Common_Slot_Counter.
pub fn c_sys_parms(syscode: u16, reg: bool, counter: u16) -> u32 {
    ((SLCO_C_SYS_PARMS as u32) << 24) | (((syscode >> 2) as u32) << 10) | ((reg as u32) << 9) | (counter as u32 & 0x1FF)
}

/// P_SYS_Parms (table 7.34): the same with Payload Channel Type (1: a composite control channel carrying payload)
/// in Reg's place.
pub fn p_sys_parms(syscode: u16, composite: bool, counter: u16) -> u32 {
    ((SLCO_P_SYS_PARMS as u32) << 24) | (((syscode >> 2) as u32) << 10) | ((composite as u32) << 9) | (counter as u32 & 0x1FF)
}

/// A system parameters short LC read back: (SLCO, the syscode's top 14 bits, Reg or Payload Channel Type, counter).
pub fn sys_parms_decode(slc: u32) -> (u8, u16, bool, u16) {
    (((slc >> 24) & 0xF) as u8, ((slc >> 10) & 0x3FFF) as u16, (slc >> 9) & 1 != 0, (slc & 0x1FF) as u16)
}

// ── Time ────────────────────────────────────────────────────────────────────────

/// A broken-down date and time; weekday 1 Sunday .. 7 Saturday (table 7.75).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct Civil {
    pub year: i32,
    pub month: u8,
    pub day: u8,
    pub hour: u8,
    pub minute: u8,
    pub second: u8,
    pub weekday: u8,
}

/// Unix time in ms to a broken-down time (proleptic Gregorian; the days-to-date method of H. Hinnant's public
/// domain algorithms).
pub fn civil_from_ms(ms: i64) -> Civil {
    let secs = ms.div_euclid(1000);
    let days = secs.div_euclid(86_400);
    let sod = secs.rem_euclid(86_400);
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = yoe + era * 400 + (m <= 2) as i64;
    Civil {
        year: y as i32,
        month: m as u8,
        day: d as u8,
        hour: (sod / 3600) as u8,
        minute: (sod % 3600 / 60) as u8,
        second: (sod % 60) as u8,
        weekday: ((days + 4).rem_euclid(7) + 1) as u8, // 1970-01-01 was a Thursday (5)
    }
}

/// NWait in frames for an NRand_Wait code (6.2.1.1.5, table 6.2).
pub fn nwait_frames(code: u8) -> u32 {
    match code {
        0..=13 => code as u32,
        14 => 15,
        _ => 24,
    }
}

/// Frames for a Backoff code (6.2.1.1.6, table 6.3); code 0 is reserved and read as 1.
pub fn backoff_frames(code: u8) -> u32 {
    match code {
        0..=5 => (code as u32).max(1),
        6 => 8,
        7 => 11,
        8 => 15,
        9 => 20,
        10 => 26,
        11 => 33,
        12 => 41,
        13 => 50,
        14 => 70,
        _ => 100,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn hex(s: &str) -> [u8; 10] {
        let mut b = [0u8; 10];
        for i in 0..10 {
            b[i] = u8::from_str_radix(&s[2 * i..2 * i + 2], 16).unwrap();
        }
        b
    }

    #[test]
    fn syscode_models() {
        for model in 0..4u8 {
            let (nb, sb) = model_bits(model);
            assert_eq!(nb + sb, 12);
            for &(net, site, par) in &[(0u16, 1u16, 3u8), ((1 << nb) - 1, (1 << sb) - 1, 1), (3, 5, 2)] {
                let s = SysCode { model, net, site, par };
                assert!(s.valid());
                assert_eq!(SysCode::decode(s.encode()), s);
            }
        }
        assert!(!SysCode { model: MODEL_TINY, net: 512, site: 1, par: 3 }.valid());
        assert!(!SysCode { model: MODEL_SMALL, net: 1, site: 1, par: 0 }.valid());
    }

    // A standard site's control channel, decoded: large model, NET 7, SITE 52, both categories.
    #[test]
    fn standard_site_aloha() {
        let a = Aloha::decode(&hex("99000900f69cd3000000")).unwrap();
        assert_eq!(SysCode::decode(a.syscode), SysCode { model: MODEL_LARGE, net: 7, site: 52, par: 3 });
        assert_eq!((a.version, a.offset, a.active_connection, a.mask, a.service_function), (2, false, true, 0, 0));
        assert_eq!((a.nrand_wait, a.reg, a.backoff, a.addr), (7, true, 6, 0));
        assert!(!a.tsccas && !a.site_sync);
        assert_eq!(a.encode(), hex("99000900f69cd3000000"));
    }

    #[test]
    fn standard_site_adjacent() {
        let b = Bcast::decode(&hex("a80034e0369cd3c00233")).unwrap();
        assert_eq!((b.lb, b.ann, b.reg, b.backoff, b.syscode), (true, BC_ADJACENT_SITE, true, 6, 0x9CD3));
        // the neighbor: NET 7 SITE 1, connected, channel 563
        assert_eq!(SysCode::decode(b.parms1 << 2), SysCode { model: MODEL_LARGE, net: 7, site: 1, par: 0 });
        assert_eq!(b.parms2 >> 22, 0b11);
        assert_eq!(b.parms2 & 0xFFF, 563);
        let again = Bcast::adjacent_site(true, 6, 0x9CD3, b.parms1 << 2, Some(true), 0, 0, 563);
        assert_eq!(again.encode(), hex("a80034e0369cd3c00233"));
    }

    #[test]
    fn standard_site_sys_parms() {
        assert_eq!(c_sys_parms(0x9CD3, true, 0), 0x29CD200);
        let (slco, top, reg, csc) = sys_parms_decode(c_sys_parms(0x9CD3, true, 300));
        assert_eq!((slco, top, reg, csc), (2, 0x9CD3 >> 2, true, 300));
        assert_eq!(sys_parms_decode(p_sys_parms(0x9CD3, false, 511)), (3, 0x9CD3 >> 2, false, 511));
    }

    #[test]
    fn round_trips() {
        let g = Grant { op: OP_TV_GRANT, chan: 123, lcn: 1, late_entry: true, emergency: true, offset: false, target: 9001, source: 1234567 };
        assert_eq!(Grant::decode(&g.encode()), Some(g));
        let e = g.encode();
        assert_eq!(e[0], 0x80 | 49);
        assert_eq!((e[2] as u16) << 4 | (e[3] as u16) >> 4, 123);
        let pv = Grant { op: OP_PV_GRANT, late_entry: true, ..g };
        assert!(!Grant::decode(&pv.encode()).unwrap().late_entry);
        let a = Ack { op: OP_ACKD, response_info: 0x2A, reason: R_REG_DENIED, target: 3100123, source: REGI };
        assert_eq!(Ack::decode(&a.encode()), Some(a));
        let h = Ahoy { service_options: 0x40, service_kind_flag: true, als: false, group: false, appended: 0, service_kind: SK_IND_VOICE, target: 7, source: 8 };
        assert_eq!(Ahoy::decode(&h.encode()), Some(h));
        let r = Rand { service_options: SO_REG, proxy: false, dep: 0b0100, service_kind: SK_REG, target: 91, source: 3100123 };
        let rd = Rand::decode(&r.encode()).unwrap();
        assert_eq!((rd, rd.trgt_adr_cnts()), (r, 1));
        let c = Clear { chan: 0, group: false, target: ALLMSI, source: TSI };
        assert_eq!(Clear::decode(&c.encode()), Some(c));
        let m = Maint { kind: MAINT_DISCON, target: TSI, source: 42 };
        assert_eq!(Maint::decode(&m.encode()), Some(m));
        let ap = BcAp { chan: 2, tx_hz: 438_500_000, rx_hz: 438_600_125 };
        assert_eq!(BcAp::decode(&ap.encode()), Some(ap));
        assert_eq!(opcode(&ap.encode()), OP_BCAST);
        let cf = Bcast::chan_freq(true, 3, 0x4011, 2);
        assert!(!Bcast::decode(&cf.encode()).unwrap().lb);
    }

    #[test]
    fn local_time() {
        // 2026-11-05 21:04:11 UTC, a Thursday
        let t = civil_from_ms(1_793_912_651_000);
        assert_eq!((t.year, t.month, t.day, t.hour, t.minute, t.second, t.weekday), (2026, 11, 5, 21, 4, 11, 5));
        assert_eq!(civil_from_ms(0).weekday, 5);
        let b = Bcast::local_time(false, 3, 0x4011, &t, Some(-6 * 60));
        let d = Bcast::decode(&b.encode()).unwrap();
        assert_eq!(d.parms1 >> 9, 5);
        assert_eq!((d.parms1 >> 5) & 0xF, 11);
        assert_eq!((d.parms1 >> 1) & 0xF, 6);
        assert_eq!(d.parms1 & 1, 1);
        assert_eq!(d.parms2 >> 19, 21);
        assert_eq!((d.parms2 >> 4) & 7, 5);
        let n = Bcast::local_time(false, 3, 0x4011, &t, None);
        assert_eq!((n.parms1 >> 1) & 0xF, 0xF);
    }

    #[test]
    fn tables() {
        assert_eq!((nwait_frames(6), nwait_frames(14), nwait_frames(15)), (6, 15, 24));
        assert_eq!((backoff_frames(3), backoff_frames(9), backoff_frames(15)), (3, 20, 100));
        assert!(is_gateway(REGI) && is_gateway(TATTSI) && !is_gateway(ALLMSID) && !is_gateway(3100123));
        assert!(is_allcall(ALLMSID) && is_allcall(ALLMSIDL) && !is_allcall(ALLMSI));
        assert_eq!(SysCode { model: MODEL_LARGE, net: 7, site: 52, par: 3 }.response_check(), (0x9CD3 >> 2) as u8 & 0x3F);
    }
}
