# SDR receive path: per-stage algorithms, AFC, ppm and the sample clock

What: every receive stage of the firmware's SDR path, from wideband complex float in to DMR bursts out, with its constants, plus ppm learning, the sample clock and warm-up.
Sources: `sdr/src/{channelizer,fm,demod,receiver,agc,ppm,rate}.c`, `esp_idf/main/{sdr_task,dsp_esp}.c`, `esp_idf/main/fir_s3.S`, `components/netclock/netclock_tempcal.{h,c}`.
Related: [sdr.md](sdr.md) (the index, the pipeline, CPU, tolerances), [sdr-frontends.md](sdr-frontends.md), [sdr-tx.md](sdr-tx.md).

Notation: `fs` is the wideband rate, `D = fs / 48000` (a whole number, enforced by `sdr/src/config.c:device`),
`M = 64`, `N = D·M`. Positions are 48 kS/s sample indices from the start of the stream unless they say otherwise.

## 1. Input conversion

| Format | Float value | Where |
|---|---|---|
| cu8 | `(v − 127.5) / 127.5` via a 256-entry LUT | `sdr/src/iq.c` |
| cs8 | `v / 128` | `sdr/src/frontend.c` (`rdof_fe_convert`) |
| cs16 | `v / 32768` | same |

The values are exact in float32 (`test_sdr.c:test_iq`). The filler for a USB gap is byte `0x80` for cu8
and `0` otherwise.

## 2. Front-end AGC (`sdr/src/agc.c`)

Measurement per chunk: cu8 bytes `≤ 1` or `≥ 254` are clipped (`rdof_agc_measure_cu8`); for float input,
`|re|` or `|im| ≥ RDOF_AGC_FULL_SCALE` (0.99f, `agc.h`). The result is the clipped share and the peak.

| Setting (`device.agc`) | Default | Meaning |
|---|---|---|
| `min` | 0 dB | lowest gain |
| `clip` | 1e-4 | clipped share that counts as clipping |
| `headroom_db` | 6 | margin kept below full scale when stepping up |
| `hold_s` | 10 | quiet time before a step up |
| `relax_s` | 300 | quiet time before the ceiling rises one step |
| `settle_s` | 0.3 | ignore measurements this long after a change |

The defaults are in `sdr/src/config.c:device` (`agc_dflt`). `device.agc` needs a fixed `device.gain`, which is
the most it may use.

The law, with `steps` being the gains the front end can set (its `gain_steps`; on an R820T, the measured steps
in `rtlsdr/src/r820.c`, [rtlsdr.md](../rtlsdr.md) R1.2: 0, 3.2, 4.5, 7.7, 9.0, ..., 44.7, 46.2 dB):

- clip share `> clip` steps the gain down one step and sets the ceiling there;
- each `relax_s` without clipping raises the ceiling one step;
- after `hold_s` without clipping, step up one step (up to the ceiling) if
  `peak_window · 10^((up − gain + headroom_db)/20) < 0.99`.

