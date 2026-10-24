# RTL-SDR: the RTL2832U and the R820T / R828D

The facts the RTL-SDR driver (`rtlsdr/src/rtl2832u.c`, `rtlsdr/src/r820.c`) is written from: every register it
writes, the value, what the field means, and where that comes from. Findings are numbered (D1.n for the RTL2832U,
T1.n for the tuner, G.n for what's still to measure) and the code cites them.

## 0. Where the facts come from

- **DS**: Realtek, RTL2832U datasheet, Rev. 1.4 (JATR-2265-11). Cited by table or section ("DS table 27").
- **RD**: Rafael Micro, R820T2 Register Description (the public copy dated December 2016). Cited by register
  ("RD R11"). Its register map (table 1-2) and field list (table 1-3).
- **W**: jtarrio/webrtlsdr (<https://github.com/jtarrio/webrtlsdr>), Apache-2.0, commit
  `9b3f5872750fa119bddd07a2bd4984b4b6716a4c` (2026-10-21), files under `src/rtlsdr/`: `rtlcom.ts`,
  `rtl2832u.ts`, `r8xx.ts`, `r820t.ts`, `r828d.ts`. Cited by file ("W r8xx.ts"). Copyright 2024 Jacobo Tarrio
  Barreiro and 2013 Google Inc.
- **B**: this project's own bench findings, in docs/reference.md ("Why 288 kS/s", "The tuner's ppm, learned").
- **Ours**: a choice made here, with the reason.

The vendor PDFs are not in the repository; local copies go in `docs/spec/vendor.local/` (git-ignored).

Notation: a register write is `reg = value` (whole byte) or `reg[bits] = value` (a masked write: only those bits
change). Demodulator registers are `page:addr` in hex. Tuner registers are `Rn` in decimal with the hex address.

## 1. RTL2832U

### D1.1 Vendor requests

One control transfer per access: bmRequestType 0x40 (write) or 0xC0 (read), bRequest 0 (DS table 27). wIndex
(DS table 28): bits 11:8 the block (0 demodulator, 1 USB, 2 system, 6 standard I2C), bit 4 set for a write, bits
3:0 the page for the demodulator. The driver's hook (`rtl_io_t.control`) takes `in`, wValue, wIndex and the data.

### D1.2 USB and system registers

wValue is the register's full address: the USB (SIE) block at 0x2000 plus the offset (DS table 29), the system
block at 0x3000 plus the offset (DS section 10, table 14). Multi-byte values go little-endian (W rtlcom.ts,
`_numberToBuffer` without big-endian). Registers used:

| Address | Name | Fields (DS) |
|---|---|---|
| 0x2000 | USB_SYSCTL | [0] DMA enable, [3] full packet mode (table 30) |
| 0x2148 | USB_EPA_CTL | [9] FIFO reset, [4] stall (table 32) |
| 0x2158 | USB_EPA_MAXPKT | [10:0] bulk endpoint max packet size, reset 0x40 (table 33) |
| 0x3000 | DEMOD_CTL | [7] demod PLL enable, [6] ADC_I enable, [5] hardware reset (0 = in reset), [3] ADC_Q enable (table 15) |
| 0x3001 | GPO | GPIO output values (table 16) |
| 0x3003 | GPOE | GPIO output enables (table 18) |
| 0x3004 | GPD | GPIO directions, 1 input, 0 output (table 19) |
| 0x300B | DEMOD_CTL_1 | named only ("IR wakeup and low current crystal mode"; table 14); the value is W's |

### D1.3 Demodulator registers

wValue = addr << 8 | 0x20, wIndex = page (| 0x10 to write); multi-byte values big-endian (W rtlcom.ts
`setDemodReg`). DS table 27 says wValue is the register's offset; W's form is the one known to work, and the
driver uses it. After each write W reads one byte with wValue 0x0120 and wIndex 0x000A (page 0x0A, register 1),
and so does the driver (what it's for isn't documented: G.7). A read is the same request with bmRequestType 0xC0
and no 0x10 in wIndex.

### D1.4 The tuner's I2C

- The tuner sits behind the demodulator's I2C repeater: `1:01[3]` IIC_repeat, 1 enable (DS table 3; "should be
  set to 1 before sending a command to the tuner", and it doesn't clear itself after a STOP). W writes the whole
  register: `1:01 = 0x18` to open, `0x10` to close (W rtlcom.ts). The driver opens it around each tuner
  operation and closes it after, also when the operation fails.
- Standard I2C requests: wIndex 0x0610 to write, 0x0600 to read; wValue the tuner's 8-bit write address (DS table
  27 "SetSTDI2C" / "GetSTDI2C"; W rtlcom.ts with 0x34 / 0x74).
- A write's data is the first register's address then the values. W sends at most 7 values a transfer (8 bytes
  with the address) and splits longer writes, each chunk starting at its own register; the driver does the same.
- A read: W first writes the start register (one byte), then reads the bytes. The tuner always answers from
  register 0 (T1.2), so the driver writes 0 and reads registers 0..n-1.

### D1.5 Baseband init (`rtl_open`)

In this order (W rtl2832u.ts `_init`; field meanings from DS where it has them):

| # | Write | Meaning |
|---|---|---|
| 1 | 0x2000 = 0x09 (1 byte) | DMA on, full packet mode (DS table 30) |
| 2 | 0x2158 = 0x0200 (2 bytes), or 0x0040 at Full Speed | bulk max packet size: W writes 512; at Full Speed the endpoint's packets are 64 bytes and the driver writes 64 (B, "The RTL drops samples silently"; DS gives 0x40 as the reset value) |
| 3 | 0x2148 = 0x0210 (2 bytes) | FIFO reset and stall (DS table 32) |
| 4 | 0x300B = 0x22 | W, "powers on demodulator analog circuitry and clock"; DS names the register only |
| 5 | 0x3000 = 0xE8 | demod PLL, ADC_I and ADC_Q on, out of reset (DS table 15) |
| 6 | 1:01 = 0x14, then 1:01 = 0x10 | soft reset of the demodulator, [2] set then cleared (W) |
| 7 | 1:15 = 0x00 | Spec_inv 0, En_aci 0 (DS table 6) |
| 8 | 1:16, 1:17, 1:18 = 0 | carrier frequency offset cleared (W) |
| 9 | 1:19, 1:1A, 1:1B = 0 | pset_iffreq 0 for now (DS table 6); set in D1.6 |
| 10 | 1:1C..1:2F = the FIR (20 bytes, one write each) | D1.9, D1.10 |
| 11 | 0:19 = 0x05 | SDR mode on, digital AGC off (W) |
| 12 | 1:93 = 0xF0, 1:94 = 0x0F | W ("FSM state-holding registers") |
| 13 | 1:11 = 0x00 | en_dagc 0 (DS table 10) |
| 14 | 1:04 = 0x00 | en_if_agc 0, en_rf_agc 0, aagc_hold 0, loop gain 0 (DS table 2) |
| 15 | 0:61 = 0x60 | err_pass 1, mode_PID 1, enable_PID 0 (DS table 13; the PID filter isn't in the SDR path) |
| 16 | 0:06 = 0x80 | opt_adc_iq [5:4] = 0, the default I / Q datapath (DS table 4); bit 7 as W |
| 17 | 1:B1 = 0x1B | en_bbin, en_dc_est, en_iq_comp, en_iq_est all 1 (DS tables 4, 5) |
| 18 | 0:0D = 0x83 | W ("disable 4.096 MHz clock output on TP_CK0") |

Then the tuner is looked for (T1.1), and for a tuner on a low IF (W r8xx.ts `open`):

| # | Write | Meaning |
|---|---|---|
| 19 | 1:B1 = 0x1A | en_bbin 0: IF input, not zero-IF; DC and IQ estimation stay on (DS tables 4, 5) |
| 20 | 0:08 = 0x4D | AD_EN_reg [7] 0, AD_EN_reg1 [6] 1: ADC_Q only, DS 9.1's "IF signal mode"; the low bits as W |
| 21 | 1:15 = 0x01 | Spec_inv 1: the tuner mixes high side, so the spectrum is inverted (DS 9.3; W) |

The crystal is 28.8 MHz (DS 10.2: GPIO1 and GPIO5 latch 0 for 28.8 MHz; W `XTAL_FREQ`).

### D1.6 The DDC's IF

`pset_iffreq = -floor(f_IF / f_crystal × 2^22)`, 22 bits two's complement, at `1:19..1:1B` (DS 9.3, table 6;
the DS's example: 4.57 MHz at 28.8 MHz gives -665554). The bytes: `1:19 = (v >> 16) & 0x3F`, `1:1A = v >> 8`,
`1:1B = v` (W rtl2832u.ts `_setIfFrequency`). f_crystal is the crystal with the ppm correction applied (W).

Ours: the driver rounds to the nearest step (28.8 MHz / 2^22 = 6.87 Hz) instead of flooring, and gives the DDC the
IF the tuner really produces: the PLL's real LO minus the RF center (T1.8), not the nominal 3.57 MHz. The PLL
lands within half its step of the LO asked (about 110 Hz at 461 MHz), and that error would otherwise show as an
offset on every channel and a bias in the learned ppm (0.25 ppm at 461 MHz). To measure: the sign (G.4).

### D1.7 The resampler

`rsamp_ratio = floor(f_crystal / f_out × 2^22)`, register `{1:9F..1:A2}` bits [27:2] (DS 9.4, table 7). W writes
`1:9F` (2 bytes) = ratio >> 16 and `1:A1` (2 bytes) = ratio & 0xFFFF, with the ratio masked to 0x0FFFFFFC, then
soft-resets the demodulator (`1:01` = 0x14, 0x10) (W rtl2832u.ts `setSampleRate`).

- The rates: 225001..300000 and 900001..3200000 S/s. Ratios from 2^27 to 3 × 2^27 (rates 300001..900000) can't be
  written: bit 27 stands for 2^27 + 2^28, so a written ratio from 96 × 2^22 to 128 × 2^22 reads as itself, and
  the 28-bit mask leaves exactly the bits the chip uses (B, "Why 288 kS/s": 480, 528 and 576 kS/s came out at
  232, 243 and 253 kS/s).
- The real rate is `f_crystal × 2^22 / (ratio & ~3)`, from the ratio before the 28-bit mask. 288000, 240000 and
  250000 come out exact at 28.8 MHz (250000 to 0.0004 S/s).
- Ours: the ratio uses the nominal crystal; the ppm correction never changes it (W applies the ppm to the ratio
  too). The rate's only ppm path is D1.8, so `rtl_resampler_ratio(28.8 MHz, rate)` is always what was written and
  the caller can time samples by it.

### D1.8 The sample frequency correction

`1:3E[5:0]` = bits 13:8, `1:3F` = bits 7:0 of `-floor(ppm × 2^24 / 10^6)`, 14 bits two's complement (W
rtl2832u.ts `setFrequencyCorrection`). The DS doesn't document it; its `sfreq_off` read-back has the same scale,
2^-24 (DS table 12, "s(14,24f)").

- B: on an R820T dongle +10 here moved the sample rate by -3.4 ppm, a third of what the formula says. So the driver
  writes 0 when `fixed_sample_rate` is set (the front end sets it), and only the LO and the DDC's IF follow the
  ppm. `rtl_sample_correction_ppm(ppm)` returns what the formula says the register takes out: `floor(ppm × 2^24 /
  10^6) × 10^6 / 2^24` (10 gives 9.954).
- The driver takes whole ppm in -400..400 (the register's 14 bits hold up to about ±488).

### D1.9 The FIR's register layout

`1:1C..1:2F`, 20 bytes, 16 coefficients c0..c15: c0..c7 one signed byte each (`1:1C..1:23`); c8..c15 signed 12-bit,
two to three bytes: for a pair (a, b) the bytes are `a >> 4`, `(a & 0xF) << 4 | b >> 8`, `b & 0xFF`
(`1:24..1:2F`).

Source: W's comment ("8 8-bit taps + 8 12-bit packed taps = 20 bytes") and its default bytes
(`CA DC D7 D8 E0 F2 0E 35 06 50 9C 0D 71 11 14 71 74 19 41 A5`). Unpacked this way they are -54, -36, -41, -40,
-32, -14, 14, 53, 101, 156, 215, 273, 327, 372, 404, 421: they rise smoothly to the last, the first half of a
symmetric low-pass, and twice their sum is 4238, near 2^12. So the filter is taken as 32 taps, symmetric, unity
gain at a sum of 4096. The DS doesn't describe the FIR. To measure: the length and symmetry, and the rate it runs
at (G.1).

### D1.10 The FIR's taps (ours)

Designed by `tools/rtl_fir_design.py` (scipy's Parks-McClellan, 32 taps, symmetric, weight 1 in the passband
and 10 in the stopband, unity gain at DC, rounded): flat from DC to 1.3 MHz (+0.8 dB at most), -3 dB at
1.74 MHz, at least 44 dB down from 3 MHz, if the FIR runs at the ADC's 28.8 MHz.

`c = 3, -18, -31, -49, -68, -81, -80, -59, -13, 58, 151, 259, 370, 471, 546, 587`

- Why that band: the widest rate is 3.2 MS/s and the receiver uses 80% of it (±1.28 MHz). The default's -3 dB
  edge is at 0.043 of the FIR's rate (1.23 MHz at 28.8 MHz); this one's is at 0.060. Whatever rate the FIR really
  runs at, it passes more of the band than the default does, so it can't cut a channel the default wouldn't.
- At 225 to 300 kS/s it is flat to 0.05 dB over the band (the band is under 0.15 MHz from DC). The fixed decimation
  after it removes what's outside the band (B, "Why 288 kS/s").

### D1.11 GPIO and the bias tee

A GPIO n as an output: `GPD[n] = 0` (output), `GPOE[n] = 1` (driver on), then `GPO[n]` the level (DS tables 16,
18, 19; W rtlcom.ts `setGpioOutput`, `setGpioBit`; read-modify-write each). The bias tee is GPIO0, on at 1 (W
`enableBiasTee`). The RTL-SDR Blog V4's HF upconverter is GPIO5, on at 0 (W r828d.ts).

### D1.12 FIFO reset

`USB_EPA_CTL = 0x0210` (FIFO reset and stall), then `0x0000` (DS table 32; W `resetBuffer`). Before streaming.

### D1.13 Standby

`DEMOD_CTL = 0x20`: demod PLL and both ADCs off, out of reset (DS table 15). After the tuner's standby (T1.11).
`rtl_open` brings it back with D1.5's 0xE8.

## 2. R820T / R820T2 / R828D

The register meanings are RD's (the R820T2). The R820T and R828D are taken to share them except where W treats the
R828D differently (T1.12).

### T1.1 Detection

- I2C write address 0x34 for the R820T / R820T2 (RD table 1-1: 0x34 write, 0x35 read; W r820t.ts). 0x74 for the
  R828D (W r828d.ts).
- Register 0 reads 0x96, fixed, as the read mode's check point (RD 1.1, table 1-2). The tuner sends LSB first, so
  the byte as the RTL hands it over is 0x69 (W r8xx.ts `check`: one byte, compared with 0x69).
- With the repeater open, the driver reads one byte at 0x34, then at 0x74: the first to give 0x69 is the tuner. A
  read that fails, or another value, means no tuner there. Neither: `rtl_open` returns -4.

### T1.2 Register access

- Writes: a page write from any register address (RD 1.1, write mode). Registers 0x05 to 0x1F are written;
  0 to 4 are status.
- Reads always start at register 0 and run up (RD 1.1, read mode). Each byte comes LSB first, so the driver
  reverses the bit order of every byte read (W r8xx.ts `_readRegBuffer`); `rtl_read_tuner_regs` returns the
  registers' real values (register 0 is 0x96).
- The driver keeps a copy of registers 0x05..0x1F as written and does masked writes from it (W r8xx.ts
  `_writeRegMask`).
- Status bits used: `R2[6]` the PLL's lock (W `_getPllLock`; RD names R2[6:0] VCO_INDICATOR), `R4[3:0]` the
  filter calibration's result (W `_calibrateFilter`; RD doesn't describe R4).

### T1.3 Init values

Written as one block from 0x05 (in W's chunks: D1.4), then a 5 ms wait (W r8xx.ts `REGISTERS`, `open`). The
fields per RD:

| Reg | Value | Fields (RD) |
|---|---|---|
| R5 0x05 | 0x83 | PWD_LT 1 (loop-through off), PWD_LNA1 0 (LNA on), LNA_GAIN_MODE 0 (auto), LNA_GAIN 3 |
| R6 0x06 | 0x32 | PWD_PDET1 0 (on), PWD_PDET3 0, FILT_3DB 1 (+3 dB), [4] 1 (RD fixed 1), PW_LNA 2 |
| R7 0x07 | 0x75 | [7] 0 (W: mixer lower sideband), PWD_MIX 1 (on), PW0_MIX 1 (normal current), MIXGAIN_MODE 1 (auto), MIX_GAIN 5 |
| R8 0x08 | 0xC0 | PWD_AMP 1 (mixer buffer on), PW0_AMP 1 (low current), IMR_G 0 |
| R9 0x09 | 0x40 | PWD_IFFILT 0 (IF filter on), PW1_IFFILT 1 (low current), IMR_P 0 |
| R10 0x0A | 0xD6 | PWD_FILT 1 (channel filter on), PW_FILT 2, [4] 1 (RD fixed 1), FILT_CODE 6 |
| R11 0x0B | 0x6C | [7] 0, FILT_BW 3 (narrowest), [4] 0, HPF 12 |
| R12 0x0C | 0xF5 | [7] 1 (RD fixed 1), PWD_VGA 1 (on), [5] 1, VGA_MODE 1 (pin), VGA_CODE 5 |
| R13 0x0D | 0x63 | LNA_VTHH 6, LNA_VTHL 3 (about 0.94 V / 0.64 V) |
| R14 0x0E | 0x75 | MIX_VTH_H 7, MIX_VTH_L 5 |
| R15 0x0F | 0x68 | [6:5] 11, CLK_OUT_ENB 0 (clock out on), [3] 1, [2] 0, CLK_AGC_ENB 0 (internal AGC clock on) |
| R16 0x10 | 0x64 | SEL_DIV 3, REF_DIV2 0, [2] 1 (RD fixed 1), CAPX 0 |
| R17 0x11 | 0x83 | PW_LDO_A 2 (2.0 V), [1:0] 11 (RD) |
| R18 0x12 | 0x80 | [7:5] 4 (W: VCO current), PW_SDM 0 |
| R19 0x13 | 0x00 | |
| R20 0x14 | 0x0F | SI2C 0, NI2C 15 |
| R21 0x15 | 0x00 | SDM_IN low byte |
| R22 0x16 | 0xC0 | SDM_IN high byte |
| R23 0x17 | 0x30 | PW_LDO_D 0 (1.8 V, 8 mA), [5:4] 11 (RD fixed), OPEN_D 0 |
| R24 0x18 | 0x48 | [6] 1 (RD) |
| R25 0x19 | 0xCC | PWD_RFFILT 1 (RF filter on), [6:5] 10 (W: polyphase current), SW_AGC 0, [3:2] 11 (RD) |
| R26 0x1A | 0x60 | RFMUX 01 (tracking filter bypassed), [5:4] 10 (W: AGC clock), PLL_AUTO_CLK 00 (128 kHz), RFFILT 00 |
| R27 0x1B | 0x00 | TF_NCH 0, TF_LP 0 (highest corners) |
| R28 0x1C | 0x54 | PDET3_GAIN 5, [2] 1 |
| R29 0x1D | 0xAE | [7:6] 10, PDET1_GAIN 5, PDET2_GAIN 6 |
| R30 0x1E | 0x4A | FILTER_EXT 1, PDET_CLK 10 |
| R31 0x1F | 0xC0 | [7] 1 (W: loop-through attenuation bypassed), [6] 1 |

### T1.4 Init sequence

After T1.3, these masked writes in order (W r8xx.ts `_initElectronics`), with the calibration (T1.5) and the IF
filter (T1.6) between the two parts:

Before the calibration: `R12[3:0] = 0` (VGA code 0, -12 dB), `R19[5:0] = 0x31` (W: VCO band), `R29[5:3] = 0`
(PDET1_GAIN 0).

After the IF filter, as `register = value / mask`:

| # | Write | Field (RD, or W where RD has none) |
|---|---|---|
| 1 | R7 = 0x00 / 0x80 | [7] 0: mixer lower sideband (W) |
| 2 | R6 = 0x10 / 0x30 | FILT_3DB 0 (0 dB), [4] 1 |
| 3 | R30 = 0x40 / 0x60 | FILTER_EXT 1, [5] 0 |
| 4 | R5 = 0x00 / 0x80 | PWD_LT 0: loop-through on |
| 5 | R31 = 0x00 / 0x80 | [7] 0 (W: loop-through attenuation on) |
| 6 | R15 = 0x00 / 0x80 | [7] 0 |
| 7 | R25 = 0x60 / 0x60 | [6:5] 11 (W: polyphase filter current) |
| 8 | R29 = 0xE5 / 0xC7 | [7:6] 11, PDET2_GAIN 5 |
| 9 | R28 = 0x24 / 0xF8 | PDET3_GAIN 2, [3] 0 |
| 10 | R13 = 0x53 / 0xFF | LNA_VTHH 5, LNA_VTHL 3 |
| 11 | R14 = 0x75 / 0xFF | MIX_VTH_H 7, MIX_VTH_L 5 |
| 12 | R5 = 0x00 / 0x60 | PWD_LNA1 0 (LNA on), [6] 0 (W: cable 1 LNA off) |
| 13 | R6 = 0x00 / 0x08 | [3] 0 (W: cable 2 LNA off) |
| 14 | R17 = 0x38 / 0x08 | [3] 1 (W: calibration bias) |
| 15 | R23 = 0x30 / 0x30 | [5:4] 11 |
| 16 | R10 = 0x40 / 0x60 | PW_FILT 2 |
| 17 | R29 = 0x00 / 0x38 | PDET1_GAIN 0 |
| 18 | R28 = 0x00 / 0x04 | [2] 0 |
| 19 | R6 = 0x00 / 0x40 | PWD_PDET3 0 |
| 20 | R26 = 0x30 / 0x30 | [5:4] 11 (W: AGC clock) |
| 21 | R29 = 0x18 / 0x38 | PDET1_GAIN 3 |
| 22 | R28 = 0x24 / 0x04 | [2] 1 |
| 23 | R30 = 0x0D / 0x1F | PDET_CLK [4:0] 13 |
| 24 | R26 = 0x20 / 0x30 | [5:4] 10 |

The sequence is W's as written, intermediate values included (R29, R28 and R26 are each set more than once): its
order may matter to the AGC detectors, and nothing says it doesn't.

### T1.5 The IF filter's calibration

W r8xx.ts `_calibrateFilter`, at most twice:

1. `R11[6:5] = 11` (FILT_BW narrowest), `R15[2] = 1` (W: calibration clock on), `R16[1:0] = 0` (CAPX no cap).
2. The PLL to 56 MHz (T1.8). It must lock, or `rtl_open` fails (-6).
3. `R11[4] = 1`, then `R11[4] = 0` (W: start the calibration; RD shows [4] as a fixed 0).
4. `R15[2] = 0` (clock off).
5. Read registers 0..4: the code is `R4[3:0]`; 15 counts as 0.
6. A code of 0 is taken at once. Otherwise the steps run again and the second result is taken.

### T1.6 The IF and the IF filter, per rate band (ours)

The tuner's IF is 3.57 MHz (W r8xx.ts `IF_FREQ`), high side (T1.9). The IF filter's fields (RD R10, R11):
FILT_CODE (R10[3:0], 0 widest to 15 narrowest, the fine step), FILT_BW (R11[6:5], 00 widest, 01 / 10 middle, 11
narrowest), HPF (R11[3:0], 0 the highest corner, 15 the lowest). RD gives no frequencies for any code. W uses
FILT_BW 11, the calibrated FILT_CODE and HPF 11 at every rate up to 3.2 MS/s (with R10[4] = 1, as RD's fixed 1),
so those pass at least 3.57 ± 1.6 MHz.

The driver picks a row by rate from one table (`r820.c`, `bands[]`) and writes `R10[4:0] = 0x10 | FILT_CODE`,
`R11 = FILT_BW << 5 | HPF` (bits 7 and 4 at 0, as RD's fixed 0):

| Rates (S/s) | IF | FILT_BW | FILT_CODE | HPF | Status |
|---|---|---|---|---|---|
| 225001..300000 | 3.57 MHz | 11 | calibrated + 0 | 11 | interim, to measure (G.2) |
| 900001..3200000 | 3.57 MHz | 11 | calibrated + 0 | 11 | W's |

- The narrow row is W's setting for now. It's the only one known to pass a band, and it passes far more than 300
  kHz. Any narrower code is a guess at frequencies no allowed source gives, and a wrong guess cuts the channels.
  The table takes measured values (G.2) without other changes; FILT_CODE is an offset from the calibrated code,
  clamped to 15.
- The IF stays at 3.57 MHz for the narrow rates. At 300 kS/s the band is 3.42 to 3.72 MHz at the tuner's output:
  far from DC, so the tuner's DC offset and low-frequency noise, and the ADC's own DC, end up 3.57 MHz from the
  center after the DDC, outside the band. The receiver's 25 kHz DC guard stays as it is.

### T1.7 The RF input filters, by frequency

Chosen by the LO frequency (RF + IF): the last row whose start is at or below it (W r8xx.ts `_setMux`). Each tune
writes `R23[3]` (OPEN_D, RD: 0 high-Z, 1 low-Z), `R26[7:6]` (RFMUX, RD: 00 tracking filter on, 01 bypass) with
`R26[1:0]` (RFFILT, RD: 00 highest band, 01 middle, 10 lowest), `R27` (TF_NCH [7:4], TF_LP [3:0]: 0 the highest
corner, 15 the lowest), then `R16[3]` and `R16[1:0]` (CAPX) = 0, `R8[5:0]` (IMR_G) = 0, `R9[5:0]` (IMR_P) = 0.

The rows (W r8xx.ts `STD_MUX_CFGS`; the RTL-SDR Blog V4 uses W r828d.ts `MUX_CFGS_RTLSDRBLOGV4`):

| From (MHz) | OPEN_D | R26 bits | R27 |
|---|---|---|---|
| 0 | 1 | 0x02 | 0xDF |
| 50 | 1 | 0x02 | 0xBE |
| 55 | 1 | 0x02 | 0x8B |
| 60 | 1 | 0x02 | 0x7B |
| 65 | 1 | 0x02 | 0x69 |
| 70 | 1 | 0x02 | 0x58 |
| 75 | 0 | 0x02 | 0x44 |
| 90 | 0 | 0x02 | 0x34 |
| 110 | 0 | 0x02 | 0x24 |
| 140 | 0 | 0x02 | 0x14 |
| 180 | 0 | 0x02 | 0x13 |
| 250 | 0 | 0x02 | 0x11 |
| 280 | 0 | 0x02 | 0x00 |
| 310 | 0 | 0x41 | 0x00 |
| 588 | 0 | 0x40 | 0x00 |

Blog V4: 0 (0, 0x02, 0xDF), 2.2 (1, 0x02, 0xDF), 50 (1, 0x02, 0xBE), 55 (1, 0x02, 0x8B), 60 (1, 0x02, 0x7B),
65 (1, 0x02, 0x69), 70 (1, 0x02, 0x58), 75 (1, 0x02, 0x44), 85 (0, 0x02, 0x44), 90 (0, 0x02, 0x34),
110 (0, 0x02, 0x24), 112 (1, 0x02, 0x24), 140 (1, 0x02, 0x14), 172 (0, 0x02, 0x14), 180 (0, 0x02, 0x13),
242 (1, 0x02, 0x13), 250 (1, 0x02, 0x11), 280 (1, 0x02, 0x00), 310 (1, 0x41, 0x00), 588 (1, 0x40, 0x00).

### T1.8 The PLL

RD R16, R20..R22:

- `Nint = 4 × NI2C + SI2C + 13`; `Nfra = SDM_IN / 2^16`; the divider `Ndiv = (Nint + Nfra) × 2`;
  VCO = Ndiv × f_ref. With REF_DIV2 = 0, f_ref is the crystal. RD suggests the half reference for crystals over
  24 MHz; W uses the whole 28.8 MHz and that's what is known to work, so the driver does too.
- LO = VCO / 2^(SEL_DIV + 1) (W `_setPll`; RD's list starts the same: 000 /2, 001 /4, 010 /8).
- SEL_DIV = floor(log2(1.77 GHz / LO)), clamped to 0..6 (W), which keeps the VCO from 1.77 to 3.54 GHz. Nint
  above 63 is refused (W); so is Nint below 13 (RD's formula has no smaller value).
- Ours: SDM_IN is the fraction rounded to the nearest 2^-16 (W truncates); a fraction that rounds to 1 carries
  into Nint. The step is 2 × f_ref / 2^16 / 2^(SEL_DIV+1): 220 Hz at 461 MHz (SEL_DIV 1).
- `R18[3]` (PW_SDM) = 1 when SDM_IN is 0, else 0 (W).
- Sequence (W `_setPll`, `_getPllLock`): `R26[3:2] = 00` (PLL_AUTO_CLK 128 kHz); R16..R22 as one block write with
  REF_DIV2 0, SEL_DIV, `R18[7:5] = 100` (W: VCO current), PW_SDM, SI2C / NI2C and SDM_IN (W writes the first two as
  separate writes before the block; ours folds them in, same values); then read registers 0..2 for `R2[6]`, up to 4
  times, waiting 5, 10, 15 and 20 ms after each miss; still unlocked: `R18[7:5] = 011` and the 4 reads again; still
  unlocked: the tune fails (-5). Locked: `R26[3:2] = 10` (8 kHz, RD).
- Not done: W reads `R4[5:4]` before choosing SEL_DIV and moves SEL_DIV one step when it differs from a reference
  (2 for the R820T, 1 for the R828D), without changing the multiplier it already worked out. The driver skips this
  (G.5).
- The crystal is the tuner's nominal 28.8 MHz times (1 + ppm / 10^6) (W `setXtalFrequency`, from the RTL's
  corrected crystal). A positive ppm means the crystal runs fast.

### T1.9 Tuning

LO = RF + IF (W `setFrequency`: the tuner mixes high side), with `R7[7] = 0` (T1.4). Each tune: the input filters
(T1.7) for the LO, then the PLL (T1.8), then the R828D's input (T1.12). The DDC then gets the IF that results
(D1.6). The driver takes 24 MHz to 1766 MHz (the front end's range). Nint 13..63 would allow LOs from about 5.9
to 1843 MHz; 24 to 1766 MHz keeps the VCO in W's 1.77 to 3.54 GHz at every SEL_DIV; past that is G.9.

### T1.10 Gain

Fields (RD): `R5[4]` LNA_GAIN_MODE (0 auto, 1 manual), `R5[3:0]` LNA_GAIN (0 min, 15 max); `R7[4]` MIXGAIN_MODE
(0 manual, 1 auto), `R7[3:0]` MIX_GAIN (0 min, 15 max); `R12[4]` VGA_MODE (0 the code, 1 the pin), `R12[3:0]`
VGA_CODE (-12 dB at 0, +3.5 dB a step: 16 dB at 8, 26.5 dB at 11).

- Manual (W `setManualGain`): LNA manual, mixer manual, VGA manual at code 8. W's steps go LNA, mixer, LNA,
  mixer: step k (0..30) is LNA_GAIN = ceil(k / 2), MIX_GAIN = floor(k / 2). The driver writes `R5[4:0]`,
  `R7[4:0]` and `R12[4:0]` once each (W writes the mode bits and the codes separately).
- The dB per step: W says "experimentally, LNA goes in 2.3 dB steps, Mixer in 1.2 dB steps" and counts 3.5 dB
  for each full step (LNA and mixer), 2.3 for the LNA's half step. So step k is nominally
  `floor(k / 2) × 3.5 + (k odd ? 2.3 : 0)` dB: 0, 2.3, 3.5, 5.8, 7.0 ... 49.0, 51.3, 52.5. These are relative,
  without the VGA, and **to measure** (G.3). They live in one table (`r820.c`, `gain_steps[]`: tenths of a dB,
  LNA, mixer and VGA codes per step), so a measured one drops in.
- `rtl_set_tuner_gain` takes the highest step at or below the ask (the lowest if the ask is below it).
- AGC (W `setAutoGain`): LNA auto, mixer auto, VGA manual at code 11 (26.5 dB).
- `R12[7]` stays 1 (RD: fixed 1). W's gain writes clear it (mask 0x9F); the driver's masks leave it.

### T1.11 Standby

Whole-register writes (W r8xx.ts `close`): R6 = 0xB1, R5 = 0xB3, R7 = 0x3A, R8 = 0x40, R9 = 0xC0, R10 = 0x3A,
R12 = 0x35, R15 = 0x68, R17 = 0x03, R23 = 0xF4, R25 = 0x0C. By RD: loop-through, LNA, mixer, mixer buffer, IF
filter, channel filter and RF filter off, the VGA off, the analog and digital PLL regulators off.

### T1.12 The R828D's inputs

W r828d.ts, by the RF frequency asked:

- A plain R828D: above 345 MHz `R5[6:5] = 00` (the air input), else `R5[6:5] = 11` (cable 1 LNA on).
- The RTL-SDR Blog V4: below 28.8 MHz the HF input: the tuner is set 28.8 MHz higher (the board's upconverter),
  `R6[3] = 1` (cable 2 LNA on), `R5[6:5] = 01`, GPIO5 = 0 (upconverter on). From 28.8 to 250 MHz: `R6[3] = 0`,
  `R5[6:5] = 11`, GPIO5 = 1. From 250 MHz: `R6[3] = 0`, `R5[6:5] = 00`, GPIO5 = 1. Written only when the input
  changes.
- RD shows R5[6] and R6[3] as fixed 0 on the R820T2; the R820T driver never sets them.

### T1.13 The tuner's crystal

28.8 MHz for both chips: the RTL's clock (W uses 28.8 MHz for every tuner; rtl.h: the Blog V4 runs its R828D on
the 28.8 MHz clock). A plain R828D dongle with its own crystal isn't covered by any allowed source (G.6).

## 3. Gaps and measurements

What no allowed source settles. Each is marked in the code where it's used.

- **G.1 The FIR's rate, length and symmetry.** Interim: 32 taps, symmetric, at 28.8 MHz (D1.9, D1.10). Measure:
  noise in (the input terminated, gain up), the FIR set to a unit pair (c15 = 2048, the rest 0) and then to ours,
  at 288 kS/s and 2.4 MS/s; the output spectrum's shape against `tools/rtl_fir_design.py`'s response gives the
  rate.
- **G.2 The IF filter in Hz.** Measured: R1.3; the narrow row now uses it (T1.6).
- **G.3 The gain steps in dB.** Measured: R1.2; `gain_steps[]` now holds it (T1.10).
- **G.4 The sign of the IF correction.** A CW carrier exactly at the center frequency should come out at 0 Hz
  within 7 Hz (the DDC's step) at 100, 461 and 900 MHz. If it comes out at twice the PLL's error instead, the sign
  in D1.6 is backward. Measured: right (R1.1).
- **G.5 The VCO fine-tune check.** Read `R4[5:4]` after tuning across the range (the board's register dump has
  it); if it ever differs from 2 (R820T) or 1 (R828D), find out what W's divider step does there. R820T: never
  differs (R1.4); the R828D is still to check.
- **G.6 A plain R828D's crystal** (not a Blog V4). A known carrier on such a dongle gives it.
- **G.7 The read after each demodulator write** (D1.3): what page 0x0A register 1 is. Harmless; kept as W does.
- **G.8 The sample frequency correction** (D1.8): B found a third of the formula on one R820T dongle. Another
  dongle, and the R828D, would say whether it's the chip.
- **G.9 PLL lock at the ends of the range** (24 MHz, 1766 MHz, and around each SEL_DIV change at 1.77 GHz /
  2^n): tune each and read the lock bit and the carrier. R820T: all lock (R1.4).

### Measured (R1.n)

All on one RTL2832U + R820T dongle (0bda:2832, "RTL2832U", filter calibration code 6) on the lab bench's Linux
host, 2026-10-24, with `tools/rtl_probe` over usbfs; bias tee on where an antenna was used.

- **R1.1 Tuning (G.4).** The carrier used is the dongle's own crystal harmonic (16 x 28.8 = 460.8 MHz, and 4x, 8x,
  15x, 30x, 42x), which no crystal error moves relative to the receiver. Over 12 centers 2 kHz apart with PLL
  errors from -104 to +97 Hz, the harmonic came out at -50 to -59 Hz: the IF correction's sign is right (a
  backward one would scatter it by twice the PLL error). The constant part scales with frequency: -14 Hz at
  115.2 MHz, -27 at 230.4, -29 at 432, -54 at 460.8, -60 at 864, -120 at 1209.6, about -0.1 ppm. Its source
  isn't known; the receiver's AFC takes it into the learned ppm. A ppm of +5 moved the harmonic +2250 Hz
  (expected +2304, less the same -0.1 ppm).
- **R1.2 Gain steps (G.3).** A CW carrier (the crystal harmonic, 50 kHz off center, 288 kS/s, VGA code 8): each
  LNA code with the mixer at 0, then each mixer code with the LNA at 0. Cumulative dB from code 0, averaged over
  230.4, 460.8 and 864 MHz:
  LNA 0, 3.2, 6.3, 9.9, 12.4, 13.7, 16.8, 19.5, 22.3, 23.8, 25.3, 26.4, 27.2, 28.3, 29.8, 29.6 (26 dB in all at
  864 MHz, 31.5 at the others);
  mixer 0, 1.3, 2.7, 4.2, 5.4, 6.8, 8.3, 9.6, 11.1, 12.5, 13.8, 15.0, 16.4, then nothing more from 13 to 15.
  The noise floor rose with the carrier (within 1 to 5 dB): the antenna's noise, not the ADC's, set it. The
  table built from these (LNA and mixer alternating, steps under 0.3 dB dropped) was checked by setting each
  step: within 2 dB of the measured carrier up to 29 dB, where the ADC started clipping on other signals.
- **R1.3 The IF filter (G.2).** The input on the antenna at 600 MHz, gain 35 dB, 2.4 MS/s, the DDC's IF moved
  over 1 to 7 MHz in 1 MHz steps to see the tuner's whole output; each setting against FILT_BW 0, code 0, HPF 11
  in 0.5 MHz slices. The low-pass hardly narrows: only FILT_BW 3 with codes 12 to 15 (and FILT_BW 1 / 2 with 12
  to 15) cut, from about 5.8 MHz, -7 to -9 dB by 7 MHz; every other setting is flat to 7 MHz. The HPF moves the
  lower corner: code 0 cuts to about 4 MHz (-7 dB at 3.5 MHz, so it hurts the 3.57 MHz IF), 2 to about 2.7 MHz,
  4 to 2.2, 7 to 1.8, 10 to 1.2, 11 and up below 0.5 MHz. Nothing comes near a 300 kHz passband: the narrowest
  useful one around a 3.57 MHz IF is about 2.7 to 5.8 MHz (HPF 2, FILT_BW 3, code 15), which the narrow row now
  uses. Slopes are shallow (under 10 dB a MHz past the edge).
- **R1.4 Lock (G.5, G.9).** Every MHz from 24 to 1766 locked, and `R4[5:4]` read 2 each time.
- **R1.5 Against librtlsdr on the same dongle** (SoapySDR / librtlsdr through the bench's own receiver, 461.52875
  MHz, 1.536 MS/s, lowest gain, bias tee on): noise floor within 2 dB, the crystal harmonic the same level
  (+24.9 / +24.8 dB over the floor), the same flat band, and a DC spike 12 dB lower here (+17.9 / +29.8 dB). DMR
  bursts from a bench radio at 461.1375 MHz (288 kS/s, center 461.1875) decoded 11 of 11 with no FEC failures,
  though every burst reached the ADC's rails at the lowest gain.
  Our own receiver (`rdofsdr`) on the same capture: 11 bursts, 0 bit errors, one call.
- **R1.6 The VGA below code 8.** The same carrier with the LNA and mixer at 0: -20.5 dBFS at VGA code 8, -42.6 at
  code 0 (22 dB less; RD's 3.5 dB a step would say 28), the floor 13 dB under it. The gain table stops at VGA 8;
  steps below it would give a strong-signal site about 20 dB more room before the ADC clips, for less SNR.

### Measuring the IF filter (G.2)

The input terminated in 50 ohms, the gain at its top step, the noise floor is then the IF filter's shape.

1. At 2.4 MS/s (the wide row, the widest view), for each FILT_BW (0..3), FILT_CODE (0..15) and HPF (0..15), write
   the codes through a bench hook (the board's register writes), wait 100 ms, and average 1 s of power spectrum.
2. Read the -3 dB edges of the floor against the flat middle. The output is centered on the IF, so an edge at
   +x Hz is the LPF at 3.57 MHz + x, an edge at -x Hz the HPF at 3.57 MHz - x (spectrum inverted: check which side
   is which with a carrier).
3. Edges outside ±1.2 MHz need a lower IF for the sweep: move the DDC's IF (D1.6) by a known amount and the band
   with it.
4. From the table of edges choose, for 225..300 kS/s, the narrowest FILT_BW / FILT_CODE / HPF whose passband still
   covers IF ± 150 kHz with 1 dB to spare at 461 MHz and at the ends of the range. Put it in `bands[]`.

### Measuring the gain steps (G.3)

1. A CW carrier from a signal generator at a known level (-60 dBm), 461 MHz, 50 kHz off the center; 288 kS/s.
2. For each LNA code 0..15 with the mixer at 0, then each mixer code 0..15 with the LNA at 15 (the VGA at code
   8), read the carrier's level in the FFT (dBFS), backing the generator off wherever the samples clip.
3. Each step's gain is the difference from the previous one. Repeat at 150 MHz and 900 MHz.
4. Build `gain_steps[]` from them: ordered by total gain, in tenths of a dB from the lowest step, with each row's
   LNA, mixer and VGA codes. The VGA steps (3.5 dB each, RD) can join the table if the LNA and mixer alone don't
   reach far enough.
