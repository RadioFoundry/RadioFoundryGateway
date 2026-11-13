// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! Transmit interrupt (docs/spec/fw/tier3.md section 3.20): ETSI TS 102 361-4 6.4.14.3.2-6.4.14.3.4 and 6.4.15,
//! with `services.transmit_interrupt` (cfg.intr). "If a Tier III TS or MS supports this feature, the feature shall
//! be implemented as described in this clause and in clause 6.4.15" (6.4.14.0): optional, off by default.
//!
//! - Payload interrupt (6.4.14.3.3): an MS in a voice call leaves the payload channel and sends a status call,
//!   status 125, to the talkgroup or to the MS to interrupt. The TSCC matches it with a voice call on the payload
//!   channels and answers C_ACKD(Message_Accepted) or C_NACKD from TSI (6.4.14.3.3.1). Accepted, the talker on RF
//!   gets the Reverse Channel cease command (RC 0100b, table 6.32) on its carrier's other slot until it stops
//!   (6.4.15.1); then the channel sends P_PROTECT(EN_PTT_ONE_MS) to the interrupter, a limited number of times,
//!   until it talks (6.4.15.1, 6.6.2.3.1.3). A talker that doesn't cease keeps the call (6.4.15.1).
//! - ALS termination (6.4.14.3.4): the caller of an Ambient Listening Service call sends status 123 to the called
//!   MS. Accepted, the called MS is de-keyed the same way, then P_CLEAR ends the call (6.4.14.3.4.2).
//! - Call timer expiration de-key (6.4.14.3.2): when a call's timer runs out while an MS talks on RF, the RC cease
//!   command first, then P_CLEAR once the inbound stops.
//!
//! The RC itself is the pre-emption's (t3emerg.rs, `send_rc`). A child module of t3site: it works on the site's
//! own state.

use super::emerg::DEKEY_MS;
use super::*;
use crate::t3udt::rand_status_group;

/// 6.6.6.0: "a system status defined for the Transmit Interrupt service to request the termination of a call"
/// (an ALS call, 6.4.14.3.4).
pub const STATUS_ALS_END: u8 = 123;
/// 6.6.6.0: "a system status defined for the Transmit Interrupt procedure" (payload interrupt, 6.4.14.3.3).
pub const STATUS_INTERRUPT: u8 = 125;

/// EV_ACTIVITY kind: a transmit interrupt's progress. op INTR_*, src the interrupting MS, dst the call's
/// destination, ch its channel; reason Message_Accepted with INTR_ACCEPTED.
pub const ACT_INTR: u8 = 8;
pub const INTR_ACCEPTED: u8 = 1; // C_ACKD to the interrupter
pub const INTR_CEASED: u8 = 2; // the talker stopped
pub const INTR_GAVE_UP: u8 = 3; // it didn't within DEKEY_MS: the call goes on (an ALS call is cleared)
pub const INTR_TALKS: u8 = 4; // the interrupter talks on the channel

// Chan.dekey
const DK_NONE: u8 = 0;
const DK_INTR: u8 = 1; // payload interrupt: then P_PROTECT for the interrupter
const DK_ALS: u8 = 2; // ALS termination: then P_CLEAR
const DK_TIMER: u8 = 3; // the call timer ran out: then P_CLEAR

/// How long the channel waits for the interrupter after the talker stops, sending P_PROTECT(EN_PTT_ONE_MS) in every
/// other burst of its slot (at most 17): "The TS shall limit the number of P_Protect PDUs so that MS occupying the
/// payload channel may continue if the Transmit Interrupt fails for any reason" (6.4.15.1).
const PROTECT_MS: i64 = 2000;

