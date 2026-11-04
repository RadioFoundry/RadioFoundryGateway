#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#
# rdofradio and rdofsdr for a PlutoSDR (and its 7010 / 7020 clones) as it comes, firmware v0.3x: nothing to
# install on it. Built in Debian 9's container, whose glibc (2.24) is older than the firmware's (2.25), against
# the firmware's own libiio and libad9361 (from ADI's sysroot for v0.38: their headers and libraries, nothing
# else), and libmosquitto 2.1 (EPL-2.0 / EDL-1.0) built here and linked in statically, without TLS (the firmware has
# no OpenSSL), with the cJSON its build needs (MIT). With pffft, the web page, MQTT, no SoapySDR.
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
MOSQ_URL=https://mosquitto.org/files/source/mosquitto-2.1.2.tar.gz
MOSQ_SHA=fd905380691ac65ea5a93779e8214941829e3d6e038d5edff9eac5fd74cbed02
CJSON_URL=https://github.com/DaveGamble/cJSON/archive/refs/tags/v1.7.18.tar.gz
CJSON_SHA=3aa806844a03442c00769b83e99970be70fbef03735ff898f4811dd03b9f5ee5
[ -f "$DL/sysroot.tar.gz" ] || curl -fL -o "$DL/sysroot.tar.gz" "$SYSROOT_URL"
echo "$SYSROOT_SHA  $DL/sysroot.tar.gz" | sha256sum -c -
[ -f "$DL/cmake.tar.gz" ] || curl -fL -o "$DL/cmake.tar.gz" "$CMAKE_URL"
[ -f "$DL/mosquitto-2.1.2.tar.gz" ] || curl -fL -o "$DL/mosquitto-2.1.2.tar.gz" "$MOSQ_URL"
echo "$MOSQ_SHA  $DL/mosquitto-2.1.2.tar.gz" | sha256sum -c -
[ -f "$DL/cjson-1.7.18.tar.gz" ] || curl -fL -o "$DL/cjson-1.7.18.tar.gz" "$CJSON_URL"
echo "$CJSON_SHA  $DL/cjson-1.7.18.tar.gz" | sha256sum -c -
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
apt-get -qq install -y --no-install-recommends gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf libc6-dev-armhf-cross make >/dev/null
mkdir -p /opt/cmake && tar -xzf /out/dl/cmake.tar.gz -C /opt/cmake --strip-components=1
export PATH=/opt/cmake/bin:$PATH
if [ ! -f /out/libs/usr/lib/libmosquitto_static.a ]; then
    # cJSON (MIT), which the 2.1 build needs found, then the client library alone, static, without TLS
    T=/out/thirdparty && rm -rf $T && mkdir -p $T/cjson $T/mosquitto
    tar -xzf /out/dl/cjson-1.7.18.tar.gz -C $T/cjson --strip-components=1
    cmake -S $T/cjson -B $T/cjson/build -DCMAKE_TOOLCHAIN_FILE=/src/tools/pluto/armhf.cmake -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/out/libs/usr -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=OFF -DENABLE_CJSON_TEST=OFF \
        -DENABLE_TARGET_EXPORT=OFF >/dev/null
    cmake --build $T/cjson/build --target install -j"$(nproc)" >/dev/null
    tar -xzf /out/dl/mosquitto-2.1.2.tar.gz -C $T/mosquitto --strip-components=1
    # the build of the static library leaves out the include folders of the shared one (common, libcommon): in the flags.
    # Without TLS it takes its random bytes from getrandom(), which glibc has from 2.25 (the firmware has 2.25, this
    # container 2.24): the kernel call itself, in a header of our own
    mkdir -p $T/compat/sys
    printf "%s\n" "#include <sys/syscall.h>" "#include <unistd.h>" \
        "static inline long getrandom(void *b, unsigned long n, unsigned f) { return syscall(SYS_getrandom, b, n, f); }" \
        > $T/compat/sys/random.h
    sed -i "s/#  if __GLIBC_PREREQ(2, 25)/#  if 1/" $T/mosquitto/libcommon/random_common.c
    cmake -S $T/mosquitto -B $T/mosquitto/build -DCMAKE_TOOLCHAIN_FILE=/src/tools/pluto/armhf.cmake \
        -DCMAKE_BUILD_TYPE=Release -DWITH_TLS=OFF -DWITH_STATIC_LIBRARIES=ON -DWITH_BROKER=OFF -DWITH_APPS=OFF \
        -DWITH_CLIENTS=OFF -DWITH_PLUGINS=OFF -DWITH_DOCS=OFF -DWITH_TESTS=OFF -DWITH_CTRL_SHELL=OFF -DWITH_LTO=OFF \
        -DWITH_WEBSOCKETS=OFF -DWITH_SRV=OFF -DCMAKE_C_FLAGS="-I$T/compat -I$T/mosquitto/common -I$T/mosquitto/libcommon -I$T/mosquitto/deps" >/dev/null
    cmake --build $T/mosquitto/build --target libmosquitto_static -j"$(nproc)" >/dev/null
    cp $T/mosquitto/build/lib/libmosquitto_static.a /out/libs/usr/lib/
    cp -r $T/mosquitto/include/mosquitto.h $T/mosquitto/include/mqtt_protocol.h $T/mosquitto/include/mosquitto \
        /out/libs/usr/include/
    rm -rf $T
