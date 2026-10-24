// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! Bit helpers, as rdof/bits.h: bursts are packed bytes, MSB first; bit arrays hold 0 or 1 per byte.

/// n (<= 64) bits of packed bytes from bit `start`, MSB first.
/// A byte at a time (a bounds check a byte, not a bit), in 32-bit halves (the S3's u64 shifts are slow); past 64
/// bits the first fall off the top, as the C's.
pub fn get_field(bytes: &[u8], start: usize, n: usize) -> u64 {
    if n <= 32 {
        u64::from(field32(bytes, start, n))
    } else {
        u64::from(field32(bytes, start, n - 32)) << 32 | u64::from(field32(bytes, start + n - 32, 32))
    }
}

/// The low 32 bits of the field.
fn field32(bytes: &[u8], start: usize, n: usize) -> u32 {
    let (mut v, mut i, end) = (0u32, start, start + n);
    while i < end {
        let off = i & 7;
        let take = (8 - off).min(end - i);
        v = v << take | u32::from(bytes[i >> 3]) >> (8 - off - take) & ((1 << take) - 1);
        i += take;
    }
    v
}

/// value's low n bits into packed bytes at bit `start`, MSB first; the bits around kept.
pub fn set_field(bytes: &mut [u8], start: usize, n: usize, value: u64) {
    for i in 0..n {
        let pos = start + i;
        let mask = 0x80u8 >> (pos & 7);
        if (value >> (n - 1 - i)) & 1 != 0 {
            bytes[pos >> 3] |= mask;
        } else {
            bytes[pos >> 3] &= !mask;
        }
    }
}

/// `value`'s low out.len() bits, MSB first.
pub fn u64_to_bits(value: u64, out: &mut [u8]) {
    let w = out.len();
    for (i, b) in out.iter_mut().enumerate() {
        *b = ((value >> (w - 1 - i)) & 1) as u8;
    }
}

pub fn bits_to_u64(bits: &[u8]) -> u64 {
    bits.iter().fold(0, |v, &b| (v << 1) | u64::from(b & 1))
}

/// Packs bits (any nonzero is a 1) into out, zero padded.
pub fn bits_to_bytes(bits: &[u8], out: &mut [u8]) {
    let (whole, rest) = bits.as_chunks::<8>();
    for (o, b) in out.iter_mut().zip(whole) {
        *o = b.iter().fold(0, |v, &bit| v << 1 | u8::from(bit != 0));
    }
    if !rest.is_empty() {
        out[whole.len()] = rest.iter().enumerate().fold(0, |v, (i, &bit)| v | u8::from(bit != 0) << (7 - i));
    }
}

/// A C length (bits or bytes); a negative one is none, as the C's loops treat it.
fn len(n: i32) -> usize {
    n.max(0) as usize
}

/// # Safety
/// `ptr` is valid for `n` elements (any pointer when `n` is 0).
pub(crate) unsafe fn slice<'a>(ptr: *const u8, n: usize) -> &'a [u8] {
    if n == 0 {
        &[]
    } else {
        core::slice::from_raw_parts(ptr, n)
    }
}

/// # Safety
/// As `slice`, writable.
pub(crate) unsafe fn slice_mut<'a>(ptr: *mut u8, n: usize) -> &'a mut [u8] {
    if n == 0 {
        &mut []
    } else {
        core::slice::from_raw_parts_mut(ptr, n)
    }
}

// rdof/bits.h

#[no_mangle]
pub unsafe extern "C" fn rdof_bytes_to_bits(input: *const u8, nbytes: usize, bits: *mut u8) {
    let input = slice(input, nbytes);
    for (&byte, out) in input.iter().zip(slice_mut(bits, nbytes * 8).as_chunks_mut::<8>().0) {
        *out = [
            byte >> 7,
            byte >> 6 & 1,
            byte >> 5 & 1,
            byte >> 4 & 1,
            byte >> 3 & 1,
            byte >> 2 & 1,
            byte >> 1 & 1,
            byte & 1,
        ];
    }
}

#[no_mangle]
pub unsafe extern "C" fn rdof_bits_to_bytes(bits: *const u8, nbits: usize, out: *mut u8) {
    bits_to_bytes(slice(bits, nbits), slice_mut(out, nbits.div_ceil(8)));
}

#[no_mangle]
pub unsafe extern "C" fn rdof_bits_to_u64(bits: *const u8, n: i32) -> u64 {
    bits_to_u64(slice(bits, len(n)))
}

#[no_mangle]
pub unsafe extern "C" fn rdof_u64_to_bits(value: u64, width: i32, bits: *mut u8) {
    u64_to_bits(value, slice_mut(bits, len(width)));
}

#[no_mangle]
pub unsafe extern "C" fn rdof_get_field(bytes: *const u8, start: i32, n: i32) -> u64 {
    let (start, n) = (len(start), len(n));
    get_field(slice(bytes, (start + n).div_ceil(8)), start, n)
}

#[no_mangle]
pub unsafe extern "C" fn rdof_set_field(bytes: *mut u8, start: i32, n: i32, value: u64) {
    let (start, n) = (len(start), len(n));
    set_field(slice_mut(bytes, (start + n).div_ceil(8)), start, n, value);
}

#[no_mangle]
pub extern "C" fn rdof_popcount64(v: u64) -> i32 {
    v.count_ones() as i32
}

#[no_mangle]
pub unsafe extern "C" fn rdof_be24(p: *const u8) -> u32 {
    let p = slice(p, 3);
    u32::from_be_bytes([0, p[0], p[1], p[2]])
}

#[no_mangle]
pub unsafe extern "C" fn rdof_be32(p: *const u8) -> u32 {
    u32::from_be_bytes(*(p as *const [u8; 4]))
}

#[no_mangle]
pub unsafe extern "C" fn rdof_put_be24(p: *mut u8, v: u32) {
    slice_mut(p, 3).copy_from_slice(&v.to_be_bytes()[1..]);
}

#[no_mangle]
pub unsafe extern "C" fn rdof_put_be32(p: *mut u8, v: u32) {
    *(p as *mut [u8; 4]) = v.to_be_bytes();
}
