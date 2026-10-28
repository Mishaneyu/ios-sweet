#!/bin/sh
# Build HotAudio.driver (CoreAudio HAL AudioServerPlugIn) for iOS arm64 into phone/audio/out (signed ad hoc).
set -e
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)
CC="$ROOT/work/userland/bin/ios-cc"
B=out/HotAudio.driver
rm -rf "$B" && mkdir -p "$B"

"$CC" -O2 -Wall -Wextra -Wno-four-char-constants -Wno-unused-parameter -bundle \
	-framework CoreFoundation -Wl,-dead_strip \
	-o "$B/HotAudio" audio_plugin.c
strip -x "$B/HotAudio"
cp Info.plist "$B/Info.plist"
codesign -f -s - "$B" 2>/dev/null

WEAK=$(nm -m "$B/HotAudio" | grep "weak external" | grep -v "dynamically looked up" || true)
if [ -n "$WEAK" ]; then
	echo "ERROR: unexpected weak external symbols in $B/HotAudio:" >&2
	echo "$WEAK" >&2
	exit 1
fi
ls -la "$B"
