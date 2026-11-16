// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! The TSCC's moves and sleep (docs/spec/fw/tier3.md section 3.12), ETSI TS 102 361-4:
//! - C_MOVE (6.3.3.1 e, 7.1.1.1.3, table 7.17): MSs (all, by Mask, or one) told to leave for another TSCC, given
//!   by its logical channel, or (to an adjacent site's TSCC) by its frequencies in the MBC form: Physical Channel
//!   Number 4095 and the MV_AP block (7.1.1.1.3.1, table 7.18); on command.
//! - The asynchronous TSCC (5.3.3, 6.1.1.2): with no inbound for T_BS_Inactive and nothing under way, the TSCC
//!   broadcasts Gen_Site_Params with the Hibernating_Flag (7.2.40), then goes quiet: no C_ALOHA, no C_SYS_Parms.
//!   Any good inbound block wakes it, the MS's BS_Dwn_Act (ETSI TS 102 361-2 7.1.2.1, table 7.5a) among them, and
//!   so does anything the site has to send.
//!
//! A child module of t3site: it works on the site's own state.

use super::*;
use super::bcast::CH_ABSOLUTE;

/// BS Outbound Activation (361-2 table 7.5a), CSBKO 111000b.
pub const OP_BS_DWN_ACT: u8 = 0b11_1000;
/// C_MOVE's sends (repeated: no answer comes).
const MOVE_SENDS: usize = 3;
/// Gen_Site_Params(Hibernating) sends before going quiet.
const HIB_NOTICES: usize = 2;

/// Bits into a block, first field first (MSB first); the CRC is the radio layer's.
fn pack(fields: &[(u32, u32)]) -> [u8; 10] {
    let (mut v, mut n) = (0u128, 0u32);
    for &(bits, x) in fields {
        v = v << bits | u128::from(x & ((1u64 << bits) - 1) as u32);
        n += bits;
    }
    debug_assert_eq!(n, 80);
    let mut b = [0u8; 10];
    for (i, o) in b.iter_mut().enumerate() {
        *o = (v >> (72 - 8 * i)) as u8;
    }
    b
}

fn field(b: &[u8; 10], at: u32, bits: u32) -> u32 {
    let v = b.iter().fold(0u128, |v, &x| v << 8 | u128::from(x));
    (v >> (80 - at - bits)) as u32 & ((1u64 << bits) - 1) as u32
}

/// C_MOVE as a CSBK (table 7.17): Mask, Reg, Backoff, the TSCC's logical channel (1-4094) and the MS address.
pub fn move_pdu(mask: u8, reg: bool, backoff: u8, chan: u16, addr: u32) -> [u8; 10] {
    pack(&[
        (1, 1),
        (1, 0),
        (6, u32::from(OP_MOVE)),
        (8, 0),
        (9, 0),
        (5, u32::from(mask.min(24))),
        (5, 0),
        (1, u32::from(reg)),
        (4, u32::from(backoff)),
        (4, 0),
        (12, u32::from(chan)),
        (24, addr),
    ])
}

/// The header of an absolute C_MOVE (7.1.1.1.3 c): LB 0, Physical Channel Number 4095; MV_AP follows.
pub fn move_mbc_hdr(mask: u8, reg: bool, backoff: u8, addr: u32) -> [u8; 10] {
    let mut b = move_pdu(mask, reg, backoff, CH_ABSOLUTE, addr);
    b[0] &= 0x7F;
    b
}

/// MV_AP (7.1.1.1.3.1, table 7.18): LB 1, PF 0, C_MOVE's CSBKO, 4 reserved bits, the destination's color code,
/// Cdeftype and CdefParms (7.2.42) as in BC_AP and VN_AP: the logical number and the MS's transmit and receive
/// frequencies.
pub fn mv_ap(cc: u8, chan: u16, ms_tx_hz: u32, ms_rx_hz: u32) -> [u8; 10] {
    let mut b = BcAp { chan, tx_hz: ms_tx_hz, rx_hz: ms_rx_hz }.encode();
    b[0] = 0x80 | OP_MOVE;
    b[1] = cc & 15;
    b
}

/// An MV_AP block's color code and CdefParms.
pub fn mv_ap_of(b: &[u8; 10]) -> Option<(u8, BcAp)> {
    if b[0] != 0x80 | OP_MOVE || b[1] >> 4 != 0 {
        return None;
    }
    let mut c = *b;
    c[0] = BcAp { chan: 1, tx_hz: 0, rx_hz: 0 }.encode()[0];
    Some((b[1] & 15, BcAp::decode(&c)?))
}

