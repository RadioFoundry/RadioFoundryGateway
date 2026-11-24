#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# build.sh's part in its Debian container: the sysroot, the toolchain, libmosquitto, rdofradio, the package.
set -euo pipefail
echo 'APT::Sandbox::User "root";' > /etc/apt/apt.conf.d/99root   # its package cache is a host folder
rm -f /etc/apt/apt.conf.d/docker-clean   # and is kept
apt-get -qq update
apt-get -qq install -y --no-install-recommends clang lld llvm gcc libc6-dev cmake make curl ca-certificates mmdebstrap \
    debian-archive-keyring gpg gpgv dpkg-dev file libsoapysdr-dev >/dev/null
export PATH=/w/rust/cargo/bin:$PATH
if [ ! -x /w/rust/cargo/bin/rustup ]; then
    curl -fsSL https://static.rust-lang.org/rustup/dist/x86_64-unknown-linux-gnu/rustup-init -o /tmp/rustup-init
    chmod +x /tmp/rustup-init && /tmp/rustup-init -y -q --no-modify-path --profile minimal --default-toolchain stable
fi
rustup target add "$RUST" >/dev/null 2>&1

# the sysroot: the C library, the kernel headers, libgcc and libiio of buster (Raspbian buster for armhf), unpacked
S=/w/sysroot-$ARCH
if [ ! -f $S/.done2 ]; then
    rm -rf $S
    if [ $ARCH = armhf ]; then
        curl -fsSL "$RASPBIAN_KEY_URL" | gpg --dearmor > /tmp/raspbian.gpg
        gpg --show-keys --with-colons /tmp/raspbian.gpg | grep -q "^fpr:*$RASPBIAN_KEY_FPR:" ||
            { echo "build.sh: Raspbian key fingerprint differs" >&2; exit 1; }
        KR=/tmp/raspbian.gpg MIRROR=http://legacy.raspbian.org/raspbian
    else
        KR=/usr/share/keyrings/debian-archive-removed-keys.gpg MIRROR=http://archive.debian.org/debian
    fi
    # gpgv checks the signatures: sqv refuses Raspbian's 2012 key (a SHA-1 self-signature), checked above by fingerprint
    mmdebstrap --quiet --variant=extract --mode=root --architectures=$ARCH --keyring=$KR \
        --aptopt="APT::Sandbox::User \"root\"" \
        --aptopt="APT::Key::GPGVCommand \"gpgv\"" \
        --include=libc6-dev,linux-libc-dev,libgcc-8-dev,libstdc++-8-dev,libiio-dev buster $S "deb $MIRROR buster main"
    # absolute links (libgcc_s.so.1 -> /lib/...) would reach this container's own files: into the sysroot instead
    find $S -type l -lname "/*" | while read -r l; do ln -sfn "$S$(readlink "$l")" "$l"; done
    touch $S/.done2
fi

# the toolchain: clang for the target, against the sysroot
cat > /w/toolchain-$ARCH.cmake <<T
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR $ARCH)
set(CMAKE_SYSROOT $S)
set(CMAKE_C_COMPILER clang)
set(CMAKE_C_COMPILER_TARGET $TRIPLE)
set(CMAKE_C_FLAGS_INIT "$CFLAGS_ARCH")
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_CXX_COMPILER_TARGET $TRIPLE)
set(CMAKE_CXX_FLAGS_INIT "$CFLAGS_ARCH")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_LIBRARY_ARCHITECTURE $TRIPLE)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
T
CC_T="clang --target=$TRIPLE --sysroot=$S $CFLAGS_ARCH -fuse-ld=lld"
U=$(echo $RUST | tr a-z- A-Z_)
printf "#!/bin/sh\nexec $CC_T \"\$@\"\n" > /w/cc-$ARCH && chmod +x /w/cc-$ARCH
export CARGO_TARGET_${U}_LINKER=/w/cc-$ARCH CC_$(echo $RUST | tr - _)=/w/cc-$ARCH

# libmosquitto 2.1, static, without TLS, and cJSON (MIT), which its build needs found
L=/w/libs-$ARCH
if [ ! -f $L/lib/libmosquitto_static.a ]; then
    T=/tmp/tp && rm -rf $T $L && mkdir -p $T/cjson $T/mosquitto $L/lib $L/include
    tar -xzf /w/dl/cjson.tar.gz -C $T/cjson --strip-components=1
    cmake -S $T/cjson -B $T/cjson/build -DCMAKE_TOOLCHAIN_FILE=/w/toolchain-$ARCH.cmake -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=$L -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=OFF -DENABLE_CJSON_TEST=OFF \
        -DENABLE_TARGET_EXPORT=OFF -DCMAKE_FIND_ROOT_PATH=$L >/dev/null
    cmake --build $T/cjson/build --target install -j"$(nproc)" >/dev/null
    tar -xzf /w/dl/mosquitto.tar.gz -C $T/mosquitto --strip-components=1
    cmake -S $T/mosquitto -B $T/mosquitto/build -DCMAKE_TOOLCHAIN_FILE=/w/toolchain-$ARCH.cmake -DCMAKE_FIND_ROOT_PATH=$L \
        -DCMAKE_BUILD_TYPE=Release -DWITH_TLS=OFF -DWITH_STATIC_LIBRARIES=ON -DWITH_BROKER=OFF -DWITH_APPS=OFF \
        -DWITH_CLIENTS=OFF -DWITH_PLUGINS=OFF -DWITH_DOCS=OFF -DWITH_TESTS=OFF -DWITH_CTRL_SHELL=OFF -DWITH_LTO=OFF \
        -DWITH_WEBSOCKETS=OFF -DWITH_SRV=OFF \
        -DCMAKE_C_FLAGS="$CFLAGS_ARCH -I$T/mosquitto/common -I$T/mosquitto/libcommon -I$T/mosquitto/deps" >/dev/null
    cmake --build $T/mosquitto/build --target libmosquitto_static -j"$(nproc)" >/dev/null
    cp $T/mosquitto/build/lib/libmosquitto_static.a $L/lib/
    cp -r $T/mosquitto/include/mosquitto.h $T/mosquitto/include/mqtt_protocol.h $T/mosquitto/include/mosquitto $L/include/
    rm -rf $T