impl Site {
    /// A status 123 or 125 C_RAND (6.4.14.3.3.1, 6.4.14.3.4.2), after the usual status checks (the MS allowed,
    /// registered). Every answer comes from TSI to the interrupting MS.
    pub(super) fn intr_request(&mut self, r: &Rand, st: u8) {
        let ms = r.source;
        if self.cfg.intr == 0 {
            // "If Transmit Interrupt is not available the TSCC shall respond C_NACKD (Source = TSI, Target =
            // Interrupting MS)"
            self.refuse(R_NOT_SUPPORTED, ms, TSI);
            return;
        }
        let group = rand_status_group(r);
        let found =
            if st == STATUS_ALS_END { self.als_call(ms, r.target, group) } else { self.intr_call(ms, r.target, group) };
        let ch = match found {
            Ok(ch) => ch,
            Err(reason) => {
                self.refuse(reason, ms, TSI);
                return;
            }
        };
        self.ack(R_MESSAGE_ACCEPTED, ms, TSI);
        let c = self.chans[ch];
        if c.dekey != DK_NONE || c.protect_ms == ms {
            return; // a repeat (the MS missed the answer): answered again
        }
        self.intr_ev(ch, INTR_ACCEPTED, ms);
        self.log(format_args!("transmit interrupt from {} for the call to {} on channel {}", ms, c.dst, ch));
        let rf = c.talking && !c.talking_net;
        match (st == STATUS_ALS_END, rf) {
            (true, false) => self.clear(ch, CLOSE_INTR), // the called MS isn't transmitting: P_CLEAR now
            (true, true) => self.dekey_start(ch, DK_ALS, ms),
            (false, true) => self.dekey_start(ch, DK_INTR, ms),
            (false, false) => self.protect_start(ch, ms), // nobody to de-key: the channel waits for it
        }
    }

    /// The voice call a payload interrupt (status 125) from ms to target is for: a talkgroup's call with G/I 1, an
    /// individual call target is a party of with G/I 0 (or a talkgroup call target is talking in: "the called
    /// party address shall be matched with the MS IDs engaged in voice calls on the payload channels"). The C_NACKD
    /// reasons are the two an interrupting MS reads (6.4.14.3.3.2): Not_Supported, Perm_User_Refused.
    fn intr_call(&self, ms: u32, target: u32, group: bool) -> Result<usize, u8> {
        let ch = if group {
            self.group_channel(target)
        } else {
            (0..CHANNELS).find(|&i| {
                let c = &self.chans[i];
                let party = c.kind == KIND_PRIVATE && (c.src == target || c.dst == target);
                let talker = c.kind == KIND_GROUP && c.talking && !c.talking_net && c.talker == target;
                Self::live(c) && c.flags & F_DATA == 0 && (party || talker)
            })
        };
        let Some(ch) = ch else { return Err(R_NOT_SUPPORTED) }; // no voice call, or a duplex or data call
        let c = &self.chans[ch];
        let party = match c.kind {
            KIND_PRIVATE => c.src == ms || c.dst == ms,
            KIND_GROUP => self.policy.tg.contains(c.dst),
            _ => false, // an all-call or a broadcast call: one way, the called parties don't transmit (6.6.2)
        };
        let theirs = (c.dekey == DK_INTR && c.intr_ms != ms) || (c.protect_ms != 0 && c.protect_ms != ms);
        if !party || theirs {
            return Err(R_PERM_USER_REFUSED);
        }
        let ours = c.dekey == DK_INTR || c.protect_ms == ms;
        if !ours && (c.preempt || c.dekey != DK_NONE || (c.talking && (c.talking_net || c.talker == ms))) {
            // being cleared, a network talker (the site can't de-key it), or the MS itself talking
            return Err(R_NOT_SUPPORTED);
        }
        Ok(ch)
    }

    /// The ALS call an ALS termination (status 123) from ms to target is for: an ALS call with target the called MS
    /// (6.4.14.3.4.2), ms its caller ("the MS ... that initiated the ALS service", 6.4.14.3.4.1).
    fn als_call(&self, ms: u32, target: u32, group: bool) -> Result<usize, u8> {
        let ch = (0..CHANNELS).find(|&i| {
            let c = &self.chans[i];
            Self::live(c) && c.als && c.dst == target
        });
        let Some(ch) = ch.filter(|_| !group) else { return Err(R_NOT_SUPPORTED) };
        let c = &self.chans[ch];
        if c.src != ms {
            return Err(R_PERM_USER_REFUSED);
        }
        if c.preempt || !matches!(c.dekey, DK_NONE | DK_ALS) {
            return Err(R_NOT_SUPPORTED); // being cleared already
        }
        Ok(ch)
    }

    fn dekey_start(&mut self, ch: usize, kind: u8, ms: u32) {
        let now = self.now;
        let c = &mut self.chans[ch];
        c.dekey = kind;
        c.dekey_until_ms = now + DEKEY_MS;
        c.rc_next_ms = now;
        c.intr_ms = ms;
        c.protect_ms = 0;
    }

