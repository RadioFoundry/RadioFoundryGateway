# Contributing

Issues and pull requests are welcome.

## Before you start

- For anything bigger than a fix, open an issue first so we can agree on the approach.
- Read [docs/reference.md](docs/reference.md) and the spec for the part you're changing ([docs/spec/](docs/spec/)).

## Pull requests

- Host tests pass: `cmake -S host-tests -B build/host && cmake --build build/host && ctest --test-dir build/host`
- The firmware builds for the profiles you touched (commands in the README).
- New behavior has a host test. Driver code gets a test against its component's shims in `test/`.
- Docs and specs are updated where behavior changes.
- One change per PR, with a commit message that says what changed and why.

## Code

- C11 for the portable code, ESP-IDF v5.5 for the firmware.
- Names: `rdof_` / `RDOF_` prefixes, `CONFIG_RDOF_` for Kconfig.
- Format with the repo's `.clang-format` where there is one; otherwise match the file.
- US English in code, docs and UI text. Short, plain sentences in comments and docs.
- Each new source file starts with SPDX lines:
  ```c
  // SPDX-License-Identifier: MIT
  // SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
  ```

## Licensing

- Contributions are under the MIT license (see [LICENSE](LICENSE)).
- No GPL or AGPL code, and no code ported from it. Facts from datasheets and standards are fine.
- Permissively licensed code (MIT, BSD, Apache-2.0, ISC) may be used: keep its notice in the file, add a row to
  [PROVENANCE.md](PROVENANCE.md) and its license text to [LICENSES/](LICENSES/).
- Add yourself to [AUTHORS](AUTHORS) if you like.
