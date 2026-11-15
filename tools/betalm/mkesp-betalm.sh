#!/usr/bin/env bash
# Pack a Heartwood kernel + betalm ADT as an ESP folder for the Renegade betalm
# UEFI with UniXNU (firmware/renegade-betalm). UniXNU reads \EFI\UniXNU\unixnu.toml
# from any FAT volume, so copy the resulting EFI/UniXNU folder into the phone's
# ESP (next to GRUB's EFI/BOOT; nothing else is touched).
#
#   tools/betalm/mkesp-betalm.sh KERNEL ADT OUT.zip
#
# CMDLINE overrides the boot-args. sweetearlyfb=1 must stay within the first 256
# characters (sweet_early_set_cmdline scans only those).
# CMDLINE overrides the boot-args. The defaults are READ-ONLY: no ufsrw/rootrw
# (the UFS driver then refuses every write) and sweetlog=0.
set -euo pipefail
KERNEL=${1:?kernel}; ADT=${2:?adt}; OUT=$(realpath -m "${3:?out.zip}")
DEFAULT_ARGS="-v sweetearlyfb=1 debug=0xc serial=3 cpus=8 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin launchd_unsecure_cache=1 wdt=-1 sweetfb_hz=60 sweetusb=2 sweetarm64e=1 launchd_disable_all_timeouts=1 sweetlog=0 sweetgpu=0 sweettouchdelay=20"
ARGS=${CMDLINE:-$DEFAULT_ARGS}
case " $ARGS " in
  *" sweetlog=1 "*|*" ufsrw=1 "*|*" rootrw=1 "*)
    if [ "${BETALM_RW:-0}" != 1 ]; then
      echo "$0: refusing a writable boot (ufsrw/rootrw/sweetlog); set BETALM_RW=1 once an iOS root partition exists" >&2
      exit 1
    fi ;;
esac
case "$ARGS" in *'"'*) echo "$0: no quotes in boot-args" >&2; exit 1;; esac
head -c 4 "$KERNEL" | od -An -tx1 | grep -q 'cf fa ed fe' || { echo "$0: $KERNEL is not a 64-bit Mach-O" >&2; exit 1; }
grep -aq 'lge,betalm' "$ADT" || { echo "$0: $ADT is not a betalm ADT (mkadt.py --board betalm)" >&2; exit 1; }

W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
P="$W/EFI/UniXNU/Payloads"; mkdir -p "$P"
cp "$KERNEL" "$P/kernel.macho"
cp "$ADT" "$P/kernel.macho.adt"
cat > "$W/EFI/UniXNU/unixnu.toml" <<TOML
version = 1
[boot]
timeout = 5
default = "xnu"
[kits]
autoload = false
[[entry]]
id = "xnu"
title = "LG G8S - iOS 18 (Heartwood)"
type = "xnu"
path = "Payloads/kernel.macho"
args = "$ARGS"
TOML
( cd "$W/EFI/UniXNU" && sha256sum Payloads/* > SHA256SUMS ) && ( cd "$W" && rm -f "$OUT" && zip -qrX "$OUT" EFI )
echo "$OUT"; unzip -l "$OUT"
