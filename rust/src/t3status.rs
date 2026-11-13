// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! The Status Transport Service's delivery (docs/spec/fw/tier3.md section 3.10): ETSI TS 102 361-4 6.6.6.1, the
//! store and forward of a 7-bit status from an MS to another MS, a talkgroup or the TSCC, with the emergency alarm
//! (status 126) and its cancel (124) of 6.6.6.3. The C_RAND carries the status in Service_Options (G/I, SUPED_SV,
//! the 5 high bits) and the 2 bits before the Service_Kind (table 6.57); the C_AHOY to the called party in
//! Service_Options_Mirror and Appended_Blocks (table 7.22, note 2). An individual called MS answers C_ACKU or
//! C_NACKU, mirrored to the caller; a talkgroup doesn't answer and the caller gets C_ACKD(Message_Accepted).
//! Status polling (127) and the transmit interrupt statuses (123, 125) aren't served here. Local delivery only:
//! a called party not registered at this site gets the caller C_NACKD(MSaway_Refused).
//! A child module of t3site: it works on the site's own state.

use super::*;

pub(super) const STATUS_PENDING: usize = 8;

pub const STATUS_CANCEL_ALARM: u8 = 124;
pub const STATUS_EMERGENCY_ALARM: u8 = 126;
pub const STATUS_POLL: u8 = 127;

/// A status on its way to an individual MS: C_AHOY sent, waiting for its C_ACKU or C_NACKU.
#[derive(Clone, Copy, Default)]
pub(super) struct StatPend {
    used: bool,
    status: u8,
    ahoy_left: u8,
    caller: u32,
    called: u32,
    next_ahoy_ms: i64,
    deadline_ms: i64,
}

/// The status a C_RAND (Service_Kind 0111b) carries: STATUS(5) in Service_Options' low bits, STATUS(2) in the
/// low two of the four bits before the Service_Kind (table 6.57).
pub fn status_of(r: &Rand) -> u8 {
    (r.service_options & 0x1F) << 2 | r.dep & 0x03
}

/// The C_RAND of a status delivery (table 6.57): G/I is Service_Options' top bit.
pub fn status_rand(status: u8, group: bool, target: u32, source: u32) -> Rand {
    Rand {
        service_options: if group { 0x40 } else { 0 } | status >> 2 & 0x1F,
        proxy: false,
        dep: status & 0x03,
        service_kind: SK_STATUS,
        target,
        source,
    }
}

/// The C_AHOY that delivers a status (table 7.22, note 2).
pub fn status_ahoy(status: u8, group: bool, target: u32, source: u32) -> Ahoy {
    Ahoy {
        service_options: if group { 0x40 } else { 0 } | status >> 2 & 0x1F,
        group,
        appended: status & 0x03,
        service_kind: SK_STATUS,
        target,
        source,
        ..Default::default()
    }
}

/// The status a C_AHOY delivers.
pub fn ahoy_status(a: &Ahoy) -> u8 {
    (a.service_options & 0x1F) << 2 | a.appended & 0x03
}

impl Site {
    /// Whether the site knows ms is on a payload channel: a party of an individual call, or the one who set up or
    /// last talked in a group call (a listener's whereabouts aren't known: the C_AHOY repeats cover it).
    fn on_payload(&self, ms: u32) -> bool {
        self.ms_busy(ms) || self.chans.iter().any(|c| Self::live(c) && (c.src == ms || c.talker == ms))
    }

