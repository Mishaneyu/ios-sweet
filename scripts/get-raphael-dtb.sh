#!/usr/bin/env bash
# Fetch the device tree blob for raphael.
#
# The Project Aloha tree ships a first-class xiaomi-raphael device directory
# that already contains a REAL raphael DTB:
#   Platforms/SurfaceDuo1Pkg/Device/xiaomi-raphael/DeviceTreeBlob/Android/android-raphael.dtb
#
# Note: the sibling DeviceTreeBlob/Linux/linux-raphael.dtb in that tree is a
# 6-byte placeholder, so it must not be used. Candidates smaller than 1 KiB are
# skipped as placeholders.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$ROOT/inputs/uefi-prebuilt/raphael.dtb"
mkdir -p "$(dirname "$DEST")"

if [ -f "$DEST" ] && [ "$(wc -c < "$DEST")" -gt 1024 ]; then
  echo "raphael.dtb already present ($(wc -c < "$DEST") bytes)"
  exit 0
fi

DEV="$ROOT/uefi/Platforms/SurfaceDuo1Pkg/Device"

# 1) Preferred: the real raphael DTB shipped by Aloha (Android variant).
# 2) Then the Linux variant of the same device dir.
# 3) Last resort: the SM8150 guacamole DTB (identical SoC).
CANDIDATES=(
  "$DEV/xiaomi-raphael/DeviceTreeBlob/Android/android-raphael.dtb"
  "$DEV/xiaomi-raphael/DeviceTreeBlob/Linux/linux-raphael.dtb"
  "$DEV/oneplus-guacamole/DeviceTreeBlob/Android/android-guacamole.dtb"
  "$DEV/oneplus-guacamole/DeviceTreeBlob/Linux/linux-guacamole.dtb"
)

for SRC in "${CANDIDATES[@]}"; do
  [ -f "$SRC" ] || continue
  SZ=$(wc -c < "$SRC")
  if [ "$SZ" -le 1024 ]; then
    echo "skip placeholder DTB: $SRC ($SZ bytes)"
    continue
  fi
  cp "$SRC" "$DEST"
  echo "copied DTB ($SZ bytes) from ${SRC#$ROOT/} -> $DEST"
  exit 0
done

echo "ERROR: no usable raphael/SM8150 DTB found in the Aloha tree." >&2
echo "Expected one of:" >&2
for SRC in "${CANDIDATES[@]}"; do echo "  - ${SRC#$ROOT/}" >&2; done
echo "You can also drop a raphael DTB at $DEST" >&2
exit 1
