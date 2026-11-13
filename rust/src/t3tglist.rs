// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! Talkgroup lists an MS holds beyond its record (docs/spec/fw/tier3.md 3.11): the subscription/attachment list
//! a registration uploads (ETSI TS 102 361-4 6.4.4.1.13.2) and the talkgroups Dynamic Group Numbering Assignment
//! gave it (6.6.8). A part of the site controller (a child module of t3site); the uploads and the DGNA transfer
//! go through the short data transactions (t3sdata, SD_TGLIST and SD_DGNA); this keeps what they leave.
//!
//! The site carries a talkgroup that a registered MS has in either list (wants_tg, the network subscriptions).
//! A new list replaces the old one (6.4.4.1.13.2); a single subscription (Trgt_Adr_Cnts 01b) replaces it too.
//! A DGNA_Address transfer replaces the fifteen dynamic talkgroups; a DGNA_Alias transfer sets the sixteenth
//! unless its address is one of the fifteen (6.6.8.1).

use super::*;
use crate::t3udt::*;

/// MSs with a list at once (the table is full: a new list is refused, a DGNA result isn't kept).
pub const TGLISTS: usize = 64;
/// Talkgroups in a subscription/attachment list (table 6.10: ADDRESS1 to ADDRESS7).
pub const LIST_TGS: usize = 7;
/// Dynamic talkgroups: fifteen by DGNA_Address, one more by DGNA_Alias (6.6.8.0).
pub const DGNA_TGS: usize = 16;

#[derive(Clone, Copy, Default)]
struct TgList {
    ms: u32, // 0: free
    sub: [u32; LIST_TGS],
    dgna: [u32; DGNA_TGS],
}

impl TgList {
    fn tgs(&self) -> impl Iterator<Item = u32> + '_ {
        self.sub.iter().chain(self.dgna.iter()).copied().filter(|&t| t != 0)
    }
}

/// The lists (all zero: none).
#[derive(Clone, Copy)]
pub(super) struct TglState {
    l: [TgList; TGLISTS],
}

impl Default for TglState {
    fn default() -> TglState {
        TglState { l: [TgList::default(); TGLISTS] }
    }
}

impl Site {
    fn tgl_index(&self, ms: u32) -> Option<usize> {
        self.tgl.l.iter().position(|e| e.ms == ms)
    }

    /// ms's entry, made when there is none (a free one, or one whose MS isn't registered any more).
    fn tgl_entry(&mut self, ms: u32) -> Option<usize> {
        if let Some(i) = self.tgl_index(ms) {
            return Some(i);
        }
        let i = (0..TGLISTS).find(|&i| self.tgl.l[i].ms == 0 || self.reg_index(self.tgl.l[i].ms).is_none())?;
        self.tgl.l[i] = TgList { ms, ..Default::default() };
        Some(i)
    }

    /// A talkgroup an MS may hold here: one the site's policy allows, not a gateway or an all-call.
    pub(super) fn tgl_allowed(&self, tg: u32) -> bool {
        self.policy.tg.contains(tg) && !is_gateway(tg) && !is_allcall(tg)
    }

    // ── registration with a list (6.4.4.1.13.2) ──

    /// C_RAND Registration with Trgt_Adr_Cnts 10b (target TATTSI), the registration accepted: C_AHOY from TATTSI
    /// for the list (table 6.11), Appended_Blocks the C_RAND's Appended_UDT_Short_Data. The answer comes after it.
    pub(super) fn tgl_register(&mut self, r: &Rand) {
        if !self.sd_again(SD_TGLIST, r.source) {
            self.sd_upload_new(SD_TGLIST, r.source, TATTSI, false, r.service_options, (r.dep & 3) + 1);
        }
    }

