# SDR front ends, the resampler, the spectrum and the I/Q history

What: the front-end interface (ops, caps), each front end (RTL-SDR, sim, LR2021), the rational resampler, the display spectrum and waterfall, and the I/Q pre-roll history.
Sources: `sdr/include/rdof/sdr/frontend.h`, `sdr/src/{frontend,resample,spectrum,iq}.c`, `esp_idf/main/{fe_rtlsdr,fe_sim,fe_lr2021,sdr_task,bench_api}.c`, `rtlsdr/src/{rtl2832u,r820,gaps}.c`.
Related: [sdr.md](sdr.md), [sdr-rx.md](sdr-rx.md), [sdr-tx.md](sdr-tx.md), [../lr2021-iq.md](../lr2021-iq.md), [../bench-api.md](../bench-api.md).

## 1. Interface (`frontend.h`)

`device.type` picks the front end: `rtlsdr` (the default), `lr2021`, `sim`, or `sx1255` (parsed, not written).
`"sim": true` forces `sim` (`config.c:rest`).

**Enums**: `rdof_fe_format_t` is CU8 (zero at 127.5), CS8 (/128) or CS16 (/32768). Duplex is `RX_ONLY`, `HALF` or
`FULL`. TX mode is `NONE`, `IQ`, `FREQ` or `4FSK`. The gap filler is byte `0x80` for CU8, else 0.

**`rdof_fe_caps_t`**:

| Field | Meaning |
|---|---|
| `kind` | name |
| `format` | the sample format read |
| `rate_min` / `rate_max`, or `rates[]` | the rates it takes |
| `freq_min` / `freq_max` | the tuning range (Hz) |
| `usable` | the share of the rate that is usable band |
| `dc_guard_hz` | how far channels stay off the center |
| `gain` | it has gain steps |
| `usb` | it sits on USB |
| `duplex`, `tx_mode`, `tx_format`, `tx_rate` (0 = the RX rate) | transmit |
| `tx_freq_min/max`, `tx_power_min/max/step` (dBm) | transmit range and power |
| `turnaround_us` | RX/TX switch time |

**`rdof_fe_ops_t`**:

| Op | Contract |
|---|---|
| `init(ctx)` | 0 or < 0 |
| `wait(ctx, ms)` | wait for a device to appear |
| `tune(ctx, rate, center, ppm, *got_rate)` | set everything; return the rate actually delivered |
| `set_ppm(ctx, ppm)` | whole ppm, the RTL convention (+ corrects a fast crystal) |
| `set_gain(ctx, dB)` | NaN = the device's own AGC |
| `gain_steps(ctx, out, max)` | the gain table (for `agc.c`) |
| `start` / `stop` | stream on / off |
| `read(ctx, out, want, ms, *filled)` | bytes, with `*filled` bytes of gap filler among them |
| `gone`, `stats`, `identity`, `error` | unplugged; counters; the serial (key for ppm learning); last error |
| `sample_ppm` (optional) | the ppm the correction takes out of the sample rate; absent = no sample clock |
| `post(ctx, x, n)` (optional) | runs on the float samples after conversion (sim: offset and loopback) |
| `spike` (optional) | USB throughput test |
| `tx_set(ctx, src, dBm)` (optional) | start or stop pulling transmit samples from `rdof_fe_tx_source_t {pull_iq, pull_freq, ctx}` |

`rdof_fe_check` refuses, when settings are saved, a rate or a channel outside the caps, with a message.

## 2. Front ends

| | `rtlsdr` (`fe_rtlsdr.c`) | `sim` (`fe_sim.c`) | `lr2021` (`fe_lr2021.c`, first run on the W12 2026-10-23) |
|---|---|---|---|
| format | CU8 | CU8 | CS16 |
| rates | 240000 to 3200000 | 48000 to 3200000 | 48000 to 240000 (after resampling) |
| tuning | 24 to 1766 MHz | 0 to 6 GHz | 150 to 960 MHz |
| usable / DC guard | 0.8 / 25 kHz | 0.8 / 25 kHz | 2·(native/8 − 7.6 kHz)/rate (±8 kHz at 125 kS/s) / 5 kHz |
| gain | the tuner's table, from tenths of a dB | none | steps 1 to 13 (no dB scale) |
| duplex / TX | RX only | FULL, TX IQ, CS16, `tx_rate` 0, −40 to 20 dBm step 1 | HALF, TX NONE; −9.5 to 22 dBm step 0.5; turnaround 107 µs |
| `sample_ppm` | 0 (`fixed_sample_rate`) | `chip` | none |
| sample clock points | per USB transfer (`on_data`) | per chunk, at its due time | none |
| `post` | — | rotation by the simulated offset, plus the loopback | — |

