#!/bin/sh
# Install HackiOS-adapted userland components onto an iOS 16 rootfs or live RMX1931 phone:
#   - /usr/local/lib/libsweet_media.dylib     (mediaserverd: skip portrait GLES prewarm, VCPHEVC encoder, FigFlashlight -> /dev/hakios-torch)
#   - /usr/local/lib/libsweet_sysfix.dylib    (containermanagerd no-personas, SpringBoard JPEG/PNG app snapshots)
#   - /System/Library/Audio/Plug-Ins/HAL/HakiosAudio.driver (Speaker, Hawking, Actuator -> /dev/hakios-haptic, Codec)
#   - /private/var/local/inject.conf          (loaded by sweet_exec_inject_init before launchd)
#   - Re-enables VirtualAudio.plugin and MediaToolbox StartRecorderServer=true
#
# Usage:
#   sudo tools/rmx1931-install-hackios.sh --root /mnt/ios [--dist DIR]   # offline mounted HFS+ rootfs
#   tools/rmx1931-install-hackios.sh [--no-restart] [--dist DIR]         # live phone over tools/flame-ssh
set -e
cd "$(dirname "$0")/.."

MODE=ssh
MOUNT_ROOT=""
RESTART=1
DIST=""

while [ $# -gt 0 ]; do
	case "$1" in
	--root)
		MODE=root
		MOUNT_ROOT=$2
		shift 2
		;;
	--dist)
		DIST=$2
		shift 2
		;;
	--no-restart)
		RESTART=0
		shift
		;;
	*)
		echo "usage: $0 [--root MOUNTED_ROOT | --no-restart] [--dist DIR]" >&2
		exit 1
		;;
	esac
done

if [ -n "$DIST" ]; then
	MEDIA_DYLIB="$DIST/lib/libsweet_media.dylib"
	SYSFIX_DYLIB="$DIST/lib/libsweet_sysfix.dylib"
	AUDIO_DRV="$DIST/HAL/HakiosAudio.driver"
else
	MEDIA_DYLIB="phone/media/out/libsweet_media.dylib"
	SYSFIX_DYLIB="phone/media/out/libsweet_sysfix.dylib"
	AUDIO_DRV="phone/audio/out/HakiosAudio.driver"
fi

for f in "$MEDIA_DYLIB" "$SYSFIX_DYLIB" "$AUDIO_DRV/HakiosAudio" "$AUDIO_DRV/Info.plist"; do
	[ -s "$f" ] || { echo "missing build artifact: $f (run phone/media/build.sh & phone/audio/build.sh or pass --dist)" >&2; exit 1; }
done

INJECT_LINE_1="/usr/local/lib/libsweet_media.dylib mediaserverd"
INJECT_LINE_2="/usr/local/lib/libsweet_sysfix.dylib containermanagerd containermanagerd_system SpringBoard"

if [ "$MODE" = root ]; then
	[ -d "$MOUNT_ROOT/System/Library/xpc" ] || { echo "not a mounted iOS rootfs: $MOUNT_ROOT" >&2; exit 1; }
	install -d -o 0 -g 0 -m 0755 "$MOUNT_ROOT/usr/local/lib"
	install -o 0 -g 0 -m 0755 "$MEDIA_DYLIB" "$MOUNT_ROOT/usr/local/lib/libsweet_media.dylib"
	install -o 0 -g 0 -m 0755 "$SYSFIX_DYLIB" "$MOUNT_ROOT/usr/local/lib/libsweet_sysfix.dylib"

	HAL_DIR="$MOUNT_ROOT/System/Library/Audio/Plug-Ins/HAL"
	install -d -o 0 -g 0 -m 0755 "$HAL_DIR"
	rm -rf "$HAL_DIR/HakiosAudio.driver"
	cp -R "$AUDIO_DRV" "$HAL_DIR/HakiosAudio.driver"
	chown -R 0:0 "$HAL_DIR/HakiosAudio.driver"
	chmod 0755 "$HAL_DIR/HakiosAudio.driver" "$HAL_DIR/HakiosAudio.driver/HakiosAudio"

	install -d -o 0 -g 0 -m 0777 "$MOUNT_ROOT/private/var/local"
	printf '%s\n%s\n' "$INJECT_LINE_1" "$INJECT_LINE_2" > "$MOUNT_ROOT/private/var/local/inject.conf"
	chown 0:0 "$MOUNT_ROOT/private/var/local/inject.conf"
	chmod 0644 "$MOUNT_ROOT/private/var/local/inject.conf"

	if [ -d "$MOUNT_ROOT/Library/Audio/Plug-Ins/HAL.disabled/VirtualAudio.plugin" ] && \
	   [ ! -e "$MOUNT_ROOT/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin" ]; then
		install -d -o 0 -g 0 -m 0755 "$MOUNT_ROOT/Library/Audio/Plug-Ins/HAL"
		mv "$MOUNT_ROOT/Library/Audio/Plug-Ins/HAL.disabled/VirtualAudio.plugin" \
		   "$MOUNT_ROOT/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin"
	fi

	python3 - "$MOUNT_ROOT" <<'PY'
