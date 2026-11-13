// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! The TSCC's moves and sleep (docs/spec/fw/tier3.md section 3.12), ETSI TS 102 361-4:
//! - C_MOVE (6.3.3.1 e, 7.1.1.1.3, table 7.17): MSs (all, by Mask, or one) told to leave for another TSCC, given
//!   by its logical channel; on command.
//! - The asynchronous TSCC (5.3.3, 6.1.1.2): with no inbound for T_BS_Inactive and nothing under way, the TSCC
//!   broadcasts Gen_Site_Params with the Hibernating_Flag (7.2.40), then goes quiet: no C_ALOHA, no C_SYS_Parms.
//!   Any good inbound block wakes it, the MS's BS_Dwn_Act (ETSI TS 102 361-2 7.1.2.1, table 7.5a) among them, and
//!   so does anything the site has to send.
//!
//! A child module of t3site: it works on the site's own state.

use super::*;

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
}