### 2.1 RTL-SDR

- Driver: `rtlsdr` (our own, written from the fact spec [docs/spec/rtlsdr.md](../rtlsdr.md); see
  [reference.md](../../reference.md), "The RTL-SDR driver: where its facts come from"). The crystal `RTL_XTAL_HZ`
  is 28.8 MHz (`rtl.h`), for the RTL and for both tuners (spec T1.13).
- Resampler ratio (spec D1.7): `ratio = floor(xtal·2^22/rate) & ~3`, written masked to 28 bits (`0x0FFFFFFC`); the
  chip reads bit 27 as 2^27 + 2^28, so the real rate is `xtal·2^22/ratio` from the unmasked ratio
  (`rtl2832u.c:rtl_resampler_ratio`). The ratio always uses the nominal crystal.
- ppm (spec D1.6, D1.8): the tuner's crystal is `28.8 MHz·(1 + ppm/1e6)` for the PLL, and the DDC's IF register is
  `−round(IF·2^22/xtal)` with the IF the PLL really gives (LO minus the center). The sample-frequency correction
  register would be `−floor(ppm·2^24/1e6)`, but the front end sets `fixed_sample_rate`, so it writes 0: the
  correction moves only the LO and IF, and the samples run on the bare crystal (what the sample clock needs).
- USB: a ring of `CONFIG_RDOF_SDR_RING_KB` (256) KB in PSRAM. The reader task runs on core 0 at priority 18.
  `usb {transfers, transfer_kb}` defaults to 8 × 8 KB. A dropped transfer becomes filler up to `gap_fill_ms` 250 ms
  (`rtlsdr/gaps.h`: `RTL_GAPS_MAX` 16 gaps, back-to-back drops merged), so the timeline holds. Bias tee from
  `device.settings.biastee`.

### 2.2 sim

The self-test clip (`selftest/clip.cu8`, `CLIP_RATE` 288 kS/s at `CLIP_CENTER`; channels 13, 4, 6, 8 and 10 of the
461 MHz plan) is looped, paced against `esp_timer`:

```
chunk_us = 1e6 · (want/2) / (rate · (1 + (sim_ppm − chip)·1e-6)) / (1 + board_ppm·1e-6)
post: x · e^{j·2π·off·n/rate}, off = −center · (sim_ppm − chip) · 1e-6, then x += 0.25 · tx_pull_iq(n)
```

A chunk more than 0.5 s late is skipped and counted in `overflows`. `sdr_rate_block` is stamped at the due time,
not the read time.

### 2.3 LR2021

The raw I/Q path is per [../lr2021-iq.md](../lr2021-iq.md). `pick(rate)` chooses one of two modes:

- **Narrow**, rates up to 120000: the LoRa modem at BW_31, route source 2, int16 I/Q at 125 kS/s (proven
  continuous), resampled with `rdof_resamp` (K = 32, passing 0.8 of the rate). The chip's own filter passes only the
  LoRa bandwidth (R2.10): one DMR channel a tune.
- **Wide**, above 120000 up to 576000: the FSK modem with rx_bw = the rate code from `lr2021_conv_rx_bw_for(rate,
  0.8)`, route source 0 through the chip's rate converter (`lr2021_cmd_raw_route_rate`), which makes exactly the
  settings' rate (R2.19): int8 I/Q, no resampling, shifted to int16. The code is the fastest in the datasheet's
  table 11-4 whose channel filter (0.37 of its rate) stays within 0.8 of the settings' rate: 1 MS/s into 576000
  gives about 355 kHz of channels.

`caps.usable` is set from the rate in `init` (`band_usable`). The planner takes the front end's band and DC guard
(`rdof_plan_center_band`, `rdof_plan_window_band`).

