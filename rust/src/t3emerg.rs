// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! Emergency pre-emption (docs/spec/fw/tier3.md section 3.9): ETSI TS 102 361-4 6.4.14.3.1, "Emergency
//! Pre-emption MS De-key". When an emergency call waits in the queue and no payload channel is free or coming free,
//! the TS may clear a non-emergency call. If a party is transmitting, the TS first sends it the Reverse Channel
//! "cease transmission" command (RC info 0100b, table 6.32) on the alternate slot of its carrier (361-1 5.1.5);
//! the MS finishes its superframe, sends a terminator and stops (6.4.15). When the inbound stops, P_CLEAR goes out
//! on the payload channel and the freed channel goes to the emergency call (the queue serves emergencies first).
//! A talker that doesn't cease in DEKEY_MS (an MS without the RC, 6.4.14.0) gets P_CLEAR over it, but the channel
//! stays out of use, still sending P_CLEAR, until its inbound stops: the emergency call is connected there only
//! "following the cessation of inbound PDUs". Meanwhile another call may be pre-empted for it.
//! A child module of t3site: it works on the site's own state.

use super::*;
use crate::t3rc::RC_CEASE;

/// ChOut.rc: an RC PDU is to be sent (the low 4 bits are the RC info).
pub const RC_SEND: u8 = 0x10;
/// The longest wait for a talker told to cease before the channel is cleared over it.
pub(super) const DEKEY_MS: i64 = 3000;
/// 361-1 5.1.5.1: repeated RC messages no sooner than 360 ms apart.
const RC_EVERY_MS: i64 = 360;
/// A held channel's inbound is quiet after this long without voice (the radio layer reports a talker once a
/// superframe, 360 ms).
const HELD_QUIET_MS: i64 = 1000;
/// The longest a channel is held for a talker that won't stop.
const HELD_MAX_MS: i64 = 60_000;

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
        let going = self.chans.iter().filter(|c| (c.r#use == USE_CLEAR && !c.held) || (c.preempt && Self::live(c))).count();
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
        let now = self.now;
        let mut rc_on_tscc = false;
        for ch in 0..CHANNELS {
            let c = self.chans[ch];
            if !c.preempt || !Self::live(&c) {
                continue;
            }
            if !c.talking || c.talking_net || now >= c.preempt_until_ms || c.kind == KIND_DUPLEX {
                // nobody on RF to de-key, the talker stopped, or it didn't in time (or a duplex call, whose
                // parties don't listen on the other slot): P_CLEAR now
                let keyed = c.talking && !c.talking_net;
                self.clear(ch, CLOSE_PREEMPT);
                if keyed {
                    let c = &mut self.chans[ch];
                    c.held = true;
                    c.preempt_until_ms = now + HELD_MAX_MS;
                    self.log(format_args!("channel {} held: its talker didn't cease", ch));
                }
                continue;
            }
            rc_on_tscc |= self.send_rc(ch, out);
        }
        rc_on_tscc
    }

    /// A held channel (cleared over a talker that didn't cease) still hears that talker: P_CLEAR goes on.
    pub(super) fn still_keyed(&self, ch: usize) -> bool {
        let c = &self.chans[ch];
        c.held && self.now - c.last_voice_ms < HELD_QUIET_MS && self.now < c.preempt_until_ms
    }

    /// Inbound voice on a held channel: the talker is still there, or (terminator, lost) gone.
    pub(super) fn held_voice(&mut self, ch: usize, what: u8) {
        let now = self.now;
        let c = &mut self.chans[ch];
        c.last_voice_ms = if matches!(what, VOICE_HEADER | VOICE_FRAME) { now } else { i64::MIN / 2 };
    }

    /// The RC cease command for channel ch's talker, on its carrier's other slot when that slot is this one and
    /// the last went RC_EVERY_MS ago or more. True when that slot is the TSCC's (its block gives way).
    pub(super) fn send_rc(&mut self, ch: usize, out: &mut SlotOut) -> bool {
        let (now, lcn, tscc) = (self.now, (self.k & 1) as usize, self.tscc_carrier * 2);
        let alt = ch ^ 1;
        if alt & 1 != lcn || now < self.chans[ch].rc_next_ms {
            return false;
        }
        out.ch[alt].rc = RC_SEND | RC_CEASE;
        self.chans[ch].rc_next_ms = now + RC_EVERY_MS;
        alt == tscc && !self.tscc_payload() // a composite TSCC's call: its payload path
    }
}
