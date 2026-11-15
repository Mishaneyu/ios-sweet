#!/usr/bin/env bash
# Flash the iOS root and boot it, in one go.
#
#   bringup.sh [--image BOOT.img] [--work-dir DIR] [--pieces-dir DIR]
#              [--dense IMG] [--no-flash] [--no-log]
#
# The phone must already be in fastboot mode (Power + Volume Down).
#
# Why the pieces: Google's fastboot.exe for Windows is 32-bit and mmaps the
# whole file, so a 16 GiB image dies with std::bad_alloc.  The pieces are
# each under 1.5 GiB and carry a leading DONT_CARE chunk that states the
# absolute block they belong at, so the bootloader places them correctly.
# They are flashed in DESCENDING order, which is also the only order that
# stays correct if a bootloader ever materialised DONT_CARE as zeros.
#
# The pieces come from the DENSE image (make_dense.py).  The bootloader treats
# DONT_CARE as a seek rather than a write, so a sparse image would leave the
# blocks it skips holding the previous root build -- including 47 blocks
# inside the HFS+ catalog B-tree.  Dense means every block is written.
#
# The HFS+ volume goes to the very start of the userdata partition: the
# kernel mounts the partition itself as the root filesystem, and the
# persistent console log lives in the last 4 MiB of the same partition.
#
# After the boot it starts reading the kernel console off the phone's
# CDC-ACM port, so the log is readable from the workstation instead of
# having to be photographed off the screen.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

IMAGE="${IMAGE:-D:/raphael-ios/boot/TEST_Z_0111.img}"
WORK_DIR="${WORK_DIR:-D:/raphael-ios/out/hfsroot2}"
PIECES_DIR="${PIECES_DIR:-$WORK_DIR/dense-pieces}"
DENSE="${DENSE:-$WORK_DIR/ios16-root-dense.simg}"
LOG="${LOG:-$WORK_DIR/ios-console.log}"
FLASH=1
READLOG=1

while [ $# -gt 0 ]; do
  case "$1" in
    --image)    IMAGE="$2"; shift 2 ;;
    --work-dir) WORK_DIR="$2"; shift 2 ;;
    --pieces-dir) PIECES_DIR="$2"; shift 2 ;;
    --dense)    DENSE="$2"; shift 2 ;;
    --log)      LOG="$2"; shift 2 ;;
    --no-flash) FLASH=0; shift ;;
    --no-log)   READLOG=0; shift ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

FASTBOOT="${FASTBOOT:-}"
if [ -z "$FASTBOOT" ]; then
  for c in fastboot ./fastboot.exe ../tools/platform-tools/fastboot.exe; do
    if command -v "$c" >/dev/null 2>&1 || [ -x "$c" ]; then FASTBOOT="$c"; break; fi
  done
fi
[ -n "$FASTBOOT" ] || { echo "fastboot not found; set FASTBOOT=" >&2; exit 1; }

echo "== fastboot: $FASTBOOT"
DEVS="$("$FASTBOOT" devices 2>&1)"
echo "$DEVS"
if [ -z "$DEVS" ]; then
  echo
  echo "!! No device in fastboot mode." >&2
  echo "!! Hold Power + Volume Down on the phone, then release Power." >&2
  exit 1
fi
echo
echo "== image: $IMAGE"
echo "== work dir: $WORK_DIR"
echo

if [ "$FLASH" = "1" ]; then
  echo "== verifying the pieces =="
  python3 "$ROOT/scripts/verify_pieces.py" "$DENSE" "$PIECES_DIR"

  echo
  echo "== simulating the flash =="
  python3 "$ROOT/scripts/simulate_flash.py" "$DENSE" "$PIECES_DIR"

  echo
  echo "== verifying the volume =="
  python3 "$ROOT/scripts/verify_root.py" "$WORK_DIR/ios16-root.simg"

  echo
  echo "== flashing userdata, descending =="
  for p in $(ls "$PIECES_DIR"/p*.simg | sort -r); do
    echo "-- $p"
    "$FASTBOOT" flash userdata "$p"
  done
  echo
  echo "== userdata done =="
else
  echo "== skipping the flash (--no-flash) =="
fi

echo
echo "== booting $IMAGE =="
"$FASTBOOT" boot "$IMAGE"

if [ "$READLOG" = "1" ]; then
  echo
  echo "== reading the kernel console over USB (Ctrl-C to stop) =="
  echo "== log file: $LOG"
  python3 "$ROOT/scripts/console_log.py" - "$LOG" || true
else
  echo
  echo "== watch the screen =="
  echo "   === SWEET t=Ns exec=N crash=N sb=N/N fb=N sw=N dn=N m=N gui=N ==="
  echo "   sw= is the number of frames that actually reached the panel."
  echo "   gui=1 means the console yielded to the GUI (the home screen is up)."
fi
