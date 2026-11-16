// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! Spec conformance tests, shared parts: a bit packer for building blocks from the 361-4 tables, a field reader,
//! and the values the tests expect, typed in from ETSI TS 102 361-1 and -4 (clause and table cited by each).
//! Nothing here comes from our encoder or our constants: the t3spec_* tests compare our code against these.

/// A 10-octet block built field by field, MSB first, as the 361-4 tables list them.
pub struct Bits {
    b: [u8; 10],
    n: usize,
}

impl Bits {
    pub fn new() -> Bits {
        Bits { b: [0; 10], n: 0 }
    }

    /// Octets 0 and 1 of a CSBK or MBC header (361-1 9.1.8; 361-4 7.1.1): LB, PF, CSBKO, FID.
    pub fn csbk(lb: bool, op: u8) -> Bits {
        Bits::new().f(1, lb as u64).f(1, 0).f(6, op as u64).f(8, 0)
    }

    /// Appends a field of `w` bits.
    pub fn f(mut self, w: usize, v: u64) -> Bits {
        assert!(w == 64 || v < (1u64 << w), "value {v:#x} doesn't fit {w} bits");
        for i in (0..w).rev() {
            if (v >> i) & 1 != 0 {
                self.b[self.n / 8] |= 0x80 >> (self.n % 8);
            }
            self.n += 1;
        }
        self
    }

    pub fn done(self) -> [u8; 10] {
        assert_eq!(self.n, 80, "a block is 80 bits");
        self.b
    }
}

/// Bits `start .. start + w` of a block, MSB first (bit 0 is octet 0's MSB).
pub fn field(b: &[u8; 10], start: usize, w: usize) -> u64 {
    let mut v = 0u64;
    for i in start..start + w {
        v = (v << 1) | ((b[i / 8] >> (7 - i % 8)) & 1) as u64;
    }
    v
}

/// Octet 0's CSBKO (361-1 9.3.32).
pub fn csbko(b: &[u8; 10]) -> u8 {
    b[0] & 0x3F
}

/// Octet 0's LB (361-1 9.3.31).
pub fn lb(b: &[u8; 10]) -> bool {
    b[0] & 0x80 != 0
}

/// The values the tests expect, from the standard.
#[allow(dead_code)]
pub mod spec {
    // Data Type (361-1 9.3.6, table 9.22)
    pub const DT_CSBK: u8 = 0b0011;
    pub const DT_MBC_HEADER: u8 = 0b0100;
    pub const DT_MBC_CONT: u8 = 0b0101;
    pub const DT_DATA_HEADER: u8 = 0b0110;
    pub const DT_IDLE: u8 = 0b1001;

    // CSBKO (361-4 annex B, table B.1, and each PDU's table in 7.1.1)
    pub const C_ALOHA: u8 = 0b01_1001; // table 7.19
    pub const C_AHOY: u8 = 0b01_1100; // table 7.22
    pub const C_RAND: u8 = 0b01_1111; // table 7.25
    pub const C_ACKD: u8 = 0b10_0000; // table 7.23
    pub const C_ACKU: u8 = 0b10_0001; // table 7.27
    pub const P_ACKU: u8 = 0b10_0011; // table 7.4: payload inbound
    pub const C_BCAST: u8 = 0b10_1000; // table 7.20
    pub const P_MAINT: u8 = 0b10_1010; // table 7.32
    pub const P_CLEAR: u8 = 0b10_1110; // table 7.30
    pub const P_PROTECT: u8 = 0b10_1111; // table 7.31
    pub const PV_GRANT: u8 = 0b11_0000; // table 7.9
    pub const TV_GRANT: u8 = 0b11_0001; // table 7.11
    pub const BTV_GRANT: u8 = 0b11_0010; // table 7.12
    pub const PD_GRANT: u8 = 0b11_0011; // table 7.13, single item
    pub const C_MOVE: u8 = 0b11_1001; // table 7.17

    // Gateway and special addresses (361-4 A.4)
    pub const ADRNULL: u32 = 0x000000;
    pub const PSTNI: u32 = 0xFFFEC0;
    pub const PABXI: u32 = 0xFFFEC1;
    pub const LINEI: u32 = 0xFFFEC2;
    pub const IPI: u32 = 0xFFFEC3;
    pub const SUPLI: u32 = 0xFFFEC4;
    pub const SDMI: u32 = 0xFFFEC5;
    pub const REGI: u32 = 0xFFFEC6;
    pub const MSI: u32 = 0xFFFEC7;
    pub const DIVERTI: u32 = 0xFFFEC9;
    pub const TSI: u32 = 0xFFFECA;
    pub const DISPATI: u32 = 0xFFFECB;
    pub const STUNI: u32 = 0xFFFECC;
    pub const AUTHI: u32 = 0xFFFECD;
    pub const GPI: u32 = 0xFFFECE;
    pub const KILLI: u32 = 0xFFFECF;
    pub const PSTNDI: u32 = 0xFFFED0;
    pub const DISPATDI: u32 = 0xFFFED3;
    pub const ALLMSI: u32 = 0xFFFED4;
    pub const IPDI: u32 = 0xFFFED5;
    pub const DGNAI: u32 = 0xFFFED6;
    pub const TATTSI: u32 = 0xFFFED7;
    pub const ALLMSIDL: u32 = 0xFFFFFD;
    pub const ALLMSIDZ: u32 = 0xFFFFFE;
    pub const ALLMSID: u32 = 0xFFFFFF;