The firmware's peak window is a max-queue of `RDOF_AGC_MAX_PEAKS` (256) entries. If it fills, the step up comes
later, never sooner. `sdr_task.c:chunk` scales the samples by `10^((ref − gain)/20)`, where `ref = agc.max`, so RSSI
holds as the gain moves (`SoapySource._scale`). Chunks that hold filler are left out of the AGC. Tests:
`test_sdr.c:test_agc` (five scripted level sequences, exact decisions, on the R820T's measured steps).

## 3. Channelizer (`sdr/src/channelizer.c`)

Overlap-save fast convolution. One forward FFT per block, then per channel a bin gather, the filter, a 64-point
inverse FFT and an NCO.

| Quantity | Value | Symbol |
|---|---|---|
| inverse FFT size | M = 64 | `channelizer.h:RDOF_CHZ_OUT_FFT` |
| forward FFT size | N = D·M (384 at 288 kS/s, 2048 at 1.536 MS/s) | |
| hop | 3N/4 (init fails if `hop % D ≠ 0`) | |
| outputs kept per block | `M − discard`, with `discard = (N − hop)/D` = 16, so 48 | |
| blocks per second | fs / hop = 1000 at every rate | |
| filter cutoff | 7500 Hz | `channelizer.h:RDOF_CHZ_CUTOFF_HZ` |
| filter taps | N − hop + 1 = N/4 + 1 | |
| group delay | N/8 inputs = 8 outputs | |

**Filter** (`rdof_chz_lowpass`): `h[i] = sinc(2·cutoff/fs · n) · I0(β·sqrt(1 − (2i/(T−1) − 1)²)) / I0(β)`, with
`n = i − (T−1)/2`, β = 5 and `I0` from its power series. It is normalized to `Σh = 1`. The frequency response is
`H[j] = DFT_N(h)` at relative bin `j < M/2 ? j : j − M`, divided by D **and by M**. The /M is there because the
inverse FFT is unnormalized. `hnoise = Σh²`.

**Channel setup**: `bin_hz = fs/N`, `k = nearbyint(offset / bin_hz)` (half to even),
`residual = offset − k·bin_hz`. The input window starts with N − hop zeros.

**Per block** (block index b, `boff = (b·hop) % N`):

1. `X = FFT_N(window)`.
2. Per channel: gather bins `kc .. kc+M/2−1`, then `kc−M/2 .. kc−1` (mod N), multiply by H, then `IFFT_M`.
3. Keep samples `discard .. M−1`. Multiply by `e^{−j2π·kc·boff/N}` (`twiddle[(kc·boff) % N]`, the bin-shift fix).
4. NCO: multiply by `e^{−jφ}`. `φ` advances by `2π·(residual + correction)/48000` per output sample.

**NCO detail**: the step is recomputed only when `correction` changes. Two interleaved phasors each step by
`step²`. The phase in double advances by `step_rad · out_per_hop` per block and is wrapped to `[0, 2π)`. That makes
the output float-close to the golden vectors (5e-5 of RMS), not bit-exact.

**Activity level** (used by the idle gate):

- `level[c]` is the maximum over the chunk's blocks of `mean|y|² / (noise_in · hnoise)`.
- `noise_in` is the 25th percentile of every `FLOOR_STEP` (4) bin, at most `FLOOR_MAX` (512) of them, of a Hann
  spectrum made from the FFT bins: `X[k]/2 − (X[k−1] + X[k+1])/4`. It is scaled by `3.4760595 / (0.375·N)`.
  0.375 is the Hann window's Σw²/N, and 3.476 = 1/(−ln 0.75) turns the 25th percentile of an exponential into its
  mean.
- `noise_in` is taken from the chunk's first block and recomputed every 8th chunk.

**Two-step API** (dual core): `rdof_chz_reserve`, `rdof_chz_forward` (the forward FFTs only, keeping each block's
spectrum), then `rdof_chz_channels(c0, c1, worker)`. Worker 1 has its own inverse FFT plan. Tests:
`test_sdr.c:test_channelizer` (5e-5 of the output RMS, with mid-stream NCO changes) and `test_chz_split`.

**Center planning** (`rdof_plan_center`): usable 0.8 (`PLAN_USABLE`), DC guard 25 kHz
(`PLAN_DC_GUARD`), candidates every 5 kHz. `span = int((2·half − (hi − lo))/2/step)`. Each candidate scores
`min(dc, 50e3) + min(img, 30e3) − |i|`, or −1e6 if `dc < 25e3`. `dc` is the nearest channel's distance from the
center. `img` is the smallest `|oa + ob|` over pairs of channels on opposite sides of the center (the IQ image).
**`rdof_plan_window`** is used when the channels don't all fit. It tries windows edged by each channel
± half the band and ± the DC guard, nudged 1 Hz inward, keeps the window holding the most channels, and breaks
ties with the `rdof_plan_center` score. `rdof_plan_center_band` and `rdof_plan_window_band` are the same with the
usable share and DC guard as arguments (the two above pass 0.8 and 25 kHz), for a front end with another band (the
LR2021). Tests: `test_plan`, `test_window`, `test_plan_edges`.

## 4. FM stage (`sdr/src/fm.c`)

| Constant | Value | Symbol |
|---|---|---|
| samples per symbol | 10 | `fm.h:RDOF_FM_SPS` |
| matched filter taps | 81 (8 symbols) | `fm.h:RDOF_FM_TAPS` |
| filter delay | 40 | `fm.h:RDOF_FM_DELAY` |
| inner block | 256 | `fm.h:RDOF_FM_BLOCK` |

**Discriminator**: `f[n] = atan2(Im(x[n]·conj(x[n−1])), Re(…)) · 48000/(2π)` in Hz. With `RDOF_SDR_FAST_ATAN2`
(`features.h`, default 1), atan2 comes from an octant fold from the float sign bits, a reciprocal by the
`0x7EF311C3` bit trick plus 3 Newton steps, and `atan(s) ≈ s·(((−0.0464964749·s² + 0.15931422)·s² − 0.327622764)·s² + 1)`.
The error is about 2e-4 rad. −0.0 gives −π, as libm's atan2 does.

**Matched filter**: RRC with α = 0.2 and span 8 symbols (the standard formula, with the special cases at t = 0 and
`|4αt| = 1`). It is normalized to unit energy, then to DC gain 1 (`h / Σh`), so the output is in Hz. The built-in
code is a folded symmetric FIR. The power `|x|²` is delayed by 40 samples to line up with it.

**S3 backends** (`dsp_esp.c`, each checked at boot against the built-in code on a test FM signal):

| Backend | Accepted if | Detail |
|---|---|---|
| int16 SIMD (`fir_s3.S:rdof_s3_fir8`) | max error ≤ 2 Hz | input rounded to whole Hz (`+1.5·2^23` trick), taps × 2^17 (`S16_COEF_SHIFT`), output `>> 9` (`S16_OUT_SHIFT`), 8 outputs per call from 8 shifted 96-tap sets (`S16_SET`), history 88 (`S16_H`) |
| `dsps_fir_f32` | max error ≤ 0.05 Hz (tried with the zero padding at either end) | taps padded to a multiple of 4 |
| built-in | otherwise | |

Tests: `test_sdr.c:test_fm` (2 Hz; 0.67 Hz measured with the polynomial atan2), `test_atan2`, `test_filters`
(RRC and channel filter design to 1e-12).

## 5. Demod (`sdr/src/demod.c`)

Input: the matched-filter output `f` (Hz) and power `p`, at 48 kS/s.

### 5.1 Geometry (`demod.c` enum, SPS = 10)

| Symbol | Value | Meaning |
|---|---|---|
| `BURST_SYMBOLS` | 132 | a burst |
| `SYNC_START_SYMBOL` | 54 | first sync symbol |
| `CACH_SYMBOLS` | 12 | |
| `SLOT_PERIOD` | 2880 | 288 symbols (60 ms, one slot pair) |
| `PRE` / `POST` / `CACH` | 540 / 780 / 120 | samples before the sync, from the sync to the end, of CACH |
| `LOOKAHEAD` | 788 | search stops this far before the data end |
| `HISTORY` | 676 | kept before `next_search` |
| `GATE_SPAN` / `GATE_LEAD` | 480 / 120 | squelch window, starting this far before the position |
| `MIN_VAR` | 24·100² | a sync window flatter than ±100 Hz is not scored |
| `RETAIN` / `SLACK_FEEDS` / `RESUM` | 2048 / 8 / 512 | buffer slack, feeds between history moves, squelch re-sum period |

### 5.2 Sync templates

Dibit to level: `01 → +3`, `00 → +1`, `10 → −1`, `11 → −3`, read from each family's 48-bit sync (bit `46 − 2i` is
the top bit of symbol i). There are four families (BS voice/data, MS voice/data). Each template is made zero-mean
and unit-norm (`unit[f][24]`).

