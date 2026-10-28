#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#
# rdofradio and rdofsdr for a PlutoSDR (and its 7010 / 7020 clones) as it comes, firmware v0.3x: nothing to
# install on it. Built in Debian 9's container, whose glibc (2.24) is older than the firmware's (2.25), against
# the firmware's own libiio and libad9361 (from ADI's sysroot for v0.38: their headers and libraries, nothing
# else). With pffft, the web page, no SoapySDR.
#
#   tools/pluto/build.sh          on an x86_64 host with Docker; output in build/pluto/
#
# Then: scp -O build/pluto/rdof-pluto.tar.gz root@192.168.2.1:/tmp (password analog), and on the Pluto
#   cd /tmp && gunzip -c rdof-pluto.tar.gz | tar -xf - && ./rdofradio --iio local: --allow-tx ... --web 0.0.0.0:8080
# It also writes the install bundle, build/pluto/rdof-pluto-bundle-VERSION.tar.gz: rdofradio with an install script
# that sets it up to start at boot and again when it exits (tools/pluto/bundle/README.md). VERSION is
# $RDOF_VERSION, else git describe, else "dev".
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$ROOT/build/pluto
DL=$OUT/dl
mkdir -p "$DL"

SYSROOT_URL=https://github.com/analogdevicesinc/plutosdr-fw/releases/download/v0.38/sysroot-v0.38.tar.gz
SYSROOT_SHA=b99b9c9c833bd7e44037cc97b3670a578b21636f244657be9f338a17cabd8223
CMAKE_URL=https://github.com/Kitware/CMake/releases/download/v3.27.9/cmake-3.27.9-linux-x86_64.tar.gz
[ -f "$DL/sysroot.tar.gz" ] || curl -fL -o "$DL/sysroot.tar.gz" "$SYSROOT_URL"
echo "$SYSROOT_SHA  $DL/sysroot.tar.gz" | sha256sum -c -
[ -f "$DL/cmake.tar.gz" ] || curl -fL -o "$DL/cmake.tar.gz" "$CMAKE_URL"
if [ ! -f "$OUT/libs/usr/include/iio.h" ]; then
    rm -rf "$OUT/libs" && mkdir -p "$OUT/libs/usr/lib" "$OUT/libs/usr/include"
    tar -xzf "$DL/sysroot.tar.gz" -C "$OUT/libs" --wildcards 'staging/usr/lib32/libiio.so*' \
        'staging/usr/lib32/libad9361.so*' staging/usr/include/iio.h staging/usr/include/ad9361.h
    mv "$OUT"/libs/staging/usr/lib32/* "$OUT/libs/usr/lib/"
    mv "$OUT"/libs/staging/usr/include/* "$OUT/libs/usr/include/"
    rm -rf "$OUT/libs/staging"
fi

VERSION=${RDOF_VERSION:-$(git -C "$ROOT" describe --always --dirty 2>/dev/null || echo dev)}
docker run --rm -v "$ROOT":/src:ro -v "$OUT":/out -e RDOF_PLUTO_LIBS=/out/libs -e RDOF_VERSION="$VERSION" debian:stretch bash -euo pipefail -c '
echo "deb http://archive.debian.org/debian stretch main" > /etc/apt/sources.list
apt-get -qq -o Acquire::Check-Valid-Until=false update
apt-get -qq install -y --no-install-recommends gcc-arm-linux-gnueabihf libc6-dev-armhf-cross make >/dev/null
mkdir -p /opt/cmake && tar -xzf /out/dl/cmake.tar.gz -C /opt/cmake --strip-components=1
export PATH=/opt/cmake/bin:$PATH
cmake -S /src -B /out/build -DCMAKE_TOOLCHAIN_FILE=/src/tools/pluto/armhf.cmake -DCMAKE_BUILD_TYPE=Release \
    -DRDOF_BUILD_TESTS=OFF -DRDOF_HOST_WEB=ON -DRDOF_PFFFT=ON -DRDOF_VERSION="$RDOF_VERSION"
cmake --build /out/build --target rdofradio rdofsdr -j"$(nproc)"
cd /out
cp build/rdofradio build/rdofsdr .
arm-linux-gnueabihf-strip rdofradio rdofsdr
echo "needs:"; arm-linux-gnueabihf-readelf -d rdofradio | grep NEEDED
echo "newest glibc symbol:"; arm-linux-gnueabihf-objdump -T rdofradio | grep -oE "GLIBC_[0-9.]+" | sort -uV | tail -1
tar -czf rdof-pluto.tar.gz rdofradio rdofsdr
chown -R '"$(id -u):$(id -g)"' /out
'

# the install bundle
B=rdof-pluto-bundle-$VERSION
rm -rf "$OUT/$B" && mkdir -p "$OUT/$B"
cp "$ROOT"/tools/pluto/bundle/{install.sh,run.sh,config.example.json,flags.example,README.md} "$OUT/$B/"
cp -r "$ROOT"/LICENSE "$ROOT"/LICENSES "$ROOT"/PROVENANCE.md "$OUT/$B/"
gzip -9 -c "$OUT/rdofradio" > "$OUT/$B/rdofradio.gz"
echo "$VERSION" > "$OUT/$B/VERSION"
chmod 755 "$OUT/$B/install.sh" "$OUT/$B/run.sh"
tar -C "$OUT" -czf "$OUT/$B.tar.gz" "$B"
rm -rf "$OUT/$B"
ls -la "$OUT"/rdofradio "$OUT"/rdofsdr "$OUT"/rdof-pluto.tar.gz "$OUT/$B.tar.gz"
