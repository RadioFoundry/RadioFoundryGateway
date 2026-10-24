# SDR transmit: modulator, SDR modem interface, loopback, bench transmit

What: the DMR 4FSK modulator, the SDR modem at interface level, its loopback check, and the bench API's transmit job.
Sources: `sdr/src/{mod,loopback}.c`, `sdr/include/rdof/sdr/{mod,sdrmodem}.h`, `esp_idf/main/{modem_sdr,bench_tx}.c`.
Related: [sdr.md](sdr.md), [sdr-rx.md](sdr-rx.md), [sdr-frontends.md](sdr-frontends.md), [../repeater.md](../repeater.md), [../slot-timing.md](../slot-timing.md), [../bench-api.md](../bench-api.md) §7.

## 1. Modulator (`mod.c`)

| Constant | Value | Symbol (`mod.h`) |
|---|---|---|
| symbol rate | 4800 | `RDOF_MOD_SYMBOL_RATE` |
| deviation per level unit | 648 Hz (so ±1944 Hz at ±3) | `RDOF_MOD_DEVIATION_HZ` |
| pulse taps per output sample | 9 (8-symbol RRC, α = 0.2) | `RDOF_MOD_TAPS` |
| polyphase phases | ≤ 1024 | `RDOF_MOD_MAX_PHASES` |
| ramp | ≤ 6 symbols (default 4, 0.83 ms) | `RDOF_MOD_MAX_RAMP` |
| symbols per segment | ≤ 144 (CACH + burst) | `RDOF_MOD_MAX_SYMBOLS` |
| slot | 144 symbols (30 ms) | `RDOF_MOD_SLOT_SYMBOLS` |
| zero padding | 12 symbols each side | `RDOF_MOD_PAD` |
| I/Q LUT | 256 entries (`LUT_BITS` 8) + second-order step | `mod.c` |

**Geometry**: `P/Q = fs/4800` in lowest terms (P = 60 at 288 kS/s). The pulse table is
`taps[ph][j] = rrc((ph + (j − 4)·P)/P)`, zero beyond ±4 symbols, scaled by `P/Σ`.

**Dibits**: `00 → +1`, `01 → +3`, `10 → −1`, `11 → −3` (`rdof_mod_symbols`). These are the levels the demod's
templates use.

**Segments** (`rdof_mod_queue(start, sym, first, count, flags)`, `rdof_mod_queue_burst(start, burst, cach, flags)`):
`start` is the output sample where symbol 0 peaks; symbol k peaks at `start + k·fs/4800`. Segments must be queued in
start order, before the output reaches the segment's lead-in; otherwise the call returns −1 (late).

```
reach  = max(4, 0.5 + ramp)                                    # symbols
lo     = start + floor((first − reach)·P/Q)
hi     = start + ceil((first + count − 1 + reach)·P/Q) + 1
env    = 1 inside [first − 0.5, first + count − 0.5) symbols; raised-cosine ramps of `ramp` symbols outside
         (RDOF_MOD_RAMP_UP / RDOF_MOD_RAMP_DOWN select which sides)
key_on = start + floor((first − 0.5 − ramp)·P/Q) + 1           # rdof_mod_key_on
```

**Output**: a 32-bit phase accumulator. Each sample's increment is
`base_inc + round_half_away(s·dev_inc)`, with `dev_inc = 648·2^32/fs` and `base_inc` from the carrier offset. `s` is
the shaped level. `rdof_mod_iq` gives `amplitude·env·e^{jφ}` (LUT plus a correction, error < 4e-7 against exact
`exp`). `rdof_mod_freq` gives Hz and the envelope, for a chip with its own FM modulator. With nothing queued the output
is zeros and the phase keeps running. The output doesn't depend on how it is pulled.

**Where the receiver reports it**: a burst whose symbol 0 peaks at TX sample s is reported at 48 kS/s position
`s·48000/tx_fs + RDOF_FM_DELAY (40) + (rx_fs > 48000 ? 8 : 0)`. The 8 is the channelizer's N/8 input delay
(`sdrmodem.c:heard_at`).

Tests: `rdof_mod_tests` (`test_mod.c:test_vectors`: the C against the golden vectors in `host-tests/fixtures/mod-vectors` at 48, 288, 96 (frequency) and
250 kS/s; I/Q within 4e-6, frequency within 0.002 Hz; round trips through the C channelizer and demod, bit-exact at
the predicted positions; `test_just_in_time`: queueing late gives the same samples).

## 2. SDR modem (interface only; `rdof/sdr/sdrmodem.h`)