### 5.3 Search (`search`)

For each position `r` in `[max(next_search, lbase + PRE + CACH), end − LOOKAHEAD)`, stepping by
`RDOF_DEMOD_SEARCH_STRIDE` (2):

1. **Squelch** (when `squelch_hz > 0`): take the variance of `f` over `[r − 120, r + 360)`, using running sums in
   float that are recomputed every 512 positions. The position passes if `var < 1950²`
   (`demod.h:RDOF_DEMOD_SQUELCH_HZ`).
2. **Prefilter** (`maybe_sync`): sample `v_i = f[r + 10i]` for i < 24. Let `var = Σv² − (Σv)²/24`. The
   position passes if `var > 0.99·MIN_VAR` and some family has `(u_f·v)² ≥ 0.79²·var`. `maybe_sync_keyed` computes
   the same thing with sums over the 15 sign keys (`SYNC_KEYS`), falling back to the loop if the keys don't match
   the families.
3. **Score** (`scores`): cosine `u_f·(v − mean) / ‖v − mean‖` for each family (0 if `var ≤ MIN_VAR`). A position
   becomes a candidate if `max|score| ≥ 0.80`.

Clusters: candidates within SPS of the cluster's first. Take the strongest, re-score `q ∈ [p0 − 1, p0 + 1]`, and keep
the best. It is a sync if `|score| ≥ 0.85` (`RDOF_DEMOD_SYNC_THRESHOLD`); the sign gives the family. For each sync
`sp`: first `fly(sp)` (release flywheel bursts due before it), then `at_sync`. After a decoded burst, or a held
start, skip candidates before `sp + POST + CACH`.

