// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
//! Noise on a bench transmit job (rdof/signoise.h; docs/spec/bench-api.md 14.6): complex Gaussian noise, band-limited
//! to +-noise_bw/2 around the carrier, added to the job's complex baseband before the transmitter. A squelch test's
//! "weak signal over noise" and, with the `noise` mode (no carrier), its "noise alone at the same power".
//!
//! Levels: the carrier's amplitude 1 (a constant-envelope mode's, an I/Q mode's |x| = 1) is power_dbm. With noise
//! the sum is scaled down so the carrier plus the noise's 4-sigma peaks stays at 1, and the attenuator takes off that
//! many dB less: the carrier stays at power_dbm, the noise at cn_db under it. The noise alone: its in-band RMS is 1/4.
//!
//! Cost: the noise is made and filtered at about four times noise_bw (the stream divided by a whole number), not at
//! the stream's rate, and spread over the stream by linear interpolation, as the SDR modem spreads a signal mode's
//! source; the interpolation leaves images at the noise rate's multiples (docs/spec/bench-api.md 14.6).
//! Rust only: no C twin.

use core::ffi::{c_char, c_int, c_void};

use crate::ffi::snprintf;
use crate::jsonp::{rdof_jget, rdof_jnode, JDoc, JNode, RDOF_JNULL, RDOF_JNUM};
use crate::sigiq::RdofSigSrc;

extern "C" {
    fn calloc(n: usize, size: usize) -> *mut c_void;
    fn free(p: *mut c_void);
    fn sin(x: f64) -> f64;
    fn cos(x: f64) -> f64;
    fn sqrt(x: f64) -> f64;
    fn log10(x: f64) -> f64;
    fn pow(x: f64, y: f64) -> f64;
    fn logf(x: f32) -> f32;
    fn sqrtf(x: f32) -> f32;
}

/// rdof_bench_noise_t
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RdofBenchNoise {
    pub bw_hz: f64,
    pub cn_db: f64,
    pub offset_hz: f64,
    pub seed: u32,
    pub alone: bool,
}

const BW_MIN: f64 = 1000.0;
const BW_MAX: f64 = 50e3;
const OFFSET_MAX: f64 = 200e3;
const CN_MIN: f64 = -20.0;
const CN_MAX: f64 = 80.0;
/// The noise's peaks kept under full scale, in its in-band RMS: |n| passes 4 sigma once in about 10^7 samples.
const PEAK_SIGMA: f64 = 4.0;
/// The noise rate, in noise_bw at least.
const RATE_BW: f64 = 4.0;
/// The low-pass: flat to bw/2, 60 dB down from 0.7 bw (Kaiser window).
const STOP_BW: f64 = 0.7;
const STOP_DB: f64 = 60.0;
const MAX_TAPS: usize = 161;
const LUT_BITS: u32 = 10;
const LUT: usize = 1 << LUT_BITS;

unsafe fn member<'a>(d: *const c_void, obj: c_int, key: &core::ffi::CStr) -> Option<&'a JNode> {
    let doc = &*(d as *const JDoc);
    let n = rdof_jnode(doc, rdof_jget(doc, obj, key.as_ptr()));
    if n.is_null() || (*n).type_ == RDOF_JNULL {
        None
    } else {
        Some(&*n)
    }
}

unsafe fn say(why: *mut c_char, cap: usize, msg: &core::ffi::CStr) {
    if !why.is_null() && cap > 0 {
        snprintf(why, cap, c"%s".as_ptr(), msg.as_ptr());
    }
}

fn is_on(nz: &RdofBenchNoise) -> bool {
    nz.bw_hz > 0.0
}

/// The noise's in-band RMS against the carrier's amplitude 1 (alone: against full scale, after the scale).
unsafe fn sigma(nz: &RdofBenchNoise) -> f64 {
    if nz.alone {
        1.0 / PEAK_SIGMA
    } else {
        pow(10.0, -nz.cn_db / 20.0)
    }
}

