#!/usr/bin/env bash
# Fetch the iPhone X (D22AP) base Apple Device Tree (ADT).
#
# raphael boots the iOS 16.7.x userland, whose MobileGestalt identity is taken
# from the real D22AP device tree (the /product node). That tree is an Apple
# artifact and is kept out of git; this script regenerates it automatically so
# the whole pipeline is push-button.
#
# Source: iPhone X (iPhone10,3 / D22AP) restore IPSW, member
#   Firmware/all_flash/DeviceTree.d22ap.im4p
# which is an IM4P-wrapped, lzfse-compressed Apple Device Tree. We fetch just
# that ~33 KB member (range requests, no full 6 GB download), unwrap the IM4P,
# lzfse-decompress, and write the raw ADT that mkadt.py's d22ap_product() reads.
#
# Output: scripts/base-d22ap.adt  (consumed by scripts/mkadt.py)
#
# Overrides (any one short-circuits the download):
#   - BASE_D22AP_ADT=/path/to/file : use a file you already have
#   - D22AP_IPSW_URL=<url>         : use a different D22AP IPSW
#   - drop a file at scripts/base-d22ap.adt by hand (the script skips if present)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$ROOT/scripts/base-d22ap.adt"
mkdir -p "$(dirname "$DEST")"

if [ -s "$DEST" ]; then
  echo "base-d22ap.adt already present ($DEST); skipping fetch"
  exit 0
fi

if [ -n "${BASE_D22AP_ADT:-}" ] && [ -s "$BASE_D22AP_ADT" ]; then
  cp "$BASE_D22AP_ADT" "$DEST"
  echo "copied provided BASE_D22AP_ADT -> $DEST"
  exit 0
fi

# Verified-available iPhone X (D22AP) iOS 16.7.16 restore IPSW.
IPSW_URL="${D22AP_IPSW_URL:-https://updates.cdn-apple.com/2025FallFCS/fullrestores/122-77006/1158E502-D76D-4226-AA06-ED5E30CD1111/iPhone10,3,iPhone10,6_16.7.16_20H392_Restore.ipsw}"
DT_MEMBER="Firmware/all_flash/DeviceTree.d22ap.im4p"

echo "Fetching D22AP base ADT from IPSW (single member, no full download):"
echo "  $IPSW_URL"
echo "  member: $DT_MEMBER"

# Make sure the pure-Python deps are present (idempotent; CI starts clean).
python3 -m pip install --quiet --disable-pip-version-check remotezip lzfse 2>/dev/null \
  || pip install --quiet --disable-pip-version-check remotezip lzfse 2>/dev/null \
  || true

# --- Primary path: pure-Python (remotezip + lzfse). No external binary, works
#     identically on macOS / Linux / Windows CI runners. ---
python3 - "$IPSW_URL" "$DT_MEMBER" "$DEST" <<'PY'
import sys, struct, os

url, member, dest = sys.argv[1], sys.argv[2], sys.argv[3]

# Decompress lzfse if available; otherwise fall back to the `ipsw` CLI below.
try:
    import lzfse  # noqa
except Exception:
    sys.exit(10)  # signal: lzfse missing, let the shell fall back to `ipsw`

try:
    from remotezip import RemoteZip
except Exception as e:
    sys.exit("remotezip unavailable: %s" % e)

def der(buf, off):
    tag = buf[off]; ln = buf[off + 1]; off += 2
    if ln & 0x80:
        n = ln & 0x7f
        ln = int.from_bytes(buf[off:off + n], 'big'); off += n
    return tag, off, off + ln

def im4p_payload(buf):
    tag, s, e = der(buf, 0)
    assert tag == 0x30, 'not a DER sequence (not IM4P)'
    items, off = [], s
    while off < e:
        t, vs, ve = der(buf, off); items.append(buf[vs:ve]); off = ve
    assert items[0] == b'IM4P', 'missing IM4P marker'
    return items[3]

with RemoteZip(url) as rz:
    data = rz.read(member)

payload = im4p_payload(data)
if payload[:4] == b'bvx2':
    payload = lzfse.decompress(payload)

# Sanity: ADT starts with little-endian {u32 nprops, u32 nchildren}.
nprops, nchildren = struct.unpack_from('<II', payload, 0)
assert 0 < nprops < 2000 and 0 <= nchildren < 2000, 'unexpected ADT header'

with open(dest, 'wb') as f:
    f.write(payload)
print("wrote %s (%d bytes, nprops=%d nchildren=%d)" % (dest, len(payload), nprops, nchildren))
PY
rc=$?
if [ "$rc" -eq 0 ]; then
  echo "DONE: $DEST"
  exit 0
fi
if [ "$rc" -ne 10 ]; then
  echo "python fetch failed (rc=$rc); falling back to ipsw CLI" >&2
fi

# --- Fallback path: the `ipsw` CLI (blacktop/ipsw) if present. ---
if command -v ipsw >/dev/null 2>&1; then
  echo "using ipsw CLI fallback"
  TMP="$(mktemp -d)"
  python3 - "$IPSW_URL" "$TMP" <<'PY'
import sys, os
from remotezip import RemoteZip
url, d = sys.argv[1], sys.argv[2]
with RemoteZip(url) as rz:
    data = rz.read("Firmware/all_flash/DeviceTree.d22ap.im4p")
open(os.path.join(d, "DeviceTree.d22ap.im4p"), "wb").write(data)
PY
  ipsw img4 im4p extract "$TMP/DeviceTree.d22ap.im4p" -o "$DEST"
  rm -rf "$TMP"
  echo "DONE (ipsw): $DEST"
  exit 0
fi

echo "ERROR: could not fetch base-d22ap.adt." >&2
echo "Install deps with:  pip install remotezip lzfse" >&2
echo "or provide one:     BASE_D22AP_ADT=/path/to/d22ap.adt bash scripts/get-base-d22ap.sh" >&2
exit 1