### 5.4 At a sync (`at_sync`)

- Level estimate: the means of the samples at the template's + and − positions give `center` and `outer`. The sync
  is accepted only if `300 < outer < 5000` Hz.
- Slicer: `x − center` against `±(2/3)·outer` and 0, giving +3/+1/−1/−3, sampled every SPS from `sp − PRE`.
- Sync bit errors ≤ `RDOF_DEMOD_MAX_SYNC_BIT_ERRORS` (4).
- Slot: for BS syncs, from the CACH TACT. Otherwise the channel's `slot_hint` (`{0, 0, 1, 2}`), else `guess_slot`:
  the gap from the last burst, rounded to 144-symbol bursts, must be within 30 samples and under 40 bursts; an odd
  count flips the slot.
- `power = 10·log10(mean |x|² over the burst + 1e-12)` dBFS (`log10f`).
- "Weaker": a voice sync whose power over the sync (24·SPS) is more than `RDOF_DEMOD_WEAKER_DB` (15 dB) under the
  slot's held call can't take the slot over.
- Voice: burst A is held pending until B decodes. Data: the slot type is decoded with the color-code filter; a
  non-idle data burst ends the voice track.

### 5.5 Voice flywheel (`fly`, `voice_at`)

- Burst n+1 of a superframe is expected at `anchor + (n+1)·2880`. It needs `next + 1320 + 4 ≤ end` (enough data).
- `voice_at` re-centers twice: slice, then move `center` by the mean slicing error. It tries offsets
  {0, −1, 1, −2, 2, −3, 3}, picks the best by (EMB errors, mean |slicing error|), and stops early when offset 0 is
  clean with a mean error < 0.35.
- 2 misses end the track; so does burst F (n = 5).

Output: the bursts in a feed are sorted by position (stably). `trim` keeps data from `next_search − HISTORY`, or
from the track's next burst − 8 if that is earlier.

