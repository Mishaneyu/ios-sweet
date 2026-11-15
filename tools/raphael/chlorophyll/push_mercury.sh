#!/bin/sh
# WSL: install Chlorophyll's Metal device for MercuryPosterExtension on the phone (Renegade bgmtl(), applied on the host
# to native/bgmtl-stage). Backup of the stock extension binary: /var/root/bgmtl-backup/MercuryPosterExtension.
#   push_bgmtl.sh install | revert
set -e
U=$(dirname "$0")/../usblink.sh
S=${BGMTL_STAGE:?set BGMTL_STAGE to the staging root}
B=/System/Library/ExtensionKit/Extensions/MercuryPosterExtension.appex/MercuryPosterExtension
L=/usr/local/lib/bgmtl
put() {	# put LOCAL REMOTE MODE: temp name, then rename
	sh $U ssh "cat > '$2.tmp' && chmod $3 '$2.tmp' && mv '$2.tmp' '$2'" < "$1"
}
case ${1:-} in
install)
	sh $U ssh "mkdir -p /var/root/bgmtl-backup $L /private/var/tmp; [ -e /var/root/bgmtl-backup/MercuryPosterExtension ] || cp -p $B /var/root/bgmtl-backup/MercuryPosterExtension"
	# libraries first: the binary must never name a missing dylib
	for n in libdrm.2.dylib libbgair.dylib bgmtl-inject.dylib bgmtl-env.dylib; do put "$S/usr/local/lib/bgmtl/$n" "$L/$n" 755; done
	put "$S/private/var/tmp/bgmtl.defer" /private/var/tmp/bgmtl.defer 644
	put "$S/$B" "$B" 755
	put "$S/private/var/tmp/bgmtl.arm" /private/var/tmp/bgmtl.arm 644
	sh $U ssh "rm -f /private/var/tmp/bgmtl.off /private/var/tmp/bgmtl.state; ls -la $L $B /private/var/tmp/bgmtl.arm /var/root/bgmtl-backup; sha256sum $L/* $B /var/root/bgmtl-backup/*"
	;;
revert)
	sh $U ssh "cp -p /var/root/bgmtl-backup/MercuryPosterExtension $B.tmp && mv $B.tmp $B && rm -f /private/var/tmp/bgmtl.arm && rm -rf $L && sha256sum $B"
	;;
*) echo "usage: $0 install|revert"; exit 2 ;;
esac
