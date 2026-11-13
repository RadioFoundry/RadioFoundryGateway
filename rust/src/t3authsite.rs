// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! MS authentication at the site (docs/spec/fw/tier3.md 3.14): ETSI TS 102 361-4 6.4.4.1.5 (figure 6.20) and
//! 6.4.8. On a registration C_RAND from an MS that must be challenged, the TSCC answers with a C_AHOY (Service_Kind
//! 1110b, Service_Kind_Flag 0, Service_Options_Mirror = the C_RAND's Service_Options, source = the challenge; table
//! 6.17) and keeps the next inbound slot for the answer (AT). The MS answers C_ACKU(Authentication Response,
//! 0100 1000b; table 6.18) with the response as its target; a good one continues the registration, a wrong one gets
//! C_NACKD(Reg_Denied). An authentication poll (Service_Options_Mirror 0, 6.4.8.2) challenges a registered MS on a
//! command; a failure removes its registration. The algorithm is t3auth.rs. A child module of t3site: it works on
//! the site's own state.

use super::*;
use crate::t3auth::{response, Rng, R_AUTH_RESPONSE};

/// Cfg.auth: no authentication (the site as without it).
pub const AUTH_OFF: u8 = 0;
/// Cfg.auth: MSs with a key are challenged at registration; MSs without one register as before.
pub const AUTH_REGISTRATION: u8 = 1;
/// Cfg.auth: every MS is challenged at registration; one without a key is refused (Reg_Denied).
pub const AUTH_REQUIRED: u8 = 2;

/// ACT_REG ops: the MS answered a challenge correctly.
pub const REG_AUTH_OK: u8 = 6;
/// ACT_REG ops: a wrong answer, or no key or no support where authentication is required.
pub const REG_AUTH_FAIL: u8 = 7;
/// ACT_REG ops: no answer to any of the challenges, or (not required) no support for authentication.
pub const REG_AUTH_NONE: u8 = 8;

/// Challenges in progress at once.
const CHALLENGES: usize = 8;

#[derive(Clone, Copy, Default)]
struct Challenge {
    used: bool,
    poll: bool, // an authentication poll; else a registration's challenge
    left: u8,   // C_AHOYs left to send
    ms: u32,
    rand: u32,
    expect: u32,
    req: Rand, // the registration's C_RAND
    next_ms: i64,
}

#[derive(Clone, Copy, Default)]
pub(super) struct Auth {
    rng: Rng,
    ch: [Challenge; CHALLENGES],
}

impl Site {
    /// The MS's key and PSN, when it has a key.
    pub(super) fn ms_key(&self, ms: u32) -> Option<([u8; 16], Option<u32>)> {
        let p = self.ms_policy(ms);
        (p.has_key != 0).then_some((p.key, (p.has_psn != 0).then_some(p.psn)))
    }

    /// Whether ms registers only through a challenge (implicit registration by a call request isn't open to it).
    pub(super) fn auth_needed(&self, ms: u32) -> bool {
        match self.cfg.auth {
            AUTH_REGISTRATION => self.ms_policy(ms).has_key != 0,
            AUTH_REQUIRED => true,
            _ => false,
        }
    }

    fn auth_find(&self, ms: u32) -> Option<usize> {
        self.auth.ch.iter().position(|c| c.used && c.ms == ms)
    }

    /// The C_AHOY of challenge i (table 6.17), with AT for the answer.
    fn auth_ahoy(&mut self, i: usize) {
        let c = self.auth.ch[i];
        let a = Ahoy {
            service_options: if c.poll { 0 } else { c.req.service_options & 0x7F }, // 6.4.8.2, table 7.61
            service_kind_flag: false,
            als: false,
            group: false,
            appended: 0,
            service_kind: SK_REG,
            target: c.ms,
            source: c.rand,
        };
        self.push(DT_CSBK, a.encode(), true, None);
    }

    /// Challenge i's next C_AHOY, or, with none left and the last one's wait over, the end of it.
    fn auth_tick(&mut self, i: usize) {
        let c = self.auth.ch[i];
        if c.left == 0 {
            // no answer: the MS's TNP_Timer ends its registration; nothing is sent (6.4.4.1.6)
            self.auth.ch[i].used = false;
            self.activity(ACT_REG, REG_AUTH_NONE, c.ms, 0, 0);
            return;
        }
        self.auth_ahoy(i);
        let every = self.cfg.ahoy_every_ms as i64;
        let c = &mut self.auth.ch[i];
        c.left -= 1;
        c.next_ms = self.now + every;
    }

