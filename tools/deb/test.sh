#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#
# Installs a package built by build.sh in a clean container of each release and checks it: apt installs it with its
# dependencies, rdofradio runs, takes the settings the package put in /etc/rdofradio, finds SoapySDR when it's
# installed (whichever version the release has) and says what to install when it isn't.
#
#   tools/deb/test.sh build/deb/out/rdofradio_<version>_<arch>.deb [IMAGE ...]
#
# The default images are the releases the package is for, of its architecture (another architecture runs under
# QEMU: binfmt for it on the host). armhf also runs on an emulated ARMv6 (a Pi Zero's ARM1176), which an ARMv7
# instruction would stop.
set -euo pipefail
DEB=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
shift
ARCH=$(dpkg-deb -f "$DEB" Architecture 2>/dev/null || echo "${DEB##*_}" | cut -d. -f1)
IMAGES=("$@")
if [ ${#IMAGES[@]} = 0 ]; then
    IMAGES=(debian:buster debian:bullseye debian:bookworm debian:trixie ubuntu:22.04 ubuntu:24.04 ubuntu:26.04)
    [ "$ARCH" = armhf ] && IMAGES=(debian:buster debian:bullseye debian:bookworm debian:trixie ubuntu:24.04 ubuntu:26.04)
    [ "$ARCH" = amd64 ] && IMAGES+=(ubuntu:20.04)
fi
case $ARCH in amd64) PLAT=linux/amd64 ;; arm64) PLAT=linux/arm64 ;; armhf) PLAT=linux/arm/v7 ;; esac
# APT_CACHE: a host folder for the downloaded packages, kept off Docker's disk
CACHE=()
[ -n "${APT_CACHE:-}" ] && mkdir -p "$APT_CACHE" && CACHE=(-v "$(cd "$APT_CACHE" && pwd)":/var/cache/apt/archives)
fail=0
for img in "${IMAGES[@]}"; do
    docker pull -q --platform $PLAT "$img" >/dev/null
    out=$(docker run --rm --platform $PLAT ${CACHE[@]+"${CACHE[@]}"} -v "$DEB":/tmp/rdofradio.deb:ro "$img" bash -c '
set -e
export DEBIAN_FRONTEND=noninteractive
. /etc/os-release
echo "APT::Sandbox::User \"root\";" > /etc/apt/apt.conf.d/99root   # the package cache may be a host folder
# buster and bullseye have moved to the archive
case $VERSION_CODENAME in buster | bullseye)
    rm -f /etc/apt/sources.list.d/debian.sources
    echo "deb http://archive.debian.org/debian $VERSION_CODENAME main" > /etc/apt/sources.list ;;
esac
apt-get -qq update >/dev/null 2>&1
o=$(apt-get install -y /tmp/rdofradio.deb 2>&1) || { echo "FAIL: apt install"; echo "$o" | tail -25; exit 1; }
rdofradio --help 2>&1 | grep -q "^usage: rdofradio" || { echo "FAIL: rdofradio --help"; rdofradio --help 2>&1 | head -3; exit 1; }
test -f /etc/rdofradio/config.json && getent passwd rdofradio >/dev/null || { echo "FAIL: settings or user"; exit 1; }
# the settings it installs parse: --check-config reads them and stops
r=$(rdofradio --config /etc/rdofradio/config.json --check-config 2>&1) || { echo "FAIL: its settings: $r"; exit 1; }
# no SoapySDR: says what to install
r=$(rdofradio --soapy driver=none -r 1536000 --rx 438.5 --listen --seconds 1 2>&1 || true)
echo "$r" | grep -q "apt install libsoapysdr" || { echo "FAIL: without SoapySDR: $r"; exit 1; }
# with it (this release has one of 0.6, 0.7, 0.8): loaded, and its own error (no such device) comes back
s=$(apt-cache search --names-only "^libsoapysdr0\.[678]" | sort | tail -1 | cut -d" " -f1)
apt-get -qq install -y "$s" >/dev/null 2>&1 || { echo "FAIL: apt install $s"; exit 1; }
r=$(rdofradio --soapy driver=none -r 1536000 --rx 438.5 --listen --seconds 1 2>&1 || true)
echo "$r" | grep -q "apt install libsoapysdr" && { echo "FAIL: SoapySDR not loaded: $r"; exit 1; }
v=$(dpkg-query -W -f "\${db:Status-Abbrev}\${Package} \${Version}\n" "libsoapysdr0.*" 2>/dev/null | awk "/^ii/ {print \$2, \$3}")
echo "ok ($PRETTY_NAME, glibc $(ldd --version | head -1 | grep -oE "[0-9.]+$"), $v)"
' 2>&1) || true
    case $(echo "$out" | tail -1) in
    ok*) echo "$img: $(echo "$out" | tail -1)" ;;
    *) echo "$img: FAIL"; echo "$out" | tail -25 | sed "s/^/    /"; fail=1 ;;
    esac
done
if [ "$ARCH" = armhf ]; then
    # a Pi Zero's CPU (ARM1176, ARMv6): rdofradio on the C library it was built against (build.sh's Raspbian sysroot),
    # run by QEMU on the host's architecture; an ARMv7 instruction would stop it
    SYSROOT=${SYSROOT:-$(dirname "$DEB")/../sysroot-armhf}
    SYSROOT=$(cd "$SYSROOT" && pwd)
    out=$(docker run --rm --platform linux/amd64 ${CACHE[@]+"${CACHE[@]}"} -v "$DEB":/tmp/rdofradio.deb:ro \
        -v "$SYSROOT":/sysroot:ro debian:trixie bash -c '
echo "APT::Sandbox::User \"root\";" > /etc/apt/apt.conf.d/99root
apt-get -qq update >/dev/null 2>&1 && apt-get -qq install -y --no-install-recommends qemu-user >/dev/null 2>&1 || exit 1
dpkg-deb -x /tmp/rdofradio.deb /tmp/x
q() { qemu-arm -cpu arm1176 -L /sysroot /tmp/x/usr/bin/rdofradio "$@" 2>&1; }
q --help | head -1
q --config /tmp/x/usr/share/rdofradio/config.json --check-config | cut -c1-60' 2>&1 | tail -2)
    echo "ARMv6 (arm1176): $out"
    case $out in "usage: rdofradio"*"config.json: ok"*) ;; *) fail=1 ;; esac
fi
exit $fail
