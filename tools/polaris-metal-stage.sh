#!/bin/sh
# Install what Metal on the Adreno 630 needs into an iOS root stage (before the
# root image is built), with iOS owners:
#   sudo sh tools/polaris-metal-stage.sh STAGE_DIR SweetMetal2.bundle.tar.gz [CLIENTS]
# SweetMetal2.bundle.tar.gz: the "sweetmetal2" artifact of the Metal workflow
# (ios-flame / ios-betalm .github/workflows/metal.yml: Turnip + metal2vulkan; the
# same bundle drives the Adreno 640 and 630, Turnip picks the GPU by chip id).
# CLIENTS: processes that get a Metal device besides backboardd
# (/var/local/mtl2-clients; default "*" = every process).
# Boot with: sweetgpu=1 sweetmtl=2 (and mkpayload.py --gpufw DIR).
set -eu
stage=${1:?usage: polaris-metal-stage.sh STAGE_DIR SweetMetal2.bundle.tar.gz [CLIENTS]}
tgz=${2:?usage: polaris-metal-stage.sh STAGE_DIR SweetMetal2.bundle.tar.gz [CLIENTS]}
clients=${3:-*}
[ -d "$stage/System/Library" ] || { echo "$stage is not an iOS root stage" >&2; exit 1; }
ext="$stage/System/Library/Extensions"
rm -rf "$ext/SweetMetal2.bundle"
tar -xzf "$tgz" -C "$ext"
chown -R 0:0 "$ext/SweetMetal2.bundle"
chmod -R u+rwX,go+rX "$ext/SweetMetal2.bundle"
mkdir -p "$stage/private/var/local"
chmod 0777 "$stage/private/var/local"
echo "$clients" > "$stage/private/var/local/mtl2-clients"
: > "$stage/private/var/local/mtl2.log"
chmod 0644 "$stage/private/var/local/mtl2-clients"
chmod 0666 "$stage/private/var/local/mtl2.log"
chown 501:501 "$stage/private/var/local/mtl2.log"
# Core Animation: 8-bit BGRA framebuffer, no compression. Else CA picks the
# iPhone X's 10-bit 'b3a8' framebuffer, which nothing here can show (black panel
# with frames running; backboardd runs as mobile, so both users).
for u in root mobile; do
	d="$stage/private/var/$u/Library/Preferences"
	mkdir -p "$d"
	cat > "$d/com.apple.backboardd.plist" <<PLIST
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
	chmod 0600 "$d/com.apple.backboardd.plist"
done
chown 0:0 "$stage/private/var/root/Library/Preferences/com.apple.backboardd.plist"
chown 501:501 "$stage/private/var/mobile/Library/Preferences/com.apple.backboardd.plist"
echo "Metal staged: $ext/SweetMetal2.bundle, mtl2-clients '$clients', backboardd preferences"
