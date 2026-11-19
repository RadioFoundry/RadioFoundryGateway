// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! Field-site behaviors on a payload channel (docs/spec/fw/tier3.md section 3.22), each off by default, so a radio
//! can meet on the bench what it met at a site:
//! - `services.payload_late_entry`: the call's grant with Late_Entry 1, as a P_GRANT-format CSBK, on the payload
//!   slot itself, every `timers.grant_every_s` in the call's gaps (6.6.2.5.2);
//! - `services.illegally_parked_s`: a call opened from RF waits for its caller's voice with
//!   P_PROTECT(ILLEGALLY_PARKED) (7.2.21, table 7.82: Group = the call's, target its called party, source the caller)
//!   every other slot of the channel; if no one has talked when the time is up, C_ACKD(Message_Accepted) to the
//!   caller from the called party, then the clear;
//! - `services.clear_lpcn`: P_CLEAR names this channel in place of 0;
//! - `services.ack_before_grant`: a group call from RF gets C_ACKD(Message_Accepted) (target the caller, source the
//!   talkgroup) on the TSCC first, then its grant `ack_before_grant_gap_s` (0.12 s) or more after the C_ACKD went out.
//! A child module of t3site: it works on the site's own state.

use super::*;

/// Chan.park_until_ms after the C_ACKD: the clear goes next.
const PARK_ACKED: i64 = -1;

impl Site {
    /// A call opened on channel ch: the parking wait starts, unless voice came with it (the network's).
    pub(super) fn park_open(&mut self, ch: usize, talking_net: bool) {
        let now = self.now;
        let c = &mut self.chans[ch];
        c.le_next_ms = now + self.cfg.grant_every_ms as i64;
        if self.cfg.park_ms == 0 || talking_net || c.flags & F_DATA != 0 {
            return;
        }
        c.park_until_ms = now + self.cfg.park_ms as i64;
        // the hang time doesn't end the wait
        c.idle_until_ms = c.idle_until_ms.max(c.park_until_ms + 1000);
    }

    /// Channel ch's outbound in slot k when nothing else took it.
    pub(super) fn park_slot(&mut self, ch: usize, out: &mut ChOut) {
        let c = self.chans[ch];
        if out.dt != 0 || !Self::live(&c) || c.r#use != USE_HANG || c.flags & F_DATA != 0 || c.protect_ms != 0 {
            return;
        }
        let now = self.now;
        if c.park_until_ms == PARK_ACKED {
            self.chans[ch].park_until_ms = 0;
            self.log(format_args!("ch {ch}: no voice from {} while parked: cleared", c.src));
            self.clear(ch, CLOSE_HANG); // P_CLEAR from this slot on
            return;
        }
        if c.park_until_ms > 0 && c.talker != 0 {
            self.chans[ch].park_until_ms = 0; // the caller (or a member) talked
        } else if c.park_until_ms > 0 && now >= c.park_until_ms {
            let ri = SysCode::decode(self.syscode).response_check();
            let a = Ack { op: OP_ACKD, response_info: ri, reason: R_MESSAGE_ACCEPTED, target: c.src, source: c.dst };
            out.dt = DT_CSBK;
            out.pdu = a.encode();
            self.chans[ch].park_until_ms = PARK_ACKED;
            return;
        }
        let parked = self.chans[ch].park_until_ms > 0;
        if parked && (self.k >> 1) & 1 == 0 {
            let group = !matches!(c.kind, KIND_PRIVATE | KIND_DUPLEX);
            let p = Protect { kind: PROTECT_ILLEGALLY_PARKED, group, target: c.dst, source: c.src };
            out.dt = DT_CSBK;
            out.pdu = p.encode();
            return;
        }
        if self.cfg.payload_le != 0 && !matches!(c.kind, KIND_PRIVATE | KIND_DUPLEX) && now >= c.le_next_ms {
            self.chans[ch].le_next_ms = now + self.cfg.grant_every_ms as i64;
            out.dt = DT_CSBK;
            out.pdu = self.grant_pdu(ch, true, if c.talker != 0 { c.talker } else { c.src });
        }
    }

    /// A call opened on payload channel ch: with `ack_before_grant`, a group call from RF gets its C_ACKD queued
    /// now; the caller then queues the grant to wait ack_gap_ms after it goes out. Whether it did.
    pub(super) fn ack_first(&mut self, ch: usize, talking_net: bool) -> bool {
        let c = self.chans[ch];
        if self.cfg.ack_first == 0 || talking_net || c.flags & F_DATA != 0 || !matches!(c.kind, KIND_GROUP | KIND_ALLCALL) {
            return false;
        }
        let ri = SysCode::decode(self.syscode).response_check();
        let a = Ack { op: OP_ACKD, response_info: ri, reason: R_MESSAGE_ACCEPTED, target: c.src, source: c.dst };
        self.push(DT_CSBK, a.encode(), false, None);
        true
    }
}