    /// The call timer of channel ch ran out (6.4.14.3.2): true while its RF talker is being de-keyed first (the
    /// call is cleared once it stops, or after DEKEY_MS); false to clear it now.
    pub(super) fn timer_dekey(&mut self, ch: usize) -> bool {
        let c = self.chans[ch];
        if c.dekey == DK_TIMER {
            return true;
        }
        if self.cfg.intr == 0 || !c.talking || c.talking_net || c.kind == KIND_DUPLEX || c.preempt {
            return false;
        }
        self.dekey_start(ch, DK_TIMER, 0);
        self.log(format_args!("call timer: de-keying {} on channel {}", c.talker, ch));
        true
    }

    /// Per slot, before the TSCC's block: drives the de-keys under way. True when this slot's TSCC block gives way
    /// to an RC burst (as preempt_tick).
    pub(super) fn intr_tick(&mut self, out: &mut SlotOut) -> bool {
        let now = self.now;
        let mut rc_on_tscc = false;
        for ch in 0..CHANNELS {
            let c = self.chans[ch];
            if c.dekey == DK_NONE {
                continue;
            }
            if !Self::live(&c) || c.preempt {
                self.chans[ch].dekey = DK_NONE; // cleared meanwhile, or the pre-emption's now
                continue;
            }
            let rf = c.talking && !c.talking_net;
            if rf && now < c.dekey_until_ms {
                rc_on_tscc |= self.send_rc(ch, out);
                continue;
            }
            self.chans[ch].dekey = DK_NONE;
            let op = if rf { INTR_GAVE_UP } else { INTR_CEASED };
            match c.dekey {
                DK_INTR => {
                    self.intr_ev(ch, op, c.intr_ms);
                    if rf {
                        // 6.4.15.1: "If the MS continues to transmit the TS shall continue to service the ongoing
                        // voice call"
                        self.log(format_args!("transmit interrupt: {} didn't cease; the call goes on", c.talker));
                    } else {
                        self.protect_start(ch, c.intr_ms);
                    }
                }
                DK_ALS => {
                    // 6.4.14.3.4.2: "Following the cessation of inbound PDUs, the payload channel shall send
                    // additional P_Clear PDUs"; one that doesn't cease is cleared too (its caller has left)
                    self.intr_ev(ch, op, c.intr_ms);
                    self.clear(ch, CLOSE_INTR);
                }
                _ => self.clear(ch, CLOSE_TIMER),
            }
        }
        rc_on_tscc
    }

    /// The talker stopped (or there was none): the channel waits PROTECT_MS for the interrupter, its hang time at
    /// least that long.
    fn protect_start(&mut self, ch: usize, ms: u32) {
        let until = self.now + PROTECT_MS;
        let c = &mut self.chans[ch];
        c.protect_ms = ms;
        c.protect_until_ms = until;
        c.idle_until_ms = c.idle_until_ms.max(until);
    }

    /// Channel ch's own slot while it waits for the interrupter: P_PROTECT(EN_PTT_ONE_MS) to it in place of a hang
    /// time terminator, every other burst, until it talks or PROTECT_MS pass (6.4.15.1; 6.6.2.3.1.3: "the MS
    /// addressed by this PDU shall enable the PTT and is allowed to transmit as soon as the payload channel is
    /// free. However, while the PDUs are being transmitted, all other MS shall disable the PTT").
    pub(super) fn protect_slot(&mut self, ch: usize, out: &mut ChOut) {
        let c = self.chans[ch];
        if !Self::live(&c) || c.talking || self.now >= c.protect_until_ms {
            self.chans[ch].protect_ms = 0;
            if Self::live(&c) && c.talking && c.talker == c.protect_ms {
                self.intr_ev(ch, INTR_TALKS, c.protect_ms);
            } else if Self::live(&c) && c.talking {
                self.log(format_args!("transmit interrupt: {} talks in place of {}", c.talker, c.protect_ms));
            }
            return;
        }
        if c.r#use == USE_HANG && (self.k >> 1) & 1 == 0 {
            let p = Protect { kind: PROTECT_EN_PTT_ONE_MS, group: false, target: c.protect_ms, source: TSI };
            out.dt = DT_CSBK;
            out.pdu = p.encode();
        }
    }

    fn intr_ev(&mut self, ch: usize, op: u8, ms: u32) {
        let mut e = self.chan_ev(ch, EV_ACTIVITY);
        e.kind = ACT_INTR;
        e.op = op;
        e.src = ms;
        if op == INTR_ACCEPTED {
            e.reason = R_MESSAGE_ACCEPTED;
        }
        self.ev(e);
    }
}
