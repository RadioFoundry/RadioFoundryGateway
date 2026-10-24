// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! DMR layer 2 (rdof/layer2.h): sync patterns, slot type Golay(20,8), EMB QR(16,7), CACH TACT Hamming(7,4),
//! and building bursts. A port of common/src/layer2.c; the
//! block codes are RadioFoundry's C (common/src/radiofoundry), called through ffi.
//!
//! The C header is the interface: every function here is exported under its C name with its C signature.
//! Out pointers the C allows to be NULL are Option<&mut _>.

use core::ffi::{c_char, c_int};
use core::sync::atomic::{AtomicBool, AtomicU16, AtomicU32, Ordering};

use crate::bits::{bits_to_bytes, bits_to_u64, get_field, u64_to_bits};
use crate::ffi::{self, RDOF_BURST_BYTES, RDOF_MASK_TERMINATOR_LC, RDOF_MASK_VOICE_LC_HEADER};

type Burst = [u8; RDOF_BURST_BYTES];

pub const RDOF_BS_VOICE: u64 = 0x755FD7DF75F7;
pub const RDOF_MS_VOICE: u64 = 0x7F7D5DD57DFD;
const RDOF_TS1_VOICE: u64 = 0x5D577F7757FF;
const RDOF_TS2_VOICE: u64 = 0x7DFFD5F55D5F;
pub const RDOF_DATA_XOR: u64 = 0xAAAAAAAAAAAA;

const VOICE_SYNC: [u64; 4] = [RDOF_BS_VOICE, RDOF_MS_VOICE, RDOF_TS1_VOICE, RDOF_TS2_VOICE];
const FAMILY_NAMES: [&[u8]; 4] = [b"bs\0", b"ms\0", b"ts1\0", b"ts2\0"];

fn set<T>(out: Option<&mut T>, v: T) {
    if let Some(o) = out {
        *o = v;
    }
}

#[no_mangle]
pub extern "C" fn rdof_family_voice_sync(f: u32) -> u64 {
    VOICE_SYNC.get(f as usize).copied().unwrap_or(RDOF_MS_VOICE)
}

#[no_mangle]
pub extern "C" fn rdof_family_name(f: u32) -> *const c_char {
    FAMILY_NAMES.get(f as usize).copied().unwrap_or(b"?\0").as_ptr().cast()
}

#[no_mangle]
pub extern "C" fn rdof_middle_field(burst: &Burst) -> u64 {
    get_field(burst, 108, 48)
}

#[no_mangle]
pub extern "C" fn rdof_sync_errors(burst: &Burst, pattern48: u64) -> c_int {
    (rdof_middle_field(burst) ^ pattern48).count_ones() as c_int
}

#[no_mangle]
pub extern "C" fn rdof_detect_sync(
    burst: &Burst,
    max_errors: c_int,
    family: Option<&mut u32>,
    data: Option<&mut bool>,
    errors: Option<&mut c_int>,
) -> bool {
    let mid = rdof_middle_field(burst);
    // Ties go to the first: BS, MS, TS1, TS2, voice before data.
    let (mut best, mut bf, mut bd) = (49, 0, false);
    for (f, &voice) in VOICE_SYNC.iter().enumerate() {
        for d in [false, true] {
            let p = if d { voice ^ RDOF_DATA_XOR } else { voice };
            let e = (mid ^ p).count_ones() as c_int;
            if e < best {
                (best, bf, bd) = (e, f as u32, d);
            }
        }
    }
    set(errors, best);
    if best > max_errors {
        return false;
    }
    set(family, bf);
    set(data, bd);
    true
}

// ── block codes ─────────────────────────────────────────────────────────────
// Golay(20,8), QR(16,7) and the CACH are RadioFoundry's (ETSI TS 102 361-1 B.3.1, B.3.2, B.3.5, B.4.1). As in the C:
// the codewords once, in tables; a word that already is one is a lookup, else the first codeword within the
// radius, the only one there (Golay(20,8) has distance 7, QR(16,7) 6), so the result is RadioFoundry's.

#[no_mangle]
pub extern "C" fn rdof_golay2087_encode(value8: c_int) -> u32 {
    unsafe { ffi::dmrFecGolay2087Encode(value8 as u8) }
}

#[no_mangle]
pub extern "C" fn rdof_qr1676_encode(value7: c_int) -> u32 {
    u32::from(unsafe { ffi::dmrFecQr1676Encode((value7 & 0x7F) as u8) })
}

#[no_mangle]
pub extern "C" fn rdof_hamming74_encode(value4: c_int) -> u32 {
    u32::from(unsafe {
        ffi::dmrCachTactEncode(((value4 >> 3) & 1) as u8, ((value4 >> 2) & 1) as u8, (value4 & 3) as u8)
    })
}

/// RadioFoundry's result (bits corrected, or -1) as rdof's: the value, or -1 with errors = radius + 1.
fn block_result(r: c_int, value: c_int, radius: c_int, errors: Option<&mut c_int>) -> c_int {
    set(errors, if r < 0 { radius + 1 } else { r });
    if r < 0 {
        -1
    } else {
        value
    }
}