/// The signal's scale: the carrier and the noise's peaks within full scale.
unsafe fn scale(nz: &RdofBenchNoise) -> f64 {
    if nz.alone {
        1.0
    } else {
        1.0 / (1.0 + PEAK_SIGMA * sigma(nz))
    }
}

/// A job's noise fields (noise_bw, cn_db or noise_dbm, noise_seed) into nz; power_dbm is the job's (alone: the noise
/// mode, where it's the noise's level). nz->bw_hz 0: none asked. false with why: a bad request.
#[no_mangle]
pub unsafe extern "C" fn rdof_signoise_parse(
    d: *const c_void,
    obj: c_int,
    power_dbm: f64,
    alone: bool,
    nz: *mut RdofBenchNoise,
    why: *mut c_char,
    cap: usize,
) -> bool {
    *nz = RdofBenchNoise { bw_hz: 0.0, cn_db: f64::NAN, offset_hz: 0.0, seed: 1, alone };
    let bw = member(d, obj, c"noise_bw");
    let cn = member(d, obj, c"cn_db");
    let nd = member(d, obj, c"noise_dbm");
    let seed = member(d, obj, c"noise_seed");
    let off = member(d, obj, c"noise_offset_hz");
    for n in [bw, cn, nd, seed, off].into_iter().flatten() {
        if n.type_ != RDOF_JNUM {
            say(why, cap, c"noise_bw, cn_db, noise_dbm, noise_seed and noise_offset_hz are numbers");
            return false;
        }
    }
    let Some(bw) = bw else {
        if alone {
            say(why, cap, c"noise: noise_bw (Hz), the band the noise fills");
            return false;
        }
        if cn.is_some() || nd.is_some() || seed.is_some() || off.is_some() {
            say(
                why,
                cap,
                c"cn_db, noise_dbm, noise_seed and noise_offset_hz need noise_bw (Hz), the band the noise fills",
            );
            return false;
        }
        return true;
    };
    if !(bw.num >= BW_MIN && bw.num <= BW_MAX) {
        if cap > 0 {
            snprintf(why, cap, c"noise_bw %.0f Hz: 1000-50000".as_ptr(), bw.num);
        }
        return false;
    }
    if let Some(o) = off {
        if !(o.num.abs() <= OFFSET_MAX) {
            say(why, cap, c"noise_offset_hz: -200000 to 200000, the noise's center from freq");
            return false;
        }
        (*nz).offset_hz = o.num;
    }
    if let Some(s) = seed {
        if !(s.num >= 0.0 && s.num <= 4294967295.0 && s.num == (s.num as u64) as f64) {
            say(why, cap, c"noise_seed: a whole number 0-4294967295");
            return false;
        }
        (*nz).seed = s.num as u32;
    }
    if power_dbm.is_nan() {
        say(
            why,
            cap,
            if alone {
                c"noise: power_dbm, the noise's level in noise_bw"
            } else {
                c"noise needs power_dbm, the carrier's level (the attenuator keeps it there)"
            },
        );
        return false;
    }
    if alone {
        if cn.is_some() || nd.is_some() {
            say(why, cap, c"noise: power_dbm is the noise's level; cn_db and noise_dbm are for noise under a carrier");
            return false;
        }
        (*nz).bw_hz = bw.num;
        return true;
    }
    let c = match (cn, nd) {
        (Some(_), Some(_)) => {
            say(why, cap, c"cn_db or noise_dbm, not both");
            return false;
        }
        (None, None) => {
            say(why, cap, c"noise_bw needs cn_db or noise_dbm");
            return false;
        }
        (Some(c), None) => c.num,
        (None, Some(n)) => power_dbm - n.num,
    };
    if !(c >= CN_MIN && c <= CN_MAX) {
        if cap > 0 {
            snprintf(
                why,
                cap,
                c"the carrier is %.1f dB over the noise: -20 to 80 (cn_db, or power_dbm less noise_dbm)".as_ptr(),
                c,
            );
        }
        return false;
    }
    (*nz).bw_hz = bw.num;
    (*nz).cn_db = c;
    true
}

