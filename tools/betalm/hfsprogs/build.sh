#!/usr/bin/env bash
# Static ARM64 hfsprogs (mkfs.hfsplus, fsck.hfsplus) for the G8S's Arch Linux: the Arch ARM repos
# do not ship hfsprogs. Ubuntu's source package 540.1.linux3, cross-built with aarch64-linux-gnu-gcc.
# OpenSSL is only used for SHA-1 in newfs_hfs's volume-UUID generation: sha.h here replaces it.
#   tools/betalm/hfsprogs/build.sh OUTDIR      (Ubuntu host with deb-src, gcc-aarch64-linux-gnu)
set -euo pipefail
OUT=$(realpath -m "${1:?outdir}"); HERE=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT; cd "$W"
apt-get source -q hfsprogs >/dev/null
cd hfsprogs-540.1.linux3
mkdir -p compat/openssl && cp "$HERE/sha.h" compat/openssl/sha.h
sed -i 's/ -lcrypto$//' newfs_hfs.tproj/Makefile.lnx
make CC=aarch64-linux-gnu-gcc CFLAGS="-I$PWD/compat -I$PWD/include -DDEBUG_BUILD=0 -D_FILE_OFFSET_BITS=64 -D LINUX=1 -D BSD=1 -D VERSION=\\\"540.1.linux3\\\" -O2" LDFLAGS=-static >/dev/null
mkdir -p "$OUT"
aarch64-linux-gnu-strip -o "$OUT/mkfs.hfsplus" newfs_hfs.tproj/newfs_hfs
aarch64-linux-gnu-strip -o "$OUT/fsck.hfsplus" fsck_hfs.tproj/fsck_hfs
(cd "$OUT" && sha256sum mkfs.hfsplus fsck.hfsplus)
