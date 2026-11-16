// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! The Status Transport Service's PDUs and its outcome (docs/spec/fw/tier3.md section 3.10): ETSI TS 102 361-4
//! 6.6.6.1, a 7-bit status from an MS to another MS, a talkgroup or the TSCC, with the emergency alarm (status 126)
//! and its cancel (124) of 6.6.6.3. The C_RAND carries the status in Service_Options (G/I, SUPED_SV, the 5 high
//! bits) and the 2 bits before the Service_Kind (table 6.57); the C_AHOY to the called party in
//! Service_Options_Mirror and Appended_Blocks (table 7.22, note 2). The delivery itself (store and forward,
//! polling, the network) is short data's (t3sdata.rs, section 3.15); this module reports each status's outcome.
//! A child module of t3site: it works on the site's own state.

use super::*;

pub const STATUS_CANCEL_ALARM: u8 = 124;
pub const STATUS_EMERGENCY_ALARM: u8 = 126;

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
    /// A status from an MS here delivered or not: the `status` activity, and the alarm in the log.
    pub(super) fn status_done(&mut self, status: u8, group: bool, caller: u32, called: u32, reason: u8) {
        let flags = if group { F_GROUP } else { 0 };
        self.ev(Event { r#type: EV_ACTIVITY, kind: ACT_STATUS, op: status, flags, src: caller, dst: called, reason, ..Default::default() });
        match status {
            STATUS_EMERGENCY_ALARM => self.log(format_args!("emergency alarm from {} to {}", caller, called)),
            STATUS_CANCEL_ALARM => self.log(format_args!("emergency alarm from {} cancelled", caller)),
            _ => {}
        }
    }
}