/// dB the samples sit under full scale for the noise's room (0: no noise). The attenuator takes off that much less.
#[no_mangle]
pub unsafe extern "C" fn rdof_signoise_scale_db(nz: *const RdofBenchNoise) -> f64 {
    let nz = &*nz;
    if !is_on(nz) {
        return 0.0;
    }
    if nz.alone {
        20.0 * log10(PEAK_SIGMA)
    } else {
        -20.0 * log10(scale(nz))
    }
}

/// How far from freq the noise reaches, Hz: its offset and its band to where the low-pass is 60 dB down.
#[no_mangle]
pub unsafe extern "C" fn rdof_signoise_reach_hz(nz: *const RdofBenchNoise) -> f64 {
    let n = &*nz;
    if !is_on(n) {
        0.0
    } else {
        n.offset_hz.abs() + STOP_BW * n.bw_hz
    }
}

/// The attenuation (dB under full_dbm, the transmitter's most, 0..span_db) that puts level_dbm (the carrier's, or
/// the noise's alone) on the air with the noise's room taken off. false with why: noise_bw too wide for the stream
/// at stream_fs, the noise reaching past room_hz from freq (0: not checked), or the level out of the transmitter's
/// reach.
#[no_mangle]
pub unsafe extern "C" fn rdof_signoise_atten(
    nz: *const RdofBenchNoise,
    level_dbm: f64,
    full_dbm: f64,
    span_db: f64,
    stream_fs: f64,
    room_hz: f64,
    atten_db: *mut f64,
    why: *mut c_char,
    cap: usize,
) -> bool {
    let n = &*nz;
    let reach = rdof_signoise_reach_hz(nz);
    if room_hz > 0.0 && reach > room_hz {
        if !why.is_null() && cap > 0 {
            snprintf(
                why,
                cap,
                c"the noise reaches %.1f kHz from freq (noise_offset_hz, and noise_bw to its filter's stop band), \
                  past the %.1f kHz this job's transmitter renders around it"
                    .as_ptr(),
                reach / 1e3,
                room_hz / 1e3,
            );
        }
        return false;
    }
    if n.bw_hz > stream_fs / RATE_BW {
        if !why.is_null() && cap > 0 {
            snprintf(
                why,
                cap,
                c"noise_bw %.0f Hz is over a quarter of the transmit stream's %.0f S/s".as_ptr(),
                n.bw_hz,
                stream_fs,
            );
        }
        return false;
    }
    let s = rdof_signoise_scale_db(nz);
    let a = full_dbm - level_dbm - s;
    if a < -0.05 {
        if !why.is_null() && cap > 0 {
            snprintf(
                why,
                cap,
                c"the %s at %.1f dBm needs the transmitter at %.1f dBm with the noise (the samples sit %.1f dB under \
                  full scale for its peaks), over its most, %.1f dBm"
                    .as_ptr(),
                if n.alone { c"noise".as_ptr() } else { c"carrier".as_ptr() },
                level_dbm,
                level_dbm + s,
                s,
                full_dbm,
            );
        }
        return false;
    }
    *atten_db = a.clamp(0.0, if span_db > 0.0 { span_db } else { 0.0 });
    true
}

/// The job's noise as JSON members for its header and summary (",\"noise_bw\":12500,..."): its length, 0 with no
/// noise. level_dbm: the carrier's level as sent (alone: the noise's).
#[no_mangle]
pub unsafe extern "C" fn rdof_signoise_json(
    nz: *const RdofBenchNoise,
    level_dbm: f64,
    out: *mut c_char,
    cap: usize,
) -> usize {
    let n = &*nz;
    if cap > 0 {
        *out = 0;
    }
    if !is_on(n) || cap == 0 {
        return 0;
    }
    let k = if n.alone {
        snprintf(
            out,
            cap,
            c",\"noise_bw\":%.0f,\"noise_offset_hz\":%.0f,\"cn_db\":null,\"noise_dbm\":%.1f,\"carrier_dbm\":null"
                .as_ptr(),
            n.bw_hz,
            n.offset_hz,
            level_dbm,
        )
    } else {
        snprintf(
            out,
            cap,
            c",\"noise_bw\":%.0f,\"noise_offset_hz\":%.0f,\"cn_db\":%.1f,\"noise_dbm\":%.1f,\"carrier_dbm\":%.1f"
                .as_ptr(),
            n.bw_hz,
            n.offset_hz,
            n.cn_db,
            level_dbm - n.cn_db,
            level_dbm,
        )
    };
    if k < 0 || k as usize >= cap {
        *out = 0;
        return 0;
    }
    k as usize
}

