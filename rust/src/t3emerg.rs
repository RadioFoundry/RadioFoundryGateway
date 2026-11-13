// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! Emergency pre-emption (docs/spec/fw/tier3.md section 3.9): ETSI TS 102 361-4 6.4.14.3.1, "Emergency
//! Pre-emption MS De-key". When an emergency call waits in the queue and no payload channel is free or coming free,
//! the TS may clear a non-emergency call. If a party is transmitting, the TS first sends it the Reverse Channel
//! "cease transmission" command (RC info 0100b, table 6.32) on the alternate slot of its carrier (361-1 5.1.5);
//! the MS finishes its superframe, sends a terminator and stops (6.4.15). When the inbound stops, P_CLEAR goes out
//! on the payload channel and the freed channel goes to the emergency call (the queue serves emergencies first).
//! A child module of t3site: it works on the site's own state.

use super::*;
use crate::t3rc::RC_CEASE;

/// ChOut.rc: an RC PDU is to be sent (the low 4 bits are the RC info).
pub const RC_SEND: u8 = 0x10;
/// The longest wait for a talker told to cease before the channel is cleared over it.
const DEKEY_MS: i64 = 3000;
/// 361-1 5.1.5.1: repeated RC messages no sooner than 360 ms apart.
const RC_EVERY_MS: i64 = 360;

impl Site {
    /// Whether a request with these service options may pre-empt (and so isn't refused for a full queue).
    pub(super) fn preempts(&self, svc: u8) -> bool {
        self.cfg.preempt != 0 && svc & SO_EMERGENCY != 0
    }

    /// An emergency request for a talkgroup whose call is open: the call becomes an emergency call, with the
    /// emergency call timer from its start, and its grants carry the Emergency flag.
    pub(super) fn emergency_upgrade(&mut self, ch: usize) {
        let c = &mut self.chans[ch];
        if c.flags & F_EMERGENCY == 0 {
            c.flags |= F_EMERGENCY;
            c.call_end_ms = c.since_ms + self.cfg.emergency_call_ms as i64;
            c.preempt = false;
        }
    }

    /// The call to clear for an emergency: not an emergency itself, not already going. A call in hang time
    /// first (nobody to de-key), then one whose talker can hear the RC on a payload slot (not the TSCC's), then
    /// the oldest.
    fn victim(&self) -> Option<usize> {
        let tscc = self.tscc_carrier * 2;
        (0..CHANNELS)
            .filter(|&i| {
                let c = &self.chans[i];
                Self::live(c) && !c.preempt && c.flags & F_EMERGENCY == 0
            })
            .min_by_key(|&i| {
                let c = &self.chans[i];
                let rf_talker = c.talking && !c.talking_net;
                (rf_talker, rf_talker && i ^ 1 == tscc, c.since_ms)
            })
    }

    /// Per slot, before the TSCC's block: starts pre-emptions the queued emergencies need and drives those under
    /// way. True when this slot's TSCC block gives way to an RC burst (the talker is on the TSCC carrier's other
    /// slot, so the TSCC's outbound is its alternate slot): the slot then carries an idle burst with the RC.
    pub(super) fn preempt_tick(&mut self, out: &mut SlotOut) -> bool {
        if self.cfg.preempt == 0 || self.withdrawn {
            return false;
        }
        let waiting = self.pend.iter().filter(|p| p.used && p.state == P_QUEUED && p.svc & SO_EMERGENCY != 0).count()
            + self.net.iter().filter(|n| n.used && n.emergency).count();
        let going = self.chans.iter().filter(|c| c.r#use == USE_CLEAR || (c.preempt && Self::live(c))).count();
        if waiting == 0 {
            for c in self.chans.iter_mut().filter(|c| c.preempt && Self::live(c)) {
                c.preempt = false; // nothing waits any more: the call goes on
            }
        } else if waiting > going && self.free_channel().is_none() {
            if let Some(ch) = self.victim() {
                let now = self.now;
                let c = &mut self.chans[ch];
                c.preempt = true;
                c.preempt_until_ms = now + DEKEY_MS;
                c.rc_next_ms = now;
                let (src, dst) = (c.src, c.dst);
                self.log(format_args!("pre-empting the call {} to {} on channel {} for an emergency", src, dst, ch));
            }
        }
        let (now, lcn, tscc) = (self.now, (self.k & 1) as usize, self.tscc_carrier * 2);
        let mut rc_on_tscc = false;
        for ch in 0..CHANNELS {
            let c = self.chans[ch];
            if !c.preempt || !Self::live(&c) {
                continue;
            }
            if !c.talking || c.talking_net || now >= c.preempt_until_ms || c.kind == KIND_DUPLEX {
                // nobody on RF to de-key, the talker stopped, or it didn't in time (or a duplex call, whose
                // parties don't listen on the other slot): P_CLEAR now
                self.clear(ch, CLOSE_PREEMPT);
                continue;
            }
            let alt = ch ^ 1;
            if alt & 1 == lcn && now >= c.rc_next_ms {
                out.ch[alt].rc = RC_SEND | RC_CEASE;
                self.chans[ch].rc_next_ms = now + RC_EVERY_MS;
                rc_on_tscc |= alt == tscc;
            }
        }
        rc_on_tscc
    }
}
