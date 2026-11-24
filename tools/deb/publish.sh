#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
#
# The apt repository from a folder of packages: one suite, stable, component main, for amd64, arm64 and armhf, signed
# with the repository's key, as a static site (Cloudflare Pages serves it at apt.radiofoundry.org).
#
#   APT_SIGNING_KEY="$(cat private-key.asc)" tools/deb/publish.sh DEBS_DIR SITE_DIR
#
# APT_SIGNING_KEY: the ASCII-armored private key (no passphrase). Needs apt-ftparchive (apt-utils) and gpg. SITE_DIR
# is made anew: pool/, dists/stable/, the public key as radiofoundry.gpg (binary, for signed-by) and .asc, an index.
set -euo pipefail
IN=$(cd "$1" && pwd)
OUT=$2
: "${APT_SIGNING_KEY:?the repository key, ASCII armored}"
rm -rf "$OUT" && mkdir -p "$OUT/pool/main/r/rdofradio"
cp "$IN"/*.deb "$OUT/pool/main/r/rdofradio/"
OUT=$(cd "$OUT" && pwd)
cd "$OUT"
ARCHS="amd64 arm64 armhf"
for a in $ARCHS; do
    d=dists/stable/main/binary-$a
    mkdir -p $d
    apt-ftparchive --arch $a packages pool > $d/Packages
    gzip -9nk $d/Packages
    xz -9k $d/Packages
done
apt-ftparchive \
    -o APT::FTPArchive::Release::Origin=RadioFoundry -o APT::FTPArchive::Release::Label=RadioFoundry \
    -o APT::FTPArchive::Release::Suite=stable -o APT::FTPArchive::Release::Codename=stable \
    -o APT::FTPArchive::Release::Architectures="$ARCHS" -o APT::FTPArchive::Release::Components=main \
    -o APT::FTPArchive::Release::Description="RadioFoundry Gateway packages" \
    release dists/stable > dists/stable/Release.tmp
mv dists/stable/Release.tmp dists/stable/Release
GNUPGHOME=$(mktemp -d)
export GNUPGHOME
trap 'rm -rf "$GNUPGHOME"' EXIT
printf '%s\n' "$APT_SIGNING_KEY" | gpg --batch --quiet --import
KEY=$(gpg --batch --list-secret-keys --with-colons | awk -F: '/^fpr/ {print $10; exit}')
gpg --batch --yes --local-user "$KEY" --digest-algo SHA512 --clearsign -o dists/stable/InRelease dists/stable/Release
gpg --batch --yes --local-user "$KEY" --digest-algo SHA512 -abs -o dists/stable/Release.gpg dists/stable/Release
gpg --batch --export "$KEY" > radiofoundry.gpg
gpg --batch --armor --export "$KEY" > radiofoundry.asc
# a page for a browser: how to add the repository (README.md, "Install with apt")
{
    echo '<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width"><title>RadioFoundry apt repository</title>'
    echo '<style>body{font:16px/1.5 system-ui,sans-serif;max-width:52rem;margin:2rem auto;padding:0 1rem}pre{background:#f2f2f2;padding:1rem;overflow-x:auto}</style>'
    echo '<h1>RadioFoundry apt repository</h1>'
    echo '<p>rdofradio (RadioFoundry Gateway) for Raspberry Pi OS (Pi Zero included), Pi-Star 4.x, Debian 10 and later and Ubuntu 20.04 and later, on armhf, arm64 and amd64.</p>'
    echo '<pre>sudo apt install -y curl ca-certificates'
    echo 'sudo curl -fsSL https://apt.radiofoundry.org/radiofoundry.gpg -o /usr/share/keyrings/radiofoundry.gpg'
    echo 'echo "deb [signed-by=/usr/share/keyrings/radiofoundry.gpg] https://apt.radiofoundry.org stable main" | sudo tee /etc/apt/sources.list.d/radiofoundry.list'
    echo 'sudo apt update &amp;&amp; sudo apt install -y rdofradio</pre>'
    echo "<p>Key fingerprint: <code>$KEY</code></p>"
    echo '<p>Packages:</p><ul>'
    for f in pool/main/r/rdofradio/*.deb; do echo "<li><a href=\"$f\">$(basename "$f")</a></li>"; done
    echo '</ul>'
} > index.html
echo "published $(ls pool/main/r/rdofradio | wc -l) packages, signed by $KEY"
