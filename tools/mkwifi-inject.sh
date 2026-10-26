#!/bin/sh
# Add the Wi-Fi files to an --inject staging tree (see tools/mkmetal-inject.sh):
#   tools/mkwifi-inject.sh WLAND FWDIR RMTFSDIR LAUNCHD_PLIST OUTDIR
# WLAND: the "wland" artifact of .github/workflows/metal.yml.
# FWDIR: the phone's /vendor/firmware_mnt/image (modem.mdt/bNN, *.jsn, wlanmdsp.mbn, bdwlan.*).
# RMTFSDIR: modemst1.img, modemst2.img, fsg.img, fsc.img read from the phone's partitions
#   (IMEI and radio calibration of that one phone: never commit or share them).
# LAUNCHD_PLIST: the root's /System/Library/xpc/launchd.plist (from the mkiosroot stage);
#   tools/patch-launchd-wland.py adds the wland boot job to it.
set -eu
wland=${1:?usage: mkwifi-inject.sh WLAND FWDIR RMTFSDIR LAUNCHD_PLIST OUTDIR}
fw=${2:?}; rmtfs=${3:?}; lplist=${4:?}; out=${5:?}
here=$(dirname "$0")
mkdir -p "$out/private/var/local/bin" "$out/private/var/local/fw/modem" "$out/private/var/local/fw/rmtfs" \
	"$out/private/var/local/fw/rw" "$out/System/Library/xpc"
cp "$wland" "$out/private/var/local/bin/wland"
chmod 0755 "$out/private/var/local/bin" "$out/private/var/local/bin/wland"
for f in "$fw"/modem.* "$fw"/*.jsn "$fw"/wlanmdsp.mbn "$fw"/bdwlan.*; do
	[ -f "$f" ] && cp "$f" "$out/private/var/local/fw/modem/"
done
for p in modemst1 modemst2 fsg fsc; do
	cp "$rmtfs/$p.img" "$out/private/var/local/fw/rmtfs/"
done
chmod 0755 "$out/private/var/local/fw" "$out/private/var/local/fw/modem"
chmod 0700 "$out/private/var/local/fw/rmtfs" "$out/private/var/local/fw/rw"
chmod 0644 "$out/private/var/local/fw/modem"/*
chmod 0600 "$out/private/var/local/fw/rmtfs"/*
# wland starts once per boot, by kern.boottime; with no RTC driver that can be the same
# on every boot, so an empty stamp from the payload makes each boot a new one.
: > "$out/private/var/local/wland.boot"
python3 -I "$here/patch-launchd-wland.py" "$lplist" "$out/System/Library/xpc/launchd.plist"
chmod 0644 "$out/System/Library/xpc/launchd.plist"
du -sh "$out/private/var/local/fw"
