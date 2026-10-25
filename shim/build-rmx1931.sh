#!/bin/bash
# Build the Realme X2 Pro (RMX1931 / samurai) bare-metal shim and wrap it in an
# Android boot image v1 (Image.gz-dtb + AVB v1.0 hash footer, 96 MiB boot partition).
#
#   shim/build-rmx1931.sh [PAYLOAD] [OUT]
#
# By default, builds with -DSHIM_READONLY when no PAYLOAD is given so zero UFS
# writes occur (safe to boot on a phone with an existing Android custom ROM).
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$(cd .. && pwd)

CC="clang --target=aarch64-none-elf"
LD="ld.lld"
OBJCOPY="llvm-objcopy"
READELF="llvm-readelf"
NM="llvm-nm"

EXTRA_FLAGS="${SHIM_CFLAGS:-}"
if [ -z "${1:-}" ] && [ -z "${ALLOW_UFS_WRITE:-}" ]; then
	EXTRA_FLAGS="$EXTRA_FLAGS -DSHIM_READONLY"
fi

CFLAGS="-O2 -ffreestanding -nostdlib -fno-builtin -fno-pic -fno-pie -mgeneral-regs-only -mstrict-align
	-fno-stack-protector -fno-asynchronous-unwind-tables -Wall -Wextra $EXTRA_FLAGS"
SRCS="start.S shim.c console.c log.c util.c mmu.c fdt.c loader.c ufs.c spmi.c sde.c showlog.c diag.c disptest.c gpuprobe.c gpufw.S smmuprobe.c ramlog.c"

mkdir -p out
objs=
for f in $SRCS; do
	$CC $CFLAGS -c "$f" -o "out/${f%.*}.o"
	objs="$objs out/${f%.*}.o"
done

abs=$($READELF -r $(echo "$objs" | sed 's| out/start.o||') | grep -c R_AARCH64_ABS64 || true)
[ "$abs" = 0 ] || {
	$READELF -r $(echo "$objs" | sed 's| out/start.o||') | grep -B5 R_AARCH64_ABS64 >&2
	echo "error: $abs absolute relocations in C objects" >&2
	exit 1
}

$LD -static -nostdlib -T shim.ld --build-id=none -N $objs -o out/shim.elf
$OBJCOPY -O binary out/shim.elf out/Image

# Pad to _end so .bss/stacks exist in the uncompressed image
size=$($NM out/shim.elf | awk '$3=="_end"{print $1}')
truncate -s $((16#$size)) out/Image

# RMX1931 ABL uses gzip-compressed ARM64 Image + appended stock DTB (Image.gz-dtb)
gzip -9 -n -c out/Image > out/Image.gz
cat out/Image.gz "$ROOT/stock/dtb" > out/Image.gz-dtb

if [ -n "${1:-}" ]; then
	cp "$1" out/ramdisk.bin
else
	(cd out && rm -rf rd && mkdir rd && cd rd && find . | cpio -o -H newc --quiet | gzip -9 -n > ../ramdisk.bin)
fi

OUT=${2:-out/shim-rmx1931-probe.img}
python3 "$ROOT/tools/mkbootimg_rmx1931.py" \
	--kernel out/Image.gz-dtb \
	--ramdisk out/ramdisk.bin \
	--cmdline "androidboot.boot_devices=soc/1d84000.ufshc androidboot.console=ttyMSM0 androidboot.hardware=qcom androidboot.usbcontroller=a600000.dwc3 lpm_levels.sleep_disabled=1" \
	-o "$OUT"

if [ -f "$ROOT/downloads/avb/avbtool.py" ]; then
	python3 "$ROOT/downloads/avb/avbtool.py" add_hash_footer \
		--image "$OUT" \
		--partition_size 100663296 \
		--partition_name boot
fi

ls -lh out/Image out/Image.gz-dtb "$OUT"