// Atomics so two cores filling the tables at once is defined behavior: both store the same values.
static GOLAY_CW: [AtomicU32; 256] = [const { AtomicU32::new(0) }; 256];
static QR_CW: [AtomicU16; 128] = [const { AtomicU16::new(0) }; 128];
static CW_READY: AtomicBool = AtomicBool::new(false);

fn cw_init() {
    if CW_READY.load(Ordering::Acquire) {
        return;
    }
    for (d, cw) in GOLAY_CW.iter().enumerate() {
        cw.store(rdof_golay2087_encode(d as c_int), Ordering::Relaxed);
    }
    for (d, cw) in QR_CW.iter().enumerate() {
        cw.store(rdof_qr1676_encode(d as c_int) as u16, Ordering::Relaxed);
    }
    CW_READY.store(true, Ordering::Release);
}

/// A codeword table entry, whichever width.
trait Codeword {
    fn get(&self) -> u32;
}
impl Codeword for AtomicU32 {
    fn get(&self) -> u32 {
        self.load(Ordering::Relaxed)
    }
}
impl Codeword for AtomicU16 {
    fn get(&self) -> u32 {
        u32::from(self.load(Ordering::Relaxed))
    }
}

/// The value whose codeword is `word` (its top bits, `shift` down), or the first within `radius`.
fn table_decode(table: &[impl Codeword], word: u32, shift: u32, radius: u32) -> (c_int, c_int) {
    let d = (word >> shift) as usize;
    if table[d].get() == word {
        return (d as c_int, 0);
    }
    table
        .iter()
        .enumerate()
        .map(|(d, cw)| (d as c_int, (cw.get() ^ word).count_ones()))
        .find(|&(_, e)| e <= radius)
        .map_or((table.len() as c_int, -1), |(d, e)| (d, e as c_int))
}

#[no_mangle]
pub extern "C" fn rdof_golay2087_decode(word20: u32, errors: Option<&mut c_int>) -> c_int {
    cw_init();
    let (d, r) = table_decode(&GOLAY_CW, word20 & 0xFFFFF, 12, 3);
    block_result(r, d, 3, errors)
}

#[no_mangle]
pub extern "C" fn rdof_qr1676_decode(word16: u32, errors: Option<&mut c_int>) -> c_int {
    cw_init();
    let (d, r) = table_decode(&QR_CW, word16 & 0xFFFF, 9, 2);
    block_result(r, d, 2, errors)
}

/// (AT, TC, LCSS) and the bits corrected (0 or 1: a Hamming(7,4) word always decodes).
fn tact_decode(word7: u8) -> (u8, u8, u8, c_int) {
    let (mut at, mut tc, mut lcss) = (0, 0, 0);
    let r = unsafe { ffi::dmrCachTactDecode(word7 & 0x7F, &mut at, &mut tc, &mut lcss) };
    (at, tc, lcss, r)
}

#[no_mangle]
pub extern "C" fn rdof_hamming74_decode(word7: u32, errors: Option<&mut c_int>) -> c_int {
    let (at, tc, lcss, r) = tact_decode(word7 as u8);
    block_result(r, c_int::from((at << 3) | (tc << 2) | lcss), 1, errors)
}

// ── field decoders ──────────────────────────────────────────────────────────

#[no_mangle]
pub extern "C" fn rdof_decode_slot_type(
    burst: &Burst,
    cc: Option<&mut c_int>,
    dt: Option<&mut c_int>,
    errors: Option<&mut c_int>,
) -> bool {
    let w = ((get_field(burst, 98, 10) << 10) | get_field(burst, 156, 10)) as u32;
    let v = rdof_golay2087_decode(w, errors);
    if v < 0 {
        return false;
    }
    set(cc, v >> 4);
    set(dt, v & 0x0F);
    true
}

#[no_mangle]
pub extern "C" fn rdof_decode_emb(
    burst: &Burst,
    cc: Option<&mut c_int>,
    pi: Option<&mut c_int>,
    lcss: Option<&mut c_int>,
    errors: Option<&mut c_int>,
) -> bool {
    let w = ((get_field(burst, 108, 8) << 8) | get_field(burst, 148, 8)) as u32;
    let v = rdof_qr1676_decode(w, errors);
    if v < 0 {
        set(pi, 0);
        set(lcss, 0);
        return false;
    }
    set(cc, v >> 3);
    set(pi, (v >> 2) & 1);
    set(lcss, v & 3);
    true
}

/// The CACH's TACT word and 17-bit payload.
fn cach_split(cach24: &[u8; 24]) -> (u8, u32) {
    let (mut tact, mut payload) = (0, 0);
    unsafe { ffi::dmrCachDeinterleave(bits_to_u64(cach24) as u32, &mut tact, &mut payload) };
    (tact, payload)
}

#[no_mangle]
pub extern "C" fn rdof_decode_cach_slot(cach24: &[u8; 24]) -> c_int {
    let (_, tc, _, _) = tact_decode(cach_split(cach24).0);
    if tc != 0 {
        2
    } else {
        1
    }
}

