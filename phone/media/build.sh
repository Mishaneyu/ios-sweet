#!/bin/sh
# Build libhot_media.dylib and libhot_sysfix.dylib for iOS arm64 into phone/media/out (signed ad hoc).
set -e
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)
CC="$ROOT/work/userland/bin/ios-cc"
mkdir -p out

"$CC" -O2 -Wall -Wextra -Wno-unused-parameter -dynamiclib \
	-install_name /usr/local/lib/libhot_media.dylib \
	-framework CoreFoundation -framework CoreMedia -framework CoreVideo -lobjc \
	-Wl,-dead_strip -o out/libhot_media.dylib inject_media.c
strip -x out/libhot_media.dylib
codesign -f -s - out/libhot_media.dylib 2>/dev/null

"$CC" -O2 -Wall -Wextra -Wno-unused-parameter -dynamiclib \
	-install_name /usr/local/lib/libhot_sysfix.dylib \
	-framework CoreFoundation -framework Foundation -lobjc \
	-Wl,-dead_strip -o out/libhot_sysfix.dylib inject_sysfix.c
strip -x out/libhot_sysfix.dylib
codesign -f -s - out/libhot_sysfix.dylib 2>/dev/null

for dylib in out/libhot_media.dylib out/libhot_sysfix.dylib; do
	WEAK=$(nm -m "$dylib" | grep "weak external" | grep -v "dynamically looked up" || true)
	if [ -n "$WEAK" ]; then
		echo "ERROR: unexpected weak external symbols in $dylib:" >&2
		echo "$WEAK" >&2
		exit 1
	fi
done
ls -la out/libhot_media.dylib out/libhot_sysfix.dylib