    /// A new challenge for ms (replacing any it has); false when the table is full.
    fn auth_start(&mut self, ms: u32, k: &[u8; 16], psn: Option<u32>, req: Option<&Rand>) -> bool {
        let Some(i) = self.auth_find(ms).or_else(|| self.auth.ch.iter().position(|c| !c.used)) else {
            self.log(format_args!("auth: {} challenges in progress, {} waits", CHALLENGES, ms));
            return false;
        };
        let rand = self.auth.rng.challenge(self.now);
        self.auth.ch[i] = Challenge {
            used: true,
            poll: req.is_none(),
            left: self.cfg.ahoy_tries.max(1),
            ms,
            rand,
            expect: response(rand, k, psn),
            req: req.copied().unwrap_or_default(),
            next_ms: 0,
        };
        self.auth_tick(i);
        true
    }

    /// A refusal for want of authentication: Reg_Denied (a registration), the record removed, the activity.
    fn auth_fail(&mut self, ms: u32, registering: bool, why: &str) {
        if registering {
            self.ack(R_REG_DENIED, ms, REGI);
        }
        self.log(format_args!("auth: {} failed: {}", ms, why));
        match self.reg_index(ms) {
            Some(i) => self.delete_record(i, REG_AUTH_FAIL),
            None => self.activity(ACT_REG, REG_AUTH_FAIL, ms, 0, R_REG_DENIED),
        }
    }

    /// A registration request that passed the other checks: true when authentication holds it (a challenge sent,
    /// or refused here), false when it registers at once.
    pub(super) fn auth_registration(&mut self, r: &Rand) -> bool {
        if self.cfg.auth == AUTH_OFF {
            return false;
        }
        let ms = r.source;
        let Some((k, psn)) = self.ms_key(ms) else {
            if self.cfg.auth == AUTH_REQUIRED {
                self.auth_fail(ms, true, "no key");
                return true;
            }
            return false;
        };
        if let Some(i) = self.auth_find(ms).filter(|&i| !self.auth.ch[i].poll) {
            // the MS repeats its request (the challenge was lost): the same challenge again
            self.auth.ch[i].req = *r;
            self.auth_ahoy(i);
            return true;
        }
        // a full table: no answer; the MS repeats its request after its random access wait
        self.auth_start(ms, &k, psn, Some(r));
        true
    }

    /// A C_ACKU or C_NACKU from an MS: true when it answers a challenge (and is handled here).
    pub(super) fn auth_answer(&mut self, a: &Ack) -> bool {
        let Some(i) = self.auth_find(a.source) else { return false };
        let c = self.auth.ch[i];
        let nack = reason_type(a.reason) == 0;
        if !(a.reason == R_AUTH_RESPONSE
            || nack
                && (a.target == c.rand || !self.pend.iter().any(|p| p.used && p.state == P_AHOY && p.called == c.ms)))
        {
            return false; // an answer to another C_AHOY (an individual call's)
        }
        self.auth.ch[i].used = false;
        let ms = c.ms;
        if nack {
            // the MS doesn't support authentication (MSNot_Supported) or refuses it
            if self.cfg.auth == AUTH_REQUIRED {
                self.auth_fail(ms, !c.poll, "not supported");
            } else {
                self.activity(ACT_REG, REG_AUTH_NONE, ms, 0, a.reason);
                if !c.poll {
                    self.register(&c.req);
                }
            }
        } else if a.target & 0xFF_FFFF == c.expect {
            self.heard(ms);
            self.activity(ACT_REG, REG_AUTH_OK, ms, 0, 0);
            if !c.poll {
                self.register(&c.req);
            }
        } else {
            self.auth_fail(ms, !c.poll, "wrong response");
        }
        true
    }

    /// An authentication poll of a registered MS with a key (6.4.8.2, Service_Options_Mirror 0). False: not
    /// registered here, no key, or no room for another challenge.
    pub(super) fn auth_poll(&mut self, ms: u32) -> bool {
        if self.reg_index(ms).is_none() {
            return false;
        }
        let Some((k, psn)) = self.ms_key(ms) else { return false };
        if self.auth_find(ms).is_some_and(|i| !self.auth.ch[i].poll) {
            return true; // its registration is being challenged now
        }
        self.auth_start(ms, &k, psn, None)
    }

    /// Each slot: the challenges' repeats and their end.
    pub(super) fn auth_timers(&mut self) {
        for i in 0..CHALLENGES {
            if self.auth.ch[i].used && self.now >= self.auth.ch[i].next_ms {
                self.auth_tick(i);
            }
        }
    }

    /// Something unpredictable for the challenges (the caller's random source).
    pub fn entropy(&mut self, v: u64) {
        self.auth.rng.stir(v);
    }
}

/// # Safety
/// s: an initialized site.
#[no_mangle]
pub unsafe extern "C" fn rdof_t3_entropy(s: *mut Site, v: u64) {
    (*s).entropy(v);
}
