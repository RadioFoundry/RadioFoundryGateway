// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! A P25 or NXDN repeater with no network, on a duplex MMDVM modem in that mode (rdof/mrelay.h): each frame the modem
//! hears goes back to it to send, as heard. The input's NAC or RAN, its status symbols and any bit errors pass
//! through; nothing is decoded but the P25 NID's NAC and DUID. Rust only: no C twin.
//!
//! The modem hands a frame up as {tag, frame (sync first) [, RSSI hi, lo]} and takes one to send as {tag, frame}, the
//! tag skipped. A P25 transmission's first frame comes as a header (P25_HDR) at its own length; every frame after it
//! as an LDU (P25_LDU) of 216 bytes, a terminator too, which is cut here to its own length.

use core::ffi::c_int;

use crate::bits::{slice, slice_mut};

const P25_HDR: u8 = 0x30;
const P25_LDU: u8 = 0x31;
const P25_LOST: u8 = 0x32;
const NXDN_DATA: u8 = 0x40;
const NXDN_LOST: u8 = 0x41;
const MODE_P25: c_int = 4;
const MODE_NXDN: c_int = 5;

const P25_LDU_BYTES: usize = 216;
const NXDN_BYTES: usize = 48;
/// sync 6 bytes, then the NAC (12 bits) and DUID (4 bits)
const P25_NID_END: usize = 8;

pub const DUID_HDU: c_int = 0x0;
pub const DUID_TDU: c_int = 0x3;
pub const DUID_LDU1: c_int = 0x5;
pub const DUID_TSDU: c_int = 0x7;
pub const DUID_LDU2: c_int = 0xA;
pub const DUID_PDU: c_int = 0xC;
pub const DUID_TDULC: c_int = 0xF;

/// rdof_mrelay_t
#[repr(C)]
pub struct RdofMrelay {
    pub mode: c_int,
    pub in_call: bool,
    pub frames: i64,
    pub lost: i64,
    pub calls: i64,
}

/// rdof_mrelay_heard_t
#[repr(C)]
#[derive(Default, Debug, PartialEq)]
pub struct RdofMrelayHeard {
    pub duid: c_int,
    pub nac: c_int,
    pub rssi: c_int,
    pub lost: bool,
    pub start: bool,
    pub end: bool,
}

#[no_mangle]
pub extern "C" fn rdof_mrelay_init(r: &mut RdofMrelay, mode: c_int) {
    *r = RdofMrelay { mode, in_call: false, frames: 0, lost: 0, calls: 0 };
}

/// A P25 frame's length by its DUID (a PDU's header block only); None: as heard.
fn p25_len(duid: c_int) -> Option<usize> {
    match duid {
        DUID_HDU => Some(99),
        DUID_TDU => Some(18),
        DUID_LDU1 | DUID_LDU2 => Some(P25_LDU_BYTES),
        DUID_TSDU | DUID_PDU => Some(45),
        DUID_TDULC => Some(54),
        _ => None,
    }
}

fn rssi_at(p: &[u8], at: usize) -> c_int {
    match p.get(at..at + 2) {
        Some(&[hi, lo]) => c_int::from(u16::from_be_bytes([hi, lo])),
        _ => -1,
    }
}

impl RdofMrelay {
    fn lost(&mut self, h: &mut RdofMrelayHeard) {
        h.lost = true;
        h.end = self.in_call;
        self.in_call = false;
        self.lost += 1;
    }

