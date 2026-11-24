#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#
# The rdofradio .deb for one architecture, for every Debian-family release from Debian 10 (buster) on: Raspberry Pi
# OS and Pi-Star 4.x, Debian 10-13, Ubuntu 20.04 and later.
#
#   tools/deb/build.sh amd64|arm64|armhf [VERSION]   on an x86_64 host with Docker; output build/deb/out/*.deb
#
# How it runs on all of them (README.md beside this file):
#   - built against the oldest release's C library and headers (a buster sysroot), so the newest glibc symbol it
#     needs is buster's 2.28; armhf against Raspbian buster's, for the ARMv6 of a Pi Zero or Pi 1 (Debian's armhf is
#     ARMv7), which every 32-bit ARM board runs;
#   - libmosquitto 2.1 linked in statically (buster has 1.5, without the MQTT v5 calls), without TLS;
#   - SoapySDR loaded at run time (rdofradio_soapydl.c), any of 0.6-0.8 or none;
#   - libiio linked: libiio0 is in every release, 0.16 on.
# Clang and lld cross-build every architecture against the sysroot; Rust stable, with the target's std.
set -euo pipefail
ARCH=${1:?usage: build.sh amd64|arm64|armhf [VERSION]}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
W=$ROOT/build/deb
case $ARCH in
amd64) TRIPLE=x86_64-linux-gnu RUST=x86_64-unknown-linux-gnu CFLAGS_ARCH= ;;
arm64) TRIPLE=aarch64-linux-gnu RUST=aarch64-unknown-linux-gnu CFLAGS_ARCH= ;;
armhf) TRIPLE=arm-linux-gnueabihf RUST=arm-unknown-linux-gnueabihf CFLAGS_ARCH="-march=armv6 -mfpu=vfp -mfloat-abi=hard -marm" ;;
*) echo "build.sh: amd64, arm64 or armhf" >&2; exit 2 ;;
esac
# the package version: a tag v1.2.3 is 1.2.3; else 0~git<date>.<hash>
if [ $# -ge 2 ]; then
    VER=$2
elif T=$(git -C "$ROOT" describe --tags --exact-match --match 'v[0-9]*' 2>/dev/null); then
    VER=${T#v}
else
    VER=0~git$(git -C "$ROOT" log -1 --format=%cd --date=format:%Y%m%d).$(git -C "$ROOT" rev-parse --short=8 HEAD)
fi
RDOF_VERSION=$(git -C "$ROOT" describe --always --dirty 2>/dev/null || echo "$VER")
MOSQ_URL=https://mosquitto.org/files/source/mosquitto-2.1.2.tar.gz
MOSQ_SHA=fd905380691ac65ea5a93779e8214941829e3d6e038d5edff9eac5fd74cbed02
CJSON_URL=https://github.com/DaveGamble/cJSON/archive/refs/tags/v1.7.18.tar.gz
CJSON_SHA=3aa806844a03442c00769b83e99970be70fbef03735ff898f4811dd03b9f5ee5
# Raspbian's archive key (its fingerprint, checked before the key is used)
RASPBIAN_KEY_URL=https://archive.raspbian.org/raspbian.public.key
RASPBIAN_KEY_FPR=A0DA38D0D76E8B5D638872819165938D90FDDD2E
mkdir -p "$W/dl" "$W/out"
[ -f "$W/dl/mosquitto.tar.gz" ] || curl -fsSL -o "$W/dl/mosquitto.tar.gz" "$MOSQ_URL"
echo "$MOSQ_SHA  $W/dl/mosquitto.tar.gz" | sha256sum -c - >/dev/null
[ -f "$W/dl/cjson.tar.gz" ] || curl -fsSL -o "$W/dl/cjson.tar.gz" "$CJSON_URL"
echo "$CJSON_SHA  $W/dl/cjson.tar.gz" | sha256sum -c - >/dev/null

mkdir -p "$W/apt"
docker run --rm --platform linux/amd64 -v "$ROOT":/src:ro -v "$W":/w -v "$W/apt":/var/cache/apt/archives \
    -e ARCH="$ARCH" -e TRIPLE="$TRIPLE" -e RUST="$RUST" -e CFLAGS_ARCH="$CFLAGS_ARCH" -e VER="$VER" \
    -e RDOF_VERSION="$RDOF_VERSION" -e RASPBIAN_KEY_URL="$RASPBIAN_KEY_URL" -e RASPBIAN_KEY_FPR="$RASPBIAN_KEY_FPR" \
    -e RUSTUP_HOME=/w/rust/rustup -e CARGO_HOME=/w/rust/cargo -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
    debian:trixie bash /src/tools/deb/inside.sh
ls -la "$W/out/rdofradio_${VER}_$ARCH.deb"
