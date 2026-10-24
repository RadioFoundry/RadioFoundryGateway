# SDR device API: a SoapySDR-style plan

Status: a plan (2026-10-24), nothing built. Today's front-end interface is `sdr/include/rdof/sdr/frontend.h`
(`rdof_fe_caps_t`, `rdof_fe_ops_t`; [sdr-frontends.md](sdr-frontends.md)). This plan moves it toward SoapySDR's
device model, so that new radios (RTL-SDR, LR2021, SX1255, and later an Airspy, HackRF or a Pluto on the P4's
High Speed USB) plug in the same way, and a host build can run against SoapySDR itself.

SoapySDR (Boost Software License) and its modules, SoapyRTLSDR among them (MIT,
github.com/pothosware/SoapyRTLSDR @ 5b544e3d), are references for the interface only. SoapyRTLSDR is a thin
wrapper over librtlsdr's API: it carries no register facts, and none of its code is taken here.

## What already matches

| SoapySDR | Ours |
|---|---|
| `getSampleRateRange`, `listSampleRates`, `setSampleRate` | `caps.rate_min/max`, `caps.rates[]`, `ops.tune` (real rate back) |
| `getFrequencyRange`, `setFrequency` | `caps.freq_min/max`, `ops.tune` |
| `setFrequencyCorrection` (ppm) | `ops.set_ppm` (whole ppm) |
| `setGainMode`, `setGain`, `getGainRange` | `ops.set_gain` (NAN: AGC), `ops.gain_steps` |
| `getNativeStreamFormat` | `caps.format` (cu8 / cs8 / cs16), `rdof_fe_convert` to float |
| `activateStream`, `readStream`, `deactivateStream` | `ops.start`, `ops.read` (with filler for lost data), `ops.stop` |
| `getHardwareKey`, `getHardwareInfo` | `caps.kind`, `ops.identity` |
| TX: `setupStream(SOAPY_SDR_TX)`, `writeStream` | `ops.tx_set` with a pull source (I/Q, frequency or 4FSK) |

## What to add, in order

1. **Named gain elements.** `listGains` (for the R820T: `LNA`, `MIX`, `VGA`; the LR2021: its gain step), each
   with a range and step, plus the overall gain that's split across them (`setGain(dir, ch, dB)`). The RTL
   driver now has a measured LNA / mixer table (docs/spec/rtlsdr.md G.3), so a smarter split (LNA first for noise
   figure, mixer next, VGA last) can be tried per element. The software AGC then sees each element.
2. **Settings as key / value.** `getSettingInfo` / `writeSetting` / `readSetting`, typed (bool, int, float,
   string) with a description, so the board's settings page and HTTP API list them without code per device:
   RTL-SDR `biastee`, `direct_samp`, `offset_tune`, `if_filter` (the measured rows), `iq_swap`; LR2021 its mode.
   SoapyRTLSDR's keys are the ones to reuse where they mean the same thing.
3. **Sensors.** `listSensors` / `readSensor`: `lo_locked`, the RTL's ADC clip share, temperature where the chip has
   one, the LR2021's RSSI. Today these are scattered in status JSON per front end.
4. **Register interfaces.** `listRegisterInterfaces` / `readRegister` / `writeRegister` (`RTL_DEMOD`, `RTL_USB`,
   `R820`; `LR2021`), behind a debug setting. The bench measurements (G.2, G.3) needed raw writes; with this the
   board's bench API can do them, and `tools/rtl_probe` isn't the only way.
5. **Stream metadata.** `readStream` with flags and a time: a sample count tied to the netclock / GNSS time, an
   overflow flag where data was filled, and end-of-burst on TX. The slot timing (slot-timing.md) wants this.
6. **Enumeration.** `enumerate(args)` / `make(args)`: each driver registers a `find` and a `make` with key /
   value args (`driver=rtlsdr,serial=...`), replacing `rdof_fe_type_t`'s fixed list. A box can then name more
   than one device of a kind.
7. **Channels.** `getNumChannels` per direction, for dual-receiver chips and full-duplex parts with separate RX and
   TX chains (today's `duplex` caps).
8. **A host shim.** On Linux / macOS, one front end that wraps SoapySDR's C API, so the same receiver runs against
   any Soapy module on a PC (and the RTL driver here can be checked against SoapyRTLSDR + librtlsdr on the bench
   without linking librtlsdr into this project).

Each step keeps `frontend.h` working: new fields and ops are optional (NULL), as `sample_ppm`, `post` and
`spike` are now.