/// An absolute C_MOVE header's Mask and address (LB 0, channel 4095).
pub fn move_mbc_of(b: &[u8; 10]) -> Option<(u8, u32)> {
    if opcode(b) != OP_MOVE || fid(b) != 0 || last_block(b) || field(b, 44, 12) != u32::from(CH_ABSOLUTE) {
        return None;
    }
    Some((field(b, 25, 5) as u8, field(b, 56, 24)))
}

/// A C_MOVE's Mask, channel and address (the single block form only).
pub fn move_of(b: &[u8; 10]) -> Option<(u8, u16, u32)> {
    if opcode(b) != OP_MOVE || fid(b) != 0 || !last_block(b) {
        return None;
    }
    Some((field(b, 25, 5) as u8, field(b, 44, 12) as u16, field(b, 56, 24)))
}

/// BS_Dwn_Act (361-2 table 7.5a): the BS address and the source.
pub fn bs_dwn_act(bs: u32, source: u32) -> [u8; 10] {
    pack(&[(1, 1), (1, 0), (6, u32::from(OP_BS_DWN_ACT)), (8, 0), (16, 0), (24, bs), (24, source)])
}

pub fn bs_dwn_act_of(b: &[u8; 10]) -> Option<(u32, u32)> {
    (opcode(b) == OP_BS_DWN_ACT && fid(b) == 0).then(|| (field(b, 32, 24), field(b, 56, 24)))
}

#[derive(Clone, Copy, Default)]
pub(super) struct TsccState {
    hibernating: bool,
    notices: usize,  // Gen_Site_Params(Hibernating) left to send before going quiet
    quiet_k: i64,    // goes quiet at this slot, once the notices are out
    last_in_ms: i64, // the last good inbound block or voice
    wakes: u32,
}

impl Site {
    /// C_MOVE to every MS (ms 0) or one: leave for the TSCC on logical channel chan.
    pub(super) fn move_cmd(&mut self, chan: u32, ms: u32) -> bool {
        if !(1..=4094).contains(&chan) || ms > 0xFF_FFFF || self.withdrawn {
            return false;
        }
        let mask = if ms != 0 { 24 } else { 0 };
        let pdu = move_pdu(mask, self.reg_bit(), self.backoff(), chan as u16, ms);
        for _ in 0..MOVE_SENDS {
            self.push(DT_CSBK, pdu, false, None);
        }
        self.log(format_args!("C_MOVE to channel {} for {}", chan, if ms != 0 { ms } else { ALLMSID }));
        true
    }

    /// C_MOVE to every MS (ms 0) or one: leave for the adjacent site whose C_SYScode is syscode (set_neighbors).
    /// By its logical channel when that names it here (tier3.md 3.11: announced, or not a clash); by its
    /// frequencies (the MBC form) when the number names other frequencies here, or when it has frequencies but
    /// no logical number. False: no such neighbor, or neither is known.
    pub(super) fn move_to_site(&mut self, syscode: u32, ms: u32) -> bool {
        if syscode == 0 || syscode > 0xFFFF || ms > 0xFF_FFFF || self.withdrawn {
            return false;
        }
        let Some(i) = self.neighbors[..self.nneighbors].iter().position(|n| u32::from(n.syscode) == syscode) else {
            return false;
        };
        let n = self.neighbors[i];
        let freqs = n.tx_hz != 0 && n.rx_hz != 0;
        let logical = (1..=4094).contains(&n.chan);
        if !logical && !freqs {
            return false;
        }
        if logical && self.nb_chan(i) != NbChan::Clash {
            return self.move_cmd(u32::from(n.chan), ms);
        }
        let mask = if ms != 0 { 24 } else { 0 };
        let hdr = move_mbc_hdr(mask, self.reg_bit(), self.backoff(), ms);
        // the MS's view: it transmits on the neighbor's inbound
        let ap = mv_ap(n.cc, if logical { n.chan } else { 0 }, n.rx_hz, n.tx_hz);
        for _ in 0..MOVE_SENDS {
            self.push(DT_MBC_HDR, hdr, false, Some(ap));
        }
        self.log(format_args!(
            "C_MOVE (absolute) to {:04x}'s TSCC {} kHz for {}",
            n.syscode,
            n.tx_hz / 1000,
            if ms != 0 { ms } else { ALLMSID }
        ));
        true
    }

