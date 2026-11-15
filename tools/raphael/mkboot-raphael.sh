#!/bin/sh
# Pack a Heartwood kernel + raphael ADT into a raphael boot image (Linux/WSL: python3 + mtools).
#
#   tools/raphael/mkboot-raphael.sh BASE.img KERNEL ADT OUT.img
#
# BASE.img is a working UniXNU-raphael boot image (Android boot header v0: kernel = Project Aloha
# SM8150 UEFI with UniXNU + DarwinBoot, ramdisk = 64 MiB FAT32 "GUACESP" holding
# EFI/UniXNU/unixnu.toml and EFI/UniXNU/Payloads/kernel.macho{,.adt}). Only the FAT changes:
# the kernel and ADT are replaced and the entry's args line is rewritten. The firmware, the
# header and the ramdisk size stay as they are, so ABL boots it exactly like BASE.img.
#
# Boot-args: $CMDLINE, else the read-only default below. RAPHAEL_RW=1 appends ufsrw=1 rootrw=1
# (only once an iOS root has been flashed to userdata on purpose). Nothing here ever enables
# sweetlog (persistent log writes to the end of userdata).
set -eu
[ $# -eq 4 ] || { echo "usage: $0 BASE.img KERNEL ADT OUT.img" >&2; exit 2; }
BASE=$1 KERNEL=$2 ADT=$3 OUT=$4
command -v mcopy >/dev/null || { echo "$0: needs mtools (apt install mtools)" >&2; exit 1; }
DEFAULT_ARGS="debug=0xc serial=3 cpus=8 keepsyms=1 -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin launchd_unsecure_cache=1 wdt=-1 sweetfb_hz=30 sweetusb=2 sweetarm64e=1 launchd_disable_all_timeouts=1 sweetlog=0"
ARGS=${CMDLINE:-$DEFAULT_ARGS}
if [ "${RAPHAEL_RW:-0}" = 1 ]; then ARGS="$ARGS ufsrw=1 rootrw=1"; fi
case " $ARGS " in *" sweetlog=1 "*) echo "$0: refusing sweetlog=1 (writes userdata)" >&2; exit 1;; esac

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
python3 - "$BASE" "$WORK" <<'PY'
import struct, sys
base, work = sys.argv[1], sys.argv[2]
b = open(base, 'rb').read()
assert b[:8] == b'ANDROID!', 'not an Android boot image'
ks, ka, rs, ra, ss, sa, ta, ps, hv = struct.unpack_from('<IIIIIIIII', b, 8)
assert hv == 0 and ss == 0, f'expected boot header v0 without second stage (v{hv}, second {ss})'
roff = ps + (ks + ps - 1) // ps * ps
fat = b[roff:roff + rs]
assert fat[0x52:0x5a] == b'FAT32   ', 'ramdisk is not FAT32'
open(f'{work}/fat.img', 'wb').write(fat)
open(f'{work}/layout', 'w').write(f'{roff} {rs}\n')
PY
export MTOOLS_SKIP_CHECK=1
F="$WORK/fat.img"
mcopy -o -i "$F" ::/EFI/UniXNU/unixnu.toml "$WORK/unixnu.toml"
python3 - "$WORK/unixnu.toml" "$ARGS" <<'PY'
import re, sys
p, args = sys.argv[1], sys.argv[2]
s = open(p).read()
assert '"' not in args and len(args) < 1000
s, n = re.subn(r'(?m)^args = ".*"$', 'args = "' + args + '"', s)
assert n == 1, f'args lines: {n}'
s, n = re.subn(r'(?m)^title = ".*"$', 'title = "Redmi K20 Pro - Heartwood"', s)
assert n == 1, f'title lines: {n}'
open(p, 'w', newline='\n').write(s)
PY
mdel -i "$F" ::/EFI/UniXNU/Payloads/kernel.macho ::/EFI/UniXNU/Payloads/kernel.macho.adt
mcopy -i "$F" "$KERNEL" ::/EFI/UniXNU/Payloads/kernel.macho
mcopy -i "$F" "$ADT" ::/EFI/UniXNU/Payloads/kernel.macho.adt
mcopy -o -i "$F" "$WORK/unixnu.toml" ::/EFI/UniXNU/unixnu.toml
# read back and compare
mcopy -o -i "$F" ::/EFI/UniXNU/Payloads/kernel.macho "$WORK/k.chk"
mcopy -o -i "$F" ::/EFI/UniXNU/Payloads/kernel.macho.adt "$WORK/a.chk"
cmp "$KERNEL" "$WORK/k.chk"; cmp "$ADT" "$WORK/a.chk"
# Zero every free cluster: the replaced kernel, ADT and old unixnu.toml (with its old boot-args)
# would otherwise stay in the image as stale bytes and confuse any raw check of the args.
python3 - "$F" <<'PY'
import struct, sys
p = sys.argv[1]
d = bytearray(open(p, 'rb').read())
bps, spc, rsv, nfats = struct.unpack_from('<H', d, 11)[0], d[13], struct.unpack_from('<H', d, 14)[0], d[16]
fatsz = struct.unpack_from('<I', d, 36)[0]
total = struct.unpack_from('<I', d, 32)[0]
data0 = rsv + nfats * fatsz
nclus = (total - data0) // spc
fat = d[rsv * bps:(rsv + fatsz) * bps]
zeroed = 0
for c in range(2, nclus + 2):
    if struct.unpack_from('<I', fat, 4 * c)[0] & 0x0fffffff == 0:
        off = (data0 + (c - 2) * spc) * bps
        d[off:off + spc * bps] = bytes(spc * bps)
        zeroed += 1
open(p, 'wb').write(d)
print(f'zeroed {zeroed} free clusters of {nclus}')
PY
mcopy -o -i "$F" ::/EFI/UniXNU/unixnu.toml "$WORK/toml.chk"
grep -q "args = \"$ARGS\"" "$WORK/toml.chk"
python3 - "$BASE" "$WORK" "$OUT" <<'PY'
import sys
base, work, out = sys.argv[1:4]
roff, rs = map(int, open(f'{work}/layout').read().split())
b = bytearray(open(base, 'rb').read())
fat = open(f'{work}/fat.img', 'rb').read()
assert len(fat) == rs
b[roff:roff + rs] = fat
open(out, 'wb').write(b)
PY
echo "$OUT: kernel $(wc -c < "$KERNEL") bytes, ADT $(wc -c < "$ADT") bytes"
echo "args: $ARGS"
