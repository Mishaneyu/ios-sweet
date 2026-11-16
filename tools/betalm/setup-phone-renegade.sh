#!/usr/bin/env bash
# Prepare Arch Linux ARM on the LG G8S to build the iOS 18 root with Renegade (tools/renegade).
# Installs packages, the static hfsprogs from hfsprogs-540.1-static-arm64-v*.zip (Claude build, see
# tools/betalm/hfsprogs/build.sh), builds apfs-fuse, installs blacktop's ipsw (linux_arm64 release),
# and clones/updates ios-sweet branch ios18-betalm in ~/ios-sweet. Touches no partition.
#
#   bash setup-phone-renegade.sh ~/hfsprogs-540.1-static-arm64-v0.zip
set -euo pipefail
HFSZIP=$(realpath "${1:?usage: setup-phone-renegade.sh HFSPROGS.zip}")
step() { printf '\n=== %s\n' "$*"; }

step "packages (pacman, only if missing; no database sync, no system upgrade)"
need=()
for p in base-devel git python fuse3 bzip2 zlib attr zstd curl unzip; do pacman -Qq "$p" >/dev/null 2>&1 || need+=("$p"); done
if [ ${#need[@]} -gt 0 ]; then sudo pacman -S --needed --noconfirm "${need[@]}"; else echo "all present"; fi

step "cmake (PyPI wheel in a venv; avoids a pacman -Syu of the whole phone)"
if ! command -v cmake >/dev/null; then
	python3 -m venv ~/.local/renegade-venv
	~/.local/renegade-venv/bin/pip install -q --upgrade pip cmake
	export PATH="$HOME/.local/renegade-venv/bin:$PATH"
fi
cmake --version | head -1

step "hfsplus kernel module"
sudo modprobe hfsplus && lsmod | grep -q '^hfsplus' && echo "hfsplus loaded"
echo hfsplus | sudo tee /etc/modules-load.d/hfsplus.conf >/dev/null

step "hfsprogs (mkfs.hfsplus, fsck.hfsplus) from $HFSZIP"
T=$(mktemp -d); unzip -q -o "$HFSZIP" -d "$T"
(cd "$T" && sha256sum -c SHA256SUMS)
sudo install -m 755 "$T/mkfs.hfsplus" "$T/fsck.hfsplus" /usr/local/bin/
rm -rf "$T"
fsck.hfsplus 2>&1 | head -1 || true

step "apfs-fuse (read Apple's APFS disk images)"
if ! command -v apfs-fuse >/dev/null; then
	rm -rf ~/src/apfs-fuse; mkdir -p ~/src
	git clone -q --recursive https://github.com/sgan81/apfs-fuse ~/src/apfs-fuse
	cmake -S ~/src/apfs-fuse -B ~/src/apfs-fuse/build -DCMAKE_BUILD_TYPE=Release >/dev/null
	make -C ~/src/apfs-fuse/build -j"$(nproc)" >/dev/null
	sudo install -m 755 ~/src/apfs-fuse/build/apfs-fuse /usr/local/bin/
fi
command -v apfs-fuse

step "ipsw (blacktop, linux_arm64)"
if ! command -v ipsw >/dev/null; then
	URL=$(curl -fsSL https://api.github.com/repos/blacktop/ipsw/releases/latest |
		python3 -c 'import json,sys; a=[x["browser_download_url"] for x in json.load(sys.stdin)["assets"] if x["name"].endswith("_linux_arm64.tar.gz")]; print(a[0])')
	echo "$URL"
	T=$(mktemp -d); curl -fsSL "$URL" -o "$T/ipsw.tar.gz"
	tar -xzf "$T/ipsw.tar.gz" -C "$T"
	sudo install -m 755 "$(find "$T" -type f -name ipsw | head -1)" /usr/local/bin/ipsw
	rm -rf "$T"
fi
ipsw version 2>/dev/null | head -1 || ipsw --version

step "ios-sweet branch ios18-betalm"
if [ -d ~/ios-sweet/.git ]; then
	git -C ~/ios-sweet fetch -q --depth 1 origin ios18-betalm && git -C ~/ios-sweet checkout -q -B ios18-betalm FETCH_HEAD
else
	git clone -q --depth 1 --branch ios18-betalm https://github.com/Mishaneyu/ios-sweet ~/ios-sweet
fi
git -C ~/ios-sweet log --oneline -1

step "space"
df -h ~
echo; echo "setup done"