import glob, os, plistlib, sys
root = sys.argv[1]
for conf in glob.glob(os.path.join(root, 'System/Library/Frameworks/MediaToolbox.framework/*/SubServerConfiguration.plist')):
    st = os.stat(conf)
    with open(conf, 'rb') as f:
        data = plistlib.load(f)
    data['StartRecorderServer'] = True
    with open(conf, 'wb') as f:
        plistlib.dump(data, f, fmt=plistlib.FMT_XML)
    os.chown(conf, st.st_uid, st.st_gid)
    os.chmod(conf, st.st_mode & 0o7777)
    print('StartRecorderServer=True:', os.path.relpath(conf, root))
PY
	echo "Installed HackiOS userland bundle into $MOUNT_ROOT"
	exit 0
fi

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/lib" "$STAGE/HAL"
cp "$MEDIA_DYLIB" "$STAGE/lib/libsweet_media.dylib"
cp "$SYSFIX_DYLIB" "$STAGE/lib/libsweet_sysfix.dylib"
cp -R "$AUDIO_DRV" "$STAGE/HAL/HakiosAudio.driver"
printf '%s\n%s\n' "$INJECT_LINE_1" "$INJECT_LINE_2" > "$STAGE/inject.conf"

tools/flame-ssh 'rm -rf /var/local/tmp/hk && mkdir -p /var/local/tmp/hk /usr/local/lib /System/Library/Audio/Plug-Ins/HAL'
COPYFILE_DISABLE=1 tar --no-mac-metadata --no-xattrs --uid 0 --gid 0 --uname root --gname wheel \
	-czf - -C "$STAGE" lib HAL inject.conf | tools/flame-ssh 'toybox zcat | tar -xpf - -C /var/local/tmp/hk'

tools/flame-ssh '
	set -e
	mv /var/local/tmp/hk/lib/libsweet_media.dylib /usr/local/lib/libsweet_media.dylib
	mv /var/local/tmp/hk/lib/libsweet_sysfix.dylib /usr/local/lib/libsweet_sysfix.dylib
	chmod 0755 /usr/local/lib/libsweet_media.dylib /usr/local/lib/libsweet_sysfix.dylib
	rm -rf /System/Library/Audio/Plug-Ins/HAL/HakiosAudio.driver
	mv /var/local/tmp/hk/HAL/HakiosAudio.driver /System/Library/Audio/Plug-Ins/HAL/HakiosAudio.driver
	mv /var/local/tmp/hk/inject.conf /private/var/local/inject.conf
	chmod 0644 /private/var/local/inject.conf
	rm -rf /var/local/tmp/hk
	if [ -d /Library/Audio/Plug-Ins/HAL.disabled/VirtualAudio.plugin ] && [ ! -e /Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin ]; then
		mkdir -p /Library/Audio/Plug-Ins/HAL
		mv /Library/Audio/Plug-Ins/HAL.disabled/VirtualAudio.plugin /Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin
	fi
'

CONF_PATH=$(tools/flame-ssh 'ls /System/Library/Frameworks/MediaToolbox.framework/*/SubServerConfiguration.plist 2>/dev/null | head -n 1' || true)
if [ -n "$CONF_PATH" ]; then
	tools/flame-ssh "cat '$CONF_PATH'" > "$STAGE/SubServerConfiguration.plist"
	python3 - "$STAGE/SubServerConfiguration.plist" <<'PY'
import plistlib, sys
p = sys.argv[1]
with open(p, 'rb') as f:
    d = plistlib.load(f)
d['StartRecorderServer'] = True
with open(p, 'wb') as f:
    plistlib.dump(d, f, fmt=plistlib.FMT_XML)
PY
	cat "$STAGE/SubServerConfiguration.plist" | tools/flame-ssh "cat > '$CONF_PATH' && chmod 0644 '$CONF_PATH'"
fi

tools/flame-ssh "sysctl -w 'kern.exec_inject=$INJECT_LINE_1 ; $INJECT_LINE_2' || true"
[ "$RESTART" = 0 ] || tools/flame-ssh 'psx -k mediaserverd' || true
echo "Live install complete."