// ── the generator ──

struct Gen {
    fs: f64,
    offset_hz: f64, // the noise's center from the carrier
    rate: f64,
    s: f32,          // the signal's scale
    g: f32,          // the filtered unit noise's gain to the output
    d: u32,          // stream samples a noise sample
    k: u32,          // the stream sample's place in the noise sample's interval
    inv_d: f32,
    ntaps: usize,
    at: usize,
    taps: [f32; MAX_TAPS],
    dl: [[f32; 2]; 2 * MAX_TAPS], // the delay line, written twice so the taps read it in one run
    x0: [f32; 2],
    x1: [f32; 2],
    rng: u64,
    phase: u32,
    lut: [[f32; 2]; LUT],
}

fn bessel_i0(x: f64) -> f64 {
    let (mut sum, mut term) = (1.0, 1.0);
    let q = x * x / 4.0;
    for k in 1..60 {
        term *= q / (k * k) as f64;
        sum += term;
        if term < sum * 1e-12 {
            break;
        }
    }
    sum
}

/// The low-pass's taps for a noise rate: how many (odd).
unsafe fn design(taps: &mut [f32; MAX_TAPS], bw: f64, rate: f64) -> usize {
    let fc = (0.5 + STOP_BW) / 2.0 * bw / rate; // cycles a sample, halfway through the transition
    let tw = (STOP_BW - 0.5) * bw / rate;
    let mut n = ((STOP_DB - 7.95) / (14.36 * tw)) as usize + 2;
    n |= 1;
    if n > MAX_TAPS {
        n = MAX_TAPS;
    }
    let beta = 0.1102 * (STOP_DB - 8.7);
    let m = (n - 1) as f64 / 2.0;
    let i0b = bessel_i0(beta);
    let mut sum = 0.0;
    let mut h = [0.0f64; MAX_TAPS];
    for (i, t) in h.iter_mut().enumerate().take(n) {
        let x = i as f64 - m;
        let sinc = if x == 0.0 {
            2.0 * fc
        } else {
            sin(2.0 * core::f64::consts::PI * fc * x) / (core::f64::consts::PI * x)
        };
        let r = x / m;
        let w = bessel_i0(beta * sqrt((1.0 - r * r).max(0.0))) / i0b;
        *t = sinc * w;
        sum += *t;
    }
    for i in 0..n {
        taps[i] = (h[i] / sum) as f32;
    }
    n
}

/// The power of unit complex white noise (at rate) within +-bw/2 after the taps and the linear interpolation to fs.
unsafe fn inband(taps: &[f32], bw: f64, rate: f64, fs: f64, d: u32) -> f64 {
    const STEPS: usize = 400;
    let m = (taps.len() - 1) as f64 / 2.0;
    let df = bw / STEPS as f64;
    let mut p = 0.0;
    for j in 0..STEPS {
        let f = -bw / 2.0 + (j as f64 + 0.5) * df;
        let mut h = 0.0;
        for (i, &t) in taps.iter().enumerate() {
            h += t as f64 * cos(2.0 * core::f64::consts::PI * f * (i as f64 - m) / rate);
        }
        // linear interpolation over d stream samples: (sin(pi f / rate) / (d sin(pi f / fs)))^2
        let li = if d <= 1 || f == 0.0 {
            1.0
        } else {
            let a = sin(core::f64::consts::PI * f / rate) / (d as f64 * sin(core::f64::consts::PI * f / fs));
            a * a
        };
        p += h * h * li * li * df / rate;
    }
    p
}

