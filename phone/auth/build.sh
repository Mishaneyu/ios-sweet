#!/bin/sh
# Build SweetAuth.app (/Applications/SweetAuth.app) and sweetauthd (/var/local/bin/sweetauthd)
# for iOS arm64 into phone/auth/out (ad-hoc signed with entitlements).
set -e
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)
CC="$ROOT/work/userland/bin/ios-cc"

rm -rf out
mkdir -p out/SweetAuth.app

# 1. Local Apple ID / iCloud compatibility daemon (127.0.0.1:29171)
"$CC" -O2 -Wall -Wno-deprecated-declarations -fobjc-arc \
	-framework Foundation -framework Security -framework Accounts \
	-o out/sweetauthd sweetauthd.m
strip -x out/sweetauthd
codesign -f -s - --entitlements sweetauthd-ents.plist out/sweetauthd

# 2. Native UIKit + WebKit "microG Settings" app for iCloud & Apple ID
"$CC" -O2 -Wall -Wno-deprecated-declarations -fobjc-arc \
	-framework UIKit -framework QuartzCore -framework CoreGraphics -framework WebKit -framework Foundation \
	-o out/SweetAuth.app/SweetAuth SweetAuth.m
strip -x out/SweetAuth.app/SweetAuth
cp Info.plist out/SweetAuth.app/Info.plist

# 3. Generate crisp PNG app icons (emerald/teal shield + key glyph, pure Python stdlib)
python3 - <<'PY'
import math, struct, zlib

def write_icon(path, size):
    rows = []
    cx = (size - 1) / 2.0
    for y in range(size):
        ty = y / float(size - 1)
        row = bytearray([0])
        for x in range(size):
            tx = x / float(size - 1)
            t = 0.58 * ty + 0.42 * tx
            r = int(14 + (16 - 14) * t)
            g = int(186 + (116 - 186) * t)
            b = int(148 + (205 - 148) * t)

            nx = (x - cx) / (size * 0.5)
            ny = (y - cx) / (size * 0.5)

            # Shield silhouette
            alpha = 0.0
            if -0.56 <= ny <= 0.60:
                if ny < 0.04:
                    half_w = 0.46
                else:
                    u = (ny - 0.04) / 0.56
                    half_w = 0.46 * max(0.0, 1.0 - u * u * 0.96)
                sdist = abs(nx) - half_w
                if ny < -0.46:
                    sdist = max(sdist, -0.48 - ny)
                if sdist < 0.025:
                    alpha = max(alpha, min(1.0, (0.025 - sdist) * size * 0.32))

            # Cutout key ring + shaft inside shield
            cut = 0.0
            ring_d = abs(math.hypot(nx, ny + 0.14) - 0.13) - 0.038
            if ring_d < 0.02:
                cut = max(cut, min(1.0, (0.02 - ring_d) * size * 0.35))
            if -0.02 <= ny <= 0.34 and abs(nx) <= 0.055:
                sd = max(abs(nx) - 0.042, abs(ny - 0.16) - 0.17)
                if sd < 0.02:
                    cut = max(cut, min(1.0, (0.02 - sd) * size * 0.35))
            if 0.18 <= ny <= 0.31 and 0.0 <= nx <= 0.15:
                td = max(nx - 0.13, abs(ny - 0.245) - 0.045)
                if td < 0.02:
                    cut = max(cut, min(1.0, (0.02 - td) * size * 0.35))

            eff = alpha * (1.0 - 0.90 * cut)
            if eff > 0.0:
                r = int(r * (1.0 - eff) + 255 * eff)
                g = int(g * (1.0 - eff) + 255 * eff)
                b = int(b * (1.0 - eff) + 255 * eff)
            row.extend((max(0, min(255, r)), max(0, min(255, g)), max(0, min(255, b))))
        rows.append(bytes(row))

    raw = b"".join(rows)
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)

    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )
    with open(path, "wb") as f:
        f.write(png)

write_icon("out/SweetAuth.app/AppIcon60x60@2x.png", 120)
write_icon("out/SweetAuth.app/AppIcon60x60@3x.png", 180)
PY

codesign -f -s - --entitlements SweetAuth-ents.plist out/SweetAuth.app

for bin in out/sweetauthd out/SweetAuth.app/SweetAuth; do
	WEAK=$(nm -m "$bin" | grep "weak external" | grep -v "dynamically looked up" || true)
	if [ -n "$WEAK" ]; then
		echo "ERROR: unexpected weak external symbols in $bin:" >&2
		echo "$WEAK" >&2
		exit 1
	fi
done

ls -la out out/SweetAuth.app
