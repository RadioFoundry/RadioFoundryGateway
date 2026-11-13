// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! Supplementary services of the site controller (docs/spec/fw/tier3.md 3.18): stun and revive without
//! authentication (ETSI TS 102 361-4 6.4.9.1), the Ambient Listening Service (6.6.2.6) and call diversion (6.6.7).
//! A part of the site controller (a child module of t3site).
//!
//! Stun and revive come from the site (the "stun" and "revive" commands): a C_AHOY from STUNI (table 6.19) that
//! the MS answers C_ACKU Message_Accepted, or C_NACKU MSNot_Supported. A stunned MS gets no user services here
//! until it is revived; registration stays. Kill (6.4.10) needs authentication and isn't built.
//!
//! Call diversion: an MS sets it with a multi-part request (C_RAND 1000b to MSI or GPI, C_AHOY from DIVERTI, the
//! address uploaded), clears its own (target TSI) or the ones that point at it (target DIVERTI). An individual
//! request for a diverted service to it then gets a UDT header with the diverted address instead (6.6.7.2).

use super::*;
use crate::t3udt::*;

/// MSs stunned at once (more: the next stun isn't kept, and its result says so).
pub const STUNNED: usize = 32;
/// Stun or revive commands in progress at once.
pub const STUN_JOBS: usize = 4;
/// MSs with a call diversion at once.
pub const DIVERSIONS: usize = 32;

/// EV_ACTIVITY kind of a supplementary service's result: op SUPP_*, src the MS, dst the address (the diverted
/// address; for stun STUNI), reason the air reason (Message_Accepted done; MSNot_Supported, MSaway_Refused ...).
pub const ACT_SUPP: u8 = 7;
pub const SUPP_STUN: u8 = 1;
pub const SUPP_REVIVE: u8 = 2;
pub const SUPP_DIVERT: u8 = 3;
pub const SUPP_DIVERT_CLEAR: u8 = 4;

// Service_Options of a call diversion request (table 7.54): Emergency, Privacy, Divert On/Off, Divert Kind (4)
pub const DIV_ON: u8 = 0x10;
pub const DIV_VOICE: u8 = 0x08;
pub const DIV_PD: u8 = 0x04;
pub const DIV_SD: u8 = 0x02;
pub const DIV_STATUS: u8 = 0x01;
pub const DIV_KINDS: u8 = 0x0F;

/// ALS_SERV in a voice C_RAND (table 6.40): the dependent bits are SUPED_VAL (2), ALS_SERV, reserved.
pub const DEP_ALS: u8 = 0b0010;

#[derive(Clone, Copy, Default)]
struct StunJob {
    used: bool,
    revive: bool,
    left: u8,
    ms: u32,
    next_ms: i64,
}

/// One MS's diversions, one address per kind (voice, packet data, short data, status: DIV_* from the MSB).
#[derive(Clone, Copy, Default)]
struct Div {
    ms: u32,      // 0: free
    to: [u32; 4], // 0: not diverted
    group: u8,    // bit j: to[j] is a talkgroup (GPI)
}

/// The supplementary services' state (all zero: nothing).
#[derive(Clone, Copy, Default)]
pub(super) struct SuppState {
    stunned: [u32; STUNNED],
    jobs: [StunJob; STUN_JOBS],
    div: [Div; DIVERSIONS],
}

/// The DIV_* bit of kind j (the index into Div.to).
fn kind_bit(j: usize) -> u8 {
    DIV_VOICE >> j
}

impl Site {
    // ── stun and revive (6.4.9.1) ──

    /// An MS this site has stunned and not revived.
    pub fn stunned(&self, ms: u32) -> bool {
        ms != 0 && self.supp.stunned.contains(&ms)
    }

    /// Stuns (revive false) or revives ms: C_AHOY from STUNI until it answers or ahoy_tries run out. The
    /// result comes as an EV_ACTIVITY ACT_SUPP. False when ms isn't an MS address or no job is free.
    pub fn stun(&mut self, ms: u32, revive: bool) -> bool {
        if ms == 0 || ms == ADRNULL || is_gateway(ms) || is_allcall(ms) {
            return false;
        }
        let i = match self.supp.jobs.iter().position(|j| j.used && j.ms == ms) {
            Some(i) => i,
            None => match self.supp.jobs.iter().position(|j| !j.used) {
                Some(i) => i,
                None => return false,
            },
        };
        let left = self.cfg.ahoy_tries.max(1);
        self.supp.jobs[i] = StunJob { used: true, revive, left, ms, next_ms: self.now };
        true
    }

    /// Table 6.19: Service_Options_Mirror 0, Service_Kind_Flag 0 stun / 1 revive, ALS 0, G/I 0, no appended
    /// blocks, Service_Kind 1101b, from STUNI. The authenticated variant (6.4.9.2) is the same C_AHOY; the MS then
    /// answers C_ACVIT with a challenge for the TSCC, which this site doesn't take yet (see stun_answer).
    fn stun_ahoy(j: &StunJob) -> [u8; 10] {
        Ahoy { service_kind_flag: j.revive, service_kind: SK_SUPPL, target: j.ms, source: STUNI, ..Default::default() }
            .encode()
    }

