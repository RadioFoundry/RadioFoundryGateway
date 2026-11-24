# apt packages

The `rdofradio` package for amd64, arm64 and armhf, and the apt repository at `https://apt.radiofoundry.org`. How one
package runs on every release from Debian 10 on: [reference.md, apt packages](../../docs/reference.md#apt-packages).

| File | What |
|---|---|
| `build.sh ARCH [VERSION]` | One package, in a Debian container on an x86-64 host with Docker: `build/deb/out/rdofradio_<version>_<arch>.deb`. The version: a tag `v1.2.3` gives `1.2.3`, else `0~git<date>.<hash>` |
| `inside.sh` | `build.sh`'s part in the container: the sysroot, clang, Rust, libmosquitto, rdofradio, the package |
| `test.sh DEB [IMAGE ...]` | Installs the package in a clean container of each release and runs it (ARM ones under QEMU; armhf also on an emulated ARMv6). `APT_CACHE=DIR` keeps the downloads in DIR |
| `publish.sh DEBS SITE` | The repository from a folder of packages, signed with `APT_SIGNING_KEY` (the private key, ASCII armored) |
| `config.json` | The settings it installs: a BrandMeister simplex hotspot with its callsign, password and ID left to fill in |
| `default` | `/etc/default/rdofradio`: the device arguments |
| `rdofradio.service`, `postinst`, `prerm`, `postrm` | The service (not enabled at install) and the `rdofradio` system user |
| `radiofoundry.asc` | The repository's public key, fingerprint `DB85 2B7A F9E6 9860 D999 AB05 8130 FCC4 2D9A 24C4` |
| `aarch64_atomics.c` | arm64: the outline atomics buster's libgcc lacks |

## Releases

`.github/workflows/release.yml` runs on a tag `v*`: it builds and tests the three packages, puts them on the GitHub
release, and publishes the repository (this release and the two before it) to the Cloudflare Pages project
`radiofoundry-apt`, served at `apt.radiofoundry.org`. Its secrets:

| Secret | What |
|---|---|
| `APT_SIGNING_KEY` | The repository's private key, ASCII armored, no passphrase |
| `CLOUDFLARE_API_TOKEN` | A token with Cloudflare Pages: Edit on the account |
| `CLOUDFLARE_ACCOUNT_ID` | The account that holds the project |

Without them a release still gets its packages on GitHub, and the workflow warns that the repository wasn't
published. A pull request that changes the packaging builds and tests the packages only.

The token can't be narrower than an account (a Pages permission covers every project in it), so the project goes in
a Cloudflare account of its own that holds nothing else; `apt.radiofoundry.org` is a CNAME to
`radiofoundry-apt.pages.dev` in the zone's account, and a custom domain on the project. The other way needs no
token: the workflow pushes the site to a branch and a Pages project with Git integration deploys it.

A new key: users then fetch `radiofoundry.gpg` again (the lines below).

## When the repository is live

The main README's "Install the package" becomes these lines, which also bring updates with `apt upgrade`:

```sh
sudo apt install -y curl ca-certificates
sudo curl -fsSL https://apt.radiofoundry.org/radiofoundry.gpg -o /usr/share/keyrings/radiofoundry.gpg
echo "deb [signed-by=/usr/share/keyrings/radiofoundry.gpg] https://apt.radiofoundry.org stable main" | sudo tee /etc/apt/sources.list.d/radiofoundry.list
sudo apt update && sudo apt install -y rdofradio
```