    /// rdof_mrelay_heard on slices: the frame to send back (out[..n]) and what was heard; None: not this mode's frame.
    pub fn heard(&mut self, cmd: u8, p: &[u8], out: &mut [u8]) -> Option<(u8, usize, RdofMrelayHeard)> {
        let mut h = RdofMrelayHeard { duid: -1, nac: -1, rssi: -1, ..Default::default() };
        let n = match (self.mode, cmd) {
            (MODE_P25, P25_LOST) | (MODE_NXDN, NXDN_LOST) => {
                self.lost(&mut h);
                return Some((cmd, 0, h));
            }
            (MODE_P25, P25_HDR | P25_LDU) if p.len() > P25_NID_END => {
                h.nac = (c_int::from(p[1 + 6]) << 4) | c_int::from(p[1 + 7] >> 4);
                h.duid = c_int::from(p[1 + 7] & 0x0F);
                let mut n = p.len() - 1;
                if cmd == P25_LDU {
                    h.rssi = rssi_at(p, 1 + P25_LDU_BYTES);
                    n = n.min(P25_LDU_BYTES);
                }
                p25_len(h.duid).map_or(n, |want| n.min(want))
            }
            (MODE_NXDN, NXDN_DATA) if p.len() > NXDN_BYTES => {
                h.rssi = rssi_at(p, 1 + NXDN_BYTES);
                NXDN_BYTES
            }
            _ => return None,
        };
        if 1 + n > out.len() {
            return None;
        }
        out[0] = 0; // the tag: the modem skips it
        out[1..=n].copy_from_slice(&p[1..=n]);
        self.frames += 1;
        if !self.in_call {
            h.start = true;
            self.in_call = true;
            self.calls += 1;
        }
        if h.duid == DUID_TDU || h.duid == DUID_TDULC {
            h.end = true;
            self.in_call = false;
        }
        Some((cmd, 1 + n, h))
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_mrelay_heard(
    r: &mut RdofMrelay,
    cmd: u8,
    p: *const u8,
    len: usize,
    out_cmd: &mut u8,
    out: *mut u8,
    cap: usize,
    out_len: &mut usize,
    h: &mut RdofMrelayHeard,
) -> bool {
    *out_len = 0;
    *h = RdofMrelayHeard { duid: -1, nac: -1, rssi: -1, ..Default::default() };
    match r.heard(cmd, slice(p, len), slice_mut(out, cap)) {
        Some((c, n, heard)) => {
            *out_cmd = c;
            *out_len = n;
            *h = heard;
            true
        }
        None => false,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn relay(mode: c_int) -> RdofMrelay {
        let mut r = RdofMrelay { mode: 0, in_call: false, frames: 0, lost: 0, calls: 0 };
        rdof_mrelay_init(&mut r, mode);
        r
    }

    /// A P25 frame as the modem hands it up: tag, sync, NID (nac, duid), then filler to len bytes, then RSSI.
    fn p25(nac: u16, duid: u8, frame_len: usize, rssi: Option<u16>) -> std::vec::Vec<u8> {
        let mut v = std::vec![0x01u8, 0x55, 0x75, 0xF5, 0xFF, 0x77, 0xFF, (nac >> 4) as u8, ((nac as u8) << 4) | duid];
        while v.len() < 1 + frame_len {
            v.push(v.len() as u8);
        }
        if let Some(r) = rssi {
            v.extend_from_slice(&r.to_be_bytes());
        }
        v
    }

    #[test]
    fn p25_call_relayed_as_heard() {
        let mut r = relay(MODE_P25);
        let mut out = [0u8; 256];
        let hdu = p25(0x293, 0x0, 99, None);
        let (c, n, h) = r.heard(P25_HDR, &hdu, &mut out).unwrap();
        assert_eq!((c, n), (P25_HDR, 100));
        assert_eq!(out[0], 0);
        assert_eq!(&out[1..100], &hdu[1..100]);
        assert_eq!((h.nac, h.duid, h.start, h.end, h.rssi), (0x293, 0, true, false, -1));
        let ldu = p25(0x293, 0x5, 216, Some(0x1234));
        let (c, n, h) = r.heard(P25_LDU, &ldu, &mut out).unwrap();
        assert_eq!((c, n), (P25_LDU, 217));
        assert_eq!(&out[1..217], &ldu[1..217]);
        assert_eq!((h.duid, h.rssi, h.start), (0x5, 0x1234, false));
        // a terminator in an LDU's length: cut to its own 18 bytes, and the call ends
        let tdu = p25(0x293, 0x3, 216, Some(7));
        let (_, n, h) = r.heard(P25_LDU, &tdu, &mut out).unwrap();
        assert_eq!(n, 19);
        assert!(h.end && !r.in_call);
        assert_eq!((r.frames, r.calls), (3, 1));
    }

    #[test]
    fn p25_loss_ends_a_call_once() {
        let mut r = relay(MODE_P25);
        let mut out = [0u8; 256];
        r.heard(P25_LDU, &p25(1, 0xA, 216, None), &mut out).unwrap();
        let (_, n, h) = r.heard(P25_LOST, &[], &mut out).unwrap();
        assert_eq!(n, 0);
        assert!(h.lost && h.end);
        let (_, _, h) = r.heard(P25_LOST, &[], &mut out).unwrap();
        assert!(h.lost && !h.end);
        assert_eq!(r.lost, 2);
    }

    #[test]
    fn nxdn_frames_and_wrong_mode() {
        let mut r = relay(MODE_NXDN);
        let mut out = [0u8; 256];
        let mut f = std::vec![0x00u8];
        f.extend((0..48).map(|i| i as u8 ^ 0x5A));
        f.extend_from_slice(&[0x00, 0x42]);
        let (c, n, h) = r.heard(NXDN_DATA, &f, &mut out).unwrap();
        assert_eq!((c, n, h.rssi, h.duid, h.start), (NXDN_DATA, 49, 0x42, -1, true));
        assert_eq!(&out[1..49], &f[1..49]);
        assert!(r.heard(P25_LDU, &p25(1, 5, 216, None), &mut out).is_none());
        assert!(r.heard(NXDN_DATA, &f[..30], &mut out).is_none());
        assert!(relay(MODE_P25).heard(NXDN_DATA, &f, &mut out).is_none());
    }
}