fi
# SoapySDR 0.8 headers (the library is loaded at run time): this container's own, apart from its other headers
mkdir -p /w/soapy-inc && cp -r /usr/include/SoapySDR /w/soapy-inc/

# arm64: the outline atomics Rust's std calls, which buster's libgcc lacks (aarch64_atomics.c)
EXTRA_LD=
if [ $ARCH = arm64 ]; then
    clang --target=$TRIPLE --sysroot=$S -O2 -mno-outline-atomics -fPIC -c /src/tools/deb/aarch64_atomics.c -o /w/atomics-$ARCH.o
    EXTRA_LD=/w/atomics-$ARCH.o
fi
B=/w/build-$ARCH
rm -rf $B
cmake -S /src -B $B -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=lld $EXTRA_LD" -DCMAKE_TOOLCHAIN_FILE=/w/toolchain-$ARCH.cmake -DCMAKE_BUILD_TYPE=Release -DRDOF_BUILD_TESTS=OFF \
    -DRDOF_HOST_WEB=ON -DRDOF_PFFFT=ON -DRDOF_VERSION="$RDOF_VERSION" -DRDOF_RUST_TARGET=$RUST -DRDOF_SOAPY_DLOPEN=ON \
    -DSOAPYSDR_INCLUDE_DIR=/w/soapy-inc -DMOSQUITTO_INCLUDE_DIR=$L/include -DMOSQUITTO_LIBRARY=$L/lib/libmosquitto_static.a \
    -DCMAKE_FIND_ROOT_PATH="$L" >/dev/null
cmake --build $B --target rdofradio -j"$(nproc)" 2>&1 | grep -E "error|warning: [^d]" || true
test -x $B/rdofradio
llvm-strip $B/rdofradio

# what it needs: each shared library a package of every release, and the newest glibc symbol
NEEDED=$(llvm-readelf -d $B/rdofradio | sed -n "s/.*Shared library: \[\(.*\)\]/\1/p" | tr "\n" " ")
GLIBC=$(llvm-objdump -T $B/rdofradio | grep -oE "GLIBC_[0-9.]+" | sort -uV | tail -1)
echo "needs: $NEEDED; newest glibc symbol: $GLIBC"
DEPS="libc6 (>= ${GLIBC#GLIBC_})"
for n in $NEEDED; do
    case $n in
    libc.so.6|libm.so.6|libpthread.so.0|libdl.so.2|librt.so.1|ld-linux*|libgcc_s.so.1) ;;
    libiio.so.0) DEPS="$DEPS, libiio0" ;;
    *) echo "build.sh: $n: which package?" >&2; exit 1 ;;
    esac
done
case " $NEEDED " in *" libgcc_s.so.1 "*) DEPS="$DEPS, libgcc1 | libgcc-s1" ;; esac

# the package
P=/tmp/pkg && rm -rf $P
install -D -m 755 $B/rdofradio $P/usr/bin/rdofradio
install -D -m 644 /src/tools/deb/rdofradio.service $P/lib/systemd/system/rdofradio.service
install -D -m 644 /src/tools/deb/default $P/etc/default/rdofradio
mkdir -p $P/usr/share/doc/rdofradio && cp -r /src/config/examples $P/usr/share/doc/rdofradio/examples
install -D -m 644 /src/tools/deb/config.json $P/usr/share/rdofradio/config.json
install -D -m 644 /src/tools/deb/README.Debian $P/usr/share/doc/rdofradio/README.Debian
install -D -m 644 /src/LICENSE $P/usr/share/doc/rdofradio/copyright
cp -r /src/LICENSES /src/PROVENANCE.md $P/usr/share/doc/rdofradio/
mkdir -p $P/DEBIAN
install -m 755 /src/tools/deb/postinst /src/tools/deb/prerm /src/tools/deb/postrm $P/DEBIAN/
printf "/etc/default/rdofradio\n" > $P/DEBIAN/conffiles
cat > $P/DEBIAN/control <<C
Package: rdofradio
Version: $VER
Architecture: $ARCH
Maintainer: RadioFoundry <packages@radiofoundry.org>
Installed-Size: $(du -sk $P --exclude=DEBIAN | cut -f1)
Depends: $DEPS, adduser
Suggests: libsoapysdr0.8 | libsoapysdr0.7 | libsoapysdr0.6, soapysdr-module-rtlsdr, rtl-sdr, mosquitto
Section: hamradio
Priority: optional
Homepage: https://radiofoundry.org
Description: RadioFoundry Gateway: a DMR hotspot, repeater and SDR receiver
 rdofradio runs an MMDVM modem (a hotspot or a duplex repeater board) and SDRs
 (an RTL-SDR, through SoapySDR; a PlutoSDR, through libiio) as a DMR hotspot,
 repeater, multi-channel receiver or Tier III site, linked to BrandMeister,
 HBlink4 and MQTT, with a web page. The service starts once it's set up:
 /usr/share/doc/rdofradio/README.Debian.
C
dpkg-deb --root-owner-group -Zxz --build $P /w/out/rdofradio_${VER}_$ARCH.deb >/dev/null
chown -R $HOST_UID:$HOST_GID /w
dpkg-deb -I /w/out/rdofradio_${VER}_$ARCH.deb | sed -n "/Package/,/Suggests/p"
