// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! The site's own login to a BrandMeister master, or any master that speaks the Homebrew repeater protocol
//! (tier3.md 5.11). A child of the unit (t3unit.rs): it sees the unit's state. Sans-IO: DMRD packets in
//! (rdof_t3unit_bm_dmrd) and out (the io's dmrd hook), HBlink's 55-byte form both ways (rdofradio converts the
//! dialect at the socket).
//!
//! The login is one repeater (two slots) for the whole site:
//! - Up: a call on a payload channel that goes beyond the site (wide_tg, or a private call to an ID not here) goes
//!   up on a slot of the login: the one its talkgroup is on at the master, else a free one. Two at a time; a third
//!   waits for a slot (not sent).
//! - Down: a group call for a talkgroup in wide_tg that the site carries (its radios' subscriptions, static_tg) is
//!   granted as a network call; a private call to a radio registered here as an implicit setup. A call that comes
//!   on the MQTT network too (the same source and destination) plays once: whichever came first.
//! - Private calls: a setup for an ID not at the site is answered (accepted) for the master, at once with no MQTT
//!   network; with one, after SETUP_WAIT_MS unless a site there answered first (its not-found doesn't count).
//! - Talkgroups (activate): the master carries a talkgroup to a login dynamically, once it is keyed there. So when a
//!   radio registers to a talkgroup in wide_tg that isn't on the login yet, the site keys it: voice headers and a
//!   terminator from the login's owner, no voice. A stream down with no voice before its end (another site's
//!   key-up) isn't played. When no radio wants a talkgroup any more and
//!   none called on it for UNLINK_AFTER_MS, the site keys TG 4000 on its slot (it drops every dynamic talkgroup on
//!   that slot: Homebrew has no way to drop one) and keys the slot's others again. renew_s keys a wanted talkgroup
//!   again when nothing was keyed on it for that long (the master's dynamic timer).

use super::*;
use crate::hblink::{rdof_dmrd_packet, rdof_hb_sink_init, rdof_hb_sink_on_frame, rdof_parse_dmrd, RdofHbSink};
use crate::layer2::{rdof_group_lc, rdof_lc_burst, RDOF_BS_VOICE, RDOF_DATA_XOR};
use crate::mmdvm::rdof_control_to_dt;

pub const BM_TGS: usize = 24;
pub const BM_PINS: usize = 16;
/// A slot with nothing up or down for this long is free.
const SLOT_QUIET_MS: i64 = 1000;
/// With an MQTT network too: how long a private call's setup waits for a site there before the master is called.
const SETUP_WAIT_MS: i64 = 1500;
/// The master's own dynamic timer (BrandMeister's default): a talkgroup heard down within it is on the login.
const DYNAMIC_MS: i64 = 900_000;
/// One DMRD of a key-up this often.
const KEY_GAP_MS: i64 = 60;
const UNLINK_TG: u32 = 4000;
/// The repeater's own talkgroup: the master keeps it on the login (a radio's announcement goes nowhere else).
const LOCAL_TG: u32 = 9;
const ANNOUNCED: usize = 32;
const ANNOUNCE_Q: usize = 8;
const WAITS: usize = 4;
const HEADS: usize = 3;
const KEY_BURSTS: usize = 3; // 2 headers, the terminator

#[derive(Clone, Copy, Default)]
pub struct BmTg {
    tg: u32,
    slot: u8,      // 1, 2: where the master carries it to the login; 0 unknown
    keyed_ms: i64, // the last stream up on it (a key-up or a call); 0 never
    call_ms: i64,  // the last call up on it from a radio here
    heard_ms: i64, // the last stream down for it
}

#[derive(Clone, Copy, Default)]
struct UpSlot {
    ch: i8, // the payload channel whose call is going up on it; -1 none
    last_ms: i64,
}

#[derive(Clone, Copy, Default)]
struct DownSlot {
    stream: u32,
    last_ms: i64,
}

#[derive(Clone, Copy, Default)]
struct Wait {
    used: bool,
    stream: u32, // the request's
    src: u32,
    dst: u32,
    svc: u8,
    due_ms: i64,
}

