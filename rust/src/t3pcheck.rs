// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! P_AHOY on a voice payload channel (docs/spec/fw/tier3.md section 3.15): ETSI TS 102 361-4 6.6.2.3.1.1, the MS
//! radio check, and 6.6.2.3.1.8, the selective clear. Both are a P_AHOY from TSI on the channel's own slot while
//! nobody talks (the hang time, as P_PROTECT, t3intr.rs), answered with P_ACKU(MS_Accepted) to TSI on the inbound
//! (6.6.2.3.2.1, 6.6.2.3.2.7).
//!
//! - Radio check: table 6.42 (an MS, Service_Kind Individual Call 0000b) or 6.43 (a talkgroup, Talkgroup Call
//!   0001b), Service_Options_Mirror 0, Service_Kind_Flag 0, Appended_Blocks 0. For a talkgroup, the first answer
//!   ends it, or a burst that doesn't decode (6.6.2.3.1.1: members' answers collide; "the TS may use the presence
//!   of the burst for the result of the talkgroup radio check").
//! - Selective clear: table 6.44 (an MS: Cancel Call 1111b, Service_Kind_Flag 0) or 6.45 (a talkgroup, flag 1).
//!   The MS answers and leaves for the TSCC; the call goes on for the others. For a talkgroup (the answers, or
//!   their colliding burst) the site then clears the call (P_CLEAR), as the `clear` command does, for members
//!   that missed it.
//!
//! The site's `radio_check` command sends this when the MS is a party of an individual call on a payload channel,
//! or the talkgroup has a call open; else the C_AHOY on the TSCC (6.4.12, t3sdata.rs). `clear_ms` is the selective
//! clear. A child module of t3site: it works on the site's own state.

use super::*;

/// The P_AHOY goes out at most every this often while the slot is free.
const PA_EVERY_MS: i64 = 360;
/// After the last P_AHOY, the wait for its answer.
const PA_ANSWER_MS: i64 = 500;
/// The whole check, while the channel's talkers leave no hang time for it.
const PA_WAIT_MS: i64 = 30_000;

#[derive(Clone, Copy, Default)]
pub(super) struct PaCheck {
    target: u32, // 0: none
    group: bool,
    clear: bool,
    left: u8,
    sent: bool,
    next_ms: i64,
    until_ms: i64,
}

impl Site {
    /// The voice payload channel the MS is on as a party of an individual call (or the talker of a group call),
    /// or the talkgroup's open call.
    fn pa_channel(&self, target: u32, group: bool) -> Option<usize> {
        (0..CHANNELS).find(|&i| {
            let c = &self.chans[i];
            if !Self::live(c) || c.flags & F_DATA != 0 || c.kind == KIND_DUPLEX {
                return false;
            }
            if group {
                matches!(c.kind, KIND_GROUP | KIND_ALLCALL) && c.dst == target
            } else {
                (c.kind == KIND_PRIVATE && (c.src == target || c.dst == target))
                    || (c.kind != KIND_PRIVATE && (c.src == target || c.talker == target))
            }
        })
    }

    /// A radio check (clear false) or a selective clear of target on its payload channel. False when it's on
    /// none, or the channel has one going.
    pub(super) fn pa_start(&mut self, target: u32, group: bool, clear: bool) -> bool {
        let Some(ch) = self.pa_channel(target, group) else { return false };
        if self.chans[ch].pa.target != 0 {
            return false;
        }
        let now = self.now;
        self.chans[ch].pa = PaCheck {
            target,
            group,
            clear,
            left: self.cfg.sd_tries.max(1),
            next_ms: now,
            until_ms: now + PA_WAIT_MS,
            ..Default::default()
        };
        true
    }

    /// Whether radio_check should go on the payload channel: the MS (or the talkgroup) is on one.
    pub(super) fn pa_applies(&self, target: u32, group: bool) -> bool {
        self.pa_channel(target, group).is_some()
    }

    fn pa_pdu(p: &PaCheck) -> [u8; 10] {
        let service_kind = match (p.clear, p.group) {
            (true, _) => SK_CANCEL,
            (false, false) => SK_IND_VOICE,
            (false, true) => SK_TG_VOICE,
        };
        let (group, flag, target) = (p.group, p.clear && p.group, p.target);
        Ahoy { group, service_kind, service_kind_flag: flag, target, source: TSI, ..Default::default() }.encode()
    }

    fn pa_what(p: &PaCheck) -> &'static str {
        if p.clear {
            "selective clear"
        } else {
            "radio check"
        }
    }

    /// Channel ch's own slot (payload_slot): its P_AHOY while nobody talks, the check's end when it's done.
    pub(super) fn pa_slot(&mut self, ch: usize, out: &mut ChOut) {
        let c = self.chans[ch];
        let p = c.pa;
        if p.target == 0 {
            return;
        }
        if self.now >= p.until_ms {
            self.pa_end(ch, None);
            return;
        }
        if c.r#use != USE_HANG || c.protect_ms != 0 || p.left == 0 || self.now < p.next_ms {
            return;
        }
        out.dt = DT_CSBK;
        out.pdu = Self::pa_pdu(&p);
        let pa = &mut self.chans[ch].pa;
        pa.left -= 1;
        pa.sent = true;
        pa.next_ms = self.now + PA_EVERY_MS;
        if pa.left == 0 {
            pa.until_ms = pa.until_ms.min(self.now + PA_ANSWER_MS);
        }
    }

    /// A P_ACKU to TSI on channel ch: the answer to its check.
    pub(super) fn pa_answer(&mut self, ch: usize, a: &Ack) -> bool {
        let p = self.chans[ch].pa;
        if p.target == 0 || a.target != TSI || (!p.group && a.source != p.target) {
            return false;
        }
        self.heard(a.source);
        self.pa_end(ch, Some((a.source, a.reason)));
        true
    }

    /// An inbound burst on channel ch that didn't decode: for a talkgroup's check or clear, its members answering.
    pub(super) fn pa_burst(&mut self, ch: usize) {
        let p = self.chans[ch].pa;
        if p.target != 0 && p.group && p.sent {
            self.pa_end(ch, Some((0, R_MS_ACCEPTED)));
        }
    }

    /// The check's end: logged; a talkgroup's selective clear then clears the call.
    fn pa_end(&mut self, ch: usize, a: Option<(u32, u8)>) {
        let p = self.chans[ch].pa;
        self.chans[ch].pa = PaCheck::default();
        let (what, to) = (Self::pa_what(&p), p.target);
        match a {
            // the event's text holds 63 characters: "selective clear of TG 16777215 on payload: answered by 16777215"
            None => self.log(format_args!("{what} of {to} on payload: no answer")),
            Some((_, reason)) if reason_type(reason) != 1 => {
                self.log(format_args!("{what} of {to} on payload: refused ({reason:#04x})"))
            }
            Some((0, _)) => self.log(format_args!("{what} of TG {to} on payload: answers collided")),
            Some((by, _)) if p.group => self.log(format_args!("{what} of TG {to} on payload: answered by {by}")),
            Some(_) => self.log(format_args!("{what} of {to} on payload: answered")),
        }
        if p.clear && p.group && Self::live(&self.chans[ch]) {
            self.clear(ch, CLOSE_CMD);
        }
    }

    /// close() of channel ch with a check going: it ends unanswered.
    pub(super) fn pa_closed(&mut self, ch: usize) {
        let p = self.chans[ch].pa;
        if p.target != 0 {
            self.chans[ch].pa = PaCheck::default();
            let what = Self::pa_what(&p);
            self.log(format_args!("{what} of {} on payload: the call ended", p.target));
        }
    }
}
