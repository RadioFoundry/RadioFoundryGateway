// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! The C this crate calls: common's FEC (rdof/fec.h) and repeater bursts (rdof/duplex.h), RadioFoundry's block
//! codes (common/src/radiofoundry), and the C library's snprintf, strtod, strtol, ceil, floor, rint, modf, calloc
//! and free.

use core::ffi::{c_char, c_int};

use crate::repeater::{RdofActivity, RdofVoiceRegen};

pub const RDOF_BURST_BYTES: usize = 33;
pub const RDOF_MASK_VOICE_LC_HEADER: u32 = 0x969696;
pub const RDOF_MASK_TERMINATOR_LC: u32 = 0x999999;
pub const RDOF_MASK_CSBK: u16 = 0xA5A5;
pub const RDOF_MASK_DATA_HEADER: u16 = 0xCCCC;
pub const RDOF_MASK_CRC9_RATE12: c_int = 0x0F0;
pub const RDOF_MASK_CRC9_RATE34: c_int = 0x1FF;
pub const RDOF_MASK_CRC9_RATE1: c_int = 0x10F;

// rdof/pdu.h's data types
pub const RDOF_DT_PI_HEADER: c_int = 0;
pub const RDOF_DT_VOICE_LC_HEADER: c_int = 1;
pub const RDOF_DT_TERMINATOR_WITH_LC: c_int = 2;
pub const RDOF_DT_CSBK: c_int = 3;
pub const RDOF_DT_MBC_HEADER: c_int = 4;
pub const RDOF_DT_MBC_CONTINUATION: c_int = 5;
pub const RDOF_DT_DATA_HEADER: c_int = 6;
pub const RDOF_DT_RATE_12: c_int = 7;
pub const RDOF_DT_RATE_34: c_int = 8;
pub const RDOF_DT_RATE_1: c_int = 10;
pub const RDOF_DT_USBD: c_int = 11;
pub const RDOF_DT_VOICE_SYNC: c_int = 0xF0;
pub const RDOF_DT_VOICE: c_int = 0xF1;

extern "C" {
    pub fn abort() -> !;
    pub fn snprintf(buf: *mut c_char, n: usize, format: *const c_char, ...) -> c_int;

    // the C library's number parsing, as the C modules use it
    pub fn strtod(s: *const c_char, end: *mut *mut c_char) -> f64;
    pub fn strtol(s: *const c_char, end: *mut *mut c_char, base: c_int) -> core::ffi::c_long;

    // the C library's heap, for buffers too big for a task's stack
    pub fn calloc(n: usize, size: usize) -> *mut core::ffi::c_void;
    pub fn free(p: *mut core::ffi::c_void);

    // the C library's math (no_std Rust has none)
    pub fn ceil(x: f64) -> f64;
    pub fn floor(x: f64) -> f64;
    pub fn rint(x: f64) -> f64;
    pub fn modf(x: f64, ip: *mut f64) -> f64;

    // rdof/fec.h
    pub fn rdof_bptc196_encode(payload12: *const u8, info196: *mut u8);
    pub fn rdof_build_burst(info196: *const u8, middle48: *const u8, slot_type20: *const u8, out: *mut u8);
    pub fn rdof_rs129_parity(msg9: *const u8, out3: *mut u8);
    pub fn rdof_bptc196_decode(burst: *const u8, out12: *mut u8) -> bool;
    pub fn rdof_check_rs129(lc12: *const u8, mask: u32) -> bool;
    pub fn rdof_check_crc16(block12: *const u8, mask: u16) -> bool;
    pub fn rdof_trellis34_decode(burst: *const u8, out18: *mut u8) -> c_int;
    pub fn rdof_rate1_payload(burst: *const u8, out24: *mut u8);
    pub fn rdof_crc16_ccitt(data: *const u8, len: usize) -> u16;
    pub fn rdof_check_crc9(block_data: *const u8, len: usize, dbsn: c_int, stored: c_int, mask: c_int) -> bool;
    pub fn rdof_embedded_signalling(burst: *const u8, out32: *mut u8);
    pub fn rdof_ambe_frames(burst: *const u8, out: *mut u8);
    pub fn rdof_embedded_lc_decode(raw128: *const u8, lc9: *mut u8) -> bool;

    // rdof/duplex.h
    pub fn rdof_duplex_lc_burst(lc9: *const u8, dt: c_int, cc: c_int, out: *mut u8);
    pub fn rdof_duplex_data(control: u8, burst: *const u8, cc: c_int, out: *mut u8) -> bool;
    pub fn rdof_duplex_silence(n: c_int, cc: c_int, out: *mut u8) -> u8;
    pub fn rdof_voice_regen_init(v: *mut RdofVoiceRegen, lc9: *const u8, cc: c_int);
    pub fn rdof_voice_regen_burst(v: *mut RdofVoiceRegen, control: u8, input: *const u8, out: *mut u8);
    pub fn rdof_activity_lc(slot1: *const RdofActivity, slot2: *const RdofActivity, ts1_low: bool, out9: *mut u8);

    // rdof/mmdvm.h

    // radiofoundry/dmrFec.h, radiofoundry/dmrCach.h
    pub fn dmrFecGolay2087Encode(data: u8) -> u32;
    pub fn dmrFecQr1676Encode(data: u8) -> u16;
    pub fn dmrCachTactEncode(at: u8, tc: u8, lcss: u8) -> u8;
    pub fn dmrCachTactDecode(code: u8, at: *mut u8, tc: *mut u8, lcss: *mut u8) -> i32;
    pub fn dmrCachInterleave(tact: u8, payload: u32) -> u32;
    pub fn dmrCachDeinterleave(bits: u32, tact: *mut u8, payload: *mut u32);
}