**`rdof_sdr_burst_t`**: the burst (33 bytes, data type, slot, voice index, family, color code, errors), `position`
(48 kS/s, at symbol 0's peak + `RDOF_FM_DELAY`), `power_dbfs`, `freq_offset_hz` (= center), `deviation_hz`
(= outer).

**`rdof_demod_skip`** (for the idle gate): feeds n zeros, then re-runs the FM stage over the last 82 samples
(81 taps + 1 for the discriminator) before real input resumes, so the output equals the ungated output.

Tests: `test_sdr.c:test_demod` (162 bursts in nine cases, every field exact; chunks of 331 to 48000) and
`test_e2e`.

## 6. Receiver (`sdr/src/receiver.c`)

**Init** (`rdof_sdr_rx_init`): `D = fs/48000`; `rdof_plan_center` if `center = 0`; the initial correction is
`f·software_ppm·1e-6`. `stride = max_chunk/hop·oph + oph` (per-channel output capacity per chunk). Each channel
gets a demod with `RDOF_DEMOD_SQUELCH_HZ`. `RSSI = round(power_dbfs + rssi_offset)`. The default `rssi_offset` is
`−gain − 25`, with gain 30 when it is auto (`config.c:rdof_sdr_config_rssi_offset`).

**Per chunk**: channelize, then for each channel demod (or skip, below), then `finish`. In `finish`, each tracker
ticks at `t0 + out_index/48000`, then AFC runs in channel order.

### 6.1 Idle gate (`receiver.h`)

A channel skips its demod for this chunk (`rdof_demod_skip`) when all of these hold:

- the gate is on (`idle_gate`, default true) and the chunk has output;
- `level ≤ RDOF_GATE_LEVEL` (3.0) in this chunk and in the last;
- not probing: `(start + c·RDOF_GATE_EVERY/C) % RDOF_GATE_EVERY ≥ RDOF_GATE_PROBE` (every 48000, 4800 long);
- more than `RDOF_GATE_HOLD` (96000, 2 s) since the channel's last burst (every channel runs its first 2 s);
- not in a call (the tracker is busy).

It changes no output.

### 6.2 AFC

| Constant | Value | Symbol (`receiver.h`) |
|---|---|---|
| gain | 0.5 | `RDOF_AFC_GAIN` |
| max step | 300 Hz | `RDOF_AFC_STEP_HZ` |
| limit | ±5000 Hz | `RDOF_AFC_LIMIT_HZ` |
| period | 4800 samples (0.1 s) | `RDOF_AFC_PERIOD` |
| correction history | 32 | `RDOF_AFC_HISTORY` |
| measuring point | 660 (middle of the sync) | `RDOF_AFC_AT` |

Per channel per chunk, over the non-voice bursts (and not inside a span the SDR modem muted):

```
share  = min(1, got / 4800)                       # got: the chunk's outputs
target = mean over bursts of (correction_at(pos + 660) + freq_offset_hz)
step   = clip(0.5 · share · (target − now), ±300 · share)
now    = clip(now + step, ±5000)
```

`correction_at` looks up the correction those samples actually went through, from the last 32 changes. Sign: the
correction is where the signal is relative to its nominal frequency; the NCO rotates by `−correction`.

## 7. ppm learning (`sdr/src/ppm.c`, `sdr_task.c` "the tuner's crystal error")

**Sign** (the RTL convention, as in librtlsdr and SoapySDR): a positive ppm corrects a **fast** crystal. A fast
crystal puts every signal low, so its AFC correction is negative. Per channel: `v = −correction / f · 1e6`.

| Constant | Value | Symbol (`ppm.h`) |
|---|---|---|
| a channel's AFC counts for | 30 s after its last burst | `RDOF_PPM_FRESH_S` |
| agreement | 1.0 ppm of the median | `RDOF_PPM_AGREE` |
| channels needed | 2 | `RDOF_PPM_MIN_CHANNELS` |
| steady band | 0.5 ppm | `RDOF_PPM_STEADY` |
| apply after | 60 s steady | `RDOF_PPM_APPLY_S` |
| first save | 900 s steady | `RDOF_PPM_FIRST_SAVE_S` |
| save every / move | 21600 s / 0.5 ppm | `RDOF_PPM_SAVE_EVERY_S` / `_SAVE_MOVE` (used by `rdof_ppm_should_save` only, which the board doesn't call: it stores into the temperature table, `cal_learn`) |

`rdof_ppm_update` (from the AFC), every `PPM_EVERY_US` (5 s):

1. Take fresh channels. `m = median(v)`, keep those within 1.0 of m, `est = median(kept)`. NaN if fewer than 2.
2. If est is NaN or `|est − residual| > 0.5`: reset `steady = 0`, `residual = est`. Otherwise
   `residual += 0.25·(est − residual)` and `steady += dt`.
3. Return `step = lrint(residual)` once `steady ≥ 60` and `|residual| ≥ 1`, else 0.

Here `residual` is the error left after `chip` (the whole ppm set on the front end), and `learned = chip + residual`.
`rdof_ppm_applied(step)`: `chip += step`, `residual −= step`. `rdof_ppm_measured(ppm)` (from the sample clock):
`est = ppm − chip`, the same steady and step rules, with no averaging.

**Applying a step** (`ppm_tick`): `set_chip_ppm(chip + step)`, then every channel's correction
`+= f_c · step · 1e-6` (`ppm_shift_channels`), so no signal moves. Which source drives it:
`xtal_trusted ? rdof_ppm_measured(xtal) : xtal_ever ? 0 : rdof_ppm_update(AFC)`. Once the sample clock has been trusted
in a run, the AFC is never used again in that run.

**Start** (`ppm_start`, per front-end serial): `chip = lrint(kept)`, where kept is the temperature table's value at
the die temperature (`nc_tcal_get`), else the legacy NVS "learned" record, else the `ppm` setting. The fraction goes
into the channels: `ppm_shift_channels(−(kept − chip))`. The same serial again (a stream restart) resumes the
previous `chip`. With `learn_ppm: false`, the setting is the chip's and nothing is learned (the clock is still
timed for the page).

**NVS** (namespace `rdof_ppm`): `"cal"` = `{serial[64], nc_tcal_t}` (read and written), `"learned"` =
`{serial[64], float}` (read only, from before the table).

**Temperature table** (`netclock_tempcal.h`): `NC_TCAL_N` 24 bins of `NC_TCAL_BIN_C` 2 °C,
`bin = lrint(T/2)·2` clamped to −128..127. `put`: `v += (x − v)/min(n+1, NC_TCAL_WEIGHT=4)`; when full, the least
recently learned bin is evicted. `get`: linear interpolation, clamped to the nearest bin beyond the ends.

- `cal_learn`: when settled, learned not NaN, and `steady ≥ 900 s`, put into the die's bin at most once per bin per
  `CAL_AGAIN_S` (7200 s), then save.
- `cal_follow`: when settled, the clock not trusted, `steady < 60`, and the table differs from the current value by
  ≥ `CAL_FOLLOW_PPM` (0.25), move the chip to `lrint(want)` and the channels to the rest.

## 8. Sample clock (`sdr/src/rate.c`, `sdr_task.c` "the sample clock")

The RTL2832U's samples and the R820T's LO share one 28.8 MHz crystal, so the sample rate gives the LO error.

**Points**: the front end calls `sdr_rate_block(t_us, total_samples)` per USB transfer (RTL `on_data`) or per due
chunk (sim). `x = (t − t0) − (n − n0)·1e6/nominal` (µs). Each `RDOF_RATE_BIN_S` (10 s) bin keeps its minimum x
(the least late transfer). At most `RATE_PENDING` (8) closed bins wait for the fit.

**Fit** (`rdof_rate_point`): least squares over up to `RDOF_RATE_POINTS` (180, 30 min) bins. `ppm = −slope` (µs/s),
`se = sqrt(ss/(n−2)/sxx)`, `rms = sqrt(ss/n)`. It is valid with ≥ `RDOF_RATE_MIN_POINTS` (6) points and
`|ppm| ≤ RDOF_RATE_MAX_PPM` (500). A point more than `RDOF_RATE_STEP_US` (200 µs) off the line means samples were lost:
the fit restarts (`steps++`), and the last estimate is kept until the new fit is valid.

**Crystal estimate** (`rate_tick`):

```
xtal     = fit.ppm − board_ppm + rate_corr          # board_ppm: netclock_rate (the board's crystal)
xtal_err = sqrt(fit.se² + board_err²)               # board_err = skew if learned this run, else 0.5 (RATE_STORED_ERR_PPM)
trusted  = settled && xtal_err ≤ 0.3 (RATE_TRUST_PPM) && fit.span ≥ 300 s (RATE_TRUST_SPAN_S)
```

`rate_corr = fe.sample_ppm()`, the part of the correction the front end applies to its sample rate. It is 0 on the
RTL (`fixed_sample_rate`) and `chip` on sim. Without `netclock_rate`, nothing is trusted. When `set_chip_ppm`
changes `sample_ppm`, the fit restarts fresh. A front end without `sample_ppm` (LR2021) has no sample clock.

**Warm-up / settled** (`settle_tick`, `nc_settle_t`, `NC_SETTLE_N` 16 samples):

- die: `netclock_die_c` within `SETTLE_DIE_C` (3 °C) over `SETTLE_WINDOW_S` (600 s), sampled every
  `SETTLE_EVERY_S` (60 s); a NaN die temperature counts as settled;
- rate (only with a sample clock): `rdof_rate_recent(300 s)` within `SETTLE_RATE_PPM` (0.2) over the same window.

A NaN sample resets the history, and the range is NaN until the window is covered. On becoming settled, the fit is
trimmed to the last 600 s. Until settled: the AFC still moves the chip, but nothing goes into the table and the clock
isn't trusted. Tests: `test_sdr.c:test_ppm`, `test_rate` (a dongle 37.2 ppm fast with jitter, stalls and a silent
drop, within 0.003 ppm), `test_settle`.
