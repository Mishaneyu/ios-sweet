#!/bin/bash
# Build the shim and wrap it in a boot.img v2 with the LG G8S (betalm) layout:
# header v2, base 0, page 4096, kernel 0x8000, ramdisk 0x1000000, tags 0x100,
# DTB 0x1f00000 (LineageOS sm8150-common BoardConfigCommon.mk).
# DTB=path/to/dtb overrides the stock DTB unpacked from your boot.img.
set -euo pipefail
cd "$(dirname "$0")"
CC=aarch64-linux-gnu-gcc
OBJCOPY=aarch64-linux-gnu-objcopy
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
mkdir -p out
# mkbootimg from android-tools if installed, else the bundled header-v2 packer.
if [ -z "${MKBOOTIMG:-}" ]; then
	if command -v mkbootimg >/dev/null; then MKBOOTIMG=mkbootimg
	else MKBOOTIMG="python3 ../tools/mkbootimg_v2.py"; fi
fi
for f in start.S shim.c console.c mmu.c fdt.c loader.c ufs.c sde.c adtfix.c; do
	$CC $CFLAGS -c $f -o out/${f%.*}.o
done
# The image runs wherever ABL puts it with no relocation pass: only the
# header's image_size (start.o, linked at 0) may be an absolute address.
abs=$(aarch64-linux-gnu-readelf -r out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/ufs.o out/sde.o out/adtfix.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	out/start.o out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/ufs.o out/sde.o out/adtfix.o -o out/shim.elf
$OBJCOPY -O binary out/shim.elf out/Image
# ABL copies the file; pad to image_size so .bss/stacks exist as zeroed bytes.
size=$(( 0x$(aarch64-linux-gnu-nm out/shim.elf | awk '$3=="_end"{print $1}') ))
truncate -s "$size" out/Image
# Ramdisk: an xnu payload (tools/mkpayload.py) if given as $1, else an empty
# newc cpio (ABL dislikes a zero-size ramdisk on some builds) -> diagnostics.
if [ -n "${1:-}" ]; then
	cp "$1" out/ramdisk.bin
else
	(cd out && rm -rf rd && mkdir rd && cd rd && find . | cpio -o -H newc --quiet | gzip -9 > ../ramdisk.bin)
fi
$MKBOOTIMG --header_version 2 --kernel out/Image --ramdisk out/ramdisk.bin \
	--dtb "${DTB:-../dtb/boot_unpacked/dtb}" --base 0 --kernel_offset 0x8000 --ramdisk_offset 0x1000000 \
	--tags_offset 0x100 --dtb_offset 0x1f00000 --pagesize 4096 \
	--os_version 17.0.0 --os_patch_level 2035-06 \
	--cmdline "console=null" -o out/shim-boot.img
# Host render of the console layout.
gcc -O1 -Wall -o out/host_test console.c host_test.c && ./out/host_test
ls -l out/Image out/shim-boot.img
