// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! Full-duplex MS to MS voice calls (docs/spec/fw/tier3.md section 3.13), ETSI TS 102 361-4 6.6.9: a C_RAND with
//! Service_Kind 1010b (table 6.73) to an individual MS; the called MS's radio check is a C_AHOY with the same
//! Service_Kind (6.6.9.1.4), and an MS without the service answers C_NACKU(MS_Duplex_Not_Supported), mirrored to
//! the caller. The call takes a whole carrier in offset timing (361-1 5.1.1.2, 5.1.4.4): each party gets a
//! PV_GRANT_DX (7.1.1.1.1.1.2, table 7.10) for its own logical channel, transmits on its inbound slot and hears
//! the other on its outbound slot; the radio layer repeats each party's inbound on the other's outbound.
//!
//! Local calls only (both parties registered here): a duplex call over the network would need two voice streams.
//! Not queued: with no carrier free the caller gets C_NACKD(SYSBUSY), and a duplex emergency doesn't pre-empt.
//!
//! A child module of t3site: it works on the site's own state.

use super::*;

/// Service_Kind 1010b: Full-Duplex MS to MS Individual Voice Call Service (table 6.73).
pub const SK_DUPLEX: u8 = 0b1010;
/// CSBKO 110101b: PV_GRANT_DX (table 7.10).
pub const OP_PV_GRANT_DX: u8 = 0b11_0101;
/// MS_Duplex_Not_Supported (7.2.8): from a called MS, mirrored by the TS.
pub const R_DUPLEX_NOT_SUPPORTED: u8 = 0x16;
/// Pend.kind and Chan.kind of a duplex call (after t3site's KIND_*).
pub(super) const KIND_DUPLEX: u8 = 4;
/// Event flag: a channel of a full-duplex call (offset timing, crossed repeat).
pub const F_DUPLEX: u8 = 0x80;

/// PV_GRANT_DX (table 7.10): the logical channel and LCN of the target's own slot, Call Direction (true: the
/// target is the calling MS), the target and its partner.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct DxGrant {
    pub chan: u16,
    pub lcn: u8,
    pub emergency: bool,
    pub target_calling: bool,
    pub target: u32,
    pub source: u32,
}

impl DxGrant {
    pub fn encode(&self) -> [u8; 10] {
        let mut v: u128 = 1 << 79 | u128::from(OP_PV_GRANT_DX) << 72; // LB 1, PF 0, FID 0
        v |= u128::from(self.chan & 0xFFF) << 52
            | u128::from(self.lcn & 1) << 51
            | u128::from(self.emergency) << 49
            | u128::from(self.target_calling) << 48
            | u128::from(self.target & 0xFF_FFFF) << 24
            | u128::from(self.source & 0xFF_FFFF);
        let mut b = [0u8; 10];
        for (i, o) in b.iter_mut().enumerate() {
            *o = (v >> (72 - 8 * i)) as u8;
        }
        b
    }

    pub fn decode(b: &[u8; 10]) -> Option<DxGrant> {
        if opcode(b) != OP_PV_GRANT_DX || fid(b) != 0 || !last_block(b) {
            return None;
        }
        let v = b.iter().fold(0u128, |v, &x| v << 8 | u128::from(x));
        Some(DxGrant {
            chan: (v >> 52) as u16 & 0xFFF,
            lcn: (v >> 51) as u8 & 1,
            emergency: v >> 49 & 1 != 0,
            target_calling: v >> 48 & 1 != 0,
            target: (v >> 24) as u32 & 0xFF_FFFF,
            source: v as u32 & 0xFF_FFFF,
        })
    }
}

