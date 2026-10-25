#!/bin/bash
# mkiosroot.sh + sweet-root-tweaks.py for hosts whose kernel cannot mount
# HFS+ (WSL2: Microsoft's kernel has no hfsplus module).
#
#   mkiosroot-wsl.sh OS.dmg SYSTEMOS.dmg APPOS.dmg OUT.img [SIZE] [STAGE_DIR]
#
# Same result as mkiosroot.sh followed by sweet-root-tweaks.py, built in two
# halves:
#   1. Everything that only needs files is done in STAGE_DIR on the host's
#      own Linux filesystem (WSL: under ~, NOT /mnt/c): the three APFS
#      volumes are copied out through apfs-fuse, the dyld/cryptex links, the
#      logd stores and the system keybag are added, and the bring-up tweaks
#      run on it.
#   2. The HFS+ image is formatted in user space (mkfs.hfsplus) and filled
#      by libguestfs: a small VM with a stock Ubuntu kernel (which has
#      hfsplus) mounts it and unpacks a tar stream of STAGE_DIR into it.
#      Numeric owners, modes, symlinks and hard links are kept.
#
# Needs: apfs-fuse, hfsprogs, rsync, python3, libguestfs-tools and a stock
# kernel for the libguestfs appliance (see README-betalm.md, option B).
# Free space: about 12 GB in STAGE_DIR plus the image (sparse, up to SIZE).
set -euo pipefail

[[ $# -ge 4 ]] || { sed -n '2,20p' "$0"; exit 2; }
os=$(realpath "$1") sysos=$(realpath "$2") appos=$(realpath "$3")
out=$(realpath -m "$4") size=${5:-16G}
stage=$(realpath -m "${6:-$HOME/iosroot-stage}")
here=$(dirname "$(realpath "$0")")
[[ ! -e $out ]] || { echo "refusing to overwrite $out" >&2; exit 1; }
[[ ! -e $stage ]] || { echo "refusing to reuse $stage (remove it first: sudo rm -rf $stage)" >&2; exit 1; }
case $stage in /mnt/*) echo "warning: $stage is on a Windows drive: slow, and links/owners may not survive" >&2 ;; esac

missing=()
for tool in apfs-fuse mkfs.hfsplus fsck.hfsplus rsync guestfish python3; do
	command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
done
if [[ ${#missing[@]} -gt 0 ]]; then
	echo "Error: missing: ${missing[*]}" >&2
	echo "  sudo apt install hfsprogs rsync python3 libguestfs-tools linux-generic" >&2
	echo "  apfs-fuse: build from https://github.com/sgan81/apfs-fuse" >&2
	exit 1
fi

work=$(mktemp -d)
mounts=()
cleanup() {
	for m in "${mounts[@]}"; do
		mountpoint -q "$m" && sudo umount "$m" 2>/dev/null || true
	done
	rm -f "$work"/*.dmg
	rmdir "$work"/* "$work" 2>/dev/null || true
}
trap cleanup EXIT

# apfs-fuse splits its options on ',' (iPhone10,3 paths): use links, keep .dmg.
apfs_mount() {
	ln -s "$1" "$work/$2.dmg"
	mkdir "$work/$2"
	sudo "$(command -v apfs-fuse)" "$work/$2.dmg" "$work/$2"
	mounts+=("$work/$2")
}
apfs_mount "$os" os
apfs_mount "$sysos" sysos
apfs_mount "$appos" appos

# ---- 1. stage the root on the host filesystem (as mkiosroot.sh does) ----
sudo mkdir -p "$stage"
copy() {
	echo "copying $1 -> /${2#"$stage"}"
	sudo mkdir -p "$2"
	sudo rsync -aH --numeric-ids "$1/" "$2/"
}
copy "$work/os/root" "$stage"
copy "$work/sysos/root" "$stage/private/preboot/Cryptexes/OS"
copy "$work/appos/root" "$stage/private/preboot/Cryptexes/App"
for m in "${mounts[@]}"; do sudo umount "$m"; done
mounts=()

dsc=private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld
sudo mkdir -p "$stage/System/Library/Caches/com.apple.dyld"
for f in "$stage/$dsc"/*; do
	sudo ln "$f" "$stage/System/Library/Caches/com.apple.dyld/${f##*/}"
done

for c in OS App; do
	(cd "$stage/private/preboot/Cryptexes/$c" && sudo find . ! -type d ! -path './System/Library/Caches/com.apple.dyld/*') |
	while IFS= read -r f; do
		f=${f#./}
		if ! sudo test -e "$stage/$f" && ! sudo test -L "$stage/$f"; then
			sudo mkdir -p "$stage/$(dirname "$f")"
			src="$stage/private/preboot/Cryptexes/$c/$f"
			# Linux hfsplus cannot hard-link symlinks: copy those.
			if sudo test -L "$src"; then sudo cp -P "$src" "$stage/$f"; else sudo ln "$src" "$stage/$f"; fi
			echo "linked $c:/$f"
		fi
	done
done

# mkdir/chown/chmod rather than install -o: uutils install (Ubuntu 26.04)
# rejects numeric ids that have no passwd entry, such as iOS's _logd (272).
db="$stage/private/var/db"
sudo mkdir -p "$db/diagnostics/timesync" "$db/uuidtext"
for d in diagnostics uuidtext diagnostics/timesync; do
	sudo touch "$db/$d/.migrated"
	sudo chown 272:272 "$db/$d" "$db/$d/.migrated"
	sudo chmod 0644 "$db/$d/.migrated"
done
sudo chmod 0751 "$db/diagnostics"
sudo chmod 0755 "$db/uuidtext" "$db/diagnostics/timesync"

sudo mkdir -p "$stage/private/var/keybags"
sudo chown 0:0 "$stage/private/var/keybags"
sudo chmod 0700 "$stage/private/var/keybags"
printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' \
	'<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	'<plist version="1.0"><dict><key>KeyBagKeys</key><data>U1dFRVQ=</data></dict></plist>' |
	sudo tee "$stage/private/var/keybags/systembag.kb" >/dev/null
sudo chown 0:0 "$stage/private/var/keybags/systembag.kb"
sudo chmod 0600 "$stage/private/var/keybags/systembag.kb"

echo "== bring-up tweaks"
sudo python3 -I "$here/sweet-root-tweaks.py" "$stage"
sudo du -sh "$stage"

# ---- 2. format the image and fill it inside the libguestfs appliance ----
truncate -s "$size" "$out"
mkfs.hfsplus -s -J -v iOSRoot "$out"
echo "== filling $out through libguestfs (several minutes)"
# Numeric owners only: iOS uids (mobile 501, _logd 272, ...) have no host names.
sudo tar -C "$stage" --numeric-owner -cf - . |
	guestfish --rw -a "$out" -- \
		run : \
		mount-vfs force hfsplus /dev/sda / : \
		tar-in - / : \
		df-h : \
		umount-all

fsck.hfsplus -f -y "$out" | grep -v 'Illegal name' || true
fsck.hfsplus -f -n "$out"
echo "done: $out (stage left in $stage: sudo rm -rf $stage when the image boots)"
