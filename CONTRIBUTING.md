# Contributing

Issues and pull requests are welcome. For anything bigger than a fix, open an issue first so we can agree on the
approach.

## Get started

```sh
git clone https://github.com/RadioFoundry/RadioFoundryGateway.git
cd RadioFoundryGateway
cmake -S host-tests -B build/host && cmake --build build/host && ctest --test-dir build/host
```

You need CMake, a C compiler and cargo ([rustup.rs](https://rustup.rs)). The firmware also needs ESP-IDF v5.5 and
Espressif's Rust toolchain ([docs/reference.md](docs/reference.md), "Firmware builds").

Before changing a part, read its section in [docs/reference.md](docs/reference.md) and its spec in
[docs/spec/](docs/spec/) (start at [overview.md](docs/spec/fw/overview.md)). Each component in `components/` has
its own README.

## Tests

Three levels. Builds go under `build/`.

| Level | Command | Needs |
|---|---|---|
| Host tests | `cmake -S host-tests -B build/host && cmake --build build/host && ctest --test-dir build/host` | the tools above (more tests with SoapySDR or libiio, Python, Node, mosquitto); offline, about 2 minutes |
| System tests | `cmake -S . -B build/linux -DRADIOFOUNDRY_DIR=/path/to/radiofoundry && cmake --build build/linux && ctest --test-dir build/linux` | a [RadioFoundry](https://github.com/RadioFoundry/RadioFoundry) checkout at `systest/radiofoundry.ref` |
| Hardware in the loop | `RDOF_BOARD=<board ip> python -m pytest -q hil-tests` | a running board; `RDOF_BOARD_MUTATE=1` adds the refused-POST checks |

- The host tests run the Rust modules and build with sanitizers: [docs/reference.md](docs/reference.md), "Host
  tests", says what runs and how to turn each off.
- The golden fixtures in `host-tests/` (`vectors.h`, `scenarios.h`, `fixtures/`) are committed. The C must
  reproduce them byte for byte.
- New behavior gets a host test. Driver code gets a test against the shims in its component's `test/`.
- CI runs the host tests (Linux and macOS), the `w12` firmware build and the system tests.

## Rust and C

The modules in `rust/` run in place of their C twins by default, in the firmware and the host build. Change the
Rust, and the C twin while it's kept; `rdof_tests_c` runs the core tests on the C. A new module is Rust only, with
no C twin. Which modules, and how the structs they share are checked: [rust/README.md](rust/README.md).

## Docs

- [docs/reference.md](docs/reference.md) and the spec describe behavior: update them in the same change.
- After a change that adds code or RAM, run `idf.py size` and update reference.md's "Size" table.
- What's measured on hardware goes in with its board and date.
- US English. Plain, direct, short sentences; bullets and tables over paragraphs.

## Code

- C11 for the portable code, ESP-IDF v5.5 for the firmware.
- Names: `rdof_` / `RDOF_` prefixes, `CONFIG_RDOF_` for Kconfig.
- Match the style of the file you're in.
- US English in code and UI text, with short, plain comments.
- Each new source file starts with SPDX lines:
  ```c
  // SPDX-License-Identifier: MIT
  // SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
  ```

## Commits and pull requests

- One change per pull request. The host tests pass, and the firmware builds for the profiles you touched.
- Commit messages: a first line of 50 characters or so, prefixed with the part (`Docs:`, `Board:`, `rdofradio:`,
  `Rust:`), then a body that says what changed and why.

## Licensing

- Contributions are under the MIT license (see [LICENSE](LICENSE)).
- No GPL or AGPL code, and no code ported from it. Facts from datasheets and standards are fine.
- Permissively licensed code (MIT, BSD, Apache-2.0, ISC) may be used: keep its notice in the file, add a row to
  [PROVENANCE.md](PROVENANCE.md) and its license text to [LICENSES/](LICENSES/).
- Add yourself to [AUTHORS](AUTHORS) if you like.