An emulated MMDVM modem (protocol 1, description `RDOF_SDRMODEM_DESC`, ending in " DCDM", " SPLIT1" appended with
`RDOF_FEATURE_REPEATER`). Dual capacity direct mode (SET_CONFIG `0x80 | 0x40`: both slots on one frequency, timed
from the TS1 / TS2 bursts it hears) follows [radio.md](radio.md) 1.6 and [../repeater.md](../repeater.md) 13.
Duplex, split duplex and slot timing are specified in [../repeater.md](../repeater.md) and
[../slot-timing.md](../slot-timing.md). `sdrmodem.c` is under active work; this section covers only the
boundary.

| Call | Direction | Meaning |
|---|---|---|
| `rdof_sdrmodem_init(m, io, rx_fs, tx_fs, can_full)` | | rx_fs is a multiple of 48000; `can_full` = front end FULL duplex |
| `rdof_sdrmodem_write(m, bytes)` | host → modem | MMDVM frames |
| `io.reply(frame)` | modem → host | replies and DMR_DATA / DMR_LOST |
| `io.rx_channel(hz)` | modem → platform | the receiver channel for an RX frequency, −1 = none (SET_FREQ NAKed) |
| `io.tx_tune(hz, *offset)` | modem → platform | the carrier offset from the center; false = can't |
| `io.now48()` (optional) | modem → platform | the front end's 48 kS/s position now |
| `rdof_sdrmodem_heard(m, ch, burst)` | receiver → modem | each burst the receiver decoded |
| `rdof_sdrmodem_pull_iq(m, pos, iq, n)` / `pull_freq(m, pos, hz, env, n)` | front end → modem | the next n TX samples from sample pos; returns keyed |
| `rdof_sdrmodem_muted_at(m, p48)` | receiver → modem | p within `MUTE_48K` (720, half a slot) of a burst it sent: the receiver skips it for AFC and the host doesn't get it |

Constants: `RDOF_SDRMODEM_QUEUE` 8 bursts per slot; `RDOF_SDRMODEM_LOST_S` 0.5 s (DMR_LOST after a call stops without
a terminator; a data packet's call ends at its last block instead, `sdrmodem.c:data_over`).

**Firmware transport** (`modem_sdr.c`): one mutex covers the modem, so the roamer's writes, the DSP's
`sdr_tx_heard` / `sdr_tx_muted` and the front end's pulls are serialized. `io_tx_tune` accepts
`|hz − center| ≤ rate·usable/2`, in IQ mode only. A modem needs `duplex ≠ RX_ONLY`, `tx_mode = IQ` and `tx_set`.
`tx_fs = caps.tx_rate` or the RX rate. Owners: an `sdr_transceiver` radio (the roamer's or repeater's `rdof_modem`
transport `"sdr"`), or a bench job, one at a time.

## 3. Loopback check (`loopback.c:rdof_sdrmodem_loopback(full, report, cap)`)

Portable; run by `sdrmodem_tests` and the firmware self-test.

| Setting | Value |
|---|---|
| rate / center | `FS` 288000 / 461.000 MHz |
| channels | `CH0` 461.0375, `CH1` 461.0875 MHz |
| chunk | `CHUNK` 4608 samples, for 0.9 s |
| bursts | `NB` 3 CSBK preambles ours, 3 another station's (in the gaps, on CH0) |
| mix | `0.5·(ours + other) + 0.02·noise` (an LCG, a sum of 4 uniforms) |

It checks:

- a real `rdof_modem` client configures it; a frequency the SDR doesn't hear is NAKed; half duplex refuses duplex;
- status shows `space = 8 − NB` (half) and transmitting;
- each of our bursts is heard by the receiver bit-exact, and in half duplex at exactly `sent48[k]`;
- none of ours reaches the host; all of the other station's do; `muted == NB` and the host gets NB (half);
- full duplex: keyed by DMR_START, at least 4 idle bursts sent and heard on CH1, unkeyed at the end;
- exactly one DMR_LOST, and nothing still transmitting.

## 4. Bench transmit (`bench_tx.c`, wire in [../bench-api.md](../bench-api.md) §7)

- `WS /api/bench/v1/tx`, one job at a time. `via=modem` uses the SDR modem (`sdr_modem_bench_open`, refused while
  a roamer owns it); `via=radio:NAME` uses an MMDVM bench radio.
- Policy comes from `bench.tx` in the settings. The lease is checked every 1 s. The arm GPIO is active low
  (armed when low). The job ticks every 20 ms.
- Power: `dBm = tx_power_min + (tx_power_max − tx_power_min)·rf_level/255`.
- Binary (raw I/Q) frames are not supported. A keying log of 8 entries is kept.
