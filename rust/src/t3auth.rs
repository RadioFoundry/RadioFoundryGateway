// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! Tier III MS authentication, the algorithm (ETSI TS 102 361-4 6.4.8, figure 6.30; docs/spec/fw/tier3.md 3.10):
//! the RC4 keystream generator keyed with the challenge (RAND, 3 bytes, most significant first) followed by the
//! MS's key K (16 bytes) makes 259 bytes; the first 256 are discarded and the last 3 are the 24-bit response,
//! XORed with the MS's PSN (3 bytes) when one is used (6.4.8.1.2). The site's procedure is in t3authsite.rs, the
//! MS simulator's in t3ms.rs.
//!
//! Also the site's challenge source: a 64-bit mixer stirred with what the caller gives it (rdof_t3_entropy) and the
//! time of each challenge, giving values in 000000h..FFFCDFh (table 6.17).

/// C_ACKU reason: Authentication Response (0100 1000b; tables 6.18 and 7.42).
pub const R_AUTH_RESPONSE: u8 = 0x48;
/// C_NACKU reason from an MS: MSNot_Supported (0000 0000b; table 7.43).
pub const R_MS_NOT_SUPPORTED: u8 = 0x00;

/// The highest challenge value (table 6.17: 000000h to FFFCDFh).
pub const CHALLENGE_MAX: u32 = 0xFF_FCDF;

/// The 24-bit response to challenge rand (its low 24 bits) for key k, XORed with psn when the MS uses one.
pub fn response(rand: u32, k: &[u8; 16], psn: Option<u32>) -> u32 {
    let mut key = [0u8; 19];
    key[0] = (rand >> 16) as u8;
    key[1] = (rand >> 8) as u8;
    key[2] = rand as u8;
    key[3..].copy_from_slice(k);
    // RC4: the key schedule, then 259 bytes of keystream
    let mut s = [0u8; 256];
    for (i, v) in s.iter_mut().enumerate() {
        *v = i as u8;
    }
    let mut j = 0u8;
    for i in 0..256 {
        j = j.wrapping_add(s[i]).wrapping_add(key[i % key.len()]);
        s.swap(i, j as usize);
    }
    let (mut i, mut j) = (0u8, 0u8);
    let mut out = 0u32;
    for n in 0..259 {
        i = i.wrapping_add(1);
        j = j.wrapping_add(s[i as usize]);
        s.swap(i as usize, j as usize);
        let b = s[s[i as usize].wrapping_add(s[j as usize]) as usize];
        if n >= 256 {
            out = out << 8 | b as u32;
        }
    }
    out ^ (psn.unwrap_or(0) & 0xFF_FFFF)
}

/// The site's challenge source. Valid all zero; stir it with something unpredictable before use (the caller's
/// random source: rdof_t3_entropy).
#[derive(Clone, Copy, Default)]
pub struct Rng {
    s: u64,
    n: u64,
}

fn mix(mut z: u64) -> u64 {
    // SplitMix64's finalizer
    z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
    z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
    z ^ (z >> 31)
}

impl Rng {
    pub fn stir(&mut self, v: u64) {
        self.s = mix(self.s ^ mix(v.wrapping_add(0x9E37_79B9_7F4A_7C15)));
    }

    /// A challenge in 0..=CHALLENGE_MAX; now (ms) is stirred in.
    pub fn challenge(&mut self, now: i64) -> u32 {
        self.n = self.n.wrapping_add(1);
        self.stir(now as u64 ^ self.n.rotate_left(40));
        (mix(self.s ^ 0xA5A5_5A5A_C3C3_3C3C) % (CHALLENGE_MAX as u64 + 1)) as u32
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const K: [u8; 16] = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16];

    #[test]
    fn vectors() {
        // 361-4 6.4.8.0, table 6.15 (no PSN) and table 6.16 (PSN 010203h)
        assert_eq!(response(0x7A17C0, &K, None), 0xE8CA1D);
        assert_eq!(response(0x7A17C0, &K, Some(0x010203)), 0xE9C81E);
        // a PSN of zero is the same as none (6.4.8.0: backward compatibility)
        assert_eq!(response(0x7A17C0, &K, Some(0)), 0xE8CA1D);
        // another key or challenge, another response
        let mut k2 = K;
        k2[15] ^= 1;
        assert_ne!(response(0x7A17C0, &k2, None), 0xE8CA1D);
        assert_ne!(response(0x7A17C1, &K, None), 0xE8CA1D);
    }

    #[test]
    fn challenges() {
        let mut a = Rng::default();
        a.stir(12345);
        let mut b = a;
        let mut last = u32::MAX;
        for t in 0..10_000 {
            let c = a.challenge(t * 60);
            assert!(c <= CHALLENGE_MAX);
            assert_ne!(c, last);
            last = c;
        }
        // stirred differently: another sequence
        b.stir(1);
        let mut a2 = Rng::default();
        a2.stir(12345);
        assert_ne!(a2.challenge(0), b.challenge(0));
        // the range's top is reached in a reasonable spread: values above 0xF00000 appear
        let mut c = Rng::default();
        c.stir(7);
        assert!((0..4000).any(|t| c.challenge(t) > 0xF0_0000));
    }
}
