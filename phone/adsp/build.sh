#!/bin/sh
# Build adspd (/var/local/bin/adspd) and sweet-audiotest (/var/local/bin/sweet-audiotest)
# for iOS arm64 into phone/adsp/out (ad-hoc signed with platform entitlements).
set -e
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)
CC="$ROOT/work/userland/bin/ios-cc"

rm -rf out
mkdir -p out

# 1. Userland SM8150 LPASS ADSP + Q6AFE/ADM/ASM + MAX98927 + Q6Voice daemon
"$CC" -O2 -Wall -Wextra -Wno-four-char-constants -Wno-unused-parameter \
	-Wl,-dead_strip \
	-o out/adspd adspd.c adsp_hw.c max98927.c adsp_glink.c adsp_q6.c
strip -x out/adspd
codesign -f -s - --entitlements adspd-ents.plist out/adspd

# 2. End-to-end CoreAudio AudioQueue stereo chime & telemetry verifier
"$CC" -O2 -Wall -Wextra -Wno-unused-parameter -fobjc-arc \
	-framework Foundation -framework AudioToolbox -lobjc \
	-Wl,-dead_strip \
	-o out/sweet-audiotest sweet-audiotest.m
strip -x out/sweet-audiotest
codesign -f -s - --entitlements adspd-ents.plist out/sweet-audiotest

for bin in out/adspd out/sweet-audiotest; do
	WEAK=$(nm -m "$bin" | grep "weak external" | grep -v "dynamically looked up" || true)
	if [ -n "$WEAK" ]; then
		echo "ERROR: unexpected weak external symbols in $bin:" >&2
		echo "$WEAK" >&2
		exit 1
	fi
done

ls -la out/adspd out/sweet-audiotest
