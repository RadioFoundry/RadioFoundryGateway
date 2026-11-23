# rust

The gateway's C ported to Rust a module at a time. Each module exports its C header's functions under the same
names and signatures and links in place of its `.c` file, so the C that calls it doesn't change.

```sh
cmake -S host-tests -B build/host && cmake --build build/host   # prints "cargo: rust for rdof_rust"
cmake -S host-tests -B build/host-c -DRDOF_RUST=OFF              # the C modules instead
```

- **The Rust is what runs, by default:** in the firmware (`CONFIG_RDOF_RUST`, default `y`), the host tools and every
  host test (`common/CMakeLists.txt`). A cross build needs `RDOF_RUST_TARGET`.
- **A build that lacks cargo, the toolchain or the target stops,** lists what's missing and names the switch for the
  C: the `sdkconfig.defaults.c` overlay for the firmware, `-DRDOF_RUST=OFF` for the host.
- **The C twins are kept for now:** for a build without a Rust toolchain, and for the feature-off builds (the Rust
  needs every `RDOF_FEATURE_*` on). `rdof_tests_c` runs the core tests on them.
- **A change to one of these modules** goes in the Rust, and in the C while it's kept.
- **A new module** goes in `RDOF_RUST_MODULES` in `rust.cmake` and in `src/lib.rs`.