/// A key-up under way: its bursts, one each KEY_GAP_MS.
#[derive(Clone, Copy)]
struct KeyUp {
    on: bool,
    slot: u8,
    src: u32,
    tg: u32,
    stream: u32,
    next: usize,
    next_ms: i64,
    dt: [c_int; KEY_BURSTS],
    n: [c_int; KEY_BURSTS],
    bursts: [[u8; 33]; KEY_BURSTS],
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct BmCounts {
    pub calls_up: u32,
    pub calls_down: u32,
    pub no_slot: u32,
    pub duplicates: u32,
    pub not_carried: u32,
    pub keyups_heard: u32,
    pub data_dropped: u32,
    pub setups_answered: u32,
    pub activations: u32,
    pub unlinks: u32,
}

pub struct Bm {
    pub on: bool,
    pub up: bool,
    pub mqtt: bool, // the site has an MQTT network too
    pub activate: bool,
    pub renew_ms: i64,
    pub announce: bool, // a radio that registers here is keyed on TG 9 so the master routes its private calls here
    announced: [(u32, i64); ANNOUNCED],
    q: [u32; ANNOUNCE_Q],
    nq: usize,
    pub unlink_ms: i64, // a talkgroup no radio wants any more is dropped this long after the last call on it here
    pub peer_id: u32,
    pub owner: u32, // the key-ups' source: the login's owner (a hotspot's ID without its two digits)
    sink: RdofHbSink,
    ups: [UpSlot; 2],
    downs: [DownSlot; 2],
    pins: [(u32, u8); BM_PINS], // (talkgroup, slot) from the settings
    twins: [u32; 4],            // streams dropped as another network's copy of a call
    heads: [[RdofNetBurst; HEADS]; 2], // per slot: a new stream's headers, held until its voice comes
    nheads: [usize; 2],
    ntwin: usize,
    ch_slot: [u8; CHANNELS], // the slot each channel's call goes up on; 0 none
    pub tgs: [BmTg; BM_TGS],
    waits: [Wait; WAITS],
    key: KeyUp,
    next_check_ms: i64,
    pub n: BmCounts,
}

unsafe fn bm(u: *mut Unit) -> *mut Bm {
    addr_of_mut!((*u).bm)
}

unsafe extern "C" fn sink_send(ctx: *mut c_void, d: *const u8, n: usize) {
    let u: *mut Unit = ctx.cast();
    if let Some(f) = (*u).io.dmrd {
        f((*u).io.ctx, d, n);
    }
}

unsafe extern "C" fn sink_rand(ctx: *mut c_void) -> u32 {
    rnd(ctx.cast())
}

pub unsafe fn bm_init(u: *mut Unit, peer_id: u32, mqtt: bool, activate: bool, renew_s: c_int, unlink_s: c_int) {
    let b = bm(u);
    ptr::write_bytes(b, 0, 1);
    (*b).on = true;
    (*b).mqtt = mqtt;
    (*b).activate = activate;
    (*b).renew_ms = i64::from(renew_s.max(0)) * 1000;
    (*b).unlink_ms = i64::from(unlink_s.max(0)) * 1000;
    (*b).peer_id = peer_id;
    (*b).owner = if peer_id > 0xFF_FFFF { peer_id / 100 } else { peer_id };
    rdof_hb_sink_init(addr_of_mut!((*b).sink), peer_id);
    (*b).sink.send = Some(sink_send);
    (*b).sink.rand32 = Some(sink_rand);
    (*b).sink.ctx = u.cast();
    (*b).ups = [UpSlot { ch: -1, last_ms: 0 }; 2];
}

fn busy(b: &Bm, s: usize, now: i64) -> bool {
    (b.ups[s].ch >= 0 && now - b.ups[s].last_ms < SLOT_QUIET_MS)
        || (b.downs[s].stream != 0 && now - b.downs[s].last_ms < SLOT_QUIET_MS)
        || (b.key.on && usize::from(b.key.slot) == s + 1)
}

fn tg_find(b: &Bm, tg: u32) -> Option<usize> {
    b.tgs.iter().position(|t| t.tg == tg && tg != 0)
}

/// Its entry, made when there is none (the oldest unwanted one goes for it).
fn tg_entry(b: &mut Bm, tg: u32) -> usize {
    if let Some(i) = tg_find(b, tg) {
        return i;
    }
    let i = b.tgs.iter().position(|t| t.tg == 0).unwrap_or_else(|| {
        let mut k = 0;
        for (i, t) in b.tgs.iter().enumerate() {
            if t.keyed_ms.max(t.heard_ms) < b.tgs[k].keyed_ms.max(b.tgs[k].heard_ms) {
                k = i;
            }
        }
        k
    });
    b.tgs[i] = BmTg { tg, ..Default::default() };
    i
}

/// A talkgroup's slot at the master: pinned in the settings, else the one it was last on (keyed or heard). 0: none.
fn tg_slot(b: &Bm, tg: u32) -> u8 {
    if let Some(p) = b.pins.iter().find(|p| p.0 == tg && tg != 0) {
        return p.1;
    }
    tg_find(b, tg).map_or(0, |i| b.tgs[i].slot)
}

/// The slot for something new up on tg. A talkgroup keeps its slot (the master carries it there, and a talkgroup on
/// both would come down twice): when that one is busy, nothing (0). A new one, or a private call: the free slot with
/// fewer talkgroups.
fn slot_for(b: &Bm, tg: Option<u32>, now: i64) -> u8 {
    let own = tg.map_or(0, |t| tg_slot(b, t));
    if own != 0 {
        return if busy(b, usize::from(own - 1), now) { 0 } else { own };
    }
    let load = |s: u8| {
        b.tgs.iter().filter(|t| t.tg != 0 && t.slot == s).count() + b.pins.iter().filter(|p| p.1 == s).count()
    };
    let mut best = 0u8;
    for s in 1..=2u8 {
        if !busy(b, usize::from(s - 1), now) && (best == 0 || load(s) < load(best)) {
            best = s;
        }
    }
    best
}

// ── up: the site's calls ──────────────────────────────────────────────────────

/// A frame of a call on payload channel ch that goes beyond the site (io_uplink).
pub unsafe fn bm_frame(u: *mut Unit, ch: usize, f: &RdofFrame) {
    let b = &mut *bm(u);
    if !b.on || !b.up || ch >= CHANNELS {
        return;
    }
    let now = (*u).now_ms;
    let mut s = b.ch_slot[ch];
    let fresh = f.new_stream || s == 0;
    if fresh {
        if s != 0 && b.ups[usize::from(s - 1)].ch == ch as i8 {
            b.ups[usize::from(s - 1)].ch = -1;
        }
        if !f.has_addr {
            return; // voice before its LC: the sink would hold it on a slot not chosen yet
        }
        if denied(u, NET_BM, f.group, f.dst) {
            return; // the site's rules keep it off the master
        }
        s = slot_for(b, if f.group { Some(f.dst) } else { None }, now);
        b.ch_slot[ch] = s;
        if s == 0 {
            b.n.no_slot += 1;
            log(u, |o| {
                o.bytes(b"tier3: brandmeister: ");
                o.int(i64::from(f.src), 0);
                o.bytes(if f.group { b" -> TG " } else { b" -> " });
                o.int(i64::from(f.dst), 0);
                o.bytes(b": both slots busy, not sent");
            });
            return;
        }
        b.n.calls_up += 1;
        b.ups[usize::from(s - 1)] = UpSlot { ch: ch as i8, last_ms: now };
        if f.group {
            let i = tg_entry(b, f.dst);
            b.tgs[i].slot = s;
            b.tgs[i].keyed_ms = now;
            b.tgs[i].call_ms = now;
        }
    }
    let k = usize::from(s - 1);
    if b.ups[k].ch != ch as i8 {
        return; // the slot went to another channel's call
    }
    b.ups[k].last_ms = now;
    if f.group {
        if let Some(i) = tg_find(b, f.dst) {
            b.tgs[i].keyed_ms = now;
            b.tgs[i].call_ms = now;
        }
    }
    let mut g = *f;
    g.slot = c_int::from(s);
    g.new_stream = fresh;
    rdof_hb_sink_on_frame(addr_of_mut!((*bm(u)).sink), &g);
    if f.dt == crate::ffi::RDOF_DT_TERMINATOR_WITH_LC {
        let b = &mut *bm(u);
        b.ups[k].ch = -1;
        b.ch_slot[ch] = 0;
    }
}

/// The channel closed: whatever it had up is over.
pub unsafe fn bm_ch_closed(u: *mut Unit, ch: usize) {
    let b = &mut *bm(u);
    if ch < CHANNELS && b.ch_slot[ch] != 0 {
        let k = usize::from(b.ch_slot[ch] - 1);
        if b.ups[k].ch == ch as i8 {
            b.ups[k].ch = -1;
        }
        b.ch_slot[ch] = 0;
    }
}

// ── down: the master's calls ──────────────────────────────────────────────────

/// Whether the site carries talkgroup tg from the network: in wide_tg, and wanted (its radios', static_tg).
pub unsafe fn bm_wants(u: *mut Unit, tg: u32) -> bool {
    if !tg_in(&(*u).pol.wide_tg, tg) || denied(u, NET_BM, true, tg) {
        return false;
    }
    if tg_in(&(*u).pol.static_tg, tg) {
        return true;
    }
    let mut t = [0u32; TGS];
    let (n, all) = want_tgs(u, &mut t);
    all || t[..n].contains(&tg)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3unit_bm_dmrd(u: *mut Unit, d: *const u8, n: usize) {
    let b = &mut *bm(u);
    if !b.on || !b.up {
        return;
    }
    let mut x: RdofNetBurst = core::mem::zeroed();
    if !rdof_parse_dmrd(d, n, &mut x) || !(1..=2).contains(&x.slot) {
        return;
    }
    let now = (*u).now_ms;
    let k = (x.slot - 1) as usize;
    let id = u32::from_be_bytes(x.stream_id);
    let new = b.downs[k].stream != id;
    b.downs[k] = DownSlot { stream: if x.terminator { 0 } else { id }, last_ms: now };
    if x.group {
        let i = tg_entry(b, x.dst);
        b.tgs[i].slot = x.slot as u8;
        b.tgs[i].heard_ms = now;
        if !bm_wants(u, x.dst) {
            if new {
                (*bm(u)).n.not_carried += 1;
            }
            return;
        }
    } else if denied(u, NET_BM, false, x.src) {
        if new {
            (*bm(u)).n.not_carried += 1;
        }
        return;
    }
    let b = &mut *bm(u);
    if new {
        b.nheads[k] = 0;
    }
    if stream_find(u, id).is_none() && !bm_is_twin(u, id) {
        // a new stream starts with its voice: the headers wait for it, and one that ends first is a key-up
        let (mut dt, mut vn) = (0, 0);
        rdof_control_to_dt(x.control, &mut dt, &mut vn);
        if dt == crate::ffi::RDOF_DT_VOICE_LC_HEADER || dt == crate::ffi::RDOF_DT_PI_HEADER {
            if b.nheads[k] < HEADS {
                b.heads[k][b.nheads[k]] = x;
                b.nheads[k] += 1;
            }
            return;
        }
        if (crate::ffi::RDOF_DT_CSBK..=crate::ffi::RDOF_DT_RATE_1).contains(&dt) {
            if new {
                b.n.data_dropped += 1; // packet data from the master: not carried (yet)
            }
            return;
        }
        if x.terminator {
            if b.nheads[k] != 0 {
                b.n.keyups_heard += 1;
            }
            b.nheads[k] = 0;
            return;
        }
        b.n.calls_down += 1;
        let held = b.heads[k];
        let nh = core::mem::take(&mut b.nheads[k]);
        for h in &held[..nh] {
            net_burst_in(u, h, 0, true);
        }
    }
    net_burst_in(u, &x, 0, true);
}

// ── private calls: the setup answered for the master ──────────────────────────

/// A setup the controller sent (EV_NET_SETUP): a request for an ID not here is the master's to answer.
pub unsafe fn bm_setup_out(u: *mut Unit, e: &Event) {
    let b = &mut *bm(u);
    if !b.on || !b.up {
        return;
    }
    if e.op == t3::SETUP_CANCEL {
        for w in b.waits.iter_mut() {
            if w.used && w.stream == e.r#ref {
                w.used = false;
            }
        }
        return;
    }
    if e.op != t3::SETUP_REQUEST || e.flags & (t3::SETUP_F_DATA | t3::SETUP_F_ALS) != 0 {
        return; // packet data and ambient listening stay on the MQTT network
    }
    if denied(u, NET_BM, false, e.dst) {
        return; // the site's rules keep calls to it off the master
    }
    let due = (*u).now_ms + if b.mqtt { SETUP_WAIT_MS } else { 0 };
    let w = Wait { used: true, stream: e.stream, src: e.src, dst: e.dst, svc: e.svc, due_ms: due };
    match b.waits.iter().position(|w| !w.used) {
        Some(i) => b.waits[i] = w,
        None => b.waits[0] = w,
    }
}

/// A setup answer from the MQTT network for a request the master may answer: true to drop it (a not-found: the
/// master is asked instead); any other answer is the network's, and the master's wait ends.
pub unsafe fn bm_setup_in(u: *mut Unit, op: u8, reason: u8, r#ref: u32) -> bool {
    let b = &mut *bm(u);
    let Some(i) = b.waits.iter().position(|w| w.used && w.stream == r#ref) else { return false };
    if op == t3::SETUP_REJECT && reason == t3::WHY_NOT_FOUND {
        b.waits[i].due_ms = b.waits[i].due_ms.min((*u).now_ms);
        return true;
    }
    b.waits[i].used = false;
    false
}

unsafe fn waits_due(u: *mut Unit) {
    for i in 0..WAITS {
        let b = &mut *bm(u);
        let w = b.waits[i];
        if !w.used || (*u).now_ms < w.due_ms {
            continue;
        }
        b.waits[i].used = false;
        if !b.up {
            continue;
        }
        b.n.setups_answered += 1;
        log(u, |o| {
            o.bytes(b"tier3: brandmeister: ");
            o.int(i64::from(w.src), 0);
            o.bytes(b" -> ");
            o.int(i64::from(w.dst), 0);
            o.bytes(b": not at a site here, called through the master");
        });
        let m = Setup {
            op: t3::SETUP_ACCEPT,
            reason: t3::WHY_OK,
            flags: 0,
            svc: w.svc,
            src: w.src,
            dst: w.dst,
            stream: rnd(u) | 1,
            r#ref: w.stream,
        };
        t3::rdof_t3_net_setup((*u).site, &m, (*u).now_ms);
        drain(u);
    }
}

// ── talkgroups: keyed so the master carries them ──────────────────────────────

/// A key-up of tg on slot: two voice headers and the terminator, no voice: the master takes the call's start, and a
/// radio that hears it has nothing to unmute for (this site's own don't hear it either: rdof_t3unit_bm_dmrd).
unsafe fn key_up(u: *mut Unit, src: u32, tg: u32, slot: u8) {
    let mut lc = [0u8; 9];
    rdof_group_lc(src, tg, true, 0, &mut lc);
    let cc = c_int::from((*u).cfg.cc);
    let data_sync = RDOF_BS_VOICE ^ RDOF_DATA_XOR;
    let mut k = KeyUp {
        on: true,
        slot,
        src,
        tg,
        stream: rnd(u) | 1,
        next: 0,
        next_ms: (*u).now_ms,
        dt: [0; KEY_BURSTS],
        n: [0; KEY_BURSTS],
        bursts: [[0; 33]; KEY_BURSTS],
    };
    for h in 0..2 {
        rdof_lc_burst(&lc, crate::ffi::RDOF_DT_VOICE_LC_HEADER, cc, data_sync, &mut k.bursts[h]);
        k.dt[h] = crate::ffi::RDOF_DT_VOICE_LC_HEADER;
    }
    rdof_lc_burst(&lc, crate::ffi::RDOF_DT_TERMINATOR_WITH_LC, cc, data_sync, &mut k.bursts[2]);
    k.dt[2] = crate::ffi::RDOF_DT_TERMINATOR_WITH_LC;
    (*bm(u)).key = k;
    log(u, |o| {
        if tg == LOCAL_TG {
            o.bytes(b"tier3: brandmeister: ");
            o.int(i64::from(src), 0);
            o.bytes(b" registered here: announced on slot ");
            o.int(i64::from(slot), 0);
            return;
        }
        o.bytes(if tg == UNLINK_TG { b"tier3: brandmeister: TG 4000 on slot " } else { b"tier3: brandmeister: keying TG " });
        if tg == UNLINK_TG {
            o.int(i64::from(slot), 0);
            o.bytes(b" (drops its dynamic talkgroups)");
        } else {
            o.int(i64::from(tg), 0);
            o.bytes(b" on slot ");
            o.int(i64::from(slot), 0);
            o.bytes(b" for the radios registered to it");
        }
    });
}

unsafe fn key_pump(u: *mut Unit) {
    let b = &mut *bm(u);
    while b.key.on && (*u).now_ms >= b.key.next_ms {
        let k = &mut b.key;
        let i = k.next;
        let mut pkt = [0u8; crate::hblink::RDOF_DMRD_LEN];
        rdof_dmrd_packet(
            i as c_int,
            k.src,
            k.tg,
            b.peer_id,
            c_int::from(k.slot),
            true,
            k.dt[i],
            k.n[i],
            k.stream,
            &k.bursts[i],
            0,
            0,
            &mut pkt,
        );
        k.next += 1;
        k.next_ms += KEY_GAP_MS;
        if k.next == KEY_BURSTS {
            k.on = false;
        }
        sink_send(u.cast(), pkt.as_ptr(), pkt.len());
    }
}

/// The talkgroups the site's radios want from the network: their subscriptions and static_tg, in wide_tg.
unsafe fn wanted(u: *mut Unit, out: &mut [u32; TGS]) -> usize {
    let mut t = [0u32; TGS];
    let (m, _) = want_tgs(u, &mut t);
    let mut n = 0;
    for &g in &t[..m] {
        if g != UNLINK_TG && tg_in(&(*u).pol.wide_tg, g) && !denied(u, NET_BM, true, g) {
            out[n] = g;
            n += 1;
        }
    }
    n
}

unsafe fn tg_tick(u: *mut Unit) {
    let now = (*u).now_ms;
    let b = &mut *bm(u);
    if b.key.on || now < b.next_check_ms {
        return;
    }
    if b.nq > 0 {
        let s = slot_for(b, None, now);
        if s != 0 {
            let id = b.q[0];
            b.q.copy_within(1.., 0);
            b.nq -= 1;
            key_up(u, id, LOCAL_TG, s);
            return;
        }
    }
    if !b.activate {
        return;
    }
    b.next_check_ms = now + 500;
    let mut want = [0u32; TGS];
    let nw = wanted(u, &mut want);
    let want = &want[..nw];
    let b = &mut *bm(u);
    // a talkgroup the site keyed that no radio wants: drop its slot's, unless it was called on lately
    for i in 0..BM_TGS {
        let t = b.tgs[i];
        if t.tg == 0 || t.keyed_ms == 0 || t.slot == 0 || want.contains(&t.tg) || now - t.call_ms < b.unlink_ms
        {
            continue;
        }
        if busy(b, usize::from(t.slot - 1), now) {
            continue;
        }
        let s = t.slot;
        for x in b.tgs.iter_mut() {
            if x.slot == s {
                x.keyed_ms = 0;
                x.heard_ms = 0;
                x.slot = 0;
            }
        }
        b.tgs[i] = BmTg::default();
        b.n.unlinks += 1;
        key_up(u, b.owner, UNLINK_TG, s);
        return;
    }
    // a wanted talkgroup not on the login (never keyed, or past renew_s with nothing heard)
    for &g in want {
        let i = tg_find(b, g);
        let t = i.map(|i| b.tgs[i]).unwrap_or_default();
        // the master's timer runs from the last call keyed here, not from what it sends
        let live = t.keyed_ms != 0 && (b.renew_ms == 0 || now - t.keyed_ms < b.renew_ms.min(DYNAMIC_MS));
        if live {
            continue;
        }
        let s = slot_for(b, Some(g), now);
        if s == 0 {
            continue; // its slot is busy: next time
        }
        let i = tg_entry(b, g);
        b.tgs[i].slot = s;
        b.tgs[i].keyed_ms = now;
        if b.tgs[i].call_ms == 0 {
            b.tgs[i].call_ms = now; // the unlink's hold counts from here
        }
        b.n.activations += 1;
        key_up(u, (*bm(u)).owner, g, s);
        return;
    }
}

/// A radio's registration record was written (del: it left). Its first, and one each renew_s, is announced to the
/// master: a key-up from it on TG 9, so its private calls come here.
pub unsafe fn bm_record(u: *mut Unit, id: u32, del: bool) {
    let b = &mut *bm(u);
    if !b.on || !b.announce {
        return;
    }
    let now = (*u).now_ms;
    let i = b.announced.iter().position(|a| a.0 == id);
    if del {
        if let Some(i) = i {
            b.announced[i] = (0, 0);
        }
        return;
    }
    let every = if b.renew_ms > 0 { b.renew_ms } else { DYNAMIC_MS };
    if i.is_some_and(|i| now - b.announced[i].1 < every) || b.q[..b.nq].contains(&id) || b.nq == ANNOUNCE_Q {
        return;
    }
    let k = i.or_else(|| b.announced.iter().position(|a| a.0 == 0)).unwrap_or_else(|| {
        (0..ANNOUNCED).min_by_key(|&k| b.announced[k].1).unwrap_or(0)
    });
    b.announced[k] = (id, now);
    if b.up {
        b.q[b.nq] = id;
        b.nq += 1;
    }
}

/// The login came up or went down.
#[no_mangle]
pub unsafe extern "C" fn rdof_t3unit_bm_link(u: *mut Unit, up: bool) {
    let b = &mut *bm(u);
    if !b.on || b.up == up {
        return;
    }
    // whatever the master had is unknown after a new login: key again what's wanted
    for t in b.tgs.iter_mut() {
        *t = BmTg::default();
    }
    b.up = up;
    b.key.on = false;
    b.downs = [DownSlot::default(); 2];
    b.ups = [UpSlot { ch: -1, last_ms: 0 }; 2];
    b.ch_slot = [0; CHANNELS];
    b.waits = [Wait::default(); WAITS];
    b.announced = [(0, 0); ANNOUNCED];
    b.nq = 0;
    log(u, |o| (o.bytes(b"tier3: brandmeister: "), o.bytes(if up { b"logged in" } else { b"down" })).1);
}

impl Bm {
    /// Whether the site's calls go to the MQTT network too: it has one (or no BrandMeister login).
    pub fn mqtt_up(&self) -> bool {
        !self.on || self.mqtt
    }
}

/// Whether stream id was dropped as a copy of a call already playing.
pub unsafe fn bm_is_twin(u: *const Unit, id: u32) -> bool {
    (*u).bm.twins.contains(&id) && id != 0
}

/// Stream id (b) is dropped: the other network's copy of the call plays.
pub unsafe fn bm_twin(u: *mut Unit, id: u32, x: &RdofNetBurst, from_bm: bool) {
    log(u, |o| {
        o.bytes(b"tier3: brandmeister: ");
        o.int(i64::from(x.src), 0);
        o.bytes(if x.group { b" -> TG " } else { b" -> " });
        o.int(i64::from(x.dst), 0);
        o.bytes(if from_bm { b": playing from the MQTT network" } else { b": playing from brandmeister" });
    });
    let b = &mut *bm(u);
    b.n.duplicates += 1;
    b.twins[b.ntwin % 4] = id;
    b.ntwin += 1;
}

pub unsafe fn bm_tick(u: *mut Unit) {
    if !(*bm(u)).on {
        return;
    }
    waits_due(u);
    if (*bm(u)).up {
        key_pump(u);
        tg_tick(u);
        key_pump(u);
    }
}

/// Talkgroup tg always goes on slot (1, 2) of the login; 0 unpins it. False: no room.
#[no_mangle]
pub unsafe extern "C" fn rdof_t3unit_bm_pin(u: *mut Unit, tg: u32, slot: c_int) -> bool {
    let b = &mut *bm(u);
    if let Some(i) = b.pins.iter().position(|p| p.0 == tg) {
        b.pins[i] = if (1..=2).contains(&slot) { (tg, slot as u8) } else { (0, 0) };
        return true;
    }
    if !(1..=2).contains(&slot) || tg == 0 {
        return slot == 0;
    }
    match b.pins.iter().position(|p| p.0 == 0) {
        Some(i) => {
            b.pins[i] = (tg, slot as u8);
            true
        }
        None => false,
    }
}

/// Ranges network net (0 MQTT, 1 the master) doesn't carry: kind 0 talkgroups, 1 private calls (the ID called up,
/// calling down). Replaces what was set; false: more than fit (16), the rest not kept.
#[no_mangle]
pub unsafe extern "C" fn rdof_t3unit_deny(
    u: *mut Unit,
    net: c_int,
    kind: c_int,
    r: *const crate::plan::RdofIdrange,
    n: usize,
) -> bool {
    if !(0..=1).contains(&net) || !(0..=1).contains(&kind) {
        return false;
    }
    let d = &mut (*u).deny[net as usize][kind as usize];
    d.n = 0;
    let src = if r.is_null() { &[][..] } else { core::slice::from_raw_parts(r, n) };
    for g in src {
        if d.n == d.r.len() {
            return false;
        }
        d.r[d.n] = (g.lo, g.hi);
        d.n += 1;
    }
    true
}

#[no_mangle]
pub unsafe extern "C" fn rdof_t3unit_bm(
    u: *mut Unit,
    peer_id: u32,
    mqtt: bool,
    activate: bool,
    renew_s: c_int,
    unlink_s: c_int,
    announce: bool,
) {
    bm_init(u, peer_id, mqtt, activate, renew_s, unlink_s);
    (*bm(u)).announce = announce;
}

/// {"up", "calls_up", ...,"tgs": [{"tg", "slot", "keyed_s", "heard_s"}]}: the status page's brandmeister part.
#[no_mangle]
pub unsafe extern "C" fn rdof_t3unit_bm_json(u: *const Unit, out: *mut c_char, cap: usize) -> usize {
    let b = &(*u).bm;
    let now = (*u).now_ms;
    c_out(out, cap, |o| {
        o.bytes(b"{\"up\": ");
        o.bytes(if b.up { b"true" } else { b"false" });
        let n = &b.n;
        for (k, v) in [
            (&b"calls_up"[..], n.calls_up),
            (b"calls_down", n.calls_down),
            (b"no_slot", n.no_slot),
            (b"duplicates", n.duplicates),
            (b"not_carried", n.not_carried),
            (b"keyups_heard", n.keyups_heard),
            (b"data_dropped", n.data_dropped),
            (b"setups_answered", n.setups_answered),
            (b"activations", n.activations),
            (b"unlinks", n.unlinks),
        ] {
            o.bytes(b", \"");
            o.bytes(k);
            o.bytes(b"\": ");
            o.int(i64::from(v), 0);
        }
        o.bytes(b", \"tgs\": [");
        let mut first = true;
        for t in b.tgs.iter().filter(|t| t.tg != 0 && t.slot != 0) {
            if !first {
                o.bytes(b", ");
            }
            first = false;
            o.bytes(b"{\"tg\": ");
            o.int(i64::from(t.tg), 0);
            o.bytes(b", \"slot\": ");
            o.int(i64::from(t.slot), 0);
            for (k, at) in [(&b"keyed_s"[..], t.keyed_ms), (b"heard_s", t.heard_ms)] {
                if at != 0 {
                    o.bytes(b", \"");
                    o.bytes(k);
                    o.bytes(b"\": ");
                    o.int((now - at) / 1000, 0);
                }
            }
            o.bytes(b"}");
        }
        o.bytes(b"]}");
    })
}