    pub(super) fn supp_timers(&mut self) {
        for i in 0..STUN_JOBS {
            let j = self.supp.jobs[i];
            if !j.used || self.now < j.next_ms {
                continue;
            }
            if j.left == 0 {
                self.stun_done(i, R_MSAWAY); // no answer
                continue;
            }
            self.push(DT_CSBK, Self::stun_ahoy(&j), true, None);
            let q = &mut self.supp.jobs[i];
            q.left -= 1;
            q.next_ms = self.now + self.cfg.ahoy_every_ms as i64;
        }
    }

    /// C_ACKU / C_NACKU to STUNI: true when it was a stun job's (or a stale answer to one).
    pub(super) fn supp_ack(&mut self, a: &Ack) -> bool {
        if a.target != STUNI {
            return false;
        }
        if let Some(i) = self.supp.jobs.iter().position(|j| j.used && j.ms == a.source) {
            self.stun_answer(i, a.reason);
        }
        true
    }

    /// The MS answered: Message_Accepted (it is stunned or revived now), or a refusal (MSNot_Supported).
    /// Hook for 6.4.9.2 / 6.4.10: with authentication the MS answers C_ACVIT instead; the TSCC's C_ACKD
    /// Authentication_Response (from t3auth's algorithm) would go here before the MS acts. Not built: such an MS
    /// doesn't answer C_ACKU, and its job ends with no answer.
    fn stun_answer(&mut self, i: usize, reason: u8) {
        self.stun_done(i, reason);
    }

    fn stun_done(&mut self, i: usize, reason: u8) {
        let j = self.supp.jobs[i];
        self.supp.jobs[i].used = false;
        let mut reason = reason;
        if reason_type(reason) == 1 {
            let at = self.supp.stunned.iter().position(|&m| m == j.ms);
            match (j.revive, at) {
                (false, None) => match self.supp.stunned.iter().position(|&m| m == 0) {
                    Some(k) => self.supp.stunned[k] = j.ms,
                    None => {
                        self.log(format_args!("stun: no room to keep {} stunned", j.ms));
                        reason = R_SYSBUSY;
                    }
                },
                (true, Some(k)) => self.supp.stunned[k] = 0,
                _ => {}
            }
        }
        let op = if j.revive { SUPP_REVIVE } else { SUPP_STUN };
        self.log(format_args!("{} {}: reason 0x{:02x}", if j.revive { "revive" } else { "stun" }, j.ms, reason));
        self.activity(ACT_SUPP, op, j.ms, STUNI, reason);
    }

    // ── what a request meets first ──

    /// Stun, ALS and diversion checks of a C_RAND for a service (not registration): true when it was answered
    /// here (refused, diverted, or a diversion request).
    pub(super) fn supp_gate(&mut self, r: &Rand) -> bool {
        let ms = r.source;
        // 6.4.9: a stunned MS may not request user services; cancelling one is harmless
        if self.stunned(ms) && r.service_kind != SK_CANCEL {
            self.refuse(R_PERM_USER_REFUSED, ms, r.target);
            return true;
        }
        let individual = match r.service_kind {
            SK_IND_VOICE | SK_IND_PD | SK_IND_UDT | SK_UDT_POLL => true,
            SK_STATUS => !rand_status_group(r),
            _ => false,
        };
        // nor receive them: it wouldn't answer the availability check
        if individual && self.stunned(r.target) {
            self.refuse(R_MSAWAY, ms, r.target);
            return true;
        }
        if r.service_kind == SK_DIVERT {
            self.div_rand(r);
            return true;
        }
        if r.service_kind == SK_IND_VOICE && r.dep & DEP_ALS != 0 && self.cfg.als == 0 {
            self.refuse(R_NOT_SUPPORTED, ms, r.target); // 6.6.2.6: ALS isn't offered here
            return true;
        }
        let kind = match r.service_kind {
            SK_IND_VOICE if r.dep & DEP_ALS == 0 => 0,
            SK_IND_PD => 1,
            SK_IND_UDT => 2,
            SK_STATUS if individual => 3,
            SK_UDT_POLL => {
                // 6.6.5.1.1: a poll of an MS that diverted its short data fails
                if self.diverted(r.target, 2).is_some() {
                    self.refuse(R_DIV_CAUSE_FAIL, ms, r.target);
                    return true;
                }
                return false;
            }
            _ => return false,
        };
        let Some((to, group)) = self.diverted(r.target, kind) else { return false };
        if to == ms || !self.allowed(ms) {
            return false; // the call goes on as usual (and meets the usual checks)
        }
        self.div_notice(ms, to, group);
        true
    }

    // ── call diversion (6.6.7) ──

    fn div_index(&self, ms: u32) -> Option<usize> {
        self.supp.div.iter().position(|d| d.ms == ms && ms != 0)
    }

    /// Where ms diverted calls of kind j (0 voice, 1 packet data, 2 short data, 3 status): (address, talkgroup).
    fn diverted(&self, ms: u32, j: usize) -> Option<(u32, bool)> {
        let d = &self.supp.div[self.div_index(ms)?];
        (d.to[j] != 0).then_some((d.to[j], d.group & (1 << j) != 0))
    }

