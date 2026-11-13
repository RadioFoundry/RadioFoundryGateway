// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! The composite TSCC (docs/spec/fw/tier3.md section 3.19): ETSI TS 102 361-4 5.3.2, "Non-Dedicated TSCC". With
//! the setting on, a call that finds every payload channel busy takes the TSCC's own slot: the TSCC sends the grant
//! (naming its own channel, LCN 0) and then carries the call there. While it does, the CACH on the TSCC's carrier
//! sends P_SYS_Parms with Payload Channel Type 1 ("Payload Channel is Composite Control Channel", 7.1.2.2, table
//! 7.34) in place of C_SYS_Parms (7.1.2.1: "never transmitted when neither of the timeslots of the physical channel
//! are a TSCC"), so idle MSs know the TSCC will come back. The call clears as any other (P_CLEAR on the slot), and
//! the TSCC returns with a C_ALOHA. Random access, registration and short data wait for it (5.3.2 a), b)).
//! A child module of t3site: it works on the site's own state.

use super::*;

/// The TSCC sends a call's grant this many times on its own slot before the slot turns payload.
const REV_GRANTS: u8 = 2;

/// SlotOut.rev: the slot carries the call, both ways (its payload path sends the outbound).
pub const REV_CALL: u8 = 1;
/// SlotOut.rev: the TSCC still sends the call's grant outbound, but the inbound is already the call's: the MS that
/// asked sends its voice header as soon as it reads the first grant (an air test lost it to the TSCC).
pub const REV_GRANT: u8 = 2;

#[derive(Clone, Copy, Default)]
pub(super) struct RevState {
    grants_left: u8,
    grant: [u8; 10],
    pub(super) reverts: u32,
}

impl Site {
    /// The TSCC's own slot as a payload channel, when the setting allows it and the TSCC is free to go: not
    /// mid-way through an MBC or a short data transfer, and not withdrawn.
    pub(super) fn composite_channel(&self) -> Option<usize> {
        let t = self.tscc_carrier * 2;
        let ok = self.cfg.composite != 0
            && self.chans[t].r#use == USE_TSCC
            && !self.withdrawn
            && !self.cont_pending
            && !self.sd_pending();
        ok.then_some(t)
    }

    /// The TSCC's slot carries a call (its grant may still be going out).
    pub(super) fn reverted(&self) -> bool {
        matches!(self.chans[self.tscc_carrier * 2].r#use, USE_CALL | USE_HANG | USE_CLEAR)
    }

    /// The TSCC's slot carries the call now: its grants are out.
    pub fn tscc_payload(&self) -> bool {
        self.reverted() && self.rev.grants_left == 0
    }

    /// open() on the TSCC's channel: its grant goes out on the TSCC's next slots, ahead of anything queued.
    pub(super) fn revert(&mut self, grant: [u8; 10]) {
        self.rev.grant = grant;
        self.rev.grants_left = REV_GRANTS;
        self.rev.reverts += 1;
        self.log(format_args!("composite TSCC: the control slot carries a call (5.3.2)"));
    }

    /// The TSCC's slot k while it carries a call: the grant still to go (the inbound already the call's), else
    /// nothing from the TSCC (the payload path sends the slot). The CACH's AT for the next TS2 slot: TS1's inbound
    /// in use.
    pub(super) fn rev_slot(&mut self, out: &mut SlotOut) {
        if self.rev.grants_left > 0 {
            self.rev.grants_left -= 1;
            out.tscc_dt = DT_CSBK;
            out.tscc_pdu = self.rev.grant;
            out.rev = REV_GRANT;
            return;
        }
        out.rev = REV_CALL;
        out.at = (self.chans[self.tscc_carrier * 2].r#use == USE_CALL) as u8;
    }

    /// close() on the TSCC's channel: the TSCC again. What the TSCC had queued before or during the call is stale
    /// (grants, answers): the MSs ask again. A C_ALOHA first.
    pub(super) fn tscc_back(&mut self) {
        let t = self.tscc_carrier * 2;
        self.chans[t] = Chan { r#use: USE_TSCC, ..Default::default() };
        self.rev.grants_left = 0;
        self.resp_len = 0;
        self.resp_head = 0;
        self.cont_pending = false;
        self.since_aloha = 3;
        self.log(format_args!("composite TSCC: the control channel again"));
    }
}