    /// A good inbound block or voice: the TSCC stays awake, or wakes.
    pub(super) fn tscc_heard(&mut self) {
        self.tscc.last_in_ms = self.now;
        self.tscc.notices = 0;
        if self.tscc.hibernating {
            self.tscc.hibernating = false;
            self.tscc.wakes += 1;
            self.log(format_args!("TSCC woken"));
        }
    }

    /// Whether Gen_Site_Params carries the Hibernating_Flag: the TSCC is about to go quiet.
    pub(super) fn about_to_hibernate(&self) -> bool {
        self.tscc.notices > 0
    }

    /// Per slot, before the TSCC's block: true while the TSCC hibernates (sends nothing).
    pub(super) fn hibernate_tick(&mut self) -> bool {
        let t = self.cfg.hibernate_ms as i64;
        if t == 0 {
            self.tscc.hibernating = false;
            return false;
        }
        if self.tscc.last_in_ms == 0 {
            self.tscc.last_in_ms = self.now; // from the start
        }
        // something to send (the notices themselves aside) or under way
        let busy = (self.resp_len > 0 && self.tscc.notices == 0)
            || self.cont_pending
            || self.chans.iter().any(Self::live)
            || self.pend.iter().any(|p| p.used)
            || self.net.iter().any(|n| n.used)
            || self.sd_pending();
        if self.tscc.hibernating {
            if busy {
                self.tscc_heard(); // something to send: awake again
            }
            return self.tscc.hibernating;
        }
        if busy || self.now - self.tscc.last_in_ms < t {
            self.tscc.notices = 0;
            return false;
        }
        if self.tscc.notices == 0 {
            // 6.1.1.2: Gen_Site_Params with the Hibernating_Flag first, then quiet
            self.tscc.notices = HIB_NOTICES;
            let (reg, bo, sc) = (self.reg_bit(), self.backoff(), self.syscode);
            let b = Bcast::gen_site_params(reg, bo, sc, true, false).encode();
            for _ in 0..HIB_NOTICES {
                self.push(DT_CSBK, b, false, None);
            }
            self.tscc.quiet_k = self.k + 2 * (HIB_NOTICES as i64 + 2);
            return false;
        }
        if self.k >= self.tscc.quiet_k {
            self.tscc.hibernating = true;
            self.tscc.notices = 0;
            self.log(format_args!("TSCC hibernating: no inbound for {} s", t / 1000));
        }
        self.tscc.hibernating
    }

    /// Whether the TSCC hibernates now, and how often it was woken.
    pub fn hibernation(&self) -> (bool, u32) {
        (self.tscc.hibernating, self.tscc.wakes)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn move_and_activation_fields() {
        let b = move_pdu(24, true, 3, 77, 3_100_123);
        assert_eq!((opcode(&b), fid(&b), last_block(&b)), (OP_MOVE, 0, true));
        assert_eq!(move_of(&b), Some((24, 77, 3_100_123)));
        assert_eq!(field(&b, 16, 9), 0, "9 reserved bits");
        assert_eq!(field(&b, 30, 5), 0, "5 reserved bits");
        assert_eq!((field(&b, 35, 1), field(&b, 36, 4), field(&b, 40, 4)), (1, 3, 0), "Reg, Backoff, reserved");
        let a = bs_dwn_act(0x123456, 3_100_123);
        assert_eq!((opcode(&a), field(&a, 16, 16)), (OP_BS_DWN_ACT, 0));
        assert_eq!(bs_dwn_act_of(&a), Some((0x123456, 3_100_123)));
        assert_eq!(move_of(&a), None);
    }

    #[test]
    fn absolute_move_round_trip() {
        let h = move_mbc_hdr(24, false, 2, 3_100_123);
        assert!(!last_block(&h));
        assert_eq!(move_of(&h), None, "not the single block form");
        assert_eq!(move_mbc_of(&h), Some((24, 3_100_123)));
        let c = mv_ap(7, 3, 461_137_500, 461_287_500);
        let (cc, ap) = mv_ap_of(&c).unwrap();
        assert_eq!((cc, ap.chan, ap.tx_hz, ap.rx_hz), (7, 3, 461_137_500, 461_287_500));
        assert_eq!(mv_ap_of(&h), None);
    }
}
