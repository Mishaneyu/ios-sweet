#!/bin/sh
# Build qmicli (/var/local/bin/qmicli) and cellulard (/var/local/bin/cellulard)
# for iOS arm64 into phone/cellular/out (ad-hoc signed with entitlements).
set -e
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)
CC="$ROOT/work/userland/bin/ios-cc"

rm -rf out
mkdir -p out

# 1. Standalone QMI/QRTR CLI probe & diagnostic tool
"$CC" -O2 -Wall -Wextra -Wno-unused-parameter \
	-o out/qmicli qmicli.c qmi_cellular.c
strip -x out/qmicli
codesign -f -s - --entitlements cellulard-ents.plist out/qmicli

# 2. Persistent CoreTelephony + SystemStatus + QMI/QRTR cellular daemon
"$CC" -O2 -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations -fobjc-arc \
	-framework Foundation -framework CoreTelephony \
	-o out/cellulard cellulard.m qmi_cellular.c
strip -x out/cellulard
codesign -f -s - --entitlements cellulard-ents.plist out/cellulard

ls -la out/qmicli out/cellulard
