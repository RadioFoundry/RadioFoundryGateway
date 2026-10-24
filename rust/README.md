# rust

The gateway's C ported to Rust a module at a time. Each module exports its C header's functions under the same names and
signatures, so the C that calls it doesn't change, and links in place of its `.c` file. The C stays until the
Rust has run on the boards; until then `CONFIG_RDOF_RUST` ("RadioFoundry Gateway common (common)" menu, off by default; `sdkconfig.defaults.rust` sets it) picks one for
all of them, and needs every `RDOF_FEATURE_*` on (the Rust is that build). `RDOF_RUST_MODULES` in `rust.cmake` lists them; a new module goes there and in `src/lib.rs`.

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
| `layer2.rs` | `common/src/layer2.c` |
| `mmdvm.rs` | `common/src/mmdvm.c` |
| `modem.rs` | `common/src/modem.c` (`rint` and `%.5f` through the C library) |
| `netcalls.rs` | `common/src/netcalls.c` |
| `oled.rs` | `common/src/oled.c` |
| `tracker.rs` | `common/src/tracker.c` (`modf`, `rint` and the FEC through C; also the timestamps) |
| `txvr.rs` | `common/src/txvr.c` (the struct shared by three tasks through the raw pointer, `tuning` volatile) |
| `usbmatch.rs` | `common/src/usbmatch.c` |
| `pdu.rs` | `common/src/pdu.c` |
| `rawburst.rs` | `common/src/rawburst.c` |
| `repeater.rs` | `common/src/repeater.c` (the struct shared by four tasks through the raw pointer; duplex.c stays C) |
| `slotgrid.rs` | `common/src/slotgrid.c` (`floor` and `%.3f` through the C library) |
| `status.rs` | `common/src/status.c` (`repr()` through the C library's `snprintf` and `strtod`) |

Code copied from RadioFoundry (`common/src/radiofoundry`) stays C for now; the Rust calls it through `src/ffi.rs`.

## Structs shared with C

A module that takes or fills a C struct mirrors it `#[repr(C)]`, field for field. `src/layout.rs` lists each
mirror by its C name with its size and alignment, and `layout.c` (built with the Rust) compares them with the C
headers': `rdof_tests_rust` stops before its tests on a mismatch, and the firmware at boot (on Xtensa, where the
sizes differ from the host's). A new mirror goes in both lists. Text the C made with `snprintf` is written in
Rust, except `%f`, which goes through C's `snprintf` (`core::fmt`'s float code would cost several KB).

## How it builds

One crate, `rdof_rust`: a `no_std` static library (no allocator, no threads; `panic = "abort"`, a panic calls
`abort()`). One library per image: each Rust static library carries its own copy of `core` and a panic handler,
so a new module goes in this crate, not a new one. It calls back into C through `src/ffi.rs` (the FEC, RadioFoundry's
block codes); the C and Rust libraries link as a cycle.

`rust.cmake`'s `rdof_rust_add()` runs cargo from CMake and makes an imported library of the result. Then
`strip_builtins.cmake` drops Rust's `compiler_builtins` from the archive. It also defines libm's `sinf`,
`sqrtf`, `expf`... and libgcc's helpers, and the archive links first, so left in, all of the image's C would use
Rust's math instead of newlib's (the DSP included), depending on link order. Rust's math in place of newlib's
could be faster; if so, that's its own change, measured on a board (`kbench`, HIL), not a link-order accident.

- **Host tests:** `rdof_tests_rust` is `rdof_tests` with this crate in place of the C it replaces, built when
  cargo is installed (stable Rust; `rustup` minimal profile).
- **ESP32-S3:** Espressif's Rust fork, for Xtensa, through `espup`: `espup install --targets esp32s3 --std`
  (`--std` skips its GCC: ESP-IDF's links). Cargo builds `core` for `xtensa-esp32s3-none-elf` from source
  (`-Zbuild-std=core`). RISC-V chips (P4) would use stable Rust and a prebuilt `core`.

## Size and speed

`4mb`, 2026-10-22, all fourteen modules: 19,008 bytes more image than the C (1,805,920 vs 1,786,912; the AMBE
tables and unrolling below are 2,352 of it, slotgrid 2,176). On a SuperMini the self-test gives the expected
output either way (53 messages, CRC `283d7439`, with each fault, and both transmit loopbacks), and the HIL suite
passes on both. The call trackers take 2.7 to 2.8 Mcycles per second of IQ, as the C (0.1% of a core); first
measured, 3.6 (with the Golay search below already in), then 3.0. Measured per call on the S3, the gap was
bounds checks and per-bit work in hot loops, which the S3 pays more for than a desktop does:

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

The AMBE decode works on the four rows as words, not a byte a bit: a 1 KB table sorts each frame byte's bits into
four streams that are the rows' bits in order, so the de-interleave is nine lookups and a few shifts. The Golay
parity is two lookups in 256 bytes of tables, a clean word skips the search, the C1 scrambling keeps the generator
in a word's top half (no masking), and that and the params' 49 bytes are unrolled (`opt-level = "s"` doesn't, and
a loop's counter, branch and variable shifts cost more than the work). Golay(23,12) searches by the data bits
flipped (at most 299 tries, against 1,771 for every pattern of up to 3 of 23 bits). `get_field` works a byte at a
time. A one-bit field is still slower than the C's; inlining didn't help.
