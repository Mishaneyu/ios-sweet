#!/bin/sh
# Build bgmtl-env.dylib in WSL/Linux (clang + ld64.lld, no Apple SDK), mark it arm64e like the Chlorophyll dylibs
# (CPU subtype 0x80000002; the image has no imports and no fixups, so the pointer format does not matter) and sign it
# ad hoc with Renegade's pacstrip --sign (ldid on Linux).
set -eu
cd "$(dirname "$0")"
O=$(mktemp -d); trap 'rm -rf "$O"' EXIT
clang -target arm64-apple-ios18.0 -O2 -fno-stack-protector -ffreestanding -fno-builtin -c bgmtl_env.c -o "$O/e.o"
ld64.lld-18 -dylib -arch arm64 -platform_version ios 18.0 18.0 -fixup_chains -init_offsets \
	-install_name /usr/local/lib/bgmtl/bgmtl-env.dylib -o bgmtl-env.dylib "$O/e.o"
if llvm-nm-18 -u bgmtl-env.dylib | grep -q .; then echo "imports present: refusing" >&2; exit 1; fi
if llvm-objdump-18 --macho --chained-fixups bgmtl-env.dylib | grep -q -e page_start -e pointer_format; then
	echo "fixups present: refusing" >&2; exit 1
fi
python3 - bgmtl-env.dylib <<'PY'
import struct, sys
p = sys.argv[1]; b = bytearray(open(p, 'rb').read())
magic, cpu, sub = struct.unpack_from('<IiI', b, 0)
assert magic == 0xfeedfacf and cpu == 0x100000c, 'not an arm64 Mach-O'
struct.pack_into('<I', b, 8, 0x80000002)
open(p, 'wb').write(b)
print('cpusubtype %#x -> 0x80000002' % sub)
PY
python3 -I ../../renegade/pacstrip.py --sign bgmtl-env.dylib
sha256sum bgmtl-env.dylib