    // Service_Kind (361-4 7.2.12, table 7.49)
    pub const SK_IND_VOICE: u8 = 0b0000;
    pub const SK_TG_VOICE: u8 = 0b0001;
    pub const SK_IND_PACKET: u8 = 0b0010;
    pub const SK_TG_PACKET: u8 = 0b0011;
    pub const SK_UDT_OUT: u8 = 0b0100;
    pub const SK_UDT_IN: u8 = 0b0101;
    pub const SK_POLL: u8 = 0b0110;
    pub const SK_STATUS: u8 = 0b0111;
    pub const SK_DIVERSION: u8 = 0b1000;
    pub const SK_CALL_ANSWER: u8 = 0b1001;
    pub const SK_DUPLEX_IND: u8 = 0b1010;
    pub const SK_REG: u8 = 0b1110;
    pub const SK_CANCEL: u8 = 0b1111;

    // Reason (361-4 7.2.8, tables 7.42 to 7.45): t t d a a a a a
    pub const MESSAGE_ACCEPTED: u8 = 0b0110_0000;
    pub const REG_ACCEPTED: u8 = 0b0110_0010;
    pub const REG_SUBSCRIPTION: u8 = 0b0110_0101;
    pub const MS_ACCEPTED: u8 = 0b0100_0100;
    pub const CALLBACK: u8 = 0b0100_0101;
    pub const MS_ALERTING: u8 = 0b0100_0110;
    pub const NOT_SUPPORTED: u8 = 0b0010_0000;
    pub const PERM_USER_REFUSED: u8 = 0b0010_0001;
    pub const NOREG_MSAWAY: u8 = 0b0010_0100;
    pub const MSAWAY_REFUSED: u8 = 0b0010_0101;
    pub const SYSBUSY: u8 = 0b0010_0111;
    pub const CALL_CANCEL_REFUSED: u8 = 0b0010_1001;
    pub const REG_REFUSED: u8 = 0b0010_1010;
    pub const REG_DENIED: u8 = 0b0010_1011;
    pub const IP_CONNECTION_FAILED: u8 = 0b0010_1100;
    pub const MS_NOT_REGISTERED: u8 = 0b0010_1101;
    pub const CALLED_PARTY_BUSY: u8 = 0b0010_1110;
    pub const GROUP_NOT_ALLOWED: u8 = 0b0010_1111;
    pub const EQUIP_BUSY: u8 = 0b0001_0011;
    pub const RECIPIENT_REFUSED: u8 = 0b0001_0100;
    pub const QUEUED_RESOURCE: u8 = 0b1010_0000;
    pub const QUEUED_BUSY: u8 = 0b1010_0001;
    pub const WAIT: u8 = 0b1110_0000;

    // Announcement_type (361-4 7.2.19, table 7.68)
    pub const ANN_WD_TSCC: u8 = 0;
    pub const CALL_TIMER_PARMS: u8 = 1;
    pub const VOTE_NOW: u8 = 2;
    pub const LOCAL_TIME: u8 = 3;
    pub const MASS_REG: u8 = 4;
    pub const CHAN_FREQ: u8 = 5;
    pub const ADJACENT_SITE: u8 = 6;
    pub const GEN_SITE_PARAMS: u8 = 7;

    // SLCO (361-4 7.1.2, tables 7.33, 7.34; 361-2 for Null and Activity Update)
    pub const SLCO_NULL: u8 = 0b0000;
    pub const SLCO_ACTIVITY: u8 = 0b0001;
    pub const SLCO_C_SYS_PARMS: u8 = 0b0010;
    pub const SLCO_P_SYS_PARMS: u8 = 0b0011;

    // Version (361-4 7.2.32, table 7.93): 100b for V1.11.1
    pub const VERSION_1_11_1: u8 = 0b100;

    /// NWait in TDMA frames for an NRand_Wait code (361-4 6.2.1.1.5, table 6.2).
    pub const NWAIT: [u32; 16] = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 15, 24];
    /// Backoff in TDMA frames for codes 1..15 (361-4 6.2.1.1.6, table 6.3); code 0 is reserved.
    pub const BACKOFF: [u32; 16] = [0, 1, 2, 3, 4, 5, 8, 11, 15, 20, 26, 33, 41, 50, 70, 100];

    // MS constants (361-4 A.2)
    pub const NRAND_NR: u32 = 6;
    pub const NRAND_NE: u32 = 10;

    /// NET and SITE widths of a network model (361-4 6.3.2.2.1.1, table 6.5 and figure 6.15).
    pub fn model_widths(model: u8) -> (u32, u32) {
        match model {
            0 => (9, 3),
            1 => (7, 5),
            2 => (4, 8),
            _ => (2, 10),
        }
    }

    /// C_SYScode built by hand: MODEL (2 MSBs), NET, SITE, PAR (2 LSBs) (figure 6.15).
    pub fn syscode(model: u8, net: u32, site: u32, par: u8) -> u16 {
        let (_, sb) = model_widths(model);
        (((model as u32) << 14) | (net << (2 + sb)) | (site << 2) | par as u32) as u16
    }
}
