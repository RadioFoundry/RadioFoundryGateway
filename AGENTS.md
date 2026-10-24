# RadioFoundry Gateway

ESP32-S3 DMR gateway firmware: a roaming transceiver, a repeater and a multi-channel SDR receiver (RTL-SDR or the
W12's LR2021), linked to HBlink4, reporting over MQTT, with an IP gateway for DMR packet data. Portable C11 core,
ESP-IDF app in `esp_idf/`, a Linux / macOS build from the top-level `CMakeLists.txt`.

[README.md](README.md) is a short overview. [docs/reference.md](docs/reference.md) is the reference (layout,
profiles, sizes, settings, front ends, CPU budgets, what's not done); the specs are in [docs/spec/](docs/spec/),
starting with [overview.md](docs/spec/fw/overview.md). Read the relevant section before changing a part you haven't
touched, and update the reference and spec when behavior they describe changes. Each component in `components/`
has its own README.

What's measured or found about the LR2021 on the board (rates, FIFO, SPI, gain, noise, ESP-IDF quirks) goes in
[docs/spec/lr2021-iq.md](docs/spec/lr2021-iq.md) as a numbered finding (R2.n, with the board and date); read it
before touching the LR2021 front end.

## Rules

- A git repo with no commits yet: the user makes the first commit. Anything named `*.local*` is git-ignored: local
  notes (the LR2021 ROM, duplex and wideband notes and the W12 front end, `docs/spec/*.local.md`), vendor files
  (`docs/spec/vendor.local/`: the LR2021 datasheet, ROM dump and disassembly, Semtech's patch driver, the W12
  schematic and datasheets) and the W12's factory flash (`firmware-backups.local/`). Never commit them; copy code
  from them only as the licensing rule allows.
- Self-contained: nothing outside this folder is referenced or reached. No mention of the project this was copied
  from anywhere except `PROVENANCE.md`.
- Names: `rdof_` / `RDOF_` / `rdof/` / `CONFIG_RDOF_`; the shown name is "RadioFoundry Gateway".
- Licensing: MIT intended. Track every piece of copied or ported code in `PROVENANCE.md` with its license text in
  `LICENSES/`. Keep GPL to a minimum. Permissively licensed code (MIT, BSD including the Clear BSD License, Apache) may be
  referenced or ported, a manufacturer's own included: for the LR2021, Semtech's USP (github.com/Lora-net/usp,
  Clear BSD) and TheClams/lr2021 (MIT). Keep the license's notice in the file and in `PROVENANCE.md` / `LICENSES/`.
  Never copy the LR2021's ROM: it has no license; take only facts from it.
- US English in code, docs and UI text. Prose in docs and comments: plain, direct, short sentences; match the
  surrounding text.
- Builds go under `build/` only; delete experiment builds when done.

## Tests

```sh
cmake -S host-tests -B build/host && cmake --build build/host && ctest --test-dir build/host
cmake -S . -B build/linux -DRADIOFOUNDRY_DIR=/path/to/radiofoundry && cmake --build build/linux && ctest --test-dir build/linux
RDOF_BOARD=<board ip> python -m pytest -q hil-tests        # against a running board; RDOF_BOARD_MUTATE=1 adds the refused-POST checks
```

The host tests are offline and take seconds. The golden fixtures in `host-tests/` (`vectors.h`, `scenarios.h`,
`fixtures/`) are committed; the C must reproduce them byte for byte. New behavior gets a host test; driver code gets
a test against the shims in its component's `test/`.

## Firmware

ESP-IDF v5.5 (`. ~/esp/v5.5/esp-idf/export.sh`), from `esp_idf/`. A profile plus overlays, each in its own build dir:

```sh
idf.py -B ../build/16mb -D SDKCONFIG=../build/16mb/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.16mb" build
idf.py -B ../build/w12 -D SDKCONFIG=../build/w12/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.16mb;sdkconfig.defaults.w12" build
idf.py -B ../build/4mb -D SDKCONFIG=../build/4mb/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.4mb" build
```

Overlays: `.roamer` / `.sdr` (one role), `.rust` (Rust modules in place of their C), `.hub`, `.dspfast`. Run
`idf.py size` after a change that adds code or RAM and keep docs/reference.md "Size" current.

Memory: internal RAM is the tight resource (about 10 KB free on the W12 while streaming). Buffers go in PSRAM
(`EXT_RAM_BSS_ATTR`) unless they need speed or DMA. SPI DMA transfers need a 4-aligned buffer and length, or
ESP-IDF allocates an internal bounce buffer on every transfer (and crashes when that allocation fails).

Lab-specific notes (boards, addresses, who flashes what, the bench rules) are in `AGENTS.local.md`, which is
git-ignored; read it when it exists.