    fn status_done(&mut self, status: u8, group: bool, caller: u32, called: u32, reason: u8) {
        let flags = if group { F_GROUP } else { 0 };
        self.ev(Event { r#type: EV_ACTIVITY, kind: ACT_STATUS, op: status, flags, src: caller, dst: called, reason, ..Default::default() });
        match status {
            STATUS_EMERGENCY_ALARM => self.log(format_args!("emergency alarm from {} to {}", caller, called)),
            STATUS_CANCEL_ALARM => self.log(format_args!("emergency alarm from {} cancelled", caller)),
            _ => {}
        }
    }

    /// C_RAND, Service_Kind Status Transport (6.6.6.1.1).
    pub(super) fn status_request(&mut self, r: &Rand) {
        let (ms, dst, status) = (r.source, r.target, status_of(r));
        let group = r.service_options & 0x40 != 0;
        // a repeat while its C_AHOY is out: tell it to wait
        if self.stat.iter().any(|p| p.used && p.caller == ms && p.called == dst) {
            self.ack(R_WAIT, ms, dst);
            return;
        }
        // polling (the data services'), the transmit interrupt statuses, gateways and extended addresses
        let tsi = dst == TSI && !group;
        if status == STATUS_POLL || matches!(status, 123 | 125) || r.proxy || (is_gateway(dst) && !tsi) {
            self.refuse(R_NOT_SUPPORTED, ms, dst);
            return;
        }
        if dst == ADRNULL || (!group && dst == ms) {
            self.refuse(R_NOT_SUPPORTED, ms, dst);
            return;
        }
        if !self.allowed(ms) {
            self.refuse(R_PERM_USER_REFUSED, ms, dst);
            return;
        }
        if self.reg_index(ms).is_none() && self.reg_bit() {
            self.refuse(R_MS_NOT_REGISTERED, ms, dst);
            return;
        }
        if status == STATUS_EMERGENCY_ALARM && self.ms_policy(ms).emergency_ok == 0 {
            self.refuse(R_PERM_USER_REFUSED, ms, dst);
            return;
        }
        // SUPED_SV (supplementary data) asked: no UDT here, the status goes without it (6.6.6.1.2.1.2 a)
        if tsi {
            // to the TSCC: a system message, taken here
            self.ack(R_MESSAGE_ACCEPTED, ms, dst);
            self.status_done(status, false, ms, dst, R_MESSAGE_ACCEPTED);
            return;
        }
        if group {
            if !is_allcall(dst) && !self.policy.tg.contains(dst) {
                self.refuse(R_GROUP_NOT_ALLOWED, ms, dst);
                return;
            }
            // no answer from a talkgroup: the C_AHOY twice for reliability, then the caller's C_ACKD (6.6.6.1.1.4)
            let a = status_ahoy(status, true, dst, ms).encode();
            self.push(DT_CSBK, a, false, None);
            self.push(DT_CSBK, a, false, None);
            self.ack(R_MESSAGE_ACCEPTED, ms, dst);
            self.status_done(status, true, ms, dst, R_MESSAGE_ACCEPTED);
            return;
        }
        if self.reg_index(dst).is_none() {
            self.refuse(R_MSAWAY, ms, dst);
            self.status_done(status, false, ms, dst, R_MSAWAY);
            return;
        }
        let Some(i) = self.stat.iter().position(|p| !p.used) else {
            self.refuse(R_SYSBUSY, ms, dst);
            return;
        };
        self.stat[i] = StatPend {
            used: true,
            status,
            ahoy_left: self.cfg.ahoy_tries,
            caller: ms,
            called: dst,
            next_ahoy_ms: self.now,
            deadline_ms: self.now + self.cfg.tp_ms as i64,
        };
        if self.on_payload(dst) {
            self.ack(R_QUEUED_BUSY, ms, dst); // stored until the called MS is back on the TSCC
        }
        self.status_tick(i);
    }

    fn status_tick(&mut self, i: usize) {
        let p = self.stat[i];
        if self.on_payload(p.called) {
            return; // on a payload channel: it can't hear the TSCC
        }
        if !self.ps_awake(p.called) {
            // asleep (power save): held for its wake frame
            self.stat[i].deadline_ms = p.deadline_ms.max(self.now + self.ps_wait_ms(p.called));
            return;
        }
        if p.ahoy_left == 0 {
            self.stat[i].used = false;
            self.refuse(R_MSAWAY, p.caller, p.called);
            self.status_done(p.status, false, p.caller, p.called, R_MSAWAY);
            return;
        }
        self.push(DT_CSBK, status_ahoy(p.status, false, p.called, p.caller).encode(), true, None);
        let q = &mut self.stat[i];
        q.ahoy_left -= 1;
        q.next_ahoy_ms = self.now + self.cfg.ahoy_every_ms as i64;
    }

    /// Per slot: C_AHOY repeats, and the store's time limit (6.6.6.1.1.5: TP_Timer).
    pub(super) fn status_timers(&mut self) {
        for i in 0..STATUS_PENDING {
            let p = self.stat[i];
            if !p.used {
                continue;
            }
            if self.now >= p.deadline_ms {
                self.stat[i].used = false;
                self.refuse(R_MSAWAY, p.caller, p.called);
                self.status_done(p.status, false, p.caller, p.called, R_MSAWAY);
            } else if self.now >= p.next_ahoy_ms {
                self.status_tick(i);
            }
        }
    }

    /// The called MS's C_ACKU / C_NACKU to a status C_AHOY: mirrored to the caller. False when it isn't one.
    pub(super) fn status_ack(&mut self, a: &Ack) -> bool {
        let Some(i) = self.stat.iter().position(|p| p.used && p.called == a.source && p.caller == a.target) else {
            return false;
        };
        let p = self.stat[i];
        self.stat[i].used = false;
        self.ack(a.reason, p.caller, p.called); // C_ACKD(MS_Accepted) or the C_NACKD, the called MS's reason
        if reason_type(a.reason) == 0 {
            self.activity(ACT_REFUSE, 0, p.caller, p.called, a.reason);
        }
        self.status_done(p.status, false, p.caller, p.called, a.reason);
        true
    }
}
