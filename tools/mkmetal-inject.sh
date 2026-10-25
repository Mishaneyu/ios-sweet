#!/bin/sh
# Stage the files Metal needs for mkpayload.py --inject (bsd/dev/sweet_inject.c):
#   tools/mkmetal-inject.sh SweetMetal2.bundle.tar.gz OUTDIR [CLIENTS]
# SweetMetal2.bundle.tar.gz: the "sweetmetal2" artifact of .github/workflows/metal.yml.
# CLIENTS: processes that get a Metal device besides backboardd (/var/local/mtl2-clients;
# default SpringBoard, "*" = every process). Then:
#   tools/mkpayload.py ... --gpufw GPUFW --inject OUTDIR --cmdline "... sweetgpu=1 sweetmtl=2 sweetfb_hz=60"
set -eu
tgz=${1:?usage: mkmetal-inject.sh SweetMetal2.bundle.tar.gz OUTDIR [CLIENTS]}
out=${2:?usage: mkmetal-inject.sh SweetMetal2.bundle.tar.gz OUTDIR [CLIENTS]}
clients=${3:-SpringBoard}
rm -rf "$out"
mkdir -p "$out/System/Library/Extensions" "$out/private/var/local" \
	"$out/private/var/root/Library/Preferences" "$out/private/var/mobile/Library/Preferences"
tar -xzf "$tgz" -C "$out/System/Library/Extensions"
chmod -R u+rwX,go+rX "$out/System"
echo "$clients" > "$out/private/var/local/mtl2-clients"
: > "$out/private/var/local/mtl2.log"
chmod 0777 "$out/private/var/local"
chmod 0644 "$out/private/var/local/mtl2-clients"
chmod 0666 "$out/private/var/local/mtl2.log"
# Core Animation, from backboardd's first frame: 8-bit BGRA framebuffer, no compression.
# Else CA picks the iPhone X's 10-bit 'b3a8' framebuffer, which nothing here can show
# (first Metal boot on the G8S: frames at 60 fps, black panel).
for u in root mobile; do
	cat > "$out/private/var/$u/Library/Preferences/com.apple.backboardd.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CAEnableDeepFramebuffer</key>
	<false/>
	<key>CAEnableFramebufferCompression</key>
	<false/>
	<key>CAEnableIOSurfaceCompression</key>
	<false/>
</dict>
</plist>
PLIST
	chmod 0600 "$out/private/var/$u/Library/Preferences/com.apple.backboardd.plist"
done
chmod 0755 "$out/private/var/root" "$out/private/var/root/Library" "$out/private/var/root/Library/Preferences" \
	"$out/private/var/mobile" "$out/private/var/mobile/Library" "$out/private/var/mobile/Library/Preferences"
cat > "$out/.owners" <<OWN
/private/var/local/mtl2.log 501 501
/private/var/mobile/Library/Preferences/com.apple.backboardd.plist 501 501
OWN
echo "staged in $out"
