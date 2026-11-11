#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#
# rdofradio for a 64-bit ARM Linux box on Debian 13 (a single-board computer with an RTL-SDR and an MMDVM modem):
# cross-built in a Debian 13 amd64 container against its arm64 SoapySDR and libmosquitto, with the Rust modules and
# the web page. The box needs libsoapysdr0.8, soapysdr-module-rtlsdr and libmosquitto1 installed.
#
#   tools/linux-arm64/build.sh [VERSION]     on an x86_64 host with Docker; output build/linux-arm64/out/rdofradio
#
# run.sh and rdofradio.service beside this file run it under systemd (their comments say how).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
W=$ROOT/build/linux-arm64
VER=${1:-$(git -C "$ROOT" describe --always --dirty 2>/dev/null || echo dev)}
mkdir -p $W/out $W/rust
cat > $W/aarch64.cmake <<'T'
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_PREFIX_PATH /usr)
T
docker run --rm --platform linux/amd64 -v $ROOT:/src:ro -v $W:/w -e VER="$VER" -e RUSTUP_HOME=/w/rust/rustup -e CARGO_HOME=/w/rust/cargo debian:trixie bash -euo pipefail -c '
dpkg --add-architecture arm64
apt-get -qq update
apt-get -qq install -y --no-install-recommends gcc-aarch64-linux-gnu libc6-dev-arm64-cross make cmake curl ca-certificates \
  libsoapysdr-dev:arm64 libmosquitto-dev:arm64 pkg-config >/dev/null
export PATH=/w/rust/cargo/bin:$PATH
if [ ! -x /w/rust/cargo/bin/cargo ]; then
  curl -fsSL https://static.rust-lang.org/rustup/dist/x86_64-unknown-linux-gnu/rustup-init -o /tmp/rustup-init && chmod +x /tmp/rustup-init
  /tmp/rustup-init -y -q --no-modify-path --profile minimal --default-toolchain stable --target aarch64-unknown-linux-gnu
fi
export CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER=aarch64-linux-gnu-gcc
cmake -S /src -B /w/build -DCMAKE_TOOLCHAIN_FILE=/w/aarch64.cmake -DCMAKE_BUILD_TYPE=Release -DRDOF_BUILD_TESTS=OFF \
  -DRDOF_HOST_WEB=ON -DRDOF_PFFFFT=ON -DRDOF_VERSION="$VER" -DRDOF_RUST_TARGET=aarch64-unknown-linux-gnu 2>&1 | tail -40
cmake --build /w/build --target rdofradio -j6 2>&1 | tail -30
cp /w/build/rdofradio /w/out/rdofradio
aarch64-linux-gnu-readelf -d /w/out/rdofradio | grep NEEDED
aarch64-linux-gnu-objdump -T /w/out/rdofradio | grep -oE "GLIBC_[0-9.]+" | sort -uV | tail -1
chown -R '"$(id -u):$(id -g)"' /w
'