    /// The diversions of ms, for tests and the API: the address per kind (voice, packet data, short data,
    /// status; 0 none) and which are talkgroups (bit j).
    pub fn diversions(&self, ms: u32) -> ([u32; 4], u8) {
        self.div_index(ms).map_or(([0; 4], 0), |i| (self.supp.div[i].to, self.supp.div[i].group))
    }

    /// C_RAND Service_Kind 1000b (tables 6.61, 6.64).
    fn div_rand(&mut self, r: &Rand) {
        let ms = r.source;
        if self.cfg.divert == 0 {
            self.refuse(R_NOT_SUPPORTED, ms, r.target); // table 6.60 note: to the initial random access
            return;
        }
        if !self.allowed(ms) {
            self.refuse(R_PERM_USER_REFUSED, ms, r.target);
            return;
        }
        if self.reg_index(ms).is_none() && self.reg_bit() {
            self.refuse(R_MS_NOT_REGISTERED, ms, r.target);
            return;
        }
        let kinds = r.service_options & DIV_KINDS;
        if r.service_options & DIV_ON == 0 {
            // single part: clear its own (TSI), or the ones that point at it (DIVERTI); C_ACKD from the target
            match r.target {
                TSI | DIVERTI => {
                    self.div_clear(ms, kinds, r.target == DIVERTI);
                    self.ack(R_MESSAGE_ACCEPTED, ms, r.target);
                    self.activity(ACT_SUPP, SUPP_DIVERT_CLEAR, ms, r.target, R_MESSAGE_ACCEPTED);
                }
                _ => self.refuse(R_NOT_SUPPORTED, ms, r.target),
            }
            return;
        }
        // multi-part: the address comes by UDT. Only to an MS or a talkgroup here (no PSTN, PABX, line,
        // dispatcher or IP gateway): the others are refused at once with the destination reason (table 6.60).
        if !matches!(r.target, MSI | GPI) {
            self.refuse(R_TRANSIENT_SYS_REFUSED, ms, r.target);
            return;
        }
        if !self.sd_again(SD_DIVERT, ms) {
            self.sd_upload_new(SD_DIVERT, ms, r.target, false, r.service_options, (r.dep & 3) + 1);
        }
    }

    /// The uploaded diversion address (table 6.63: the Address Format, one block): kept for the kinds asked;
    /// the final C_ACK reason (table 6.60).
    pub(super) fn div_set(&mut self, ms: u32, gw: u32, so: u8, format: u8, blocks: &Blocks) -> u8 {
        let mut buf = [0u8; MAX_DATA];
        let n = blocks.data(&mut buf);
        let to = AddrList::of(&buf[..n]).a[0];
        let group = gw == GPI;
        let ok = format == FMT_ADDRESS
            && to != ms
            && if group {
                self.tgl_allowed(to)
            } else {
                to != 0 && to != ADRNULL && !is_gateway(to) && !is_allcall(to)
            };
        if !ok {
            return R_TRANSIENT_SYS_REFUSED;
        }
        let i = match self.div_index(ms).or_else(|| self.supp.div.iter().position(|d| d.ms == 0)) {
            Some(i) => i,
            None => {
                self.log(format_args!("divert: no room for {}", ms));
                return R_TRANSIENT_SYS_REFUSED;
            }
        };
        let d = &mut self.supp.div[i];
        d.ms = ms;
        for j in 0..4 {
            if so & kind_bit(j) != 0 {
                d.to[j] = to;
                if group {
                    d.group |= 1 << j;
                } else {
                    d.group &= !(1 << j);
                }
            }
        }
        if d.to == [0; 4] {
            *d = Div::default();
        }
        self.activity(ACT_SUPP, SUPP_DIVERT, ms, to, R_MESSAGE_ACCEPTED);
        R_MESSAGE_ACCEPTED
    }

    /// Clears ms's diversions of the kinds (DIV_*), or (incoming) the other MSs' diversions to ms.
    fn div_clear(&mut self, ms: u32, kinds: u8, incoming: bool) {
        for d in self.supp.div.iter_mut().filter(|d| d.ms != 0) {
            if !incoming && d.ms != ms {
                continue;
            }
            for j in 0..4 {
                let hit = if incoming { d.to[j] == ms && d.group & (1 << j) == 0 } else { true };
                if kinds & kind_bit(j) != 0 && hit {
                    d.to[j] = 0;
                    d.group &= !(1 << j);
                }
            }
            if d.to == [0; 4] {
                *d = Div::default();
            }
        }
    }

    /// 6.6.7.2: the caller is told where the called MS diverted the service: a UDT header (SF 1, A 0, source MSI
    /// or GPI per table 6.65) and the address. The request ends there.
    fn div_notice(&mut self, caller: u32, to: u32, group: bool) {
        let Some(b) = AddrList::blocks(true, &[to]) else { return };
        if !self.sd_notice(caller, if group { GPI } else { MSI }, &b) {
            self.refuse(R_SYSBUSY, caller, to);
        }
    }
}