The FIFO is 1 KB (the size field's limit). A task on core 0 at priority 24, above Wi-Fi, reads it: on the RX FIFO
IRQ (DIO7, at 768 bytes narrow, 512 wide) one DMA read of that many bytes, then, in the wide mode, by the level until
it's under half the IRQ's level, since data arriving during a read can pass the threshold before the re-arm and the
FIFO-high flag then never rises again. The loss flags are checked every 8 KB. Raw bytes go into a PSRAM ring;
`read()`, on the DSP's core, converts (and in the narrow mode resamples). A loss, a retune or a ppm change ends the
session and starts a new one. A board's crystal error goes in `device.ppm` (the W12: −28).

The W12's GC1109 front end is driven by DIO masks (`CONFIG_RDOF_LR2021_DIO{9,10,11}_{PA,BYPASS}`): receive is CSD
high, CTX low (its LNA: there is no receive bypass); transmit CTX high, with CPS high for the PA inside 860–930 MHz,
low (the transmit bypass) outside. On the W12 a 902–928 MHz SAW follows the LNA, so 461 MHz arrives about 46 dB down.

## 3. Rational resampler (`resample.c`, `rdof/sdr/resample.h`)

`L/M` = out/in reduced by their gcd, each ≤ 1000. The prototype has N = L·K taps (K per phase: 32 for the
channelizer's 0.8, as few as 10 for a band of 0.4), Kaiser β = 7.857. The cutoff is halfway between `usable·out/2`
and `min(in, out) − fp`, normalized at `L·in`. The gain is L. Each phase is stored reversed, `h[(n % L)·K + K − 1 −
n / L]`, and the history split into I and Q, so an output is two contiguous dot products, with four running sums (one
sum waits on each add: about 140 cycles an output on the S3). The phase step is carried without a divide.
esp-dsp's `dsps_dotprod_f32` was tried and was slower at these lengths (its call cost). Measured: flat to 0.001 dB over the usable band, aliases under −60 dB
(`test_sdr.c:test_resample`); on the S3, 135.6 cycles an output at 10 taps, 283 at 32 (kbench).

## 4. Display spectrum and waterfall (`spectrum.c`, `sdr_task.c`)

| Item | Value |
|---|---|
| window | periodic Hann `0.5 − 0.5·cos(2πi/n)`; power normalized by `1/(Σw)²` |
| FFTs per chunk | up to `RDOF_SPECTRUM_FFTS` (4), spread across the chunk so 30 ms bursts don't strobe; power averaged over every FFT of the row's `per_row` chunks |
| bins | `SPEC_BINS` 512 |
| byte per bin | `clamp(−20·log10(p), 0, 255)`, which is −2 × dBFS |
| order | FFT-shifted: lowest frequency first, DC at bin n/2 |
| waterfall | `SPEC_ROWS` 64 rows, one per `lrint(0.2·rate/CHUNK)` chunks (about 200 ms); each row records `spec_sc`, the wideband sample count |
| running | only while viewed in the last 10 s |

The wire side (`GET /spectrum`, WS streams) is in [../bench-api.md](../bench-api.md) §6.1. Tests:
`test_sdr.c:test_spectrum`.

## 5. I/Q history and pre-roll (`bench_api.c`, `hist_write`, `bench_on_chunk`)

Called from the DSP task for each chunk:

| Item | Value |
|---|---|
| ring length | `HIST_S` 2.0 s; pre-roll offered up to `PREROLL_S` 1.0 s (the rest is slack for a slow reader) |
| channels | cs16, 48 kS/s, `sat16(y·32767)` (truncated, then clamped to ±32767), `[C][HIST_CH_N = 96000][2]` |
| wideband | cs8, `sat8(x·127)`, `HIST_S·fs` samples |
| memory | PSRAM; channel rings only if more than `HIST_KEEP_FREE` (1.5 MB) would stay free |
| when | only while warm: an I/Q stream opened within `HIST_WARM_US` (600 s); the kept samples start where warming began (`hist_*_from`) |
| reallocation | on each new run (retune) |

A stream's pre-roll goes out ahead of its live frames, introduced by a `hist_marker_t` ("HIST", from, count, post,
t0, and a trigger's rise, level and floor), flagged `IQ_FLAG_PREROLL` (32). The wire format and the trigger are in
[../bench-api.md](../bench-api.md) §6.2.