fi
cmake -S /src -B /out/build -DCMAKE_TOOLCHAIN_FILE=/src/tools/pluto/armhf.cmake -DCMAKE_BUILD_TYPE=Release \
    -DRDOF_BUILD_TESTS=OFF -DRDOF_HOST_WEB=ON -DRDOF_PFFFT=ON -DRDOF_VERSION="$RDOF_VERSION"
cmake --build /out/build --target rdofradio rdofsdr rdofchz -j"$(nproc)"
cd /out
cp build/rdofradio build/rdofsdr build/rdofchz .
arm-linux-gnueabihf-strip rdofradio rdofsdr rdofchz
echo "needs:"; arm-linux-gnueabihf-readelf -d rdofradio | grep NEEDED
echo "newest glibc symbol:"; arm-linux-gnueabihf-objdump -T rdofradio | grep -oE "GLIBC_[0-9.]+" | sort -uV | tail -1
tar -czf rdof-pluto.tar.gz rdofradio rdofsdr rdofchz
chown -R '"$(id -u):$(id -g)"' /out
'

# the install bundle
B=rdof-pluto-bundle-$VERSION
rm -rf "$OUT/$B" && mkdir -p "$OUT/$B"
cp "$ROOT"/tools/pluto/bundle/{install.sh,run.sh,config.example.json,flags.example,README.md} "$OUT/$B/"
cp -r "$ROOT"/LICENSE "$ROOT"/LICENSES "$ROOT"/PROVENANCE.md "$OUT/$B/"
# the FPGA's bitstreams (tools/pluto/fpga/, built in rfg-pluto; MIT and ADI BSD): install.sh puts the board's on
# its SD card
if ls "$ROOT"/tools/pluto/fpga/*.bit.bin > /dev/null 2>&1; then
    mkdir -p "$OUT/$B/fpga"
    cp "$ROOT"/tools/pluto/fpga/* "$OUT/$B/fpga/"
fi
gzip -9 -c "$OUT/rdofradio" > "$OUT/$B/rdofradio.gz"
echo "$VERSION" > "$OUT/$B/VERSION"
chmod 755 "$OUT/$B/install.sh" "$OUT/$B/run.sh"
tar -C "$OUT" -czf "$OUT/$B.tar.gz" "$B"
rm -rf "$OUT/$B"
ls -la "$OUT"/rdofradio "$OUT"/rdofsdr "$OUT"/rdof-pluto.tar.gz "$OUT/$B.tar.gz"