| Module | Replaces |
|---|---|
| `ambe.rs` | `common/src/ambe.c` |
| `benchcfg.rs` | `common/src/benchcfg.c` (numbers in strings through the C library's `strtol`) |
| `benchtx.rs` | `common/src/benchtx.c` (fixed-point numbers through the C library's `snprintf`) |
| `bits.rs` | `common/src/bits.c` |
| `config.rs` | `common/src/config.c` (`strtod`, `strtol`, `rint` and `%g` through the C library) |
| `hblink.rs` | `common/src/hblink.c` |
| `jitter.rs` | `common/src/jitter.c` (`ceil` and `rint` through the C library) |
| `json.rs` | `common/src/json.c` (floats through the C library's `snprintf`) |
| `jsonp.rs` | `common/src/jsonp.c` (numbers through the C library's `strtod`) |
| `lastheard.rs` | `common/src/lastheard.c` (a frequency's text through the C library's `sscanf` and `llround`) |
| `layer2.rs` | `common/src/layer2.c` |
| `mmdvm.rs` | `common/src/mmdvm.c` |
| `modem.rs` | `common/src/modem.c` (`rint` and `%.5f` through the C library) |
| `netcalls.rs` | `common/src/netcalls.c` |
| `oled.rs` | `common/src/oled.c` |
| `parrot.rs` | `common/src/parrot.c` (through the raw pointer: the hooks may look at it; the voice regeneration, the CRC-32 and the data reply's bursts stay C: `duplex.c`, `fec.c`, `ipgw.c`) |
| `pdu.rs` | `common/src/pdu.c` |
| `rawburst.rs` | `common/src/rawburst.c` |
| `repeater.rs` | `common/src/repeater.c` (the struct shared by four tasks through the raw pointer; `duplex.c` stays C) |
| `slotgrid.rs` | `common/src/slotgrid.c` (`floor` and `%.3f` through the C library) |
| `status.rs` | `common/src/status.c` (`repr()` through the C library's `snprintf` and `strtod`) |
| `tracker.rs` | `common/src/tracker.c` (`modf`, `rint` and the FEC through C; also the timestamps) |
| `txvr.rs` | `common/src/txvr.c` (the struct shared by three tasks through the raw pointer, `tuning` volatile) |
| `usbmatch.rs` | `common/src/usbmatch.c` |

Modules with no C twin (new code is written in Rust only; its C callers build with `RDOF_RUST`):

| Module | What |
|---|---|
| `mrelay.rs` | `rdof/mrelay.h`: a P25 or NXDN repeater on a duplex MMDVM modem (`bench_repeater` `mode`) |
| `sigmulti.rs` | `rdof/sigmulti.h`: several carriers in one bench transmit job, any signal mode's transmitter once a carrier |
| `sigiq.rs` | `rdof/sigiq.h`: the bench's `iq` transmit mode, the client's cs16 baseband (`sdr/src/modes.c`) |

The Tier III trunked site ([tier3.md](../docs/spec/fw/tier3.md) sections 8 and 9) is Rust only, behind two
features: `t3`, the controller (`t3site.rs`, `t3tscc.rs` and the other `t3*.rs`, with the MS simulator `t3ms.rs` and
the air model `t3air.rs`), and `tier3`, the site around it in `rdofradio` (`t3unit.rs`, `t3cfg.rs`, `t3rf.rs`,
`t3sim.rs`, `t3split.rs`). Their C headers are the contract (`rdof/t3.h`, `rdof/t3unit.h` and the others).
`rdofradio` and the host tests build them with `RDOF_TIER3` (on by default); the firmware doesn't. Their own tests,
`t3*tests.rs` and `t3spec_*.rs`, run under `cargo test` (ctest runs them too).

- `config.rs` and `benchcfg.rs` read the version 3 plan, which stays C (`siteplan.c`, `site.c`): `src/plan.rs`
  mirrors its structs and declares the accessors they call.
- Code copied from RadioFoundry (`common/src/radiofoundry`) stays C for now; the Rust calls it through `src/ffi.rs`.

## Structs shared with C

A module that takes or fills a C struct mirrors it `#[repr(C)]`, field for field.

- `src/layout.rs` lists each mirror by its C name with its size and alignment; `layout.c` (built with the Rust)
  compares them with the C headers'. A new mirror goes in both lists.
- On a mismatch `rdof_tests` stops before its tests, and the firmware stops at boot (on Xtensa, where the sizes
  differ from the host's).
- Text the C made with `snprintf` is written in Rust, except `%f`, which goes through C's `snprintf` (`core::fmt`'s
  float code would cost several KB).

## How it builds

One crate, `rdof_rust`: a `no_std` static library (no allocator, no threads; `panic = "abort"`, a panic calls
`abort()`).

- **One library per image.** Each Rust static library carries its own copy of `core` and a panic handler, so a new
  module goes in this crate, not a new one.
- **It calls back into C** through `src/ffi.rs` (the FEC, RadioFoundry's block codes); the C and Rust libraries link
  as a cycle.
- **`rust.cmake`'s `rdof_rust_add()`** runs cargo from CMake (into `<build dir>/common/rdof_rust-cargo/` on the host)
  and makes an imported library of `librdof_rust.a`.
- **`strip_builtins.cmake`** then drops Rust's `compiler_builtins` from the archive. It defines libm's `sinf`, `sqrtf`,
  `expf`... and libgcc's helpers, and the archive links first: left in, all of the image's C would use Rust's math
  in place of newlib's (the DSP included), depending on link order. Rust's math could be faster; if so, that's its own
  change, measured on a board (`kbench`, HIL), not a link-order accident.

| Target | Toolchain |
|---|---|
| Host | stable Rust (`rustup`, minimal profile) |
| ESP32-S3 | Espressif's Rust fork for Xtensa, through `espup`: `espup install --targets esp32s3 --std` (`--std` skips its GCC: ESP-IDF's links). Cargo builds `core` for `xtensa-esp32s3-none-elf` from source (`-Zbuild-std=core`) |
| RISC-V chips (P4) | would use stable Rust and a prebuilt `core` |

## Size and speed

`4mb`, 2026-10-22, all fourteen modules then ported:

- **Image:** 19,008 bytes more than the C (1,805,920 vs 1,786,912); the AMBE tables and unrolling below are 2,352 of
  it, slotgrid 2,176.
- **Output:** on a SuperMini the self-test gives the expected output either way (53 messages, CRC `283d7439`, with
  each fault, and both transmit loopbacks), and the HIL suite passes on both.
- **Speed:** the call trackers take 2.7 to 2.8 Mcycles per second of IQ, as the C (0.1% of a core); first measured,
  3.6 (with the Golay search below already in), then 3.0. Measured per call on the S3, the gap was bounds checks and
  per-bit work in hot loops, which the S3 pays more for than a desktop does:

| Call (cycles) | C | first measured | now |
|---|---|---|---|
| `rdof_ambe_decode_frame`, clean | 2,325 | 4,935 | 559 |
| `rdof_ambe_decode_frame`, errors | 34,916 | 5,359 | 1,091 |
| `rdof_ambe_decode_frame`, clean, params NULL (the tracker's) | 2,067 | | 398 |
| `rdof_bytes_to_bits`, 33 bytes | 2,302 | 3,234 | 1,120 |
| `rdof_bits_to_bytes`, 264 bits | 4,508 | 3,439 | 3,439 |
| `rdof_get_field`, 10 + 48 + 10 bits | 1,426 | 1,377 | 468 |
| `rdof_embedded_signalling` (32 one-bit `rdof_get_field`s) | 1,457 | 2,281 | 2,537 |
| the tracker's CSBK JSON | 19,958 | 18,480 | 18,480 |

What closed the gap:

- **AMBE:** the decode works on the four rows as words, not a byte a bit. A 1 KB table sorts each frame byte's bits
  into four streams that are the rows' bits in order, so the de-interleave is nine lookups and a few shifts.
- **Golay:** the parity is two lookups in 256 bytes of tables, and a clean word skips the search. Golay(23,12)
  searches by the data bits flipped (at most 299 tries, against 1,771 for every pattern of up to 3 of 23 bits).
- **Unrolling:** the C1 scrambling keeps the generator in a word's top half (no masking), and it and the params' 49
  bytes are unrolled by hand (`opt-level = "s"` doesn't, and a loop's counter, branch and variable shifts cost more
  than the work).
- **`get_field`** works a byte at a time. A one-bit field is still slower than the C's; inlining didn't help.