impl Site {
    /// A carrier with both slots free (so not the TSCC's): its first channel.
    fn free_carrier(&self) -> Option<usize> {
        (0..CHANNELS).step_by(2).find(|&i| self.chans[i].r#use == USE_IDLE && self.chans[i + 1].r#use == USE_IDLE)
    }

    /// C_RAND, Service_Kind 1010b (6.6.9.1).
    pub(super) fn duplex_request(&mut self, r: &Rand) {
        let (ms, dst) = (r.source, r.target);
        if self.cfg.duplex == 0 || r.proxy {
            self.refuse(R_NOT_SUPPORTED, ms, dst);
            return;
        }
        if self.pend.iter().any(|p| p.used && p.caller_local && p.caller == ms && p.called == dst) {
            self.ack(R_WAIT, ms, dst); // a repeat while the radio check is out
            return;
        }
        if self.duplex_channel(ms, dst).is_some() {
            self.duplex_regrant(ms, dst);
            return;
        }
        let Some(prio) = self.admit(r) else { return };
        if self.reg_index(dst).is_none() {
            self.refuse(R_NOREG_MSAWAY, ms, dst); // local only
            return;
        }
        if self.ms_busy(dst) {
            self.refuse(R_CALLED_PARTY_BUSY, ms, dst);
            return;
        }
        let Some(i) = self.free_pend() else {
            self.refuse(R_SYSBUSY, ms, dst);
            return;
        };
        self.pend[i] = Pend {
            used: true,
            kind: KIND_DUPLEX,
            state: P_AHOY,
            svc: r.service_options,
            prio,
            caller_local: true,
            called_local: true,
            caller: ms,
            called: dst,
            since_ms: self.now,
            deadline_ms: self.now + self.cfg.tp_ms as i64,
            ahoy_left: self.cfg.ahoy_tries,
            next_ahoy_ms: self.now,
            next_ack_ms: self.now + WACK_EVERY_MS,
            ..Default::default()
        };
        self.ack(R_WAIT, ms, dst);
        self.ahoy_tick(i);
    }

    /// Both parties ready: a free carrier, both its channels opened (each with its party's grant), or busy.
    pub(super) fn duplex_ready(&mut self, p: &Pend) {
        let Some(ch) = self.free_carrier() else {
            self.fail(p, R_SYSBUSY, WHY_CONGESTION);
            return;
        };
        let mut flags = F_DUPLEX;
        if p.svc & SO_EMERGENCY != 0 {
            flags |= F_EMERGENCY;
        }
        // LCN 1 to the caller, LCN 2 to the called MS: each channel's src is its own party
        self.open(ch, KIND_DUPLEX, p.caller, p.called, flags, 0, false);
        self.open(ch + 1, KIND_DUPLEX, p.called, p.caller, flags, 0, false);
    }

    /// The duplex call between a and b: its caller's channel.
    fn duplex_channel(&self, a: u32, b: u32) -> Option<usize> {
        (0..CHANNELS).step_by(2).find(|&i| {
            let c = &self.chans[i];
            Self::live(c) && c.kind == KIND_DUPLEX && ((c.src == a && c.dst == b) || (c.src == b && c.dst == a))
        })
    }

    fn duplex_regrant(&mut self, a: u32, b: u32) {
        if let Some(ch) = self.duplex_channel(a, b) {
            for c in [ch, ch + 1] {
                let g = self.duplex_grant(c);
                self.push(DT_CSBK, g, false, None);
            }
        }
    }

    /// Channel ch's PV_GRANT_DX: to its own party, for its own slot.
    pub(super) fn duplex_grant(&self, ch: usize) -> [u8; 10] {
        let c = &self.chans[ch];
        DxGrant {
            chan: self.cfg.carriers[ch / 2].chan,
            lcn: (ch % 2) as u8,
            emergency: c.flags & F_EMERGENCY != 0,
            target_calling: ch & 1 == 0,
            target: c.src,
            source: c.dst,
        }
        .encode()
    }

    /// Clearing one channel of a duplex call clears the other.
    pub(super) fn duplex_clear(&mut self, ch: usize, why: u8) {
        if self.chans[ch].kind == KIND_DUPLEX && Self::live(&self.chans[ch ^ 1]) {
            self.clear(ch ^ 1, why);
        }
    }

    /// Whether the call on ch is held by its duplex partner: the other party still talks or within hang time.
    pub(super) fn duplex_held(&self, ch: usize) -> bool {
        let (c, o) = (&self.chans[ch], &self.chans[ch ^ 1]);
        c.kind == KIND_DUPLEX && Self::live(o) && (o.talking || self.now < o.idle_until_ms)
    }

    /// The channel whose outbound carries ch's inbound voice: its duplex partner, or ch itself.
    pub fn repeat_channel(&self, ch: usize) -> usize {
        if ch < CHANNELS && self.chans[ch].kind == KIND_DUPLEX && Self::live(&self.chans[ch]) {
            ch ^ 1
        } else {
            ch
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pv_grant_dx_fields() {
        let g = DxGrant { chan: 0xABC, lcn: 1, emergency: true, target_calling: false, target: 0x123456, source: 0xFEDCBA };
        let b = g.encode();
        assert_eq!((opcode(&b), fid(&b), last_block(&b)), (OP_PV_GRANT_DX, 0, true));
        assert_eq!((b[2], b[3]), (0xAB, 0xCA), "channel, LCN, reserved 0, Emergency, Call Direction");
        assert_eq!(DxGrant::decode(&b), Some(g));
        assert_eq!(Grant::decode(&b), None, "not a simplex grant");
    }
}