#[no_mangle]
pub extern "C" fn rdof_encode_cach(slot: c_int, at: c_int, lcss: c_int, out24: &mut [u8; 24]) {
    rdof_encode_cach_lc(slot, at, lcss, 0, out24);
}

#[no_mangle]
pub extern "C" fn rdof_encode_cach_lc(slot: c_int, at: c_int, lcss: c_int, payload17: u32, out24: &mut [u8; 24]) {
    let cach = unsafe {
        let tact = ffi::dmrCachTactEncode(at as u8, (slot - 1) as u8, lcss as u8);
        ffi::dmrCachInterleave(tact, payload17 & 0x1FFFF)
    };
    u64_to_bits(u64::from(cach), out24);
}

#[no_mangle]
pub extern "C" fn rdof_decode_cach_payload(cach24: &[u8; 24], lcss: Option<&mut c_int>) -> u32 {
    let (tact, payload) = cach_split(cach24);
    set(lcss, c_int::from(tact_decode(tact).2));
    payload
}

// ── burst builders ──────────────────────────────────────────────────────────

#[no_mangle]
pub extern "C" fn rdof_slot_type_bits(cc: c_int, dt: c_int, out20: &mut [u8; 20]) {
    u64_to_bits(u64::from(rdof_golay2087_encode(((cc & 0xF) << 4) | (dt & 0xF))), out20);
}

#[no_mangle]
pub extern "C" fn rdof_emb_bits(cc: c_int, lcss: c_int, pi: c_int, out16: &mut [u8; 16]) {
    u64_to_bits(u64::from(rdof_qr1676_encode(((cc & 0xF) << 3) | ((pi & 1) << 2) | (lcss & 3))), out16);
}

#[no_mangle]
pub extern "C" fn rdof_info_burst(info196: &[u8; 196], dt: c_int, cc: c_int, sync48: u64, out: &mut Burst) {
    let mut mid = [0u8; 48];
    let mut st = [0u8; 20];
    u64_to_bits(sync48, &mut mid);
    rdof_slot_type_bits(cc, dt, &mut st);
    unsafe { ffi::rdof_build_burst(info196.as_ptr(), mid.as_ptr(), st.as_ptr(), out.as_mut_ptr()) };
}

#[no_mangle]
pub extern "C" fn rdof_data_burst(payload12: &[u8; 12], dt: c_int, cc: c_int, sync48: u64, out: &mut Burst) {
    let mut info = [0u8; 196];
    unsafe { ffi::rdof_bptc196_encode(payload12.as_ptr(), info.as_mut_ptr()) };
    rdof_info_burst(&info, dt, cc, sync48, out);
}

#[no_mangle]
pub extern "C" fn rdof_voice_lc12(lc9: &[u8; 9], data_type: c_int, out12: &mut [u8; 12]) {
    let mask = if data_type == 1 { RDOF_MASK_VOICE_LC_HEADER } else { RDOF_MASK_TERMINATOR_LC };
    let mut par = [0u8; 3];
    unsafe { ffi::rdof_rs129_parity(lc9.as_ptr(), par.as_mut_ptr()) };
    out12[..9].copy_from_slice(lc9);
    for (i, p) in par.iter().enumerate() {
        out12[9 + i] = p ^ (mask >> (16 - 8 * i)) as u8;
    }
}

#[no_mangle]
pub extern "C" fn rdof_group_lc(src: u32, dst: u32, group: bool, service_options: c_int, out9: &mut [u8; 9]) {
    out9[0] = if group { 0x00 } else { 0x03 };
    out9[1] = 0;
    out9[2] = service_options as u8;
    out9[3..6].copy_from_slice(&dst.to_be_bytes()[1..]);
    out9[6..9].copy_from_slice(&src.to_be_bytes()[1..]);
}

#[no_mangle]
pub extern "C" fn rdof_lc_burst(lc9: &[u8; 9], data_type: c_int, cc: c_int, sync48: u64, out: &mut Burst) {
    let mut lc12 = [0u8; 12];
    rdof_voice_lc12(lc9, data_type, &mut lc12);
    rdof_data_burst(&lc12, data_type, cc, sync48, out);
}

#[no_mangle]
pub extern "C" fn rdof_voice_burst(voice216: &[u8; 216], middle48: &[u8; 48], out: &mut Burst) {
    let mut bits = [0u8; 264];
    bits[..108].copy_from_slice(&voice216[..108]);
    bits[108..156].copy_from_slice(middle48);
    bits[156..].copy_from_slice(&voice216[108..]);
    bits_to_bytes(&bits, out);
}

#[no_mangle]
pub extern "C" fn rdof_voice_middle(cc: c_int, lcss: c_int, frag32: &[u8; 32], pi: c_int, out48: &mut [u8; 48]) {
    let mut e = [0u8; 16];
    rdof_emb_bits(cc, lcss, pi, &mut e);
    out48[..8].copy_from_slice(&e[..8]);
    out48[8..40].copy_from_slice(frag32);
    out48[40..].copy_from_slice(&e[8..]);
}