    /// The uploaded list (B.3.2 Address Format): kept, replacing ms's last; (reason, Response_Info) for the final
    /// C_ACK. One bit per address, ADDRESS1 the MSB of the seven (table 6.10): 1 accepted. ADRNULL and the bits
    /// past the addresses sent are 1 (all accepted is 1111111b whatever the count).
    pub(super) fn tgl_apply(&mut self, ms: u32, list: &AddrList) -> (u8, u8) {
        let Some(r) = self.reg_index(ms) else { return (R_REG_SUBSCRIPTION, 0) };
        // the single subscription goes: the list replaces it
        let rec = &mut self.regs[r].rec;
        let old = core::mem::take(&mut rec.sub_tg);
        if old != 0 {
            rec.remove_tg(old);
        }
        let slot = self.tgl_entry(ms);
        let mut sub = [0u32; LIST_TGS];
        let mut info = 0x7F;
        for (j, &tg) in list.addrs().iter().take(LIST_TGS).enumerate() {
            if tg == ADRNULL {
                continue;
            }
            if slot.is_some() && self.tgl_allowed(tg) {
                sub[j] = tg;
            } else {
                info &= !(0x40 >> j);
            }
        }
        if let Some(i) = slot {
            self.tgl.l[i].sub = sub;
        }
        self.subs_dirty = true;
        self.write_record(r);
        (if info == 0x7F { R_REG_ACCEPTED } else { R_REG_SUBSCRIPTION }, info)
    }

    /// A single subscription (Trgt_Adr_Cnts 01b) or deregistration: the list goes.
    pub(super) fn tgl_clear_list(&mut self, ms: u32) {
        if let Some(i) = self.tgl_index(ms) {
            self.tgl.l[i].sub = [0; LIST_TGS];
            self.subs_dirty = true;
        }
    }

    /// The record of ms is gone: so are its lists.
    pub(super) fn tgl_forget(&mut self, ms: u32) {
        if let Some(i) = self.tgl_index(ms) {
            self.tgl.l[i] = TgList::default();
        }
    }

    // ── DGNA (6.6.8) ──

    /// Why a DGNA transfer can't go (0: it can): the format, or a talkgroup this site doesn't allow.
    pub(super) fn dgna_refusal(&self, format: u8, blocks: &Blocks) -> u8 {
        let mut buf = [0u8; MAX_DATA];
        let n = blocks.data(&mut buf);
        let l = AddrList::of(&buf[..n]);
        let addrs = match format {
            FMT_ADDRESS => l.addrs(),
            FMT_MIXED => &l.a[..1],
            _ => return R_NOT_SUPPORTED,
        };
        if addrs.iter().all(|&tg| tg == ADRNULL || self.tgl_allowed(tg)) {
            0
        } else {
            R_GROUP_NOT_ALLOWED
        }
    }

    /// The called MS took a DGNA transfer (its C_ACKU MS_Accepted): what it now holds (6.6.8.1).
    pub(super) fn tgl_dgna(&mut self, ms: u32, blocks: &Blocks, format: u8) {
        let mut buf = [0u8; MAX_DATA];
        let n = blocks.data(&mut buf);
        let l = AddrList::of(&buf[..n]);
        let Some(i) = self.tgl_entry(ms) else {
            self.log(format_args!("dgna: no room to keep {}'s talkgroups", ms));
            return;
        };
        let e = &mut self.tgl.l[i];
        if format == FMT_ADDRESS {
            e.dgna[..DGNA_TGS - 1].fill(0);
            for (j, &tg) in l.addrs().iter().take(DGNA_TGS - 1).enumerate() {
                e.dgna[j] = tg;
            }
        } else {
            let tg = l.a[0];
            if tg == ADRNULL || !e.dgna[..DGNA_TGS - 1].contains(&tg) {
                e.dgna[DGNA_TGS - 1] = tg;
            }
        }
        self.subs_dirty = true;
    }

    // ── what the site carries ──

    /// A registered MS holds tg in a list.
    pub(super) fn tgl_wants(&self, tg: u32) -> bool {
        tg != 0 && self.tgl.l.iter().any(|e| e.ms != 0 && e.tgs().any(|t| t == tg))
    }

    /// ms's listed talkgroups (subscription list, then DGNA).
    pub(super) fn tgl_of(&self, ms: u32) -> impl Iterator<Item = u32> + '_ {
        self.tgl.l.iter().filter(move |e| e.ms == ms && ms != 0).flat_map(|e| e.tgs())
    }

    /// The talkgroups ms holds in its lists, for tests and the API: (subscription list, dynamic talkgroups).
    pub fn tg_lists(&self, ms: u32) -> ([u32; LIST_TGS], [u32; DGNA_TGS]) {
        match self.tgl_index(ms) {
            Some(i) if ms != 0 => (self.tgl.l[i].sub, self.tgl.l[i].dgna),
            _ => ([0; LIST_TGS], [0; DGNA_TGS]),
        }
    }
}