fn splitmix(x: u64) -> u64 {
    let mut z = x.wrapping_add(0x9e37_79b9_7f4a_7c15);
    z = (z ^ (z >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
    z = (z ^ (z >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
    z ^ (z >> 31)
}

impl Gen {
    #[inline(always)]
    fn uniform(&mut self) -> f32 {
        // xorshift64*: the top 24 bits, -1..1
        self.rng ^= self.rng >> 12;
        self.rng ^= self.rng << 25;
        self.rng ^= self.rng >> 27;
        let r = self.rng.wrapping_mul(0x2545_f491_4f6c_dd1d);
        ((r >> 40) as i32 - (1 << 23)) as f32 * (1.0 / (1 << 23) as f32)
    }

    /// A complex Gaussian sample of power 1 (Marsaglia's polar method).
    #[inline(always)]
    unsafe fn gauss(&mut self) -> [f32; 2] {
        loop {
            let u = self.uniform();
            let v = self.uniform();
            let s = u * u + v * v;
            if s < 1.0 && s > 0.0 {
                let f = sqrtf(-logf(s) / s);
                return [u * f, v * f];
            }
        }
    }

    /// The next noise sample, filtered, at the output's level.
    #[inline(always)]
    unsafe fn next(&mut self) -> [f32; 2] {
        let x = self.gauss();
        let n = self.ntaps;
        self.at = if self.at == 0 { n - 1 } else { self.at - 1 };
        self.dl[self.at] = x;
        self.dl[self.at + n] = x;
        let line = &self.dl[self.at..self.at + n];
        // the taps are symmetric (n odd): each multiplies a pair of samples, half the products
        let m = n / 2;
        let (head, tail) = (&line[..m], &line[n - m..]);
        let (mut re, mut im) = (self.taps[m] * line[m][0], self.taps[m] * line[m][1]);
        for ((t, a), b) in self.taps[..m].iter().zip(head).zip(tail.iter().rev()) {
            re += t * (a[0] + b[0]);
            im += t * (a[1] + b[1]);
        }
        [re * self.g, im * self.g]
    }
}

/// A generator for nz on a transmit stream of stream_fs samples a second (NULL: no noise, or noise_bw over a quarter
/// of it, or no memory).
#[no_mangle]
pub unsafe extern "C" fn rdof_signoise_new(nz: *const RdofBenchNoise, stream_fs: f64) -> *mut c_void {
    let n = &*nz;
    if !is_on(n) || !(stream_fs > 0.0) || n.bw_hz > stream_fs / RATE_BW {
        return core::ptr::null_mut();
    }
    let g = calloc(1, core::mem::size_of::<Gen>()) as *mut Gen;
    if g.is_null() {
        return g.cast();
    }
    let x = &mut *g; // all zeros is a valid Gen
    let d = ((stream_fs / (RATE_BW * n.bw_hz)) as u32).max(1);
    x.fs = stream_fs;
    x.offset_hz = n.offset_hz;
    x.d = d;
    x.inv_d = 1.0 / d as f32;
    x.rate = stream_fs / d as f64;
    x.ntaps = design(&mut x.taps, n.bw_hz, x.rate);
    let p = inband(&x.taps[..x.ntaps], n.bw_hz, x.rate, stream_fs, d);
    let s = scale(n);
    x.s = s as f32;
    x.g = (s * sigma(n) / sqrt(p)) as f32;
    x.rng = splitmix(n.seed as u64 ^ 0x5eed_0000_0000_0000) | 1;
    for (i, e) in x.lut.iter_mut().enumerate() {
        let a = 2.0 * core::f64::consts::PI * i as f64 / LUT as f64;
        *e = [cos(a) as f32, sin(a) as f32];
    }
    for _ in 0..x.ntaps {
        x.next(); // the delay line full: no ramp at the start
    }
    x.x1 = x.next();
    g.cast()
}

#[no_mangle]
pub unsafe extern "C" fn rdof_signoise_free(g: *mut c_void) {
    free(g);
}

/// The rate the noise is made at, samples a second.
#[no_mangle]
pub unsafe extern "C" fn rdof_signoise_rate(g: *const c_void) -> f64 {
    (*(g as *const Gen)).rate
}

/// n stream samples (I, Q interleaved), in place: the signal scaled for the noise's room, the noise added around
/// off_hz (the carrier, from the stream's center) and its own offset. The noise is made at its own rate around 0 Hz
/// and moved to its place at the stream's rate, so an offset costs nothing more.
#[no_mangle]
pub unsafe extern "C" fn rdof_signoise_add(g: *mut c_void, iq: *mut f32, n: c_int, off_hz: f64) {
    if g.is_null() || n <= 0 {
        return;
    }
    let x = &mut *(g as *mut Gen);
    let out = core::slice::from_raw_parts_mut(iq, 2 * n as usize);
    let step = (((off_hz + x.offset_hz) / x.fs) * 4294967296.0) as i64 as u32;
    let s = x.s;
    for o in out.chunks_exact_mut(2) {
        if x.k == 0 {
            x.x0 = x.x1;
            x.x1 = x.next();
        }
        x.k += 1;
        let w = x.k as f32 * x.inv_d;
        if x.k == x.d {
            x.k = 0;
        }
        let re = x.x0[0] + (x.x1[0] - x.x0[0]) * w;
        let im = x.x0[1] + (x.x1[1] - x.x0[1]) * w;
        let r = x.lut[(x.phase >> (32 - LUT_BITS)) as usize];
        x.phase = x.phase.wrapping_add(step);
        o[0] = s * o[0] + re * r[0] - im * r[1];
        o[1] = s * o[1] + re * r[1] + im * r[0];
    }
}

// ── the noise mode: no carrier, only the noise (its source renders zeros for `seconds`) ──

struct NoiseTx {
    left: u64,
}

unsafe extern "C" fn tx_iq(ctx: *mut c_void, iq: *mut f32, n: c_int) -> c_int {
    let t = &mut *(ctx as *mut NoiseTx);
    let k = (n.max(0) as u64).min(t.left) as usize;
    core::ptr::write_bytes(iq, 0, 2 * k);
    t.left -= k as u64;
    k as c_int
}

unsafe extern "C" fn tx_done(ctx: *mut c_void) -> bool {
    (*(ctx as *const NoiseTx)).left == 0
}

/// The noise mode's transmitter (rdof_sig_mode_t.tx_new): `seconds` of nothing, which the platform's noise fills.
#[no_mangle]
pub unsafe extern "C" fn rdof_signoise_tx_new(
    d: *const c_void,
    job: c_int,
    fs: f64,
    seconds: *mut f64,
    error: *mut c_char,
    ecap: usize,
) -> *mut c_void {
    let secs = match member(d, job, c"seconds") {
        Some(n) if n.type_ == RDOF_JNUM => n.num,
        _ => 0.0,
    };
    if !(secs > 0.0 && secs <= 3600.0) {
        say(error, ecap, c"noise: seconds (0-3600), how long it keys");
        return core::ptr::null_mut();
    }
    let t = calloc(1, core::mem::size_of::<NoiseTx>()) as *mut NoiseTx;
    if t.is_null() {
        say(error, ecap, c"no memory for the job");
        return t.cast();
    }
    (*t).left = (secs * fs + 0.5) as u64;
    *seconds = secs;
    t.cast()
}

#[no_mangle]
pub unsafe extern "C" fn rdof_signoise_tx_free(tx: *mut c_void) {
    free(tx);
}

#[no_mangle]
pub unsafe extern "C" fn rdof_signoise_tx_src(tx: *mut c_void) -> RdofSigSrc {
    RdofSigSrc {
        freq: None,
        iq: Some(tx_iq),
        done: Some(tx_done),
        peak_hz: 0.0,
        rate: 0.0,
        span_lo_hz: 0.0,
        span_hi_hz: 0.0,
        ctx: tx,
    }
}
